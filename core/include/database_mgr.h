#pragma once

#include <string>
#include <vector>
#include <functional>
#include <cstdint>

// ── Structs de datos persistidos ──────────────────────────────────────────────

struct DeviceRecord {
    std::string device_uid;
    std::string custom_name;   // vacío = usar nombre nativo
    float       saved_volume;  // [0.0 – 1.0]
    bool        is_muted;
    bool        is_active;
};

struct GroupRecord {
    int32_t     id;
    std::string name;
    float       master_volume; // [0.0 – 1.0]
    bool        is_muted;
    bool        is_active;
};

struct MemberRecord {
    int32_t     group_id;
    std::string device_uid;
    float       relative_volume; // [0.0 – 1.0]
    bool        is_active;
};

// ── DatabaseManager ───────────────────────────────────────────────────────────

/**
 * Gestiona toda la persistencia local en SQLite.
 *
 * Responsabilidades:
 *  - Abrir/crear el archivo .db y aplicar PRAGMAs
 *  - Crear el schema (CREATE TABLE IF NOT EXISTS)
 *  - CRUD de devices, virtual_groups y group_devices
 *  - Debounce de 300 ms para escrituras de volumen
 *  - Carga completa del estado al iniciar la app
 */
class DatabaseManager {
public:
    DatabaseManager();
    ~DatabaseManager();

    // No copiable
    DatabaseManager(const DatabaseManager&)            = delete;
    DatabaseManager& operator=(const DatabaseManager&) = delete;

    // ── Ciclo de vida ─────────────────────────────────────────────────────────

    /**
     * Abre (o crea) el archivo .db en dbPath.
     * Aplica PRAGMAs y crea el schema si no existe.
     * Retorna false si no puede abrir el archivo.
     */
    bool open(const std::string& dbPath);

    /**
     * Hace flush de cualquier escritura pendiente (debounce) y cierra la conexión.
     */
    void close();

    bool isOpen() const;

    // ── Carga inicial ─────────────────────────────────────────────────────────

    /**
     * Carga todos los dispositivos, grupos y membresías desde SQLite.
     * Llamar una vez tras open(), antes de inicializar el AudioEngine.
     */
    bool loadDevices(std::vector<DeviceRecord>& out) const;
    bool loadGroups(std::vector<GroupRecord>& out) const;
    bool loadMembers(std::vector<MemberRecord>& out) const;

    // ── Dispositivos ──────────────────────────────────────────────────────────

    /**
     * INSERT OR REPLACE de un dispositivo.
     * Las escrituras de volumen pasan por debounce de 300 ms.
     * Las escrituras de label son inmediatas.
     */
    bool upsertDevice(const DeviceRecord& device, bool immediate = false);

    /**
     * Actualiza is_active del dispositivo y de todas sus membresías.
     * Siempre inmediato (evento de hardware, no slider).
     */
    bool setDeviceActive(const std::string& uid, bool active);

    /**
     * Actualiza custom_name del dispositivo. Siempre inmediato.
     */
    bool setDeviceLabel(const std::string& uid, const std::string& label);

    // ── Grupos ────────────────────────────────────────────────────────────────

    /**
     * Inserta un grupo nuevo. Rellena record.id con el rowid asignado.
     */
    bool insertGroup(GroupRecord& record);

    /**
     * UPDATE de nombre, master_volume o is_muted de un grupo.
     */
    bool updateGroup(const GroupRecord& record);

    /**
     * DELETE del grupo. ON DELETE CASCADE elimina sus membresías.
     */
    bool deleteGroup(int32_t groupId);

    // ── Membresías ────────────────────────────────────────────────────────────

    /**
     * INSERT de una membresía (group_id, device_uid).
     * Retorna false si el trigger de límite de 4 lo rechaza.
     */
    bool insertMember(const MemberRecord& member);

    /**
     * DELETE de una membresía específica.
     */
    bool removeMember(int32_t groupId, const std::string& deviceUid);

    /**
     * UPDATE de relative_volume de un miembro.
     */
    /** immediate = false: debounce de 300 ms (arrastre del slider de miembro). */
    bool updateMemberVolume(int32_t groupId, const std::string& deviceUid,
                            float relativeVolume, bool immediate = true);

    /**
     * UPDATE de is_active de un miembro (al conectar/desconectar dispositivo).
     */
    bool setMemberActive(int32_t groupId, const std::string& deviceUid,
                         bool active);

    // ── Flush manual ─────────────────────────────────────────────────────────

    /**
     * Fuerza la escritura inmediata de cualquier upsert pendiente en el debounce.
     * Llamar antes de cerrar la app o en puntos de checkpoint.
     */
    void flushPending();

    // ── Estado de la app (clave/valor) ────────────────────────────────────────

    /** Lee un valor; devuelve false si la clave no existe. */
    bool getAppState(const std::string& key, std::string& outValue) const;
    bool setAppState(const std::string& key, const std::string& value);
    bool deleteAppState(const std::string& key);

private:
    struct Impl;
    Impl* impl_;
};
