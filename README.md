# Mercenaries2Remastered (Beta 1)

Fixes, modern-system tweaks and gameplay improvements for **Mercenaries 2: World in Flames (PC)**, plus an
optional port of the Xbox 360 **Title Update 4** and the **"Blow It Up Again Pack"** DLC, built on the player's
PC from their own Xbox 360 files.

Mercenaries2Remastered is a `dinput8.dll` proxy. Every change is made in memory at runtime, and converted content goes into
`Mercenaries2Remastered\content\`, which the game mounts as its patch folder. The only game file the setup changes is
`Mercenaries2.exe`, which gets the large-address-aware flag (4 GB of memory instead of 2); the original is
kept as `Mercenaries2.bak` and put back on uninstall.

Supported game builds (`Mercenaries2.exe` without the disc check):

| Build | MD5 | made 4 GB aware by the setup |
|---|---|---|
| 1.0 (Aug 13 2008) | `857b3387d54774a32c1328effb5de4d4` | `2e0d3a41df30c59714444c3a84c3f372` |
| 1.1 (Sep 12 2008) | `5b9976f162e050f4adcc51bb997ba97f` | `4f90b0560d41ea407cf65224c1177334` |

`dinput8.dll` loads the mod built for the running exe from `Mercenaries2Remastered\`. With other builds only the file fix is active.

## Installing

Download `Mercenaries2Remastered-Beta1-Setup.zip` from Releases, unzip it anywhere and run `Mercenaries2RemasteredSetup.exe`:

* **Mod only**: the DLL and its settings.
* **Mod + Xbox 360 TU4 and DLC**: also converts the Xbox 360 content. Select your own files:
  * the base game Xbox 360 European game disc image,
  * Title Update 4 for the European version of the game (`TU_12K2218_0000008000000.0000000000102`, or the zip it came in),
  * the Blow It Up Again Pack package (`45410828/00000002/...`, or the zip it came in).

  The conversion runs on your PC with a bundled Python. It takes a few minutes and needs about 1.5 GB of
  temporary space. If it is interrupted, pressing Install again continues where it stopped. Nothing from
  the Xbox 360 or PC game is included in this repository or in the release.

Installing over an earlier install rebuilds `Mercenaries2Remastered.ini` from the defaults and keeps your values.
Uninstall in the same program removes everything again and restores the original `Mercenaries2.exe`.

## Features

Settings are in `Mercenaries2Remastered.ini` in the game folder; it is created on first start with comments. The log is
in `Mercenaries2Remastered.log` (the last five runs are kept as `.1.log` ... `.5.log`).

| Area | What it does |
|---|---|
| **File fix** | `data\vz.wad` from some installs is split at 2 GB into `vz.wad` + `vz.wad_1`; the game only opens the first part and hangs forever on the loading screen. The file APIs are redirected so the parts read as one file. |
| **Display** | Borderless window on the game's monitor instead of exclusive fullscreen (`[Display] Borderless`), DPI awareness (`DpiAware`). |
| **Performance** | Towns hold 60 fps: faster render-thread hand-off, redundant D3D state calls dropped, small objects skipped in the depth, shadow and reflection passes, reflections every 2nd frame, a fast forest instance decoder, SecuROM detours removed from hot code, more streaming reads in flight. Shaders the game used before are pre-compiled at start (`Mercenaries2Remastered\shaders.bin`), so fewer hitches. The job worker and two spin-waits no longer burn a core. |
| **4 GB memory** | `Mercenaries2.exe` is made large address aware by the setup; the game's memory pools and renderer buffers are enlarged (several overflowed in busy scenes). |
| **Gamepad** | Right stick drives the uncapped mouse-look path (speed, curve, dead zone, invert); configurable stick dead zone. |
| **Xbox prompts** | While a gamepad is used, the on-screen key icons show the Xbox buttons bound to each action. The glyphs come from the Xbox atlases the PC game ships but never uses; the installer extracts them from your `vz.wad` into `Mercenaries2Remastered\prompts.bin`. |
| **Camera** | On-foot camera distance/height/shoulder offset, aim camera that keeps the reticle on the line the shots take, field of view, smoother blending; mounted guns driven by the stick directly. |
| **Aim assist** | Gamepad aim assist. Hip fire: the enemy nearest the view centre is marked red and the shots go to it; the view is not turned. Aiming down the sight: snap to the enemy nearest the reticle (upper chest, from the character's bones), then a gentle magnet. Enemies behind walls and subdued enemies are skipped. The game's own aim magnetism, target follow and aim hold, and the mounted-gun pull toward targets, are off. |
| **Vehicles** | Steering speed/inertia, high-speed steering, optional arcade handling (grip, yaw damping, centre of mass). Mass, power and top speed are untouched. |
| **Vehicle deformation** | Cars, helicopters, jets and boats dent where they are hit (crashes, rams, explosions): the impact is read from the vehicle's physics, the dents are applied in a patched copy of the game's vertex shaders (position, normals, creases), capped so cabins stay clear of the occupants. Detachable parts dent with the body; windows break open where a dent reaches them (`[Deformation]`). Tanks and motorcycles keep their shape. |
| **Tyre marks** | Skid marks where tyres slide (handbrake, locked brakes, wheelspin, drifting), projected onto the ground from any camera angle and fading out over two minutes (`[TyreMarks]`). |
| **Graphics** | Forced anisotropic filtering, SMAA 1x and colour grading on the final frame (F10 toggles); grass, trees, objects, model detail, terrain detail and the streamed world reach further, far clip plane x2 (`[Graphics]`, `[Color]`). Raising the distance settings above the defaults is not recommended: more of the world is loaded at once, which can run the 32-bit game out of memory or into engine limits (crashes, freezes, missing objects) and lowers the frame rate. F9 FPS counter. |
| **Xbox 360 rendering** | Brought back from the Xbox 360 build (its shaders were disassembled and compared with the PC ones): normal maps decoded correctly (the PC shaders flattened or over-tilted every normal-mapped surface); 2x2 filtered sun shadows on grass, bushes, trees, decals and water instead of one hard sample; direct lighting without the PC-only clamp to 1 and the Xbox colour/eye-adaptation limits; linear HDR bloom in FP16 targets (the PC packed the bloom into 16-bit integers and lost most of the glow of fire, explosions and lights); soft particles (smoke and fire fade where they meet the ground and walls, from the scene depth); fire on burning bushes, trees and wrecks (the PC never computed the surface the flames are emitted from); model sun shadows of far trees and trees in the fade zone instead of flat cards; the Xbox motion-blur mask; the paper sheets blowing through the streets (removed in patch 1.1); the Xbox depth-aware edge anti-aliasing while the game's Anti-Aliasing is off (`[Graphics] XboxShaders, LinearBloom, SoftParticles, EmitterFix, ModeledTreeShadows, PaperAnimations, DepthAA`). |
| **Gameplay** | Walking over a dropped weapon that uses the ammo of one of your guns (also the holstered one) takes the ammo and removes the weapon, no button or animation; a full gun takes nothing (`[Gameplay] AutoAmmoPickup`). |
| **Fixes** | The Anti-Aliasing option works in the borderless window and on the 3D scene (`AAFix`); movies no longer go black after skipping one (`MovieFix`); stowed weapons, holsters and pouches no longer twitch above 30 fps (`TwitchFix`); water on native NVIDIA drivers. |
| **Cheats** | Always available: the cheats from the TU4 update, typed as words in the PDA on PC (see [Cheats](#cheats)). Gamepad codes work as on console. |
| **Offline** | Answers the "play online?" question with offline (the servers are gone). The promo outfits that needed an EA online code (Chris Vacation, Jennifer Rebel, Mattias MetalHead) are unlocked in the wardrobe. |
| **TU4 + DLC** | Mounts the converted content. The DLC is started from Extras > Downloadable Content like on Xbox 360 (own level and master script). The DLC outfits (Obama, Palin, Grandpa Mattias) are always available in the campaign wardrobe, with their names and voice lines. Gamepad navigation to the DLC entry in Extras. |
| **Engine fixes** | Extra string tables: the game stored each new table in the wrong slot, so only the last one added was searched. Asset managers (scrub models and three others) hold at most 256 loaded assets and froze the game when one more was added (e.g. after a helicopter transfer); assets beyond 256 go to an overflow list. The texture pool handed out free textures of a different size with the same power-of-two class (a 1024x1024 texture for a 1920x1080 movie, crash when the menu movie started after a DLC race); only exact sizes are reused. |
| **Lua** | The TU4 and DLC scripts call script functions the PC game does not have; they are added: `Sys.WereCheatsUsed` / `SetCheatsUsed` (cheats), `Object.GetInfiniteAmmo`, `Human.SetChatterSet` (DLC outfit voices), `Hud.Set` / `GetMapCorrectionOffset` (DLC map), `Sys.Get` / `SetMasterScriptName`, `RequiredAsset`, `AddStringDb`, `Sound.OpenStreamFile` / `CloseStreamFile`, `Net.DLCGetMountPoint` and `Net.GetUserName` (DLC). Xbox Live leaderboards (`Net.Leaderboard*`, DLC mission scores) and `Hud.ReleaseLeftStickControl` (Xbox support menu focus) do nothing. Script errors and `Debug.Printf` go to the log; crashes are logged with registers and stack. |
| **Plugins** | `*.asi` files from the game folder, `scripts\` and `plugins\` are loaded at start. |

## Cheats

Open the PDA, type the word (or enter the gamepad code), close the PDA. One code per PDA visit. `SASSY` has to be
entered first.

| PC (type in the PDA) | Gamepad (in the PDA) | Effect |
|---|---|---|
| `SASSY` | LB RB RB LB RB LB LB RB RB RB LB | Enables cheats |
| `NOTTODAYSOLANO` | Up Down Left Down Right Right | Invincibility |
| `TRIGGERHAPPY` | Up Down Left Right Left Left | Infinite ammo |
| `FILLERUP` | Up Up Up Down Down Down | Infinite fuel |
| `AIRDIVA` | Right Left Down Up Right Left Down Up | 25 of every airstrike |
| `MAKEITGLOW` | Up Up Down Down Left Right Left Right | 25 nukes |
| `CARGOCULT` | Left Right Right Left Up Up Left Up | 25 of every supply drop |
| `THATSMYCAR` | Up Down Left Right Right Left | 25 of every vehicle |
| `FASHIONVICTIM` | Up Right Down Left Up | All costumes |
| `HOOKMEUP` | Up Left Down Right Up | Grappling hook |

## Repository layout

```
dll/         dinput8.dll proxy (C, MinGW)         dll/build.sh
  loader/      dinput8.dll: loads Mercenaries2Remastered-1.0.dll or -1.1.dll for the running exe
  v11.map      1.0 -> 1.1 game addresses and changed code patterns
  tools/       retarget.py (sources for 1.1), check_build.py (all Expect() checks against both exes)
  src/         main.c       file split fix, IAT hooks, ASI loader
               d3d.c        D3D9 hooks: borderless, texture tracking, frame callbacks
               gfx.c msaa.c anisotropic filtering, SMAA (third_party/smaa), colour grading, MSAA fix
               deform.c     vehicle deformation: impacts from physics, dent rendering
               vspatch.c    vertex / pixel shader patching for the dents and broken glass
               lod.c        draw distances, camera planes
               pickup.c     automatic ammo pickup
               perf.c       CPU usage fixes
               cull.c statecache.c securom.c forest.c shaderwarm.c drawpool.c stream.c   performance
               post.c       tyre marks
               xbshader.c bloom.c softpart.c emitfix.c treeshadow.c depthaa.c paper.c   Xbox 360 rendering
                            (*_shaders.h, xbshader_patches.h: generated shader bytecode)
               fireprobe.c msm.c dump.c   developer-build diagnostics (M2R_SHOT=1)
               cheats.c     typed PDA cheat words
               mem.c        memory pools (4 GB exe)
               content.c    content mounting, DLC session, Lua bindings, engine fixes, crash log
               assettable.c asset managers past 256 entries
               prompts*.c   Xbox button prompts (data from prompts.bin)
               cam.c aim.c  camera, mounted guns, aim assist
               jitter.c movie.c water.c   twitching loose parts, movie fix, water fix
               pad.c vehicle.c online.c fps.c shot.c util.c dxt.c
  test/        file split test (runs under Wine)
converter/   Xbox 360 -> PC content converter (Python 3, numpy, Pillow, capstone, ffmpeg)
  m2conv/      m2conv.py    driver: inputs, indexing, TU4 pass, DLC, pictures, TU4 + costumes, install
               build_tu4.py TU4 vz-patch / shell-patch (script merge, layers, costumes, menu patches)
               build_dlc.py DLC01.doh -> DLC01.wad (+ audio)
               conv_*.py    asset converters (textures, meshes, layers, strings, scripts, trees, physics)
               prompts.py   Xbox prompt data from the PC game files
               hkreflect.py Havok class layouts from Mercenaries2.exe
  native/      IMA ADPCM encoder used by the audio conversion
setup/       Mercenaries2RemasteredSetup.exe (Win32)
tools/       build_release.sh, gen_default_ini.py, gen_smaa.py
third_party/ SMAA (MIT)
```

## Building

On Linux (or WSL) with `mingw-w64` (both i686 and x86_64), `python3`, `pip`, `curl` and `zip`:

```
tools/build_release.sh
```

This builds the DLL, the installer and the IMA encoder, downloads the embeddable Python 3.11 for Windows,
numpy, Pillow, capstone and ffmpeg (from PyPI / python.org), and writes `dist/Mercenaries2Remastered-Beta1-Setup.zip`.

The DLL alone: `dll/build.sh` (i686-w64-mingw32-gcc) -> `dll/out/dinput8.dll` (loader) and
`dll/out/Mercenaries2Remastered-1.0.dll` / `-1.1.dll`. The sources address the 1.0 exe; the 1.1 DLL is compiled from a copy
in which `tools/retarget.py` replaced every game address and changed byte pattern from `v11.map`. A new game address
needs a line in `v11.map`; `python3 tools/check_build.py <1.0 exe> <1.1 exe>` (in `dll/`) checks every code pattern
against both executables and prints the map lines for patterns whose bytes moved. `M2R_SHOT=1 dll/build.sh` builds a developer DLL with a screenshot key (`[Display] ScreenshotKey`, default F11; Shift+F11 = the same frame without / with the post effects, saved in `Screenshots\`).

The converter also runs directly (Linux or Windows) with Python 3.10+, numpy, Pillow, capstone and ffmpeg
(`M2_FFMPEG` = path to ffmpeg):

```
python3 converter/m2conv/m2conv.py --game <PC game folder> --iso <image> --tu <TU4> --dlc <DLC> [--keep-work]
python3 converter/m2conv/prompts.py --game <PC game folder>
```

## Notes

* No game data is stored here. Everything the tools need is read from the player's own PC and Xbox 360
  files at install time.
* The TU4/DLC conversion is checked against the files listed above (MD5 of the extracted wads); other
  releases are refused unless `--force` is given.
* `converter/m2conv/data/DejaVuSansCondensed-Bold.ttf` is the DejaVu font (license in
  `DejaVu-LICENSE.txt`), used for the DLC menu picture.

## License

Mercenaries2Remastered is free software: you can redistribute it and/or modify it under the terms of the GNU
General Public License, version 3 or (at your option) any later version (`LICENSE`), with an additional
permission to combine it with the game (`LICENSE-EXCEPTION`). Anyone who distributes a modified version, in
source or binary form, must keep the copyright notices and make the complete source available under the same
terms. Authors are listed in `AUTHORS`. Third-party parts keep their own licenses (`third_party/smaa`: MIT,
DejaVu font: `converter/m2conv/data/DejaVu-LICENSE.txt`). The release also bundles Python (PSF license), numpy
(BSD), Pillow (MIT-CMU), capstone (BSD) and the ffmpeg build of imageio-ffmpeg (GPL; source at ffmpeg.org), each
with its own license.

Mercenaries 2: World in Flames is a trademark of Electronic Arts. This project is not affiliated with EA or
Pandemic Studios.
