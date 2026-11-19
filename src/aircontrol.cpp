// Air control: steer a car's angle while it is in the air, as a helicopter is
// steered. The game's own helicopter inputs are used, so whatever keys or stick
// the controls menu gives the helicopter work here too:
//   CPad::GetCarGunUpDown (0x4AAF00)     - pitch, when pressed (Num8 / Num5, right stick)
//   CPad::GetSteeringUpDown (0x4AAF50)   - pitch otherwise: forward / back: nose down / nose up
//   CPad::GetCarGunLeftRight (0x4AAEB0)  - roll, when pressed (Num4 / Num6, right stick)
//   CPad::GetSteeringLeftRight (0x4AAFD0) - roll otherwise: left / right: roll left / roll right
// Both return -128..128 and 0 while the player has no control (cutscenes).
//
// "In the air": a car (vehicle type 0, +0x29C) with none of its wheels touching
// (CAutomobile's count of wheels on the ground, +0x5C4, set by ProcessControl at
// 0x595C44) for 0.15 s, not sitting on its roof or side (ground under it within
// 1.3 m while hardly moving up or down), and not in water. Helicopters (handling
// flag 0x20000), the RC Baron (model 194) and cars flying with the cheat already
// fly with these inputs and are left alone; so are bikes and boats.
//
// The input turns the car around its own right axis (pitch) and forward axis
// (roll) by changing CPhysical::m_vecTurnSpeed (+0x7C, radians per 1/50 s, world
// space) towards a target rate, a fixed amount per time step (CTimer::ms_fTimeStep,
// 1 = 1/50 s), so it acts the same at any tick rate. With no input the car keeps
// its own spin.
//
// Heavier helicopters ([Vehicles] HeliWeight): the game's helicopters turn and
// change speed almost instantly. Before each update, the change the last update
// made to the player's helicopter's turn speed (all axes) and horizontal speed is
// cut to a fraction, as if it had more mass and turn mass: it leans, turns and
// speeds up more slowly and carries its momentum, while its top speed (thrust
// against drag, both cut alike) and its hover (vertical speed is untouched) stay
// the game's. An update in which it hit something (m_nCollisionRecords +0xE6 or
// m_fDamageImpulse +0x104, cleared by ProcessControl at 0x594F78) or stood on
// the ground keeps the game's result, so collisions and landings are unchanged.
//
// Helicopter controls ([Vehicles] HeliControls). The game's helicopters (flight
// model 6) run the helicopter part of CVehicle::FlyingControl (0x5B69A5, ebx =
// the vehicle), which reads its inputs into three locals: pitch [esp+24h] (steer
// up / down, or special up / down when pressed, Num8 / Num5), roll [esp+28h] and
// yaw [esp+2Ch]. With the game's default scheme (0xA10B0D clear) steer left /
// right (A / D) rolls it and look left / right or turret left / right (Num4 /
// Num6) turns it. Both schemes meet at 0x5B703F, where a jump to a stub, for
// the player's helicopter only, puts turret left / right into the roll and
// steer left / right (or look left / right) into the yaw, and scales the pitch:
// pitch and roll input that tilts further fades out as the nose or side nears
// the tilt limit ([Vehicles] HeliMaxPitch / HeliMaxRoll, degrees) - as in GTA V,
// pushing forward dips the nose to gain speed rather than diving - and past the
// limit a damped push in its turn speed brings it back.
//
#include "aircontrol.h"
#include "config.h"
#include <windows.h>
#include <math.h>
#include <string.h>

static const DWORD PAD0 = 0x7DBCB0, PAD_DISABLE = 0xF0;
static const DWORD ENT_RIGHT = 0x04, ENT_FORWARD = 0x14, ENT_POS = 0x34, ENT_MODEL = 0x5C, ENT_STATUS = 0x50;
static const DWORD PHYS_MOVE = 0x70, PHYS_TURN = 0x7C;
static const DWORD VEH_HANDLING = 0x120, HANDLING_FLAGS = 0xCC, VEH_TYPE = 0x29C, CAR_WHEELS_ON_GROUND = 0x5C4;
static const DWORD HANDLING_HELI = 0x20000, MODEL_RCBARON = 194;
static const DWORD CHEAT_FLYING_CARS = 0xA10B28, CHEAT_FLYING_A = 0xA10AE5;
static const DWORD CUTSCENE_RUNNING = 0xA10AB2, MENU_ACTIVE = 0x869668, USER_PAUSE = 0xA10B36;
static float* const TimeStep = (float*)0x975424;   // CTimer::ms_fTimeStep

typedef unsigned char* (__cdecl* Find_t)();
static const Find_t FindPlayerVehicle = (Find_t)0x4BC1E0;
static const auto SteerUpDown    = (short (__thiscall*)(void*))0x4AAF50;
static const auto SteerLeftRight = (short (__thiscall*)(void*))0x4AAFD0;
static const auto CarGunUpDown   = (short (__thiscall*)(void*))0x4AAF00;
static const auto CarGunLeftRight = (short (__thiscall*)(void*))0x4AAEB0;
static const auto LookRight      = (bool (__thiscall*)(void*))0x4AAC60;   // yaw +1 in the game's helicopter code
static const auto LookLeft       = (bool (__thiscall*)(void*))0x4AAC90;   // yaw -1
static const auto WaterLevel     = (bool (__cdecl*)(float, float, float, float*, bool))0x5C2C80;
static const auto ProcessVerticalLine = (bool (__cdecl*)(const float* point, float zStop, void* colPoint,
    unsigned char** entity, bool b, bool v, bool p, bool o, bool d, bool ignoreSeeThrough, void* poly))0x4D8B00;

// Rates in radians per 1/50 s for a 1500 kg car: up to 1.2 rad/s (about 70
// degrees a second), built up over about 0.6 s, so the car swings into the turn
// rather than snapping; both scaled by [Vehicles] AirControlStrength. Heavier
// vehicles turn slower (by the square root of the mass ratio: a 3500 kg truck at
// about two thirds), light ones a little faster.
static const float kMaxRate = 0.024f, kAccel = 0.0008f, kRefMass = 1500.0f;
static const DWORD PHYS_MASS = 0xB8;   // CPhysical::m_fMass

static bool g_on = false, g_lineOk = false, g_fromTick = false;
static unsigned char* g_veh = nullptr;   // the car last seen
static float g_airTime = 0.0f;           // seconds with no wheel on the ground
static bool g_heliOn = false;
static struct { unsigned char* veh; float turn[3], move[2]; } g_heli;   // what the last update was left with
static const DWORD PHYS_COLLISIONS = 0xE6, PHYS_DAMAGE_IMPULSE = 0x104;
static const short MODEL_RCRAIDER = 195, MODEL_RCGOBLIN = 231;
extern "C" {
    unsigned char* volatile g_heliCtlVeh = nullptr;   // the helicopter the stubs act for
    volatile float g_heliRoll = 0.0f;                // its roll input ([esp+28h]: < 0 rolls right, as steer right did)
    volatile float g_heliYaw = 0.0f;                 // its yaw input ([esp+2Ch]: > 0 turns right)
    volatile float g_heliPitchScale = 1.0f;          // its pitch input is multiplied by this
}
static bool g_heliCtlOn = false;
// Planes ([Vehicles] PlaneControls): the plane part of FlyingControl (flight models
// 1, 3, 4, 5: the RC Baron, the Skimmer; 0x5B5A78) reads steer left / right into one
// local, [esp+10h], and uses it both to turn (0x5B60C5, x flying handling +0x0C,
// fYaw) and to roll (0x5B61BD / 0x5B61CF, x +0x18, fRoll). For the player's plane,
// two stubs put steer left / right into it before the turn and turret left / right
// before the roll, as the helicopters have them. Pitch is read as the helicopter's already.
extern "C" {
    unsigned char* volatile g_planeVeh = nullptr;
    volatile float g_planeRoll = 0.0f;   // > 0 rolls right, as steer right did
    volatile float g_planeYaw = 0.0f;    // > 0 turns right
}
static bool g_planeOn = false;
static const DWORD REMOTE_VEHICLE = 0x94AD28 + 4;   // CPlayerInfo +4: the RC vehicle

static void PlaneControls() {
    unsigned char* veh = *(unsigned char**)REMOTE_VEHICLE;
    if (!veh) veh = FindPlayerVehicle();
    if (!veh) return;
    g_planeRoll = CarGunLeftRight((void*)PAD0) / 128.0f;
    g_planeYaw = SteerLeftRight((void*)PAD0) / 128.0f;
    g_planeVeh = veh;
}

static float Dot(const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

// Move rate towards target by at most step.
static float Approach(float rate, float target, float step) {
    if (rate < target) return rate + step < target ? rate + step : target;
    return rate - step > target ? rate - step : target;
}

static bool Grounded(unsigned char* veh) {
    const float* p = (const float*)(veh + ENT_POS);
    float level;
    if (WaterLevel(p[0], p[1], p[2], &level, true) && p[2] < level + 2.0f) return true;   // in or on water
    if (!g_lineOk) return false;
    if (fabsf(((const float*)(veh + PHYS_MOVE))[2]) > 0.02f) return false;   // rising or falling (1 m/s)
    alignas(16) unsigned char colPoint[64];
    unsigned char* hit = nullptr;
    // buildings and objects, not vehicles (the car itself)
    if (!ProcessVerticalLine(p, p[2] - 1.3f, colPoint, &hit, true, false, false, true, false, true, nullptr)) return false;
    return true;   // resting on its roof or side
}

static void HeliWeight(unsigned char* veh) {
    float* turn = (float*)(veh + PHYS_TURN);
    float* move = (float*)(veh + PHYS_MOVE);
    const bool fresh = veh != g_heli.veh;
    g_heli.veh = veh;
    const bool hit = *(veh + PHYS_COLLISIONS) != 0 || *(const float*)(veh + PHYS_DAMAGE_IMPULSE) > 0.0f;
    if (!fresh && !hit && *(veh + CAR_WHEELS_ON_GROUND) == 0) {
        const float w = g_cfg.heliWeight / 100.0f;
        const float kTurn = 1.0f / (1.0f + 1.2f * w), kMove = 1.0f / (1.0f + 0.7f * w);   // 100%: turn mass x2.2, mass x1.7
        for (int i = 0; i < 3; i++) turn[i] = g_heli.turn[i] + (turn[i] - g_heli.turn[i]) * kTurn;
        for (int i = 0; i < 2; i++) move[i] = g_heli.move[i] + (move[i] - g_heli.move[i]) * kMove;
    }
    for (int i = 0; i < 3; i++) g_heli.turn[i] = turn[i];
    for (int i = 0; i < 2; i++) g_heli.move[i] = move[i];
}

// 1 within 70% of the limit, fading to 0 at it, for input that tilts further.
static float Fade(float angle, float limit) {
    if (limit <= 0.0f) return 1.0f;
    const float t = (angle - 0.7f * limit) / (0.3f * limit);
    return t <= 0.0f ? 1.0f : t >= 1.0f ? 0.0f : 1.0f - t;
}

static void HeliControls(unsigned char* veh, float ts) {
    const float* right = (const float*)(veh + ENT_RIGHT);
    const float* fwd = (const float*)(veh + ENT_FORWARD);
    const float kDeg = 57.29578f;
    const float pitch = asinf(fwd[2] < -1.0f ? -1.0f : fwd[2] > 1.0f ? 1.0f : fwd[2]) * kDeg;      // > 0: nose up
    const float roll = -asinf(right[2] < -1.0f ? -1.0f : right[2] > 1.0f ? 1.0f : right[2]) * kDeg;  // > 0: right side down
    const float maxPitch = (float)g_cfg.heliMaxPitch, maxRoll = (float)g_cfg.heliMaxRoll;

    // pitch input as the helicopter reads it (< 0: nose down)
    const short gun = CarGunUpDown((void*)PAD0);
    const float pitchIn = (gun > 1 || gun < -1 ? -gun : SteerUpDown((void*)PAD0)) / 128.0f;
    g_heliPitchScale = pitchIn < 0.0f ? Fade(-pitch, maxPitch) : pitchIn > 0.0f ? Fade(pitch, maxPitch) : 1.0f;
    const float rollIn = CarGunLeftRight((void*)PAD0) / 128.0f;   // > 0: roll right (Num6)
    g_heliRoll = -rollIn * (rollIn > 0.0f ? Fade(roll, maxRoll) : rollIn < 0.0f ? Fade(-roll, maxRoll) : 1.0f);
    const short steer = SteerLeftRight((void*)PAD0);              // > 0: right (D)
    g_heliYaw = steer ? steer / 128.0f : LookRight((void*)PAD0) ? 1.0f : LookLeft((void*)PAD0) ? -1.0f : 0.0f;
    g_heliCtlVeh = veh;

    // past a limit (momentum, a gust of input): turn back towards it, damped;
    // only in the air (on the ground it rests as the ground holds it)
    if (*(veh + CAR_WHEELS_ON_GROUND) != 0) return;
    float* turn = (float*)(veh + PHYS_TURN);
    const float k = 0.0006f * ts, damp = 0.15f * ts;   // per degree past it; share of the outward spin removed
    if (maxPitch > 0.0f && fabsf(pitch) > maxPitch) {
        const float over = pitch > 0.0f ? pitch - maxPitch : pitch + maxPitch;   // > 0: too far nose up
        float rate = Dot(turn, right);                                          // > 0: nose rising
        float d = -over * k;
        if (rate * over > 0.0f) d -= rate * (damp > 1.0f ? 1.0f : damp);
        for (int i = 0; i < 3; i++) turn[i] += right[i] * d;
    }
    if (maxRoll > 0.0f && fabsf(roll) > maxRoll) {
        const float over = roll > 0.0f ? roll - maxRoll : roll + maxRoll;       // > 0: too far right
        float rate = Dot(turn, fwd);                                            // > 0: rolling right
        float d = -over * k;
        if (rate * over > 0.0f) d -= rate * (damp > 1.0f ? 1.0f : damp);
        for (int i = 0; i < 3; i++) turn[i] += fwd[i] * d;
    }
}

// What happened to the helicopter keys, in the log: 2 s after getting into a
// helicopter, whether our keys were on for it and how many of those updates the
// game's flying code actually took them (the stub counts each), and whether our
// jump at 0x5B703F is still there (another mod could have written over it).
extern "C" volatile long g_heliStubHits = 0;
static unsigned char* g_heliCode = nullptr;   // the stub
static unsigned char* g_diagVeh = nullptr;
static int g_diagTicks = 0, g_diagLogs = 0;
static void HeliDiag(unsigned char* veh, unsigned status, bool real, bool ctl) {
    if (veh != g_diagVeh) { g_diagVeh = veh; g_diagTicks = 0; g_heliStubHits = 0; }
    if (++g_diagTicks != 60 || g_diagLogs >= 20) return;
    g_diagLogs++;
    const unsigned char* site = (const unsigned char*)0x5B703F;
    const bool hooked = g_heliCode && site[0] == 0xE9 && (DWORD)(0x5B7044 + *(const int*)(site + 1)) == (DWORD)g_heliCode;
    Log("AirControl: helicopter model %d status %u: our keys %s; the flying code took them in %ld of 60 updates; jump at 5B703F %s (%02X %02X %02X %02X %02X)",
        *(short*)(veh + ENT_MODEL), status,
        !ctl ? "off (setting, or not installed)" : !real ? "off (not the player's own helicopter)" : "on",
        g_heliStubHits, hooked ? "in place" : "MISSING or replaced", site[0], site[1], site[2], site[3], site[4]);
}

static void Run() {
    g_heliCtlVeh = nullptr;   // the game's own inputs unless set below
    g_planeVeh = nullptr;
    // installed parts, as the settings have them now (the Modern Fix menu changes them while playing)
    const bool air = g_on && g_cfg.airControl, heavy = g_heliOn && g_cfg.heliWeight > 0;
    const bool heliCtl = g_heliCtlOn && g_cfg.heliControls, plane = g_planeOn && g_cfg.planeControls;
    if ((!air && !heavy && !heliCtl && !plane) || ReplayPlaying() || *(volatile unsigned char*)MENU_ACTIVE || *(volatile unsigned char*)USER_PAUSE ||
        *(volatile unsigned char*)CUTSCENE_RUNNING || *(unsigned short*)(PAD0 + PAD_DISABLE)) {
        g_airTime = 0.0f; g_heli.veh = nullptr; return;
    }
    if (plane) PlaneControls();   // whichever the player flies; only the plane code reads it
    unsigned char* veh = FindPlayerVehicle();
    if (veh != g_veh) { g_veh = veh; g_airTime = 0.0f; }
    const float ts = *TimeStep;
    if (!veh || *(int*)(veh + VEH_TYPE) != 0 || !(ts > 0.0f) || ts > 10.0f) { g_heli.veh = nullptr; return; }   // cars only
    const unsigned status = *(veh + ENT_STATUS) >> 3;
    const unsigned char* handling = *(unsigned char**)(veh + VEH_HANDLING);
    const bool isHeli = handling && (*(const DWORD*)(handling + HANDLING_FLAGS) & HANDLING_HELI);
    if (status != 0 && status != 3) {   // the player's car or RC car
        if (isHeli) HeliDiag(veh, status, false, heliCtl);
        g_heli.veh = nullptr; return;
    }
    if (!handling) return;
    if (isHeli) {   // a helicopter
        const short model = *(short*)(veh + ENT_MODEL);
        const bool real = status == 0 && model != MODEL_RCRAIDER && model != MODEL_RCGOBLIN;
        if (heavy && real) HeliWeight(veh);
        else g_heli.veh = nullptr;
        if (heliCtl && real) HeliControls(veh, ts);
        HeliDiag(veh, status, real, heliCtl);
        return;
    }
    g_heli.veh = nullptr;
    if (!air || *(short*)(veh + ENT_MODEL) == (short)MODEL_RCBARON ||
        *(volatile unsigned char*)CHEAT_FLYING_CARS || *(volatile unsigned char*)CHEAT_FLYING_A) return;   // flies already

    if (*(veh + CAR_WHEELS_ON_GROUND) != 0) { g_airTime = 0.0f; return; }
    g_airTime += ts / 50.0f;
    if (g_airTime < 0.15f || Grounded(veh)) return;

    // Pitch as the helicopter reads it (FlyingControl, 0x5B5AB0..0x5B5B41): the
    // "special control" up / down (GetCarGunUpDown: right stick, or the keys the
    // controls menu gives it, Num8 / Num5 by default) when pressed, negated;
    // otherwise steer up / down.
    const short gun = CarGunUpDown((void*)PAD0);
    const float pitchIn = (gun > 1 || gun < -1 ? -gun : SteerUpDown((void*)PAD0)) / 128.0f;   // < 0: nose down
    // Roll: the turret left / right control (GetCarGunLeftRight: right stick, or
    // its keys, Num4 / Num6 by default) when pressed, otherwise steer left / right.
    const short gunLR = CarGunLeftRight((void*)PAD0);
    const float rollIn = (gunLR > 1 || gunLR < -1 ? gunLR : SteerLeftRight((void*)PAD0)) / 128.0f;   // > 0: roll right
    if (fabsf(pitchIn) < 0.1f && fabsf(rollIn) < 0.1f) return;

    const float mass = *(const float*)(veh + PHYS_MASS);
    float weight = mass > 1.0f ? sqrtf(kRefMass / mass) : 1.0f;
    weight = weight < 0.5f ? 0.5f : weight > 1.25f ? 1.25f : weight;
    const float scale = g_cfg.airControlStrength / 100.0f * weight;
    const float* right = (const float*)(veh + ENT_RIGHT);
    const float* fwd = (const float*)(veh + ENT_FORWARD);
    float* turn = (float*)(veh + PHYS_TURN);
    const float step = kAccel * scale * ts;
    // Turning about the right axis lifts the nose for a positive rate; about the
    // forward axis, a positive rate lowers the right side.
    float dPitch = 0.0f, dRoll = 0.0f;
    if (fabsf(pitchIn) >= 0.1f) {
        const float rate = Dot(turn, right);
        dPitch = Approach(rate, pitchIn * kMaxRate * scale, step) - rate;
    }
    if (fabsf(rollIn) >= 0.1f) {
        const float rate = Dot(turn, fwd);
        dRoll = Approach(rate, rollIn * kMaxRate * scale, step) - rate;
    }
    for (int i = 0; i < 3; i++) turn[i] += right[i] * dPitch + fwd[i] * dRoll;
}

void AirControlBeforeTick() { g_fromTick = true; Run(); }
void AirControlAfterPresent() { if (!g_fromTick) Run(); }   // FixedTick off: once per frame

static void WriteCode(DWORD addr, const void* bytes, int len) {
    DWORD old;
    VirtualProtect((void*)addr, len, PAGE_EXECUTE_READWRITE, &old);
    memcpy((void*)addr, bytes, len);
    VirtualProtect((void*)addr, len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)addr, len);
}

static void Rel(unsigned char* at, DWORD target) { *(int*)at = (int)(target - ((DWORD)at + 4)); }

// Jump from `site` (len bytes, the rest nops) to a stub.
static void JumpTo(DWORD site, int len, const unsigned char* stub) {
    unsigned char p[16] = { 0xE9 };
    *(int*)(p + 1) = (int)((DWORD)stub - (site + 5));   // relative to where the jump will be, not this buffer
    for (int i = 5; i < len; i++) p[i] = 0x90;
    WriteCode(site, p, len);
}

static bool InstallHeliControls() {
    // 0x5B703F: push 0; call CPad::GetPad (0x4AB060); pop ecx - where both input schemes meet
    static const unsigned char site[] = { 0x6A, 0x00, 0xE8 };
    // 0x5B6F2C: call GetCarGunLeftRight (yaw in the default scheme); 0x5B6E74: GetSteeringLeftRight (roll)
    if (memcmp((void*)0x5B703F, site, sizeof site) || 0x5B703F + 7 + *(const int*)(0x5B703F + 3) != 0x4AB060 ||
        0x5B6F2C + 5 + *(const int*)(0x5B6F2D) != 0x4AAEB0 || 0x5B6E74 + 5 + *(const int*)(0x5B6E75) != 0x4AAFD0)
        return false;
    unsigned char* code = (unsigned char*)VirtualAlloc(nullptr, 128, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!code) return false;
    unsigned char stub[] = {
        0x3B, 0x1D, 0, 0, 0, 0,                   // cmp ebx, [g_heliCtlVeh]
        0x75, 0x26,                               // jne game
        0xFF, 0x05, 0, 0, 0, 0,                   // inc dword [g_heliStubHits]
        0xD9, 0x44, 0x24, 0x24,                   // fld dword [esp+24h]     pitch
        0xD8, 0x0D, 0, 0, 0, 0,                   // fmul dword [g_heliPitchScale]
        0xD9, 0x5C, 0x24, 0x24,                   // fstp dword [esp+24h]
        0xA1, 0, 0, 0, 0,                         // mov eax, [g_heliRoll]
        0x89, 0x44, 0x24, 0x28,                   // mov [esp+28h], eax      roll
        0xA1, 0, 0, 0, 0,                         // mov eax, [g_heliYaw]
        0x89, 0x44, 0x24, 0x2C,                   // mov [esp+2Ch], eax      yaw
        0x6A, 0x00,                               // game: push 0
        0xB8, 0x60, 0xB0, 0x4A, 0x00,             // mov eax, 0x4AB060
        0xFF, 0xD0,                               // call eax                CPad::GetPad
        0xE9, 0, 0, 0, 0,                         // jmp 0x5B7046 (pop ecx)
    };
    *(DWORD*)(stub + 2) = (DWORD)&g_heliCtlVeh;
    *(DWORD*)(stub + 10) = (DWORD)&g_heliStubHits;
    *(DWORD*)(stub + 20) = (DWORD)&g_heliPitchScale;
    *(DWORD*)(stub + 29) = (DWORD)&g_heliRoll;
    *(DWORD*)(stub + 38) = (DWORD)&g_heliYaw;
    memcpy(code, stub, sizeof stub);
    g_heliCode = code;
    Rel(code + sizeof stub - 4, 0x5B7046);
    JumpTo(0x5B703F, 7, code);
    return true;
}

static bool InstallPlaneControls() {
    // 0x5B60C2: fld [edx+0Ch]; fmul [esp+10h]                     (yaw: fYaw)
    static const unsigned char rollSite[] = { 0xD9, 0x42, 0x0C, 0xD8, 0x4C, 0x24, 0x10 };
    // 0x5B613A: lea eax,[ebx+4]; cmp dword [esp+1FCh],1           (before the roll: fRoll)
    static const unsigned char yawSite[] = { 0x8D, 0x43, 0x04, 0x83, 0xBC, 0x24, 0xFC, 0x01, 0x00, 0x00, 0x01 };
    if (memcmp((void*)0x5B60C2, rollSite, sizeof rollSite) || memcmp((void*)0x5B613A, yawSite, sizeof yawSite)) return false;
    unsigned char* code = (unsigned char*)VirtualAlloc(nullptr, 128, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!code) return false;
    unsigned char roll[] = {
        0x3B, 0x1D, 0, 0, 0, 0,                   // cmp ebx, [g_planeVeh]
        0x75, 0x09,                               // jne game
        0xA1, 0, 0, 0, 0,                         // mov eax, [g_planeYaw]
        0x89, 0x44, 0x24, 0x10,                   // mov [esp+10h], eax
        0xD9, 0x42, 0x0C,                         // game: fld [edx+0Ch]
        0xD8, 0x4C, 0x24, 0x10,                   // fmul [esp+10h]
        0xE9, 0, 0, 0, 0,                         // jmp 0x5B60C9
    };
    *(DWORD*)(roll + 2) = (DWORD)&g_planeVeh;
    *(DWORD*)(roll + 9) = (DWORD)&g_planeYaw;
    unsigned char yaw[] = {
        0x3B, 0x1D, 0, 0, 0, 0,                   // cmp ebx, [g_planeVeh]
        0x75, 0x09,                               // jne game
        0xA1, 0, 0, 0, 0,                         // mov eax, [g_planeRoll]
        0x89, 0x44, 0x24, 0x10,                   // mov [esp+10h], eax
        0x8D, 0x43, 0x04,                         // game: lea eax, [ebx+4]
        0x83, 0xBC, 0x24, 0xFC, 0x01, 0x00, 0x00, 0x01,   // cmp dword [esp+1FCh], 1
        0xE9, 0, 0, 0, 0,                         // jmp 0x5B6145 (flags kept for the jne)
    };
    *(DWORD*)(yaw + 2) = (DWORD)&g_planeVeh;
    *(DWORD*)(yaw + 9) = (DWORD)&g_planeRoll;
    memcpy(code, roll, sizeof roll);
    Rel(code + sizeof roll - 4, 0x5B60C9);
    unsigned char* y = code + 64;
    memcpy(y, yaw, sizeof yaw);
    Rel(y + sizeof yaw - 4, 0x5B6145);
    JumpTo(0x5B60C2, 7, code);
    JumpTo(0x5B613A, 11, y);
    return true;
}

void InstallAirControl() {
    // Every part is installed even when its setting is off: the Modern Fix menu
    // (fixmenu.cpp) turns them on and off while playing (Run reads the settings).
    // GetSteeringUpDown / GetSteeringLeftRight: cmp word [ecx+0F0h],0 (controls disabled); push ebx
    static const unsigned char upDown[] = { 0x66, 0x83, 0xB9, 0xF0, 0x00, 0x00, 0x00, 0x00, 0x53, 0x74, 0x05 };
    static const unsigned char leftRight[] = { 0x66, 0x83, 0xB9, 0xF0, 0x00, 0x00, 0x00, 0x00, 0x53, 0x55, 0x74, 0x05 };
    // ProcessControl: m_nWheelsOnGround = 0 (mov byte [ebp+5C4h],0)
    static const unsigned char wheels[] = { 0xC6, 0x85, 0xC4, 0x05, 0x00, 0x00, 0x00 };
    // GetCarGunUpDown: cmp word [ecx+0F0h],0; je; xor eax,eax; ret
    static const unsigned char gunUpDown[] = { 0x66, 0x83, 0xB9, 0xF0, 0x00, 0x00, 0x00, 0x00, 0x74, 0x06, 0x31, 0xC0, 0xC3 };
    if (memcmp((void*)0x4AAF50, upDown, sizeof upDown) || memcmp((void*)0x4AAFD0, leftRight, sizeof leftRight) ||
        memcmp((void*)0x4AAF00, gunUpDown, sizeof gunUpDown) || memcmp((void*)0x4AAEB0, gunUpDown, sizeof gunUpDown) ||
        memcmp((void*)0x595C44, wheels, sizeof wheels)) {
        Log("AirControl: game code is not v1.0; off");
        return;
    }
    static const unsigned char lineCode[] = { 0x53, 0x56, 0x55, 0x83, 0xEC, 0x40, 0x66, 0x83 };
    g_lineOk = !memcmp((void*)0x4D8B00, lineCode, sizeof lineCode);
    // ProcessControl clears the collision count (mov byte [ebp+0E6h],0)
    static const unsigned char collisions[] = { 0xC6, 0x85, 0xE6, 0x00, 0x00, 0x00, 0x00 };
    g_on = true;
    g_heliOn = !memcmp((void*)0x594F78, collisions, sizeof collisions);
    Log("AirControl: cars in the air turn with the helicopter controls (strength %d%%) (%s)", g_cfg.airControlStrength,
        g_cfg.airControl ? "on" : "off");
    if (g_heliOn) Log("AirControl: helicopters are heavier (weight %d%%)", g_cfg.heliWeight);
    g_heliCtlOn = InstallHeliControls();
    if (g_heliCtlOn) Log("AirControl: helicopter steer left / right turns it, turret left / right rolls it; tilt limits %d / %d degrees (%s)",
                         g_cfg.heliMaxPitch, g_cfg.heliMaxRoll, g_cfg.heliControls ? "on" : "off");
    else Log("AirControl: helicopter flying code is not v1.0 or was changed by another mod; its controls are the game's");
    g_planeOn = InstallPlaneControls();
    Log(g_planeOn ? "AirControl: planes steered like the helicopters (steer left / right turns, turret left / right rolls) (%s)"
                  : "AirControl: plane flying code is not v1.0 or was changed by another mod; planes keep the game's controls (%s)",
        g_cfg.planeControls ? "on" : "off");
}
