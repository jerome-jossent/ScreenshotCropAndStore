#pragma once
#include <windows.h>
#include <utility>

// Détient un bitmap DIB 32bpp top-down (BGRA, alpha non significatif) représentant
// une capture du bureau virtuel. Non copiable (propriétaire de l'HBITMAP), déplaçable.
struct CapturedImage
{
    int width = 0;
    int height = 0;
    int strideBytes = 0;   // width * 4
    void* bits = nullptr;  // pointeur vers les pixels (propriété de hBitmap, ne pas libérer séparément)
    HBITMAP hBitmap = nullptr;

    CapturedImage() = default;
    CapturedImage(const CapturedImage&) = delete;
    CapturedImage& operator=(const CapturedImage&) = delete;

    CapturedImage(CapturedImage&& other) noexcept { *this = std::move(other); }

    CapturedImage& operator=(CapturedImage&& other) noexcept
    {
        if (this != &other)
        {
            Reset();
            width = other.width;
            height = other.height;
            strideBytes = other.strideBytes;
            bits = other.bits;
            hBitmap = other.hBitmap;

            other.width = 0;
            other.height = 0;
            other.strideBytes = 0;
            other.bits = nullptr;
            other.hBitmap = nullptr;
        }
        return *this;
    }

    ~CapturedImage() { Reset(); }

    void Reset()
    {
        if (hBitmap)
        {
            DeleteObject(hBitmap);
            hBitmap = nullptr;
        }
        bits = nullptr;
    }

    bool IsValid() const { return hBitmap != nullptr && bits != nullptr; }
};

// Capture l'intégralité du bureau virtuel (tous les écrans), curseur incrusté.
// 'bounds' reçoit la position/taille du bureau virtuel en coordonnées écran.
CapturedImage CaptureVirtualScreen(RECT& bounds);
