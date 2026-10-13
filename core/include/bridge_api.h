#pragma once

#include <stdint.h>
#include <stdbool.h>

/**
 * bridge_api.h — C-API pública del Core Engine (SyncBar Audio)
 *
 * Esta es la ÚNICA interfaz que las capas de UI deben usar:
 *   - macOS (Swift):   via .bridging-header
 *   - Windows (C#):    via P/Invoke [DllImport]
 *
 * Reglas de uso:
 *   1. Llamar sb_init() una vez al arrancar la app.
 *   2. Registrar callbacks con sb_register_device_callbacks() antes de sb_init()
 *      o inmediatamente después.
 *   3. Los callbacks se invocan desde el hilo del HAL — hacer dispatch al
 *      hilo principal de la UI antes de actualizar vistas.
 *   4. Llamar sb_shutdown() al cerrar la app.
 *
 * Diseño completo: aidlc-docs/design-artifacts/api-specification.md
 */

#ifdef _WIN32
  #ifdef SYNCBAR_BUILDING_DLL
    #define SB_API __declspec(dllexport)
  #else
    #define SB_API __declspec(dllimport)
  #endif
#else
  #define SB_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ── Códigos de resultado ──────────────────────────────────────────────────── */

typedef enum SB_Result {
    SB_OK                        = 0,
    SB_ERR_DEVICE_NOT_FOUND      = 1,
    SB_ERR_GROUP_NOT_FOUND       = 2,
    SB_ERR_GROUP_LIMIT_EXCEEDED  = 3,  // Grupo ya tiene 4 dispositivos
    SB_ERR_DEVICE_ALREADY_MEMBER = 4,
    SB_ERR_INVALID_VOLUME        = 5,  // Valor fuera de [0.0, 1.0]
    SB_ERR_LABEL_TOO_LONG        = 6,  // Label > 64 caracteres UTF-8
    SB_ERR_DB_FAILURE            = 7,
    SB_ERR_NOT_INITIALIZED       = 8,
    SB_ERR_HAL_FAILURE           = 9,  // El hardware rechazó el cambio
} SB_Result;

/* ── Structs de datos (snapshots de solo lectura para la UI) ───────────────── */

/**
 * Snapshot de un dispositivo de audio.
 * display_name = custom_name si existe, sino native_name.
 */
typedef struct SB_DeviceInfo {
    char    uid[256];           // UID persistente del hardware
    char    native_name[128];   // Nombre reportado por el SO
    char    display_name[128];  // Nombre a mostrar en la UI
    float   volume;             // [0.0 – 1.0]
    bool    is_muted;
    bool    is_active;          // false = desconectado físicamente
    bool    has_volume_control; // false = el hardware no expone volumen (HDMI, etc.)
} SB_DeviceInfo;

/**
 * Snapshot de un grupo virtual.
 */
typedef struct SB_GroupInfo {
    int32_t id;
    char    name[128];
    float   master_volume;      // [0.0 – 1.0]
    bool    is_muted;           // Mute lógico (no propaga al hardware)
    bool    is_active;          // false si todos los miembros están inactivos
    int32_t member_count;       // Número de miembros (0–4)
} SB_GroupInfo;

/**
 * Snapshot de un miembro de grupo.
 */
typedef struct SB_MemberInfo {
    char    device_uid[256];
    float   relative_volume;    // Balance relativo dentro del grupo [0.0 – 1.0]
    bool    is_active;
} SB_MemberInfo;

/* ── Callbacks de eventos de hardware ─────────────────────────────────────── */

/** Invocado cuando un dispositivo de audio se conecta al sistema. */
typedef void (*SB_DeviceConnectedCallback)(const char* uid);

/** Invocado cuando un dispositivo de audio se desconecta del sistema. */
typedef void (*SB_DeviceDisconnectedCallback)(const char* uid);

/* ── Ciclo de vida ─────────────────────────────────────────────────────────── */

/**
 * Inicializa el motor de audio, carga estado desde SQLite y registra el HAL.
 *
 * @param db_path  Ruta absoluta al archivo .db (UTF-8). Pasa NULL para usar
 *                 la ruta por defecto del sistema (~/.config/syncbar/syncbar.db).
 * @return SB_OK o SB_ERR_DB_FAILURE
 *
 * Es idempotente: una segunda llamada retorna SB_OK sin reinicializar.
 */
SB_API SB_Result sb_init(const char* db_path);

/**
 * Registra callbacks para eventos de conexión/desconexión de hardware.
 * Puede llamarse antes o después de sb_init().
 * Pasar NULL para desregistrar un callback.
 */
SB_API void sb_register_device_callbacks(
    SB_DeviceConnectedCallback    on_connected,
    SB_DeviceDisconnectedCallback on_disconnected
);

/**
 * Libera todos los recursos del motor. Hace flush de escrituras pendientes.
 * Llamar al cerrar la app.
 */
SB_API void sb_shutdown(void);

/**
 * Retorna 1 si el motor está inicializado, 0 si no.
 */
SB_API int sb_is_initialized(void);

/* ── Dispositivos ──────────────────────────────────────────────────────────── */

/**
 * Retorna la lista de todos los dispositivos conocidos (activos e inactivos).
 *
 * @param out_devices   Buffer pre-allocado por el caller.
 * @param in_max_count  Tamaño del buffer (número de SB_DeviceInfo).
 * @param out_count     Número de dispositivos escritos en out_devices.
 * @return SB_OK o SB_ERR_NOT_INITIALIZED
 */
SB_API SB_Result sb_get_devices(
    SB_DeviceInfo* out_devices,
    int32_t        in_max_count,
    int32_t*       out_count
);

/**
 * Establece el volumen de un dispositivo.
 * @param volume [0.0 – 1.0]. El núcleo convierte a dB para el HAL.
 * Si el dispositivo está desconectado, el valor solo se guarda.
 * @return SB_OK | SB_ERR_DEVICE_NOT_FOUND | SB_ERR_INVALID_VOLUME |
 *         SB_ERR_HAL_FAILURE (el hardware rechazó el cambio; no se guarda)
 */
SB_API SB_Result sb_set_device_volume(const char* uid, float volume);

/**
 * Establece el estado de mute de un dispositivo individual.
 * @return SB_OK | SB_ERR_DEVICE_NOT_FOUND |
 *         SB_ERR_HAL_FAILURE (el hardware rechazó el cambio, p. ej. HDMI sin mute)
 */
SB_API SB_Result sb_set_device_mute(const char* uid, bool muted);

/**
 * Establece el nombre personalizado de un dispositivo.
 * @param label UTF-8, máximo 64 caracteres. NULL o "" para limpiar el label.
 * @return SB_OK | SB_ERR_DEVICE_NOT_FOUND | SB_ERR_LABEL_TOO_LONG
 */
SB_API SB_Result sb_set_device_label(const char* uid, const char* label);

/* ── Grupos virtuales ──────────────────────────────────────────────────────── */

/**
 * Crea un nuevo grupo virtual.
 * @param name        Nombre del grupo (UTF-8, máx 64 chars).
 * @param out_group_id ID asignado al nuevo grupo.
 * @return SB_OK | SB_ERR_LABEL_TOO_LONG | SB_ERR_DB_FAILURE
 */
SB_API SB_Result sb_create_group(const char* name, int32_t* out_group_id);

/**
 * Retorna la lista de todos los grupos.
 * @param out_groups    Buffer pre-allocado por el caller.
 * @param in_max_count  Tamaño del buffer.
 * @param out_count     Número de grupos escritos.
 * @return SB_OK | SB_ERR_NOT_INITIALIZED
 */
SB_API SB_Result sb_get_groups(
    SB_GroupInfo* out_groups,
    int32_t       in_max_count,
    int32_t*      out_count
);

/**
 * Retorna los miembros de un grupo específico.
 * @param out_members Buffer de tamaño mínimo 4 (límite fijo por grupo).
 * @return SB_OK | SB_ERR_GROUP_NOT_FOUND
 */
SB_API SB_Result sb_get_group_members(
    int32_t        group_id,
    SB_MemberInfo* out_members,
    int32_t*       out_count
);

/**
 * Agrega un dispositivo a un grupo.
 * @return SB_OK | SB_ERR_GROUP_NOT_FOUND | SB_ERR_DEVICE_NOT_FOUND
 *       | SB_ERR_GROUP_LIMIT_EXCEEDED | SB_ERR_DEVICE_ALREADY_MEMBER
 */
SB_API SB_Result sb_add_device_to_group(int32_t group_id, const char* device_uid);

/**
 * Elimina un dispositivo de un grupo.
 * @return SB_OK | SB_ERR_GROUP_NOT_FOUND | SB_ERR_DEVICE_NOT_FOUND
 */
SB_API SB_Result sb_remove_device_from_group(int32_t group_id, const char* device_uid);

/**
 * Establece el volumen maestro del grupo.
 * Distribuye proporcionalmente: device_vol = master_volume * relative_volume.
 * Solo afecta dispositivos con is_active = true en el grupo.
 * @return SB_OK | SB_ERR_GROUP_NOT_FOUND | SB_ERR_INVALID_VOLUME
 */
SB_API SB_Result sb_set_group_volume(int32_t group_id, float master_volume);

/**
 * Establece el mute lógico del grupo.
 * NO modifica el hardware de los dispositivos miembros (ADR-03).
 * @return SB_OK | SB_ERR_GROUP_NOT_FOUND
 */
SB_API SB_Result sb_set_group_mute(int32_t group_id, bool muted);

/**
 * Renombra un grupo.
 * @return SB_OK | SB_ERR_GROUP_NOT_FOUND | SB_ERR_LABEL_TOO_LONG
 */
SB_API SB_Result sb_set_group_name(int32_t group_id, const char* name);

/**
 * Elimina un grupo y todas sus membresías (CASCADE en SQLite).
 * @return SB_OK | SB_ERR_GROUP_NOT_FOUND
 */
SB_API SB_Result sb_delete_group(int32_t group_id);

#ifdef __cplusplus
}
#endif
