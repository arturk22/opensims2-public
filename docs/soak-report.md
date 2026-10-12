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
| reset | 0 | 0 |
| stuck | 8 | 15 |
| died | 9 | 20 |
| pinned | 14 | 88 |
| roundtrip | 0 | 0 |
| instruction budget (20000) hit | 1 | 13 |

## Findings per lot

| Lot | Category | Where | Detail |
|---|---|---|---|
| N001_Lot11 | pinned | autonomy | Alexander#142 bladder at -100 for more than 6 game hours |
| N001_Lot11 | pinned | autonomy | Alexander#142 fun at -100 for more than 6 game hours |
| N001_Lot11 | pinned | autonomy | Cassandra#179 fun at -100 for more than 6 game hours |
| N001_Lot11 | pinned | autonomy | Mortimer#197 bladder at -100 for more than 6 game hours |
| N001_Lot11 | pinned | autonomy | Mortimer#197 fun at -100 for more than 6 game hours |
| N001_Lot11 | stuck | autonomy | Alexander#142 in "Dormir" for 6.4 game hours |
| N001_Lot11 | stuck | autonomy | Alexander#142 in "Levando a criança" since tick 69895 (33.2 game hours at the end) |
| N001_Lot11 | stuck | autonomy | Template in "Leave" for 17.6 game hours |
| N001_Lot16 | died | autonomy | Don#197 in "Falecer" since tick 82602 (26.1 game hours at the end) |
| N001_Lot16 | pinned | autonomy | Don#197 comfort at -100 for more than 6 game hours |
| N001_Lot16 | pinned | autonomy | Don#197 hygiene at -100 for more than 6 game hours |
| N001_Lot16 | pinned | autonomy | Don#197 social at -100 for more than 6 game hours |
| N001_Lot18 | died | autonomy | Nina#210 in "Falecer" since tick 63328 (36.8 game hours at the end) |
| N001_Lot18 | pinned | autonomy | Dina#271 comfort at -100 for more than 6 game hours |
| N001_Lot18 | pinned | autonomy | Dina#271 hygiene at -100 for more than 6 game hours |
| N001_Lot18 | pinned | autonomy | Dina#271 social at -100 for more than 6 game hours |
| N001_Lot18 | pinned | autonomy | Nina#210 social at -100 for more than 6 game hours |
| N001_Lot36 | died | autonomy | Brandi#2 in "Falecer" since tick 98255 (17.4 game hours at the end) |
| N001_Lot36 | died | autonomy | Dustin#248 in "Falecer" since tick 76157 (29.7 game hours at the end) |
| N001_Lot36 | pinned | autonomy | Beau#270 bladder at -100 for more than 6 game hours |
| N001_Lot36 | pinned | autonomy | Beau#270 energy at -100 for more than 6 game hours |
| N001_Lot36 | pinned | autonomy | Beau#270 fun at -100 for more than 6 game hours |
| N001_Lot36 | pinned | autonomy | Beau#270 hygiene at -100 for more than 6 game hours |
| N001_Lot36 | pinned | autonomy | Brandi#2 comfort at -100 for more than 6 game hours |
| N001_Lot36 | pinned | autonomy | Brandi#2 fun at -100 for more than 6 game hours |
| N001_Lot36 | pinned | autonomy | Brandi#2 hygiene at -100 for more than 6 game hours |
| N001_Lot36 | pinned | autonomy | Brandi#2 social at -100 for more than 6 game hours |
| N001_Lot36 | pinned | autonomy | Dustin#248 hygiene at -100 for more than 6 game hours |
| N001_Lot36 | pinned | autonomy | Dustin#248 social at -100 for more than 6 game hours |
| N001_Lot36 | stuck | autonomy | Beau#270 in "Levando a criança" since tick 68000 (34.2 game hours at the end) |
| N001_Lot36 | stuck | autonomy | Dustin#248 in "Dormir" for 6.3 game hours |
| N001_Lot49 | pinned | autonomy | Darren#347 bladder at -100 for more than 6 game hours |
| N001_Lot49 | pinned | autonomy | Darren#347 comfort at -100 for more than 6 game hours |
| N001_Lot49 | pinned | autonomy | Darren#347 energy at -100 for more than 6 game hours |
| N001_Lot49 | pinned | autonomy | Darren#347 fun at -100 for more than 6 game hours |
| N001_Lot49 | pinned | autonomy | Darren#347 hunger at -100 for more than 6 game hours |
| N001_Lot49 | pinned | autonomy | Darren#347 hygiene at -100 for more than 6 game hours |
| N001_Lot49 | pinned | autonomy | Darren#347 social at -100 for more than 6 game hours |
| N001_Lot49 | pinned | autonomy | Dirk#367 bladder at -100 for more than 6 game hours |
| N001_Lot49 | pinned | autonomy | Dirk#367 comfort at -100 for more than 6 game hours |
| N001_Lot49 | pinned | autonomy | Dirk#367 energy at -100 for more than 6 game hours |
| N001_Lot49 | pinned | autonomy | Dirk#367 fun at -100 for more than 6 game hours |
| N001_Lot49 | pinned | autonomy | Dirk#367 hunger at -100 for more than 6 game hours |
| N001_Lot49 | pinned | autonomy | Dirk#367 hygiene at -100 for more than 6 game hours |
| N001_Lot49 | pinned | autonomy | Dirk#367 social at -100 for more than 6 game hours |
| N001_Lot49 | stuck | autonomy | Darren#347 in "Usar" since tick 62673 (37.2 game hours at the end) |
| N001_Lot49 | stuck | autonomy | Dirk#367 in "Usar" since tick 62489 (37.3 game hours at the end) |
| N001_Lot68 | pinned | autonomy | Angela#11 fun at -100 for more than 6 game hours |
| N001_Lot68 | pinned | autonomy | Angela#11 hygiene at -100 for more than 6 game hours |
| N001_Lot68 | pinned | autonomy | Daniel#116 bladder at -100 for more than 6 game hours |
| N001_Lot68 | pinned | autonomy | Daniel#116 comfort at -100 for more than 6 game hours |
| N001_Lot68 | pinned | autonomy | Daniel#116 energy at -100 for more than 6 game hours |
| N001_Lot68 | pinned | autonomy | Daniel#116 fun at -100 for more than 6 game hours |
| N001_Lot68 | pinned | autonomy | Daniel#116 hunger at -100 for more than 6 game hours |
| N001_Lot68 | pinned | autonomy | Daniel#116 hygiene at -100 for more than 6 game hours |
| N001_Lot68 | pinned | autonomy | Daniel#116 social at -100 for more than 6 game hours |
| N001_Lot68 | pinned | autonomy | Lilith#114 fun at -100 for more than 6 game hours |
| N001_Lot68 | pinned | autonomy | Lilith#114 social at -100 for more than 6 game hours |
| N001_Lot68 | pinned | autonomy | Mary-Sue#37 bladder at -100 for more than 6 game hours |
| N001_Lot68 | pinned | autonomy | Mary-Sue#37 comfort at -100 for more than 6 game hours |
| N001_Lot68 | pinned | autonomy | Mary-Sue#37 energy at -100 for more than 6 game hours |
| N001_Lot68 | pinned | autonomy | Mary-Sue#37 fun at -100 for more than 6 game hours |
| N001_Lot68 | pinned | autonomy | Mary-Sue#37 hunger at -100 for more than 6 game hours |
| N001_Lot68 | pinned | autonomy | Mary-Sue#37 hygiene at -100 for more than 6 game hours |
| N001_Lot68 | pinned | autonomy | Mary-Sue#37 social at -100 for more than 6 game hours |
| N001_Lot68 | stuck | autonomy | Lilith#114 in "Dormir" for 6.5 game hours |
| N002_Lot10 | pinned | autonomy | Johnny#389 fun at -100 for more than 6 game hours |
| N002_Lot10 | stuck | autonomy | Jenny#386 in "Dormir" for 7.0 game hours |
| N002_Lot10 | stuck | autonomy | Jill#454 in "Levando a criança" since tick 105680 (13.3 game hours at the end) |
| N002_Lot2 | died | autonomy | Olive#119 in "Falecer" since tick 94729 (19.4 game hours at the end) |
| N002_Lot2 | pinned | autonomy | Ophelia#230 fun at -100 for more than 6 game hours |
| N002_Lot22 | died | autonomy | Buck#151 in "Falecer" since tick 91539 (21.1 game hours at the end) |
| N002_Lot22 | died | autonomy | General in "Falecer" since tick 91294 (21.3 game hours at the end) |
| N002_Lot22 | died | autonomy | Ripp#149 in "Falecer" since tick 101203 (15.8 game hours at the end) |
| N002_Lot22 | died | autonomy | Tank#144 in "Falecer" since tick 92954 (20.4 game hours at the end) |
| N002_Lot22 | pinned | autonomy | Ripp#149 comfort at -100 for more than 6 game hours |
| N002_Lot22 | pinned | autonomy | Ripp#149 fun at -100 for more than 6 game hours |
| N002_Lot22 | pinned | autonomy | Ripp#149 social at -100 for more than 6 game hours |
| N002_Lot22 | pinned | autonomy | Tank#144 fun at -100 for more than 6 game hours |
| N002_Lot22 | pinned | autonomy | Tank#144 social at -100 for more than 6 game hours |
| N002_Lot22 | budget | autonomy | 13 times |
| N002_Lot3 | died | autonomy | Loki#130 in "Falecer" since tick 118792 (6.0 game hours at the end) |
| N002_Lot3 | pinned | autonomy | Circe#147 fun at -100 for more than 6 game hours |
| N002_Lot3 | pinned | autonomy | Circe#147 social at -100 for more than 6 game hours |
| N002_Lot3 | pinned | autonomy | Nervous#182 hygiene at -100 for more than 6 game hours |
| N002_Lot32 | error | autonomy | exit 1: error: no elevation array (3ARY 0x01): not a lot package? |
| N002_Lot32 | error | lot-save | exit 1: error: no elevation array (3ARY 0x01): not a lot package? |
| N002_Lot4 | died | autonomy | Lazlo#137 in "Falecer" since tick 116884 (7.1 game hours at the end) |
| N002_Lot4 | died | autonomy | Vidcund#152 in "Falecer" since tick 75043 (30.3 game hours at the end) |
| N002_Lot4 | pinned | autonomy | Lazlo#137 comfort at -100 for more than 6 game hours |
| N002_Lot4 | pinned | autonomy | Lazlo#137 energy at -100 for more than 6 game hours |
| N002_Lot4 | pinned | autonomy | Lazlo#137 fun at -100 for more than 6 game hours |
| N002_Lot4 | pinned | autonomy | Lazlo#137 hygiene at -100 for more than 6 game hours |
| N002_Lot4 | pinned | autonomy | Lazlo#137 social at -100 for more than 6 game hours |
| N002_Lot4 | pinned | autonomy | Vidcund#152 comfort at -100 for more than 6 game hours |
| N002_Lot4 | pinned | autonomy | Vidcund#152 hygiene at -100 for more than 6 game hours |
| N002_Lot4 | pinned | autonomy | Vidcund#152 social at -100 for more than 6 game hours |
| N002_Lot4 | stuck | autonomy | Pascal#142 in "Dar à Luz" since tick 8366 (67.4 game hours at the end) |
| N003_Lot192 | died | autonomy | Bottom#1 in "Falecer" since tick 101904 (15.4 game hours at the end) |
| N003_Lot192 | died | autonomy | Oberon#39 in "Falecer" since tick 107959 (12.0 game hours at the end) |
| N003_Lot192 | died | autonomy | Puck#49 in "Falecer" since tick 100122 (16.4 game hours at the end) |
| N003_Lot192 | died | autonomy | Titania#12 in "Falecer" since tick 104392 (14.0 game hours at the end) |
| N003_Lot192 | pinned | autonomy | Oberon#39 comfort at -100 for more than 6 game hours |
| N003_Lot192 | pinned | autonomy | Oberon#39 fun at -100 for more than 6 game hours |
| N003_Lot192 | pinned | autonomy | Oberon#39 hygiene at -100 for more than 6 game hours |
| N003_Lot192 | pinned | autonomy | Oberon#39 social at -100 for more than 6 game hours |
| N003_Lot192 | pinned | autonomy | Puck#49 fun at -100 for more than 6 game hours |
| N003_Lot192 | pinned | autonomy | Titania#12 hygiene at -100 for more than 6 game hours |
| N003_Lot192 | stuck | autonomy | Bottom#1 in "Dormir" for 6.7 game hours |
| N003_Lot192 | stuck | autonomy | Oberon#39 in "Dormir" for 7.9 game hours |
| N003_Lot192 | stuck | autonomy | Titania#12 in "Dormir" for 8.1 game hours |
| N003_Lot46 | died | autonomy | Isabella#137 in "Falecer" since tick 34421 (52.9 game hours at the end) |
| N003_Lot46 | died | autonomy | Mercutio#381 in "Falecer" since tick 32666 (53.9 game hours at the end) |
| N003_Lot46 | died | autonomy | Patrizio#135 in "Falecer" since tick 45107 (46.9 game hours at the end) |
| N003_Lot46 | died | autonomy | Romeo#402 in "Falecer" since tick 31006 (54.8 game hours at the end) |
| N003_Lot46 | pinned | autonomy | Patrizio#135 hygiene at -100 for more than 6 game hours |
| N003_Lot46 | pinned | autonomy | Patrizio#135 social at -100 for more than 6 game hours |
| N003_Lot52 | pinned | autonomy | Consort#136 fun at -100 for more than 6 game hours |
| N003_Lot52 | pinned | autonomy | Consort#136 social at -100 for more than 6 game hours |
| N003_Lot52 | pinned | autonomy | Hermia#158 fun at -100 for more than 6 game hours |
| N003_Lot52 | pinned | autonomy | Hermia#158 hygiene at -100 for more than 6 game hours |
| N003_Lot52 | pinned | autonomy | Juliette#140 comfort at -100 for more than 6 game hours |
| N003_Lot52 | pinned | autonomy | Juliette#140 fun at -100 for more than 6 game hours |
| N003_Lot52 | pinned | autonomy | Tybalt#198 fun at -100 for more than 6 game hours |
| N003_Lot52 | pinned | autonomy | Tybalt#198 hygiene at -100 for more than 6 game hours |
| N003_Lot52 | stuck | autonomy | Juliette#140 in "Dormir" for 10.0 game hours |

## Stacks of crashes


## Timing (seconds, per lot: autonomy / lot-save / app)

- N001_Lot11: 23 / 1 / 
- N001_Lot16: 7 / 1 / 
- N001_Lot18: 12 / 1 / 
- N001_Lot22: 5 / 1 / 
- N001_Lot24: 5 / 1 / 
- N001_Lot32: 5 / 0 / 
- N001_Lot36: 8 / 1 / 
- N001_Lot38: 5 / 0 / 
- N001_Lot40: 3 / 0 / 
- N001_Lot47: 4 / 1 / 
- N001_Lot49: 9 / 1 / 
- N001_Lot51: 3 / 0 / 
- N001_Lot53: 4 / 0 / 
- N001_Lot55: 6 / 1 / 
- N001_Lot57: 5 / 0 / 
- N001_Lot59: 6 / 0 / 
- N001_Lot64: 3 / 1 / 
- N001_Lot66: 4 / 1 / 
- N001_Lot68: 18 / 1 / 
- N001_Lot70: 3 / 0 / 
- N001_Lot73: 11 / 0 / 
- N001_Lot75: 5 / 1 / 
- N001_Lot77: 8 / 0 / 
- N001_Lot79: 7 / 1 / 
- N001_Lot80: 1 / 0 / 
- N001_Lot82: 16 / 1 / 
- N002_Lot10: 16 / 1 / 
- N002_Lot2: 9 / 1 / 
- N002_Lot22: 25 / 1 / 
- N002_Lot23: 6 / 1 / 
- N002_Lot25: 7 / 1 / 
- N002_Lot27: 4 / 1 / 
- N002_Lot29: 17 / 1 / 
- N002_Lot3: 27 / 1 / 
- N002_Lot31: 16 / 1 / 
- N002_Lot32: 0 / 0 / 
- N002_Lot4: 22 / 1 / 
- N003_Lot126: 7 / 1 / 
- N003_Lot128: 10 / 0 / 
- N003_Lot133: 8 / 1 / 
- N003_Lot135: 6 / 1 / 
- N003_Lot144: 4 / 1 / 
- N003_Lot149: 4 / 1 / 
- N003_Lot153: 6 / 1 / 
- N003_Lot157: 5 / 1 / 
- N003_Lot159: 5 / 1 / 
- N003_Lot164: 4 / 1 / 
- N003_Lot166: 6 / 1 / 
- N003_Lot168: 7 / 1 / 
- N003_Lot171: 5 / 0 / 
- N003_Lot175: 4 / 1 / 
- N003_Lot179: 12 / 1 / 
- N003_Lot181: 5 / 1 / 
- N003_Lot183: 9 / 0 / 
- N003_Lot187: 1 / 1 / 
- N003_Lot192: 38 / 1 / 
- N003_Lot195: 9 / 0 / 
- N003_Lot45: 4 / 0 / 
- N003_Lot46: 14 / 1 / 
- N003_Lot47: 5 / 1 / 
- N003_Lot48: 4 / 1 / 
- N003_Lot50: 3 / 1 / 
- N003_Lot52: 33 / 1 / 
- Tutorial_Lot1: 2 / 0 / 
- Tutorial_Lot14: 6 / 0 / 
- Tutorial_Lot18: 10 / 1 / 
- Tutorial_Lot19: 6 / 1 / 
- Tutorial_Lot3: 5 / 1 / 
- Tutorial_Lot4: 1 / 1 / 
