# Vista do bairro: o que o binário de Mac faz

Fontes e convenções como em `docs/shaders-mac.md`: endereços do BV 1.10 ppc
(`/mac/sims2_bv.ppc`); nomes conferidos com `baserom.sym` (outro build BV
com símbolos) e as strings conferidas no Mac 1.0 base. "binário" = lido no
código decompilado, "dados" = lido nos arquivos do jogo instalado (base
2004, `data/game`), "inferido" = dedução nossa, "só BV" = ausente no 1.0.

Classes: `cTSNHoodTerrain` (o recurso de terreno, lado do jogo),
`nTSSG::cNHoodTerrain` (o terreno no scene graph), `cTSNHoodOccupantManager`
(ruas, flora e enfeites) e `nTSSG::cNHoodOccupantManager` (os nós deles),
`nTSSG::cLotImposterDisplay`/`cLotImposterManager` (casas vistas de fora),
`nTSSG::cCameraNeighborhood`, `cTSNHoodUserToolBase` e
`cTSUserToolNeighborhoodMoveIn` (clicar num lote), `cTSGameStateController`
(entrar e sair).

Unidades: **1 tile do bairro = 10 unidades** (`terrainCellSize 10` em
`neighborhoodroads.matShad`; `cTSNHoodTerrain::SetTileWidth`, BV `0x4cff7c`,
lê do mundo e guarda largura e 1/largura em `+0x24`/`+0x28`). O bairro base
tem 128×128 tiles = 1280×1280 unidades, Z para cima. Um tile do bairro tem
10×10 tiles de lote: o `LotInfo` (`0x0BF999E7`) dá posição e tamanho do
lote **em tiles do bairro** (ex.: N001 lote 11, 4×3 em (67,56) = um lote
de 40×30).

**Nada disto existe no código atual**: `src/formats/neighborhood.*` lê só
Sims/famílias/relacionamentos/`LotInfo`/NID, e o app só abre um lote
(`--lot`). Este documento é o ponto de partida do agente f.

---

## 1. Recursos do pacote do bairro (`N00x_Neighborhood.package`, grupo 0xFFFFFFFF)

| tipo | instância | conteúdo | quem lê |
|---|---|---|---|
| `0xABCB5DA4` | 0 | terreno (alturas) | `cTSNHoodTerrain::Read`, BV `0x4d4fcc` (fora de função no Ghidra; achado pela vtable `0x27c2150`) |
| `0xABD0DC63` | 0 | ocupantes: flora, ruas, pontes, enfeites | `cTSNHoodOccupantManager::Read`, BV `0x556dc8` |
| `0xEC44BDDC` | 0x262A | lido em `nTSSG::cTSSGSystem::OnNeighborhoodEntered` (BV `0x7bac98`); 115 bytes, 64 zeros + dados | **aberto** (também há 3 no pacote do lote) |
| `0x0BF999E7` | ID do lote | `LotInfo` (já lido em `src/formats/neighborhood.cpp`) | — |

Os dados são little-endian (o Mac troca os bytes ao ler).

### 1.1 Terreno (`0xABCB5DA4`), binário + dados

```
u32  tipo (= 0xABCB5DA4; Read recusa se diferente)
u32  versão (4 em N001..N003/Tutorial; Write grava 4)
u32  tilesX (128), u32 tilesY (128)                       → +0x14/+0x18
f32  nível da água (312,5 nos 4 bairros)                   → +0x34
str  tipo de terreno (u32 tamanho + bytes): "Temperate" | "Desert"   (versão ≥ 2)
camada de elevação (versão ≥ 3):
  u32 0x6B943B43, u32 1, str "c2DArray", u32 129, u32 129,
  129×129 f32 = altura de cada vértice (N001: 122..512)
camada de propriedades de vértice:
  u32 0x6B943B43, u32 1, str "c2DArray", u32 129, u32 129, 129×129 u8
  (zero nos 4 bairros; tamanho confere byte a byte)
```

Versão 1 = sem tipo de terreno; < 3 = sem camadas (o jogo cria a de
elevação vazia, `InitTerrainElevationLayer`). Ao terminar o Read chama
`InitCommon`. As alturas são absolutas (a água fica em 312,5).

**Altura num ponto** (`GetTerrainHeight`, BV `0x4d15bc`): fora de
[0, tilesX·10]×[0, tilesY·10] devolve 0; senão i = ⌊x/10⌋, j = ⌊y/10⌋
(presos a tiles−1), fx = x − 10i, fy = y − 10j, h00/h11 os cantos da
diagonal, e o tile é dividido pela diagonal (0,0)–(1,1):

```
se fy < fx: h = h00 + (fx·(h10−h00) + fy·(h11−h10)) / 10
senão:      h = h00 + (fy·(h01−h00) + fx·(h11−h01)) / 10
```

### 1.2 Ocupantes (`0xABD0DC63`), binário + dados (o parse de N001 fecha no último byte)

```
u32 versão (111 = 0x6F; ≤ 0x6C → vazio)
4 listas, cada uma: u16 versão da lista, u32 n, n registros:
  flora (versão 8, 763 em N001)
  ruas (versão 3, 391)
  ruas com modelo = pontes (versão 3, 13)
  enfeites "prop" (versão 8, 72)
```

Cabeçalho comum (`cTSNHoodOccupant::Read`, BV `0xb1b6c8`): `u8 versão`,
posição `f32×3` (`+4`), retângulo `f32×4` (minX, minY, maxX, maxY; versão < 2
→ vazio). Depois:

- **flora** (`cTSNHoodFloraOccupant::Read`, BV `0xb1b9d0`, versão ≥ 6):
  cabeçalho, `u8` versão, `f32` rotação em graus em torno de Z (≥ 7), `u32`
  ID do produto (> 7).
- **enfeite** (`cTSNHoodPropOccupant::Read`, BV `0xb1b800`, versão ≥ 7):
  cabeçalho, `u8`, `u32` ID do produto (> 7), `f32` rotação.
- **rua** (`cTSNHoodRoadOccupant::Read`, BV `0xb1caf8`, versão > 2):
  cabeçalho, `u8`; 4 cantos (`f32×3` posição + `f32×2` UV) = o quad já
  colado no terreno, nos cantos do tile (ex.: (10,1020)…(20,1030)); `u32`
  peça (`+0x7c`), `u32` flags (`+0x80`, sempre 0), `u8`, `u8`, `u32` n de
  peças extras e n × (`u32` + 3 × (`f32×3` + `f32×2`)) (0 em N001). Versões
  < 2 multiplicam posições por 1,25 (bairros antigos).
- **ponte** (`cTSNHoodRoadOccupantWithModel::Read`, BV `0xb1cd60`): a rua
  acima, mais `u8`, `f32×3` (`+0x9c`), quatérnio `f32×4` (`+0xa8`), `f32×3`
  (`+0xb8`; lixo em N001, não usado pelo que vimos).

**Produtos de flora e enfeites** (dados): `TSData/Res/Catalog/CANHObjects/
catcanhobjects.bundle.package`, 84 recursos `0x6D619378` (XML de property
set). O ID gravado no ocupante é a chave `guid`; `modelname` é o CRES a
desenhar (`treeBirch06`, `forest_01`, `neighborhood_streetlight`,
`network-prop-69ad0000`…), mais `allowedinlot`, `allowedonroad`,
`removeonlotplop`, `placementsurface`, `sort`, `type` (`canh` = efeitos).
Em N001 a flora usa 17 produtos (70 = `treeBirch06` 169×, 68 = `treeBirch04`
160×, 58 = `forest_01`…). `cTSNHoodOccupant::GetProduct(id)` →
`cTSCatalog::NeighborhoodObjectProductByID` (BV `0xb2b324`). O nó usa o
modelo do produto (`GetModelName` → vtable `0xcc` do produto).

---

## 2. Textura do terreno (decidida no carregamento, não salva)

Os bairros base **não guardam pintura**: o jogo decide a cobertura de cada
vértice ao carregar, por `TSData/Sys/NeighborhoodTerrain.ini` (dados; lido
por `cTSNHoodTerrain::ReadIniFile`, BV `0x4d477c`, com os padrões abaixo se
faltar a chave).

### 2.1 O ini

- `[PaintInfos]` `id=forçaMáx,textura,nVariantes,pinturaDoLote,x0,y0,x1,y1…`
  (`AddPaintInfo`, BV `0x4d5750`): força em `+8`, nome da textura `+0xc`,
  nVariantes `+0x18`, pintura do lote `+0x1c`, curva de espalhamento em
  `+0x28`; o último x da curva é o raio em unidades (`+0x3c`) e
  ⌈x/10⌉ o raio em tiles (`+0x38`).
- `[TerrainTypeInfos]` `Temperate=0x10,0x11` = (penhasco, praia).
- `[PaintMapTable_<tipo>]`: 32 linhas × 16 colunas de IDs de pintura
  (u32 cada, tabela `+0x14` do tipo).
- `[NHoodTerrainTunables]` (padrão no binário → valor do ini base):
  `MaxBeachAltitude` 4 → 4; `MaxBeachWidthInCells` 2 → 2;
  `HeightRangeForTemperatureDistribution` 320 → 500; `CliffAngleMin`
  109,9/3,14 rad… → 52°; `CliffAngleMax` → 56°; `WaterDepthThresholdForPaint`
  15 → 50; `MaxRoadProximityValue` 50 → 20. Os ângulos viram cossenos
  (`cos(graus·3,14/180)`) e k = 1/(cosMin − cosMax).

### 2.2 Proximidades (binário)

- **Água** (`ComputeWaterProximityValues`, BV `0x4d1008`): vértice com
  profundidade > 0 vale 0, os outros `MaxBeachWidthInCells+2`; depois uma
  distância por inundação (4-vizinhos, +1 por passo, BV `0x4d098c`).
- **Rua** (`ComputeRoadProximityValues`, BV `0x4d1848`): vértice cujo tile
  (x..x+1, y..y+1) tem ocupante de rua = 0, os outros `MaxRoadProximity`;
  inundação igual; depois `v = ⌊v·16/MaxRoadProximity⌋`, preso a 15.
  Assim a coluna da tabela (0..15) é a distância à rua em 1/16 de 20 tiles.

### 2.3 Decisão (`DecideSurfaceCover`, BV `0x4d5bc4`)

Para cada vértice (x, y) com altura h, profundidade d = água − h
(`WaterDepth`, vtable `0x44`) e normal n:

```
se d > WaterDepthThreshold: nada
se |n.z| < cosMin (penhasco):
    w = 1 se |n.z| ≤ cosMax, senão (cosMin − |n.z|)·k
    w *= forçaMáx(penhasco); se w > 0: soma(penhasco, w) só neste vértice
senão se proxÁgua ≤ MaxBeachWidthInCells e h ≤ água + MaxBeachAltitude:
    soma(praia, 1) e espalha (abaixo)
senão:
    t   = ⌊(h − água)·31 / HeightRange⌋
    lin = 31 − t, preso a [0, 31]          (lin 31 = beira d'água, 0 = topo)
    soma(Tabela[lin][proxRua], 1) e espalha
espalhar(pintura p, raio R tiles, curva C):
  para cada vértice v' a ≤ R tiles (caixa), exceto
    · (só pinturas normais) os com d' > WaterDepthThreshold,
    · os de praia (h' ≤ água+MaxBeachAltitude, proxÁgua' ≤ MaxBeachWidth e d' ≤ 0):
  dist = √(dx²+dy²)·10; se dist < x_último e C(dist)·forçaMáx > 0: soma(p, C(dist)) em v'
```

`soma` (`AddLandTypeAtVertex`, BV `0x4d0d20`): cada vértice guarda **até 3
pares (pintura u8, peso u16)** com peso = w·255 + 1; pintura já presente
fica com o maior peso; cheio, substitui o de menor peso se o novo for maior.
`NormalizePaintWeights` (BV `0x4d3478`) normaliza depois.

### 2.4 Desenho (`nTSSG::cNHoodTerrain`, texto `neighborhood.matShad`)

- Malha: 4 vértices por tile, 2 triângulos (`AddTileToMeshBuilder`, BV
  `0x6d677c`), separada em blocos de 8×8 tiles; os subsets se chamam
  `"%d_%s"` = (bloco, material) (`AddToPaintSurf`, BV `0x6da3b8`).
- Ordem (camada = `nhoodCanvasLayer` −8): **canvas** preto que só escreve Z
  (`NeighborhoodCanvas`), depois **uma passada por pintura e variante**
  (`NeighborhoodTerrainPaint`, camada −7): `alphaBlend srcAlpha + one`,
  `depthTest acceptIfEqual`, estágio 0 = mapa alfa da pintura (clamp,
  escala `alphaMapScaleU/V` = 1/nº de vértices), estágio 1 = textura da
  pintura repetida a cada `nhoodPaintTextureScale` = 13 tiles, cor =
  `nhoodSunDirBiased`. Ou seja: **as pinturas somam por cima do preto,
  cada uma pesada pelo seu alfa**.
- Material por pintura e variante (`InitPaintDisplay`, BV `0x6dad4c`):
  textura = nome do ini, ou `"%s-%02d"` (`nh-temperate-wet-00`, `-01`…)
  quando nVariantes ≥ 2. O mapa alfa (`CreateAlphaTextureForPaint`, BV
  `0x6db584`) é uma textura de 8 bits do tamanho da grade de vértices,
  preenchida pelos pesos (o preenchimento exato, `UpdateTopMipSurface`, não
  foi lido: **inferido** peso/256 → alfa).
- Variante: a grade é dividida em células de `TextureVariantGridSize` tiles
  (padrão 4, ini 13; `InitPaintVariants`, BV `0x6d8d50`), cada célula
  sorteia um byte e variante = byte mod nVariantes.
- **Luz** (`NHoodTerrainLighting`, camada −5): multiplica o que está na tela
  (`srcFactor(destColor)`) pela textura `nhoodLightMap`, com UV = vértice/128
  (uma amostra por vértice do terreno); recorta o que está abaixo de
  `waterHeight − 0,5` (312,45 no texto). Estado de luz `neighbourhood` em
  `Lights/Lighting.txt`: sol (1,75, −2, 1,5) cor (1, 0,9, 0,8) 1,75; céu
  (−2, 1,75, 2) (0,7, 0,75, 0,8) 0,5; fill1 (0,0,1) 0,25; skylight 0,25;
  `lightMap $nhoodLightMapID` (0x0BE702EF); `hmNHoodShadowStrength 0,47`.
  Pelo `docs/lighting-mac.md` §5, com `lightMap` o iluminamento de uma
  normal é o texel do mapa ambiente em ((n.x+1)/2, (n.y+1)/2) × a cor;
  **inferido**: o `nhoodLightMap` é isso por vértice, com a sombra do height
  map. A geração não foi achada.
- Água: material `nhoodWaterMaterial` (`NeighborhoodWater`, parâmetros
  `ratioW`, `ratioH`, `useReflection`; `CreateWaterMaterial`, BV `0x6d8978`),
  plano no nível da água; constantes no texto (`nhoodWaterAlpha 0,35`,
  reflexo `(0,55, 0,6, 0,65)` 0,75…). Terreno parcialmente submerso usa
  `NeighborhoodWaterTerrain`/`CanvasStraddle`.

---

## 3. Ruas, pontes, flora e enfeites (`nTSSG::cNHoodOccupantManager`)

- **Rua** (`CreateSceneGraphNodeForRoadOccupant`, BV `0x788ad0`): o quad
  gravado (2 triângulos, UVs gravadas) com o material
  `"neighborhood-roads-%s-%08x"` = (tipo de terreno, peça `+0x7c`). Os
  materiais estão em `neighborhoodroads.matShad`: 5 peças por tipo
  (`00004b00`, `00000f00`, `00000300`, `00005700`, `00020700`) com texturas
  `Temperate_roads_00004b04` etc.; `alphaBlend srcAlpha/invSrcAlpha`,
  textura em clamp × `nhoodLightMap` (UV de luz = posição/1280), camada
  `nhoodRoadLayer` −1. As peças extras usam `"neighborhood-roads-%08x"`
  (`bridge-sides`). Em N001: 361× `4b00`, 10× `0f00`, 8× `0300`, 7×
  `20700`, 5× `5700` (os IDs vêm das peças de rede do SimCity 4;
  **inferido**: reta, cruzamento, fim, curva, T).
- **Ponte**: modelo `"%04x-bridge"` com os 16 bits altos da peça
  (`GetModelName`, BV `0xb1a4bc`; N001: `0ac3-bridge`…), posição/quatérnio
  gravados.
- **Flora/enfeite**: o CRES `modelname` do produto, na posição gravada,
  girado pela rotação. A flora é agrupada num nó de geometria próprio
  (`cNHoodFloraGeometryNode`); sombras de árvore na camada −2
  (`NeighborhoodTreeGUOBMaterial`, alfa 0,65).
- `IsAnIntersectionOrBend` (BV `0xb1a3b0`): ligado em (N ou S) e (L ou O).

---

## 4. Lotes vistos do bairro (imposters)

- Cada lote salvo leva no próprio pacote um modelo pronto: CRES/SHPE/GMND/
  GMDC `"<Lote>!imposter"` (ex.: `N001_Lot11!imposter_gmdc`), com TXMT e
  TXTR `walls_N`, `roofs`, `terrain`, `terrainLmap`, `slices_0` (dados,
  `N001_Lot11.package`). O renderer de modelos atual já sabe desenhar isso.
- `cLotImposterDisplay::CreateLotImposter` (BV `0x76b134`) carrega o
  recurso pela chave do `LotInfo` (vtable `0x48`); sem imposter, usa um
  modelo substituto pelo nome (`LotInfo` vtable `0x34`). O nó recebe um
  property set `"LotData"` com `"LotId"` (`AddLotImposter`, BV `0x76cd28`) —
  é por ele que o clique acha o lote (§6).
- Posição (`SetOrientationAndTranslationForImposterNode`, BV `0x769640`):
  giro em Z de −(lado da frente mod 4)·90°, transladado para o canto do
  lote de modo que a pegada cubra (x, y)…(x+w, y+d) **×10**; Z = elevação
  do lote (`LotInfo` vtable `0x40`) + `ImposterZOffset` (padrão 1,0, ini
  `Neighborhood.ini` 0,5). Lotes fora do terreno não entram.
- O terreno do bairro sob o lote é escondido (`HideOrShowTerrainUnderImposter`,
  BV `0x76723c`) e aplainado (`cTSNHoodTerrain::FlattenTerrainUnderLot`,
  BV `0x4d1b10`); `UpdateImposterFading` (BV `0x76aa40`) desbota imposters
  conforme a câmera (detalhe **aberto**).
- **Pedaços pretos em N002** (rodada 8): não é falha de material nem de
  textura. Os 13 TXTR de cada lote (`walls_0..8`, `roofs`, `terrain`,
  `terrainLmap`, `slices_0`) resolvem e decodificam (128x128 Raw32). As fatias
  e telhados que saem pretos têm texels escuros nos dados: `N002_Lot4`
  `slices_0` é cinza liso (20,20,20) nas 1.842 células opacas, o `roofs` de
  vários lotes tem média (23..48), igual ao `N001_Lot11` (39,38,32) que sai
  escuro também; a foto do lote 4 (`IMG 0x35CA0000`) mostra mesmo máquinas
  e telhados pretos. Se o jogo clareia as fatias (fator do shader do
  material de fatia, só no binário), isso falta.
- Ao salvar o lote o jogo regenera o imposter (`cLotImposterManager::
  GenerateLotImposter`, BV `0x77ab40`; `SaveLotImposterInLot`, BV
  `0x776570`). **Inferido** pelo fluxo de mensagens: `ExitLotBegin` marca
  `0xCC181346`, que também aparece em `cTSSaveLotController::SaveLot` e em
  `cLotImposterManager::UpdateLotBounds`.
- Dentro de um lote, `nTSSG::cLotSkirt` desenha o terreno do bairro em volta
  (células de lote, rua e mar; `lotskirt.matShad`) e os imposters vizinhos
  com Z relativo ao lote atual. Fora do escopo agora.

---

## 5. Câmera (`nTSSG::cCameraNeighborhood`, sobre `cTSCameraSmoothSims1Controller`)

Config `TSData/Res/UserData/Cameras/NeighborhoodCamera.txt` (dados; o mesmo
parser de `src/formats/camera_config.*`): `zoom 1 (650 450 230)`, `fov 33`,
`clip 10..2000`, `pitch (40 32 25)`, 8 orientações de 45°, `smoothing 5 8`,
`wheel 5`, `constrain .35 .35 .35 .35`, teclado (`keyboardtranslationspeed
50`…). A posição do olho segue a mesma fórmula da câmera Live
(`docs/camera-ui-mac.md` §1.1). O que muda (binário):

- Ao entrar no bairro (mensagem `0xB6`, `DoMessage`, BV `0x765134`): W =
  tilesX·10, H = tilesY·10, Z = valor do terreno (vtable `0x3c`); se a
  câmera não tinha posição, o pivô vai para **(W/2, H/2, Z/2)**.
- **Limites do pivô** (`SetBoundedCOIPosition`, BV `0x765768`): x preso a
  [c0·W, W − c2·W], y a [c1·H, H − c3·H] com os 4 valores de `constrain`
  (sem eles, [0, W]×[0, H]); Z fixo. Com 0,35: x, y ∈ [448, 832].
- **Não entrar no chão** (`UpdateSubjectMotion`, BV `0x765dd4`): com
  movimento, tenta pivô + movimento e recua o passo (÷5) até 5 vezes
  enquanto o olho ficaria sob o terreno; o teste (`IsUnderTerrain`, BV
  `0x7659b0`) pega a maior altura dos vértices em volta do olho para o nível
  de zoom. `ConstrainDistanceAlongCurve` (BV `0x765640`, nomeado errado
  `cLotImposter::~cLotImposter` no Ghidra) desce o zoom até o olho ficar
  acima do terreno.
- O ouvinte de som chama `"CameraManNH"` no bairro (`"CameraMan"` no lote).

---

## 6. Clicar num lote e entrar/sair

- **Escolha** (`cTSNHoodUserToolBase::PickLot`, BV `0xa25764`): raio da
  câmera pelo ponto do mouse (viewer vtable `0x54`), interseção com o scene
  graph (vtable `0x16c`), e sobe pelos pais do nó atingido até achar o
  property set `"LotData"` com `"LotId"` ≥ 0 → `LotInfo` pelo ID. Não usa
  a pegada no chão. O chão sob o mouse (`GroundAtGameWindowPoint`, BV
  `0xa254d4`) é outra interseção, só com o terreno; tile = ⌊pos/10⌋
  (`ConvertToTileCoordinates`).
- **Destaque** ao passar o mouse: `HighlightLot` (BV `0xa251c8`) cria um
  destaque do terreno no retângulo do lote (`AddTerrainHighlight`), ou o
  contorno do imposter (`CreateLotImposterHighlightGeometry`); materiais
  `NHoodTerrainHighlightMaterial`/`OutlineHighlight` (camadas +1/+2).
- **Clique** (`cTSUserToolNeighborhoodMoveIn::OnMouseDown`, BV `0x3bb1f4`):
  sem lote → mensagem de erro se o modo exige um; lote não habitável no modo
  → texto de erro (`NormalNeighborhoodErrorText`); senão `SetSelectedLot` e,
  se pode entrar (`OKToEnterInCurrentMode`), posta a mensagem
  **`0x2BEA2063`** (pedido de transição; tratada por
  `cTSGameStateController::DoMessage`) com o ID do lote.
- **Entrar** (`cTSGameStateController`): `TransitionToLot` (BV `0x14a384`)
  → `ExitNeighborhood` (BV `0x14486c`) → `EnterLotBegin` (BV `0x145f0c`)
  → carga do lote → `EnterLotEnd` (BV `0x144b48`). Tudo por mensagens
  encadeadas; o carregamento em si é o que o app já faz com `--lot`.
- **Sair**: `ExitLotBegin` (BV `0x145814`) posta `0x2D0D20AD` (salvar,
  `cTSSaveLotController`; ver `docs/save-mac.md`) encadeada com
  `0x8D03C06D` (→ `ExitLotEnd`, BV `0x144dec`), e então
  `TransitionToNeighborhood` (BV `0x14b6ac`)/`EnterNeighborhood` (BV
  `0x146850`) recriam a vista (imposters do zero, `ConstructLotImposters`,
  BV `0x76d340`). Com a flag `0x800` sai sem salvar (`0xACFD5984`).

**No app** (rodada 8): o valor do lote do popup e o patrimônio do diálogo
vêm do `SIMI` do pacote do lote (`docs/save-mac.md` §5: `+0xA4` valor do lote,
`+0xAC` valor dos objetos da família, `+0xA8` dos do lote). Lote vazio mostra
o valor do lote mais o dos objetos; família, os fundos (`FAMI`) mais o valor do
lote e dos objetos da família (**inferido**: "Patrimônio"). Sair do lote:
o painel de opções do puck (script `0x49060001`, botão `0xA210`, "Vizinhança";
`0xA220` Sair; `0xA230` Salvar não ligado) além do Esc; só aparece quando o
lote veio do bairro. O puck do bairro: zoom `0x2BE6FB48`/`0x2BE6FB4E`,
rotação `0x2BE6FB52`/`0x2BE6FB57`.

---

## 7. Base vs BV

As strings de todo o caminho acima existem no Mac 1.0 base
(`PaintMapTable_`, `TextureVariantGridSize`, `ImposterZOffset`,
`%04x-bridge`, `neighborhood-roads-%s-%08x`, `LotData`, `CameraManNH`,
`MaxRoadProximityValue`, `NeighborhoodTerrainPaint`, `nhoodLightMap`,
`nhoodWaterMaterial`). **Só BV**: estações (`WeatherLight`,
`nTSWeather::ExtractSeasonPostfix` no imposter, neve no terreno
`cTerrainSnowGeometryNode`), negócios/férias/sub-bairros. A pintura manual
do terreno do bairro e os enfeites colocados pelo jogador são ferramentas
que um bairro base não precisa (os 4 bairros não têm pintura salva).

## 8. Para implementar (ordem sugerida)

1. Ler `0xABCB5DA4` e `0xABD0DC63` (layouts §1, conferidos em N001).
2. Malha do terreno 129×129 a 10 unidades; pintura pelo ini (§2.3) em
   mapas alfa por pintura; somar texturas como §2.4; multiplicar por uma
   luz por vértice (aproximação até achar a geração do `nhoodLightMap`).
3. Água no nível gravado; ruas como quads com o material da peça.
4. Flora/enfeites pelos CRES do `catcanhobjects`; imposters dos lotes.
5. Câmera com os limites §5 e escolha de lote por raio contra os imposters.

## 9. Aberto

- Como o `nhoodLightMap` é gerado (classe e resolução).
- Preenchimento do mapa alfa (peso → alfa) e a semente das variantes.
- `0xEC44BDDC` (bairro e lote) e `0xCC2A6A34` (14 bytes, `cTSNeighborManager`).
- Desbotamento dos imposters pela câmera.
- Significado das peças de rua (nomes SC4) e dos 2 bytes `+0x85/+0x86`.
