// Mission helpers: skip a mission's scripted intro scene, and end a mission that
// got stuck.
//
// Scripted scenes. Many missions (the RC missions such as Demolition Man, most
// mission intros) don't play a real cutscene (CCutsceneMgr, which Enter already
// skips) but a scene the mission script stages itself: player control off,
// widescreen bars on (TheCamera +0x6D, 0x7E46F5), script cameras, subtitles and
// speech, each step held by a WAIT. Those can't be skipped in the game.
//
// Intro skip ([Missions] IntroSkip). During such a scene (on a mission, widescreen
// on, player controls off, no real cutscene), holding the key (Enter) for 3 s
// fast-forwards it: every update each mission script's wake time (CRunningScript
// +0x7C, which WAIT sets) is cleared, so every WAIT ends at the next update, and its
// TIMERA / TIMERB (locals 16 and 17, +0x70 / +0x74) jump 10 s ahead, so the usual
// "TIMERA = 0, loop until TIMERA > 5000" steps (the RC intros) end too; mission
// speech is not played and counts as finished (the phone-skip hooks); subtitles
// are cleared. The script still runs every command in order, so whatever the scene
// sets up still happens. It stops once the scene ends (widescreen off or control
// back), a real cutscene starts, the mission ends, or after 60 s.
//
// Stuck missions ([Missions] SoftlockSeconds, EndMissionKey). A mission is
// treated as stuck when the player has had no control (CPad +0xF0) for that long
// on a mission, outside real cutscenes, phone calls and the intro skip. A subtitle
// then warns, and 15 s later, if nothing changed, the mission is ended. Holding the
// end key (End) for 3 s on a mission ends it at once, for missions stuck in a way
// that can't be detected (the player still has control).
//
// Ending a mission does what the game does when you are wasted or busted on one
// (CRunningScript::DoDeatharrestCheck, 0x460D00), without the death: the mission
// script's gosub stack (+0x14, depth +0x2C) is unwound to its bottom entry (ip
// +0x10 = stack[0]), "wasted/busted happened" (+0x84) is set, the on-mission
// variable (script space + [0x978748]) is cleared, the script is woken and the
// game's message clean-up (0x584130) runs. The mission script then runs its own
// "mission failed" and clean-up code, as after a death, and its marker comes back:
// start the mission again from there. Two seconds later anything still left over
// is put back: player control (CPlayerInfo::MakePlayerSafe(false), 0x4BBC10, as
// SET_PLAYER_CONTROL on), widescreen off (0x46B863), the camera (CCamera::Restore,
// 0x46BC7D), a fade in (CCamera::Fade, 0x46FBCB) and normal game speed.
#include "missionhelp.h"
#include "phoneskip.h"
#include "stuntjump.h"
#include "config.h"
#include <windows.h>
#include <string.h>

static const DWORD GAME_STATE = 0x9B5F08, MENU_ACTIVE = 0x869668, GAME_WINDOW = 0x7897A4, CLOCK = 0x974B2C;
static const DWORD ACTIVE_SCRIPTS = 0x975338, SCRIPT_SPACE = 0x821280, ON_MISSION_VAR = 0x978748;
static const DWORD SCR_IP = 0x10, SCR_STACK = 0x14, SCR_DEPTH = 0x2C, SCR_MISSION = 0x7A, SCR_WAKE = 0x7C,
                   SCR_WB_DONE = 0x84, SCR_TIMER_A = 0x70, SCR_TIMER_B = 0x74;   // TIMERA/B: locals 16 and 17 (+0x30)
static const DWORD THE_CAMERA = 0x7E4688, WIDESCREEN = THE_CAMERA + 0x6D, CUTSCENE_RUNNING = 0xA10AB2;
static const DWORD PAD0_DISABLE = 0x7DBCB0 + 0xF0, PLAYER_INFO = 0x94AD28, WB_STATE = 0x94ADF4;
static const DWORD TIME_SCALE = 0x97F264, PED_STATE = 0x244, PED_HEALTH = 0x354;
static const int PED_ANSWER_MOBILE = 36;

static const auto OnMission       = (bool (__cdecl*)())0x44FE30;
static const auto FindPlayerPed   = (unsigned char* (__cdecl*)())0x4BC120;
static const auto ClearMessages   = (void (__cdecl*)())0x5841E0;
static const auto ClearForFail    = (void (__cdecl*)())0x584130;
static const auto AddMessageJumpQ = (void (__cdecl*)(const wchar_t*, unsigned, unsigned short))0x584300;
static const auto MakePlayerSafe  = (void (__thiscall*)(void*, bool))0x4BBC10;
static const auto WideScreenOff   = (void (__thiscall*)(void*))0x46B863;
static const auto CameraRestore   = (void (__thiscall*)(void*))0x46BC7D;
static const auto CameraFade      = (void (__thiscall*)(void*, float, short))0x46FBCB;

static bool g_ok = false;   // code checked as v1.0

// Rampages (kill frenzies) aren't missions: the skull pickup's script runs
// START_KILL_FRENZY (01F9, CDarkel::StartFrenzy 0x429B60) and CDarkel::Update
// (0x42A650) keeps the timer and the count; CDarkel::Status (0xA10A72): 0 none,
// 1 running, 2 passed, 3 failed. CDarkel::ResetOnPlayerDeath (0x429F90) fails a
// running one as dying does (status 3, weapons back); the pickup's script then
// sees it (READ_KILL_FRENZY_STATUS) and shows RAMPAGE FAILED.
static const DWORD FRENZY_STATUS = 0xA10A72;
static const auto FailFrenzy = (void (__cdecl*)())0x429F90;
static bool g_frenzyOk = false;   // CDarkel code checked as v1.0
static bool FrenzyRunning() { return g_frenzyOk && *(volatile short*)FRENZY_STATUS == 1; }

static bool Playing() { return *(int*)GAME_STATE == 9 && !*(unsigned char*)MENU_ACTIVE; }
static bool InFront() { HWND w = *(HWND*)GAME_WINDOW; return w && GetForegroundWindow() == w; }
static bool NoControl() { return *(unsigned short*)PAD0_DISABLE != 0; }
static bool Cutscene() { return *(unsigned char*)CUTSCENE_RUNNING != 0; }
static bool KeyHeld(int vk) { return (GetAsyncKeyState(vk) & 0x8000) && !ModifierDown() && InFront(); }

// The game's script list; `fn` gets each mission script.
template <class F> static void ForMissionScripts(F fn) {
    int guard = 0;
    for (unsigned char* s = *(unsigned char**)ACTIVE_SCRIPTS; s && guard < 200; s = *(unsigned char**)s, guard++)
        if (s[SCR_MISSION]) fn(s);
}

// Something only the player can be in that isn't a stuck mission.
static bool PlayerBusy() {
    unsigned char* ped = FindPlayerPed();
    return !ped || *(float*)(ped + PED_HEALTH) <= 0.0f || *(int*)(ped + PED_STATE) == PED_ANSWER_MOBILE ||
           *(unsigned char*)WB_STATE != 0;
}

// ---------------------------------------------------------------- intro skip
static DWORD g_introHold = 0, g_introStart = 0;
static bool g_introSkipping = false;

static bool ScriptedScene() {
    return OnMission() && *(unsigned char*)WIDESCREEN && NoControl() && !Cutscene();
}

static void IntroSkip() {
    if (!g_cfg.introSkip && g_introSkipping) {   // turned off in the Modern Fix menu while skipping
        g_introSkipping = false;
        MissionAudioSkip(false);
    }
    if (!g_cfg.introSkip || !Playing()) return;
    const DWORD real = GetTickCount();
    if (!g_introSkipping) {
        if (!ScriptedScene() || !KeyHeld(g_cfg.introSkipKey)) { g_introHold = 0; return; }
        if (!g_introHold) { g_introHold = real; return; }
        if (real - g_introHold < 3000) return;
        g_introHold = 0;
        g_introSkipping = true;
        g_introStart = real;
        MissionAudioSkip(true);
        Log("Missions: skipping a scripted scene");
    }
    if (!ScriptedScene() || real - g_introStart > 60000) {
        g_introSkipping = false;
        MissionAudioSkip(false);
        ClearMessages();
        Log("Missions: scripted scene skipped (%lu ms)", real - g_introStart);
        return;
    }
    ForMissionScripts([](unsigned char* s) {
        *(DWORD*)(s + SCR_WAKE) = 0;          // every WAIT ends now
        *(int*)(s + SCR_TIMER_A) += 10000;    // and every "until TIMERA / TIMERB passes N ms" step
        *(int*)(s + SCR_TIMER_B) += 10000;
    });
    ClearMessages();
}

// ---------------------------------------------------------------- stuck missions
static DWORD g_stuckSince = 0, g_warnedAt = 0, g_restoreAt = 0, g_endHold = 0;
static const wchar_t kWarn[] = L"This mission seems to be stuck. It will be ended in 15 seconds unless it carries on.";
static const wchar_t kEnded[] = L"The mission was stuck and has been ended.";
static const wchar_t kEndedByKey[] = L"Mission ended.";

static bool EndMission(const wchar_t* message) {
    bool done = false;
    ForMissionScripts([&](unsigned char* s) {
        unsigned short& depth = *(unsigned short*)(s + SCR_DEPTH);
        if (done || depth == 0) return;   // not inside the mission's main gosub
        if (depth > 6) {   // the gosub stack holds 6: a script deeper than that is corrupt
            char name[9] = "";
            memcpy(name, s + 8, 8);
            for (char& c : name) if (c && (c < 32 || c > 126)) c = '?';
            Log("Missions: script %s has gosub depth %u (more than the 6 the game keeps); not ended this way", name, (unsigned)depth);
            return;
        }
        depth = 0;
        *(DWORD*)(s + SCR_IP) = ((DWORD*)(s + SCR_STACK))[0];
        s[SCR_WB_DONE] = 1;
        *(DWORD*)(s + SCR_WAKE) = 0;
        done = true;
    });
    if (!done) { Log("Missions: no mission script to end"); return false; }
    MissionFailShown();   // ended here = failed: its restart is offered
    if (const DWORD var = *(DWORD*)ON_MISSION_VAR) *(int*)(SCRIPT_SPACE + var) = 0;
    ClearForFail();
    if (g_introSkipping) { g_introSkipping = false; MissionAudioSkip(false); }
    AddMessageJumpQ(message, 7000, 0);
    g_restoreAt = *(DWORD*)CLOCK + 2000;
    if (!g_restoreAt) g_restoreAt = 1;
    return true;
}

// What a stuck scene may have left behind, after the mission's own clean-up ran.
static void RestorePlayer() {
    void* camera = (void*)THE_CAMERA;
    if (NoControl()) MakePlayerSafe((void*)PLAYER_INFO, false);
    if (*(unsigned char*)WIDESCREEN) WideScreenOff(camera);
    if (!Cutscene()) CameraRestore(camera);
    CameraFade(camera, 1.0f, 1);   // fade in
    *(float*)TIME_SCALE = 1.0f;
    Log("Missions: player control, camera and screen put back");
}

static void StuckMissions() {
    if (!Playing()) return;
    const DWORD now = *(DWORD*)CLOCK;
    if (g_restoreAt && now - g_restoreAt < 0x80000000u) { g_restoreAt = 0; RestorePlayer(); }

    // End key held 3 s on a mission or a rampage
    if (g_cfg.endMissionKey && (OnMission() || FrenzyRunning()) && KeyHeld(g_cfg.endMissionKey)) {
        const DWORD real = GetTickCount();
        if (!g_endHold) g_endHold = real ? real : 1;
        else if (real - g_endHold >= 3000) {
            g_endHold = 0;
            if (FrenzyRunning()) {   // the rampage's own script ends it as failed
                Log("Missions: end key held; failing the rampage");
                FailFrenzy();
                g_stuckSince = g_warnedAt = 0;
                return;
            }
            Log("Missions: end key held; ending the mission");
            if (EndMission(kEndedByKey)) { g_stuckSince = g_warnedAt = 0; return; }
        }
    } else {
        g_endHold = 0;
    }

    if (g_cfg.softlockSeconds <= 0) { g_stuckSince = g_warnedAt = 0; return; }
    const bool stuck = OnMission() && NoControl() && !Cutscene() && !g_introSkipping && !PlayerBusy();
    if (!stuck) { g_stuckSince = g_warnedAt = 0; return; }
    if (!g_stuckSince) { g_stuckSince = now ? now : 1; return; }
    if (now - g_stuckSince < (DWORD)g_cfg.softlockSeconds * 1000u) return;
    if (!g_warnedAt) {
        g_warnedAt = now ? now : 1;
        AddMessageJumpQ(kWarn, 15000, 0);
        Log("Missions: no player control for %d s on a mission; ending it in 15 s", g_cfg.softlockSeconds);
        return;
    }
    if (now - g_warnedAt < 15000) return;
    Log("Missions: mission still stuck; ending it");
    EndMission(kEnded);
    g_stuckSince = g_warnedAt = 0;
}

// ---------------------------------------------------------------- restart prompt
// Story missions start when their contact's script runs START_MISSION (0417,
// CRunningScript::ProcessOneCommand sees it first: stuntjump.cpp's hook calls
// MissionLaunching) while the player stands in the pink marker. That spot is
// recorded: position (+0x34), heading, the drawn area (CGame::currArea 0x978810)
// and the player's area (+0x5F), and whether they were on foot (+0x3AC), plus the
// last-passed mission name (CStats::LastMissionPassedName, 0x7E9D78).
//
// When the on-mission flag goes back to 0 after the mission showed MISSION FAILED
// (failed, wasted, busted) or was ended here (End key, stuck), without that name
// changing, and the player is back in control, alive and
// out of hospital / police station, the help box asks "Restart?" (Y / N, 30 s,
// CHud::SetHelpMessage 0x55BFC0, cleared with null as CLEAR_HELP does). Y puts
// the player back on that spot, standing still, so the contact's script sees them
// in the marker and starts the mission again the normal way.
//
// The move is done by the game's own script commands, in a tiny script started
// like START_NEW_SCRIPT does it (free list 0xA0FDDC, CRunningScript::Init
// 0x450CF0, active list 0x975338, +0x78 active). Its ip is the offset of the code
// below from the script space; it runs in the same update and ends itself:
//   03CB LOAD_SCENE x y z
//   0362 WARP_CHAR_FROM_CAR_TO_COORD player x y z   (in a vehicle)
//   00A1 SET_CHAR_COORDINATES player x y z          (on foot)
//   0173 SET_CHAR_HEADING player heading
//   0373 SET_CAMERA_BEHIND_PLAYER
//   004E TERMINATE_THIS_SCRIPT
// Some missions start in a vehicle (the RC missions: RCHELY1 waits for the player
// in the Top Fun van, model 189, within 4 m of its spot). Then the vehicle model
// and its heading are recorded too, and Y puts the player back on the spot in a
// vehicle of that model: the one they are in, else one within 15 m (vehicle pool
// 0xA0FDE4, 0x5DC a slot), else a new one (0247 REQUEST_MODEL, 038B
// LOAD_ALL_MODELS_NOW, 00A5 CREATE_CAR into a local, 036A WARP_CHAR_INTO_CAR,
// 01C3 / 0249 no longer needed), then 00A1 moves it there (the command moves the
// vehicle when the character is in one) and 0175 SET_CAR_HEADING turns it.
// The side jobs (taxi, ambulance, fire truck, vigilante, Pizza Boy) start from a
// key, not a place, so they aren't offered (mission script names "taxi",
// "ambulan", "firetru", "copcar", "pizza"), and neither is buying a property
// (a script named with "buy": lnkvbuy, ochebuy...), which never shows PASSED.
static const DWORD FREE_SCRIPTS = 0xA0FDDC, SCRIPT_INIT = 0x450CF0, SCR_ACTIVE = 0x78;
static const DWORD CURR_AREA = 0x978810, LAST_PASSED = 0x7E9D78, PED_POOL = 0x97F2AC, PED_SIZE = 0x6D8;
static const DWORD ENT_POS = 0x34, ENT_AREA = 0x5F, PED_IN_VEHICLE = 0x3AC;
static const auto SetHelpMessage = (void (__cdecl*)(const wchar_t*, bool, bool))0x55BFC0;

static const DWORD VEH_POOL = 0xA0FDE4, VEH_SIZE = 0x5DC, ENT_MODEL = 0x5C, SCR_NAME = 0x08;
static const auto FindPlayerVehicle = (unsigned char* (__cdecl*)())0x4BC1E0;
static struct {
    bool valid, onFoot, rampage, jobSeen;
    float x, y, z, heading;
    int area, model;
    unsigned char pedArea;
    char lastPassed[8], name[8];
} g_start;
static bool g_wasOnMission = false, g_promptPending = false, g_prompting = false;
static DWORD g_endedAt = 0, g_promptAt = 0, g_restartClock = 0;
static bool g_yesDown = false, g_noDown = false;
static unsigned char g_restartCode[160];
static const wchar_t kPrompt[] = L"Mission failed. Restart it? Press Y for yes, N for no.";
static const wchar_t kRampagePrompt[] = L"Rampage failed. Restart it? Press Y for yes, N for no.";

static void RecordStart(bool rampage) {
    unsigned char* ped = FindPlayerPed();
    if (!ped) return;
    unsigned char* veh = ped[PED_IN_VEHICLE] ? FindPlayerVehicle() : nullptr;
    const float* m = (const float*)((veh ? veh : ped) + 4);   // right, forward, up, position
    g_start.valid = true;
    g_start.rampage = rampage;
    g_start.onFoot = !veh;
    g_start.model = veh ? *(short*)(veh + ENT_MODEL) : -1;
    g_start.name[0] = 0;
    g_start.jobSeen = false;
    g_start.x = m[12]; g_start.y = m[13]; g_start.z = m[14];
    g_start.heading = __builtin_atan2f(-m[4], m[5]) * 57.29578f;
    g_start.area = *(int*)CURR_AREA;
    g_start.pedArea = ped[ENT_AREA];
    memcpy(g_start.lastPassed, (void*)LAST_PASSED, 8);
    if (rampage) lstrcpyA(g_start.name, "rampage");
    g_promptPending = false;
    Log("Missions: %s started at %.1f %.1f %.1f (area %d, %s %d)", rampage ? "rampage" : "mission",
        g_start.x, g_start.y, g_start.z, g_start.area, g_start.onFoot ? "on foot" : "in vehicle model", g_start.model);
}

static bool g_restartOk = false;   // the script hook is in (stuntjump.cpp): missions are seen starting
static bool g_passShown = false;   // MISSION PASSED was shown since the mission started
static bool g_failShown = false;   // MISSION FAILED was shown (or the mission was ended here) since it started

void MissionLaunching() {
    if (!g_ok || !g_restartOk || !g_cfg.missionRestart) return;
    RecordStart(false);
    g_passShown = g_failShown = false;
}

// The RC missions (rcheli, rcrace1, rcplne1) show MISSION PASSED but don't register
// a passed mission, so the last-passed name doesn't change: the message counts too.
void MissionPassShown() { g_passShown = true; }

// A mission counts as failed only when it says so. The restart used to be offered
// for any mission that ended without being passed, and missions that end without
// MISSION PASSED - the scene-only ones like An Old Friend (whose scene was skipped),
// property purchases, the opening - were offered as failed. Every story mission's
// fail path (death and arrest included) shows PRINT_BIG "M_FAIL" (80 places in
// main.scm); ending one here (End key, stuck rescue) counts as failed too.
void MissionFailShown() { g_failShown = true; }

// Script handle of a pool object: slot index << 8 | the slot's flag byte (as
// CPools::GetPedRef / GetVehicleRef; GetAt at 0x451CB0 / 0x451C70 checks it).
static int Handle(DWORD poolVar, DWORD size, unsigned char* obj) {
    const unsigned char* pool = *(const unsigned char**)poolVar;
    const DWORD objects = *(const DWORD*)pool;
    const unsigned char* flags = *(const unsigned char* const*)(pool + 4);
    const int index = (int)(((DWORD)obj - objects) / size);
    return index << 8 | flags[index];
}

// A vehicle of `model` within 15 m of the start, or null.
static unsigned char* NearbyVehicle(int model) {
    const unsigned char* pool = *(const unsigned char**)VEH_POOL;
    unsigned char* objects = *(unsigned char**)pool;
    const unsigned char* flags = *(const unsigned char* const*)(pool + 4);
    const int size = *(const int*)(pool + 8);
    for (int i = 0; i < size; i++) {
        if (flags[i] & 0x80) continue;   // free slot
        unsigned char* v = objects + i * VEH_SIZE;
        if (*(short*)(v + ENT_MODEL) != model) continue;
        const float* q = (const float*)(v + ENT_POS);
        const float dx = q[0] - g_start.x, dy = q[1] - g_start.y, dz = q[2] - g_start.z;
        if (dx * dx + dy * dy + dz * dz < 15.0f * 15.0f) return v;
    }
    return nullptr;
}

// Side jobs started from a key in their vehicle (taxi, ambulance, fire truck,
// vigilante, Pizza Boy), and buying a property: each sale is a mission script
// (lnkvbuy, ochebuy, washbuy, carbuy, boatbuy...: a name with "buy") that ends
// without MISSION PASSED, so it looked failed and offered a restart. So did the
// new game's opening (intro, initial: no MISSION PASSED either), when its scene
// was skipped.
static bool IsJob(const char* n) {
    static const char* const jobs[] = { "taxi", "ambulan", "firetru", "copcar", "pizza", "intro", "initial" };
    for (const char* j : jobs) if (!lstrcmpiA(n, j)) return true;
    char l[9];
    int i = 0;
    for (; i < 8 && n[i]; i++) l[i] = (char)(n[i] >= 'A' && n[i] <= 'Z' ? n[i] + 32 : n[i]);
    l[i] = 0;
    return strstr(l, "buy") != nullptr;
}

static unsigned char* Put(unsigned char* p, unsigned short op) { *p++ = (unsigned char)op; *p++ = (unsigned char)(op >> 8); return p; }
static unsigned char* PutInt(unsigned char* p, int v) { *p++ = 1; memcpy(p, &v, 4); return p + 4; }
static unsigned char* PutFloat(unsigned char* p, float v) { *p++ = 6; memcpy(p, &v, 4); return p + 4; }

static bool StartRestartScript(unsigned char* ped) {
    unsigned char* s = *(unsigned char**)FREE_SCRIPTS;
    if (!s) { Log("Missions: no free script slot for the restart"); return false; }
    const int me = Handle(PED_POOL, PED_SIZE, ped);
    unsigned char* cur = ped[PED_IN_VEHICLE] ? FindPlayerVehicle() : nullptr;
    unsigned char* p = g_restartCode;
    auto moveTo = [&](unsigned short op) {   // op player x y z
        p = Put(p, op); p = PutInt(p, me); p = PutFloat(p, g_start.x); p = PutFloat(p, g_start.y); p = PutFloat(p, g_start.z);
    };
    p = Put(p, 0x03CB); p = PutFloat(p, g_start.x); p = PutFloat(p, g_start.y); p = PutFloat(p, g_start.z);
    if (g_start.onFoot) {
        moveTo(cur ? 0x0362 : 0x00A1);
        p = Put(p, 0x0173); p = PutInt(p, me); p = PutFloat(p, g_start.heading);
    } else if (cur && *(short*)(cur + ENT_MODEL) == g_start.model) {
        moveTo(0x00A1);   // moves the vehicle with the player in it
        p = Put(p, 0x0175); p = PutInt(p, Handle(VEH_POOL, VEH_SIZE, cur)); p = PutFloat(p, g_start.heading);
    } else if (unsigned char* parked = NearbyVehicle(g_start.model)) {
        const int car = Handle(VEH_POOL, VEH_SIZE, parked);
        if (cur) moveTo(0x0362);
        p = Put(p, 0x036A); p = PutInt(p, me); p = PutInt(p, car);
        moveTo(0x00A1);
        p = Put(p, 0x0175); p = PutInt(p, car); p = PutFloat(p, g_start.heading);
    } else {
        if (cur) moveTo(0x0362);
        p = Put(p, 0x0247); p = PutInt(p, g_start.model);
        p = Put(p, 0x038B);
        p = Put(p, 0x00A5); p = PutInt(p, g_start.model);
        p = PutFloat(p, g_start.x); p = PutFloat(p, g_start.y); p = PutFloat(p, g_start.z);
        *p++ = 3; *p++ = 0; *p++ = 0;   // local 0
        p = Put(p, 0x0175); *p++ = 3; *p++ = 0; *p++ = 0; p = PutFloat(p, g_start.heading);
        p = Put(p, 0x036A); p = PutInt(p, me); *p++ = 3; *p++ = 0; *p++ = 0;
        p = Put(p, 0x01C3); *p++ = 3; *p++ = 0; *p++ = 0;
        p = Put(p, 0x0249); p = PutInt(p, g_start.model);
    }
    p = Put(p, 0x0373);
    p = Put(p, 0x004E);
    // START_NEW_SCRIPT: unlink from the free list, Init, link at the head of the active list
    unsigned char* next = *(unsigned char**)s;
    unsigned char* prev = *(unsigned char**)(s + 4);
    if (prev) *(unsigned char**)prev = next; else *(unsigned char**)FREE_SCRIPTS = next;
    if (next) *(unsigned char**)(next + 4) = prev;
    ((void (__thiscall*)(void*))SCRIPT_INIT)(s);
    *(DWORD*)(s + SCR_IP) = (DWORD)g_restartCode - SCRIPT_SPACE;
    unsigned char* head = *(unsigned char**)ACTIVE_SCRIPTS;
    *(unsigned char**)s = head;
    *(unsigned char**)(s + 4) = nullptr;
    if (head) *(unsigned char**)(head + 4) = s;
    *(unsigned char**)ACTIVE_SCRIPTS = s;
    s[SCR_ACTIVE] = 1;
    // the place's area, as the door scripts set it, before the move
    *(int*)CURR_AREA = g_start.area;
    ped[ENT_AREA] = g_start.pedArea;
    return true;
}

static bool KeyPressed(int vk, bool& was) {
    const bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
    const bool press = down && !was && !ModifierDown() && InFront();
    was = down;
    return press;
}

static void EndPrompt() {
    g_prompting = false;
    SetHelpMessage(nullptr, false, false);   // as CLEAR_HELP
}

static void RestartPrompt() {
    if (!g_restartOk || !g_cfg.missionRestart) {   // off (the Modern Fix menu): no prompt, none pending
        if (g_prompting) EndPrompt();
        g_start.valid = g_promptPending = false;
        return;
    }
    if (!Playing()) return;
    const DWORD now = *(DWORD*)CLOCK;
    if (now < g_restartClock || now - g_restartClock > 10000) {   // a game was loaded
        g_start.valid = g_promptPending = false;
        if (g_prompting) EndPrompt();
        g_wasOnMission = OnMission();
    }
    g_restartClock = now;

    // Rampages: recorded when one starts (the player is on the skull pickup), offered
    // again when it fails (time up, death, arrest, End key) - not when passed.
    static short lastFrenzy = 0;
    const short frenzy = g_frenzyOk ? *(volatile short*)FRENZY_STATUS : 0;
    if (frenzy != lastFrenzy) {
        if (frenzy == 1) RecordStart(true);
        else if (lastFrenzy == 1 && g_start.valid && g_start.rampage) {
            Log("Missions: rampage %s", frenzy == 2 ? "passed" : "failed: restart offered");
            if (frenzy == 3) { g_promptPending = true; g_endedAt = now; }
            else g_start.valid = false;
        }
        lastFrenzy = frenzy;
    }

    // The mission's name: the oldest mission script on the active list (a new script is
    // linked at the head, so the mission itself is behind the helpers it starts). The
    // newest one was taken before: on a new game the opening (initial) starts its
    // helper threads as mission scripts, among them one named "rampage", so the skipped
    // opening was logged as "rampage ended" and offered as a failed mission. Every
    // mission script seen while on the mission is also checked against the jobs and the
    // opening, so a helper's name can't hide them.
    const bool on = OnMission();
    if (on && g_start.valid && !g_start.rampage)
        ForMissionScripts([](unsigned char* sc) {
            char n[9] = "";
            lstrcpynA(n, (const char*)(sc + SCR_NAME), 8);
            if (IsJob(n)) g_start.jobSeen = true;
            lstrcpynA(g_start.name, n, 8);   // the last one visited is the oldest
        });
    if (g_wasOnMission && !on && g_start.valid && !g_start.rampage) {
        const bool passed = g_passShown || memcmp(g_start.lastPassed, (void*)LAST_PASSED, 8) != 0;
        const bool job = g_start.jobSeen || IsJob(g_start.name);
        const bool failed = g_failShown && !passed && !job;
        if (failed) { g_promptPending = true; g_endedAt = now; }
        Log("Missions: %s ended (%s)", g_start.name[0] ? g_start.name : "mission",
            passed ? "passed" : job ? "a side job, a purchase or the game's opening: no restart offered"
                   : failed ? "failed: restart offered" : "ended without MISSION FAILED: no restart offered");
        if (!failed) g_start.valid = false;
    }
    g_wasOnMission = on;
    // A rampage running, or a mission on: nothing to offer now. (A failed rampage's
    // prompt waits out the on-mission flag instead; starting a real mission records
    // its own start, which drops it.)
    if (frenzy == 1 || (on && !g_start.rampage)) { g_promptPending = false; if (g_prompting) EndPrompt(); return; }
    if (on) return;

    if (g_promptPending && !g_prompting) {
        unsigned char* ped = FindPlayerPed();
        const bool ready = ped && !PlayerBusy() && !NoControl() && !*(unsigned char*)WIDESCREEN && !Cutscene();
        if (!ready || now - g_endedAt < 3000) return;   // after MISSION FAILED, hospital or police station
        g_promptPending = false;
        g_prompting = true;
        g_promptAt = now;
        g_yesDown = (GetAsyncKeyState(g_cfg.restartYesKey) & 0x8000) != 0;
        g_noDown = (GetAsyncKeyState(g_cfg.restartNoKey) & 0x8000) != 0;
        SetHelpMessage(g_start.rampage ? kRampagePrompt : kPrompt, false, true);
        return;
    }
    if (!g_prompting) return;
    if (KeyPressed(g_cfg.restartNoKey, g_noDown) || now - g_promptAt > 30000) {
        EndPrompt();
        Log("Missions: restart declined");
        return;
    }
    if (!KeyPressed(g_cfg.restartYesKey, g_yesDown)) return;
    EndPrompt();
    unsigned char* ped = FindPlayerPed();
    if (ped && !PlayerBusy() && StartRestartScript(ped))
        Log(g_start.rampage ? "Missions: player put back at the rampage pickup to restart it"
                            : "Missions: player put back at the mission's marker to restart it");
}

// GET_CHAR_COORDINATES (00A0, handler at 0x449B8D) of a character that no longer
// exists. Scripts keep handles of street peds they picked (vigilante criminals);
// the game can remove such a ped (far away, a fast flight) and the command then
// read from null (CPools::GetPedAt 0x451CB0 returns 0): "Unhandled exception" at
// 0x449BAD. The handler reads only +0x3AC (in a vehicle), +0x3A8 (the vehicle) and
// the position (+0x34); a missing character now reads as a blank one at 0,0,0, on
// foot, and the script's own IS_CHAR_DEAD checks (a missing character counts as
// dead) end its business with it as usual.
extern "C" {
    alignas(16) unsigned char g_noChar[0x400] = {};   // stands in for a missing character
    volatile LONG g_noCharReads = 0;
}

static void InstallMissingCharGuard() {
    // cmp byte [eax+3ACh],0 right after the GetPedAt call
    static const unsigned char site[] = { 0x80, 0xB8, 0xAC, 0x03, 0x00, 0x00, 0x00 };
    if (memcmp((void*)0x449BAD, site, sizeof site) || *(const unsigned char*)0x449BA8 != 0xE8 ||
        0x449BAD + *(const int*)0x449BA9 != 0x451CB0) {
        Log("Missions: GET_CHAR_COORDINATES code is not v1.0; no guard for missing characters");
        return;
    }
    unsigned char* code = (unsigned char*)VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!code) return;
    unsigned char stub[] = {
        0x85, 0xC0,                               // test eax, eax
        0x75, 0x0B,                               // jnz have
        0xB8, 0, 0, 0, 0,                         // mov eax, g_noChar
        0xF0, 0xFF, 0x05, 0, 0, 0, 0,             // lock inc dword [g_noCharReads]
        0x80, 0xB8, 0xAC, 0x03, 0x00, 0x00, 0x00, // have: cmp byte [eax+3ACh], 0
        0xE9, 0, 0, 0, 0,                         // jmp 0x449BB4 (je with these flags)
    };
    // jnz must skip the mov (5) and the lock inc (7)
    stub[3] = 12;
    *(DWORD*)(stub + 5) = (DWORD)g_noChar;
    *(DWORD*)(stub + 12) = (DWORD)&g_noCharReads;
    memcpy(code, stub, sizeof stub);
    *(int*)(code + sizeof stub - 4) = (int)(0x449BB4 - ((DWORD)code + sizeof stub));
    unsigned char patch[7] = { 0xE9, 0, 0, 0, 0, 0x90, 0x90 };
    *(int*)(patch + 1) = (int)((DWORD)code - (0x449BAD + 5));
    DWORD old;
    VirtualProtect((void*)0x449BAD, 7, PAGE_EXECUTE_READWRITE, &old);
    memcpy((void*)0x449BAD, patch, 7);
    VirtualProtect((void*)0x449BAD, 7, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)0x449BAD, 7);
}

static bool g_fromTick = false;
static void Run() {
    static LONG reported = 0;
    if (g_noCharReads != reported) {
        reported = g_noCharReads;
        Log("Missions: a script asked where a character that no longer exists is (%ld so far); no crash", reported);
    }
    if (!g_ok) return;
    IntroSkip();
    StuckMissions();
    RestartPrompt();
}
void MissionHelpBeforeTick() { g_fromTick = true; Run(); }
void MissionHelpAfterPresent() { if (!g_fromTick) Run(); }   // FixedTick off: once per frame

void InstallMissionHelp() {
    InstallMissingCharGuard();   // a crash fix: always
    // Installed even when every part is off: the Modern Fix menu (fixmenu.cpp) turns them
    // on and off while playing, and each part checks its setting on every update.
    // DoDeatharrestCheck: push ebx; mov ebx,ecx; cmp byte [ebx+83h],0
    static const unsigned char deatharrest[] = { 0x53, 0x89, 0xCB, 0x80, 0xBB, 0x83, 0x00, 0x00, 0x00, 0x00 };
    // SetWideScreenOn: mov byte [ecx+6Dh],1; ret
    static const unsigned char wideOn[] = { 0xC6, 0x41, 0x6D, 0x01, 0xC3 };
    // CRunningScript::Process: wake time check, cmp [ebx+7Ch]
    static const unsigned char wake[] = { 0x8B, 0x43, 0x7C, 0x8B, 0x15, 0x2C, 0x4B, 0x97, 0x00 };
    // SET_PLAYER_CONTROL off branch: mov ecx,ebx; push 1; call MakePlayerSafe (checks its address)
    static const unsigned char safe[] = { 0x89, 0xD9, 0x6A, 0x01, 0xE8 };
    if (memcmp((void*)0x460D00, deatharrest, sizeof deatharrest) || memcmp((void*)0x46B875, wideOn, sizeof wideOn) ||
        memcmp((void*)0x44FDA6, wake, sizeof wake) || memcmp((void*)0x452300, safe, sizeof safe) ||
        0x452305 + *(const int*)0x452305 + 4 != 0x4BBC10) {
        Log("Missions: script code is not v1.0; intro skip and stuck-mission help not installed");
        return;
    }
    g_ok = true;
    // CDarkel::ResetOnPlayerDeath: cmp word [CDarkel::Status], 1 (rampage running)
    static const unsigned char darkel[] = { 0x66, 0x83, 0x3D, 0x72, 0x0A, 0xA1, 0x00, 0x01 };
    g_frenzyOk = !memcmp((void*)0x429F90, darkel, sizeof darkel);
    if (!g_frenzyOk) Log("Missions: rampage code is not v1.0; rampages can't be ended or restarted");
    Log("Missions: hold key %d for 3 s to skip a mission's scripted scene%s (%s)", g_cfg.introSkipKey,
        MissionAudioSkip(false) ? "" : " (speech hooks missing: speech isn't skipped)", g_cfg.introSkip ? "on" : "off");
    if (g_cfg.softlockSeconds > 0) Log("Missions: a mission with no player control for %d s is ended", g_cfg.softlockSeconds);
    if (g_cfg.endMissionKey) Log("Missions: hold key %d for 3 s to end the current mission", g_cfg.endMissionKey);
    g_restartOk = ScriptHookInstalled();
    if (!g_restartOk) Log("Missions: script hook not installed; no restart prompt");
    else Log("Missions: failed missions offer a restart (keys %d / %d) (%s)", g_cfg.restartYesKey, g_cfg.restartNoKey,
             g_cfg.missionRestart ? "on" : "off");
}
