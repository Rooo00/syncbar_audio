#include "audio_engine.h"
#include "audio_hal.h"
#include "audio_logic.h"

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include <mutex>
#include <algorithm>
#include <stdexcept>

// ─────────────────────────────────────────────────────────────────────────────
// Tipos internos de estado en memoria
// ─────────────────────────────────────────────────────────────────────────────

namespace {

struct DeviceState {
    std::string uid;
    std::string native_name;
    std::string custom_name;   // vacío = usar native_name
    float       volume      = 1.0f;
    bool        is_muted    = false;
    bool        is_active   = true;
    bool        has_volume_control = true;  // false = el hardware no expone volumen

};

struct MemberState {
    std::string device_uid;
    float       relative_volume = 1.0f;
    bool        is_active       = true;
};

struct GroupState {
    int32_t                  id            = 0;
    std::string              name;
    float                    master_volume = 1.0f;
    bool                     is_muted      = false;
    bool                     is_active     = true;
    std::vector<MemberState> members;
};

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// AudioEngine::Impl — estado en memoria + coordinación
// ─────────────────────────────────────────────────────────────────────────────

struct AudioEngine::Impl {
    // Estado en memoria (fuente de verdad en runtime)
    std::unordered_map<std::string, DeviceState> devices;
    std::unordered_map<int32_t,     GroupState>  groups;
    mutable std::mutex                           stateMutex;

    DatabaseManager* db          = nullptr;
    bool             initialized = false;

    // Callbacks hacia la UI
    std::function<void(const std::string&)> onConnect;
    std::function<void(const std::string&)> onDisconnect;

    // Acceso al hardware (CoreAudio, WASAPI o un mock en tests)
    std::unique_ptr<AudioHal> hal;

    // ── Helpers ───────────────────────────────────────────────────────────────

    DeviceState* findDevice(const std::string& uid) {
        auto it = devices.find(uid);
        return it != devices.end() ? &it->second : nullptr;
    }

    GroupState* findGroup(int32_t id) {
        auto it = groups.find(id);
        return it != groups.end() ? &it->second : nullptr;
    }

    // Recalcula is_active del grupo según sus miembros
    void recalcGroupActive(GroupState& g) {
        g.is_active = std::any_of(g.members.begin(), g.members.end(),
                                  [](const MemberState& m){ return m.is_active; });
    }

    // Construye el display_name de un dispositivo
    static std::string displayName(const DeviceState& d) {
        return d.custom_name.empty() ? d.native_name : d.custom_name;
    }

    // Aplica volumen al hardware. Un dispositivo desconectado no se toca
    // (NoDevice): su valor solo se guarda.
    HalResult halSetVolume(const DeviceState& d, float volume) {
        if (!d.is_active) return HalResult::NoDevice;
        return hal->setVolume(d.uid, volume);
    }

    HalResult halSetMute(const DeviceState& d, bool muted) {
        if (!d.is_active) return HalResult::NoDevice;
        return hal->setMute(d.uid, muted);
    }

    // Copia al estado lo que reporta el hardware para un dispositivo conectado
    static void applyHardware(DeviceState& ds, const HalDevice& hw) {
        ds.native_name        = hw.name;
        ds.has_volume_control = hw.has_volume_control;
        if (hw.volume_valid) ds.volume = hw.volume;
        ds.is_muted           = hw.muted;
    }

    /**
     * Cruza la lista actual del hardware con el estado en memoria y SQLite:
     * altas, reconexiones y desconexiones. Lo usan init() y el hotplug.
     * Requiere stateMutex tomado. Devuelve los UIDs que cambiaron.
     */
    void reconcile(const std::vector<HalDevice>& hardware,
                   std::vector<std::string>& connected,
                   std::vector<std::string>& disconnected) {
        std::unordered_map<std::string, const HalDevice*> present;
        for (auto& hw : hardware) present[hw.uid] = &hw;

        // Dispositivos nuevos o reconectados
        for (auto& hw : hardware) {
            auto it = devices.find(hw.uid);
            if (it == devices.end()) {
                DeviceState ds;
                ds.uid       = hw.uid;
                ds.is_active = true;
                applyHardware(ds, hw);
                devices[hw.uid] = ds;

                if (db) {
                    DeviceRecord rec;
                    rec.device_uid   = ds.uid;
                    rec.saved_volume = ds.volume;
                    rec.is_muted     = ds.is_muted;
                    rec.is_active    = true;
                    db->upsertDevice(rec, /*immediate=*/true);
                }
                connected.push_back(hw.uid);
            } else if (!it->second.is_active) {
                it->second.is_active = true;
                applyHardware(it->second, hw);
                if (db) db->setDeviceActive(hw.uid, true);
                connected.push_back(hw.uid);
            } else {
                // Ya conectado: solo refrescar capacidades
                it->second.native_name        = hw.name;
                it->second.has_volume_control = hw.has_volume_control;
            }
        }

        // Dispositivos desconectados
        for (auto& [uid, ds] : devices) {
            if (!ds.is_active || present.count(uid)) continue;
            ds.is_active = false;
            if (db) db->setDeviceActive(uid, false);
            disconnected.push_back(uid);
        }

        // Membresías según el estado de cada dispositivo (US-05)
        for (auto& [gid, g] : groups) {
            for (auto& m : g.members) {
                auto dit = devices.find(m.device_uid);
                m.is_active = (dit != devices.end() && dit->second.is_active);
            }
            recalcGroupActive(g);
        }
    }

    /**
     * Callback del HAL ante altas/bajas (hilo del sistema). Los callbacks
     * hacia la UI se invocan después de soltar el lock.
     */
    void handleHardwareChange() {
        auto hardware = hal->listOutputDevices();

        std::vector<std::string> connected;
        std::vector<std::string> disconnected;
        std::function<void(const std::string&)> notifyConnect;
        std::function<void(const std::string&)> notifyDisconnect;
        {
            std::lock_guard<std::mutex> lock(stateMutex);
            if (!initialized) return;
            reconcile(hardware, connected, disconnected);
            notifyConnect    = onConnect;
            notifyDisconnect = onDisconnect;
        }
        if (notifyConnect)    for (auto& uid : connected)    notifyConnect(uid);
        if (notifyDisconnect) for (auto& uid : disconnected) notifyDisconnect(uid);
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// AudioEngine — constructor / destructor
// ─────────────────────────────────────────────────────────────────────────────

AudioEngine::AudioEngine() : AudioEngine(createPlatformAudioHal()) {}

AudioEngine::AudioEngine(std::unique_ptr<AudioHal> hal) : impl_(new Impl()) {
    impl_->hal = hal ? std::move(hal) : createPlatformAudioHal();
}

AudioEngine::~AudioEngine() {
    shutdown();
    delete impl_;
}

bool AudioEngine::isInitialized() const {
    return impl_->initialized;
}

void AudioEngine::registerCallbacks(
    std::function<void(const std::string&)> onConnect,
    std::function<void(const std::string&)> onDisconnect)
{
    std::lock_guard<std::mutex> lock(impl_->stateMutex);
    impl_->onConnect    = std::move(onConnect);
    impl_->onDisconnect = std::move(onDisconnect);
}

// ─────────────────────────────────────────────────────────────────────────────
// init — carga estado desde SQLite + cruza con el HAL + observa altas/bajas
// ─────────────────────────────────────────────────────────────────────────────

bool AudioEngine::init(DatabaseManager& db) {
    if (impl_->initialized) return true;
    impl_->db = &db;

    // 1. Cargar estado persistido desde SQLite
    std::vector<DeviceRecord> savedDevices;
    std::vector<GroupRecord>  savedGroups;
    std::vector<MemberRecord> savedMembers;

    db.loadDevices(savedDevices);
    db.loadGroups(savedGroups);
    db.loadMembers(savedMembers);

    // Poblar mapa de dispositivos desde SQLite
    for (auto& rec : savedDevices) {
        DeviceState ds;
        ds.uid         = rec.device_uid;
        ds.custom_name = rec.custom_name;
        ds.volume      = rec.saved_volume;
        ds.is_muted    = rec.is_muted;
        ds.is_active   = false; // Se actualiza al enumerar hardware
        impl_->devices[rec.device_uid] = ds;
    }

    // Poblar grupos
    for (auto& rec : savedGroups) {
        GroupState gs;
        gs.id            = rec.id;
        gs.name          = rec.name;
        gs.master_volume = rec.master_volume;
        gs.is_muted      = rec.is_muted;
        gs.is_active     = false;
        impl_->groups[rec.id] = gs;
    }

    // Poblar membresías
    for (auto& rec : savedMembers) {
        auto git = impl_->groups.find(rec.group_id);
        if (git == impl_->groups.end()) continue;
        MemberState ms;
        ms.device_uid      = rec.device_uid;
        ms.relative_volume = rec.relative_volume;
        ms.is_active       = false;
        git->second.members.push_back(ms);
    }

    // 2. Cruzar con el hardware actual (sin avisar a la UI: aún no hay vista)
    auto hardware = impl_->hal->listOutputDevices();
    {
        std::lock_guard<std::mutex> lock(impl_->stateMutex);
        std::vector<std::string> connected;
        std::vector<std::string> disconnected;
        impl_->reconcile(hardware, connected, disconnected);

        // Guardados como activos que ya no están conectados (se desconectaron
        // con la app cerrada): reflejarlo también en SQLite.
        for (auto& rec : savedDevices) {
            if (!rec.is_active) continue;
            auto it = impl_->devices.find(rec.device_uid);
            if (it != impl_->devices.end() && !it->second.is_active) {
                db.setDeviceActive(rec.device_uid, false);
            }
        }
        impl_->initialized = true;
    }

    // 3. Observar altas/bajas de dispositivos
    Impl* impl = impl_;
    impl_->hal->startWatching([impl] { impl->handleHardwareChange(); });
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// shutdown
// ─────────────────────────────────────────────────────────────────────────────

void AudioEngine::shutdown() {
    if (!impl_->initialized) return;

    impl_->hal->stopWatching();
    if (impl_->db) impl_->db->flushPending();
    std::lock_guard<std::mutex> lock(impl_->stateMutex);
    impl_->initialized = false;
}

// ─────────────────────────────────────────────────────────────────────────────
// Dispositivos
// ─────────────────────────────────────────────────────────────────────────────

std::vector<DeviceInfo> AudioEngine::getDevices() const {
    std::lock_guard<std::mutex> lock(impl_->stateMutex);
    std::vector<DeviceInfo> result;
    result.reserve(impl_->devices.size());
    for (auto& [uid, ds] : impl_->devices) {
        DeviceInfo info;
        info.uid          = ds.uid;
        info.native_name  = ds.native_name;
        info.display_name = Impl::displayName(ds);
        info.volume       = ds.volume;
        info.is_muted     = ds.is_muted;
        info.is_active    = ds.is_active;
        info.has_volume_control = ds.has_volume_control;
        result.push_back(std::move(info));
    }
    return result;
}

AudioEngine::Result AudioEngine::setDeviceVolume(const std::string& uid, float volume) {
    if (!impl_->initialized) return Result::NotInitialized;
    if (!AudioLogic::clampVolume(volume) && volume > 1.0f) return Result::InvalidVolume;
    if (volume < 0.0f || volume > 1.0f) return Result::InvalidVolume;

    std::lock_guard<std::mutex> lock(impl_->stateMutex);
    auto* ds = impl_->findDevice(uid);
    if (!ds) return Result::DeviceNotFound;

    const float newVolume = AudioLogic::clampVolume(volume);
    if (impl_->halSetVolume(*ds, newVolume) == HalResult::Rejected) {
        // El hardware no aceptó el cambio: no se guarda un valor que no se aplicó
        return Result::HalFailure;
    }
    ds->volume = newVolume;

    // Persistir con debounce
    if (impl_->db) {
        DeviceRecord rec;
        rec.device_uid   = uid;
        rec.custom_name  = ds->custom_name;
        rec.saved_volume = ds->volume;
        rec.is_muted     = ds->is_muted;
        rec.is_active    = ds->is_active;
        impl_->db->upsertDevice(rec, /*immediate=*/false);
    }
    return Result::OK;
}

AudioEngine::Result AudioEngine::setDeviceMute(const std::string& uid, bool muted) {
    if (!impl_->initialized) return Result::NotInitialized;

    std::lock_guard<std::mutex> lock(impl_->stateMutex);
    auto* ds = impl_->findDevice(uid);
    if (!ds) return Result::DeviceNotFound;

    if (impl_->halSetMute(*ds, muted) == HalResult::Rejected) {
        // El hardware no aceptó el cambio (p. ej. HDMI sin mute)
        return Result::HalFailure;
    }
    ds->is_muted = muted;

    if (impl_->db) {
        DeviceRecord rec;
        rec.device_uid   = uid;
        rec.custom_name  = ds->custom_name;
        rec.saved_volume = ds->volume;
        rec.is_muted     = ds->is_muted;
        rec.is_active    = ds->is_active;
        impl_->db->upsertDevice(rec, /*immediate=*/true);
    }
    return Result::OK;
}

AudioEngine::Result AudioEngine::setDeviceLabel(const std::string& uid,
                                                 const std::string& label) {
    if (!impl_->initialized) return Result::NotInitialized;
    if (label.size() > 64) return Result::LabelTooLong;

    std::lock_guard<std::mutex> lock(impl_->stateMutex);
    auto* ds = impl_->findDevice(uid);
    if (!ds) return Result::DeviceNotFound;

    ds->custom_name = label;
    if (impl_->db) impl_->db->setDeviceLabel(uid, label);
    return Result::OK;
}

// ─────────────────────────────────────────────────────────────────────────────
// Grupos virtuales
// ─────────────────────────────────────────────────────────────────────────────

AudioEngine::Result AudioEngine::createGroup(const std::string& name, int32_t& outId) {
    if (!impl_->initialized) return Result::NotInitialized;
    if (name.size() > 64) return Result::LabelTooLong;

    std::lock_guard<std::mutex> lock(impl_->stateMutex);

    GroupRecord rec;
    rec.name          = name;
    rec.master_volume = 1.0f;
    rec.is_muted      = false;
    rec.is_active     = true;

    if (!impl_->db || !impl_->db->insertGroup(rec)) return Result::DbFailure;

    GroupState gs;
    gs.id            = rec.id;
    gs.name          = name;
    gs.master_volume = 1.0f;
    gs.is_muted      = false;
    gs.is_active     = true;
    impl_->groups[rec.id] = std::move(gs);

    outId = rec.id;
    return Result::OK;
}

std::vector<GroupInfo> AudioEngine::getGroups() const {
    std::lock_guard<std::mutex> lock(impl_->stateMutex);
    std::vector<GroupInfo> result;
    result.reserve(impl_->groups.size());
    for (auto& [id, gs] : impl_->groups) {
        GroupInfo info;
        info.id            = gs.id;
        info.name          = gs.name;
        info.master_volume = gs.master_volume;
        info.is_muted      = gs.is_muted;
        info.is_active     = gs.is_active;
        info.member_count  = static_cast<int32_t>(gs.members.size());
        result.push_back(std::move(info));
    }
    return result;
}

std::vector<MemberInfo> AudioEngine::getGroupMembers(int32_t groupId) const {
    std::lock_guard<std::mutex> lock(impl_->stateMutex);
    std::vector<MemberInfo> result;
    auto* gs = impl_->findGroup(groupId);
    if (!gs) return result;
    for (auto& m : gs->members) {
        MemberInfo info;
        info.device_uid      = m.device_uid;
        info.relative_volume = m.relative_volume;
        info.is_active       = m.is_active;
        result.push_back(std::move(info));
    }
    return result;
}

AudioEngine::Result AudioEngine::addDeviceToGroup(int32_t groupId,
                                                   const std::string& uid) {
    if (!impl_->initialized) return Result::NotInitialized;

    std::lock_guard<std::mutex> lock(impl_->stateMutex);
    auto* gs = impl_->findGroup(groupId);
    if (!gs) return Result::GroupNotFound;
    if (!impl_->findDevice(uid)) return Result::DeviceNotFound;

    // Verificar límite de 4
    if (static_cast<int>(gs->members.size()) >= AudioLogic::kMaxGroupMembers)
        return Result::GroupLimitExceeded;

    // Verificar que no sea miembro ya
    for (auto& m : gs->members) {
        if (m.device_uid == uid) return Result::DeviceAlreadyMember;
    }

    // Persistir (el trigger SQLite también valida el límite)
    if (impl_->db) {
        MemberRecord rec;
        rec.group_id        = groupId;
        rec.device_uid      = uid;
        rec.relative_volume = 1.0f;
        rec.is_active       = impl_->devices.at(uid).is_active;
        if (!impl_->db->insertMember(rec)) return Result::GroupLimitExceeded;
    }

    MemberState ms;
    ms.device_uid      = uid;
    ms.relative_volume = 1.0f;
    ms.is_active       = impl_->devices.at(uid).is_active;
    gs->members.push_back(std::move(ms));
    impl_->recalcGroupActive(*gs);

    return Result::OK;
}

AudioEngine::Result AudioEngine::removeDeviceFromGroup(int32_t groupId,
                                                        const std::string& uid) {
    if (!impl_->initialized) return Result::NotInitialized;

    std::lock_guard<std::mutex> lock(impl_->stateMutex);
    auto* gs = impl_->findGroup(groupId);
    if (!gs) return Result::GroupNotFound;

    auto it = std::find_if(gs->members.begin(), gs->members.end(),
                           [&uid](const MemberState& m){ return m.device_uid == uid; });
    if (it == gs->members.end()) return Result::DeviceNotFound;

    gs->members.erase(it);
    impl_->recalcGroupActive(*gs);

    if (impl_->db) impl_->db->removeMember(groupId, uid);
    return Result::OK;
}

AudioEngine::Result AudioEngine::setGroupVolume(int32_t groupId, float masterVolume) {
    if (!impl_->initialized) return Result::NotInitialized;
    if (masterVolume < 0.0f || masterVolume > 1.0f) return Result::InvalidVolume;

    std::lock_guard<std::mutex> lock(impl_->stateMutex);
    auto* gs = impl_->findGroup(groupId);
    if (!gs) return Result::GroupNotFound;

    gs->master_volume = AudioLogic::clampVolume(masterVolume);

    // Calcular volúmenes efectivos para cada miembro activo
    std::vector<float> relatives;
    relatives.reserve(gs->members.size());
    for (auto& m : gs->members) relatives.push_back(m.relative_volume);

    auto effective = AudioLogic::distributeGroupVolume(gs->master_volume, relatives);

    for (size_t i = 0; i < gs->members.size(); ++i) {
        auto& m = gs->members[i];
        if (!m.is_active) continue;

        auto* ds = impl_->findDevice(m.device_uid);
        if (!ds) continue;

        // Solo aplicar al hardware si el grupo no está muteado
        if (!gs->is_muted) {
            ds->volume = effective[i];
            impl_->halSetVolume(*ds, ds->volume);

            if (impl_->db) {
                DeviceRecord rec;
                rec.device_uid   = ds->uid;
                rec.custom_name  = ds->custom_name;
                rec.saved_volume = ds->volume;
                rec.is_muted     = ds->is_muted;
                rec.is_active    = ds->is_active;
                impl_->db->upsertDevice(rec, /*immediate=*/false);
            }
        }
    }

    // Persistir el volumen maestro del grupo
    if (impl_->db) {
        GroupRecord rec;
        rec.id            = gs->id;
        rec.name          = gs->name;
        rec.master_volume = gs->master_volume;
        rec.is_muted      = gs->is_muted;
        rec.is_active     = gs->is_active;
        impl_->db->updateGroup(rec);
    }
    return Result::OK;
}

AudioEngine::Result AudioEngine::setGroupMute(int32_t groupId, bool muted) {
    if (!impl_->initialized) return Result::NotInitialized;

    std::lock_guard<std::mutex> lock(impl_->stateMutex);
    auto* gs = impl_->findGroup(groupId);
    if (!gs) return Result::GroupNotFound;

    // Mute LÓGICO: no toca el hardware (ADR-03)
    gs->is_muted = muted;

    if (impl_->db) {
        GroupRecord rec;
        rec.id            = gs->id;
        rec.name          = gs->name;
        rec.master_volume = gs->master_volume;
        rec.is_muted      = gs->is_muted;
        rec.is_active     = gs->is_active;
        impl_->db->updateGroup(rec);
    }
    return Result::OK;
}

AudioEngine::Result AudioEngine::setGroupName(int32_t groupId,
                                               const std::string& name) {
    if (!impl_->initialized) return Result::NotInitialized;
    if (name.size() > 64) return Result::LabelTooLong;

    std::lock_guard<std::mutex> lock(impl_->stateMutex);
    auto* gs = impl_->findGroup(groupId);
    if (!gs) return Result::GroupNotFound;

    gs->name = name;

    if (impl_->db) {
        GroupRecord rec;
        rec.id            = gs->id;
        rec.name          = gs->name;
        rec.master_volume = gs->master_volume;
        rec.is_muted      = gs->is_muted;
        rec.is_active     = gs->is_active;
        impl_->db->updateGroup(rec);
    }
    return Result::OK;
}

AudioEngine::Result AudioEngine::deleteGroup(int32_t groupId) {
    if (!impl_->initialized) return Result::NotInitialized;

    std::lock_guard<std::mutex> lock(impl_->stateMutex);
    if (!impl_->findGroup(groupId)) return Result::GroupNotFound;

    impl_->groups.erase(groupId);
    if (impl_->db) impl_->db->deleteGroup(groupId); // CASCADE elimina membresías
    return Result::OK;
}
