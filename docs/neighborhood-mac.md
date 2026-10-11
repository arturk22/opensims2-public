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
  foi lido: **inferido** peso/256 → alfa). Rodada 8: o binário não
  converte: `NormalizePaintWeights` (BV `0x4d3478`) grava em cada vértice,
  no mapa de 8 bits de cada pintura (`SetPaintValue`, BV `0x4d33e4`;
  `GetPaintAlphaMap`, BV `0x4d31e0`), o byte baixo de
  `peso · 250 / Σ pesos` (até 3 pinturas por vértice; a soma dá ≈ 250), e o
  `cNHoodTerrainPaintAlphaTextureUpdateStrategy::UpdateTopMipSurface` (BV
  `0x6dc1c8`) copia esse mapa direto (8 bits → 8 bits, só o retângulo sujo)
  para a textura alfa. Alfa = esse byte / 255.
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
  map. Rodada 8: a geração é o
  `cLightingManager::CreateNeighborhoodLightMap` (BV `0x706920`, casado com o
  `baserom.sym`): duas texturas TXTR (`0x1C4A276C`) com lado = a potência de 2
  ≥ (vértices − 2); para cada vértice lê a altura e a normal do terreno; uma
  segunda grade de alturas sobe os retângulos dos lotes ("blobs":
  altura × `+0x284`, alargados por `+0x288`, máximo com o terreno); onde a
  altura da sombra do sol (grade `+0x228`, do `UpdateHeightMapShadows`) passa
  da do vértice, a normal é desviada pela diferença (`FUN_01809070`);
  depois o texel = **mapa de luz do estado de luz amostrado em
  ((n.x·0,5 + 0,5)·L, (n.y·0,5 + 0,5)·A)** (o mesmo `lightMap` de
  `docs/lighting-mac.md` §5), vezes a cor do estado (`+0xd8`), arredondado e
  preso em 255, alfa 255. A primeira textura usa só o terreno e a segunda
  também os lotes. A intensidade da sombra é `hmNHoodShadowStrength`.
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

### 6.1 Famílias, mudar para um lote, despejar e preço dos lotes (rodada 9, binário)

- **Caixa de famílias** (`cTSWinProcFamilyCatalog`): as famílias "sem casa"
  de `cTSFamilyManager::GetHomelessFamilyIDs` (BV `0x4fbfe0`): lote da
  família (vtable `0xa8`) < 1 e ID entre 1 e 32728 (`< 0x7FD9`; as de cima
  são as reservadas de NPCs/townies).
- **Família nova** (`MakeNewFamily`, `0x4ffd28`): o menor ID livre a partir
  de 1, índice de criação = maior + 1 (a caixa ordena por ele), **fundos =
  "money for new family" = 20000** (`cTSNeighborhoodConstants`, constantes
  float do bairro com padrões no código; a string existe no base 1.0) e
  mensagem `0xA5`. Outras constantes do mesmo bloco (base): "new family
  start hour" 7, "perfect family friend count" 15, "weight of lot size in
  overall score" 3, "yard score multiplier" 0,33, "upkeep penalty per dirty
  object" 6 e "... per broken object" 15 (a pontuação do lote; não seguido).
- **Preço de um lote** (`cTSNeighborhood::GetLotFileInfo`, `0x4d8d8c`, vtable
  `0x5c`): lê o `SIMI` do pacote do lote; preço = **+0xA4 (valor do lote) +
  +0xA8 (objetos do lote) + round(+0xB0 × fator)** (+0xB0 e o fator, vtable
  `0x13c` de um serviço, não identificados)
  **+ +0xAC (objetos da família) se o lote não tem a flag 8** ("família
  despejada", abaixo). Também devolve "tem casa" = global 22 do SIMI ("Lot
  Has House") ≠ 0 e "pode mudar" = global 21 ("Inhibit Move In") = 0. Lote
  sem SIMI (nunca salvo): `nTSWorld::LotValue(L, P)` (`0x1635f4`) com L, P =
  tamanho em tiles de lote (tiles do bairro × 10): **100 · round(6,5 · L · P
  / 100) − 1000** (40×30 → 6800). No BV, aluguel e negócios mudam isso.
- **Pode mudar?** (`cTSFamilyManager::CanMoveFamilyIntoLot`, `0x4fb804`):
  sem bairro −1, sem lote 2, sem família 1; tipo do lote (`LotInfo` vtable
  `0x10`→`0x20`): **1 (comunitário) → 3** ("Families cannot move into
  community lots.", STR# 0x82 #3 do `Neighborhood.package`), tipos do BV 3/5/6
  → 4/10/2; lote vazio: família sem a flag 1 → 9; **fundos < preço → 7**
  ("The %s family cannot afford this lot.", #2) salvo em lote de tipo 2;
  lote ocupado → 5/6/8/9 conforme a relação entre as famílias (juntar).
  Os textos são o índice − 1 do STR# 0x82 (ou 0xC9 nos modos do BV).
- **Confirmar** (`cTSUserToolNeighborhoodMoveIn::ConfirmAndMoveInFamily`,
  `0x3b9ae8`): diálogo "Move %s family into this lot?" (#5) + "Cost to move
  in: " (#6) com o preço; Sim → mensagem `0x2BEA2063` (transição para o lote)
  com a família e o lote. **A cobrança é na carga** do lote
  (`cTSLoadLotController::LoadLot`, passo `0x1B`, `0x8804ec`): mudando
  (`+0x34` e não `+0x35`), fundos −= preço de `GetLotFileInfo` (salvo lote
  de tipo 2); depois global 9 = família, fundos para o simulador, e no
  primeiro dia `cTSNeighborhood::SetupFamilyFirstDay` (vtable `0x44`).
  `cTSFamilyManager::MoveIn` (`0x4fb730`) só liga família e lote.
- **Despejar** (`cTSGameStateController::EvictFamily`, `0x139740`/`0x1434cc`;
  ferramenta `cTSUserToolEvictFamily`): **fundos += preço do lote**
  (`GetLotFileInfo`, ainda sem a flag 8, então com os objetos da família),
  salvo lotes de tipo 2/3; a família fica sem lote, o lote ganha a **flag 8**
  e perde a família (tipo 3: o bairro limpa as referências); mensagens
  `0xA7` e `0xA6`. Ao entrar de novo num lote com a flag 8
  (`CleanupThingsRelatedToEvictedFamily`, `0x87e7b0`) o jogo apaga os Sims e
  os objetos da família que ficaram (pessoas, categoria `0x67`, produtos com
  o bit 0 da propriedade `0x2A`; inferido pelos testes).

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

---

## 10. Famílias e lotes no app, e o que a rodada 9 acrescentou ao §6.1

As regras estão no §6.1. Acréscimos lidos no binário nesta frente:

- **Fator da arquitetura**: o `+0xB0` do `SIMI` é o valor da arquitetura
  (`cTSSimulator::GetArchValue`; a ordem dos thunks do `baserom.sym` dá os
  nomes: `GetTotalObjectsValue`, `GetArchValue`, `SetArchValue`,
  `GetLotValue`, ...), e o serviço da vtable `0x13c` é o catálogo
  (`FUN_000c87e8` também serve `cTSNHoodOccupant::GetProduct`):
  `cTSCatalog::RefundRate` (BV `0xb2a66c`) devolve o float **0,8**. A conta é
  `(int)(0,8 × (float)+0xB0)` (truncada). Getters do simulador por vtable
  (interface): `0x84` → `+0xA8`, `0x88` → `+0xAC`, `0x8c` → soma, `0x90` →
  `+0xB0`, `0x98` → `+0xA4`.
- **Valor de lote sem pacote** (dados): o `+0xA4` dos 63 lotes do jogo é
  ⌊6,5 · L · P / 100⌋ · 100 − 1000 com L × P **sem a faixa da rua** (a borda
  do lot info: bits 1/4 tiram uma coluna, 2/8 uma linha) e arredondando para
  baixo (30 × 50 → 8700, 30 × 30 → 4800, 50 × 50 → 15200); o app usa isso.
- **Flags do FAMI** (`+0x60`): `SetNewLot` liga o bit `0x04` (vtable
  `0x120`; `0x124` o lê), `0x11c` lê o bit `0x02`. `SetLotID` (vtable `0xac`)
  grava `+0x30`.
- **Primeiro dia** (`SetupFamilyFirstDay`, BV `0x4db0c0`): `SetNewLot(0)`,
  `SetCurrentTime(hora de início, 0, −1…)` ("new family start hour", 7),
  `ClearHistory` e `cTSSimulator::Resume(true)` (BV `0x49bd48`), que zera o
  global 17 "Paused" (os lotes à venda guardam 1).
- **Pertences no despejo**: `cTSObject::IsDeletedByEvict` (BV `0xafeb84`):
  vtable `0x184` = 2, ou vtable `0x1c8` = 0 sem a flag `0x10` de `+0xF0`.

**No app** (`formats/hood_moves.*`, `formats/lot_arrivals.*`,
`app/hood_bins.*`; `s2pkg hood-lots`, `hood-move-in`, `hood-move-out`,
`hood-bulldoze`): mudar grava no FAMI o lote e a flag `0x04`; ao carregar o
lote, os membros da família que não estão nele chegam (um estado PERS novo
por Sim, ao lado da caixa de correio: **inferido**), o preço sai dos fundos e
o jogo despausa; salvar grava os Sims (sem `cPerson`: o lote os acha pelo
GUID nos arquivos de personagem) e os fundos. O despejo devolve o preço e
limpa o pacote do lote na hora (os Sims da família e o que carregam; o valor
dos objetos da família passa para os do lote, **inferido**: o jogo recalcula
ao carregar) em vez da flag 8. Demolir: só lote vazio (STR# 0x83 #26/#29), sai
o lot info, o nome e o pacote. A hora de início do primeiro dia não é aplicada.

---

## 11. Valores do lote, despejo e o primeiro dia (rodada 10, binário + dados)

### 11.1 Quem refaz os globais 46–49 e o SIMI +0xA4…+0xB0

`cTSLot::ComputeAndStoreLotData` (BV `0x46fbd4`, vtable `0x2c` do `cTSLot`;
o nome vem da ordem do `baserom.sym`) faz tudo de uma vez:

```
cEdithObjectModule::ComputeStats(&família, &lote, &b1, &b2, &b3, &temCasa)   (BV 0x52c638)
arq  = nTSWorld::ArchitectureValue()                                          (BV 0x168a74)
temCasa |= FUN_001646b8()            (algum tile do lote com piso/parede construída)
lot info (se o lote não é ...): vtables 0x108/0x118/0x110 ← b1, b2, b3   (estatísticas do lote)
simulador: SetFamilyObjectsValue(família)  → +0xAC, globais 48/49  (UpdateTotalObjectsValue)
           SetLotObjectsValue(lote)        → +0xA8, globais 48/49
           SetArchValue(arq)               → +0xB0
           SetLotValue(nTSWorld::LotValue(L, P)) → +0xA4, globais 46/47
           SetGlobal(23 "Lot Size", área do mundo, vtable 0x11c do WorldDB)
           SetGlobal(22 "Lot Has House", temCasa)
```

(`L, P` = largura e profundidade do retângulo do lote × `FLOAT_0274b608`,
o `100 · round(6,5 · L · P / 100) − 1000` do §6.1.) `UpdateTotalObjectsValue`
(BV `0x498418`): soma = +0xA8 + +0xAC; global 48 = soma / 1000, 49 = soma −
1000 · (soma / 1000).

**Quando**: `cTSLot::DoMessage` (BV `0x4700c8`) chama a vtable `0x2c` nas
mensagens **`0x89`** (virada do dia, a mesma do relógio em
`docs/primitives-mac.md` §4c), **`0x8B`** e **`0xCD94C8EE`**. A `0x8B` também é
assinada pelo módulo de objetos, telhados, cercas, roteamento, pontuação de
ambiente e a janela de valor do lote sem Sims (`cTSWinProcSimlessLotValueUI`)
— é a mensagem de "o lote mudou" (quem a envia não foi achado: não há
`li r4,0x8b` fora dos assinantes). Ou seja: **todo dia e a cada mudança do
lote**, não a cada compra pela primitiva.

`ComputeStats` (por objeto do módulo; pula os com bit `0x10000`/`0x20000` em
`+0x214`, os com o bit `0x400` da propriedade de catálogo `0x28`, as partes
não-líderes de multi-tile, tipo 7 e os fora do mundo):

| objeto | vai para |
|---|---|
| `IsDeletedByEvict` (§11.2) | **família** (+0xAC) |
| senão, `GetBuildModeType()` = 0 (objeto do modo compra) | **lote** (+0xA8) |
| senão, tipo (OBJD campo 9) 0xB ou 0xC | lote, com o preço do corrimão (`RailingCost`: 0, 1 ou 2 × o preço do objeto `0x2C7BF690` conforme o modelo do corrimão) |
| outros objetos do modo construção | nada (entram na arquitetura) |

O valor é `GetCurrentValue` (dado 41). Pessoas: só ligam um dos booleanos
(dado de pessoa `0x4c` = 0xD). `temCasa` liga para um objeto de catálogo no
mundo sobre algo construído (vtable `0xc4` do mundo). O opensims2 lê os
globais salvos e não os refaz.

### 11.2 `IsDeletedByEvict`, a flag 8 e a limpeza

`cTSObject::IsDeletedByEvict` (BV `0xafeb84`) = **pessoa** (tipo 2) **ou
objeto do modo compra** (`GetBuildModeType` = 0, o campo `0x45` do catálogo)
**sem o bit 4 de "movement flags"** (dado 43, `+0xF0`; STR# 0xCB #4 **"stays
after evict"**). Portanto o despejo leva **toda a mobília comprada** (é o
"vendido por §" do STR# 0x83 #18), menos o que tem "stays after evict"; a
construção (paredes, portas, janelas, escadas…) fica. Isso fecha o preço do
§6.1: o lote com a flag 8 vale +0xA4 + +0xA8 + arquitetura, sem os objetos
da família.

`cTSLoadLotController::CleanupThingsRelatedToEvictedFamily` (BV `0x87e7b0`,
chamado no `LoadLot`): só se o lot info tem a **flag 8**; manda a mensagem
`0xDD`; para cada objeto sem contêiner (`GetContainer` = 0) ou pessoa,
apaga (módulo vtable `0x48(id, 1)`) se for **pessoa ou categoria `0x67`**,
**produto com o bit 0 da propriedade `0x2A`**, ou `IsDeletedByEvict` num lote
residencial (para o tipo 0 só se o bairro (`+0x8c`→`0x18`→`0x48`) ≠ 2; os
tipos 2/3 do BV não apagam por `IsDeletedByEvict`); pula o GUID `0xF37E5AE0` e
as partes não-líderes. No fim, vtables `0x54` e `0x20(1)` do módulo e `0xa8`
(limpa a flag, inferido). O módulo de objetos também tem um despachante de
eventos de lote (vtable `0xd8`, `FUN_00524490`) que roda a árvore por nome
"import lot" (0), **"evict lot"** (1) ou "msg show special" (2) em todos os
objetos.

**Contradição com o código**: `formats/hood_moves.cpp` apaga só os Sims da
família e o que eles carregam e passa o +0xAC para o +0xA8; o jogo apaga
também toda a mobília comprada sem "stays after evict" e recalcula os
valores na carga (§11.1).

### 11.3 O primeiro dia e os Sims que chegam

`cTSNeighborhood::SetupFamilyFirstDay` (BV `0x4db0c0`): `SetNewLot(0)`;
`SetCurrentTime(hora, 0, −1, −1, −1, −1)` com a hora = "new family start
hour" (constante do bairro, **7**; −1 = campo não muda: só a hora e o minuto
0 mudam, o dia fica); `ClearHistory`; `Resume(true)`; e **roda a árvore por
nome "reset lot" em todos os objetos** (vtable `0xec` do
`cIEdithObjectModule`).

No "Controller - Age" (`0x7F07FBBC`), "reset lot" (`0x100C`) → "Sub - Set
Age - All" (`0x1019`) → **"Sub - Generate Family"** (`0x101A`), e depois
"Preg - Check For Pregnant Sims". O Generate Family é quem cria os Sims que
chegam (nada no C++):

1. sai se o lote é o CAS ("Lot - Is CAS?") ou não é residencial;
2. referência = a **caixa de correio** (Set to Next GUID `0x39CCF441`) ou, sem
   ela, a **lixeira de fora** (`0xD20C0AEE`);
3. para cada vizinho da família (dado 61 = família do lote) que não está
   morto ("Death - Is Sim Dead? (NID)") e ainda não tem objeto no lote:
   **Create New Object Instance** com flag 0x04 (Sim a partir do vizinho no
   stack object, posição 6), então **Find Location For** modo 4 (ao lado da
   referência, flags 0x09: começa no objeto do local, objetos explícitos),
   senão modo 0 duas vezes (mesmo cômodo, depois qualquer lugar); achou →
   "Selectable: Set"; não achou → "Selectable: Unselect Sim" e Remove Object
   Instance.

O `cPerson` desses Sims é o que o Create New Object Instance com vizinho
cria (o estado vem do personagem e do vizinho, como um Sim novo); a
"Function - Main" dele então chama "Init - Relationships" (`docs/cas-mac.md`
§17.5). O opensims2 (`formats/lot_arrivals`) põe os Sims ao lado da caixa
de correio por inferência: **é isso**, mas pela árvore e só no primeiro dia,
e a hora de início (7:00) ainda não é aplicada.

### 11.4 Pôr, mover e girar lotes (`cTSUserToolPlaceLotBase` e filhas)

Ferramentas: `cTSUserToolPlaceNewLot` (lote vazio, retângulo arrastado),
`cTSUserToolPlaceLotFromInventory` (da caixa de lotes, `cTSWinProcLotCatalog`),
`cTSUserToolMoveLot` (mover/girar um lote do bairro) e
`cTSUserToolMoveLotToCatalog` (mandar para a caixa: o lote sob o cursor
ganha o material de realce `GetLotHighlightMaterial`; o clique faz a troca,
não decodificada). Retângulos em tiles do bairro; ×10 (`FLOAT_0274b608`)
para unidades do mundo.

**Validade comum** (`IsOKToPlaceLotCommon`, BV `0xa29e5c`), falha se:
1. um canto do retângulo não é válido (vtable `0x58`);
2. **outro lote** cobre o retângulo encolhido de 1 tile (`IntersectsOtherLots`,
   BV `0xa29cd8`), ou algum tile do retângulo é de outro lote sem ocupante de
   rua (tipo 2) nele;
3. há **ocupantes bloqueantes** (consulta tipo `0xF`: flora, enfeites…) cujo
   volume cruza o retângulo e que não são removíveis (vtable `0x34` = 0)
   (`AreBlockingObjectsPresent`, BV `0xa271cc`);
4. lote que não é de praia (flag `0x80` do lot info) com algum tile de **água**
   (terreno vtable `0x90`);
5. não há rua (ocupantes tipo 2) encostada, ou o **declive** passa do limite
   (`IsLotSlopeMoreThanTheLimit`, BV `0xa26720`: entre o vértice mais baixo e
   o mais alto do retângulo, Δaltura / (distância em tiles × 10) > **0,5**);
6. as ruas que tocam o lote: um pedaço de rua que cruza a borda só vale se
   tiver pelo menos 2 conexões; as bordas com rua são marcadas;
7. vtable `0x9c(ret, 1)` (acesso pela rua) falha, ou, para praia,
   `IsOKToPlaceBeachLot`.

**Frente e giro**: o lot info guarda por borda (0–3) se ela tem rua
(vtable `0x38`); a ferramenta guarda as bordas com rua no lugar
(`this+0x68…0x6b`). Mover ou pôr da caixa exige, para cada borda i,
`lote.borda(i) == lugar[(i + giro − giro_do_lote + 4) mod 4]` (a rua tem de
ficar nas mesmas bordas, giradas); depois
`TryModifyingTerrainToConformToLot` (BV `0xa2a918`) tenta pôr o terreno do
bairro nas alturas dos vértices do lote (giradas, + a altura da rua da
borda da frente): um vértice fora da faixa da rua cuja altura muda mais de
**0,01** não pode ser de outro lote nem estar sob uma rua que não acompanha;
senão falha. O giro do mover (`RotateLot`, BV `0x222b2c`) segue a borda
mais próxima do cursor, muda o cursor (0x1C/0x1E/0x1F/0x1D para frente
0/1/2/3), reavalia e gira o imposter (`RotateLotImposter`).

**Lote vazio** (`cTSUserToolPlaceNewLot::PlaceLot`, BV `0x2c7344`): marca as
bordas com rua no lot info; a **frente é a primeira borda com rua**
(0, 1, 2, 3); calcula a altura da rua da frente (`FUN_00a265cc`) e nivela as
ruas encostadas (`FlattenAbuttingRoadsIfPossible`); lote de praia
(`this+0x116`) ganha a flag `0x80`; `SmoothenRoadsConnectedToLot`; **suaviza o
terreno sob o lote** (terreno vtable `0x88` com o ângulo de talude 2,0;
passadas 8, do ini `[SmoothingUnderNewLot] SmoothingTalusAngle` /
`NumberOfSmoothingPasses`); grava tamanho (L, P), zona (residencial/
comunitário), nome, posição; pinta a saia (`SetLotSkirtPaint`) e entra no
bairro (vtable `0xf4`).

**Da caixa** (`PlaceLotInternal`, BV `0x3fe320`): tira o lote provisório,
cria o lot info pelo catálogo (`+0x10c` vtable `0x1c`), marca-o, nomeia,
posiciona, ajusta as ruas, grava a frente, tipos 4/6 (BV) ganham a flag
`0x10` e a mensagem `0xEC06FDD1`; entra no bairro, conforma o terreno (agora
para valer), pinta a saia, e liga a flag `0x100`.

**Terreno sob o imposter** (`cLotImposterDisplay::HideOrShowTerrainUnderImposter`,
BV `0x76723c`): para cada tile do retângulo do lote (do lot info, vtable
`0x34`, ou o dado), dentro do terreno, esconde/mostra os **dois triângulos**
do tile (terreno vtable `0x58(tile, mostrar, 1)`). `cTSNHoodTerrain::
FlattenTerrainUnderLot` (BV `0x4d1b10`), para lotes sem a flag `0x80`, nivela
(vtable `0x24`) cada vértice do retângulo que não é compartilhado com outro
lote nem fica sob uma rua.

## 12. No app (rodada 10, frente do bairro)

- **Primeiro dia** (`formats/lot_arrivals.*`): a família com a flag `0x04`
  do FAMI ("lote novo", posta pelo `MoveIn`) é a que está se mudando: paga o
  preço na carga (passo `0x1B`) e tem o primeiro dia (passo `0x1F`, também
  com a flag 4 do lot info): relógio às **7:00** (`SetCurrentTime(7, 0, −1…)`,
  BV `0x49af84`: só hora e minuto), globais 17 "Paused" = 0 e os relatórios
  de despesas zerados (`ClearHistory`; inferido que são esses). O save do
  bairro (`vm/hood_save.*`) tira a flag `0x04` da família do lote e grava o
  lot info do lote salvo com a flag 1 e sem as 4 e 8 (`SaveLot`, BV
  `0x884ec4`). `r10-bairro-primeiro-dia.jpg`: Subúrbio no lote 24 às 7:02,
  §4.280 depois de pagar §17.621.
- **`cPerson` dos Sims que chegam** (`vm/lot_records.cpp`): o save escreve um
  `cPerson` (cPerson 3 < cLocomotable 4 < cAnimatable 0x10 < cObject 0x11)
  feito do de outro Sim do mesmo esqueleto (pelo dado 58: ≥ 16 `auskel`,
  3–15 `cuskel`, 2 `puskel`): a lista de modelos do molde sem as escolhas de
  material (os nomes são de malhas e materiais compostos na hora pelo jogo:
  **inferido** que ele os refaz pelo personagem), a pose e o nome
  `<arquivo do personagem> - <nome>_objt` do Sim, e os dados de subclasse do
  molde com a pose trocada. O estado (PERS) do Sim que chegou é gravado como
  objeto novo (com as partes vazias que o jogo grava) e o seu tipo de save
  entra na tabela `objt` (montado pela definição do personagem). `s2pkg
  hood-save` traz os Sims que chegam como o app (`addArrivingSims` e a
  cobrança): mudar a família 8 para o lote 24 e `hood-save --check` dá a mesma
  VM depois de salvar e carregar, e uma segunda passada não muda o bairro.
  Ao entrar de novo o lote acha os Sims pelo `cPerson`
  (`r10-bairro-cperson-de-volta.jpg`, sem nova cobrança).
- **Despejo** (`formats/hood_moves.cpp`, `formats/lot_evict.*`): "Mudar
  Família" devolve o preço e põe a flag 8 no lot info, sem mexer no pacote do
  lote; o preço do lote passa a deixar de fora os objetos da família. Na
  próxima carga (`addArrivingSims`, que o app, o desenho dos objetos —
  `dropLeftoverCObjects` — e o `hood-save` chamam) a limpeza do §11.2 tira
  do módulo de objetos salvo, antes da VM criá-los, os Sims, os objetos de
  categoria `0x67`, os produtos com o bit 0 do campo `0x2A` do OBJD e o que
  `IsDeletedByEvict` pega (tipo 2, ou modo compra sem "stays after evict"),
  pulando o PlumbBob e as partes não-líderes (que vão com o líder), e o que
  está nos slots deles; o +0xAC vira 0 e os globais 48/49 seguem (§11.1: o
  que `IsDeletedByEvict` pega é tudo que conta como da família). O global 9
  passa a 0 quando ninguém mora no lote (`LoadLot`), e o save tira a flag 8.
  Malpaga fora do lote 36: 292 → 63 objetos, preço §10.743 (§1.600 + §2.770 +
  0,8 × §7.967), a casa vazia (`r10-bairro-despejo-limpo.jpg`); a família 0
  do pacote não conta mais como família do lote. Os Sims que chegam ficam ao
  lado da caixa de correio, senão da lixeira de fora (o "Sub - Generate
  Family" do §11.3; aqui em C++, não pela árvore).
