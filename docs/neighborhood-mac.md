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
- **Tom de sombra dos imposters** (rodada 10, `imposter.matShad` +
  Ghidra headless): `ImposterColorScalar()` põe `colorScalar (1,1,1,1)
  -applyShapeColor 0` e, se a cena tem uma luz `kShadeLight` (`user1`) e
  nenhuma `kShapeColorLight` (destaque), `tsUserLight shadeLight 0`: paredes,
  fatias, telhado e chão do imposter multiplicam a textura pela cor dessa
  luz. No bairro (modo de iluminação 2) `cLightingManager::AddOutdoorLights`
  (BV `0x70cd3c`) cria um `nTSSG::cTerrainShadeLight` (tipo 8; no caminho de
  luzes interpoladas, uma ambiente "LM_OutdoorShade" de tipo 7). A cor da
  luz não foi decodificada; pelo nome, tomada como a sombra do terreno sob o
  lote: o light map do bairro no centro do lote ÷ o do chão plano aberto,
  por canal, entre 0,4 e 1 (`hood::terrainShade`; só paredes, fatias e
  telhado, o chão é da frente do bairro). Nos três bairros nenhum lote cai
  numa sombra de morro (o terreno é aplainado sob eles), então o tom fica 1.
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
  start hour" 7 (padrão do código; o FCNS global 4 do `objects.package` diz
  **8,0**, rodada 12), "perfect family friend count" 15, "weight of lot size in
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
  `SetCurrentTime(hora de início, 0, −1…)` ("new family start hour": 7 no código, **8** no FCNS global 4),
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
— é a mensagem de "o lote mudou". **Quem a envia (rodada 11, binário)**:
o estado "modo vivo" do `cTSMainToolbar` (`SetMode(1)` cria o objeto de
estado em BV `0x159f400`, que posta `0x8B` e depois `0xCBCCDECE`): a cada
**volta ao modo vivo** (saindo de compra, construção, etc.; os modos do
`SetMode`: 1 vivo, 3 compra, 4 construção). Os outros `li rX,0x8b` do
binário são o código de erro de colocação `0x8B` ("Only exposed terrain")
ou assinaturas. Ou seja: **todo dia e ao voltar ao modo vivo**, não a cada
compra.

`ComputeStats` (por objeto do módulo; pula os com bit `0x10000`/`0x20000` em
`+0x214`, os com o bit `0x400` da propriedade de catálogo `0x28`, as partes
não-líderes de multi-tile, tipo 7 e os fora do mundo):

| objeto | vai para |
|---|---|
| `IsDeletedByEvict` (§11.2) | **família** (+0xAC) |
| senão, `GetBuildModeType()` = 0 (objeto do modo compra) | **lote** (+0xA8) |
| senão, tipo (OBJD campo 9) 0xB ou 0xC | lote, com o preço do corrimão (`RailingCost`: 0, 1 ou 2 × o preço do objeto `0x2C7BF690` conforme o modelo do corrimão) |
| outros objetos do modo construção | nada (entram na arquitetura) |

O valor é `GetCurrentValue` (dado 41). **`GetBuildModeType`** (BV `0xb00c60`)
lê o campo `0x45` da definição de catálogo do objeto, que num objeto
multi-tile é a do **mestre** (rodada 17): as partes de uma porta têm 0 e o
mestre "Door - 1 Tile - Value" tem 8, então as portas (com "stays after
evict") vão para o lote. Lendo a definição da parte, as portas ficavam de
fora e o preço do lote caía depois de salvar (§10.857 → §10.387 no "Ranch
Retreat"); com o mestre os objetos do lote saem iguais aos salvos pelo jogo
nos 12 lotes do catálogo. Sobra uma diferença de §150 nos objetos da família
em todos os lotes do jogo conferidos: os três CDs de videogame (`VideoGame -
CD - ...`, §50 cada, fora do mundo em (-1, -1), sem contêiner) entram no
valor que o jogo salvou e não no nosso; o `IsInWorld` (BV `0xaf627c`: flag
+0x7a e `cWorldDB` vtable `0xf8` da posição) não explica isso (não
conferido). Pessoas: só ligam um dos booleanos
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
senão falha. O giro do mover (`RotateLot`, BV `0x222b2c`) segue a direção
do arrasto a partir do ponto do clique (§15.1), muda o cursor (0x1C/0x1E/0x1F/0x1D para frente
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

**O arquivo do lote novo** (rodada 21, playtest 33: "lot 83 has no lot
file" ao entrar num lote recém-criado em N001). O jogo não grava arquivo no
`PlaceLot`: o `cTSLoadLotController::LoadLot` (§7.2 do
`game-flow-mac.md`, passos 0xB–0x11) abre o **modelo**
`TSData/Res/LotTemplates/%s%dx%d` (`LotTemplate`/`CommunityLotTemplate`,
`<através da rua × 10>x<ao longo × 10>`, girado como o lote) e o arquivo só
nasce no save. Os 10 modelos de cada tipo são exatamente os 10 tamanhos com
a faixa de rua (2×3 → 30x30 … 5×5 → 60x50) e todos têm a faixa (10 tiles
travados, 3ARY `0x03`), os portais de carro/pedestre, caixa de correio e
lixeira (orelhão no comunitário) na **borda 0** (x baixo), chão plano de
`lottexture-test-01` (o de todo lote do jogo e da saia do bairro) e o valor
do lote (SIMI +0xA4 = `nTSWorld::LotValue` do tamanho sem a rua). No
opensims2 o arquivo é escrito já ao pôr o lote (`hood::makeLotFile`, e na
entrada para lotes sem arquivo): cópia do modelo com os globais 10/41 e o
lot info base (`0x6C589723`) do lote. **Diferença de registro** (não de
comportamento): em vez de girar o conteúdo do modelo, o arquivo fica no
quadro do modelo e o lot info vira o de um lote criado de frente para a
borda 0 e girado até a sua frente (frente de criação 0, tamanho base
através×ao longo, bordas com rua nesse quadro), como os lotes do jogo
girados depois de criados (`GetCurrentSize`, `RotateLotImposter`).
**Inferido**: a faixa do lote vazio tem de dar para uma rua
(`hood::checkEmptyLot`; frente = borda da faixa), senão não há modelo que
sirva (o lote 83, 4×3 com a faixa em x baixo e a rua em y baixo, pediria um
30x40). O lote 83 da playtest, posto antes disso, recebe na entrada o modelo
do seu tamanho sem girar (faixa e portais na borda sem rua).

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
  com a flag 4 do lot info): relógio às **8:00** (rodada 12: `SetCurrentTime(hora, 0, −1…)` com a
  hora lida do FCNS `0x46434E53` grupo `0x7FD46CD0` instância 4, "new family
  start hour" = 8,0; antes o opensims2 usava o padrão 7 do código;
  BV `0x49af84`: só hora e minuto), globais 17 "Paused" = 0 e os relatórios
  de despesas zerados (`ClearHistory`; inferido que são esses). O save do
  bairro (`vm/hood_save.*`) tira a flag `0x04` da família do lote e grava o
  lot info do lote salvo com a flag 1 e sem as 4 e 8 (`SaveLot`, BV
  `0x884ec4`). O `LoadLot` não roda ticks antes de mostrar o lote: um lote
  salvo abre na hora salva (lote 68 às 07:08) e, na velocidade normal (um
  tick a cada 42 ms, 1 minuto do jogo = 1,26 s), 25 s depois está por volta
  das 07:26: não é erro (rodada 12; `s2pkg autonomy` e o app abrem na mesma
  hora, e 600 ticks levam 25 s no app). `r10-bairro-primeiro-dia.jpg`: Subúrbio no lote 24 às 7:02,
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
- **Imposters** (`app/hood_view.*`, `formats/hood_scene.*`): o giro é
  −(frente − frente de criação)·90° e o tamanho troca quando esse giro é
  ímpar (`SetOrientationAndTranslationForImposterNode`, BV `0x769640`;
  `cTSLotInfo::GetCurrentSize`, BV `0x48326c`); antes girava pela frente só,
  o que punha os lotes de frente de criação 1 de N002 (10, 22, 25) de lado,
  `r10-bairro-n002-giro.jpg`. O terreno do bairro some sob os lotes cujo chão
  do imposter não tem 4 vértices nem um por vértice do retângulo (o jogo só
  então cria o "imposter terrain", `CreateLotImposterTerrainGeometry`, BV
  `0x7686cc`): 10 lotes em N001, 7 em N002. No lugar entra o chão do imposter
  com as pinturas do terreno do bairro (as mesmas camadas e máscaras,
  recortadas; a máscara ganhou uma origem), `ImposterZOffset` abaixo do
  imposter (`UpdateImposterTerrainTransform`, BV `0x766a40`), e o chão do
  imposter vai por cima pré-multiplicado. **Inferido**: o retângulo escondido
  é o sem a faixa da rua (`GetBoundingRectExcludingRoads`), que o chão do
  imposter não cobre (com a faixa a água aparecia por baixo).
  `r10-bairro-terreno-sob-imposter.jpg`. **Desbotamento**: o
  `UpdateImposterFading` (BV `0x76aa40`) sai logo quando `+0x6d` está ligado,
  e o construtor o liga (vista do bairro); ele só roda com os vizinhos vistos
  de dentro de um lote (a saia do lote), com distância 2500 (`+0x78`,
  "neighborhoodPropFadeDistance"): alfa = (d / 2500)² perto da câmera. Na
  vista do bairro não há desbotamento.
- **Pôr, mover e mandar lotes para a caixa** (`formats/hood_lots.*`,
  `app/hood_bins.*`, `app/hood_mode.*`; §11.4): o painel Lotes & Casas liga
  a caixa (`0xC100` → painel `0xC000`, os lotes do `LotCatalog` do jogador
  com a foto IMG `0x35CA0000`, preço pelo `GetLotFileInfo`), os lotes vazios
  (`0xD100` → `0xD000`: botões `0xD001`–`0xD00A` = tamanhos 0–9 das tabelas
  do binário, `onBtnClickedID`), mover/girar (`0xA000`) e mandar para a
  caixa (`0xE000`). O retângulo do lote vazio é o tamanho mais a faixa da rua
  no lado do giro (`UpdateLotRect`); a validade é o `IsOKToPlaceLotCommon`
  (§11.4, itens 1–6: terreno, outros lotes fora das ruas, flora/enfeites que
  não podem ficar nem sair (`allowedinlot`/`removeonlotplop` do catálogo;
  **inferido** que a vtable `0x34` é isso), água, rua, declive > 0,5, ruas só
  nas bordas, uma borda inteira de rua, as pontas ligadas a ela); a frente do
  lote vazio é a primeira borda de rua; lote da caixa e lote movido exigem as
  ruas nas suas bordas giradas. Pôr ou mover tira a flora e os enfeites com
  `removeonlotplop` sobre o lote (`FUN_00a284cc`; o recurso de ocupantes é
  regravado cortando os registros). O lote vazio ganha um lot info (versão
  13, base 6) sem pacote (preço = valor do mundo); o da caixa, o pacote
  copiado como `<P>_Lot<id>` (id = `AcquireNextAvailableLotID`) e o lot info
  da base; o movido só muda posição, frente e altura. Mandar para a caixa
  (só casa sem família; `#26` para lote vazio) copia o pacote como
  `cx_<próximo>` com a base `0x6C589723` (nomes de 7 bits, ao contrário do
  lot info do bairro) e tira o lote do bairro. Depois de cada mudança a vista
  do bairro é refeita (troca de cena). Conferência: `s2pkg hood-lot-check`
  passa 58 dos 63 lotes do jogo onde estão (os outros 5: uma árvore de
  floresta 0,4 unidade dentro, uma rua de lado), `r10-bairro-caixa-de-lotes.jpg`,
  `r10-bairro-lote-vazio-vermelho.jpg`, `r10-bairro-lote-colocado.jpg`.
  **Não feito**: conformar o terreno ao lote e alisá-lo (a vista aplaina sob
  os lotes ao carregar), girar com as teclas (a ferramenta escolhe o primeiro
  giro que cabe: conveniência), a caixa de nome/zona do lote novo (fica
  residencial com o nome da base), lotes de praia e mandar lote ocupado com a
  família para a caixa.

## 13. Rodada 11: conformar, alisar, girar e a conferência dos 63 lotes (binário)

- **Ocupantes no caminho** (`AreBlockingObjectsPresent`): o
  `cTSNHoodOccupantManager::GetOccupants(rect, tipos)` (BV `0x554794`) só
  devolve o ocupante cujo **retângulo de tiles** (vtable `0x20`, BV
  `0xb1ae28`) cruza o do lote estritamente; esse retângulo é o das bordas
  ÷ 10 com folga: o canto baixo é ⌊v⌋ salvo se sobra mais de 0,93 do tile
  (`FLOAT_0274bbf4`), o alto ⌈v⌉ salvo se falta mais de 0,93. Depois o teste
  de volume (vtable `0x24`, BV `0xb1a11c`) é **inclusivo** e o bloqueio é
  `IsAllowedInLot` (vtable `0x34`: o `allowedinlot` do produto; o
  `removeonlotplop` não entra; inferido que o getter `0xd0` do produto é
  esse campo). O opensims2 olhava só o volume: os lotes 32 de N001, 50 e 192
  de N003 falhavam por galhos de 0,4–2 unidades dentro.
- **Ruas do lote**: cada rua que `GetOccupants(rect, 2)` devolve, recortada
  ao lote, tem de tocar o anel de tiles da borda e ter **duas conexões ou
  mais** nos seus bits de conexão (vtable `0x88` = bit i do byte `+0x86`,
  o segundo `u8` do registro), senão o lote **não pode** ficar ali (o
  opensims2 contava ruas vizinhas e pulava a rua). Bordas marcadas como
  antes.
- **Altura da rua por borda** (`this+0x6c`): o chão sob a rua da borda; as
  ruas ficam 0,4 acima do terreno (`FlattenToTerrain`, `FLOAT_0274b4b0`), e
  as elevações dos 63 lotes são a média dos cantos − 0,4 (múltiplos de
  0,1875; o lote 22 de N002 difere 0,09).
- **Conferência** (`s2pkg hood-lot-check`, agora com a conformação de
  terreno em ensaio): **61 de 63** passam onde estão. Os 2 que falham
  falham também pelas regras do binário (dados da Maxis, não nossa): o lote
  3 de N002 tem a torre de rádio (`allowedinlot=0`) 2 unidades dentro e o
  lote 4 de N002 tem um fim de rua (peça `0x300`, 1 conexão) na borda.
- **Conformar** (`TryModifyingTerrainToConformToLot`, BV `0xa2a918`, chamado
  em ensaio pelo `IsOKToPlaceLot` do mover e da caixa e para valer ao pôr):
  a faixa de rua do lote é a dos vértices u ∈ [0,1] (frente de criação 0),
  v ∈ [P−1,P] (1), u ∈ [L−1,L] (2) ou v ∈ [0,1] (3); giro g = (frente −
  frente de criação) mod 4 leva (u, v) a (u, v), (v, L−u), (L−u, P−v) ou
  (P−v, u); altura = altura do vértice do lote + altura da rua da borda da
  frente (0 sem rua). Cada vértice entra num `hash_map` (`FUN_0197c6b4`: um
  segundo valor diferente falha). Fora da faixa, se muda mais de **0,01**:
  falha se um lote cruza o retângulo 3×3 do vértice e não é o próprio; as
  ruas em volta põem os vértices que mantêm nivelados
  (`GetSameHeightVertices`, BV `0xb1ac10`: cantos (x0,y0), (x0,y1), (x1,y1),
  (x1,y0); cruzamento ou curva — conexões nos dois eixos — os outros 3;
  reta só nos bits 0/2 o canto `{1,0,3,2}[i]`, só 1/3 o `{3,2,1,0}[i]`) na
  mesma altura. Falha também se as alturas do lote não são uma por vértice.
- **Alisar** (`cTSNHoodTerrain::Smoothen`, BV `0x1689538`, vtable `0x88` da
  interface do terreno): para cada passada, cada vértice do retângulo e cada
  um dos 8 vizinhos dentro dele (dx `{1,1,0,−1,−1,−1,0,1}`, dy
  `{0,−1,−1,−1,0,1,1,1}`), excesso = h − hv − tan(talude) × 10 (× 1,414 nos
  diagonais); se > 0, cada lado não travado anda 0,2 × excesso para o outro.
  Travado (`FUN_004d359c`): ocupante de qualquer tipo ou lote no retângulo
  3×3 do vértice. Depois `FUN_00a28af0` arredonda cada vértice do retângulo
  para múltiplo de **0,1875**.
- **Girar** (`KeyCommand` das três ferramentas): a mensagem `0xACB3A629`
  (`,`/`.`) soma +1 ou −1 ao giro (lote vazio, `SetRotation`), à frente
  (caixa, `SetCurrentFrontEdge`) ou à frente do lote movido
  (`RotateLotImposter` + `EvaluatePlacementValidity`); `0x8CA297E2` cancela.
  O mover gira pela direção do arrasto com o botão apertado (`RotateLot`, §15.1).
- **Praia** (flag `0x80` do lot info): sem o teste de água e com
  `IsOKToPlaceBeachLot` (BV `0xa2687c`): a última borda com rua a no máximo
  10 acima do mar, a 1,0 da altura de rua do lote base, e um vértice no mar
  ou abaixo na borda oposta. O `FlattenTerrainUnderLot` não mexe em lote de
  praia. No jogo base não há botão de lote vazio de praia (o `this+0x116`
  vem dos tamanhos 10–19 das expansões).
- **No app**: os lotes da caixa e os movidos gravam o terreno conformado; o
  lote vazio, o alisado (`formats/hood_lots.*`, `Terrain::write`); `,`/`.`
  giram o lote (`--hood-turn` sem tela); o puck desenha **por cima** dos
  painéis Famílias e Lotes & Casas (a arte do painel começa sob o puck,
  x = −28/−98), e a câmera fica onde estava quando a vista é refeita depois
  de pôr um lote (`LotCamera::pose`). `r11-bairro-puck-sobre-painel.jpg`,
  `r11-bairro-lote-conformado.jpg`.
- **Caixa de nome e zona** (`cTSWinProcPlaceLotDialog`, script
  `0x49001002`; `cTSUserToolPlaceNewLot::OnRelease` a mostra e, com Aceitar,
  chama `PlaceLot(nome, zona)`): botões de opção 1 "Residencial" e 2
  "Comunidade" (estilo `radiocheck`: imagem de 8 quadros de 20×15, marcado e
  não marcado; residencial ligado no `Show`), o texto 3 começa com STR#
  0x83 #29 "<Enter Lot Name>" todo selecionado (digitar substitui). Aceitar
  (`onBtnClickedID 0xA000`): `nTSUI::ValidateUIString` recusa `\ / : * ? <
  > | ;` (UIText STR# 0x99 #9+#10, título #11; o texto padrão, com `<>`,
  cai aqui) e o bairro (vtable `0xe4`) recusa nome de outro lote (STR# 0x82
  #8, título #9); Cancelar fecha sem pôr. Os índices do `FUN_000da4b0` são
  1-based. No app: `--hood-lot-name`/`--hood-lot-zone community` sem tela,
  `r11-bairro-nome-do-lote.jpg`, `r11-bairro-lote-comunitario-novo.jpg`.
- **Mandar para a caixa** (`cTSUserToolMoveLotToCatalog`, BV `0x37f554`):
  lote sem casa → STR# 0x83 #26; com família morando → a pergunta #28 ganha
  #31 ("a família … será movida com o lote e perderá os relacionamentos") e
  o Sim manda a mensagem `0xEC1AAC03` com o lote (o controle de estado salva
  lote e família no catálogo); flag `0x40` do lote e família fora (BV,
  STR# 0xD2) são de expansão.
- **Caixa de Lotes & Casas** (`cTSWinProcLotCatalog`): o botão `0xC` sob a
  seta da bandeja ("Excluir", BV `0x454bec`) apaga o lote escolhido depois
  de perguntar STR# 0x83 #15 ("Tem certeza de que quer remover este lote?",
  #24 "Excluir a família %s e o lote dela?" com família); os filtros
  `0xC400` Ocupado, `0xC500` Desocupado e `0xC600` Todos são o
  `LoadLots(1|2|0)` (BV `0x457738`: com família ou não). No app:
  `r11-bairro-excluir-da-caixa.jpg`.
- **Os Sims que chegam pela árvore** (rodada 11): com a VM (app, `hood-save`,
  `autonomy`) o `addArrivingSims` não cria mais os Sims; no primeiro dia
  (`SetupFamilyFirstDay`) o `vm::runResetLot` faz o
  `cEdithObjectModule::BroadcastMessage("reset lot")` (BV `0x525b2c`: a
  árvore desse nome de cada objeto que a tem, empurrada com 4 parâmetros
  zero; aqui rodada na hora como árvore de um tick, o objeto como objeto da
  pilha), com os OBJD dos personagens da família somados ao código antes.
  Em N001 lote 24 (família 8): 7 objetos têm "reset lot"; o "Sub - Generate
  Family" do Controller - Age cria os 3 Sims pelo Create New Object
  Instance (flag 0x04) e o Find Location For modo 4 os põe ao lado da caixa
  de correio. O Find Location For agora **prefere tiles vazios** sem a flag
  0x02 (inferido: primeiro só tiles sem outro objeto, Sims inclusive;
  antes os três ficavam no mesmo tile). O dado de pessoa 109 do Sim novo
  (cópia do vizinho, o último ID de interação dele) é refeito pela fila
  (`syncQueueData`, como a retomada faz). O app dá atores aos Sims que a
  árvore criou (`LotSimulation::arrivedSims`). `hood-save --check`: 26 de
  26 iguais, e a segunda passada também. `r11-bairro-familia-chega-pela-arvore.jpg`.
- **Opções do bairro** (puck `0x2BF08BEC`, "Opções - F5"): o painel
  `0x49001028` preso ao puck pelo `aabbccdd:tl` do `0xA200`, com "Menu
  Principal" `0xA221` (volta ao menu) e "Sair" `0xA220` (sai do jogo); as
  janelas de opções `0xA241`–`0xA246` ainda não respondem aqui. Teclas F1,
  F2 e F5 abrem/fecham Famílias, Lotes & Casas e Opções (as dicas dos
  botões). `r11-bairro-opcoes.jpg`.
- **"Informações" do Lot Info Dialog** (`0xB004`/`0xC004`): troca o painel
  básico `0xB000` pelo avançado `0xC000` (família, lote, patrimônio, uma
  foto de rosto por membro em `0xC111`+k com o botão `0xC101`+k que escolhe o
  membro, o nome em `0xC110`, o ícone de aspiração `0xC201`–`0xC206` pelo
  dado 46 e a biografia da família na caixa de texto). A foto é o IMG do
  arquivo do personagem cuja instância é um bit de idade (8 adulto, 0x10
  idoso…; o bit << 8 é a silhueta; inferido dos arquivos). O que o
  `cTSWinProcLotInfo` faz exatamente não foi lido. Sem tela: `--hood-select
  <lote> --hood-info <membro>`, `r11-bairro-info-do-lote.jpg`. "Transferir
  Lote" (empacotar em `.Sims2Pack`) e "Ver Amigos" ainda não.
- **Lote ocupado para a caixa** (no app; o formato do catálogo com família
  é **inferido**): a pergunta é a #28 + #31; Sim copia o pacote do lote como
  `cx_<n>` com o lot info base e, dentro dele, o FAMI da família, as strings
  do nome (STR# da família), o guarda-roupa (`0x6C4F359D`) e a foto
  (`0x8C3CE95A`); o bairro perde o FAMI, as strings, o guarda-roupa, a foto,
  as relações (SREL) entre a família e os outros Sims e as entradas deles
  nas listas de relações dos SDSC ("perderá todos os seus relacionamentos").
  Os personagens e os SDSC ficam. A caixa mostra o lote no filtro Ocupado;
  pôr de volta grava o FAMI morando no lote novo (com o número antigo, ou o
  próximo livre e o dado 61 dos Sims refeito) e tira o lote da caixa (não
  dá para pôr a família duas vezes). N001: Caixão para a caixa (84
  relações), de volta no lote 83, os 4 Sims e §511.632 voltam;
  `hood-save --check` 26 de 26. `r11-bairro-lote-com-familia-para-caixa.jpg`,
  `r11-bairro-familia-volta-com-o-lote.jpg`.

## 14. Rodada 12 (frente do bairro 4): opções, decorações, história, amigos, cabeças

Tudo daqui vem dos scripts de UI e dos dados; o que o binário faz por trás
**não foi lido** (sem Ghidra nesta rodada) e está marcado como inferido.

- **Janelas de opções do puck** (`0x49001028`, botões `0xA241` gráficos,
  `0xA242` áudio, `0xA243` jogo, `0xA245` câmera, `0xA246` créditos; `0xA244`
  casa fica escondido): as mesmas janelas do painel de opções do lote
  (`app/options_panel.*`): áudio `0x49060F01` e jogo `0x49060F02` mexem no
  `Options.txt`; gráficos `0x49060F03`, câmera `0x49060F05` e créditos
  `0x49060F08` são desenhadas como o script as tem e os grupos de rádio
  (Desligado/Médio/Alto...) só guardam o clique na memória (nada do motor os
  lê). A janela fica com o canto inferior esquerdo no do painel e **sob** o
  puck e o painel (a aba da arte fica atrás). `r12-bairro-opcoes-*.jpg`.
  **Rodada 15 (binário):** a janela de gráficos é a
  `cTSWinProcOptionsPanelPerformance` (o `cTSWinProcGraphicsOptionsDialog`
  é outra coisa: um diálogo montado das `DialogEntries` de um ini). As listas
  `0x35`/`0x45` (classe `0xAC17CE2A`, `CreateScreenSizeListBox`/
  `CreateRefreshRateListBox`) são enchidas pela vtable `0x8C` (BV
  `0x421958`, `FillScreenSizeListBox`) e `0x98` (BV `0x421f9c`,
  `FillRefreshRateListBox`): os modos do dispositivo na ordem dele, só os
  válidos (`IsDisplayModeValid`, BV `0x42173c`: o formato de pixel do
  painel, 800 ≤ largura ≤ `maxResWidth` e 600 ≤ altura ≤ `maxResHeight` —
  propriedades do `Graphics Rules.sgr` `ScreenModeResolution`, 1600 × 1200
  no $High, 1600/1200 também de padrão no código —, altura < largura e
  frequência 0 ou 45..100), cada tamanho uma vez, texto `"%ux%u"`; nenhuma
  linha: o primeiro modo assim mesmo; escolhida a do modo atual, senão a
  linha 0. As frequências: as dos modos válidos do tamanho escolhido, cada
  uma uma vez, `"%u"` (0 = STR# 0x83 #15 do texto das opções, grupo
  `0x7F232750`). Depois, **em janela** o `FillScreenSizeListBox` limpa a
  lista de frequências e desliga o rótulo "Freqüência de Atualização"
  (`0xAD7FAA35`, bit 2); em tela cheia liga. O app sempre roda em janela:
  a lista fica vazia e o rótulo na `colorfontdisabled` (a ordem dos modos
  que o Mac dá ao dispositivo é inferida: a do CoreGraphics).
  `r15-graficos-modos.jpg`.
- **Decorações (F3)**: painel `0x4D517180` (botão `0x2D51756D`): cinco
  botões de tipo = o campo `sort` do produto em `catcanhobjects`
  (A100 flora, A200 stone, A300 landmark, A400 effects, A500 misc), o
  caixote de itens do painel das famílias (0x1000/0x2000, itens `0x49001006`),
  Mão `0xA5`, Conta-Gotas `0xA4`, Desfazer `0xA1`, Refazer `0xA2`. 72 produtos
  têm `showincatalog=1`. Só os efeitos têm imagem de UI (`thumbnailgroupid`
  0x499DB772); as outras miniaturas são renderizadas do modelo
  (`renderModelThumbnail`). Colocar: um clique no tile (centro do tile, como
  os 72 enfeites de N001: x e y múltiplos de 10 + 5) com `,` `.` girando 90°;
  o registro entra no recurso `0xABD0DC63` (38 bytes: versão 2, posição,
  retângulo, versão 8, depois, **flora: rotação e GUID; enfeite: GUID e
  rotação**) no fim da lista. **Inferido**: sort `flora` vai na lista de flora e
  o resto na de enfeites (N001–N003 têm moinhos e campos de cultivo na lista de
  flora, então o jogo pode decidir de outro jeito); `placementsurface` 0 só
  terra, 1 só água, 2 ambos; `allowedinlot`/`allowedonroad` = 0 proíbem lote e
  rua sob a pegada. A Mão tira o decorativo clicado e o segura para pôr em
  outro lugar; o Conta-Gotas pega o produto. Os nomes dos produtos
  (`stringsetid` 123) são todos "Rocha": as dicas não mostram nome. Testes:
  `tests/test_hood_moves.cpp` (adicionar/remover o registro),
  `s2pkg hood-decor`. `r12-bairro-decoracoes-*.jpg`.
- **História (F4)**: painel `0x49001030` ("Bem-vindo a %s!"): a imagem
  `<N>_Neighborhood.png` (400x300) e a descrição do bairro (CTSS) na caixa
  de texto, ou "Não há história" (`0x4D7407B2`); Pronto fecha. O editor
  (botão `0x0CB23B3C`) só mostra a caixa vazia, sem digitar nem gravar; as
  páginas da pasta Storytelling do jogo (`AppendStorytellingDirName`) não
  são lidas.
- **Lot Info**: "Transferir Lote" (`0xC00B`, `0xD006`, `0xE006`) abre
  `0xEFFF4500` (dois botões de opção, Aceitar/Cancelar), sem exportar nada.
  "Ver Amigos da Família e Parentes" (`0xA003`/`0xB003`/`0xC003`) fecha o
  diálogo e realça os lotes de amigos (azul) e parentes (violeta): **inferido**,
  pelas SREL dos membros (bits 4/5 dos flags = amigo, bit 14 ou relação
  familiar != 0 = parente). `r12-bairro-ver-amigos.jpg`.
- **Cabeças**: todos os arquivos de personagem dos 4 bairros base têm a
  imagem de rosto; para os que não tiverem, `app/head_shot.*` renderiza a
  cabeça com a câmera do CAS e luz levantada (inferido; `--hood-render-heads`
  força para teste). `r12-bairro-cabecas-renderizadas.jpg`.
- `FlattenAbuttingRoadsIfPossible` e o `RotateLot` do mover: lidos na
  rodada 13, §15.

## 15. Rodada 13 (RE): o mover de lotes e o nivelamento das ruas (binário)

Lido no BV 1.10 (Ghidra headless, nomes do `symxfer` conferidos pelos
chamadores e pelas vtables).

### 15.1 O mover (`cTSUserToolMoveLot`)

`MouseEvent` (BV `0x223584`): 3 = mover, 4 = apertar, 5 = soltar. Estado do
tool: `+0xd1` lote na mão, `+0xd2` botão apertado com o lote na mão, `+0xd3`
há um lote sob o cursor, `+0xd4` o lugar é válido, `+0xe4` ID do lote,
`+0xe8` lot info, `+0xec/+0xf0` tile do bairro, `+0xf4` frente (borda 0–3),
`+0xf8/+0xfc` ponto do clique.

- **Mover sem lote** (`FUN_00222ec8`): `PickLot` (vtable `0x60`) e
  `FUN_002223b4` (o lote pode ser movido); sim → cursor `0x19` e `+0xd3`,
  e já guarda o lot info, a frente (lot info vtable `0x54`) e o tile; não →
  cursor 0.
- **Soltar sem lote** (`FUN_002230d8`): com `+0xd3`, pega o lote (`+0xd1`,
  liga as teclas de manipulação).
- **Mover com o lote, botão solto**: `GroundAtGameWindowPoint` (vtable
  `0x74`, unidades do mundo do bairro, 10 por tile) → tile
  (`ConvertToTileCoordinates`, vtable `0x54`), `MoveLotImposter`,
  `EvaluatePlacementValidity`.
- **Apertar com o lote** (`DoMouseDown`, BV `0x222e20`): guarda o ponto do
  chão em `+0xf8` e liga `+0xd2`. Sem lote na mão, com um lote sob o cursor,
  só troca o cursor para `0x1a`.
- **Arrastar com o botão apertado = girar** (`RotateLot`, BV `0x222b2c`):
  o lote **não** anda; a frente segue a **direção do arrasto a partir do
  ponto do clique** (não a borda mais perto do cursor, como o doc dizia):
  `FUN_00a27080(clique, agora)`: se |dx| + |dy| < **16,0** unidades (1,6
  tile do bairro), nada; senão θ = atan2(dy, dx) e
  `frente = ⌊2,5 − 2θ/π⌋ mod 4` (θ = 0, +x → **2**; +y → **1**; −x → **0**;
  −y → **3**: quadrantes de 90° centrados nos eixos, o mesmo número de borda
  do `FlattenAbuttingRoadsIfPossible` abaixo). O cursor 2D é posto
  (`Set2DCursor`); se a frente mudou: grava `+0xf4`,
  `EvaluatePlacementValidity`, cursor `0x1C`/`0x1E`/`0x1F`/`0x1D` para a
  frente 0/1/2/3 e `RotateLotImposter(lote, frente, false)` (mensagem
  `0x4C4CCCEC` com o lote, a frente e o bool).
- **Soltar com o lote**: apaga `+0xd2`; se `IsOKToPlaceLot` (BV `0x222768`:
  `IsOKToPlaceLotCommon` com a flag de praia `0x80`, as bordas com rua do
  lote giradas iguais às do lugar, e `TryModifyingTerrainToConformToLot`
  em ensaio) → põe: cursor 0, mensagem `0xB1` com o lote, tira o realce,
  `FUN_00a284cc(ret)`, vtable `0x9c(ret, 0)`,
  `SmoothenRoadsConnectedToLot`, posição (lot info `0x24`), frente (`0x50`),
  altura da rua da frente (`this+0x6c[frente]` se a borda tem rua, senão 0;
  lot info `0x3c`), `TryModifyingTerrainToConformToLot` **para valer**,
  praia → `FUN_00a2afec`, flag `0x100`, `MoveLotImposter(..., true)`,
  `SetLotSkirtPaint`, lot info `0x5c(1)`, solta o lote. Inválido: o lote
  continua na mão. **O mover não chama `FlattenAbuttingRoadsIfPossible`**
  (só o lote vazio novo).
- **Teclas** (`KeyCommand`, BV `0x222c90`): `0x8CA297E2` cancela
  (`RestoreImposterToOriginalState`, solta tudo); `0xACB3A629` gira: valor
  da tecla > 0 → frente + 1, senão + 3 (−1), mod 4; `RotateLotImposter` e
  `EvaluatePlacementValidity`.
- **Realce** (`EvaluatePlacementValidity`, BV `0x2229d8`): o retângulo do
  lote, válido ou não, e para o `AddLotPlopHighlight` as bordas com rua do
  lote giradas: `rua[(i + frente − frente_do_lote + 4) mod 4] = borda(i)`.
- Da caixa (`cTSUserToolPlaceLotFromInventory::SetCurrentFrontEdge`, BV
  `0x3fd6d8`): fora do modo `+0x100`, frente nova → `RotateLotImposter` e
  `UpdatePlacementValidity`.

### 15.2 `FlattenAbuttingRoadsIfPossible` (BV `0xa2a598`)

Chamado só por `cTSUserToolPlaceNewLot::PlaceLot` (BV `0x2c7344`) com o
retângulo do lote em tiles do bairro (x0, y0, x1, y1), a **frente** (a
primeira borda com rua, 0..3) e `h` = a altura da rua dessa borda
(`FUN_00a265cc`: `this+0x6c[frente]`, 0 sem rua). Bordas: 0 = x baixo,
1 = y alto, 2 = x alto, 3 = y baixo.

1. A **faixa da frente**: 0 → x ∈ [x0, x0+1], y ∈ [y0, y1]; 1 → y ∈
   [y1−1, y1]; 2 → x ∈ [x1−1, x1]; 3 → y ∈ [y0, y0+1] (a outra coordenada
   inteira).
2. Quatro tiras de fora, uma por borda, em ordem 0..3:
   [x0−1, x0]×[y0−1, y1+1], [x0−1, x1+1]×[y1, y1+1],
   [x1, x1+1]×[y0−1, y1+1], [x0−1, x1+1]×[y0−1, y0]; pula a que sai do
   bairro (< 0 ou > tamanho `this+0x88/+0x8c`).
3. Ruas da tira (`GetOccupants(tira, 2)` do gerente de ocupantes,
   `this+0xc4` vtable `0x28`); nenhuma → próxima tira.
4. Se a tira cruza a faixa da frente (teste inclusivo), a altura é **fixa**
   em `h`; senão é livre.
5. `DetermineFlattenHeightForRoadOccs(ruas, &altura, fixa)` (BV `0xa27714`,
   vtable `0xa8`): faixa [lo, hi] = [h, h] se fixa, senão [−FLT_MAX,
   FLT_MAX]. Para cada rua, o retângulo dela (vtable `0x20`), passado pela
   vtable `0x50(ret, 1)` do tool; se há um **lote** nesse retângulo
   (bairro `this+0x60` vtable `0xec`) e os retângulos se cruzam, cada
   vértice da interseção (terreno `this+0x64` vtable `0x28(x, y)` = altura)
   tem de estar a **0,01** de lo e de hi (senão **falha**), e lo = hi = essa
   altura (os vértices já presos a um lote mandam). No fim: lo == hi → essa
   altura; livre e sem lote → a altura do vértice (x0, y0) da primeira rua
   (dentro de [lo, hi]).
6. Deu certo: altura arredondada para o múltiplo de **0,1875** mais perto
   (`round(a / 0,1875) · 0,1875`) e `FlattenRoadsAndTerrain(ruas, altura)`
   (BV `0xa27a30`, vtable `0xac`): **todos os vértices** do retângulo de
   cada rua (x0..x1, y0..y1, inclusivo) recebem a altura (terreno vtable
   `0x20(altura, x, y, ?, 1)`). Falhou: a tira fica como está.

Depois o `PlaceLot` segue com a praia, `SmoothenRoadsConnectedToLot`
(gerente de ocupantes vtable `0x50`), a suavização sob o lote etc. (§11.4).

### 15.3 "Ver Amigos": `cTSNeighborhood::ShowFamilyRelationshipsWeb` (BV `0x4da814`)

O slot `0x108` da vtable do `cTSNeighborhood` (ao lado do
`HideFamilyRelationshipsWeb`, `0x10c`; o symxfer não o nomeou):

1. esconde a teia anterior (`Hide`: o desenhador de teia do `TSSGSystem`
   vtable `0xf0` limpa, e `RemoveAllLotHighlights`); começa a teia nova;
2. origem = o centro do lote da família (`família→0xa8` = ID do lote; o
   retângulo em tiles do bairro × **10**; z = vtable `0x28` do terreno −
   **2,0**);
3. `GetFamilyFriendsGuids(família, guids, true)` (BV `0x53a480`): para cada
   vizinho N fora da família e com a vtable `0x40(0x3d)` verdadeira, se
   algum membro M da família passa no **`TestFriendsAndFamily(N, M)`** (BV
   `0x537a50`), entra o GUID de N. O teste: os dois se conhecem (tabela de
   relações, vtable `0x14`) e (os flags de relação dos **dois lados** têm o
   bit **4 ou 5**, ou um dos lados tem o campo de relação **familiar**
   (vtable `0x24(guid, 3)`) ≠ 0). (O `TestFriends`, com `false`, exige os
   bits 4/5 dos dois lados e nenhuma relação familiar.)
4. para cada GUID, o primeiro lote cuja família o contém (lot info vtable
   `0x78`) entra num conjunto (um lote só uma vez);
5. para cada lote do conjunto: um **arco** da origem ao centro dele
   (desenhador vtable `0x1c(1,0, 45,0, de, para, "RelationshipArchGreen", 8)`)
   e o realce do lote com o material **"DefaultLotHighlight"**
   (`AddLotHighlight(lote, nulo)`, BV `0x4da600`).

Ou seja, **amigos e parentes saem iguais**: um arco verde e o realce padrão,
sem cores diferentes; o inferido da rodada 12 (azul para amigos, violeta
para parentes, sem arcos) não é o do jogo. Quem chama o slot `0x108` não foi
procurado (inferido: o botão da janela do lote).

### 15.4 Decorações: `cTSUserToolPlopNeighborhoodObject` (binário)

- **Tipo do produto** (o carregador do catálogo `catcanhobjects`, BV
  `0xb34c64`, produto `0x6D619378` → `cTSProductNeighborhoodObject`): o
  `sort` vira um número (product vtable `200`): `flora` 0, `stone` 1,
  `landmark` 2, `effects` 3, `misc` 4. Só o tipo 3 (efeitos) recebe o
  `thumbnailgroupid`/`thumbnailinstanceid`. `placementsurface` 0/1/2 vai
  para a vtable `0xdc`; três booleanos (`== 1`) para `0xd0`, `0xd4`, `0xd8`,
  que pela ordem das propriedades são `allowedinlot`, `removeonlotplop` e
  `allowedonroad` (ordem inferida; `0xd0` = `allowedinlot` bate com a
  rodada 11).
- **Flora ou enfeite** (`InstantiateObjectForCatalogProduct`, BV
  `0x4164d4`): tipo **0 → `CreateFloraOccupant`**, tipos **1–4 →
  `CreatePropOccupant`** (confirma o inferido da rodada 12: só o `sort`
  `flora` vai para a lista de flora). Depois põe o ocupante no ponto
  guardado, a rotação, mostra e cursor `0x1a`.
- **Pode pôr?** (`IsOKToPlaceObjectAtCurrentLocation`, BV `0x415d34`;
  retângulo de tiles do ocupante, vtable `0x20`, x0 ≤ x < x1):
  1. (`removeonlotplop` ou não `allowedinlot`) e há um lote no retângulo
     (bairro vtable `0xec`) → erro **1**;
  2. superfície 0: cada tile tem de ser terra (terreno vtable `0x98`),
     senão erro **3**; superfície 1: cada tile água (vtable `0x94`), senão
     erro **4**; 2: sem teste;
  3. sem `allowedonroad`: uma rua do retângulo (`GetOccupants(ret, 2)`) cujo
     volume (vtable `0x24`) cruza os limites do ocupante (vtable `0x1c`) →
     erro **2**;
  4. senão pode. (Os textos dos erros 1–4 saem pelo `FUN_01633638`; não
     lidos.)
- **Altura** (`SetOccupantPosition`, BV `0x415b74`): z = altura do terreno
  no ponto; com superfície 0 ou 1, z = max(terreno, nível da água (terreno
  vtable `0x3c`)); com 2, só o terreno.

### 15.5 A foto do rosto: `cTSUIThumbnailFactorySimHead::SetupScene` (BV `0x392a88`)

O que o jogo faz quando renderiza a cabeça de um Sim (a "foto" das janelas
de família; o `app/head_shot.*` do opensims2 é inferido):

- cena "UIUCPHead"; o esqueleto do Sim copiado (`CopyPersonSkeleton`), os
  objetos dos ossos `glasses_grip` e `hat_grip` copiados junto;
- **pose**: `"%c-UIPose-%s%02d_anim"` (ou `"%c-UIPose-pose%d_anim"` sem o
  texto), com a letra da idade (`a` padrão, `c`, `p`; as outras letras são
  dos bichos do BV), o texto do sexo ("M"/"F", como nos recursos
  `a-UIpose-M07_anim`) e um número sorteado de 1 a N (N 2–9 conforme a
  idade); com a carta de ódio (tipo 1) usa `a-hateLetterPose-%02d_anim`
  (1–3);
- **o sorteio da pose (rodada 15):** `FUN_001718c8(estado, 1, N)` é um LCG
  próprio da fábrica (`+0xa8`: estado = estado · `0x41C64E6D` + `0x3039`) que
  devolve **1 + ((N − 1) · (16 bits altos do novo estado)) >> 16**, isto é,
  de 1 a **N − 1** (o topo não entra). A letra vem do tipo de idade/espécie
  (vtable `0x84` do Sim ou `0x4c` da descrição): 1 → `c`, 2 e 13 → `p`, os
  bichos do BV (5–12) → `d`/`l`/`f`/`w`/`k`, o resto → `a`. Para humanos N
  sai de uma palavra de flags (campo `0x54` da descrição; inferido: as
  flags de idade do CAS): bit 0 → 4, senão bit 2 → 6, senão bit 6 livre →
  **9** (poses 1–8, as oito `M01`–`M08`/`F01`–`F08` do jogo base), bit 6 →
  4; com um dos bits 0/2/6 o texto do sexo ganha um sufixo (não lido). Os
  arquivos do jogo base têm `a`/`c`/`p` × `M`/`F` × 01–08 e `b-UIpose-01…04`
  (sem sexo). O app escolhe um número fixo por arquivo de personagem (1–8):
  o mesmo conjunto, sem o sorteio a cada foto (fica assim: o visual basta);
- **enquadramento** (humano, espécie 1): o osso `head` (posição no mundo
  P); caixa de P + (−0,15, −0,15, −0,10) até P + (0,15, 0,15, 0,20) (0,30 ×
  0,30 × 0,30, de 0,10 abaixo a 0,20 acima do osso); a câmera olha para o
  **centro** da caixa, de frente para o Sim (rotação de π), e a distância é
  a que faz a caixa caber no campo de visão: d = (lado maior da caixa
  na tela) / (2 · tan(fov · 0,5)) (mín. 0,01) + 2 × a profundidade; o
  multiplicador do fov é 1,0 para humanos; luz e cor (`0,8`, `1,2`) no
  fim. (Bichos do BV usam outras caixas.)

Para o opensims2: trocar o "0,8 de proximidade" inferido por esta caixa
de 0,30 em volta do osso `head` e o enquadramento por ajuste do fov.


## 16. Rodada 13 (frente do bairro 5): no app

O que o §15 leu, agora no opensims2, mais o que esta frente leu no binário
(o mesmo Ghidra headless).

- **`FlattenAbuttingRoadsIfPossible`** (`hood::flattenAbuttingRoads`,
  `formats/hood_lots.*`): o lote vazio novo nivela as ruas das quatro tiras
  de fora (§15.2) antes de alisar o terreno; as ruas mexidas são deitadas de
  novo no terreno, cada canto 0,4 acima do seu vértice (**inferido**: o
  `SmoothenRoadsConnectedToLot` chama a vtable `0x5c(1)` da rua, lida como o
  `FlattenToTerrain`; o `SmoothenContainingRoadSegment`: rodada 14, §17.3; pontes ficam de fora). O recurso de ocupantes
  ganhou `Occupants::setRoadHeights` (só o z dos cantos). Inferido também:
  a vtable `0x50(ret, 1)` do tool é o `ExpandVertexRect` de 1, e o lote do
  bairro (vtable `0xec`) o primeiro que cruza o retângulo. Teste em
  `tests/test_hood_moves.cpp`; em N001, um 4x3 em (80, 8) nivela 2 ruas
  para 329,625 e o `hood-lot-check` continua passando todos.
- **`RotateLot`** (`hood::frontFromDrag`, `HoodMode::mouseDragged/mouseUp`):
  com um lote na mão (vazio, da caixa ou movido) apertar guarda o ponto do
  chão; arrastar com o botão apertado gira (a frente segue a direção do
  arrasto, zona morta de 16 unidades), o lote não anda; soltar põe o lote
  (o lote vazio pede o nome), inválido continua na mão. Os passos `--click`
  sem tela apertam e soltam no mesmo ponto; `--hood-drag dx,dy` (unidades
  do mundo) depois de `--hood-place-at` arrasta a partir do meio do lote.
  `r13-bairro-girar-arrastando.jpg` (frente 0 válida; arrastado para +x,
  frente 2, sem rua, vermelho).
- **Decorações** (§15.4; `hood::decorationError`): flora → lista de flora,
  o resto → enfeites (confirmado: `FUN_00b36510` dá o `ObjectType` pelo sort
  — flora 0, stone 1, landmark 2, effects 3, misc 4 — e o
  `InstantiateObjectForCatalogProduct` cria flora para 0, enfeite para
  1–4). Os erros são os índices do STR# 0x89 do `Text/Neighborhood.package`
  (1 lote, 2 rua, 3 terra, 4 água; "Não pode ser colocado dentro de
  lotes.", ...), checados nessa ordem: lote (`removeonlotplop` ou sem
  `allowedinlot`, retângulo de tiles), cada tile do retângulo todo terra
  (`IsCompletelyLandTile`, vtable `0x98`: nenhum vértice abaixo do mar) ou
  toda água (`IsCompletelyWaterTile`, `0x94`: os 4 abaixo), rua pelo volume
  (inclusivo). Os getters do produto: `0xd0` allowedinlot (`+0xcc`, padrão
  1), `0xd4` removeonlotplop (`+0xce`), `0xd8` allowedonroad (`+0xcd`),
  `0xdc` placementsurface (`+0xd0`, padrão 2). O z segue o §15.4. A recusa
  aparece na tela como a dica do jogo (`ShowErrorTooltip`: a janela de dica
  do gerente de janelas por 3000 ms no ponto; aqui uma caixa como as dicas
  do modo vivo, ao lado do ponto clicado; o visual é inferido).
  `r13-bairro-decoracao-recusada.jpg`.
- **"Ver Amigos"** (§15.3): amigos e parentes iguais; o lote de cada um
  ganha o realce "DefaultLotHighlight" (branco a 0,3, como o realce do
  mouse: inferido) e um arco verde "RelationshipArchGreen" do meio do lote
  da família (largura 1, 45°, as pontas 2 abaixo do chão; a forma — uma fita
  z = x(1 − x) do comprimento — é inferida; o material também). Antes:
  azul e violeta inferidos. Os membros da família são IDs de vizinho; um
  vizinho conta se tem família (dado 61) e não é da família.
  `r13-bairro-ver-amigos-arcos.jpg`.
- **Cabeças** (§15.5): a pose `<idade>-UIpose-<M|F>NN` (o número fixo por
  arquivo de personagem; o jogo sorteia), a caixa de 0,30 em volta do osso
  `head`, a câmera de frente à distância que cabe a caixa no fov (rodada 14:
  20°, o da câmera das miniaturas, §17.4) mais 2 × a profundidade, e a luz "UIUCPHead" do `Lights/LightRig.txt` (recurso
  "UIPieMenu": `UIPieMenu_cres` do `Sims3D/LightRigs.package`, cinco luzes
  direcionais key/bottomFill/rightFill/rightRim/leftRim, com as intensidades
  e cores do `Lights/LightRigs/PieMenu.txt`); a direção de cada luz é o +Z
  do seu nó (**inferido**; dá a key de frente e de cima, os rims atrás e o
  bottomFill de baixo, como os nomes dizem). Saiu o clareamento inferido.
  `r13-bairro-cabecas-rig.jpg`.
- **Opções** (`app/options_panel.*`, `formats/graphics_options.hpp`): cada
  grupo de rádio das janelas Gráficos e Câmera é uma opção (`Shadows`,
  `LightingQuality`, `EffectsQuality`, `ObjectDetail`, `MaterialDetail`,
  `Reflection`, `UITransparency`, `SquarePixels`, `ObjectHiding`; `LivePIP`,
  `SpecialEventCamera`, `NeighborhoodCameraDrift`, `SnapshotSize`,
  `SnapshotQuality`, `VideoSize`, `VideoSound`, `VideoQuality`) gravada no
  `Options.txt` como `option.<nome> = <n>` (1 = o primeiro botão). O motor
  honra, como o `Graphics Rules.sgr` define: **Shadows** baixo = sem `guob`
  (os planos de sombra no chão dos objetos, "groundshadow"/"*wallshadow") e
  sem `heightMapShadows` (a sombra dos morros no terreno do bairro); médio e
  alto com os dois (simShadows/objectShadows não existem aqui);
  **Reflection** = `GameSettings::reflections` (opção `reflection` do
  `Options.txt`, que o espelho da frente r13-objects-render lê a cada
  quadro) e `reflectionWithExtraViewer`/`nhoodWaterReflection`; **LightingQuality** baixo = sem
  `specHighlights`. Vale para o que carrega depois (o jogo aplica no
  "Aplicar Mudanças"). A rolagem nas bordas já ia para o `Options.txt`. Não
  há "distância de visão" nem "velocidade da câmera" no jogo base.
  `r13-bairro-opcoes-sombras.jpg` (padrão × Sombras Desligado + Iluminação
  Baixo: a sombra do morro some).
- **Diálogo "yes-no"** (QA ao vivo): o título vinha do rótulo do editor do
  tipo (STR# 0xD9) quando o diálogo não tem título; o jogo usa só a string
  do título (byte 6 ou temp 3 com a flag 0x10) — 172 dos 176 diálogos
  sim/não dos objetos não têm nenhuma (o "Ei $NeighborLocal:1, ... pode
  falar agora?" do telefone, `Answer - Call Not for Answering Sim`, título
  0). Agora, sem título, a caixa não tem barra de título (como o FreeSO).
  Inferido: a janela do jogo (`cTSObjectDialog`) não foi decodificada.


## 17. Rodada 14 (construção 6): o arco e o realce de "Ver Amigos", a sombra dos imposters, as ruas, as cabeças

### 17.1 O arco e o realce (dados + binário)

Os dois materiais estão no script de material do bairro
(`Materials.package`, `0xCD7FE87A-1C0532FA-26B3DB7C-FF3A262A`):

- `RelationshipArchGreen`: `StandardMaterial`, `stdMatDiffCoef (0.3, 1, 0.3)`,
  `stdMatSpecCoef (1, 1, 1)`, `stdMatSpecPower 20`, `stdMatAlphaMultiplier 0.6`,
  `stdMatAlphaBlendMode blend` (iluminado, sem textura);
- `DefaultLotHighlight`: `NHoodTerrainHighlightMaterial` com
  `highlightTexture "lotselection-good"` e `highlightAlpha 0.25` (a textura
  vezes `colorScalar (1, 1, 1, alpha)`, sem luz, misturada, endereçamento
  `tile`).

A forma vem do binário: o desenhador é o `nTSSG::c3DUIManager` e o slot
`0x1c` é **`AddArch(de, para, material, float, ulong, ulong, float)`** (BV
`0x6a9818`; o `baserom.sym` dá o nome), chamado com `1.0`, `8`, `32` e
`45.0`. Ele guarda um `cGeneralizedCylinderData` (0x30 bytes: de, para,
material, raio `+0x20` = 1,0, as duas contagens, ângulo `+0x2c` = 45); o
`End` (BV `0x6aa1f8`) monta para cada um os pontos de controle
(`FUN_006a8284`: P1 = de + 0,25·d e P2 = de + 0,75·d na horizontal, os dois
a z = max(z de, z para) + tan(ângulo) × 0,25 × |d|) e o `FUN_006a9970` faz
um **tubo** de raio 1 em volta da curva de Bézier cúbica (coeficientes 3, 6,
−3): anéis ao longo da curva, cada um com `n` vértices em volta (ângulo
2π/(n − 1)), o referencial do anel pelos produtos vetoriais da tangente;
cada material num subconjunto "subset%d". Qual contagem é ao longo e qual é
em volta: inferido (32 ao longo, 8 em volta).

No código (`hood::relationshipArchMesh`, `HoodView::showFriends`): o tubo
entre os meios dos lotes (2 abaixo do chão, §15.3) num modelo por arco com o
material do script; o realce com a textura `lotselection-good` vezes 0,25,
sem luz, num quadrado sobre o lote (a textura uma vez no lote: inferido; o
jogo projeta no terreno). Antes: uma fita z = x(1 − x) e um branco a 0,3
(inferidos). `test_hood_terrain`; `docs/screenshots/r14-bairro-ver-amigos-tubos.jpg`.

### 17.2 A sombra dos imposters e a opção Sombras

O `ImposterColorScalar` de cada lote é a sombra do terreno sob ele
(`cTerrainShadeLight`, `hood::terrainShade` sobre o mapa de luz). Desde a
rodada 13 a opção Sombras refaz o mapa de luz na hora
(`HoodView::refreshLightMap`), mas os imposters ficavam com a sombra da
carga. Agora cada lote guarda o imposter lido (antes dos materiais) e o
modelo no renderer, e o `refreshLightMap` refaz os materiais de cada um com a
sombra nova (`Renderer::replaceObject`). Conferido só compilando e no
caminho da carga (o clique na janela de Gráficos sem tela não foi exercitado).

### 17.3 `SmoothenRoadsConnectedToRectangle` e `SmoothenContainingRoadSegment` (binário)

`SmoothenRoadsConnectedToLot` (BV `0xa26cf4`) chama a vtable `0x50` do
gerente de ocupantes = `SmoothenRoadsConnectedToRectangle(rect)` (BV
`0x557a18`), entre as mensagens `0x0C2BD5B7` e `0x8C2BD74C`:

1. as ruas do retângulo (`GetOccupants(rect, 2)`) recebem a vtable
   **`0x5c(1)` da interface de rua, que só liga uma flag** (`+0x6c`,
   "visitada"; `0x64` a lê) — não é o `FlattenToTerrain` (o §16 inferia);
2. o retângulo crescido de 1 tile (preso ao tamanho do bairro, `+0x50/+0x54`):
   para cada rua não visitada, uma curva ou cruzamento (vtable `0x4c` =
   `IsAnIntersectionOrBend`) só fica visitada; uma reta tem o seu trecho
   alisado por **`SmoothenContainingRoadSegment`** (BV `0x5575c0`):
   - `CollectRoadOccupantsInSegment` (BV `0x556f40`): pelas ligações da rua
     (vtable `0x48(dir)` = bit `dir` do byte de ligações; direções 0 −x, 1 +y,
     2 +x, 3 −y, `DAT_02734bd0/bc0`), anda célula a célula nos dois sentidos
     do eixo enquanto há ligação e a rua da célula seguinte não é curva; o
     retângulo do trecho é a união das células (com ligações nos dois eixos,
     ou nenhuma, o trecho é a própria rua);
   - um par de vértices por passo ao longo do trecho (o vértice e o do outro
     lado da rua); fixo (com a altura do vértice, ou a do outro lado quando é
     ele que tem lote/rua visitada em volta) se há um lote (bairro vtable
     `0xec`) ou uma rua visitada nas 4 células em volta de um dos dois; as
     duas pontas fixas; os outros livres em ±**10** da altura;
   - `FUN_00553d90(1,1, …, 20)`: 20 vezes, para cada passo e cada vizinho, um
     degrau maior que **1,1** perde **0,3** do excesso de cada lado (dentro
     da faixa de cada um);
   - os dois vértices de cada par recebem o valor (terreno vtable `0x20`).

No código (`hood::flattenAbuttingRoads`, depois do nivelamento): isso, com o
lote novo contando como lote, e as ruas mexidas deitadas de novo no terreno
(0,4 sobre cada vértice; inferido que vem da mensagem do fim). Inferido: as
flags de visita começam limpas. `test_hood_moves` (uma lombada numa rua reta
ao lado do lote baixa e fica nivelada de um lado ao outro);
`hood-place-lot <cópia de N001> empty:0 82 2 2` alisa 2 trechos e o
`hood-lot-check` continua passando os 27 lotes.

### 17.4 A foto do rosto: o campo de visão e os óculos/chapéus (binário)

- **Campo de visão**: o da câmera do renderizador de miniaturas,
  `cTSUIThumbnailFactorySceneBased::InitRenderer` (BV `0xa819e0`): viewer
  vtable `0x210(20 × π/180)` = **20°** (`FLOAT_0274b6e8` = 20, `DAT_02ca94a4`
  = π/180, posto no inicializador estático `FUN_022dc76c`). O `SetupScene`
  multiplica por 1,0 nos humanos (`FLOAT_0274b470`) e por outro valor nos
  bichos do BV. A distância é d = max(0,01, maior lado da caixa / (2 ·
  tan(fov/2))) + 2 × a profundidade da caixa (0,30: os lados são as
  diferenças máx − mín), e os planos (viewer vtable `0x218`) **0,8 × o
  primeiro termo** (perto) e **1,2 × d** (longe) — os "0,8 e 1,2" que o §15.5
  lia como luz e cor.
- **Óculos e chapéus** (`CopyObjectsAtBone("glasses_grip")` com a flag 2,
  `"hat_grip"` com a flag 1; flags `0x3F` para humanos, `0x3FEA` para o
  resto): só no modo em que a foto é tirada do **objeto vivo** do Sim no lote
  (`this+0xa4 == 1`: `CopyPersonSkeleton` do nó de cena da pessoa, e então os
  objetos presos naqueles ossos); no modo do arquivo de personagem (o do
  bairro) não há objeto vivo e nada é copiado. Os óculos de rosto do CAS são
  sobreposições da malha do rosto e já saem na foto.

No código (`app/head_shot.cpp`): 20°, d = 0,30/(2·tan 10°) + 0,60, planos
0,8 e 1,2 como acima (antes: 30° e + 0,30, inferidos). As fotos do opensims2
só são feitas do arquivo de personagem (o painel do lote no bairro), então a
cópia dos objetos dos ossos não se aplica (não há foto do Sim vivo no lote).
`docs/screenshots/r14-bairro-cabeca-fov20.jpg`.

