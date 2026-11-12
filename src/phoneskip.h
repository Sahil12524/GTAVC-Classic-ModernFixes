// Enter skips the phone call you are on (phoneskip.cpp).
#pragma once

void InstallPhoneSkip();      // first frame, with the other game hooks
void PhoneSkipFrame();        // start of every frame (fixedtick.cpp): reads the key
void PhoneSkipBeforeTick();   // start of a game update, before the scripts run (fixedtick.cpp)
