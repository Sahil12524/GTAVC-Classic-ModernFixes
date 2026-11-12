// Freeze diagnostics (watchdog.cpp).
#pragma once
#include <windows.h>

extern volatile LONG g_heartbeat;   // bumped by the game thread every main-loop iteration
extern volatile bool g_watchdogArmed;   // set once the game's main loop runs
void StartWatchdog();               // DllMain, on the game thread
