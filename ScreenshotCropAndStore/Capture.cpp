#include "Capture.h"

CapturedImage CaptureVirtualScreen(RECT& bounds)
{
    int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    bounds = { vx, vy, vx + vw, vy + vh };

    CapturedImage result;
    if (vw <= 0 || vh <= 0) return result;

    HDC screenDC = GetDC(nullptr);
    HDC memDC = CreateCompatibleDC(screenDC);

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = vw;
    bmi.bmiHeader.biHeight = -vh; // négatif = bitmap top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP hBitmap = CreateDIBSection(memDC, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!hBitmap)
    {
        DeleteDC(memDC);
        ReleaseDC(nullptr, screenDC);
        return result;
    }

    HGDIOBJ oldBmp = SelectObject(memDC, hBitmap);
    BitBlt(memDC, 0, 0, vw, vh, screenDC, vx, vy, SRCCOPY);

    // Incrustation du curseur à sa position exacte (relative au bureau virtuel).
    CURSORINFO ci{};
    ci.cbSize = sizeof(CURSORINFO);
    if (GetCursorInfo(&ci) && (ci.flags & CURSOR_SHOWING) && ci.hCursor)
    {
        ICONINFO iconInfo{};
        if (GetIconInfo(ci.hCursor, &iconInfo))
        {
            int cx = ci.ptScreenPos.x - vx - static_cast<int>(iconInfo.xHotspot);
            int cy = ci.ptScreenPos.y - vy - static_cast<int>(iconInfo.yHotspot);
            DrawIconEx(memDC, cx, cy, ci.hCursor, 0, 0, 0, nullptr, DI_NORMAL);
            if (iconInfo.hbmMask) DeleteObject(iconInfo.hbmMask);
            if (iconInfo.hbmColor) DeleteObject(iconInfo.hbmColor);
        }
    }

    SelectObject(memDC, oldBmp);
    DeleteDC(memDC);
    ReleaseDC(nullptr, screenDC);

    result.width = vw;
    result.height = vh;
    result.strideBytes = vw * 4;
    result.bits = bits;
    result.hBitmap = hBitmap;
    return result;
}
