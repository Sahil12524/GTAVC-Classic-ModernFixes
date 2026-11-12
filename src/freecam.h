// Mouse look around the player's vehicle (freecam.cpp).
#pragma once

void InstallFreeCam();       // first frame, with the other game hooks
void FreeCamAfterUpdate();   // after each game update, before it is drawn (fixedtick.cpp)
