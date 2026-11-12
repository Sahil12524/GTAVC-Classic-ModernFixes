#include "config.h"
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

Config g_cfg;

static void GamePath(char* out, const char* file) {
    GetModuleFileNameA(nullptr, out, MAX_PATH);   // ...\gta-vc.exe
    if (char* slash = strrchr(out, '\\')) slash[1] = 0;
    lstrcatA(out, file);
}

static const char kDefaultIni[] =
    "[FixedTick]\r\n"
    "; The game world (physics, AI, scripts) updates TickRate times per second, exactly\r\n"
    "; like the original game running at that frame rate, while frames are drawn as fast\r\n"
    "; as FPSLimit allows. 30 = the original game's own frame limiter. 0 = off.\r\n"
    "Enabled=1\r\n"
    "TickRate=30\r\n"
    "\r\n"
    "[FrameLimiter]\r\n"
    "; 0 = your display's refresh rate, -1 = unlimited, or a number of frames per second.\r\n"
    "FPSLimit=0\r\n"
    "\r\n"
    "[Display]\r\n"
    "; Fullscreen refresh rate in Hz. 0 = the highest your display supports at the game's\r\n"
    "; resolution (the game itself would use the driver default, usually 60 Hz).\r\n"
    "RefreshRate=0\r\n"
    "\r\n"
    "[Compatibility]\r\n"
    "; Run the game on one CPU (0 = \"CPU 0\", 1 = \"CPU 1\" in Task Manager's affinity\r\n"
    "; list), like setting affinity by hand. -1 = all cores (default).\r\n"
    "CPUCore=-1\r\n"
    "; Another d3d8.dll to pass graphics through to (d3d8to9, dgVoodoo...): rename it,\r\n"
    "; e.g. to d3d8_next.dll, put it next to gta-vc.exe and set D3D8Chain=d3d8_next.dll.\r\n"
    "; Empty = Windows' own d3d8.dll.\r\n"
    "D3D8Chain=\r\n"
    "\r\n"
    "[Streaming]\r\n"
    "; Read audio\\SFX.RAW and models\\gta3.img once at startup, at low priority, so\r\n"
    "; Windows keeps them in RAM and ped speech / model loading don't freeze the game\r\n"
    "; for a moment on a hard disk. 1 = on, 0 = off.\r\n"
    "PreloadFiles=1\r\n"
    "; Memory in MB the game may keep loaded models and textures in (the game gives\r\n"
    "; itself 45). More = buildings and cars loaded once stay loaded: less popping in,\r\n"
    "; fewer low-detail stand-ins. 0 = the game's own.\r\n"
    "MemoryMB=512\r\n"
    "; Vehicle models kept loaded at once (game: 25, at most 50). More = more kinds of\r\n"
    "; cars in traffic without loading. 0 = the game's own.\r\n"
    "VehicleModels=50\r\n"
    "\r\n"
    "[DrawDistance]\r\n"
    "; Times the menu's Draw Distance: how far full-detail buildings, objects, peds and\r\n"
    "; vehicles are drawn before the low-detail versions take over (the game also stops\r\n"
    "; full detail at 300 m; that becomes 300 m times this). 1 = the game's own, up to 5.\r\n"
    "Scale=3.0\r\n"
    "; Times the weather's far clip: how far anything is seen before the fog. 1 = the\r\n"
    "; game's own, up to 3.\r\n"
    "FarClip=1.5\r\n"
    "\r\n"
    "[Graphics]\r\n"
    "; Anisotropic texture filtering: roads, walls and ground seen at an angle stay\r\n"
    "; sharp instead of blurring. 2-16, 0 = the game's own.\r\n"
    "Anisotropic=16\r\n"
    "\r\n"
    "[Camera]\r\n"
    "; Look around the vehicle you're in with the mouse. 1 = on, 0 = off.\r\n"
    "FreeCam=1\r\n"
    "; Mouse speed in percent of the on-foot camera's (which follows the game's own\r\n"
    "; mouse sensitivity setting).\r\n"
    "FreeCamSensitivity=100\r\n"
    "; While driving, the camera goes back behind the vehicle after the mouse has been\r\n"
    "; still this many milliseconds. -1 = never. Parked, it always stays where you put it.\r\n"
    "FreeCamReturnDelay=1500\r\n"
    "; 1 = moving the mouse forward lowers the camera instead of raising it.\r\n"
    "FreeCamInvertY=0\r\n"
    "\r\n"
    "[QuickSave]\r\n"
    "; F5 saves the game as it is right now, F9 loads that save. Not while wanted, on a\r\n"
    "; mission or on water. 1 = on, 0 = off.\r\n"
    "Enabled=1\r\n"
    "; The game save slot (1-8) the quick save goes into; it also shows in Load Game.\r\n"
    "; The save is named \"Quick Save\". If the slot holds one of your own saves, it is\r\n"
    "; first copied to GTAVCsf<slot> (before quick save).b in the same folder.\r\n"
    "Slot=8\r\n"
    "; Keys as Windows virtual-key codes: 116 = F5, 120 = F9.\r\n"
    "SaveKey=116\r\n"
    "LoadKey=120\r\n"
    "; Also quick save by itself every this many minutes of play (paused menus don't\r\n"
    "; count), into the same slot, so F9 loads it. Only when it is safe: no mission or\r\n"
    "; job, not wanted, not on water, not on a phone call or in a cutscene, in control\r\n"
    "; and not in the air. If it isn't safe it waits and saves once it is. Any save\r\n"
    "; restarts the count. 0 = off.\r\n"
    "AutoMinutes=5\r\n"
    "\r\n"
    "[AutoSave]\r\n"
    "; Saves the game a few seconds after you pass a mission or finish a job (taxi,\r\n"
    "; vigilante, firefighter, paramedic, rampages...) with money earned, once you are\r\n"
    "; not wanted, not on water and in control. Not after being wasted or busted.\r\n"
    "; Like a safehouse save, loading it starts you at your nearest safehouse.\r\n"
    "; 1 = on, 0 = off.\r\n"
    "Enabled=1\r\n"
    "; The game save slot (1-8). The save is named \"Auto Save\"; one of your own saves\r\n"
    "; in that slot is first copied to GTAVCsf<slot> (before auto save).b.\r\n"
    "Slot=7\r\n"
    "\r\n"
    "[Vehicles]\r\n"
    "; In the Rhino the turret, and in the fire truck the water cannon, aim where the\r\n"
    "; camera looks: move the mouse to aim (FreeCam on). 1 = on, 0 = off.\r\n"
    "MouseAim=1\r\n"
    "\r\n"
    "[Fixes]\r\n"
    "; \"Martha's Mug Shot\" swaps the Ocean Drive hotel for a stand-in model while you are\r\n"
    "; on its roof. If the mission ends without swapping it back, the hotel stays without\r\n"
    "; walls (and saves keep it that way). Off a mission, it is put back. 1 = on.\r\n"
    "MissionLeftovers=1\r\n"
    "\r\n"
    "[Weapons]\r\n"
    "; Walk, run and sprint while firing any gun, each with its own animation (the fire\r\n"
    "; animations no longer lock the legs). Sprint and jump with the rocket launcher and\r\n"
    "; minigun. The M4, Ruger and M60 aim in third person instead of the first-person\r\n"
    "; scope, so you can move while aiming. 1 = on.\r\n"
    "RunAndGun=1\r\n"
    "; Getting shot no longer stops your shooting (you still flinch). 1 = on.\r\n"
    "KeepFiringWhenHit=1\r\n"
    "\r\n"
    "[Population]\r\n"
    "; How busy the streets are, 0.5 to 3 (0 = leave it to the game, which uses 0.6\r\n"
    "; without a gta3.ini). Peds: up to 25 x PedDensity on the streets. Cars: up to\r\n"
    "; 12 x CarDensity traffic cars.\r\n"
    "PedDensity=2.5\r\n"
    "CarDensity=1.5\r\n"
    "; A ped you turn the camera away from is kept until it is this many metres away\r\n"
    "; (25 to 50; the game removes it at 25, so peds vanish when you look around).\r\n"
    "KeepOffscreen=50\r\n"
    "\r\n"
    "[PhoneSkip]\r\n"
    "; During a phone call you answered (Tab), Enter skips the rest of the call. 1 = on.\r\n"
    "Enabled=1\r\n"
    "; Key as a Windows virtual-key code: 13 = Enter.\r\n"
    "Key=13\r\n"
    "\r\n"
    "[Interiors]\r\n"
    "; Teleporting out of a room shows the city again, and flying into a room (jetpack)\r\n"
    "; shows that room, with the scene loaded around you. 1 = on, 0 = off.\r\n"
    "AutoSwitch=1\r\n"
    "; Inside an interior that is built into the city (Ocean View, the mall, Malibu...)\r\n"
    "; the city around it is drawn too: open doors show the street, not black.\r\n"
    "ShowOutside=1\r\n"
    "; Walking through an interior's door doesn't fade the screen to black and back:\r\n"
    "; the room (or the street) is there at once. Missions' and cutscenes' fades stay.\r\n"
    "NoDoorFade=1\r\n";

bool ModifierDown() {
    static const int keys[] = { VK_SHIFT, VK_CONTROL, VK_MENU, VK_LWIN, VK_RWIN };
    for (int k : keys) if (GetAsyncKeyState(k) & 0x8000) return true;
    return false;
}

void LoadConfig() {
    char ini[MAX_PATH];
    GamePath(ini, "VCModernFix.ini");
    if (GetFileAttributesA(ini) == INVALID_FILE_ATTRIBUTES) {
        HANDLE h = CreateFileA(ini, GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD written;
            WriteFile(h, kDefaultIni, sizeof kDefaultIni - 1, &written, nullptr);
            CloseHandle(h);
        }
    }
    g_cfg.fixedTick = GetPrivateProfileIntA("FixedTick", "Enabled", 1, ini) != 0;
    int tick = (int)GetPrivateProfileIntA("FixedTick", "TickRate", 30, ini);
    g_cfg.tickRate = tick < 20 ? 20 : tick > 120 ? 120 : tick;
    int fps = (int)GetPrivateProfileIntA("FrameLimiter", "FPSLimit", 0, ini);
    g_cfg.fpsLimit = (fps == -1 || fps == 0 || (fps >= 20 && fps <= 1000)) ? fps : 0;
    int hz = (int)GetPrivateProfileIntA("Display", "RefreshRate", 0, ini);
    g_cfg.refreshHz = (hz >= 0 && hz <= 1000) ? hz : 0;
    int core = (int)GetPrivateProfileIntA("Compatibility", "CPUCore", -1, ini);
    g_cfg.cpuCore = (core >= -1 && core < 32) ? core : -1;
    GetPrivateProfileStringA("Compatibility", "D3D8Chain", "", g_cfg.d3d8Chain, sizeof g_cfg.d3d8Chain, ini);
    if (char* semi = strchr(g_cfg.d3d8Chain, ';')) *semi = 0;
    for (int n = lstrlenA(g_cfg.d3d8Chain); n > 0 && (g_cfg.d3d8Chain[n - 1] == ' ' || g_cfg.d3d8Chain[n - 1] == '\t'); n--)
        g_cfg.d3d8Chain[n - 1] = 0;
    if (!lstrcmpiA(g_cfg.d3d8Chain, "d3d8.dll")) g_cfg.d3d8Chain[0] = 0;   // that would be this DLL
    g_cfg.preload = GetPrivateProfileIntA("Streaming", "PreloadFiles", 1, ini) != 0;
    int mem = (int)GetPrivateProfileIntA("Streaming", "MemoryMB", 512, ini);
    g_cfg.streamMemoryMB = mem <= 0 ? 0 : mem < 45 ? 45 : mem > 1536 ? 1536 : mem;
    int veh = (int)GetPrivateProfileIntA("Streaming", "VehicleModels", 50, ini);
    g_cfg.vehicleModels = veh <= 0 ? 0 : veh < 25 ? 25 : veh > 50 ? 50 : veh;
    char dd[32];
    GetPrivateProfileStringA("DrawDistance", "Scale", "3.0", dd, sizeof dd, ini);
    float scale = (float)atof(dd);
    g_cfg.drawDistance = !(scale >= 1.0f) ? 1.0f : scale > 5.0f ? 5.0f : scale;
    GetPrivateProfileStringA("DrawDistance", "FarClip", "1.5", dd, sizeof dd, ini);
    scale = (float)atof(dd);
    g_cfg.farClip = !(scale >= 1.0f) ? 1.0f : scale > 3.0f ? 3.0f : scale;
    int an = (int)GetPrivateProfileIntA("Graphics", "Anisotropic", 16, ini);
    g_cfg.anisotropy = an <= 1 ? 0 : an > 16 ? 16 : an;
    g_cfg.freeCam = GetPrivateProfileIntA("Camera", "FreeCam", 1, ini) != 0;
    int sens = (int)GetPrivateProfileIntA("Camera", "FreeCamSensitivity", 100, ini);
    g_cfg.freeCamSensitivity = sens < 10 ? 10 : sens > 1000 ? 1000 : sens;
    int ret = (int)GetPrivateProfileIntA("Camera", "FreeCamReturnDelay", 1500, ini);
    g_cfg.freeCamReturnDelay = ret < -1 ? -1 : ret > 60000 ? 60000 : ret;
    g_cfg.freeCamInvertY = GetPrivateProfileIntA("Camera", "FreeCamInvertY", 0, ini) != 0;
    g_cfg.phoneSkip = GetPrivateProfileIntA("PhoneSkip", "Enabled", 1, ini) != 0;
    int pk = (int)GetPrivateProfileIntA("PhoneSkip", "Key", 0x0D, ini);
    g_cfg.phoneSkipKey = pk > 0 && pk < 256 ? pk : 0x0D;
    g_cfg.interiors = GetPrivateProfileIntA("Interiors", "AutoSwitch", 1, ini) != 0;
    g_cfg.interiorView = GetPrivateProfileIntA("Interiors", "ShowOutside", 1, ini) != 0;
    g_cfg.noDoorFade = GetPrivateProfileIntA("Interiors", "NoDoorFade", 1, ini) != 0;
    g_cfg.quickSave = GetPrivateProfileIntA("QuickSave", "Enabled", 1, ini) != 0;
    int qslot = (int)GetPrivateProfileIntA("QuickSave", "Slot", 8, ini);
    g_cfg.quickSaveSlot = qslot < 1 ? 1 : qslot > 8 ? 8 : qslot;
    int qk = (int)GetPrivateProfileIntA("QuickSave", "SaveKey", 0x74, ini);
    g_cfg.quickSaveKey = qk > 0 && qk < 256 ? qk : 0x74;
    qk = (int)GetPrivateProfileIntA("QuickSave", "LoadKey", 0x78, ini);
    g_cfg.quickLoadKey = qk > 0 && qk < 256 ? qk : 0x78;
    int every = (int)GetPrivateProfileIntA("QuickSave", "AutoMinutes", 5, ini);
    g_cfg.quickSaveEvery = every < 0 ? 0 : every > 120 ? 120 : every;
    g_cfg.autoSave = GetPrivateProfileIntA("AutoSave", "Enabled", 1, ini) != 0;
    int aslot = (int)GetPrivateProfileIntA("AutoSave", "Slot", 7, ini);
    g_cfg.autoSaveSlot = aslot < 1 ? 1 : aslot > 8 ? 8 : aslot;
    g_cfg.mouseAim = GetPrivateProfileIntA("Vehicles", "MouseAim", 1, ini) != 0;
    g_cfg.missionLeftovers = GetPrivateProfileIntA("Fixes", "MissionLeftovers", 1, ini) != 0;
    g_cfg.runAndGun = GetPrivateProfileIntA("Weapons", "RunAndGun", 1, ini) != 0;
    g_cfg.keepFiringWhenHit = GetPrivateProfileIntA("Weapons", "KeepFiringWhenHit", 1, ini) != 0;
    GetPrivateProfileStringA("Population", "PedDensity", "2.5", dd, sizeof dd, ini);
    scale = (float)atof(dd);
    g_cfg.pedDensity = !(scale > 0) ? 0.0f : scale < 0.5f ? 0.5f : scale > 3.0f ? 3.0f : scale;
    GetPrivateProfileStringA("Population", "CarDensity", "1.5", dd, sizeof dd, ini);
    scale = (float)atof(dd);
    g_cfg.carDensity = !(scale > 0) ? 0.0f : scale < 0.5f ? 0.5f : scale > 3.0f ? 3.0f : scale;
    GetPrivateProfileStringA("Population", "KeepOffscreen", "50", dd, sizeof dd, ini);
    scale = (float)atof(dd);
    g_cfg.offscreenKeep = !(scale > 0) ? 0.0f : scale < 25.0f ? 25.0f : scale > 50.0f ? 50.0f : scale;

    char logPath[MAX_PATH];
    GamePath(logPath, "VCModernFix.log");
    DeleteFileA(logPath);   // one log per session
    Log("VCModernFix: FixedTick=%d TickRate=%d FPSLimit=%d RefreshRate=%d CPUCore=%d D3D8Chain=%s",
        g_cfg.fixedTick, g_cfg.tickRate, g_cfg.fpsLimit, g_cfg.refreshHz, g_cfg.cpuCore,
        g_cfg.d3d8Chain[0] ? g_cfg.d3d8Chain : "(Windows)");
}

// Same as setting the game's affinity to one CPU in Task Manager, done before the
// game creates its window, input devices or threads (DllMain of d3d8.dll, which the
// exe imports). If that CPU doesn't exist, the first available one after it is used.
void ApplyCpuAffinity() {
    if (g_cfg.cpuCore < 0) { Log("CPU affinity: off (all cores)"); return; }
    DWORD_PTR procMask = 0, sysMask = 0;
    if (!GetProcessAffinityMask(GetCurrentProcess(), &procMask, &sysMask) || !procMask) {
        Log("CPU affinity: could not read the process affinity, left unchanged");
        return;
    }
    int cpu = -1;
    for (int i = g_cfg.cpuCore; i < (int)(sizeof(DWORD_PTR) * 8) && cpu < 0; i++)
        if (procMask & ((DWORD_PTR)1 << i)) cpu = i;
    for (int i = 0; i < g_cfg.cpuCore && cpu < 0; i++)
        if (procMask & ((DWORD_PTR)1 << i)) cpu = i;
    if (cpu < 0) { Log("CPU affinity: no usable CPU, left unchanged"); return; }
    if (SetProcessAffinityMask(GetCurrentProcess(), (DWORD_PTR)1 << cpu))
        Log("CPU affinity: game pinned to CPU %d (CPUCore=%d)", cpu, g_cfg.cpuCore);
    else
        Log("CPU affinity: SetProcessAffinityMask failed (error %lu)", GetLastError());
}

// Called from the game thread, where Direct3D 8 has set the x87 FPU to single
// precision; the C library expects the default (64-bit mantissa, all exceptions
// masked), so that is set for the duration of the call and the game's own control
// word put back after.
void Log(const char* fmt, ...) {
    unsigned short gameCw, defaultCw = 0x037F;
    __asm__ volatile("fnstcw %0" : "=m"(gameCw));
    __asm__ volatile("fldcw %0" : : "m"(defaultCw));
    char path[MAX_PATH];
    GamePath(path, "VCModernFix.log");
    if (FILE* f = fopen(path, "a")) {
        va_list ap;
        va_start(ap, fmt);
        vfprintf(f, fmt, ap);
        va_end(ap);
        fputc('\n', f);
        fclose(f);
    }
    __asm__ volatile("fldcw %0" : : "m"(gameCw));
}
