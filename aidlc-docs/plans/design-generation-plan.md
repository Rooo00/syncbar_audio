# Design Generation Plan — SyncBar Audio

**Fase:** 5 — Design  
**Arquitectura:** Monolith path (3 capas)  
**Orden de generación:** Domain Model → DB Schema → C-API Spec → Component Diagram → Component Dependency Matrix

---

## Checklist de Artefactos

- [x] `domain-model.md` — Entidades, atributos y relaciones del dominio
- [x] `db-schema.md` — Esquema SQLite detallado con DDL
- [x] `api-specification.md` — C-API completa del Bridge (FFI)
- [x] `component-diagram.md` — Diagrama de componentes por capa
- [x] `component-dependency-matrix.md` — Matriz de dependencias entre componentes
- [x] `architectural-decisions.md` — Registro de decisiones arquitectónicas (ADR)

---

## Orden de Implementación (Fase 6)

1. **Foundation:** `database_mgr` (SQLite schema + CRUD)
2. **HAL:** `audio_engine` — detección de dispositivos (CoreAudio primero)
3. **Business Logic:** `audio_engine` — grupos, volumen, mute, debounce
4. **Bridge:** `bridge_api` — C-API expuesta
5. **UI macOS:** SwiftUI — MenuBarExtra, pop-over, sliders
6. **UI Windows:** WinUI 3 — TrayIcon, flyout (segunda iteración)
