// Widescreen ([Graphics] Widescreen): the 3D picture and the HUD for the screen's
// real shape instead of the game's stretched 4:3 (widescreen.cpp).
#pragma once

void InstallWidescreen();   // first frame, with the other game hooks
void WidescreenFrame();     // every frame, before Present
void InstallNearClip();     // first frame: near clip brought in for views wider than the game's
