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
