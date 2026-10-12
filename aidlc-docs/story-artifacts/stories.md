# User Stories — SyncBar Audio

**Fase:** 3 — Story Development  
**Decisiones de planning aplicadas:**
- Usuarios: melómanos casuales y usuarios del día a día
- Plataforma prioritaria: macOS (Core C++ en paralelo)
- Grupos: planos, máximo 4 dispositivos
- Mute: lógico a nivel de software
- Desconexión: dispositivo queda inactivo, se reactiva al reconectarse

---

## Epic E1 — Core Engine (C++)

### US-01 — Detección reactiva de dispositivos de audio
**Como** usuario con múltiples dispositivos de audio,  
**quiero** que la aplicación detecte automáticamente cuando conecto o desconecto un dispositivo,  
**para** que siempre vea la lista actualizada sin tener que reiniciar la app.

**Criterios de aceptación:**
- [ ] El sistema detecta la conexión de un nuevo dispositivo en ≤ 500 ms
- [ ] El sistema detecta la desconexión de un dispositivo en ≤ 500 ms
- [ ] Cada dispositivo detectado tiene un UID persistente recuperado del subsistema nativo (CoreAudio/WASAPI)
- [ ] La detección es reactiva (event-driven), no por polling

**Origen:** RF-01  
**Prioridad:** Alta

---

### US-02 — Control de volumen individual por dispositivo
**Como** usuario con bocinas y AirPods conectados simultáneamente,  
**quiero** ajustar el volumen de cada dispositivo de forma independiente,  
**para** equilibrar los niveles de audio según mi preferencia sin afectar otros dispositivos.

**Criterios de aceptación:**
- [ ] El volumen se expresa en escala lineal (0.0–1.0) mapeada a dB internamente
- [ ] El cambio de volumen se aplica al hardware en ≤ 10 ms (RNF-01)
- [ ] El estado de volumen se persiste en SQLite con debounce de 300 ms tras soltar el slider
- [ ] El mute individual por dispositivo funciona de forma independiente al mute de grupo

**Origen:** RF-02, RNF-01  
**Prioridad:** Alta

---

### US-03 — Sincronización de volumen en grupos virtuales
**Como** usuario con un grupo de bocinas creado,  
**quiero** que al mover el slider del grupo todos los dispositivos miembros ajusten su volumen proporcionalmente,  
**para** mantener el balance relativo que configuré entre ellos.

**Criterios de aceptación:**
- [ ] El núcleo calcula la proporción de ganancia en dB para cada miembro al modificar el volumen del grupo
- [ ] La distribución es equitativa si todos los miembros están al mismo nivel inicial
- [ ] Si un miembro está al 50% y otro al 100%, la proporción relativa se mantiene al escalar
- [ ] El cálculo y aplicación ocurre en ≤ 10 ms (RNF-01)
- [ ] Máximo 4 dispositivos por grupo (límite fijo)

**Origen:** RF-02, RNF-01  
**Prioridad:** Alta

---

### US-04 — Persistencia de configuración con debounce
**Como** usuario que personaliza nombres y volúmenes,  
**quiero** que mis configuraciones se guarden automáticamente,  
**para** que estén disponibles la próxima vez que abra la aplicación sin hacer nada extra.

**Criterios de aceptación:**
- [ ] Los cambios de volumen se escriben en SQLite con debounce de 300 ms (no en cada frame del slider)
- [ ] Los nombres personalizados se guardan inmediatamente al confirmar la edición
- [ ] La base de datos usa `PRAGMA foreign_keys = ON` y `ON DELETE CASCADE`
- [ ] Al iniciar la app, el estado se restaura desde SQLite antes de renderizar la UI

**Origen:** RF-03, RNF-03  
**Prioridad:** Alta

---

### US-05 — Dispositivo inactivo al desconectarse
**Como** usuario con un grupo configurado,  
**quiero** que si uno de mis dispositivos se desconecta, el grupo siga funcionando con los restantes,  
**para** no perder mi configuración cuando desconecto temporalmente un dispositivo.

**Criterios de aceptación:**
- [ ] Al desconectarse un dispositivo miembro, su campo `is_active` se actualiza a `false` en SQLite
- [ ] El grupo continúa operando con los dispositivos activos restantes
- [ ] Al reconectarse el dispositivo (mismo UID), su `is_active` vuelve a `true` automáticamente
- [ ] La UI refleja visualmente qué dispositivos del grupo están activos/inactivos

**Origen:** RF-01, RF-03 (decisión de planning P6)  
**Prioridad:** Media

---

### US-06 — HAL multiplataforma sin modificaciones de kernel
**Como** desarrollador del sistema,  
**quiero** que el núcleo se comunique con CoreAudio en macOS y WASAPI en Windows mediante compilación condicional,  
**para** mantener un único codebase en `/core` sin instalar KEXTs ni drivers adicionales.

**Criterios de aceptación:**
- [ ] El código en `/core` usa `#ifdef __APPLE__` para CoreAudio y `#ifdef _WIN32` para WASAPI
- [ ] No se requiere instalación de KEXTs ni extensiones de kernel firmadas
- [ ] El binario compila correctamente en macOS 13+ (Apple Silicon y x86) y Windows 11 (64-bit)
- [ ] La memoria de la UI en inactividad no supera 50 MB

**Origen:** RNF-02, 2.3 Restricciones  
**Prioridad:** Alta

---

## Epic E2 — Bridge / FFI (C-API)

### US-07 — C-API para control de dispositivos
**Como** capa de UI (Swift o C#),  
**quiero** llamar funciones del núcleo C++ para listar dispositivos, cambiar volumen y mute,  
**para** no duplicar lógica de audio en cada plataforma.

**Criterios de aceptación:**
- [ ] La C-API expone: `get_devices()`, `set_volume(uid, value)`, `set_mute(uid, bool)`
- [ ] Las funciones son thread-safe para llamadas desde el hilo principal de la UI
- [ ] El bridge no introduce latencia adicional perceptible (llamada directa, sin serialización)
- [ ] En macOS se consume vía `.bridging-header`; en Windows vía P/Invoke

**Origen:** RF-01, RF-02, Sección 2.4  
**Prioridad:** Alta

---

### US-08 — C-API para gestión de grupos virtuales
**Como** capa de UI,  
**quiero** llamar funciones del núcleo para crear, modificar y eliminar grupos virtuales,  
**para** que la lógica de agrupamiento viva en el Core y no en la UI.

**Criterios de aceptación:**
- [ ] La C-API expone: `create_group(name)`, `add_device_to_group(group_id, uid)`, `remove_device_from_group(group_id, uid)`, `set_group_volume(group_id, value)`, `set_group_mute(group_id, bool)`, `delete_group(group_id)`
- [ ] `add_device_to_group` retorna error si el grupo ya tiene 4 dispositivos
- [ ] El mute de grupo es lógico (no modifica el hardware de los miembros)
- [ ] Los grupos son planos (sin anidamiento)

**Origen:** RF-02, decisiones P3, P4, P5  
**Prioridad:** Alta

---

### US-09 — C-API para etiquetas personalizadas
**Como** capa de UI,  
**quiero** llamar funciones del núcleo para leer y escribir nombres personalizados de dispositivos y grupos,  
**para** que Sofía pueda llamar a sus bocinas "Bocina Izquierda" y "Bocina Derecha".

**Criterios de aceptación:**
- [ ] La C-API expone: `set_device_label(uid, label)`, `get_device_label(uid)`, `set_group_name(group_id, name)`, `get_group_name(group_id)`
- [ ] Los labels se persisten en SQLite en la columna `custom_name` de la tabla `devices`
- [ ] El label máximo es de 64 caracteres UTF-8
- [ ] Si no hay label personalizado, se retorna el nombre nativo del dispositivo

**Origen:** RF-05, RF-03  
**Prioridad:** Media

---

## Epic E3 — UI macOS (SwiftUI)

### US-10 — App residente en Menu Bar sin icono en Dock
**Como** usuario de macOS,  
**quiero** que SyncBar Audio viva solo en la barra de menú superior,  
**para** que no ocupe espacio en el Dock ni aparezca en el App Switcher.

**Criterios de aceptación:**
- [ ] La app usa `MenuBarExtra` de SwiftUI y `LSUIElement = true` en Info.plist
- [ ] No aparece ningún icono en el Dock
- [ ] No aparece en el App Switcher (Cmd+Tab)
- [ ] El ícono de la barra de menú es reconocible y usa SF Symbols

**Origen:** RF-04  
**Prioridad:** Alta

---

### US-11 — Pop-over con lista de dispositivos y sliders de volumen
**Como** Andrés (melómano de escritorio),  
**quiero** hacer clic en el ícono de la barra de menú y ver todos mis dispositivos con un slider de volumen cada uno,  
**para** ajustar el volumen de mis bocinas y AirPods sin abrir ninguna ventana.

**Criterios de aceptación:**
- [ ] Al hacer clic en el ícono aparece un pop-over (no una ventana)
- [ ] Cada dispositivo activo muestra: nombre (custom o nativo), slider de volumen, botón de mute
- [ ] Los dispositivos inactivos se muestran atenuados con indicador visual
- [ ] El pop-over se cierra al hacer clic fuera de él
- [ ] El consumo de RAM de la UI no supera 50 MB en inactividad

**Origen:** RF-04, RF-01, RNF (memoria)  
**Prioridad:** Alta

---

### US-12 — Edición inline de nombre de dispositivo
**Como** Sofía (ambientadora del cuarto),  
**quiero** hacer doble clic en el nombre de un dispositivo para renombrarlo directamente en el pop-over,  
**para** llamar a mis bocinas con nombres que yo entienda.

**Criterios de aceptación:**
- [ ] Doble clic (o clic en ícono de edición) activa un campo de texto inline sobre el nombre
- [ ] Al presionar Enter o hacer clic fuera, el nombre se guarda y la UI vuelve al modo lectura
- [ ] Al presionar Escape, se cancela la edición sin guardar
- [ ] El nombre actualizado se refleja inmediatamente en la UI
- [ ] El cambio se persiste vía `set_device_label()` del bridge

**Origen:** RF-05  
**Prioridad:** Media

---

### US-13 — Gestión visual de grupos virtuales en macOS
**Como** Sofía,  
**quiero** crear un grupo con mis dos bocinas y ver un slider único que las controle juntas,  
**para** ambientar mi cuarto sin tener que mover dos sliders por separado.

**Criterios de aceptación:**
- [ ] La UI permite crear un grupo nuevo con nombre personalizado
- [ ] Se pueden agregar hasta 4 dispositivos al grupo mediante una interfaz visual (checkboxes o drag)
- [ ] El grupo muestra un slider maestro y un botón de mute lógico
- [ ] Los dispositivos miembros del grupo muestran su volumen individual debajo del slider maestro
- [ ] Un grupo con todos sus miembros inactivos se muestra como "sin señal"

**Origen:** RF-02, RF-05, decisiones P3–P6  
**Prioridad:** Alta

---

## Epic E4 — UI Windows (WinUI 3 / C#)

> **Nota:** Las stories E4 son equivalentes funcionales de E3 adaptadas a Windows. Se desarrollan en segunda iteración.

### US-14 — App residente en System Tray sin icono en Taskbar
**Origen:** RF-04 | **Prioridad:** Alta (segunda iteración)

### US-15 — Flyout con lista de dispositivos y sliders de volumen
**Origen:** RF-04, RF-01 | **Prioridad:** Alta (segunda iteración)

### US-16 — Edición inline de nombre de dispositivo (Windows)
**Origen:** RF-05 | **Prioridad:** Media (segunda iteración)

### US-17 — Gestión visual de grupos virtuales en Windows
**Origen:** RF-02 | **Prioridad:** Alta (segunda iteración)

---

## Resumen de Prioridades

| ID | Story | Epic | Prioridad | Iteración |
|----|-------|------|-----------|-----------|
| US-01 | Detección reactiva de dispositivos | E1 Core | Alta | 1 |
| US-02 | Control de volumen individual | E1 Core | Alta | 1 |
| US-03 | Sincronización de volumen en grupos | E1 Core | Alta | 1 |
| US-04 | Persistencia con debounce | E1 Core | Alta | 1 |
| US-05 | Dispositivo inactivo al desconectarse | E1 Core | Media | 1 |
| US-06 | HAL multiplataforma sin kernel mods | E1 Core | Alta | 1 |
| US-07 | C-API control de dispositivos | E2 Bridge | Alta | 1 |
| US-08 | C-API grupos virtuales | E2 Bridge | Alta | 1 |
| US-09 | C-API etiquetas personalizadas | E2 Bridge | Media | 1 |
| US-10 | Menu Bar sin Dock (macOS) | E3 macOS | Alta | 1 |
| US-11 | Pop-over con sliders (macOS) | E3 macOS | Alta | 1 |
| US-12 | Edición inline de nombre (macOS) | E3 macOS | Media | 1 |
| US-13 | Gestión de grupos (macOS) | E3 macOS | Alta | 1 |
| US-14 | System Tray sin Taskbar (Windows) | E4 Windows | Alta | 2 |
| US-15 | Flyout con sliders (Windows) | E4 Windows | Alta | 2 |
| US-16 | Edición inline de nombre (Windows) | E4 Windows | Media | 2 |
| US-17 | Gestión de grupos (Windows) | E4 Windows | Alta | 2 |
