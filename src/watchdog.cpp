// Freeze diagnostics. A background thread watches a heartbeat the game thread
// bumps every loop iteration; if it stops for 3 seconds, the game thread is
// briefly suspended and where it is stuck (EIP, registers, return addresses on
// its stack, a few EIP samples to show a loop) is appended to VCModernFix.log,
// written with plain Win32 calls (no C library, whose locks the stuck thread may
// hold). Does nothing while the game runs normally.
#include "watchdog.h"
#include <windows.h>

volatile LONG g_heartbeat = 0;
volatile bool g_watchdogArmed = false;
static HANDLE g_gameThread = nullptr;
static char g_logPath[MAX_PATH];

static void RawLog(const char* text) {
    HANDLE h = CreateFileA(g_logPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD n;
    WriteFile(h, text, lstrlenA(text), &n, nullptr);
    WriteFile(h, "\r\n", 2, &n, nullptr);
    CloseHandle(h);
}

static bool InModule(DWORD a, HMODULE m) {
    MEMORY_BASIC_INFORMATION mbi;
    return VirtualQuery((void*)a, &mbi, sizeof mbi) && mbi.AllocationBase == (void*)m;
}

static void Dump(int which) {
    HMODULE self = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)&Dump, &self);
    char line[512];
    wsprintfA(line, "WATCHDOG: game thread made no progress for 3 s (dump %d). d3d8.dll base %08X", which, (DWORD)self);
    RawLog(line);

    for (int sample = 0; sample < 8; sample++) {
        if (SuspendThread(g_gameThread) == (DWORD)-1) return;
        CONTEXT c = {};
        c.ContextFlags = CONTEXT_FULL;
        if (GetThreadContext(g_gameThread, &c)) {
            if (sample == 0) {
                wsprintfA(line, "  EIP %08X ESP %08X EBP %08X EAX %08X EBX %08X ECX %08X EDX %08X ESI %08X EDI %08X",
                          c.Eip, c.Esp, c.Ebp, c.Eax, c.Ebx, c.Ecx, c.Edx, c.Esi, c.Edi);
                RawLog(line);
                // return addresses into gta-vc.exe or this DLL among the top 512 stack slots
                int len = lstrlenA(lstrcpyA(line, "  stack:"));
                for (int i = 0; i < 512 && len < 480; i++) {
                    DWORD v;
                    if (!ReadProcessMemory(GetCurrentProcess(), (void*)(c.Esp + i * 4), &v, 4, nullptr)) break;
                    bool exeCode = v >= 0x401000 && v < 0x67E000;
                    if (exeCode || InModule(v, self)) len += wsprintfA(line + len, " %08X%s", v, exeCode ? "" : "*");
                }
                RawLog(line);
            } else {
                wsprintfA(line, "  sample %d: EIP %08X ESP %08X", sample, c.Eip, c.Esp);
                RawLog(line);
            }
        }
        ResumeThread(g_gameThread);
        Sleep(50);
    }
}

static DWORD WINAPI WatchdogThread(void*) {
    LONG last = g_heartbeat;
    int stuckMs = 0, dumps = 0;
    for (;;) {
        Sleep(250);
        if (!g_watchdogArmed) continue;   // intro movies play outside the frame loop
        LONG now = g_heartbeat;
        if (now != last) { last = now; stuckMs = 0; continue; }
        stuckMs += 250;
        if (stuckMs >= 3000 && dumps < 3) {
            Dump(++dumps);
            stuckMs = -3000;   // next dump after 6 s more, if still stuck
        }
    }
}

void StartWatchdog() {
    GetModuleFileNameA(nullptr, g_logPath, MAX_PATH);
    char* slash = g_logPath + lstrlenA(g_logPath);
    while (slash > g_logPath && slash[-1] != '\\') slash--;
    lstrcpyA(slash, "VCModernFix.log");
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &g_gameThread,
                    THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0);
    if (g_gameThread) CloseHandle(CreateThread(nullptr, 0, WatchdogThread, nullptr, 0, nullptr));
}
