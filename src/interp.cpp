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
    { 0x97F2AC, 0x6D8, 140, true  },   // peds
    { 0xA0FDE4, 0x5DC, 110, true  },   // vehicles
    { 0x94DBE0, 0x1A0, 460, true  },   // objects
    { 0x97F240, 0x64, 7000, false },   // buildings
    { 0x978834, 0x64, 1250, false },   // treadables
    { 0x97F25C, 0x68, 2340, false },   // dummies
};
static const int kMovingPools = 3;
static const int kMaxMoving = 140 + 110 + 460;
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
static unsigned char* BoneFrames(unsigned char* e, int* count) {
    unsigned char* rwObject = *(unsigned char**)(e + 0x4C);
    if (!rwObject || rwObject[0] != 2) return nullptr;   // rpCLUMP
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
struct Slot { void* ent; unsigned char id; bool valid; Frame m; };
static Slot g_cur[kMaxMoving], g_prev[kMaxMoving];
static Pose g_poseCur[kMaxMoving], g_posePrev[kMaxMoving];   // n == 0: none
static Frame g_camCur, g_camPrev;
static bool g_haveCur = false, g_havePrev = false;

void InterpReset() { g_haveCur = g_havePrev = false; }

void InterpCapture() {
    memcpy(g_prev, g_cur, sizeof g_cur);
    memcpy(g_posePrev, g_poseCur, sizeof g_poseCur);
    g_camPrev = g_camCur;
    g_havePrev = g_haveCur;

    int base = 0;
    for (int p = 0; p < kMovingPools; p++) {
        const PoolDef& d = kPools[p];
        Pool* pool = GetPool(d);
        for (int i = 0; i < d.maxSlots; i++) {
            Slot& s = g_cur[base + i];
            s.valid = false;
            g_poseCur[base + i].n = 0;
            if (!pool || i >= pool->size || (pool->flags[i] & 0x80)) continue;
            unsigned char* e = (unsigned char*)pool->entries + i * d.stride;
            unsigned char* rwMat = *(unsigned char**)(e + 4 + 0x40);   // CMatrix::m_attachment
            if (!rwMat) continue;
            s.ent = e;
            s.id = pool->flags[i] & 0x7F;
            s.m = ReadMatrix(rwMat);   // what is drawn (the RenderWare matrix)
            s.valid = true;
            int bones;
            if (unsigned char* frames = BoneFrames(e, &bones)) ReadPose(frames, bones, g_poseCur[base + i]);
        }
        base += d.maxSlots;
    }
    g_camCur = ReadMatrix(Camera);
    g_timePrev = g_timeCur;
    g_timeCur = *GameTimeMs;
    ParticlesCapture();
    g_haveCur = true;
}

static bool UnitAxes(const Frame& m, V3& r, V3& f, V3& u) {
    float lr = Len(m.right), lf = Len(m.fwd), lu = Len(m.up);
    if (lr < 1e-4f || lf < 1e-4f || lu < 1e-4f) return false;
    r = m.right * (1.0f / lr); f = m.fwd * (1.0f / lf); u = m.up * (1.0f / lu);
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
static unsigned char* const ParticleTypes = (unsigned char*)0x975428;
static const int kParticleTypes = 0x53, kParticleTypeStride = 0x94;
static const int kParticleHash = 8192, kMaxParticleSaves = 4096;
struct PRec { unsigned char* p; int death; V3 pos; };
static PRec g_pHashA[kParticleHash], g_pHashB[kParticleHash];
static PRec* g_pCur = g_pHashA;
static PRec* g_pPrev = g_pHashB;
struct PSave { unsigned char* p; V3 before, set; };
static PSave g_pSaved[kMaxParticleSaves];
static int g_numPSaved = 0;

static unsigned PHash(const void* p) { return ((unsigned)(DWORD)p >> 4) * 2654435761u >> 19; }   // 13 bits

static void ParticlesCapture() {
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
            if (!c || !o || c->death != death || o->death != death) continue;
            V3 now;
            memcpy(&now, p, 12);
            if (Len(now - c->pos) > 1e-4f || Len(c->pos - o->pos) > kMaxJump) continue;
            PSave& s = g_pSaved[g_numPSaved++];
            s.p = p;
            s.before = now;
            s.set = Lerp(o->pos, c->pos, alpha);
            memcpy(p, &s.set, 12);
        }
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
static Parts g_partsCur[110], g_partsPrev[110];
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

    // camera
    Frame cam;
    void* frame;
    unsigned char* rw = CameraRwMatrix(&frame);
    if (rw && Blend(g_camPrev, g_camCur, alpha, cam)) {
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
void InterpPartsCapture() {
    memcpy(g_partsPrev, g_partsCur, sizeof g_partsCur);
    const PoolDef& d = kPools[1];
    Pool* pool = GetPool(d);
    for (int i = 0; i < d.maxSlots; i++) {
        g_partsCur[i].n = 0;
        const Slot& c = g_cur[kPools[0].maxSlots + i];
        if (!c.valid || !pool || i >= pool->size || (pool->flags[i] & 0x80)) continue;
        if (unsigned char* root = VehicleRootFrame((unsigned char*)c.ent)) CollectParts(root, g_partsCur[i]);
    }
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
static EntFlags g_flags[140 + 110 + 460 + 7000 + 1250 + 2340];

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
