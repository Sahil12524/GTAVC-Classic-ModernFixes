// Unique stunt jumps without slow motion or the cinematic camera (stuntjump.cpp).
#pragma once

void InstallStuntJump();   // first frame, with the other game hooks
bool ScriptHookInstalled();   // the script command hook is in (missionhelp.cpp needs it to see missions start)
