#pragma once
// Fire truck, ambulance and vigilante targets on the mainland before the bridges open (oddjobs.cpp).
// Before a script command runs (stuntjump.cpp's ProcessOneCommand hook): true when
// it was one of those jobs' island checks and has been answered here.
bool OddJobScriptCommand(unsigned char* script);
