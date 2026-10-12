# Soak report (round 15, survival 5)

Runs of `tools/soak.sh` over the 69 lots (3 game days, 129600 ticks), headless (`SOAK_NO_APP=1`), 2 lots at a time, seeded (`OS2_SEED=1` and `2`), babies named (`--name-babies`). **Before** is main c3911d1 (the round's branch point); **after** is the r15-survival5 branch rebased on main 1782c4d, run with `S2PKG_FIRES=1` and `SOAK_KEEP_LOGS=1` (new: keeps the autonomy logs).

| Category | before, seed 1 | after, seed 1 | before, seed 2 | after, seed 2 |
|---|---|---|---|---|
| Crashes / hangs | 0 / 0 | 0 / 0 | 0 / 0 | 0 / 0 |
| Load errors | 1 (N002_Lot32) | 1 | 1 | 1 |
| Household Mains reset | 2 | 2 | 0 | 1 |
| Sims that died ("Falecer") | 9 (5 lots) | 12 (6 lots) | 22 (8 lots) | 20 (8 lots) |
| Sims stuck > 6 game hours | 35 | 35 | 23 | 24 |
| of them not sleeping, working or at school | 5 | 3 | 4 | 4 |
| Motives pinned at -100 > 6 game hours | 21 | 15 | 29 | 22 |
| Save round trips that differ | 0 | 0 | 0 | 0 |
| Instruction budget (20000) hit | 5 (2 lots) | 0 | 0 | 1 (N003_Lot192, "Controller - Relationship Decay") |

What changed (TODO.md, "Rodada 15, sobrevivência 5"; `docs/primitives-mac.md` Set to Next and "Rodada 15 (sobrevivência 5)", `docs/routing-mac.md` §2):
- **Three-Sim socials**: Push Interaction's link (flag 0x40) is a chain as `TryPushAction` makes it (the new one takes the linked one's link, the linked one points to the new one). "Social - Baby - Talk To" is A → C → B, so A ending tells the baby's holder: Brandi no longer holds the baby for 11 h.
- **Set to Next 9** is one object a side (the tile's last), +X, +Y, −X, −Y, going on from the target's side; **Set to Next 8** walks into the slots (a Sim on a chair, a plate on a counter), as the binary's tile iterator. "Set to Next - Good Dining Chair" no longer spins (N002_Lot22: 4 resets of the cooks a seeded game → 0; the budget hits of seed 1, 5 → 0), and the social bunny's "Don't Stand on Sims" sees the Sim on the chair and moves away (Olive/Ophelia's "Chair - Route Fail - Sim on Tile" for hours: gone).
- **Path planner admission** as `AddThisToPathPlanner`: only the ceiling filter (FootprintBlocksCeiling) is the binary's; the others (small things on surfaces, detached, wall-hung, "can be stepped over", landing pieces) are gone, each object blocks by its footprint; models without a footprint list by their meshes' box cut to the tile; the part's tile by GetLocalOffsetFromLeader (OBJD sub index; same tile as before on every bed and piano of the lots); EvalTileForGoal by footprints, not tiles. Half-soak A/B of this step alone: failed interactions 192/159 → 141/244 (seed 2: N001_Lot68's diving board is now reached and the divers cannot leave the pool; trash piles), resets 20/46 → 7/6.
- **Reset's free place** as FUN_00af87c8 (square spiral of tile centres, room first). A walker standing in a diagonal wall's cells escapes through them (N002_Lot4: Vidcund failed every route from a diagonal doorway and starved with Pascal in a seed-1 run).
- **Long loops are the trees'**: piano, easel, darts and the newspaper exit only on failing motives (hunger ≤ −50, energy ≤ −80, ...; the newspaper at fun ≥ 60); Alexander's 7.7 h "Tocar" ends at hunger −51. Not a bug.
- **Fires** checked against the engine-re reading: first spread try ≥ 900 ticks (seen ≥ 1117), tries ≥ 200 apart (≥ 219), ≤ 8 per Fire (≤ 5 seen), never a second Fire on a tile; lifetimes 37–4281 ticks with the trees' flame 73–176 and the 20 × Idle 30 wait. The deaths are still mostly fires (seed 1: 9 of 12; seed 2: all but Olive's old age), so the counts follow where the seed starts a fire.
- Left (TODO): the pool exit (N001_Lot68's Mary-Sue starves in the pool, seed 1), "Jogar Fora" on kicked-over trash piles failing over and over, stairs and route penalties still outside the planner.
- The remaining non-sleep/work stuck: piano/painting/darts loops (the trees' rules). Beau's social worker car (seed 2) and Lilith's "Desaparecido" (seed 1) did not recur.

# Soak report (round 14, survival 4)

Runs of `tools/soak.sh` over the 69 lots (3 game days, 129600 ticks), headless (`SOAK_NO_APP=1`), 2 lots at a time, seeded (`OS2_SEED=1` and `2`). **Before** is main b14e777 with its own `tools/soak.sh`; **after** is the r14-survival4 branch rebased on it, with its `tools/soak.sh` (which names the babies: `autonomy --name-babies`).

| Category | before, seed 1 | after, seed 1 | before, seed 2 | after, seed 2 |
|---|---|---|---|---|
| Crashes / hangs | 0 / 0 | 0 / 0 | 0 / 0 | 0 / 0 |
| Load errors | 1 (N002_Lot32) | 1 | 1 | 1 |
| Household Mains reset | 1 | 2 | 2 | 0 |
| Sims that died ("Falecer") | 10 (7 lots) | 9 (5 lots) | 17 (9 lots) | 22 (8 lots) |
| Sims stuck > 6 game hours | 22 | 33 | 17 | 23 |
| of them not sleeping, working or at school | 4 | 5 | 3 | 4 |
| Motives pinned at -100 > 6 game hours | 31 | 21 | 26 | 29 |
| Save round trips that differ | 1 (N001_Lot80) | 0 | 1 (N001_Lot80) | 0 |
| Instruction budget (20000) hit | 31 | 5 | 5 | 0 |

What changed (TODO.md, "Rodada 14, sobrevivência 4"; `docs/routing-mac.md` §2, `docs/primitives-mac.md` §4f/§4g):
- **Object footprints** as the binary's `AddThisToPathPlanner`: the model's CRES "footprint" mask (16x16 a tile, halved to 1/8-tile cells) or the OBJD box, not the whole tile; the step test and Snap use them too. "Tocar" on N001_Lot11's piano: 43 failures a day to 0 (Cassandra now plays for hours). With the planner alone the step test (whole tiles) refused what the planner allowed: N002_Lot2 went from 22 failed interactions to 167; with the footprint step test and Snap, and a Snap in a portal resetting the locomotor (a modular stair's tree snapped Olive down and the walk carried her back to the top's point on the lower level, walled in), 9.
- **Births**: "Dar à Luz" waited for the name dialog (type 23, no time limit in the game either); the soak now names the babies. Pascal (N002_Lot4) and Brandi (N001_Lot36) get their babies.
- **N001_Lot80's round trip**: an evicted family's lot; the check's reload cleaned up after the eviction again (the save clears the flag).
- **Reset** as `cTSPerson::OnCleanup`: the person goes to a free place near; object data 7 = OBJD field 45.
- **Deaths** (`S2PKG_FIRES=1`): in the after runs every death but four is a fire (the Sim burning on a Fire's tile after "Fogo!"); two are old age (Olive, Patrizio, good motives), two starvation (Angela, Tybalt). One fire on a lot without a smoke alarm kills the whole family (N003_Lot46, N003_Lot52, N003_Lot192, N002_Lot4 and N002_Lot22 lose 3-4 each), so the counts follow where the seed starts a fire. The panic does take Sims 2 tiles away (global slot 9); they die because the fire spreads (10-32 Fires for hours) and the next panic, about another Fire, can send them onto a third Fire's tile (Fires allow person intersection, as in the game). Fires start in ~6.5% of cooks (unskilled cooks and forgotten food), per cook, not per tick.
- The stuck rise is sleep and work (Sims reach beds and leave for work more); the others: Cassandra's piano and painting (6-8 h), Brandi holding the baby in a three-person social whose starter left ("Person C - Baby Holder", 11 h), Beau's social worker car (seed 2), Lilith "Desaparecido" (in both).
- The budget hits left: N002_Lot3's "Set to Next - Good Dining Chair" check tree (`OS2_CHECK_TREE_ERRORS=1`).

# Soak report (round 13, survival 3)

Runs of `tools/soak.sh` over the 69 lots (3 game days, 129600 ticks), headless (`SOAK_NO_APP=1`), 2 lots at a time, all with this branch's `tools/soak.sh` (which ends a Sim's interaction when it is removed from the lot). Main moved twice during the round, so there are two pairs: **A** is main 98c5c85 against the r13-survival3 work rebased on it (without the Set to Next fix); **B** is main 494c8fe (with r13-anim-choreo and the walking merge) against the final branch rebased on it. The branch point (main 461ec3d) is given too.

| Category | 461ec3d | A: main 98c5c85 | A: + survival3 | B: main 494c8fe | B: + survival3 (final) |
|---|---|---|---|---|---|
| Crashes / hangs | 0 / 0 | 0 / 0 | 0 / 0 | 0 / 0 | 0 / 0 |
| Load errors | 1 (N002_Lot32) | 1 | 1 | 1 | 1 |
| Household Mains reset | 1 | 0 | 0 | 3 (budget, N001_Lot11) | 1 |
| Sims that died ("Falecer") | 23 (10 lots) | 11 (7 lots) | 11 (8 lots) | 27 (10 lots) | 23 (10 lots) |
| Sims stuck > 6 game hours | 18 | 26 | 17 | 21 | 29 |
| Motives pinned at -100 > 6 game hours | 59 | 37 | 33 | 30 | 23 |
| Save round trips that differ | 68 lots | 68 | 1 (N001_Lot80) | 68 | 1 (N001_Lot80) |
| Instruction budget (20000) hit | 3 | 16 | 6 | 54 | 17 |

What changed (TODO.md, "Rodada 13, sobrevivência 3"; `docs/primitives-mac.md` §4g):
- **Round trips**: since the global sim objects (r12-life3) every `lot-save --check` differed: the curtain and the tutorial controllers delete themselves after running and the reload makes them again, as the game does. The check now skips the objects the reload made (`VM::globalsMadeAtLoad`). N001_Lot80 (an empty lot whose 125 global objects make children) still differs.
- **The shrubs** no longer reset every hour: `TryChangeMaterial` looks the mesh group's name up and never reads it, so a missing STR# 0x87 entry is no error.
- **Reset after an error** as `HandleError` → `Cleanup`: the Reset path zeroes the object data but for the binary's list and runs Init (fewer budget hits: a looping object starts clean).
- **The social worker's car** already left (r12-life3); the child goes with it (removed by "Sub - Send Passengers Out of World"); the "stuck in Levando a criança" findings were the soak's (3 of the 26 in A). With the walking merge (B) N001_Lot36's Beau is again kept in the car: to check with the walking front.
- **Set to Next's "where" filter** only on node version 1 (the base game's version-0 nodes have other bytes there): this broke the baby's care on N002_Lot10 since r12-survival2 (414ef03; bisected).
- Nobody calls the firefighters but the smoke alarm in the fire's room (game behavior).
- **The walking merge** (9d7607c, "the per-step placement test takes the Sims' bodies") raises the deaths: bisected on N001_Lot68 + N002_Lot10, 1 death at caf8207 and 8 at 9d7607c (B's 27 against A's 11). Outside this front (walking/routing).

# Soak report (round 12)

Run of `tools/soak.sh` over the 69 lots (3 game days of autonomy each, 129600 ticks), headless (`SOAK_NO_APP=1`), 2 lots at a time. "Before" is main b87a616 (the round-11 report's own code, re-run at the start of this round); "after" is the r12-survival2 branch rebased on main 1389fd8. The soak now also counts the household's Mains **reset** after an error (the game resets the object, `docs/primitives-mac.md` §4g); "dead" Mains no longer stay dead. "Died" counts every Sim that ran "Falecer" (they are now reaped: the Grim Reaper's Main no longer dies).

## Before / after (round 12, survival)

| Category | Before | After |
|---|---|---|
| Crashes / hangs | 0 / 0 | 0 / 0 |
| Load errors | 1 lot (N002_Lot32, not a lot) | 1 lot |
| Household Mains dead | 2 (Ophelia, Circe) | 0 (and 0 reset); the Grim Reaper's Main no longer dies |
| Sims that died ("Falecer") | 23 (8 lots) | 20 (9 lots) |
| Sims stuck > 6 game hours | 12 (8 lots) | 15 (8 lots: 7 long sleeps, 3 children carried off in the social worker's car that never leaves, 2 "Usar" deadlock in N001_Lot49, Pascal's birth) |
| Motives pinned at -100 > 6 game hours | 187 | 88 (47 in the run before the last fix) |
| Save round trips that differ | 0 | 0 |
| Instruction budget (20000) hit | 0 | 13 (N002_Lot22, during a fire; the object is reset) |

Engine causes fixed this round (TODO.md, "Rodada 12, Sims vivos e destravados"): the Grim Reaper had no neighbor (Force Recalc of Wants of neighbor 0) and reaped again the Sim it had just turned into an urn (Set to Next skips objects on the kill queue); text notifications need only the tree's owner; a failed Main is reset as the game does; Find Best Object for Function looks only in the rooms the Sim can reach, which found the diagonal doors that never opened (upstairs of N001_Lot68 and N003_Lot52); a multi-tile object's bone slots are placed from its lead (the straight staircase: nobody reached the upper floor of N001_Lot11, so "Dormir" always failed); bone slots with directions stand at the bones (the cheap fridge put N002_Lot22's family on the counter: they starved); Set to Next 3 on the last part of a burnt multi-tile object (51 budget hits in N001_Lot36); a Sim on my goal steps aside 1-3 tiles (the oven of a stove whose cooktop is in use); Idle for Input nests a higher-priority interaction over the running one (the child the social worker takes). The deaths left are mostly kitchen fires of cooks without skill on lots without a smoke alarm (game behavior, §4g "Cozinheiros que saem do forno"), and N003_Lot46/N003_Lot192.

# Soak report (round 11)

Run of `tools/soak.sh` over the 69 lots of N001, N002, N003 and Tutorial (3 game days of autonomy each, 129600 ticks), headless (`SOAK_NO_APP=1`), 2 lots at a time. "Before" is the round-11 branch point (main 3872cc0); "after" is the r11-food branch rebased on main 55e68fe (so it also has main's overlay and blocked-walker changes).

## Before / after (round 11, food and fire)

| Category | Before | After |
|---|---|---|
| Crashes / hangs | 0 / 0 | 0 / 0 |
| Load errors | 1 lot (N002_Lot32, not a lot) | 1 lot |
| Household Mains dead | not counted (the soak grepped a message autonomy no longer prints) | 2 (N002_Lot2 Ophelia: "Force Recalc of Wants: no neighbor 0"; N002_Lot3 Circe: text notification without a stack object) |
| Sims that died ("Falecer") | 26 (11 lots) | 23 (8 lots) |
| Sims stuck > 6 game hours | 23 (11 lots) | 22 (8 lots; 12 are long sleeps or music) |
| Motives pinned at -100 > 6 game hours | 167 (36 Sims) | 165 (34 Sims) |
| Save round trips that differ | 0 | 0 |
| Instruction budget (20000) hit | 0 | 0 |

Lots that no longer lose anyone: N001_Lot16 (Don threw his frozen dinner out of the oven and set the kitchen on fire), N001_Lot36 (Brandi's bath push had no toddler: dead Main, starved), N001_Lot49, N002_Lot10, N002_Lot3. Lots that lose more: N003_Lot192 (2 -> 4), N003_Lot46 (1 -> 4), N002_Lot4 (0 -> 2); with N001_Lot68 and N003_Lot52 (4 each) the deaths left are almost all **fires**: a low-skill cook burns the food ("Main - Cook - Loop": 20% once it overcooks) or cooked food is left in the oven (with "Free Will", global 30, on, "Cook - _Type - Bake" puts the dish in and leaves; "Push - Get Food" calls the cook back, who often arrives after the 10 minutes before it burns), and these lots have no smoke alarm in the kitchen (only 6 of the 69 lots have one), so no firefighter comes and the household's Sims only panic (they extinguish only when the player says so, docs/primitives-mac.md §4f). With an alarm the firefighter now comes and puts it out (N002_Lot10 with a fire set at tick 600: out in about an hour, nobody dies).

Engine causes fixed this round (TODO.md, "Rodada 11, comida e fogo"): a waiting entry interrupted the running one only when its priority is higher (cTSPerson::ShouldInterrupt; FreeSO lets autonomous entries interrupt each other, so every "*Push Look At" cancelled cooking); Find Best Object for Function as the binary (in use, burning, disabled objects skipped; check tree for the reference; first place flag; hypotenuse); fire trucks drive in and firefighters run their own extinguish test (Run Tree by Name picked the global table); arriving Sims get their object and lead tile IDs (a passer-by's Main died) and lot-save --check reads a moved-in lot back right; Push Interaction with the last FBA's entry takes its parameters (the toddler of the bath); new objects get their birth date (every new meal was 22,000 hours old and spoiled); action strings from the running code's semi-global ("Ask For Lessons From ..." on food). Special dialogs (types >= 5) wait for the player in the game too (no timeout): Circe's "Atender" (N002_Lot3) is that.

## Round 9

Run of `tools/soak.sh` over the 69 lots of N001, N002, N003 and Tutorial (3 game days of autonomy each, 129600 ticks), without the app step (`SOAK_NO_APP=1`: headless only this round), 2 lots at a time. "Before" is the branch point (main de88a7d, the dialogs waiting for the player forever); "after" is the round-9 sim-fixes branch. Dialogs now time out as the game's (the last button after 4000 + 40 x length ms of game time), so the soak no longer needs `--dialog-answer`.

## Before / after

| Category | Before | After |
|---|---|---|
| Crashes / hangs | 0 / 0 | 0 / 0 |
| Load errors | 1 lot (N002_Lot32, not a lot) | 1 lot |
| Mains dead | 0 | 0 |
| Sims stuck > 6 game hours (besides sleep, work, school) | 19 (10 in "Atender") | 12 (1 "Atender": N002_Lot3 Circe, a special dialog type with no time limit) |
| Sims that died ("Falecer") | 24 (13 lots) | 27 (11 lots) |
| Motives pinned at -100 > 6 game hours | 182 (38 Sims) | 126 (29 Sims) |
| Save round trips that differ | 14 lots | 0 |
| Instruction budget (20000) hit | 1 (N001_Lot36) | 0 |

Deaths: before, 10 Sims were frozen in a phone dialog and could not run "Falecer" even with every motive at -100; now they live on, and the deaths left are starvation. Engine bugs behind deaths fixed this round: every modular deck stair locked (N003_Lot192: the Sims in the pit could not reach the fridge), the "Desespero" pushes failing (Push Interaction read the found entry from the wrong object; the Main died), glasses and trays dropped on the floor closing kitchen tiles, "Align Trashcan To Sim" and "Chess - Reset Sim" spinning out the budget. Still open (TODO.md): cooking fails in a loop ("Food Chain - Failure - Cook", no prep surface found) and each failure leaves a tray; "Fogo!" (N001_Lot49 Darren, 7-8 hours); special dialogs (type >= 5) have no time limit.


# Generated tables (after, seed 1)

Generated by `tools/soak.sh` on 2026-10-12: 69 lots, autonomy for 129600 ticks (72 game hours, 1800 ticks each), lot-save of 300 ticks with --check, app run of 600 VM ticks. Hang limit 900s (app 300s).

## Counts by category

| Category | Lots | Findings |
|---|---|---|
| crash | 0 | 0 |
| hang | 0 | 0 |
| error | 1 | 2 |
| dead | 0 | 0 |
| reset | 1 | 2 |
| stuck | 11 | 35 |
| died | 6 | 12 |
| pinned | 6 | 15 |
| roundtrip | 0 | 0 |
| instruction budget (20000) hit | 0 | 0 |

## Findings per lot

| Lot | Category | Where | Detail |
|---|---|---|---|
| N001_Lot11 | stuck | autonomy | Alexander#142 in "Dormir" for 6.8 game hours |
| N001_Lot11 | stuck | autonomy | Alexander#142 in "Dormir" for 9.3 game hours |
| N001_Lot11 | stuck | autonomy | Alexander#142 in "Tocar" for 8.1 game hours |
| N001_Lot11 | stuck | autonomy | Alexander#142 in "Tocar" since tick 118115 (6.4 game hours at the end) |
| N001_Lot11 | stuck | autonomy | Cassandra#179 in "Trabalhando" for 6.6 game hours |
| N001_Lot16 | pinned | autonomy | Don#197 comfort at -100 for more than 6 game hours |
| N001_Lot16 | pinned | autonomy | Don#197 energy at -100 for more than 6 game hours |
| N001_Lot16 | pinned | autonomy | Don#197 hygiene at -100 for more than 6 game hours |
| N001_Lot18 | pinned | autonomy | Dina#271 hygiene at -100 for more than 6 game hours |
| N001_Lot18 | pinned | autonomy | Dina#271 social at -100 for more than 6 game hours |
| N001_Lot18 | pinned | autonomy | Nina#210 social at -100 for more than 6 game hours |
| N001_Lot36 | pinned | autonomy | Beau#270 hygiene at -100 for more than 6 game hours |
| N001_Lot36 | stuck | autonomy | Dustin#248 in "Dormir" for 6.3 game hours |
| N001_Lot49 | stuck | autonomy | Darren#347 in "Cochilar" for 7.4 game hours |
| N001_Lot49 | stuck | autonomy | Darren#347 in "Dormir" for 7.1 game hours |
| N001_Lot68 | died | autonomy | Mary-Sue#37 in "Falecer" since tick 51470 (43.4 game hours at the end) |
| N001_Lot68 | stuck | autonomy | Daniel#116 in "Dormir" for 6.3 game hours |
| N002_Lot10 | died | autonomy | Jenny#386 in "Falecer" since tick 51852 (43.2 game hours at the end) |
| N002_Lot10 | died | autonomy | Johnny#389 in "Falecer" since tick 51059 (43.6 game hours at the end) |
| N002_Lot10 | stuck | autonomy | Jenny#386 in "Cochilar" for 6.3 game hours |
| N002_Lot10 | stuck | autonomy | Jill#454 in "Jogar" for 9.6 game hours |
| N002_Lot10 | stuck | autonomy | Johnny#389 in "Dormir" for 7.6 game hours |
| N002_Lot2 | died | autonomy | Olive#119 in "Falecer" since tick 95059 (19.2 game hours at the end) |
| N002_Lot2 | stuck | autonomy | Ophelia#230 in "Cochilar" for 7.1 game hours |
| N002_Lot2 | stuck | autonomy | Ophelia#230 in "Dormir" for 9.2 game hours |
| N002_Lot22 | died | autonomy | Buck#151 in "Falecer" since tick 15325 (63.5 game hours at the end) |
| N002_Lot22 | died | autonomy | General in "Falecer" since tick 15980 (63.1 game hours at the end) |
| N002_Lot22 | died | autonomy | Ripp#149 in "Falecer" since tick 67564 (34.5 game hours at the end) |
| N002_Lot22 | died | autonomy | Tank#144 in "Falecer" since tick 68211 (34.1 game hours at the end) |
| N002_Lot22 | pinned | autonomy | Ripp#149 hygiene at -100 for more than 6 game hours |
| N002_Lot22 | pinned | autonomy | Ripp#149 social at -100 for more than 6 game hours |
| N002_Lot22 | pinned | autonomy | Tank#144 comfort at -100 for more than 6 game hours |
| N002_Lot22 | pinned | autonomy | Tank#144 fun at -100 for more than 6 game hours |
| N002_Lot3 | pinned | autonomy | Circe#147 social at -100 for more than 6 game hours |
| N002_Lot3 | pinned | autonomy | Nervous#182 hygiene at -100 for more than 6 game hours |
| N002_Lot3 | stuck | autonomy | Circe#147 in "Trabalhando" for 9.6 game hours |
| N002_Lot32 | error | autonomy | exit 1: error: no elevation array (3ARY 0x01): not a lot package? |
| N002_Lot32 | error | lot-save | exit 1: error: no elevation array (3ARY 0x01): not a lot package? |
| N002_Lot4 | reset | autonomy | Pascal#142: 2 x Tree table not found (object 204) |
| N002_Lot4 | reset | autonomy | Vidcund#152: 2 x Animation name is empty string. |
| N002_Lot4 | stuck | autonomy | Lazlo#137 in "Dormir" for 9.5 game hours |
| N002_Lot4 | stuck | autonomy | Lazlo#137 in "Trabalhando" for 6.7 game hours |
| N002_Lot4 | stuck | autonomy | Lazlo#137 in "Trabalhando" for 6.8 game hours |
| N002_Lot4 | stuck | autonomy | Pascal#142 in "Trabalhando" for 6.6 game hours |
| N002_Lot4 | stuck | autonomy | Vidcund#152 in "Trabalhando" for 6.9 game hours |
| N003_Lot192 | died | autonomy | Oberon#39 in "Falecer" since tick 81367 (26.8 game hours at the end) |
| N003_Lot192 | died | autonomy | Puck#49 in "Falecer" since tick 80438 (27.3 game hours at the end) |
| N003_Lot192 | died | autonomy | Titania#12 in "Falecer" since tick 79685 (27.7 game hours at the end) |
| N003_Lot192 | stuck | autonomy | Bottom#1 in "Dormir" for 7.9 game hours |
| N003_Lot192 | stuck | autonomy | Bottom#1 in "Na Escola" for 6.6 game hours |
| N003_Lot192 | stuck | autonomy | Bottom#1 in "Na Escola" for 6.7 game hours |
| N003_Lot192 | stuck | autonomy | Oberon#39 in "Dormir" for 6.2 game hours |
| N003_Lot192 | stuck | autonomy | Oberon#39 in "Dormir" for 6.3 game hours |
| N003_Lot192 | stuck | autonomy | Titania#12 in "Dormir" for 6.6 game hours |
| N003_Lot46 | died | autonomy | Patrizio#135 in "Falecer" since tick 83913 (25.4 game hours at the end) |
| N003_Lot46 | stuck | autonomy | Isabella#137 in "Dormir" for 6.0 game hours |
| N003_Lot46 | stuck | autonomy | Mercutio#381 in "Dormir" for 6.8 game hours |
| N003_Lot46 | stuck | autonomy | Mercutio#381 in "Dormir" for 7.6 game hours |
| N003_Lot46 | stuck | autonomy | Patrizio#135 in "Cochilar" for 6.3 game hours |
| N003_Lot46 | stuck | autonomy | Romeo#402 in "Cochilar" for 6.3 game hours |
| N003_Lot46 | stuck | autonomy | Romeo#402 in "Dormir" for 6.1 game hours |
| N003_Lot52 | pinned | autonomy | Juliette#140 fun at -100 for more than 6 game hours |
| N003_Lot52 | pinned | autonomy | Tybalt#198 fun at -100 for more than 6 game hours |
| N003_Lot52 | stuck | autonomy | Consort#136 in "Trabalhando" for 7.3 game hours |
| N003_Lot52 | stuck | autonomy | Juliette#140 in "Cochilar" for 6.2 game hours |

## Stacks of crashes


## Timing (seconds, per lot: autonomy / lot-save / app)

- N001_Lot11: 13 / 0 / 
- N001_Lot16: 6 / 0 / 
- N001_Lot18: 8 / 0 / 
- N001_Lot22: 3 / 1 / 
- N001_Lot24: 2 / 1 / 
- N001_Lot32: 3 / 1 / 
- N001_Lot36: 5 / 0 / 
- N001_Lot38: 2 / 1 / 
- N001_Lot40: 3 / 0 / 
- N001_Lot47: 4 / 1 / 
- N001_Lot49: 9 / 1 / 
- N001_Lot51: 3 / 1 / 
- N001_Lot53: 3 / 0 / 
- N001_Lot55: 4 / 0 / 
- N001_Lot57: 4 / 0 / 
- N001_Lot59: 6 / 1 / 
- N001_Lot64: 3 / 1 / 
- N001_Lot66: 4 / 1 / 
- N001_Lot68: 12 / 1 / 
- N001_Lot70: 2 / 1 / 
- N001_Lot73: 8 / 1 / 
- N001_Lot75: 5 / 1 / 
- N001_Lot77: 8 / 1 / 
- N001_Lot79: 7 / 1 / 
- N001_Lot80: 1 / 1 / 
- N001_Lot82: 16 / 1 / 
- N002_Lot10: 12 / 0 / 
- N002_Lot2: 10 / 0 / 
- N002_Lot22: 16 / 1 / 
- N002_Lot23: 4 / 1 / 
- N002_Lot25: 6 / 0 / 
- N002_Lot27: 2 / 1 / 
- N002_Lot29: 13 / 0 / 
- N002_Lot3: 16 / 1 / 
- N002_Lot31: 11 / 0 / 
- N002_Lot32: 0 / 0 / 
- N002_Lot4: 14 / 0 / 
- N003_Lot126: 6 / 0 / 
- N003_Lot128: 7 / 0 / 
- N003_Lot133: 6 / 0 / 
- N003_Lot135: 4 / 0 / 
- N003_Lot144: 4 / 0 / 
- N003_Lot149: 3 / 0 / 
- N003_Lot153: 4 / 1 / 
- N003_Lot157: 4 / 0 / 
- N003_Lot159: 3 / 1 / 
- N003_Lot164: 3 / 0 / 
- N003_Lot166: 3 / 1 / 
- N003_Lot168: 5 / 0 / 
- N003_Lot171: 2 / 1 / 
- N003_Lot175: 2 / 1 / 
- N003_Lot179: 6 / 1 / 
- N003_Lot181: 3 / 0 / 
- N003_Lot183: 5 / 0 / 
- N003_Lot187: 1 / 1 / 
- N003_Lot192: 30 / 1 / 
- N003_Lot195: 5 / 1 / 
- N003_Lot45: 3 / 1 / 
- N003_Lot46: 14 / 0 / 
- N003_Lot47: 4 / 1 / 
- N003_Lot48: 4 / 1 / 
- N003_Lot50: 2 / 1 / 
- N003_Lot52: 18 / 1 / 
- Tutorial_Lot1: 1 / 1 / 
- Tutorial_Lot14: 3 / 0 / 
- Tutorial_Lot18: 7 / 0 / 
- Tutorial_Lot19: 7 / 0 / 
- Tutorial_Lot3: 6 / 0 / 
- Tutorial_Lot4: 2 / 1 / 
