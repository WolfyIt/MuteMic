#pragma once
#include <windows.h>
#include <string>
#include <vector>

struct IAudioEndpointVolume;
struct IAudioMeterInformation;

namespace mutemic {

enum class MicState {
    Unmuted,
    Muted,
    NoDevice,
};

struct CaptureDevice {
    std::wstring id;
    std::wstring name;
};

// Control del endpoint de captura (mute + medidor de nivel).
// Cachea las interfaces y se re-adquiere solo ante error, así el poll del
// medidor (cada ~80 ms) no re-enumera dispositivos constantemente.
class AudioController {
public:
    AudioController() = default;
    ~AudioController();

    AudioController(const AudioController&) = delete;
    AudioController& operator=(const AudioController&) = delete;

    // Lista de dispositivos de captura activos.
    static std::vector<CaptureDevice> Enumerate();

    // Selecciona dispositivo por id ("" = predeterminado del sistema).
    void SetDeviceId(const std::wstring& id);

    MicState GetState();
    MicState Toggle();

    // Fija mute explícitamente (para restaurar el estado original al salir).
    bool SetMuted(bool muted);

    // Pico de nivel 0..1 del dispositivo actual (0 si no hay dispositivo).
    float GetPeak();

    // Nombre amigable del dispositivo actual ("" si no hay).
    std::wstring CurrentDeviceName();

private:
    bool EnsureEndpoint();
    void ReleaseEndpoint();

    std::wstring deviceId_;           // "" = default
    std::wstring currentName_;
    IAudioEndpointVolume* volume_ = nullptr;
    IAudioMeterInformation* meter_ = nullptr;
};

}  // namespace mutemic
