// Interiors: the drawn area follows the player, and the city stays visible from
// inside interiors that are part of it.
//
// Vice City draws one "area" at a time (CGame::currArea 0x978810): 0 is the city,
// 1-18 are interiors (Ocean View, the mall, Malibu...), 13 is shown in every area.
// Buildings of other areas are not drawn or streamed. Only the mission script
// changes the area: each door has a script trigger that sets the interior's area,
// drops the other areas' models (CStreaming::RemoveBuildingsNotInArea 0x40DAA0),
// loads the scene and puts the player just inside (SET_AREA_VISIBLE, LOAD_SCENE).
//
// 1. AutoSwitch. Leaving a room any other way - a trainer teleport, or flying in
//    with a jetpack - left the wrong area set. Once per game update this looks at
//    what the player stands on: a vertical line down from the player
//    (CWorld::ProcessVerticalLine 0x4D8B00, buildings only). Its area:
//    * an interior other than the current one: the player is in that room. Coming
//      from the city, a ceiling above the player is also required
//      (CWorld::GetIsLineOfSightClear 0x4DA560), so a sidewalk never counts.
//    * the city while an interior is set: the player has left the room - once
//      outside every building of that interior (their collision bounding
//      spheres), or, for a room this file switched to, with open sky above.
//    * nothing (in the air) while an interior is set: once the interior's nearest
//      building is 30 m or more away.
//    The new area must hold for half a second with the player in control. Then it
//    is applied like the script does it (area, RemoveBuildingsNotInArea, LoadScene
//    with CTimer::Stop / Update around it). Going back to the city also undoes what
//    the door scripts set inside (interior lighting, low traffic) unless on a mission.
//
// 2. ShowOutside. Almost every interior is built inside its real building, so the
//    city around it can be drawn too: through the door you see the street instead
//    of black. Every "is this entity in the current area" test the game makes for
//    drawing, streaming and shadows (cmp eax,[currArea]; je; cmp reg8,13 - twelve
//    places) calls AreaCmp instead, which also accepts the city (area 0) while
//    g_outside is set. RemoveBuildingsNotInArea then keeps the city's models.
//    Two kinds of buildings would overlap, flicker or block the view, so they are
//    moved to a hidden area (0xFE) while you're inside and put back when the area
//    changes: the interior's own stand-in for the outside (dark facades, street and
//    black panels it shows through doors and windows: htl_exterior03,
//    club_exterior01-12, cop_fake_ext01-03, cop_blackpoly...), and city buildings
//    inside the room - its "fake" interior seen through windows from outside, its
//    exterior shell, the ground under it: centre inside the collision box of one of
//    the room's buildings, or 60% of its own box inside one. The log lists what was
//    hidden and the city buildings left touching the room. Interiors away from the city (the stadium, high up
//    in the sky) keep the original black: the city must be within 25 m of the
//    player and the room within 80 m when the area is set.
#include "interior.h"
#include "config.h"
#include <windows.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

static int* const CurrArea = (int*)0x978810;
static const DWORD GAME_STATE = 0x9B5F08, MENU_ACTIVE = 0x869668, CLOCK = 0x974B2C;
static const DWORD PAD0_DISABLE = 0x7DBCB0 + 0xF0;
static const DWORD BUILDING_POOL = 0x97F240, SECOND_POOL = 0x978834, MODEL_INFO = 0x92D4C8;
static const DWORD ENT_POS = 0x34, ENT_MODEL = 0x5C, ENT_AREA = 0x5F, ENTITY_SIZE = 0x64;
static const DWORD PED_STATE = 0x244, MI_COLMODEL = 0x1C;
static const unsigned char AREA_EVERYWHERE = 13, AREA_HIDDEN = 0xFE;
static float* const CarDensity = (float*)0x686FC8;   // CCarCtrl::CarDensityMultiplier

typedef unsigned char* (__cdecl* Find_t)();
static const Find_t FindPlayerPed = (Find_t)0x4BC120, FindPlayerVehicle = (Find_t)0x4BC1E0;
static const auto ProcessVerticalLine = (bool (__cdecl*)(const float* point, float zStop, void* colPoint,
    unsigned char** entity, bool b, bool v, bool p, bool o, bool d, bool ignoreSeeThrough, void* poly))0x4D8B00;
static const auto LineOfSightClear = (bool (__cdecl*)(const float* p0, const float* p1, bool b, bool v,
    bool p, bool o, bool d, bool ignoreSeeThrough, bool ignoreSomeObjects))0x4DA560;
static const DWORD REMOVE_NOT_IN_AREA = 0x40DAA0;
static const auto LoadScene = (void (__cdecl*)(const float*))0x40AF60;
static const auto TimerStop = (void (__cdecl*)())0x4D0DB0;
static const auto TimerUpdate = (void (__cdecl*)())0x4D0F30;
static const auto StopExtraColour = (void (__cdecl*)(bool))0x4CE9E0;
static const auto OnMission = (bool (__cdecl*)())0x44FE30;

static bool g_fromIdle = false;
static int g_pending = -1;          // area the player seems to be in, waiting to hold
static DWORD g_pendingSince = 0;
static int g_forced = -1;           // interior this file switched to (no door script ran)
static unsigned g_switches = 0, g_viewLogs = 0;

// ---------------------------------------------------------------- the area test
// Called instead of `cmp eax/ebx, [currArea]`: ZF set when the entity's area (the
// register) is the current one or vcmf_also - the city while it is shown from
// inside an interior, or the interior next to the player while it is shown from
// the street (-1: none). Only the flags change.
extern "C" {
int vcmf_also = -1;
void vcmf_area_cmp_eax();
void vcmf_area_cmp_ebx();
}
__asm__(
    ".text\n"
    ".globl _vcmf_area_cmp_eax\n"
    "_vcmf_area_cmp_eax:\n"
    "  cmpl 0x978810, %eax\n"
    "  je 1f\n"
    "  cmpl _vcmf_also, %eax\n"
    "1: ret\n"
    ".globl _vcmf_area_cmp_ebx\n"
    "_vcmf_area_cmp_ebx:\n"
    "  cmpl 0x978810, %ebx\n"
    "  je 2f\n"
    "  cmpl _vcmf_also, %ebx\n"
    "2: ret\n");
static bool vcmf_outside = false;   // inside an interior, the city around it is shown

// `cmp eax,[currArea]` (3B 05 10 88 97 00) followed by `je` in the entity tests of
// streaming (6), the renderer (3) and shadows (3); `cmp ebx,[currArea]` in the
// dummy -> object conversion around the player (so lamp posts outside appear).
static const DWORD kEaxSites[] = { 0x40E680, 0x40E6BD, 0x40E6FD, 0x40E73D, 0x40E7F6, 0x40E8C6,
                                   0x4C7940, 0x4C7D55, 0x4C81DB, 0x56B992, 0x56C033, 0x56C1F0 };
static const DWORD kEbxSite = 0x53DA8A;
// Calls to "is water drawn in this area" (areas 0, 1, 2 only), in CWaterLevel.
static const DWORD kWaterSites[] = { 0x5BFF22, 0x5C172E };
static const DWORD WATER_AREA_FN = 0x4A4360;
// Calls to RemoveBuildingsNotInArea: SET_AREA_VISIBLE and the respawn after wasted/busted.
static const DWORD kRemoveSites[] = { 0x630B53, 0x42BB16 };

static DWORD g_waterFn = WATER_AREA_FN, g_removeFn = REMOVE_NOT_IN_AREA;
static bool g_viewOk = false;       // ShowOutside installed
static int g_viewArea = 0;          // area the outside view was decided for
static bool g_viewDecided = true;

// ---------------------------------------------------------------- entities
template <class F> static void ForEachBuilding(F f) {
    const DWORD pools[2] = { BUILDING_POOL, SECOND_POOL };
    for (DWORD pool : pools) {
        unsigned char* pl = *(unsigned char**)pool;
        if (!pl) continue;
        unsigned char* objs = *(unsigned char**)pl;
        const unsigned char* flags = *(unsigned char**)(pl + 4);
        const int size = *(int*)(pl + 8);
        if (!objs || !flags) continue;
        for (int i = 0; i < size; i++)
            if (!(flags[i] & 0x80)) f(objs + i * ENTITY_SIZE);
    }
}

static const float* ColModel(unsigned char* e) {
    const short model = *(short*)(e + ENT_MODEL);
    if (model < 0) return nullptr;
    unsigned char* mi = ((unsigned char**)MODEL_INFO)[model];
    return mi ? *(const float**)(mi + MI_COLMODEL) : nullptr;
}

// World centre of the entity's collision bounding sphere (its position without one).
static void Centre(unsigned char* e, const float* col, float* c) {
    const float* m = (const float*)(e + 4);   // right, forward, up, pos (16 bytes each)
    for (int k = 0; k < 3; k++)
        c[k] = col ? m[12 + k] + m[k] * col[0] + m[4 + k] * col[1] + m[8 + k] * col[2] : m[12 + k];
}

static float Dist(const float* a, const float* b) {
    const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return sqrtf(dx * dx + dy * dy + dz * dz);
}

static int RealArea(unsigned char* e);

// Distance from p to the nearest building of `area` (to its collision bounding
// sphere; negative inside it). Hidden buildings count for their own area. 1e9 if none.
static void WorldBox(const float* m, const float* mn, const float* mx, float* lo, float* hi);
static bool IslandCopy(unsigned char* e, const float* col);

// Distance from p to the nearest building of `area`: to its collision box (0
// inside it). Not to its bounding sphere: the mall's MALLUNDER, a black box 266 m
// long around the whole mall, has a sphere reaching far into the streets around
// it, so the player would never count as out of the mall there. Pieces hidden as
// stand-ins for the outside (island copies, that black box) don't count.
static float NearestOfArea(int area, const float* p) {
    float best = 1e9f;
    ForEachBuilding([&](unsigned char* e) {
        if (e[ENT_AREA] != area && !(e[ENT_AREA] == AREA_HIDDEN && RealArea(e) == area)) return;
        const float* col = ColModel(e);
        if (!col || (area != 0 && IslandCopy(e, col))) return;
        float c[3];
        Centre(e, col, c);
        if (Dist(p, c) - col[3] >= best) return;   // the sphere is already further
        float lo[3], hi[3], d2 = 0;
        WorldBox((const float*)(e + 4), col + 4, col + 7, lo, hi);
        for (int k = 0; k < 3; k++) {
            const float o = p[k] < lo[k] ? lo[k] - p[k] : p[k] > hi[k] ? p[k] - hi[k] : 0;
            d2 += o * o;
        }
        const float d = sqrtf(d2);
        if (d < best) best = d;
    });
    return best;
}

// ---------------------------------------------------------------- outside view
static unsigned char* g_hidden[8192];
static unsigned char g_hiddenArea[8192];   // the area each one had
static int g_nHidden = 0;

struct Piece { float m[16]; float mn[3], mx[3], c[3], r; float lo[3], hi[3]; };
static Piece g_pieces[1024];

static void Unhide() {
    for (int i = 0; i < g_nHidden; i++)
        if (g_hidden[i][ENT_AREA] == AREA_HIDDEN) g_hidden[i][ENT_AREA] = g_hiddenArea[i];
    g_nHidden = 0;
}

static int RealArea(unsigned char* e) {
    if (e[ENT_AREA] != AREA_HIDDEN) return e[ENT_AREA];
    for (int i = 0; i < g_nHidden; i++)
        if (g_hidden[i] == e) return g_hiddenArea[i];
    return 0;
}

static void HideOne(unsigned char* e) {
    if (g_nHidden >= 8192) return;
    g_hiddenArea[g_nHidden] = e[ENT_AREA];
    g_hidden[g_nHidden++] = e;
    e[ENT_AREA] = AREA_HIDDEN;
}

static const char* ModelName(unsigned char* e) {
    const short model = *(short*)(e + ENT_MODEL);
    unsigned char* mi = model >= 0 ? ((unsigned char**)MODEL_INFO)[model] : nullptr;
    return mi ? (const char*)(mi + 4) : "?";   // CBaseModelInfo: vtable, name[24], colModel
}

// An interior's own stand-in for the outside: the facade, street or black panels
// it shows through its doors and windows instead of the city (htl_exterior03,
// club_exterior01-12, cop_fake_ext01-03, cop_blackpoly, biker_bar_exterior...).
static bool IsStandIn(const char* name) {
    char n[25];
    int i = 0;
    for (; i < 24 && name[i]; i++) n[i] = (char)(name[i] >= 'A' && name[i] <= 'Z' ? name[i] + 32 : name[i]);
    n[i] = 0;
    return strstr(n, "exterior") || strstr(n, "fake_ext") || strstr(n, "blackpoly") ||
           (i >= 4 && !strcmp(n + i - 4, "_ext"));
}

// World-space box around an entity's collision box.
static void WorldBox(const float* m, const float* mn, const float* mx, float* lo, float* hi) {
    for (int k = 0; k < 3; k++) {
        float c = m[12 + k], h = 0;
        for (int j = 0; j < 3; j++) {
            const float a = m[4 * j + k];   // row j (right, forward, up), component k
            c += a * 0.5f * (mn[j] + mx[j]);
            h += fabsf(a) * 0.5f * (mx[j] - mn[j]);
        }
        lo[k] = c - h;
        hi[k] = c + h;
    }
}

// The buildings of interior `area` near p go to g_pieces (their collision boxes);
// its stand-ins for the city are hidden instead: pieces named like an exterior,
// pieces bigger than 150 m (the mansion's IslandLOD1/2, low-detail copies of a
// whole island for the view from its windows; the mall's MALLUNDER, a black box
// around the whole mall that hides the outside) except a roof (the mall's own
// roof mlmallroof, 248 m, seen from inside), and pieces of the same area more
// than 300 m away (another place). Returns how many pieces.
static bool IslandCopy(unsigned char* e, const float* col) {
    const float* mn = col + 4, * mx = col + 7;   // CColModel: sphere (centre, radius), box (min, max)
    if (mx[0] - mn[0] <= 150 && mx[1] - mn[1] <= 150 && mx[2] - mn[2] <= 150) return false;
    char n[25];
    const char* name = ModelName(e);
    int i = 0;
    for (; i < 24 && name[i]; i++) n[i] = (char)(name[i] >= 'A' && name[i] <= 'Z' ? name[i] + 32 : name[i]);
    n[i] = 0;
    return !strstr(n, "roof");
}

// A city building too big to count as one building around a room (roads, land).
static bool Huge(const float* col) {
    const float* mn = col + 4, * mx = col + 7;
    return mx[0] - mn[0] > 150 || mx[1] - mn[1] > 150 || mx[2] - mn[2] > 150;
}

// Is this interior building inside a city building (the lobby inside the hotel's
// shell)? Its collision centre inside the collision box of a city building at
// least 4 m tall. Interiors also hold copies of the city for their cutscenes and
// window views (the lawyer's lawcutmapa-c streets): those are not enclosed, and
// from the street they would sit on top of the real city and flicker. Buildings
// never move, so the answer is kept (by entity, open addressing).
static unsigned char* g_encKey[4096];
static unsigned char g_encVal[4096];

static bool Enclosed(unsigned char* e) {
    unsigned h = ((unsigned)(DWORD)e / ENTITY_SIZE) & 4095;
    while (g_encKey[h] && g_encKey[h] != e) h = (h + 1) & 4095;
    if (g_encKey[h] == e) return g_encVal[h];
    float c[3];
    Centre(e, ColModel(e), c);
    bool in = false;
    ForEachBuilding([&](unsigned char* b) {
        if (in || RealArea(b) != 0) return;
        const float* col = ColModel(b);
        if (!col || Huge(col)) return;
        const float* mn = col + 4, * mx = col + 7;
        if (mx[2] - mn[2] < 4.0f) return;   // ground, roads
        const float* m = (const float*)(b + 4);
        const float d[3] = { c[0] - m[12], c[1] - m[13], c[2] - m[14] };
        if (d[0] * d[0] + d[1] * d[1] + d[2] * d[2] > (col[3] + 1) * (col[3] + 1)) return;
        for (int k = 0; k < 3; k++) {
            const float l = d[0] * m[4 * k] + d[1] * m[4 * k + 1] + d[2] * m[4 * k + 2];
            if (l < mn[k] - 0.5f || l > mx[k] + 0.5f) return;
        }
        in = true;
    });
    int used = 0;
    for (int i = 0; i < 4096; i++) used += g_encKey[i] != nullptr;
    if (used < 3500) { g_encKey[h] = e; g_encVal[h] = in; }
    return in;
}

static int CollectPieces(int area, const float* p, int& standIns, char* standInNames, bool street = false) {
    int n = 0;
    standIns = 0;
    standInNames[0] = 0;
    ForEachBuilding([&](unsigned char* e) {
        if (e[ENT_AREA] != area || n >= 1024) return;
        const float* col = ColModel(e);
        float c[3];
        Centre(e, col, c);
        if (Dist(c, p) > 300) { HideOne(e); return; }   // another place with the same area
        if (!col) { if (street) HideOne(e); return; }
        if (IsStandIn(ModelName(e)) || IslandCopy(e, col) || (street && !Enclosed(e))) {
            HideOne(e);
            if (standIns++ < 8 && strlen(standInNames) < 160) {
                if (standInNames[0]) strcat(standInNames, " ");
                strncat(standInNames, ModelName(e), 24);
            }
            return;
        }
        const float* mn = col + 4, * mx = col + 7;
        Piece& pc = g_pieces[n];
        memcpy(pc.c, c, sizeof c);
        memcpy(pc.m, e + 4, sizeof pc.m);
        for (int k = 0; k < 3; k++) { pc.mn[k] = mn[k] - 0.5f; pc.mx[k] = mx[k] + 0.5f; }
        pc.r = col[3] + 0.5f;
        WorldBox(pc.m, pc.mn, pc.mx, pc.lo, pc.hi);
        n++;
    });
    return n;
}

static void PunchRoom(int pieces, const float* p);   // below, with the street view
static void Punch(unsigned char* e);
static unsigned char* g_shells[32];   // city buildings Hide kept as the outside around the room
static int g_nShells = 0;

// What the city looks like from inside interior `area` at p: the interior's
// stand-in exterior pieces are hidden, and so is every city building that is
// inside one of the room's buildings - its centre inside that building's
// collision box, or 60% or more of its own box inside that building's box.
static void Hide(int area, const float* p) {
    int standIns, byCentre = 0, byOverlap = 0, shells = 0;
    g_nShells = 0;
    char standInNames[200];
    const int n = CollectPieces(area, p, standIns, standInNames);
    int kept = 0;
    ForEachBuilding([&](unsigned char* e) {
        if (e[ENT_AREA] != 0) return;
        const float* col = ColModel(e);
        float c[3], lo[3], hi[3];
        Centre(e, col, c);
        if (col) WorldBox((const float*)(e + 4), col + 4, col + 7, lo, hi);
        float best = 0;
        for (int i = 0; i < n; i++) {
            const Piece& pc = g_pieces[i];
            float f = 1;   // share of the city building's box inside this piece's box
            if (col) {
                float inter = 1, own = 1;
                for (int k = 0; k < 3; k++) {
                    const float a = (hi[k] < pc.hi[k] ? hi[k] : pc.hi[k]) - (lo[k] > pc.lo[k] ? lo[k] : pc.lo[k]);
                    inter *= a > 0 ? a : 0;
                    own *= hi[k] - lo[k] > 0.1f ? hi[k] - lo[k] : 0.1f;
                }
                f = inter / own;
                if (f > best) best = f;
            }
            // Centre inside the piece, and a real part of it there too: a long street
            // whose middle happens to be under the room (odNroad01 under the Malibu's
            // dance floor) is not part of the room and stays.
            if (f >= 0.25f && Dist(c, pc.c) <= pc.r) {
                const float d[3] = { c[0] - pc.m[12], c[1] - pc.m[13], c[2] - pc.m[14] };
                bool in = true;
                for (int k = 0; k < 3 && in; k++) {
                    const float l = d[0] * pc.m[4 * k] + d[1] * pc.m[4 * k + 1] + d[2] * pc.m[4 * k + 2];
                    in = l >= pc.mn[k] && l <= pc.mx[k];
                }
                if (in) {
                    // Bigger than the room piece: the building's outside around the
                    // room (the Malibu's od_clubout with its roof and ladders, its
                    // fences and neon). It stays; its black door panels are opened.
                    float ownVol = 1, pieceVol = 1;
                    for (int k = 0; k < 3; k++) {
                        ownVol *= col ? col[7 + k] - col[4 + k] : 0;
                        pieceVol *= pc.mx[k] - pc.mn[k];
                    }
                    if (ownVol > pieceVol) {
                        if (g_nShells < 32) g_shells[g_nShells++] = e;
                        shells++;
                        return;
                    }
                    HideOne(e); byCentre++; return;
                }
            }
        }
        if (best >= 0.6f) { HideOne(e); byOverlap++; return; }
        if (best >= 0.05f && kept < 25 && g_viewLogs < 50)   // what is left touching the room, to tune this
            Log("Interiors:   kept city %s (%.0f%% inside the room's boxes) at %.0f %.0f %.0f", ModelName(e), best * 100, c[0], c[1], c[2]), kept++;
    });
    if (g_viewLogs < 50)
        Log("Interiors:   hid %d stand-in exterior pieces (%s), %d city buildings by centre, %d by overlap; "
            "kept %d buildings around the room", standIns, standInNames[0] ? standInNames : "none", byCentre, byOverlap, shells);
    PunchRoom(n, p);   // black parts inside these pieces are opened
}

static bool InCity(int area, const float* p) {
    return NearestOfArea(area, p) < 80.0f && NearestOfArea(0, p) < 25.0f;
}

static const float* PlayerPos() {
    if (*(int*)GAME_STATE != 9) return nullptr;
    unsigned char* ped = FindPlayerPed();
    if (!ped) return nullptr;
    unsigned char* veh = FindPlayerVehicle();
    return (const float*)((veh ? veh : ped) + ENT_POS);
}

static bool SameName(const char* a, const char* b) {
    for (int i = 0; i < 24; i++) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return false;
        if (!x) return true;
    }
    return true;
}

// The city building the player is standing inside: its exterior shell, which has
// dark boxes in its doorways and windows and glass that would cover the room's own
// (od_clevelander around the Ocean View lobby). Any tall city building whose
// collision box contains p is hidden, with its low-detail model ("LOD" + the name
// without its first 3 letters), drawn in its place when it isn't. Except the
// buildings Hide kept as the outside around the room (the Malibu's od_clubout,
// with its roof and ladders seen through the door): only their black parts inside
// the room are opened.

static void HideContaining(const float* p) {
    char lods[8][25];
    int nLods = 0;
    ForEachBuilding([&](unsigned char* e) {
        if (e[ENT_AREA] != 0) return;
        const float* col = ColModel(e);
        if (!col) return;
        const float* mn = col + 4, * mx = col + 7;
        if (mx[0] - mn[0] > 150 || mx[1] - mn[1] > 150 || mx[2] - mn[2] > 150) return;
        const float* m = (const float*)(e + 4);
        const float d[3] = { p[0] - m[12], p[1] - m[13], p[2] - m[14] };
        float l[3];
        for (int k = 0; k < 3; k++) {
            l[k] = d[0] * m[4 * k] + d[1] * m[4 * k + 1] + d[2] * m[4 * k + 2];
            if (l[k] < mn[k] - 0.3f || l[k] > mx[k] + 0.3f) return;
        }
        if (mx[2] - l[2] < 3.0f) return;   // flat (ground, a floor): not around the player
        for (int i = 0; i < g_nShells; i++)
            if (g_shells[i] == e) { Punch(e); return; }
        const char* name = ModelName(e);
        if (g_viewLogs < 50) Log("Interiors:   hid city building around the player: %s", name);
        HideOne(e);
        if (nLods < 8 && strlen(name) > 3) {
            strcpy(lods[nLods], "lod");
            strncat(lods[nLods], name + 3, 21);
            nLods++;
        }
    });
    if (!nLods) return;
    ForEachBuilding([&](unsigned char* e) {
        if (e[ENT_AREA] != 0) return;
        const char* name = ModelName(e);
        for (int i = 0; i < nLods; i++)
            if (SameName(name, lods[i])) { HideOne(e); return; }
    });
}

// ---------------------------------------------------------------- looking in from the street
// Standing in the city next to an interior built into it, that interior is drawn
// too (vcmf_also), with its stand-in exterior pieces hidden. The city building's
// doorway is usually closed by a black panel (od_clevelander: two triangles with
// texture wee_blak_32 behind the Ocean View doors). Each part of a nearby city
// building whose texture is black (name with "blak"/"black") and whose centre is
// inside one of the interior's buildings is made invisible: its triangle strip's
// indices are all set to its first vertex (zero-area triangles), and the mesh
// header's serial number is bumped so RenderWare rebuilds the model's vertex and
// index buffers. Put back the same way when the player walks away.
//   RpAtomic: +0x18 geometry. RpGeometry (object type 8): +0x14 numVertices,
//   +0x54 RpMeshHeader, +0x5C morph target (+0x14 vertices). RpMeshHeader: +4
//   numMeshes, +6 serialNum, +0xC firstMeshOffset, meshes after the 16-byte header:
//   { indices, numIndices, material }. RpMaterial: +0 texture (+0x10 name).
// The geometry is the model's, shared by every copy of the building, and stays in
// memory while the model is loaded even when the building's own drawn copy is
// deleted (walking away, area changes). So a part is put back only when it can be
// checked: the building has its drawn copy again, with the same geometry, and the
// same mesh still holds exactly what was written. Until then it stays on the list.
struct Punched { unsigned char* ent; unsigned char* geo; unsigned char* header; unsigned short* idx; int count; int saved; int mesh; };
static Punched g_punched[64];
static int g_nPunched = 0;
static unsigned short g_savedIdx[32768];
static int g_savedUsed = 0;
static int g_lookArea = -1, g_lookPieces = 0;
static unsigned g_lookLogs = 0;

static unsigned char* AtomicGeometry(unsigned char* e) {
    unsigned char* obj = *(unsigned char**)(e + 0x4C);   // CEntity::m_rwObject
    if (!obj || obj[0] != 1) return nullptr;             // rpATOMIC
    unsigned char* geo = *(unsigned char**)(obj + 0x18);
    return geo && geo[0] == 8 ? geo : nullptr;           // rpGEOMETRY
}

static bool BlackTexture(unsigned char* material) {
    unsigned char* tex = *(unsigned char**)material;
    if (!tex) return false;
    char n[33];
    int i = 0;
    for (const char* s = (const char*)(tex + 0x10); i < 32 && s[i]; i++)
        n[i] = (char)(s[i] >= 'A' && s[i] <= 'Z' ? s[i] + 32 : s[i]);
    n[i] = 0;
    return strstr(n, "blak") || strstr(n, "black");
}

static void Punch(unsigned char* e) {
    unsigned char* geo = AtomicGeometry(e);
    if (!geo) return;
    unsigned char* mt = *(unsigned char**)(geo + 0x5C);
    unsigned char* header = *(unsigned char**)(geo + 0x54);
    if (!mt || *(unsigned char**)mt != geo || !header) return;
    const float* verts = *(const float**)(mt + 0x14);
    const int nVerts = *(int*)(geo + 0x14);
    const int nMeshes = *(unsigned short*)(header + 4);
    if (!verts || nVerts <= 0 || nMeshes <= 0 || nMeshes > 64) return;
    unsigned char* meshes = header + 16 + *(unsigned*)(header + 12);
    const float* m = (const float*)(e + 4);
    bool changed = false;
    for (int i = 0; i < nMeshes; i++) {
        unsigned char* mesh = meshes + 12 * i;
        unsigned short* idx = *(unsigned short**)mesh;
        const int count = *(int*)(mesh + 4);
        unsigned char* material = *(unsigned char**)(mesh + 8);
        if (!idx || count < 3 || count > 4096 || !material || !BlackTexture(material)) continue;
        bool done = false;
        for (int j = 0; j < g_nPunched && !done; j++) done = g_punched[j].idx == idx;
        if (done) continue;
        bool degenerate = true;   // already all one vertex: nothing to open (or to save)
        for (int k = 1; k < count && degenerate; k++) degenerate = idx[k] == idx[0];
        if (degenerate) continue;
        float lo[3] = { 1e9f, 1e9f, 1e9f }, hi[3] = { -1e9f, -1e9f, -1e9f };
        bool ok = true;
        for (int k = 0; k < count && ok; k++) {
            if (idx[k] >= nVerts) { ok = false; break; }
            const float* v = verts + 3 * idx[k];
            for (int a = 0; a < 3; a++) { if (v[a] < lo[a]) lo[a] = v[a]; if (v[a] > hi[a]) hi[a] = v[a]; }
        }
        if (!ok) continue;
        float c[3];
        for (int a = 0; a < 3; a++) {
            const float l[3] = { 0.5f * (lo[0] + hi[0]), 0.5f * (lo[1] + hi[1]), 0.5f * (lo[2] + hi[2]) };
            c[a] = m[12 + a] + m[a] * l[0] + m[4 + a] * l[1] + m[8 + a] * l[2];
        }
        bool inRoom = false;
        for (int j = 0; j < g_lookPieces && !inRoom; j++) {
            const Piece& pc = g_pieces[j];
            inRoom = c[0] >= pc.lo[0] - 1.5f && c[0] <= pc.hi[0] + 1.5f && c[1] >= pc.lo[1] - 1.5f &&
                     c[1] <= pc.hi[1] + 1.5f && c[2] >= pc.lo[2] - 1.5f && c[2] <= pc.hi[2] + 1.5f;
        }
        if (!inRoom || g_nPunched >= 64 || g_savedUsed + count > 32768) continue;
        Punched& pu = g_punched[g_nPunched++];
        pu = { e, geo, header, idx, count, g_savedUsed, i };
        memcpy(g_savedIdx + g_savedUsed, idx, count * sizeof *idx);
        g_savedUsed += count;
        for (int k = 1; k < count; k++) idx[k] = idx[0];
        changed = true;
        if (g_lookLogs < 50)
            Log("Interiors:   opened the doorway of %s (black part at %.1f %.1f %.1f)", ModelName(e), c[0], c[1], c[2]), g_lookLogs++;
    }
    if (changed) (*(unsigned short*)(header + 6))++;   // RenderWare rebuilds the model's buffers
}

// The mesh is still exactly as Punch left it.
static bool StillPunched(const Punched& pu, unsigned char* geo) {
    if (geo != pu.geo || *(unsigned char**)(geo + 0x54) != pu.header) return false;
    if (pu.mesh >= *(unsigned short*)(pu.header + 4)) return false;
    unsigned char* mesh = pu.header + 16 + *(unsigned*)(pu.header + 12) + 12 * pu.mesh;
    if (*(unsigned short**)mesh != pu.idx || *(int*)(mesh + 4) != pu.count) return false;
    for (int k = 1; k < pu.count; k++)
        if (pu.idx[k] != pu.idx[0]) return false;
    return true;
}

static void Unpunch() {
    int keep = 0;
    for (int i = 0; i < g_nPunched; i++) {
        const Punched& pu = g_punched[i];
        unsigned char* geo = AtomicGeometry(pu.ent);
        if (!geo) { g_punched[keep++] = pu; continue; }   // not drawn right now: check again later
        if (StillPunched(pu, geo)) {
            memcpy(pu.idx, g_savedIdx + pu.saved, pu.count * sizeof *pu.idx);
            (*(unsigned short*)(pu.header + 6))++;
        }
        // otherwise the model was loaded again from the file: nothing to put back
    }
    g_nPunched = keep;
    if (!keep) g_savedUsed = 0;
}

static void PunchNear(const float* p) {
    ForEachBuilding([&](unsigned char* e) {
        if (e[ENT_AREA] != 0) return;
        const float* m = (const float*)(e + 4);
        const float pos[3] = { m[12], m[13], m[14] };
        if (Dist(pos, p) < 60.0f) Punch(e);
    });
}

// Inside an interior: its pieces (from Hide) decide which black parts are opened.
static void PunchRoom(int pieces, const float* p) {
    g_lookPieces = pieces;
    PunchNear(p);
}

// People inside, seen from the street. The door scripts switch an interior's ped
// paths on when you walk in and off when you walk out (SWITCH_PED_ROADS_ON/OFF,
// 022A/022B: the Malibu, the mall, the mansion...), so from the street the room
// is empty. The boxes are read from main.scm (SET_AREA_VISIBLE n followed by
// 022A), and while area n is shown from the street its ped paths are switched on:
// the game's own population then walks people in. They are switched off again
// when the view ends, unless the player is now in that area (its script has them).
// CPathFind::SwitchPedRoadsOffInArea(x1, x2, y1, y2, z1, z2, off), ThePaths.
static const DWORD THE_PATHS = 0x9B6E5C, SWITCH_PED_ROADS = 0x435980;
typedef void (__fastcall* SwitchPedRoads_t)(void*, int, float, float, float, float, float, float, int);
static struct { int area; float lo[3], hi[3]; } g_pedRoads[16];
static int g_nPedRoads = 0, g_pedRoadsOn = -1;

static void LoadPedRoads() {
    static const unsigned char code[] = { 0x53, 0x56, 0x89, 0xCB, 0x57, 0x55 };
    if (memcmp((void*)SWITCH_PED_ROADS, code, sizeof code)) { Log("Interiors: ped paths code is not v1.0; rooms seen from the street stay empty"); return; }
    char path[MAX_PATH];
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    if (char* slash = strrchr(path, '\\')) slash[1] = 0;
    lstrcatA(path, "data\\main.scm");
    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    const DWORD size = GetFileSize(f, nullptr);
    unsigned char* d = size != INVALID_FILE_SIZE && size < (64u << 20) ? (unsigned char*)malloc(size) : nullptr;
    DWORD got = 0;
    if (d && !ReadFile(f, d, size, &got, nullptr)) got = 0;
    CloseHandle(f);
    for (DWORD a = 0; a + 128 < got && g_nPedRoads < 16; a++) {
        if (d[a] != 0xBB || d[a + 1] != 0x04 || d[a + 2] != 0x04) continue;   // 04BB, 8-bit area
        const int area = d[a + 3];
        if (area == 0 || area == AREA_EVERYWHERE) continue;
        for (DWORD i = a + 4; i < a + 96; i++) {
            if (d[i] != 0x2A || d[i + 1] != 0x02) continue;   // 022A, six floats
            float v[6];
            bool ok = true;
            for (int k = 0; k < 6 && ok; k++) {
                ok = d[i + 2 + 5 * k] == 6;
                memcpy(&v[k], d + i + 3 + 5 * k, 4);
            }
            if (!ok) continue;
            bool dup = false;
            for (int j = 0; j < g_nPedRoads; j++)
                dup = dup || (g_pedRoads[j].area == area && g_pedRoads[j].lo[0] == fminf(v[0], v[3]) &&
                              g_pedRoads[j].lo[1] == fminf(v[1], v[4]));
            if (dup) break;
            auto& r = g_pedRoads[g_nPedRoads++];
            r.area = area;
            for (int k = 0; k < 3; k++) { r.lo[k] = fminf(v[k], v[k + 3]); r.hi[k] = fmaxf(v[k], v[k + 3]); }
            break;
        }
    }
    free(d);
    Log("Interiors: %d interiors have people walked in when seen from the street", g_nPedRoads);
}

static void SwitchPedRoads(int area, bool off) {
    for (int j = 0; j < g_nPedRoads; j++) {
        if (g_pedRoads[j].area != area) continue;
        const auto& r = g_pedRoads[j];
        ((SwitchPedRoads_t)SWITCH_PED_ROADS)((void*)THE_PATHS, 0, r.lo[0], r.hi[0], r.lo[1], r.hi[1], r.lo[2], r.hi[2], off);
    }
}

static void PedRoadsOn(int area) {
    if (g_pedRoadsOn == area || *CurrArea != 0) return;
    bool any = false;
    for (int j = 0; j < g_nPedRoads; j++) any = any || g_pedRoads[j].area == area;
    if (!any) return;
    SwitchPedRoads(area, false);
    g_pedRoadsOn = area;
}

static void PedRoadsOff() {
    if (g_pedRoadsOn < 0) return;
    if (*CurrArea != g_pedRoadsOn) SwitchPedRoads(g_pedRoadsOn, true);
    g_pedRoadsOn = -1;
}

static void LookEnd() {
    PedRoadsOff();
    if (g_lookArea < 0) return;
    Unpunch();
    Unhide();
    vcmf_also = -1;
    g_lookArea = -1;
}

// The interior with a building inside a city building within 20 m of p (its
// stand-in exterior pieces don't count), or -1.
static int InteriorNear(const float* p) {
    int best = -1;
    float bestD = 20.0f;
    ForEachBuilding([&](unsigned char* e) {
        const int a = RealArea(e);
        if (a == 0 || a == AREA_EVERYWHERE || a > 18) return;
        const float* col = ColModel(e);
        if (!col || IslandCopy(e, col) || IsStandIn(ModelName(e))) return;
        // Distance from p to the piece's collision box (0 inside it).
        float lo[3], hi[3], d2 = 0;
        WorldBox((const float*)(e + 4), col + 4, col + 7, lo, hi);
        for (int k = 0; k < 3; k++) {
            const float o = p[k] < lo[k] ? lo[k] - p[k] : p[k] > hi[k] ? p[k] - hi[k] : 0;
            d2 += o * o;
        }
        const float d = sqrtf(d2);
        if (d < bestD && Enclosed(e)) { bestD = d; best = a; }
    });
    return best;
}

static void UpdateLook(const float* p) {
    static DWORD last = 0;
    const DWORD now = *(DWORD*)CLOCK;
    if (now - last < 250) return;
    last = now;
    const int nearArea = p ? InteriorNear(p) : -1;
    if (nearArea != g_lookArea) {
        LookEnd();
        if (nearArea < 0) return;
        int standIns;
        char names[200];
        g_lookArea = nearArea;
        vcmf_also = nearArea;
        g_lookPieces = CollectPieces(nearArea, p, standIns, names, true);
        PedRoadsOn(nearArea);
        if (g_lookLogs < 50)
            Log("Interiors: showing area %d from the street (player at %.0f %.0f %.0f, %d stand-in pieces hidden: %s)",
                nearArea, p[0], p[1], p[2], standIns, names[0] ? names : "none"), g_lookLogs++;
    }
    PunchNear(p);   // again each time: models stream in as the player walks up
}

// The area was just set (by a door script, a respawn, AutoSwitch or a save being
// loaded): decide whether the city is shown with it. True when it is.
static bool DecideView(int area, const float* p) {
    if (area == g_viewArea && vcmf_outside) return true;   // set again (scripts do): keep it
    LookEnd();
    Unpunch();          // doorways opened from inside the last interior
    g_lookPieces = 0;
    g_nShells = 0;
    Unhide();
    vcmf_outside = false;
    vcmf_also = -1;
    g_viewArea = area;
    g_viewDecided = p != nullptr;
    if (!g_viewOk || !p || area == 0 || area == AREA_EVERYWHERE || !InCity(area, p)) return false;
    if (g_viewLogs < 50) Log("Interiors: area %d shows the city around it (player at %.0f %.0f %.0f)", area, p[0], p[1], p[2]);
    Hide(area, p);
    HideContaining(p);
    g_viewLogs++;
    vcmf_outside = true;
    vcmf_also = 0;
    return true;
}

static void __cdecl HookRemove(int area) {
    // Walking out of an interior shown with the city: it stays loaded too, since it
    // is still drawn through the door.
    const bool keep = area == 0 && vcmf_outside;
    if (g_viewOk && DecideView(area, PlayerPos())) return;   // the city stays loaded
    if (keep) return;
    ((void (__cdecl*)(int))g_removeFn)(area);
}

static bool __cdecl HookWater() {
    return vcmf_outside || ((bool (__cdecl*)())g_waterFn)();
}

static void UpdateView() {
    if (!g_viewOk) return;
    const int cur = *CurrArea;
    if (cur != g_viewArea || !g_viewDecided) { DecideView(cur, PlayerPos()); return; }
    const float* p = PlayerPos();
    if (cur == 0) { UpdateLook(p); return; }
    if (!vcmf_outside) return;
    const DWORD now = *(DWORD*)CLOCK;
    // Four times a second: the building the player has walked (or been put) into.
    static DWORD lastShell = 0;
    if (p && now - lastShell >= 250) { lastShell = now; HideContaining(p); }
    // Once a second: still next to the city? (A scripted scene may move the player
    // somewhere else in the same area.)
    static DWORD last = 0;
    if (now - last < 1000) return;
    last = now;
    if (p && NearestOfArea(0, p) > 80.0f) {
        Unhide();
        vcmf_outside = false;
        vcmf_also = -1;
        if (g_viewLogs++ < 50) Log("Interiors: area %d is away from the city here; outside view off", cur);
    }
}

// ---------------------------------------------------------------- second area behind
// Where the interior and the city building around it draw the same wall or floor,
// the two fight for the same pixels and flicker. While a second area is drawn
// (vcmf_also), everything is drawn with the viewport's depth range shrunk by a
// tiny fraction (MaxZ * (1 - eps)), except the second area's buildings: those keep
// the full range, so they end up slightly further away and lose every tie with the
// area the player is in. Buildings are drawn one at a time by
// CRenderer::RenderOneNonRoad (0x4C9DA0, called from 0x4C9ED1, 0x4C9F23, 0x4CA10D
// and the fading-entity list at 0x5813D1); a wrapper puts the full depth range in
// place around a second-area building. eps is a few steps of the depth buffer:
// 16-bit depth needs more than 24-bit.
static const DWORD RENDER_ONE = 0x4C9DA0;
static const DWORD kRenderSites[] = { 0x4C9ED1, 0x4C9F23, 0x4CA10D, 0x5813D1 };
static DWORD g_renderOneFn = RENDER_ONE;
static IDirect3DDevice8* g_dev = nullptr;
static SetViewport_t g_setViewport = nullptr;
static D3DVIEWPORT8 g_vp;
static bool g_vpValid = false, g_drawingAlso = false, g_biasOk = false;
static float g_eps = 3e-5f;

void InteriorDepthFormat(int fmt) {
    g_eps = (fmt == D3DFMT_D16 || fmt == D3DFMT_D16_LOCKABLE || fmt == D3DFMT_D15S1) ? 1.2e-4f : 3e-5f;
}

static D3DVIEWPORT8 Biased(const D3DVIEWPORT8& vp) {
    D3DVIEWPORT8 b = vp;
    if (g_biasOk && vcmf_also >= 0 && !g_drawingAlso) b.MaxZ = vp.MinZ + (vp.MaxZ - vp.MinZ) * (1.0f - g_eps);
    return b;
}

HRESULT InteriorSetViewport(IDirect3DDevice8* dev, const D3DVIEWPORT8* vp, SetViewport_t orig) {
    g_dev = dev;
    g_setViewport = orig;
    if (!vp) return orig(dev, vp);
    g_vp = *vp;
    g_vpValid = true;
    const D3DVIEWPORT8 b = Biased(*vp);
    return orig(dev, &b);
}

static void __cdecl HookRenderOne(unsigned char* e) {
    const bool second = g_biasOk && vcmf_also >= 0 && g_vpValid && g_dev && g_setViewport && e &&
                        e[ENT_AREA] == vcmf_also;
    if (second) { g_drawingAlso = true; g_setViewport(g_dev, &g_vp); }
    ((void (__cdecl*)(unsigned char*))g_renderOneFn)(e);
    if (second) {
        g_drawingAlso = false;
        const D3DVIEWPORT8 b = Biased(g_vp);
        g_setViewport(g_dev, &b);
    }
}

// ---------------------------------------------------------------- AutoSwitch
static bool CeilingAbove(const float* p) {
    const float up[3] = { p[0], p[1], p[2] + 30.0f };
    return !LineOfSightClear(p, up, true, false, false, false, false, true, false);
}

// The area the player should be in, or -1 for "leave it as it is".
static int Wanted(const float* p) {
    const int cur = *CurrArea;
    alignas(16) unsigned char colPoint[64];
    unsigned char* hit = nullptr;
    const bool found = ProcessVerticalLine(p, p[2] - 60.0f, colPoint, &hit, true, false, false, false, false, true, nullptr);
    if (found && hit) {
        const int a = RealArea(hit);
        if (a == AREA_EVERYWHERE || a == cur) return -1;
        if (a != 0) return (cur != 0 || CeilingAbove(p)) ? a : -1;    // standing in another interior
        // On the city's ground while an interior is set.
        if (NearestOfArea(cur, p) > 0.0f) return 0;
        if (cur == g_forced && !CeilingAbove(p)) return 0;
        return -1;
    }
    if (cur != 0 && NearestOfArea(cur, p) > 30.0f) return 0;   // in the air, far from the room
    return -1;
}

static void Switch(int area, const float* p) {
    const int from = *CurrArea;
    const float pos[3] = { p[0], p[1], p[2] };
    *CurrArea = area;
    HookRemove(area);
    TimerStop();
    LoadScene(pos);
    TimerUpdate();
    if (area == 0 && !OnMission()) {   // what the door scripts undo on the way out
        StopExtraColour(false);
        if (*CarDensity < 1.0f) *CarDensity = 1.0f;
    }
    g_forced = area != 0 ? area : -1;
    if (g_switches++ < 50)
        Log("Interiors: area %d -> %d at %.1f %.1f %.1f (scene loaded)", from, area, pos[0], pos[1], pos[2]);
}

static void UpdateSwitch() {
    if (!g_cfg.interiors) return;
    const int cur = *CurrArea;
    if (g_forced >= 0 && cur != g_forced) g_forced = -1;   // the script changed it since
    unsigned char* ped = nullptr;
    if (*(int*)GAME_STATE == 9 && !*(unsigned char*)MENU_ACTIVE && !*(unsigned short*)PAD0_DISABLE)
        ped = FindPlayerPed();
    const int state = ped ? *(int*)(ped + PED_STATE) : 0;
    if (!ped || state == 54 || state == 55) { g_pending = -1; return; }   // dying / dead: the game resets it
    unsigned char* veh = FindPlayerVehicle();
    const float* p = (const float*)((veh ? veh : ped) + ENT_POS);
    const int want = Wanted(p);
    const DWORD now = *(DWORD*)CLOCK;
    if (want < 0 || want == cur) { g_pending = -1; return; }
    if (want != g_pending) { g_pending = want; g_pendingSince = now; return; }
    if (now - g_pendingSince < 500) return;
    g_pending = -1;
    Switch(want, p);
}

// ------------------------------------------------- people in a room seen from the street
// With the room's ped paths on (PedRoadsOn) the game still wouldn't put anyone in
// it: CPopulation::AddToPopulation (0x53BA80) creates people 40-50 m from the
// player when in view, 15-25 m when not, and the room is closer than that. And
// ManagePopulation deletes anyone more than 25 m away who has been off screen for
// 4 s (CPed +0x35C, the time they may stay), so the crowd left inside is deleted
// when you walk out and turn to the street. So, while a room with ped paths is
// shown from the street and the player is within 45 m of it:
//  - twice a second AddToPopulation is called once more, centred on the room
//    (its FindPlayerCentreOfWorld call, 0x53BAE6, answers the room's centre) with a
//    radius that stays inside the room's box, and without its "in view and closer
//    than 40 m" test (0x53C937); up to 12 people in the room;
//  - every update, people inside the room's box get their 4 s again.
static const DWORD ADD_TO_POPULATION = 0x53BA80, CENTRE_SITE = 0x53BAE6, VIS_SITE = 0x53C937;
static const DWORD VIS_OK = 0x53CF57;   // the asm below continues at 0x53C93D
static const DWORD PED_POOL = 0x97F2AC, PED_SIZE = 0x6D8, PED_STAY_UNTIL = 0x35C;
extern "C" { int vcmf_force_spawn = 0; void vcmf_spawn_vis(); }
// At 0x53C937 (je VIS_OK, after "test al, al" with al = point in view): while
// vcmf_force_spawn is set, the point counts as out of view.
__asm__(".text\n.globl _vcmf_spawn_vis\n_vcmf_spawn_vis:\n"
        "  testb %al, %al\n  je 1f\n  cmpl $0, _vcmf_force_spawn\n  jne 1f\n"
        "  pushl $0x53C93D\n  ret\n"
        "1: pushl $0x53CF57\n  ret\n");
static DWORD g_centreFn = 0;
static bool g_spawnOk = false;
static float g_spawnCentre[3];
static unsigned g_spawnLogs = 0;

static float* __cdecl HookCentre(int player) {
    if (vcmf_force_spawn) return g_spawnCentre;
    return ((float* (__cdecl*)(int))g_centreFn)(player);
}

template <class F> static void ForEachPed(F f) {
    unsigned char* pool = *(unsigned char**)PED_POOL;   // { objects, flags, size }
    if (!pool) return;
    unsigned char* objs = *(unsigned char**)pool;
    const unsigned char* flags = *(unsigned char**)(pool + 4);
    const int size = *(int*)(pool + 8);
    if (!objs || !flags || size <= 0 || size > 1000) return;
    for (int i = 0; i < size; i++)
        if (!(flags[i] & 0x80)) f(objs + i * PED_SIZE);
}

static void RoomPeople(const float* p) {
    if (!g_spawnOk || g_pedRoadsOn < 0 || *CurrArea != 0 || !p) return;
    int j = 0;
    while (j < g_nPedRoads && g_pedRoads[j].area != g_pedRoadsOn) j++;
    if (j == g_nPedRoads) return;
    const float* lo = g_pedRoads[j].lo, * hi = g_pedRoads[j].hi;
    const float c[3] = { 0.5f * (lo[0] + hi[0]), 0.5f * (lo[1] + hi[1]), p[2] };
    const float dx = p[0] - c[0], dy = p[1] - c[1];
    if (dx * dx + dy * dy > 45.0f * 45.0f) return;
    const DWORD now = *(DWORD*)CLOCK;
    unsigned char* player = FindPlayerPed();
    int inside = 0;
    ForEachPed([&](unsigned char* ped) {
        if (ped == player) return;
        const float* q = (const float*)(ped + ENT_POS);
        if (q[0] < lo[0] || q[0] > hi[0] || q[1] < lo[1] || q[1] > hi[1] || q[2] < lo[2] || q[2] > hi[2]) return;
        *(DWORD*)(ped + PED_STAY_UNTIL) = now + 4000;
        inside++;
    });
    static DWORD last = 0;
    if (now - last < 500 || inside >= 12 || *(unsigned short*)PAD0_DISABLE) return;
    last = now;
    float r = 0.5f * (hi[0] - lo[0]);
    if (0.5f * (hi[1] - lo[1]) < r) r = 0.5f * (hi[1] - lo[1]);
    if (r > 25.0f) r = 25.0f;
    memcpy(g_spawnCentre, c, sizeof c);
    vcmf_force_spawn = 1;
    ((void (__cdecl*)(float, float, float, float))ADD_TO_POPULATION)(0.0f, r, 0.0f, r);
    vcmf_force_spawn = 0;
    if (g_spawnLogs < 3) {
        Log("Interiors: walking people into area %d seen from the street (%d inside, radius %.0f m)", g_pedRoadsOn, inside, r);
        g_spawnLogs++;
    }
}

// ------------------------------------------------------------- no door fade
// Every interior door script calls one shared main.scm routine: fade out (500 ms,
// 1500 ms for the Malibu), wait until black, and after the area is switched and the
// scene loaded, fade back in. Its six DO_FADE commands (016A) are at fixed places
// in main.scm; when the DO_FADE handler (0x45410F) is running one of those, its
// TheCamera.Fade call (0x454130) is skipped: the screen never goes black, and the
// routine's "while fading" loop ends at once. Every other fade (missions,
// cutscenes, deaths, arrests) is left alone. Off: [Interiors] NoDoorFade=0.
static DWORD CallTarget(DWORD site);
static void Redirect(DWORD site, void* target);
static const DWORD SCRIPT_SPACE = 0x821280, FADE_SITE = 0x454130, CAMERA_FADE = 0x46FBCB;
static const DWORD kDoorFades[] = { 0x2033E, 0x20366, 0x20374, 0x2041B, 0x20443, 0x20451 };
extern "C" { DWORD vcmf_fade_fn = 0; int vcmf_fade_skip(unsigned char* script); void vcmf_fade(); }
// At 0x454130, in place of "call CCamera::Fade" (thiscall, float time, short dir,
// ret 8): esi is the running CRunningScript.
__asm__(".text\n.globl _vcmf_fade\n_vcmf_fade:\n"
        "  pushl %ecx\n  pushl %esi\n  call _vcmf_fade_skip\n  addl $4, %esp\n  popl %ecx\n"
        "  testl %eax, %eax\n  jnz 1f\n  jmp *_vcmf_fade_fn\n"
        "1: ret $8\n");
static unsigned g_fadeLogs = 0;

extern "C" int vcmf_fade_skip(unsigned char* script) {
    if (!script) return 0;
    const DWORD ip = *(DWORD*)(script + 0x10);   // CRunningScript::m_nIp, just past DO_FADE's parameters
    for (DWORD at : kDoorFades) {
        // 016A, 16-bit time, 8-bit direction: 7 bytes; the main script is at the
        // start of the script space, as in main.scm.
        const unsigned char* op = (const unsigned char*)(SCRIPT_SPACE + at);
        if (ip == at + 7 && op[0] == 0x6A && op[1] == 0x01 && op[2] == 0x05 && op[5] == 0x04) {
            if (g_fadeLogs < 2) { Log("Interiors: door fade skipped (main.scm %05lX)", at); g_fadeLogs++; }
            return 1;
        }
    }
    return 0;
}

static void InstallNoDoorFade() {
    if (!g_cfg.noDoorFade) { Log("Interiors: door fades on"); return; }
    const DWORD target = CallTarget(FADE_SITE);
    static const unsigned char before[] = { 0xB9, 0x88, 0x46, 0x7E, 0x00 };   // mov ecx, TheCamera
    if (!target || memcmp((void*)(FADE_SITE - 0x10), before, sizeof before)) {
        Log("Interiors: DO_FADE code is not v1.0; door fades stay");
        return;
    }
    if (target != CAMERA_FADE) Log("Interiors: camera fade hooked by another mod (%08lX); chaining to it", target);
    vcmf_fade_fn = target;
    Redirect(FADE_SITE, (void*)&vcmf_fade);
    Log("Interiors: no fade to black at interior doors");
}

static void Update() {
    UpdateView();
    UpdateSwitch();
    // Parts still waiting to be put back (their building wasn't drawn when the view ended).
    if (g_nPunched && g_lookArea < 0 && !vcmf_outside) Unpunch();
    if (*CurrArea == 0 && g_lookArea >= 0) RoomPeople(PlayerPos());
}

void InteriorEndOfTick() {
    g_fromIdle = true;
    Update();
}

void InteriorAfterPresent() {
    if (!g_fromIdle) Update();   // FixedTick off: once per frame, after drawing
}

// ---------------------------------------------------------------- install
static DWORD CallTarget(DWORD site) {
    const unsigned char* p = (const unsigned char*)site;
    return p[0] == 0xE8 ? site + 5 + *(const int*)(p + 1) : 0;
}

static void Write(DWORD site, const void* bytes, int len) {
    DWORD old;
    VirtualProtect((void*)site, len, PAGE_EXECUTE_READWRITE, &old);
    memcpy((void*)site, bytes, len);
    VirtualProtect((void*)site, len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)site, len);
}

static void Redirect(DWORD site, void* target) {
    unsigned char call[5] = { 0xE8 };
    *(int*)(call + 1) = (int)((DWORD)target - (site + 5));
    Write(site, call, 5);
}

static void PatchCmp(DWORD site, void* stub) {
    unsigned char call[6] = { 0xE8, 0, 0, 0, 0, 0x90 };
    *(int*)(call + 1) = (int)((DWORD)stub - (site + 5));
    Write(site, call, 6);
}

static void InstallView() {
    if (!g_cfg.interiorView) { Log("Interiors: outside view off"); return; }
    static const unsigned char eaxCmp[] = { 0x3B, 0x05, 0x10, 0x88, 0x97, 0x00, 0x74 };
    static const unsigned char ebxCmp[] = { 0x3B, 0x1D, 0x10, 0x88, 0x97, 0x00, 0x74 };
    bool ok = !memcmp((void*)kEbxSite, ebxCmp, sizeof ebxCmp);
    for (DWORD s : kEaxSites) ok = ok && !memcmp((void*)s, eaxCmp, sizeof eaxCmp);
    for (DWORD s : kWaterSites) ok = ok && CallTarget(s) != 0;
    for (DWORD s : kRemoveSites) ok = ok && CallTarget(s) != 0;
    // "is water drawn here": cmp dword [currArea], 0; mov al, 1
    static const unsigned char water[] = { 0x83, 0x3D, 0x10, 0x88, 0x97, 0x00, 0x00, 0xB0, 0x01 };
    ok = ok && !memcmp((void*)WATER_AREA_FN, water, sizeof water);
    if (!ok) { Log("Interiors: area tests are not the v1.0 code; outside view not installed"); return; }
    const DWORD water0 = CallTarget(kWaterSites[0]), remove0 = CallTarget(kRemoveSites[0]);
    if (water0 != WATER_AREA_FN || CallTarget(kWaterSites[1]) != water0 ||
        remove0 != REMOVE_NOT_IN_AREA || CallTarget(kRemoveSites[1]) != remove0)
        Log("Interiors: area calls hooked by another mod (%08lX %08lX); chaining to them", water0, remove0);
    g_waterFn = water0;
    g_removeFn = remove0;
    for (DWORD s : kEaxSites) PatchCmp(s, (void*)&vcmf_area_cmp_eax);
    PatchCmp(kEbxSite, (void*)&vcmf_area_cmp_ebx);
    for (DWORD s : kWaterSites) Redirect(s, (void*)&HookWater);
    for (DWORD s : kRemoveSites) Redirect(s, (void*)&HookRemove);
    // Second area slightly behind: only if all four draw calls are there.
    bool renderOk = true;
    for (DWORD s : kRenderSites) renderOk = renderOk && CallTarget(s) == CallTarget(kRenderSites[0]) && CallTarget(s);
    if (renderOk) {
        g_renderOneFn = CallTarget(kRenderSites[0]);
        if (g_renderOneFn != RENDER_ONE) Log("Interiors: building drawing hooked by another mod (%08lX); chaining to it", g_renderOneFn);
        for (DWORD s : kRenderSites) Redirect(s, (void*)&HookRenderOne);
        g_biasOk = true;
    } else {
        Log("Interiors: building draw calls are not the v1.0 code; overlapping surfaces may flicker");
    }
    LoadPedRoads();
    // People walked into a room seen from the street (RoomPeople).
    static const unsigned char visJe[] = { 0x0F, 0x84 };
    const DWORD visTarget = VIS_SITE + 6 + *(const int*)(VIS_SITE + 2);
    if (g_nPedRoads && CallTarget(CENTRE_SITE) && !memcmp((void*)VIS_SITE, visJe, 2) && visTarget == VIS_OK &&
        !memcmp((void*)(VIS_SITE - 2), "\x84\xC0", 2) && *(unsigned char*)ADD_TO_POPULATION == 0x53) {
        g_centreFn = CallTarget(CENTRE_SITE);
        Redirect(CENTRE_SITE, (void*)&HookCentre);
        unsigned char jmp[6] = { 0xE9, 0, 0, 0, 0, 0x90 };
        *(int*)(jmp + 1) = (int)((DWORD)&vcmf_spawn_vis - (VIS_SITE + 5));
        Write(VIS_SITE, jmp, 6);
        g_spawnOk = true;
    } else if (g_nPedRoads) {
        Log("Interiors: population code is not v1.0; rooms seen from the street get no extra people");
    }
    g_viewOk = true;
    g_viewArea = *CurrArea;
    g_viewDecided = false;
    Log("Interiors: outside view installed (the city is drawn from inside interiors built into it)");
}

void InstallInterior() {
    InstallNoDoorFade();
    if (!g_cfg.interiors && !g_cfg.interiorView) { Log("Interiors: off"); return; }
    struct { DWORD addr; unsigned char bytes[8]; int len; } checks[] = {
        { 0x4D8B00, { 0x53, 0x56, 0x55, 0x83, 0xEC, 0x40, 0x66, 0x83 }, 8 },   // CWorld::ProcessVerticalLine
        { 0x40DAA0, { 0x53, 0x8B, 0x1D, 0x40, 0xF2, 0x97, 0x00 }, 7 },         // RemoveBuildingsNotInArea
        { 0x40AF60, { 0x53, 0x56, 0x57, 0x55, 0x83, 0xEC, 0x70 }, 7 },         // CStreaming::LoadScene
        { 0x4D0DB0, { 0xA1, 0x2C, 0x4B, 0x97, 0x00, 0xA3, 0x8C, 0x8D }, 8 },   // CTimer::Stop
        { 0x4CE9E0, { 0x80, 0x7C, 0x24, 0x04, 0x00 }, 5 },                     // CTimeCycle::StopExtraColour
        { 0x630B4E, { 0xA3, 0x10, 0x88, 0x97, 0x00, 0xE8 }, 6 },               // SET_AREA_VISIBLE: currArea
    };
    for (auto& c : checks) {
        if (memcmp((void*)c.addr, c.bytes, c.len)) {
            g_cfg.interiors = g_cfg.interiorView = false;
            Log("Interiors: game code at %08lX is not the v1.0 code; not installed", c.addr);
            return;
        }
    }
    InstallView();
    if (g_cfg.interiors) Log("Interiors: auto switch installed (the drawn area follows the player)");
}
