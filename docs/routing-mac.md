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

### A pegada de um objeto (binário, rodada 14)

O que um objeto põe no planejador é a **pegada** dele, não o tile
(`cTSObject::AddThisToPathPlanner`, BV `0xaf68e0`, vtable `0x518`): pula
quem a vtable `0x35c`/`0x118` recusa, os tipos `0x12`/`0xd` com od `0x46`,
portas/janelas/escadas (tipos 8, 9, 12) sem OBJD[`0x4b`] = `0x93`; pede a
pegada (`GetFootprint`, vtable `0x100`) e, se o objeto não é "sem
pegada" e `TSObjectGetPathPlannerCostAndPassibility` dá custo e passagem,
chama `cITSRouteManager::AddObjectRectangle` (vtable `0x5c` do gerente)
para **cada retângulo** não vazio (x0 < x1 e y0 < y1), com custo
`od 10 (route penalty) · escala + 1`. O `AddObjectRectangle` põe o
retângulo e, com passagem > 1, o mesmo inflado 0,12499 com custo
`FLT_MAX` (§2). (Com a vtable `0x170(5)` — "allow intersection" — a
passagem vira 3 ou 5 pelo `0x18c`; o detalhe não foi seguido.)

`GetFootprint` (`FUN_00af9e40`) é o `TSObjectWorldSpaceFootprint` (BV
`0x8aa40c`) calculado em (0, 0) e direção 0, guardado no objeto
(`+0x1d4`, válido pela flag `+0x287`), e levado ao lugar e à direção do
objeto pelo `FUN_0191fa14`, que gira cada retângulo pela direção & 6 (0:
(x, y); 2: (−y, x); 4: (−x, −y); 6: (y, −x)). O
`TSObjectWorldSpaceFootprint`:
- pessoa (tipo 2): a caixa de meia largura 0,1 (`TSObjectBaseFootprintWidth`
  = 0,2), cada lado puxado para fora pelos nibbles do dado de pessoa 73 nas
  direções pares;
- objeto **sem** a flag `0x80` do od 8 (STR# 0x8E: "Do NOT Use Maya Model
  Footprint"): a pegada **do modelo** (`AggregateFootprint` com o od 9,
  "footprint index", vtable `0x2c8` → `0x2cc` → o `cObject` do grafo de
  cena, BV `0x658ab8`); se ela existe, é ela;
- senão a caixa do OBJD: meia largura = OBJD[`0x43`]/16 · 0,5 (1 tile se
  0), cada lado puxado para dentro por um nibble do OBJD[`0x4d`]/16, os
  nibbles girados pela direção (`(8 − dir)/2`).

A pegada do modelo é a extensão `cDataListExtension` **"footprint"** do
CRES: uma lista por índice ("0", ...; com od 9 = 0, a de menor nome por
`strcmp`), com `minx/maxx/miny/maxy` e, por tile do modelo "(x,y)", uma
máscara binária de **32 bytes = 16 × 16 bits** (uma linha de 16 bits por
1/16 tile em y, o bit 0 o x mais baixo). O `FUN_006537a4` reduz a máscara a
**8 × 8** (uma célula de 1/8 tile fica marcada se **qualquer** das 4
subcélulas estiver) e o `FUN_00656cc0` faz de cada trecho de bits seguidos
de uma linha um retângulo (linhas iguais seguidas se juntam):
`x = tx + c·0,125 − 0,5 + 0,01 … tx + c'·0,125 − 0,5 − 0,01`, y igual pelas
linhas — no espaço do modelo, encolhido 0,01. Para uma parte de um
multi-tile, o tile "(x,y)" é o da parte no modelo (`vtable 0x150`, não
lido: aqui o deslocamento da parte ao líder girado de volta, inferido) e o
retângulo é relativo à própria parte; uma lista sem esse tile não dá
retângulo. Sem lista "footprint", o `cObject` usa a caixa do modelo
cortada ao tile (±0,5; aqui a caixa do OBJD, inferido).

Exemplo: o "Piano Upright" (`pianoUprightSaloon`): tiles (0,0)/(1,0) o
piano (linhas 1–15 de 16 cheias), (0,1)/(1,1) o banco, só as linhas 0–4
(5/16 do tile, 3/8 depois da redução) e as colunas do meio. Os ossos
`route_piano_*_s` ficam a 1,30 no y do modelo: fora da pegada do banco
(0,5–0,875), mas dentro do tile dele.

O teste por passo do andar e o `Snap` usam a mesma pegada
(`TSObjectCanSetPosition`: a caixa da pessoa contra as pegadas dos objetos,
`VM::pointFree`, `footprintBlocker` em `routing.cpp`).

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
recalcula quando ele muda de tile ou direção).

**Rodada 9 (corrige a tabela antiga):** o teste da vtable `0x98` é do
`cITSPerson` do dono (`QueryInterface(0x5A69D3D5)`, o `cTSPerson` em
`+0x2E0`): `FUN_00ac889c` = **"é humano"** = dado de pessoa 186 (espécie,
`cTSPerson +0x816`) == 0. Não é um filtro de "quem pode bloquear": decide
só a **forma** dos retângulos. No jogo base não há espécie (todos humanos).

- **Humano** (`0x98` verdadeiro): três retângulos centrados na caixa do
  roteador (`+0x54..+0x60`, em 1/65536 de tile), sem olhar a direção:

  | meia largura extra | custo | quando |
  |---|---|---|
  | 0,375 tile | 5 | só parado (`+0x66` == 0) |
  | 0,1875 tile | 7,5 | sempre |
  | 0,0625 tile | 10 | sempre |

- **Animal** (só BV): pela `GetSESpeciesFlag` (`0x54`: espécie 1 → 2, 2 →
  4, 3 → 8): pares de retângulos deslocados para a frente e para trás pela
  direção (cos/sin × 0,09375/0,125, ou ±0,5 para o corpo comprido),
  meias larguras 0,375 (custo 5, parado), 0,25 (7,5) e 0,125 (10) na
  espécie 1, menores (0,1875/0,0625) nas outras; outra espécie: nada.

Ou seja, o coelho social (uma pessoa de espécie 0) **é** um obstáculo de
custo no binário como qualquer Sim: o "blocked by person" contra ele vem do
estado de bloqueio (§6), não de faltar o retângulo.

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
frente), não um contato: o Sim para antes de encostar. **Corrigido na rodada
14** (front "walking2", vtable do `cLocomotable` lida: `0x41c`
`LocoPlaceFromSceneGraph`, `0x428` `ComputeTurn`, `0x42c` `CreateLocoRequest`,
`0x430` `FollowOneStep`, `0x434` `EndFollow`, `0x438`
`InterruptActiveLocoState`, `0x43c` `CopyActiveLocoStateToEndingLocoState`,
`0x440` `MarkActiveLocoStateAsCompleted`, `0x44c`–`0x454` `Begin*Path`,
`0x478` `EstimateWhenPathWillBeBlocked`, `0x47c` `EstimateDistanceToBlocker`):
o "bloqueado" do `CollisionCheck` é `InterruptActiveLocoState(2)` (BV
`0x6b4fa0`): com velocidade > 0 ele chama `cActiveAnimLoco::StopLocomotion`
(BV `0xd2f9b4`, "parada forte"), que acha o clipe de parada do pé em que o
ciclo está (porção 0 ou 0,5) e o toca de onde o Sim está, com a velocidade
caindo numa função de mistura sobre a distância da parada; o estado vai a 6
e só passa a 7 ("bloqueado", `FollowOneStep` devolve 0 → `DoBlocked`) quando
esse pedido termina. **A desaceleração é esse clipe de parada.** Os 5/100 do
`FollowOneStep` não são desaceleração: quando a fase ativa entrega para a
seguinte (`CopyActiveLocoStateToEndingLocoState`, a fase velha acaba de
tocar como "ending"), a próxima `Begin*Path` começa em agora − 5 ms (ou
− 100 ms com velocidade > 0,25), para as duas se sobreporem; isso só
acontece se `EstimateWhenPathWillBeBlocked` não vê bloqueio (e não com
caminho ≤ 0,5 e ≥ 40 ms de clipe ainda).

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

**Rodada 13, os ramos exatos do `DoBlockedGoalStillAvailable`** (lidos no
BV; contador `+0x3c` incrementado a cada entrada, 1 com bloqueador novo ou
que andou ≥ 0,25; "rota de base" = a pilha de rotas tem só esta; `IsRouting`
= o bloqueador tem rota corrente, mesmo esperando):

- **bloqueador sem rota** (parado): dormindo (`cTSPerson::IsSleeping`) →
  re-rota com contador < 2, senão falha 15; acordado: contador < 2 e ele
  saiu do tile anotado (`route 0x1d4/0x1d8`, mapa ID → tile) → re-rota;
  contador ≤ 3 → `AskBlockerToMove` (aceito → 0x11, espera); senão
  `TryGetSelfOutOfWay` (uma vez por rota: contador da rota `0xd8/0xdc`) →
  re-rota; senão "CT - Route Blocked" (rota de base) ou falha 14;
- **bloqueador roteando e bloqueado por mim** (os dois se bloqueiam): fora
  da rota de base → 14; `TryGetSelfOutOfWay` com contador < 3 → re-rota;
  senão, contador > 4 → "CT - Route Blocked"/14; ele saiu do tile → re-rota;
  senão `DoPause` (0x1C: "CT - Route Blocked Animation" com o bloqueador e o
  número de pausas; `DoContinueAfterPause` → `DoInitial`);
- **bloqueador roteando, não por mim**: contador 1 → pausa; 2 e ele saiu do
  tile → re-rota; senão "CT - Route Blocked"/14.

`TryGetSelfOutOfWay` (BV `0x57e9ac`): os dois no mundo e a ≤ 4 tiles →
`CreateMoveOutOfWayDestination(eu, bloqueador, 0)` e uma rota empurrada
para o destino (GotoRoutingSlot), depois a rota original de novo. Na espera
(0x11) o bloqueador não é reavaliado: só sai com ele a > 1,25 tile
(re-rota) ou com o tempo (`DoBlocked`).

**Esperando o outro sair** (`DoWaitingForPersonToMove`, BV `0x579ffc`):
se o bloqueador está a mais de 1,25 tile → re-rota (1). Perto: na primeira
vez toca **"CT - Route Ask To Move Animation"** e manda o pedido; depois
espera com **"CT - Route Waiting Animation"** de `(1,25 − d)·24` ticks
(mín. 1) enquanto se passaram < 126 ticks do pedido (< 420 se o
bloqueador tem o bit 0x40 do dado 8); passou disso → `DoBlocked` de novo.
`DoWaitingForPortalToBecomeFree` é igual para portais ocupados (espera de
24 ticks, limite 126).

## 6.1 Andar ao longo da rota: o locomotor (`nTSSG::cLocomotable`, binário, rodada 13)

Lido no BV (`BeginFollow` `0x6b5770`, `FollowOneStep` `0x6b80e8`,
`OnPreSimulatorTick` `0x6b8fec`, `LocoPlaceFromSceneGraph` `0x6b2ee0`,
`ComputeTurn` `0x6ae2e0`, `Begin*Path`). O `BeginFollow` é muito longo; o que
segue é o que foi lido com segurança, e o resto está marcado.

**O Sim anda pela animação, não por uma velocidade fixa.** A rota do planner
vira uma **curva** (objeto COM `0x1C1D06AD` em `+0x18c`; vtable `0xc` põe um
ponto, `0x54` o ponto num parâmetro, `100` o comprimento) e a animação de
locomoção (o "active anim loco" do coreógrafo, `+0x1a4`) corre sobre ela. A
cada passo o locomotor pega a **transformação da raiz** da animação
(vtables `0x198`/`0x1a0`) e chama `LocoPlaceFromSceneGraph`:

- posição = a translação da raiz, com z = a altura do piso naquele ponto
  (`FUN_` mal nomeada `HasAnyStage`);
- `TSObjectCanSetPosition(sim, p, 0, 0)`: passou → `SetPosition(p)` e a
  orientação = o yaw do quaternion da raiz (vtable `0x2a0`);
- não passou: se o Sim está preso (vtable `0x2e0`) e nem a posição atual
  passa, põe assim mesmo; senão procura nos erros o de código **0x0C**
  (cruza objeto), guarda o ID dele como **bloqueador** (`+0x258`,
  `SetBlockingObjectID`) e o passo falha (→ "bloqueado", §6).

**A curva** (`BeginFollow`): cada trecho da rota é dividido em pedaços de
**0,2** (`n = ⌊|trecho| / 0,2⌋` pontos intermediários, z de cada um pela
altura do piso). O primeiro ponto é avançado de `min(d · 0,3333, 0,25)` na
direção em que o Sim olha girada pelos passos de 45° do `ComputeTurn`
(vtable `0x428`, que devolve `d`, a distância até o primeiro ponto; a
rodada 13 "walking" corrigiu a leitura antiga, "velocidade v, só andando":
ver "A saída" abaixo). A direção final é arredondada para **múltiplos de 45°**
(`⌊h / (π/4) + 0,5⌋ · π/4`, em `+0x1c4`).

**Viradas** (`ComputeTurn`): o ângulo entre a direção atual e a desejada,
normalizado em [−π, π], vira um número de passos de **45°**: enquanto
< −40°, −1 e soma 45°; enquanto > 40°, +1 e tira 45°. Ou seja, até ±40° não
há animação de virar (a curva absorve); acima disso, viradas em passos de
45°. No começo, com o primeiro trecho curto (≤ 0,25) o `BeginFollow` testa
o papel **9** do conjunto de locomoção (vtable `0x28(9, −1)`: na string de
viradas do conjunto global o décimo nome, **`stepShort`**; corrigido na
rodada 13 "walking", antes lido como o virar no lugar) e os ângulos contra
π/2 para decidir se dá um passo curto em vez de começar a andar (detalhe
não fechado).

**Fases** (`tLocoState` em `+0x188`, estado espelhado em `+0x330`): o
`BeginFollow` monta até três caminhos guardados: o **passo curto/virada de
saída** (`+0x190`), o **caminho para a frente** (`+0x19c`) e a **virada do
fim** (`+0x1a0`, para olhar na direção final). `FollowOneStep` (chamado no
`OnPreSimulatorTick` e pelo `DoWalking` do roteador):

| estado | o que faz |
|---|---|
| 1 | cria o pedido de animação (vtable `0x42c`, `CreateLocoRequest`) e o põe na fila do coreógrafo no tick de início; → 2 |
| 2 | animação correndo; quando o pedido termina (vtable `0xfc`) → 3 |
| 3 | próxima fase: `+0x190` → `BeginShortStepPath`, senão `+0x19c` → `BeginCachedForwardPath`, senão `+0x1a0` → `BeginCachedEndTurnPath` (→ 1, começando no tick atual, ou no fim da fase anterior se ela acabou há menos de N ticks; N = 25 com velocidade ≤ 0,25); nada mais → 4 |
| 4 | **chegou** (devolve 1, "FollowOneStep - Done") |
| 6/7 | **bloqueado** (devolve 0) |

Cada fase zera a velocidade/ciclo (`+0x1b4`/`+0x1b8` = −1) e passa a
direção (`+0x1c0` ← a direção do fim da fase anterior). Ao parar antes do
fim (bloqueio previsto) o passo toca um clipe de parada (§6, rodada 14: os
5/100 ms são o começo antecipado da fase seguinte, não uma desaceleração).

**Velocidade e estilo de andar** (`BeginFollow` e a velocidade do conjunto,
`cLocoSet` vtable `0x3c` = `FUN_00d0c444`, lidos na rodada 13 do front
"walking"): os 20 pesos em `+0x1dc` são os dos **ciclos de andar alt0–alt19**
(no conjunto global: alt0/alt1 normal, alt2 sexy, alt3 grávida, alt4 feliz,
alt5 deprimido, alt6 bravo, alt7 saltitante, alt8 furtivo). Os rótulos dos
dados de pessoa (STR# `0xC8`): 112 "LocoWeight 0", 113 "LocoWeight 1", 114
"LocoPersonalityIndex", 115 "LocoPersonalityWeight", 116 "LocoMoodIndex", 117
"LocoMoodWeight". `m = clamp(117/100)`; `w0 = clamp(112/100)·(1−m)`,
`w1 = clamp(113/100)·(1−m)`; se 114 ∈ 2..19, `w[114] = clamp(115/100)·(1−m)`;
se 116 ∈ 2..19, `w[116] += m`. Exemplos nos dados: "Interaction - Sneak
Here" põe 116 = 8 e 117 = 80 (o furtivo), "Skip Here" 7 e 75 (o saltitante),
"Locomotion - Reset Walk to Default" zera 114–117. A velocidade: para cada
`k` com `k = 0` ou `w[k] > 0,01`, `v[k]` = velocidade (ANIM `velocity` ×
1000, m/s) do recurso do tipo `100 + k` do conjunto; `v = Σ v[k]·w[k] /
Σ w[k]` (só os `w[k]` com `v[k] > 0`); sem nenhum, `v[0]` (1,75 se nem isso).
O jogo **mistura** os ciclos pelos pesos (`cLocoCycle`); desde a rodada 14 o
opensims2 também (ver "Feito na rodada 14").

**Correr**: o dado de objeto **17** ("walk style" no STR# `0x8D`) = 1 corre:
`BeginFollow` pega então o conjunto de corrida (as strings 3–5 do bloco, em
vez das 0–2). Quem põe: "Function - Pre Route - Decide to Run" (chama o
global "Route - Decide Whether to Run" 0x327, pela distância e constantes de
tuning), as interações que apressam (bombeiro, "Go Here Frantically",
"Correr" de brincadeira). O `DoBeginFollow` (BV `0x5745e0`) lê o dado 17
(vtable `0x34(0x11)`) e passa ao planner e ao `BeginFollow`. A corrida do
adulto (`a-male-run-alt0-normal`) anda 8,3 m/s pelo cabeçalho (4,36 m em
0,525 s); o `s2pkg anim-root` mostra o pé de apoio parado nessa velocidade.
Os clipes de virada da corrida vêm com multiplicador de velocidade na string
("turnAndGo-alt@2.75": 2,75× mais rápido; parada 1,9×, virar no lugar e
passo 1,5×).

**Os clipes de começo, parada, passo e virada** (string 0 do bloco, por
papel): `turnInPlace`, `turnAndGo[Short][R]`, `turnAndStop[Short][R]`,
`step[Short]`, cada um feito a 0, 45, 90, 135 e 180 graus
(`a-turnAndGo-alt-90`, `a-TurnInPlace-90`, `a-step-45`; o nome do papel
"step-alt" vira "step-45"). Os tipos de locomoção do cabeçalho: 6 turn and
go, 7 turn and stop, 8 turn in place, 9 step, 10 step in place, 11–19 os
ciclos alt0–alt8. **Todos viram no sentido horário** (cabeçalho `turn` =
−45°·n). A busca do conjunto (`cLocoSet` vtable `0x30`, BV `0xd0c7ec`,
nomeada errado pelo symxfer): o ângulo pedido vai a [−π, π] e a passos de
45° com limiar de **±40°** (como o `ComputeTurn`), e procura o recurso do
tipo com `|turn − alvo| ≤ 0,01` (não espelhado) **ou `|turn + alvo| ≤ 0,01`
(espelhado)**; para os tipos 6/7 a porção do ciclo (`cyclePortionCompleted`,
0,5 nos normais, 0 nos "R" = o outro pé) casa também com ±0,5, isto é,
qualquer pé serve quando falta o outro. **Isso confirma o espelhamento** que
a rodada 13 (routing2) tinha inferido para o virar no lugar: uma virada
anti-horária toca o clipe horário espelhado.

**A raiz dos clipes** (medida com `s2pkg anim-root`): os ciclos de andar são
no lugar (raiz parada em y; o quadro anda na velocidade do cabeçalho, e é
nela que o pé de apoio desliza menos: 0,10 m/s contra 0,38 com ±20%).

**As unidades da corrida (rodada 14, verificado):** a velocidade do conjunto
(`FUN_00d0c444`) é o `velocity` do ANIM × 1000: o cabeçalho dá metros por
tick de animação (1 ms), então m/s; a curva (`cPath`) e a raiz andam em
unidades do mundo, e 1 tile = 1 m (a pelve do esqueleto em repouso a 1,0, o
passo do andar 1,39 m por ciclo de 0,69 s). O `s2pkg anim-root` mostra o pé
de apoio parado (0,04 m) na velocidade do cabeçalho da corrida, 8,31 m/s
(4,36 m em 0,525 s; criança 5,8; `runShort` 4,0), e o andar a 2,02 m/s. Ou
seja, 8,3 tiles/s é a corrida do jogo (4× o andar), não um erro de unidade;
o opensims2 já usa tiles por segundo sobre os ticks por segundo da VM, e
nada mudou. (O `CreateStepCycle` multiplica a velocidade dos clipes
misturados por 1,4 antes do × 1000 ao montar o pedido; para que serve esse
1,4 não foi lido.) O
`turnAndGo-N` começa com o corpo virado +N em relação ao quadro e termina no
quadro (o quadro já sai na direção nova; a raiz atrasa até 0,15 m atrás dele
e alcança); o `turnAndStop-N` começa no quadro e termina virado −N; o
`TurnInPlace-N` tem o quadro na direção inicial; o `step-N` mantém o corpo a
+N do quadro o tempo todo (passo de lado/para trás sem virar).

**A saída** (`BeginFollow`, corrigido): o ponto de saída não depende de estar
andando. O `ComputeTurn` (vtable `0x428`) devolve **a distância até o
primeiro ponto** e os passos de 45°; com distância > 0 o primeiro ponto da
curva fica a `min(d·0,3333, 0,25)` na direção em que o Sim olha **girada
pelos passos·45°** (o decompilador perdeu a variável; a listagem em
`0x6b63b4` lê os passos da pilha). O mesmo no fim: o último trecho chega pela
direção final (`+0x1c4`, múltiplo de 45°).

**Feito antes (rodada 13, routing2)** (`routing.cpp` `shapeWalk`/`turnSteps`/
`turnPoint`): o caminho cortado a cada 0,2 nos trechos do chão; a saída
pela frente para quem replaneja andando; virar no lugar
(`<idade>-TurnInPlace-45/90/135/180`) antes de sair quando o primeiro trecho
passa de 40° e no fim para a direção final. O front "walking" trocou o virar
no lugar da saída pelo `turnAndGo` (o virar no lugar fica para rotas sem
clipes de começo e para a virada final quando não houve parada) e a saída
passou a valer para todos, girada pelos passos. Captura antiga:
`docs/screenshots/r13-virar-no-lugar.jpg`.

**Feito no opensims2 (rodada 13, front "walking")** (`locomotion.cpp`
`gaitOf`/`locoClip`, `routing.cpp` `locoStep`/`stepBlocker`, `sim_motion.cpp`,
`lot_vm.cpp`; testes `test_routing` `testLocoPhases`/`testRunnerStep`,
`test_animation_vm` `testGait`): um **quadro** anda pela rota e o Sim fica
onde está a raiz do clipe (posição = quadro + deslocamento da raiz girado
pela direção do quadro; direção = a do quadro + o yaw da raiz), como o
`LocoPlaceFromSceneGraph`. As fases: **começo** parado com `turnAndGo` dos
passos do primeiro trecho (espelhado anti-horário; `turnAndGoShort` quando o
trecho final é mais curto que começo + parada; `stepShort` quando ≤ 0,25,
como o `BeginFollow` testa o papel 9), o quadro cobrindo a distância do
clipe na duração do clipe; **andar** com o ciclo do passo (`gaitOf`) na
velocidade dele, o quadro virando a cada trecho (o desenho suaviza a 7 rad/s);
**parada** no fim da rota, quando o que falta é a distância de uma parada:
`turnAndStop` da direção em que vem para a direção final (ou a curta depois
de um começo curto), do pé em que o ciclo está ("R" no primeiro e no último
quarto do ciclo, inferido das porções dos clipes), o quadro cobrindo o que
falta. O desenho põe o ator no quadro e o clipe põe a raiz. A saída gira
pelos passos (e não entra no caminho de outro Sim: inferido). **Bloqueio por
passo**: a posição nova da raiz não pode entrar no corpo de outro Sim
(`TSObjectCanSetPosition` com o erro 0x0C → bloqueador, que segue a lógica
do §6); inferidos: os corpos como discos de 0,125 (metade da largura do
planner, a previsão do `CollisionCheck` já os mantém a 0,5), quem já está
dentro pode sair, o Sim alvo da rota não bloqueia; objetos não são testados
por passo. Medido (`OS2_DEBUG_LOCO=1`, `s2pkg pie ... here:x,y`): no começo
e no andar o quadro anda na velocidade do clipe (desvio 0,001–0,003 m/s); a
parada esticada sobre o que falta desliza até ~0,1 m/s. Captura:
`docs/screenshots/r13-locomotor.jpg` (Don em N001_Lot16 virando 180° no
`turnAndGo-alt-180` espelhado, andando e parando com `turnAndStop-alt-0`).

**Feito na rodada 14 (front "walking2")** (`routing.cpp` `locoStep`/
`blendedClip`/`stepBlocker`/`tileBlocker`, `locomotion.cpp` `gaitOf`,
`sim_motion.cpp`, `lot_vm.cpp`, `anim_player` `Options::rate`; testes
`test_routing` `testLocoBlend`/`testLocoHalt`):

- **Mistura por ângulo** (`cActiveAnimLoco::CreateStepCycle`, BV `0xd2bf28`,
  lido): o ângulo pedido vai ao clipe do conjunto em passos de 45° (a regra
  de ±40°); se sobra mais de 0,01 rad, o clipe 45° adiante na direção da
  sobra entra com peso |sobra|/(π/4) e o primeiro com 1 − isso. Feito para
  o começo (`turnAndGo[Short]`, `stepShort`) e a parada (`turnAndStop[Short]`):
  duração, distância, raiz e virada são as dos dois pesadas; o desenho toca
  os dois como camadas do mesmo nível com os pesos, cada um na sua
  velocidade sobre a duração da mistura (e o `@1.5`/`@2.75` da corrida, que
  antes o desenho ignorava).
- **Ciclos de andar misturados** pelos pesos dos dados 112–117 (normalizados),
  cada um tocado na sua duração sobre a duração pesada (os pés em passo); a
  velocidade como antes. Inferido: o mais pesado é a camada de base.
- **`walkShort`/`runShort`** (meio passo, ~1 m/s): usados quando o que sobra
  da rota depois do começo, menos a parada, é menor que uma passada do ciclo
  normal (inferido; o binário compara distâncias de passada em
  `ComputeForwardWeightFromTurnDistance`/`ComputeTurnDistanceFromStride`,
  não decodificados por inteiro).
- **Curva**: o quadro anda numa Catmull-Rom pelos pontos da rota (cortados a
  cada 0,2), por comprimento de arco; a direção é a tangente. Inferido: o
  `cPath` do jogo usa uma estratégia cúbica (`cPathStrategyCubic`) cujos
  coeficientes não foram lidos.
- **Parada ao bloquear** (`StopLocomotion`): o passo bloqueado (previsto ou
  pelo teste por passo) toca `turnAndStop` de 0° do pé em que o ciclo está,
  de onde o corpo está, o quadro andando no máximo 2 passos (previsto) ou
  nada (o próximo passo já entraria), e parado onde o lugar seguinte está
  tomado; acabado o clipe, a rota segue para o `DoBlocked` com aquele
  bloqueador (sumido: segue andando).
- **Teste por passo contra objetos**: o tile para onde a raiz entra não pode
  ter objeto que o planner evita (os mesmos critérios do `addObstacles`: não
  os da rota, nem o alvo, nem quem deixa passar, nem os de patamar); quem já
  está num tile assim pode sair. Um objeto bloqueador falha a rota com **15**
  (`blocked by object`, o ramo "não é pessoa" do
  `DoBlockedGoalStillAvailable`). Inferido: o corpo do objeto como o tile.
- Medido (`OS2_DEBUG_LOCO=1`, que agora mostra as camadas e a parada): no
  andar o quadro anda na velocidade da mistura (desvio 0,003 m/s); as
  esquinas viram em ~4 ticks em vez de num tick; a parada misturada desliza
  ~0,02–0,06 m/s. Captura: `docs/screenshots/r14-locomotor-mistura.jpg` (Don
  em N001_Lot16: começo, ciclos `walkShort` alt1/alt0 a 0,75/0,25, parada
  `turnAndStop` 0°/45° a 0,82/0,17).
- **As mortes do merge do andar** (bisect da rodada 13 em 9d7607c): são
  incêndios do fogão sem detector de fumaça ("Fogo!" por horas, depois
  "Falecer"), e variam só com a semente: com `OS2_SEED` 1–6 em N001_Lot68 +
  N002_Lot10, antes do andar (98c5c85) 31 mortes, com o andar (main 67de446)
  18, sem o teste por passo 16, com o teste do caf8207 21, com esta rodada
  22. O teste por passo
  não as causa; o "1 contra 8" do bisect é a sorte de uma semente.

**Rodada 14, front "engine-re": o binário sobre o que o walking2 inferiu**
(lido no BV com a vtable do `cActiveAnimLoco`, vptr `0x28318f4`: `0x90`
`DetermineTurnResources`, `0x94` `ComputeForwardWeightFromTurnDistance`,
`0x98` `ComputeTurnDistanceFromStride`, `0x9c` `ComputeTurnWeight`, `0xa0`
`CreateTurnInPlaceCycle`, `0xa4` `CreateStepCycle`, `0xa8` `PushLocoCycle`,
`0xac` `PushTurnBlendCycles`, `0xb0` `MarkLastLocoCycle`; e a do
`cIAnimResource` pela secundária do `cAnimResourceConst`: `0xe4`
`DurationTicks`, `0xe8` `HeadingOffset`, `0xec` `LocoType`, `0xf0`
`LocoDistance`, `0xf4` `LocoCyclePortionCompleted`, `0xf8` `LocoStrideLength`,
`0xfc` `LocoStrideVelocityMPT`, `0x100` `LocoFinalCyclePortionCompleted`,
`0x104` `VelocityMPT`, `0x108` `TurnRotation`):

- **Os tipos de locomoção do conjunto não são os do cabeçalho.**
  `cAnimResourceConst::ReadFrom` (BV `0xd491a8`) converte os ANIM de versão
  < 7 (todos os do jogo base são v6): tipo `t` < 11 fica; 11 ≤ `t` < 30 →
  `t + 89` (**100 + k** = ciclo altK); `t` ≥ 30 → `t + 170` (**200 + k**).
  Depois o tipo é um campo de 12 bits (`LocoType` = bytes 9–10 >> 4) e a
  prioridade os 4 bits de baixo. Então: 1 (não achado nos dados), 6
  turnAndGo, 7 turnAndStop, 8 turnInPlace, 9 step, 10 stepInPlace,
  **100 + k** os ciclos `walk-altK` **e** `walkShort-altK` (os dois têm o
  mesmo tipo de cabeçalho 11 + k), **200 + k** as poses `walk-altK-*Blend`
  (0 ticks, prioridade 2, cabeçalho 30 + k: `normalBlend`, `pregnantBlend`).
  `AddLocoResource` (BV `0xd0ce78`) guarda os recursos ordenados por tipo e
  depois por `VelocityMPT`.
- **`walkShort` (confirmado em parte)**: `LocoResourcesWithClosestVelocity(tipo,
  v)` (BV `0xd0c1a4`) devolve o par de recursos consecutivos do tipo cujas
  velocidades cercam `v` (o lento e o rápido: walkShort ~1 m/s e walk ~2 m/s)
  e o `v` limitado a eles. O `InitChannelNetwork` (BV `0xd2cd68`) faz isso para
  cada `100 + k` com peso, guarda as distâncias (`LocoDistance` × escala) e as
  passadas (`LocoStrideLength`) dos dois, e mistura os dois pelas passadas:
  `ComputeTurnWeight(lento, rápido, s)` = `(s_r − s) / (s_r − s_l)` limitado a
  [0, 1] (peso do lento); `ComputeTurnDistanceFromStride` e
  `ComputeForwardWeightFromTurnDistance` (BV `0xd2b958`: com `d` entre as
  distâncias do lento e do rápido, peso = `(d_r − d)/(d_r − d_l)`) fazem o
  mesmo pelas distâncias. O número de ciclos do trecho reto é arredondado
  **para cima em meios ciclos** (`⌈(resto − passada·fase)/(passada·0,5)⌉`) e o
  que sobra é acertado misturando passada longa e curta (laço de até 10
  tentativas em passos de 0,1 do peso, ±0,25 ciclo). Ou seja: o jogo não
  troca para `walkShort` só no fim; **mistura** walk e walkShort em todos os
  ciclos para que um número inteiro de meios passos caiba exatamente na rota.
  Os detalhes do laço não foram fechados (o decompilador perdeu os
  argumentos das chamadas virtuais); a regra do walking2 ("walkShort quando
  o que sobra é menor que uma passada") é uma aproximação.
- **As poses 200 + k** (`PushTurnBlendCycles`, BV `0xd3107c`): para cada ciclo
  altK com peso > 0 e com recurso `200 + k`, um ciclo filho com a pose, o
  peso do ciclo e o tempo de mistura `+0xec` (0,2, limitado a 0,95 × a
  distância do clipe de começo e do de parada) vai sobre o ciclo de andar.
  O opensims2 não toca essas poses (TODO).
- **A curva é uma B-spline cúbica uniforme, não Catmull-Rom.**
  `cPathStrategyCubic::UpdatePathData` (BV `0xd56704`): os pontos de
  controle são o primeiro ponto da rota **três vezes**, todos os pontos e o
  último três vezes; cada janela de 4 pontos vira um trecho com a matriz
  `1/6 · [[1,4,1,0],[−3,0,3,0],[3,−6,3,0],[−1,3,−3,1]]` (lida em
  `0x273a740`, o 1/6 em `0x274bc98`). A curva começa e termina nos extremos
  mas **não passa** pelos pontos do meio (corta as quinas; com a rota cortada
  a cada 0,2 o corte é pequeno nas retas e maior nas quinas). O comprimento é
  medido amostrando cada trecho (passo inicial `1/⌊comprimento/0,03⌋`,
  metade enquanto a corda passa de 0,005) e o parâmetro de cada ponto da
  rota é reescalado pelo comprimento da curva. **TODO**: trocar a Catmull-Rom
  do walking2 por essa B-spline.
- **`CreateStepCycle` é só do passo (tipo 9), não do começo/parada.** Ele
  chama `DetermineTurnResources(escala, 9, ângulo, 0, ...)`
  (`NextLocoResourceByTurnRotation`: o clipe `step-N` mais perto em passos
  de 45° e um segundo do mesmo ângulo, o `stepShort`), mistura os dois
  ângulos vizinhos por `|sobra|/(π/4)` e, dentro de cada ângulo, o `step` e o
  `stepShort` pela distância pedida; distância menor que a do mais curto →
  mistura com o `stepInPlace` (tipo 10; sem ele, o tipo 1) a distância 0;
  maior que a do mais longo → falha (anda normal). A velocidade do pedido é
  `Σ w·VelocityMPT / Σ w × 1,4 × multiplicador do nome (@1.5) × 1000` (o
  **1,4** é um fator fixo de rapidez dos passos, `0x277d8e8`). Quem chama
  (`InitChannelNetwork`): só quando a rota é reta (|corda − comprimento| <
  0,02) e as direções de começo e fim do pedido são iguais (|Δ| < 0,01): o
  Sim **dá passos sem virar** na direção da corda relativa ao rosto. O começo
  e a parada (tipos 6/7) usam `DetermineTurnResources` com o clipe de 45°
  mais perto **sem misturar ângulos** (o resto vai para a curva); o par
  misturado deles é do mesmo ângulo (o clipe e o seu "Short"/"R", pela
  passada e pela porção do ciclo). **Corrigir** (TODO): o walking2 mistura os
  dois ângulos vizinhos no `turnAndGo`/`turnAndStop`; no jogo isso é só do
  `step`.
- **Os 5/100 ms**: o `+0x24c` comparado com 0,25 não é a velocidade: é o
  comprimento do começo da rota medido no `BeginFollow` (soma dos trechos até
  passar de 0,75, ou até mudar de andar/sala). Ou seja, 5 ms com rota curta
  (≤ 0,25), 100 ms senão; o mesmo `+0x24c` decide o N = 25 do estado 3.
- **O teste por passo e os outros Sims** (`LocoPlaceFromSceneGraph` →
  `TSObjectCanSetPosition` → `TestPlaceInWorld` →
  `TestPlacementWithRespectToObjects`/`...Object`, BV `0x8ace04`/`0x8ac158`):
  - o passo passa sem testar objetos se o dado de pessoa **68** tem o bit
    `0x02`; com o bit `0x20`, objetos com `IsOKToFlyOver` são ignorados;
  - entre duas pessoas: passa se uma delas tem `CanIntersectPeople` —
    pessoa: o bit `0x08` do campo `+0x72a` (inferido: o dado de pessoa 68)
    ou, como objeto, `GetFlags()` (dado de objeto 8) com `0x04 | 0x10`
    (`HasZeroExtent`, `AllowPersonIntersection` nos nomes do FreeSO); passa
    se a árvore "allow intersection" (OBJf 5) de uma das duas deixa; passa se
    não dividem `FootprintBlocksCeiling` nem `FootprintBlocksFloor`;
  - senão o corpo de um Sim é um **ponto**: bloqueia (erro **0x0C**) se os
    centros estão a **< 0,283** um do outro (`0x274b968`). O walking2 usa
    discos de 0,125 (0,25 entre centros): **corrigir para 0,283** (TODO);
  - o alvo da rota não tem exceção no teste (a exceção do opensims2 é
    inferida); quem já está dentro: o `LocoPlaceFromSceneGraph` aceita a
    posição se o Sim "está preso" (vtable `0x2e0`) e a posição atual também
    falha;
  - contra objetos, com o flag 4 (dado quando o Sim tem `0xc0`/`0xc4` da
    interface de pessoa e o pedido é da sequência 1) o teste usa também um
    ponto 0,25 (idade 4/8) ou 0,5 (idade 2) à frente do Sim com raio 0,3;
    contra portas (flag 2), um círculo de 0,3.

**Feito na rodada 14, front "walking3"** (o locomotor conforme a leitura
acima; `locomotion.cpp` `LocoCurve`/`blendGaits`/`strideWeight`,
`routing.cpp` `locoStep`/`turnClip`/`sideStep`/`stepBlocker`; testes
`test_routing` `testLocoCurve`/`testLocoStride`/`testSidestep`/`testLocoBlend`):

- **Curva**: a Catmull-Rom saiu; o quadro anda numa B-spline cúbica uniforme
  com o primeiro e o último ponto três vezes (a matriz de `0x273a740`). O
  comprimento de cada trecho vem de 16 cordas (inferido: o binário reduz o
  passo até a corda ficar < 0,005; com a rota cortada a cada 0,2 as cordas
  aqui ficam < 0,0125) e o quadro anda por comprimento de arco. Um ponto da
  rota é "passado" quando acaba o trecho que termina perto dele (o último só
  no fim). No começo o quadro também vira pela tangente da curva (o que
  sobra do ângulo vai para a curva; inferido: a orientação do quadro é a do
  caminho); a parada e o passo de lado mantêm a direção em que começaram.
- **Começo/parada**: `turnAndGo`/`turnAndStop[R]` do passo de 45° mais perto
  (regra de ±40°), sem misturar ângulos, misturados com o seu `Short` do
  mesmo ângulo pelo peso da passada (inferido que é o mesmo peso dos
  ciclos: o doc diz "pela passada").
- **walk/walkShort pela passada**: o peso do walkShort é o que faz caber um
  número inteiro de meios ciclos entre o começo e a parada: `h = ⌈(trecho −
  começo − parada)/(meia passada longa)⌉` e o peso resolvido exatamente
  (começo e parada também mudam com o peso). Inferido: a "passada" como a
  distância de um ciclo (velocidade × duração; o `LocoStrideLength` do
  cabeçalho é ~meio ciclo) e o termo de fase do binário ignorado; o laço de
  10 tentativas do binário não foi lido inteiro. Replanejado andando,
  recalcula com só a parada pela frente.
- **Passo de lado** (`CreateStepCycle`): rota reta (|corda − comprimento| <
  0,02) para a direção final igual à atual (< 0,01): `step`/`stepShort` dos
  dois ângulos vizinhos (|sobra|/45°), pela distância dentro de cada ângulo,
  `stepInPlace` abaixo do curto, falha acima do longo (anda normal);
  duração = distância / (velocidades pesadas × 1,4 × `@`). A regra antiga
  "`stepShort` com rota ≤ 0,25" saiu.
- **5/100 ms**: o começo da rota (trechos somados até passar de 0,75, ou
  até mudar de andar; a troca de sala não é testada) ≤ 0,25 → 5 ms, senão
  100 ms; o desenho funde as fases nesse tempo (era 0,1 s fixo). Inferido:
  só o crossfade; a VM não antecipa o relógio da fase seguinte, e o N = 25
  do estado 3 não se aplica (as fases encadeiam tick a tick).
- **Teste por passo**: Sims são pontos, bloqueiam a < 0,283; sem exceção
  para o alvo; passa se um dos dois tem o dado de objeto 8 com 0x04|0x10,
  o bit 0x08 do dado de pessoa 68 (inferido, `+0x72a`), ou se a árvore
  OBJf 5 de qualquer um deixa; o bit 0x02 do dado de pessoa 68 pula o teste.
  Não feito: `FootprintBlocksCeiling/Floor`, o ponto à frente do flag 4 e o
  círculo das portas.
- **Poses `*Blend` (200 + k)**: `gaitOf` sabe achá-las (string 2/5 do
  conjunto, peso do ciclo). O alvo delas é do tipo 6 e os canais são todos
  **delta** (bit 15 apagado; `docs/primitives-mac.md` §4 "Canais delta"):
  desde a rodada 14 (front "anim-additive") o player as multiplica sobre os
  ciclos e elas vêm ligadas (`OS2_NO_LOCO_POSES=1` tira, para comparar). A
  idosa Isabella (N003_Lot46) anda curvada, cabeça para a frente e braços
  junto ao corpo, sem abrir os braços: `docs/screenshots/r14-anim-aditivo.jpg`
  (sem e com as poses). Soak com `OS2_SEED` 1 e 2 (N003_Lot46, N001_Lot16,
  20.000 ticks): saída idêntica com e sem as poses (só o desenho muda).
- **O ajuste da passada no binário** (rodada 14, front "anim-additive",
  lido em parte no `cActiveAnimLoco::InitChannelNetwork`, BV `0xd2cd68`;
  **não implementado**: os papéis de algumas variáveis não fecharam, e
  trocar a regra mudaria o tempo das rotas na VM). Com `d` o comprimento
  da rota, `s_c`/`s_p` começo e parada e `L` a passada longa (a soma pesada
  dos ciclos), o resto `r = d − s_c − s_p` vira um número de **meios
  ciclos** `n = ⌈(r − L·(f₀ + f₁)) / (0,5·L)⌉` (≥ 0), onde `f₀` é a fase
  do fim do começo (`1 − porção`, menos 0,5 se ≥ 0,5) e `f₁` a porção do
  ciclo atual (vtable `0x188` do locomotor, reduzida a < 0,5; 0 parado); o
  total de ciclos é `c = f₁ + 0,5·n + f₀`. Com clipe de parada, até 2
  tentativas: a cada `n` ímpar troca o pé da parada (`|porção − 0,5|`,
  `DetermineTurnResources` de novo, vtable `0x90`) e, se `L·c + s_c + s_p`
  ainda não cobre `d`, soma meio ciclo. Depois, com `T_l = L·c + s_c + s_p`
  e `T_s` o mesmo com a passada, o começo e a parada curtos: o peso do curto
  é `w = (T_l − d)/(T_l − T_s)` em começo, ciclos e parada; **se `w > 0,3`**
  (`0x274b670`) e `c ≥ 0,5`, tira meio ciclo e resolve de novo com um termo
  de ±0,25 ciclo (`0x274b54c`/`0x274b7b4`) e a parada misturada a
  `1 − k/10` (`0x274b6e0` = −10), `k` = 0..10 até o peso dar ≥ 0 (o "laço
  de 10 tentativas"). O que sobra (`d` − começo − `c`·passada − parada, ≥ 0)
  vai para `+0xf8`. Rotas curtas (sem espaço para ciclos, o ramo com
  `0,9·(s_c + s_p) ≥ d`, `0x274b6d0`) misturam começo e parada pelo
  comprimento, com `(d − 0,9·s)/(4 − 0,9·s)` (`0x274b488` = 4). O walking3
  resolve `w` exato num número inteiro de meios ciclos sem o termo de fase
  nem a troca para `−½` ciclo acima de 0,3.
- Medido: `OS2_DEBUG_LOCO=1` (Don em N001_Lot16, "Vir Aqui" a 14,5,7,5):
  começo `turnAndGo-alt-135` 0,85 + `Short` 0,14, ciclos alt1/alt0 longos e
  curtos (0,16 de walkShort; 1,85 m/s), parada `turnAndStop-alt-0` 0,69 +
  `Short` 0,30; `s2pkg anim-root` do walk-alt1 a 1,854 m/s: 0,30 m/s (0,32 na
  velocidade própria: a medida já tem esse piso). Soak com `OS2_SEED` 1–2 em
  N001_Lot68, N002_Lot10, N001_Lot16, N001_Lot11 (129.600 ticks): ciclos
  312,1 G antes, 306,8 G depois (−1,7%); mortes 9 antes, 3 depois (os
  incêndios variam com a semente). Captura:
  `docs/screenshots/r14-locomotor-bspline.jpg` (Isabella, idosa, em
  N003_Lot46, ciclos walk/walkShort misturados).

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

## 8.3 Árvores da rota: pós-rota, goal check e along route (binário, rodada 7)

Os números são os **entry points** do STR# 0xF5 (`objects.package`, grupo
`0x7FE59FD0`), que o motor procura com `GetTreeID(entry)` (vtable `0x170`
do `cTSObject`) e roda com `GosubObjectTree` (vtable `0x3cc`, modo 5 =
árvore empurrada na thread do Sim, como a pré-rota): 37 (0x25) "Pre Route",
**38 (0x26) "Post Route"**, **39 (0x27) "Goal Check"**, 40 (0x28)
"Reaction Handler" e **41 (0x29) "Along Route Callback"**. As árvores ficam no semi-global
da pessoa ("Function - Post Route", …). Em todas, falso ou erro levam a rota
a `DoNeedFailureFeedback` (0x14) com o código abaixo.

| árvore | quem roda, em que objeto | quando | parâmetros | falso → |
|---|---|---|---|---|
| **post route 0x26** (`TriggerPostRouteTree`, BV `0x577cf4`) | a árvore **do próprio Sim** que anda, stack object = ele | em `DoFailedOrSucceeded` (BV `0x577ea0`, estados 9/10), uma vez: só se a pilha de rotas tem **uma** rota, a rota tem o flag `+0xac` ligado e o `+0xad` desligado (o `+0xad` é ligado ao rodar), e o estado não pede para pular (`+0x4c`) | 0 = código de resultado da rota (`+0x64`), 1 = o `+0x10` do objetivo atual se houver objetivo (senão 0), 2–3 = 0 | código 12 "post-route failed" (estado 7/8 → `DoWaitingForPostRouteTree`, BV `0x56ddac`) |
| **goal check 0x27** (`TriggerGoalCheckTree`, BV `0x575e34`) | a árvore 39 **do objeto dono do código do frame que chamou o Go To** (o frame logo abaixo do topo da pilha do Sim: o OBJf do seletor desse frame), rodada no Sim com stack object = o stack object desse frame (sem ele, o alvo da rota) | em `DoWalking` (BV `0x576acc`) **a cada tile cruzado** (o avanço da rota, vtable `0x60`, indica que mudou de célula e o temporizador de callbacks deixa); antes chama `AlongRouteUpdate` | 0 = índice do slot de rota (`WorkOutSlotIndex`), 1 = `+0x14` do objetivo, 2 = `+0x10` do objetivo, 3 = distância restante (vtable `0x3c`) × 100 | código 13 "goal not available" (estado 4; bloqueado: estado 0x19 → 9) |
| **along route 0x29** (`TriggerAlongRouteTree`, BV `0x5766a0`) | a árvore **do próprio Sim**, stack object = ele | logo depois de um goal check que passou (`DoWalkingWaitingForGoalCheckTree`, BV `0x576970`), no mesmo tile, se o temporizador ainda permite; ou direto quando não há goal check | 0 = ID do alvo, 1 = índice do slot, 2 = `+0x14` do objetivo, 3/4 = dois valores da rota (vtable `0xf0`, padrão 0 e −1), 5 = vtable `0x1d0` da rota, 6 = distância restante × 100 | código 16 "interrupted by callback" (estado 0x1E → `DoWaitingForAlongRouteTree`, BV `0x56db30`) |

Os testes de "sem árvore" valem para os três: sem rota/alvo (`0x118`) ou
sem objetivos (`0xf4`), não roda e segue andando. O goal check só existe se
o objeto que chamou define a entrada 39. Assim, uma cadeira cuja interação manda o Sim
andar até ela pode recusar no meio do caminho se outro Sim sentou
(texto do editor da entrada 39).

**Diferença com o código atual**: `vm/routing` só roda a pré-rota. Pós-rota,
goal check e along route não rodam (TODO "Pós-rota (entrada 0x26…)"). A
pós-rota é do Sim, não do alvo. É a árvore do semi-global da pessoa que
decide sentar ou chamar o alvo.

### Constantes do lugar para sair (`CreateMoveOutOfWayDestination`, BV `0x57dcac`)

O destino ("Destination - For Go Here & Shoo") é criado na posição do
**bloqueador** (`this` = roteador do bloqueador, `param_1` = de quem pede)
com um slot sintético (`0xFBA4DA4F`, §8.2) como slot 0. A interface do slot
tem o objeto em `+0x20`, então os offsets abaixo são os do objeto: direções
`+0x30` (máscara), `0xD0(a, b, c)` → `+0x38 = a` (a resolução de §8.2),
`+0x3c = b`, `+0x40 = c`, `0xCC` → `+0x44` (a largura do cone de §8.2),
`0xD4` → `+0x4c`, `0x40` → `+0x48` (direção de facing 0–7).

- **Tipo 0** (pedido de quem anda; `MoveOutOfWay` com o tipo do pedido e
  `TryGetSelfOutOfWay`, BV `0x57e9ac`, sempre 0): falha se os dois estão a
  **mais de 4 tiles**. Toma a direção d de quem pede (dado 1) e os pontos
  quem-pede + **0,1** tile × unit(d+6) e + 0,1 × unit(d+2) (à esquerda e à
  direita dele). Se o bloqueador está mais perto (ou à mesma distância) do primeiro, a direção
  do slot é **d+7** (−45°). Senão é **d+1** (+45°), mod 8. Valores:
  `a = 0,0625`, `b = 0,3`, `c = 0,3`, largura `100`, `+0x4c = 0,025`. O facing
  aponta do bloqueador para quem pede. As flags `0xdc(0,1)` e `0x74/0x7c/0x48/0x50 = 0`,
  `0x8c = 1`, `0x84 = 0`.
- **Tipo 1**: direções relativas 1, 2, 6 e 7 (±45° e ±90°), `a = 1`, `b = 3`,
  `c = 1`, `+0x4c = 0,25`, largura `0,625`, `0x48 = 0`, `0x50 = 0`.
- **Tipo 2**: igual ao 1 com `b = 6`, `0x48 = 1`, `0x50 = 1`, `0x94 = 1`.

**Inferido**: pela varredura de §8.2 (fileiras com passo `+0x38` e largura
`+0x44`), `b`/`c` são os raios máximo/mínimo. No tipo 0 isso daria uma
fileira a 0,3 tile do bloqueador, a 45° da direção de quem pede, com passo de
1/16 e largura praticamente ilimitada (o custo de §8.2 escolhe). Os tipos 1/2
dariam fileiras de 1 a 3 ou 6 tiles. Isso substitui os "1 a 6 tiles" do texto acima (que
eram o tipo 2).

**Quem pede cada tipo** (rodada 8, binário): o tipo vem de quem chama.
- `DoBlockedGoalStillAvailable` (BV `0x57a8d4`) chama
  `AskBlockerToMove` (vtable `0xcc`, BV `0x57d8e8`) com prioridade = dado
  33 − 1 (mín. 1), só nas primeiras 3 tentativas (`+0x3c` ≤ 3), e
  **tipo 1** quando a rota tem um objeto de destino (vtable `0x118`) e o
  bloqueador está a **menos de 1,5 tile** dele (`FLOAT_0274b6b4`), ou seja,
  em cima do meu objetivo; senão **tipo 0**. `AskBlockerToMove` repassa o
  tipo ao `MoveOutOfWay` do bloqueador (vtable `0x90`), que o repassa ao
  `CreateMoveOutOfWayDestination`.
- **Tipo 2**: `MoveOutOfWay(long prioridade)` (vtable `0x8c`, BV
  `0x57f488`; o Ghidra o junta ao fim do `MoveOutOfWay` de 4 argumentos):
  sem quem peça, o próprio Sim procura um lugar a 1–6 tiles e empurra a
  interação "sair do caminho" (tipo 3) com a prioridade dada. Quem chama a
  vtable `0x8c` não foi achado (provavelmente uma chamada da VM).


## 8.4 O ponto do "Vir Aqui": afastar dos objetos e das paredes (binário, rodada 13)

`cTSUserToolStandardAction::GoHere` (BV `0x358098`): cria o destino
(`NewDestinationObjectID`: 1 chão, 2 pescável, 4 praia; 0 senão), e o ponto
é

```
p = TSObjectMoveAwayFromOtherObjects(sim, clique, 0,4, pularSims=false, portas=true)
p = MovePointAwayFromWalls(sim, p, 0,45)
TSObjectCanSetPosition(destino, p, 0, 0, ...)   (orientação atual do destino)
  falhou → apaga o destino, nada; senão SetPosition(p) e abre o pie
```

**`TSObjectMoveAwayFromOtherObjects(obj, loc, d, pularSims, portas)`** (BV
`0x8a9144`; `obj` aqui é o **Sim**, não o destino):

1. Devolve `loc` sem mexer se `obj` é pessoa (interface `0x5A69D3D5`) com o
   bit `0x02` do dado de pessoa `0x44` (68), ou se `obj->CanIntersectObjects()`
   (vtable `0x238`).
2. Para cada objeto dos **3×3 tiles** em volta do tile de `loc` (`IsOnLot(tile)`,
   vtable `0xf8` da `cIWorldDB`):
   - com `portas` e o objeto de tipo **8** (porta): afasta `p` das pegadas
     indexadas **0 e 2** dele (`TSObjectIndexedWorldSpaceFootprint`);
   - se um dos dois é pessoa (tipo 2), os dois têm de ter
     `CanIntersectPeople()` falso (vtable `0x1d8`), senão o objeto é
     ignorado;
   - se o objeto não tem `CanIntersectObjects()`, e (sem `pularSims` ou
     ele não é pessoa), e a árvore **"allow intersection"** (OBJf 5,
     `RunTree(5, id do outro, 0, 1, 0)`) não deixa nem a dele com o `obj`
     nem a do `obj` com ele: afasta `p` da pegada dele (`GetFootprint`,
     vtable `0x100`, na posição e orientação dele).
3. Com `portas`: o anel de **5×5** (tiles a 2 do centro, fora do 3×3):
   portas diagonais (`IsDiagonalDoor`) → pegadas 0 e 2.
4. Os empurrões são **em sequência** sobre o mesmo `p` (cada objeto vê o `p`
   já empurrado pelos anteriores).

**Empurrar `p` para fora de uma pegada a distância `d`** (`FUN_0191f6d0`;
pegada = lista de retângulos x0, y0, x1, y1; ε = 0,0001): para cada
retângulo com `p` dentro dele inflado de `d` (inclusivo): em x, se `p.x` >
centro, `dx = x1 + d + ε − p.x` (fica o maior positivo); senão `dx = x0 − d
− ε − p.x` (fica o mais negativo); o mesmo em y. Se algum ≠ 0: se |dy| ≤
|dx| e |dx| ≥ d + 2ε → só `p.y += dy`; se |dy| > |dx| e |dy| ≥ d + 2ε →
só `p.x += dx`; senão os dois. (Ou seja, empurra pelo eixo mais curto, a não
ser que os dois sejam curtos.)

**`MovePointAwayFromWalls(sim, loc, d)`** (BV `0x3574f0`; `d` = 0,45, ε =
0,001): para os lados 1, 2, 4, 8, 0x10, 0x20 do tile de `loc`, em ordem, se
há parede ali (gerente de partições vtable `0xb0` com o `cWallLocator`
(tile, lado)):

- 1: `fy = y − ⌊y⌋ < d` → `y += d − fy + ε`; 2: o mesmo em x;
- 4: `⌈y⌉ − y < d` → `y −= d − (⌈y⌉ − y) + ε`; 8: o mesmo em x;
- **0x10**: `s = (fy − (⌈x⌉ − x)) / √2` (distância com sinal à diagonal
  fx + fy = 1); 0 ≤ s < d → `t = d − s + ε`, `x += t/√2`, `y −= t/√2`;
  −d < s < 0 → `t = d + s + ε`, `x −= t/√2`, `y += t/√2`;
- **0x20**: `s = (fy − fx) / √2` (distância à diagonal fx = fy);
  0 ≤ s < d → `t = (d − s + ε)/√2`, `x += t`, `y += t`; −d < s < 0 →
  `t = (d + s + ε)/√2`, `x −= t`, `y −= t`.

Literal do binário: nos dois casos diagonais o empurrão vai pela normal da
**outra** diagonal, isto é, **ao longo** da parede (0x10 mede até fx + fy = 1
e anda em (1, −1); 0x20 mede até fx = fy e anda em (1, 1)), então o ponto não
se afasta da parede diagonal. Parece um erro do jogo; para ser fiel, copiar
assim. A geometria de qual diagonal é 0x10/0x20 vem das fórmulas (inferido:
o `cWallLocator` só guarda tile + lado, `FUN_0156003c`).

### Clique num objeto e no tapete (`cTSUserToolStandardAction::MouseEvent`, BV `0x359ef4`)

Clique com um objeto sob o mouse (rodada 13):

- **tapete** = `GetType()` **0x13** e dado do objeto `0x3b` (vtable
  `0x34(0x3b)`) ≠ **0xA7**; para o tapete o tool acha também o chão sob o
  mouse (`GroundOrWallButNotPondAtGameWindowPoint`);
- `ObjectHasInteractions(obj)` (cache por objeto; o sistema de interações
  vtable `0x48(Sim alvo, obj)`);
- **não é tapete, com interações** → `PickObject(obj, nenhum)` (o pie dele,
  mensagem `0xC0` com o ID);
- **não é tapete, sem interações** → só a dica (`ObjectTooltipString`);
- **tapete sem interações** → `GoHere(chão)` normal (o pie do "Vir Aqui"),
  se achou chão;
- **tapete com interações** → se achou chão e `GoHere(chão, &destino)` deu
  certo (com o parâmetro de saída o `GoHere` não abre pie), `PickObject(tapete,
  destino)`: **um pie só com o tapete e o destino** (a mensagem `0xC0` leva
  os dois IDs), e, se o destino ainda é do mesmo seletor, o Sim alvo o guarda
  (`AddManagedDestinationObject`); sem chão ou sem destino → o pie só do
  tapete.

## 9. Comparação com o código atual (main `3b4c4db`: `src/vm/path_planner.*`, `routing.cpp`)

| jogo | código |
|---|---|
| célula mínima 0,125 tile | 1/8 tile (igual; o código marcava como aproximação) |
| custo do portal = 1 + OBJD[0x35]·escala; trava = árvore de portal (entrada 15) em modo teste | custo aproximado; portas sempre abertas para o A* |
| ao chegar no portal roda a entrada 15 de verdade e espera; portal ocupado → espera 24 ticks, até 126 | a VM segura o Sim no portal e o app abre a porta (aproximação) |
| `MoveOutOfWay`: interação "sair do caminho" (tipo 3) na fila do bloqueador, com a prioridade de quem pede, só se a dele é menor (ou dado 159 bit 0x02), a ≤ 4 tiles | pede até 3 vezes e espera, sem pôr nada na fila do bloqueador |
| tipo 1 (1–3 tiles) quando o bloqueador está a < 1,5 tile do objeto de destino, senão tipo 0 (0,3 tile) | igual desde a rodada 12 (direções do tipo 1 relativas a quem pede, inferido) |
| slots de osso de um objeto multi-tile: o modelo é o do líder | ossos a partir do líder desde a rodada 12 (inferido; a escada reta só funciona assim) |
| objetivos avaliados por `EvalTileForGoal` (códigos e penalidades +10/+5, §8.2) | aproximação da busca de objetivos (`routing.cpp`) |
| pegada do objeto: a do modelo (CRES "footprint", máscara 16×16 reduzida a 1/8 tile) ou a caixa do OBJD (flag 0x80 do od 8) | igual desde a rodada 14 (`VM::routingFootprint`); o tile da parte no modelo e a caixa do modelo sem lista são inferidos; `OS2_TILE_FOOTPRINTS=1` volta ao tile inteiro para comparar |
| quadtree, inflação 1/8, filtros, A* com ruído, Sims como retângulos de custo, códigos de falha, "CT - Route Blocked" | seguem o doc |

## 10. O que falta

- O custo exato de cada `ePassabilityState`.
- O teste de segmento do `CollisionCheck` (vtable 0x478) em detalhe.
- Confirmar que nada escreve a escala de portal (`DAT_02d86430`) por um
  caminho indireto.
