#include "audio.h"

#include <mmdeviceapi.h>
#include <endpointvolume.h>

namespace {

// RAII simple para interfaces COM, evita dependencias (nada de ATL/WRL).
template <typename T>
class ComPtr {
public:
    ComPtr() = default;
    ~ComPtr() { Release(); }
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;

    T** operator&() { Release(); return &ptr_; }
    T* operator->() const { return ptr_; }
    explicit operator bool() const { return ptr_ != nullptr; }

private:
    void Release() {
        if (ptr_) { ptr_->Release(); ptr_ = nullptr; }
    }
    T* ptr_ = nullptr;
};

// Obtiene IAudioEndpointVolume del mic de captura por defecto.
// Devuelve false si no hay dispositivo o algo falla.
bool GetDefaultMicVolume(ComPtr<IAudioEndpointVolume>& outVolume) {
    ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                  CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                  reinterpret_cast<void**>(&enumerator));
    if (FAILED(hr)) return false;

    ComPtr<IMMDevice> device;
    hr = enumerator->GetDefaultAudioEndpoint(eCapture, eConsole, &device);
    if (FAILED(hr)) return false;  // E_NOTFOUND si no hay mic conectado

    hr = device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
                          reinterpret_cast<void**>(&outVolume));
    return SUCCEEDED(hr);
}

}  // namespace

MicController::MicController() {
    comInitialized_ = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));
}

MicController::~MicController() {
    if (comInitialized_) CoUninitialize();
}

MicState MicController::GetState() {
    ComPtr<IAudioEndpointVolume> volume;
    if (!GetDefaultMicVolume(volume)) return MicState::NoDevice;

    BOOL muted = FALSE;
    if (FAILED(volume->GetMute(&muted))) return MicState::NoDevice;
    return muted ? MicState::Muted : MicState::Unmuted;
}

MicState MicController::Toggle() {
    ComPtr<IAudioEndpointVolume> volume;
    if (!GetDefaultMicVolume(volume)) return MicState::NoDevice;

    BOOL muted = FALSE;
    if (FAILED(volume->GetMute(&muted))) return MicState::NoDevice;

    BOOL newMuted = !muted;
    if (FAILED(volume->SetMute(newMuted, nullptr))) return MicState::NoDevice;
    return newMuted ? MicState::Muted : MicState::Unmuted;
}
