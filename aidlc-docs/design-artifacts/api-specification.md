# C-API Specification — SyncBar Audio Bridge (FFI)

**Archivo header:** `core/include/bridge_api.h`  
**Implementación:** `core/src/bridge_api.cpp`  
**Consumidores:**
- macOS: Swift vía `.bridging-header`
- Windows: C# vía P/Invoke (`[DllImport]`)

**Convención:** C linkage (`extern "C"`), tipos primitivos únicos, sin excepciones C++.

---

## Tipos y Estructuras

```c
// bridge_api.h

#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ─── Códigos de resultado ─────────────────────────────────────────────── */

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
} SB_Result;

/* ─── Snapshot de dispositivo (lectura) ────────────────────────────────── */

typedef struct SB_DeviceInfo {
    char    uid[256];           // UID persistente del hardware
    char    display_name[128];  // custom_name si existe, sino native_name
    float   volume;             // [0.0 – 1.0]
    bool    is_muted;
    bool    is_active;          // false = desconectado físicamente
} SB_DeviceInfo;

/* ─── Snapshot de grupo (lectura) ──────────────────────────────────────── */

typedef struct SB_GroupInfo {
    int32_t id;
    char    name[128];
    float   master_volume;      // [0.0 – 1.0]
    bool    is_muted;           // Mute lógico (no propaga al hardware)
    bool    is_active;          // false si todos los miembros están inactivos
    int32_t member_count;       // Número de miembros (0–4)
} SB_GroupInfo;

/* ─── Snapshot de miembro de grupo ────────────────────────────────────── */

typedef struct SB_MemberInfo {
    char    device_uid[256];
    float   relative_volume;    // Balance relativo dentro del grupo [0.0 – 1.0]
    bool    is_active;
} SB_MemberInfo;

/* ─── Callbacks de eventos de hardware ────────────────────────────────── */

typedef void (*SB_DeviceConnectedCallback)(const char* uid);
typedef void (*SB_DeviceDisconnectedCallback)(const char* uid);
```

---

## API — Ciclo de Vida del Motor

```c
/* Inicializa el motor de audio, carga estado desde SQLite y registra callbacks HAL.
 * Debe llamarse una vez al iniciar la app, antes de cualquier otra función.
 * db_path: ruta absoluta al archivo .db (UTF-8)
 * Returns: SB_OK o SB_ERR_DB_FAILURE */
SB_Result sb_init(const char* db_path);

/* Registra callbacks para eventos de conexión/desconexión de hardware.
 * Los callbacks se invocan desde el hilo del HAL; la UI debe hacer dispatch al hilo principal. */
void sb_register_device_callbacks(
    SB_DeviceConnectedCallback    on_connected,
    SB_DeviceDisconnectedCallback on_disconnected
);

/* Libera todos los recursos del motor. Llamar al cerrar la app. */
void sb_shutdown(void);
```

---

## API — Dispositivos

```c
/* Retorna la lista de todos los dispositivos conocidos (activos e inactivos).
 * out_devices: buffer pre-allocado por el caller
 * in_max_count: tamaño del buffer
 * out_count: número de dispositivos escritos
 * Returns: SB_OK o SB_ERR_NOT_INITIALIZED */
SB_Result sb_get_devices(
    SB_DeviceInfo* out_devices,
    int32_t        in_max_count,
    int32_t*       out_count
);

/* Establece el volumen de un dispositivo.
 * volume: [0.0 – 1.0]. El núcleo convierte a dB para el HAL.
 * Returns: SB_OK, SB_ERR_DEVICE_NOT_FOUND, SB_ERR_INVALID_VOLUME */
SB_Result sb_set_device_volume(const char* uid, float volume);

/* Establece el estado de mute de un dispositivo individual.
 * Returns: SB_OK, SB_ERR_DEVICE_NOT_FOUND */
SB_Result sb_set_device_mute(const char* uid, bool muted);

/* Establece el nombre personalizado de un dispositivo.
 * label: UTF-8, máximo 64 caracteres. NULL para limpiar el label.
 * Returns: SB_OK, SB_ERR_DEVICE_NOT_FOUND, SB_ERR_LABEL_TOO_LONG */
SB_Result sb_set_device_label(const char* uid, const char* label);
```

---

## API — Grupos Virtuales

```c
/* Crea un nuevo grupo virtual con el nombre dado.
 * out_group_id: ID asignado al nuevo grupo
 * Returns: SB_OK, SB_ERR_DB_FAILURE */
SB_Result sb_create_group(const char* name, int32_t* out_group_id);

/* Retorna la lista de todos los grupos.
 * out_groups: buffer pre-allocado por el caller
 * Returns: SB_OK o SB_ERR_NOT_INITIALIZED */
SB_Result sb_get_groups(
    SB_GroupInfo* out_groups,
    int32_t       in_max_count,
    int32_t*      out_count
);

/* Retorna los miembros de un grupo específico.
 * out_members: buffer de tamaño mínimo 4 (límite fijo)
 * Returns: SB_OK, SB_ERR_GROUP_NOT_FOUND */
SB_Result sb_get_group_members(
    int32_t       group_id,
    SB_MemberInfo* out_members,
    int32_t*      out_count
);

/* Agrega un dispositivo a un grupo.
 * Returns: SB_OK, SB_ERR_GROUP_NOT_FOUND, SB_ERR_DEVICE_NOT_FOUND,
 *          SB_ERR_GROUP_LIMIT_EXCEEDED, SB_ERR_DEVICE_ALREADY_MEMBER */
SB_Result sb_add_device_to_group(int32_t group_id, const char* device_uid);

/* Elimina un dispositivo de un grupo.
 * Returns: SB_OK, SB_ERR_GROUP_NOT_FOUND, SB_ERR_DEVICE_NOT_FOUND */
SB_Result sb_remove_device_from_group(int32_t group_id, const char* device_uid);

/* Establece el volumen maestro del grupo.
 * Distribuye proporcionalmente: device_vol = master_volume * relative_volume
 * Solo afecta dispositivos con is_active = true en el grupo.
 * Returns: SB_OK, SB_ERR_GROUP_NOT_FOUND, SB_ERR_INVALID_VOLUME */
SB_Result sb_set_group_volume(int32_t group_id, float master_volume);

/* Establece el mute lógico del grupo (NO modifica el hardware de los miembros).
 * Returns: SB_OK, SB_ERR_GROUP_NOT_FOUND */
SB_Result sb_set_group_mute(int32_t group_id, bool muted);

/* Renombra un grupo.
 * Returns: SB_OK, SB_ERR_GROUP_NOT_FOUND, SB_ERR_LABEL_TOO_LONG */
SB_Result sb_set_group_name(int32_t group_id, const char* name);

/* Elimina un grupo y todas sus membresías (CASCADE en SQLite).
 * Returns: SB_OK, SB_ERR_GROUP_NOT_FOUND */
SB_Result sb_delete_group(int32_t group_id);
```

---

## Notas de Integración

### macOS (Swift)

```swift
// SyncBarAudio/CoreBridge/SyncBarBridge.h  (bridging header)
#import "bridge_api.h"

// Uso en Swift:
var devices = [SB_DeviceInfo](repeating: SB_DeviceInfo(), count: 32)
var count: Int32 = 0
sb_get_devices(&devices, 32, &count)
```

### Windows (C#)

```csharp
// CoreBridge/SyncBarBridge.cs
using System.Runtime.InteropServices;

public static class SyncBarBridge {
    private const string DLL = "syncbar_core.dll";

    [DllImport(DLL)] public static extern SBResult sb_init(string dbPath);
    [DllImport(DLL)] public static extern SBResult sb_set_device_volume(string uid, float volume);
    [DllImport(DLL)] public static extern SBResult sb_set_group_volume(int groupId, float volume);
    // ... resto de funciones
}
```

---

## Garantías de Thread Safety

| Función | Thread-safe | Notas |
|---------|-------------|-------|
| `sb_init` / `sb_shutdown` | No | Solo desde hilo principal, una vez |
| `sb_get_devices` / `sb_get_groups` | Sí | Lectura con mutex interno |
| `sb_set_device_volume` | Sí | Escritura atómica al HAL |
| `sb_set_group_volume` | Sí | Mutex cubre todos los miembros |
| Callbacks HAL | — | Se invocan desde hilo del HAL; hacer dispatch a main thread en la UI |

---

## Fórmula de Volumen en Grupos

```
// Conversión lineal → dB para el HAL
float linear_to_db(float linear) {
    if (linear <= 0.0f) return -96.0f;  // Silencio absoluto
    return 20.0f * log10f(linear);
}

// Volumen efectivo de cada miembro al mover el slider maestro
float effective_volume = master_volume * member.relative_volume;
float db_value = linear_to_db(effective_volume);
// db_value se pasa al HAL (CoreAudio/WASAPI)
```
