#include "audio_engine.h"
#include "audio_logic.h"

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>
#include <mutex>
#include <algorithm>
#include <stdexcept>

// ── HAL: CoreAudio (macOS) ────────────────────────────────────────────────────
#ifdef __APPLE__
#include <CoreAudio/CoreAudio.h>
#include <AudioToolbox/AudioServices.h>
#endif

// ── HAL: WASAPI (Windows) — stub para compilación cruzada ────────────────────
#ifdef _WIN32
// Implementación en iteración 2
#endif

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

#ifdef __APPLE__
    AudioDeviceID core_audio_id = kAudioDeviceUnknown;
#endif
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
// CoreAudio HAL
// ─────────────────────────────────────────────────────────────────────────────

#ifdef __APPLE__

namespace CoreAudioHAL {

// Obtiene el UID persistente de un AudioDeviceID como std::string
static std::string getUID(AudioDeviceID deviceId) {
    CFStringRef uidRef = nullptr;
    AudioObjectPropertyAddress addr {
        kAudioDevicePropertyDeviceUID,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    UInt32 size = sizeof(uidRef);
    OSStatus err = AudioObjectGetPropertyData(deviceId, &addr, 0, nullptr,
                                              &size, &uidRef);
    if (err != noErr || !uidRef) return {};

    char buf[256] = {};
    CFStringGetCString(uidRef, buf, sizeof(buf), kCFStringEncodingUTF8);
    CFRelease(uidRef);
    return std::string(buf);
}

// Obtiene el nombre nativo de un AudioDeviceID
static std::string getName(AudioDeviceID deviceId) {
    CFStringRef nameRef = nullptr;
    AudioObjectPropertyAddress addr {
        kAudioDevicePropertyDeviceNameCFString,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    UInt32 size = sizeof(nameRef);
    OSStatus err = AudioObjectGetPropertyData(deviceId, &addr, 0, nullptr,
                                              &size, &nameRef);
    if (err != noErr || !nameRef) return "Unknown Device";

    char buf[256] = {};
    CFStringGetCString(nameRef, buf, sizeof(buf), kCFStringEncodingUTF8);
    CFRelease(nameRef);
    return std::string(buf);
}

// Verifica si un dispositivo tiene canales de salida (output)
static bool isOutputDevice(AudioDeviceID deviceId) {
    AudioObjectPropertyAddress addr {
        kAudioDevicePropertyStreamConfiguration,
        kAudioDevicePropertyScopeOutput,
        kAudioObjectPropertyElementMain
    };
    UInt32 size = 0;
    OSStatus err = AudioObjectGetPropertyDataSize(deviceId, &addr, 0, nullptr, &size);
    if (err != noErr || size == 0) return false;

    auto* bufList = reinterpret_cast<AudioBufferList*>(malloc(size));
    err = AudioObjectGetPropertyData(deviceId, &addr, 0, nullptr, &size, bufList);
    bool hasOutput = (err == noErr && bufList->mNumberBuffers > 0);
    free(bufList);
    return hasOutput;
}

// Enumera todos los dispositivos de salida activos
static std::vector<AudioDeviceID> enumerateOutputDevices() {
    AudioObjectPropertyAddress addr {
        kAudioHardwarePropertyDevices,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    UInt32 size = 0;
    OSStatus err = AudioObjectGetPropertyDataSize(kAudioObjectSystemObject,
                                                  &addr, 0, nullptr, &size);
    if (err != noErr || size == 0) return {};

    std::vector<AudioDeviceID> devices(size / sizeof(AudioDeviceID));
    err = AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, 0, nullptr,
                                     &size, devices.data());
    if (err != noErr) return {};

    // Filtrar solo dispositivos de salida
    devices.erase(
        std::remove_if(devices.begin(), devices.end(),
                       [](AudioDeviceID d) { return !isOutputDevice(d); }),
        devices.end()
    );
    return devices;
}

// ¿El dispositivo expone esta propiedad de salida como modificable?
static bool isOutputPropertySettable(AudioDeviceID deviceId,
                                     AudioObjectPropertySelector selector,
                                     AudioObjectPropertyElement element) {
    AudioObjectPropertyAddress addr {
        selector, kAudioDevicePropertyScopeOutput, element
    };
    if (!AudioObjectHasProperty(deviceId, &addr)) return false;
    Boolean settable = false;
    return AudioObjectIsPropertySettable(deviceId, &addr, &settable) == noErr
        && settable;
}

// Elementos (main + canales 1 y 2) con kAudioDevicePropertyVolumeScalar modificable.
// Respaldo para dispositivos sin VirtualMainVolume.
static std::vector<AudioObjectPropertyElement> scalarVolumeElements(AudioDeviceID deviceId) {
    std::vector<AudioObjectPropertyElement> elements;
    const AudioObjectPropertyElement candidates[] = { kAudioObjectPropertyElementMain, 1, 2 };
    for (auto e : candidates) {
        if (isOutputPropertySettable(deviceId, kAudioDevicePropertyVolumeScalar, e)) {
            elements.push_back(e);
        }
    }
    return elements;
}

// ¿Se puede controlar el volumen de este dispositivo? (HDMI/DisplayPort suelen no permitirlo)
static bool hasVolumeControl(AudioDeviceID deviceId) {
    return isOutputPropertySettable(deviceId,
               kAudioHardwareServiceDeviceProperty_VirtualMainVolume,
               kAudioObjectPropertyElementMain)
        || !scalarVolumeElements(deviceId).empty();
}

// Lee el volumen [0.0, 1.0]. Devuelve false si el dispositivo no lo expone.
static bool getVolume(AudioDeviceID deviceId, float& outVolume) {
    Float32 vol = 0.0f;
    UInt32 size = sizeof(vol);
    AudioObjectPropertyAddress addr {
        kAudioHardwareServiceDeviceProperty_VirtualMainVolume,
        kAudioDevicePropertyScopeOutput,
        kAudioObjectPropertyElementMain
    };
    if (AudioObjectGetPropertyData(deviceId, &addr, 0, nullptr, &size, &vol) == noErr) {
        outVolume = AudioLogic::clampVolume(static_cast<float>(vol));
        return true;
    }
    addr.mSelector = kAudioDevicePropertyVolumeScalar;
    for (auto e : scalarVolumeElements(deviceId)) {
        addr.mElement = e;
        size = sizeof(vol);
        if (AudioObjectGetPropertyData(deviceId, &addr, 0, nullptr, &size, &vol) == noErr) {
            outVolume = AudioLogic::clampVolume(static_cast<float>(vol));
            return true;
        }
    }
    return false;
}

// Aplica el volumen. Intenta VirtualMainVolume y, si falla, VolumeScalar por canal.
// Devuelve false si el hardware rechazó el cambio en todas las vías.
static bool setVolume(AudioDeviceID deviceId, float linearVolume) {
    Float32 vol = AudioLogic::clampVolume(linearVolume);
    const UInt32 kVolSize = sizeof(vol);
    AudioObjectPropertyAddress addr {
        kAudioHardwareServiceDeviceProperty_VirtualMainVolume,
        kAudioDevicePropertyScopeOutput,
        kAudioObjectPropertyElementMain
    };
    if (isOutputPropertySettable(deviceId, addr.mSelector, addr.mElement) &&
        AudioObjectSetPropertyData(deviceId, &addr, 0, nullptr, kVolSize, &vol) == noErr) {
        return true;
    }
    bool applied = false;
    addr.mSelector = kAudioDevicePropertyVolumeScalar;
    for (auto e : scalarVolumeElements(deviceId)) {
        addr.mElement = e;
        if (AudioObjectSetPropertyData(deviceId, &addr, 0, nullptr, kVolSize, &vol) == noErr) {
            applied = true;
        }
    }
    return applied;
}

// Obtiene el estado de mute de un dispositivo
static bool getMute(AudioDeviceID deviceId) {
    UInt32 muted = 0;
    AudioObjectPropertyAddress addr {
        kAudioDevicePropertyMute,
        kAudioDevicePropertyScopeOutput,
        kAudioObjectPropertyElementMain
    };
    UInt32 size = sizeof(muted);
    AudioObjectGetPropertyData(deviceId, &addr, 0, nullptr, &size, &muted);
    return muted != 0;
}

// Establece el estado de mute de un dispositivo
static bool setMute(AudioDeviceID deviceId, bool muted) {
    UInt32 val = muted ? 1 : 0;
    AudioObjectPropertyAddress addr {
        kAudioDevicePropertyMute,
        kAudioDevicePropertyScopeOutput,
        kAudioObjectPropertyElementMain
    };
    UInt32 size = sizeof(val);
    OSStatus err = AudioObjectSetPropertyData(deviceId, &addr, 0, nullptr,
                                              size, &val);
    return err == noErr;
}

} // namespace CoreAudioHAL

#endif // __APPLE__

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

#ifdef __APPLE__
    // Listener de hotplug registrado en CoreAudio
    AudioObjectPropertyListenerProc hotplugProc = nullptr;
    // Puntero a this para el callback estático
    AudioEngine::Impl* selfPtr = nullptr;
#endif

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

    // Resultado de aplicar un cambio al hardware
    enum class HalStatus { Applied, NoHardware, Rejected };

    // Aplica volumen al HAL para un dispositivo específico.
    // NoHardware = desconectado o plataforma sin HAL (el valor solo se guarda).
    HalStatus halSetVolume(const DeviceState& d, float volume) {
#ifdef __APPLE__
        if (d.core_audio_id == kAudioDeviceUnknown) return HalStatus::NoHardware;
        return CoreAudioHAL::setVolume(d.core_audio_id, volume)
            ? HalStatus::Applied : HalStatus::Rejected;
#else
        (void)d; (void)volume;
        return HalStatus::NoHardware; // WASAPI: Iteración 2
#endif
    }

    // Aplica mute al HAL para un dispositivo específico
    bool halSetMute(const DeviceState& d, bool muted) {
#ifdef __APPLE__
        if (d.core_audio_id == kAudioDeviceUnknown) return false;
        return CoreAudioHAL::setMute(d.core_audio_id, muted);
#elif defined(_WIN32)
        (void)d; (void)muted;
        return false; // Iteración 2
#else
        return false;
#endif
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// Hotplug listener (CoreAudio)
// ─────────────────────────────────────────────────────────────────────────────

#ifdef __APPLE__

static OSStatus hotplugListenerProc(
    AudioObjectID                       /*inObjectID*/,
    UInt32                              /*inNumberAddresses*/,
    const AudioObjectPropertyAddress*   /*inAddresses*/,
    void*                               inClientData)
{
    // Impl es una struct definida en audio_engine.cpp — acceso directo por puntero
    auto* impl = static_cast<AudioEngine::Impl*>(inClientData);
    if (!impl) return noErr;

    // Obtener la lista actual de dispositivos de salida
    auto currentIds = CoreAudioHAL::enumerateOutputDevices();

    // Construir mapa uid → AudioDeviceID de los dispositivos actuales
    std::unordered_map<std::string, AudioDeviceID> currentMap;
    for (AudioDeviceID devId : currentIds) {
        std::string uid = CoreAudioHAL::getUID(devId);
        if (!uid.empty()) currentMap[uid] = devId;
    }

    // Cambios detectados; los callbacks se invocan después de soltar el lock
    // para que la UI nunca corra código con stateMutex tomado.
    std::vector<std::string> connected;
    std::vector<std::string> disconnected;
    std::function<void(const std::string&)> onConnect;
    std::function<void(const std::string&)> onDisconnect;

    {
        std::lock_guard<std::mutex> lock(impl->stateMutex);

        // Dispositivos nuevos o reconectados
        for (auto& [uid, devId] : currentMap) {
            auto it = impl->devices.find(uid);
            if (it == impl->devices.end()) {
                // Dispositivo completamente nuevo
                DeviceState ds;
                ds.uid            = uid;
                ds.native_name    = CoreAudioHAL::getName(devId);
                ds.has_volume_control = CoreAudioHAL::hasVolumeControl(devId);
                CoreAudioHAL::getVolume(devId, ds.volume);
                ds.is_muted       = CoreAudioHAL::getMute(devId);
                ds.is_active      = true;
                ds.core_audio_id  = devId;
                impl->devices[uid] = ds;

                // Persistir el nuevo dispositivo
                if (impl->db) {
                    DeviceRecord rec;
                    rec.device_uid   = uid;
                    rec.saved_volume = ds.volume;
                    rec.is_muted     = ds.is_muted;
                    rec.is_active    = true;
                    impl->db->upsertDevice(rec, /*immediate=*/true);
                }
                connected.push_back(uid);
            } else {
                // El AudioDeviceID puede cambiar entre conexiones
                it->second.core_audio_id      = devId;
                it->second.has_volume_control = CoreAudioHAL::hasVolumeControl(devId);
                if (it->second.is_active) continue;  // ya estaba conectado: sin evento

                // Dispositivo conocido que se reconecta
                it->second.is_active = true;
                for (auto& [gid, g] : impl->groups) {
                    for (auto& m : g.members) {
                        if (m.device_uid == uid) m.is_active = true;
                    }
                    impl->recalcGroupActive(g);
                }
                if (impl->db) impl->db->setDeviceActive(uid, true);
                connected.push_back(uid);
            }
        }

        // Dispositivos desconectados
        for (auto& [uid, ds] : impl->devices) {
            if (!ds.is_active || currentMap.count(uid)) continue;
            ds.is_active     = false;
            ds.core_audio_id = kAudioDeviceUnknown;
            for (auto& [gid, g] : impl->groups) {
                for (auto& m : g.members) {
                    if (m.device_uid == uid) m.is_active = false;
                }
                impl->recalcGroupActive(g);
            }
            if (impl->db) impl->db->setDeviceActive(uid, false);
            disconnected.push_back(uid);
        }

        onConnect    = impl->onConnect;
        onDisconnect = impl->onDisconnect;
    }

    if (onConnect)    for (auto& uid : connected)    onConnect(uid);
    if (onDisconnect) for (auto& uid : disconnected) onDisconnect(uid);

    return noErr;
}

#endif // __APPLE__

// ─────────────────────────────────────────────────────────────────────────────
// AudioEngine — constructor / destructor
// ─────────────────────────────────────────────────────────────────────────────

AudioEngine::AudioEngine() : impl_(new Impl()) {}

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
// init — carga estado desde SQLite + enumera hardware + registra hotplug
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

    // 2. Enumerar hardware actual y cruzar con estado persistido
#ifdef __APPLE__
    auto hwDevices = CoreAudioHAL::enumerateOutputDevices();
    for (AudioDeviceID devId : hwDevices) {
        std::string uid  = CoreAudioHAL::getUID(devId);
        std::string name = CoreAudioHAL::getName(devId);
        if (uid.empty()) continue;

        auto it = impl_->devices.find(uid);
        if (it != impl_->devices.end()) {
            // Dispositivo conocido: actualizar estado de hardware
            it->second.native_name   = name;
            it->second.is_active     = true;
            it->second.core_audio_id = devId;
            it->second.has_volume_control = CoreAudioHAL::hasVolumeControl(devId);
            CoreAudioHAL::getVolume(devId, it->second.volume);
            it->second.is_muted      = CoreAudioHAL::getMute(devId);
        } else {
            // Dispositivo nuevo (no estaba en SQLite)
            DeviceState ds;
            ds.uid           = uid;
            ds.native_name   = name;
            ds.has_volume_control = CoreAudioHAL::hasVolumeControl(devId);
            CoreAudioHAL::getVolume(devId, ds.volume);
            ds.is_muted      = CoreAudioHAL::getMute(devId);
            ds.is_active     = true;
            ds.core_audio_id = devId;
            impl_->devices[uid] = ds;

            // Persistir el nuevo dispositivo
            DeviceRecord rec;
            rec.device_uid   = uid;
            rec.saved_volume = ds.volume;
            rec.is_muted     = ds.is_muted;
            rec.is_active    = true;
            db.upsertDevice(rec, /*immediate=*/true);
        }
    }

    // Actualizar is_active de membresías según hardware
    for (auto& [gid, g] : impl_->groups) {
        for (auto& m : g.members) {
            auto dit = impl_->devices.find(m.device_uid);
            m.is_active = (dit != impl_->devices.end() && dit->second.is_active);
        }
        impl_->recalcGroupActive(g);
    }

    // 3. Registrar listener de hotplug
    impl_->selfPtr = impl_;
    AudioObjectPropertyAddress addr {
        kAudioHardwarePropertyDevices,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    AudioObjectAddPropertyListener(kAudioObjectSystemObject, &addr,
                                   hotplugListenerProc, impl_);
#endif

    impl_->initialized = true;
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// shutdown
// ─────────────────────────────────────────────────────────────────────────────

void AudioEngine::shutdown() {
    if (!impl_->initialized) return;

#ifdef __APPLE__
    AudioObjectPropertyAddress addr {
        kAudioHardwarePropertyDevices,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain
    };
    AudioObjectRemovePropertyListener(kAudioObjectSystemObject, &addr,
                                      hotplugListenerProc, impl_);
#endif

    if (impl_->db) impl_->db->flushPending();
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
    if (impl_->halSetVolume(*ds, newVolume) == Impl::HalStatus::Rejected) {
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

    ds->is_muted = muted;
    impl_->halSetMute(*ds, muted);

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
