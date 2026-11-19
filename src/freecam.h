// Mouse look around the player's vehicle (freecam.cpp).
#pragma once

void InstallFreeCam();       // first frame, with the other game hooks
void FreeCamBoatsApply();     // the menu changed FreeCam / FreeCamBoats
void FreeCamAfterUpdate();   // after each game update, before it is drawn (fixedtick.cpp)
// Each drawn frame (interp.cpp): the view turn (yaw left +, pitch up +) the next update
// will make from mouse counts it hasn't read yet, plus what the latest update turned
// from the mouse, and the vehicle the camera orbits. False when this camera isn't in use.
bool FreeCamFrameTurn(float mx, float my, float* dYaw, float* dPitch, float* lastYaw, float* lastPitch,
                      const unsigned char** veh);
