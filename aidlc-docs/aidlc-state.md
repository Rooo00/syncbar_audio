# AI-SDLC State — SyncBar Audio

**Proyecto:** SyncBar Audio
**Versión SRS:** 1.0 (Mayo 2026)
**Arquitectura:** Monolith path (Core C++ compartido + UIs nativas)
**Última actualización:** Octubre 2026

---

## Estado Actual

| Fase | Nombre | Estado |
|------|--------|--------|
| 1 | Requirements Assessment | ✅ Completada (SRS ISO 29148) |
| 2 | Story Planning | ✅ Completada |
| 3 | Story Development | ✅ Completada |
| 4 | Architecture Decision | ✅ Completada (definida en SRS) |
| 5 | Design | ✅ Completada |
| 6 | Code Generation Planning | ✅ Completada |
| 7 | Iterative Code Generation | ✅ Completada (F1–F9, Iteración 1) |

---

## Decisiones Tomadas

- **Arquitectura:** 3 capas — UI Nativa / Bridge FFI / Core C++
- **macOS UI:** SwiftUI con MenuBarExtra + ObservableObject (macOS 13+)
- **Windows UI:** WinUI 3 con C#/.NET y TrayIcon (Iteración 2)
- **Persistencia:** SQLite con PRAGMA foreign_keys y ON DELETE CASCADE
- **HAL:** Bifurcación por compilación condicional (#ifdef _WIN32 / #ifdef __APPLE__)
- **Restricción de memoria:** UI ≤ 50 MB RAM en inactividad
- **Restricción de latencia:** ≤ 10 ms en grupos virtuales
- **Límite de dispositivos por grupo:** 4 (fijo)
- **Mute de grupo:** Lógico a nivel de software (no propaga al hardware)
- **Desconexión de dispositivo:** Queda como inactivo (is_active = false), se reactiva al reconectarse
- **Personas:** Melómanos casuales y usuarios del día a día (Andrés y Sofía)
- **Nombre del proyecto macOS:** SyncBarAudio (renombrado de AudioSyncPro; la copia antigua se eliminó en la limpieza de octubre 2026)
- **Ruteo de audio de grupos:** ADR-07 propuesto (Aggregate Device multi-salida) — pendiente de implementación
- **Repositorio:** `.gitignore` agregado; artefactos de `build/` y `.DS_Store` fuera del control de versiones

---

## Resumen de Implementación — Iteración 1

### Core C++ (218 tests, 0 fallos)

| Fase | Módulo | Tests |
|------|--------|-------|
| F1 | CMake + SQLite 3.49.1 | — (compilación) |
| F2 | DatabaseManager | 44 ✅ |
| F3 | AudioLogic (header-only) | 64 ✅ |
| F4 | HAL CoreAudio + AudioEngine | 43 ✅ |
| F5 | StateManager + VirtualGroupService | 43 ✅ |
| F6 | Bridge API (C-API) | 67 ✅ |

### macOS UI (Swift / SwiftUI)

| Fase | Módulo | Estado |
|------|--------|--------|
| F7 | App Shell (MenuBarExtra, ViewModel) | ✅ BUILD SUCCEEDED |
| F8 | DeviceRowView (slider, mute, inline editor) | ✅ BUILD SUCCEEDED |
| F9 | GroupRowView (slider maestro, mute lógico, GroupEditorSheet) | ✅ BUILD SUCCEEDED |

---

## Estructura del Proyecto

```
sync-bar-audio/
├── CMakeLists.txt
├── build/lib/
│   ├── libsqlite3.a
│   └── libsyncbar_core.a          ← Core compilado (187 KB)
├── core/
│   ├── CMakeLists.txt
│   ├── vendor/sqlite3.c + sqlite3.h
│   ├── include/
│   │   ├── audio_engine.h
│   │   ├── audio_logic.h          ← header-only
│   │   ├── bridge_api.h
│   │   └── database_mgr.h
│   ├── src/
│   │   ├── audio_engine.cpp       ← HAL CoreAudio + StateManager + Groups
│   │   ├── bridge_api.cpp
│   │   └── database_mgr.cpp
│   └── tests/
│       ├── test_audio_engine.cpp
│       ├── test_audio_logic.cpp
│       ├── test_bridge_api.cpp
│       └── test_database_mgr.cpp
├── macos/
│   └── SyncBarAudio/
│       ├── SyncBarAudio.xcodeproj/
│       └── SyncBarAudio/
│           ├── App.swift
│           ├── Info.plist
│           ├── Assets.xcassets/
│           ├── CoreBridge/
│           │   ├── SyncBarBridge.h
│           │   ├── AudioViewModel.swift
│           │   └── AudioViewModel+Preview.swift
│           └── Views/
│               ├── PopoverView.swift
│               ├── DeviceRowView.swift
│               ├── GroupRowView.swift      ← incluye GroupEditorSheet
│               └── InlineLabelEditor.swift
├── windows/                               ← Iteración 2
└── aidlc-docs/
    ├── requirements/
    ├── story-artifacts/
    ├── design-artifacts/
    ├── plans/
    └── aidlc-state.md
```

---

## Correcciones post-Iteración 1 (octubre 2026)

| Paso | Contenido | Estado |
|------|-----------|--------|
| Paso 1 | Debounce con temporizador (US-04), errores de volumen del HAL + "Sin control de volumen" (US-02/US-11), hotplug sin avisos repetidos y fuera del lock (US-01/US-05), SQLite de `vendor/` dentro del core, sin permiso de micrófono | ✅ Tests en CTest (Linux) y CI · ⚠️ Pendiente de verificar en una Mac con dispositivos reales |
| Paso 2 | Interfaz `AudioHal` (CoreAudio / nulo / mock), reconciliación única para `init()` y hotplug, rechazos de mute (`SB_ERR_HAL_FAILURE`), 36 tests nuevos con `MockAudioHal` | ✅ Tests en CTest (Linux) y CI · ⚠️ Pendiente de verificar en una Mac con dispositivos reales |
| Paso 3 | Ruteo de grupos (ADR-07 fase 1) | Pendiente |
| Paso 4 | Volumen relativo por miembro (US-03) | Pendiente |

---

## Próximo Paso — Iteración 2

→ UI Windows (WinUI 3 / C#): TrayIcon, Flyout, SyncBarBridge.cs (P/Invoke), vistas XAML.
→ El Core C++ ya está completo y validado — solo se necesita la capa de UI.
