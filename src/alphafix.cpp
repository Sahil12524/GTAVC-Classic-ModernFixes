// See-through leaves drawn in two passes ([Graphics] TwoPassAlpha).
//
// Palms, trees, bushes and fences are leaf/plank cards with a texture alpha. The game
// draws them with alpha blending and depth writes both on, so every pixel of a card
// writes depth, even where the texture is almost clear. A card drawn before the
// leaves behind it (in the same palm, or the next palm) blocks them there: the
// leaves behind are missing in a box around the near card's pale edges, and it looks
// like bits of the crown float in the sky.
//
// Each draw that blends and writes depth is done twice: first only its solid pixels
// (alpha test, alpha >= kSolidRef) with depth writes, then the rest (alpha < kSolidRef)
// blended without depth writes. Solid leaves hide what is behind them as before; the
// soft edges no longer hide anything. The game's device is a pure device (no
// GetRenderState), so SetRenderState (vtable 50) is hooked to keep the states this
// needs; after Reset they are back at Direct3D's defaults. They are put back exactly
// after the two passes, so RenderWare's own cache of them stays right.
#include "alphafix.h"
#include "config.h"
#include "timing.h"
#include <string.h>

static const DWORD kSolidRef = 0xC0;

typedef HRESULT (__stdcall* DrawPrim_t)(IDirect3DDevice8*, D3DPRIMITIVETYPE, UINT, UINT);
typedef HRESULT (__stdcall* DrawIndexed_t)(IDirect3DDevice8*, D3DPRIMITIVETYPE, UINT, UINT, UINT, UINT);
static DrawPrim_t g_origDP = nullptr;
static DrawIndexed_t g_origDIP = nullptr;
typedef HRESULT (__stdcall* SetRS_t)(IDirect3DDevice8*, D3DRENDERSTATETYPE, DWORD);
static SetRS_t g_origSetRS = nullptr;
static bool g_on = false;

// The water is drawn in two calls from RenderScene: CWaterLevel::RenderWater
// (0x5C1710, call at 0x4A6594: the sea sectors) and, after the opaque world, the
// see-through wavy layer near the camera (0x5BFF00, call at 0x4A65AE). Both are
// see-through with depth writes on, mostly below kSolidRef: split, their layers
// (surface, wave mask) stacked without depth and looked wrong. Their draws are left
// as the game makes them, and anisotropic filtering is off for them too
// (WaterDrawing in dllmain.cpp): the wave textures are made to blur at a shallow
// angle, as in the original game.
static bool g_suspended = false;
static void* g_waterFn[2] = { nullptr, nullptr };

// The wave mask (0x5BF0E0, from 0x5BF62E in 0x5BF540, which Idle's render half calls
// first, at 0x4A5E40: FixedTick's HookMirror calls WaterMaskStep there) is a 17 x 17 grid model
// (atomic at 0xA10860: +0x18 geometry, +0x5C morph target, +0x14 vertices) whose inner
// points it moves on every draw, keeping them from draw to draw (0x5BF30E..0x5BF385):
//   x = (x + n) * 0.99 + row * b,  y = (y + n') * 0.99 + column * b
// with n, n' = (rand() / 32768 * 2 - 1) * k and b = (1 - 0.99) * 2: a damped random
// walk, one step per draw. The original game takes 30 steps a second; at ~1000 fps it
// took 33 times as many: the waves near the camera ran far too fast and wobbled.
// Now it steps only on tick frames (as the game, with real rand() numbers), and the
// grid after each tick step is kept. On a redraw frame the two rand() calls (MaskRand)
// answer 16384, which makes n exactly 0, and the inner points are set beforehand to
// (kept - row/column * b) / 0.99, so the game's own step lands on the kept grid: the
// redraw frame draws the tick's pattern as it is, which is put back after the draw.
// At this point of the frame the clock and TheCamera are still the tick's (Interp
// moves them later, in ConstructRenderList). Redraw frames don't call rand(), so the
// game's random sequence is the same as without redraws.
static const int kMaskSide = 17, kMaskVerts = kMaskSide * kMaskSide;
static float g_maskCur[kMaskVerts][3], g_maskReal[kMaskVerts][3];
static bool g_maskHave = false;          // a tick grid is kept
static bool g_maskRedraw = false;
static bool g_maskOk = false;

extern "C" volatile unsigned char g_redraw;

static int __cdecl MaskRand() {
    if (g_maskRedraw) return 0x4000;      // n = (0.5 * 2 - 1) * k = 0
    return ((int (__cdecl*)())0x6499F0)();
}

static float* MaskVerts() {
    unsigned char* atomic = *(unsigned char**)0xA10860;
    if (!atomic) return nullptr;
    unsigned char* geo = *(unsigned char**)(atomic + 0x18);
    if (!geo || geo[0] != 8 || *(int*)(geo + 0x14) != kMaskVerts) return nullptr;   // rpGEOMETRY, 289 vertices
    unsigned char* mt = *(unsigned char**)(geo + 0x5C);
    return mt ? *(float**)(mt + 0x14) : nullptr;
}

void WaterMaskStep(void* fn) {
    if (!g_maskOk) { ((void (__cdecl*)())fn)(); return; }
    float* v = MaskVerts();
    if (!v) { ((void (__cdecl*)())fn)(); return; }
    if (!g_redraw) {   // tick frame: the game's own step
        g_maskRedraw = false;
        ((void (__cdecl*)())fn)();
        memcpy(g_maskCur, v, sizeof g_maskCur);
        g_maskHave = true;
        return;
    }
    memcpy(g_maskReal, v, sizeof g_maskReal);
    if (g_maskHave) {
        const long double b = (1.0L - (long double)0.99f) * 2.0L;   // as the game works it out (x87)
        for (int r = 1; r < kMaskSide - 1; r++)
            for (int c = 1; c < kMaskSide - 1; c++) {
                const int i = r * kMaskSide + c;
                for (int k = 0; k < 2; k++) {
                    const float target = g_maskCur[i][k];   // the latest tick's pattern
                    const long double base = (long double)(k == 0 ? r : c) * (float)b;
                    v[i * 3 + k] = (float)(((long double)target - base) / (long double)0.99f);
                }
            }
    }
    g_maskRedraw = true;
    ((void (__cdecl*)())fn)();
    g_maskRedraw = false;
    memcpy(v, g_maskReal, sizeof g_maskReal);
}

static void WaterCall(int i) {
    g_suspended = true;
    WaterDrawing(true);
    ((void (__cdecl*)())g_waterFn[i])();
    WaterDrawing(false);
    g_suspended = false;
}

static void __cdecl RenderWaterWhole() { WaterCall(0); }
static void __cdecl RenderWaterLayerWhole() { WaterCall(1); }

// The split is for the 3D world only: it is done while RenderScene (0x4A6570, called
// once a frame from 0x4A604A) draws, never for the menus, HUD and radar drawn after
// it. The vertex format check (HookedSetVS) alone missed the menu text with SkyGfx's
// mobile trails on (its colour filter changes the vertex shader behind RenderWare's
// cache): the text's soft edges were drawn over the next letters again.
static const DWORD RENDER_SCENE_SITE = 0x4A604A;
static void* g_renderSceneFn = nullptr;
static bool g_inWorld = false;
static void __cdecl RenderSceneWorld() {
    g_inWorld = true;
    ((void (__cdecl*)())g_renderSceneFn)();
    g_inWorld = false;
}

static void InstallSceneWrap() {
    unsigned char* call = (unsigned char*)RENDER_SCENE_SITE;
    if (g_renderSceneFn) return;
    if (call[0] != 0xE8) { Log("Graphics: RenderScene call is not v1.0; two-pass leaves off"); return; }
    g_renderSceneFn = (void*)((DWORD)call + 5 + *(int*)(call + 1));   // the game's, or another mod's in front of it
    DWORD old;
    VirtualProtect(call, 5, PAGE_EXECUTE_READWRITE, &old);
    *(int*)(call + 1) = (int)((DWORD)&RenderSceneWorld - ((DWORD)call + 5));
    VirtualProtect(call, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), call, 5);
}

static void InstallWaterWrap() {
    // rand: push ebx; push 1; call _getptd ... imul ebx, ebx, 41C64E6Dh; the mask calls it at 0x5BF30E, 0x5BF346
    {
        static const DWORD randCalls[2] = { 0x5BF30E, 0x5BF346 };
        bool ok = !memcmp((void*)0x6499F0, "\x53\x6A\x01\xE8", 4) && !memcmp((void*)0x6499FC, "\x69\xDB\x6D\x4E\xC6\x41", 6);
        for (DWORD c : randCalls) ok = ok && *(unsigned char*)c == 0xE8 && c + 5 + *(int*)(c + 1) == 0x6499F0;
        if (ok) {
            for (DWORD c : randCalls) {
                DWORD old;
                VirtualProtect((void*)c, 5, PAGE_EXECUTE_READWRITE, &old);
                *(int*)(c + 1) = (int)((DWORD)&MaskRand - (c + 5));
                VirtualProtect((void*)c, 5, old, &old);
                FlushInstructionCache(GetCurrentProcess(), (void*)c, 5);
            }
            g_maskOk = true;
        }
    }
    static const DWORD sites[2] = { 0x4A6594, 0x4A65AE };
    void* const wraps[2] = { (void*)&RenderWaterWhole, (void*)&RenderWaterLayerWhole };
    int n = 0;
    for (int i = 0; i < 2; i++) {
        unsigned char* call = (unsigned char*)sites[i];
        if (g_waterFn[i] || call[0] != 0xE8) continue;
        g_waterFn[i] = (void*)((DWORD)call + 5 + *(int*)(call + 1));   // the game's, or another mod's in front of it
        DWORD old;
        VirtualProtect(call, 5, PAGE_EXECUTE_READWRITE, &old);
        *(int*)(call + 1) = (int)((DWORD)wraps[i] - ((DWORD)call + 5));
        VirtualProtect(call, 5, old, &old);
        FlushInstructionCache(GetCurrentProcess(), call, 5);
        n++;
    }
    if (n) Log("Graphics: water (sea and wavy near layer) drawn as the game does (no two passes, no anisotropic filtering)");
    Log(g_maskOk ? "Graphics: water wave mask steps 30 times a second as in the game (its pattern held between steps)"
                 : "Graphics: rand / wave mask code is not v1.0; the wave mask's pattern changes every frame");
}

// The render states set so far (Direct3D's defaults after CreateDevice / Reset).
static DWORD g_rs[256];
static void DefaultStates() {
    memset(g_rs, 0, sizeof g_rs);
    g_rs[D3DRS_ZENABLE] = TRUE;            // auto depth buffer
    g_rs[D3DRS_ZWRITEENABLE] = TRUE;
    g_rs[D3DRS_ALPHAFUNC] = D3DCMP_ALWAYS;
}

static HRESULT __stdcall HookedSetRS(IDirect3DDevice8* dev, D3DRENDERSTATETYPE st, DWORD v) {
    if ((unsigned)st < 256) g_rs[st] = v;
    return g_origSetRS(dev, st, v);
}

// Only 3D draws are split. Screen-space drawing (RenderWare's Im2D: menus, HUD text,
// the radar) uses pre-transformed vertices (FVF XYZRHW). Split, the radar's mask -
// drawn with alpha 0 to write depth where the round radar hides its square map -
// wrote no depth (its pixels failed the solid pass), so the map's corners showed past
// the disc; and in text the soft glyph edges were drawn over the next glyphs' solid
// parts (the menu's coloured overlaps). SetVertexShader (vtable 76) gives the format:
// a fixed-function FVF, or a shader handle (never XYZRHW here).
typedef HRESULT (__stdcall* SetVS_t)(IDirect3DDevice8*, DWORD);
static SetVS_t g_origSetVS = nullptr;
static bool g_screenSpace = false;
static HRESULT __stdcall HookedSetVS(IDirect3DDevice8* dev, DWORD handle) {
    g_screenSpace = handle < 0x10000 && (handle & D3DFVF_POSITION_MASK) == D3DFVF_XYZRHW;
    return g_origSetVS(dev, handle);
}

void AlphaFixReset() { DefaultStates(); }
static unsigned g_twoPass = 0;   // draws done in two passes (logged once)

struct Saved { DWORD test, ref, func; };

// True if this draw should be split; sets up the first pass.
static bool BeginSolid(IDirect3DDevice8* dev, Saved& s) {
    if (!g_inWorld || g_suspended || g_screenSpace || !g_rs[D3DRS_ALPHABLENDENABLE] || !g_rs[D3DRS_ZWRITEENABLE] || !g_rs[D3DRS_ZENABLE]) return false;
    s.test = g_rs[D3DRS_ALPHATESTENABLE];
    s.ref = g_rs[D3DRS_ALPHAREF];
    s.func = g_rs[D3DRS_ALPHAFUNC];
    g_origSetRS(dev, D3DRS_ALPHATESTENABLE, TRUE);
    g_origSetRS(dev, D3DRS_ALPHAFUNC, D3DCMP_GREATEREQUAL);
    g_origSetRS(dev, D3DRS_ALPHAREF, kSolidRef);
    return true;
}

static void BeginSoft(IDirect3DDevice8* dev) {
    g_origSetRS(dev, D3DRS_ALPHAFUNC, D3DCMP_LESS);
    g_origSetRS(dev, D3DRS_ZWRITEENABLE, FALSE);
}

static void End(IDirect3DDevice8* dev, const Saved& s) {
    g_origSetRS(dev, D3DRS_ZWRITEENABLE, TRUE);
    g_origSetRS(dev, D3DRS_ALPHAREF, s.ref);
    g_origSetRS(dev, D3DRS_ALPHAFUNC, s.func);
    g_origSetRS(dev, D3DRS_ALPHATESTENABLE, s.test);
    TimingCount(TC_TWO_PASS);   // per frame in the Timing line
    if (++g_twoPass == 1) Log("Graphics: see-through draws done in two passes (first one seen)");
}

static HRESULT __stdcall HookedDP(IDirect3DDevice8* dev, D3DPRIMITIVETYPE t, UINT start, UINT count) {
    Saved s;
    if (!g_on || !g_cfg.twoPassAlpha || !BeginSolid(dev, s)) return g_origDP(dev, t, start, count);
    HRESULT hr = g_origDP(dev, t, start, count);
    BeginSoft(dev);
    g_origDP(dev, t, start, count);
    End(dev, s);
    return hr;
}

static HRESULT __stdcall HookedDIP(IDirect3DDevice8* dev, D3DPRIMITIVETYPE t, UINT minIdx, UINT num,
                                   UINT start, UINT count) {
    Saved s;
    if (!g_on || !g_cfg.twoPassAlpha || !BeginSolid(dev, s)) return g_origDIP(dev, t, minIdx, num, start, count);
    HRESULT hr = g_origDIP(dev, t, minIdx, num, start, count);
    BeginSoft(dev);
    g_origDIP(dev, t, minIdx, num, start, count);
    End(dev, s);
    return hr;
}

void InstallAlphaFix(IDirect3DDevice8* dev, DWORD createFlags, void* (*patch)(void*, int, void*)) {
    InstallWaterWrap();
    InstallSceneWrap();
    // installed even when off: the Modern Fix menu (fixmenu.cpp) turns it on and off
    // while playing; each draw checks g_cfg.twoPassAlpha
    DefaultStates();
    if (!g_origDIP) {
        g_origSetRS = (SetRS_t)patch(dev, 50, (void*)&HookedSetRS);   // SetRenderState
        g_origDP  = (DrawPrim_t)patch(dev, 70, (void*)&HookedDP);       // DrawPrimitive
        g_origDIP = (DrawIndexed_t)patch(dev, 71, (void*)&HookedDIP);   // DrawIndexedPrimitive
        g_origSetVS = (SetVS_t)patch(dev, 76, (void*)&HookedSetVS);     // SetVertexShader
    }
    g_screenSpace = false;
    g_on = true;
    Log("Graphics: see-through leaves (palms, trees, fences) drawn in two passes, solid at alpha >= %lu; screen-space "
        "drawing (menus, HUD, radar) left whole (%s)", kSolidRef,
        g_cfg.twoPassAlpha ? "on" : "off");
}
