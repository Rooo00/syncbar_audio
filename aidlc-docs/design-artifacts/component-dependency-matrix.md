# Component Dependency Matrix — SyncBar Audio

**Lectura:** La fila depende de la columna. ✅ = dependencia directa, — = sin dependencia.

---

## Matriz de Dependencias

| Componente | HAL_CoreAudio | HAL_WASAPI | AudioLogic | StateManager | DatabaseManager | BridgeAPI | AudioViewModel (mac) | AudioViewModel (win) |
|---|:---:|:---:|:---:|:---:|:---:|:---:|:---:|:---:|
| **HAL_CoreAudio** | — | — | — | — | — | — | — | — |
| **HAL_WASAPI** | — | — | — | — | — | — | — | — |
| **AudioLogic** | — | — | — | — | — | — | — | — |
| **StateManager** | — | — | ✅ | — | — | — | — | — |
| **AudioEngine** | ✅ | ✅ | ✅ | ✅ | — | — | — | — |
| **DatabaseManager** | — | — | — | — | — | — | — | — |
| **BridgeAPI** | — | — | — | — | ✅ | — | — | — |
| **BridgeAPI** (cont.) | — | — | — | — | — | — | — | — |
| **AudioViewModel (mac)** | — | — | — | — | — | ✅ | — | — |
| **AudioViewModel (win)** | — | — | — | — | — | ✅ | — | — |
| **Views macOS** | — | — | — | — | — | — | ✅ | — |
| **Views Windows** | — | — | — | — | — | — | — | ✅ |

---

## Orden de Compilación (Dependency-First)

```
Nivel 0 (sin dependencias externas):
  ├── HAL_CoreAudio      (solo CoreAudio framework)
  ├── HAL_WASAPI         (solo WASAPI headers)
  ├── AudioLogic         (solo <cmath>)
  └── DatabaseManager    (solo SQLite3)

Nivel 1 (depende de Nivel 0):
  └── AudioEngine / StateManager
        ├── usa HAL_CoreAudio o HAL_WASAPI (condicional)
        ├── usa AudioLogic
        └── usa DatabaseManager

Nivel 2 (depende de Nivel 1):
  └── BridgeAPI
        └── usa AudioEngine (a través de instancia singleton)

Nivel 3 (depende de Nivel 2):
  ├── AudioViewModel (macOS Swift)
  └── AudioViewModel (Windows C#)
        └── ambos usan BridgeAPI vía FFI

Nivel 4 (depende de Nivel 3):
  ├── Views macOS (SwiftUI)
  └── Views Windows (XAML)
        └── ambos usan su respectivo AudioViewModel
```

---

## Dependencias Externas

| Componente | Dependencia Externa | Versión | Plataforma |
|------------|--------------------|---------|----|
| HAL_CoreAudio | CoreAudio.framework | macOS 13+ | macOS |
| HAL_CoreAudio | AudioToolbox.framework | macOS 13+ | macOS |
| HAL_WASAPI | mmdeviceapi.h (Windows SDK) | Win 11 | Windows |
| HAL_WASAPI | audioclient.h (Windows SDK) | Win 11 | Windows |
| DatabaseManager | sqlite3.h (amalgamation) | 3.x | Ambas |
| AudioLogic | `<cmath>` (stdlib) | C++17 | Ambas |
| BridgeAPI | `<mutex>`, `<atomic>` (stdlib) | C++17 | Ambas |
| MenuBarApp | SwiftUI, AppKit | macOS 13+ | macOS |
| TrayApp | WinUI 3, .NET 8 | Win 11 | Windows |

---

## Reglas de Aislamiento (RNF-02)

```
/core/
  ├── Solo puede usar: stdlib C++17, SQLite3 amalgamation
  ├── NO puede importar: AppKit, SwiftUI, WinRT, .NET
  ├── Bifurcación de plataforma: SOLO mediante #ifdef __APPLE__ / #ifdef _WIN32
  └── Toda lógica de negocio debe compilar en ambas plataformas

/macos/
  ├── Solo puede llamar al Core vía bridge_api.h
  └── NO puede acceder directamente a audio_engine.h ni database_mgr.h

/windows/
  ├── Solo puede llamar al Core vía syncbar_core.dll (P/Invoke)
  └── NO puede acceder directamente a headers del Core
```

---

## Interfaces entre Capas

```
┌─────────────────────────────────────────────────────────┐
│  UI macOS          │  Interfaz          │  UI Windows    │
│  (Swift)           │                   │  (C#)          │
├────────────────────┼───────────────────┼────────────────┤
│  bridging-header   │  bridge_api.h     │  P/Invoke DLL  │
│  (static lib)      │  (C-API)          │  (shared lib)  │
├────────────────────┴───────────────────┴────────────────┤
│                    Core Engine (C++)                     │
│              libsyncbar_core.a / .dylib / .dll           │
└─────────────────────────────────────────────────────────┘
```

**macOS:** El Core se compila como librería estática (`.a`) y se enlaza directamente al bundle de la app Swift.  
**Windows:** El Core se compila como DLL (`.dll`) y se carga en runtime vía P/Invoke.
