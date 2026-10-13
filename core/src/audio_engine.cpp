#include "audio_engine.h"
#include "audio_hal.h"
#include "audio_logic.h"
#include "audio_router.h"

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include <mutex>
#include <algorithm>
#include <atomic>
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
    HalTransport transport  = HalTransport::Unknown;
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

    // ── Ruteo de grupos (ADR-07) ──────────────────────────────────────────────
    // routingMutex serializa las operaciones de ruteo. Orden de locks:
    // routingMutex → stateMutex. Al router se le llama sin stateMutex, porque
    // crear o destruir una salida dispara avisos de hardware en otro hilo.
    std::unique_ptr<AudioRouter> router;
    std::mutex                   routingMutex;
    std::atomic<bool>            routingDirty{false};  // aviso llegado durante una operación
    // Protegidos por routingMutex:
    std::string              outputUid;       // salida creada por el router ("" = ninguna)
    std::vector<std::string> outputMembers;   // miembros con los que se creó
    std::string              previousOutput;  // salida del sistema a restaurar
    // Protegido por stateMutex (lo leen getGroups/getOutputGroupId):
    int32_t outputGroupId = 0;

    static constexpr const char* kPreviousOutputKey = "previous_default_output";

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
        ds.transport          = hw.transport;
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
                it->second.transport          = hw.transport;
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

        syncRouting();
    }

    // ── Ruteo ─────────────────────────────────────────────────────────────────

    // Preferencia para el reloj maestro: cableado antes que Bluetooth (ADR-07)
    static int clockRank(HalTransport t) {
        switch (t) {
            case HalTransport::BuiltIn:   return 0;
            case HalTransport::USB:       return 1;
            case HalTransport::HDMI:      return 2;
            case HalTransport::Other:     return 3;
            case HalTransport::Unknown:   return 3;
            case HalTransport::Bluetooth: return 4;
        }
        return 3;
    }

    /**
     * Arma la salida de un grupo con sus miembros conectados.
     * Requiere stateMutex. Devuelve false si el grupo no existe.
     */
    bool buildOutputSpec(int32_t groupId, GroupOutputSpec& spec) {
        auto* g = findGroup(groupId);
        if (!g) return false;
        spec.name = "SyncBar - " + g->name;
        spec.member_uids.clear();
        spec.clock_uid.clear();
        int bestRank = 1000;
        for (auto& m : g->members) {
            auto* d = findDevice(m.device_uid);
            if (!d || !d->is_active) continue;
            spec.member_uids.push_back(d->uid);
            int rank = clockRank(d->transport);
            if (rank < bestRank) {
                bestRank       = rank;
                spec.clock_uid = d->uid;
            }
        }
        return true;
    }

    /**
     * Crea (o reemplaza) la salida del grupo y la pone como salida del
     * sistema. Si algo falla, el estado anterior queda intacto.
     * Requiere routingMutex; no stateMutex.
     */
    AudioEngine::Result applyOutputLocked(int32_t groupId, const GroupOutputSpec& spec) {
        const bool fresh = outputUid.empty();
        const std::string previous = fresh ? router->currentDefaultOutput() : previousOutput;

        std::string created = router->createGroupOutput(spec);
        if (created.empty()) return AudioEngine::Result::RoutingFailure;
        if (!router->setDefaultOutput(created)) {
            router->destroyGroupOutput(created);
            return AudioEngine::Result::RoutingFailure;
        }
        if (!fresh) router->destroyGroupOutput(outputUid);

        outputUid      = created;
        outputMembers  = spec.member_uids;
        previousOutput = previous;
        if (db && fresh && !previous.empty()) db->setAppState(kPreviousOutputKey, previous);
        {
            std::lock_guard<std::mutex> lock(stateMutex);
            outputGroupId = groupId;
        }
        return AudioEngine::Result::OK;
    }

    /**
     * Restaura la salida previa (si sigue conectada) y destruye la del grupo.
     * Requiere routingMutex; no stateMutex.
     */
    void teardownOutputLocked() {
        if (outputUid.empty()) return;

        if (!previousOutput.empty()) {
            bool restorable;
            {
                std::lock_guard<std::mutex> lock(stateMutex);
                auto* d = findDevice(previousOutput);
                restorable = !d || d->is_active;  // desconocida: se intenta igual
            }
            if (restorable) router->setDefaultOutput(previousOutput);
        }
        router->destroyGroupOutput(outputUid);

        outputUid.clear();
        outputMembers.clear();
        previousOutput.clear();
        if (db) db->deleteAppState(kPreviousOutputKey);
        std::lock_guard<std::mutex> lock(stateMutex);
        outputGroupId = 0;
    }

    /**
     * Ajusta la salida activa a los miembros conectados del grupo: la
     * reconstruye si cambiaron y la desactiva si el grupo ya no existe o se
     * quedó sin miembros. Requiere routingMutex; no stateMutex.
     */
    void syncRoutingLocked() {
        do {
            routingDirty = false;
            if (outputUid.empty()) return;

            int32_t groupId;
            GroupOutputSpec spec;
            bool exists;
            {
                std::lock_guard<std::mutex> lock(stateMutex);
                groupId = outputGroupId;
                exists  = buildOutputSpec(groupId, spec);
            }
            if (!exists || spec.member_uids.empty()) {
                teardownOutputLocked();
            } else if (spec.member_uids != outputMembers &&
                       applyOutputLocked(groupId, spec) != AudioEngine::Result::OK) {
                teardownOutputLocked();
            }
        } while (routingDirty);
    }

    /**
     * Punto de entrada tras cambios de hardware o de membresía (sin locks).
     * Si otro hilo está ruteando, se marca como pendiente: ese hilo vuelve a
     * llamar a syncRouting() después de soltar routingMutex. Así no se bloquea
     * si el aviso llega en su propio hilo, y ningún aviso se pierde.
     */
    void syncRouting() {
        routingDirty = true;
        while (routingDirty) {
            std::unique_lock<std::mutex> lock(routingMutex, std::try_to_lock);
            if (!lock.owns_lock()) return;
            syncRoutingLocked();
        }
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// AudioEngine — constructor / destructor
// ─────────────────────────────────────────────────────────────────────────────

AudioEngine::AudioEngine() : AudioEngine(createPlatformAudioHal()) {}

AudioEngine::AudioEngine(std::unique_ptr<AudioHal> hal,
                         std::unique_ptr<AudioRouter> router) : impl_(new Impl()) {
    impl_->hal    = hal    ? std::move(hal)    : createPlatformAudioHal();
    impl_->router = router ? std::move(router) : createPlatformAudioRouter();
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

    // 3. Si la app se cerró con un grupo activo como salida, restaurar la
    //    salida previa (la salida del grupo desapareció con el proceso).
    std::string pendingRestore;
    if (db.getAppState(Impl::kPreviousOutputKey, pendingRestore)) {
        bool restorable;
        {
            std::lock_guard<std::mutex> lock(impl_->stateMutex);
            auto* d = impl_->findDevice(pendingRestore);
            restorable = !pendingRestore.empty() && (!d || d->is_active);
        }
        if (restorable) impl_->router->setDefaultOutput(pendingRestore);
        db.deleteAppState(Impl::kPreviousOutputKey);
    }

    // 4. Observar altas/bajas de dispositivos
    Impl* impl = impl_;
    impl_->hal->startWatching([impl] { impl->handleHardwareChange(); });
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// shutdown
// ─────────────────────────────────────────────────────────────────────────────

void AudioEngine::shutdown() {
    if (!impl_->initialized) return;

    {
        std::lock_guard<std::mutex> routing(impl_->routingMutex);
        impl_->teardownOutputLocked();
    }
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

    // Si el dispositivo pertenece a grupos, su slider individual redefine su
    // balance: relativo = volumen / maestro. Así el siguiente movimiento del
    // slider maestro respeta el nivel que el usuario acaba de elegir (US-03).
    for (auto& [gid, g] : impl_->groups) {
        for (auto& m : g.members) {
            if (m.device_uid != uid) continue;
            m.relative_volume = AudioLogic::computeRelativeVolume(newVolume, g.master_volume);
            if (impl_->db) {
                impl_->db->updateMemberVolume(gid, uid, m.relative_volume,
                                              /*immediate=*/false);
            }
        }
    }
    return Result::OK;
}

AudioEngine::Result AudioEngine::setMemberVolume(int32_t groupId,
                                                  const std::string& uid,
                                                  float relativeVolume) {
    if (!impl_->initialized) return Result::NotInitialized;
    if (relativeVolume < 0.0f || relativeVolume > 1.0f) return Result::InvalidVolume;

    std::lock_guard<std::mutex> lock(impl_->stateMutex);
    auto* gs = impl_->findGroup(groupId);
    if (!gs) return Result::GroupNotFound;
    auto it = std::find_if(gs->members.begin(), gs->members.end(),
                           [&uid](const MemberState& m) { return m.device_uid == uid; });
    if (it == gs->members.end()) return Result::DeviceNotFound;

    const float relative = AudioLogic::clampVolume(relativeVolume);
    auto* ds = impl_->findDevice(uid);

    // Igual que setGroupVolume: con el grupo muteado (lógico, ADR-03) no se
    // toca el hardware; solo cambia el balance guardado.
    if (ds && !gs->is_muted) {
        const float effective =
            AudioLogic::distributeGroupVolume(gs->master_volume, {relative}).front();
        if (impl_->halSetVolume(*ds, effective) == HalResult::Rejected) {
            return Result::HalFailure;
        }
        ds->volume = effective;
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

    it->relative_volume = relative;
    if (impl_->db) impl_->db->updateMemberVolume(groupId, uid, relative, /*immediate=*/false);
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
        info.is_output     = (gs.id == impl_->outputGroupId);
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

    std::unique_lock<std::mutex> lock(impl_->stateMutex);
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

    lock.unlock();
    impl_->syncRouting();  // reconstruye o desactiva la salida del grupo si aplica
    return Result::OK;
}

AudioEngine::Result AudioEngine::removeDeviceFromGroup(int32_t groupId,
                                                        const std::string& uid) {
    if (!impl_->initialized) return Result::NotInitialized;

    std::unique_lock<std::mutex> lock(impl_->stateMutex);
    auto* gs = impl_->findGroup(groupId);
    if (!gs) return Result::GroupNotFound;

    auto it = std::find_if(gs->members.begin(), gs->members.end(),
                           [&uid](const MemberState& m){ return m.device_uid == uid; });
    if (it == gs->members.end()) return Result::DeviceNotFound;

    gs->members.erase(it);
    impl_->recalcGroupActive(*gs);

    if (impl_->db) impl_->db->removeMember(groupId, uid);
    lock.unlock();
    impl_->syncRouting();  // reconstruye o desactiva la salida del grupo si aplica
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

    std::unique_lock<std::mutex> lock(impl_->stateMutex);
    if (!impl_->findGroup(groupId)) return Result::GroupNotFound;

    impl_->groups.erase(groupId);
    if (impl_->db) impl_->db->deleteGroup(groupId); // CASCADE elimina membresías
    lock.unlock();
    impl_->syncRouting();  // reconstruye o desactiva la salida del grupo si aplica
    return Result::OK;
}

// ─────────────────────────────────────────────────────────────────────────────
// Ruteo: grupo como salida del sistema (ADR-07)
// ─────────────────────────────────────────────────────────────────────────────

AudioEngine::Result AudioEngine::activateGroupOutput(int32_t groupId) {
    if (!impl_->initialized) return Result::NotInitialized;

    Result r;
    {
        std::lock_guard<std::mutex> routing(impl_->routingMutex);
        GroupOutputSpec spec;
        bool exists;
        {
            std::lock_guard<std::mutex> lock(impl_->stateMutex);
            exists = impl_->buildOutputSpec(groupId, spec);
        }
        if (!exists)                       r = Result::GroupNotFound;
        else if (spec.member_uids.empty()) r = Result::NoActiveMembers;
        else                               r = impl_->applyOutputLocked(groupId, spec);
    }
    if (impl_->routingDirty) impl_->syncRouting();  // avisos llegados durante la operación
    return r;
}

AudioEngine::Result AudioEngine::deactivateGroupOutput() {
    if (!impl_->initialized) return Result::NotInitialized;

    {
        std::lock_guard<std::mutex> routing(impl_->routingMutex);
        impl_->teardownOutputLocked();
    }
    if (impl_->routingDirty) impl_->syncRouting();
    return Result::OK;
}

int32_t AudioEngine::getOutputGroupId() const {
    std::lock_guard<std::mutex> lock(impl_->stateMutex);
    return impl_->outputGroupId;
}
