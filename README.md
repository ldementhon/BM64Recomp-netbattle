# Bomberman 64: Recompiled
[Bomberman 64: Recompiled](https://github.com/RevoSucks/BM64Recomp) is a project that uses [N64: Recompiled](https://github.com/Mr-Wiseguy/N64Recomp) to **statically recompile** Bomberman 64 into a native port with many new features, enhancements, and extensive mod support. This project uses [RT64](https://github.com/rt64/rt64) as the rendering engine to provide some of these enhancements.

### **This repository and its releases do not contain game assets. The original game is required to build or run this project.**

> **Online Battle fork.** The `netbattle` branch adds four-player online Battle with rollback netcode for Apple Silicon Mac and Windows x64. See [Online Battle development](#online-battle-development) for building, testing, running a server and making releases. `master` mirrors upstream.

## Table of Contents
* [System Requirements](#system-requirements)
* [Features](#features)
  * [Plug and Play](#plug-and-play)
  * [Fully Intact N64 Effects](#fully-intact-n64-effects)
  * [Easy-to-Use Menus](#easy-to-use-menus)
  * [High Framerate Support](#high-framerate-support)
  * [Widescreen and Ultrawide Support](#widescreen-and-ultrawide-support)
  * [Additional Control Options](#additional-control-options)
  * [Low Input Lag](#low-input-lag)
  * [Instant Load Times](#instant-load-times)
  * [Linux and Steam Deck Support](#linux-and-steam-deck-support)
* [Planned Features](#planned-features)
* [FAQ](#faq)
* [Known Issues](#known-issues)
* [Building](#building)
* [Online Battle development](#online-battle-development)
* [Libraries Used and Projects Referenced](#libraries-used-and-projects-referenced)

## System Requirements
Currently, only a **Windows** exe is provided but other operating systems will be supported soon.<br/>
A GPU supporting Direct3D 12.0 (Shader Model 6), Vulkan 1.2, or Metal Argument Buffers Tier 2 support is required to run this project. The oldest GPUs that should be supported for each vendor are:
* GeForce GT 630
* Radeon HD 7750 (the one from 2012, not to be confused with the RX 7000 series) and newer
* Intel HD 510 (Skylake)
* A Mac with Apple Silicon or an Intel 7th Gen CPU with MacOS 13.0+

On x86-64 PCs, a CPU supporting the SSE4.1 instruction set is also required (Intel Core 2 Penryn series or AMD Bulldozer and newer). ARM64 builds will work on any ARM64 CPU.

If you have issues with crashes on startup, make sure your graphics drivers are fully up to date. 

## Features

#### Plug and Play
Simply provide your copy of the 1.0 North American version of the game in the main menu and start playing! This project will automatically load assets from the provided copy, so there is no need to go through a separate extraction step or build the game yourself. Other versions of the game may be supported in the future.

#### Fully Intact N64 Effects
A lot of care was put into RT64 to make sure all graphical effects were rendered exactly as they did originally on the N64. No workarounds or "hacks" were made to replicate these effects, with the only modifications to them being made for enhancement purposes such as widescreen support.

#### Easy-to-Use Menus
Gameplay settings, graphics settings, input mappings, and audio settings can all be configured with the in-game config menu. The menus can all be used with mouse, controller, or keyboard for maximum convenience.

#### High Framerate Support
Play at any framerate you want thanks to functionality provided by RT64! Game objects and terrain, texture scrolling, screen effects, and most HUD elements are all rendered at high framerates. By default, this project is configured to run at your monitor's refresh rate. You can also play at the original framerate of the game if you prefer. **Changing framerate has no effect on gameplay.**

**Note**: External framerate limiters (such as the NVIDIA Control Panel) are known to potentially cause problems, so if you notice any stuttering then turn them off and use the manual framerate slider in the in-game graphics menu instead.

#### Widescreen and Ultrawide Support
Any aspect ratio is supported, with most effects modded to work correctly in widescreen. The HUD can also be positioned at 16:9 when using ultrawide aspect ratios if preferred.

**Note**: Some animation quirks can be seen at the edges of the screen in certain cutscenes when using very wide aspect ratios.

#### Mod Support
Install community made mods and texture packs! Mods can change any part of the game, including adding completely new features and content. You can install mods by simply dragging the mod files onto the game window before starting the game or by clicking the **Install Mods** button in the mod menu. Mods can be toggled in the mod menu, and some mods can be configured there as well.

If you're interested in making texture packs, check out [the RT64 documentation](https://github.com/rt64/rt64/blob/main/TEXTURE-PACKS.md).

#### Additional Control Options
Customize your experience by setting your stick deadzone to your liking, as well as adjusting the Y axis inversion for aiming.

For local multiplayer, open **Controls**, select **P1–P4**, and choose **Controller**, **Keyboard**, or **Off** for each player before starting the game. Enabling a controller player assigns the next unused gamepad; the dropdown lets you change it explicitly. Bindings, sensitivity, and calibration are saved separately for each player. P1 defaults to keyboard and P2–P4 default to off. Existing single-player bindings are retained on P1.

If releasing a stick makes your player turn around, select **Calibrate Stick** for that player. Leave the stick centered, circle its full range, then follow the eight release prompts (twice in each direction) and save. Calibration measures the rebound and adds a safety margin to the deadzone. The global deadzone remains a minimum; the two deadzones are not added together. Recalibrate after changing controllers or movement-axis bindings. Controller assignment follows connection order on startup, so check the player assignments before playing.

Four-player controls and stick calibration were adapted from [pmaccamp's fork](https://github.com/pmaccamp/BM64Recomp/commit/9c7d7e9b215797492982a2216baf95f9f3ccc8d1). Local fixes preserve diagonal stick direction, avoid taking another player's controller when enabling a port, handle disconnects safely, and limit binding resets to the selected player. Controller Pak costume storage is not emulated; Battle character select uses the built-in characters.

#### Low Input Lag
This project has been optimized to have as little input lag as possible, making the game feel more responsive than ever!

#### Instant Load Times
Saving and loading files, going from place to place, and pausing all happen in the blink of an eye thanks to the game running natively on modern hardware.

#### Linux and Steam Deck Support
A Linux binary is available for playing on most up-to-date distros, including on the Steam Deck.

## FAQ

#### What is static recompilation?
Static recompilation is the process of automatically translating an application from one platform to another. For more details, check out the full description of how this project's recompilation works here: [N64: Recompiled](https://github.com/Mr-Wiseguy/N64Recomp).

#### How is this related to the decompilation project?
Unlike N64 ports in the past, this project is not based on the source code provided by a decompilation of the game. This is because static recompilation bypasses the need for decompiled source code when making a port, allowing ports to be made **without source code**. However, the reverse engineering work done by the decompilation team was invaluable for providing some of the enhancements featured in this project. For this reason, the project uses headers and some functions from the [Decompilation Project](https://github.com/RevoSucks/BM64Recomp) in order to make modifications to the game. Many thanks to the decompilation team for all of the hard work they've done.

#### Where is the savefile stored?
- Windows: `%LOCALAPPDATA%\BM64Recompiled\saves`
- Linux: `~/.config/BM64Recompiled/saves`
- macOS: `~/Library/Application Support/BM64Recompiled/saves`

#### How do I choose a different ROM?
**You don't.** This project is **only** a port of Bomberman 64, and it will only accept one specific ROM: the US version of the N64 release of Bomberman 64. ROMs in formats other than .z64 will be automatically converted, as long as it is the correct ROM. **It is not an emulator and it cannot run any arbitrary ROM.** 

Instead, you can change the game by installing mods. See the [mod support](#mod-support) section for details.

#### Can you run this project as a portable application?
Yes, if you place a file named `portable.txt` in the same folder as the executable then this project will run in portable mode. In portable mode, the save files, config files, and mods are placed in the same folder as the executable.

## Known Issues
* Overlays such as MSI Afterburner and other software such as Wallpaper Engine can cause performance issues with this project that prevent the game from rendering correctly. Disabling such software is recommended.

## Building
Building instructions on how to build this project can be found in the [BUILDING.md](BUILDING.md) file.

Controller regression checks on macOS/Linux: after configuring a build with `CMAKE_EXPORT_COMPILE_COMMANDS=ON`, run `python3 tests/run_controller_tests.py build` (replace `build` with your build directory). The tests use SDL virtual gamepads and temporary settings; no physical controllers or ROM are required. Set `SDL2_CONFIG` if `sdl2-config` is installed outside the configured SDL2 prefix.

## Online Battle development

This fork adds online Battle: a lobby server, rollback netcode, and an **Online Battle** screen in the launcher. It targets Apple Silicon Macs and Windows x64; Linux builds are not set up for online play. The design, status and test history are in [experiments/online-lobby-plan.md](experiments/online-lobby-plan.md); the server and protocol are described in [server/README.md](server/README.md); [AGENTS.md](AGENTS.md) lists the working rules.

### Branches

- `master` mirrors [upstream](https://github.com/RevoSucks/BM64Recomp). Don't commit to it; fast-forward it from upstream instead.
- `netbattle` is the shared branch for online work. Branch from it for each change and merge back into it, for example through a pull request.
- To pick up upstream fixes:
  ```bash
  git fetch upstream
  git switch master
  git merge --ff-only upstream/master
  git push origin master
  git switch netbattle
  git merge master
  ```

### Dependencies are patched, not forked

The submodules stay pinned to their upstream commits. This fork's changes to them live in [experiments/patches/](experiments/patches/), and the Mac build script applies them; patched submodules then show as modified in `git status`. Never commit a submodule pointer change. If you change dependency code, update the matching patch file (or add a new one and apply it in `experiments/build-macos.sh`). [experiments/STATE_REPLAY.md](experiments/STATE_REPLAY.md) lists what each patch does.

### Building on a Mac

Requirements: an Apple Silicon Mac, Xcode with its Metal toolchain, CMake, Ninja, LLVM 18 (`brew install llvm@18`), and your own US 1.0 ROM in `.z64` format (SHA-256 `e6da7c26127788cd894b88b71cc055ff9dec0d0f4f8e10d9b15b40153af2b52a`). Never commit or share the ROM, or anything generated from it.

1. Clone with submodules, then put the pinned tools next to the repository in `../tools` (or set `BM64_TOOLS_ROOT`):
   ```bash
   git clone --recurse-submodules -b netbattle https://github.com/ldementhon/BM64Recomp-netbattle.git BM64Recomp
   mkdir tools && cd tools
   git clone --recurse-submodules https://github.com/N64Recomp/N64Recomp
   git -C N64Recomp checkout 98bf104b1b5ed83126af8bcab0cc964782617dbf && git -C N64Recomp submodule update --init --recursive
   git clone https://github.com/bomberhackers/bm64
   git -C bm64 checkout c7c5fd5a73a9d5969673535030fc4f1df956fa5f
   git clone https://github.com/libsdl-org/SDL SDL2
   git -C SDL2 checkout fb1497566c5a05e2babdcf45ef0ab5c7cca2c4ae
   cd ../BM64Recomp
   ```
2. Build everything (dependency patches, SDL2, N64Recomp, ROM decompression, code generation, the game):
   ```bash
   sh experiments/build-macos.sh "/path/to/Bomberman 64 (USA).z64"
   ```
   If `xcrun metal` can't be found, set `TOOLCHAINS` to your Metal toolchain's identifier (shown by `xcrun --find metal` errors, or under `/Library/Developer/Toolchains`).
3. After that, rebuild the game with:
   ```bash
   cmake --build build --target BM64Recompiled -j 8
   ```
   The app is `build/BM64Recompiled.app`. Choose the ROM on first launch.
4. Build and test the lobby server and network code:
   ```bash
   cmake -S server -B build/lobby -DCMAKE_BUILD_TYPE=Release
   cmake --build build/lobby -j 8
   ctest --test-dir build/lobby --output-on-failure
   ```

### Building on Windows

The Windows game is built from a source bundle that a Mac exports after its own build. The bundle carries the guest code generated on the Mac, so both platforms of a release run identical game code; building it separately on each machine could make online matches drift apart. The bundle is generated from the ROM, so keep it private.

1. On the Mac, after building:
   ```bash
   python3 experiments/export-online-source.py build/share/windows-source.tar.gz
   ```
2. Copy the file to the PC and extract it there (`tar -xzf windows-source.tar.gz` creates `source/`).
3. Install Visual Studio 2022 with Desktop development with C++, plus CMake, Ninja and LLVM (clang-cl; 19.1.3 is what has been tested). In an **x64 Native Tools Command Prompt for VS 2022** with those on `PATH`:
   ```bat
   cd source
   powershell -ExecutionPolicy Bypass -File experiments\build-windows.ps1
   cmake -S server -B build-lobby -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl
   cmake --build build-lobby
   ctest --test-dir build-lobby --output-on-failure
   ```
   The script checks every file against the bundle's manifest, then leaves `build\BM64Recompiled.exe` with its DLLs, `assets` and `recompcontrollerdb.txt`.

### Checks

Run these on the Mac before sharing a change, from the repository root (the game checks need the ROM at `../Bomberman 64 (USA).z64`):

```bash
ctest --test-dir build/lobby --output-on-failure
python3 tests/run_controller_tests.py
sh tests/run_replay_input_tests.sh
sh tests/run_game_session_tests.sh
python3 tests/run_fingerprint.py
python3 tests/run_network_game.py baseline
python3 tests/run_network_game.py pause
```

`run_network_game.py` with no argument runs every network mode: four real game processes through a local server, with delay, packet loss and rollbacks, checking that their states agree. Changes to gameplay, input or timing also need a Mac/Windows match; `experiments/online-lobby-plan.md` records how those were run.

### Running a server

```bash
build/lobby/bm64-lobby 0.0.0.0 6464 build/share/release-v2.txt
```

Players need UDP port 6464 forwarded to the host, or a shared network such as Tailscale. On Windows, allow the port in Windows Firewall. Details are in [server/README.md](server/README.md).

### Releases and version numbers

Each Mac + Windows pair sent to players is one numbered online release (v1, v2, ...). The number is `BM64_ONLINE_RELEASE` in [src/online/release.hpp](src/online/release.hpp). The launcher shows it, and players and servers on different numbers are told which download to get.

- **When to bump:** for every new pair of downloads, even if the code barely changed. A number must never be reused for different files.
- **When to also bump the protocol:** if you change what the lobby server and clients send each other, also bump `protocol_version` in `src/online/lobby_protocol.hpp` and the `BM64 online vN` text in `src/online/fingerprint.hpp`, `tests/run_fingerprint.py` and `tests/run_windows_fingerprint.ps1`, and update the expected values in `tests/fingerprint.cpp`.
- **Who picks numbers:** agree on who makes releases. Tag the commit each release was built from (`git tag online-v3 && git push origin online-v3`), so two people never ship the same number.

To make a release:

1. Bump `BM64_ONLINE_RELEASE` and commit.
2. Build the Mac game and server, export the source bundle, build Windows from it, and run the checks on both.
3. Stage the builds:
   - `build/share/staging-mac/BM64Recompiled/`: `BM64Recompiled.app` (from `build/`), plus [server/player-readme/mac.txt](server/player-readme/mac.txt) saved as `README.txt`.
   - `build/share/staging-windows/BM64Recompiled/`: `BM64Recompiled.exe`, `SDL2.dll`, `dxcompiler.dll`, `dxil.dll`, `assets/` and `recompcontrollerdb.txt` from the Windows `build` folder, plus [server/player-readme/windows.txt](server/player-readme/windows.txt) saved as `README.txt`.
4. Package them on the Mac:
   ```bash
   python3 server/make_release.py --rom "/path/to/Bomberman 64 (USA).z64"
   ```
   It refuses a staged ROM or a build made with another release number, then writes `build/share/BM64Recompiled-vN-mac.zip`, `BM64Recompiled-vN-windows.zip` and `release-vN.txt`, and prints the server command.
5. Restart the server with the new `release-vN.txt` and send players the two zips.

## Libraries Used and Projects Referenced
* [RT64](https://github.com/rt64/rt64) for the project's rendering engine
* [RmlUi](https://github.com/mikke89/RmlUi) for building the menus and launcher
* [lunasvg](https://github.com/sammycage/lunasvg) for SVG rendering, used by RmlUi
* [FreeType](https://freetype.org/) for font rendering, used by RmlUi  
* [moodycamel::ConcurrentQueue](https://github.com/cameron314/concurrentqueue) for semaphores and fast, lock-free MPMC queues
* [Gamepad Motion Helpers](https://github.com/JibbSmart/GamepadMotionHelpers) for sensor fusion and calibration algorithms to implement gyro aiming
* [Bomberman 64 Decompilation](https://github.com/Bomberhackers/bm64) for headers and some function definitions, used for making patches or some enhancements
* [Ares emulator](https://github.com/ares-emulator/ares) for RSP vector instruction reference implementations, used in RSP recompilation

# Special Thanks:

* [ZeldaRET](https://github.com/zeldaret) : for teaching me the fundamental knowledge of N64 decompilation.
* [Decompals](https://github.com/decompals) : for helping in the initial disassembly and decompilation of the game.
* [Wiseguy](https://github.com/Mr-Wiseguy) and [DarioSamo](https://github.com/DarioSamo): for creating [N64Recomp](https://github.com/N64Recomp) and [RT64](https://github.com/rt64/rt64), assisting with fantastic technical support along the way.
