// Widescreen ([Graphics] Widescreen): the game's own picture is made for a 4:3 screen
// and stretched to any other; VC Modern Fix fits it to the screen's real shape.
//
// 3D picture (Hor+: the 4:3 view kept, more seen to the sides on a wider screen).
// CDraw::SetFOV (0x54A2E0) stores the camera's horizontal FOV, made for 4:3, in
// CDraw::ms_fFOV (0x696658); CameraSize (0x580860) makes the RenderWare view window
// tan(FOV / 2) wide and that divided by CDraw::ms_fAspectRatio (0x94DD38) high, and
// CDraw::CalculateAspectRatio (0x54A270, called before each frame's camera set-up)
// sets that ratio to 4:3 (5:4 with the cutscene bars; 16:9 / 5:3 with the menu's
// own widescreen option, which only letterboxes for 4:3 monitors). So:
//   - SetFOV stores the FOV widened for the screen: 2 atan(tan(FOV / 2) * S * 3/4),
//     S = width / height. The frustum (0x46C3F2) and the view window use it.
//   - CalculateAspectRatio returns the game's 4:3 value times S * 3/4, so the height
//     of the view is exactly the game's own.
//   - Readers that must not see the wider FOV read the game's own instead:
//     0x46DDA5 (70 / FOV = the camera's LOD distance multiplier: details would come in
//     later), 0x627D39 (saves the FOV to put it back through SetFOV: it would widen
//     twice), 0x577985 (corona / sprite height), and 0x577975 (sprite width: the
//     game's FOV times S * 3/4, so a corona stays round).
// HUD: each part of the game has its own copy of the 1/640 that scales 640-wide
// positions and sizes to the screen width (fmul [const]). CHud (0x697A70, 87 uses),
// CRadar (0x68FD14, 34), CDarkel's kill frenzy text (0x6874E4, 8) and 0x697DC0 (2,
// next to CHud) read a variable instead: 1/640 * 4/3 / S while playing, so the HUD
// keeps its 4:3 proportions (things placed from the right edge stay at the right
// edge, from the left at the left), and the game's 1/640 while a menu is open (the
// map in the menu is drawn by CRadar on the menu's stretched layout).
// Menus are left as the game draws them. With another widescreen mod loaded
// (GTAVC.WidescreenFix.asi) nothing here is installed.
#include "widescreen.h"
#include "config.h"
#include <windows.h>
#include <string.h>

static const DWORD CALC_ASPECT = 0x54A270, SET_FOV = 0x54A2E0;
static float* const AspectRatio = (float*)0x94DD38;   // CDraw::ms_fAspectRatio
static float* const FovVar = (float*)0x696658;        // CDraw::ms_fFOV
static const DWORD WIDESCREEN_BARS = 0x7E46F5;        // TheCamera's cutscene borders on
static const DWORD SCREEN_W = 0x9B48E4, SCREEN_H = 0x9B48E8, MENU_ACTIVE = 0x869668;
static const float kDegToRad = 0.017453292f;

static bool g_on = false;
static float g_horScale = 1.0f;            // S * 3/4: how much wider than 4:3 the screen is
static float g_fovOrig = 70.0f;            // the FOV the game set (4:3)
static float g_fovSpriteW = 70.0f;         // g_fovOrig * g_horScale (sprite width)
static float g_hudScaleX = 1.0f / 640.0f;  // the HUD's 1/640, for the screen's shape
static long g_fovCalls = 0, g_aspectCalls = 0;

static void UpdateShape() {
    const int w = *(const int*)SCREEN_W, h = *(const int*)SCREEN_H;
    float s = (w > 0 && h > 0) ? (float)w / (float)h : 4.0f / 3.0f;
    if (s < 1.0f || s > 6.0f) s = 4.0f / 3.0f;   // not a real screen
    g_horScale = s * 0.75f;
}

extern "C" void __cdecl vcmf_ws_setfov(float fov) {
    g_fovCalls++;
    g_fovOrig = fov;
    g_fovSpriteW = fov * g_horScale;
    float f = fov;
    if (fov > 1.0f && fov < 170.0f && g_horScale != 1.0f)
        f = 2.0f * __builtin_atanf(__builtin_tanf(fov * 0.5f * kDegToRad) * g_horScale) / kDegToRad;
    *FovVar = f;
}

extern "C" float __cdecl vcmf_ws_aspect() {
    g_aspectCalls++;
    const float base = *(volatile unsigned char*)WIDESCREEN_BARS ? 1.25f : 4.0f / 3.0f;
    const float a = base * g_horScale;
    *AspectRatio = a;
    return a;
}

static void Write(DWORD at, const void* p, size_t n) {
    DWORD old;
    VirtualProtect((void*)at, n, PAGE_EXECUTE_READWRITE, &old);
    memcpy((void*)at, p, n);
    VirtualProtect((void*)at, n, old, &old);
}

static void Jump(DWORD at, const void* to) {
    unsigned char code[5] = { 0xE9 };
    *(int*)(code + 1) = (int)((DWORD)to - (at + 5));
    Write(at, code, 5);
}

// Every "fmul dword ptr [constAddr]" (D8 0D addr) in the game's code points at var.
static int RedirectFmul(DWORD constAddr, const float* var) {
    const unsigned char* exe = (const unsigned char*)GetModuleHandleA(nullptr);
    const IMAGE_NT_HEADERS* nt = (const IMAGE_NT_HEADERS*)(exe + ((const IMAGE_DOS_HEADER*)exe)->e_lfanew);
    const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);   // .text
    const DWORD start = (DWORD)exe + sec->VirtualAddress, end = start + sec->Misc.VirtualSize - 6;
    int n = 0;
    for (DWORD p = start; p < end; p++) {
        const unsigned char* c = (const unsigned char*)p;
        if (c[0] == 0xD8 && c[1] == 0x0D && *(const DWORD*)(c + 2) == constAddr) {
            const DWORD a = (DWORD)var;
            Write(p + 2, &a, 4);
            n++;
        }
    }
    return n;
}

// One "fmul dword ptr [constAddr]" at `site` points at var.
static bool RedirectFmulAt(DWORD site, DWORD constAddr, const float* var) {
    const unsigned char* c = (const unsigned char*)site;
    if (c[0] != 0xD8 || c[1] != 0x0D || *(const DWORD*)(c + 2) != constAddr) return false;
    const DWORD a = (DWORD)var;
    Write(site + 2, &a, 4);
    return true;
}

// A reader of CDraw::ms_fFOV (its operand at `operand`) reads var instead.
static bool RedirectFovRead(DWORD operand, const float* var) {
    if (*(const DWORD*)operand != (DWORD)FovVar) return false;
    const DWORD a = (DWORD)var;
    Write(operand, &a, 4);
    return true;
}

void InstallWidescreen() {
    if (!g_cfg.widescreen) { Log("Widescreen: off (the game's stretched 4:3 picture)"); return; }
    if (GetModuleHandleA("GTAVC.WidescreenFix.asi")) {
        Log("Widescreen: the Widescreen Fix (GTAVC.WidescreenFix.asi) is loaded; widescreen left to it");
        return;
    }
    static const unsigned char aspectOrig[] = { 0x80, 0x3D, 0x52, 0x96, 0x86, 0x00, 0x00 };
    static const unsigned char aspect43[] = { 0xC7, 0x05, 0x38, 0xDD, 0x94, 0x00, 0xAB, 0xAA, 0xAA, 0x3F };   // at 0x54A2C0
    static const unsigned char fovOrig[] = { 0xD9, 0x44, 0x24, 0x04, 0xD9, 0x1D, 0x58, 0x66, 0x69, 0x00, 0xC3 };
    if (memcmp((void*)CALC_ASPECT, aspectOrig, sizeof aspectOrig) || memcmp((void*)0x54A2C0, aspect43, sizeof aspect43)
        || memcmp((void*)SET_FOV, fovOrig, sizeof fovOrig)) {
        Log("Widescreen: CDraw's aspect ratio / FOV code is not v1.0 (or changed by another mod); not installed");
        return;
    }
    UpdateShape();
    g_fovOrig = g_fovSpriteW = *FovVar;
    const bool lod = RedirectFovRead(0x46DDA5, &g_fovOrig);
    const bool save = RedirectFovRead(0x627D39, &g_fovOrig);
    const bool spriteW = RedirectFovRead(0x577975, &g_fovSpriteW);
    const bool spriteH = RedirectFovRead(0x577985, &g_fovOrig);
    if (!save) {   // without it, the FOV the game puts back would widen again each time
        Log("Widescreen: the FOV save at 0x627D39 is not v1.0; not installed");
        return;
    }
    Jump(SET_FOV, (void*)&vcmf_ws_setfov);
    Jump(CALC_ASPECT, (void*)&vcmf_ws_aspect);
    const int hud = RedirectFmul(0x697A70, &g_hudScaleX);
    const int radar = RedirectFmul(0x68FD14, &g_hudScaleX);
    const int darkel = RedirectFmul(0x6874E4, &g_hudScaleX);
    const int hud2 = RedirectFmul(0x697DC0, &g_hudScaleX);
    // The sniper scope: CHud draws its circle (scaled above), and Render2dStuff fills
    // the screen beside it with black, CSprite2d::DrawRect from the edge to the
    // centre -/+ 210 * the file's own 1/640 (0x68E5C0, fmul at 0x4A6417 and 0x4A646B).
    // Left stretched, the black stopped short of the narrower circle and the city
    // showed in two strips beside it. Top and bottom use the height and are left alone.
    const bool scopeL = RedirectFmulAt(0x4A6417, 0x68E5C0, &g_hudScaleX);
    const bool scopeR = RedirectFmulAt(0x4A646B, 0x68E5C0, &g_hudScaleX);
    FlushInstructionCache(GetCurrentProcess(), nullptr, 0);
    g_on = true;
    Log("Widescreen: picture fitted to %dx%d (%.2f:1): FOV widened x%.3f to the sides (Hor+), height as the game's 4:3; "
        "LOD distance %s, sprites %s",
        *(const int*)SCREEN_W, *(const int*)SCREEN_H, g_horScale * 4.0f / 3.0f, g_horScale,
        lod ? "kept" : "follows the wider FOV (code not v1.0)", spriteW && spriteH ? "kept round" : "not adjusted (code not v1.0)");
    Log("Widescreen: HUD and radar keep 4:3 proportions: %d HUD, %d radar, %d kill frenzy, %d other scale uses (v1.0: 87, 34, 8, 2); "
        "menus as the game draws them", hud, radar, darkel, hud2);
    Log("Widescreen: sniper scope's black sides %s", scopeL && scopeR ? "fitted to the 4:3 circle"
                                                     : "left as the game draws them (code not v1.0)");
}

// Near clip for wide views. The camera modes set the near clip plane
// (RwCameraSetNearClipPlane 0x64A860, cdecl (camera, near); 35 calls in the camera
// code), up to about 0.9 m, for the game's 70 degrees. A wider drawn view (a
// trainer's FOV slider at 120 makes about 131 degrees on 16:9) sees things at the
// edges of the screen much closer to the camera along its axis: the player's legs,
// low in the picture with the camera down and close behind them, were cut off by
// the near plane. With the drawn FOV (CDraw::ms_fFOV 0x696658, as the latest update
// left it) wider than the game's 70 widened for the screen's shape, the near plane
// is brought in by tan(normal / 2) / tan(drawn / 2), at most to a quarter. At the
// game's own FOV nothing changes.
static const DWORD NEAR_CLIP = 0x64A860;
typedef void* (__cdecl* NearClip_t)(void*, float);
static NearClip_t g_nearClipFn = nullptr;

static void* __cdecl HookNearClip(void* camera, float nearClip) {
    if (!g_cfg.wideNearClip) return g_nearClipFn(camera, nearClip);
    const int w = *(const int*)SCREEN_W, h = *(const int*)SCREEN_H;
    float s = (w > 0 && h > 0) ? (float)w / (float)h : 4.0f / 3.0f;
    if (s < 1.0f || s > 6.0f) s = 4.0f / 3.0f;
    const float tNormal = 0.7002075f * (g_on ? s * 0.75f : 1.0f);   // tan(35 deg), widened like the picture
    const float drawn = *FovVar;
    if (drawn > 1.0f && drawn < 175.0f) {
        const float tDrawn = __builtin_tanf(drawn * 0.5f * kDegToRad);
        if (tDrawn > tNormal * 1.01f) {
            float k = tNormal / tDrawn;
            if (k < 0.25f) k = 0.25f;
            nearClip *= k;
        }
    }
    return g_nearClipFn(camera, nearClip);
}

void InstallNearClip() {
    static const unsigned char head[] = { 0xD9, 0x44, 0x24, 0x08, 0x56, 0x8B, 0x74, 0x24, 0x08 };   // fld [esp+8]; push esi; mov esi,[esp+8]
    if (memcmp((void*)NEAR_CLIP, head, sizeof head)) {
        Log("NearClip: RwCameraSetNearClipPlane is not v1.0 or hooked by another mod; near clip as in the game");
        return;
    }
    unsigned char* t = (unsigned char*)VirtualAlloc(nullptr, 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!t) return;
    memcpy(t, head, 5);   // fld [esp+8]; push esi
    t[5] = 0xE9;
    *(int*)(t + 6) = (int)((NEAR_CLIP + 5) - ((DWORD)t + 10));
    g_nearClipFn = (NearClip_t)t;
    Jump(NEAR_CLIP, (void*)&HookNearClip);
    FlushInstructionCache(GetCurrentProcess(), nullptr, 0);
    Log("NearClip: near clip brought in for views wider than the game's (the player's legs were cut off at a wide FOV)");
}

void WidescreenFrame() {
    if (!g_on) return;
    const float before = g_horScale;
    UpdateShape();
    if (g_horScale != before)
        Log("Widescreen: screen is now %dx%d", *(const int*)SCREEN_W, *(const int*)SCREEN_H);
    const float scaled = (1.0f / 640.0f) / (g_horScale > 1.0f ? g_horScale : 1.0f);
    g_hudScaleX = *(volatile unsigned char*)MENU_ACTIVE ? 1.0f / 640.0f : scaled;
}
