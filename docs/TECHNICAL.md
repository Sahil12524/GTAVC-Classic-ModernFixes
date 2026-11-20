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
in the game is hooked for this. On an SSD none of it is needed. The two files are
mapped only when `gta-vc.exe` has the large-address-aware flag and the address
space left (`GlobalMemoryStatusEx`) still holds them plus `MemoryMB` plus 512 MB;
otherwise they only get the file-cache read, and the log says why. Once a minute
a `Memory:` line gives the committed memory, the RAM in use (and its peak) and
the address space left.

**Opened doorways and streamed models** (`src/interior.cpp`). Each opened black
part keeps pointers into its building model's geometry, which the streaming frees
when it drops the model. Each record also keeps the model id. Before a part is
put back, the record is dropped when that model isn't loaded
(`ms_aInfoForModel` 0x94DDD8, 20 bytes a model, +0 = 1) or the building now shows
another model: freed geometry is never written to.

**Small fixes**:
- **The car brought back after F9** (`quicksave.cpp`): its spot is cleared first
  with `CTheScripts::ClearSpaceForMissionEntity` (0x45F180), as `CREATE_CAR`
  does. That removes only random cars and peds the game may delete, never the
  player.
- **Ending a stuck mission** (`missionhelp.cpp`): a mission script whose gosub
  depth is over 6 (more than the game's stack holds, so corrupt) is left alone
  and logged.

**Per-frame mouse look on foot** (`[Camera] FrameMouseLook`, `src/mouselook.cpp`,
`src/interp.cpp`): `CPad::UpdateMouse` (0x4AD820) reads the mouse once per update
(`_InputGetMouseState` 0x5FF290, called at 0x4AD86D), so with FixedTick the
on-foot mouse camera showed up 33-66 ms late. While the camera is CCam mode 4 with
the mouse flag (0xA10B4C, `Process_FollowPedWithMouse` 0x47BE8F) the mouse is also
read every drawn frame; the movement (and any button pressed meanwhile) is added
to the game's own read at 0x4AD86D, so the game gets exactly what it would have.
The drawn camera is then the latest update's camera turned by the movement not
yet used, as the game will turn it (0x47C033..0x47C234: yaw +0xBC += -2.5 X sens_h
FOV/80, pitch +0xAC += 4 Y sens_v FOV/80, X / Y flipped by the invert options
0xA10A4C / 0xA10A4D, pitch kept in -1.562..0.785), orbiting the player: its offset
from the player in the camera's own axes is kept and set around where the player
is drawn; that offset is blended between the last two updates (the game eases the
camera's distance and height). The game also turns the camera by itself (swinging
behind the player, target locks): that part, the latest update's turn beyond what
its mouse movement gives, is drawn blended (`- (1 - blend) * self turn`), so the
view never switches between drawing methods. Off while a trainer window has the
mouse, in menus, cutscenes and replays.

**Frame timing figures**: every 10 s of play (30 reports at most) the log gets a
`Frames:` line: FPS, worst gap, frames over 1.5 refresh intervals, time blocked in
Present (vsync), and the cost of update frames and in-between frames. The
`Display:` line shows the presentation interval the game asked for.

**Vsync pacing** (`src/framelimiter.cpp`): with `FPSLimit=0` and vsync asked for in
fullscreen, the limiter steps aside (its own clock drifted against the display's
real refresh: a held frame every couple of seconds). It checks each second that
vsync really holds the frames; at over 1.15x the refresh rate (vsync forced off in
the driver) the limiter takes over again for the session.

**Frames shown on time** (`src/framelimiter.cpp`, `src/fixedtick.cpp`): each frame
gets its present time (the limiter's next slot) when it starts; it is blended for
that time, the tick decision is made for it, and it is held until then before
Present (the wait used to come after Present, so frames with a game update in
them, a few ms longer to draw, reached the screen later than the moment they
showed: a 30 Hz shudder on things moving across the view). The game clock may
then read a tick's scheduled time up to that slot, slightly ahead of real time.

**Mouse feel and per-frame look in vehicles** (`src/mouselook.cpp`,
`src/freecam.cpp`, `src/interp.cpp`): in play the mouse is read every drawn frame
into one running total of scaled counts (`[Camera] MouseSensitivity` percent;
`MouseEqualAxes` scales vertical counts by 2.5 sens_h / 4 sens_v so up / down
turns as fast as left / right). At its update the game gets the whole counts not
handed yet (fractions carry over). The drawn camera is turned by the counts not
handed yet: on foot as the game will (CCam mode 4), in a vehicle as FreeCam's
string-camera hook will (`FreeCamFrameTurn`, mode 18), with the latest update's
own turning (beyond its mouse part, measured from TheCamera's last two matrices)
drawn blended. The camera turns around the point of its view line nearest the
player / vehicle. `MouseSmoothing` (ms, default 0) averages the total over time
for mice that report less often than frames are drawn.

**Unpaced frames** (`FPSLimit=-1`, or vsync pacing): a frame's on-screen time is
predicted as now + the recent average time from frame start to Present, kept
separately for frames with and without a game update.

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

**Free cam on boats and RC vehicles:** boats have their own camera,
`MODE_BEHINDBOAT` (22, `CCam::Process_BehindBoat` 0x480F17; the mode switch's
jump table at 0x68B260 is indexed by mode - 1, and only mode 18 calls 0x47FE8C),
so the hooks above never ran for them. Where the camera picks the requested mode
for a vehicle (0x470C23: vehicle type 1, a boat, writes 0x16 to `ReqMode`
0x70388A at 0x470C30), boats now request 0x12 like other vehicles (the byte at
0x470C37, `[Camera] FreeCamBoats`). The RC vehicles you drive aren't the vehicle
you sit in: they are `CPlayerInfo::m_pRemoteVehicle` (Players[0] + 4, 0x94AD2C,
as `0484 GET_REMOTE_CONTROLLED_CAR` reads it), so free cam takes the remote
vehicle first, then the one you are in.

**Quick save and quick load** (`[QuickSave]`, `src/quicksave.cpp`): F5 saves the
game exactly as it is, wherever you are, into save slot 8 (set by `Slot`), named "Quick Save" in Load Game. If that slot
holds one of your own saves, it is copied to `GTAVCsf8 (before quick save).b` in
the same folder before the first quick save. F9 loads it.
The keys only act when pressed on their own: with Shift, Ctrl, Alt or Win held
(ShadowPlay's Alt+F9 recording, for example) they do nothing. The phone-skip key
works the same way.
A message on screen says what happened. Not saved while you are wanted, on a
mission or on water, nor while you are dead
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

**On water** (all three saves): in a boat, or where the water map has water
(`CWaterLevel::GetWaterLevel` 0x5C2C80) and either below the surface + 1 m, or
above it with no solid ground over the water below: a vertical line down to 2 m
under the surface through buildings and objects (`CWorld::ProcessVerticalLine`
0x4D8B00) finds only the seabed, or nothing. That catches a trainer's
walk-on-water, hovering or flying low over water and a car on it; a pier, jetty
or beach above the water level is land.

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
(CWorld::ProcessVerticalLine 0x4D8B00, buildings only) finds what they stand on.
The line has no area test (its sector loop at 0x4D84D0 only checks "uses
collision", CEntity +0x51 bit 0), so it hits the city, every interior and hidden
pieces alike. Distances are to the collision boxes of the interior's pieces,
never its stand-ins for the street (`htl_exterior03`, 8 x 58 m, reaches across the
Ocean Drive sidewalk):
- another interior's floor: switch to that interior, if within 2 m of one of its
  pieces (not from above its roof) and the hit piece isn't its stand-in. Coming
  from the city, a ceiling above the player is also needed
  (CWorld::GetIsLineOfSightClear 0x4DA560), so a sidewalk never counts.
- the city's ground, or ground drawn in every area (area 13: Ocean Drive's road
  `oceanrda04`-`06` and the whole beach east of the Ocean View, `miamiland_kb02`-
  `04`, `odhighsandgrs1`, `odlowsand1`/`2`), while an interior is set: switch to
  the city once more than 1.5 m outside the interior's pieces (pieces over 150 m
  left out, except a roof). Before, area 13 meant "leave it": flying from the
  Ocean View over Ocean Drive kept area 1 until more than 60 m up.
- the current interior's own piece: switch to the city once more than 4 m from
  all of its pieces, the big ones included (the mall's roof and black box, the
  stadium's stands): above its roof.
- nothing (in the air, over water, high up) while an interior is set: switch to
  the city once more than 3 m from the interior's pieces.

A room's pieces cover its floor to within 0.85 m (Little Havana's cafe, area 7,
stands on its own area-13 floor `cof_intfloor`); the gaps between the 1.5/3/4 m
"out" and 2 m "in" distances keep the area from flipping on a room's edge.

The new area must hold for half a second while the player has control. Then it
is applied the way the door scripts do it: set the area, unload the other area's
models (0x40DAA0), load the scene around the player (CStreaming::LoadScene
0x40AF60). Going back to the city also turns off interior lighting and restores
normal traffic, unless you're on a mission. `Interiors:` lines in the log list
each switch with the rule that decided it, what the line hit below, the nearest
piece of the room and its distance, and whether the room was entered by
AutoSwitch or set by the game: `area 1 -> 0 at x y z (scene loaded): out of the
room, over ground drawn in every area (below: oceanrda05_dy, area 13; nearest
piece of area 1: ht_upstairs, 25.4 m); the room was entered by AutoSwitch`.
Doors keep working through the game's own scripts.

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
  list, and a mesh already reduced to one vertex is never taken. A model loaded
  again (streamed out and back in; seen with `man_doorway2` every street-view check,
  250 ms) comes back closed at a new address: it is opened again in the same list
  entry (same building and mesh), not a new one, and only counted in the log.
  Log lines: `showing area N from the street`, `opened the doorway of ...`,
  `... was loaded again; its doorway opened again (N times so far)`.
- **Interiors laid over the city** (`OverCity`): area 10, the Ammu-Nation shooting
  range (`hogans_alley_b`, `hogans_alley`), isn't inside its own building: it covers
  the downtown street and crossing in front of the shop (x -697..-645, y 1164..1299)
  with a plain floor at street level. It is never shown from the street, the city
  is never drawn around it, and AutoSwitch never switches into it from the city
  (otherwise its floor covered the road there).
- **The mansion's copy of its garden:** area 2 holds `man_backside_int`, a
  131 x 46 m copy of the mansion's back garden and terrace for the view from its
  windows, with its lawn 0.7 m above the real one (`man_grnd2`). It counts as a
  stand-in (`IsStandIn`), so it is hidden whenever the city is drawn with area 2;
  before, it floated over the lawn at knee height (cutting through people's legs).
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
- **Room walls before the plants in the room:** some rooms keep their walls in a
  model drawn last (IDE flag 4), like their plants: the Ocean View lobby's
  `ht_mainfloor_dy` (flags 164, collision box 21 x 26 x 5 m around the lobby) and
  its `ht_veg04_dy` yuccas, `ht_veg01_dy` trees and `ht_veg02_dy` planters (flags
  36). These go into the alpha list, sorted by the distance from the camera
  (`CRenderer::ms_vecCameraPosition`, 0x975398) to each entity's position and drawn
  far to near. The plant groups' positions are a few metres from the lobby's, so
  from behind the player at the counter the yuccas were drawn before the walls.
  Their DXT3 leaf cards write depth over half-clear texels, the wall then failed
  the depth test there, and the street drawn earlier with the solid world showed
  through the wall. While you are in an interior, a piece of it whose collision box
  holds the camera is sorted as further than anything else (100000 + its box
  volume, so the biggest goes first). The five calls to
  `InsertEntityIntoSortedList` (0x581440) that only sort (0x4C7BA1, 0x4C80A8,
  0x4C819C, 0x4C8388, 0x4CA0F6) go through a wrapper. The four for entities fading
  in (0x4C7B78, 0x4C7C6A, 0x4C8355, 0x4C84AB, which set bit 7 of +0x54) are left
  alone, because their distance also sets the fade. Log line per room visit:
  `area N: <model> (V m3 around the camera) drawn before the plants, glass and
  other see-through things in it`.
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
  0x4C85AD, `mov eax,[count]; inc [count]; mov [list+eax*4],reg`) call a stub. With
  twice the detail distance, downtown filled all 2000 entries and everything after
  was dropped (a road piece next to the player included: the scan isn't nearest
  first). The list is moved to a 16000-entry one in the DLL: the stubs add to it
  (stopping at 15990, logged if ever reached) and its three reads (0x4C9F83,
  0x4CA1BB, 0x4CA203, `mov reg,[list+idx*4]`) point at it.
- The see-through list: entities whose model is drawn last (IDE flag 4: trees,
  palms, fences) go into `CVisibilityPlugins`' distance-sorted alpha entity list
  (0xA0FD84), made with 200 nodes in `InitAlphaLists` (`push 0C8h` at 0x5828DB).
  When it is full, `InsertEntityIntoSortedList` (0x581440) returns false and the
  entity is drawn unsorted with the solid ones. A near palm drawn that way before
  the one behind it writes depth over the clear parts of its leaf cards, so the far
  palm's leaves are cut out in a box of sky ("floating" leaves). Seen in the game
  too, much more often with longer draw distances. The push becomes 1000, patched
  in DllMain before the lists are made (the bytes are checked first). FixedTick's
  snapshots of the alpha lists (list header + node array, saved and put back
  around redraw frames) copy that many nodes too: restoring only 200 left the
  header pointing at nodes past them that were not put back, and
  `CLinkList::InsertSorted` (0x582BD0) crashed at 0x582C12 on the broken free list.
- Two-pass see-through drawing (`alphafix.cpp`, [Graphics] TwoPassAlpha). The
  sorting above is per entity; inside one palm the leaf cards are still drawn in
  model order with blending and depth writes on, so the clear parts of a near card
  write depth and hide the leaves behind it: pale boxes in the crown (also in the
  original game). DrawPrimitive / DrawIndexedPrimitive (device vtable 70, 71) are
  hooked: a draw with ALPHABLENDENABLE, ZENABLE and ZWRITEENABLE on is drawn twice,
  first alpha test GREATEREQUAL 0xC0 with depth writes (the solid leaves), then
  alpha test LESS 0xC0 without depth writes (the soft edges). The game creates a
  pure device (no GetRenderState), so SetRenderState (vtable 50) is hooked to keep
  the states set (Direct3D's defaults again after Reset); they are put back as they
  were after the two passes, so RenderWare's state cache stays right. Draws that
  only blend (particles, HUD, glass with depth writes off) are untouched, and so is
  the water: it is see-through with depth writes on too, mostly below 0xC0, and
  split its layers (surface, wave mask) stacked without depth and looked wrong,
  worst looking out to sea. RenderScene draws the water in two calls, both through
  a wrapper that turns the two passes off while they run (each chains to whatever
  was called there, another mod's hook included): `CWaterLevel::RenderWater`
  (0x4A6594 -> 0x5C1710, the sea sectors) and, after the opaque world, the
  see-through wavy layer near the camera (0x4A65AE -> 0x5BFF00). Wrapping only the
  first left the near layer split, which made the shoreline jump up and down the
  sand from one frame to the next (measured from a 60 fps recording: 508, 491,
  453, 453, 468, 457 px in consecutive frames with the camera still).
- The far clip: `Idle` gives the RenderWare camera the weather's far clip
  (`CTimeCycle::m_fCurrentFarClip`, 0x9B6A6C) each frame, calling
  `RwCameraSetFarClipPlane` (0x64A930) at 0x4A6032; it gets that times `FarClip`.
  The weather's own value is untouched.
- Anisotropic filtering (`[Graphics] Anisotropic`, `src/dllmain.cpp`):
  `IDirect3DDevice8::SetTextureStageState` (vtable 63) is hooked; where RenderWare
  asks for linear minification, anisotropic is used at the level set (capped at
  the card's `MaxAnisotropy`). Not for the water: its wave textures are made to blur
  at a shallow angle, as in the original game. Around both water calls (0x4A6594 and 0x4A65AE go through the
  wrappers in `alphafix.cpp`, installed even with TwoPassAlpha off)
  `WaterDrawing` sets every stage RenderWare asked to be linear back to linear,
  and anisotropic again after (RenderWare caches each stage's filter and doesn't
  set it again, so the hook alone can't).
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
save only, the player is put on foot, in the city (area 0), at the nearest
safehouse you own, and put back right after. Owned safehouses are the ones whose
save script (PSAVE1-10) is running (`CTheScripts::pActiveScripts`, 0x975338).
The spot is where that script itself stands the player for a save, from its
`0055 SET_PLAYER_COORDINATES` and `0171 SET_PLAYER_HEADING` in main.scm (PSAVE2-10
just before their save gosub; PSAVE1 the spot after its save, 0x10929), 3-16 m
from the save pickup. Saved on the pickup itself, the safehouse would ask to
save again as soon as the game loads. The height is the script's ground height
plus 1 m, as `SET_PLAYER_COORDINATES` places a ped's centre. No time passes,
unlike a safehouse save.
The help box shows "Game auto saved...".

**Air control** (`[Vehicles] AirControl`, `src/aircontrol.cpp`)
Before each game update, a car the player drives (vehicle type 0 at +0x29C, status
player or RC) that has had no wheel on the ground for 0.15 s (CAutomobile +0x5C4,
counted by `ProcessControl` at 0x595C44) is turned by the helicopter inputs:
pitch about its right axis is read as `CVehicle::FlyingControl` reads it for
helicopters (0x5B5AB0..0x5B5B41): `CPad::GetCarGunUpDown` (0x4AAF00; right stick,
Num8 / Num5 by default), negated, when it is beyond 1, else `GetSteeringUpDown`
(0x4AAF50); roll about its forward axis likewise from `GetCarGunLeftRight` (0x4AAEB0;
turret left / right, Num4 / Num6 by default) when beyond 1, else
`GetSteeringLeftRight` (0x4AAFD0). The part of
`m_vecTurnSpeed` (+0x7C, rad per 1/50 s, world space) along that axis moves toward
input x 0.024 (about 1.2 rad/s) by 0.0008 per time step (`CTimer::ms_fTimeStep`;
full rate after about 0.6 s), both times `AirControlStrength` / 100 and times
sqrt(1500 / `m_fMass` (+0xB8)) kept within 0.5-1.25, so heavy vehicles turn
slower; with no input the car keeps its spin. Not
on water (`CWaterLevel::GetWaterLevel` + 2 m), not resting on its roof or side
(ground within 1.3 m below by `CWorld::ProcessVerticalLine` while moving less than
1 m/s up or down), not in helicopters (handling flag 0x20000), the RC Baron
(model 194) or with the flying-car cheats (0xA10B28, 0xA10AE5), which already fly
with these inputs; not while controls are off, in menus, cutscenes or replays.

**Heavier helicopters** (`[Vehicles] HeliWeight`, `src/aircontrol.cpp`)
Before each update, for the player's helicopter (handling flag 0x20000, status
player, not the RC Raider 195 / RC Goblin 231), the change the last update made to
`m_vecTurnSpeed` (+0x7C) is cut to 1 / (1 + 1.2 w) and to the horizontal part of
`m_vecMoveSpeed` (+0x70) to 1 / (1 + 0.7 w), w = HeliWeight / 100: the same as
a turn mass and mass that much larger. Thrust and drag are cut alike, so top speed
is unchanged; vertical speed is left alone, so lift against gravity (hover) is
too. Not after an update with a collision (`m_nCollisionRecords` +0xE6,
`m_fDamageImpulse` +0x104, both cleared by `CAutomobile::ProcessControl` at
0x594F78) or a wheel on the ground (+0x5C4), which keep the game's result.

**Helicopter controls** (`[Vehicles] HeliControls`, `HeliMaxPitch`, `HeliMaxRoll`, `src/aircontrol.cpp`)
The game's helicopters are flight model 6 in `CVehicle::FlyingControl` (0x5B54C0,
jump table 0x69C894, helicopter part 0x5B69A5, ebx = vehicle). It reads pitch into
[esp+24h] (steer up / down, or `GetCarGunUpDown` negated when pressed), roll into
[esp+28h] and yaw into [esp+2Ch]; with the default scheme (byte 0xA10B0D clear)
roll is steer left / right negated and yaw is look right (+1) / look left (-1),
or turret left / right (`GetCarGunLeftRight` 0x4AAEB0) when pressed. Both
schemes meet at 0x5B703F (push 0; call `CPad::GetPad`), patched with a jump to a
stub that, only while ebx is `g_heliCtlVeh` (the player's real helicopter, set
before each update), writes roll = -turret left / right / 128, yaw = steer left /
right / 128 (look keys when not steering), and multiplies the pitch by
`g_heliPitchScale`. Input that tilts further fades from 70% of the limit to 0 at
it (pitch from forward.z, roll from right.z); past the limit, in the air, turn
speed about that axis gets 0.0006 per degree back, and 15% of any outward spin
removed, per time step.

**Plane controls** (`[Vehicles] PlaneControls`, `src/aircontrol.cpp`): the plane part
of `CVehicle::FlyingControl` (flight models 1, 3, 4, 5 via the jump table at
0x69C894: RC Baron, Skimmer; code from 0x5B5A78) reads steer left / right into
`[esp+10h]` and uses it for both yaw (0x5B60C5, x flying handling +0x0C, fYaw) and
roll (0x5B61BD / 0x5B61CF, x +0x18, fRoll). Stubs at 0x5B60C2 and 0x5B613A put steer
left / right there before the yaw and turret left / right (GetCarGunLeftRight / 128)
before the roll, for the player's (or remote-controlled) plane only. Pitch is unchanged (steer
up / down, or special up / down when pressed). The Dodo (flight model 0) has its own
code and is left alone.

**Hunter weapons** (`[Vehicles] HunterWeapons`, `HunterFireKey`, `src/hunter.cpp`)
`CAutomobile::ProcessControl` fires the Hunter's (model 155, player) rockets when
CPad 0x4AA9F0 (called at 0x597F6F) says so, 350 ms apart, else its cannon on CPad
0x4AA9B0 (at 0x5981EA), 60 ms apart. Both calls are redirected: they also answer
yes while the fire key is held and that weapon is picked. The helicopter code's
handbrake read (CPad 0x4AAB60 at 0x5B7049) answers no while the key fires. The
wheel: `CPad::UpdateMouse` takes DirectInput's wheel value at 0x4AD934 (mov eax,
[esp+14h]; test eax,eax), now a call into a stub that, while the player is in the
Hunter, adds it to `g_wheelDelta` and gives the game 0 (so the radio doesn't
change); the next update switches the weapon and `CHud::SetHelpMessage` names it.
While the fire key (picked weapon), the left mouse button (rockets) or the right
one (cannon; `NewMouseControllerState` 0x94D788 / 0x94D789) is held, the two fire
reads answer from those alone, the game's own fire inputs not asked. One rocket
per shot: the game's shot calls `CWeapon::FireProjectile` (0x5CCF90, thiscall,
3 arguments) twice, at 0x5980B0 (left pod) and 0x5981D0 (right pod); for the
player's Hunter only one of them goes through, alternating. Nothing
reacts while the game menu or a trainer window is open: the call to
`CPad::UpdatePads` at 0x4A4412 (which Modern VC Trainer hooks; this hook calls
it) sets the padding byte 0x7DD867 of the raw mouse state first - the game never
writes it - and a trainer menu blanks all 16 bytes after the pad update, so the
byte found cleared means a window has the input; the wheel and keys are then left
to it.

**Trainer window cursor** (`TrainerWindowFrame`, `src/hunter.cpp`): Modern VC
Trainer moves its window's mouse cursor in its `CPad::UpdatePads` hook (by that
update's mouse movement, then it blanks the game's pads and mouse), so the cursor -
and dragging, sliders, scrolling - moved 30 times a second, once per game update,
however fast the window was drawn. While a trainer window is open, FixedTick's
`Redraw` runs that pad update (the hook above, so the trainer's hook and the game's
update) at the start of each redraw frame, after saving the redraw state and before
anything draws: the cursor gets the mouse movement every frame. Each extra update
reads DirectInput itself (the game's read, `GameRead` in mouselook.cpp), so it gets
the movement since the last read. The game sees no input from it (the trainer blanks
the pads after reading, as on every update while the window is open); the pad
memory isn't in the redraw state, so the extra updates stay, which is harmless while
the pads are blanked. The trainer is unchanged.

**No island loading screen** (`[Streaming] NoIslandScreen`, `src/faststream.cpp`)
Crossing to the other island, CStreaming's level change (0x40DFB0, run from
`CStreaming::Update`) stops the clock and calls `LoadingIslandScreen` (0x4A68A0:
load the splash 0x4A6E80, draw and present it 0x4A72C0) twice, at 0x40E00E and
0x40E01C, before loading the new island. Those two calls (its only callers) are
replaced with 5-byte nops (push / pop around them stay balanced); the loading
is unchanged.

**Faster streaming** (`[Streaming] FastLoadMs`, `src/faststream.cpp`)
`CStreaming::Update` (0x40EE80) runs one step of the loader per game update
(inlined at 0x40EF43): finish the current channel's read (`ProcessLoadingChannel`
0x40BEB0, which first asks `CdStreamGetStatus` 0x408480 and returns if the read
isn't done), start the next read if it is idle (`RequestModelStream` 0x40B8A0),
switch channel (0x6F7754). Before each update the same step runs again, yielding
to the CdStream thread between steps, for up to FastLoadMs while a channel has a
read in progress or models are requested (`ms_numModelsRequested` 0x975354),
stopping as soon as a step leaves both channels idle. With gta3.img held in RAM
(PreloadFiles) a read finishes in well under a millisecond, so several batches
arrive per update instead of one every two.

**Missing characters in GET_CHAR_COORDINATES** (`src/missionhelp.cpp`): the 00A0
handler (0x449B8D) reads the character `CPools::GetPedAt` (0x451CB0) returns
without a null check; a street ped a script kept (vigilante criminals) can be
removed by the game, and 0x449BAD then read address 0x3AC. That instruction is
now a jump to a stub that, for null, uses a blank 1 KB stand-in (in no vehicle,
position 0,0,0) and counts it (logged); the script's IS_CHAR_DEAD checks treat
the missing character as dead.

**Rampages** (`src/missionhelp.cpp`): a rampage isn't a mission - its pickup's
script runs START_KILL_FRENZY (01F9, `CDarkel::StartFrenzy` 0x429B60) and
`CDarkel::Update` (0x42A650) runs the timer; `CDarkel::Status` (0xA10A72) is 0 none,
1 running, 2 passed, 3 failed. The End key (3 s) calls
`CDarkel::ResetOnPlayerDeath` (0x429F90), which fails a running rampage as dying
does (status 3, weapons given back); the pickup's script then shows RAMPAGE
FAILED. The restart prompt records the player's spot when the status turns 1
and offers it when it goes from 1 to 3 (the same restart script puts the player
back on the pickup); a passed rampage (2) isn't offered.

**Mouse aim for the Rhino and fire truck** (`[Vehicles] MouseAim`, `src/gameplay.cpp`)
The turret (`CAutomobile::TankControl`) and the water cannon (`FireTruckControl`)
turn with the numpad "car gun" keys, by changing `m_fCarGunLR` (+0x5B0) and
`m_fCarGunUD` (+0x5B4, cannon only). Before each game update they are turned toward
where the camera looks (TheCamera's forward, 0x7E4698), which FreeCam moves with
the mouse; the turret turns at most 2.5 rad/s, the cannon 5 rad/s. In these two
vehicles FreeCam doesn't swing the camera back behind while you drive, so the gun
stays where you aimed.

**Physicals past the map edge** (`src/gameplay.cpp`, always on)
Moving entities are linked into every world sector their bounding box covers:
sector 0x792D30 + (y * 80 + x) * 40, from X * 0.02 + 48 and Y * 0.02 + 40 for the
box's corners and centre. Two copies of that code, `CPhysical::Add` (0x4BB310,
through the vtable) and the per-update re-file (0x4BAEE0), never clamp the numbers
to the 80 x 80 grid, unlike `CEntity::Add` and the other sector code. A vehicle
past the map edge was linked into memory past the grid (0x7D1530 on): a list head
there held the text "ARMO" (write at 0x4BB53E), and lists ended up with empty
nodes (0x4B119B, 0x4D6EE9). Where the last of the six numbers is worked out
(0x4BB46D `mov eax,[esp+28h]; cmp eax,[esp+20h]` and 0x4BB043 `mov eax,[esp+30h];
cmp eax,[esp+28h]`) stubs clamp all six to 0-79 and count the clamps (logged).

**Empty sector entries** (`src/gameplay.cpp`, always on)
A list node with no entity crashed the collision test
(`CPhysical::ProcessCollisionSectorList` 0x4B1070, at 0x4B119B `mov cl,[ecx+51h]`)
and the line-of-sight test (0x4D6EC0, at 0x4D6EE9 `mov cx,[eax+58h]`). Both reads
jump to stubs that skip such a node (on to the next-node tests at 0x4B38BB and
0x4D6F40) and count it; the count is logged (first, then every 100).

**Line of sight off the map** (`src/gameplay.cpp`, always on)
`CWorld::GetIsLineOfSightClear` (0x4DA560, 84 callers: scripts, the camera, peds)
steps from sector to sector along the line without clamping the sector numbers,
so a line ending off the map read lists past the grid (garbage node, crash at
0x4D6FE7). All 17 of its sector tests call 0x4D6EC0 (cdecl, sector address first);
its entry jumps to a stub that answers "clear" for an address outside
0x792D30-0x7D1530 without reading it (counted, logged every 1000).

**Invalid positions** (`src/gameplay.cpp`, always on)
After leaving the mall onto its roof the camera's target got a NaN position: every
sector number became 0x80000000 (the clamps above counted thousands) and
`CWaterLevel::GetWaterLevel` (0x5C2C80) read its 128 x 128 table (0x864C70) far out
of range (0x5C2D01). Before each update every ped, vehicle and object (pools
0x97F2AC / 0xA0FDE4 / 0x94DBE0) is checked: the matrix's right, forward, up and
position and the move / turn speeds (+0x70, +0x7C) must be finite and within 1e6.
The last good matrix is kept per pool slot; a bad entity gets it back (or, with none
known, upright at the player), its speeds are zeroed, `CEntity::UpdateRwFrame`
(0x489360) is called, and it is logged with its pool and model. The active CCam's
Front / Source / Up (+0x168 / +0x174 / +0x18C) are put back behind its target if
they went bad. The water lookup's cell numbers are also clamped to 0-127 (stub at
0x5C2CF8).

**Riot cleared on load** (`[Fixes] ClearRiotOnLoad`, `src/gameplay.cpp`)
The riot cheat doesn't set a switch: it rewrites the ped types' hostility.
`CPedType::ms_apPedType` (0xA0DA64, 23 types from ped.dat) holds each type's flag
(+0), threats (+0x18) and avoid list (+0x1C), and the save keeps the whole table
(`CPedType::Load` 0x530340, called at 0x61C362), so a saved riot returns with every
load. Each load reads ped.dat first (0x4A4850, then `GenericLoad` 0x61A700), so the
defaults are copied before the save is applied; afterwards every type but the
players gets its default threats and avoid list back, and gangs keep only their
hostility to PLAYER1 (bit of type 0) from the save, which missions set. (Keeping
all of a gang's gang bits, as a first version did, left a riot's 0xFFFFF as every
gang hating every gang, its own included.) Changes are logged.

**Mission leftovers** (`[Fixes] MissionLeftovers`, `src/gameplay.cpp`)
"Martha's Mug Shot" (PORNO3) swaps the Ocean Drive hotel `od_chariot` (3700) for
`od_wtsign` (3568), and its LOD `LODchariot` (3733) for `LODd_buildnew` (3661),
while you are on its roof, and swaps them back afterwards. If the mission ends
without that, the swap stays in `CTheScripts::BuildingSwapArray` (0x94DBE8) and
every save keeps it: the hotel shows without its walls. Off a mission such an
entry is undone like the game's `UndoBuildingSwaps` (0x458CC0) does it
(`CBuilding::ReplaceWithNewModel`, 0x407DB0), and the mission's flag `$4487` reset.

**Scripted scene skip** (`[Missions] IntroSkip`, `src/missionhelp.cpp`)
Many mission intros (the RC missions, among others) are not CCutsceneMgr
cutscenes but scenes the mission script stages: player control off, widescreen on
(TheCamera +0x6D, 0x7E46F5, set by `SetWideScreenOn` 0x46B875), script cameras,
subtitles and speech, each step held by a WAIT. A scene counts while on a mission
with widescreen on, player controls off (CPad +0xF0) and no real cutscene
(`CCutsceneMgr::ms_running`, 0xA10AB2). Holding the key 3 s starts the skip: before
each update the wake time of every mission script (CRunningScript +0x7A set; wake
time +0x7C, compared with the game clock at 0x44FDA6) is cleared, so every WAIT
ends at the next update, and its TIMERA / TIMERB (locals 16 and 17, +0x70 / +0x74,
which `CTheScripts::Process` advances at 0x45023F) jump 10 s ahead. Most intros
wait on those timers rather than on WAIT: the RC missions (e.g. `RCHELI`, from
0x12EF31) do `TIMERA = 0`, then loop `WAIT 0` until `TIMERA > 5000` for every
step. Mission speech is not played and reports finished (the
phone-skip hooks on 0x45ABD7 / 0x45AC04, installed when either feature is on); and
subtitles are cleared (0x5841E0). The script still runs every command in order.
The skip stops when the scene ends, a real cutscene starts, the mission ends, or
after 60 s.

**Stuck missions** (`[Missions] SoftlockSeconds`, `EndMissionKey`, same file)
On a mission with no player control for `SoftlockSeconds` of game time (outside
real cutscenes, phone calls, the scene skip, and while wasted or busted), a
subtitle warns (`CMessages::AddMessageJumpQ`, 0x584300, as PRINT_NOW), and 15 s
later the mission is ended. Holding `EndMissionKey` 3 s ends it at once. Ending
is `CRunningScript::DoDeatharrestCheck` (0x460D00) without the death: the mission
script's gosub stack (+0x14, depth +0x2C) is unwound to stack[0] into the ip
(+0x10), "deatharrest executed" (+0x84) is set, the on-mission variable (script
space + [0x978748]) is cleared, the wake time zeroed and 0x584130 called. The
mission script then runs its own failed and clean-up code. Two seconds later the
leftovers are put back: `CPlayerInfo::MakePlayerSafe(false)` (0x4BBC10 on
Players[0], 0x94AD28; what SET_PLAYER_CONTROL on calls), `SetWideScreenOff`
(0x46B863), `CCamera::Restore` (0x46BC7D), `CCamera::Fade(1.0, 1)` (fade in,
0x46FBCB) and time scale 1.0.

**Restart prompt** (`[Missions] RestartPrompt`, same file)
Missions are started by their contact's script with `0417 START_MISSION` (one
parameter, the mission number; the handler at 0x608C1F loads it from main.scm).
The `ProcessOneCommand` hook (`src/stuntjump.cpp`, installed when either feature
is on) calls `MissionLaunching` when the next command is 0417: the player's
position and heading (or their vehicle's, with its model, +0x5C), the drawn area
(0x978810), the player's area (+0x5F) and `CStats::LastMissionPassedName`
(0x7E9D78) are recorded; the mission script's name (+8) is read while it runs.
When the on-mission flag drops without that name changing and the mission isn't
a side job (`taxi`, `ambulan`, `firetru`, `copcar`, `pizza`) or a property
purchase (a script name with `buy`: `lnkvbuy`, `ochebuy`, `washbuy`, `carbuy`...,
which end without changing the last-passed name), the help box asks
(`CHud::SetHelpMessage` 0x55BFC0, forever; cleared with null like `03E6
CLEAR_HELP`) once the player is alive, in control and not wasted/busted.
Y starts a small script the way `004F START_NEW_SCRIPT` does (free list 0xA0FDDC,
`CRunningScript::Init` 0x450CF0, ip set, linked at the head of the active list
0x975338, +0x78 active). Its ip is the offset of a buffer in the DLL from the
script space (0x821280); the code is game commands with literal parameters:
`03CB LOAD_SCENE`; on foot `00A1 SET_CHAR_COORDINATES` (or `0362
WARP_CHAR_FROM_CAR_TO_COORD` from a vehicle) and `0173 SET_CHAR_HEADING`; for a
vehicle start `036A WARP_CHAR_INTO_CAR` into a same-model vehicle within 15 m,
or `0247 REQUEST_MODEL`, `038B LOAD_ALL_MODELS_NOW`, `00A5 CREATE_CAR` (into
local 0), `036A`, `01C3`, `0249`, then `00A1` (which moves the vehicle the
character is in) and `0175 SET_CAR_HEADING`; finally `0373
SET_CAMERA_BEHIND_PLAYER` and `004E TERMINATE_THIS_SCRIPT`. Handles are
`slot << 8 | flag byte` (ped pool 0x97F2AC, 0x6D8 a slot; vehicle pool 0xA0FDE4,
0x5DC a slot), as `GetAt` (0x451CB0, 0x451C70) checks them. The script runs and
ends within the same update. The contact's script then sees the player in its
marker and starts the mission again.

**Unique stunt jumps** (`[StuntJumps] NormalCamera`, `src/stuntjump.cpp`)
Vice City's unique stunt jumps are not exe code: a `main.scm` script named `usj`
watches every jump's start box. When the player's vehicle enters one it runs
(v1.0 offsets): `015D SET_TIME_SCALE 0.3` (0x14C9C), `015F
SET_FIXED_CAMERA_POSITION $0D00 $0D04 $0D08 0 0 0` (0x14CA3), `0158
CAMERA_ON_VEHICLE $0C54 15 2` (fixed mode, jump cut; 0x14CBD), then loops while
the vehicle is in the air and checks the landing box (`$0C68`), waits 300 ms,
`015D SET_TIME_SCALE 1.0` (0x14DBB) and `02EB RESTORE_CAMERA_JUMPCUT` (0x14DC2),
and pays the reward. Scripts run one command at a time through
`CRunningScript::ProcessOneCommand` (0x44FBE0, thiscall, `al` = 0 to carry on),
called by `CRunningScript::Process` at 0x44FDB5; `m_nIp` (+0x10) is an offset
into the script space (0x821280), the name is at +8. That call is redirected:
while the `usj` script is about to run one of those four opcodes, the command is
stepped over (its parameters skipped by their type bytes: int32/float 5,
global/local/int16 3, int8 2) instead of run. Detection, landing and reward run
unchanged at normal speed, with the game's own chase camera. In v1.0 those five
positions are the only such commands in the script; a command whose parameters
have another form is run normally. Log: `StuntJump: skipped ...`.

**Ped sight crash** (always on, `src/gameplay.cpp`)
`CPed::OurPedCanSeeThisOne` (0x50D360, used by scripts) looks at the peds in the
world sectors between two peds, but doesn't keep the sector numbers inside the
80 x 80 grid (0x792D48) the way the game's other sector scans do. A ped outside the
map, or a bad position, makes it read past the grid and crash at 0x50D660. A stub
at 0x50D60E clamps the four numbers to 0-79. (0x50D60E is in `CPed::BuildPedLists`,
0x50D4B0, the sector scan below.)

**Nearby-ped list** (always on, `src/gameplay.cpp`, `InstallNearPedList`)
`CPed::BuildPedLists` (0x50D4B0; from `CPed::ProcessControl` 0x505814 and 0x531D68,
for each ped once every 16 frames) collects every ped in the sectors around a ped
into a static list of 50 pointers at 0x7D5230 (length: word 0xA10A3E), sorts it by
distance (`SortPeds` 0x50D120) and keeps the 10 nearest (ped +0x56C). It never
checks the length (stores at 0x50D6D2 / 0x50D6E7, `inc` at 0x50D6EE, end marker at
0x50D735). With more than 49 peds around (a trainer crowd) the list ran over the
next globals: four CCarCtrl dwords (0x7D52F8), the time cycle's "light on ground"
table (0x7D5308, 24 x 7 bytes; mixed into 0x9B6A70, the brightness of lamps, traffic
lights and their coronas), the time cycle's object ambient red (0x7D53B0, mixed into
0xA0D8CC: the red of the ambient light on every ped, vehicle and object), two
20-float random tables (0x7D5458, 0x7D54A8 for particles) and the start of
CRenderer's visible list (0x7D54F8). Each list wrote other pointer bytes there, so
the scene washed red, pink or orange and lamps changed colour from update to update.
All five references to 0x7D5230 now point to a 512-place list in VCModernFix, and
the `inc` goes through a stub that stops at 511. Log: `Peds: nearby-ped list moved
...` at start, then every 10 s while it acts `Peds: nearby-ped lists longer than the
game's 50 places: N so far (longest L, peds left out at 511: K)`.

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
groups are built (log, one line per animation: `Weapons: <name>: legs freed, Root turn
(max N deg, K Root keyframes) put on Spine1 each update (M Spine1 keyframes)`).

The torso turn. Those fire animations are authored on a body that swings: the Root
track turns the ped about the vertical by up to 48 degrees from the armed stance they
start from (RIFLE_fire / M60_fire -47, shotgun_fire -48, UZI_fire -45, buddy_fire -38,
buddy_fireRELOAD -10, FLAME_fire +27, python_fire +21; minus = to the right), and
Spine, Spine1 and the arms are posed on top of it so the gun lies along the aim. With
Root dropped the gun pointed off to the side by that turn (bullets were fine: they
never come from the bones). The turn can't be baked into the keyframes at load: it must
be measured against the Root * Pelvis the walk/run/sprint/idle gives at that moment,
which differs from the armed idle by 10-60 degrees and swings with the stride (a fold
against the armed idle still left 29-64 degrees while moving). So both calls of
`RpAnimBlendClumpUpdateAnimations` (0x404690) in CWorld::Process (0x4D75BF, 0x4D769F;
args clump, dt, doRender) are chained, and after an update with doRender each
association on the clump (clump + [0x978798] -> CAnimBlendClumpData: +0 first link,
+8 bone count, +0x10 frame data, 0x18 bytes each, hanim keyframe +0x10, bone tag
+0x14; association = link - 4: +0x0C node count, +0x10 nodes (0x1C bytes, sequence
+0x14), +0x14 hierarchy, +0x18 blend, +0x20 time) that plays one of these animations
with no node linked to its Root track gets: F = fire Root * Pelvis sampled at its
time (the dropped tracks stay in the hierarchy), C = Root * Pelvis of the pose,
yaw = twist of F * C^-1 about Z times the blend (limited to 90 degrees), and
Spine1 := (C * Spine)^-1 * Rz(yaw) * (C * Spine) * Spine1 in the hanim keyframe (+8).
Spine1 (tag 3) because the thighs hang from Spine (player.dff: Root > Pelvis > Spine >
{Spine1 > Neck > head and arms, L Thigh, R Thigh}). Quaternions in memory are x y z w
with x y z negated from the file (LoadAnimFile 0x404F32), equal to the bind rotations,
and world = parent * local. Keyframes are floats at sequence +0x28 (0x20 bytes with
translation, else 0x14; time at +0x10 as deltas after CalcTotalTime 0x401DF0); the
"compressed" path (+0x2C) is never taken (its flag is cleared at 0x404C32), so the data
is only read. A block loaded with run and gun off keeps its Root linked and is left
alone. Log at first use per animation: `Weapons: <name>: torso turned N deg on Spine1`.

Also: firing while sprinting (the fire check skips PEDMOVE_SPRINT at 0x534B18, and
`CPed::Attack` breaks off on the sprint key at 0x52BDE0); HEAVY (0x200 in
`CWeaponInfo` +0x5C, 0x782A14) is cleared on the rocket launcher and minigun so
they can sprint and jump; and the aim button no longer sends the M4, Ruger or M60
into the first-person scope (checks at 0x5349DB, 0x5349E0, 0x5349E5), which held
the player still. Other peds share these animations, so they can move while firing too.

**Free drive-bys** (`[Weapons] FreeDriveBy`, `DriveByKey`, `src/driveby.cpp`)
`DoDriveByShootings` for cars (0x5C97B0), bikes (0x5C91E0) and boats (0x5C9540) run
each update for the player's vehicle: they need a slot-5 weapon (the SMGs), take
"looking left / right" from the camera's look state (Q / E), play the arm-out
animation and, with the in-car fire button (CPad 0x4AAA60), call
`CWeapon::FireFromCar` (0x5D44E0) -> `FireInstantHitFromCar` (0x5CB0A0), which fires
straight out of the window through `DoDriveByAutoAiming` (0x5CA400). Changed:
- drive-by mode: the key (Alt) switches it on release, unless Tab, Enter, F4, Esc
  or Space went down with it (Alt+Tab etc.). It goes off when you leave the vehicle,
  a mission turns drive-bys off (`CPlayerInfo` +0x147) or no gun is left;
- the look step (from `movzx eax, byte [0x7E46FE]` at 0x5C982C / 0x5C925E / 0x5C95BC
  to 0x5C9893 / 0x5C92CB / 0x5C9623) is replaced: Q / E as before; in drive-by mode
  otherwise the side the camera looks at (bikes: no side within 50 degrees of
  ahead, so the bike's own forward shot);
- the slot check's `GetWeaponInfo` (0x5C97FC / 0x5C922C / 0x5C958C) answers slot 5
  for slots 3 and 6 too (pistols, M4, Ruger), and `RemoveWeaponWhenEnteringVehicle`
  (0x4FF6A0, called at 0x4EFB96 / 0x5002D3 / 0x51A3E8) keeps the gun in hand if it is
  one of those, else SMG, rifle, pistol (the weapon held before is stored at ped
  +0x4F8, so getting out gives it back as before);
- in drive-by mode the auto-aim call (0x5CB960) is replaced: a line test from the
  camera (position 0x7E46B8, forward 0x7E4698) along its centre line, starting where
  it passes the vehicle (`CWorld::ProcessLineOfSight` 0x4D92D0, `pIgnoreEntity` =
  the vehicle), gives the aim point; the target is set on the line from the window
  through it, at the weapon's range, with a little spread;
- the fire reads (0x5C99B6 / 0x5C9474 / 0x5C970F) wait for the gun's own pace
  (Colt 300 ms, Python 700, M4 110, Ruger 130; SMGs the game's 70);
- in drive-by mode the mouse wheel (the Hunter's wheel stub at 0x4AD934, installed
  for this even with `HunterWeapons=0`) goes through pistol, SMG and rifle
  (`CPed::SetCurrentWeapon` 0x4FF8E0) instead of the radio;
- after `CHud::Draw` (0x557320, call at 0x4A64D0) the game's crosshair sprite
  (siteM16, `CHud::Sprites` 0xA10744 + 64 * 4) is drawn in the screen centre with
  `CSprite2d::Draw` (0x5786A0), a little smaller than `CHud::Draw` draws it for
  SMGs and pistols (32 * 0.3 at 640 x 448, the game's 32 * 0.4).
Not for the Rhino, fire truck, Predator or Hunter, helicopter / plane handling
(flags 0x60000), or the top-down and cinematic cameras. FreeCam doesn't swing the
camera back while the gun is out, and holds its elevation: the height step
(WorkOutCamHeight) follows the vehicle's nose as seen from behind, which tilted a
camera looking from the side up and down on its own; the elevation it had when the
gun came out is kept, and only the mouse changes it.

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
can't undo them. The ped pool holds 210 (the game's 140, `PED_POOL_SLOTS`, set in
`CPools::Initialise` at 0x4C02C8 from DllMain) and the car pool 110.

**A full ped pool** (always on, `src/population.cpp`)
Every ped comes from one pool (`CPools::ms_pPedPool` 0x97F2AC), and most of the
game's ped makers wrote to the null pointer a full pool gives back. A trainer or a
mission holding many characters filled it (crashes at 0x4496E7 and 0x53B653). The
pool is never capped for scripts or mods; the game survives it in layers:
1. *Making room.* `CPed::operator new` (0x50DA60) goes through `vcmf_ped_new`.
   With no slot free, the farthest street person (created-by +0x160 = 1) nobody
   depends on is removed the way `ManagePopulation` removes one (`CWorld::Remove`
   0x4DB310, then the deleting destructor), off-screen ones first. Never removed:
   - the player or player types;
   - anyone in a vehicle (+0x3AC) or getting into one (+0x3A8 set);
   - the dead or dying (state 0x36 / 0x37);
   - a group leader (+0x151 bit 2, set by `CPed::SetLeader` 0x4F07D0);
   - anyone another ped leads (+0x184) or has as its objective (+0x16C);
   - anyone made in the last 250 ms (3 s caused a double free at 0x6616B7).
2. *Headroom per kind of maker.* While fewer than 30 slots are free, the street
   caps (0x694DC8 / 0x694DCC) are lowered by the shortfall. Each maker stops short
   of the last slots and skips that person or car this time:

   | Maker | Stops when fewer than this many slots are free |
   | --- | --- |
   | New street groups (`AddToPopulation` 0x53BA80, calls at 0x53E5C6 / 0x53E99B) | 16 |
   | Traffic cars (`GenerateOneRandomCar` 0x426DB0, calls at 0x4292D6 / 0x4292F0) | 6 |
   | Emergency cars (0x42AE70, call at 0x42BB86) | 6 |
   | Other street people through `AddPed` | 4 |
   | Drivers, passengers and crews | 1 (after trying to make room) |
   | Script create-character commands | 9, and only if nobody can be removed |
   | Road-block cops | 3, and only if nobody can be removed |

   Each refusal is counted per kind and printed under the census line.
3. *Null exits.* `CPopulation::AddPed` (0x53B600) starts in `AddPedGuard` (a
   trampoline with its first 7 bytes): with no slot it returns 0. Each maker that
   used the result without a check got a jump to a stub. With a ped, the stub does
   the original instructions; without one, it counts the miss (`g_nullHits`) and
   leaves through the maker's own "made nobody" path. Every path was checked for
   stack and FPU balance.

   | Site | What makes nobody | Leaves at |
   | --- | --- | --- |
   | 0x53CB45 | AddToPopulation's group loop (the group ends) | 0x53CF01 |
   | 0x53AC36 | `AddPedInCar` (returns 0) | 0x53AC90 |
   | 0x53B488 | `AddDeadPedInFrontOfCar` | 0x53B3C0 |
   | 0x5AC150 | Heli rope crew | 0x5AC1C4 |
   | 0x6325A9 | SWAT member on a rope | 0x632600 |
   | 0x5B877C | `CVehicle::SetupPassenger` | 0x5B8856 |
   | 0x5B8896 | `SetUpDriver` | 0x5B8913 |
   | 0x419C5B / 0x419C76 | The special-vehicle crew flags | (skipped) |
   | 0x62FD2F / 0x62FEA7 | Script 0560 / 0561 | handle store |
   | 0x445EE4 / 0x4529C4 / 0x45E055 | Script 0129 / 01C8 / 0376 | handle store |
   | 0x44409E / 0x4440BF | Road-block cops | 0x444265 |

   The script exits store handle -1, as 009A's exit at 0x4496E5 already did.
4. *Handles.* `CPool::GetAt` for peds (0x451CB0) and vehicles (0x451C70) didn't
   check the slot: -1 read the byte before the flag array. They now return 0 for a
   slot outside the pool.
5. *Commands on a missing character.* 147 script commands read their first
   parameter as a character (GetAt right after `CollectParameters`) and use it
   without a check. Their parameter counts and whether they are conditions were
   read from each v1.0 handler. Before such a command runs (the
   `ProcessOneCommand` hook in `stuntjump.cpp`), its character is read the same way
   (`CollectParameters` 0x451010 on a copy of the ip). When there is none, the
   command is stepped over, and a condition reads false (`UpdateCompareFlag`
   0x463F00, NOT flag at +0x82). 00A0 keeps its own guard; 00D9, 0154 and 0352 write
   the script themselves and run as before.

*Dead bodies.* `ManagePopulation` (0x53DB9D) marks a deletable body (state 0x37,
`CanBeDeleted` 0x4FFEF0) to fade 30 s after death (+0x55C), as bit 7 of +0x152.
It removes the body once its clump alpha (0x5814F0) is 0. The alpha falls 8 a step
(0x505790, 30 steps a second) only in the ped's ProcessControl (vtable +0x20; all
five ped classes, vtables 0x691BAC, 0x691CE4, 0x691D78, 0x69272C and 0x694D70, call
0x505790 first). `CWorld::Process` (0x4D76C0) runs that only for entities on the
moving list (0x9B48A8, node at +0xE0). In game, after a fight at the player's feet
(VCTrainer's Vice Squad), 150 bodies stayed for minutes. Most were never flagged
although dead well over 30 s, and the flagged ones didn't finish fading, so the
game's own removal never ran. They went only once the player walked away.

The first version cleared only while fewer than 30 slots were free. Each batch of
10 put the pool back above 30 ("has room again (33/39/35 free)"), the street
population took the slots, and the clearing stopped: 163 bodies became 153 in a
minute. Now the scan runs every 250 ms whenever there are more than 20 bodies. It
hands over the oldest bodies the game would delete, down to the 20 newest, at most
10 a scan. Each one must be:
- random (+0x160 = 1);
- dead at least 15 s;
- not in a vehicle (+0x3AC, as the game checks; the last vehicle +0x3A8 stays set
  after death and doesn't count);
- `CanBeDeleted`;
- not a living ped's objective or leader;
- not the one body the player stands on (the nearest dead ped within 1.5 m, in
  3D) and not the player's weapon lock-on target (CPed +0x508, set by the player's
  targeting at 0x532F99 / 0x533A7D). The player is `CWorld::Players[0].m_pPed`
  (0x94AD28), not the camera target 0x7E4E94, which is the car while driving.

  The first test (within 2 m flat, or any registered pointer inside the player, the
  player info or the camera) kept up to 42 bodies at once. The pointer fields
  can't hold that many: the player's melee-attacker list (+0x6A0) has 6 entries,
  and the others hold one ped each. The flat 2 m circle, which ignores height,
  takes every body piled where the player fights. Those pointers no longer protect
  a body, since the deleting destructor clears them. The census still counts what
  the old test would have kept (within 2 m flat; a pointer in the player ped, with
  the last field offset; the player info; the camera).

A line under the census gives, for the last scan, the oldest body, how many bodies
were left alone and why, and how the handed bodies went (removed by the game,
removed here, kept by the game).

A handed body gets the fade flag and is faded at the game's own rate (8 alpha per
1/30 s: 255 to 0 in about 1.06 s) through its clump alpha (0x581500, never
raised). An off-screen one goes to 0 at once. Bodies the game flagged itself are
finished the same way, from their current alpha. At alpha 0, `ManagePopulation`
removes the body itself. If it is still there 0.2 s later, it is removed exactly as
`ManagePopulation` does at 0x53DBF6 (`CWorld::Remove` 0x4DB310, then the deleting
destructor). This happens only after the game's own checks pass again: not a
player, `CanBeDeleted`, not in a vehicle, still dead and still flagged. It runs
after Present, between game updates. Each handed body is checked against its pool
slot (id and address) before it is touched. A body the game no longer wants
removed (flag cleared, alive again) is dropped from the list and left alone. None
of this runs during an F1 replay. The first 10 handed are logged with their
registered reference count (+0x60 list), as are the first 5 removed here and the
first 5 the game kept.

*VCTrainer's bodies.* The trainer makes its people with create-char (009A) from its
own script object "vctrain". They are script characters (+0x160 = 2), so the game
never deletes them: once, 202 of 205 peds were dead trainer characters (type 18,
model 83), and everything else was refused. Each ped made through 0x50DA60
rewrites a per-slot record: address, the slot's id byte and the model asked for,
set only when the maker is 009A (0x449675 / 0x4496AC / 0x4496CB) and the script is
"vctrain". A reused slot gets a new id, so it never matches.

A dead one goes through the street-body scan with the same checks (not in a
vehicle, not a living ped's target or leader, more than 2 m away, not referenced by
the player or the camera; `CanBeDeleted` is skipped because it refuses only script
characters). When picked (the oldest first, the 20 newest kept), it is first
released the game's way: `CTheScripts::CleanUpThisPed` 0x45EB10, which 01C2 calls,
makes it random, resets its AI and lowers the mission ped count at 0x9B5F70. The
trainer already does this for its dead bodyguards. Then it fades and is removed as
a street body. This applies with or without a mission; characters any other script
made are never touched.

For all bodies, the minimum age is 15 s, or 5 s while fewer than 30 slots are free.

The trainer keeps the handles. Every command it sends on a character is in the
skipped-command table: 009B, 01B2, 01C2, 01CB, 01DE, 01E0, 02E2, 0326, 048F and
04D7. It runs them through `ProcessOneCommand` (0x44FBE0), so our hook sees them.
Its own handle lookups call 0x451CB0, which goes to the bounded lookup and returns
none for a freed or reused slot. Its pool walks (bodyguards, nearby people,
touch-kill) check each slot's free flag on every pass and keep no pointer to a
dead ped. The pool's memory stays allocated, so nothing it reads can fault.

*The census.* The census line names the script behind a script create only when
the pointer is on the active script list (0x975338). Each command keeps its script
in its own place: EBP for 009A and 01C8, EBX for 0129, the handler's `[esp+18h]`
for 0376. Skipped commands are listed per script and command.

The world sector list (`CEntryInfoNode` pool 0xA0D8BC) holds 9600 (the game's
3200; a crowd of 180 filled it, crash at 0x4BB557).

*A reaction to a removed ped* (crash at 0x4E94EB). A ped's threat scan (0x50BE00)
can take an event from the event list (0x7DAAB8) that another ped caused. That ped
goes in +0x1A0, a registered reference, and +0x5BC gets 0x2000000. The reaction runs
after a delay (+0x5C0, in 0x4E8E20). If the other ped is removed during the delay
(the game's body removal, our body clearing, a street person removed to make room, a
script deleting it), the reference sets +0x1A0 to 0. The check before the reaction
(0x50BD00) drops the event only if +0x1A0 is still set but invalid, or if +0x190 is 0
as well. So when another threat sits in +0x190, the 0x2000000 case (0x4E94E0) reads
the position at 0+0x34. The 0x100000 case checks its ped first (0x4E8EDD) and leaves
through 0x4E9980, which pops the three FPU values and returns. The 0x2000000 case
now jumps to the same exit when +0x1A0 is 0. The game only skips that one reaction;
the ped carries on. Big fights with many deaths make this much more likely. Log
lines: `a ped's delayed reaction to a ped removed meanwhile is skipped` at start-up,
and `... skipped at 0x4E94E0 (... N so far)` (the first 20 times).

Log lines: `Population: 16 of 16 ped makers make nobody ...`, `... AddPed makes nobody
...`, `... handles outside the pool ... read as "none"`, `147 script commands ...`;
while playing `ped pool short (N free); <kind>: one not made this time`, `ped pool
full; <what> not made, the game carried on`, `script <name>: command XXXX on a
character that doesn't exist ... skipped`, and `refused since the last line: ...`
under the 30-second census line.

Pool size: each ped slot is 0x6D8 bytes in the game plus about 4 KB of
VCModernFix's per-slot arrays (`interp.cpp` poses and matrices, `gameplay.cpp`
`g_good`, `fixedtick.cpp` flags). 400 slots would cost about 1.1 MB more than 210.
The pool stays at 210 because the RAM isn't the limit:
- `CReferences` is a fixed list. `CEntity::RegisterReference` (0x4C6AC0) quietly
  skips registering when its free list (0x9786B0) is empty, and a reference that
  isn't registered isn't cleared when its ped is deleted.
- Update time grows with the number of live peds (about 20 ms per update with 200).
- A full pool no longer crashes.

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

**A model with no animation** (`InstallNoAnimationGuard`, `src/gameplay.cpp`). Each
bone of an animated model is posed by one of two callbacks the animation update
hands to the frame walk: 0x4042D0 (pushed at 0x404866) and 0x4042A0 (pushed at
0x404897), which also call FrameUpdateCallbackSkinned (0x403DF0 / 0x4039D0). All of
them pose the bone from its null-terminated list of nodes at +4 of the update data
(one per animation playing) and read the first node without checking the list, so a
model with no animation at all crashed reading 0x14 (at 0x403ED2 and 0x4043B4; seen
with a trainer's camera option). Both pushes give a wrapper instead: an empty list
leaves the bone as it is (logged 5 times).

**A failed Reset** (`RetryReset`, `src/dllmain.cpp`). RenderWare's device restore
(0x65D8D9) doesn't check Reset's result and calls `GetRenderTarget` next, which on
a device that wasn't reset reads a null pointer inside Windows' d3d8.dll (seen with
RTSS running: crash at system d3d8.dll+0x4E0DB, called from 0x65D8F3). Reset fails
while the device is still lost (the window not back in front), which an overlay
taking or giving back focus can cause. A failed Reset is tried again: the window's
messages are handled, and once `TestCooperativeLevel` stops saying
`D3DERR_DEVICELOST`, Reset runs again (from the fourth try with the game's own
refresh rate, from the seventh with the driver's default), and once a second even
while still lost, for up to 60 seconds. The log records the failure, every 5 s what
it waits for (device state, game window in front / minimized), and the try that
worked. Seen once with RTSS: Reset failed with `D3DERR_DEVICELOST` (88760868) and
the device stayed lost for over 40 s. It waits for as long as the game is in the
background (alt-tabbed or minimized). It gives up after 60 s with the game window
in front and Reset still failing, or when the window is closed. Then it ends the
process with a log line (`Display: Reset still failing ... closing the game instead
of crashing in RenderWare (0x65D8D9)`), since returning the failure only crashed a
few instructions later.

**Which update step is slow** (`src/updateprofile.cpp`). The call of
`CGame::Process` (0x4A4410, from Idle at 0x4A5DA0) and its 68 direct calls (0x4A4412
... 0x4A4660) go through thunks built at start-up (`push n; jmp ProfEnter`).
`ProfEnter` keeps the return address and `rdtsc` on a side stack and returns into
the real target (the one the call had: the trainer's and `hunter.cpp`'s hook on
`CPad::UpdatePads` are chained). `ProfExit` adds the cycles to that step and jumps
back with eax, edx and st(0) untouched. It is installed after everything else.
Logs:
- each update over 25 ms with its three slowest steps (the first 20, then every
  50th): `UpdateProfile: slow update ...`;
- once a minute, the five steps with the most time per update.

**Where VCModernFix's own time goes** (`src/timing.cpp`, `src/timing.h`). Sections
of VCModernFix's own work are timed with `rdtsc` (converted to ms against QPC over
each minute) and written once a minute as one `Timing:` line (120 lines at most):
- per game update: InterpCapture, parts capture, the tick's snapshots for redraw
  frames (alpha lists, PreRender state, ConstructRenderList output, pool flags), the
  per-entity render flags saved, what is put back after the update, interiors
  (`InteriorEndOfTick` without LoadScene), LoadScene per interior switch, quick
  save, the other before-tick helpers;
- per redraw frame: the state saved and loaded, the five pools' flag check, the real
  render list put back, the blend (InterpApply / InterpRestore);
- per drawn frame: `PopulationAfterPresent`, the see-through list inserts
  (the five sorting call sites; `CLinkList::InsertSorted` 0x582BD0 walks from the head
  while the node's distance is smaller, so each insert is O(list length));
- counts: two-pass draws per frame (`alphafix.cpp`), see-through inserts per frame,
  skinned-pose lookups per update (`BoneFrames`: `IS_CLUMP_SKINNED` 0x57F580 and
  `GetAnimHierarchyFromSkinClump` 0x57F250 each run `RpClumpForAllAtomics` 0x640D00
  with a callback that stops at the first atomic, then read one plugin field).

Work done a new way (each was checked against the old way during development, with
identical results over many sessions; only the list check below still runs, as a
safety net, and writes a `Timing:   check ...` line):
- `InterpCapture` and `InterpPartsCapture` swap two buffers instead of copying cur into
  prev (1.4 MB of poses, 42 KB of slots and 351 KB of vehicle parts each update; 0.04
  + 0.01 ms measured outside the game on an Intel Core Ultra 5 250K, more with the
  game's data in the cache). Prev then holds exactly the bytes the copy would have put
  there. Cur is filled for every slot (`valid`, the pose and part counts) before
  anything reads it, and every reader tests `valid` and reads only the first n bones /
  parts, so the older bytes left in invalid slots are never read.
- Interiors (`src/interior.cpp`): `NearestOfArea`, `RoomDist`, `CollectPieces` and
  `InteriorNear` walk a list of the interiors' buildings (by `RealArea`, in the order
  the whole-pool walk visits them: building pool then treadables, by slot) instead of
  both building pools (8250 slots). Each still tests the area itself, so it sees the
  same buildings in the same order and gives the same answer. The lists are rebuilt
  when either pool's object array, size or slot flags (a memcmp of 8250 bytes) differ
  from when they were built; a building's area byte changes only through `HideOne` /
  `Unhide`, which `RealArea` sees through. Every 5 s the lists are compared with a
  whole-pool walk (about 0.01 ms); a difference is logged and the lists are rebuilt.
  The city (area 0: `NearestOfArea(0)`, `Hide`, `HideContaining`, `PunchNear`) still
  walks the pools.

Other measured work:
- `Log()` (`src/config.cpp`) keeps one handle opened for appending
  (`FILE_APPEND_DATA`, shared for reading and writing) and writes each line with one
  `WriteFile`; it used to open, append and close the file per line. Outside the game,
  4 threads x 2000 lines: 108-126 us a line before, with about 10% of the lines lost
  or torn when threads logged at once (separate `fopen` appends); 3.6 us after, none
  lost. A line is in the file as soon as `WriteFile` returns.
- Interior switches log how long deciding the view and `LoadScene` took
  (`... took 2.1 ms to decide the view, 30.4 ms in LoadScene`). `CStreaming::LoadScene`
  (0x40AF60), from the bytes: the level at the position (0x4DD300); pending requests
  without a priority flag dropped (0x40D6E0); the other island's models removed; then,
  for every building of the current level and then of both levels, `RequestModel`
  (0x40E310) when 0x4C7D40 says it should be loaded (its area test at 0x4C7D55 is one
  of ShowOutside's `AreaCmp` sites, so it also accepts the area shown with it); a few
  car models of the zone; `LoadAllRequestedModels` (0x40B5F0) synchronously; and RW
  objects created for loaded buildings near the position. While a room is shown from
  the street (`InteriorNear` within 20 m), the game's own streaming already requests
  its models through the same area tests, so they are usually loaded before a switch.
- Preload (`src/preload.cpp`) keeps the mapped views of `SFX.RAW` and `gta3.img`
  only when reading the file took 500 ms or more (a hard disk). Read in tens of ms
  the file was already in the Windows file cache (or the disk is a fast SSD): the
  game's own reads come from the cache, and the views would only add the same pages
  to the game's working set and take 636 MB of its address space.

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
- saves and restores the random number seed, and starts the entity `PreRender`
  pass from the seed the tick had before its own `PreRender`, so lamp flicker,
  muzzle flashes and coronas get the tick's random numbers. Once a minute the log
  counts redraw frames whose seed and point-light count (`0x9751B0`) after
  `PreRender` matched the tick's (`FixedTick: redraw frames' PreRender started from
  the tick's random numbers: ...`); particles the tick spawned are skipped on redraw
  frames, which changes the numbers after them;
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
- A frame shows the moment one tick length ago on a steady clock, between the
  last two ticks' scheduled times. Ticks can only start on a drawn frame (up to
  a frame late), so the game's clock read in `CTimer::Update` (`0x4D0F71`, the
  `QueryPerformanceCounter` import call) returns the tick's scheduled time during
  a tick (never earlier than its previous reading, never later than the real
  clock). Every tick then covers exactly one tick length, and frames drawn one tick
  length behind always fall between the last two ticks with no clamping. Before
  this, 28 / 35 ms steps at 144 Hz made a frame hitch about six times a second.
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
than 10 m is drawn as is. A particle spawned during the latest tick has no earlier
position: it is drawn along its own velocity, `position - velocity * timestep *
(1 - alpha)` with the tick's time step (the update moves it by velocity x
`CTimer::ms_fTimeStep`, 0x5620F6). Drawn at its spawn point instead, it stood still
for a whole tick and then moved, and short-lived fast ones (rain drops, splashes, a
broken fire hydrant's spray) looked choppy. The tables hold 8192 live particles.

**Leaves and newspapers** (`CRubbish`, `src/interp.cpp`): 32 sheets of 0x44 bytes at
0x7F92F8. `CRubbish::Update` moves a blown sheet once per tick; `Render` (0x567FF7)
draws state +0x1C == 1 (lying) at +0x00, other states (flying) at +0x0C, turned by
+0x34. Each tick records the drawn position and angle; redraw frames write the blend
of the last two ticks into the field `Render` reads (a new flight, +0x20 changed, is
drawn as is) and put the real values back after.

**Water** (`CWaterLevel::RenderWater` 0x5C1710, `src/interp.cpp`): the wave shape
comes from the clock (`time & 0xFFF`, 0x5C185B), which redraw frames run forward.
The texture drift is two accumulators (0x77FA6C, 0x77FA70) moved by a step on every
draw (0x5C18A7: 0.0005 x `CWeather::Wind` 0x9B6A94 + 0.0006, not while paused,
0xA10B36 / 0xA10B76), and FixedTick puts them back after each redraw frame
(`redraw_state.h`): the drift moved only at the 30 Hz ticks, and every redraw frame
drew it a whole step ahead before it snapped back, so the texture stepped and
jumped over smooth waves. The same block (gated by the two pause flags) moves four
more accumulators by the sine / cosine of the wave angle plus the step (0x77FA74,
0x77FA78, 0x77FA7C, 0x77FA80: the surface's wobble), with the same result. Each
tick frame measures how far the renderer moved each of the six (InterpApply /
InterpRestore with tick set bracket the tick's drawing); on a redraw frame each is
set back by (1 - alpha) of that before drawing, so with the renderer's own move the
frame draws it alpha of a tick on from the tick, like the clock, and the values are
put back after. A move over 0.25 is a wrap-around (reset at 0x5C19CF..) and keeps
the previous move. (Original game at 30 fps: one move per frame, the same speed.)
The wave mask (0x5BF0E0) is stepped by 0x5BF540 (call at 0x5BF62E), which Idle's render
half calls first, at 0x4A5E40, once per drawn frame - not from inside either water
call. (FixedTick hooks that call already, as `HookMirror`; the name came from taking
it for `CMirrors::BeforeMainRender`.) The mask is a 17 x 17 grid model (atomic at
0xA10860: +0x18 geometry, +0x5C morph target, +0x14 vertices, row x 17 + column)
whose inner points it moves on every draw and keeps from draw to draw
(0x5BF30E..0x5BF385): `x = (x + n) * 0.99 + row * b`, `y = (y + n') * 0.99 + column * b`,
with `n = (rand() / 32768 * 2 - 1) * k` (`rand` 0x6499F0) and `b = (1 - 0.99) * 2`: a
damped random walk, one step per draw (the height, +8, is worked out fresh from the
clock). The original game takes 30 steps a second; at ~1000 fps it took 33 times as
many, and the waves near the camera ran far too fast and wobbled (they looked right
with `FPSLimit=30`). `WaterMaskStep` (`alphafix.cpp`, called by HookMirror) lets the
step run on tick frames, with real `rand()` numbers, and keeps the grid it leaves. On
a redraw frame both `rand()` calls (`MaskRand`) answer 0x4000, which makes `n` exactly
0, and the inner points are set beforehand to `(kept - row/column * b) / 0.99`, so the
game's own step lands on the kept grid; the grid is put back after. At that point of
the frame the clock and TheCamera are still the tick's (Interp moves them later, in
ConstructRenderList), so the mask is drawn exactly as the tick drew it. Redraw frames
don't call `rand()`, so the game's random sequence is the same as without redraws.
Everything else about the water stays smooth on redraw frames: the clock-driven wave
shapes and the six accumulators above.

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

**F1 replays:** `CReplay::Mode` (0xA10AF6) is 1 while a replay plays
(`CReplay::TriggerPlayback` 0x6209E0 sets it; the game tests it in 48 places).
The replay swaps the pools' contents for its recording and rebuilds the world
from it every update, so while it plays: no redraw frames and no blending
(`Gameplay()` in fixedtick.cpp is false: frames run at the tick rate, as with the
original limiter); the per-tick and per-frame helpers (quick save keys and saves,
phone skip, gameplay, mission helpers, interiors) are skipped; free cam's three
hooks pass straight through. The log notes `Replay: playing` / `Replay: ended`.

**Deleted entities in the draw lists** (`src/fixedtick.cpp`): redraw frames only
run while the ped, vehicle, object, building and dummy pools' slot flags (CPool
+4; buildings 7000 and dummies 2340, about 9 KB compared a frame) are as the last
tick left them; an entity added or removed outside the game update (another
mod or a trainer acting from Present) makes the frame wait for the next tick.
And `CRenderer::RenderOneNonRoad` (0x4C9DA0) skips an entity whose `m_rwObject`
(+0x4C) is null instead of crashing at 0x582739 (seen while flying a helicopter).

**Crash log** (`src/watchdog.cpp`): a vectored exception handler logs access
violations, illegal instructions, divide by zero and stack overflow (at most 4)
with module + offset, what was read or written, registers and return addresses
into `gta-vc.exe` / DLLs on the stack, then passes the exception on unchanged.
The game's own handler (0x677E40) only shows a message box. The first one of a
session also writes a minidump (`MiniDumpWriteDump` from dbghelp.dll, loaded only
then; data segments, indirectly referenced memory, thread info) to
`VCModernFix-crash-YYYYMMDD-HHMMSS.dmp`; only the newest 3 are kept. At start-up
the last session's log is renamed to `VCModernFix.previous.log`. Once a crash is
logged, the freeze watchdog writes one line (`WATCHDOG: no progress after the crash
above ...; watchdog stopped`) and its thread ends: the game thread then sits in its
crash message box, and nothing more is polled or dumped. (A logged exception that the
game survives, such as the one ExitGuard handles, keeps the heartbeat going, so the
watchdog carries on.)

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

## The Modern Fix menu (`src/fixmenu.cpp`)

A page under the game's Options menu switches the fixes while playing; each change
is written to `VCModernFix.ini` (`SaveSetting`, `WritePrivateProfileString`).

**The game's menu.** CMenuManager (FrontEndMenuManager 0x869630; m_nCurrScreen
+0xF8, m_nCurrOption +0x30) draws its pages from `aScreens` (0x6D8B70): 35 pages of
226 bytes (name[8], previous page, row on it, 12 rows of 18 bytes: action (short),
text key[8], save slot, target page, x, y, alignment). Row text is
`TheText.Get(key)` (CText::Get 0x584F30, TheText 0x94B220). Page 22 is never used by
the game (reVC: MENUPAGE_SAVE_UNUSED; no code compares the current page with 22).

**The page.** Options gets a row above Back (CHANGEMENU to page 22, key `VMFO`);
page 22 is renamed `VMFT` and its rows are rewritten: the category list (actions
0x60 + n, centred), or one category's switches (actions 0x70 + n, left column at
x 40 like Display Setup) and Back (the game's action 34). The game's actions end at
53, and every place that reads a row's action uses a compare chain whose default
skips unknown ones. CText::Get is hooked at its start (a trampoline with its first
7 bytes, or a chain to another mod's jump there): keys starting `VMF` are answered
with our labels.

**Drawing and input.** Three default jumps of the game's action switches go to stubs:
- 0x49EBF4 (Draw, the right column): ON / OFF from the game's `FEM_ON` / `FEM_OFF`
  into ebx, the right-text register (cleared per row at 0x49E726).
- 0x49972F (ProcessButtonPresses, Enter or a click): opens a category
  (`SwitchToNewScreen(22)` after the rows are rewritten) or switches the item.
- 0x499DB7 (left / right, change amount [esp+84h]): switches the item.
CMenuManager::SwitchToNewScreen (0x4983EF, thiscall, int8: 0 = the row's target,
-2 = back) is hooked at its start: back (Back or Esc) from a category shows the
list, with the page's previous page set to itself and the row to the category for
that one call; entering page 22 from another page shows the list.

**Live settings.** Every feature is installed at start whatever its setting (after
the usual byte checks), and checks its `g_cfg` setting where it acts: hooks pass
straight to the game when off. Code patches that are on or off go back to the
game's bytes: boats' camera mode (0x470C37), the island loading screen calls, run
and gun's sprint / scope jumps (and the heavy-gun flag is put back). Population
keeps the game's densities (from gta3.ini or its defaults) to put back; the draw
distance's 300 m limit and LOD scale follow the setting each frame; anisotropic
filtering resets the stages RenderWare left linear (`AnisotropyChanged`).
ShowOutside turned off ends the view at once (`ViewOff`: rooms put back, the area
test strict again). Numbers switched OFF take the game's value; ON puts back the
ini's number, or a default.

## Building

Requires [Zig](https://ziglang.org/) (`winget install zig.zig`). Run
`build.bat`; it produces `d3d8.dll` (32-bit).

## Source files

| File | Purpose |
| --- | --- |
| `src/dllmain.cpp` | d3d8 proxy, refresh rate, `Present` hook; no vsync for FPSLimit=-1 (on the call only); a refused fullscreen device retried |
| `src/fixedtick.cpp` | fixed-tick loop, redraw frames (F1 replays included), hooks; 3D markers (0x814018) blended between ticks |
| `src/interp.cpp` | camera/object/pose blending, render-list flag save/restore |
| `src/watchdog.cpp` | freeze diagnostics and the crash log into `VCModernFix.log`; stops after a crash has been logged and the game stopped |
| `src/mousefix.cpp` | mouse device re-creation fix |
| `src/freecam.cpp` | mouse look around the vehicle |
| `src/mouselook.cpp` | on-foot mouse look read every frame |
| `src/quicksave.cpp` | F5 quick save, F9 quick load |
| `src/phoneskip.cpp` | Enter skips phone calls |
| `src/interior.cpp` | The drawn interior follows the player (teleports, jetpack); the city is drawn from inside interiors |
| `src/drawdist.cpp` | streaming memory, vehicle models kept loaded, draw distance |
| `src/gameplay.cpp` | mouse aim for the Rhino and fire truck, mission leftovers (hotel swap), run and gun, keep firing when hit |
| `src/population.cpp` | ped and car density, peds kept when you look away, surviving a full ped pool, script commands on missing characters |
| `src/updateprofile.cpp` | times each step of the game update; logs slow updates |
| `src/timing.cpp` | times VCModernFix's own work per update and per frame; the `Timing:` line and its before / after checks |
| `src/preload.cpp` | reads `SFX.RAW`, `gta3.img` and the audio streams into memory at start; the `Memory:` line |
| `src/modernkeys.cpp` | modern key layout (Q radio, E horn, Shift sprint, WASD, turret keys); Delete off aim / zoom; bikes lean with Num8 / Num5 only (the call at 0x60A69D) |
| `src/oddjobs.cpp` | Fire truck / ambulance / vigilante jobs: their `0038 $236 0` island checks answered "open" while the player is on the mainland before the bridges open, so targets are picked around the player |
| `src/pizza.cpp` | Pizza Boy: right + left mouse button throws; every throw aimed at the nearest waiting customer ($7966..$7975 with $7996..$8005 set), the script's IS_BUTTON_PRESSED 5 / 7 / 17 answered and $8040 / $8041 set before SET_OBJECT_VELOCITY |
| `src/widescreen.cpp` | widescreen picture and 4:3 HUD; near clip brought in for a wider FOV |
| `src/viewscan.cpp` | the world scan covers the whole view at any camera angle |
| `src/farlights.cpp` | 120 corona slots, corona distance, far city lights drawn as sprites |
| `src/clouds.cpp` | the cloud layers rise with a high camera |
| `src/onfootcam.cpp` | free camera on foot: the player runs the way you press, aiming turns them to the camera |
| `src/stuntjump.cpp` | unique stunt jumps without slow motion or the cinematic camera |
| `src/missionhelp.cpp` | skip scripted mission scenes, end stuck missions, restart prompt (only after PRINT_BIG "M_FAIL" or a mission ended here) |
| `src/faststream.cpp` | faster model / texture streaming |
| `src/driveby.cpp` | free drive-bys aimed with the camera |
| `src/hunter.cpp` | Hunter: mouse wheel picks rockets / cannon, Space fires it |
| `src/aircontrol.cpp` | steer a car's pitch and roll in the air with the helicopter controls; heavier helicopters |
| `src/exitguard.cpp` | skips a crash in the game's own clean-up when quitting |
| `src/fixmenu.cpp` | Options > Modern Fix: switches for the fixes while playing, saved to the ini |
| `src/alphafix.cpp` | see-through leaves in two passes, only while RenderScene draws (the call at 0x4A604A) and never for pre-transformed (XYZRHW) vertices; the water's wave mask stepped 30 times a second |
| `src/redraw_state.h` | generated table of render state restored on redraw frames |
| `src/framelimiter.cpp` | timer-based frame limiter |
| `src/config.cpp` | `VCModernFix.ini`, `VCModernFix.log` |
| `VCModernFix.ini` | default settings |
| `docs/TECHNICAL.md` | this file |
| `SetLargeAddressAware.ps1` | set or clear the 4 GB flag on `gta-vc.exe` |
| `build.bat` | build script |
