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
    bool noIslandScreen = true; // [Streaming] NoIslandScreen: no loading screen when crossing between islands
    int  fastLoadMs = 4;      // [Streaming] FastLoadMs: extra model/texture loading per game update (ms), 0 = off
    int  vehicleModels = 50;  // [Streaming] VehicleModels: vehicle models kept loaded, 0 = game's 25 (max 50)
    float drawDistance = 3.0f; // [DrawDistance] Scale: times the menu's Draw Distance, 1 = game's own
    float farClip = 1.5f;     // [DrawDistance] FarClip: times the weather's far clip (how far anything is seen)
    bool highView = true;     // [DrawDistance] HighView: above 100 m the view reaches further with the height (up to 4 km)
    bool fullViewScan = true; // [DrawDistance] FullViewScan: the world scan covers the whole view at any camera angle (viewscan.cpp)
    int  anisotropy = 16;     // [Graphics] Anisotropic: texture filtering level, 0 = game's own
    bool twoPassAlpha = true; // [Graphics] TwoPassAlpha: leaves drawn solid parts first, soft edges after
    bool widescreen = true;   // [Graphics] Widescreen: picture and HUD for the screen's real shape (widescreen.cpp)
    bool farLights = true;    // [Graphics] FarLights: 120 corona slots, far buildings' and lamp posts' lights (farlights.cpp)
    float coronaDistance = 5.0f;  // [Graphics] CoronaDistance: times the distance light coronas are seen from, 1 = game
    bool risingClouds = true; // [Graphics] RisingClouds: the cloud layers rise with a high camera (clouds.cpp)
    bool wideNearClip = true; // [Graphics] WideFovNearClip: near clip brought in for views wider than the game's (widescreen.cpp)
    bool freeCam    = true;   // [Camera] FreeCam: look around the vehicle with the mouse
    int  freeCamSensitivity = 100;   // [Camera] FreeCamSensitivity: percent of the on-foot mouse speed
    int  freeCamReturnDelay = 1500;  // [Camera] FreeCamReturnDelay: ms of still mouse while moving, -1 = never
    bool freeCamInvertY = false;     // [Camera] FreeCamInvertY
    bool frameMouseLook = true;      // [Camera] FrameMouseLook: on-foot mouse look read every frame
    bool freeOrbitOnFoot = true;     // [Camera] FreeOrbitOnFoot: camera goes round the player; they run the way you press
    bool cameraSwingBehind = true;   // [Camera] CameraSwingBehind: the camera slowly swings behind a running player
    int mouseSmoothMs = 4;           // [Camera] MouseSmoothing: ms the per-frame mouse movement is spread over
    int mouseSensitivity = 100;      // [Camera] MouseSensitivity: percent, on top of the game's menu setting
    bool mouseEqualAxes = true;      // [Camera] MouseEqualAxes: up / down as fast as left / right
    bool freeCamBoats = true;        // [Camera] FreeCamBoats: boats use the vehicle camera (mouse look)
    int  carLookForwardKey = 0x43;   // [Camera] CarLookForwardKey: virtual-key code, held in a vehicle = camera behind it looking forward, 0 = off (C)
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
    bool modernKeys = true;   // [Controls] ModernKeys: Q radio, E horn, Shift sprint, Caps Lock missions, Num4/6/8/5, Shift/Ctrl pitch
    bool mouseAim   = true;   // [Vehicles] MouseAim: tank turret / fire truck cannon aim where the camera looks
    bool airControl = true;   // [Vehicles] AirControl: steer a car's pitch and roll in the air (helicopter controls)
    int  airControlStrength = 100;   // [Vehicles] AirControlStrength: percent, 25-300
    bool planeControls = true;  // [Vehicles] PlaneControls: planes steered like the helicopters
    bool heliControls = true; // [Vehicles] HeliControls: helicopter yaw on steer L/R, roll on turret L/R, tilt limits
    int  heliMaxPitch = 30;   // [Vehicles] HeliMaxPitch: degrees the nose tips, 0 = no limit
    int  heliMaxRoll = 40;    // [Vehicles] HeliMaxRoll: degrees it leans to a side, 0 = no limit
    bool hunterWeapons = true; // [Vehicles] HunterWeapons: mouse wheel picks rockets / cannon, one key fires it
    int  hunterFireKey = 0x20; // [Vehicles] HunterFireKey: virtual-key code, Space
    int  heliWeight = 100;    // [Vehicles] HeliWeight: extra helicopter inertia in percent, 0 = game's own (max 300)
    bool introSkip = true;          // [Missions] IntroSkip: hold the key 3 s to fast-forward a scripted mission scene
    int  introSkipKey = 0x0D;       // [Missions] IntroSkipKey: virtual-key code, Enter
    int  softlockSeconds = 90;      // [Missions] SoftlockSeconds: no control this long on a mission = stuck, 0 = off
    int  endMissionKey = 0x23;      // [Missions] EndMissionKey: hold 3 s to end the mission, 0 = off (End)
    bool missionRestart = true;     // [Missions] RestartPrompt: offer to restart a failed mission at its marker
    bool pizzaMouseThrow = true;    // [Missions] PizzaMouseThrow: Pizza Boy: right mouse held + left throws at the customer (pizza.cpp)
    int  restartYesKey = 0x59;      // [Missions] RestartYesKey: Y
    int  restartNoKey = 0x4E;       // [Missions] RestartNoKey: N
    bool stuntJumpNormal = true;    // [StuntJumps] NormalCamera: no slow motion / cinematic camera on unique jumps
    bool clearRiotOnLoad = true;     // [Fixes] ClearRiotOnLoad: a riot / everyone-attacks cheat saved in a game is undone on load
    bool missionLeftovers = true;   // [Fixes] MissionLeftovers: undo mission model swaps a mission left behind
    bool stationFlag = true;        // [Fixes] PoliceStationFlag: a stale "inside the police station" flag is cleared (gameplay.cpp)
    bool runAndGun  = true;   // [Weapons] RunAndGun: run while firing two-handed and heavy guns
    bool freeDriveBy = true;         // [Weapons] FreeDriveBy: shoot from any car, bike or boat where the camera looks
    int  driveByKey = 0x12;          // [Weapons] DriveByKey: virtual-key code that takes the gun out / puts it away, Alt
    bool keepFiringWhenHit = true;   // [Weapons] KeepFiringWhenHit: a bullet hit doesn't stop the player's attack
    float pedDensity = 2.5f;  // [Population] PedDensity: 0.5-3, 0 = the game's own (0.6 without gta3.ini)
    float carDensity = 1.5f;  // [Population] CarDensity: 0.5-3, 0 = the game's own
    int maxStreetCops = 3;    // [Population] MaxStreetCops: walking cops about while not wanted, -1 = no limit
    float offscreenKeep = 50.0f;   // [Population] KeepOffscreen: metres a ped out of view is kept, 0 = game (25)
};
extern Config g_cfg;

// True while Shift, Ctrl, Alt or a Windows key is held: hotkeys only act when pressed
// on their own, so combinations meant for other programs (ShadowPlay's Alt+F9,
// Alt+Enter) don't trigger them.
bool ModifierDown();

// An F1 replay is playing (CReplay::Mode, 0xA10AF6, set to 1 by
// CReplay::TriggerPlayback 0x6209E0). The world is then the recording, rebuilt
// every frame: the features that act on the world stand down until it ends.
bool ReplayPlaying();

// Writes one setting to VCModernFix.ini (the Modern Fix menu, fixmenu.cpp).
void SaveSetting(const char* section, const char* key, const char* value);
void LoadConfig();
void ApplyCpuAffinity();   // DllMain, right after LoadConfig
void Log(const char* fmt, ...);   // appends to VCModernFix.log next to gta-vc.exe
