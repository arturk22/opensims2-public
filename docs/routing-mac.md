# Roteamento: o que o binário de Mac faz

Fontes e convenções como em `docs/primitives-mac.md`. Cobre a arquitetura,
os obstáculos e os custos, o A* (§4), os Sims como obstáculos (§5), a
máquina de estados do roteador com os bloqueios e as re-rotas (§6), os
códigos de falha (§7) e os portais (§8). Classes: `cTSRouteManager`
(monta o mundo de rota do lote), `nPathPlanner::cPathPlanner`,
`nPathPlanner::cQuadTree`/`cQuadTreeCell`, `cAStarSearch` e nós
(`cAStarStartNode`, `cAStarGoalNode`, `cAStarPortalNode`), `cTSRoute`
(a rota de um Sim), `cTSRoutingObjectAggregate` (a máquina de estados que
anda a rota, espera portas e portais).

## 1. Não é uma grade de tiles (binário)

O mundo de rota é, por andar, uma **quadtree de retângulos em coordenadas
contínuas** (`cQuadTree::Init(x, y, largura, altura, célula mínima, ...)`:
o número de níveis é `ceil(log2(lado / célula mínima)) + 1`). Cada obstáculo
é um retângulo (`AddRectangle`) com um estado de passagem
(`ePassabilityState`) e um custo; as células se subdividem onde há
retângulos e se fundem onde não há (`SubdivideCell`, `MergeCellChildren`).
O A* (`cAStarSearch::FindPath`) anda pelas células e pelos **portais**
(portas e escadas, `AddDoorPortal`/`AddPortal`, nós `cAStarPortalNode` com
custo de transição próprio). **Célula mínima = 0,125 tile** (binário: `cTSRouteManager::DoMessage`, na
mensagem 0xB3 de lote carregado, chama `cPathPlanner::Init(xmin, ymin,
xmax, ymax, 0,125, 1,0)` com os limites do mundo; o `Init` guarda os seis
valores e `ResetPathPlannerData` cria uma `cQuadTree` por andar com
eles). Logo depois vêm os pisos, telhados, postes e toldos.

## 2. Obstáculos (binário, `cTSRouteManager`)

- `AddObjectRectangle`/`AddWallRectangle`: cada objeto e parede entra
  **duas vezes**: o retângulo exato com o seu estado/custo e, se ele bloqueia
  (estado > 1), um retângulo **inflado 0,12499 tile** de cada lado com custo
  infinito (`FLT_MAX`). Os 0,125 tile são a folga do corpo do Sim: o centro
  do Sim não pode chegar a menos de 1/8 de tile de um obstáculo.
- Também entram postes, telhados/toldos (`AddRoofRectangle`, `AddPosts`),
  bloqueadores de porta (`AddDoorBlockerRect`) e paredes curvas de piscina.
- Pegadas de objeto que mudam são refeitas antes de cada plano
  (`UpdateDirtyFootprints` no começo de `PlanPath`).

### Custo do piso (`WorkOutTileCostAndPassability`)

```
custo = custo do material do piso (FloorTileRoutingCost), preso em [0,5, 10];
        sem custo no material → o padrão do gerenciador (+0x34)
água (tile parcial ou todo): passagem 7 (parcial) / 15 (impassável), custo FLT_MAX
desnível do tile ≥ 1 → impassável (15); entre 0,5 e 1 → custo = (h − 0,5)·10·2 se for maior
+10 quando as duas checagens finais do mundo derem verdadeiro (inferido: tile
    descoberto/fora de sala; não decifrado)
```

## 3. O caminho (binário, `cPathPlanner::CreateResultPath`)

Depois do A*, o caminho de pontos passa por filtros, nesta ordem:
`PointSpacingFilter` → `CornerCuttingFilter` → `ImpassableRegionBoundaryFilter`
→ `LaplacianFilter(0,45)` → `NewPortalDirectionFilter` →
`LaplacianFilterWeighted(0,325)`. Ou seja: o Sim anda uma linha suavizada,
não de centro a centro de tile. `PlanPath` normaliza as direções dos
pontos de partida para [0, 2π) (−999 = qualquer).

## 4. O A* (binário, BV `0xba019c`, `0xbaa6f8`, `0xba91b0`)

Os nós são pontos (início, fim, nós de portal e nós nas células da
quadtree); `AddNodeToOpenList` (BV `0xba019c`):

```
g(n) = g(pai) + custo_transição(pai → n) + U(0; 0,01)   (ruído uniforme)
       custo_transição ≥ 1,7e38 (FLT_MAX/2) → aresta proibida
h(n) = cNoisyDistancePathHeuristic (BV 0xbaa6f8):
       d = distância euclidiana (x, y) até o objetivo
       a = min(0,1 · g(n), 3)
       d < 3        → h = d
       3 ≤ d < 6    → h = d + U(0; a) · (d − 3)/3
       d ≥ 6        → h = d + U(0; a)
f(n) = g + h      (com peso do nó-objetivo, quando ele tem um: g + h·peso)
nó já aberto: troca o pai se o novo f for menor
```

O ruído vem de um gerador linear (`seed · 0x278DDE6D`, semente pelo
`clock()`), então dois Sims não andam exatamente o mesmo caminho.

Custo de transição dentro da quadtree (`CalculateCostToPointInCell`,
`...InOtherCell`): **comprimento euclidiano do segmento × custo de
travessia da célula** (`GetTraversalCost`: o piso do §2 e o maior custo
dos retângulos que cobrem a célula); um segmento que passa de uma célula
para outra pondera os dois custos pela fração do segmento em cada uma;
célula impassável (custo > FLT_MAX/2) → `FLT_MAX`; cruzar uma porta
(`LineCrossesDoor`) soma o custo da porta (FLT_MAX se trancada).

## 5. Sims e objetos que andam como obstáculos (binário)

Não há "evitação" no passo a passo: cada roteador que anda (Sim, animal,
carro) põe **retângulos de custo** na quadtree, que os outros contornam no
A* (`AddBlockerToPathPlanner`, BV `0x571428`; `UpdateBlockerRect`
recalcula quando ele muda de tile ou direção). Parado, centrado na posição
e deslocado para a frente pela direção (8 direções × 45°):

| retângulo (meia largura) | custo | quando |
|---|---|---|
| 0,375 tile | 5 | só parado (dois, um deslocado ±0,125/0,09375 pela frente) |
| 0,25 tile (adulto, tipo 2) / 0,1875 (tipo 8, menor) | 7,5 | sempre |
| 0,09375 tile | 10 | sempre |

Andando, o retângulo é o do tile atual do roteador ± 0,375 com custo 5.
São custos finitos: um Sim passa por cima de outro se o desvio custar
mais. Quem resolve o encontro é o estado de bloqueio (§6).

## 6. A máquina de estados do roteador (`cTSRoutingObjectAggregate`)

`ProcessGoto` (BV `0x57b500`) chama um tratador por estado (`GRS_State`,
estado em `+0`):

| estado | tratador | | estado | tratador |
|---|---|---|---|---|
| 0/1 | `DoInitial` (planeja) | | 0x0D | `DoWalkingSucceeded` |
| 2 | `DoWaitingForPortal` | | 0x0E | `DoWalkingFailed` |
| 3 | `DoWalking` | | 0x0F | `DoWalkTerminated` |
| 4 | `DoWalkingWaitingForGoalCheckTree` | | 0x10 | `DoFoundPath` |
| 5 | `DoWaitingForPreRouteTree` | | 0x11 | `DoWaitingForPersonToMove` |
| 6 | `DoWaitingForRouteBlockedTree` | | 0x12 | `DoWaitingForPortalToBecomeFree` |
| 7/8 | `DoWaitingForPostRouteTree` | | 0x14/0x15 | `DoNeedFailureFeedback` / `DoWaitingForFailureFeedback` |
| 9/10 | `DoFailedOrSucceeded` | | 0x17 | `DoObjectDeletedWhileWalking` |
| 0x0B/0x0C | `DoPostFailedOrSucceeded` (falhou/sucesso) | | 0x18 | `DoBlocked` |
| 0x19 | `DoBlockedWaitingForGoalCheckTree` | | 0x1A | `DoBlockedGoalStillAvailable` |
| 0x1C/0x1D | `DoPause` / `DoContinueAfterPause` | | 0x1E | `DoWaitingForAlongRouteTree` |
| 0x1F | `DoStartPortalTree` | | | |

**Andando** (`DoWalking`, BV `0x576acc`): a cada tick roda a árvore de
"goal check" do alvo (função 20 do OBJf, estado 4) e a "along route"
(função 22, estado 0x1E) quando é a hora; o locomotor anda o passo e diz
`1` (trecho acabou: próximo trecho, ou o portal (0x1F), ou sucesso 0x0D),
`2` (continua) ou `0` (**bloqueado** → 0x18).

O passo (`nTSSG::cLocomotable::FollowOneStep`, BV `0x6b80e8`) devolve 0
quando o teste de colisão do trecho seguinte falha (vtable `0x41C`), ou
quando o estado da locomoção termina como "bloqueado" (estado 7, ou estado
4 com bloqueador marcado). O bloqueador vem do
`cLocomotable::CollisionCheck` (BV `0x6b49e8`), rodado durante o passo:

```
passo = 50 / velocidade (m/s), mín. 1 tick (200 se velocidade ≤ 0,01)
horizonte = agora + passo · 10,02 ticks, limitado ao fim do caminho
para cada janela [t, t + passo] até o horizonte:
    testa a trajetória prevista contra os outros (vtable 0x478: objetos
    que andam e Sims, pelas posições estimadas deles nesse intervalo)
    achou → bloqueador = ID; se a distância até ele (vtable 0x47C)
    não diminui entre o começo e o fim da janela, para aqui
achou e está a menos de 2 passos (ou no fim do caminho): estado
"bloqueado" (0x438(2)) e o ID fica para o DoBlocked (vtable 0x1A8)
```

Ou seja, o "bloqueado" do locomotor é **previsto** (até ~10 passos à
frente), não um contato: o Sim para antes de encostar. Ao parar, a
desaceleração usa 5 ticks (ou 100 se a velocidade passa de 0,25).

**Bloqueado** (`DoBlocked`, BV `0x5762a4`): pergunta à rota quem bloqueia
(vtable `0x1A8`, ID do objeto):

- ninguém (quina, parede nova): na primeira vez pausa e tenta de novo
  (0x1D); na segunda falha com **15** (`blocked by object`) e feedback
  (0x14);
- alguém e ainda há objetivos (> 1 restante): roda a árvore de goal check
  (0x19) ou vai para `DoBlockedGoalStillAvailable` (0x1A);
- senão falha com **2** (`path not found`).

`DoBlockedGoalStillAvailable` (BV `0x57a8d4`), contador de tentativas em
`+0x3C` (zera quando o bloqueador muda ou se afasta ≥ 0,25 tile):

- o bloqueador sumiu → **1**; não é pessoa → **15**; pilha de rotas > 2
  (rota dentro de rota) → **14** (`blocked by person`);
- é um Sim parado: com até 3 tentativas, **pede para ele sair**
  (`AskBlockerToMove`, vtable `0xCC`, com a prioridade = dado de pessoa
  33 "Priority" − 1, mínimo 1, e se o objetivo do bloqueador está a menos
  de 1,5 tile) → espera (0x11);
- senão **re-rota** (estado 1 = `DoInitial` de novo, com o bloqueador
  agora no planner) enquanto as tentativas são poucas (< 2 a < 5 conforme
  o caso);
- esgotou: na rota de base roda a árvore global **"CT - Route Blocked"**
  (estado 6; falha dela → **14**); senão **14**.

**Esperando o outro sair** (`DoWaitingForPersonToMove`, BV `0x579ffc`):
se o bloqueador está a mais de 1,25 tile → re-rota (1). Perto: na primeira
vez toca **"CT - Route Ask To Move Animation"** e manda o pedido; depois
espera com **"CT - Route Waiting Animation"** de `(1,25 − d)·24` ticks
(mín. 1) enquanto se passaram < 126 ticks do pedido (< 420 se o
bloqueador tem o bit 0x40 do dado 8); passou disso → `DoBlocked` de novo.
`DoWaitingForPortalToBecomeFree` é igual para portais ocupados (espera de
24 ticks, limite 126).

## 7. Códigos de falha (binário + `STR#` 0xF1 "route results")

`DoPostFailedOrSucceeded` grava o código no **dado de pessoa 62
"route result"** (`SetPersonData(0x3E, código)`) e, com saída de debug,
escreve "Route failed because : " + o texto do `STR#` 0xF1 (grupo
`0x7FE59FD0`, entrada = código):

| código | texto | onde (BV) |
|---|---|---|
| 0 | success | — |
| 1 | unknown | `InitRoute`, objetivo sumiu, `DoNeedFailureFeedback` |
| 2 | path not found | `cTSRoute::FindPathWithPlanner` (A* sem caminho), `DoBlocked` |
| 3 | interrupted | `DoWalking`, `DoInitial`, esperas (`ShouldInterrupt`: a interação foi cancelada) |
| 4 | couldn't sit down | nenhum gravador no BV |
| 5 | couldnt stand up | nenhum gravador no BV |
| 6 | no valid goals | `InitRoute`: o plano falhou sem código e há 0 objetivos (com objetivos → 2); gerador de objetivos de cadeira sem slot |
| 7 | destination occupied | candidato ocupado (`EvalTileForGoal` 1 ou 6); slot de "contenção" (cadeira/cama) ocupado (BV `0x474494`) |
| 8 | no valid chair | nenhum gravador no BV |
| 9 | wall in the way | candidato com `EvalTileForGoal` 2, 3 ou 4 (sala errada, parede na linha, parede encostada) |
| 10 | altitudes don't match | candidato com `EvalTileForGoal` 5 |
| 11 | pre-route failed | árvore "pre route" (OBJf 18) devolveu false |
| 12 | post-route failed | árvore "post route" (OBJf 19) |
| 13 | goal not available | árvore "goal check" (OBJf 20) |
| 14 | blocked by person | §6 |
| 15 | blocked by object | §6, `InitRoute` |
| 16 | interrupted by callback | árvore "along route" (OBJf 22) |

Os códigos 6, 7, 9 e 10 são gravados **por candidato** enquanto os
objetivos são gerados (BV `0x478f8c..0x4790dc`: `EvalTileForGoal` → código
pela tabela acima; 0 "fora do mundo" não grava nada; 7 "válido" adiciona o
objetivo, e 6 também se a rota permite empurrar objetos, flag `+0x117`),
então o código final é o do **último candidato recusado** (e só vale se
nenhum objetivo sobrou). O bloqueador do código 7 vai para `+0x1A4`. 4, 5
e 8 (sentar/levantar/cadeira) não têm gravador no motor do BV (restos do
Sims 1; árvores podem gravar o dado 62 elas mesmas). Com falha, se a primitiva
permite (flag "sem árvores de falha" desligada), o roteador passa por
`DoNeedFailureFeedback` (0x14): toca **"CT - Route Failure Feedback"**
(global; o Sim faz o gesto de "não dá") com o sub-motivo em `+0x40`
(7, 8, 0xC–0x11, conforme o ramo acima). Depois a primitiva volta false.
As árvores globais `CT - Route Blocked Animation`, `CT - Route Waiting
Animation`, `CT - Route Ask To Move Animation`, `CT - Route Blocked` e
`CT - Route Failure Feedback` são chamadas pelo nome (strings no binário,
BV `0x0102366c..`).

## 8. Portas e portais (binário)

Cada portal (porta, arco, escada) é um par de pontos de entrada com uma
direção, um **custo de transição** (`cPortal +0x34`) e uma trava por
sentido. O objeto registra o portal em `cTSObject::PortalAddToPathPlannerImpl`
(BV `0xe22a44`): um par de slots de portal do modelo por ligação, e

```
custo = 1 + campo 0x35 do OBJD · escala      (escala: float em DAT_02d86430)
callback de trava = TSObjectPortalLockedCB se o objeto tem a entrada 15
                    ("portal") do OBJD e ela tem a versão de teste (vtable 0x3B8)
```

A escala `DAT_02d86430` (e o vizinho `DAT_02d86b70`, também lido só no
`PortalAddToPathPlannerImpl`) fica no `__bss` e **não tem gravador**: nenhum
`stw/stfs` para esse endereço (direto ou por base + deslocamento) e nenhum
ponteiro para ele nos dados. Ou seja, vale **0,0** em execução e **todo
portal custa 1** (o campo 0x35 do OBJD não tem efeito). Inferido de não
haver gravador; confirmar se um dia aparecer um caminho indireto.

**A trava é a árvore de portal rodada em modo TESTE** (`TSObjectPortalLockedCB`,
BV `0xe208c8`): na primeira vez que o A* de um Sim cruza o portal naquela
busca, roda a árvore da entrada 15 do objeto do portal (ou do líder do
multi-tile) **como teste** sobre o Sim, com os parâmetros: slots de
entrada/saída (`vtable 0x380`), as salas dos dois lados e os pares de
coordenadas; `trancado = !resultado`; o custo é recalculado como acima.
É o "Function - Portal - TEST" das árvores (porta trancada, só família,
portinha de animal). O resultado fica guardado por sentido até a busca
seguinte.

No A* (`cAStarPortalNode::CalculateTransitionCost`, BV `0xba271c`): livre
→ o custo; trancado → `FLT_MAX`, ou 5000 se o contexto tem o flag `0x200`
(rota que ignora trancas). As portas também põem retângulos de porta
(`AddDoorRect`) e de bloqueio (`AddDoorBlockerRect`, a folha aberta).

**Andando** pelo portal: quando o próximo nó do caminho é o começo de um
portal (`IsNextPathNodeAStairStart`/`GetPortalInfo`), o roteador vai para o
estado 0x1F e `TriggerPortalTree` (BV `0x575248`) roda a **mesma entrada
15 de verdade** (não teste) na pilha do Sim, com os parâmetros: os dois
slots do portal, as salas e o nível; o Sim fica em `DoWaitingForPortal`
(estado 2) até a árvore voltar (ela anima a porta abrindo, ou sobe os
degraus com Snap Into) e o roteador segue no fim do portal
(`DoLocomotionContinuationAtPortalEnd`). Se outro Sim ocupa o portal (dado
de objeto 0x3E do portal ≠ 0), espera em `DoWaitingForPortalToBecomeFree`
(animação de espera de 24 ticks, até 126 ticks). Antes de rodar, tira o
bit 0x40 do dado 0x44 do Sim.

## 8.1 Pedir para sair: `AskBlockerToMove` / `MoveOutOfWay` (binário)

`AskBlockerToMove` (BV `0x57d8e8`) recusa se o bloqueador já é o bloqueador
de alguma rota da minha pilha (evita ciclo); senão chama o `MoveOutOfWay`
do roteador do bloqueador (BV `0x57ee3c`), que **aceita** se:

- ele ainda não foi pedido (`IsAskedToMove`);
- os flags de pessoa (dado 159) têm o bit 0x02, **ou** a prioridade dele
  (dado 33) é menor que a do pedido (prioridade de quem pede − 1, mín. 1);
- os dois são roteadores ativos, a minha rota tem > 1 objetivo, estamos a
  ≤ 4 tiles e ele não está, ele mesmo, bloqueado por alguém;
- `CreateMoveOutOfWayDestination` (BV `0x57dcac`) acha um lugar livre perto
  (um gerador de objetivos com raios de 1 a 6 tiles em volta dele, longe da
  minha linha de caminho).

Aceitando, o jogo **empurra uma interação "sair do caminho" (tipo 3) na
fila dele, com a minha prioridade** (`IsMoveOutOfWay` reconhece essas
interações), marca-o como pedido e liga a minha rota a ele; eu fico em
`DoWaitingForPersonToMove` (§6).

## 8.2 Avaliar um ponto de objetivo: `cTSRoute::EvalTileForGoal` (BV `0x475074`)

Para cada ponto candidato (slot de rota, em volta do objeto), devolve um
código e soma penalidades num custo:

| código | quando |
|---|---|
| 0 | fora do mundo |
| 2 | sala diferente da sala exigida (do objeto ou do Sim, conforme o slot) |
| 3 | há parede entre o ponto e o objeto (linha de visão), sem flag que permita |
| 4 | um círculo de 0,3 tile em volta do ponto toca parede (com 0,4 tocando: +10 no custo) |
| 5 | altura do piso diferente da do alvo (> 0,01) ou tile não plano |
| 1 / 6 | ocupado: por um objeto (6, ID devolvido) ou por um Sim/rota (1) |
| 7 | **válido** |

Penalidades no custo (quando o slot permite o caso em vez de recusar):
+10 por parede atravessada na linha, +10 por parede a menos de 0,4, +10 por
sala diferente, +10 por objeto que pode ser empurrado, +5 se há porta no
tile, +5 se o tile é água/terreno especial (consulta do mundo `0x170`). 

### Gerador de pontos de objetivo (BV `0x4778b4`, `0x4788cc`)

O CLSID `0xFBA4DA4F` (IID `0xFBA4DA54`) **é um slot de rota sintético**
(`cITSRoutingSlot`), não um gerador à parte: `CreateBestGoalPointList`
(BV `0x472b64`) cria um, liga as direções pedidas (máscara de 8 bits:
`vtable 0x20(dir)` por bit, ou `0x18` = todas), põe distâncias mín./máx.
1,0 (`0xD0`), o modo de facing (`0x30`/`0x28`/`0x40(valor)`) e devolve o
slot; daí em diante ele é tratado como um slot do SLOT do objeto. O mesmo
serve para `CreateMoveOutOfWayDestination` e para os localizadores de
parede.

A varredura (BV `0x4778b4`, chamada com um callback por candidato):

- para cada direção permitida do slot, anda **em fileiras** do raio mínimo
  ao máximo do slot com passo = resolução do slot (`+0x38`, tiles), e em
  cada fileira de `-largura` a `+largura` do cone (`+0x44`), gerando pontos
  na grade do slot;
- se o ponto cai noutro andar que o do slot (escada/terreno), **+10** no
  custo; cada ponto passa por `EvalTileForGoal` (§8.2) e vai para o
  callback (que adiciona os válidos);
- se há candidatos demais, sorteia uma fração (gerador aleatório do jogo);
  para quando o número de objetivos atinge o máximo da rota (`+0xB8`, e
  `+0xB4` para os de prioridade).

Custo de um objetivo aceito (BV `0x4790xx`): penalidades de §8.2 +
`|distância − distância preferida (+0x30)| · 16 · peso (+0x34)` − `peso ·
0,01 · |desvio lateral|` (desempate); o A* depois escolhe pelo custo total
(caminho + objetivo). Para cadeiras, `ScanConeForSittingGoals` usa o mesmo
laço com o callback de slots de cadeira. O `cTSRoute::ScanCone` que o
symxfer nomeou no BV é outra função (um dump de desejos).

## 9. Comparação com o código atual (main `3b4c4db`: `src/vm/path_planner.*`, `routing.cpp`)

| jogo | código |
|---|---|
| célula mínima 0,125 tile | 1/8 tile (igual; o código marcava como aproximação) |
| custo do portal = 1 + OBJD[0x35]·escala; trava = árvore de portal (entrada 15) em modo teste | custo aproximado; portas sempre abertas para o A* |
| ao chegar no portal roda a entrada 15 de verdade e espera; portal ocupado → espera 24 ticks, até 126 | a VM segura o Sim no portal e o app abre a porta (aproximação) |
| `MoveOutOfWay`: interação "sair do caminho" (tipo 3) na fila do bloqueador, com a prioridade de quem pede, só se a dele é menor (ou dado 159 bit 0x02), a ≤ 4 tiles | pede até 3 vezes e espera, sem pôr nada na fila do bloqueador |
| objetivos avaliados por `EvalTileForGoal` (códigos e penalidades +10/+5, §8.2) | aproximação da busca de objetivos (`routing.cpp`) |
| quadtree, inflação 1/8, filtros, A* com ruído, Sims como retângulos de custo, códigos de falha, "CT - Route Blocked" | seguem o doc |

## 10. O que falta

- O custo exato de cada `ePassabilityState`.
- O teste de segmento do `CollisionCheck` (vtable 0x478) em detalhe.
- Confirmar que nada escreve a escala de portal (`DAT_02d86430`) por um
  caminho indireto.
