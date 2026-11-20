// Unique stunt jumps without the slow motion and the cinematic camera.
//
// In Vice City the unique stunt jumps are not exe code: main.scm runs a script
// named "usj" that watches every jump's start box. When the player's vehicle
// enters one, it runs (main.scm v1.0 offsets):
//   0x14C9C  015D SET_TIME_SCALE 0.3                       slow motion
//   0x14CA3  015F SET_FIXED_CAMERA_POSITION $0D00 $0D04 $0D08 0 0 0
//   0x14CBD  0158 CAMERA_ON_VEHICLE $0C54 15 (fixed) 2 (jump cut)
//   ...      loop while the vehicle is in the air; landing box -> $0C68 = 1
//   0x14DB6  0001 WAIT 300
//   0x14DBB  015D SET_TIME_SCALE 1.0
//   0x14DC2  02EB RESTORE_CAMERA_JUMPCUT
// then pays the reward ("UNIQUE STUNT BONUS!") if the landing counted.
//
// Scripts run one command at a time through CRunningScript::ProcessOneCommand
// (0x44FBE0, thiscall, al = 0: carry on), called by CRunningScript::Process at
// 0x44FDB5 (m_nIp at +0x10 is an offset into the script space at 0x821280, the
// name at +8). That call is redirected: while the "usj" script is about to run one
// of those four commands, the command is stepped over (its parameters skipped by
// their data types) instead of run. The jump detection, landing check and reward
// all run as before, at normal speed, with the game's own chase camera following
// the vehicle. Any other script, and any other command, runs unchanged.
#include "stuntjump.h"
#include "config.h"
#include "missionhelp.h"
#include "population.h"
#include "pizza.h"
#include "oddjobs.h"
#include <windows.h>
#include <string.h>

static const DWORD PROCESS_SITE = 0x44FDB5, PROCESS_ONE = 0x44FBE0, SCRIPT_SPACE = 0x821280;
static const DWORD SCRIPT_NAME = 0x08, SCRIPT_IP = 0x10;
static DWORD g_processOne = PROCESS_ONE;
static unsigned g_skipLogs = 0;

// The skipped commands and how many parameters each has.
static const struct { unsigned short op; int params; } kSkip[] = {
    { 0x015D, 1 },   // SET_TIME_SCALE
    { 0x015F, 6 },   // SET_FIXED_CAMERA_POSITION
    { 0x0158, 3 },   // CAMERA_ON_VEHICLE
    { 0x02EB, 0 },   // RESTORE_CAMERA_JUMPCUT
};

// Size of one parameter by its data type byte; 0 = a type these commands never use.
static int ParamSize(unsigned char type) {
    switch (type) {
    case 1: case 6: return 5;           // int32, float
    case 2: case 3: case 5: return 3;   // global var, local var, int16
    case 4: return 2;                   // int8
    default: return 0;
    }
}

static bool IsUsj(const unsigned char* script) {
    // "usj" in any case, then the end of the name
    const char* n = (const char*)(script + SCRIPT_NAME);
    return (n[0] | 0x20) == 'u' && (n[1] | 0x20) == 's' && (n[2] | 0x20) == 'j' && n[3] == 0;
}

// Length of the command at `ip` if it is one to skip, else 0.
static int SkipLength(DWORD ip) {
    const unsigned char* p = (const unsigned char*)(SCRIPT_SPACE + ip);
    const unsigned short op = (unsigned short)(p[0] | p[1] << 8);
    for (const auto& k : kSkip) {
        if (op != k.op) continue;
        int len = 2;
        for (int i = 0; i < k.params; i++) {
            const int size = ParamSize(p[len]);
            if (!size) return 0;   // not the form the stunt script uses: run it normally
            len += size;
        }
        return len;
    }
    return 0;
}

static bool g_hooked = false;
bool ScriptHookInstalled() { return g_hooked; }

static bool __fastcall HookProcessOne(unsigned char* script, void*) {
    const DWORD at = *(DWORD*)(script + SCRIPT_IP);
    const unsigned char* op = (const unsigned char*)(SCRIPT_SPACE + at);
    if (op[0] == 0x17 && op[1] == 0x04) MissionLaunching();   // 0417 START_MISSION (missionhelp.cpp)
    // 01E3 PRINT_WITH_NUMBER_BIG / 00BA PRINT_BIG "M_PASS": MISSION PASSED on screen
    if (((op[0] == 0xE3 && op[1] == 0x01) || (op[0] == 0xBA && op[1] == 0x00)) && !memcmp(op + 2, "M_PASS", 7))
        MissionPassShown();
    // 00BA PRINT_BIG "M_FAIL": MISSION FAILED! on screen (every story mission's fail path)
    if (op[0] == 0xBA && op[1] == 0x00 && !memcmp(op + 2, "M_FAIL", 7)) MissionFailShown();
    // A command aimed at a character that doesn't exist is stepped over
    // (population.cpp) instead of crashing in the command.
    if (ScriptCharGuard(script)) return false;
    // Pizza Boy's throw keys answered for the mouse (pizza.cpp)
    if (PizzaScriptCommand(script)) return false;
    // Fire truck / ambulance / vigilante targets on the mainland before the bridges open (oddjobs.cpp)
    if (OddJobScriptCommand(script)) return false;
    if (g_cfg.stuntJumpNormal && IsUsj(script)) {
        DWORD& ip = *(DWORD*)(script + SCRIPT_IP);
        if (const int len = SkipLength(ip)) {
            if (g_skipLogs < 8) {
                const unsigned char* p = (const unsigned char*)(SCRIPT_SPACE + ip);
                Log("StuntJump: skipped %02X%02X at main.scm %05lX (no slow motion, normal camera)", p[1], p[0], ip);
                g_skipLogs++;
            }
            ip += len;
            return false;   // carry on with the next command
        }
    }
    return ((bool (__thiscall*)(void*))g_processOne)(script);
}

// The same hook also tells missionhelp.cpp when a mission starts (restart prompt).
// Installed even when both are off: the Modern Fix menu (fixmenu.cpp) turns them on
// and off while playing.
void InstallStuntJump() {
    // ProcessOneCommand: inc word [CTheScripts::CommandsExecuted]; mov eax,[ecx+10h];
    // movzx edx, byte [eax+821280h]
    static const unsigned char processOne[] = { 0x66, 0xFF, 0x05, 0x66, 0x0A, 0xA1, 0x00, 0x8B, 0x41, 0x10,
                                                0x0F, 0xB6, 0x90, 0x80, 0x12, 0x82, 0x00 };
    // Process: mov ecx,ebx; call ...; test al,al; je (back to mov ecx,ebx)
    const unsigned char* site = (const unsigned char*)PROCESS_SITE;
    if (site[-2] != 0x89 || site[-1] != 0xD9 || site[0] != 0xE8 || site[5] != 0x84 || site[6] != 0xC0 ||
        memcmp((void*)PROCESS_ONE, processOne, sizeof processOne)) {
        Log("StuntJump: script code is not v1.0; stunt jumps keep their slow motion, no mission restart prompt");
        return;
    }
    const DWORD target = PROCESS_SITE + 5 + *(const int*)(site + 1);
    if (target != PROCESS_ONE) Log("StuntJump: script command call hooked by another mod (%08lX); chaining to it", target);
    g_processOne = target;
    unsigned char call[5] = { 0xE8 };
    *(int*)(call + 1) = (int)((DWORD)&HookProcessOne - (PROCESS_SITE + 5));
    DWORD old;
    VirtualProtect((void*)PROCESS_SITE, 5, PAGE_EXECUTE_READWRITE, &old);
    memcpy((void*)PROCESS_SITE, call, 5);
    VirtualProtect((void*)PROCESS_SITE, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)PROCESS_SITE, 5);
    g_hooked = true;
    Log(g_cfg.stuntJumpNormal ? "StuntJump: unique stunt jumps play at normal speed with the normal camera (on)"
                              : "StuntJump: unique stunt jumps at normal speed (off: slow motion and jump camera, as the game)");
}
