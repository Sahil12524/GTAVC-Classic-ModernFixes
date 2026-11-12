// Fixed-tick simulation, frames drawn in between ("redraw frames").
//
// The game's Idle() (0x4A5D80) does, once per frame:
//   CTimer::Update -> per-frame inits -> CGame::Process (physics, AI, scripts,
//   camera, input) -> DMAudio.Service -> render (mirrors, ConstructRenderList,
//   entity PreRender, RenderScene, effects, HUD, fade) -> Present.
//
// Here the whole Idle() runs only TickRate times per second ("tick"), so the
// game world advances exactly as in the original game running at that rate.
// Between ticks, "redraw frames" run only the render half of Idle(), entered at
// 0x4A5DE3, and must leave no trace in the game state. The render half was
// analysed statically for everything it changes (README: "How redraw frames
// stay invisible to the game"); on redraw frames:
//   * CTimer's time step is 0, so every timer/animation driven by it stands still;
//   * the global state the render half writes (kRedrawState) is drawn from the
//     values the last tick left (HUD message state machines, flash counters, blur
//     fade...) and every byte is put back afterwards;
//   * lists and request flags filled before drawing and used up while drawing
//     (kRedrawLists, alpha-sorted entity lists) get the values from before the
//     tick drew, at the same two points: before the mirror pass, after PreRender;
//   * ConstructRenderList and the entity PreRender pass are skipped (their
//     results are those restored lists);
//   * the RNG seed is saved and restored;
//   * particles and floating water objects can't be spawned, and the helicopter
//     rotor (advanced by a constant per draw) doesn't advance.
// In phase 1 a redraw frame therefore draws the same image as its tick; phase 2
// will move the camera and objects between ticks.
#include "fixedtick.h"
#include "config.h"
#include "redraw_state.h"
#include "interp.h"
#include "freecam.h"
#include "quicksave.h"
#include "phoneskip.h"
#include "interior.h"
#include "gameplay.h"
#include "watchdog.h"
#include <windows.h>
#include <string.h>

// ---------------------------------------------------------------- addresses (v1.0)
static const DWORD IDLE              = 0x4A5D80;
static const DWORD IDLE_CALL_SITE    = 0x4A5BE0;   // AppEventHandler: call Idle
static const DWORD IDLE_RENDER_HALF  = 0x4A5DE3;   // after CGame::Process / audio / demo check
static const DWORD MIRROR_SITE       = 0x4A5E40, MIRROR_FN     = 0x5BF540;  // CMirrors::BeforeMainRender
static const DWORD CRL_SITE          = 0x4A5E45, CRL_FN        = 0x4CA260;  // CRenderer::ConstructRenderList
static const DWORD PRERENDER_SITE    = 0x4A5E4A, PRERENDER_FN  = 0x4CA1F0;  // CRenderer::PreRender
static const DWORD SPRITE_INIT       = 0x578930;   // CSprite2d::InitPerFrame
static const DWORD FONT_INIT         = 0x5522B0;   // CFont::InitPerFrame
static const DWORD ADD_PARTICLE      = 0x563CC0;   // CParticle::AddParticle (all overloads end here)
static const DWORD WATER_OBJ_A       = 0x5BB570;   // water renderer: spawn floating object
static const DWORD WATER_OBJ_B       = 0x636D20;   // water renderer: spawn floating object
static const DWORD RNG_STATE         = 0x66A2C0;   // returns per-thread data; RNG seed at +8
static const DWORD PLAY_ONE_SHOT     = 0x5F9DA0;   // DMAudio one-shot sound (ped PreRender), ret 0Ch

static float* const TimeStep         = (float*)0x975424;   // CTimer::ms_fTimeStep
static float* const TimeStepNC       = (float*)0x9753A8;   // CTimer::ms_fTimeStepNonClipped
static float* const RotorStep        = (float*)0x69C144;   // CHeli::Render rotor angle step (only user)
static volatile unsigned char* const MenuActive   = (unsigned char*)0x869668;
static volatile unsigned char* const UserPause    = (unsigned char*)0xA10B36;  // CTimer::m_UserPause
static volatile unsigned char* const WantRestart  = (unsigned char*)0x869669;
static volatile unsigned char* const WantLoad     = (unsigned char*)0xA10B6E;
static volatile unsigned char* const SpecialDraw  = (unsigned char*)0xA10B9C;  // gates 0x544797 (pads+draw)

// CLinkList<T> (0x44 bytes: 4 sentinel links + node array at +0x40), 16-byte nodes.
struct ListInfo { DWORD addr; unsigned capacity; };
static const ListInfo kAlphaLists[] = {
    { 0xA0FD84, 200 }, { 0x97F2F8, 20 }, { 0x978D44, 30 },   // cleared in ConstructRenderList
};

// ---------------------------------------------------------------- state
extern "C" volatile unsigned char g_redraw = 0;   // read by the generated stubs
static bool g_rotorOk = false;   // the game's helicopter rotor code is there (0x5AF226)

// 3D markers (the pink/yellow rings, cylinders and arrows; C3dMarkers, 32 x 0x78
// bytes at 0x814018). C3dMarkers::Render (0x570BA0) draws each marker placed since
// the last render (+0x52) and clears that flag, and deletes any marker that has a
// model (+0x48) but wasn't placed. Markers placed by the game logic (scripts: shop
// and mission rings) are placed once per tick, so the first redraw after a tick
// would delete them and the next tick create them again - their spin and pulse
// restarting 30 times a second. A redraw flags every live marker as placed (drawn,
// not deleted) and puts the live markers back exactly as they were afterwards.
static const DWORD MARKERS = 0x814018, MARKER_SIZE = 0x78, NUM_MARKERS = 32;
static const DWORD MARKER_MODEL = 0x48, MARKER_PLACED = 0x52;
static bool g_markersOk = false;   // C3dMarkers::Render is the game's own code
static unsigned char g_markerSave[NUM_MARKERS * MARKER_SIZE];
static bool g_markerLive[NUM_MARKERS];

static void MarkersBeginRedraw() {
    if (!g_markersOk) return;
    unsigned char* m = (unsigned char*)MARKERS;
    memcpy(g_markerSave, m, sizeof g_markerSave);
    for (DWORD i = 0; i < NUM_MARKERS; i++, m += MARKER_SIZE) {
        g_markerLive[i] = *(void**)(m + MARKER_MODEL) != nullptr;
        if (g_markerLive[i]) m[MARKER_PLACED] = 1;
    }
}

static void MarkersEndRedraw() {
    if (!g_markersOk) return;
    // Live markers were not deleted (they were flagged), so their model pointers
    // are unchanged; a marker first created during the redraw (a radar blip marker
    // placed by the HUD) is kept as the game left it.
    for (DWORD i = 0; i < NUM_MARKERS; i++)
        if (g_markerLive[i])
            memcpy((unsigned char*)MARKERS + i * MARKER_SIZE, g_markerSave + i * MARKER_SIZE, MARKER_SIZE);
}
static bool g_snapValid = false;                  // last tick rendered the 3D scene
static LARGE_INTEGER g_freq;
static LONGLONG g_period = 0, g_nextTick = 0;
static HANDLE g_timer = nullptr;

static const unsigned kNumLists = sizeof kAlphaLists / sizeof kAlphaLists[0];
static const unsigned kListBytes = 0x44 + 200 * 16;
struct Snapshot {
    unsigned char lists[kNumLists][kListBytes];
    unsigned char state[kRedrawStateBytes];   // large enough for either table
};
static Snapshot g_before;   // tick, before the mirror pass        (kRedrawLists + lists)
static Snapshot g_prePR;    // tick, right before PreRender            (kRedrawLists)
static unsigned char g_prePRState[kPreRenderStateBytes];    // tick, right before PreRender
static unsigned char g_afterPRState[kPreRenderStateBytes];  // redraw frame start
static Snapshot g_after;    // redraw frame start = what the tick left (kRedrawState + lists)
static void (*g_redrawEntry)(void* arg) = nullptr;

// ConstructRenderList's output as the real pass (game camera) left it this tick.
static unsigned char g_crlTrue[kCrlStateBytes];
static bool g_crlValid = false;     // g_crlTrue / entity flags saved this tick
static bool g_interpTick = false;   // this tick draws blended (normal gameplay)
static bool g_interpOn = false;     // blended camera/objects currently applied
static LONGLONG g_tickStart = 0;    // QPC when the latest tick ran
static LONGLONG g_prevTickStart = 0;

// Where between the previous and the latest tick a frame drawn now sits. Frames
// show the moment one tick length ago on a steady clock, measured against when
// the two ticks really ran, so the jitter of tick start times (up to a frame)
// never shows as uneven motion.
static float BlendAlpha(LONGLONG now) {
    LONGLONG span = g_tickStart - g_prevTickStart;
    if (span <= 0) return 1.0f;
    return (float)(now - g_period - g_prevTickStart) / (float)span;
}

// ---------------------------------------------------------------- snapshots
template <unsigned N>
static void Save(Snapshot& s, const StateRange (&table)[N], bool lists = true) {
    unsigned char* p = s.state;
    for (const StateRange& r : table) { memcpy(p, (void*)r.addr, r.size); p += r.size; }
    for (unsigned i = 0; lists && i < kNumLists; i++) {
        const ListInfo& l = kAlphaLists[i];
        memcpy(s.lists[i], (void*)l.addr, 0x44);
        if (void* nodes = *(void**)(l.addr + 0x40)) memcpy(s.lists[i] + 0x44, nodes, l.capacity * 16);
    }
}

template <unsigned N>
static void Load(const Snapshot& s, const StateRange (&table)[N], bool lists = true) {
    const unsigned char* p = s.state;
    for (const StateRange& r : table) { memcpy((void*)r.addr, p, r.size); p += r.size; }
    for (unsigned i = 0; lists && i < kNumLists; i++) {
        const ListInfo& l = kAlphaLists[i];
        memcpy((void*)l.addr, s.lists[i], 0x44);
        if (void* nodes = *(void**)(l.addr + 0x40)) memcpy(nodes, s.lists[i] + 0x44, l.capacity * 16);
    }
}

template <unsigned N>
static void SaveRanges(unsigned char* buf, const StateRange (&table)[N]) {
    for (const StateRange& r : table) { memcpy(buf, (void*)r.addr, r.size); buf += r.size; }
}
template <unsigned N>
static void LoadRanges(const unsigned char* buf, const StateRange (&table)[N]) {
    for (const StateRange& r : table) { memcpy((void*)r.addr, buf, r.size); buf += r.size; }
}

// Undo everything a pass with the blended camera changed in ConstructRenderList's
// output, so game logic reads what the real pass computed.
static void RestoreRealRenderList() {
    if (!g_crlValid) return;
    LoadRanges(g_crlTrue, kCrlState);
    EntityFlagsRestore();
}

// ---------------------------------------------------------------- hooks in the render half
// CMirrors::BeforeMainRender is the first thing the render half does.
// What each hooked call site called before VCModernFix: the game's function, or
// another mod's hook on it (SilentPatch and other ASI plugins are in place by then), which
// is then called in turn.
static DWORD g_idleFn = IDLE, g_mirrorFn = MIRROR_FN, g_crlFn = CRL_FN, g_preRenderFn = PRERENDER_FN;

static void __cdecl HookMirror() {
    if (!g_redraw) FreeCamAfterUpdate();   // trainer fly modes: vehicle keeps its heading
    if (g_redraw) Load(g_before, kRedrawLists);
    else          Save(g_before, kRedrawLists);
    ((void(__cdecl*)())g_mirrorFn)();
}

// Tick: the real pass (game camera) as in the original game, its result saved for
// game logic; then, if blending, the camera and objects are moved to where this
// frame draws them and the list is built again for that view.
// Redraw frame: the camera is already blended (Redraw), build the list for it.
static void __cdecl HookConstructRenderList() {
    ((void(__cdecl*)())g_crlFn)();
    if (g_redraw || !g_interpTick) return;
    SaveRanges(g_crlTrue, kCrlState);
    EntityFlagsSave();
    g_crlValid = true;
    InterpCapture();
    InterpApply(BlendAlpha(g_tickStart), true);   // about the previous tick; redraw frames move on from it
    g_interpOn = true;
    ((void(__cdecl*)())g_crlFn)();
}

// The entity PreRender pass runs on every drawn frame, so shadows, lights,
// coronas, wheels and bones follow the blended positions. On a redraw frame it
// starts from the values the tick had right before its own PreRender (lists it
// fills, counts, temporaries); its sounds are blocked, particles and water props
// can't spawn, the RNG and everything else are put back after the frame.
static void __cdecl HookPreRender() {
    if (!g_redraw) {
        Save(g_prePR, kRedrawLists, false);
        SaveRanges(g_prePRState, kPreRenderState);
        g_snapValid = true;
    } else {
        Load(g_prePR, kRedrawLists, false);   // alpha lists were just rebuilt for this view
        LoadRanges(g_prePRState, kPreRenderState);
    }
    ((void(__cdecl*)())g_preRenderFn)();
    if (!g_redraw) {
        if (!g_interpOn) return;
        InterpPartsCapture();
    }
    InterpPartsApply();   // doors, wheels, bonnets... between the two ticks
}

// ---------------------------------------------------------------- main loop
static void WaitUntil(LONGLONG target) {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    LONGLONG remaining = target - now.QuadPart;
    const LONGLONG margin = g_freq.QuadPart / 2000;   // spin the last 0.5 ms
    if (g_timer && remaining > margin) {
        LARGE_INTEGER due;
        due.QuadPart = -((remaining - margin) * 10000000LL / g_freq.QuadPart);
        if (SetWaitableTimer(g_timer, &due, 0, nullptr, nullptr, FALSE)) WaitForSingleObject(g_timer, INFINITE);
    }
    do { YieldProcessor(); QueryPerformanceCounter(&now); } while (now.QuadPart < target);
}

static bool Gameplay(void* arg) {
    return arg && !*MenuActive && !*WantRestart && !*WantLoad && !*SpecialDraw;
}

static bool CanRedraw(void* arg) {
    return Gameplay(arg) && g_snapValid && g_crlValid;
}

static void Redraw(void* arg) {
    const float ts = *TimeStep, tsNC = *TimeStepNC, rotor = *RotorStep;
    int* seed = nullptr;
    if (char* ptd = ((char*(__cdecl*)(int))RNG_STATE)(1)) seed = (int*)(ptd + 8);
    const int savedSeed = seed ? *seed : 0;

    Save(g_after, kRedrawState);   // everything the render half may change
    SaveRanges(g_afterPRState, kPreRenderState);
    g_redraw = 1;
    *TimeStep = 0.0f;
    *TimeStepNC = 0.0f;
    if (g_rotorOk) *RotorStep = 0.0f;
    ((void(__cdecl*)())SPRITE_INIT)();
    ((void(__cdecl*)())FONT_INIT)();
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    InterpApply(BlendAlpha(now.QuadPart), false);
    MarkersBeginRedraw();
    g_redrawEntry(arg);            // Idle()'s render half; returns through Idle()'s epilogue
    MarkersEndRedraw();
    InterpRestore(false);
    RestoreRealRenderList();
    LoadRanges(g_afterPRState, kPreRenderState);
    Load(g_after, kRedrawState);   // ...and all of it put back
    if (g_rotorOk) *RotorStep = rotor;
    *TimeStepNC = tsNC;
    *TimeStep = ts;
    if (seed) *seed = savedSeed;
    g_redraw = 0;
}

static void __cdecl HookIdle(void* arg) {
    g_heartbeat++;
    g_watchdogArmed = true;
    QuickSaveFrame();
    PhoneSkipFrame();
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (g_nextTick == 0) g_nextTick = now.QuadPart;

    // Pause menu: the game clock is stopped (CTimer::m_UserPause), so nothing in
    // the world moves; run the whole frame at full rate like the start menu.
    if (*MenuActive && *UserPause) {
        g_nextTick = now.QuadPart;   // first frame after unpausing is a tick
        g_snapValid = g_crlValid = g_interpTick = false;
        InterpReset();
        ((void(__cdecl*)(void*))g_idleFn)(arg);
        return;
    }

    if (now.QuadPart < g_nextTick) {
        if (CanRedraw(arg)) { Redraw(arg); return; }
        WaitUntil(g_nextTick);   // menu, loading, fades: run at the tick rate like the original
        QueryPerformanceCounter(&now);
    }
    // Tick. Late by a whole interval or more (a slow frame: streaming, alt-tab...)?
    // Restart the schedule from now instead of running catch-up ticks back to back
    // (each with a tiny time step, which shows as a stall then a lurch); the game
    // measures real elapsed time, so the next tick covers the gap, as in the
    // original game after a slow frame. Far behind: also don't blend across it.
    g_nextTick += g_period;
    if (now.QuadPart - g_nextTick > 4 * g_period) {
        g_nextTick = now.QuadPart + g_period;
        InterpReset();
    } else if (g_nextTick <= now.QuadPart) {
        g_nextTick = now.QuadPart + g_period;
    }
    g_prevTickStart = g_tickStart;
    g_tickStart = now.QuadPart;
    g_snapValid = g_crlValid = false;
    g_interpTick = Gameplay(arg);
    if (!g_interpTick) InterpReset();
    QuickSaveBeforeTick();
    PhoneSkipBeforeTick();
    GameplayBeforeTick();   // turret aim, mission leftovers
    ((void(__cdecl*)(void*))g_idleFn)(arg);
    if (g_interpOn) {
        InterpRestore(true);
        RestoreRealRenderList();
        g_interpOn = false;
    }
    InteriorEndOfTick();    // the drawn area follows the player (teleports, jetpack)
    QuickSaveEndOfTick();   // F5 / F9: the frame is drawn, everything is back to the real state
}

// ---------------------------------------------------------------- patching
static bool Matches(DWORD addr, const unsigned char* bytes, int len) { return memcmp((void*)addr, bytes, len) == 0; }

// Target of the call instruction at `site`, or 0 if it isn't a call.
static DWORD CallTarget(DWORD site) {
    const unsigned char* p = (const unsigned char*)site;
    return p[0] == 0xE8 ? site + 5 + *(const int*)(p + 1) : 0;
}

// A call site VCModernFix hooks: it must still be a call; what it calls (the game's
// function or another mod's hook on it) is kept and called by VCModernFix in turn.
static bool ChainCall(DWORD site, DWORD original, DWORD* out, const char* what) {
    DWORD t = CallTarget(site);
    if (!t) { Log("FixedTick: %s call at %06X was replaced by another mod (not a call)", what, site); return false; }
    if (t != original) Log("FixedTick: %s at %06X is hooked by another mod (%08X); chaining to it", what, site, t);
    *out = t;
    return true;
}

// A function entry VCModernFix hooks: the game's original bytes, or another mod's
// 5-byte jmp hook (which is then relocated into VCModernFix's stub and still runs).
static bool EntryOk(DWORD fn, const unsigned char* bytes, int len, const char* what) {
    if (memcmp((void*)fn, bytes, len) == 0) return true;
    if (*(const unsigned char*)fn == 0xE9) { Log("FixedTick: %s at %06X is hooked by another mod; chaining to it", what, fn); return true; }
    Log("FixedTick: %s at %06X was changed by another mod in an unknown way", what, fn);
    return false;
}

static void WriteCode(DWORD addr, const void* bytes, int len) {
    DWORD old;
    VirtualProtect((void*)addr, len, PAGE_EXECUTE_READWRITE, &old);
    memcpy((void*)addr, bytes, len);
    VirtualProtect((void*)addr, len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)addr, len);
}

static void RedirectCall(DWORD site, void* target) {
    unsigned char call[5] = { 0xE8 };
    *(int*)(call + 1) = (int)((DWORD)target - (site + 5));
    WriteCode(site, call, 5);
}

static unsigned char* g_code = nullptr;   // executable block for the generated stubs
static int g_codeUsed = 0;
static unsigned char* Emit(const void* bytes, int len) {
    unsigned char* p = g_code + g_codeUsed;
    memcpy(p, bytes, len);
    g_codeUsed += len;
    return p;
}

// fn's first `len` bytes (whole instructions, no relative operands) are moved
// into a stub that returns 0 while g_redraw is set, else runs them and jumps
// back into fn. `argBytes`: stack bytes the function itself pops (ret N), 0 for cdecl.
static void SkipDuringRedraw(DWORD fn, int len, unsigned short argBytes = 0) {
    unsigned char* stub = g_code + g_codeUsed;
    unsigned char head[] = {
        0x80, 0x3D, 0, 0, 0, 0, 0x00,   // cmp byte ptr [g_redraw], 0
        0x74, 0x05,                     // je  original
        0x31, 0xC0,                     // xor eax, eax
        0xC2, 0, 0,                     // ret argBytes
    };
    *(DWORD*)(head + 2) = (DWORD)&g_redraw;
    *(unsigned short*)(head + 12) = argBytes;
    Emit(head, sizeof head);
    unsigned char jmp[5] = { 0xE9 };
    if (*(const unsigned char*)fn == 0xE9) {
        // another mod hooked this entry with a jmp: go where it goes
        DWORD theirs = fn + 5 + *(const int*)(fn + 1);
        *(int*)(jmp + 1) = (int)(theirs - ((DWORD)(g_code + g_codeUsed) + 5));
        Emit(jmp, 5);
        len = 5;
    } else {
        Emit((void*)fn, len);
        *(int*)(jmp + 1) = (int)((fn + len) - ((DWORD)(g_code + g_codeUsed) + 5));
        Emit(jmp, 5);
    }

    unsigned char patch[16];
    patch[0] = 0xE9;
    *(int*)(patch + 1) = (int)((DWORD)stub - (fn + 5));
    for (int i = 5; i < len; i++) patch[i] = 0x90;
    WriteCode(fn, patch, len);
}

bool InstallFixedTick() {
    if (!g_cfg.fixedTick) { Log("FixedTick: disabled in VCModernFix.ini"); return false; }

    // Everything is checked before anything is patched: all or nothing.
    static const unsigned char idleHead[]    = { 0x53, 0x56, 0x57, 0x55, 0x83, 0xEC, 0x20 };
    static const unsigned char renderHalf[]  = { 0x80, 0x3D, 0x69, 0x96, 0x86, 0x00, 0x01 };  // cmp [0x869669], 1
    static const unsigned char particle[]    = { 0x53, 0x56, 0x57, 0x55, 0x81, 0xEC, 0x98, 0x00, 0x00, 0x00 };
    static const unsigned char waterObj[]    = { 0x66, 0x83, 0x3D, 0x64, 0x0A, 0xA1, 0x00, 0x28 };
    static const unsigned char rotor[]       = { 0xD8, 0x05, 0x44, 0xC1, 0x69, 0x00 };          // fadd [0x69C144]
    static const unsigned char oneShot[]     = { 0x83, 0xEC, 0x08, 0x8B, 0x54, 0x24, 0x10 };    // sub esp,8; mov edx,[esp+10h]
    // Idle()'s own code must be the game's: redraw frames enter its render half.
    if (!Matches(IDLE, idleHead, sizeof idleHead) || !Matches(IDLE_RENDER_HALF, renderHalf, sizeof renderHalf)) {
        Log("FixedTick: Idle() (0x4A5D80) is not the v1.0 code (other exe version, or changed by another mod); not installed");
        return false;
    }
    bool ok = ChainCall(IDLE_CALL_SITE, IDLE, &g_idleFn, "Idle call")
           && ChainCall(MIRROR_SITE, MIRROR_FN, &g_mirrorFn, "mirror pass")
           && ChainCall(CRL_SITE, CRL_FN, &g_crlFn, "ConstructRenderList")
           && ChainCall(PRERENDER_SITE, PRERENDER_FN, &g_preRenderFn, "PreRender")
           && EntryOk(ADD_PARTICLE, particle, sizeof particle, "AddParticle")
           && EntryOk(WATER_OBJ_A, waterObj, sizeof waterObj, "water object spawn A")
           && EntryOk(WATER_OBJ_B, waterObj, sizeof waterObj, "water object spawn B")
           && EntryOk(PLAY_ONE_SHOT, oneShot, sizeof oneShot, "ped one-shot sound");
    if (!ok) { Log("FixedTick: not installed (see above)"); return false; }
    // Helicopter rotor step: only handled if the game's own code is there.
    g_rotorOk = Matches(0x5AF226, rotor, sizeof rotor);
    if (!g_rotorOk) Log("FixedTick: helicopter rotor code changed by another mod; rotor left to it");
    // C3dMarkers::Render: push ebx/esi/ebp; mov [NumActiveMarkers],0; call; xor ebx,ebx;
    // mov ebp,aMarkerArray; ... cmp byte [ebp+52h],0 ... cmp dword [ebp+48h],0
    static const unsigned char markerRender[] = { 0x53, 0x56, 0x55, 0xC7, 0x05, 0x10, 0x8E, 0x97, 0x00, 0x00, 0x00, 0x00, 0x00 };
    static const unsigned char markerLoop[] = { 0xBD, 0x18, 0x40, 0x81, 0x00 };
    static const unsigned char markerFlag[] = { 0x80, 0x7D, 0x52, 0x00 };
    static const unsigned char markerModel[] = { 0x83, 0x7D, 0x48, 0x00 };
    g_markersOk = Matches(0x570BA0, markerRender, sizeof markerRender) && Matches(0x570BB4, markerLoop, sizeof markerLoop)
               && Matches(0x570BC0, markerFlag, sizeof markerFlag) && Matches(0x570C51, markerModel, sizeof markerModel);
    if (!g_markersOk) Log("FixedTick: 3D marker code changed by another mod; markers left to it");

    g_code = (unsigned char*)VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!g_code) { Log("FixedTick: VirtualAlloc failed"); return false; }

    // Entry into Idle()'s render half: rebuild Idle()'s frame, then jump in.
    unsigned char entry[12];
    memcpy(entry, idleHead, sizeof idleHead);
    entry[7] = 0xE9;
    unsigned char* e = Emit(entry, 12);
    *(int*)(e + 8) = (int)(IDLE_RENDER_HALF - ((DWORD)e + 12));
    g_redrawEntry = (void (*)(void*))e;

    SkipDuringRedraw(ADD_PARTICLE, sizeof particle);
    SkipDuringRedraw(WATER_OBJ_A, sizeof waterObj);
    SkipDuringRedraw(WATER_OBJ_B, sizeof waterObj);
    SkipDuringRedraw(PLAY_ONE_SHOT, sizeof oneShot, 0x0C);   // thiscall, 3 arguments
    DWORD old;
    VirtualProtect(RotorStep, sizeof(float), PAGE_READWRITE, &old);   // .data, already writable

    RedirectCall(MIRROR_SITE, (void*)&HookMirror);
    RedirectCall(CRL_SITE, (void*)&HookConstructRenderList);
    RedirectCall(PRERENDER_SITE, (void*)&HookPreRender);

    QueryPerformanceFrequency(&g_freq);
    g_period = g_freq.QuadPart / g_cfg.tickRate;
    g_timer = CreateWaitableTimerExW(nullptr, nullptr, 0x00000002 /* HIGH_RESOLUTION */, TIMER_ALL_ACCESS);
    if (!g_timer) g_timer = CreateWaitableTimerW(nullptr, TRUE, nullptr);

    RedirectCall(IDLE_CALL_SITE, (void*)&HookIdle);   // last: switches the game over
    Log("FixedTick: installed, %d ticks per second, %u bytes of render state tracked (%u in lists)",
        g_cfg.tickRate, kRedrawStateBytes, kRedrawListsBytes);
    return true;
}

