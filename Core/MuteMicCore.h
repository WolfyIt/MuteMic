#pragma once
#include <windows.h>
#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <winrt/Windows.Media.Playback.h>

#include "AudioController.h"
#include "HotkeyHook.h"
#include "SettingsStore.h"
#include "TrayIcon.h"

namespace mutemic {

// Orquestador central. Vive en el hilo de UI: ventana oculta para
// tray/hotkeys/timers + callbacks hacia las ventanas WinUI.
class MuteMicCore {
public:
    static MuteMicCore& Get();

    bool Init();
    void Term();

    // ── Acciones ──
    void ToggleMute();
    void SetMuteExplicit(bool muted);
    void SetDeviceId(const std::wstring& id);   // "" = predeterminado
    void SetServicePaused(bool paused);
    void RequestExit();

    // true durante el teardown: los handlers de UI (render por frame,
    // Closing, Changed) deben no-op para no tocar objetos muriendo.
    static bool Exiting() { return exiting_.load(std::memory_order_relaxed); }
    void PreviewCue();

    // CLI / segunda instancia: 0=show, 1=toggle, 2=mute, 3=unmute.
    void ExecuteVerb(UINT verb);

    // ── Shortcut cards ──
    // Cada Shortcut de settings_.shortcuts es una card. El binding se
    // captura con StartBind: el PRIMER input que llegue (tecla, botón de
    // mouse o botón de gamepad) define tipo y código de la card.
    std::vector<Shortcut>& Shortcuts() { return settings_.shortcuts; }
    size_t AddShortcut();                       // devuelve el índice nuevo
    void RemoveShortcut(size_t idx);
    void RenameShortcut(size_t idx, const std::wstring& name);
    void SetShortcutMode(size_t idx, UINT mode);
    void StartBind(size_t idx);
    void CancelBind();
    void TestShortcut(size_t idx);              // dispara la acción (toggle)
    std::wstring BindingName(const Shortcut& sc) const;
    bool AnyPadConnected() const;
    static std::wstring PadButtonName(UINT bit);

    // Sonido de feedback
    void SetSoundVolume(UINT volume0to100);
    void SetSoundFile(bool muteCue, const std::wstring& fileName);
    void PreviewSound(bool muteCue);
    static std::vector<std::wstring> EnumerateSounds(bool muteCue);

    // ── Estado ──
    Settings& GetSettings() { return settings_; }
    void SaveSettings();
    MicState State();
    float LastLevel() const { return lastLevel_; }
    float PollPeak();
    std::wstring CurrentDeviceName();
    std::wstring HotkeyName() const;   // resumen del primer binding (tooltip)
    bool ServicePaused() const { return servicePaused_; }
    static std::vector<CaptureDevice> EnumerateDevices() { return AudioController::Enumerate(); }

    // ── Callbacks hacia la UI (hilo de UI) ──
    void AddStateListener(std::function<void()> listener);
    std::function<void()> onShortcutsChanged;   // tras capturar/borrar binding
    std::function<void()> onOpenSettings;
    std::function<void(int x, int y)> onTrayFlyout;

private:
    MuteMicCore() = default;

    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT HandleMessage(HWND, UINT, WPARAM, LPARAM);

    void RefreshTray();
    void UpdateMeterTimer();
    std::wstring BuildTooltip();
    void NotifyStateChanged();
    void PlayCue(bool muteCue);
    void CaptureOriginalState();
    void RestoreOriginalState();

    // Re-publica TODOS los bindings de las cards en los hooks y en
    // RegisterHotKey (solo cards de teclado en modo toggle con mods —
    // la vía que funciona sobre apps elevadas).
    void ApplyBindings();
    void DispatchShortcut(size_t idx, bool down);
    void FinishBind();   // post-captura: aplicar, notificar, cancelar resto

    Settings settings_;
    AudioController audio_;
    std::unique_ptr<TrayIcon> tray_;
    HWND hwnd_ = nullptr;
    HANDLE singleInstanceMutex_ = nullptr;

    std::vector<std::function<void()>> stateListeners_;
    winrt::Windows::Media::Playback::MediaPlayer player_{ nullptr };
    std::optional<bool> originalMute_;

    // Captura de binding en curso: índice de la card (-1 = ninguna).
    int bindingIdx_ = -1;
    // Debounce del release en modos hold (teclas macro): card pendiente.
    int pendingReleaseIdx_ = -1;

    // Poll de gamepads (XInput) con backoff en desconectados.
    WORD padPrev_[4] = {};
    bool padConnected_[4] = {};
    ULONGLONG padRecheck_[4] = {};

    bool servicePaused_ = false;
    bool sessionLocked_ = false;
    bool timerRunning_ = false;
    float lastLevel_ = 0.0f;
    UINT taskbarCreatedMsg_ = 0;
    UINT showSettingsMsg_ = 0;
    UINT cmdMsg_ = 0;            // CLI: MuteMic.Cmd
    static inline std::atomic<bool> exiting_{ false };
};

}  // namespace mutemic
