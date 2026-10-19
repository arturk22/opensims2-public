# opensims2

A native reimplementation of The Sims 2 (2004) engine in C++, for **64-bit macOS and 64-bit Windows**: Metal on the Mac, Direct3D 12 (through SDL3) on Windows. It's a learning project, made just for fun, and it's still a work in progress, so expect bugs and missing features.

This repository has only the engine code. No game files are included: you need your own copy of The Sims 2.

## Requirements
- **Mac:** Apple Silicon (M1 or newer), macOS 13 or newer (64-bit)
- **Windows:** 64-bit Windows 10 or 11 (x64) with a graphics card that supports Direct3D 12. The Windows version is new and less tested than the Mac one: please report what you find in an issue.
- Your own copy of The Sims 2 (see [Game files](#game-files))
- To build from source: on the Mac, Command Line Tools and CMake (`xcode-select --install`, `brew install cmake`); on Windows, see [Build and run](#build-and-run)

## Game files
opensims2 reads the files from your own copy of The Sims 2. Only the base game content is used for now (expansion packs and stuff packs are ignored). Where to get the files:

- **PC version installed on Windows:** on Windows there is nothing to copy: opensims2 finds the game in its install folder (usually `C:\Program Files\EA GAMES\The Sims 2`, or `C:\Program Files (x86)\EA GAMES\The Sims 2`). For the Mac, copy that whole folder to your Mac, with a USB drive for example.
- **PC discs, without Windows:** copy each disc's contents into folders named `disc1`, `disc2`, `disc3` and `disc4` inside one folder, then extract them with [`tools/extract_game.py`](tools/extract_game.py) (Python comes with macOS's Command Line Tools):
  ```
  python3 extract_game.py <folder with disc1..disc4> ~/TheSims2 --lang en
  ```
  `--lang` is the start of the `<lang>_compressed.zip` file name on disc 4 (for example, `pt-br_compressed.zip` means `--lang pt-br`). The game ends up in `~/TheSims2`.
- **Mac version:** in the folder picker, choose the game's app; opensims2 looks for the game files inside it. This hasn't been tested yet, so please open an issue telling how it went.
- **Other editions:** not tested yet. If yours has a folder named `TSData`, try it and tell us in an issue.

On the Mac, the first time opensims2 runs, it asks for the game folder. Choose the folder that contains `TSData` (or any folder above it, since it looks a few levels down). It remembers the choice; to pick another folder later, run `defaults delete org.opensims2.opensims2 GameDataPath` in Terminal.

On Windows, opensims2 looks for the game in its own folder and the folders above it (so you can put the `opensims2-windows-x64` folder inside your The Sims 2 folder), then in the usual install folders. If your game is somewhere else, start it with the path to the game's `TSData\Res`:
```
opensims2_win.exe "D:\Games\The Sims 2\TSData\Res"
```

## Download
From the [latest release](https://github.com/opensims2/opensims2-public/releases/latest):

- **Mac:** get `opensims2.zip`, unzip it and open `opensims2.app`. macOS will block it the first time because it isn't notarized: go to **System Settings → Privacy & Security** and click **Open Anyway**.
- **Windows:** get `opensims2-windows-x64.zip`, unzip it and double-click `opensims2_win.exe` in the `opensims2-windows-x64` folder. Windows may warn that the app comes from an unknown publisher, because it isn't signed: click **More info → Run anyway**. The Windows build is attached to the release a few minutes after it is published.

You don't need any build tools for this, only to build from source.

## Build and run
On the Mac:
```
git clone https://github.com/opensims2/opensims2-public.git opensims2
cd opensims2
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target opensims2 -j
open build/opensims2.app
```
On Windows (x64), with Visual Studio 2022 Build Tools (C++ workload) and [vcpkg](https://github.com/microsoft/vcpkg), in an **x64 Native Tools Command Prompt for VS 2022**:
```
vcpkg install sdl3 zlib --triplet x64-windows
git clone https://github.com/opensims2/opensims2-public.git opensims2
cd opensims2
cmake -S . -B build-win -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_TOOLCHAIN_FILE=<vcpkg>\scripts\buildsystems\vcpkg.cmake
cmake --build build-win
build-win\opensims2_win.exe
```
The shaders are compiled with the Windows SDK's `dxc`.

## First run
On the first run, the Mac app asks for the game folder (see [Game files](#game-files)); the Windows app looks for it by itself. It then opens on the neighborhood menu, and from there you can enter a neighborhood and then a lot.

The game starts in Brazilian Portuguese. To play in English, on the Mac:
```
open --env OS2_LOCALE=English opensims2.app
```
(from the folder with the app; if you built it, that's `build/`). On Windows, in a Command Prompt in the app's folder:
```
set OS2_LOCALE=English
opensims2_win.exe
```
Other languages: the folder names in `TSData/Res/Locale` (`French`, `German`, `Spanish`, `UK English`, ...).

## Controls
- **WASD / arrow keys:** move the camera
- **Q / E:** rotate
- **Z / X or mouse wheel:** zoom
- **Page Up / Page Down:** change floor
- **Right mouse drag:** free rotate and tilt
- **Home:** reset the camera

## Contributing
Bug reports and pull requests are welcome. Project layout, command-line tools and reverse-engineering notes are in [DEVELOPMENT.md](DEVELOPMENT.md).

## License
The code is licensed under the [Mozilla Public License 2.0](LICENSE). Some files are based on [OpenTS2](https://github.com/LazyDuchess/OpenTS2) and [FreeSO](https://github.com/RHY3756547/FreeSO), also MPL-2.0. `third_party/` keeps its own licenses (metal-cpp: Apache 2.0, Speex: BSD). The Windows build also ships SDL3 and zlib (both under the zlib license).

This project isn't affiliated with or endorsed by Electronic Arts or Maxis. The Sims is a trademark of Electronic Arts Inc.
