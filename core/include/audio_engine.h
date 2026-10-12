#pragma once

#include "database_mgr.h"
#include "audio_logic.h"

#include <string>
#include <vector>
#include <functional>
#include <cstdint>

// ── Structs de estado en runtime ──────────────────────────────────────────────

/**
 * Snapshot de un dispositivo de audio tal como lo ve el AudioEngine en memoria.
 * display_name = custom_name si existe, sino native_name.
 */
struct DeviceInfo {
    std::string uid;
    std::string native_name;
    std::string display_name;
    float       volume;      // [0.0 – 1.0]
    bool        is_muted;
    bool        is_active;   // false = desconectado físicamente
};

/**
 * Snapshot de un grupo virtual en runtime.
 */
struct GroupInfo {
    int32_t     id;
    std::string name;
    float       master_volume;
    bool        is_muted;    // Mute lógico (no propaga al hardware)
    bool        is_active;   // false si todos los miembros están inactivos
    int32_t     member_count;
};

/**
 * Snapshot de un miembro de grupo en runtime.
 */
struct MemberInfo {
    std::string device_uid;
    float       relative_volume;
    bool        is_active;
};

// ── AudioEngine ───────────────────────────────────────────────────────────────

/**
 * Núcleo del motor de audio.
 *
 * Responsabilidades:
 *  - Inicializar el HAL (CoreAudio en macOS, WASAPI en Windows)
 *  - Mantener el estado en memoria de dispositivos y grupos
 *  - Aplicar cambios de volumen/mute al hardware vía HAL
 *  - Gestionar grupos virtuales y calcular proporciones de volumen
 *  - Propagar eventos de hardware (connect/disconnect) a la UI
 *  - Coordinar la persistencia con DatabaseManager
 */
class AudioEngine {
public:
    // Impl es público para que el callback estático de CoreAudio pueda hacer el cast.
    // No forma parte de la API pública — no usar desde fuera del Core.
    struct Impl;
    AudioEngine();
    ~AudioEngine();

    // No copiable
    AudioEngine(const AudioEngine&)            = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    // ── Ciclo de vida ─────────────────────────────────────────────────────────

    /**
     * Inicializa el HAL, carga el estado desde SQLite y registra listeners.
     * Debe llamarse desde el hilo principal antes de cualquier otra operación.
     */
    bool init(DatabaseManager& db);

    /**
     * Detiene el HAL, hace flush de pendientes y libera recursos.
     */
    void shutdown();

    bool isInitialized() const;

    // ── Callbacks de hardware ─────────────────────────────────────────────────

    /**
     * Registra callbacks para eventos de conexión/desconexión de hardware.
     * Los callbacks se invocan desde el hilo del HAL.
     * La UI debe hacer dispatch al hilo principal antes de actualizar la vista.
     */
    void registerCallbacks(
        std::function<void(const std::string& uid)> onConnect,
        std::function<void(const std::string& uid)> onDisconnect
    );

    // ── Dispositivos ──────────────────────────────────────────────────────────

    std::vector<DeviceInfo> getDevices() const;

    // SB_Result codes (definidos en bridge_api.h, replicados aquí para
    // no crear dependencia circular en el engine)
    enum class Result {
        OK                      = 0,
        DeviceNotFound          = 1,
        GroupNotFound           = 2,
        GroupLimitExceeded      = 3,
        DeviceAlreadyMember     = 4,
        InvalidVolume           = 5,
        LabelTooLong            = 6,
        DbFailure               = 7,
        NotInitialized          = 8,
    };

    Result setDeviceVolume(const std::string& uid, float volume);
    Result setDeviceMute(const std::string& uid, bool muted);
    Result setDeviceLabel(const std::string& uid, const std::string& label);

    // ── Grupos ────────────────────────────────────────────────────────────────

    Result createGroup(const std::string& name, int32_t& outId);
    std::vector<GroupInfo>  getGroups() const;
    std::vector<MemberInfo> getGroupMembers(int32_t groupId) const;
    Result addDeviceToGroup(int32_t groupId, const std::string& uid);
    Result removeDeviceFromGroup(int32_t groupId, const std::string& uid);
    Result setGroupVolume(int32_t groupId, float masterVolume);
    Result setGroupMute(int32_t groupId, bool muted);
    Result setGroupName(int32_t groupId, const std::string& name);
    Result deleteGroup(int32_t groupId);

private:
    Impl* impl_;
};
