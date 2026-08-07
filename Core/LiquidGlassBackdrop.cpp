#include "pch.h"
#include "LiquidGlassBackdrop.h"

#include <algorithm>
#include <cmath>
#include <d3d11.h>
#include <fstream>
#include <vector>

#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/Windows.Graphics.Effects.h>
#include <windows.graphics.capture.interop.h>

#include <winrt/Microsoft.Graphics.Canvas.h>
#include <winrt/Microsoft.Graphics.Canvas.Effects.h>
#include <winrt/Microsoft.Graphics.Canvas.UI.Xaml.h>

#include <winrt/Windows.Foundation.Numerics.h>

namespace mutemic {
namespace {

namespace WGC = winrt::Windows::Graphics::Capture;
namespace WGD = winrt::Windows::Graphics::DirectX;
namespace MGC = winrt::Microsoft::Graphics::Canvas;
namespace MGCX = winrt::Microsoft::Graphics::Canvas::UI::Xaml;
namespace MUXC = winrt::Microsoft::UI::Xaml::Controls;

// Parámetros del repo (curva f y glow) + forma adaptada a la ventana:
// lente = rect redondeado en px, refracción solo en una banda del borde.
struct GlassParams {
    float cornerRad, bandPx, fPower, a, b, c, d, noise, blurPx;
    float glowWeight, glowBias, glowE0, glowE1;
};
// "Full effect" (blur + noise + glow)
constexpr GlassParams kFrosted{ 16.0f, 110.0f, 1.0f, 0.7f, 2.3f, 5.2f, 6.9f, 0.10f, 6.0f,
                                0.3f, 0.0f, 0.06f, 0.0f };
// "Without blur or noise" (refracción limpia, dirección normal)
constexpr GlassParams kClear{ 16.0f, 90.0f, 1.779f, 0.992f, 2.332f, 4.544f, 6.923f, 0.0f, 0.0f,
                              0.0f, 0.0f, 0.06f, 0.0f };

constexpr int kMaxLenses = 4;          // principal, flyout, y margen a futuro
constexpr UINT kRecaptureDelayMs = 220;  // debounce del refresco por eventos

// ── Una LENTE: una ventana que pinta el snapshot con refracción ──
// Cada lente tiene su propio grafo de efectos porque el shader recibe el
// tamaño de SU ventana (winSize) y su propia traslación dentro del monitor.
struct Lens {
    HWND hwnd = nullptr;
    MUXC::Grid host{ nullptr };
    MGCX::CanvasControl canvas{ nullptr };
    MGCX::CanvasControl::Draw_revoker drawRevoker{};

    MGC::Effects::Transform2DEffect fxShift{ nullptr };
    MGC::Effects::Transform2DEffect fxDown{ nullptr };   // blur a media res
    MGC::Effects::GaussianBlurEffect fxBlur{ nullptr };
    MGC::Effects::Transform2DEffect fxUp{ nullptr };
    MGC::Effects::PixelShaderEffect fxGlass{ nullptr };
    bool graphIsFrost = false;

    // ── Predicción de movimiento (late-stage reprojection) ──
    // El marco de la ventana lo mueve DWM al instante; nuestro frame se
    // presenta 1-2 frames después. Muestrear la posición actual deja el
    // cristal atrasado en arrastres rápidos. Se estima la velocidad y se
    // muestrea donde la ventana ESTARÁ al presentarse el frame.
    POINT lastTopLeft{ 0, 0 };
    LARGE_INTEGER lastPosQpc{};
    float velX = 0.0f, velY = 0.0f;   // px por ms

    bool InUse() const { return hwnd != nullptr; }
};

// Latencia estimada del pipeline (invalidate → draw → present).
constexpr float kPredictMs = 14.0f;
// Tope de corrección: evita saltos absurdos si la estimación se dispara.
constexpr float kPredictMaxPx = 140.0f;

// ── Estado COMPARTIDO: una captura para todas las lentes ──
struct Shared {
    bool active = false;
    bool frost = false;
    bool shareFriendly = false;
    HMONITOR monitor = nullptr;

    WGC::GraphicsCaptureItem item{ nullptr };
    WGC::Direct3D11CaptureFramePool framePool{ nullptr };
    WGC::GraphicsCaptureSession session{ nullptr };
    WGC::Direct3D11CaptureFramePool::FrameArrived_revoker frameRevoker{};

    MGC::CanvasBitmap lastFrame{ nullptr };
    std::vector<uint8_t> shader;
    ULONGLONG lastFrameMs = 0;     // throttle del pipeline a ~30 fps

    // Refresco por eventos del escritorio (en vez de recapturar al mover).
    HWINEVENTHOOK hookForeground = nullptr;
    HWINEVENTHOOK hookLocation = nullptr;
    UINT_PTR recaptureTimer = 0;

    Lens lenses[kMaxLenses];
};

Shared g;

Lens* FindLens(HWND hwnd) {
    for (auto& l : g.lenses)
        if (l.hwnd == hwnd) return &l;
    return nullptr;
}

Lens* FindLensByCanvas(MGCX::CanvasControl const& c) {
    for (auto& l : g.lenses)
        if (l.canvas && l.canvas == c) return &l;
    return nullptr;
}

Lens* FirstLens() {
    for (auto& l : g.lenses)
        if (l.InUse() && l.canvas) return &l;
    return nullptr;
}

// ── Log de diagnóstico: %exe%\mutemic-glass.log ──
// El pipeline tiene varios puntos de fallo silencioso (captura, device,
// shader, effect properties); esto los hace visibles sin debugger.
void Log(const char* msg, HRESULT hr = S_OK) {
#ifndef NDEBUG
    // Solo en Debug: en Release no se escribe nada a disco.
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring file = path;
    file = file.substr(0, file.find_last_of(L'\\')) + L"\\mutemic-glass.log";
    std::ofstream out(file, std::ios::app);
    out << msg;
    if (FAILED(hr)) {
        char buf[32];
        sprintf_s(buf, " hr=0x%08X", static_cast<unsigned>(hr));
        out << buf;
    }
    out << "\n";
#else
    (void)msg; (void)hr;
#endif
}

std::vector<uint8_t> LoadShaderBlob() {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring file = path;
    const size_t slash = file.find_last_of(L'\\');
    file = file.substr(0, slash) + L"\\LiquidGlass.cso";

    std::ifstream in(file, std::ios::binary);
    if (!in) return {};
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(in),
                                std::istreambuf_iterator<char>());
}

// Excluir/incluir TODAS nuestras ventanas de la captura: si una lente
// aparece en el snapshot que ella misma muestrea, se produce el efecto
// túnel infinito.
void SetLensesCaptureAffinity(bool exclude) {
    for (auto& l : g.lenses)
        // SOLO ventanas visibles: tocar el display affinity de una ventana
        // oculta (el flyout en reposo) fuerza una re-composición que la hace
        // parpadear en pantalla — el "fantasma del tray" al cambiar de app.
        if (l.hwnd && IsWindowVisible(l.hwnd))
            SetWindowDisplayAffinity(l.hwnd,
                                     exclude ? WDA_EXCLUDEFROMCAPTURE : WDA_NONE);
}

void InvalidateAllLenses() {
    for (auto& l : g.lenses)
        if (l.canvas && l.hwnd && IsWindowVisible(l.hwnd)) l.canvas.Invalidate();
}

void ReleaseLensGraph(Lens& l) {
    l.fxShift = nullptr;
    l.fxDown = nullptr;
    l.fxBlur = nullptr;
    l.fxUp = nullptr;
    l.fxGlass = nullptr;
}

bool StartCaptureForMonitor(HMONITOR monitor) {
    Lens* primary = FirstLens();
    if (!primary) return false;

    // GraphicsCaptureItem del monitor vía interop (sin picker; funciona en
    // apps desktop unpackaged).
    auto factory = winrt::get_activation_factory<WGC::GraphicsCaptureItem>();
    auto interop = factory.as<IGraphicsCaptureItemInterop>();
    WGC::GraphicsCaptureItem item{ nullptr };
    HRESULT hr = interop->CreateForMonitor(
        monitor, winrt::guid_of<WGC::GraphicsCaptureItem>(), winrt::put_abi(item));
    if (FAILED(hr)) {
        Log("CreateForMonitor FAILED", hr);
        return false;
    }

    // El device del CanvasControl ES un IDirect3DDevice: mismo device para
    // captura y para envolver los frames como CanvasBitmap (requisito).
    // Los CanvasControl usan el device compartido de Win2D, así que el
    // bitmap resultante es dibujable desde CUALQUIER lente.
    try {
        auto device = primary->canvas.Device();

        g.item = item;
        g.framePool = WGC::Direct3D11CaptureFramePool::Create(
            device, WGD::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, item.Size());
        g.session = g.framePool.CreateCaptureSession(item);
    } catch (winrt::hresult_error const& e) {
        Log("FramePool/session create FAILED", e.code());
        g.item = nullptr;
        g.framePool = nullptr;
        g.session = nullptr;
        return false;
    }
    try { g.session.IsCursorCaptureEnabled(false); } catch (...) {}
    try { g.session.IsBorderRequired(false); } catch (...) {}

    g.frameRevoker = g.framePool.FrameArrived(
        winrt::auto_revoke, [](auto&& pool, auto&&)
        {
            auto frame = pool.TryGetNextFrame();
            Lens* primary = FirstLens();
            if (!frame || !primary) return;

            // Throttle: el escritorio puede entregar frames a 144 Hz
            // (wallpaper animado, video). Para un fondo de cristal, 30 fps
            // son indistinguibles y cuestan una fracción.
            const ULONGLONG now = GetTickCount64();
            if (now - g.lastFrameMs < 33) return;
            g.lastFrameMs = now;

            try {
                const bool first = (g.lastFrame == nullptr);
                auto wrapped = MGC::CanvasBitmap::CreateFromDirect3D11Surface(
                    primary->canvas.Device(), frame.Surface());

                if (g.shareFriendly) {
                    // COPIA real del frame (el buffer del pool se recicla al
                    // cerrar la captura; un wrap colgaría de memoria muerta).
                    MGC::CanvasRenderTarget copy(
                        primary->canvas.Device(),
                        static_cast<float>(wrapped.SizeInPixels().Width),
                        static_cast<float>(wrapped.SizeInPixels().Height),
                        96.0f);
                    {
                        auto cds = copy.CreateDrawingSession();
                        cds.DrawImage(wrapped);
                        cds.Close();
                    }
                    g.lastFrame = copy;
                    // Snapshot tomado: cerrar captura y volver a ser VISIBLES
                    // en capturas/screen share. El snapshot sigue siendo
                    // válido aunque las ventanas se muevan — solo se
                    // refresca cuando cambia el FONDO (ver los WinEventHook).
                    g.frameRevoker.revoke();
                    if (g.session) { g.session.Close(); g.session = nullptr; }
                    if (g.framePool) { g.framePool.Close(); g.framePool = nullptr; }
                    g.item = nullptr;
                    SetLensesCaptureAffinity(false);
                } else {
                    g.lastFrame = wrapped;
                }

                if (first) Log("first frame arrived");
                InvalidateAllLenses();
            } catch (winrt::hresult_error const& e) {
                Log("FrameArrived wrap FAILED", e.code());
            } catch (...) {
                Log("FrameArrived wrap FAILED (unknown)");
            }
        });

    g.session.StartCapture();
    g.monitor = monitor;
    return true;
}

void StopCapture() {
    g.frameRevoker.revoke();
    if (g.session) { g.session.Close(); g.session = nullptr; }
    if (g.framePool) { g.framePool.Close(); g.framePool = nullptr; }
    g.item = nullptr;
    g.monitor = nullptr;
    g.lastFrame = nullptr;
    // El grafo referencia recursos del device: soltarlo también.
    for (auto& l : g.lenses) ReleaseLensGraph(l);
}

// ¿Hay alguna lente visible? (la principal puede estar en el tray mientras
// el flyout SÍ se ve: ahí el cristal se sigue necesitando).
Lens* FirstVisibleLens() {
    for (auto& l : g.lenses)
        if (l.InUse() && l.canvas && IsWindowVisible(l.hwnd)) return &l;
    return nullptr;
}

// Toma un snapshot nuevo AHORA (excluyendo nuestras ventanas).
void DoRecapture() {
    if (!g.active) return;
    if (g.session) return;                  // ya hay un ciclo en curso
    Lens* visible = FirstVisibleLens();
    if (!visible) return;                   // todo oculto: no gastar GPU

    SetLensesCaptureAffinity(true);
    StartCaptureForMonitor(MonitorFromWindow(visible->hwnd, MONITOR_DEFAULTTONEAREST));
}

void CALLBACK RecaptureTimerProc(HWND, UINT, UINT_PTR id, DWORD) {
    KillTimer(nullptr, id);
    g.recaptureTimer = 0;
    DoRecapture();
}

void ScheduleRecapture() {
    if (!g.active || !g.shareFriendly) return;
    // Debounce con reinicio: mientras sigan llegando eventos no se recaptura.
    if (g.recaptureTimer) KillTimer(nullptr, g.recaptureTimer);
    g.recaptureTimer = SetTimer(nullptr, 0, kRecaptureDelayMs, RecaptureTimerProc);
}

// Hook de eventos del escritorio: el snapshot solo se refresca cuando el
// FONDO cambió de verdad (otra app pasó a primer plano, una ventana ajena
// se movió/cerró). Mover NUESTRA ventana no invalida nada.
void CALLBACK WinEventProc(HWINEVENTHOOK, DWORD event, HWND hwnd,
                           LONG idObject, LONG idChild, DWORD, DWORD) {
    if (!g.active || !hwnd) return;
    if (idObject != OBJID_WINDOW || idChild != CHILDID_SELF) return;
    if (FindLens(hwnd)) return;               // es una de nuestras lentes

    // Ignorar ventanas de nuestro propio proceso (tray, tooltips, cues).
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == GetCurrentProcessId()) return;

    if (event == EVENT_OBJECT_LOCATIONCHANGE && !IsWindowVisible(hwnd)) return;

    ScheduleRecapture();
}

void InstallEventHooks() {
    if (g.hookForeground) return;
    // WINEVENT_OUTOFCONTEXT: el callback llega por la cola de mensajes de
    // ESTE hilo (UI). Sin inyección de DLL en otros procesos.
    g.hookForeground = SetWinEventHook(
        EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr,
        WinEventProc, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    g.hookLocation = SetWinEventHook(
        EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE, nullptr,
        WinEventProc, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    Log("win event hooks installed");
}

void RemoveEventHooks() {
    if (g.hookForeground) { UnhookWinEvent(g.hookForeground); g.hookForeground = nullptr; }
    if (g.hookLocation) { UnhookWinEvent(g.hookLocation); g.hookLocation = nullptr; }
    if (g.recaptureTimer) { KillTimer(nullptr, g.recaptureTimer); g.recaptureTimer = 0; }
}

void OnDraw(MGCX::CanvasControl const& sender, MGCX::CanvasDrawEventArgs const& args) {
    if (!g.active) return;
    Lens* lens = FindLensByCanvas(sender);
    if (!lens || !lens->hwnd) return;

    auto ds = args.DrawingSession();

    if (!g.lastFrame) {
        // Aún sin frame: fondo neutro para evitar flash.
        ds.Clear(winrt::Windows::UI::ColorHelper::FromArgb(255, 16, 18, 22));
        return;
    }

    // Región del monitor detrás del área CLIENTE. GetWindowRect incluye los
    // bordes invisibles de resize de DWM (~8 px por lado): con eso la lente
    // salía más grande que el canvas y las bandas derecha/inferior quedaban
    // cortadas fuera de la ventana.
    RECT cr{};
    GetClientRect(lens->hwnd, &cr);
    POINT tl{ 0, 0 };
    ClientToScreen(lens->hwnd, &tl);
    MONITORINFO mi{ sizeof(mi) };
    HMONITOR mon = MonitorFromWindow(lens->hwnd, MONITOR_DEFAULTTONEAREST);
    GetMonitorInfoW(mon, &mi);

    if (mon != g.monitor && lens == FirstLens()) {
        // La ventana principal cambió de monitor: recapturar allá.
        StopCapture();
        DoRecapture();
        return;
    }

    const int w = cr.right, h = cr.bottom;
    if (w <= 0 || h <= 0) return;

    // ── Late-stage reprojection ──
    // DWM ya movió el marco; este frame se verá ~kPredictMs después. Se
    // estima la velocidad de la ventana y se muestrea la zona donde ESTARÁ
    // al presentarse, no donde está al dibujar. Sin esto el cristal se
    // arrastra visiblemente en movimientos rápidos.
    LARGE_INTEGER now{}, freq{};
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    float predX = 0.0f, predY = 0.0f;
    if (lens->lastPosQpc.QuadPart != 0) {
        const float dtMs = static_cast<float>(now.QuadPart - lens->lastPosQpc.QuadPart)
                           * 1000.0f / static_cast<float>(freq.QuadPart);
        if (dtMs > 0.5f && dtMs < 120.0f) {
            const float instX = (tl.x - lens->lastTopLeft.x) / dtMs;
            const float instY = (tl.y - lens->lastTopLeft.y) / dtMs;
            // Suavizado exponencial: el ruido de un frame suelto no debe
            // hacer saltar la lente.
            lens->velX = lens->velX * 0.45f + instX * 0.55f;
            lens->velY = lens->velY * 0.45f + instY * 0.55f;
        } else if (dtMs >= 120.0f) {
            lens->velX = lens->velY = 0.0f;   // quieta: sin predicción
        }
        predX = std::clamp(lens->velX * kPredictMs, -kPredictMaxPx, kPredictMaxPx);
        predY = std::clamp(lens->velY * kPredictMs, -kPredictMaxPx, kPredictMaxPx);
    }
    lens->lastTopLeft = tl;
    lens->lastPosQpc = now;

    // Redondeo a píxel ENTERO: si la traslación cae en medio píxel, D2D
    // interpola bilinealmente y TODO el fondo se suaviza — el texto detrás
    // del cristal pierde nitidez sin razón. Con offset entero, la zona sin
    // refracción es copia 1:1 y el texto se lee igual que sin cristal.
    const float ox = std::round(static_cast<float>(tl.x - mi.rcMonitor.left) + predX);
    const float oy = std::round(static_cast<float>(tl.y - mi.rcMonitor.top) + predY);
    const GlassParams& P = g.frost ? kFrosted : kClear;

    const char* stage = "start";
    try {
        // ── Grafo cacheado por lente: construir el PixelShaderEffect
        // (parsea el blob) en cada Draw causa jitter. Se arma una vez y por
        // frame solo se actualizan fuente, traslación y winSize.
        if (!lens->fxGlass || lens->graphIsFrost != g.frost) {
            stage = "build graph";
            lens->fxShift = MGC::Effects::Transform2DEffect();

            winrt::Windows::Graphics::Effects::IGraphicsEffectSource src = lens->fxShift;
            if (P.blurPx > 0.1f) {
                // Blur a MEDIA resolución (como el "Blur downscale 0.5" del
                // repo): reduce ~4× el costo GPU del gaussian; el resultado
                // ya viene borroso así que el upscale no pierde nada.
                using winrt::Windows::Foundation::Numerics::make_float3x2_scale;
                lens->fxDown = MGC::Effects::Transform2DEffect();
                lens->fxDown.Source(lens->fxShift);
                lens->fxDown.TransformMatrix(make_float3x2_scale(0.5f));

                lens->fxBlur = MGC::Effects::GaussianBlurEffect();
                lens->fxBlur.Source(lens->fxDown);
                lens->fxBlur.BlurAmount(P.blurPx * 0.5f);
                lens->fxBlur.BorderMode(MGC::Effects::EffectBorderMode::Hard);

                lens->fxUp = MGC::Effects::Transform2DEffect();
                lens->fxUp.Source(lens->fxBlur);
                lens->fxUp.TransformMatrix(make_float3x2_scale(2.0f));
                src = lens->fxUp;
            } else {
                lens->fxDown = nullptr;
                lens->fxBlur = nullptr;
                lens->fxUp = nullptr;
            }

            lens->fxGlass = MGC::Effects::PixelShaderEffect(
                winrt::array_view<uint8_t const>(g.shader.data(),
                                                 g.shader.data() + g.shader.size()));
            lens->fxGlass.Source1(src);

            auto props = lens->fxGlass.Properties();
            using winrt::Windows::Foundation::PropertyValue;
            props.Insert(L"cornerRad", PropertyValue::CreateSingle(P.cornerRad));
            props.Insert(L"bandPx", PropertyValue::CreateSingle(P.bandPx));
            props.Insert(L"fPower", PropertyValue::CreateSingle(P.fPower));
            props.Insert(L"pa", PropertyValue::CreateSingle(P.a));
            props.Insert(L"pb", PropertyValue::CreateSingle(P.b));
            props.Insert(L"pc", PropertyValue::CreateSingle(P.c));
            props.Insert(L"pd", PropertyValue::CreateSingle(P.d));
            props.Insert(L"noiseAmt", PropertyValue::CreateSingle(P.noise));
            props.Insert(L"glowWeight", PropertyValue::CreateSingle(P.glowWeight));
            props.Insert(L"glowBias", PropertyValue::CreateSingle(P.glowBias));
            props.Insert(L"glowE0", PropertyValue::CreateSingle(P.glowE0));
            props.Insert(L"glowE1", PropertyValue::CreateSingle(P.glowE1));
            lens->graphIsFrost = g.frost;
        }

        // ── Por frame: fuente, traslación y tamaño ──
        stage = "per-frame update";
        lens->fxShift.Source(g.lastFrame);
        lens->fxShift.TransformMatrix(
            winrt::Windows::Foundation::Numerics::make_float3x2_translation(-ox, -oy));
        lens->fxGlass.Properties().Insert(L"winSize", winrt::box_value(
            winrt::Windows::Foundation::Numerics::float2{
                static_cast<float>(w), static_cast<float>(h) }));

        stage = "DrawImage";
        // TODO en píxeles físicos (ds.Units = Pixels): el capture, el rect
        // de la ventana y la posición de escena del shader quedan en el
        // mismo espacio — sin esto la lente sale descentrada (el "blob").
        ds.Units(MGC::CanvasUnits::Pixels);
        ds.DrawImage(lens->fxGlass,
                     winrt::Windows::Foundation::Rect(0, 0,
                         static_cast<float>(w), static_cast<float>(h)),
                     winrt::Windows::Foundation::Rect(0, 0,
                         static_cast<float>(w), static_cast<float>(h)));
        static bool loggedFirstDraw = false;
        if (!loggedFirstDraw) { Log("first draw OK"); loggedFirstDraw = true; }
    } catch (winrt::hresult_error const& e) {
        static bool loggedDrawFail = false;
        if (!loggedDrawFail) {
            std::string msg = "Draw FAILED at [";
            msg += stage;
            msg += "] msg: ";
            msg += winrt::to_string(e.message());
            Log(msg.c_str(), e.code());
            loggedDrawFail = true;
        }
    } catch (...) {
        Log("Draw FAILED (unknown)");
    }
}

// Inserta el CanvasControl como capa inferior del host y engancha Draw.
bool AttachCanvas(Lens& lens, MUXC::Grid const& host) {
    try {
        lens.canvas = MGCX::CanvasControl();
    } catch (winrt::hresult_error const& e) {
        // Típico: falta Microsoft.Graphics.Canvas.dll junto al exe.
        Log("CanvasControl create FAILED", e.code());
        return false;
    }
    lens.canvas.ClearColor(winrt::Windows::UI::ColorHelper::FromArgb(255, 16, 18, 22));

    // Cubrir TODO el host sin asumir su layout (la ventana principal tiene
    // 2 filas, el flyout 1: el span se calcula, no se hardcodea).
    const int rows = static_cast<int>(host.RowDefinitions().Size());
    const int cols = static_cast<int>(host.ColumnDefinitions().Size());
    MUXC::Grid::SetRow(lens.canvas, 0);
    MUXC::Grid::SetColumn(lens.canvas, 0);
    if (rows > 1) MUXC::Grid::SetRowSpan(lens.canvas, rows);
    if (cols > 1) MUXC::Grid::SetColumnSpan(lens.canvas, cols);

    lens.drawRevoker = lens.canvas.Draw(winrt::auto_revoke, &OnDraw);
    host.Children().InsertAt(0, lens.canvas);
    lens.host = host;
    return true;
}

void DetachCanvas(Lens& lens) {
    lens.drawRevoker.revoke();
    if (lens.host && lens.canvas) {
        uint32_t index = 0;
        if (lens.host.Children().IndexOf(lens.canvas, index))
            lens.host.Children().RemoveAt(index);
    }
    ReleaseLensGraph(lens);
    lens.canvas = nullptr;
    lens.host = nullptr;
    if (lens.hwnd) SetWindowDisplayAffinity(lens.hwnd, WDA_NONE);
    lens.hwnd = nullptr;
    lens.graphIsFrost = false;
}

}  // namespace

bool LiquidGlassBackdrop::Start(HWND hwnd, MUXC::Grid const& host, bool frost,
                                bool shareFriendly) {
    if (!WGC::GraphicsCaptureSession::IsSupported()) {
        Log("GraphicsCapture NOT supported");
        return false;
    }

    if (g.active && g.lenses[0].hwnd == hwnd) {
        // Reconfiguración en caliente (solo cambió frost o el modo).
        const bool modeChanged = (g.shareFriendly != shareFriendly);
        g.frost = frost;
        g.shareFriendly = shareFriendly;
        if (modeChanged) {
            StopCapture();
            DoRecapture();
        }
        RequestRedraw();
        return true;
    }

    Log("=== glass Start ===");
    g.shader = LoadShaderBlob();
    if (g.shader.empty()) {
        Log("LiquidGlass.cso NOT FOUND next to exe");
        return false;
    }

    g.shareFriendly = shareFriendly;
    g.frost = frost;

    Lens& primary = g.lenses[0];
    primary.hwnd = hwnd;
    if (!AttachCanvas(primary, host)) {
        primary.hwnd = nullptr;
        return false;
    }

    // Nuestras ventanas NO deben aparecer en su propia captura.
    SetLensesCaptureAffinity(true);

    HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    g.active = true;  // antes de StartCapture: FrameArrived usa las lentes

    InstallEventHooks();

    // La captura necesita el device del canvas; puede no existir hasta que
    // el control cargue. CreateResources dispara cuando esté listo.
    if (primary.canvas.ReadyToDraw()) {
        if (!StartCaptureForMonitor(mon)) { Stop(); return false; }
    } else {
        Log("canvas not ready, deferring capture to CreateResources");
        primary.canvas.CreateResources([mon](auto&&, auto&&)
        {
            if (g.active && !g.session) StartCaptureForMonitor(mon);
        });
    }
    return true;
}

bool LiquidGlassBackdrop::AttachLens(HWND hwnd, MUXC::Grid const& host) {
    if (!g.active || !hwnd) return false;

    if (Lens* existing = FindLens(hwnd)) {
        // Ya registrada. Si el host cambió (el flyout se reconstruye entero
        // al cambiar tema), hay que mudar el canvas al árbol nuevo — si no,
        // la lente quedaría colgando de un Grid que ya nadie muestra.
        if (existing->host == host) {
            existing->canvas.Invalidate();
            return true;
        }
        DetachCanvas(*existing);
        existing->hwnd = hwnd;
        if (!AttachCanvas(*existing, host)) {
            existing->hwnd = nullptr;
            return false;
        }
        existing->canvas.Invalidate();
        Log("lens re-hosted");
        return true;
    }

    for (auto& l : g.lenses) {
        if (l.InUse()) continue;
        l.hwnd = hwnd;
        if (!AttachCanvas(l, host)) {
            l.hwnd = nullptr;
            return false;
        }
        // Que no salga en el snapshot cuando se vuelva a capturar.
        if (g.session) SetWindowDisplayAffinity(hwnd, WDA_EXCLUDEFROMCAPTURE);
        // La principal pudo irse al tray y soltar la captura: si esta lente
        // se muestra y no hay snapshot, hay que tomar uno.
        if (!g.lastFrame) DoRecapture();
        l.canvas.Invalidate();
        Log("lens attached");
        return true;
    }
    Log("AttachLens: no free slot");
    return false;
}

void LiquidGlassBackdrop::DetachLens(HWND hwnd) {
    Lens* l = FindLens(hwnd);
    if (!l || l == &g.lenses[0]) return;   // la primaria se quita con Stop()
    DetachCanvas(*l);
}

void LiquidGlassBackdrop::Stop() {
    if (!g.active) return;
    RemoveEventHooks();
    StopCapture();
    for (auto& l : g.lenses)
        if (l.InUse()) DetachCanvas(l);
    g.active = false;
}

void LiquidGlassBackdrop::RequestRedraw() {
    if (!g.active) return;
    // Mover/redimensionar solo cambia QUÉ zona del snapshot se muestrea:
    // re-muestrear es gratis y NO se recaptura (recapturar en cada
    // movimiento era la causa del jitter al arrastrar la ventana).
    InvalidateAllLenses();
}

void LiquidGlassBackdrop::RefreshCapture() {
    if (!g.active) return;
    ScheduleRecapture();
}

void LiquidGlassBackdrop::SetFrost(bool frost) {
    if (!g.active || g.frost == frost) return;
    g.frost = frost;
    InvalidateAllLenses();   // el grafo se reconstruye solo (graphIsFrost)
}

void LiquidGlassBackdrop::OnWindowVisibility(bool visible) {
    if (!g.active) return;
    if (!visible) {
        // Al tray: liberar framepool (2 buffers del tamaño del monitor),
        // último frame y grafo — el costo del glass cae a ~0.
        StopCapture();
        Log("capture released (window hidden)");
    } else if (!g.session) {
        DoRecapture();
        Log("capture rearmed (window shown)");
    }
}

bool LiquidGlassBackdrop::IsActive() {
    return g.active;
}

}  // namespace mutemic
