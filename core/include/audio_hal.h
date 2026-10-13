#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

// ── AudioHal ──────────────────────────────────────────────────────────────────
//
// Capa de abstracción del hardware de audio (HAL). Aísla a AudioEngine de la
// API de cada plataforma (CoreAudio en macOS, WASAPI en Windows) y permite
// sustituirla por un mock en los tests.
//
// Los dispositivos se identifican siempre por su UID persistente; cada
// implementación traduce el UID a su identificador nativo.

/**
 * Tipo de conexión del dispositivo. Se usa para elegir el reloj maestro de un
 * grupo (ADR-07): una salida cableada es más estable que una Bluetooth.
 */
enum class HalTransport {
    Unknown,
    BuiltIn,
    USB,
    HDMI,       // HDMI / DisplayPort
    Bluetooth,
    Other,
};

/**
 * Snapshot de un dispositivo de salida tal como lo reporta el sistema.
 */
struct HalDevice {
    std::string uid;
    std::string name;
    float       volume             = 1.0f;  // [0.0 – 1.0]; válido si volume_valid
    bool        volume_valid       = false; // false = el sistema no reporta volumen
    bool        muted              = false;
    bool        has_volume_control = true;  // false = HDMI/DisplayPort, etc.
    HalTransport transport         = HalTransport::Unknown;
};

/**
 * Resultado de aplicar un cambio al hardware.
 */
enum class HalResult {
    Applied,   // El hardware aceptó el cambio
    NoDevice,  // El dispositivo no está conectado (el valor solo se guarda)
    Rejected,  // El dispositivo está conectado pero rechazó el cambio
};

class AudioHal {
public:
    using ChangeCallback = std::function<void()>;

    virtual ~AudioHal() = default;

    /** Lista los dispositivos de salida conectados en este momento. */
    virtual std::vector<HalDevice> listOutputDevices() = 0;

    virtual HalResult setVolume(const std::string& uid, float volume) = 0;
    virtual HalResult setMute(const std::string& uid, bool muted) = 0;

    /**
     * Empieza a observar altas/bajas de dispositivos. El callback puede
     * invocarse desde un hilo del sistema; el receptor debe volver a llamar
     * a listOutputDevices() para obtener el estado nuevo.
     */
    virtual void startWatching(ChangeCallback onDevicesChanged) = 0;
    virtual void stopWatching() = 0;
};

/**
 * HAL de la plataforma actual: CoreAudio en macOS; en el resto, un HAL sin
 * dispositivos (WASAPI llegará en la Iteración 2).
 */
std::unique_ptr<AudioHal> createPlatformAudioHal();
