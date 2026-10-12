# SyncBar Audio — Arquitectura actual (as-built)

**Fecha:** Octubre 2026
**Alcance:** describe cómo está construida la app **hoy**, según el código del repositorio (no según el diseño previsto). Para las decisiones de diseño ver `architectural-decisions.md`; para el ruteo de audio propuesto ver ADR-07.

---

## 1. Resumen

SyncBar Audio es una utilidad de barra de menú para macOS que lista los dispositivos de salida de audio, permite renombrarlos, ajustar su volumen/mute y agruparlos en "grupos virtuales" con un volumen maestro.

La app está dividida en tres capas:

```
┌──────────────────────────────────────────────────────────────┐
│ UI macOS — SwiftUI (MenuBarExtra)                            │
│   App.swift → PopoverView → DeviceRowView / GroupRowView     │
│   AudioViewModel (ObservableObject)                          │
├──────────────── bridging header (SyncBarBridge.h) ───────────┤
│ Bridge — C-API (bridge_api.h / .cpp)                         │
│   Funciones sb_*  ·  structs POD  ·  estado global singleton │
├──────────────────────────────────────────────────────────────┤
│ Core C++17 (libsyncbar_core.a)                               │
│   AudioEngine ── estado en memoria + HAL CoreAudio           │
│   AudioLogic  ── matemáticas de volumen (header-only)        │
│   DatabaseManager ── persistencia SQLite + debounce          │
└──────────────────────────────────────────────────────────────┘
          │                                   │
     CoreAudio HAL                     SQLite (~/Library/Application
   (AudioObject* APIs)                  Support/SyncBarAudio/syncbar.db)
```

**Importante:** la app es solo un **plano de control**. Ajusta propiedades de los dispositivos (volumen, mute) pero no ruta ni mezcla audio. Un grupo no hace que varios dispositivos suenen a la vez (ver §7 y ADR-07).

---

## 2. Stack tecnológico

| Capa | Tecnología | Versión / detalle |
|---|---|---|
| UI macOS | Swift + SwiftUI | Swift 5.9, `MenuBarExtra` con `.menuBarExtraStyle(.window)`, `ObservableObject`/`@Published` |
| Destino | macOS | 13.0+ (`MACOSX_DEPLOYMENT_TARGET`, `LSMinimumSystemVersion`) |
| Interop | Bridging header Objective-C | `SyncBarBridge.h` importa `bridge_api.h`; Swift llama a C directamente |
| Core | C++17 | Sin extensiones de compilador; `-Wall -Wextra -Wpedantic -Werror` (`/W4 /WX` en MSVC) |
| Audio (macOS) | CoreAudio + AudioToolbox | `AudioObjectGet/SetPropertyData`, `AudioObjectAddPropertyListener` |
| Audio (Windows) | WASAPI | **Solo stub**; las funciones HAL devuelven `false` |
| Persistencia | SQLite 3.49.1 (amalgamation en `core/vendor/`) | WAL, `foreign_keys=ON`, `synchronous=NORMAL`, triggers |
| Build | CMake 3.21+ (core) + Xcode (app) | Core: estática en macOS, DLL en Windows. Opción `-DUNIVERSAL=ON` para arm64+x86_64 |
| Tests | Ejecutables C++ propios (sin framework) | 4 binarios registrados en CTest; solo se compilan en `Debug` |
| CI | GitHub Actions (`.github/workflows/ci.yml`) | macOS 14: build Debug + `ctest`; build Release del core + `xcodebuild` de la app sin firma |

---

## 3. Estructura del repositorio

```
CMakeLists.txt                  Proyecto raíz: C++17, flags, valida APPLE/WIN32
core/
  CMakeLists.txt                Targets: sqlite3, syncbar_core, tests (Debug)
  include/
    audio_engine.h              Clase AudioEngine + DeviceInfo/GroupInfo/MemberInfo
    audio_logic.h               Namespace AudioLogic (header-only)
    bridge_api.h                C-API pública (SB_*)
    database_mgr.h              Clase DatabaseManager + *Record
  src/
    audio_engine.cpp            HAL CoreAudio + estado + grupos + hotplug (867 líneas)
    bridge_api.cpp              Implementación de la C-API
    database_mgr.cpp            Schema, CRUD, triggers, debounce
  tests/                        test_audio_logic / test_audio_engine / test_database_mgr / test_bridge_api
  vendor/sqlite3.{c,h}
macos/
  SyncBarAudio/                 Proyecto Xcode
aidlc-docs/                     Requisitos, historias, diseño, ADRs
.gitignore                      Excluye build/, DerivedData, .DS_Store, *.db, etc.
```

---

## 4. Core C++

### 4.1 AudioLogic (`audio_logic.h`)

Funciones puras, sin estado ni dependencias de plataforma:

- `linearToDb` / `dbToLinear` — conversión con piso de silencio en −96 dB.
- `clampVolume` — acota a [0, 1].
- `distributeGroupVolume(master, relativos)` — volumen efectivo = `master × relativo` por miembro.
- `computeRelativeVolume` — inverso del anterior.
- `applyVolumeCurve` / `volumeToSlider` — curva perceptual (exponente 2).
- `kMaxGroupMembers = 4`.

`computeRelativeVolume`, `applyVolumeCurve` y `volumeToSlider` solo se usan en tests; ni el engine ni la UI las llaman (los sliders son lineales).

### 4.2 AudioEngine (`audio_engine.h/.cpp`)

Patrón **pImpl** (`AudioEngine::Impl`). Contiene:

- **Estado en memoria** (fuente de verdad en runtime):
  `unordered_map<uid, DeviceState>` y `unordered_map<id, GroupState>`; cada grupo guarda un `vector<MemberState>`.
  Todo protegido por un único `std::mutex stateMutex`.
- **HAL CoreAudio** (namespace `CoreAudioHAL`, bajo `#ifdef __APPLE__`):
  - `enumerateOutputDevices` — `kAudioHardwarePropertyDevices` filtrado por `kAudioDevicePropertyStreamConfiguration` en scope de salida.
  - `getUID` / `getName` — `kAudioDevicePropertyDeviceUID` / `DeviceNameCFString`. El UID es la clave persistente.
  - `get/setVolume` — `kAudioHardwareServiceDeviceProperty_VirtualMainVolume`; si no está disponible, `kAudioDevicePropertyVolumeScalar` por canal (main, 1, 2). Ambas revisan el `OSStatus`.
  - `hasVolumeControl` — indica si el dispositivo permite cambiar su volumen (HDMI/DisplayPort suelen no permitirlo); se expone como `has_volume_control`.
  - `get/setMute` — `kAudioDevicePropertyMute`.
- **Hotplug**: un listener sobre `kAudioHardwarePropertyDevices` en `kAudioObjectSystemObject`. En cada cambio vuelve a enumerar y, con `stateMutex` tomado, compara contra el mapa en memoria, marca altas/bajas/reconexiones y actualiza membresías y SQLite. Los callbacks `onConnect`/`onDisconnect` se invocan **después** de soltar el lock y solo para dispositivos nuevos, reconectados o desconectados.
- **Operaciones** (devuelven `AudioEngine::Result`): dispositivos (`setDeviceVolume/Mute/Label`) y grupos (`create`, `addDevice`, `removeDevice`, `setGroupVolume`, `setGroupMute`, `setGroupName`, `delete`).

**Secuencia de `init()`:**
1. Carga `devices`, `virtual_groups` y `group_devices` desde SQLite (todo inactivo).
2. Enumera el hardware, cruza por UID, toma volumen/mute reales del hardware y da de alta los dispositivos nuevos.
3. Recalcula `is_active` de miembros y grupos.
4. Registra el listener de hotplug.

**Semántica de grupos:**
- `setGroupVolume` calcula `master × relativo` y lo escribe en el hardware de cada miembro activo (si el grupo no está muteado).
- `setGroupMute` es **lógico** (ADR-03): solo bloquea la propagación de volumen; no toca el mute del hardware.
- Al añadir un miembro, su `relative_volume` se fija en 1.0. **No existe operación para cambiarlo**, así que en la práctica todos los miembros reciben el volumen maestro.

### 4.3 DatabaseManager (`database_mgr.h/.cpp`)

pImpl sobre `sqlite3*`, con un wrapper RAII `Stmt` para sentencias preparadas.

**Schema:**

| Tabla | Clave | Columnas |
|---|---|---|
| `devices` | `device_uid` | `custom_name`, `saved_volume`, `is_muted`, `is_active` |
| `virtual_groups` | `id` (autoincrement) | `name`, `master_volume`, `is_muted`, `is_active` |
| `group_devices` | (`group_id`, `device_uid`) | `relative_volume`, `is_active` — FKs con `ON DELETE CASCADE` |

Triggers:
- `trg_max_devices_per_group` — aborta con `GROUP_DEVICE_LIMIT_EXCEEDED` al insertar un quinto miembro.
- `trg_update_group_active` — recalcula `virtual_groups.is_active` al cambiar `is_active` de un miembro.

**Debounce de volumen (300 ms):** `upsertDevice(rec, immediate=false)` guarda el registro en un mapa `pendingDevices` con su timestamp y despierta a un hilo propio de `DatabaseManager`. Ese hilo escribe cada registro cuando pasan 300 ms sin cambios; se inicia en `open()` y se detiene en `close()`, que además hace `flushPending()`.

### 4.4 Bridge C-API (`bridge_api.h/.cpp`)

- Interfaz plana en C (`extern "C"`) exportada con `SB_API` (`visibility("default")` / `__declspec(dllexport)`), pensada para Swift (bridging header) y C# (P/Invoke).
- **Estado global singleton:** `g_engine`, `g_db`, `g_initMutex` y los punteros a callbacks C.
- Tipos POD con buffers de tamaño fijo (`SB_DeviceInfo`, `SB_GroupInfo`, `SB_MemberInfo`). Las cadenas se copian con `safeCopy` (truncado seguro).
- Códigos `SB_Result` mapeados 1:1 desde `AudioEngine::Result`.
- `sb_init(db_path)` crea el `DatabaseManager` y el `AudioEngine`, registra los callbacks e inicializa. Si `db_path` es nulo usa una ruta por defecto por plataforma.
- `sb_shutdown()` hace flush y libera.

---

## 5. App macOS (SwiftUI)

| Archivo | Rol |
|---|---|
| `App.swift` | `@main`. Un `MenuBarExtra` cuyo ícono (`speaker.wave.2[.fill]`) cambia si hay un grupo activo sin mute. Inyecta el ViewModel como `environmentObject`. |
| `Info.plist` | `LSUIElement=YES` (sin Dock ni Cmd+Tab), bundle `com.syncbar.audio`, sin permisos de micrófono (no se necesitan para controlar salidas). |
| `CoreBridge/SyncBarBridge.h` | Bridging header → `bridge_api.h`. |
| `CoreBridge/AudioViewModel.swift` | Único ViewModel (`ObservableObject`). Publica `devices`, `groups` e `isReady`. Llama `sb_*` de forma síncrona en el hilo principal y actualiza el estado de forma optimista si devuelve `SB_OK`. Convierte los buffers C a `String` con `cStr`. |
| `CoreBridge/AudioViewModel+Preview.swift` | Datos simulados para Xcode Previews (`init(skipBridgeInit:)`). |
| `Views/PopoverView.swift` | Contenedor: secciones de dispositivos y grupos. |
| `Views/DeviceRowView.swift` | Fila: nombre editable (doble clic), slider de volumen lineal, botón mute. Se deshabilita si está inactivo o muteado. |
| `Views/GroupRowView.swift` | Slider maestro, mute lógico, `sheet` para editar miembros (`GroupEditorSheet`). |
| `Views/InlineLabelEditor.swift` | `TextField` reutilizable con confirmación al perder el foco. |

**Volumen no aplicable:** si `has_volume_control` es `false`, `DeviceRowView` deshabilita el slider y muestra "Sin control de volumen". Si el hardware rechaza un cambio, `sb_set_device_volume` devuelve `SB_ERR_HAL_FAILURE` y el ViewModel no actualiza su valor (el slider vuelve a su posición).

**Callbacks de hardware:** C no puede capturar contexto Swift, así que el ViewModel se guarda en una variable global `weak var _sharedViewModel`. Los closures C hacen `DispatchQueue.main.async` hacia el ViewModel. En una conexión se vuelve a leer toda la lista de dispositivos; en una desconexión se actualiza el estado local.

**Enlazado (Xcode):** `libsyncbar_core.a` desde `../../build/lib`, más `CoreAudio.framework` y `AudioToolbox.framework`. También `-lc++`. SQLite se compila como librería `OBJECT` y sus objetos van dentro de `libsyncbar_core.a`, así que la app usa la versión 3.49.1 de `vendor/` y no la del sistema.

---

## 6. Flujos principales

**Arranque**
`SyncBarAudioApp` → `AudioViewModel.init` → `sb_register_device_callbacks` → `sb_init(~/Library/Application Support/SyncBarAudio/syncbar.db)` → `DatabaseManager.open` (PRAGMAs + schema) → `AudioEngine.init` (carga SQLite, enumera HAL, registra el listener) → `refreshAll()` (`sb_get_devices`, `sb_get_groups`, `sb_get_group_members`).

**Mover el slider de un dispositivo**
`Slider.set` → `vm.setDeviceVolume` → `sb_set_device_volume` → `AudioEngine::setDeviceVolume` (lock) → `CoreAudioHAL::setVolume` → `db.upsertDevice(immediate=false)` (debounce) → la UI actualiza su copia local.

**Mover el slider maestro de un grupo**
`sb_set_group_volume` → `distributeGroupVolume` → `setVolume` en el HAL de cada miembro activo → upsert con debounce de cada dispositivo.

**Conectar o desconectar un dispositivo**
CoreAudio (hilo del HAL) → `hotplugListenerProc` → reenumera, actualiza el estado y SQLite bajo el lock → callback C → `DispatchQueue.main.async` → el ViewModel refresca.

---

## 7. Observaciones sobre el estado actual

Hallazgos de la lectura del código, sin cambios aplicados:

1. **No hay ruteo de audio.** Los grupos solo reparten volumen; no generan salida simultánea. Ver ADR-07.
2. **Volumen relativo siempre en 1.0.** No hay API (`sb_set_member_volume` o similar) para ajustarlo, así que `distributeGroupVolume` siempre devuelve el volumen maestro.
3. *(Resuelto en octubre 2026)* ~~El debounce no tiene temporizador.~~ Ahora un hilo escribe los pendientes a los 300 ms.
4. *(Resuelto en octubre 2026, solo volumen)* ~~Errores del HAL ignorados.~~ El volumen revisa el `OSStatus`, usa `VolumeScalar` como respaldo y la UI marca los dispositivos sin control de volumen. Pendiente: `setMute` sigue sin informar rechazos del hardware, y el camino "rechazado" no tiene test hasta que exista el mock del HAL (Paso 2).
5. *(Resuelto en octubre 2026)* ~~El listener de hotplug avisaba de todos los dispositivos y con el lock tomado.~~ Ahora solo avisa de cambios reales, fuera del lock, y una reconexión también reactiva las membresías de grupo.
6. **Estado global** en el bridge y en Swift (`g_engine`, `_sharedViewModel`): solo admite una instancia y complica los tests.
7. **Un solo archivo concentra demasiado.** `audio_engine.cpp` mezcla HAL, estado, grupos y persistencia; no hay una interfaz de HAL que permita usar un mock.
8. *(Resuelto en octubre 2026)* ~~SQLite de `vendor/` no usado por la app.~~ Ahora va dentro de `libsyncbar_core.a`.
9. **Windows es solo un stub.** `ole32`/`mmdevapi` se enlazan, pero no hay implementación ni carpeta `windows/`.
10. **Infraestructura de calidad.** *(Resuelto en octubre 2026: `.gitignore`, se eliminaron `build/`, `.DS_Store` y `AudioSyncPro/`; tests en CTest; CI en GitHub Actions.)* Pendiente: los tests dependían de includes transitivos de libc++ (`<string>`, `<atomic>`), ya corregidos; el código no compila en plataformas distintas de macOS/Windows por `-Werror` en los stubs.
11. *(Resuelto en octubre 2026)* ~~Permiso de micrófono innecesario.~~ Eliminado de `Info.plist`.
