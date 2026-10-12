# Component Diagram — SyncBar Audio

---

## Vista General de Capas

```
╔══════════════════════════════════════════════════════════════════════════╗
║                     CAPA 1 — UI NATIVA                                  ║
║                                                                          ║
║  ┌─────────────────────────────┐   ┌─────────────────────────────────┐  ║
║  │       macOS (SwiftUI)       │   │      Windows (WinUI 3 / C#)     │  ║
║  │                             │   │                                 │  ║
║  │  ┌─────────────────────┐    │   │  ┌───────────────────────────┐  │  ║
║  │  │   MenuBarApp.swift  │    │   │  │      App.xaml.cs          │  │  ║
║  │  │  (MenuBarExtra)     │    │   │  │     (TrayIcon)            │  │  ║
║  │  └──────────┬──────────┘    │   │  └────────────┬──────────────┘  │  ║
║  │             │               │   │               │                 │  ║
║  │  ┌──────────▼──────────┐    │   │  ┌────────────▼──────────────┐  │  ║
║  │  │     Views/          │    │   │  │        Views/             │  │  ║
║  │  │  DeviceRowView      │    │   │  │   DeviceControl.xaml      │  │  ║
║  │  │  GroupRowView       │    │   │  │   GroupControl.xaml       │  │  ║
║  │  │  PopoverView        │    │   │  │   FlyoutView.xaml         │  │  ║
║  │  └──────────┬──────────┘    │   │  └────────────┬──────────────┘  │  ║
║  │             │               │   │               │                 │  ║
║  │  ┌──────────▼──────────┐    │   │  ┌────────────▼──────────────┐  │  ║
║  │  │   CoreBridge/       │    │   │  │      CoreBridge/          │  │  ║
║  │  │  bridging-header    │    │   │  │   SyncBarBridge.cs        │  │  ║
║  │  │  AudioViewModel     │    │   │  │   (P/Invoke)              │  │  ║
║  │  └──────────┬──────────┘    │   │  └────────────┬──────────────┘  │  ║
║  └─────────────┼───────────────┘   └───────────────┼─────────────────┘  ║
╚════════════════╪══════════════════════════════════╪═════════════════════╝
                 │                                  │
                 └──────────────┬───────────────────┘
                                │  C-API (bridge_api.h)
╔═══════════════════════════════▼══════════════════════════════════════════╗
║                     CAPA 2 — BRIDGE / FFI                               ║
║                                                                          ║
║  ┌───────────────────────────────────────────────────────────────────┐  ║
║  │                        bridge_api.h / .cpp                        │  ║
║  │                                                                   │  ║
║  │   sb_init()          sb_get_devices()      sb_create_group()      │  ║
║  │   sb_shutdown()      sb_set_device_volume  sb_set_group_volume()  │  ║
║  │   sb_register_*()    sb_set_device_mute()  sb_set_group_mute()    │  ║
║  │                      sb_set_device_label() sb_add/remove_device() │  ║
║  └───────────────────────────────────────────────────────────────────┘  ║
╚══════════════════════════════════════════════════════════════════════════╝
                                │
╔═══════════════════════════════▼══════════════════════════════════════════╗
║                     CAPA 3 — CORE ENGINE (C++)                          ║
║                                                                          ║
║  ┌─────────────────────┐   ┌─────────────────────┐                      ║
║  │   audio_engine.h    │   │   database_mgr.h     │                      ║
║  │   audio_engine.cpp  │   │   database_mgr.cpp   │                      ║
║  │                     │   │                      │                      ║
║  │  AudioDeviceService │   │  PersistenceService  │                      ║
║  │  VirtualGroupService│   │  (SQLite + debounce) │                      ║
║  │  AudioLogic (dB)    │   │                      │                      ║
║  │  StateManager       │   └──────────────────────┘                      ║
║  └──────────┬──────────┘                                                 ║
║             │                                                            ║
║  ┌──────────▼──────────────────────────────────────────────────────┐    ║
║  │                    HAL (dentro de audio_engine.cpp)             │    ║
║  │                                                                 │    ║
║  │   #ifdef __APPLE__              #ifdef _WIN32                   │    ║
║  │   ┌─────────────────────┐       ┌─────────────────────────┐    │    ║
║  │   │   CoreAudio HAL     │       │      WASAPI HAL          │    │    ║
║  │   │  AudioObjectGetProp │       │  IMMDeviceEnumerator     │    │    ║
║  │   │  AudioObjectSetProp │       │  IAudioEndpointVolume    │    │    ║
║  │   │  AudioHardwareAddPL │       │  IMMNotificationClient   │    │    ║
║  │   └─────────────────────┘       └─────────────────────────┘    │    ║
║  │   #endif                        #endif                          │    ║
║  └─────────────────────────────────────────────────────────────────┘    ║
╚══════════════════════════════════════════════════════════════════════════╝
                                │
                    ┌───────────▼───────────┐
                    │   Hardware de Audio   │
                    │  (Bocinas, AirPods,   │
                    │   Bluetooth, USB)     │
                    └───────────────────────┘
```

---

## Componentes Detallados

### Capa 3 — Core Engine

| Componente | Archivo | Responsabilidad |
|------------|---------|-----------------|
| `AudioEngine` | `audio_engine.h/.cpp` | Orquesta detección, volumen, grupos y HAL |
| `HAL_CoreAudio` | `audio_engine.cpp` (`#ifdef __APPLE__`) | Interfaz con CoreAudio API |
| `HAL_WASAPI` | `audio_engine.cpp` (`#ifdef _WIN32`) | Interfaz con WASAPI |
| `AudioLogic` | `audio_engine.cpp` | Cálculos dB, proporciones de grupo |
| `StateManager` | `audio_engine.cpp` | Estado en memoria de dispositivos y grupos |
| `DatabaseManager` | `database_mgr.h/.cpp` | CRUD SQLite, debounce, carga inicial |

### Capa 2 — Bridge

| Componente | Archivo | Responsabilidad |
|------------|---------|-----------------|
| `BridgeAPI` | `bridge_api.h/.cpp` | C-API pública, validaciones de entrada, thread safety |

### Capa 1 — UI macOS

| Componente | Archivo | Responsabilidad |
|------------|---------|-----------------|
| `MenuBarApp` | `App.swift` | Punto de entrada, `MenuBarExtra`, ciclo de vida |
| `AudioViewModel` | `CoreBridge/AudioViewModel.swift` | Estado observable, llama al bridge, dispatch al main thread |
| `PopoverView` | `Views/PopoverView.swift` | Contenedor principal del pop-over |
| `DeviceRowView` | `Views/DeviceRowView.swift` | Fila de dispositivo: nombre, slider, mute |
| `GroupRowView` | `Views/GroupRowView.swift` | Fila de grupo: nombre, slider maestro, mute lógico |
| `InlineLabelEditor` | `Views/InlineLabelEditor.swift` | Campo de edición inline de nombre |

### Capa 1 — UI Windows *(segunda iteración)*

| Componente | Archivo | Responsabilidad |
|------------|---------|-----------------|
| `TrayApp` | `App.xaml.cs` | Punto de entrada, TrayIcon, ciclo de vida |
| `SyncBarBridge` | `CoreBridge/SyncBarBridge.cs` | P/Invoke wrapper de la C-API |
| `AudioViewModel` | `CoreBridge/AudioViewModel.cs` | Estado observable (INotifyPropertyChanged) |
| `FlyoutView` | `Views/FlyoutView.xaml` | Contenedor del flyout |
| `DeviceControl` | `Views/DeviceControl.xaml` | Control de dispositivo con slider |
| `GroupControl` | `Views/GroupControl.xaml` | Control de grupo con slider maestro |

---

## Flujo de Datos — Cambio de Volumen de Dispositivo

```
Usuario mueve slider
        │
        ▼
DeviceRowView (SwiftUI)
  onChange(volume)
        │
        ▼
AudioViewModel.setVolume(uid, value)   ← debounce 300ms para persistencia
        │
        ▼
sb_set_device_volume(uid, value)       ← C-API Bridge
        │
        ▼
AudioEngine::setDeviceVolume(uid, value)
        │
        ├── AudioLogic::linearToDb(value)
        │
        ▼
HAL::setVolume(uid, dbValue)
        │
        ├── #ifdef __APPLE__: AudioObjectSetPropertyData(kAudioHardwareServiceDeviceProperty_VirtualMasterVolume)
        └── #ifdef _WIN32:    IAudioEndpointVolume::SetMasterVolumeLevelScalar()
```

## Flujo de Datos — Evento de Desconexión de Hardware

```
Hardware desconectado
        │
        ▼
HAL callback (hilo del HAL)
  on_device_disconnected(uid)
        │
        ▼
AudioEngine::handleDisconnect(uid)
  StateManager: device.is_active = false
  StateManager: group_devices.is_active = false (para ese uid)
  DatabaseManager: UPDATE devices SET is_active=0 WHERE uid=?
  DatabaseManager: UPDATE group_devices SET is_active=0 WHERE device_uid=?
        │
        ▼
BridgeAPI: invoca SB_DeviceDisconnectedCallback(uid)
        │
        ▼
AudioViewModel (UI): dispatch al main thread
  → actualiza lista de dispositivos
  → dispositivo aparece atenuado en la UI
```
