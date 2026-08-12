#include "pch.h"
#include "AudioController.h"
#include "Telemetry.h"

#include <initguid.h>  // define PKEY_Device_FriendlyName en este TU
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <functiondiscoverykeys_devpkey.h>

namespace mutemic {
namespace {

template <typename T>
struct ComPtr {
    T* p = nullptr;
    ~ComPtr() { if (p) p->Release(); }
    T** operator&() { return &p; }
    T* operator->() const { return p; }
    explicit operator bool() const { return p != nullptr; }
};

std::wstring FriendlyName(IMMDevice* device) {
    ComPtr<IPropertyStore> props;
    if (FAILED(device->OpenPropertyStore(STGM_READ, &props))) return L"";
    PROPVARIANT var;
    PropVariantInit(&var);
    std::wstring name;
    if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &var)) &&
        var.vt == VT_LPWSTR && var.pwszVal) {
        name = var.pwszVal;
    }
    PropVariantClear(&var);
    return name;
}

HRESULT GetDevice(const std::wstring& id, IMMDevice** outDevice) {
    ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                  CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                  reinterpret_cast<void**>(&enumerator));
    if (FAILED(hr)) return hr;

    if (id.empty())
        return enumerator->GetDefaultAudioEndpoint(eCapture, eConsole, outDevice);
    return enumerator->GetDevice(id.c_str(), outDevice);
}

// Silencio entre reintentos cuando no hay dispositivo. Suficientemente corto
// para que enchufar un micro se note enseguida, suficientemente largo para
// que un llamador a 165 Hz no toque el servicio de audio más de una vez.
constexpr ULONGLONG kRetryMs = 1000;

}  // namespace

AudioController::~AudioController() {
    ReleaseEndpoint();
}

std::vector<CaptureDevice> AudioController::Enumerate() {
    std::vector<CaptureDevice> result;

    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                reinterpret_cast<void**>(&enumerator))))
        return result;

    ComPtr<IMMDeviceCollection> collection;
    if (FAILED(enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &collection)))
        return result;

    UINT count = 0;
    collection->GetCount(&count);
    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> device;
        if (FAILED(collection->Item(i, &device))) continue;

        LPWSTR id = nullptr;
        if (FAILED(device->GetId(&id))) continue;

        CaptureDevice info;
        info.id = id;
        CoTaskMemFree(id);
        info.name = FriendlyName(device.p);
        if (info.name.empty()) info.name = L"Dispositivo de captura";
        result.push_back(std::move(info));
    }
    return result;
}

void AudioController::SetDeviceId(const std::wstring& id) {
    if (deviceId_ == id) return;
    deviceId_ = id;
    // Elección explícita del usuario: cancelar el backoff para que el cambio
    // se sienta inmediato en vez de esperar a que venza el reintento.
    retryAfter_ = 0;
    ReleaseEndpoint();
}

// Una operación sobre un endpoint ya adquirido falló (dispositivo removido,
// otro proceso peleándose el mismo endpoint, servicio de audio saturado).
//
// PORQUÉ EXISTE, en vez de llamar a ReleaseEndpoint directamente: soltar las
// interfaces deja volume_ en null, y la SIGUIENTE llamada vuelve a hacer la
// enumeración COM completa. Con un llamador por frame eso es una tormenta de
// RPC contra el servicio de audio — el mismo que usa el shell — y el
// escritorio entero se vuelve inusable. El backoff de EnsureEndpoint solo
// cubría los fallos AL ADQUIRIR; este es el fallo AL USAR, que entraba por la
// misma puerta sin pagar peaje.
void AudioController::FailEndpoint() {
    ReleaseEndpoint();
    retryAfter_ = GetTickCount64() + kRetryMs;
    // El backoff hace de limitador: como mucho una línea por segundo.
    Telemetry::Event("AUDIO", "endpoint.fail",
                     "operation failed on an acquired endpoint; backing off");
}

void AudioController::ReleaseEndpoint() {
    if (volume_) { volume_->Release(); volume_ = nullptr; }
    if (meter_) { meter_->Release(); meter_ = nullptr; }
    currentName_.clear();
}

// COSTE: esta función hace CoCreateInstance + GetDefaultAudioEndpoint + dos
// Activate. Es una ida y vuelta RPC al servicio de audio de Windows, el mismo
// que usa el shell. Los llamadores incluyen un hook Rendering que corre a la
// frecuencia real del monitor (165 Hz aquí), así que cualquier ruta que no
// haga corto se convierte en una tormenta de RPC que deja inutilizable el
// escritorio entero: barra de tareas muerta, ventanas negras, y CERO CPU
// visible en este proceso porque está bloqueado esperando, no calculando.
//
// Dos condiciones tenían que romperse para que eso pasara, y las dos están
// arregladas aquí:
//
//  1. El corto exigía `volume_ && meter_`. Pero el medidor es OPCIONAL —
//     este mismo archivo lo dice más abajo y sigue adelante sin él. Un
//     endpoint sin medidor dejaba `meter_` en null para siempre, el corto no
//     disparaba nunca, y se re-adquiría en CADA llamada. Ahora el corto mira
//     solo `volume_`, que es lo que define "adquirido".
//  2. No había backoff. Sin dispositivo real, cada fallo reintentaba de
//     inmediato. Ahora un fallo silencia los reintentos 1 s.
bool AudioController::EnsureEndpoint() {
    if (volume_) return true;

    const ULONGLONG now = GetTickCount64();
    if (now < retryAfter_) return false;

    ReleaseEndpoint();

    ComPtr<IMMDevice> device;
    if (FAILED(GetDevice(deviceId_, &device))) {
        // El dispositivo guardado ya no existe: pasa cuando se desconecta,
        // se deshabilita, o tras una actualización de Windows que cambia
        // los IDs de endpoint. Antes esto dejaba la app en "sin
        // dispositivo" para siempre, aunque hubiera micrófonos de sobra.
        // Ahora se cae al predeterminado del sistema y se OLVIDA el id
        // muerto, para no reintentarlo en cada llamada.
        if (deviceId_.empty()) {
            retryAfter_ = now + kRetryMs;
            Telemetry::Event("AUDIO", "acquire.fail", "no default capture device");
            return false;
        }
        deviceId_.clear();
        if (FAILED(GetDevice(deviceId_, &device))) {
            retryAfter_ = now + kRetryMs;
            return false;
        }
        fellBackToDefault_ = true;
    }

    if (FAILED(device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
                                reinterpret_cast<void**>(&volume_)))) {
        volume_ = nullptr;
        retryAfter_ = now + kRetryMs;
        Telemetry::Event("AUDIO", "acquire.fail", "Activate(IAudioEndpointVolume) failed");
        return false;
    }
    if (FAILED(device->Activate(__uuidof(IAudioMeterInformation), CLSCTX_ALL, nullptr,
                                reinterpret_cast<void**>(&meter_)))) {
        meter_ = nullptr;
        // Sin medidor igual podemos mutear; no es fatal.
    }
    currentName_ = FriendlyName(device.p);
    return true;
}

MicState AudioController::GetState() {
    if (!EnsureEndpoint()) return MicState::NoDevice;
    BOOL muted = FALSE;
    if (FAILED(volume_->GetMute(&muted))) {
        FailEndpoint();  // dispositivo removido: re-adquirir tras el backoff
        return MicState::NoDevice;
    }
    return muted ? MicState::Muted : MicState::Unmuted;
}

MicState AudioController::Toggle() {
    if (!EnsureEndpoint()) return MicState::NoDevice;
    BOOL muted = FALSE;
    if (FAILED(volume_->GetMute(&muted))) {
        FailEndpoint();
        return MicState::NoDevice;
    }
    BOOL newMuted = !muted;
    if (FAILED(volume_->SetMute(newMuted, nullptr))) {
        FailEndpoint();
        return MicState::NoDevice;
    }
    return newMuted ? MicState::Muted : MicState::Unmuted;
}

bool AudioController::SetMuted(bool muted) {
    if (!EnsureEndpoint()) return false;
    if (FAILED(volume_->SetMute(muted ? TRUE : FALSE, nullptr))) {
        FailEndpoint();
        return false;
    }
    return true;
}

float AudioController::GetPeak() {
    if (!EnsureEndpoint() || !meter_) return 0.0f;
    float peak = 0.0f;
    if (FAILED(meter_->GetPeakValue(&peak))) {
        FailEndpoint();
        return 0.0f;
    }
    return peak;
}

std::wstring AudioController::CurrentDeviceName() {
    EnsureEndpoint();
    return currentName_;
}

}  // namespace mutemic
