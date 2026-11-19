#include "config.h"
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

Config g_cfg;
static void CloseLog();   // with Log, at the end

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
    "; 0 = your display's refresh rate (with vsync on, vsync alone paces the frames),\r\n"
    "; -1 = unlimited (vsync off too, also through d3d8to9 / DXVK), or a number of\r\n"
    "; frames per second.\r\n"
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
    "; Load models and textures faster: before each game update, up to this many\r\n"
    "; milliseconds of extra loading while anything is waiting (the game loads one\r\n"
    "; batch per update, so textures can show up late or blurry when you move fast).\r\n"
    "; 0 = the game's own rate, up to 20.\r\n"
    "FastLoadMs=4\r\n"
    "; No \"Greetings from Vice City\" loading screen flashing up when you cross to the\r\n"
    "; other island; the island still loads. 1 = on, 0 = the game's screen.\r\n"
    "NoIslandScreen=1\r\n"
    "\r\n"
    "[DrawDistance]\r\n"
    "; Times the menu's Draw Distance: how far full-detail buildings, objects, peds and\r\n"
    "; vehicles are drawn before the low-detail versions take over (the game also stops\r\n"
    "; full detail at 300 m; that becomes 300 m times this). 1 = the game's own, up to 5.\r\n"
    "Scale=3.0\r\n"
    "; Times the weather's far clip: how far anything is seen before the fog. 1 = the\r\n"
    "; game's own, up to 3.\r\n"
    "FarClip=1.5\r\n"
    "; 1 = high up (above 100 m: helicopters, planes) the view reaches further the\r\n"
    "; higher you are, up to 4 km, and the fog moves out with it. 0 = the game's own.\r\n"
    "HighView=1\r\n"
    "; 1 = objects, roads and building walls at the edges of the screen don't vanish\r\n"
    "; while a helicopter or plane banks or pitches (the game's world scan only covered\r\n"
    "; part of a tilted view). 0 = the game's own.\r\n"
    "FullViewScan=1\r\n"
    "\r\n"
    "[Graphics]\r\n"
    "; Anisotropic texture filtering: roads, walls and ground seen at an angle stay\r\n"
    "; sharp instead of blurring. 2-16, 0 = the game's own.\r\n"
    "Anisotropic=16\r\n"
    "; Palm and tree leaves drawn in two passes: solid parts first, then the soft\r\n"
    "; edges without hiding what is behind them (no leaves missing in boxes of sky).\r\n"
    "; 1 = on, 0 = the game's own.\r\n"
    "TwoPassAlpha=1\r\n"
    "; Widescreen: the picture fits the screen's real shape (16:9, 21:9...) instead of\r\n"
    "; stretching the game's 4:3 one: you see more to the sides, and the HUD, radar and\r\n"
    "; kill frenzy text keep their 4:3 proportions. 1 = on, 0 = the game's own.\r\n"
    "Widescreen=1\r\n"
    "; Light coronas (street lamps, windows, neon, car lights) are seen this many\r\n"
    "; times further than in the game: as in the game up close, dimmer and smaller\r\n"
    "; further away. 1 = the game's own distances, at most 10.\r\n"
    "CoronaDistance=5.0\r\n"
    "; 1 = the lights of buildings and lamp posts too far away to be drawn still shine\r\n"
    "; at night (up to 2.5 km), and the game has 120 corona slots instead of 56.\r\n"
    "; 0 = the game's own.\r\n"
    "FarLights=1\r\n"
    "; 1 = the clouds rise with the camera when you are high up (they sank below a\r\n"
    "; helicopter and the sky above was empty). 0 = the game's own.\r\n"
    "RisingClouds=1\r\n"
    "; 1 = with a field of view wider than the game's (a trainer's FOV slider), the near\r\n"
    "; clip is brought in so the player's legs aren't cut off. 0 = the game's own.\r\n"
    "WideFovNearClip=1\r\n"
    "\r\n"
    "[Camera]\r\n"
    "; On foot the mouse camera goes all the way round the player (you can see their\r\n"
    "; face) and they run the way you press, as in newer games; aiming a gun turns\r\n"
    "; them to where the camera looks. 1 = on, 0 = the game's (always from behind).\r\n"
    "FreeOrbitOnFoot=1\r\n"
    "; With it, the camera slowly swings back behind the player while they run and\r\n"
    "; the mouse is left alone (as in San Andreas). 1 = on, 0 = off.\r\n"
    "CameraSwingBehind=1\r\n"
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
    "; Boats use the same camera as other vehicles (the string camera that follows\r\n"
    "; and swings behind), so the mouse look works on boats too. 0 = the game's own\r\n"
    "; boat camera, without mouse look.\r\n"
    "FreeCamBoats=1\r\n"
    "; In a vehicle, holding this key puts the camera straight behind it, looking\r\n"
    "; forward (the mouse look starts again from there). Virtual-key code, 67 = C,\r\n"
    "; 0 = off.\r\n"
    "CarLookForwardKey=67\r\n"
    "; On foot, the mouse turns the camera every frame instead of 30 times a second\r\n"
    "; (no 33-66 ms delay at high refresh rates). Needs FixedTick. 1 = on.\r\n"
    "FrameMouseLook=1\r\n"
    "; Mouse speed in percent, on top of the game's own menu setting (on foot, in\r\n"
    "; vehicles, aiming).\r\n"
    "MouseSensitivity=100\r\n"
    "; 1 = looking up / down turns as fast as left / right (the game's vertical speed\r\n"
    "; doesn't follow its menu slider). 0 = the game's own.\r\n"
    "MouseEqualAxes=1\r\n"
    "; Spread mouse movement over this many milliseconds. At hundreds of FPS a frame\r\n"
    "; gets 0, 1 or 2 mouse counts, so slow panning stepped; 4 spreads each count\r\n"
    "; evenly (about 2 ms delay). 0 = off (direct), 8-12 for 125 Hz mice.\r\n"
    "MouseSmoothing=4\r\n"
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
    "[Controls]\r\n"
    "; Modern key layout, put back whenever the game's menu closes (a reset or change\r\n"
    "; in the controls menu doesn't undo it): W A S D move and drive, Shift sprints,\r\n"
    "; Q changes the radio, E sounds the horn, Caps Lock starts a vehicle mission\r\n"
    "; (taxi, vigilante...), Num4 / Num6 roll and Num8 / Num5 pitch a helicopter\r\n"
    "; (turret left / right / up / down), A / D turn it. No mouse button is the\r\n"
    "; handbrake (Space is). Left Ctrl doesn't fire (the mouse and Num0 do), so it is\r\n"
    "; free for other programs. Delete no longer aims (scopes) or zooms: the right mouse\r\n"
    "; button aims. Bikes lean with Num8 / Num5 only (W / S don't lean them, so braking\r\n"
    "; doesn't pull a wheelie). Other mouse and gamepad bindings are left as they are.\r\n"
    "; 1 = on, 0 = the bindings set in the game's controls menu.\r\n"
    "ModernKeys=1\r\n"
    "\r\n"
    "[Vehicles]\r\n"
    "; In the Rhino the turret, and in the fire truck the water cannon, aim where the\r\n"
    "; camera looks: move the mouse to aim (FreeCam on). 1 = on, 0 = off.\r\n"
    "MouseAim=1\r\n"
    "; In the air, a car's nose and sides are steered with the helicopter controls:\r\n"
    "; forward / back tips the nose down / up, left / right rolls it. Set the keys in\r\n"
    "; the game's controls menu. 1 = on, 0 = off.\r\n"
    "AirControl=1\r\n"
    "; How fast the car turns in the air, in percent (25-300).\r\n"
    "AirControlStrength=100\r\n"
    "; Helicopters feel heavier: they lean, turn and speed up more slowly and carry\r\n"
    "; their momentum; top speed and hovering stay the game's. In percent, 0 = the\r\n"
    "; game's own light handling, up to 300.\r\n"
    "HeliWeight=100\r\n"
    "; Helicopter controls as in GTA V: steer left / right (A / D) turns it, turret left\r\n"
    "; / right (Num4 / Num6) rolls it, pitch keeps its keys (Num8 / Num5). The nose\r\n"
    "; dips only so far (pushing forward gains speed instead of diving) and it leans\r\n"
    "; only so far to a side. Keys are the ones set in the game's controls menu.\r\n"
    "; 1 = on, 0 = the game's own controls.\r\n"
    "HeliControls=1\r\n"
    "; Planes (Skimmer, RC Baron) with the same controls: steer left / right (A / D)\r\n"
    "; turns it (rudder), turret left / right (Num4 / Num6) rolls it, pitch as before.\r\n"
    "; No tilt limits (planes can loop and roll over). 1 = on, 0 = the game's own.\r\n"
    "PlaneControls=1\r\n"
    "; Furthest the nose tips up or down, in degrees (0 = no limit).\r\n"
    "HeliMaxPitch=30\r\n"
    "; Furthest it leans to a side, in degrees (0 = no limit).\r\n"
    "HeliMaxRoll=40\r\n"
    "; In the Hunter, the mouse wheel switches between rockets and the cannon (instead\r\n"
    "; of the radio station) and HunterFireKey fires the one picked; a message shows\r\n"
    "; which. The game's own fire keys still work. 1 = on, 0 = off.\r\n"
    "HunterWeapons=1\r\n"
    "; Virtual-key code of the Hunter's fire key (32 = Space).\r\n"
    "HunterFireKey=32\r\n"
    "\r\n"
    "[Fixes]\r\n"
    "; \"Martha's Mug Shot\" swaps the Ocean Drive hotel for a stand-in model while you are\r\n"
    "; on its roof. If the mission ends without swapping it back, the hotel stays without\r\n"
    "; walls (and saves keep it that way). Off a mission, it is put back. 1 = on.\r\n"
    "MissionLeftovers=1\r\n"
    "; The riot cheat (FIGHTFIGHTFIGHT, or a trainer's riot option) is stored in the\r\n"
    "; save and can't be turned off. Loading a game puts civilians, cops and the like\r\n"
    "; back to normal; gangs keep their story hostility. 1 = on.\r\n"
    "ClearRiotOnLoad=1\r\n"
    "; The police station's \"player inside\" flag stays set if you leave without its\r\n"
    "; door (teleport, flying out) and goes into the save: every load in Ocean Beach\r\n"
    "; gave 2 stars. It is cleared while you are outside the station. 1 = on.\r\n"
    "PoliceStationFlag=1\r\n"
    "\r\n"
    "[StuntJumps]\r\n"
    "; Unique stunt jumps play at normal speed and the camera keeps following your\r\n"
    "; vehicle as usual, instead of the slow-motion cinematic camera. The jump, the\r\n"
    "; landing check and the \"UNIQUE STUNT BONUS!\" reward work as before. 1 = on.\r\n"
    "NormalCamera=1\r\n"
    "\r\n"
    "[Missions]\r\n"
    "; Hold this key for 3 seconds during a mission's scripted scene (the intros of\r\n"
    "; the RC missions and others that can't be skipped) to fast-forward it. The\r\n"
    "; scene still sets everything up; it just doesn't wait. 1 = on.\r\n"
    "IntroSkip=1\r\n"
    "; Virtual-key code, 13 = Enter.\r\n"
    "IntroSkipKey=13\r\n"
    "; A mission that leaves you without control for this many seconds (outside real\r\n"
    "; cutscenes) is treated as stuck: a message warns, and 15 seconds later it is\r\n"
    "; ended as if failed, so you can start it again from its marker. 0 = off.\r\n"
    "SoftlockSeconds=90\r\n"
    "; Hold this key for 3 seconds on a mission to end it the same way (for a mission\r\n"
    "; stuck while you still have control). Virtual-key code, 35 = End. 0 = off.\r\n"
    "EndMissionKey=35\r\n"
    "; After a mission shows MISSION FAILED (or is ended with the End key or as stuck),\r\n"
    "; ask whether to restart it: Y puts you back\r\n"
    "; in the mission's marker, which starts it again. Only for missions started on\r\n"
    "; foot from a marker. 1 = on.\r\n"
    "RestartPrompt=1\r\n"
    "; Virtual-key codes for yes and no: 89 = Y, 78 = N.\r\n"
    "RestartYesKey=89\r\n"
    "RestartNoKey=78\r\n"
    "; Pizza Boy: hold the right mouse button and press the left one to throw a pizza;\r\n"
    "; every throw (the game's look left / right + fire too) goes at the customer.\r\n"
    "; 1 = on, 0 = the game's throw.\r\n"
    "PizzaMouseThrow=1\r\n"
    "\r\n"
    "[Weapons]\r\n"
    "; Walk, run and sprint while firing any gun, each with its own animation (the fire\r\n"
    "; animations no longer lock the legs). Sprint and jump with the rocket launcher and\r\n"
    "; minigun. The M4, Ruger and M60 aim in third person instead of the first-person\r\n"
    "; scope, so you can move while aiming. 1 = on.\r\n"
    "RunAndGun=1\r\n"
    "; Getting shot no longer stops your shooting (you still flinch). 1 = on.\r\n"
    "KeepFiringWhenHit=1\r\n"
    "; Drive-bys from any car, bike or boat you drive: press DriveByKey (Alt) once to\r\n"
    "; take the gun out, aim with the mouse (the shot goes under the crosshair), fire\r\n"
    "; to shoot, mouse wheel to switch between pistol, SMG and rifle; press it again\r\n"
    "; to put the gun away. The gun in your hand is kept when you get in. Q / E\r\n"
    "; drive-bys work as in the game. 1 = on.\r\n"
    "FreeDriveBy=1\r\n"
    "; Key that takes the gun out / puts it away (virtual-key code, 18 = Alt).\r\n"
    "DriveByKey=18\r\n"
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
    "; Walking cops about while you aren't wanted, at most (the higher ped density\r\n"
    "; filled streets near the police station with them). Cops after you when you\r\n"
    "; are wanted aren't limited. -1 = no limit.\r\n"
    "MaxStreetCops=3\r\n"
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
    "; From the street, a room built into its city building (Ocean View, the mall,\r\n"
    "; Malibu...) is drawn through its open doorways instead of black; windows stay\r\n"
    "; solid. From inside, the street stays drawn too. 1 = on, 0 = the game's black.\r\n"
    "ShowOutside=1\r\n"
    "; Walking through an interior's door doesn't fade the screen to black and back:\r\n"
    "; the room (or the street) is there at once. Missions' and cutscenes' fades stay.\r\n"
    "NoDoorFade=1\r\n";

bool ReplayPlaying() { return *(volatile unsigned char*)0xA10AF6 != 0; }

bool ModifierDown() {
    static const int keys[] = { VK_SHIFT, VK_CONTROL, VK_MENU, VK_LWIN, VK_RWIN };
    for (int k : keys) if (GetAsyncKeyState(k) & 0x8000) return true;
    return false;
}

void SaveSetting(const char* section, const char* key, const char* value) {
    char ini[MAX_PATH];
    GamePath(ini, "VCModernFix.ini");
    if (!WritePrivateProfileStringA(section, key, value, ini))
        Log("Config: could not save [%s] %s=%s (error %lu)", section, key, value, GetLastError());
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
    g_cfg.noIslandScreen = GetPrivateProfileIntA("Streaming", "NoIslandScreen", 1, ini) != 0;
    int flm = (int)GetPrivateProfileIntA("Streaming", "FastLoadMs", 4, ini);
    g_cfg.fastLoadMs = flm <= 0 ? 0 : flm > 20 ? 20 : flm;
    int veh = (int)GetPrivateProfileIntA("Streaming", "VehicleModels", 50, ini);
    g_cfg.vehicleModels = veh <= 0 ? 0 : veh < 25 ? 25 : veh > 50 ? 50 : veh;
    char dd[32];
    GetPrivateProfileStringA("DrawDistance", "Scale", "3.0", dd, sizeof dd, ini);
    float scale = (float)atof(dd);
    g_cfg.drawDistance = !(scale >= 1.0f) ? 1.0f : scale > 5.0f ? 5.0f : scale;
    GetPrivateProfileStringA("DrawDistance", "FarClip", "1.5", dd, sizeof dd, ini);
    scale = (float)atof(dd);
    g_cfg.farClip = !(scale >= 1.0f) ? 1.0f : scale > 3.0f ? 3.0f : scale;
    g_cfg.highView = GetPrivateProfileIntA("DrawDistance", "HighView", 1, ini) != 0;
    g_cfg.fullViewScan = GetPrivateProfileIntA("DrawDistance", "FullViewScan", 1, ini) != 0;
    g_cfg.risingClouds = GetPrivateProfileIntA("Graphics", "RisingClouds", 1, ini) != 0;
    g_cfg.wideNearClip = GetPrivateProfileIntA("Graphics", "WideFovNearClip", 1, ini) != 0;
    g_cfg.stationFlag = GetPrivateProfileIntA("Fixes", "PoliceStationFlag", 1, ini) != 0;
    int an = (int)GetPrivateProfileIntA("Graphics", "Anisotropic", 16, ini);
    g_cfg.anisotropy = an <= 1 ? 0 : an > 16 ? 16 : an;
    g_cfg.twoPassAlpha = GetPrivateProfileIntA("Graphics", "TwoPassAlpha", 1, ini) != 0;
    g_cfg.widescreen = GetPrivateProfileIntA("Graphics", "Widescreen", 1, ini) != 0;
    g_cfg.farLights = GetPrivateProfileIntA("Graphics", "FarLights", 1, ini) != 0;
    {
        char cd[32];
        GetPrivateProfileStringA("Graphics", "CoronaDistance", "5.0", cd, sizeof cd, ini);
        const float k = (float)atof(cd);
        g_cfg.coronaDistance = !(k >= 1.0f) ? 1.0f : k > 10.0f ? 10.0f : k;
    }
    g_cfg.freeCam = GetPrivateProfileIntA("Camera", "FreeCam", 1, ini) != 0;
    int sens = (int)GetPrivateProfileIntA("Camera", "FreeCamSensitivity", 100, ini);
    g_cfg.freeCamSensitivity = sens < 10 ? 10 : sens > 1000 ? 1000 : sens;
    int ret = (int)GetPrivateProfileIntA("Camera", "FreeCamReturnDelay", 1500, ini);
    g_cfg.freeCamReturnDelay = ret < -1 ? -1 : ret > 60000 ? 60000 : ret;
    g_cfg.freeCamInvertY = GetPrivateProfileIntA("Camera", "FreeCamInvertY", 0, ini) != 0;
    {
        const int k = (int)GetPrivateProfileIntA("Camera", "CarLookForwardKey", 0x43, ini);
        g_cfg.carLookForwardKey = k > 0 && k < 256 ? k : 0;
    }
    g_cfg.frameMouseLook = GetPrivateProfileIntA("Camera", "FrameMouseLook", 1, ini) != 0;
    g_cfg.freeOrbitOnFoot = GetPrivateProfileIntA("Camera", "FreeOrbitOnFoot", 1, ini) != 0;
    g_cfg.cameraSwingBehind = GetPrivateProfileIntA("Camera", "CameraSwingBehind", 1, ini) != 0;
    g_cfg.mouseSmoothMs = GetPrivateProfileIntA("Camera", "MouseSmoothing", 4, ini);
    g_cfg.mouseSensitivity = GetPrivateProfileIntA("Camera", "MouseSensitivity", 100, ini);
    if (g_cfg.mouseSensitivity < 10) g_cfg.mouseSensitivity = 10;
    if (g_cfg.mouseSensitivity > 1000) g_cfg.mouseSensitivity = 1000;
    g_cfg.mouseEqualAxes = GetPrivateProfileIntA("Camera", "MouseEqualAxes", 1, ini) != 0;
    if (g_cfg.mouseSmoothMs < 0) g_cfg.mouseSmoothMs = 0;
    if (g_cfg.mouseSmoothMs > 50) g_cfg.mouseSmoothMs = 50;
    g_cfg.freeCamBoats = GetPrivateProfileIntA("Camera", "FreeCamBoats", 1, ini) != 0;
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
    g_cfg.modernKeys = GetPrivateProfileIntA("Controls", "ModernKeys", 1, ini) != 0;
    g_cfg.mouseAim = GetPrivateProfileIntA("Vehicles", "MouseAim", 1, ini) != 0;
    g_cfg.airControl = GetPrivateProfileIntA("Vehicles", "AirControl", 1, ini) != 0;
    int acs = (int)GetPrivateProfileIntA("Vehicles", "AirControlStrength", 100, ini);
    g_cfg.airControlStrength = acs < 25 ? 25 : acs > 300 ? 300 : acs;
    int hw = (int)GetPrivateProfileIntA("Vehicles", "HeliWeight", 100, ini);
    g_cfg.heliWeight = hw < 0 ? 0 : hw > 300 ? 300 : hw;
    g_cfg.hunterWeapons = GetPrivateProfileIntA("Vehicles", "HunterWeapons", 1, ini) != 0;
    int hfk = (int)GetPrivateProfileIntA("Vehicles", "HunterFireKey", 0x20, ini);
    g_cfg.hunterFireKey = hfk > 0 && hfk < 256 ? hfk : 0x20;
    g_cfg.heliControls = GetPrivateProfileIntA("Vehicles", "HeliControls", 1, ini) != 0;
    g_cfg.planeControls = GetPrivateProfileIntA("Vehicles", "PlaneControls", 1, ini) != 0;
    int hp = (int)GetPrivateProfileIntA("Vehicles", "HeliMaxPitch", 30, ini);
    g_cfg.heliMaxPitch = hp <= 0 ? 0 : hp < 10 ? 10 : hp > 85 ? 85 : hp;
    int hr = (int)GetPrivateProfileIntA("Vehicles", "HeliMaxRoll", 40, ini);
    g_cfg.heliMaxRoll = hr <= 0 ? 0 : hr < 10 ? 10 : hr > 85 ? 85 : hr;
    g_cfg.clearRiotOnLoad = GetPrivateProfileIntA("Fixes", "ClearRiotOnLoad", 1, ini) != 0;
    g_cfg.missionLeftovers = GetPrivateProfileIntA("Fixes", "MissionLeftovers", 1, ini) != 0;
    g_cfg.stuntJumpNormal = GetPrivateProfileIntA("StuntJumps", "NormalCamera", 1, ini) != 0;
    g_cfg.introSkip = GetPrivateProfileIntA("Missions", "IntroSkip", 1, ini) != 0;
    int isk = (int)GetPrivateProfileIntA("Missions", "IntroSkipKey", 0x0D, ini);
    g_cfg.introSkipKey = isk > 0 && isk < 256 ? isk : 0x0D;
    int soft = (int)GetPrivateProfileIntA("Missions", "SoftlockSeconds", 90, ini);
    g_cfg.softlockSeconds = soft <= 0 ? 0 : soft < 30 ? 30 : soft > 600 ? 600 : soft;
    int emk = (int)GetPrivateProfileIntA("Missions", "EndMissionKey", 0x23, ini);
    g_cfg.endMissionKey = emk > 0 && emk < 256 ? emk : 0;
    g_cfg.missionRestart = GetPrivateProfileIntA("Missions", "RestartPrompt", 1, ini) != 0;
    g_cfg.pizzaMouseThrow = GetPrivateProfileIntA("Missions", "PizzaMouseThrow", 1, ini) != 0;
    int ryk = (int)GetPrivateProfileIntA("Missions", "RestartYesKey", 0x59, ini);
    g_cfg.restartYesKey = ryk > 0 && ryk < 256 ? ryk : 0x59;
    int rnk = (int)GetPrivateProfileIntA("Missions", "RestartNoKey", 0x4E, ini);
    g_cfg.restartNoKey = rnk > 0 && rnk < 256 ? rnk : 0x4E;
    g_cfg.runAndGun = GetPrivateProfileIntA("Weapons", "RunAndGun", 1, ini) != 0;
    g_cfg.freeDriveBy = GetPrivateProfileIntA("Weapons", "FreeDriveBy", 1, ini) != 0;
    int dbk = (int)GetPrivateProfileIntA("Weapons", "DriveByKey", 0x12, ini);
    g_cfg.driveByKey = dbk > 0 && dbk < 256 ? dbk : 0x12;
    g_cfg.keepFiringWhenHit = GetPrivateProfileIntA("Weapons", "KeepFiringWhenHit", 1, ini) != 0;
    GetPrivateProfileStringA("Population", "PedDensity", "2.5", dd, sizeof dd, ini);
    scale = (float)atof(dd);
    g_cfg.pedDensity = !(scale > 0) ? 0.0f : scale < 0.5f ? 0.5f : scale > 3.0f ? 3.0f : scale;
    GetPrivateProfileStringA("Population", "CarDensity", "1.5", dd, sizeof dd, ini);
    scale = (float)atof(dd);
    g_cfg.carDensity = !(scale > 0) ? 0.0f : scale < 0.5f ? 0.5f : scale > 3.0f ? 3.0f : scale;
    {
        const int c = (int)GetPrivateProfileIntA("Population", "MaxStreetCops", 3, ini);
        g_cfg.maxStreetCops = c < 0 ? -1 : c > 50 ? 50 : c;
    }
    GetPrivateProfileStringA("Population", "KeepOffscreen", "50", dd, sizeof dd, ini);
    scale = (float)atof(dd);
    g_cfg.offscreenKeep = !(scale > 0) ? 0.0f : scale < 25.0f ? 25.0f : scale > 50.0f ? 50.0f : scale;

    char logPath[MAX_PATH];
    GamePath(logPath, "VCModernFix.log");
    // One log per session; the last session's is kept (after a crash, restarting the
    // game would otherwise wipe the log that shows it).
    char prevPath[MAX_PATH];
    GamePath(prevPath, "VCModernFix.previous.log");
    CloseLog();   // a line logged before this (none today) would have opened the old file
    if (!MoveFileExA(logPath, prevPath, MOVEFILE_REPLACE_EXISTING)) DeleteFileA(logPath);
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

// One handle to VCModernFix.log for the session, opened at the first line (after
// LoadConfig has moved the last session's log aside: CloseLog). Each line used to open,
// append and close the file (fopen / fclose), which bursts such as the interior lines
// or the census paid for every line. Now each line is formatted in memory and written
// with one WriteFile on a handle opened for appending (FILE_APPEND_DATA): every write
// lands at the end of the file in one piece, even from several threads (the memory
// and preload threads log too), and the watchdog's own appends (watchdog.cpp RawLog)
// still go after it. Nothing is buffered in the process: once WriteFile returns, the
// line is in the file as other programs see it, and it survives the game crashing.
// The file stays readable (and writable for the watchdog) while the game runs.
static void* volatile g_logHandle = nullptr;

static void CloseLog() {
    if (void* h = InterlockedExchangePointer((void* volatile*)&g_logHandle, nullptr))
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
}

static HANDLE LogHandle() {
    void* h = g_logHandle;
    if (h) return h;
    char path[MAX_PATH];
    GamePath(path, "VCModernFix.log");
    HANDLE f = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return INVALID_HANDLE_VALUE;   // tried again at the next line
    if (void* other = InterlockedCompareExchangePointer((void* volatile*)&g_logHandle, f, nullptr)) {
        CloseHandle(f);   // another thread opened it first
        return other;
    }
    return f;
}

// Called from the game thread, where Direct3D 8 has set the x87 FPU to single
// precision; the C library expects the default (64-bit mantissa, all exceptions
// masked), so that is set for the duration of the call and the game's own control
// word put back after.
void Log(const char* fmt, ...) {
    unsigned short gameCw, defaultCw = 0x037F;
    __asm__ volatile("fnstcw %0" : "=m"(gameCw));
    __asm__ volatile("fldcw %0" : : "m"(defaultCw));
    char stackBuf[1024];
    char* buf = stackBuf;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(stackBuf, sizeof stackBuf - 2, fmt, ap);
    va_end(ap);
    if (n >= (int)sizeof stackBuf - 2) {   // a long line (the census): formatted again in a big enough buffer
        if (char* big = (char*)malloc(n + 3)) {
            va_start(ap, fmt);
            vsnprintf(big, n + 1, fmt, ap);
            va_end(ap);
            buf = big;
        } else {
            n = (int)sizeof stackBuf - 3;
        }
    }
    if (n >= 0) {
        buf[n] = '\r';   // as the text-mode fopen wrote it
        buf[n + 1] = '\n';
        const HANDLE h = LogHandle();
        DWORD written;
        if (h != INVALID_HANDLE_VALUE) WriteFile(h, buf, n + 2, &written, nullptr);
    }
    if (buf != stackBuf) free(buf);
    __asm__ volatile("fldcw %0" : : "m"(gameCw));
}
