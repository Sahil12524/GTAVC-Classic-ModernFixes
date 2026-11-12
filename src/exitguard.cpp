// Crash-free exit.
//
// Quitting runs CGame::ShutDown (0x4A49E0, called at 0x6005C9 when the game is
// playing), which frees every model, texture dictionary and pool one by one before
// the process ends. A texture still in use by something freed later can then be
// freed twice (RwTextureDestroy, 0x64DEC0, unlinking it from a dictionary that is
// already gone, 0x64DEF1): "Unhandled exception c0000005" on exit. Everything that
// matters is already on disk by then (saves are written when made, settings when
// changed), and Windows frees the memory of a process that ends anyway.
//
// So that call goes through a guard: if the shutdown hits an access violation, the
// fault is logged (address and the gta-vc.exe return addresses on the stack) and
// the rest of CGame::ShutDown is skipped; the game's own exit carries on (audio,
// RenderWare, the window, Windows settings it changed). Should anything later in
// the exit fault as well, the process ends quietly.
#include "exitguard.h"
#include "config.h"
#include <windows.h>
#include <string.h>

static const DWORD SHUTDOWN_SITE = 0x6005C9, GAME_SHUTDOWN = 0x4A49E0;

extern "C" {
DWORD vcmf_exit_regs[6];          // esp, ebp, ebx, esi, edi, fs:[0]
DWORD vcmf_shutdown_fn = GAME_SHUTDOWN;
volatile LONG vcmf_in_shutdown = 0;
void vcmf_guarded_shutdown();
void vcmf_shutdown_resume();
}
__asm__(".text\n"
        ".globl _vcmf_guarded_shutdown\n_vcmf_guarded_shutdown:\n"
        "  movl %esp, _vcmf_exit_regs\n"
        "  movl %ebp, _vcmf_exit_regs+4\n"
        "  movl %ebx, _vcmf_exit_regs+8\n"
        "  movl %esi, _vcmf_exit_regs+12\n"
        "  movl %edi, _vcmf_exit_regs+16\n"
        "  movl %fs:0, %eax\n"
        "  movl %eax, _vcmf_exit_regs+20\n"
        "  movl $1, _vcmf_in_shutdown\n"
        "  call *_vcmf_shutdown_fn\n"
        "  movl $0, _vcmf_in_shutdown\n"
        "  ret\n"
        // where a fault inside the shutdown continues: back at the call's return
        ".globl _vcmf_shutdown_resume\n_vcmf_shutdown_resume:\n"
        "  movl _vcmf_exit_regs, %esp\n"
        "  movl _vcmf_exit_regs+4, %ebp\n"
        "  movl _vcmf_exit_regs+8, %ebx\n"
        "  movl _vcmf_exit_regs+12, %esi\n"
        "  movl _vcmf_exit_regs+16, %edi\n"
        "  movl _vcmf_exit_regs+20, %eax\n"
        "  movl %eax, %fs:0\n"
        "  movl $0, _vcmf_in_shutdown\n"
        "  xorl %eax, %eax\n"
        "  ret\n");

static volatile LONG g_exiting = 0;

static void LogFault(const char* where, EXCEPTION_POINTERS* e) {
    const CONTEXT* c = e->ContextRecord;
    const ULONG_PTR* info = e->ExceptionRecord->ExceptionInformation;
    char trail[160] = "";
    int n = 0, len = 0;
    const DWORD* sp = (const DWORD*)c->Esp;
    for (int i = 0; i < 256 && n < 10; i++) {
        DWORD v;
        if (IsBadReadPtr(sp + i, 4)) break;
        v = sp[i];
        if (v < 0x401000 || v >= 0x6D0000) continue;
        const unsigned char* p = (const unsigned char*)v;
        if (p[-5] != 0xE8 && !(p[-6] == 0xFF && p[-5] == 0x15) && !(p[-2] == 0xFF)) continue;   // after a call
        len += wsprintfA(trail + len, " %06lX", v);
        n++;
    }
    Log("ExitGuard: %s: access violation at %08lX (%s %08lX), callers:%s", where, c->Eip,
        info[0] ? "writing" : "reading", (DWORD)info[1], trail);
}

static LONG CALLBACK ExitHandler(EXCEPTION_POINTERS* e) {
    if (e->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || !g_exiting) return EXCEPTION_CONTINUE_SEARCH;
    if (vcmf_in_shutdown) {
        vcmf_in_shutdown = 0;
        LogFault("game shutdown", e);
        Log("ExitGuard: rest of the game's clean-up skipped; exiting normally");
        e->ContextRecord->Eip = (DWORD)&vcmf_shutdown_resume;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    LogFault("exit", e);
    Log("ExitGuard: ending the process");
    TerminateProcess(GetCurrentProcess(), 0);
    return EXCEPTION_CONTINUE_SEARCH;
}

// Marks the exit (any fault from here on is the exit's) before the shutdown runs.
extern "C" void vcmf_exit_begin() {
    g_exiting = 1;
    Log("ExitGuard: game is quitting");
}
extern "C" void vcmf_exit_call();
__asm__(".text\n"
        ".globl _vcmf_exit_call\n_vcmf_exit_call:\n"
        "  call _vcmf_exit_begin\n"
        "  jmp _vcmf_guarded_shutdown\n");

void InstallExitGuard() {
    const unsigned char* p = (const unsigned char*)SHUTDOWN_SITE;
    // cmp dword [gameState], 9; jne +5; call CGame::ShutDown
    static const unsigned char before[] = { 0x83, 0x3D, 0x08, 0x5F, 0x9B, 0x00, 0x09, 0x75, 0x05 };
    if (p[0] != 0xE8 || memcmp(p - sizeof before, before, sizeof before)) {
        Log("ExitGuard: exit code is not v1.0; not installed");
        return;
    }
    vcmf_shutdown_fn = SHUTDOWN_SITE + 5 + *(const int*)(p + 1);
    if (vcmf_shutdown_fn != GAME_SHUTDOWN)
        Log("ExitGuard: game shutdown hooked by another mod (%08lX); chaining to it", vcmf_shutdown_fn);
    AddVectoredExceptionHandler(1, ExitHandler);
    unsigned char call[5] = { 0xE8 };
    *(int*)(call + 1) = (int)((DWORD)&vcmf_exit_call - (SHUTDOWN_SITE + 5));
    DWORD old;
    VirtualProtect((void*)SHUTDOWN_SITE, 5, PAGE_EXECUTE_READWRITE, &old);
    memcpy((void*)SHUTDOWN_SITE, call, 5);
    VirtualProtect((void*)SHUTDOWN_SITE, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)SHUTDOWN_SITE, 5);
    Log("ExitGuard: installed (a crash in the game's own clean-up on quit is skipped)");
}
