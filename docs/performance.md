# Performance

Measured on the dev Mac (RelWithDebInfo, `-O2 -g`), N001_Lot11 and the 69 lots of
`TSData/Res/UserData/Neighborhoods`. The rule for every change below: the simulation
stays bit for bit the same (the full `lot-run` output of the 69 lots at 1800 ticks and
`autonomy N001_Lot11 --ticks 43200` diffed against the baseline, `ctest` and
`ctest -j8` green) and screenshots stay byte identical (`cmp` of
`--lot N001_Lot11 --vm --ui live --run-vm 600` and `--lot N002_Lot3 --vm --run-vm 900`).

## How to measure

- `s2pkg lot-run $(find <data>/TSData/Res/UserData/Neighborhoods -name '*_Lot*.package') --ticks 1800`
- `s2pkg autonomy <N001_Lot11.package> --ticks 43200`
- `/usr/bin/time -l` for wall time, user time, cycles and instructions retired. The
  machine is shared with other builds, so wall time moves a lot; compare **cycles
  elapsed** and **instructions retired**, or run when the load is low.
- `sample <pid> 10 1 -f out.txt` (or `sample <name> -wait`) for a profile: read the
  "Sort by top of stack" section, and the call graph for the callers.
- `OS2_PERF=1` in the app prints, on stdout, `opensims2: perf: <phase> <ms>` for each
  load phase (resource index, lot, model, objects, Sims, audio, VM) and, in the live
  window, the CPU time of a frame (without the wait for the drawable) every 120 frames;
  with `--screenshot ... --frames N` it prints the per-frame VM-step and capture times.

## Baseline (commit a806ade, before this round)

| What | Result |
| --- | --- |
| `lot-run`, 69 lots x 1800 ticks | ~21 s wall on a quiet machine (~33 s under load); 73.1 G instructions, 53.2 G cycles |
| `autonomy N001_Lot11 --ticks 43200` | ~14 s wall (~17.6 s under load); 49.0 G instructions, 34.6 G cycles |
| App, `--lot N001_Lot11 --vm --ui live --run-vm 600 --screenshot` | 5.1 s wall for the whole run; the load alone ~2.3 s: resource index 0.4-0.5 s, objects 0.45 s, Sims 0.9 s, VM 0.2 s |
| App live window | 10 ms per frame including the wait for the next drawable (60 Hz), worst 247 ms (the first frame) |

The profile was dominated by walks over the `std::map<ObjectId, Object>` (each node is
1.8 KB, with the fields the walks read spread over 4 cache lines): `containedIn`,
`objectsOnTile`, `setToNext`, `followContainers`, `handleTimers`, `tickMotives`,
`tickOverlayEvents`, `useCount`, plus `VM::object(id)` (a map lookup per variable
access), `tileFree` building a whole blocked grid to test one tile, and the path
planner rebuilding its quadtree (1 MB leaf map allocated and cleared per plan).

## Changes

Each row is a commit; the numbers are `lot-run` / `autonomy` on the machine at that
moment (wall, user seconds; the load varied from 2 to 19, so they only show the trend).

| Change | lot-run | autonomy |
| --- | --- | --- |
| Baseline | 21 s | 14 s |
| `VM::object(id)` through a direct id table (`byId_`, kept by `createObject`/`removeObject`), `ObjectDefinition::field` inline | 16 s | 10 s |
| `Object::position` and `timers` moved next to `data`, so the scans over all objects touch fewer cache lines | 10 s | 10 s |
| `tileFree` tests one tile instead of building the whole blocked grid; planner keeps its summed-area tables between plans | 11.0 s user | 6.0 s user |
| Planner's pooled leaf maps handed back all -1 by clearing only the quads | 8.4 s user | 6.2 s user |
| Per-tick loops, `Set to Next`, `containedIn`, `objectsOnTile`, `tileFree` scan the id table (`VM::objectTable()`, in id order like the map) instead of walking the map | 6.7 s user | 4.1 s user |
| `useCount` and `neighborSim` through the id table | 5.4 s user | 3.7 s user |
| Package indexes read in parallel (8 threads), 128 open package files instead of 32, CRC name hashes from tables (identical on 200k random strings) | same VM numbers | same |

Final comparison, same load for both binaries, one after the other:

| | baseline (a806ade) | now |
| --- | --- | --- |
| `lot-run` instructions / cycles | 73.1 G / 53.2 G | 59.8 G / 32.8 G |
| `autonomy` instructions / cycles | 49.0 G / 34.6 G | 41.9 G / 22.2 G |
| App run (`--run-vm 600 --screenshot`), wall | 5.1 s | 3.4 s |
| Resource index build | 0.40-0.51 s | 0.22 s |
| Live window, CPU per frame without the drawable wait | not measured (10 ms with it) | 4.1-4.5 ms |

## Round 9: loading

Scenes now load on a worker thread behind the loading screens (`docs/loading-mac.md`);
the main thread keeps drawing at 60 Hz and spends 1-3 ms on the scene's main-thread
part (`OS2_PERF=1` prints `scene loaded in <worker> ms, then <main> ms on the main
thread`). Before, the window froze for the whole load (its delegate was unset).

| What (N001 lot 11, `--hood N001 --hood-lot 11 --vm --ui live`) | before | after |
| --- | --- | --- |
| `loadSimParts` | 0.46-0.47 s | 0.03-0.06 s, and once per app run (shared by lot entries, outfit changes, `--character`) |
| Lot load ("placed in" line) | 1.97 s | 1.46-1.48 s |
| Whole headless run with `--run-vm 600 --screenshot` | 3.0-3.6 s wall | 2.5-2.7 s wall |
| Main thread blocked by a lot load in the window | the whole load (~2 s) | 1-3 ms |

`loadSimParts` read all 26,167 3IDRs of the game to keep the 6,828 that belong to a
GZPS: it now reads only those, on 8 threads, through `ResourceIndex::readConcurrent`
(positioned reads on a descriptor per package, so the threads and the index's own
`read` do not share a file position). The result is the same vector, checked element
by element against the old code. Screenshots stay byte identical (`cmp` of the lot run
above with and without `--loading-shot`, and before/after the change); no VM code
changed, so `lot-run`/`autonomy` were not rerun.

Left: `loadLotObjects` (0.25-0.5 s: about 40% model parsing, 55% texture decoding in
`Renderer::addObject`), the Sims' textures (70-100 ms each), Buy and Build mode setup
(85 ms each, both reading the catalog), `LotSimulation` (0.19 s).

## Round 11: loading

| What | before | after |
| --- | --- | --- |
| `Catalog::load` (Buy) + (Build), cold | 2 x 80 ms (two passes over the OBJDs) | 92 ms for both in one pass |
| Same, next scene or mode on the same index | 2 x 80 ms | 0.6 ms (the entries are kept; each caller gets a copy with its own thumbnail cache) |
| `BuildCatalog::load` (patterns, fences, paints, roofs) | 35 ms | 35 ms cold, 0.3 ms after |
| "Buy mode" + "Build mode" laps of N001 lot 11, first load | 77-80 + 85-87 ms | 78-92 + 42-53 ms |

The caches key on the index (address and the count of the resources of the type), so a
different `ResourceIndex` reads again. The first lot of a run still pays the cold read;
every later scene (lot entries after returning to the neighborhood) skips the 160 ms.
The screenshot of the lot run (`--hood N001 --hood-lot 11 --vm --ui live --run-vm 600`)
has the same pixels as before up to the run-to-run noise it already had (1 to 8 bytes
of 3.7 MB differ between two runs of the same build, before and after).

## Round 14

Baseline = main after the walking merge (locomotion, per-step Sim test), both binaries
built from the same tree one after the other. `lot-run` over the 69 lots (1800 ticks) and
`autonomy` of N001_Lot11 and N002_Lot3 (43200 ticks) give byte-identical output (`cmp`).

| What | before | after |
| --- | --- | --- |
| `lot-run` 69 lots, cycles / instructions / wall | 75.4 G / 233 G / 30.4 s | 27.5 G / 83 G / 9.3 s |
| `autonomy N001_Lot11 --ticks 43200`, cycles / instructions / wall | 21.8 G / 49.9 G / 8.5 s | 15.8 G / 43.5 G / 5.4 s |
| `autonomy N002_Lot3 --ticks 43200`, cycles | 21.4 G | 15.9 G |
| Headless capture per frame (`--run-vm 1430 --frames 12`, includes the offscreen render) | 15.0 ms | 12.8-13.8 ms (noisy) |

What was done, each a commit:

- **Wants library kept between VMs.** `libraryFor` held its cache with `weak_ptr`, so in
  `lot-run` each lot's VM dropped the library and the next lot reread and parsed its 493
  definitions and 211 trees: 55% of the whole run. It is held strongly now, keyed by where the
  definitions/trees/tuning live (package path, offset, size), not by the index's address. A
  faster `decode` (no entities, no copying char by char) cut the cold parse ~5%.
- **Quadtree cache of the path planner.** The planner records the obstacle/cost calls since
  `clearObstacles()`; the quadtrees (squares and leaf maps) are kept in a 4-entry LRU keyed by
  that exact call list (compared, not hashed) and the levels searched. In a 12-hour N001_Lot11
  run 449 of 684 plans hit. The A* and its noise generator are untouched.
- **Object loops through the ID table.** `VM::eachObject()` walks `byId_` in ID order with
  the map's semantics (an object made during the loop is met if its ID is later, a removed one
  is skipped) and replaces the walks over the map's 1.8 KB nodes in 45 loops (`flushDestinations`
  at every tick, `stepBlocker` at every walking step, `queue`, `timer`, `routing`, `reach`,
  `placement`, ...). This was the biggest part of the autonomy gain (21.8 G to 15.8 G).
- **Frustum culling in the renderer.** Static parts (not the skinned Sims) carry their
  model-space box; a part whose 8 corners are all outside one clip half-space of the pass is not
  drawn. The clip volume is convex and the map linear, so such a part draws no pixel. It applies
  to the mirror's reflected pass with its own matrix, which therefore draws less than before.
  The reflection pass was already skipped when no mirror is in view. Resolution was left alone.

Screenshots (`--lot N001_Lot11 --vm --ui live --run-vm 600`, `--lot N002_Lot3 --vm --run-vm 900`,
and 12 frames up to tick 1430): decoded pixels equal to the base build up to the run-to-run noise
the app already has (base vs base differs by 37-217 bytes of 3.7 MB; new vs base by 1-217); 22 of
24 frames of the 1-minute run are identical, the other two differ by 8 and 12 bytes.

`loadSimParts` was already done in round 9 (64 ms cold, 0 afterwards; see above). A lot load is
2.2-2.5 s: `loadLotObjects` 0.68 s (the four Sims' GPU upload 75-110 ms each), `LotSimulation`
0.62 s, Buy/Build mode setup 165 + 125 ms, resource index 80 ms (file reads).

Not done: an index for `containedIn`/`followContainers`. Fields are written directly
(`o.data[objdata::ContainerId] = ...`, `Context::set` by field number, saves) at dozens of
places, so a derived index cannot be kept exact without a setter everywhere; they now cost about
5% of an autonomy run each. The quadtree still misses when a walking Sim's cost rectangle moves
(the summed-area tables are rebuilt, ~6% of the run).

## What is left (not done)

- The scans of all objects per tick (`followContainers`, `tickOverlayEvents`,
  `handleTimers`) and `containedIn` remain O(objects) per call; an index would need every
  write to `data[ContainerId]`/`position` to go through a setter.
- The quadtree of the planner is still rebuilt on every plan (summed-area tables of
  512x512 cells for a 64-tile lot); caching it needs a cheap way to know the obstacles
  did not change.
- Load: the Sims' textures take 45 ms each to upload; `Catalog::load` reads 47 resources again after the lot's (round 11: kept between scenes, see above).
  8% of the resource bytes read at load are the same resource read twice (4.9 of 64 MB).
