# opensims2

A native reimplementation (C++ + Metal) of The Sims 2 (2004) engine for macOS. It's a learning project, made just for fun, and it's still a work in progress, so expect bugs and missing features.

This repository has only the engine code. No game files are included: you need your own copy of The Sims 2.

## Requirements
- A Mac with Apple Silicon (M1 or newer) running macOS 13 or newer
- Your own copy of The Sims 2 (see [Game files](#game-files))
- To build from source: Command Line Tools and CMake (`xcode-select --install`, `brew install cmake`)

## Game files
opensims2 reads the files from your own copy of The Sims 2. Only the base game content is used for now (expansion packs and stuff packs are ignored). Where to get the files:

- **PC version installed on Windows:** copy the whole install folder (usually `C:\Program Files\EA GAMES\The Sims 2`, or `C:\Program Files (x86)\EA GAMES\The Sims 2`) to your Mac, with a USB drive for example.
- **PC discs, without Windows:** copy each disc's contents into folders named `disc1`, `disc2`, `disc3` and `disc4` inside one folder, then extract them with [`tools/extract_game.py`](tools/extract_game.py) (Python comes with macOS's Command Line Tools):
  ```
  python3 extract_game.py <folder with disc1..disc4> ~/TheSims2 --lang en
  ```
  `--lang` is the start of the `<lang>_compressed.zip` file name on disc 4 (for example, `pt-br_compressed.zip` means `--lang pt-br`). The game ends up in `~/TheSims2`.
- **Mac version:** in the folder picker, choose the game's app; opensims2 looks for the game files inside it. This hasn't been tested yet, so please open an issue telling how it went.
- **Other editions:** not tested yet. If yours has a folder named `TSData`, try it and tell us in an issue.

The first time opensims2 runs, it asks for the game folder. Choose the folder that contains `TSData` (or any folder above it, since it looks a few levels down). It remembers the choice; to pick another folder later, run `defaults delete org.opensims2.opensims2 GameDataPath` in Terminal.

## Download
Get `opensims2.zip` from the [latest release](https://github.com/opensims2/opensims2-public/releases/latest), unzip it and open `opensims2.app`. macOS will block it the first time because it isn't notarized: go to **System Settings → Privacy & Security** and click **Open Anyway**. You don't need Command Line Tools or CMake for this, only to build from source.

## Build and run
```
git clone https://github.com/opensims2/opensims2-public.git opensims2
cd opensims2
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target opensims2 -j
open build/opensims2.app
```

## First run
On the first run, the app asks for the game folder (see [Game files](#game-files)). It then opens on the neighborhood menu, and from there you can enter a neighborhood and then a lot.

The game starts in Brazilian Portuguese. To play in English:
```
open --env OS2_LOCALE=English opensims2.app
```
(from the folder with the app; if you built it, that's `build/`).
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
The code is licensed under the [Mozilla Public License 2.0](LICENSE). Some files are based on [OpenTS2](https://github.com/LazyDuchess/OpenTS2) and [FreeSO](https://github.com/RHY3756547/FreeSO), also MPL-2.0. `third_party/` keeps its own licenses (metal-cpp: Apache 2.0, Speex: BSD).

This project isn't affiliated with or endorsed by Electronic Arts or Maxis. The Sims is a trademark of Electronic Arts Inc.
