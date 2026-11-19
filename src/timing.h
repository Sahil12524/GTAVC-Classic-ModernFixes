#pragma once
// Where VCModernFix's own time goes, per game update and per drawn frame: one
// "Timing:" line a minute in VCModernFix.log (timing.cpp). Sections are timed with
// the time stamp counter (a few cycles to read), converted to ms with the QPC clock.
#include <intrin.h>

enum TimingSection {
    // per game update (tick)
    TS_INTERP_CAPTURE,     // InterpCapture: matrices, poses, particles, rubbish of this tick
    TS_PARTS_CAPTURE,      // InterpPartsCapture: vehicle part matrices of this tick
    TS_TICK_SNAPSHOTS,     // the tick's snapshots for redraw frames (lists, PreRender state, CRL output, pool flags)
    TS_ENTFLAGS_SAVE,      // ConstructRenderList's per-entity flags saved
    TS_TICK_RESTORE,       // after the tick: blended matrices put back, real render list put back
    TS_INTERIOR,           // InteriorEndOfTick (without LoadScene)
    TS_LOAD_SCENE,         // the game's LoadScene inside an interior switch (counted per switch)
    TS_QUICKSAVE,          // QuickSaveFrame / BeforeTick / EndOfTick
    TS_HELPERS,            // the other before-tick helpers (phone, gameplay, missions, air, hunter, streaming)
    // per redraw frame
    TS_REDRAW_SAVELOAD,    // Redraw: kRedrawState and PreRender state saved and loaded, list snapshots loaded
    TS_POOL_CHECK,         // PoolsUnchanged: the five pools' slot flags compared
    TS_REDRAW_CRL_BACK,    // the real ConstructRenderList output put back after the redraw (kCrlState + entity flags)
    TS_INTERP_APPLY,       // InterpApply + InterpRestore on redraw frames
    // per drawn frame
    TS_POPULATION,         // PopulationAfterPresent (free-slot count, bodies)
    TS_ALPHA_INSERT,       // inserts into the see-through list (5 sorting call sites)
    TS_COUNT
};

enum TimingCounter {
    TC_TWO_PASS,           // draws split into two passes (alphafix)
    TC_ALPHA_INSERTS,      // see-through list inserts
    TC_BONE_LOOKUPS,       // BoneFrames calls (IS_CLUMP_SKINNED + SKIN_HIERARCHY each)
    TC_COUNT
};

extern unsigned long long g_timingSum[TS_COUNT];
extern unsigned long long g_timingMax[TS_COUNT];
extern unsigned g_timingCalls[TS_COUNT];
extern unsigned long long g_timingCount[TC_COUNT];

static inline unsigned long long TimingStart() { return __rdtsc(); }
static inline void TimingEnd(int s, unsigned long long t0) {
    const unsigned long long d = __rdtsc() - t0;
    g_timingSum[s] += d;
    if (d > g_timingMax[s]) g_timingMax[s] = d;
    g_timingCalls[s]++;
}
static inline void TimingCount(int c, unsigned n = 1) { g_timingCount[c] += n; }

// A before / after pair measured in the same session: the old way of doing some
// work, run beside the new one for a while to check they give the same result
// (rule: every replaced copy or scan is checked against the original).
struct TimingCheck {
    const char* name;
    unsigned long long oldCycles, newCycles;
    unsigned calls, same, differ;
};
void TimingCheckAdd(TimingCheck& c, unsigned long long oldCycles, unsigned long long newCycles, bool same);
void TimingRegisterCheck(TimingCheck* c);

void TimingTick();                 // a game update ran (fixedtick.cpp)
void TimingFrame(bool redraw);     // a frame was drawn (fixedtick.cpp)
void TimingAfterPresent();         // once a minute: the Timing: line
