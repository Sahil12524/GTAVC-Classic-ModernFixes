// Free camera on foot ([Camera] FreeOrbitOnFoot): the mouse camera goes all the way
// round the player, so you can see their face, and they run the way you press
// (W away from the camera, S towards it, A / D to its sides), as in newer games.
// Aiming (a gun or thrown weapon out, with fire or the aim button held, and for
// 0.7 s after) turns them to where the camera looks, as before, so shots go to the
// crosshair.
//
// CPlayerPed::ProcessControl picks the on-foot movement at 0x537CFB:
//   - a first-person weapon camera (sniper, M16...)       -> 0x535550
//   - the mouse camera (CCam::Using3rdPersonMouseCam,
//     0x4758B0: mouse flag 0xA10B4C and camera mode 4)    -> 0x5357D0 (call at 0x537D2C)
//   - fighting (ped state 0x11)                           -> 0x535BB0
//   - otherwise (the joystick camera)                     -> 0x535F40
// 0x5357D0 turns the player to the camera's heading (CCamera::Orientation, 0x7E47E8)
// whenever they move, and the mouse camera keeps them facing away from it. The
// call at 0x537D2C goes to a wrapper that, unless aiming or fighting, runs it with
// the pressed direction as "forward" (FreeMove, below). The camera's own turning
// of the player is held off the same way (below).
#include "onfootcam.h"
#include "config.h"
#include <windows.h>
#include <math.h>
#include <string.h>

static const DWORD SITE = 0x537D2C, RUN_AROUND = 0x5357D0, ZELDA = 0x535F40;
// The player: CWorld::Players[0].m_pPed. (Not TheCamera's target entity, 0x7E4E94,
// which is the car while driving: read as the player it was the car's memory.)
static const DWORD PLAYER_PED = 0x94AD28;
static const DWORD CLOCK = 0x974B2C;
static const DWORD PED_STATE = 0x244, PED_WEAPONS = 0x408, PED_SLOT = 0x504, WEAPON_SIZE = 0x18;
static const auto TargetHeld = (bool (__thiscall*)(void*))0x4AA4D0;   // aim / lock-on button (RMB)
static const auto FireHeld = (bool (__thiscall*)(void*))0x4AAA60;     // fire (LMB)
typedef void (__thiscall* Control_t)(void*, void*);

static DWORD g_runAround = RUN_AROUND;
static DWORD g_aimUntil = 0;

// A gun or something thrown (weapon types 12 and up; 0-11 are fists and melee),
// with fire or aim held, or let go less than 0.7 s ago.
static bool Aiming(unsigned char* ped, void* pad) {
    const DWORD now = *(volatile DWORD*)CLOCK;
    const int slot = (signed char)ped[PED_SLOT];
    const int type = slot >= 0 && slot < 10 ? *(int*)(ped + PED_WEAPONS + slot * WEAPON_SIZE) : 0;
    if (type < 12) { g_aimUntil = 0; return false; }
    if (FireHeld(pad) || TargetHeld(pad)) g_aimUntil = now + 700;
    return now < g_aimUntil && g_aimUntil - now <= 700;
}

static bool FreeNow(unsigned char* ped, void* pad) {
    return g_cfg.freeOrbitOnFoot && ped && pad && *(int*)(ped + PED_STATE) != 0x11 && !Aiming(ped, pad);
}

// CPad::GetPedWalkLeftRight (0x4AAE30) / GetPedWalkUpDown (0x4AAD40): hooked
// (InstallWalkHooks). While the player is on foot with the free camera (not aiming,
// not fighting), pad 0's walk reads answer "straight ahead, as hard as pressed":
// the game picks the walk animation from them (S was "walk backwards", so the
// player, turned to face the camera, still moved away from it), and the body is
// turned to the pressed direction (FreeMove). The real values: g_walkLR / g_walkUD.
typedef short (__thiscall* Walk_t)(void*);
static Walk_t g_walkLR = (Walk_t)0x4AAE30, g_walkUD = (Walk_t)0x4AAD40;
static const auto WalkLeftRight = [](void* pad) { return g_walkLR(pad); };
static const auto WalkUpDown = [](void* pad) { return g_walkUD(pad); };
static bool g_walkHooked = false;
static int g_wSignFwd = -1;   // what W gives up / down (learned in FreeMove)
static bool FreeOnFoot();      // below

static short __fastcall HookWalkLR(void* pad, void*) {
    if (pad == (void*)0x7DBCB0 && FreeOnFoot()) return 0;
    return g_walkLR(pad);
}
static short __fastcall HookWalkUD(void* pad, void*) {
    const short ud = g_walkUD(pad);
    if (pad != (void*)0x7DBCB0 || !FreeOnFoot()) return ud;
    const float lr = g_walkLR(pad);
    float mag = sqrtf(lr * lr + (float)ud * ud);
    if (mag > 128.0f) mag = 128.0f;
    return (short)(mag * g_wSignFwd);
}

// Which sign W gives the walk's up / down and D its left / right: learned while
// playing (W or D held alone), so "forward" is fed exactly as W gives it.
static int g_wSign = 0, g_dSign = 0;
static bool Held(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

// The heading that points a ped along world direction (x, y), learned from the
// player: their stored heading (+0x374) against their matrix's forward (+0x14).
// The candidates are the ways an angle can be read off a direction; the one that
// matches is kept (and logged).
static int g_conv = -1;
static float Wrap(float a) {
    while (a > 3.14159265f) a -= 6.28318531f;
    while (a < -3.14159265f) a += 6.28318531f;
    return a;
}
static float HeadingFor(int conv, float x, float y) {
    switch (conv) {
    case 0: return atan2f(-x, y);
    case 1: return atan2f(x, -y);
    case 2: return atan2f(x, y);
    default: return atan2f(-x, -y);
    }
}
static void LearnConvention(unsigned char* ped) {
    const float* f = (const float*)(ped + 0x14);
    const float cur = *(float*)(ped + 0x374);
    if (f[0] * f[0] + f[1] * f[1] < 0.5f) return;   // lying down / tilted
    for (int c = 0; c < 4; c++) {
        if (fabsf(Wrap(HeadingFor(c, f[0], f[1]) - cur)) < 0.15f) {
            if (c != g_conv) Log("OnFootCam: player heading convention %d (forward %.2f %.2f, heading %.0f deg)", c, f[0], f[1], cur * 57.3f);
            g_conv = c;
            return;
        }
    }
}

// The active camera's look direction (CCam::Front, +0x168: what the game turns the
// player to with the mouse camera), level.
static const DWORD CAMS = 0x7E4810, CAM_SIZE = 0x1CC, CAM_ACTIVE = 0x7E46FE;
static bool CameraFront(float* x, float* y) {
    const float* fr = (const float*)(CAMS + *(unsigned char*)CAM_ACTIVE * CAM_SIZE + 0x168);
    const float l = sqrtf(fr[0] * fr[0] + fr[1] * fr[1]);
    if (l < 0.01f) return false;
    *x = fr[0] / l; *y = fr[1] / l;
    return true;
}

// The camera swings back behind a running player, as in San Andreas ([Camera]
// CameraSwingBehind): once the mouse has been still for 1.2 s while they run, the
// mouse camera's yaw (CCam +0xBC, mode 4; the view is (-cos yaw, -sin yaw)
// level) eases towards their facing (+0x14), at most about 85 degrees a second.
// Moving the mouse stops it (the game's mouse movement this update,
// CPad::NewMouseControllerState x / y 0x94D790 / 0x94D794). It turns the camera
// in the game update, which the per-frame mouse look draws smoothly in between.
static DWORD g_lastMouse = 0;
static void SwingBehind(unsigned char* ped) {
    const DWORD now = *(volatile DWORD*)0x974B2C;
    if (*(volatile float*)0x94D790 != 0.0f || *(volatile float*)0x94D794 != 0.0f || now < g_lastMouse) g_lastMouse = now;
    if (!g_cfg.cameraSwingBehind || now - g_lastMouse < 1200) return;
    const float* v = (const float*)(ped + 0x70);
    if (v[0] * v[0] + v[1] * v[1] < 0.02f * 0.02f) return;   // standing (about 1 m/s)
    unsigned char* cam = (unsigned char*)(CAMS + *(unsigned char*)CAM_ACTIVE * CAM_SIZE);
    if (*(short*)(cam + 0x0C) != 4) return;   // the mouse camera only
    const float* f = (const float*)(ped + 0x14);
    if (f[0] * f[0] + f[1] * f[1] < 0.5f) return;
    float& yaw = *(float*)(cam + 0xBC);
    const float diff = Wrap(atan2f(-f[1], -f[0]) - yaw);
    const float ts = *(float*)0x975424;
    const float t = ts > 0.0f && ts < 10.0f ? ts : 1.0f;
    float step = diff * (0.025f * t > 1.0f ? 1.0f : 0.025f * t);
    const float maxStep = 0.03f * t;
    if (step > maxStep) step = maxStep;
    if (step < -maxStep) step = -maxStep;
    yaw += step;
}

// Look the other way (LookBack, below): true from the update after the camera was
// turned round until the update after it was turned back, i.e. while the camera's
// Front (written by CCamera::Process, after the player's control) is the turned one.
static bool g_frontFlipped = false;

static void FreeMove(unsigned char* ped, void* pad) {
    LearnConvention(ped);
    const float lr = WalkLeftRight(pad), ud = WalkUpDown(pad);
    const bool w = Held('W') || Held(VK_UP), s_ = Held('S') || Held(VK_DOWN);
    const bool a = Held('A') || Held(VK_LEFT), d = Held('D') || Held(VK_RIGHT);
    if (w && !s_ && !a && !d && (ud > 1.0f || ud < -1.0f)) {
        const int sign = ud > 0 ? 1 : -1;
        if (sign != g_wSign) { g_wSign = sign; g_wSignFwd = sign; Log("OnFootCam: W gives walk up / down %+d", sign); }
    }
    if (d && !a && !w && !s_ && (lr > 1.0f || lr < -1.0f)) {
        const int sign = lr > 0 ? 1 : -1;
        if (sign != g_dSign) { g_dSign = sign; Log("OnFootCam: D gives walk left / right %+d", sign); }
    }
    float mag = sqrtf(lr * lr + ud * ud);
    if (mag < 1.0f) { ((Control_t)g_runAround)(ped, pad); return; }
    if (mag > 128.0f) mag = 128.0f;
    const int wSign = g_wSign ? g_wSign : -1, dSign = g_dSign ? g_dSign : 1;
    const float fwd = ud * wSign, right = lr * dSign;   // > 0: forward, right
    // The player's heading (+0x374 / +0x378) is in the camera matrix's terms: the
    // game turns them to the camera with atan2(-front.x, front.y) of the camera's
    // look direction (0x47CDD9), and forward = (-sin h, cos h), so left is +.
    // (CCamera::Orientation is minus that - the log showed camera 48, orientation
    // -48 - so using it sent them the mirrored way, often towards the camera.)
    // The way to go, in the world: the camera's level look direction for forward,
    // its right side ((y, -x) with z up) for right, mixed as pressed; then the
    // heading for it in the player's own convention (learned above).
    float cx, cy;
    // The game moves the player relative to the camera by the pad as it is (the log:
    // facing the camera with "forward" fed in, they still moved along the camera's
    // look direction). So the pad and the camera are left alone - S moves them
    // towards the camera, A / D to its sides - and only their body is turned to face
    // the way they move.
    ((Control_t)g_runAround)(ped, pad);
    if (g_conv < 0 || !CameraFront(&cx, &cy)) return;
    // Looking the other way: the player keeps going as the camera faced before it
    // turned round, so only the view turns, never the way they run.
    if (g_frontFlipped) { cx = -cx; cy = -cy; }
    const float dx = cx * fwd + cy * right, dy = cy * fwd - cx * right;
    const float h = Wrap(HeadingFor(g_conv, dx, dy));
    *(float*)(ped + 0x378) = h;   // m_fRotationDest: where they turn to, whatever it set
    // m_fRotationCur turned towards it here too, fast as in newer games (half a
    // turn in about 0.3 s): left to the game, a big turn (S: about face) didn't
    // happen - the body kept running the old way with only the head turned.
    {
        float& cur = *(float*)(ped + 0x374);
        const float ts = *(float*)0x975424;   // CTimer::ms_fTimeStep, 1 = 1/50 s
        const float step = 0.21f * (ts > 0.0f && ts < 10.0f ? ts : 1.0f);
        const float diff = Wrap(h - cur);
        cur = Wrap(cur + (diff > step ? step : diff < -step ? -step : diff));
    }
    if (!g_frontFlipped) SwingBehind(ped);
}

// The player on foot, the mouse camera on (CCam::Using3rdPersonMouseCam 0x4758B0 on
// Cams[0], as the game checks it), not a first-person weapon camera (0x46B8E2),
// and free (not aiming / fighting).
static bool FreeOnFoot() {
    if (!g_cfg.freeOrbitOnFoot) return false;
    unsigned char* ped = *(unsigned char**)PLAYER_PED;   // the player
    if (!ped || ped[0x3AC]) return false;   // in a vehicle
    if (!((bool (__thiscall*)(void*))0x4758B0)((void*)0x7E4810)) return false;
    if (((bool (__thiscall*)(void*))0x46B8E2)((void*)0x7E4688)) return false;
    return FreeNow(ped, (void*)0x7DBCB0);
}

static void LookBack(unsigned char* ped, void* pad);   // below

static void __fastcall OnFootControl(unsigned char* ped, void*, void* pad) {
    LookBack(ped, pad);
    if (FreeNow(ped, pad)) FreeMove(ped, pad);
    else ((Control_t)g_runAround)(ped, pad);
}

// The camera turns the player too, every frame, to its own heading (atan2 of its
// front, CCam +0x168 / +0x16C, written to the player's m_fRotationCur / Dest
// +0x374 / +0x378 and their matrix), in two places:
//   0x47EF94: with the mouse camera, if CPed::CanStrafeOrMouseControl (0x501890,
//             call at 0x47EF9F) says yes: that call answers no while free;
//   0x47CDBF: at the end of the mouse camera's update, unless CPad::GetPad(0)
//             (call at 0x47CDC1) has bit 0x20 of its disable word (+0xF0) set:
//             while free that call gives a stand-in pad with the bit set, which
//             only that test reads (it then returns).
static const DWORD STRAFE_SITE = 0x47EF9F, STRAFE_FN = 0x501890, GETPAD_SITE = 0x47CDC1, GETPAD_FN = 0x4AB060;
static const DWORD PAD0 = 0x7DBCB0;
static DWORD g_strafeFn = STRAFE_FN, g_getPadFn = GETPAD_FN;
static unsigned char g_skipPad[0x100];   // +0xF0 = 0x20

static bool __fastcall HookCanStrafe(unsigned char* ped, void*) {
    if (FreeNow(ped, (void*)PAD0)) return false;
    return ((bool (__thiscall*)(void*))g_strafeFn)(ped);
}

// The mouse camera also turns itself by the right stick's left / right
// (CPad 0x4A9A80, reading pad 0's RightStickX 0x7DBCB4; call at 0x47C062), which
// on the keyboard A / D (and the arrows) set too: holding A turned the camera, and
// the player, running relative to it, went round in a circle. While free, that
// turn is 0 when one of those keys is held (a gamepad's right stick still turns).
static const DWORD LOOK_LR_SITE = 0x47C062, LOOK_LR_FN = 0x4A9A80;
static DWORD g_lookLRFn = LOOK_LR_FN;

static bool KeyHeld(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

static short __fastcall HookLookLeftRight(void* pad, void*) {
    if (FreeNow(*(unsigned char**)PLAYER_PED, (void*)PAD0) &&
        (KeyHeld('A') || KeyHeld('D') || KeyHeld(VK_LEFT) || KeyHeld(VK_RIGHT)))
        return 0;
    return ((short (__thiscall*)(void*))g_lookLRFn)(pad);
}

static unsigned char* __cdecl HookGetPad(int n) {
    if (FreeNow(*(unsigned char**)PLAYER_PED, (void*)PAD0)) return g_skipPad;
    return ((unsigned char* (__cdecl*)(int))g_getPadFn)(n);
}

// On-foot look behind (action 13: middle mouse button, Num1, Caps Lock) turns the
// camera round; with the free camera that also turned the way the player runs
// (relative to the camera) round; LookBack (below) does it instead, turning only
// the camera. While on, its keyboard and mouse bindings (ControlsManager 0x9B491C,
// table +0xFF8: [action * 4 + type] { key, order }; none: keyboard 0x420, mouse 0)
// are cleared only while playing: whenever the game's menu is open
// (FrontEndMenuManager.m_bMenuActive 0x869668) they are put back, so the controls
// menu shows them, can change them and saves them (gta_vc.set) as they are; what it
// leaves is taken again when the menu closes. (Clearing them for good left "unbound"
// in the menu, saved that way, and the menu couldn't set the middle button again.)
// Once per run, an action left with no key at all by that is given the game's
// defaults back: Num1 (0x404) and the middle mouse button (mouse code 2; the
// menu's names, 0x616300: 1 left, 2 middle, 3 right).
struct Bind { int key, order; };
static Bind* const LookBehind = (Bind*)(0x9B491C + 0xFF8) + 13 * 4;
static Bind g_lookSaved[3];
static bool g_lookCleared = false, g_lookRepaired = false;
static const DWORD MENU_ACTIVE = 0x869668;
static const auto SetKeyForAction = (void (__thiscall*)(void*, int, int, int))0x615F80;   // action, key, type

static bool LookBehindBound() {
    return LookBehind[0].key != 0x420 || LookBehind[1].key != 0x420 || LookBehind[2].key != 0;
}

static bool g_installed = false;

void OnFootCamAfterPresent() {
    if (!g_installed) return;
    const bool clear = g_cfg.freeOrbitOnFoot && !*(volatile bool*)MENU_ACTIVE;
    if (clear && !g_lookCleared) {
        if (!g_lookRepaired) {
            g_lookRepaired = true;
            if (!LookBehindBound()) {
                SetKeyForAction((void*)0x9B491C, 13, 0x404, 0);
                SetKeyForAction((void*)0x9B491C, 13, 2, 2);
                Log("OnFootCam: on-foot look behind had no key; the game's Num1 and middle mouse button put back");
            }
        }
        memcpy(g_lookSaved, LookBehind, sizeof g_lookSaved);
        LookBehind[0] = { 0x420, 0 };
        LookBehind[1] = { 0x420, 0 };
        LookBehind[2] = { 0, 0 };
        g_lookCleared = true;
    } else if (!clear && g_lookCleared) {
        memcpy(LookBehind, g_lookSaved, sizeof g_lookSaved);
        g_lookCleared = false;
    }
}

// Look the other way on foot, with the look behind keys taken off the game above
// (the middle mouse button and Num1 by default, or whatever the Controls menu set):
// held, the camera turns round to the other side of the player, the player not
// turned at all. If the player's back is to the camera, the camera goes in front of
// them, looking at their face; if they face the camera, it goes behind them, looking
// the way they face. Let go, the camera turns back by the same angle (the mouse's
// turning meanwhile kept). The way the player runs stays as the camera was before
// (FreeMove), and the swing behind is held off.
// The turn is written to the mouse camera's yaw (CCam +0xBC, mode 4; the view is
// (-cos yaw, -sin yaw) level) here, in the player's control, before CCamera::Process
// builds the camera from it in the same update, so the whole update shows the
// turned camera (drawn as a cut: frame blending takes a turn of over 90 degrees in
// one update as a cut). Off while aiming (aiming turns the player to the camera),
// fighting, in cutscenes (player controls off), with another camera mode, and with
// FreeOrbitOnFoot off (the game's own look behind is then left as it is).
// The game's key codes (RsKeyCodes) for the look behind keys: below 256 the
// character; 1000 Esc, 1001..1012 F1..F12, 1013.. Ins Del Home End PgUp PgDn, arrows,
// keypad / * + -, the keypad (Del, End=1, 2, 3, 4, 5, NumLock, 6, 7, 8, 9, 0, Enter),
// Scroll, Pause, Backspace, Tab, Caps Lock, Enter, LShift, RShift, Shift, LCtrl,
// RCtrl, LAlt, RAlt, LWin, RWin, Apps; 1056 none (the 0x404 = Num1 above fits).
static int RsKeyToVk(int k) {
    static const unsigned char special[] = {
        VK_ESCAPE, VK_F1, VK_F2, VK_F3, VK_F4, VK_F5, VK_F6, VK_F7, VK_F8, VK_F9, VK_F10, VK_F11, VK_F12,
        VK_INSERT, VK_DELETE, VK_HOME, VK_END, VK_PRIOR, VK_NEXT, VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT,
        VK_DIVIDE, VK_MULTIPLY, VK_ADD, VK_SUBTRACT,
        VK_DECIMAL, VK_NUMPAD1, VK_NUMPAD2, VK_NUMPAD3, VK_NUMPAD4, VK_NUMPAD5, VK_NUMLOCK, VK_NUMPAD6,
        VK_NUMPAD7, VK_NUMPAD8, VK_NUMPAD9, VK_NUMPAD0, VK_RETURN,
        VK_SCROLL, VK_PAUSE, VK_BACK, VK_TAB, VK_CAPITAL, VK_RETURN, VK_LSHIFT, VK_RSHIFT, VK_SHIFT,
        VK_LCONTROL, VK_RCONTROL, VK_LMENU, VK_RMENU, VK_LWIN, VK_RWIN, VK_APPS };
    if (k >= 1000 && k < 1000 + (int)sizeof special) return special[k - 1000];
    if (k <= 0 || k >= 256) return 0;
    if (k >= 'a' && k <= 'z') return k - 'a' + 'A';
    if ((k >= 'A' && k <= 'Z') || (k >= '0' && k <= '9') || k == ' ') return k;
    const SHORT v = VkKeyScanA((CHAR)k);
    return v == -1 ? 0 : (v & 0xFF);
}

static bool LookBackKeyHeld() {
    const Bind* b = g_lookCleared ? g_lookSaved : LookBehind;
    for (int i = 0; i < 2; i++)
        if (const int vk = RsKeyToVk(b[i].key)) if (Held(vk)) return true;
    static const int mouse[] = { 0, VK_LBUTTON, VK_MBUTTON, VK_RBUTTON, 0, 0, VK_XBUTTON1, VK_XBUTTON2 };
    const int m = b[2].key;
    return m > 0 && m < 8 && mouse[m] && Held(mouse[m]);
}

static struct {
    bool on;              // the camera is turned round
    unsigned char* cam;   // the CCam turned
    float delta;          // the turn written to its yaw
    DWORD last;           // game clock at the latest call
} g_look;
static unsigned g_lookLogs = 0;

static void LookBack(unsigned char* ped, void* pad) {
    const DWORD now = *(volatile DWORD*)CLOCK;
    // not called for a while (a vehicle, a cutscene, a load): the turn is stale
    if (g_look.on && (now < g_look.last || now - g_look.last > 500)) g_look.on = false;
    g_look.last = now;
    g_frontFlipped = g_look.on;   // the camera built last update is the turned one
    unsigned char* cam = (unsigned char*)(CAMS + *(unsigned char*)CAM_ACTIVE * CAM_SIZE);
    const bool want = g_installed && g_cfg.freeOrbitOnFoot && pad == (void*)PAD0 && ped == *(unsigned char**)PLAYER_PED &&
                      !*(unsigned short*)(PAD0 + 0xF0) && *(short*)(cam + 0x0C) == 4 && FreeNow(ped, pad) &&
                      LookBackKeyHeld();
    if (want == g_look.on) return;
    if (want) {
        const float* f = (const float*)(ped + 0x14);
        const float l = sqrtf(f[0] * f[0] + f[1] * f[1]);
        if (l < 0.5f) return;   // lying down / tilted: no facing to go by
        float& yaw = *(float*)(cam + 0xBC);
        const float fx = f[0] / l, fy = f[1] / l;
        const float vx = -cosf(yaw), vy = -sinf(yaw);   // where the camera looks now
        const bool backToCamera = vx * fx + vy * fy > 0.0f;
        const float target = backToCamera ? atan2f(fy, fx)      // in front, looking at their face
                                          : atan2f(-fy, -fx);   // behind, looking where they face
        g_look.delta = Wrap(target - yaw);
        g_look.cam = cam;
        g_look.on = true;
        yaw += g_look.delta;
        if (g_lookLogs++ < 3)
            Log("OnFootCam: look the other way: camera turned %.0f deg to %s the player (player not turned)",
                g_look.delta * 57.29578f, backToCamera ? "face" : "the back of");
    } else {
        // the same camera still in the mouse mode: turned back by the same angle
        if (cam == g_look.cam && *(short*)(cam + 0x0C) == 4) *(float*)(cam + 0xBC) -= g_look.delta;
        g_look.on = false;
    }
}

static void WriteCode(DWORD addr, const void* bytes, int len) {
    DWORD old;
    VirtualProtect((void*)addr, len, PAGE_EXECUTE_READWRITE, &old);
    memcpy((void*)addr, bytes, len);
    VirtualProtect((void*)addr, len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)addr, len);
}

// Installed even when off: the Modern Fix menu turns it on and off while playing.
void InstallOnFootCam() {
    const unsigned char* p = (const unsigned char*)SITE;
    // call 0x5357D0 after "mov ecx, 0x7E4810; call 0x4758B0" (the mouse camera check)
    if (p[0] != 0xE8 || SITE + 5 + *(const int*)(p + 1) != RUN_AROUND || *(const unsigned char*)ZELDA != 0x53 ||
        *(const unsigned char*)0x537D17 != 0xB9) {
        Log("OnFootCam: player control code is not v1.0 or was changed by another mod; camera as in the game");
        return;
    }
    unsigned char call[5] = { 0xE8 };
    *(int*)(call + 1) = (int)((DWORD)&OnFootControl - (SITE + 5));
    WriteCode(SITE, call, 5);
    g_installed = true;

    // The walk reads: cmp word [ecx+0F0h],0 (8 bytes, moved to a trampoline as they
    // are); push ebx - a jump to the hook over the first 8.
    {
        static const unsigned char start[] = { 0x66, 0x83, 0xB9, 0xF0, 0x00, 0x00, 0x00, 0x00, 0x53 };
        const DWORD fns[2] = { 0x4AAE30, 0x4AAD40 };
        void* const hooks[2] = { (void*)&HookWalkLR, (void*)&HookWalkUD };
        Walk_t* const tramps[2] = { &g_walkLR, &g_walkUD };
        bool ok = !memcmp((void*)fns[0], start, sizeof start) && !memcmp((void*)fns[1], start, sizeof start);
        unsigned char* t = ok ? (unsigned char*)VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE) : nullptr;
        if (t) {
            for (int i = 0; i < 2; i++) {
                unsigned char* tr = t + i * 32;
                memcpy(tr, start, 8);
                tr[8] = 0xE9;
                *(int*)(tr + 9) = (int)((fns[i] + 8) - ((DWORD)tr + 13));
                *tramps[i] = (Walk_t)tr;
                unsigned char j[8] = { 0xE9, 0, 0, 0, 0, 0x90, 0x90, 0x90 };
                *(int*)(j + 1) = (int)((DWORD)hooks[i] - (fns[i] + 5));
                WriteCode(fns[i], j, 8);
            }
            g_walkHooked = true;
        } else {
            Log("OnFootCam: walk input code changed by another mod; S / A / D may walk backwards / sideways");
        }
    }

    auto target = [](DWORD site) -> DWORD {
        const unsigned char* q = (const unsigned char*)site;
        return q[0] == 0xE8 ? site + 5 + *(const int*)(q + 1) : 0;
    };
    *(unsigned short*)(g_skipPad + 0xF0) = 0x20;
    bool camOk = true;
    if (DWORD t = target(STRAFE_SITE)) {
        g_strafeFn = t;
        *(int*)(call + 1) = (int)((DWORD)&HookCanStrafe - (STRAFE_SITE + 5));
        WriteCode(STRAFE_SITE, call, 5);
    } else camOk = false;
    if (DWORD t = target(GETPAD_SITE)) {
        g_getPadFn = t;
        *(int*)(call + 1) = (int)((DWORD)&HookGetPad - (GETPAD_SITE + 5));
        WriteCode(GETPAD_SITE, call, 5);
    } else camOk = false;
    if (DWORD t = target(LOOK_LR_SITE); t == LOOK_LR_FN) {
        g_lookLRFn = t;
        *(int*)(call + 1) = (int)((DWORD)&HookLookLeftRight - (LOOK_LR_SITE + 5));
        WriteCode(LOOK_LR_SITE, call, 5);
    } else camOk = false;
    if (!camOk) Log("OnFootCam: camera code changed by another mod; the camera may still turn the player");
    Log("OnFootCam: free camera on foot, the player runs the way you press; aiming turns them to the camera (%s)",
        g_cfg.freeOrbitOnFoot ? "on" : "off");
}
