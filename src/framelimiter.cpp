// Frame limiter for modern PCs.
//
// The game's own limiter (WinMain, 0x600477) is a busy loop: it keeps calling
// PeekMessage until 1000 / RsGlobal.maxFPS milliseconds have passed since the
// last CTimer::Update, which pins one CPU core at 100% and caps at 30 FPS.
// RsGlobal.maxFPS is raised to 1000 so that loop never waits; frames are paced
// here instead, after IDirect3DDevice8::Present, on a high-resolution waitable
// timer with only the last fraction of a millisecond spun. (With fixed-tick on,
// the game world's own rate is set by fixedtick.cpp, not by this limiter.)
#include "framelimiter.h"
#include "config.h"
#include <windows.h>
#include <string.h>

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

// RsGlobal.maxFPS (gta-vc.exe v1.0): WinMain compares the frame time against
// 1000.0 / maxFPS (fild [0x9B48EC] / fdivr [0x6D5EA4] at 0x600480).
// RsInitialise sets it to 30 with "mov dword [0x9B48EC], 1Eh" at 0x602D62.
static int* const MaxFPS = (int*)0x9B48EC;
static const DWORD kMaxFPSInitSite = 0x602D62;
static const unsigned char kMaxFPSInit[] = { 0xC7, 0x05, 0xEC, 0x48, 0x9B, 0x00, 0x1E, 0x00, 0x00, 0x00 };

static int g_cap = 0;                // resolved frames per second, 0 = no limit
static bool g_isV10 = false;
static bool g_maxFpsApplied = false;
static HANDLE g_timer = nullptr;
static bool g_highResTimer = false;
static LARGE_INTEGER g_freq;
static LONGLONG g_nextFrame = 0;     // QPC tick the next frame is due at
static bool g_vsync = false;         // fullscreen, Present waits for the display's refresh

static int DesktopRefresh() {
    DEVMODEA dm = {};
    dm.dmSize = sizeof dm;
    if (EnumDisplaySettingsA(nullptr, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1)
        return (int)dm.dmDisplayFrequency;
    return 60;
}

void FrameLimiterInit() {
    g_isV10 = memcmp((const void*)kMaxFPSInitSite, kMaxFPSInit, sizeof kMaxFPSInit) == 0;
    if (g_cfg.fpsLimit > 0) g_cap = g_cfg.fpsLimit;
    else if (g_cfg.fpsLimit == 0) g_cap = DesktopRefresh();   // refined once the device exists
    else g_cap = 0;

    QueryPerformanceFrequency(&g_freq);
    // Windows 10 1803+: sub-millisecond timer that doesn't change the system-wide
    // timer resolution.
    g_timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    g_highResTimer = g_timer != nullptr;
    if (!g_timer) {
        g_timer = CreateWaitableTimerW(nullptr, TRUE, nullptr);
        if (HMODULE winmm = LoadLibraryA("winmm.dll"))
            if (auto begin = (UINT(WINAPI*)(UINT))GetProcAddress(winmm, "timeBeginPeriod")) begin(1);
    }
    Log("FrameLimiter: %s, limit %d FPS", g_isV10 ? "v1.0" : "unknown exe (game limiter left alone)", g_cap);
}

void FrameLimiterSetRefresh(int hz) {
    if (g_cfg.fpsLimit == 0 && hz > 1) {
        g_cap = hz;
        g_nextFrame = 0;
        Log("FrameLimiter: display runs at %d Hz, limit %d FPS", hz, g_cap);
    }
}

// ---------------------------------------------------------------- timing figures
// Every 10 s while frames are drawn between updates: frame interval (average /
// worst), frames that took over 1.5 display intervals, time blocked in Present
// (long = the driver waits for the display's refresh: vsync), and how long game
// updates and in-between frames take. A smooth picture needs every frame,
// update frames included, inside one display interval.
struct FrameStats {
    LONGLONG start, last;
    int frames, slow, ticks, draws;
    LONGLONG maxGap, present, presentMax, tick, tickMax, draw, drawMax;
};
static FrameStats g_fs;
static int g_fsReports = 0;

void FrameStatsPresent(long long blocked) {
    if (g_fsReports >= 30) return;
    g_fs.present += blocked;
    if (blocked > g_fs.presentMax) g_fs.presentMax = blocked;
}

void FrameStatsWork(bool tick, long long taken) {
    if (g_fsReports >= 30) return;
    if (tick) { g_fs.ticks++; g_fs.tick += taken; if (taken > g_fs.tickMax) g_fs.tickMax = taken; }
    else { g_fs.draws++; g_fs.draw += taken; if (taken > g_fs.drawMax) g_fs.drawMax = taken; }
}

static void FrameStatsFrame(LONGLONG now) {
    if (g_fsReports >= 30 || !g_freq.QuadPart) return;
    if (!g_fs.start) { memset(&g_fs, 0, sizeof g_fs); g_fs.start = g_fs.last = now; return; }
    const LONGLONG gap = now - g_fs.last;
    g_fs.last = now;
    g_fs.frames++;
    if (gap > g_fs.maxGap) g_fs.maxGap = gap;
    if (g_cap > 0 && gap * g_cap * 2 > g_freq.QuadPart * 3) g_fs.slow++;
    if (now - g_fs.start < g_freq.QuadPart * 10) return;
    if (g_fs.draws > 0 && g_fs.ticks > 0) {
        const double ms = 1000.0 / (double)g_freq.QuadPart;
        g_fsReports++;
        Log("Frames: %.0f FPS, worst gap %.1f ms, %d of %d over 1.5 refreshes; Present %.2f ms avg %.1f max; "
            "update frames %.2f ms avg %.1f max, in-between frames %.2f ms avg %.1f max",
            g_fs.frames / ((now - g_fs.start) * ms / 1000.0), g_fs.maxGap * ms, g_fs.slow, g_fs.frames,
            g_fs.present * ms / g_fs.frames, g_fs.presentMax * ms,
            g_fs.tick * ms / g_fs.ticks, g_fs.tickMax * ms, g_fs.draw * ms / g_fs.draws, g_fs.drawMax * ms);
    }
    memset(&g_fs, 0, sizeof g_fs);
    g_fs.start = g_fs.last = now;
}

// With vsync the display paces the frames itself. A second pacer on our own clock
// at the nominal rate (144 Hz) drifts against the display's real refresh (never
// exactly 144.000), and every couple of seconds a frame missed its refresh and
// the picture held for two (13-15 ms gaps). So with FPSLimit=0 ("the display's
// rate") and vsync on, Present alone paces the frames - but only if it really
// does: a driver setting can force vsync off (Present then never waits and the
// game ran at 400-600 FPS on a 144 Hz display). Each second without the limiter
// is counted; well over the refresh rate means vsync isn't holding the frames,
// and the limiter takes over again for the session.
static bool g_vsyncHolds = true;
static LONGLONG g_vsCheckStart = 0;
static int g_vsCheckFrames = 0;

void FrameLimiterSetVsync(bool on) {
    if (on != g_vsync) {
        g_vsync = on;
        g_nextFrame = 0;
        g_vsyncHolds = true;
        g_vsCheckStart = 0;
        if (g_cfg.fpsLimit == 0)
            Log(on ? "FrameLimiter: vsync is on; the display paces the frames" : "FrameLimiter: vsync is off; frames paced at %d FPS", g_cap);
    }
}

// Frames are shown at exact, evenly spaced times. Waiting after Present (as before)
// spaced the frame *starts* evenly, but a frame with a game update in it takes a
// few ms longer to draw, so it reached the screen later than the moment it shows:
// moving things (a car seen from the side) shuddered 30 times a second. Now each
// frame gets its present time ("slot") when it starts, is drawn for that moment
// (fixedtick.cpp blends for it), and is held until the slot before Present.
static bool g_slotTaken = false;   // this frame's slot (g_nextFrame) is fixed

static bool Pacing() {
    return g_cap > 0 && !(g_vsync && g_vsyncHolds && g_cfg.fpsLimit == 0);
}

bool FrameLimiterPacing() { return Pacing(); }

long long FrameLimiterFrameTime() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (!Pacing()) return now.QuadPart;
    if (g_slotTaken) return g_nextFrame > now.QuadPart ? g_nextFrame : now.QuadPart;
    const LONGLONG period = g_freq.QuadPart / g_cap;
    // first frame, or behind (loading, alt-tab, a slow frame): start over from now
    if (g_nextFrame == 0 || now.QuadPart - g_nextFrame > period / 2) g_nextFrame = now.QuadPart;
    if (g_nextFrame < now.QuadPart) g_nextFrame = now.QuadPart;
    g_slotTaken = true;
    return g_nextFrame;
}

void FrameLimiterBeforePresent() {
    // Every frame: RsInitialise writes maxFPS = 30 after this DLL loads, and another
    // mod can write it again later. The Widescreen Fix keeps its own limits in the
    // same variable (its MenuFrameLimit in menus, the game's 30 when the menu's Frame
    // Limiter is on), and with it installed the game ran at 30 FPS: frames are paced
    // here (FPSLimit), so the game's busy loop must never wait.
    if (g_isV10 && *MaxFPS != 1000) {
        if (g_maxFpsApplied) {
            static int logged = 0;
            if (logged++ < 3)
                Log("FrameLimiter: another mod set the game's frame cap to %d FPS; put back to unlimited (VC Modern Fix paces frames, FPSLimit)", *MaxFPS);
        }
        *MaxFPS = 1000;
    }
    g_maxFpsApplied = true;
    if (!Pacing()) { g_slotTaken = false; return; }
    if (!g_slotTaken) FrameLimiterFrameTime();
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    const LONGLONG margin = g_freq.QuadPart / (g_highResTimer ? 3000 : 1000);   // 0.33 ms / 1 ms
    LONGLONG remaining = g_nextFrame - now.QuadPart;
    if (g_timer && remaining > margin) {
        LARGE_INTEGER due;
        due.QuadPart = -((remaining - margin) * 10000000LL / g_freq.QuadPart);  // relative, 100 ns units
        if (SetWaitableTimer(g_timer, &due, 0, nullptr, nullptr, FALSE))
            WaitForSingleObject(g_timer, INFINITE);
    }
    while (now.QuadPart < g_nextFrame) {
        YieldProcessor();
        QueryPerformanceCounter(&now);
    }
}

void FrameLimiterAfterPresent() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    FrameStatsFrame(now.QuadPart);
    if (g_slotTaken) {
        g_slotTaken = false;
        if (g_cap > 0) g_nextFrame += g_freq.QuadPart / g_cap;
    }
    if (g_cap > 0 && g_vsync && g_vsyncHolds && g_cfg.fpsLimit == 0) {
        if (!g_vsCheckStart) { g_vsCheckStart = now.QuadPart; g_vsCheckFrames = 0; return; }
        g_vsCheckFrames++;
        const LONGLONG took = now.QuadPart - g_vsCheckStart;
        if (took < g_freq.QuadPart) return;
        const double fps = g_vsCheckFrames * (double)g_freq.QuadPart / (double)took;
        g_vsCheckStart = now.QuadPart;
        g_vsCheckFrames = 0;
        if (fps < g_cap * 1.15) return;
        g_vsyncHolds = false;
        g_nextFrame = 0;
        Log("FrameLimiter: vsync isn't holding the frames (%.0f FPS, probably forced off in the graphics driver); frames paced at %d FPS", fps, g_cap);
    }
}
