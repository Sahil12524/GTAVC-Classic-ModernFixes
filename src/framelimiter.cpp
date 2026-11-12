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

void FrameLimiterWait() {
    // Done on the first frame: RsInitialise writes maxFPS = 30 after this DLL loads.
    if (!g_maxFpsApplied) {
        if (g_isV10) *MaxFPS = 1000;
        g_maxFpsApplied = true;
    }
    if (g_cap <= 0) return;

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    const LONGLONG period = g_freq.QuadPart / g_cap;

    if (g_nextFrame == 0 || now.QuadPart - g_nextFrame > period) {
        // first frame, or more than a frame behind (loading, alt-tab, a tick that
        // took long): start over instead of rushing to catch up
        g_nextFrame = now.QuadPart + period;
        return;
    }

    const LONGLONG margin = g_freq.QuadPart / (g_highResTimer ? 3000 : 1000);   // 0.33 ms / 1 ms
    LONGLONG remaining = g_nextFrame - now.QuadPart;
    if (g_timer && remaining > margin) {
        LARGE_INTEGER due;
        due.QuadPart = -((remaining - margin) * 10000000LL / g_freq.QuadPart);  // relative, 100 ns units
        if (SetWaitableTimer(g_timer, &due, 0, nullptr, nullptr, FALSE))
            WaitForSingleObject(g_timer, INFINITE);
    }
    do {
        YieldProcessor();
        QueryPerformanceCounter(&now);
    } while (now.QuadPart < g_nextFrame);

    g_nextFrame += period;
}
