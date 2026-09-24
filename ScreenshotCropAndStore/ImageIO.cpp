#include "ImageIO.h"
#include <wincodec.h>
#include <wrl/client.h>
#include <vector>
#include <cstring>

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

using Microsoft::WRL::ComPtr;

namespace
{
    // Copie la sous-région 'crop' de l'image source dans un buffer BGRA top-down compact.
    bool CropPixels(const CapturedImage& full, RECT crop, std::vector<BYTE>& outBuffer, int& outW, int& outH)
    {
        if (!full.IsValid()) return false;

        int w = crop.right - crop.left;
        int h = crop.bottom - crop.top;
        if (w <= 0 || h <= 0) return false;
        if (crop.left < 0 || crop.top < 0 || crop.right > full.width || crop.bottom > full.height) return false;

        outW = w;
        outH = h;
        outBuffer.resize(static_cast<size_t>(w) * h * 4);

        const BYTE* src = static_cast<const BYTE*>(full.bits);
        for (int row = 0; row < h; ++row)
        {
            const BYTE* srcRow = src
                + static_cast<size_t>(crop.top + row) * full.strideBytes
                + static_cast<size_t>(crop.left) * 4;
            BYTE* dstRow = outBuffer.data() + static_cast<size_t>(row) * w * 4;
            memcpy(dstRow, srcRow, static_cast<size_t>(w) * 4);
        }
        return true;
    }
}

bool SaveCroppedPng(const CapturedImage& full, RECT crop, const std::wstring& path)
{
    std::vector<BYTE> pixels;
    int w = 0, h = 0;
    if (!CropPixels(full, crop, pixels, w, h)) return false;

    ComPtr<IWICImagingFactory> wicFactory;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&wicFactory));
    if (FAILED(hr)) return false;

    ComPtr<IWICStream> stream;
    hr = wicFactory->CreateStream(&stream);
    if (FAILED(hr)) return false;

    hr = stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE);
    if (FAILED(hr)) return false;

    ComPtr<IWICBitmapEncoder> encoder;
    hr = wicFactory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
    if (FAILED(hr)) return false;

    hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
    if (FAILED(hr)) return false;

    ComPtr<IWICBitmapFrameEncode> frame;
    hr = encoder->CreateNewFrame(&frame, nullptr);
    if (FAILED(hr)) return false;

    hr = frame->Initialize(nullptr);
    if (FAILED(hr)) return false;

    hr = frame->SetSize(static_cast<UINT>(w), static_cast<UINT>(h));
    if (FAILED(hr)) return false;

    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    hr = frame->SetPixelFormat(&format);
    if (FAILED(hr)) return false;

    UINT stride = static_cast<UINT>(w) * 4;
    hr = frame->WritePixels(static_cast<UINT>(h), stride,
        static_cast<UINT>(pixels.size()), pixels.data());
    if (FAILED(hr)) return false;

    hr = frame->Commit();
    if (FAILED(hr)) return false;

    hr = encoder->Commit();
    return SUCCEEDED(hr);
}

bool CopyCroppedToClipboard(HWND owner, const CapturedImage& full, RECT crop)
{
    std::vector<BYTE> pixels;
    int w = 0, h = 0;
    if (!CropPixels(full, crop, pixels, w, h)) return false;

    BITMAPINFOHEADER bih{};
    bih.biSize = sizeof(BITMAPINFOHEADER);
    bih.biWidth = w;
    bih.biHeight = h; // positif = bottom-up, attendu par CF_DIB
    bih.biPlanes = 1;
    bih.biBitCount = 32;
    bih.biCompression = BI_RGB;
    bih.biSizeImage = static_cast<DWORD>(w) * h * 4;

    size_t totalSize = sizeof(BITMAPINFOHEADER) + static_cast<size_t>(bih.biSizeImage);
    HGLOBAL hGlobal = GlobalAlloc(GMEM_MOVEABLE, totalSize);
    if (!hGlobal) return false;

    void* dest = GlobalLock(hGlobal);
    if (!dest)
    {
        GlobalFree(hGlobal);
        return false;
    }

    memcpy(dest, &bih, sizeof(BITMAPINFOHEADER));
    BYTE* destPixels = static_cast<BYTE*>(dest) + sizeof(BITMAPINFOHEADER);

    // Notre buffer source est top-down ; CF_DIB attend bottom-up : on inverse les lignes.
    for (int row = 0; row < h; ++row)
    {
        const BYTE* srcRow = pixels.data() + static_cast<size_t>(row) * w * 4;
        BYTE* dstRow = destPixels + static_cast<size_t>(h - 1 - row) * w * 4;
        memcpy(dstRow, srcRow, static_cast<size_t>(w) * 4);
    }

    GlobalUnlock(hGlobal);

    if (!OpenClipboard(owner))
    {
        GlobalFree(hGlobal);
        return false;
    }
    EmptyClipboard();
    // Le presse-papier prend possession de hGlobal en cas de succès : ne pas le libérer nous-mêmes.
    HANDLE result = SetClipboardData(CF_DIB, hGlobal);
    CloseClipboard();

    if (!result)
    {
        GlobalFree(hGlobal);
        return false;
    }
    return true;
}
