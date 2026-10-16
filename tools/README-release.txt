Mercenaries2Remastered Beta 2 - Mercenaries 2: World in Flames (PC)
===================================================================

Works with Mercenaries2.exe 1.0 and 1.1 (the versions without the disc check).

Run Mercenaries2RemasteredSetup.exe, pick the game folder and choose:

  * Mod + Xbox 360 Title Update 4 and the "Blow It Up Again Pack" DLC
      Builds PC versions of the Xbox 360 Title Update 4 changes and the DLC
      from your own Xbox 360 files, on your PC:
        - Mercenaries 2 Xbox 360 game disc image (.iso, Redump "(Europe) (En,Es,Ru)")
        - Title Update 4 package (TU_12K2218_...), or the .zip it came in
        - "Blow It Up Again Pack" DLC package, or the .zip it came in
      Takes a few minutes and about 1.5 GB of free space while it works.
      If it is stopped, pressing Install again continues where it stopped.

  * Mod only
      The fixes and features without any Xbox 360 content.

Both options also extract the Xbox button glyphs (shown in prompts while a gamepad
is used) from your PC game files into Mercenaries2Remastered\prompts.bin.

Everything goes into the game folder as dinput8.dll, Mercenaries2Remastered.ini and
Mercenaries2Remastered\ (the mod for each game version and the converted content). Mercenaries2.exe is made 4 GB (large address) aware; the
original is kept as Mercenaries2.bak. Uninstall removes everything and puts the
original exe back.

After installing
  - DLC: Extras > Downloadable Content.
  - DLC outfits (Obaminator, Sarahcuda, Gramps) are in the PMC wardrobe.
  - Mission briefings keep the outfit you wear (except Obaminator, Sarahcuda
    and the chicken suit, which still switch to the default outfit).
  - The transport (landing zone) map is shown in the Xbox 850x720 box.
  - The promo outfits that needed an EA online code (Chris Vacation, Jennifer
    Rebel, Mattias MetalHead) are unlocked.
  - Settings: Mercenaries2Remastered.ini in the game folder (borderless window, camera,
    gamepad, aim assist, vehicles, car deformation, graphics, ...). Log: Mercenaries2Remastered.log.
  - Cheats: open the PDA, type a word, close the PDA. First SASSY (enables
    cheats), then: NOTTODAYSOLANO invincibility, TRIGGERHAPPY infinite ammo,
    FILLERUP fuel, AIRDIVA airstrikes, MAKEITGLOW nukes, CARGOCULT supplies,
    THATSMYCAR vehicles, FASHIONVICTIM costumes, HOOKMEUP grappling hook.
    One word per PDA visit.
  - Keys: F9 FPS counter, F10 colour grading on/off.
  - Graphics as on the Xbox 360 ([Graphics] in the ini): correct normal maps, filtered
    shadows on vegetation, unclamped lighting, linear HDR bloom, soft particles,
    fire on burning bushes, trees and wrecks, model shadows of far trees, paper
    sheets in the streets, and the Xbox edge anti-aliasing (DepthAA, used while
    the game's Anti-Aliasing is off; the Xbox has no MSAA).
  - Aim assist (gamepad): hip fire marks the enemy nearest the view centre in red and
    sends the shots to it; aiming down the sight snaps to the enemy nearest the
    reticle. The game's own aim magnetism (on foot and on mounted guns) is off.
  - This is a beta: please report problems with Mercenaries2Remastered.log attached.

Converter command line (same thing the setup runs):
  converter\python\python.exe converter\m2conv\m2conv.py --game <game folder>
      --iso <image> --tu <TU4 package or zip> --dlc <DLC package or zip>

License: GNU GPL version 3 or later with an additional permission to combine it with
the game (LICENSE, LICENSE-EXCEPTION). Source code and authors: see the project page.
