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
  streets**, a **longer draw distance**, **seamless interiors**, **widescreen**,
  **far city lights**, **GTA V style helicopter controls** and more.

Every feature can be switched off in `VCModernFix.ini`, and most from inside the
game under **Options > Modern Fix**. What changed in each release is in
[CHANGELOG.md](CHANGELOG.md).

> Fan-made and unofficial. Not affiliated with or endorsed by Rockstar Games or
> Take-Two Interactive. This repository contains no game files; you need your
> own copy of the game.

---

## Contents

- [Requirements](#requirements)
- [Installation](#installation)
  - [Adding SilentPatch and other ASI mods](#adding-silentpatch-and-other-asi-mods-optional)
  - [A complete setup](#a-complete-setup)
  - [Extra enhancement mods: VC Mod Manager](#extra-enhancement-mods-optional-vc-mod-manager)
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
- [Changelog](CHANGELOG.md)

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
   Put `SetLargeAddressAware.ps1` next to `gta-vc.exe` or in a folder inside the
   game folder (it finds `gta-vc.exe` in its own folder or one level up), close
   the game, and run:

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

### Extra enhancement mods (optional): VC Mod Manager

VC Modern Fix doesn't copy what other authors already make and keep up to date.
`VCModManager.bat` (with `VCModManager.ps1`, both from `tools\`, next to
`gta-vc.exe`) installs those mods for you from their **official GitHub
releases**, always the latest one:

| Mod | Author | What it does | Offered as |
| --- | --- | --- | --- |
| [d3d8to9](https://github.com/crosire/d3d8to9/releases/latest) | crosire | Direct3D 9 behind VC Modern Fix (`d3d8_next.dll`, `D3D8Chain`) | optional, needed by the two below |
| [DXVK](https://github.com/doitsujin/dxvk/releases/latest) | doitsujin | Direct3D 9 on Vulkan (`d3d9.dll`) | optional, not recommended on Windows (below) |
| [SkyGfx](https://github.com/aap/skygfx_vc/releases/latest) | aap | PS2 / Xbox look | optional |

**On Windows, d3d8to9 without DXVK is recommended.** Windows' own Direct3D 9
runs the game well, and SkyGfx only needs d3d8to9. With DXVK and an NVIDIA card a
crash was seen: DXVK's window handler and the driver's passed a key message back
and forth until the stack ran out. For SkyGfx's colours with a sharp picture,
set `trailsSwitch=2` in `skygfx.ini` (its PS2 trails blur and ripple the picture)
and keep **Trails** on in the game's Display menu.

After switching between Windows' Direct3D 8, d3d8to9 and DXVK, check the game's
**Screen Resolution**: the game saves it as a place in the list of screen modes,
and each of them lists the modes in another order.

The Widescreen Fix and Project2DFX were offered at first and are now **retired**:
- **Widescreen Fix:** its current release is a whole overhaul, with its own frame
  limits, menus, HUD, mouse and loading. With it, the game ran at 30 FPS and
  free drive-by didn't install.
- **Project2DFX:** its draw distance and limit adjuster overlap VC Modern Fix.
  Together they stacked the distances, resized the game's render lists (a
  freeze on loading a save) and led to a crash in bullet traces.

VC Modern Fix does widescreen itself ([Widescreen](#widescreen)). If one of the
two is installed, the manager still lists it so you can disable or remove it,
but no longer installs it.

Double-click `VCModManager.bat` with the game closed. The first time, it asks
whether you want extra mods at all, then about each one (Enter takes the
suggested answer). After that it shows a menu with each mod's state, the
installed release and the latest release. Pick a mod to:

- **Install** it: it is downloaded and installed, with anything it needs
  (d3d8to9 for DXVK and SkyGfx, an ASI loader if the folder has none).
- **Update** it: your edited `.ini` files are kept. **U** updates everything.
- **Disable** it: its files move to `VCModManager\disabled\<mod>`, so the game
  doesn't load them. Mods that need it are disabled too.
- **Enable** it again: the files move back, with no download.
- **Remove** it completely: its files and its download are deleted. Install
  it again any time: it's downloaded and set up again.

It never overwrites a file it didn't install: `d3d8.dll` (VC Modern Fix),
`dinput8.dll`, SilentPatch and the trainer are left alone. From the mods'
archives it leaves out what would break this setup: the Widescreen Fix's own
ASI loader (shipped as `d3d8.dll`) and its `scripts\global.ini` (it makes the
loader skip `.asi` files next to `gta-vc.exe`), SkyGfx's `d3d8.dll`, and DXVK's
`d3d8.dll` (d3d8to9 is used instead, so SkyGfx works too). It also sets a few options so the mods work with VC Modern Fix, on every
install, update and enable:
- **Widescreen Fix:**
  - `MenuFrameLimit = 0`, because VC Modern Fix paces the frames;
  - `NoIslandLoading = 0` and `NoInteriorLoading = 0`, because VC Modern Fix
    does the island and interior loading its own way.
- **Project2DFX:** its limit adjuster's log is turned on, to show which limits it
  leaves to VC Modern Fix.

VC Modern Fix adapts on its side as well:
- **Frame cap:** it keeps the game's own frame cap lifted every frame, even when
  another mod (the Widescreen Fix) sets it again.
- **Drive-by:** it chains its drive-by crosshair through the Widescreen Fix's HUD
  hook.
- **Draw distance:** with Project2DFX installed, it leaves draw, detail and view
  distance to Project2DFX (set them in `VCLodLights.cfg`), so the two don't stack.

Everything it does is written to `VCModManager\VCModManager.log`. Commands work too:
`VCModManager.bat status`, `check`, `install <mod>`, `update <mod|all>`,
`disable <mod>`, `enable <mod>`, `remove <mod>`.

Controller support isn't in the list: [GInput](https://cookieplmonster.github.io/mods/gta-vc)
(Silent) isn't published on GitHub, so install it by hand from its page.

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
game's own loading) and keeps them in memory. On a fast disk, or when Windows
already has the files cached (read in under half a second), they are left to the
Windows file cache instead of being kept mapped in the game, which saves about
640 MB of the game's memory; the log says which (`Preload: ...`).

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

### Big crowds turn the scene red, pink or orange
With more than 49 people close together (a trainer's bodyguard crowd), the game's
list of nearby people runs past its 50 places into the time cycle's colour tables:
the light on peds, cars and objects turns red, pink or orange and street lamps
change colour every update. **Fix:** the list has 512 places.

### Two stars for no reason in Ocean Beach
The game marks you as "inside the police station" when you go in through its door,
and only its exit door clears it. While it is set, being anywhere in Ocean Beach
without the cop uniform gives 2 stars. Leaving the station any other way (a
teleport, flying out) left it set, it was saved, and every load at the Ocean View
hotel brought the stars back. **Fix:** it is cleared as soon as you are not inside
the station.

### Riot mode stuck in a save
The riot cheat (FIGHTFIGHTFIGHT, or a trainer's riot / "everyone attacks" option)
makes peds fight each other by changing who hates whom, and the game keeps that in
the save: reloading brings the riot straight back, and nothing turns it off. Now
loading a game puts everyone (civilians, cops, medics, firemen, criminals,
prostitutes and the gangs) back to how they treat each other in a new game; only
the gangs' hostility toward you, which the story changes, is kept. Load the save
once and save again to fix the file. `[Fixes] ClearRiotOnLoad=0` keeps the riot.

### "Martha's Mug Shot" leaves the Ocean Drive hotel without walls
If that mission ends the wrong way, the building swap it makes stays in your
save for good. **Fix:** a swap left behind outside a mission is undone.

### Teleporting or flying into / out of interiors leaves the world undrawn
The game only changes the drawn area through door scripts, so teleporting out
of a room (trainers, mods) leaves the city invisible, and flying into a room
with a jetpack leaves the room invisible. **Fix:** the drawn area follows where
the player actually is.

### Crashes when flying far past the edge of the map
The game files every moving vehicle, ped and object in a grid of map squares, but
it never checked that something was still inside the grid. Fly or drive far out
over the sea and the game wrote into unrelated memory past the end of it, which
crashed sooner or later (at 0x4BB53E, 0x4B119B or 0x4D6EE9) or quietly damaged
other data.
Anything past the edge is now kept in the outermost squares, as the game already
does for buildings, and leftover empty entries are skipped. The "is the line
between these two points clear?" check (used by missions, the camera and peds)
had the same problem with lines that end off the map (a crash at the mall,
0x4D6FE7): squares off the map are now skipped as empty.

### Things with an invalid position
Now and then something in the game ends up with an invalid position or speed (a
broken number, "NaN"); seen after leaving the mall onto its roof. The camera then
followed it into nowhere and the game crashed (0x5C2D01). Before each update every
ped, vehicle and object is checked: one that went bad is put back where it last
was and stopped, the camera is put back behind it, and the log names it (kind and
model), so the cause can be found.

### Crash during vigilante and other jobs that chase street peds
Vigilante (and some missions) keep track of ordinary street peds. The game can
remove such a ped - for example when you fly away from it in the Hunter - and a
script asking where it is then crashed the game ("Unhandled exception" at
00449BAD). The missing ped now just counts as gone, and the job carries on.

### Getting shot stops your shooting
When the player is hit by a bullet, the game cancels their attack. **Fix:** you
still flinch, but keep firing.

### Crash when a ped has no animation
The game crashes ("Unhandled exception c0000005 at 00403ed2") if it poses a
character that has no animation playing at all, which a trainer's camera or
ped options can cause. **Fix:** that character keeps its last pose for the
moment instead.

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
- "On water" means water, not just a boat: swimming, standing or walking on the
  water (a trainer's walk-on-water), hovering or flying low over it, or a car on
  it. A pier, jetty or beach above the water counts as land. The timed quick save
  and the auto save after missions follow the same rule.

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
save, loading it puts you at your nearest owned safehouse, standing
next to its save point exactly where the game stands you after saving there
(not on the save icon, so it doesn't ask to save again).

### Skip phone calls
During a phone call you answered, **Enter** skips the rest of it. Everything
the call sets up (new contacts, mission markers) still happens.

### Instant mouse look on foot
The game reads the mouse only 30 times a second, so at 144 Hz turning the camera
on foot lagged 33-66 ms behind your hand. The mouse is now read every frame and
the camera turns right away, exactly as the game then turns it, so aiming feels
direct. `[Camera] FrameMouseLook=0` turns it off.

### Free camera on foot
The mouse camera goes all the way round Tommy, so you can see his face, and he
runs the way you press (W away from the camera, S towards it, A / D to the
sides) instead of always turning his back to the camera. Aiming a gun or a
thrown weapon (fire or the aim button held) turns him to where the camera looks,
so shots still go to the crosshair. As in San Andreas he always runs forward the
way he faces (no walking backwards or sideways), and turns quickly to the way
you press. Holding the on-foot look behind key (middle mouse button or Num1, or
what the Controls menu sets) turns only the camera to his other side: with his
back to the camera it goes in front of him, looking at his face; facing the
camera, it goes behind him. He isn't turned and keeps running the way he was;
letting go turns the camera back. `[Camera] FreeOrbitOnFoot=0` or **Free
camera on foot** in the Modern Fix menu turns it off (and gives the game's own
look behind back).
While he runs and you leave the mouse alone for a moment, the camera slowly
swings back behind him, as in San Andreas; moving the mouse stops it.
`[Camera] CameraSwingBehind=0` or **Camera swings behind** turns that off.

### Mouse look in vehicles
In cars, bikes, boats and aircraft the mouse turns the camera around the
vehicle and raises or lowers it, at the same speed as on foot. Drive with the
mouse still for 1.5 s and the camera swings back behind you; parked, it stays
where you put it. Off while the game's own mouse steering is on.
Hold **C** to put the camera straight behind the vehicle, looking forward
(`[Camera] CarLookForwardKey`, a virtual-key code, `0` = off).
It also works on **boats** (they now use the same follow camera as other
vehicles; `FreeCamBoats=0` gives them back the game's own boat camera) and on
the **remote-controlled vehicles** of the RC missions, such as the helicopter in
*Demolition Man*.

### Mouse aim for the Rhino tank and the fire truck
The tank turret and the fire truck's water cannon aim where the camera looks,
instead of only with the numpad keys.

### Steer your car in the air
Once a car leaves the ground (a jump, a ramp, a fall), you can turn it in the
air, as in newer GTA games, with the same controls as a helicopter:
forward / back tips the nose down / up and left / right rolls the car. Set the
keys in the game's controls menu, where the helicopter's are (by default Num8 /
Num5 for the nose; Num4 / Num6 or the steering keys for the roll). Use it to land on
your wheels or to do flips. Helicopters, planes, bikes and boats are unchanged.
`[Vehicles] AirControlStrength` sets how fast the car turns.

### Heavier helicopters
The game's helicopters snap into turns and change speed almost instantly. They
now lean, turn and speed up more gradually and carry their momentum, like a
heavy machine; their top speed and hovering are the game's. Crashes and landings
are unchanged. `[Vehicles] HeliWeight` sets how heavy (0 = the game's own). RC
helicopters keep their toy handling.

### GTA V style helicopter controls
In helicopters, steer left / right (A / D) turns the helicopter, turret left /
right (Num4 / Num6) rolls it, and pitch keeps its keys (Num8 / Num5). Like in
GTA V, pushing forward dips the nose to gain speed but never into a dive (30
degrees at most), and it leans at most 40 degrees to a side; it settles back
when it goes past. The keys are whatever the game's controls menu sets.
`[Vehicles] HeliControls`, `HeliMaxPitch` and `HeliMaxRoll` change or turn it off.

Planes (the Skimmer, the RC Baron) fly with the same keys: steer left / right turns
the plane, turret left / right (Num4 / Num6) rolls it, pitch as before, with no tilt
limits so it can still loop. `[Vehicles] PlaneControls=0` gives the game's own.

### Hunter weapons on the mouse wheel
In the Hunter, scroll the mouse wheel (or press **Tab** or **Num+**) to switch
between rockets and the cannon (the wheel doesn't change the radio there, and
Num+ doesn't start or stop the vehicle side mission there: **Caps Lock** still
does), and press **Space** to fire the one picked.
The **left mouse button** fires rockets and the **right** one the cannon, whichever
is picked. Each shot launches one rocket, the two pods taking turns (the game
launched both at once).
A message shows which weapon is selected when you get in and each time it
changes. The game's own fire keys still work. `[Vehicles] HunterWeapons` and
`HunterFireKey` change or turn it off.

### Free drive-bys
In any car, bike or boat you drive, press **Alt** once to take your gun out of the
window. Aim with the mouse: your driver leans out of the side the camera looks at
(on a bike, also straight ahead) and the shot goes under the game's own crosshair
in the middle of the screen. Hold **fire** to shoot, scroll the **mouse wheel** to
switch between your pistol, SMG and rifle (the radio doesn't change while the gun
is out), and press **Alt** again to put it away. Pistols, SMGs, the M4 and the
Ruger all work; the gun in your hand is the one you get in with (the game always
took the SMG), and pistols and rifles keep their own rate of fire. With the gun
away, Q / E drive-bys work as in the game. Vehicles with their own gun on the fire
button (Rhino, fire truck, Predator, Hunter, helicopters, planes) are left alone,
and missions that turn drive-bys off still do. `[Weapons] FreeDriveBy=0` turns it
off; `DriveByKey` changes the key.

### Run and gun
- **Walk, run and sprint while firing any gun**: pistols, shotguns, SMGs,
  rifles, the M60, flamethrower, minigun and rocket launcher. Your legs follow
  your movement while the upper body keeps each gun's own firing pose, turned
  the way the animators made it, so two-handed guns and the Python point where
  you aim, standing or moving.
- Fire while sprinting; the rocket launcher and minigun no longer stop you from
  sprinting or jumping.
- The M4, Ruger and M60 aim in third person instead of forcing the
  first-person scope, so you can move while aiming.

### Busier streets
More peds (up to 62 instead of 15) and more traffic (up to 18 cars instead of 7)
by default, adjustable from half to triple. Walking cops are kept to 3 while you
aren't wanted: past that, the game's street cops come as ordinary pedestrians, so
the streets around the police station are busy but not full of police
(`[Population] MaxStreetCops`).

### Widescreen
The game draws a 4:3 picture and stretches it to any screen, so on 16:9 or 21:9
everything looks too wide. **Now** the picture fits the screen's real shape
(`[Graphics] Widescreen=1`):
- **More to the sides:** you see the same height as the game shows at 4:3, plus
  more to the left and right (Hor+).
- **Distances unchanged:** the detail and draw distances stay as they were, and
  street-lamp glows stay round.
- **HUD in proportion:** the HUD, radar and kill-frenzy text keep their 4:3
  proportions. Items at the right edge stay at the right edge.

Menus are drawn as the game draws them. If another widescreen mod is loaded,
VC Modern Fix leaves widescreen to it.

With a wider field of view (a trainer's FOV slider), the camera's near clip plane
is brought in to match, so the player's legs are no longer cut off when the camera
is low and close.

### Nothing vanishes at the screen's edges
The game decided what to draw from a triangle on the map: the camera and two
corners of the view. Banking or pitching a helicopter or plane pushed the other two
corners outside it, so buildings, roads, bridges and objects at the edges of the
screen disappeared at some angles. Now the whole view is covered at any angle, for
drawing and for loading what is in view.

### Light coronas from far away
At night the whole city glows from far away: every street lamp, lit window and neon
sign of the buildings and lamp posts, up to 2.5 km, drawn as steady points of light
(no blinking, no limit on how many). Buildings in front hide them. They come in
where the game's own glow fades out, so up close the lights look exactly as in the
game. Car lights and the like are seen further (`[Graphics] CoronaDistance`, 5 times
the game's by default), dimming gradually with distance. The game can show 120
coronas at once instead of 56 (`[Graphics] FarLights=0` turns all of this off).

### Clouds at any height
The game put its clouds at fixed heights (about 40 - 160 m), so from a helicopter
or a high rooftop they sank below you and the sky above was empty. Now the cloud
layers rise with you above 30 m: the sky looks the same from any height, and on
the ground nothing changes.

### Seeing the city from high up
From a helicopter or plane high above the city, the ground used to vanish into grey
fog and nothing. Now above 100 m the view reaches further the higher you are (up to
4 km), and the fog moves out with it (`[DrawDistance] HighView=0` for the game's own).

### Longer draw distance
- Everything is drawn 3x as far as the menu's Draw Distance setting (which still
  works), including the full-detail limit that is normally fixed at 300 m.
- The far clip (where the world ends in the fog) is pushed 1.5x further.
- Anisotropic texture filtering x16: roads and walls seen at an angle stay sharp.
- Palm and tree leaves drawn in two passes, so no leaves go missing in pale boxes of sky inside the crowns.

### Skip scripted mission scenes
Many missions (the RC missions such as *Demolition Man*, and other mission
intros) don't play a real cutscene, which Enter already skips. They stage a
scene with the mission script instead: black bars, script cameras, dialogue and
timed waits, none of which can be skipped. Now **hold Enter for 3 seconds**
during such a scene to fast-forward it: every timed wait ends at once and the
dialogue is skipped. The script still runs every step in order, so everything
the scene sets up (vehicles, people, objectives) is still there when you get
control back.

### Missions that get stuck
Sometimes a mission breaks and leaves you stuck: no control, a camera that never
comes back, a scene that never ends. Before, the only way out was to reload a
save.
- **Automatic:** if a mission leaves you without control for **90 seconds**
  (outside real cutscenes and phone calls), a message warns you. If nothing
  changes within 15 more seconds, the mission is ended.
- **By hand:** **hold End for 3 seconds** on a mission to end it right away.
  This covers missions stuck while you still have control (a target that never
  appears, a trigger that never fires).

The mission is ended the way the game ends one when you are wasted or busted, but
without the death or the hospital: the mission's own "mission failed" and
clean-up code runs, and anything the stuck scene left behind (player control,
black bars, the camera, a black screen, slow motion) is put back. Then you are
offered a restart (below).

### Restart a failed mission
When a mission fails, once you're back in control (out of the hospital or police
station too) the help box asks: **"Mission failed. Restart it? Press Y for yes,
N for no."** A mission counts as failed only when it says so: it showed
**MISSION FAILED** (every story mission does, on being wasted or busted too), or
you ended it with End, or it was ended because it was stuck. A mission that ends
any other way (a scene-only mission like *An Old Friend*, even with its scene
skipped) asks nothing.
- **Y** puts you back exactly where the mission started: in its pink marker,
  facing the same way, or for missions that start in a vehicle (the RC missions
  start in the Top Fun van) in a vehicle of that model on the same spot. The
  mission then starts again the normal way, cutscene and all.
- **N**, or no answer within 30 seconds, leaves you where you are.

The side jobs (taxi, ambulance, fire truck, vigilante, Pizza Boy) start from a
key press, not a place, so they don't ask, and neither does buying a property
(its script ends without MISSION PASSED).

**Rampages** work the same way: holding **End** for 3 seconds during a rampage
fails it (the timer stops and the game shows RAMPAGE FAILED), and a failed
rampage - time up, wasted, busted or ended with End - asks **"Rampage failed.
Restart it?"**; **Y** puts you back on the skull pickup where it started.

### Pizza Boy with the mouse
Pizza Boy throws a pizza when you hold look left or look right and press fire,
a drive-by. Now **hold the right mouse button and click the left one**: the pizza
goes to the nearest customer still waiting, on whichever side they are, aimed so
it lands within the 3 m the job counts as delivered. The job's own throw, pizza
count and pay are used. With no waiting customer within 30 m a click throws
nothing, so no pizza is wasted. Throws with the game's own keys are aimed the
same way. Release the left button before the next throw.
(`[Missions] PizzaMouseThrow`, or Options > Modern Fix > Missions.)

### Unique stunt jumps without the slow motion
Hitting a unique stunt jump normally drops the game into slow motion and cuts
to a fixed cinematic camera until you land. Now the jump plays at normal speed
and the camera keeps following your vehicle as usual. The jump still counts:
landing it pays the **"UNIQUE STUNT BONUS!"** reward exactly as before.

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
| **Enter** (hold 3 s) | Fast-forward a mission's scripted scene |
| **End** (hold 3 s) | End the current mission (if it got stuck) or rampage |
| **Y** / **N** | Restart a failed mission at its marker: yes / no |
| **Right + left mouse button** (Pizza Boy) | Throw a pizza at the nearest waiting customer |
| **Num8 / Num5** (bike) | Lean forward / back (wheelie) |
| **Mouse** (in a vehicle) | Look around the vehicle; aim the Rhino turret / fire truck cannon |
| **C** (hold, in a vehicle) | Camera behind the vehicle, looking forward |
| **Middle mouse button** (hold, on foot) | Camera to the player's other side (face or back); the player isn't turned |
| **Helicopter controls** (a car in the air) | Forward / back: nose down / up; left / right: roll |
| **A / D** (helicopter) | Turn left / right |
| **Num4 / Num6** (helicopter) | Roll left / right |
| **Num8 / Num5** (helicopter) | Pitch nose down / up |
| **Q** / **E** (vehicle) | Radio station / horn |
| **Caps Lock** (vehicle) | Start the vehicle mission (taxi, vigilante...) |
| **Shift** (on foot) | Sprint |
| **Mouse wheel**, **Tab** or **Num+** (Hunter) | Switch rockets / cannon |
| **Space** (Hunter) | Fire the selected weapon |
| **Left / right mouse button** (Hunter) | Fire rockets / cannon |
| **Alt** (driving, pressed on its own) | Take the gun out / put it away (free drive-by) |
| **Mouse / fire / mouse wheel** (gun out) | Aim / shoot / switch gun |

The hotkeys only react when pressed **on their own**: with Shift, Ctrl, Alt or
a Windows key held they do nothing. So shortcuts for other programs, such as
NVIDIA ShadowPlay's **Alt+F9** (record) or **Alt+Enter**, don't quick load the
game or skip a call. All keys can be changed in `VCModernFix.ini`.

**Modern keys** (`[Controls] ModernKeys`, on by default) set the game's own key
bindings to the layout above: W A S D, Shift sprint, Q radio, E horn, Caps Lock
vehicle missions, Num4 / Num6 / Num8 / Num5 for the turret and helicopter roll
and pitch. They are put back each time the game's menu closes, so a reset or a
change in the controls menu doesn't undo them; to bind keys yourself, turn
**Modern keys** off under Options > Modern Fix > Vehicles (that also brings back
the bindings you had). No mouse button is the handbrake (Space is), and left
Ctrl doesn't fire (the mouse and Num0 do), so it stays free for other programs;
Delete no longer aims or zooms a scoped weapon (the right mouse button does);
bikes lean only with Num8 / Num5, so braking with S doesn't pull a wheelie and
holding W doesn't cancel Num5;
vehicle look left / right stay on Num1 / Num2.

---

## Settings

All settings are in `VCModernFix.ini` next to `gta-vc.exe`, read when the game
starts. The file itself explains each one. `1` = on, `0` = off.

### The Modern Fix menu

Most fixes can also be turned on and off from inside the game, without
restarting: **Options > Modern Fix** (in the pause menu and the start menu). Pick
a group, then press Enter, click, or press left / right on a line to switch it
between ON and OFF. It takes effect right away and is saved to
`VCModernFix.ini`.

| Group | Switches |
| --- | --- |
| Graphics | Two-pass leaves, anisotropic filtering, longer draw distance (with the far clip), faster model loading, no island loading screen, far view when high up, no vanishing edges, far city lights, longer corona distance, clouds rise when high up, wide FOV near clip |
| Camera and mouse | Vehicle mouse look, mouse look on boats, vehicle camera returns, invert vehicle mouse Y, mouse read every frame, free camera on foot, camera swings behind, equal mouse axes, mouse smoothing, car look forward key |
| Vehicles | Turret mouse aim, car air control, helicopter controls, helicopter nose limit, helicopter lean limit, plane controls, heavier helicopters, Hunter weapon keys |
| Weapons | Run and gun, keep firing when hit, free drive-by |
| Missions | Skip mission scenes, stuck mission rescue, End mission key, restart failed missions, pizza throw with mouse, normal stunt jumps, skip phone calls |
| Saving | Quick save keys, timed quick save, auto save |
| World and fixes | Interior auto switch, rooms seen from street, no door fade, more people, more traffic, keep people in view, fewer street cops, mission leftovers fix, clear riot on load, police station 2-star fix |

Far city lights can be switched while playing, but its 120 corona slots are set
when the game starts.

For a setting that is a number (draw distance, density, minutes...), OFF sets
the game's own value and ON puts back the number you had (or the default). The
numbers themselves, the keys and the start-up settings (fixed tick, frame
limiter, memory, refresh rate) are changed in the ini only.

Equal mouse axes and mouse smoothing work only with "Mouse read every frame" on.
Turning run and gun off brings back the sprint, scope and heavy-gun limits at
once; the firing animations already loaded keep free legs until the game loads
them again.

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
| | `HighView` | `1` | Above 100 m the view reaches further with the height (up to 4 km), fog moved out; `0` = the game's |
| | `FullViewScan` | `1` | Edges of the view don't vanish while a helicopter or plane banks; `0` = the game's |
| `[Graphics]` | `Anisotropic` | `16` | Anisotropic filtering level 2-16, `0` = off |
| `[Graphics]` | `TwoPassAlpha` | `1` | Leaves drawn solid parts first, soft edges after; `0` = game's own |
| | `Widescreen` | `1` | Picture and HUD fitted to the screen's shape (Hor+); `0` = the game's stretched 4:3 |
| | `CoronaDistance` | `5.0` | Times the distance car and other light coronas are seen from (1-10); `1` = the game's |
| | `FarLights` | `1` | The city's lights glow at night up to 2.5 km, 120 corona slots; `0` = the game's |
| | `RisingClouds` | `1` | The clouds rise with a high camera; `0` = the game's (they sink below it) |
| | `WideFovNearClip` | `1` | With a FOV wider than the game's, the near clip comes in (no cut-off legs); `0` = the game's |
| `[Camera]` | `FreeCam` | `1` | Mouse look in vehicles |
| | `FreeCamSensitivity` | `100` | Percent of the on-foot mouse speed |
| | `FreeCamReturnDelay` | `1500` | ms of still mouse while driving before the camera swings back, `-1` = never |
| | `FreeCamInvertY` | `0` | `1` = moving the mouse forward lowers the camera |
| | `FreeCamBoats` | `1` | Boats use the vehicle camera, with mouse look. `0` = the game's boat camera |
| | `CarLookForwardKey` | `67` | Held in a vehicle: camera behind it, looking forward. Virtual-key code (67 = C), `0` = off |
| | `FrameMouseLook` | `1` | On foot, the mouse turns the camera every frame (no 30 Hz delay) |
| `[QuickSave]` | `Enabled` | `1` | F5 quick save, F9 quick load |
| | `Slot` | `8` | Save slot 1-8 used for quick saves |
| | `SaveKey` | `116` | [Virtual-key code](https://learn.microsoft.com/windows/win32/inputdev/virtual-key-codes), 116 = F5 |
| | `LoadKey` | `120` | 120 = F9 |
| | `AutoMinutes` | `5` | Timed quick save every N minutes of play, `0` = off |
| `[AutoSave]` | `Enabled` | `1` | Save after passing a mission or finishing a job |
| | `Slot` | `7` | Save slot 1-8 used for auto saves |
| `[PhoneSkip]` | `Enabled` | `1` | A key skips the phone call you are on |
| | `Key` | `13` | Virtual-key code, 13 = Enter |
| `[Streaming]` | `NoIslandScreen` | `1` | No "Greetings from Vice City" loading screen when crossing between islands |
| `[Streaming]` | `FastLoadMs` | `4` | Extra model / texture loading per game update in ms (0 = the game's rate) |
| `[Vehicles]` | `MouseAim` | `1` | Rhino turret and fire truck cannon aim with the camera |
| `[Vehicles]` | `AirControl` | `1` | Steer a car's nose and roll in the air with the helicopter controls |
| `[Vehicles]` | `AirControlStrength` | `100` | How fast the car turns in the air, in percent (25-300) |
| `[Vehicles]` | `HunterWeapons` | `1` | Hunter: mouse wheel picks rockets / cannon, fire key fires it |
| `[Vehicles]` | `HunterFireKey` | `32` | Hunter fire key (Space) |
| `[Vehicles]` | `HeliControls` | `1` | Helicopter: steer left / right turns, turret left / right rolls, tilt limits |
| | `PlaneControls` | `1` | Planes (Skimmer, RC Baron): steer left / right turns, turret left / right rolls |
| `[Vehicles]` | `HeliMaxPitch` | `30` | Furthest the helicopter's nose tips, in degrees (0 = no limit) |
| `[Vehicles]` | `HeliMaxRoll` | `40` | Furthest the helicopter leans to a side, in degrees (0 = no limit) |
| `[Vehicles]` | `HeliWeight` | `100` | Extra helicopter weight in percent, 0 = the game's handling (max 300) |
| `[Weapons]` | `RunAndGun` | `1` | Move while firing any gun |
| | `KeepFiringWhenHit` | `1` | Getting shot doesn't stop your shooting |
| | `FreeDriveBy` | `1` | Drive-bys from any car, bike or boat, aimed with the mouse |
| | `DriveByKey` | `18` | Key that takes the gun out / puts it away (18 = Alt) |
| `[Population]` | `PedDensity` | `2.5` | 0.5-3, peds on the streets (up to 25 x this). `0` = the game's own |
| | `CarDensity` | `1.5` | 0.5-3, traffic cars (up to 12 x this). `0` = the game's own |
| | `KeepOffscreen` | `50` | Metres an off-screen ped is kept (25-50; game: 25) |
| | `MaxStreetCops` | `3` | Walking cops about while you aren't wanted, at most (wanted: no limit); `-1` = no limit |
| `[Fixes]` | `MissionLeftovers` | `1` | Undo the hotel swap a mission left behind |
| | `ClearRiotOnLoad` | `1` | A riot cheat saved in a game is undone when it loads |
| | `PoliceStationFlag` | `1` | A stale "inside the police station" flag (2 stars on load) is cleared outside it |
| `[StuntJumps]` | `NormalCamera` | `1` | Unique stunt jumps at normal speed with the normal chase camera |
| `[Missions]` | `IntroSkip` | `1` | Hold the key 3 s to fast-forward a scripted mission scene |
| | `IntroSkipKey` | `13` | Virtual-key code, 13 = Enter |
| | `SoftlockSeconds` | `90` | No control this long on a mission = stuck (30-600), `0` = off |
| | `EndMissionKey` | `35` | Hold 3 s to end the current mission, 35 = End, `0` = off |
| | `RestartPrompt` | `1` | Offer to restart a mission that showed MISSION FAILED (or was ended) at its marker |
| | `PizzaMouseThrow` | `1` | Pizza Boy: hold the right mouse button, the left one throws; every throw goes at the customer |
| | `RestartYesKey` | `89` | Virtual-key code, 89 = Y |
| | `RestartNoKey` | `78` | Virtual-key code, 78 = N |
| `[Interiors]` | `AutoSwitch` | `1` | The drawn area follows the player (teleports, jetpack) |
| | `ShowOutside` | `1` | From the street, rooms built into city buildings show through open doorways instead of black (windows stay solid) (and the street from inside) |
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

Every start writes `VCModernFix.log` next to `gta-vc.exe`; the one from the
session before is kept as `VCModernFix.previous.log`, so a crash's log survives
restarting the game. It holds:

- which features were installed, and which turned themselves off and why;
- the refresh rate, frame limit and memory settings used;
- each quick save, auto save and timed quick save, and why one waited;
- other mods it found and chained to;
- every 10 seconds the frame rate and frame times (`Frames:`), and once a minute
  the game update's slowest steps (`UpdateProfile:`), the mod's own time per update
  and per frame (`Timing:`) and the memory used (`Memory:`);
- if the game ever freezes for 3 seconds, a short report of where it got stuck;
- if the game crashes, where (`EXCEPTION ... at ...` lines), which the game's own
  "Unhandled exception" box doesn't keep. The first crash of a session also
  writes a crash dump, `VCModernFix-crash-<date>-<time>.dmp` (the newest 3 are
  kept): attach it too.

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

**FPS stuck at the refresh rate with d3d8to9 / DXVK.** Set `[FrameLimiter]
FPSLimit=-1` for unlimited: VC Modern Fix then asks Direct3D for no vsync.

**Low resolution or blurry after installing d3d8to9 / DXVK.** Pick your screen's
resolution again under Options > Display Setup (see
[VC Mod Manager](#extra-enhancement-mods-optional-vc-mod-manager)).

**The picture looks soft or ripples like heat haze with SkyGfx.** That's SkyGfx's
PS2 trails; set `trailsSwitch=2` in `skygfx.ini`, or turn Trails off in the
game's Display menu.

---

## Known limitations

- **One tick behind:** drawing shows the world one game update (33 ms) behind,
  the price of showing real in-between positions instead of guesses.
- **Mouse look delay:** the game reads the mouse only on its 30 updates per
  second, so turning the camera on foot lags by about 33-66 ms. Smoothing this
  is planned.
- **Mirror reflections** on update frames use the real camera.
- **3D markers** (the big pink arrow over a mission target, rings and cylinders)
  are blended between updates like cars and people, so they spin, bob and follow
  their target at the full frame rate.
- **F1 replays** get the smooth extra frames like normal play (the cars, people
  and camera blended between updates), but the mod's helpers (quick save, free
  cam, mission helpers, interiors) pause until the replay ends: the replay
  rebuilds the world from its recording every update, which those features must
  not touch.
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
| Timed quick save | In use (the log shows timed quick saves made and waiting while in the air) |
| Hotkey modifier filter | New, needs testing |
| Busier streets | In use (the log's census: 30 to 120+ people, ped density 2.5) |
| Peds kept off screen, ped-sight crash fix, Rhino / fire truck mouse aim | New, needs testing |
| Interior switch in the city (flying or teleporting in and out), Ocean View lobby walls, full ped pool guards, crowd colour fix | In use (the log shows the switches); the latest changes need testing |
| The mod's own work made cheaper (`Timing:` line, log file, preload memory) | New, needs testing |
| Unique stunt jumps without slow motion | New, needs testing |
| Scripted scene skip | Tested in game |
| Stuck-mission detection, End key | New, needs testing |
| Restart prompt for failed missions | Tested in game |
| Steering a car in the air | Tested in game |
| Heavier helicopters | New, needs testing |
| Faster model / texture loading | New, needs testing |
| No island loading screen | New, needs testing |
| GTA V style helicopter controls | New, needs testing |
| Hunter weapons on the mouse wheel / Space | New, needs testing |
| Free drive-bys | New, needs testing |
| Instant mouse look on foot | In use (the log's camera check: every on-foot frame turned by the mouse) |
| Riot cleared on load | New, needs testing |
| Modern Fix menu (53 switches) | Tested in game |
| Widescreen, near clip at a wide FOV | Tested in game |
| Far city lights, corona distance, high view, no vanishing edges, rising clouds | In use (the log's far-light and view-scan counts); needs more testing |
| Police station two-star fix | Tested in game (the log shows the flag cleared on load) |
| Fewer street cops | In use; the crash it caused while driving is fixed, needs testing |
| Restart prompt only after MISSION FAILED | New, needs testing |
| Pizza Boy mouse throw at the nearest waiting customer | New, needs testing |
| Delete off aim / zoom, bikes lean with Num8 / Num5 only | New, needs testing |
| Smooth 3D markers and F1 replays | New, needs testing |
| d3d8to9 + SkyGfx: uncapped FPS, resolution change, menus / radar drawn whole | Tested in game |

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
| `VCModernFix.ini` | Default settings (the same file the DLL writes when none exists) |
| `CHANGELOG.md` | What changed in each release |
| `tools/VCModManager.bat`, `tools/VCModManager.ps1` | VC Mod Manager (optional extra mods) |
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
