// d3d8.dll proxy: forwards Direct3DCreate8 to the real system DLL, hooks the
// Direct3D 8 device (refresh rate, frame limiter) and installs the fixed-tick
// game loop.
//
// gta-vc.exe imports only Direct3DCreate8 from d3d8.dll, so that is the only
// export needed. This DLL is independent of any dinput8.dll mod: if another
// mod later hooks Present too, it chains through this hook.
#include <windows.h>
#include <d3d8.h>
#include <string.h>
#include "config.h"
#include "fixedtick.h"
#include "framelimiter.h"
#include "watchdog.h"
#include "mousefix.h"
#include "preload.h"
#include "freecam.h"
#include "mouselook.h"
#include "quicksave.h"
#include "phoneskip.h"
#include "interior.h"
#include "drawdist.h"
#include "widescreen.h"
#include "viewscan.h"
#include "clouds.h"
#include "farlights.h"
#include "gameplay.h"
#include "exitguard.h"
#include "population.h"
#include "stuntjump.h"
#include "missionhelp.h"
#include "aircontrol.h"
#include "hunter.h"
#include "faststream.h"
#include "driveby.h"
#include "alphafix.h"
#include "fixmenu.h"
#include "modernkeys.h"
#include "onfootcam.h"
#include "pizza.h"
#include "updateprofile.h"
#include "timing.h"

typedef IDirect3D8* (WINAPI* Create8_t)(UINT);
typedef HRESULT (__stdcall* CreateDevice_t)(IDirect3D8*, UINT, D3DDEVTYPE, HWND, DWORD,
                                            D3DPRESENT_PARAMETERS*, IDirect3DDevice8**);
typedef HRESULT (__stdcall* Reset_t)(IDirect3DDevice8*, D3DPRESENT_PARAMETERS*);
typedef HRESULT (__stdcall* Present_t)(IDirect3DDevice8*, const RECT*, const RECT*, HWND, const RGNDATA*);

static CreateDevice_t g_origCreateDevice = nullptr;
static Reset_t g_origReset = nullptr;
static Present_t g_origPresent = nullptr;
static SetViewport_t g_origSetViewport = nullptr;

// Replaces one vtable entry and returns the old one. Vtables are shared by
// every object of the class, so this is done only once per class.
static void* PatchVtable(void* object, int index, void* hook) {
    void** vtbl = *(void***)object;
    DWORD old;
    VirtualProtect(&vtbl[index], sizeof(void*), PAGE_EXECUTE_READWRITE, &old);
    void* orig = vtbl[index];
    vtbl[index] = hook;
    VirtualProtect(&vtbl[index], sizeof(void*), old, &old);
    return orig;
}

// The game asks for 60 Hz in fullscreen even on a 144/240 Hz display. Pick the
// highest rate the display offers for the game's resolution (or the one set in
// VCModernFix.ini).
static void ChooseRefreshRate(IDirect3D8* d3d, UINT adapter, D3DPRESENT_PARAMETERS* pp) {
    if (!d3d || !pp || pp->Windowed) return;
    const UINT requested = pp->FullScreen_RefreshRateInHz;
    UINT best = 0;
    UINT count = d3d->GetAdapterModeCount(adapter);
    for (UINT i = 0; i < count; i++) {
        D3DDISPLAYMODE m;
        if (FAILED(d3d->EnumAdapterModes(adapter, i, &m))) continue;
        if (m.Width != pp->BackBufferWidth || m.Height != pp->BackBufferHeight || m.Format != pp->BackBufferFormat) continue;
        if (g_cfg.refreshHz ? m.RefreshRate == (UINT)g_cfg.refreshHz : m.RefreshRate > best) best = m.RefreshRate;
    }
    if (best) pp->FullScreen_RefreshRateInHz = best;
    Log("Display: %ux%u fullscreen, game asked for %u Hz, using %u Hz, presentation interval %08X (0 / 1 = vsync, "
        "80000000 = no vsync)", pp->BackBufferWidth, pp->BackBufferHeight, requested, pp->FullScreen_RefreshRateInHz,
        (unsigned)pp->FullScreen_PresentationInterval);
}

// FPSLimit=-1 means unlimited, but the game asks for vsync (interval 1) when it
// resets the device. Windows' own D3D8 often ran past it; Direct3D 9 / DXVK
// (d3d8to9 chained as d3d8_next.dll) honour it, and the game was held to the
// display's refresh rate. Unlimited asks Direct3D for no vsync - only for the call:
// the game's own parameters keep its interval. Left changed, the game saw its vsync
// "lost" and reset the device again every frame (a black screen). Should resets
// still come in a burst, the game's interval is used from then on.
static bool g_noVsync = true;
static UINT NoVsyncBefore(D3DPRESENT_PARAMETERS* pp) {
    if (!pp || pp->Windowed || g_cfg.fpsLimit != -1 || !g_noVsync) return pp ? pp->FullScreen_PresentationInterval : 0;
    static DWORD burstStart = 0;
    static int burst = 0;
    const DWORD now = GetTickCount();
    if (now - burstStart > 10000) { burstStart = now; burst = 0; }
    if (++burst > 6) {
        g_noVsync = false;
        Log("Display: the game keeps resetting the device with vsync off; its own vsync setting is used from now on");
        return pp->FullScreen_PresentationInterval;
    }
    const UINT game = pp->FullScreen_PresentationInterval;
    pp->FullScreen_PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    return game;
}

static void ReportRefresh(IDirect3DDevice8* dev, const D3DPRESENT_PARAMETERS* pp) {
    FrameLimiterSetVsync(pp && !pp->Windowed && pp->FullScreen_PresentationInterval != D3DPRESENT_INTERVAL_IMMEDIATE);
    if (pp && pp->Windowed) {
        DEVMODEA dm = {};
        dm.dmSize = sizeof dm;
        if (EnumDisplaySettingsA(nullptr, ENUM_CURRENT_SETTINGS, &dm)) FrameLimiterSetRefresh((int)dm.dmDisplayFrequency);
        return;
    }
    D3DDISPLAYMODE mode;
    if (dev && SUCCEEDED(dev->GetDisplayMode(&mode)) && mode.RefreshRate) FrameLimiterSetRefresh((int)mode.RefreshRate);
}

// VCModernFix's own game hooks go in at the first presented frame. By then every
// ASI loader (Ultimate ASI Loader...) has loaded its plugins and SilentPatch has
// applied both its load-time patches and its delayed ones (after rsINITIALISE and
// after the audio library starts, both before the first frame). Where a plugin
// already hooked a place VCModernFix hooks, VCModernFix calls through to it.
static void InstallHooks() {
    InstallMouseFix();
    InstallFixedTick();
    InstallFreeCam();
    InstallFrameMouse();
    InstallQuickSave();
    InstallPhoneSkip();
    InstallInterior();
    InstallDrawDistance();
    InstallWidescreen();
    InstallNearClip();
    InstallClouds();
    InstallViewScan();
    InstallFarLights();
    InstallGameplay();
    InstallExitGuard();
    InstallPopulation();
    InstallStuntJump();
    InstallMissionHelp();
    InstallAirControl();
    InstallModernKeys();
    InstallOnFootCam();
    InstallHunter();
    InstallFastStream();
    InstallDriveBy();
    InstallPizza();
    InstallFixMenu();   // the Modern Fix page under Options
    InstallUpdateProfile();   // last: times the update's steps, through every other hook on them
}

static HRESULT __stdcall HookedPresent(IDirect3DDevice8* dev, const RECT* src, const RECT* dst,
                                       HWND wnd, const RGNDATA* dirty) {
    g_heartbeat++;
    static bool hooksInstalled = false;
    if (!hooksInstalled) { hooksInstalled = true; InstallHooks(); }
    DrawDistanceFrame();
    WidescreenFrame();
    ViewScanLog();
    FarLightsLog();
    FrameLimiterBeforePresent();
    LARGE_INTEGER p0, p1;
    QueryPerformanceCounter(&p0);
    HRESULT hr = g_origPresent(dev, src, dst, wnd, dirty);
    QueryPerformanceCounter(&p1);
    FrameStatsPresent(p1.QuadPart - p0.QuadPart);
    FrameLimiterAfterPresent();
    ModernKeysAfterPresent();   // the game's menu closed: key layout put back
    OnFootCamAfterPresent();    // on-foot look behind off while the free camera is on
    PizzaAfterPresent();        // Pizza Boy: right + left mouse button throws
    {
        const unsigned long long t0 = TimingStart();
        PopulationAfterPresent();   // ped pool headroom for missions, cops and drivers
        TimingEnd(TS_POPULATION, t0);
    }
    UpdateProfileAfterPresent();   // which update step made a slow update frame
    TimingAfterPresent();          // once a minute: where VCModernFix's own time went
    if (!ReplayPlaying()) {   // FixedTick off: once per frame; never during an F1 replay
        QuickSaveAfterPresent();
        InteriorAfterPresent();
        GameplayAfterPresent();
        MissionHelpAfterPresent();
        AirControlAfterPresent();
        HunterAfterPresent();
        FastStreamAfterPresent();
        DriveByAfterPresent();
    }
    return hr;
}

// A failed Reset. RenderWare (0x65D8D9) doesn't check Reset's result: it goes on to
// GetRenderTarget, which on a device that wasn't reset reads a null pointer inside
// Windows' d3d8.dll and the game crashes. Reset fails while the device is still lost
// (the game window not back in front yet), which an overlay (RTSS, Steam, Discord)
// taking or giving back focus at that moment can cause. So it is tried again until
// it works: the window's messages are handled meanwhile, and once the device says it
// can be reset (TestCooperativeLevel: D3DERR_DEVICENOTRESET) Reset runs again, with
// the game's own refresh rate after a few tries in case ours is the problem, then the
// driver's default. A device that stays lost is still tried once a second (a mode
// change that didn't take leaves it lost with the window in front), and every 5 s
// the log says what it is waiting for.
static HRESULT RetryReset(IDirect3DDevice8* dev, D3DPRESENT_PARAMETERS* pp, const D3DPRESENT_PARAMETERS& asked,
                          HRESULT hr) {
    static unsigned logs = 0;
    if (logs < 20) { logs++; Log("Display: Reset failed (%08lX); trying again until the device can be reset", (unsigned long)hr); }
    const DWORD start = GetTickCount();
    DWORD lastTry = start, lastReport = start, lastLoop = start, inFront = 0;
    int tries = 0;
    bool quit = false;
    // Waits as long as the game is in the background (alt-tabbed, minimized): the
    // device stays lost until the player comes back. It gives up after 60 s of the
    // game window in front with Reset still failing, or when the window is closed.
    while (FAILED(hr) && inFront < 60000 && !quit) {
        MSG msg;
        while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) quit = true;
            TranslateMessage(&msg); DispatchMessageA(&msg);
        }
        const HRESULT coop = dev->TestCooperativeLevel();
        const DWORD now = GetTickCount();
        {
            HWND w = *(HWND*)0x7897A4;
            if (w && !IsWindow(w)) quit = true;
            if (w && GetForegroundWindow() == w && !IsIconic(w)) inFront += now - lastLoop;
            lastLoop = now;
        }
        if (now - lastReport >= 5000 && logs < 20) {   // why it waits: for the log
            lastReport = now;
            HWND w = *(HWND*)0x7897A4;
            logs++;
            Log("Display: waiting %lu s: device %08lX, game window %s, %s", (now - start) / 1000, (unsigned long)coop,
                w && GetForegroundWindow() == w ? "in front" : "not in front", w && IsIconic(w) ? "minimized" : "not minimized");
        }
        // Still lost: in the background. Reset is still tried once a second, in case
        // the device stays lost in front (a mode change that didn't take).
        if (coop == D3DERR_DEVICELOST && now - lastTry < 1000) { Sleep(50); continue; }
        lastTry = now;
        D3DPRESENT_PARAMETERS p = ++tries > 3 ? asked : *pp;
        if (tries > 6) p.FullScreen_RefreshRateInHz = 0;   // then the driver's default rate
        hr = g_origReset(dev, &p);
        if (SUCCEEDED(hr)) {
            *pp = p;
            if (logs < 20) { logs++; Log("Display: Reset worked on try %d (%u Hz)", tries + 1, p.FullScreen_RefreshRateInHz); }
            break;
        }
        Sleep(50);
    }
    if (FAILED(hr)) {
        // RenderWare's device restore (0x65D8D9) doesn't check Reset's result: the
        // game would crash inside Windows' d3d8.dll. End it here instead, with a line
        // in the log, rather than a crash a few instructions later.
        Log("Display: Reset still failing (%08lX) after %lu s (%lu s with the game in front)%s; closing the game "
            "instead of crashing in RenderWare (0x65D8D9)", (unsigned long)hr, (GetTickCount() - start) / 1000,
            inFront / 1000, quit ? ", window closed" : "");
        TerminateProcess(GetCurrentProcess(), 1);
    }
    return hr;
}

static HRESULT __stdcall HookedReset(IDirect3DDevice8* dev, D3DPRESENT_PARAMETERS* pp) {
    const D3DPRESENT_PARAMETERS asked = pp ? *pp : D3DPRESENT_PARAMETERS{};
    IDirect3D8* d3d = nullptr;
    D3DDEVICE_CREATION_PARAMETERS cp;
    if (SUCCEEDED(dev->GetDirect3D(&d3d)) && SUCCEEDED(dev->GetCreationParameters(&cp)))
        ChooseRefreshRate(d3d, cp.AdapterOrdinal, pp);
    if (d3d) d3d->Release();
    const UINT gameInterval = NoVsyncBefore(pp);
    HRESULT hr = g_origReset(dev, pp);
    if (FAILED(hr) && pp) hr = RetryReset(dev, pp, asked, hr);
    if (SUCCEEDED(hr)) { AlphaFixReset(); ReportRefresh(dev, pp); }
    if (pp) pp->FullScreen_PresentationInterval = gameInterval;
    return hr;
}

// Interiors: draws the second area a little further away in the depth buffer.
static HRESULT __stdcall HookedSetViewport(IDirect3DDevice8* dev, const D3DVIEWPORT8* vp) {
    return InteriorSetViewport(dev, vp, g_origSetViewport);
}

// Anisotropic filtering ([Graphics] Anisotropic). RenderWare sets each texture's
// filter with SetTextureStageState (vtable 63); where it asks for linear
// minification, anisotropic is used instead, at the level set (at most what the
// card supports), so surfaces seen at an angle (roads, walls, ground) stay sharp.
typedef HRESULT (__stdcall* SetTSS_t)(IDirect3DDevice8*, DWORD, D3DTEXTURESTAGESTATETYPE, DWORD);
static SetTSS_t g_origSetTSS = nullptr;
static DWORD g_cardAniso = 0;  // the card's MaxAnisotropy, 0 = none
// The level in use: [Graphics] Anisotropic, at most what the card does; 0 = off. The
// Modern Fix menu (fixmenu.cpp) changes g_cfg.anisotropy while playing.
static DWORD CurAniso() {
    const DWORD want = g_cfg.anisotropy > 0 ? (DWORD)g_cfg.anisotropy : 0;
    if (!want || g_cardAniso < 2) return 0;
    return want < g_cardAniso ? want : g_cardAniso;
}
// The water is drawn without it (WaterDrawing, around CWaterLevel::RenderWater): its
// wave textures are made to blur at a shallow angle. RenderWare keeps its own copy of
// each stage's filter and doesn't set it again, so the stages it asked to be linear
// are set linear for the water and anisotropic again after.
static IDirect3DDevice8* g_tssDev = nullptr;
static DWORD g_minAsked[8];    // the minification filter RenderWare asked for, per stage
static bool g_inWater = false;

static HRESULT __stdcall HookedSetTSS(IDirect3DDevice8* dev, DWORD stage, D3DTEXTURESTAGESTATETYPE type, DWORD value) {
    if (type == D3DTSS_MINFILTER && stage < 8) { g_minAsked[stage] = value; g_tssDev = dev; }
    const DWORD aniso = CurAniso();
    if (aniso && !g_inWater && type == D3DTSS_MINFILTER && value == D3DTEXF_LINEAR) {
        g_origSetTSS(dev, stage, D3DTSS_MAXANISOTROPY, aniso);
        value = D3DTEXF_ANISOTROPIC;
    }
    return g_origSetTSS(dev, stage, type, value);
}

void WaterDrawing(bool on) {
    if (on == g_inWater) return;
    g_inWater = on;
    if (!CurAniso() || !g_tssDev || !g_origSetTSS) return;
    for (DWORD s = 0; s < 8; s++)
        if (g_minAsked[s] == D3DTEXF_LINEAR)
            g_origSetTSS(g_tssDev, s, D3DTSS_MINFILTER, on ? D3DTEXF_LINEAR : D3DTEXF_ANISOTROPIC);
}

// The menu turned it on or off: the stages RenderWare set to linear get the new
// filter now (RenderWare doesn't set a stage's filter again while it stays the same).
void AnisotropyChanged() {
    if (!g_tssDev || !g_origSetTSS || g_inWater) return;
    const DWORD aniso = CurAniso();
    for (DWORD s = 0; s < 8; s++) {
        if (g_minAsked[s] != D3DTEXF_LINEAR) continue;
        if (aniso) g_origSetTSS(g_tssDev, s, D3DTSS_MAXANISOTROPY, aniso);
        g_origSetTSS(g_tssDev, s, D3DTSS_MINFILTER, aniso ? D3DTEXF_ANISOTROPIC : D3DTEXF_LINEAR);
    }
}

static void ChooseAnisotropy(IDirect3DDevice8* dev) {
    D3DCAPS8 caps;
    g_cardAniso = 0;
    if (FAILED(dev->GetDeviceCaps(&caps))) return;
    const DWORD MINF_ANISOTROPIC = 0x00000400;   // D3DPTFILTERCAPS_MINFANISOTROPY (not in MinGW's d3d8caps.h)
    if (!(caps.TextureFilterCaps & MINF_ANISOTROPIC) || caps.MaxAnisotropy < 2) {
        Log("Graphics: the card has no anisotropic filtering");
        return;
    }
    g_cardAniso = caps.MaxAnisotropy;
    if (CurAniso()) Log("Graphics: anisotropic filtering x%lu (card supports up to x%lu)", CurAniso(), caps.MaxAnisotropy);
    else Log("Graphics: anisotropic filtering off (card supports up to x%lu)", caps.MaxAnisotropy);
}

static HRESULT __stdcall HookedCreateDevice(IDirect3D8* d3d, UINT adapter, D3DDEVTYPE type, HWND wnd,
                                            DWORD flags, D3DPRESENT_PARAMETERS* pp, IDirect3DDevice8** out) {
    ChooseRefreshRate(d3d, adapter, pp);
    const UINT gameInterval = NoVsyncBefore(pp);
    HRESULT hr = g_origCreateDevice(d3d, adapter, type, wnd, flags, pp, out);
    // Changing the resolution in the game's menu makes a new device. Through d3d8to9
    // on Direct3D 9 that failed at once (the fullscreen window just left fullscreen
    // with the old device and wasn't in front yet), and the game went on with no
    // device: a frozen picture. Tried again for up to 5 s with the window brought to
    // the front, the last tries at the driver's default refresh rate.
    if (FAILED(hr) && pp && !pp->Windowed) {
        Log("Display: creating the %ux%u device failed (%08lX); trying again", pp->BackBufferWidth,
            pp->BackBufferHeight, (unsigned long)hr);
        const HWND w = pp->hDeviceWindow ? pp->hDeviceWindow : wnd;
        for (int i = 1; i <= 50 && FAILED(hr); i++) {
            MSG msg;
            while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageA(&msg); }
            if (w) { ShowWindow(w, SW_RESTORE); SetForegroundWindow(w); }
            Sleep(100);
            if (i == 30) pp->FullScreen_RefreshRateInHz = 0;
            hr = g_origCreateDevice(d3d, adapter, type, wnd, flags, pp, out);
            if (SUCCEEDED(hr)) Log("Display: device made on try %d (%u Hz)", i + 1, pp->FullScreen_RefreshRateInHz);
        }
        if (FAILED(hr)) Log("Display: the %ux%u device still can't be made (%08lX)", pp->BackBufferWidth,
                            pp->BackBufferHeight, (unsigned long)hr);
    }
    struct Restore {
        D3DPRESENT_PARAMETERS* pp; UINT v;
        ~Restore() { if (pp) pp->FullScreen_PresentationInterval = v; }
    } restore{ pp, gameInterval };
    if (SUCCEEDED(hr) && out && *out) {
        if (!g_origPresent) {
            g_origReset   = (Reset_t)PatchVtable(*out, 14, (void*)&HookedReset);       // IDirect3DDevice8::Reset
            g_origPresent = (Present_t)PatchVtable(*out, 15, (void*)&HookedPresent);   // IDirect3DDevice8::Present
            g_origSetViewport = (SetViewport_t)PatchVtable(*out, 40, (void*)&HookedSetViewport);   // SetViewport
            g_origSetTSS = (SetTSS_t)PatchVtable(*out, 63, (void*)&HookedSetTSS);   // SetTextureStageState
        }
        InstallAlphaFix(*out, flags, &PatchVtable);
        ChooseAnisotropy(*out);
        if (pp) {
            InteriorDepthFormat((int)pp->AutoDepthStencilFormat);
            Log("Display: depth buffer format %d", (int)pp->AutoDepthStencilFormat);
        }
        ReportRefresh(*out, pp);
    }
    return hr;
}

// The asm label gives the export an undecorated name (no @4) so it matches
// the "Direct3DCreate8" import in gta-vc.exe.
extern "C" __declspec(dllexport) IDirect3D8* WINAPI Direct3DCreate8(UINT sdk) __asm__("_Direct3DCreate8");
extern "C" IDirect3D8* WINAPI Direct3DCreate8(UINT sdk) {
    static Create8_t real = nullptr;
    if (!real) {
        char path[MAX_PATH];
        if (g_cfg.d3d8Chain[0]) {
            // another d3d8.dll (d3d8to9, dgVoodoo...) renamed next to gta-vc.exe
            GetModuleFileNameA(nullptr, path, MAX_PATH);
            if (char* slash = strrchr(path, '\\')) slash[1] = 0;
            lstrcatA(path, g_cfg.d3d8Chain);
            if (HMODULE h = LoadLibraryA(path)) real = (Create8_t)GetProcAddress(h, "Direct3DCreate8");
            if (real) Log("D3D8: passing through to %s", g_cfg.d3d8Chain);
            else Log("D3D8: could not use %s (error %lu); using Windows' d3d8.dll", g_cfg.d3d8Chain, GetLastError());
        }
        if (!real) {
            GetSystemDirectoryA(path, MAX_PATH);   // SysWOW64 for this 32-bit process
            lstrcatA(path, "\\d3d8.dll");
            if (HMODULE h = LoadLibraryA(path)) real = (Create8_t)GetProcAddress(h, "Direct3DCreate8");
        }
        if (!real) return nullptr;
    }
    IDirect3D8* d3d = real(sdk);
    if (d3d && !g_origCreateDevice)
        g_origCreateDevice = (CreateDevice_t)PatchVtable(d3d, 15, (void*)&HookedCreateDevice);   // IDirect3D8::CreateDevice
    return d3d;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        LoadConfig();
        PatchStreamingEarly();   // before CStreaming::Init gives the streamer its 45 MB
        PatchPedPoolEarly();     // before CPools::Initialise makes the ped pool
        PatchCoronaSlotsEarly(); // before CCoronas::Init clears the corona slots
        ApplyCpuAffinity();
        StartWatchdog();
        FrameLimiterInit();
        StartPreload();
        // game hooks: at the first presented frame (HookedPresent)
    }
    return TRUE;
}
