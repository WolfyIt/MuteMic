#include "pch.h"
#include "MuteMicCore.h"

#include <psapi.h>
#include <wtsapi32.h>
#include <xinput.h>

#pragma comment(lib, "psapi.lib")

#include <winrt/Windows.Media.Core.h>

#include "Autostart.h"
#include "IconRenderer.h"
#include "LiquidGlassBackdrop.h"
#include "NativeFlyout.h"
#include "Telemetry.h"
#include "VisualCue.h"

namespace mutemic {
namespace {

constexpr wchar_t kWindowClass[] = L"MuteMicCoreWindow";
constexpr wchar_t kMutexName[] = L"Local\\MuteMic_SingleInstance";
// Mutex PROPIO del proceso de ajustes. Existe para que CUALQUIER daemon
// —incluido uno recién arrancado— pueda saber si ya hay una ventana viva
// sin depender de su propio `settingsUiRunning_`, que se pierde al reiniciar.
// Sin esto: se hace Quit en el tray, el proceso de ajustes sobrevive, se
// reabre la app y al pedir ajustes salen DOS ventanas — la del huérfano que
// responde al broadcast, y una nueva que el daemon crea porque no sabe que
// el huérfano existe.
constexpr wchar_t kSettingsUiMutex[] = L"Local\\MuteMic_SettingsUI";
constexpr UINT_PTR kMeterTimerId = 1;
constexpr UINT_PTR kHoldReleaseTimerId = 3;
constexpr UINT_PTR kPadTimerId = 4;
constexpr UINT_PTR kTrimTimerId = 5;   // recorte de memoria al ir al tray
constexpr UINT_PTR kUiWatchTimerId = 6;  // vigila el proceso de ajustes
constexpr UINT_PTR kHideExitTimerId = 7; // ventana oculta -> terminar proceso
constexpr UINT_PTR kCuePlayerTimerId = 8; // soltar el MediaPlayer en reposo

// El MediaPlayer del cue de sonido era el ÚNICO de los cinco subsistemas sin
// enfriamiento. Medido: crearlo cuesta 0,6 MB, pero reproducir arrastra el
// pipeline de Media Foundation, y eso no volvía nunca. Es el residuo que
// dejaba al daemon en ~9 MB en vez de volver a ~6 tras un cue.
// 10 s: más que el enfriamiento de los gráficos, porque encadenar
// mute/unmute es normal y recrear el reproductor tiene latencia audible.
constexpr UINT kCuePlayerIdleMs = 10000;

// Gracia antes de matar el proceso de UI cuando la ventana se oculta.
// MEDIDO: reabrir la ventana cuesta 374 ms; mantenerla "caliente" cuesta
// ~172 MB de private bytes residentes. No compensa. 15 s cubren el caso de
// minimizar y restaurar enseguida sin pagar el resto del día.
constexpr UINT kHideExitMs = 15000;

// Definida más abajo (junto al resto de helpers); se usa en HandleMessage.
void TrimWorkingSet(const char* reason);

// Red de seguridad de salida del proceso de ajustes. Ver RequestExit.
void CALLBACK ForcedExitProc(HWND, UINT, UINT_PTR id, DWORD) {
    KillTimer(nullptr, id);
    Telemetry::Event("APP", "exit.forced",
                     "Application::Exit did not end the message loop");
    Telemetry::Shutdown("settings-forced");
    ExitProcess(0);
}
// Frecuencia del medidor que alimenta el ÍCONO DEL TRAY (16x16 px) y la
// sincronización de estado. NO es el medidor de la ventana: ese va por el
// hook de render, aparte.
//
// Estaba en 33 ms = 30 despertares por segundo, veinticuatro horas al día,
// para decidir el color de un ícono diminuto cuyo umbral de "hay voz" es
// binario. A 250 ms son 4, y visualmente no se distingue. Es el cambio de
// mayor relación beneficio/riesgo del proyecto: una constante.
//
// Provisional: cuando entre IAudioEndpointVolumeCallback, el ESTADO pasa a
// ser por evento y este timer solo tendrá que cubrir el nivel de voz.
constexpr UINT kMeterIntervalMs = 250;
constexpr UINT kPadIntervalMs = 30;
// Debounce del "soltar" en modos hold: las teclas macro (NZXT, Synapse F14)
// no mantienen el key-down — repiten pares down+up.
constexpr UINT kHoldReleaseMs = 170;
// El umbral vive ahora en AudioController.h (kMeterActive), en la escala
// perceptual y compartido con la barra de la ventana.
// RegisterHotKey por card: id = base + índice.
constexpr int kRegisteredHotkeyBase = 100;

TrayFace FaceFor(MicState state, float level, bool paused) {
    if (paused || state == MicState::NoDevice) return TrayFace::NoDevice;
    const bool active = MeterDisplay(level) > 0.0f;
    if (state == MicState::Muted)
        return active ? TrayFace::RedActive : TrayFace::RedIdle;
    return active ? TrayFace::GreenActive : TrayFace::GreenIdle;
}

std::wstring ExeDir() {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring dir = path;
    const size_t slash = dir.find_last_of(L'\\');
    return (slash == std::wstring::npos) ? dir : dir.substr(0, slash);
}

std::wstring SoundDir(bool muteCue) {
    return ExeDir() + (muteCue ? L"\\Sounds\\mute" : L"\\Sounds\\unmute");
}

}  // namespace

MuteMicCore& MuteMicCore::Get() {
    static MuteMicCore instance;
    return instance;
}

bool MuteMicCore::Init(Mode mode) {
    mode_ = mode;
    const UINT showMsg = RegisterWindowMessageW(L"MuteMic.ShowSettings");

    // El MUTEX de instancia única lo posee el DAEMON. El proceso de UI es
    // efímero y convive con él: no compite por el mutex ni se auto-cierra.
    if (mode == Mode::Daemon) {
        singleInstanceMutex_ = CreateMutexW(nullptr, TRUE, kMutexName);
        if (!singleInstanceMutex_ || GetLastError() == ERROR_ALREADY_EXISTS) {
            PostMessageW(HWND_BROADCAST, showMsg, 0, 0);
            return false;
        }
    }
    showSettingsMsg_ = showMsg;
    bringToFrontMsg_ = RegisterWindowMessageW(L"MuteMic.BringSettingsToFront");
    settingsChangedMsg_ = RegisterWindowMessageW(L"MuteMic.SettingsChanged");
    playCueMsg_ = RegisterWindowMessageW(L"MuteMic.PlayCue");
    quitMsg_ = RegisterWindowMessageW(L"MuteMic.Quit");
    cmdMsg_ = RegisterWindowMessageW(L"MuteMic.Cmd");

    IconRenderer::Startup();
    settings_ = SettingsStore::Load();
    audio_.SetDeviceId(settings_.deviceId);
    CaptureOriginalState();

    taskbarCreatedMsg_ = RegisterWindowMessageW(L"TaskbarCreated");

    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kWindowClass;
    RegisterClassW(&wc);

    hwnd_ = CreateWindowExW(0, kWindowClass, L"MuteMic", 0, 0, 0, 0, 0,
                            nullptr, nullptr, GetModuleHandleW(nullptr), this);
    if (!hwnd_) return false;

    WTSRegisterSessionNotification(hwnd_, NOTIFY_FOR_THIS_SESSION);

    // Scancode faltante en cards de teclado migradas.
    for (auto& sc : settings_.shortcuts)
        if (sc.type == 0 && sc.vk != 0 && sc.scan == 0)
            sc.scan = MapVirtualKeyW(sc.vk, MAPVK_VK_TO_VSC);

    // ── Solo el DAEMON posee tray, hooks y bindings globales ──
    // Si el proceso de UI también los instalara, cada tecla se procesaría
    // dos veces y habría dos iconos en la bandeja.
    if (mode == Mode::Daemon) {
        tray_ = std::make_unique<TrayIcon>(hwnd_);
        tray_->Add(FaceFor(State(), 0.0f, false), 0.0f, BuildTooltip());

        if (HotkeyHook::Install(hwnd_)) {
            tray_->ShowBalloon(L"MuteMic is running",
                               (L"Shortcut: " + HotkeyName() +
                                L". Right-click the tray icon for the menu.").c_str());
        } else {
            tray_->ShowBalloon(L"MuteMic — shortcut unavailable",
                               L"Couldn't install the keyboard hook. "
                               L"You can still toggle from the tray.");
        }
        ApplyBindings();
    } else {
        // El proceso de ajustes NO instala hooks permanentes (eso duplicaría
        // el procesamiento de cada atajo), pero sí tiene que poder RECIBIR
        // WM_APP_CAPTURED cuando el usuario remapea. Sin este target el
        // hook temporal de BeginCapture postearía a nullptr y la captura no
        // terminaría nunca.
        HotkeyHook::SetTarget(hwnd_);
        // Señal de "hay una ventana de ajustes viva" legible por cualquier
        // daemon. Se libera sola al morir el proceso.
        settingsUiMutex_ = CreateMutexW(nullptr, TRUE, kSettingsUiMutex);
    }

    if (mode == Mode::Daemon) {
        SetTimer(hwnd_, kPadTimerId, kPadIntervalMs, nullptr);

        // Raw Input del teclado en segundo plano (RIDEV_INPUTSINK). Aporta
        // los eventos de SOLTAR tecla que RegisterHotKey no manda, útiles
        // para push-to-talk. (Medido: no atraviesa UIPI, igual que las
        // otras vías — ver vault/ATTEMPTS.md.) RIDEV_NOLEGACY NO se usa:
        // suprimiría los mensajes de teclado normales de TODO el sistema.
        RAWINPUTDEVICE rid{};
        rid.usUsagePage = 0x01;   // Generic Desktop
        rid.usUsage = 0x06;       // Keyboard
        rid.dwFlags = RIDEV_INPUTSINK;
        rid.hwndTarget = hwnd_;
        const bool ok = RegisterRawInputDevices(&rid, 1, sizeof(rid)) != FALSE;
        char buf[64];
        sprintf_s(buf, "ok=%d err=%lu", ok ? 1 : 0, ok ? 0UL : GetLastError());
        Telemetry::Event("HOTKEY", "rawinput.register", buf);
    }

    UpdateMeterTimer();
    Telemetry::Event("APP", "core.init",
                     mode == Mode::Daemon ? "mode=daemon" : "mode=settings-ui");
    return true;
}

// Lanza el proceso EFÍMERO de ajustes. Es el mismo ejecutable con otra
// bandera: un solo binario que distribuir, y el proceso pesado solo existe
// mientras la ventana está abierta.
void MuteMicCore::OpenSettings() {
    if (mode_ == Mode::Daemon)
        LaunchSettingsProcess();
    else if (onOpenSettings)
        onOpenSettings();
}

// BUCLE DE BROADCAST — el fallo más caro de este proyecto. Qué pasaba:
//
//   LaunchSettingsProcess()
//     -> PostMessage(HWND_BROADCAST, showSettingsMsg_)
//          -> llega a la ventana oculta de ESTE MISMO daemon
//               -> WndProc: msg == showSettingsMsg_ -> OpenSettings()
//                    -> mode_ == Daemon -> LaunchSettingsProcess()  [bucle]
//
// HWND_BROADCAST alcanza TODAS las ventanas de nivel superior del escritorio,
// incluidas las nuestras. El guard `settingsUiRunning_` protegía el
// CreateProcess pero estaba DESPUÉS del broadcast, así que no frenaba el
// ciclo. Cada vuelta encolaba un mensaje en cada ventana de cada aplicación
// de la sesión: escritorio congelado, barra de tareas muerta, y cero CPU
// visible en MuteMic porque el coste lo pagaban los DEMÁS procesos vaciando
// sus colas.
//
// La causa de fondo era semántica: un mismo mensaje significaba "abre los
// ajustes" para el daemon y "traete al frente" para la ventana. Dos
// destinatarios, dos significados, un solo identificador. Ahora son dos
// mensajes, y el que se emite aquí el daemon lo ignora por construcción.
bool MuteMicCore::LaunchSettingsProcess() {
    // Si ya hay una ventana de ajustes abierta, que se traiga al frente
    // en vez de abrir otra. Este mensaje SOLO lo atiende un proceso de UI.
    PostMessageW(HWND_BROADCAST, bringToFrontMsg_, 0, 0);
    if (settingsUiRunning_) return true;
    // Puede haber una ventana viva que ESTE daemon no lanzó (quedó huérfana
    // de una instancia anterior). El broadcast de arriba ya la trae al
    // frente; crear otra encima es lo que producía dos ventanas.
    if (HANDLE existing = OpenMutexW(SYNCHRONIZE, FALSE, kSettingsUiMutex)) {
        CloseHandle(existing);
        Telemetry::Event("APP", "settings.launch",
                         "ok=1 reused an existing settings window");
        return true;
    }

    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring cmd = std::wstring(L"\"") + exe + L"\" --settings";

    STARTUPINFOW si{ sizeof(si) };
    PROCESS_INFORMATION pi{};
    const bool ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE,
                                   0, nullptr, nullptr, &si, &pi) != FALSE;
    if (ok) {
        CloseHandle(pi.hThread);
        if (settingsProcess_) CloseHandle(settingsProcess_);
        settingsProcess_ = pi.hProcess;
        settingsUiRunning_ = true;
        // Vigilar su muerte para saber cuándo vuelve a estar cerrada.
        SetTimer(hwnd_, kUiWatchTimerId, 1000, nullptr);
    }
    char buf[64];
    sprintf_s(buf, "ok=%d err=%lu", ok ? 1 : 0, ok ? 0UL : GetLastError());
    Telemetry::Event("APP", "settings.launch", buf);
    return ok;
}

void MuteMicCore::Term() {
    RestoreOriginalState();

    if (hwnd_) {
        KillTimer(hwnd_, kMeterTimerId);
        KillTimer(hwnd_, kPadTimerId);
        KillTimer(hwnd_, kHoldReleaseTimerId);
        KillTimer(hwnd_, kTrimTimerId);
        WTSUnRegisterSessionNotification(hwnd_);
        for (size_t i = 0; i < settings_.shortcuts.size(); ++i)
            UnregisterHotKey(hwnd_, kRegisteredHotkeyBase + static_cast<int>(i));
    }
    NativeFlyout::Term();
    VisualCue::Term();
    HotkeyHook::UninstallMouse();
    HotkeyHook::Uninstall();
    tray_.reset();
    if (cueSource_) { try { cueSource_.Close(); } catch (...) {} cueSource_ = nullptr; }
    if (player_) { player_.Close(); player_ = nullptr; }
    if (hwnd_) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    IconRenderer::Shutdown();
    if (settingsUiMutex_) {
        // Soltarlo explícitamente: mientras exista, cualquier daemon cree
        // que hay una ventana viva y se niega a abrir una nueva.
        CloseHandle(settingsUiMutex_);
        settingsUiMutex_ = nullptr;
    }
    if (singleInstanceMutex_) {
        CloseHandle(singleInstanceMutex_);
        singleInstanceMutex_ = nullptr;
    }
}

// ─────────────────────────────────────────────────────────────────────────────

LRESULT CALLBACK MuteMicCore::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    MuteMicCore* self = nullptr;
    if (msg == WM_CREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<MuteMicCore*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<MuteMicCore*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (self) return self->HandleMessage(hwnd, msg, wParam, lParam);
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void MuteMicCore::DispatchShortcut(size_t idx, bool down) {
    if (servicePaused_ || idx >= settings_.shortcuts.size()) return;

    // DEDUPE entre vías de entrada. Una misma tecla puede llegar por hasta
    // tres caminos a la vez (RegisterHotKey, hook de bajo nivel, raw
    // input); sin esto, un solo toque muteaba y desmuteaba al instante.
    // La primera vía que llegue gana; las demás se ignoran en la ventana.
    const ULONGLONG now = GetTickCount64();
    const size_t slot = idx % kMaxDedupe;
    if (down) {
        if (now - lastDownMs_[slot] < kDedupeMs) return;
        lastDownMs_[slot] = now;
    } else {
        if (now - lastUpMs_[slot] < kDedupeMs) return;
        lastUpMs_[slot] = now;
    }

    const UINT mode = settings_.shortcuts[idx].mode;

    if (down) {
        // Down dentro de la ventana de debounce = pulsación continua.
        if (pendingReleaseIdx_ == static_cast<int>(idx)) {
            KillTimer(hwnd_, kHoldReleaseTimerId);
            pendingReleaseIdx_ = -1;
        }
        switch (mode) {
            case 0: ToggleMute(); break;
            case 1: SetMuteExplicit(false); break;  // PTT: abrir mic
            case 2: SetMuteExplicit(true); break;   // PTM: silenciar
        }
    } else if (mode != 0) {
        // Release diferido (absorbe los up fantasma de teclas macro).
        pendingReleaseIdx_ = static_cast<int>(idx);
        SetTimer(hwnd_, kHoldReleaseTimerId, kHoldReleaseMs, nullptr);
    }
}

LRESULT MuteMicCore::HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case HotkeyHook::WM_APP_HOTKEY:
            DispatchShortcut(static_cast<size_t>(wParam), true);
            return 0;

        case HotkeyHook::WM_APP_HOTKEY_UP:
            DispatchShortcut(static_cast<size_t>(wParam), false);
            return 0;

        case WM_INPUT: {
            // ── Tercera vía de entrada: Raw Input con RIDEV_INPUTSINK ──
            // Llega por la cola de raw input, que es DISTINTA de los
            // mensajes de ventana y de los hooks. UIPI bloquea aquellos dos
            // sobre ventanas elevadas; sobre esta no está documentado que lo
            // haga. Se registra el proceso en foco en cada pulsación para
            // comprobarlo con datos y no con suposiciones.
            UINT size = 0;
            GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam), RID_INPUT,
                            nullptr, &size, sizeof(RAWINPUTHEADER));
            if (size == 0 || size > 256) break;
            BYTE buf[256];
            if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam), RID_INPUT,
                                buf, &size, sizeof(RAWINPUTHEADER)) != size)
                break;
            auto* raw = reinterpret_cast<RAWINPUT*>(buf);
            if (raw->header.dwType != RIM_TYPEKEYBOARD) break;

            const USHORT vk = raw->data.keyboard.VKey;
            const bool up = (raw->data.keyboard.Flags & RI_KEY_BREAK) != 0;
            if (vk == 0 || vk == 0xFF) break;   // teclas fantasma del HID

            for (size_t i = 0; i < settings_.shortcuts.size(); ++i) {
                auto const& sc = settings_.shortcuts[i];
                if (sc.type != 0 || !sc.Bound() || sc.vk != vk) continue;
                // Los modificadores se comprueban con el estado actual: raw
                // input entrega cada tecla suelta, sin combinar.
                if (sc.mods) {
                    const bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
                    const bool alt = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
                    const bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
                    const bool win = ((GetAsyncKeyState(VK_LWIN) |
                                       GetAsyncKeyState(VK_RWIN)) & 0x8000) != 0;
                    if (((sc.mods & MOD_CONTROL) != 0) != ctrl) continue;
                    if (((sc.mods & MOD_ALT) != 0) != alt) continue;
                    if (((sc.mods & MOD_SHIFT) != 0) != shift) continue;
                    if (((sc.mods & MOD_WIN) != 0) != win) continue;
                }

                if (Telemetry::Enabled()) {
                    wchar_t fgExe[MAX_PATH] = L"?";
                    if (HWND fg = GetForegroundWindow()) {
                        DWORD pid = 0;
                        GetWindowThreadProcessId(fg, &pid);
                        if (HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,
                                                   FALSE, pid)) {
                            DWORD n = MAX_PATH;
                            QueryFullProcessImageNameW(p, 0, fgExe, &n);
                            CloseHandle(p);
                        }
                    }
                    const wchar_t* base = wcsrchr(fgExe, L'\\');
                    char exeUtf8[128] = "?";
                    WideCharToMultiByte(CP_UTF8, 0, base ? base + 1 : fgExe, -1,
                                        exeUtf8, sizeof(exeUtf8), nullptr, nullptr);
                    char msg[192];
                    sprintf_s(msg, "idx=%zu vk=0x%02X up=%d fg=%s", i, vk,
                              up ? 1 : 0, exeUtf8);
                    Telemetry::Event("HOTKEY", "rawinput.recv", msg);
                }

                // Filtrar auto-repetición: solo interesan los FLANCOS.
                const size_t slot = i % kMaxDedupe;
                if (!up) {
                    if (rawKeyDown_[slot]) break;   // repetición, ignorar
                    rawKeyDown_[slot] = true;
                } else {
                    rawKeyDown_[slot] = false;
                }

                DispatchShortcut(i, !up);
                break;
            }
            break;   // DefWindowProc debe ver WM_INPUT para su limpieza
        }

        case WM_HOTKEY: {
            // Vía RegisterHotKey (cards de teclado en modo toggle). Esta es
            // la ruta que SÍ atraviesa UIPI: si el mensaje llega estando el
            // Administrador de tareas en foco, el atajo funciona ahí.
            const int idx = static_cast<int>(wParam) - kRegisteredHotkeyBase;
            const bool valid =
                idx >= 0 && static_cast<size_t>(idx) < settings_.shortcuts.size();
            {
                // QUIÉN tenía el foco al llegar el atajo. Sin esto no se
                // puede distinguir "no llegó sobre una app elevada" de
                // "llegó y muteó, pero no se vio" — que es justo la duda.
                wchar_t fgExe[MAX_PATH] = L"?";
                DWORD fgPid = 0;
                if (HWND fg = GetForegroundWindow()) {
                    GetWindowThreadProcessId(fg, &fgPid);
                    if (HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,
                                               FALSE, fgPid)) {
                        DWORD n = MAX_PATH;
                        QueryFullProcessImageNameW(p, 0, fgExe, &n);
                        CloseHandle(p);
                    }
                }
                const wchar_t* base = wcsrchr(fgExe, L'\\');
                char exeUtf8[128] = "?";
                WideCharToMultiByte(CP_UTF8, 0, base ? base + 1 : fgExe, -1,
                                    exeUtf8, sizeof(exeUtf8), nullptr, nullptr);

                char buf[256];
                sprintf_s(buf, "idx=%d valid=%d paused=%d mode=%u fg=%s pid=%lu",
                          idx, valid ? 1 : 0, servicePaused_ ? 1 : 0,
                          valid ? settings_.shortcuts[idx].mode : 99u,
                          exeUtf8, fgPid);
                Telemetry::Event("HOTKEY", "wm_hotkey.recv", buf);
            }
            if (valid && !servicePaused_ && settings_.shortcuts[idx].mode == 0) {
                ToggleMute();
                // Resultado REAL del toggle: si el estado cambió, el atajo
                // funcionó aunque no se haya visto ningún indicador.
                char buf[64];
                sprintf_s(buf, "muted=%d", State() == MicState::Muted ? 1 : 0);
                Telemetry::Event("HOTKEY", "toggle.result", buf);
            }
            return 0;
        }

        case HotkeyHook::WM_APP_CAPTURED: {
            // Resultado de captura de TECLADO.
            if (bindingIdx_ >= 0 &&
                static_cast<size_t>(bindingIdx_) < settings_.shortcuts.size()) {
                const UINT vk = static_cast<UINT>(wParam);
                if (vk != 0) {
                    auto& sc = settings_.shortcuts[bindingIdx_];
                    sc.type = 0;
                    sc.vk = vk;
                    sc.mods = LOWORD(lParam);
                    sc.scan = HIWORD(lParam);
                    sc.code = 0;
                }
                FinishBind();
            }
            return 0;
        }

        case HotkeyHook::WM_APP_MOUSE_CAPTURED: {
            if (bindingIdx_ >= 0 &&
                static_cast<size_t>(bindingIdx_) < settings_.shortcuts.size()) {
                auto& sc = settings_.shortcuts[bindingIdx_];
                sc.type = 1;
                sc.code = static_cast<UINT>(wParam);
                sc.vk = sc.scan = sc.mods = 0;
                FinishBind();
            }
            return 0;
        }

        case WM_TIMER:
            if (wParam == kUiWatchTimerId) {
                // ¿Murió el proceso de ajustes? Entonces sus ~160 MB ya
                // volvieron al sistema, y hay que recargar los ajustes que
                // haya guardado.
                if (settingsProcess_ &&
                    WaitForSingleObject(settingsProcess_, 0) == WAIT_OBJECT_0) {
                    CloseHandle(settingsProcess_);
                    settingsProcess_ = nullptr;
                    settingsUiRunning_ = false;
                    KillTimer(hwnd, kUiWatchTimerId);
                    settings_ = SettingsStore::Load();
                    audio_.SetDeviceId(settings_.deviceId);
                    ApplyBindings();
                    RefreshTray();
                    UpdateMeterTimer();
                    Telemetry::Event("APP", "settings.closed", "settings reloaded");
                }
            } else if (wParam == kCuePlayerTimerId) {
                KillTimer(hwnd, kCuePlayerTimerId);
                if (cueSource_) {
                    try { cueSource_.Close(); } catch (...) {}
                    cueSource_ = nullptr;
                    cueSourceFile_.clear();
                }
                if (player_) {
                    try { player_.Close(); } catch (...) {}
                    player_ = nullptr;
                    Telemetry::Memory("PERF", "subsystem", "mediaplayer.released");
                }
                // Y de paso devolver el working set: el daemon no lo recorta
                // nunca (el timer de trim solo se arma al ocultar la ventana,
                // que en el daemon no ocurre). Sin esto la memoria baja a
                // saltos y a destiempo, cuando Windows decide.
                TrimWorkingSet("cue-idle");
            } else if (wParam == kHideExitTimerId) {
                KillTimer(hwnd, kHideExitTimerId);
                Telemetry::Event("APP", "hidden.exit",
                                 "settings window hidden past grace; releasing process");
                RequestExit();
            } else if (wParam == kTrimTimerId) {
                TrimWorkingSet("tray-idle");
                // Foto del árbol de sondas. Se engancha a un timer que YA
                // existía: observarse no debe costar un despertar propio.
                Probes::Snapshot("tray-idle");
                // Re-armar a intervalo largo mientras siga oculta. Barato
                // (una llamada cada 45 s) y mantiene la app en su mínimo en
                // vez de dejarla subir sola.
                KillTimer(hwnd, kTrimTimerId);
                SetTimer(hwnd, kTrimTimerId, 45000, nullptr);
            } else if (wParam == kMeterTimerId) {
                lastLevel_ = audio_.GetPeak();

                // SINCRONIZACIÓN DE ESTADO ENTRE PROCESOS.
                //
                // NotifyStateChanged() solo se dispara cuando ESTE proceso
                // cambia el mute. Tras el split en daemon, quien mutea es el
                // daemon (atajo o tray) y quien muestra el estado es el
                // proceso de ajustes — que nunca se enteraba y se quedaba en
                // "Mic Live" para siempre.
                //
                // No hace falta IPC: ambos procesos leen el MISMO endpoint de
                // Core Audio, que es la única fuente de verdad. Basta con
                // mirar si cambió y avisar. El timer ya existía y ya llamaba
                // a GetPeak, así que esto no agrega ni un despertar.
                if (const MicState now = State(); now != lastNotifiedState_) {
                    lastNotifiedState_ = now;
                    NotifyStateChanged();
                }
                // ¿Se cayó al predeterminado porque el dispositivo guardado
                // desapareció? Persistirlo: si no, el id muerto se queda en
                // el registro y la UI sigue mostrando un micro inexistente.
                if (audio_.ConsumeFellBackToDefault()) {
                    settings_.deviceId.clear();
                    SaveSettings();
                    Telemetry::Event("AUDIO", "device.fallback",
                                     "saved device gone, using system default");
                }
                RefreshTray();
            } else if (wParam == kPadTimerId) {
                const ULONGLONG now = GetTickCount64();
                for (DWORD i = 0; i < 4; ++i) {
                    if (!padConnected_[i] && now < padRecheck_[i]) continue;
                    XINPUT_STATE st{};
                    if (XInputGetState(i, &st) != ERROR_SUCCESS) {
                        padConnected_[i] = false;
                        padPrev_[i] = 0;
                        padRecheck_[i] = now + 3000;
                        continue;
                    }
                    padConnected_[i] = true;
                    const WORD btns = st.Gamepad.wButtons;
                    const WORD downEdges = btns & ~padPrev_[i];
                    const WORD upEdges = padPrev_[i] & ~btns;
                    padPrev_[i] = btns;

                    if (bindingIdx_ >= 0 && downEdges != 0) {
                        // Captura por GAMEPAD: bit más bajo presionado.
                        auto& sc = settings_.shortcuts[bindingIdx_];
                        sc.type = 2;
                        sc.code = downEdges & (~downEdges + 1);
                        sc.vk = sc.scan = sc.mods = 0;
                        FinishBind();
                        continue;
                    }
                    if (downEdges || upEdges) {
                        for (size_t s = 0; s < settings_.shortcuts.size(); ++s) {
                            auto const& sc = settings_.shortcuts[s];
                            if (sc.type != 2 || sc.code == 0) continue;
                            if (downEdges & sc.code) DispatchShortcut(s, true);
                            if (upEdges & sc.code) DispatchShortcut(s, false);
                        }
                    }
                }
            } else if (wParam == kHoldReleaseTimerId) {
                KillTimer(hwnd_, kHoldReleaseTimerId);
                const int idx = pendingReleaseIdx_;
                pendingReleaseIdx_ = -1;
                if (!servicePaused_ && idx >= 0 &&
                    static_cast<size_t>(idx) < settings_.shortcuts.size()) {
                    switch (settings_.shortcuts[idx].mode) {
                        case 1: SetMuteExplicit(true); break;   // PTT: mute
                        case 2: SetMuteExplicit(false); break;  // PTM: abrir
                    }
                }
            }
            return 0;

        case WM_TRAYICON:
            switch (LOWORD(lParam)) {
                case WM_LBUTTONUP:
                    ToggleMute();
                    break;
                case WM_RBUTTONUP: {
                    POINT pt;
                    GetCursorPos(&pt);
                    // Flyout NATIVO: vive en el núcleo Win32, sin XAML.
                    // Es la superficie de uso diario, así que no puede ser
                    // la que obligue a cargar el runtime pesado.
                    if (ShowNativeFlyout(pt.x, pt.y)) {
                        // servido
                    } else if (onTrayFlyout) {
                        onTrayFlyout(pt.x, pt.y);
                    } else {
                        UINT cmd = tray_->ShowMenu(IsAutostartEnabled(), servicePaused_);
                        switch (cmd) {
                            case IDM_OPEN:
                                OpenSettings();
                                break;
                            case IDM_TOGGLE_MUTE:
                                ToggleMute();
                                break;
                            case IDM_AUTOSTART:
                                SetAutostart(!IsAutostartEnabled());
                                break;
                            case IDM_PAUSE_SERVICE:
                                SetServicePaused(!servicePaused_);
                                break;
                            case IDM_EXIT:
                                RequestExit();
                                break;
                        }
                    }
                    break;
                }
            }
            return 0;

        case WM_WTSSESSION_CHANGE:
            if (wParam == WTS_SESSION_LOCK) sessionLocked_ = true;
            else if (wParam == WTS_SESSION_UNLOCK) sessionLocked_ = false;
            UpdateMeterTimer();
            return 0;

        case WM_QUERYENDSESSION:
            // Apagado/logoff del sistema: responder TRUE RÁPIDO. Sin esto
            // (y con el Closing de la ventana cancelando cierres) Windows
            // se quedaba esperando y el apagado tardaba minutos.
            exiting_ = true;
            return TRUE;

        case WM_ENDSESSION:
            if (wParam) {
                // El sistema SE VA a apagar: lo único crítico es devolver
                // el mic a su estado original y soltar los hooks — rápido.
                exiting_ = true;
                RestoreOriginalState();
                HotkeyHook::UninstallMouse();
                HotkeyHook::Uninstall();
            }
            return 0;

        default:
            if (msg == taskbarCreatedMsg_ && tray_) {
                tray_->ReAdd();
                RefreshTray();
                return 0;
            }
            // El daemon se está cerrando: la ventana de ajustes también.
            // Solo lo atiende un proceso de UI, y se ignora el propio.
            if (quitMsg_ != 0 && msg == quitMsg_) {
                if (mode_ == Mode::SettingsUi &&
                    wParam != static_cast<WPARAM>(GetCurrentProcessId())) {
                    Telemetry::Event("APP", "quit.follow", "daemon is exiting");
                    RequestExit();
                }
                return 0;
            }
            // El proceso de ajustes pide que suene un cue. SOLO lo atiende
            // el daemon: así la única sesión de audio del mezclador es la
            // suya. wParam = pid del emisor, lParam = 1 si es cue de mute.
            if (playCueMsg_ != 0 && msg == playCueMsg_) {
                if (mode_ == Mode::Daemon &&
                    wParam != static_cast<WPARAM>(GetCurrentProcessId()))
                    PlayCue(lParam != 0);
                return 0;
            }
            // Otro proceso guardó los ajustes: recargar. El pid del emisor
            // viaja en wParam; ignorar el propio es lo que impide que un
            // broadcast se realimente (ver SaveSettings).
            if (settingsChangedMsg_ != 0 && msg == settingsChangedMsg_) {
                if (wParam != static_cast<WPARAM>(GetCurrentProcessId()))
                    ReloadSettings();
                return 0;
            }
            // "Traete al frente": SOLO para un proceso de UI. Si el daemon
            // lo atendiera volvería a emitirlo y se cerraría el bucle otra
            // vez — es exactamente el fallo que este par de mensajes separa.
            if (bringToFrontMsg_ != 0 && msg == bringToFrontMsg_) {
                if (mode_ == Mode::SettingsUi && onOpenSettings) onOpenSettings();
                return 0;
            }
            // "Abre los ajustes": lo emite una SEGUNDA invocación del exe que
            // encuentra el mutex tomado y se muere acto seguido (ver Init).
            // Solo el daemon lo atiende, y el emisor ya no existe para
            // recibir la respuesta, así que no hay ciclo posible.
            if (showSettingsMsg_ != 0 && msg == showSettingsMsg_) {
                if (mode_ == Mode::Daemon) OpenSettings();
                return 0;
            }
            // CLI: verbos desde otra invocación del exe.
            if (cmdMsg_ != 0 && msg == cmdMsg_) {
                ExecuteVerb(static_cast<UINT>(wParam));
                return 0;
            }
            break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ─────────────────────────────────────────────────────────────────────────────
// Acciones

void MuteMicCore::ToggleMute() {
    MicState state = audio_.Toggle();
    if (state != MicState::NoDevice) {
        if (settings_.playSound)
            PlayCue(state == MicState::Muted);
        if (settings_.visualCue != 0)
            VisualCue::Show(state == MicState::Muted,
                            settings_.visualCue, settings_.cueEdge,
                            settings_.theme == 1, settings_.glass,
                            settings_.frost);
    }
    RefreshTray();
    NotifyStateChanged();
}

void MuteMicCore::SetMuteExplicit(bool muted) {
    const MicState before = audio_.GetState();
    if (before == MicState::NoDevice) return;
    const bool wasMuted = (before == MicState::Muted);
    if (wasMuted == muted) return;

    if (!audio_.SetMuted(muted)) return;
    if (settings_.playSound) PlayCue(muted);
    if (settings_.visualCue != 0)
        VisualCue::Show(muted, settings_.visualCue, settings_.cueEdge,
                        settings_.theme == 1, settings_.glass, settings_.frost);
    RefreshTray();
    NotifyStateChanged();
}

void MuteMicCore::ExecuteVerb(UINT verb) {
    switch (verb) {
        // OpenSettings() es CONSCIENTE DEL MODO: en el daemon lanza el
        // proceso de UI, en el proceso de UI trae la ventana al frente.
        // Antes esto llamaba directamente a `onOpenSettings`, que solo lo
        // asigna App::OnLaunched — o sea, SOLO existe en el proceso de
        // ajustes. En el daemon era null, así que abrir el acceso directo
        // con la app ya corriendo no hacía absolutamente nada, en silencio.
        case 0: OpenSettings(); break;
        case 1: if (!servicePaused_) ToggleMute(); break;
        case 2: if (!servicePaused_) SetMuteExplicit(true); break;
        case 3: if (!servicePaused_) SetMuteExplicit(false); break;
    }
}

void MuteMicCore::SetDeviceId(const std::wstring& id) {
    if (settings_.deviceId == id) return;
    RestoreOriginalState();
    settings_.deviceId = id;
    audio_.SetDeviceId(id);
    CaptureOriginalState();
    SaveSettings();
    UpdateMeterTimer();
    RefreshTray();
    NotifyStateChanged();
}

void MuteMicCore::SetServicePaused(bool paused) {
    if (servicePaused_ == paused) return;
    servicePaused_ = paused;
    if (paused) {
        HotkeyHook::Uninstall();
        HotkeyHook::UninstallMouse();
        for (size_t i = 0; i < settings_.shortcuts.size(); ++i)
            UnregisterHotKey(hwnd_, kRegisteredHotkeyBase + static_cast<int>(i));
    } else {
        HotkeyHook::Install(hwnd_);
        ApplyBindings();
    }
    UpdateMeterTimer();
    RefreshTray();
    NotifyStateChanged();
}

void MuteMicCore::RequestExit() {
    // Idempotente y DIFERIDO: si nos llamaron desde dentro de un wndproc
    // (menú del tray) o de un handler XAML, destruir todo ahí mismo causa
    // use-after-free (el 0xC0000005 al salir). Encolar al dispatcher hace
    // el teardown con el stack limpio y en orden.
    bool expected = false;
    if (!exiting_.compare_exchange_strong(expected, true)) return;

    // El daemon se lleva consigo al proceso de ajustes. Antes no: hacer Quit
    // en el tray dejaba la ventana viva (aunque oculta), y esa huérfana era
    // la que después producía dos ventanas al reabrir.
    if (IsDaemon() && quitMsg_ != 0)
        PostMessageW(HWND_BROADCAST, quitMsg_,
                     static_cast<WPARAM>(GetCurrentProcessId()), 0);

    auto doExit = [] {
        auto& self = Get();
        // Orden: primero lo que dibuja/captura (glass, cues), luego hooks,
        // luego tray, y el mic restaurado dentro de Term.
        LiquidGlassBackdrop::Stop();
        const bool daemon = self.IsDaemon();
        self.Term();
        if (daemon) {
            // El daemon no tiene Application de XAML: su bucle es un
            // GetMessage clásico y se sale con WM_QUIT.
            PostQuitMessage(0);
        } else {
            winrt::Microsoft::UI::Xaml::Application::Current().Exit();

            // RED DE SEGURIDAD. `Application::Exit()` asume el bucle de
            // mensajes que instala el WinMain generado por XAML; aquí el
            // WinMain es MANUAL (DISABLE_XAML_GENERATED_MAIN) y no siempre
            // lo rompe. Medido: la ventana se destruía, el hook de render se
            // revocaba, y el proceso seguía vivo indefinidamente con ~79 MB
            // comprometidos, su timer del medidor corriendo y una ráfaga de
            // disco cada 45 s. Nunca se escribía `APP shutdown` — esa
            // ausencia fue la pista.
            //
            // El proceso de ajustes es EFÍMERO por diseño: si medio segundo
            // después seguimos aquí, se termina y punto. El timer con hwnd
            // nulo se despacha por el bucle de este hilo, que precisamente
            // es el que sigue vivo.
            SetTimer(nullptr, 0, 500, ForcedExitProc);
        }
    };

    if (IsDaemon()) { doExit(); return; }

    auto dq = winrt::Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread();
    if (dq) dq.TryEnqueue(doExit);
    else doExit();
}

// ─────────────────────────────────────────────────────────────────────────────
// Shortcut cards

namespace {

// ¿Es una tecla DEDICADA, que nadie usa para escribir ni como atajo de app?
//
// PORQUÉ IMPORTA: `RegisterHotKey` es la única vía que funciona cuando una
// app ELEVADA tiene el foco (el hook de bajo nivel lo bloquea UIPI: un
// proceso normal no puede ver el input dirigido a uno elevado, p. ej. el
// Administrador de tareas). Pero RegisterHotKey se APROPIA de la tecla en
// todo el sistema: registrar "M" sin modificadores haría que escribir una
// M en cualquier parte muteara el micro.
//
// La línea se traza aquí: estas teclas no existen en ningún teclado normal
// (F13-F24 solo aparecen mapeadas desde software de macros) o ya son
// funciones globales de por sí (multimedia). Apropiárselas es exactamente
// lo que el usuario pidió al asignarlas.
bool IsDedicatedKey(UINT vk) {
    if (vk >= VK_F13 && vk <= VK_F24) return true;          // macro keys
    switch (vk) {
        case VK_VOLUME_MUTE: case VK_VOLUME_DOWN: case VK_VOLUME_UP:
        case VK_MEDIA_NEXT_TRACK: case VK_MEDIA_PREV_TRACK:
        case VK_MEDIA_STOP: case VK_MEDIA_PLAY_PAUSE:
        case VK_LAUNCH_MAIL: case VK_LAUNCH_MEDIA_SELECT:
        case VK_LAUNCH_APP1: case VK_LAUNCH_APP2:
        case VK_BROWSER_BACK: case VK_BROWSER_FORWARD:
        case VK_BROWSER_REFRESH: case VK_BROWSER_STOP:
        case VK_BROWSER_SEARCH: case VK_BROWSER_FAVORITES:
        case VK_BROWSER_HOME:
        case VK_PAUSE: case VK_SCROLL:
            return true;
        default:
            return false;
    }
}

}  // namespace

void MuteMicCore::OnWindowHidden() {
    if (!hwnd_) return;
    // Diferido: al esconder, XAML todavía está liberando superficies de
    // composición. Recortar antes de que termine no sirve de nada porque
    // esas páginas vuelven a tocarse enseguida.
    //
    // PERIÓDICO, no una sola vez: aunque la ventana esté escondida, la app
    // sigue trabajando (medidor del micro, icono del tray, hooks), y ese
    // trabajo vuelve a traer páginas al working set. Un único recorte deja
    // la memoria subiendo poco a poco hasta quedarse arriba — que es
    // exactamente el "se va acumulando" reportado. Se re-arma solo mientras
    // la ventana siga oculta.
    SetTimer(hwnd_, kTrimTimerId, 900, nullptr);

    // OCULTAR TIENE QUE TERMINAR EL PROCESO.
    //
    // Medido: la ✕ de esta ventana NO dispara AppWindow().Closing — oculta.
    // Por eso RequestExit() nunca corría y el proceso quedaba vivo
    // indefinidamente con ~75 MB comprometidos, su timer del medidor
    // andando y una ráfaga de disco cada 45 s. Minimizar hacía lo mismo.
    //
    // Este timer es la única vía que cubre AMBOS casos, porque los dos
    // terminan en un cambio de visibilidad. Reabrir cancela la salida.
    if (mode_ == Mode::SettingsUi) {
        SetTimer(hwnd_, kHideExitTimerId, kHideExitMs, nullptr);
        Telemetry::Event("APP", "window.hidden", "exit grace armed");
    }
}

bool MuteMicCore::ShowNativeFlyout(int x, int y) {
    NativeFlyout::Model m;
    m.muted = (State() == MicState::Muted);
    m.serviceActive = !servicePaused_;
    m.deviceName = CurrentDeviceName();
    m.theme = settings_.theme;
    m.glass = settings_.glass;

    const auto devices = AudioController::Enumerate();
    const std::wstring current = settings_.deviceId;
    for (auto const& d : devices) {
        NativeFlyout::DeviceEntry e;
        e.name = d.name;
        e.id = d.id;
        // Sin deviceId guardado se está usando el predeterminado del
        // sistema, que es el primero que devuelve la enumeración.
        e.current = current.empty() ? (&d == &devices.front()) : (d.id == current);
        m.devices.push_back(std::move(e));
    }

    NativeFlyout::Show(x, y, m, [this, devices](NativeFlyout::Action a, int payload) {
        switch (a) {
            case NativeFlyout::Action::ToggleMute:
                ToggleMute();
                break;
            case NativeFlyout::Action::OpenSettings:
                OpenSettings();
                break;
            case NativeFlyout::Action::Quit:
                RequestExit();
                break;
            case NativeFlyout::Action::SelectDevice:
                if (payload >= 0 && static_cast<size_t>(payload) < devices.size())
                    SetDeviceId(devices[payload].id);
                break;
        }
    });
    return NativeFlyout::IsOpen();
}

void MuteMicCore::OnWindowShown() {
    if (!hwnd_) return;
    KillTimer(hwnd_, kTrimTimerId);
    KillTimer(hwnd_, kHideExitTimerId);   // volvió a la vista: no salir
    if (mode_ == Mode::SettingsUi)
        Telemetry::Event("APP", "window.shown", "exit grace cancelled");
}

namespace {

// Devuelve al sistema las páginas del working set que ya no se usan.
//
// QUÉ HACE DE VERDAD: no "libera" memoria en el sentido de free() — mueve
// las páginas del working set a la lista de standby, donde el SO puede
// reutilizarlas al instante si alguien las necesita. Si la app vuelve a
// tocarlas, regresan con un fallo de página blando (barato).
//
// POR QUÉ HACE FALTA: mostrar la ventana de WinUI 3 construye el árbol
// visual, las superficies de composición y las texturas del render. Al
// esconderla, ese trabajo no se deshace: la app se queda con 120-200 MB
// residentes durante días para algo que nadie está mirando. Para una app
// que vive en el tray, conservar ese working set es puro egoísmo.
//
// LO QUE NO ES: un truco cosmético para que el Administrador de tareas
// muestre un número bonito. Las páginas quedan REALMENTE disponibles.
void TrimWorkingSet(const char* reason) {
    PROCESS_MEMORY_COUNTERS_EX before{};
    before.cb = sizeof(before);
    GetProcessMemoryInfo(GetCurrentProcess(),
                         reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&before),
                         sizeof(before));

    SetProcessWorkingSetSizeEx(GetCurrentProcess(),
                               static_cast<SIZE_T>(-1),
                               static_cast<SIZE_T>(-1),
                               QUOTA_LIMITS_HARDWS_MIN_DISABLE |
                               QUOTA_LIMITS_HARDWS_MAX_DISABLE);

    PROCESS_MEMORY_COUNTERS_EX after{};
    after.cb = sizeof(after);
    GetProcessMemoryInfo(GetCurrentProcess(),
                         reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&after),
                         sizeof(after));

    char buf[192];
    sprintf_s(buf, "reason=%s before_mb=%.1f after_mb=%.1f private_mb=%.1f",
              reason,
              before.WorkingSetSize / 1048576.0,
              after.WorkingSetSize / 1048576.0,
              after.PrivateUsage / 1048576.0);
    Telemetry::Event("PERF", "workingset.trim", buf);
}

}  // namespace

void MuteMicCore::ApplyBindings() {
    HotkeyHook::ClearBindings();
    HotkeyHook::UninstallMouse();
    for (size_t i = 0; i < settings_.shortcuts.size(); ++i)
        UnregisterHotKey(hwnd_, kRegisteredHotkeyBase + static_cast<int>(i));
    if (servicePaused_) return;

    for (size_t i = 0; i < settings_.shortcuts.size(); ++i) {
        auto const& sc = settings_.shortcuts[i];
        if (!sc.Bound()) continue;
        const int id = static_cast<int>(i);
        switch (sc.type) {
            case 0: {  // teclado
                // RegisterHotKey es la ÚNICA vía que atraviesa UIPI (apps
                // elevadas con el foco: Administrador de tareas, instaladores,
                // juegos lanzados como admin). Se usa siempre que se pueda:
                //   - modo toggle (RegisterHotKey no notifica key-up, así que
                //     PTT/PTM no pueden usarlo), y
                //   - con modificadores, O una tecla dedicada (F13-F24,
                //     multimedia) que sea seguro apropiarse globalmente.
                // Si no aplica, queda el hook — que funciona en todo salvo
                // sobre ventanas elevadas.
                bool registered = false;
                const bool canRegister =
                    (sc.mode == 0) && (sc.mods != 0 || IsDedicatedKey(sc.vk));
                if (canRegister) {
                    registered = RegisterHotKey(
                        hwnd_, kRegisteredHotkeyBase + id,
                        sc.mods | MOD_NOREPEAT, sc.vk) != FALSE;
                    if (!registered) {
                        // Otra app ya se apropió del combo. Vale la pena
                        // saberlo: explica un atajo "que no funciona" sin
                        // que nada esté roto en nuestro lado.
                        char buf[96];
                        sprintf_s(buf, "idx=%d vk=0x%02X mods=0x%02X err=%lu",
                                  id, sc.vk, sc.mods, GetLastError());
                        Telemetry::Event("HOTKEY", "register.failed", buf);
                    }
                }
                if (!registered)
                    HotkeyHook::AddKeyBinding(id, { sc.vk, sc.scan, sc.mods });
                {
                    char buf[96];
                    sprintf_s(buf, "idx=%d vk=0x%02X mods=0x%02X mode=%u path=%s",
                              id, sc.vk, sc.mods, sc.mode,
                              registered ? "RegisterHotKey(UIPI-proof)" : "LL-hook");
                    Telemetry::Event("HOTKEY", "bind", buf);
                }
                break;
            }
            case 1:  // mouse
                HotkeyHook::AddMouseBinding(id, sc.code);
                break;
            case 2:  // gamepad: lo despacha el poll
                break;
        }
    }
}

size_t MuteMicCore::AddShortcut() {
    Shortcut sc;
    wchar_t name[32];
    swprintf_s(name, L"Shortcut %u",
               static_cast<UINT>(settings_.shortcuts.size() + 1));
    sc.name = name;
    settings_.shortcuts.push_back(sc);
    SaveSettings();
    return settings_.shortcuts.size() - 1;
}

void MuteMicCore::RemoveShortcut(size_t idx) {
    if (idx >= settings_.shortcuts.size()) return;
    CancelBind();
    settings_.shortcuts.erase(settings_.shortcuts.begin() + idx);
    SaveSettings();
    ApplyBindings();
    RefreshTray();
}

void MuteMicCore::RenameShortcut(size_t idx, const std::wstring& name) {
    if (idx >= settings_.shortcuts.size()) return;
    settings_.shortcuts[idx].name = name;
    SaveSettings();
    RefreshTray();
}

void MuteMicCore::SetShortcutMode(size_t idx, UINT mode) {
    if (idx >= settings_.shortcuts.size()) return;
    if (hwnd_) KillTimer(hwnd_, kHoldReleaseTimerId);
    pendingReleaseIdx_ = -1;
    settings_.shortcuts[idx].mode = (mode > 2) ? 0 : mode;
    SaveSettings();
    ApplyBindings();
}

void MuteMicCore::StartBind(size_t idx) {
    if (idx >= settings_.shortcuts.size()) return;
    bindingIdx_ = static_cast<int>(idx);
    // Los tres capturadores a la vez: el primer input gana y define el tipo.
    HotkeyHook::BeginCapture();
    HotkeyHook::BeginMouseCapture();
    // (gamepad: el poll revisa bindingIdx_)
}

void MuteMicCore::CancelBind() {
    bindingIdx_ = -1;
    HotkeyHook::CancelCapture();
    HotkeyHook::CancelMouseCapture();
}

void MuteMicCore::FinishBind() {
    bindingIdx_ = -1;
    HotkeyHook::CancelCapture();
    HotkeyHook::CancelMouseCapture();
    SaveSettings();
    ApplyBindings();
    RefreshTray();
    if (onShortcutsChanged) onShortcutsChanged();
}

void MuteMicCore::TestShortcut(size_t idx) {
    if (idx >= settings_.shortcuts.size()) return;
    // Demostración: dispara la acción base con cues incluidos.
    ToggleMute();
}

std::wstring MuteMicCore::BindingName(const Shortcut& sc) const {
    if (!sc.Bound()) return L"Not bound";
    switch (sc.type) {
        case 0: {
            KeyCombo combo{ sc.vk, sc.scan, sc.mods };
            return HotkeyHook::ComboName(combo);
        }
        case 1: return HotkeyHook::MouseButtonName(sc.code);
        case 2: return L"Pad: " + PadButtonName(sc.code);
    }
    return L"Not bound";
}

bool MuteMicCore::AnyPadConnected() const {
    return padConnected_[0] || padConnected_[1] ||
           padConnected_[2] || padConnected_[3];
}

std::wstring MuteMicCore::PadButtonName(UINT bit) {
    switch (bit) {
        case XINPUT_GAMEPAD_A: return L"A";
        case XINPUT_GAMEPAD_B: return L"B";
        case XINPUT_GAMEPAD_X: return L"X";
        case XINPUT_GAMEPAD_Y: return L"Y";
        case XINPUT_GAMEPAD_LEFT_SHOULDER: return L"LB";
        case XINPUT_GAMEPAD_RIGHT_SHOULDER: return L"RB";
        case XINPUT_GAMEPAD_BACK: return L"Back / View";
        case XINPUT_GAMEPAD_START: return L"Start / Menu";
        case XINPUT_GAMEPAD_LEFT_THUMB: return L"LS click";
        case XINPUT_GAMEPAD_RIGHT_THUMB: return L"RS click";
        case XINPUT_GAMEPAD_DPAD_UP: return L"D-Pad Up";
        case XINPUT_GAMEPAD_DPAD_DOWN: return L"D-Pad Down";
        case XINPUT_GAMEPAD_DPAD_LEFT: return L"D-Pad Left";
        case XINPUT_GAMEPAD_DPAD_RIGHT: return L"D-Pad Right";
    }
    return L"None";
}

// ─────────────────────────────────────────────────────────────────────────────
// Sonido

// UNA SOLA ENTRADA EN EL MEZCLADOR DE VOLUMEN.
//
// Un MediaPlayer registra una sesión de audio de renderizado a nombre de su
// proceso. Con el split en daemon, previsualizar un sonido o probar una card
// creaba un segundo MediaPlayer en el proceso de ajustes, y Windows mostraba
// "MuteMic" dos veces en el mezclador. Es cosmético, pero es la clase de
// suciedad que distingue un programa cuidado de uno que solo funciona.
//
// El arreglo no es cerrar el player después de sonar — eso deja igual un
// duplicado parpadeando. Es que el proceso de ajustes NO reproduzca nunca:
// le pide al daemon, que ya tiene su sesión abierta. Si no hay daemon (se
// lanzó --settings suelto), se reproduce local, porque entonces no hay
// duplicado posible.
void MuteMicCore::PlayCue(bool muteCue) {
    Probes::Hit(Probe::CuePlay);
    // Rearmar el enfriamiento del reproductor en cada cue.
    if (hwnd_) SetTimer(hwnd_, kCuePlayerTimerId, kCuePlayerIdleMs, nullptr);
    if (mode_ == Mode::SettingsUi && playCueMsg_ != 0) {
        if (HANDLE daemon = OpenMutexW(SYNCHRONIZE, FALSE, kMutexName)) {
            CloseHandle(daemon);
            PostMessageW(HWND_BROADCAST, playCueMsg_,
                         static_cast<WPARAM>(GetCurrentProcessId()),
                         muteCue ? 1 : 0);
            return;
        }
    }

    std::wstring file =
        SoundDir(muteCue) + L"\\" +
        (muteCue ? settings_.soundMuteFile : settings_.soundUnmuteFile);
    if (GetFileAttributesW(file.c_str()) == INVALID_FILE_ATTRIBUTES) {
        auto available = EnumerateSounds(muteCue);
        if (available.empty()) return;
        if (muteCue) settings_.soundMuteFile = available.front();
        else settings_.soundUnmuteFile = available.front();
        SaveSettings();
        file = SoundDir(muteCue) + L"\\" + available.front();
    }

    for (auto& c : file)
        if (c == L'\\') c = L'/';

    try {
        if (!player_) {
            Telemetry::Memory("PERF", "subsystem", "mediaplayer.before");
            player_ = winrt::Windows::Media::Playback::MediaPlayer();
            // Cero integración con el sistema de medios (sin SMTC, categoría
            // SoundEffects): sin overlays ni "doorbell" por cada cue.
            try { player_.CommandManager().IsEnabled(false); } catch (...) {}
            try {
                player_.AudioCategory(
                    winrt::Windows::Media::Playback::MediaPlayerAudioCategory::SoundEffects);
            } catch (...) {}
            Telemetry::Memory("PERF", "subsystem", "mediaplayer.after");
        }
        player_.Volume(settings_.soundVolume / 100.0);

        // Solo se construye un MediaSource cuando CAMBIA el archivo, y se
        // cierra el anterior explícitamente. Reproducir el mismo sonido mil
        // veces reusa el mismo objeto.
        if (!cueSource_ || cueSourceFile_ != file) {
            if (cueSource_) {
                try { cueSource_.Close(); } catch (...) {}
                cueSource_ = nullptr;
            }
            cueSource_ = winrt::Windows::Media::Core::MediaSource::CreateFromUri(
                winrt::Windows::Foundation::Uri(L"file:///" + file));
            cueSourceFile_ = file;
            player_.Source(cueSource_);
        }
        // Rebobinar y disparar: sin re-asignar Source no hay objeto nuevo.
        try { player_.PlaybackSession().Position(winrt::Windows::Foundation::TimeSpan{ 0 }); }
        catch (...) {}
        player_.Play();
    } catch (...) {
        // Sin audio de salida o archivo corrupto: el cue es opcional.
    }
}

void MuteMicCore::SetSoundVolume(UINT volume0to100) {
    settings_.soundVolume = (volume0to100 > 100) ? 100 : volume0to100;
    SaveSettings();
}

void MuteMicCore::SetSoundFile(bool muteCue, const std::wstring& fileName) {
    if (muteCue) settings_.soundMuteFile = fileName;
    else settings_.soundUnmuteFile = fileName;
    SaveSettings();
}

void MuteMicCore::PreviewSound(bool muteCue) {
    PlayCue(muteCue);
}

std::vector<std::wstring> MuteMicCore::EnumerateSounds(bool muteCue) {
    std::vector<std::wstring> result;
    WIN32_FIND_DATAW fd;
    HANDLE find = FindFirstFileW((SoundDir(muteCue) + L"\\*.wav").c_str(), &fd);
    if (find == INVALID_HANDLE_VALUE) return result;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            result.push_back(fd.cFileName);
    } while (FindNextFileW(find, &fd));
    FindClose(find);
    return result;
}

void MuteMicCore::PreviewCue() {
    VisualCue::Show(State() == MicState::Muted,
                    settings_.visualCue, settings_.cueEdge,
                    settings_.theme == 1, settings_.glass, settings_.frost);
}

// ─────────────────────────────────────────────────────────────────────────────
// Estado original (restaurar al salir)

void MuteMicCore::CaptureOriginalState() {
    const MicState s = audio_.GetState();
    if (s == MicState::NoDevice) originalMute_.reset();
    else originalMute_ = (s == MicState::Muted);
}

void MuteMicCore::RestoreOriginalState() {
    if (originalMute_.has_value()) {
        audio_.SetMuted(*originalMute_);
        originalMute_.reset();
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Estado / tray

// PROPAGACIÓN DE AJUSTES ENTRE PROCESOS.
//
// Tras el split en daemon hay DOS copias en memoria de la configuración. La
// ventana guardaba a disco y el daemon nunca recargaba, así que cambiar el
// cue, el sonido, el tema o el dispositivo no llegaba nunca a quien de hecho
// reacciona al atajo. El síntoma que lo destapó: cambiar el estilo de cue y
// ver el nuevo al pulsar el botón de test de la card (corre en la ventana)
// pero el viejo al pulsar F14 (lo atiende el daemon).
//
// Se arregla en general, no cue por cue: quien guarda avisa, y el otro
// recarga. El pid va en wParam porque HWND_BROADCAST también nos alcanza a
// nosotros — ignorar el propio mensaje es lo que evita repetir el bucle de
// broadcast que costó la sesión anterior.
void MuteMicCore::SaveSettings() {
    Probes::Hit(Probe::SettingsSave);
    SettingsStore::Save(settings_);
    if (settingsChangedMsg_ != 0)
        PostMessageW(HWND_BROADCAST, settingsChangedMsg_,
                     static_cast<WPARAM>(GetCurrentProcessId()), 0);
}

// Relee del disco y re-aplica todo lo que depende de la configuración.
// Barato: solo corre cuando el OTRO proceso guardó de verdad.
void MuteMicCore::ReloadSettings() {
    Probes::Hit(Probe::SettingsReload);
    settings_ = SettingsStore::Load();
    audio_.SetDeviceId(settings_.deviceId);
    ApplyBindings();
    UpdateMeterTimer();
    RefreshTray();
    NotifyStateChanged();
    if (onShortcutsChanged) onShortcutsChanged();
    Telemetry::Event("APP", "settings.reload", "another process saved");
}

MicState MuteMicCore::State() { return audio_.GetState(); }

// El llamador es un hook CompositionTarget::Rendering: corre a la frecuencia
// REAL del monitor — 165 Hz en la máquina de desarrollo, y más en otras. Cada
// llamada cruza a Core Audio.
//
// Un medidor de nivel no necesita 165 Hz: el ojo no distingue por encima de
// ~60, y la barra ya suaviza por interpolación. Limitarlo aquí, y no en el
// llamador, mantiene la garantía en un solo sitio para TODOS los llamadores
// presentes y futuros.
//
// Esto no es una optimización cosmética. Cuando algo va mal aguas abajo —un
// endpoint que se cae, dos procesos peleándose el mismo dispositivo— la
// frecuencia es lo que convierte un error recuperable en una tormenta de RPC
// que deja inutilizable el escritorio entero. Ya pasó dos veces. Bajar el
// ritmo elimina el amplificador aunque el fallo de fondo reaparezca.
float MuteMicCore::PollPeak() {
    Probes::Hit(Probe::LevelBarUpdate);
    constexpr ULONGLONG kMinIntervalMs = 16;   // ~60 Hz techo
    const ULONGLONG now = GetTickCount64();
    if (now - lastPeakPoll_ < kMinIntervalMs)
        return lastLevel_;                     // valor cacheado, cero coste
    lastPeakPoll_ = now;
    lastLevel_ = audio_.GetPeak();
    return lastLevel_;
}

std::wstring MuteMicCore::CurrentDeviceName() { return audio_.CurrentDeviceName(); }

std::wstring MuteMicCore::HotkeyName() const {
    // Tooltip: primer shortcut con binding.
    for (auto const& sc : settings_.shortcuts)
        if (sc.Bound()) return BindingName(sc);
    return L"(none)";
}

void MuteMicCore::AddStateListener(std::function<void()> listener) {
    stateListeners_.push_back(std::move(listener));
}

void MuteMicCore::UpdateMeterTimer() {
    const bool shouldRun =
        !servicePaused_ && !sessionLocked_ && State() != MicState::NoDevice;

    if (shouldRun && !timerRunning_) {
        SetTimer(hwnd_, kMeterTimerId, kMeterIntervalMs, nullptr);
        timerRunning_ = true;
    } else if (!shouldRun && timerRunning_) {
        KillTimer(hwnd_, kMeterTimerId);
        timerRunning_ = false;
        lastLevel_ = 0.0f;
        RefreshTray();
    }
}

std::wstring MuteMicCore::BuildTooltip() {
    std::wstring tip;
    if (settings_.tooltipShowDevice) {
        std::wstring name = audio_.CurrentDeviceName();
        if (!name.empty()) tip = name;
    }

    const MicState state = State();
    std::wstring stateText =
        servicePaused_ ? L"Service paused"
        : state == MicState::Muted ? L"MUTED"
        : state == MicState::Unmuted ? L"Mic live"
        : L"No microphone";
    tip += tip.empty() ? stateText : (L" — " + stateText);

    if (settings_.tooltipShowShortcut && !servicePaused_)
        tip += L" (" + HotkeyName() + L")";

    if (tip.size() > 127) tip.resize(127);
    return tip;
}

void MuteMicCore::RefreshTray() {
    if (!tray_) return;
    const MicState state = State();
    tray_->Update(FaceFor(state, lastLevel_, servicePaused_), lastLevel_,
                  BuildTooltip());
}

void MuteMicCore::NotifyStateChanged() {
    for (auto& l : stateListeners_)
        if (l) l();
}

}  // namespace mutemic
