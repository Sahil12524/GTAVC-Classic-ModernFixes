// Skip a mission's scripted intro scene; end a mission that got stuck (missionhelp.cpp).
#pragma once

void InstallMissionHelp();        // first frame, with the other game hooks
void MissionHelpBeforeTick();     // start of a game update, before the scripts run (fixedtick.cpp)
void MissionHelpAfterPresent();   // after Present (dllmain.cpp): used when FixedTick is off
void MissionLaunching();          // START_MISSION is about to run (stuntjump.cpp's script hook)
void MissionPassShown();          // a script shows MISSION PASSED (M_PASS) (stuntjump.cpp's script hook)
void MissionFailShown();          // a script shows MISSION FAILED (M_FAIL) (stuntjump.cpp's script hook)
