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
#include "quicksave.h"
#include "phoneskip.h"
#include "interior.h"
#include "drawdist.h"
#include "gameplay.h"
#include "exitguard.h"
#include "population.h"

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
    Log("Display: %ux%u fullscreen, game asked for %u Hz, using %u Hz", pp->BackBufferWidth,
        pp->BackBufferHeight, requested, pp->FullScreen_RefreshRateInHz);
}

static void ReportRefresh(IDirect3DDevice8* dev, const D3DPRESENT_PARAMETERS* pp) {
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
    InstallQuickSave();
    InstallPhoneSkip();
    InstallInterior();
    InstallDrawDistance();
    InstallGameplay();
    InstallExitGuard();
    InstallPopulation();
}

static HRESULT __stdcall HookedPresent(IDirect3DDevice8* dev, const RECT* src, const RECT* dst,
                                       HWND wnd, const RGNDATA* dirty) {
    g_heartbeat++;
    static bool hooksInstalled = false;
    if (!hooksInstalled) { hooksInstalled = true; InstallHooks(); }
    DrawDistanceFrame();
    HRESULT hr = g_origPresent(dev, src, dst, wnd, dirty);
    FrameLimiterWait();
    QuickSaveAfterPresent();
    InteriorAfterPresent();
    GameplayAfterPresent();
    return hr;
}

static HRESULT __stdcall HookedReset(IDirect3DDevice8* dev, D3DPRESENT_PARAMETERS* pp) {
    IDirect3D8* d3d = nullptr;
    D3DDEVICE_CREATION_PARAMETERS cp;
    if (SUCCEEDED(dev->GetDirect3D(&d3d)) && SUCCEEDED(dev->GetCreationParameters(&cp)))
        ChooseRefreshRate(d3d, cp.AdapterOrdinal, pp);
    if (d3d) d3d->Release();
    HRESULT hr = g_origReset(dev, pp);
    if (SUCCEEDED(hr)) ReportRefresh(dev, pp);
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
static DWORD g_maxAniso = 0;   // 0 = off

static HRESULT __stdcall HookedSetTSS(IDirect3DDevice8* dev, DWORD stage, D3DTEXTURESTAGESTATETYPE type, DWORD value) {
    if (g_maxAniso && type == D3DTSS_MINFILTER && value == D3DTEXF_LINEAR) {
        g_origSetTSS(dev, stage, D3DTSS_MAXANISOTROPY, g_maxAniso);
        value = D3DTEXF_ANISOTROPIC;
    }
    return g_origSetTSS(dev, stage, type, value);
}

static void ChooseAnisotropy(IDirect3DDevice8* dev) {
    D3DCAPS8 caps;
    g_maxAniso = 0;
    if (!g_cfg.anisotropy || FAILED(dev->GetDeviceCaps(&caps))) return;
    const DWORD MINF_ANISOTROPIC = 0x00000400;   // D3DPTFILTERCAPS_MINFANISOTROPY (not in MinGW's d3d8caps.h)
    if (!(caps.TextureFilterCaps & MINF_ANISOTROPIC) || caps.MaxAnisotropy < 2) {
        Log("Graphics: the card has no anisotropic filtering");
        return;
    }
    g_maxAniso = (DWORD)g_cfg.anisotropy < caps.MaxAnisotropy ? (DWORD)g_cfg.anisotropy : caps.MaxAnisotropy;
    Log("Graphics: anisotropic filtering x%lu (card supports up to x%lu)", g_maxAniso, caps.MaxAnisotropy);
}

static HRESULT __stdcall HookedCreateDevice(IDirect3D8* d3d, UINT adapter, D3DDEVTYPE type, HWND wnd,
                                            DWORD flags, D3DPRESENT_PARAMETERS* pp, IDirect3DDevice8** out) {
    ChooseRefreshRate(d3d, adapter, pp);
    HRESULT hr = g_origCreateDevice(d3d, adapter, type, wnd, flags, pp, out);
    if (SUCCEEDED(hr) && out && *out) {
        if (!g_origPresent) {
            g_origReset   = (Reset_t)PatchVtable(*out, 14, (void*)&HookedReset);       // IDirect3DDevice8::Reset
            g_origPresent = (Present_t)PatchVtable(*out, 15, (void*)&HookedPresent);   // IDirect3DDevice8::Present
            g_origSetViewport = (SetViewport_t)PatchVtable(*out, 40, (void*)&HookedSetViewport);   // SetViewport
            if (g_cfg.anisotropy)
                g_origSetTSS = (SetTSS_t)PatchVtable(*out, 63, (void*)&HookedSetTSS);   // SetTextureStageState
        }
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
        ApplyCpuAffinity();
        StartWatchdog();
        FrameLimiterInit();
        StartPreload();
        // game hooks: at the first presented frame (HookedPresent)
    }
    return TRUE;
}
