ATOMIC MGPU BRIDGE 3.0 - INSTALL
--------------------------------

A ReShade add-on for PCs with two NVIDIA graphics cards: the game renders on
the first card, and the second card runs DLSS Neural Rendering, frame
generation and the display. A fork of MGPU Bridge by Marcelo Guibout
(github.com/maohgad-web/Neural-coprocessor).

This is a hobby project, not a product. Games with anti-cheat and online
games are at your own risk.

WHAT YOU NEED
-------------

- Two NVIDIA graphics cards. The SECOND card runs DLSS Neural Rendering,
  which NVIDIA documents as an RTX 50-series feature (RTX 40-series has been
  reported working - an RTX 4090, in Cyberpunk 2077). USE AN RTX 50- OR
  40-SERIES CARD AS THE SECOND CARD. On a card
  that cannot run it, the driver refuses it ("FeatureNotSupported",
  0xBAD00001) and you get the picture without neural rendering. The
  installer's Check and the log both warn when no RTX 50- or 40-series card is found.

- A monitor plugged into the SECOND card. Recommended: the card that renders
  the game runs HEADLESS (no monitor at all), and your monitor hangs off the
  second card, which does the neural work, frame generation and the display.
  The installer's check shows which card each monitor is plugged into.

- For HDR: turn on HDR in Windows' display settings. The picture follows each
  game's own HDR setting; HDR=1 also turns SDR games into HDR.

- ReShade 6.8.0 or newer, the version WITH ADD-ON SUPPORT (reshade.me).
  The effects-only version never loads add-ons and does not say so. If
  ReShade.log has no line saying the "Atomic MGPU Bridge" add-on was
  registered, this is why. You do not need any effect packages.

- A DirectX 12 game. DirectX 11 games are experimental (DX11=1 in mgpu.ini).
  Vulkan games are not supported.

- Your own nvngx_dlssnr.dll (the DLSS Neural Rendering module). This download
  does not include it and cannot: nothing from NVIDIA may be redistributed.
  Do not put it, or any other NVIDIA file, in a package you give to someone.

- Do not run another neural rendering add-on at the same time.

INSTALL - THE EASY WAY
----------------------

1. Unpack this zip anywhere and run AtomicMGPU-Setup.exe from that folder.
2. Pick the game (Steam games are listed; "Browse..." for any other game's
   .exe) and click Install / Update. The game must be closed.
3. YOUR OWN nvngx_dlssnr.dll: the installer looks for a copy already on this
   PC (for example in another game set up for the bridge) and offers to copy
   it into the game's mgpu folder; "Add my nvngx_dlssnr.dll..." lets you pick
   it. It is not included and may not be redistributed.

The installer checks the folder first and says what is missing. It adds
ReShade (with add-on support) as dxgi.dll if the game has none, puts AMD's
frame generation DLL (for AMD FSR 3) in the mgpu folder, keeps your
mgpu.ini when you update, and Uninstall removes exactly what it put in.

INSTALL - BY HAND
-----------------

Everything goes in the folder that holds the game's .exe. In Steam:
right-click the game -> Manage -> Browse local files, then go down until you
see the .exe (for Unreal Engine games usually ...\<Game>\Binaries\Win64\).

When you are done it should look like this:

<the folder with the game .exe>\
    Game.exe                          the game
    dxgi.dll                          ReShade with add-on support (made by the
                                      ReShade installer; some games use
                                      d3d12.dll instead - either is fine)
    ReShade.ini                       made by ReShade on first run
    nvngx.dll_mgpu_bridge.addon64     <- from this download
    mgpu.ini                          <- from this download
    gpu1.ini                          <- from this download
    ReShade2.ini                      <- from this download
    mgpu\
        nvngx_dlssnr.dll              <- yours, NOT beside the .exe
    reshade-shaders\Shaders\
        mgpu_depth_tap.fx             <- from this download

1. Make a folder called mgpu beside the game's .exe and put your
   nvngx_dlssnr.dll in it. Do not leave a copy beside the .exe: some games
   load it from there onto the wrong card. The panel says INSTALL PROBLEM if
   it finds one.

2. Put mgpu_depth_tap.fx in ReShade's Shaders folder (usually
   reshade-shaders\Shaders in the game folder). You do not need to enable it;
   the add-on does that. Without it the bridge never gets the game's depth
   and never starts. Check that EffectSearchPaths in ReShade.ini points at
   that folder - ReShade only loads shaders it finds there.

3. Do not rename the add-on file. Its name must contain "nvngx.dll", or the
   neural rendering module refuses to work.

4. Each game folder needs its own mgpu.ini, beside the add-on.

FIRST RUN
---------

Start the game and get into gameplay. The bridge starts by itself
(AutoArm=1) once the game is drawing a 3D scene; in a menu or a loading
screen it waits, which can take up to about a minute. That is normal.

The picture appears on the monitor plugged into the second card, in a window
on top of the game. The game keeps the keyboard and the mouse.

While it starts, a tray icon (bottom right of the taskbar) shows what it is
doing: grey waiting, amber arming, green running, red error. Hover over it
for details, click it for a menu.

Set G-SYNC to "full screen only" in the NVIDIA Control Panel.

Turn the game's own DLSS Super Resolution on (any quality mode), so the game
produces motion vectors.

For 3x or 4x frame generation, cap the game's frame rate (in the game, RTSS
or the NVIDIA Control Panel) so that game fps times the multiplier fits your
monitor - about 40 fps for 4x on a 240 Hz screen.

Do not change the game's resolution while the bridge is running. Restart the
game after changing it. Changing DLSS quality or the DLSS model while playing
is fine: the bridge follows it within a second.

THE PANEL AND THE HOTKEYS
-------------------------

Home opens the ReShade overlay with the Atomic MGPU Bridge panel. Every
setting is there and is saved to mgpu.ini.

CTRL+ALT+F10        start the bridge by hand (with AutoArm=0)
CTRL+ALT+F6         hide / show the picture
CTRL+ALT+F7         view: output / input / split
CTRL+ALT+F3 / F4    move the split line left / right (SHIFT: big steps)
CTRL+ALT+F5         pick the pass the intensity keys change
CTRL+ALT+F1 / F2    intensity down / up
CTRL+ALT+H          FPS HUD
CTRL+ALT+G          frame generation on / off
CTRL+ALT+P          screenshot
CTRL+ALT+R          video recording start / stop
CTRL+ALT+B          benchmark start / stop
CTRL+ALT+W          benchmark sweep start / stop
CTRL+ALT+C / S      clock / stopwatch

All of them can be changed in the panel.

IF SOMETHING GOES WRONG
-----------------------

The picture opens on the wrong monitor: set Monitor=0 (or 1, 2...) in
mgpu.ini.

Nothing happens at all: ReShade is the effects-only version, or the add-on
was renamed.

The bridge never starts: mgpu_depth_tap.fx is missing or not in
EffectSearchPaths, or you are still in a menu.

The neural picture never appears: nvngx_dlssnr.dll is missing from the mgpu
folder.

To report a problem: set DetailedLog=1 in mgpu.ini (or tick "Detailed log" in
the panel), reproduce it, and send ReShade.log from the game folder.

LICENSE
-------

MIT - see LICENSE. The add-on and its design are Marcelo Guibout's; this
fork's changes are by davidm71.
