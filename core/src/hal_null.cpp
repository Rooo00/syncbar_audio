// ── HAL nulo ──────────────────────────────────────────────────────────────────
//
// Implementación de AudioHal para plataformas sin HAL todavía (Windows hasta la
// Iteración 2, y Linux para compilar y probar el core). No reporta
// dispositivos: los cambios solo se guardan en el estado y en SQLite.

#include "audio_hal.h"

#include <memory>
#include <string>
#include <vector>

namespace {

class NullAudioHal final : public AudioHal {
public:
    std::vector<HalDevice> listOutputDevices() override { return {}; }
    HalResult setVolume(const std::string&, float) override { return HalResult::NoDevice; }
    HalResult setMute(const std::string&, bool) override { return HalResult::NoDevice; }
    void startWatching(ChangeCallback) override {}
    void stopWatching() override {}
};

} // namespace

std::unique_ptr<AudioHal> createPlatformAudioHal() {
    return std::make_unique<NullAudioHal>();
}
