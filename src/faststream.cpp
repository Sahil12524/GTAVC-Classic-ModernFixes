// Faster model and texture streaming ([Streaming] FastLoadMs).
//
// The game streams models and their textures (TXDs) from models\gta3.img in
// CStreaming::Update (0x40EE80, once per game update), which runs one step of its
// loader each time (inlined at 0x40EF43):
//   if loading a big model (0xA10B75): channel = 0
//   if the channel's read finished: ProcessLoadingChannel(channel) (0x40BEB0) -
//       turns the data read into models and textures
//   if no channel error (0x94DB60 == -1):
//       if the channel is idle: RequestModelStream(channel) (0x40B8A0) - starts
//           the next read of as many requested files as fit
//       if it isn't waiting for the second half of a big model: switch channel
// (channel 0x6F7754, its first-use flag 0x6F7758; channel states 0 idle, 1
// reading, 2 big model half done, at 0x8116C8 + 0x38 * channel). So a batch takes
// at least two updates to arrive, and only one batch is in flight per channel: at
// 30 updates a second, a quick drive or flight outruns it and models and textures
// show up late or blurry. CdStream reads on its own thread, and gta3.img is held in
// RAM by preload.cpp, so a read finishes in well under a millisecond.
//
// Before each game update this runs the same step again, yielding to the read
// thread between steps, for up to FastLoadMs milliseconds while a channel has a
// read in progress or anything waits to be loaded - and stops as soon as both
// channels are idle with nothing requested. ProcessLoadingChannel only acts on a
// finished read (it asks CdStreamGetStatus 0x408480 first), so nothing waits on
// the disk. The game itself runs the same steps back to back when it loads
// everything at once (CStreaming::LoadAllRequestedModels).
#include "faststream.h"
#include "config.h"
#include <windows.h>
#include <string.h>

static int* const Channel = (int*)0x6F7754;
static volatile unsigned char* const ChannelInit = (unsigned char*)0x6F7758;
static volatile unsigned char* const LoadingBigModel = (unsigned char*)0xA10B75;
static volatile int* const ChannelError = (int*)0x94DB60;
static const DWORD CHANNEL_STATE = 0x8116C8, CHANNEL_SIZE = 0x38;
static const DWORD MENU_ACTIVE = 0x869668;
static const auto ProcessLoadingChannel = (bool (__cdecl*)(int))0x40BEB0;
static const auto RequestModelStream = (void (__cdecl*)(int))0x40B8A0;
// CStreaming::ms_numModelsRequested (CStreaming::RequestModel increments it at 0x40E492)
static volatile int* const g_numRequested = (int*)0x975354;

static bool g_on = false, g_fromTick = false;
static LARGE_INTEGER g_freq;
static unsigned g_steps = 0, g_runs = 0;

static int State(int ch) { return *(volatile int*)(CHANNEL_STATE + CHANNEL_SIZE * ch); }

static void Step() {
    if (*LoadingBigModel) *Channel = 0;
    int ch = *Channel;
    const int st = State(ch);
    if (st == 1 || st == 2) ProcessLoadingChannel(ch);
    if (*ChannelError == -1) {
        if (State(ch) == 0) RequestModelStream(ch);
        if (State(ch) != 2) *Channel = 1 - ch;
    }
}

static void Run() {
    if (!g_on || g_cfg.fastLoadMs <= 0 || !*ChannelInit || *(volatile unsigned char*)MENU_ACTIVE) return;
    LARGE_INTEGER start, now;
    QueryPerformanceCounter(&start);
    const LONGLONG budget = g_freq.QuadPart * g_cfg.fastLoadMs / 1000;
    bool ran = false;
    for (int i = 0; i < 400; i++) {
        const bool busy = State(0) != 0 || State(1) != 0;
        const bool waiting = *g_numRequested > 0;
        if (!busy && !waiting) break;                 // nothing to load
        Step();
        g_steps++;
        ran = true;
        if (State(0) == 0 && State(1) == 0) break;     // no read started: nothing loadable now
        QueryPerformanceCounter(&now);
        if (now.QuadPart - start.QuadPart >= budget) break;
        if (State(0) == 1 || State(1) == 1) SwitchToThread();   // let the read thread finish its read
    }
    if (ran) g_runs++;
}

void FastStreamBeforeTick() { g_fromTick = true; Run(); }
void FastStreamAfterPresent() { if (!g_fromTick) Run(); }   // FixedTick off: once per frame

// No island loading screen ([Streaming] NoIslandScreen). Crossing to the other
// island, CStreaming's level change (0x40DFB0, from CStreaming::Update) stops the
// clock and, before loading the new island's buildings and collision, shows the
// "Greetings from Vice City" splash: LoadingIslandScreen (0x4A68A0: load the
// splash 0x4A6E80, draw and present it 0x4A72C0) called twice, at 0x40E00E and
// 0x40E01C (push name; call; pop ecx later). Those two calls are replaced with 5-byte
// nops; the loading itself is untouched (with the files in RAM it takes a moment).
static const DWORD kIslandSites[] = { 0x40E00E, 0x40E01C };
static unsigned char g_islandOrig[2][5];
static bool g_islandOk = false;

// On (5-byte nops) or the game's calls, as [Streaming] NoIslandScreen has it now (the
// Modern Fix menu changes it while playing).
void NoIslandScreenApply() {
    if (!g_islandOk) return;
    static const unsigned char nop5[] = { 0x0F, 0x1F, 0x44, 0x00, 0x00 };
    for (int i = 0; i < 2; i++) {
        const DWORD s = kIslandSites[i];
        const unsigned char* want = g_cfg.noIslandScreen ? nop5 : g_islandOrig[i];
        if (!memcmp((void*)s, want, 5)) continue;
        DWORD old;
        VirtualProtect((void*)s, 5, PAGE_EXECUTE_READWRITE, &old);
        memcpy((void*)s, want, 5);
        VirtualProtect((void*)s, 5, old, &old);
        FlushInstructionCache(GetCurrentProcess(), (void*)s, 5);
    }
}

static void InstallNoIslandScreen() {
    const DWORD* sites = kIslandSites;
    for (int i = 0; i < 2; i++) {
        const DWORD s = sites[i];
        const unsigned char* p = (const unsigned char*)s;
        // push eax; call LoadingIslandScreen (the pop ecx for it follows later)
        if (p[0] != 0xE8 || s + 5 + *(const int*)(p + 1) != 0x4A68A0 || p[-1] != 0x50) {
            Log("Streaming: island loading screen code is not v1.0 or was changed by another mod; left as is");
            return;
        }
    }
    for (int i = 0; i < 2; i++) memcpy(g_islandOrig[i], (void*)sites[i], 5);
    g_islandOk = true;
    NoIslandScreenApply();
    Log("Streaming: no loading screen when crossing between islands (%s)", g_cfg.noIslandScreen ? "on" : "off");
}

void InstallFastStream() {
    InstallNoIslandScreen();
    // installed even at 0: the Modern Fix menu (fixmenu.cpp) turns it on and off
    // The game's loader step, as checked above (0x40EF43...0x40EFE7).
    static const unsigned char step[] = {
        0x80, 0x3D, 0x58, 0x77, 0x6F, 0x00, 0x00,   // cmp byte [6F7758h], 0
    };
    static const unsigned char process[] = { 0x53, 0x56, 0x57, 0x55, 0x83, 0xEC, 0x10, 0xFF, 0x74, 0x24, 0x24, 0xE8 };
    static const unsigned char request[] = { 0x53, 0x56, 0x57, 0x55, 0x83, 0xEC, 0x20 };
    const bool calls = *(const unsigned char*)0x40EF96 == 0xE8 && 0x40EF9B + *(const int*)0x40EF97 == 0x40BEB0 &&
                       *(const unsigned char*)0x40EFC1 == 0xE8 && 0x40EFC6 + *(const int*)0x40EFC2 == 0x40B8A0;
    if (memcmp((void*)0x40EF43, step, sizeof step) || memcmp((void*)0x40BEB0, process, sizeof process) ||
        memcmp((void*)0x40B8A0, request, sizeof request) || !calls ||
        memcmp((void*)0x40E492, "\xFF\x05\x54\x53\x97\x00", 6)) {
        Log("FastStream: streaming code is not v1.0 or was changed by another mod; off");
        return;
    }
    QueryPerformanceFrequency(&g_freq);
    g_on = true;
    Log("FastStream: up to %d ms of extra model / texture loading before each game update (0 = off)", g_cfg.fastLoadMs);
}
