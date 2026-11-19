// Street population ([Population]).
//
// Density. At start-up the game (0x627FE0, called at 0x4A4B2D) reads two numbers from
// gta3.ini, a ped and a car density factor (0x6DBA00 / 0x6DBA04, kept between 0.5 and
// 3). Without that file both stay 0.6. From them it works out the most peds on the
// streets (MaxNumberOfPedsInUse, 0x694DC8 = 25 x factor; 0x694DCC = 40 x factor inside
// interiors) and the most traffic cars (MaxNumberOfCarsInUse, 0x686FCC = 12 x factor).
// The ped factor also scales how many peds a zone asks for (AddToPopulation, 0x53BE76),
// the car factor how many cars (0x426EB4). So a plain install has at most 15 peds
// and 7 cars. Here the factors come from the ini, and they're applied again after
// that start-up code runs, so a gta3.ini can't undo them.
//
// Off-screen removal. CPopulation::ManagePopulation (0x53D690) removes a ped more
// than 51 m away, and one that is off-screen (CEntity::GetIsOnScreen, 0x4885D0) more
// than 25 m away (0x694EB4, read at 0x53DEDA, times the speed-based creation
// multiplier). Look at a ped 30 m away, turn the camera, and it is gone. That
// distance is read from KeepOffscreen instead. (0x694EB4 is also used elsewhere,
// so only this one read is moved.)
//
// A full ped pool. Every ped comes from one pool (PED_POOL_SLOTS; CPools::ms_pPedPool,
// 0x97F2AC: { objects, flags, size, first free }; a flag with bit 0x80 is a free
// slot), and most of the game's ped makers wrote to the nothing a full pool gives
// (crashes at 0x4496E7, 0x53B653). Trainers and mission scripts may hold as many
// characters as they like; the game survives a full pool in layers:
//   1. a far street person is removed to make room (EvictOnePed);
//   2. each kind of maker keeps clear of the last few slots, so the game's own
//      makers (street groups 16, traffic cars 6, emergency cars 6, the rest 1) and
//      scripts (8) stop before the pool is empty, each skipping that one person or
//      car this time (logged, counted per kind in the census line);
//   3. every maker that can't take "none" got a path that makes nobody instead of
//      writing to nothing (the null exits below);
//   4. a script command aimed at a character that doesn't exist (a failed create
//      gives the script -1) is stepped over instead of run (ScriptCharGuard).
#include "population.h"
#include "config.h"
#include <windows.h>
#include <string.h>

static float* const PedFactor = (float*)0x6DBA00;
static float* const CarFactor = (float*)0x6DBA04;
static int* const MaxPeds = (int*)0x694DC8;
static int* const MaxPedsInterior = (int*)0x694DCC;
static int* const MaxCars = (int*)0x686FCC;
static const DWORD READ_DENSITY = 0x627FE0, READ_DENSITY_CALL = 0x4A4B2D;
static const DWORD OFFSCREEN_FLD = 0x53DEDA;

static float g_offscreenDist = 25.0f;
static bool g_densityOk = false, g_offscreenOk = false;

// The game's own values (gta3.ini or its defaults), put back when the Modern Fix menu
// turns a density off (0).
static bool g_haveOrig = false;
static float g_origPed = 1.0f, g_origCar = 1.0f;
static int g_origMaxPeds = 25, g_origMaxInterior = 40, g_origMaxCars = 12;
static void SaveOrig() {
    g_origPed = *PedFactor; g_origCar = *CarFactor;
    g_origMaxPeds = *MaxPeds; g_origMaxInterior = *MaxPedsInterior; g_origMaxCars = *MaxCars;
    g_haveOrig = true;
}

// The ped caps the density asks for; PopulationAfterPresent lowers what the game
// sees when the ped pool runs short.
static int g_wantMaxPeds = 25, g_wantMaxInterior = 40;

static void ApplyDensity() {
    if (g_cfg.pedDensity > 0) {
        *PedFactor = g_cfg.pedDensity;
        *MaxPeds = (int)(25.0f * g_cfg.pedDensity);
        *MaxPedsInterior = (int)(40.0f * g_cfg.pedDensity);
    } else if (g_haveOrig) {
        *PedFactor = g_origPed;
        *MaxPeds = g_origMaxPeds;
        *MaxPedsInterior = g_origMaxInterior;
    }
    g_wantMaxPeds = *MaxPeds;
    g_wantMaxInterior = *MaxPedsInterior;
    if (g_cfg.carDensity > 0) {
        *CarFactor = g_cfg.carDensity;
        *MaxCars = (int)(12.0f * g_cfg.carDensity);
    } else if (g_haveOrig) {
        *CarFactor = g_origCar;
        *MaxCars = g_origMaxCars;
    }
}

static void __cdecl ReadDensity() {
    ((void (__cdecl*)())READ_DENSITY)();
    SaveOrig();
    ApplyDensity();
}

// The Modern Fix menu (fixmenu.cpp) changed a density or the off-screen distance.
void PopulationSettingsChanged() {
    if (g_densityOk) ApplyDensity();
    if (g_offscreenOk) g_offscreenDist = g_cfg.offscreenKeep > 0 ? g_cfg.offscreenKeep : *(float*)0x694EB4;
}

static void Patch(DWORD at, const void* code, size_t n) {
    DWORD old;
    VirtualProtect((void*)at, n, PAGE_EXECUTE_READWRITE, &old);
    memcpy((void*)at, code, n);
    VirtualProtect((void*)at, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)at, n);
}

static int FreePedSlots() {
    const unsigned char* pool = *(const unsigned char* const*)0x97F2AC;
    if (!pool) return 0;
    const unsigned char* flags = *(const unsigned char* const*)(pool + 4);
    const int size = *(const int*)(pool + 8);
    if (!flags || size <= 0 || size > 4096) return 0;
    int free = 0;
    for (int i = 0; i < size; i++) free += (flags[i] & 0x80) != 0;
    return free;
}

// ---------------------------------------------------------------- refusals
// Who was told "not now" because the pool was short, since the last census line.
enum { K_SCRIPT, K_STREET, K_VEHICLE, K_TRAFFIC, K_EMERGENCY, K_COPS, K_COUNT };
static const char* const kKindName[K_COUNT] = {
    "scripts", "street people", "people for vehicles", "traffic cars", "emergency cars", "cops and crews" };
static long g_refused[K_COUNT], g_refusedTotal[K_COUNT];
static int g_refuseLogs[K_COUNT];

static long g_bodiesCleared = 0, g_bodiesClearedTotal = 0;   // ClearOldBodies, below
static int SkipBreakdown(char* out, int room);                  // with the script guard, below

static void Refuse(int kind, DWORD from, int free) {
    g_refused[kind]++;
    g_refusedTotal[kind]++;
    if (g_refuseLogs[kind]++ < 5)
        Log("Population: ped pool short (%d free); %s: one not made this time (from %X)", free, kKindName[kind], from);
}

// ---------------------------------------------------------------- headroom
// Whenever fewer than 30 slots are free, the street and interior caps the game
// reads (0x694DC8 / 0x694DCC) are lowered by the shortfall, and the population
// stops adding people until slots free up again; the configured caps come back
// with them.
static const int PED_RESERVE = 30;
static void LogMakers(int used);   // below
static void ClearOldBodies();      // below
static void FadeHandedBodies();    // below
static bool g_poolShort = false;

// The null exits (below): each one counts here when it made nobody.
extern "C" { volatile long g_nullHits[16] = {}; }
static const char* const kNullName[16] = {
    "a street group member", "a person for a vehicle", "a dead person in front of a car",
    "a helicopter crew member", "a SWAT member on a rope", "a vehicle passenger", "a vehicle driver",
    "a special vehicle's driver flags", "a special vehicle's passenger flags",
    "script 0560 (random driver)", "script 0561 (random passenger)", "script 0129 (character in a car)",
    "script 01C8 (character as passenger)", "script 0376 (random character)", "a road block cop", "a road block cop" };
static long g_nullSeen[16];
static int g_nullLogs = 0;

static volatile long g_charSkips = 0;
static long g_charSkipsSeen = 0;

// The reaction guard (below): each skipped reaction to a removed ped counts here.
extern "C" { volatile long g_eventPedGone = 0; }
static long g_eventPedGoneSeen = 0;
static int g_eventPedGoneLogs = 0;

void PopulationAfterPresent() {
    for (int i = 0; i < 16; i++) {
        const long n = g_nullHits[i];
        if (n != g_nullSeen[i]) {
            g_nullSeen[i] = n;
            if (g_nullLogs++ < 30) Log("Population: ped pool full; %s not made, the game carried on (%ld so far)", kNullName[i], n);
        }
    }
    {
        const long n = g_eventPedGone;
        if (n != g_eventPedGoneSeen) {
            g_eventPedGoneSeen = n;
            if (g_eventPedGoneLogs++ < 20)
                Log("Population: a ped's delayed reaction to a ped who was removed meanwhile skipped at 0x4E94E0 (crash there before; %ld so far)", n);
        }
    }
    if (!g_densityOk) return;
    const unsigned char* pool = *(const unsigned char* const*)0x97F2AC;
    if (!pool) return;
    const unsigned char* flags = *(const unsigned char* const*)(pool + 4);
    const int size = *(const int*)(pool + 8);
    if (!flags || size <= 0 || size > 4096) return;
    int free = 0;
    for (int i = 0; i < size; i++) free += (flags[i] & 0x80) != 0;
    if (*(volatile unsigned char*)0xA10AF6 != 1) {   // never while an F1 replay rebuilds the world
        ClearOldBodies();      // every 250 ms, whenever there are more than 20 bodies
        FadeHandedBodies();    // every frame
    }
    bool refusals = g_charSkips != g_charSkipsSeen || g_bodiesCleared;
    for (long r : g_refused) refusals |= r != 0;
    if (size - free > 120 || refusals) LogMakers(size - free);
    const int shortBy = free < PED_RESERVE ? PED_RESERVE - free : 0;
    const int peds = g_wantMaxPeds - shortBy, inside = g_wantMaxInterior - shortBy;
    *MaxPeds = peds > 0 ? peds : 0;
    *MaxPedsInterior = inside > 0 ? inside : 0;
    if ((shortBy > 0) != g_poolShort) {
        g_poolShort = shortBy > 0;
        if (g_poolShort) {
            Log("Population: ped pool nearly full (%d of %d free); fewer street people until slots free up", free, size);
            // Who holds the slots: created by (+0x160: 1 street, 2 mission), in a
            // vehicle (+0x3AC), dead (state 0x36 / 0x37), and by ped type (+0x3D4).
            const unsigned char* ents = *(const unsigned char* const*)pool;
            int street = 0, mission = 0, other = 0, inCar = 0, dead = 0, types[24] = {};
            for (int i = 0; i < size; i++) {
                if (flags[i] & 0x80) continue;
                const unsigned char* ped = ents + i * 0x6D8;
                const int by = ped[0x160], st = *(const int*)(ped + 0x244), ty = *(const int*)(ped + 0x3D4);
                if (by == 1) street++; else if (by == 2) mission++; else other++;
                if (ped[0x3AC]) inCar++;
                if (st == 0x36 || st == 0x37) dead++;
                if (ty >= 0 && ty < 24) types[ty]++;
            }
            char t[200] = "";
            for (int k = 0, n = 0; k < 24; k++)
                if (types[k]) n += wsprintfA(t + n, "%d:%d ", k, types[k]);
            Log("Population:   street %d, mission %d, other %d; in vehicles %d, dead %d; by type %s",
                street, mission, other, inCar, dead, t);
        }
        else Log("Population: ped pool has room again (%d of %d free)", free, size);
    }
}

// ---------------------------------------------------------------- making room
// CPed::operator new (0x50DA60: mov ecx, [0x97F2AC]; call CPool::New 0x50F000; ret)
// is sent here. When the pool has no slot, the farthest street person nobody
// depends on is removed the way CPopulation::ManagePopulation removes one
// (0x53E2E5: CWorld::Remove 0x4DB310, then its deleting destructor, vtable +8 with
// 1), off-screen ones first, and the slot is handed out. Never:
//   - the player (CWorld::Players[0].m_pPed, 0x94AD28) or player types (+0x3D4, 0-3);
//   - a mission or script character (created-by +0x160 must be 1, random);
//   - one in a vehicle (+0x3AC), or getting into one (+0x3A8 set, +0x3AC not);
//   - one dead or dying (state +0x244 0x36 / 0x37, which the game's clean-up still
//     refers to);
//   - a group's leader (+0x151 bit 2, set by CPed::SetLeader 0x4F07D0), or anyone
//     another ped leads to (+0x184 leader) or has as its objective (+0x16C);
//   - one made in the last 250 ms (below).
// Pointers the game registered on a ped (CEntity::RegisterReference 0x4C6AC0: the
// leader +0x184, the objective +0x16C, a vehicle's driver and passengers) are
// cleared by its destructor; the checks above keep away from the ones that a
// cleared pointer would leave half done.
static const DWORD PED_NEW = 0x50DA60, POOL_NEW = 0x50F000;
static int g_evictLogs = 0, g_noEvictLogs = 0;

// People made in the last 250 ms are never removed: whoever made them may still be
// setting them up (a street gang's followers get their just-made leader; a script
// goes on to set the one it made). Removing such a one is what left the game
// freeing a ped twice (crash at 0x6616B7). (3 s was too long: VCTrainer's script
// made 23 Vice Squad characters a second, so everyone around was "new" and none
// could make room.)
struct Recent { void* ped; DWORD at; };
static Recent g_recent[256];
static unsigned g_recentNext = 0;
static bool MadeRecently(const void* ped, DWORD now) {
    for (const Recent& r : g_recent)
        if (r.ped == ped && now - r.at < 250) return true;
    return false;
}

static bool EvictOnePed() {
    const DWORD now = *(volatile DWORD*)0x974B2C;
    unsigned char* pool = *(unsigned char**)0x97F2AC;
    unsigned char* player = *(unsigned char**)0x94AD28;   // CWorld::Players[0].m_pPed (not the camera's target)
    if (!pool || !player) return false;
    unsigned char* ents = *(unsigned char**)pool;
    const unsigned char* flags = *(const unsigned char**)(pool + 4);
    const int size = *(const int*)(pool + 8);
    if (!ents || !flags || size <= 0 || size > 4096) return false;
    // People other peds depend on: their objective (+0x16C) and their leader (+0x184).
    static const void* held[2 * 4096];
    int nHeld = 0;
    for (int i = 0; i < size; i++) {
        if (flags[i] & 0x80) continue;
        const unsigned char* ped = ents + i * 0x6D8;
        if (const void* t = *(void* const*)(ped + 0x16C)) held[nHeld++] = t;
        if (const void* l = *(void* const*)(ped + 0x184)) held[nHeld++] = l;
    }
    const float* pp = (const float*)(player + 0x34);
    unsigned char* best = nullptr;
    float bestScore = -1.0f;
    for (int i = 0; i < size; i++) {
        if (flags[i] & 0x80) continue;
        unsigned char* ped = ents + i * 0x6D8;
        if (ped == player || *(int*)(ped + 0x3D4) <= 3) continue;
        if (ped[0x160] != 1 || ped[0x3AC]) continue;
        if (*(void**)(ped + 0x3A8)) continue;   // getting into a vehicle
        if (ped[0x151] & 2) continue;           // leads a group
        const int state = *(int*)(ped + 0x244);
        if (state == 0x36 || state == 0x37) continue;
        if (MadeRecently(ped, now)) continue;
        bool isHeld = false;
        for (int k = 0; k < nHeld && !isHeld; k++) isHeld = held[k] == ped;
        if (isHeld) continue;
        const float* p = (const float*)(ped + 0x34);
        const float dx = p[0] - pp[0], dy = p[1] - pp[1], dz = p[2] - pp[2];
        // Off-screen ones first, as the game removes; when every street person is in
        // view (VCTrainer spawning a crowd around the player filled the pool that
        // way), the farthest one in view, rather than the game crashing.
        float score = dx * dx + dy * dy + dz * dz;
        if (!((bool (__thiscall*)(void*))0x4885D0)(ped)) score += 1.0e8f;
        if (score > bestScore) { bestScore = score; best = ped; }
    }
    if (!best) {
        if (g_noEvictLogs++ < 10) Log("Population: ped pool full and no street person can be removed");
        return false;
    }
    ((void (__cdecl*)(void*))0x4DB310)(best);
    (*(void (__thiscall**)(void*, int))(*(DWORD*)best + 8))(best, 1);
    if (g_evictLogs++ < 10) Log("Population: ped pool full; a street person was removed to make room");
    return true;
}

// ---------------------------------------------------------------- dead bodies
// Why bodies fill the pool. CPopulation::ManagePopulation (0x53DB9D) marks a body
// it may delete (state 0x37, CPed::CanBeDeleted 0x4FFEF0, not in a vehicle) to fade
// out once it has been dead 30 s (+0x55C, the time of death; 15 s while 0xA10A72
// is 1), as bit 7 of +0x152, and removes it (CWorld::Remove 0x4DB310, then its
// deleting destructor) once its clump's alpha (0x5814F0) is 0.
// The alpha goes down 8 a step (0x505790, 30 steps a second) only in the ped's
// ProcessControl (vtable +0x20; every ped class calls 0x505790 first), which
// CWorld::Process (0x4D76C0) runs for entities on the moving list (0x9B48A8, node
// at +0xE0). In game, after a fight at the player's feet (VCTrainer's Vice Squad),
// 150 bodies stayed for minutes: most were never flagged although dead well over
// 30 s, and the ones that were flagged didn't finish fading, so the game's own
// removal never ran for them. They went only when the player walked away.
//
// Every 250 ms, whenever there are more than 20 bodies (not only while the pool is
// short: the street population takes every slot that frees up, so a clearing that
// stops at 30 free slots stalls after each batch), the oldest bodies the game
// itself would delete are handed over, down to the 20 newest, at most 10 a scan.
// A body qualifies when:
//   - it is a random ped (created-by +0x160 = 1, never a mission or script
//     character) and not a player type;
//   - it is dead (0x37, not dying) for at least 15 s;
//   - it is not in a vehicle (+0x3AC, as ManagePopulation checks; its last vehicle
//     +0x3A8 stays set after death and doesn't count), and is CanBeDeleted;
//   - no living ped has it as its objective (+0x16C) or leader (+0x184);
//   - it is more than 2 m from the player, and no pointer the player, the player
//     info (0x94AD28) or the camera (0x7E4688) keeps refers to it (its registered
//     references, CEntity +0x60).
// The census line under it lists, for the last scan, how many bodies were left
// alone and why, and how the handed bodies went.
// A handed body gets the game's fade flag (+0x152 bit 7) and is faded at the game's
// own rate (8 alpha per 1/30 s step: 255 to 0 in about 1.06 s; off screen, at
// once) with its clump alpha (0x5814F0 / 0x581500, never raised). Bodies the game
// flagged itself are faded the same way. When the alpha is 0, ManagePopulation
// removes the body with its own checks; if it is still there 0.2 s later, it is
// removed here the same way ManagePopulation does it (0x53DBF6: CWorld::Remove
// 0x4DB310, then its deleting destructor), after the same checks: not a player
// (0x4F4930), CanBeDeleted, not in a vehicle, still dead and still flagged. This runs
// after Present, between game updates, never inside one. Every handed body is
// checked against its pool slot (same id, same address) before it is touched; one
// the game has removed, or no longer wants removed (flag cleared, alive again), is
// dropped from the list and left alone. Pointers registered on it (CEntity +0x60
// list, RegisterReference 0x4C6AC0) are cleared by its destructor; the first log
// lines say how many it had.
static int g_bodyLogs = 0, g_bodyGoneLogs = 0, g_bodyKeptLogs = 0;
static DWORD g_bodyScanAt = 0;
struct Fading { unsigned char* ped; int slot; unsigned char id; DWORD start; };
static Fading g_fading[128];
static int g_nFading = 0;
static const DWORD FADE_MS = 1063;    // 255 / 8 steps at 30 a second
static const DWORD GRACE_MS = 200;    // after the fade, the game's own removal goes first
static long g_bodiesRemovedHere = 0, g_bodiesRemovedByGame = 0, g_bodiesKeptByGame = 0;

// Why bodies were or weren't handed over, from the last scan (for the census line).
struct BodyStats {
    int dead, oldest, young, notRandom, inVehicle, held, notDeletable, flagged, eligible, other, fading, trainer;
    int under, aimed;                                        // protected: the one under the player, the lock-on target
    int near2m, refPlayer, refInfo, refCamera, refField;     // what the old test would have kept (census only)
};
static BodyStats g_bodyStats;

// Characters VCTrainer made: the create-char command (009A) run by its script
// object "vctrain" (see NoteMaker). They are script characters (+0x160 = 2), which
// the game never removes, so its Vice Squad and spawned crowds stayed as bodies
// (202 of 205 peds once). Each is remembered per pool slot by its address, the
// slot's id byte (CPool::New gives a slot a new id each time it is used) and the
// model asked for; every ped made anywhere rewrites its slot's record, so a slot used
// again by a mission script never matches. A dead one is cleared like a street body:
// before it is handed over it is released the game's way, CTheScripts::CleanUpThisPed
// (0x45EB10, what MARK_CHAR_AS_NO_LONGER_NEEDED 01C2 calls: random again, its AI
// reset, the mission ped count 0x9B5F70 lowered), as VCTrainer itself does for a dead
// bodyguard. Characters any other script made are never touched.
static const DWORD RELEASE_CHAR = 0x45EB10;
static bool g_releaseOk = false;
struct TrainerMark { const unsigned char* ped; short model; unsigned char id; bool set; };
static TrainerMark g_trainerMade[4096];
static long g_trainerReleased = 0;

static void MarkMade(void* p, bool trainer, int model) {
    const unsigned char* pool = *(const unsigned char* const*)0x97F2AC;
    if (!pool || !p) return;
    const unsigned char* ents = *(const unsigned char* const*)pool;
    const unsigned char* flags = *(const unsigned char* const*)(pool + 4);
    const int size = *(const int*)(pool + 8);
    const int off = (int)((const unsigned char*)p - ents);
    if (!ents || !flags || off < 0 || off % 0x6D8) return;
    const int slot = off / 0x6D8;
    if (slot >= size || slot >= 4096) return;
    if (trainer && model >= 0 && model < 0x8000) g_trainerMade[slot] = { (const unsigned char*)p, (short)model, flags[slot], true };
    else g_trainerMade[slot] = {};
}

static bool TrainerMade(const unsigned char* ped, int slot, unsigned char id) {
    if (slot < 0 || slot >= 4096) return false;
    const TrainerMark& m = g_trainerMade[slot];
    return m.set && m.ped == ped && m.id == id && m.model == *(const short*)(ped + 0x5C);
}

// For the census only: where pointers to this body are kept (its registered
// references, CEntity +0x60, nodes { next, address of the pointer }). These no
// longer protect a body: the player's melee-attacker list (+0x6A0, 6 entries) and
// other fields can still name a ped that died long ago, and the deleting destructor
// clears every registered pointer anyway.
enum { REF_PLAYER = 1, REF_INFO = 2, REF_CAMERA = 4 };
static int PlayerRefs(const unsigned char* ped, const unsigned char* player, int* field) {
    int kinds = 0, guard = 0;
    for (const unsigned char* r = *(const unsigned char* const*)(ped + 0x60); r && guard < 64; r = *(const unsigned char* const*)r, guard++) {
        const DWORD at = *(const DWORD*)(r + 4);
        if (at >= (DWORD)player && at < (DWORD)player + 0x6D8) { kinds |= REF_PLAYER; *field = (int)(at - (DWORD)player); }
        else if (at >= 0x94AD28 && at < 0x94AD28 + 0x170) kinds |= REF_INFO;      // CWorld::Players[0] (CPlayerInfo)
        else if (at >= 0x7E4688 && at < 0x7E4688 + 0x1000) kinds |= REF_CAMERA;   // TheCamera and after it
    }
    return kinds;
}

static bool Tracked(const unsigned char* ped) {
    for (int k = 0; k < g_nFading; k++) if (g_fading[k].ped == ped) return true;
    return false;
}

static bool AddFading(unsigned char* ped, int slot, unsigned char id, DWORD start) {
    if (Tracked(ped)) return true;
    if (g_nFading >= 128) return false;
    g_fading[g_nFading++] = { ped, slot, id, start };
    return true;
}

// Each frame: handed bodies fade at the game's rate; a faded one the game hasn't
// removed after GRACE_MS is removed the way ManagePopulation does it.
static void FadeHandedBodies() {
    if (!g_nFading) return;
    const unsigned char* pool = *(const unsigned char* const*)0x97F2AC;
    if (!pool) { g_nFading = 0; return; }
    unsigned char* ents = *(unsigned char* const*)pool;
    const unsigned char* flags = *(const unsigned char* const*)(pool + 4);
    const int size = *(const int*)(pool + 8);
    if (!ents || !flags) { g_nFading = 0; return; }
    const DWORD now = *(volatile DWORD*)0x974B2C;
    int keep = 0;
    for (int k = 0; k < g_nFading; k++) {
        const Fading f = g_fading[k];
        if (f.slot >= size || flags[f.slot] != f.id || ents + f.slot * 0x6D8 != f.ped) {   // the game removed it
            g_bodiesRemovedByGame++;
            continue;
        }
        unsigned char* ped = f.ped;
        void* clump = *(void**)(ped + 0x4C);
        const int state = *(int*)(ped + 0x244);
        if (state != 0x37 || !(ped[0x152] & 0x80) || !clump) {        // the game wants it kept now
            g_bodiesKeptByGame++;
            if (g_bodyKeptLogs++ < 5)
                Log("Population: a body handed to the fade is no longer one to remove (state %X, fade flag %d, clump %d); left to the game",
                    state, (ped[0x152] & 0x80) != 0, clump != nullptr);
            continue;
        }
        const DWORD t = now >= f.start ? now - f.start : 0;
        const int target = t >= FADE_MS ? 0 : 255 - (int)(t * 240 / 1000);
        int alpha = ((int (__cdecl*)(void*))0x5814F0)(clump);
        if (alpha > target) {
            alpha = target < 0 ? 0 : target;
            ((void (__cdecl*)(void*, int))0x581500)(clump, alpha);
        }
        if (alpha > 0 || t < FADE_MS + GRACE_MS) { g_fading[keep++] = f; continue; }
        // Faded and still here: ManagePopulation's own checks (0x53DB72..0x53DB97), then its removal.
        if (((bool (__thiscall*)(void*))0x4F4930)(ped) || !((bool (__thiscall*)(void*))0x4FFEF0)(ped) || ped[0x3AC]) {
            g_bodiesKeptByGame++;
            if (g_bodyKeptLogs++ < 5) Log("Population: a faded body is no longer deletable (player, in a vehicle or kept by a script); left to the game");
            continue;
        }
        ((void (__cdecl*)(void*))0x4DB310)(ped);                              // CWorld::Remove
        (*(void (__thiscall**)(void*, int))(*(DWORD*)ped + 8))(ped, 1);       // deleting destructor
        g_bodiesRemovedHere++;
        if (g_bodyGoneLogs++ < 5)
            Log("Population: a faded body the game hadn't removed after %lu ms was removed its way (CWorld::Remove, then its destructor)",
                t - FADE_MS);
    }
    g_nFading = keep;
}

static void ClearOldBodies() {
    const DWORD now = *(volatile DWORD*)0x974B2C;
    if (now - g_bodyScanAt < 250 && now >= g_bodyScanAt) return;
    g_bodyScanAt = now;
    static const int KEEP_BODIES = 20, PER_SCAN = 10;
    unsigned char* pool = *(unsigned char**)0x97F2AC;
    // The player ped (CWorld::Players[0].m_pPed), not the camera's target (0x7E4E94),
    // which is the car while driving.
    unsigned char* player = *(unsigned char**)0x94AD28;
    if (!pool || !player) return;
    unsigned char* ents = *(unsigned char**)pool;
    const unsigned char* flags = *(const unsigned char**)(pool + 4);
    const int size = *(const int*)(pool + 8);
    if (!ents || !flags || size <= 0 || size > 4096) return;
    // People a living ped is after (+0x16C objective) or follows (+0x184 leader).
    // A dead ped's own targets don't count: it no longer acts on them.
    static const void* held[2 * 4096];
    int nHeld = 0, free = 0;
    for (int i = 0; i < size; i++) {
        if (flags[i] & 0x80) { free++; continue; }
        const unsigned char* ped = ents + i * 0x6D8;
        const int st = *(const int*)(ped + 0x244);
        if (st == 0x36 || st == 0x37) continue;
        if (const void* t = *(void* const*)(ped + 0x16C)) held[nHeld++] = t;
        if (const void* l = *(void* const*)(ped + 0x184)) held[nHeld++] = l;
    }
    const float* pp = (const float*)(player + 0x34);
    // Protected now: the one body the player stands on (the nearest within 1.5 m, in
    // 3D, so bodies on another floor don't count), and the player's weapon lock-on
    // target (CPed +0x508, set by the player's targeting at 0x532F99 / 0x533A7D).
    const void* aimed = *(void* const*)(player + 0x508);
    const unsigned char* under = nullptr;
    float underD2 = 1.5f * 1.5f;
    for (int i = 0; i < size; i++) {
        if (flags[i] & 0x80) continue;
        const unsigned char* ped = ents + i * 0x6D8;
        if (ped == player || *(const int*)(ped + 0x244) != 0x37) continue;
        const float* p = (const float*)(ped + 0x34);
        const float dx = p[0] - pp[0], dy = p[1] - pp[1], dz = p[2] - pp[2];
        const float d2 = dx * dx + dy * dy + dz * dz;
        if (d2 < underD2) { underD2 = d2; under = ped; }
    }
    struct Body { unsigned char* ped; DWORD died; int slot; bool trainer; };
    static Body bodies[4096];
    int n = 0, dead = 0;
    BodyStats s = {};
    // Dead at least 15 s; 5 s while the pool is short.
    const DWORD minAge = free < PED_RESERVE ? 5000 : 15000;
    for (int i = 0; i < size; i++) {
        if (flags[i] & 0x80) continue;
        unsigned char* ped = ents + i * 0x6D8;
        if (*(int*)(ped + 0x244) != 0x37) continue;
        dead++;
        const DWORD diedAt = *(DWORD*)(ped + 0x55C);
        if (diedAt <= now && (int)((now - diedAt) / 1000) > s.oldest) s.oldest = (int)((now - diedAt) / 1000);
        // VCTrainer's own character, still a script character: released before it is handed over.
        const bool trainer = g_releaseOk && ped[0x160] == 2 && TrainerMade(ped, i, flags[i]);
        if (trainer) s.trainer++;
        if (Tracked(ped)) { s.fading++; continue; }                 // handed over already
        if (ped == player || *(int*)(ped + 0x3D4) <= 3 || (ped[0x160] != 1 && !trainer)) { s.notRandom++; continue; }
        // As ManagePopulation: not in a vehicle (+0x3AC). Its last vehicle (+0x3A8)
        // stays set after death and doesn't matter to the game's removal.
        if (ped[0x3AC]) { s.inVehicle++; continue; }
        if (!*(void**)(ped + 0x4C)) { s.other++; continue; }    // no clump to fade
        const DWORD died = *(DWORD*)(ped + 0x55C);
        if (now - died < minAge || died > now) { s.young++; continue; }
        bool isHeld = false;
        for (int k = 0; k < nHeld && !isHeld; k++) isHeld = held[k] == ped;
        if (isHeld) { s.held++; continue; }
        // CanBeDeleted (0x4FFEF0) checks only "in a vehicle" (above) and that it isn't a
        // script character; VCTrainer's own are released first, so only theirs pass it.
        if (!trainer && !((bool (__thiscall*)(void*))0x4FFEF0)(ped)) { s.notDeletable++; continue; }
        // What the old test (within 2 m flat, or any pointer from the player, the player
        // info or the camera) would have kept, counted for the census only.
        const float* p = (const float*)(ped + 0x34);
        const float dx = p[0] - pp[0], dy = p[1] - pp[1];
        if (dx * dx + dy * dy < 4.0f) s.near2m++;
        int field = -1;
        const int refs = PlayerRefs(ped, player, &field);
        if (refs & REF_PLAYER) { s.refPlayer++; s.refField = field; }
        if (refs & REF_INFO) s.refInfo++;
        if (refs & REF_CAMERA) s.refCamera++;
        if (ped == under) { s.under++; continue; }
        if (ped == aimed) { s.aimed++; continue; }
        if (!trainer && (ped[0x152] & 0x80)) {
            // The game flagged it itself: its fade is finished from where the alpha is.
            const int alpha = ((int (__cdecl*)(void*))0x5814F0)(*(void**)(ped + 0x4C));
            const int done = alpha >= 255 ? 0 : alpha <= 0 ? 255 : 255 - alpha;
            if (AddFading(ped, i, flags[i], now - (DWORD)done * 1000 / 240)) s.fading++;
            s.flagged++;
            continue;
        }
        bodies[n++] = { ped, died, i, trainer };
    }
    s.dead = dead;
    s.eligible = n;
    g_bodyStats = s;
    int want = dead - g_nFading - KEEP_BODIES;
    if (want > PER_SCAN) want = PER_SCAN;
    for (int c = 0; c < want && n > 0 && g_nFading < 128; c++) {
        int oldest = 0;
        for (int k = 1; k < n; k++) if (bodies[k].died < bodies[oldest].died) oldest = k;
        const Body b = bodies[oldest];
        bodies[oldest] = bodies[--n];
        unsigned char* ped = b.ped;
        if (b.trainer) {
            ((void (__cdecl*)(void*))RELEASE_CHAR)(ped);   // as 01C2 MARK_CHAR_AS_NO_LONGER_NEEDED
            g_trainerMade[b.slot] = {};
            g_trainerReleased++;
            if (ped[0x160] != 1 || *(int*)(ped + 0x244) != 0x37 || ped[0x3AC] || !*(void**)(ped + 0x4C)) {
                if (g_bodyKeptLogs++ < 5) Log("Population: a VCTrainer body released with 0x%X isn't a random dead body now; left to the game", RELEASE_CHAR);
                continue;
            }
        }
        const bool onScreen = ((bool (__thiscall*)(void*))0x4885D0)(ped);
        ped[0x152] |= 0x80;   // fade out, as ManagePopulation marks it after 30 s
        // On screen it fades where it lies; off screen nobody sees it, so its alpha is 0 at once.
        AddFading(ped, b.slot, flags[b.slot], onScreen ? now : now - FADE_MS);
        g_bodiesCleared++;
        g_bodiesClearedTotal++;
        if (g_bodyLogs++ < 10) {
            int refs = 0;
            for (const unsigned char* r = *(const unsigned char* const*)(ped + 0x60); r && refs < 100; r = *(const unsigned char* const*)r) refs++;
            const float* p = (const float*)(ped + 0x34);
            const float dx = p[0] - pp[0], dy = p[1] - pp[1];
            Log("Population: %d bodies; one dead %lu s, %d m away, %s, %d registered reference(s), handed to the game's fade and removal",
                dead, (now - b.died) / 1000, (int)__builtin_sqrtf(dx * dx + dy * dy), onScreen ? "on screen" : "off screen", refs);
        }
    }
}

// ---------------------------------------------------------------- the census
// Who makes people: the stub below (at 0x50DA60) keeps the caller's EBP, EBX and
// stack pointer. Each script create command keeps its running script
// (CRunningScript, name at +8) in its own place, and its type and model in its own
// ScriptParams (0x7D7438 + 4 * n):
//   009A CREATE_CHAR (0x449675 / 0x4496AC / 0x4496CB): EBP; type [0], model [1];
//   0129 CREATE_CHAR_INSIDE_CAR (0x445E7B / 0x445EA6 / 0x445ECA): EBX (0x444BF7);
//     car [0], type [1], model [2];
//   01C8 CREATE_CHAR_AS_PASSENGER (0x45295B / 0x452986 / 0x4529AA): EBP; type [1],
//     model [2];
//   0376 CREATE_RANDOM_CHAR (0x45E044): the handler's [esp+18h], 8 bytes above the
//     stub's stack (the return address and the size pushed); no type or model.
// A script pointer is used only when it is on the active script list (0x975338,
// linked at +0); the road block's cops (0x44409E) and everything else get no name.
// Each maker (return address, and the script's name) is counted and logged every
// 30 s while there's a crowd, with the refusals per kind of maker.
extern "C" { void* g_pedNewEbp = nullptr; void* g_pedNewEbx = nullptr; unsigned char* g_pedNewEsp = nullptr; }
struct Maker { DWORD from; char script[9]; int type, model, count; };
static Maker g_makers[32];
static DWORD g_makerLogAt = 0;

// A script the game runs is on its active list. VCTrainer runs its own script
// object (vctrain) outside that list, so a pointer that isn't on it is still taken
// when it is readable and its name is 1-8 lower-case letters, digits or '_'
// followed by zeros: what a script name looks like, which a stray stack value
// almost never is.
static bool ActiveScript(const void* s) {
    if (!s) return false;
    int guard = 0;
    for (const unsigned char* a = *(const unsigned char* const*)0x975338; a && guard < 256; a = *(const unsigned char* const*)a, guard++)
        if (a == s) return true;
    if (IsBadReadPtr(s, 0x88)) return false;
    const unsigned char* n = (const unsigned char*)s + 8;
    int len = 0;
    while (len < 8 && n[len]) {
        const unsigned char c = n[len];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return false;
        len++;
    }
    for (int i = len; i < 8; i++) if (n[i]) return false;
    return len > 0;
}

static void ScriptName(const unsigned char* s, char* name) {
    memcpy(name, s + 8, 8);
    name[8] = 0;
    for (int i = 0; i < 8; i++) if (name[i] && (name[i] < 32 || name[i] > 126)) name[i] = '?';
}

// Returns true for VCTrainer's create-char (009A run by its script "vctrain"), with
// the model it asked for.
static bool NoteMaker(DWORD from, int* madeModel) {
    char name[9] = "";
    int type = -1, model = -1;
    const void* script = nullptr;
    int typeAt = -1;   // ScriptParams index of the type; the model follows it
    switch (from) {
    case 0x449675: case 0x4496AC: case 0x4496CB: script = g_pedNewEbp; typeAt = 0; break;
    case 0x445E7B: case 0x445EA6: case 0x445ECA: script = g_pedNewEbx; typeAt = 1; break;
    case 0x45295B: case 0x452986: case 0x4529AA: script = g_pedNewEbp; typeAt = 1; break;
    case 0x45E044: if (g_pedNewEsp) script = *(void* const*)(g_pedNewEsp + 8 + 0x18); break;
    }
    if (ActiveScript(script)) {
        ScriptName((const unsigned char*)script, name);
        if (typeAt >= 0) { type = ((int*)0x7D7438)[typeAt]; model = ((int*)0x7D7438)[typeAt + 1]; }
    }
    const bool trainer = (from == 0x449675 || from == 0x4496AC || from == 0x4496CB) && !strcmp(name, "vctrain");
    *madeModel = model;
    for (Maker& m : g_makers) {
        if (m.count && m.from == from && !strcmp(m.script, name)) { m.count++; m.type = type; m.model = model; return trainer; }
        if (!m.count) { m.from = from; memcpy(m.script, name, 9); m.type = type; m.model = model; m.count = 1; return trainer; }
    }
    return trainer;
}

static void LogMakers(int used) {
    const DWORD now = *(volatile DWORD*)0x974B2C;
    if (now - g_makerLogAt < 30000 && now >= g_makerLogAt) return;
    g_makerLogAt = now;
    char line[600]; int n = 0;
    for (const Maker& m : g_makers) {
        if (!m.count || n > 500) continue;
        if (m.script[0] && m.type >= 0) n += wsprintfA(line + n, "%X (script %s, type %d, model %d) x%d; ", m.from, m.script, m.type, m.model, m.count);
        else if (m.script[0]) n += wsprintfA(line + n, "%X (script %s) x%d; ", m.from, m.script, m.count);
        else n += wsprintfA(line + n, "%X x%d; ", m.from, m.count);
    }
    Log("Population: %d people; made since the last line by: %s", used, n ? line : "-");
    memset(g_makers, 0, sizeof g_makers);
    char ref[400]; int r = 0;
    for (int k = 0; k < K_COUNT; k++)
        if (g_refused[k]) r += wsprintfA(ref + r, "%s %ld (%ld in all); ", kKindName[k], g_refused[k], g_refusedTotal[k]);
    const long skips = g_charSkips;
    if (skips != g_charSkipsSeen) r += wsprintfA(ref + r, "script commands on missing characters skipped %ld; ", skips - g_charSkipsSeen);
    if (g_bodiesCleared) r += wsprintfA(ref + r, "dead bodies handed to the game's fade and removal %ld (%ld in all); ",
                                        g_bodiesCleared, g_bodiesClearedTotal);
    g_bodiesCleared = 0;
    if (r) Log("Population:   refused since the last line: %s", ref);
    if (g_bodyStats.dead) {
        const BodyStats& b = g_bodyStats;
        Log("Population:   bodies at the last scan: %d dead (oldest %d s), %d of them VCTrainer's not yet released; fading %d "
            "(%d flagged by the game), %d more could go; left alone: too young (15 s, 5 s while short) %d, mission/script %d, in a vehicle %d, "
            "a living ped's target or leader %d, not deletable %d, under the player %d, the player's lock-on target %d, other %d; "
            "no longer kept (the old test): within 2 m flat %d, a pointer in the player ped %d (last at +0x%X), in the player info %d, "
            "in the camera %d; since the start: VCTrainer bodies released %ld, removed by the game %ld, faded but removed here %ld, kept by the game %ld",
            b.dead, b.oldest, b.trainer, b.fading, b.flagged, b.eligible, b.young, b.notRandom, b.inVehicle, b.held, b.notDeletable,
            b.under, b.aimed, b.other, b.near2m, b.refPlayer, b.refField < 0 ? 0 : b.refField, b.refInfo, b.refCamera,
            g_trainerReleased, g_bodiesRemovedByGame, g_bodiesRemovedHere, g_bodiesKeptByGame);
    }
    if (skips != g_charSkipsSeen) {
        char by[400];
        if (SkipBreakdown(by, sizeof by)) Log("Population:   skipped commands by script: %s", by);
    }
    g_charSkipsSeen = skips;
    memset(g_refused, 0, sizeof g_refused);
}

// ---------------------------------------------------------------- CPed::operator new
// Script create-character sites whose "none" path is installed (their ped-new
// return addresses); each is refused once only 8 slots are left and nobody can be
// removed, so the game's own makers keep those 8. The road block's cops (0x444041,
// CRoadBlocks) are refused at 2.
static DWORD g_scriptSafe[16];
static int g_nScriptSafe = 0;
static DWORD g_copSafe[4];
static int g_nCopSafe = 0;
static void AddSafe(DWORD* list, int& n, int cap, DWORD from) { if (n < cap) list[n++] = from; }
static bool InList(const DWORD* list, int n, DWORD from) {
    for (int i = 0; i < n; i++) if (list[i] == from) return true;
    return false;
}

extern "C" void* __cdecl vcmf_ped_new_c(unsigned) {
    const DWORD from = (DWORD)__builtin_return_address(0);
    if (InList(g_scriptSafe, g_nScriptSafe, from)) {
        const int free = FreePedSlots();
        if (free <= 8 && !EvictOnePed()) { Refuse(K_SCRIPT, from, free); return nullptr; }
    } else if (InList(g_copSafe, g_nCopSafe, from)) {
        const int free = FreePedSlots();
        if (free <= 2 && !EvictOnePed()) { Refuse(K_COPS, from, free); return nullptr; }
    }
    void* p = ((void* (__thiscall*)(void*))POOL_NEW)(*(void**)0x97F2AC);
    if (!p && EvictOnePed()) p = ((void* (__thiscall*)(void*))POOL_NEW)(*(void**)0x97F2AC);
    if (p) {
        g_recent[g_recentNext++ % 256] = { p, *(volatile DWORD*)0x974B2C };
        int model = -1;
        const bool trainer = NoteMaker(from, &model);
        MarkMade(p, trainer, model);   // every new ped rewrites its slot's record
    }
    return p;
}
extern "C" void vcmf_ped_new();
__asm__(".text\n"
        ".globl _vcmf_ped_new\n_vcmf_ped_new:\n"
        "  movl %ebp, _g_pedNewEbp\n"
        "  movl %ebx, _g_pedNewEbx\n"
        "  movl %esp, _g_pedNewEsp\n"
        "  jmp _vcmf_ped_new_c\n");

// The script's create-character command (0x009A, the one VCTrainer uses) no longer
// writes to nothing (0x4496E5: mov ebx, eax; mov byte [ebx+0x160], 2): with no ped
// it skips to where the new handle is stored (0x449862: mov [ScriptParams], eax;
// then the script's output variable) with -1, "no character", which the script
// reads as a failed create.
extern "C" { volatile long g_createCharFails = 0; }
extern "C" void vcmf_create_char_null();
__asm__(".text\n"
        ".globl _vcmf_create_char_null\n_vcmf_create_char_null:\n"
        "  testl %eax, %eax\n  jz 1f\n"
        "  movl %eax, %ebx\n"
        "  movb $2, 0x160(%ebx)\n"
        "  pushl $0x4496EE\n  ret\n"
        "1: lock incl _g_createCharFails\n"
        "  movl $-1, %eax\n"
        "  pushl $0x449862\n  ret\n");

static void InstallCreateCharGuard() {
    static const unsigned char orig[] = { 0x89, 0xC3, 0xC6, 0x83, 0x60, 0x01, 0x00, 0x00, 0x02 };   // mov ebx, eax (89 C3)
    if (memcmp((void*)0x4496E5, orig, sizeof orig) || *(const unsigned char*)0x449862 != 0xA3) {
        Log("Population: create-character code is not v1.0; its full-pool guard not installed");
        return;
    }
    unsigned char code[9] = { 0xE9, 0, 0, 0, 0, 0x90, 0x90, 0x90, 0x90 };
    *(int*)(code + 1) = (int)((DWORD)&vcmf_create_char_null - (0x4496E5 + 5));
    Patch(0x4496E5, code, sizeof code);
    AddSafe(g_scriptSafe, g_nScriptSafe, 16, 0x449675);
    AddSafe(g_scriptSafe, g_nScriptSafe, 16, 0x4496AC);
    AddSafe(g_scriptSafe, g_nScriptSafe, 16, 0x4496CB);
    Log("Population: a script creating a character into a full pool gets \"none\" instead of crashing (0x4496E7)");
}

// ---------------------------------------------------------------- null exits
// Each maker below wrote to the ped it was given without checking it. A jump at
// that spot goes to a stub that does the original instructions when there is a
// ped, and otherwise counts the miss (g_nullHits) and leaves through the maker's
// own "made nobody" path, checked to leave the stack and the FPU as that path
// expects:
//   0  0x53CB45 CPopulation::AddToPopulation's group loop: the group ends here
//      (0x53CF01, its return).
//   1  0x53AC36 CPopulation::AddPedInCar: returns 0 (0x53AC90).
//   2  0x53B488 CPopulation::AddDeadPedInFrontOfCar: returns 0 (0x53B3C0); its
//      caller (0x429044) checks.
//   3  0x5AC150 a police helicopter's rope crew (0x5ABE20): returns false (0x5AC1C4).
//   4  0x6325A9 a SWAT member down a rope (0x632550): returns false (0x632600).
//   5  0x5B877C CVehicle::SetupPassenger: returns the empty seat, 0 (0x5B8856).
//   6  0x5B8896 CVehicle::SetUpDriver: returns 0 (0x5B8913).
//   7  0x419C5B, 8 0x419C76 CCarCtrl::SetUpDriverAndPassengersForVehicle sets a flag
//      on the driver and passenger it got: skipped for one that isn't there.
//   9  0x62FD2F 0560 CREATE_RANDOM_CHAR_AS_DRIVER, 10 0x62FEA7 0561
//      CREATE_RANDOM_CHAR_AS_PASSENGER, 11 0x445EE4 0129 CREATE_CHAR_INSIDE_CAR,
//      12 0x4529C4 01C8 CREATE_CHAR_AS_PASSENGER, 13 0x45E055 0376
//      CREATE_RANDOM_CHAR: the script gets handle -1, like 009A above.
//   14 0x44409E, 15 0x4440BF CRoadBlocks' cops (0x444041): no more cops for that
//      road block (0x444265, its end).
extern "C" {
void vcmf_nx_group(); void vcmf_nx_pedincar(); void vcmf_nx_deadped(); void vcmf_nx_helicrew();
void vcmf_nx_rope(); void vcmf_nx_passenger(); void vcmf_nx_driver(); void vcmf_nx_spdriver();
void vcmf_nx_sppass(); void vcmf_nx_s0560(); void vcmf_nx_s0561(); void vcmf_nx_s0129();
void vcmf_nx_s01c8(); void vcmf_nx_s0376(); void vcmf_nx_rb1(); void vcmf_nx_rb2();
}
__asm__(".text\n"
        "_vcmf_nx_group:\n"
        "  addl $0x10, %esp\n  testl %eax, %eax\n  jz 1f\n"
        "  cmpb $0, 0x24(%esp)\n  pushl $0x53CB4D\n  ret\n"
        "1: lock incl _g_nullHits+0\n  pushl $0x53CF01\n  ret\n"

        "_vcmf_nx_pedincar:\n"
        "  movl %eax, %esi\n  addl $0x10, %esp\n  testl %esi, %esi\n  jz 1f\n"
        "  movb 0x51(%esi), %al\n  pushl $0x53AC3E\n  ret\n"
        "1: lock incl _g_nullHits+4\n  xorl %eax, %eax\n  pushl $0x53AC90\n  ret\n"

        "_vcmf_nx_deadped:\n"
        "  movl %eax, %ebx\n  addl $0x10, %esp\n  testl %ebx, %ebx\n  jz 1f\n"
        "  pushl $0x53B48D\n  ret\n"
        "1: lock incl _g_nullHits+8\n  pushl $0x53B3C0\n  ret\n"

        "_vcmf_nx_helicrew:\n"
        "  movl %eax, %ebp\n  addl $0x10, %esp\n  testl %ebp, %ebp\n  jz 1f\n"
        "  movb 0x51(%ebp), %al\n  pushl $0x5AC158\n  ret\n"
        "1: lock incl _g_nullHits+12\n  pushl $0x5AC1C4\n  ret\n"

        "_vcmf_nx_rope:\n"
        "  addl $0x10, %esp\n  testl %eax, %eax\n  jz 1f\n"
        "  movb 0x51(%eax), %dl\n  pushl $0x6325AF\n  ret\n"
        "1: lock incl _g_nullHits+16\n  pushl $0x632600\n  ret\n"

        "_vcmf_nx_passenger:\n"
        "  movl %eax, %esi\n  popl %ecx\n  testl %esi, %esi\n  jz 1f\n"
        "  movswl 0x5C(%esi), %eax\n  pushl $0x5B8783\n  ret\n"
        "1: popl %ecx\n  lock incl _g_nullHits+20\n  pushl $0x5B8856\n  ret\n"

        "_vcmf_nx_driver:\n"
        "  testl %eax, %eax\n  jz 1f\n"
        "  movl %eax, 0x1A8(%ebx)\n  pushl $0x5B889C\n  ret\n"
        "1: addl $8, %esp\n  lock incl _g_nullHits+24\n  pushl $0x5B8913\n  ret\n"

        "_vcmf_nx_spdriver:\n"
        "  testl %eax, %eax\n  jz 1f\n"
        "  movb 0x155(%eax), %cl\n  pushl $0x419C61\n  ret\n"
        "1: lock incl _g_nullHits+28\n  pushl $0x419C6D\n  ret\n"

        "_vcmf_nx_sppass:\n"
        "  testl %eax, %eax\n  jz 1f\n"
        "  movb 0x155(%eax), %dl\n  pushl $0x419C7C\n  ret\n"
        "1: lock incl _g_nullHits+32\n  pushl $0x419C88\n  ret\n"

        "_vcmf_nx_s0560:\n"
        "  testl %eax, %eax\n  jz 1f\n"
        "  movl %eax, %ebp\n  popl %ecx\n  movb $2, 0x160(%ebp)\n  pushl $0x62FD39\n  ret\n"
        "1: addl $8, %esp\n  lock incl _g_nullHits+36\n  movl $-1, %eax\n  pushl $0x62FE36\n  ret\n"

        "_vcmf_nx_s0561:\n"
        "  testl %eax, %eax\n  jz 1f\n"
        "  movl %eax, %edi\n  popl %ecx\n  movb $2, 0x160(%edi)\n  pushl $0x62FEB1\n  ret\n"
        "1: addl $8, %esp\n  lock incl _g_nullHits+40\n  movl $-1, %eax\n  pushl $0x62FF8E\n  ret\n"

        "_vcmf_nx_s0129:\n"
        "  testl %eax, %eax\n  jz 1f\n"
        "  movl %eax, %ebp\n  movb $2, 0x160(%ebp)\n  pushl $0x445EED\n  ret\n"
        "1: lock incl _g_nullHits+44\n  movl $-1, %eax\n  pushl $0x44608C\n  ret\n"

        "_vcmf_nx_s01c8:\n"
        "  testl %eax, %eax\n  jz 1f\n"
        "  movl %eax, %ebx\n  movb $2, 0x160(%ebx)\n  pushl $0x4529CD\n  ret\n"
        "1: lock incl _g_nullHits+48\n  movl $-1, %eax\n  pushl $0x452B39\n  ret\n"

        "_vcmf_nx_s0376:\n"
        "  testl %eax, %eax\n  jz 1f\n"
        "  movl %eax, %ebx\n  movb $2, 0x160(%ebx)\n  pushl $0x45E05E\n  ret\n"
        "1: lock incl _g_nullHits+52\n  movl $-1, %eax\n  pushl $0x45E1E5\n  ret\n"

        "_vcmf_nx_rb1:\n"
        "  popl %ecx\n  testl %eax, %eax\n  jz 1f\n  pushl $0x4440A3\n  ret\n"
        "1: lock incl _g_nullHits+56\n  pushl $0x444265\n  ret\n"

        "_vcmf_nx_rb2:\n"
        "  popl %ecx\n  testl %eax, %eax\n  jz 1f\n  pushl $0x4440C4\n  ret\n"
        "1: lock incl _g_nullHits+60\n  pushl $0x444265\n  ret\n");

struct NullExit { DWORD at; unsigned char n; unsigned char orig[10]; void (*stub)(); };
static const NullExit kNullExits[16] = {
    { 0x53CB45, 8, { 0x83, 0xC4, 0x10, 0x80, 0x7C, 0x24, 0x24, 0x00 }, vcmf_nx_group },
    { 0x53AC36, 8, { 0x89, 0xC6, 0x83, 0xC4, 0x10, 0x8A, 0x46, 0x51 }, vcmf_nx_pedincar },
    { 0x53B488, 5, { 0x89, 0xC3, 0x83, 0xC4, 0x10 }, vcmf_nx_deadped },
    { 0x5AC150, 8, { 0x89, 0xC5, 0x83, 0xC4, 0x10, 0x8A, 0x45, 0x51 }, vcmf_nx_helicrew },
    { 0x6325A9, 6, { 0x8A, 0x50, 0x51, 0x83, 0xC4, 0x10 }, vcmf_nx_rope },
    { 0x5B877C, 7, { 0x89, 0xC6, 0x59, 0x0F, 0xBF, 0x46, 0x5C }, vcmf_nx_passenger },
    { 0x5B8896, 6, { 0x89, 0x83, 0xA8, 0x01, 0x00, 0x00 }, vcmf_nx_driver },
    { 0x419C5B, 6, { 0x8A, 0x88, 0x55, 0x01, 0x00, 0x00 }, vcmf_nx_spdriver },
    { 0x419C76, 6, { 0x8A, 0x90, 0x55, 0x01, 0x00, 0x00 }, vcmf_nx_sppass },
    { 0x62FD2F, 10, { 0x89, 0xC5, 0x59, 0xC6, 0x85, 0x60, 0x01, 0x00, 0x00, 0x02 }, vcmf_nx_s0560 },
    { 0x62FEA7, 10, { 0x89, 0xC7, 0x59, 0xC6, 0x87, 0x60, 0x01, 0x00, 0x00, 0x02 }, vcmf_nx_s0561 },
    { 0x445EE4, 9, { 0x89, 0xC5, 0xC6, 0x85, 0x60, 0x01, 0x00, 0x00, 0x02 }, vcmf_nx_s0129 },
    { 0x4529C4, 9, { 0x89, 0xC3, 0xC6, 0x83, 0x60, 0x01, 0x00, 0x00, 0x02 }, vcmf_nx_s01c8 },
    { 0x45E055, 9, { 0x89, 0xC3, 0xC6, 0x83, 0x60, 0x01, 0x00, 0x00, 0x02 }, vcmf_nx_s0376 },
    { 0x44409E, 5, { 0x85, 0xC0, 0x59, 0x74, 0x0A }, vcmf_nx_rb1 },
    { 0x4440BF, 5, { 0x85, 0xC0, 0x59, 0x74, 0x0B }, vcmf_nx_rb2 },
};
static bool g_nullExitOk[16];

// The stores the script exits jump to: mov [ScriptParams], eax.
static bool ScriptStoreOk(int i) {
    static const DWORD store[16] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x62FE36, 0x62FF8E, 0x44608C, 0x452B39, 0x45E1E5, 0, 0 };
    if (!store[i]) return true;
    static const unsigned char mov[] = { 0xA3, 0x38, 0x74, 0x7D, 0x00 };
    return !memcmp((void*)store[i], mov, sizeof mov);
}

static void InstallNullExits() {
    int ok = 0;
    for (int i = 0; i < 16; i++) {
        const NullExit& e = kNullExits[i];
        if (memcmp((void*)e.at, e.orig, e.n) || !ScriptStoreOk(i)) {
            Log("Population: code at %X is not v1.0; %s keeps no full-pool exit", e.at, kNullName[i]);
            continue;
        }
        unsigned char code[16];
        memset(code, 0x90, sizeof code);
        code[0] = 0xE9;
        *(int*)(code + 1) = (int)((DWORD)e.stub - (e.at + 5));
        Patch(e.at, code, e.n);
        g_nullExitOk[i] = true;
        ok++;
    }
    // Script sites the exits made safe: refused at 8 free (vcmf_ped_new_c).
    if (g_nullExitOk[11]) { AddSafe(g_scriptSafe, g_nScriptSafe, 16, 0x445E7B); AddSafe(g_scriptSafe, g_nScriptSafe, 16, 0x445EA6); AddSafe(g_scriptSafe, g_nScriptSafe, 16, 0x445ECA); }
    if (g_nullExitOk[12]) { AddSafe(g_scriptSafe, g_nScriptSafe, 16, 0x45295B); AddSafe(g_scriptSafe, g_nScriptSafe, 16, 0x452986); AddSafe(g_scriptSafe, g_nScriptSafe, 16, 0x4529AA); }
    if (g_nullExitOk[13]) AddSafe(g_scriptSafe, g_nScriptSafe, 16, 0x45E044);
    if (g_nullExitOk[14]) AddSafe(g_copSafe, g_nCopSafe, 4, 0x44409E);
    if (g_nullExitOk[15]) AddSafe(g_copSafe, g_nCopSafe, 4, 0x4440BF);
    Log("Population: %d of 16 ped makers make nobody instead of crashing when the pool is full", ok);
}

// ---------------------------------------------------------------- a reaction to a removed ped
// A ped's threat scan (0x50BE00) can pick an event from the event list (0x7DAAB8)
// caused by another ped: that ped goes in +0x1A0 (a registered reference) and
// +0x5BC gets 0x2000000; the reaction runs after a delay (+0x5C0). If that ped is
// removed meanwhile (the game's body removal, our clearing, a street person made
// room for, a script deleting it) the reference turns +0x1A0 to 0. The check before
// the reaction (0x50BD00) drops the event only when +0x1A0 is still set and no
// longer valid, or when +0x190 is also 0, so with another threat in +0x190 the
// flag stays, and the 0x2000000 case (0x4E94E0, in 0x4E8E20) reads the removed
// ped's position at 0+0x34 (crash at 0x4E94EB). The 0x100000 case checks its ped
// (0x4E8EDD) and leaves through 0x4E9980, which drops the three values the FPU
// holds there and returns; the 0x2000000 case gets the same exit for a 0.
extern "C" void vcmf_event_ped_gone();
__asm__(".text\n"
        "_vcmf_event_ped_gone:\n"
        "  movl 0x1A0(%ebp), %edx\n  testl %edx, %edx\n  jz 1f\n"
        "  pushl $0x4E94E6\n  ret\n"
        "1: lock incl _g_eventPedGone\n  pushl $0x4E9980\n  ret\n");

static void InstallEventPedGuard() {
    static const unsigned char site[] = { 0x8B, 0x95, 0xA0, 0x01, 0x00, 0x00, 0x8D, 0x4A, 0x34 };   // mov edx,[ebp+1A0h]; lea ecx,[edx+34h]
    static const unsigned char exitPath[] = { 0xDE, 0xD9, 0xDD, 0xD8, 0xEB, 0xD5 };                // fcompp; fstp st0; jmp 0x4E995B
    static const unsigned char branch[] = { 0x0F, 0x84, 0x0E, 0x06, 0x00, 0x00 };                  // 0x4E8ECC: je 0x4E94E0
    static const unsigned char nullTest[] = { 0x85, 0xD2, 0x0F, 0x84, 0x9B, 0x0A, 0x00, 0x00 };      // 0x4E8EDD: test edx,edx; je 0x4E9980
    if (memcmp((void*)0x4E94E0, site, sizeof site) || memcmp((void*)0x4E9980, exitPath, sizeof exitPath) ||
        memcmp((void*)0x4E8ECC, branch, sizeof branch) || memcmp((void*)0x4E8EDD, nullTest, sizeof nullTest)) {
        Log("Population: ped reaction code at 0x4E94E0 is not v1.0; its removed-ped check not installed");
        return;
    }
    unsigned char code[6] = { 0xE9, 0, 0, 0, 0, 0x90 };
    *(int*)(code + 1) = (int)((DWORD)&vcmf_event_ped_gone - (0x4E94E0 + 5));
    Patch(0x4E94E0, code, sizeof code);
    Log("Population: a ped's delayed reaction to a ped removed meanwhile is skipped (crash at 0x4E94EB)");
}

// ---------------------------------------------------------------- CPopulation::AddPed
// AddPed (0x53B600, cdecl: type, model, position, flag) gets its ped from
// CPed::operator new at six places and writes the position without checking (crash
// at 0x53B653). Its start goes here: with no slot free (and nobody to remove) it
// returns 0, as the null exits above and its other callers (0x538C2A, 0x538D03,
// 0x539607, 0x539DD3, 0x53A2ED, 0x53A421, all checking) take. Street people
// (AddToPopulation, attractors, the dead in front of a car) leave the last 4 slots
// to drivers, passengers and crews.
static void* g_addPedTramp = nullptr;
static int g_addPedOk = 0;

static bool CallerTakesNone(DWORD from) {
    switch (from) {
    case 0x538C2F: case 0x538D08: case 0x53960C: case 0x539DD8: case 0x53A2F2: case 0x53A426: return true;
    case 0x53CB45: return g_nullExitOk[0];
    case 0x53AC36: return g_nullExitOk[1];
    case 0x53B488: return g_nullExitOk[2];
    case 0x5AC150: return g_nullExitOk[3];
    case 0x6325A9: return g_nullExitOk[4];
    }
    return false;
}

// Street cops ([Population] MaxStreetCops). The zones' ped groups include cops
// (ped type 6, CPed +0x3D4), most of all around the police headquarters in
// Washington Beach; with the higher ped density and peds kept further away
// offscreen they filled whole streets there while the player wasn't wanted. A
// street person who would be a cop (type 6, from the street callers) is made as a
// civilian instead (the type and model of one about now) while the player has no
// wanted level (player ped +0x5F4 -> CWanted +0x20) and that many cops are already
// about. Cops sent after a wanted player, in cars and at
// roadblocks don't come this way and are left alone.
static int CountCops() {
    const unsigned char* pool = *(const unsigned char* const*)0x97F2AC;
    if (!pool) return 0;
    const unsigned char* objs = *(const unsigned char* const*)pool;
    const unsigned char* flags = *(const unsigned char* const*)(pool + 4);
    const int size = *(const int*)(pool + 8);
    if (!objs || !flags || size <= 0 || size > 4096) return 0;
    int n = 0;
    for (int i = 0; i < size; i++)
        if (!(flags[i] & 0x80) && *(const int*)(objs + i * 0x6D8 + 0x3D4) == 6) n++;
    return n;
}
static bool PlayerWanted() {
    // CWorld::Players[0].m_pPed (0x94AD28), not the camera's target (0x7E4E94): that
    // is the car while driving, and its +0x5F4 isn't a wanted record (crash reading it)
    const unsigned char* ped = *(const unsigned char* const*)0x94AD28;
    if (!ped) return false;
    const unsigned char* w = *(const unsigned char* const*)(ped + 0x5F4);
    return w && *(const int*)(w + 0x20) > 0;
}
static long g_copsRefused = 0, g_copsSwapped = 0;

// A civilian (ped type 4 or 5) about now, picked in turn, for the type and model of
// a street person made instead of a cop: its model is loaded, being in use.
static bool PickCivilian(int* type, int* model) {
    const unsigned char* pool = *(const unsigned char* const*)0x97F2AC;
    if (!pool) return false;
    const unsigned char* objs = *(const unsigned char* const*)pool;
    const unsigned char* flags = *(const unsigned char* const*)(pool + 4);
    const int size = *(const int*)(pool + 8);
    if (!objs || !flags || size <= 0 || size > 4096) return false;
    static int start = 0;
    for (int k = 0; k < size; k++) {
        const int i = (start + k) % size;
        if (flags[i] & 0x80) continue;
        const unsigned char* p = objs + i * 0x6D8;
        const int t = *(const int*)(p + 0x3D4);
        const int m = *(const short*)(p + 0x5C);
        if ((t == 4 || t == 5) && m > 0 && m < 6500 && *(const void* const*)(0x92D4C8 + m * 4)) {
            start = i + 1;
            *type = t;
            *model = m;
            return true;
        }
    }
    // No civilian about (near the police station nearly every ped picked is a cop):
    // a civilian model the streamer has loaded (CStreaming::ms_aInfoForModel 0x94DDD0,
    // 20 bytes a model, +8 state 1 = loaded), from default.ide's CIVMALE (type 4) and
    // CIVFEMALE (type 5) peds.
    static const unsigned char kMale[] = { 7, 11, 12, 15, 16, 19, 20, 26, 27, 28, 29, 33, 34, 37, 39, 41, 42, 45, 46, 47, 51,
                                           52, 55, 56, 58, 60, 61, 62, 64, 66, 68, 69, 72, 74, 76, 78, 82 };
    static const unsigned char kFemale[] = { 9, 10, 13, 14, 17, 18, 21, 22, 23, 25, 31, 32, 35, 36, 38, 40, 44, 49, 50, 53, 54,
                                             57, 59, 63, 65, 67, 71, 75, 77, 79, 80, 81 };
    static int turn = 0;
    const int nm = sizeof kMale, nf = sizeof kFemale, n = nm + nf;
    for (int k = 0; k < n; k++) {
        const int j = (turn + k) % n;
        const int id = j < nm ? kMale[j] : kFemale[j - nm];
        if (*(const unsigned char*)(0x94DDD0 + id * 20 + 8) != 1 || !*(const void* const*)(0x92D4C8 + id * 4)) continue;
        turn = j + 1;
        *type = j < nm ? 4 : 5;
        *model = id;
        return true;
    }
    return false;
}

static void* __cdecl AddPedGuard(int type, int model, void* pos, int flag) {
    const DWORD from = (DWORD)__builtin_return_address(0);
    if (CallerTakesNone(from)) {
        const bool street = from != 0x53AC36 && from != 0x5AC150 && from != 0x6325A9;
        if (street && type == 6 && g_cfg.maxStreetCops >= 0 && !PlayerWanted() && CountCops() >= g_cfg.maxStreetCops) {
            // made as an ordinary street person instead (refusing left the streets by
            // the police station empty: nearly every ped picked there is a cop)
            if (PickCivilian(&type, &model)) {
                if (g_copsSwapped++ < 3)
                    Log("Population: %d cops about already; a street cop made as a civilian instead (MaxStreetCops)",
                        g_cfg.maxStreetCops);
            } else {
                if (g_copsRefused++ < 3)
                    Log("Population: %d cops about already and no civilian model to use; a street cop not made",
                        g_cfg.maxStreetCops);
                return nullptr;
            }
        }
        const int need = street ? 4 : 1;
        int free = FreePedSlots();
        if (free < need && !street && EvictOnePed()) free = FreePedSlots();
        if (free < need) {
            Refuse(street ? K_STREET : from == 0x53AC36 ? K_VEHICLE : K_COPS, from, free);
            return nullptr;
        }
    }
    return ((void* (__cdecl*)(int, int, void*, int))g_addPedTramp)(type, model, pos, flag);
}

static void InstallAddPedGuard() {
    static const unsigned char orig[] = { 0x53, 0x56, 0x57, 0x55, 0x83, 0xEC, 0x58 };   // push ebx..ebp; sub esp, 58h
    if (memcmp((void*)0x53B600, orig, sizeof orig)) {
        Log("Population: CPopulation::AddPed is not v1.0; it keeps no full-pool check");
        return;
    }
    unsigned char* t = (unsigned char*)VirtualAlloc(nullptr, 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!t) return;
    memcpy(t, orig, sizeof orig);
    t[7] = 0xE9;
    *(int*)(t + 8) = (int)(0x53B607 - ((DWORD)t + 12));
    g_addPedTramp = t;
    unsigned char jmp[7] = { 0xE9, 0, 0, 0, 0, 0x90, 0x90 };
    *(int*)(jmp + 1) = (int)((DWORD)&AddPedGuard - (0x53B600 + 5));
    Patch(0x53B600, jmp, sizeof jmp);
    g_addPedOk = 1;
    Log("Population: CPopulation::AddPed makes nobody when the pool is full (crash at 0x53B653)");
}

// ---------------------------------------------------------------- cars
// A traffic car (CCarCtrl::GenerateOneRandomCar 0x426DB0, called at 0x4292D6 and
// 0x4292F0) or an ambulance / fire truck (0x42AE70, at 0x42BB86) comes with a
// driver and passengers; with fewer than 6 slots free that car is skipped this
// time, as when the game finds no road for it.
static int g_carGuards = 0;
static void __cdecl GenerateCarIfRoom() {
    const int free = FreePedSlots();
    if (free < 6) { Refuse(K_TRAFFIC, 0x426DB0, free); return; }
    ((void (__cdecl*)())0x426DB0)();
}
static void __cdecl GenerateEmergencyCarIfRoom() {
    const int free = FreePedSlots();
    if (free < 6) { Refuse(K_EMERGENCY, 0x42AE70, free); return; }
    ((void (__cdecl*)())0x42AE70)();
}
static void RedirectCall(DWORD at, DWORD target, void* to, const char* what) {
    const unsigned char* p = (const unsigned char*)at;
    if (p[0] != 0xE8 || at + 5 + *(const int*)(p + 1) != target) {
        Log("Population: %s call at %X is not v1.0; no full-pool check there", what, at);
        return;
    }
    unsigned char call[5] = { 0xE8 };
    *(int*)(call + 1) = (int)((DWORD)to - (at + 5));
    Patch(at, call, sizeof call);
    g_carGuards++;
}

// ---------------------------------------------------------------- handles
// CPool<CPed>::GetAt (0x451CB0) and CPool<CVehicle>::GetAt (0x451C70) turn a
// script handle (slot << 8 | id) into a pointer. They don't check the slot: -1, a
// failed create, read the byte before the flag array and could return a pointer in
// front of the pool. Now a slot outside the pool gives 0, "no such character".
static void* __fastcall SafePedAt(const unsigned char* pool, void*, int handle) {
    const int slot = handle >> 8;
    if (!pool || slot < 0 || slot >= *(const int*)(pool + 8)) return nullptr;
    if ((*(const unsigned char* const*)(pool + 4))[slot] != (handle & 0xFF)) return nullptr;
    return *(unsigned char* const*)pool + slot * 0x6D8;
}
static void* __fastcall SafeVehicleAt(const unsigned char* pool, void*, int handle) {
    const int slot = handle >> 8;
    if (!pool || slot < 0 || slot >= *(const int*)(pool + 8)) return nullptr;
    if ((*(const unsigned char* const*)(pool + 4))[slot] != (handle & 0xFF)) return nullptr;
    return *(unsigned char* const*)pool + slot * 0x5DC;
}
static void InstallSafeGetAt() {
    static const unsigned char head[] = { 0x53, 0x8B, 0x5C, 0x24, 0x08, 0x89, 0xD8, 0x56, 0x89, 0xCE, 0xC1, 0xF8, 0x08 };
    static const unsigned char pedSize[] = { 0x69, 0xC0, 0xD8, 0x06, 0x00, 0x00 }, carSize[] = { 0x69, 0xC0, 0xDC, 0x05, 0x00, 0x00 };
    struct { DWORD at; const unsigned char* size; void* to; const char* what; } sites[] = {
        { 0x451CB0, pedSize, (void*)&SafePedAt, "character" }, { 0x451C70, carSize, (void*)&SafeVehicleAt, "vehicle" } };
    for (auto& s : sites) {
        if (memcmp((void*)s.at, head, sizeof head) || memcmp((void*)(s.at + 0x20), s.size, 6)) {
            Log("Population: %s handle lookup at %X is not v1.0; not bounds-checked", s.what, s.at);
            continue;
        }
        unsigned char jmp[5] = { 0xE9 };
        *(int*)(jmp + 1) = (int)((DWORD)s.to - (s.at + 5));
        Patch(s.at, jmp, sizeof jmp);
        Log("Population: %s handles outside the pool (a failed create's -1) read as \"none\" (%X)", s.what, s.at);
    }
}

// ---------------------------------------------------------------- script commands
// A script holding -1 (or the handle of a character the game removed) and then
// giving it a weapon, an objective, a position... crashed in the command: 143
// commands read their first parameter as a character (CPools GetAt right after
// CollectParameters) and use it without checking. Before such a command runs
// (CRunningScript::ProcessOneCommand, hooked in stuntjump.cpp), its character is
// read the same way (CollectParameters 0x451010 on a copy of the ip); when there is
// none, the command is stepped over (its parameters skipped by type) and a
// condition reads false (UpdateCompareFlag 0x463F00, with the command's NOT flag at
// +0x82), as IS_CHAR_DEAD already treats a missing character as dead. Inputs and
// outputs per command, and which are conditions, were read from each v1.0 handler.
// 00A0 has its own guard (missionhelp.cpp); 00D9 / 0154 / 0352 write to the script
// themselves and run as before.
// Not guarded, because the game's own handlers are safe with a missing character
// and also take its handle off the mission clean-up list (CMissionCleanup at
// 0x97F060, 50 slots, RemoveEntityFromList 0x4518E0, which looks the handle up
// itself): 009B DELETE_CHAR (0x449932), 01C2 MARK_CHAR_AS_NO_LONGER_NEEDED
// (0x45272E), 01C5 DONT_REMOVE_CHAR (0x452812) and 034F (0x45D1A7). Skipped, their
// dead handles stayed on the list. The game loads collision around everything on
// that list (and around the player), so once it was full a mission's new vehicle
// far from the player (the vigilante criminals' car, spawned up to 500 m away) got
// no collision and waited for it forever (04F1 in COPCAR): from about level 10 no
// more criminals came.
struct CharCmd { unsigned short op; unsigned char in, out, cond; };
static const CharCmd kCharCmds[] = {
    {0x009C,2,0,0}, {0x009E,6,0,0}, {0x009F,1,0,0}, {0x00A1,4,0,0}, {0x00A3,6,0,1}, {0x00A4,8,0,1},
    {0x00DB,2,0,1}, {0x00DD,2,0,1}, {0x00DF,1,0,1}, {0x0114,3,0,0}, {0x011A,2,0,0}, {0x011C,1,0,0}, {0x0123,2,0,1},
    {0x0126,1,0,1}, {0x0162,3,1,0}, {0x0172,1,1,0}, {0x0173,2,0,0}, {0x0184,2,0,1}, {0x0187,1,1,0}, {0x0192,1,0,0},
    {0x0193,1,0,0}, {0x0194,4,0,0}, {0x01B2,3,0,0}, {0x01B9,2,0,0}, {0x01BE,4,0,0},
    {0x01C9,2,0,0}, {0x01CA,2,0,0}, {0x01CB,2,0,0}, {0x01CC,2,0,0}, {0x01CE,2,0,0}, {0x01CF,2,0,0}, {0x01D0,2,0,0},
    {0x01D1,2,0,0}, {0x01D2,2,0,0}, {0x01D3,2,0,0}, {0x01D4,2,0,0}, {0x01D5,2,0,0}, {0x01D8,2,0,0}, {0x01D9,2,0,0},
    {0x01DE,2,0,0}, {0x01DF,2,0,0}, {0x01E0,1,0,0}, {0x01E1,3,0,0}, {0x01ED,1,0,0}, {0x020E,2,0,0}, {0x020F,2,0,0},
    {0x0211,3,0,0}, {0x0223,2,0,0}, {0x0226,1,1,0}, {0x022C,2,0,0}, {0x022D,2,0,0}, {0x022F,1,0,0}, {0x0239,3,0,0},
    {0x0243,2,0,0}, {0x0245,2,0,0}, {0x0291,2,0,0}, {0x02A9,2,0,0}, {0x02AB,6,0,0}, {0x02CB,1,0,1}, {0x02D8,2,0,1},
    {0x02E0,1,0,1}, {0x02E2,2,0,0}, {0x02F2,2,0,1}, {0x0319,2,0,0}, {0x031D,2,0,1}, {0x031F,2,0,1}, {0x0320,2,0,1},
    {0x0321,1,0,0}, {0x0326,1,1,0}, {0x0332,2,0,0}, {0x0337,2,0,0}, {0x0350,2,0,0}, {0x0353,1,0,0},
    {0x035F,2,0,0}, {0x0362,4,0,0}, {0x0365,1,0,0}, {0x036A,2,0,0}, {0x0372,3,0,0}, {0x0377,1,0,0}, {0x039E,2,0,0},
    {0x03A3,1,0,1}, {0x03C0,1,1,0}, {0x03E2,1,0,0}, {0x03F9,3,0,0}, {0x03FE,2,0,0}, {0x0411,2,0,0}, {0x0433,2,0,0},
    {0x0446,2,0,0}, {0x0448,2,0,1}, {0x0449,1,0,1}, {0x044B,1,0,1}, {0x0464,8,0,0}, {0x046B,2,0,0}, {0x046D,1,1,0},
    {0x0470,1,1,0}, {0x047A,1,0,1}, {0x0480,2,0,1}, {0x0483,2,0,0}, {0x0489,2,0,0}, {0x048F,1,0,0}, {0x04A5,1,3,0},
    {0x04AD,1,0,1}, {0x04B8,2,3,0}, {0x04C2,2,0,0}, {0x04C4,4,3,0}, {0x04C5,1,0,1}, {0x04C6,2,0,0}, {0x04D7,2,0,0},
    {0x04D8,2,0,0}, {0x04DD,1,1,0}, {0x04EB,3,0,0}, {0x04F0,1,0,1}, {0x04F3,1,0,0}, {0x04F4,8,0,0}, {0x04F5,3,0,0},
    {0x04FF,1,0,1}, {0x0502,3,0,0}, {0x050D,1,0,1}, {0x0510,5,0,1}, {0x0514,3,0,0}, {0x051A,2,0,1}, {0x0521,1,0,1},
    {0x0526,2,0,0}, {0x052B,2,0,0}, {0x053C,2,0,0}, {0x053D,1,0,0}, {0x0549,1,0,0}, {0x054A,2,0,0}, {0x0562,2,0,0},
    {0x0568,2,0,0}, {0x056B,2,0,0}, {0x056C,1,0,1}, {0x056D,1,0,1}, {0x0571,1,0,1}, {0x0573,2,0,0}, {0x0579,1,0,0},
    {0x0580,2,1,0}, {0x0584,1,0,0}, {0x0586,1,0,1}, {0x0588,2,0,0}, {0x058B,1,0,1}, {0x0593,2,0,0}, {0x0597,1,0,1},
};
static unsigned char g_charCmd[0x600];   // opcode -> index + 1 in kCharCmds

// Skipped commands per script and command since the last census line, so the
// line shows which script sends the bursts.
struct Skip { char script[9]; unsigned short op; long count; };
static Skip g_skips[16];
static long g_skipsOther = 0;
static void NoteSkip(const char* name, unsigned short op) {
    for (Skip& s : g_skips) {
        if (s.count && s.op == op && !strcmp(s.script, name)) { s.count++; return; }
        if (!s.count) { memcpy(s.script, name, 9); s.op = op; s.count = 1; return; }
    }
    g_skipsOther++;
}
static int SkipBreakdown(char* out, int room) {
    int n = 0;
    for (int pass = 0; pass < 6; pass++) {   // the biggest first, up to 6
        int best = -1;
        for (int i = 0; i < 16; i++)
            if (g_skips[i].count > 0 && (best < 0 || g_skips[i].count > g_skips[best].count)) best = i;
        if (best < 0 || n > room - 40) break;
        n += wsprintfA(out + n, "%s %04X x%ld, ", g_skips[best].script, g_skips[best].op, g_skips[best].count);
        g_skips[best].count = -g_skips[best].count;   // printed
    }
    if (g_skipsOther && n < room - 30) n += wsprintfA(out + n, "others x%ld, ", g_skipsOther);
    memset(g_skips, 0, sizeof g_skips);
    g_skipsOther = 0;
    return n;
}
static bool g_charCmdOk = false;
static int g_charSkipLogs = 0;

static int ParamSize(unsigned char type) {
    switch (type) {
    case 1: case 6: return 5;           // int32, float
    case 2: case 3: case 5: return 3;   // global var, local var, int16
    case 4: return 2;                   // int8
    default: return 0;
    }
}

bool ScriptCharGuard(unsigned char* script) {
    if (!g_charCmdOk) return false;
    DWORD& ip = *(DWORD*)(script + 0x10);
    const unsigned char* p = (const unsigned char*)(0x821280 + ip);
    const unsigned raw = p[0] | p[1] << 8, op = raw & 0x7FFF;
    if (op >= sizeof g_charCmd || !g_charCmd[op]) return false;
    const CharCmd& c = kCharCmds[g_charCmd[op] - 1];
    int len = 2;
    for (int i = 0; i < c.in + c.out; i++) {
        const int size = ParamSize(p[len]);
        if (!size) return false;   // a form these commands don't use: run it as the game would
        len += size;
    }
    DWORD at = ip + 2;
    const int saved = *(int*)0x7D7438;
    ((void (__thiscall*)(void*, DWORD*, int))0x451010)(script, &at, 1);
    const int handle = *(int*)0x7D7438;
    *(int*)0x7D7438 = saved;
    if (SafePedAt(*(const unsigned char* const*)0x97F2AC, nullptr, handle)) return false;
    ip += len;
    if (c.cond) {
        script[0x82] = (raw & 0x8000) ? 1 : 0;
        ((void (__thiscall*)(void*, int))0x463F00)(script, 0);
    }
    InterlockedIncrement(&g_charSkips);
    char name[9];
    ScriptName(script, name);
    NoteSkip(name, (unsigned short)op);
    if (g_charSkipLogs++ < 10) {
        Log("Population: script %s: command %04X on a character that doesn't exist (handle %d) skipped instead of crashing",
            name, op, handle);
    }
    return true;
}

static void InstallScriptCharGuard() {
    // CollectParameters (ip pointer at [esp+1Ch], count at [esp+20h] after its pushes)
    // and UpdateCompareFlag (cmp byte [ecx+82h], 0: the NOT flag) as v1.0 has them
    static const unsigned char collect[] = { 0x53, 0x56, 0x57, 0x55, 0x83, 0xEC, 0x08, 0x8B, 0x7C, 0x24, 0x20 };
    static const unsigned char compare[] = { 0x80, 0xB9, 0x82, 0x00, 0x00, 0x00, 0x00, 0x8B, 0x44, 0x24, 0x04 };
    if (memcmp((void*)0x451010, collect, sizeof collect) || memcmp((void*)0x463F00, compare, sizeof compare)) {
        Log("Population: script parameter code is not v1.0; commands on missing characters run as before");
        return;
    }
    for (unsigned i = 0; i < sizeof kCharCmds / sizeof kCharCmds[0]; i++) g_charCmd[kCharCmds[i].op] = (unsigned char)(i + 1);
    g_charCmdOk = true;
    Log("Population: %u script commands on a character that doesn't exist are skipped instead of crashing",
        (unsigned)(sizeof kCharCmds / sizeof kCharCmds[0]));
}

// ---------------------------------------------------------------- street groups
// CPopulation::AddToPopulation (0x53BA80; cdecl, 4 floats; called by
// CPopulation::Update at 0x53E5C6 and 0x53E99B) makes a whole group at once - a
// gang, a couple, a leader and followers - after one head count. With fewer than
// 16 slots free it adds nobody this time; it is asked again on the next updates,
// once there's room.
static const DWORD ADD_TO_POP = 0x53BA80;
static const DWORD kAddToPopCalls[] = { 0x53E5C6, 0x53E99B };
static int __cdecl AddToPopulationIfRoom(float a, float b, float c, float d) {
    const int free = FreePedSlots();
    if (free < 16) { Refuse(K_STREET, ADD_TO_POP, free); return 0; }
    return ((int (__cdecl*)(float, float, float, float))ADD_TO_POP)(a, b, c, d);
}

static void InstallAddToPopulationGuard() {
    for (DWORD at : kAddToPopCalls) {
        const unsigned char* p = (const unsigned char*)at;
        if (p[0] != 0xE8 || at + 5 + *(const int*)(p + 1) != ADD_TO_POP) {
            Log("Population: street group code at %X is not v1.0; its full-pool guard not installed", at);
            continue;
        }
        unsigned char call[5] = { 0xE8 };
        *(int*)(call + 1) = (int)((DWORD)&AddToPopulationIfRoom - (at + 5));
        Patch(at, call, sizeof call);
    }
    Log("Population: no new street groups while fewer than 16 ped slots are free (crash at 0x53B653)");
}

// A bigger ped pool. CPools::Initialise (0x4C02C7: push 0x8C, the size given to the
// ped pool's constructor) gives it PED_POOL_SLOTS instead. A ped's handle is its
// slot * 256 + an id, so more slots fit; the save keeps only the player. Done at
// load (DllMain), before the game creates its pools.
void PatchPedPoolEarly() {
    static const unsigned char orig[] = { 0x68, 0x8C, 0x00, 0x00, 0x00, 0xE8 };
    if (memcmp((void*)0x4C02C7, orig, sizeof orig)) {
        Log("Population: ped pool code is not v1.0; pool size not changed");
        return;
    }
    const DWORD n = PED_POOL_SLOTS;
    Patch(0x4C02C8, &n, 4);
    Log("Population: ped pool holds %d (game: 140)", PED_POOL_SLOTS);
    // Three places check a ped pointer by its pool slot against a fixed 140
    // (CPool::GetIndex 0x451CF0, sar 8, then cmp eax,0x8C). With the bigger pool,
    // peds in slots 140+ counted as invalid: BuildPedLists left them out of every
    // near-ped list. A scan of the whole .text found no other 140 check on a ped
    // slot and no static 140-entry array indexed by one.
    //  0x4F4860 IsPedPointerValid_NotInWorld: slot < 140 (jl)
    //  0x4F48A0 IsPedPointerValid:            slot > 140 (jg) is invalid; 140 itself
    //           got through (the game's off-by-one), so it gets slots - 1
    //  0x50D7F5 CPed::BuildPedLists, inlined:  slot < 140 (jl)
    {
        struct SlotCmp { DWORD call; unsigned char tail[13]; DWORD imm; DWORD value; const char* name; };
        static const SlotCmp sites[] = {
            { 0x4F486A, { 0xC1, 0xF8, 0x08, 0x85, 0xC0, 0x7C, 0x07, 0x3D, 0x8C, 0x00, 0x00, 0x00, 0x7C }, 0x4F4877, PED_POOL_SLOTS, "IsPedPointerValid_NotInWorld" },
            { 0x4F48B7, { 0xC1, 0xF8, 0x08, 0x30, 0xD2, 0x85, 0xC0, 0x7C, 0x09, 0x3D, 0x8C, 0x00, 0x00 }, 0x4F48C6, PED_POOL_SLOTS - 1, "IsPedPointerValid" },
            { 0x50D7E9, { 0xC1, 0xF8, 0x08, 0x85, 0xC0, 0x7C, 0x07, 0x3D, 0x8C, 0x00, 0x00, 0x00, 0x7C }, 0x50D7F6, PED_POOL_SLOTS, "BuildPedLists" },
        };
        int fixed = 0;
        for (const SlotCmp& s : sites) {
            const unsigned char* p = (const unsigned char*)s.call;
            if (p[0] != 0xE8 || s.call + 5 + *(const int*)(p + 1) != 0x451CF0 || memcmp(p + 5, s.tail, sizeof s.tail) ||
                *(const DWORD*)s.imm != 0x8C) {
                Log("Population: ped slot check in %s (%X) is not v1.0; left at 140", s.name, s.imm - 1);
                continue;
            }
            Patch(s.imm, &s.value, 4);
            ++fixed;
        }
        Log("Population: %d of 3 ped slot checks now accept all %d slots (game: 140)", fixed, PED_POOL_SLOTS);
    }
    // Each entity is listed in the world sectors it touches, one CEntryInfoNode
    // per sector (pool 0xA0D8BC, 3200, made at 0x4C02A5: push 0xC80). A crowd of 180
    // people filled it, and CWorld::Add (0x4BB545, via 0x489BD0) wrote to the
    // nothing it got back (crash at 0x4BB557). Three times as many.
    static const unsigned char nodes[] = { 0x68, 0x80, 0x0C, 0x00, 0x00, 0xE8 };
    if (!memcmp((void*)0x4C02A5, nodes, sizeof nodes)) {
        const DWORD m = 0xC80 * 3;
        Patch(0x4C02A6, &m, 4);
        Log("Population: world sector list holds %d entries (game: 3200)", (int)m);
    }
}

// Installed even when off: the Modern Fix menu turns each part on and off while playing.
void InstallPopulation() {
    {
        static const unsigned char orig[] = { 0x8B, 0x0D, 0xAC, 0xF2, 0x97, 0x00, 0xE8 };
        if (!memcmp((void*)PED_NEW, orig, sizeof orig) && PED_NEW + 11 + *(const int*)(PED_NEW + 7) == POOL_NEW) {
            unsigned char jmp[5] = { 0xE9 };
            *(int*)(jmp + 1) = (int)((DWORD)&vcmf_ped_new - (PED_NEW + 5));
            Patch(PED_NEW, jmp, sizeof jmp);
            Log("Population: a full ped pool makes room by removing a far street person (crash at 0x4496E7)");
            InstallCreateCharGuard();
            InstallNullExits();
            InstallAddPedGuard();
            InstallAddToPopulationGuard();
            RedirectCall(0x4292D6, 0x426DB0, (void*)&GenerateCarIfRoom, "traffic car");
            RedirectCall(0x4292F0, 0x426DB0, (void*)&GenerateCarIfRoom, "traffic car");
            RedirectCall(0x42BB86, 0x42AE70, (void*)&GenerateEmergencyCarIfRoom, "emergency car");
            if (g_carGuards) Log("Population: no new traffic or emergency car while fewer than 6 ped slots are free (%d calls)", g_carGuards);
        } else {
            Log("Population: ped allocation code is not v1.0; full-pool guard not installed");
        }
        InstallSafeGetAt();
        InstallScriptCharGuard();
        InstallEventPedGuard();
        // CTheScripts::CleanUpThisPed (what 01C2 calls): only a script character (+0x160 = 2) becomes random.
        static const unsigned char release[] = { 0x53, 0x56, 0x57, 0x55, 0x83, 0xEC, 0x08, 0x8B, 0x6C, 0x24, 0x1C, 0x85, 0xED };
        static const unsigned char onlyScript[] = { 0x80, 0xBD, 0x60, 0x01, 0x00, 0x00, 0x02 };
        g_releaseOk = !memcmp((void*)RELEASE_CHAR, release, sizeof release) && !memcmp((void*)(RELEASE_CHAR + 0x13), onlyScript, sizeof onlyScript);
        if (g_releaseOk) Log("Population: dead characters VCTrainer made are cleared like street bodies (released with 0x%X first)", RELEASE_CHAR);
        else Log("Population: script character release at %X is not v1.0; VCTrainer's bodies are left to the game", RELEASE_CHAR);
    }
    {
        const unsigned char* p = (const unsigned char*)READ_DENSITY_CALL;
        const bool ok = p[0] == 0xE8 && READ_DENSITY_CALL + 5 + *(const int*)(p + 1) == READ_DENSITY
                     && *(const DWORD*)0x628139 == (DWORD)MaxPeds && *(const DWORD*)0x628191 == (DWORD)MaxCars;
        if (ok) {
            unsigned char call[5] = { 0xE8 };
            *(int*)(call + 1) = (int)((DWORD)&ReadDensity - (READ_DENSITY_CALL + 5));
            Patch(READ_DENSITY_CALL, call, sizeof call);
            g_densityOk = true;
            if (!g_haveOrig) SaveOrig();   // the start-up read ran already, before any change
            ApplyDensity();   // in case the start-up code already ran
            Log("Population: ped density %.2f (up to %d peds), car density %.2f (up to %d cars)",
                *PedFactor, *MaxPeds, *CarFactor, *MaxCars);
        } else {
            Log("Population: density code is not v1.0; density not changed");
        }
    }
    {
        static const unsigned char fld[] = { 0xD9, 0x05, 0xB4, 0x4E, 0x69, 0x00 };   // fld [0x694EB4]
        if (!memcmp((void*)OFFSCREEN_FLD, fld, sizeof fld)) {
            g_offscreenDist = g_cfg.offscreenKeep > 0 ? g_cfg.offscreenKeep : *(float*)0x694EB4;
            g_offscreenOk = true;
            unsigned char code[6] = { 0xD9, 0x05 };
            *(DWORD*)(code + 2) = (DWORD)&g_offscreenDist;
            Patch(OFFSCREEN_FLD, code, sizeof code);
            Log("Population: peds out of view are kept up to %.0f m away (game: 25 m)", g_offscreenDist);
        } else {
            Log("Population: ped removal code is not v1.0; off-screen distance not changed");
        }
    }
}
