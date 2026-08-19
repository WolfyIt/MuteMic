#pragma once
#include <windows.h>
#include <cmath>
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

// ESCALA PERCEPTUAL DEL MEDIDOR — se resuelve UNA vez, aquí, porque la
// consumen dos sitios (el ícono del tray y la barra de la ventana) y tenerla
// duplicada garantizaba que se desincronizaran.
//
// PORQUÉ EXISTE: IAudioMeterInformation::GetPeakValue devuelve AMPLITUD
// LINEAL 0..1, pero el oído es logarítmico. El umbral del tray estaba en
// 0,04 lineal = -28 dBFS, y el habla de conversación pica entre -35 y -20
// dBFS: el umbral caía justo por encima de la voz normal, así que había que
// gritarle o golpear el micrófono. La barra usaba sqrt(), que es media
// corrección y dejaba un segmento diminuto casi fijo.
//
// Se mapea [-60 dBFS, 0 dBFS] a [0, 1], que es lo que hace cualquier medidor
// de audio real. Referencia: voz suave ~-40 dB -> 0,33; voz normal ~-20 dB
// -> 0,67; grito ~-6 dB -> 0,9.
inline float MeterNormalize(float linearPeak) {
    constexpr float kFloorDb = -60.0f;
    if (linearPeak <= 0.0f) return 0.0f;
    const float db = 20.0f * std::log10(linearPeak);
    if (db <= kFloorDb) return 0.0f;
    return (db - kFloorDb) / -kFloorDb;
}

// Puerta de ruido: por debajo de esto es silencio. 0,25 normalizado =
// -45 dBFS, justo encima del ruido de sala típico y por debajo del habla
// suave.
inline constexpr float kMeterGate = 0.25f;

// Valor LISTO PARA MOSTRAR, 0..1. Aplica la puerta y reescala el resto al
// rango completo, para que la barra no salte de golpe a un cuarto cuando
// empieza la voz.
//
// Es la ÚNICA definición de "cuánto sonido hay": la usan la barra de la
// ventana y la decisión de si el ícono del tray va "activo". Un solo número
// y una sola regla — si el ícono se enciende, la barra se mueve, siempre.
inline float MeterDisplay(float linearPeak) {
    const float n = MeterNormalize(linearPeak);
    if (n <= kMeterGate) return 0.0f;
    return (n - kMeterGate) / (1.0f - kMeterGate);
}

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
