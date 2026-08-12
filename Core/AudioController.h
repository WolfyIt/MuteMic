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
    // Fallo de una operación sobre un endpoint ya adquirido: suelta las
    // interfaces Y arma el backoff, para que un llamador por frame no
    // reintente la adquisición COM a la frecuencia del monitor.
    void FailEndpoint();

    std::wstring deviceId_;           // "" = default
    std::wstring currentName_;
    // El id guardado no existía y se cayó al predeterminado: el core lo
    // consulta para persistir el cambio y no arrastrar un id muerto.
    bool fellBackToDefault_ = false;

public:
    bool ConsumeFellBackToDefault() {
        const bool v = fellBackToDefault_;
        fellBackToDefault_ = false;
        return v;
    }
    const std::wstring& DeviceId() const { return deviceId_; }

private:
    IAudioEndpointVolume* volume_ = nullptr;
    // OPCIONAL: hay endpoints sin medidor. Su ausencia NO es un fallo de
    // adquisición y no debe forzar re-adquirir (ver EnsureEndpoint).
    IAudioMeterInformation* meter_ = nullptr;
    // Backoff tras un fallo de adquisición. Sin esto, un llamador por frame
    // reintenta la enumeración COM 165 veces por segundo.
    ULONGLONG retryAfter_ = 0;
};

}  // namespace mutemic
