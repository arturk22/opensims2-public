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
multi-tile, o tile "(x,y)" é o da parte no modelo e o retângulo é
relativo à própria parte; uma lista sem esse tile não dá retângulo.
**Rodada 15 (binário):** a vtable `0x150` é o
**`GetLocalOffsetFromLeader`**: 0 para um objeto comum (BV `0xaf80f4`); para
uma parte (`cEdithMTObject`, BV `0xaf2410`) **(x da parte − x do líder, y do
líder − y da parte)** dos **`GetNormalizedOffset`** (BV `0xaf0148`: os
inteiros em `+0x2ec/+0x2f0/+0x2f4` do XMTO, postos só pela vtable; inferido:
os deslocamentos do multi-tile no OBJD, sem giro), arredondado para o tile.
Não é a posição no mundo girada de volta (o app; dá o mesmo se o
deslocamento normalizado é o do mundo com a direção do líder desfeita e o y
trocado de sinal — conferido na rodada 15 em todas as camas (direções 0, 2, 6) e pianos (direção 2) dos lotes: o índice "sub" do OBJD (campo `0x56`: x no byte alto, y no baixo) dá o mesmo tile; o VM usa agora a fórmula do binário com ele). O `AggregateFootprint`
(vtable `0x2c8`, BV `0xaf49a0`) passa o od 9 e o `HasExtendedFootprint`
(vtable `0x1a8`, BV `0xb012f0`: objeto que **não** é parte de multi-tile e
porta (tipo 8) com a vtable `0x25c` & `0x50`, ou **OBJD[`0x4e`] ≠ 0**; nenhum
OBJD do jogo base tem o `0x4e`) ao `cObject` (`FUN_00658ab8`): com pegada
estendida, **todos** os tiles "(x,y)" da lista juntos; senão só o tile da
parte. **Sem lista "footprint"** no recurso (com od 9 = 0) a pegada é a
**caixa do modelo** (`FUN_00a45e54` no nó da cena) menos o tile (x, y) e,
sem pegada estendida, **cortada a ±0,5** (`FLOAT_0274b5a4` = −0,5,
`FLOAT_0274b4d4` = 0,5) — confirmado; o VM faz assim desde a rodada 15 (sobrevivência 5; a caixa das malhas sem os planos de sombra, inferido).
Com od 9 ≠ 0 e sem a lista "%d", ou com a flag `0x80`, a caixa do OBJD.

Exemplo: o "Piano Upright" (`pianoUprightSaloon`): tiles (0,0)/(1,0) o
piano (linhas 1–15 de 16 cheias), (0,1)/(1,1) o banco, só as linhas 0–4
(5/16 do tile, 3/8 depois da redução) e as colunas do meio. Os ossos
`route_piano_*_s` ficam a 1,30 no y do modelo: fora da pegada do banco
(0,5–0,875), mas dentro do tile dele.

O teste por passo do andar e o `Snap` usam a mesma pegada
(`TSObjectCanSetPosition`: a caixa da pessoa contra as pegadas dos objetos,
`VM::pointFree` em `routing.cpp`, `footprintBlocker` em `routing_loco.cpp`).

### Quem entra no planejador (binário, rodada 15)

O começo do `AddThisToPathPlanner` (BV `0xaf68e0`, vtables do `cTSObject`
resolvidas com `tools/symxfer/vtables.py`) deixa de fora:
- `IsBeingEdited` (`0x35c`: `+0x78 > 0`), `GetContainer` (`0x118`: objeto
  num slot), os tipos `0x12`/`0xd` que não são multi-tile e têm o od `0x46`;
- `FootprintBlocksCeiling` (`0x23c`): **od 42 bit `0x1000`** ("attached to
  ceiling", as luminárias de teto);
- portas, janelas e portais de escada modular (tipos 8, 9 e 12) salvo
  OBJD[`0x4b`] = `0x93`; fora do mundo (`IsInWorld`, `0x2e0`); pegada vazia.

Depois `TSObjectGetPathPlannerCostAndPassibility` (BV `0xaf5270`):
`CanIntersectPeople` (`0x1d8`) é **od 8 & `0x14`** ("allow object and person
intersection" / "allow person intersection"). Sem ela (ou janela) o objeto
**bloqueia**: passagem 11, ou 9 com `IsOKToFlyOver` (od 8 bit 0 "can be
stepped over", ou od 40 bit `0x1000`); as duas são > 1, então o
`AddObjectRectangle` (BV `0x4f34b4`) põe também o retângulo inflado 0,12499
com o custo máximo. Com ela, o objeto só entra se tem penalidade de rota (od
10 ≥ 1) ou árvore "allow intersection" (`GetTreeID(5)`), com passagem 0 e
custo `od 10 · escala + 1`.

Não há no binário os filtros que o VM tinha por falta das pegadas: objetos
"pequenos" (od 4 além do chão: copos, bandejas, mas também estátuas e
vasos), "soltos do chão" (od 42 `0x80`), pendurados na parede (od 42
`0x200` com parede: cortinas), od 8 bit 0, as peças de patamar das escadas
modulares (têm od 8 `0x04`). Tirados na rodada 15: cada um bloqueia pela sua
pegada. Sem lista "footprint" o VM agora usa a **caixa do modelo** (as
malhas no plano xy, sem os planos de sombra; inferido) cortada ao tile da
parte, como o `cObject` (BV `0x658ab8`): cortinas viram faixas de 0,3 tile
junto da parede, lápides e luminárias de jardim uns 0,2–0,3 tile. O
`EvalTileForGoal` (§8.2) também passou a testar a caixa do Sim (0,2) contra
as pegadas no ponto, não o tile inteiro.

**Rodada 15 (planejador), o resto do `AddThisToPathPlanner`** (binário):
- **Escadas** (tipos 10 e 11) entram pelas pegadas como qualquer objeto; só
  portas, janelas e portais de escada modular (8, 9, 12) ficam de fora. O VM
  faz assim agora (`OS2_STAIRS_OUT_OF_PLANNER=1` volta ao antigo para
  comparar; `N003_Lot192`, 1 dia, semente 1: 66 interações falhas → 26).
- **A exceção `0x93`** é o campo de **índice** `0x4b` do OBJD (deslocamento
  `0x40 + 2·0x4b` = `0xD6`; a mesma conta dos campos `0x43`/`0x4d` da
  pegada): nenhum OBJD do jogo base tem `0x93` ali (3.262 têm 0; os outros
  1–11). Feito, sem efeito no base.
- **A penalidade de rota não pesa**: o custo é `od 10 · DAT_02cc4c04 + 1` e
  esse float fica no `__bss` sem nenhum gravador (só três leituras:
  `AddThisToPathPlanner`, `TSObjectGetPathPlannerCostAndPassibility`,
  `TSObjectPathPlannerCostAndPassibilityCB`; nenhuma instrução `st*` com o
  par `lis 0x2cc`/`0x4c04`), como a escala dos portais (`DAT_02d86430`, §8).
  Vale 0, o custo é **1** (o do piso), e os objetos que deixam pessoas
  passar (od 8 & `0x14`) com penalidade entram como passáveis de custo 1:
  deixá-los fora (o VM) dá o mesmo.
- Com a árvore "allow intersection" (`GetTreeID(5)`) o retângulo que
  bloquearia (9/11) entra com passagem 3 (pode pular por cima) ou 5 e o
  callback `TSObjectPathPlannerCostAndPassibilityCB` (BV `0xaf854c`), que
  roda a árvore 5 com o Sim do contexto (o contexto do plano é só o ID do
  Sim, `FindPathWithPlanner` BV `0x4767f0`): passa → passagem 0, custo 1. O
  VM tira o objeto do plano quando a árvore deixa (`intersectionAllowed`):
  o mesmo. Qualquer passagem > 1 numa célula a bloqueia
  (`cQuadTreeCell::UpdateTraversalCostAndPassability`, BV `0xba802c`: a
  célula fica com o maior estado dos seus retângulos; > 1 → custo `FLT_MAX`).
- Não identificado: com o bit 0 de `+0x15e` do objeto o tile inteiro (o
  tile da posição, x..x+1, y..y+1) entra também, com o custo da penalidade.
- Água: `WorkOutTileCostAndPassability` só olha água (7 parcial, 15 toda)
  nos tiles do **nível 0** (`cFloorTile +8` = 0: o terreno). A piscina não é
  água para o planejador: é o **andar −1** (§8.5).

### Custo do piso (`WorkOutTileCostAndPassability`)

```
custo = custo do material do piso (FloorTileRoutingCost), preso em [0,5, 10];
        sem custo no material → o padrão do gerenciador (+0x34)
água (IsPartWater): passagem 15; folga baixa até o andar de cima (FUN_00165c88): 7; custo FLT_MAX (§9.1)
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

(**Corrigido na rodada n18**, §9.1: qualquer colisão no horizonte bloqueia e
chama o `InterruptActiveLocoState(2)`; os 2 passos decidem só se a parada
cabe antes da colisão ou se o andar acaba na hora com o `EndFollow`.)

**Rodada 22, o teste entre duas pessoas** (`EstimateWhenPathWillBeBlocked`,
BV `0x6b3574`, caso humano; o teste varrido é a vtable `0x474`, BV
`0x6b00c8`: a primeira raiz de |(p − q)(t)| = r em [0, 1], ou já dentro):
um primeiro teste a 0,633 (0,283 + 0,35) entre os centros; depois, entre
dois humanos (vtable `0x98` dos dois), **bloqueia se os centros chegam a
menos de 0,283, ou se o ponto 0,35 à frente de quem anda (na direção do
movimento, `0x468`) chega a menos de 0,283 do centro do outro**; a idade
não entra (só a espécie: animais têm outros números). Antes o VM usava
0,5 em toda volta (inferido): um Sim ao lado ou atrás bloqueava, e dois
Sims parados a ~0,52 um do outro nunca saíam do lugar (o pedido para sair
e o que esperava, cada passo "bloqueado": N001_Lot68). Com as paredes
infladas 0,125 o centro anda até 0,375 fora do meio de um corredor de 1
tile: um Sim parado sozinho no corredor é ultrapassado.
**Rodada n03, o resto do `CollisionCheck`** (BV `0x6b49e8`) e quem entra no
`EstimateWhenPathWillBeBlocked` (BV `0x6b3574`): ficam de fora o próprio
Sim, quem está fora do mundo (`0x2e0`), quem pode ser atravessado
(`CanIntersectPeople`, `0x1d8`), quem está contido (`IsContained`,
`0x11c`), outro andar (`GetLevel`, `0x90`) e **um ID a ignorar**: quando a
janela acha um bloqueador que está se afastando (a distância no fim da
janela não é menor que no começo), o `CollisionCheck` passa esse ID como o
"ignorar" das janelas seguintes, e a varredura segue sem ele (o VM agora
faz assim; antes voltava a testá-lo na janela seguinte). Não há no binário
a exceção do Sim que é o alvo da rota nem a de "já sobrepostos (< 0,3)":
as duas seguiam no VM como inferidas (sem elas dois Sims numa conversa ou
encostados ficavam "bloqueados" para sempre); **saíram na rodada n18** (§9.1),
com a regra do binário (só se larga quem já toca e se afasta).

Também (inferido): quem começa a rota dentro da inflação de um objeto sai
para o ponto mais perto da célula livre mais perto, não para o meio dela
(o meio podia ficar na direção do outro Sim, e esse primeiro passo era
"bloqueado" para sempre).

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

**Rodada 19**: o "bloqueado por mim" do outro é o bloqueador que o
locomotor dele guardou ao parar (`CollisionCheck` → `InterruptActiveLocoState(2)`,
lido pelo `DoBlocked`, vtable `0x1A8`), não só o da rota depois do
`DoBlocked` dele: dois Sims que se encontram de frente param um para o outro
com um tick de diferença, e o primeiro a chegar ao `DoBlocked` tomava o outro
por "roteando, bloqueado por outra coisa" e desistia (14) — dois Sims
mandados juntos para a mesma cama (N002_Lot10). E o `DoContinueAfterPause`
→ `DoInitial` → `DoFoundPath` são estados próprios: depois da pausa o
andar continua no tick seguinte (no mesmo tick o mesmo bloqueador contava a
2ª tentativa na hora e a rota falhava; "Route To Can" com "CT - Route
Failure Feedback" dava centenas de voltas num tick no N001_Lot68).

**Rodada 22, o ramo "bloqueado por mim" lido de novo** (BV `0x57a8d4`):
depois do `TryGetSelfOutOfWay` (só com `0xd8` da rota == 0, a contagem de
"sair do caminho" que ele incrementa, e contador < 3), se a rota corrente
do bloqueador (`0x68`) tem `0xd0` ligado, **o contador volta um e pausa**
(`DoPause`). `0xd0`/`0xd4` é a marca da rota de "sair do caminho" (o
`TryGetSelfOutOfWay` chama `0xd4(1)` na rota nova) e `0x140` põe nela o
bloqueador = quem ele deixa passar (`0x13c` é o que o outro lê como "o
bloqueador dele"). Ou seja: quem já saiu do caminho uma vez e encontra o
outro saindo do caminho por ele espera sem gastar tentativa. E a rota de
sair do caminho é empurrada sobre a rota (pilha > 1): bloqueada, falha só
ela (14) e a rota volta (o VM planeja de novo; o que a rota de baixo faz
nesse retorno não foi lido, inferido). Dois Sims mandados para a mesma
cama no mesmo tick saíam os dois do caminho, se encontravam de novo e
pausavam igual até a 5ª tentativa; agora deitam os dois (`test_hood_vm`).

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

**Rodada 22 (`r22-door`, inferido):** o trecho cortado a 0,2 sai do ponto
deslocado só quando a reta dele até o primeiro ponto da rota está livre no
planejador (fora das inflações); senão sai de onde o Sim está (os pontos de
corte antes do deslocamento são pulados), e o andar volta para a reta que o
planejador deixou livre. Antes, a visita convidada do N001_Lot68 virava com
"turnAndGo-135", andava paralela ao caminho planejado 0,13 tile para o lado e
entrava na pegada da caixa de correio (falha 15 na batida à porta).

**Quem atende a porta (rodada 22, dados):** ninguém da família por conta
própria. A visita bate ("CT - Knock on Door" da porta: campainha, mensagem 55
aos Sims dos cômodos dos atributos 3/4 da porta, que só fazem "Look At") e
fica com o dado de pessoa 34 "Greet Status" = 0 esperando; "Cumprimentar" (TTAB
da pessoa, `0x2066`/`0x2067`) não tem anúncio de motivo e nunca entra no Find
Best Interaction; o "Emitter - Be Greeted" (awareness) de quem chega a pé nasce
com a relação 0 = 1 para todos ("já visto"), só a volta do trabalho/escola, o
carro e o jornal o fazem com 0. O jogador cumprimenta; com o status > 0 o
portal da porta deixa a visita passar.

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

**Feito antes (rodada 13, routing2)** (`routing_plan.cpp` `shapeWalk`/`turnSteps`/
`turnPoint`): o caminho cortado a cada 0,2 nos trechos do chão; a saída
pela frente para quem replaneja andando; virar no lugar
(`<idade>-TurnInPlace-45/90/135/180`) antes de sair quando o primeiro trecho
passa de 40° e no fim para a direção final. O front "walking" trocou o virar
no lugar da saída pelo `turnAndGo` (o virar no lugar fica para rotas sem
clipes de começo e para a virada final quando não houve parada) e a saída
passou a valer para todos, girada pelos passos. Captura antiga:
`docs/screenshots/r13-virar-no-lugar.jpg`.

**Feito no opensims2 (rodada 13, front "walking")** (`locomotion.cpp`
`gaitOf`/`locoClip`, `routing_loco.cpp` `locoStep`/`stepBlocker`, `sim_motion.cpp`,
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

**Feito na rodada 14 (front "walking2")** (`routing_loco.cpp` `locoStep`/
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
`routing_loco.cpp` `locoStep`/`turnClip`/`sideStep`/`stepBlocker`; testes
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
- **Rodada 15 (front "anim3"): o ajuste lido até o fim e feito**
  (`fitStride` em `locomotion.cpp`; `routing_loco.cpp` `fit`). Os papéis que
  faltavam: `L`/`Ls` são as `LocoDistance` (× escala) do ciclo longo e do
  curto (walk 1,393, walkShort 0,683 por ciclo: a mesma "passada" que o
  walking3 usava); `0x178`/`0x188` do pedido de locomoção são as porções
  de começo e de fim (`PortionCompleted`/`EndPortionCompleted`), e os
  termos de fase só existem **sem** clipe de começo (`f₀ = (1 − porção)
  mod 0,5`, o resto do meio ciclo em que o Sim já anda) e **sem** clipe de
  parada (`f₁` = porção de fim mod 0,5); com começo e parada os dois são 0.
  Constantes lidas no binário: 0,3 (`0x274b670`), ±0,25 (`0x274b54c`,
  `0x274b7b4`), −10 (`0x274b6e0`), 0,9 (`0x274b6d0`), 4 (`0x274b488`).
  O laço acima de 0,3: `c' = c − ½`, `v` (peso curto da parada) de 1 para
  baixo em 0,1, `w = num(v)/den` com `num = d − parada(v) − começo_l −
  L(c' + ¼) − ¼·passada(v)` e `den = (c' + ¼)(Ls − L) + (começo_s −
  começo_l)`, até `w ≥ 0` (dez tentativas; senão `w = 0`): ou seja,
  `começo(w) + (c' + ¼)·passada(w) + ¼·passada(v) + parada(v) = d`. A
  parada planejada leva o seu peso (`Loco::stopWeight`). Inferidos: o pé
  da parada pela porção em que o andar termina (o primeiro pé do binário
  vem do getter `0x178` e vira a cada `n` ímpar, até duas vezes, como
  aqui), `v` limitado a [0, 1] e o ramo das rotas curtas (`0,9·(começo +
  parada) ≥ d`, com `(d − 0,9·s)/(4 − 0,9·s)`) como antes. Teste:
  `test_routing` "fit stride".
- **(Corrigido na rodada n14, `docs/anim-mac.md` §8.2–8.3: a vtable `0x41c` é o `LocoPlaceFromSceneGraph`, não o `BeginFollow`; a primeira fase não começa adiantada, os 5/100 ms são o começo da fase seguinte em EndTicks − 5/100 quando o pedido anterior começa a sair, e o `DAT_02c11c04` vale 140.)** Os 5/100 ms, lidos de novo (`FollowOneStep`, BV `0x6b80e8`): não são
  um adiantamento de cada fase. Quando o `BeginFollow` (vtable `0x41c`)
  monta o caminho, a **primeira** fase é criada com o início em
  `agora − 5` (ou `− 100` com `+0x24c > 0,25`, em ticks de animação = ms):
  ela já começa 5/100 ms adiantada. As seguintes (estado 3) começam no
  **fim exato da anterior** (`+0x1ac`) se ele foi há menos de N ms (N =
  25 com `+0x24c ≤ 0,25`; senão `DAT_02c11c04 × 0,5`, um global sem
  escrita achada), senão agora. Feito: a primeira fase (começo ou passo de
  lado) com `Loco::lead` = 5/100 ms em ticks da VM (o quadro já anda isso,
  o clipe é tocado desde lá); o encadeamento exato em ms das fases
  seguintes não (falta o N do caso longo; as fases seguem encadeando tick
  a tick). O desenho continua fundindo as fases nesse tempo.
- **O tempo de mistura das poses `*Blend`** (`PushTurnBlendCycles`, BV
  `0xd3107c`, e o `+0xec` do `InitChannelNetwork`): 0,2 (`0x274b644`),
  limitado a 0,95 (`0x274bc94`) da distância do clipe de começo (com ele)
  e da do clipe de parada (com ele). Feito (`locoPoseBlend`, as camadas
  de pose com o seu `fade`); inferido: a unidade como distância ao longo
  do caminho (os ciclos de locomoção andam por distância), convertida em
  segundos pela velocidade do andar.
- Soak com `OS2_SEED` 1–4 em N001_Lot68, N002_Lot10, N001_Lot16 e
  N001_Lot11 (43.200 ticks cada), antes → depois do ajuste e do
  adiantamento: interações terminadas com verdadeiro 1.338 → 1.473, com
  falso 591 → 619, buscas 1.926 → 2.066, humor médio final 7,8 → 10,4,
  nenhuma morte (as execuções divergem cedo; a soma de quatro sementes não
  mostra perda).
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

**Rodada n14 (locomoção, `docs/anim-mac.md` §8):** lidos e feitos o relógio
das fases (`ComputeLocoStartTicks`, a entrega do `FollowOneStep` quando o
pedido começa a sair, o N = 25/70 do estado 3, `DAT_02c11c04` = 140), as
trocas dentro do pedido (`cLocoCycle`: `BlendDistance` `+0xec`, rampa
cossenoidal), o pé da parada (getter `0x178` = `CyclePortionCompleted`), o
ramo das rotas curtas do ajuste (0,9, espremido) e o `StopLocomotion` (meio
ciclo e parada; o teste por passo bloqueia sem clipe). A rota acaba quando
a parada começa a sair (233,3 ms antes do fim). Os "Inferido" acima sobre o
`walkShort` "por porção" e o primeiro pé saem; os que ficam estão no §8.8
de lá.

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
Rodada 18: o `TriggerBlockedAnimation` (BV `0x579bc0`) passa as pausas no
parâmetro 0 e o bloqueador no 1 (a árvore verifica o 1 como objeto e escolhe
a animação pelo 0); o `TriggerAskToMoveAnimation` (`0x579798`) não passa
parâmetros. As árvores globais `CT - Route Blocked Animation`, `CT - Route Waiting
Animation`, `CT - Route Ask To Move Animation`, `CT - Route Blocked` e
`CT - Route Failure Feedback` são chamadas pelo nome (strings no binário,
BV `0x0102366c..`).

### 7.1 Orçamento de nós por turno das árvores da rota (binário, rodada n29)

O orçamento que importa para as rotas é o das **árvores** que o roteador
chama no meio do plano ou do passo: o guarda do portal, a "allow
intersection" (entrada 5), as checagens do objetivo, a pós-rota e as globais
"CT - Route ...". O A* do §4 não roda árvore e não usa esse contador (se o A*
do binário tem um teto próprio de nós abertos, não foi lido). Cada árvore roda
no `cTSTreeSim` de quem anda, e o `cTSTreeSim::DoNodeAction` (BV `0xe1b18c`,
lido na rodada 16) sobe o contador `+0xc` a cada nó e, passando de
`DAT_0277ea94` = **100000**, lança o erro `0x44D` "Too many iterations"
(`docs/primitives-mac.md` §4g "Limite de nós por turno"; com uma flag de
depuração, bit 0 do valor `0x33`, não entendida, o limite é 300000; a pilha de
1000 quadros é o `DAT_0277ea90`, vizinho dele). É o mesmo valor em qualquer
árvore, no jogo e na ferramenta.

No código o valor é um só: `vm::kNodeLimitPerTurn` (`src/vm/vm.hpp`).
`VM::instructionsPerTurn` e o padrão de `VM::run` partem dele (as árvores das
rotas rodam com `vm.run(t, vm.instructionsPerTurn)`: `portalLockedFor` em
`routing_plan.cpp`, `allowsIntersection` em `routing.cpp`), o app (`LotVM`) o
usa, e todo comando do `s2pkg` que roda o VM o pega por `s2pkgBudget()`
(`cli/s2pkg/budget.hpp`; `S2PKG_BUDGET=n` muda, para experimentar). Na rodada
15 o `autonomy` passou de 20000 aos 100000 e na 16 (commit `bd77823f`)
`lot-run`, `pie`, `route`, `buy`, `tutorial`, `wants`, `lot-save`, `bhav` e
`bhav-sweep` também; a rodada n29 tirou o último 100000 escrito à mão
(`lot-save`) e deu a todos a mesma constante, com um teste (`test_vm`: o valor
e um laço cortado em 100000 nós).

**Medida** (build da rodada n29, 68 dos 69 lotes: o `N002_Lot32` não é lote;
`lot-run` com `S2PKG_BUDGET=20000` ("antes") e o padrão 100000 ("depois"), o
mesmo binário, `OS2_ROUTE_FAILS=1` para contar as rotas que falharam):

| | 1800 ticks, antes (20000) | 1800 ticks, depois (100000) | 20000 ticks, antes | 20000 ticks, depois |
|---|---|---|---|---|
| rotas que falharam (todos os lotes) | 57 | 57 | 513 | 513 |
| lotes com a saída ou a lista de falhas diferente | | 0 de 68 | | 0 de 68 |
| falhas "instruction budget exhausted" | 0 | 0 | 0 | 0 |
| ciclos (soma) | 132,8 G | 133,0 G | 422,3 G | 416,9 G |
| instruções retiradas (soma) | 382,2 G | 381,8 G | 1079,4 G | 1078,9 G |
| tempo de parede (soma; a máquina estava carregada) | 57,6 s | 57,4 s | 226,6 s | 219,1 s |

Repetido depois de rebasear sobre a main com a rodada n18b (`8fad03dd`), 1800
ticks: 133 rotas que falharam nas duas colunas (o total mudou com as rotas do
n18b, não com o limite), saída idêntica nos 68 lotes, ciclos 136,9 G e
137,5 G.

Nenhum "sem rota" some e nenhum aparece: a saída do `lot-run` é idêntica nos
68 lotes e a lista das rotas que falharam (tick, Sim, código, árvore) também.
Os códigos são os mesmos nas duas colunas (1800 ticks: 2 = caminho não achado
29, 13 = objetivo indisponível 15, 11 = pré-rota 6, 14 = bloqueado por pessoa 4,
7 = destino ocupado 2, 9 = parede 1). O custo é ruído: o limite é só o valor inicial de
um contador do `VM::run`; repetindo o mesmo comando 6 vezes com o mesmo limite
(N001_Lot36 e N003_Lot192, 1800 ticks), os ciclos variam até 14% de uma
repetição a outra e as instruções 1%, mais que a diferença entre os dois
limites. Mais longe: `lot-run` do `N003_Lot192` por 80000 ticks (344 rotas
que falharam nas duas) e `autonomy` com `OS2_SEED=2` até o tick 78000 (2702
rotas que falharam nas duas) também não estouram o 20000 em turno nenhum; o
"Controller - Relationship Decay" de 22.798 nós (tick 77371, semente 2) que a
rodada 16 achou não reapareceu com o código de hoje (inferido: o estado
daquele tick mudou com as rodadas seguintes). Ou seja: o 20000 das
ferramentas era um erro latente (podia dar um falso "Too many iterations"
e reiniciar a Main, como deu uma vez), não um que escondesse rotas nos lotes
dos dados de hoje.

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
entrada/saída (`vtable 0x380`), os andares dos dois lados (rodada 15: não as salas) e os pares de
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
slots do portal, os andares e o nível; o Sim fica em `DoWaitingForPortal`
(estado 2) até a árvore voltar (ela anima a porta abrindo, ou sobe os
degraus com Snap Into) e o roteador segue no fim do portal
(`DoLocomotionContinuationAtPortalEnd`). Se outro Sim ocupa o portal (dado
de objeto 0x3E do portal ≠ 0), espera em `DoWaitingForPortalToBecomeFree`
(animação de espera de 24 ticks, até 126 ticks). Antes de rodar, tira o
bit 0x40 do dado 0x44 do Sim.

**A folha da porta** (rodada 16, dados): a árvore de portal das portas
(`Function - Portal - SG`, semi-global `0x7FD0DEBA` `0x200A`) chama `Set door
open` (`0x200D`) **uma vez**, com o lado em temp 1 (`0x2081`/`0x2082`); o
`0x200D` toca **uma folha**: a direita (`r_door_rot`) com o Animate Overlay
sem flip, ou a esquerda com o mesmo nome **com flip** (byte 2 = 1; o espelho
leva `r_` em `l_`). A lista de animações (STR# `0x86` do semi-global) é 1
`o-door-open-CCW`, 2 `o-door-close-CW`, 3 `o-door-open-CW`, 4
`o-door-close-CCW`: a folha que abriu CCW (−91°, último quadro) fecha com a
CW, que começa em −91° (e vice-versa). Numa porta de 2 tiles o Sim que passa
abre só a folha do seu lado (pelo código; não conferido no jogo). Os lotes
do jogo base guardam as portas com os ossos na pose de repouso (fechadas):
nenhum nó `r_door_*`/`l_door_*` salvo difere do repouso em `N001_Lot82`,
`N001_Lot11`, `N002_Lot10` e `N001_Lot68`. No app, cada overlay que a VM
toca num objeto é uma camada própria (`LotSimulation::onObjectOverlay`,
prioridade e nível 1 + o id do overlay) tirada quando a VM o tira, com o
objeto mantendo a pose daquele momento; antes a folha com flip substituía a
camada da outra e a fechava de golpe. `OS2_DEBUG_DOORS=1` lista os ângulos
das folhas numa corrida sem tela.

**Os parâmetros 2 e 3 são os andares** (rodada 15): o
`TSObjectPortalLockedCB` (BV `0xe208c8`) monta os parâmetros da árvore de
teste assim: 0/1 = `vtable 0x380` (o slot de contenção de cada ponta), **2/3
= o andar (`vtable 0x90`) do objeto de entrada e do de saída**, 4/5 os IDs,
6/7 os slots de rota de cada ponta. O `TriggerPortalTree` monta os mesmos:
2/3 de `cTSRoutePortalInfo +8`/`+0x1c`, que `GetPortalInfoFromPathNode` (BV
`0x472514`) enche com o **andar do nó do caminho + 1** de cada ponta. O VM passava as **salas** (desde a rodada 6); a escada da piscina
e o trampolim decidem por eles: `param 2 > param 3` = descer (entrar na
água), e o TEST do trampolim só deixa descer.

### 8.5 Portais de objeto e a piscina (rodada 15, binário)

**A piscina é um andar**: o lote com piscina tem o andar −1 (VM 0), o fundo
da piscina é piso dele ("floor_poolroundedlipblue") e as paredes de piscina
(tipo 29) o fecham. O planejador não trata água de piscina: um Sim nadando
está no andar −1 e anda nesse piso. Sai por um **portal de objeto**:

- `cTSObject::AddThisToPathPlanner` chama `PortalAddToPathPlanner` (vtable
  `0x588`) de todo objeto que `IsPortal` (vtable `0x374`: tem pares de slots
  de portal); num multi-tile só o líder (`cEdithMTObject::
  PortalAddToPathPlanner`, BV `0xaf1884`).
- `PortalUpdateSlotInfoList` → `PortalFillInSlotInfoList` (vtable `0x564`, BV
  `0xe21a18`; o multi-tile percorre as partes) → `PortalGatherSlotInformation`
  (vtable `0x568`, BV `0xe1ff3c`): para cada slot de rota do objeto, os de
  porta/escada (8, 10, 11, 12) pelos nomes gerados
  (`TSObjectAutoPortalConnectionNameGenerator`); os outros só se o slot é
  **par de portal** (`cTSRoutingSlot::IsPortalPair`): o nome do slot (osso,
  `vtable 0x144` índice 0) e o nome da outra ponta (índice 1). Nos dados os
  dois nomes são a entrada do STR# 0x90 ("Model - Bones") do slot,
  **"meu,outro"**. O par é a flag **`0x200000`** do SLOT (confirmado na rodada n03, §8.6). Nos
  2.991 slots de rota do `objects.package` ela só aparece em 4 grupos: a
  escada da piscina, o trampolim, o "Route - Step Over" (o portal de passar
  por cima, `UpdateStepOverPortal`) e dois slots de uma poltrona (nomes sem
  vírgula: nenhum par); a `0x80000`, que as duas também têm, está em 72
  grupos. A escada tem ainda `0x400000`/`0x800000` (os
  `DisableLocomotionContinuationAt{Start,End}OfPortalPair`). No
  `cTSRoutingSlot` o par é o bit `0x200000` das RS flags (rodada n03,
  §8.6: confirmado; o `0x100000` de antes era a vtable errada).
- `PortalConnectSlotInformation` (BV `0xe1f878`) liga cada par ao slot cujo
  nome é o "outro"; `PortalAddToPathPlannerImpl` (BV `0xe22a44`) chama o
  `AddPortal` do gerente (`cTSRouteManager::AddPortal` → `cPathPlanner::
  AddPortal`, BV `0x6064b4`) entre os pontos dos dois slots
  (`PortalGetLocationOfSlot`: o osso no mundo, andar da parte − 1), custo 1,
  trava = a árvore 15. O `cPortal` tem um bool de mão única
  (`cPortal::IsOneWay`): `MakeNodesForPortal` (BV `0x604584`) liga a ponta A à
  B e só liga B à A quando não é de mão única. Inferido: cada ligação é um
  portal A → B; como os dois slots da escada (e do trampolim) se nomeiam um
  ao outro, dá ida e volta.
- **Escada da piscina** ("Pool Ladder", grupo `0x7F585FFD`): a parte "0,0,0"
  no andar −1 (slot "route_poolladder_0_n,route_poolladder_0_s") e a "1,0,1"
  no deque ("route_poolladder_0_s,route_poolladder_0_n"); a função 15 das
  duas é "Function - Portal" (`0x1006`): `param 2 > param 3` → "Sub - Get In"
  (`0x1008`), senão "Sub - Get Out" (`0x1009`, "a2o-poolLadder-climbOut").
- **Trampolim** ("Pool Diving Board", `0x7F5BA5F7`): "route_pooldivingboard_0_s"
  no deque e "slot_divingboard_0" na água; o TEST (`0x1005`) só passa com
  `param 2 > param 3`: não se sobe pelo trampolim.

No VM: `VM::refreshObjectPortals` (ao carregar o lote e ao mudar o mundo no
modo construção) junta os pares em `World::objectPortals` e o planejador faz
deles portais (`Portal::object`, `oneWay` quando o outro slot não nomeia
este); a árvore de teste e a de verdade são as da função 15 da parte de
entrada. `N001_Lot68`: a Mary-Sue mergulha (o trampolim, deque → água),
nada até a escada e sobe (antes ficava no andar −1 sem rota e morria de
fome). Rodada 16: o modo compra também refaz os portais (pôr, mover, pegar,
vender ou apagar um objeto: `applyPlacement`, `pickUp`, `deleteGroup` em
`vm/placement.cpp`); `N001_Lot68`: a escada movida para (3, 12) liga
(3,5; 13) e (3,5; 11,25), e vendida some o portal dela.

### 8.6 Escadas como portais de slot: duas faixas de mão única (rodada n03, binário, Ghidra MCP)

O jogo **não** faz o portal da escada pela geometria dos degraus: ele vem
dos **slots de rota** das peças, como o da escada da piscina (§8.5). O VM
fazia um portal só por escada, do meio do patamar de baixo ao meio do de
cima, nos dois sentidos; dois Sims em sentidos opostos se encontravam no
mesmo ponto do patamar e um esperava o outro para sempre (`N001_Lot11`, o
Mortimer subindo a escada da varanda e o Alexander descendo:
"blocked by 142" e falhas 14/9 até o fim).

**Os bits do `cTSRoutingSlot`** (vtables lidas nesta rodada; o objeto tem
duas: a primária, `cITSSlot`, e a de `cITSRoutingSlot` em `+0x20`, cujas
entradas são thunks `this -= 0x20`):
- `Init` (BV `0x4ae920`, depois do `cTSSlot::Init` `0x4ae7e0`): `+0x30` =
  as **RS flags** do descritor **sem conversão** (`cTSSlotDescriptor` vtable
  `0x64` = `+0x38`, a 4ª u32 depois do x,y,z no SLOT: o nosso
  `Slots::Slot::flags`); `+0x24/+0x28/+0x2c` os multiplicadores (em pé,
  sentado, chão), `+0x34` o snap target, `+0x38..+0x44` mín/máx/ótimo e o
  gradiente.
- `cITSRoutingSlot` vtable `0xa8` `IsPortalPair` = flags & **`0x200000`**
  (BV `0x4aede4` pelo thunk `0x16732c0`); `0xac` = `0x400000`
  (`DisableLocomotionContinuationAtStartOfPortalPair`), `0xb0` = `0x800000`
  (`...AtEnd...`), `0xb4` `IsAssociatedWithAContainmentSlot` = `0x200000`
  **e** `0x2000000`. **Confirmado** o `0x200000` que a rodada 15 inferiu
  (a leitura de `+0x30 >> 20` de antes era a vtable primária, que não é a
  que o `PortalGatherSlotInformation` chama).
- `cITSSlot` vtable `0x20` = `+0x18` = o u32 do descritor em `+0x24` (no
  SLOT, o que vem depois do "max size": o nosso `unknown10`), e `0x24` =
  `+0x1c` = o índice do nome (`ts2Extra[0]`). `cTSObject::ModelBoneName`
  (BV `0xafbb30`) só dá nome a slot com **`0x20000`** nesse campo; o nome é
  a entrada do STR# `0x90` do slot, partida por vírgulas (índice 0 = o
  próprio, 1 = o outro).
- `cTSSlotDescriptor::Read` (BV `0x4b08a4`): a ordem é a do nosso parser
  (tipo, x, y, z, 3 multiplicadores, RS flags, snap, mín/máx/ótimo,
  `+0x20`, `+0x24`, gradiente, altura, facing, resolução, os 2 u32 do TS2).
  Ele escala x,y por 1/16 até a "versão" `0x46` e as proximidades inteiras
  por 1/16 entre 9 e `0x42`; os SLOT `0x43`–`0x46` do jogo base já têm x,y
  em tiles (0,7, 0,495…), então a versão comparada ali não é a do
  cabeçalho que o nosso parser lê (não conferido; o parser fica como está).

**Quem entra** (`PortalFillInSlotInfoList`, BV `0xe21a18`, e o do
multi-tile `0xaf17f4`, que percorre as partes no líder):
- objeto de tipo `0xc` (o "Portal - A" da escada modular): os slots dele e
  os do **outro extremo** (`PortalGetOtherEndOfStairs`, BV `0xe21144`: o
  último objeto do mesmo grupo de objetos, fora ele, de tipo 11 ou 12, na
  prática o "... - End"); os "Bridge" do meio não entram;
- os outros: os slots do próprio objeto (no multi-tile, de todas as
  partes, na lista do líder);
- depois `PortalGatherDynamicSlotInfo` (BV `0xe213b4`) junta dois portais
  dinâmicos dos dados de objeto `0x58`–`0x61` (objeto, slot, GUID), que só
  o BV usa (o jogo base tem 88 dados de objeto, até o 87).

`PortalGatherSlotInformation` (BV `0xe1ff3c`), para cada slot de rota com
nome (`ModelBoneName`; nomes repetidos ficam com o primeiro):
- par de portal (`0x200000`): a ligação "meu nome → outro nome" (§8.5);
- senão, só nos tipos **8, 10, 11, 12** (porta, escada reta, escada
  modular, portal da modular): o outro nome sai de
  `TSObjectAutoPortalConnectionNameGenerator` (BV `0xe1fa5c`), que troca o
  sufixo: `_n`↔`_s`, `_e`↔`_w`, `_nw`↔`_se`, `_ne`↔`_sw`,
  **`_top`↔`_bottom`** (strings em BV `0x1045eb4`). Cada slot gera uma
  ligação; `PortalConnectSlotInformation` (BV `0xe1f878`) acha o slot com o
  outro nome e descarta as ligações sem par.

Cada peça das escadas do jogo base tem **dois** slots de rota (flags `0x800`
ou `0x1000` no slot 0, `0x1800` no slot 1; `0x20000` no campo do nome):
- escada reta: o pad de baixo ("Lo 0,5 Pad") tem `route_staircasestraight_1_bottom`
  (slot 0) e `_0_bottom` (slot 1); o de cima ("Hi 0,0 Pad") tem `_0_top`
  (slot 0) e `_1_top` (slot 1);
- escada modular: o "Portal - A" tem `route_modularStair_0_top` (slot 0) e
  `_1_top` (slot 1); o "End", `_1_bottom` (slot 0) e `_0_bottom` (slot 1).

São **duas faixas** lado a lado, a 0,33 tile uma da outra: `_0_*` e `_1_*`.

**A direção** (`PortalAddToPathPlannerImpl`, BV `0xe22a44`): para cada
ligação A → B, o objeto do slot A precisa ter a entrada 15 (senão a ligação
não vira portal); nos tipos 10, 11 e 12 o portal sai da ponta cujo slot é o
**de número 0**: A é slot 0 → A → B; senão, B é slot 0 → B → A; nenhum dos
dois é 0 → A → B de mão dupla. Com slot 0 o portal é de **mão única**: o
segundo bool do `cTSRouteManager::AddPortal` (BV `0x4f26f4`) vai ao `+0x61`
do `cPortal` (construtor BV `0xba3448`), que é o `IsOneWay` (BV `0xba2f04`,
`lbz +0x61`), e `MakeNodesForPortal` (BV `0x604584`) só liga B → A sem ele.
Resultado nas duas escadas: a faixa `_0` é a de **descer** (do slot 0 de
cima ao de baixo) e a `_1` a de **subir** (do slot 0 de baixo ao de cima);
quem sobe e quem desce não se cruzam (cada um pela sua direita). A
posição de cada ponta (`PortalGetLocationOfSlot`, BV `0xe21b6c`) é a do
objeto mais o deslocamento do slot girado pela direção dele, no andar do
objeto; a direção (`PortalGetDirectionOfSlot`) é a do objeto mais a do
slot. Portas (tipo 8) usam o mesmo par de nomes mas vão pelo
`AddDoorPortal`; as escadas reta e modular (tipos 10 e 12) põem ainda
retângulos de custo 10 junto das pontas conforme o tipo de modelo
(`TSObjectGetModelType` 2–4, ±0,32499; não feito).

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

**Nadar no app (rodada 16, QA ao vivo passada 13).** O "Mergulhar" deixava o
Sim em T-pose: a troca de roupa (`onOutfitChange`) dava ao Sim o modelo do
maiô com o mesmo número de partes, e `SkinnedModel::pose` só refazia a saída
quando o número de partes mudava, então o corpo antigo ficava sem pose (a
casca em T, sem cabeça, "na borda do lote"); agora refaz quando os vértices,
triângulos ou material de uma parte mudam. Na água o Sim fica na **superfície**
(`LotSimulation::floorHeight`: no fundo de uma piscina, o piso do andar da
piscina menos `depth`), porque os clipes de nado são feitos em torno dela
("a-swimSlow-idle": raiz 0,59 abaixo, pés 1,4 abaixo; inferido dos clipes);
parado na água toca "a-swimSlow-idle" no lugar do idle de pé (inferido). O
conjunto de locomoção de nado ("Loco - Anims" dos objetos da piscina) lista as
viradas sem papel ("swim-TurnInPlace,swimSlowTurnAndGoShort,..."): o papel sai
do nome (inferido), e o nadador vira com "a-swim-TurnInPlace-90" e para com
"a-swimSlowTurnAndStop-0" em vez do "a-TurnInPlace-90" de pé.
`docs/screenshots/r16-pool-dive-swim.jpg`. Os cinco ícones de fila iguais da
QA não se repetiram sem tela (uma entrada: o trampolim, depois "Pool -
Controller: Nadar"); provavelmente cliques repetidos enquanto nada parecia
acontecer.

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
(caminho + objetivo). Para cadeiras, `ScanConeForSittingGoals` (BV `0x4788cc`;
`docs/seat-mac.md` §A) usa o mesmo laço com o callback de slots de cadeira. O `cTSRoute::ScanCone` que o
symxfer nomeou no BV é outra função (um dump de desejos).

**Os objetivos em pé depois da varredura** (rodada n03, BV `0x478518`, o
callback de pontos é o `0x4740d8`): a cada objetivo, `placar += em pé × 10`
(`+0x4c` do pedido); com `HasRandomScoring` (vtable `0x78` do slot, RS
flag `0x2000`) `+ 20·sorteio − 10`; **+25** se a direção do objetivo
(`+0x20`) não está na máscara de paredes em volta do alvo (só quando o slot
não tem a RS flag `0x80000`, vtable `0x4c`; a máscara vem do mundo, vtable
`0x148`, com o lugar e a direção do alvo, e cada bit marcado espalha para
as duas direções vizinhas; **lida e feita na rodada n18**, §9.1: é o
`GetWallBlockFlagsAtTile` e a direção é a da última origem varrida, não a
do objetivo); **+20** (`HasStationaryBonus`, vtable `0x80` = RS flag `0x40000`
desligada) para o objetivo **exatamente onde o roteador está** (compara os
8 primeiros bytes do lugar, `FUN_01655330`, com o do roteador) se ele não
está contido (`IsContained`, vtable `0x11c`): feito; e com o byte `+0x24`
do pedido o placar vai × 0,01.

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
| **post route 0x26** (`TriggerPostRouteTree`, BV `0x577cf4`) | a árvore **do próprio Sim** que anda, stack object = ele | em `DoFailedOrSucceeded` (BV `0x577ea0`, estados 9/10), uma vez: só se a pilha de rotas tem **uma** rota, a rota tem o flag `+0xac` ligado e o `+0xad` desligado (o `+0xad` é ligado ao rodar), e o estado não pede para pular (`+0x4c`) | 0 = o `+0x10` do objetivo atual (a cadeira de um objetivo de sentar; 0 sem objetivo), 1 = código de resultado da rota (`+0x64`), 2–3 = 0 (corrigido na rodada n01: o u32 big-endian põe o `+0x10` primeiro; `docs/seat-mac.md` §A) | código 12 "post-route failed" (estado 7/8 → `DoWaitingForPostRouteTree`, BV `0x56ddac`) |
| **goal check 0x27** (`TriggerGoalCheckTree`, BV `0x575e34`) | a árvore 39 **do objeto dono do código do frame que chamou o Go To** (o frame logo abaixo do topo da pilha do Sim: o OBJf do seletor desse frame), rodada no Sim com stack object = o stack object desse frame (sem ele, o alvo da rota) | em `DoWalking` (BV `0x576acc`), **antes do passo**, quando `DoAlongRouteCheckTest` (vtable `0x60` da rota, BV `0x470fe4`) diz (caminho novo, sala ou andar mudou, mais de 1 andado no caminho; na 1ª vez de uma rota sempre) e o byte `+0x20` do gerente de rotas deixa (rodada n18b, §9.2; antes "a cada tile cruzado"); e **forçado** no `DoBlocked` com bloqueador (estado 0x19); antes chama `AlongRouteUpdate` | 0 = índice do slot de rota (`WorkOutSlotIndex`), 1 = `+0x14` do objetivo, 2 = `+0x10` do objetivo, 3 = distância restante (vtable `0x3c`) × 100 | código 13 "goal not available" (estado 4; bloqueado: estado 0x19 → 9) |
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

**Corrigido na rodada n30 (§9.3)**: `0xD0` é `SetDistances(mín., máx.,
ótima)`, `0xCC` `SetGradient` e `0xD4` `SetSpacing`; a leitura abaixo
(resolução, largura) ficou só como histórico. **Inferido** (antes): pela varredura de §8.2 (fileiras com passo `+0x38` e largura
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
| portais de objeto (pares de slots, a escada da piscina e o trampolim), parâmetros 2/3 das árvores de portal = andares (§8, §8.5) | desde a rodada 15; a flag `0x200000` do par e o portal de mão única por ligação conferidos na rodada n03 (§8.6) |
| escadas: portais dos slots das peças (nomes `_top`/`_bottom`), duas faixas de mão única a 0,33 tile (§8.6) | desde a rodada n03 (antes um portal só, de patamar a patamar, nos dois sentidos; `OS2_STAIR_ONE_PORTAL=1` volta ao antigo); o portal geométrico fica para escadas sem objetos |
| escadas (tipos 10, 11) no planejador pelas pegadas; exceção do campo `0x4b` = `0x93`; penalidade de rota com escala 0 (custo 1) | desde a rodada 15 (§2) |

## 9.1 Rodada n18 (rotas, resto): o que foi lido no binário

Leitura de 2026-10-15, **Ghidra MCP** (só leitura; as funções que o
projeto não tem como função foram lidas na listagem, `disassemble_bytes`
com `dry_run`, nada gravado). Endereços BV.

### O custo e a passagem de uma célula (`ePassabilityState`)

`cQuadTreeCell::UpdateTraversalCostAndPassability` (BV `0xba802c`): o
estado da célula é o **maior** estado dos retângulos que a cobrem e o custo
o **maior** custo (partindo de 1,0). Os retângulos com callback (`+0x24`,
os de "allow intersection" e de porta) são avaliados uma vez por contexto e
guardados (`+0x2c`/`+0x30`/`+0x34`); os do tipo 1 (`+0x1c`, uma forma com
interface `0xabfb0f11`) só contam se o meio da célula cai dentro da forma.
Uma célula **sem nenhum retângulo de piso** (flag `+0x1e & 1`) fica no
estado `0x11` (17, "sem piso"). Com um retângulo de tipo 0 a soma recomeça
ignorando os de estado > 1 (não identificado quem põe esse tipo). No fim:
**estado > 1 → custo `FLT_MAX`** (a célula bloqueia); 0 e 1 passam com o
maior custo. O A* (`IsSegmentPassableAndConstantCost`, BV `0xba6d94`) só
pergunta "estado > 1 ou custo > `FLT_MAX`/2". Os números são máscaras: bit 0
bloqueia, `2` pode-se passar por cima (3 = árvore "allow intersection", 9 =
`IsOKToFlyOver`), `4` deixa com a árvore (5), `8` objeto (11), 7 e 15 do
piso, 17 sem piso; **para o planejador do jogo base todos os > 1 são
iguais**. Ou seja: o "custo exato de cada estado" é este, e o VM já o tinha
(bloqueado = bits, livre = custo).

### O custo do piso (`WorkOutTileCostAndPassability`, BV `0x4f3d84`; lido de novo)

```
estado = 0; custo = 1
se FloorTileRoutingCost(tile): custo = "routingcost" do padrão do piso, preso em [0,5; 10]
senão:                          custo = +0x34 do gerenciador = 2,0 (construtor BV 0x4f9294)
só no nível 0 (cFloorTile +8 = 0):
    IsPartWater(tile)             → estado 15, custo FLT_MAX
    senão FUN_00165c88(tile, 1)   → estado 7,  custo FLT_MAX  (folga até o andar de cima menor que
                                    WorldDB 0x15c; não é "água parcial" como §2 dizia)
    estado ≤ 12: desnível h ≥ 1 → estado 15, FLT_MAX; 0,5 ≤ h < 1 → custo = (h − 0,5)·20 se maior
    +10 se WorldOutsideSupport (WorldDB vtable 0x170) 0x24(tile, 0) e 0x2c(tile) (não decifrado)
```

`FloorTileRoutingCost` (BV `0x1674e0`): o padrão do piso do tile (WorldDB
`0x9c`) no catálogo (`cTSGlobals` `0x40` → `0xa8(1, id, 1)`) e o
`RoutingCost` da entrada (`cTSProductPatternEntry +0x68`, interface `+0x50`,
vtable `0xd4`), lido do CPF do padrão (`ReadProperties`, BV `0x84fed8`,
propriedade **`routingcost`**; 0 se faltar, construtor BV `0x849f28`). Nos
dados (`Catalog/Patterns/catpatterns.bundle.package`): **136+2 pisos com 1**,
**8 com 4** (terra, cascalho, areia, cobertura de casca, asfalto e as duas
faixas de rua), as paredes 0. **Um tile sem piso (o terreno) custa 2,0**:
os Sims preferem pisos ao gramado e evitam areia, terra e asfalto. O
`+0x34` só é escrito no construtor (o comando de trapaça "routingCost" tem
classe própria, não seguido). O mesmo custo entra no placar dos objetivos
em pé (callback BV `0x4740d8`, pelo `0x58` do gerenciador): `placar −=
10·(custo − 1)` se custo > 1 (`0x274b470` = 1,0): um objetivo no gramado
perde 10 contra um no piso.

### O Snap num ponto só (`cTSObject::TrySnap`, BV `0xe43624` e `0xe43084`)

Modos 0/3 (slot de rota do alvo) e 4 (slot do SLOT global): **um ponto**,
`posição do alvo + TotalRoutingSlotOffsetFromContainer(slot, raio)` girado
pela orientação do alvo; o andar é o do alvo. O deslocamento
(`TotalRoutingSlotOffsetFromContainer` BV `0xaf7d90` →
`SlotOffsetFromContainer` BV `0xafa28c`): slot **com** `0x20000` (osso) →
a posição do **primeiro osso** do nome do slot (índice 0, `0x144`/`0x4dc`;
sem osso no modelo, um valor fixo); **sem** → o vetor do slot (x, y, z do
SLOT) mais o raio do Sim girado pela direção do slot. O raio é o `+0x210`
do Sim (o "slot displacement", 0 no jogo base), zerado com a RS flag
`0x20000000` e de sinal trocado com `0x10000000`. Sem varredura e sem
anel. Modo 2 ("na frente"): o tile vizinho na direção do alvo
(`FUN_01703bc0`). Direção: com `0xE4` (RS `0x1000`) a do alvo + a do slot
(osso: `SlotDirectionFromContainer`; senão `0xE8`), senão a do Sim.

O teste (`TrySnap` interno, BV `0xe43084`), na ordem:
1. pessoa sem o bit `0x04` do dado de pessoa 68: um círculo de **0,3** em
   volta do ponto novo toca parede (`FUN_008a6c84`, o mesmo teste do código
   4 do `EvalTileForGoal`) → erro **8**;
2. slot sem a RS flag `0x800` (vtable `0x44`, "ignorar salas"): o ponto
   fora do lote → erro 2; a sala do ponto novo diferente da do velho
   (gerente de partições `0x13c`, inferido que é a sala) → falha;
3. slot sem a RS flag `0x80000` (vtable `0x4c`), mesmo andar: parede entre o
   lugar velho e o novo (gerente `0xa0(5)` `0x4c`), não porta, que bloqueia
   → erro **8**;
4. `TSObjectCanSetPositionAndOrientation` no ponto (com contêiner,
   `TSObjectCanPlaceInSlot`): **sem lista de exceções** (o alvo conta como
   qualquer objeto).
Falha: od 52 = o primeiro erro + 2 (1 quando pediu a um Sim para sair).

### O +25 dos objetivos em pé (`FUN_00478518`)

A máscara é `cEdithObjectModule::GetWallBlockFlagsAtTile(tile do alvo,
direção do alvo)` (BV `0x524880` → `FUN_00af634c`; o "mundo" do §8.2 é o
`ObjectManager` do `cTSGlobals`, vtable `0x68`): para os segmentos de parede
do tile (`FUN_0156003c` + partições `0xb0`), segmento 2 → bit 0, 4 → bit 2,
8 → bit 4, 1 → bit 6 (as diagonais não entram), o que no VM é **o bit 2k =
parede na aresta da direção 2k** (como `tileWallSegments`); depois girada
para o sentido do alvo (bit `a` → `a − dir`). Cada bit marcado espalha para
`±1`. O teste é com o `+0x20` do **contexto** da varredura (r30, não o
objetivo): a varredura (`FUN_004778b4`) escreve ali a direção de cada
origem que percorre, então vale a **última** origem varrida (a direção
absoluta: `k` + a do alvo, `+0x20` = `0xac(r1)` no `0x479788`); **todos** os
objetivos em pé do slot ganham +25 se essa direção não está na máscara (a
mistura de sentidos, absoluto contra relativo, é do binário). Os objetivos
de osso (`GetPossibleSlotsFromModelRoutingSlot`) recebem a máscara absoluta
(direção 0) por outro caminho (`0x479260`). Sem a RS flag `0x80000`.

### O teste previsto (`cLocomotable::CollisionCheck`, BV `0x6b49e8`, lido de novo)

O ID ignorado começa em **0**: não há exceção para o alvo da rota. Para cada
janela, `EstimateWhenPathWillBeBlocked` (`0x478`) dá o bloqueador e o
instante; o bloqueador só é **ignorado** (passa a ser o "ignorar" das
janelas seguintes) quando a colisão é **no instante atual** (o começo da
varredura, os dois já se tocam) **e** a distância (`0x47c`) no fim da janela
é maior que no começo (estão se afastando); qualquer outra colisão no
horizonte é bloqueio. Ou seja, as duas exceções do VM ("já sobrepostos <
0,3" e "o alvo da rota") não existem: quem já toca o outro só segue se se
afasta. Achado um bloqueador: `SetBlockingObjectID`,
`InterruptActiveLocoState(2)` e, se o pedido expirou ou a colisão é antes
de `0xa0(0)` + 2 passos, `EndFollow`.

### A contagem do bloqueio (`DoBlockedGoalStillAvailable`, BV `0x57a8d4`)

O contador `+0x3c` sobe a cada entrada e volta a 1 só quando o bloqueador
difere do **último** guardado (`+0x3e`, que fica no estado da rota e não é
apagado quando o caminho fica livre) ou quando ele se afastou ≥ 0,25 do lugar
anotado (mapa da rota `0x1d8`/`0x1d4`, ID → lugar; **rodada n30, §9.3**: a
chave é o ID de quem anda, não o do bloqueador: é quem anda que se afastou). O VM apagava o
bloqueador quando o caminho ficava livre, e a contagem recomeçava: dois
Sims a 0,285 um do outro, cada um "entrando" no outro pelo teste por passo
(0,283) mas fora da previsão (a exceção dos 0,3), pausavam e replanejavam
para sempre com o contador em 1 (`N001_Lot11`, `OS2_SEED=12`: 1.303 "step
into" em 12.000 ticks).

### Os retângulos de custo 10 das escadas (`PortalAddToPathPlannerImpl`)

Nos tipos 10 e 12, com `TSObjectGetModelType` ≠ 1 (BV `0xe1ec30`: roda a
árvore da entrada **0x30** do OBJf e lê o temp 0; 1 = nenhum), as duas
pontas (o objeto e `0x590`, a outra ponta; a de andar maior primeiro) põem
`AddObjectRectangle` de custo **10** junto da caixa da pegada (`0x100`): tipo
2 ou 4, um retângulo de `min − 0,32499` a `min` num eixo e de `min −
0,32499` a `min + 0,12499` no outro; tipo 3 ou 4, o mesmo do lado de `max`
(`max − 0,12499` a `max + 0,32499`). A ordem dos eixos nos argumentos não
foi conferida contra os dados (não feito).

### Sobras desta leitura

- A caixa do modelo sem lista "footprint" (`FUN_00a45e54`): a união das
  caixas dos nós com a flag 4 (`0xa4(4)`), não escondidos (`0x114`) e com
  limites (`0x180`), descendo nos filhos sem a flag pedida; o que é a flag 4
  do nó não foi lido (segue inferido: as malhas sem os planos de sombra).
- O `+10` do `WorldOutsideSupport` e o estado 7 (`FUN_00165c88`) no piso.
- O ponto de colisão do `CollisionCheck` vem da trajetória que o motor prevê
  (vtable `0x478`); aqui a dos pontos da rota em linha reta (`alongRoute`).

### O que mudou no código (rodada n18) e as medidas

Um commit por parte, `ctest -j2` verde em cada um (53 testes):

1. **O contador do bloqueio** (`routing_walk.cpp`, `Route::lastBlocker`,
   estado salvo versão 4): a rota guarda o último bloqueador e o contador
   sobe com ele mesmo depois de o caminho ficar livre.
2. **O `CollisionCheck`** (`routing_loco.cpp` `simAhead`,
   `routing_walk.cpp`): a primeira colisão no horizonte (cortado no fim da
   rota) bloqueia; só se larga quem já toca e se afasta; saíram as duas
   exceções nossas (o alvo da rota, "já sobrepostos < 0,3"); a parada com
   clipe só quando acaba 2 passos antes da colisão, senão para na hora (no
   jogo base quase sempre: a parada não cabe em meio tile). Saiu o espaço
   inferido de 2 passos da rodada n14.
3. **Snap num ponto só** (`reach.cpp`): o primeiro osso ou o deslocamento do
   slot, "na frente" o tile adiante; os testes do binário na ordem (parede a
   0,3 só do tile do ponto, lote e sala sem `0x800`, parede no meio sem
   `0x80000`, pegadas e Sims a 0,283, sem exceção do alvo); objeto no
   caminho dá od 52 = `0x0E`. `OS2_SNAP_SCAN=1` volta à varredura antiga.
   **O anel** de 0,5/0,75 em volta dos ossos saiu também das rotas
   (`OS2_BONE_RING=1` o põe de volta): o café servido pelo lado do balcão
   (0,75 do osso `route_counter_std_0_N`, a mão a 0,13 da cafeteira) era um
   ponto dele.
4. **O custo do piso** (`world.cpp`, `formats/build_catalog` `floorRoutingCosts`,
   `path_planner.cpp`): por quarto de tile, o `routingcost` do padrão (preso
   em [0,5; 10]), 2 sem piso ou com padrão que o catálogo não conhece, o
   desnível como antes; as células do planejador pelo quarto do seu meio; o
   corte de quinas do binário (`CornerCuttingFilter`: tira um ponto só se a
   reta até o seguinte custa menos que o caminho + 0,5); o placar dos
   objetivos em pé −10 por unidade acima de 1 pelo quarto do ponto.
5. **O +25** (`routing.cpp` `scanStanding`): a máscara de paredes do tile do
   alvo girada e espalhada, contra a direção da última origem varrida.

Não feitos: os retângulos de custo 10 das escadas (a ordem dos eixos e a
árvore 0x30 do tipo de modelo), o `+10` do `WorldOutsideSupport`, o estado 7,
a sala do Snap como o gerente de partições a dá (aqui a sala do tile).

**A "regressão" da rodada n14** (`N001_Lot68` 19 → 28 falhas de rota, 2 → 7
paradas, 12.000 ticks, uma semente): com 14 sementes (0–13, `OS2_SEED`,
`s2pkg autonomy`) não há regressão — antes da n14 262 falhas no Lot68 e 262
no Lot11, com a n14 277 e 201 (a semente padrão vai de 13 para 21, as outras
sobem e descem). O que a n14 mudou e importa: o teste por passo bloqueia na
hora, e com o contador zerado a cada caminho livre e a exceção dos 0,3 (a
previsão não via dois Sims a 0,285) dois Sims se "entravam" para sempre
(`N001_Lot11`, semente 12: 1.303 bloqueios por passo). Com as partes 1 e 2
o laço acaba (as rotas falham com 14); a causa de fundo é o topo da escada
reta do Lot11 (§10). Medidas finais (14 sementes × 12.000 ticks, main da
rodada 17 → esta rodada): Lot68 258 → 337 falhas (mais 13/11 do "Route -
Dynamic w/Lead in", os Sims seguindo alvos que andam, com os desvios pelos
pisos), Lot11 177 → 650 (456 numa só semente, o topo da escada; as outras
13: 163 → 194); paradas com clipe 80 → 16, bloqueios por passo 1.321 → 255.

## 9.2 Rodada n18b (rotas, laços): o que foi lido e o que mudou

Leitura de 2026-10-15, **Ghidra MCP** (só leitura). Rodada de correção
depois da n18: as falhas de rota subiram (14 sementes × 12.000 ticks,
`OS2_SEED` 0–13, `s2pkg autonomy` com `OS2_ROUTE_FAILS=1`): `N001_Lot68`
258 → 337 e `N001_Lot11` 177 → 650 (456 numa semente só). Medido de novo
no começo desta rodada: 356 e 647.

### Onde o Sim vai estar (`GlobalLocationEstimate`, BV `0x6af398`)

O `EstimateWhenPathWillBeBlocked` (BV `0x6b3574`) pede ao locomotor onde
cada um estará em cada instante: a vtable `0x3e8` do `cLocomotable` é
`GlobalLocationEstimate`: sem caminho (`+0x18c`, o caminho da locomoção) ou
com comprimento 0, o lugar atual; senão o ponto do caminho (vtable `0x54`
do caminho) na distância `DistanceCompletedEstimate` (vtable `0x470`) desse
instante. A direção do "ponto 0,35 à frente" é a `GlobalDirectionEstimate`
(vtable `0x468` → `0x488`, BV `0x6aea88`): a tangente do caminho nessa
distância, a direção do Sim (vtable `0x1e4`) sem caminho, e durante uma
virada a direção interpolada pelo tempo. O outro Sim igual (vtables
`0x2b0`/`0x2b8` do seu `cILocomotable`). Ou seja, **a previsão segue o
caminho cúbico que o quadro percorre** (o `LocoCurve`), não as retas entre
os pontos da rota: o VM usava as retas, e o quadro corta por dentro nas
curvas. No `N001_Lot11` (semente 2) um Sim que virava ao passar por outro
parado a 0,33 da sua reta chegava a 0,28 dele sem a previsão ver; o teste
por passo (0,283) o barrava, e os dois ficaram se barrando o resto do jogo
(1.303 "step into" na n14, 456 falhas de rota na n18).

**O teste por passo** (`LocoPlaceFromSceneGraph`, BV `0x6b2ee0`): se o
lugar novo falha no `TSObjectCanSetPosition` mas o lugar **atual** também
falha (por qualquer motivo), o passo vai assim mesmo; só bloqueia (`0xc` =
pessoa → `SetBlockingObjectID`) quando o lugar atual vale. O VM faz isso
pessoa a pessoa (quem já está a < 0,283 não barra); a versão "qualquer
motivo" não foi feita.

### Quando roda o goal check (`DoAlongRouteCheckTest`, BV `0x470fe4`)

`DoWalking` (BV `0x576acc`), **antes** do passo (`FollowOneStep`):

```
se DoAlongRouteCheckTest(andar do Sim, comprimento do caminho × porção feita, forçar = 0)
   e o byte +0x20 do gerente de rotas (GRS_State +0x20 = cTSGlobals::GetRouteManager; 1 do construtor):
      AlongRouteUpdate; goal check (estado 4) ou along route (0x1E)
depois o passo
```

`DoAlongRouteCheckTest(nível, andado, forçar)` devolve 1 (e anota os quatro)
quando `forçar`, ou o caminho mudou (`+0xd8` da rota ≠ `+0x50`: o caminho
da rota, refeito a cada planejamento), ou a sala do Sim mudou
(`GetRoomID`, vtable `0x204` do `cTSPerson`, ≠ `+0x54`), ou o andar mudou
(`+0x4c`, que o `Construct` da rota põe em −999: **na primeira vez de toda
rota**), ou o andado difere do anotado (`+0x48`) em mais de 1,0
(`GetPathLength` × `GetPathPortionCompleted`, vtables `0x2c4` × `0x2c8` do
`cILocomotable`, que fica em `+0x338` do `cLocomotable`). O VM rodava as
árvores ao cruzar um tile, depois do passo.

**Bloqueado** (`DoBlocked`, BV `0x5762a4`): com bloqueador e caminho de
mais de um nó, chama `DoAlongRouteCheckTest` **forçado** (último argumento
1) e o goal check (`DoBlockedWaitingForGoalCheckTree`, estado 0x19): falso
→ 13; verdadeiro → `DoBlockedGoalStillAvailable` (0x1A). `DoFoundPath`
(BV `0x575154`) → `DoBeginFollow` (BV `0x5745e0`) não testa colisão: a
primeira vez de `DoWalking` sempre chega ao teste.

**O 13 dos sociais é de propósito.** "Route - Dynamic w/Lead in" (social,
semi-global `0x7FD90EDB` `0x204C`) só faz o Go To Routing Slot e devolve o
resultado; o goal check do social ("Function - Goal Check", `0x2041`)
chama "Goal Check - Continue?" (`0x2079`: falso a ≤ 3 tiles do outro) e
"Goal Check - Update Dynamic Route?" (`0x2055`), que, quando o outro
andou, roda "Route - Refresh route" (`0x2066`) → "Route - Dynamic w/Lead
in" **de dentro do goal check** (uma rota dentro da outra). Quem chamou
("Route to Person X", `0x202A`) lê o dado de pessoa 62 = 13 como "perto o
bastante" e faz o lead-in. Com o goal check na primeira vez da rota, cada
social que começa a ≤ 3 tiles acaba logo com 13: esses 13 não são falhas.

### A pilha de rotas (`ProcessGoto`, BV `0x57b500`)

O roteador tem uma **pilha de rotas** (`cTSRoutingObjectAggregate`
`+0x14..+0x24`, a corrente pela vtable `0x68`; `DoNeedFailureFeedback`, BV
`0x5774cc`, só toca a animação de falha com a pilha de tamanho 1). O VM
tinha uma rota por objeto: a rota de dentro do goal check substituía a de
fora, e de volta do goal check a instrução de fora não achava rota e
tomava o falso da árvore pelo da pré-rota: **11 "pre-route failed"**, que o
"Route to Person X" não lê como 13 (o social falhava). Feito: a rota de
fora fica guardada (`Object::outerRoutes`) e volta quando a de dentro
acaba; o falso do goal check dá 13.

Mais lido: a rota de sair do caminho do `TryGetSelfOutOfWay` (BV
`0x57e9ac`) é construída com o `+0x115` = 1 (vtable `0xcc`): sem animação
de falha e sem gravar o dado 62; a contagem de saídas fica em `+0x11c`
(vtables `0xd8`/`0xdc`, não `+0xd8`). "CT - Route Blocked" (semi-global
da pessoa `0x7F01EC29` `0x20BE`) só devolve verdadeiro.

### Os retângulos de custo 10 das escadas (lidos de novo, não feitos)

`AddObjectRectangle` (vtable `0x5c` do gerente, BV `0x4f34b4`) recebe
(a0, b0, a1, b1, andar, custo, estado…); com estado > 1 põe outro
retângulo 0,12499 maior com custo `FLT_MAX` (a inflação). Pelo código das
portas no mesmo `PortalAddToPathPlannerImpl`, a ordem do gerente é a
**transposta** da do `cFootprint` (o retângulo `(r0, r1, r2, r3)` vai como
`(r1, r0, r3, r2)`), o que, com o `cLocation` de x primeiro, dá (y0, x0, y1,
x1). A caixa das escadas é a do `GetFootprint(lugar, direção)` (vtable
`0x100`, BV `0xaca7d4` na pessoa): no mundo, girada. Então os retângulos
(ponta de cima: x de `x0 − 0,325` a `x0`; de baixo: `x1` a `x1 + 0,325`;
nos cantos de y conforme o tipo 2/3/4 da árvore 0x30) ficam sempre do
mesmo lado em x, seja qual for a direção da escada; como a árvore 0x30
pode depender da direção, não foi feito (inferido demais; não muda o
impasse abaixo).

### O que mudou no código (rodada n18b) e as medidas

Um commit por parte, `ctest -j2` verde em cada um (55 testes no fim):

1. **A previsão pelo caminho do quadro** (`routing_loco.cpp`, `WayAhead`):
   `simAhead` amostra o `LocoCurve` do Sim (o que o `locoStep` mantém ou
   faria) e o do outro. Teste: `test_routing` (um Sim dentro de uma curva,
   a ≥ 0,29 das retas e < 0,283 da curva, é visto) e `stair_top.sh`
   (`N001_Lot11`, semente 2, 3.000 ticks: no máximo 2 falhas no patamar e 2
   bloqueios por passo; a build da n18 dá 33 e 36).
2. **O goal check pelo `DoAlongRouteCheckTest`** (`routing_walk.cpp`
   `advance`, `Route::plans`/`check*`, estado salvo versão 5), antes do
   passo; não durante o sair do caminho (inferido: no binário é outra rota).
3. **A pilha de rotas** (`Object::outerRoutes`, `keepOuterRoute`/`endRoute`);
   se o Sim andou, a rota de fora planeja de novo de onde ele está quando a
   árvore a deixa seguir (inferido). Teste: `test_routing` (um goal check que
   roteia: a de fora falha com 13, não 11).
4. **O goal check no bloqueio** (`DoBlocked` forçado).

Medidas (14 sementes × 12.000 ticks; "resto" = sem os 13 de propósito do
"Route - Dynamic w/Lead in"/"Route - Refresh route"):

| | Lot68 total | Lot68 resto | Lot11 total | Lot11 resto | interações autônomas que acabam falsas (68/11) |
|---|---|---|---|---|---|
| main (n18) | 356 | 205 | 647 | 588 | 28 / 33 |
| 1. curva | 349 | 218 | 483 | 409 | 33 / 27 |
| 2. goal check | 734 | 366 | 806 | 492 | 14 / 29 |
| 3. pilha de rotas | 637 | 28 | 666 | 171 | 19 / 4 |
| 4. goal check no bloqueio | 636 | 35 | 650 | 161 | 18 / 4 |

O total sobe porque cada social que começa perto (ou cujo alvo andou)
acaba com o 13 de propósito, duas vezes quando o goal check refaz a rota
(a de dentro e a de fora): o que o binário faz. O resto (as falhas de
verdade: 14, 15, 9, 10, 3) caiu de 205/588 para 35/161, e as interações
"feitas" subiram (750 → 770 no Lot68, 489 → 505 no Lot11). Dos 161 do
Lot11, 117 são da semente 4: o impasse do alpendre (§10).

## 9.3 Rodada n30 (o impasse do alpendre): o que foi lido

Leitura de 2026-10-15, **Ghidra MCP** (só leitura, nada renomeado nem
gravado; as funções que o projeto não tem como função lidas com
`disassemble_bytes` em `dry_run`). Endereços BV. Os nomes dos métodos do
slot sintético vêm da tabela de símbolos do binário BV que está em
`references/sims2_mac_decomp/baserom.sym` (outro arranjo de endereços,
mas os mesmos nomes); a posição de cada um na vtable foi conferida pelos
getters que a varredura chama.

O cenário (`N001_Lot11`, sementes 2 e 4 até o tick 3000): o alpendre é o
tile (8, 12, 2), parede a oeste (x = 8) e ao norte (y = 13), a porta a leste
(x = 9) e a escada do deck ao sul; o planejador deixa livre só
x ∈ [8,25; 9], y ∈ [12; 12,75] (paredes infladas 0,125). A Cassandra sobe a
escada rumo à porta e o Alexander sai pela porta rumo à escada: cada um
para sobre a ponta de portal de que o outro precisa (a do topo da escada e
a da porta, a 0,35 uma da outra).

### O lugar para sair do caminho (`CreateMoveOutOfWayDestination`, BV `0x57dcac`, lido de novo)

A função **não escolhe ponto nenhum**: cria o objeto "Destination - For Go
Here & Shoo" no lugar (`cLocation`, vtable `0xa8`) de quem vai sair do
caminho (`this`) e põe nele, como slot 0, um slot de rota sintético
(`cTSRoutingSlot`, CLSID `0xFBA4DA4F`). O ponto sai depois da varredura
comum desse slot (§8.2, BV `0x4778b4`) e do A*, na rota da interação "Move
Out Of Way" (pedido de outro) ou na rota empurrada pelo `TryGetSelfOutOfWay`.
**Devolve 1 sempre que os dois estão a ≤ 4 tiles** (no tipo 0; nos tipos
1 e 2 sempre): não confere se a varredura terá algum ponto válido. Sem
ponto, é a rota até o destino que falha.

Os métodos do slot (vtable; os getters já vistos na varredura, os setters
pelo par getter/setter da tabela de símbolos, inferido onde só a vizinhança
diz): `0x20` AllowDirection(d), `0x18` todas as direções, `0x28`/`0x30`/`0x40`
os modos de facing (`0x40` = SetFacingDirection(d)), `0x44`/`0x48`
Get/SetIgnoreRooms (a RS flag `0x800`), `0x4c`/`0x50` Get/SetIgnoreWalls (a RS
flag `0x80000` que tira o +25), `0x5c`–`0x68` o "slot displacement",
`0x6c`/`0x70` Get/SetAllowOtherDirectionsAtLowerScore, `0x74`/`0x78`
Set/HasRandomScoring, `0x7c`/`0x80` Set/HasStationaryBonus, **`0xb8`
GetMinDist, `0xbc` GetMaxDist, `0xc0` GetOptimalDist, `0xc4` GetSpacing,
`0xc8` GetGradient, `0xcc` SetGradient, `0xd0` SetDistances(mín., máx.,
ótima), `0xd4` SetSpacing**, `0xdc` SetMultiplier(posição, valor). A
varredura (`BuildGoalList`, BV `0x478d2c`, lida na listagem: `0x479500`–
`0x4797d8`) monta o contexto assim: mín. `+0x28` = deslocamento + `0xb8`,
máx. `+0x2c` = deslocamento + `0xbc` (no máximo 32), ótima `+0x30` =
deslocamento + `0xc0`, gradiente `+0x34` = `0xc8`, passo `+0x38` = o maior de
`0xc4` e 1/16, `+0x40` = π/4 (o cone de 45°) e a **largura `+0x44` = 0** (as
metas em pé nunca a usam); recusa sem metas se não for 0 ≤ mín. ≤ ótima ≤ máx.
Isso **corrige a leitura de §8.3** ("`a` = resolução, `b`/`c` = máx./mín.,
largura 100"): com ela o tipo 0 teria mín. 0,3 > ótima e nenhuma meta.

- **Tipo 0** (o pedido de quem anda e o `TryGetSelfOutOfWay`): a direção
  `d+7` ou `d+1` de antes (d = a de quem pede; absoluta, inferido: é
  calculada da direção de quem pede, e o tipo 0 não chama nenhum setter de
  "absoluto"), `SetDistances(0,0625, 0,3, 0,3)`, `SetGradient(100)`,
  `SetSpacing(0,025)` (a varredura usa 1/16), facing para quem pede
  (`0x40`, a direção de quem sai até quem pede), sem sorteio (`0x74(0)`),
  sem o bônus de ficar parado (`0x7c(0)`), salas e paredes contam (`0x48(0)`,
  `0x50(0)`), multiplicador em pé 1 (`0xdc(0, 1)`). A varredura dá **duas
  fileiras** (0,3 e 0,2375 à frente, a mais longe primeiro) de **três pontos**
  cada (lateral −1/16, 0 e +1/16: o ±2/16 cai fora do cone, abaixo), cada um
  pelo `EvalTileForGoal`; placar 100 − 16·100·|d − 0,3| − 0,001·100·|lateral|
  − penalidades (o do meio a 0,3: 100; os laterais a 0,3: ~89,7; a fileira
  de 0,2375: ~0–13), +25 sem parede na direção (§9.1). Ou seja: **um passo de
  0,3 tile na diagonal**, à frente de quem pede, do lado em que quem sai está;
  com parede a menos de 0,3 do ponto (`EvalTileForGoal` 4), outra sala (2) ou
  um Sim perto (6), nada.
- **Tipo 1** (o bloqueador está a < 1,5 tile do objeto da minha rota):
  direções 1, 2, 6 e 7, `SetDistances(1, 3, 1)`, `SetSpacing(0,25)`,
  `SetGradient(0,625)`, sorteio ligado, sem o bônus de ficar parado,
  `0x70(1)` (outras direções com placar menor), salas e paredes contam.
- **Tipo 2**: o 1 com `SetDistances(1, 6, 1)` e salas e paredes ignoradas
  (`0x48(1)`, `0x50(1)`), `0x94(1)`. As direções dos tipos 1/2 são relativas à
  direção do destino, que é a de quem sai (o `cLocation` dele; inferido).

### O cone da varredura (`FUN_01655af0`)

Antes do `EvalTileForGoal` cada ponto em pé passa por um filtro: no
referencial da origem e da direção (`+0x18`, `+0x20` do contexto), à frente
f ≥ 0 e f ∈ [mín. − largura − 0,001; máx. + largura + 0,001], e 2·atan2(máx(0,
|lateral| − largura), f) ≤ o ângulo (`+0x40`). As fileiras vão de
`floor(−f·tan(22,5°)/passo)` a `ceil(f·tan(22,5°)/passo)`, então os dois
pontos das pontas, que passam de f·tan(22,5°), **não entram** (o VM os
punha).

### A contagem das tentativas (`DoBlockedGoalStillAvailable`, BV `0x57a8d4`, lido de novo)

A rota guarda um mapa ID de roteador → lugar (`cTSRoute +0x134`,
`Get/SetRouterLastKnownLocation`, vtables `0x1d8`/`0x1d4`). O `InitRoute` (BV
`0x5737cc`) anota, **a cada planejamento**, o lugar de todos os roteadores do
gerente de rotas, o próprio Sim incluído. No `DoBlockedGoalStillAvailable`:

- o contador `+0x3c` sobe a cada entrada e volta a 1 quando o bloqueador é
  outro que o último (`+0x3e`) **ou quando eu (quem anda) me afastei ≥ 0,25
  do lugar anotado para mim**; depois o meu lugar é anotado de novo
  (`0x1d4(eu, meu lugar)`). Corrige §6 e §9.1, que diziam "o bloqueador se
  afastou 0,25";
- "ele saiu do lugar" (a re-rota dos ramos: parado com contador < 2; o que
  me bloqueia e roteia com contador == 2; o que eu bloqueio até o 4) é o
  lugar atual do bloqueador **diferente** do anotado para ele no meu último
  planejamento (os 8 bytes do `FTilePt`, `FUN_01655330`: qualquer
  movimento); sem anotação, não. Corrige o VM, que comparava o tile do
  bloqueador com o de quando o contador recomeçou;
- os ramos são os de §6 (rodadas 13, 19, 22), conferidos de novo.

### "CT - Route Blocked" (`TriggerRouteBlockedTree` BV `0x57946c`, `DoWaitingForRouteBlockedTree` BV `0x56d7dc`)

Só na rota de base e **uma vez por rota** (`0xe4`/`0xe0` da rota,
`RouteBlockedTreeHasRun`): stack object = o bloqueador, parâmetros 0 = o
índice do slot (`WorkOutSlotIndex`), 1 = o ID do alvo da rota, 2 = o `+0x14`
do objetivo atual, 3 = 0; a árvore é procurada pelo nome nas do Sim (o
semi-global da pessoa: "CT - Route Blocked" `0x20BE` só devolve
verdadeiro). **Verdadeiro → `DoInitial`** (planeja de novo); falso → 14.
Da segunda vez que o ramo chega lá, 14 direto. O VM falhava com 14 na
volta da árvore e passava o bloqueador como parâmetro 0.

### A pausa (`DoPause` BV `0x579e48`, `DoContinueAfterPause` BV `0x570f30`)

`DoPause` copia o estado da locomoção para a rota, vai ao 0x1D e roda "CT -
Route Blocked Animation" (`TriggerBlockedAnimation`); **a pausa dura o que a
árvore dura**. `DoContinueAfterPause`: `ShouldInterrupt` → 3, senão
`DoInitial`. Como o VM.

### Esperando quem foi pedido (`DoWaitingForPersonToMove` BV `0x579ffc`, `MoveOutOfWay` BV `0x57ee3c`, `DoPostFailedOrSucceeded` BV `0x56f610`)

- `MoveOutOfWay` aceita (empurra a interação "sair do caminho" com a
  prioridade de quem pede) **sem saber se há lugar**; recusa se já foi
  pedido, se a prioridade não deixa, se a minha rota tem ≤ 1 objetivo, a
  mais de 4 tiles, ou se a rota corrente dele já pediu a alguém para sair
  (`0x188`). O VM exigia um lugar livre e recusava se a rota dele tinha
  qualquer bloqueador.
- Aceito, a minha rota guarda quem foi pedido e o ID da interação (`0x190`).
  Quando a rota dele **dentro dessa interação** acaba (chegou ou falhou), o
  `DoPostFailedOrSucceeded` dela avisa toda rota que o pediu
  (`0x198(sucesso)`, `0x190(0, 0)`), e o `DoWaitingForPersonToMove`, sem o
  pedido, vai **na hora** ao `DoInitial`. O VM esperava até 126 ticks ou até
  1,25 tile. A rota do `TryGetSelfOutOfWay` (flag `+0x115`, `0xcc`) não grava o
  dado 62 nem avisa ninguém; ao acabar sai da pilha e a de baixo continua do
  estado em que ficou (o `DoInitial` que o `DoBlockedGoalStillAvailable` lhe
  deu): planeja de novo.

### O teste por passo (`LocoPlaceFromSceneGraph`, BV `0x6b2ee0`, lido de novo)

`TSObjectCanSetPosition` no lugar novo falha → se o Sim está no mundo, o
mesmo teste no **lugar atual**; se o atual também falha (qualquer motivo:
outro Sim a < 0,283, uma pegada, parede), o passo vai assim mesmo. Senão a
lista de erros é percorrida e o primeiro erro `0xC` ("intersects object")
dá o bloqueador; sem `0xC` (parede...) o passo para sem bloqueador
(`DoBlocked`: pausa, depois 15). O VM fazia "já dentro deste Sim" pessoa a
pessoa e "já numa pegada" só para as pegadas.

### Como o jogo acaba com o impasse

Pela leitura, o roteador não tem nada além disto: pedir para sair (aceito
mesmo sem lugar; quem pede replaneja assim que a rota do outro acaba),
sair do caminho uma vez por rota, a pausa com replanejamento, o "CT - Route
Blocked" (um replanejamento a mais) e, esgotadas as tentativas (contador
> 4, ou > 3 com o outro parado), o 14 "blocked by person": a interação
acaba e a autonomia escolhe outra.


### O que mudou no código (rodada n30) e as medidas

Um commit por parte, `ctest -j2` verde em cada um (57 testes):

1. **As metas em pé** (`routing.cpp`): o filtro do cone antes do
   `EvalTileForGoal` (as pontas da fileira, que passam de f·tan 22,5°, saem)
   e a pessoa no ponto (código 6) a 0,283 do centro (o teste de
   `TSObjectCanSetPositionAndOrientation` entre pessoas; era meio tile,
   inferido), com as exceções do teste por passo (`peopleMayOverlap`).
   `test_routing`: os dois Sims que se cruzam começam um tile antes do
   objetivo do outro.
2. **O sair do caminho** (`routing_loco.cpp` `moveOutOfWaySlot`,
   `askBlockerToMove`; `routing_walk.cpp` `selfOutOfWay`): o slot sintético
   dos tipos 0/1/2 e as metas pela varredura (`routingGoals`), em vez da
   fileira de até 6 tiles com o primeiro ponto livre longe do caminho de
   quem pede (nossa); o pedido aceito sem lugar, a rota guarda quem foi
   pedido e a interação (`Route::asked`/`askedUid`), e quem pediu replaneja
   assim que a rota do outro nessa interação acaba (`moveOutOfWayRouteEnded`);
   com um pedido pendente, nem outro pedido nem sair do caminho; o
   `TryGetSelfOutOfWay` replaneja mesmo sem lugar. Estado salvo versão 6.
   `test_routing`: o slot e as suas metas; o bloqueador do corredor dá o
   passo de 0,3 e quem anda passa.
3. **O contador** (`Route::places`, `notePlace`): cada planejamento anota o
   lugar de todos os roteadores; o contador volta a 1 com o bloqueador
   novo ou quando **quem anda** saiu 0,25 do lugar anotado; "ele saiu do
   lugar" é o bloqueador fora do lugar anotado no último planejamento.
   Estado salvo versão 7.
4. **O teste por passo** (`stepBlocker`): o lugar atual inválido por
   qualquer motivo deixa o passo ir.
5. **A pilha de rotas** (`routeStackDepth`): a rota de dentro de uma árvore
   de callback (inclusive a da falha, cuja rota fica na pilha até o
   `DoPostFailedOrSucceeded`) não é a de base; pilha > 2 → 14; a árvore de
   falha só para a rota sozinha.
6. **"CT - Route Blocked"** (`routeBlocked`): uma vez por rota, o
   bloqueador como stack object e os parâmetros do binário; verdadeiro
   replaneja. Estado salvo versão 8. Ajudas de teste: `OS2_ROUTE_FAILS_TREE`,
   `OS2_DEBUG_GOALCHECK`, o "step into" diz se o bloqueador é pessoa.
7. **`stair_top` estrito de novo** e **`glides` em 0** (o deslize do
   turnAndGo de 180° era o relógio da cena andando debaixo de um diálogo que
   pausava o jogo: `LotSimulation::speedScale`, `docs/anim-mac.md`).

Medidas (`s2pkg autonomy`, `OS2_ROUTE_FAILS=1`; main = `21384f9e`):

| | main | rodada n30 |
|---|---|---|
| Alpendre, 28 sementes × 3.000 ticks: sementes com o alpendre preso (> 10 falhas fora as de propósito) | 6 (2, 4, 10, 12, 15, 18) | 3 (6, 10, 21) |
| `stair_top` (sementes 2/4: falhas no alpendre fora as de propósito) | 8 / 13 | 1 / 0 |
| 14 sementes × 12.000 ticks, falhas das rotas dos que andam (sem o 13 dos sociais, o "Move Out Of Way" de quem foi pedido, "Okay to Stand Here?", "Command - Reposition" e a caminhada da árvore de falha), Lot68 / Lot11 | 28 / 316 | 40 / 341 |
| as mesmas, todas menos o 13 dos sociais ("resto"), Lot68 / Lot11 | 39 / 954 | 51 / 3.058 |
| "Interaction - Move Out Of Way" que falha (Lot11) | 103 | 1.956 |

O "resto" do Lot11 sobe porque o pedido para sair agora é aceito sem
lugar, como no binário: cada pedido a um Sim parado no alpendre dá as três
tentativas do "Interaction - Move Out Of Way" (o slot 0 e os globais 8 e
6), todas sem ponto; os pedidos se repetem nos alpendres presos que
sobram (sementes 3, 6 e 10 em 12.000 ticks). As falhas das rotas de quem
anda ficam no mesmo nível (as sementes presas mudam: 2, 4, 10, 12 → 3, 6,
10).

## 9.4 Rodada n31b (a escada do deque e o passo bloqueado): o que foi lido

Leitura de 2026-10-16, **Ghidra MCP** (só leitura; as funções que o
projeto não tem como função lidas com `disassemble_bytes` em `dry_run`).
Endereços BV; os nomes de métodos sem símbolo vêm da `baserom.sym` (a
posição na vtable conferida pela vizinhança, como na §9.3). As duas causas
de rota que a rodada n31 achou (`docs/primitives-mac.md` §4g "Rodada n31").

### A ponta de baixo das faixas de uma escada que acaba no meio do tile

O cenário: no `N003_Lot192` o deque do andar 2 (x 21–27, y 24–28) só desce
pelas escadas modulares de 6 degraus (x 26/27 e 12/13): o "Portal - A" em
y = 26,25 (andar 2), as "Bridge" em 26,50…27,25 e a "End" em 27,50 (andar
1). As faixas (§8.6) acabam embaixo em y = 27,71 (`OS2_OBJECT_PORTALS=1`:
568 slot 0 (27,67; 25,81; 2) → 600 slot 1 (27,67; 27,71; 1)).

- **`PortalAddToPathPlannerImpl` (BV `0xe22a44`), lido inteiro**: os pontos
  das duas pontas (`PortalGetLocationOfSlot`, BV `0xe21b6c`: o lugar do
  objeto mais o vetor do slot guardado pelo `PortalGatherSlotInformation`,
  vtable `0x12c` do objeto, girado pelo quatérnio da direção; andar do
  objeto − 1) vão ao `AddPortal` do gerente (vtable `0x84`) **como estão**:
  nenhum ajuste e nenhum teste de passagem. Nos tipos 10–12 a mão única do
  slot 0 (§8.6). As portas (tipo 8) vão pelo `AddDoorPortal` (vtable `0x68`)
  com um retângulo de ±0,2 em volta do meio (dois mais, deslocados 0,25,
  numa porta diagonal) e os retângulos da folha (`TSObjectIndexedWorld
  SpaceFootprint` índices 0 e 2, vtable `0x70` do gerente).
- **`cPathPlanner::AddPortal`** (BV `0x6064b4`) → **`MakeNodesForPortal`**
  (BV `0x604584`): um `cAStarPortalNode` em cada ponta; uma ponta com dado
  de nó (`cPathNodeData`, `+0x40`/`+0x44` do `cPortal`) se junta ao nó já
  registrado com o mesmo dado (mapa `+0x6c` do planejador): portais que
  dividem um slot dividem o nó.
- **`cQuadTree::ConnectPortalNode`** (BV `0xba45b4`, chamado por
  `InternalUpdateDirtyPortalConnections`): liga o nó da ponta, nos dois
  sentidos, à célula de custo uniforme que contém o ponto
  (`GetQTUniformCostCell`, BV `0xba43e8`: desce a quadtree até uma folha de
  custo uniforme) e às dos quatro pontos a ±0,0625 em x e y (as diagonais).
  Ir a uma célula bloqueada custa `FLT_MAX` (§4): a ponta tem caminho se uma
  dessas cinco células passa.
- **O que bloqueia ali**: só pegadas, paredes e piso. As peças da escada
  entram pela pegada (§2, rodada 15: tipos 10 e 11; o "Portal - A", tipo 12,
  não). **O binário não tem regra de "tile debaixo dos degraus"**: o
  roteador não tem função de escada além dos portais
  (`IsNextPathNodeAStairStart`/`End`, `AreAtStairStart`), o piso do térreo é
  o terreno em todo tile e os de cima são os retângulos de piso. No
  `N003_Lot192` as pegadas (do modelo, lista "footprint") são: "End" 600
  [27,00; 27,25 – 28,00; 27,50], "Bridge" 598 [27,00–27,25], 593
  [26,75–27,00] etc.; infladas 0,12499 acabam em y = 27,625. As pontas
  (27,33 e 27,67; 27,71) e os pontos a ±0,0625 (y 27,65) ficam **livres**: no
  jogo o deque desce por elas. O "Deck - Hi Stub" (27,5; 28,0; 1) deixa
  pessoas passar (od 8 `0x04`): fora do planejador.
- **No VM**: `World::addStair` marca sem piso os tiles debaixo dos degraus,
  no andar de baixo (de antes da rodada 15, quando as escadas não entravam
  no planejador; inferido), e o planejador fecha esses tiles inteiros mais
  0,125 (`kNoFloor`). Numa escada que acaba no meio do tile (os 6 degraus
  do Lot192 vão de y 26,0 a 27,5) isso fecha também a metade livre (27,5 –
  28,0, mais 0,125): a ponta em 27,71 fica dentro e o portal sai do plano
  (`leafAt` −1). Daí o deque sem caminho para baixo. As escadas de 3 degraus
  e as retas cobrem tiles inteiros (as pegadas dos "Lo" das retas são tiras
  de 0,1 a cada 0,125, que a inflação junta), e lá dá o mesmo.
- **Os retângulos de custo 10** (§9.1, §9.2) não bloqueiam: o
  `AddObjectRectangle` vai com passagem 0 (sem a cópia inflada de
  `FLT_MAX`). O "tipo de modelo" (`TSObjectGetModelType`, BV `0xe1ec30`: a
  entrada 48 do OBJf, temp 0) é o **modelo do corrimão** (1 nenhum, 2/3 de
  um lado, 4 dos dois; `docs/buy-build-mac.md` §7.13.1): são os pés do
  corrimão nas pontas. As escadas dos lotes medidos aqui ("No Railings",
  "Deck") são modelo 1: nenhum retângulo. A ordem dos eixos, lida de novo:
  o `AddObjectRectangle` exige p1 ≤ p3 e p2 ≤ p4; a ponta de cima vai com
  (r1 − 0,325; r0 − 0,325; r1 + 0,125; r0) (modelo 2/4) e (r3 − 0,125;
  r0 − 0,325; r3 + 0,325; r0) (3/4), a de baixo com (r1 − 0,325; r2; r1 +
  0,125; r2 + 0,325) e (r3 − 0,125; r2; r3 + 0,325; r2 + 0,325), sendo r a
  caixa em `+0xc..+0x18` do que a vtable `0x100` devolve; com o planejador em
  (y, x) (o `FindPathWithPlanner` passa o `FTilePt` trocado), a ponta de cima
  ganha os retângulos do lado de x menor e a de baixo do de x maior,
  qualquer que seja a direção (inferido que a caixa é no mundo; não feito).

### O goal check do bloqueio

- **`DoBlocked`** (BV `0x5762a4`), relido na listagem: com bloqueador
  (`GetBlockingObjectID`, rota vtable `0x1a8`) e caminho de mais de um nó,
  se o gerente de rotas deixa as árvores (vtable `0x20`, byte `+0x20`, 1 do
  construtor BV `0x4f9294`), o **contador por tick** do roteador
  (`cTSRoutingObjectAggregate +0x3c`) sobe; se ele passou de 1 neste tick
  (com o byte `+0x21` do gerente, vtable `0x28`, também 1 do construtor e
  sem gravador fora da vtable, e o `IsPreSimulatorTickEnabled` de quem anda,
  vtable `0x39c` = byte `+0x284`, que o construtor do `cTSObject`, BV
  `0xb0c7a8`, põe em 1 e o `SetPreSimulatorTickEnabled` do `cTSPerson`, BV
  `0xaca0bc`, não muda), **devolve 1 sem mudar de estado**: o `DoBlocked`
  roda de novo no tick seguinte, com o mesmo bloqueador. Senão
  `DoAlongRouteCheckTest` forçado e `TriggerGoalCheckTree` → estado 0x19
  (`+4` = 3); sem goal check, direto a 0x1A (`+4` = 2). Sem bloqueador: a
  1ª vez pausa (0x1D), depois 0x14 com o código 15 (`+0x40` = 7); caminho de
  um nó: código 2 (`+0x40` = 8).
- **O contador por tick**: `OnPreSimulatorTick` do agregado (BV `0x56ee34`,
  só pela vtable) zera `+0x34..+0x40` e chama `FlushManagedDestinationObjects
  (0)` (BV `0x56e9b0`). O `DoWalking` (BV `0x576c40`) sobe o mesmo `+0x3c`
  antes do goal check do andar e, passando de 1, devolve 1 **sem dar o
  passo** (o `DoAlongRouteCheckTest` já anotou: esse goal check se perde).
  Ou seja: **um goal check por tick por Sim**; o do bloqueio que seria o
  segundo espera o tick seguinte.
- **`DoBlockedWaitingForGoalCheckTree`** (BV `0x56da50`): a árvore voltou
  verdadeira (pessoa vtable `0x18` → `0x70`) → estado **0x1A**
  (`DoBlockedGoalStillAvailable`), `+4` = 2. **Não passa pelo `DoWalking`**:
  o passo não é testado de novo e o bloqueador é o que o locomotor guardou.
  Falsa (`0x2a0`) → 0x14 com o código 13 (§9.2).
- **`DoBlockedGoalStillAvailable`** (BV `0x57a8d4`), o começo: bloqueador 0 →
  0x14, `+0x40` = 0xC, código 1; o objeto do bloqueador não existe mais
  (`0x74` do gerente de objetos nulo) → `+0x40` = 0xD, código **1**; não é
  pessoa (sem a interface `0xfa1c39f7`) → `+0x40` = 0xE, código **15**
  "blocked by object"; pessoa: os ramos da §6/§9.3.
- **No VM**: de volta do goal check do bloqueio (`r.check = 3`) o `advance`
  recomeçava do alto: testava o passo de novo com o Sim parado (a locomoção
  morta no bloqueio), não achava o bloqueio, esquecia o bloqueador e andava;
  no tick seguinte o passo entrava no objeto de novo. O contador nunca subia
  e a rota nunca falhava: os sociais presos da rodada n31 (o Daniel do
  `N001_Lot68` com o passo dentro do balcão 151, a Cassandra do `N001_Lot11`
  no 691), e a "Wait - Person X State" do outro Sim também não acabava.

### O cercado das urnas do `N003_Lot52` (não é a mesma causa)

O cenário (main `a18ad74d`, semente 10, 3 dias): a Hérmia faz "Chutar" na
urna 153 às 23:49, anda até o **centro** dela (50,50; 41,50; 1) e de lá
toda rota para a casa falha com 2 (264 vezes) até ela morrer. O cercado (x
43–55, y 40–45) tem cerca nos quatro lados e o portão (423/460) a oeste.

- **Por que o centro**: "Interaction - Kick" (`objects.package`,
  `0x7F38B8A7` `0x2062`) vai ao slot de rota 18 da urna (literal, sem
  árvores de falha; se falhar, o 19): um slot de osso (`0x20000`) com o nome
  12 do STR# `0x90` = "route_tombstonemoderate_0_n,_ne,_nw" (o 19: "_s,_se,
  _sw"), os ossos da **lápide**, que o modelo da urna não tem. No binário:
  `NumModelBones` (BV `0xaf4cbc`) conta os nomes da lista (3), sem olhar o
  modelo; o ponto de cada um é o `SlotOffsetFromContainer` (BV `0xafa28c`) →
  `AggregateSlotOffset` (BV `0xafd7ac`) → `nTSSG::cObject::SlotOffset` (BV
  `0x65b03c`): osso que não existe → um vetor estático (`DAT_02c0a948`,
  escrito só na inicialização, BV `0x20a544c`; inferido zero) → o ponto do
  objeto. O `EvalTileForGoal` desses pontos (chamado pelo
  `GetPossibleSlotsFromModelRoutingSlot` com as flags 0 e o alvo como 10º
  argumento) testa o Sim no ponto (`TSObjectCanSetPositionAndOrientation`)
  sem exceção para o alvo, mas a urna deixa pessoas passarem (od 8 `0x10`):
  o centro vale. **O VM faz o mesmo.**
- **Por que não sai**: o caminho do portão até a casa passa pelos lances
  "Stair Modular - Loft" (x 36, y 23–27, as duas pontas no andar 1). O
  "Function - Portal - TEST" das escadas modulares (`0x7F117CD9` `0x2016`:
  a idade; o slot 1 da ponta de saída, "Get in Temp 0 - Stair - Top/Bottom";
  um Snap de teste, `0x2E` modo 3 flags `0x12`) falha com 8 para quem está
  no cercado: o `TrySnap` (BV `0xe43084`), com o slot sem a RS flag
  `0x80000`, no mesmo andar e **com x e y diferentes**, procura parede entre
  onde o Sim está e o ponto (gerente de partições `0xa0(5)` → `0x4c`; não
  porta; e o `0xac` da parede) — e a cerca do cercado está no meio: os
  quatro lances ficam trancados nos dois sentidos e o planejador não acha a
  casa. Pela leitura **o binário faz o mesmo teste** (não lidos: o alcance do
  `0x4c` — se é o segmento inteiro — e o que o `0xac` da parede diz); não
  conferido no jogo (TESTAR). Diferença do VM: ele procura a parede mesmo
  quando x ou y é igual (o binário só com os dois diferentes); não muda este
  caso.

### O que mudou no código (rodada n31b) e as medidas

Um commit por parte, `ctest -j2` verde em cada um (58 testes; novo:
`deck_stairs`):

1. **O piso debaixo dos degraus** (`world.cpp` `World::stairOver`,
   `path_planner.cpp`): o `addStair` anota os tiles debaixo dos degraus em vez
   de tirar o piso deles; o planejador só os fecha quando a escada não é de
   objetos (os mundos sem objetos dos testes) ou com
   `OS2_STAIRS_OUT_OF_PLANNER=1`. `walkable()`/`hasFloor()` continuam a
   deixá-los de fora (Ir Aqui, colocação: inferido, não mexido). Teste
   `deck_stairs`: Juliette e Tybalt saem do deque do `N003_Lot192` em 7.000
   ticks, com no máximo 2 "Leave" cada.
2. **Do goal check do bloqueio direto ao contador** (`routing_walk.cpp`
   `advance`): de volta do goal check (`r.check = 3`) o passo não é testado
   de novo; o bloqueador guardado vai ao `DoBlockedGoalStillAvailable`
   (sumido: falha com 1). Teste `test_routing`: um objeto posto no caminho
   depois do plano e tirado durante o goal check ainda faz a rota falhar com
   15 (antes ela seguia e chegava).
3. **Um goal check por tick** (`countGoalCheck`; `Thread::turn` = a chamada
   do `VM::run`, um turno do Sim por tick; `Object::goalCheckTurn`): passado o
   primeiro, o andar fica parado aquele tick e o check anotado se perde; o
   `DoBlocked` espera o tick seguinte com o mesmo bloqueador
   (`blockCheckDone` = `kBlockPending`, no mesmo byte salvo). Teste: no
   primeiro turno da rota de dentro de um goal check só um check rodou.
   Inferido: que o `OnPreSimulatorTick` do agregado roda uma vez por tick
   (o nome e quem o chama, `AggregateOnPreSimulatorTick`).
4. Depuração: `OS2_ROUTE_FAILS_NEAR` com as pontas e andares dos portais
   trancados, `OS2_ROUTE_FAILS_MAP` com os objetos que bloqueiam a 2 tiles.

Medidas de 3 dias (`s2pkg autonomy`, `S2PKG_VISITORS=1 S2PKG_FIRES=1`; main =
`a18ad74d`; "parte 2" = partes 1 e 2; "n31b" = tudo). Contam só os Sims da
casa; os fogos mudam de semente quando a ordem dos eventos muda, então as
mortes contam por corrida com ou sem fogo:

| | main | parte 2 | n31b |
|---|---|---|---|
| `N003_Lot192` 12 h: Juliette / Tybalt | "Leave" 117 / 112 vezes, não saem | saem 23:58 / 00:10 (1 "Leave" cada) | idem |
| sociais ≥ 10 h (`N001_Lot68` s1–6, `N001_Lot11` s1–6, `N003_Lot52` s1–12) | 5: "Estapear" 62 h+ (68 s2), "Dar Coque" 23 h+ (52 s1), "Fofocar" 19 h e "Brincar de Tapa na Mão" 19 h+ (52 s7), "Papear" 22 h (52 s10) | 0 | 0 |
| outras ≥ 20 h | "Relaxar" 60 h+ (68 s5), "Dormir" 27 h+ (68 s2, s4), "Ler" 47 h+ (11 s4) | "Dormir" 28 h+ (68 s2), "Levando a criança" 40 h (11 s4) | "Dormir" 54 h+ (68 s6) |
| mortes em corridas sem fogo / com fogo (corridas com fogo), 68 · 11 · 52 | 0 / 4 (1) · 0 / 0 · 6 / 20 (6) | 2 / 8 (2) · 0 / 0 (2) · 1 / 20 (6) | 1 / 12 (3) · 0 / 0 · 6 / 12 (4) |
| mortes no cercado das urnas (52) | 4 | 3 | 4 |

As 14 sementes × 12.000 ticks (`OS2_SEED` 0–13, `OS2_ROUTE_FAILS=1`,
`OS2_DEBUG_BLOCK=1`; "resto" = as falhas sem o 13 de propósito dos sociais;
"de quem anda" = o resto sem o "Move Out Of Way" de quem foi pedido, "Okay to
Stand Here?", "Command - Reposition" e a árvore de falha):

| | main | parte 2 | n31b |
|---|---|---|---|
| `N001_Lot68`: resto / de quem anda / bloqueios por passo | 51 / 40 / 11 | 62 / 47 / 6 | 201 / 87 / 88 |
| `N001_Lot11`: resto / de quem anda / bloqueios por passo | 3.058 / 341 / 87 | 1.604 / 250 / 65 | 68 / 68 / 12 |
| interações autônomas que acabam falsas (68 / 11) | 19 / 145 | 22 / 100 | 22 / 0 |
| `stair_top` (sementes 1–8, falhas no alpendre) | 0, 1, 5, 0, 0, 8, 0, 0 | — | 0 em todas |

O `N001_Lot11` deixa de ter o alpendre preso (o "Move Out Of Way" dos
pedidos aos Sims parados ali somava quase todo o resto). No `N001_Lot68` o
resto sobe por uma semente: na 7 a Mary-Sue, a 0,015 da quina da mesa 639
(dentro da inflação), refaz por uma hora (11.529 → 13.339) "Route To Can"
(14) e a caminhada da árvore de falha, cujo primeiro passo (o giro do
clipe de partida) entra na pegada da mesa: 15, como no binário com essa
ordem; antes o passo testado de novo com ela parada passava. Fica no TODO
(o passo de partida junto de uma quina).

## 10. O que falta

- Os retângulos de custo 10 junto das pontas das escadas reta e modular
  (§9.1, §9.2, §9.4: são os pés do corrimão, modelos 2–4 da entrada 48;
  passáveis; nenhum nos lotes medidos; falta saber se a caixa `+0xc..+0x18`
  da vtable `0x100` é no mundo) e os portais dinâmicos dos dados de objeto
  `0x58`–`0x61` (só no BV).
- O cercado das urnas do `N003_Lot52` (§9.4): ler o alcance do `0x4c` do
  gerente de partições (`0xa0(5)`) e o `0xac` da parede no teste de parede do
  `TrySnap`, e ver no jogo se quem chuta uma urna lá à noite sai; no VM, o
  teste de parede do Snap só quando x e y mudam, como o binário.
- O passo de partida junto de uma quina (§9.4, `N001_Lot68` semente 7): um Sim
  a 0,015 de uma pegada, dentro da inflação, cujo clipe de partida o leva
  para dentro dela, falha com 15 a cada rota por uma hora.
- O `+10` do `WorldOutsideSupport` e o estado 7 no custo do piso (§9.1).
- O alpendre do `N001_Lot11` (rodadas n18b, n23, n30; §9.2, §9.3): os
  impasses das sementes 2 e 4 acabaram com a leitura da rodada n30 (o passo
  de 0,3 para sair do caminho, o contador, o "CT - Route Blocked" que
  replaneja, o aviso a quem pediu). Fica o que o binário também não desfaz,
  pela leitura: um Sim que para no alpendre (um social com outro que está
  ali, um passo para fora do caminho de outro) o segura enquanto as suas
  interações falham, ninguém tendo para onde sair (o alpendre fica na sala
  0, o lado de fora de um andar cujo único piso ali é ele; os slots de
  "Okay to Stand Here?", "Command - Reposition" e os globais 8 e 6 do "Move
  Out Of Way" não acham ponto). Em 3.000 ticks: 3 das sementes 0–27 (6, 10 e
  21; antes 6: 2, 4, 10, 12, 15, 18). Não lido: se o relógio do jogo traz
  outra saída (a autonomia que muda de escolha por outra razão), só no jogo
  rodando (TESTAR).
- A escala de x,y por 1/16 do `cTSSlotDescriptor::Read` até a "versão"
  `0x46` (os SLOT `0x43`–`0x46` do jogo base já estão em tiles: qual versão
  ele compara?).
- Confirmar que nada escreve a escala de portal (`DAT_02d86430`) por um
  caminho indireto.
