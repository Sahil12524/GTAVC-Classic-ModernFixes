#pragma once

// Ped pool size (the game's: 140). Set early (PatchPedPoolEarly, from DllMain,
// before CPools::Initialise); arrays kept per pool slot (interp.cpp, gameplay.cpp)
// are this big.
static const int PED_POOL_SLOTS = 210;
void PatchPedPoolEarly();

void InstallPopulation();
void PopulationSettingsChanged();   // the Modern Fix menu changed a density or KeepOffscreen
void PopulationAfterPresent();      // each frame: fewer street people while the ped pool is nearly full
// Before a script command runs (stuntjump.cpp's ProcessOneCommand hook): true when
// it was aimed at a character that doesn't exist and has been stepped over.
bool ScriptCharGuard(unsigned char* script);
