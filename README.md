# HTL-PC-VR

An **unofficial** PC VR launcher and emulator build for *Here They Lie* (PS4, CUSA05344, update
1.03). It plays **your own copy** of the game in a PC VR headset with tracked controllers, or on
your monitor in the game's own 2D mode at up to 4K and in HDR. It is based on a fork of the
[shadPS4](https://github.com/shadps4-emu/shadPS4) PlayStation 4 emulator.

> **Not affiliated with, endorsed by, or connected to Sony Interactive Entertainment or
> Tangentlemen.** *Here They Lie* is a trademark of its respective owners. This repository and
> its releases contain **no game files**. You need your own legally obtained copy of the game and
> its 1.03 update.

## What it does

- Runs *Here They Lie* through an OpenXR headset: tested on Quest 2 through Virtual Desktop.
  SteamVR and Meta Link should also work.
- Tracked controllers:
  - your left hand follows the left controller while the torch is lit;
  - the right trigger is use/interact and the left trigger switches the torch on and off;
  - the sticks walk and turn;
  - both stick clicks recenter.
- Adds options of its own to the in-game pause menu:
  - Controller Hands;
  - Comfort Vignette;
  - Snap Turning.
- Optional 150% and 200% render resolution, and a darker headset picture.
- Skips the intro movies and startup notes for faster loading.
- Installs the game from your own fake-signed (unencrypted) PS4 packages.
- **No headset needed:** `Play Here They Lie 2D.bat` starts the game's own single-view 2D mode on
  the monitor, fullscreen.
  - The scene is drawn at 3840x2160, 200% of the game's 1920x1080 picture.
  - The mouse turns the camera directly, as in a PC game.
  - WASD walks, and holding Ctrl crouches.
- **HDR:** `Play Here They Lie 2D HDR.bat` shows the same picture as HDR10 on an HDR monitor. Turn
  on HDR in Windows first.

Among the emulator fixes in this build:
- real occlusion queries, which took the game from 20 to 72 fps;
- fixes for corrupted mipmaps and the random colour tint;
- crash fixes in pak file reading;
- frame pacing for 72 Hz;
- the PS4 font warning is now only written to the log.

## Using a release

1. Download the release zip and extract it anywhere. You need about 16 GB free if you install
   the game into it.
2. Drop the base game `.pkg` and its 1.03 update `.pkg` onto `Install game from pkg.bat`. If the
   game is already installed somewhere, start any `Play ... .bat` and pick that folder instead.
3. Start your OpenXR runtime, put the headset on, and double-click `Play Here They Lie VR.bat`.
   Without a headset, double-click `Play Here They Lie 2D.bat` (or the HDR one) instead.

The first runs stutter while shaders are translated. They are kept on your PC, so it gets
smoother as you play.

The release's own `README.txt` covers controls, settings and troubleshooting.

**What it writes into your game folder:** small overlay `.pak` files, next to the game's own paks,
that add the menu options and skip the movies. Your original files are never modified.
`Remove game patches.bat` deletes the overlays again.

## Repository layout

| Path | What it is |
|---|---|
| `src/`, `CMakeLists.txt`, `cmake/`, `externals/` | The emulator: a shadPS4 fork with the *Here They Lie* changes (see `src/core/known_title.cpp`, `src/core/vr/`). |
| `SUBMODULES.txt` | The third-party libraries and the exact commits used. |
| `LICENSES/` | Licence texts of the emulator and its dependencies. |

## Building

Requirements: Windows 10/11 x64, Visual Studio 2022 build tools, CMake, Ninja, LLVM (clang-cl),
the Vulkan SDK and Git.

```powershell
# 1. Fetch the third-party libraries at the commits this build used
powershell -ExecutionPolicy Bypass -File scripts\fetch-externals.ps1

# 2. Configure and build from a "x64 Native Tools" prompt
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release `
      -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl
cmake --build build --target shadps4
```

This builds `shadps4.exe`, the emulator. The release's launcher, `HTL-VR.exe`, is a separate
program and is not part of this repository.

## Licence

- **Emulator:** the emulator code, a fork of shadPS4, is licensed under the
  **GNU General Public License v2.0 or later**. See [`LICENSE`](LICENSE) and `LICENSES/`.
- **Third-party libraries:** they keep their own licences, as listed in `LICENSES/`, `REUSE.toml`
  and their repositories.

*Here They Lie* and all of its assets are the property of their owners and are not part of this
project.

## Credits

- [shadPS4](https://github.com/shadps4-emu/shadPS4) and its contributors, for the emulator this
  build is forked from.
- The authors of the libraries listed in `SUBMODULES.txt`.
