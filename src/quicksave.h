// Quick save / quick load (quicksave.cpp).
#pragma once

void InstallQuickSave();       // first frame, with the other game hooks
void QuickSaveFrame();         // start of every frame (fixedtick.cpp): reads the keys
void QuickSaveBeforeTick();    // start of a game update (fixedtick.cpp): on-screen messages
void QuickSaveEndOfTick();     // end of a game update, after it is drawn (fixedtick.cpp)
void QuickSaveAfterPresent();  // after Present (dllmain.cpp): used when FixedTick is off
