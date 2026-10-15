# Inventário do binário: o que já é fiel, o que é inferido e o que falta

Feito em 2026-10-15, sobre o main `44b9280c`. Só documentação: nada no código
mudou. Fontes: o `baserom.sym` (100 mil nomes, o mesmo que o
`syms.txt` já desmanglado), o `ppc_syms.txt` do BV, o nosso `src/`, os
`docs/*-mac.md`, o `TODO.md`, o `docs/TODO-HISTORICO.md` e as issues. Sem
Ghidra: tudo saiu dos nomes e dos textos que já temos.

## 1. Método

**Classes.** Cada símbolo do `baserom.sym` foi desmanglado (`c++filt`) e
cortado em `Classe::método`. Isso dá 2.485 classes. Ficaram de fora:

- bibliotecas e motor genérico: `std`, `__gnu_cxx`, `nGZNumerics`,
  `nGZGraphic4`, `nGZSceneGraph`, `cGZWin*`, `cRZ*`, `cGZDB*`, `cGZLua*`,
  `cHTML*`, `Snd`, D3D, templates;
- rede, compressão, áudio, Lua, depuração, trapaças, testes, miniaturas,
  tutorial e vídeo de TV;
- tudo de expansão, pelo nome (Pet, Season, Business, Vacation, College,
  Apartment, Hobby, Trait, Awning, HalfWall, CurvedPool, Inventory,
  Shopping, Garden, Weather, Rewards, Collection, Album, Web e outros). O
  filtro é por palavra; não temos a lista de classes do base 1.0 (o
  executável base só tem RTTI dentro do Ghidra).

As 816 classes que sobram foram postas num sistema por expressão regular
sobre o nome (`cTSRoute*` → rotas, `nGZSceneDBAnim::*` → animação, e assim
por diante). Métodos `Write`/`Read`/`SaveToStream`/`LoadFromStream` de
qualquer classe contam em "salvar/carregar". Ficam fora construtores,
destrutores, operadores e `QueryInterface`/`AddRef`/`Release`. Sobram
**13.977 métodos relevantes**.

**Casamento.** Um método conta como **nomeado** quando:

1. o par `Classe::método` aparece em `src/` ou nos `docs/` (sem
   `docs/issues/` e `docs/ideias/`, que listam o que *ainda* falta ler); ou
2. o nome do método aparece sozinho em `src/` ou nos `docs/`, tem 8 letras
   ou mais, existe em no máximo 2 classes relevantes e não é genérico
   (`Get…`, `Set…`, `Init`, `Update`, `Read`, ...).

Conferi 40 casamentos da regra 2 sorteados: 38 são a função certa (95%).
"Nomeado em src" é o mesmo teste, só no código.

**Marcadores "inferido".** Linhas com `inferred` ou `inferid` em cada
arquivo de `src/`, com o arquivo posto num sistema pelo caminho (tabela da
§3). Também contei os dos docs. São um piso: muita aproximação não traz a
palavra.

**Fórmulas.**

- **Cobertura de nomes** = métodos nomeados ÷ métodos relevantes do sistema.
- **Índice de fidelidade** = Σ notas ÷ número de linhas da tabela do
  sistema, com **fiel = 1**, **inferido = 0,5** (feito, mas com partes
  deduzidas por nós, aproximadas ou sem conferir) e **faltando = 0**. A
  nota de cada linha veio dos docs, do `TODO.md` e de olhar o código
  (conferi, por exemplo, que `EvalTileForGoal` e `AskBlockerToMove` já
  estão em `src/vm/routing*.cpp`, e que a regra do corte de paredes está em
  `src/formats/lot_model.cpp`).
- **Total** = Σ notas de todas as linhas ÷ todas as linhas.

**O que os números medem e o que não medem.**

- A cobertura de nomes mede **largura do estudo**: quantas funções do jogo
  alguém já leu e citou. Ela é baixa de propósito. O denominador tem
  milhares de getters, funções de editor e caminhos que o jogo base não
  usa. Funções lidas só pelo endereço (`FUN_…`) não contam. Um nome citado
  como "falta ler" no `TODO.md` conta, por engano.
- O índice de fidelidade mede **o que importa para jogar**: as partes que
  se vêem. É um julgamento por linha, não uma medida automática.
- Nenhum dos dois diz que o código está sem bugs. Fiel quer dizer "segue o
  que lemos no binário".

## 2. Resumo

| Sistema | Classes (estudadas) | Cobertura de nomes | Índice de fidelidade | "inferido" em src / docs |
|---|---|---|---|---|
| VM e primitivas | 54 (19) | 233 / 1.610 = 14,5% | 8,5 / 11 = **77%** | 125 / 80 |
| Rotas | 27 (12) | 113 / 622 = 18,2% | 6 / 8 = **75%** | 59 / 38 |
| Animação e IK | 78 (22) | 88 / 1.364 = 6,5% | 4,5 / 9 = **50%** | 39 / 14 |
| Sentar | (usa rotas, slots, IK) | 9 / 34 funções-chave = 26% | 5,5 / 10 = **55%** | 10 / 8 |
| Colocação e construção | 180 (71) | 311 / 2.556 = 12,2% | 7,5 / 11 = **68%** | 63 / 50 |
| Corte de paredes | 11 (4) | 18 / 159 = 11,3% | 3,5 / 6 = **58%** | 4 / (na ui-live §8c) |
| Autonomia | 11 (2) | 64 / 251 = 25,5% | 4 / 5 = **80%** | 9 / (em primitives) |
| Motivos, desejos, memórias, relacionamentos | 21 (7) | 41 / 355 = 11,5% | 3 / 5 = **60%** | 12 / 6 |
| Salvar e carregar | 32 (4) | 78 / 936 = 8,3% | 4,5 / 7 = **64%** | 12 / 2 |
| Bairro e famílias | 64 (29) | 105 / 1.250 = 8,4% | 5 / 7 = **71%** | 40 / 45 |
| CAS | 89 (23) | 87 / 1.534 = 5,7% | 5,5 / 8 = **69%** | 36 / 27 |
| Desenho | 211 (31) | 129 / 2.309 = 5,6% | 4 / 9 = **44%** | 54 / 36 |
| Câmera e UI de jogo | 59 (21) | 90 / 1.031 = 8,7% | 6,5 / 9 = **72%** | 43 / 52 |
| **Total** | **816 (245)** | **1.357 / 13.977 = 9,7%** (só src: 699 = 5,0%) | **68 / 105 = 65%** | **519 em src** |

"Estudadas" = classes com pelo menos um método nomeado. Os 519 marcadores
de src incluem 15 de áudio, fora dos sistemas. Os 4 do corte são 2 do `wall_view` e 2 do `lot_model.cpp`, que também contam no desenho. "Sentar" não tem classes
próprias: a cobertura é das 34 funções que a issue 001 manda ler.

Em uma frase: **a lógica do jogo (VM, autonomia, rotas) está quase toda
lida; o que mais falta é o corpo do Sim (animação, IK, sentar) e o
desenho (telhados, cercas, água, sombras).**

## 3. Tabelas por sistema

Coluna "nomeados" = métodos nomeados / relevantes da classe.

### 3.1 VM e primitivas

Código: `src/vm/vm.*`, `primitives.cpp`, `queue.cpp`, `dialog.cpp`,
`engine*.{hpp,cpp}`, `message.cpp`, `timer.cpp`, `person_prims.cpp`,
`world.*`, `lot.*`, `src/formats/simantics.*`, `bhav.*`,
`src/app/dialog_window*.cpp`, `live_dialogs.cpp`, `lot_vm*.{cpp,mm}`.
Docs: `primitives-mac.md`.

| Classes e funções do jogo | Onde está | Estado | Notas |
|---|---|---|---|
| Despacho e operandos: `cTSObject::OnTryElement`, `cTSBehaviorNode::GetNewPrimitiveByID`, `*Param::SwizzleParams`, `XPrimParam::Read` | `vm/primitives.cpp`, `formats/simantics.*` | fiel | 40 de 40 handlers com nome; largura de todos os campos. |
| Donos de dados e contas: `cTSObject::InterpValue`, `TryExpression`, `TryArrayOperation` | `vm/vm.cpp`, `primitives.cpp` | fiel | Rodada 15. |
| Pilha e chamadas: `cTSTreeSim`, `cTSTreeStack`, `cTSTreeStackElem`, `DoNodeAction`/`Gosub` | `vm/vm.cpp`, `thread_state.*` | fiel | Até 8 argumentos, 1000 quadros, 100000 nós (rodada 16). |
| Primitivas lidas uma a uma: `TrySetToNext`, `TrySnap`, `try_new_reach`, `TryFindFunctionalObject`, `TryDialog`, `TryOnTimer`, `TryChangeMaterial`, `TryCreateObject`, `TryInventoryAction`, `TryGenericSimCall` | `vm/primitives.cpp`, `reach.cpp`, `timer.cpp`, `dialog.cpp` | fiel | Sobram itens "Confirmar" no `TODO.md` (Set to Next 13/33/37, tabelas 0x02/0x04 do Change Material). |
| Fila de interações: `cTSInteraction` (77 métodos, 14 nomeados), `TryPushAction`, `cTSPerson::TryIdleForInput` | `vm/queue.cpp` | inferido | Sub-fila e interações aninhadas (erro 0x6F) faltam; a fila salva é lida por inferência. 11 marcadores. |
| Relógio: `cTSSimulator::SimulateOneTick`, `Simulate` | `vm/vm.cpp` | fiel | 30 ticks por minuto, hora do dia, meia-noite. |
| Erros: `cTSObject::HandleError`, `GetErrorString` | `vm/vm.cpp`, `errors.hpp` | fiel | Falta o reset do object data 7 (`GetDefaultNicenessMultiplier`). |
| Módulo de objetos: `cEdithObjectModule` (120, 26 nomeados): `MakeNewOutOfWorldObject`, `FlushKillQueue`, pontos de entrada (load, cleanup, roomChanged, dynMultiTile) | `vm/vm.cpp`, `kill.cpp`, `app/vm_objects.cpp` | inferido | Os pontos de entrada além de Init/Main não rodam. |
| Dados mantidos pelo motor: object data "room", campos de globais, `cTSGlobals` (58, 2 nomeados), `cTSDataTables` (39, 0) | `vm/world.*` | faltando | "Falta: a sala" está aberto no `TODO.md` desde a rodada 15. |
| Som: `TryPlaySound` | `vm/sound.cpp` | inferido | Tabelas 10000/20000 divergem do doc; flag 0x40 inferida. |
| Atributos de tipo: `cTSObjectTypeAttributeManager` | `vm/vm.cpp` | fiel | Rodada 16. |

### 3.2 Rotas

Código: `src/vm/routing*.cpp`, `path_planner.*`, `locomotion.*`,
`sim_motion.*`, `turn.cpp`, `src/app/door_portals.*`. Docs:
`routing-mac.md`.

| Classes e funções do jogo | Onde está | Estado | Notas |
|---|---|---|---|
| Planejador: `nPathPlanner::cPathPlanner` (48, 14), `cQuadTree`, `cAStarSearch`, `CreateResultPath` | `vm/path_planner.*` | fiel | Célula 1/8 de tile, A* com ruído. |
| Obstáculos: `cTSRouteManager` (70, 12), `WorkOutTileCostAndPassability` | `vm/path_planner.cpp`, `routing_plan.cpp` | inferido | O custo exato de cada `ePassabilityState` falta; tile da parte de multi-tile e caixa sem lista são inferidos. |
| Objetivos: `cTSRoute::EvalTileForGoal`, gerador de pontos | `vm/routing.cpp` | fiel | A tabela da §9 do `routing-mac.md` ainda diz "aproximação": está velha. |
| Máquina do roteador: `cTSRoutingObjectAggregate` (95, 48): pós-rota, goal check, portais, `AskBlockerToMove`/`MoveOutOfWay` | `vm/routing*.cpp` | fiel | O mais estudado da rota. |
| Locomotor: `nTSSG::cLocomotable` (57, 14): `FollowOneStep`, `ComputeTurn`, `CollisionCheck` | `vm/routing_loco.cpp`, `routing_walk.cpp` | inferido | O teste de segmento do `CollisionCheck` não foi lido. 22 marcadores nos dois arquivos. |
| Slots de rota: `cTSRoutingSlot` (58, 3), `cTSSlotDescriptor` (39, 0), `cTSSlotLoader` (19, 0) | `vm/routing.cpp`, `formats/` (SLOT) | inferido | Direções, distâncias e facing do SLOT são inferidos (`TODO.md`). |
| Portais de objeto e escadas: `cPathPlanner::AddDoorPortal`, pares de slots | `vm/routing_plan.cpp` | inferido | Flag `0x200000` e o slot de portal do degrau são inferidos; um Sim não sobe a escada reta do N001_Lot11. |
| Rota salva: `cTSRoute::SaveToStream` | `vm/lot_save.cpp` | fiel | `save-mac.md` §7c. |

### 3.3 Animação e IK

Código: `src/vm/animation.*`, `src/formats/anim*.{hpp,cpp}`,
`scene_anim.*`, `skeleton.*`, `skinning.*`, `src/app/anim_snaps.*`,
`look_at.*`, `cinematic_*`, `lot_actors.cpp`. Docs: `primitives-mac.md`
(Animate, deltas), `cas-mac.md` §17.4b, `cinematics-mac.md`. Não há um
`anim-mac.md`.

| Classes e funções do jogo | Onde está | Estado | Notas |
|---|---|---|---|
| Arquivo ANIM: `cAnimResourceConst::ReadFrom` (67, 7) | `formats/anim.*` | inferido | Preenchimento das strings, unidade das tangentes e ordem de Euler são inferidos (`TODO.md` M3). |
| Canais e deltas: `cActiveAnimSkeletal::SetupChannel`, `SetApplyDeltaDataToIK` | `formats/anim_player.cpp` | fiel | Rodada 15. |
| Mistura por níveis: `cChannelUberOutlet`, `cChannelUber`, `cBlendFunction2` (29, 0) | `formats/anim_player.cpp` | inferido | O peso do delta e o `0x20` do `cChannelUber` não foram lidos. |
| Fila de coreografia: `cChoreoQueue` (81, 0), `cChoreoRequest` (62, 3), `cChoreoHandler*` | — | faltando | Temos um `AnimPlayer` nosso; a fila do jogo nunca foi lida. |
| Locomoção: `cActiveAnimLoco::CreateStepCycle`, `PushTurnBlendCycles`, `StopLocomotion`, `cLocoCycle` (63, 0) | `vm/locomotion.*`, `formats/anim_player.cpp` | inferido | Ciclos de passo lidos; transições e blends da locomoção não. |
| IK: `cActiveAnimIK` (28, 5), `cIKChain` (47, 0), `cIKStrategy7DOF`, `ComputeIKRotationSolution` | `formats/anim_player.cpp` | inferido | O solucionador não foi lido; a mistura de alvos é linear e inferida. |
| Olhar: `cActiveAnimLookAt` (29, 1), `cIKStrategyLookAt` | `app/look_at.*` | inferido | A primitiva Look At é fiel; a cabeça no corpo não (o alvo a 0,75 do chão é inferido). |
| Eventos: `cEventChannel`, `cActiveAnimEvent`, árvores de evento | `vm/animation.cpp` | inferido | As árvores de evento recebem só (evento, objeto IK); 20 marcadores em `animation.cpp`. |
| Cinemáticas: `cCinematicScene::ReadFrom` (23, 9), `TryCinematic` | `formats/cinematic.*`, `app/cinematic_*` | inferido | Spots, velocidade forçada e "ObjectHiding" abertos. |

### 3.4 Sentar

Código: `src/vm/reach.cpp`, `slot_bones.cpp`, `routing.cpp`,
`src/app/seat_module.*`, `seat_check.*`, `vm_objects.cpp`, `lot_vm.cpp`.
Docs: `docs/issues/001-sentar-fiel-ao-binario.md` (plano, ainda aberto).

| Classes e funções do jogo | Onde está | Estado | Notas |
|---|---|---|---|
| Achar a cadeira: `TryFindFunctionalObject` (função 26) | `vm/reach.cpp` | fiel | |
| Snap: `cTSObject::TrySnap` | `vm/reach.cpp` | fiel | |
| A. Objetivos de sentar: `cTSRoute::ScanConeForSittingGoals`, `GetPossibleSittingSlotsFromModel`, `IsPersonSittingOnChairGoal`, `CreateContainmentGoalPoint` | `vm/routing.cpp` | faltando | A rota usa a varredura genérica de slots. |
| B. Snap Into e slot: `TrySnapInto`, `PlaceInSlot`, `AggregateOnPlaceInSlot`, `TestPlaceInSlot` | `vm/placement.cpp`, `reach.cpp` | inferido | Ordem dos passos e slot ocupado inferidos. |
| C. Corpo no slot: `cEdithMTObject::SlotOffsetFromContainer`, `cTSPerson::UpdateSlotDisplacement`, `GetSlotHeight` | `app/seat_module.*` | inferido | O app prende a raiz ao osso; a VM não sabe. |
| D. Salto da raiz: `PostSnap`, `cObject::ApplyAnimationOffsets`, `GlobalRootTransPosition` | `formats/anim_player.*` (`stayInWorld`) | inferido | Object data 108–111 não aplicados. |
| E. Sentado e parado (idles da cadeira, fila vazia) | `app/lot_vm.cpp` | inferido | Regra nossa: segura a pose. |
| F. Animação de entrar/sair (STR# 0x92) | `vm/animation.cpp` | inferido | Escolha da entrada em parte inferida. |
| G. IK das mãos e pés: `cChoreoHandlerIK::HandleChoreoRequest`, `FindIKTargetBone` | `formats/anim_player.cpp` | inferido | Mãos 6–10 cm curtas nos sociais. |
| H. Levantar: `DoesPathStartFromContainment` | `app/seat_module.*` | inferido | Um tick de deslize no `getOut`. |

### 3.5 Colocação e construção

Código: `src/vm/placement.*`, `build_edit*.{hpp,cpp}`, `rooms.*`,
`src/formats/build_catalog.*`, `catalog.*`, `design_catalog.*`,
`lot_edit.*`, `lot_write.*`, `lot_roof.*`, `src/app/buy_mode.*`,
`build_mode*.{hpp,cpp}`, `design_tool.*`. Docs: `buy-build-mac.md`.

| Classes e funções do jogo | Onde está | Estado | Notas |
|---|---|---|---|
| Catálogo: `cTSCatalog` (106, 4), `cTSWinProcCatalog`, `cTSWinProcBuyPanel` | `formats/catalog.*`, `app/buy_mode.cpp` | fiel | Ordem entre preços iguais inferida. |
| Preço, venda, depreciação (`DayPassed`) | `app/buy_mode.cpp`, `vm/` | fiel | |
| Colocação: `cTSUserToolObject` (93, 17), `TestPlacementCommon`, `TestPlacementWithRespectToWalls/Objects`, `AutoRotateObjectToMatchWalls` | `vm/placement.*`, `app/buy_mode.cpp` | inferido | Faltam `HasSupport`/`SupportedUnderneath` e o teste de altura do od 4; 10 marcadores em `buy_mode.cpp`. |
| Desfazer: `cTSUserToolUndoable*`, `cTSUserToolUndoManager` | `app/build_mode*.cpp` | inferido | Lido (§5, §7.7), mas não conferido peça por peça. |
| Paredes: `cTSUserToolWall`, `nTSWorld::cWallGraph`, `cPartitionManager` (92, 13) | `vm/build_edit_walls.cpp` | fiel | |
| Salas: `cRoomManager::FloodRoomRecurse` | `vm/rooms.*` | inferido | O fechamento do cômodo retangular (`FUN_009a7dd8`) não foi lido. |
| Pisos e revestimentos: `cTSUserToolFloorPattern`, `cTSUserToolWallpaper` | `vm/build_edit.cpp` | fiel | |
| Terreno, lagos, piscinas: `cTSUserToolTerrain*`, `cTSUserToolWater`, `cTSUserToolUndoablePool` | `vm/build_edit_ground.cpp` | inferido | "Lido e inferido" (§7.11); 9 marcadores. |
| Escadas modulares: `cTSUserToolModularStairs`, `FindEnd` | `vm/build_edit_stairs.cpp` | inferido | `FindEnd` fiel; o +0,75 de fundação e a "elevation" do degrau não. |
| Telhados: `cTSUserToolRoof` (48, 8), `cAutoRoofData`, `nTSWorld::cWorldRoof` (47, 0) | `formats/lot_roof.cpp`, `vm/build_edit.cpp` | inferido | Retângulos do auto-telhado lidos; o mundo dos telhados não. |
| Portas e janelas: máscaras `_wallmask`, `HideWhenWallDown`, auto-giro | `formats/lot_model.cpp`, `vm/placement.cpp` | inferido | Objetos sem máscara usam um recorte retangular. |

### 3.6 Corte de paredes

Código: `src/app/wall_view.*`, `src/formats/lot_model.cpp` (regra do
corte), `src/app/lot_view.mm`, `live_ui.cpp` (botões). Docs:
`ui-live-mac.md` §8b–8c, `buy-build-mac.md` §7.6.1, issue 002.

| Classes e funções do jogo | Onde está | Estado | Notas |
|---|---|---|---|
| Modos: `cTSMainToolbar::SetWallMode`, `cITSMainToolbar::eWallMode`, `cTSWinProcUCP::onWallChanged` | `app/wall_view.*`, `live_ui.cpp` | fiel | 4 modos; telhado só no modo 3. |
| Regra: `cWallCutawayManager::KeepOnlyFrontFacingOutsideWalls`, `CollectWallsEligibleForCutaway`, `nTSWorld::PartitionStyleSupportsCutaway` | `formats/lot_model.cpp` | inferido | Usamos a direção da vista no lugar da posição da câmera. O `TODO.md` (M4, "Cortada") ainda diz que a regra "não foi lida": está velho. |
| Corte pelo Sim: `cutawayForSelectedSim`, `SetSelectedSimLocation` | `app/lot_view.mm`, `formats/lot_model.cpp` | inferido | Linha no plano, não a consulta de raio do jogo. |
| Memória e alvos: `cutawayMemorySize` (3), `AddTargetLocation`, `Expose`, `SetCinematicsCutaway`, `SelectWallsBlockingPoints` | — | faltando | Sem memória de cômodos; cinemáticas sem corte próprio. |
| Geometria cortada: `nTSSG::cWallManager` (59, 5), `cWallGeometryNode` (60, 1), `cSimplifiedWall` (10, 0) | `formats/lot_model.cpp` | inferido | UV do topo (`wall_top`) é chute. |
| Objetos com a parede baixa: `cTSObject::HideWhenWallDown`, `SuppressHideWhenWallDownFlag` | `app/build_mode*.cpp`, `vm/placement.cpp` | fiel | Issue 002. |

### 3.7 Autonomia

Código: `src/vm/autonomy*.{hpp,cpp}`, `motives.cpp`, `src/app/pie_menu.*`.
Docs: `primitives-mac.md` (0x03, 0x21, 0x1D, menu de pie),
`data-findings.md` §16.

| Classes e funções do jogo | Onde está | Estado | Notas |
|---|---|---|---|
| `cTSPerson::TryFindBestAction`, `FBA_ScoreInteraction`, `FBA_CheckInteractions`, `FBA_RunCheckTree` | `vm/autonomy.cpp` | fiel | O uso da memória de falhas (8 por 9999 ticks) é inferido. |
| `TryFind5WorstMotives`, `Set Motive Change`, laço de `cTSPerson::OnSimulatorTick` | `vm/motives.cpp` | fiel | |
| Menu de pie: `cEdithObjectTestSim::TestInteraction`, `AppendInteractionsForAuto`, `sMakeNewMenuItem` | `app/pie_menu.cpp` | fiel | Rodada 19. |
| Vieses (dono, memória, relação, clima) | `vm/autonomy.cpp` | fiel | No base, só o de dono pesa. |
| Registros de objetos da pessoa: `cTSPerson::cObjectRecord`, `+0x60c` | — | faltando | O `OnCleanup` usa; não modelado. |

`cTSPerson` tem 238 métodos e 63 nomeados: é a classe de jogo mais lida
depois do `cTSObject` (409, 129).

### 3.8 Motivos, desejos, memórias e relacionamentos

Código: `src/vm/motives.*`, `wants.*`, `src/formats/wants.*`,
`src/app/wants_panel.*`. Docs: `wants-mac.md`, `primitives-mac.md`.

| Classes e funções do jogo | Onde está | Estado | Notas |
|---|---|---|---|
| `cTSMotives` (17, 4): `GetMotiveRef`, `GetOldMotiveRef`, `UpdateMood` | `vm/motives.cpp` | fiel | O decaimento vem das árvores. |
| Curvas: `cTSMotiveCurve`, `cTSMotiveCurveSet`, `cTSPiecewiseFunction` (15, 0) | `vm/autonomy.cpp` | inferido | Só `InteractionScoreCurve` foi lida. |
| Desejos e medos: `cTSWantSimulator` (92, 28), `cTSWantNode`, `cTSWantInstance` (41, 0), `cTSWantTreeManager` | `vm/wants.cpp` | inferido | Ordem dos operadores, empates e parâmetros da check tree inferidos. |
| Memórias: `cTSMemoryDefinition`, `cTSMemoryInstance` (18, 0), `cTSMemorySimulator` | `vm/` (tokens do Manage Inventory) | inferido | Só o lado SimAntics existe; o motor das memórias não foi lido. |
| Relacionamentos: `TryNewRelationship` (fiel), `cTSRelationshipTable` (18, 0), `cTSFamilyRelationship` (21, 0), gravador das `SREL` | `vm/primitives.cpp`, `vm/hood_save.cpp` | inferido | O gravador das `SREL` do lote não foi achado (`save-mac.md` §7). |

### 3.9 Salvar e carregar

Código: `src/vm/lot_save.*`, `lot_records.*`, `lot_resume.*`,
`save_types.*`, `hood_save.*`, `family_write.*`,
`src/formats/lot_objects*.cpp`, `character_write.*`, `neighborhood_write.cpp`,
`src/app/game_save.*`. Docs: `save-mac.md`, `game-flow-mac.md` §7.

| Classes e funções do jogo | Onde está | Estado | Notas |
|---|---|---|---|
| XOBJ, XMTO e PERS: `cTSObject::Write/Read`, `cEdithMTObject::Write/Read`, `cTSPerson::Write` | `formats/lot_objects*.cpp`, `vm/lot_save.cpp` | fiel | Layout pela versão do ObjM. |
| ObjM: `cEdithObjectModule` (`read_pre_object`, `SplitLoad`) | `formats/lot_objects.cpp` | fiel | Órfãos do persist DB ignorados, como no jogo. |
| Campos sem nome: token, tabela de relacionamentos, timers, os 6 u32 do XMTO | `vm/lot_records.cpp` | inferido | Lidos como bytes; os nomes faltam. |
| Rota e animação em andamento | `vm/lot_resume.cpp` | fiel | `save-mac.md` §7c. |
| Controladores: `cTSLoadLotController::LoadLot`, `cTSSaveLotController`, `cTSSaveNeighborhoodController` (8, 0) | `app/game_save.*`, `app/lot_loading.mm` | inferido | `LoadLot` lido (rodada 12); o salvar do bairro não. |
| Persist DB: `cTSPersistSystem` (122, 0), `cTSPersistDBRecord`, cache | `formats/dbpf.*` | inferido | Funciona pelo formato DBPF público; a classe do jogo nunca foi lida. |
| Inventários do bairro | — | faltando | Não são salvos (`TODO.md`, Manage Inventory). |

### 3.10 Bairro e famílias

Código: `src/formats/neighborhood*.{hpp,cpp}`, `hood_*.{hpp,cpp}`,
`lot_arrivals.*`, `lot_evict.*`, `src/vm/hood_bridge.*`,
`lot_neighborhood.*`, `src/app/hood_*.{hpp,cpp}`, `main_menu.*`. Docs:
`neighborhood-mac.md`.

| Classes e funções do jogo | Onde está | Estado | Notas |
|---|---|---|---|
| Terreno: `cTSNHoodTerrain` (66, 16), `nTSSG::cNHoodTerrain` (58, 8) | `formats/hood_terrain.*`, `app/hood_view.cpp` | inferido | Mapa alfa, semente das variantes e o `nhoodLightMap` abertos. |
| Ruas, flora, enfeites: `cTSNHoodOccupantManager` (61, 6) | `formats/hood_scene.*` | fiel | Alisar e nivelar ruas (rodadas 13–14). |
| Imposters: `cLotImposterManager` (38, 2), `cLotImposterDisplay` (31, 12) | `formats/hood_lots.cpp` | inferido | O desbotamento pela câmera falta; 7 marcadores. |
| Lotes: `cTSUserToolPlaceLotBase` (35, 16), `cTSUserToolMoveLot`, `cTSLotInfo`, entrar e sair | `app/hood_mode.cpp`, `formats/hood_lots.cpp` | fiel | |
| Famílias e vizinhos: `cTSFamily` (82, 10), `cTSNeighbor` (61, 3), `cTSNeighborManager`, `cTSFamilyManager` | `vm/hood_bridge.cpp`, `formats/neighborhood.*` | inferido | Despejar a família do próprio lote não é tratado; 7 marcadores no `hood_bridge`. |
| Mudar, despejar, valor do lote: `cTSUserToolNeighborhoodMoveIn`, `cTSUserToolEvictFamily`, `GetLotValue` | `formats/lot_evict.*`, `app/hood_*` | inferido | O jogo soma os 3 CDs de videogame ao valor; nós não. |
| Câmera do bairro: `nTSSG::cCameraNeighborhood` | `app/hood_view.cpp` | fiel | |

### 3.11 CAS

Código: `src/app/cas_*.{hpp,cpp}`, `src/formats/cas_*.{hpp,cpp}`,
`sim_character.*`, `sim_censor.*`, `src/vm/genetics.*`,
`baby_character.*`, `appearance.cpp`, `src/app/head_shot.*`,
`dialog_window_clothes.cpp`, `src/render/cas_camera.*`. Docs:
`cas-mac.md`.

| Classes e funções do jogo | Onde está | Estado | Notas |
|---|---|---|---|
| Telas: `cTSUISimEditor` (105, 8), `nTSUICAS::cTSUICASComponent*` | `app/cas_mode*.cpp` | inferido | Falta a biografia da família. |
| Corpo e roupas: `nTSCASCore::cTSSimOutfitData` (133, 12), `cTSCASBodyPartSystem` (141, 7), `cTSBinCatalog` | `formats/cas_catalog.*`, `app/cas_mode_panels.cpp` | inferido | O corpo gordo sai igual ao magro; "random clothing" (`MatchHairStyle`…) aberto. |
| Rosto: arquétipos, modificadores, LxNR, `BlendDeltas` | `formats/cas_face.*` | inferido | A mistura exata não foi lida. |
| Pele, maquiagem, overlays: `cTSSkinTextureCompositor` (27, 0) | `formats/cas_face.*`, `sim_character.*` | inferido | O compositor do jogo não foi lido. |
| Genética: `nTSGenetics`, `cTSSimDNA` | `vm/genetics.*` | fiel | |
| Personalidade, signo, aspiração | `formats/cas_personality.*` | fiel | Tabelas não conferidas no base 1.0. |
| Gravar o personagem | `formats/character_write.*` | fiel | |
| Câmera do CAS: `cCameraCreateASimController` | `render/cas_camera.*` | inferido | Pares spec ↔ localizador, rotação do zoom e roda inferidos. |

### 3.12 Desenho

Código: `src/render/*`, `src/formats/lot_model.*`, `lot_lightmap.*`,
`model.*`, `gmdc.*`, `txmt.*`, `effects.*`, `lighting_config.*`,
`src/app/lot_lighting.*`, `lot_effects.*`, `plumbob_effects.mm`. Docs:
`shaders-mac.md`, `lighting-mac.md`, `effects-mac.md`.

| Classes e funções do jogo | Onde está | Estado | Notas |
|---|---|---|---|
| Materiais e shaders (caminho DX8 dos `.matShad`) | `render/shaders.metal`, `renderer.cpp` | inferido | Muito lido; faltam a fórmula do normal map, o passe de recorte e o CelShaded. |
| Luz e light maps: `cLightingManager` (88, 25), `cLightMapManager`, `cDiffusion*` | `formats/lot_lightmap.*`, `app/lot_lighting.*` | inferido | Lista longa de inferidos no `TODO.md` M4 (sala de fora, portais, paredes sem grade). |
| Sombras: `nTSSG::cShadow` (32, 7), height map do sol | `formats/lot_lightmap.cpp`, `render/` | inferido | Faltam objetos e árvores (`AddBlobToHeightMap`, "LM_sunHM"). |
| Paredes e pisos: `cWallGeometryNode`, `cFloorGeometryNode` (32, 0), `cFloorManager` (31, 0) | `formats/lot_model.cpp` | inferido | UV dos pisos e do topo das paredes são chutes. |
| Telhados e cercas: `cFenceGeometryNode` (57, 0), `cFenceManager` (41, 0), classes de telhado | `formats/lot_roof.cpp`, `lot_model.cpp` | inferido | Pagodes seguem o OpenTS2; cônico, cúpula e octogonal não são desenhados. |
| Água e piscina: `cWaterGeometryBuilder` (35, 0), `cPoolGeometryNode` | `formats/lot_model.cpp` | inferido | Cor, alfa e reflexo são chutes; falta o bump animado. |
| Efeitos: `cTSEffectsManager` (55, 1), `cTSParticlesEffect`, `cTSMetaParticlesEffect` | `render/particle_system.*`, `app/lot_effects.*` | inferido | 7 marcadores no sistema de partículas. |
| Objetos e Sims: `nTSSG::cObject` (140, 20), `nTSSG::cPerson` (50, 5), censura | `app/vm_objects.cpp`, `formats/sim_censor.*` | inferido | Objetos animados escolhidos pela malha de repouso. |
| Espelhos: `MirrorReflectionMaterial`, alvo `ScreenReflection` | `render/` | faltando | Só um espelho reflete por quadro, sem o stencil do jogo. |

### 3.13 Câmera e UI de jogo

Código: `src/render/camera.*`, `src/app/camera_*.{hpp,cpp}`,
`live_ui.*`, `live_panels.*`, `options_panel.*`, `game_cursors.*`,
`game_flow.*`, `pie_menu.*`, `src/formats/camera_config.*`, `ui_*.{hpp,cpp}`.
Docs: `camera-ui-mac.md`, `ui-live-mac.md`, `game-flow-mac.md`.

| Classes e funções do jogo | Onde está | Estado | Notas |
|---|---|---|---|
| Câmera: `cTSCameraSmoothSims1Controller` (58, 9), `cCameraLive` | `render/camera.*`, `app/camera_*` | inferido | Faltam seguir o Sim com a zona morta, Tab, fade de objetos e vista de cima (T). |
| Menu de pie: `cTSWinPieMenu` (36, 5) | `app/pie_menu.*` | fiel | |
| Velocidade, relógio, dinheiro: `cTSMainToolbar` (89, 10), `cTSWinProcLivePanel`, `cTSSimulator::Simulate` | `app/live_ui.cpp` | fiel | |
| Painéis: motivos, relacionamentos, emprego, desejos (`cTSWinProcPanel*`, `cTSWinProcMotiveBar`) | `app/live_panels.*`, `wants_panel.*` | fiel | |
| Painel de memórias: `cTSWinProcPanelMemories` (21, 0) | — | faltando | |
| Fila de ações: `cTSUIActionQueue`, `cTSWinActionQueueIcon` | `app/live_ui.cpp` | fiel | |
| Clique e cursores: `cTSWinSceneGraph::ObjectAtGameWindowPoint` | `app/game_cursors.*`, `lot_view.mm` | fiel | |
| Fluxo do jogo: `cTSGameStateController` (65, 14), `cTSStartupController` | `app/game_flow.*`, `scene_*.mm` | inferido | O caso Construir do `ApplySavedUIState` não foi achado. |
| Diálogos: `cTSUIDialogManager`, `cTSObjectDialog` | `app/dialog_window*.cpp`, `vm/dialog.cpp` | inferido | 27 marcadores somados. |

## 4. Achados de passagem

- O `TODO.md` (M4, "Lote (telhados, cercas, água, modos de parede)") diz que
  a regra do `cWallCutawayManager` "não foi lida". Ela foi lida na rodada 11
  (`ui-live-mac.md` §8c) e já está em `lot_model.cpp`.
- A tabela da §9 do `routing-mac.md` (main `3b4c4db`) diz "aproximação" para
  `EvalTileForGoal` e `MoveOutOfWay`. Os dois já estão no código.
- As classes mais lidas: `cTSObject` (129 de 409), `cTSPerson` (63 de 238),
  `cTSRoutingObjectAggregate` (48 de 95). As grandes nunca abertas:
  `cChoreoQueue` (81), `cTSPersistSystem` (122), `cFenceGeometryNode` (57),
  `cIKChain` (47), `nTSWorld::cWorldRoof` (47), `cTSSlotDescriptor` (39).

## 5. Plano de rodadas (só plano, nada lançado)

Ordem por impacto: o que mais se vê, ou mais quebra, ao jogar o jogo base.
"Opus" = leitura do binário; "Sonnet" = frentes simples, sem binário.
Ghidra: pedir ao usuário para abrir antes; senão, headless e só leitura.

| # | Rodada | Por que vem antes | Agentes | Ghidra |
|---|---|---|---|---|
| 1 | **Sentar** (issue 001, linhas A–H) | Sentar aparece em toda refeição, sofá e privada; 7 de 10 partes inferidas. | 3 Opus (1 leitura + 2 código) | sim |
| 2 | **Corpo do Sim: coreografia, mistura e IK** (`cChoreoQueue`, `cChannelUber*`, `cActiveAnimIK`, `cIKChain`, transições de locomoção) | Todo Sim, todo quadro. Junta com a parte G do sentar se rodar junto. | 2 Opus (1 leitura + 1 código) | sim |
| 3 | **Rotas: slots e escadas** (`cTSRoutingSlot`, `cTSSlotDescriptor`, `CollisionCheck`, portal do degrau) | Sims presos (a escada do N001_Lot11) quebram o jogo. | 1 Opus | sim |
| 4 | **VM: o que o motor mantém** (object data "room", pontos de entrada load/cleanup/roomChanged, sub-fila e aninhadas) | Sims travados no soak; afeta luzes e salas. | 1 Opus | sim |
| 5 | **Corte de paredes: memória e alvos** (`cutawayMemorySize`, `AddTargetLocation`, `Expose`, posição da câmera, raio do Sim) | Visível em todo lote; rodada pequena. | 1 Opus | sim |
| 6 | **Desenho: telhados, cercas e água** (`cWorldRoof`, `cFenceGeometryNode`, `cWaterGeometryBuilder`) e sombras de objetos | Muito visível, mas a regra é "bom o bastante". | 1 Opus (geometria) + 1 Sonnet (água e espelho, sem binário) | Opus sim |
| 7 | **Construção: suporte, telhado e terreno** (`HasSupport`, `SupportedUnderneath`, leveler, `cTSUserToolRoof`, piscinas) | Erros de colocação no modo Construção. | 1 Opus | sim |
| 8 | **Relacionamentos e memórias** (`cTSRelationshipTable`, gravador das `SREL`, `cTSMemory*`) + painel de memórias | Afeta histórias e o painel; não quebra o jogo. | 1 Opus + 1 Sonnet (painel) | Opus sim |
| 9 | **Salvar: campos sem nome** (token, timers, XMTO) e inventários do bairro | Hoje o save já volta igual; é limpeza. | 1 Opus | sim |
| 10 | **CAS: corpo gordo, `BlendDeltas`, roupa aleatória** | O corpo gordo é um bug visível no CAS. | 1 Opus | sim |
| 11 | **Câmera: seguir o Sim, Tab, fade, vista de cima** | Conforto; os valores já estão nos dados. | 1 Sonnet | não |
| 12 | **Bairro: imposters e terreno** (desbotar, mapa alfa, `nhoodLightMap`) | Pouco visível. | 1 Opus | sim |

Total: 14 agentes Opus e 3 Sonnet, em 12 rodadas. As rodadas 1 e 2 tocam
os mesmos arquivos (`anim_player.cpp`, `seat_module.*`): rodar em sequência,
não em paralelo.
