// Light coronas and far city lights ([Graphics] FarLights, CoronaDistance).
//
// Three parts:
//
// 1. More corona slots. CCoronas keeps its coronas in aCoronas (0x815128, 56 slots
//    of 0x68 bytes, the id at +0xC, 0 = free); a corona that finds no free slot is
//    not shown. Only the corona code (0x542100..0x5446F0) uses the array: 146
//    operands address it (slot 0's fields, indexed by slot * 0x68) and 12 compares
//    hold the count (cmp ..., 38h with a one-byte immediate, so at most 127; two of
//    the loops handle 8 slots per step, so a multiple of 8). Before the game starts
//    (DllMain) the operands are pointed at an array of 120 slots here and the
//    compares at 120, all checked first: anything not as in v1.0 leaves the game's 56.
//
// 2. Corona distance for car lights and the rest. CCoronas::RegisterCorona
//    (0x542490, cdecl, 16 arguments; the 8th the distance it is seen from, the 5th
//    its brightness, the 15th "long distance" for the sun and moon) is called from 10
//    places (0x5427F4: the wrapper the other 60 go through: cars, traffic lights,
//    pickups, scripts). Those calls pass the distance times CoronaDistance (not the
//    sun's and moon's) and a brightness that is the game's up to half the game's
//    distance and falls off past it. The two calls of ProcessLightsForEntity (lamps,
//    windows and neon on buildings and objects, 0x5419E0 / 0x5421A5) are left exactly
//    as the game makes them: near lights look as in the game.
//
// 3. Far city lights. The game shows a building's or lamp post's lights only while
//    the entity is drawn, and only as coronas: 56 at once, each with a line-of-sight
//    test that blinks it on and off far away. Registering far lights as coronas too
//    (the previous way) ran out of slots, blinked and popped. Now far lights are
//    drawn here as glow sprites, steady, with no limit on their number: right after
//    CCoronas::Render (call at 0x4A657F, every drawn frame), with the corona render's
//    own sprite code - CSprite::InitSpriteBuffer 0x577770, CSprite::CalcScreenCoors
//    0x5778B0 (point, out screen point, out w, h, respect far clip),
//    CSprite::RenderBufferedOneXLUSprite 0x577350 (x, y, z, w, h, r, g, b, intensity,
//    1/z, alpha; 96 to a batch), CSprite::FlushSpriteBuffer 0x577790 - and its blend
//    (additive, RwRenderStateSet 0x649BA0: blend 2 / 2, vertex alpha on, no depth
//    write, no fog), the coronastar texture (gpCoronaTexture[0], 0x695538), but with
//    the depth test on: buildings in front hide them, without any line-of-sight test.
//    Every light (C2dEffect type 0: position +0, colour +0xC, distance +0x14, size
//    +0x1C, kind +0x24) of the buildings, treadables and dummies (lamp posts not
//    near enough to be objects) is listed once (their positions never change; listed
//    again when the pools are made again). Each frame a light is drawn when its
//    entity is in the current area, visible and upright (as ProcessLightsForEntity
//    asks: +0x53 bit 0 clear, +0x52 bit 2 set, up z at least 0.96), it is night
//    (kinds 0-11; 12-14 are off or special), it is in front within the far clip and
//    2.5 km, and it is further than 60% of its own corona distance: it comes in as
//    the game's corona fades out and is full beyond it. It stays a few pixels big,
//    pulled a little towards the camera so the low-detail building it sits on
//    doesn't hide it.
#include "farlights.h"
#include "config.h"
#include <windows.h>
#include <math.h>
#include <string.h>

// ---------------------------------------------------------------- 1. corona slots
static const DWORD CORONAS = 0x815128, CORONA_SIZE = 0x68;
static const int kGameSlots = 56, kSlots = 120;
static const DWORD CODE_LO = 0x542100, CODE_HI = 0x5446F0;
static const int kOperands = 146;
alignas(16) static unsigned char g_slots[kSlots * CORONA_SIZE];

struct CountSite { DWORD at; unsigned char len; unsigned char bytes[6]; };   // the immediate is the last byte
static const CountSite kCounts[] = {
    { 0x542434, 4, { 0x66, 0x83, 0xF8, 0x38 } }, { 0x542442, 4, { 0x66, 0x83, 0xF8, 0x38 } },
    { 0x5425B7, 4, { 0x66, 0x83, 0xFA, 0x38 } }, { 0x5425C5, 4, { 0x66, 0x83, 0xFA, 0x38 } },
    { 0x5425F4, 4, { 0x66, 0x83, 0xFA, 0x38 } }, { 0x542603, 4, { 0x66, 0x83, 0xFA, 0x38 } },
    { 0x542955, 4, { 0x66, 0x83, 0xF8, 0x38 } }, { 0x54310C, 3, { 0x83, 0xF8, 0x38 } },
    { 0x54349A, 4, { 0x83, 0x3C, 0x24, 0x38 } }, { 0x543E0C, 6, { 0x66, 0x83, 0x7C, 0x24, 0x38, 0x38 } },
    { 0x544086, 6, { 0x66, 0x83, 0x7C, 0x24, 0x34, 0x38 } }, { 0x544217, 3, { 0x83, 0xFB, 0x38 } },
};

static void WriteBytes(DWORD at, const void* p, size_t n) {
    DWORD old;
    VirtualProtect((void*)at, n, PAGE_EXECUTE_READWRITE, &old);
    memcpy((void*)at, p, n);
    VirtualProtect((void*)at, n, old, &old);
}

static const char* g_slotNote = "not checked yet";
void PatchCoronaSlotsEarly() {
    if (!g_cfg.farLights) { g_slotNote = "FarLights off: the game's 56"; return; }
    for (const CountSite& c : kCounts)
        if (memcmp((void*)c.at, c.bytes, c.len)) { g_slotNote = "corona code is not v1.0: the game's 56"; return; }
    static DWORD ops[kOperands + 1];
    int n = 0;
    for (DWORD p = CODE_LO; p + 4 <= CODE_HI; p++) {
        const DWORD v = *(const DWORD*)p;
        if (v >= CORONAS && v < CORONAS + kGameSlots * CORONA_SIZE) {
            if (n > kOperands) break;
            ops[n++] = p;
        }
    }
    if (n != kOperands) { g_slotNote = "corona code is not v1.0 (array uses): the game's 56"; return; }
    const DWORD delta = (DWORD)g_slots - CORONAS;
    for (int i = 0; i < n; i++) {
        const DWORD v = *(const DWORD*)ops[i] + delta;
        WriteBytes(ops[i], &v, 4);
    }
    for (const CountSite& c : kCounts) {
        const unsigned char k = (unsigned char)kSlots;
        WriteBytes(c.at + c.len - 1, &k, 1);
    }
    FlushInstructionCache(GetCurrentProcess(), nullptr, 0);
    g_slotNote = "120 (game: 56)";
}

// ---------------------------------------------------------------- 2. corona distance
static const DWORD REGISTER = 0x542490;
static const DWORD kRegSites[] = { 0x488C9D, 0x542304, 0x542382, 0x5427F4, 0x5C5011, 0x5C5052, 0x5C5161, 0x5C51A2 };
static const int kRegCount = sizeof kRegSites / sizeof kRegSites[0];
typedef void (__cdecl* Reg_t)(DWORD, DWORD, DWORD, DWORD, DWORD, const float*, float, float, void*, DWORD, DWORD,
                              DWORD, DWORD, float, DWORD, float);
static Reg_t g_regFn[kRegCount];
static const DWORD CAM_POS = 0x7E46B8;   // TheCamera's position

template <int I>
static void __cdecl HookRegister(DWORD id, DWORD r, DWORD g, DWORD b, DWORD a, const float* coors, float size, float dist,
                                 void* tex, DWORD flare, DWORD refl, DWORD los, DWORD streak, float angle, DWORD longDist,
                                 float nearDist) {
    const float k = g_cfg.coronaDistance;
    if (!longDist && dist > 1.0f && dist < 2000.0f && k > 1.0f && coors) {
        // the game's brightness up to half its distance, then down to 50% at its
        // distance and 20% at half the new one (the game's own fade does the rest)
        const float* c = (const float*)CAM_POS;
        const float dx = coors[0] - c[0], dy = coors[1] - c[1], dz = coors[2] - c[2];
        const float d = sqrtf(dx * dx + dy * dy + dz * dz), own = dist, reach = dist * k * 0.5f;
        float t = 1.0f;
        if (d > own * 0.5f && d <= own) t = 1.0f - 0.5f * (d - own * 0.5f) / (own * 0.5f);
        else if (d > own) t = reach > own ? 0.5f - 0.3f * fminf(1.0f, (d - own) / (reach - own)) : 0.2f;
        a = (DWORD)((float)(a & 0xFF) * t + 0.5f);
        dist *= k;
    }
    g_regFn[I](id, r, g, b, a, coors, size, dist, tex, flare, refl, los, streak, angle, longDist, nearDist);
}

static void* const kRegHooks[] = {
    (void*)&HookRegister<0>, (void*)&HookRegister<1>, (void*)&HookRegister<2>, (void*)&HookRegister<3>,
    (void*)&HookRegister<4>, (void*)&HookRegister<5>, (void*)&HookRegister<6>, (void*)&HookRegister<7> };
static_assert(sizeof kRegHooks / sizeof kRegHooks[0] == kRegCount, "one hook per call");

// ---------------------------------------------------------------- 3. far city lights
static const DWORD RENDER_SITE = 0x4A657F, CORONAS_RENDER = 0x542FF0;
static DWORD g_renderFn = CORONAS_RENDER;
static const DWORD MODEL_INFO = 0x92D4C8, CURR_AREA = 0x978810, CAM_FWD = 0x7E4698, HOURS = 0xA10B6B;
static const DWORD SCREEN_W = 0x9B48E4, SCREEN_H = 0x9B48E8, CORONA_TEX = 0x695538;
static const DWORD ENT_MODEL = 0x5C, ENT_AREA = 0x5F;
static const int kModels = 6500, kMaxLights = 32768, kMaxSprites = 4000;
static const float kMaxDist = 2500.0f;

static const auto Get2dEffect = (unsigned char* (__thiscall*)(void*, int))0x53F260;
static const auto RenderState = (int (__cdecl*)(int, int))0x649BA0;
static const auto InitSprites = (void (__cdecl*)())0x577770;
static const auto FlushSprites = (void (__cdecl*)())0x577790;
static const auto ScreenCoors = (bool (__cdecl*)(const float*, float*, float*, float*, bool))0x5778B0;
static const auto Sprite = (void (__cdecl*)(float, float, float, float, float, unsigned char, unsigned char, unsigned char,
                                           short, float, unsigned char))0x577350;

struct FarLight { float x, y, z, size, dist; unsigned char r, g, b, kind; unsigned char* ent; };
static FarLight g_lights[kMaxLights];
static int g_nLights = 0;
static unsigned char g_modelLight[kModels];   // 0 not checked, 1 no light, 2 has a light
static unsigned char* g_poolObjs[3];
static bool g_built = false;
static bool g_on = false;
static long g_drawn = 0, g_frames = 0;

static bool ModelHasLight(int id) {
    if (id < 0 || id >= kModels) return false;
    if (g_modelLight[id]) return g_modelLight[id] == 2;
    unsigned char* mi = *(unsigned char**)(MODEL_INFO + id * 4);
    bool light = false;
    if (mi) {
        const int n = mi[0x1A];
        for (int i = 0; i < n && !light; i++) {
            const unsigned char* e = Get2dEffect(mi, i);
            if (e && e[0x10] == 0) light = true;   // C2dEffect type 0: a light
        }
    }
    g_modelLight[id] = light ? 2 : 1;
    return light;
}

struct PoolInfo { DWORD pool; int size; };
static const PoolInfo kPools[] = { { 0x97F240, 0x64 }, { 0x978834, 0x64 }, { 0x97F25C, 0x68 } };   // buildings, treadables, dummies

static bool PoolsChanged() {
    for (int p = 0; p < 3; p++) {
        unsigned char* pl = *(unsigned char**)kPools[p].pool;
        unsigned char* objs = pl ? *(unsigned char**)pl : nullptr;
        if (objs != g_poolObjs[p]) return true;
    }
    return false;
}

static void Build() {
    int n = 0, ents = 0;
    for (int p = 0; p < 3; p++) {
        unsigned char* pl = *(unsigned char**)kPools[p].pool;
        g_poolObjs[p] = pl ? *(unsigned char**)pl : nullptr;
        if (!pl) continue;
        unsigned char* objs = *(unsigned char**)pl;
        const unsigned char* flags = *(unsigned char**)(pl + 4);
        const int count = *(int*)(pl + 8);
        if (!objs || !flags || count <= 0 || count > 100000) continue;
        for (int i = 0; i < count && n < kMaxLights; i++) {
            if (flags[i] & 0x80) continue;
            unsigned char* e = objs + i * kPools[p].size;
            const int model = *(short*)(e + ENT_MODEL);
            if (!ModelHasLight(model)) continue;
            ents++;
            unsigned char* mi = *(unsigned char**)(MODEL_INFO + model * 4);
            const float* m = (const float*)(e + 4);   // right, fwd, up, pos (16 bytes apart)
            const int fx = mi[0x1A];
            for (int k = 0; k < fx && n < kMaxLights; k++) {
                const unsigned char* fxp = Get2dEffect(mi, k);
                if (!fxp || fxp[0x10] != 0) continue;
                const unsigned char kind = fxp[0x24];
                const float dist = *(const float*)(fxp + 0x14), size = *(const float*)(fxp + 0x1C);
                if (kind >= 12 || !(dist > 0.0f && dist < 5000.0f) || !(size > 0.0f && size < 50.0f)) continue;
                if (fxp[0x0C] + fxp[0x0D] + fxp[0x0E] == 0) continue;
                const float* lp = (const float*)fxp;
                FarLight& l = g_lights[n++];
                l.x = m[12] + m[0] * lp[0] + m[4] * lp[1] + m[8] * lp[2];
                l.y = m[13] + m[1] * lp[0] + m[5] * lp[1] + m[9] * lp[2];
                l.z = m[14] + m[2] * lp[0] + m[6] * lp[1] + m[10] * lp[2];
                l.size = size;
                l.dist = dist;
                l.r = fxp[0x0C]; l.g = fxp[0x0D]; l.b = fxp[0x0E];
                l.kind = kind;
                l.ent = e;
            }
        }
    }
    g_nLights = n;
    static int logs = 0;
    if (logs++ < 3) Log("FarLights: %d far lights on %d buildings, treadables and lamp posts", n, ents);
}

// How much night it is: full from 20:00 to 05:59, easing in and out over the hours
// around dusk and dawn (game hours, CClock 0xA10B6B).
static float Night() {
    const int h = *(const unsigned char*)HOURS;
    if (h >= 20 || h <= 5) return 1.0f;
    if (h == 19 || h == 6) return 0.6f;
    if (h == 18 || h == 7) return 0.25f;
    return 0.0f;
}

static void DrawFarLights() {
    if (!g_on || !g_cfg.farLights || ReplayPlaying()) return;
    const float night = Night();
    if (night <= 0.0f) return;
    // listed once per set of pools (their entities never move); again after a load
    if (!g_built || PoolsChanged()) { g_built = true; Build(); return; }
    const unsigned char* tex = *(const unsigned char* const*)CORONA_TEX;
    if (!tex || !*(const void* const*)tex) return;   // texture, its raster
    const float* c = (const float*)CAM_POS;
    const float* f = (const float*)CAM_FWD;
    const int area = *(const int*)CURR_AREA;
    const float sw = (float)*(const int*)SCREEN_W, sh = (float)*(const int*)SCREEN_H;
    int drawn = 0;
    bool begun = false;
    for (int i = 0; i < g_nLights && drawn < kMaxSprites; i++) {
        const FarLight& l = g_lights[i];
        const float dx = l.x - c[0], dy = l.y - c[1], dz = l.z - c[2];
        if (dx * f[0] + dy * f[1] + dz * f[2] <= 0.0f) continue;   // behind
        const float d2 = dx * dx + dy * dy + dz * dz;
        const float start = l.dist * 0.6f > 30.0f ? l.dist * 0.6f : 30.0f;
        if (d2 < start * start || d2 > kMaxDist * kMaxDist) continue;
        const unsigned char* e = l.ent;
        const int ea = e[ENT_AREA];
        if ((ea != area && ea != 13) || (e[0x53] & 1) || !((e[0x52] >> 2) & 1) || *(const float*)(e + 0x2C) < 0.96f) continue;
        const float d = sqrtf(d2);
        // pulled towards the camera (1.5% of the distance, 1 - 10 m)
        float pull = d * 0.015f;
        pull = pull < 1.0f ? 1.0f : pull > 10.0f ? 10.0f : pull;
        const float p[3] = { l.x - dx / d * pull, l.y - dy / d * pull, l.z - dz / d * pull };
        float sc[3], w, h;
        if (!ScreenCoors(p, sc, &w, &h, true)) continue;
        if (sc[0] < -40.0f || sc[0] > sw + 40.0f || sc[1] < -40.0f || sc[1] > sh + 40.0f || !(sc[2] > 0.1f)) continue;
        const float fadeIn = l.dist > start ? fminf(1.0f, (d - start) / (l.dist - start)) : 1.0f;
        const float fadeOut = d > kMaxDist * 0.7f ? (kMaxDist - d) / (kMaxDist * 0.3f) : 1.0f;
        const float k = fadeIn * fadeOut * night * 0.9f;
        if (k < 0.02f) continue;
        float size = l.size;
        if (w * size < 2.5f && w > 0.0f) size = 2.5f / w;   // at least a few pixels
        if (size > l.size * 8.0f) size = l.size * 8.0f;
        if (!begun) {
            begun = true;
            RenderState(14, 0);   // fog
            RenderState(8, 0);    // depth write
            RenderState(6, 1);    // depth test
            RenderState(12, 1);   // vertex alpha
            RenderState(10, 2);   // source blend: one
            RenderState(11, 2);   // destination blend: one
            RenderState(1, (int)(DWORD) * (void* const*)tex);   // the coronastar raster
            InitSprites();
        }
        Sprite(sc[0], sc[1], sc[2], w * size, h * size, (unsigned char)(l.r * k), (unsigned char)(l.g * k),
               (unsigned char)(l.b * k), 255, 1.0f / sc[2], 255);
        drawn++;
    }
    if (begun) {
        FlushSprites();
        RenderState(10, 5);   // as CCoronas::Render leaves them (0x5434A9..)
        RenderState(11, 6);
        RenderState(12, 0);
        RenderState(8, 1);
        RenderState(6, 1);
    }
    g_drawn += drawn;
    g_frames++;
}

static void __cdecl HookCoronasRender() {
    ((void (__cdecl*)())g_renderFn)();
    DrawFarLights();
}

void FarLightsLog() {
    static DWORD at = 0;
    static int lines = 0;
    const DWORD now = GetTickCount();
    if (!g_on || lines >= 10 || now - at < 120000) return;
    at = now;
    if (!g_frames) return;
    lines++;
    Log("FarLights: %.0f far lights drawn per frame on average (%ld frames, %d listed)",
        (double)g_drawn / g_frames, g_frames, g_nLights);
    g_drawn = g_frames = 0;
}

// ---------------------------------------------------------------- install
static DWORD CallTarget(DWORD site) {
    const unsigned char* p = (const unsigned char*)site;
    return p[0] == 0xE8 ? site + 5 + *(const int*)(p + 1) : 0;
}
static void RedirectCall(DWORD site, void* to) {
    unsigned char call[5] = { 0xE8 };
    *(int*)(call + 1) = (int)((DWORD)to - (site + 5));
    WriteBytes(site, call, 5);
}

void InstallFarLights() {
    // The hooks go in either way; FarLights and CoronaDistance are read every call, so
    // the Modern Fix menu can switch them while playing (corona slots only at start).
    Log("FarLights: corona slots %s; far city lights %s now", g_slotNote, g_cfg.farLights ? "on" : "off");
    int regs = 0;
    for (int i = 0; i < kRegCount; i++) {
        const DWORD t = CallTarget(kRegSites[i]);
        if (!t) { g_regFn[i] = (Reg_t)REGISTER; continue; }
        g_regFn[i] = (Reg_t)t;
        RedirectCall(kRegSites[i], kRegHooks[i]);
        regs++;
    }
    static const unsigned char spriteStart[] = { 0x53, 0x55, 0x83, 0xEC, 0x50 };   // RenderBufferedOneXLUSprite
    const DWORD rt = CallTarget(RENDER_SITE);
    if (rt && !memcmp((void*)0x577350, spriteStart, sizeof spriteStart) && *(const unsigned char*)0x5778B0 == 0x53 &&
        *(const unsigned char*)0x577790 == 0x83) {
        g_renderFn = rt;
        RedirectCall(RENDER_SITE, (void*)&HookCoronasRender);
        g_on = true;
    }
    FlushInstructionCache(GetCurrentProcess(), nullptr, 0);
    Log("FarLights: car and other lights seen x%.1f further (%d of %d calls); building and lamp lights near as in the game; "
        "far city lights %s", g_cfg.coronaDistance, regs, kRegCount,
        g_on ? "drawn as steady glows at night (up to 2.5 km)" : "off (corona render code not v1.0)");
}
