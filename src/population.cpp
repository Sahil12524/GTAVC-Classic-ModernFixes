// Street population ([Population]).
//
// Density. At start-up the game (0x627FE0, called at 0x4A4B2D) reads two numbers from
// gta3.ini, a ped and a car density factor (0x6DBA00 / 0x6DBA04, kept between 0.5 and
// 3). Without that file both stay 0.6. From them it works out the most peds on the
// streets (MaxNumberOfPedsInUse, 0x694DC8 = 25 x factor; 0x694DCC = 40 x factor inside
// interiors) and the most traffic cars (MaxNumberOfCarsInUse, 0x686FCC = 12 x factor).
// The ped factor also scales how many peds a zone asks for (AddToPopulation, 0x53BE76),
// the car factor how many cars (0x426EB4). So a plain install has at most 15 peds
// and 7 cars. Here the factors come from the ini, and they're applied again after
// that start-up code runs, so a gta3.ini can't undo them.
//
// Off-screen removal. CPopulation::ManagePopulation (0x53D690) removes a ped more
// than 51 m away, and one that is off-screen (CEntity::GetIsOnScreen, 0x4885D0) more
// than 25 m away (0x694EB4, read at 0x53DEDA, times the speed-based creation
// multiplier). Look at a ped 30 m away, turn the camera, and it is gone. That
// distance is read from KeepOffscreen instead. (0x694EB4 is also used elsewhere,
// so only this one read is moved.)
#include "population.h"
#include "config.h"
#include <windows.h>
#include <string.h>

static float* const PedFactor = (float*)0x6DBA00;
static float* const CarFactor = (float*)0x6DBA04;
static int* const MaxPeds = (int*)0x694DC8;
static int* const MaxPedsInterior = (int*)0x694DCC;
static int* const MaxCars = (int*)0x686FCC;
static const DWORD READ_DENSITY = 0x627FE0, READ_DENSITY_CALL = 0x4A4B2D;
static const DWORD OFFSCREEN_FLD = 0x53DEDA;

static float g_offscreenDist = 25.0f;

static void ApplyDensity() {
    if (g_cfg.pedDensity > 0) {
        *PedFactor = g_cfg.pedDensity;
        *MaxPeds = (int)(25.0f * g_cfg.pedDensity);
        *MaxPedsInterior = (int)(40.0f * g_cfg.pedDensity);
    }
    if (g_cfg.carDensity > 0) {
        *CarFactor = g_cfg.carDensity;
        *MaxCars = (int)(12.0f * g_cfg.carDensity);
    }
}

static void __cdecl ReadDensity() {
    ((void (__cdecl*)())READ_DENSITY)();
    ApplyDensity();
}

static void Patch(DWORD at, const void* code, size_t n) {
    DWORD old;
    VirtualProtect((void*)at, n, PAGE_EXECUTE_READWRITE, &old);
    memcpy((void*)at, code, n);
    VirtualProtect((void*)at, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)at, n);
}

void InstallPopulation() {
    if (g_cfg.pedDensity > 0 || g_cfg.carDensity > 0) {
        const unsigned char* p = (const unsigned char*)READ_DENSITY_CALL;
        const bool ok = p[0] == 0xE8 && READ_DENSITY_CALL + 5 + *(const int*)(p + 1) == READ_DENSITY
                     && *(const DWORD*)0x628139 == (DWORD)MaxPeds && *(const DWORD*)0x628191 == (DWORD)MaxCars;
        if (ok) {
            unsigned char call[5] = { 0xE8 };
            *(int*)(call + 1) = (int)((DWORD)&ReadDensity - (READ_DENSITY_CALL + 5));
            Patch(READ_DENSITY_CALL, call, sizeof call);
            ApplyDensity();   // in case the start-up code already ran
            Log("Population: ped density %.2f (up to %d peds), car density %.2f (up to %d cars)",
                *PedFactor, *MaxPeds, *CarFactor, *MaxCars);
        } else {
            Log("Population: density code is not v1.0; density not changed");
        }
    }
    if (g_cfg.offscreenKeep > 0) {
        static const unsigned char fld[] = { 0xD9, 0x05, 0xB4, 0x4E, 0x69, 0x00 };   // fld [0x694EB4]
        if (!memcmp((void*)OFFSCREEN_FLD, fld, sizeof fld)) {
            g_offscreenDist = g_cfg.offscreenKeep;
            unsigned char code[6] = { 0xD9, 0x05 };
            *(DWORD*)(code + 2) = (DWORD)&g_offscreenDist;
            Patch(OFFSCREEN_FLD, code, sizeof code);
            Log("Population: peds out of view are kept up to %.0f m away (game: 25 m)", g_offscreenDist);
        } else {
            Log("Population: ped removal code is not v1.0; off-screen distance not changed");
        }
    }
}
