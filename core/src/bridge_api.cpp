#include "bridge_api.h"
#include "audio_engine.h"
#include "database_mgr.h"

#include <cstring>
#include <cstdlib>
#include <mutex>
#include <string>

// ── Helpers internos ──────────────────────────────────────────────────────────

namespace {

// Copia segura de std::string a buffer C con truncado garantizado
static void safeCopy(char* dst, size_t dstSize, const std::string& src) {
    if (!dst || dstSize == 0) return;
    size_t len = src.size() < dstSize - 1 ? src.size() : dstSize - 1;
    std::memcpy(dst, src.c_str(), len);
    dst[len] = '\0';
}

// Convierte AudioEngine::Result → SB_Result
static SB_Result toSBResult(AudioEngine::Result r) {
    switch (r) {
        case AudioEngine::Result::OK:                  return SB_OK;
        case AudioEngine::Result::DeviceNotFound:      return SB_ERR_DEVICE_NOT_FOUND;
        case AudioEngine::Result::GroupNotFound:       return SB_ERR_GROUP_NOT_FOUND;
        case AudioEngine::Result::GroupLimitExceeded:  return SB_ERR_GROUP_LIMIT_EXCEEDED;
        case AudioEngine::Result::DeviceAlreadyMember: return SB_ERR_DEVICE_ALREADY_MEMBER;
        case AudioEngine::Result::InvalidVolume:       return SB_ERR_INVALID_VOLUME;
        case AudioEngine::Result::LabelTooLong:        return SB_ERR_LABEL_TOO_LONG;
        case AudioEngine::Result::DbFailure:           return SB_ERR_DB_FAILURE;
        case AudioEngine::Result::NotInitialized:      return SB_ERR_NOT_INITIALIZED;
        case AudioEngine::Result::HalFailure:          return SB_ERR_HAL_FAILURE;
        case AudioEngine::Result::NoActiveMembers:     return SB_ERR_NO_ACTIVE_MEMBERS;
        case AudioEngine::Result::RoutingFailure:      return SB_ERR_ROUTING_FAILURE;
        default:                                       return SB_ERR_NOT_INITIALIZED;
    }
}

// Ruta por defecto del archivo de base de datos
static std::string defaultDbPath() {
#ifdef __APPLE__
    const char* home = std::getenv("HOME");
    if (!home) home = "/tmp";
    return std::string(home) +
           "/Library/Application Support/SyncBarAudio/syncbar.db";
#elif defined(_WIN32)
    const char* appdata = std::getenv("APPDATA");
    if (!appdata) appdata = "C:\\Temp";
    return std::string(appdata) + "\\SyncBarAudio\\syncbar.db";
#else
    return "/tmp/syncbar.db";
#endif
}

} // namespace

// ── Estado global del bridge ──────────────────────────────────────────────────

static AudioEngine*    g_engine    = nullptr;
static DatabaseManager* g_db       = nullptr;
static std::mutex       g_initMutex;

// Callbacks registrados por la UI
static SB_DeviceConnectedCallback    g_onConnected    = nullptr;
static SB_DeviceDisconnectedCallback g_onDisconnected = nullptr;

// ── Ciclo de vida ─────────────────────────────────────────────────────────────

SB_Result sb_init(const char* db_path) {
    std::lock_guard<std::mutex> lock(g_initMutex);

    // Idempotente: segunda llamada retorna OK sin reinicializar
    if (g_engine && g_engine->isInitialized()) return SB_OK;

    std::string path = (db_path && db_path[0] != '\0')
                       ? std::string(db_path)
                       : defaultDbPath();

    // Crear directorio si no existe (solo en macOS por ahora)
#ifdef __APPLE__
    {
        // Extraer directorio del path
        std::string dir = path.substr(0, path.rfind('/'));
        if (!dir.empty()) {
            std::string cmd = "mkdir -p \"" + dir + "\"";
            std::system(cmd.c_str());
        }
    }
#endif

    g_db = new DatabaseManager();
    if (!g_db->open(path)) {
        delete g_db;
        g_db = nullptr;
        return SB_ERR_DB_FAILURE;
    }

    g_engine = new AudioEngine();

    // Registrar callbacks de hardware antes de init()
    g_engine->registerCallbacks(
        [](const std::string& uid) {
            if (g_onConnected) g_onConnected(uid.c_str());
        },
        [](const std::string& uid) {
            if (g_onDisconnected) g_onDisconnected(uid.c_str());
        }
    );

    if (!g_engine->init(*g_db)) {
        delete g_engine; g_engine = nullptr;
        g_db->close();
        delete g_db;     g_db     = nullptr;
        return SB_ERR_DB_FAILURE;
    }

    return SB_OK;
}

void sb_register_device_callbacks(
    SB_DeviceConnectedCallback    on_connected,
    SB_DeviceDisconnectedCallback on_disconnected)
{
    std::lock_guard<std::mutex> lock(g_initMutex);
    g_onConnected    = on_connected;
    g_onDisconnected = on_disconnected;

    // Si el engine ya está inicializado, actualizar los callbacks en él
    if (g_engine) {
        g_engine->registerCallbacks(
            [](const std::string& uid) {
                if (g_onConnected) g_onConnected(uid.c_str());
            },
            [](const std::string& uid) {
                if (g_onDisconnected) g_onDisconnected(uid.c_str());
            }
        );
    }
}

void sb_shutdown(void) {
    std::lock_guard<std::mutex> lock(g_initMutex);

    if (g_engine) {
        g_engine->shutdown();
        delete g_engine;
        g_engine = nullptr;
    }
    if (g_db) {
        g_db->close();
        delete g_db;
        g_db = nullptr;
    }
    g_onConnected    = nullptr;
    g_onDisconnected = nullptr;
}

int sb_is_initialized(void) {
    return (g_engine && g_engine->isInitialized()) ? 1 : 0;
}

// ── Dispositivos ──────────────────────────────────────────────────────────────

SB_Result sb_get_devices(SB_DeviceInfo* out_devices,
                          int32_t        in_max_count,
                          int32_t*       out_count) {
    if (!out_devices || !out_count) return SB_ERR_NOT_INITIALIZED;
    if (!g_engine || !g_engine->isInitialized()) {
        *out_count = 0;
        return SB_ERR_NOT_INITIALIZED;
    }

    auto devices = g_engine->getDevices();
    int32_t n = static_cast<int32_t>(devices.size());
    if (n > in_max_count) n = in_max_count;

    for (int32_t i = 0; i < n; ++i) {
        auto& src = devices[static_cast<size_t>(i)];
        auto& dst = out_devices[i];
        std::memset(&dst, 0, sizeof(dst));
        safeCopy(dst.uid,          sizeof(dst.uid),          src.uid);
        safeCopy(dst.native_name,  sizeof(dst.native_name),  src.native_name);
        safeCopy(dst.display_name, sizeof(dst.display_name), src.display_name);
        dst.volume    = src.volume;
        dst.is_muted  = src.is_muted;
        dst.is_active = src.is_active;
        dst.has_volume_control = src.has_volume_control;
    }
    *out_count = n;
    return SB_OK;
}

SB_Result sb_set_device_volume(const char* uid, float volume) {
    if (!uid) return SB_ERR_DEVICE_NOT_FOUND;
    if (!g_engine) return SB_ERR_NOT_INITIALIZED;
    return toSBResult(g_engine->setDeviceVolume(uid, volume));
}

SB_Result sb_set_device_mute(const char* uid, bool muted) {
    if (!uid) return SB_ERR_DEVICE_NOT_FOUND;
    if (!g_engine) return SB_ERR_NOT_INITIALIZED;
    return toSBResult(g_engine->setDeviceMute(uid, muted));
}

SB_Result sb_set_device_label(const char* uid, const char* label) {
    if (!uid) return SB_ERR_DEVICE_NOT_FOUND;
    if (!g_engine) return SB_ERR_NOT_INITIALIZED;
    std::string l = (label && label[0] != '\0') ? std::string(label) : "";
    return toSBResult(g_engine->setDeviceLabel(uid, l));
}

// ── Grupos virtuales ──────────────────────────────────────────────────────────

SB_Result sb_create_group(const char* name, int32_t* out_group_id) {
    if (!name || !out_group_id) return SB_ERR_NOT_INITIALIZED;
    if (!g_engine) return SB_ERR_NOT_INITIALIZED;
    return toSBResult(g_engine->createGroup(name, *out_group_id));
}

SB_Result sb_get_groups(SB_GroupInfo* out_groups,
                         int32_t       in_max_count,
                         int32_t*      out_count) {
    if (!out_groups || !out_count) return SB_ERR_NOT_INITIALIZED;
    if (!g_engine || !g_engine->isInitialized()) {
        *out_count = 0;
        return SB_ERR_NOT_INITIALIZED;
    }

    auto groups = g_engine->getGroups();
    int32_t n = static_cast<int32_t>(groups.size());
    if (n > in_max_count) n = in_max_count;

    for (int32_t i = 0; i < n; ++i) {
        auto& src = groups[static_cast<size_t>(i)];
        auto& dst = out_groups[i];
        std::memset(&dst, 0, sizeof(dst));
        dst.id            = src.id;
        safeCopy(dst.name, sizeof(dst.name), src.name);
        dst.master_volume = src.master_volume;
        dst.is_muted      = src.is_muted;
        dst.is_active     = src.is_active;
        dst.member_count  = src.member_count;
        dst.is_output     = src.is_output;
    }
    *out_count = n;
    return SB_OK;
}

SB_Result sb_get_group_members(int32_t        group_id,
                                SB_MemberInfo* out_members,
                                int32_t*       out_count) {
    if (!out_members || !out_count) return SB_ERR_NOT_INITIALIZED;
    if (!g_engine) return SB_ERR_NOT_INITIALIZED;

    auto members = g_engine->getGroupMembers(group_id);
    if (members.empty() && group_id > 0) {
        // Verificar si el grupo existe
        auto groups = g_engine->getGroups();
        bool found = false;
        for (auto& g : groups) { if (g.id == group_id) { found = true; break; } }
        if (!found) { *out_count = 0; return SB_ERR_GROUP_NOT_FOUND; }
    }

    int32_t n = static_cast<int32_t>(members.size());
    for (int32_t i = 0; i < n; ++i) {
        auto& src = members[static_cast<size_t>(i)];
        auto& dst = out_members[i];
        std::memset(&dst, 0, sizeof(dst));
        safeCopy(dst.device_uid, sizeof(dst.device_uid), src.device_uid);
        dst.relative_volume = src.relative_volume;
        dst.is_active       = src.is_active;
    }
    *out_count = n;
    return SB_OK;
}

SB_Result sb_add_device_to_group(int32_t group_id, const char* device_uid) {
    if (!device_uid) return SB_ERR_DEVICE_NOT_FOUND;
    if (!g_engine) return SB_ERR_NOT_INITIALIZED;
    return toSBResult(g_engine->addDeviceToGroup(group_id, device_uid));
}

SB_Result sb_remove_device_from_group(int32_t group_id, const char* device_uid) {
    if (!device_uid) return SB_ERR_DEVICE_NOT_FOUND;
    if (!g_engine) return SB_ERR_NOT_INITIALIZED;
    return toSBResult(g_engine->removeDeviceFromGroup(group_id, device_uid));
}

SB_Result sb_set_group_volume(int32_t group_id, float master_volume) {
    if (!g_engine) return SB_ERR_NOT_INITIALIZED;
    return toSBResult(g_engine->setGroupVolume(group_id, master_volume));
}

SB_Result sb_set_group_mute(int32_t group_id, bool muted) {
    if (!g_engine) return SB_ERR_NOT_INITIALIZED;
    return toSBResult(g_engine->setGroupMute(group_id, muted));
}

SB_Result sb_set_group_name(int32_t group_id, const char* name) {
    if (!name) return SB_ERR_LABEL_TOO_LONG;
    if (!g_engine) return SB_ERR_NOT_INITIALIZED;
    return toSBResult(g_engine->setGroupName(group_id, name));
}

SB_Result sb_delete_group(int32_t group_id) {
    if (!g_engine) return SB_ERR_NOT_INITIALIZED;
    return toSBResult(g_engine->deleteGroup(group_id));
}

// ── Ruteo ─────────────────────────────────────────────────────────────────────

SB_Result sb_activate_group_output(int32_t group_id) {
    if (!g_engine) return SB_ERR_NOT_INITIALIZED;
    return toSBResult(g_engine->activateGroupOutput(group_id));
}

SB_Result sb_deactivate_group_output(void) {
    if (!g_engine) return SB_ERR_NOT_INITIALIZED;
    return toSBResult(g_engine->deactivateGroupOutput());
}
