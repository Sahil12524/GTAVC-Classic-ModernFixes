// Settings from VCModernFix.ini (config.cpp).
#pragma once

struct Config {
    bool fixedTick  = true;   // [FixedTick] Enabled
    int  tickRate   = 30;     // [FixedTick] TickRate: game simulation updates per second
    int  fpsLimit   = 0;      // [FrameLimiter] FPSLimit: 0 = display refresh rate, -1 = unlimited, else FPS
    int  refreshHz  = 0;      // [Display] RefreshRate: fullscreen refresh rate, 0 = highest available
    int  cpuCore    = -1;     // [Compatibility] CPUCore: logical CPU to run on, -1 = all
    char d3d8Chain[260] = ""; // [Compatibility] D3D8Chain: another d3d8.dll to pass through to (game folder)
    bool preload    = true;   // [Streaming] PreloadFiles: warm the file cache with SFX.RAW and gta3.img
    int  streamMemoryMB = 512; // [Streaming] MemoryMB: memory for loaded models, 0 = game's 45 MB
    int  vehicleModels = 50;  // [Streaming] VehicleModels: vehicle models kept loaded, 0 = game's 25 (max 50)
    float drawDistance = 3.0f; // [DrawDistance] Scale: times the menu's Draw Distance, 1 = game's own
    float farClip = 1.5f;     // [DrawDistance] FarClip: times the weather's far clip (how far anything is seen)
    int  anisotropy = 16;     // [Graphics] Anisotropic: texture filtering level, 0 = game's own
    bool freeCam    = true;   // [Camera] FreeCam: look around the vehicle with the mouse
    int  freeCamSensitivity = 100;   // [Camera] FreeCamSensitivity: percent of the on-foot mouse speed
    int  freeCamReturnDelay = 1500;  // [Camera] FreeCamReturnDelay: ms of still mouse while moving, -1 = never
    bool freeCamInvertY = false;     // [Camera] FreeCamInvertY
    bool quickSave  = true;   // [QuickSave] Enabled
    int  quickSaveKey = 0x74; // [QuickSave] SaveKey: virtual-key code, F5
    int  quickLoadKey = 0x78; // [QuickSave] LoadKey: virtual-key code, F9
    bool phoneSkip  = true;   // [PhoneSkip] Enabled: Enter skips the phone call you are on
    int  phoneSkipKey = 0x0D; // [PhoneSkip] Key: virtual-key code, Enter
    bool interiors  = true;   // [Interiors] AutoSwitch: the drawn interior follows the player
    bool interiorView = true; // [Interiors] ShowOutside: the city is drawn from inside interiors
    bool noDoorFade = true;   // [Interiors] NoDoorFade: no fade to black walking through interior doors
    int  quickSaveEvery = 5;  // [QuickSave] AutoMinutes: quick save every N minutes of play when safe, 0 = off
    int  quickSaveSlot = 8;   // [QuickSave] Slot: game save slot 1-8 used for the quick save
    bool autoSave   = true;   // [AutoSave] Enabled: save after a mission or job is completed
    int  autoSaveSlot = 7;    // [AutoSave] Slot: game save slot 1-8
    bool mouseAim   = true;   // [Vehicles] MouseAim: tank turret / fire truck cannon aim where the camera looks
    bool missionLeftovers = true;   // [Fixes] MissionLeftovers: undo mission model swaps a mission left behind
    bool runAndGun  = true;   // [Weapons] RunAndGun: run while firing two-handed and heavy guns
    bool keepFiringWhenHit = true;   // [Weapons] KeepFiringWhenHit: a bullet hit doesn't stop the player's attack
    float pedDensity = 2.5f;  // [Population] PedDensity: 0.5-3, 0 = the game's own (0.6 without gta3.ini)
    float carDensity = 1.5f;  // [Population] CarDensity: 0.5-3, 0 = the game's own
    float offscreenKeep = 50.0f;   // [Population] KeepOffscreen: metres a ped out of view is kept, 0 = game (25)
};
extern Config g_cfg;

// True while Shift, Ctrl, Alt or a Windows key is held: hotkeys only act when pressed
// on their own, so combinations meant for other programs (ShadowPlay's Alt+F9,
// Alt+Enter) don't trigger them.
bool ModifierDown();

void LoadConfig();
void ApplyCpuAffinity();   // DllMain, right after LoadConfig
void Log(const char* fmt, ...);   // appends to VCModernFix.log next to gta-vc.exe
