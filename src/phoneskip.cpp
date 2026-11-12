// Enter skips the phone call you are on.
//
// Answering the mobile (Tab) puts the player in PED_ANSWER_MOBILE (state 36,
// CPed::SetAnswerMobile 0x4F59C0, script opcode in 0x62EFCA). The call itself is
// the mission script playing one line after another: LOAD_MISSION_AUDIO, wait for
// HAS_MISSION_AUDIO_LOADED, PRINT_NOW the subtitle, PLAY_MISSION_AUDIO (0x45ABD7),
// then wait until HAS_MISSION_AUDIO_FINISHED (0x45AC04). When the last line is
// done the script hangs up (state back to normal) and carries on.
//
// Pressing Enter during a call: the line being spoken is stopped
// (DMAudio::ClearMissionAudio 0x5F9770, both slots), and until the call ends every
// remaining line is not played and reports "finished" straight away, so the
// script runs through the rest of the call in a moment and hangs up as usual -
// whatever it sets up afterwards (new contact, mission marker) still happens.
// Subtitles are cleared while skipping (CMessages::ClearMessages 0x5841E0).
#include "phoneskip.h"
#include "config.h"
#include <windows.h>
#include <string.h>

static const DWORD SITE_PLAY = 0x45ABD7, FN_PLAY = 0x5F97B0;           // DMAudio::PlayLoadedMissionAudio(slot)
static const DWORD SITE_FINISHED = 0x45AC04, FN_FINISHED = 0x5F9790;   // DMAudio::IsMissionAudioSampleFinished(slot)
static const auto ClearMissionAudio = (void (__thiscall*)(void*, unsigned char))0x5F9770;
static const auto ClearMessages = (void (__cdecl*)())0x5841E0;
static const auto FindPlayerPed = (unsigned char* (__cdecl*)())0x4BC120;
static void* const DMAudio = (void*)0xA10B8A;
static const DWORD PED_STATE = 0x244, PED_ANSWER_MOBILE = 36;
static const DWORD GAME_STATE = 0x9B5F08, MENU_ACTIVE = 0x869668, GAME_WINDOW = 0x7897A4, CLOCK = 0x974B2C;

static DWORD g_play = FN_PLAY, g_finished = FN_FINISHED;
static bool g_skipping = false, g_down = false, g_press = false;
static DWORD g_skipStart = 0;
static unsigned g_skips = 0;

static void __fastcall HookPlay(void* audio, void*, unsigned char slot) {
    if (g_skipping) return;   // skipped line: not played
    ((void (__thiscall*)(void*, unsigned char))g_play)(audio, slot);
}

static bool __fastcall HookFinished(void* audio, void*, unsigned char slot) {
    if (g_skipping) return true;
    return ((bool (__thiscall*)(void*, unsigned char))g_finished)(audio, slot);
}

static bool InCall(unsigned char* ped) {
    return ped && *(int*)(ped + PED_STATE) == (int)PED_ANSWER_MOBILE;
}

void PhoneSkipFrame() {
    if (!g_cfg.phoneSkip) return;
    const bool down = (GetAsyncKeyState(g_cfg.phoneSkipKey) & 0x8000) != 0;
    const bool mod = ModifierDown();   // Alt+Enter and the like aren't a skip
    if (down && !g_down && !mod) g_press = true;
    if (mod) g_press = false;
    g_down = down;
}

void PhoneSkipBeforeTick() {
    if (!g_cfg.phoneSkip) return;
    const bool press = g_press;
    g_press = false;
    const bool playing = *(int*)GAME_STATE == 9 && !*(unsigned char*)MENU_ACTIVE;
    unsigned char* ped = playing ? FindPlayerPed() : nullptr;
    const bool inCall = InCall(ped);
    const DWORD now = *(DWORD*)CLOCK;

    if (press && inCall && !g_skipping) {
        HWND w = *(HWND*)GAME_WINDOW;
        if (w && GetForegroundWindow() == w) {
            g_skipping = true;
            g_skipStart = now;
            ClearMissionAudio(DMAudio, 0);
            ClearMissionAudio(DMAudio, 1);
            ClearMessages();
            if (g_skips++ < 20) Log("PhoneSkip: phone call skipped");
        }
    }
    if (!g_skipping) return;
    if (!playing) return;   // menu open: wait
    if (!inCall || now - g_skipStart > 30000) {   // hung up (or a call that never ends)
        ClearMessages();
        g_skipping = false;
        return;
    }
    ClearMessages();
}

static DWORD CallTarget(DWORD site) {
    const unsigned char* p = (const unsigned char*)site;
    return p[0] == 0xE8 ? site + 5 + *(const int*)(p + 1) : 0;
}

static void Redirect(DWORD site, void* target) {
    unsigned char call[5] = { 0xE8 };
    *(int*)(call + 1) = (int)((DWORD)target - (site + 5));
    DWORD old;
    VirtualProtect((void*)site, 5, PAGE_EXECUTE_READWRITE, &old);
    memcpy((void*)site, call, 5);
    VirtualProtect((void*)site, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)site, 5);
}

void InstallPhoneSkip() {
    if (!g_cfg.phoneSkip) { Log("PhoneSkip: off"); return; }
    // CPed::SetAnswerMobile: push ebx; mov ebx,ecx; mov eax,[ebx+244h]; cmp eax,24h
    static const unsigned char answer[] = { 0x53, 0x89, 0xCB, 0x8B, 0x83, 0x44, 0x02, 0x00, 0x00, 0x83, 0xF8, 0x24 };
    const DWORD play = CallTarget(SITE_PLAY), finished = CallTarget(SITE_FINISHED);
    if (!play || !finished || memcmp((void*)0x4F59C0, answer, sizeof answer)) {
        g_cfg.phoneSkip = false;
        Log("PhoneSkip: phone / mission audio code is not the v1.0 code; not installed");
        return;
    }
    if (play != FN_PLAY || finished != FN_FINISHED)
        Log("PhoneSkip: mission audio calls hooked by another mod (%08lX %08lX); chaining to them", play, finished);
    g_play = play;
    g_finished = finished;
    Redirect(SITE_PLAY, (void*)&HookPlay);
    Redirect(SITE_FINISHED, (void*)&HookFinished);
    Log("PhoneSkip: installed, key %d (virtual-key code)", g_cfg.phoneSkipKey);
}
