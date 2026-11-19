// Free drive-by ([Weapons] FreeDriveBy, DriveByKey): in any car, bike or boat you
// drive, press Alt to take a gun out of the window, aim with the mouse (the shot
// goes where the camera looks, under the game's own crosshair), switch guns with
// the mouse wheel, and press Alt again to put it away.
//
// The game's drive-bys: CAutomobile (0x5C97B0), CBike (0x5C91E0) and CBoat
// (0x5C9540) each have a DoDriveByShootings, run every update for the player's
// vehicle. Each one:
//  1. returns unless the driver's weapon is in slot 5 (the SMGs: CWeaponInfo +0x60,
//     from GetWeaponInfo 0x5D5710);
//  2. works out "looking left" / "looking right" from the camera's look-left /
//     look-right state (Q / E), or the look keys in the top-down and cinematic
//     cameras;
//  3. only when one is set (bikes: or fire held, for a forward shot) plays the arm
//     out of the window (Driveby_L / _R, DrivebyL_ in low cars, BIKEx_drivebyLHS /
//     RHS / FT on bikes) and, with the in-car fire button (CPad 0x4AAA60) held and
//     the weapon's timer passed, fires: CWeapon::FireFromCar (0x5D44E0) ->
//     FireInstantHitFromCar (0x5CB0A0), which shoots from the window straight out
//     sideways (or forward), through DoDriveByAutoAiming (0x5CA400, call at
//     0x5CB960) and a line test that ignores the vehicle.
//
// Changed here:
//  1. The slot check also lets pistols (slot 3) and the M4 / Ruger (slot 6) through.
//     Getting into a vehicle (CPed::RemoveWeaponWhenEnteringVehicle 0x4FF6A0, three
//     calls) the game picks the SMG; now the gun in your hand is kept if it is a
//     pistol, SMG or rifle with ammo, else SMG, rifle, pistol. Getting out gives back
//     what you held before, as the game does.
//  2. Drive-by mode, toggled with the drive-by key (Alt; it switches on release, and
//     not when any other key was pressed while it was held, so Alt+Tab or a
//     trainer's Alt+Q / Alt+E doesn't; keys already held, such as W, don't count): while it is on,
//     the arm is out of the side the camera looks at (bikes: or forward, within 50
//     degrees of straight ahead) and fire shoots. Off, the game's own Q / E drive-by
//     is unchanged. It switches off when you leave the vehicle or have no gun left.
//  3. In drive-by mode the shot goes where the camera looks: from the camera along
//     its centre line a line test (CWorld::ProcessLineOfSight 0x4D92D0, the vehicle
//     ignored) finds what is under the screen centre, and the bullet goes from the
//     window to that point. FreeCam turns the camera with the mouse.
//  4. Pistols and rifles keep their own pace (the game fires every 70 ms), unless
//     another mod has shortened the game's 70 ms (a trainer's rapid fire): then the
//     game's shortened interval paces every gun.
//  5. In drive-by mode the mouse wheel switches between your pistol, SMG and rifle
//     (the wheel is taken from the radio by the Hunter's wheel hook, hunter.cpp).
//  6. The game's crosshair (HUD sprite siteM16, CHud::Sprites 0xA10744 + 64 * 4, the
//     size CHud::Draw gives it for SMGs and pistols: 32 * 0.4 at 640 x 448) is drawn
//     in the screen centre after the HUD (CHud::Draw 0x557320, call at 0x4A64D0),
//     with CSprite2d::Draw (0x5786A0).
// Not for vehicles that use the fire button for their own gun (Rhino, fire truck,
// Predator, Hunter, helicopters, planes) or while the game turns off drive-bys
// (CPlayerInfo +0x147, mission scripts). The game's camera modes where the view
// isn't yours (top-down, cinematic) keep the old drive-by.
#include "driveby.h"
#include "config.h"
#include <windows.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>

typedef unsigned char* (__cdecl* Find_t)();
static const Find_t FindPlayerVehicle = (Find_t)0x4BC1E0;
static const Find_t FindPlayerPed = (Find_t)0x4BC120;
static const auto GetPad = (unsigned char* (__cdecl*)(int))0x4AB060;
static const auto SetHelpMessage = (void (__cdecl*)(const wchar_t*, bool, bool))0x55BFC0;
static const DWORD FN_FIRED = 0x4AAA60, FN_LOOK_LEFT = 0x4AAC90, FN_LOOK_RIGHT = 0x4AAC60;
static const DWORD FN_INFO = 0x5D5710, FN_AUTOAIM = 0x5CA400, FN_LOS = 0x4D92D0;
static const DWORD FN_ENTER_WEAPON = 0x4FF6A0, FN_SET_WEAPON = 0x4FF8E0, FN_PLAYER_INFO = 0x531D40;
static const DWORD FN_HUD_DRAW = 0x557320, FN_SPRITE_DRAW = 0x5786A0, SPRITE_SITE_M16 = 0xA10844;
static const DWORD IGNORE_ENTITY = 0x9B6E58;                 // CWorld::pIgnoreEntity
static const DWORD CAM_FORWARD = 0x7E4698, CAM_POS = 0x7E46B8; // TheCamera matrix
static const DWORD CAM_ACTIVE = 0x7E46FE, CAM_CINEMATIC = 0x7E46E7;
static const DWORD CAMS = 0x7E4810, CAM_SIZE = 0x1CC;        // Cams[]: Mode +0x0C, LookingLeft +8, LookingRight +9
static const DWORD PAD0_DISABLE = 0x7DBCB0 + 0xF0, MENU_ACTIVE = 0x869668, GAME_WINDOW = 0x7897A4;
static const DWORD CLOCK = 0x974B2C, SCREEN_W = 0x9B48E4, SCREEN_H = 0x9B48E8;
static const DWORD VEH_DRIVER = 0x1A8, VEH_HANDLING = 0x120, ENT_MODEL = 0x5C;
static const DWORD PED_WEAPONS = 0x408, PED_SLOT = 0x504, PED_STORED = 0x4F8, PED_ATTACHED = 0x3D4;

static bool g_ok = false;
static bool g_mode = false;                 // drive-by mode: gun out, aimed with the camera
static unsigned char* g_modeVeh = nullptr;  // the vehicle it was switched on in
static DWORD g_lastRun = 0, g_nextShot = 0;
static unsigned g_shotLogs = 0, g_modeLogs = 0;
static unsigned char g_fakeSlot5[0x64];   // a weapon info that passes the slot check

static const float kForwardCos = 0.64f;   // bikes: within about 50 degrees of straight ahead fire forward
static const int kSlots[] = { 3, 5, 6 };  // pistol, SMG, rifle: the order the wheel goes through

static bool Excluded(const unsigned char* veh) {
    const short m = *(const short*)(veh + ENT_MODEL);
    if (m == 162 || m == 137 || m == 160 || m == 155) return true;   // Rhino, fire truck, Predator, Hunter
    const unsigned char* h = *(unsigned char* const*)(veh + VEH_HANDLING);
    return h && (*(const unsigned*)(h + 0xCC) & 0x60000);             // helicopter / plane handling
}

static bool CameraYours() {
    const int act = *(unsigned char*)CAM_ACTIVE;
    const short mode = *(short*)(CAMS + act * CAM_SIZE + 0x0C);
    return mode != 1 && !*(unsigned char*)CAM_CINEMATIC;   // not top-down or cinematic
}

// The player drives `veh` themselves, controls on, and free drive-by applies to it.
static bool FreeFor(const unsigned char* veh) {
    if (!g_ok || !veh || veh != FindPlayerVehicle()) return false;
    if (*(unsigned char* const*)(veh + VEH_DRIVER) != FindPlayerPed()) return false;
    if (*(unsigned short*)PAD0_DISABLE || *(unsigned char*)MENU_ACTIVE || ReplayPlaying()) return false;
    return !Excluded(veh);
}

// Drive-by mode is on for `veh` and the camera is the player's.
static bool Active(const unsigned char* veh) {
    return g_mode && veh == g_modeVeh && FreeFor(veh) && CameraYours();
}

static unsigned char* CurWeapon(unsigned char* ped) {
    return ped + PED_WEAPONS + *(signed char*)(ped + PED_SLOT) * 0x18;
}

static bool HasGun(unsigned char* ped, int slot) {
    const unsigned char* w = ped + PED_WEAPONS + slot * 0x18;
    return *(const int*)w != 0 && *(const int*)(w + 0x0C) > 0;
}

static bool DriveByAllowed(unsigned char* ped) {
    const unsigned char* pi = ((unsigned char* (__thiscall*)(void*))FN_PLAYER_INFO)(ped);
    return pi && pi[0x147];
}

static void SetWeapon(unsigned char* ped, int slot) {
    ((void (__thiscall*)(void*, int))FN_SET_WEAPON)(ped, *(int*)(ped + PED_WEAPONS + slot * 0x18));
}

// ---------------------------------------------------------------- side
// Replaces step 2 of the three DoDriveByShootings: al = left, ah = right.
extern "C" unsigned __cdecl vcmf_db_side(int bike, unsigned char* veh) {
    const int act = *(unsigned char*)CAM_ACTIVE;
    const unsigned char* cam = (const unsigned char*)(CAMS + act * CAM_SIZE);
    bool left, right;
    if (*(const short*)(cam + 0x0C) == 1 || *(unsigned char*)CAM_CINEMATIC) {
        unsigned char* pad = GetPad(0);
        left = ((bool (__thiscall*)(void*))FN_LOOK_LEFT)(pad);
        right = ((bool (__thiscall*)(void*))FN_LOOK_RIGHT)(pad);
    } else {
        left = cam[8] != 0;
        right = cam[9] != 0;
    }
    if (!Active(veh)) return left | (right << 8);
    g_lastRun = *(DWORD*)CLOCK;   // the gun is out right now
    if (left || right) return left | (right << 8);   // Q / E: the game's look, the shot follows the camera
    const float* r = (const float*)(veh + 0x04);
    const float* f = (const float*)(veh + 0x14);
    const float* c = (const float*)CAM_FORWARD;
    const float lx = c[0] * r[0] + c[1] * r[1] + c[2] * r[2];
    const float ly = c[0] * f[0] + c[1] * f[1] + c[2] * f[2];
    if (bike && ly > kForwardCos * sqrtf(lx * lx + ly * ly)) return 0;   // forward: the bike's own fire path
    return lx < 0.0f ? 1u : 0x100u;
}

// jmp here from the start of step 2 (movzx eax, byte [0x7E46FE]); back to its end.
#define SIDE_STUB(name, kind, vehreg, leftOfs, rightOfs, back) \
    ".globl _" name "\n_" name ":\n" \
    "  pushl %" vehreg "\n  pushl $" kind "\n  call _vcmf_db_side\n  addl $8, %esp\n" \
    "  movb %al, " leftOfs "(%esp)\n  movb %ah, " rightOfs "(%esp)\n" \
    "  pushl $" back "\n  ret\n"
extern "C" void vcmf_db_car(), vcmf_db_bike(), vcmf_db_boat();
__asm__(".text\n"
        SIDE_STUB("vcmf_db_car", "0", "esi", "0x8", "0x10", "0x5C9893")
        SIDE_STUB("vcmf_db_bike", "1", "ebp", "0x4", "0x0", "0x5C92CB")
        SIDE_STUB("vcmf_db_boat", "0", "edi", "0x8", "0xc", "0x5C9623"));
#undef SIDE_STUB

// ---------------------------------------------------------------- weapons
static bool SlotOk(int slot) { return slot == 3 || slot == 5 || slot == 6; }

static void* __cdecl InfoForSlotCheck(int type) {
    unsigned char* info = ((unsigned char* (__cdecl*)(int))FN_INFO)(type);
    if (info && g_cfg.freeDriveBy && SlotOk(*(int*)(info + 0x60))) return g_fakeSlot5;
    return info;
}

static void __fastcall EnterWeapon(unsigned char* ped, void*) {
    typedef void (__thiscall* Fn_t)(void*);
    if (!g_cfg.freeDriveBy || ped != FindPlayerPed() || *(int*)(ped + PED_ATTACHED) || !DriveByAllowed(ped)) { ((Fn_t)FN_ENTER_WEAPON)(ped); return; }
    const int cur = *(signed char*)(ped + PED_SLOT);
    int slot = -1;
    if (cur >= 0 && cur < 10 && SlotOk(cur) && HasGun(ped, cur)) slot = cur;   // the gun in your hand
    else if (HasGun(ped, 5)) slot = 5;
    else if (HasGun(ped, 6)) slot = 6;
    else if (HasGun(ped, 3)) slot = 3;
    if (slot < 0 || slot == 5) { ((Fn_t)FN_ENTER_WEAPON)(ped); return; }   // the game's SMG, or no gun
    if (*(int*)(ped + PED_STORED) == 45) *(int*)(ped + PED_STORED) = *(int*)CurWeapon(ped);
    SetWeapon(ped, slot);
}

static DWORD ShotGap(int type) {
    switch (type) {
    case 17: return 300;   // Colt 45
    case 18: return 700;   // Python
    case 26: return 110;   // M4
    case 27: return 130;   // Ruger
    default: return 0;     // the SMGs: the game's 70 ms
    }
}

// The game's own drive-by interval: the "add eax, 46h" byte after each FireFromCar
// (0x5C94A8 bike, 0x5C973E boat, 0x5C99EA car: timer = now + 70 ms). A trainer's
// rapid fire shortens it; then that interval paces every gun and the pistol / rifle
// pacing above is left out, so the other mod's rate is never capped here.
static const DWORD DRIVEBY_GAP_BYTE = 0x5C94A8;
static DWORD Gap(int type) {
    static int s_logged = -1;
    const int game = *(volatile unsigned char*)DRIVEBY_GAP_BYTE;
    if (game != s_logged) {
        if (game != 0x46)
            Log("DriveBy: shot interval set to %d ms by another mod; own pistol / rifle pacing off", game);
        else if (s_logged != -1)
            Log("DriveBy: shot interval back to the game's 70 ms; own pistol / rifle pacing on");
        s_logged = game;
    }
    return game != 0x46 ? (DWORD)game : ShotGap(type);
}

static bool __fastcall FiredGate(void* pad, void*) {
    if (!((bool (__thiscall*)(void*))FN_FIRED)(pad)) return false;
    if (!g_cfg.freeDriveBy) return true;
    return (int)(*(DWORD*)CLOCK - g_nextShot) >= 0;
}

// The mouse wheel in drive-by mode: the next (up) or previous (down) gun you have.
bool DriveByWantsWheel() {
    return g_mode && Active(FindPlayerVehicle());
}

void DriveByWheel(int delta) {
    if (!delta || !DriveByWantsWheel()) return;
    unsigned char* ped = FindPlayerPed();
    if (!ped) return;
    const int cur = *(signed char*)(ped + PED_SLOT);
    int at = 0;
    for (int i = 0; i < 3; i++) if (kSlots[i] == cur) at = i;
    for (int step = 1; step < 3; step++) {
        const int slot = kSlots[(at + (delta > 0 ? step : 3 - step)) % 3];
        if (!HasGun(ped, slot)) continue;
        if (*(int*)(ped + PED_STORED) == 45) *(int*)(ped + PED_STORED) = *(int*)CurWeapon(ped);
        SetWeapon(ped, slot);
        return;
    }
}

// ---------------------------------------------------------------- aim
static void __cdecl AimShot(unsigned char* ped, unsigned char* veh, float* src, float* tgt) {
    if (!g_cfg.freeDriveBy || !FreeFor(veh)) { ((void (__cdecl*)(void*, void*, float*, float*))FN_AUTOAIM)(ped, veh, src, tgt); return; }
    unsigned char* driver = *(unsigned char**)(veh + VEH_DRIVER);
    g_nextShot = *(DWORD*)CLOCK + Gap(*(int*)CurWeapon(driver));
    if (!Active(veh)) { ((void (__cdecl*)(void*, void*, float*, float*))FN_AUTOAIM)(ped, veh, src, tgt); return; }
    float dx = tgt[0] - src[0], dy = tgt[1] - src[1], dz = tgt[2] - src[2];
    const float range = sqrtf(dx * dx + dy * dy + dz * dz);
    if (range < 1.0f) return;
    const float* p = (const float*)CAM_POS;
    const float* c = (const float*)CAM_FORWARD;
    const float cl = sqrtf(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]);
    if (cl < 0.5f) return;
    const float fx = c[0] / cl, fy = c[1] / cl, fz = c[2] / cl;
    // Start the line where it passes the vehicle, so nothing between the camera and
    // the vehicle is in the way.
    const float* vp = (const float*)(veh + 0x34);
    float d0 = (vp[0] - p[0]) * fx + (vp[1] - p[1]) * fy + (vp[2] - p[2]) * fz;
    if (d0 < 0.0f) d0 = 0.0f;
    float start[3] = { p[0] + fx * d0, p[1] + fy * d0, p[2] + fz * d0 };
    float end[3] = { p[0] + fx * (d0 + range), p[1] + fy * (d0 + range), p[2] + fz * (d0 + range) };
    float aim[3] = { end[0], end[1], end[2] };
    alignas(16) unsigned char colPoint[0x40] = {};
    void* hit = nullptr;
    void* const ignored = *(void**)IGNORE_ENTITY;
    *(void**)IGNORE_ENTITY = veh;
    const bool found = ((bool (__cdecl*)(const float*, const float*, void*, void**, bool, bool, bool, bool, bool, bool, bool, bool))FN_LOS)(
        start, end, colPoint, &hit, true, true, true, true, true, false, false, true);
    *(void**)IGNORE_ENTITY = ignored;
    if (found) memcpy(aim, colPoint, sizeof aim);
    dx = aim[0] - src[0]; dy = aim[1] - src[1]; dz = aim[2] - src[2];
    const float l = sqrtf(dx * dx + dy * dy + dz * dz);
    if (l < 0.05f) return;
    // a little spread, as the game's own drive-by has
    const float s = range * 0.006f;
    for (int i = 0; i < 3; i++) {
        const float d = i == 0 ? dx : i == 1 ? dy : dz;
        tgt[i] = src[i] + d / l * range + ((rand() & 255) / 255.0f - 0.5f) * s;
    }
    if (g_shotLogs++ < 3)
        Log("DriveBy: free shot (weapon %d, aim %s %.1f m away)", *(int*)CurWeapon(driver), found ? "hit" : "open", l);
}

bool DriveByAiming() {
    return g_mode && Active(FindPlayerVehicle());
}

// ---------------------------------------------------------------- rampage kills
// A rampage (CDarkel, status 0xA10A72 = 1) counts a kill when CPed::InflictDamage
// passes CDarkel::RegisterKillByPlayer (0x429E90, cdecl ped, weapon type, headshot;
// calls at 0x526840 and 0x526B02) the rampage's weapon (CDarkel::WeaponType,
// 0xA10244): drive-by rampages want 42 (the car-shot type FireInstantHitFromCar
// gives, 0x5CBC92), Uzi rampages take 42 too. In drive-by mode, a kill by the player
// while a rampage runs is passed as the rampage's weapon when it is the drive-by
// type (42) or the gun in hand, so Alt drive-bys count the same as the game's own.
static const DWORD FRENZY_STATUS = 0xA10A72, FRENZY_WEAPON = 0xA10244, REGISTER_KILL = 0x429E90;
static const DWORD kKillSites[] = { 0x526840, 0x526B02 };
static DWORD g_killFn[2] = { REGISTER_KILL, REGISTER_KILL };
static unsigned g_killLogs = 0;
typedef void (__cdecl* Kill_t)(void*, int, bool);

static int RampageWeapon(int weapon) {
    if (*(volatile short*)FRENZY_STATUS != 1 || !g_mode) return weapon;
    unsigned char* veh = FindPlayerVehicle();
    unsigned char* ped = FindPlayerPed();
    if (!veh || veh != g_modeVeh || !ped) return weapon;
    const int want = *(volatile int*)FRENZY_WEAPON, held = *(int*)CurWeapon(ped);
    const int counted = (want == 42 || want == held) ? want : weapon;
    if (g_killLogs++ < 10)
        Log("DriveBy: rampage kill in drive-by mode (game's weapon %d, rampage wants %d, gun %d): counted as %d",
            weapon, want, held, counted);
    return counted;
}
static void __cdecl Kill0(void* p, int w, bool h) { ((Kill_t)g_killFn[0])(p, RampageWeapon(w), h); }
static void __cdecl Kill1(void* p, int w, bool h) { ((Kill_t)g_killFn[1])(p, RampageWeapon(w), h); }

// ---------------------------------------------------------------- mode key
static bool HasAnyGun(unsigned char* ped) {
    for (int s : kSlots) if (HasGun(ped, s)) return true;
    return false;
}

static void SetMode(bool on, unsigned char* veh, const char* why) {
    if (on == g_mode) return;
    g_mode = on;
    g_modeVeh = on ? veh : nullptr;
    if (g_modeLogs++ < 10) Log("DriveBy: mode %s (%s)", on ? "on" : "off", why);
}

// Each frame: the drive-by key switches on release, unless another key went down
// with it (Alt+Tab, Alt+Enter, Alt+F4).
static bool g_keyDown = false, g_keySpoiled = false;

// A key pressed while the drive-by key is held: an Alt shortcut (Alt+Tab, Alt+F4) or
// another program's Alt combination (a trainer's Alt+Q / Alt+E door locks). Keys
// already held when it went down (W, A, ... while driving) don't count; mouse
// buttons and the modifier keys themselves don't either.
static bool g_heldAtPress[256];
static bool Countable(int k) {
    return k > VK_XBUTTON2 && k != g_cfg.driveByKey && k != VK_MENU && k != VK_LMENU && k != VK_RMENU &&
           k != VK_SHIFT && k != VK_LSHIFT && k != VK_RSHIFT && k != VK_CONTROL && k != VK_LCONTROL && k != VK_RCONTROL;
}
static void NoteHeldKeys() {
    for (int k = 0; k < 256; k++) g_heldAtPress[k] = Countable(k) && (GetAsyncKeyState(k) & 0x8000);
}
static bool OtherKeyDown() {
    for (int k = 0; k < 256; k++) {
        if (!Countable(k)) continue;
        const bool held = (GetAsyncKeyState(k) & 0x8000) != 0;
        if (held && !g_heldAtPress[k]) return true;
        if (!held) g_heldAtPress[k] = false;   // let go and pressed again counts
    }
    return false;
}

static void KeyFrame() {
    unsigned char* veh = FindPlayerVehicle();
    unsigned char* ped = FindPlayerPed();
    // Leaving the vehicle, a mission turning drive-bys off, or no gun left: mode off.
    if (g_mode && (!veh || veh != g_modeVeh || !ped || !FreeFor(veh) || !DriveByAllowed(ped) || !HasAnyGun(ped)))
        SetMode(false, nullptr, "left the vehicle or no gun");
    HWND w = *(HWND*)GAME_WINDOW;
    const bool fg = w && GetForegroundWindow() == w;
    const bool down = fg && (GetAsyncKeyState(g_cfg.driveByKey) & 0x8000) != 0;
    if (down) {
        if (!g_keyDown) { g_keySpoiled = false; NoteHeldKeys(); }
        if (OtherKeyDown()) g_keySpoiled = true;
        g_keyDown = true;
        return;
    }
    if (!g_keyDown) return;
    g_keyDown = false;
    if (g_keySpoiled || !fg || *(unsigned char*)MENU_ACTIVE) return;
    static const wchar_t on[] = L"Drive-by: aim with the mouse, fire to shoot, mouse wheel switches guns. Alt puts the gun away.";
    static const wchar_t noGun[] = L"Drive-by: you have no pistol, SMG or rifle with ammo.";
    static const wchar_t off[] = L"Drive-by: gun put away.";
    if (g_mode) {
        SetMode(false, nullptr, "key");
        SetHelpMessage(off, true, false);
        return;
    }
    if (!veh || !ped || !FreeFor(veh) || !DriveByAllowed(ped)) return;   // not driving, or drive-bys off
    if (!HasAnyGun(ped)) { SetHelpMessage(noGun, true, false); return; }
    const int cur = *(signed char*)(ped + PED_SLOT);
    static const int kPrefer[] = { 5, 6, 3 };
    if (!(cur >= 0 && cur < 10 && SlotOk(cur) && HasGun(ped, cur)))   // got in with no gun out: take one
        for (int s : kPrefer) if (HasGun(ped, s)) {
            if (*(int*)(ped + PED_STORED) == 45) *(int*)(ped + PED_STORED) = *(int*)CurWeapon(ped);
            SetWeapon(ped, s);
            break;
        }
    SetMode(true, veh, "key");
    SetHelpMessage(on, true, false);
}

void DriveByAfterPresent() {
    if (!g_ok) return;
    if (!g_cfg.freeDriveBy) {   // turned off in the Modern Fix menu
        SetMode(false, nullptr, "turned off");
        g_keyDown = false;
        return;
    }
    KeyFrame();
}

// ---------------------------------------------------------------- crosshair
struct Rect { float left, bottom, right, top; };

// The HUD draw call at 0x4A64D0 goes on to what was there: CHud::Draw, or another
// mod's hook of it (the Widescreen Fix scales the HUD there).
static DWORD g_hudDrawFn = FN_HUD_DRAW;
static void __cdecl HudDraw() {
    ((void (__cdecl*)())g_hudDrawFn)();
    if (!g_ok || !g_mode) return;
    if (*(DWORD*)CLOCK - g_lastRun > 250) return;   // the gun isn't out right now
    if (!Active(FindPlayerVehicle())) return;
    if (!*(void**)SPRITE_SITE_M16) return;            // texture not loaded
    const float w = (float)*(int*)SCREEN_W, h = (float)*(int*)SCREEN_H;
    if (w < 64 || h < 64) return;
    // a little smaller than CHud::Draw sizes it for SMGs and pistols (32 * 0.4, 0x557740...)
    const float hx = w * (1.0f / 640.0f) * 32.0f * 0.3f, hy = h * (1.0f / 448.0f) * 32.0f * 0.3f;
    const float cx = w * 0.5f, cy = h * 0.5f;
    Rect r = { cx - hx, cy + hy, cx + hx, cy - hy };
    const unsigned white = 0xFFFFFFFFu;
    ((void (__thiscall*)(void*, const Rect*, const unsigned*, float, float, float, float, float, float, float, float))FN_SPRITE_DRAW)(
        (void*)SPRITE_SITE_M16, &r, &white, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f);
}

// ---------------------------------------------------------------- install
static DWORD CallTarget(DWORD site) {
    const unsigned char* p = (const unsigned char*)site;
    return p[0] == 0xE8 ? site + 5 + *(const int*)(p + 1) : 0;
}

static void Write5(DWORD site, unsigned char op, const void* target) {
    unsigned char code[5] = { op };
    *(int*)(code + 1) = (int)((DWORD)target - (site + 5));
    DWORD old;
    VirtualProtect((void*)site, 5, PAGE_EXECUTE_READWRITE, &old);
    memcpy((void*)site, code, 5);
    VirtualProtect((void*)site, 5, old, &old);
}

void InstallDriveBy() {
    // installed even when off: the Modern Fix menu (fixmenu.cpp) turns it on and off while
    // playing, and every hook below then does what the game does
    static const unsigned char kStep2[] = { 0x0F, 0xB6, 0x05, 0xFE, 0x46, 0x7E, 0x00 };
    static const DWORD kStep2Sites[] = { 0x5C982C, 0x5C925E, 0x5C95BC };
    static const DWORD kFiredSites[] = { 0x5C99B6, 0x5C9474, 0x5C970F };
    static const DWORD kInfoSites[] = { 0x5C97FC, 0x5C922C, 0x5C958C };
    static const DWORD kEnterSites[] = { 0x4EFB96, 0x5002D3, 0x51A3E8 };
    // Each check names itself, so the log says which one another mod changed. The HUD
    // draw call (0x4A64D0) and CSprite2d::Draw may be hooked by another mod (the
    // Widescreen Fix hooks both to scale the HUD): the call is chained, and the
    // crosshair is drawn through whatever CSprite2d::Draw now is.
    const char* bad = nullptr;
    char what[48];
    auto need = [&](bool c, const char* name, DWORD at) {
        if (!c && !bad) { wsprintfA(what, "%s at %06lX", name, at); bad = what; }
    };
    need(!memcmp((void*)0x5C9893, "\xC7\x44\x24\x14\x6F\x00\x00\x00", 8), "car weapon code", 0x5C9893);
    need(!memcmp((void*)0x5C92CB, "\x80\x7C\x24\x04\x00", 5), "bike weapon code", 0x5C92CB);
    need(!memcmp((void*)0x5C9623, "\x80\x7C\x24\x08\x00", 5), "boat weapon code", 0x5C9623);
    need(CallTarget(0x5CB960) == FN_AUTOAIM, "auto-aim call", 0x5CB960);
    const DWORD hud = CallTarget(0x4A64D0);
    need(hud != 0, "HUD draw call", 0x4A64D0);
    for (DWORD s : kStep2Sites) need(!memcmp((void*)s, kStep2, sizeof kStep2), "drive-by step", s);
    for (DWORD s : kFiredSites) need(CallTarget(s) == FN_FIRED, "fire button call", s);
    for (DWORD s : kInfoSites) need(CallTarget(s) == FN_INFO, "weapon info call", s);
    for (DWORD s : kEnterSites) need(CallTarget(s) == FN_ENTER_WEAPON, "car weapon choice call", s);
    if (bad) { Log("DriveBy: %s is not v1.0 (or changed by another mod); free drive-by not installed", bad); return; }
    g_hudDrawFn = hud;
    if (hud != FN_HUD_DRAW) Log("DriveBy: HUD draw call hooked by another mod (%08lX); chaining to it", hud);
    if (memcmp((void*)FN_SPRITE_DRAW, "\x8B\x44\x24\x04\x8B\x54\x24\x08\x53\x89\xCB", 11))
        Log("DriveBy: CSprite2d::Draw hooked by another mod; the drive-by crosshair is drawn through it");
    *(int*)(g_fakeSlot5 + 0x60) = 5;
    Write5(0x5C982C, 0xE9, (void*)&vcmf_db_car);
    Write5(0x5C925E, 0xE9, (void*)&vcmf_db_bike);
    Write5(0x5C95BC, 0xE9, (void*)&vcmf_db_boat);
    for (DWORD s : kFiredSites) Write5(s, 0xE8, (void*)&FiredGate);
    for (DWORD s : kInfoSites) Write5(s, 0xE8, (void*)&InfoForSlotCheck);
    for (DWORD s : kEnterSites) Write5(s, 0xE8, (void*)&EnterWeapon);
    Write5(0x5CB960, 0xE8, (void*)&AimShot);
    Write5(0x4A64D0, 0xE8, (void*)&HudDraw);
    void* const kills[2] = { (void*)&Kill0, (void*)&Kill1 };
    for (int i = 0; i < 2; i++) {
        if (const DWORD t = CallTarget(kKillSites[i])) { g_killFn[i] = t; Write5(kKillSites[i], 0xE8, kills[i]); }
        else Log("DriveBy: rampage kill call at %06lX changed by another mod; drive-by mode kills count as the game decides",
                 kKillSites[i]);
    }
    FlushInstructionCache(GetCurrentProcess(), nullptr, 0);
    g_ok = true;
    Log("DriveBy: free drive-by installed, %s (key %d toggles; cars, bikes, boats; pistols, SMGs, M4, Ruger)",
        g_cfg.freeDriveBy ? "on" : "off", g_cfg.driveByKey);
}
