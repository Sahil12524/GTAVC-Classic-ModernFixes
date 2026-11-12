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
#include "freecam.h"
#include "config.h"
#include "gameplay.h"
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
static const DWORD FIND_PLAYER_VEHICLE = 0x4BC1E0;
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

static const float PI = 3.14159265f;

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
    if (!g_fromRender) KeepFlyHeading();   // FixedTick off: one update late, still turns only the camera
    unsigned char* veh = *(unsigned char**)(cam + CAM_TARGET);
    unsigned char* player = ((unsigned char*(__cdecl*)())FIND_PLAYER_VEHICLE)();
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

    const bool mouseSteering = *(unsigned char*)MOUSE_PLAYER_CAM && !*(unsigned char*)NO_MOUSE_STEER;
    const bool blocked = mouseSteering || *(unsigned short*)(PAD0 + PAD_DISABLE);
    const float mx = *(float*)MOUSE_X, my = *(float*)MOUSE_Y;
    if (blocked) {
        s.active = false;
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
        s.relYaw -= mx * 2.5f * *(float*)SENS_H * scale;
        while (s.relYaw > PI) s.relYaw -= 2 * PI;
        while (s.relYaw < -PI) s.relYaw += 2 * PI;
        const float dp = my * 4.0f * *(float*)SENS_V * scale;
        s.pitch += g_cfg.freeCamInvertY ? dp : -dp;
        if (s.pitch > 1.2f) s.pitch = 1.2f;
        if (s.pitch < -0.6f) s.pitch = -0.6f;
        s.lastMouse = now;
    }
    // In the Rhino and fire truck the camera aims the gun: it stays where you put it.
    if (s.active && g_cfg.freeCamReturnDelay >= 0 && Moving(veh) && !MouseAimVehicle(veh) &&
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

static void __fastcall HookHeight(unsigned char* cam, void*, float* t, float orientation, float height) {
    g_origHeight(cam, nullptr, t, orientation, height);
    if (cam != s.cam || s.pitch == 0.0f) return;
    float* src = (float*)(cam + CAM_SOURCE);
    float dx = src[0] - t[0], dy = src[1] - t[1], dz = src[2] - t[2];
    float h = sqrtf(dx * dx + dy * dy);
    if (h < 0.01f) return;
    float dist = sqrtf(h * h + dz * dz);
    float e = atan2f(dz, h) + s.pitch;
    if (e > 1.25f) e = 1.25f;    // about 72 degrees above
    if (e < -0.25f) e = -0.25f;  // about 14 degrees below
    float nh = dist * cosf(e) / h;
    src[0] = t[0] + dx * nh;
    src[1] = t[1] + dy * nh;
    src[2] = t[2] + dist * sinf(e);
}

static void __fastcall HookRot(unsigned char* cam, void*, float* t, float orientation) {
    if (cam == s.cam && s.active) return;   // looking around: don't swing back behind the vehicle
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

void InstallFreeCam() {
    if (!g_cfg.freeCam) { Log("FreeCam: off"); return; }
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
    Log("FreeCam: installed (sensitivity %d%%, return delay %d ms, invert Y %d)", g_cfg.freeCamSensitivity,
        g_cfg.freeCamReturnDelay, g_cfg.freeCamInvertY);
}
