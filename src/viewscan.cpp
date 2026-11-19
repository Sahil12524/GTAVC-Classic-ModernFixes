// The world scan sees the whole screen at any camera angle (buildings, roads,
// bridges and objects at the screen's edges vanished while banking or pitching a
// helicopter or plane).
//
// CRenderer::ScanWorld (0x4C85E0) finds what to draw by marking the world sectors
// (50 m squares) under the view on the map, with CRenderer::ScanSectorPoly
// (0x4C8FE0; cdecl: polygon of 2D points in sector units, number of points, the
// function run on each sector). It gives it a triangle: the camera and two corners
// of the view at one depth, the top two when the camera looks up, the bottom two
// when it looks down (the camera's forward z at 0x4C87B1). The view's footprint on
// the map is the camera and all four corners; level and upright the triangle
// covers it, but rolled (a helicopter banking) or pitched steeply, the other two
// corners stick out past it, and every sector there - under the screen's edges -
// was never scanned: what stood in it wasn't drawn. The streaming request for what
// is in view (0x4C6D70, call at 0x4C7127) builds the same triangle.
//
// The three calls (0x4C7127, 0x4C8B08: the near "priority" part, 0x4C8C29: up to the
// 300 m detail distance or the far clip) get a polygon covering all of it: the
// triangle's depth is worked out from its corner (the corner directions come from
// the RenderWare camera, Scene.camera 0x7E4E90: its frame's matrix and view window,
// as ScanWorld itself takes them), and the scan gets the outline (convex hull) of
// the camera and the four corners at that depth. When a triangle doesn't match the
// camera this way, it is scanned as the game made it.
#include "viewscan.h"
#include "config.h"
#include <windows.h>
#include <math.h>

static const DWORD SCAN_POLY = 0x4C8FE0;
static const DWORD kSites[] = { 0x4C7127, 0x4C8B08, 0x4C8C29 };
static const DWORD SCENE_CAMERA = 0x7E4E90;
static const float kCell = 0.02f, kOffX = 48.0f, kOffY = 40.0f;   // world -> sector units (0x690230..38)
typedef void (__cdecl* Scan_t)(float*, int, void*);
static Scan_t g_scanFn[3] = { (Scan_t)SCAN_POLY, (Scan_t)SCAN_POLY, (Scan_t)SCAN_POLY };
static volatile long g_wider = 0, g_asGame = 0;

struct P2 { float x, y; };
static float Cross(const P2& o, const P2& a, const P2& b) { return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x); }

// Convex hull of n <= 7 points (monotone chain), counter-clockwise; returns its size.
static int Hull(P2* p, int n, P2* out) {
    for (int i = 1; i < n; i++)   // sort by x, then y
        for (int j = i; j > 0 && (p[j].x < p[j - 1].x || (p[j].x == p[j - 1].x && p[j].y < p[j - 1].y)); j--) {
            const P2 t = p[j]; p[j] = p[j - 1]; p[j - 1] = t;
        }
    int k = 0;
    for (int i = 0; i < n; i++) {
        while (k >= 2 && Cross(out[k - 2], out[k - 1], p[i]) <= 0) k--;
        out[k++] = p[i];
    }
    for (int i = n - 2, t = k + 1; i >= 0; i--) {
        while (k >= t && Cross(out[k - 2], out[k - 1], p[i]) <= 0) k--;
        out[k++] = p[i];
    }
    return k - 1;
}

// The polygon for the game's triangle, or 0 to scan the triangle itself.
static int Widen(const float* tri, float* out) {
    const unsigned char* cam = *(const unsigned char* const*)SCENE_CAMERA;
    if (!cam) return 0;
    const unsigned char* frame = *(const unsigned char* const*)(cam + 4);
    if (!frame) return 0;
    const float* m = (const float*)(frame + 0x10);   // right, up, at, pos (RwMatrix)
    const float vwx = *(const float*)(cam + 0x68), vwy = *(const float*)(cam + 0x6C);
    if (!(vwx > 0.0f && vwx < 50.0f && vwy > 0.0f && vwy < 50.0f)) return 0;
    P2 w[3];
    for (int i = 0; i < 3; i++) w[i] = { (tri[i * 2] - kOffX) / kCell, (tri[i * 2 + 1] - kOffY) / kCell };
    const P2 c = { m[12], m[13] };
    if (!(fabsf(w[0].x - c.x) < 2.0f && fabsf(w[0].y - c.y) < 2.0f)) return 0;
    // the four corner directions at depth 1, on the map
    static const float sx[4] = { -1, 1, 1, -1 }, sy[4] = { 1, 1, -1, -1 };
    P2 dir[4];
    for (int k = 0; k < 4; k++) {
        dir[k].x = m[0] * sx[k] * vwx + m[4] * sy[k] * vwy + m[8];
        dir[k].y = m[1] * sx[k] * vwx + m[5] * sy[k] * vwy + m[9];
    }
    // the triangle's depth: its two corners must be two of these four at one depth.
    // Each corner can lie along more than one direction on the map (two corners'
    // directions line up when the camera is rolled and pitched), so every depth that
    // fits is kept and the one both corners share is taken.
    float cand[2][4];
    int nc[2] = { 0, 0 };
    for (int i = 1; i < 3; i++) {
        const P2 v = { w[i].x - c.x, w[i].y - c.y };
        const float len = sqrtf(v.x * v.x + v.y * v.y);
        for (int k = 0; k < 4; k++) {
            const float dd = dir[k].x * dir[k].x + dir[k].y * dir[k].y;
            if (dd < 1e-6f) continue;
            const float d = (v.x * dir[k].x + v.y * dir[k].y) / dd;
            const float rx = v.x - d * dir[k].x, ry = v.y - d * dir[k].y;
            if (d > 0.0f && sqrtf(rx * rx + ry * ry) <= 1.0f + 0.01f * len) cand[i - 1][nc[i - 1]++] = d;
        }
    }
    float depth = -1.0f, best = 1e30f;
    for (int a = 0; a < nc[0]; a++)
        for (int b = 0; b < nc[1]; b++) {
            const float diff = fabsf(cand[0][a] - cand[1][b]);
            if (diff <= 0.5f + 0.01f * cand[0][a] && diff < best) { best = diff; depth = (cand[0][a] + cand[1][b]) * 0.5f; }
        }
    // No shared depth: the near "priority" scan, whose corners the game builds with
    // only some of their axes scaled (0x4C8992..0x4C8A06), so they are on no corner
    // of the view. It gets its own triangle plus the four corners at the depth of
    // its farther point over the longest corner direction: never less than the
    // game's, about as far.
    int np = 5;
    P2 pts[7] = { c }, hull[14];
    if (!(depth > 0.0f && depth < 20000.0f)) {
        float far2 = 0.0f, dmax = 0.0f;
        for (int i = 1; i < 3; i++) {
            const float vx = w[i].x - c.x, vy = w[i].y - c.y;
            if (vx * vx + vy * vy > far2) far2 = vx * vx + vy * vy;
        }
        for (int k = 0; k < 4; k++) {
            const float dd = dir[k].x * dir[k].x + dir[k].y * dir[k].y;
            if (dd > dmax) dmax = dd;
        }
        if (!(dmax > 1e-6f) || !(far2 > 1.0f)) return 0;
        depth = sqrtf(far2 / dmax);
        if (!(depth < 20000.0f)) return 0;
        pts[5] = w[1];
        pts[6] = w[2];
        np = 7;
    }
    for (int k = 0; k < 4; k++) pts[k + 1] = { c.x + dir[k].x * depth, c.y + dir[k].y * depth };
    const int n = Hull(pts, np, hull);
    if (n < 3 || n > 7) return 0;
    for (int i = 0; i < n; i++) {
        out[i * 2] = hull[i].x * kCell + kOffX;
        out[i * 2 + 1] = hull[i].y * kCell + kOffY;
    }
    return n;
}

template <int I>
static void __cdecl HookScan(float* poly, int n, void* fn) {
    if (n == 3 && poly && g_cfg.fullViewScan) {
        float wide[16];   // up to 7 points
        const int m = Widen(poly, wide);
        if (m >= 3) {
            InterlockedIncrement(&g_wider);
            g_scanFn[I](wide, m, fn);
            return;
        }
        InterlockedIncrement(&g_asGame);
    }
    g_scanFn[I](poly, n, fn);
}

static void* const kHooks[] = { (void*)&HookScan<0>, (void*)&HookScan<1>, (void*)&HookScan<2> };

void InstallViewScan() {
    int done = 0;
    for (int i = 0; i < 3; i++) {
        const unsigned char* p = (const unsigned char*)kSites[i];
        if (p[0] != 0xE8) continue;
        g_scanFn[i] = (Scan_t)(kSites[i] + 5 + *(const int*)(p + 1));   // the game's, or another mod's hook
        unsigned char call[5] = { 0xE8 };
        *(int*)(call + 1) = (int)((DWORD)kHooks[i] - (kSites[i] + 5));
        DWORD old;
        VirtualProtect((void*)kSites[i], 5, PAGE_EXECUTE_READWRITE, &old);
        memcpy((void*)kSites[i], call, 5);
        VirtualProtect((void*)kSites[i], 5, old, &old);
        done++;
    }
    FlushInstructionCache(GetCurrentProcess(), nullptr, 0);
    Log("ViewScan: the world scan covers the whole view at any camera angle (%d of 3 scans; edges vanished while "
        "banking / pitching)", done);
}

void ViewScanLog() {
    static DWORD at = 0;
    static int lines = 0;
    const DWORD now = GetTickCount();
    if (lines >= 10 || now - at < 120000) return;
    at = now;
    const long w = InterlockedExchange(&g_wider, 0), g = InterlockedExchange(&g_asGame, 0);
    if (!w && !g) return;
    lines++;
    Log("ViewScan: %ld scans covered the whole view, %ld scanned as the game made them (no camera)", w, g);
}
