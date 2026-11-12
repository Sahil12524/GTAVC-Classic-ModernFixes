# GTAVC Classic ModernFixes

Modern fixes and quality-of-life improvements for the **classic GTA: Vice City
(PC, version 1.0)**. Drop in one file, `d3d8.dll`, and the 2002 game:

- runs **smoothly at your monitor's full refresh rate** (144 Hz, 240 Hz...)
  while its physics, missions and timing behave exactly like the original;
- gets **quick save / quick load**, a **timed quick save** and **auto save after
  missions**;
- loses a set of **long-standing bugs**: the mouse that stops working, the crash
  on exit, freezes from disk reads, a ped-sight crash, a mission that leaves a
  hotel without walls;
- gains **mouse look in vehicles**, **run and gun with every weapon**, **busier
  streets**, a **longer draw distance**, **seamless interiors** and more.

Every feature can be switched off in `VCModernFix.ini`.

> Fan-made and unofficial. Not affiliated with or endorsed by Rockstar Games or
> Take-Two Interactive. This repository contains no game files; you need your
> own copy of the game.

---

## Contents

- [Requirements](#requirements)
- [Installation](#installation)
  - [Adding SilentPatch and other ASI mods](#adding-silentpatch-and-other-asi-mods-optional)
  - [A complete setup](#a-complete-setup)
- [Uninstalling](#uninstalling)
- [What it fixes](#what-it-fixes)
- [What it adds](#what-it-adds)
- [Controls](#controls)
- [Settings](#settings)
- [Compatibility with other mods](#compatibility-with-other-mods)
- [The log file](#the-log-file)
- [Troubleshooting](#troubleshooting)
- [Known limitations](#known-limitations)
- [Testing status](#testing-status)
- [Building from source](#building-from-source)
- [How it works (short version)](#how-it-works-short-version)
- [Credits](#credits)

---

## Requirements

| | |
| --- | --- |
| **Game** | Grand Theft Auto: Vice City for PC, **version 1.0**: the `gta-vc.exe` that is **3,088,896 bytes**. |
| **Operating system** | Windows 10 or Windows 11. The DLL uses the Universal C Runtime that ships with them; Windows 7/8 with the Universal C Runtime installed should work but is untested. |
| **Graphics** | Any GPU that runs the original game. DirectX 8 is part of Windows; nothing extra to install. |

**How to check your version:** right-click `gta-vc.exe`, choose **Properties**;
the size should read **3,088,896 bytes**. Setting the 4 GB flag (see
[Installation](#installation)) doesn't change the size.

**Other game versions** (1.1, Steam, Rockstar Games Launcher, the Definitive
Edition) are **not supported**: they keep the game's code at different
addresses. Before changing anything, every feature checks the original 1.0
bytes at each place it hooks; on any other exe those checks fail, the feature
stays off and the log says so. The game doesn't crash, but the mod does nothing
useful either.

**This mod needs no ASI loader.** It is a `d3d8.dll`, which the game loads by
itself. Ultimate ASI Loader is only needed for `.asi` plugins such as SilentPatch
or Modern VC Trainer (see below).

---

## Installation

1. Download `d3d8.dll` and `VCModernFix.ini` from the
   [Releases](../../releases) page (or [build them yourself](#building-from-source)).
2. Copy both files into the game folder, **next to `gta-vc.exe`**. Keep the name
   `d3d8.dll`: the game only loads it under that name, so it doesn't work as an
   `.asi` file or from the `scripts` folder.
   If the folder already has a `d3d8.dll` from another mod, see
   [Other d3d8.dll mods](#other-d3d8dll-mods) first.
3. **Optional, recommended:** let the game use 4 GB of memory instead of 2 GB.
   Put `SetLargeAddressAware.ps1` in a folder **inside** the game folder (for
   example `Grand Theft Auto Vice City\VCModernFix\`; it edits the `gta-vc.exe`
   one level up), close the game, and run:

   ```bash
   powershell -ExecutionPolicy Bypass -File SetLargeAddressAware.ps1
   ```

   It backs up the exe first as `gta-vc (Before LAA).exe`. The larger model
   memory and draw distance settings are tuned with this flag on.
4. Start the game. A `VCModernFix.log` file appears next to `gta-vc.exe` and
   lists every feature that was installed.

If `VCModernFix.ini` is missing, the mod writes a fresh one with the default
settings on first start.

### Adding SilentPatch and other ASI mods (optional)

`.asi` mods are loaded by [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader/releases/latest):

1. Download `Ultimate-ASI-Loader.zip` (the **32-bit** one, **not** `_x64`) and put
   its `dinput8.dll` next to `gta-vc.exe`. **Keep the name `dinput8.dll`.** Don't
   rename the loader to `d3d8.dll`: that name belongs to this mod.
2. Put `.asi` files next to `gta-vc.exe` or in a `scripts` folder inside the game
   folder; the loader looks in both.

[SilentPatch for Vice City](https://github.com/CookiePLMonster/SilentPatch)
comes as three files that all go next to `gta-vc.exe`: `SilentPatchVC.asi`,
`SilentPatchVC.ini` and `ddraw.dll` (its DDraw component, loaded by the game
itself, not by the ASI loader).

### A complete setup

This is a tested game folder with this mod, SilentPatch and
[Modern VC Trainer](https://github.com/Sahil12524/GTAVC-Classic-ModernTrainer):

| File (next to `gta-vc.exe`) | From | Loaded by |
| --- | --- | --- |
| `d3d8.dll`, `VCModernFix.ini` | **this mod** | the game (Direct3D 8) |
| `dinput8.dll` | Ultimate ASI Loader 10.0.2 (32-bit) | the game (DirectInput) |
| `SilentPatchVC.asi`, `SilentPatchVC.ini` | SilentPatch 1.1.12.1 | Ultimate ASI Loader |
| `ddraw.dll` | SilentPatch DDraw Component 1.1.7 | the game (DirectDraw) |
| `VCTrainer.asi`, `VCTrainer*.ini` | Modern VC Trainer (optional) | Ultimate ASI Loader |
| `Mss32.dll` | the game itself (Miles Sound System) | the game; don't touch |

Each `.dll` name is a different slot the game loads on its own, so **only one
file can have each name**. Two mods that both want `d3d8.dll` (or both want
`dinput8.dll`) can't simply sit side by side; see
[Other d3d8.dll mods](#other-d3d8dll-mods).

## Uninstalling

Delete `d3d8.dll`, `VCModernFix.ini` and `VCModernFix.log` from the game folder.
Leave `dinput8.dll` (the ASI loader) and SilentPatch's files in place if you use
them; they don't depend on this mod. To remove the 4 GB flag, run
`SetLargeAddressAware.ps1 -Undo` or restore `gta-vc (Before LAA).exe`.
Apart from that optional flag, the mod never changes `gta-vc.exe` or any game
file on disk; everything happens in memory while the game runs. Your saves are
untouched: quick saves and auto saves are ordinary game saves that the plain
game loads too.

---

## What it fixes

### Physics and timing broken above 30-60 FPS
The original game updates everything (physics, AI, scripts, the camera) once
per drawn frame. At high frame rates car handling, animations, cutscenes and
some missions break, so players have to cap the game at 30 FPS.

**Fix:** the game world runs at a **fixed 30 updates per second**, exactly like
the original with its frame limiter on, so physics, missions and timers behave
as designed. In between, extra frames are drawn as fast as your display allows,
and everything that moves is **smoothly blended** between the two most recent
updates: the camera, cars, bikes, boats, aircraft, peds (including their body
poses, bone by bone), objects, doors, wheels, particles, shadows, headlights and
glows. You get 144 Hz smoothness with 30 Hz game logic.

### Fullscreen locked to 60 Hz
The game asks for 60 Hz in fullscreen. **Fix:** it uses the highest refresh
rate your monitor supports at the game's resolution (or the one you set).

### The frame limiter burns a CPU core
The game's own limiter spins a CPU core at 100% while it waits. **Fix:** it is
replaced by a timer-based limiter that sleeps instead, capped at your refresh
rate by default.

### Mouse stops working until you Alt+Tab
Switching some menu settings re-creates the mouse device without releasing the
old one, which keeps the mouse to itself, so the game gets no mouse input until
the window loses focus. **Fix:** the old device is released first.

### Crash when quitting the game
"Unhandled exception c0000005" on exit, caused by the game freeing textures in
the wrong order during its final clean-up. **Fix:** that clean-up is guarded;
the crash is skipped and the game closes normally. Saves and settings are
written long before this point.

### Stutter and freezes from hard-disk reads
Speech, mission audio, radio and models are read from disk while you play; on a
hard disk each seek can freeze a frame. **Fix:** at start-up a low-priority
thread reads these files once (about 1.3 GB, 10-15 s on a hard disk, after the
game's own loading) and keeps them in memory. Not needed on an SSD, harmless
there.

### Pop-in: buildings and cars appearing late, in low detail
The game gives models only 45 MB of memory and keeps 25 vehicle models loaded,
so things are constantly dropped and reloaded. **Fix:** 512 MB and 50 vehicle
models by default.

### Peds disappearing when you look away
A ped that is off screen is deleted once they are 25 m away, so you look at
someone, turn the camera, and they are gone. **Fix:** off-screen peds are kept
up to 50 m away.

### Crash in the ped line-of-sight check
A script check of whether one ped can see another reads outside the world grid
when a ped is outside the map, which crashes the game. **Fix:** the grid
coordinates are clamped to the map.

### "Martha's Mug Shot" leaves the Ocean Drive hotel without walls
If that mission ends the wrong way, the building swap it makes stays in your
save for good. **Fix:** a swap left behind outside a mission is undone.

### Teleporting or flying into / out of interiors leaves the world undrawn
The game only changes the drawn area through door scripts, so teleporting out
of a room (trainers, mods) leaves the city invisible, and flying into a room
with a jetpack leaves the room invisible. **Fix:** the drawn area follows where
the player actually is.

### Getting shot stops your shooting
When the player is hit by a bullet, the game cancels their attack. **Fix:** you
still flinch, but keep firing.

---

## What it adds

### Quick save and quick load
- **F5** saves the game exactly as it is, where you are, without the 6 hours a
  safehouse save passes. **F9** loads it.
- Saved into save slot 8 (configurable), named **"Quick Save"** in the Load
  Game menu. If that slot holds one of your own saves, it is backed up first as
  `GTAVCsf8 (before quick save).b`, never overwritten.
- If you are in a car or on a bike, it comes back with you after F9: same model,
  colours and damage, with you in the driver's seat.
- Not allowed (a message tells you why) while wanted, on a mission or job, on
  water, dead, or during cutscenes, because such saves wouldn't load properly.

### Timed quick save
Every **5 minutes** of play (pause menus don't count) the game quick saves by
itself into the same slot, so F9 always loads the newest one. It only saves
when it is safe: no mission or job, no wanted level, not on water, not on a
phone call, not in a cutscene, in control of the player, alive and not in the
air. If it isn't safe it waits and saves as soon as it is. Any other save
restarts the count.

### Auto save after missions
A few seconds after you **pass a mission or finish a job** (taxi, vigilante,
firefighter, paramedic, rampages...) with money earned, the game is saved to
slot 7 as **"Auto Save"**. Not after being wasted or busted. Like a safehouse
save, loading it puts you at your nearest owned safehouse.

### Skip phone calls
During a phone call you answered, **Enter** skips the rest of it. Everything
the call sets up (new contacts, mission markers) still happens.

### Mouse look in vehicles
In cars, bikes, boats and aircraft the mouse turns the camera around the
vehicle and raises or lowers it, at the same speed as on foot. Drive with the
mouse still for 1.5 s and the camera swings back behind you; parked, it stays
where you put it. Off while the game's own mouse steering is on.

### Mouse aim for the Rhino tank and the fire truck
The tank turret and the fire truck's water cannon aim where the camera looks,
instead of only with the numpad keys.

### Run and gun
- **Walk, run and sprint while firing any gun**: pistols, shotguns, SMGs,
  rifles, the M60, flamethrower, minigun and rocket launcher. Your legs follow
  your movement while the upper body keeps each gun's own firing pose.
- Fire while sprinting; the rocket launcher and minigun no longer stop you from
  sprinting or jumping.
- The M4, Ruger and M60 aim in third person instead of forcing the
  first-person scope, so you can move while aiming.

### Busier streets
More peds (up to 62 instead of 15) and more traffic (up to 18 cars instead of 7)
by default, adjustable from half to triple.

### Longer draw distance
- Everything is drawn 3x as far as the menu's Draw Distance setting (which still
  works), including the full-detail limit that is normally fixed at 300 m.
- The far clip (where the world ends in the fog) is pushed 1.5x further.
- Anisotropic texture filtering x16: roads and walls seen at an angle stay sharp.

### Seamless interiors
- **No fade to black** when walking through interior doors.
- **See the city from inside** interiors that are built inside their real
  building: open doors and windows show the street instead of black.
- **See into interiors from the street**, with people inside.

---

## Controls

| Key | Action |
| --- | --- |
| **F5** | Quick save |
| **F9** | Quick load |
| **Enter** | Skip the rest of the phone call you are on |
| **Mouse** (in a vehicle) | Look around the vehicle; aim the Rhino turret / fire truck cannon |

The hotkeys only react when pressed **on their own**: with Shift, Ctrl, Alt or
a Windows key held they do nothing. So shortcuts for other programs, such as
NVIDIA ShadowPlay's **Alt+F9** (record) or **Alt+Enter**, don't quick load the
game or skip a call. All keys can be changed in `VCModernFix.ini`.

---

## Settings

All settings are in `VCModernFix.ini` next to `gta-vc.exe`, read when the game
starts. The file itself explains each one. `1` = on, `0` = off.

| Section | Setting | Default | What it does |
| --- | --- | --- | --- |
| `[FixedTick]` | `Enabled` | `1` | Fixed game updates with smooth frames in between. `0` = the original behaviour |
| | `TickRate` | `30` | Game updates per second (20-120). 30 matches the original; higher values change physics like the original at high FPS |
| `[FrameLimiter]` | `FPSLimit` | `0` | `0` = your refresh rate, `-1` = unlimited, or a number of FPS |
| `[Display]` | `RefreshRate` | `0` | Fullscreen refresh rate in Hz, `0` = the highest available |
| `[Compatibility]` | `CPUCore` | `-1` | Run the game on one CPU (as Task Manager's affinity), `-1` = all |
| | `D3D8Chain` | *(empty)* | Another `d3d8.dll` to pass graphics through to, see below |
| `[Streaming]` | `PreloadFiles` | `1` | Keep audio and model files in memory (no hard-disk freezes) |
| | `MemoryMB` | `512` | Memory for models and textures, 45-1536 MB. `0` = the game's 45 MB |
| | `VehicleModels` | `50` | Vehicle models kept loaded, 25-50. `0` = the game's 25 |
| `[DrawDistance]` | `Scale` | `3.0` | Times the menu's Draw Distance, 1-5 |
| | `FarClip` | `1.5` | Times the weather's far clip, 1-3 |
| `[Graphics]` | `Anisotropic` | `16` | Anisotropic filtering level 2-16, `0` = off |
| `[Camera]` | `FreeCam` | `1` | Mouse look in vehicles |
| | `FreeCamSensitivity` | `100` | Percent of the on-foot mouse speed |
| | `FreeCamReturnDelay` | `1500` | ms of still mouse while driving before the camera swings back, `-1` = never |
| | `FreeCamInvertY` | `0` | `1` = moving the mouse forward lowers the camera |
| `[QuickSave]` | `Enabled` | `1` | F5 quick save, F9 quick load |
| | `Slot` | `8` | Save slot 1-8 used for quick saves |
| | `SaveKey` | `116` | [Virtual-key code](https://learn.microsoft.com/windows/win32/inputdev/virtual-key-codes), 116 = F5 |
| | `LoadKey` | `120` | 120 = F9 |
| | `AutoMinutes` | `5` | Timed quick save every N minutes of play, `0` = off |
| `[AutoSave]` | `Enabled` | `1` | Save after passing a mission or finishing a job |
| | `Slot` | `7` | Save slot 1-8 used for auto saves |
| `[PhoneSkip]` | `Enabled` | `1` | A key skips the phone call you are on |
| | `Key` | `13` | Virtual-key code, 13 = Enter |
| `[Vehicles]` | `MouseAim` | `1` | Rhino turret and fire truck cannon aim with the camera |
| `[Weapons]` | `RunAndGun` | `1` | Move while firing any gun |
| | `KeepFiringWhenHit` | `1` | Getting shot doesn't stop your shooting |
| `[Population]` | `PedDensity` | `2.5` | 0.5-3, peds on the streets (up to 25 x this). `0` = the game's own |
| | `CarDensity` | `1.5` | 0.5-3, traffic cars (up to 12 x this). `0` = the game's own |
| | `KeepOffscreen` | `50` | Metres an off-screen ped is kept (25-50; game: 25) |
| `[Fixes]` | `MissionLeftovers` | `1` | Undo the hotel swap a mission left behind |
| `[Interiors]` | `AutoSwitch` | `1` | The drawn area follows the player (teleports, jetpack) |
| | `ShowOutside` | `1` | See the city from interiors and into interiors from the street |
| | `NoDoorFade` | `1` | No fade to black at interior doors |

The exit crash fix and the ped-sight crash fix are always on.

---

## Compatibility with other mods

### SilentPatch and ASI plugins
Fully compatible. This mod is `d3d8.dll` and Ultimate ASI Loader is
`dinput8.dll`, so they don't clash; see [A complete setup](#a-complete-setup).

The ASI loader loads its plugins while the game starts, long before the first
frame is drawn. This mod installs its game hooks at that first drawn frame,
after SilentPatch and other plugins have applied their patches. Where another
mod already hooked the same place, this mod calls through to it instead of
replacing it, and the log says so ("hooked by another mod; chaining to it").
SilentPatch was checked against its source: none of its patches touch a place
this mod hooks, and its frame limiter and mouse fix work together with this
mod's.

### Modern VC Trainer
[Modern VC Trainer](https://github.com/Sahil12524/GTAVC-Classic-ModernTrainer)
(`VCTrainer.asi`, loaded by Ultimate ASI Loader) works alongside this mod and
was tested with it, including its fly and noclip modes, which this mod's
smoothing and vehicle mouse look handle. Both draw through the same Direct3D 8
`Present` call and each chains to the other. Hotkeys don't collide: the trainer
uses INSERT / F7 and the hotkeys you bind; this mod uses F5, F9 and Enter (all
changeable). If you bind trainer hotkeys to plain F5, F9 or Enter, change one of
the two.

### Other d3d8.dll mods
Wrappers such as d3d8to9 or dgVoodoo are also called `d3d8.dll`. Rename the other
mod's file (for example to `d3d8_next.dll`), keep it next to `gta-vc.exe`, and set
`D3D8Chain=d3d8_next.dll` under `[Compatibility]`. Graphics then pass through it
instead of Windows' own `d3d8.dll`.

### Other gameplay mods
Mods that edit the same game code (other frame-rate fixes, other quick-save mods,
`gta3.ini` density edits) may conflict. Turn off the matching feature in
`VCModernFix.ini` if two mods do the same thing.

---

## The log file

Every start writes `VCModernFix.log` next to `gta-vc.exe`:

- which features were installed, and which turned themselves off and why;
- the refresh rate, frame limit and memory settings used;
- each quick save, auto save and timed quick save, and why one waited;
- other mods it found and chained to;
- if the game ever freezes for 3 seconds, a short report of where it got stuck.

Please attach it to any bug report.

---

## Troubleshooting

**Nothing changed / no log file appears.** The game didn't load `d3d8.dll`.
Check that it sits next to the `gta-vc.exe` you start (not in `scripts` or a
subfolder), and that it is really named `d3d8.dll`, not `d3d8.dll.dll` (turn on
file name extensions in Explorer to see).

**Another mod stopped working after installing this one.** If that mod also came
as a `d3d8.dll`, it was overwritten: put it back under another name and use
`D3D8Chain` (see [Other d3d8.dll mods](#other-d3d8dll-mods)). If your ASI loader
was named `d3d8.dll`, rename it to `dinput8.dll`.

**`.asi` mods (SilentPatch, the trainer) don't load.** That's the ASI loader, not
this mod: check that the 32-bit Ultimate ASI Loader `dinput8.dll` is next to
`gta-vc.exe`.

**The log says a feature "is not the v1.0 code".** Your `gta-vc.exe` isn't
version 1.0, or another mod changed that code first. Other versions aren't
supported.

**Quick save says "Can't quick save...".** You are wanted, on a mission or job,
on water, or in a cutscene. Those saves wouldn't load properly, so they are
blocked.

**F9 says "No quick save to load".** Nothing has been quick saved into the quick
save slot yet.

**Alt+F9 used to quick load my game.** Fixed: hotkeys ignore presses with Alt,
Ctrl, Shift or Win held.

**The game feels different to the original.** Set `[FixedTick] Enabled=0` to
compare. If it still feels different, please report it with the log.

**Low frame rate in busy areas.** Lower `[DrawDistance] Scale` and
`[Population] PedDensity` / `CarDensity`.

**Game runs out of memory.** Run `SetLargeAddressAware.ps1` (4 GB flag) or lower
`MemoryMB`.

---

## Known limitations

- **One tick behind:** drawing shows the world one game update (33 ms) behind,
  the price of showing real in-between positions instead of guesses.
- **Mouse look delay:** the game reads the mouse only on its 30 updates per
  second, so turning the camera on foot lags by about 33-66 ms. Smoothing this
  is planned.
- **Mirror reflections** on update frames use the real camera.
- **Not 64-bit:** the game is a compiled 32-bit program; making it 64-bit would
  need its source code. It runs fine as 32-bit on 64-bit Windows, and the 4 GB
  flag is the most memory a 32-bit program can use.
- **Not multithreaded:** the game does almost all its work on one thread.
- **Version 1.0 only**, see [Requirements](#requirements).

---

## Testing status

| Feature | Status |
| --- | --- |
| Fixed tick and smooth frames (camera, vehicles, peds, poses, parts, shadows, lights) | Tested in game |
| Refresh rate, frame limiter, mouse fix, exit crash fix | Tested in game |
| Quick save / load, auto save after missions, phone skip | Tested in game |
| Mouse look in vehicles, interiors, draw distance, streaming | Tested in game |
| Run and gun, keep firing when hit | Tested in game |
| Smooth game clock and particles (water, blinking lights, smoke) | Needs more testing |
| Timed quick save, hotkey modifier filter | New, needs testing |
| Busier streets, peds kept off screen, ped-sight crash fix, Rhino / fire truck mouse aim | New, needs testing |

Bug reports are welcome; please include `VCModernFix.log` and what you were doing.

---

## Building from source

Requirements: [Zig](https://ziglang.org/) (built with 0.17.0; its bundled clang
and MinGW are used as a C++ cross-compiler).

```bash
winget install zig.zig
```

Then run `build.bat` from the project folder. It produces a 32-bit `d3d8.dll`.
No Visual Studio or DirectX SDK is needed.

| Path | Contents |
| --- | --- |
| `src/` | All source code; one file per feature (see [docs/TECHNICAL.md](docs/TECHNICAL.md#source-files)) |
| `docs/TECHNICAL.md` | Detailed technical notes: every hook, address and design decision |
| `VCModernFix.ini` | Default settings |
| `SetLargeAddressAware.ps1` | Sets or clears the 4 GB flag on `gta-vc.exe` |
| `build.bat` | Build script |

---

## How it works (short version)

`gta-vc.exe` loads `d3d8.dll` from its own folder before the one in Windows.
This DLL forwards the game's single Direct3D 8 call to the real `d3d8.dll`,
hooks the Direct3D device (refresh rate, frame pacing, texture filtering), and
at the first drawn frame patches the game's code in memory. Nothing on disk is
modified (except by the optional 4 GB script, which backs up the exe first).

The game's main loop is split in two: the full update runs at the fixed tick
rate, and between ticks only the drawing half runs. Everything the drawing half
changes was found by a static scan of the exe (686 functions) and is saved and
restored around each extra frame, so the game logic never sees them. Positions,
poses and the game clock are blended between the last two ticks for those
frames.

Every place the mod changes is checked byte for byte against the original 1.0
code first. On any mismatch that feature stays off.

The full write-up, with addresses, is in [docs/TECHNICAL.md](docs/TECHNICAL.md).

---

## Credits

- [SilentPatch](https://github.com/CookiePLMonster/SilentPatch) by CookiePLMonster,
  used as a compatibility reference.
- [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader) by
  ThirteenAG.
- The re3 / reVC reverse-engineering community, for the game's structure
  layouts used as a reference.

Grand Theft Auto and Vice City are trademarks of Take-Two Interactive Software.
This project is not affiliated with or endorsed by Rockstar Games or Take-Two
Interactive.
