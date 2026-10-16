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
| VM e primitivas | 54 (19) | 233 / 1.610 = 14,5% | 10 / 11 = **91%** (com a n41) | 125 / 80 |
| Rotas | 27 (13) | 113 / 622 = 18,2% (não recontado na n18) | 7,5 / 8 = **94%** | 59 / 38 |
| Animação e IK | 78 (22) | 88 / 1.364 = 6,5% (não recontado na n23, n34 nem n43) | 8,5 / 9 = **94%** (com a n43) | 36 / 14 |
| Sentar | (usa rotas, slots, IK) | 22 / 34 funções-chave = 65% | 8 / 10 = **80%** | 10 / 8 |
| Colocação e construção | 180 (71) | 311 / 2.556 = 12,2% (não recontado na n20, n36 nem n37) | 10 / 11 = **91%** (com a n37) | 64 / 50 |
| Corte de paredes | 11 (5) | 32 / 159 = 20,1% (não recontado na n19) | 6 / 6 = **100%** | 4 / (na ui-live §8c–8d) |
| Autonomia | 11 (2) | 64 / 251 = 25,5% | 4 / 5 = **80%** | 9 / (em primitives) |
| Motivos, desejos, memórias, relacionamentos | 21 (7) | 41 / 355 = 11,5% (não recontado na n17 nem na n33: a n33 nomeia os ~20 métodos do `cTSEnvironmentScoreManager`) | 5 / 5 = **100%** | 3 / 7 |
| Salvar e carregar | 32 (4) | 78 / 936 = 8,3% (não recontado na n09) | 6 / 7 = **86%** | 10 / 2 |
| Bairro e famílias | 64 (29) | 105 / 1.250 = 8,4% (não recontado na n12 nem na n27) | 6 / 7 = **86%** (com a n27) | 41 / 45 |
| CAS | 89 (25) | 87 / 1.534 = 5,7% (não recontado na n10 nem na n21) | 7 / 8 = **88%** | 33 / 28 |
| Desenho | 211 (36) | 129 / 2.309 = 5,6% (não recontado na n13, n16, n19, n22, n25, n36 nem n40) | 9,5 / 10 = **95%** (com a n40) | 55 / 41 |
| Câmera e UI de jogo | 59 (21) | 90 / 1.031 = 8,7% (não recontado na n11 nem na n27) | 11,5 / 13 = **88%** (com a n27) | 43 / 52 |
| **Total** | **816 (252)** | **1.371 / 13.977 = 9,8%** (só src: 699 = 5,0%) | **96,5 / 110 = 87,7%** (com a n23, a n25, a n27, a n36, a n34, a n37, a n40, a n41 e a n43) | **505 em src** |

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
| Fila de interações: `cTSInteraction` (77 métodos, 14 nomeados), `TryPushAction`, `cTSPerson::TryIdleForInput`, `SetCurrentAction` | `vm/queue.cpp`, `links.cpp` | inferido | Sub-fila e aninhadas (erro 0x6F) feitas; `SetCurrentAction` (dados de pessoa 39/71/73/110, sub-ações) também no Gosub Found Action (rodada n04). Rodada n31 (`primitives-mac.md` §4g): os bits `0x400` (autônoma, dado 166), `0x800` (testar como autônoma) e `0x4000` (sem continuação) do `+0x54`, `ActionSkipped`/`ClearSubActions` desfazendo vínculos, a espera de um tick depois da recusada e os testes do `TestInteraction` (depuração, idade, visitante) no Push e na fila. Falta: o dado de pessoa 164, o evento 4 do `ActionSkipped`, os testes `0x214`/`0x2e8` do objeto, o "trocar" da fila cheia (vtable `0x1FC`). Rodada n41 (`primitives-mac.md` §4j): o dado de pessoa 39 "route status" como o roteador do base o mantém (1 durante a rota, era sempre 0: os "Shoo" enxotavam quem passava andando) e o `SetCurrentSubAction` (39, 73, 71, 110 := 0); falta o 109 da sub-ação. |
| Relógio: `cTSSimulator::SimulateOneTick`, `Simulate` | `vm/vm.cpp` | fiel | 30 ticks por minuto, hora do dia, meia-noite. |
| Erros: `cTSObject::HandleError`, `GetErrorString` | `vm/vm.cpp`, `errors.hpp` | fiel | Falta o reset do object data 7 (`GetDefaultNicenessMultiplier`). |
| Módulo de objetos: `cEdithObjectModule` (120, 30 nomeados): `MakeNewOutOfWorldObject`, `FlushKillQueue`, `PostLoad`, `UpdateObjectsInRoom`, `UpdateObjectsAdjacentToWall`, pontos de entrada (load, cleanup, roomChanged, wallAdjacency) | `vm/vm.cpp`, `kill.cpp`, `lot_resume.cpp`, `rooms.cpp`, `placement.cpp`, `build_edit.cpp` | fiel | Rodada n04 (`primitives-mac.md` §4i): load em todo objeto carregado, roomChanged pelo `OnRoomChanged`, wallAdjacency pelo `UpdateWallAdjacency`. dynMultiTile (entrada 8): nenhum OBJf do jogo base a tem. |
| Dados mantidos pelo motor: object data "room" (`SetLocation`/`SetRoom`), od 5 (wall adjacency), campos de globais, `cTSGlobals` (58, 2 nomeados), `cTSDataTables` (39, 0) | `vm/world.*`, `rooms.cpp`, `placement.cpp` | inferido | Sala e od 5 pelo binário (rodada n04; a sala é a do tile, o binário usa o quarto de tile do ponto). `cTSGlobals` e `cTSDataTables` não lidos. |
| Som: `TryPlaySound` | `vm/sound.cpp` | fiel | Rodada n41 (`primitives-mac.md` §4j): tabelas pelo \|n\| (doc e código batem), o 0x40 é parar (`cTSAudioEventHandler::DoEvent`), índice negativo pelo valor, volume não lido. Inferido: o laço (o binário o deixa à trilha). |
| Atributos de tipo: `cTSObjectTypeAttributeManager` | `vm/vm.cpp` | fiel | Rodada 16. |

### 3.2 Rotas

Código: `src/vm/routing*.cpp`, `path_planner.*`, `locomotion.*`,
`sim_motion.*`, `turn.cpp`, `src/app/door_portals.*`. Docs:
`routing-mac.md`.

| Classes e funções do jogo | Onde está | Estado | Notas |
|---|---|---|---|
| Planejador: `nPathPlanner::cPathPlanner` (48, 14), `cQuadTree`, `cAStarSearch`, `CreateResultPath` | `vm/path_planner.*` | fiel | Célula 1/8 de tile, A* com ruído. |
| Obstáculos: `cTSRouteManager` (70, 12), `WorkOutTileCostAndPassability` | `vm/path_planner.cpp`, `routing_plan.cpp`, `world.cpp` | inferido | Rodada n18 (`routing-mac.md` §9.1): o estado de cada célula (o maior; > 1 bloqueia) conferido, o custo do piso por quarto de tile (`routingcost` do padrão, 2 sem piso) e o corte de quinas do binário feitos. Seguem inferidos: tile da parte de multi-tile, a caixa do modelo sem lista (a flag 4 do nó), o `+10` do `WorldOutsideSupport` e o estado 7. |
| Objetivos: `cTSRoute::EvalTileForGoal`, gerador de pontos | `vm/routing.cpp` | fiel | A tabela da §9 do `routing-mac.md` ainda diz "aproximação": está velha. Rodada n30 (§9.3): o filtro do cone antes do `EvalTileForGoal` e a pessoa no ponto a 0,283 (era meio tile, inferido); falta o "drop to floor below" (`0x98`). |
| Máquina do roteador: `cTSRoutingObjectAggregate` (95, 48): pós-rota, goal check, portais, `AskBlockerToMove`/`MoveOutOfWay` | `vm/routing*.cpp` | fiel | O mais estudado da rota. Rodada n18b (`routing-mac.md` §9.2): o goal check quando o `DoAlongRouteCheckTest` diz (antes do passo, na 1ª vez de toda rota, caminho/sala/andar novo ou mais de 1 andado) e forçado no `DoBlocked`; a pilha de rotas (a rota de dentro de um goal check guarda a de fora: o 11 falso dos sociais acabou). Rodada n30 (§9.3): o `CreateMoveOutOfWayDestination` dá ao destino um slot sintético (tipo 0: um passo de 0,3 a 45°) e o `MoveOutOfWay` aceita sem lugar; quem pediu replaneja quando a rota do outro acaba; o contador volta a 1 quando quem anda saiu 0,25 (os lugares de todos os roteadores anotados a cada planejamento); o "CT - Route Blocked" verdadeiro replaneja, uma vez por rota; a profundidade da pilha conta a rota da árvore de falha. Rodada n31b (§9.4): do goal check do bloqueio direto ao `DoBlockedGoalStillAvailable` com o bloqueador guardado (o passo não é testado de novo) e um goal check por tick (o contador `+0x3c` do agregado, zerado no `OnPreSimulatorTick`). Inferido: a direção relativa dos tipos 1/2. |
| Locomotor: `nTSSG::cLocomotable` (57, 14): `FollowOneStep`, `ComputeTurn`, `CollisionCheck` | `vm/routing_loco.cpp`, `routing_walk.cpp` | fiel | Rodada n18: o `CollisionCheck` lido de novo e feito (a primeira colisão no horizonte, só se larga quem já toca e se afasta, parada com clipe só se acaba 2 passos antes, senão `EndFollow`); as duas exceções nossas saíram; o contador do `DoBlockedGoalStillAvailable` guarda o bloqueador (`routing-mac.md` §9.1). Rodada n18b: a trajetória prevista é a do `GlobalLocationEstimate` (o caminho cúbico do quadro, não as retas entre os pontos; §9.2). Rodada n30: o teste por passo deixa ir quem já está num lugar inválido por qualquer motivo (`LocoPlaceFromSceneGraph`). |
| Slots de rota: `cTSRoutingSlot` (58, 3), `cTSSlotDescriptor` (39, 0), `cTSSlotLoader` (19, 0) | `vm/routing.cpp`, `formats/` (SLOT) | inferido | Rodada n03: `Init`, `Read`/`Write` do descritor e os bits das RS flags (par `0x200000`, aleatório `0x2000`, parado `0x40000`, paredes `0x80000`, continuação `0x400000`/`0x800000`) lidos; o nome de osso exige `0x20000` (`routing-mac.md` §8.6); o +20 "parado" feito; o +25 das paredes lido e feito na rodada n18 (`GetWallBlockFlagsAtTile`, a direção da última origem). Rodada n30: a vtable dos setters (`0xd0` SetDistances(mín., máx., ótima), `0xcc` SetGradient, `0xd4` SetSpacing, getters `0xb8`–`0xc8`). Falta: a escala 1/16 do `Read` por versão. |
| Portais de objeto e escadas: `cPathPlanner::AddDoorPortal`, pares de slots | `vm/routing_plan.cpp` | fiel | Rodada n03: escadas pelos slots das peças, duas faixas de mão única (sobe/desce), flag `0x200000` e mão única conferidas (`routing-mac.md` §8.6). Rodada n31b (§9.4): `PortalAddToPathPlannerImpl`, `MakeNodesForPortal` e `ConnectPortalNode` lidos (a ponta liga às células em ±0,0625); o binário não fecha o tile debaixo dos degraus (só as pegadas das peças), e o planejador agora também não quando a escada é de objetos: as pontas das escadas que acabam no meio do tile ficam livres. Falta: retângulos de custo 10 dos pés do corrimão (modelos 2–4; passáveis; lidos nas rodadas n18/n31b, não feitos). |
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
| Mistura por níveis: `cChannelUberOutlet`, `cChannelUber`, `cBlendFunction2` (29, 0), `cBlendInterval` | `formats/anim_player.cpp` | fiel | Rodada n02: o peso do canal (`cChannelUber::Weight`: peso do pedido × peso de IK) e a memória do osso (osso sem canal fica como estava) lidos e feitos (`anim-mac.md` §2–3). Rodada n43 (§13.1–13.2): `cBlendInterval::Value` (BV `0xd28880`) é linear e a função de peso do pedido (`CreateWeightBlendFunction` `FUN_00a46c00`: tipo 0, 0,001 → peso, encolhida num pedido curto) lidas e feitas. |
| Fila de coreografia: `cChoreoQueue` (81, 0), `cChoreoRequest` (62, 3), `cChoreoHandler*`, `cAnimatable::OnPreSimulatorTick` | `formats/anim_player.cpp`, `vm/animation.cpp` | fiel | Rodada n02: ordem do quadro (pedidos → tratadores → outlets, IK no segundo passo), regras de expiração e a saída dos expirados lidas (`anim-mac.md` §1–2). Rodada n43 (§13.3–13.4): o fim da ação (o Animate Sim acaba quando o pedido começa a sair; o pedido segue saindo com os eventos enquanto o próximo entra), o 9997 só com saída, a sincronia do byte 8 com os fins da primitiva anterior (140 ms) e a pressa do dado de objeto 85 lidos (BV e base) e feitos. Sobram: o nível −1 para o que não tem função de peso própria (objetos, "grasp"), a sincronia dos overlays, a pressa da locomoção. |
| Locomoção: `cActiveAnimLoco::CreateStepCycle`, `PushTurnBlendCycles`, `StopLocomotion`, `cLocoCycle` (63, 0) | `vm/locomotion.*`, `vm/routing_loco.cpp`, `formats/anim_player.cpp` | inferido | Rodada n14 (`anim-mac.md` §8): relógio das fases (`ComputeLocoStartTicks`, entrega do `FollowOneStep` na saída do pedido, N = 25/70), trocas do `cLocoCycle` (`+0xec`, cosseno), pé da parada (`0x178`), rotas curtas e `StopLocomotion` lidos e feitos. Rodada n43 (§13.6): a janela do `ComputeLocoStartTicks` com os fins da primitiva anterior e o pedido novo ao replanejar andando, feitos. Seguem inferidos a troca centrada (±b/2), a rampa de velocidade (`CreateVelocityBlendFunction`, parâmetros não seguidos) e a parada fraca. |
| IK: `cActiveAnimIK` (28, 5), `cIKChain` (47, 0), `cIKStrategy7DOF`, `ComputeIKRotationSolution` | `formats/anim.cpp` (`applyIk`) | fiel | Rodada n02: o solucionador de duas juntas do binário (dobradiça em X, giro pelo polo, 65 %/75 % da torção) e a mistura de alvos pelo contato conferida (`anim-mac.md` §4–5). Sobras: ordem de Euler da torção, contato 0. |
| Olhar: `cActiveAnimLookAt` (29, 1), `cIKStrategyLookAt` | `app/look_at.*`, `formats/anim.cpp` (`lookAtIkRotation`) | fiel | Rodada n34 (`anim-mac.md` §12): o `AnimUpdate` (BV `0xd41e04`) portado — estados `0x26AD`–`0x26B2`, rampa de velocidade, limites −51/+36, 70, 40, a duração contada da chegada, "no early exit", a torção da coluna com "including Spine", o peso entrando e saindo — e as duas cadeias da estratégia de olhar (a cabeça pela solução de translação, BV `0xd46cdc`, também nos 338 ANIM com essa cadeia; spine0..neck por 0,2/0,08/0,05/0,1 da torção, BV `0xd46568`). Inferidos: o `root_rot` como quadro da torção, a rotação de repouso da cabeça, a forma linear do blend. Faltam os slots (byte 14), a câmera, os eventos "look_state" (9901–9906) à árvore do olhar e o teste de alcance sem 0x40. |
| Eventos: `cEventChannel`, `cActiveAnimEvent`, árvores de evento | `vm/animation.cpp` | fiel | Rodada n23 (`anim-mac.md` §10): os 5 parâmetros do `cCustomEventHandler2::HandleEvents`, os temps postos de volta e o 9997/9998/9999 também no Animate Sim. Rodada n34 (§12.1): o construtor do `cActiveAnimEvent` (BV `0xd3a290`) confirma os tipos (3 = 9999 no fim do pedido, 6 = 9998 no fim de cada volta, 7 = 9997, −1 abortado) e o 9998 vem a cada volta dos overlays em laço (também os de Sim até parar, que agora andam na VM). Sobram: o tempo do evento do `RunTree`, o erro de uma árvore de overlay (descartado). |
| Cinemáticas: `cCinematicScene::ReadFrom` (23, 9), `TryCinematic` | `formats/cinematic.*`, `app/cinematic_*` | inferido | Rodada n23 (`cinematics-mac.md` §8): o andar do ator de origem, o "ObjectHiding" e o espelho pelo temp 3 lidos. Rodada n34 (§9): sem 0x04 a cena começa no Animate Sim de um ator (`try_animate_sim`, BV `0x69e6d8`; a espera de 1 s saiu), o spot aponta pelo −Z do nó (`cSpotLight`, BV `0xc9f3ac`), o Start/Stop mexem no corte por quadro do relógio de animação (`cCustomAnimTimer2`), cujo efeito na velocidade não foi seguido (a velocidade normal forçada segue nossa); falta o fade da câmera de cinema. |

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
| Colocação: `cTSUserToolObject` (93, 17), `TestPlacementCommon`, `TestPlacementWithRespectToWalls/Objects`, `AutoRotateObjectToMatchWalls` | `vm/placement.*`, `app/buy_mode.cpp` | inferido | Rodada n20 (§7.24): a colocação não usa `HasSupport` (só a escadaria, `CeilingTooHighForObject` `0x30`, feito); feitos `IsFlat` (`0x12`) e os quatro quartos com piso pelo relevo do lote (`VM::placementLot`), conferidos em 12.930 objetos dos lotes (`s2pkg placement-sweep`); o od 4 (bit 0 no chão) não recusa nenhum, mas o `0x2c(1)` do objeto `0xFBA4DA53` segue não lido; rodada n37 (§7.25): a piscina sob o tile (`FUN_00164c40`, `0x8E`; objetos de piscina passam, inferido); faltam a camada de visibilidade do piso, a árvore "allow intersection", as pegadas dentro do tile e a ferramenta de lareira (`TryPlace` lido, não feito). |
| Desfazer: `cTSUserToolUndoable*`, `cTSUserToolUndoManager` | `app/build_mode*.cpp`, `app/undo_order.hpp` | fiel | Rodada n20 (§7.24): gerente (`0x2807200`: submeter, desfazer, refazer, soltar), estados do undoable e ordem das operações lidos; uma pilha só para Compra e Construção, esvaziada ao entrar no Vivo (feito); desfazer e refazer gravam o lote byte a byte igual (`tests/build_undo.sh`). Rodada n37 (§7.25): o dinheiro é o `cTSUserToolUndoable::AdjustFunds` (vtable `0x68`) = `cTSSimulator::Spend(6, ±custo)`: o desfazer devolve, o refazer cobra sem testar os fundos (feito). |
| Paredes: `cTSUserToolWall`, `nTSWorld::cWallGraph`, `cPartitionManager` (92, 13) | `vm/build_edit_walls.cpp` | fiel | |
| Salas: `cRoomManager::FloodRoomRecurse`, `cWallGraph::SetRoomsPostAttach` | `vm/rooms.*`, `formats/lot_edit.cpp` | fiel | Rodada n20: o `FUN_009a7dd8` não fecha o cômodo, aplaina o retângulo (feito); rodada n37 (§7.25): os IDs novos pelo `SetRoomsPostAttach` a cada parede (o lado esquerdo da linha que fecha o ciclo; o lado cercado ao dividir o lado de fora, pela paridade `FUN_00c1c480`). Inferidos: a direção da aresta nova e os quartos sem piso dos andares de cima fora das salas. |
| Pisos e revestimentos: `cTSUserToolFloorPattern`, `cTSUserToolWallpaper` | `vm/build_edit.cpp` | fiel | |
| Terreno, lagos, piscinas: `cTSUserToolTerrain*`, `cTSUserToolWater`, `cTSUserToolDeckFoundation`, `cTSUserToolUndoablePool` | `vm/build_edit_ground.cpp` | inferido | Rodada n07: o teste de vértice `FUN_009e9550` lido (§7.23); rodada n20 (§7.24): as flags do pincel (`0x2BF7`) vêm de um inicializador estático e o pincel usa o teste do jogo; piscinas: `0x8A`/`0x81`/`0x86` do binário; rodada n37 (§7.25): o bit 7 (objetos, `0x61`/`0x60`/`0x68`), o bit 13 (não é o marcador de deck), o nivelamento das fundações, decks e piscinas (`MayPlaceQuadrilateral`, `MoveFloorElevations` com a volta, `CalculateTopElevation`), os marcadores 3ARY `0x14` gravados e o preço por quarto (feitos). Inferidos: `ConformsToCeiling` (só suportes passam), os tipos do `ObjectMayBeInFoundation`, `ChangeFloorPatterns`/suportes não lidos, a água ligada (`FUN_008b02d4`). |
| Escadas modulares: `cTSUserToolModularStairs`, `FindEnd` | `vm/build_edit_stairs.cpp` | fiel | `FindEnd` fiel; rodada n20 (§7.24): o +0,75 é de deck no nível do próprio tile de cima (os lances de piso levantado não o ganham, como os salvos), a "elevation" é a altura absoluta (`+0x20c`) e a escavação usa o teste de vértice do jogo (`0x2BB7`). |
| Telhados: `cTSUserToolRoof` (48, 8), `cAutoRoofData`, `nTSWorld::cWorldRoof` (47, 10) | `formats/lot_roof*.cpp`, `vm/build_edit_walls.cpp` | fiel | Retângulos do auto-telhado lidos; rodada n07: `IsRoofValid` (tamanho, cantos, objetos, piscina) lido e usado (§7.22); rodada n13: o pré-teste (`IsAnyRoofAtLocation`, `DoesLocationHasClearanceHeight`) e a caixa com o beiral; 245 dos 275 telhados do jogo aceitos (os 30 têm janelas postas depois, que o jogo também recusaria). |
| Portas e janelas: máscaras `_wallmask`, `HideWhenWallDown`, auto-giro | `formats/lot_model.cpp`, `vm/placement.cpp` | fiel | Rodada n36 (`walls-floors-mac.md` §6.2–6.3): cada face pelo nome do `WallCutoutMaterialName`, o UV1 da face com o `tsMirrorWallMask` (`wallMaskMirrorFlag`), o topo com a máscara da face A (`SetCutoutMaterialImpl`), só nós 0 e 2, sem máscara sem buraco (o recorte retangular saiu). Inferidos: o referencial do objeto dos nomes (conferido pelos nomes que existem), o nível pela altura e os lados com recorte (`+0x124`) pelos nomes que existem. |

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
| `cTSPerson::TryFindBestAction`, `FBA_ScoreInteraction`, `FBA_CheckInteractions`, `FBA_RunCheckTree` | `vm/autonomy.cpp` | fiel | O uso da memória de falhas (8 por 9999 ticks) é inferido. Parâmetro 0 = 1 das check trees conferido (n32: `FUN_004c5cb0` com autônomo 1). |
| `TryFind5WorstMotives`, `Set Motive Change`, laço de `cTSPerson::OnSimulatorTick` | `vm/motives.cpp` | fiel | |
| Menu de pie: `cEdithObjectTestSim::TestInteraction`, `AppendInteractionsForAuto`, `sMakeNewMenuItem` | `app/pie_menu.cpp` | fiel | Rodada 19. |
| Vieses (dono, memória, relação, clima) | `vm/autonomy.cpp` | fiel | No base, só o de dono pesa. |
| Registros de objetos da pessoa: `cTSPerson::cObjectRecord`, `+0x60c` | — | faltando | O `OnCleanup` usa; não modelado. Rodada n41 (`primitives-mac.md` §4j): lidos em parte, são o contador de uso (od 62) e dizem quando a ação e a sub-ação acabam (`StackJustPopped`); quem os cria não foi achado. |

`cTSPerson` tem 238 métodos e 63 nomeados: é a classe de jogo mais lida
depois do `cTSObject` (409, 129).

### 3.8 Motivos, desejos, memórias e relacionamentos

Código: `src/vm/motives.*`, `environment.*`, `wants.*`, `src/formats/wants.*`,
`src/app/wants_panel.*`. Docs: `wants-mac.md`, `primitives-mac.md`.

| Classes e funções do jogo | Onde está | Estado | Notas |
|---|---|---|---|
| `cTSMotives` (17, 4): `GetMotiveRef`, `GetOldMotiveRef`, `UpdateMood`, a atualização de 60 ticks (vtable 0x10); `cTSEnvironmentScoreManager` | `vm/motives.cpp`, `vm/environment.cpp`, `formats/environment_catalog.cpp` | fiel | Rodada n17 (`motives-mac.md` §3–§5): decaimento `dado/24/30` a cada 60 ticks, trava ±100, conjunto antigo, humor do base com o dado 163 sem trava, leitura arredondada sem trava, Set Motive Change acumula. Rodada n33 (§8): o ambiente do `cTSEnvironmentScoreManager` (paredes, pisos e objetos por sala e célula de fora, a curva do STR# 0x2BC, o preço do mestre nos multi-tile, a luz do light map de piso pelo app; bate com os 55 Sims salvos a menos da luz) e o passo de 60 pelo tique do simulador (do SIMI). Inferido: `HasModel` de todo objeto, sala −1 fora do mundo; sem host (headless) a luz fica de fora. |
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
| Câmera do CAS: `cCameraCreateASimController` | `render/cas_camera.*` | inferido | Rodada n21: pares spec ↔ localizador (efeitos de câmera do SimAntics do "CAS - Backdrop"), roda, botões segurados e o passo do zoom segurado (× 50 / Δt) lidos. Inferido: a direção do zoom (rodada n39: convenções da GZ e ordem lidas, o binário gira nos eixos da câmera; com o localizador como lido isso sobe acima da cabeça, a rotação do `SetActive` não foi achada; o app segue nos eixos do mundo). |

### 3.12 Desenho

Código: `src/render/*`, `src/formats/lot_model.*`, `lot_lightmap.*`,
`model.*`, `gmdc.*`, `txmt.*`, `effects.*`, `lighting_config.*`,
`src/app/lot_lighting.*`, `lot_effects.*`, `plumbob_effects.mm`, `lot_water.*`. Docs:
`shaders-mac.md`, `lighting-mac.md`, `effects-mac.md`, `roof-fence-water-mac.md`.

| Classes e funções do jogo | Onde está | Estado | Notas |
|---|---|---|---|
| Materiais e shaders (caminho DX8 dos `.matShad`) | `render/shaders.metal`, `renderer.cpp`, `formats/model.cpp` | fiel | Rodada n22 (`shaders-mac.md` §13.2–13.4): o passe de bump do DX8 (1ª direcional e 1ª ponto/spot por pixel, o resto no vértice; sem tangentes no Mac), o recorte da pia (`CreateAndRegisterObjectCutoutMaterial`, o mapa `+0xe8` do `cObject`) e o `celShading.matShad` inteiro (linha da rampa, borda, especular, contorno). Inferidos: a força da conversão altura→normal (não está no binário de Mac) e os padrões de registrador do bump sem tangente. |
| Luz e light maps: `cLightingManager` (88, 31), `cLightMapManager`, `cDiffusionManager`, `cFloorColorGrid` (29, 17), `cWallDiffusionManager` (29, 13), `cDiffusionWall` (24, 10) | `formats/lot_lightmap.*`, `lot_diffusion.*`, `app/lot_lighting.*` | fiel | Rodada n16 (`lighting-mac.md` §10): grade de piso (paredes que bloqueiam a luz, portas inclusive, fontes por sala e por andar aberto, varreduras, cópia pela guarda 4×4), paredes com amostras próprias (retas, luz das fontes, piso trazido, propagação, cantos escuros, células 8×24), cômodos sem luz, fontes só com `cDiffusionLightExtension`, irradiância pelos `Lookup*Diffusion`. Portais pelo construtor do jogo (`0x7149fc`: lado pela direção da luz, posição, parede do `L`). Rodada n35 (§11): Delta com sinal pelo `MaxAbsElement` e conjuntos por sala de luz; a fonte de difusão de um portal na própria luz (sala do tile da janela), `doorOrArchDirectMultiplier` 0, `FindPortalSuppression`, práticas "LM_PortalDirect"/"LM_PortalSpill"; difusão de paredes só nos grafos 3/8/10/11 e piscina; o piso das paredes pela grade (passada otimizada, a que o jogo roda com o `LightingOptimizations` do `Graphics Rules`). Inferidos: a altura da luz, as retas montadas das faces, o `+0x20` da supressão (andar), a cor das células de parede fora da difusão, quais 8 luzes ficam num objeto. |
| Sombras: `nTSSG::cShadow` (32, 25), `cShadowManager` (`DoMessage`, `UpdateShadowsAdjacentToWall`, `UpdateLevelShadows`), height map do sol | `formats/object_shadow.*`, `formats/lot_lightmap.cpp`, `app/vm_objects.*`, `render/` | fiel | Rodada n06: os planos GUOB lidos; rodada n35 (`lighting-mac.md` §11.6): o "LM_sunHM" feito; rodada n40 (§12): as opções (bit 2 = `guob`; o estilo 3 são as GUOB), o `UpdateGUOBs` do base 1.0 (chão sempre, `wallshadow` com paredes não baixadas, os quatro lados com paredes erguidas no modo 2 onde há parede em todo o lado do retângulo de tiles do objeto, girados pela direção) e quem recalcula (`OnMove`, `SetIsVisible`, paredes, modo e andar) feitos, a direção e a cor da sombra dos Sims conferidas. Inferidos: a caixa de um GMDC sem bounding mesh (os vértices), o andar visível, o volume do espelho fora; não lidos: a vtable `0x1f4` do base no `UpdateGUOBs` e o `0x1a0` que zera o estilo 3; não feito: o recálculo pelas salas de luz. |
| Paredes e pisos: `cWallGeometryNode`, `cFloorGeometryNode` (32, 5), `cFloorManager` (31, 6) | `formats/lot_model.cpp` | fiel | Rodada n19 (`walls-floors-mac.md`): nós de parede (espessura, −0,01, span: meia parede 1,025, sótão telhado − 0,25), chanfros, 3 colunas, papel `u` 0/0,5/1 e `v` do topo ou do chão, tampas, topo com UV (x, y) sem light map (`2 × wall-top`), encontros; pisos um triângulo por quarto com UV (y, x) e a rotação 2/4/8. Rodada n36 (§6): a borda dos pisos (`cFloorThicknessGeometryNode`: faixa de 0,15 nas arestas da borda de cada andar ≥ 1, material `FloorThickness`), o UV das máscaras por face com o espelho, a parede só para os `PartitionType` 0 e 3 e com um tile ao lado no lote. Inferido: o +0,01 do UV1 do topo no lado A. |
| Telhados: `nTSSG::c…Roof`, `nTSWorld::c…RoofType`, classes `c{Even,Odd}Triangle{First,Second}…` | `formats/lot_roof_game.*`, `lot_roof.cpp` | fiel | Rodada n13 (`roof-fence-water-mac.md` §1.4–1.5): os tipos 1–6 como o jogo (grades, `ConnectHeightField`, triângulo escondido sob outro telhado, telhado com telhado nos vértices, célula do atlas por classe, faixas); inferidos: a inclinação global (0,5) e a ordem dos getters das texturas das faixas; sem portar: o `IsTriangleCenterSupportedByOtherRoof` do anel da mansarda; diagonais e pagodes ainda no OpenTS2. |
| Cercas: `cFenceGeometryNode` (57, 0), `cFenceManager` (41, 0) | `formats/lot_model.cpp`, `formats/lot.*` (marcadores) | fiel | Rodada n25 (`roof-fence-water-mac.md` §2.1): trilho, poste, arcos e modelos (`FenceGeometryResource`: a cerca padrão para estilo sem cerca, nada no diagonal sem modelo) como o jogo; o +0,75 é o marcador cru 4 do 3ARY `0x14` (nenhum produto do jogo base), feito no trilho (duas pontas), no poste (o ponto) e na meia parede (nó 5); cerca nenhuma muda nos modos de parede (o modo baixo só tira os nós de meia parede). Rodada n36 (`walls-floors-mac.md` §6.4): meia parede é o estilo de `PartitionType` 3 (o `ishalfwall` da cerca do catálogo; a cerca coberta não vira parede), o segundo teste do poste de meia parede, trilhos e postes de meia parede fora no modo baixo. Inferidos: quem passa o `ishalfwall` ao tipo 3, um poste de arco em cada ponta, o estilo 4 como palco de expansão. |
| Água e piscina: `cWaterGeometryBuilder` (35, 0), `cPoolGeometryNode` | `formats/lot_water.*`, `lot_model.cpp` | inferido | Rodada n06 (§3): alfa por profundidade (`Terrain.ini`), só o reflexo, leito e praia pintados como o binário; piscina como o caminho DX8 do Mac (ondas, reflexo, camadas de fundo); falta a passada de bump e as cáusticas. |
| Efeitos: `cTSEffectsManager` (55, 1), `cTSParticlesEffect`, `cTSMetaParticlesEffect`, `cTSSequenceEffect` | `render/particle_system.*`, `render/particle_layer.*`, `app/lot_effects.*` | fiel | Rodada n15 (`effects-mac.md` §4e): descrição, bits, emissão (`ApplyEffect`, `AddRateCurve`/`Inject`/`Maintain`, começo forte/suave), física em passos de 1/15 s, partículas no mundo salvo `rigid`, meta-partículas (cadeias pelas flags dos componentes, `cTSRWState`, colisores planos, caminhos) e a ordem dentro da camada 7 (`cDisplayList::AddGeometry`) lidas e seguidas. Rodada n38 (§4f): LOD pela distância da câmera (`SetLOD(float)`, faixas e escalas dos componentes), iluminação (bit 23 → luz `environmentCube`), colisores das partículas (`plane`, `floors`, `walls`) com efeitos de colisão e morte, a filha da meta que morre e o `<base>_death`, `cTSSequenceEffect` (laço, começo/parada forte, sobreposição, espera do filho), cadeias de estado pelo `SwitchDescription`, trator, `screw`/`wiggle`/`stretch`, o giro "view-relative"; densidade e vento confirmados (1 e zero). Inferidos: as paredes e o quarto do piso do colisor (consultas do `WorldDB` não lidas), o terreno como piso, o padrão do `effectLODSoftChange`. Faltam: terreno das partículas e metas, o atrator do alvo, o `screenBloom`. |
| Objetos e Sims: `nTSSG::cObject` (140, 20), `nTSSG::cPerson` (50, 5), censura | `app/vm_objects.cpp`, `formats/sim_censor.*` | inferido | Objetos animados escolhidos pela malha de repouso. |
| Espelhos: `MirrorReflectionMaterial`, alvo `ScreenReflection` | `render/renderer.cpp` | fiel | Rodada n22 (`shaders-mac.md` §13.1): um observador por espelho (`cViewerRefNodeRecursiveImpl`), todos no mesmo alvo, cada um no seu retângulo e no stencil dos quads (`cApplyStencilStateForOverlappingReflections`: 1 no próprio, 0 nos outros); a censura da vista mostra o reflexo. Nosso: no máximo 8 por quadro; o filtro de visibilidade do observador (máscara `0x07440100`, `InvisInReflections`) não foi resolvido. |

### 3.13 Câmera e UI de jogo

Código: `src/render/camera.*`, `src/app/camera_*.{hpp,cpp}`,
`live_ui.*`, `live_panels.*`, `options_panel.*`, `game_cursors.*`,
`game_flow.*`, `pie_menu.*`, `src/formats/camera_config.*`, `ui_*.{hpp,cpp}`.
Docs: `camera-ui-mac.md`, `ui-live-mac.md`, `game-flow-mac.md`.

| Classes e funções do jogo | Onde está | Estado | Notas |
|---|---|---|---|
| Câmera: `cTSCameraSmoothSims1Controller` (58, 9), `cCameraLive` | `render/camera.*`, `app/camera_*` | fiel | Rodada n39 (`camera-ui-mac.md` §1.11): teclado pelas velocidades suavizadas do `DoMessage` e os pedidos do `Update` (mover `0x9b66110a` → `UpdateSubjectMotion`, girar a partir do rumo atual, zoom pela roda com `keyboardzoomscale`), o arrasto do botão direito como velocidade, a borda da janela pelos passos `0x3b6c780f`/`0xbb6c7808` (`sqrt(fov·30)`, 0,0032/0,002, contador de repetição, margens 4/16, 1,25), o pivô e o pitch em 7 passos, `rotateCameraSims1Mode` (troca `,`/`.` e nega o dx do mouse). Inferidos: as setas do mapa de dispositivo (o acelerador as consome), a amostra de altura do piso. |
| Vista de cima: `cTSMainToolbar::SetTopDown`/`SetMode`, a câmera "Build" do `cCameraLive` | `app/camera_mode.hpp`, `LotCamera::setDefinition`, painéis de Comprar e Construir | fiel | Rodada n11 (`camera-ui-mac.md` §1.7): T só em Comprar e Construir, o flag lembrado no Viver, a troca copia pivô, rumo, distância e pitch. Rodada n39: o `pivotheightabovefloor` do Build é o 1,2 do `cCameraLive::Reset` (BV `0x6a3c70`). |
| Seguir o Sim: `cTSMainToolbar::AutotrackSim`/`CenterOnSim`, `cTSCameraSmoothSims1Controller::Update`/`HandleRequest`, `cTSWinProcSimChooserThumbnail` | `app/camera_follow.*`, `live_ui.cpp` | fiel | Rodada n11 (§1.8): zona morta medida do alvo do pivô ao ponto do piso do Sim, os movimentos do jogador acabam com o seguimento, o clique direito centra e segue, o esquerdo na selecionada centra e para. |
| Fade de objetos: `cTSSGSystem::ObjectFadeParams`, `cObject::UpdateVisibilityAndRendering` | `app/object_fade.hpp`, `lot_actors.cpp`, `Renderer::setInstanceAlpha` | fiel | Rodada n11 (§1.10): "Obstructs View" (od 40 bit 3), alfa = (profundidade/distância)^queda. A câmera de cinema (`FUN_0065deb0`) e o `ParentFadeAlpha` não foram feitos. |
| Câmera livre: `cCameraCameraManController` e a base de primeira pessoa | `render/free_camera.*`, `LotCamera::matchView`, `app_view.mm` | fiel | Rodada n11 (§1.9): Tab/Y/Escape, números do `FirstPersonCameras.txt`, velocidade e altura do `Update`. Rodada n39 (§1.11): o olhar com o mouse (sempre ligado, cursor preso, 0,0005/0,001 por pixel), a órbita (`PickOrbitCenter`, a conta ao pé da letra) e a "CameraManNH" do bairro. Inferidos: o FOV de partida, o sentido da órbita, o centro só no piso; a "First" (nenhuma tecla) não foi feita. |
| Menu de pie: `cTSWinPieMenu` (36, 5) | `app/pie_menu.*` | fiel | |
| Velocidade, relógio, dinheiro: `cTSMainToolbar` (89, 10), `cTSWinProcLivePanel`, `cTSSimulator::Simulate` | `app/live_ui.cpp` | fiel | |
| Painéis: motivos, relacionamentos, emprego, desejos (`cTSWinProcPanel*`, `cTSWinProcMotiveBar`) | `app/live_panels.*`, `wants_panel.*` | fiel | |
| Painel de memórias: `cTSWinProcPanelMemories` (21, 0), `cTSWinProcMemoryThumbnail`, `cTSWinProcMemoryDescription` | `app/live_panels.cpp`, `app/wants_panel.cpp` | fiel | Rodada n08 (`relationships-mac.md` §5): miniaturas `0x4906500A` (fundo bom/ruim, ícone com máscara, seleção), descrição `0x4906500C`. Inferido: páginas por setas (o jogo rola), o painel expandido não é desenhado. |
| Fila de ações: `cTSUIActionQueue`, `cTSWinActionQueueIcon` | `app/live_ui.cpp` | fiel | |
| Clique e cursores: `cTSWinSceneGraph::ObjectAtGameWindowPoint` | `app/game_cursors.*`, `lot_view.mm` | fiel | |
| Fluxo do jogo: `cTSGameStateController` (65, 14), `cTSStartupController` | `app/game_flow.*`, `scene_*.mm` | fiel | Rodada n27 (`game-flow-mac.md` §7.5–7.6): o caso Construir é o do dormitório (só no BV; o base 1.0 escolhe Viver ou Comprar); o estado da barra salvo no lote (`cTSMainToolbar::Write/Read`, recurso `0x8B0C79D6`: pausa, andar, modo de parede) volta na entrada e é salvo. Inferido: os lotes 1 e 4 do Tutorial abrem rodando. |
| Diálogos: `cTSUIDialogManager`, `cTSObjectDialog` | `app/dialog_window*.cpp`, `vm/dialog.cpp` | inferido | Rodada n27 (`primitives-mac.md` "Dialog"): `SetParams`, `SetupDialog` (a caixa de fechar fora da fila: o travamento do `0c`), o laço das larguras e o `DoLayout` (ícone e texto em volta) do binário; os 35 tokens do `ParseUIString` do jogo base na ordem e com as regras do jogo. Falta: os diálogos de escolha (`dialog_window_pickers.cpp`), os tokens de inventário e o formatador de números do idioma. |


## 4–7. Planos já feitos

Os planos das rodadas 1 a 42 (§5, §6 e §7, todas feitas) estão em
`docs/inventario-historico.md`, com a numeração de antes. Os achados de
passagem da antiga §4 foram corrigidos nos arquivos de origem.

## 8. Plano de rodadas 43 em diante (as linhas que seguem "inferido" ou "faltando")

Feito em 2026-10-16, com a §7 toda feita (índice 86,8%, 95,5 / 110). As
rodadas vêm das linhas de §3 que não são "fiel" e das sobras de cada rodada
no `TODO.md`. **Uma rodada por vez** (pedido do usuário, 2026-10-16): a
seguinte só depois da anterior entrar no main. Opus com Ghidra (só
leitura). Ordem por impacto no jogo base: primeiro o que se vê em todo Sim.

| # | Rodada | Linhas de §3 | Área |
|---|---|---|---|
| 43 | ~~**Corpo do Sim: transições** — como um pedido de animação passa ao seguinte: a forma dos intervalos de blend (`cBlendInterval::Value`, baserom `0x3436c8`; o app faz entrada e saída lineares), a saída do pedido velho enquanto o novo entra (`CreateTransitionBlend`/`FUN_00a46c00`, `EndActionTicksOfPreviousBlockingPrimitive`; hoje o nível −1 é nosso), a velocidade dos pedidos pelo dado 85 (`OnPreSimulatorTick` `0x6980d8`, vtable `0x34`) e as sobras da locomoção (n14: a janela do `ComputeLocoStartTicks` com o fim da primitiva anterior, a troca centrada ±b/2, a rampa de velocidade `FUN_00a47144`, a parada fraca, o fade de entrada ao replanejar andando, o começo 5/100 ms adiantado)~~ **feita** (n43, `anim-mac.md` §13): o intervalo de blend é linear e a função de peso do pedido foi portada (entrada/saída encolhidas num pedido curto); o Animate Sim acaba quando o pedido começa a sair e o pedido segue saindo (eventos, 9998/9999) enquanto o próximo entra (o nível −1 saiu dos Sims), sem 9997 sem saída, com a sincronia do byte 8; a pressa do dado de objeto 85; a janela do `ComputeLocoStartTicks` e o pedido novo ao replanejar; lidos e não feitos a troca centrada, a rampa de velocidade, a parada fraca e a pressa da locomoção (o 5/100 ms não se aplica) | 3.3 mistura, coreografia, locomoção | animação |
| 44 | **Sentar, resto**: o salto da raiz (`PostSnap`, `cObject::ApplyAnimationOffsets`, `GlobalRootTransPosition`; o `stayInWorld` inferido), a faixa do STR# 0x92 pela altura em metros e a bandeja do bar, o susto com o Sim sentado (o jogo tira do slot antes?), as sobras de IK da n02 (ordem de Euler `0x01020100`, contatos em 0, alvo do outlet do nó) | 3.4 D, F; 3.3 IK | animação |
| 45 | **Diálogos de escolha**: `dialog_window_pickers.cpp` (telefone, arara, aspiração; 10 marcadores), o formatador de números e hora do idioma (`FUN_0006e164`), o `SetBuffer` da caixa de fechar, a pausa dos lotes 1 e 4 do Tutorial, as strings do diálogo de árvore semi-global | 3.13 diálogos | UI |
| 46 | **VM, resto**: os registros de objetos da pessoa (`cTSPerson +0x60c`, a única linha "faltando": quem os cria, trocar o `useCount` calculado), o 109 da sub-ação, o 37/38/40/55 do `UpdatePersonDataFields`, `cTSGlobals`, `cTSDataTables`, as sobras da fila (dado 164, evento 4 do `ActionSkipped`, `0x214`/`0x2e8`, a troca da fila cheia `0x1FC`) | 3.1 fila, dados; 3.7 registros | VM |
| 47 | **Salvar, resto** (a 26 da §6 do histórico): `cTSSaveNeighborhoodController`, `cTSPersistSystem`, o u32 `+0x34`/`+0x38` e o u8 `+0xFD` do NGBH | 3.9 controladores, persist | salvar |
| 48 | **Colocação, resto**: a camada de visibilidade do piso, a árvore "allow intersection", as pegadas dentro do tile, a ferramenta da lareira (`TryPlace` lido na n37), o `0x2c(1)` do `0xFBA4DA53`; terreno: `ConformsToCeiling`, `ObjectMayBeInFoundation`, a água ligada (`FUN_008b02d4`) | 3.5 colocação, terreno | construção |
| 49 | **Rotas, resto**: obstáculos (tile da parte de multi-tile, a caixa sem lista, o `+10` do `WorldOutsideSupport`, o estado 7), a escala 1/16 do slot por versão, os retângulos de custo 10 da escada, o "drop to floor below" (`0x98`), a direção relativa dos tipos 1/2; a quina da mesa do N001_Lot68 (semente 7) | 3.2 obstáculos, slots, objetivos | rotas |
| 50 | **Água, objetos e cinema**: o bump e as cáusticas da piscina, os objetos animados pela malha de repouso (`nTSSG::cObject`), o fade da câmera de cinema (caixa `0x148`, bits `+0x108`), a velocidade da cena (`cCustomAnimTimer2`) | 3.12 água, objetos; 3.3 cinemáticas | desenho |
| 51 | **CAS, resto**: qual aba abre qual painel de rosto, a pose dos traços, a aspiração sugerida, a rotação do zoom (`SetActive`), o gerador de números e os tons 1–4 | 3.11 telas, câmera, corpo | CAS |
| 52 | **Bairro, resto**: a cor do `cTerrainShadeLight` (`terrainShade`), os nomes das flags do FAMI/SREL e as unidades da distância do `hood_bridge` | 3.10 imposters, famílias | bairro |

Cada rodada marca a sua linha "feita" com uma linha do que foi feito, como no
histórico, e põe o que sobrar no `TODO.md`.
