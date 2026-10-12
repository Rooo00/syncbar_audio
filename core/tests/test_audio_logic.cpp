/**
 * test_audio_logic.cpp
 *
 * Verifica los criterios de completitud de F3:
 *  - linearToDb(0.0)  → -96.0
 *  - linearToDb(1.0)  →   0.0
 *  - dbToLinear(-96)  →   0.0
 *  - dbToLinear(0.0)  →   1.0
 *  - clampVolume fuera de rango
 *  - distributeGroupVolume preserva proporciones
 *  - computeRelativeVolume
 *  - applyVolumeCurve / volumeToSlider (round-trip)
 *  - isSilent
 */

#include "audio_logic.h"
#include <cstdio>
#include <cmath>

// ── Utilidad ──────────────────────────────────────────────────────────────────

static int passed = 0;
static int failed = 0;

// Comparación con tolerancia para floats
static bool nearEq(float a, float b, float eps = 1e-4f) {
    return std::fabs(a - b) <= eps;
}

#define CHECK(cond, msg)                                          \
    do {                                                          \
        if (cond) {                                               \
            printf("  [PASS] %s\n", msg); ++passed;              \
        } else {                                                  \
            printf("  [FAIL] %s  (line %d)\n", msg, __LINE__);   \
            ++failed;                                             \
        }                                                         \
    } while(0)

#define CHECK_NEAR(a, b, msg) CHECK(nearEq((a),(b)), msg)

// ── Tests ─────────────────────────────────────────────────────────────────────

void test_linearToDb() {
    printf("\n[T1] linearToDb\n");
    using namespace AudioLogic;

    CHECK_NEAR(linearToDb(0.0f),   -96.0f,  "linearToDb(0.0) = -96.0 dB");
    CHECK_NEAR(linearToDb(1.0f),     0.0f,  "linearToDb(1.0) =   0.0 dB");
    CHECK_NEAR(linearToDb(0.5f),    -6.0206f,"linearToDb(0.5) ≈  -6.02 dB");
    CHECK_NEAR(linearToDb(0.1f),   -20.0f,  "linearToDb(0.1) = -20.0 dB");
    CHECK_NEAR(linearToDb(-0.1f),  -96.0f,  "linearToDb(negative) clamps to -96 dB");
    // Valor muy pequeño pero positivo → silencio
    CHECK_NEAR(linearToDb(1e-6f),  -96.0f,  "linearToDb(tiny) = -96.0 dB");
}

void test_dbToLinear() {
    printf("\n[T2] dbToLinear\n");
    using namespace AudioLogic;

    CHECK_NEAR(dbToLinear(-96.0f), 0.0f,  "dbToLinear(-96) = 0.0");
    CHECK_NEAR(dbToLinear(0.0f),   1.0f,  "dbToLinear(0)   = 1.0");
    CHECK_NEAR(dbToLinear(-6.021f),0.5f,  "dbToLinear(-6.02) ≈ 0.5");
    CHECK_NEAR(dbToLinear(-20.0f), 0.1f,  "dbToLinear(-20) = 0.1");
    // Por debajo del umbral de silencio
    CHECK_NEAR(dbToLinear(-200.0f),0.0f,  "dbToLinear(-200) = 0.0 (below silence)");
}

void test_roundtrip() {
    printf("\n[T3] Round-trip linearToDb <-> dbToLinear\n");
    using namespace AudioLogic;

    float values[] = { 0.0f, 0.1f, 0.25f, 0.5f, 0.75f, 1.0f };
    for (float v : values) {
        float roundtrip = dbToLinear(linearToDb(v));
        // Para v=0 el round-trip es 0→-96→0, exacto
        CHECK(nearEq(roundtrip, v, 1e-3f),
              ("round-trip v=" + std::to_string(v)).c_str());
    }
}

void test_clampVolume() {
    printf("\n[T4] clampVolume\n");
    using namespace AudioLogic;

    CHECK_NEAR(clampVolume(-0.1f), 0.0f, "clampVolume(-0.1) = 0.0");
    CHECK_NEAR(clampVolume(0.0f),  0.0f, "clampVolume(0.0)  = 0.0");
    CHECK_NEAR(clampVolume(0.5f),  0.5f, "clampVolume(0.5)  = 0.5");
    CHECK_NEAR(clampVolume(1.0f),  1.0f, "clampVolume(1.0)  = 1.0");
    CHECK_NEAR(clampVolume(1.1f),  1.0f, "clampVolume(1.1)  = 1.0");
    CHECK_NEAR(clampVolume(99.0f), 1.0f, "clampVolume(99.0) = 1.0");
}

void test_distributeGroupVolume() {
    printf("\n[T5] distributeGroupVolume\n");
    using namespace AudioLogic;

    // Caso 1: todos al 100% relativo, maestro al 50% → todos al 50%
    {
        auto result = distributeGroupVolume(0.5f, {1.0f, 1.0f, 1.0f});
        CHECK(result.size() == 3,          "size = 3");
        CHECK_NEAR(result[0], 0.5f,        "member[0] = 0.5");
        CHECK_NEAR(result[1], 0.5f,        "member[1] = 0.5");
        CHECK_NEAR(result[2], 0.5f,        "member[2] = 0.5");
    }

    // Caso 2: proporciones distintas — maestro al 100%, relativos {1.0, 0.5}
    {
        auto result = distributeGroupVolume(1.0f, {1.0f, 0.5f});
        CHECK_NEAR(result[0], 1.0f, "member[0] = 1.0 (full)");
        CHECK_NEAR(result[1], 0.5f, "member[1] = 0.5 (half)");
    }

    // Caso 3: maestro al 50%, relativos {1.0, 0.5} → {0.5, 0.25}
    {
        auto result = distributeGroupVolume(0.5f, {1.0f, 0.5f});
        CHECK_NEAR(result[0], 0.5f,  "member[0] = 0.5");
        CHECK_NEAR(result[1], 0.25f, "member[1] = 0.25");
    }

    // Caso 4: maestro a 0 → todos silenciados
    {
        auto result = distributeGroupVolume(0.0f, {1.0f, 0.8f, 0.6f, 0.4f});
        for (size_t i = 0; i < result.size(); ++i) {
            CHECK_NEAR(result[i], 0.0f,
                       ("member[" + std::to_string(i) + "] = 0.0 when master=0").c_str());
        }
    }

    // Caso 5: maestro a 1, relativos a 1 → todos al máximo
    {
        auto result = distributeGroupVolume(1.0f, {1.0f, 1.0f, 1.0f, 1.0f});
        for (size_t i = 0; i < result.size(); ++i) {
            CHECK_NEAR(result[i], 1.0f,
                       ("member[" + std::to_string(i) + "] = 1.0 when master=1").c_str());
        }
    }

    // Caso 6: valores fuera de rango son clampados
    {
        auto result = distributeGroupVolume(1.5f, {1.2f, -0.1f});
        CHECK_NEAR(result[0], 1.0f, "out-of-range master+relative clamped to 1.0");
        CHECK_NEAR(result[1], 0.0f, "negative relative clamped to 0.0");
    }

    // Caso 7: lista vacía
    {
        auto result = distributeGroupVolume(0.8f, {});
        CHECK(result.empty(), "empty relativeVolumes → empty result");
    }
}

void test_computeRelativeVolume() {
    printf("\n[T6] computeRelativeVolume\n");
    using namespace AudioLogic;

    // device=0.5, master=1.0 → relative=0.5
    CHECK_NEAR(computeRelativeVolume(0.5f, 1.0f), 0.5f,
               "computeRelative(0.5, 1.0) = 0.5");

    // device=0.5, master=0.5 → relative=1.0
    CHECK_NEAR(computeRelativeVolume(0.5f, 0.5f), 1.0f,
               "computeRelative(0.5, 0.5) = 1.0");

    // device=0.25, master=0.5 → relative=0.5
    CHECK_NEAR(computeRelativeVolume(0.25f, 0.5f), 0.5f,
               "computeRelative(0.25, 0.5) = 0.5");

    // master=0 → retorna deviceVolume (sin división por cero)
    CHECK_NEAR(computeRelativeVolume(0.7f, 0.0f), 0.7f,
               "computeRelative(0.7, 0.0) = 0.7 (no div by zero)");

    // Resultado clampado a [0,1]
    CHECK_NEAR(computeRelativeVolume(1.0f, 0.5f), 1.0f,
               "computeRelative(1.0, 0.5) clamped to 1.0");
}

void test_isSilent() {
    printf("\n[T7] isSilent\n");
    using namespace AudioLogic;

    CHECK( isSilent(0.0f),       "isSilent(0.0) = true");
    CHECK( isSilent(1e-6f),      "isSilent(1e-6) = true (below threshold)");
    CHECK(!isSilent(0.01f),      "isSilent(0.01) = false");
    CHECK(!isSilent(1.0f),       "isSilent(1.0) = false");
}

void test_volumeCurve() {
    printf("\n[T8] applyVolumeCurve / volumeToSlider round-trip\n");
    using namespace AudioLogic;

    // La mitad del slider (0.5) con curva cuadrática → 0.25 lineal
    CHECK_NEAR(applyVolumeCurve(0.5f, 2.0f), 0.25f,
               "applyVolumeCurve(0.5, 2.0) = 0.25");

    // Extremos se preservan
    CHECK_NEAR(applyVolumeCurve(0.0f), 0.0f, "applyVolumeCurve(0.0) = 0.0");
    CHECK_NEAR(applyVolumeCurve(1.0f), 1.0f, "applyVolumeCurve(1.0) = 1.0");

    // Round-trip: slider → linear → slider
    float sliderValues[] = { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f };
    for (float s : sliderValues) {
        float linear   = applyVolumeCurve(s, 2.0f);
        float backToS  = volumeToSlider(linear, 2.0f);
        CHECK(nearEq(backToS, s, 1e-4f),
              ("round-trip slider=" + std::to_string(s)).c_str());
    }
}

void test_constants() {
    printf("\n[T9] Constants sanity check\n");
    using namespace AudioLogic;

    CHECK(kSilenceDb     == -96.0f,  "kSilenceDb = -96.0");
    CHECK(kMaxDb         ==   0.0f,  "kMaxDb = 0.0");
    CHECK(kMaxGroupMembers == 4,     "kMaxGroupMembers = 4");
    CHECK(kSilenceLinear  >  0.0f,   "kSilenceLinear > 0");
    CHECK(kSilenceLinear  <  0.001f, "kSilenceLinear < 0.001");
}

// ── Main ──────────────────────────────────────────────────────────────────────

int main() {
    printf("=== AudioLogic Tests (F3) ===\n");

    test_linearToDb();
    test_dbToLinear();
    test_roundtrip();
    test_clampVolume();
    test_distributeGroupVolume();
    test_computeRelativeVolume();
    test_isSilent();
    test_volumeCurve();
    test_constants();

    printf("\n══════════════════════════════════\n");
    printf("Results: %d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
