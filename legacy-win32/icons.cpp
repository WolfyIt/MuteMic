#include "icons.h"

namespace {

constexpr int kIconSize = 32;

COLORREF StateColor(MicState state) {
    switch (state) {
        case MicState::Unmuted:  return RGB(46, 204, 64);    // verde
        case MicState::Muted:    return RGB(220, 53, 45);    // rojo
        case MicState::NoDevice: return RGB(128, 128, 128);  // gris
    }
    return RGB(128, 128, 128);
}

// Dibuja una silueta simple de micrófono en blanco sobre el círculo.
void DrawMicGlyph(HDC dc) {
    HBRUSH white = static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH));
    HPEN whitePen = CreatePen(PS_SOLID, 2, RGB(255, 255, 255));
    HGDIOBJ oldBrush = SelectObject(dc, white);
    HGDIOBJ oldPen = SelectObject(dc, whitePen);

    // Cápsula del mic
    RoundRect(dc, 13, 7, 20, 18, 6, 6);
    // Arco del soporte
    Arc(dc, 10, 12, 23, 22, 10, 20, 23, 20);
    // Pie
    MoveToEx(dc, 16, 21, nullptr);
    LineTo(dc, 16, 25);
    MoveToEx(dc, 12, 25, nullptr);
    LineTo(dc, 21, 25);

    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(whitePen);
}

void DrawSlash(HDC dc) {
    HPEN pen = CreatePen(PS_SOLID, 3, RGB(255, 255, 255));
    HGDIOBJ old = SelectObject(dc, pen);
    MoveToEx(dc, 8, 24, nullptr);
    LineTo(dc, 24, 8);
    SelectObject(dc, old);
    DeleteObject(pen);
}

}  // namespace

HICON CreateStateIcon(MicState state) {
    HDC screen = GetDC(nullptr);
    HDC colorDC = CreateCompatibleDC(screen);
    HBITMAP colorBmp = CreateCompatibleBitmap(screen, kIconSize, kIconSize);
    // Máscara monocroma: 0 = opaco, 1 = transparente.
    HBITMAP maskBmp = CreateBitmap(kIconSize, kIconSize, 1, 1, nullptr);
    ReleaseDC(nullptr, screen);

    HGDIOBJ oldColor = SelectObject(colorDC, colorBmp);

    // Fondo negro (queda transparente vía máscara).
    RECT full = {0, 0, kIconSize, kIconSize};
    FillRect(colorDC, &full, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));

    // Círculo de fondo con el color del estado.
    HBRUSH circleBrush = CreateSolidBrush(StateColor(state));
    HPEN circlePen = CreatePen(PS_SOLID, 1, StateColor(state));
    HGDIOBJ oldBrush = SelectObject(colorDC, circleBrush);
    HGDIOBJ oldPen = SelectObject(colorDC, circlePen);
    Ellipse(colorDC, 1, 1, kIconSize - 1, kIconSize - 1);
    SelectObject(colorDC, oldBrush);
    SelectObject(colorDC, oldPen);
    DeleteObject(circleBrush);
    DeleteObject(circlePen);

    DrawMicGlyph(colorDC);
    if (state == MicState::Muted) DrawSlash(colorDC);

    // Máscara: transparente fuera del círculo, opaco dentro.
    HDC maskDC = CreateCompatibleDC(nullptr);
    HGDIOBJ oldMask = SelectObject(maskDC, maskBmp);
    FillRect(maskDC, &full, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));  // todo transparente
    HBRUSH black = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    HPEN blackPen = static_cast<HPEN>(GetStockObject(BLACK_PEN));
    HGDIOBJ oldMaskBrush = SelectObject(maskDC, black);
    HGDIOBJ oldMaskPen = SelectObject(maskDC, blackPen);
    Ellipse(maskDC, 1, 1, kIconSize - 1, kIconSize - 1);  // círculo opaco
    SelectObject(maskDC, oldMaskBrush);
    SelectObject(maskDC, oldMaskPen);
    SelectObject(maskDC, oldMask);
    DeleteDC(maskDC);

    SelectObject(colorDC, oldColor);
    DeleteDC(colorDC);

    ICONINFO info = {};
    info.fIcon = TRUE;
    info.hbmMask = maskBmp;
    info.hbmColor = colorBmp;
    HICON icon = CreateIconIndirect(&info);

    DeleteObject(colorBmp);
    DeleteObject(maskBmp);
    return icon;
}
