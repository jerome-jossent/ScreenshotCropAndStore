#pragma once
#include <windows.h>
#include "Capture.h"

// Affiche l'overlay de sélection plein écran (boucle de message dédiée : bloque l'appelant
// jusqu'à validation ou annulation, comme une boîte de dialogue modale).
// Retourne true si une zone a été validée ; outSelection reçoit alors le rectangle choisi
// en coordonnées locales à l'image capturée (0,0 = coin haut-gauche du bureau virtuel).
bool ShowSelectionOverlay(HINSTANCE hInstance, const CapturedImage& capture, const RECT& virtualBounds, RECT& outSelection);
