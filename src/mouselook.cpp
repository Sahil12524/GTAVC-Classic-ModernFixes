// Per-frame mouse look ([Camera] FrameMouseLook) and the mod's own mouse feel
// ([Camera] MouseSensitivity, MouseEqualAxes, MouseSmoothing).
//
// The game reads the mouse once per game update (30 times a second):
// CPad::UpdateMouse (0x4AD820) calls _InputGetMouseState (0x5FF290, DirectInput,
// at 0x4AD86D) for the movement since the last read (DIMOUSESTATE: lX, lY, lZ,
// buttons). With frames drawn between updates, mouse look showed up one to two
// updates late (33-66 ms).
//
// Now, in play, the mouse is read every drawn frame (MouseFrameBegin, from
// interp.cpp). All movement goes into one running total; at its update the game
// gets everything not handed to it yet, added to its own read (buttons pressed in
// between are kept too), so it sees exactly the movement there was. Meanwhile the
// drawn camera is turned by the movement the game hasn't used yet, the way the
// game will turn it:
//  - on foot (here, FrameMouseTurn), CCam mode 4 with the mouse flag 0xA10B4C,
//    Process_FollowPedWithMouse 0x47BE8F, 0x47C033..0x47C234:
//      yaw   (CCam +0xBC) += -2.5 * X * horizontal sensitivity (0x94DBD0) * FOV / 80
//      pitch (CCam +0xAC) +=  4.0 * Y * vertical sensitivity   (0xA0D964) * FOV / 80
//    with X / Y flipped when the menu's invert options are on (0xA10A4C / 0xA10A4D,
//    as 0x4AD88E does). The view is (-cos yaw cos pitch, -sin yaw cos pitch,
//    sin pitch): more yaw turns left, more pitch looks up.
//  - in a vehicle (freecam.cpp, FreeCamFrameTurn), from MouseFrameCounts.
//
// Mouse feel: the counts are scaled before anything uses them (the game, the
// drawn camera, the vehicle camera, turret aiming): MouseSensitivity percent, and
// with MouseEqualAxes the vertical counts are scaled so looking up / down turns as
// fast as left / right (the game turns 4.0 x vertical sensitivity against 2.5 x
// horizontal, and its menu slider only changes the horizontal one). Fractions are
// carried to the next update, so nothing is lost or rounded away.
#include "mouselook.h"
#include "config.h"
#include <windows.h>
#include <math.h>
#include <string.h>

static const DWORD READ_SITE = 0x4AD86D, READ_FN = 0x5FF290;
static const DWORD CAM_ACTIVE = 0x7E46FE, CAMS = 0x7E4810, CAM_SIZE = 0x1CC;
static const DWORD MOUSE_CAM = 0xA10B4C, INVERT_X = 0xA10A4C, INVERT_Y = 0xA10A4D;
static const DWORD SENS_H = 0x94DBD0, SENS_V = 0xA0D964;
static const DWORD GAME_STATE = 0x9B5F08, MENU_ACTIVE = 0x869668, PAD0_DISABLE = 0x7DBCB0 + 0xF0;
static const DWORD GAME_WINDOW = 0x7897A4;

struct MouseState { LONG x, y, z; unsigned char buttons[8]; };
typedef int (__cdecl* Read_t)(MouseState*);
static Read_t g_read = (Read_t)READ_FN;
static bool g_ok = false;
static LONG g_accZ = 0;
static unsigned char g_accButtons[8];
static bool g_have = false;
static unsigned g_logs = 0;

// ---------------------------------------------------------------- counts
// Running totals in scaled counts; g_hand = what the game has been given.
static double g_totX = 0, g_totY = 0, g_handX = 0, g_handY = 0;
static LONGLONG g_qpf = 0;
static LONGLONG g_lastGameRead = 0;   // when the game last read the mouse
static unsigned g_staleLogs = 0;

static bool InPlay() {
    return *(int*)GAME_STATE == 9 && !*(unsigned char*)MENU_ACTIVE && !ReplayPlaying();
}

static void Scale(double* kx, double* ky) {
    *kx = *ky = 1.0;
    if (!InPlay()) return;
    *kx = *ky = g_cfg.mouseSensitivity / 100.0;
    const float sh = *(float*)SENS_H, sv = *(float*)SENS_V;
    if (g_cfg.mouseEqualAxes && sv > 1e-6f && sh > 1e-6f) *ky *= (2.5 * sh) / (4.0 * sv);
}

static LONGLONG Now() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }

// Mouse smoothing ([Camera] MouseSmoothing, ms, 0 = off). A mouse that reports
// less often than frames are drawn (125 Hz) moves the camera in uneven steps;
// every read is recorded as (time, total) and the drawn camera can use the total
// averaged over the last few ms. The game always gets the plain total.
struct MSample { LONGLONG t; double x, y; };
static const int kRing = 256;
static MSample g_ring[kRing];
static int g_ringHead = 0, g_ringCount = 0;

static void PushSample(LONGLONG t) {
    g_ring[g_ringHead] = { t, g_totX, g_totY };
    g_ringHead = (g_ringHead + 1) % kRing;
    if (g_ringCount < kRing) g_ringCount++;
}

static void SmoothedTotal(LONGLONG now, double* sx, double* sy) {
    const LONGLONG w = g_qpf * g_cfg.mouseSmoothMs / 1000;
    if (w <= 0 || !g_ringCount) { *sx = g_totX; *sy = g_totY; return; }
    const LONGLONG from = now - w;
    double ax = 0, ay = 0;
    LONGLONG end = now;
    MSample m = {};
    for (int i = 0; i < g_ringCount && end > from; i++) {
        m = g_ring[(g_ringHead - 1 - i + kRing) % kRing];
        const LONGLONG start = m.t > from ? m.t : from;
        if (end > start) { ax += m.x * (double)(end - start); ay += m.y * (double)(end - start); }
        if (start < end) end = start;
    }
    if (end > from) { ax += m.x * (double)(end - from); ay += m.y * (double)(end - from); }   // older than the records
    *sx = ax / (double)w;
    *sy = ay / (double)w;
}

static void AddRaw(LONG x, LONG y) {
    double kx, ky;
    Scale(&kx, &ky);
    g_totX += x * kx;
    g_totY += y * ky;
}

// ---------------------------------------------------------------- on-foot turn bookkeeping
// The game also turns its camera by itself (swinging behind the player as he runs,
// a target lock, a script...). Drawn frames show that part blended like
// everything else, and only the mouse's part ahead of time:
//   drawn = latest update's turn + mouse not used yet - (1 - blend) * self turn
// where the self turn is what the latest update turned beyond what its mouse
// movement gave. Right before an update this equals the previous view plus all the
// mouse movement; right after, the same: the view never switches between two
// ways of drawing (each switch was a visible jump).
//
// The self turn is measured on the captured camera matrices (interp.cpp hands in
// the captured fronts' yaw / pitch), not on CCam Beta / Alpha: another mod's hook
// after CGame::Process (an aim lock) can write Beta / Alpha after the update built
// the matrix. Counted from Beta, that write turned redraw frames away from the
// captured view, and the next update jumped forward by it: the view shook every
// tick. Measured on the matrices, it lands in the next update's matrix and is
// blended there like any of the game's own turns. The cut / snap rule is decided
// once per tick, at capture, so drawing never switches mode inside a tick.
static bool g_armed = false;          // the latest update started with the mouse camera on
static int g_beforeCam = -1;
static float g_mouseYaw = 0.0f, g_mousePitch = 0.0f;     // what its mouse movement should turn
// since the latest capture: the mouse reads (updates) and the turn their movement gave
static bool g_tickRead = false, g_tickReadArmed = false;
static int g_tickReadCam = -1;
static float g_tickMouseYaw = 0.0f, g_tickMousePitch = 0.0f;
// held from the latest capture until the next one
static bool g_tickArmed = false, g_tickSnap = false;
static float g_tickSelfYaw = 0.0f, g_tickSelfPitch = 0.0f;
// late writes: CCam Beta / Alpha at the capture, checked at the next mouse read
static bool g_capValid = false;
static int g_capCam = -1;
static float g_capYaw = 0.0f, g_capPitch = 0.0f;
static int g_lateWrites = 0;
static float g_lateMax = 0.0f;   // radians

static const float kPitchMax = 0.7853982f, kPitchMin = -1.5620697f;   // 0x47C293..0x47C2C8

static float ClampPitch(float a) { return a > kPitchMax ? kPitchMax : a < kPitchMin ? kPitchMin : a; }

static float WrapAngle(float a) {
    while (a > 3.14159265f) a -= 6.2831853f;
    while (a < -3.14159265f) a += 6.2831853f;
    return a;
}

static float YawFor(float x, float fov) {
    const float xs = x * (*(unsigned char*)INVERT_X ? -1.0f : 1.0f);
    return -2.5f * xs * *(float*)SENS_H * fov * 0.0125f;
}

static float PitchFor(float y, float fov) {
    const float ys = y * (*(unsigned char*)INVERT_Y ? -1.0f : 1.0f);
    return 4.0f * ys * *(float*)SENS_V * fov * 0.0125f;
}

static bool MouseCamActive();

// At the game's mouse read, right before the update turns its camera.
static void BeforeUpdate(LONG x, LONG y) {
    const int act = *(unsigned char*)CAM_ACTIVE;
    const unsigned char* cam = (const unsigned char*)(CAMS + act * CAM_SIZE);
    const float beta = *(const float*)(cam + 0xBC), alpha = *(const float*)(cam + 0xAC);
    // changed since the capture: written after the update (CamCheck)
    if (g_capValid && act == g_capCam) {
        const float dy = fabsf(WrapAngle(beta - g_capYaw)), dp = fabsf(alpha - g_capPitch);
        const float d = dy > dp ? dy : dp;
        if (d > 1e-4f) { g_lateWrites++; if (d > g_lateMax) g_lateMax = d; }
    }
    g_capValid = false;
    g_armed = MouseCamActive();
    g_beforeCam = act;
    const float fov = *(const float*)(cam + 0xB4);
    g_mouseYaw = YawFor((float)x, fov);
    g_mousePitch = PitchFor((float)y, fov);
    // what this update's mouse movement turns (the game clamps the pitch)
    if (!g_tickRead) { g_tickReadArmed = g_armed; g_tickReadCam = act; }
    else g_tickReadArmed = g_tickReadArmed && g_armed && act == g_tickReadCam;
    g_tickRead = true;
    g_tickMouseYaw += g_mouseYaw;
    g_tickMousePitch += ClampPitch(alpha + g_mousePitch) - alpha;
}

void MouseTickCaptured(bool ok, float yaw, float pitch, float prevYaw, float prevPitch) {
    // the game's own turn this tick: the captured front's turn beyond its mouse movement
    g_tickArmed = ok && g_tickRead && g_tickReadArmed;
    g_tickSelfYaw = g_tickArmed ? WrapAngle(yaw - prevYaw - g_tickMouseYaw) : 0.0f;
    g_tickSelfPitch = g_tickArmed ? pitch - prevPitch - g_tickMousePitch : 0.0f;
    g_tickSnap = g_tickArmed && (!(fabsf(g_tickSelfYaw) < 0.8f) || !(fabsf(g_tickSelfPitch) < 0.8f));
    g_tickRead = g_tickReadArmed = false;
    g_tickMouseYaw = g_tickMousePitch = 0.0f;
    // CCam as captured, to see whether something writes it before the next read
    g_capValid = g_ok && g_cfg.frameMouseLook && MouseCamActive();
    if (g_capValid) {
        g_capCam = *(unsigned char*)CAM_ACTIVE;
        const unsigned char* cam = (const unsigned char*)(CAMS + g_capCam * CAM_SIZE);
        g_capYaw = *(const float*)(cam + 0xBC);
        g_capPitch = *(const float*)(cam + 0xAC);
    }
}

bool MouseTickSnap() { return g_tickSnap; }

void MouseTakeLateWrites(int* count, float* maxDeg) {
    *count = g_lateWrites;
    *maxDeg = g_lateMax * 57.29578f;
    g_lateWrites = 0;
    g_lateMax = 0.0f;
}

// The game's own read: everything not handed to it yet (its own read included).
static int __cdecl GameRead(MouseState* out) {
    const int r = g_read(out);
    if (!g_ok || !g_cfg.frameMouseLook) return r;   // off in the Modern Fix menu: the game's own read
    if (r >= 0) AddRaw(out->x, out->y);
    g_lastGameRead = Now();
    PushSample(g_lastGameRead);
    if (r >= 0) {
        // whole counts now, the fraction at a later update
        const LONG gx = (LONG)(g_totX - g_handX), gy = (LONG)(g_totY - g_handY);
        g_handX += gx; g_handY += gy;
        out->x = gx; out->y = gy;
        if (g_have) {
            out->z += g_accZ;
            for (int i = 0; i < 8; i++) out->buttons[i] |= g_accButtons[i];
        }
    }
    if (!InPlay()) { g_totX = g_handX; g_totY = g_handY; }   // menus: no fractions kept
    g_accZ = 0;
    memset(g_accButtons, 0, sizeof g_accButtons);
    g_have = false;
    BeforeUpdate(r >= 0 ? out->x : 0, r >= 0 ? out->y : 0);
    return r;
}

static bool FrameReadAllowed() {
    if (!g_ok || !g_cfg.frameMouseLook || !InPlay()) return false;
    HWND w = *(HWND*)GAME_WINDOW;
    return w && GetForegroundWindow() == w && !TrainerWindowOpen();
}

static bool MouseCamActive() {
    if (!FrameReadAllowed() || *(unsigned short*)PAD0_DISABLE || !*(unsigned char*)MOUSE_CAM) return false;
    const int act = *(unsigned char*)CAM_ACTIVE;
    return *(short*)(CAMS + act * CAM_SIZE + 0x0C) == 4;   // FOLLOWPED (with the mouse)
}

static LONGLONG g_frameNow = 0;
static bool g_frameRead = false;

void MouseFrameBegin() {
    g_frameRead = false;
    g_frameNow = Now();
    if (!FrameReadAllowed()) return;
    // The game skips its mouse read at times (its own "in front" check, a trainer's
    // fly mode replacing the pad update...). Movement read meanwhile would pile up
    // with nowhere to go: the drawn camera ran ahead, then the game got it all at
    // once and spun round. So frames read only while the game keeps reading too.
    if (g_frameNow - g_lastGameRead > g_qpf / 12) {   // ~83 ms: over two updates missed
        if (g_totX != g_handX || g_totY != g_handY) {
            if (g_staleLogs < 3) { g_staleLogs++; Log("MouseLook: the game stopped reading the mouse; per-frame look paused until it reads again"); }
        }
        return;
    }
    MouseState st = {};
    if (g_read(&st) >= 0) {
        AddRaw(st.x, st.y);
        g_accZ += st.z;
        for (int i = 0; i < 8; i++) g_accButtons[i] |= st.buttons[i] & 0x80;
        g_have = true;
        if ((st.x || st.y) && g_logs < 1) { g_logs++; Log("MouseLook: camera turned with the mouse each frame"); }
    }
    PushSample(g_frameNow);
    g_frameRead = true;
}

bool MouseFrameCounts(float* dx, float* dy) {
    *dx = *dy = 0.0f;
    if (!g_frameRead) return false;
    double sx, sy;
    SmoothedTotal(g_frameNow, &sx, &sy);
    // Fractions included: the game gets whole counts and the rest at a later
    // update, and the drawn turn (total - handed) carries on across it unchanged.
    *dx = (float)(sx - g_handX);
    *dy = (float)(sy - g_handY);
    return true;
}

bool FrameMouseTurn(float blend, float* dYaw, float* dPitch) {
    *dYaw = *dPitch = 0.0f;
    if (!g_frameRead || !MouseCamActive()) return false;
    const int act = *(unsigned char*)CAM_ACTIVE;
    if (!g_armed || !g_tickArmed || act != g_beforeCam) return false;   // the camera just changed: drawn blended
    if (g_tickSnap) return false;   // a cut / snap this tick (decided at capture): blended
    const unsigned char* cam = (const unsigned char*)(CAMS + act * CAM_SIZE);
    const float fov = *(const float*)(cam + 0xB4);
    // Alpha only for the clamp the next update puts on the mouse movement: the
    // turn the latest update made by itself is the one measured on the matrices
    const float pitch = *(const float*)(cam + 0xAC);
    const float lag = 1.0f - (blend < 0.0f ? 0.0f : blend > 1.0f ? 1.0f : blend);
    float mx, my;
    MouseFrameCounts(&mx, &my);
    const float mouseYaw = YawFor(mx, fov), mousePitch = PitchFor(my, fov);
    if (!(fabsf(mouseYaw) < 1.0f) || !(fabsf(mousePitch) < 1.0f)) return false;   // a jump: let the game do it
    *dYaw = mouseYaw - lag * g_tickSelfYaw;
    *dPitch = ClampPitch(pitch + mousePitch) - pitch - lag * g_tickSelfPitch;
    return true;
}

void InstallFrameMouse() {
    // installed even when off: the Modern Fix menu (fixmenu.cpp) turns it on and off
    if (!g_cfg.fixedTick) { Log("MouseLook: needs FixedTick (frames drawn between updates); off"); return; }
    const unsigned char* p = (const unsigned char*)READ_SITE;
    if (p[0] != 0xE8) { Log("MouseLook: mouse read call changed by another mod; off"); return; }
    const DWORD target = READ_SITE + 5 + *(const int*)(p + 1);
    if (target != READ_FN) Log("MouseLook: mouse read hooked by another mod (%08lX); chaining to it", target);
    g_read = (Read_t)target;
    unsigned char call[5] = { 0xE8 };
    *(int*)(call + 1) = (int)((DWORD)&GameRead - (READ_SITE + 5));
    DWORD old;
    VirtualProtect((void*)READ_SITE, 5, PAGE_EXECUTE_READWRITE, &old);
    memcpy((void*)READ_SITE, call, 5);
    VirtualProtect((void*)READ_SITE, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)READ_SITE, 5);
    QueryPerformanceFrequency((LARGE_INTEGER*)&g_qpf);
    g_ok = true;
    Log("MouseLook: mouse read every frame (on foot and in vehicles), %s; sensitivity %d%%, equal axes %d, smoothing %d ms",
        g_cfg.frameMouseLook ? "on" : "off", g_cfg.mouseSensitivity, g_cfg.mouseEqualAxes ? 1 : 0, g_cfg.mouseSmoothMs);
}
