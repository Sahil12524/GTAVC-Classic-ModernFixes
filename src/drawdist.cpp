// Streaming memory and draw distance.
//
// Streaming memory. CStreaming::Init (0x410780) gives the model streamer 45 MB
// (CStreaming::ms_memoryAvailable, 0x94DD54, set at 0x410799) and keeps 25 vehicle
// models loaded (desiredNumVehiclesLoaded, 0x6867A0, set at 0x41078F). With so
// little, models and their textures are thrown out and loaded again all the time:
// buildings and cars show up late, low-detail first. The two values written there
// are replaced ([Streaming] MemoryMB, VehicleModels; the vehicle list holds at
// most 50). Patched in the code before the game starts (DllMain), and set directly
// at the first frame in case Init already ran. Only the main exe's own reads use
// them; gta-vc.exe is large-address-aware, so there is room.
//
// Draw distance. Each frame CCamera::Process (0x46DE..) works out
// LODDistMultiplier (TheCamera+0xF0, 70 / FOV, more when high up, at most 2.2) and
// multiplies it by CRenderer::ms_lodDistScale (0x690220, the menu's Draw Distance
// slider) at 0x46DEC3. Every model's distance (from the IDE files), the switch
// from a building to its low-detail LOD, peds' low-detail and fade distances and
// vehicle components all scale with it. That fmul reads a float of this DLL
// instead: the slider's value times [DrawDistance] Scale.
// Vehicles' own low-detail and fade distances (CVisibilityPlugins::
// SetRenderWareCamera, 0x581840: 70, 90, 100 m, big vehicles 60 and 150 m, squared
// at 0x974B00, 0x974B10, 0x97508C, 0x975200, 0x975210) use
// GenerationDistMultiplier (TheCamera+0xF4) instead, which also sets where traffic
// and peds are created and removed, so it is left alone: those five distances get
// the same Scale after the function runs (its 5 callers, 0x4A5CEA...).
#include "drawdist.h"
#include "config.h"
#include <windows.h>
#include <string.h>

static const DWORD INIT_VEHICLES_IMM = 0x410795, INIT_MEMORY_IMM = 0x41079F;
static int* const MemoryAvailable = (int*)0x94DD54;
static int* const DesiredVehicles = (int*)0x6867A0;
static const DWORD LOD_FMUL = 0x46DEC3;   // fmul dword ptr [0x690220]
static float* const LodDistScale = (float*)0x690220;
static const DWORD SET_RW_CAMERA = 0x581840;
static const DWORD kCameraSites[] = { 0x4A5CEA, 0x4A5EBC, 0x4A5FBC, 0x4A60E8, 0x4A7350 };
static float* const kVehicleDists[] = { (float*)0x974B00, (float*)0x974B10, (float*)0x97508C,
                                        (float*)0x975200, (float*)0x975210 };

// The 300 m limit. CRenderer's LOD_DISTANCE (0x69022C, 300.0, read in 28 places
// in ScanWorld / SetupEntityVisibility, 0x4C6EC1...): beyond it the world scan
// only looks at big low-detail LOD buildings, and full-detail models past it are
// pushed to their fade distance. So no multiplier shows full detail past 300 m;
// it becomes 300 m times Scale. More full-detail models in view means more entries
// in CRenderer's visible list (0x7D54F8, 2000 entries, count 0xA0D1E4), which the
// game fills without checking: its three writes (0x4C7613, 0x4C77ED, 0x4C85AD:
// mov eax,[count]; inc [count]; mov [list+eax*4], reg) call a stub that stops at
// 1990 instead.
static float* const LodDistance = (float*)0x69022C;
static const DWORD kVisAdds[] = { 0x4C7613, 0x4C77ED, 0x4C85AD };
extern "C" { void vcmf_vis_add_ebp(); void vcmf_vis_add_esi(); }
__asm__(".text\n"
        ".globl _vcmf_vis_add_ebp\n_vcmf_vis_add_ebp:\n"
        "  movl 0xA0D1E4, %eax\n  cmpl $1990, %eax\n  jae 1f\n"
        "  movl %ebp, 0x7D54F8(,%eax,4)\n  incl 0xA0D1E4\n1: ret\n"
        ".globl _vcmf_vis_add_esi\n_vcmf_vis_add_esi:\n"
        "  movl 0xA0D1E4, %eax\n  cmpl $1990, %eax\n  jae 2f\n"
        "  movl %esi, 0x7D54F8(,%eax,4)\n  incl 0xA0D1E4\n2: ret\n");

// The far clip. Idle (0x4A6032) gives the RenderWare camera the weather's far clip
// (CTimeCycle::m_fCurrentFarClip, 0x9B6A6C) each frame: RwCameraSetFarClipPlane
// (0x64A930) is called with it times [DrawDistance] FarClip instead, so the city
// is seen further before the fog. The weather's own value is left alone.
static const DWORD FARCLIP_SITE = 0x4A6032, RW_SET_FAR = 0x64A930;
static DWORD g_setFarFn = 0;

static void* __cdecl HookSetFarClip(void* camera, float farClip) {
    return ((void* (__cdecl*)(void*, float))g_setFarFn)(camera, farClip * g_cfg.farClip);
}

static float g_lodScale = 1.2f;   // read by the patched fmul
static DWORD g_setCameraFn = 0;
static bool g_drawOk = false;

static void Write(DWORD site, const void* bytes, int len) {
    DWORD old;
    VirtualProtect((void*)site, len, PAGE_EXECUTE_READWRITE, &old);
    memcpy((void*)site, bytes, len);
    VirtualProtect((void*)site, len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)site, len);
}

static DWORD CallTarget(DWORD site) {
    const unsigned char* p = (const unsigned char*)site;
    return p[0] == 0xE8 ? site + 5 + *(const int*)(p + 1) : 0;
}

static int MemoryBytes() { return g_cfg.streamMemoryMB * 1024 * 1024; }

// DllMain: before CStreaming::Init runs.
void PatchStreamingEarly() {
    static const unsigned char vehicles[] = { 0xC7, 0x05, 0xA0, 0x67, 0x68, 0x00, 0x19, 0x00, 0x00, 0x00 };
    static const unsigned char memory[]   = { 0xC7, 0x05, 0x54, 0xDD, 0x94, 0x00, 0x00, 0x00, 0xD0, 0x02 };
    if (g_cfg.vehicleModels && !memcmp((void*)(INIT_VEHICLES_IMM - 6), vehicles, sizeof vehicles))
        Write(INIT_VEHICLES_IMM, &g_cfg.vehicleModels, 4);
    if (g_cfg.streamMemoryMB && !memcmp((void*)(INIT_MEMORY_IMM - 6), memory, sizeof memory)) {
        const int bytes = MemoryBytes();
        Write(INIT_MEMORY_IMM, &bytes, 4);
    }
}

static void __cdecl HookSetCamera(void* camera) {
    ((void (__cdecl*)(void*))g_setCameraFn)(camera);
    const float s = g_cfg.drawDistance;
    for (float* d : kVehicleDists) *d *= s * s;   // squared distances
}

void InstallDrawDistance() {
    // Init may have run already (it writes its own values): set them now too.
    if (g_cfg.streamMemoryMB && *MemoryAvailable < MemoryBytes()) *MemoryAvailable = MemoryBytes();
    if (g_cfg.vehicleModels && *DesiredVehicles < g_cfg.vehicleModels) *DesiredVehicles = g_cfg.vehicleModels;
    Log("Streaming: %d MB for models (game: 45), %d vehicle models kept loaded (game: 25)",
        *MemoryAvailable / (1024 * 1024), *DesiredVehicles);

    // far clip (independent of Scale)
    static const unsigned char farPush[] = { 0xFF, 0x35, 0x6C, 0x6A, 0x9B, 0x00, 0x50 };   // push [farclip]; push eax
    if (g_cfg.farClip != 1.0f) {
        const DWORD t = CallTarget(FARCLIP_SITE);
        if (t && !memcmp((void*)(FARCLIP_SITE - 7), farPush, sizeof farPush)) {
            if (t != RW_SET_FAR) Log("DrawDistance: far clip call hooked by another mod (%08lX); chaining to it", t);
            g_setFarFn = t;
            unsigned char call[5] = { 0xE8 };
            *(int*)(call + 1) = (int)((DWORD)&HookSetFarClip - (FARCLIP_SITE + 5));
            Write(FARCLIP_SITE, call, 5);
            Log("DrawDistance: far clip x%.2f of the weather's", g_cfg.farClip);
        } else {
            Log("DrawDistance: far clip code is not v1.0; not changed");
        }
    }

    if (g_cfg.drawDistance == 1.0f) { Log("DrawDistance: game's own"); return; }
    static const unsigned char fmul[] = { 0xD8, 0x0D, 0x20, 0x02, 0x69, 0x00 };
    bool camOk = true;
    const DWORD target = CallTarget(kCameraSites[0]);
    for (DWORD s : kCameraSites) camOk = camOk && CallTarget(s) == target && target;
    if (memcmp((void*)LOD_FMUL, fmul, sizeof fmul) || !camOk) {
        Log("DrawDistance: camera code is not v1.0 (or hooked by another mod); not changed");
        return;
    }
    g_lodScale = *LodDistScale * g_cfg.drawDistance;
    unsigned char patched[6] = { 0xD8, 0x0D };
    float* p = &g_lodScale;
    memcpy(patched + 2, &p, 4);
    Write(LOD_FMUL, patched, 6);
    g_setCameraFn = target;
    if (target != SET_RW_CAMERA) Log("DrawDistance: vehicle distances hooked by another mod (%08lX); chaining to it", target);
    for (DWORD s : kCameraSites) {
        unsigned char call[5] = { 0xE8 };
        *(int*)(call + 1) = (int)((DWORD)&HookSetCamera - (s + 5));
        Write(s, call, 5);
    }
    g_drawOk = true;

    // the 300 m limit, with the visible list guarded first
    static const unsigned char add[] = { 0xA1, 0xE4, 0xD1, 0xA0, 0x00, 0xFF, 0x05, 0xE4, 0xD1, 0xA0, 0x00 };
    bool addsOk = *LodDistance == 300.0f;
    for (DWORD s : kVisAdds)
        addsOk = addsOk && !memcmp((void*)s, add, sizeof add) && *(DWORD*)(s + 14) == 0x7D54F8;
    // 89 2C 85 (ebp) / 89 34 85 (esi), then the list address
    for (DWORD s : kVisAdds) {
        const unsigned char* p = (const unsigned char*)s + 11;
        addsOk = addsOk && p[0] == 0x89 && (p[1] == 0x2C || p[1] == 0x34) && p[2] == 0x85;
    }
    if (addsOk) {
        for (DWORD s : kVisAdds) {
            const bool esi = ((const unsigned char*)s)[12] == 0x34;
            unsigned char code[18];
            memset(code, 0x90, sizeof code);
            code[0] = 0xE8;
            *(int*)(code + 1) = (int)((DWORD)(esi ? &vcmf_vis_add_esi : &vcmf_vis_add_ebp) - (s + 5));
            Write(s, code, sizeof code);
        }
        *LodDistance = 300.0f * g_cfg.drawDistance;
        Log("DrawDistance: full detail out to %.0f m (game: 300 m); visible list guarded at 1990 of 2000",
            *LodDistance);
    } else {
        Log("DrawDistance: renderer code is not v1.0; the 300 m limit stays");
    }
    Log("DrawDistance: x%.2f on top of the menu's Draw Distance (buildings, LODs, peds, vehicles)", g_cfg.drawDistance);
}

// Every frame: follow the menu slider.
void DrawDistanceFrame() {
    if (g_drawOk) g_lodScale = *LodDistScale * g_cfg.drawDistance;
}
