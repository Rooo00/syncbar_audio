#pragma once

// MockAudioHal — HAL falso para probar AudioEngine sin hardware.
//
// El test conserva un std::shared_ptr<MockHalState> para manipular los
// dispositivos después de entregar el HAL al engine:
//
//   auto state = std::make_shared<MockHalState>();
//   state->devices.push_back(MockHalState::device("UID-A"));
//   AudioEngine engine(std::make_unique<MockAudioHal>(state));
//   ...
//   state->disconnect("UID-A");   // simula el hotplug y avisa al engine

#include "audio_hal.h"

#include <algorithm>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

struct MockHalState {
    std::mutex             mutex;
    std::vector<HalDevice> devices;              // conectados
    std::set<std::string>  rejectVolume;         // UIDs que rechazan setVolume
    std::set<std::string>  rejectMute;           // UIDs que rechazan setMute
    AudioHal::ChangeCallback onChanged;
    int setVolumeCalls = 0;
    int setMuteCalls   = 0;
    float lastVolume   = -1.0f;

    static HalDevice device(const std::string& uid, float volume = 0.5f,
                            bool hasVolumeControl = true) {
        HalDevice d;
        d.uid                = uid;
        d.name               = "Mock " + uid;
        d.volume             = volume;
        d.volume_valid       = true;
        d.has_volume_control = hasVolumeControl;
        return d;
    }

    // Simula la conexión de un dispositivo y avisa como lo haría el sistema
    void connect(const HalDevice& d) {
        AudioHal::ChangeCallback cb;
        {
            std::lock_guard<std::mutex> lock(mutex);
            devices.push_back(d);
            cb = onChanged;
        }
        if (cb) cb();
    }

    // Simula la desconexión de un dispositivo
    void disconnect(const std::string& uid) {
        AudioHal::ChangeCallback cb;
        {
            std::lock_guard<std::mutex> lock(mutex);
            devices.erase(std::remove_if(devices.begin(), devices.end(),
                              [&](const HalDevice& d) { return d.uid == uid; }),
                          devices.end());
            cb = onChanged;
        }
        if (cb) cb();
    }

    // Aviso de cambio sin cambios reales (el sistema puede emitirlos)
    void notifyWithoutChanges() {
        AudioHal::ChangeCallback cb;
        {
            std::lock_guard<std::mutex> lock(mutex);
            cb = onChanged;
        }
        if (cb) cb();
    }

    bool isConnected(const std::string& uid) {
        return std::any_of(devices.begin(), devices.end(),
                           [&](const HalDevice& d) { return d.uid == uid; });
    }
};

class MockAudioHal final : public AudioHal {
public:
    explicit MockAudioHal(std::shared_ptr<MockHalState> state)
        : state_(std::move(state)) {}

    std::vector<HalDevice> listOutputDevices() override {
        std::lock_guard<std::mutex> lock(state_->mutex);
        return state_->devices;
    }

    HalResult setVolume(const std::string& uid, float volume) override {
        std::lock_guard<std::mutex> lock(state_->mutex);
        ++state_->setVolumeCalls;
        if (!state_->isConnected(uid)) return HalResult::NoDevice;
        if (state_->rejectVolume.count(uid)) return HalResult::Rejected;
        state_->lastVolume = volume;
        return HalResult::Applied;
    }

    HalResult setMute(const std::string& uid, bool) override {
        std::lock_guard<std::mutex> lock(state_->mutex);
        ++state_->setMuteCalls;
        if (!state_->isConnected(uid)) return HalResult::NoDevice;
        if (state_->rejectMute.count(uid)) return HalResult::Rejected;
        return HalResult::Applied;
    }

    void startWatching(ChangeCallback onDevicesChanged) override {
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->onChanged = std::move(onDevicesChanged);
    }

    void stopWatching() override {
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->onChanged = nullptr;
    }

private:
    std::shared_ptr<MockHalState> state_;
};
