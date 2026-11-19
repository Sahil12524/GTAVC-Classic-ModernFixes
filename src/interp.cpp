// Camera and object interpolation between game ticks.
//
// Every tick records the camera and the matrix of every ped, vehicle and object.
// A drawn frame at time `alpha` (0..1) into the current tick interval shows them at
// prev + (cur - prev) * alpha, so drawing runs one tick behind the simulation.
// Positions are lerped; the forward and up axes are lerped, re-normalised and
// re-orthogonalised, and right is rebuilt from them with the handedness of the
// real matrix. Anything that jumped (teleport, respawn, camera cut, more than 90
// degrees in one tick) is drawn where it really is.
//
// Objects: the CMatrix (entity+4) gets the blended matrix, and the RenderWare
// frame matrix gets the same movement as an offset (blended * inverse(real)), not
// a copy: some objects keep extra rotation only in the RenderWare matrix (spinning
// pickups turn there in PreRender). CEntity::UpdateRwFrame (0x489360) marks the
// frame dirty, and for skinned clumps (peds, cutscene objects)
// CEntity::UpdateRpHAnim (0x489330) rebuilds the bone matrices from the moved root,
// both when moving and when putting back (game logic reads bone positions). The camera is moved the
// way CCamera::Process does it (0x46DCC1..0x46DD4F): CalculateDerivedValues
// (0x46C3B6) for the frustum, then copy into the RenderWare camera frame,
// RwMatrixUpdate, RwFrameUpdateObjects, RwFrameOrthoNormalize. InterpRestore puts
// every byte it changed back.
#include "interp.h"
#include "config.h"
#include "population.h"
#include "mouselook.h"
#include "freecam.h"
#include "timing.h"
#include <windows.h>
#include <math.h>
#include <string.h>

// ---------------------------------------------------------------- game (v1.0)
static unsigned char* const Camera = (unsigned char*)0x7E4688;   // TheCamera; CMatrix at +0
static const DWORD CAM_DERIVED   = 0x46C3B6;   // CCamera::CalculateDerivedValues (thiscall)
static const DWORD ENT_UPDATE_RW = 0x489360;   // CEntity::UpdateRwFrame (thiscall)
static const DWORD ENT_UPDATE_SKIN = 0x489330; // CEntity::UpdateRpHAnim (thiscall; clumps only)
static const DWORD RW_MAT_UPDATE = 0x6437B0;   // RwMatrixUpdate(RwMatrix*)
static const DWORD RW_FRAME_UPD  = 0x644D00;   // RwFrameUpdateObjects(RwFrame*)
static const DWORD RW_FRAME_ORTH = 0x645320;   // RwFrameOrthoNormalize(RwFrame*)

struct Pool { void* entries; unsigned char* flags; int size; int firstFree; };   // CPool<T>
struct PoolDef { DWORD ptr; unsigned stride; int maxSlots; bool moves; };
static const PoolDef kPools[] = {
    { 0x97F2AC, 0x6D8, PED_POOL_SLOTS, true  },   // peds (population.h)
    { 0xA0FDE4, 0x5DC, 110, true  },   // vehicles
    { 0x94DBE0, 0x1A0, 460, true  },   // objects
    { 0x97F240, 0x64, 7000, false },   // buildings
    { 0x978834, 0x64, 1250, false },   // treadables
    { 0x97F25C, 0x68, 2340, false },   // dummies
};
static const int kMovingPools = 3;
static const int kMaxMoving = PED_POOL_SLOTS + 110 + 460;
static const float kMaxJump = 10.0f;   // metres per tick; faster = teleport

static Pool* GetPool(const PoolDef& d) { return *(Pool**)d.ptr; }

// ---------------------------------------------------------------- math
struct V3 { float x, y, z; };
static V3 operator+(V3 a, V3 b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
static V3 operator-(V3 a, V3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
static V3 operator*(V3 a, float s) { return { a.x * s, a.y * s, a.z * s }; }
static float Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static V3 Cross(V3 a, V3 b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
static float Len(V3 a) { return sqrtf(Dot(a, a)); }
static V3 Lerp(V3 a, V3 b, float t) { return a + (b - a) * t; }

struct Frame { V3 right, fwd, up, pos; };   // CMatrix: right +0, fwd +0x10, up +0x20, pos +0x30

static Frame ReadMatrix(const unsigned char* m) {
    Frame f;
    memcpy(&f.right, m + 0x00, 12); memcpy(&f.fwd, m + 0x10, 12);
    memcpy(&f.up, m + 0x20, 12);    memcpy(&f.pos, m + 0x30, 12);
    return f;
}
static void WriteMatrix(unsigned char* m, const Frame& f) {
    memcpy(m + 0x00, &f.right, 12); memcpy(m + 0x10, &f.fwd, 12);
    memcpy(m + 0x20, &f.up, 12);    memcpy(m + 0x30, &f.pos, 12);
}

// false: too far apart to blend (draw `b` as it is)
static bool Blend(const Frame& a, const Frame& b, float t, Frame& out) {
    if (Len(b.pos - a.pos) > kMaxJump) return false;
    float lf = Len(b.fwd), lu = Len(b.up), lr = Len(b.right);
    float laf = Len(a.fwd), lau = Len(a.up);
    if (lf < 1e-4f || lu < 1e-4f || lr < 1e-4f || laf < 1e-4f || lau < 1e-4f) return false;
    if (Dot(a.fwd, b.fwd) <= 0.0f || Dot(a.up, b.up) <= 0.0f) return false;   // > 90 degrees
    V3 f = Lerp(a.fwd * (1.0f / laf), b.fwd * (1.0f / lf), t);
    V3 u = Lerp(a.up * (1.0f / lau), b.up * (1.0f / lu), t);
    float n = Len(f);
    if (n < 1e-4f) return false;
    f = f * (1.0f / n);
    u = u - f * Dot(u, f);
    n = Len(u);
    if (n < 1e-4f) return false;
    u = u * (1.0f / n);
    V3 r = Cross(f, u);
    if (Dot(r, b.right) < 0.0f) r = r * -1.0f;   // keep the real matrix's handedness
    out.fwd = f * lf;
    out.up = u * lu;
    out.right = r * lr;
    out.pos = Lerp(a.pos, b.pos, t);
    return true;
}

// ---------------------------------------------------------------- skinned poses
// RpHAnimHierarchy: +4 numNodes, +0x20 currentAnim (interpolator). In this
// RenderWare build the interpolator's per-bone frames start at +0x48, stride at
// +0x24, apply callback at +0x38 (RpHAnimHierarchyUpdateMatrices, 0x6464A1..).
// With the standard callback (0x66A8A0) a frame holds the bone's rotation
// quaternion at +8 and translation at +0x18; anything else is left alone.
static const DWORD IS_CLUMP_SKINNED = 0x57F580;   // cdecl(RpClump*)
static const DWORD SKIN_HIERARCHY   = 0x57F250;   // GetAnimHierarchyFromSkinClump, cdecl
static const DWORD STD_APPLY_CB     = 0x66A8A0;
static const int kMaxBones = 64;
struct Pose { int n; float q[kMaxBones][4]; float t[kMaxBones][3]; };

// Bone frames of a skinned entity, or nullptr. *count = bones.
// IS_CLUMP_SKINNED and SKIN_HIERARCHY each run RpClumpForAllAtomics (0x640D00) with a
// callback that stops at the first atomic (0x57F9B0, 0x57F280: return 0), then read one
// plugin field (0x649960, 0x649950): a few dozen instructions per call, counted in the
// Timing line (TC_BONE_LOOKUPS) rather than cached.
static unsigned char* BoneFrames(unsigned char* e, int* count) {
    unsigned char* rwObject = *(unsigned char**)(e + 0x4C);
    if (!rwObject || rwObject[0] != 2) return nullptr;   // rpCLUMP
    TimingCount(TC_BONE_LOOKUPS);
    if (!((char(__cdecl*)(void*))IS_CLUMP_SKINNED)(rwObject)) return nullptr;
    unsigned char* hier = ((unsigned char*(__cdecl*)(void*))SKIN_HIERARCHY)(rwObject);
    if (!hier) return nullptr;
    int n = *(int*)(hier + 4);
    unsigned char* anim = *(unsigned char**)(hier + 0x20);
    if (!anim || n <= 0 || n > kMaxBones) return nullptr;
    if (*(DWORD*)(anim + 0x38) != STD_APPLY_CB || *(int*)(anim + 0x24) != 0x24) return nullptr;
    *count = n;
    return anim + 0x48;
}

static void ReadPose(unsigned char* frames, int n, Pose& p) {
    p.n = n;
    for (int i = 0; i < n; i++) {
        memcpy(p.q[i], frames + i * 0x24 + 0x08, 16);
        memcpy(p.t[i], frames + i * 0x24 + 0x18, 12);
    }
}
static void WritePose(unsigned char* frames, const Pose& p) {
    for (int i = 0; i < p.n; i++) {
        memcpy(frames + i * 0x24 + 0x08, p.q[i], 16);
        memcpy(frames + i * 0x24 + 0x18, p.t[i], 12);
    }
}
static void BlendPose(const Pose& a, const Pose& b, float t, Pose& out) {
    out.n = b.n;
    for (int i = 0; i < b.n; i++) {
        const float* qa = a.q[i]; const float* qb = b.q[i];
        float s = (qa[0] * qb[0] + qa[1] * qb[1] + qa[2] * qb[2] + qa[3] * qb[3]) < 0.0f ? -1.0f : 1.0f;
        float q[4], len = 0.0f;
        for (int k = 0; k < 4; k++) { q[k] = qa[k] * s * (1.0f - t) + qb[k] * t; len += q[k] * q[k]; }
        len = len > 1e-12f ? 1.0f / sqrtf(len) : 0.0f;
        for (int k = 0; k < 4; k++) out.q[i][k] = len > 0.0f ? q[k] * len : qb[k];
        for (int k = 0; k < 3; k++) out.t[i][k] = a.t[i][k] + (b.t[i][k] - a.t[i][k]) * t;
    }
}

static void ParticlesCapture();
static void RubbishCapture();

// ---------------------------------------------------------------- game clock
// Drawing code reads CTimer::m_snTimeInMilliseconds (0x974B2C) for water, blinking
// lights, corona flicker, HUD and radar blinking. It also computes ages as
// "now - timestamp" compared unsigned (18 sites, e.g. 0x54A780, 0x568050): a clock
// earlier than a timestamp set by the latest tick wraps to a huge age, which drew
// bullet traces and smoke wrongly and could remove entries. So the clock is never
// earlier than the latest tick: tick frames draw with the real time, exactly like
// the original game; redraw frames run it forward from there by the same fraction
// of a tick interval as everything else is blended, and the real value is put
// back after. Clock-driven animation leads positions by at most one tick.
static int* const GameTimeMs = (int*)0x974B2C;
static int g_timeCur = 0, g_timePrev = 0, g_timeReal = 0;
static bool g_timeApplied = false;

// ---------------------------------------------------------------- history
// The last two ticks' records live in two buffers each (slots, poses), and each tick
// swaps which one is "cur" instead of copying cur into prev (1.4 MB of poses and
// 44 KB of slots a tick). The result is the same: prev then holds exactly the bytes
// the copy would have put there (the buffer that was cur), and CaptureMoving writes
// `valid` and the pose count of every one of the kMaxMoving slots of cur before
// anything reads it. Only fields of an invalid slot, or bones past a pose's count,
// keep older values than the copy left - and nothing reads those: every reader tests
// `valid` first (FindSlot, InterpApply, InterpPartsCapture / Apply, CamCheck through
// FindSlot) and reads only the first n bones (BlendPose, WritePose, after pp.n == pc.n).
struct Slot { void* ent; unsigned char id; bool valid; Frame m; };
static Slot g_slotsA[kMaxMoving], g_slotsB[kMaxMoving];
static Pose g_posesA[kMaxMoving], g_posesB[kMaxMoving];   // n == 0: none
static Slot* g_cur = g_slotsA;
static Slot* g_prev = g_slotsB;
static Pose* g_poseCur = g_posesA;
static Pose* g_posePrev = g_posesB;
static Frame g_camCur, g_camPrev;
static bool g_haveCur = false, g_havePrev = false;

void InterpReset() { g_haveCur = g_havePrev = false; }

static void CaptureMoving(Slot* cur, Pose* poses) {
    int base = 0;
    for (int p = 0; p < kMovingPools; p++) {
        const PoolDef& d = kPools[p];
        Pool* pool = GetPool(d);
        for (int i = 0; i < d.maxSlots; i++) {
            Slot& s = cur[base + i];
            s.valid = false;
            poses[base + i].n = 0;
            if (!pool || i >= pool->size || (pool->flags[i] & 0x80)) continue;
            unsigned char* e = (unsigned char*)pool->entries + i * d.stride;
            unsigned char* rwMat = *(unsigned char**)(e + 4 + 0x40);   // CMatrix::m_attachment
            if (!rwMat) continue;
            s.ent = e;
            s.id = pool->flags[i] & 0x7F;
            s.m = ReadMatrix(rwMat);   // what is drawn (the RenderWare matrix)
            s.valid = true;
            int bones;
            if (unsigned char* frames = BoneFrames(e, &bones)) ReadPose(frames, bones, poses[base + i]);
        }
        base += d.maxSlots;
    }
}

// Yaw / pitch (radians) of a camera's front, as the mouse look measures turns
// (on foot through MouseTickCaptured, in a vehicle in MouseTurnCamera).
static bool FrontAngles(const V3& fwd, float* yaw, float* pitch) {
    const float l = Len(fwd);
    if (!(l > 1e-4f)) return false;
    float s = fwd.z / l;
    s = s < -1.0f ? -1.0f : s > 1.0f ? 1.0f : s;
    *yaw = atan2f(fwd.y, fwd.x);
    *pitch = asinf(s);
    return true;
}

static unsigned g_tickNo = 0;   // captures so far (per-tick decisions in MouseTurnCamera)

void InterpCapture() {
    { Slot* t = g_prev; g_prev = g_cur; g_cur = t; }
    { Pose* t = g_posePrev; g_posePrev = g_poseCur; g_poseCur = t; }
    g_camPrev = g_camCur;
    g_havePrev = g_haveCur;

    CaptureMoving(g_cur, g_poseCur);
    g_camCur = ReadMatrix(Camera);
    {
        // the game's own camera turn this tick, measured on the captured matrices
        float y = 0.0f, p = 0.0f, py = 0.0f, pp = 0.0f;
        const bool ok = g_havePrev && FrontAngles(g_camCur.fwd, &y, &p) && FrontAngles(g_camPrev.fwd, &py, &pp);
        MouseTickCaptured(ok, y, p, py, pp);
        g_tickNo++;
    }
    g_timePrev = g_timeCur;
    g_timeCur = *GameTimeMs;
    ParticlesCapture();
    RubbishCapture();
    g_haveCur = true;
}

static bool UnitAxes(const Frame& m, V3& r, V3& f, V3& u) {
    float lr = Len(m.right), lf = Len(m.fwd), lu = Len(m.up);
    if (lr < 1e-4f || lf < 1e-4f || lu < 1e-4f) return false;
    r = m.right * (1.0f / lr); f = m.fwd * (1.0f / lf); u = m.up * (1.0f / lu);
    return true;
}

// Mouse look each frame (mouselook.cpp, freecam.cpp). While the mouse turns the
// camera, the drawn camera isn't simply blended: it is the latest update's camera
// turned by the mouse movement the game hasn't used yet (minus the part of the
// latest update's own turning not reached yet, see mouselook.cpp), around the
// point it looks at, next to the player or vehicle. Where the player / vehicle is
// relative to the camera (in the camera's own axes) is blended between the last
// two updates, like everything else: the game eases the camera's distance and
// height from update to update. At the next update the game turns its camera by
// that same movement, so the view carries on from where it is.
static int FindSlot(const void* ent) {
    if (!ent) return -1;
    for (int i = 0; i < kMaxMoving; i++)
        if (g_cur[i].valid && g_cur[i].ent == ent && g_prev[i].valid && g_prev[i].ent == ent && g_cur[i].id == g_prev[i].id)
            return i;
    return -1;
}

static float Wrap(float a) {
    while (a > 3.14159265f) a -= 6.2831853f;
    while (a < -3.14159265f) a += 6.2831853f;
    return a;
}

// Why the latest frame wasn't mouse-turned (CamCheck): 1 no camera axes, 2 camera
// transition, 3 vehicle camera not in use / no counts, 4 big self turn, 5 big turn,
// 6 no player / vehicle record, 7 player / vehicle jumped, 8 too far.
static int g_mtReason = 0;

static bool MouseTurnCamera(float alpha, Frame& cam) {
    g_mtReason = 0;
    MouseFrameBegin();
    float dYaw = 0.0f, dPitch = 0.0f;
    const void* ent = nullptr;
    V3 r, f, u, pr, pf, pu;
    if (!UnitAxes(g_camCur, r, f, u) || !UnitAxes(g_camPrev, pr, pf, pu)) { g_mtReason = 1; return false; }
    // In a camera transition (getting in / out of a vehicle...) the drawn camera is a
    // mix of two cameras, not the active one: drawn blended until it is the active one.
    {
        const unsigned char act = *(const unsigned char*)0x7E46FE;
        const float* front = (const float*)(0x7E4810 + act * 0x1CC + 0x168);   // CCam Front
        const float c = front[0] * f.x + front[1] * f.y + front[2] * f.z;
        if (!(c > 0.9994f)) {   // more than 2 degrees apart
            static unsigned logs = 0;
            if (logs < 3) { logs++; Log("MouseLook: camera in a transition; drawn blended until it ends"); }
            { g_mtReason = 2; return false; }
        }
    }
    // A cut / snap in the latest tick's own turning is decided once per tick and
    // held for all its frames: switching between mouse-turned and blended inside a
    // tick was a jump each time.
    if (MouseTickSnap()) { g_mtReason = 4; return false; }
    static unsigned vehSnapTick = ~0u;
    if (vehSnapTick == g_tickNo) { g_mtReason = 4; return false; }
    if (FrameMouseTurn(alpha, &dYaw, &dPitch)) {
        ent = ((const unsigned char* (__cdecl*)())0x4BC120)();   // FindPlayerPed
    } else {
        float mx, my, lastYaw, lastPitch;
        const unsigned char* veh = nullptr;
        if (!MouseFrameCounts(&mx, &my) || !FreeCamFrameTurn(mx, my, &dYaw, &dPitch, &lastYaw, &lastPitch, &veh)) { g_mtReason = 3; return false; }
        // the latest update's own turning (the vehicle turning, the camera swinging
        // back), beyond its mouse movement, is drawn blended: measured on the
        // captured matrices, the same way as on foot (MouseTickCaptured)
        float cy, cp, py, pp;
        FrontAngles(f, &cy, &cp);
        FrontAngles(pf, &py, &pp);
        const float selfYaw = Wrap(cy - py - lastYaw);
        const float selfPitch = cp - pp - lastPitch;
        if (!(fabsf(selfYaw) < 0.8f) || !(fabsf(selfPitch) < 0.8f)) { vehSnapTick = g_tickNo; g_mtReason = 4; return false; }   // a cut / snap: blended this tick
        const float lag = 1.0f - alpha;
        dYaw -= lag * selfYaw;
        dPitch -= lag * selfPitch;
        ent = veh;
    }
    if (!(fabsf(dYaw) < 1.5f) || !(fabsf(dPitch) < 1.5f)) { g_mtReason = 5; return false; }
    const int k = FindSlot(ent);
    if (k < 0) { g_mtReason = 6; return false; }
    const V3 entCur = g_cur[k].m.pos, entPrev = g_prev[k].m.pos;
    if (Len(entCur - entPrev) > kMaxJump) { g_mtReason = 7; return false; }
    const V3 entDrawn = Lerp(entPrev, entCur, alpha);
    const V3 rel = entCur - g_camCur.pos, relPrev = entPrev - g_camPrev.pos;
    if (Len(rel) > 40.0f || Len(relPrev) > 40.0f) { g_mtReason = 8; return false; }
    // The game turns its camera around a target point straight above / below the
    // player or vehicle, on the view line: the point of the view line at the
    // player's horizontal distance. Where it is from the player, in the camera's
    // axes (right, forward, up), and how far the camera is from it, are blended
    // between the last two updates.
    auto along = [](const V3& d, const V3& fw) {
        const float h = fw.x * fw.x + fw.y * fw.y;
        return h > 0.01f ? (d.x * fw.x + d.y * fw.y) / h : Dot(d, fw);
    };
    const float dCur = along(rel, f), dPrev = along(relPrev, pf);
    const float ef = dPrev + (dCur - dPrev) * alpha;                       // camera to target point
    // The target point's offset from the player / vehicle is kept in world terms
    // (it is mostly straight up): in the camera's axes it tilted with every
    // update's turn, and the view jumped at each update when panning up / down.
    const V3 offCur = (g_camCur.pos + f * dCur) - entCur, offPrev = (g_camPrev.pos + pf * dPrev) - entPrev;
    const V3 pivot = entDrawn + Lerp(offPrev, offCur, alpha);
    const float cy = cosf(dYaw), sy = sinf(dYaw);
    auto yaw = [&](V3 v) { return V3{ v.x * cy - v.y * sy, v.x * sy + v.y * cy, v.z }; };
    const V3 r2 = yaw(r), f1 = yaw(f), u1 = yaw(u);
    const float cp = cosf(dPitch), sp = sinf(dPitch);
    const V3 f2 = f1 * cp + u1 * sp, u2 = u1 * cp - f1 * sp;
    // The camera's roll (a helicopter's camera leans with it) is taken from the latest
    // update as it is: the part it rolled since the update before is drawn blended,
    // or it stepped 30 times a second.
    auto rollOf = [](const V3& rr, const V3& uu) { return atan2f(-rr.z, uu.z); };
    const float selfRoll = Wrap(rollOf(r, u) - rollOf(pr, pu));
    V3 r3 = r2, u3 = u2;
    if (fabsf(selfRoll) < 0.8f && fabsf(selfRoll) > 1e-5f) {
        const float th = (1.0f - alpha) * selfRoll;   // rotates the roll back by that much
        const float ct = cosf(th), st = sinf(th);
        r3 = r2 * ct + u2 * st;
        u3 = u2 * ct - r2 * st;
    }
    cam.right = r3; cam.fwd = f2; cam.up = u3;
    cam.pos = pivot - f2 * ef;
    return true;
}

// Mark the moved frame dirty; skinned clumps also rebuild their bone matrices.
static void UpdateFrameAndSkin(unsigned char* e) {
    ((void(__fastcall*)(void*, void*))ENT_UPDATE_RW)(e, nullptr);
    const unsigned char* rwObject = *(unsigned char**)(e + 0x4C);
    if (rwObject && rwObject[0] == 2)   // rpCLUMP (UpdateRpHAnim reads it as a clump)
        ((void(__fastcall*)(void*, void*))ENT_UPDATE_SKIN)(e, nullptr);
}

// ---------------------------------------------------------------- apply / restore
// Bikes position their head/tail lights (and lean) from a second matrix,
// CBike::m_leanMatrix (+0x2C4), that CBike::CalculateLeanMatrix (0x609C90) builds
// from the bike's matrix once per tick: +0x2C0 says "already built", cleared by the
// next tick. Redraws would draw the lights where the bike was at the tick while the
// bike itself is drawn blended - lights detached from the bike, jumping each tick.
// So while a bike is drawn blended its lean matrix is rebuilt from the blended
// matrix, and put back afterwards (on a tick frame: rebuilt from the real matrix,
// exactly what the game itself would have built).
static const DWORD BIKE_VTBL = 0x6D7B34;        // CBike vtable (CBike::CBike, 0x615723)
static const DWORD BIKE_CALC_LEAN = 0x609C90;   // CBike::CalculateLeanMatrix (thiscall)
static const DWORD BIKE_LEAN_FLAG = 0x2C0;      // bool, then CMatrix m_leanMatrix at +0x2C4
static const DWORD BIKE_LEAN_SIZE = 0x4C;       // flag (+pad) + 0x48-byte CMatrix
static int g_bikeOk = -1;                        // -1 = not checked yet

static bool BikeCodeOk() {
    if (g_bikeOk < 0) {
        static const unsigned char head[] = { 0x53, 0x56, 0x89, 0xCB, 0x57, 0x81, 0xEC, 0xB0, 0x00, 0x00, 0x00,
                                              0x80, 0xBB, 0xC0, 0x02, 0x00, 0x00, 0x00 };
        static const unsigned char setFlag[] = { 0xC6, 0x83, 0xC0, 0x02, 0x00, 0x00, 0x01 };
        static const unsigned char ctor[] = { 0xC7, 0x03, 0x34, 0x7B, 0x6D, 0x00 };   // mov [ebx], CBike vtable
        g_bikeOk = !memcmp((void*)BIKE_CALC_LEAN, head, sizeof head) && !memcmp((void*)0x609DDA, setFlag, sizeof setFlag)
                   && !memcmp((void*)0x615723, ctor, sizeof ctor);
        if (!g_bikeOk) Log("Interp: bike lean code changed by another mod; bike lights left to it");
    }
    return g_bikeOk == 1;
}

struct Saved {
    unsigned char* ent;
    bool lean;                                // bike: lean matrix rebuilt, leanSave holds it
    unsigned char leanSave[BIKE_LEAN_SIZE];
    unsigned char mat[0x48], rw[0x40];        // before
    unsigned char matSet[0x40], rwSet[0x40];  // what InterpApply wrote
    unsigned char* frames;                    // bone frames changed, or nullptr
    Pose pose;                                // ...and their values before
};
static Saved g_saved[kMaxMoving];
static Pose g_blendPose;

static float g_alpha = 0.0f;   // of the current InterpApply

// ---------------------------------------------------------------- particles
// CParticle (pool): +0 position, +0xC velocity, +0x18 end-of-life time (fixed at
// creation), +0x54 next. 83 particle types at 0x975428, stride 0x94, active list
// head at +0x90 (CParticle::Update, 0x5620A8..0x563B10). Each tick records every
// live particle's position; drawn frames show it blended between the two ticks.
// A recycled particle (different end-of-life time) or a jump is drawn as is.
//
// A particle spawned during the latest tick has no earlier position to blend from.
// Drawn where it is, it stood still for a whole tick and then moved: rain drops,
// splashes and the spray of a broken fire hydrant are short-lived and fast, so a
// large share of them was always frozen and the effect looked choppy. It is drawn
// where its own velocity puts it at that moment instead: one tick's move
// (CParticle::Update: position += velocity * CTimer::ms_fTimeStep, 0x5620F6) back at
// the start of the tick, its real position at the end.
static float g_pTickStep = 0.0f;   // the latest tick's time step
static unsigned char* const ParticleTypes = (unsigned char*)0x975428;
static const int kParticleTypes = 0x53, kParticleTypeStride = 0x94;
static const int kParticleHash = 16384, kMaxParticleSaves = 8192;   // room for heavy rain
struct PRec { unsigned char* p; int death; V3 pos; };
static PRec g_pHashA[kParticleHash], g_pHashB[kParticleHash];
static PRec* g_pCur = g_pHashA;
static PRec* g_pPrev = g_pHashB;
struct PSave { unsigned char* p; V3 before, set; };
static PSave g_pSaved[kMaxParticleSaves];
static int g_numPSaved = 0;

static unsigned PHash(const void* p) { return ((unsigned)(DWORD)p >> 4) * 2654435761u >> 18; }   // 14 bits

static void ParticlesCapture() {
    g_pTickStep = *(float*)0x975424;   // CTimer::ms_fTimeStep
    PRec* t = g_pPrev; g_pPrev = g_pCur; g_pCur = t;
    memset(g_pCur, 0, sizeof(PRec) * kParticleHash);
    for (int i = 0; i < kParticleTypes; i++) {
        for (unsigned char* p = *(unsigned char**)(ParticleTypes + i * kParticleTypeStride + 0x90); p;
             p = *(unsigned char**)(p + 0x54)) {
            unsigned h = PHash(p);
            for (int probe = 0; probe < 16; probe++, h = (h + 1) & (kParticleHash - 1)) {
                if (!g_pCur[h].p) {
                    g_pCur[h].p = p;
                    g_pCur[h].death = *(int*)(p + 0x18);
                    memcpy(&g_pCur[h].pos, p, 12);
                    break;
                }
            }
        }
    }
}

static const PRec* PFind(const PRec* table, const unsigned char* p) {
    unsigned h = PHash(p);
    for (int probe = 0; probe < 16; probe++, h = (h + 1) & (kParticleHash - 1)) {
        if (table[h].p == p) return &table[h];
        if (!table[h].p) return nullptr;
    }
    return nullptr;
}

static void ParticlesApply(float alpha) {
    g_numPSaved = 0;
    for (int i = 0; i < kParticleTypes; i++) {
        for (unsigned char* p = *(unsigned char**)(ParticleTypes + i * kParticleTypeStride + 0x90); p;
             p = *(unsigned char**)(p + 0x54)) {
            if (g_numPSaved >= kMaxParticleSaves) return;
            const PRec* c = PFind(g_pCur, p);
            const PRec* o = PFind(g_pPrev, p);
            int death = *(int*)(p + 0x18);
            if (!c || c->death != death) continue;
            V3 now;
            memcpy(&now, p, 12);
            if (Len(now - c->pos) > 1e-4f) continue;
            V3 set;
            if (o && o->death == death) {
                if (Len(c->pos - o->pos) > kMaxJump) continue;
                set = Lerp(o->pos, c->pos, alpha);
            } else {   // spawned during the latest tick: along its own velocity
                V3 vel;
                memcpy(&vel, p + 0xC, 12);
                const V3 step = vel * g_pTickStep;
                if (!(g_pTickStep > 0.0f) || !(Len(step) <= kMaxJump)) continue;
                set = c->pos - step * (1.0f - alpha);
            }
            PSave& s = g_pSaved[g_numPSaved++];
            s.p = p;
            s.before = now;
            s.set = set;
            memcpy(p, &s.set, 12);
        }
    }
}

// ---------------------------------------------------------------- rubbish
// Leaves and newspapers on the roads (CRubbish, 32 sheets of 0x44 bytes at
// 0x7F92F8, init 0x568C64). A car driving past blows a sheet up: CRubbish::Update
// moves it once per tick, so it flew at 30 frames a second. CRubbish::Render
// (0x567FF7) draws state +0x1C == 1 (lying) at +0x00, any other state (flying) at
// +0x0C, turned by +0x34. Each tick records where every sheet is drawn and its
// angle; drawn frames show them blended between the two ticks.
static unsigned char* const Sheets = (unsigned char*)0x7F92F8;
static const int kSheets = 32, kSheetSize = 0x44;
struct RSheet { unsigned char state; unsigned moveStart; V3 pos; float angle; };
static RSheet g_rCur[kSheets], g_rPrev[kSheets];
struct RSave { bool on; int field; V3 before, set; float angBefore, angSet; };
static RSave g_rSaved[kSheets];

static V3 SheetPos(const unsigned char* s, unsigned char state) {
    V3 p;
    memcpy(&p, s + (state == 1 ? 0x00 : 0x0C), 12);
    return p;
}

static void RubbishCapture() {
    memcpy(g_rPrev, g_rCur, sizeof g_rCur);
    for (int i = 0; i < kSheets; i++) {
        const unsigned char* s = Sheets + i * kSheetSize;
        RSheet& r = g_rCur[i];
        r.state = s[0x1C];
        r.moveStart = *(const unsigned*)(s + 0x20);
        r.pos = SheetPos(s, r.state);
        r.angle = *(const float*)(s + 0x34);
    }
}

static void RubbishApply(float alpha) {
    for (int i = 0; i < kSheets; i++) {
        RSave& sv = g_rSaved[i];
        sv.on = false;
        unsigned char* s = Sheets + i * kSheetSize;
        const RSheet& c = g_rCur[i];
        const RSheet& o = g_rPrev[i];
        if (!c.state || !o.state || s[0x1C] != c.state) continue;
        if (c.state != 1 && o.state != 1 && o.moveStart != c.moveStart) continue;   // a new flight
        if (c.state == 1 && o.state == 1) continue;                                 // lying still
        const int field = c.state == 1 ? 0x00 : 0x0C;
        V3 now;
        memcpy(&now, s + field, 12);
        if (Len(now - c.pos) > 1e-4f || Len(c.pos - o.pos) > 10.0f) continue;
        sv.on = true;
        sv.field = field;
        sv.before = now;
        sv.set = Lerp(o.pos, c.pos, alpha);
        memcpy(s + field, &sv.set, 12);
        float* ang = (float*)(s + 0x34);
        sv.angBefore = *ang;
        float d = c.angle - o.angle;
        sv.angSet = fabsf(d) < 3.14159265f ? o.angle + d * alpha : *ang;
        *ang = sv.angSet;
    }
}

// ---------------------------------------------------------------- water drift
// CWaterLevel::RenderWater (0x5C1710) shapes the waves from the clock (time & 0xFFF,
// 0x5C185B), which redraw frames run forward. The water texture's drift is two
// accumulators (0x77FA6C, 0x77FA70) that it moves by a step on every draw (0x5C18A7:
// step = 0.0005 * wind (CWeather::Wind 0x9B6A94) + 0.0006, not while paused,
// 0xA10B36, or 0xA10B76), and FixedTick puts them back after each redraw frame
// (redraw_state.h). So the drift moved only at the 30 Hz ticks and every redraw
// frame drew it a whole step ahead, then it snapped back: steady waves with the
// texture stepping over them. The same block moves four more accumulators by the
// sine / cosine of the wave angle plus the step (0x77FA74, 0x77FA78, 0x77FA7C,
// 0x77FA80: the surface's wobble), with the same result. Each tick frame measures
// how far the renderer moved each of the six; on a redraw frame each is set back by
// (1 - alpha) of that before drawing, so with the renderer's own move it is drawn
// alpha of a tick on from the tick, like the clock; they are put back afterwards.
// (A move bigger than 0.25 is a wrap-around, 0x5C19CF..: the previous move is kept.)
static float* const WaterAcc = (float*)0x77FA6C;   // 6 floats
static const int kWaterAcc = 6;
static int g_waterOk = -1;                 // -1 unchecked
static bool g_waterApplied = false, g_waterTickOn = false, g_waterTickSaved = false;
static float g_waterSaved[kWaterAcc], g_waterMove[kWaterAcc];

static bool WaterStepping() {
    return !*(volatile unsigned char*)0xA10B36 && !*(volatile unsigned char*)0xA10B76;   // not paused: moved per draw
}

static void WaterApply(float alpha, bool tick) {
    g_waterApplied = false;
    if (g_waterOk < 0)
        g_waterOk = !memcmp((void*)0x5C184F, "\xD9\x05\x9C\xCD\x69\x00\xD8\x0D\x94\x6A\x9B\x00", 12) &&
                    !memcmp((void*)0x5C18A7, "\xD9\x05\x6C\xFA\x77\x00\xD8\xC4", 8) &&
                    !memcmp((void*)0x5C18FB, "\xD8\x05\x74\xFA\x77\x00", 6) &&
                    !memcmp((void*)0x5C19C3, "\xD8\x05\x80\xFA\x77\x00", 6);
    if (!g_waterOk) return;
    memcpy(g_waterSaved, WaterAcc, sizeof g_waterSaved);
    if (tick) { g_waterTickOn = WaterStepping(); g_waterTickSaved = true; return; }   // measured after the frame
    if (!WaterStepping()) return;
    for (int i = 0; i < kWaterAcc; i++) WaterAcc[i] -= (1.0f - alpha) * g_waterMove[i];
    g_waterApplied = true;
}

static void WaterRestore(bool tick) {
    if (tick) {
        if (g_waterOk != 1 || !g_waterTickSaved) return;
        g_waterTickSaved = false;
        for (int i = 0; i < kWaterAcc; i++) {
            const float d = g_waterTickOn ? WaterAcc[i] - g_waterSaved[i] : 0.0f;
            if (fabsf(d) <= 0.25f) g_waterMove[i] = d;
        }
        return;
    }
    if (!g_waterApplied) return;
    memcpy(WaterAcc, g_waterSaved, sizeof g_waterSaved);
    g_waterApplied = false;
}

static void RubbishRestore(bool tick) {
    for (int i = 0; i < kSheets; i++) {
        RSave& sv = g_rSaved[i];
        if (!sv.on) continue;
        sv.on = false;
        unsigned char* s = Sheets + i * kSheetSize;
        V3 now;
        memcpy(&now, s + sv.field, 12);
        float* ang = (float*)(s + 0x34);
        if (tick && (Len(now - sv.set) > 1e-6f || *ang != sv.angSet)) continue;   // the frame's own update moved it
        memcpy(s + sv.field, &sv.before, 12);
        *ang = sv.angBefore;
    }
}

static void ParticlesRestore(bool tick) {
    for (int i = g_numPSaved - 1; i >= 0; i--) {
        PSave& s = g_pSaved[i];
        V3 now;
        memcpy(&now, s.p, 12);
        if (tick && Len(now - s.set) > 1e-6f) continue;   // the frame's own update moved it
        memcpy(s.p, &s.before, 12);
    }
    g_numPSaved = 0;
}

// ---------------------------------------------------------------- vehicle parts
// Doors, bonnet, boot, wheels (spin and suspension), bike forks... are child
// frames of the vehicle's RenderWare frame, set by PreRender from game state that
// changes once per tick. After every PreRender their local matrices are blended
// between the last two ticks. RwFrame: +0x10 modelling matrix, +0x98 child,
// +0x9C next sibling, +0xA0 root (RwFrame init at 0x644841, RwFrameUpdateObjects).
static const int kMaxParts = 48;
struct Part { unsigned char* frame; unsigned char m[0x40]; };
struct Parts { int n; Part p[kMaxParts]; };
// Swapped each tick like the slots (InterpPartsCapture): CaptureParts sets n of every
// vehicle and writes parts 0..n-1, and InterpPartsApply reads only those.
static Parts g_partsA[110], g_partsB[110];
static Parts* g_partsCur = g_partsA;
static Parts* g_partsPrev = g_partsB;
struct SavedPart { unsigned char* frame; unsigned char before[0x40], set[0x40]; };
static SavedPart g_savedParts[110 * kMaxParts];
static int g_numSavedParts = 0;

static void CollectParts(unsigned char* frame, Parts& out) {
    for (unsigned char* c = *(unsigned char**)(frame + 0x98); c && out.n < kMaxParts; c = *(unsigned char**)(c + 0x9C)) {
        Part& p = out.p[out.n++];
        p.frame = c;
        memcpy(p.m, c + 0x10, 0x40);
        CollectParts(c, out);
    }
}

static unsigned char* VehicleRootFrame(unsigned char* e) {
    unsigned char* rwObject = *(unsigned char**)(e + 0x4C);
    if (!rwObject || rwObject[0] != 2) return nullptr;   // rpCLUMP
    return *(unsigned char**)(rwObject + 4);
}

static int g_numSaved = 0;
static bool g_camApplied = false;
static unsigned char g_camSave[0x40], g_camSave160[4], g_camSaveD4[8], g_camSave7D8[12], g_camSave8B8[0x30];
static unsigned char g_camRwSave[0x40];

static unsigned char* CameraRwMatrix(void** frameOut) {
    void* rwCam = *(void**)(Camera + 0x808);          // CCamera::m_pRwCamera
    if (!rwCam) return nullptr;
    void* frame = *(void**)((char*)rwCam + 4);        // RwObject.parent
    if (!frame) return nullptr;
    *frameOut = frame;
    return (unsigned char*)frame + 0x10;              // RwFrame.modelling
}

// The tail of CCamera::Process (0x46DCC1..0x46DD4F) with the matrix in TheCamera.
static void PushCameraToRw() {
    ((void(__fastcall*)(void*, void*))CAM_DERIVED)(Camera, nullptr);
    void* frame;
    unsigned char* rw = CameraRwMatrix(&frame);
    if (!rw) return;
    memcpy(rw + 0x00, Camera + 0x00, 12);   // right
    memcpy(rw + 0x10, Camera + 0x20, 12);   // RW up  = game up
    memcpy(rw + 0x20, Camera + 0x10, 12);   // RW at  = game forward
    memcpy(rw + 0x30, Camera + 0x30, 12);   // pos
    memcpy(Camera + 0x7D8, Camera + 0x30, 12);
    ((void(__cdecl*)(void*))RW_MAT_UPDATE)(rw);
    ((void(__cdecl*)(void*))RW_FRAME_UPD)(frame);
    ((void(__cdecl*)(void*))RW_FRAME_ORTH)(frame);
}

// Camera smoothness check, for the log: every 10 s (20 reports at most) while the
// camera moves, how much the drawn view turned per frame and how far the player /
// their vehicle moved on screen (in the camera's axes), on frames with a game update
// and on the frames between. Much bigger steps on update frames = the view jumps
// at updates; flips between drawing methods are counted too.
static struct {
    LONGLONG start;
    int frames[2], switches, mouseFrames, reasons[10];
    double rot[2], rotMax[2], drift[2], driftMax[2];
    bool have, lastMouse;
    V3 lastFwd, lastLocal;
} g_cc;
static int g_ccReports = 0;

static void CamCheck(bool tick, bool mouse, const Frame& cam, float alpha) {
    if (g_ccReports >= 20) return;
    LARGE_INTEGER now, f;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&f);
    const unsigned char* ent = ((const unsigned char* (__cdecl*)())0x4BC1E0)();   // FindPlayerVehicle
    if (!ent) ent = ((const unsigned char* (__cdecl*)())0x4BC120)();             // FindPlayerPed
    const int k = FindSlot(ent);
    V3 r, fw, u;
    if (k < 0 || !UnitAxes(cam, r, fw, u)) { g_cc.have = false; return; }
    const V3 rel = Lerp(g_prev[k].m.pos, g_cur[k].m.pos, alpha) - cam.pos;
    const V3 local = { Dot(rel, r), Dot(rel, fw), Dot(rel, u) };
    if (!g_cc.start) { memset(&g_cc, 0, sizeof g_cc); g_cc.start = now.QuadPart; }
    if (g_cc.have) {
        const int i = tick ? 1 : 0;
        float c = Dot(fw, g_cc.lastFwd);
        c = c > 1.0f ? 1.0f : c < -1.0f ? -1.0f : c;
        const double rot = acosf(c) * 57.29578;
        const double drift = Len(local - g_cc.lastLocal);
        g_cc.frames[i]++;
        g_cc.rot[i] += rot; if (rot > g_cc.rotMax[i]) g_cc.rotMax[i] = rot;
        g_cc.drift[i] += drift; if (drift > g_cc.driftMax[i]) g_cc.driftMax[i] = drift;
        if (mouse != g_cc.lastMouse) g_cc.switches++;
        if (mouse) g_cc.mouseFrames++;
        else if (g_mtReason > 0 && g_mtReason < 10) g_cc.reasons[g_mtReason]++;
    }
    g_cc.have = true;
    g_cc.lastMouse = mouse;
    g_cc.lastFwd = fw;
    g_cc.lastLocal = local;
    if (now.QuadPart - g_cc.start < f.QuadPart * 10) return;
    int lateWrites = 0;
    float lateMax = 0.0f;
    MouseTakeLateWrites(&lateWrites, &lateMax);   // this window's, reported or not
    if (g_cc.frames[0] && g_cc.frames[1] && (g_cc.rot[0] + g_cc.rot[1]) > 1.0) {
        g_ccReports++;
        Log("CamCheck: %s; turn per frame %.3f deg (max %.2f) between updates, %.3f (max %.2f) on update frames; "
            "player on screen moved %.4f m (max %.3f) between, %.4f (max %.3f) on update frames; mouse-turned %d of %d frames, %d switches; "
            "late camera writes %d (max %.2f deg); "
            "not turned because: axes %d, transition %d, camera %d, self turn %d, big turn %d, no record %d, jump %d, far %d",
            ent == ((const unsigned char* (__cdecl*)())0x4BC1E0)() ? "in a vehicle" : "on foot",
            g_cc.rot[0] / g_cc.frames[0], g_cc.rotMax[0], g_cc.rot[1] / g_cc.frames[1], g_cc.rotMax[1],
            g_cc.drift[0] / g_cc.frames[0], g_cc.driftMax[0], g_cc.drift[1] / g_cc.frames[1], g_cc.driftMax[1],
            g_cc.mouseFrames, g_cc.frames[0] + g_cc.frames[1], g_cc.switches, lateWrites, (double)lateMax,
            g_cc.reasons[1], g_cc.reasons[2],
            g_cc.reasons[3], g_cc.reasons[4], g_cc.reasons[5], g_cc.reasons[6], g_cc.reasons[7], g_cc.reasons[8]);
    }
    memset(&g_cc, 0, sizeof g_cc);
    g_cc.start = now.QuadPart;
}

void InterpApply(float alpha, bool tick) {
    g_numSaved = 0;
    g_numPSaved = 0;
    g_camApplied = false;
    g_timeApplied = false;
    if (!g_haveCur || !g_havePrev) return;
    if (alpha < 0.0f) alpha = 0.0f;
    if (alpha > 1.0f) alpha = 1.0f;
    g_alpha = alpha;

    // game clock (redraw frames only, forward from the latest tick) and particles
    if (!tick && g_timeCur >= g_timePrev && g_timeCur - g_timePrev < 1000 && *GameTimeMs == g_timeCur) {
        g_timeReal = *GameTimeMs;
        *GameTimeMs = g_timeCur + (int)(alpha * (float)(g_timeCur - g_timePrev));
        g_timeApplied = true;
    }
    ParticlesApply(alpha);
    RubbishApply(alpha);
    WaterApply(alpha, tick);

    // camera
    Frame cam;
    void* frame;
    unsigned char* rw = CameraRwMatrix(&frame);
    if (rw && Blend(g_camPrev, g_camCur, alpha, cam)) {
        const bool mouse = MouseTurnCamera(alpha, cam);
        CamCheck(tick, mouse, cam, alpha);
        memcpy(g_camSave, Camera, 0x40);
        memcpy(g_camSave160, Camera + 0x160, 4);
        memcpy(g_camSaveD4, Camera + 0xD4, 8);
        memcpy(g_camSave7D8, Camera + 0x7D8, 12);
        memcpy(g_camSave8B8, Camera + 0x8B8, 0x30);
        memcpy(g_camRwSave, rw, 0x40);
        WriteMatrix(Camera, cam);
        PushCameraToRw();
        g_camApplied = true;
    }

    // peds, vehicles, objects
    int base = 0;
    for (int p = 0; p < kMovingPools; p++) {
        const PoolDef& d = kPools[p];
        Pool* pool = GetPool(d);
        for (int i = 0; i < d.maxSlots; i++) {
            const Slot& c = g_cur[base + i];
            const Slot& o = g_prev[base + i];
            if (!c.valid || !o.valid || c.ent != o.ent || c.id != o.id) continue;
            if (!pool || i >= pool->size || (pool->flags[i] & 0x80) || (pool->flags[i] & 0x7F) != c.id) continue;
            unsigned char* e = (unsigned char*)c.ent;
            unsigned char* rwMat = *(unsigned char**)(e + 4 + 0x40);   // CMatrix::m_attachment
            if (!*(void**)(e + 0x4C) || !rwMat) continue;                // no RenderWare object
            // Blend what is drawn: the RenderWare matrix, tick to tick. The CMatrix
            // (read while drawing for lights, shadows, LOD) gets the same movement as
            // an offset, so an object the game or a mod moves outside its normal
            // update (trainer fly mode writes only the CMatrix) stays exactly as it
            // would be drawn, relative to everything else, just smooth.
            Frame f;
            const Frame real = ReadMatrix(rwMat);
            if (!Blend(o.m, c.m, alpha, f)) continue;
            V3 rr, rf, ru, br, bf, bu;   // unit axes of the real and blended matrix
            if (!UnitAxes(real, rr, rf, ru) || !UnitAxes(f, br, bf, bu)) continue;
            Saved& s = g_saved[g_numSaved++];
            s.ent = e;
            memcpy(s.mat, e + 4, 0x48);
            memcpy(s.rw, rwMat, 0x40);
            WriteMatrix(rwMat, f);
            ((void(__cdecl*)(void*))RW_MAT_UPDATE)(rwMat);
            // offset = blended * inverse(real), applied to the CMatrix
            auto rot = [&](V3 v) { return br * Dot(rr, v) + bf * Dot(rf, v) + bu * Dot(ru, v); };
            Frame cm = ReadMatrix(e + 4);
            cm.right = rot(cm.right); cm.fwd = rot(cm.fwd); cm.up = rot(cm.up);
            cm.pos = rot(cm.pos - real.pos) + f.pos;
            WriteMatrix(e + 4, cm);
            memcpy(s.matSet, e + 4, 0x40);
            memcpy(s.rwSet, rwMat, 0x40);
            s.lean = false;
            if (p == 1 && *(DWORD*)e == BIKE_VTBL && BikeCodeOk()
                && !*(void**)(e + BIKE_LEAN_FLAG + 4 + 0x40)) {   // lean matrix not attached to RenderWare
                memcpy(s.leanSave, e + BIKE_LEAN_FLAG, BIKE_LEAN_SIZE);
                e[BIKE_LEAN_FLAG] = 0;   // PreRender rebuilds it from the blended matrix
                s.lean = true;
            }

            // body pose, bone by bone
            s.frames = nullptr;
            const Pose& pc = g_poseCur[base + i];
            const Pose& pp = g_posePrev[base + i];
            int bones;
            unsigned char* frames = (pc.n && pp.n == pc.n) ? BoneFrames(e, &bones) : nullptr;
            if (frames && bones == pc.n) {
                ReadPose(frames, bones, s.pose);
                BlendPose(pp, pc, alpha, g_blendPose);
                WritePose(frames, g_blendPose);
                s.frames = frames;
            }
            UpdateFrameAndSkin(e);
        }
        base += d.maxSlots;
    }
}

// tick: the frame's drawing may legitimately move objects (water props bob, the
// game's own PreRender places parts): keep anything it changed, put back only
// what still holds the blended values. Redraw frames leave no trace at all.
// Tick, after PreRender: record this tick's part matrices (they don't depend on
// where the vehicle is drawn, only on its state).
static void CaptureParts(Parts* cur) {
    const PoolDef& d = kPools[1];
    Pool* pool = GetPool(d);
    for (int i = 0; i < d.maxSlots; i++) {
        cur[i].n = 0;
        const Slot& c = g_cur[kPools[0].maxSlots + i];
        if (!c.valid || !pool || i >= pool->size || (pool->flags[i] & 0x80)) continue;
        if (unsigned char* root = VehicleRootFrame((unsigned char*)c.ent)) CollectParts(root, cur[i]);
    }
}

void InterpPartsCapture() {
    { Parts* t = g_partsPrev; g_partsPrev = g_partsCur; g_partsCur = t; }
    CaptureParts(g_partsCur);
}

// After PreRender (tick and redraw frames): blend each part between the ticks.
void InterpPartsApply() {
    g_numSavedParts = 0;
    if (!g_haveCur || !g_havePrev) return;
    const PoolDef& d = kPools[1];
    Pool* pool = GetPool(d);
    for (int i = 0; i < d.maxSlots; i++) {
        const int s = kPools[0].maxSlots + i;
        const Slot& c = g_cur[s];
        const Slot& o = g_prev[s];
        const Parts& pc = g_partsCur[i];
        const Parts& pp = g_partsPrev[i];
        if (!c.valid || !o.valid || c.ent != o.ent || c.id != o.id || !pc.n || pc.n != pp.n) continue;
        if (!pool || i >= pool->size || (pool->flags[i] & 0x80) || (pool->flags[i] & 0x7F) != c.id) continue;
        unsigned char* root = VehicleRootFrame((unsigned char*)c.ent);
        if (!root) continue;
        for (int k = 0; k < pc.n; k++) {
            const Part& a = pp.p[k];
            const Part& b = pc.p[k];
            if (a.frame != b.frame) break;   // hierarchy changed (part fell off): stop here
            // PreRender just set this frame from the current state; only blend if it
            // still holds exactly what this tick recorded
            if (memcmp(b.frame + 0x10, b.m, 0x40) != 0) continue;
            Frame fa = ReadMatrix(a.m), fb = ReadMatrix(b.m), f;
            if (!Blend(fa, fb, g_alpha, f)) continue;   // > 90 degrees (fast wheel spin): as is
            SavedPart& sp = g_savedParts[g_numSavedParts++];
            sp.frame = b.frame;
            memcpy(sp.before, b.frame + 0x10, 0x40);
            WriteMatrix(b.frame + 0x10, f);
            ((void(__cdecl*)(void*))RW_MAT_UPDATE)(b.frame + 0x10);
            memcpy(sp.set, b.frame + 0x10, 0x40);
        }
        ((void(__cdecl*)(void*))RW_FRAME_UPD)(root);
    }
}

static void RestoreParts(bool tick) {
    for (int i = g_numSavedParts - 1; i >= 0; i--) {
        SavedPart& sp = g_savedParts[i];
        if (tick && memcmp(sp.frame + 0x10, sp.set, 0x40) != 0) continue;   // the frame's own drawing moved it
        memcpy(sp.frame + 0x10, sp.before, 0x40);
        ((void(__cdecl*)(void*))RW_FRAME_UPD)(*(void**)(sp.frame + 0xA0));
    }
    g_numSavedParts = 0;
}

void InterpRestore(bool tick) {
    RestoreParts(tick);
    ParticlesRestore(tick);
    RubbishRestore(tick);
    WaterRestore(tick);
    if (g_timeApplied) { *GameTimeMs = g_timeReal; g_timeApplied = false; }
    for (int i = g_numSaved - 1; i >= 0; i--) {
        Saved& s = g_saved[i];
        unsigned char* rwMat = *(unsigned char**)(s.mat + 0x40);
        if (!tick || memcmp(s.ent + 4, s.matSet, 0x40) == 0) memcpy(s.ent + 4, s.mat, 0x48);
        if (rwMat && (!tick || memcmp(rwMat, s.rwSet, 0x40) == 0)) memcpy(rwMat, s.rw, 0x40);
        if (s.lean) {
            if (tick && s.ent[BIKE_LEAN_FLAG]) {
                // built this frame from the blended matrix: build it again from the
                // real one, as the game's own PreRender would have
                s.ent[BIKE_LEAN_FLAG] = 0;
                ((void(__fastcall*)(void*, void*))BIKE_CALC_LEAN)(s.ent, nullptr);
            } else {
                memcpy(s.ent + BIKE_LEAN_FLAG, s.leanSave, BIKE_LEAN_SIZE);
            }
        }
        if (s.frames) WritePose(s.frames, s.pose);
        UpdateFrameAndSkin(s.ent);
    }
    g_numSaved = 0;
    if (g_camApplied) {
        memcpy(Camera, g_camSave, 0x40);
        memcpy(Camera + 0x160, g_camSave160, 4);
        memcpy(Camera + 0xD4, g_camSaveD4, 8);
        memcpy(Camera + 0x7D8, g_camSave7D8, 12);
        memcpy(Camera + 0x8B8, g_camSave8B8, 0x30);
        void* frame;
        if (unsigned char* rw = CameraRwMatrix(&frame)) {
            memcpy(rw, g_camRwSave, 0x40);
            ((void(__cdecl*)(void*))RW_FRAME_UPD)(frame);
        }
        g_camApplied = false;
    }
}

// ---------------------------------------------------------------- ConstructRenderList flags
// Written per object: +0x41 alpha fade, +0x42 flag word, +0x54/+0x55 flag bytes,
// +0x58 scan code (tools/p2.py).
// A slot is restored only if the same object (pool id) is still in it: objects
// created while drawing (water props on a tick) keep their own values.
struct EntFlags { unsigned char used, a41; unsigned short w42, w54, w58; };
static EntFlags g_flags[PED_POOL_SLOTS + 110 + 460 + 7000 + 1250 + 2340];

void EntityFlagsSave() {
    int k = 0;
    for (const PoolDef& d : kPools) {
        Pool* pool = GetPool(d);
        int n = pool ? (pool->size < d.maxSlots ? pool->size : d.maxSlots) : 0;
        for (int i = 0; i < n; i++, k++) {
            EntFlags& f = g_flags[k];
            f.used = 0;
            if (pool->flags[i] & 0x80) continue;
            unsigned char* e = (unsigned char*)pool->entries + i * d.stride;
            f.used = 0x80 | (pool->flags[i] & 0x7F);
            f.a41 = e[0x41];
            memcpy(&f.w42, e + 0x42, 2);
            memcpy(&f.w54, e + 0x54, 2);
            memcpy(&f.w58, e + 0x58, 2);
        }
        k += d.maxSlots - n;
    }
}

void EntityFlagsRestore() {
    int k = 0;
    for (const PoolDef& d : kPools) {
        Pool* pool = GetPool(d);
        int n = pool ? (pool->size < d.maxSlots ? pool->size : d.maxSlots) : 0;
        for (int i = 0; i < n; i++, k++) {
            const EntFlags& f = g_flags[k];
            if ((pool->flags[i] & 0x80) || f.used != (0x80 | (pool->flags[i] & 0x7F))) continue;
            unsigned char* e = (unsigned char*)pool->entries + i * d.stride;
            e[0x41] = f.a41;
            memcpy(e + 0x42, &f.w42, 2);
            memcpy(e + 0x54, &f.w54, 2);
            memcpy(e + 0x58, &f.w58, 2);
        }
        k += d.maxSlots - n;
    }
}
