#pragma once
// Light coronas and far city lights (farlights.cpp).
void PatchCoronaSlotsEarly();   // DllMain: 120 corona slots instead of 56
void InstallFarLights();        // first frame
void FarLightsLog();            // once a frame: a line every 2 minutes
