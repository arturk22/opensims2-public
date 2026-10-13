# opensims2: development

A native reimplementation (C++ + Metal) of The Sims 2 (2004) engine for macOS, using the original CD assets. If you only want to try the game, see the [README](README.md).

## Getting the code
The game data in `data/` is about 2.8 GB of Git LFS. To skip it, clone without LFS and use the data zips from the [latest release](https://github.com/opensims2/opensims2/releases/latest) instead (unzip them and point the app at `opensims2-data` with `--data` or the folder picker):
```
GIT_LFS_SKIP_SMUDGE=1 git clone https://github.com/opensims2/opensims2.git
```
Requirements: Command Line Tools (`xcode-select --install`), `cmake` and `ninja` (`brew install cmake ninja`).

## Layout
- `tools/` – extraction scripts (CCD/IMG → ISO, etc.)
- `data/` – content extracted from the CDs (LFS)
- `src/formats/` – parsers (DBPF, QFS, TXTR, GMDC, ...)
- `src/vm/` – SimAntics VM
- `src/render/` – Metal renderer
- `src/app/` – macOS app
- `cli/s2pkg/` – `.package` inspector
- `third_party/` – metal-cpp (Apple headers) and libspeex 1.2.1 (BSD, decoder for the SPX voices)

## References
Third-party projects live in `../references/` (next to this repo, outside git) and **can be used freely**: read them, compare against them, borrow ideas and port code. When porting MPL-2.0 code, keep the license header in the copied files.

| Folder | What it is | Useful for |
|---|---|---|
| `OpenTS2` | PC Sims 2 in Unity/C# (MPL-2.0) | materials (Floor, Wallpaper, Phong, SimSkin…), lot/terrain, SimAntics |
| `FreeSO` | The Sims Online/The Sims 1 in C# (MPL-2.0) | full SimAntics VM (same family) |
| `sims2_mac_decomp` | PowerPC Mac Sims 2 decomp with `baserom.sym` | function names for Ghidra |

To recreate it: `git clone --depth 1` of `LazyDuchess/OpenTS2`, `RHY3756547/FreeSO` and `ChrisNonyminus/sims2_mac_decomp` (with `GIT_LFS_SKIP_SMUDGE=1`).

## s2pkg
```
cmake -S . -B build -G Ninja && cmake --build build && (cd build && ctest)

s2pkg ls|stats|verify|decode-all <file.package>
s2pkg dump <file.package> <TTTTTTTT-GGGGGGGG-HHHHHHHH-IIIIIIII>   # decoded dump (RCOL, STR#, OBJD, BHAV, ...)
s2pkg texture <file.package> <tgi> out.png                        # TXTR (+LIFO) -> PNG
s2pkg mesh <file.package> <tgi> out.obj                           # GMDC -> OBJ
s2pkg repack <in.package> <out.package> [--recompress|--decompress]  # rewrites as DBPF 1.1/index 7.1 (keeps the bytes, recompresses or decompresses)
s2pkg compare <a.package> <b.package>                             # same TGIs with the same decompressed bytes
s2pkg object <Res-dir> <cres|0xGUID|name> <out-dir>               # full object -> OBJ + MTL + PNG
s2pkg find <Res-dir> <name> <type-hex>                            # resolves a scenegraph name
s2pkg lot <Res-dir> <lot.package>...                              # lot terrain/floors/walls/objects: summary and what didn't resolve
s2pkg bhav <Res-dir> <group-hex> <instance-hex> [arg...]          # runs a BHAV on the SimAntics VM, with a trace
s2pkg bhav-sweep <Res-dir>                                        # runs every BHAV: results and missing primitives/owners
# bhav, bhav-sweep and lot-run load a neighborhood (N001, or the lot's own); OS2_HOOD=<neighborhood folder>|none changes that
# language (Res/Locale/<language>: UI, fonts, sounds, text): OS2_LOCALE=<folder>, default "Brazilian Portuguese" (applies to the app too)
s2pkg lot-run <lot.package>... [--ticks N] [--list] [--trace ID]  # lot objects on the VM without rendering, with the lot's neighborhood: Init, N ticks of Main, failure summary
s2pkg prim-usage <Res-dir> <opcode-hex> [n]                       # how trees use a primitive: histogram of each operand byte and n examples
s2pkg pie <lot.package> [sim [object]] [--all] [--select N [--ticks N]] [--motives N]  # an object's pie menu (TTAB + guards on the VM) for a Sim; --select queues it and runs N ticks
s2pkg material-survey <Res-dir> [type]                            # TXMT material types and the properties of each
s2pkg object-materials <Res-dir> <cres|0xGUID|name>               # an object's materials as the renderer builds them
s2pkg texture-faces <Res-dir> <txtr_name> <prefix>                # every texture in a TXTR (the 6 faces of an env cube)
s2pkg sound-list <Res-dir> [filter]                               # sound samples (XA/SPX/MP3): group, format, length
s2pkg sound-info <Res-dir> <name>                                 # track settings, hit list and samples of a sound
s2pkg sound-extract <Res-dir> <name|TGI> out.wav [n]              # decodes a sample (or the n-th of a track) to WAV
s2pkg sound-sweep <Res-dir>                                       # decodes every sound: coverage per format (see docs/audio.md)
tools/decode_all.sh                                               # decode-all on every package
```
Example: `s2pkg object data/game/TSData/Res 0xEC7618F6 out/` exports the "Chair - Dining - Value1" chair.

## App
```
cmake --build build && open build/opensims2.app
```
An AppKit window + `MTKView` with the Metal renderer (`src/render/`) and the lot camera configured by the game's `LiveCamera.txt` (the `data/game` folder is found by walking up from the executable; `--data <dir>` or `OS2_DATA` change that).

### Packaging and opening without the command line
```
cmake --build build --target package-app   # build/package/opensims2.app + opensims2.zip
```
The `.app` doesn't include the game data and is ad hoc signed (`codesign -s -`; runs on Apple Silicon for the local user, without notarization). When opened without flags, the app looks for the game folder (the one with `TSData/`, or one that contains `game/` or `data/game/`) in this order: `--data`, `OS2_DATA`, `data/game` above the executable, the folder remembered in the user preferences (key `GameDataPath` in the `org.opensims2.opensims2` domain), the folder containing the `.app` and `~/Library/Application Support/opensims2`. If none is found, it opens a folder picker and remembers the choice (to change it: `defaults delete org.opensims2.opensims2 GameDataPath`). All flags still apply.

Without scene flags the app opens the **main menu** (the neighborhood chooser) → neighborhood → lot, switching scenes in the same process; neighborhoods are copied (APFS clone, no disk cost) to the player folder `~/Library/Application Support/opensims2/Neighborhoods` the first time they're opened, and the game loads and saves there (`--user-dir <folder>` or `OS2_USER_DIR` change the folder; `data/game` is never written). On a lot, puck Options → Save, or Neighborhood/Esc → "Save?"; volume, mute and edge scrolling live in `Options.txt`. Headless, `--click`/`--wait` go through the scenes and `--flow-shots` saves each one (see `docs/game-flow-mac.md`).

```
build/opensims2.app/Contents/MacOS/opensims2 --object sofa --object 0xEC7618F6 --zoom 2 --orientation 1 --screenshot out.png
```
`--object <OBJD name | 0xGUID | CRES>` (repeatable) picks the objects (default: the chair), `--zoom`/`--orientation` move the camera N steps from the default, `--lot <N001_Lot82 | file.package>` shows a lot's terrain, floors, walls, roofs, fences, water and objects instead of the test floor (`--walls up | cutaway | down` picks the wall mode), `--pivot x,y` centers the camera on that point (in tiles), `--lighting <state>` picks a lighting state from the game's `TSData/Res/Lights/Lighting.txt` (`day` by default; also `night`, `morning`, `evening`, `neighbourhood`, `CAS`, ...) and `--screenshot` renders one frame offscreen, saves the PNG and exits (no screen recording permission needed).

`--vm` runs the lot's objects on the SimAntics VM while it's shown, with the Sims following the VM (walking with the walk animation, playing Animate Sim); `--vm-walk <sim>,<object>[,<anim>]` makes a Sim walk to an object (and then play entry `<anim>` of the global animation list), and `--run-vm <ticks>` with `--screenshot out.png [--frames n]` runs those ticks (30 per second) without a window and saves n frames (`out-1.png` ... `out-n.png`). E.g.: `opensims2 --lot N001_Lot11 --run-vm 240 --vm-walk 197,127,124 --frames 4 --pivot 5,22 --zoom 3 --screenshot out.png`.

With the VM (`--vm`), clicking an object opens its pie menu for the selected Sim (`--sim-id <id|name>`); picking an entry adds it to the Sim's queue, shown in the top corner. Headless (these turn the VM on by themselves): `--pie <object> [--pie-select N]` opens an object's menu (OBJD name or ID) and picks entry N, `--click x,y` (repeatable) clicks the image like the mouse; with `--run-vm`/`--frames` the VM runs afterwards (the menu closes after the pick; `--motives N` sets every Sim's motives to N instead of the saved ones; objects resume the lot's saved state, `--init` makes them start from their Inits):
```
build/opensims2.app/Contents/MacOS/opensims2 --lot N001_Lot11 --walls cutaway --zoom 2 --pie Fridge --pie-select 0 --screenshot out.png
build/opensims2.app/Contents/MacOS/opensims2 --lot N001_Lot11 --orientation 2 --sim-id Mortimer --pie Newspaper --pie-select 0 --pivot 18,21 --zoom 1 --run-vm 690 --frames 3 --screenshot out.png
```

`--play-sound <name | TGI>` plays a game sound (track or sample; `--sound-index n` picks a track's sample, `--sound-wav out.wav` saves the PCM) and exits without opening a window; see `docs/audio.md`.

Camera: WASD/arrows move, Q/E or `,`/`.` rotate, Z/X, `+`/`-` or the wheel zoom, Page Up/Down change floor (buy mode: the objects' floor; the puck's "Up/Down One Level" buttons too), right/middle drag rotates and tilts freely, Home resets to the default.

The shaders (`src/render/shaders.metal`) are embedded in the binary and compiled at runtime, so the Command Line Tools are enough (no Xcode). `MTL_DEBUG_LAYER=1` turns on API validation.
