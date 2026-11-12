// Mouse input that stops working until the game loses and regains focus.
//
// The game sets up its DirectInput mouse in 0x5FFA20(exclusive): CreateDevice
// (GUID_SysMouse) into RsGlobal.ps->mouse (0x813D3C), SetDataFormat,
// SetCooperativeLevel(window, exclusive ? EXCLUSIVE|FOREGROUND : NONEXCLUSIVE|
// FOREGROUND), Acquire. Four of its five callers (the menu switching between
// exclusive and non-exclusive mouse, 0x498F99/0x49901B/0x499092, and 0x4ADA81)
// call it while a device already exists: the old device is overwritten without
// Unacquire/Release and, if it had the mouse exclusively, keeps it. The new
// device's Acquire is then refused, so every read (0x5FF290) returns nothing,
// until a focus change makes Windows take the mouse away from the leaked device
// (switching to Task Manager does exactly that).
//
// Fix: before each of those calls, the existing device is unacquired and released;
// then the game's own setup runs unchanged.
#include "mousefix.h"
#include "config.h"
#include <windows.h>
#include <string.h>

static const DWORD MOUSE_SETUP = 0x5FFA20;   // cdecl(bool exclusive)
static const DWORD SETUP_SITES[] = { 0x4AD855, 0x498F99, 0x49901B, 0x499092, 0x4ADA81 };
// RsGlobal.ps (0x9B48F4) -> +0x1C mouse device, exactly as 0x5FFA20/0x5FF290 reach it
static void** MouseDeviceSlot() {
    unsigned char* ps = *(unsigned char**)0x9B48F4;
    return ps ? (void**)(ps + 0x1C) : nullptr;
}

static unsigned g_releases = 0;

static int __cdecl HookMouseSetup(int exclusive) {
    void** slot = MouseDeviceSlot();
    if (void* dev = slot ? *slot : nullptr) {
        void** vt = *(void***)dev;
        ((long(__stdcall*)(void*))vt[8])(dev);   // IDirectInputDevice8::Unacquire
        ((unsigned long(__stdcall*)(void*))vt[2])(dev);   // IUnknown::Release
        *slot = nullptr;
        if (g_releases++ < 20) Log("Mouse: released the previous mouse device before re-creating it (exclusive=%d)", exclusive & 0xFF);
    }
    return ((int(__cdecl*)(int))MOUSE_SETUP)(exclusive);
}

static bool CallsTo(DWORD site, DWORD target) {
    const unsigned char* p = (const unsigned char*)site;
    return p[0] == 0xE8 && site + 5 + *(const int*)(p + 1) == target;
}

static void Redirect(DWORD site, void* target) {
    unsigned char call[5] = { 0xE8 };
    *(int*)(call + 1) = (int)((DWORD)target - (site + 5));
    DWORD old;
    VirtualProtect((void*)site, 5, PAGE_EXECUTE_READWRITE, &old);
    memcpy((void*)site, call, 5);
    VirtualProtect((void*)site, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)site, 5);
}

// Each site is hooked only if it still calls the game's own function; a site that
// another mod (e.g. SilentPatch) already redirected is left to that mod.
void InstallMouseFix() {
    int hooked = 0;
    for (DWORD s : SETUP_SITES) {
        if (CallsTo(s, MOUSE_SETUP)) { Redirect(s, (void*)&HookMouseSetup); hooked++; }
        else Log("Mouse fix: mouse setup call at %06X changed by another mod; left to it", s);
    }
    Log("Mouse fix: installed at %d of %d setup calls", hooked, (int)(sizeof SETUP_SITES / sizeof SETUP_SITES[0]));
}
