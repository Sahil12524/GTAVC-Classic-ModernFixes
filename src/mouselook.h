// Per-frame mouse look and mouse feel (mouselook.cpp).
#pragma once

void InstallFrameMouse();   // first frame, with the other game hooks
// Each drawn frame (interp.cpp), first: reads the mouse while in play.
void MouseFrameBegin();
// Mouse counts read but not yet handed to the game (as the game will read them).
bool MouseFrameCounts(float* dx, float* dy);
// On foot: true while the mouse camera is in use; dYaw / dPitch are the drawn turn
// (radians) on top of the latest update's camera.
bool FrameMouseTurn(float blend, float* dYaw, float* dPitch);
// At each capture (interp.cpp, InterpCapture): the captured camera front's yaw /
// pitch (radians) and the previous capture's; ok = both fronts usable. The game's
// own turn this tick, and whether it is a cut / snap, are decided here once and
// held for every frame drawn until the next capture.
void MouseTickCaptured(bool ok, float yaw, float pitch, float prevYaw, float prevPitch);
bool MouseTickSnap();   // the latest tick's own turn was a cut / snap: drawn blended
// CamCheck: ticks where CCam Beta / Alpha changed between the capture and the next
// mouse read (written by something after the update), and the biggest change
// (degrees); both reset by the call.
void MouseTakeLateWrites(int* count, float* maxDeg);
bool TrainerWindowOpen();   // hunter.cpp: a trainer window has the mouse
