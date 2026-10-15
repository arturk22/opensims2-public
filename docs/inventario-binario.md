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
| VM e primitivas | 54 (19) | 233 / 1.610 = 14,5% | 9,5 / 11 = **86%** | 125 / 80 |
| Rotas | 27 (13) | 113 / 622 = 18,2% | 7 / 8 = **88%** | 59 / 38 |
| Animação e IK | 78 (22) | 88 / 1.364 = 6,5% | 6 / 9 = **67%** | 39 / 14 |
| Sentar | (usa rotas, slots, IK) | 22 / 34 funções-chave = 65% | 7,5 / 10 = **75%** | 10 / 8 |
| Colocação e construção | 180 (71) | 311 / 2.556 = 12,2% | 7,5 / 11 = **68%** | 63 / 50 |
| Corte de paredes | 11 (5) | 32 / 159 = 20,1% | 5,5 / 6 = **92%** | 4 / (na ui-live §8c–8d) |
| Autonomia | 11 (2) | 64 / 251 = 25,5% | 4 / 5 = **80%** | 9 / (em primitives) |
| Motivos, desejos, memórias, relacionamentos | 21 (7) | 41 / 355 = 11,5% | 3 / 5 = **60%** | 12 / 6 |
| Salvar e carregar | 32 (4) | 78 / 936 = 8,3% | 4,5 / 7 = **64%** | 12 / 2 |
| Bairro e famílias | 64 (29) | 105 / 1.250 = 8,4% | 5 / 7 = **71%** | 40 / 45 |
| CAS | 89 (23) | 87 / 1.534 = 5,7% | 5,5 / 8 = **69%** | 36 / 27 |
| Desenho | 211 (31) | 129 / 2.309 = 5,6% | 4 / 9 = **44%** | 54 / 36 |
| Câmera e UI de jogo | 59 (21) | 90 / 1.031 = 8,7% | 6,5 / 9 = **72%** | 43 / 52 |
| **Total** | **816 (246)** | **1.371 / 13.977 = 9,8%** (só src: 699 = 5,0%) | **73 / 105 = 70%** | **519 em src** |

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
| Fila de interações: `cTSInteraction` (77 métodos, 14 nomeados), `TryPushAction`, `cTSPerson::TryIdleForInput`, `SetCurrentAction` | `vm/queue.cpp`, `links.cpp` | inferido | Sub-fila e aninhadas (erro 0x6F) feitas; `SetCurrentAction` (dados de pessoa 39/71/73/110, sub-ações) também no Gosub Found Action (rodada n04). A fila salva é lida por inferência; falta o dado de pessoa 164. |
| Relógio: `cTSSimulator::SimulateOneTick`, `Simulate` | `vm/vm.cpp` | fiel | 30 ticks por minuto, hora do dia, meia-noite. |
| Erros: `cTSObject::HandleError`, `GetErrorString` | `vm/vm.cpp`, `errors.hpp` | fiel | Falta o reset do object data 7 (`GetDefaultNicenessMultiplier`). |
| Módulo de objetos: `cEdithObjectModule` (120, 30 nomeados): `MakeNewOutOfWorldObject`, `FlushKillQueue`, `PostLoad`, `UpdateObjectsInRoom`, `UpdateObjectsAdjacentToWall`, pontos de entrada (load, cleanup, roomChanged, wallAdjacency) | `vm/vm.cpp`, `kill.cpp`, `lot_resume.cpp`, `rooms.cpp`, `placement.cpp`, `build_edit.cpp` | fiel | Rodada n04 (`primitives-mac.md` §4i): load em todo objeto carregado, roomChanged pelo `OnRoomChanged`, wallAdjacency pelo `UpdateWallAdjacency`. dynMultiTile (entrada 8): nenhum OBJf do jogo base a tem. |
| Dados mantidos pelo motor: object data "room" (`SetLocation`/`SetRoom`), od 5 (wall adjacency), campos de globais, `cTSGlobals` (58, 2 nomeados), `cTSDataTables` (39, 0) | `vm/world.*`, `rooms.cpp`, `placement.cpp` | inferido | Sala e od 5 pelo binário (rodada n04; a sala é a do tile, o binário usa o quarto de tile do ponto). `cTSGlobals` e `cTSDataTables` não lidos. |
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
| Locomotor: `nTSSG::cLocomotable` (57, 14): `FollowOneStep`, `ComputeTurn`, `CollisionCheck` | `vm/routing_loco.cpp`, `routing_walk.cpp` | inferido | Rodada n03: o `CollisionCheck` e as exclusões do `EstimateWhenPathWillBeBlocked` lidos (quem se afasta sai da varredura, feito); as exceções do alvo da rota e de "já sobrepostos" não estão no binário e seguem inferidas (`routing-mac.md` §6.1). |
| Slots de rota: `cTSRoutingSlot` (58, 3), `cTSSlotDescriptor` (39, 0), `cTSSlotLoader` (19, 0) | `vm/routing.cpp`, `formats/` (SLOT) | inferido | Rodada n03: `Init`, `Read`/`Write` do descritor e os bits das RS flags (par `0x200000`, aleatório `0x2000`, parado `0x40000`, paredes `0x80000`, continuação `0x400000`/`0x800000`) lidos; o nome de osso exige `0x20000` (`routing-mac.md` §8.6); o +20 "parado" feito. Falta: a escala 1/16 do `Read` por versão e o +25 das paredes. |
| Portais de objeto e escadas: `cPathPlanner::AddDoorPortal`, pares de slots | `vm/routing_plan.cpp` | fiel | Rodada n03: escadas pelos slots das peças, duas faixas de mão única (sobe/desce), flag `0x200000` e mão única conferidas (`routing-mac.md` §8.6). Falta: retângulos de custo junto das pontas da escada. |
| Rota salva: `cTSRoute::SaveToStream` | `vm/lot_save.cpp` | fiel | `save-mac.md` §7c. |

### 3.3 Animação e IK

Código: `src/vm/animation.*`, `src/formats/anim*.{hpp,cpp}`,
`scene_anim.*`, `skeleton.*`, `skinning.*`, `src/app/anim_snaps.*`,
`look_at.*`, `cinematic_*`, `lot_actors.cpp`. Docs: `anim-mac.md` (fila de
coreografia, expiração, peso dos canais, solucionador de IK; rodada n02),
`primitives-mac.md` (Animate, deltas), `cas-mac.md` §17.4b, `cinematics-mac.md`.

| Classes e funções do jogo | Onde está | Estado | Notas |
|---|---|---|---|
| Arquivo ANIM: `cAnimResourceConst::ReadFrom` (67, 7) | `formats/anim.*` | inferido | Preenchimento das strings, unidade das tangentes e ordem de Euler são inferidos (`TODO.md` M3). |
| Canais e deltas: `cActiveAnimSkeletal::SetupChannel`, `SetApplyDeltaDataToIK` | `formats/anim_player.cpp` | fiel | Rodada 15. |
| Mistura por níveis: `cChannelUberOutlet`, `cChannelUber`, `cBlendFunction2` (29, 0) | `formats/anim_player.cpp` | inferido | Rodada n02: o peso do canal (`cChannelUber::Weight`: peso do pedido × peso de IK) e a memória do osso (osso sem canal fica como estava) lidos e feitos (`anim-mac.md` §2–3); a forma dos intervalos de blend (`cBlendInterval::Value`) não foi lida. |
| Fila de coreografia: `cChoreoQueue` (81, 0), `cChoreoRequest` (62, 3), `cChoreoHandler*` | `formats/anim_player.cpp` | inferido | Rodada n02: ordem do quadro (pedidos → tratadores → outlets, IK no segundo passo), regras de expiração e a saída dos expirados lidas (`anim-mac.md` §1–2); o `AnimPlayer` segue isso, mas a sobreposição no fim de um pedido (nível −1) continua nossa. |
| Locomoção: `cActiveAnimLoco::CreateStepCycle`, `PushTurnBlendCycles`, `StopLocomotion`, `cLocoCycle` (63, 0) | `vm/locomotion.*`, `formats/anim_player.cpp` | inferido | Ciclos de passo lidos; transições e blends da locomoção não. |
| IK: `cActiveAnimIK` (28, 5), `cIKChain` (47, 0), `cIKStrategy7DOF`, `ComputeIKRotationSolution` | `formats/anim.cpp` (`applyIk`) | fiel | Rodada n02: o solucionador de duas juntas do binário (dobradiça em X, giro pelo polo, 65 %/75 % da torção) e a mistura de alvos pelo contato conferida (`anim-mac.md` §4–5). Sobras: ordem de Euler da torção, contato 0. |
| Olhar: `cActiveAnimLookAt` (29, 1), `cIKStrategyLookAt` | `app/look_at.*` | inferido | A primitiva Look At é fiel; a cabeça no corpo não (o alvo a 0,75 do chão é inferido). |
| Eventos: `cEventChannel`, `cActiveAnimEvent`, árvores de evento | `vm/animation.cpp` | inferido | As árvores de evento recebem só (evento, objeto IK); 20 marcadores em `animation.cpp`. |
| Cinemáticas: `cCinematicScene::ReadFrom` (23, 9), `TryCinematic` | `formats/cinematic.*`, `app/cinematic_*` | inferido | Spots, velocidade forçada e "ObjectHiding" abertos. |

### 3.4 Sentar

Código: `src/vm/reach.cpp`, `slot_bones.cpp`, `routing.cpp`,
`src/app/seat_module.*`, `seat_check.*`, `vm_objects.cpp`, `lot_vm.cpp`.
Docs: `docs/seat-mac.md` (leitura do binário, rodada n01), `docs/issues/001-sentar-fiel-ao-binario.md` (parcialmente feita).

| Classes e funções do jogo | Onde está | Estado | Notas |
|---|---|---|---|
| Achar a cadeira: `TryFindFunctionalObject` (função 26) | `vm/reach.cpp` | fiel | |
| Snap: `cTSObject::TrySnap` | `vm/reach.cpp` | fiel | |
| A. Objetivos de sentar: `cTSRoute::ScanConeForSittingGoals` (BV `0x4788cc`), `GetPossibleSittingSlotsFromModel`, `IsPersonSittingOnChairGoal`, `CreateContainmentGoalPoint` | `vm/routing.cpp`, `routing_walk.cpp` | fiel | Cadeira na pós-rota; o objetivo de contenção não roda no jogo base. |
| B. Snap Into e slot: `TrySnapInto`, `PlaceInSlot`, `AggregateOnPlaceInSlot`, `TestPlaceInSlot` | `vm/placement.cpp`, `routing_walk.cpp` | fiel | Falta a árvore 0x34 "allow in slot". |
| C. Corpo no slot: `cObject::OnPlaceInSlot`, `ObjectSlotOffset`, `cTSPerson::UpdateSlotDisplacement` | `app/seat_module.*` | fiel | O grafo de cena prende ao osso do slot, como o app; a VM só tem a posição do contêiner. |
| D. Salto da raiz: `PostSnap`, `cObject::ApplyAnimationOffsets`, `GlobalRootTransPosition` | `formats/anim_player.*` (`stayInWorld`) | inferido | Os od 108–111 nunca são escritos pelas árvores (zero); o `stayInWorld` segue inferido (rodada 2). |
| E. Sentado e parado (idles da cadeira, fila vazia) | `app/lot_vm.cpp`, `formats/anim_player.cpp` | fiel | Rodada n02: o motor não tem idle; osso sem canal fica como estava (`anim-mac.md` §6). A idle em pé do app segue nossa. |
| F. Animação de entrar/sair (STR# 0x92) | `vm/reach.cpp` | inferido | Ordem das listas fiel (só a superfície); a faixa pela altura em metros falta. |
| G. IK das mãos e pés: `cChoreoHandlerIK::HandleChoreoRequest`, `FindIKTargetBone` | `formats/anim.cpp` | fiel | Rodada n02: o solucionador do binário (`anim-mac.md` §4). |
| H. Levantar: `TrySnap`, `RemoveFromWorld`, `DoesPathStartFromContainment` | `vm/reach.cpp`, `app/seat_module.*` | inferido | Direção fiel; o ponto único do Snap (sem o anel nosso) falta. |

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
`ui-live-mac.md` §8b–8d, `buy-build-mac.md` §7.6.1, issue 002.

| Classes e funções do jogo | Onde está | Estado | Notas |
|---|---|---|---|
| Modos: `cTSMainToolbar::SetWallMode`, `cITSMainToolbar::eWallMode`, `cTSWinProcUCP::onWallChanged` | `app/wall_view.*`, `live_ui.cpp` | fiel | 4 modos; telhado só no modo 3. |
| Regra: `cWallCutawayManager::KeepOnlyFrontFacingOutsideWalls`, `CollectWallsEligibleForCutaway`, `nTSWorld::PartitionStyleSupportsCutaway`, `UpdateSceneGraph` | `formats/lot_model.cpp` (`lotCutawayWalls`), `app/lot_view.mm` | fiel | Rodada n05: pela posição da câmera; refeito só quando o conjunto cortado muda (`ui-live-mac.md` §8d). Inferido: o mapa de cômodos das faces (o de luz). |
| Corte pelo Sim: `cutawayForSelectedSim`, `SetSelectedSimLocation`, `CutawayLocationChanged`, `nTSWorld::cWallGraph::WallsBetweenPointSetAndPoint` | `app/lot_view.mm`, `formats/lot_model.cpp` | fiel | Rodada n05: raio do Sim até 3,0 de altura, passo de tiles e regra das arestas do jogo (`lotWallsBetweenPointsAndCamera`). |
| Memória e alvos: `cutawayMemorySize` (3), `AddTargetLocation`, `Expose`, `SetCinematicsCutaway`, `SelectWallsBlockingPoints`, `SelectRoomBlockers`, `SelectNearSemicircleBlockers` | `formats/lot_model.cpp`, `app/cinematic_host.cpp` | fiel | Rodada n05. Inferidos: os vértices do meio círculo, o cômodo de um ponto, o flag +0x194 do Start (desligado) e quando a memória zera (troca de lote). |
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
`src/app/lot_lighting.*`, `lot_effects.*`, `plumbob_effects.mm`, `lot_water.*`. Docs:
`shaders-mac.md`, `lighting-mac.md`, `effects-mac.md`, `roof-fence-water-mac.md`.

| Classes e funções do jogo | Onde está | Estado | Notas |
|---|---|---|---|
| Materiais e shaders (caminho DX8 dos `.matShad`) | `render/shaders.metal`, `renderer.cpp` | inferido | Muito lido; faltam a fórmula do normal map, o passe de recorte e o CelShaded. |
| Luz e light maps: `cLightingManager` (88, 25), `cLightMapManager`, `cDiffusion*` | `formats/lot_lightmap.*`, `app/lot_lighting.*` | inferido | Lista longa de inferidos no `TODO.md` M4 (sala de fora, portais, paredes sem grade). |
| Sombras: `nTSSG::cShadow` (32, 7), height map do sol | `formats/lot_lightmap.cpp`, `render/` | inferido | Rodada n06: os planos GUOB dos objetos lidos (`UpdateGUOBs`, `EnableGUOB`) e conferidos no lote; falta recalcular os de parede ao mover objetos e o "LM_sunHM". |
| Paredes e pisos: `cWallGeometryNode`, `cFloorGeometryNode` (32, 0), `cFloorManager` (31, 0) | `formats/lot_model.cpp` | inferido | UV dos pisos e do topo das paredes são chutes. |
| Telhados e cercas: `cFenceGeometryNode` (57, 0), `cFenceManager` (41, 0), classes de telhado | `formats/lot_roof.cpp`, `lot_model.cpp` | inferido | Rodada n06 (`roof-fence-water-mac.md` §1–2): grade, alturas, camadas e triângulos do jogo lidos; espessura, beiral, meia água e mansarda corrigidos; a triangulação e o atlas ainda são do OpenTS2. Trilho e poste das cercas conferidos. |
| Água e piscina: `cWaterGeometryBuilder` (35, 0), `cPoolGeometryNode` | `formats/lot_water.*`, `lot_model.cpp` | inferido | Rodada n06 (§3): alfa por profundidade (`Terrain.ini`), só o reflexo, leito e praia pintados como o binário; piscina como o caminho DX8 do Mac (ondas, reflexo, camadas de fundo); falta a passada de bump e as cáusticas. |
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
| 1 | **Sentar** (issue 001, linhas A–H) — **feita** (n01): objetivos de sentar, Snap Into/PlaceInSlot, direção do Snap e lista 0x92 do binário; E, G, I ficam para a rodada 2 (`docs/seat-mac.md`). | Sentar aparece em toda refeição, sofá e privada; 7 de 10 partes inferidas. | 3 Opus (1 leitura + 2 código) | sim |
| 2 | **Corpo do Sim: coreografia, mistura e IK** (`cChoreoQueue`, `cChannelUber*`, `cActiveAnimIK`, `cIKChain`, transições de locomoção) — **feita** (n02): ordem da fila, expiração, peso dos canais e o solucionador de IK do binário; memória do osso; E, G, I da issue 001 (`docs/anim-mac.md`); transições de locomoção ficam no `TODO.md`. | Todo Sim, todo quadro. Junta com a parte G do sentar se rodar junto. | 2 Opus (1 leitura + 1 código) | sim |
| 3 | **Rotas: slots e escadas** (`cTSRoutingSlot`, `cTSSlotDescriptor`, `CollisionCheck`, portal do degrau) — **feita** (n03): escadas como portais dos slots (duas faixas de mão única, os Sims não se travam no patamar), bits do `cTSRoutingSlot`, `CollisionCheck` e o +20 "parado" do binário (`routing-mac.md` §6.1, §8.2, §8.6). | Sims presos (a escada do N001_Lot11) quebram o jogo. | 1 Opus | sim |
| 4 | **VM: o que o motor mantém** (object data "room", pontos de entrada load/cleanup/roomChanged, sub-fila e aninhadas) — **feita** (n04): `OnRoomChanged` (entrada 7, portas com as duas salas), `UpdateObjectsInRoom`, load em todo objeto carregado (`PostLoad`), `UpdateWallAdjacency` (od 5 e entrada 6, 319/321 bancadas batem), `SetCurrentAction` (`primitives-mac.md` §4i). | Sims travados no soak; afeta luzes e salas. | 1 Opus | sim |
| 5 | **Corte de paredes: memória e alvos** (`cutawayMemorySize`, `AddTargetLocation`, `Expose`, posição da câmera, raio do Sim) — **feita** (n05): posição da câmera, `WallsBetweenPointSetAndPoint` (raio até 3,0 de altura), `Expose` com memória de 3 cômodos e o Start das cinemáticas (`ui-live-mac.md` §8d). | Visível em todo lote; rodada pequena. | 1 Opus | sim |
| 6 | **Desenho: telhados, cercas e água** (`cWorldRoof`, `cFenceGeometryNode`, `cWaterGeometryBuilder`) e sombras de objetos — **feita** (n06): telhados, cercas, água e GUOB lidos (`docs/roof-fence-water-mac.md`); água do terreno como o jogo (alfa por profundidade, leito e praia), parâmetros dos telhados, pinturas antes dos GUOB; piscina com o `RegularWavesHLSL` e as camadas de fundo; a triangulação dos telhados, o bump da água e as cáusticas ficam no `TODO.md`. | Muito visível, mas a regra é "bom o bastante". | 1 Opus (geometria) + 1 Sonnet (água e espelho, sem binário) | Opus sim |
| 7 | **Construção: suporte, telhado e terreno** (`HasSupport`, `SupportedUnderneath`, leveler, `cTSUserToolRoof`, piscinas) | Erros de colocação no modo Construção. | 1 Opus | sim |
| 8 | **Relacionamentos e memórias** (`cTSRelationshipTable`, gravador das `SREL`, `cTSMemory*`) + painel de memórias | Afeta histórias e o painel; não quebra o jogo. | 1 Opus + 1 Sonnet (painel) | Opus sim |
| 9 | **Salvar: campos sem nome** (token, timers, XMTO) e inventários do bairro | Hoje o save já volta igual; é limpeza. | 1 Opus | sim |
| 10 | **CAS: corpo gordo, `BlendDeltas`, roupa aleatória** | O corpo gordo é um bug visível no CAS. | 1 Opus | sim |
| 11 | **Câmera: seguir o Sim, Tab, fade, vista de cima** | Conforto; os valores já estão nos dados. | 1 Sonnet | não |
| 12 | **Bairro: imposters e terreno** (desbotar, mapa alfa, `nhoodLightMap`) | Pouco visível. | 1 Opus | sim |

Total: 14 agentes Opus e 3 Sonnet, em 12 rodadas. As rodadas 1 e 2 tocam
os mesmos arquivos (`anim_player.cpp`, `seat_module.*`): rodar em sequência,
não em paralelo.
