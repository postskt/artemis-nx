# Artemis NX

A portable C++17 runtime for **Artemis Engine**-format visual novels, with a
pure-core engine and SDL2 desktop/Switch presentation layers.

This repository contains **engine and tooling source only**. It does not
contain any commercial game assets, scripts, audio, fonts, encryption keys or
NSP/NRO files. To use it, provide your own legally obtained game data.

## Architecture

```text
src/core/
  Config.cpp/.hpp        runner_config.json loader
  AstModel.hpp           AST model, sprites, audio commands, GameSnapshot
  LuaLoader.cpp/.hpp     sandboxed Lua 5.4 AST loader
  AssetManager.cpp/.hpp  case-insensitive asset index + direct-path fallback
  Engine.cpp/.hpp        excall/eval/select, f/g variables, fg layers, audio queue

src/platform/
  Renderer.cpp/.hpp      SDL2 logical-size rendering, 64-texture LRU,
                         background/CG/fg/textbox/select/OSD, title/save UI
  AudioPlayer.cpp/.hpp   8-channel mixer, voice lock, BGM intro -> loop
  InputHandler.cpp/.hpp  keyboard/mouse/touch/controller hot-plug
  GameSelector.cpp/.hpp  multi-game launcher
  SaveManager.cpp/.hpp   JSON save slots + PNG thumbnails
  Settings.cpp/.hpp      persisted user settings
  Gallery.cpp/.hpp       CG thumbnail list + unlock state
  main.cpp               PC + __SWITCH__ entry point, title/game/menu screens
```

The core has no SDL dependency. It emits a `GameSnapshot` and a one-shot
`AudioCommand` queue; platform code renders/plays the snapshot.

## Features

- Artemis `.ast` Lua-table parsing in a constrained Lua sandbox.
- `excall`, labels, `eval`, `cond`, `select`, `msgon`/`msgoff`.
- `bg`, `cg`, event-CG (`:ev/`), `fg`, `face`, `ex01`, `colortone`, `flash`.
- Multiple foreground characters sorted by `lv`/`z`, with `x/mx/my/x2/y`,
  character size and face/ex offsets.
- 64-entry texture LRU cache with immediate `SDL_DestroyTexture` eviction.
- SDL2 logical-size letterboxing for desktop and Switch.
- 8 mixer channels: voice on channel 0, SE on 1–7, BGM intro/loop handoff via
  `Mix_HookMusicFinished`.
- Controller/touch/keyboard input and a multi-game launcher.
- Native title screen and save/load slots (12 slots, thumbnail previews).
- Title/save/config/gallery layouts follow the original `list_windows.tbl`
  coordinates and sprite-sheet frames 1:1.
- Settings UI (text/auto speed, BGM/SE/voice volume, fullscreen) persisted as JSON.
- CG gallery with 4x5 thumbnail grid and persistent unlock state.
- Text history rollback with Backspace.
- `F5` opens the save menu, `F9` opens the load menu, `Esc` opens the in-game menu.
- `Q` / `E` switch config pages; `--ui-screen` + `--ui-screenshot` render a
  single menu frame for layout verification.
- Switch build with devkitPro, NRO and NSP-staging friendly CMake targets.

## Build on Windows (MSYS2 UCRT64)

```powershell
cmake -S . -B build -G Ninja `
  -DCMAKE_BUILD_TYPE=Debug `
  -DCMAKE_C_COMPILER=gcc `
  -DCMAKE_CXX_COMPILER=g++ `
  -DARTC_BUILD_PLATFORM=ON
cmake --build build -j 8
```

The desktop executable is:

```text
build/src/platform/artemis_runner.exe
```

Run it against your own prepared data:

```powershell
.\build\src\platform\artemis_runner.exe `
  --root path\to\game_data `
  --config path\to\game_data\runner_config.json `
  --save-dir path\to\saves
```

The title screen is shown by default. Use `--no-title` for a direct game
launch. In game, `F5` opens the save menu, `F9` opens the load menu, `Esc`
opens the pause menu, and `Backspace` rewinds one text step. Saves are JSON +
PNG thumbnails under `--save-dir`.

Or use the launcher:

```powershell
.\build\src\platform\artemis_runner.exe --games-dir path\to\games
```

## Build for Nintendo Switch

Use devkitPro's Switch CMake toolchain from an MSYS2 shell:

```bash
source /etc/profile.d/devkit-env.sh
cmake -S . -B build-switch -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=/opt/devkitpro/cmake/Switch.cmake \
  -DCMAKE_BUILD_TYPE=Release \
  -DARTC_BUILD_SMOKE=OFF \
  -DARTC_BUILD_PLATFORM=ON
cmake --build build-switch -j 8
```

The build produces:

```text
build-switch/src/platform/artemis_runner.elf
build-switch/artemis_runner.nro
```

`main.cpp` contains a `__SWITCH__` path that uses `romfsInit()` and the
`romfs:/` root. The standalone NRO target (`artemis_runner_nro`) always opens
the launcher and avoids probing a missing RomFS.

## Tooling

```text
tools/export_config.py   Extract runner_config.json from an Artemis PC install
tools/core_smoke.cpp     Headless core parser/smoke test
tools/build_switch.sh    Switch NRO build helper
```

The core smoke test can parse a full script set without SDL:

```powershell
.\build\artc_core_smoke.exe `
  --root path\to\game_data `
  --config path\to\game_data\runner_config.json `
  --parse-all
```

## Third-party

- Lua 5.4.8 (MIT)
- nlohmann/json 3.11.3 (MIT)

See [third_party/NOTICE.md](third_party/NOTICE.md).

## Legal

This project is an independent compatibility runtime and does not include
Artemis Engine, Nintendo SDK code, game assets, encryption keys or installable
game packages. Use it only with data you are legally entitled to use.

## License

MIT. See [LICENSE](LICENSE).
