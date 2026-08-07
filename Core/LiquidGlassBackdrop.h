#pragma once
#include <windows.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>

namespace mutemic {

// Backdrop "Liquid Glass" real: captura el monitor (excluyendo NUESTRAS
// ventanas vía WDA_EXCLUDEFROMCAPTURE), y cada ventana registrada pinta la
// región del snapshot que tiene detrás a través del shader de refracción
// (portado del repo LiquidGlass a HLSL/D2D) en un CanvasControl de Win2D.
//
// (Se usa CanvasControl y no SystemBackdrop porque la composición de Win2D
// vive en Windows.UI.Composition y el backdrop de WinUI 3 en
// Microsoft.UI.Composition — árboles incompatibles. CanvasControl es la vía
// soportada de Win2D dentro de XAML/WinUI 3.)
//
// ── Arquitectura: UNA captura compartida + N "lentes" ──
// El snapshot se toma con todas nuestras ventanas excluidas, así que es
// válido sin importar dónde estén: mover una ventana solo cambia QUÉ zona
// de la misma textura se muestrea (gratis, sin recapturar — recapturar
// durante el arrastre era la causa del jitter).
// El snapshot se refresca por EVENTOS reales del escritorio (cambio de
// ventana en primer plano, ventanas que se mueven detrás), con debounce.
//
//   frost = true  → "Full effect": blur gaussiano + noise + glow
//   frost = false → "Without blur or noise": refracción limpia
class LiquidGlassBackdrop {
public:
    // Lente primaria (ventana principal). host = panel raíz; el canvas se
    // inserta como primer hijo (detrás de todo). Devuelve false si el
    // sistema no soporta captura o falta el shader.
    static bool Start(HWND hwnd,
                      winrt::Microsoft::UI::Xaml::Controls::Grid const& host,
                      bool frost, bool shareFriendly);

    // Lente adicional (flyout del tray, futuros overlays). Comparte el
    // snapshot y los parámetros de la lente primaria. No-op si el glass no
    // está activo. Idempotente por hwnd.
    static bool AttachLens(HWND hwnd,
                           winrt::Microsoft::UI::Xaml::Controls::Grid const& host);

    // Quita una lente adicional (al cerrar/destruir su ventana).
    static void DetachLens(HWND hwnd);

    // Quita TODAS las lentes, detiene la captura y restaura display affinity.
    static void Stop();

    // Redibujar todas las lentes con el snapshot actual (mover/redimensionar).
    // NO recaptura: es barato y es lo que se llama durante un arrastre.
    static void RequestRedraw();

    // Forzar un snapshot nuevo (cambió el fondo). Con debounce interno.
    static void RefreshCapture();

    // Cambiar frost sin reiniciar la captura.
    static void SetFrost(bool frost);

    // Ventana oculta en el tray → suelta captura, frames y grafo de efectos
    // (decenas de MB de GPU/RAM que nadie ve). Al volver a mostrarse, se
    // rearma solo. El modo glass sigue "activo" lógicamente.
    static void OnWindowVisibility(bool visible);

    static bool IsActive();
};

}  // namespace mutemic
