# Story Generation Plan — SyncBar Audio

**Fase:** 2 — Story Planning  
**Basado en:** SRS v1.0 (ISO/IEC/IEEE 29148:2018)  
**Enfoque de descomposición:** Feature-based por capa arquitectónica

---

## Metodología de Descomposición

Las user stories se organizan en **4 epics** que mapean directamente a las capas del SRS:

| Epic | Capa | Requerimientos origen |
|------|------|-----------------------|
| E1 — Core Engine | Núcleo C++ | RF-01, RF-02, RF-03, RNF-01, RNF-02, RNF-03 |
| E2 — Bridge / FFI | Capa de Enlace | RF-01 a RF-05 (exposición) |
| E3 — UI macOS | Capa de Interfaz macOS | RF-04, RF-05 |
| E4 — UI Windows | Capa de Interfaz Windows | RF-04, RF-05 |

---

## Plan de Stories por Epic

### E1 — Core Engine (C++)

**Paso 1.1** — Story: Detección reactiva de hardware  
Origen: RF-01  
[Answer]: 

**Paso 1.2** — Story: Cálculo de ganancia en grupos virtuales  
Origen: RF-02  
[Answer]: 

**Paso 1.3** — Story: Persistencia con debounce en SQLite  
Origen: RF-03, RNF-03  
[Answer]: 

**Paso 1.4** — Story: HAL multiplataforma (CoreAudio / WASAPI)  
Origen: RNF-02  
[Answer]: 

**Paso 1.5** — Story: Cumplimiento de latencia ≤ 10 ms  
Origen: RNF-01  
[Answer]: 

---

### E2 — Bridge / FFI

**Paso 2.1** — Story: Exposición de C-API para control de dispositivos  
Origen: RF-01, RF-04  
[Answer]: 

**Paso 2.2** — Story: Exposición de C-API para grupos virtuales  
Origen: RF-02  
[Answer]: 

**Paso 2.3** — Story: Exposición de C-API para persistencia  
Origen: RF-03, RF-05  
[Answer]: 

---

### E3 — UI macOS (SwiftUI)

**Paso 3.1** — Story: App residente en MenuBar (sin Dock)  
Origen: RF-04  
[Answer]: 

**Paso 3.2** — Story: Listado y control de volumen de dispositivos  
Origen: RF-01, RF-02  
[Answer]: 

**Paso 3.3** — Story: Edición inline de etiquetas de dispositivos  
Origen: RF-05  
[Answer]: 

**Paso 3.4** — Story: Gestión visual de grupos virtuales  
Origen: RF-02  
[Answer]: 

---

### E4 — UI Windows (WinUI 3 / C#)

**Paso 4.1** — Story: App residente en System Tray (sin Taskbar)  
Origen: RF-04  
[Answer]: 

**Paso 4.2** — Story: Listado y control de volumen de dispositivos  
Origen: RF-01, RF-02  
[Answer]: 

**Paso 4.3** — Story: Edición inline de etiquetas de dispositivos  
Origen: RF-05  
[Answer]: 

**Paso 4.4** — Story: Gestión visual de grupos virtuales  
Origen: RF-02  
[Answer]: 

---

## Preguntas de Planning Embebidas

Responde con `[Answer]:` antes de continuar a la Fase 3.

### P1 — Personas
[Answer]: Usuarios del día a día y melómanos casuales. Personas que quieren conectar bocinas de escritorio y AirPods al mismo tiempo, o dos bocinas Bluetooth para ambientar su cuarto, y necesitan equilibrar el volumen de ambas con un slider en un pop-over, sin abrir herramientas nativas confusas.

---

### P2 — Prioridad de plataforma
[Answer]: Core C++ en paralelo desde el inicio. Interfaz macOS primero, Windows después.

---

### P3 — Grupos virtuales
[Answer]: Solo planos (un nivel de agrupamiento). Sin anidamiento.

---

### P4 — Mute en grupos
[Answer]: Mute lógico a nivel de software. Los dispositivos físicos no cambian su estado en el hardware.

---

### P5 — Límite de dispositivos por grupo
[Answer]: Límite fijo de 4 dispositivos por grupo. Prioridad de cero latencia percibida.

---

### P6 — Comportamiento ante desconexión
[Answer]: Mantener el dispositivo en el grupo como "inactivo" (is_active = false) y reactivarlo automáticamente al reconectarse.

---

## Checklist de Completitud

- [x] Todas las preguntas P1–P6 respondidas
- [x] Sin respuestas ambiguas
- [x] Epics E1–E4 confirmados como correctos
- [x] Listo para avanzar a Fase 3 — Story Development
