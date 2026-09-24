#pragma once
#include <windows.h>
#include <string>
#include "Capture.h"

// Découpe 'crop' (coordonnées locales à l'image capturée) et l'enregistre en PNG à 'path'.
bool SaveCroppedPng(const CapturedImage& full, RECT crop, const std::wstring& path);

// Découpe 'crop' et copie le résultat dans le presse-papier (format CF_DIB, le plus compatible).
bool CopyCroppedToClipboard(HWND owner, const CapturedImage& full, RECT crop);
