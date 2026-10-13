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

#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
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

// ── Volumen relativo por miembro (US-03 / US-13) ─────────────────────────────

static float memberRelative(AudioEngine& e, int32_t gid, const std::string& uid) {
    for (auto& m : e.getGroupMembers(gid)) if (m.device_uid == uid) return m.relative_volume;
    return -1.0f;
}

static float deviceVolume(AudioEngine& e, const std::string& uid) {
    auto devices = e.getDevices();
    auto* d = findDevice(devices, uid);
    return d ? d->volume : -1.0f;
}

static bool near(float a, float b) { return a > b - 1e-4f && a < b + 1e-4f; }

// Grupo "Sala" con UID-A y UID-B y el maestro al 80 %
struct MemberFixture : HalFixture {
    int32_t gid = 0;
    MemberFixture() {
        engine->init(db);
        engine->createGroup("Sala", gid);
        engine->addDeviceToGroup(gid, "UID-A");
        engine->addDeviceToGroup(gid, "UID-B");
        engine->setGroupVolume(gid, 0.8f);
    }
};

void test_member_volume_applies() {
    printf("\n[H8] setMemberVolume aplica maestro × relativo\n");
    MemberFixture f;
    CHECK(f.engine->setMemberVolume(f.gid, "UID-A", 0.5f) == AudioEngine::Result::OK,
          "setMemberVolume → OK");
    CHECK(near(memberRelative(*f.engine, f.gid, "UID-A"), 0.5f), "relativo de UID-A = 0.5");
    CHECK(near(deviceVolume(*f.engine, "UID-A"), 0.4f), "UID-A al 40 % (0.8 × 0.5)");
    CHECK(near(f.hw->lastVolume, 0.4f), "el HAL recibió 0.4");
    CHECK(near(deviceVolume(*f.engine, "UID-B"), 0.8f), "UID-B no cambia");
}

void test_member_proportion_kept() {
    printf("\n[H9] El maestro respeta la proporción (criterio de US-03)\n");
    MemberFixture f;
    f.engine->setMemberVolume(f.gid, "UID-A", 0.5f);
    f.engine->setMemberVolume(f.gid, "UID-B", 1.0f);
    f.engine->setGroupVolume(f.gid, 0.6f);
    CHECK(near(deviceVolume(*f.engine, "UID-A"), 0.3f), "miembro al 50 % → 30 % con maestro al 60 %");
    CHECK(near(deviceVolume(*f.engine, "UID-B"), 0.6f), "miembro al 100 % → 60 % con maestro al 60 %");
}

void test_member_volume_persisted() {
    printf("\n[H10] El relativo se guarda con debounce\n");
    MemberFixture f;
    f.engine->setMemberVolume(f.gid, "UID-A", 0.25f);
    std::this_thread::sleep_for(std::chrono::milliseconds(450));
    std::vector<MemberRecord> members;
    f.db.loadMembers(members);
    float saved = -1.0f;
    for (auto& m : members) if (m.device_uid == "UID-A") saved = m.relative_volume;
    CHECK(near(saved, 0.25f), "SQLite tiene 0.25 sin flushPending()");
}

void test_member_volume_edge_cases() {
    printf("\n[H11] Rechazo del hardware, grupo muteado, miembro desconectado, errores\n");
    MemberFixture f;

    f.hw->rejectVolume.insert("UID-A");
    CHECK(f.engine->setMemberVolume(f.gid, "UID-A", 0.5f) == AudioEngine::Result::HalFailure,
          "hardware rechaza → HalFailure");
    CHECK(near(memberRelative(*f.engine, f.gid, "UID-A"), 1.0f), "el relativo no cambia");
    CHECK(near(deviceVolume(*f.engine, "UID-A"), 0.8f), "el volumen no cambia");
    f.hw->rejectVolume.clear();

    f.engine->setGroupMute(f.gid, true);
    int calls = f.hw->setVolumeCalls;
    CHECK(f.engine->setMemberVolume(f.gid, "UID-B", 0.5f) == AudioEngine::Result::OK,
          "grupo muteado → OK");
    CHECK(f.hw->setVolumeCalls == calls, "grupo muteado: no se toca el hardware (ADR-03)");
    CHECK(near(memberRelative(*f.engine, f.gid, "UID-B"), 0.5f), "el relativo sí se guarda");
    f.engine->setGroupMute(f.gid, false);

    f.hw->disconnect("UID-B");
    calls = f.hw->setVolumeCalls;
    CHECK(f.engine->setMemberVolume(f.gid, "UID-B", 0.2f) == AudioEngine::Result::OK,
          "miembro desconectado → OK");
    CHECK(f.hw->setVolumeCalls == calls, "desconectado: no se llama al HAL");
    CHECK(near(memberRelative(*f.engine, f.gid, "UID-B"), 0.2f), "el relativo se guarda");

    CHECK(f.engine->setMemberVolume(f.gid, "UID-A", 1.5f) == AudioEngine::Result::InvalidVolume,
          "1.5 → InvalidVolume");
    CHECK(f.engine->setMemberVolume(999, "UID-A", 0.5f) == AudioEngine::Result::GroupNotFound,
          "grupo inexistente → GroupNotFound");
    CHECK(f.engine->setMemberVolume(f.gid, "UID-X", 0.5f) == AudioEngine::Result::DeviceNotFound,
          "no es miembro → DeviceNotFound");
}

void test_device_slider_updates_relative() {
    printf("\n[H12] El slider individual recalcula el relativo (decisión 1a)\n");
    MemberFixture f;
    f.engine->setDeviceVolume("UID-A", 0.2f);
    CHECK(near(memberRelative(*f.engine, f.gid, "UID-A"), 0.25f),
          "UID-A al 20 % con maestro al 80 % → relativo 0.25");
    f.engine->setGroupVolume(f.gid, 0.4f);
    CHECK(near(deviceVolume(*f.engine, "UID-A"), 0.1f), "maestro al 40 % → UID-A al 10 %");
    CHECK(near(deviceVolume(*f.engine, "UID-B"), 0.4f), "UID-B sigue al maestro");
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
    test_member_volume_applies();
    test_member_proportion_kept();
    test_member_volume_persisted();
    test_member_volume_edge_cases();
    test_device_slider_updates_relative();

    printf("\n══════════════════════════════════\n");
    printf("Results: %d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
