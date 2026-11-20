# ctr64 compatibility list

What is known about each game on a **New 3DS** (Old 3DS: see the end). One row per game: the
latest known state, the version it was seen on, and where it came from. Update a row whenever a
report or a test changes it; keep the older note in "History" when it is useful.

Status: **Perfect** (full speed, no known issue) · **Playable** (minor issues) · **Issues**
(playable with clear problems) · **Broken** (crash, freeze, can't get in game) · **?** (not tested)

Sources: *forum* = user reports, *HW* = our hardware runs, *Az* = our Azahar runs (correctness
only: Azahar runs the 3DS CPU ~2x slower than hardware, so its speed numbers are not hardware
speed).

| Game | Status | Seen on | Notes | Source |
|---|---|---|---|---|
| Animal Forest (Animal Crossing, fan translation) | Issues | 0.9.2-dev | Good speed, drops too often; horizontal/vertical line glitches while walking; drops on room change; froze in Tom Nook's store after ~1 h. Az: boots to the new-game dialogue, graphics right. HW 0.9.2-dev: boot to the K.K. intro ~100% (the 1 h freeze in Nook's store not retested) | forum, Az, HW |
| Banjo-Kazooie | Perfect | 0.9.1 | Runs really well; near perfect, minimal occasional stutter (played to Mumbo's Mountain) | forum |
| Beetle Adventure Racing! | Playable | 0.9.2-dev | Good; minor graphics glitches; small drops (e.g. crashing into a wall). HW 0.9.2-dev: intro/demo/main menu 100% (race not reached) | forum, HW |
| Blast Corps | Good | 0.9.2-dev | 0.9.1 froze in the bulldozer intro (two RDP interrupts merged when two display lists were in flight; fixed in rdp_core.c). Ground glitches fixed: light/shadow pools came out as black squares (8-bit frame buffer read as RGBA; now red-to-all like GLideN64) and, with anti-aliasing on HW only, whole ground patches were missing (huge off-screen triangles dropped by the 3DS rasterizer; now clipped to a guard band). HW 0.9.2-dev: intro and gameplay 96-98%, one heavy scene ~75%; boot to the first level ~100%, no crash (a 0.9.1 crash report in the first level came with the two-interrupt problem above). The whole-system freeze from the forum report not seen | forum, HW |
| Bomberman 64: The Second Attack! | Perfect | 0.9.0 | Near perfect | forum |
| Bomberman Hero | Playable | 0.9.2-dev | Minor slowdowns on scene changes and dying. HW 0.9.2-dev: boot to level select 100%, scene change 94% | forum, HW |
| Bust-A-Move '99 | Perfect | 0.9.0 | Near perfect | forum |
| Charlie Blast's Territory | Perfect | 0.9.1 | Near perfect | forum |
| Command & Conquer | Playable | 0.9.2-dev | ~99.5% in our run | HW |
| Conker's Bad Fur Day | Issues | 0.9.2-dev | Boot to gameplay on HW: mostly 87-100%; rain intro ~68% and one cutscene stretch ~73% (both CPU and video bound); 64 MB ROM now loads whole (178 MB mode), no SD streaming hitches | HW |
| Diddy Kong Racing | Perfect | 0.9.0 | Near perfect | forum |
| Donald Duck: Goin' Quackers | Broken | 0.9.0 | Picture freezes before gameplay, sound continues | forum |
| Donkey Kong 64 | Playable | 0.9.2-dev | Major graphics glitches, major stutter (0.9.0-0.9.1). The intro (DK Rap) drew a red wall and no dancers since 0.9.0: the game switches the FPU register mode (Status.FR) and the recompiler took back code compiled for the other mode; fixed in 0.9.2-dev. Shade subdivision made 1 M triangles/s in the intro; capped. HW 0.9.2-dev: intro renders right at 70-100% (was 23%, one 39% stretch early on); gameplay on DK Isles (new game, walking and swimming) ~91%, renders right; video thread CPU-bound (display lists ~17 ms/VI, no single hotspot). Froze once in game on HW (cause unknown, not reproduced since) | forum, Az, HW |
| Dr. Mario 64 | Perfect | 0.9.1 | Near perfect | forum |
| Duck Dodgers Starring Daffy Duck | Playable | 0.9.2-dev | Stages stutter, 50-80% (forum). HW 0.9.2-dev: boot to the opening scene 100%; hub walking 100% (from a save state; stages not checked) | forum, Az, HW |
| Extreme-G | Playable | 0.9.0 | Runs great, some audio stutter | forum |
| F-Zero X | Playable | 0.9.1 | Runs great, a little audio stutter; black vehicles of 0.9.0 fixed in 0.9.1 | forum |
| Gauntlet Legends | Issues | 0.9.2-dev | ~20% speed (forum). Black screen with threaded video (all versions): its display lists branch to themselves; fixed in 0.9.2-dev (its display lists run on the CPU thread). 0.9.2-dev: the self-looping list is no longer restarted 35000 times a second, no 832x960 buffers. HW 0.9.2-dev: menus 96-99%, attract ~40%, gameplay 28-39% (still CPU-bound: GLideN64 display-list work ~20 ms per frame) | forum, Az, HW |
| GoldenEye 007 | Playable | 0.9.2-dev | 96.6-99.7% | HW |
| Harvest Moon 64 | Playable | 0.9.2-dev | Almost perfect, minor stutter. HW 0.9.2-dev: boot/title/name entry ~100% | forum, HW |
| Hydro Thunder | Issues | 0.9.0 | 50-100%; graphics fine | forum |
| Jet Force Gemini | Playable | 0.9.2-dev | ~99.5% in our run; stutter and audio stutter reported | HW, forum |
| Kirby 64: The Crystal Shards | Playable | 0.9.2-dev | Glitches of 0.9.0 gone; minor drops (collecting shards). HW 0.9.2-dev: to the first level tutorial ~100% | forum, HW |
| Legend of Zelda: Majora's Mask | Playable | 0.9.2-dev | 96.8-99.7% | HW |
| Legend of Zelda: Ocarina of Time | Playable | 0.9.2-dev | ~99.9%, Dodongo's Cavern 95-100% | HW |
| Magical Tetris Challenge | Perfect | 0.9.1 | Near perfect | forum |
| Mario Golf | Good | 0.9.2-dev | 0.9.1 crashed entering a course (null depth texture in FrameBuffer::getTexture; fixed). Menus were 40% with AA (frame buffer shadow copies; now adaptive power-of-two storage). HW 0.9.2-dev: menus and course ~100% with AA | forum, HW |
| Mario Kart 64 | Playable | 0.9.2-dev | ~94.5% with anti-aliasing, ~100% without; stutter between scenes and in menus | HW, forum |
| Mario Tennis | Playable | 0.9.2-dev | ~75%; graphics fixed in 0.9.1; sometimes a match doesn't start (0.9.0). Az: menus render right. HW 0.9.2-dev: intro/menus were 45-55% (its 384x240 picture got 480-line buffers, 768x960 at 2x, rendered outside VRAM); fixed: now ~100% through intro and menus with anti-aliasing (matches not reached) | forum, Az, HW |
| Mickey's Speedway USA | Playable | 0.9.2-dev | Full speed most of the time in game; big drops in menus. HW 0.9.2-dev: to the house hub 96-100% | forum, HW |
| Micro Machines 64 Turbo | Perfect | 0.9.1 | Near perfect | forum |
| Ms. Pac-Man: Maze Madness | Playable | 0.9.2-dev | Minor slowdowns. HW 0.9.2-dev: to the title ~100% | forum, HW |
| Mystical Ninja Starring Goemon | Playable | 0.9.2-dev | 0.9.0 glitches mostly gone; some audio stutter. Az: gameplay renders right. HW 0.9.2-dev: into gameplay 83-100%, one ~40% stretch in the intro | forum, Az, HW |
| Paper Mario | Playable | 0.9.2-dev | ~100%; audio stutter on room change (adaptive audio queue added in 0.9.2-dev, compile stalls remain) | HW, forum |
| Pilotwings 64 | Issues | 0.9.2-dev | 98-99.6%; Class A rocket pack mission 2: rain makes the whole screen flash (not reproduced yet: Class A needs a save with Beginner class done) | HW, forum |
| Pokemon Puzzle League | Playable | 0.9.2-dev | Near perfect in game, stutter in menus. HW 0.9.2-dev: menus/Puzzle Village ~100% | forum, HW |
| Rat Attack! | Good | 0.9.2-dev | 0.9.1 ~20% speed. HW 0.9.2-dev: menus and first level ~100% (boot 77%) | forum, HW |
| Rayman 2: The Great Escape | Issues | 0.9.2-dev | Froze after ~5 min (music continues; forum): with threaded video the game rewrote a display list before the video thread had read it. Fixed in 0.9.2-dev (its display lists run on the CPU thread; Azahar from a save state froze at ~VI 5600 before, clean to 8000 after). HW 0.9.2-dev: no freeze in a 6500-VI run; gameplay 60-90%, some scenes ~100% | forum, Az, HW |
| Ridge Racer 64 | Perfect | 0.9.0 | Near perfect | forum |
| Snowboard Kids 2 | Perfect | 0.9.0 | Near perfect | forum |
| Space Invaders | Playable | 0.9.2-dev | Minor stutter. HW 0.9.2-dev: 100% | forum, HW |
| Star Fox 64 | Playable | 0.9.2-dev | ~99.5% | HW |
| Star Soldier: Vanishing Earth | Perfect | 0.9.0 | Near perfect | forum |
| Star Wars Episode I: Racer | Playable | 0.9.2-dev | Almost perfect in game, stutter in menus. HW 0.9.2-dev: title 95-100%, but name entry was 15-20% (its 480-line menus at 2x needed 8 MB, rendered outside VRAM); fixed: such a game drops anti-aliasing to 1x; now 100% through title/menus/name entry | forum, HW |
| Super Mario 64 | Perfect | 0.9.2-dev | ~99.5-99.9% | HW, forum |
| Super Smash Bros. | Playable | 0.9.2-dev | 97.5-99.9% | HW |
| V-Rally Edition 99 (Europe) | Issues | 0.9.2-dev | Doesn't start, ~10 fps. Az: starts, language screen, attract demo, main menu; demo heavy (~2 VIs per frame). HW 0.9.2-dev: boots now; language/intro/menus at full speed (50 VI/s: a PAL game, so 83% of 60) | forum, Az, HW |
| Viewpoint 2064 (prototype) | Playable | 0.9.2-dev | Almost perfect in game. HW 0.9.2-dev: to the first stage 100% | forum, HW |
| Wave Race 64 | Perfect | 0.9.2-dev | 0.9.1: stutters, ~70% in game. Its GLideN64 game settings copy every frame to N64 memory in sync mode, so each frame waited ~9 ms for the GPU; 0.9.2-dev uses the double-buffered copy for it (the same picture, no wait). HW 0.9.2-dev: race (Dolphin Park) 100% | forum, Az, HW |
| WipeOut 64 | Perfect | 0.9.0 | Near perfect | forum |
| Yoshi's Story | Perfect | 0.9.2-dev | 0.9.1: major graphics glitches; ~70% while moving. Its three scrolling backgrounds are 336-pixel images drawn past their edge; drawn as one piece they must wrap at 336, which the 3DS GPU cannot (half the background black or repeated, the big foreground tree missing). 0.9.2-dev draws them in strips as the N64 does. HW 0.9.2-dev: gameplay 100%, background right | forum, Az, HW |

## Old 3DS

- CIA on Old 3DS 11.17 / Luma 13.5: fails to start, no core.log or crash.txt (report, 0.9.1).
  The Luma dump showed the HOME Menu itself crashing: the CIA asked for the 96 MB memory mode,
  which leaves the HOME Menu too little memory. 0.9.2 uses the 80 MB mode retail games use, with
  less memory use to fit (Azahar Old 3DS: SM64, MK64, SSB, GoldenEye, OoT, PM, MM run 1800 VIs;
  GoldenEye Dam gameplay). Not yet confirmed on an Old 3DS.
- ROMs are streamed from SD in 64 KB pages unless one fits with 12 MB of heap to spare (Azahar-tested).

## Requests from users

- Turn the bottom screen off during gameplay: done (0.9.2).
- Swap Circle Pad and D-pad (racing games), per-game controls: done (0.9.2-dev).
- QR code / Universal Updater for updates: Universal Updater request submitted.
