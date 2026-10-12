/**
 * test_database_mgr.cpp
 *
 * Verifica los criterios de completitud de F2:
 *  - Schema se crea correctamente
 *  - PRAGMA foreign_keys = ON (integridad referencial)
 *  - Trigger trg_max_devices_per_group rechaza el 5to dispositivo
 *  - Trigger trg_update_group_active actualiza virtual_groups.is_active
 *  - Debounce: no escribe en cada llamada dentro de la ventana
 *  - loadAll retorna el estado correcto tras reiniciar
 */

#include "database_mgr.h"
#include <cassert>
#include <cstdio>
#include <thread>
#include <chrono>

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

// ── Tests ─────────────────────────────────────────────────────────────────────

void test_schema_creation() {
    printf("\n[T1] Schema creation\n");
    DatabaseManager db;
    CHECK(db.open(":memory:"), "open(:memory:) returns true");
    CHECK(db.isOpen(),         "isOpen() is true after open");
    db.close();
    CHECK(!db.isOpen(),        "isOpen() is false after close");
}

void test_device_upsert_and_load() {
    printf("\n[T2] Device upsert and load\n");
    DatabaseManager db;
    db.open(":memory:");

    DeviceRecord d;
    d.device_uid   = "UID-BOCINA-1";
    d.custom_name  = "Bocina Izquierda";
    d.saved_volume = 0.75f;
    d.is_muted     = false;
    d.is_active    = true;

    CHECK(db.upsertDevice(d, /*immediate=*/true), "upsertDevice immediate returns true");

    std::vector<DeviceRecord> devices;
    CHECK(db.loadDevices(devices),       "loadDevices returns true");
    CHECK(devices.size() == 1,           "loadDevices returns 1 device");
    CHECK(devices[0].device_uid   == "UID-BOCINA-1",       "uid matches");
    CHECK(devices[0].custom_name  == "Bocina Izquierda",   "custom_name matches");
    CHECK(devices[0].saved_volume == 0.75f,                "saved_volume matches");
    CHECK(devices[0].is_active    == true,                 "is_active matches");

    // Upsert actualiza el registro existente
    d.saved_volume = 0.5f;
    db.upsertDevice(d, true);
    db.loadDevices(devices);
    CHECK(devices[0].saved_volume == 0.5f, "upsert updates existing record");

    db.close();
}

void test_device_label() {
    printf("\n[T3] Device label update\n");
    DatabaseManager db;
    db.open(":memory:");

    DeviceRecord d;
    d.device_uid   = "UID-AIRPODS";
    d.saved_volume = 1.0f;
    d.is_muted     = false;
    d.is_active    = true;
    db.upsertDevice(d, true);

    CHECK(db.setDeviceLabel("UID-AIRPODS", "Mis AirPods"), "setDeviceLabel returns true");

    std::vector<DeviceRecord> devices;
    db.loadDevices(devices);
    CHECK(devices[0].custom_name == "Mis AirPods", "label persisted correctly");

    // Limpiar label (NULL)
    CHECK(db.setDeviceLabel("UID-AIRPODS", ""), "setDeviceLabel empty clears label");

    db.close();
}

void test_group_crud() {
    printf("\n[T4] Group CRUD\n");
    DatabaseManager db;
    db.open(":memory:");

    GroupRecord g;
    g.name          = "Cuarto";
    g.master_volume = 0.8f;
    g.is_muted      = false;
    g.is_active     = true;

    CHECK(db.insertGroup(g),  "insertGroup returns true");
    CHECK(g.id > 0,           "insertGroup fills id");

    std::vector<GroupRecord> groups;
    CHECK(db.loadGroups(groups),    "loadGroups returns true");
    CHECK(groups.size() == 1,       "loadGroups returns 1 group");
    CHECK(groups[0].name == "Cuarto", "group name matches");

    g.name = "Cuarto Actualizado";
    CHECK(db.updateGroup(g), "updateGroup returns true");
    db.loadGroups(groups);
    CHECK(groups[0].name == "Cuarto Actualizado", "group name updated");

    CHECK(db.deleteGroup(g.id), "deleteGroup returns true");
    db.loadGroups(groups);
    CHECK(groups.empty(), "group deleted");

    db.close();
}

void test_member_limit_trigger() {
    printf("\n[T5] Trigger: max 4 devices per group\n");
    DatabaseManager db;
    db.open(":memory:");

    // Insertar 4 dispositivos
    for (int i = 1; i <= 5; ++i) {
        DeviceRecord d;
        d.device_uid   = "UID-DEV-" + std::to_string(i);
        d.saved_volume = 1.0f;
        d.is_muted     = false;
        d.is_active    = true;
        db.upsertDevice(d, true);
    }

    GroupRecord g;
    g.name = "Grupo Test"; g.master_volume = 1.0f; g.is_muted = false; g.is_active = true;
    db.insertGroup(g);

    // Agregar 4 miembros — deben funcionar
    for (int i = 1; i <= 4; ++i) {
        MemberRecord m;
        m.group_id        = g.id;
        m.device_uid      = "UID-DEV-" + std::to_string(i);
        m.relative_volume = 1.0f;
        m.is_active       = true;
        bool ok = db.insertMember(m);
        CHECK(ok, ("insertMember #" + std::to_string(i) + " succeeds").c_str());
    }

    // El 5to debe ser rechazado por el trigger
    MemberRecord m5;
    m5.group_id        = g.id;
    m5.device_uid      = "UID-DEV-5";
    m5.relative_volume = 1.0f;
    m5.is_active       = true;
    CHECK(!db.insertMember(m5), "5th member rejected by trigger");

    std::vector<MemberRecord> members;
    db.loadMembers(members);
    CHECK(members.size() == 4, "group has exactly 4 members");

    db.close();
}

void test_group_active_trigger() {
    printf("\n[T6] Trigger: group is_active updates on member disconnect\n");
    DatabaseManager db;
    db.open(":memory:");

    // Dispositivo + grupo + membresía
    DeviceRecord d;
    d.device_uid = "UID-SOLO"; d.saved_volume = 1.0f; d.is_muted = false; d.is_active = true;
    db.upsertDevice(d, true);

    GroupRecord g;
    g.name = "Solo Group"; g.master_volume = 1.0f; g.is_muted = false; g.is_active = true;
    db.insertGroup(g);

    MemberRecord m;
    m.group_id = g.id; m.device_uid = "UID-SOLO"; m.relative_volume = 1.0f; m.is_active = true;
    db.insertMember(m);

    // Desconectar el único miembro
    CHECK(db.setMemberActive(g.id, "UID-SOLO", false), "setMemberActive false returns true");

    // El trigger debe haber puesto el grupo en is_active = false
    std::vector<GroupRecord> groups;
    db.loadGroups(groups);
    CHECK(!groups[0].is_active, "group is_active = false when all members inactive");

    // Reconectar
    CHECK(db.setMemberActive(g.id, "UID-SOLO", true), "setMemberActive true returns true");
    db.loadGroups(groups);
    CHECK(groups[0].is_active, "group is_active = true when member reconnects");

    db.close();
}

void test_cascade_delete() {
    printf("\n[T7] ON DELETE CASCADE: deleting group removes members\n");
    DatabaseManager db;
    db.open(":memory:");

    DeviceRecord d;
    d.device_uid = "UID-CASCADE"; d.saved_volume = 1.0f; d.is_muted = false; d.is_active = true;
    db.upsertDevice(d, true);

    GroupRecord g;
    g.name = "Cascade Group"; g.master_volume = 1.0f; g.is_muted = false; g.is_active = true;
    db.insertGroup(g);

    MemberRecord m;
    m.group_id = g.id; m.device_uid = "UID-CASCADE"; m.relative_volume = 1.0f; m.is_active = true;
    db.insertMember(m);

    db.deleteGroup(g.id);

    std::vector<MemberRecord> members;
    db.loadMembers(members);
    CHECK(members.empty(), "CASCADE: members deleted when group is deleted");

    db.close();
}

void test_debounce() {
    printf("\n[T8] Debounce: multiple writes within 300ms produce single DB write\n");
    DatabaseManager db;
    db.open(":memory:");

    // Insertar dispositivo base
    DeviceRecord d;
    d.device_uid = "UID-DEBOUNCE"; d.saved_volume = 1.0f; d.is_muted = false; d.is_active = true;
    db.upsertDevice(d, /*immediate=*/true);

    // Simular arrastre de slider: 5 escrituras rápidas con debounce
    for (int i = 1; i <= 5; ++i) {
        d.saved_volume = static_cast<float>(i) * 0.1f;
        db.upsertDevice(d, /*immediate=*/false);
    }

    // Dentro de la ventana de 300ms, la DB aún tiene el valor original
    std::vector<DeviceRecord> devices;
    db.loadDevices(devices);
    CHECK(devices[0].saved_volume == 1.0f,
          "DB not updated during debounce window (still has original value)");

    // Esperar a que expire el debounce y forzar flush
    std::this_thread::sleep_for(std::chrono::milliseconds(350));
    db.flushPending();

    db.loadDevices(devices);
    CHECK(devices[0].saved_volume == 0.5f,
          "DB updated after debounce flush (last value = 0.5)");

    db.close();
}

void test_debounce_timer() {
    printf("\n[T8b] Debounce: pending write reaches DB without flush or further writes\n");
    DatabaseManager db;
    db.open(":memory:");

    DeviceRecord d;
    d.device_uid = "UID-TIMER"; d.saved_volume = 1.0f; d.is_muted = false; d.is_active = true;
    db.upsertDevice(d, /*immediate=*/true);

    // Una sola escritura con debounce (soltar el slider) y nada más
    d.saved_volume = 0.25f;
    db.upsertDevice(d, /*immediate=*/false);

    std::this_thread::sleep_for(std::chrono::milliseconds(450));

    std::vector<DeviceRecord> devices;
    db.loadDevices(devices);
    CHECK(!devices.empty() && devices[0].saved_volume == 0.25f,
          "DB updated by debounce timer without flushPending()");

    db.close();
}

void test_persist_and_reload() {
    printf("\n[T9] Persist and reload state\n");
    const std::string path = "/tmp/syncbar_test.db";
    std::remove(path.c_str());

    // Primera sesión: escribir datos
    {
        DatabaseManager db;
        db.open(path);

        DeviceRecord d;
        d.device_uid = "UID-PERSIST"; d.custom_name = "Bocina Test";
        d.saved_volume = 0.6f; d.is_muted = true; d.is_active = true;
        db.upsertDevice(d, true);

        GroupRecord g;
        g.name = "Grupo Persist"; g.master_volume = 0.9f; g.is_muted = false; g.is_active = true;
        db.insertGroup(g);

        MemberRecord m;
        m.group_id = g.id; m.device_uid = "UID-PERSIST"; m.relative_volume = 0.8f; m.is_active = true;
        db.insertMember(m);

        db.close();
    }

    // Segunda sesión: recargar y verificar
    {
        DatabaseManager db;
        db.open(path);

        std::vector<DeviceRecord> devices;
        db.loadDevices(devices);
        CHECK(devices.size() == 1,                        "reload: 1 device");
        CHECK(devices[0].custom_name == "Bocina Test",    "reload: custom_name persisted");
        CHECK(devices[0].saved_volume == 0.6f,            "reload: volume persisted");
        CHECK(devices[0].is_muted == true,                "reload: is_muted persisted");

        std::vector<GroupRecord> groups;
        db.loadGroups(groups);
        CHECK(groups.size() == 1,                         "reload: 1 group");
        CHECK(groups[0].name == "Grupo Persist",          "reload: group name persisted");

        std::vector<MemberRecord> members;
        db.loadMembers(members);
        CHECK(members.size() == 1,                        "reload: 1 member");
        CHECK(members[0].relative_volume == 0.8f,         "reload: relative_volume persisted");

        db.close();
    }

    std::remove(path.c_str());
}

// ── Main ──────────────────────────────────────────────────────────────────────

int main() {
    printf("=== DatabaseManager Tests (F2) ===\n");

    test_schema_creation();
    test_device_upsert_and_load();
    test_device_label();
    test_group_crud();
    test_member_limit_trigger();
    test_group_active_trigger();
    test_cascade_delete();
    test_debounce();
    test_debounce_timer();
    test_persist_and_reload();

    printf("\n══════════════════════════════════\n");
    printf("Results: %d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
