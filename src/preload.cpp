// Keeps the files the game reads while you play in RAM. The game reads some data
// synchronously: every ped speech line and mission audio clip is an fseek + fread
// of up to 100 KB at a random place in audio\SFX.RAW (cSampleManager, 0x5D6F49 and
// 0x5D7D39), and the streaming code waits for models from models\gta3.img when it
// needs them right away (0x408410). On a hard disk each of those reads is a seek:
// 50-150 ms during which the game thread does nothing - a frozen frame.
//
// A thread reads both files once, start to end, at background I/O priority (the
// game's own loading goes first) but normal memory priority, then maps them into
// the process and touches every page. The mapped views stay for the whole session,
// so the pages are part of the game's working set and Windows does not drop them
// the way it drops plain file-cache pages; the game's own reads of those files are
// served from the same physical pages. It costs address space (about 640 MB of the
// 4 GB the large-address-aware exe has) and RAM, nothing else: no game code is
// changed. If a file can't be mapped, it still gets the plain file-cache warm-up.
// The views are only kept when the first read shows a slow disk (500 ms or more for
// the file): read in tens of ms, the file was already in the Windows file cache (or
// is on a fast SSD), and the game's reads are served from there without the views.
// The radio and other audio streams are read into the file cache afterwards.
#include "preload.h"
#include "config.h"
#include <windows.h>

static const char* const kFiles[] = { "audio\\SFX.RAW", "models\\gta3.img" };

// The views take about 640 MB of address space for the whole session. That only
// fits when gta-vc.exe is large address aware (4 GB on 64-bit Windows; without the
// flag the game has 2 GB, and the model memory, MemoryMB, plus the game's own data
// need most of it). So the files are mapped only when the exe has the flag and the
// address space left still holds the views, the model memory still to be used and
// 512 MB for everything else; otherwise they only get the file-cache warm-up.
static bool g_mapFiles = true;

static unsigned long long FileBytes(const char* dir, char* slash) {
    unsigned long long total = 0;
    for (const char* name : kFiles) {
        lstrcpyA(slash, name);
        WIN32_FILE_ATTRIBUTE_DATA a;
        if (GetFileAttributesExA(dir, GetFileExInfoStandard, &a))
            total += (unsigned long long)a.nFileSizeHigh << 32 | a.nFileSizeLow;
    }
    return total;
}

static void MapsFit(const char* dir, char* slash) {
    const unsigned char* exe = (const unsigned char*)GetModuleHandleA(nullptr);
    const IMAGE_NT_HEADERS* nt = (const IMAGE_NT_HEADERS*)(exe + ((const IMAGE_DOS_HEADER*)exe)->e_lfanew);
    if (!(nt->FileHeader.Characteristics & IMAGE_FILE_LARGE_ADDRESS_AWARE)) {
        g_mapFiles = false;
        Log("Preload: gta-vc.exe is not large address aware (2 GB); files only warmed in the file cache, not kept mapped "
            "(SetLargeAddressAware.ps1 sets the flag)");
        return;
    }
    MEMORYSTATUSEX ms = { sizeof ms };
    if (!GlobalMemoryStatusEx(&ms)) return;
    const unsigned long long files = FileBytes(dir, slash);
    const unsigned long long models = (unsigned long long)(g_cfg.streamMemoryMB > 0 ? g_cfg.streamMemoryMB : 45) << 20;
    const unsigned long long need = files + models + (512ull << 20);
    if (ms.ullAvailVirtual < need) {
        g_mapFiles = false;
        Log("Preload: %u MB of address space left, %u MB needed for the mapped files (%u MB), model memory (%u MB) and "
            "512 MB headroom; files only warmed in the file cache", (unsigned)(ms.ullAvailVirtual >> 20),
            (unsigned)(need >> 20), (unsigned)(files >> 20), (unsigned)(models >> 20));
    }
}

// Once a minute, how much memory the game uses: its committed private memory, the
// RAM it has in use, and the address space left of its 2 or 4 GB.
typedef struct { DWORD cb, PageFaultCount; SIZE_T PeakWorkingSetSize, WorkingSetSize, QuotaPeakPagedPoolUsage,
                 QuotaPagedPoolUsage, QuotaPeakNonPagedPoolUsage, QuotaNonPagedPoolUsage, PagefileUsage,
                 PeakPagefileUsage, PrivateUsage; } MemCounters;
static DWORD WINAPI MemoryLogThread(void*) {
    typedef BOOL (WINAPI* GetPMI_t)(HANDLE, MemCounters*, DWORD);
    auto get = (GetPMI_t)GetProcAddress(GetModuleHandleA("kernel32.dll"), "K32GetProcessMemoryInfo");
    for (;;) {
        Sleep(60000);
        MEMORYSTATUSEX ms = { sizeof ms };
        GlobalMemoryStatusEx(&ms);
        MemCounters pm = { sizeof pm };
        if (get && get(GetCurrentProcess(), &pm, sizeof pm))
            Log("Memory: %u MB committed, %u MB in RAM (peak %u MB), %u of %u MB address space free",
                (unsigned)(pm.PrivateUsage >> 20), (unsigned)(pm.WorkingSetSize >> 20), (unsigned)(pm.PeakWorkingSetSize >> 20),
                (unsigned)(ms.ullAvailVirtual >> 20), (unsigned)(ms.ullTotalVirtual >> 20));
        else
            Log("Memory: %u of %u MB address space free", (unsigned)(ms.ullAvailVirtual >> 20), (unsigned)(ms.ullTotalVirtual >> 20));
    }
}

static void NormalMemoryPriority() {
    // Background mode lowers memory priority too, which would put the cached pages
    // first in line to be discarded. ThreadMemoryPriority (Windows 8+).
    typedef BOOL (WINAPI* SetThreadInformation_t)(HANDLE, int, LPVOID, DWORD);
    auto set = (SetThreadInformation_t)GetProcAddress(GetModuleHandleA("kernel32.dll"), "SetThreadInformation");
    if (!set) return;
    ULONG priority = 5;   // MEMORY_PRIORITY_NORMAL
    set(GetCurrentThread(), 0 /* ThreadMemoryPriority */, &priority, sizeof priority);
}

// The reads wait until the game has finished loading. Started at DLL load, they ran
// while the game read its own data files and models\gta3.img for the start: on a
// hard disk (the owner's E: is a 2.5" laptop HDD) the 1.3 GB read here and the game's
// boot reads pulled the disk head back and forth, and a cold start took 20-50 s on
// the loading screen instead of 5-7 s. Background I/O priority does not stop that: the
// game's reads still wait for the seek away from this thread's position. So the thread
// sleeps until the game state is 9 (playing) and has stayed so for 3 s; the menu, the
// intro videos and the loading screen have the disk to themselves.
static const DWORD GAME_STATE = 0x9B5F08;   // 9 = playing
static void WaitForGameLoaded() {
    LARGE_INTEGER f, t0, t1;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    DWORD playingSince = 0;
    for (;;) {
        const DWORD now = GetTickCount();
        if (*(volatile int*)GAME_STATE == 9) {
            if (!playingSince) playingSince = now;
            else if (now - playingSince >= 3000) break;
        } else playingSince = 0;
        Sleep(100);
    }
    QueryPerformanceCounter(&t1);
    Log("Preload: started %u s after launch, once the game had been playing for 3 s (the intro, menu and loading "
        "screen read the disk alone)", (unsigned)((t1.QuadPart - t0.QuadPart) / f.QuadPart));
}

static DWORD WINAPI PreloadThread(void*) {
    WaitForGameLoaded();
    SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);   // low CPU and I/O priority
    NormalMemoryPriority();
    const DWORD chunk = 1 << 20;
    void* buf = VirtualAlloc(nullptr, chunk, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!buf) return 0;
    char dir[MAX_PATH];
    GetModuleFileNameA(nullptr, dir, MAX_PATH);
    char* slash = dir + lstrlenA(dir);
    while (slash > dir && slash[-1] != '\\') slash--;
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    MapsFit(dir, slash);
    for (const char* name : kFiles) {
        lstrcpyA(slash, name);
        HANDLE h = CreateFileA(dir, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) { Log("Preload: %s not found, skipped", name); continue; }
        LARGE_INTEGER t0, t1, size;
        QueryPerformanceCounter(&t0);
        unsigned long long total = 0;
        DWORD got;
        while (ReadFile(h, buf, chunk, &got, nullptr) && got) total += got;
        QueryPerformanceCounter(&t1);
        const unsigned readMs = (unsigned)((t1.QuadPart - t0.QuadPart) * 1000 / f.QuadPart);

        // keep it resident: a read-only view of the whole file, every page touched.
        // Only when the read shows a slow disk. A read under kCachedMs (300 MB in tens
        // of ms: already in the Windows file cache, or an SSD) means the game's own reads
        // of the file are served from the cache or a fast disk anyway: the view would
        // only add the same pages to the game's working set and take 300 MB of its
        // address space, so the file is left to the file cache.
        static const unsigned kCachedMs = 500;
        const bool fastRead = readMs < kCachedMs;
        bool kept = false;
        DWORD err = 0;
        if (!g_mapFiles || fastRead) {
            // map nothing (MapsFit said why, or the read was fast); the read above warmed the file cache
        } else if (GetFileSizeEx(h, &size) && size.HighPart == 0 && size.LowPart) {
            HANDLE map = CreateFileMappingA(h, nullptr, PAGE_READONLY, 0, 0, nullptr);
            if (!map) err = GetLastError();
            if (map) {
                if (const volatile BYTE* view = (const volatile BYTE*)MapViewOfFile(map, FILE_MAP_READ, 0, 0, 0)) {
                    BYTE sum = 0;
                    for (DWORD o = 0; o < size.LowPart; o += 4096) sum += view[o];
                    (void)sum;
                    kept = true;   // the view is never unmapped
                } else err = GetLastError();
                CloseHandle(map);   // the view keeps the mapping alive
            }
        }
        CloseHandle(h);
        Log("Preload: %s, %u MB read in %u ms, %s", name, (unsigned)(total >> 20), readMs,
            kept ? "slow disk (over 500 ms): kept in memory for the session"
                 : fastRead ? "cached or fast disk (under 500 ms): left to the Windows file cache, not kept mapped"
                            : "in the file cache only");
        if (!kept && g_mapFiles && !fastRead) Log("Preload: mapping %s failed, error %lu", name, err);
    }
    // Radio stations (.adf) and the ambience / cutscene streams (.mp3): about 700 MB,
    // read by the sound system a few KB at a time while they play, partly on the game
    // thread (0x5D58E0, the game's stream read callback). Too big to keep mapped in a
    // 32-bit process, so they only get the file-cache warm-up.
    static const char* const kStreams[] = { "audio\\*.adf", "audio\\*.mp3" };
    unsigned long long streamBytes = 0;
    unsigned streamFiles = 0;
    LARGE_INTEGER s0, s1;
    QueryPerformanceCounter(&s0);
    for (const char* pattern : kStreams) {
        lstrcpyA(slash, pattern);
        WIN32_FIND_DATAA fd;
        HANDLE find = FindFirstFileA(dir, &fd);
        if (find == INVALID_HANDLE_VALUE) continue;
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            lstrcpyA(slash, "audio\\");
            lstrcatA(slash, fd.cFileName);
            HANDLE h = CreateFileA(dir, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                   OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h == INVALID_HANDLE_VALUE) continue;
            DWORD got;
            while (ReadFile(h, buf, chunk, &got, nullptr) && got) streamBytes += got;
            CloseHandle(h);
            streamFiles++;
        } while (FindNextFileA(find, &fd));
        FindClose(find);
    }
    QueryPerformanceCounter(&s1);
    Log("Preload: %u audio stream files, %u MB read in %u ms (in the Windows file cache)", streamFiles,
        (unsigned)(streamBytes >> 20), (unsigned)((s1.QuadPart - s0.QuadPart) * 1000 / f.QuadPart));

    VirtualFree(buf, 0, MEM_RELEASE);
    SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_END);
    return 0;
}

void StartPreload() {
    if (HANDLE t = CreateThread(nullptr, 0, MemoryLogThread, nullptr, 0, nullptr)) CloseHandle(t);
    if (!g_cfg.preload) { Log("Preload: off"); return; }
    if (HANDLE t = CreateThread(nullptr, 0, PreloadThread, nullptr, 0, nullptr)) CloseHandle(t);
}
