// Quick save (F5) and quick load (F9), into one of the game's own save slots.
//
// Quick save writes the game exactly as it is, with the game's own save code:
// C_PcSave::SaveSlot (0x61D8B0) minus its DoGameSpecificStuffBeforeSave step
// (0x61A690), which is what makes a safehouse save pass 6 hours, refill stamina
// and tidy memory - a quick save keeps the time of day. That is: the slot's file
// name (0x61A630), the user-files folder (0x602240), CFileMgr::OpenFile "wb"
// (0x48DF90), GenericSave (0x61C420), CFileMgr::CloseFile (0x48DEA0), then the
// menu's slot list is refreshed (PopulateSlotInfo 0x61D4A0), so the save also
// shows in Load Game like any other.
//
// Not saved (a message says why): wanted level, on a mission
// (CTheScripts::IsPlayerOnAMission 0x44FE30), on water (in a boat, or at the
// water surface by CWaterLevel::GetWaterLevel 0x5C2C80), and - because such a
// save would not load properly - while the player is dead or player controls are
// switched off (cutscenes, scripted scenes).
//
// Quick load does what the menu's Load Game does (0x497306): CheckSlotDataValid
// (0x61A6C0), fade the audio out, set "restart" and "load a saved game"; the main
// loop (0x6004F0) then restarts the game from that slot. It runs at the end of a
// game update, after the frame is drawn, so nothing is drawn after the game's
// models are released.
#include "quicksave.h"
#include "config.h"
#include <windows.h>
#include <string.h>

static const DWORD GAME_STATE = 0x9B5F08;          // 9 = playing
static const DWORD MENU_ACTIVE = 0x869668;
static const DWORD MENU = 0x869630;                 // FrontEndMenuManager
static const DWORD WANT_RESTART = 0x869669, WANT_LOAD = 0x86966C;
static const DWORD GAME_WINDOW = 0x7897A4;
static const DWORD PCSAVE = 0x9753C4;               // PcSaveHelper (error code at +0)
static const DWORD SAVE_NAME = 0x978428;            // ValidSaveName
static const DWORD AUDIO = 0xA10B8A;                // DMAudio
static const DWORD PAD0_DISABLE = 0x7DBCB0 + 0xF0;  // CPad::GetPad(0)->DisablePlayerControls
static const DWORD CLOCK = 0x974B2C;
static const DWORD PED_STATE = 0x244, PED_HEALTH = 0x354, PED_WANTED = 0x5F4, WANTED_LEVEL = 0x20;
static const DWORD PED_VEHICLE = 0x3A8, PED_IN_VEHICLE = 0x3AC;
static const DWORD ENT_POS = 0x34, VEH_TYPE = 0x29C;   // vehicle type 1 = boat
static const DWORD ENT_AREA = 0x5F, CURR_AREA = 0x978810;   // entity's area, CGame::currArea

typedef unsigned char* (__cdecl* Find_t)();
static const Find_t FindPlayerPed = (Find_t)0x4BC120, FindPlayerVehicle = (Find_t)0x4BC1E0;
static const auto OnMission     = (bool (__cdecl*)())0x44FE30;
static const auto WaterLevel    = (bool (__cdecl*)(float, float, float, float*, bool))0x5C2C80;
static const auto MakeSaveName  = (void (__cdecl*)(int))0x61A630;
static const auto UserFolder    = (void (__cdecl*)())0x602240;
static const auto GameOpenFile  = (int (__cdecl*)(const char*, const char*))0x48DF90;
static const auto GenericSave   = (bool (__cdecl*)(int))0x61C420;
static const auto GameCloseFile = (int (__cdecl*)(int))0x48DEA0;
static const auto PopulateSlots = (void (__thiscall*)(void*))0x61D4A0;
static const auto SlotValid     = (bool (__cdecl*)(int))0x61A6C0;
static const auto SetHelp       = (void (__cdecl*)(const wchar_t*, bool, bool))0x55BFC0;
static const auto AudioA        = (void (__thiscall*)(void*))0x5F9D80;
static const auto AudioB        = (void (__thiscall*)(void*))0x5F9E50;
static const auto EffectsVol    = (void (__thiscall*)(void*, unsigned char))0x5F9CC0;
static const auto MusicVol      = (void (__thiscall*)(void*, unsigned char))0x5F9C90;
static const auto ResetTimers   = (void (__thiscall*)(void*, unsigned))0x5F98D0;

// Messages go into the game's help box (top left, like "Press ... to ..." hints).
// They are shown at the end of the next game update rather than in the middle of
// saving, so nothing the save does in between can clear them.
static wchar_t g_msg[128], g_pendingMsg[128];
static bool g_hasMsg = false;
static void Show(const wchar_t* text) {
    lstrcpynW(g_pendingMsg, text, 128);
    g_hasMsg = true;
}
static void FlushMessage() {
    if (!g_hasMsg || *(int*)0x9B5F08 != 9 || *(unsigned char*)0x869668) return;   // playing, no menu
    lstrcpynW(g_msg, g_pendingMsg, 128);   // the game keeps the pointer: static storage
    SetHelp(g_msg, true, false);
    g_hasMsg = false;
}

// "Quick Loaded" once the loaded game has been running for a moment (after the
// fade-in and after your vehicle is back).
static bool g_loadedMsg = false;
static DWORD g_loadedSince = 0;
static bool g_carPendingFwd();   // defined with the vehicle code below
static void LoadedMessage() {
    if (!g_loadedMsg || *(int*)0x9B5F08 != 9 || *(unsigned char*)0x869668) return;
    const DWORD now = *(DWORD*)0x974B2C;
    if (!g_loadedSince) { g_loadedSince = now ? now : 1; return; }
    if (now - g_loadedSince < 800 || g_carPendingFwd()) return;
    g_loadedMsg = false;
    Show(L"Quick Loaded");
}

static bool InGame() {
    if (*(int*)GAME_STATE != 9 || *(unsigned char*)MENU_ACTIVE || !FindPlayerPed()) return false;
    HWND w = *(HWND*)GAME_WINDOW;
    return w && GetForegroundWindow() == w;
}

static bool OnWater(unsigned char* ped) {
    unsigned char* veh = FindPlayerVehicle();
    if (veh && *(int*)(veh + VEH_TYPE) == 1) return true;   // in a boat
    const float* p = (const float*)((veh ? veh : ped) + ENT_POS);
    float level;
    return WaterLevel(p[0], p[1], p[2], &level, true) && p[2] < level + 1.0f;
}

// The save's name (shown in Load Game) is the text GenericSave looks up for the
// last mission passed (CText::Get at 0x61C51A, first 0x30 bytes of the file after
// its 4-byte block size). During a quick save that lookup returns "Quick Save".
static const DWORD NAME_SITE = 0x61C51A, TEXT_GET = 0x584F30;
static const wchar_t kQuickName[] = L"Quick Save", kAutoName[] = L"Auto Save";
static const wchar_t* g_naming = nullptr;   // the name while VCModernFix saves
static DWORD g_textGet = TEXT_GET;

static const wchar_t* __fastcall HookSaveName(void* text, void*, const char* key) {
    if (g_naming) return g_naming;
    return ((const wchar_t*(__thiscall*)(void*, const char*))g_textGet)(text, key);
}

static bool IsNamed(const char* path, const wchar_t* name) {
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    unsigned char head[4 + 0x30] = {};
    DWORD got = 0;
    ReadFile(h, head, sizeof head, &got, nullptr);
    CloseHandle(h);
    return got == sizeof head && !memcmp(head + 4, name, (lstrlenW(name) + 1) * sizeof(wchar_t));
}

// A quick save into a slot that holds one of your own saves first copies that save
// to "<name> (before quick save).b" next to it ("... 2).b", "... 3).b" if taken);
// existing files are never overwritten. If the copy fails, nothing is saved.
static bool BackUpOtherSave(const char* path, const wchar_t* name, const char* what) {
    const DWORD attr = GetFileAttributesA(path);
    if (attr == INVALID_FILE_ATTRIBUTES) {
        Log("QuickSave: %s does not exist yet (GetFileAttributes error %lu)", path, GetLastError());
        return true;
    }
    if (IsNamed(path, name)) { Log("QuickSave: %s holds the previous %s; replacing it", path, what); return true; }
    char base[MAX_PATH], backup[MAX_PATH];
    lstrcpynA(base, path, MAX_PATH - 32);
    int n = lstrlenA(base);
    if (n > 2 && !lstrcmpiA(base + n - 2, ".b")) base[n - 2] = 0;
    for (int i = 1; i < 100; i++) {
        if (i == 1) wsprintfA(backup, "%s (before %s).b", base, what);
        else wsprintfA(backup, "%s (before %s %d).b", base, what, i);
        if (CopyFileA(path, backup, TRUE)) {
            Log("QuickSave: %s holds one of your saves; copied it to %s first", path, backup);
            return true;
        }
        if (GetLastError() != ERROR_FILE_EXISTS) break;
    }
    Log("QuickSave: could not back up %s (error %lu); not saved", path, GetLastError());
    return false;
}

// CPools::SavePedPool (0x4BEEBE) writes the player only when not in a vehicle
// (bInVehicle +0x3AC == 0): a safehouse save is always made on foot. A save made
// in a car would have no player, and the first script to look for the player after
// loading it crashes (0x638341). So for the save itself the player is put on foot,
// upright, where they sit; everything is put back right after. Loading it puts you
// on foot at that spot (vehicles are not part of a save, as in the original game).
struct OnFoot {
    unsigned char* ped;
    unsigned char save[0x40], inVehicle, vehicle[4], state[4];
    explicit OnFoot(unsigned char* p) : ped(nullptr) {
        if (!p[PED_IN_VEHICLE]) return;
        ped = p;
        memcpy(save, p + 4, 0x40);
        inVehicle = p[PED_IN_VEHICLE];
        memcpy(vehicle, p + PED_VEHICLE, 4);
        memcpy(state, p + PED_STATE, 4);
        float* m = (float*)(p + 4);   // right, forward, up, position (16 bytes each)
        float fx = m[4], fy = m[5], l = fx * fx + fy * fy;
        if (l > 0.0001f) {
            float s = 1.0f / __builtin_sqrtf(l);
            fx *= s; fy *= s;
            m[0] = fy; m[1] = -fx; m[2] = 0;
            m[4] = fx; m[5] = fy; m[6] = 0;
            m[8] = 0; m[9] = 0; m[10] = 1;
        }
        m[14] += 0.5f;   // a little above the seat, so they land on their feet
        p[PED_IN_VEHICLE] = 0;
        *(void**)(p + PED_VEHICLE) = nullptr;
        *(int*)(p + PED_STATE) = 1;   // PED_IDLE
    }
    ~OnFoot() {
        if (!ped) return;
        memcpy(ped + 4, save, 0x40);
        ped[PED_IN_VEHICLE] = inVehicle;
        memcpy(ped + PED_VEHICLE, vehicle, 4);
        memcpy(ped + PED_STATE, state, 4);
    }
};

// ---------------------------------------------------------------- your vehicle
// A save keeps no vehicles (only garages do). The vehicle you are in at F5 is
// written next to the save as GTAVCsf<slot>.quickcar, tied to that exact save by
// its checksum (the file's last 4 bytes). After F9 loads that save, the vehicle is
// created again where it was - same model, colours and health - and you are put in
// the driver's seat, the way the game's own spawning does it (CStreaming::RequestModel,
// CVehicle::operator new + constructor, CWorld::Add, CPed::WarpPedIntoCar).
// Loading the quick save from the menu brings no vehicle back.
struct QuickCar {
    unsigned magic, checksum;
    int model, type;          // type: CVehicleModelInfo +0x3C, 0 car (helis too), 5 bike
    float matrix[16];
    unsigned char color1, color2, pad[2];
    float health;
};
static const unsigned QUICKCAR_MAGIC = 0x31435156;   // "VQC1"
static const DWORD MODEL_INFOS = 0x92D4C8, STREAM_INFO = 0x94DDD8, MI_VEHTYPE = 0x3C;
static const DWORD ENT_MODEL = 0x5C, ENT_STATUS = 0x50, VEH_COLOR1 = 0x1A0, VEH_COLOR2 = 0x1A1;
static const DWORD VEH_HEALTH = 0x204, VEH_DOORLOCK = 0x230;
static const auto RequestModel   = (void (__cdecl*)(int, int))0x40E310;
static const auto LoadRequested  = (void (__cdecl*)(bool))0x40B5F0;
static const auto ModelDeletable = (void (__cdecl*)(int))0x40AEA0;
static const auto TxdDeletable   = (void (__cdecl*)(int))0x40ADF0;
static const auto VehicleNew     = (void* (__cdecl*)(unsigned))0x5BAB20;
static const auto AutomobileCtor = (unsigned char* (__thiscall*)(void*, int, int))0x59E620;
static const auto BikeCtor       = (unsigned char* (__thiscall*)(void*, int, int))0x615740;
static const auto WorldAdd       = (void (__cdecl*)(void*))0x4DB3F0;
static const auto SetObjective   = (void (__thiscall*)(void*, int, void*))0x521F10;
static const auto WarpIntoCar    = (void (__thiscall*)(void*, void*))0x4EF8B0;

static QuickCar g_car;
static bool g_carPending = false;
static DWORD g_carSince = 0;
static bool g_carPendingFwd() { return g_carPending; }

static void CarPath(const char* save, char* out) {
    lstrcpynA(out, save, MAX_PATH - 16);
    int n = lstrlenA(out);
    if (n > 2 && !lstrcmpiA(out + n - 2, ".b")) out[n - 2] = 0;
    lstrcatA(out, ".quickcar");
}

static bool SaveChecksum(const char* save, unsigned* sum) {
    HANDLE h = CreateFileA(save, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD got = 0;
    bool ok = SetFilePointer(h, -4, nullptr, FILE_END) != INVALID_SET_FILE_POINTER &&
              ReadFile(h, sum, 4, &got, nullptr) && got == 4;
    CloseHandle(h);
    return ok;
}

// Called before the save is written: what to bring back (nothing if on foot).
static bool RecordCar(unsigned char* veh, QuickCar& c) {
    if (!veh) return false;
    c.magic = QUICKCAR_MAGIC;
    c.model = *(short*)(veh + ENT_MODEL);
    unsigned char* mi = c.model > 0 && c.model < 0x1964 ? ((unsigned char**)MODEL_INFOS)[c.model] : nullptr;
    c.type = mi ? *(int*)(mi + MI_VEHTYPE) : -1;
    if (c.type != 0 && c.type != 5) {
        Log("QuickSave: vehicle model %d (type %d) can't be brought back on quick load", c.model, c.type);
        return false;
    }
    memcpy(c.matrix, veh + 4, sizeof c.matrix);
    c.color1 = veh[VEH_COLOR1];
    c.color2 = veh[VEH_COLOR2];
    c.health = *(float*)(veh + VEH_HEALTH);
    return true;
}

static void WriteCar(const char* save, bool have, QuickCar& c) {
    char path[MAX_PATH];
    CarPath(save, path);
    if (!have || !SaveChecksum(save, &c.checksum)) { DeleteFileA(path); return; }
    HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) { Log("QuickSave: could not write %s (error %lu)", path, GetLastError()); return; }
    DWORD put = 0;
    WriteFile(h, &c, sizeof c, &put, nullptr);
    CloseHandle(h);
    Log("QuickSave: vehicle model %d kept in %s", c.model, path);
}

// At F9: is there a vehicle for exactly this save?
static void ReadCar(const char* save) {
    g_carPending = false;
    char path[MAX_PATH];
    CarPath(save, path);
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    QuickCar c;
    DWORD got = 0;
    ReadFile(h, &c, sizeof c, &got, nullptr);
    CloseHandle(h);
    unsigned sum;
    if (got != sizeof c || c.magic != QUICKCAR_MAGIC || !SaveChecksum(save, &sum) || sum != c.checksum) {
        Log("QuickSave: %s does not belong to this save; no vehicle brought back", path);
        return;
    }
    g_car = c;
    g_carPending = true;
    g_carSince = 0;
}

// After the load: once the game has run for half a second (the scene and its
// collision are in), the vehicle is created and the player put in it.
static void BringBackCar() {
    unsigned char* ped = FindPlayerPed();
    if (!ped || *(int*)GAME_STATE != 9 || *(unsigned char*)MENU_ACTIVE) return;
    const DWORD now = *(DWORD*)CLOCK;
    if (!g_carSince) { g_carSince = now ? now : 1; return; }
    if (now - g_carSince < 500) return;
    g_carPending = false;
    if (FindPlayerVehicle()) return;
    const int model = g_car.model;
    unsigned char* si = (unsigned char*)STREAM_INFO + model * 20;
    const bool keep = (si[1] & 1) != 0;
    RequestModel(model, 1);
    LoadRequested(false);
    if (si[0] != 1) { Log("QuickSave: vehicle model %d could not be loaded", model); Show(L"Your vehicle could not be brought back"); return; }
    if (!keep) { ModelDeletable(model); TxdDeletable(model); }
    unsigned char* veh = nullptr;
    if (g_car.type == 5) { if (void* m = VehicleNew(0x4EC)) veh = BikeCtor(m, model, 1); }
    else                 { if (void* m = VehicleNew(0x5DC)) veh = AutomobileCtor(m, model, 1); }
    if (!veh) { Log("QuickSave: vehicle pool full"); Show(L"Your vehicle could not be brought back"); return; }
    memcpy(veh + 4, g_car.matrix, sizeof g_car.matrix);
    ((float*)(veh + 4))[14] += 0.2f;   // just above where it stood
    veh[VEH_COLOR1] = g_car.color1;
    veh[VEH_COLOR2] = g_car.color2;
    if (g_car.health > 0.0f) *(float*)(veh + VEH_HEALTH) = g_car.health;
    veh[ENT_STATUS] = (veh[ENT_STATUS] & 7) | 0x20;   // STATUS_ABANDONED, like the spawn cheat
    *(int*)(veh + VEH_DOORLOCK) = 1;                  // unlocked
    WorldAdd(veh);
    SetObjective(ped, 0x12, veh);   // OBJECTIVE_ENTER_CAR_AS_DRIVER, as opcode 0369 does
    WarpIntoCar(ped, veh);
    Log("QuickSave: vehicle model %d brought back, player in the driver's seat", model);
}

// Why the game can't be saved right now (nullptr: it can).
static const wchar_t* Blocked(unsigned char* ped) {
    unsigned char* wanted = *(unsigned char**)(ped + PED_WANTED);
    if (wanted && *(int*)(wanted + WANTED_LEVEL) > 0) return L"Can't quick save while wanted by the police";
    if (OnMission()) return L"Can't quick save during a mission";
    if (OnWater(ped)) return L"Can't quick save on water";
    if (*(unsigned short*)PAD0_DISABLE || *(float*)(ped + PED_HEALTH) <= 0.0f ||
        *(int*)(ped + PED_STATE) == 54 || *(int*)(ped + PED_STATE) == 55)
        return L"Can't quick save right now";
    return nullptr;
}

// For the save only: the player stands at `at` (a safehouse's save point), in the
// city (area 0), facing `heading`; everything is put back right after.
struct AtSavePoint {
    unsigned char* ped = nullptr;
    unsigned char matrix[0x40], area;
    int currArea;
    AtSavePoint(unsigned char* p, const float* at) {
        if (!p || !at) return;
        ped = p;
        memcpy(matrix, p + 4, 0x40);
        area = p[ENT_AREA];
        currArea = *(int*)CURR_AREA;
        float* m = (float*)(p + 4);
        m[0] = 1; m[1] = 0; m[2] = 0;   // upright, facing north like a fresh spawn
        m[4] = 0; m[5] = 1; m[6] = 0;
        m[8] = 0; m[9] = 0; m[10] = 1;
        m[12] = at[0]; m[13] = at[1]; m[14] = at[2] + 0.5f;   // a little above the marker
        p[ENT_AREA] = 0;
        *(int*)CURR_AREA = 0;
    }
    ~AtSavePoint() {
        if (!ped) return;
        memcpy(ped + 4, matrix, 0x40);
        ped[ENT_AREA] = area;
        *(int*)CURR_AREA = currArea;
    }
};

// Writes the game to slot `slotNumber` (1-8) named `name`; `what`: "quick save" /
// "auto save". `at`: where the player is saved (nullptr: where they are).
static bool WriteSave(int slotNumber, const wchar_t* name, const char* what, const float* at = nullptr) {
    unsigned char* ped = FindPlayerPed();
    const int slot = slotNumber - 1;
    MakeSaveName(slot);
    *(int*)PCSAVE = 0;
    UserFolder();
    if (!BackUpOtherSave((const char*)SAVE_NAME, name, what)) return false;
    int file = GameOpenFile((const char*)SAVE_NAME, "wb");
    if (!file) { *(int*)PCSAVE = 1; Log("QuickSave: could not create %s", (const char*)SAVE_NAME); return false; }
    LARGE_INTEGER f, t0, t1;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    bool ok;
    QuickCar car;
    const bool haveCar = !at && RecordCar(FindPlayerVehicle(), car);
    {
        OnFoot onFoot(ped);
        if (onFoot.ped && !at) Log("QuickSave: player is in a vehicle; saved on foot at that spot");
        AtSavePoint savePoint(ped, at);   // after OnFoot, undone before it
        g_naming = name;   // the save's name in Load Game
        ok = GenericSave(file);
        g_naming = nullptr;
    }
    if (GameCloseFile(file)) ok = false;
    QueryPerformanceCounter(&t1);
    PopulateSlots((void*)PCSAVE);
    WriteCar((const char*)SAVE_NAME, ok && haveCar, car);
    if (ok) Log("QuickSave: %s to slot %d (%s) in %u ms", what, slot + 1, (const char*)SAVE_NAME,
                (unsigned)((t1.QuadPart - t0.QuadPart) * 1000 / f.QuadPart));
    else Log("QuickSave: writing %s failed", (const char*)SAVE_NAME);
    return ok;
}

static bool AutoSavePending();   // the mission auto save is waiting to save
static DWORD g_timedSince = 0;   // game time of the last save (any kind), for the timed quick save

static void QuickSave() {
    const wchar_t* why = Blocked(FindPlayerPed());
    if (why) { Show(why); Log("QuickSave: not saved (%ls)", why); return; }
    const bool ok = WriteSave(g_cfg.quickSaveSlot, kQuickName, "quick save");
    if (ok) g_timedSince = *(DWORD*)CLOCK;
    Show(ok ? L"Quick Saved" : L"Quick save failed");
}

// ---------------------------------------------------------------- timed quick save
// Every [QuickSave] AutoMinutes of play (game time: paused menus don't count) the
// game is quick saved, into the quick save slot, so F9 loads it. Only when a quick
// save is allowed (Blocked: no mission or job, not wanted, not on water, in control,
// alive) and also: not on a phone call, not in a cutscene, not falling or in the air
// (vertical speed, CPhysical::m_vecMoveSpeed +0x70), and no mission auto save
// waiting. When it is due but not allowed it waits, checking each update, and saves
// as soon as everything is clear. Any save (F5, auto save) restarts the count, and
// so does loading a game.
static const DWORD PHYS_SPEED = 0x70, CUTSCENE_RUNNING = 0xA10AB2;   // CCutsceneMgr::ms_running
static const int PED_ANSWER_MOBILE = 36;
static DWORD g_timedClock = 0;
static bool g_timedWaitLogged = false;

static const char* TimedBlocked(unsigned char* ped) {
    if (Blocked(ped)) return "quick save not allowed (mission, wanted, water or no control)";
    if (*(int*)(ped + PED_STATE) == PED_ANSWER_MOBILE) return "on a phone call";
    if (*(unsigned char*)CUTSCENE_RUNNING) return "cutscene";
    unsigned char* veh = FindPlayerVehicle();
    const float vz = ((const float*)((veh ? veh : ped) + PHYS_SPEED))[2];
    if (vz > 0.03f || vz < -0.03f) return "in the air";
    if (AutoSavePending()) return "mission auto save waiting";
    return nullptr;
}

static void TimedQuickSave() {
    if (g_cfg.quickSaveEvery <= 0) return;
    unsigned char* ped = FindPlayerPed();
    if (*(int*)GAME_STATE != 9 || *(unsigned char*)MENU_ACTIVE || !ped) return;
    const DWORD now = *(DWORD*)CLOCK;
    if (!g_timedSince || now < g_timedClock || now - g_timedClock > 10000) {   // start, or a game was loaded
        g_timedSince = now ? now : 1;
        g_timedWaitLogged = false;
    }
    g_timedClock = now;
    if (now - g_timedSince < (DWORD)g_cfg.quickSaveEvery * 60000u || g_carPending || g_loadedMsg) return;
    if (const char* why = TimedBlocked(ped)) {
        if (!g_timedWaitLogged) Log("QuickSave: timed quick save due; waiting (%s)", why);
        g_timedWaitLogged = true;
        return;
    }
    g_timedWaitLogged = false;
    g_timedSince = now;
    Show(WriteSave(g_cfg.quickSaveSlot, kQuickName, "timed quick save") ? L"Quick Saved (timed)"
                                                                          : L"Timed quick save failed");
}

// ---------------------------------------------------------------- auto save
// A mission or job is anything that sets the script's "on a mission" flag: story
// missions, and taxi, vigilante, firefighter, paramedic, rampages... When it ends
// (the flag goes back to 0) without being wasted or busted (CPlayerInfo
// m_WBState, 0x94ADF4), and either a mission was passed (CStats::
// LastMissionPassedName, 0x7E9D78, changed) or money was earned (Players[0].
// m_nMoney, 0x94ADC8), the game is saved a few seconds later, once a quick save
// would be allowed (not wanted, not on water, in control). After a game is loaded
// (the game timer, 0x974B2C, jumps) it starts over without saving.
static const DWORD WB_STATE = 0x94ADF4, MONEY = 0x94ADC8, LAST_PASSED = 0x7E9D78;
static struct {
    bool onMission, pending;
    int money;
    char lastPassed[8];
    DWORD since, lastClock;
} g_auto;

// Like a safehouse save, an auto save puts you at a safehouse when loaded: the
// save point of the nearest safehouse you own. Each owned safehouse runs its save
// script, PSAVE1-10 (started when it is bought; PSAVE1, the Ocean View hotel, from
// the start), found in CTheScripts::pActiveScripts (0x975338; CRunningScript: next
// +0, name +8). Their save points are where those scripts create the save pickup
// (opcode 0213, model PICKUPSAVE); PSAVE5 takes its point from $570-$572.
static const DWORD ACTIVE_SCRIPTS = 0x975338, SCRIPT_SPACE = 0x821280;
static const struct { const char* name; float x, y, z; } kSavePoints[] = {
    { "psave1", 219.3f, -1273.5f, 11.5f },  { "psave2", -378.3f, -597.2f, 25.8f },
    { "psave3", -1033.8f, -846.9f, 13.0f }, { "psave4", -992.7f, 193.4f, 11.4f },
    { "psave5", 0, 0, 0 },                  { "psave6", -664.1f, -1476.3f, 13.8f },
    { "psave7", -878.5f, -575.1f, 11.2f },  { "psave8", -1059.6f, -282.2f, 11.2f },
    { "psave9", 0.0f, 957.9f, 11.1f },      { "psave10", 499.5f, -66.9f, 11.4f },
};

static void NearestSavePoint(unsigned char* ped, float* out) {
    const float* p = (const float*)(ped + ENT_POS);
    float best = 1e30f;
    const char* which = nullptr;
    out[0] = kSavePoints[0].x; out[1] = kSavePoints[0].y; out[2] = kSavePoints[0].z;   // Ocean View
    int guard = 0;
    for (unsigned char* s = *(unsigned char**)ACTIVE_SCRIPTS; s && guard < 200; s = *(unsigned char**)s, guard++) {
        const char* name = (const char*)(s + 8);
        for (const auto& k : kSavePoints) {
            if (lstrcmpiA(name, k.name)) continue;
            float pt[3] = { k.x, k.y, k.z };
            if (k.x == 0 && k.y == 0) {   // PSAVE5: from its variables
                const float* v = (const float*)(SCRIPT_SPACE + 570 * 4);
                pt[0] = v[0]; pt[1] = v[1]; pt[2] = v[2];
                if (pt[0] == 0 && pt[1] == 0) continue;
            }
            const float dx = pt[0] - p[0], dy = pt[1] - p[1], d = dx * dx + dy * dy;
            if (d < best) { best = d; which = k.name; out[0] = pt[0]; out[1] = pt[1]; out[2] = pt[2]; }
        }
    }
    Log("AutoSave: saving at the save point of %s (%.0f, %.0f, %.0f)", which ? which : "psave1 (no safehouse found)",
        out[0], out[1], out[2]);
}

static void AutoSave() {
    unsigned char* ped = FindPlayerPed();
    if (*(int*)GAME_STATE != 9 || *(unsigned char*)MENU_ACTIVE || !ped) return;
    const DWORD now = *(DWORD*)CLOCK;
    const bool on = OnMission();
    if (now < g_auto.lastClock || now - g_auto.lastClock > 10000) {   // a game was loaded
        g_auto.onMission = on;
        g_auto.pending = false;
        g_auto.money = *(int*)MONEY;
        memcpy(g_auto.lastPassed, (void*)LAST_PASSED, 8);
    }
    g_auto.lastClock = now;
    if (on && !g_auto.onMission) {
        g_auto.money = *(int*)MONEY;
        memcpy(g_auto.lastPassed, (void*)LAST_PASSED, 8);
        g_auto.pending = false;
    } else if (!on && g_auto.onMission) {
        const bool passed = memcmp(g_auto.lastPassed, (void*)LAST_PASSED, 8) != 0;
        const int earned = *(int*)MONEY - g_auto.money;
        const bool lost = *(unsigned char*)WB_STATE != 0 || *(float*)(ped + PED_HEALTH) <= 0.0f;
        if (!lost && (passed || earned > 0)) {
            g_auto.pending = true;
            g_auto.since = now;
            Log("AutoSave: %s ended (%s, $%d earned); saving when possible", passed ? "mission" : "job",
                passed ? "passed" : "finished", earned);
        } else {
            Log("AutoSave: mission/job ended %s; no auto save", lost ? "wasted or busted" : "with nothing gained");
        }
    }
    g_auto.onMission = on;
    if (!g_auto.pending || on || now - g_auto.since < 4000 || Blocked(ped)) return;
    g_auto.pending = false;
    float at[3];
    NearestSavePoint(ped, at);
    const bool ok = WriteSave(g_cfg.autoSaveSlot, kAutoName, "auto save", at);
    if (ok) g_timedSince = now;
    Show(ok ? L"Game auto saved. Loading it starts you at your nearest safehouse." : L"Auto save failed");
}

static bool AutoSavePending() { return g_cfg.autoSave && g_auto.pending; }

static void QuickLoad() {
    const int slot = g_cfg.quickSaveSlot - 1;
    if (!SlotValid(slot)) {
        Show(L"No quick save to load");
        Log("QuickSave: nothing to load in slot %d", slot + 1);
        return;
    }
    Log("QuickSave: loading slot %d", slot + 1);
    MakeSaveName(slot);
    ReadCar((const char*)SAVE_NAME);
    g_loadedMsg = true;
    g_loadedSince = 0;
    void* audio = (void*)AUDIO;
    AudioA(audio);
    AudioB(audio);
    *(unsigned char*)(MENU + 0x11) = 1;
    *(unsigned char*)WANT_RESTART = 1;
    EffectsVol(audio, 0);
    MusicVol(audio, 0);
    for (int i = 0; i < 10; i++) ((int*)0x862460)[i] = 0;
    ResetTimers(audio, *(unsigned*)CLOCK);
    *(unsigned char*)WANT_LOAD = 1;
}

static bool g_fromIdle = false;
static bool g_downSave = false, g_downLoad = false, g_pressSave = false, g_pressLoad = false;

// Every frame: remember a key press until the end of the next game update. A key
// pressed with Shift, Ctrl, Alt or Win held (ShadowPlay's Alt+F9, say) doesn't
// count, and a modifier pressed before the update runs cancels a pending press.
void QuickSaveFrame() {
    if (!g_cfg.quickSave) return;
    const bool save = (GetAsyncKeyState(g_cfg.quickSaveKey) & 0x8000) != 0;
    const bool load = (GetAsyncKeyState(g_cfg.quickLoadKey) & 0x8000) != 0;
    const bool mod = ModifierDown();
    if (save && !g_downSave && !mod) g_pressSave = true;
    if (load && !g_downLoad && !mod) g_pressLoad = true;
    if (mod) g_pressSave = g_pressLoad = false;
    g_downSave = save;
    g_downLoad = load;
}

static void Act() {
    if (g_cfg.autoSave) AutoSave();
    if (!g_cfg.quickSave) return;
    TimedQuickSave();
    const bool save = g_pressSave, load = g_pressLoad;
    g_pressSave = g_pressLoad = false;
    if (!(save || load) || !InGame()) return;
    if (load) QuickLoad();
    else QuickSave();
}

// Start of a game update, before the game runs (fixedtick.cpp): messages go in
// here, where the game's own code and the trainer show theirs.
static bool g_installed = false;   // quick save or auto save is on, and the code is v1.0

void QuickSaveBeforeTick() {
    if (!g_installed) return;
    LoadedMessage();
    FlushMessage();
}

void QuickSaveEndOfTick() {
    if (!g_installed) return;
    g_fromIdle = true;
    if (g_carPending) BringBackCar();
    Act();
}

void QuickSaveAfterPresent() {
    if (!g_installed || g_fromIdle) return;   // FixedTick off: once per frame, after drawing
    QuickSaveFrame();
    if (g_carPending) BringBackCar();
    LoadedMessage();
    FlushMessage();
    Act();
}

void InstallQuickSave() {
    if (!g_cfg.quickSave && !g_cfg.autoSave) { Log("QuickSave: quick save and auto save off"); return; }
    // C_PcSave::SaveSlot: movsx eax,byte [esp+4]; push ebx; push ebp; mov ebx,ecx; push eax; call
    static const unsigned char saveSlot[] = { 0x0F, 0xBE, 0x44, 0x24, 0x04, 0x53, 0x55, 0x89, 0xCB, 0x50, 0xE8 };
    // CTheScripts::IsPlayerOnAMission: cmp dword [OnAMissionFlag], 0
    static const unsigned char onMission[] = { 0x83, 0x3D, 0x48, 0x87, 0x97, 0x00, 0x00 };
    const unsigned char* site = (const unsigned char*)NAME_SITE;
    const DWORD nameTarget = site[0] == 0xE8 ? NAME_SITE + 5 + *(const int*)(site + 1) : 0;
    const char* bad = memcmp((void*)0x61D8B0, saveSlot, sizeof saveSlot) ? "save slot code (0x61D8B0)"
                    : memcmp((void*)0x44FE30, onMission, sizeof onMission) ? "mission check (0x44FE30)"
                    : !nameTarget ? "save name lookup (0x61C51A)" : nullptr;
    if (bad) {
        g_cfg.quickSave = g_cfg.autoSave = false;
        Log("QuickSave: %s is not the v1.0 code (another exe version or mod); not installed", bad);
        return;
    }
    if (nameTarget != TEXT_GET) Log("QuickSave: save name lookup is hooked by another mod (%08lX); chaining to it", nameTarget);
    g_textGet = nameTarget;
    unsigned char call[5] = { 0xE8 };
    *(int*)(call + 1) = (int)((DWORD)&HookSaveName - (NAME_SITE + 5));
    DWORD old;
    VirtualProtect((void*)NAME_SITE, 5, PAGE_EXECUTE_READWRITE, &old);
    memcpy((void*)NAME_SITE, call, 5);
    VirtualProtect((void*)NAME_SITE, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)NAME_SITE, 5);
    g_installed = true;
    if (g_cfg.quickSave)
        Log("QuickSave: installed, save key %d, load key %d (virtual-key codes), slot %d, named \"Quick Save\"",
            g_cfg.quickSaveKey, g_cfg.quickLoadKey, g_cfg.quickSaveSlot);
    if (g_cfg.quickSave && g_cfg.quickSaveEvery > 0)
        Log("QuickSave: timed quick save every %d minutes of play, when safe", g_cfg.quickSaveEvery);
    if (g_cfg.autoSave) Log("AutoSave: on, after missions and jobs, slot %d, named \"Auto Save\"", g_cfg.autoSaveSlot);
}
