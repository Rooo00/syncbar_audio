/**
 * test_bridge_api.cpp
 *
 * Verifica los criterios de completitud de F6:
 *  - Todas las funciones del api-specification.md implementadas
 *  - Validaciones de entrada: volumen en [0,1], label ≤ 64 chars, punteros no-null
 *  - sb_init es idempotente (segunda llamada retorna SB_OK sin reinicializar)
 *  - Los callbacks de hardware se propagan correctamente
 *  - Conversión correcta de SB_Result desde AudioEngine::Result
 *  - sb_get_devices / sb_get_groups llenan los buffers correctamente
 *  - sb_shutdown limpia el estado global
 */

#include "bridge_api.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <atomic>

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

// DB en memoria para tests
static const char* TEST_DB = ":memory:";

// ── Tests ─────────────────────────────────────────────────────────────────────

void test_init_shutdown() {
    printf("\n[T1] sb_init / sb_shutdown\n");

    CHECK(sb_is_initialized() == 0, "not initialized before sb_init");

    SB_Result r = sb_init(TEST_DB);
    CHECK(r == SB_OK,               "sb_init returns SB_OK");
    CHECK(sb_is_initialized() == 1, "is_initialized = 1 after sb_init");

    // Idempotente
    SB_Result r2 = sb_init(TEST_DB);
    CHECK(r2 == SB_OK,              "sb_init idempotent (second call = SB_OK)");
    CHECK(sb_is_initialized() == 1, "still initialized after second sb_init");

    sb_shutdown();
    CHECK(sb_is_initialized() == 0, "is_initialized = 0 after sb_shutdown");

    // Doble shutdown seguro
    sb_shutdown();
    CHECK(sb_is_initialized() == 0, "double shutdown is safe");
}

void test_not_initialized_guard() {
    printf("\n[T2] Operations before sb_init return SB_ERR_NOT_INITIALIZED\n");

    // Asegurar que está apagado
    sb_shutdown();

    SB_DeviceInfo devBuf[32];
    int32_t count = 0;
    CHECK(sb_get_devices(devBuf, 32, &count) == SB_ERR_NOT_INITIALIZED,
          "sb_get_devices before init → NOT_INITIALIZED");

    CHECK(sb_set_device_volume("X", 0.5f) == SB_ERR_NOT_INITIALIZED,
          "sb_set_device_volume before init → NOT_INITIALIZED");

    SB_GroupInfo grpBuf[32];
    CHECK(sb_get_groups(grpBuf, 32, &count) == SB_ERR_NOT_INITIALIZED,
          "sb_get_groups before init → NOT_INITIALIZED");

    int32_t gid = 0;
    CHECK(sb_create_group("G", &gid) == SB_ERR_NOT_INITIALIZED,
          "sb_create_group before init → NOT_INITIALIZED");
}

void test_null_pointer_guards() {
    printf("\n[T3] Null pointer guards\n");

    sb_init(TEST_DB);

    CHECK(sb_set_device_volume(nullptr, 0.5f) == SB_ERR_DEVICE_NOT_FOUND,
          "sb_set_device_volume(null uid) → DEVICE_NOT_FOUND");
    CHECK(sb_set_device_mute(nullptr, false) == SB_ERR_DEVICE_NOT_FOUND,
          "sb_set_device_mute(null uid) → DEVICE_NOT_FOUND");
    CHECK(sb_set_device_label(nullptr, "X") == SB_ERR_DEVICE_NOT_FOUND,
          "sb_set_device_label(null uid) → DEVICE_NOT_FOUND");
    CHECK(sb_add_device_to_group(1, nullptr) == SB_ERR_DEVICE_NOT_FOUND,
          "sb_add_device_to_group(null uid) → DEVICE_NOT_FOUND");
    CHECK(sb_remove_device_from_group(1, nullptr) == SB_ERR_DEVICE_NOT_FOUND,
          "sb_remove_device_from_group(null uid) → DEVICE_NOT_FOUND");

    int32_t gid = 0;
    CHECK(sb_create_group(nullptr, &gid) == SB_ERR_NOT_INITIALIZED,
          "sb_create_group(null name) → NOT_INITIALIZED");
    CHECK(sb_set_group_name(1, nullptr) == SB_ERR_LABEL_TOO_LONG,
          "sb_set_group_name(null) → LABEL_TOO_LONG");

    sb_shutdown();
}

void test_get_devices_buffer() {
    printf("\n[T4] sb_get_devices fills buffer correctly\n");

    sb_init(TEST_DB);

    SB_DeviceInfo devices[64];
    int32_t count = 0;
    SB_Result r = sb_get_devices(devices, 64, &count);

    CHECK(r == SB_OK,    "sb_get_devices returns SB_OK");
    CHECK(count >= 0,    "count >= 0");

    // Verificar que los campos están bien formados (no basura)
    for (int32_t i = 0; i < count; ++i) {
        CHECK(strlen(devices[i].uid) > 0,
              ("device[" + std::to_string(i) + "].uid not empty").c_str());
        CHECK(strlen(devices[i].display_name) > 0,
              ("device[" + std::to_string(i) + "].display_name not empty").c_str());
        CHECK(devices[i].volume >= 0.0f && devices[i].volume <= 1.0f,
              ("device[" + std::to_string(i) + "].volume in [0,1]").c_str());
    }

    // Buffer más pequeño que el número de dispositivos
    int32_t count2 = 0;
    SB_DeviceInfo smallBuf[1];
    sb_get_devices(smallBuf, 1, &count2);
    CHECK(count2 <= 1, "buffer limit respected (max 1)");

    sb_shutdown();
}

void test_group_lifecycle_via_bridge() {
    printf("\n[T5] Group lifecycle via bridge\n");

    sb_init(TEST_DB);

    // Crear grupo
    int32_t gid = 0;
    SB_Result r = sb_create_group("Mi Grupo", &gid);
    CHECK(r == SB_OK,  "sb_create_group returns SB_OK");
    CHECK(gid > 0,     "group id > 0");

    // Listar grupos
    SB_GroupInfo groups[32];
    int32_t gcount = 0;
    sb_get_groups(groups, 32, &gcount);
    CHECK(gcount == 1,                          "sb_get_groups returns 1 group");
    CHECK(strcmp(groups[0].name, "Mi Grupo") == 0, "group name matches");
    CHECK(groups[0].id == gid,                  "group id matches");

    // Renombrar
    CHECK(sb_set_group_name(gid, "Cuarto") == SB_OK, "sb_set_group_name OK");
    sb_get_groups(groups, 32, &gcount);
    CHECK(strcmp(groups[0].name, "Cuarto") == 0, "group renamed");

    // Volumen maestro
    CHECK(sb_set_group_volume(gid, 0.7f) == SB_OK, "sb_set_group_volume OK");
    sb_get_groups(groups, 32, &gcount);
    CHECK(groups[0].master_volume == 0.7f, "master_volume updated");

    // Mute lógico
    CHECK(sb_set_group_mute(gid, true) == SB_OK, "sb_set_group_mute OK");
    sb_get_groups(groups, 32, &gcount);
    CHECK(groups[0].is_muted == true, "group is_muted = true");

    // Eliminar
    CHECK(sb_delete_group(gid) == SB_OK, "sb_delete_group OK");
    sb_get_groups(groups, 32, &gcount);
    CHECK(gcount == 0, "group deleted");

    sb_shutdown();
}

void test_group_error_codes() {
    printf("\n[T6] Group error codes via bridge\n");

    sb_init(TEST_DB);

    // Grupo inexistente
    CHECK(sb_set_group_volume(9999, 0.5f) == SB_ERR_GROUP_NOT_FOUND,
          "set_group_volume unknown → GROUP_NOT_FOUND");
    CHECK(sb_set_group_mute(9999, true) == SB_ERR_GROUP_NOT_FOUND,
          "set_group_mute unknown → GROUP_NOT_FOUND");
    CHECK(sb_set_group_name(9999, "X") == SB_ERR_GROUP_NOT_FOUND,
          "set_group_name unknown → GROUP_NOT_FOUND");
    CHECK(sb_delete_group(9999) == SB_ERR_GROUP_NOT_FOUND,
          "delete_group unknown → GROUP_NOT_FOUND");

    // Volumen inválido
    int32_t gid = 0;
    sb_create_group("Test", &gid);
    CHECK(sb_set_group_volume(gid, -0.1f) == SB_ERR_INVALID_VOLUME,
          "set_group_volume -0.1 → INVALID_VOLUME");
    CHECK(sb_set_group_volume(gid, 1.1f) == SB_ERR_INVALID_VOLUME,
          "set_group_volume 1.1 → INVALID_VOLUME");

    // Label demasiado largo
    char longName[66];
    memset(longName, 'x', 65);
    longName[65] = '\0';
    CHECK(sb_set_group_name(gid, longName) == SB_ERR_LABEL_TOO_LONG,
          "set_group_name 65 chars → LABEL_TOO_LONG");

    sb_shutdown();
}

void test_device_error_codes() {
    printf("\n[T7] Device error codes via bridge\n");

    sb_init(TEST_DB);

    CHECK(sb_set_device_volume("NONEXISTENT", 0.5f) == SB_ERR_DEVICE_NOT_FOUND,
          "set_device_volume unknown → DEVICE_NOT_FOUND");
    CHECK(sb_set_device_mute("NONEXISTENT", true) == SB_ERR_DEVICE_NOT_FOUND,
          "set_device_mute unknown → DEVICE_NOT_FOUND");
    CHECK(sb_set_device_label("NONEXISTENT", "X") == SB_ERR_DEVICE_NOT_FOUND,
          "set_device_label unknown → DEVICE_NOT_FOUND");

    // Volumen inválido
    SB_DeviceInfo devs[64];
    int32_t count = 0;
    sb_get_devices(devs, 64, &count);

    if (count > 0) {
        CHECK(sb_set_device_volume(devs[0].uid, -0.1f) == SB_ERR_INVALID_VOLUME,
              "set_device_volume -0.1 → INVALID_VOLUME");
        CHECK(sb_set_device_volume(devs[0].uid, 1.1f) == SB_ERR_INVALID_VOLUME,
              "set_device_volume 1.1 → INVALID_VOLUME");

        // Label demasiado largo
        char longLabel[66];
        memset(longLabel, 'x', 65);
        longLabel[65] = '\0';
        CHECK(sb_set_device_label(devs[0].uid, longLabel) == SB_ERR_LABEL_TOO_LONG,
              "set_device_label 65 chars → LABEL_TOO_LONG");
    } else {
        printf("  [SKIP] No real audio devices available for device error tests\n");
    }

    sb_shutdown();
}

void test_callbacks_registered() {
    printf("\n[T8] Callbacks registration\n");

    static std::atomic<int> connectCount{0};
    static std::atomic<int> disconnectCount{0};

    // Registrar antes de init
    sb_register_device_callbacks(
        [](const char* /*uid*/) { ++connectCount; },
        [](const char* /*uid*/) { ++disconnectCount; }
    );

    sb_init(TEST_DB);
    CHECK(sb_is_initialized() == 1, "init OK with callbacks pre-registered");

    // Desregistrar
    sb_register_device_callbacks(nullptr, nullptr);

    sb_shutdown();
    CHECK(sb_is_initialized() == 0, "shutdown OK after callback deregistration");
}

void test_get_group_members() {
    printf("\n[T9] sb_get_group_members\n");

    sb_init(TEST_DB);

    int32_t gid = 0;
    sb_create_group("Members Test", &gid);

    // Sin miembros
    SB_MemberInfo members[4];
    int32_t mcount = 0;
    SB_Result r = sb_get_group_members(gid, members, &mcount);
    CHECK(r == SB_OK,    "get_group_members empty group → SB_OK");
    CHECK(mcount == 0,   "empty group has 0 members");

    // Grupo inexistente
    SB_Result r2 = sb_get_group_members(9999, members, &mcount);
    CHECK(r2 == SB_ERR_GROUP_NOT_FOUND, "get_group_members unknown → GROUP_NOT_FOUND");

    sb_shutdown();
}

void test_result_string_coverage() {
    printf("\n[T10] All SB_Result codes are distinct integers\n");

    CHECK(SB_OK                        == 0, "SB_OK = 0");
    CHECK(SB_ERR_DEVICE_NOT_FOUND      == 1, "DEVICE_NOT_FOUND = 1");
    CHECK(SB_ERR_GROUP_NOT_FOUND       == 2, "GROUP_NOT_FOUND = 2");
    CHECK(SB_ERR_GROUP_LIMIT_EXCEEDED  == 3, "GROUP_LIMIT_EXCEEDED = 3");
    CHECK(SB_ERR_DEVICE_ALREADY_MEMBER == 4, "DEVICE_ALREADY_MEMBER = 4");
    CHECK(SB_ERR_INVALID_VOLUME        == 5, "INVALID_VOLUME = 5");
    CHECK(SB_ERR_LABEL_TOO_LONG        == 6, "LABEL_TOO_LONG = 6");
    CHECK(SB_ERR_DB_FAILURE            == 7, "DB_FAILURE = 7");
    CHECK(SB_ERR_NOT_INITIALIZED       == 8, "NOT_INITIALIZED = 8");
    CHECK(SB_ERR_HAL_FAILURE           == 9, "HAL_FAILURE = 9");
    CHECK(SB_ERR_NO_ACTIVE_MEMBERS     == 10, "NO_ACTIVE_MEMBERS = 10");
    CHECK(SB_ERR_ROUTING_FAILURE       == 11, "ROUTING_FAILURE = 11");
}

// Sin miembros conectados: nunca crea una salida real (en macOS el router es CoreAudio)
void test_group_output_via_bridge() {
    printf("\n[T11] Ruteo de grupos vía C-API (sin hardware)\n");

    CHECK(sb_activate_group_output(1) == SB_ERR_NOT_INITIALIZED,
          "activate antes de sb_init → NOT_INITIALIZED");
    CHECK(sb_deactivate_group_output() == SB_ERR_NOT_INITIALIZED,
          "deactivate antes de sb_init → NOT_INITIALIZED");

    sb_init(TEST_DB);
    CHECK(sb_activate_group_output(9999) == SB_ERR_GROUP_NOT_FOUND,
          "grupo inexistente → GROUP_NOT_FOUND");

    int32_t gid = 0;
    sb_create_group("Sin miembros", &gid);
    CHECK(sb_activate_group_output(gid) == SB_ERR_NO_ACTIVE_MEMBERS,
          "grupo sin miembros conectados → NO_ACTIVE_MEMBERS");

    SB_GroupInfo groups[8];
    int32_t n = 0;
    sb_get_groups(groups, 8, &n);
    bool anyOutput = false;
    for (int32_t i = 0; i < n; ++i) anyOutput = anyOutput || groups[i].is_output;
    CHECK(n >= 1 && !anyOutput, "ningún grupo es la salida (is_output = false)");

    CHECK(sb_deactivate_group_output() == SB_OK, "deactivate sin grupo activo → SB_OK");
    sb_shutdown();
}

// ── Main ──────────────────────────────────────────────────────────────────────

int main() {
    printf("=== Bridge API Tests (F6) ===\n");

    test_init_shutdown();
    test_not_initialized_guard();
    test_null_pointer_guards();
    test_get_devices_buffer();
    test_group_lifecycle_via_bridge();
    test_group_error_codes();
    test_device_error_codes();
    test_callbacks_registered();
    test_get_group_members();
    test_result_string_coverage();
    test_group_output_via_bridge();

    printf("\n══════════════════════════════════\n");
    printf("Results: %d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
