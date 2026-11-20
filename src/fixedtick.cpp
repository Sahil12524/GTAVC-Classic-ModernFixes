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
#include "missionhelp.h"
#include "aircontrol.h"
#include "hunter.h"
#include "faststream.h"
#include "gameplay.h"
#include "watchdog.h"
#include "framelimiter.h"
#include "drawdist.h"
#include "alphafix.h"
#include "timing.h"
#include <windows.h>
#include <string.h>

// ---------------------------------------------------------------- addresses (v1.0)
static const DWORD IDLE              = 0x4A5D80;
static const DWORD IDLE_CALL_SITE    = 0x4A5BE0;   // AppEventHandler: call Idle
static const DWORD IDLE_RENDER_HALF  = 0x4A5DE3;   // after CGame::Process / audio / demo check
static const DWORD MIRROR_SITE       = 0x4A5E40, MIRROR_FN     = 0x5BF540;  // the water's wave mask (named mirrors before)
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

// CLinkList<T> (0x44 bytes: used head +0, used tail +0x10, free head +0x20, free tail
// +0x30, node array at +0x40), 16-byte nodes with prev at +8 and next at +0xC.
struct ListInfo { DWORD addr; unsigned capacity; };
static const ListInfo kAlphaLists[] = {
    { 0xA0FD84, 0 }, { 0x97F2F8, 20 }, { 0x978D44, 30 },   // cleared in ConstructRenderList
};
// How many nodes of a list's array are in use, read from the list itself: the highest
// node index on its used or free chain, plus one. The sizes were fixed numbers (the
// alpha entity list's as VC Modern Fix sets it, 20 and 30 for the other two). A limit
// adjuster that gives a list a bigger array (Project2DFX's Open Limit Adjuster grows
// 0x978D44 to 62) then had only part of it saved and put back on every redraw frame:
// the restored head and nodes 0-29 pointed at nodes 30-61 as the redraw left them,
// the chain closed on itself, and the next clear of the list (0x582B40, on loading a
// save) went round it forever (freeze of 2026-11-19 at 0x582B5B). 0 = don't save or
// restore the list (no array, a chain that leaves the array, or more than fits).
static const unsigned kListMaxNodes = 4096;
static volatile long g_listSkipped = 0;
static unsigned ListNodesInUse(DWORD list) {
    const DWORD links = *(const DWORD*)(list + 0x40);
    if (!links) return 0;
    unsigned top = 0;
    for (DWORD head = list; head <= list + 0x20; head += 0x20) {
        const DWORD tail = head + 0x10;
        DWORD n = *(const DWORD*)(head + 0xC);
        for (unsigned steps = 0; n != tail; steps++) {
            if (steps >= kListMaxNodes || n < links || ((n - links) & 15) || (n - links) / 16 >= kListMaxNodes) {
                if (InterlockedIncrement(&g_listSkipped) == 1)
                    Log("FixedTick: render list at %06lX has a node outside its array or more than %u nodes; "
                        "not saved or put back on redraw frames", list, kListMaxNodes);
                return 0;
            }
            const unsigned idx = (n - links) / 16;
            if (idx + 1 > top) top = idx + 1;
            n = *(const DWORD*)(n + 0xC);
        }
    }
    return top;
}

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

// Their movement between ticks. A marker is placed once per tick (the big pink arrow
// above a mission target by the radar's blips, its spin and bob from the game clock),
// so redraw frames drew it where the last tick put it: it spun and bobbed 30 times a
// second, and ran ahead of the blended car or person it hangs over. At the end of each
// tick every placed marker's matrix (+0, position at +0x30) and size (+0x6C) are kept
// with its id (+0x54); a redraw frame draws a marker placed by both of the last two
// ticks between them, at the same blend as the objects: the position and size in a
// straight line, the spin about the vertical the short way round.
static const DWORD MARKER_ID = 0x54, MARKER_SCALE = 0x6C;
struct MarkerShot { bool placed; DWORD id; float m[16]; float size; };
static MarkerShot g_markerPrev[NUM_MARKERS], g_markerCur[NUM_MARKERS];
static bool g_markerHistory = false;

static void MarkersEndOfTick() {
    if (!g_markersOk) return;
    memcpy(g_markerPrev, g_markerCur, sizeof g_markerCur);
    const unsigned char* m = (const unsigned char*)MARKERS;
    for (DWORD i = 0; i < NUM_MARKERS; i++, m += MARKER_SIZE) {
        MarkerShot& s = g_markerCur[i];
        s.placed = *(void* const*)(m + MARKER_MODEL) != nullptr && m[MARKER_PLACED];
        s.id = *(const DWORD*)(m + MARKER_ID);
        memcpy(s.m, m, sizeof s.m);
        s.size = *(const float*)(m + MARKER_SCALE);
    }
    g_markerHistory = true;
}

static void MarkersForget() { g_markerHistory = false; memset(g_markerCur, 0, sizeof g_markerCur); }

static void MarkersBlend(float a) {
    if (!g_markerHistory) return;
    a = a < 0.0f ? 0.0f : a > 1.0f ? 1.0f : a;
    unsigned char* m = (unsigned char*)MARKERS;
    for (DWORD i = 0; i < NUM_MARKERS; i++, m += MARKER_SIZE) {
        const MarkerShot& p = g_markerPrev[i];
        const MarkerShot& c = g_markerCur[i];
        if (!g_markerLive[i] || !p.placed || !c.placed || p.id != c.id) continue;
        float* M = (float*)m;
        bool jumped = false;
        for (int k = 12; k < 15; k++) {
            const float d = c.m[k] - p.m[k];
            if (d > 20.0f || d < -20.0f) jumped = true;   // moved to another place: no blend
        }
        if (jumped) continue;
        memcpy(M, c.m, sizeof c.m);
        for (int k = 12; k < 15; k++) M[k] = p.m[k] + (c.m[k] - p.m[k]) * a;
        *(float*)(m + MARKER_SCALE) = p.size + (c.size - p.size) * a;
        // spin: the right vector's turn about the vertical from the previous tick, the short way
        float d = __builtin_atan2f(c.m[1], c.m[0]) - __builtin_atan2f(p.m[1], p.m[0]);
        while (d > 3.14159265f) d -= 6.2831853f;
        while (d < -3.14159265f) d += 6.2831853f;
        if (d > 1.5f || d < -1.5f) continue;   // a jump, not a spin
        const float t = -(1.0f - a) * d, cs = __builtin_cosf(t), sn = __builtin_sinf(t);
        for (int axis = 0; axis < 3; axis++) {   // right, up, at: turned back by the part not yet reached
            float* v = M + axis * 4;
            const float x = v[0], y = v[1];
            v[0] = x * cs - y * sn;
            v[1] = x * sn + y * cs;
        }
    }
}

static void MarkersBeginRedraw(float alpha) {
    if (!g_markersOk) return;
    unsigned char* m = (unsigned char*)MARKERS;
    memcpy(g_markerSave, m, sizeof g_markerSave);
    for (DWORD i = 0; i < NUM_MARKERS; i++, m += MARKER_SIZE) {
        g_markerLive[i] = *(void**)(m + MARKER_MODEL) != nullptr;
        if (g_markerLive[i]) m[MARKER_PLACED] = 1;
    }
    MarkersBlend(alpha);
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
// The fire truck's water jet (CWaterCannons::aCannons, 3 x 0x19C bytes at 0x97F34C:
// id +0, newest point +4 (short), 16 points at +0x0C, their speeds at +0xCC, in use
// at +0x18C). Each tick the truck adds a point at the nozzle (0x57B160) and
// CWaterCannon::Update_OncePerFrame (0x57BB90, through the call at 0x4A45B9) moves
// every point by its speed x the time step; CWaterCannons::Render (call at 0x4A6515)
// draws the jet through the points every frame. Redraw frames drew the last tick's
// points: the jet moved 30 times a second. At the end of each tick the points are
// kept; a point that is the same drop as the tick before (it moved by its speed x
// the step) is drawn between its two places, at the same blend as the objects, and
// the newest one (at the nozzle) between the last two nozzle points, so the jet's
// start follows the blended truck. The live points are put back after the redraw.
static const DWORD CANNONS = 0x97F34C, CANNON_SIZE = 0x19C, NUM_CANNONS = 3, CANNON_POINTS = 16;
static const DWORD CANNON_POS = 0x0C, CANNON_VEL = 0xCC, CANNON_USED = 0x18C;
static bool g_cannonsOk = false;   // the game's own water cannon code
struct CannonShot {
    DWORD id;
    float pos[CANNON_POINTS][3];
    bool used[CANNON_POINTS], same[CANNON_POINTS];   // same: the drop it was last tick, moved
    bool hasNew;
    float newest[3];                                  // the point added at the nozzle this tick
};
static CannonShot g_cannonPrev[NUM_CANNONS], g_cannonCur[NUM_CANNONS];
static bool g_cannonHistory = false;
static unsigned char g_cannonSave[NUM_CANNONS * CANNON_SIZE];

static void CannonsEndOfTick() {
    if (!g_cannonsOk) return;
    memcpy(g_cannonPrev, g_cannonCur, sizeof g_cannonCur);
    const float ts = *TimeStep;
    const unsigned char* c = (const unsigned char*)CANNONS;
    for (DWORD k = 0; k < NUM_CANNONS; k++, c += CANNON_SIZE) {
        CannonShot& s = g_cannonCur[k];
        const CannonShot& p = g_cannonPrev[k];
        s.id = *(const DWORD*)c;
        s.hasNew = false;
        const bool sameJet = g_cannonHistory && s.id && p.id == s.id;
        for (DWORD i = 0; i < CANNON_POINTS; i++) {
            const float* q = (const float*)(c + CANNON_POS) + i * 3;
            const float* v = (const float*)(c + CANNON_VEL) + i * 3;
            s.used[i] = s.id && c[CANNON_USED + i];
            memcpy(s.pos[i], q, sizeof s.pos[i]);
            s.same[i] = false;
            if (!s.used[i]) continue;
            if (sameJet && p.used[i]) {
                float err = 0.0f;
                for (int a = 0; a < 3; a++) {
                    const float d = q[a] - p.pos[i][a] - v[a] * ts;
                    err += d * d;
                }
                s.same[i] = err < 1.0f;   // moved by its speed: the same drop
            }
            if (!s.same[i] && !s.hasNew) { s.hasNew = true; memcpy(s.newest, q, sizeof s.newest); }
        }
    }
    g_cannonHistory = true;
}

static void CannonsForget() { g_cannonHistory = false; memset(g_cannonCur, 0, sizeof g_cannonCur); }

static void CannonsBeginRedraw(float a) {
    if (!g_cannonsOk) return;
    memcpy(g_cannonSave, (const void*)CANNONS, sizeof g_cannonSave);
    if (!g_cannonHistory) return;
    a = a < 0.0f ? 0.0f : a > 1.0f ? 1.0f : a;
    unsigned char* c = (unsigned char*)CANNONS;
    for (DWORD k = 0; k < NUM_CANNONS; k++, c += CANNON_SIZE) {
        const CannonShot& p = g_cannonPrev[k];
        const CannonShot& s = g_cannonCur[k];
        if (!s.id || *(const DWORD*)c != s.id || p.id != s.id) continue;
        for (DWORD i = 0; i < CANNON_POINTS; i++) {
            if (!s.used[i] || !c[CANNON_USED + i]) continue;
            float* q = (float*)(c + CANNON_POS) + i * 3;
            const float* from;
            if (s.same[i]) from = p.pos[i];
            else if (p.hasNew && s.hasNew && !memcmp(s.pos[i], s.newest, sizeof s.newest)) from = p.newest;
            else continue;
            float d2 = 0.0f;
            for (int ax = 0; ax < 3; ax++) d2 += (s.pos[i][ax] - from[ax]) * (s.pos[i][ax] - from[ax]);
            if (d2 > 400.0f) continue;   // more than 20 m: not a blend
            for (int ax = 0; ax < 3; ax++) q[ax] = from[ax] + (s.pos[i][ax] - from[ax]) * a;
        }
    }
}

static void CannonsEndRedraw() {
    if (g_cannonsOk) memcpy((void*)CANNONS, g_cannonSave, sizeof g_cannonSave);
}

static bool g_snapValid = false;                  // last tick rendered the 3D scene
static LARGE_INTEGER g_freq;
static LONGLONG g_period = 0, g_nextTick = 0;
static HANDLE g_timer = nullptr;

static const unsigned kNumLists = sizeof kAlphaLists / sizeof kAlphaLists[0];
static const unsigned kListBytes = 0x44 + kListMaxNodes * 16;
struct Snapshot {
    unsigned listNodes[kNumLists];   // nodes saved per list (0: not saved)
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
static bool g_tickReplay = false;   // an F1 replay was playing when this tick started
static LONGLONG g_tickStart = 0;    // QPC time the latest tick's world is at (its scheduled time)
static LONGLONG g_prevTickStart = 0;

// The game's clock (CTimer::Update, 0x4D0F71: QueryPerformanceCounter through the
// import at 0x6F2450) during a tick reads the tick's scheduled time instead of the
// moment the tick happened to start. Ticks can only start on a drawn frame, so at
// 144 Hz they started up to a frame late, 28 or 35 ms apart instead of 33.3: the
// world moved by those uneven steps, and a frame right after a short step had to
// be held back (blend clamped), so motion and camera turns hitched about six
// times a second. On the schedule every tick covers exactly one tick length, and
// frames drawn one tick length behind always fall between the last two ticks.
static const DWORD TIMER_QPC_SITE = 0x4D0F71;
static const DWORD TIMER_LAST = 0x705300;   // CTimer's previous reading (LARGE_INTEGER)
static bool g_inTick = false;
static LONGLONG g_frameTime = 0;   // when the frame being drawn will be shown
static bool g_clockHooked = false;   // GameClock installed
static LONGLONG g_tickClock = 0;

static BOOL WINAPI GameClock(LARGE_INTEGER* out) {
    const BOOL r = QueryPerformanceCounter(out);
    if (r && g_inTick && g_tickClock) {
        LONGLONG v = g_tickClock;
        const LONGLONG last = *(const LONGLONG*)TIMER_LAST;
        if (v < last) v = last;                       // never back in time
        const LONGLONG bound = g_frameTime > out->QuadPart ? g_frameTime : out->QuadPart;
        if (v > bound) v = bound;                     // at most the frame's own time (slot)
        out->QuadPart = v;
    }
    return r;
}

// Where between the previous and the latest tick a frame drawn now sits: the
// moment one tick length ago, between the two ticks' scheduled times.
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
        const unsigned n = ListNodesInUse(l.addr);
        s.listNodes[i] = n;
        if (!n) continue;
        memcpy(s.lists[i], (void*)l.addr, 0x44);
        memcpy(s.lists[i] + 0x44, *(void**)(l.addr + 0x40), n * 16);
    }
}

template <unsigned N>
static void Load(const Snapshot& s, const StateRange (&table)[N], bool lists = true) {
    const unsigned char* p = s.state;
    for (const StateRange& r : table) { memcpy((void*)r.addr, p, r.size); p += r.size; }
    for (unsigned i = 0; lists && i < kNumLists; i++) {
        const ListInfo& l = kAlphaLists[i];
        const unsigned n = s.listNodes[i];
        // only into the same node array it was saved from (a list made again
        // meanwhile, on a game load, is left as the game made it)
        if (!n || *(const DWORD*)(l.addr + 0x40) != *(const DWORD*)(s.lists[i] + 0x40)) continue;
        memcpy((void*)l.addr, s.lists[i], 0x44);
        memcpy(*(void**)(l.addr + 0x40), s.lists[i] + 0x44, n * 16);
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
    const unsigned long long t0 = TimingStart();
    if (g_redraw) Load(g_before, kRedrawLists);
    else          Save(g_before, kRedrawLists);
    TimingEnd(g_redraw ? TS_REDRAW_SAVELOAD : TS_TICK_SNAPSHOTS, t0);
    WaterMaskStep((void*)g_mirrorFn);   // the water's wave mask: steps on ticks only (alphafix.cpp)
}

// Tick: the real pass (game camera) as in the original game, its result saved for
// game logic; then, if blending, the camera and objects are moved to where this
// frame draws them and the list is built again for that view.
// Redraw frame: the camera is already blended (Redraw), build the list for it.
static void __cdecl HookConstructRenderList() {
    ((void(__cdecl*)())g_crlFn)();
    // F1 starts (or the replay ends) inside this update's game logic, after
    // g_interpTick was decided: the world just changed places, so no blending.
    if (g_redraw || !g_interpTick || ReplayPlaying() != g_tickReplay) return;
    unsigned long long t0 = TimingStart();
    SaveRanges(g_crlTrue, kCrlState);
    TimingEnd(TS_TICK_SNAPSHOTS, t0);
    t0 = TimingStart();
    EntityFlagsSave();
    TimingEnd(TS_ENTFLAGS_SAVE, t0);
    g_crlValid = true;
    t0 = TimingStart();
    InterpCapture();
    TimingEnd(TS_INTERP_CAPTURE, t0);
    InterpApply(BlendAlpha(g_frameTime), true);   // about the previous tick; redraw frames move on from it
    g_interpOn = true;
    ((void(__cdecl*)())g_crlFn)();
}

// The entity PreRender pass runs on every drawn frame, so shadows, lights,
// coronas, wheels and bones follow the blended positions. On a redraw frame it
// starts from the values the tick had right before its own PreRender (lists it
// fills, counts, temporaries); its sounds are blocked, particles and water props
// can't spawn, the RNG and everything else are put back after the frame.
//
// Random numbers: PreRender and the drawing after it call rand() (0x6499F0; seed in
// the per-thread data, RNG_STATE +8) for lamp and light flicker, muzzle flashes and
// coronas. A redraw frame used to start from the seed the tick left at its end, so
// its numbers were other than the tick's and those lights changed from frame to
// frame. It now starts PreRender from the seed the tick had right before its own
// PreRender, so it draws the same numbers (the seed is put back after the frame, as
// before: the game's sequence is untouched). Every redraw frame then compares the
// seed and the point-light count (CPointLights::NumLights 0x9751B0, reset in Idle at
// 0x4A5D98 before the update; AddLight 0x567700 refuses at 32) after PreRender with
// the tick's (logged by FixedTickLogPreRender): equal means the same random
// numbers and the same lights.
static int* SeedPtr() {
    char* ptd = ((char*(__cdecl*)(int))RNG_STATE)(1);
    return ptd ? (int*)(ptd + 8) : nullptr;
}
static volatile const int* const NumPointLights = (volatile const int*)0x9751B0;
static int g_prePRSeed = 0, g_postPRSeed = 0, g_postPRLights = 0;
static bool g_prSeedOk = false;
static unsigned g_prSame = 0, g_prSeedDiff = 0, g_prLightsDiff = 0;

static void __cdecl HookPreRender() {
    int* seed = SeedPtr();
    const unsigned long long t0 = TimingStart();
    if (!g_redraw) {
        Save(g_prePR, kRedrawLists, false);
        SaveRanges(g_prePRState, kPreRenderState);
        g_snapValid = true;
        g_prSeedOk = seed != nullptr;
        if (seed) g_prePRSeed = *seed;
    } else {
        Load(g_prePR, kRedrawLists, false);   // alpha lists were just rebuilt for this view
        LoadRanges(g_prePRState, kPreRenderState);
        if (seed && g_prSeedOk) *seed = g_prePRSeed;   // the tick's random numbers (Redraw puts the seed back)
    }
    TimingEnd(g_redraw ? TS_REDRAW_SAVELOAD : TS_TICK_SNAPSHOTS, t0);
    ((void(__cdecl*)())g_preRenderFn)();
    if (!g_redraw) {
        if (seed) g_postPRSeed = *seed;
        g_postPRLights = *NumPointLights;
    } else if (seed && g_prSeedOk) {
        const bool seedSame = *seed == g_postPRSeed, lightsSame = *NumPointLights == g_postPRLights;
        if (seedSame && lightsSame) g_prSame++;
        if (!seedSame) g_prSeedDiff++;
        if (!lightsSame) {
            static unsigned logs = 0;
            g_prLightsDiff++;
            if (logs < 3) {
                logs++;
                Log("FixedTick: a redraw frame's PreRender made %d point lights, its tick %d (lights near the edge of the blended view)",
                    *NumPointLights, g_postPRLights);
            }
        }
    }
    if (!g_redraw) {
        if (!g_interpOn) return;
        const unsigned long long tp = TimingStart();
        InterpPartsCapture();
        TimingEnd(TS_PARTS_CAPTURE, tp);
    }
    InterpPartsApply();   // doors, wheels, bonnets... between the two ticks
}

// Once a minute while redraw frames run (30 lines at most).
static void FixedTickLogPreRender() {
    static DWORD at = 0;
    static int lines = 0;
    const DWORD now = GetTickCount();
    if (lines >= 30 || now - at < 60000) return;
    at = now;
    if (!(g_prSame + g_prSeedDiff + g_prLightsDiff)) return;
    lines++;
    Log("FixedTick: redraw frames' PreRender started from the tick's random numbers: %u drew the same random numbers and "
        "point lights as their tick, %u other random numbers (a particle the tick spawned is skipped on redraw frames), "
        "%u another number of point lights", g_prSame, g_prSeedDiff, g_prLightsDiff);
    g_prSame = g_prSeedDiff = g_prLightsDiff = 0;
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

// Normal play and F1 replays: extra frames and blending. Not in menus, loading or
// special draws: those run at the tick rate, as the original game did with its
// limiter. A replay sets the world from its recording on every update, so its cars,
// people and camera move from tick to tick like in play and are blended the same way
// (it ran at 30 FPS before); the blend history is dropped whenever a replay starts or
// ends (the recording and the live world are different places).
static bool Gameplay(void* arg) {
    return arg && !*MenuActive && !*WantRestart && !*WantLoad && !*SpecialDraw;
}

// Redraw frames draw from the lists the last tick built (visible entities, alpha
// lists). An entity deleted after that tick - by another mod or a trainer acting
// from its Present / overlay hook, outside the game update - would still be in
// them, with its model gone (m_rwObject null): drawing it crashed in the vehicle
// render loop (0x4C9DDD -> 0x582739). So the ped, vehicle and object pools' slot
// flags (CPool +4: in use and id per slot) are kept at the end of each tick, and
// a redraw happens only while they are unchanged; otherwise the frame waits for
// the next tick, which builds new lists. The building and dummy pools are watched
// too (a mod swapping IPL buildings from its Present hook); they hold 7000 and 2340
// (CPools::Initialise 0x4C0308 / 0x4C036B), so comparing them costs about 9 KB of
// memcmp a frame.
static const DWORD kPools[] = { 0x97F2AC, 0xA0FDE4, 0x94DBE0, 0x97F240, 0x97F25C };   // peds, vehicles, objects, buildings, dummies
static const int kNumPools = sizeof kPools / sizeof kPools[0];
static const int kPoolFlagsMax = 8192;
static unsigned char g_poolFlags[kNumPools][kPoolFlagsMax];
static int g_poolSizes[kNumPools];

static void PoolFlags(int i, const unsigned char** flags, int* size) {
    const unsigned char* pool = *(const unsigned char* const*)kPools[i];
    *flags = pool ? *(const unsigned char* const*)(pool + 4) : nullptr;
    int n = pool ? *(const int*)(pool + 8) : 0;
    *size = !*flags || n < 0 ? 0 : n > kPoolFlagsMax ? kPoolFlagsMax : n;
}

static void SnapshotPools() {
    for (int i = 0; i < kNumPools; i++) {
        const unsigned char* flags;
        PoolFlags(i, &flags, &g_poolSizes[i]);
        if (g_poolSizes[i]) memcpy(g_poolFlags[i], flags, g_poolSizes[i]);
    }
}

static bool PoolsUnchanged() {
    for (int i = 0; i < kNumPools; i++) {
        const unsigned char* flags;
        int size;
        PoolFlags(i, &flags, &size);
        if (size != g_poolSizes[i] || (size && memcmp(g_poolFlags[i], flags, size))) return false;
    }
    return true;
}

static bool CanRedraw(void* arg) {
    if (!(Gameplay(arg) && g_snapValid && g_crlValid)) return false;
    const unsigned long long t0 = TimingStart();
    const bool same = PoolsUnchanged();
    TimingEnd(TS_POOL_CHECK, t0);
    if (same) return true;
    static unsigned logs = 0;
    if (logs < 5) { logs++; Log("FixedTick: an entity was added or removed outside the game update; waiting for the next tick to draw"); }
    g_crlValid = false;   // stays off until the next tick builds new lists
    return false;
}

static void Redraw(void* arg) {
    const float ts = *TimeStep, tsNC = *TimeStepNC, rotor = *RotorStep;
    int* seed = nullptr;
    if (char* ptd = ((char*(__cdecl*)(int))RNG_STATE)(1)) seed = (int*)(ptd + 8);
    const int savedSeed = seed ? *seed : 0;

    unsigned long long t0 = TimingStart();
    Save(g_after, kRedrawState);   // everything the render half may change
    SaveRanges(g_afterPRState, kPreRenderState);
    TimingEnd(TS_REDRAW_SAVELOAD, t0);
    TrainerWindowFrame();          // trainer window open: its cursor moves this frame too (hunter.cpp)
    g_redraw = 1;
    *TimeStep = 0.0f;
    *TimeStepNC = 0.0f;
    if (g_rotorOk) *RotorStep = 0.0f;
    ((void(__cdecl*)())SPRITE_INIT)();
    ((void(__cdecl*)())FONT_INIT)();
    t0 = TimingStart();
    InterpApply(BlendAlpha(g_frameTime), false);
    TimingEnd(TS_INTERP_APPLY, t0);
    MarkersBeginRedraw(BlendAlpha(g_frameTime));
    CannonsBeginRedraw(BlendAlpha(g_frameTime));
    g_redrawEntry(arg);            // Idle()'s render half; returns through Idle()'s epilogue
    CannonsEndRedraw();
    MarkersEndRedraw();
    t0 = TimingStart();
    InterpRestore(false);
    TimingEnd(TS_INTERP_APPLY, t0);
    t0 = TimingStart();
    RestoreRealRenderList();
    TimingEnd(TS_REDRAW_CRL_BACK, t0);
    t0 = TimingStart();
    LoadRanges(g_afterPRState, kPreRenderState);
    Load(g_after, kRedrawState);   // ...and all of it put back
    TimingEnd(TS_REDRAW_SAVELOAD, t0);
    if (g_rotorOk) *RotorStep = rotor;
    *TimeStepNC = tsNC;
    *TimeStep = ts;
    if (seed) *seed = savedSeed;
    g_redraw = 0;
}

// How long frames take from start to shown (QPC), for unpaced frames' times.
static LONGLONG g_emaDraw = 0, g_emaTick = 0;
static void Ema(LONGLONG* e, LONGLONG d) {
    const LONGLONG cap = g_freq.QuadPart / 33;   // a slow frame (loading) shouldn't skew it
    if (d > cap) d = cap;
    if (d < 0) d = 0;
    *e += (d - *e) / 8;
}

static void __cdecl HookIdle(void* arg) {
    g_heartbeat++;
    g_watchdogArmed = true;
    const bool replay = ReplayPlaying();
    static bool wasReplay = false;
    if (replay != wasReplay) {
        wasReplay = replay;
        InterpReset();   // the recording and the live world: no blending across
        MarkersForget();
        g_crlValid = false;
        Log(replay ? "Replay: playing (extra frames and blending as in play; mod helpers paused)" : "Replay: ended");
    }
    g_tickReplay = replay;
    if (!replay) {
        unsigned long long t0 = TimingStart();
        QuickSaveFrame();
        TimingEnd(TS_QUICKSAVE, t0);
        t0 = TimingStart();
        PhoneSkipFrame();
        TimingEnd(TS_HELPERS, t0);
    }
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    // The frame is drawn for the moment it will be shown (the frame limiter's slot,
    // up to a frame ahead), and whether it is a tick is decided for that moment too.
    LONGLONG frame = FrameLimiterFrameTime();
    if (!FrameLimiterPacing()) {
        // No limiter (unlocked, or vsync): a frame is shown when it is done, so its
        // time is predicted from how long recent frames of its kind took; a frame
        // with a game update in it takes longer.
        frame = now.QuadPart + g_emaDraw;
        if (frame >= g_nextTick && g_nextTick) frame = now.QuadPart + g_emaTick;
    }
    if (frame < now.QuadPart) frame = now.QuadPart;
    g_frameTime = frame;
    if (g_nextTick == 0) g_nextTick = frame;

    // Pause menu: the game clock is stopped (CTimer::m_UserPause), so nothing in
    // the world moves; run the whole frame at full rate like the start menu.
    if (*MenuActive && *UserPause) {
        g_nextTick = now.QuadPart;   // first frame after unpausing is a tick
        g_snapValid = g_crlValid = g_interpTick = false;
        InterpReset();
        ((void(__cdecl*)(void*))g_idleFn)(arg);
        return;
    }

    if (frame < g_nextTick) {
        if (CanRedraw(arg)) {
            Redraw(arg);
            TimingFrame(true);
            LARGE_INTEGER end;
            QueryPerformanceCounter(&end);
            FrameStatsWork(false, end.QuadPart - now.QuadPart);
            Ema(&g_emaDraw, end.QuadPart - now.QuadPart);
            return;
        }
        WaitUntil(g_nextTick);   // menu, loading, fades: run at the tick rate like the original
        QueryPerformanceCounter(&now);
        if (frame < now.QuadPart) frame = g_frameTime = now.QuadPart;
    }
    // Tick. Late by a whole interval or more (a slow frame: streaming, alt-tab...)?
    // Restart the schedule from now instead of running catch-up ticks back to back
    // (each with a tiny time step, which shows as a stall then a lurch); the game
    // measures real elapsed time, so the next tick covers the gap, as in the
    // original game after a slow frame. Far behind: also don't blend across it.
    LONGLONG sched = g_nextTick;   // when this tick was due
    g_nextTick += g_period;
    if (frame - g_nextTick > 4 * g_period) {
        g_nextTick = frame + g_period;
        sched = frame;
        InterpReset();
    } else if (g_nextTick <= frame) {
        g_nextTick = frame + g_period;
        sched = frame;
    }
    if (sched > frame) sched = frame;
    if (!g_clockHooked) sched = now.QuadPart;   // the game reads the real clock
    g_prevTickStart = g_tickStart;
    g_tickStart = sched;
    g_tickClock = sched;
    g_snapValid = g_crlValid = false;
    g_interpTick = Gameplay(arg);
    if (!g_interpTick) InterpReset();
    if (!replay) {   // during a replay the world is the recording: leave it alone
        unsigned long long t0 = TimingStart();
        QuickSaveBeforeTick();
        TimingEnd(TS_QUICKSAVE, t0);
        t0 = TimingStart();
        PhoneSkipBeforeTick();
        GameplayBeforeTick();   // turret aim, mission leftovers
        MissionHelpBeforeTick();   // intro skip, stuck missions (before the scripts run)
        AirControlBeforeTick();    // cars in the air: pitch and roll with the helicopter controls
        HunterBeforeTick();        // Hunter: wheel picks the weapon
        FastStreamBeforeTick();    // extra model / texture loading
        TimingEnd(TS_HELPERS, t0);
    }
    g_inTick = true;
    ((void(__cdecl*)(void*))g_idleFn)(arg);
    g_inTick = false;
    TimingTick();
    TimingFrame(false);
    {
        LARGE_INTEGER end;
        QueryPerformanceCounter(&end);
        FrameStatsWork(true, end.QuadPart - now.QuadPart);
        if (g_interpTick) Ema(&g_emaTick, end.QuadPart - now.QuadPart);
    }
    unsigned long long t0 = TimingStart();
    SnapshotPools();   // what the lists this tick built were made from
    if (g_interpTick && ReplayPlaying() == replay) { MarkersEndOfTick(); CannonsEndOfTick(); }   // 3D markers, water jet
    else { MarkersForget(); CannonsForget(); }
    TimingEnd(TS_TICK_SNAPSHOTS, t0);
    if (g_interpOn) {
        t0 = TimingStart();
        InterpRestore(true);
        RestoreRealRenderList();
        g_interpOn = false;
        TimingEnd(TS_TICK_RESTORE, t0);
    }
    if (ReplayPlaying() != replay) {   // a replay started or ended inside this update
        InterpReset();
        MarkersForget();
        CannonsForget();
        g_crlValid = false;   // its lists are from the other world: the next tick builds new ones
    }
    if (replay || ReplayPlaying()) return;   // the mod helpers stay paused during a replay
    FixedTickLogPreRender();
    InteriorEndOfTick();   // the drawn area follows the player (teleports, jetpack); timed in interior.cpp
    t0 = TimingStart();
    QuickSaveEndOfTick();   // F5 / F9: the frame is drawn, everything is back to the real state
    TimingEnd(TS_QUICKSAVE, t0);
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
    // CWaterCannon::Update_OncePerFrame: push ebx; mov ebx,ecx; mov eax,[ebx+8]; push ebp;
    // add eax,96h; and the points loop's "cmp byte [ebx+eax+18Ch],0"
    static const unsigned char cannonUpdate[] = { 0x53, 0x89, 0xCB, 0x8B, 0x43, 0x08, 0x55, 0x05, 0x96, 0x00, 0x00, 0x00 };
    static const unsigned char cannonUsed[] = { 0x80, 0xBC, 0x03, 0x8C, 0x01, 0x00, 0x00, 0x00 };
    static const unsigned char cannonArray[] = { 0xBE, 0x4C, 0xF3, 0x97, 0x00 };
    g_cannonsOk = Matches(0x57BB90, cannonUpdate, sizeof cannonUpdate) && Matches(0x57BBD3, cannonUsed, sizeof cannonUsed)
               && Matches(0x57B127, cannonArray, sizeof cannonArray);
    if (!g_cannonsOk) Log("FixedTick: water cannon code changed by another mod; the fire truck's jet left to it");

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

    // CRenderer::RenderOneNonRoad (0x4C9DA0, cdecl entity): an entity in the draw
    // lists whose model is gone (m_rwObject +0x4C null: deleted, or its model
    // removed, after the lists were built) is skipped instead of crashing at
    // 0x582739 (vehicle lighting reads the clump).
    static const unsigned char renderOne[] = { 0x53, 0x56, 0x57, 0x55, 0x83, 0xEC, 0x08, 0x8B, 0x5C, 0x24, 0x1C };
    if (Matches(0x4C9DA0, renderOne, sizeof renderOne)) {
        static const unsigned char guard[] = {
            0x8B, 0x44, 0x24, 0x04,                     // mov eax, [esp+4]   entity
            0x85, 0xC0,                                 // test eax, eax
            0x74, 0x06,                                 // je skip
            0x83, 0x78, 0x4C, 0x00,                     // cmp dword [eax+4Ch], 0
            0x75, 0x01,                                 // jne draw
            0xC3,                                       // skip: ret
            0x53, 0x56, 0x57, 0x55, 0x83, 0xEC, 0x08,   // draw: the moved instructions
            0xE9, 0, 0, 0, 0,                           // jmp 0x4C9DA7
        };
        unsigned char* g = Emit(guard, sizeof guard);
        *(int*)(g + sizeof guard - 4) = (int)(0x4C9DA7 - ((DWORD)g + sizeof guard));
        unsigned char patch[7] = { 0xE9, 0, 0, 0, 0, 0x90, 0x90 };
        *(int*)(patch + 1) = (int)((DWORD)g - (0x4C9DA0 + 5));
        WriteCode(0x4C9DA0, patch, sizeof patch);
    } else {
        Log("FixedTick: entity draw (0x4C9DA0) changed by another mod; no guard for deleted entities");
    }
    DWORD old;
    VirtualProtect(RotorStep, sizeof(float), PAGE_READWRITE, &old);   // .data, already writable

    RedirectCall(MIRROR_SITE, (void*)&HookMirror);
    RedirectCall(CRL_SITE, (void*)&HookConstructRenderList);
    RedirectCall(PRERENDER_SITE, (void*)&HookPreRender);

    QueryPerformanceFrequency(&g_freq);
    g_period = g_freq.QuadPart / g_cfg.tickRate;
    g_timer = CreateWaitableTimerExW(nullptr, nullptr, 0x00000002 /* HIGH_RESOLUTION */, TIMER_ALL_ACCESS);
    if (!g_timer) g_timer = CreateWaitableTimerW(nullptr, TRUE, nullptr);

    static const unsigned char qpcCall[] = { 0xFF, 0x15, 0x50, 0x24, 0x6F, 0x00 };   // call [QueryPerformanceCounter]
    if (Matches(TIMER_QPC_SITE, qpcCall, sizeof qpcCall)) {
        unsigned char call[6] = { 0xE8, 0, 0, 0, 0, 0x90 };
        *(int*)(call + 1) = (int)((DWORD)&GameClock - (TIMER_QPC_SITE + 5));
        WriteCode(TIMER_QPC_SITE, call, sizeof call);
        g_clockHooked = true;
        Log("FixedTick: game clock follows the tick schedule (even steps)");
    } else {
        Log("FixedTick: game clock read (0x4D0F71) changed by another mod; ticks use the real start time");
    }

    RedirectCall(IDLE_CALL_SITE, (void*)&HookIdle);   // last: switches the game over
    Log("FixedTick: installed, %d ticks per second, %u bytes of render state tracked (%u in lists); "
        "redraw frames' PreRender starts from the tick's random numbers",
        g_cfg.tickRate, kRedrawStateBytes, kRedrawListsBytes);
    return true;
}

