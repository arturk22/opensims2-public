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


# Generated tables (after)

Generated by `tools/soak.sh` on 2026-10-12: 69 lots, autonomy for 129600 ticks (72 game hours, 1800 ticks each), lot-save of 300 ticks with --check, app run of 600 VM ticks. Hang limit 900s (app 300s).

## Counts by category

| Category | Lots | Findings |
|---|---|---|
| crash | 0 | 0 |
| hang | 0 | 0 |
| error | 1 | 2 |
| dead | 0 | 0 |
| reset | 1 | 1 |
| stuck | 10 | 29 |
| died | 10 | 23 |
| pinned | 8 | 23 |
| roundtrip | 1 | 1 |
| instruction budget (20000) hit | 2 | 17 |

## Findings per lot

| Lot | Category | Where | Detail |
|---|---|---|---|
| N001_Lot11 | stuck | autonomy | Alexander#142 in "Cochilar" for 9.6 game hours |
| N001_Lot11 | stuck | autonomy | Alexander#142 in "Dormir" for 6.8 game hours |
| N001_Lot11 | stuck | autonomy | Cassandra#179 in "Praticar Pintura" for 7.5 game hours |
| N001_Lot11 | stuck | autonomy | Cassandra#179 in "Trabalhando" for 6.5 game hours |
| N001_Lot11 | stuck | autonomy | Cassandra#179 in "Trabalhando" for 6.6 game hours |
| N001_Lot16 | died | autonomy | Don#197 in "Falecer" since tick 108691 (11.6 game hours at the end) |
| N001_Lot16 | pinned | autonomy | Don#197 comfort at -100 for more than 6 game hours |
| N001_Lot16 | pinned | autonomy | Don#197 hygiene at -100 for more than 6 game hours |
| N001_Lot16 | pinned | autonomy | Don#197 social at -100 for more than 6 game hours |
| N001_Lot18 | died | autonomy | Nina#210 in "Falecer" since tick 47054 (45.9 game hours at the end) |
| N001_Lot18 | pinned | autonomy | Dina#271 comfort at -100 for more than 6 game hours |
| N001_Lot18 | pinned | autonomy | Dina#271 hygiene at -100 for more than 6 game hours |
| N001_Lot18 | pinned | autonomy | Dina#271 social at -100 for more than 6 game hours |
| N001_Lot18 | pinned | autonomy | Nina#210 social at -100 for more than 6 game hours |
| N001_Lot36 | pinned | autonomy | Beau#270 hygiene at -100 for more than 6 game hours |
| N001_Lot36 | pinned | autonomy | Dustin#248 fun at -100 for more than 6 game hours |
| N001_Lot36 | stuck | autonomy | Beau#270 in "Levando a criança" since tick 44805 (47.1 game hours at the end) |
| N001_Lot36 | stuck | autonomy | Brandi#2 in "Dar à Luz" since tick 104687 (13.8 game hours at the end) |
| N001_Lot36 | stuck | autonomy | Brandi#2 in "Dormir" for 11.1 game hours |
| N001_Lot49 | stuck | autonomy | Darren#347 in "Dormir" for 8.0 game hours |
| N001_Lot68 | died | autonomy | Angela#11 in "Falecer" since tick 46652 (46.1 game hours at the end) |
| N001_Lot68 | died | autonomy | Daniel#116 in "Falecer" since tick 49917 (44.3 game hours at the end) |
| N001_Lot68 | died | autonomy | Lilith#114 in "Falecer" since tick 51691 (43.3 game hours at the end) |
| N001_Lot68 | died | autonomy | Mary-Sue#37 in "Falecer" since tick 47145 (45.8 game hours at the end) |
| N001_Lot68 | budget | autonomy | 2 times |
| N001_Lot80 | roundtrip | lot-save | exit 1:   VM state after load -> save -> load: differs:;    object 134: missing from the second;    object 138: missing from the second; |
| N002_Lot10 | died | autonomy | Jill#454 in "Falecer" since tick 112906 (9.3 game hours at the end) |
| N002_Lot10 | stuck | autonomy | Jenny#386 in "Trabalhando" for 7.7 game hours |
| N002_Lot10 | stuck | autonomy | Jill#454 in "Cochilar" for 7.8 game hours |
| N002_Lot10 | stuck | autonomy | Johnny#389 in "Dormir" for 7.2 game hours |
| N002_Lot10 | stuck | autonomy | Johnny#389 in "Dormir" for 7.7 game hours |
| N002_Lot10 | stuck | autonomy | Pollination in "Dormir" for 7.0 game hours |
| N002_Lot2 | died | autonomy | Olive#119 in "Falecer" since tick 89437 (22.3 game hours at the end) |
| N002_Lot2 | pinned | autonomy | Ophelia#230 fun at -100 for more than 6 game hours |
| N002_Lot2 | stuck | autonomy | Ophelia#230 in "Dormir" for 8.4 game hours |
| N002_Lot22 | died | autonomy | Buck#151 in "Falecer" since tick 28762 (56.0 game hours at the end) |
| N002_Lot22 | died | autonomy | General in "Falecer" since tick 48013 (45.3 game hours at the end) |
| N002_Lot22 | died | autonomy | Ripp#149 in "Falecer" since tick 32976 (53.7 game hours at the end) |
| N002_Lot22 | died | autonomy | Tank#144 in "Falecer" since tick 29414 (55.7 game hours at the end) |
| N002_Lot3 | died | autonomy | Circe#147 in "Falecer" since tick 83252 (25.7 game hours at the end) |
| N002_Lot3 | died | autonomy | Loki#130 in "Falecer" since tick 84659 (25.0 game hours at the end) |
| N002_Lot3 | died | autonomy | Nervous#182 in "Falecer" since tick 97144 (18.0 game hours at the end) |
| N002_Lot3 | pinned | autonomy | Circe#147 comfort at -100 for more than 6 game hours |
| N002_Lot3 | pinned | autonomy | Circe#147 hygiene at -100 for more than 6 game hours |
| N002_Lot3 | pinned | autonomy | Circe#147 social at -100 for more than 6 game hours |
| N002_Lot3 | pinned | autonomy | Nervous#182 hygiene at -100 for more than 6 game hours |
| N002_Lot3 | stuck | autonomy | Circe#147 in "Trabalhando" for 9.3 game hours |
| N002_Lot3 | budget | autonomy | 15 times |
| N002_Lot32 | error | autonomy | exit 1: error: no elevation array (3ARY 0x01): not a lot package? |
| N002_Lot32 | error | lot-save | exit 1: error: no elevation array (3ARY 0x01): not a lot package? |
| N002_Lot4 | pinned | autonomy | Vidcund#152 fun at -100 for more than 6 game hours |
| N002_Lot4 | pinned | autonomy | Vidcund#152 hygiene at -100 for more than 6 game hours |
| N002_Lot4 | pinned | autonomy | Vidcund#152 social at -100 for more than 6 game hours |
| N002_Lot4 | stuck | autonomy | Lazlo#137 in "Trabalhando" for 6.6 game hours |
| N002_Lot4 | stuck | autonomy | Lazlo#137 in "Trabalhando" for 6.7 game hours |
| N002_Lot4 | stuck | autonomy | Pascal#142 in "Dar à Luz" since tick 8541 (67.3 game hours at the end) |
| N003_Lot192 | died | autonomy | Puck#49 in "Falecer" since tick 50910 (43.7 game hours at the end) |
| N003_Lot192 | pinned | autonomy | Puck#49 comfort at -100 for more than 6 game hours |
| N003_Lot192 | pinned | autonomy | Puck#49 hygiene at -100 for more than 6 game hours |
| N003_Lot192 | reset | autonomy | Oberon#39: 1 x object 137 is not a person |
| N003_Lot192 | stuck | autonomy | Bottom#1 in "Dormir" for 12.5 game hours |
| N003_Lot192 | stuck | autonomy | Bottom#1 in "Dormir" for 8.1 game hours |
| N003_Lot192 | stuck | autonomy | Bottom#1 in "Na Escola" for 6.8 game hours |
| N003_Lot192 | stuck | autonomy | Oberon#39 in "Dormir" for 6.8 game hours |
| N003_Lot192 | stuck | autonomy | Oberon#39 in "Dormir" for 8.2 game hours |
| N003_Lot192 | stuck | autonomy | Oberon#39 in "Trabalhando" for 6.5 game hours |
| N003_Lot192 | stuck | autonomy | Titania#12 in "Dormir" for 7.9 game hours |
| N003_Lot46 | died | autonomy | Mercutio#381 in "Falecer" since tick 88823 (22.7 game hours at the end) |
| N003_Lot46 | died | autonomy | Patrizio#135 in "Falecer" since tick 80842 (27.1 game hours at the end) |
| N003_Lot46 | died | autonomy | Romeo#402 in "Falecer" since tick 80887 (27.1 game hours at the end) |
| N003_Lot46 | stuck | autonomy | Mercutio#381 in "Dormir" for 7.4 game hours |
| N003_Lot52 | died | autonomy | Consort#136 in "Falecer" since tick 60594 (38.3 game hours at the end) |
| N003_Lot52 | died | autonomy | Hermia#158 in "Falecer" since tick 28239 (56.3 game hours at the end) |
| N003_Lot52 | died | autonomy | Juliette#140 in "Falecer" since tick 60881 (38.2 game hours at the end) |
| N003_Lot52 | died | autonomy | Tybalt#198 in "Falecer" since tick 16097 (63.1 game hours at the end) |
| N003_Lot52 | pinned | autonomy | Consort#136 fun at -100 for more than 6 game hours |
| N003_Lot52 | pinned | autonomy | Consort#136 hygiene at -100 for more than 6 game hours |
| N003_Lot52 | pinned | autonomy | Consort#136 social at -100 for more than 6 game hours |
| N003_Lot52 | pinned | autonomy | Juliette#140 fun at -100 for more than 6 game hours |
| Tutorial_Lot18 | stuck | autonomy | Tutorial in "Cochilar" for 6.2 game hours |
| Tutorial_Lot18 | stuck | autonomy | Tutorial in "Cochilar" for 6.3 game hours |

## Stacks of crashes


## Timing (seconds, per lot: autonomy / lot-save / app)

- N001_Lot11: 22 / 1 / 
- N001_Lot16: 9 / 1 / 
- N001_Lot18: 13 / 1 / 
- N001_Lot22: 6 / 1 / 
- N001_Lot24: 5 / 1 / 
- N001_Lot32: 5 / 1 / 
- N001_Lot36: 8 / 1 / 
- N001_Lot38: 5 / 1 / 
- N001_Lot40: 5 / 0 / 
- N001_Lot47: 8 / 0 / 
- N001_Lot49: 11 / 1 / 
- N001_Lot51: 4 / 1 / 
- N001_Lot53: 5 / 0 / 
- N001_Lot55: 7 / 1 / 
- N001_Lot57: 6 / 1 / 
- N001_Lot59: 10 / 0 / 
- N001_Lot64: 5 / 0 / 
- N001_Lot66: 7 / 1 / 
- N001_Lot68: 15 / 1 / 
- N001_Lot70: 4 / 1 / 
- N001_Lot73: 13 / 1 / 
- N001_Lot75: 8 / 1 / 
- N001_Lot77: 17 / 1 / 
- N001_Lot79: 13 / 1 / 
- N001_Lot80: 3 / 1 / 
- N001_Lot82: 28 / 1 / 
- N002_Lot10: 23 / 1 / 
- N002_Lot2: 8 / 1 / 
- N002_Lot22: 16 / 1 / 
- N002_Lot23: 3 / 1 / 
- N002_Lot25: 5 / 1 / 
- N002_Lot27: 4 / 0 / 
- N002_Lot29: 13 / 1 / 
- N002_Lot3: 16 / 1 / 
- N002_Lot31: 13 / 1 / 
- N002_Lot32: 0 / 0 / 
- N002_Lot4: 15 / 1 / 
- N003_Lot126: 5 / 1 / 
- N003_Lot128: 7 / 1 / 
- N003_Lot133: 7 / 0 / 
- N003_Lot135: 4 / 1 / 
- N003_Lot144: 3 / 1 / 
- N003_Lot149: 3 / 0 / 
- N003_Lot153: 4 / 0 / 
- N003_Lot157: 3 / 1 / 
- N003_Lot159: 4 / 0 / 
- N003_Lot164: 3 / 0 / 
- N003_Lot166: 4 / 0 / 
- N003_Lot168: 4 / 1 / 
- N003_Lot171: 3 / 1 / 
- N003_Lot175: 2 / 1 / 
- N003_Lot179: 6 / 0 / 
- N003_Lot181: 2 / 1 / 
- N003_Lot183: 5 / 1 / 
- N003_Lot187: 3 / 1 / 
- N003_Lot192: 58 / 1 / 
- N003_Lot195: 8 / 1 / 
- N003_Lot45: 5 / 0 / 
- N003_Lot46: 18 / 1 / 
- N003_Lot47: 5 / 1 / 
- N003_Lot48: 7 / 1 / 
- N003_Lot50: 3 / 1 / 
- N003_Lot52: 17 / 1 / 
- Tutorial_Lot1: 2 / 1 / 
- Tutorial_Lot14: 5 / 1 / 
- Tutorial_Lot18: 7 / 0 / 
- Tutorial_Lot19: 7 / 1 / 
- Tutorial_Lot3: 6 / 0 / 
- Tutorial_Lot4: 2 / 1 / 
