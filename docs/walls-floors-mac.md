# Paredes e pisos no binário de Mac

Rodada n19 (2026-10-15), linha 19 do `docs/inventario-binario.md` §6.
Binário: Bon Voyage 1.10 (`sims2_bv.ppc`), endereços BV. Modo: **Ghidra
MCP**, só leitura (decompilar, ler memória e `disassemble_bytes` com
`dry_run`, sem criar função nem renomear). Os nomes vêm do `symxfer` (mapa
regerado na rodada, fora do repositório) e das vtables; nesta área ele erra
bastante: `0x749f98` sai como `cWaterGeometryBuilder::SetWaterDepth`,
`0x745cfc` como `cTSObjectAssetCollector::Init`, `0x6d1558` como
`cFloorGeometryNode::WorldLocationsForPatternRotation` (é o
`cFenceManager::SetLevelRange`). Os nomes abaixo foram conferidos pelo que
cada função faz e por quem a chama.

## 1. As classes

- `nTSSG::cWallManager`: uma lista de níveis (`cWallLevel`, `0x34c` bytes),
  cada um com **seis nós de geometria** (`cWallGeometryNode`), um por
  `tWallTypeIndex`, e um grafo de paredes por índice (`+0x5c + 4·i`).
  `CreateWallNodesOnLevel` (`0x6c2744`) cria os seis e regula cada um
  (vtable da interface: `0x18` espessura, `0x1c` altura baixada, `0x20`
  deslocamento de altura, `0x24` "vertical span", `0x28` ignora o modo de
  parede, `0x2c` nome do subconjunto dos vértices, `0x30` `AddWallSide`,
  `0x3c` `AddWallTop`, `0x48` `RefreshWallVertex`, `0x4c`
  `SetVertexUpDownState`, `0x50` `SetSegmentUpDownState`, `0x54`
  `SetUpDownState`):

  | índice | quem vai (`AddWallImpl` `0x6c64c8`) | espessura | desloc. | span | modo de parede |
  |---|---|---|---|---|---|
  | 0 | `floorToFloor`: paredes, fundação (23), piscina (29) | 0,15 | −0,01 | 1 | segue |
  | 1 | `floorToFloorBasedCeiling`: saias de deck (16, 24) | 0 | 0 | 4 | ignora |
  | 2 | `floorToRoof`: sótão (3) | 0,15 | −0,01 | 2 | segue |
  | 3 | meia parede (`PartitionStyleIsHalfWall`) | 0,15 | −0,01 | 0 | segue |
  | 4 | parede de palco (só expansão) | 0,1 | −0,01 | 5 | ignora |
  | 5 | meia parede com as duas pontas num quarto de marcador 4 (`FUN_00166414`; rodada n25: o estilo de deck/fundação cru 4, nenhum no jogo base) | 0,1 | **+0,75** | 0 | segue |

  Os padrões do construtor do nó (`0x74ab40`): espessura 0,15 (`+0x1c4`),
  altura baixada 0,2 (`+0x1c8`), deslocamento −0,01 (`+0x1cc`), span 1
  (`+0x1d0`). O "span" é o `verticalSpan` dos `walls.txt`/`fences.txt`
  (tabela `0x28660e0`: `fixedHeight` 0, `floorToFloor` 1, `floorToRoof` 2,
  `floorToStairs` 3, `floorToFloorBasedCeiling` 4).
- `cWallGeometryNode`: as peças de parede. Cada **face** é um
  `cWallSide` (6 vértices + a tampa da ponta, `cWallSide::InsertVertices`
  `0x745ec4`), cada **topo** um `cWallTop` (6 vértices,
  `cWallTop::InsertVertices` `0x745cfc`), os dois `cWallSegmentPiece`;
  cada vértice do grafo com 3 ou mais paredes ganha um `cWallVertexPiece`
  (`BuildVertices` `0x74a40c`), o polígono do encontro.
- `cWallManager::AddWallToGeometry` (`0x6c642c` → `0x6c6340`) põe em cada
  parede: a face do localizador, a face do localizador oposto
  (`OppositeWallLocator`) e um topo (subconjunto `"wall_top"`).
- `cFloorManager` / `cFloorGeometryNode`: um nó por andar; cada **quarto
  de tile** é um triângulo (§4). `cFloorThicknessGeometryNode` (a borda dos
  pisos, existe também no 1.0 base) não foi lida nesta rodada.
- `cSimplifiedWall` só serve ao `cLotImposterManager` (as paredes
  simplificadas dos lotes vistos do bairro): não entra no lote.

## 2. Uma face e um topo (`cWallPiece::ComputeMetaData` `0x749138`, `ComputeSegmentVertices` `0x744520`)

`cWallSegmentPiece::BuildVertices` (`0x749f98`; quem chama:
`AddWallSide` `0x74a258`, `AddWallTop` `0x74a0a4`) recebe a parede
vizinha a ignorar, `+0x34` = "textura presa no topo" (é o
`PartitionStyleRequiresFlatTop` do estilo, `RefreshAdjacentWalls`
`0x6c3738`), `0x61` = `WallsUp()` do nó, a espessura, o deslocamento, a
altura baixada e o span.

**Metadados** (por localizador `a → b`):

- `P0`, `P1` = as pontas no chão (`SampledFloorElevation`), `d = P1 − P0`,
  `N = normalize(cross(normalize(d), cima))` = a **direita** de `a → b`
  (a face A; a B é a esquerda). `h` = espessura / 2.
- Os quatro cantos começam em `±h·N` e são **chanfrados** pelos vizinhos do
  mesmo nó (mesmo grafo) em cada ponta: na ponta, para a face com normal
  `n_f`, a parede vizinha que fica daquele lado (a primeira girando de `d`
  para o lado da face; na ponta `b`, de `−d`) tem a face `n_g` virada para
  o mesmo ângulo, e o canto vira `h·m / (m·n_f)` com `m =
  normalize(n_f + n_g)` (início A: `N − N₁`; início B: `−N + N₂`; fim B:
  `−N − N₃`; fim A: `N + N₄`, com `Nᵢ` a direita da vizinha saindo da
  ponta). Sem vizinha (ponta solta) fica `±h·N` e o flag `+0x60` liga a
  tampa (§2.3).
- Os 6 pontos: `Q0 = P0 + A₀`, `Q1 = meio + h·N` (no chão do meio, sem
  chanfro), `Q2 = P1 + A₁`, `Q3 = P1 + B₁`, `Q4 = meio − h·N`, `Q5 = P0 + B₀`.

**Alturas** (`WallTopElevation` `0x743ab0`, em `P0`, no meio e em `P1`):
span 0 → chão + **1,025**; 1 → o chão do andar de cima (no último andar,
o chão + a altura do andar); 2 → `max(RoofHeightAtLocation − 0,25, chão)`
(`cWorldRoof` `0xc23f8c`: o maior `GetSampledElevationByRoofID` dos
telhados do andar sobre o ponto; sem telhado, o chão de cima); 4 → o chão
de cima − 0,15; 5 → chão + 0,75. Cada coluna sobe `H` = topo − chão
(paredes erguidas) ou a altura baixada (0,2); todos os z ganham o
deslocamento (−0,01).

### 2.1 Face (`cWallSide`, 6 vértices: baixo `Q0 Q1 Q2`, alto `Q2 Q1 Q0`; índices `0x2735d00`: 0 1 4, 4 5 0, 1 2 3, 3 4 1)

Normal `N`. Três UVs por vértice (`cWallRenderVertex`, 48 bytes: posição,
normal, UV0, UV1, UV2), que o `WallpaperAndMask` usa como papel, máscara e
light map (`docs/shaders-mac.md` §5):

- **UV0, papel:** `u` = **0, 0,5, 1** no início, meio e fim (um papel por
  trecho de parede, também na diagonal, que estica √2; o chanfro não mexe
  no `u`). `v` embaixo = `D/3` com a textura presa no topo
  (`requiresFlatTop`: paredes, fundação, piscina, saias) ou **1** sem
  (sótão, meia parede); +0,05 no span 4. `v` em cima = `v` embaixo − `H/3`.
  `D` é a altura cheia (topo − chão), mesmo com a parede baixada: **a
  parede cortada mostra o pé do papel** (o rodapé), não o alto.
- **UV1, máscara:** `u` = `(d̂·A₀)/L`, 0,5, `1 + (d̂·A₁)/L` (segue o
  chanfro), `v` = 1 embaixo e `1 − H/3` em cima.
- **UV2, light map:** a célula da face no light map
  (`cLightMapManager`, `TSSGSystem` vtable `0xc0`, `0x50`), `v` escalado
  por `3 / ceil(H máx)` (1 nos spans 0 e 5).

### 2.2 Topo (`cWallTop`, 6 vértices `Q0..Q5` no alto; índices os mesmos)

- Normal fixa **para cima** (o vetor global `0x2c20610`), não a da
  inclinação.
- **UV0 = (x, y) do ponto no mundo** (o "chute" do nosso `wall_top` era
  isso mesmo); UV1 = `(u chanfrado, 1 − H/3 + 0,01)` no lado A e
  `(…, 1 − H/3)` no B; UV2 = um canto fixo (0,999–1, 0,999–1).
- O subconjunto é `"wall_top"` com a máscara `"wallSolid_mask"` e **página
  de light map −1** (`AddWallPieceToSubsetIDMaps` `0x6c60a8`: só as faces,
  tipo 0, pegam a página do `cLightMapManager`; `cWallSubsetID` começa com
  −1, `0x17c7a04`). `CombinedWallMaterial` (`0x6c1738`, sem função no
  projeto) só põe a página se ela for ≥ 0, e o `WallpaperAndMask` sem página
  desenha `2·papel`: o topo é **`2 × wall-top`, sem luz** (a textura é um
  marrom escuro, média (49, 34, 24)).

### 2.3 Tampa da ponta solta (na mesma peça da face)

Só no início do localizador (a outra ponta é o início do localizador
oposto): `Q5 Q0` embaixo e em cima, normal `−d̂`, papel `u` de `1 − espessura`
a 1 com o `v` da face, máscara `u = 0`.

### 2.4 Encontro de 3 ou mais paredes (`cWallVertexPiece`)

Em volta do vértice, para cada parede (menos a que sai), o canto do alto
do lado direito de quem sai do vértice (`Q0` se a parede começa ali, `Q3`
se termina): um leque `0, i, i+1`. UV0 = (x, y), normal para cima,
material o do topo (`InitRenderStatesForVertexSubset` `0x6c1f4c`:
`CombinedWallMaterial("wall_top", "wallSolid_mask", 0, −1, …)`). Com 2
paredes os chanfros já fecham o canto.

## 3. Paredes baixadas

- **Modo baixo:** `cWallManager::SetLevelUpDownState` (`0x6c4e10`) baixa
  todas as peças dos nós 0, 2, 3 e 5 do andar (`SetUpDownState` `0x748a64`).
- **Corte (cutaway):** o `cWallCutawayManager::UpdateSceneGraph`
  (`0xc8cd90`) troca o conjunto de paredes cortadas pelo **conjunto das
  pontas** delas. Cada ponta nova: `SetVertexUpDownState(ponta, baixo)`
  (`0x7487d8`: em toda parede que toca a ponta, os vértices de cima
  daquela ponta, nas duas faces e no topo, e o polígono do encontro) e,
  em toda parede que toca a ponta, `SetSegmentUpDownState(baixo)`
  (`0x748668`: os vértices do **meio**). Ao erguer uma ponta, o meio só
  volta se a outra ponta da parede não estiver baixada. Resultado: uma
  parede inteira fica baixa se as duas pontas estão baixadas (cortada ou
  não), e uma parede que toca uma só ponta baixada **desce em rampa**: cheia
  na ponta erguida, 0,2 do meio até a ponta baixada.
- `SetRenderVertexUpDownState` (`0x747f1c`): o vértice de cima vai ao chão
  + `H` (+ desloc.), com `H` = altura baixada; o `v` do papel e o da
  máscara viram (`D/3` ou 1) − `H/3`.
- Os nós ligados pelo corte: 0 e 2 (as meias paredes 3 e 5 dependem de um
  valor de um serviço `0xe92db716` não identificado, inferido "não").

## 4. Pisos (`cFloorGeometryNode::SetTileQuadrantSubset` `0x75fa6c`)

- `cFloorManager::InitializePatternsAndElevationsOnLevel` (`0x6d277c`)
  passa, para cada tile do andar, o padrão, o material
  (`FloorMaterialName` `0x6d1e24`, o nome do produto) e a **rotação salva**
  (WorldDB vtable `0xa0`) ao `SetFloorTileSubset` (`0x7607ec`), que chama o
  `SetTileQuadrantSubset` para cada quarto presente (máscara `+0xc`).
- **Um triângulo por quarto**: os dois cantos da aresta do quarto e o
  centro, na ordem 0 oeste `(x, y+1), (x, y)`; 1 norte `(x+1, y+1),
  (x, y+1)`; 2 leste `(x+1, y), (x+1, y+1)`; 3 sul `(x, y), (x+1, y)`;
  posição pelo `SampledFloorElevation` (o centro é a média bilinear), normal
  `normalize(cross(v1 − v0, v2 − v0))`. Meio tile diagonal = dois quartos.
- **UV0** (`WorldLocationsForPatternRotation`, `FUN_0075d004`): os cantos
  do quarto `q'` = `q` girado pela rotação (2 → `q+3`, 4 → `q+2`, 8 →
  `q+1`, qualquer outro valor → `q`), e o UV de cada vértice é **(y, x)**
  desse canto no mundo (o centro, `(y + 0,5, x + 0,5)`). É o que o código
  já fazia (o topo da textura no oeste, a direita no norte; a rotação gira
  em volta do centro): agora conferido. A escala
  `floorMaterialScaleU/V` fica no `floor.matShad` (§6 do shaders-mac).
- **UV1**: o retângulo do tile no light map (`cLightMapManager` vtable
  `0x48`), cantos e centro.

## 5. O que o código faz (rodada n19, `src/formats/lot_model.cpp`)

| Peça | Antes | Agora (binário) |
|---|---|---|
| Nós e alturas | espessura 0,15 para tudo, topo no andar de cima, sótão = telhado − 0,1875 (OpenTS2), meia parede 1,0 | `wallNode` (índices 0–3 do §1), deslocamento −0,01 em todos os z, `wallTopElevation` (meia parede 1,025, sótão telhado − 0,25, saia de deck o andar de cima − 0,15 e espessura 0) |
| Cantos | cada trecho esticado meia espessura nas pontas (sobrepostos) | chanfro do `ComputeMetaData` com as paredes do mesmo nó; tampa nas pontas soltas |
| Face | um quad por trecho (dois no sótão), `u` = posição no mundo, `v` = (topo − z)/3 com o topo **cortado** | três colunas (início, meio, fim); `u` 0/0,5/1; `v` a partir do topo cheio (`requiresFlatTop`) ou de 1 no chão; a parede cortada mostra o pé do papel |
| Topo | quad com `u, v` = (x, y), luz do piso do andar (inferido), normal da inclinação | 6 pontos chanfrados, (x, y), normal para cima, sem light map: material `wall_top 2x` (`2 × wall-top`, sem luz) |
| Encontros | nada (os trechos esticados se cruzavam) | polígono em leque onde 3+ paredes do nó se encontram |
| Corte | parede cortada inteira; as outras inteiras | pelas pontas das cortadas: rampa na parede que toca uma ponta baixada; inteira baixa com as duas pontas baixadas (e só essas escondem o que está pendurado nelas, `HideShowAttachedObjects` `0x6c2ee4`) |
| Paredes baixas | normais e sótão | normais, sótão e meias paredes (nós 0, 2, 3) |
| Pisos | já um triângulo por quarto com (y, x); rotação lida como potência de 2 | igual, rotação só 2/4/8 (os lotes do jogo têm todos 0 ou 1) |

Testes: `testWallGeometry` (chanfro, tampas, encontro, UV e material do
topo, o pé do papel, a rampa, meia parede, UV e rotação dos pisos) em
`tests/test_formats.cpp`. Os dados: as 18 531 paredes dos 69 lotes têm 1
ou √2 de comprimento (um localizador cada), 184 são saias de deck 16.

Antes/depois (em cima ou à esquerda o antes), `--ui live` com a câmera
inclinada:

- `docs/screenshots/n19-paredes-corte-antes-depois.jpg`: N001_Lot11 e
  N003_Lot52 em corte: as rampas nas paredes que tocam as cortadas, o pé do
  papel nas cortadas, os topos;
- `docs/screenshots/n19-paredes-cantos-topos-antes-depois.jpg`: cantos
  chanfrados e topos (N001_Lot11 de perto, a baía de N001_Lot77 com as
  paredes erguidas, o salão de N002_Lot31);
- `docs/screenshots/n19-paredes-baixas-antes-depois.jpg`: paredes baixas;
- `docs/screenshots/n19-pisos-antes-depois.jpg`: pisos (N002_Lot3 com as
  paredes diagonais e meios tiles, a cozinha xadrez de N001_Lot11): iguais.

**Inferido ou de fora:**

- a máscara de portas e janelas segue a projeção plana da rodada 16 (o `u`
  ao longo do −x do objeto); o jogo usa o UV1 de cada vértice e o
  `wallMaskMirrorFlag` do `CombinedWallMaterial`, não lidos por inteiro;
- ~~o nó 5 (meia parede a +0,75 e 0,1 de espessura sobre piso "tipo 4") não
  é separado do 3, como o +0,75 das cercas~~ (rodada n25,
  `roof-fence-water-mac.md` §2.1: o "tipo 4" é o marcador cru 4 do 3ARY
  `0x14`, que nenhum produto do jogo base tem; feito: a cerca coberta com as
  duas pontas nele vai para o nó 5); a meia parede segue sendo "cerca com
  revestimento" (inferido: o jogo pergunta o `PartitionType` 3 do estilo);
- no modo baixo, fundações e paredes de piscina (nó 0) ficam erguidas (quem
  chama o `SetLevelUpDownState` para quais andares não foi lido); as meias
  paredes não entram no corte (o valor do serviço `0xe92db716`);
- o light map das faces é o nosso atlas (`LotLightMapLayout`, a célula
  cobre a face chanfrada), não o do `cLightMapManager`;
- as laterais dos buracos sem máscara (batentes) são nossas, no material do
  topo;
- `cFloorThicknessGeometryNode` (a borda dos pisos) não é desenhado.

## 6. Rodada n36: borda dos pisos, máscaras, sem máscara, meia parede

Rodada n36 (2026-10-15), linha 36 do `docs/inventario-binario.md` §7.
Modo: **Ghidra MCP**, só leitura (decompilar, ler memória e
`disassemble_bytes` com `dry_run`; nada criado nem renomeado). A classe da
borda dos pisos quase não tem função no projeto BV: foi lida no **1.0 base**
(`sims2_base_1.0.ppc`, onde as funções existem) e conferida no BV pelo
`disassemble_bytes` (o construtor da peça e a espessura); os nomes vêm do
`baserom.sym` (o outro build ppc com símbolos: `0xcb2808..0xcb5a7c`).

### 6.1 A borda dos pisos (`nTSSG::cFloorThicknessGeometryNode`)

- **Quem a cria:** o andar de pisos (`cFloor`, BV `FUN_006503f0`, o que
  monta os subconjuntos `floorTile_%s_room_%d_sector_%d_%d_grid_%s`) dá a
  cada subconjunto de piso **de andar ≥ 1** que não é de grade (`grid_no`)
  um subconjunto no nó de espessura (`param_1[9]`), com o material
  **`FloorThickness`**. No térreo (0) e nos andares negativos (piscinas,
  porões) não há borda.
- **O material** (`Materials.package`, `materialDefinition FloorThickness`):
  `StandardMaterial`, `stdMatDiffCoef (0.5, 0.5, 0.5)`, `stdMatSpecPower 0`,
  textura `floor-edge`. Iluminado pelas luzes como um objeto (sem light map).
- **Que arestas** (`UpdateOrderedLocatorSet`, 1.0 `0x9d6628`): para cada
  quarto de tile do nó, as três arestas do triângulo do quarto viram
  localizadores ordenados (`OrderedLocator`, 1.0 `0x9d41b4`: as pontas na
  ordem menor primeiro) e cada um **entra no conjunto se não está e sai se
  está** (`+0x14c`). Sobram as arestas da borda da união dos quartos do
  andar: a beira do piso (de tile inteiro ou de meio tile diagonal); as
  arestas entre dois quartos com piso se cancelam.
- **A peça** (`cFloorTilePiece`, BV `0x7ab07c`, 1.0 `0x9d4320`): o
  triângulo do quarto (`FUN_0015cb40`: quarto 0 `(x, y+1) → (x, y)`, 1
  `(x+1, y+1) → (x, y+1)`, 2 `(x+1, y) → (x+1, y+1)`, 3 `(x, y) → (x+1, y)`,
  depois as duas meias diagonais até o centro), cada aresta `a → b` um quad
  de 4 vértices (32 bytes: posição, normal, UV): `a` e `b` no
  `SampledFloorElevation` (`0x8a5fc0`) e os mesmos **0,15 abaixo**
  (constante `0x274b638`), normal `Perp(b − a)` normalizada
  (`0x7ab05c`: `(d.y, −d.x, 0)`, a direita da aresta, para fora do
  triângulo), UV `(0, 0)` em `a` em cima, `(1, 0)` em `b` em cima,
  `(0, 1)` e `(1, 1)` embaixo; índices `0 2 1, 1 2 3`. Só os quads das
  arestas do conjunto vão para o subconjunto
  (`InsertFloorTilePieceInGeomPart`, 1.0 `0x9d5c10`).
- Os 0,15 batem com o `walls.txt`: `floorToFloorBasedCeiling` (as saias de
  deck) "tem o topo no fundo da geometria de espessura do piso de cima".

### 6.2 Máscara de porta e janela: UV, espelho, topo

- **O material** (`CombinedWallMaterial`, BV `0x6c1738`, sem função no
  projeto): com a máscara, lê do TXMT da máscara a propriedade
  **`tsMirrorWallMask`** (vtable `0x60` do material, padrão de
  `0x2c13d50`) e a passa como **`wallMaskMirrorFlag`** ao
  `WallpaperAndMask` (o nome do material inclui o valor:
  `%s%s%s_%d%s%s` = papel, máscara, espelho, página, ...). O shader espelha o
  UV 1 por `ffTextureMatrix -scalev (−1,1) -trans (1,0)` (`u → 1 − u`,
  `shaders-mac.md` §5). Nos dados: 283 dos 306 TXMTs `WallMask` têm a
  propriedade; as faces `s` retas têm todas 1, as `n` 0 (as duas usam quase
  sempre a mesma textura, a do `n`); nas diagonais `nw`/`se` o valor varia
  por objeto (`s2pkg material-survey <Res> WallMask --list` mostra o
  `mirror=`).
- **O UV da máscara é o UV1 da face** (§2.1): `u` ao longo do localizador
  da própria face, 0 no começo e 1 no fim (com o chanfro: `(d̂·A₀)/L` no
  canto), `v` = 1 no chão da coluna e `1 − H/3` no alto (H a altura
  desenhada: baixada, `1 − 0,2/3`). As duas faces de uma parede correm em
  sentidos opostos; o espelho da face `s` põe o buraco no mesmo lugar do
  mundo que o da `n`.
- **Quem recebe a máscara** (`SetCutoutMaterialImpl`, BV `0x6c5bb0`): para
  o localizador dado (a face), `0x6c5564(localizador, nó, peça, …, máscara)`
  nos nós **0 e 2** (paredes de andar a andar e sótão; não as meias
  paredes, saias de deck nem o palco), na peça **0 (a face) e 1 (o topo)**.
  O topo (um por parede, no localizador `a → b`) só acha o subconjunto
  quando o localizador é o dele: a máscara da face A vai também para o topo,
  que a amostra com o seu UV1 (`u` ao longo de `a → b`, `v = 1 − H/3`, +0,01
  no lado A): numa parede inteira (H = 3) cai na linha de cima da máscara
  (sólida: o topo fica), numa parede baixada (H = 0,2) cai perto do pé, onde
  a porta é vazada: **o topo da parede cortada some sobre a porta**.
- **O nome de cada face** (`cObject::ComputeWallCutoutLocators` `0x6612d8`
  → `WallCutoutMaterialName` `0x658428`): o localizador da face, relativo ao
  centro do tile do objeto líder (a parte + a diferença das posições,
  `0x8b5130`), num referencial `(X, Y)` em que o dígito E/W vem de `Y` e o
  N/S de `−X`; a direção da face pelo sentido do localizador (`X` igual: `Y`
  crescendo `S`, decrescendo `N`; `Y` igual: `X` crescendo `W`,
  decrescendo `E`; diagonais `X↑ Y↓ NW`, `X↓ Y↑ SE`, `X↑ Y↑ SW`,
  `X↓ Y↓ NE`, tabela `0x27cdeac`) e os dígitos com o arredondamento do lado
  da face (`S`: `X + 0,5`; `N`: `X − 0,5`; E/W pelo `Y` do mesmo jeito), o
  nível = andar da parte − andar do líder. No referencial do objeto (o seu
  `+x` = leste, `+y` = norte), `X = −y`, `Y = x` (inferido: com ele os nomes
  saem os dos dados, a janela de 1 tile na borda `y = −0,5` do seu tile:
  face `n` `0_0`, face `s` `0_1s`).
- O `+0x124` do objeto (os lados com recorte, `0xaf815c`) não foi achado
  em quem escreve; o código usa os nomes que existem.

### 6.3 Sem máscara, sem buraco

`ComputeWallCutoutLocators`: com o texto do STR# `0x85` vazio não há
localizador nenhum, e `SetCutoutMaterialImpl` só troca o material da face
(e do topo) pelo da máscara: o jogo **não abre caixa** em parede nenhuma.
Uma porta ou janela sem o TXMT da face deixa a parede inteira (rodada n20
já tinha lido; agora feito).

### 6.4 Meia parede e cerca

- **`AddWallImpl`** (BV `0x6c64c8`; o symxfer o chama
  `cNHoodAutomata::ReadTrackData`): sai sem geometria se o estilo não é
  parede (`PartitionStyleIsWall` `0x8b566c`: `PartitionType` 0 ou 3) nem
  palco (`IsStageWall`); nem se os dois tiles ao lado da parede estão fora do
  lote (WorldDB `0xf8`). O nó: `floorToRoof` 2, `floorToFloor` 0, meia
  parede (`PartitionType` 3) 5 com `FUN_00166414` senão 3, palco 4, o resto
  1. **Cerca (`PartitionType` 1) nunca vira parede**, com ou sem papel.
- **`PartitionType`** (`0x8b5614`) = o campo `+0xc0` do produto da partição
  (`PartitionProductOfStyle` → interface, vtable `0x10c` → `0xf1bde4`;
  quem grava, vtable `0xcc` → `0xf1b8ec`). Os parsers do `walls.txt`
  (`wall`, `defaultPattern`, `wallThickness`, `invisibleLightBlocking`,
  `stageWall`), do `fences.txt` (`fence`, `railModel`, `diagRailModel`,
  `postModel`) e do `fencearches.txt` não têm palavra de meia parede; a
  cerca do catálogo (property set `0x2CB230B8`) tem **`ishalfwall`**
  (`ReadProperties` BV `0x84f518`, campo `+0x64`; `FillProperties`
  `0x84d910` escreve `rail`, `diagrail`, `post`, `height`, `maywalkover`,
  `posttilespacing`, `ishalfwall`, `bearsload`, `noshadow`,
  `isgreenhouse`). Quem passa o `ishalfwall` ao tipo 3 não foi achado
  (inferido: é o produto feito da cerca do catálogo com `ishalfwall`).
  Nenhuma cerca do catálogo do jogo base tem `ishalfwall` verdadeiro.
