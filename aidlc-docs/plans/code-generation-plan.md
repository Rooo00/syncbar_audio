# Code Generation Plan — SyncBar Audio

**Fase:** 6 — Code Generation Planning  
**Iteración:** 1 (Core C++ + Bridge + UI macOS)  
**Orden:** Dependency-first (Nivel 0 → Nivel 4)

---

## Resumen de Fases de Implementación

| Fase | Nombre | Archivos | Stories cubiertas | Estado |
|------|--------|----------|-------------------|--------|
| F1 | Foundation — CMake + SQLite | `CMakeLists.txt` x2, `sqlite3` amalgamation | US-04 (base) | ✅ |
| F2 | Database Manager | `database_mgr.h`, `database_mgr.cpp` | US-04, US-05 | ✅ |
| F3 | Audio Logic | `audio_engine.h` (parcial), AudioLogic | US-02, US-03 | ✅ |
| F4 | HAL — CoreAudio | `audio_engine.cpp` (`#ifdef __APPLE__`) | US-01, US-02, US-06 | ✅ |
| F5 | Core Engine — State + Groups | `audio_engine.cpp` (StateManager, GroupService) | US-03, US-05 | ✅ |
| F6 | Bridge API | `bridge_api.h`, `bridge_api.cpp` | US-07, US-08, US-09 | ✅ |
| F7 | macOS App Shell | `App.swift`, `Info.plist`, `AudioViewModel.swift` | US-10 | ✅ |
| F8 | macOS Device UI | `DeviceRowView.swift`, `PopoverView.swift` | US-11, US-12 | ✅ |
| F9 | macOS Group UI | `GroupRowView.swift`, `InlineLabelEditor.swift` | US-13 | ✅ |

---

## Fase F1 — Foundation: CMake + SQLite

**Objetivo:** Estructura de compilación funcional. El proyecto debe compilar (vacío) antes de escribir lógica.

### Archivos a generar:
- `sync-bar-audio/CMakeLists.txt` — Root CMake, configura el proyecto y agrega subdirectorios
- `sync-bar-audio/core/CMakeLists.txt` — Compila el Core como librería estática (`libsyncbar_core.a`)
- `sync-bar-audio/core/vendor/sqlite3.c` — SQLite amalgamation (single-file)
- `sync-bar-audio/core/vendor/sqlite3.h` — SQLite header

### Criterios de completitud:
- [ ] `cmake -B build && cmake --build build` ejecuta sin errores
- [ ] La librería estática `libsyncbar_core.a` se genera en `/build`
- [ ] SQLite3 compila como parte del Core sin dependencias externas

---

## Fase F2 — Database Manager

**Objetivo:** Capa de persistencia completa y testeada antes de tocar audio.

### Archivos a generar:
- `core/include/database_mgr.h` — Clase `DatabaseManager` con interfaz pública
- `core/src/database_mgr.cpp` — Implementación: init, schema migration, CRUD, debounce

### Funciones a implementar:
```
DatabaseManager::open(path)          → Abre/crea el archivo .db, aplica PRAGMAs
DatabaseManager::initSchema()        → CREATE TABLE IF NOT EXISTS (devices, virtual_groups, group_devices)
DatabaseManager::loadAll()           → Carga estado completo al iniciar
DatabaseManager::upsertDevice()      → INSERT OR REPLACE con debounce 300ms
DatabaseManager::upsertGroup()       → INSERT OR REPLACE en virtual_groups
DatabaseManager::upsertMembership()  → INSERT OR REPLACE en group_devices
DatabaseManager::deleteGroup()       → DELETE (CASCADE maneja group_devices)
DatabaseManager::setDeviceActive()   → UPDATE is_active en devices + group_devices
DatabaseManager::setDeviceLabel()    → UPDATE custom_name (inmediato, sin debounce)
DatabaseManager::close()             → Flush pendientes + sqlite3_close
```

### Criterios de completitud:
- [ ] Schema se crea correctamente en un archivo `.db` nuevo
- [ ] `PRAGMA foreign_keys = ON` verificado con test de integridad referencial
- [ ] Trigger `trg_max_devices_per_group` rechaza el 5to dispositivo
- [ ] Trigger `trg_update_group_active` actualiza `virtual_groups.is_active` correctamente
- [ ] Debounce de 300ms no escribe en cada llamada consecutiva dentro de la ventana
- [ ] `loadAll()` retorna el estado correcto tras reiniciar

---

## Fase F3 — Audio Logic

**Objetivo:** Funciones matemáticas puras de audio, sin dependencias de plataforma.

### Archivos a generar (dentro de `audio_engine.h/.cpp`):
- Namespace `AudioLogic` con funciones estáticas

### Funciones a implementar:
```cpp
namespace AudioLogic {
    float linearToDb(float linear);          // 20 * log10(linear), -96dB para 0
    float dbToLinear(float db);              // pow(10, db/20)
    float clampVolume(float v);              // Clamp a [0.0, 1.0]
    
    // Calcula el volumen efectivo de cada miembro al mover el slider maestro
    // Mantiene proporciones relativas entre miembros
    std::vector<float> distributeGroupVolume(
        float masterVolume,
        const std::vector<float>& relativeVolumes
    );
}
```

### Criterios de completitud:
- [ ] `linearToDb(0.0f)` retorna `-96.0f`
- [ ] `linearToDb(1.0f)` retorna `0.0f`
- [ ] `distributeGroupVolume(0.5, {1.0, 0.5})` retorna `{0.5, 0.25}`
- [ ] `clampVolume(-0.1f)` retorna `0.0f`, `clampVolume(1.1f)` retorna `1.0f`

---

## Fase F4 — HAL CoreAudio

**Objetivo:** Comunicación real con el hardware de audio en macOS.

### Archivos a generar (dentro de `audio_engine.cpp`, `#ifdef __APPLE__`):
- Clase interna `CoreAudioHAL`

### Funciones a implementar:
```cpp
// Dentro de #ifdef __APPLE__
class CoreAudioHAL {
    std::vector<AudioDeviceSnapshot> enumerateDevices();
    void setVolume(AudioDeviceID deviceId, float linearVolume);
    void setMute(AudioDeviceID deviceId, bool muted);
    std::string getPersistentUID(AudioDeviceID deviceId);
    void registerHotplugListener(
        std::function<void(const std::string& uid)> onConnect,
        std::function<void(const std::string& uid)> onDisconnect
    );
    void unregisterHotplugListener();
}
```

### APIs de CoreAudio a usar:
- `AudioObjectGetPropertyData` con `kAudioHardwarePropertyDevices` — enumeración
- `kAudioDevicePropertyDeviceUID` — UID persistente
- `kAudioHardwareServiceDeviceProperty_VirtualMasterVolume` — volumen
- `kAudioDevicePropertyMute` — mute
- `AudioObjectAddPropertyListener` con `kAudioHardwarePropertyDevices` — hotplug

### Criterios de completitud:
- [ ] `enumerateDevices()` retorna todos los dispositivos de salida activos
- [ ] El UID retornado es persistente entre reinicios del sistema
- [ ] `setVolume(uid, 0.5f)` cambia el volumen audiblemente en el hardware
- [ ] El callback de hotplug se invoca al conectar/desconectar un dispositivo Bluetooth
- [ ] No se instalan KEXTs ni se requieren permisos de administrador

---

## Fase F5 — Core Engine: State Manager + Group Service

**Objetivo:** Lógica de negocio completa del motor de audio.

### Archivos a generar:
- `core/include/audio_engine.h` — Clase `AudioEngine` completa
- `core/src/audio_engine.cpp` — Implementación de StateManager y VirtualGroupService

### Funciones a implementar:
```cpp
class AudioEngine {
public:
    // Ciclo de vida
    bool init(DatabaseManager& db);
    void shutdown();

    // Dispositivos
    std::vector<DeviceInfo> getDevices() const;
    SB_Result setDeviceVolume(const std::string& uid, float volume);
    SB_Result setDeviceMute(const std::string& uid, bool muted);
    SB_Result setDeviceLabel(const std::string& uid, const std::string& label);

    // Grupos
    SB_Result createGroup(const std::string& name, int32_t& outId);
    std::vector<GroupInfo> getGroups() const;
    std::vector<MemberInfo> getGroupMembers(int32_t groupId) const;
    SB_Result addDeviceToGroup(int32_t groupId, const std::string& uid);
    SB_Result removeDeviceFromGroup(int32_t groupId, const std::string& uid);
    SB_Result setGroupVolume(int32_t groupId, float masterVolume);
    SB_Result setGroupMute(int32_t groupId, bool muted);
    SB_Result setGroupName(int32_t groupId, const std::string& name);
    SB_Result deleteGroup(int32_t groupId);

    // Callbacks de hardware
    void registerCallbacks(
        std::function<void(const std::string&)> onConnect,
        std::function<void(const std::string&)> onDisconnect
    );

private:
    // Estado en memoria (fuente de verdad en runtime)
    std::unordered_map<std::string, DeviceState> devices_;
    std::unordered_map<int32_t, GroupState>      groups_;
    std::mutex                                    stateMutex_;
    DatabaseManager*                              db_;
    std::unique_ptr<CoreAudioHAL>                 hal_;  // o WASAPI
};
```

### Criterios de completitud:
- [ ] `init()` carga el estado desde SQLite antes de retornar
- [ ] `setGroupVolume(id, 0.5)` aplica volumen proporcional a cada miembro activo
- [ ] `addDeviceToGroup` retorna `SB_ERR_GROUP_LIMIT_EXCEEDED` al intentar agregar el 5to
- [ ] `handleDisconnect(uid)` marca `is_active=false` en memoria y persiste en SQLite
- [ ] `handleReconnect(uid)` restaura `is_active=true` y reactiva membresías
- [ ] Todas las operaciones de escritura son thread-safe (mutex)

---

## Fase F6 — Bridge API

**Objetivo:** C-API pública que expone el Core a las UIs.

### Archivos a generar:
- `core/include/bridge_api.h` — Structs, enums y declaraciones (ya diseñado en api-specification.md)
- `core/src/bridge_api.cpp` — Implementación: singleton de AudioEngine, validaciones, conversiones

### Patrón de implementación:
```cpp
// bridge_api.cpp
static AudioEngine* g_engine = nullptr;
static DatabaseManager* g_db = nullptr;

SB_Result sb_init(const char* db_path) {
    if (g_engine) return SB_OK; // Ya inicializado
    g_db = new DatabaseManager();
    if (!g_db->open(db_path)) return SB_ERR_DB_FAILURE;
    g_engine = new AudioEngine();
    if (!g_engine->init(*g_db)) return SB_ERR_DB_FAILURE;
    return SB_OK;
}
```

### Criterios de completitud:
- [ ] Todas las funciones de `api-specification.md` implementadas
- [ ] Validaciones de entrada: volumen en [0,1], label ≤ 64 chars, punteros no-null
- [ ] `sb_init` es idempotente (segunda llamada retorna SB_OK sin reinicializar)
- [ ] Los callbacks de hardware se propagan correctamente a la UI
- [ ] La librería compila como `.a` (macOS) y `.dll` (Windows) sin cambios de código

---

## Fase F7 — macOS App Shell

**Objetivo:** App funcional en la barra de menú, sin lógica de audio aún.

### Archivos a generar:
- `macos/AudioSyncPro/App.swift` — `@main`, `MenuBarExtra`, `LSUIElement`
- `macos/AudioSyncPro/Info.plist` — `LSUIElement = YES`, permisos de audio
- `macos/AudioSyncPro/CoreBridge/AudioViewModel.swift` — `@Observable`, llama al bridge, dispatch al main thread
- `macos/AudioSyncPro/CoreBridge/SyncBarBridge.h` — Bridging header que importa `bridge_api.h`

### Criterios de completitud:
- [ ] La app aparece en la barra de menú con un ícono SF Symbol
- [ ] No aparece en el Dock ni en el App Switcher (Cmd+Tab)
- [ ] Al hacer clic en el ícono aparece un pop-over vacío (placeholder)
- [ ] `sb_init()` se llama en el arranque y `sb_shutdown()` al cerrar
- [ ] El consumo de RAM en inactividad es ≤ 50 MB

---

## Fase F8 — macOS Device UI

**Objetivo:** Lista de dispositivos con sliders funcionales conectados al Core.

### Archivos a generar:
- `macos/AudioSyncPro/Views/PopoverView.swift` — Contenedor principal
- `macos/AudioSyncPro/Views/DeviceRowView.swift` — Fila: nombre, slider, botón mute
- `macos/AudioSyncPro/Views/InlineLabelEditor.swift` — Edición inline de nombre

### Criterios de completitud:
- [ ] El pop-over muestra todos los dispositivos activos detectados por el Core
- [ ] Mover el slider cambia el volumen del hardware en tiempo real (≤ 10ms)
- [ ] El botón de mute individual funciona y refleja el estado visualmente
- [ ] Los dispositivos inactivos aparecen atenuados con indicador visual
- [ ] Doble clic en el nombre activa el editor inline
- [ ] Enter guarda el nombre, Escape cancela sin guardar
- [ ] El nombre personalizado persiste tras reiniciar la app

---

## Fase F9 — macOS Group UI

**Objetivo:** Creación y control de grupos virtuales desde la UI.

### Archivos a generar:
- `macos/AudioSyncPro/Views/GroupRowView.swift` — Fila de grupo: nombre, slider maestro, mute lógico, lista de miembros
- `macos/AudioSyncPro/Views/GroupEditorView.swift` — Sheet para crear/editar grupo y seleccionar miembros

### Criterios de completitud:
- [ ] El usuario puede crear un grupo nuevo con nombre personalizado
- [ ] Puede agregar hasta 4 dispositivos al grupo (el 5to muestra error)
- [ ] El slider maestro del grupo ajusta el volumen de todos los miembros activos proporcionalmente
- [ ] El botón de mute del grupo es lógico (no silencia el hardware)
- [ ] Los miembros inactivos del grupo se muestran atenuados
- [ ] Eliminar un grupo elimina sus membresías (CASCADE)
- [ ] La configuración del grupo persiste tras reiniciar

---

## Checklist Global de la Iteración 1

- [ ] F1 — CMake compila sin errores
- [ ] F2 — DatabaseManager: schema, CRUD, debounce, triggers
- [ ] F3 — AudioLogic: funciones matemáticas verificadas
- [ ] F4 — HAL CoreAudio: enumeración, volumen, mute, hotplug
- [ ] F5 — AudioEngine: state, grupos, thread safety
- [ ] F6 — BridgeAPI: todas las funciones del api-specification.md
- [ ] F7 — App Shell: MenuBar, sin Dock, sb_init/shutdown
- [ ] F8 — Device UI: sliders, mute, inline editor
- [ ] F9 — Group UI: crear, editar, slider maestro, mute lógico
