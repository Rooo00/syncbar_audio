// ── Router nulo ───────────────────────────────────────────────────────────────
//
// AudioRouter para plataformas sin ruteo todavía (Windows hasta la Iteración 2,
// y Linux para compilar y probar el core). No puede crear salidas de grupo:
// AudioEngine::activateGroupOutput devuelve RoutingFailure.

#include "audio_router.h"

#include <memory>
#include <string>

namespace {

class NullAudioRouter final : public AudioRouter {
public:
    std::string currentDefaultOutput() override { return {}; }
    bool setDefaultOutput(const std::string&) override { return false; }
    std::string createGroupOutput(const GroupOutputSpec&) override { return {}; }
    void destroyGroupOutput(const std::string&) override {}
};

} // namespace

std::unique_ptr<AudioRouter> createPlatformAudioRouter() {
    return std::make_unique<NullAudioRouter>();
}
