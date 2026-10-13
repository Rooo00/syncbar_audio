#pragma once

#include <memory>
#include <string>
#include <vector>

// ── AudioRouter ───────────────────────────────────────────────────────────────
//
// Ruteo de audio de los grupos virtuales (ADR-07, fase 1): crea un dispositivo
// de salida que reproduce en todos los miembros a la vez (en macOS, un
// Aggregate Device privado y "stacked") y cambia la salida por defecto del
// sistema. Separado de AudioHal para poder sustituirlo por un mock en tests y
// por otra estrategia (Process Tap, WASAPI loopback) sin tocar el motor.

/**
 * Prefijo del UID de las salidas de grupo que crea la app. AudioHal oculta los
 * dispositivos con este prefijo para que no aparezcan como dispositivos.
 */
inline constexpr const char* kGroupOutputUidPrefix = "com.syncbar.group.";

/**
 * Descripción del dispositivo de salida de un grupo.
 */
struct GroupOutputSpec {
    std::string              name;         // Nombre visible del dispositivo creado
    std::vector<std::string> member_uids;  // Miembros conectados, en orden
    std::string              clock_uid;    // Reloj maestro (uno de member_uids)
};

class AudioRouter {
public:
    virtual ~AudioRouter() = default;

    /** UID de la salida por defecto actual del sistema ("" si no se conoce). */
    virtual std::string currentDefaultOutput() = 0;

    /** Cambia la salida por defecto del sistema. */
    virtual bool setDefaultOutput(const std::string& uid) = 0;

    /**
     * Crea el dispositivo de salida del grupo.
     * @return UID del dispositivo creado, o "" si falló.
     */
    virtual std::string createGroupOutput(const GroupOutputSpec& spec) = 0;

    /** Destruye un dispositivo creado con createGroupOutput(). */
    virtual void destroyGroupOutput(const std::string& outputUid) = 0;
};

/**
 * Router de la plataforma actual: Aggregate Device en macOS; fuera de macOS
 * (hasta la Iteración 2) un router nulo que no puede crear salidas de grupo.
 */
std::unique_ptr<AudioRouter> createPlatformAudioRouter();
