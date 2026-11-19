// Faster model and texture streaming (faststream.cpp).
#pragma once

void InstallFastStream();        // first frame, with the other game hooks
void NoIslandScreenApply();      // the Modern Fix menu changed [Streaming] NoIslandScreen
void FastStreamBeforeTick();     // start of a game update (fixedtick.cpp)
void FastStreamAfterPresent();   // after Present (dllmain.cpp): used when FixedTick is off
