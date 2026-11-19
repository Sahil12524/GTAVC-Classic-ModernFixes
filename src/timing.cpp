// Once a minute, where VCModernFix's own time went (timing.h): per game update the
// work around the tick (blending capture, snapshots for redraw frames, interiors,
// quick save, helpers), per redraw frame the work that keeps it invisible to the game,
// per drawn frame the population check and the see-through list; counts of two-pass
// draws, see-through list inserts and skinned-pose lookups; and the before / after
// pairs of the checks that compare an old way of doing some work with its
// replacement (TimingCheck). Nothing here changes what the game does.
#include "timing.h"
#include "config.h"
#include <windows.h>
#include <string.h>
#include <stdio.h>

unsigned long long g_timingSum[TS_COUNT];
unsigned long long g_timingMax[TS_COUNT];
unsigned g_timingCalls[TS_COUNT];
unsigned long long g_timingCount[TC_COUNT];

static unsigned g_ticks = 0, g_redraws = 0, g_frames = 0;
static TimingCheck* g_checks[16];
static int g_nChecks = 0;
static double g_cyclesPerMs = 0;
static LARGE_INTEGER g_q0;
static unsigned long long g_c0 = 0;
static int g_lines = 0;

void TimingTick() { g_ticks++; }
void TimingFrame(bool redraw) { g_frames++; if (redraw) g_redraws++; }

void TimingRegisterCheck(TimingCheck* c) {
    for (int i = 0; i < g_nChecks; i++) if (g_checks[i] == c) return;
    if (g_nChecks < 16) g_checks[g_nChecks++] = c;
}

void TimingCheckAdd(TimingCheck& c, unsigned long long oldCycles, unsigned long long newCycles, bool same) {
    TimingRegisterCheck(&c);
    c.oldCycles += oldCycles;
    c.newCycles += newCycles;
    c.calls++;
    if (same) c.same++; else c.differ++;
}

static int Section(char* out, int room, const char* name, int s, unsigned per) {
    if (!per || room < 80) return 0;
    const double avg = g_timingSum[s] / g_cyclesPerMs / per, mx = g_timingMax[s] / g_cyclesPerMs;
    return snprintf(out, room, "%s %.3f (max %.2f); ", name, avg, mx);
}

void TimingAfterPresent() {
    LARGE_INTEGER q, f;
    QueryPerformanceCounter(&q);
    const unsigned long long c = __rdtsc();
    if (!g_c0) { g_c0 = c; g_q0 = q; return; }
    QueryPerformanceFrequency(&f);
    const double ms = (q.QuadPart - g_q0.QuadPart) * 1000.0 / f.QuadPart;
    if (ms < 60000.0) return;
    g_cyclesPerMs = (c - g_c0) / ms;
    g_c0 = c;
    g_q0 = q;
    if (g_ticks && g_lines < 120) {
        g_lines++;
        // the C library's double formatting with the default x87 control word, as Log does
        unsigned short gameCw, defaultCw = 0x037F;
        __asm__ volatile("fnstcw %0" : "=m"(gameCw));
        __asm__ volatile("fldcw %0" : : "m"(defaultCw));
        char line[1600];
        int n = snprintf(line, sizeof line, "Timing: %u updates, %u frames (%u redraw); ms per update: ", g_ticks, g_frames, g_redraws);
        const int room = (int)sizeof line;
        n += Section(line + n, room - n, "InterpCapture", TS_INTERP_CAPTURE, g_ticks);
        n += Section(line + n, room - n, "parts capture", TS_PARTS_CAPTURE, g_ticks);
        n += Section(line + n, room - n, "snapshots", TS_TICK_SNAPSHOTS, g_ticks);
        n += Section(line + n, room - n, "entity flags saved", TS_ENTFLAGS_SAVE, g_ticks);
        n += Section(line + n, room - n, "put back after the update", TS_TICK_RESTORE, g_ticks);
        n += Section(line + n, room - n, "interiors", TS_INTERIOR, g_ticks);
        n += Section(line + n, room - n, "quick save", TS_QUICKSAVE, g_ticks);
        n += Section(line + n, room - n, "helpers", TS_HELPERS, g_ticks);
        if (g_timingCalls[TS_LOAD_SCENE] && n < room - 80)
            n += snprintf(line + n, room - n, "LoadScene %u x %.1f (max %.1f); ", g_timingCalls[TS_LOAD_SCENE],
                          g_timingSum[TS_LOAD_SCENE] / g_cyclesPerMs / g_timingCalls[TS_LOAD_SCENE],
                          g_timingMax[TS_LOAD_SCENE] / g_cyclesPerMs);
        if (n < room - 40) n += snprintf(line + n, room - n, "ms per redraw frame: ");
        n += Section(line + n, room - n, "save/load", TS_REDRAW_SAVELOAD, g_redraws);
        n += Section(line + n, room - n, "pool check", TS_POOL_CHECK, g_redraws);
        n += Section(line + n, room - n, "render list put back", TS_REDRAW_CRL_BACK, g_redraws);
        n += Section(line + n, room - n, "blend", TS_INTERP_APPLY, g_redraws);
        if (n < room - 40) n += snprintf(line + n, room - n, "ms per frame: ");
        n += Section(line + n, room - n, "population", TS_POPULATION, g_frames);
        n += Section(line + n, room - n, "see-through list inserts", TS_ALPHA_INSERT, g_frames);
        if (g_frames && n < room - 160)
            n += snprintf(line + n, room - n, "per frame: %.0f two-pass draws, %.0f see-through inserts; per update: %.0f pose lookups",
                          (double)g_timingCount[TC_TWO_PASS] / g_frames, (double)g_timingCount[TC_ALPHA_INSERTS] / g_frames,
                          (double)g_timingCount[TC_BONE_LOOKUPS] / g_ticks);
        Log("%s", line);
        for (int i = 0; i < g_nChecks; i++) {
            TimingCheck& k = *g_checks[i];
            if (!k.calls) continue;
            Log("Timing:   check %s: %u compared, %u identical, %u different; old way %.4f ms, new way %.4f ms per call",
                k.name, k.calls, k.same, k.differ, k.oldCycles / g_cyclesPerMs / k.calls, k.newCycles / g_cyclesPerMs / k.calls);
            k.calls = k.same = k.differ = 0;
            k.oldCycles = k.newCycles = 0;
        }
        __asm__ volatile("fldcw %0" : : "m"(gameCw));
    }
    memset(g_timingSum, 0, sizeof g_timingSum);
    memset(g_timingMax, 0, sizeof g_timingMax);
    memset(g_timingCalls, 0, sizeof g_timingCalls);
    memset(g_timingCount, 0, sizeof g_timingCount);
    g_ticks = g_redraws = g_frames = 0;
}
