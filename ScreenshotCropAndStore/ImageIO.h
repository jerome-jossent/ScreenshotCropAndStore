#pragma once
#include <windows.h>
#include <string>
#include <vector>
#include "Capture.h"

// Découpe 'crop' (coordonnées locales à l'image capturée) et l'enregistre en PNG à 'path'.
bool SaveCroppedPng(const CapturedImage& full, RECT crop, const std::wstring& path);

// Découpe 'crop' et copie le résultat dans le presse-papier (format CF_DIB, le plus compatible).
bool CopyCroppedToClipboard(HWND owner, const CapturedImage& full, RECT crop);

// Copie la sous-région 'crop' (coordonnées locales) dans un buffer BGRA top-down compact.
// Utilisé pour conserver en mémoire la dernière capture (post-it).
bool CropToBuffer(const CapturedImage& full, RECT crop, std::vector<BYTE>& outBuffer, int& outW, int& outH);
