#include "pch.h"
#include "IconRenderer.h"

#include <algorithm>
#include <objidl.h>
// gdiplus.h usa min/max sin calificar; con NOMINMAX hay que inyectarlos.
namespace Gdiplus { using std::min; using std::max; }
#include <gdiplus.h>

namespace mutemic {
namespace {

ULONG_PTR g_gdiplusToken = 0;

namespace G = Gdiplus;

// El MISMO glifo de micrófono que usa Windows 11 en el tray ("app is using
// your microphone"): Segoe Fluent Icons U+E720. Nítido a cualquier DPI
// porque es un glifo de fuente, no un bitmap.
constexpr wchar_t kMicGlyph[] = L"\uE720";

G::Color TubeColor(TrayFace face) {
    switch (face) {
        // Neón de cartel: verde #39FF14 / rojo #FF3131 (match Colors.xaml).
        case TrayFace::GreenIdle:
        case TrayFace::GreenActive: return G::Color(255, 0x39, 0xFF, 0x14);
        case TrayFace::RedIdle:
        case TrayFace::RedActive:   return G::Color(255, 0xFF, 0x31, 0x31);
        case TrayFace::NoDevice:
        default:                    return G::Color(255, 0x8A, 0x8A, 0x8A);
    }
}

bool IsActive(TrayFace face) {
    return face == TrayFace::GreenActive || face == TrayFace::RedActive;
}

G::Color WithAlpha(const G::Color& c, float alpha01) {
    BYTE a = static_cast<BYTE>(std::clamp(alpha01, 0.0f, 1.0f) * 255.0f);
    return G::Color(a, c.GetR(), c.GetG(), c.GetB());
}

G::Color Lighter(const G::Color& c, int amount) {
    return G::Color(255,
                    static_cast<BYTE>(std::min(255, c.GetR() + amount)),
                    static_cast<BYTE>(std::min(255, c.GetG() + amount)),
                    static_cast<BYTE>(std::min(255, c.GetB() + amount)));
}

// Fuente de glifos: Fluent (Win11) con fallback a MDL2 (Win10).
std::unique_ptr<G::FontFamily> GlyphFontFamily() {
    auto fluent = std::make_unique<G::FontFamily>(L"Segoe Fluent Icons");
    if (fluent->IsAvailable()) return fluent;
    return std::make_unique<G::FontFamily>(L"Segoe MDL2 Assets");
}

}  // namespace

void IconRenderer::Startup() {
    if (g_gdiplusToken) return;
    G::GdiplusStartupInput input;
    G::GdiplusStartup(&g_gdiplusToken, &input, nullptr);
}

void IconRenderer::Shutdown() {
    if (g_gdiplusToken) {
        G::GdiplusShutdown(g_gdiplusToken);
        g_gdiplusToken = 0;
    }
}

int IconRenderer::TrayIconSizeForDpi(UINT dpi) {
    int size = MulDiv(16, static_cast<int>(dpi), 96);
    if (size < 16) size = 16;
    return (size + 1) & ~1;
}

HICON IconRenderer::Render(TrayFace face, float level, int sizePx) {
    Startup();

    level = std::clamp(level, 0.0f, 1.0f);
    const G::Color color = TubeColor(face);
    const bool active = IsActive(face);

    // Look de tubo de neón (cartel): el PERÍMETRO del mic es el tubo.
    // Verde: idle apagado ↔ hablar a plena potencia (contraste = señal).
    // Rojo (mute): SIEMPRE vivo tipo neón — a 16 px un rojo tenue se lee
    // gris y se pierde; que grite "muteado".
    float intensity, glow;
    if (face == TrayFace::NoDevice) {
        intensity = 0.55f;
        glow = 0.0f;
    } else if (active) {
        intensity = 1.0f;
        glow = 0.55f + 0.45f * level;      // halo que respira con la voz
    } else if (face == TrayFace::RedIdle) {
        intensity = 0.95f;                 // rojo neón encendido
        glow = 0.35f;
    } else {
        intensity = 0.45f;                 // verde en reposo: tenue a propósito
        glow = 0.0f;
    }

    // Supersampling 4×: se dibuja grande y se reduce con bicúbico —
    // máxima nitidez al tamaño real del tray (que Windows fija en 16 pt).
    const int ss = 4;
    const int big = sizePx * ss;
    const float sBig = big / 32.0f;

    G::Bitmap bmpBig(big, big, PixelFormat32bppARGB);
    {
        G::Graphics g(&bmpBig);
        g.SetSmoothingMode(G::SmoothingModeAntiAlias);
        g.SetPixelOffsetMode(G::PixelOffsetModeHighQuality);
        g.Clear(G::Color(0, 0, 0, 0));

        // Glifo como path → trazamos su CONTORNO como tubo de neón.
        auto family = GlyphFontFamily();
        G::GraphicsPath glyph;
        G::StringFormat fmt;
        fmt.SetAlignment(G::StringAlignmentCenter);
        fmt.SetLineAlignment(G::StringAlignmentCenter);
        G::RectF box(0.0f, 0.0f, static_cast<float>(big), static_cast<float>(big));
        glyph.AddString(kMicGlyph, -1, family.get(), G::FontStyleRegular,
                        26.0f * sBig, box, &fmt);

        // Capas de afuera hacia adentro (receta de cartel de neón):
        // halo ancho difuso → halo medio → tubo saturado → núcleo casi blanco.
        if (glow > 0.02f) {
            G::Pen haloWide(WithAlpha(color, 0.16f * glow), (7.0f + 3.0f * glow) * sBig);
            haloWide.SetLineJoin(G::LineJoinRound);
            g.DrawPath(&haloWide, &glyph);
            G::Pen haloTight(WithAlpha(color, 0.34f * glow), (3.6f + 1.5f * glow) * sBig);
            haloTight.SetLineJoin(G::LineJoinRound);
            g.DrawPath(&haloTight, &glyph);
        }
        // Cuerpo del glifo muy tenue para dar volumen (vidrio del tubo).
        G::SolidBrush body(WithAlpha(color, 0.18f * intensity));
        g.FillPath(&body, &glyph);
        // Tubo (contorno saturado) + núcleo caliente (solo brilla activo).
        G::Pen tube(WithAlpha(color, intensity), 1.7f * sBig);
        tube.SetLineJoin(G::LineJoinRound);
        g.DrawPath(&tube, &glyph);
        if (glow > 0.02f) {
            G::Pen corePen(WithAlpha(Lighter(color, 130), 0.95f * intensity), 0.8f * sBig);
            corePen.SetLineJoin(G::LineJoinRound);
            g.DrawPath(&corePen, &glyph);
        }

        // Barra de mute (diagonal, mismo tratamiento neón).
        if (face == TrayFace::RedIdle || face == TrayFace::RedActive) {
            G::GraphicsPath slash;
            slash.AddLine(7.5f * sBig, 25.5f * sBig, 24.5f * sBig, 6.5f * sBig);
            if (glow > 0.02f) {
                G::Pen slashHalo(WithAlpha(color, 0.34f * glow), (3.6f + 1.5f * glow) * sBig);
                slashHalo.SetStartCap(G::LineCapRound);
                slashHalo.SetEndCap(G::LineCapRound);
                g.DrawPath(&slashHalo, &slash);
            }
            G::Pen slashPen(WithAlpha(color, intensity), 2.2f * sBig);
            slashPen.SetStartCap(G::LineCapRound);
            slashPen.SetEndCap(G::LineCapRound);
            g.DrawPath(&slashPen, &slash);
        }
    }

    // Downscale 4× → tamaño real con bicúbico de alta calidad.
    G::Bitmap bmp(sizePx, sizePx, PixelFormat32bppARGB);
    {
        G::Graphics g(&bmp);
        g.SetInterpolationMode(G::InterpolationModeHighQualityBicubic);
        g.SetPixelOffsetMode(G::PixelOffsetModeHighQuality);
        g.Clear(G::Color(0, 0, 0, 0));
        g.DrawImage(&bmpBig, G::RectF(0.0f, 0.0f,
                    static_cast<float>(sizePx), static_cast<float>(sizePx)));
    }

    HICON icon = nullptr;
    bmp.GetHICON(&icon);
    return icon;
}

}  // namespace mutemic
