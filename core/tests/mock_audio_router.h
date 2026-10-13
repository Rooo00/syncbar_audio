#pragma once

// MockAudioRouter — router falso para probar el ruteo de grupos sin CoreAudio.
// Igual que MockAudioHal, el test conserva un shared_ptr al estado.

#include "audio_router.h"

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

struct MockRouterState {
    std::mutex                             mutex;
    std::string                            defaultOutput = "UID-SYSTEM";
    std::map<std::string, GroupOutputSpec> outputs;      // creadas y vivas
    std::vector<std::string>               defaultHistory;  // setDefaultOutput recibidos
    bool failCreate     = false;
    bool failSetDefault = false;
    int  createCalls    = 0;
    int  nextId         = 1;

    const GroupOutputSpec* current() {
        auto it = outputs.find(defaultOutput);
        return it != outputs.end() ? &it->second : nullptr;
    }
};

class MockAudioRouter final : public AudioRouter {
public:
    explicit MockAudioRouter(std::shared_ptr<MockRouterState> state)
        : state_(std::move(state)) {}

    std::string currentDefaultOutput() override {
        std::lock_guard<std::mutex> lock(state_->mutex);
        return state_->defaultOutput;
    }

    bool setDefaultOutput(const std::string& uid) override {
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->defaultHistory.push_back(uid);
        if (state_->failSetDefault) return false;
        state_->defaultOutput = uid;
        return true;
    }

    std::string createGroupOutput(const GroupOutputSpec& spec) override {
        std::lock_guard<std::mutex> lock(state_->mutex);
        ++state_->createCalls;
        if (state_->failCreate) return {};
        std::string uid = "com.syncbar.group." + std::to_string(state_->nextId++);
        state_->outputs[uid] = spec;
        return uid;
    }

    void destroyGroupOutput(const std::string& outputUid) override {
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->outputs.erase(outputUid);
    }

private:
    std::shared_ptr<MockRouterState> state_;
};
