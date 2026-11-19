// Steer a car's pitch and roll in the air with the helicopter controls (aircontrol.cpp).
#pragma once

void InstallAirControl();        // first frame, with the other game hooks
void AirControlBeforeTick();     // start of a game update (fixedtick.cpp)
void AirControlAfterPresent();   // after Present (dllmain.cpp): used when FixedTick is off
