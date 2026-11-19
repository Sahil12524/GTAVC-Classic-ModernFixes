// Which step of the game update takes the time on slow update frames.
//
// CGame::Process (0x4A4410, called from Idle at 0x4A5DA0) runs the whole update as a
// list of 68 direct calls (0x4A4412 ... 0x4A4660: streaming, scripts, the world,
// population, traffic, cameras, audio...). Each of those calls, and the call of
// CGame::Process itself, goes through a small thunk built at start-up (push its
// number; jmp ProfEnter): ProfEnter keeps the return address and the time stamp
// counter on a side stack and calls the real target with the stack and registers
// as the game left them; when it returns, ProfExit adds the cycles to that step and
// returns to the game with eax / edx / st(0) untouched. Nothing else changes, and a
// call that another mod already redirected keeps going to that mod.
//
// After each frame (UpdateProfileAfterPresent): an update over 25 ms is logged with
// its three slowest steps (the first 20, then every 50th), and once a minute the
// five steps that took the most time on average per update.
#include "updateprofile.h"
#include "config.h"
#include <windows.h>
#include <string.h>

static const int kMaxSites = 80;
static const DWORD PROCESS = 0x4A4410, PROCESS_CALL = 0x4A5DA0, FIRST = 0x4A4412, LAST = 0x4A4660;

extern "C" {
    DWORD g_profTarget[kMaxSites];          // where each call went before
    volatile DWORD g_profTick[kMaxSites];   // cycles (low 32 bits) this update, per step
    DWORD g_profRet[8], g_profIdx[8], g_profT0[8], g_profDepth = 0, g_profJmp = 0;
    void vcmf_prof_enter();
    void vcmf_prof_exit();
}
static DWORD g_profSite[kMaxSites];
static int g_nSites = 0;
static bool g_on = false;

// Stack on entry: thunk's number, the game's return address, the call's arguments.
__asm__(".text\n"
        ".globl _vcmf_prof_enter\n_vcmf_prof_enter:\n"
        "  pushl %ecx\n  pushl %edx\n"                       // [esp+8] number, [esp+12] return
        "  movl _g_profDepth, %ecx\n"
        "  cmpl $0, 8(%esp)\n  jne 1f\n  xorl %ecx, %ecx\n"   // CGame::Process starts: a clean side stack
        "1: cmpl $8, %ecx\n  jb 2f\n  movl $7, %ecx\n"       // never past the side stack
        "2: movl 12(%esp), %edx\n  movl %edx, _g_profRet(,%ecx,4)\n"
        "  movl 8(%esp), %edx\n  movl %edx, _g_profIdx(,%ecx,4)\n"
        "  leal 1(%ecx), %edx\n  movl %edx, _g_profDepth\n"
        "  pushl %eax\n  rdtsc\n  movl %eax, _g_profT0(,%ecx,4)\n  popl %eax\n"
        "  movl $_vcmf_prof_exit, 12(%esp)\n"
        "  movl 8(%esp), %edx\n  movl _g_profTarget(,%edx,4), %edx\n  movl %edx, 8(%esp)\n"
        "  popl %edx\n  popl %ecx\n"
        "  ret\n"                                             // to the real target, returning to ProfExit

        ".globl _vcmf_prof_exit\n_vcmf_prof_exit:\n"
        "  pushl %eax\n  pushl %edx\n  pushl %ecx\n  pushl %ebx\n"
        "  movl _g_profDepth, %ecx\n  testl %ecx, %ecx\n  jz 3f\n  decl %ecx\n  movl %ecx, _g_profDepth\n"
        "3: rdtsc\n  subl _g_profT0(,%ecx,4), %eax\n"
        "  movl _g_profIdx(,%ecx,4), %ebx\n  addl %eax, _g_profTick(,%ebx,4)\n"
        "  movl _g_profRet(,%ecx,4), %eax\n  movl %eax, _g_profJmp\n"
        "  popl %ebx\n  popl %ecx\n  popl %edx\n  popl %eax\n"
        "  jmp *_g_profJmp\n");

static double g_cyclesPerMs = 0;
static LARGE_INTEGER g_q0;
static unsigned long long g_c0 = 0;
static unsigned long long g_sum[kMaxSites];
static double g_maxMs[kMaxSites];
static unsigned g_updates = 0, g_slow = 0, g_slowLogs = 0;
static DWORD g_summaryAt = 0;

static void Calibrate() {
    LARGE_INTEGER q, f;
    QueryPerformanceCounter(&q);
    const unsigned long long c = __builtin_ia32_rdtsc();
    if (!g_c0) { g_c0 = c; g_q0 = q; return; }
    QueryPerformanceFrequency(&f);
    const double ms = (q.QuadPart - g_q0.QuadPart) * 1000.0 / f.QuadPart;
    if (ms > 2000.0) g_cyclesPerMs = (c - g_c0) / ms;
}

static const char* StepName(DWORD target) {
    switch (target) {
    case 0x4A4410: return "CGame::Process";
    case 0x44FED0: return "scripts";
    case 0x53E5F0: return "population";
    case 0x4292A0: return "traffic cars";
    case 0x42BB70: return "emergency cars";
    }
    return "";
}

void UpdateProfileAfterPresent() {
    if (!g_on) return;
    if (g_cyclesPerMs <= 0) { Calibrate(); if (g_cyclesPerMs <= 0) { memset((void*)g_profTick, 0, sizeof g_profTick); return; } }
    const DWORD total = g_profTick[0];
    if (!total) return;   // no update since the last frame
    DWORD tick[kMaxSites];
    memcpy(tick, (const void*)g_profTick, sizeof tick);
    memset((void*)g_profTick, 0, sizeof g_profTick);
    g_updates++;
    for (int i = 0; i < g_nSites; i++) {
        g_sum[i] += tick[i];
        const double ms = tick[i] / g_cyclesPerMs;
        if (ms > g_maxMs[i]) g_maxMs[i] = ms;
    }
    const double totalMs = total / g_cyclesPerMs;
    if (totalMs > 25.0) {
        g_slow++;
        if (g_slowLogs < 20 || g_slow % 50 == 0) {
            g_slowLogs++;
            int top[3] = { -1, -1, -1 };
            for (int i = 1; i < g_nSites; i++)
                for (int k = 0; k < 3; k++)
                    if (top[k] < 0 || tick[i] > tick[top[k]]) {
                        for (int m = 2; m > k; m--) top[m] = top[m - 1];
                        top[k] = i;
                        break;
                    }
            char line[300]; int n = 0;
            for (int k = 0; k < 3; k++)
                if (top[k] > 0)
                    n += wsprintfA(line + n, "%X %s (call at %X) %d.%d ms; ", g_profTarget[top[k]], StepName(g_profTarget[top[k]]),
                                   g_profSite[top[k]], (int)(tick[top[k]] / g_cyclesPerMs),
                                   (int)(tick[top[k]] * 10 / g_cyclesPerMs) % 10);
            Log("UpdateProfile: slow update %d.%d ms (%u so far): %s", (int)totalMs, (int)(totalMs * 10) % 10, g_slow, line);
        }
    }
    const DWORD now = GetTickCount();
    if (!g_summaryAt) g_summaryAt = now;
    if (now - g_summaryAt >= 60000 && g_updates) {
        g_summaryAt = now;
        int top[5] = { -1, -1, -1, -1, -1 };
        for (int i = 1; i < g_nSites; i++)
            for (int k = 0; k < 5; k++)
                if (top[k] < 0 || g_sum[i] > g_sum[top[k]]) {
                    for (int m = 4; m > k; m--) top[m] = top[m - 1];
                    top[k] = i;
                    break;
                }
        char line[500]; int n = 0;
        for (int k = 0; k < 5; k++)
            if (top[k] > 0) {
                const double avg = g_sum[top[k]] / g_cyclesPerMs / g_updates;
                n += wsprintfA(line + n, "%X %s %d.%02d ms (max %d.%d); ", g_profTarget[top[k]], StepName(g_profTarget[top[k]]),
                               (int)avg, (int)(avg * 100) % 100, (int)g_maxMs[top[k]], (int)(g_maxMs[top[k]] * 10) % 10);
            }
        const double all = g_sum[0] / g_cyclesPerMs / g_updates;
        Log("UpdateProfile: %u updates, CGame::Process %d.%02d ms avg (max %d.%d), %u over 25 ms; most time: %s",
            g_updates, (int)all, (int)(all * 100) % 100, (int)g_maxMs[0], (int)(g_maxMs[0] * 10) % 10, g_slow, line);
        memset(g_sum, 0, sizeof g_sum);
        for (double& m : g_maxMs) m = 0;
        g_updates = 0;
        g_slow = 0;
    }
}

static bool AddSite(unsigned char* thunks, DWORD at) {
    const unsigned char* p = (const unsigned char*)at;
    if (p[0] != 0xE8 || g_nSites >= kMaxSites) return false;
    const DWORD target = at + 5 + *(const int*)(p + 1);
    const int i = g_nSites++;
    g_profTarget[i] = target;
    g_profSite[i] = at;
    unsigned char* t = thunks + 10 * i;
    t[0] = 0x68; *(DWORD*)(t + 1) = (DWORD)i;                                    // push i
    t[5] = 0xE9; *(int*)(t + 6) = (int)((DWORD)&vcmf_prof_enter - ((DWORD)t + 10));   // jmp ProfEnter
    return true;
}

static void Redirect(unsigned char* thunks, int i) {
    unsigned char call[5] = { 0xE8 };
    *(int*)(call + 1) = (int)((DWORD)(thunks + 10 * i) - (g_profSite[i] + 5));
    DWORD old;
    VirtualProtect((void*)g_profSite[i], 5, PAGE_EXECUTE_READWRITE, &old);
    memcpy((void*)g_profSite[i], call, 5);
    VirtualProtect((void*)g_profSite[i], 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)g_profSite[i], 5);
}

// Last of all the installs, so every other hook on these calls is in place first.
void InstallUpdateProfile() {
    // CGame::Process as v1.0 has it: push ebx; push ebp; its calls from 0x4A4412 to
    // 0x4A4660 and ret at 0x4A4667. The calls' targets aren't checked: the first one
    // (CPad::UpdatePads) is hooked by the trainer and hunter.cpp, and is chained.
    const unsigned char* pc = (const unsigned char*)PROCESS_CALL;
    const unsigned char* body = (const unsigned char*)PROCESS;
    if (pc[0] != 0xE8 || body[0] != 0x53 || body[1] != 0x55 || *(const unsigned char*)FIRST != 0xE8 ||
        *(const unsigned char*)LAST != 0xE8 || *(const unsigned char*)0x4A4667 != 0xC3) {
        Log("UpdateProfile: CGame::Process is not v1.0 (or another mod changed it); update steps not timed");
        return;
    }
    unsigned char* thunks = (unsigned char*)VirtualAlloc(nullptr, 10 * kMaxSites, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!thunks) return;
    AddSite(thunks, PROCESS_CALL);   // number 0: the whole update
    // The calls of the body, found by walking its instructions (lengths of the forms
    // it uses, checked against the disassembly): every E8 between FIRST and LAST.
    static const DWORD kCalls[] = {
        0x4A4412, 0x4A4417, 0x4A4433, 0x4A4438, 0x4A448A, 0x4A4491, 0x4A449C, 0x4A44A1, 0x4A44A8, 0x4A44D9,
        0x4A44DE, 0x4A44E3, 0x4A44E8, 0x4A44ED, 0x4A44F2, 0x4A44F7, 0x4A44FC, 0x4A4501, 0x4A4506, 0x4A450B,
        0x4A4510, 0x4A4515, 0x4A451A, 0x4A451F, 0x4A4524, 0x4A4529, 0x4A452E, 0x4A4533, 0x4A4538, 0x4A453D,
        0x4A4542, 0x4A454C, 0x4A4556, 0x4A455D, 0x4A456A, 0x4A4570, 0x4A4577, 0x4A4588, 0x4A458E, 0x4A459C,
        0x4A45AA, 0x4A45AF, 0x4A45B4, 0x4A45B9, 0x4A45BE, 0x4A45C3, 0x4A45C8, 0x4A45D2, 0x4A45D7, 0x4A45DC,
        0x4A45E1, 0x4A45E6, 0x4A45EB, 0x4A45F0, 0x4A45F5, 0x4A45FA, 0x4A4608, 0x4A460D, 0x4A461B, 0x4A4620,
        0x4A4625, 0x4A462A, 0x4A462F, 0x4A4634, 0x4A463E, 0x4A4651, 0x4A4656, 0x4A465B, 0x4A4660 };
    int skipped = 0;
    for (DWORD at : kCalls)
        if (!AddSite(thunks, at)) skipped++;
    for (int i = 0; i < g_nSites; i++) Redirect(thunks, i);
    g_on = true;
    Log("UpdateProfile: %d steps of the game update timed; slow updates (over 25 ms) are logged with their slowest steps%s",
        g_nSites - 1, skipped ? " (some calls not v1.0, skipped)" : "");
}
