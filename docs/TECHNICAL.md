# Technical notes

How GTAVC Classic ModernFixes works inside `gta-vc.exe` v1.0: every hook, address
and design decision. For installing and using the mod, see the
[README](../README.md).

## Features in detail

**Fixed-tick game world** (`[FixedTick]`)
The original game updates physics, AI, scripts and the camera once per drawn
frame. Above ~60 FPS that breaks things (car handling, animations, cutscene
timing). VC Modern Fix runs the game world exactly `TickRate` times per second,
30 by default, which matches the original game with its own frame limiter on.
Frames are drawn in between as fast as your display allows. Physics behave like
the original at any frame rate.

**Native refresh rate** (`[Display]`)
The game asks for 60 Hz in fullscreen. VC Modern Fix picks the highest refresh
rate your display supports at the game's resolution.

**Frame limiter without a busy CPU core** (`[FrameLimiter]`)
The game's own limiter spins a CPU core at 100%. It's turned off, and frames are
paced on a high-resolution timer instead, by default at your refresh rate.

**Other mods: SilentPatch and ASI plugins**
VC Modern Fix doesn't load `.asi` plugins itself. Use the
[Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader)
(32-bit build) as `dinput8.dll` next to `gta-vc.exe`, with SilentPatch
(`SilentPatchVC.asi`) and any other plugins in the game folder or `scripts\`.
VC Modern Fix is `d3d8.dll`, so the two never clash.

VC Modern Fix installs its game hooks at the first presented frame. By then any
ASI loader has loaded its plugins, and SilentPatch has applied both its
load-time patches and its delayed ones (when the game's initialise event
returns, and after the audio library starts; both before the first frame). Where
a plugin already hooked a place VC Modern Fix also hooks, VC Modern Fix calls
through to the plugin's hook instead of replacing it: call sites keep whatever
they called, and entry points hooked with a `jmp` are relocated into VC Modern
Fix's stub. Each case is logged ("hooked by another mod; chaining to it"). If a
place it needs was changed in a way it doesn't recognise, that feature turns
itself off and the log says where. Tested with a plugin hooking the
`ConstructRenderList` call and the start of `AddParticle`: both of its hooks
kept running while blending stayed active.

**SilentPatch, checked against its source** (commit `7497654`): 203 of its byte
patterns resolve in this exe (254 matches). Its fixed 1.0 addresses were checked
too.
- None of its patches touch a place VC Modern Fix hooks or relies on.
- 124 matches are inside drawing or `PreRender` code that VC Modern Fix runs on
  extra frames (HUD and text scaling, coronas, shadows, car and bike lights,
  special effects). These replacements are stateless (scale, position,
  filtering, render state, constants), or their state is scaled by the time
  step, which is 0 on extra frames (the boat rear-flap smoothing,
  `fRearElevatorValue`).
- Its "mouse fix" makes `0x601740` always return false, so the game never takes
  the mouse in exclusive DirectInput mode. That is the known fix for Vice City's
  mouse getting stuck, and VC Modern Fix's mouse setup hook passes it through.
- Its frame limiter (hooks at `0x6004A2`/`0x600449`) measures time since the
  last frame against `1000 / RsGlobal.maxFPS`. VC Modern Fix sets that to 1000
  and paces frames itself, so SilentPatch's limiter lets every frame through.

**Other `d3d8.dll` mods** (d3d8to9, dgVoodoo...): rename the other mod's
`d3d8.dll` (e.g. to `d3d8_next.dll`), put it next to `gta-vc.exe` and set
`D3D8Chain=d3d8_next.dll`. VC Modern Fix passes graphics through to it instead
of Windows' own `d3d8.dll`.

**Mouse that stops working** (`src/mousefix.cpp`)
The game sets up its DirectInput mouse in `0x5FFA20`: `CreateDevice`, data
format, cooperative level (exclusive or not, foreground), then `Acquire`. Four
of its five callers (the menu switching between exclusive and non-exclusive
mouse, and `0x4ADA81`) call it while a mouse device already exists. The old
device is overwritten without being released and can keep the mouse
exclusively, so the new one's `Acquire` is refused and every read
(`0x5FF290`) returns nothing. Only a focus change (for example switching to
Task Manager) makes Windows take the mouse away from the leaked device. VC
Modern Fix unacquires and releases the existing device before each of those
calls, then runs the game's setup unchanged.

**CPU affinity** (`[Compatibility]`): `CPUCore=1` runs the game on one CPU
("CPU 1"), as Task Manager's affinity setting does. `-1` uses all cores. The
game does nearly all its work on one thread.

**No freezes from hard-disk reads** (`[Streaming]`, `src/preload.cpp`): while you
play, the game reads from disk on its own thread: ped speech and mission audio
from `audio\SFX.RAW`, radio and ambience streams (`audio\*.adf`, `*.mp3`, a few
KB at a time through its stream callback 0x5D58E0), and it waits for models from
`models\gta3.img`. On a hard disk each read can be a 50-150 ms seek - a frozen
frame. At startup a low-priority thread reads all of them once (about 1.3 GB,
10-15 s on a hard disk, the game's own loading goes first). `SFX.RAW` and
`gta3.img` stay mapped in the game's memory for the session (about 640 MB of the
4 GB address space); the audio streams stay in the Windows file cache. Nothing
in the game is hooked for this. On an SSD none of it is needed.

**Mouse look in vehicles** (`[Camera]`, `src/freecam.cpp`): in a car, on a bike,
in a boat or an aircraft, moving the mouse turns the camera around the vehicle
and raises or lowers it, at the same speed as the on-foot mouse camera (the
game's mouse sensitivity setting applies). While you look around, the camera
keeps its angle to the vehicle as it turns. Driving with the mouse still for 1.5 s
hands the camera back to the game, which swings it behind the vehicle as before;
parked, it stays where you put it. The game's own wall, vehicle and
line-of-sight checks still run on the new position. Three calls inside the
vehicle camera (0x47FE8C) are hooked: the "string" step 0x48623C, the height
step 0x474F16 and the swing-back 0x4758C6. Off while the game itself uses the
mouse to steer (mouse steering in the Controls menu) and in cutscenes. Trainer fly /
noclip modes turn the vehicle to face the camera after every update, so its front
follows the mouse. A vehicle left like that (exactly level, no speed, facing the
camera) is flying: the camera's angle around it is then kept as a world direction
rather than an angle to the vehicle's heading, or turning the vehicle would turn
the camera, which would turn the vehicle again.

**Quick save and quick load** (`[QuickSave]`, `src/quicksave.cpp`): F5 saves the
game exactly as it is, wherever you are, into save slot 8 (set by `Slot`), named "Quick Save" in Load Game. If that slot
holds one of your own saves, it is copied to `GTAVCsf8 (before quick save).b` in
the same folder before the first quick save. F9 loads it.
The keys only act when pressed on their own: with Shift, Ctrl, Alt or Win held
(ShadowPlay's Alt+F9 recording, for example) they do nothing. The phone-skip key
works the same way.
A message on screen says what happened. Not saved while you are wanted, on a
mission or on water (in a boat, or at the water surface), nor while you are dead
or the game has taken away control (cutscenes), since such saves don't load
properly. It uses the game's own save code (GenericSave 0x61C420) without the
safehouse step that passes 6 hours (0x61A690), so the time of day is kept. F9
does what the menu's Load Game does: the game restarts from that slot. The game only
saves the player when on foot (CPools::SavePedPool 0x4BEEBE), so in a vehicle the
player is put on foot where they sit for the moment of the save; loading it puts
you on foot at that spot. The vehicle you were in is written next to the save
(`GTAVCsf8.quickcar`, tied to that save by its checksum); half a second after F9
loads, it is created again where it was, with its colours and health, and you are
put in the driver's seat. Loading the quick save from the menu brings no vehicle
back; boats are never quick saved (on water).

**Timed quick save** (`AutoMinutes`, default 5): every 5 minutes of play (game
time, so paused menus don't count) the game quick saves by itself into the same
slot, so F9 loads the latest one, and the message says "Quick Saved (timed)". It
only saves when a quick save is allowed (no mission or job, not wanted, not on
water, in control, alive) and also not on a phone call (ped state 36), not in a
cutscene (CCutsceneMgr::ms_running 0xA10AB2), not in the air (vertical speed of
the player or their vehicle, CPhysical +0x70) and not while a mission auto save is
waiting. When it's due but not safe, it waits and saves as soon as everything is
clear (the log says what it waited for). Any save (F5 or a mission auto save)
restarts the count, and so does loading a game. `AutoMinutes=0` turns it off.

**Skip phone calls** (`[PhoneSkip]`, `src/phoneskip.cpp`): during a call you
answered (Tab), Enter skips the rest of it. The line being spoken stops and the
remaining lines are not played and count as finished, so the call's script runs
to its end in a moment and hangs up as usual: whatever the call sets up still
happens. Only while the player is on the mobile (PED_ANSWER_MOBILE, state 36, set
by CPed::SetAnswerMobile 0x4F59C0). Hooks the script's PLAY_MISSION_AUDIO (0x45ABD7)
and HAS_MISSION_AUDIO_FINISHED (0x45AC04) calls.

**Interiors follow the player** (`[Interiors]`, `src/interior.cpp`): the game
draws one area at a time (CGame::currArea 0x978810: 0 = the city, 1-18 = interiors,
13 = everywhere), and only the door scripts change it. Teleporting out of a room
used to leave the city undrawn, and flying into a room with a jetpack left the
room undrawn. Each game update, a vertical line down from the player
(CWorld::ProcessVerticalLine 0x4D8B00, buildings only) finds what they stand on:
- an interior's floor: switch to that interior. Coming from the city, a ceiling
  above the player is also needed (CWorld::GetIsLineOfSightClear 0x4DA560), so a
  sidewalk next to an interior never counts.
- the city's ground while an interior is set: switch to the city once the player
  is outside every building of that interior (collision bounding spheres), or,
  for a room this feature switched to, has open sky above.
- nothing (in the air) while an interior is set: switch to the city when the
  interior's nearest building is 30 m or more away.

The new area must hold for half a second while the player has control. Then it
is applied the way the door scripts do it: set the area, unload the other area's
models (0x40DAA0), load the scene around the player (CStreaming::LoadScene
0x40AF60). Going back to the city also turns off interior lighting and restores
normal traffic, unless you're on a mission. `Interiors:` lines in the log list
each switch. Doors keep working through the game's own scripts.

**No fade at interior doors** (`NoDoorFade`, same file): every interior door script
calls one shared `main.scm` routine that fades to black (500 ms, 1500 ms for the
Malibu), waits for the black, and after the area switch and scene load fades back
in. Its six `DO_FADE` (016A) commands sit at fixed places (main.scm 0x2033E,
0x20366, 0x20374, 0x2041B, 0x20443, 0x20451). The call to `CCamera::Fade` in the
DO_FADE handler (0x454130) is skipped when the running script's position is just
past one of those, and the bytes there are still DO_FADE: the screen never goes
black and the routine's "while fading" loop ends at once. Fades anywhere else
(missions, cutscenes, deaths, arrests) are unchanged. Log lines: `no fade to black
at interior doors`, `door fade skipped`.

**Outside view from interiors** (`ShowOutside`, same file): almost every interior
is built inside its real building, so the city around it is drawn too, and open
doors show the street instead of black.
- **Area tests:** the twelve tests the game makes for drawing, streaming and
  shadows (`cmp eax,[currArea]` at 0x40E680, 0x40E6BD, 0x40E6FD, 0x40E73D,
  0x40E7F6, 0x40E8C6, 0x4C7940, 0x4C7D55, 0x4C81DB, 0x56B992, 0x56C033 and
  0x56C1F0) now call a stub that also accepts area 0. So does the dummy-to-object
  test at 0x53DA8A, which makes the lamp posts outside appear.
- **Keeping the city loaded:** the calls to RemoveBuildingsNotInArea (0x630B53 in
  the door scripts, 0x42BB16 on respawn) keep the city's models loaded, so nothing
  pops in.
- **Water:** the "is water drawn here" test (0x4A4360) says yes.
- **Overlapping pieces:** two kinds are moved to a hidden area (0xFE) while you're
  inside, and put back when the area changes:
  - the interior's own stand-in for the outside, shown through its doors and
    windows instead of the city: models named `*exterior*`, `*fake_ext*`,
    `*blackpoly*` or `*_ext` (`htl_exterior03`, `club_exterior01`-`12`,
    `cop_fake_ext01`-`03`, `cop_blackpoly`, `biker_bar_exterior`). Drawn over the
    real city they block the view and flicker against it.
  - city buildings inside the room: the "fake" interior seen through windows from
    outside, the exterior shell, the ground under it. A building counts when its
    centre is inside the collision box of one of the room's buildings and at least
    25% of its own box is inside that box, or 60% of its own box is inside one. (The
    25% keeps long streets whose middle lies under a room: `odNroad01_dy`/`_nt`, a
    256 m stretch of Ocean Drive centred under the Malibu's dance floor.) A city
    building bigger than the room piece it matched is the building's outside around
    the room (the Malibu's `od_clubout` with its roof and ladders, its fences and
    neon): it stays.
  - the city building you are standing inside: its exterior shell, which has dark
    boxes in its doorways and glass over the room's own (`od_clevelander` around
    the Ocean View lobby). Any tall city building whose collision box contains the
    player, checked four times a second, and its low-detail `LOD...` model. Except
    a building kept above as the outside around the room (the Malibu's
    `od_clubout`): only its black door panels inside the room are opened, as from
    the street.

  The log lists the stand-ins hidden, how many city buildings were hidden, and the
  city buildings left touching the room (`kept city ...`), for tuning.
- **Looking in from the street:** standing in the city within 20 m of the
  collision box of one of an interior's buildings, that interior is drawn too.
  Its stand-ins for the city are hidden: pieces named like an exterior, pieces
  bigger than 600 m (the mansion's `IslandLOD1`/`2`, 1100-1400 m low-detail copies
  of a whole island for its window view, also hidden from inside; the mall's own
  roof `mlmallroof`, 248 m, and `MALLUNDER` stay), and its pieces more than
  300 m away. From the street, only interior buildings inside a city building are
  drawn and count as "near": their collision centre must be in the collision box of
  a city building at least 4 m tall (the lobby inside `od_clevelander`). That keeps
  out the city copies interiors carry for cutscenes (the lawyer's `lawcutmapa`-`c`
  streets), which would flicker against the real streets. Each building's answer
  is kept, since buildings never move. A city building's doorway is usually closed by a black panel
  (behind the Ocean View doors: two triangles of `od_clevelander` textured
  `wee_blak_32`). Any part of a city building within 60 m that has a black texture
  (name containing `blak`/`black`) and sits inside one of the interior's buildings
  is made invisible: its triangle-strip indices all point at one vertex, and the
  mesh header's serial number is bumped so RenderWare rebuilds the model. When
  you walk away it's put back the same way. The geometry is the model's, shared by
  all its copies, and stays loaded when the building's drawn copy is deleted, so a
  part is put back only once it can be checked (drawn copy back, same geometry,
  same mesh still holding exactly what was written); until then it waits on the
  list, and a mesh already reduced to one vertex is never taken. Log lines: `showing area N from the
  street`, `opened the doorway of ...`.
- **People inside, seen from the street:** the door scripts switch an interior's
  ped paths on when you walk in and off when you walk out (`SWITCH_PED_ROADS_ON`/
  `OFF`, 022A/022B), so from the street the room would be empty. At start the
  boxes are read from `data\main.scm` (`SET_AREA_VISIBLE n` followed by 022A: the
  Malibu, the mall, the mansion, area 3). While area n is shown from the street its
  ped paths are switched on (`CPathFind::SwitchPedRoadsOffInArea`, 0x435980), and
  the game's own population walks people in; the game creates them where you
  can't see them being created, so they appear as you approach. When the view ends
  they're switched off again, unless you're now in that area (its script has them).
  Log line: `N interiors have people walked in when seen from the street`.

  That alone isn't enough: `CPopulation::AddToPopulation` (0x53BA80) creates
  people 40-50 m from you when in view and 15-25 m when not, and the room is closer
  than that; and `ManagePopulation` deletes anyone more than 25 m away who has been
  off screen for 4 s (`CPed` +0x35C), so the crowd is deleted when you walk out and
  turn to the street. So while such a room is shown from the street and you're
  within 45 m of it, `AddToPopulation` is called once more twice a second, centred
  on the room (its `FindPlayerCentreOfWorld` call at 0x53BAE6 answers the room's
  centre), with a radius that stays inside the room's box and without its "in view
  and closer than 40 m" test (the branch at 0x53C937), up to 12 people in the room.
  People inside the room's box get their 4 s again every update. Log line:
  `walking people into area N seen from the street`.
- **Shared surfaces:** where the interior and the city building around it draw
  the same wall or floor, they would flicker. While a second area is drawn,
  everything is drawn with the viewport's depth range shrunk a little
  (`MaxZ * (1 - eps)`, hooked `IDirect3DDevice8::SetViewport`), except the second
  area's buildings. A wrapper around `CRenderer::RenderOneNonRoad` (0x4C9DA0,
  called at 0x4C9ED1, 0x4C9F23, 0x4CA10D and 0x5813D1) gives those the full range.
  They land slightly further away and lose every tie with the area you're in.
  `eps` is 3e-5 with a 24-bit depth buffer, or 1.2e-4 with a 16-bit one (logged as
  `Display: depth buffer format`).
- **Interiors away from the city:** the stadium arenas float high above the map,
  so they stay as before. The view is used only when the city is within 25 m and
  the room within 80 m as the area is set, and checked again every second.
- **What doesn't change:** the interior's own lighting (its time-cycle "extra
  colours") also lights the street you see through the door.

**Streaming memory and draw distance** (`[Streaming] MemoryMB`, `VehicleModels`,
`[DrawDistance] Scale`, `src/drawdist.cpp`)
- `CStreaming::Init` (0x410780) gives the model streamer 45 MB
  (`ms_memoryAvailable`, 0x94DD54, written at 0x410799) and keeps 25 vehicle models
  loaded (`desiredNumVehiclesLoaded`, 0x6867A0, written at 0x41078F; the list holds
  at most 50). With that little, models and textures are dropped and loaded again
  all the time: buildings and cars show up late, low-detail first. The two values
  written there are replaced, in the code before the game starts (DllMain) and
  directly at the first frame in case `Init` already ran. Defaults 512 MB and 50;
  `gta-vc.exe` is large-address-aware here (`SetLargeAddressAware.ps1`).
- Every frame `CCamera::Process` works out `LODDistMultiplier` (TheCamera+0xF0:
  70 / FOV, more when high up, at most 2.2) and multiplies it by
  `CRenderer::ms_lodDistScale` (0x690220, the menu's Draw Distance slider) at
  0x46DEC3. Every model's distance from the IDE files, the switch from a building
  to its low-detail LOD, peds' low-detail and fade distances and vehicle
  components scale with it. That `fmul` reads a float of the DLL instead: the
  slider's value times `Scale` (kept up to date every frame, so the slider still
  works).
- Vehicles' own low-detail and fade distances (`CVisibilityPlugins::
  SetRenderWareCamera`, 0x581840: 70, 90 and 100 m, big vehicles 60 and 150 m,
  stored squared at 0x974B00, 0x974B10, 0x97508C, 0x975200, 0x975210) use
  `GenerationDistMultiplier` (TheCamera+0xF4), which also decides where traffic and
  peds are created and removed. That is left alone; the five distances are
  multiplied by `Scale` squared after the function runs (wrapped at its five
  callers, 0x4A5CEA, 0x4A5EBC, 0x4A5FBC, 0x4A60E8, 0x4A7350).
- The 300 m limit: CRenderer's `LOD_DISTANCE` (0x69022C, 300.0, read in 28
  places in `ScanWorld` and `SetupEntityVisibility`). Beyond it the world scan only
  looks at the big low-detail LOD buildings, and full-detail models past it are
  pushed to their fade distance, so no multiplier alone shows full detail past
  300 m. It becomes 300 m times `Scale`. More full-detail models in view means more
  entries in the renderer's visible list (0x7D54F8, 2000 entries, count 0xA0D1E4),
  which the game fills without checking: its three writes (0x4C7613, 0x4C77ED,
  0x4C85AD, `mov eax,[count]; inc [count]; mov [list+eax*4],reg`) call a stub that
  stops adding at 1990.
- The far clip: `Idle` gives the RenderWare camera the weather's far clip
  (`CTimeCycle::m_fCurrentFarClip`, 0x9B6A6C) each frame, calling
  `RwCameraSetFarClipPlane` (0x64A930) at 0x4A6032; it gets that times `FarClip`.
  The weather's own value is untouched.
- Anisotropic filtering (`[Graphics] Anisotropic`, `src/dllmain.cpp`):
  `IDirect3DDevice8::SetTextureStageState` (vtable 63) is hooked; where RenderWare
  asks for linear minification, anisotropic is used at the level set (capped at
  the card's `MaxAnisotropy`).
- Memory: the game is a 32-bit program. With the large-address flag it can use
  4 GB of address space in total, and that is the ceiling; it can't use 8 GB.
  Every model file in `gta3.img` is about 312 MB, so the 512 MB budget already lets
  everything that has been loaded stay loaded.
  Log lines: `Streaming: N MB for models ...`, `DrawDistance: ...`, `Graphics: ...`.

**Auto save** (`[AutoSave]`, `src/quicksave.cpp`)
A mission or job is anything that sets the script's "on a mission" flag: story
missions, taxi, vigilante, firefighter, paramedic, rampages. When that flag goes
back to 0 without being wasted or busted (`CPlayerInfo::m_WBState`, 0x94ADF4), and
a mission was passed (`CStats::LastMissionPassedName`, 0x7E9D78, changed) or money
was earned (0x94ADC8), the game is saved about 4 seconds later, once a quick save
would be allowed (not wanted, not on water, in control). It uses the quick save's
code, into `Slot` (default 7), named "Auto Save"; one of your own saves in that
slot is first copied to `GTAVCsf<slot> (before auto save).b`. Loading a game
resets the tracking. Log lines start with `AutoSave:`.
Like a safehouse save, loading an auto save starts you at a safehouse: for the
save only, the player is put on foot, in the city (area 0), at the save point of
the nearest safehouse you own, and put back right after. Owned safehouses are
the ones whose save script (PSAVE1-10) is running (`CTheScripts::pActiveScripts`,
0x975338); their save points are where those scripts create the save pickup
(PSAVE5 from `$570`-`$572`). No time passes, unlike a safehouse save.
The help box shows "Game auto saved...".

**Mouse aim for the Rhino and fire truck** (`[Vehicles] MouseAim`, `src/gameplay.cpp`)
The turret (`CAutomobile::TankControl`) and the water cannon (`FireTruckControl`)
turn with the numpad "car gun" keys, by changing `m_fCarGunLR` (+0x5B0) and
`m_fCarGunUD` (+0x5B4, cannon only). Before each game update they are turned toward
where the camera looks (TheCamera's forward, 0x7E4698), which FreeCam moves with
the mouse; the turret turns at most 2.5 rad/s, the cannon 5 rad/s. In these two
vehicles FreeCam doesn't swing the camera back behind while you drive, so the gun
stays where you aimed.

**Mission leftovers** (`[Fixes] MissionLeftovers`, `src/gameplay.cpp`)
"Martha's Mug Shot" (PORNO3) swaps the Ocean Drive hotel `od_chariot` (3700) for
`od_wtsign` (3568), and its LOD `LODchariot` (3733) for `LODd_buildnew` (3661),
while you are on its roof, and swaps them back afterwards. If the mission ends
without that, the swap stays in `CTheScripts::BuildingSwapArray` (0x94DBE8) and
every save keeps it: the hotel shows without its walls. Off a mission such an
entry is undone like the game's `UndoBuildingSwaps` (0x458CC0) does it
(`CBuilding::ReplaceWithNewModel`, 0x407DB0), and the mission's flag `$4487` reset.

**Ped sight crash** (always on, `src/gameplay.cpp`)
`CPed::OurPedCanSeeThisOne` (0x50D360, used by scripts) looks at the peds in the
world sectors between two peds, but doesn't keep the sector numbers inside the
80 x 80 grid (0x792D48) the way the game's other sector scans do. A ped outside the
map, or a bad position, makes it read past the grid and crash at 0x50D660. A stub
at 0x50D60E clamps the four numbers to 0-79.

**Run and gun** (`[Weapons] RunAndGun`, `src/gameplay.cpp`)
A ped moves through its walk/run/sprint animations, and a gun's fire animation is
laid over them. `colt45_fire` and `TEC_fire` only move the right arm, so pistols
fire on the move. `RIFLE_fire`, `UZI_fire`, `M60_fire`, `FLAME_fire`,
`buddy_fire(RELOAD)`, `shotgun_fire` and `python_fire` also move the root, pelvis
and legs (bone tags 0, 1, 41-43, 51-53), which holds the player still. Those tracks
are dropped from the standing fire animations (CAnimManager's hierarchies at
0x7F3D28; `CAnimBlendAssociation::Init`, 0x401988, skips a track whose bone tag
matches no bone). The legs follow the walk/run/sprint and the upper body keeps each
gun's own two-handed pose. Crouch animations stay whole. Tracks are linked to bones
once, when a weapon's animation block streams in: CStreaming calls `LoadAnimFile`
(0x404A50, at 0x40C5CE) and then `CreateAnimAssocGroups` (0x405430), and every shot
copies those links. So the tracks are dropped in a hook on that load call, before the
groups are built (log: `Weapons: legs freed in ...`).

Also: firing while sprinting (the fire check skips PEDMOVE_SPRINT at 0x534B18, and
`CPed::Attack` breaks off on the sprint key at 0x52BDE0); HEAVY (0x200 in
`CWeaponInfo` +0x5C, 0x782A14) is cleared on the rocket launcher and minigun so
they can sprint and jump; and the aim button no longer sends the M4, Ruger or M60
into the first-person scope (checks at 0x5349DB, 0x5349E0, 0x5349E5), which held
the player still. Other peds share these animations, so they can move while firing too.

**Keep firing when hit** (`[Weapons] KeepFiringWhenHit`)
When a bullet hits a ped, `CWeapon::DoBulletImpact` (0x5CF493 for the player, plus
4 more hit paths) calls `CPed::ClearAttackByRemovingAnim` (0x52CF70) before the
flinch, so the player stops shooting. Those calls now skip the player: you still
flinch, but your attack carries on.

**Busier streets** (`[Population]`, `src/population.cpp`)
At start-up the game reads a ped and a car density factor from `gta3.ini`; without
that file both stay 0.6, which allows at most 15 peds (25 x factor) and 7 traffic
cars (12 x factor). The ped factor also scales how many peds each zone asks for.
The factors now come from `PedDensity` (2.5: up to 62 peds) and `CarDensity` (1.5:
up to 18 cars), and they're applied after that start-up code, so a `gta3.ini`
can't undo them. The ped pool holds 140 and the car pool 110, so both fit.

**Peds vanishing when you look away** (`[Population] KeepOffscreen`)
`CPopulation::ManagePopulation` (0x53D690) removes a ped more than 51 m away, but a
ped that is off-screen already at 25 m (0x694EB4, read at 0x53DEDA). Look at
someone 30 m away, turn the camera, and they're gone. That one read now uses
`KeepOffscreen` (50 m), so peds stay until they're really far away.

**Crash-free exit** (always on, `src/exitguard.cpp`)
Quitting runs `CGame::ShutDown` (0x4A49E0, called at 0x6005C9), which frees every
model, texture dictionary and pool one by one. A texture can then be freed after
its dictionary (`RwTextureDestroy` 0x64DEC0 faulting at 0x64DEF1): "Unhandled
exception c0000005" on exit. The call goes through a guard: an access violation
inside it is logged (`ExitGuard:` line with the address and the callers) and the
rest of that clean-up is skipped, while the game's own exit carries on. Saves and
settings are on disk long before this point.

**4 GB of memory** (optional, `SetLargeAddressAware.ps1`)
Marks `gta-vc.exe` large address aware, so the 32-bit game can use 4 GB instead
of 2 GB on 64-bit Windows.

## How it works

`d3d8.dll` forwards `Direct3DCreate8` (the only function the game imports from
it) to the system `d3d8.dll`. It hooks `CreateDevice`/`Reset` for the refresh
rate and `Present` for the frame limiter.

### The game loop

`Idle()` (`0x4A5D80`) runs once per frame:
`CTimer::Update` → per-frame inits → `CGame::Process` (everything that moves) →
audio → render (mirrors, `ConstructRenderList`, entity `PreRender`,
`RenderScene`, effects, HUD, fade) → `Present`.

Its single call site (`0x4A5BE0`) is redirected. The whole `Idle()` runs only
when a tick is due. Between ticks, *redraw frames* run only the render half,
entered at `0x4A5DE3`. Without a tick, `CTimer::Update` doesn't run, so game
time doesn't advance. The game's own limiter is disabled
(`RsGlobal.maxFPS`, `0x9B48EC`, set to 1000) so it doesn't block redraw frames.
The pause menu stops the game clock (`CTimer::m_UserPause`), so it runs whole
frames at full rate, like the start menu (which has its own loop).
Loading and fully faded screens run at the tick rate with no redraw frames.

### How redraw frames stay invisible to the game

The render half changes game state as it draws. Every function reachable from it
(686 game functions, including every entity class's `PreRender`/`Render` from its
vtable) was scanned for state it changes. A redraw frame:

- sets `CTimer::ms_fTimeStep` and `ms_fTimeStepNonClipped` to 0, freezing every
  timer and animation driven by the time step (checked: nothing divides by it);
- draws from the state the last tick left, and afterwards puts back all
  3,179 bytes of global state the render half can change (`kRedrawState` in
  `src/redraw_state.h`, generated by the scan). That covers HUD message state
  machines, flash counters, motion-blur fade, trail histories and per-frame flags.
  The HUD's help box, for example, copies new text to a "last shown" buffer and
  plays its sound when a message arrives. An extra frame sees that already done,
  so it draws the box and changes nothing;
- gives lists and request flags their values from *before* the tick drew. These
  are filled by game logic, `ConstructRenderList` or `PreRender` and used up while
  drawing (`kRedrawLists`: shadow and light store counts, blur request, sprite and
  font buffers). Without that, they would be empty on extra frames. Two snapshots
  are used: before the mirror pass and after `PreRender`. The split comes from
  a static scan of the exe: reset while drawing and written outside it. HUD state
  that only `CHud::Initialise` writes outside drawing counts as a state machine.
  Input state, the timer, the world scan code and streaming are never touched;
- skips `ConstructRenderList` and the entity `PreRender` pass, and restores their
  results instead, including the alpha-sorted entity lists (`CLinkList`s at
  `0xA0FD84`, `0x97F2F8`, `0x978D44`, header plus nodes). Without this,
  transparent objects would be inserted again on every redraw;
- saves and restores the random number seed;
- blocks particle spawning (`CParticle::AddParticle`, `0x563CC0`) and
  floating-object spawning in the water renderer (`0x5BB570`, `0x636D20`), the
  same way a full pool would;
- stops the helicopter rotor from advancing (it adds a constant per draw,
  `0x69C144`, not scaled by time step);
- keeps 3D markers (the shop and mission rings, cylinders and arrows;
  `C3dMarkers`, 32 x 0x78 bytes at `0x814018`) alive. `C3dMarkers::Render`
  (`0x570BA0`) deletes every marker not placed since the last render, and
  scripts place theirs once per tick, so each redraw would delete them and the
  next tick create them again (spin and pulse restarting 30 times a second). A
  redraw flags every live marker as placed and puts them back exactly as they
  were afterwards.

### Blending between ticks (phase 2a, `src/interp.cpp`)

Each tick records the camera matrix and the matrix of every ped, vehicle and
object (pool slot and pool ID, so reused slots are never blended). A frame drawn
`alpha` of the way into the tick interval shows `prev + (cur - prev) * alpha`.
Positions are lerped. The forward and up axes are lerped and re-orthogonalised,
right is rebuilt with the real matrix's handedness, and axis lengths are kept.
More than 10 m or 90 degrees in one tick counts as a teleport or camera cut, and
that thing is drawn where it really is.

- Bikes: their head and tail lights come from a second matrix,
  `CBike::m_leanMatrix` (`+0x2C4`), built once per tick by
  `CBike::CalculateLeanMatrix` (`0x609C90`, flag `+0x2C0`). While a bike is
  drawn blended, the flag is cleared so `PreRender` rebuilds it from the blended
  matrix; afterwards it is put back (on a tick frame, rebuilt from the real
  matrix, as the game would have). Without this the lights sat where the bike
  was at the last tick, away from the bike and jumping every tick.
- Objects: what is blended is what is drawn, the RenderWare frame matrix, tick
  to tick. The `CMatrix` at `+4`, which drawing code reads for lights, shadows
  and LOD, gets the same movement as an offset (blended × inverse(real)). This
  keeps objects moved outside the normal update consistent with what they
  carry. Trainer fly mode, for example, writes only the `CMatrix`, after the
  game update: the car's drawn matrix and the player sitting in it both lag
  that write by one tick, exactly as in the original game, and both blend
  together. Then `CEntity::UpdateRwFrame` (`0x489360`) runs. For skinned clumps (peds,
  cutscene characters), `CEntity::UpdateRpHAnim` (`0x489330`) rebuilds the bone
  matrices from the moved root, and again after putting back, since game logic
  reads bone positions.
- A frame shows the moment one tick length ago on a steady clock, measured
  against when the last two ticks actually ran. The jitter in tick start times
  never shows as uneven motion.
- The camera is moved the way `CCamera::Process` does it:
  `CalculateDerivedValues` (`0x46C3B6`, frustum), then copy into the RenderWare
  camera frame, `RwMatrixUpdate`, `RwFrameUpdateObjects`, `RwFrameOrthoNormalize`.
- Every byte changed (matrices, RenderWare matrices, the camera fields involved)
  is put back after the frame is drawn.

**What's visible has to follow the blended camera**, or things at the trailing
screen edge would vanish while turning. So `ConstructRenderList` runs for every
drawn frame. Its output also feeds game logic, for example visibility flags that
decide whether off-screen peds can be removed. So on a tick, the real pass runs
first with the game's camera, exactly as before, and its output is saved:
`kCrlState` (12 KB of globals, including a 350-node distance-sorted list and a
48-entry table), plus each world object's alpha fade, visibility flags and scan
code. The blended pass runs after that, and the saved output is put back after
every blended pass. Game logic only ever sees the real pass.

**Ped poses** (`BoneFrames`/`BlendPose`): each tick also records every skinned
ped's bone keyframes from its RenderWare animation interpolator. This RenderWare
build keeps them at interpolator `+0x48`, stride at `+0x24`, rotation
quaternion at `+8`, translation at `+0x18`, read from
`RpHAnimHierarchyUpdateMatrices` (`0x6464A1`). They are only touched when the
standard apply callback (`0x66A8A0`) and a 0x24 stride are in use. Drawn frames
blend each bone (normalized quaternion lerp, translation lerp) before
`PreRender` rebuilds the bone matrices. Animation advances only in game logic:
nothing in the `PreRender` tree passes the time step, and its one skeleton
helper (`0x525960`) only reads.

**Game clock (3a):** drawing code reads `CTimer::m_snTimeInMilliseconds`
(`0x974B2C`, 90 reads in the drawing and `PreRender` code) for water, blinking
lights, corona flicker and HUD/radar blinking. The same code also computes ages
as "now − timestamp" compared unsigned (18 sites, e.g. `0x54A780`, `0x568050`).
A clock earlier than a timestamp from the latest tick wraps to a huge age:
bullet traces and gun smoke draw wrongly and entries get removed. So the clock
is never earlier than the latest tick. Tick frames draw with the real time,
exactly like the original. Redraw frames run it forward from the latest tick by
the same fraction of a tick as the blending, and the real value is put back
after. Clock-driven animation leads positions by at most one tick, which isn't
visible. The previous-time,
non-clipped and pause-mode clocks are not read while drawing (pause-mode only
by menu/HUD) and stay untouched. So does the frame counter (`0xA0D898`), which
counts whole ticks.

**Particles (3a):** a `CParticle` has position at `+0`, velocity at `+0xC`,
end-of-life time at `+0x18` and next at `+0x54`. There are 83 particle types at
`0x975428`, stride `0x94`, with the active list at `+0x90`
(`CParticle::Update`, `0x5620A8`–`0x563B10`). The update applies wind, drag and
collisions, not just velocity, so each tick records every live particle's
position (hash table by pointer). Drawn frames blend between the two recorded
positions. A recycled particle (different end-of-life time) or a jump of more
than 10 m is drawn as is. Particles spawned during the latest tick appear at
their spawn point.

**Mouse look (3b, open):** drawing shows the moment one tick ago, and the
game reads the mouse only on ticks. Turning therefore lags about one to two
ticks (33–66 ms at `TickRate=30`). Options:
- **Turn the camera between ticks from raw mouse input**, with the game world
  kept at 30 ticks. It would need the camera modes (on foot, car, aiming,
  sniper) to be handled individually.

(`TickRate=60` would halve the delay, but it changes the game world to the
original's 60 FPS behaviour, so it's not used.)

**Vehicle parts** (`InterpPartsCapture`/`InterpPartsApply`): doors, bonnet,
boot, wheels (spin and suspension), bike forks and other parts are child frames
of the vehicle's RenderWare frame (`+0x98` child, `+0x9C` next, `+0xA0` root).
`PreRender` sets them from game state that changes once per tick. Each tick
records their local matrices after `PreRender`. Every drawn frame, after
`PreRender`, each part still holding exactly that tick's value is blended
between the two ticks. A part that turns more than 90 degrees per tick (fast
wheel spin) is drawn as is, and a changed hierarchy (a part fell off) stops the
blending for that vehicle.

**PreRender on every frame (2b):** a redraw frame runs the entity `PreRender`
pass starting from the values the tick had right before its own `PreRender`.
Those values are the counts and temporaries in `kPreRenderState`, generated by
a static scan of the exe with true writes only (x87 arithmetic on memory is a read), plus
`kRedrawLists`. Its ped one-shot sound (`0x5F9DA0`, `ret 0Ch`) is blocked.
Particles and water props can't spawn, the RNG is put back, and the time step
is 0. Car parts that break off (`0x59AAA0`) are left alone: their flag is
cleared by the tick's `PreRender`, which always runs first.

**Free-list heads are never restored.** The static scan checks every restore
table for globals written with a value loaded through a pointer, which is the
"pop from a free list" pattern. Two such globals were found and are excluded
(asserted by the generator): the shadow polygon pool's free list
(`CShadows`, `0x9B48D4`) and the particle free list (`0x98002C`). Rewinding the
shadow list after `PreRender` had taken blocks from it linked blocks into two
lists at once, and the static-shadow drawing loop then never ended.

**Freeze watchdog** (`src/watchdog.cpp`): once the game loop runs, a background
thread watches a heartbeat from the main loop and `Present`. If it stops for 3
seconds, the game thread is briefly suspended. Its registers, its return
addresses into `gta-vc.exe`/`d3d8.dll` and 8 instruction-pointer samples are
written to `VCModernFix.log` with plain Win32 calls. This is how the shadow-list
freeze was found.

**Tick schedule after a slow frame:** a tick that is a whole interval or more
late restarts the schedule from that moment. It isn't followed by catch-up
ticks back to back, which would each get a tiny time step and show as a stall
followed by a lurch. The game measures real elapsed time, so the next tick
covers the gap, as in the original game after a slow frame.

**Restoring after a tick frame** keeps any change the frame's own drawing made
(water props bobbing, parts placed by `PreRender`). Only matrices that still
hold the blended values are put back. Redraw frames are rolled back completely.

Known limits: mirror reflections on tick frames use the real camera (the mirror
pass feeds the real list build, so it keeps its original place). Streaming may
request models a few milliseconds earlier.

All hook sites are checked byte for byte before anything is patched. On any
mismatch, fixed tick isn't installed and the log says so.

## Building

Requires [Zig](https://ziglang.org/) (`winget install zig.zig`). Run
`build.bat`; it produces `d3d8.dll` (32-bit).

## Source files

| File | Purpose |
| --- | --- |
| `src/dllmain.cpp` | d3d8 proxy, refresh rate, `Present` hook |
| `src/fixedtick.cpp` | fixed-tick loop, redraw frames, hooks |
| `src/interp.cpp` | camera/object/pose blending, render-list flag save/restore |
| `src/watchdog.cpp` | freeze diagnostics into `VCModernFix.log` |
| `src/mousefix.cpp` | mouse device re-creation fix |
| `src/freecam.cpp` | mouse look around the vehicle |
| `src/quicksave.cpp` | F5 quick save, F9 quick load |
| `src/phoneskip.cpp` | Enter skips phone calls |
| `src/interior.cpp` | The drawn interior follows the player (teleports, jetpack); the city is drawn from inside interiors |
| `src/drawdist.cpp` | streaming memory, vehicle models kept loaded, draw distance |
| `src/gameplay.cpp` | mouse aim for the Rhino and fire truck, mission leftovers (hotel swap), run and gun, keep firing when hit |
| `src/population.cpp` | ped and car density, peds kept when you look away |
| `src/exitguard.cpp` | skips a crash in the game's own clean-up when quitting |
| `src/redraw_state.h` | generated table of render state restored on redraw frames |
| `src/framelimiter.cpp` | timer-based frame limiter |
| `src/config.cpp` | `VCModernFix.ini`, `VCModernFix.log` |
| `VCModernFix.ini` | default settings |
| `docs/TECHNICAL.md` | this file |
| `SetLargeAddressAware.ps1` | set or clear the 4 GB flag on `gta-vc.exe` |
| `build.bat` | build script |
