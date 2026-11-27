#pragma once
#include <windows.h>
#include <memory>
#include <vector>

// Pixels d'une capture (BGRA, top-down, sans padding), partagés entre tous les post-its
// qui l'affichent (shared_ptr : la mémoire est libérée quand plus personne ne l'utilise).
struct PostItImage
{
    int width = 0;
    int height = 0;
    std::vector<BYTE> pixels;
    POINT screenOrigin{};   // position écran (pixels physiques) où la capture a été prise
};

// Ouvre l'image dans une fenêtre sans bord, au premier plan, à sa taille réelle (réduite si
// elle dépasse l'écran). Plusieurs post-its peuvent coexister.
void OpenPostIt(HINSTANCE hInstance, std::shared_ptr<const PostItImage> image);

void CloseAllPostIts();
int PostItCount();

// Mode traversable (global, tous les post-its) : les clics passent à travers les fenêtres.
bool PostItsClickThrough();
void SetPostItsClickThrough(bool enabled);
