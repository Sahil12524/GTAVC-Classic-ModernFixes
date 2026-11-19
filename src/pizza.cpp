// Pizza Boy with the mouse ([Missions] PizzaMouseThrow).
//
// The Pizza Boy job (mission script "pizza", main.scm 0x12B4D2) throws a pizza when,
// on its bike ($7965), look left or look right is held and fire is pressed: it asks
// IS_BUTTON_PRESSED (00E1) pad 0 for button 7 (look right: the box goes from 0.7 m to
// the bike's right, gosub 0x164B) or 5 (look left: -0.7 m, gosub 0x16F9), then 17
// (fire). The throw makes a box (029B, handle $8010), places it by the bike (035C),
// sets $8009 (no second throw until fire is let go: the script clears it only when a
// look key is held without fire) and launches it with SET_OBJECT_VELOCITY (038C
// $8010 $8040 $8041 5.0): 12 straight out of that side. A delivery counts when a
// box comes within 3 m (2D) of a customer with an open order (0472, checked for
// each of them in turn).
//
// Now right mouse held + left mouse throws: those three questions are answered here
// for the side the nearest waiting customer is on (the script's own throw, count and
// pay), and $8009 is cleared when the left button is let go, for the next one. With
// no waiting customer within 30 m the click throws nothing (no pizza wasted). Every
// throw - mouse or the game's keys - also goes at that customer: just before 038C,
// $8040 / $8041 are set to point from the bike at them, fast enough to reach them (at
// least the game's 12). Without one in reach it flies out to the side as in the game.
#include "pizza.h"
#include "config.h"
#include <windows.h>
#include <math.h>
#include <string.h>

static const DWORD SCRIPT_SPACE = 0x821280, PED_POOL = 0x97F2AC, VEH_POOL = 0xA0FDE4;
static const DWORD MENU_ACTIVE = 0x869668, GAME_WINDOW = 0x7897A4, GAME_STATE = 0x9B5F08;
static const DWORD UPDATE_COMPARE = 0x463F00;
static const int V_BIKE = 7965, V_THROWN = 8009, V_BOX = 8010, V_VX = 8040, V_VY = 8041;
static const auto FindPlayerVehicle = (unsigned char* (__cdecl*)())0x4BC1E0;

static int& Var(int n) { return *(int*)(SCRIPT_SPACE + n * 4); }
static float& VarF(int n) { return *(float*)(SCRIPT_SPACE + n * 4); }

// Pool object for a script handle (slot << 8 | its flag byte), or null.
static unsigned char* At(DWORD poolVar, int size, int handle) {
    const unsigned char* pool = *(const unsigned char* const*)poolVar;
    const int slot = handle >> 8;
    if (!pool || slot < 0 || slot >= *(const int*)(pool + 8)) return nullptr;
    if ((*(const unsigned char* const*)(pool + 4))[slot] != (handle & 0xFF)) return nullptr;
    return *(unsigned char* const*)pool + slot * size;
}

static bool IsPizzaScript(const unsigned char* script) { return !lstrcmpiA((const char*)(script + 8), "pizza"); }

static DWORD g_scriptSeen = 0;          // GetTickCount when the pizza script last asked for a button
static bool g_request = false, g_ourThrow = false, g_lmbWas = false;
static DWORD g_requestAt = 0;
static int g_logs = 0;

static unsigned char* Bike() {
    unsigned char* bike = At(VEH_POOL, 0x5DC, Var(V_BIKE));
    return bike && FindPlayerVehicle() == bike ? bike : nullptr;
}

// The customer to throw at: the nearest one with an open order. The job keeps up to
// 10 customers ($7966..$7975), each waiting while its flag ($7996..$8005) is 1;
// $8014 is only the script's loop over them (the last one it checked), so aiming at
// it sometimes threw at another customer, on the other side, and wasted a pizza.
static const int V_CUSTOMERS = 7966, V_WAITING = 7996, kCustomers = 10;
static const float kReach = 30.0f;   // farther than this: no throw from the mouse
static unsigned char* Customer() {
    const unsigned char* bike = Bike();
    if (!bike) return nullptr;
    const float* b = (const float*)(bike + 0x34);
    unsigned char* best = nullptr;
    float bestD2 = kReach * kReach;
    for (int i = 0; i < kCustomers; i++) {
        if (Var(V_WAITING + i) != 1) continue;
        unsigned char* ped = At(PED_POOL, 0x6D8, Var(V_CUSTOMERS + i));
        if (!ped || !(*(const float*)(ped + 0x354) > 0.0f)) continue;   // gone or dead
        const float* q = (const float*)(ped + 0x34);
        const float dx = q[0] - b[0], dy = q[1] - b[1], d2 = dx * dx + dy * dy;
        if (d2 < bestD2) { bestD2 = d2; best = ped; }
    }
    return best;
}

// 7 (right) or 5 (left): the side of the bike the customer is on; 0: no customer in
// reach (the mouse doesn't throw).
static int CustomerSide(const unsigned char* bike) {
    const unsigned char* ped = Customer();
    if (!ped) return 0;
    const float* m = (const float*)(bike + 4);            // right vector first
    const float* q = (const float*)(ped + 0x34);
    const float dx = q[0] - m[12], dy = q[1] - m[13];
    return dx * m[0] + dy * m[1] >= 0.0f ? 7 : 5;
}

// The box's flight, set before SET_OBJECT_VELOCITY runs.
static void AimAtCustomer() {
    const unsigned char* bike = Bike();
    const unsigned char* ped = Customer();
    if (!bike || !ped) return;
    const float* b = (const float*)(bike + 0x34);
    const float* q = (const float*)(ped + 0x34);
    const float dx = q[0] - b[0], dy = q[1] - b[1];
    const float dist = sqrtf(dx * dx + dy * dy);
    if (!(dist > 0.5f)) return;
    float speed = dist * 2.0f;                 // there in about half a second
    speed = speed < 12.0f ? 12.0f : speed;     // never slower than the game's throw
    VarF(V_VX) = dx / dist * speed;
    VarF(V_VY) = dy / dist * speed;
    if (g_logs < 10) { g_logs++; Log("Pizza: box thrown at the nearest waiting customer, %.1f m away", dist); }
}

// The pizza script's throw as v1.0 main.scm has it (IS_BUTTON_PRESSED 0 7 at 0x17CE
// into the mission), checked when the job starts. A mission runs from the mission
// area the game loads it into (script space + 225512, SIZE_MAIN_SCRIPT), not from its
// place in main.scm (0x12B4D2): checked there, the job was always taken for a changed
// one and the mouse throw stayed off.
static const DWORD MISSION_AREA = SCRIPT_SPACE + 225512;
static int g_scmOk = -1;
static bool ScmOk() {
    if (g_scmOk < 0) {
        static const unsigned char ask[] = { 0xE1, 0x00, 0x04, 0x00, 0x04, 0x07 };
        g_scmOk = !memcmp((const void*)(MISSION_AREA + 0x17CE), ask, sizeof ask);
        Log(g_scmOk ? "Pizza: Pizza Boy is the game's own; mouse throw and aim on"
                    : "Pizza: Pizza Boy isn't the game's own main.scm; its throw is left as it is");
    }
    return g_scmOk != 0;
}

bool PizzaScriptCommand(unsigned char* script) {
    if (!g_cfg.pizzaMouseThrow || !IsPizzaScript(script) || !ScmOk()) return false;
    DWORD& ip = *(DWORD*)(script + 0x10);
    const unsigned char* p = (const unsigned char*)(SCRIPT_SPACE + ip);
    const unsigned raw = p[0] | p[1] << 8, op = raw & 0x7FFF;
    // 038C SET_OBJECT_VELOCITY $8010 $8040 $8041 5.0: aim it, then let it run
    if (op == 0x038C && p[2] == 2 && *(const unsigned short*)(p + 3) == V_BOX * 4) {
        AimAtCustomer();
        if (g_request) { g_request = false; g_ourThrow = true; }
        return false;
    }
    // 00E1 IS_BUTTON_PRESSED 0 (5 | 7 | 17), as int8s
    if (op != 0x00E1 || p[2] != 4 || p[3] != 0 || p[4] != 4) return false;
    const int button = p[5];
    if (button != 5 && button != 7 && button != 17) return false;
    g_scriptSeen = GetTickCount();
    if (!g_request) return false;   // the game's own keys
    const unsigned char* bike = Bike();
    if (!bike) return false;
    const int side = CustomerSide(bike);
    if (!side) {   // nobody to throw to: no pizza wasted
        g_request = false;
        if (g_logs < 10) { g_logs++; Log("Pizza: no customer with an open order within %.0f m; not thrown", kReach); }
        return false;
    }
    const bool pressed = button == 17 || button == side;
    ip += 6;
    script[0x82] = (raw & 0x8000) ? 1 : 0;   // the command's NOT flag, as UpdateCompareFlag reads it
    ((void (__thiscall*)(void*, int))UPDATE_COMPARE)(script, pressed ? 1 : 0);
    return true;
}

void PizzaAfterPresent() {
    const bool lmb = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    const bool rmb = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
    const DWORD now = GetTickCount();
    const HWND w = *(const HWND*)GAME_WINDOW;
    const bool active = g_cfg.pizzaMouseThrow && now - g_scriptSeen < 1000 && *(const int*)GAME_STATE == 9 &&
                        !*(const unsigned char*)MENU_ACTIVE && w && GetForegroundWindow() == w && Bike();
    if (!active) {
        g_request = g_ourThrow = false;
        g_lmbWas = lmb;
        return;
    }
    if (lmb && !g_lmbWas && rmb) { g_request = true; g_requestAt = now; }
    if (g_request && now - g_requestAt > 1000) g_request = false;   // the script didn't throw (no box left?)
    if (!lmb && g_ourThrow) { Var(V_THROWN) = 0; g_ourThrow = false; }   // fire let go: ready for the next
    g_lmbWas = lmb;
}

void InstallPizza() {
    Log("Pizza: Pizza Boy throws with the mouse (hold right button, left button throws), at the customer (%s)",
        g_cfg.pizzaMouseThrow ? "on" : "off");
}
