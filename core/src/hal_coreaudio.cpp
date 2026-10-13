// ── HAL: CoreAudio (macOS) ────────────────────────────────────────────────────
//
// Implementación de AudioHal sobre CoreAudio. Traduce UIDs persistentes a
// AudioDeviceID y observa altas/bajas con un listener sobre
// kAudioHardwarePropertyDevices.

#include "audio_hal.h"
#include "audio_logic.h"

#include <CoreAudio/CoreAudio.h>
#include <AudioToolbox/AudioServices.h>

#include <algorithm>
#include <cstdlib>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

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

class CoreAudioHal final : public AudioHal {
public:
    ~CoreAudioHal() override { stopWatching(); }

    std::vector<HalDevice> listOutputDevices() override {
        std::vector<HalDevice> result;
        std::unordered_map<std::string, AudioDeviceID> ids;

        for (AudioDeviceID devId : CoreAudioHAL::enumerateOutputDevices()) {
            HalDevice d;
            d.uid = CoreAudioHAL::getUID(devId);
            if (d.uid.empty()) continue;
            d.name               = CoreAudioHAL::getName(devId);
            d.has_volume_control = CoreAudioHAL::hasVolumeControl(devId);
            d.volume_valid       = CoreAudioHAL::getVolume(devId, d.volume);
            d.muted              = CoreAudioHAL::getMute(devId);
            ids[d.uid] = devId;
            result.push_back(std::move(d));
        }

        std::lock_guard<std::mutex> lock(mutex_);
        ids_ = std::move(ids);
        return result;
    }

    HalResult setVolume(const std::string& uid, float volume) override {
        AudioDeviceID devId = lookup(uid);
        if (devId == kAudioDeviceUnknown) return HalResult::NoDevice;
        return CoreAudioHAL::setVolume(devId, volume) ? HalResult::Applied
                                                      : HalResult::Rejected;
    }

    HalResult setMute(const std::string& uid, bool muted) override {
        AudioDeviceID devId = lookup(uid);
        if (devId == kAudioDeviceUnknown) return HalResult::NoDevice;
        return CoreAudioHAL::setMute(devId, muted) ? HalResult::Applied
                                                   : HalResult::Rejected;
    }

    void startWatching(ChangeCallback onDevicesChanged) override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (watching_) return;
            callback_ = std::move(onDevicesChanged);
            watching_ = true;
        }
        AudioObjectPropertyAddress addr = devicesAddress();
        AudioObjectAddPropertyListener(kAudioObjectSystemObject, &addr,
                                       &CoreAudioHal::listenerProc, this);
    }

    void stopWatching() override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!watching_) return;
            watching_ = false;
            callback_ = nullptr;
        }
        AudioObjectPropertyAddress addr = devicesAddress();
        AudioObjectRemovePropertyListener(kAudioObjectSystemObject, &addr,
                                          &CoreAudioHal::listenerProc, this);
    }

private:
    static AudioObjectPropertyAddress devicesAddress() {
        return AudioObjectPropertyAddress {
            kAudioHardwarePropertyDevices,
            kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain
        };
    }

    // Invocado por CoreAudio en su propio hilo
    static OSStatus listenerProc(AudioObjectID, UInt32,
                                 const AudioObjectPropertyAddress*,
                                 void* clientData) {
        auto* self = static_cast<CoreAudioHal*>(clientData);
        ChangeCallback cb;
        {
            std::lock_guard<std::mutex> lock(self->mutex_);
            cb = self->callback_;
        }
        if (cb) cb();
        return noErr;
    }

    AudioDeviceID lookup(const std::string& uid) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = ids_.find(uid);
        return it != ids_.end() ? it->second : kAudioDeviceUnknown;
    }

    std::mutex                                     mutex_;
    std::unordered_map<std::string, AudioDeviceID> ids_;      // uid → id actual
    ChangeCallback                                 callback_;
    bool                                           watching_ = false;
};

} // namespace

std::unique_ptr<AudioHal> createPlatformAudioHal() {
    return std::make_unique<CoreAudioHal>();
}
