// The Modern Fix page in the game's Options menu: every fix that acts while playing
// can be turned on and off there, and the change is saved to VCModernFix.ini.
//
// The game's frontend (CMenuManager, FrontEndMenuManager 0x869630) draws its pages
// from a table, aScreens (0x6D8B70, 35 pages of 226 bytes: name[8], previous page,
// row on it, then 12 rows of 18 bytes: action (short), text key[8], save slot,
// target page, x, y (short), alignment). Each row's text is TheText.Get(key)
// (CText::Get 0x584F30, TheText 0x94B220). m_nCurrScreen is at +0xF8, m_nCurrOption
// at +0x30.
//
// Page 22 is never used by the game (reVC: MENUPAGE_SAVE_UNUSED): it becomes the
// Modern Fix page, reached from a new row under Options (above Back). Its rows are
// rewritten here: first the list of categories, then the switches of the one picked.
// The text keys are our own ("VMF..."), answered by a hook on CText::Get. The rows
// use actions the game doesn't have, which its code passes over to a default case;
// three of those default jumps come here instead:
//   0x49ebf4 (drawing the right column: ON / OFF, the game's own words),
//   0x49972f (Enter or a click on the row: switch it, or open a category),
//   0x499db7 (left / right on the row: switch it).
// Back (the game's row, or Esc) from a category goes to the category list: the
// game's CMenuManager::SwitchToNewScreen (0x4983EF, -2 = back) is hooked for it.
#include "fixmenu.h"
#include "config.h"
#include "alphafix.h"
#include "faststream.h"
#include "freecam.h"
#include "gameplay.h"
#include "modernkeys.h"
#include "population.h"
#include <windows.h>
#include <string.h>
#include <stdio.h>

#pragma pack(push, 1)
struct MenuRow {
    short action;
    char key[8];
    signed char slot, target;
    unsigned short x, y;
    unsigned char align, pad;
};
struct MenuPage {
    char name[8];
    signed char prev, parent;
    MenuRow rows[12];
};
#pragma pack(pop)
static_assert(sizeof(MenuRow) == 18 && sizeof(MenuPage) == 226, "aScreens layout");

static MenuPage* const Pages = (MenuPage*)0x6D8B70;
static const int kOptions = 27, kPage = 22;
static const int kOptionsRow = 5;                       // our row on the Options page
static const short ACT_CHANGEMENU = 4, ACT_BACK = 34;
static const short ACT_CAT = 0x60, ACT_ITEM = 0x70;     // + index; the game's actions end at 53
static const DWORD TEXT_GET = 0x584F30, THE_TEXT = 0x94B220, SWITCH_SCREEN = 0x4983EF;
static const DWORD RIGHT_DEFAULT = 0x49EBF4, RIGHT_DONE = 0x49EF35;
static const DWORD SELECT_DEFAULT = 0x49972F, SELECT_DONE = 0x499CB2;
static const DWORD CHANGE_DEFAULT = 0x499DB7, CHANGE_DONE = 0x499FCC;

// ---------------------------------------------------------------- the switches
enum Kind { BOOL_, INT_, FLOAT_ };
struct Item {
    const char* label;
    Kind kind;
    void* value;
    float off, on;              // INT_ / FLOAT_: the value meaning off, and the default when on
    const char* section;
    const char* key;
    void (*changed)();
    float* value2;              // a second number switched with it (draw distance: far clip)
    float on2;
    const char* key2;
    float remembered, remembered2;   // the on value to go back to (the ini's, or the default)
};

static void NoOp() {}
static Item kGraphics[] = {
    { "Two-pass leaves", BOOL_, &g_cfg.twoPassAlpha, 0, 0, "Graphics", "TwoPassAlpha", NoOp },
    { "Anisotropic filtering", INT_, &g_cfg.anisotropy, 0, 16, "Graphics", "Anisotropic", AnisotropyChanged },
    { "Longer draw distance", FLOAT_, &g_cfg.drawDistance, 1.0f, 3.0f, "DrawDistance", "Scale", NoOp,
      &g_cfg.farClip, 1.5f, "FarClip" },
    { "Faster model loading", INT_, &g_cfg.fastLoadMs, 0, 4, "Streaming", "FastLoadMs", NoOp },
    { "No island loading screen", BOOL_, &g_cfg.noIslandScreen, 0, 0, "Streaming", "NoIslandScreen", NoIslandScreenApply },
    { "Far view when high up", BOOL_, &g_cfg.highView, 0, 0, "DrawDistance", "HighView", NoOp },
    { "No vanishing edges", BOOL_, &g_cfg.fullViewScan, 0, 0, "DrawDistance", "FullViewScan", NoOp },
    { "Far city lights", BOOL_, &g_cfg.farLights, 0, 0, "Graphics", "FarLights", NoOp },
    { "Longer corona distance", FLOAT_, &g_cfg.coronaDistance, 1.0f, 5.0f, "Graphics", "CoronaDistance", NoOp },
    { "Clouds rise when high up", BOOL_, &g_cfg.risingClouds, 0, 0, "Graphics", "RisingClouds", NoOp },
    { "Wide FOV near clip", BOOL_, &g_cfg.wideNearClip, 0, 0, "Graphics", "WideFovNearClip", NoOp },
};
static Item kCamera[] = {
    { "Vehicle mouse look", BOOL_, &g_cfg.freeCam, 0, 0, "Camera", "FreeCam", FreeCamBoatsApply },
    { "Mouse look on boats", BOOL_, &g_cfg.freeCamBoats, 0, 0, "Camera", "FreeCamBoats", FreeCamBoatsApply },
    { "Vehicle camera returns", INT_, &g_cfg.freeCamReturnDelay, -1, 1500, "Camera", "FreeCamReturnDelay", NoOp },
    { "Invert vehicle mouse Y", BOOL_, &g_cfg.freeCamInvertY, 0, 0, "Camera", "FreeCamInvertY", NoOp },
    { "Mouse read every frame", BOOL_, &g_cfg.frameMouseLook, 0, 0, "Camera", "FrameMouseLook", NoOp },
    { "Free camera on foot", BOOL_, &g_cfg.freeOrbitOnFoot, 0, 0, "Camera", "FreeOrbitOnFoot", NoOp },
    { "Camera swings behind", BOOL_, &g_cfg.cameraSwingBehind, 0, 0, "Camera", "CameraSwingBehind", NoOp },
    { "Equal mouse axes", BOOL_, &g_cfg.mouseEqualAxes, 0, 0, "Camera", "MouseEqualAxes", NoOp },
    { "Mouse smoothing", INT_, &g_cfg.mouseSmoothMs, 0, 4, "Camera", "MouseSmoothing", NoOp },
    { "Car look forward key", INT_, &g_cfg.carLookForwardKey, 0, 0x43, "Camera", "CarLookForwardKey", NoOp },
};
static Item kVehicles[] = {
    { "Modern keys", BOOL_, &g_cfg.modernKeys, 0, 0, "Controls", "ModernKeys", ModernKeysChanged },
    { "Turret mouse aim", BOOL_, &g_cfg.mouseAim, 0, 0, "Vehicles", "MouseAim", NoOp },
    { "Car air control", BOOL_, &g_cfg.airControl, 0, 0, "Vehicles", "AirControl", NoOp },
    { "Helicopter controls", BOOL_, &g_cfg.heliControls, 0, 0, "Vehicles", "HeliControls", NoOp },
    { "Helicopter nose limit", INT_, &g_cfg.heliMaxPitch, 0, 30, "Vehicles", "HeliMaxPitch", NoOp },
    { "Helicopter lean limit", INT_, &g_cfg.heliMaxRoll, 0, 40, "Vehicles", "HeliMaxRoll", NoOp },
    { "Plane controls", BOOL_, &g_cfg.planeControls, 0, 0, "Vehicles", "PlaneControls", NoOp },
    { "Heavier helicopters", INT_, &g_cfg.heliWeight, 0, 100, "Vehicles", "HeliWeight", NoOp },
    { "Hunter weapon keys", BOOL_, &g_cfg.hunterWeapons, 0, 0, "Vehicles", "HunterWeapons", NoOp },
};
static Item kWeapons[] = {
    { "Run and gun", BOOL_, &g_cfg.runAndGun, 0, 0, "Weapons", "RunAndGun", GameplaySettingsChanged },
    { "Keep firing when hit", BOOL_, &g_cfg.keepFiringWhenHit, 0, 0, "Weapons", "KeepFiringWhenHit", NoOp },
    { "Free drive-by", BOOL_, &g_cfg.freeDriveBy, 0, 0, "Weapons", "FreeDriveBy", NoOp },
};
static Item kMissions[] = {
    { "Skip mission scenes", BOOL_, &g_cfg.introSkip, 0, 0, "Missions", "IntroSkip", NoOp },
    { "Stuck mission rescue", INT_, &g_cfg.softlockSeconds, 0, 90, "Missions", "SoftlockSeconds", NoOp },
    { "End mission key", INT_, &g_cfg.endMissionKey, 0, 0x23, "Missions", "EndMissionKey", NoOp },
    { "Restart failed missions", BOOL_, &g_cfg.missionRestart, 0, 0, "Missions", "RestartPrompt", NoOp },
    { "Pizza throw with mouse", BOOL_, &g_cfg.pizzaMouseThrow, 0, 0, "Missions", "PizzaMouseThrow", NoOp },
    { "Normal stunt jumps", BOOL_, &g_cfg.stuntJumpNormal, 0, 0, "StuntJumps", "NormalCamera", NoOp },
    { "Skip phone calls", BOOL_, &g_cfg.phoneSkip, 0, 0, "PhoneSkip", "Enabled", NoOp },
};
static Item kSaving[] = {
    { "Quick save keys", BOOL_, &g_cfg.quickSave, 0, 0, "QuickSave", "Enabled", NoOp },
    { "Timed quick save", INT_, &g_cfg.quickSaveEvery, 0, 5, "QuickSave", "AutoMinutes", NoOp },
    { "Auto save", BOOL_, &g_cfg.autoSave, 0, 0, "AutoSave", "Enabled", NoOp },
};
static Item kWorld[] = {
    { "Interior auto switch", BOOL_, &g_cfg.interiors, 0, 0, "Interiors", "AutoSwitch", NoOp },
    { "Rooms seen from street", BOOL_, &g_cfg.interiorView, 0, 0, "Interiors", "ShowOutside", NoOp },
    { "No door fade", BOOL_, &g_cfg.noDoorFade, 0, 0, "Interiors", "NoDoorFade", NoOp },
    { "More people", FLOAT_, &g_cfg.pedDensity, 0, 2.5f, "Population", "PedDensity", PopulationSettingsChanged },
    { "More traffic", FLOAT_, &g_cfg.carDensity, 0, 1.5f, "Population", "CarDensity", PopulationSettingsChanged },
    { "Keep people in view", FLOAT_, &g_cfg.offscreenKeep, 0, 50.0f, "Population", "KeepOffscreen", PopulationSettingsChanged },
    { "Fewer street cops", INT_, &g_cfg.maxStreetCops, -1, 3, "Population", "MaxStreetCops", NoOp },
    { "Mission leftovers fix", BOOL_, &g_cfg.missionLeftovers, 0, 0, "Fixes", "MissionLeftovers", NoOp },
    { "Clear riot on load", BOOL_, &g_cfg.clearRiotOnLoad, 0, 0, "Fixes", "ClearRiotOnLoad", NoOp },
    { "Police station 2-star fix", BOOL_, &g_cfg.stationFlag, 0, 0, "Fixes", "PoliceStationFlag", NoOp },
};

struct Category { const char* name; const char* title; Item* items; int count; };
#define CAT(n, t, a) { n, t, a, (int)(sizeof a / sizeof a[0]) }
static Category kCats[] = {
    CAT("Graphics", "GRAPHICS", kGraphics),
    CAT("Camera and mouse", "CAMERA AND MOUSE", kCamera),
    CAT("Vehicles", "VEHICLES", kVehicles),
    CAT("Weapons", "WEAPONS", kWeapons),
    CAT("Missions", "MISSIONS", kMissions),
    CAT("Saving", "SAVING", kSaving),
    CAT("World and fixes", "WORLD AND FIXES", kWorld),
};
#undef CAT
static const int kNumCats = (int)(sizeof kCats / sizeof kCats[0]);
// A page holds 12 rows: the switches and Back.
#define FITS(a) static_assert(sizeof a / sizeof a[0] <= 11, "too many rows for one menu page: " #a)
FITS(kGraphics); FITS(kCamera); FITS(kVehicles); FITS(kWeapons); FITS(kMissions); FITS(kSaving); FITS(kWorld);
#undef FITS

static float Get(const Item& it, const void* p) {
    return it.kind == BOOL_ ? (*(const bool*)p ? 1.0f : 0.0f)
         : it.kind == INT_ ? (float)*(const int*)p : *(const float*)p;
}
static void Set(const Item& it, void* p, float v) {
    if (it.kind == BOOL_) *(bool*)p = v != 0.0f;
    else if (it.kind == INT_) *(int*)p = (int)(v + (v < 0 ? -0.5f : 0.5f));
    else *(float*)p = v;
}
static bool IsOn(const Item& it) {
    return it.kind == BOOL_ ? *(const bool*)it.value : Get(it, it.value) != it.off;
}

static void Save(const char* section, const char* key, const Item& it, float v) {
    char buf[32];
    if (it.kind == FLOAT_) snprintf(buf, sizeof buf, "%.2f", v);
    else snprintf(buf, sizeof buf, "%d", (int)(v + (v < 0 ? -0.5f : 0.5f)));
    SaveSetting(section, key, buf);
}

static void Toggle(Item& it) {
    const bool on = !IsOn(it);
    if (it.kind == BOOL_) {
        *(bool*)it.value = on;
        Save(it.section, it.key, it, on ? 1.0f : 0.0f);
    } else {
        if (!on) it.remembered = Get(it, it.value);
        const float v = on ? it.remembered : it.off;
        Set(it, it.value, v);
        Save(it.section, it.key, it, v);
        if (it.value2) {
            if (!on) it.remembered2 = *it.value2;
            *it.value2 = on ? it.remembered2 : it.off;   // same off value (1 = the game's)
            Save(it.section, it.key2, it, *it.value2);
        }
    }
    it.changed();
    Log("Menu: %s %s", it.label, on ? "on" : "off");
}

// ---------------------------------------------------------------- the page
static int g_cat = -1;   // category shown, -1 = the list

static void SetRow(MenuRow& r, short action, const char* key, int x, int y, int align, int target = 0) {
    memset(&r, 0, sizeof r);
    r.action = action;
    strncpy(r.key, key, 7);
    r.target = (signed char)target;
    r.x = (unsigned short)x;
    r.y = (unsigned short)y;
    r.align = (unsigned char)align;
}

static void ShowList() {
    MenuPage& p = Pages[kPage];
    memset(p.rows, 0, sizeof p.rows);
    char key[8];
    for (int c = 0; c < kNumCats; c++) {
        snprintf(key, sizeof key, "VMFC%d", c);
        SetRow(p.rows[c], ACT_CAT + c, key, 320, 110 + 25 * c, 3);
    }
    SetRow(p.rows[kNumCats], ACT_BACK, "FEDS_TB", 320, 110 + 25 * kNumCats + 15, 3);
    g_cat = -1;
}

static void ShowCategory(int c) {
    MenuPage& p = Pages[kPage];
    memset(p.rows, 0, sizeof p.rows);
    const Category& cat = kCats[c];
    char key[8];
    for (int i = 0; i < cat.count; i++) {
        snprintf(key, sizeof key, "VMFI%d", i);
        SetRow(p.rows[i], ACT_ITEM + i, key, 40, 78 + 25 * i, 1);
    }
    SetRow(p.rows[cat.count], ACT_BACK, "FEDS_TB", 320, 78 + 25 * cat.count + 15, 3);
    g_cat = c;
}

// ---------------------------------------------------------------- text
typedef wchar_t* (__thiscall* Get_t)(void*, const char*);
static Get_t g_get = nullptr;

static const wchar_t* Wide(const char* s) {
    static wchar_t ring[16][48];
    static int at = 0;
    wchar_t* w = ring[at];
    at = (at + 1) % 16;
    int i = 0;
    for (; s[i] && i < 47; i++) w[i] = (unsigned char)s[i];
    w[i] = 0;
    return w;
}

static wchar_t* __fastcall HookGet(void* text, void*, const char* key) {
    if (key && key[0] == 'V' && key[1] == 'M' && key[2] == 'F') {
        const char k = key[3];
        const int n = key[4] >= '0' && key[4] <= '9' ? key[4] - '0' : -1;
        if (k == 'O') return (wchar_t*)Wide("Modern Fix");
        if (k == 'T') return (wchar_t*)Wide(g_cat >= 0 ? kCats[g_cat].title : "MODERN FIX");
        if (k == 'C' && n >= 0 && n < kNumCats) return (wchar_t*)Wide(kCats[n].name);
        if (k == 'I' && g_cat >= 0 && n >= 0 && n < kCats[g_cat].count) return (wchar_t*)Wide(kCats[g_cat].items[n].label);
        return (wchar_t*)Wide("");
    }
    return g_get(text, key);
}

// The right column of a row with one of our actions: ON / OFF in the game's words.
extern "C" const wchar_t* __cdecl vcmf_menu_right(int action) {
    if (g_cat < 0 || action < ACT_ITEM || action >= ACT_ITEM + kCats[g_cat].count) return nullptr;
    return g_get((void*)THE_TEXT, IsOn(kCats[g_cat].items[action - ACT_ITEM]) ? "FEM_ON" : "FEM_OFF");
}

// ---------------------------------------------------------------- input
typedef void (__thiscall* Switch_t)(void*, int);
static Switch_t g_switch = nullptr;

static short RowAction(unsigned char* menu) {
    const int screen = *(int*)(menu + 0xF8), row = *(int*)(menu + 0x30);
    if (screen < 0 || screen >= 35 || row < 0 || row >= 12) return 0;
    return Pages[screen].rows[row].action;
}

// Enter or a click on a row with one of our actions.
extern "C" void __cdecl vcmf_menu_select(unsigned char* menu) {
    if (*(int*)(menu + 0xF8) != kPage) return;
    const short a = RowAction(menu);
    if (a >= ACT_CAT && a < ACT_CAT + kNumCats) {
        ShowCategory(a - ACT_CAT);
        g_switch(menu, kPage);   // the game's page change (row 0)
    } else if (g_cat >= 0 && a >= ACT_ITEM && a < ACT_ITEM + kCats[g_cat].count) {
        Toggle(kCats[g_cat].items[a - ACT_ITEM]);
    }
}

// Left / right on a row with one of our actions.
extern "C" void __cdecl vcmf_menu_change(unsigned char* menu, int amount) {
    if (!amount || *(int*)(menu + 0xF8) != kPage || g_cat < 0) return;
    const short a = RowAction(menu);
    if (a >= ACT_ITEM && a < ACT_ITEM + kCats[g_cat].count) Toggle(kCats[g_cat].items[a - ACT_ITEM]);
}

static void __fastcall HookSwitch(unsigned char* menu, void*, int screenArg) {
    const signed char screen = (signed char)screenArg;
    const int cur = *(int*)(menu + 0xF8);
    if (cur == kPage && screen == -2 && g_cat >= 0) {
        // Back from a category: to the category list, on the category's row.
        MenuPage& p = Pages[kPage];
        const int cat = g_cat;
        ShowList();
        p.prev = kPage;
        p.parent = (signed char)cat;
        g_switch(menu, -2);
        p.prev = kOptions;
        p.parent = kOptionsRow;
        return;
    }
    if (cur != kPage && screen == 0) {   // a row's target page: ours starts at the list
        const int row = *(int*)(menu + 0x30);
        if (cur >= 0 && cur < 35 && row >= 0 && row < 12 && Pages[cur].rows[row].target == kPage) ShowList();
    }
    g_switch(menu, screenArg);
}

// jmp here from the default cases (registers as the game left them).
extern "C" void vcmf_menu_right_stub(), vcmf_menu_select_stub(), vcmf_menu_change_stub();
__asm__(".text\n"
        ".globl _vcmf_menu_right_stub\n_vcmf_menu_right_stub:\n"
        "  pushal\n"
        "  movswl 0x6D8B7A(%esi), %eax\n"          // this row's action
        "  pushl %eax\n  call _vcmf_menu_right\n  addl $4, %esp\n"
        "  testl %eax, %eax\n  jz 1f\n"
        "  movl %eax, 16(%esp)\n"                  // ebx in the pushal frame: the right text
        "1: popal\n  pushl $0x49EF35\n  ret\n"
        ".globl _vcmf_menu_select_stub\n_vcmf_menu_select_stub:\n"
        "  pushal\n  pushl %ebp\n  call _vcmf_menu_select\n  addl $4, %esp\n  popal\n"
        "  pushl $0x499CB2\n  ret\n"
        ".globl _vcmf_menu_change_stub\n_vcmf_menu_change_stub:\n"
        "  pushal\n"
        "  movsbl 0xA4(%esp), %eax\n"              // [esp+84h] before pushal: left -1 / right +1
        "  pushl %eax\n  pushl %ebp\n  call _vcmf_menu_change\n  addl $8, %esp\n  popal\n"
        "  pushl $0x499FCC\n  ret\n");

// ---------------------------------------------------------------- install
static void Write(DWORD at, const void* bytes, int n) {
    DWORD old;
    VirtualProtect((void*)at, n, PAGE_EXECUTE_READWRITE, &old);
    memcpy((void*)at, bytes, n);
    VirtualProtect((void*)at, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)at, n);
}

static DWORD JmpTarget(DWORD at) {
    const unsigned char* p = (const unsigned char*)at;
    return p[0] == 0xE9 ? at + 5 + *(const int*)(p + 1) : 0;
}

static void Jmp(DWORD at, void* to) {
    unsigned char j[5] = { 0xE9 };
    *(int*)(j + 1) = (int)((DWORD)to - (at + 5));
    Write(at, j, 5);
}

// A jump at the start of a function to hook; the bytes it covers are moved to a
// trampoline that goes on with the function. A function another mod already starts
// with a jump to its own hook is chained to that hook.
static void* HookStart(DWORD at, const unsigned char* expect, int len, void* hook) {
    void* orig = nullptr;
    if (!memcmp((void*)at, expect, len)) {
        unsigned char* t = (unsigned char*)VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (!t) return nullptr;
        memcpy(t, (void*)at, len);
        t[len] = 0xE9;
        *(int*)(t + len + 1) = (int)((at + len) - ((DWORD)t + len + 5));
        orig = t;
    } else if (DWORD t = JmpTarget(at)) {
        Log("Menu: %06lX is hooked by another mod (%08lX); chaining to it", at, t);
        orig = (void*)t;
    } else {
        return nullptr;
    }
    Jmp(at, hook);
    return orig;
}

static bool KeyIs(const char* k, const char* want) { return !strncmp(k, want, 8); }

void InstallFixMenu() {
    for (int c = 0; c < kNumCats; c++)
        for (int i = 0; i < kCats[c].count; i++) {
            Item& it = kCats[c].items[i];
            it.remembered = IsOn(it) ? Get(it, it.value) : it.on;
            if (it.value2) it.remembered2 = IsOn(it) ? *it.value2 : it.on2;
        }
    MenuPage& opt = Pages[kOptions];
    MenuPage& page = Pages[kPage];
    const bool tableOk = KeyIs(opt.name, "FET_OPT") && KeyIs(opt.rows[4].key, "FET_PS") &&
                         KeyIs(opt.rows[5].key, "FEDS_TB") && opt.rows[6].action == 0 && opt.rows[6].key[0] == 0 &&
                         KeyIs(page.name, "FET_SG") && KeyIs(page.rows[0].key, "FED_LWR");
    const bool jumpsOk = JmpTarget(RIGHT_DEFAULT) == RIGHT_DONE && JmpTarget(SELECT_DEFAULT) == SELECT_DONE &&
                         JmpTarget(CHANGE_DEFAULT) == CHANGE_DONE;
    if (!tableOk || !jumpsOk) {
        Log("Menu: the game's menu is not the v1.0 one (or changed by another mod: table %d, code %d); no Modern Fix page",
            tableOk, jumpsOk);
        return;
    }
    static const unsigned char getStart[] = { 0x53, 0x55, 0x89, 0xCB, 0x83, 0xEC, 0x40 };          // push ebx; push ebp; mov ebx,ecx; sub esp,40h
    static const unsigned char switchStart[] = { 0x53, 0x55, 0x89, 0xCD, 0x8B, 0x5C, 0x24, 0x0C };  // push ebx; push ebp; mov ebp,ecx; mov ebx,[esp+0Ch]
    g_get = (Get_t)HookStart(TEXT_GET, getStart, sizeof getStart, (void*)&HookGet);
    if (!g_get) { Log("Menu: CText::Get is not the v1.0 code; no Modern Fix page"); return; }
    g_switch = (Switch_t)HookStart(SWITCH_SCREEN, switchStart, sizeof switchStart, (void*)&HookSwitch);
    if (!g_switch) {
        Log("Menu: the menu's page change is not the v1.0 code; no Modern Fix page");
        return;   // the text hook stays: it only answers our own keys
    }
    // Options: our row above Back.
    opt.rows[6] = opt.rows[5];
    SetRow(opt.rows[5], ACT_CHANGEMENU, "VMFO", 0, 0, 3, kPage);
    // Page 22: ours.
    memset(page.name, 0, sizeof page.name);
    strcpy(page.name, "VMFT");
    page.prev = kOptions;
    page.parent = kOptionsRow;
    ShowList();
    Jmp(RIGHT_DEFAULT, (void*)&vcmf_menu_right_stub);
    Jmp(SELECT_DEFAULT, (void*)&vcmf_menu_select_stub);
    Jmp(CHANGE_DEFAULT, (void*)&vcmf_menu_change_stub);
    int n = 0;
    for (int c = 0; c < kNumCats; c++) n += kCats[c].count;
    Log("Menu: Options > Modern Fix: %d switches in %d groups, saved to VCModernFix.ini", n, kNumCats);
}
