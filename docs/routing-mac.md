# Roteamento: o que o binário de Mac faz

Fontes e convenções como em `docs/primitives-mac.md`. **Cobertura parcial**:
a arquitetura, os obstáculos e os custos; faltam o A* em detalhe, os
códigos de falha e a evitação entre Sims. Classes: `cTSRouteManager`
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

## 4. Comparação com o código atual (`src/vm/world.*`, `routing.cpp`)

| jogo | código |
|---|---|
| quadtree de retângulos contínuos, custos por material/desnível | A* em tiles inteiros com 8 vizinhos |
| obstáculos inflados 1/8 de tile para o corpo | tile inteiro bloqueado |
| portas e escadas como portais com custo de transição | portas abrem a aresta; escadas por tile de patamar |
| caminho suavizado (Laplaciano 0,45/0,325, corte de quinas) | centros de tile |

Para o modo sem tela basta o A* em tiles; para o Sim andar como no jogo, os
pontos 1 e 3 importam.

## 5. O que falta

- Células mínimas e o custo/heurística do A* (`OpenAdjacentNodes`,
  `CalculateTransitionCost`).
- Evitação entre Sims e re-rota ao ser bloqueado (`cTSRoutingObjectAggregate`:
  `DoWaitingForPortalToBecomeFree`, `TriggerPortalTree`), os códigos de
  falha de rota e as árvores de falha.
- Escolha dos objetivos (`cTSRoute::CreateBestGoalPointList`,
  `EvalTileForGoal`, `ScanCone` para cadeiras).
