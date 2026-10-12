#pragma once

#include <vector>
#include <cmath>
#include <algorithm>
#include <cstdint>

/**
 * AudioLogic
 *
 * Funciones matemáticas puras de audio.
 * Sin dependencias de plataforma, sin estado global.
 * Todas las funciones son constexpr o inline donde es posible.
 *
 * Convenciones:
 *  - Volumen lineal: [0.0, 1.0]  (lo que ve la UI y el usuario)
 *  - Volumen en dB:  [-96.0, 0.0] (lo que recibe el HAL)
 *  - Silencio absoluto: -96.0 dB (umbral de percepción humana)
 */
namespace AudioLogic {

// ── Constantes ────────────────────────────────────────────────────────────────

/// Umbral de silencio absoluto en dB. Por debajo de este valor = mudo.
inline constexpr float kSilenceDb     = -96.0f;

/// Volumen lineal mínimo antes de considerar silencio.
/// Equivale a -96 dB: 10^(-96/20) ≈ 1.585e-5
inline constexpr float kSilenceLinear = 1.585e-5f;

/// Volumen máximo en dB (0 dB = ganancia unitaria).
inline constexpr float kMaxDb         = 0.0f;

/// Máximo de dispositivos por grupo (ADR-04).
inline constexpr int32_t kMaxGroupMembers = 4;

// ── Conversiones ──────────────────────────────────────────────────────────────

/**
 * Convierte volumen lineal [0.0, 1.0] a decibelios.
 * linear = 0.0  → -96.0 dB (silencio absoluto)
 * linear = 0.5  → -6.02 dB
 * linear = 1.0  →  0.0  dB (ganancia unitaria)
 */
inline float linearToDb(float linear) {
    if (linear <= kSilenceLinear) return kSilenceDb;
    return 20.0f * std::log10(linear);
}

/**
 * Convierte decibelios a volumen lineal [0.0, 1.0].
 * db = -96.0 → 0.0
 * db =  -6.0 → ~0.501
 * db =   0.0 → 1.0
 */
inline float dbToLinear(float db) {
    if (db <= kSilenceDb) return 0.0f;
    return std::pow(10.0f, db / 20.0f);
}

/**
 * Clamp de volumen lineal al rango válido [0.0, 1.0].
 */
inline float clampVolume(float v) {
    return std::clamp(v, 0.0f, 1.0f);
}

/**
 * Clamp de volumen en dB al rango válido [kSilenceDb, kMaxDb].
 */
inline float clampDb(float db) {
    return std::clamp(db, kSilenceDb, kMaxDb);
}

// ── Lógica de grupos ──────────────────────────────────────────────────────────

/**
 * Calcula el volumen efectivo de cada miembro al mover el slider maestro.
 *
 * Fórmula: effectiveVolume[i] = clamp(masterVolume * relativeVolumes[i])
 *
 * Esto preserva el balance relativo entre miembros:
 *   - Si todos están al 100% relativo y el maestro baja a 0.5,
 *     todos quedan al 50%.
 *   - Si uno está al 50% relativo y el maestro está al 1.0,
 *     ese miembro queda al 50%.
 *
 * @param masterVolume    Volumen maestro del grupo [0.0, 1.0]
 * @param relativeVolumes Volumen relativo de cada miembro [0.0, 1.0]
 * @return                Volumen efectivo de cada miembro [0.0, 1.0]
 *
 * Precondición: relativeVolumes.size() <= kMaxGroupMembers (máx 4)
 */
inline std::vector<float> distributeGroupVolume(
    float masterVolume,
    const std::vector<float>& relativeVolumes)
{
    const float master = clampVolume(masterVolume);
    std::vector<float> result;
    result.reserve(relativeVolumes.size());

    for (float rel : relativeVolumes) {
        result.push_back(clampVolume(master * clampVolume(rel)));
    }
    return result;
}

/**
 * Calcula el nuevo relative_volume de un miembro cuando el usuario
 * ajusta su volumen individual dentro de un grupo activo.
 *
 * Fórmula: newRelative = deviceVolume / masterVolume
 *
 * Si masterVolume == 0, retorna deviceVolume directamente
 * (no se puede dividir por cero; el balance se restaura cuando
 * el maestro suba de nuevo).
 *
 * @param deviceVolume  Volumen deseado para el dispositivo [0.0, 1.0]
 * @param masterVolume  Volumen maestro actual del grupo [0.0, 1.0]
 * @return              Nuevo relative_volume [0.0, 1.0]
 */
inline float computeRelativeVolume(float deviceVolume, float masterVolume) {
    const float dev    = clampVolume(deviceVolume);
    const float master = clampVolume(masterVolume);
    if (master <= kSilenceLinear) return dev;
    return clampVolume(dev / master);
}

// ── Utilidades de percepción ──────────────────────────────────────────────────

/**
 * Retorna true si el volumen lineal es perceptiblemente silencioso.
 * Umbral: < kSilenceLinear (~-96 dB).
 */
inline bool isSilent(float linear) {
    return linear <= kSilenceLinear;
}

/**
 * Aplica una curva de potencia (power curve) al valor lineal del slider
 * para que el control sea más natural al oído humano.
 *
 * La percepción del volumen es logarítmica, pero los sliders de UI
 * son lineales. Esta curva hace que la mitad del slider corresponda
 * aproximadamente a la mitad del volumen percibido.
 *
 * curve = 2.0 → curva cuadrática (buena para volumen general)
 *
 * @param sliderValue Valor del slider [0.0, 1.0]
 * @param curve       Exponente de la curva (default: 2.0)
 * @return            Volumen lineal ajustado [0.0, 1.0]
 */
inline float applyVolumeCurve(float sliderValue, float curve = 2.0f) {
    return std::pow(clampVolume(sliderValue), curve);
}

/**
 * Inverso de applyVolumeCurve: convierte volumen lineal a posición de slider.
 * Útil para inicializar el slider desde un volumen guardado.
 *
 * @param linearVolume Volumen lineal [0.0, 1.0]
 * @param curve        Exponente de la curva (debe coincidir con applyVolumeCurve)
 * @return             Posición del slider [0.0, 1.0]
 */
inline float volumeToSlider(float linearVolume, float curve = 2.0f) {
    const float v = clampVolume(linearVolume);
    if (v <= 0.0f) return 0.0f;
    return std::pow(v, 1.0f / curve);
}

} // namespace AudioLogic
