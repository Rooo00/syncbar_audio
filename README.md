# SyncBar Audio

Utilidad de sistema multiplataforma (hoy solo macOS; Windows planeado para la Iteración 2) para la gestión avanzada de dispositivos de audio y la creación de grupos de salida virtuales de baja latencia.

Vive exclusivamente en la barra de menú (macOS) o bandeja del sistema (Windows). Sin ventanas, sin Dock, sin complejidad.

---

## Casos de uso

- Conectar bocinas de escritorio y AirPods al mismo tiempo y controlar el volumen de ambos con un slider
- Agrupar dos bocinas Bluetooth para ambientar un cuarto y controlarlas como una sola
- Renombrar dispositivos con nombres propios ("Bocina Izquierda", "Mis AirPods")

---

## Arquitectura

```
┌─────────────────────────────────────────────────────┐
│  UI Nativa                                          │
│  macOS: SwiftUI + MenuBarExtra                      │
│  Windows: WinUI 3 + TrayIcon (Iteración 2)          │
├─────────────────────────────────────────────────────┤
│  Bridge / FFI                                       │
│  C-API (bridge_api.h) — Swift via bridging header   │
│                       — C# via P/Invoke             │
├─────────────────────────────────────────────────────┤
│  Core Engine (C++17)                                │
│  AudioEngine · AudioLogic · DatabaseManager         │
│  HAL: CoreAudio (macOS) / WASAPI (Windows)          │
└─────────────────────────────────────────────────────┘
```

**Restricciones de diseño:**
- RAM en inactividad: ≤ 50 MB
- Latencia de grupos virtuales: ≤ 10 ms
- Sin KEXTs, sin modificaciones de kernel
- Compatible con macOS 13+ (Apple Silicon y x86) y Windows 11 (64-bit)

---

## Estructura del proyecto

```
syncbar_audio/
├── CMakeLists.txt              # Build del Core
├── core/                       # Núcleo compartido C++17
│   ├── include/
│   │   ├── audio_engine.h      # Motor principal + tipos de runtime
│   │   ├── audio_logic.h       # Matemáticas de audio (header-only)
│   │   ├── bridge_api.h        # C-API pública (FFI)
│   │   └── database_mgr.h      # Persistencia SQLite
│   ├── src/
│   │   ├── audio_engine.cpp    # HAL CoreAudio/WASAPI + StateManager + Groups
│   │   ├── bridge_api.cpp      # Implementación de la C-API
│   │   └── database_mgr.cpp    # CRUD SQLite + debounce
│   ├── tests/                  # 4 ejecutables de prueba (solo en Debug)
│   └── vendor/sqlite3.*        # SQLite 3.49.1 amalgamation
├── macos/
│   └── SyncBarAudio/           # Proyecto Xcode
│       └── SyncBarAudio/
│           ├── App.swift       # @main, MenuBarExtra, LSUIElement
│           ├── CoreBridge/
│           │   ├── SyncBarBridge.h         # Bridging header
│           │   └── AudioViewModel.swift    # ObservableObject, llama al bridge
│           └── Views/
│               ├── PopoverView.swift       # Contenedor principal
│               ├── DeviceRowView.swift     # Slider + mute + inline editor
│               ├── GroupRowView.swift      # Slider maestro + GroupEditorSheet
│               └── InlineLabelEditor.swift # Editor inline reutilizable
└── aidlc-docs/                 # Documentación AI-SDLC
```

---

## Compilar el Core

**Requisitos:** CMake 3.21+, Xcode Command Line Tools (macOS) o MSVC (Windows)

> La carpeta `build/` no se versiona (ver `.gitignore`); se genera localmente con los comandos siguientes.

```bash
# Configurar (Debug)
cmake -B build -DCMAKE_BUILD_TYPE=Debug

# Compilar
cmake --build build

# Ejecutar todos los tests (los ejecutables solo se generan en Debug)
./build/core/test_database_mgr
./build/core/test_audio_logic
./build/core/test_audio_engine
./build/core/test_bridge_api
```

**Artefacto generado:** `build/lib/libsyncbar_core.a`

---

## Compilar la app macOS

**Requisitos:** Xcode 15+, macOS 13+

```bash
# Desde la raíz del proyecto — compilar el Core primero
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build

# Luego abrir en Xcode
open macos/SyncBarAudio/SyncBarAudio.xcodeproj
```

O desde línea de comandos:

```bash
xcodebuild -project macos/SyncBarAudio/SyncBarAudio.xcodeproj \
  -scheme SyncBarAudio \
  -configuration Release \
  -destination "platform=macOS" \
  build
```

---

## Base de datos

SQLite en `~/Library/Application Support/SyncBarAudio/syncbar.db`

```sql
-- Tres tablas relacionales
devices        -- Configuración de hardware detectado
virtual_groups -- Grupos creados por el usuario
group_devices  -- Relación muchos-a-muchos (máx 4 por grupo)
```

Características:
- `PRAGMA foreign_keys = ON` + `ON DELETE CASCADE`
- Trigger que limita a 4 dispositivos por grupo
- Trigger que recalcula `is_active` del grupo al conectar/desconectar miembros
- Debounce de 300 ms para escrituras de volumen (evita escrituras excesivas al arrastrar sliders)

---

## C-API del Bridge

Funciones principales expuestas en `core/include/bridge_api.h`:

```c
// Ciclo de vida
sb_init(db_path)       // Inicializa motor + carga SQLite + registra HAL
sb_shutdown()          // Flush pendientes + libera recursos
sb_register_device_callbacks(onConnect, onDisconnect)

// Dispositivos
sb_get_devices(buf, maxCount, outCount)
sb_set_device_volume(uid, volume)   // [0.0 – 1.0]
sb_set_device_mute(uid, muted)
sb_set_device_label(uid, label)     // máx 64 chars UTF-8

// Grupos virtuales
sb_create_group(name, outId)
sb_get_groups(buf, maxCount, outCount)
sb_get_group_members(groupId, buf, outCount)
sb_add_device_to_group(groupId, uid)
sb_remove_device_from_group(groupId, uid)
sb_set_group_volume(groupId, masterVolume)
sb_set_group_mute(groupId, muted)   // lógico — no toca hardware
sb_set_group_name(groupId, name)
sb_delete_group(groupId)
```

---

## Decisiones de diseño clave

| ADR | Decisión |
|-----|----------|
| ADR-01 | Core C++ + UIs nativas independientes (sin Electron/Qt) |
| ADR-02 | SQLite con debounce 300 ms para escrituras de slider |
| ADR-03 | Mute de grupo lógico — no propaga al hardware |
| ADR-04 | Límite fijo de 4 dispositivos por grupo |
| ADR-05 | Dispositivo inactivo al desconectarse, no eliminado del grupo |
| ADR-06 | macOS primero, Windows en Iteración 2 |

| ADR-07 | *(Propuesto)* Ruteo de audio de grupos vía Aggregate Device multi-salida |

Documentación completa en `aidlc-docs/design-artifacts/architectural-decisions.md`.
Arquitectura tal como está construida hoy: `aidlc-docs/design-artifacts/current-architecture.md`.

---

## Tests

```
Core Engine — 218 tests (cifra reportada al generar el código; no hay CI que la verifique)
├── test_database_mgr   44 tests  Schema, CRUD, triggers, debounce, persist/reload
├── test_audio_logic    64 tests  dB, lineal, curvas, distribución de grupos
├── test_audio_engine   43 tests  Init, grupos, volumen, mute, thread safety
└── test_bridge_api     67 tests  C-API completa, guards, idempotencia, callbacks
```

---

## Roadmap

**Iteración 1 — Completada**
- [x] Core C++ (HAL CoreAudio, grupos virtuales, SQLite, Bridge API)
- [x] macOS UI (MenuBarExtra, sliders, mute, inline editor, grupos)

**Iteración 2 — Pendiente**
- [ ] UI Windows (WinUI 3, TrayIcon, P/Invoke bridge)
- [ ] HAL WASAPI (ya preparado con `#ifdef _WIN32` en audio_engine.cpp)
- [ ] Instalador macOS (.dmg) y Windows (.msix)
