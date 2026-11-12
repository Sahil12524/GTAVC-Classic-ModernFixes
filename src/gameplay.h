// Mouse aim for the Rhino and fire truck, and mission leftovers (gameplay.cpp).
#pragma once

void InstallGameplay();                           // first frame, with the other game hooks
void GameplayBeforeTick();                        // start of a game update (fixedtick.cpp)
void GameplayAfterPresent();                      // after Present (dllmain.cpp): used when FixedTick is off
bool MouseAimVehicle(const unsigned char* veh);   // the vehicle aims with the camera (FreeCam keeps looking)
