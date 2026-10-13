// ── Router: CoreAudio (macOS) ─────────────────────────────────────────────────
//
// Salida de grupo = Aggregate Device privado y "stacked" (Multi-Output): CoreAudio
// reproduce el mismo audio en todos los subdispositivos. El reloj maestro es el
// miembro elegido por AudioEngine; el resto lleva drift compensation (ADR-07).
// Privado = solo visible para este proceso y desaparece si el proceso termina.

#include "audio_router.h"

#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>

#include <atomic>
#include <memory>
#include <string>
#include <type_traits>
#include <unistd.h>

namespace {

// Libera un objeto CoreFoundation al salir de alcance
struct CFRelease_ {
    void operator()(const void* p) const { if (p) CFRelease(p); }
};
template <typename T>
using CFPtr = std::unique_ptr<std::remove_pointer_t<T>, CFRelease_>;

CFPtr<CFStringRef> makeCFString(const std::string& s) {
    return CFPtr<CFStringRef>(
        CFStringCreateWithCString(kCFAllocatorDefault, s.c_str(), kCFStringEncodingUTF8));
}

CFPtr<CFNumberRef> makeCFInt(int value) {
    return CFPtr<CFNumberRef>(CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &value));
}

AudioObjectPropertyAddress systemAddress(AudioObjectPropertySelector selector) {
    return AudioObjectPropertyAddress {
        selector, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain
    };
}

// UID persistente → AudioDeviceID actual (kAudioDeviceUnknown si no existe)
AudioDeviceID deviceForUID(const std::string& uid) {
    auto uidRef = makeCFString(uid);
    if (!uidRef) return kAudioDeviceUnknown;
    CFStringRef qualifier = uidRef.get();
    AudioDeviceID deviceId = kAudioDeviceUnknown;
    UInt32 size = sizeof(deviceId);
    auto addr = systemAddress(kAudioHardwarePropertyTranslateUIDToDevice);
    OSStatus err = AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr,
                                              static_cast<UInt32>(sizeof(qualifier)), &qualifier,
                                              &size, &deviceId);
    return err == noErr ? deviceId : kAudioDeviceUnknown;
}

// AudioDeviceID → UID persistente ("" si falla)
std::string uidForDevice(AudioDeviceID deviceId) {
    CFStringRef uidRef = nullptr;
    UInt32 size = sizeof(uidRef);
    auto addr = systemAddress(kAudioDevicePropertyDeviceUID);
    if (AudioObjectGetPropertyData(deviceId, &addr, 0, nullptr, &size, &uidRef) != noErr
        || !uidRef) {
        return {};
    }
    CFPtr<CFStringRef> owned(uidRef);
    char buf[256] = {};
    if (!CFStringGetCString(uidRef, buf, sizeof(buf), kCFStringEncodingUTF8)) return {};
    return std::string(buf);
}

class CoreAudioRouter final : public AudioRouter {
public:
    std::string currentDefaultOutput() override {
        AudioDeviceID deviceId = kAudioDeviceUnknown;
        UInt32 size = sizeof(deviceId);
        auto addr = systemAddress(kAudioHardwarePropertyDefaultOutputDevice);
        if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, 0, nullptr,
                                       &size, &deviceId) != noErr
            || deviceId == kAudioDeviceUnknown) {
            return {};
        }
        return uidForDevice(deviceId);
    }

    bool setDefaultOutput(const std::string& uid) override {
        AudioDeviceID deviceId = deviceForUID(uid);
        if (deviceId == kAudioDeviceUnknown) return false;
        auto addr = systemAddress(kAudioHardwarePropertyDefaultOutputDevice);
        return AudioObjectSetPropertyData(kAudioObjectSystemObject, &addr, 0, nullptr,
                                          static_cast<UInt32>(sizeof(deviceId)), &deviceId) == noErr;
    }

    std::string createGroupOutput(const GroupOutputSpec& spec) override {
        if (spec.member_uids.empty()) return {};

        // UID único dentro de este proceso (el agregado es privado)
        const std::string uid = std::string(kGroupOutputUidPrefix)
                              + std::to_string(getpid()) + "."
                              + std::to_string(counter_++);

        auto subdevices = CFPtr<CFMutableArrayRef>(
            CFArrayCreateMutable(kCFAllocatorDefault, 0, &kCFTypeArrayCallBacks));
        auto one  = makeCFInt(1);
        auto zero = makeCFInt(0);
        if (!subdevices || !one || !zero) return {};

        for (const auto& member : spec.member_uids) {
            auto memberUid = makeCFString(member);
            auto sub = CFPtr<CFMutableDictionaryRef>(
                CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
                                          &kCFTypeDictionaryKeyCallBacks,
                                          &kCFTypeDictionaryValueCallBacks));
            if (!memberUid || !sub) return {};
            CFDictionarySetValue(sub.get(), CFSTR(kAudioSubDeviceUIDKey), memberUid.get());
            // Drift compensation en todos menos el reloj maestro
            CFDictionarySetValue(sub.get(), CFSTR(kAudioSubDeviceDriftCompensationKey),
                                 member == spec.clock_uid ? zero.get() : one.get());
            CFArrayAppendValue(subdevices.get(), sub.get());
        }

        auto name  = makeCFString(spec.name);
        auto aggId = makeCFString(uid);
        auto clock = makeCFString(spec.clock_uid.empty() ? spec.member_uids.front()
                                                         : spec.clock_uid);
        auto desc = CFPtr<CFMutableDictionaryRef>(
            CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
                                      &kCFTypeDictionaryKeyCallBacks,
                                      &kCFTypeDictionaryValueCallBacks));
        if (!name || !aggId || !clock || !desc) return {};

        CFDictionarySetValue(desc.get(), CFSTR(kAudioAggregateDeviceNameKey), name.get());
        CFDictionarySetValue(desc.get(), CFSTR(kAudioAggregateDeviceUIDKey), aggId.get());
        CFDictionarySetValue(desc.get(), CFSTR(kAudioAggregateDeviceSubDeviceListKey),
                             subdevices.get());
        CFDictionarySetValue(desc.get(), CFSTR(kAudioAggregateDeviceMainSubDeviceKey),
                             clock.get());
        CFDictionarySetValue(desc.get(), CFSTR(kAudioAggregateDeviceIsPrivateKey), one.get());
        CFDictionarySetValue(desc.get(), CFSTR(kAudioAggregateDeviceIsStackedKey), one.get());

        AudioObjectID created = kAudioObjectUnknown;
        if (AudioHardwareCreateAggregateDevice(desc.get(), &created) != noErr
            || created == kAudioObjectUnknown) {
            return {};
        }
        return uid;
    }

    void destroyGroupOutput(const std::string& outputUid) override {
        AudioDeviceID deviceId = deviceForUID(outputUid);
        if (deviceId != kAudioDeviceUnknown) AudioHardwareDestroyAggregateDevice(deviceId);
    }

private:
    std::atomic<unsigned> counter_{1};
};

} // namespace

std::unique_ptr<AudioRouter> createPlatformAudioRouter() {
    return std::make_unique<CoreAudioRouter>();
}
