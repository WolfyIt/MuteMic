#include "pch.h"
#include "LiquidGlassBackdrop.h"

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

struct State {
    bool active = false;
    bool frost = false;
    bool shareFriendly = false;
    HWND hwnd = nullptr;
    HMONITOR monitor = nullptr;

    MUXC::Grid host{ nullptr };
    MGCX::CanvasControl canvas{ nullptr };
    MGCX::CanvasControl::Draw_revoker drawRevoker{};

    WGC::GraphicsCaptureItem item{ nullptr };
    WGC::Direct3D11CaptureFramePool framePool{ nullptr };
    WGC::GraphicsCaptureSession session{ nullptr };
    WGC::Direct3D11CaptureFramePool::FrameArrived_revoker frameRevoker{};

    MGC::CanvasBitmap lastFrame{ nullptr };
    std::vector<uint8_t> shader;
    ULONGLONG lastWrapMs = 0;   // throttle del pipeline a ~30 fps

    // Grafo de efectos CACHEADO: construir el PixelShaderEffect (parsea el
    // blob) en cada Draw causa jitter. Se arma una vez y por frame solo se
    // actualizan fuente, traslación y winSize.
    MGC::Effects::Transform2DEffect fxShift{ nullptr };
    MGC::Effects::Transform2DEffect fxDown{ nullptr };   // blur a media res
    MGC::Effects::GaussianBlurEffect fxBlur{ nullptr };
    MGC::Effects::Transform2DEffect fxUp{ nullptr };
    MGC::Effects::PixelShaderEffect fxGlass{ nullptr };
    bool graphIsFrost = false;
};

State g;

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

bool StartCaptureForMonitor(HMONITOR monitor) {
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
    Log("CreateForMonitor ok");

    // El device del CanvasControl ES un IDirect3DDevice: mismo device para
    // captura y para envolver los frames como CanvasBitmap (requisito).
    try {
        auto device = g.canvas.Device();

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
            if (!frame || !g.canvas) return;

            // Ventana oculta (en el tray): no gastar GPU en un backdrop
            // que nadie ve. El frame se consume igual para drenar el pool.
            if (g.hwnd && !IsWindowVisible(g.hwnd)) return;

            // Throttle: el escritorio puede entregar frames a 144 Hz
            // (wallpaper animado, video). Para un fondo de cristal, 30 fps
            // son indistinguibles y cuestan una fracción.
            const ULONGLONG now = GetTickCount64();
            if (now - g.lastWrapMs < 33) return;
            g.lastWrapMs = now;

            try {
                const bool first = (g.lastFrame == nullptr);
                auto wrapped = MGC::CanvasBitmap::CreateFromDirect3D11Surface(
                    g.canvas.Device(), frame.Surface());

                if (g.shareFriendly) {
                    // COPIA real del frame (el buffer del pool se recicla al
                    // cerrar la captura; un wrap colgaría de memoria muerta).
                    MGC::CanvasRenderTarget copy(
                        g.canvas.Device(),
                        static_cast<float>(wrapped.SizeInPixels().Width),
                        static_cast<float>(wrapped.SizeInPixels().Height),
                        96.0f);
                    {
                        auto cds = copy.CreateDrawingSession();
                        cds.DrawImage(wrapped);
                        cds.Close();
                    }
                    g.lastFrame = copy;
                    // Snapshot tomado: cerrar captura y volvernos VISIBLES
                    // en capturas/screen share. Un movimiento re-dispara
                    // el ciclo (ver RequestRedraw).
                    g.frameRevoker.revoke();
                    if (g.session) { g.session.Close(); g.session = nullptr; }
                    if (g.framePool) { g.framePool.Close(); g.framePool = nullptr; }
                    g.item = nullptr;
                    if (g.hwnd) SetWindowDisplayAffinity(g.hwnd, WDA_NONE);
                } else {
                    g.lastFrame = wrapped;
                }

                if (first) Log("first frame arrived");
                g.canvas.Invalidate();
            } catch (winrt::hresult_error const& e) {
                Log("FrameArrived wrap FAILED", e.code());
            } catch (...) {
                Log("FrameArrived wrap FAILED (unknown)");
            }
        });

    g.session.StartCapture();
    Log("capture started");
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
    g.fxShift = nullptr;
    g.fxDown = nullptr;
    g.fxBlur = nullptr;
    g.fxUp = nullptr;
    g.fxGlass = nullptr;
}

void OnDraw(MGCX::CanvasControl const& sender, MGCX::CanvasDrawEventArgs const& args) {
    if (!g.active || !g.hwnd) return;

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
    GetClientRect(g.hwnd, &cr);
    POINT tl{ 0, 0 };
    ClientToScreen(g.hwnd, &tl);
    MONITORINFO mi{ sizeof(mi) };
    HMONITOR mon = MonitorFromWindow(g.hwnd, MONITOR_DEFAULTTONEAREST);
    GetMonitorInfoW(mon, &mi);

    if (mon != g.monitor) {
        // Cambió de monitor: reiniciar la captura ahí; el próximo frame pinta.
        StopCapture();
        StartCaptureForMonitor(mon);
        return;
    }

    const int w = cr.right, h = cr.bottom;
    if (w <= 0 || h <= 0) return;

    const float ox = static_cast<float>(tl.x - mi.rcMonitor.left);
    const float oy = static_cast<float>(tl.y - mi.rcMonitor.top);
    const GlassParams& P = g.frost ? kFrosted : kClear;

    const char* stage = "start";
    try {
        // ── Grafo cacheado: se construye una vez (o si cambió frost) ──
        if (!g.fxGlass || g.graphIsFrost != g.frost) {
            stage = "build graph";
            g.fxShift = MGC::Effects::Transform2DEffect();

            winrt::Windows::Graphics::Effects::IGraphicsEffectSource src = g.fxShift;
            if (P.blurPx > 0.1f) {
                // Blur a MEDIA resolución (como el "Blur downscale 0.5" del
                // repo): reduce ~4× el costo GPU del gaussian; el resultado
                // ya viene borroso así que el upscale no pierde nada.
                using winrt::Windows::Foundation::Numerics::make_float3x2_scale;
                g.fxDown = MGC::Effects::Transform2DEffect();
                g.fxDown.Source(g.fxShift);
                g.fxDown.TransformMatrix(make_float3x2_scale(0.5f));

                g.fxBlur = MGC::Effects::GaussianBlurEffect();
                g.fxBlur.Source(g.fxDown);
                g.fxBlur.BlurAmount(P.blurPx * 0.5f);
                g.fxBlur.BorderMode(MGC::Effects::EffectBorderMode::Hard);

                g.fxUp = MGC::Effects::Transform2DEffect();
                g.fxUp.Source(g.fxBlur);
                g.fxUp.TransformMatrix(make_float3x2_scale(2.0f));
                src = g.fxUp;
            } else {
                g.fxDown = nullptr;
                g.fxBlur = nullptr;
                g.fxUp = nullptr;
            }

            g.fxGlass = MGC::Effects::PixelShaderEffect(
                winrt::array_view<uint8_t const>(g.shader.data(),
                                                 g.shader.data() + g.shader.size()));
            g.fxGlass.Source1(src);

            auto props = g.fxGlass.Properties();
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
            g.graphIsFrost = g.frost;
        }

        // ── Por frame: fuente, traslación y tamaño ──
        stage = "per-frame update";
        g.fxShift.Source(g.lastFrame);
        g.fxShift.TransformMatrix(
            winrt::Windows::Foundation::Numerics::make_float3x2_translation(-ox, -oy));
        g.fxGlass.Properties().Insert(L"winSize", winrt::box_value(
            winrt::Windows::Foundation::Numerics::float2{
                static_cast<float>(w), static_cast<float>(h) }));

        stage = "DrawImage";
        // TODO en píxeles físicos (ds.Units = Pixels): el capture, el rect
        // de la ventana y la posición de escena del shader quedan en el
        // mismo espacio — sin esto la lente sale descentrada (el "blob").
        ds.Units(MGC::CanvasUnits::Pixels);
        ds.DrawImage(g.fxGlass,
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

}  // namespace

bool LiquidGlassBackdrop::Start(HWND hwnd, MUXC::Grid const& host, bool frost,
                                bool shareFriendly) {
    if (!WGC::GraphicsCaptureSession::IsSupported()) {
        Log("GraphicsCapture NOT supported");
        return false;
    }

    if (g.active && g.hwnd == hwnd) {
        const bool modeChanged = (g.shareFriendly != shareFriendly);
        g.frost = frost;
        g.shareFriendly = shareFriendly;
        if (modeChanged) {
            // Reiniciar el ciclo de captura con la política nueva.
            StopCapture();
            SetWindowDisplayAffinity(hwnd, WDA_EXCLUDEFROMCAPTURE);
            StartCaptureForMonitor(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST));
        }
        RequestRedraw();
        return true;
    }
    g.shareFriendly = shareFriendly;

    Log("=== glass Start ===");
    g.shader = LoadShaderBlob();
    if (g.shader.empty()) {
        Log("LiquidGlass.cso NOT FOUND next to exe");
        return false;
    }
    Log("shader blob loaded");

    g.hwnd = hwnd;
    g.host = host;
    g.frost = frost;

    // Canvas como PRIMER hijo del grid (detrás de todo), a pantalla completa.
    try {
        g.canvas = MGCX::CanvasControl();
    } catch (winrt::hresult_error const& e) {
        // Típico: falta Microsoft.Graphics.Canvas.dll junto al exe.
        Log("CanvasControl create FAILED", e.code());
        g.hwnd = nullptr;
        g.host = nullptr;
        return false;
    }
    g.canvas.ClearColor(winrt::Windows::UI::ColorHelper::FromArgb(255, 16, 18, 22));
    winrt::Microsoft::UI::Xaml::Controls::Grid::SetRow(g.canvas, 0);
    winrt::Microsoft::UI::Xaml::Controls::Grid::SetRowSpan(g.canvas, 2);
    g.drawRevoker = g.canvas.Draw(winrt::auto_revoke, &OnDraw);
    host.Children().InsertAt(0, g.canvas);

    // La ventana NO debe aparecer en su propia captura.
    SetWindowDisplayAffinity(hwnd, WDA_EXCLUDEFROMCAPTURE);

    HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    g.active = true;  // antes de StartCapture: FrameArrived usa g.canvas

    // La captura necesita el device del canvas; puede no existir hasta que
    // el control cargue. CreateResources dispara cuando esté listo.
    if (g.canvas.ReadyToDraw()) {
        Log("canvas ready, starting capture");
        if (!StartCaptureForMonitor(mon)) { Stop(); return false; }
    } else {
        Log("canvas not ready, deferring capture to CreateResources");
        g.canvas.CreateResources([mon](auto&&, auto&&)
        {
            Log("CreateResources fired");
            if (g.active && !g.session) StartCaptureForMonitor(mon);
        });
    }
    return true;
}

void LiquidGlassBackdrop::Stop() {
    if (!g.active) return;
    StopCapture();
    g.drawRevoker.revoke();
    if (g.host && g.canvas) {
        uint32_t index = 0;
        if (g.host.Children().IndexOf(g.canvas, index))
            g.host.Children().RemoveAt(index);
    }
    g.canvas = nullptr;
    g.host = nullptr;
    if (g.hwnd) SetWindowDisplayAffinity(g.hwnd, WDA_NONE);
    g.hwnd = nullptr;
    g.active = false;
}

void LiquidGlassBackdrop::RequestRedraw() {
    if (!g.active || !g.canvas) return;
    g.canvas.Invalidate();

    // Share-friendly: la ventana se movió y el snapshot quedó viejo —
    // re-disparar el ciclo excluir→capturar→des-excluir (debounced).
    if (g.shareFriendly && !g.session && g.hwnd && IsWindowVisible(g.hwnd)) {
        const ULONGLONG now = GetTickCount64();
        if (now - g.lastWrapMs > 150) {
            SetWindowDisplayAffinity(g.hwnd, WDA_EXCLUDEFROMCAPTURE);
            StartCaptureForMonitor(MonitorFromWindow(g.hwnd, MONITOR_DEFAULTTONEAREST));
        }
    }
}

void LiquidGlassBackdrop::OnWindowVisibility(bool visible) {
    if (!g.active) return;
    if (!visible) {
        // Al tray: liberar framepool (2 buffers del tamaño del monitor),
        // último frame y grafo — el costo del glass cae a ~0.
        StopCapture();
        Log("capture released (window hidden)");
    } else if (!g.session && g.hwnd) {
        HMONITOR mon = MonitorFromWindow(g.hwnd, MONITOR_DEFAULTTONEAREST);
        StartCaptureForMonitor(mon);
        Log("capture rearmed (window shown)");
    }
}

bool LiquidGlassBackdrop::IsActive() {
    return g.active;
}

}  // namespace mutemic
