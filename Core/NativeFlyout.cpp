#include "pch.h"
#include "NativeFlyout.h"
#include "Telemetry.h"

#include <algorithm>
#include <windowsx.h>          // GET_X_LPARAM
#include <shellscalingapi.h>   // GetDpiForMonitor
#include <d2d1_1.h>
#include <d2d1helper.h>
#include <d3d11.h>
#include <dcomp.h>
#include <dwmapi.h>
#include <dwrite.h>
#include <dxgi1_3.h>

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "dcomp.lib")

namespace mutemic {
namespace {

constexpr wchar_t kClass[] = L"MuteMicNativeFlyout";
// Cierre pedido desde el hook de mouse. Se procesa en el bucle de
// mensajes, NUNCA dentro del hook (ver MouseProc).
constexpr UINT kMsgDismiss = WM_APP + 1;

// Métricas en DIP (se escalan por el DPI del monitor donde se abre).
constexpr float kWidth = 280.0f;
constexpr float kRowH = 34.0f;
constexpr float kStateH = 44.0f;
constexpr float kPadX = 8.0f;
constexpr float kPadY = 8.0f;
constexpr float kRadius = 8.0f;
constexpr float kDividerH = 9.0f;
constexpr float kIconW = 30.0f;

// Glifos de Segoe Fluent Icons. Se escriben como arrays porque los
// caracteres de uso privado se corrompen al pegarlos literales.
constexpr wchar_t kGlyphMic[]    = { 0xE720, 0 };
constexpr wchar_t kGlyphMicOff[] = { 0xF781, 0 };
constexpr wchar_t kGlyphOpen[]   = { 0xE8A7, 0 };
constexpr wchar_t kGlyphPower[]  = { 0xE7E8, 0 };
constexpr wchar_t kGlyphDevice[] = { 0xE995, 0 };
constexpr wchar_t kGlyphCheck[]  = { 0xE73E, 0 };

struct Palette {
    D2D1_COLOR_F root, text, subtext, icon, divider, border, hover, accent;
};

Palette PaletteFor(UINT theme, bool glass) {
    auto rgba = [](UINT32 hex, float a) {
        return D2D1::ColorF(hex, a);
    };
    if (theme == 1) {   // light
        return { rgba(0xF6F7FA, glass ? 0.62f : 0.97f),
                 rgba(0x17181C, 1.0f), rgba(0x5A5D66, 1.0f),
                 rgba(0x17181C, 1.0f), rgba(0x000000, 0.10f),
                 rgba(0xFFFFFF, glass ? 0.22f : 0.13f),
                 rgba(0x000000, 0.06f), rgba(0x0B7A2E, 1.0f) };
    }
    return { rgba(0x0D0E11, glass ? 0.52f : 0.97f),
             rgba(0xE8E8F0, 1.0f), rgba(0x9AA0AC, 1.0f),
             rgba(0xE8E8F0, 1.0f), rgba(0xFFFFFF, 0.09f),
             rgba(0xFFFFFF, 0.11f), rgba(0xFFFFFF, 0.07f),
             rgba(0x39FF14, 1.0f) };
}

// Una fila del menú. El layout se calcula una vez por apertura y el
// hit-test es una búsqueda lineal sobre estos rects: con seis filas es
// más rápido que cualquier estructura "lista".
struct Row {
    enum class Kind { State, Action, Device, Divider } kind = Kind::Action;
    std::wstring label;
    const wchar_t* glyph = nullptr;
    NativeFlyout::Action action = NativeFlyout::Action::ToggleMute;
    int payload = 0;
    bool checked = false;
    bool enabled = true;
    D2D1_RECT_F rect{};
};

struct State {
    HWND hwnd = nullptr;
    bool open = false;
    UINT dpi = 96;
    float scale = 1.0f;
    int hover = -1;

    NativeFlyout::Model model;
    std::vector<Row> rows;
    std::function<void(NativeFlyout::Action, int)> onAction;

    // Gráficos
    winrt::com_ptr<ID3D11Device> d3d;
    winrt::com_ptr<IDXGISwapChain1> swap;
    winrt::com_ptr<IDCompositionDevice> dcomp;
    winrt::com_ptr<IDCompositionTarget> dcTarget;
    winrt::com_ptr<IDCompositionVisual> dcVisual;
    winrt::com_ptr<ID2D1Factory1> d2dFactory;
    winrt::com_ptr<ID2D1Device> d2dDevice;
    winrt::com_ptr<ID2D1DeviceContext> d2d;
    winrt::com_ptr<IDWriteFactory> dwrite;
    winrt::com_ptr<IDWriteTextFormat> fmtText;
    winrt::com_ptr<IDWriteTextFormat> fmtSub;
    winrt::com_ptr<IDWriteTextFormat> fmtIcon;
    bool gfxReady = false;

    // Hook para cerrar al hacer click fuera. La barra de tareas no roba
    // la activación, así que WM_ACTIVATE no siempre llega: el hook de
    // mouse es la vía fiable (misma lección que el flyout anterior).
    HHOOK mouseHook = nullptr;
};

State g;

// Soltado de la pila gráfica en reposo: se usa desde el WndProc (más abajo)
// y se define después, junto al resto de la gestión de gráficos.
constexpr UINT_PTR kGfxCooldownTimer = 1;
constexpr UINT kGfxCooldownMs = 3000;
void ReleaseGraphics(const char* reason);

float Sc(float dip) { return dip * g.scale; }

// ── Construcción del modelo de filas ──
void BuildRows() {
    g.rows.clear();

    Row state;
    state.kind = Row::Kind::State;
    state.label = g.model.muted ? L"Microphone muted" : L"Microphone live";
    g.rows.push_back(state);

    Row divider;
    divider.kind = Row::Kind::Divider;
    g.rows.push_back(divider);

    Row mute;
    mute.label = g.model.muted ? L"Unmute" : L"Mute";
    mute.glyph = g.model.muted ? kGlyphMicOff : kGlyphMic;
    mute.action = NativeFlyout::Action::ToggleMute;
    g.rows.push_back(mute);

    // Dispositivos: máximo 4 para que el menú no crezca sin control.
    const size_t maxDevices = 4;
    if (!g.model.devices.empty()) {
        g.rows.push_back(divider);
        for (size_t i = 0; i < g.model.devices.size() && i < maxDevices; ++i) {
            Row d;
            d.kind = Row::Kind::Device;
            d.label = g.model.devices[i].name;
            d.glyph = kGlyphDevice;
            d.action = NativeFlyout::Action::SelectDevice;
            d.payload = static_cast<int>(i);
            d.checked = g.model.devices[i].current;
            g.rows.push_back(d);
        }
    }

    g.rows.push_back(divider);

    Row open;
    open.label = L"Open MuteMic";
    open.glyph = kGlyphOpen;
    open.action = NativeFlyout::Action::OpenSettings;
    g.rows.push_back(open);

    Row quit;
    quit.label = L"Quit";
    quit.glyph = kGlyphPower;
    quit.action = NativeFlyout::Action::Quit;
    g.rows.push_back(quit);
}

// Layout: alto total y rect de cada fila. Devuelve el alto en DIP.
float LayoutRows() {
    float y = kPadY;
    for (auto& r : g.rows) {
        const float h = (r.kind == Row::Kind::Divider) ? kDividerH
                      : (r.kind == Row::Kind::State)   ? kStateH
                                                       : kRowH;
        r.rect = D2D1::RectF(kPadX, y, kWidth - kPadX, y + h);
        y += h;
    }
    return y + kPadY;
}

// ── Gráficos ──
bool EnsureGraphics(int pxW, int pxH) {
    if (!g.gfxReady) {
        Telemetry::Memory("PERF", "subsystem", "flyout.gfx.before");
        HRESULT hr = D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
            g.d3d.put(), nullptr, nullptr);
        if (FAILED(hr)) { Telemetry::Fail("FLYOUT", "d3d.create", hr); return false; }

        auto dxgiDev = g.d3d.try_as<IDXGIDevice>();
        if (!dxgiDev) return false;

        D2D1_FACTORY_OPTIONS fo{};
        hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                               __uuidof(ID2D1Factory1), &fo,
                               g.d2dFactory.put_void());
        if (FAILED(hr)) { Telemetry::Fail("FLYOUT", "d2d.factory", hr); return false; }
        if (FAILED(g.d2dFactory->CreateDevice(dxgiDev.get(), g.d2dDevice.put())))
            return false;
        if (FAILED(g.d2dDevice->CreateDeviceContext(
                D2D1_DEVICE_CONTEXT_OPTIONS_NONE, g.d2d.put())))
            return false;

        if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
                                       __uuidof(IDWriteFactory),
                                       reinterpret_cast<IUnknown**>(g.dwrite.put()))))
            return false;

        if (FAILED(DCompositionCreateDevice(dxgiDev.get(),
                                            __uuidof(IDCompositionDevice),
                                            g.dcomp.put_void())))
            return false;
        g.gfxReady = true;
        Telemetry::Memory("PERF", "subsystem", "flyout.gfx.after");
    }

    // Formatos de texto: se recrean si cambia el DPI (el tamaño va en px).
    g.fmtText = nullptr; g.fmtSub = nullptr; g.fmtIcon = nullptr;
    g.dwrite->CreateTextFormat(L"Segoe UI Variable Text", nullptr,
                               DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                               DWRITE_FONT_STRETCH_NORMAL, Sc(14.0f), L"",
                               g.fmtText.put());
    if (!g.fmtText)
        g.dwrite->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                   DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                   Sc(14.0f), L"", g.fmtText.put());
    g.dwrite->CreateTextFormat(L"Segoe UI Variable Small", nullptr,
                               DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
                               DWRITE_FONT_STRETCH_NORMAL, Sc(12.0f), L"",
                               g.fmtSub.put());
    if (!g.fmtSub)
        g.dwrite->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                                   DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                   Sc(12.0f), L"", g.fmtSub.put());
    g.dwrite->CreateTextFormat(L"Segoe Fluent Icons", nullptr,
                               DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                               DWRITE_FONT_STRETCH_NORMAL, Sc(15.0f), L"",
                               g.fmtIcon.put());
    if (!g.fmtIcon)
        g.dwrite->CreateTextFormat(L"Segoe MDL2 Assets", nullptr,
                                   DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                   DWRITE_FONT_STRETCH_NORMAL, Sc(15.0f), L"",
                                   g.fmtIcon.put());
    for (auto* f : { g.fmtText.get(), g.fmtSub.get(), g.fmtIcon.get() })
        if (f) f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

    // Swapchain del tamaño exacto de la ventana.
    g.swap = nullptr;
    winrt::com_ptr<IDXGIDevice> dxgiDev = g.d3d.as<IDXGIDevice>();
    winrt::com_ptr<IDXGIAdapter> adapter;
    if (FAILED(dxgiDev->GetAdapter(adapter.put()))) return false;
    winrt::com_ptr<IDXGIFactory2> factory;
    if (FAILED(adapter->GetParent(__uuidof(IDXGIFactory2), factory.put_void())))
        return false;

    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = pxW;
    sd.Height = pxH;
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc = { 1, 0 };
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.Scaling = DXGI_SCALING_STRETCH;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    sd.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
    if (FAILED(factory->CreateSwapChainForComposition(g.d3d.get(), &sd, nullptr,
                                                      g.swap.put())))
        return false;

    g.dcTarget = nullptr;
    g.dcVisual = nullptr;
    if (FAILED(g.dcomp->CreateTargetForHwnd(g.hwnd, TRUE, g.dcTarget.put())))
        return false;
    if (FAILED(g.dcomp->CreateVisual(g.dcVisual.put()))) return false;
    g.dcVisual->SetContent(g.swap.get());
    g.dcTarget->SetRoot(g.dcVisual.get());
    g.dcomp->Commit();
    return true;
}

void DrawText_(const wchar_t* text, IDWriteTextFormat* fmt, D2D1_RECT_F const& rc,
               ID2D1Brush* brush, DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING) {
    if (!text || !fmt) return;
    fmt->SetTextAlignment(align);
    g.d2d->DrawText(text, static_cast<UINT32>(wcslen(text)), fmt, rc, brush,
                    D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
}

void Render() {
    if (!g.swap || !g.d2d) return;

    winrt::com_ptr<IDXGISurface> surface;
    if (FAILED(g.swap->GetBuffer(0, __uuidof(IDXGISurface), surface.put_void())))
        return;
    D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    winrt::com_ptr<ID2D1Bitmap1> target;
    if (FAILED(g.d2d->CreateBitmapFromDxgiSurface(surface.get(), &bp, target.put())))
        return;

    const Palette P = PaletteFor(g.model.theme, g.model.glass);
    const float w = Sc(kWidth);
    const float h = Sc(LayoutRows());

    g.d2d->SetTarget(target.get());
    g.d2d->BeginDraw();
    g.d2d->Clear(D2D1::ColorF(0, 0.0f));

    winrt::com_ptr<ID2D1SolidColorBrush> br;
    g.d2d->CreateSolidColorBrush(P.root, br.put());

    // Fondo redondeado + borde hairline: el look nativo de Win11.
    D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(
        D2D1::RectF(0.5f, 0.5f, w - 0.5f, h - 0.5f), Sc(kRadius), Sc(kRadius));
    g.d2d->FillRoundedRectangle(rr, br.get());
    br->SetColor(P.border);
    g.d2d->DrawRoundedRectangle(rr, br.get(), 1.0f);

    for (size_t i = 0; i < g.rows.size(); ++i) {
        const Row& r = g.rows[i];
        const D2D1_RECT_F rc = D2D1::RectF(Sc(r.rect.left), Sc(r.rect.top),
                                           Sc(r.rect.right), Sc(r.rect.bottom));

        if (r.kind == Row::Kind::Divider) {
            const float y = (rc.top + rc.bottom) * 0.5f;
            br->SetColor(P.divider);
            g.d2d->DrawLine(D2D1::Point2F(rc.left, y), D2D1::Point2F(rc.right, y),
                            br.get(), 1.0f);
            continue;
        }

        // Resalte de hover, con el mismo radio que el root.
        if (static_cast<int>(i) == g.hover && r.kind != Row::Kind::State) {
            br->SetColor(P.hover);
            g.d2d->FillRoundedRectangle(
                D2D1::RoundedRect(rc, Sc(5.0f), Sc(5.0f)), br.get());
        }

        if (r.kind == Row::Kind::State) {
            // LED de estado + texto: verde neón vivo, rojo muteado.
            const D2D1_COLOR_F led = g.model.muted
                ? D2D1::ColorF(0xFF3131, 1.0f)
                : D2D1::ColorF(0x39FF14, 1.0f);
            const float cy = (rc.top + rc.bottom) * 0.5f;
            const float rad = Sc(4.0f);
            br->SetColor(led);
            g.d2d->FillEllipse(
                D2D1::Ellipse(D2D1::Point2F(rc.left + Sc(9.0f), cy), rad, rad),
                br.get());
            // Halo tenue: el mismo lenguaje visual que el icono del tray.
            D2D1_COLOR_F halo = led; halo.a = 0.25f;
            br->SetColor(halo);
            g.d2d->FillEllipse(
                D2D1::Ellipse(D2D1::Point2F(rc.left + Sc(9.0f), cy), rad * 2.2f,
                              rad * 2.2f), br.get());

            br->SetColor(P.text);
            DrawText_(r.label.c_str(), g.fmtSub.get(),
                      D2D1::RectF(rc.left + Sc(24.0f), rc.top, rc.right, rc.bottom),
                      br.get());
            continue;
        }

        // Icono + etiqueta.
        br->SetColor(r.enabled ? P.icon : P.subtext);
        if (r.glyph)
            DrawText_(r.glyph, g.fmtIcon.get(),
                      D2D1::RectF(rc.left + Sc(6.0f), rc.top,
                                  rc.left + Sc(kIconW), rc.bottom),
                      br.get(), DWRITE_TEXT_ALIGNMENT_CENTER);

        br->SetColor(r.enabled ? P.text : P.subtext);
        DrawText_(r.label.c_str(), g.fmtText.get(),
                  D2D1::RectF(rc.left + Sc(kIconW + 4.0f), rc.top,
                              rc.right - Sc(24.0f), rc.bottom),
                  br.get());

        // Palomita del dispositivo activo.
        if (r.checked) {
            br->SetColor(P.accent);
            DrawText_(kGlyphCheck, g.fmtIcon.get(),
                      D2D1::RectF(rc.right - Sc(22.0f), rc.top, rc.right, rc.bottom),
                      br.get(), DWRITE_TEXT_ALIGNMENT_CENTER);
        }
    }

    g.d2d->EndDraw();
    g.d2d->SetTarget(nullptr);
    g.swap->Present(0, 0);
}

int HitTest(int px, int py) {
    const float x = px / g.scale, y = py / g.scale;
    for (size_t i = 0; i < g.rows.size(); ++i) {
        const Row& r = g.rows[i];
        if (r.kind == Row::Kind::Divider || r.kind == Row::Kind::State) continue;
        if (x >= r.rect.left && x <= r.rect.right &&
            y >= r.rect.top && y <= r.rect.bottom)
            return static_cast<int>(i);
    }
    return -1;
}

// El taskbar no roba la activación al hacer click, así que WM_ACTIVATE no
// siempre llega. El hook de mouse es la vía fiable para cerrar al pinchar
// fuera (lección heredada del flyout anterior, ver vault/LESSONS.md).
//
// ⚠ REGLA CRÍTICA: un hook de bajo nivel corre DENTRO de la cola de
// entrada del sistema. Todo lo que tarde aquí congela el ratón y el
// teclado de TODA la máquina, no solo de esta app. Prohibido: operaciones
// de ventana (ShowWindow), E/S de archivo (el log), y desengancharse a sí
// mismo. Aquí SOLO se hace PostMessage; el trabajo real ocurre después,
// en el bucle de mensajes de nuestra ventana.
LRESULT CALLBACK MouseProc(int code, WPARAM wParam, LPARAM lParam) {
    if (code == HC_ACTION && g.open &&
        (wParam == WM_LBUTTONDOWN || wParam == WM_RBUTTONDOWN ||
         wParam == WM_NCLBUTTONDOWN || wParam == WM_NCRBUTTONDOWN)) {
        auto* mi = reinterpret_cast<MSLLHOOKSTRUCT*>(lParam);
        RECT rc{};
        GetWindowRect(g.hwnd, &rc);
        if (!PtInRect(&rc, mi->pt))
            PostMessageW(g.hwnd, kMsgDismiss, 0, 0);
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_MOUSEMOVE: {
            const int h = HitTest(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            if (h != g.hover) { g.hover = h; Render(); }
            // Para recibir WM_MOUSELEAVE y limpiar el hover al salir.
            TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hwnd, 0 };
            TrackMouseEvent(&tme);
            return 0;
        }
        case WM_MOUSELEAVE:
            if (g.hover != -1) { g.hover = -1; Render(); }
            return 0;

        case WM_LBUTTONUP: {
            const int h = HitTest(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            if (h >= 0 && g.rows[h].enabled) {
                const auto action = g.rows[h].action;
                const int payload = g.rows[h].payload;
                auto cb = g.onAction;
                NativeFlyout::Hide();
                if (cb) cb(action, payload);
            }
            return 0;
        }

        case WM_KEYDOWN:
            if (wParam == VK_ESCAPE) { NativeFlyout::Hide(); return 0; }
            break;

        case WM_ACTIVATE:
            if (LOWORD(wParam) == WA_INACTIVE) NativeFlyout::Hide();
            return 0;

        case WM_TIMER:
            if (wParam == kGfxCooldownTimer) {
                KillTimer(hwnd, kGfxCooldownTimer);
                // Reabrieron el menú mientras corría el enfriamiento: no
                // soltar nada, se está usando.
                if (!g.open) ReleaseGraphics("cooldown");
                return 0;
            }
            break;

        default:
            // Cierre diferido pedido por el hook de mouse: aquí ya estamos
            // fuera de la cola de entrada del sistema y es seguro tocar
            // ventanas y desenganchar el hook.
            if (msg == kMsgDismiss) { NativeFlyout::Hide(); return 0; }
            break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ── Soltado de la pila gráfica en reposo ──
//
// MEDIDO: montar los gráficos del flyout cuesta 45,9 MB de private bytes
// (6,1 -> 52,0 en la sonda `flyout.gfx`). Abrir el menú del tray UNA vez
// dejaba esos 46 MB residentes para siempre en un proceso que vive todo el
// día, porque solo `Term()` liberaba, y `Term()` corre al cerrar la app.
//
// El patrón es el "modo caliente" que el Liquid Glass ya usa: mantener vivo
// un rato por si vuelve a hacer falta —abrir el menú dos veces seguidas es
// normal— y soltar cuando queda claro que no. La VENTANA se conserva: cuesta
// nada y evita recrear la clase y el subclassing.
void ReleaseGraphics(const char* reason) {
    if (!g.gfxReady) return;

    // Mismo orden que exige la doc de IDXGIDevice3::Trim: limpiar el pipeline
    // y vaciar ANTES de soltar, para que D3D no retenga referencias a lo que
    // estamos destruyendo. Sin esto la destrucción es diferida y la memoria
    // no vuelve en el momento.
    if (g.d3d) {
        winrt::com_ptr<ID3D11DeviceContext> ctx;
        g.d3d->GetImmediateContext(ctx.put());
        if (ctx) { ctx->ClearState(); ctx->Flush(); }
        if (auto dxgi3 = g.d3d.try_as<IDXGIDevice3>()) dxgi3->Trim();
    }

    g.fmtIcon = nullptr;
    g.fmtSub = nullptr;
    g.fmtText = nullptr;
    g.dwrite = nullptr;
    g.d2d = nullptr;
    g.d2dDevice = nullptr;
    g.d2dFactory = nullptr;
    g.dcVisual = nullptr;
    g.dcTarget = nullptr;
    g.dcomp = nullptr;
    g.swap = nullptr;
    g.d3d = nullptr;
    g.gfxReady = false;

    Telemetry::Memory("PERF", "subsystem", "flyout.gfx.released");
    Telemetry::Event("FLYOUT", "gfx.release", reason);
}

bool EnsureWindow() {
    if (g.hwnd) return true;

    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = WndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = kClass;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        RegisterClassW(&wc);
        registered = true;
    }

    g.hwnd = CreateWindowExW(
        WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOOLWINDOW | WS_EX_TOPMOST |
            WS_EX_NOACTIVATE,
        kClass, L"", WS_POPUP, 0, 0, 10, 10,
        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!g.hwnd) return false;

    // Esquinas redondeadas del sistema y sin borde pintado por DWM: el
    // borde lo dibujamos nosotros para controlar su color por tema.
    DWORD corner = 2 /*DWMWCP_ROUND*/;
    DwmSetWindowAttribute(g.hwnd, 33 /*WINDOW_CORNER_PREFERENCE*/, &corner,
                          sizeof(corner));
    COLORREF none = 0xFFFFFFFE /*DWMWA_COLOR_NONE*/;
    DwmSetWindowAttribute(g.hwnd, 34 /*BORDER_COLOR*/, &none, sizeof(none));
    return true;
}

}  // namespace

void NativeFlyout::Show(int anchorX, int anchorY, Model const& model,
                        std::function<void(Action, int)> onAction) {
    g.model = model;
    g.onAction = std::move(onAction);
    g.hover = -1;

    if (!EnsureWindow()) return;

    // DPI del monitor donde se abre (portátil + monitor externo = DPIs
    // distintos; usar el del sistema saldría borroso en uno de los dos).
    HMONITOR mon = MonitorFromPoint({ anchorX, anchorY }, MONITOR_DEFAULTTONEAREST);
    UINT dpiX = 96, dpiY = 96;
    GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dpiX, &dpiY);
    g.dpi = dpiX;
    g.scale = dpiX / 96.0f;

    BuildRows();
    const float hDip = LayoutRows();
    const int w = static_cast<int>(Sc(kWidth) + 0.5f);
    const int h = static_cast<int>(Sc(hDip) + 0.5f);

    // Anclado sobre el área de trabajo (nunca tapando el taskbar).
    MONITORINFO mi{ sizeof(mi) };
    GetMonitorInfoW(mon, &mi);
    const int gap = static_cast<int>(Sc(12.0f));
    int x = anchorX - w / 2;
    x = std::clamp<int>(x, mi.rcWork.left + gap, mi.rcWork.right - w - gap);
    int y = mi.rcWork.bottom - h - gap;
    if (y < mi.rcWork.top) y = mi.rcWork.top + gap;

    SetWindowPos(g.hwnd, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE);

    if (!EnsureGraphics(w, h)) {
        Telemetry::Event("FLYOUT", "graphics.unavailable");
        return;
    }

    ShowWindow(g.hwnd, SW_SHOWNOACTIVATE);
    g.open = true;
    Render();

    if (!g.mouseHook)
        g.mouseHook = SetWindowsHookExW(WH_MOUSE_LL, MouseProc,
                                        GetModuleHandleW(nullptr), 0);
    if (g.hwnd) KillTimer(g.hwnd, kGfxCooldownTimer);   // se está usando
    Probes::Hit(Probe::FlyoutShow);
    Telemetry::Event("FLYOUT", "show");
}

void NativeFlyout::Update(Model const& model) {
    if (!g.open) return;
    g.model = model;
    BuildRows();
    Render();
}

void NativeFlyout::Hide() {
    if (!g.open) return;
    g.open = false;
    if (g.mouseHook) { UnhookWindowsHookEx(g.mouseHook); g.mouseHook = nullptr; }
    if (g.hwnd) {
        ShowWindow(g.hwnd, SW_HIDE);
        // Enfriamiento: si el menú no se reabre en 3 s, se sueltan los 46 MB.
        SetTimer(g.hwnd, kGfxCooldownTimer, kGfxCooldownMs, nullptr);
    }
    Telemetry::Event("FLYOUT", "hide");
}

bool NativeFlyout::IsOpen() { return g.open; }

void NativeFlyout::Term() {
    Hide();
    // Una sola lista de recursos, en ReleaseGraphics. Antes esta función
    // tenía su propia copia: dos listas del mismo conjunto, garantizadas para
    // divergir en cuanto alguien añada un recurso y actualice solo una. Es el
    // mismo error de fondo que el mensaje con dos significados y los ajustes
    // con dos copias en memoria.
    ReleaseGraphics("term");
    if (g.hwnd) { DestroyWindow(g.hwnd); g.hwnd = nullptr; }
}

}  // namespace mutemic
