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
// The radio and other audio streams are read into the file cache afterwards.
#include "preload.h"
#include "config.h"
#include <windows.h>

static const char* const kFiles[] = { "audio\\SFX.RAW", "models\\gta3.img" };

static void NormalMemoryPriority() {
    // Background mode lowers memory priority too, which would put the cached pages
    // first in line to be discarded. ThreadMemoryPriority (Windows 8+).
    typedef BOOL (WINAPI* SetThreadInformation_t)(HANDLE, int, LPVOID, DWORD);
    auto set = (SetThreadInformation_t)GetProcAddress(GetModuleHandleA("kernel32.dll"), "SetThreadInformation");
    if (!set) return;
    ULONG priority = 5;   // MEMORY_PRIORITY_NORMAL
    set(GetCurrentThread(), 0 /* ThreadMemoryPriority */, &priority, sizeof priority);
}

static DWORD WINAPI PreloadThread(void*) {
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

        // keep it resident: a read-only view of the whole file, every page touched
        bool kept = false;
        DWORD err = 0;
        if (GetFileSizeEx(h, &size) && size.HighPart == 0 && size.LowPart) {
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
            kept ? "kept in memory for the session" : "in the file cache only");
        if (!kept) Log("Preload: mapping %s failed, error %lu", name, err);
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
    if (!g_cfg.preload) { Log("Preload: off"); return; }
    if (HANDLE t = CreateThread(nullptr, 0, PreloadThread, nullptr, 0, nullptr)) CloseHandle(t);
}
