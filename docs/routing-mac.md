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
custo de transição próprio). A célula mínima não foi achada (o `Init` do
planner é chamado por vtable).

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
| 4 | couldn't sit down | |
| 5 | couldnt stand up | |
| 6 | no valid goals | (`CreateBestGoalPointList` vazia) |
| 7 | destination occupied | |
| 8 | no valid chair | |
| 9 | wall in the way | |
| 10 | altitudes don't match | |
| 11 | pre-route failed | árvore "pre route" (OBJf 18) devolveu false |
| 12 | post-route failed | árvore "post route" (OBJf 19) |
| 13 | goal not available | árvore "goal check" (OBJf 20) |
| 14 | blocked by person | §6 |
| 15 | blocked by object | §6, `InitRoute` |
| 16 | interrupted by callback | árvore "along route" (OBJf 22) |

(4–10 não foram rastreados até quem os grava.) Com falha, se a primitiva
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
sentido (`IsLocked`, calculada sob demanda por um callback
`bool(*)(bool* trancado, float* custo, nó, nó, contexto)` que o objeto
registra em `PortalAddToPathPlannerImpl`). No A*
(`cAStarPortalNode::CalculateTransitionCost`, BV `0xba271c`): portal livre
→ o custo dele; trancado → `FLT_MAX`, ou 5000 se o contexto tem o flag
`0x200` (rota que ignora trancas). As portas também põem retângulos de
porta (`AddDoorRect`) e de bloqueio (`AddDoorBlockerRect`, a folha
aberta). Andando, ao chegar no início do portal o roteador roda a árvore
**de entrada 15 ("portal") do OBJD** do objeto do portal com o par de
slots (`TriggerPortalTree`, BV `0x575248`; estado 0x1F → 2), e espera o
portal ficar livre (`DoWaitingForPortalToBecomeFree`) se outro Sim o usa
(dado de objeto 0x3E do portal ≠ 0).

## 9. Comparação com o código atual (`src/vm/world.*`, `routing.cpp`)

| jogo | código |
|---|---|
| quadtree de retângulos contínuos, custos por material/desnível | A* em tiles inteiros com 8 vizinhos |
| obstáculos inflados 1/8 de tile para o corpo | tile inteiro bloqueado |
| portas e escadas como portais com custo de transição | portas abrem a aresta; escadas por tile de patamar |
| caminho suavizado (Laplaciano 0,45/0,325, corte de quinas) | centros de tile |
| A* com custo = comprimento × custo da célula, ruído U(0; 0,01) e heurística com ruído até 3 | A* em tiles sem ruído |
| outros Sims = retângulos de custo 5/7,5/10; bloqueio → pede para sair, espera, re-rota, "CT - Route Blocked" | Sims não se bloqueiam |
| falha grava o dado de pessoa 62 com o código do STR# 0xF1 e toca "CT - Route Failure Feedback" | a primitiva só volta false |

Para o modo sem tela basta o A* em tiles; para o Sim andar como no jogo, os
pontos 1 e 3 importam.

## 10. O que falta

- O tamanho mínimo de célula da quadtree e o custo exato de cada
  `ePassabilityState`.
- Quem grava os códigos 4–10, o `CreateBestGoalPointList`/`EvalTileForGoal`
  /`ScanCone` (escolha dos objetivos).
- O callback de trava/custo das portas (o que o jogo pergunta ao objeto) e
  os valores de custo dos portais.
- O passo do locomotor (`FollowOneStep`) e quando ele diz "bloqueado".
