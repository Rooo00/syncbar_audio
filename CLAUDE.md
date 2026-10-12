# SyncBar Audio — Instrucciones para Claude Code

Proyecto: utilidad de barra de menú (macOS) / bandeja del sistema (Windows, Iteración 2)
para controlar dispositivos de salida de audio y agruparlos en grupos virtuales.
`core/` (C++17 + CoreAudio/WASAPI + SQLite, CMake) y `macos/SyncBarAudio/`
(Swift 5.9 + SwiftUI `MenuBarExtra`, macOS 13+). Idioma de trabajo: español.

## Dónde está cada cosa

- **Estado / roadmap (fuente de avance):** `aidlc-docs/aidlc-state.md` (fases AI-SDLC,
  decisiones tomadas, próximo paso) y `aidlc-docs/plans/code-generation-plan.md`
  (fases F1–F9 con criterios de completitud).
- **Arquitectura tal como está construida (as-built):** `aidlc-docs/design-artifacts/current-architecture.md`
  — incluye la sección 7 "Observaciones" con la deuda técnica conocida.
- **Decisiones de diseño (ADRs):** `aidlc-docs/design-artifacts/architectural-decisions.md`
  (ADR-01…07; ADR-07 = ruteo de audio de grupos, *Propuesto*).
- **Documentos de definición (fuente de verdad de QUÉ construir):**
  - `aidlc-docs/Requerimientospdf.pdf` / `Requerimientos.docx` → SRS (ISO 29148): RF-xx, RNF-xx
  - `aidlc-docs/story-artifacts/stories.md` → historias US-01…US-15 con criterios de aceptación
  - `aidlc-docs/story-artifacts/personas.md` → personas (Andrés, Sofía)
  - `aidlc-docs/design-artifacts/` → `domain-model.md`, `db-schema.md`, `api-specification.md`,
    `component-diagram.md`, `component-dependency-matrix.md`
- El estado y el plan registran el avance sobre esos documentos, nunca al revés.

## Flujo obligatorio para cada step

Cuando el usuario pida avanzar ("sigue con…", "el próximo step", "implementa X"),
**NO escribas código todavía**. Sigue estos pasos en orden:

### 1. Leer el estado y el plan
Localiza el step en `aidlc-state.md` / `code-generation-plan.md` (fase, criterios de
completitud) o en las Observaciones de `current-architecture.md` si es deuda técnica.
Si viene de un ADR, lee el ADR completo, no solo el título.

### 2. Leer el requerimiento real
Identifica la historia (US-xx) y el requisito del SRS (RF-xx / RNF-xx) de donde sale y
lee la **sección exacta**, no un resumen de memoria. Si dos documentos se contradicen,
dilo explícitamente y prefiere el más reciente/específico. Ojo: algunos requisitos del
SRS ya se sabe que no son alcanzables tal cual (p. ej. "latencia ≤ 10 ms en grupos" con
Bluetooth, ver ADR-07); señálalo en vez de prometerlo.

### 3. Mapear el código existente (contexto completo de la función)
Verifica contra el código real, no contra los documentos:
- **Core:** `core/include/*.h`, `core/src/audio_engine.cpp` (HAL CoreAudio + estado + grupos +
  hotplug), `database_mgr.cpp` (schema, triggers, debounce), `bridge_api.cpp` (C-API `sb_*`),
  `audio_logic.h` (matemáticas de volumen), tests en `core/tests/`.
- **App macOS:** `macos/SyncBarAudio/SyncBarAudio/CoreBridge/AudioViewModel.swift` (único
  ViewModel, llama `sb_*`) y `Views/` (`PopoverView` → `DeviceRowView` / `GroupRowView`).
- Una función solo está "hecha" si recorre **toda la cadena**:
  `AudioEngine` → `sb_*` en `bridge_api.h/.cpp` → método en `AudioViewModel` → control
  alcanzable desde `PopoverView`. Busca con `grep` cada eslabón. Ejemplos de huecos ya
  encontrados: `relative_volume` existe en schema/engine pero no hay `sb_*` ni UI para
  cambiarlo; `applyVolumeCurve`/`volumeToSlider` solo se usan en tests; los grupos ajustan
  volumen pero no rutean audio (ADR-07).
- Para búsquedas amplias usa el subagente Explore para no llenar el contexto.

### 4. Presentar el resumen y ESPERAR confirmación
Muestra al usuario, de forma breve:
- **Qué es la función** y de qué US / RF / ADR sale (cita exacta).
- **Estado actual real** (qué existe en cada capa, qué falta, discrepancias con los docs).
- **Qué se va a implementar** (core, bridge, ViewModel, vista, tests, docs) y en qué orden.
- **Diagrama de flujo** de la función (UI → ViewModel → `sb_*` → AudioEngine → HAL/SQLite).
- **Próximo step después de este.**

Luego pregunta si ejecutar. **No modifiques código ni documentos hasta recibir
confirmación explícita.** Si el step es grande, propón dividirlo en sub-steps.

### 5. Ejecutar solo lo confirmado
Implementa únicamente el alcance aprobado. Si aparece algo fuera de alcance, anótalo y
consúltalo; no lo hagas por tu cuenta.

### 6. Definición de "hecho" — rebanada vertical
Un ítem solo se marca ✅ si cumple **todas** las capas que aplican:
- Core: `AudioEngine` (+ `DatabaseManager` y migración/schema si cambia la persistencia).
- Bridge: función `sb_*` en `bridge_api.h` + implementación + `SB_Result` correcto.
- App: método en `AudioViewModel` + **control alcanzable desde `PopoverView`**.
- Tests que pasan:
  ```bash
  cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug && cmake --build build
  ctest --test-dir build --output-on-failure
  ```
  y la app compila en **Release** (es lo que corre el CI):
  ```bash
  cmake -S . -B build-rel -DCMAKE_BUILD_TYPE=Release && cmake --build build-rel
  xcodebuild -project macos/SyncBarAudio/SyncBarAudio.xcodeproj -target SyncBarAudio \
    -configuration Release CODE_SIGNING_ALLOWED=NO build
  ```
- CI verde (`.github/workflows/ci.yml`: job `core` + job `macos-app`).
- Verificación real cuando sea posible: correr la app en una Mac con dispositivos reales
  (cambiar volumen, conectar/desconectar audífonos Bluetooth). Desde un entorno sin macOS
  (sesión en la nube, Linux) **no se puede verificar CoreAudio ni Xcode**: dilo explícitamente.

### 7. Actualizar los documentos con honestidad
- En `aidlc-state.md`: marca ✅ citando la US / RF contra la que se verificó y actualiza
  "Última actualización".
- Si cambia la arquitectura, actualiza `current-architecture.md` (y quita de "Observaciones"
  lo que quedó resuelto, indicando cuándo).
- Si se toma o cambia una decisión de diseño, agrega o actualiza el ADR correspondiente.
- Documenta lo que NO se hizo o se hizo distinto; un hueco encontrado vale tanto como un
  ítem completado.
- Distingue bloqueos externos (hardware, cuenta de Apple Developer, firma/notarización) de
  los resolubles.

## Reglas del proyecto

- No commitear sin que el usuario lo pida. Trabajar en una rama, nunca directo en `main`.
  Commits con mensaje que explique el porqué.
- No incluir cambios no relacionados ni artefactos generados: `build/`, `DerivedData/`,
  `xcuserdata/`, `*.db` y `.DS_Store` están en `.gitignore`; no los fuerces al commit.
- Restricciones de diseño vigentes (no romperlas sin un ADR): sin KEXTs ni modificaciones
  de kernel, RAM ≤ 50 MB en inactividad, máximo 4 dispositivos por grupo (ADR-04), mute
  de grupo lógico (ADR-03), dispositivo desconectado queda inactivo (ADR-05).
- Patrones de diseño vigentes (mantener consistencia):
  - Core: pImpl (`AudioEngine::Impl`, `DatabaseManager::Impl`), RAII para recursos SQLite,
    HAL por plataforma con `#ifdef __APPLE__` / `_WIN32`, un solo `stateMutex` para el estado.
  - Bridge: C-API plana `extern "C"` con prefijo `sb_`, structs POD con buffers fijos,
    errores como `SB_Result` (nunca excepciones a través de la frontera C).
  - App: un `ObservableObject` (`AudioViewModel`) inyectado como `environmentObject`;
    los callbacks de CoreAudio llegan en otro hilo → `DispatchQueue.main.async`.
  - Planeado (ADR-07): interfaz `AudioRouter` separada del estado, con mock para tests.
- Trampas conocidas:
  - El core compila con `-Wall -Wextra -Wpedantic -Werror`: un warning rompe el build.
  - Incluye explícitamente cada header estándar que uses (`<string>`, `<atomic>`…); libc++
    los trae de forma transitiva y oculta el error hasta otra plataforma.
  - Todo `#Preview` y los helpers de preview van dentro de `#if DEBUG`; si no, el build
    Release falla.
  - La app enlaza `../../build/lib/libsyncbar_core.a`: compila el core antes de `xcodebuild`.
- Reporta siempre con honestidad qué se verificó de verdad y qué no.
