# opensims2

A native reimplementation (C++ + Metal) of The Sims 2 (2004) engine for macOS. It's a learning project, made just for fun, and it's still a work in progress, so expect bugs and missing features.

This repository has only the engine code. No game files are included: you need your own copy of The Sims 2.

## Requirements
- A Mac with Apple Silicon (M1 or newer) running macOS 13 or newer
- The Sims 2 base game (2004 PC version) installed or copied somewhere on the Mac, that is, a folder containing `TSData/`. Other versions haven't been tested.
- Command Line Tools and CMake:
```
xcode-select --install
brew install cmake
```

## Build and run
```
git clone https://github.com/opensims2/opensims2-public.git opensims2
cd opensims2
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target opensims2 -j
open build/opensims2.app
```
On the first run, the app asks for the game folder: choose the folder that contains `TSData/`. It remembers the choice. It then opens on the neighborhood menu, and from there you can enter a neighborhood and then a lot.

The game starts in Brazilian Portuguese. To play in English:
```
open --env OS2_LOCALE=English build/opensims2.app
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

This project isn't affiliated with or endorsed by Electronic Arts or Maxis. The Sims is a trademark of Electronic Arts Inc.
