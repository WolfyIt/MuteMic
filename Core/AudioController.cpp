#include "pch.h"
#include "AudioController.h"

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
    ReleaseEndpoint();
}

void AudioController::ReleaseEndpoint() {
    if (volume_) { volume_->Release(); volume_ = nullptr; }
    if (meter_) { meter_->Release(); meter_ = nullptr; }
    currentName_.clear();
}

bool AudioController::EnsureEndpoint() {
    if (volume_ && meter_) return true;
    ReleaseEndpoint();

    ComPtr<IMMDevice> device;
    if (FAILED(GetDevice(deviceId_, &device))) return false;

    if (FAILED(device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
                                reinterpret_cast<void**>(&volume_)))) {
        volume_ = nullptr;
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
        ReleaseEndpoint();  // dispositivo removido: re-adquirir la próxima
        return MicState::NoDevice;
    }
    return muted ? MicState::Muted : MicState::Unmuted;
}

MicState AudioController::Toggle() {
    if (!EnsureEndpoint()) return MicState::NoDevice;
    BOOL muted = FALSE;
    if (FAILED(volume_->GetMute(&muted))) {
        ReleaseEndpoint();
        return MicState::NoDevice;
    }
    BOOL newMuted = !muted;
    if (FAILED(volume_->SetMute(newMuted, nullptr))) {
        ReleaseEndpoint();
        return MicState::NoDevice;
    }
    return newMuted ? MicState::Muted : MicState::Unmuted;
}

bool AudioController::SetMuted(bool muted) {
    if (!EnsureEndpoint()) return false;
    if (FAILED(volume_->SetMute(muted ? TRUE : FALSE, nullptr))) {
        ReleaseEndpoint();
        return false;
    }
    return true;
}

float AudioController::GetPeak() {
    if (!EnsureEndpoint() || !meter_) return 0.0f;
    float peak = 0.0f;
    if (FAILED(meter_->GetPeakValue(&peak))) {
        ReleaseEndpoint();
        return 0.0f;
    }
    return peak;
}

std::wstring AudioController::CurrentDeviceName() {
    EnsureEndpoint();
    return currentName_;
}

}  // namespace mutemic
