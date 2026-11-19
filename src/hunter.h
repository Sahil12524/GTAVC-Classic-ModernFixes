// Hunter: mouse wheel picks rockets or cannon, one key fires it (hunter.cpp).
#pragma once

void InstallHunter();         // first frame, with the other game hooks
void HunterBeforeTick();      // start of a game update (fixedtick.cpp)
void HunterAfterPresent();    // after Present (dllmain.cpp): used when FixedTick is off
void TrainerWindowFrame();   // redraw frame (fixedtick.cpp): the trainer window's cursor moves every frame
