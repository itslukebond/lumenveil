# Installing Lumenveil

This guide covers FINAL FANTASY XV WINDOWS EDITION from Steam, on Windows 10 or 11 and on Linux through
Steam's Proton.

Which upscaler you can use depends on your graphics card:

- NVIDIA DLSS needs a GeForce RTX card. DLSS Frame Generation needs an RTX 40 or 50 series card.
- AMD FSR runs on most DirectX 12 cards.
- Intel XeSS runs on Intel Arc and on other DirectX 12 cards with Shader Model 6.4.

I've tested all three on one card, an NVIDIA GeForce RTX 5090, where AMD's files run FSR 3.1. FSR on AMD Radeon
cards, AMD's FSR 4 and XeSS on Intel Arc are untested.

Linux through Steam Proton is experimental. I've tested it on one NVIDIA system, with the proton-cachyos build of
Proton. See Known issues at the end.

## What you need

- The release zip of this mod, lumenveil-<version>.zip, from the Releases page of the mod's GitHub page (it is the
  zip under Assets).
You need the files for at least one upscaler. Get the ones you want to use.

- NVIDIA's DLSS SDK zip, for DLSS. This mod does not include NVIDIA's files, so you download them from NVIDIA yourself:
  1. Open https://github.com/NVIDIA/DLSS
  2. Choose Code, then Download ZIP. You get a file called DLSS-main.zip.
  3. Leave it in your Downloads folder. The install script finds it there.

  By downloading them you accept NVIDIA's licence for those files.
- AMD's FidelityFX SDK zip, only if you want AMD FSR. This mod does not include AMD's files either:
  1. Download https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/archive/refs/tags/v2.4.0.zip
     (about 200 MB). Your browser saves it as FidelityFX-SDK-2.4.0.zip.
  2. Leave it in your Downloads folder. The install script finds it there.

  By downloading them you accept AMD's licence for those files.
- Intel's XeSS SDK zip, only if you want Intel XeSS. This mod does not include Intel's files either:
  1. Download https://github.com/intel/xess/releases/download/v3.0.2/XeSS_SDK_3.0.2.zip
     (about 77 MB). It is the file listed on Intel's release page, https://github.com/intel/xess/releases/tag/v3.0.2,
     and your browser saves it as XeSS_SDK_3.0.2.zip.
  2. Leave it in your Downloads folder. The install script finds it there.

  By downloading them you accept Intel's licence for those files.

Close the game before you install or uninstall. The scripts refuse to run while it is open.

## Install on Windows

1. Right click the release zip and choose Extract All. Open the extracted folder.
2. Double click install.bat.
   - If Windows shows "Windows protected your PC", choose More info, then Run anyway.
   - If you see "Extract the whole zip to a folder first", you opened install.bat from inside the zip.
     Extract the zip as in step 1, then run install.bat from the extracted folder.
3. The script finds the game through Steam, copies the mod's files next to ffxv_s.exe, takes NVIDIA's two
   files from DLSS-main.zip in your Downloads folder, takes AMD's three from FidelityFX-SDK-2.4.0.zip and Intel's
   three from XeSS_SDK_3.0.2.zip when they are there, and raises the game's frame rate limit from 120 to 240 fps.
   It shows what it did and waits for a key press.
4. Start the game and press Insert for the mod's menu.

If the script cannot find the game, it asks for the game folder. Paste the folder that holds ffxv_s.exe, for
example D:\SteamLibrary\steamapps\common\FINAL FANTASY XV. To open it from Steam, right click the game, then Manage,
Browse local files.

## Install on Linux (experimental)

1. Extract the release zip, open a terminal in the extracted folder and run:

   bash install.sh

2. The script finds the game through Steam, copies the mod's files next to ffxv_s.exe, takes NVIDIA's two files
   from DLSS-main.zip in your Downloads folder, takes AMD's three from FidelityFX-SDK-2.4.0.zip and Intel's three
   from XeSS_SDK_3.0.2.zip when they are there, and raises the game's frame rate limit from 120 to 240 fps.
   If it cannot find the game, it asks for the game folder, or you can name it:

   bash install.sh --game-dir "/path/to/steamapps/common/FINAL FANTASY XV"

3. Steam must load the mod's dxgi.dll instead of Proton's own. The script prints this launch option and copies
   it to the clipboard when it can:

   WINEDLLOVERRIDES="dxgi=n,b" %command%

   In Steam: right click the game, Properties, General, Launch Options, paste. If you already have launch
   options, put WINEDLLOVERRIDES="dxgi=n,b" in front of them and keep one %command%.
4. Start the game and press Insert for the mod's menu.

## What the scripts change

- They copy the mod's files into the game folder and write a list of them to
  ffxv_dlss\installed_files.txt, so the uninstall script removes exactly those files and nothing else.
- If another tool's dxgi.dll is already in the game folder, they keep it as dxgi.dll.before_ffxv_dlss and the
  uninstall script puts it back. If that tool is ReShade, Lumenveil loads it when the game starts, so ReShade keeps
  working. After you update ReShade with its own setup, run the install script again: the newer ReShade replaces the
  kept copy. Any other tool stays off while Lumenveil is installed. README.md, Works with other mods, has the details.
- In the game's settings file GraphicsConfig.ini they set MaxFramerate=240 when it is lower, and keep the
  original file beside it as GraphicsConfig.ini.before_ffxv_dlss. Nothing else in that file changes.
- The update script keeps each file it replaces as <name>.before_update and lists its changes in
  ffxv_dlss\last_update.txt, so the undo script can put them back. The uninstall script removes both.
- They never change the game's exe, the game's own files or Steam's settings.

## Uninstall

Close the game, then run uninstall.bat (Windows) or bash uninstall.sh (Linux) from the extracted release folder.
It removes the files the install script copied, the mod's settings file ffxv_dlss.ini and its logs. It puts back
another tool's dxgi.dll if the install script kept one, and sets MaxFramerate back to what it was, unless you have
changed it since. Files you added to the ffxv_dlss folder yourself stay, and the script lists them.

If you changed the scaling mode in the mod's menu, the game still renders below full resolution. Set Resolution
back to 100% in the game's graphics options. On Linux, also remove WINEDLLOVERRIDES="dxgi=n,b" from the game's Launch Options.

To uninstall by hand, delete dxgi.dll, ffxv_dlss.ini, ffxv_dlss.log and the ffxv_dlss folder from the game folder.
If dxgi.dll.before_ffxv_dlss is there (another tool's dxgi.dll, ReShade for example), rename it back to dxgi.dll.

## Updating DLSS, FSR and XeSS

NVIDIA, AMD and Intel bring out newer versions of their files from time to time. To use a newer one, you don't need to
install again:

1. Download the newer zip or DLL from NVIDIA, AMD or Intel only (the pages linked above). The whole SDK zip is
   fine, and so is a single DLL.
2. Leave it in your Downloads folder.
3. Close the game, then run update.bat (Windows) or bash update.sh (Linux) from the extracted release folder.

The update script looks at every zip and DLL at the top of your Downloads folder and picks the newest version of each
of the eight files the mod uses. It prints one line per file: updated, already up to date, or refused with the reason.

It only uses a file that is a 64-bit Windows DLL with the right name, carries its maker's name and the right product
name, and has a digital signature that names NVIDIA, AMD or Intel. On Windows it also asks Windows whether that
signature is valid. On Linux it can only check that the signature is there, so download from the makers' own pages.
Copies in a development, debug or ARM folder of an SDK are never used. AMD's three files and Intel's three files only
go in together, from one download. It never puts in an older version than the one in place unless you ask for it
with --allow-older (Linux) or -AllowOlder (Windows).

Each file it replaces is kept beside it as <name>.before_update, for example ffxv_dlss\nvngx_dlss.dll.before_update
(about 250 MB if all eight are replaced). To go back, close the game and run undo-update.bat (Windows) or
bash undo-update.sh (Linux). It puts back the files from before the last update. Only the last update can be undone:
the next update that changes something replaces the kept files. If you undo an update and don't want it again, delete
that download, or the next update will pick it up.

NVIDIA's frame generation file nvngx_dlssg.dll is updated too. If frame generation stops working after an update,
run the undo.

The script shows each file's own version number. AMD's three files and Intel's three files each carry their own
number, so these differ from each other and from the FSR version shown in the mod's menu.

To use one file, zip or folder instead of your Downloads folder, give it with --from on Linux
(bash update.sh --from ~/Downloads/DLSS-main.zip) or -From on Windows, from a command prompt in the release folder
(update.bat -From "D:\Downloads\DLSS-main.zip").

Running the install script again keeps any newer file that is already in place.

## Installing by hand

Use this if a script does not work for you. The game folder is the folder that holds ffxv_s.exe.

1. From the release zip, copy the contents of its game folder into the game folder:

   | From the release zip | To the game folder |
   | --- | --- |
   | game\dxgi.dll | dxgi.dll |
   | game\ffxv_dlss\streamline (five Streamline files) | ffxv_dlss\streamline |
   | game\ffxv_dlss\fonts (two fonts and their licence) | ffxv_dlss\fonts |

2. From NVIDIA's DLSS-main.zip, copy these two files out of lib/Windows_x86_64/rel (not the dev folder):

   | NVIDIA file | To the game folder |
   | --- | --- |
   | nvngx_dlss.dll | ffxv_dlss\nvngx_dlss.dll |
   | nvngx_dlssg.dll (Frame Generation) | ffxv_dlss\streamline\nvngx_dlssg.dll |

3. For AMD FSR only, copy these three files out of Kits/FidelityFX/signedbin in AMD's FidelityFX-SDK-2.4.0.zip:

   | AMD file | To the game folder |
   | --- | --- |
   | amd_fidelityfx_loader_dx12.dll | ffxv_dlss\fidelityfx\amd_fidelityfx_loader_dx12.dll |
   | amd_fidelityfx_upscaler_dx12.dll | ffxv_dlss\fidelityfx\amd_fidelityfx_upscaler_dx12.dll |
   | amd_fidelityfx_framegeneration_dx12.dll (FSR frame generation) | ffxv_dlss\fidelityfx\amd_fidelityfx_framegeneration_dx12.dll |

4. For Intel XeSS only, copy these three files out of the bin folder in Intel's XeSS_SDK_3.0.2.zip (not
   libxess_dx11.dll):

   | Intel file | To the game folder |
   | --- | --- |
   | libxess.dll | ffxv_dlss\xess\libxess.dll |
   | libxess_fg.dll (XeSS frame generation) | ffxv_dlss\xess\libxess_fg.dll |
   | libxell.dll (XeSS frame generation) | ffxv_dlss\xess\libxell.dll |

5. Raise the game's frame rate limit: open GraphicsConfig.ini in a text editor and change the MaxFramerate line
   to MaxFramerate=240. The file is in:
   - Windows: Documents\My Games\FINAL FANTASY XV\Steam\<number>\savestorage\GraphicsConfig.ini
   - Linux: <Steam library>/steamapps/compatdata/637650/pfx/drive_c/users/steamuser/Documents/My Games/FINAL FANTASY XV/Steam/<number>/savestorage/GraphicsConfig.ini

   The game creates the file the first time you change a graphics option.
6. Linux only: set the launch option WINEDLLOVERRIDES="dxgi=n,b" %command% as in Install on Linux.

## Troubleshooting

- The script cannot find the game: give it the game folder when it asks. On Linux you can also use
  bash install.sh --game-dir followed by the folder in quotes.
- install.bat closes at once or will not start: a company or school computer may block PowerShell scripts. Install
  by hand as above.
- "Could not copy" on Windows: if the game is under Program Files, right click install.bat and choose Run as
  administrator.
- NVIDIA's files were not found: check that DLSS-main.zip (or the folder you extracted from it) is directly in
  your Downloads folder, not in a subfolder, then run the install script again. You can also copy the two files by
  hand as above. The install script can be run again at any time. Only DLSS and DLSS Frame Generation need them.
- AMD's FSR files were not found: check that FidelityFX-SDK-2.4.0.zip (or the folder you extracted from it) is
  directly in your Downloads folder, then run the install script again. On Linux you can name the zip with
  bash install.sh --fsr followed by its path in quotes. Or copy the three files by hand as above. Only AMD FSR needs
  them.
- Intel's XeSS files were not found: check that XeSS_SDK_3.0.2.zip (or the folder you extracted from it) is directly
  in your Downloads folder, then run the install script again. On Linux you can name the zip with
  bash install.sh --xess followed by its path in quotes. Or copy the three files by hand as above. Only Intel XeSS
  needs them.
- XeSS frame generation is unavailable: it needs libxess_fg.dll and libxell.dll beside libxess.dll in
  ffxv_dlss\xess. Copy all three by hand as above, or run the install script again with the whole zip.
- The settings file was not found: start the game once, change any graphics option, quit, then run the install
  script again. Or set MaxFramerate=240 by hand as above.
- The frame rate stops at 120 again after you change graphics options in the game: run the install script again,
  or set MaxFramerate=240 by hand.
- The menu says a DLL is missing: it names the file and where it goes. See Installing by hand, steps 2 to 4.
- The game will not start, or freezes: create (or open) ffxv_dlss.ini in the game folder and add these two lines,
  which turn off the mod's own presentation (the menu, fps counter and Frame Generation go with it):

  [d3d12]
  present=0

- The mod's log is ffxv_dlss.log in the game folder. Include it when you report a problem.

## Known issues

- Quitting the game can crash during Frame Generation shutdown. Save before you quit.
- Linux with an NVIDIA card: the game can freeze, most often as a save finishes loading or a battle starts. Players
  see the same freeze without the mod on NVIDIA's newer Linux drivers
  (https://github.com/ValveSoftware/Proton/issues/74), and there is no fix from NVIDIA yet. If it happens, close the
  game and start it again. One build of Proton Experimental also stops at the loading screen; proton-cachyos works
  (in Steam: Properties, Compatibility).
- AMD FSR. Tested on an NVIDIA GeForce RTX 5090 only, where AMD's files run FSR 3.1. Untested on AMD Radeon cards
  and with AMD's FSR 4. If FSR frame generation looks wrong on your card, turn it off in the mod's menu and keep FSR
  upscaling.
- Intel XeSS. Tested on an NVIDIA GeForce RTX 5090 only. Untested on Intel Arc.
