/**
 * test_engine_hal.cpp
 *
 * Prueba AudioEngine contra un HAL simulado (MockAudioHal), cubriendo los
 * caminos que antes solo existían con hardware real:
 *  - init() cruza el hardware con lo guardado en SQLite
 *  - El hardware rechaza volumen o mute → HalFailure y el estado no cambia
 *  - Dispositivo desconectado: el volumen solo se guarda, no se llama al HAL
 *  - has_volume_control se propaga desde el HAL
 *  - Hotplug: un aviso por cambio real, membresías de grupo (US-05),
 *    callbacks fuera del lock y fin de la observación en shutdown()
 */

#include "audio_engine.h"
#include "database_mgr.h"
#include "mock_audio_hal.h"

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

// ── Utilidad ──────────────────────────────────────────────────────────────────

static int passed = 0;
static int failed = 0;

#define CHECK(cond, msg)                                          \
    do {                                                          \
        if (cond) {                                               \
            printf("  [PASS] %s\n", msg); ++passed;              \
        } else {                                                  \
            printf("  [FAIL] %s  (line %d)\n", msg, __LINE__);   \
            ++failed;                                             \
        }                                                         \
    } while(0)

static const DeviceInfo* findDevice(const std::vector<DeviceInfo>& devices,
                                    const std::string& uid) {
    for (auto& d : devices) if (d.uid == uid) return &d;
    return nullptr;
}

static const DeviceRecord* findRecord(const std::vector<DeviceRecord>& records,
                                      const std::string& uid) {
    for (auto& r : records) if (r.device_uid == uid) return &r;
    return nullptr;
}

// ── Fixture: hardware simulado con UID-A y UID-B conectados ──────────────────

struct HalFixture {
    std::shared_ptr<MockHalState> hw = std::make_shared<MockHalState>();
    DatabaseManager               db;
    std::unique_ptr<AudioEngine>  engine;

    HalFixture() {
        db.open(":memory:");
        hw->devices.push_back(MockHalState::device("UID-A", 0.3f));
        hw->devices.push_back(MockHalState::device("UID-B", 0.6f));
        engine = std::make_unique<AudioEngine>(std::make_unique<MockAudioHal>(hw));
    }
};

// ── Tests ─────────────────────────────────────────────────────────────────────

void test_init_merges_hardware_and_db() {
    printf("\n[H1] init() cruza el hardware con SQLite\n");
    HalFixture f;

    // En SQLite: UID-A con nombre propio y UID-C que no está conectado
    DeviceRecord a;
    a.device_uid = "UID-A"; a.custom_name = "Sala"; a.saved_volume = 0.9f;
    a.is_muted = false; a.is_active = false;
    f.db.upsertDevice(a, /*immediate=*/true);
    DeviceRecord c;
    c.device_uid = "UID-C"; c.saved_volume = 0.4f; c.is_muted = false; c.is_active = true;
    f.db.upsertDevice(c, /*immediate=*/true);

    CHECK(f.engine->init(f.db), "init() con MockAudioHal");

    auto devices = f.engine->getDevices();
    auto* da = findDevice(devices, "UID-A");
    auto* db_ = findDevice(devices, "UID-B");
    auto* dc = findDevice(devices, "UID-C");

    CHECK(da && da->is_active, "UID-A (guardado + conectado) queda activo");
    CHECK(da && da->volume == 0.3f, "UID-A toma el volumen real del hardware");
    CHECK(da && da->display_name == "Sala", "UID-A conserva su nombre propio");
    CHECK(db_ && db_->is_active, "UID-B (nuevo) aparece activo");
    CHECK(dc && !dc->is_active, "UID-C (solo en SQLite) queda inactivo");
    CHECK(dc && dc->volume == 0.4f, "UID-C conserva su volumen guardado");

    std::vector<DeviceRecord> records;
    f.db.loadDevices(records);
    CHECK(findRecord(records, "UID-B") != nullptr, "UID-B se persiste en SQLite");
    auto* rc = findRecord(records, "UID-C");
    CHECK(rc && !rc->is_active, "UID-C se marca inactivo en SQLite");
}

void test_volume_applied_and_rejected() {
    printf("\n[H2] Volumen: aplicado, rechazado y dispositivo desconectado\n");
    HalFixture f;
    DeviceRecord c;
    c.device_uid = "UID-C"; c.saved_volume = 0.4f; c.is_muted = false; c.is_active = false;
    f.db.upsertDevice(c, /*immediate=*/true);
    f.engine->init(f.db);

    CHECK(f.engine->setDeviceVolume("UID-A", 0.7f) == AudioEngine::Result::OK,
          "volumen aceptado → OK");
    CHECK(f.hw->lastVolume == 0.7f, "el HAL recibió 0.7");
    CHECK(findDevice(f.engine->getDevices(), "UID-A")->volume == 0.7f,
          "el estado refleja 0.7");

    f.hw->rejectVolume.insert("UID-A");
    CHECK(f.engine->setDeviceVolume("UID-A", 0.2f) == AudioEngine::Result::HalFailure,
          "volumen rechazado → HalFailure");
    CHECK(findDevice(f.engine->getDevices(), "UID-A")->volume == 0.7f,
          "el estado no cambia tras un rechazo");

    int callsBefore = f.hw->setVolumeCalls;
    CHECK(f.engine->setDeviceVolume("UID-C", 0.8f) == AudioEngine::Result::OK,
          "dispositivo desconectado → OK (solo se guarda)");
    CHECK(f.hw->setVolumeCalls == callsBefore, "no se llama al HAL si está desconectado");
    CHECK(findDevice(f.engine->getDevices(), "UID-C")->volume == 0.8f,
          "el volumen guardado se actualiza");
}

void test_mute_applied_and_rejected() {
    printf("\n[H3] Mute: aplicado y rechazado\n");
    HalFixture f;
    f.engine->init(f.db);

    CHECK(f.engine->setDeviceMute("UID-A", true) == AudioEngine::Result::OK,
          "mute aceptado → OK");
    CHECK(findDevice(f.engine->getDevices(), "UID-A")->is_muted, "UID-A queda muteado");

    f.hw->rejectMute.insert("UID-B");
    CHECK(f.engine->setDeviceMute("UID-B", true) == AudioEngine::Result::HalFailure,
          "mute rechazado → HalFailure");
    CHECK(!findDevice(f.engine->getDevices(), "UID-B")->is_muted,
          "UID-B no queda muteado tras el rechazo");

    std::vector<DeviceRecord> records;
    f.db.loadDevices(records);
    auto* rb = findRecord(records, "UID-B");
    CHECK(rb && !rb->is_muted, "el rechazo no se persiste en SQLite");
}

void test_volume_control_flag() {
    printf("\n[H4] has_volume_control llega desde el HAL\n");
    auto hw = std::make_shared<MockHalState>();
    hw->devices.push_back(MockHalState::device("UID-HDMI", 1.0f, /*hasVolumeControl=*/false));
    DatabaseManager db;
    db.open(":memory:");
    AudioEngine engine(std::make_unique<MockAudioHal>(hw));
    engine.init(db);

    auto devices = engine.getDevices();
    auto* d = findDevice(devices, "UID-HDMI");
    CHECK(d && !d->has_volume_control, "UID-HDMI sin control de volumen");
}

void test_hotplug_events_and_groups() {
    printf("\n[H5] Hotplug: un aviso por cambio real y membresías de grupo\n");
    HalFixture f;
    f.engine->init(f.db);

    std::vector<std::string> connected;
    std::vector<std::string> disconnected;
    f.engine->registerCallbacks(
        [&](const std::string& uid) { connected.push_back(uid); },
        [&](const std::string& uid) { disconnected.push_back(uid); });

    int32_t gid = 0;
    f.engine->createGroup("Sala", gid);
    f.engine->addDeviceToGroup(gid, "UID-A");

    f.hw->notifyWithoutChanges();
    CHECK(connected.empty() && disconnected.empty(),
          "aviso sin cambios → ningún callback");

    f.hw->connect(MockHalState::device("UID-NEW", 0.5f));
    CHECK(connected.size() == 1 && connected[0] == "UID-NEW",
          "conexión nueva → un solo onConnect");

    f.hw->disconnect("UID-A");
    CHECK(disconnected.size() == 1 && disconnected[0] == "UID-A",
          "desconexión → un solo onDisconnect");
    CHECK(!f.engine->getGroups()[0].is_active, "el grupo queda inactivo (US-05)");
    CHECK(!f.engine->getGroupMembers(gid)[0].is_active, "el miembro queda inactivo");

    std::vector<DeviceRecord> records;
    f.db.loadDevices(records);
    auto* ra = findRecord(records, "UID-A");
    CHECK(ra && !ra->is_active, "SQLite: UID-A inactivo");

    f.hw->connect(MockHalState::device("UID-A", 0.3f));
    CHECK(connected.size() == 2 && connected[1] == "UID-A",
          "reconexión → onConnect de UID-A");
    CHECK(f.engine->getGroups()[0].is_active, "el grupo se reactiva");
    CHECK(f.engine->getGroupMembers(gid)[0].is_active, "el miembro se reactiva");

    f.db.loadDevices(records);
    ra = findRecord(records, "UID-A");
    CHECK(ra && ra->is_active, "SQLite: UID-A activo de nuevo");
}

void test_callbacks_run_outside_lock() {
    printf("\n[H6] Los callbacks pueden llamar al engine (sin bloqueo)\n");
    HalFixture f;
    f.engine->init(f.db);

    size_t seen = 0;
    AudioEngine* engine = f.engine.get();
    f.engine->registerCallbacks(
        [&, engine](const std::string&) { seen = engine->getDevices().size(); },
        nullptr);

    // Si el callback corriera con stateMutex tomado, esto se bloquearía
    // (CTest corta el test por timeout).
    f.hw->connect(MockHalState::device("UID-NEW"));
    CHECK(seen == 3, "el callback leyó los 3 dispositivos");
}

void test_shutdown_stops_watching() {
    printf("\n[H7] shutdown() deja de observar el hardware\n");
    HalFixture f;
    f.engine->init(f.db);
    CHECK(f.hw->onChanged != nullptr, "init() registra la observación");

    f.engine->shutdown();
    CHECK(f.hw->onChanged == nullptr, "shutdown() la quita");
}

// ── Main ──────────────────────────────────────────────────────────────────────

int main() {
    printf("=== AudioEngine + MockAudioHal Tests ===\n");

    test_init_merges_hardware_and_db();
    test_volume_applied_and_rejected();
    test_mute_applied_and_rejected();
    test_volume_control_flag();
    test_hotplug_events_and_groups();
    test_callbacks_run_outside_lock();
    test_shutdown_stops_watching();

    printf("\n══════════════════════════════════\n");
    printf("Results: %d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
