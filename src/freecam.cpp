// Mouse look around the vehicle you're driving.
//
// In a car, bike, boat or aircraft the game uses its "camera on a string" mode
// (CCam mode 18, 0x47FE8C): the camera keeps its position from the last update and
// is dragged behind the vehicle as it moves, then raised to a height
// (WorkOutCamHeight, 0x474F16) and swung back behind the vehicle when it is driving
// forward (RotCamIfInFrontCar, 0x4758C6). The mouse is not used at all.
//
// Three calls inside that mode are hooked:
//  - before the "string" step (0x48623C) the mouse turns the camera's position
//    around the vehicle, kept at the same angle to the vehicle's heading while
//    you're looking around; the game then applies its own distance limits;
//  - after the height step the camera is raised or lowered by the mouse's vertical
//    movement (the distance stays the same);
//  - the swing back behind the vehicle is skipped while you're looking around.
// Everything after that (the game's wall and vehicle checks, the line-of-sight
// test, the camera's direction) runs unchanged on the new position.
//
// You stop looking around when the vehicle is moving and the mouse has been still
// for [Camera] FreeCamReturnDelay ms: the game's own camera then swings back
// behind the vehicle, as before. Parked, the camera stays where you put it.
// Mouse speed and direction follow the game's own on-foot mouse camera
// (0x47C09F: Beta -= X * 2.5 * horizontal sensitivity * FOV/80, Alpha likewise
// with 4.0 * Y). Off while the game uses the mouse to steer (Controls menu,
// mouse steering) and while player controls are disabled (cutscenes).
//
// The vehicle is the one you control: a remote-controlled one if there is one
// (CPlayerInfo +4, Players[0] at 0x94AD28, as GET_REMOTE_CONTROLLED_CAR reads it:
// the RC helicopter, plane and cars of the RC missions), else the one you sit in.
//
// Boats ([Camera] FreeCamBoats): the game gives boats their own camera,
// MODE_BEHINDBOAT (22, CCam::Process_BehindBoat 0x480F17), which none of the
// above runs in. Where the camera picks a mode for the target vehicle (0x470C23:
// vehicle type 1, a boat, requests 0x16 at 0x470C30), boats request mode 18 like
// every other vehicle instead (the byte at 0x470C37), so they get the same string
// camera, and with it the mouse look.
#include "freecam.h"
#include "config.h"
#include "gameplay.h"
#include "driveby.h"
#include <windows.h>
#include <math.h>
#include <string.h>

static const DWORD SITE_STRING = 0x48038E, FN_STRING = 0x48623C;   // Cam_On_A_String_Unobscured(target, baseDist)
static const DWORD SITE_HEIGHT = 0x4803A5, FN_HEIGHT = 0x474F16;   // WorkOutCamHeight(target, orientation, height)
static const DWORD SITE_ROT    = 0x4803B8, FN_ROT    = 0x4758C6;   // RotCamIfInFrontCar(target, orientation)

static const DWORD CAM_TARGET = 0x1B0, CAM_SOURCE = 0x174, CAM_FOV = 0xB4;   // CCam
static const DWORD ENT_FORWARD = 0x14, PHYS_MOVE_SPEED = 0x70;            // CEntity matrix, CPhysical
static const DWORD MOUSE_X = 0x94D790, MOUSE_Y = 0x94D794;                // CPad::NewMouseControllerState
static const DWORD SENS_H = 0x94DBD0, SENS_V = 0xA0D964;                  // CCamera mouse sensitivity
static const DWORD PAD0 = 0x7DBCB0, PAD_DISABLE = 0xF0;                   // CPad::GetPad(0), DisablePlayerControls
static const DWORD MOUSE_PLAYER_CAM = 0xA10B4C, NO_MOUSE_STEER = 0x69C610;
static const DWORD FIND_PLAYER_VEHICLE = 0x4BC1E0, REMOTE_VEHICLE = 0x94AD28 + 4;

// The vehicle the player controls: the remote-controlled one, else the one they are in.
static unsigned char* ControlledVehicle() {
    if (unsigned char* rc = *(unsigned char**)REMOTE_VEHICLE) return rc;
    return ((unsigned char*(__cdecl*)())FIND_PLAYER_VEHICLE)();
}
static const DWORD CLOCK = 0x974B2C, TIMESTEP = 0x975424;

typedef void (__fastcall* String_t)(void*, void*, float*, float);
typedef void (__fastcall* Height_t)(void*, void*, float*, float, float);
typedef void (__fastcall* Rot_t)(void*, void*, float*, float);
static String_t g_origString;
static Height_t g_origHeight;
static Rot_t g_origRot;

static struct {
    unsigned char* veh;    // the player's vehicle the state belongs to
    unsigned char* cam;    // CCam processing it this update
    bool  active;          // looking around: the mouse decides the camera's angle
    float relYaw;          // camera position angle relative to the vehicle's heading
    float pitch;           // added to the camera's elevation, radians
    float horiz;           // horizontal distance before the pitch change
    float heading;
    bool  flying;          // trainer fly mode: relYaw is measured from flyRef, a fixed world direction
    float flyRef;
    DWORD lastMouse, lastClock;
} s;
static unsigned g_starts = 0;
// What the latest update turned the camera by from the mouse, as a view turn
// (yaw left +, pitch up +), for the per-frame drawing (FreeCamFrameTurn).
static float g_updYaw = 0.0f, g_updPitch = 0.0f;
// Camera elevation above the target point, radians. Low limit: about 5 degrees
// below it (the old 14 put the camera at ground level behind the vehicle, where the
// game's collision pushed it back up: it bounced). The mouse's pitch is kept so
// the elevation stays inside the limits, so turning back answers at once.
static const float kElevMin = -0.09f, kElevMax = 1.25f;
static float g_base = 0.0f;   // the game's own elevation at the latest update

static const float PI = 3.14159265f;

// [Camera] CarLookForwardKey (C): held in a vehicle, the camera is put straight
// behind it, level with the game's own height, looking forward; the mouse is not
// used meanwhile. Let go, the mouse look carries on from there (and, driving, the
// game's camera takes over again after FreeCamReturnDelay as usual). By default C is
// only the on-foot crouch (the game's defaults, 0x619D4A), so nothing in a vehicle
// shares it. Not while the game's menu is open.
static const DWORD MENU_ACTIVE = 0x869668;
static unsigned g_fwdLogs = 0;
static bool LookForwardHeld() {
    const int k = g_cfg.carLookForwardKey;
    return k > 0 && !*(volatile unsigned char*)MENU_ACTIVE && (GetAsyncKeyState(k) & 0x8000) != 0;
}

static float Heading(const unsigned char* veh, float fallback) {
    float fx = *(const float*)(veh + ENT_FORWARD), fy = *(const float*)(veh + ENT_FORWARD + 4);
    return fx * fx + fy * fy < 0.01f ? fallback : atan2f(fy, fx);
}

static bool Moving(const unsigned char* veh) {
    const float* v = (const float*)(veh + PHYS_MOVE_SPEED);   // units per 1/50 s
    return v[0] * v[0] + v[1] * v[1] + v[2] * v[2] > 0.02f * 0.02f;   // > about 1 m/s
}

// Trainer fly / noclip modes (VCTrainer's included) put the vehicle somewhere each
// update and turn it to face the camera (TheCamera's forward, 0x7E4698). Such a
// vehicle is recognised by what that leaves behind: perfectly level (up = 0,0,1),
// no speed or spin at all, and facing exactly the camera's level direction. The
// vehicle is left to face where the camera looks (the front follows the mouse);
// while flying, the camera's angle around it is kept as a world direction instead
// of an angle to the vehicle's heading, or turning the vehicle would turn the
// camera, which would turn the vehicle again.
static const DWORD CAMERA_FORWARD = 0x7E4698;   // TheCamera matrix forward
static const DWORD ENT_RIGHT = 0x04, ENT_UP = 0x24, PHYS_TURN_SPEED = 0x7C;
static struct {
    unsigned char* veh;   // the flying vehicle, or null
} g_fly;
static unsigned g_flyLogs = 0;

static bool FacesCameraLevel(const unsigned char* veh) {
    const float* r = (const float*)(veh + ENT_RIGHT);
    const float* f = (const float*)(veh + ENT_FORWARD);
    const float* u = (const float*)(veh + ENT_UP);
    const float* mv = (const float*)(veh + PHYS_MOVE_SPEED);
    const float* tn = (const float*)(veh + PHYS_TURN_SPEED);
    if (u[0] != 0.0f || u[1] != 0.0f || u[2] != 1.0f || r[2] != 0.0f || f[2] != 0.0f) return false;
    for (int i = 0; i < 3; i++) if (mv[i] != 0.0f || tn[i] != 0.0f) return false;
    const float* c = (const float*)CAMERA_FORWARD;
    float l = sqrtf(c[0] * c[0] + c[1] * c[1]);
    if (l < 0.001f) return false;
    return fabsf(f[0] - c[0] / l) < 0.001f && fabsf(f[1] - c[1] / l) < 0.001f;
}

static bool g_fromRender = false;   // called by FixedTick before each tick is drawn

static void KeepFlyHeading() {
    unsigned char* veh = ((unsigned char*(__cdecl*)())FIND_PLAYER_VEHICLE)();
    // never during cutscenes / scripted scenes, when the script may be turning it
    if (!veh || *(unsigned short*)(PAD0 + PAD_DISABLE) || !FacesCameraLevel(veh)) { g_fly.veh = nullptr; return; }
    if (veh != g_fly.veh && g_flyLogs++ < 3)
        Log("FreeCam: vehicle in a trainer fly mode; it faces where the camera looks");
    g_fly.veh = veh;
}

void FreeCamAfterUpdate() {
    if (!g_cfg.freeCam) return;
    g_fromRender = true;
    KeepFlyHeading();
}

static void __fastcall HookString(unsigned char* cam, void*, float* t, float baseDist) {
    if (ReplayPlaying() || !g_cfg.freeCam) {   // the replay's camera, or turned off in the menu
        s.cam = nullptr;
        s.veh = nullptr;
        g_origString(cam, nullptr, t, baseDist);
        return;
    }
    if (!g_fromRender) KeepFlyHeading();   // FixedTick off: one update late, still turns only the camera
    unsigned char* veh = *(unsigned char**)(cam + CAM_TARGET);
    unsigned char* player = ControlledVehicle();
    if (!veh || veh != player) {
        s.cam = nullptr;
        s.veh = nullptr;   // next time in a vehicle: start fresh
        g_origString(cam, nullptr, t, baseDist);
        return;
    }
    if (veh != s.veh) {   // another vehicle: start behind it, as the game does
        memset(&s, 0, sizeof s);
        s.veh = veh;
        s.heading = Heading(veh, 0.0f);
    }
    s.cam = cam;
    float* src = (float*)(cam + CAM_SOURCE);
    const float live = s.heading = Heading(veh, s.heading);
    // In a fly mode the vehicle turns to face the camera: the camera's angle is then
    // kept from a fixed direction, so turning the vehicle doesn't turn the camera.
    const bool flying = g_fly.veh == veh;
    if (flying && !s.flying) s.flyRef = live;
    if (!flying && s.flying) {
        s.relYaw += s.flyRef - live;
        while (s.relYaw > PI) s.relYaw -= 2 * PI;
        while (s.relYaw < -PI) s.relYaw += 2 * PI;
    }
    s.flying = flying;
    const float heading = flying ? s.flyRef : live;
    const DWORD now = *(DWORD*)CLOCK;
    const bool newUpdate = now != s.lastClock;
    s.lastClock = now;
    if (newUpdate) g_updYaw = g_updPitch = 0.0f;

    const bool mouseSteering = *(unsigned char*)MOUSE_PLAYER_CAM && !*(unsigned char*)NO_MOUSE_STEER;
    const bool blocked = mouseSteering || *(unsigned short*)(PAD0 + PAD_DISABLE);
    const float mx = *(float*)MOUSE_X, my = *(float*)MOUSE_Y;
    if (blocked) {
        s.active = false;
    } else if (LookForwardHeld()) {   // straight behind the vehicle, looking forward
        if (!s.active) {
            float dx = src[0] - t[0], dy = src[1] - t[1];
            s.horiz = sqrtf(dx * dx + dy * dy);
            if (s.horiz < 0.01f) s.horiz = *(float*)(cam + 0xD0);
            s.active = true;
        }
        float rel = live + PI - heading;
        while (rel > PI) rel -= 2 * PI;
        while (rel < -PI) rel += 2 * PI;
        s.relYaw = rel;
        s.pitch = 0.0f;
        s.lastMouse = now;
        if (g_fwdLogs++ == 0) Log("FreeCam: look forward key held: camera behind the vehicle, looking forward");
    } else if (newUpdate && (mx != 0.0f || my != 0.0f)) {   // once per game update
        if (!s.active) {
            float dx = src[0] - t[0], dy = src[1] - t[1];
            s.horiz = sqrtf(dx * dx + dy * dy);
            s.relYaw = s.horiz > 0.01f ? atan2f(dy, dx) - heading : PI;
            s.active = true;
            if (g_starts++ < 3) Log("FreeCam: looking around the vehicle with the mouse (camera %.1f m away, full distance %.1f m)",
                                    s.horiz, *(float*)(cam + 0xD0));
        }
        const float scale = *(float*)(cam + CAM_FOV) / 80.0f * g_cfg.freeCamSensitivity / 100.0f;
        const float turn = -mx * 2.5f * *(float*)SENS_H * scale;
        s.relYaw += turn;
        g_updYaw = turn;
        const float pitchBefore = s.pitch;
        while (s.relYaw > PI) s.relYaw -= 2 * PI;
        while (s.relYaw < -PI) s.relYaw += 2 * PI;
        const float dp = my * 4.0f * *(float*)SENS_V * scale;
        s.pitch += g_cfg.freeCamInvertY ? dp : -dp;
        if (s.pitch > 1.2f) s.pitch = 1.2f;
        if (s.pitch < -0.6f) s.pitch = -0.6f;
        g_updPitch = -(s.pitch - pitchBefore);   // camera raised = view tilted down
        s.lastMouse = now;
    }
    // In the Rhino and fire truck, and while shooting from the vehicle, the camera
    // aims the gun: it stays where you put it.
    if (s.active && g_cfg.freeCamReturnDelay >= 0 && Moving(veh) && !MouseAimVehicle(veh) && !DriveByAiming() &&
        now - s.lastMouse > (DWORD)g_cfg.freeCamReturnDelay)
        s.active = false;   // hand the camera back to the game: it swings behind the vehicle

    if (s.horiz > 0.01f && (s.active || s.pitch != 0.0f)) {
        // The game drags the camera out to its full distance (CCam +0xD0, set by the
        // string step each update) as the vehicle drives off. Holding the distance
        // from when looking started would keep a camera that was close behind the
        // vehicle (after a bump, parked) stuck there, inside a long vehicle's rear:
        // ease it out (or in) to the full distance instead, over about half a second.
        const float full = *(float*)(cam + 0xD0);
        if (newUpdate && full > 1.0f && full < 100.0f) {
            const float k = 1.0f - powf(0.8f, *(float*)TIMESTEP);
            s.horiz += (full - s.horiz) * k;
        }
        float dx = src[0] - t[0], dy = src[1] - t[1];
        float a = s.active ? heading + s.relYaw : atan2f(dy, dx);
        src[0] = t[0] + s.horiz * cosf(a);   // undo last update's pitch change to the distance
        src[1] = t[1] + s.horiz * sinf(a);
    }
    g_origString(cam, nullptr, t, baseDist);
    float dx = src[0] - t[0], dy = src[1] - t[1];
    s.horiz = sqrtf(dx * dx + dy * dy);

    if (!s.active && s.pitch != 0.0f && newUpdate) {   // ease back to the game's height
        float step = 0.04f * *(float*)TIMESTEP;          // about 2 radians per second
        s.pitch = fabsf(s.pitch) <= step ? 0.0f : s.pitch - (s.pitch > 0 ? step : -step);
    }
}

// While the drive-by gun is out the camera's elevation is held: the game's height
// step follows the vehicle's nose (bumps, braking, slopes) as seen from behind,
// which tilts the camera up and down on its own when it looks from the side. The
// elevation the game gave when the gun came out is kept; only the mouse changes it.
static bool g_dbHeld = false;
static float g_dbElev = 0.0f;

// Per-frame drawing (interp.cpp): the turn the next update will make from the mouse
// movement it hasn't seen yet (mx, my: counts as the game will read them), exactly
// as HookString will apply it. Only while this camera is the one in use.
bool FreeCamFrameTurn(float mx, float my, float* dYaw, float* dPitch, float* lastYaw, float* lastPitch,
                      const unsigned char** veh) {
    *dYaw = *dPitch = 0.0f;
    if (!g_cfg.freeCam || ReplayPlaying() || !s.cam || !s.veh || s.veh != ControlledVehicle()) return false;
    const unsigned char act = *(unsigned char*)0x7E46FE;
    if (s.cam != (unsigned char*)(0x7E4810 + act * 0x1CC) || *(short*)(s.cam + 0x0C) != 18) return false;
    const bool mouseSteering = *(unsigned char*)MOUSE_PLAYER_CAM && !*(unsigned char*)NO_MOUSE_STEER;
    if (mouseSteering || *(unsigned short*)(PAD0 + PAD_DISABLE)) return false;
    if (LookForwardHeld()) return false;   // the mouse doesn't turn it: drawn blended
    // the game's mouse state is flipped by the menu's invert options (0x4AD88E)
    if (*(unsigned char*)0xA10A4C) mx = -mx;
    if (*(unsigned char*)0xA10A4D) my = -my;
    const float scale = *(float*)(s.cam + CAM_FOV) / 80.0f * g_cfg.freeCamSensitivity / 100.0f;
    *dYaw = -mx * 2.5f * *(float*)SENS_H * scale;
    const float dp = my * 4.0f * *(float*)SENS_V * scale;
    float p = s.pitch + (g_cfg.freeCamInvertY ? dp : -dp);
    if (p > 1.2f) p = 1.2f;
    if (p < -0.6f) p = -0.6f;
    if (g_base + p > kElevMax) p = kElevMax - g_base;   // as HookHeight will keep it
    if (g_base + p < kElevMin) p = kElevMin - g_base;
    *dPitch = -(p - s.pitch);
    *lastYaw = g_updYaw;
    *lastPitch = g_updPitch;
    *veh = s.veh;
    return true;
}

static void __fastcall HookHeight(unsigned char* cam, void*, float* t, float orientation, float height) {
    g_origHeight(cam, nullptr, t, orientation, height);
    const bool hold = cam == s.cam && DriveByAiming() && !ReplayPlaying();
    if (!hold) g_dbHeld = false;
    if (cam != s.cam || ReplayPlaying()) return;
    float* src = (float*)(cam + CAM_SOURCE);
    float dx = src[0] - t[0], dy = src[1] - t[1], dz = src[2] - t[2];
    float h = sqrtf(dx * dx + dy * dy);
    if (h < 0.01f) return;
    float dist = sqrtf(h * h + dz * dz);
    float base = atan2f(dz, h);
    if (hold) {
        if (!g_dbHeld) { g_dbHeld = true; g_dbElev = base; }
        base = g_dbElev;
    }
    g_base = base;
    if (s.pitch == 0.0f && !hold) return;   // the game's own height
    if (!hold) {   // keep the mouse's part inside the limits
        float p = s.pitch;
        if (base + p > kElevMax) p = kElevMax - base;
        if (base + p < kElevMin) p = kElevMin - base;
        if (p != s.pitch) {
            if (g_updPitch != 0.0f) g_updPitch -= p - s.pitch;   // the mouse's turn this update, as kept (view up +)
            s.pitch = p;
        }
    }
    float e = base + s.pitch;
    if (e > kElevMax) e = kElevMax;   // about 72 degrees above
    if (e < kElevMin) e = kElevMin;   // about 5 degrees below
    float nh = dist * cosf(e) / h;
    src[0] = t[0] + dx * nh;
    src[1] = t[1] + dy * nh;
    src[2] = t[2] + dist * sinf(e);
}

static void __fastcall HookRot(unsigned char* cam, void*, float* t, float orientation) {
    if (cam == s.cam && s.active && !ReplayPlaying()) return;   // looking around: don't swing back behind the vehicle
    g_origRot(cam, nullptr, t, orientation);
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

// Boats on the vehicle camera: the camera mode byte at 0x470C37 (game: 16h, boat camera).
static bool g_boatOk = false;
void FreeCamBoatsApply() {
    if (!g_boatOk) return;
    const unsigned char mode = g_cfg.freeCam && g_cfg.freeCamBoats ? 0x12 : 0x16;   // MODE_CAM_ON_A_STRING / boat
    if (*(unsigned char*)0x470C37 == mode) return;
    DWORD old;
    VirtualProtect((void*)0x470C37, 1, PAGE_EXECUTE_READWRITE, &old);
    *(unsigned char*)0x470C37 = mode;
    VirtualProtect((void*)0x470C37, 1, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)0x470C37, 1);
}

// Installed even when off: the Modern Fix menu (fixmenu.cpp) turns it on and off while
// playing; the hooks then pass straight to the game's camera.
void InstallFreeCam() {
    if (CallTarget(SITE_STRING) != FN_STRING || CallTarget(SITE_HEIGHT) != FN_HEIGHT || CallTarget(SITE_ROT) != FN_ROT) {
        Log("FreeCam: vehicle camera code changed by another mod (%06lX %06lX %06lX); not installed",
            CallTarget(SITE_STRING), CallTarget(SITE_HEIGHT), CallTarget(SITE_ROT));
        return;
    }
    g_origString = (String_t)FN_STRING;
    g_origHeight = (Height_t)FN_HEIGHT;
    g_origRot = (Rot_t)FN_ROT;
    Redirect(SITE_STRING, (void*)&HookString);
    Redirect(SITE_HEIGHT, (void*)&HookHeight);
    Redirect(SITE_ROT, (void*)&HookRot);
    {
        // cmp dword [esp+38h],1; jne ...; fstp st0; mov word [ReqMode 0x70388A], 16h
        static const unsigned char boat[] = { 0x83, 0x7C, 0x24, 0x38, 0x01, 0x0F, 0x85, 0x37, 0x2D, 0x00, 0x00, 0xDD, 0xD8,
                                              0x66, 0xC7, 0x05, 0x8A, 0x38, 0x70, 0x00, 0x16, 0x00 };
        if (!memcmp((void*)0x470C23, boat, sizeof boat)) {
            g_boatOk = true;
            FreeCamBoatsApply();
            Log("FreeCam: boats %s", g_cfg.freeCam && g_cfg.freeCamBoats ? "use the vehicle camera (mouse look on boats)"
                                                                          : "keep their own camera");
        } else {
            Log("FreeCam: boat camera code is not v1.0; boats keep their own camera");
        }
    }
    Log("FreeCam: installed, %s (sensitivity %d%%, return delay %d ms, invert Y %d)", g_cfg.freeCam ? "on" : "off", g_cfg.freeCamSensitivity,
        g_cfg.freeCamReturnDelay, g_cfg.freeCamInvertY);
}
