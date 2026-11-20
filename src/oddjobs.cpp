// Fire truck, ambulance and vigilante jobs on the mainland before the bridges open.
//
// The three vehicle jobs (mission scripts "firetru" main.scm 0x106F5C, "ambulan"
// 0x104DED, "copcar" 0x109078) pick each target - the burning car, the patient, the
// criminal - at a random spot, and ask twice whether the mainland is open: global
// $236, set to 1 by "Phnom Penh '86" (BARON2, main.scm 0x69B98) when the bridges
// open, 0 until then (00D6 0; 0038 $236 0; 004D ...):
//  1. where to look: $236 = 0 -> a random spot on the beach islands only (x -247..683,
//     y -1813..1308); open -> on the island the player is on (mainland box x -1802..
//     -270, y -1518..1591);
//  2. which spots to throw away: $236 = 0 -> any on the mainland and the middle
//     islands.
// Then the spot must be 150 to 600 m from the player, or it tries again. Driving a
// fire truck, ambulance or police car on the mainland before the bridges open (a
// trainer, a mod or a save that got there), almost every spot was too far: no burning
// car, no blip, and the job ran out of time with nothing to do.
//
// Now, while the player is on the mainland (the game's own box above), those checks in
// those three scripts are answered "open", so targets are picked around the player as
// they are after the bridges open. On the beach islands the game's answer stands, and
// no other script's $236 check is touched.
#include "oddjobs.h"
#include "config.h"
#include <windows.h>

static const DWORD SCRIPT_SPACE = 0x821280, UPDATE_COMPARE = 0x463F00;
static const unsigned short V_ISLANDS_OPEN = 236;
static const auto FindPlayerVehicle = (unsigned char* (__cdecl*)())0x4BC1E0;
static const auto FindPlayerPed = (unsigned char* (__cdecl*)())0x4BC120;
static unsigned g_logs = 0;

static bool IsVehicleJob(const unsigned char* script) {
    const char* n = (const char*)(script + 8);
    return !lstrcmpiA(n, "firetru") || !lstrcmpiA(n, "ambulan") || !lstrcmpiA(n, "copcar");
}

static bool OnMainland() {
    const unsigned char* e = FindPlayerVehicle();
    if (!e) e = FindPlayerPed();
    if (!e) return false;
    const float* p = (const float*)(e + 0x34);
    return p[0] >= -1802.615f && p[0] <= -270.134f && p[1] >= -1518.473f && p[1] <= 1591.896f;
}

bool OddJobScriptCommand(unsigned char* script) {
    if (!IsVehicleJob(script)) return false;
    DWORD& ip = *(DWORD*)(script + 0x10);
    const unsigned char* p = (const unsigned char*)(SCRIPT_SPACE + ip);
    const unsigned raw = p[0] | p[1] << 8;
    // 0038 $236 0 (global var, int8 0), with or without NOT
    if ((raw & 0x7FFF) != 0x0038 || p[2] != 2 || *(const unsigned short*)(p + 3) != V_ISLANDS_OPEN * 4 ||
        p[5] != 4 || p[6] != 0)
        return false;
    if (*(const int*)(SCRIPT_SPACE + V_ISLANDS_OPEN * 4) != 0 || !OnMainland()) return false;   // the game's answer
    ip += 7;
    script[0x82] = (raw & 0x8000) ? 1 : 0;   // the command's NOT flag, as UpdateCompareFlag reads it
    ((void (__thiscall*)(void*, int))UPDATE_COMPARE)(script, 0);   // "$236 == 0" is false: open
    if (g_logs < 6) {
        g_logs++;
        Log("OddJobs: %s on the mainland before the bridges open: targets picked around the player", (const char*)(script + 8));
    }
    return true;
}
