# Architectural Decision Records (ADR) — SyncBar Audio

---

## ADR-01: Núcleo compartido en C++ con UIs nativas independientes

**Estado:** Aceptado  
**Fecha:** Mayo 2026

**Contexto:**  
La app debe correr en macOS y Windows con consumo de RAM ≤ 50 MB y sin modificaciones de kernel.

**Decisión:**  
Core en C++17 con compilación condicional para HAL. UI 100% nativa por plataforma (SwiftUI / WinUI 3).

**Consecuencias:**
- ✅ Consumo mínimo de RAM (sin runtime de Electron/Qt)
- ✅ Integración perfecta con el diseño del SO
- ✅ Un solo codebase de lógica de negocio
- ⚠️ Dos codebases de UI a mantener

---

## ADR-02: SQLite como motor de persistencia con debounce

**Estado:** Aceptado  
**Fecha:** Mayo 2026

**Contexto:**  
Los sliders generan eventos continuos; escribir en cada frame causaría degradación de rendimiento.

**Decisión:**  
SQLite con debounce de 300 ms en la capa C++. `PRAGMA foreign_keys = ON` y `ON DELETE CASCADE`.

**Consecuencias:**
- ✅ Cero dependencias de red, funciona offline
- ✅ Sin escrituras excesivas durante arrastre de slider
- ✅ Integridad referencial garantizada por el motor

---

## ADR-03: Mute de grupo lógico (no propaga al hardware)

**Estado:** Aceptado  
**Fecha:** Mayo 2026

**Contexto:**  
Un dispositivo puede pertenecer a múltiples grupos. Propagar el mute al hardware causaría conflictos entre grupos.

**Decisión:**  
El mute de grupo es un flag lógico en `virtual_groups.is_muted`. El HAL no recibe ninguna instrucción de mute cuando se mutea un grupo.

**Consecuencias:**
- ✅ Sin conflictos entre grupos que comparten dispositivos
- ✅ El estado del hardware es predecible
- ⚠️ El usuario debe entender que "mutear el grupo" no silencia el dispositivo en otros contextos

---

## ADR-04: Límite fijo de 4 dispositivos por grupo

**Estado:** Aceptado  
**Fecha:** Mayo 2026

**Contexto:**  
El caso de uso principal es 2–4 dispositivos (bocinas + AirPods). Un límite fijo simplifica la UI y reduce la latencia de cálculo de proporciones.

**Decisión:**  
Máximo 4 dispositivos por grupo, enforced por trigger SQLite y validación en BridgeAPI.

**Consecuencias:**
- ✅ Latencia de cálculo de grupo predecible y mínima
- ✅ UI más simple (no necesita scroll en la lista de miembros)
- ⚠️ No cubre casos de uso con más de 4 dispositivos (fuera del alcance v1.0)

---

## ADR-05: Dispositivo inactivo al desconectarse (no eliminado del grupo)

**Estado:** Aceptado  
**Fecha:** Mayo 2026

**Contexto:**  
Los dispositivos Bluetooth se desconectan frecuentemente. Eliminarlos del grupo obligaría al usuario a reconfigurar constantemente.

**Decisión:**  
Al desconectarse, `is_active = false` en `devices` y `group_devices`. Al reconectarse con el mismo UID, `is_active = true` automáticamente.

**Consecuencias:**
- ✅ La configuración del usuario persiste entre desconexiones
- ✅ El grupo sigue funcionando con los miembros activos restantes
- ⚠️ La UI debe comunicar claramente qué miembros están inactivos

---

## ADR-06: macOS como plataforma prioritaria (primera iteración)

**Estado:** Aceptado  
**Fecha:** Mayo 2026

**Contexto:**  
El Core C++ se desarrolla en paralelo para ambas plataformas, pero la UI de macOS se implementa primero para validar el producto con usuarios reales.

**Decisión:**  
Iteración 1: Core C++ + Bridge + UI macOS. Iteración 2: UI Windows.

**Consecuencias:**
- ✅ Feedback temprano de usuarios macOS
- ✅ El Core ya estará validado cuando se implemente Windows
- ⚠️ Usuarios Windows deben esperar la segunda iteración

---

## ADR-07: Estrategia de ruteo de audio para grupos virtuales

**Estado:** Aceptado (octubre 2026)  
**Fecha:** Octubre 2026

**Contexto:**  
La implementación actual de grupos solo controla volumen: `setGroupVolume` calcula `master × relativo` y lo escribe en el hardware de cada miembro (`kAudioHardwareServiceDeviceProperty_VirtualMainVolume`). No existe ruteo de audio: el sistema sigue enviando el sonido a una sola salida, por lo que el caso de uso principal ("bocinas + AirPods al mismo tiempo") no se cumple.

Además, `VirtualMainVolume` no está disponible en dispositivos sin volumen por hardware (HDMI, DisplayPort, algunos DACs USB) ni en dispositivos agregados, y hoy los errores de `getVolume` se ignoran.

Opciones evaluadas (macOS):

| Opción | Pros | Contras |
|---|---|---|
| A. Aggregate Device "stacked" (Multi-Output) vía `AudioHardwareCreateAggregateDevice` | API pública, sin drivers; CoreAudio resuelve remuestreo y drift; poco código | Sin compensación de latencia por dispositivo; el agregado no tiene volumen propio |
| B. Process Tap (macOS 14.2+) + mezclador propio | Retraso configurable por dispositivo (alinea Bluetooth con cableado) | Sube el mínimo a macOS 14.2; requiere permiso de captura; código de tiempo real complejo |
| C. AudioServerPlugIn (driver virtual, tipo BlackHole) | Control total en macOS 13+ | Instalación con admin, notarización, reinicio de `coreaudiod`; contrario al objetivo de simplicidad |

En Windows no existe equivalente a los Aggregate Devices; la Iteración 2 requerirá WASAPI loopback + render a N endpoints (equivalente a la opción B).

**Decisión:**  
1. **Fase 1:** opción A. Al activar un grupo se crea un Aggregate Device privado y apilado (`kAudioAggregateDeviceIsPrivateKey=1`, `kAudioAggregateDeviceIsStackedKey=1`) con drift compensation en los sub-dispositivos que no son reloj maestro. El reloj maestro será preferentemente una salida cableada. El agregado se establece como salida por defecto y, al desactivar el grupo, se restaura la salida previa (también al cerrar la app o tras un crash, ya que el agregado privado desaparece con el proceso).
2. El control de volumen/mute por miembro existente se mantiene: es justamente lo que el agregado no ofrece.
3. El ruteo se encapsula tras una interfaz `AudioRouter` en el core, separada del estado, con implementación por plataforma (CoreAudio aggregate ahora; WASAPI loopback en Iteración 2) y un mock para tests.
4. **Fase 2 (condicional):** si el desfase con dispositivos Bluetooth resulta molesto en pruebas con usuarios, migrar a la opción B con retraso ajustable por dispositivo.
5. Corregir el manejo de volumen: verificar `OSStatus`, usar `kAudioDevicePropertyVolumeScalar` por canal como respaldo y exponer en la UI los dispositivos sin control de volumen.

**Consecuencias:**
- ✅ Se cumple el caso de uso de salida simultánea sin drivers ni KEXTs
- ✅ Reutiliza el modelo de grupos, volúmenes relativos y persistencia existentes
- ✅ La interfaz `AudioRouter` prepara la implementación de Windows y mejora la testeabilidad
- ⚠️ Los dispositivos Bluetooth (150–300 ms de latencia) quedarán desfasados respecto a salidas cableadas en la Fase 1
- ⚠️ El requisito "latencia ≤ 10 ms en grupos virtuales" no es alcanzable con Bluetooth bajo ninguna opción; debe reformularse en el SRS (p. ej. "deriva acotada entre miembros" y "retraso ajustable por dispositivo")
- ⚠️ La app pasa a modificar la salida por defecto del sistema; debe restaurarla de forma robusta

**Decisiones de implementación (octubre 2026):**
- Un solo grupo activo como salida a la vez; activar otro reemplaza al anterior y conserva la salida previa original.
- Se puede activar con un solo miembro conectado. Si el grupo se queda sin miembros conectados, se desactiva solo y restaura la salida previa.
- La salida previa se guarda en SQLite (`app_state.previous_default_output`) mientras el grupo está activo. Si la app se cierra de golpe, el siguiente `init()` la restaura (si sigue conectada) y borra la clave.
- Reloj maestro: el primer miembro conectado por orden de preferencia de transporte (interno → USB → HDMI → otro/desconocido → Bluetooth).
- La salida se reconstruye cuando cambia el conjunto de miembros conectados (hotplug o edición del grupo).

**Nota sobre RNF-01:** con un Aggregate Device la app no procesa buffers (lo hace CoreAudio), así que no introduce latencia propia medible; pero el desfase entre miembros cableados y Bluetooth (150–300 ms) hace que el objetivo del requisito no se cumpla en la práctica. Se propone reformular RNF-01 en el SRS como "deriva acotada entre miembros" y, en una fase 2, "retraso ajustable por dispositivo". El SRS (`Requerimientos.docx` / `.pdf`) no se ha modificado.

**Estado de implementación:**
- 3a ✅ Interfaz `AudioRouter`, lógica en `AudioEngine`, `app_state`, `MockAudioRouter` y tests (`test_engine_routing`).
- 3b ✅ (compila en CI; sin probar con hardware) `router_coreaudio.cpp`: `AudioHardwareCreateAggregateDevice` con `private=1`, `stacked=1`, reloj en `kAudioAggregateDeviceMainSubDeviceKey` y drift compensation en el resto de subdispositivos; salida por defecto con `kAudioHardwarePropertyDefaultOutputDevice`; UID → dispositivo con `kAudioHardwarePropertyTranslateUIDToDevice`. El HAL informa el transporte (`kAudioDevicePropertyTransportType`) y oculta los agregados propios (UID con prefijo `com.syncbar.group.`). Fuera de macOS sigue `router_null.cpp`.
- 3c ✅ (compila en CI; sin probar con hardware) Bridge (`sb_activate_group_output`, `sb_deactivate_group_output`, `SB_GroupInfo.is_output`, `SB_ERR_NO_ACTIVE_MEMBERS`, `SB_ERR_ROUTING_FAILURE`) y UI (botón ▶︎/■ en `GroupRowView`, etiqueta "Salida", mensaje de error en la fila, ícono de la barra de menú).
- 3d ⏳ Documentación y pruebas manuales en una Mac.
