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


Generated by `tools/soak.sh` on 2026-10-11: 69 lots, autonomy for 129600 ticks (72 game hours, 1800 ticks each), lot-save of 300 ticks with --check, app run of 600 VM ticks. Hang limit 900s (app 300s).

## Counts by category

| Category | Lots | Findings |
|---|---|---|
| crash | 0 | 0 |
| hang | 0 | 0 |
| error | 1 | 2 |
| dead | 2 | 2 |
| stuck | 8 | 22 |
| died | 8 | 23 |
| pinned | 14 | 165 |
| roundtrip | 0 | 0 |
| instruction budget (20000) hit | 0 | 0 |

## Findings per lot

| Lot | Category | Where | Detail |
|---|---|---|---|
| N001_Lot11 | pinned | autonomy | Mortimer#197 social at -100 for more than 6 game hours |
| N001_Lot11 | stuck | autonomy | Alexander#142 in "Dormir" for 7.0 game hours |
| N001_Lot11 | stuck | autonomy | Alexander#142 in "Dormir" for 9.2 game hours |
| N001_Lot11 | stuck | autonomy | Alexander#142 in "Dormir" for 9.7 game hours |
| N001_Lot11 | stuck | autonomy | Alexander#142 in "Tocar" for 9.7 game hours |
| N001_Lot11 | stuck | autonomy | Alexander#142 in "Tocar" since tick 118131 (6.4 game hours at the end) |
| N001_Lot11 | stuck | autonomy | Mortimer#197 in "Tocar" for 7.7 game hours |
| N001_Lot16 | pinned | autonomy | Don#197 comfort at -100 for more than 6 game hours |
| N001_Lot16 | pinned | autonomy | Don#197 fun at -100 for more than 6 game hours |
| N001_Lot16 | pinned | autonomy | Don#197 hygiene at -100 for more than 6 game hours |
| N001_Lot16 | pinned | autonomy | Don#197 social at -100 for more than 6 game hours |
| N001_Lot18 | pinned | autonomy | Dina#271 hygiene at -100 for more than 6 game hours |
| N001_Lot18 | pinned | autonomy | Dina#271 social at -100 for more than 6 game hours |
| N001_Lot18 | pinned | autonomy | Nina#210 hygiene at -100 for more than 6 game hours |
| N001_Lot36 | pinned | autonomy | Beau#270 fun at -100 for more than 6 game hours |
| N001_Lot36 | pinned | autonomy | Beau#270 hygiene at -100 for more than 6 game hours |
| N001_Lot36 | pinned | autonomy | Brandi#2 fun at -100 for more than 6 game hours |
| N001_Lot36 | pinned | autonomy | Brandi#2 hygiene at -100 for more than 6 game hours |
| N001_Lot36 | pinned | autonomy | Dustin#248 fun at -100 for more than 6 game hours |
| N001_Lot36 | stuck | autonomy | Brandi#2 in "Dar à Luz" since tick 104616 (13.9 game hours at the end) |
| N001_Lot36 | stuck | autonomy | Brandi#2 in "Dormir" for 11.4 game hours |
| N001_Lot49 | pinned | autonomy | Darren#347 fun at -100 for more than 6 game hours |
| N001_Lot49 | pinned | autonomy | Dirk#367 fun at -100 for more than 6 game hours |
| N001_Lot49 | stuck | autonomy | Darren#347 in "Jogar" for 7.2 game hours |
| N001_Lot49 | stuck | autonomy | Dirk#367 in "Dormir" for 7.1 game hours |
| N001_Lot68 | died | autonomy | Angela#11 in "Falecer" since tick 34116 (53.0 game hours at the end) |
| N001_Lot68 | died | autonomy | Daniel#116 in "Falecer" since tick 41739 (48.8 game hours at the end) |
| N001_Lot68 | died | autonomy | Lilith#114 in "Falecer" since tick 48590 (45.0 game hours at the end) |
| N001_Lot68 | died | autonomy | Mary-Sue#37 in "Falecer" since tick 35937 (52.0 game hours at the end) |
| N001_Lot68 | pinned | autonomy | Angela#11 bladder at -100 for more than 6 game hours |
| N001_Lot68 | pinned | autonomy | Angela#11 comfort at -100 for more than 6 game hours |
| N001_Lot68 | pinned | autonomy | Angela#11 energy at -100 for more than 6 game hours |
| N001_Lot68 | pinned | autonomy | Angela#11 fun at -100 for more than 6 game hours |
| N001_Lot68 | pinned | autonomy | Angela#11 hunger at -100 for more than 6 game hours |
| N001_Lot68 | pinned | autonomy | Angela#11 hygiene at -100 for more than 6 game hours |
| N001_Lot68 | pinned | autonomy | Angela#11 social at -100 for more than 6 game hours |
| N001_Lot68 | pinned | autonomy | Daniel#116 bladder at -100 for more than 6 game hours |
| N001_Lot68 | pinned | autonomy | Daniel#116 comfort at -100 for more than 6 game hours |
| N001_Lot68 | pinned | autonomy | Daniel#116 energy at -100 for more than 6 game hours |
| N001_Lot68 | pinned | autonomy | Daniel#116 fun at -100 for more than 6 game hours |
| N001_Lot68 | pinned | autonomy | Daniel#116 hunger at -100 for more than 6 game hours |
| N001_Lot68 | pinned | autonomy | Daniel#116 hygiene at -100 for more than 6 game hours |
| N001_Lot68 | pinned | autonomy | Daniel#116 social at -100 for more than 6 game hours |
| N001_Lot68 | pinned | autonomy | Lilith#114 bladder at -100 for more than 6 game hours |
| N001_Lot68 | pinned | autonomy | Lilith#114 comfort at -100 for more than 6 game hours |
| N001_Lot68 | pinned | autonomy | Lilith#114 energy at -100 for more than 6 game hours |
| N001_Lot68 | pinned | autonomy | Lilith#114 fun at -100 for more than 6 game hours |
| N001_Lot68 | pinned | autonomy | Lilith#114 hunger at -100 for more than 6 game hours |
| N001_Lot68 | pinned | autonomy | Lilith#114 hygiene at -100 for more than 6 game hours |
| N001_Lot68 | pinned | autonomy | Lilith#114 social at -100 for more than 6 game hours |
| N001_Lot68 | pinned | autonomy | Mary-Sue#37 bladder at -100 for more than 6 game hours |
| N001_Lot68 | pinned | autonomy | Mary-Sue#37 comfort at -100 for more than 6 game hours |
| N001_Lot68 | pinned | autonomy | Mary-Sue#37 energy at -100 for more than 6 game hours |
| N001_Lot68 | pinned | autonomy | Mary-Sue#37 fun at -100 for more than 6 game hours |
| N001_Lot68 | pinned | autonomy | Mary-Sue#37 hunger at -100 for more than 6 game hours |
| N001_Lot68 | pinned | autonomy | Mary-Sue#37 hygiene at -100 for more than 6 game hours |
| N001_Lot68 | pinned | autonomy | Mary-Sue#37 social at -100 for more than 6 game hours |
| N002_Lot10 | pinned | autonomy | Johnny#389 hygiene at -100 for more than 6 game hours |
| N002_Lot10 | stuck | autonomy | Jenny#386 in "Dormir" for 7.0 game hours |
| N002_Lot10 | stuck | autonomy | Jenny#386 in "Dormir" for 7.5 game hours |
| N002_Lot10 | stuck | autonomy | Jill#454 in "Dormir" for 11.0 game hours |
| N002_Lot10 | stuck | autonomy | Johnny#389 in "Dormir" for 6.8 game hours |
| N002_Lot10 | stuck | autonomy | Pollination in "Jogar" for 8.4 game hours |
| N002_Lot2 | dead | autonomy | Ophelia#230: Missing neighbor for data access. (Force Recalc of Wants: no neighbor 0) |
| N002_Lot2 | died | autonomy | Olive#119 in "Falecer" since tick 85633 (24.4 game hours at the end) |
| N002_Lot2 | pinned | autonomy | Olive#119 bladder at -100 for more than 6 game hours |
| N002_Lot2 | pinned | autonomy | Olive#119 comfort at -100 for more than 6 game hours |
| N002_Lot2 | pinned | autonomy | Olive#119 social at -100 for more than 6 game hours |
| N002_Lot2 | pinned | autonomy | Ophelia#230 bladder at -100 for more than 6 game hours |
| N002_Lot2 | pinned | autonomy | Ophelia#230 energy at -100 for more than 6 game hours |
| N002_Lot2 | pinned | autonomy | Ophelia#230 hunger at -100 for more than 6 game hours |
| N002_Lot2 | pinned | autonomy | Ophelia#230 social at -100 for more than 6 game hours |
| N002_Lot2 | stuck | autonomy | Ophelia#230 in "Dormir" for 10.0 game hours |
| N002_Lot2 | stuck | autonomy | Ophelia#230 in "Transição de Idade" since tick 87474 (23.4 game hours at the end) |
| N002_Lot22 | died | autonomy | General in "Falecer" since tick 55038 (41.4 game hours at the end) |
| N002_Lot22 | died | autonomy | Ripp#149 in "Falecer" since tick 98458 (17.3 game hours at the end) |
| N002_Lot22 | died | autonomy | Tank#144 in "Falecer" since tick 69465 (33.4 game hours at the end) |
| N002_Lot22 | pinned | autonomy | Ripp#149 bladder at -100 for more than 6 game hours |
| N002_Lot22 | pinned | autonomy | Ripp#149 comfort at -100 for more than 6 game hours |
| N002_Lot22 | pinned | autonomy | Ripp#149 fun at -100 for more than 6 game hours |
| N002_Lot22 | pinned | autonomy | Ripp#149 hunger at -100 for more than 6 game hours |
| N002_Lot22 | pinned | autonomy | Ripp#149 hygiene at -100 for more than 6 game hours |
| N002_Lot22 | pinned | autonomy | Tank#144 bladder at -100 for more than 6 game hours |
| N002_Lot22 | pinned | autonomy | Tank#144 energy at -100 for more than 6 game hours |
| N002_Lot22 | pinned | autonomy | Tank#144 fun at -100 for more than 6 game hours |
| N002_Lot22 | pinned | autonomy | Tank#144 hunger at -100 for more than 6 game hours |
| N002_Lot22 | pinned | autonomy | Tank#144 hygiene at -100 for more than 6 game hours |
| N002_Lot22 | pinned | autonomy | Tank#144 social at -100 for more than 6 game hours |
| N002_Lot22 | stuck | autonomy | Buck#151 in "Levando a criança" since tick 109800 (11.0 game hours at the end) |
| N002_Lot22 | stuck | autonomy | Ripp#149 in "Levar Cutucão" for 7.2 game hours |
| N002_Lot3 | dead | autonomy | Circe#147: Could not find object reference (text notification without a stack object) |
| N002_Lot3 | pinned | autonomy | Circe#147 bladder at -100 for more than 6 game hours |
| N002_Lot3 | pinned | autonomy | Circe#147 comfort at -100 for more than 6 game hours |
| N002_Lot3 | pinned | autonomy | Circe#147 energy at -100 for more than 6 game hours |
| N002_Lot3 | pinned | autonomy | Circe#147 fun at -100 for more than 6 game hours |
| N002_Lot3 | pinned | autonomy | Circe#147 hunger at -100 for more than 6 game hours |
| N002_Lot3 | pinned | autonomy | Circe#147 hygiene at -100 for more than 6 game hours |
| N002_Lot3 | pinned | autonomy | Circe#147 social at -100 for more than 6 game hours |
| N002_Lot3 | stuck | autonomy | Circe#147 in "Atender" since tick 14898 (63.7 game hours at the end) |
| N002_Lot32 | error | autonomy | exit 1: error: no elevation array (3ARY 0x01): not a lot package? |
| N002_Lot32 | error | lot-save | exit 1: error: no elevation array (3ARY 0x01): not a lot package? |
| N002_Lot4 | died | autonomy | Lazlo#137 in "Falecer" since tick 88055 (23.1 game hours at the end) |
| N002_Lot4 | died | autonomy | Vidcund#152 in "Falecer" since tick 93589 (20.0 game hours at the end) |
| N002_Lot4 | pinned | autonomy | Lazlo#137 bladder at -100 for more than 6 game hours |
| N002_Lot4 | pinned | autonomy | Lazlo#137 comfort at -100 for more than 6 game hours |
| N002_Lot4 | pinned | autonomy | Lazlo#137 energy at -100 for more than 6 game hours |
| N002_Lot4 | pinned | autonomy | Lazlo#137 fun at -100 for more than 6 game hours |
| N002_Lot4 | pinned | autonomy | Lazlo#137 hunger at -100 for more than 6 game hours |
| N002_Lot4 | pinned | autonomy | Lazlo#137 hygiene at -100 for more than 6 game hours |
| N002_Lot4 | pinned | autonomy | Lazlo#137 social at -100 for more than 6 game hours |
| N002_Lot4 | pinned | autonomy | Vidcund#152 bladder at -100 for more than 6 game hours |
| N002_Lot4 | pinned | autonomy | Vidcund#152 comfort at -100 for more than 6 game hours |
| N002_Lot4 | pinned | autonomy | Vidcund#152 energy at -100 for more than 6 game hours |
| N002_Lot4 | pinned | autonomy | Vidcund#152 fun at -100 for more than 6 game hours |
| N002_Lot4 | pinned | autonomy | Vidcund#152 hunger at -100 for more than 6 game hours |
| N002_Lot4 | pinned | autonomy | Vidcund#152 hygiene at -100 for more than 6 game hours |
| N002_Lot4 | stuck | autonomy | Pascal#142 in "Dar à Luz" since tick 8405 (67.3 game hours at the end) |
| N003_Lot192 | died | autonomy | Bottom#1 in "Falecer" since tick 27572 (56.7 game hours at the end) |
| N003_Lot192 | died | autonomy | Oberon#39 in "Falecer" since tick 30871 (54.8 game hours at the end) |
| N003_Lot192 | died | autonomy | Puck#49 in "Falecer" since tick 25684 (57.7 game hours at the end) |
| N003_Lot192 | died | autonomy | Titania#12 in "Falecer" since tick 51485 (43.4 game hours at the end) |
| N003_Lot192 | pinned | autonomy | Bottom#1 bladder at -100 for more than 6 game hours |
| N003_Lot192 | pinned | autonomy | Bottom#1 comfort at -100 for more than 6 game hours |
| N003_Lot192 | pinned | autonomy | Bottom#1 energy at -100 for more than 6 game hours |
| N003_Lot192 | pinned | autonomy | Bottom#1 fun at -100 for more than 6 game hours |
| N003_Lot192 | pinned | autonomy | Bottom#1 hunger at -100 for more than 6 game hours |
| N003_Lot192 | pinned | autonomy | Bottom#1 hygiene at -100 for more than 6 game hours |
| N003_Lot192 | pinned | autonomy | Bottom#1 mood at -100 for more than 6 game hours |
| N003_Lot192 | pinned | autonomy | Bottom#1 social at -100 for more than 6 game hours |
| N003_Lot192 | pinned | autonomy | Oberon#39 bladder at -100 for more than 6 game hours |
| N003_Lot192 | pinned | autonomy | Oberon#39 comfort at -100 for more than 6 game hours |
| N003_Lot192 | pinned | autonomy | Oberon#39 energy at -100 for more than 6 game hours |
| N003_Lot192 | pinned | autonomy | Oberon#39 fun at -100 for more than 6 game hours |
| N003_Lot192 | pinned | autonomy | Oberon#39 hunger at -100 for more than 6 game hours |
| N003_Lot192 | pinned | autonomy | Oberon#39 hygiene at -100 for more than 6 game hours |
| N003_Lot192 | pinned | autonomy | Oberon#39 social at -100 for more than 6 game hours |
| N003_Lot192 | pinned | autonomy | Puck#49 bladder at -100 for more than 6 game hours |
| N003_Lot192 | pinned | autonomy | Puck#49 comfort at -100 for more than 6 game hours |
| N003_Lot192 | pinned | autonomy | Puck#49 energy at -100 for more than 6 game hours |
| N003_Lot192 | pinned | autonomy | Puck#49 fun at -100 for more than 6 game hours |
| N003_Lot192 | pinned | autonomy | Puck#49 hunger at -100 for more than 6 game hours |
| N003_Lot192 | pinned | autonomy | Puck#49 hygiene at -100 for more than 6 game hours |
| N003_Lot192 | pinned | autonomy | Puck#49 social at -100 for more than 6 game hours |
| N003_Lot192 | pinned | autonomy | Titania#12 bladder at -100 for more than 6 game hours |
| N003_Lot192 | pinned | autonomy | Titania#12 comfort at -100 for more than 6 game hours |
| N003_Lot192 | pinned | autonomy | Titania#12 energy at -100 for more than 6 game hours |
| N003_Lot192 | pinned | autonomy | Titania#12 fun at -100 for more than 6 game hours |
| N003_Lot192 | pinned | autonomy | Titania#12 hunger at -100 for more than 6 game hours |
| N003_Lot192 | pinned | autonomy | Titania#12 hygiene at -100 for more than 6 game hours |
| N003_Lot192 | pinned | autonomy | Titania#12 social at -100 for more than 6 game hours |
| N003_Lot46 | died | autonomy | Isabella#137 in "Falecer" since tick 26099 (57.5 game hours at the end) |
| N003_Lot46 | died | autonomy | Mercutio#381 in "Falecer" since tick 20299 (60.7 game hours at the end) |
| N003_Lot46 | died | autonomy | Patrizio#135 in "Falecer" since tick 21753 (59.9 game hours at the end) |
| N003_Lot46 | died | autonomy | Romeo#402 in "Falecer" since tick 31411 (54.5 game hours at the end) |
| N003_Lot46 | pinned | autonomy | Isabella#137 bladder at -100 for more than 6 game hours |
| N003_Lot46 | pinned | autonomy | Isabella#137 comfort at -100 for more than 6 game hours |
| N003_Lot46 | pinned | autonomy | Isabella#137 energy at -100 for more than 6 game hours |
| N003_Lot46 | pinned | autonomy | Isabella#137 fun at -100 for more than 6 game hours |
| N003_Lot46 | pinned | autonomy | Isabella#137 hunger at -100 for more than 6 game hours |
| N003_Lot46 | pinned | autonomy | Isabella#137 hygiene at -100 for more than 6 game hours |
| N003_Lot46 | pinned | autonomy | Isabella#137 social at -100 for more than 6 game hours |
| N003_Lot46 | pinned | autonomy | Mercutio#381 bladder at -100 for more than 6 game hours |
| N003_Lot46 | pinned | autonomy | Mercutio#381 comfort at -100 for more than 6 game hours |
| N003_Lot46 | pinned | autonomy | Mercutio#381 energy at -100 for more than 6 game hours |
| N003_Lot46 | pinned | autonomy | Mercutio#381 fun at -100 for more than 6 game hours |
| N003_Lot46 | pinned | autonomy | Mercutio#381 hunger at -100 for more than 6 game hours |
| N003_Lot46 | pinned | autonomy | Mercutio#381 hygiene at -100 for more than 6 game hours |
| N003_Lot46 | pinned | autonomy | Mercutio#381 social at -100 for more than 6 game hours |
| N003_Lot46 | pinned | autonomy | Patrizio#135 bladder at -100 for more than 6 game hours |
| N003_Lot46 | pinned | autonomy | Patrizio#135 comfort at -100 for more than 6 game hours |
| N003_Lot46 | pinned | autonomy | Patrizio#135 energy at -100 for more than 6 game hours |
| N003_Lot46 | pinned | autonomy | Patrizio#135 fun at -100 for more than 6 game hours |
| N003_Lot46 | pinned | autonomy | Patrizio#135 hunger at -100 for more than 6 game hours |
| N003_Lot46 | pinned | autonomy | Patrizio#135 hygiene at -100 for more than 6 game hours |
| N003_Lot46 | pinned | autonomy | Patrizio#135 social at -100 for more than 6 game hours |
| N003_Lot46 | pinned | autonomy | Romeo#402 bladder at -100 for more than 6 game hours |
| N003_Lot46 | pinned | autonomy | Romeo#402 comfort at -100 for more than 6 game hours |
| N003_Lot46 | pinned | autonomy | Romeo#402 energy at -100 for more than 6 game hours |
| N003_Lot46 | pinned | autonomy | Romeo#402 fun at -100 for more than 6 game hours |
| N003_Lot46 | pinned | autonomy | Romeo#402 hygiene at -100 for more than 6 game hours |
| N003_Lot46 | pinned | autonomy | Romeo#402 social at -100 for more than 6 game hours |
| N003_Lot52 | died | autonomy | Consort#136 in "Falecer" since tick 80468 (27.3 game hours at the end) |
| N003_Lot52 | died | autonomy | Hermia#158 in "Falecer" since tick 14655 (63.9 game hours at the end) |
| N003_Lot52 | died | autonomy | Juliette#140 in "Falecer" since tick 72220 (31.9 game hours at the end) |
| N003_Lot52 | died | autonomy | Tybalt#198 in "Falecer" since tick 53558 (42.2 game hours at the end) |
| N003_Lot52 | pinned | autonomy | Consort#136 bladder at -100 for more than 6 game hours |
| N003_Lot52 | pinned | autonomy | Consort#136 comfort at -100 for more than 6 game hours |
| N003_Lot52 | pinned | autonomy | Consort#136 energy at -100 for more than 6 game hours |
| N003_Lot52 | pinned | autonomy | Consort#136 fun at -100 for more than 6 game hours |
| N003_Lot52 | pinned | autonomy | Consort#136 hunger at -100 for more than 6 game hours |
| N003_Lot52 | pinned | autonomy | Consort#136 social at -100 for more than 6 game hours |
| N003_Lot52 | pinned | autonomy | Hermia#158 bladder at -100 for more than 6 game hours |
| N003_Lot52 | pinned | autonomy | Hermia#158 comfort at -100 for more than 6 game hours |
| N003_Lot52 | pinned | autonomy | Hermia#158 energy at -100 for more than 6 game hours |
| N003_Lot52 | pinned | autonomy | Hermia#158 fun at -100 for more than 6 game hours |
| N003_Lot52 | pinned | autonomy | Hermia#158 hunger at -100 for more than 6 game hours |
| N003_Lot52 | pinned | autonomy | Hermia#158 hygiene at -100 for more than 6 game hours |
| N003_Lot52 | pinned | autonomy | Hermia#158 social at -100 for more than 6 game hours |
| N003_Lot52 | pinned | autonomy | Juliette#140 bladder at -100 for more than 6 game hours |
| N003_Lot52 | pinned | autonomy | Juliette#140 comfort at -100 for more than 6 game hours |
| N003_Lot52 | pinned | autonomy | Juliette#140 energy at -100 for more than 6 game hours |
| N003_Lot52 | pinned | autonomy | Juliette#140 fun at -100 for more than 6 game hours |
| N003_Lot52 | pinned | autonomy | Juliette#140 hunger at -100 for more than 6 game hours |
| N003_Lot52 | pinned | autonomy | Juliette#140 hygiene at -100 for more than 6 game hours |
| N003_Lot52 | pinned | autonomy | Juliette#140 social at -100 for more than 6 game hours |
| N003_Lot52 | pinned | autonomy | Tybalt#198 bladder at -100 for more than 6 game hours |
| N003_Lot52 | pinned | autonomy | Tybalt#198 comfort at -100 for more than 6 game hours |
| N003_Lot52 | pinned | autonomy | Tybalt#198 energy at -100 for more than 6 game hours |
| N003_Lot52 | pinned | autonomy | Tybalt#198 fun at -100 for more than 6 game hours |
| N003_Lot52 | pinned | autonomy | Tybalt#198 hunger at -100 for more than 6 game hours |
| N003_Lot52 | pinned | autonomy | Tybalt#198 hygiene at -100 for more than 6 game hours |
| N003_Lot52 | pinned | autonomy | Tybalt#198 social at -100 for more than 6 game hours |
| Tutorial_Lot18 | died | autonomy | Tutorial in "Falecer" since tick 111311 (10.2 game hours at the end) |

## Stacks of crashes


## Timing (seconds, per lot: autonomy / lot-save / app)

- N001_Lot11: 35 / 2 / 
- N001_Lot16: 21 / 1 / 
- N001_Lot18: 28 / 2 / 
- N001_Lot22: 10 / 1 / 
- N001_Lot24: 10 / 1 / 
- N001_Lot32: 11 / 1 / 
- N001_Lot36: 23 / 2 / 
- N001_Lot38: 11 / 2 / 
- N001_Lot40: 9 / 1 / 
- N001_Lot47: 17 / 2 / 
- N001_Lot49: 32 / 2 / 
- N001_Lot51: 11 / 1 / 
- N001_Lot53: 12 / 1 / 
- N001_Lot55: 17 / 1 / 
- N001_Lot57: 14 / 2 / 
- N001_Lot59: 28 / 1 / 
- N001_Lot64: 12 / 2 / 
- N001_Lot66: 19 / 2 / 
- N001_Lot68: 42 / 2 / 
- N001_Lot70: 11 / 1 / 
- N001_Lot73: 40 / 2 / 
- N001_Lot75: 25 / 2 / 
- N001_Lot77: 42 / 2 / 
- N001_Lot79: 33 / 2 / 
- N001_Lot80: 6 / 2 / 
- N001_Lot82: 74 / 2 / 
- N002_Lot10: 76 / 2 / 
- N002_Lot2: 26 / 2 / 
- N002_Lot22: 57 / 2 / 
- N002_Lot23: 12 / 1 / 
- N002_Lot25: 15 / 1 / 
- N002_Lot27: 8 / 1 / 
- N002_Lot29: 47 / 2 / 
- N002_Lot3: 52 / 2 / 
- N002_Lot31: 37 / 1 / 
- N002_Lot32: 0 / 1 / 
- N002_Lot4: 61 / 1 / 
- N003_Lot126: 11 / 1 / 
- N003_Lot128: 16 / 1 / 
- N003_Lot133: 12 / 1 / 
- N003_Lot135: 10 / 1 / 
- N003_Lot144: 8 / 1 / 
- N003_Lot149: 8 / 1 / 
- N003_Lot153: 9 / 0 / 
- N003_Lot157: 7 / 1 / 
- N003_Lot159: 7 / 0 / 
- N003_Lot164: 5 / 1 / 
- N003_Lot166: 7 / 1 / 
- N003_Lot168: 8 / 0 / 
- N003_Lot171: 5 / 0 / 
- N003_Lot175: 4 / 1 / 
- N003_Lot179: 12 / 0 / 
- N003_Lot181: 6 / 0 / 
- N003_Lot183: 9 / 1 / 
- N003_Lot187: 4 / 0 / 
- N003_Lot192: 49 / 1 / 
- N003_Lot195: 9 / 1 / 
- N003_Lot45: 4 / 1 / 
- N003_Lot46: 30 / 2 / 
- N003_Lot47: 7 / 1 / 
- N003_Lot48: 9 / 0 / 
- N003_Lot50: 5 / 1 / 
- N003_Lot52: 29 / 1 / 
- Tutorial_Lot1: 2 / 1 / 
- Tutorial_Lot14: 6 / 1 / 
- Tutorial_Lot18: 14 / 1 / 
- Tutorial_Lot19: 8 / 1 / 
- Tutorial_Lot3: 5 / 0 / 
- Tutorial_Lot4: 2 / 1 / 
