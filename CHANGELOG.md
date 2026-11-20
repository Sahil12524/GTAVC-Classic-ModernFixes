# Changelog

All notable changes to VC Modern Fix (GTAVC Classic ModernFixes). Every feature
can be switched off in `VCModernFix.ini`; most also in the game's
**Options > Modern Fix** menu. Settings named below are `[Section] Key`.

## Unreleased

### Fixed

- **Free drive-by stopped shooting** (`[Weapons] FreeDriveBy`): after one clip
  the crosshair stayed but nothing fired until Alt was pressed twice. The game
  refills a drive-by clip only when the arm goes back inside the vehicle, and in
  Alt mode it stays out. The clip now reloads with the gun out, after the gun's
  own reload time. A shot timer left far ahead by loading a save no longer
  blocks firing.
- **Fire truck, ambulance and vigilante jobs had no targets on the mainland
  before the bridges open**: the job scripts only looked for burning cars,
  patients and criminals on the beach islands until "Phnom Penh '86", so started
  on the mainland (reached with a trainer, a mod or a save) they found nothing
  150–600 m away: no target, no blip. On the mainland they now pick targets
  around the player, as after the bridges open. On the beach islands, and in
  every other script, the game's own check stands.
- **Fire truck water jet at 30 fps**: the jet's points move once per game tick;
  they are now drawn between their last two places on every frame, and the jet's
  start follows the blended truck. The game's own water data (putting out fires,
  pushing people) is unchanged.

## 2026-11-20

Everything since the first public commit (2026-11-13).

### Added: the Modern Fix menu

- **Options > Modern Fix**, in the pause menu and the start menu: 53 switches in
  7 groups (Graphics, Camera and mouse, Vehicles, Weapons, Missions, Saving,
  World and fixes). A switch takes effect at once and is saved to
  `VCModernFix.ini`. For a number setting, OFF sets the game's own value and ON
  puts back the number you had.
- Every feature added in this release that works while playing has a switch,
  including far view when high up, no vanishing edges, far city lights, corona
  distance, rising clouds, wide FOV near clip, car look forward key, vehicle
  camera return, inverted vehicle mouse Y, helicopter nose / lean limits,
  fewer street cops, the police station fix and the Pizza Boy mouse throw.

### Added: graphics

- **Widescreen** (`[Graphics] Widescreen`): the picture fits the screen's real
  shape (16:9, 21:9...) instead of a stretched 4:3; HUD, radar, kill frenzy text
  and the sniper scope keep their 4:3 proportions.
- **Two-pass leaves** (`[Graphics] TwoPassAlpha`): palms, trees and fences drawn
  solid parts first, soft edges after; no more leaves missing in boxes of sky.
  Applied to the 3D world only (menus, HUD and radar are drawn whole).
- **Anisotropic filtering x16** (`[Graphics] Anisotropic`), off for the water so
  its waves look as in the game.
- **Light coronas from far away** (`[Graphics] CoronaDistance`, default 5x):
  car, pickup and script lights are seen further, as in the game up close and
  dimmer and smaller further away.
- **Far city lights** (`[Graphics] FarLights`): at night the lights of buildings,
  lamp posts and treadables too far away for the game to draw still glow (up to
  2.5 km), steady, without flicker; 120 corona slots instead of 56.
- **Seeing the city from high up** (`[DrawDistance] HighView`): above 100 m the
  view reaches further with the height (up to 4 km) and the fog moves out with it.
- **Nothing vanishes at the screen's edges** (`[DrawDistance] FullViewScan`):
  roads, buildings and objects at the edges no longer disappear while a
  helicopter or plane banks or pitches (the game's world scan only covered part
  of a tilted view).
- **Clouds at any height** (`[Graphics] RisingClouds`): the cloud layers rise
  with a high camera instead of sinking below it.
- **Wide FOV near clip** (`[Graphics] WideFovNearClip`): with a field of view
  wider than the game's (a trainer's FOV slider at 120), the player's legs are no
  longer cut off.
- **Smooth 3D markers:** the big pink arrow over a mission target, rings and
  cylinders spin, bob and follow their target at the full frame rate (they moved
  30 times a second).
- **Smooth F1 replays:** replays get the smooth extra frames between game updates
  too (they ran at 30 FPS).

### Added: camera and mouse

- **Free camera on foot** (`[Camera] FreeOrbitOnFoot`, `CameraSwingBehind`): the
  camera goes all the way round the player, who runs the way you press; it
  slowly swings behind while running.
- **Instant mouse look on foot** (`[Camera] FrameMouseLook`, `MouseEqualAxes`,
  `MouseSmoothing`, `MouseSensitivity`): the mouse turns the camera every frame
  instead of 30 times a second.
- **Mouse look on boats** (`[Camera] FreeCamBoats`).
- **Look forward in a vehicle** (`[Camera] CarLookForwardKey`, C): hold to put the
  camera straight behind the vehicle.
- **Look behind on foot** with the middle mouse button: the camera goes to the
  player's other side; the player isn't turned.

### Added: vehicles

- **Steer a car in the air** (`[Vehicles] AirControl`, `AirControlStrength`).
- **Heavier helicopters** (`[Vehicles] HeliWeight`).
- **GTA V style helicopter controls** (`[Vehicles] HeliControls`, `HeliMaxPitch`,
  `HeliMaxRoll`) and **plane controls** (`PlaneControls`).
- **Hunter weapons** (`[Vehicles] HunterWeapons`, `HunterFireKey`): mouse wheel,
  Tab or Num+ picks rockets / cannon, Space fires it.
- **Free drive-bys** (`[Weapons] FreeDriveBy`, `DriveByKey`): Alt takes the gun out
  in any car, bike or boat; aim with the mouse.

### Added: controls

- **Modern keys** (`[Controls] ModernKeys`): W A S D, Shift sprint, Q radio,
  E horn, Caps Lock vehicle missions, Num4 / 6 / 8 / 5 turret and helicopter
  roll / pitch, no mouse handbrake, left Ctrl free for other programs. Put back
  whenever the game's menu closes.
  - **Delete no longer aims or zooms** a scoped weapon; the right mouse button
    aims.
  - **Bikes lean with Num8 / Num5 only.** W / S also leaned them, so braking
    pulled a wheelie and holding W cancelled Num5.

### Added: missions

- **Skip scripted mission scenes** (`[Missions] IntroSkip`): hold Enter 3 s.
- **Stuck missions** (`[Missions] SoftlockSeconds`, `EndMissionKey`): a mission
  with no control for 90 s is ended; hold End 3 s to end one (or a rampage).
- **Restart a failed mission** (`[Missions] RestartPrompt`): Y puts you back in its
  marker (or the start vehicle, or the rampage pickup).
- **Unique stunt jumps without slow motion** (`[StuntJumps] NormalCamera`).
- **Pizza Boy with the mouse** (`[Missions] PizzaMouseThrow`): hold the right
  mouse button, the left one throws. Every throw (the game's keys too) goes at
  the nearest customer still waiting; with nobody in reach (30 m) a click throws
  nothing, so no pizza is wasted.

### Added: streaming, saving, world

- **Faster model / texture loading** (`[Streaming] FastLoadMs`).
- **No island loading screen** (`[Streaming] NoIslandScreen`).
- **Busier streets**: a 210-slot ped pool and guards for a full pool.
- **Fewer street cops** (`[Population] MaxStreetCops`, default 3): at most this
  many walking cops while you aren't wanted; extra ones become civilians.
- **Interiors** keep people in rooms seen from the street; a room's own walls are
  drawn before the see-through things in it.
- **VC Mod Manager** (`tools\VCModManager.bat`): installs, updates, disables,
  enables and removes d3d8to9, DXVK and SkyGfx from their official releases,
  without overwriting files it didn't install.
- **`SetLargeAddressAware.ps1`** works from the game folder itself or from a
  folder inside it.

### Fixed: game bugs

- **Two stars for no reason in Ocean Beach** (`[Fixes] PoliceStationFlag`): a
  saved "inside the police station" flag is cleared while you're outside it.
- **Riot mode stuck in a save** (`[Fixes] ClearRiotOnLoad`).
- **Big crowds turned the scene and coronas red, pink or orange** (the game's
  50-place nearby-ped list ran over the time cycle).
- **Crashes far past the map's edge** (sector numbers, water level lookup,
  line-of-sight tests, collision checks).
- **Things with an invalid position** (NaN / huge values) are put back.
- **Crash during vigilante and other jobs** chasing street peds that were
  removed; 143 script commands on a character that doesn't exist are skipped.
- **Crash when a ped has no animation**, bullet traces with no lifetime, a full
  ped pool (0x4496E7, 0x53B653), a ped's delayed reaction to a removed ped.
- **Car AI freezes** in angle loops on bad values (0x42335C) and line-of-sight
  tests with far ends (0x4DAF45).
- **Vigilante soft lock after level 10**: the criminal's car never got collision
  (the cleanup list filled up); fixed.
- **Too many police near the police station** with a higher ped density.

### Fixed: VC Modern Fix itself

- **Crash while driving** (d3d8.dll+0x1FCAF): the street-cop check read the
  camera's target as the player, which is the car while driving. Every use now
  reads the real player.
- **Mission restart offered after a mission that didn't fail** (skipping An Old
  Friend's scene, property purchases, the opening). A restart is now offered only
  when the mission showed MISSION FAILED (all 80 fail paths in main.scm, wasted
  and busted included) or was ended with End or as stuck.
- **Radar map's square corners showing past the round radar** and **coloured
  overlaps between letters in the menus and HUD**: the two-pass leaves split
  screen-space drawing too.
- **FPS held at the refresh rate through Direct3D 9 / DXVK** with
  `FPSLimit=-1`: the game's vsync request is now turned off for that call only
  (changing the game's own copy made it reset the display every frame: a black
  screen).
- **Freeze after changing the resolution through Direct3D 9**: a refused device
  ("device lost") is logged and tried again with the window in front.
- **Pizza Boy throws at the wrong side**: the throw now picks the nearest waiting
  customer, not the script's loop variable.
- **Overall cleanup**: removed unused verification code and dead settings; less
  work per frame in interiors and blending.

### Removed

- **Whole-map loading** and **FarDetail**: they caused FPS drops, flicker and
  missing ground.
- The Widescreen Fix and Project2DFX are no longer offered by the mod manager
  (they overlap VC Modern Fix and caused a 30 FPS cap and a freeze on loading).

### Compatibility notes

- **d3d8to9 + SkyGfx** work behind VC Modern Fix (`D3D8Chain=d3d8_next.dll`).
  For a sharp picture with SkyGfx's colours, `trailsSwitch=2` in `skygfx.ini`.
- **DXVK on Windows**: with an NVIDIA card, a crash was seen (an endless window
  message loop between DXVK's and the driver's window handlers). On Windows,
  d3d8to9 alone is recommended; DXVK isn't needed there.

## 2026-11-13

- First public commit: fixed tick with smooth frames in between, refresh rate,
  frame limiter, mouse fix, exit crash fix, preloading, quick save / load, timed
  quick save, auto save, phone skip, mouse look in vehicles, Rhino / fire truck
  mouse aim, run and gun, keep firing when hit, busier streets, longer draw
  distance, seamless interiors, mission leftovers fix.
