// Modern keys ([Controls] ModernKeys): the game's key bindings set to a layout like
// newer games, put back each time the game's menu closes (so a reset or a change in
// the controls menu doesn't undo them) and restored to what they were when turned
// off in the Modern Fix menu.
//
// The bindings are CControllerConfigManager's (ControlsManager, 0x9B491C) table at
// +0xFF8: 44 actions x 4 controller types (0 keyboard, 1 second keyboard key,
// 2 mouse, 3 joystick) x { key, order }. Keys are RenderWare codes: letters and
// Space as ASCII, others from 1000 (the controls menu's names, 0x6164C8, give them:
// Num4 0x407, Num5 0x408, Num6 0x40A, Num8 0x40C, Caps Lock 0x414, left Shift 0x416,
// right Shift 0x417, left Ctrl 0x419, Insert 0x3F5, Num+ 0x401); 0x420 is none.
// SetControllerKeyAssociatedWithAction (0x615F80, thiscall action, key, type) sets
// one and numbers the order, as the game's own defaults (0x619B80) do.
//
// The layout (keyboard; joystick bindings are left as they are):
//   on foot: W A S D move, Shift sprints, Space jumps (the game's), LMB / Num0 fires
//            (left Ctrl doesn't fire: trainers use it, e.g. for a boost); Delete
//            doesn't aim (scope) or zoom: the right mouse button aims
//   vehicle: W / S accelerate / brake (and, as in the game, nose down / up in the
//            air; bikes lean only with Num8 / Num5, below), A / D steer (in a helicopter: turn), Q radio,
//            E horn, Caps Lock starts the vehicle mission (taxi, vigilante...),
//            Num4 / Num6 turret left / right (helicopter roll), Num8 / Num5 turret
//            up / down (helicopter pitch); vehicle look left / right keep Num1 / Num2
//            (Q / E are radio and horn now); no mouse button is the handbrake (the
//            game's RMB): the mouse looks around and aims in vehicles
#include "modernkeys.h"
#include "config.h"
#include <windows.h>
#include <string.h>

static const DWORD CONTROLS = 0x9B491C, SETTINGS = CONTROLS + 0xFF8;
static const int kActions = 44, kNone = 0x420;
static const DWORD MENU_ACTIVE = 0x869668;
static const auto SetKey = (void (__thiscall*)(void*, int, int, int))0x615F80;
static const auto ResetOrder = (void (__thiscall*)(void*, int))0x615E80;

enum {
    FIRE = 0, GO_FORWARD = 3, GO_BACK = 4, GO_LEFT = 5, GO_RIGHT = 6, SNIPER_ZOOM_IN = 7, SNIPER_ZOOM_OUT = 8,
    SPRINT = 12, LOCK_TARGET = 34,
    ACCELERATE = 16, BRAKE = 17, RADIO = 18, HORN = 19, SUBMISSIONS = 20, HANDBRAKE = 21,
    LOOK_LEFT = 24, LOOK_RIGHT = 25, TURRET_LEFT = 27, TURRET_RIGHT = 28, TURRET_UP = 29, TURRET_DOWN = 30,
};
enum {
    K_INSERT = 0x3F5, K_DELETE = 0x3F6, K_UP = 0x3FB, K_DOWN = 0x3FC, K_LEFT = 0x3FD, K_RIGHT = 0x3FE, K_NUMPLUS = 0x401,
    K_NUM1 = 0x404, K_NUM2 = 0x405, K_NUM4 = 0x407, K_NUM5 = 0x408, K_NUM6 = 0x40A, K_NUM8 = 0x40C,
    K_NUM0 = 0x40E, K_CAPS = 0x414, K_LSHIFT = 0x416, K_RSHIFT = 0x417,
};
// action, first key, second key (kNone: none)
static const int kLayout[][3] = {
    { GO_FORWARD, K_UP, 'W' },     { GO_BACK, K_DOWN, 'S' },
    { GO_LEFT, K_LEFT, 'A' },      { GO_RIGHT, K_RIGHT, 'D' },
    { ACCELERATE, K_UP, 'W' },     { BRAKE, K_DOWN, 'S' },
    { SPRINT, K_LSHIFT, K_RSHIFT },
    { FIRE, K_NUM0, kNone },
    { RADIO, 'Q', K_INSERT },      { HORN, 'E', kNone },
    { SUBMISSIONS, K_CAPS, K_NUMPLUS },
    { LOOK_LEFT, K_NUM1, kNone },  { LOOK_RIGHT, K_NUM2, kNone },
    { TURRET_LEFT, K_NUM4, kNone }, { TURRET_RIGHT, K_NUM6, kNone },
    { TURRET_UP, K_NUM8, kNone },  { TURRET_DOWN, K_NUM5, kNone },
};

struct Bind { int key, order; };
static Bind* const Table = (Bind*)SETTINGS;   // [action * 4 + type]
static Bind g_saved[kActions * 4];             // the bindings before the first change
static bool g_haveSaved = false, g_ok = false, g_applied = false, g_wasMenu = false;

static void Apply() {
    if (!g_haveSaved) { memcpy(g_saved, Table, sizeof g_saved); g_haveSaved = true; }
    void* cm = (void*)CONTROLS;
    for (const auto& l : kLayout) {
        for (int t = 0; t < 2; t++) Table[l[0] * 4 + t] = { kNone, 0 };   // both keyboard keys
        ResetOrder(cm, l[0]);
        SetKey(cm, l[0], l[1], 0);
        if (l[2] != kNone) SetKey(cm, l[0], l[2], 1);
    }
    Table[HANDBRAKE * 4 + 2] = { 0, 0 };   // mouse: no button
    ResetOrder(cm, HANDBRAKE);
    // Delete is the game's keyboard key for Target (aim: the scope of the sniper and
    // scoped rifles) and its sniper zoom: off, the right mouse button aims.
    static bool logged = false;
    if (!logged) {
        logged = true;
        char list[128] = "";
        for (int a = 0; a < kActions; a++)
            for (int t = 0; t < 2; t++)
                if (Table[a * 4 + t].key == K_DELETE) wsprintfA(list + lstrlenA(list), " %d", a);
        Log("ModernKeys: Delete was bound to action(s)%s; taken off aim (34) and sniper zoom (7, 8)", list[0] ? list : " none");
    }
    static const int kNoDelete[] = { SNIPER_ZOOM_IN, SNIPER_ZOOM_OUT, LOCK_TARGET };
    for (int a : kNoDelete)
        for (int t = 0; t < 2; t++)
            if (Table[a * 4 + t].key == K_DELETE) { Table[a * 4 + t] = { kNone, 0 }; ResetOrder(cm, a); }
    g_applied = true;
}

static void Restore() {
    if (g_haveSaved) memcpy(Table, g_saved, sizeof g_saved);
    g_applied = false;
}

// ---------------------------------------------------------------- bike lean
// A bike leans by -GetSteeringUpDown + GetCarGunUpDown (CBike's control inputs, calls
// at 0x60A69D and 0x60A6C4, into its lean +0x474). With the keyboard, GetSteeringUpDown
// (0x4AAF50) is the larger of LeftStickY - forward / back, W / S here, the same keys
// as accelerate / brake - and the d-pad up / down. So braking with S leaned the bike
// back (a wheelie out of nowhere), and accelerating with W leaned it forward, which
// cancelled Num5 (lean back). With modern keys a bike's lean comes from the lean keys
// (Num8 / Num5) only: the d-pad part of it here, the car gun part as before.
static const DWORD BIKE_LEAN_CALL = 0x60A69D, STEER_UP_DOWN = 0x4AAF50;
static short __fastcall BikeSteerUpDown(const unsigned char* pad) {
    if (!g_cfg.modernKeys || *(const unsigned short*)(pad + 0xEC) != 0)   // the game's for other pad modes
        return ((short (__thiscall*)(const void*))STEER_UP_DOWN)(pad);
    if (*(const unsigned short*)(pad + 0xF0)) return 0;   // controls disabled
    const int v = (*(const short*)(pad + 0x12) - *(const short*)(pad + 0x10)) / 2;   // d-pad down - up
    return (short)v;
}

static void InstallBikeLean() {
    unsigned char* call = (unsigned char*)BIKE_LEAN_CALL;
    if (call[0] != 0xE8 || BIKE_LEAN_CALL + 5 + *(int*)(call + 1) != STEER_UP_DOWN) {
        Log("ModernKeys: bike lean code is not v1.0 (or changed by another mod); W / S still lean bikes");
        return;
    }
    DWORD old;
    VirtualProtect(call, 5, PAGE_EXECUTE_READWRITE, &old);
    *(int*)(call + 1) = (int)((DWORD)&BikeSteerUpDown - (BIKE_LEAN_CALL + 5));
    VirtualProtect(call, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), call, 5);
    Log("ModernKeys: bikes lean with Num8 / Num5 only (W / S accelerate and brake without leaning)");
}

// ---------------------------------------------------------------- install / per frame
void InstallModernKeys() {
    // SetControllerKeyAssociatedWithAction and ResetSettingOrder as in v1.0
    static const unsigned char setKey[] = { 0x53, 0x8B, 0x5C, 0x24, 0x08, 0x55, 0x53, 0x89 };
    if (memcmp((void*)0x615F80, setKey, sizeof setKey) || *(const unsigned char*)0x615E80 != 0x53) {
        Log("ModernKeys: game code is not v1.0; the game's own key bindings");
        return;
    }
    g_ok = true;
    InstallBikeLean();
    if (g_cfg.modernKeys) Apply();
    Log("ModernKeys: %s (Q radio, E horn, Shift sprint, Caps Lock vehicle mission, WASD, Num4/6/8/5 turret, "
        "no mouse handbrake)", g_cfg.modernKeys ? "on" : "off");
}

// The game's menu closed: the bindings may have been reset or changed there.
void ModernKeysAfterPresent() {
    if (!g_ok) return;
    const bool menu = *(volatile unsigned char*)MENU_ACTIVE != 0;
    if (g_wasMenu && !menu && g_cfg.modernKeys) Apply();
    g_wasMenu = menu;
}

// The Modern Fix menu switched it.
void ModernKeysChanged() {
    if (!g_ok) return;
    if (g_cfg.modernKeys) Apply();
    else if (g_applied) Restore();
}
