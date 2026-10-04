<img src="https://github.com/ares-emulator/ares/blob/master/ares/ares/resource/logo@2x.png" width="350"/>

[![License: ISC](https://img.shields.io/badge/License-ISC-blue.svg)](https://github.com/higan-emu/ares/blob/master/LICENSE)

**ares** is a multi-system emulator that began development on October 14th, 2004.
It is a descendant of [higan](https://github.com/higan-emu/higan) and [bsnes](https://github.com/bsnes-emu/bsnes/), and focuses on accuracy and preservation.

It's worth noting that ares takes some uncommon design approaches that essentially trade speed for code clarity. We avoid state machines and bitmasks (when possible). Most cores end up being half the amount of code, but slower. The code is clearer and less spaghettified, especially for systems with lots of processors. C bitfields being non-portable incurs a speedhit. Windows also has a speedhit over Linux due to its ABI needing more instructions to switch contexts.

Experimental side-by-side 3D (this fork)
----------------------------------------

This fork adds stereoscopic output for Nintendo 64 (including 64DD) and
PlayStation. Open **Settings → Video** to show the **Configuration** window,
then select **Stereoscopic 3D → Mode → Side by Side**. Reload the game after
changing any stereo setting. Stereo is off by default and does not affect
other systems.

* **Depth** controls eye separation; zero produces two identical views.
* **Convergence** moves the plane of zero disparity. Its scale depends on the
  game's projection, so start with low depth and adjust per game.
* **Swap Eyes** reverses left and right for displays that expect the other order.
* **Full SBS Width** preserves each eye's normal aspect ratio. Leave it unchecked
  for a display's half-SBS mode, which expands each half to the whole screen.

The off-axis projection follows the approach in
[Dolphin's stereo shader](https://github.com/dolphin-emu/dolphin/blob/master/Source/Core/VideoCommon/GeometryShaderGen.cpp):
eye separation depends on depth relative to a convergence plane. The local
`dolphin-3d` Metal stereoscopy implementation was also used as an architectural
reference. This is an independent implementation for ares' rendering pipelines.
On macOS it works with ares' **Metal** video driver; N64 rasterization continues
to use paraLLEl-RDP through **MoltenVK**. No geometry-shader support is required.

Both cores render separate eye images while preserving the original mono
memory for emulated reads. They must recover depth because their rasterizers
receive projected coordinates:

* **Nintendo 64:** Z-buffered triangles use projected Z to reconstruct relative
  depth. Edges and texture/color/depth gradients are transformed together.
  Rectangles, triangles without Z, and primitives using a constant depth source
  remain flat. Nonstandard depth mappings can produce incorrect stereo.
* **PlayStation:** GPU vertices are matched to recent GTE projections. Missing
  or ambiguous matches stay flat. Games using software transforms or reusing
  screen coordinates for different depths can have partial or unstable stereo.
  Sprites and video remain flat.

This is experimental, not a verified game compatibility release. Drawing three
views costs more CPU/GPU time and memory; PlayStation stereo also renders
synchronously. Save states retain the normal format and rebuild auxiliary eye
buffers after loading. Depth settings are global, not saved per game. Rotation,
cropping, and display shaders which mix adjacent pixels can interfere with SBS
presentation; start with the normal unrotated, uncropped output.

The optional regression harness needs N64 and PS1 cores and a working Vulkan
device (MoltenVK on macOS). It uses synthetic scenes and needs no ROM or BIOS:

```sh
cmake -S . -B build -G Ninja -DARES_BUILD_STEREO_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

On macOS, select a full Xcode installation or prefix configure/build commands
with `DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer`. Tests cover
depth/convergence math, GTE matching, separate eye rasterization, unchanged
guest memory, zero-depth mono equivalence, eye swapping, CPU/VRAM transfers,
SBS pixel packing, and N64 2× rendering. Real-game compatibility and non-macOS
runtime behavior have not yet been verified.

Official Releases
-----------------

Official releases are available from
[the ares website](https://ares-emu.net).

Nightly Builds
--------------

Automated, untested builds of ares are available for Windows and macOS as a [pre-release](https://github.com/ares-emulator/ares/releases/tag/nightly). 
Only the latest nightly build is kept.

Building ares
-------------

ares supports building on Windows, macOS, and various Linux/BSD distributions. See build instructions for:

* [Windows](https://github.com/ares-emulator/ares/wiki/Build-Instructions-For-Windows)
* [macOS](https://github.com/ares-emulator/ares/wiki/Build-Instructions-For-macOS)
* [Linux](https://github.com/ares-emulator/ares/wiki/Build-Instructions-For-Linux)
* [BSD](https://github.com/ares-emulator/ares/wiki/Build-Instructions-For-BSD)

Command-line options
--------------------

When started from the command-line, ares accepts a few options.

```
Usage: ./ares [options] game(s)

  --help                 Displays available options and exit
  --version              Displays the version string of the application
  --terminal             Create new terminal window (Windows only)
  --fullscreen           Start in full screen mode
  --pseudofullscreen     Start in pseudo full screen mode
  --kiosk                Start in minimal UI mode (implies --no-file-prompt)
  --system system        Specify the system name
  --shader shader        Specify a slang shader to load (requires OpenGL or Metal)
  --setting name=value   Specify a value for a setting
  --dump-all-settings    Show a list of all existing settings and exit
  --no-file-prompt       Do not prompt to load (optional) additional roms (eg: 64DD)
  --settings-file path   Specify a settings file override (settings.bml)
  --save-state slot      Specify a save state slot to load (1-9)
```

The --system option is useful when the system type cannot be auto-detected.
--fullscreen will only have an effect if a game is also passed in argument.

Example:
`ares --system MSX examples.rom --fullscreen`

Specifying multiple games allows for multi-cart support.  For example, to load
the Super GameBoy BIOS and a game in one command (to avoid a file prompt), you 
can do:

`ares "Super GameBoy.sfc" "Super Mario Land.gb"`

The --no-file-prompt option is useful if you wish to launch a game from CLI
without being prompted to load additional roms. For example, some Nintendo 64 
games optionally support 64DD expansion disks, so this option can be used to
suppress the "64DD Disk" file dialog, and assume any secondary content is 
disconnected.

High-level Components
---------------------

* __ares__:       emulator cores and component implementations
* __desktop-ui__: main GUI implementation for this project
* __hiro__:       cross-platform GUI toolkit that utilizes native APIs on supported platforms
* __nall__:       Near's alternative to the C++ standard library
* __ruby__:       interface between a hiro application and platform-specific APIs for emulator video, audio, and input
* __mia__:        internal ROM database and ROM/image loader
* __libco__:      cooperative multithreading library

Contributing
------------

Please join our discord to chat with other ares developers: https://discord.com/invite/gz2quhk2kv
