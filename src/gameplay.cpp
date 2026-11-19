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
#include "population.h"
#include <windows.h>
#include <math.h>
#include <stdio.h>
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
    if (!g_aimOk || !g_cfg.mouseAim || !veh) return false;
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

static int FireAnimIndex(const char* name) {
    for (int i = 0; i < (int)(sizeof kFireAnims / sizeof kFireAnims[0]); i++)
        if (!_stricmp(name, kFireAnims[i])) return i;
    return -1;
}

// The torso turn. The two-handed fire animations are authored on a body that
// swings: their Root track turns the ped up to 48 degrees about the vertical (the
// rifle stance, right shoulder back; python_fire the other way), and Spine, Spine1
// and the arms are posed on top of that turned Root so the gun lies along the aim.
// With the Root and Pelvis tracks dropped, the body kept the walk/idle facing and
// the gun pointed off to the side by the missing turn. The turn can't be baked into
// the keyframes at load: what it must be measured against is the Root * Pelvis the
// walk/run/sprint/idle gives at that moment, which swings 10-60 degrees with the
// stride and the move animation. So after each animation update the turn is put on
// Spine1 (bone tag 3) from the live poses:
//   F = fire Root * Pelvis at the fire association's current time (the dropped tracks,
//       still in the hierarchy), C = Root * Pelvis now in the pose (from the base),
//   yaw = twist of F * C^-1 about the entity's vertical (Z), times the fire blend,
//   Spine1' = (C * Spine)^-1 * Rz(yaw) * (C * Spine) * Spine1.
// Spine1, not Spine: in the ped skeleton (player.dff frame list and HAnim node flags)
// the thighs hang from Spine (Root > Pelvis > Spine > {Spine1 > Neck > head, arms;
// L/R Thigh}), so only Spine1 turns the chest, arms and head without the legs.
// Quaternions in memory are x y z w with x y z negated from the .ifp on load
// (LoadAnimFile 0x404F32..), they match the ped's bind rotations, and a bone's world
// rotation is parent * local, so the turn is applied in the parent's frame as above.
// Keyframes are floats at sequence +0x28 (stride 0x20 with translation, else 0x14;
// +0x10 time, the first absolute, the rest deltas after CalcTotalTime 0x401DF0); the
// "compressed" load path is unused (LoadAnimFile clears its flag), so the data stays
// as loaded for the block's life and is read, never changed.
// Applied only to an association that plays one of the fire animations without its
// Root bone linked (the links are made when the block loads), so a block loaded with
// run and gun off plays the game's own stance untouched. kTurnSign is the direction;
// -1 would mirror the turn (the conjugate).
static const float kTurnSign = 1.0f;
static const DWORD CLUMP_OFFSET = 0x978798;       // RpAnimBlend plugin offset in an RpClump
static const DWORD UPDATE_ANIMS = 0x404690;       // RpAnimBlendClumpUpdateAnimations(clump, dt, doRender)
static const DWORD kUpdateAnimCalls[] = { 0x4D75BF, 0x4D769F };   // CWorld::Process
static const DWORD IS_CLUMP_SKINNED_FN = 0x57F580;
typedef void (__cdecl* UpdateAnims_t)(void*, float, int);
static UpdateAnims_t g_updateAnims[2];
static bool g_torsoOk = false;
static unsigned g_torsoLogged = 0;                // fire animations already logged at first use

struct Quat { float x, y, z, w; };
static Quat QMul(const Quat& a, const Quat& b) {
    return { a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
             a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
             a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
             a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z };
}
static Quat QConj(const Quat& q) { return { -q.x, -q.y, -q.z, q.w }; }
static bool QNorm(Quat& q) {
    const float l = sqrtf(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (!(l > 1e-6f && l < 1e6f)) return false;   // also false for NaN
    q = { q.x / l, q.y / l, q.z / l, q.w / l };
    return true;
}
// Angle of q's turn about the vertical (its twist about Z), -PI..PI.
static float YawOf(const Quat& q) { return Wrap(2.0f * atan2f(q.z, q.w)); }

// A track's rotation at time t (seconds), interpolated between its keyframes.
static bool SampleRot(const unsigned char* seq, float t, Quat& out) {
    const int n = *(const int*)(seq + 0x20);
    const unsigned char* kf = *(unsigned char* const*)(seq + 0x28);
    if (!kf || n <= 0 || n > 10000 || !(*(const int*)(seq + 4) & 1)) return false;
    const int stride = (*(const int*)(seq + 4) & 2) ? 0x20 : 0x14;
    float at = *(const float*)(kf + 0x10);
    if (n == 1 || t <= at) { out = *(const Quat*)kf; return QNorm(out); }
    for (int i = 1; i < n; i++) {
        const unsigned char* k = kf + i * stride;
        const float dt = *(const float*)(k + 0x10);
        if (t <= at + dt) {
            const float u = dt > 1e-6f ? (t - at) / dt : 1.0f;
            const Quat a = *(const Quat*)(k - stride), b = *(const Quat*)k;
            const float s = (a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w) < 0.0f ? -1.0f : 1.0f;
            out = { a.x + (b.x * s - a.x) * u, a.y + (b.y * s - a.y) * u,
                    a.z + (b.z * s - a.z) * u, a.w + (b.w * s - a.w) * u };
            return QNorm(out);
        }
        at += dt;
    }
    out = *(const Quat*)(kf + (n - 1) * stride);
    return QNorm(out);
}

static const unsigned char* FindTrack(const unsigned char* h, const char* bone) {
    const unsigned char* seqs = *(unsigned char* const*)(h + 0x18);
    const int n = *(const short*)(h + 0x1C);
    if (!seqs || n <= 0 || n > 64) return nullptr;
    for (int k = 0; k < n; k++)
        if (!_strnicmp((const char*)(seqs + k * 0x30 + 8), bone, 24)) return seqs + k * 0x30;
    return nullptr;
}

// Log line at load: the largest turn of Root * Pelvis from the first frame (the
// armed stance the animation starts from), at the Root keyframes.
static void LogFireAnim(const char* name, const unsigned char* h) {
    const unsigned char* root = FindTrack(h, "Root");
    const unsigned char* pelvis = FindTrack(h, "Pelvis");
    const unsigned char* spine1 = FindTrack(h, "Spine1");
    if (!root || !pelvis) { Log("Weapons: %s: legs freed (no Root/Pelvis track found)", name); return; }
    const int n = *(const int*)(root + 0x20);
    const unsigned char* kf = *(unsigned char* const*)(root + 0x28);
    const int stride = (*(const int*)(root + 4) & 2) ? 0x20 : 0x14;
    float t = 0.0f, big = 0.0f;
    Quat first = { 0, 0, 0, 1 };
    for (int i = 0; kf && i < n && i < 10000; i++) {
        t = i ? t + *(const float*)(kf + i * stride + 0x10) : *(const float*)(kf + 0x10);
        Quat r, p;
        if (!SampleRot(root, t, r) || !SampleRot(pelvis, t, p)) continue;
        const Quat f = QMul(r, p);
        if (!i) { first = f; continue; }
        const float y = YawOf(QMul(f, QConj(first)));
        if (fabsf(y) > fabsf(big)) big = y;
    }
    Log("Weapons: %s: legs freed, Root turn (max %+.1f deg, %d Root keyframes) put on Spine1 each update (%d Spine1 keyframes)",
        name, big * 180.0f / PI, n, spine1 ? *(const int*)(spine1 + 0x20) : 0);
}

static void FreeLegsInFireAnims() {
    const int count = *(int*)0x978540;
    if (count <= 0 || count > 4000) return;
    for (int i = 0; i < count; i++) {
        unsigned char* h = (unsigned char*)(0x7F3D28 + i * 0x28);
        const char* name = (const char*)h;
        if (name[0] != 'R' && name[0] != 'U' && name[0] != 'M' && name[0] != 'F' &&
            name[0] != 'b' && name[0] != 's' && name[0] != 'p') continue;
        if (FireAnimIndex(name) < 0) continue;
        unsigned char* seqs = *(unsigned char**)(h + 0x18);
        const int n = *(short*)(h + 0x1C);
        if (!seqs || n <= 0 || n > 64) continue;
        int stripped = 0;
        for (int k = 0; k < n; k++) {
            short* tag = (short*)(seqs + k * 0x30 + 0x24);
            if (IsLegTrack(*tag)) { *tag = kNoBone; stripped++; }
        }
        if (stripped) LogFireAnim(name, h);   // fresh from the file (a second pass finds nothing left to strip)
    }
}

// After a clump's animations were updated with its bones posed (doRender): turn
// Spine1 by the dropped Root turn of a fire animation playing on it (see above).
static void TurnTorsoToFireAnim(unsigned char* clump) {
    if (!clump || clump[0] != 2) return;   // rpCLUMP
    const int off = *(const int*)CLUMP_OFFSET;
    if (off <= 0 || off > 0x1000) return;
    unsigned char* cd = *(unsigned char**)(clump + off);
    if (!cd) return;
    const int count = *(const int*)0x978540;
    Quat* bone[4] = {};
    Quat C = { 0, 0, 0, 1 };
    bool haveBones = false;
    float yaw = 0.0f, weight = 0.0f;
    int logIdx = -1;
    unsigned char* link = *(unsigned char**)cd;
    for (int guard = 0; link && guard < 64; guard++, link = *(unsigned char**)link) {
        const unsigned char* a = link - 4;
        const unsigned char* h = *(unsigned char* const*)(a + 0x14);
        const DWORD rel = (DWORD)h - 0x7F3D28;
        if (!h || rel % 0x28 || (int)(rel / 0x28) >= count || h[0x1E]) continue;
        const int fi = FireAnimIndex((const char*)h);
        if (fi < 0) continue;
        float b = *(const float*)(a + 0x18);
        if (!(b > 0.001f)) continue;
        if (b > 1.0f) b = 1.0f;
        const unsigned char* root = FindTrack(h, "Root");
        const unsigned char* pelvis = FindTrack(h, "Pelvis");
        if (!root || !pelvis) continue;
        // Root still linked to its bone: the block was loaded with run and gun off and
        // the game plays the animation whole.
        const unsigned char* nodes = *(unsigned char* const*)(a + 0x10);
        const int nn = *(const short*)(a + 0x0C);
        if (!nodes || nn <= 0 || nn > 64) continue;
        bool linked = false;
        for (int i = 0; i < nn && !linked; i++) linked = *(unsigned char* const*)(nodes + i * 0x1C + 0x14) == root;
        if (linked) continue;
        if (!haveBones) {
            if (!((char(__cdecl*)(void*))IS_CLUMP_SKINNED_FN)(clump)) return;
            const unsigned char* frames = *(unsigned char* const*)(cd + 0x10);
            const int nf = *(const int*)(cd + 8);
            if (!frames || nf <= 0 || nf > 64) return;
            for (int i = 0; i < nf; i++) {
                const int id = *(const int*)(frames + i * 0x18 + 0x14);
                unsigned char* kf = *(unsigned char* const*)(frames + i * 0x18 + 0x10);
                if (id >= 0 && id <= 3 && kf) bone[id] = (Quat*)(kf + 8);
            }
            if (!bone[0] || !bone[1] || !bone[2] || !bone[3]) return;
            C = QMul(*bone[0], *bone[1]);
            if (!QNorm(C)) return;
            haveBones = true;
        }
        Quat r, p;
        if (!SampleRot(root, *(const float*)(a + 0x20), r) || !SampleRot(pelvis, *(const float*)(a + 0x20), p)) continue;
        const float y = YawOf(QMul(QMul(r, p), QConj(C)));
        if (!(fabsf(y) <= PI)) continue;
        yaw += b * y;
        weight += b;
        logIdx = fi;
    }
    if (!(weight > 0.0f)) return;
    if (weight > 1.0f) yaw /= weight;
    yaw *= kTurnSign;
    if (yaw > PI / 2) yaw = PI / 2;
    if (yaw < -PI / 2) yaw = -PI / 2;
    if (!(fabsf(yaw) > 0.0005f)) return;
    Quat M = QMul(C, *bone[2]);
    if (!QNorm(M)) return;
    const Quat G = { 0.0f, 0.0f, sinf(yaw * 0.5f), cosf(yaw * 0.5f) };
    Quat s1 = QMul(QMul(QMul(QConj(M), G), M), *bone[3]);
    if (!QNorm(s1)) return;
    *bone[3] = s1;
    if (logIdx >= 0 && !(g_torsoLogged & (1u << logIdx))) {
        g_torsoLogged |= 1u << logIdx;
        Log("Weapons: %s: torso turned %+.1f deg on Spine1 (first use this session)", kFireAnims[logIdx], yaw * 180.0f / PI);
    }
}

static void __cdecl UpdateAnimsHook0(void* clump, float dt, int doRender) {
    g_updateAnims[0](clump, dt, doRender);
    if ((doRender & 0xFF) && g_torsoOk) TurnTorsoToFireAnim((unsigned char*)clump);
}
static void __cdecl UpdateAnimsHook1(void* clump, float dt, int doRender) {
    g_updateAnims[1](clump, dt, doRender);
    if ((doRender & 0xFF) && g_torsoOk) TurnTorsoToFireAnim((unsigned char*)clump);
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
    if (g_cfg.runAndGun) FreeLegsInFireAnims();
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

static bool g_heavyHad[2] = { false, false };   // the flag was set (put back when turned off in the menu)
static void RunAndGun() {
    for (int i = 0; i < 2; i++) {
        unsigned* f = WeaponFlags(kHeavyWeapons[i]);
        if (*f & 0x200u) { g_heavyHad[i] = true; *f &= ~0x200u; }
    }
}

// The code patches of run and gun: on, or back to the game's own bytes (the menu).
static bool g_sprintOk = false, g_scopeOk = false;
static unsigned char g_scopeOrig[3][2];
static void WriteCode(DWORD a, const void* b, int n) {
    DWORD old;
    VirtualProtect((void*)a, n, PAGE_EXECUTE_READWRITE, &old);
    memcpy((void*)a, b, n);
    VirtualProtect((void*)a, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)a, n);
}
static void RunAndGunPatches(bool on) {
    if (g_sprintOk) {
        static const unsigned char sprintFire[] = { 0x0F, 0x84, 0x94, 0x01, 0x00, 0x00 };   // at 0x534B18
        static const unsigned char nops[6] = { 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 };
        WriteCode(SPRINT_FIRE_JE, on ? nops : sprintFire, 6);
        const unsigned char jb = on ? 0xEB : 0x74;
        WriteCode(SPRINT_BREAKOUT_JE, &jb, 1);
    }
    if (g_scopeOk) {
        static const unsigned char nop2[2] = { 0x90, 0x90 };
        for (int i = 0; i < 3; i++) WriteCode(kScopeJumps[i], on ? nop2 : g_scopeOrig[i], 2);
    }
}

// Keep firing when hit ([Weapons] KeepFiringWhenHit). When a bullet hits a ped
// (CWeapon::DoBulletImpact at 0x5CF493 for the player, and 4 more hit paths),
// the game calls CPed::ClearAttackByRemovingAnim (0x52CF70) before playing the
// "shot" flinch: the player stops shooting and has to press fire again. The calls
// go through this check: the player keeps the flinch but not the cancelled attack.
static const DWORD CLEAR_ATTACK_ANIM = 0x52CF70;
static const DWORD kClearAttackCalls[] = { 0x5C4014, 0x5C9D73, 0x5CBC42, 0x5CF493, 0x5CF4FC };
static void __fastcall ClearAttackUnlessPlayer(void* ped, void*) {
    if (g_cfg.keepFiringWhenHit && ped == ((void* (__cdecl*)())0x4BC120)()) return;   // FindPlayerPed
    ((void (__thiscall*)(void*))CLEAR_ATTACK_ANIM)(ped);
}

// ---------------------------------------------------------------- invalid positions
// An entity whose position, orientation or speed became NaN / infinite (seen
// after leaving the mall onto its roof: the camera's target got a NaN position, every
// sector number then came out as 0x80000000, and the water-level lookup under the
// camera, 0x5C2D01, read far outside its table). Before each update every ped,
// vehicle and object is checked: one that went bad gets back its last good matrix
// (kept per pool slot each update; entity matrix +4: right, forward, up, position),
// its move and turn speeds (+0x70, +0x7C) are zeroed, its RenderWare frame is
// updated (CEntity::UpdateRwFrame 0x489360), and it is logged with its pool and
// model. The active camera (TheCamera.Cams[ActiveCam]: Front +0x168, Source +0x174,
// Up +0x18C), which the car camera builds on from its last position, is put back
// behind its target if it went bad too.
struct NanPool { DWORD pool, size; int slots; const char* name; };
static const NanPool kNanPools[] = {
    { 0x97F2AC, 0x6D8, PED_POOL_SLOTS, "ped" }, { 0xA0FDE4, 0x5DC, 110, "vehicle" }, { 0x94DBE0, 0x1A0, 460, "object" } };
struct GoodMatrix { const unsigned char* ent; float m[16]; };
static GoodMatrix g_good[PED_POOL_SLOTS + 110 + 460];
static unsigned g_nanLogs = 0;

static bool Finite(const float* f, int n) {
    for (int i = 0; i < n; i++) {
        const unsigned bits = *(const unsigned*)&f[i];
        if ((bits & 0x7F800000u) == 0x7F800000u) return false;   // NaN or infinity
        if (f[i] > 1.0e6f || f[i] < -1.0e6f) return false;
    }
    return true;
}

// The matrix's right, forward, up and position (each followed by a flags / pad word).
static bool MatrixFinite(const float* m) {
    return Finite(m, 3) && Finite(m + 4, 3) && Finite(m + 8, 3) && Finite(m + 12, 3);
}

// CWaterLevel::GetWaterLevel (0x5C2C80) turns the position into a cell of its 128 x
// 128 table (0x864C70) without a range check; with a NaN position the cell came out
// as 0x80000000 and the read crashed (0x5C2D01). At 0x5C2CF8 (shl eax,7; mov ecx,
// [esp+8]; add eax,ecx) a stub keeps both numbers in 0-127.
extern "C" { volatile long g_waterClamps = 0; }
extern "C" void vcmf_water_clamp();
__asm__(".text\n"
        ".globl _vcmf_water_clamp\n_vcmf_water_clamp:\n"
        "  movl 0xc(%esp), %ecx\n"
        "  cmpl $127, %eax\n  jbe 1f\n  lock incl _g_waterClamps\n"
        "  testl %eax, %eax\n  jl 2f\n  movl $127, %eax\n  jmp 1f\n2: xorl %eax, %eax\n"
        "1: cmpl $127, %ecx\n  jbe 3f\n  lock incl _g_waterClamps\n"
        "  testl %ecx, %ecx\n  jl 4f\n  movl $127, %ecx\n  jmp 3f\n4: xorl %ecx, %ecx\n"
        "3: shll $7, %eax\n  addl %ecx, %eax\n  ret\n");

static void InstallWaterClamp() {
    static const DWORD SITE = 0x5C2CF8;
    static const unsigned char orig[] = { 0xC1, 0xE0, 0x07, 0x8B, 0x4C, 0x24, 0x08, 0x01, 0xC8, 0x8A, 0x80, 0x70, 0x4C, 0x86, 0x00 };
    if (memcmp((void*)SITE, orig, sizeof orig)) { Log("Fixes: water level code is not v1.0; range check not installed"); return; }
    unsigned char code[9] = { 0xE8, 0, 0, 0, 0, 0x90, 0x90, 0x90, 0x90 };
    *(int*)(code + 1) = (int)((DWORD)&vcmf_water_clamp - (SITE + 5));
    DWORD old;
    VirtualProtect((void*)SITE, sizeof code, PAGE_EXECUTE_READWRITE, &old);
    memcpy((void*)SITE, code, sizeof code);
    VirtualProtect((void*)SITE, sizeof code, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)SITE, sizeof code);
    Log("Fixes: water level lookup kept inside its table (crash at 0x5C2D01)");
}

// ---------------------------------------------------------------- angle wrap loops
// The car AI (CCarCtrl steering, 0x41E000..0x424100) brings angles into -pi..pi with
// loops like `while (a < -pi) a += 2*pi` (fld [a]; fadd [0x687088] 2*pi; fstp [a];
// fld [a]; fcomp [0x68708C] -pi; fnstsw; ...; je back) and the twin `while (a > pi)
// a -= 2*pi` (fsub [0x687088]; fcomp [0x687090] +pi). With a = -inf or NaN, or a
// finite value so large that adding 2*pi doesn't change it, the loop never ends:
// the freeze at 0x42335C. All 24 copies in the exe (found by those byte patterns)
// get their fcomp replaced by a call to a stub of their own: the fld before it
// loaded a; the stub drops that, fixes a in memory and does the same fld / fcomp
// itself, so the loop's own test sees the result and ends:
//  - inside -pi..pi already: nothing changes (one site, 0x422E90, also runs its
//    first test through here);
//  - finite and |a| <= 1e4: wrapped in one step, the fmod the loop does in many
//    (a - 2*pi * floor((a + pi) / 2*pi), kept within the game's own -pi / +pi);
//  - not finite, or |a| > 1e4: 0.
// The wrap is done with SSE registers, so the x87 stack the game is using around the
// loop is never deeper than the loop's own fld made it.
struct AngleLoop { DWORD fcomp, fld; DWORD bound; };   // bound: 0x68708C (-pi) or 0x687090 (+pi)
static const AngleLoop kAngleLoops[] = {
    { 0x41E159, 0x41E155, 0x68708C }, { 0x41E3B4, 0x41E3B0, 0x68708C }, { 0x41EBB3, 0x41EBAF, 0x68708C },
    { 0x41ED09, 0x41ED05, 0x68708C }, { 0x41F66A, 0x41F666, 0x68708C }, { 0x41F932, 0x41F92E, 0x68708C },
    { 0x41FA47, 0x41FA43, 0x68708C }, { 0x421747, 0x421743, 0x68708C }, { 0x422E90, 0x422E8E, 0x68708C },
    { 0x423360, 0x42335E, 0x68708C }, { 0x423A60, 0x423A5E, 0x68708C }, { 0x424022, 0x42401E, 0x68708C },
    { 0x41E192, 0x41E18E, 0x687090 }, { 0x41E3F2, 0x41E3EE, 0x687090 }, { 0x41EBE6, 0x41EBE2, 0x687090 },
    { 0x41ED44, 0x41ED40, 0x687090 }, { 0x41F6A2, 0x41F69E, 0x687090 }, { 0x41F967, 0x41F963, 0x687090 },
    { 0x41FA82, 0x41FA7E, 0x687090 }, { 0x421712, 0x42170E, 0x687090 }, { 0x422F4C, 0x422F4A, 0x687090 },
    { 0x42341C, 0x42341A, 0x687090 }, { 0x423B1C, 0x423B1A, 0x687090 }, { 0x424055, 0x424051, 0x687090 },
};
static const int kNumAngleLoops = sizeof kAngleLoops / sizeof *kAngleLoops;
extern "C" {
volatile long g_angleResets[32] = {}, g_angleWraps[32] = {};
volatile unsigned g_angleBad[32] = {};   // the last value written 0 at each, as bits
void vcmf_angle_fix();
}
// eax = the angle's address, edx = the loop's index. Keeps every general register but
// ecx (saved by the caller), and xmm0-2.
__asm__(".text\n"
        ".globl _vcmf_angle_fix\n_vcmf_angle_fix:\n"
        "  subl $48, %esp\n"
        "  movups %xmm0, (%esp)\n  movups %xmm1, 16(%esp)\n  movups %xmm2, 32(%esp)\n"
        "  movl (%eax), %ecx\n  andl $0x7FFFFFFF, %ecx\n"
        "  cmpl $0x7F800000, %ecx\n  jae 8f\n"           // infinity or NaN
        "  cmpl $0x461C4000, %ecx\n  ja 8f\n"            // |a| > 10000
        "  movss (%eax), %xmm0\n"
        "  ucomiss 0x68708C, %xmm0\n  jb 1f\n"           // a < -pi
        "  ucomiss 0x687090, %xmm0\n  jbe 9f\n"          // a <= +pi: as it is
        "1: movaps %xmm0, %xmm1\n"
        "  addss 0x687090, %xmm1\n  divss 0x687088, %xmm1\n"   // (a + pi) / 2pi
        "  cvttss2si %xmm1, %ecx\n  cvtsi2ssl %ecx, %xmm2\n"
        "  ucomiss %xmm2, %xmm1\n  jae 2f\n  decl %ecx\n"     // floor
        "2: cvtsi2ssl %ecx, %xmm2\n  mulss 0x687088, %xmm2\n  subss %xmm2, %xmm0\n"
        "  maxss 0x68708C, %xmm0\n  minss 0x687090, %xmm0\n"
        "  movss %xmm0, (%eax)\n"
        "  lock incl _g_angleWraps(,%edx,4)\n  jmp 9f\n"
        "8: movl (%eax), %ecx\n  movl %ecx, _g_angleBad(,%edx,4)\n"
        "  movl $0, (%eax)\n  lock incl _g_angleResets(,%edx,4)\n"
        "9: movups (%esp), %xmm0\n  movups 16(%esp), %xmm1\n  movups 32(%esp), %xmm2\n"
        "  addl $48, %esp\n  ret\n");

static bool g_angleOk[32];
static long g_angleLogged[32];
static unsigned g_angleLogs = 0;

// Builds one stub: fstp st0; push eax/ecx/edx; eax = &a; edx = index; call
// vcmf_angle_fix; pop; fld [a]; fcomp [bound]; ret. The fld forms in the table:
// [esp+disp8], [esi], [ebx].
static bool BuildAngleStub(int k, unsigned char* out) {
    const AngleLoop& L = kAngleLoops[k];
    const unsigned char* f = (const unsigned char*)L.fld;
    const unsigned char* c = (const unsigned char*)L.fcomp;
    const int fldLen = (int)(L.fcomp - L.fld);
    if (c[0] != 0xD8 || c[1] != 0x1D || *(const DWORD*)(c + 2) != L.bound || f[0] != 0xD9) return false;
    const bool espForm = fldLen == 4 && f[1] == 0x44 && f[2] == 0x24;
    const bool esiForm = fldLen == 2 && f[1] == 0x06, ebxForm = fldLen == 2 && f[1] == 0x03;
    if (!espForm && !esiForm && !ebxForm) return false;
    const DWORD disp = espForm ? (DWORD)(int)(signed char)f[3] : 0;
    unsigned char* p = out;
    *p++ = 0xDD; *p++ = 0xD8;                                    // fstp st(0): the a the loop loaded
    *p++ = 0x50; *p++ = 0x51; *p++ = 0x52;                       // push eax, ecx, edx
    if (espForm) { *p++ = 0x8D; *p++ = 0x84; *p++ = 0x24; *(DWORD*)p = disp + 16; p += 4; }   // lea eax,[esp+disp+16]
    else { *p++ = 0x89; *p++ = esiForm ? 0xF0 : 0xD8; }          // mov eax, esi / ebx
    *p++ = 0xBA; *(DWORD*)p = (DWORD)k; p += 4;                  // mov edx, k
    *p++ = 0xE8; *(int*)p = (int)((DWORD)&vcmf_angle_fix - ((DWORD)p + 4)); p += 4;
    *p++ = 0x5A; *p++ = 0x59; *p++ = 0x58;                       // pop edx, ecx, eax
    if (espForm) { *p++ = 0xD9; *p++ = 0x84; *p++ = 0x24; *(DWORD*)p = disp + 4; p += 4; }   // fld [esp+disp+4]
    else { *p++ = 0xD9; *p++ = esiForm ? 0x06 : 0x03; }          // fld [esi] / [ebx]
    *p++ = 0xD8; *p++ = 0x1D; *(DWORD*)p = L.bound; p += 4;      // fcomp [bound]
    *p++ = 0xC3;
    return true;
}

static void InstallAngleWrapGuard() {
    // the constants the loops use: 2*pi, -pi, +pi
    if (*(const float*)0x687088 < 6.28f || *(const float*)0x687088 > 6.29f || *(const float*)0x68708C > -3.14f ||
        *(const float*)0x687090 < 3.14f) {
        Log("Fixes: car AI angle constants are not v1.0; angle wrap loops left as they are");
        return;
    }
    unsigned char* mem = (unsigned char*)VirtualAlloc(nullptr, 64 * kNumAngleLoops, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!mem) return;
    int n = 0;
    for (int k = 0; k < kNumAngleLoops; k++) {
        unsigned char* stub = mem + 64 * k;
        if (!BuildAngleStub(k, stub)) {
            Log("Fixes: angle wrap loop at %06lX is not the v1.0 code; left as it is", kAngleLoops[k].fcomp);
            continue;
        }
        unsigned char call[6] = { 0xE8, 0, 0, 0, 0, 0x90 };
        *(int*)(call + 1) = (int)((DWORD)stub - (kAngleLoops[k].fcomp + 5));
        WriteCode(kAngleLoops[k].fcomp, call, 6);
        g_angleOk[k] = true;
        n++;
    }
    FlushInstructionCache(GetCurrentProcess(), mem, 64 * kNumAngleLoops);
    Log("Fixes: %d of %d car AI angle wrap loops end on any value (freeze at 0x42335C): fmod in one step, 0 for a value not finite or past 1e4",
        n, kNumAngleLoops);
}

static void LogAngleWraps() {
    for (int k = 0; k < kNumAngleLoops; k++) {
        const long n = g_angleResets[k];
        if (!g_angleOk[k] || n == g_angleLogged[k]) continue;
        g_angleLogged[k] = n;
        if (g_angleLogs++ >= 100) continue;
        const unsigned bits = g_angleBad[k];
        float v;
        memcpy(&v, &bits, 4);
        Log("Fixes: angle wrap loop at %06lX (%s) got %g, wrote 0 (would have looped forever); at this loop: %ld written 0, %ld wrapped in one step",
            kAngleLoops[k].fcomp, kAngleLoops[k].bound == 0x68708C ? "a < -pi" : "a > pi", v, n, g_angleWraps[k]);
    }
}

static void FixCamera() {
    const int act = *(unsigned char*)0x7E46FE;
    unsigned char* cam = (unsigned char*)(0x7E4810 + act * 0x1CC);
    float* front = (float*)(cam + 0x168);
    float* src = (float*)(cam + 0x174);
    float* up = (float*)(cam + 0x18C);
    if (Finite(front, 3) && Finite(src, 3) && Finite(up, 3)) return;
    const unsigned char* target = *(unsigned char* const*)(cam + 0x1B0);
    if (!target || !MatrixFinite((const float*)(target + 4))) return;
    const float* fwd = (const float*)(target + 0x14);
    const float* pos = (const float*)(target + 0x34);
    src[0] = pos[0] - fwd[0] * 6.0f; src[1] = pos[1] - fwd[1] * 6.0f; src[2] = pos[2] + 2.0f;
    float f[3] = { pos[0] - src[0], pos[1] - src[1], pos[2] - src[2] };
    const float l = sqrtf(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    for (int i = 0; i < 3; i++) front[i] = l > 0.01f ? f[i] / l : (i == 1 ? 1.0f : 0.0f);
    up[0] = 0.0f; up[1] = 0.0f; up[2] = 1.0f;
    if (g_nanLogs++ < 40) Log("Fixes: camera had an invalid position; put back behind its target");
}

// A vehicle whose speeds are finite but absurd counts as invalid too (freeze at
// 0x42335C: a cop seated at the wheel had its collision switched on by a mod, collided
// with its own car, and the car's physics produced a heading the AI steering's angle
// wrap could never bring back). The bounds, from the game's own limits:
//  - move speed: the fastest top speed in handling.cfg is the Infernus' 240 km/h,
//    stored as 240 / 180 = 1.33 units a step (a step is 1/50 s); 100 units a step
//    (5 km/s, 18,000 km/h, the whole map in under a second) is 75 times that, far
//    past any boost a trainer gives;
//  - turn speed: 10 radians a step (500 rad/s, 80 turns a second); a car flipped by
//    an explosion turns well under 1.
static const float VEH_MAX_MOVE = 100.0f, VEH_MAX_TURN = 10.0f;
static unsigned g_vehResetLogs = 0;

static float Length3(const float* v) { return sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }

static void FixInvalidEntities() {
    int slot0 = 0;
    for (const NanPool& p : kNanPools) {
        const unsigned char* pool = *(const unsigned char* const*)p.pool;
        if (!pool) { slot0 += p.slots; continue; }
        unsigned char* ents = *(unsigned char* const*)pool;
        const unsigned char* flags = *(const unsigned char* const*)(pool + 4);
        int n = *(const int*)(pool + 8);
        if (n > p.slots) n = p.slots;
        for (int i = 0; i < n; i++) {
            GoodMatrix& g = g_good[slot0 + i];
            if (flags[i] & 0x80) { g.ent = nullptr; continue; }   // free slot
            unsigned char* e = ents + i * p.size;
            float* m = (float*)(e + 4);
            float* speeds = (float*)(e + 0x70);
            const bool ok = MatrixFinite(m) && Finite(speeds, 6);
            const bool vehicle = p.pool == 0xA0FDE4;
            const float move = ok && vehicle ? Length3(speeds) : 0.0f, turn = ok && vehicle ? Length3(speeds + 3) : 0.0f;
            const bool absurd = move > VEH_MAX_MOVE || turn > VEH_MAX_TURN;
            if (ok && !absurd) { g.ent = e; memcpy(g.m, m, sizeof g.m); continue; }
            const bool had = g.ent == e;
            if (absurd) {
                if (g_vehResetLogs++ < 100)
                    Log("Fixes: vehicle %08X model %d had heading %.3g / speed %.3g (turn speed %.3g), reset (%s)",
                        (unsigned)(DWORD)e, *(short*)(e + 0x5C), atan2f(-m[4], m[5]), move, turn,
                        had ? "put back where it was last update, stopped" : "stopped");
            } else if (g_nanLogs++ < 40)
                Log("Fixes: %s (model %d) had an invalid position / speed (%.3g %.3g %.3g); %s",
                    p.name, *(short*)(e + 0x5C), m[12], m[13], m[14],
                    had ? "put back where it last was" : "no good position known, stopped only");
            if (had) memcpy(m, g.m, sizeof g.m);
            else if (!MatrixFinite(m)) {   // no good matrix for it: upright at the player
                const unsigned char* player = ((const unsigned char* (__cdecl*)())0x4BC120)();
                static const float axes[3][3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };
                for (int k = 0; k < 3; k++) memcpy(m + k * 4, axes[k], 12);
                if (player && player != e && Finite((const float*)(player + 0x34), 3)) memcpy(m + 12, player + 0x34, 12);
                else { m[12] = 0.0f; m[13] = 0.0f; m[14] = 20.0f; }
            }
            memset(speeds, 0, 6 * sizeof(float));
            if (*(void**)(e + 0x4C)) ((void (__thiscall*)(void*))0x489360)(e);   // CEntity::UpdateRwFrame
        }
        slot0 += p.slots;
    }
    FixCamera();
}

static bool g_fromTick = false;   // FixedTick calls GameplayBeforeTick
static void LogSectorItems();
static void LogPhysicalAddClamps();

// ---------------------------------------------------------------- police station flag
// Two stars "for no reason" when a game starts. The script SECURI (main.scm
// 0x1BB08) gives 2 stars (ALTER_WANTED_LEVEL_NO_DROP 2) while the player is in the
// BEACH2 zone (Ocean Beach, the Ocean View hotel included), global $987 is 1 and
// they don't wear the cop outfit (PLAYER6). $987 means "inside the police
// station": its entry door (0x1D46C, with SET_AREA_VISIBLE 12) sets it, and only
// walking out through its exit door (0x1D63B) clears it. Leaving another way (a
// trainer teleport, flying out, the interior view) left it at 1, it went into the
// save, and every load in Ocean Beach brought the stars back. It is cleared when the
// player isn't in the station's interior (area 12, near 390 -478), before the
// scripts run, with this main.scm only (its 0038 $987 1 test at 0x1BB3B checked).
static const DWORD SCRIPT_SPACE = 0x821280, STATION_FLAG = SCRIPT_SPACE + 987 * 4;
static int g_stationOk = -1;
static void StationFlag() {
    if (g_stationOk < 0) {
        static const unsigned char test[] = { 0x38, 0x00, 0x02, 0x6C, 0x0F, 0x04, 0x01 };   // 0038 $987 1 (int8)
        g_stationOk = !memcmp((const void*)(SCRIPT_SPACE + 0x1BB3B), test, sizeof test);
        if (!g_stationOk) Log("Fixes: main.scm is not the game's own; police station flag left alone");
    }
    if (!g_stationOk) return;
    int& flag = *(int*)STATION_FLAG;
    if (flag != 1) return;
    const unsigned char* ped = ((const unsigned char* (__cdecl*)())0x4BC120)();   // FindPlayerPed
    if (!ped) return;
    const float* p = (const float*)(ped + 0x34);
    const float dx = p[0] - 390.0f, dy = p[1] - (-478.0f);
    const bool inside = *(const int*)0x978810 == 12 && dx * dx + dy * dy < 60.0f * 60.0f;
    if (inside) return;
    flag = 0;
    static int logs = 0;
    if (logs++ < 5) Log("Fixes: \"inside the police station\" was still set with the player outside it (at %.0f %.0f, area %d): "
                        "cleared (it gave 2 stars in Ocean Beach)", p[0], p[1], *(const int*)0x978810);
}

static void Run() {
    LogAngleWraps();
    LogSectorItems();
    LogPhysicalAddClamps();
    if (*(int*)GAME_STATE != 9 || *(unsigned char*)MENU_ACTIVE) return;
    if (g_cfg.stationFlag) StationFlag();
    if (!ReplayPlaying()) FixInvalidEntities();
    if (g_leftoversOk && g_cfg.missionLeftovers) MissionLeftovers();
    if (g_aimOk && g_cfg.mouseAim) MouseAim();
    if (g_runGunOk && g_cfg.runAndGun) RunAndGun();
}

void GameplayBeforeTick() { g_fromTick = true; Run(); }
static void NearPedListAfterPresent();   // below
void GameplayAfterPresent() {
    if (!g_fromTick) Run();   // FixedTick off: once per frame
    NearPedListAfterPresent();
}

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

// ---------------------------------------------------------------- physicals past the map edge
// CPhysical::Add (0x4BAEE0) puts a moving vehicle, ped or object into the world
// sectors its bounding box covers: sector = 0x792D30 + (y * 80 + x) * 40, with
// x = X * 0.02 + 48 and y = Y * 0.02 + 40 for the box's corners and centre. Unlike
// the game's other sector code it never keeps them inside the 80 x 80 grid, so a
// vehicle past the map edge (flown far out over the sea) is linked into memory past
// the grid: that corrupted other game data and crashed later (a list head holding
// the text "ARMO" at 0x4BB53E, an empty list entry at 0x4B119B). When the last of the
// six numbers is worked out (0x4BB043: mov eax,[esp+30h]; cmp eax,[esp+28h]) a stub
// clamps them all to 0-79, as CEntity::Add does: such a vehicle is kept in the edge
// sectors.
extern "C" { volatile long g_physAddClamps = 0; }
extern "C" void vcmf_physadd_clamp();
#define PCLAMP(slot, n) \
    "  movl " slot "(%esp), %eax\n  testl %eax, %eax\n  jge 1" n "f\n  xorl %eax, %eax\n  lock incl _g_physAddClamps\n" \
    "1" n ": cmpl $79, %eax\n  jle 2" n "f\n  movl $79, %eax\n  lock incl _g_physAddClamps\n" \
    "2" n ": movl %eax, " slot "(%esp)\n"
__asm__(".text\n"
        ".globl _vcmf_physadd_clamp\n_vcmf_physadd_clamp:\n"
        PCLAMP("0x24", "1") PCLAMP("0x18", "2") PCLAMP("0x2c", "3")
        PCLAMP("0x1c", "4") PCLAMP("0x20", "5") PCLAMP("0x34", "6")
        "  movl 0x34(%esp), %eax\n"
        "  cmpl 0x2c(%esp), %eax\n"
        "  ret\n"
        // CPhysical::Add itself (0x4BB310, called through the vtable), same six numbers
        // in other slots; last at 0x4BB46D: mov eax,[esp+28h]; cmp eax,[esp+20h]
        ".globl _vcmf_physadd2_clamp\n_vcmf_physadd2_clamp:\n"
        PCLAMP("0x1c", "7") PCLAMP("0x10", "8") PCLAMP("0x24", "9")
        PCLAMP("0x14", "10") PCLAMP("0x18", "11") PCLAMP("0x2c", "12")
        "  movl 0x2c(%esp), %eax\n"
        "  cmpl 0x24(%esp), %eax\n"
        "  ret\n");
#undef PCLAMP
extern "C" void vcmf_physadd2_clamp();
static long g_physAddLogged = 0;

static void InstallPhysicalAddClamp() {
    // both copies of the sector code: the re-file (0x4BAEE0) and CPhysical::Add (0x4BB310)
    static const unsigned char orig1[] = { 0x8B, 0x44, 0x24, 0x30, 0x3B, 0x44, 0x24, 0x28 };   // at 0x4BB043
    static const unsigned char grid1[] = { 0x81, 0x44, 0x24, 0x04, 0x30, 0x2D, 0x79, 0x00 };   // add [esp+4], 0x792D30 at 0x4BB095
    static const unsigned char orig2[] = { 0x8B, 0x44, 0x24, 0x28, 0x3B, 0x44, 0x24, 0x20 };   // at 0x4BB46D
    static const unsigned char grid2[] = { 0x81, 0xC5, 0x30, 0x2D, 0x79, 0x00 };               // add ebp, 0x792D30 at 0x4BB4BA
    const struct { DWORD site; const unsigned char* orig; DWORD gridAt; const unsigned char* grid; size_t gridLen; void* stub; } sites[] = {
        { 0x4BB043, orig1, 0x4BB095, grid1, sizeof grid1, (void*)&vcmf_physadd_clamp },
        { 0x4BB46D, orig2, 0x4BB4BA, grid2, sizeof grid2, (void*)&vcmf_physadd2_clamp },
    };
    for (const auto& s : sites) {
        if (memcmp((void*)s.site, s.orig, 8) || memcmp((void*)s.gridAt, s.grid, s.gridLen)) {
            Log("Fixes: sector code at %06lX is not v1.0; sector clamp for things past the map edge not installed there", s.site);
            continue;
        }
        unsigned char code[8] = { 0xE8, 0, 0, 0, 0, 0x90, 0x90, 0x90 };
        *(int*)(code + 1) = (int)((DWORD)s.stub - (s.site + 5));
        DWORD old;
        VirtualProtect((void*)s.site, sizeof code, PAGE_EXECUTE_READWRITE, &old);
        memcpy((void*)s.site, code, sizeof code);
        VirtualProtect((void*)s.site, sizeof code, old, &old);
        FlushInstructionCache(GetCurrentProcess(), (void*)s.site, sizeof code);
        Log("Fixes: sector numbers clamped to the world grid at %06lX (things past the map edge)", s.site);
    }
}

static void LogPhysicalAddClamps() {
    const long n = g_physAddClamps;
    if (n != g_physAddLogged && (g_physAddLogged == 0 || n - g_physAddLogged >= 1000)) {   // first, then every 1000
        Log("Fixes: %ld sector numbers past the map edge kept inside the grid (something is beyond the world's edge)", n);
        g_physAddLogged = n;
    }
}

// ---------------------------------------------------------------- empty sector entries
// CPhysical::ProcessCollisionSectorList (0x4B1070) goes through the world sector
// lists a moving entity is in and checks it against each entity there (list node:
// entity +0, next +8). A node can be left with no entity (an entity removed from
// the world without its node being unlinked): the read of the entity's flags
// (0x4B119B: mov cl,[ecx+51h]) then crashed (seen after flying a Sanchez far out
// over the sea). That instruction jumps to a stub: an empty node is skipped, on to
// the next one (0x4B38BB), and counted in the log.
extern "C" { volatile long g_nullSectorItems = 0; }
extern "C" void vcmf_sector_item_guard();
__asm__(".text\n"
        ".globl _vcmf_sector_item_guard\n_vcmf_sector_item_guard:\n"
        "  testl %ecx, %ecx\n  jz 1f\n"
        "  movb 0x51(%ecx), %cl\n  movb $1, %al\n"
        "  pushl $0x4B11A0\n  ret\n"
        "1: lock incl _g_nullSectorItems\n"
        "  pushl $0x4B38BB\n  ret\n"
        // the line-of-sight sector list test (0x4D6EC0, 17 calls from 0x4DA560):
        // 0x4D6EE9 mov cx,[eax+58h]; mov ebp,[ebx+8]  (eax = entity, ebx = node)
        ".globl _vcmf_los_item_guard\n_vcmf_los_item_guard:\n"
        "  movl 8(%ebx), %ebp\n"
        "  testl %eax, %eax\n  jz 1f\n"
        "  movw 0x58(%eax), %cx\n"
        "  pushl $0x4D6EF0\n  ret\n"
        "1: lock incl _g_nullSectorItems\n"
        "  movl %ebp, %ebx\n"
        "  pushl $0x4D6F40\n  ret\n");
extern "C" void vcmf_los_item_guard();

// CWorld::GetIsLineOfSightClear (0x4DA560, 84 callers: scripts, the camera, peds)
// steps from sector to sector along the line without keeping the sector numbers
// inside the grid, so a line with an end off the map read lists past it (garbage
// node at 0x4D6FE7, near the mall). Its per-sector test (0x4D6EC0, cdecl, sector
// first) is entered through a stub: a sector outside the grid holds nothing, so it
// answers "clear" without reading it.
extern "C" { volatile long g_losOutside = 0; }
extern "C" void vcmf_los_sector_guard();
__asm__(".text\n"
        ".globl _vcmf_los_sector_guard\n_vcmf_los_sector_guard:\n"
        "  movl 4(%esp), %eax\n"
        "  cmpl $0x792D30, %eax\n  jb 1f\n"
        "  cmpl $0x7D1530, %eax\n  jae 1f\n"   // 0x792D30 + 80 * 80 * 40
        "  cmpb $0, 0xc(%esp)\n"
        "  pushl $0x4D6EC5\n  ret\n"           // flags of the cmpb kept for its je
        "1: lock incl _g_losOutside\n"
        "  movb $1, %al\n  ret\n");
static long g_losOutsideLogged = 0;
static long g_nullSectorLogged = 0;

// The off-map guard above turned a crash into a freeze: GetIsLineOfSightClear steps
// over every sector row and column between the two ends, so a line with an end at an
// absurd place (a coordinate of 1e9, an entity thrown out of the world) made it call
// the sector test billions of times, each answered "clear" by the guard (freeze of
// 2026-11-19: the game thread in 0x4DAF45..0x4DAFBC and the guard for 6 s, called
// from the population update 0x53E5F0). The function's entry (0x4DA560, cdecl, the
// two CVector pointers first) now goes through a check: an end with a coordinate
// that is not a number or lies past 10,000 units (the map spans -2400..1600) means
// the line is "not clear" at once, the answer for something that is not in the world.
// The prologue the jump replaces (push ebx/esi/edi/ebp; sub esp,260h) runs in the stub.
extern "C" { volatile long g_losAbsurd = 0; }
static volatile long g_losAbsurdShown = 0;
extern "C" int __cdecl vcmf_los_points_sane(const float* a, const float* b, DWORD ret) {
    for (int i = 0; i < 3; i++)
        if (!(a[i] >= -1e4f && a[i] <= 1e4f) || !(b[i] >= -1e4f && b[i] <= 1e4f)) {
            if (InterlockedIncrement(&g_losAbsurdShown) <= 5)
                Log("Fixes: line-of-sight test from %06lX between %.0f %.0f %.0f and %.0f %.0f %.0f answered \"not clear\" "
                    "(an end far outside the world; would have frozen the game)", ret,
                    (double)a[0], (double)a[1], (double)a[2], (double)b[0], (double)b[1], (double)b[2]);
            return 0;
        }
    return 1;
}
extern "C" void vcmf_los_entry_guard();
__asm__(".text\n"
        ".globl _vcmf_los_entry_guard\n_vcmf_los_entry_guard:\n"
        "  pushl (%esp)\n"           // return address, for the log
        "  pushl 0xc(%esp)\n"        // second point
        "  pushl 0xc(%esp)\n"        // first point
        "  call _vcmf_los_points_sane\n"
        "  addl $12, %esp\n"
        "  testl %eax, %eax\n  jz 1f\n"
        "  pushl %ebx\n  pushl %esi\n  pushl %edi\n  pushl %ebp\n"
        "  subl $0x260, %esp\n"
        "  pushl $0x4DA56A\n  ret\n"
        "1: lock incl _g_losAbsurd\n"
        "  xorl %eax, %eax\n  ret\n");
static long g_losAbsurdLogged = 0;

static void InstallSectorItemGuard() {
    static const DWORD SITE = 0x4B119B;
    static const unsigned char orig[] = { 0x8A, 0x49, 0x51, 0xB0, 0x01, 0x80, 0xE1, 0x01 };
    static const unsigned char next[] = { 0x83, 0xBC, 0x24, 0x84, 0x00, 0x00, 0x00, 0x00 };   // cmp [esp+84h],0 at 0x4B38BB
    if (memcmp((void*)SITE, orig, sizeof orig) || memcmp((void*)0x4B38BB, next, sizeof next)) {
        Log("Fixes: collision sector code is not v1.0; empty sector entry guard not installed");
        return;
    }
    unsigned char code[5] = { 0xE9 };
    *(int*)(code + 1) = (int)((DWORD)&vcmf_sector_item_guard - (SITE + 5));
    DWORD old;
    VirtualProtect((void*)SITE, 5, PAGE_EXECUTE_READWRITE, &old);
    memcpy((void*)SITE, code, 5);
    VirtualProtect((void*)SITE, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)SITE, 5);
    Log("Fixes: empty world sector entries skipped in collision checks (crash at 0x4B119B)");

    static const DWORD LOS_SITE = 0x4D6EE9;
    static const unsigned char losOrig[] = { 0x66, 0x8B, 0x48, 0x58, 0x8B, 0x6B, 0x08, 0x66, 0x39, 0xF1 };   // ...; cmp cx,si
    static const unsigned char losNext[] = { 0x85, 0xED, 0x75, 0x9C };                                       // test ebp,ebp; jne at 0x4D6F40
    if (memcmp((void*)LOS_SITE, losOrig, sizeof losOrig) || memcmp((void*)0x4D6F40, losNext, sizeof losNext)) {
        Log("Fixes: line-of-sight sector code is not v1.0; empty entry guard not installed there");
        return;
    }
    unsigned char los[7] = { 0xE9, 0, 0, 0, 0, 0x90, 0x90 };
    *(int*)(los + 1) = (int)((DWORD)&vcmf_los_item_guard - (LOS_SITE + 5));
    VirtualProtect((void*)LOS_SITE, sizeof los, PAGE_EXECUTE_READWRITE, &old);
    memcpy((void*)LOS_SITE, los, sizeof los);
    VirtualProtect((void*)LOS_SITE, sizeof los, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)LOS_SITE, sizeof los);
    Log("Fixes: empty world sector entries skipped in line-of-sight tests (crash at 0x4D6EE9)");

    static const DWORD SEC_SITE = 0x4D6EC0;
    static const unsigned char secOrig[] = { 0x80, 0x7C, 0x24, 0x0C, 0x00, 0x53, 0x56, 0x57 };
    const unsigned char* c = (const unsigned char*)0x4DA6FD;   // a call to it from GetIsLineOfSightClear
    if (memcmp((void*)SEC_SITE, secOrig, sizeof secOrig) || c[0] != 0xE8 || 0x4DA6FD + 5 + *(const int*)(c + 1) != SEC_SITE) {
        Log("Fixes: line-of-sight sector test is not v1.0; off-map guard not installed");
        return;
    }
    unsigned char sec[5] = { 0xE9 };
    *(int*)(sec + 1) = (int)((DWORD)&vcmf_los_sector_guard - (SEC_SITE + 5));
    VirtualProtect((void*)SEC_SITE, sizeof sec, PAGE_EXECUTE_READWRITE, &old);
    memcpy((void*)SEC_SITE, sec, sizeof sec);
    VirtualProtect((void*)SEC_SITE, sizeof sec, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)SEC_SITE, sizeof sec);
    Log("Fixes: line-of-sight tests skip sectors off the map (crash at 0x4D6FE7)");

    static const DWORD LOS_ENTRY = 0x4DA560;
    static const unsigned char entryOrig[] = { 0x53, 0x56, 0x57, 0x55, 0x81, 0xEC, 0x60, 0x02, 0x00, 0x00 };
    if (memcmp((void*)LOS_ENTRY, entryOrig, sizeof entryOrig)) {
        Log("Fixes: GetIsLineOfSightClear (0x4DA560) is not v1.0 or already hooked; absurd-line guard not installed");
        return;
    }
    unsigned char ent[10] = { 0xE9, 0, 0, 0, 0, 0x90, 0x90, 0x90, 0x90, 0x90 };
    *(int*)(ent + 1) = (int)((DWORD)&vcmf_los_entry_guard - (LOS_ENTRY + 5));
    VirtualProtect((void*)LOS_ENTRY, sizeof ent, PAGE_EXECUTE_READWRITE, &old);
    memcpy((void*)LOS_ENTRY, ent, sizeof ent);
    VirtualProtect((void*)LOS_ENTRY, sizeof ent, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)LOS_ENTRY, sizeof ent);
    Log("Fixes: line-of-sight tests with an end past 10,000 units answer \"not clear\" at once (freeze at 0x4DAF45)");
}

// ---------------------------------------------------------------- bullet traces with no lifetime
// CBulletTraces::Render (0x5729F0) fades each trace (array at 0x8118D0, 0x2C each:
// active +0x18, made at +0x1C, lifetime in ms +0x20) by (lifetime - age) * alpha /
// lifetime: "div esi" at 0x572B74 with the lifetime loaded at 0x572B18. A trace
// added with a lifetime of 0 (another mod's bullet traces, 2026-11-19: integer
// divide by zero at 0x572B74) crashed the game. The load goes through a stub: a
// lifetime of 0 reads as 1 ms, so such a trace is drawn faded out and expires.
extern "C" { volatile long g_traceZeroLife = 0; }
extern "C" void vcmf_trace_life();
__asm__(".text\n"
        ".globl _vcmf_trace_life\n_vcmf_trace_life:\n"
        "  movl 0x8118F0(%ebp), %esi\n"
        "  testl %esi, %esi\n  jnz 1f\n"
        "  movl $1, %esi\n"
        "  lock incl _g_traceZeroLife\n"
        "1: ret\n");
static long g_traceZeroLogged = 0;

static void InstallBulletTraceGuard() {
    static const DWORD SITE = 0x572B18;
    static const unsigned char orig[] = { 0x8B, 0xB5, 0xF0, 0x18, 0x81, 0x00 };   // mov esi,[ebp+8118F0h]
    static const unsigned char div[] = { 0xF7, 0xF6 };                             // div esi at 0x572B74
    if (memcmp((void*)SITE, orig, sizeof orig) || memcmp((void*)0x572B74, div, sizeof div)) {
        Log("Fixes: bullet trace code is not v1.0 (or changed by another mod); zero-lifetime guard not installed");
        return;
    }
    unsigned char code[6] = { 0xE8, 0, 0, 0, 0, 0x90 };
    *(int*)(code + 1) = (int)((DWORD)&vcmf_trace_life - (SITE + 5));
    DWORD old;
    VirtualProtect((void*)SITE, sizeof code, PAGE_EXECUTE_READWRITE, &old);
    memcpy((void*)SITE, code, sizeof code);
    VirtualProtect((void*)SITE, sizeof code, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)SITE, sizeof code);
    Log("Fixes: bullet traces with no lifetime are drawn faded out instead of crashing (crash at 0x572B74)");
}

static void LogSectorItems() {
    if (g_traceZeroLife != g_traceZeroLogged && (g_traceZeroLogged == 0 || g_traceZeroLife - g_traceZeroLogged >= 1000)) {
        Log("Fixes: %ld bullet trace draws with a lifetime of 0 (would have crashed at 0x572B74)", (long)g_traceZeroLife);
        g_traceZeroLogged = g_traceZeroLife;
    }
    const long n = g_nullSectorItems;
    if (n != g_nullSectorLogged && (g_nullSectorLogged == 0 || n - g_nullSectorLogged >= 100)) {   // first, then every 100
        Log("Fixes: skipped %ld empty world sector entries in collision checks (would have crashed at 0x4B119B)", n);
        g_nullSectorLogged = n;
    }
    const long m = g_losOutside;
    if (m != g_losOutsideLogged && (g_losOutsideLogged == 0 || m - g_losOutsideLogged >= 1000)) {
        Log("Fixes: %ld line-of-sight steps off the map skipped (would have read past the world grid)", m);
        g_losOutsideLogged = m;
    }
    const long k = g_losAbsurd;
    if (k != g_losAbsurdLogged && (g_losAbsurdLogged == 0 || k - g_losAbsurdLogged >= 100)) {
        Log("Fixes: %ld line-of-sight tests with an end far outside the world answered \"not clear\" (would have frozen)", k);
        g_losAbsurdLogged = k;
    }
}

// ---------------------------------------------------------------- riot in a save
// Riot ([Fixes] ClearRiotOnLoad). The riot cheat (FIGHTFIGHTFIGHT, and trainers'
// riot / "everyone attacks" options) doesn't set a switch: it rewrites the ped types'
// hostility. CPedType::ms_apPedType (0xA0DA64, 23 types, ped.dat) holds for each
// type its flag (+0), threats (+0x18: the types and events it attacks or flees)
// and avoid list (+0x1C). The save keeps the whole table (CPedType::Load 0x530340,
// called at 0x61C362), so a riot saved into a game comes back with every load,
// and nothing in the game turns it off again.
// Each load reads ped.dat first (CGame 0x4A4850, then GenericLoad 0x61A700), so the
// table holds the defaults right before the save is applied: they are kept, and
// after it:
//  - civilians, cops, emergency services, firemen, criminals, prostitutes and the
//    special type get their default threats and avoid list back;
//  - gangs get their ped.dat threats and avoid list back too; only their hostility
//    to the player (PLAYER1, which story missions set) is kept from the save. (An
//    earlier version kept all gang bits: after a riot every gang then hated every
//    gang, its own included, and resaving stored that.)
static const DWORD PEDTYPES = 0xA0DA64, PEDTYPE_LOAD = 0x530340, PEDTYPE_LOAD_SITE = 0x61C362;
static const int kPedTypes = 23;

static bool IsGang(int t) { return t >= 7 && t <= 15; }
static bool IsPlayer(int t) { return t <= 3; }

static void __cdecl PedTypeLoad(unsigned char* buf, unsigned size) {
    unsigned char** types = (unsigned char**)PEDTYPES;
    unsigned flag[kPedTypes], threat[kPedTypes], avoid[kPedTypes];
    bool ok = true;
    for (int i = 0; i < kPedTypes; i++) {
        if (!types[i]) { ok = false; break; }
        flag[i] = *(unsigned*)types[i];
        threat[i] = *(unsigned*)(types[i] + 0x18);
        avoid[i] = *(unsigned*)(types[i] + 0x1C);
    }
    ((void (__cdecl*)(unsigned char*, unsigned))PEDTYPE_LOAD)(buf, size);
    if (!ok || !g_cfg.clearRiotOnLoad) return;
    int fixed = 0;
    for (int i = 0; i < kPedTypes; i++) {
        if (IsPlayer(i)) continue;
        unsigned* t = (unsigned*)(types[i] + 0x18);
        unsigned* a = (unsigned*)(types[i] + 0x1C);
        unsigned nt = *t, na = *a;
        if (IsGang(i)) {
            nt = threat[i] | (*t & flag[0]);   // ped.dat, plus hostility to the player from the save
            na = avoid[i];
        } else {
            nt = threat[i];
            na = avoid[i];
        }
        if (nt != *t || na != *a) {
            if (fixed++ < 23) Log("Fixes: ped type %d hostility from the save %08X / %08X -> %08X / %08X", i, *t, *a, nt, na);
            *t = nt;
            *a = na;
        }
    }
    if (fixed) Log("Fixes: riot / everyone-attacks state saved in this game cleared (%d ped types)", fixed);
}

static bool g_runGunOn = false;   // the patches are on

// ---------------------------------------------------------------- animation with none playing
// Every bone of an animated model is posed each update by one of two callbacks that
// CAnimManager's clump update (0x404850) hands to RenderWare's frame walk: 0x4042D0
// (push at 0x404866) and 0x4042A0 (push at 0x404897). They (and FrameUpdateCallback-
// Skinned 0x403DF0 / 0x4039D0 they call) pose the bone from its list of nodes, one per
// animation playing, null-terminated from +4 of the update data (2nd argument), and
// read the first node without checking the list: a model with no animation at all
// crashed (reading 0x14 at 0x403ED2 or 0x4043B4; seen with a trainer's camera option,
// from CGame::Process through the animation update). Both pushes give our wrappers
// instead: such a bone is left as it is.
static const DWORD kUpdatePushes[] = { 0x404866, 0x404897 };
static DWORD g_frameUpdate[2] = { 0x4042D0, 0x4042A0 };
static long g_noAnimLogged = 0;
static bool NoAnimation(void* data) {
    if (data && ((void**)data)[1]) return false;
    if (g_noAnimLogged < 5) { g_noAnimLogged++; Log("Fixes: a model's bone had no animation playing; left as it was (game would crash)"); }
    return true;
}
static void* __cdecl FrameUpdate0(void* frame, void* data) {
    if (NoAnimation(data)) return frame;
    return ((void* (__cdecl*)(void*, void*))g_frameUpdate[0])(frame, data);
}
static void* __cdecl FrameUpdate1(void* frame, void* data) {
    if (NoAnimation(data)) return frame;
    return ((void* (__cdecl*)(void*, void*))g_frameUpdate[1])(frame, data);
}
static void InstallNoAnimationGuard() {
    void* const wrappers[2] = { (void*)&FrameUpdate0, (void*)&FrameUpdate1 };
    for (int i = 0; i < 2; i++) {
        const unsigned char* p = (const unsigned char*)kUpdatePushes[i];
        if (p[0] != 0x68 || *(const DWORD*)(p + 1) != g_frameUpdate[i]) {
            Log("Fixes: animation update code at %06lX is not v1.0; no-animation guard not installed there", kUpdatePushes[i]);
            continue;
        }
        const DWORD w = (DWORD)wrappers[i];
        WriteCode(kUpdatePushes[i] + 1, &w, 4);
    }
}

// ---------------------------------------------------------------- blend with no model
// CAnimManager::BlendAnimation (0x405640, cdecl: clump, group, anim id, delta) reads
// the clump's animation data (clump + ClumpOffset 0x978798, then [that]) without a
// check: with no clump (a ped whose RW object +0x4C is 0) it crashed at 0x40565E
// reading 0x3C. Its 150 callers are mostly ped code; which one passed the null isn't
// known (no return address was left in the dump). Its entry now goes through a stub:
// with a clump and its animation data it goes on as before; otherwise the caller gets
// a zeroed scratch CAnimBlendAssociation (0x3C bytes: CAnimBlendAssocGroup::
// CopyAnimation 0x401010 does `push 3Ch; call operator new`, then the copy constructor
// 0x401A50) whose hierarchy (+0x14) is a zeroed one with no sequences, so the flags,
// callback and speed the callers write go there, and nothing is played. It is zeroed
// again on each use. Each new return address is logged once, with the ped when the
// caller pushed `[reg+4Ch]` (or loaded it into the register it pushed).
extern "C" {
void* g_blendTramp = nullptr;
void* __cdecl vcmf_blend_no_model(const DWORD* regs);
void vcmf_blend_entry();
}
// pushal: edi esi ebp esp ebx edx ecx eax, then the return address, then the arguments.
__asm__(".text\n"
        ".globl _vcmf_blend_entry\n_vcmf_blend_entry:\n"
        "  pushal\n"
        "  movl 36(%esp), %eax\n  testl %eax, %eax\n  jz 1f\n"
        "  movl 0x978798, %ecx\n  cmpl $0, (%eax,%ecx)\n  je 1f\n"
        "  popal\n  jmp *_g_blendTramp\n"
        "1: pushl %esp\n  call _vcmf_blend_no_model\n  addl $4, %esp\n"
        "  movl %eax, 28(%esp)\n  popal\n  ret\n");

static long g_blendNoModel = 0;
static DWORD g_blendFrom[64];
static int g_nBlendFrom = 0;

// `p` is a ped in the ped pool as it is now (in its array, on a slot, slot in use).
static bool PedInPool(DWORD p) {
    const unsigned char* pool = *(const unsigned char* const*)0x97F2AC;
    if (!pool) return false;
    const DWORD objs = *(const DWORD*)pool;
    const unsigned char* flags = *(const unsigned char* const*)(pool + 4);
    const int size = *(const int*)(pool + 8);
    if (!objs || !flags || p < objs || p >= objs + (DWORD)size * 0x6D8 || (p - objs) % 0x6D8) return false;
    return !(flags[(p - objs) / 0x6D8] & 0x80);
}

// The ped whose +0x4C the caller pushed as the clump, or 0.
static DWORD BlendCallerPed(const DWORD* regs, DWORD ret) {
    const unsigned char* c = (const unsigned char*)ret;
    if (IsBadReadPtr(c - 9, 9) || c[-5] != 0xE8) return 0;
    int base = -1;
    if (c[-8] == 0xFF && (c[-7] & 0xF8) == 0x70 && (c[-7] & 7) != 4 && c[-6] == 0x4C) base = c[-7] & 7;   // push [reg+4Ch]
    else if (c[-6] >= 0x50 && c[-6] <= 0x57 && c[-9] == 0x8B && (c[-8] & 0xC0) == 0x40 && ((c[-8] >> 3) & 7) == c[-6] - 0x50 &&
             (c[-8] & 7) != 4 && c[-7] == 0x4C)
        base = c[-8] & 7;                                                                             // mov r,[base+4Ch]; push r
    if (base < 0 || base == 4) return 0;
    const DWORD p = regs[7 - base];   // pushal order: register number n is at 7 - n
    return PedInPool(p) ? p : 0;
}

extern "C" void* __cdecl vcmf_blend_no_model(const DWORD* regs) {
    alignas(16) static unsigned char scratch[0x3C], hierarchy[0x40];
    const DWORD ret = regs[8];
    const int group = (int)regs[10], anim = (int)regs[11];
    memset(scratch, 0, sizeof scratch);
    memset(hierarchy, 0, sizeof hierarchy);
    *(void**)(scratch + 0x14) = hierarchy;          // m_pHierarchy: no sequences, length 0
    *(short*)(scratch + 0x0E) = (short)group;       // m_nGroupId
    *(short*)(scratch + 0x2C) = (short)anim;        // m_nAnimId
    g_blendNoModel++;
    bool seen = false;
    for (int i = 0; i < g_nBlendFrom && !seen; i++) seen = g_blendFrom[i] == ret;
    if (!seen && g_nBlendFrom < 64) {
        g_blendFrom[g_nBlendFrom++] = ret;
        const DWORD clump = regs[9];
        const DWORD ped = BlendCallerPed(regs, ret);
        char who[96] = "";
        if (ped)
            snprintf(who, sizeof who, ", ped %08lX model %d state %d", ped, *(const short*)(ped + 0x5C), *(const int*)(ped + 0x244));
        Log("Anim: BlendAnimation with no model from %06lX, anim %d group %d (clump %08lX%s); a blank animation returned, nothing played (%ld so far)",
            ret, anim, group, clump, clump ? ", no animation data" : "", g_blendNoModel);
        if (who[0]) Log("Anim:   the caller passed ped+0x4C%s", who);
    }
    return scratch;
}

static void InstallBlendAnimationGuard() {
    static const DWORD BLEND = 0x405640;
    static const unsigned char orig[] = { 0x53, 0x56, 0x57, 0x55, 0x83, 0xEC, 0x20 };   // push ebx..ebp; sub esp, 20h
    static const unsigned char next[] = { 0x8B, 0x7C, 0x24, 0x34, 0x8B, 0x44, 0x24, 0x3C, 0x03, 0x3D, 0x98, 0x87, 0x97, 0x00 };
    if (memcmp((void*)BLEND, orig, sizeof orig) || memcmp((void*)(BLEND + 7), next, sizeof next)) {
        Log("Anim: CAnimManager::BlendAnimation is not v1.0 (or hooked by another mod); no-model guard not installed");
        return;
    }
    unsigned char* t = (unsigned char*)VirtualAlloc(nullptr, 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!t) return;
    memcpy(t, orig, sizeof orig);
    t[7] = 0xE9;
    *(int*)(t + 8) = (int)((BLEND + 7) - ((DWORD)t + 12));
    FlushInstructionCache(GetCurrentProcess(), t, 16);
    g_blendTramp = t;
    unsigned char jmp[7] = { 0xE9, 0, 0, 0, 0, 0x90, 0x90 };
    *(int*)(jmp + 1) = (int)((DWORD)&vcmf_blend_entry - (BLEND + 5));
    WriteCode(BLEND, jmp, sizeof jmp);
    Log("Anim: BlendAnimation with no model returns a blank animation instead of crashing (crash at 0x40565E)");
}

// ---------------------------------------------------------------- nearby-ped list
// The whole scene washed red, pink or orange, lamps and their coronas changing
// colour, from update to update, with a big crowd (150 trainer bodyguards on Ocean
// Drive). CPed::BuildPedLists (0x50D4B0, thiscall; from CPed::ProcessControl at
// 0x505814 and from 0x531D68; runs for a ped when (ped+0x5A + frame counter
// 0xA0D898) & 15 == 0) gathers every ped in the world sectors around the ped
// into a static list of 50 pointers at 0x7D5230, its length a word at 0xA10A3E:
//   0x50D6CA / 0x50D6E0  movzx eax, word [0xA10A3E]
//   0x50D6D2 / 0x50D6E7  mov [eax*4 + 0x7D5230], edx      (no length check)
//   0x50D6EE             inc word [0xA10A3E]
//   0x50D735             mov dword [eax*4 + 0x7D5230], 0  (end marker)
//   0x50D74D             push 0x7D5230; call 0x50D120     (SortPeds, by distance)
//   0x50D780             mov eax, [edx*4 + 0x7D5230]      (the 10 nearest -> ped+0x56C)
// With more than 49 peds around, the list runs on over the game's next globals:
//   0x7D52F8  4 dwords of CCarCtrl (cleared at 0x4348BB)       list places 50-53
//   0x7D5308  time cycle "light on ground" table, 24 x 7 bytes  places 54-95
//             (timecyc.dat load 0x4D0A6C; mixed at 0x4CEFED into 0x9B6A70, the
//             brightness of street lamps, traffic lights and their coronas)
//   0x7D53B0  time cycle object ambient RED, 24 x 7 bytes        places 96-137
//             (load 0x4D089D; mixed at 0x4CECDC into 0xA0D8CC: red of the ambient
//             light on every ped, vehicle and object)
//   0x7D5458  20-float random table of 0x5C8110                  places 138-157
//   0x7D54A8  CParticle's 20-float random table (0x563CC0)      places 158-177
//   0x7D54F8  CRenderer's visible entity list                    places 178-
// The ped pointers' bytes became the object ambient red and the lamps' brightness
// (-1199 % .. +1199 % in the log), different for every ped the list was built for:
// a new colour every update. The write watch on 0x7D5360 (time cycle, place 76)
// caught exactly these: 0x50D6EE (after the store), 0x50D311 (SortPeds' swap) and
// 0x50D740 (the end marker).
// The list now lives here, with 512 places, and never grows past 511 (end marker
// included): every reference to 0x7D5230 points here, and the length's increment
// goes through a stub that stops at 511 and counts.
#define VCMF_PED_LIST_CAP 512
#define VCMF_STR2(x) #x
#define VCMF_STR(x) VCMF_STR2(x)
extern "C" {
    void* g_nearPedList[VCMF_PED_LIST_CAP];
    volatile long g_pedListPastGame = 0;   // lists longer than the game's 50 places
    volatile long g_pedListFull = 0;       // peds left out at 511
    volatile unsigned g_pedListLongest = 0;
}
extern "C" void vcmf_ped_list_inc();
__asm__(".text\n"
        ".globl _vcmf_ped_list_inc\n_vcmf_ped_list_inc:\n"
        "  cmpw $(" VCMF_STR(VCMF_PED_LIST_CAP) " - 1), 0xA10A3E\n  jae 2f\n"
        "  incw 0xA10A3E\n"
        "  movzwl 0xA10A3E, %eax\n"            // eax is free here: reloaded before its next use
        "  cmpl _g_pedListLongest, %eax\n  jbe 1f\n  movl %eax, _g_pedListLongest\n"
        "1: cmpl $50, %eax\n  jne 3f\n  lock incl _g_pedListPastGame\n"
        "3: pushl $0x50D6F5\n  ret\n"
        "2: lock incl _g_pedListFull\n"
        "  pushl $0x50D6F5\n  ret\n");

static bool g_pedListOk = false;
static void InstallNearPedList() {
    struct Ref { DWORD at; unsigned char bytes[11]; int len, disp; };   // disp: offset of the 0x7D5230 operand
    static const Ref refs[] = {
        { 0x50D6D2, { 0x89, 0x14, 0x85, 0x30, 0x52, 0x7D, 0x00 }, 7, 3 },                         // mov [eax*4+list], edx
        { 0x50D6E7, { 0x89, 0x14, 0x85, 0x30, 0x52, 0x7D, 0x00 }, 7, 3 },                         // mov [eax*4+list], edx
        { 0x50D735, { 0xC7, 0x04, 0x85, 0x30, 0x52, 0x7D, 0x00, 0x00, 0x00, 0x00, 0x00 }, 11, 3 }, // mov [eax*4+list], 0
        { 0x50D74D, { 0x68, 0x30, 0x52, 0x7D, 0x00 }, 5, 1 },                                     // push list (SortPeds)
        { 0x50D780, { 0x8B, 0x04, 0x95, 0x30, 0x52, 0x7D, 0x00 }, 7, 3 },                         // mov eax, [edx*4+list]
    };
    static const unsigned char inc[] = { 0x66, 0xFF, 0x05, 0x3E, 0x0A, 0xA1, 0x00 };             // inc word [0xA10A3E]
    static const unsigned char next[] = { 0x8B, 0x7F, 0x08 };                                    // 0x50D6F5: mov edi, [edi+8]
    bool ok = !memcmp((void*)0x50D6EE, inc, sizeof inc) && !memcmp((void*)0x50D6F5, next, sizeof next);
    for (const Ref& r : refs) ok = ok && !memcmp((void*)r.at, r.bytes, r.len);
    if (!ok) {
        Log("Peds: nearby-ped list code (0x50D4B0) is not v1.0 or was changed by another mod; its 50-place list not moved");
        return;
    }
    const DWORD list = (DWORD)g_nearPedList;
    for (const Ref& r : refs) WriteCode(r.at + r.disp, &list, 4);
    unsigned char jmp[7] = { 0xE9, 0, 0, 0, 0, 0x90, 0x90 };
    *(int*)(jmp + 1) = (int)((DWORD)&vcmf_ped_list_inc - (0x50D6EE + 5));
    WriteCode(0x50D6EE, jmp, sizeof jmp);
    g_pedListOk = true;
    Log("Peds: nearby-ped list moved to a %d-place list (the game's 50 places at 0x7D5230 ran over the time cycle's "
        "lamp brightness and object ambient red with 50+ peds around: scene and coronas changing colour every update)",
        VCMF_PED_LIST_CAP);
}

// Every 10 s while it acts (20 lines at most): how many lists would have run past
// the game's 50 places, the longest, and any ped left out at 511.
static void NearPedListAfterPresent() {
    static long seen = -1;
    static int lines = 0;
    static DWORD at = 0;
    if (!g_pedListOk || lines >= 20) return;
    const long n = g_pedListPastGame + g_pedListFull;
    if (n == seen) return;
    const DWORD now = GetTickCount();
    if (seen >= 0 && now - at < 10000) return;
    at = now;
    seen = n;
    if (!n) return;
    lines++;
    Log("Peds: nearby-ped lists longer than the game's 50 places: %ld so far (longest %u, peds left out at %d: %ld); "
        "kept in VCModernFix's list, the time cycle tables at 0x7D5308 / 0x7D53B0 untouched",
        g_pedListPastGame, g_pedListLongest, VCMF_PED_LIST_CAP - 1, g_pedListFull);
}

void InstallGameplay() {
    InstallNearPedList();
    InstallSectorClamp();
    InstallPhysicalAddClamp();
    InstallWaterClamp();
    InstallSectorItemGuard();
    InstallBulletTraceGuard();
    InstallNoAnimationGuard();
    InstallAngleWrapGuard();
    InstallBlendAnimationGuard();
    // Each part below is installed even when its setting is off: the Modern Fix menu
    // (fixmenu.cpp) turns them on and off while playing (GameplaySettingsChanged).
    {
        const unsigned char* p = (const unsigned char*)PEDTYPE_LOAD_SITE;
        const bool ok = p[0] == 0xE8 && PEDTYPE_LOAD_SITE + 5 + *(const int*)(p + 1) == PEDTYPE_LOAD
                     && *(DWORD*)(PEDTYPE_LOAD + 0x12 + 3) == PEDTYPES;   // mov edx,[ecx*4+0xA0DA64] at 0x530352
        if (ok) {
            unsigned char call[5] = { 0xE8 };
            *(int*)(call + 1) = (int)((DWORD)&PedTypeLoad - (PEDTYPE_LOAD_SITE + 5));
            DWORD old;
            VirtualProtect((void*)PEDTYPE_LOAD_SITE, 5, PAGE_EXECUTE_READWRITE, &old);
            memcpy((void*)PEDTYPE_LOAD_SITE, call, 5);
            VirtualProtect((void*)PEDTYPE_LOAD_SITE, 5, old, &old);
            FlushInstructionCache(GetCurrentProcess(), (void*)PEDTYPE_LOAD_SITE, 5);
            Log("Fixes: riot saved in a game is cleared on load (%s)", g_cfg.clearRiotOnLoad ? "on" : "off");
        } else {
            Log("Fixes: save loading code is not v1.0; riot clear not installed");
        }
    }
    {
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
            if (g_cfg.runAndGun) FreeLegsInFireAnims();   // blocks already loaded keep their old links until reloaded
            // The torso turn, after each clump's animation update in CWorld::Process.
            // Whatever the calls lead to now (the game's function or another mod's
            // hook) is called first, then the turn.
            bool callsOk = true;
            for (DWORD s : kUpdateAnimCalls) callsOk = callsOk && *(const unsigned char*)s == 0xE8;
            static const unsigned char kUpdateCode[] = { 0x53, 0x56, 0x57, 0x55, 0x83, 0xEC, 0x40 };   // at 0x404690
            if (callsOk && !memcmp((void*)UPDATE_ANIMS, kUpdateCode, sizeof kUpdateCode)
                && *(DWORD*)0x4046A3 == CLUMP_OFFSET && *(DWORD*)0x4046B5 == 0x6F7000) {   // add eax,[ClumpOffset]; mov [gpAnimBlendClump],ebx
                void* const hooks[2] = { (void*)&UpdateAnimsHook0, (void*)&UpdateAnimsHook1 };
                for (int i = 0; i < 2; i++) {
                    const DWORD s = kUpdateAnimCalls[i];
                    g_updateAnims[i] = (UpdateAnims_t)(s + 5 + *(const int*)(s + 1));
                    unsigned char call[5] = { 0xE8 };
                    *(int*)(call + 1) = (int)((DWORD)hooks[i] - (s + 5));
                    WriteCode(s, call, 5);
                }
                g_torsoOk = true;
                Log("Weapons: two-handed fire animations turn the torso (Spine1) by their own Root turn while the legs move");
            } else {
                Log("Weapons: animation update code is not v1.0; torso turn not installed (two-handed guns may point sideways while moving)");
            }
        }
        static const unsigned char sprintFire[] = { 0x0F, 0x84, 0x94, 0x01, 0x00, 0x00 };   // at 0x534B18
        static const unsigned char breakout[] = { 0x74, 0x3E };                              // at 0x52BDE0
        if (g_runGunOk && !memcmp((void*)SPRINT_FIRE_JE, sprintFire, 6) && !memcmp((void*)SPRINT_BREAKOUT_JE, breakout, 2)) {
            g_sprintOk = true;
            Log("Weapons: firing while sprinting");
        }
        if (!memcmp((void*)0x5349D8, kScopeCode, sizeof kScopeCode)) {
            for (int i = 0; i < 3; i++) memcpy(g_scopeOrig[i], (void*)kScopeJumps[i], 2);
            g_scopeOk = true;
            Log("Weapons: M4, Ruger and M60 aim in third person (move while aiming)");
        } else {
            Log("Weapons: aim code is not v1.0; M4/Ruger/M60 keep the first-person scope");
        }
        if (g_cfg.runAndGun) RunAndGunPatches(true);
        g_runGunOn = g_cfg.runAndGun;
        Log(g_runGunOk ? "Weapons: two-handed and heavy guns fire on the move (own animations, legs free) (%s)"
                       : "Weapons: weapon info is not v1.0; run and gun not installed (%s)", g_cfg.runAndGun ? "on" : "off");
    }
    {
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
    {
        // fld [ebx+0x5B0] in TankControl and fadd/fstp [ebx+0x5B0] in FireTruckControl
        static const unsigned char tank[] = { 0xD9, 0x83, 0xB0, 0x05, 0x00, 0x00 };
        static const unsigned char truck[] = { 0xD8, 0x83, 0xB0, 0x05, 0x00, 0x00 };
        g_aimOk = !memcmp((void*)0x58659A, tank, sizeof tank) && !memcmp((void*)0x57AB90, truck, sizeof truck);
        Log(g_aimOk ? "Vehicles: Rhino turret and fire truck cannon aim with the camera (mouse)"
                    : "Vehicles: turret code is not v1.0; mouse aim not installed");
    }
    {
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

// The Modern Fix menu changed a setting: the code patches follow it now.
void GameplaySettingsChanged() {
    if (!(g_runGunOk || g_scopeOk) || g_runGunOn == g_cfg.runAndGun) return;
    g_runGunOn = g_cfg.runAndGun;
    RunAndGunPatches(g_runGunOn);
    if (g_runGunOn) {
        if (g_runGunOk) FreeLegsInFireAnims();   // animations loaded already keep their links until loaded again
    } else {
        for (int i = 0; i < 2; i++)
            if (g_heavyHad[i]) *WeaponFlags(kHeavyWeapons[i]) |= 0x200u;   // heavy guns slow you down again
    }
}
