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

static volatile LONG g_crashLogs = 0;   // crash log below: exceptions logged so far

// With its window in the background (alt-tab, minimised) the game's main loop
// waits in WaitMessage (call [0x6F253C] at 0x6004E1, returning to 0x6004E7) until
// a window message comes: no heartbeat, but not a freeze. That return address near
// the top of the game thread's stack means it is just waiting.
static bool WaitingInBackground() {
    if (SuspendThread(g_gameThread) == (DWORD)-1) return false;
    bool waiting = false;
    CONTEXT c = {};
    c.ContextFlags = CONTEXT_CONTROL;
    if (GetThreadContext(g_gameThread, &c)) {
        for (int i = 0; i < 64 && !waiting; i++) {
            DWORD v;
            if (!ReadProcessMemory(GetCurrentProcess(), (void*)(c.Esp + i * 4), &v, 4, nullptr)) break;
            waiting = v == 0x6004E7;
        }
    }
    ResumeThread(g_gameThread);
    return waiting;
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
        // After a crash the game thread sits in the game's "Unhandled exception"
        // message box; dumps of that only bury the EXCEPTION lines. One line, then
        // the watchdog stops for the rest of the session (its thread ends: no more
        // polling once a fatal exception has been logged and the game stopped).
        if (stuckMs >= 3000 && g_crashLogs > 0) {
            RawLog("WATCHDOG: no progress after the crash above (the game's crash message box); watchdog stopped");
            return 0;
        }
        if (stuckMs >= 3000 && dumps < 3) {
            if (WaitingInBackground()) { stuckMs = 0; continue; }   // alt-tabbed, not frozen
            Dump(++dumps);
            stuckMs = -3000;   // next dump after 6 s more, if still stuck
        }
    }
}

// ---------------------------------------------------------------- crash log
// The game's own crash handler (0x677E40, its unhandled-exception filter) only
// shows "Unhandled exception" in a message box. A vectored handler sees the
// exception first and writes where it happened to VCModernFix.log: code, address
// (module + offset), what was read or written, registers, and return addresses
// into gta-vc.exe or a DLL on the stack. It always passes the exception on
// unchanged (handlers that expect one, like ExitGuard's, still get it), so a line
// here followed by the game carrying on means it was handled. At most 4 a session
// (g_crashLogs, above).

static void ModuleOf(DWORD a, char* out, int size, DWORD* offset) {
    HMODULE m = nullptr;
    out[0] = 0; *offset = a;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)a, &m) || !m) { lstrcpynA(out, "?", size); return; }
    char path[MAX_PATH];
    GetModuleFileNameA(m, path, MAX_PATH);
    const char* base = path + lstrlenA(path);
    while (base > path && base[-1] != '\\') base--;
    lstrcpynA(out, base, size);
    *offset = a - (DWORD)m;
}

// A minidump of the first crash of a session, next to the log:
// VCModernFix-crash-YYYYMMDD-HHMMSS.dmp (threads, stacks, the game's global data),
// written with dbghelp.dll's MiniDumpWriteDump, loaded only when needed. Only the
// newest 3 dumps are kept.
typedef BOOL (WINAPI* MiniDumpWriteDump_t)(HANDLE, DWORD, HANDLE, int, void*, void*, void*);
struct DumpExceptionInfo { DWORD ThreadId; EXCEPTION_POINTERS* ExceptionPointers; BOOL ClientPointers; };
static volatile LONG g_dumped = 0;

static void PruneDumps(const char* dir) {
    char pattern[MAX_PATH], oldest[MAX_PATH] = "";
    for (;;) {
        wsprintfA(pattern, "%sVCModernFix-crash-*.dmp", dir);
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA(pattern, &fd);
        if (h == INVALID_HANDLE_VALUE) return;
        int count = 0;
        char first[MAX_PATH] = "";
        do {   // names sort by date: the smallest is the oldest
            count++;
            if (!first[0] || lstrcmpA(fd.cFileName, first) < 0) lstrcpynA(first, fd.cFileName, MAX_PATH);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
        if (count <= 3) return;
        wsprintfA(oldest, "%s%s", dir, first);
        if (!DeleteFileA(oldest)) return;
    }
}

static void WriteDump(EXCEPTION_POINTERS* e) {
    if (InterlockedExchange(&g_dumped, 1)) return;
    char dir[MAX_PATH], path[MAX_PATH], line[MAX_PATH + 64];
    lstrcpynA(dir, g_logPath, MAX_PATH);
    char* slash = dir + lstrlenA(dir);
    while (slash > dir && slash[-1] != '\\') slash--;
    *slash = 0;
    SYSTEMTIME t;
    GetLocalTime(&t);
    wsprintfA(path, "%sVCModernFix-crash-%04d%02d%02d-%02d%02d%02d.dmp", dir, t.wYear, t.wMonth, t.wDay,
              t.wHour, t.wMinute, t.wSecond);
    HMODULE dbg = LoadLibraryA("dbghelp.dll");
    MiniDumpWriteDump_t write = dbg ? (MiniDumpWriteDump_t)GetProcAddress(dbg, "MiniDumpWriteDump") : nullptr;
    if (!write) { RawLog("  crash dump: dbghelp.dll not available"); return; }
    HANDLE f = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) { RawLog("  crash dump: could not create the file"); return; }
    DumpExceptionInfo info = { GetCurrentThreadId(), e, FALSE };
    const int kind = 0x1 | 0x40 | 0x800;   // MiniDumpWithDataSegs | WithIndirectlyReferencedMemory | WithThreadInfo
    const BOOL ok = write(GetCurrentProcess(), GetCurrentProcessId(), f, kind, &info, nullptr, nullptr);
    CloseHandle(f);
    if (!ok) DeleteFileA(path);
    wsprintfA(line, ok ? "  crash dump: %s" : "  crash dump: writing %s failed", path);
    RawLog(line);
    PruneDumps(dir);
}

// A stack value is a return address when the bytes just before it are a call:
// E8 rel32, or FF /2 (call r/m32) in its 2, 3, 6 and 7 byte forms.
static bool AfterCall(DWORD v) {
    unsigned char b[7];
    if (!ReadProcessMemory(GetCurrentProcess(), (void*)(v - 7), b, 7, nullptr)) return false;
    if (b[2] == 0xE8) return true;                                                 // call rel32
    if (b[5] == 0xFF && (b[6] & 0x38) == 0x10 && (b[6] & 0xC0) == 0xC0) return true;   // call reg
    if (b[5] == 0xFF && (b[6] & 0x38) == 0x10 && (b[6] & 0xC7) <= 0x07 && (b[6] & 7) != 4 && (b[6] & 7) != 5) return true;   // call [reg]
    if (b[4] == 0xFF && (b[5] & 0x38) == 0x10 && (b[5] & 0xC0) == 0x40) return true;   // call [reg+disp8]
    if (b[1] == 0xFF && (b[2] & 0x38) == 0x10 && ((b[2] & 0xC0) == 0x80 || b[2] == 0x15)) return true;   // [reg+disp32], [abs]
    if (b[0] == 0xFF && (b[1] & 0x38) == 0x10 && (b[1] & 7) == 4 && (b[1] & 0xC0) == 0x80) return true;  // [sib+disp32]
    return false;
}

// Up to 32 return addresses into gta-vc.exe, this DLL (*) and VCTrainer.asi (T), from
// the top 1024 stack slots, each with its slot (+offset from ESP).
static void LogReturnAddresses(DWORD esp, HMODULE self) {
    const HMODULE trainer = GetModuleHandleA("VCTrainer.asi");
    char line[1024];
    int len = lstrlenA(lstrcpyA(line, "  return addresses:")), found = 0;
    for (int i = 0; i < 1024 && found < 32; i++) {
        DWORD v;
        if (!ReadProcessMemory(GetCurrentProcess(), (void*)(esp + i * 4), &v, 4, nullptr)) break;
        const bool exe = v >= 0x401000 && v < 0x67E000;
        const char* tag = exe ? "" : InModule(v, self) ? "*" : trainer && InModule(v, trainer) ? "T" : nullptr;
        if (!tag || !AfterCall(v)) continue;
        len += wsprintfA(line + len, " %08X%s(+%X)", v, tag, i * 4);
        found++;
    }
    if (!found) lstrcatA(line, " none found");
    RawLog(line);
}

static LONG CALLBACK CrashLogger(EXCEPTION_POINTERS* e) {
    const DWORD code = e->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION && code != EXCEPTION_PRIV_INSTRUCTION &&
        code != EXCEPTION_INT_DIVIDE_BY_ZERO && code != EXCEPTION_STACK_OVERFLOW)
        return EXCEPTION_CONTINUE_SEARCH;
    // Not before the game's main loop runs: its start-up code scans memory and
    // handles the access violations that causes (0xA123B4).
    if (!g_watchdogArmed) return EXCEPTION_CONTINUE_SEARCH;
    if (InterlockedIncrement(&g_crashLogs) > 4) return EXCEPTION_CONTINUE_SEARCH;
    const CONTEXT* c = e->ContextRecord;
    char mod[64], line[512], access[64] = "";
    DWORD off;
    ModuleOf(c->Eip, mod, sizeof mod, &off);
    if (code == EXCEPTION_ACCESS_VIOLATION && e->ExceptionRecord->NumberParameters >= 2) {
        const ULONG_PTR kind = e->ExceptionRecord->ExceptionInformation[0];
        wsprintfA(access, ", %s %08X", kind == 0 ? "reading" : kind == 1 ? "writing" : "executing",
                  (DWORD)e->ExceptionRecord->ExceptionInformation[1]);
    }
    wsprintfA(line, "EXCEPTION %08X at %08X (%s+%X)%s, replay playing %d, thread %lu", code, c->Eip, mod, off, access,
              *(volatile unsigned char*)0xA10AF6, GetCurrentThreadId());
    RawLog(line);
    wsprintfA(line, "  EAX %08X EBX %08X ECX %08X EDX %08X ESI %08X EDI %08X EBP %08X ESP %08X",
              c->Eax, c->Ebx, c->Ecx, c->Edx, c->Esi, c->Edi, c->Ebp, c->Esp);
    RawLog(line);
    int len = lstrlenA(lstrcpyA(line, "  stack:"));
    HMODULE self = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)&CrashLogger, &self);
    for (int i = 0; i < 256 && len < 470; i++) {
        DWORD v;
        if (!ReadProcessMemory(GetCurrentProcess(), (void*)(c->Esp + i * 4), &v, 4, nullptr)) break;
        const bool exe = v >= 0x401000 && v < 0x67E000;
        if (exe || InModule(v, self)) len += wsprintfA(line + len, " %08X%s", v, exe ? "" : "*");
    }
    RawLog(line);
    LogReturnAddresses(c->Esp, self);
    WriteDump(e);
    return EXCEPTION_CONTINUE_SEARCH;
}

void StartWatchdog() {
    GetModuleFileNameA(nullptr, g_logPath, MAX_PATH);
    char* slash = g_logPath + lstrlenA(g_logPath);
    while (slash > g_logPath && slash[-1] != '\\') slash--;
    lstrcpyA(slash, "VCModernFix.log");
    AddVectoredExceptionHandler(1, CrashLogger);
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &g_gameThread,
                    THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0);
    if (g_gameThread) CloseHandle(CreateThread(nullptr, 0, WatchdogThread, nullptr, 0, nullptr));
}
