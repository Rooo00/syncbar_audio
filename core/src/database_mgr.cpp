#include "database_mgr.h"
#include "sqlite3.h"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <stdexcept>
#include <cstring>

// ── Helpers internos ──────────────────────────────────────────────────────────

namespace {

// Ejecuta una sentencia SQL sin resultados. Retorna true si SQLITE_OK.
bool exec(sqlite3* db, const char* sql) {
    char* errMsg = nullptr;
    int rc = sqlite3_exec(db, sql, nullptr, nullptr, &errMsg);
    if (rc != SQLITE_OK) {
        // errMsg se libera aunque no lo usemos
        sqlite3_free(errMsg);
        return false;
    }
    return true;
}

// RAII wrapper para sqlite3_stmt
struct Stmt {
    sqlite3_stmt* s = nullptr;
    explicit Stmt(sqlite3* db, const char* sql) {
        sqlite3_prepare_v2(db, sql, -1, &s, nullptr);
    }
    ~Stmt() { if (s) sqlite3_finalize(s); }
    bool ok() const { return s != nullptr; }
    // No copiable
    Stmt(const Stmt&) = delete;
    Stmt& operator=(const Stmt&) = delete;
};

using Clock     = std::chrono::steady_clock;
using TimePoint = std::chrono::time_point<Clock>;

} // namespace

// ── Impl ──────────────────────────────────────────────────────────────────────

struct DatabaseManager::Impl {
    sqlite3* db = nullptr;

    // Debounce: mapa uid → (record pendiente, timestamp de última modificación)
    struct PendingWrite {
        DeviceRecord record;
        TimePoint    timestamp;
    };
    std::unordered_map<std::string, PendingWrite> pendingDevices;
    std::mutex                                    pendingMutex;

    static constexpr auto kDebounceMs = std::chrono::milliseconds(300);

    // Hilo que escribe los pendientes cuando vence su ventana de debounce,
    // sin esperar a que llegue otra escritura (US-04).
    std::thread             flusher;
    std::condition_variable flushCv;
    bool                    stopFlusher = false;  // protegido por pendingMutex

    void flusherLoop() {
        std::unique_lock<std::mutex> lock(pendingMutex);
        while (!stopFlusher) {
            if (pendingDevices.empty()) {
                flushCv.wait(lock);
                continue;
            }
            // Escribir los vencidos y calcular el próximo vencimiento
            auto now = Clock::now();
            TimePoint next = TimePoint::max();
            for (auto it = pendingDevices.begin(); it != pendingDevices.end(); ) {
                auto due = it->second.timestamp + kDebounceMs;
                if (due <= now) {
                    writeDevice(it->second.record);
                    it = pendingDevices.erase(it);
                } else {
                    if (due < next) next = due;
                    ++it;
                }
            }
            if (next != TimePoint::max()) flushCv.wait_until(lock, next);
        }
    }

    void startFlusher() {
        {
            std::lock_guard<std::mutex> lock(pendingMutex);
            stopFlusher = false;
        }
        flusher = std::thread([this] { flusherLoop(); });
    }

    void stopFlusherThread() {
        {
            std::lock_guard<std::mutex> lock(pendingMutex);
            stopFlusher = true;
        }
        flushCv.notify_all();
        if (flusher.joinable()) flusher.join();
    }

    // Escribe el DeviceRecord directamente en SQLite (sin debounce)
    bool writeDevice(const DeviceRecord& d) {
        const char* sql =
            "INSERT INTO devices (device_uid, custom_name, saved_volume, is_muted, is_active) "
            "VALUES (?, ?, ?, ?, ?) "
            "ON CONFLICT(device_uid) DO UPDATE SET "
            "  custom_name  = excluded.custom_name, "
            "  saved_volume = excluded.saved_volume, "
            "  is_muted     = excluded.is_muted, "
            "  is_active    = excluded.is_active;";

        Stmt stmt(db, sql);
        if (!stmt.ok()) return false;

        sqlite3_bind_text(stmt.s, 1, d.device_uid.c_str(), -1, SQLITE_TRANSIENT);
        // custom_name: NULL si vacío
        if (d.custom_name.empty())
            sqlite3_bind_null(stmt.s, 2);
        else
            sqlite3_bind_text(stmt.s, 2, d.custom_name.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_double(stmt.s, 3, static_cast<double>(d.saved_volume));
        sqlite3_bind_int(stmt.s, 4, d.is_muted  ? 1 : 0);
        sqlite3_bind_int(stmt.s, 5, d.is_active ? 1 : 0);

        return sqlite3_step(stmt.s) == SQLITE_DONE;
    }
};

// ── Constructor / Destructor ──────────────────────────────────────────────────

DatabaseManager::DatabaseManager() : impl_(new Impl()) {}

DatabaseManager::~DatabaseManager() {
    close();
    delete impl_;
}

// ── Ciclo de vida ─────────────────────────────────────────────────────────────

bool DatabaseManager::open(const std::string& dbPath) {
    int rc = sqlite3_open(dbPath.c_str(), &impl_->db);
    if (rc != SQLITE_OK) return false;

    // ── PRAGMAs de rendimiento e integridad ───────────────────────────────────
    exec(impl_->db, "PRAGMA journal_mode = WAL;");
    exec(impl_->db, "PRAGMA foreign_keys = ON;");
    exec(impl_->db, "PRAGMA synchronous = NORMAL;");
    exec(impl_->db, "PRAGMA cache_size = -8000;");  // 8 MB

    // ── Schema ────────────────────────────────────────────────────────────────
    const char* schema = R"SQL(

        CREATE TABLE IF NOT EXISTS devices (
            device_uid      TEXT    NOT NULL,
            custom_name     TEXT,
            saved_volume    REAL    NOT NULL DEFAULT 1.0,
            is_muted        INTEGER NOT NULL DEFAULT 0,
            is_active       INTEGER NOT NULL DEFAULT 1,
            CONSTRAINT pk_devices          PRIMARY KEY (device_uid),
            CONSTRAINT chk_volume          CHECK (saved_volume >= 0.0 AND saved_volume <= 1.0),
            CONSTRAINT chk_is_muted        CHECK (is_muted  IN (0, 1)),
            CONSTRAINT chk_is_active       CHECK (is_active IN (0, 1))
        );

        CREATE TABLE IF NOT EXISTS virtual_groups (
            id              INTEGER NOT NULL,
            name            TEXT    NOT NULL DEFAULT 'Nuevo Grupo',
            master_volume   REAL    NOT NULL DEFAULT 1.0,
            is_muted        INTEGER NOT NULL DEFAULT 0,
            is_active       INTEGER NOT NULL DEFAULT 1,
            CONSTRAINT pk_virtual_groups   PRIMARY KEY (id AUTOINCREMENT),
            CONSTRAINT chk_name_length     CHECK (length(name) <= 64),
            CONSTRAINT chk_master_volume   CHECK (master_volume >= 0.0 AND master_volume <= 1.0),
            CONSTRAINT chk_group_muted     CHECK (is_muted  IN (0, 1)),
            CONSTRAINT chk_group_active    CHECK (is_active IN (0, 1))
        );

        CREATE TABLE IF NOT EXISTS group_devices (
            group_id        INTEGER NOT NULL,
            device_uid      TEXT    NOT NULL,
            relative_volume REAL    NOT NULL DEFAULT 1.0,
            is_active       INTEGER NOT NULL DEFAULT 1,
            CONSTRAINT pk_group_devices    PRIMARY KEY (group_id, device_uid),
            CONSTRAINT fk_group  FOREIGN KEY (group_id)
                REFERENCES virtual_groups(id) ON DELETE CASCADE ON UPDATE CASCADE,
            CONSTRAINT fk_device FOREIGN KEY (device_uid)
                REFERENCES devices(device_uid) ON DELETE CASCADE ON UPDATE CASCADE,
            CONSTRAINT chk_rel_volume      CHECK (relative_volume >= 0.0 AND relative_volume <= 1.0),
            CONSTRAINT chk_member_active   CHECK (is_active IN (0, 1))
        );

        CREATE INDEX IF NOT EXISTS idx_group_devices_device
            ON group_devices(device_uid);

        CREATE INDEX IF NOT EXISTS idx_group_devices_group
            ON group_devices(group_id);

        CREATE TRIGGER IF NOT EXISTS trg_max_devices_per_group
        BEFORE INSERT ON group_devices
        BEGIN
            SELECT RAISE(ABORT, 'GROUP_DEVICE_LIMIT_EXCEEDED')
            WHERE (
                SELECT COUNT(*) FROM group_devices
                WHERE group_id = NEW.group_id
            ) >= 4;
        END;

        -- Estado de la app (clave/valor). Hoy guarda la salida por defecto
        -- previa mientras un grupo está activo como salida (ADR-07), para
        -- restaurarla si la app se cerró de golpe.
        CREATE TABLE IF NOT EXISTS app_state (
            key     TEXT NOT NULL,
            value   TEXT NOT NULL,
            CONSTRAINT pk_app_state PRIMARY KEY (key)
        );

        CREATE TRIGGER IF NOT EXISTS trg_update_group_active
        AFTER UPDATE OF is_active ON group_devices
        BEGIN
            UPDATE virtual_groups
            SET is_active = (
                SELECT MAX(is_active) FROM group_devices
                WHERE group_id = NEW.group_id
            )
            WHERE id = NEW.group_id;
        END;

    )SQL";

    if (!exec(impl_->db, schema)) return false;

    impl_->startFlusher();
    return true;
}

void DatabaseManager::close() {
    if (!impl_->db) return;
    impl_->stopFlusherThread();
    flushPending();
    sqlite3_close(impl_->db);
    impl_->db = nullptr;
}

bool DatabaseManager::isOpen() const {
    return impl_->db != nullptr;
}

// ── Carga inicial ─────────────────────────────────────────────────────────────

bool DatabaseManager::loadDevices(std::vector<DeviceRecord>& out) const {
    const char* sql =
        "SELECT device_uid, COALESCE(custom_name,''), saved_volume, is_muted, is_active "
        "FROM devices;";

    Stmt stmt(impl_->db, sql);
    if (!stmt.ok()) return false;

    out.clear();
    while (sqlite3_step(stmt.s) == SQLITE_ROW) {
        DeviceRecord r;
        r.device_uid   = reinterpret_cast<const char*>(sqlite3_column_text(stmt.s, 0));
        r.custom_name  = reinterpret_cast<const char*>(sqlite3_column_text(stmt.s, 1));
        r.saved_volume = static_cast<float>(sqlite3_column_double(stmt.s, 2));
        r.is_muted     = sqlite3_column_int(stmt.s, 3) != 0;
        r.is_active    = sqlite3_column_int(stmt.s, 4) != 0;
        out.push_back(std::move(r));
    }
    return true;
}

bool DatabaseManager::loadGroups(std::vector<GroupRecord>& out) const {
    const char* sql =
        "SELECT id, name, master_volume, is_muted, is_active "
        "FROM virtual_groups;";

    Stmt stmt(impl_->db, sql);
    if (!stmt.ok()) return false;

    out.clear();
    while (sqlite3_step(stmt.s) == SQLITE_ROW) {
        GroupRecord r;
        r.id            = sqlite3_column_int(stmt.s, 0);
        r.name          = reinterpret_cast<const char*>(sqlite3_column_text(stmt.s, 1));
        r.master_volume = static_cast<float>(sqlite3_column_double(stmt.s, 2));
        r.is_muted      = sqlite3_column_int(stmt.s, 3) != 0;
        r.is_active     = sqlite3_column_int(stmt.s, 4) != 0;
        out.push_back(std::move(r));
    }
    return true;
}

bool DatabaseManager::loadMembers(std::vector<MemberRecord>& out) const {
    const char* sql =
        "SELECT group_id, device_uid, relative_volume, is_active "
        "FROM group_devices;";

    Stmt stmt(impl_->db, sql);
    if (!stmt.ok()) return false;

    out.clear();
    while (sqlite3_step(stmt.s) == SQLITE_ROW) {
        MemberRecord r;
        r.group_id        = sqlite3_column_int(stmt.s, 0);
        r.device_uid      = reinterpret_cast<const char*>(sqlite3_column_text(stmt.s, 1));
        r.relative_volume = static_cast<float>(sqlite3_column_double(stmt.s, 2));
        r.is_active       = sqlite3_column_int(stmt.s, 3) != 0;
        out.push_back(std::move(r));
    }
    return true;
}

// ── Dispositivos ──────────────────────────────────────────────────────────────

bool DatabaseManager::upsertDevice(const DeviceRecord& device, bool immediate) {
    if (immediate) {
        // Escritura directa (labels, eventos de hardware)
        std::lock_guard<std::mutex> lock(impl_->pendingMutex);
        // Si había un pending para este uid, lo descartamos (ya escribimos directo)
        impl_->pendingDevices.erase(device.device_uid);
        return impl_->writeDevice(device);
    }

    // Escritura con debounce (sliders de volumen)
    std::lock_guard<std::mutex> lock(impl_->pendingMutex);
    auto now = Clock::now();

    auto it = impl_->pendingDevices.find(device.device_uid);
    if (it != impl_->pendingDevices.end()) {
        // Ya hay un pending: actualizar el record y el timestamp
        it->second.record    = device;
        it->second.timestamp = now;
    } else {
        impl_->pendingDevices[device.device_uid] = { device, now };
    }

    impl_->flushCv.notify_one();
    return true;
}

bool DatabaseManager::setDeviceActive(const std::string& uid, bool active) {
    // Actualiza el dispositivo
    {
        const char* sql = "UPDATE devices SET is_active = ? WHERE device_uid = ?;";
        Stmt stmt(impl_->db, sql);
        if (!stmt.ok()) return false;
        sqlite3_bind_int(stmt.s, 1, active ? 1 : 0);
        sqlite3_bind_text(stmt.s, 2, uid.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt.s) != SQLITE_DONE) return false;
    }
    // Actualiza todas sus membresías (el trigger trg_update_group_active
    // se encarga de recalcular virtual_groups.is_active)
    {
        const char* sql = "UPDATE group_devices SET is_active = ? WHERE device_uid = ?;";
        Stmt stmt(impl_->db, sql);
        if (!stmt.ok()) return false;
        sqlite3_bind_int(stmt.s, 1, active ? 1 : 0);
        sqlite3_bind_text(stmt.s, 2, uid.c_str(), -1, SQLITE_TRANSIENT);
        return sqlite3_step(stmt.s) == SQLITE_DONE;
    }
}

bool DatabaseManager::setDeviceLabel(const std::string& uid, const std::string& label) {
    const char* sql = "UPDATE devices SET custom_name = ? WHERE device_uid = ?;";
    Stmt stmt(impl_->db, sql);
    if (!stmt.ok()) return false;

    if (label.empty())
        sqlite3_bind_null(stmt.s, 1);
    else
        sqlite3_bind_text(stmt.s, 1, label.c_str(), -1, SQLITE_TRANSIENT);

    sqlite3_bind_text(stmt.s, 2, uid.c_str(), -1, SQLITE_TRANSIENT);
    return sqlite3_step(stmt.s) == SQLITE_DONE;
}

// ── Grupos ────────────────────────────────────────────────────────────────────

bool DatabaseManager::insertGroup(GroupRecord& record) {
    const char* sql =
        "INSERT INTO virtual_groups (name, master_volume, is_muted, is_active) "
        "VALUES (?, ?, ?, ?);";

    Stmt stmt(impl_->db, sql);
    if (!stmt.ok()) return false;

    sqlite3_bind_text(stmt.s, 1, record.name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(stmt.s, 2, static_cast<double>(record.master_volume));
    sqlite3_bind_int(stmt.s, 3, record.is_muted  ? 1 : 0);
    sqlite3_bind_int(stmt.s, 4, record.is_active ? 1 : 0);

    if (sqlite3_step(stmt.s) != SQLITE_DONE) return false;

    record.id = static_cast<int32_t>(sqlite3_last_insert_rowid(impl_->db));
    return true;
}

bool DatabaseManager::updateGroup(const GroupRecord& record) {
    const char* sql =
        "UPDATE virtual_groups "
        "SET name = ?, master_volume = ?, is_muted = ?, is_active = ? "
        "WHERE id = ?;";

    Stmt stmt(impl_->db, sql);
    if (!stmt.ok()) return false;

    sqlite3_bind_text(stmt.s, 1, record.name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(stmt.s, 2, static_cast<double>(record.master_volume));
    sqlite3_bind_int(stmt.s, 3, record.is_muted  ? 1 : 0);
    sqlite3_bind_int(stmt.s, 4, record.is_active ? 1 : 0);
    sqlite3_bind_int(stmt.s, 5, record.id);

    return sqlite3_step(stmt.s) == SQLITE_DONE;
}

bool DatabaseManager::deleteGroup(int32_t groupId) {
    // ON DELETE CASCADE en group_devices elimina las membresías automáticamente
    const char* sql = "DELETE FROM virtual_groups WHERE id = ?;";
    Stmt stmt(impl_->db, sql);
    if (!stmt.ok()) return false;
    sqlite3_bind_int(stmt.s, 1, groupId);
    return sqlite3_step(stmt.s) == SQLITE_DONE;
}

// ── Membresías ────────────────────────────────────────────────────────────────

bool DatabaseManager::insertMember(const MemberRecord& member) {
    const char* sql =
        "INSERT INTO group_devices (group_id, device_uid, relative_volume, is_active) "
        "VALUES (?, ?, ?, ?);";

    Stmt stmt(impl_->db, sql);
    if (!stmt.ok()) return false;

    sqlite3_bind_int(stmt.s, 1, member.group_id);
    sqlite3_bind_text(stmt.s, 2, member.device_uid.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_double(stmt.s, 3, static_cast<double>(member.relative_volume));
    sqlite3_bind_int(stmt.s, 4, member.is_active ? 1 : 0);

    // El trigger trg_max_devices_per_group retorna SQLITE_CONSTRAINT si hay >= 4
    int rc = sqlite3_step(stmt.s);
    return rc == SQLITE_DONE;
}

bool DatabaseManager::removeMember(int32_t groupId, const std::string& deviceUid) {
    const char* sql =
        "DELETE FROM group_devices WHERE group_id = ? AND device_uid = ?;";
    Stmt stmt(impl_->db, sql);
    if (!stmt.ok()) return false;
    sqlite3_bind_int(stmt.s, 1, groupId);
    sqlite3_bind_text(stmt.s, 2, deviceUid.c_str(), -1, SQLITE_TRANSIENT);
    return sqlite3_step(stmt.s) == SQLITE_DONE;
}

bool DatabaseManager::updateMemberVolume(int32_t groupId,
                                         const std::string& deviceUid,
                                         float relativeVolume) {
    const char* sql =
        "UPDATE group_devices SET relative_volume = ? "
        "WHERE group_id = ? AND device_uid = ?;";
    Stmt stmt(impl_->db, sql);
    if (!stmt.ok()) return false;
    sqlite3_bind_double(stmt.s, 1, static_cast<double>(relativeVolume));
    sqlite3_bind_int(stmt.s, 2, groupId);
    sqlite3_bind_text(stmt.s, 3, deviceUid.c_str(), -1, SQLITE_TRANSIENT);
    return sqlite3_step(stmt.s) == SQLITE_DONE;
}

bool DatabaseManager::setMemberActive(int32_t groupId,
                                      const std::string& deviceUid,
                                      bool active) {
    const char* sql =
        "UPDATE group_devices SET is_active = ? "
        "WHERE group_id = ? AND device_uid = ?;";
    Stmt stmt(impl_->db, sql);
    if (!stmt.ok()) return false;
    sqlite3_bind_int(stmt.s, 1, active ? 1 : 0);
    sqlite3_bind_int(stmt.s, 2, groupId);
    sqlite3_bind_text(stmt.s, 3, deviceUid.c_str(), -1, SQLITE_TRANSIENT);
    // El trigger trg_update_group_active recalcula virtual_groups.is_active
    return sqlite3_step(stmt.s) == SQLITE_DONE;
}

// ── Flush ─────────────────────────────────────────────────────────────────────

void DatabaseManager::flushPending() {
    std::lock_guard<std::mutex> lock(impl_->pendingMutex);
    for (auto& [uid, pending] : impl_->pendingDevices) {
        impl_->writeDevice(pending.record);
    }
    impl_->pendingDevices.clear();
}

// ── Estado de la app (clave/valor) ────────────────────────────────────────────

bool DatabaseManager::getAppState(const std::string& key, std::string& outValue) const {
    Stmt stmt(impl_->db, "SELECT value FROM app_state WHERE key = ?;");
    if (!stmt.ok()) return false;
    sqlite3_bind_text(stmt.s, 1, key.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt.s) != SQLITE_ROW) return false;
    const unsigned char* text = sqlite3_column_text(stmt.s, 0);
    outValue = text ? reinterpret_cast<const char*>(text) : "";
    return true;
}

bool DatabaseManager::setAppState(const std::string& key, const std::string& value) {
    Stmt stmt(impl_->db,
        "INSERT INTO app_state (key, value) VALUES (?, ?) "
        "ON CONFLICT(key) DO UPDATE SET value = excluded.value;");
    if (!stmt.ok()) return false;
    sqlite3_bind_text(stmt.s, 1, key.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt.s, 2, value.c_str(), -1, SQLITE_TRANSIENT);
    return sqlite3_step(stmt.s) == SQLITE_DONE;
}

bool DatabaseManager::deleteAppState(const std::string& key) {
    Stmt stmt(impl_->db, "DELETE FROM app_state WHERE key = ?;");
    if (!stmt.ok()) return false;
    sqlite3_bind_text(stmt.s, 1, key.c_str(), -1, SQLITE_TRANSIENT);
    return sqlite3_step(stmt.s) == SQLITE_DONE;
}
