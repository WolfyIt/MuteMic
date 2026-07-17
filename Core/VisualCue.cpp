#include "pch.h"
#include "VisualCue.h"

#include <algorithm>
#include <atomic>
#include <objidl.h>
namespace Gdiplus { using std::min; using std::max; }
#include <gdiplus.h>

#include <d3d11.h>
#include <dxgi1_3.h>
#include <d2d1_1.h>
#include <dcomp.h>
#include <dwmapi.h>

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dcomp.lib")

#include "IconRenderer.h"  // Startup() de GDI+

namespace mutemic {
namespace {

namespace G = Gdiplus;

// ── Arquitectura anti-FPS-drop, v2 ──
// v1 usaba UpdateLayeredWindow: cada ventanita carga una superficie de
// redirección que DWM compone por CPU encima del juego → lo saca de
// independent flip (165→~112 fps con 4 regiones).
// v2 presenta por DirectComposition: ventanas WS_EX_NOREDIRECTIONBITMAP
// (SIN superficie de redirección) + swapchain flip-model premultiplicado.
// El contenido es un visual que DWM puede mandar a un plano de hardware
// (MPO) sin degradar al juego, y el hilo de render se cadencia con
// DwmFlush() = vsync exacto al refresh del monitor (60/144/165…).
// El artwork sigue siendo GDI+ sobre un DIB persistente (barato, regiones
// chicas); solo cambió la presentación. Si D3D/DComp fallan (GPU rara,
// driver viejo) cae al camino v1 con timer.

constexpr wchar_t kClass[] = L"MuteMicCueOverlay";
constexpr UINT_PTR kAnimTimer = 1;
constexpr UINT kMsgAnimDone = WM_APP + 7;   // el hilo terminó: Term en UI
constexpr int kFrameMs = 33;                // solo fallback ULW
constexpr int kInMs = 150, kHoldMs = 620, kOutMs = 360;
constexpr int kTotalMs = kInMs + kHoldMs + kOutMs;
constexpr int kIconLagMs = 80;

constexpr wchar_t kMicGlyph[] = { 0xE720, 0 };
constexpr int kMaxRegions = 4;

// Qué dibuja cada región (en coords LOCALES de su ventanita).
enum class RegionKind {
    EdgeTop,        // franja superior del anillo + mic badge
    EdgeBottom,
    EdgeLeft,
    EdgeRight,
    CornerTL,       // escuadra + mic badge
    CornerTR,
    CornerBL,
    CornerBR,
    Notch,
};

struct Region {
    HWND hwnd = nullptr;
    RECT rc{};              // rect en pantalla (px)
    RegionKind kind{};
    HDC dc = nullptr;       // buffer persistente por animación
    HBITMAP dib = nullptr;
    HGDIOBJ oldBmp = nullptr;
    void* bits = nullptr;   // pixels del DIB (premultiplied BGRA)

    // Camino DirectComposition
    winrt::com_ptr<IDXGISwapChain1> swap;
    winrt::com_ptr<IDCompositionTarget> dcTarget;
    winrt::com_ptr<IDCompositionVisual> dcVisual;
    winrt::com_ptr<ID2D1Bitmap1> srcBmp;
};

struct CueState {
    bool active = false;
    bool muted = false;
    UINT style = 0;
    UINT edge = 1;
    bool light = false;
    bool glass = false;
    bool frost = true;
    bool fgFullscreen = false;
    RECT mon{};
    RECT work{};
    LARGE_INTEGER qpcStart{};
    Region regions[kMaxRegions];
    int regionCount = 0;
};
CueState c;

// Dispositivos compartidos (se crean una vez, viven todo el proceso).
struct Gfx {
    bool tried = false;
    bool ok = false;
    winrt::com_ptr<ID3D11Device> d3d;
    winrt::com_ptr<IDXGIDevice> dxgi;
    winrt::com_ptr<IDXGIFactory2> factory;
    winrt::com_ptr<IDCompositionDevice> dcomp;
    winrt::com_ptr<ID2D1Factory1> d2dFactory;
    winrt::com_ptr<ID2D1Device> d2dDev;
    winrt::com_ptr<ID2D1DeviceContext> d2dCtx;
};
Gfx gfx;

HANDLE g_thread = nullptr;
std::atomic<bool> g_stop{ false };

double ElapsedMs() {
    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    return (now.QuadPart - c.qpcStart.QuadPart) * 1000.0 / freq.QuadPart;
}

bool InitGfx() {
    if (gfx.tried) return gfx.ok;
    gfx.tried = true;

    HRESULT hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
        gfx.d3d.put(), nullptr, nullptr);
    if (FAILED(hr)) return false;

    gfx.dxgi = gfx.d3d.try_as<IDXGIDevice>();
    if (!gfx.dxgi) return false;

    winrt::com_ptr<IDXGIAdapter> adapter;
    if (FAILED(gfx.dxgi->GetAdapter(adapter.put()))) return false;
    if (FAILED(adapter->GetParent(__uuidof(IDXGIFactory2),
                                  gfx.factory.put_void()))) return false;

    if (FAILED(DCompositionCreateDevice(gfx.dxgi.get(),
                                        __uuidof(IDCompositionDevice),
                                        gfx.dcomp.put_void()))) return false;

    D2D1_FACTORY_OPTIONS fo{};
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_MULTI_THREADED,
                                 __uuidof(ID2D1Factory1), &fo,
                                 gfx.d2dFactory.put_void()))) return false;
    if (FAILED(gfx.d2dFactory->CreateDevice(gfx.dxgi.get(),
                                            gfx.d2dDev.put()))) return false;
    if (FAILED(gfx.d2dDev->CreateDeviceContext(
            D2D1_DEVICE_CONTEXT_OPTIONS_NONE, gfx.d2dCtx.put()))) return false;

    gfx.ok = true;
    return true;
}

G::Color CueColor(bool muted, float a) {
    BYTE alpha = static_cast<BYTE>(std::clamp(a, 0.0f, 1.0f) * 255.0f);
    return muted ? G::Color(alpha, 0xFF, 0x31, 0x31)
                 : G::Color(alpha, 0x39, 0xFF, 0x14);
}

float Envelope(int t) {
    if (t <= 0) return 0.0f;
    if (t < kInMs) {
        const float x = t / static_cast<float>(kInMs);
        return 1.0f - (1.0f - x) * (1.0f - x);
    }
    if (t < kInMs + kHoldMs) return 1.0f;
    const int o = t - kInMs - kHoldMs;
    if (o >= kOutMs) return 0.0f;
    const float x = 1.0f - o / static_cast<float>(kOutMs);
    return x * x;
}

float Scale() { return (c.mon.bottom - c.mon.top) / 1440.0f; }

// Mic minimalista (estilo contador de FPS): sombra sutil + glifo.
void DrawMiniMic(G::Graphics& g, float x, float y, float size,
                 G::Color const& color, bool muted, float env) {
    G::FontFamily fluent(L"Segoe Fluent Icons");
    G::FontFamily mdl2(L"Segoe MDL2 Assets");
    const G::FontFamily* fam = fluent.IsAvailable() ? &fluent : &mdl2;

    G::StringFormat fmt;
    fmt.SetAlignment(G::StringAlignmentCenter);
    fmt.SetLineAlignment(G::StringAlignmentCenter);

    G::GraphicsPath shadow;
    G::RectF sbox(x + size * 0.05f, y + size * 0.07f, size, size);
    shadow.AddString(kMicGlyph, -1, fam, G::FontStyleRegular, size * 0.86f,
                     sbox, &fmt);
    G::SolidBrush sh(G::Color(static_cast<BYTE>(120 * env), 0, 0, 0));
    g.FillPath(&sh, &shadow);

    G::GraphicsPath glyph;
    G::RectF box(x, y, size, size);
    glyph.AddString(kMicGlyph, -1, fam, G::FontStyleRegular, size * 0.86f,
                    box, &fmt);
    G::SolidBrush fill(color);
    g.FillPath(&fill, &glyph);

    if (muted) {
        G::Pen slash(color, size * 0.10f);
        slash.SetStartCap(G::LineCapRound);
        slash.SetEndCap(G::LineCapRound);
        g.DrawLine(&slash, x + size * 0.16f, y + size * 0.84f,
                   x + size * 0.84f, y + size * 0.12f);
    }
}

// Rounded rect con radio arbitrario (figura cerrada dentro de un path).
void AddRoundRect(G::GraphicsPath& p, float x, float y, float w, float h,
                  float r) {
    r = std::min(r, std::min(w, h) * 0.5f);
    p.StartFigure();
    p.AddArc(x, y, 2 * r, 2 * r, 180, 90);
    p.AddArc(x + w - 2 * r, y, 2 * r, 2 * r, 270, 90);
    p.AddArc(x + w - 2 * r, y + h - 2 * r, 2 * r, 2 * r, 0, 90);
    p.AddArc(x, y + h - 2 * r, 2 * r, 2 * r, 90, 90);
    p.CloseFigure();
}

// Tira con gradiente perpendicular. dir: 0=desde arriba, 1=abajo, 2=izq, 3=der.
void DrawStrip(G::Graphics& g, float x, float y, float w, float h, int dir,
               G::Color const& col) {
    const G::Color clear(0, col.GetR(), col.GetG(), col.GetB());
    G::PointF g0, g1;
    switch (dir) {
        case 0: g0 = { x, y }; g1 = { x, y + h + 1 }; break;
        case 1: g0 = { x, y + h }; g1 = { x, y - 1 }; break;
        case 2: g0 = { x, y }; g1 = { x + w + 1, y }; break;
        case 3: g0 = { x + w, y }; g1 = { x - 1, y }; break;
    }
    G::GraphicsPath p;
    AddRoundRect(p, x, y, w, h, std::min(w, h) * 0.5f);
    G::LinearGradientBrush b(g0, g1, col, clear);
    b.SetWrapMode(G::WrapModeTileFlipXY);
    g.FillPath(&b, &p);
}

// ── Edges: anillo redondeado GLOBAL dibujado recortado por región ──
// Cada región traslada el path global a sus coords locales; los pixels
// fuera de su ventana se recortan solos → cero seams, cero traslapes.
void DrawEdgeRingLocal(G::Graphics& g, Region const& r, float env,
                       float envIcon) {
    const float s = Scale();
    const float th = 14.0f * s;
    const float R = 24.0f * s;
    const float monW = static_cast<float>(c.mon.right - c.mon.left);
    const float monH = static_cast<float>(c.mon.bottom - c.mon.top);
    const float ox = static_cast<float>(r.rc.left - c.mon.left);
    const float oy = static_cast<float>(r.rc.top - c.mon.top);

    // Anillo = rounded rect exterior − rounded rect interior (FillMode
    // Alternate con dos figuras).
    G::GraphicsPath ring;
    ring.SetFillMode(G::FillModeAlternate);
    AddRoundRect(ring, -ox, -oy, monW, monH, R);
    AddRoundRect(ring, -ox + th, -oy + th, monW - 2 * th, monH - 2 * th,
                 std::max(R - th, 2.0f));
    G::SolidBrush body(CueColor(c.muted, 0.85f * env));
    g.FillPath(&body, &ring);

    // Glow suave hacia adentro (un segundo anillo translúcido pegado).
    const float gw = th * 0.85f;
    G::GraphicsPath glow;
    glow.SetFillMode(G::FillModeAlternate);
    AddRoundRect(glow, -ox + th, -oy + th, monW - 2 * th, monH - 2 * th,
                 std::max(R - th, 2.0f));
    AddRoundRect(glow, -ox + th + gw, -oy + th + gw,
                 monW - 2 * (th + gw), monH - 2 * (th + gw),
                 std::max(R - th - gw, 2.0f));
    G::SolidBrush glowBrush(CueColor(c.muted, 0.22f * env));
    g.FillPath(&glowBrush, &glow);

    if (r.kind == RegionKind::EdgeTop) {
        DrawMiniMic(g, 26.0f * s, th + 10.0f * s, 26.0f * s,
                    CueColor(c.muted, envIcon), c.muted, envIcon);
    }
}

// ── Notch (gota) en coords locales de su ventanita ──
void DrawNotchLocal(G::Graphics& g, int W, int H, float env, float envIcon) {
    const float s = Scale();
    const float iw = 96.0f * s;
    const float bw = 150.0f * s;
    const float ext = 34.0f * s;
    const float depth = 52.0f * s;
    const float d = depth * env;
    if (d < 1.0f) return;

    // Eje "u" = lado largo local; profundidad "v" desde la base.
    const bool horiz = (c.edge < 2);
    const float axisLen = horiz ? static_cast<float>(W) : static_cast<float>(H);
    const float cu = axisLen * 0.5f;
    const float uA = cu - bw * 0.5f - ext, uB = cu - iw * 0.5f;
    const float uC = cu + iw * 0.5f, uD = cu + bw * 0.5f + ext;

    auto buildPath = [&](G::GraphicsPath& p, bool closeBase) {
        p.AddBezier(uA, 0.0f, uA + ext * 0.9f, 0.0f, uB - iw * 0.35f, d, uB, d);
        p.AddLine(uB, d, uC, d);
        p.AddBezier(uC, d, uC + iw * 0.35f, d, uD - ext * 0.9f, 0.0f, uD, 0.0f);
        if (closeBase) p.CloseFigure();
    };
    G::GraphicsPath rim, body;
    buildPath(rim, false);   // frente abierto: el rim no toca el bezel
    buildPath(body, true);

    // Mapear (u,v) al borde local correspondiente.
    G::Matrix mtx;
    switch (c.edge) {
        case 0: break;                                             // top
        case 1: mtx.SetElements(1, 0, 0, -1, 0, static_cast<float>(H)); break;
        case 2: mtx.SetElements(0, 1, 1, 0, 0, 0); break;          // left
        case 3: mtx.SetElements(0, 1, -1, 0, static_cast<float>(W), 0); break;
    }
    rim.Transform(&mtx);
    body.Transform(&mtx);

    BYTE bodyA = c.glass ? (c.frost ? 205 : 150) : 240;
    G::Color bodyCol = c.light
        ? G::Color(static_cast<BYTE>(bodyA * env), 243, 245, 249)
        : G::Color(static_cast<BYTE>(bodyA * env), 9, 10, 13);
    G::SolidBrush bodyBrush(bodyCol);
    g.FillPath(&bodyBrush, &body);

    G::Pen rimPen(CueColor(c.muted, 0.60f * env), 2.2f * s);
    rimPen.SetStartCap(G::LineCapRound);
    rimPen.SetEndCap(G::LineCapRound);
    g.DrawPath(&rimPen, &rim);

    // Mic clippeado al cuerpo: nunca se asoma fuera del notch.
    G::RectF bounds;
    body.GetBounds(&bounds);
    const float icon = 26.0f * s;
    G::PointF center(bounds.X + bounds.Width / 2, bounds.Y + bounds.Height / 2);
    switch (c.edge) {
        case 0: center.Y = bounds.Y + bounds.Height * 0.58f; break;
        case 1: center.Y = bounds.Y + bounds.Height * 0.42f; break;
        case 2: center.X = bounds.X + bounds.Width * 0.58f; break;
        case 3: center.X = bounds.X + bounds.Width * 0.42f; break;
    }
    g.SetClip(&body);
    DrawMiniMic(g, center.X - icon / 2, center.Y - icon / 2, icon,
                CueColor(c.muted, envIcon), c.muted, envIcon);
    g.ResetClip();
}

// Dibuja una región (coords locales). W/H = tamaño de SU ventana.
void DrawRegion(G::Graphics& g, Region const& r, float env, float envIcon) {
    const float s = Scale();
    const float th = 14.0f * s;
    const float arm = std::min(c.mon.right - c.mon.left,
                               c.mon.bottom - c.mon.top) * 0.085f;
    const int W = r.rc.right - r.rc.left;
    const int H = r.rc.bottom - r.rc.top;
    const G::Color col = CueColor(c.muted, 0.80f * env);

    switch (r.kind) {
        case RegionKind::EdgeTop:
        case RegionKind::EdgeBottom:
        case RegionKind::EdgeLeft:
        case RegionKind::EdgeRight:
            DrawEdgeRingLocal(g, r, env, envIcon);
            break;
        case RegionKind::CornerTL:
            DrawStrip(g, 0, 0, arm, th, 0, col);
            DrawStrip(g, 0, 0, th, arm, 2, col);
            DrawMiniMic(g, th + 10.0f * s, th + 10.0f * s, 26.0f * s,
                        CueColor(c.muted, envIcon), c.muted, envIcon);
            break;
        case RegionKind::CornerTR:
            DrawStrip(g, W - arm, 0, arm, th, 0, col);
            DrawStrip(g, W - th, 0, th, arm, 3, col);
            break;
        case RegionKind::CornerBL:
            DrawStrip(g, 0, H - th, arm, th, 1, col);
            DrawStrip(g, 0, H - arm, th, arm, 2, col);
            break;
        case RegionKind::CornerBR:
            DrawStrip(g, W - arm, H - th, arm, th, 1, col);
            DrawStrip(g, W - th, H - arm, th, arm, 3, col);
            break;
        case RegionKind::Notch:
            DrawNotchLocal(g, W, H, env, envIcon);
            break;
    }
}

// Pinta el DIB de cada región para el tiempo t (compartido por ambos caminos).
void PaintRegions(int t) {
    const float env = Envelope(t);
    // El icono entra atrasado pero SALE adelantado (se esconde antes de que
    // el cuerpo se retraiga).
    const float envIcon = (t < kInMs + kHoldMs / 2)
                              ? Envelope(t - kIconLagMs)
                              : Envelope(t + kIconLagMs);
    for (int i = 0; i < c.regionCount; ++i) {
        auto& r = c.regions[i];
        if (!r.dc) continue;
        G::Graphics g(r.dc);
        g.SetSmoothingMode(G::SmoothingModeAntiAlias);
        g.SetPixelOffsetMode(G::PixelOffsetModeHighQuality);
        g.Clear(G::Color(0, 0, 0, 0));
        DrawRegion(g, r, env, envIcon);
        g.Flush();
    }
    GdiFlush();   // asegura que los bits del DIB estén escritos
}

// ── Presentación DirectComposition: DIB → D2D bitmap → swapchain ──
void PresentRegionsDComp() {
    for (int i = 0; i < c.regionCount; ++i) {
        auto& r = c.regions[i];
        if (!r.swap || !r.srcBmp || !r.bits) continue;
        const UINT w = r.rc.right - r.rc.left;

        r.srcBmp->CopyFromMemory(nullptr, r.bits, w * 4);

        winrt::com_ptr<IDXGISurface> surf;
        if (FAILED(r.swap->GetBuffer(0, __uuidof(IDXGISurface),
                                     surf.put_void()))) continue;
        D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                              D2D1_ALPHA_MODE_PREMULTIPLIED));
        winrt::com_ptr<ID2D1Bitmap1> target;
        if (FAILED(gfx.d2dCtx->CreateBitmapFromDxgiSurface(
                surf.get(), &bp, target.put()))) continue;

        gfx.d2dCtx->SetTarget(target.get());
        gfx.d2dCtx->BeginDraw();
        gfx.d2dCtx->Clear(D2D1::ColorF(0, 0, 0, 0));
        gfx.d2dCtx->DrawBitmap(r.srcBmp.get());
        gfx.d2dCtx->EndDraw();
        gfx.d2dCtx->SetTarget(nullptr);

        r.swap->Present(0, 0);
    }
}

// Hilo de render: un frame por composición de DWM (= refresh del monitor).
DWORD WINAPI RenderThread(void*) {
    HWND notify = c.regions[0].hwnd;
    while (!g_stop.load(std::memory_order_relaxed)) {
        const int t = static_cast<int>(ElapsedMs());
        if (t >= kTotalMs) {
            PostMessageW(notify, kMsgAnimDone, 0, 0);
            break;
        }
        PaintRegions(t);
        PresentRegionsDComp();
        DwmFlush();   // vsync del compositor: 60/144/165 Hz, lo que sea
    }
    return 0;
}

void StopThread() {
    if (!g_thread) return;
    g_stop.store(true, std::memory_order_relaxed);
    WaitForSingleObject(g_thread, 1000);
    CloseHandle(g_thread);
    g_thread = nullptr;
    g_stop.store(false, std::memory_order_relaxed);
}

void DestroyRegions() {
    StopThread();
    for (int i = 0; i < c.regionCount; ++i) {
        auto& r = c.regions[i];
        r.dcVisual = nullptr;
        r.dcTarget = nullptr;
        r.srcBmp = nullptr;
        r.swap = nullptr;
        if (r.dc) {
            SelectObject(r.dc, r.oldBmp);
            DeleteDC(r.dc);
            r.dc = nullptr;
        }
        if (r.dib) {
            DeleteObject(r.dib);
            r.dib = nullptr;
        }
        r.bits = nullptr;
        if (r.hwnd) {
            KillTimer(r.hwnd, kAnimTimer);
            DestroyWindow(r.hwnd);
            r.hwnd = nullptr;
        }
    }
    if (gfx.ok && gfx.dcomp) gfx.dcomp->Commit();
    c.regionCount = 0;
    c.active = false;
}

// Fallback v1: UpdateLayeredWindow con timer (solo si no hay D3D/DComp).
void RenderAllULW() {
    if (!c.active) return;
    const int t = static_cast<int>(ElapsedMs());
    if (t >= kTotalMs) {
        VisualCue::Term();
        return;
    }
    PaintRegions(t);
    // Si las regiones son DComp (hilo no arrancó), presentar por swapchain.
    if (c.regions[0].swap) {
        PresentRegionsDComp();
        return;
    }
    HDC screen = GetDC(nullptr);
    for (int i = 0; i < c.regionCount; ++i) {
        auto& r = c.regions[i];
        if (!r.hwnd || !r.dc) continue;
        POINT dst{ r.rc.left, r.rc.top };
        POINT srcPt{ 0, 0 };
        SIZE size{ r.rc.right - r.rc.left, r.rc.bottom - r.rc.top };
        BLENDFUNCTION blend{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
        UpdateLayeredWindow(r.hwnd, screen, &dst, &size, r.dc, &srcPt,
                            0, &blend, ULW_ALPHA);
    }
    ReleaseDC(nullptr, screen);
}

LRESULT CALLBACK CueProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_TIMER:
            if (wParam == kAnimTimer) { RenderAllULW(); return 0; }
            break;
        case WM_NCHITTEST:
            return HTTRANSPARENT;   // click-through garantizado
        default:
            if (msg == kMsgAnimDone) { VisualCue::Term(); return 0; }
            break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

bool AddRegion(RECT rc, RegionKind kind, bool useDComp) {
    if (c.regionCount >= kMaxRegions) return false;
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = CueProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = kClass;
        RegisterClassW(&wc);
        registered = true;
    }

    auto& r = c.regions[c.regionCount];
    r.rc = rc;
    r.kind = kind;
    const int w = rc.right - rc.left, h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return false;

    DWORD ex = WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW |
               WS_EX_TOPMOST;
    ex |= useDComp ? WS_EX_NOREDIRECTIONBITMAP : WS_EX_LAYERED;

    r.hwnd = CreateWindowExW(ex, kClass, L"", WS_POPUP,
                             rc.left, rc.top, w, h,
                             nullptr, nullptr, GetModuleHandleW(nullptr),
                             nullptr);
    if (!r.hwnd) return false;

    // Buffer persistente para toda la animación (fuente del artwork).
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    HDC screen = GetDC(nullptr);
    r.dc = CreateCompatibleDC(screen);
    r.dib = CreateDIBSection(r.dc, &bi, DIB_RGB_COLORS, &r.bits, nullptr, 0);
    ReleaseDC(nullptr, screen);
    if (!r.dc || !r.dib) {
        DestroyWindow(r.hwnd);
        r.hwnd = nullptr;
        return false;
    }
    r.oldBmp = SelectObject(r.dc, r.dib);

    if (useDComp) {
        DXGI_SWAP_CHAIN_DESC1 sd{};
        sd.Width = w;
        sd.Height = h;
        sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        sd.SampleDesc = { 1, 0 };
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = 2;
        sd.Scaling = DXGI_SCALING_STRETCH;
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        sd.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
        HRESULT hr = gfx.factory->CreateSwapChainForComposition(
            gfx.d3d.get(), &sd, nullptr, r.swap.put());
        if (SUCCEEDED(hr))
            hr = gfx.dcomp->CreateTargetForHwnd(r.hwnd, TRUE,
                                                r.dcTarget.put());
        if (SUCCEEDED(hr)) hr = gfx.dcomp->CreateVisual(r.dcVisual.put());
        if (SUCCEEDED(hr)) {
            r.dcVisual->SetContent(r.swap.get());
            r.dcTarget->SetRoot(r.dcVisual.get());
        }
        if (SUCCEEDED(hr)) {
            D2D1_BITMAP_PROPERTIES1 sp = D2D1::BitmapProperties1(
                D2D1_BITMAP_OPTIONS_NONE,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                                  D2D1_ALPHA_MODE_PREMULTIPLIED));
            hr = gfx.d2dCtx->CreateBitmap(
                D2D1::SizeU(w, h), nullptr, 0, &sp, r.srcBmp.put());
        }
        if (FAILED(hr)) {
            // Sin swapchain esta región no puede presentar: abortar región.
            r.srcBmp = nullptr;
            r.dcVisual = nullptr;
            r.dcTarget = nullptr;
            r.swap = nullptr;
            SelectObject(r.dc, r.oldBmp);
            DeleteDC(r.dc);
            DeleteObject(r.dib);
            r.dc = nullptr;
            r.dib = nullptr;
            r.bits = nullptr;
            DestroyWindow(r.hwnd);
            r.hwnd = nullptr;
            return false;
        }
    }

    ShowWindow(r.hwnd, SW_SHOWNOACTIVATE);
    ++c.regionCount;
    return true;
}

}  // namespace

void VisualCue::Show(bool muted, UINT style, UINT edge,
                     bool lightTheme, bool glass, bool frost) {
    if (style == 0 || style > 3) return;
    IconRenderer::Startup();

    // Cue en curso: reiniciar limpio (regiones distintas por estilo).
    Term();

    const bool useDComp = InitGfx();

    HWND fg = GetForegroundWindow();
    HMONITOR mon = fg ? MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST) : nullptr;
    if (!mon) {
        POINT pt;
        GetCursorPos(&pt);
        mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    }
    MONITORINFO mi{ sizeof(mi) };
    GetMonitorInfoW(mon, &mi);

    c.muted = muted;
    c.style = style;
    c.edge = edge;
    c.light = lightTheme;
    c.glass = glass;
    c.frost = frost;
    c.mon = mi.rcMonitor;
    c.work = mi.rcWork;
    QueryPerformanceCounter(&c.qpcStart);

    c.fgFullscreen = false;
    if (fg) {
        RECT fr{};
        GetWindowRect(fg, &fr);
        c.fgFullscreen = fr.left <= mi.rcMonitor.left && fr.top <= mi.rcMonitor.top &&
                         fr.right >= mi.rcMonitor.right && fr.bottom >= mi.rcMonitor.bottom;
    }

    const float s = (c.mon.bottom - c.mon.top) / 1440.0f;
    const int th = static_cast<int>(14.0f * s);
    const int ringR = static_cast<int>(24.0f * s);
    const int monW = c.mon.right - c.mon.left;
    const int monH = c.mon.bottom - c.mon.top;
    c.active = true;

    if (style == 1) {
        // Edges: anillo global partido en 4 franjas SIN traslape.
        // Las esquinas (arcos, radio ringR) viven en las franjas top/bottom;
        // las laterales cubren solo la parte recta (grosor th).
        const int topH = std::max(static_cast<int>(64.0f * s), ringR + 2);
        const int botH = std::max(th, ringR) + 2;
        AddRegion({ c.mon.left, c.mon.top, c.mon.right, c.mon.top + topH },
                  RegionKind::EdgeTop, useDComp);
        AddRegion({ c.mon.left, c.mon.bottom - botH, c.mon.right, c.mon.bottom },
                  RegionKind::EdgeBottom, useDComp);
        AddRegion({ c.mon.left, c.mon.top + topH,
                    c.mon.left + th + 2, c.mon.bottom - botH },
                  RegionKind::EdgeLeft, useDComp);
        AddRegion({ c.mon.right - th - 2, c.mon.top + topH,
                    c.mon.right, c.mon.bottom - botH },
                  RegionKind::EdgeRight, useDComp);
    } else if (style == 2) {
        // Corners: 4 ventanitas cuadradas (TL más grande por el badge).
        const int arm = static_cast<int>(std::min(monW, monH) * 0.085f) + th;
        const int tl = std::max(arm, static_cast<int>(150.0f * s));
        AddRegion({ c.mon.left, c.mon.top, c.mon.left + tl, c.mon.top + tl },
                  RegionKind::CornerTL, useDComp);
        AddRegion({ c.mon.right - arm, c.mon.top, c.mon.right, c.mon.top + arm },
                  RegionKind::CornerTR, useDComp);
        AddRegion({ c.mon.left, c.mon.bottom - arm, c.mon.left + arm, c.mon.bottom },
                  RegionKind::CornerBL, useDComp);
        AddRegion({ c.mon.right - arm, c.mon.bottom - arm, c.mon.right, c.mon.bottom },
                  RegionKind::CornerBR, useDComp);
    } else {
        // Notch: UNA ventanita del tamaño de la gota (+margen de curvas).
        const int along = static_cast<int>(320.0f * s);
        const int depth = static_cast<int>(96.0f * s);
        RECT rc{};
        if (edge == 0) {
            const int cx = c.mon.left + monW / 2;
            rc = { cx - along / 2, c.mon.top, cx + along / 2, c.mon.top + depth };
        } else if (edge == 1) {
            const bool taskbarVisible =
                !c.fgFullscreen && (c.work.bottom < c.mon.bottom);
            const int baseY = taskbarVisible ? c.work.bottom : c.mon.bottom;
            const int cx = c.mon.left + monW / 2;
            rc = { cx - along / 2, baseY - depth, cx + along / 2, baseY };
        } else if (edge == 2) {
            const int cy = c.mon.top + monH / 2;
            rc = { c.mon.left, cy - along / 2, c.mon.left + depth, cy + along / 2 };
        } else {
            const int cy = c.mon.top + monH / 2;
            rc = { c.mon.right - depth, cy - along / 2, c.mon.right, cy + along / 2 };
        }
        AddRegion(rc, RegionKind::Notch, useDComp);
    }

    if (c.regionCount == 0) {
        c.active = false;
        return;
    }

    if (useDComp) {
        gfx.dcomp->Commit();   // publica los visuals de todas las regiones
        g_stop.store(false, std::memory_order_relaxed);
        g_thread = CreateThread(nullptr, 0, RenderThread, nullptr, 0, nullptr);
        if (!g_thread) {
            // Sin hilo no hay animación: degradar a timer + present manual.
            SetTimer(c.regions[0].hwnd, kAnimTimer, kFrameMs, nullptr);
        }
    } else {
        SetTimer(c.regions[0].hwnd, kAnimTimer, kFrameMs, nullptr);
        RenderAllULW();
    }
}

void VisualCue::Term() {
    DestroyRegions();
}

}  // namespace mutemic
