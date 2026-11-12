// Small gameplay fixes and features run at the start of each game update.
//
// Mouse aim ([Vehicles] MouseAim). The Rhino's turret (CAutomobile::TankControl,
// 0x586575...) and the fire truck's water cannon (FireTruckControl, 0x57AB60...)
// turn with the "car gun" keys (numpad) by changing two angles in the vehicle:
// m_fCarGunLR (+0x5B0) and m_fCarGunUD (+0x5B4, cannon only). Before each update
// they are turned toward where the camera looks (TheCamera's forward, 0x7E4698),
// which FreeCam moves with the mouse: aim with the mouse, fire as usual. The turn is
// rate-limited like a real turret. In the vehicle's own space the tank turret points
// at (-sin LR, cos LR), the cannon at (sin LR cos UD, cos LR cos UD, sin UD); the
// game keeps the cannon's UD between 0.05 and 0.3.
//
// Mission leftovers ([Fixes] MissionLeftovers). "Martha's Mug Shot" (PORNO3) swaps
// the Ocean Drive hotel od_chariot (3700) for od_wtsign (3568), and its LOD
// LODchariot (3733) for LODd_buildnew (3661), while you are on the roof, and swaps
// them back when you come down or the mission ends. Should the mission end without
// that, the swap stays in CTheScripts::BuildingSwapArray (0x94DBE8, 25 x {building,
// new model, old model}) and every save keeps it: the hotel shows without walls.
// Off a mission such an entry is undone the way the game's own UndoBuildingSwaps
// (0x458CC0) does it: CBuilding::ReplaceWithNewModel (0x407DB0) with the old model,
// and the entry cleared. The mission's "hotel swapped" flag ($4487) is reset too.
#include "gameplay.h"
#include "config.h"
#include <windows.h>
#include <math.h>
#include <string.h>

typedef unsigned char* (__cdecl* Find_t)();
static const Find_t FindPlayerVehicle = (Find_t)0x4BC1E0;
static const auto OnMission = (bool (__cdecl*)())0x44FE30;
static const DWORD CAMERA_FORWARD = 0x7E4698, TIMESTEP = 0x975424;
static const DWORD PAD0_DISABLE = 0x7DBCB0 + 0xF0;
static const DWORD WB_STATE = 0x94ADF4;    // CWorld::Players[0].m_WBState, 0 = playing
static const DWORD MENU_ACTIVE = 0x869668, GAME_STATE = 0x9B5F08;
static const DWORD ENT_MODEL = 0x5C, GUN_LR = 0x5B0, GUN_UD = 0x5B4;
static const short MODEL_FIRETRUCK = 137, MODEL_RHINO = 162;
static const float PI = 3.14159265f;

static bool g_aimOk = false, g_leftoversOk = false;

static float Wrap(float a) {
    while (a > PI) a -= 2 * PI;
    while (a < -PI) a += 2 * PI;
    return a;
}

// Turns `cur` toward `target` by at most `step` (angles, shortest way).
static float TurnToward(float cur, float target, float step) {
    const float d = Wrap(target - cur);
    return fabsf(d) <= step ? cur + d : cur + (d > 0 ? step : -step);
}

bool MouseAimVehicle(const unsigned char* veh) {
    if (!g_aimOk || !veh) return false;
    const short model = *(const short*)(veh + ENT_MODEL);
    return model == MODEL_RHINO || model == MODEL_FIRETRUCK;
}

static void MouseAim() {
    unsigned char* veh = FindPlayerVehicle();
    if (!MouseAimVehicle(veh) || *(unsigned short*)PAD0_DISABLE || *(unsigned char*)WB_STATE) return;
    const float* r = (const float*)(veh + 0x04);
    const float* f = (const float*)(veh + 0x14);
    const float* u = (const float*)(veh + 0x24);
    const float* c = (const float*)CAMERA_FORWARD;
    const float lx = c[0] * r[0] + c[1] * r[1] + c[2] * r[2];
    const float ly = c[0] * f[0] + c[1] * f[1] + c[2] * f[2];
    const float lz = c[0] * u[0] + c[1] * u[1] + c[2] * u[2];
    if (lx * lx + ly * ly < 0.0001f) return;   // looking straight up or down
    const float dt = *(float*)TIMESTEP / 50.0f;   // seconds this update
    float* lr = (float*)(veh + GUN_LR);
    if (*(short*)(veh + ENT_MODEL) == MODEL_RHINO) {
        float a = TurnToward(*lr, atan2f(-lx, ly), 2.5f * dt);
        if (a < 0) a += 2 * PI;
        if (a > 2 * PI) a -= 2 * PI;
        *lr = a;
    } else {
        *lr = Wrap(TurnToward(*lr, atan2f(lx, ly), 5.0f * dt));
        // The camera behind the truck looks about 0.12 rad down: that is the middle
        // of the cannon's range; looking up raises it.
        const float pitch = asinf(lz < -1 ? -1 : lz > 1 ? 1 : lz);
        float ud = 0.175f + (pitch + 0.12f);
        ud = ud < 0.05f ? 0.05f : ud > 0.3f ? 0.3f : ud;
        float* cur = (float*)(veh + GUN_UD);
        *cur = TurnToward(*cur, ud, 1.0f * dt);
    }
}

// ---------------------------------------------------------------- mission leftovers
struct BuildingSwap { void* building; int newModel, oldModel; };
static BuildingSwap* const SwapArray = (BuildingSwap*)0x94DBE8;
static const DWORD REPLACE_MODEL = 0x407DB0;
static int* const HotelSwappedFlag = (int*)(0x821280 + 4487 * 4);   // $4487 in main.scm
static const struct { int newModel, oldModel; } kLeftovers[] = { { 3568, 3700 }, { 3661, 3733 } };

static void MissionLeftovers() {
    if (OnMission()) return;
    for (int i = 0; i < 25; i++) {
        BuildingSwap& s = SwapArray[i];
        if (!s.building) continue;
        for (const auto& k : kLeftovers) {
            if (s.newModel != k.newModel || s.oldModel != k.oldModel) continue;
            ((void (__thiscall*)(void*, int))REPLACE_MODEL)(s.building, s.oldModel);
            Log("Fixes: building swap %d -> %d left by a mission undone (Ocean Drive hotel)", k.oldModel, k.newModel);
            s.building = nullptr;
            s.newModel = s.oldModel = -1;
            *HotelSwappedFlag = 0;
            break;
        }
    }
}

// ---------------------------------------------------------------- run and gun
// Weapons ([Weapons] RunAndGun). A ped moves through its walk/run/sprint animations;
// a gun's fire animation is laid over them (ASSOC_PARTIAL). colt45_fire and TEC_fire
// hold only right-arm tracks, so pistols fire on the move. RIFLE_fire, UZI_fire,
// M60_fire, FLAME_fire, buddy_fire(RELOAD), shotgun_fire and python_fire also hold
// the root, pelvis and leg tracks (bone tags 0, 1, 41-43, 51-53): they pin the legs
// and the root, so the player stands still while firing. Those tracks are dropped
// from the standing fire animations: CAnimBlendAssociation::Init (0x401988) finds a
// track's bone by its tag (CAnimBlendSequence +0x24) and skips tracks whose bone isn't
// found, so they get a tag no bone has. The legs then follow the walk/run/sprint and
// the upper body keeps the gun's own two-handed pose. Crouch animations are left
// whole. Animations live in CAnimManager's hierarchies (0x7F3D28, 0x28 bytes each,
// count at 0x978540; tracks at +0x18, count +0x1C, 0x30 bytes each) and weapon blocks
// are loaded on demand, so they are checked each update.
//
// HEAVY (0x200 in CWeaponInfo +0x5C, 0x782A14 + type * 0x64) on the rocket launcher
// and minigun blocks sprinting and jumping: it is cleared.
static unsigned* WeaponFlags(int type) { return (unsigned*)(0x782A14 + type * 0x64 + 0x5C); }
static const int kHeavyWeapons[] = { 30, 33 };   // rocket launcher, minigun
static bool g_runGunOk = false, g_hitKeepOk = false;
static const char* const kFireAnims[] = { "RIFLE_fire", "UZI_fire", "M60_fire", "FLAME_fire", "buddy_fire",
                                          "buddy_fireRELOAD", "shotgun_fire", "python_fire" };
static const short kNoBone = 0x7FF0;

static bool IsLegTrack(short tag) {
    return tag == 0 || tag == 1 || (tag >= 41 && tag <= 43) || (tag >= 51 && tag <= 53) || tag == 2000 || tag == 2001;
}

static void FreeLegsInFireAnims() {
    const int count = *(int*)0x978540;
    if (count <= 0 || count > 4000) return;
    char freed[160] = "";
    int nFreed = 0;
    for (int i = 0; i < count; i++) {
        unsigned char* h = (unsigned char*)(0x7F3D28 + i * 0x28);
        const char* name = (const char*)h;
        if (name[0] != 'R' && name[0] != 'U' && name[0] != 'M' && name[0] != 'F' &&
            name[0] != 'b' && name[0] != 's' && name[0] != 'p') continue;
        bool match = false;
        for (const char* n : kFireAnims) if (!_stricmp(name, n)) { match = true; break; }
        if (!match) continue;
        unsigned char* seqs = *(unsigned char**)(h + 0x18);
        const int n = *(short*)(h + 0x1C);
        if (!seqs || n <= 0 || n > 64) continue;
        int stripped = 0;
        for (int k = 0; k < n; k++) {
            short* tag = (short*)(seqs + k * 0x30 + 0x24);
            if (IsLegTrack(*tag)) { *tag = kNoBone; stripped++; }
        }
        if (stripped && nFreed++ < 8 && strlen(freed) + strlen(name) + 2 < sizeof freed) {
            if (freed[0]) strcat(freed, " ");
            strcat(freed, name);
        }
    }
    if (nFreed) Log("Weapons: legs freed in %s", freed);
}

// The bone links. Tracks are matched to bones once: when an animation block
// streams in, CStreaming (0x40C5CE) calls CAnimManager::LoadAnimFile (0x404A50) and
// then CreateAnimAssocGroups (0x405430), which builds each group's associations
// against the ped skeleton; every shot copies those. So the leg tracks are dropped
// right after LoadAnimFile returns, before the groups are built. (0x4055F9 is the
// start-up load of ped.ifp, 0x406E03 the cutscene loader: hooked too, harmless.)
static const DWORD LOAD_ANIM_FILE = 0x404A50;
static const DWORD kLoadAnimCalls[] = { 0x4055F9, 0x406E03, 0x40C5CE };
static int __cdecl LoadAnimFileHook(int stream, int compress, int names) {
    const int r = ((int (__cdecl*)(int, int, int))LOAD_ANIM_FILE)(stream, compress, names);
    FreeLegsInFireAnims();
    return r;
}

// Firing while sprinting. ProcessPlayerWeapon only fires when the fire button is
// down and m_nMoveState (+0x24C) isn't PEDMOVE_SPRINT (5): je at 0x534B18 is
// skipped. CPed::Attack ends the attack once the sprint key is held past the
// weapon's breakout point (0x52BDE0): that jump is made unconditional.
static const DWORD SPRINT_FIRE_JE = 0x534B18, SPRINT_BREAKOUT_JE = 0x52BDE0;

// The aim button. CPlayerPed::ProcessPlayerWeapon (0x5349C3) sends the rocket
// launcher, sniper, laser scope, M4, Ruger, M60 and camera into a first-person aim
// camera (PED_SNIPER_MODE, m_fMoveSpeed = 0): with the M4, Ruger or M60, aiming
// froze the player. Their three checks (je at 0x5349DB, 0x5349E0, 0x5349E5) are
// skipped, so they aim in third person like the SMGs, and you can move.
static const DWORD kScopeJumps[] = { 0x5349DB, 0x5349E0, 0x5349E5 };
static const unsigned char kScopeCode[] = { 0x83, 0xF8, 0x1A, 0x74, 0x13, 0x83, 0xF8, 0x1B, 0x74, 0x0E,
                                            0x83, 0xF8, 0x20, 0x74, 0x09 };   // at 0x5349D8

static void RunAndGun() {
    for (int t : kHeavyWeapons) *WeaponFlags(t) &= ~0x200u;
}

// Keep firing when hit ([Weapons] KeepFiringWhenHit). When a bullet hits a ped
// (CWeapon::DoBulletImpact at 0x5CF493 for the player, and 4 more hit paths),
// the game calls CPed::ClearAttackByRemovingAnim (0x52CF70) before playing the
// "shot" flinch: the player stops shooting and has to press fire again. The calls
// go through this check: the player keeps the flinch but not the cancelled attack.
static const DWORD CLEAR_ATTACK_ANIM = 0x52CF70;
static const DWORD kClearAttackCalls[] = { 0x5C4014, 0x5C9D73, 0x5CBC42, 0x5CF493, 0x5CF4FC };
static void __fastcall ClearAttackUnlessPlayer(void* ped, void*) {
    if (ped == ((void* (__cdecl*)())0x4BC120)()) return;   // FindPlayerPed
    ((void (__thiscall*)(void*))CLEAR_ATTACK_ANIM)(ped);
}

static bool g_fromTick = false;   // FixedTick calls GameplayBeforeTick

static void Run() {
    if (*(int*)GAME_STATE != 9 || *(unsigned char*)MENU_ACTIVE) return;
    if (g_leftoversOk) MissionLeftovers();
    if (g_aimOk) MouseAim();
    if (g_runGunOk) RunAndGun();
}

void GameplayBeforeTick() { g_fromTick = true; Run(); }
void GameplayAfterPresent() { if (!g_fromTick) Run(); }   // FixedTick off: once per frame

// ---------------------------------------------------------------- sector clamp
// CPed::OurPedCanSeeThisOne (0x50D360, used by scripts, e.g. at 0x445D41) looks at
// the peds in the world sectors between two peds. It turns positions into sector
// numbers (x * 0.02 + 48, y * 0.02 + 40; the grid is 80 x 80, 0x792D48, 40 bytes a
// sector) without keeping them inside the grid, unlike the game's other sector
// scans. A ped outside the map (or a bad position) makes it read far past the
// grid and crash (0x50D660). After the last of the four numbers is worked out
// (0x50D60E: fldcw [esp+28h]; mov eax,[esp+38h]) a stub clamps all four to 0-79.
extern "C" void vcmf_sector_clamp();
#define CLAMP(slot, n) \
    "  movl " slot "(%esp), %eax\n  testl %eax, %eax\n  jge 1" n "f\n  xorl %eax, %eax\n" \
    "1" n ": cmpl $79, %eax\n  jle 2" n "f\n  movl $79, %eax\n2" n ": movl %eax, " slot "(%esp)\n"
__asm__(".text\n"
        ".globl _vcmf_sector_clamp\n_vcmf_sector_clamp:\n"
        "  fldcw 0x2c(%esp)\n"
        CLAMP("0x1c", "1") CLAMP("0x10", "2") CLAMP("0x28", "3") CLAMP("0x3c", "4")
        "  movl 0x3c(%esp), %eax\n"
        "  ret\n");
#undef CLAMP

static void InstallSectorClamp() {
    static const DWORD SITE = 0x50D60E;
    static const unsigned char orig[] = { 0xD9, 0x6C, 0x24, 0x28, 0x8B, 0x44, 0x24, 0x38 };
    if (memcmp((void*)SITE, orig, sizeof orig)) { Log("Fixes: ped sight code is not v1.0; sector clamp not installed"); return; }
    unsigned char code[8] = { 0xE8, 0, 0, 0, 0, 0x90, 0x90, 0x90 };
    *(int*)(code + 1) = (int)((DWORD)&vcmf_sector_clamp - (SITE + 5));
    DWORD old;
    VirtualProtect((void*)SITE, sizeof code, PAGE_EXECUTE_READWRITE, &old);
    memcpy((void*)SITE, code, sizeof code);
    VirtualProtect((void*)SITE, sizeof code, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)SITE, sizeof code);
    Log("Fixes: ped sight check kept inside the world grid (crash at 0x50D660)");
}

void InstallGameplay() {
    InstallSectorClamp();
    if (g_cfg.runAndGun) {
        // CWeaponInfo::GetWeaponInfo: lea eax,[ecx+ecx*2]; shl eax,5; lea eax,[eax+ecx*4]; add eax,0x782A14
        static const unsigned char kTagLookup[] = { 0x0F, 0xBF, 0x46, 0x24, 0x83, 0xF8, 0xFF };
        static const unsigned char get[] = { 0x8D, 0x04, 0x49, 0xC1, 0xE0, 0x05, 0x8D, 0x04, 0x88, 0x05, 0x14, 0x2A, 0x78, 0x00 };
        g_runGunOk = !memcmp((void*)0x5D5714, get, sizeof get)
                  && !memcmp((void*)0x401988, kTagLookup, sizeof kTagLookup)   // movsx eax,[esi+24h]; cmp eax,-1
                  && *(DWORD*)0x404BB4 == 0x7F3D28;
        if (g_runGunOk) {
            for (DWORD s : kLoadAnimCalls) {
                const unsigned char* p = (const unsigned char*)s;
                g_runGunOk = g_runGunOk && p[0] == 0xE8 && s + 5 + *(const int*)(p + 1) == LOAD_ANIM_FILE;
            }
        }
        if (g_runGunOk) {
            for (DWORD s : kLoadAnimCalls) {
                unsigned char call[5] = { 0xE8 };
                *(int*)(call + 1) = (int)((DWORD)&LoadAnimFileHook - (s + 5));
                DWORD old;
                VirtualProtect((void*)s, 5, PAGE_EXECUTE_READWRITE, &old);
                memcpy((void*)s, call, 5);
                VirtualProtect((void*)s, 5, old, &old);
            }
            FreeLegsInFireAnims();   // blocks already loaded keep their old links until reloaded
        }
        static const unsigned char sprintFire[] = { 0x0F, 0x84, 0x94, 0x01, 0x00, 0x00 };   // at 0x534B18
        static const unsigned char breakout[] = { 0x74, 0x3E };                              // at 0x52BDE0
        if (g_runGunOk && !memcmp((void*)SPRINT_FIRE_JE, sprintFire, 6) && !memcmp((void*)SPRINT_BREAKOUT_JE, breakout, 2)) {
            DWORD old;
            VirtualProtect((void*)SPRINT_FIRE_JE, 6, PAGE_EXECUTE_READWRITE, &old);
            memset((void*)SPRINT_FIRE_JE, 0x90, 6);
            VirtualProtect((void*)SPRINT_FIRE_JE, 6, old, &old);
            VirtualProtect((void*)SPRINT_BREAKOUT_JE, 1, PAGE_EXECUTE_READWRITE, &old);
            *(unsigned char*)SPRINT_BREAKOUT_JE = 0xEB;
            VirtualProtect((void*)SPRINT_BREAKOUT_JE, 1, old, &old);
            FlushInstructionCache(GetCurrentProcess(), nullptr, 0);
            Log("Weapons: firing while sprinting");
        }
        if (!memcmp((void*)0x5349D8, kScopeCode, sizeof kScopeCode)) {
            for (DWORD s : kScopeJumps) {
                DWORD old;
                VirtualProtect((void*)s, 2, PAGE_EXECUTE_READWRITE, &old);
                *(unsigned short*)s = 0x9090;
                VirtualProtect((void*)s, 2, old, &old);
            }
            FlushInstructionCache(GetCurrentProcess(), (void*)0x5349D8, sizeof kScopeCode);
            Log("Weapons: M4, Ruger and M60 aim in third person (move while aiming)");
        } else {
            Log("Weapons: aim code is not v1.0; M4/Ruger/M60 keep the first-person scope");
        }
        Log(g_runGunOk ? "Weapons: two-handed and heavy guns fire on the move (own animations, legs free)"
                       : "Weapons: weapon info is not v1.0; run and gun not installed");
    }
    if (g_cfg.keepFiringWhenHit) {
        bool ok = true;
        for (DWORD s : kClearAttackCalls) {
            const unsigned char* p = (const unsigned char*)s;
            ok = ok && p[0] == 0xE8 && s + 5 + *(const int*)(p + 1) == CLEAR_ATTACK_ANIM;
        }
        if (ok) {
            for (DWORD s : kClearAttackCalls) {
                unsigned char call[5] = { 0xE8 };
                *(int*)(call + 1) = (int)((DWORD)&ClearAttackUnlessPlayer - (s + 5));
                DWORD old;
                VirtualProtect((void*)s, 5, PAGE_EXECUTE_READWRITE, &old);
                memcpy((void*)s, call, 5);
                VirtualProtect((void*)s, 5, old, &old);
            }
            FlushInstructionCache(GetCurrentProcess(), nullptr, 0);
        }
        g_hitKeepOk = ok;
        Log(ok ? "Weapons: the player keeps firing when hit"
               : "Weapons: bullet impact code is not v1.0; keep firing when hit not installed");
    }
    if (g_cfg.mouseAim) {
        // fld [ebx+0x5B0] in TankControl and fadd/fstp [ebx+0x5B0] in FireTruckControl
        static const unsigned char tank[] = { 0xD9, 0x83, 0xB0, 0x05, 0x00, 0x00 };
        static const unsigned char truck[] = { 0xD8, 0x83, 0xB0, 0x05, 0x00, 0x00 };
        g_aimOk = !memcmp((void*)0x58659A, tank, sizeof tank) && !memcmp((void*)0x57AB90, truck, sizeof truck);
        Log(g_aimOk ? "Vehicles: Rhino turret and fire truck cannon aim with the camera (mouse)"
                    : "Vehicles: turret code is not v1.0; mouse aim not installed");
    }
    if (g_cfg.missionLeftovers) {
        // SaveAllScripts reads the array (mov ecx,[ebp+0x94DBE8] at 0x45F945), and
        // CBuilding::ReplaceWithNewModel's first bytes (another mod may hook the
        // call to it in UndoBuildingSwaps, so that call isn't checked)
        static const unsigned char save[] = { 0x8B, 0x8D, 0xE8, 0xDB, 0x94, 0x00 };
        static const unsigned char replace[] = { 0x53, 0x56, 0x8B, 0x31, 0x55, 0x89, 0xCD };
        const bool saveOk = !memcmp((void*)0x45F945, save, sizeof save);
        const bool replaceOk = !memcmp((void*)REPLACE_MODEL, replace, sizeof replace);
        g_leftoversOk = saveOk && replaceOk;
        if (!g_leftoversOk) Log("Fixes: swap array check %d, ReplaceWithNewModel check %d", saveOk, replaceOk);
        Log(g_leftoversOk ? "Fixes: mission leftovers check on (Ocean Drive hotel)"
                          : "Fixes: building swap code is not v1.0; mission leftovers check not installed");
    }
}
