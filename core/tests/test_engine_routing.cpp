/**
 * test_engine_routing.cpp
 *
 * Ruteo de grupos (ADR-07, fase 1) con MockAudioHal + MockAudioRouter:
 *  - Activación: solo miembros conectados, reloj cableado, salida previa guardada
 *  - Errores: sin miembros conectados, fallo del router (estado intacto)
 *  - Desactivación: restaura la salida previa y limpia SQLite
 *  - Hotplug y cambios de membresía: reconstruye o desactiva la salida
 *  - Un solo grupo a la vez; borrar el grupo; shutdown()
 *  - Recuperación tras un cierre abrupto (app_state en SQLite)
 */

#include "audio_engine.h"
#include "database_mgr.h"
#include "mock_audio_hal.h"
#include "mock_audio_router.h"

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

static HalDevice device(const std::string& uid, HalTransport transport) {
    HalDevice d = MockHalState::device(uid);
    d.transport = transport;
    return d;
}

using Members = std::vector<std::string>;

// ── Fixture ──────────────────────────────────────────────────────────────────
//
// Hardware: bocinas USB, AirPods (Bluetooth), bocina BT y la salida del sistema.
// Grupo "Sala" = AirPods + bocinas USB (en ese orden, para probar el reloj).

struct RoutingFixture {
    std::shared_ptr<MockHalState>    hw     = std::make_shared<MockHalState>();
    std::shared_ptr<MockRouterState> router = std::make_shared<MockRouterState>();
    DatabaseManager                  db;
    std::unique_ptr<AudioEngine>     engine;
    int32_t                          sala   = 0;

    explicit RoutingFixture(bool initNow = true) {
        db.open(":memory:");
        hw->devices.push_back(device("UID-SYSTEM",  HalTransport::BuiltIn));
        hw->devices.push_back(device("UID-AIRPODS", HalTransport::Bluetooth));
        hw->devices.push_back(device("UID-USB",     HalTransport::USB));
        hw->devices.push_back(device("UID-BT",      HalTransport::Bluetooth));
        engine = std::make_unique<AudioEngine>(std::make_unique<MockAudioHal>(hw),
                                               std::make_unique<MockAudioRouter>(router));
        if (initNow) init();
    }

    void init() {
        engine->init(db);
        engine->createGroup("Sala", sala);
        engine->addDeviceToGroup(sala, "UID-AIRPODS");
        engine->addDeviceToGroup(sala, "UID-USB");
    }

    bool isOutput(int32_t gid) {
        for (auto& g : engine->getGroups()) if (g.id == gid) return g.is_output;
        return false;
    }

    std::string savedPrevious() {
        std::string v;
        return db.getAppState("previous_default_output", v) ? v : "";
    }
};

// ── Tests ─────────────────────────────────────────────────────────────────────

void test_activate() {
    printf("\n[R1] Activar: miembros conectados, reloj cableado, salida previa guardada\n");
    RoutingFixture f;

    CHECK(f.engine->activateGroupOutput(f.sala) == AudioEngine::Result::OK, "activar → OK");
    auto* out = f.router->current();
    CHECK(out != nullptr, "la salida del sistema es la del grupo");
    CHECK(out && out->member_uids == (Members{"UID-AIRPODS", "UID-USB"}),
          "la salida incluye a los dos miembros");
    CHECK(out && out->clock_uid == "UID-USB", "reloj = USB (cableado) aunque AirPods va primero");
    CHECK(out && out->name == "SyncBar - Sala", "nombre visible de la salida");
    CHECK(f.engine->getOutputGroupId() == f.sala, "getOutputGroupId() = Sala");
    CHECK(f.isOutput(f.sala), "GroupInfo.is_output = true");
    CHECK(f.savedPrevious() == "UID-SYSTEM", "SQLite guarda la salida previa");
}

void test_activate_errors() {
    printf("\n[R2] Errores: grupo inexistente, sin miembros conectados, fallo del router\n");
    RoutingFixture f;

    CHECK(f.engine->activateGroupOutput(999) == AudioEngine::Result::GroupNotFound,
          "grupo inexistente → GroupNotFound");

    int32_t vacio = 0;
    f.engine->createGroup("Vacío", vacio);
    CHECK(f.engine->activateGroupOutput(vacio) == AudioEngine::Result::NoActiveMembers,
          "sin miembros → NoActiveMembers");
    CHECK(f.router->createCalls == 0, "no se llama al router");

    f.router->failCreate = true;
    CHECK(f.engine->activateGroupOutput(f.sala) == AudioEngine::Result::RoutingFailure,
          "el router no puede crear la salida → RoutingFailure");
    f.router->failCreate     = false;
    f.router->failSetDefault = true;
    CHECK(f.engine->activateGroupOutput(f.sala) == AudioEngine::Result::RoutingFailure,
          "no se puede cambiar la salida del sistema → RoutingFailure");
    CHECK(f.router->outputs.empty(), "la salida creada se destruye tras el fallo");
    CHECK(f.router->defaultOutput == "UID-SYSTEM", "la salida del sistema no cambia");
    CHECK(f.engine->getOutputGroupId() == 0, "ningún grupo activo");
    CHECK(f.savedPrevious().empty(), "SQLite no guarda nada");
}

void test_deactivate() {
    printf("\n[R3] Desactivar: restaura la salida previa\n");
    RoutingFixture f;
    f.engine->activateGroupOutput(f.sala);

    CHECK(f.engine->deactivateGroupOutput() == AudioEngine::Result::OK, "desactivar → OK");
    CHECK(f.router->defaultOutput == "UID-SYSTEM", "vuelve la salida previa");
    CHECK(f.router->outputs.empty(), "la salida del grupo se destruye");
    CHECK(f.engine->getOutputGroupId() == 0 && !f.isOutput(f.sala), "ningún grupo activo");
    CHECK(f.savedPrevious().empty(), "SQLite se limpia");
    CHECK(f.engine->deactivateGroupOutput() == AudioEngine::Result::OK,
          "desactivar sin grupo activo → OK");
}

void test_hotplug_rebuilds() {
    printf("\n[R4] Hotplug: reconstruye la salida o la desactiva\n");
    RoutingFixture f;
    f.engine->activateGroupOutput(f.sala);

    f.hw->disconnect("UID-AIRPODS");
    auto* out = f.router->current();
    CHECK(out && out->member_uids == (Members{"UID-USB"}),
          "AirPods desconectados → salida solo con USB");
    CHECK(f.router->outputs.size() == 1, "la salida anterior se destruye");
    CHECK(f.engine->getOutputGroupId() == f.sala, "el grupo sigue activo");
    CHECK(f.savedPrevious() == "UID-SYSTEM", "la salida previa original se conserva");

    f.hw->connect(device("UID-AIRPODS", HalTransport::Bluetooth));
    out = f.router->current();
    CHECK(out && out->member_uids == (Members{"UID-AIRPODS", "UID-USB"}),
          "AirPods reconectados → vuelven a la salida");

    f.hw->disconnect("UID-AIRPODS");
    f.hw->disconnect("UID-USB");
    CHECK(f.engine->getOutputGroupId() == 0, "sin miembros conectados → se desactiva");
    CHECK(f.router->defaultOutput == "UID-SYSTEM", "vuelve la salida previa");
    CHECK(f.router->outputs.empty(), "no queda ninguna salida de grupo");
}

void test_membership_changes() {
    printf("\n[R5] Cambios de membresía con el grupo activo\n");
    RoutingFixture f;
    f.engine->activateGroupOutput(f.sala);

    f.engine->addDeviceToGroup(f.sala, "UID-BT");
    auto* out = f.router->current();
    CHECK(out && out->member_uids == (Members{"UID-AIRPODS", "UID-USB", "UID-BT"}),
          "agregar un miembro lo suma a la salida");

    f.engine->removeDeviceFromGroup(f.sala, "UID-USB");
    out = f.router->current();
    CHECK(out && out->member_uids == (Members{"UID-AIRPODS", "UID-BT"}),
          "quitar un miembro lo saca de la salida");
    CHECK(out && out->clock_uid == "UID-AIRPODS",
          "sin miembros cableados, el reloj es el primero");

    f.engine->deleteGroup(f.sala);
    CHECK(f.engine->getOutputGroupId() == 0, "borrar el grupo activo lo desactiva");
    CHECK(f.router->defaultOutput == "UID-SYSTEM", "vuelve la salida previa");
}

void test_switch_groups() {
    printf("\n[R6] Un solo grupo a la vez\n");
    RoutingFixture f;
    int32_t cuarto = 0;
    f.engine->createGroup("Cuarto", cuarto);
    f.engine->addDeviceToGroup(cuarto, "UID-BT");

    f.engine->activateGroupOutput(f.sala);
    CHECK(f.engine->activateGroupOutput(cuarto) == AudioEngine::Result::OK,
          "activar otro grupo → OK");
    CHECK(f.engine->getOutputGroupId() == cuarto, "ahora el activo es Cuarto");
    CHECK(!f.isOutput(f.sala) && f.isOutput(cuarto), "is_output cambia de grupo");
    CHECK(f.router->outputs.size() == 1, "solo existe una salida de grupo");
    CHECK(f.savedPrevious() == "UID-SYSTEM", "la salida previa sigue siendo la original");

    f.engine->deactivateGroupOutput();
    CHECK(f.router->defaultOutput == "UID-SYSTEM", "desactivar vuelve a la original");
}

void test_previous_output_disconnected() {
    printf("\n[R7] La salida previa se desconectó mientras el grupo estaba activo\n");
    RoutingFixture f;
    f.engine->activateGroupOutput(f.sala);
    f.hw->disconnect("UID-SYSTEM");

    size_t before = f.router->defaultHistory.size();
    f.engine->deactivateGroupOutput();
    bool triedSystem = false;
    for (size_t i = before; i < f.router->defaultHistory.size(); ++i)
        if (f.router->defaultHistory[i] == "UID-SYSTEM") triedSystem = true;
    CHECK(!triedSystem, "no se intenta restaurar una salida desconectada");
    CHECK(f.router->outputs.empty(), "la salida del grupo se destruye igual");
}

void test_shutdown_restores() {
    printf("\n[R8] shutdown() restaura la salida previa\n");
    RoutingFixture f;
    f.engine->activateGroupOutput(f.sala);
    f.engine->shutdown();

    CHECK(f.router->defaultOutput == "UID-SYSTEM", "vuelve la salida previa");
    CHECK(f.router->outputs.empty(), "la salida del grupo se destruye");
    CHECK(f.savedPrevious().empty(), "SQLite se limpia");
}

void test_crash_recovery() {
    printf("\n[R9] Recuperación tras un cierre abrupto\n");
    RoutingFixture f(/*initNow=*/false);
    // Simula una sesión anterior que terminó con un grupo activo
    f.db.setAppState("previous_default_output", "UID-USB");
    f.router->defaultOutput = "UID-SYSTEM";  // macOS eligió otra salida al desaparecer el grupo

    f.init();
    CHECK(f.router->defaultOutput == "UID-USB", "init() restaura la salida previa guardada");
    CHECK(f.savedPrevious().empty(), "la clave se borra de SQLite");
}

void test_crash_recovery_disconnected() {
    printf("\n[R10] Recuperación: la salida guardada ya no está conectada\n");
    RoutingFixture f(/*initNow=*/false);
    DeviceRecord rec;
    rec.device_uid = "UID-GONE"; rec.saved_volume = 1.0f; rec.is_muted = false; rec.is_active = true;
    f.db.upsertDevice(rec, /*immediate=*/true);
    f.db.setAppState("previous_default_output", "UID-GONE");

    f.init();
    CHECK(f.router->defaultOutput == "UID-SYSTEM", "no se cambia la salida del sistema");
    CHECK(f.savedPrevious().empty(), "la clave se borra igual");
}

void test_concurrent_hotplug() {
    printf("\n[R11] Hotplug en otro hilo mientras se activa y desactiva\n");
    RoutingFixture f;

    std::thread plug([&f] {
        for (int i = 0; i < 200; ++i) {
            f.hw->disconnect("UID-AIRPODS");
            f.hw->connect(device("UID-AIRPODS", HalTransport::Bluetooth));
        }
    });
    for (int i = 0; i < 200; ++i) {
        f.engine->activateGroupOutput(f.sala);
        f.engine->deactivateGroupOutput();
    }
    plug.join();

    f.engine->activateGroupOutput(f.sala);
    auto* out = f.router->current();
    CHECK(out && out->member_uids == (Members{"UID-AIRPODS", "UID-USB"}),
          "estado final coherente: ambos miembros en la salida");
    f.engine->deactivateGroupOutput();
    CHECK(f.router->outputs.empty() && f.router->defaultOutput == "UID-SYSTEM",
          "sin salidas huérfanas y con la salida original");
}

// ── Main ──────────────────────────────────────────────────────────────────────

int main() {
    printf("=== AudioEngine — ruteo de grupos (ADR-07) ===\n");

    test_activate();
    test_activate_errors();
    test_deactivate();
    test_hotplug_rebuilds();
    test_membership_changes();
    test_switch_groups();
    test_previous_output_disconnected();
    test_shutdown_restores();
    test_crash_recovery();
    test_crash_recovery_disconnected();
    test_concurrent_hotplug();

    printf("\n══════════════════════════════════\n");
    printf("Results: %d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
