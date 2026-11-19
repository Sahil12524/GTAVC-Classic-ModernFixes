// Hunter weapons: the mouse wheel or Tab picks rockets or the cannon, and one key (Space)
// fires the one picked, also with Shift / Ctrl held (with modern keys they aren't
// shortcut keys: a trainer's boost may be on Ctrl); the game's own fire keys keep working. A help message
// names the weapon when you get in and each time it changes.
//
// The Hunter's weapons are in CAutomobile::ProcessControl, for model 155 driven by
// the player (0x597F44):
//   rockets: if CPad 0x4AA9F0 (called at 0x597F6F) and 350 ms since the last shot
//   cannon:  else if CPad 0x4AA9B0 (called at 0x5981EA) and 60 ms since the last
// Both calls are redirected to functions that also answer yes while the fire key
// is held and that weapon is picked. The helicopter code reads the handbrake
// (CPad 0x4AAB60, called at 0x5B7049), which Space is by default; it reads no
// while the fire key fires, so firing doesn't also brake.
//
// The wheel: CPad::UpdateMouse takes DirectInput's wheel movement at 0x4AD934
// (mov eax,[esp+14h]; test eax,eax) and sets the wheel up / down buttons from it,
// which the radio (and anything else bound to the wheel) reads. That is a call
// into a stub that, while the player is in the Hunter, adds the movement to
// g_wheelDelta and gives the game 0 - so the radio doesn't change - and the next
// update switches the weapon.
#include "hunter.h"
#include "config.h"
#include "driveby.h"
#include "mouselook.h"
#include <windows.h>
#include <string.h>

static const DWORD PAD0 = 0x7DBCB0, PAD_DISABLE = 0xF0, GAME_WINDOW = 0x7897A4;
static const DWORD ENT_MODEL = 0x5C, ENT_STATUS = 0x50, VEH_TYPE = 0x29C;
static const DWORD CUTSCENE_RUNNING = 0xA10AB2, MENU_ACTIVE = 0x869668, USER_PAUSE = 0xA10B36;
static const short MODEL_HUNTER = 155;
static const DWORD ROCKET_SITE = 0x597F6F, ROCKET_FN = 0x4AA9F0;
static const DWORD CANNON_SITE = 0x5981EA, CANNON_FN = 0x4AA9B0;
static const DWORD BRAKE_SITE = 0x5B7049, BRAKE_FN = 0x4AAB60;
static const DWORD WHEEL_SITE = 0x4AD934;

typedef unsigned char* (__cdecl* Find_t)();
static const Find_t FindPlayerVehicle = (Find_t)0x4BC1E0;
static const auto SetHelpMessage = (void (__cdecl*)(const wchar_t*, bool, bool))0x55BFC0;

extern "C" {
    volatile unsigned char g_wheelEat = 0;   // the stub keeps the wheel from the game
    volatile int g_wheelDelta = 0;           // wheel movement it kept
}

static bool g_on = false, g_fromTick = false, g_inHunter = false;
static bool g_rockets = true;               // the weapon picked
static DWORD g_rocketFn = ROCKET_FN, g_cannonFn = CANNON_FN, g_brakeFn = BRAKE_FN;
static unsigned char* g_lastVeh = nullptr;

// A trainer window (Modern VC Trainer) open over the game: while its menu is open
// it blanks the game's raw mouse state (CPad::PCTempMouseControllerState,
// 0x7DD860, all 16 bytes) after every pad update. Byte 0x7DD867 is padding the
// game never writes (7 button bytes, then the floats at +8), so it is set before
// the pad update (CGame::Process -> CPad::UpdatePads, call at 0x4A4412, which the
// trainer hooks - VCModernFix's hook is put on top and calls it) and found blanked
// afterwards only when such a window took the input.
static const DWORD PADS_SITE = 0x4A4412, PADS_FN = 0x4AB6C0;
static volatile unsigned char* const kMouseSpare = (unsigned char*)0x7DD867;
static DWORD g_padsFn = PADS_FN;
static bool g_overlayOpen = false;

static void __cdecl HookUpdatePads() {
    *kMouseSpare = 0xA5;
    ((void(__cdecl*)())g_padsFn)();
    g_overlayOpen = *kMouseSpare != 0xA5;
    *kMouseSpare = 0;
}

bool TrainerWindowOpen() { return g_overlayOpen; }   // mouselook.h

// The trainer window's own mouse cursor. The trainer moves it in its pad-update hook
// (by the update's mouse movement, then it blanks the game's pads and mouse), so it
// moved 30 times a second, once per game update, however fast the window itself was
// drawn: the cursor, dragging, sliders and scrolling felt like 30 fps. While the
// window is open, FixedTick's redraw frames run that pad update too (the game's,
// with the trainer's hook in front of it), so the cursor gets the mouse movement on
// every frame. The game sees no input from it: the trainer blanks the pads after
// reading them, as on every update while its window is open.
static bool g_padsHooked = false;
void TrainerWindowFrame() {
    if (g_padsHooked && g_overlayOpen) HookUpdatePads();
}

// A modifier held: Alt or a Windows key, and Shift or Ctrl unless modern keys are on
// (then they may be held while flying and firing: Shift sprints, a trainer boosts on Ctrl).
static bool HunterModifierDown() {
    if (!g_cfg.modernKeys) return ModifierDown();
    static const int keys[] = { VK_MENU, VK_LWIN, VK_RWIN };
    for (int k : keys) if (GetAsyncKeyState(k) & 0x8000) return true;
    return false;
}

// Game window in front, no game menu, no trainer window, no modifier held.
static bool InputFree() {
    HWND w = *(HWND*)GAME_WINDOW;
    return !g_overlayOpen && !*(volatile unsigned char*)MENU_ACTIVE && !HunterModifierDown() && w && GetForegroundWindow() == w;
}

// The fire keys: the fire key (Space) fires the picked weapon, the left mouse
// button the rockets, the right one the cannon (the game's mouse state,
// CPad::NewMouseControllerState 0x94D788 LMB / 0x94D789 RMB, read after the pad
// update, so a trainer window that takes the mouse blanks it). While any of them
// is held, they alone decide: the game's own fire inputs (which may be on the same
// keys) are not asked. Otherwise (gamepad, the game's keys) the game's are.
static const DWORD MOUSE_LMB = 0x94D788, MOUSE_RMB = 0x94D789;

static bool OurFire(bool* rockets, bool* cannon) {
    if (!g_inHunter || !InputFree()) return false;
    const bool key = (GetAsyncKeyState(g_cfg.hunterFireKey) & 0x8000) != 0;
    const bool lmb = *(volatile unsigned char*)MOUSE_LMB != 0, rmb = *(volatile unsigned char*)MOUSE_RMB != 0;
    if (!key && !lmb && !rmb) return false;
    *rockets = lmb || (key && g_rockets);
    *cannon = rmb || (key && !g_rockets);
    return true;
}

static bool FireHeld() {
    bool r, c;
    return OurFire(&r, &c);
}

// A short record of what the Hunter's fire reads answered and why (each change,
// at most 60 lines a session), to tell from VCModernFix.log what fired.
static unsigned g_diagLines = 0;
static void Diag(const char* what, bool on, bool ours) {
    if (!g_inHunter || g_diagLines >= 60) return;
    const bool key = (GetAsyncKeyState(g_cfg.hunterFireKey) & 0x8000) != 0;
    g_diagLines++;
    Log("Hunter: %s %s (%s; key %d LMB %d RMB %d, picked %s, window free %d)", what, on ? "on" : "off",
        ours ? "VCModernFix keys" : "game's own input", key, *(volatile unsigned char*)MOUSE_LMB != 0,
        *(volatile unsigned char*)MOUSE_RMB != 0, g_rockets ? "rockets" : "cannon", InputFree());
}

static bool __fastcall HookRocket(void* pad, void*) {
    bool r, c, v;
    const bool ours = OurFire(&r, &c);
    v = ours ? r : ((bool (__thiscall*)(void*))g_rocketFn)(pad);
    static bool last = false;
    if (v != last) { last = v; Diag("rocket trigger", v, ours); }
    return v;
}

static short __fastcall HookCannon(void* pad, void*) {
    bool r, c;
    const bool ours = OurFire(&r, &c);
    const short v = ours ? (c ? 255 : 0) : ((short (__thiscall*)(void*))g_cannonFn)(pad);
    static bool last = false;
    if ((v != 0) != last) { last = v != 0; Diag("cannon trigger", last, ours); }
    return v;
}

// The Hunter's cannon sound (cAudioManager, 0x5EF3F6) reads the cannon input on
// its own: it gets the same answer, so it sounds only when the cannon fires.
static DWORD g_cannonSoundFn = CANNON_FN;
static short __fastcall HookCannonSound(void* pad, void*) {
    bool r, c;
    if (OurFire(&r, &c)) return c ? 255 : 0;
    return ((short (__thiscall*)(void*))g_cannonSoundFn)(pad);
}

// One rocket per shot. The game's Hunter shot launches two at once, one from each
// pod: two calls to CWeapon::FireProjectile (0x5CCF90, thiscall: shooter, start,
// power; ret 0Ch) at 0x5980B0 (left) and 0x5981D0 (right). For the player's
// Hunter only one goes, the pods taking turns.
static const DWORD SHOT_L_SITE = 0x5980B0, SHOT_R_SITE = 0x5981D0, FIRE_PROJECTILE = 0x5CCF90;
static DWORD g_shotLFn = FIRE_PROJECTILE, g_shotRFn = FIRE_PROJECTILE;
static bool g_rightNext = false;
typedef bool (__thiscall* Shot_t)(void*, void*, void*, float);

static bool __fastcall HookShotLeft(void* weapon, void*, void* shooter, void* start, float power) {
    if (g_inHunter && g_rightNext) return true;   // this shot is the right pod's
    if (g_inHunter && g_diagLines < 60) { g_diagLines++; Log("Hunter: rocket launched (left pod)"); }
    return ((Shot_t)g_shotLFn)(weapon, shooter, start, power);
}

static bool __fastcall HookShotRight(void* weapon, void*, void* shooter, void* start, float power) {
    if (!g_inHunter) return ((Shot_t)g_shotRFn)(weapon, shooter, start, power);
    const bool fire = g_rightNext;
    g_rightNext = !g_rightNext;   // the next shot from the other pod
    if (fire && g_diagLines < 60) { g_diagLines++; Log("Hunter: rocket launched (right pod)"); }
    return fire ? ((Shot_t)g_shotRFn)(weapon, shooter, start, power) : true;
}

static bool __fastcall HookBrake(void* pad, void*) {
    if (FireHeld()) return false;   // Space fires, it doesn't brake
    return ((bool (__thiscall*)(void*))g_brakeFn)(pad);
}

// Num+ switches the weapon too. In the game Num+ is bound to the vehicle side
// mission (action 20: vigilante in the Hunter), and with modern keys as its second
// key (Caps Lock first). While the player is in the Hunter, every keyboard binding
// of Num+ (key 0x401 in ControlsManager's table, 0x9B491C + 0xFF8: [action * 4 +
// type] { key, order }, types 0 and 1 the keyboard) is taken off, so the game sees
// nothing from it and it only switches; it is put back on getting out and whenever
// the game's menu is open (the controls menu shows and saves the bindings as they
// are). Caps Lock still starts and stops the side mission.
struct Bind { int key, order; };
static Bind* const Bindings = (Bind*)(0x9B491C + 0xFF8);
static const int kNumPlus = 0x401, kNoKey = 0x420, kActions = 44;
static int g_plusIdx[16];
static Bind g_plusSaved[16];
static int g_plusN = 0;
static bool g_plusTaken = false;

static void TakeNumPlus() {
    if (g_plusTaken) return;
    g_plusTaken = true;
    g_plusN = 0;
    for (int a = 0; a < kActions; a++)
        for (int t = 0; t < 2; t++) {
            const int i = a * 4 + t;
            if (Bindings[i].key == kNumPlus && g_plusN < 16) {
                g_plusIdx[g_plusN] = i;
                g_plusSaved[g_plusN++] = Bindings[i];
                Bindings[i] = { kNoKey, 0 };
            }
        }
    static int logs = 0;
    if (g_plusN && logs++ < 3) Log("Hunter: Num+ switches the weapon; its %d game binding(s) held off while in the Hunter", g_plusN);
}

static void GiveBackNumPlus() {
    if (!g_plusTaken) return;
    for (int k = 0; k < g_plusN; k++)
        if (Bindings[g_plusIdx[k]].key == kNoKey) Bindings[g_plusIdx[k]] = g_plusSaved[k];   // unless something set it meanwhile
    g_plusN = 0;
    g_plusTaken = false;
}

static void Announce() {
    static const wchar_t rockets[] = L"Hunter: ROCKETS selected. Mouse wheel, Tab or Num+ switches, Space fires it. LMB rockets, RMB cannon.";
    static const wchar_t cannon[] = L"Hunter: CANNON selected. Mouse wheel, Tab or Num+ switches, Space fires it. LMB rockets, RMB cannon.";
    SetHelpMessage(g_rockets ? rockets : cannon, true, false);
}

static void Run() {
    bool in = false;
    if (g_on && g_cfg.hunterWeapons && !ReplayPlaying() && !*(volatile unsigned char*)MENU_ACTIVE && !*(volatile unsigned char*)USER_PAUSE &&
        !*(volatile unsigned char*)CUTSCENE_RUNNING && !*(unsigned short*)(PAD0 + PAD_DISABLE)) {
        unsigned char* veh = FindPlayerVehicle();
        in = veh && *(int*)(veh + VEH_TYPE) == 0 && *(short*)(veh + ENT_MODEL) == MODEL_HUNTER &&
             (*(veh + ENT_STATUS) >> 3) == 0;
        if (in && veh != g_lastVeh) Announce();   // just got in
        g_lastVeh = in ? veh : nullptr;
    }
    const int delta = g_wheelDelta;
    g_wheelDelta = 0;
    const bool free = InputFree();
    static bool tabWas = false, plusWas = false;   // Tab and Num+ switch too, once per press
    const bool tab = (GetAsyncKeyState(VK_TAB) & 0x8000) != 0;
    const bool tabPress = tab && !tabWas;
    tabWas = tab;
    const bool plus = (GetAsyncKeyState(VK_ADD) & 0x8000) != 0;
    const bool plusPress = plus && !plusWas;
    plusWas = plus;
    if (in) TakeNumPlus(); else GiveBackNumPlus();
    if (in && g_inHunter && (delta || tabPress || plusPress) && free) {
        g_rockets = !g_rockets;   // two weapons: either way switches
        Announce();
    }
    // Drive-by mode (driveby.cpp): the wheel switches guns instead of the radio.
    const bool dbWheel = !in && DriveByWantsWheel();
    if (dbWheel && delta && free) DriveByWheel(delta);
    g_inHunter = in;
    g_wheelEat = (in || dbWheel) && free;   // otherwise the wheel goes to the game / the trainer window
}

void HunterBeforeTick() { g_fromTick = true; Run(); }
void HunterAfterPresent() {
    if (*(volatile unsigned char*)MENU_ACTIVE) GiveBackNumPlus();   // the menu shows the bindings as they are
    if (!g_fromTick) Run();   // FixedTick off: once per frame
}

static void WriteCode(DWORD addr, const void* bytes, int len) {
    DWORD old;
    VirtualProtect((void*)addr, len, PAGE_EXECUTE_READWRITE, &old);
    memcpy((void*)addr, bytes, len);
    VirtualProtect((void*)addr, len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)addr, len);
}

static DWORD CallTarget(DWORD site) {
    const unsigned char* p = (const unsigned char*)site;
    return p[0] == 0xE8 ? site + 5 + *(const int*)(p + 1) : 0;
}

static void RedirectCall(DWORD site, void* target) {
    unsigned char call[5] = { 0xE8 };
    *(int*)(call + 1) = (int)((DWORD)target - (site + 5));
    WriteCode(site, call, 5);
}

// The wheel stub and the pad-update hook (trainer window detection), also used by
// drive-by mode to switch guns.
static bool InstallWheel() {
    static const unsigned char wheel[] = { 0x8B, 0x44, 0x24, 0x14, 0x85, 0xC0 };   // mov eax,[esp+14h]; test eax,eax
    if (memcmp((void*)WHEEL_SITE, wheel, sizeof wheel)) return false;
    unsigned char* code = (unsigned char*)VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!code) { Log("Hunter: VirtualAlloc failed"); return false; }
    unsigned char stub[] = {
        0x8B, 0x44, 0x24, 0x18,                     // mov eax, [esp+18h]   the wheel ([esp+14h] before the call)
        0x80, 0x3D, 0, 0, 0, 0, 0x00,               // cmp byte [g_wheelEat], 0
        0x74, 0x08,                                 // je done
        0x01, 0x05, 0, 0, 0, 0,                     // add [g_wheelDelta], eax
        0x31, 0xC0,                                 // xor eax, eax          the game sees no wheel
        0x85, 0xC0,                                 // done: test eax, eax
        0xC3,                                       // ret (flags kept for the jle after)
    };
    *(DWORD*)(stub + 6) = (DWORD)&g_wheelEat;
    *(DWORD*)(stub + 15) = (DWORD)&g_wheelDelta;
    memcpy(code, stub, sizeof stub);
    unsigned char call[6] = { 0xE8, 0, 0, 0, 0, 0x90 };
    *(int*)(call + 1) = (int)((DWORD)code - (WHEEL_SITE + 5));
    WriteCode(WHEEL_SITE, call, sizeof call);

    if (DWORD t = CallTarget(PADS_SITE)) {   // the game's UpdatePads, or the trainer's hook on it
        g_padsFn = t;
        RedirectCall(PADS_SITE, (void*)&HookUpdatePads);
        g_padsHooked = true;
    } else {
        Log("Hunter: pad update call replaced by another mod; trainer window not detected");
    }
    return true;
}

// Installed even when off: the Modern Fix menu (fixmenu.cpp) turns it on and off while
// playing; off, the player is never "in the Hunter" here and every hook asks the game.
void InstallHunter() {
    // Each call must still be a call; another mod's hook on it is chained to.
    g_rocketFn = CallTarget(ROCKET_SITE);
    g_cannonFn = CallTarget(CANNON_SITE);
    g_brakeFn = CallTarget(BRAKE_SITE);
    static const unsigned char wheel[] = { 0x8B, 0x44, 0x24, 0x14, 0x85, 0xC0 };   // mov eax,[esp+14h]; test eax,eax
    static const unsigned char hunterCheck[] = { 0x81, 0xF9, 0x9B, 0x00, 0x00, 0x00 };   // cmp ecx, 155 (0x597F48)
    if (!g_rocketFn || !g_cannonFn || !g_brakeFn || memcmp((void*)WHEEL_SITE, wheel, sizeof wheel) ||
        memcmp((void*)0x597F48, hunterCheck, sizeof hunterCheck)) {
        Log("Hunter: game code is not v1.0 or was changed by another mod; weapon keys as in the game");
        InstallWheel();   // still for drive-by guns and the trainer window
        return;
    }
    if (g_rocketFn != ROCKET_FN || g_cannonFn != CANNON_FN || g_brakeFn != BRAKE_FN)
        Log("Hunter: a fire or handbrake call is hooked by another mod; chaining to it");

    if (!InstallWheel()) return;
    const DWORD shotL = CallTarget(SHOT_L_SITE), shotR = CallTarget(SHOT_R_SITE);
    if (shotL && shotR) {
        g_shotLFn = shotL;
        g_shotRFn = shotR;
        RedirectCall(SHOT_L_SITE, (void*)&HookShotLeft);
        RedirectCall(SHOT_R_SITE, (void*)&HookShotRight);
    } else {
        Log("Hunter: rocket launch calls replaced by another mod; two rockets per shot");
    }
    static const unsigned char soundCheck[] = { 0x81, 0xF9, 0x9B, 0x00, 0x00, 0x00 };   // cmp ecx, 155 (0x5EF3E2)
    if (!memcmp((void*)0x5EF3E2, soundCheck, sizeof soundCheck) && CallTarget(0x5EF3F6)) {
        g_cannonSoundFn = CallTarget(0x5EF3F6);
        RedirectCall(0x5EF3F6, (void*)&HookCannonSound);
    }
    RedirectCall(ROCKET_SITE, (void*)&HookRocket);
    RedirectCall(CANNON_SITE, (void*)&HookCannon);
    RedirectCall(BRAKE_SITE, (void*)&HookBrake);
    g_on = true;
    Log("Hunter: mouse wheel picks rockets / cannon, key %d fires it (%s)", g_cfg.hunterFireKey,
        g_cfg.hunterWeapons ? "on" : "off");
}
