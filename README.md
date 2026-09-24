# ScreenshotCropAndStore (C++ / Win32 / Direct2D)

Portage natif du prototype C#/WinForms, pensé pour une fluidité maximale de l'overlay de
sélection : le rendu passe de logiciel (GDI+, CPU) à **accéléré GPU (Direct2D)**, synchronisé
au rafraîchissement de l'écran (vsync) via `EndDraw()`. Résultat : on peut redessiner l'écran
entier à chaque mouvement de souris sans les optimisations "dirty rect" qui étaient
nécessaires côté GDI+.

## Fonctionnalités (identiques à la version C#)

- **Impr écran** : capture tout le bureau virtuel (curseur inclus), overlay plein écran pour
  cadrer la zone (glisser-déposer, `Échap` pour annuler).
- **Ctrl + Impr écran** : reprend une capture fraîche, recadrée automatiquement sur la
  dernière zone sélectionnée — sans repasser par le cadrage à la souris.
- **Loupe** (32×32 px source, zoom ×8, rendu net nearest-neighbor) qui suit le curseur, avec
  réticule sur le pixel exact et grande croix de visée (vide au centre).
- **Lignes de repérage** horizontale/verticale plein écran suivant le curseur.
- Copie presse-papier (`CF_DIB`) + enregistrement PNG horodaté (`AAAA-MM-JJ_HH-mm-ss.png`)
  dans un dossier configurable.
- Icône dans le tray (menu : nouvelle capture, répéter la dernière zone, ouvrir/changer le
  dossier, démarrage avec Windows, quitter).
- Démarrage automatique via `HKCU\...\Run` (pas de droits admin nécessaires).

## Pourquoi c'est plus fluide qu'en C#/GDI+

GDI+ (WinForms) rasterise les formes en logiciel, sans synchronisation d'affichage : chaque
`Invalidate()` déclenche un travail CPU non coordonné avec le taux de rafraîchissement de
l'écran, d'où les saccades malgré les optimisations de zone. Direct2D, lui, s'appuie sur le
GPU et se présente via DWM en étant nativement cadencé au vsync — c'est le même mécanisme que
les compositeurs de bureau ou les jeux utilisent pour un rendu fluide.

## Build

1. Ouvrir `ScreenshotCropAndStore.sln` dans Visual Studio 2022 (charge de travail **"Développement
   Desktop en C++"**).
2. Sélectionner la configuration `Release` / `x64`.
3. Compiler (`Ctrl+Maj+B`). L'exécutable est généré dans `bin\Release\ScreenshotCropAndStore.exe`.

Aucune dépendance externe : tout repose sur des bibliothèques Windows natives (Direct2D,
DirectWrite, WIC, GDI, Shell32) déjà présentes sur Windows 10/11. Pas de runtime à installer,
pas de package NuGet/vcpkg — l'exécutable est autonome et très léger (quelques centaines de
Ko).

## Premier lancement

- Dossier par défaut des captures : `Images\Captures` dans le profil utilisateur.
- "Démarrer avec Windows" activé par défaut au premier lancement.
- Même point d'attention Windows 11 que la version C# : si *Paramètres > Accessibilité >
  Clavier > "Utiliser la touche Impr écran pour ouvrir la capture d'écran"* est activé,
  désactive-le pour éviter un déclenchement en double avec l'outil Windows.

## Pistes d'amélioration possibles

- Utiliser l'API de duplication de bureau (DXGI Desktop Duplication) au lieu de `BitBlt` pour
  la capture initiale — gain surtout utile en cas de captures très fréquentes ou d'options
  HDR/multi-GPU avancées ; `BitBlt` reste largement suffisant ici car la capture n'a lieu
  qu'une fois par screenshot, pas à chaque frame.
- Éditeur d'annotations post-capture (flèches, texte, floutage), en réutilisant `Overlay.cpp`
  comme base.
