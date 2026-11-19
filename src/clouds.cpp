// Clouds that stay in the sky at any height.
//
// CClouds::Render (0x53FC50, called at 0x4A6570) puts its two cloud layers at fixed
// heights above the sea, whatever the camera's height: the low clouds around the
// camera at 40 + 60 * their z (0x5404C4..0x5404D7), the fluffy clouds and their
// highlight pass at 40 + 40 * their z (0x54069D..0x5406B0, 0x540AE9..0x540AFC): all
// between about 40 and 160 m. From a helicopter or a rooftop above that they were
// below the camera - they sank to the horizon and the sky above was empty. (The
// moon, the stars and the rest are placed from the camera already.)
//
// The 40 m those heights start from (the fadd of [0x6954AC] in each of the three)
// reads a value here instead: 40 m, plus however far the camera is above 30 m. On
// the ground the clouds are exactly the game's; higher up the layers go up with you,
// so the sky looks the same from any height. It is set right before each frame's
// clouds are drawn, from the camera that frame draws with.
#include "clouds.h"
#include "config.h"
#include <windows.h>
#include <string.h>

static const DWORD RENDER_SITE = 0x4A6570, CLOUDS_RENDER = 0x53FC50, CAM_Z = 0x7E46C0;
static const DWORD kFadds[] = { 0x5404D1, 0x5406AA, 0x540AF6 };   // fadd dword ptr [0x6954AC]
static float g_base = 40.0f;
static DWORD g_renderFn = CLOUDS_RENDER;

static void __cdecl HookCloudsRender() {
    const float z = *(const float*)CAM_Z;
    g_base = 40.0f + (g_cfg.risingClouds && z > 30.0f && z < 20000.0f ? z - 30.0f : 0.0f);
    ((void (__cdecl*)())g_renderFn)();
}

static void Write(DWORD at, const void* p, size_t n) {
    DWORD old;
    VirtualProtect((void*)at, n, PAGE_EXECUTE_READWRITE, &old);
    memcpy((void*)at, p, n);
    VirtualProtect((void*)at, n, old, &old);
}

void InstallClouds() {
    static const unsigned char fadd[] = { 0xD8, 0x05, 0xAC, 0x54, 0x69, 0x00 };
    const unsigned char* site = (const unsigned char*)RENDER_SITE;
    bool ok = site[0] == 0xE8;
    for (DWORD a : kFadds) ok = ok && !memcmp((void*)a, fadd, sizeof fadd);
    if (!ok) { Log("Clouds: cloud code is not v1.0 (or changed by another mod); clouds as in the game"); return; }
    g_renderFn = (DWORD)(RENDER_SITE + 5 + *(const int*)(site + 1));
    for (DWORD a : kFadds) {
        const float* p = &g_base;
        Write(a + 2, &p, 4);
    }
    unsigned char call[5] = { 0xE8 };
    *(int*)(call + 1) = (int)((DWORD)&HookCloudsRender - (RENDER_SITE + 5));
    Write(RENDER_SITE, call, 5);
    FlushInstructionCache(GetCurrentProcess(), nullptr, 0);
    Log("Clouds: the cloud layers rise with the camera above 30 m (they sank below a high camera)");
}
