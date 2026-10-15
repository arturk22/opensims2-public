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
| Rotas | 27 (13) | 113 / 622 = 18,2% (não recontado na n18) | 7,5 / 8 = **94%** | 59 / 38 |
| Animação e IK | 78 (22) | 88 / 1.364 = 6,5% (não recontado na n23) | 6,5 / 9 = **72%** | 36 / 14 |
| Sentar | (usa rotas, slots, IK) | 22 / 34 funções-chave = 65% | 8 / 10 = **80%** | 10 / 8 |
| Colocação e construção | 180 (71) | 311 / 2.556 = 12,2% (não recontado na n20) | 9 / 11 = **82%** | 64 / 50 |
| Corte de paredes | 11 (5) | 32 / 159 = 20,1% (não recontado na n19) | 6 / 6 = **100%** | 4 / (na ui-live §8c–8d) |
| Autonomia | 11 (2) | 64 / 251 = 25,5% | 4 / 5 = **80%** | 9 / (em primitives) |
| Motivos, desejos, memórias, relacionamentos | 21 (7) | 41 / 355 = 11,5% (não recontado na n17) | 5 / 5 = **100%** | 3 / 7 |
| Salvar e carregar | 32 (4) | 78 / 936 = 8,3% (não recontado na n09) | 6 / 7 = **86%** | 10 / 2 |
| Bairro e famílias | 64 (29) | 105 / 1.250 = 8,4% (não recontado na n12 nem na n27) | 6 / 7 = **86%** (com a n27) | 41 / 45 |
| CAS | 89 (25) | 87 / 1.534 = 5,7% (não recontado na n10 nem na n21) | 7 / 8 = **88%** | 33 / 28 |
| Desenho | 211 (35) | 129 / 2.309 = 5,6% (não recontado na n13, n16, n19, n22 nem n25) | 9 / 10 = **90%** | 55 / 41 |
| Câmera e UI de jogo | 59 (21) | 90 / 1.031 = 8,7% (não recontado na n11 nem na n27) | 11,5 / 13 = **88%** (com a n27) | 43 / 52 |
| **Total** | **816 (252)** | **1.371 / 13.977 = 9,8%** (só src: 699 = 5,0%) | **92,5 / 110 = 84,1%** (com a n23, a n25 e a n27) | **505 em src** |

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
| Pilha e chamadas: `cTSTreeSim`, `cTSTreeStack`, `cTSTreeStackElem`, `DoNodeAction`/`Gosub` | `vm/vm.cpp`, `thread_state.*` | fiel | Até 8 argumentos, 1000 quadros, 100000 nós (rodada 16; constante única `vm::kNodeLimitPerTurn`, n29). |
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
| Obstáculos: `cTSRouteManager` (70, 12), `WorkOutTileCostAndPassability` | `vm/path_planner.cpp`, `routing_plan.cpp`, `world.cpp` | inferido | Rodada n18 (`routing-mac.md` §9.1): o estado de cada célula (o maior; > 1 bloqueia) conferido, o custo do piso por quarto de tile (`routingcost` do padrão, 2 sem piso) e o corte de quinas do binário feitos. Seguem inferidos: tile da parte de multi-tile, a caixa do modelo sem lista (a flag 4 do nó), o `+10` do `WorldOutsideSupport` e o estado 7. |
| Objetivos: `cTSRoute::EvalTileForGoal`, gerador de pontos | `vm/routing.cpp` | fiel | A tabela da §9 do `routing-mac.md` ainda diz "aproximação": está velha. |
| Máquina do roteador: `cTSRoutingObjectAggregate` (95, 48): pós-rota, goal check, portais, `AskBlockerToMove`/`MoveOutOfWay` | `vm/routing*.cpp` | fiel | O mais estudado da rota. Rodada n18b (`routing-mac.md` §9.2): o goal check quando o `DoAlongRouteCheckTest` diz (antes do passo, na 1ª vez de toda rota, caminho/sala/andar novo ou mais de 1 andado) e forçado no `DoBlocked`; a pilha de rotas (a rota de dentro de um goal check guarda a de fora: o 11 falso dos sociais acabou). |
| Locomotor: `nTSSG::cLocomotable` (57, 14): `FollowOneStep`, `ComputeTurn`, `CollisionCheck` | `vm/routing_loco.cpp`, `routing_walk.cpp` | fiel | Rodada n18: o `CollisionCheck` lido de novo e feito (a primeira colisão no horizonte, só se larga quem já toca e se afasta, parada com clipe só se acaba 2 passos antes, senão `EndFollow`); as duas exceções nossas saíram; o contador do `DoBlockedGoalStillAvailable` guarda o bloqueador (`routing-mac.md` §9.1). Rodada n18b: a trajetória prevista é a do `GlobalLocationEstimate` (o caminho cúbico do quadro, não as retas entre os pontos; §9.2). |
| Slots de rota: `cTSRoutingSlot` (58, 3), `cTSSlotDescriptor` (39, 0), `cTSSlotLoader` (19, 0) | `vm/routing.cpp`, `formats/` (SLOT) | inferido | Rodada n03: `Init`, `Read`/`Write` do descritor e os bits das RS flags (par `0x200000`, aleatório `0x2000`, parado `0x40000`, paredes `0x80000`, continuação `0x400000`/`0x800000`) lidos; o nome de osso exige `0x20000` (`routing-mac.md` §8.6); o +20 "parado" feito; o +25 das paredes lido e feito na rodada n18 (`GetWallBlockFlagsAtTile`, a direção da última origem). Falta: a escala 1/16 do `Read` por versão. |
| Portais de objeto e escadas: `cPathPlanner::AddDoorPortal`, pares de slots | `vm/routing_plan.cpp` | fiel | Rodada n03: escadas pelos slots das peças, duas faixas de mão única (sobe/desce), flag `0x200000` e mão única conferidas (`routing-mac.md` §8.6). Falta: retângulos de custo 10 junto das pontas da escada (lidos na rodada n18, `routing-mac.md` §9.1; não feitos). |
| Rota salva: `cTSRoute::SaveToStream` | `vm/lot_save.cpp` | fiel | `save-mac.md` §7c. |

### 3.3 Animação e IK

Código: `src/vm/animation.*`, `src/formats/anim*.{hpp,cpp}`,
`scene_anim.*`, `skeleton.*`, `skinning.*`, `src/app/anim_snaps.*`,
`look_at.*`, `cinematic_*`, `lot_actors.cpp`. Docs: `anim-mac.md` (fila de
coreografia, expiração, peso dos canais, solucionador de IK; rodada n02),
`primitives-mac.md` (Animate, deltas), `cas-mac.md` §17.4b, `cinematics-mac.md`.

| Classes e funções do jogo | Onde está | Estado | Notas |
|---|---|---|---|
| Arquivo ANIM: `cAnimResourceConst::ReadFrom` (67, 7) | `formats/anim.*` | fiel | Rodada n23 (`anim-mac.md` §9): `ReadFrom`/`WriteTo` lidos (os 11.757 ANIM regravados byte a byte), tangentes `±m·2^(e−31)` por tick, chaves assadas a cada 33,333 ticks, Euler XYZ em graus e as rotações amostradas por quadro com slerp (`cChannelStrategyQ`). |
| Canais e deltas: `cActiveAnimSkeletal::SetupChannel`, `SetApplyDeltaDataToIK` | `formats/anim_player.cpp` | fiel | Rodada 15. |
| Mistura por níveis: `cChannelUberOutlet`, `cChannelUber`, `cBlendFunction2` (29, 0) | `formats/anim_player.cpp` | inferido | Rodada n02: o peso do canal (`cChannelUber::Weight`: peso do pedido × peso de IK) e a memória do osso (osso sem canal fica como estava) lidos e feitos (`anim-mac.md` §2–3); a forma dos intervalos de blend (`cBlendInterval::Value`) não foi lida. |
| Fila de coreografia: `cChoreoQueue` (81, 0), `cChoreoRequest` (62, 3), `cChoreoHandler*` | `formats/anim_player.cpp` | inferido | Rodada n02: ordem do quadro (pedidos → tratadores → outlets, IK no segundo passo), regras de expiração e a saída dos expirados lidas (`anim-mac.md` §1–2); o `AnimPlayer` segue isso, mas a sobreposição no fim de um pedido (nível −1) continua nossa. |
| Locomoção: `cActiveAnimLoco::CreateStepCycle`, `PushTurnBlendCycles`, `StopLocomotion`, `cLocoCycle` (63, 0) | `vm/locomotion.*`, `vm/routing_loco.cpp`, `formats/anim_player.cpp` | inferido | Rodada n14 (`anim-mac.md` §8): relógio das fases (`ComputeLocoStartTicks`, entrega do `FollowOneStep` na saída do pedido, N = 25/70), trocas do `cLocoCycle` (`+0xec`, cosseno), pé da parada (`0x178`), rotas curtas e `StopLocomotion` lidos e feitos; seguem inferidos a janela do fim da primitiva anterior, a troca centrada (±b/2), a rampa de velocidade e o espaço da parada ao bloquear. |
| IK: `cActiveAnimIK` (28, 5), `cIKChain` (47, 0), `cIKStrategy7DOF`, `ComputeIKRotationSolution` | `formats/anim.cpp` (`applyIk`) | fiel | Rodada n02: o solucionador de duas juntas do binário (dobradiça em X, giro pelo polo, 65 %/75 % da torção) e a mistura de alvos pelo contato conferida (`anim-mac.md` §4–5). Sobras: ordem de Euler da torção, contato 0. |
| Olhar: `cActiveAnimLookAt` (29, 1), `cIKStrategyLookAt` | `app/look_at.*` | inferido | Rodada n23 (`anim-mac.md` §11): o alvo a 3/4 da caixa do alvo e as velocidades do primitivo seguem o `try_look_at`; o `cActiveAnimLookAt::AnimUpdate` (cadeia de IK, limites, perfil de velocidade, estados) não foi portado, nem os slots. |
| Eventos: `cEventChannel`, `cActiveAnimEvent`, árvores de evento | `vm/animation.cpp` | inferido | Rodada n23 (`anim-mac.md` §10): os 5 parâmetros do `cCustomEventHandler2::HandleEvents` (evento, stack object, porção %, voltas, abortado), os temps postos de volta e o 9997/9998/9999 também no Animate Sim seguem o binário; inferidos: qual chave do motor é a 6 e a 3, a chave de cada volta de um laço; 16 marcadores em `animation.cpp`. |
| Cinemáticas: `cCinematicScene::ReadFrom` (23, 9), `TryCinematic` | `formats/cinematic.*`, `app/cinematic_*` | inferido | Rodada n23 (`cinematics-mac.md` §8): o andar do ator de origem, o "ObjectHiding" (a opção gráfica, 1 nas cenas da casa inteira) e o espelho pelo temp 3 lidos; o Start não muda a velocidade do jogo (a normal forçada é nossa); spots como pontos e quem começa a cena sem 0x04 abertos. |

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
| H. Levantar: `TrySnap`, `RemoveFromWorld`, `DoesPathStartFromContainment` | `vm/reach.cpp`, `app/seat_module.*` | fiel | Rodada n18: o Snap num ponto só (o primeiro osso ou o deslocamento do slot) com os testes do binário; o anel nosso saiu também das rotas (`seat-mac.md` §H). Inferido: a sala do teste como a do tile. |

### 3.5 Colocação e construção

Código: `src/vm/placement.*`, `build_edit*.{hpp,cpp}`, `rooms.*`,
`src/formats/build_catalog.*`, `catalog.*`, `design_catalog.*`,
`lot_edit.*`, `lot_write.*`, `lot_roof.*`, `src/app/buy_mode.*`,
`build_mode*.{hpp,cpp}`, `design_tool.*`. Docs: `buy-build-mac.md`.

| Classes e funções do jogo | Onde está | Estado | Notas |
|---|---|---|---|
| Catálogo: `cTSCatalog` (106, 4), `cTSWinProcCatalog`, `cTSWinProcBuyPanel` | `formats/catalog.*`, `app/buy_mode.cpp` | fiel | Ordem entre preços iguais inferida. |
| Preço, venda, depreciação (`DayPassed`) | `app/buy_mode.cpp`, `vm/` | fiel | |
| Colocação: `cTSUserToolObject` (93, 17), `TestPlacementCommon`, `TestPlacementWithRespectToWalls/Objects`, `AutoRotateObjectToMatchWalls` | `vm/placement.*`, `app/buy_mode.cpp` | inferido | Rodada n20 (§7.24): a colocação não usa `HasSupport` (só a escadaria, `CeilingTooHighForObject` `0x30`, feito); feitos `IsFlat` (`0x12`) e os quatro quartos com piso pelo relevo do lote (`VM::placementLot`), conferidos em 12.930 objetos dos lotes (`s2pkg placement-sweep`); o od 4 (bit 0 no chão) não recusa nenhum, mas o `0x2c(1)` do objeto `0xFBA4DA53` segue não lido; faltam a camada de visibilidade do piso, a piscina sob o térreo, a árvore "allow intersection" e as pegadas dentro do tile. |
| Desfazer: `cTSUserToolUndoable*`, `cTSUserToolUndoManager` | `app/build_mode*.cpp`, `app/undo_order.hpp` | fiel | Rodada n20 (§7.24): gerente (`0x2807200`: submeter, desfazer, refazer, soltar), estados do undoable e ordem das operações lidos; uma pilha só para Compra e Construção, esvaziada ao entrar no Vivo (feito); desfazer e refazer gravam o lote byte a byte igual (`tests/build_undo.sh`). Inferido: quem devolve o dinheiro no desfazer (vtable `0x68` → sim system `0xb0(6, ±custo)`). |
| Paredes: `cTSUserToolWall`, `nTSWorld::cWallGraph`, `cPartitionManager` (92, 13) | `vm/build_edit_walls.cpp` | fiel | |
| Salas: `cRoomManager::FloodRoomRecurse` | `vm/rooms.*`, `formats/lot_edit.cpp` | inferido | Rodada n20: o `FUN_009a7dd8` não fecha o cômodo, aplaina o retângulo (feito); os IDs novos ainda vêm da inundação por componentes com o ID antigo da maioria, não do `SetRoomsPostAttach` (§7.3). |
| Pisos e revestimentos: `cTSUserToolFloorPattern`, `cTSUserToolWallpaper` | `vm/build_edit.cpp` | fiel | |
| Terreno, lagos, piscinas: `cTSUserToolTerrain*`, `cTSUserToolWater`, `cTSUserToolUndoablePool` | `vm/build_edit_ground.cpp` | inferido | Rodada n07: o teste de vértice `FUN_009e9550` lido (§7.23); rodada n20 (§7.24): as flags do pincel (`0x2BF7`) vêm de um inicializador estático e o pincel usa o teste do jogo (falta o bit 7, objetos); piscinas: `0x8A`/`0x81`/`0x86` do binário; o nivelamento das fundações e piscinas (`MayPlaceQuadrilateral`) segue inferido. |
| Escadas modulares: `cTSUserToolModularStairs`, `FindEnd` | `vm/build_edit_stairs.cpp` | fiel | `FindEnd` fiel; rodada n20 (§7.24): o +0,75 é de deck no nível do próprio tile de cima (os lances de piso levantado não o ganham, como os salvos), a "elevation" é a altura absoluta (`+0x20c`) e a escavação usa o teste de vértice do jogo (`0x2BB7`). |
| Telhados: `cTSUserToolRoof` (48, 8), `cAutoRoofData`, `nTSWorld::cWorldRoof` (47, 10) | `formats/lot_roof*.cpp`, `vm/build_edit_walls.cpp` | fiel | Retângulos do auto-telhado lidos; rodada n07: `IsRoofValid` (tamanho, cantos, objetos, piscina) lido e usado (§7.22); rodada n13: o pré-teste (`IsAnyRoofAtLocation`, `DoesLocationHasClearanceHeight`) e a caixa com o beiral; 245 dos 275 telhados do jogo aceitos (os 30 têm janelas postas depois, que o jogo também recusaria). |
| Portas e janelas: máscaras `_wallmask`, `HideWhenWallDown`, auto-giro | `formats/lot_model.cpp`, `vm/placement.cpp` | inferido | Objetos sem máscara usam um recorte retangular; rodada n20: no jogo não há caixa (`ComputeWallCutoutLocators`: STR# `0x85` vazio = sem recorte), trocar fica para a área do `lot_model` (TODO). |

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
| Geometria cortada: `nTSSG::cWallManager` (62, 16), `cWallGeometryNode` (66, 11), `cSimplifiedWall` (10, 0) | `formats/lot_model.cpp` | fiel | Rodada n19 (`walls-floors-mac.md` §3): o corte pelas pontas das paredes cortadas (`UpdateSceneGraph`, `SetVertex/SegmentUpDownState`): rampa na parede que toca uma ponta baixada, inteira baixa com as duas; o pé do papel na cortada. Inferidos: fundações e piscinas no modo baixo, meias paredes fora do corte. `cSimplifiedWall` é só dos imposters do bairro. |
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
| `cTSMotives` (17, 4): `GetMotiveRef`, `GetOldMotiveRef`, `UpdateMood`, a atualização de 60 ticks (vtable 0x10) | `vm/motives.cpp` | fiel | Rodada n17 (`motives-mac.md` §3–§5): decaimento `dado/24/30` a cada 60 ticks, trava ±100, conjunto antigo, humor do base com o dado 163 sem trava, leitura arredondada sem trava, Set Motive Change acumula. Falta: o ambiente (`cTSEnvironmentScoreManager`, fica o salvo). |
| Curvas: `cTSMotiveCurve`, `cTSMotiveCurveSet`, `cTSPiecewiseFunction` (15, 0), `cTSMotiveEffects` | `vm/motives.cpp`, `vm/autonomy.cpp` | fiel | Rodada n17 (`motives-mac.md` §1–§2, §6): leitura, inserção, valor em float, conjuntos e a pontuação do base (motivos 3, 5–9, 13–15). |
| Desejos e medos: `cTSWantSimulator` (92, 28), `cTSWantNode`, `cTSWantInstance` (41, 0), `cTSWantTreeManager` | `vm/wants.cpp`, `vm/gnu_sort.hpp` | fiel | Rodada n17 (`wants-mac.md` §9): raízes por id de árvore, `std::sort` da libstdc++, operadores e check tree conferidos, nível, `RawAspirationScore`. Inferido: NPCs com neighbor ID; ordenação lida só no BV. |
| Memórias: `cTSMemoryDefinition`, `cTSMemoryInstance` (18, 0), `cTSMemorySimulator`, `cTSPerson::UpdateMemories`/`PruneMemories` | `vm/memories.*`, `app/wants_panel.cpp` | fiel | Rodada n08 (`relationships-mac.md` §4): tokens de memória, decaimento diário, poda, a pontuação (dado 54; bate nos 50 Sims dos lotes) e Humor+Memória = Humor. Inferido: o `$Constant` dos textos. |
| Relacionamentos: `TryNewRelationship` (fiel), `cTSRelationshipTable` (18, 0), `cTSRelationshipEntry`, `cTSFamilyRelationship` (21, 0), gravador das `SREL` | `vm/primitives.cpp`, `vm/lot.cpp`, `vm/lot_records.cpp`, `vm/hood_*.cpp`, `vm/ownership.cpp` | fiel | Rodada n08 (`relationships-mac.md` §1–3, §6): a tabela de cada objeto (no XOBJ) e de cada vizinho (no SDSC) lista as SREL que valem, chave crua (dono e objeto juntos), ordem do `GUIDHash`, SREL versão 2 sem os bytes velhos, a morte de objeto. Inferido: ordem num mesmo balde, apagar as SREL de quem morreu. |

### 3.9 Salvar e carregar

Código: `src/vm/lot_save.*`, `lot_records.*`, `lot_resume.*`,
`save_types.*`, `hood_save.*`, `family_write.*`,
`src/formats/lot_objects*.cpp`, `character_write.*`, `neighborhood_write.cpp`,
`src/app/game_save.*`. Docs: `save-mac.md`, `game-flow-mac.md` §7.

| Classes e funções do jogo | Onde está | Estado | Notas |
|---|---|---|---|
| XOBJ, XMTO e PERS: `cTSObject::Write/Read`, `cEdithMTObject::Write/Read`, `cTSPerson::Write` | `formats/lot_objects*.cpp`, `vm/lot_save.cpp` | fiel | Layout pela versão do ObjM. |
| ObjM: `cEdithObjectModule` (`read_pre_object`, `SplitLoad`) | `formats/lot_objects.cpp` | fiel | Órfãos do persist DB ignorados, como no jogo. |
| Campos sem nome: cabeçalho, token, iteradores, timers, XMTO (`cTSObject::WriteHeader`/`WriteTimers`/`AddTimer`/`RunTree`, `cTSTreeStack::Write`, `cEdithMTObject::Read`) | `formats/lot_objects*.cpp`, `vm/lot_save.cpp`, `vm/lot_resume.cpp`, `vm/timer.cpp` | fiel | Rodada n09 (`save-mac.md` §8): todos com nome e lidos/gravados pelo sentido; timers com ID do contador do simulador e repetição só com verdadeiro; deslocamentos do XMTO feitos como o jogo (9.562 de 9.562). Falta o u32 +0x34/+0x38 e o u8 +0xFD do NGBH. |
| Rota e animação em andamento | `vm/lot_resume.cpp` | fiel | `save-mac.md` §7c. |
| Controladores: `cTSLoadLotController::LoadLot`, `cTSSaveLotController`, `cTSSaveNeighborhoodController` (8, 0) | `app/game_save.*`, `app/lot_loading.mm` | inferido | `LoadLot` lido (rodada 12); o salvar do bairro não. |
| Persist DB: `cTSPersistSystem` (122, 0), `cTSPersistDBRecord`, cache | `formats/dbpf.*` | inferido | Funciona pelo formato DBPF público; a classe do jogo nunca foi lida. |
| Inventários do bairro: `cTSNeighborhood::Write`/`Read`, `cTSInventory::Read`/`Write` | `formats/neighborhood*.cpp`, `vm/hood_save.cpp` | fiel | Já eram salvos (rodada 16; a linha estava desatualizada). Rodada n09: ordem contados/singulares confirmada; os 16 dados do bairro (dono 0x22) lidos e salvos. |

### 3.10 Bairro e famílias

Código: `src/formats/neighborhood*.{hpp,cpp}`, `hood_*.{hpp,cpp}`,
`lot_arrivals.*`, `lot_evict.*`, `src/vm/hood_bridge.*`,
`lot_neighborhood.*`, `src/app/hood_*.{hpp,cpp}`, `main_menu.*`. Docs:
`neighborhood-mac.md`.

| Classes e funções do jogo | Onde está | Estado | Notas |
|---|---|---|---|
| Terreno: `cTSNHoodTerrain` (66, 16), `nTSSG::cNHoodTerrain` (58, 8) | `formats/hood_terrain.*`, `formats/hood_scene.*`, `app/hood_view.cpp` | fiel | Rodada n12: pesos e mapa alfa (`NormalizePaintWeights`, texel no meio do tile, soma sobre o canvas preto), normais de 6 faces, variantes (byte por célula de 13 tiles, semente do relógio) e o `nhoodLightMap` (`CreateNeighborhoodLightMap`: sombra por varredura, blobs das árvores, normal desviada) do binário; inferidos só o gradiente sob a água e a cor `+0xd8` do estado (`neighborhood-mac.md` §20). |
| Ruas, flora, enfeites: `cTSNHoodOccupantManager` (61, 6) | `formats/hood_scene.*` | fiel | Alisar e nivelar ruas (rodadas 13–14). |
| Imposters: `cLotImposterManager` (38, 2), `cLotImposterDisplay` (31, 12) | `formats/hood_lots.cpp`, `formats/hood_scene.cpp` | inferido | Desbotamento lido (n12): só na saia do lote, que o app não desenha; os lotes brancos das capturas eram o alfa do `multiply` (corrigido). Falta: a cor do `cTerrainShadeLight` (`terrainShade` inferido); 7 marcadores. |
| Lotes: `cTSUserToolPlaceLotBase` (35, 16), `cTSUserToolMoveLot`, `cTSLotInfo`, entrar e sair | `app/hood_mode.cpp`, `formats/hood_lots.cpp` | fiel | |
| Famílias e vizinhos: `cTSFamily` (82, 10), `cTSNeighbor` (61, 3), `cTSNeighborManager`, `cTSFamilyManager` | `vm/hood_bridge.cpp`, `formats/neighborhood.*` | inferido | Rodada n27: despejar a família do lote aberto o jogo recusa (`EvictFamily` BV `0x1434cc`; ninguém sai para o bairro), o app igual. Seguem os 7 marcadores do `hood_bridge` (nomes das flags do FAMI/SREL, unidades da distância). |
| Mudar, despejar, valor do lote: `cTSUserToolNeighborhoodMoveIn`, `cTSUserToolEvictFamily`, `GetLotValue` | `formats/lot_evict.*`, `app/hood_*` | fiel | Rodada n27: os 3 CDs ficam fora da soma também no jogo (`IsInWorld` = `OnInit` feito e `IsOnLot` do tile, BV `0xaf627c`/`0xd1604`); a diferença está só nos valores gravados pela Maxis. |
| Câmera do bairro: `nTSSG::cCameraNeighborhood` | `app/hood_view.cpp` | fiel | |

### 3.11 CAS

Código: `src/app/cas_*.{hpp,cpp}`, `src/formats/cas_*.{hpp,cpp}`,
`sim_character.*`, `sim_censor.*`, `src/vm/genetics.*`,
`baby_character.*`, `appearance.cpp`, `src/app/head_shot.*`,
`dialog_window_clothes.cpp`, `src/render/cas_camera.*`. Docs:
`cas-mac.md`.

| Classes e funções do jogo | Onde está | Estado | Notas |
|---|---|---|---|
| Telas: `cTSUISimEditor` (105, 8), `nTSUICAS::cTSUICASComponent*` | `app/cas_mode*.cpp` | inferido | Rodada n21: a biografia da família não existe no CAS do jogo base (o botão `0x8CB7B638` está escondido no `CB980E50` e nada o mostra; `SaveFamily` grava a caixa vazia), como o app já fazia; zoom e girar enquanto seguram o botão; o passo → câmera do rosto conferido (`EditSimFace` dos painéis de cabeças, rosto e maquiagem). Seguem inferidos: qual aba abre qual painel de rosto, a pose dos traços, a aspiração sugerida. |
| Corpo e roupas: `nTSCASCore::cTSSimOutfitData` (133, 12), `cTSCASBodyPartSystem` (141, 7), `cTSBinCatalog` | `formats/cas_catalog.*`, `formats/character_write.*`, `app/cas_mode*.cpp` | fiel | Rodada n10: corpo gordo = morph `fattop`/`fatbot` do fitness 2 (`0x8141c4`, `SetFitnessLevel`), botão aleatório (`HandleButtonGlobalRandomize`, `SetDefaultClothing`, `RandomizeForHumans`) e flags das categorias (`CheckInit`). Inferidos: o gerador de números, os tons 1–4, o cabelo de bebê; `MatchHairStyle`… do pedido de NPC não lido. |
| Rosto: arquétipos, modificadores, LxNR, `BlendDeltas` | `formats/cas_face.*` | fiel | Rodada n10: peso das máscaras (`BuildRegionMask`: byte de cima da cor) e `hairneck` parado (`BlendArchetype`). Rodada n21: a compensação do modificador no `BlendArchetype` só existe para animais (espécie ≠ 0); `ApplyModifier` (BV `0x809fc4`) = valor × escala do lado, como já era; o rosto do filho é o `BlendFaces` (neutro, regiões na ordem dos passos do `FaceInfo.package`, limites de altura). Inferido: as normais refeitas. |
| Pele, maquiagem, overlays: `cTSSkinTextureCompositor` (27, 0) | `formats/cas_face.*`, `sim_character.*`, `formats/character_write.*` | fiel | Rodada n21: camadas do `GetMaterialLayersToBeComposited` (um overlay por bin, ordem estável por layer, `cafOverlay` na ordem de escolha) e a mistura do compositor (`dst += (src − dst)·α` nos quatro canais, no tamanho da pele); máscaras e quantidades não aparecem nos dados do base. O shader faz a mesma conta em ponto flutuante, sem o reempacotamento DXT1. |
| Genética: `nTSGenetics`, `cTSSimDNA` | `vm/genetics.*` | fiel | |
| Personalidade, signo, aspiração | `formats/cas_personality.*` | fiel | Tabelas não conferidas no base 1.0. |
| Gravar o personagem | `formats/character_write.*` | fiel | |
| Câmera do CAS: `cCameraCreateASimController` | `render/cas_camera.*` | inferido | Rodada n21: pares spec ↔ localizador (efeitos de câmera do SimAntics do "CAS - Backdrop"), roda, botões segurados e o passo do zoom segurado (× 50 / Δt) lidos. Inferido: a direção do zoom (a ordem Q·Rx·Rz foi lida; a convenção do quaternion da GZ não). |

### 3.12 Desenho

Código: `src/render/*`, `src/formats/lot_model.*`, `lot_lightmap.*`,
`model.*`, `gmdc.*`, `txmt.*`, `effects.*`, `lighting_config.*`,
`src/app/lot_lighting.*`, `lot_effects.*`, `plumbob_effects.mm`, `lot_water.*`. Docs:
`shaders-mac.md`, `lighting-mac.md`, `effects-mac.md`, `roof-fence-water-mac.md`.

| Classes e funções do jogo | Onde está | Estado | Notas |
|---|---|---|---|
| Materiais e shaders (caminho DX8 dos `.matShad`) | `render/shaders.metal`, `renderer.cpp`, `formats/model.cpp` | fiel | Rodada n22 (`shaders-mac.md` §13.2–13.4): o passe de bump do DX8 (1ª direcional e 1ª ponto/spot por pixel, o resto no vértice; sem tangentes no Mac), o recorte da pia (`CreateAndRegisterObjectCutoutMaterial`, o mapa `+0xe8` do `cObject`) e o `celShading.matShad` inteiro (linha da rampa, borda, especular, contorno). Inferidos: a força da conversão altura→normal (não está no binário de Mac) e os padrões de registrador do bump sem tangente. |
| Luz e light maps: `cLightingManager` (88, 31), `cLightMapManager`, `cDiffusionManager`, `cFloorColorGrid` (29, 17), `cWallDiffusionManager` (29, 13), `cDiffusionWall` (24, 10) | `formats/lot_lightmap.*`, `lot_diffusion.*`, `app/lot_lighting.*` | fiel | Rodada n16 (`lighting-mac.md` §10): grade de piso (paredes que bloqueiam a luz, portas inclusive, fontes por sala e por andar aberto, varreduras, cópia pela guarda 4×4), paredes com amostras próprias (retas, luz das fontes, piso trazido, propagação, cantos escuros, células 8×24), cômodos sem luz, fontes só com `cDiffusionLightExtension`, irradiância pelos `Lookup*Diffusion`. Portais pelo construtor do jogo (`0x7149fc`: lado pela direção da luz, posição, parede do `L`). Rodada n35 (§11): Delta com sinal pelo `MaxAbsElement` e conjuntos por sala de luz; a fonte de difusão de um portal na própria luz (sala do tile da janela), `doorOrArchDirectMultiplier` 0, `FindPortalSuppression`, práticas "LM_PortalDirect"/"LM_PortalSpill"; difusão de paredes só nos grafos 3/8/10/11 e piscina; o piso das paredes pela grade (passada otimizada, a que o jogo roda com o `LightingOptimizations` do `Graphics Rules`). Inferidos: a altura da luz, as retas montadas das faces, o `+0x20` da supressão (andar), a cor das células de parede fora da difusão, quais 8 luzes ficam num objeto. |
| Sombras: `nTSSG::cShadow` (32, 7), height map do sol | `formats/lot_lightmap.cpp`, `render/` | inferido | Rodada n06: os planos GUOB dos objetos lidos (`UpdateGUOBs`, `EnableGUOB`) e conferidos no lote; rodada n35 (`lighting-mac.md` §11.6): o "LM_sunHM" (`cHeightMapShadowLight`: o sol × o fator do height map na caixa de cada parte) feito; o `AddBlobToHeightMap` só alimenta a textura do bairro. Falta recalcular as GUOB de parede ao mover objetos. |
| Paredes e pisos: `cWallGeometryNode`, `cFloorGeometryNode` (32, 5), `cFloorManager` (31, 6) | `formats/lot_model.cpp` | fiel | Rodada n19 (`walls-floors-mac.md`): nós de parede (espessura, −0,01, span: meia parede 1,025, sótão telhado − 0,25), chanfros, 3 colunas, papel `u` 0/0,5/1 e `v` do topo ou do chão, tampas, topo com UV (x, y) sem light map (`2 × wall-top`), encontros; pisos um triângulo por quarto com UV (y, x) e a rotação 2/4/8. Inferidos: o UV da máscara (plano), o nó 5 da meia parede; falta a borda dos pisos (`cFloorThicknessGeometryNode`). |
| Telhados: `nTSSG::c…Roof`, `nTSWorld::c…RoofType`, classes `c{Even,Odd}Triangle{First,Second}…` | `formats/lot_roof_game.*`, `lot_roof.cpp` | fiel | Rodada n13 (`roof-fence-water-mac.md` §1.4–1.5): os tipos 1–6 como o jogo (grades, `ConnectHeightField`, triângulo escondido sob outro telhado, telhado com telhado nos vértices, célula do atlas por classe, faixas); inferidos: a inclinação global (0,5) e a ordem dos getters das texturas das faixas; sem portar: o `IsTriangleCenterSupportedByOtherRoof` do anel da mansarda; diagonais e pagodes ainda no OpenTS2. |
| Cercas: `cFenceGeometryNode` (57, 0), `cFenceManager` (41, 0) | `formats/lot_model.cpp`, `formats/lot.*` (marcadores) | fiel | Rodada n25 (`roof-fence-water-mac.md` §2.1): trilho, poste, arcos e modelos (`FenceGeometryResource`: a cerca padrão para estilo sem cerca, nada no diagonal sem modelo) como o jogo; o +0,75 é o marcador cru 4 do 3ARY `0x14` (nenhum produto do jogo base), feito no trilho (duas pontas), no poste (o ponto) e na meia parede (nó 5); cerca nenhuma muda nos modos de parede (o modo baixo só tira os nós de meia parede). Inferidos: meia parede = cerca coberta, um poste de arco em cada ponta, o estilo 4 como palco de expansão. |
| Água e piscina: `cWaterGeometryBuilder` (35, 0), `cPoolGeometryNode` | `formats/lot_water.*`, `lot_model.cpp` | inferido | Rodada n06 (§3): alfa por profundidade (`Terrain.ini`), só o reflexo, leito e praia pintados como o binário; piscina como o caminho DX8 do Mac (ondas, reflexo, camadas de fundo); falta a passada de bump e as cáusticas. |
| Efeitos: `cTSEffectsManager` (55, 1), `cTSParticlesEffect`, `cTSMetaParticlesEffect` | `render/particle_system.*`, `app/lot_effects.*` | fiel | Rodada n15 (`effects-mac.md` §4e): descrição, bits, emissão (`ApplyEffect`, `AddRateCurve`/`Inject`/`Maintain`, começo forte/suave), física em passos de 1/15 s, partículas no mundo salvo `rigid`, meta-partículas (cadeias pelas flags dos componentes, `cTSRWState`, colisores planos, caminhos) e a ordem dentro da camada 7 (`cDisplayList::AddGeometry`) lidas e seguidas. Sobras: iluminação (bit 23), LOD pela distância, colisores das partículas e terreno, `wiggle`/`screw`, trator, `cTSSequenceEffect`. |
| Objetos e Sims: `nTSSG::cObject` (140, 20), `nTSSG::cPerson` (50, 5), censura | `app/vm_objects.cpp`, `formats/sim_censor.*` | inferido | Objetos animados escolhidos pela malha de repouso. |
| Espelhos: `MirrorReflectionMaterial`, alvo `ScreenReflection` | `render/renderer.cpp` | fiel | Rodada n22 (`shaders-mac.md` §13.1): um observador por espelho (`cViewerRefNodeRecursiveImpl`), todos no mesmo alvo, cada um no seu retângulo e no stencil dos quads (`cApplyStencilStateForOverlappingReflections`: 1 no próprio, 0 nos outros); a censura da vista mostra o reflexo. Nosso: no máximo 8 por quadro; o filtro de visibilidade do observador (máscara `0x07440100`, `InvisInReflections`) não foi resolvido. |

### 3.13 Câmera e UI de jogo

Código: `src/render/camera.*`, `src/app/camera_*.{hpp,cpp}`,
`live_ui.*`, `live_panels.*`, `options_panel.*`, `game_cursors.*`,
`game_flow.*`, `pie_menu.*`, `src/formats/camera_config.*`, `ui_*.{hpp,cpp}`.
Docs: `camera-ui-mac.md`, `ui-live-mac.md`, `game-flow-mac.md`.

| Classes e funções do jogo | Onde está | Estado | Notas |
|---|---|---|---|
| Câmera: `cTSCameraSmoothSims1Controller` (58, 9), `cCameraLive` | `render/camera.*`, `app/camera_*` | inferido | A velocidade do teclado e o pan (`0x3b6c780f`/`0xbb6c7808`: `sqrt(largura·30)`, aceleração por repetição) seguem inferidos (`TODO.md`). |
| Vista de cima: `cTSMainToolbar::SetTopDown`/`SetMode`, a câmera "Build" do `cCameraLive` | `app/camera_mode.hpp`, `LotCamera::setDefinition`, painéis de Comprar e Construir | fiel | Rodada n11 (`camera-ui-mac.md` §1.7): T só em Comprar e Construir, o flag lembrado no Viver, a troca copia pivô, rumo, distância e pitch. Inferido: o `pivotheightabovefloor` do Build. |
| Seguir o Sim: `cTSMainToolbar::AutotrackSim`/`CenterOnSim`, `cTSCameraSmoothSims1Controller::Update`/`HandleRequest`, `cTSWinProcSimChooserThumbnail` | `app/camera_follow.*`, `live_ui.cpp` | fiel | Rodada n11 (§1.8): zona morta medida do alvo do pivô ao ponto do piso do Sim, os movimentos do jogador acabam com o seguimento, o clique direito centra e segue, o esquerdo na selecionada centra e para. |
| Fade de objetos: `cTSSGSystem::ObjectFadeParams`, `cObject::UpdateVisibilityAndRendering` | `app/object_fade.hpp`, `lot_actors.cpp`, `Renderer::setInstanceAlpha` | fiel | Rodada n11 (§1.10): "Obstructs View" (od 40 bit 3), alfa = (profundidade/distância)^queda. A câmera de cinema (`FUN_0065deb0`) e o `ParentFadeAlpha` não foram feitos. |
| Câmera livre: `cCameraCameraManController` e a base de primeira pessoa | `render/free_camera.*`, `LotCamera::matchView`, `app_view.mm` | inferido | Rodada n11 (§1.9): Tab/Y/Escape, números do `FirstPersonCameras.txt`, velocidade e altura do `Update`; inferidos o olhar com o mouse e o FOV de partida; a órbita (Shift + teclas), a "CameraManNH" e a "First" não foram feitas. |
| Menu de pie: `cTSWinPieMenu` (36, 5) | `app/pie_menu.*` | fiel | |
| Velocidade, relógio, dinheiro: `cTSMainToolbar` (89, 10), `cTSWinProcLivePanel`, `cTSSimulator::Simulate` | `app/live_ui.cpp` | fiel | |
| Painéis: motivos, relacionamentos, emprego, desejos (`cTSWinProcPanel*`, `cTSWinProcMotiveBar`) | `app/live_panels.*`, `wants_panel.*` | fiel | |
| Painel de memórias: `cTSWinProcPanelMemories` (21, 0), `cTSWinProcMemoryThumbnail`, `cTSWinProcMemoryDescription` | `app/live_panels.cpp`, `app/wants_panel.cpp` | fiel | Rodada n08 (`relationships-mac.md` §5): miniaturas `0x4906500A` (fundo bom/ruim, ícone com máscara, seleção), descrição `0x4906500C`. Inferido: páginas por setas (o jogo rola), o painel expandido não é desenhado. |
| Fila de ações: `cTSUIActionQueue`, `cTSWinActionQueueIcon` | `app/live_ui.cpp` | fiel | |
| Clique e cursores: `cTSWinSceneGraph::ObjectAtGameWindowPoint` | `app/game_cursors.*`, `lot_view.mm` | fiel | |
| Fluxo do jogo: `cTSGameStateController` (65, 14), `cTSStartupController` | `app/game_flow.*`, `scene_*.mm` | fiel | Rodada n27 (`game-flow-mac.md` §7.5–7.6): o caso Construir é o do dormitório (só no BV; o base 1.0 escolhe Viver ou Comprar); o estado da barra salvo no lote (`cTSMainToolbar::Write/Read`, recurso `0x8B0C79D6`: pausa, andar, modo de parede) volta na entrada e é salvo. Inferido: os lotes 1 e 4 do Tutorial abrem rodando. |
| Diálogos: `cTSUIDialogManager`, `cTSObjectDialog` | `app/dialog_window*.cpp`, `vm/dialog.cpp` | inferido | Rodada n27 (`primitives-mac.md` "Dialog"): `SetParams`, `SetupDialog` (a caixa de fechar fora da fila: o travamento do `0c`), o laço das larguras e o `DoLayout` (ícone e texto em volta) do binário; os 35 tokens do `ParseUIString` do jogo base na ordem e com as regras do jogo. Falta: os diálogos de escolha (`dialog_window_pickers.cpp`), os tokens de inventário e o formatador de números do idioma. |

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
| 7 | **Construção: suporte, telhado e terreno** (`HasSupport`, `SupportedUnderneath`, leveler, `cTSUserToolRoof`, piscinas) — **feita** (n07): suporte pelas salas de carga, colunas, paredes de carga e topo de mansarda (`IsFloorable`, paredes `0x48`), `IsRoofValid` (`0x10`/`0x7E`/`0x7F`/`0x1C`), códigos das piscinas; conferido nos 63 lotes (`s2pkg lot-save --check-build`: 14.071 de 14.098 pisos); o teste de vértice do terreno lido mas não aplicado (flags desconhecidas) | Erros de colocação no modo Construção. | 1 Opus | sim |
| 8 | **Relacionamentos e memórias** (`cTSRelationshipTable`, gravador das `SREL`, `cTSMemory*`) + painel de memórias — **feita** (n08): tabelas de relacionamento do XOBJ e do SDSC, SREL como o jogo grava, motor das memórias e pontuação, painel de memórias com os scripts do jogo (`docs/relationships-mac.md`). | Afeta histórias e o painel; não quebra o jogo. | 1 Opus + 1 Sonnet (painel) | Opus sim |
| 9 | **Salvar: campos sem nome** (token, timers, XMTO) e inventários do bairro — **feita** (n09): cabeçalho, token temporário (campos 4–7 como cópias), iteradores dos arrays, timers (ID, quem pôs, repetir), XMTO (deslocamentos) e NGBH (dados do bairro) com nome e pelo sentido (`docs/save-mac.md` §8); `lot-save --check` byte a byte igual nos 69 lotes, `hood-save --check` 68 de 68. | Hoje o save já volta igual; é limpeza. | 1 Opus | sim |
| 10 | **CAS: corpo gordo, `BlendDeltas`, roupa aleatória** — **feita** (n10): corpo gordo como morph `fattop`/`fatbot` (não há roupa gorda nos dados), botão aleatório como o `RandomizeForHumans` (cabelo sem chapéu por categoria, 49 % roupa inteira, flags 1\|8 fora, "default" de reserva), peso das máscaras de região e `hairneck` do `BlendArchetype` (`docs/cas-mac.md`, fim). | O corpo gordo é um bug visível no CAS. | 1 Opus | sim |
| 11 | **Câmera: seguir o Sim, Tab, fade, vista de cima** — **feita** (n11): vista de cima (T, a câmera Build e o flag do `SetMode`), seguir o Sim (zona morta do alvo, movimentos do jogador, centrar), fade de objetos "Obstructs View" e a câmera livre do Tab (`camera-ui-mac.md` §1.7–1.10); órbita, "CameraManNH" e fade da câmera de cinema ficam no `TODO.md`. | Conforto; os valores já estão nos dados. | 1 Sonnet | não |
| 12 | **Bairro: imposters e terreno** (desbotar, mapa alfa, `nhoodLightMap`) — **feita** (n12): mapa alfa, variantes, normais e `nhoodLightMap` do binário (`neighborhood-mac.md` §20); o desbotamento só existe na saia do lote; os lotes brancos das capturas eram o alfa do `multiply`. | Pouco visível. | 1 Opus | sim |

Total: 14 agentes Opus e 3 Sonnet, em 12 rodadas. As rodadas 1 e 2 tocam
os mesmos arquivos (`anim_player.cpp`, `seat_module.*`): rodar em sequência,
não em paralelo.

## 6. Plano de rodadas 13 em diante (o que ainda segura o índice)

Feito em 2026-10-15, depois das rodadas 1–11, para as linhas de §3 que
seguem "inferido" ou "faltando". Rodam **até 3 em paralelo**, desde que
sejam de **áreas diferentes** (coluna "Área"): duas rodadas da mesma área
nunca rodam juntas. Todas Opus com Ghidra (só leitura). Ordem por impacto
no jogo base.

| # | Rodada | Linhas de §3 | Área |
|---|---|---|---|
| 13 | **Telhados do jogo**: triangulação, células do atlas e telhado com telhado (portar o `lot_roof.cpp` para as funções do `cWorldRoof` mapeadas no `TODO.md`) + pré-teste 0x7F do `cTSUserToolRoof` (beiral sobre janelas) — **feita** (n13): tipos 1–6 pelo caminho do jogo (`formats/lot_roof_game.*`, `roof-fence-water-mac.md` §1.5) e o pré-teste com a caixa do beiral (245 de 275 telhados aceitos; os 30 restantes o jogo também recusa) | 3.12 telhados, 3.5 telhados | telhado |
| 14 | **Locomoção**: `PushTurnBlendCycles`, `StopLocomotion`, `cLocoCycle`, encadeamento das fases e o primeiro pé da parada — **feita** (n14): relógio das fases e entrega na saída do pedido (a rota acaba 233 ms antes do fim da parada), trocas por `BlendDistance` com cosseno, pé da parada pelo `0x178`, rotas curtas, parada ao bloquear (`anim-mac.md` §8) | 3.3 locomoção | animação |
| 15 | **Partículas**: `cTSEffectsManager`, `cTSParticlesEffect`, `cTSMetaParticlesEffect` (os 7 marcadores) e a ordem dentro da camada 7 — **feita** (n15): emissão, física, meta-partículas e a ordem na camada 7 lidas e seguidas (`effects-mac.md` §4e); sobras no `TODO.md` | 3.12 efeitos | efeitos |
| 16 | **Luz e light maps**: `cLightingManager`, `cLightMapManager`, `cDiffusion*` (lista M4 do `TODO.md`) — **feita** (n16): a grade de piso e as paredes do `cDiffusionManager` lidas inteiras e feitas (`formats/lot_diffusion.*`), cômodos sem luz, fontes só com extensão de difusão, irradiância dos cômodos e o `L` dos portais pelos `Lookup*Diffusion`, portais como o construtor do jogo os monta (`lighting-mac.md` §10); a sala da fonte de um portal segue inferida | 3.12 luz | luz |
| 17 | **Motivos e desejos**: `cTSMotiveCurve*`, `cTSPiecewiseFunction`, `UpdateMood` (dado 163), `cTSWantSimulator` (ordem, empates, check tree) — **feita** (n17): curvas e conjuntos do jogo, motor de 60 ticks (decaimento, trava, conjunto antigo), humor do base com o dado 163 (bate com os 55 Sims salvos), pontuação da autonomia do base, desejos pela ordem do mapa de árvores e o `std::sort` da libstdc++ (`motives-mac.md`, `wants-mac.md` §9) | 3.8 curvas, desejos | motivos |
| 18 | **Rotas, resto**: custo por `ePassabilityState`, `FollowOneStep`/`ComputeTurn`, Snap num ponto só, bônus +25 longe da parede, retângulos de custo 10 da escada — **feita** (n18): `CollisionCheck` e contador do bloqueio do binário (o laço de "entradas" de dois Sims da n14 acabou; sem regressão em 14 sementes), Snap num ponto só e sem o anel (o café pelo lado), custo do piso por padrão (2 no terreno) com o corte de quinas do binário, o +25 (`routing-mac.md` §9.1); os retângulos da escada lidos e não feitos | 3.2 obstáculos, locomotor, slots; 3.4 H | rotas |
| 19 | **Paredes e pisos**: `cFloorGeometryNode`, `cFloorManager`, `cWallGeometryNode`, `cSimplifiedWall` (UV dos pisos e do topo) — **feita** (n19): paredes como o `cWallGeometryNode` (nós, chanfros, 3 colunas, UVs do papel, topo sem luz com UV do mundo, tampas, encontros, corte pelas pontas com rampas) e a rotação dos pisos; UV dos pisos e do topo conferidos (`walls-floors-mac.md`) | 3.12 paredes e pisos, 3.6 geometria | lote |
| 20 | **Colocação de objetos e salas**: `cTSUserToolObject`/`TestPlacement*` com `HasSupport`, `FUN_009a7dd8` (cômodo), desfazer peça por peça, +0,75 da escada — **feita** (n20): teste de vértice `FUN_009e9550` com as flags dos inicializadores estáticos (`0x2BB7`), nivelador e cômodo que aplaina o retângulo, arrastes tudo ou nada, `IsFlat`/piso inteiro/`CeilingTooHighForObject` na colocação, desfazer numa pilha só até o Vivo e byte a byte, escadas com elevation absoluta (`buy-build-mac.md` §7.24) | 3.5 colocação, salas, desfazer, escadas | construção |
| 21 | **Pele e CAS, resto**: `cTSSkinTextureCompositor`, câmera do CAS, compensação do arquétipo, `BlendFaces`, biografia da família — **feita** (n21): compositor e camadas lidos (um overlay por bin, `cafOverlay` com a vírgula do jogo: editar um Sim salvo não perde a maquiagem), `BlendFaces` com os dados do `FaceInfo.package`, câmera (pares pelo SimAntics, botões segurados, roda), compensação só de animais, biografia da família inexistente no base (`cas-mac.md`, fim); a direção do zoom segue inferida | 3.11 pele, câmera, rosto, telas | CAS |
| 22 | **Shaders e espelhos**: normal map, passe de recorte, CelShaded, `MirrorReflectionMaterial` com stencil — **feita** (n22): todo espelho em vista reflete com o stencil do jogo e a censura da vista mostra o reflexo; o recorte da pia (a cuba aparece, o balcão mantém o design); o bump do DX8 sem tangentes do Mac; o `celShading.matShad` inteiro (`shaders-mac.md` §13) | 3.12 materiais, espelhos | shaders |
| 23 | **Animação, resto**: arquivo ANIM (strings, tangentes, Euler), canal de eventos, Look At na cabeça, cinemáticas (spots, ObjectHiding) — **feita** (n23): ANIM lido e regravado byte a byte (tangentes por tick, chaves a 30/s, rotações por quadro com slerp), árvores de evento com os 5 parâmetros e os temps postos de volta (o Animate Sim recebe 9997–9999), Look At a 3/4 da caixa do alvo com as velocidades do primitivo, cinemáticas no andar da origem (`anim-mac.md` §9–11, `cinematics-mac.md` §8); o `AnimUpdate` do olhar e os spots ficam no `TODO.md` | 3.3 ANIM, eventos, olhar, cinemáticas | animação |
| 24 | **VM, resto**: `cTSGlobals`, `cTSDataTables`, `TryPlaySound`, `cTSInteraction`, registros de objetos da pessoa (`+0x60c`, faltando) | 3.1 fila, dados, som; 3.7 registros | VM |
| 25 | **Cercas**: `cFenceGeometryNode`, `cFenceManager`, o +0,75 por tipo de piso — **feita** (n25): gerente (13 nós por andar, postes do FPL, modos de parede: cerca nenhuma é cortada nem baixada), modelos (cerca padrão, diagonal sem troca), arcos (peças das pontas só onde a linha acaba) e o +0,75 = marcador cru 4 do 3ARY `0x14` (nenhum no jogo base: os lotes do jogo saem iguais, bytes iguais nas capturas) lidos e feitos (`roof-fence-water-mac.md` §2.1) | 3.12 cercas | lote |
| 26 | **Salvar, resto**: salvar do bairro (`cTSSaveNeighborhoodController`), `cTSPersistSystem` | 3.9 controladores, persist | salvar |
| 27 | **UI de jogo**: `ApplySavedUIState` (caso Construir), diálogos (27 marcadores), famílias e despejo se a 12 não fizer — **feita** (n27): o caso Construir é só das expansões, o estado da barra salvo no lote (pausa, andar, paredes) volta e é salvo, o `cTSObjectDialog` e os 35 tokens do `ParseUIString` do binário (fim do travamento do `0c`; mensagem e botões dos temps como o `SetParams`: as cartas de chance), o despejo do lote aberto recusado como no jogo, os CDs fora do valor também no jogo; os diálogos de escolha ficam no `TODO.md` | 3.13 fluxo, diálogos; 3.10 famílias | UI |
| 28 | **Desempenho das rotas e dos testes** (pedido do usuário, passa na frente): cachear a árvore do planejador (quadtree e tabelas de soma 512×512) entre planos, com um jeito barato de saber que os obstáculos não mudaram; índice para `containedIn`/`followContainers` (setter em toda escrita de `ContainerId`/posição). Regra do `docs/performance.md`: simulação bit a bit igual e capturas idênticas; medir antes/depois com `lot-run` dos 69 lotes e `autonomy` (ciclos e instruções) — **feita** (n28): grades do planejador sincronizadas só onde os carimbos de obstáculo/custo mudaram e quadtree partida de uma pirâmide de somas atualizada por retângulo (as somas são as das tabelas bit a bit enquanto todo custo é 0 ou fica em [0,5; 16]); índice dos contidos (`VM::setContainer`/`noteContainer` em toda escrita do dado 2, SimAntics e carregamento inclusive; `OS2_CHECK_CONTAINED=1` confere); Set to Next 24 e o teste de pegada a cada passo numa passada só pelos objetos; timers, destinos e objetos por GUID (Set to Next 1/4/18) listados. Sem índice por tile (pede setter em toda escrita de posição). `autonomy N001_Lot11` 78,5 → 42,1 G instruções e 26,1 → 14,9 G ciclos, `lot-run` dos 69 lotes 97,8 → 76,3 G instruções (`docs/performance.md`, rodada n28) | `docs/performance.md` "What is left" | rotas (só depois do reforço `n18b-rotas` entrar) |
| 29 | **`s2pkg` com 100.000 nós por rota** (pedido do usuário, passa na frente; débito 3.1): os comandos `lot-run`, `pie`, `route`... usam 20.000 nós por turno e podem dar "sem rota" onde o jogo e o app (100.000, do binário) acham; um valor só, do binário, para todos — **feita** (n29): os comandos já usavam os 100000 desde a rodada 16 (`bd77823f`); agora `vm::kNodeLimitPerTurn` (`DAT_0277ea94`, `docs/routing-mac.md` §7.1) é o valor único do app, do VM e de todo o `s2pkg` (o `lot-save` tinha o seu), com teste; medido nos 68 lotes (`lot-run`, 1800 e 20000 ticks): as mesmas rotas que falharam (57 e 513), saída idêntica, custo igual; nenhum "sem rota" some, porque nenhum turno passa de 20000 nós nos dados de hoje | `docs/ideias/DEBITOS-TECNICOS.md` 3.1 | ferramentas |

Ordem: as linhas 28 e 29 passam na frente das 24–27 (pedido do usuário, 2026-10-15): a 29 na próxima vaga; a 28 quando a área "rotas" ficar livre.

Limites: algumas linhas dependem de valores que só existem com o jogo
rodando (UV da parede da piscina, flags do pincel de terreno); nesses
casos a rodada lê o que der e deixa a linha "inferido" com o motivo.

## 7. Plano de rodadas 30 em diante (bugs achados e sobras das rodadas)

Feito em 2026-10-15, com a §6 quase toda feita (índice 82–83%). Primeiro os
bugs que aparecem jogando ou nos soaks; depois as sobras de cada rodada
(listadas no `TODO.md` com o nome da rodada). Mesmas regras da §6: até 3 em
paralelo, áreas diferentes, Opus com Ghidra só leitura.

| # | Rodada | De onde | Área |
|---|---|---|---|
| 30 | **Impasse na varanda do N001_Lot11** (sementes 2/4/6: dois Sims no degrau de um tile entre a escada do deck e a porta) e **voltar `stair_top` e `glides` à forma estrita** (afrouxados na n23): `CreateMoveOutOfWayDestination`, a regra "posição atual também inválida" do teste de passo, tempo das pausas | n18b, n23 | rotas |
| 31 | **Visitantes em laço de "Leave"** no N003_Lot192 (Tybalt e Juliette, ~327 vezes a partir das 02:00; na v0.2.0 saíam) | fix-noite | VM/sociais |
| 32 | **Fogo sem bombeiros** no N001_Lot68 (fogão às 17:29, dois Sims morrem): conferir no binário quem chama os bombeiros sem detector e se é só a semente | fix-noite, `TODO.md` incêndios | VM |
| 33 | **Ambiente (motivo 13)**: `cTSEnvironmentScoreManager` (BV `0x5805a0`, `GetEnvironmentRoomID` `0x57ffa8`), refeito a cada 60 ticks; e o tique do primeiro passo de 60 depois de carregar | n17 | motivos |
| 34 | **Olhar e eventos, resto**: portar o `AnimUpdate` do look-at (BV `0xd41e04`), eixo dos spots, chave de evento 6 contra 3, evento por volta, velocidade das cinemáticas | n23 | animação |
| 35 | **Luz, resto**: sala da fonte do portal, `doorOrArchDirectMultiplier`, supressão janela-janela, luzes diretas das janelas, grafos de parede 3/8/10/11, passe incremental, `LM_sunHM` e `AddBlobToHeightMap` dos objetos; conferir o brilho dos objetos à noite (geladeira quase branca) — **feita** (n35, `lighting-mac.md` §11): Delta com sinal e conjuntos por sala de luz (a geladeira branca vinha das Delta só positivas e da sala da VM), portais como o jogo (fonte na luz, portas sem luz direta, supressão, práticas das janelas), paredes só dos grafos de luz e piscina com o piso da grade (a passada otimizada que o jogo roda), "LM_sunHM" por objeto; o `AddBlobToHeightMap` é só do bairro | n16 | luz |
| 36 | **Lote, resto**: borda de espessura do piso (`cFloorThicknessGeometryNode`), UV e espelhamento das máscaras de porta/janela, tirar o recorte retangular sem máscara (o jogo não tem), meia parede pelo `PartitionType` 3 | n19, n20, n25 | lote |
| 37 | **Construção, resto**: nivelar fundação e piscina (`MayPlaceQuadrilateral`), bit 7 do teste de vértice, o Construir gravar os marcadores 3ARY `0x14`, `SetRoomsPostAttach`, quem devolve o dinheiro no desfazer, ferramenta da lareira | n20, n25 | construção |
| 38 | **Partículas, resto**: iluminação (bit 23), LOD pela distância, colisores e terreno, wiggle/screw, tractor, `cTSSequenceEffect`, efeitos de colisão/morte | n15 | efeitos |
| 39 | **Câmera, resto**: órbita (Shift+WASD, `PickOrbitCenter`), `CameraManNH` no bairro, fade das cinemáticas, fórmula do pan pelo teclado, direção do zoom do CAS | n11, n21 | câmera |
| 40 | **Sombras**: `nTSSG::cShadow`, height map do sol, sombras de parede recalculadas ao mover objetos | §3.12 | sombras |
| 41 | **VM, resto** (a 24 da §6) e **Salvar, resto** (a 26 da §6), quando a 28 (desempenho) entrar | §6 | VM / salvar |

Ordem: 30 a 32 primeiro (bugs). A 30 e a 31/32 esperam a 28 (desempenho, que
mexe nas rotas e na VM). As outras podem rodar já, em áreas livres.

**Precisam de análise dinâmica** (o jogo rodando; não gastar rodadas lendo):
UV da parede da piscina, o `+0x194` de início das cinemáticas, o valor
inicial de alguns globais que nada no binário escreve.
