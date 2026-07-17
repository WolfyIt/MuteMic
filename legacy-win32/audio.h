#pragma once
#include <windows.h>

// Estado del micrófono por defecto (captura).
enum class MicState {
    Unmuted,
    Muted,
    NoDevice,
};

// Wrapper mínimo de Core Audio para el mic de captura por defecto.
// Re-adquiere el endpoint en cada operación para tolerar
// conexión/desconexión de dispositivos sin mantener referencias colgadas.
class MicController {
public:
    MicController();
    ~MicController();

    MicController(const MicController&) = delete;
    MicController& operator=(const MicController&) = delete;

    // Devuelve el estado actual, o NoDevice si no hay mic.
    MicState GetState();

    // Togglea mute. Devuelve el nuevo estado (NoDevice si falló).
    MicState Toggle();

private:
    bool comInitialized_ = false;
};
