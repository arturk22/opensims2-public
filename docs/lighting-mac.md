# Iluminação: o que o binário de Mac faz com `Lighting.txt`

Fontes e convenções como em `docs/shaders-mac.md` ("binário", "texto",
"inferido", "só BV"). Dados: `TSData/Res/Lights/Lighting.txt` (raiz),
`LotLights.txt`, `objects/*.txt`, `windows/windows.txt`, `LightRigs/*.txt`,
`CAS_*.txt`. Classes: `nTSSG::cLightingManager` (quase sem nomes no BV: o
`tools/symxfer` não alinhou esse objeto; os nomes abaixo vêm do
`baserom.sym` casados pelo conteúdo e são marcados com o endereço BV),
`cTSLightingState`, `cTSLightingParserState`, `nTSSG::cLerpLightSet`,
`nTSSG::cLightMapManager`, `nTSSG::cDiffusionManager`.

Resumo para o renderer:

1. **Só dia e noite** no jogo base: estado `day` das 7:00 às 18:59 e
   `night` das 19:00 às 6:59 (§2). `morning`/`evening` existem, mas
   `morningEvening` é `false` no `Lighting.txt`.
2. As luzes do estado (`sun`, `sky`, `fill1`, `fill2`, `simFill1..3` +
   `skylight`) **só iluminam o que está fora de casa** (sala 0). Objetos e
   Sims dentro de uma sala recebem as **luzes da sala**: um ambiente + N
   direcionais "Delta" calculadas da irradiância da sala em 6 direções (§4),
   que sai dos light maps (§4.1), não direto das lâmpadas.
   Paredes, pisos e terreno não usam luzes: usam os light maps (§5), uma
   difusão em grade de meio tile que divide a luz por `1 + k` a cada passo.
3. Cada luz tem uma máscara de grupo: as `simFill*` só iluminam Sims; com
   alguma `simFill` presente, `fill1`/`fill2` deixam de iluminar Sims (§3).
4. Na versão "WithAmbient" a cor de cada direcional é **subtraída** da cor
   do `skylight` (§3).

---

## 1. Os arquivos e o parser

`Lighting.txt` é um script do mesmo `nGZCommandParser` dos `.matShad`
(`set*`, `define`/`create`, `include`/`sinclude`). Comandos de
`cTSLightingParserState` (registro em `FUN_01ba4da4`, textos de ajuda no
binário):

| comando | argumentos |
|---|---|
| `lightingState nome [: pai]` ... `end` | bloco; herda tudo do pai |
| `control` | `-edith <bool>`, `-debugPortals <bool>` |
| `light nome (dir) (cor) [intensidade]` | `nome` ∈ `sun`=0, `sky`=1, `fill1`=2, `fill2`=3, `simFill1`=4, `simFill2`=5, `simFill3`=6 (tabela em `0x283fefc`, binário) |
| `skylight (cor) força` | bit 7 do estado |
| `fog (cor) perto longe` | bit 8 |
| `environment (cor) [força]` | bit 9 (luz `environmentCube`: escurece env cubes/partículas) |
| `particles (cor)` | bit 10 |
| `lightMap id -strength f -tint (cor)` | light map pré-calculado (bairro) |
| `transitionTimes maxIn maxOut` | tempos máximos de transição |
| `unlitRoomColor (cor) [-threshold f]` | cor de sala sem luz |
| `UIShade`, `flatShade (cor) f [-sims -objects -indoors]` | qualidade de luz baixa |
| `projectiveShadowStrength`, `portalSaturation`, `objectLightThreshold`, `diffParam`, `modifiers` | sombras, janelas, difusão |

`cTSLightingState` (binário, `TSCreateLightsFromLightingStateWithAmbient`):
`bitset<14>` em `+0xC` (quais campos foram dados); luz i em `+0x10 + i·0x1C`:
cor (3 floats), intensidade (`+0x1C`), direção (`+0x20`, 3 floats);
`skylight` em `+0x154`; `environment` `+0x160`; `particles` `+0x16C`; fog
cor `+0x1A8`, perto `+0x1B4`, longe `+0x1B8`.

Globais lidos por `UpdateFromConfigFile` (`FUN_007175b8`): `roomSaturation`,
`roomCeilingStrength`, `roomPortalStrength`, os parâmetros de sala (§4)
com prefixos `sim:`/`object:`/`wall:`, `thresholdLightsByArea`,
`portalSuppressionRadius/Amount`, `portalCornerSuppressionAmount`,
`portalSuppressionAffectsRoomLighting`, `hm*` (sombra do sol por height
map), `outdoorStrength`, `terrainStrength`, `wallDrivenPortals`,
`morningEvening`, `objectLightJitter`, `portalLightJitter`,
`spillPlaneOffset`, `shadowStrengthLimit`, `incrementalUpdates`,
`objectLightTransitionTime`, `doorOrArchDirectMultiplier`,
`minimalLightingDirectionalMix`, `lightRangeCutoff`,
`lightRadianceRangeCutoff`.

Luzes de objeto: as luzes vêm do próprio modelo (nós de luz do CRES,
`cPointLight`/`cSpotLight`/`cDirectionalLight` com nome, ex.
`lightingCeilingValue_light`); `objects/lightObjects.txt` e
`windows/windows.txt` só sobrescrevem atributos por nome
(`lightAttribOverride nome atributo valor`: `intensity`, `falloffRate`,
`color`, `coneAngle`, `conePenumbra`, `diffusionSource*`). As janelas têm
uma spot "portal" cada (`window..._light`).

## 2. Hora do dia → estado (binário, base 1.0 = BV)

`cTSSimulator` recalcula a fase a cada hora cheia (`SimulateOneTick` →
vtable `0xC8`, `FUN_004984d4`; no 1.0 `FUN_00b1fee8`) e a grava no global
SimAntics 4 ("Time Of Day"):

| hora | fase | estado (`morningEvening` false) | com `morningEvening` |
|---|---|---|---|
| 0–5 | 2 | `night` | `night` |
| 6 | 3 | `night` | `morning` |
| 7–17 | 0 | `day` | `day` |
| 18 | 1 | `day` | `evening` |
| 19–23 | 2 | `night` | `night` |

(`UpdateLightingState`, BV `FUN_00706408`, 1.0 `FUN_009ef630`.) O nome
passa por `SetLightingStateByName` (BV `FUN_00702280`, 1.0 `FUN_009ef740`),
que primeiro tenta `<nome>NoPortals` quando uma opção global está desligada
(`DAT_02c1bf1c`; inferido: as janelas como luzes desligadas na qualidade
baixa) e cai no nome puro; e liga as variantes de textura dia/noite
(`FUN_006fdec8`). No BV o dia também pode virar `overcast`/`spring`/
`autumn`/`winter` e a noite `vacationNight` (**só BV**). Para `day`/`night`
o BV também troca o céu refletido do skirt
(`skymap-generic-day-highres-envcube` / `skymap-generic-night-envcube`).
A troca é interpolada (`cLerpLightSet::UpdateLights(t)`, tempos de
`transitionTimes` e `objectLightTransitionTime`).

## 3. Luzes de fora (binário)

`AddOutdoorLights` (BV `FUN_0070cd3c`) chama
`TSCreateLightsFromLightingState` → `...WithAmbient`:

- `skylight` (bit 7) → `cAmbientLight` "LM_Outdoor_Ambient", cor = valor
  guardado em `+0x154`, escala 1.
- Cada `light` i (bits 0–6) → `cDirectionalLight` "LM_Outdoor_Dir_i":
  **cor = cor_i − cor_skylight**, escala difusa = intensidade_i,
  orientação = `−direção_i` (a direção do arquivo aponta da cena para a
  luz, Z para cima; a negação é binário). Fog/environment/particles viram
  luzes "LM_Fog"/"LM_Environment"/"LM_Particles".
- Sala = 0 (fora; `FUN_00bfdd60`: sala < 0 vira 0x8000) e máscara de grupo:
  `simFill1..3` (4–6) só o grupo dos Sims (`+0x4B8`); `fill1`/`fill2` (2–3),
  se houver alguma `simFill`, objetos + outros + extra (`+0x4BC|0x4C0|0x4C4`),
  sem Sims; o resto, todos. `fog` todos; `environment` máscara `+0x4CC`.
- Com sombras de height map ligadas (dia), o `sun` ganha uma luz extra
  "LM_sunHM" (sombra do terreno); fora isso "LM_OutdoorShade".

Não conferido: se o valor de `skylight` é guardado já × força (inferido:
`(1,1,1) 0.5` → 0,5; senão a subtração daria cores negativas no `day`).
Rodada 15: `TSCreateLightsFromLightingStateWithAmbient` (BV `0xd741dc`) e
`FindIllumFromLightingState` (`0x702ae0`) usam a cor de `+0x154` e as
direções `+0x20 + i·0x1C` como estão (escala 1 no ambiente, nenhuma
normalização); o que o comando do parser guarda (força multiplicada?
direção normalizada?) depende do tratador de `skylight`/`light`, que não
foi achado (o registro dos comandos fica em `0x1ba4e80…`; o de `modifiers`
é `0x1ba4da4`).

## 4. Luzes das salas (binário)

`AddRoomLights` (BV `FUN_00710a3c`): para cada sala ≠ 0, três conjuntos de
luzes, cada um com a sua afinação (`cTSRoomLightTuning`, 0x3C bytes) e
máscara: **objetos** (`+0xD0`, máscara `+0x4BC`), **Sims** (`+0x10C`,
`+0x4B8`) e **paredes/outros** (`+0x148`, `+0x4C0`); e uma luz
`environment` da sala ("LM_Room%d"). Campos da afinação
(`UpdateFromConfigFile`; prefixo `sim:`/`object:`/`wall:` sobre o valor
sem prefixo):

| off | nome | `Lighting.txt` (sem prefixo / sim / object / wall) |
|---|---|---|
| 0x00 | `roomLightingType` (int) | 4 |
| 0x04 | `roomOverallStrength` | 0,4 / 0,8 / 0,5 / 0,2 |
| 0x08 | `roomBounceStrength` | 0,25 |
| 0x0C | `roomBounceCrossover` | 1 |
| 0x10 | `roomSideStrength` | 1,2 |
| 0x14 | `roomSideCrossover` | 1 |
| 0x18 | `roomSideSlope` | 0,75 |
| 0x1C | `roomSpecular` | 0,8 |
| 0x20 | `roomFromLightMapMix` | – / 0 / 0 / 1 |
| 0x24 | `roomFromLightMapStrength` | – / 2 / – / 3 |
| 0x28 | `roomDirectionalBoost` | – / 4 / 4 / 1 |
| 0x2C | `roomCrossBias` | 0 |

`roomLightingType` escolhe o construtor (`FUN_007109bc`): 0 →
`FUN_0070f718`, 1 → `FUN_0070e950`, 2–8 → `FUN_0070daa8` com N = tipo
luzes direcionais. Com 4 (o do jogo), `FUN_0070daa8`:

```
I[0..3] = irradiância da sala nos 4 lados, I[4], I[5] = cima/baixo (cores RGB)
S = I0+I1+I2+I3
lados  i=0..3: L_i = (I_i + SideCrossover·I4)·SideStrength·Overall
vert.  j=4,5:  L_j = (I_j + BounceCrossover·S)·BounceStrength·Overall
A = (L0+…+L5)/6 ;  L_k −= A
ambiente: FromLightMapMix = 0 → cAmbientLight cor A, escala 1
          senão → luz de ambiente do light map (mix, FromLightMapStrength·0,25)
repetir N vezes:
   d = direção com maior max(R,G,B) de L (N < 3: só os lados)
   dir = tabela[d]; se lado: dir −= SideSlope·V        (V inferido = (0,0,1))
   cDirectionalLight "Delta<k>": cor L_d, escala DirectionalBoost,
                                 especular relativa roomSpecular
   L_d = 0
   (k = 0 e CrossBias > 0: ajusta o par oposto)
```

**Tabela das 6 direções** (binário: `DAT_02c1bf2c`, preenchida no
inicializador estático `FUN_020e6a90` com os vetores unitários e o negador
`FUN_0017267c`): `0 = +X`, `1 = −X`, `2 = +Y`, `3 = −Y`, `4 = +Z`, `5 = −Z`.
`V` = `(0, 0, 1)` (`DAT_02c1c874`). O vetor é a direção **em que a luz
viaja** (mesma convenção das luzes de fora, que usam −direção do arquivo):
`I[0]` é a luz que anda para +X (vem das paredes do lado −X), e a Delta
dos lados aponta `tabela[d] − SideSlope·(0,0,1)`, isto é, para o lado e
para baixo. `I[4]` (+Z) é a luz que sobe do piso, `I[5]` (−Z) a que desce
do teto (§4.1). Todas essas luzes recebem o ID da sala
(`SetRenderGroupIDAndCompareMode`): **só objetos daquela sala as recebem**.

### 4.1 De onde vem `I`: o light map, não as lâmpadas (binário)

`GatherRoomLighting` (BV `FUN_007147c8`) **não soma as lâmpadas**: lê a
luz já difundida nos light maps (§5) de cada cômodo e as janelas. Cada
cômodo tem um `cRoomLightingDesc` (vetor em `+0x1AC` do gerenciador):
`I[6]` (cores, `+0x00`), pesos `w[6]` (`+0x48`), contadores em `+0x60..`,
e a lista de luzes de portal (`+0x7C`).

```
para cada cômodo: I[k] = 0, w[k] = 0
GatherFloorLighting (FUN_00714258), cada quarto de tile do cômodo:
   c   = difusão do piso naquele ponto (light map, §5)
   alb = Mix(branco, cor do piso, roomSaturation)        (roomSaturation 0,5)
   I[4] += 0,25·c ⊙ alb ;  w[4] += 1                      (luz que sobe do piso)
   I[5] += 0,25·c · roomCeilingStrength ;  w[5] += 1      (teto, 1)
GatherWallLighting (FUN_00713ca0), cada lado de parede do cômodo:
   c   = difusão da parede (light map) · 0,5
   alb = Mix(branco, cor do papel de parede, roomSaturation)
   n   = normal do lado (dy, −dx)/|…| (aponta para dentro do cômodo)
   n.x > 0: I[0] += n.x·c⊙alb, w[0] += n.x ;  n.x < 0: I[1] += |n.x|·c⊙alb, w[1] += |n.x|
   n.y > 0: I[2] += …,          w[2] += n.y ;  n.y < 0: I[3] += …,           w[3] += |n.y|
FUN_0070010c, cada luz de portal (janela/porta) do cômodo:
   cor = cor do portal · roomPortalStrength (0,75); (dx, dy) = direção do portal
   mesmo rateio por sinal em I[0..3]/w[0..3] no cômodo de destino
depois: I[k] /= w[k] onde w[k] ≠ 0     (média ponderada)
```

A cor do piso/papel é a cor média da textura do material (consulta por
nome, `FUN_000c82f8` → vtable `0xC`). Daí `AddRoomLights` (§4) monta o
ambiente e as Delta. Conferido em BV; no 1.0 só se conferiu que os mesmos
nomes de configuração existem.

As lâmpadas entram na luz dos objetos por dois caminhos: como **luzes
práticas** (a `cPointLight`/`cSpotLight` do modelo, que ilumina objetos e
Sims diretamente, com `modifiers insideObjects` *practical* na intensidade
e *practical falloff* no `FalloffRate`; inferido pelo comentário do
`Lighting.txt` e pela estrutura de `FUN_0070ae3c`, que faz o mesmo com as
janelas) e como **fontes de difusão** (escala *diffusion* e queda
*diffusion falloff*, §5), que chegam a `I` pelo light map.

Janelas e portas (`SetAndAddPortalLights`, BV `FUN_0070ae3c`): para cada
portal, `FindPortalLightValues` (`FUN_0070a884`) dá cor e as quatro forças
do `modifiers insidePortals/insidePortalSpills/outside*`; a luz prática é
uma spot "LM_PortalDirect" com intensidade `jitter · intensidade ·
practical · supressão` e queda `falloff · practicalFalloff`, mais a
"LM_PortalSpill"; e o portal vira fonte de difusão com cor × *diffusion* e
queda × *diffusion falloff*. A supressão vem de `FindPortalSuppression`
quando `portalSuppressionAmount > 0` (fórmula do comentário do
`Lighting.txt`: `1 − (1 − r/R)·f`; não conferida). `portalLightJitter` 0
→ fator 1.

## 5. Light maps de parede, piso e terreno: a difusão (binário)

`cLightMapManager` mantém as texturas `floorLightMap_<página>`
(`floorTextureSize` 512), `wallLightMap_<página>` (tiles de parede 8×24
texels: `wallTileSizeX/Y`) e os mapas de incidência. **Página 0 do piso =
o lote**: texels por tile = `tamanho >> 6` (8 com 512, a página cobre 64
tiles); o tile (x, y) ocupa `u ∈ [8x, 8x+8)`, `v ∈ [512 − 8y − 8, 512 − 8y)`
(V invertido) (`FloorTileLightMapTexels`, BV `0x6f65f8`). As outras
páginas (andares de cima) guardam tiles soltos numa tabela.

`cDiffusionManager` (BV `0xd7cad4..0xd7fdc0`) calcula a luz por **grades
de difusão**: uma `cFloorColorGrid` por andar e o `cWallDiffusionManager`
para as paredes. A cada atualização, `DoDiffusionPass` (BV `0xd7dd80`):
limpa cada grade, semeia as luzes, propaga, copia para os light maps, e
depois faz o mesmo nas paredes.

### 5.1 A grade do piso

Amostras em **meio tile** (grade `2·largura × 2·altura` do lote); cada
amostra tem duas metades (os dois triângulos de um tile cortado na
diagonal, `+0x00` e `+0x24`), cada uma com cor RGB, ID da sala (`+0x20`),
"existe" (`+0x1B`), "de fora" (`+0x1A`) e os bits de vizinhança (`+0x18`:
para quais das 8 direções a luz passa, isto é, sem parede no caminho;
`+0x19`: qual metade do vizinho).

1. **Limpa** (`FUN_00eeb8a4`): dentro = `insideAmbientColor ·
   insideFloorAmbient`; fora = `outsideDiffusionAmbientColor ·
   outsideFloorAmbient` (do estado: `diffParam`; dia 0,001 dentro, 0,25
   fora).
2. **Fontes** (`cDiffusionLightSource`, montada de cada luz de objeto com
   `cDiffusionLightExtension`, `FUN_00d82498`): cor = `diffusionSourceIntensity
   · diffusionSourceColor`; `diffusionSourceFalloff` (`+0x38`),
   `...DirectionalFalloff`, `...WallIntensityScale` (`+0x3C`),
   `...FloorIntensityScale` (`+0x40`), `...InterFloorFalloff` (`+0x44`),
   `...IsDirectional`; retângulo `diffusionSourceMin/Max` (em tiles,
   relativo à luz) girado pela direção do objeto e somado à posição
   (`FUN_00d82334`); sala = a do ponto um pouco à frente do objeto. Esses
   valores vêm da extensão da luz no modelo e dos `lightAttribOverride` de
   `objects/*.txt` e `windows/windows.txt`.
3. **Semeia e propaga** (`FUN_00eeccc0`, por grade): pega as fontes do
   andar da grade ou acima, que tocam uma amostra da sua sala
   (`FUN_00eeba10`), **ordena pela queda** (`+0x38`, decrescente) e trata
   juntas as de queda igual (|Δ| < 0,01):
   - semente (`FUN_00eebdec`): em cada amostra do retângulo **da mesma
     sala**, `cor = max(cor, corFonte · FloorIntensityScale)` por canal;
     fonte em andar acima: × `(1 − InterFloorFalloff)` por andar, e só se
     o piso deixa passar;
   - propagação (`FUN_00eec3a4`), até `floorPropagationSteps` (50)
     passadas ou até nada mudar: cada passada varre o retângulo ativo nos
     4 sentidos, e cada amostra puxa das vizinhas que mudaram na passada
     anterior, pelas 8 direções (`d`: 0 (0,−1), 1 (−1,−1), 2 (−1,0),
     3 (−1,1), 4 (0,1), 5 (1,1), 6 (1,0), 7 (1,−1); `DAT_02747cb8`):

```
k = falloff_da_fonte · floorThreshold          (diffParam floorThreshold: dia 0,4, noite 0,2)
diagonal (d ímpar): k ·= 1,4
por canal:  se (viz − cor) > k·cor:  cor = viz / (1 + k) + 0,01   (FUN_00ee7c6c)
```

   Ou seja: cada passo de meio tile divide a luz por `1 + k` e a luz não
   atravessa paredes (só segue os bits de vizinhança).
4. **Cômodos escuros** (`BrightenRoom`, BV `0xd7d028`): soma
   `0,5 · unlitRoomColor` em todas as amostras (piso e paredes) do cômodo.
   Quem decide quais cômodos (`FindUnlitRooms`/`BrightenUnlitRooms`, com
   o `-threshold`) não foi decompilado.
5. **Cópia** (`FUN_00eea560` → `FUN_00ee9874`/`FUN_00ee95d0`): por tile,
   as 4 amostras dos cantos (amostra vazia = o ambiente de dentro/fora)
   viram os texels do tile por interpolação; metade diagonal à parte.

### 5.2 Paredes

`cWallDiffusionManager` (BV `0xee6000..0xef5000`, sem nomes): mesmos
parâmetros com prefixo de parede (`insideWallAmbient`, `outsideWallAmbient`,
`wallThreshold`, `wallPropagationSteps`). Ordem em `DoDiffusionPass`:
limpa cada parede (`FUN_00eee87c`, ambiente por parede); semeia as fontes
da mesma sala com `WallIntensityScale` e `k = wallThreshold · falloff`,
atenuada por andar como no piso (`FUN_00ef28ac` → `FUN_00ef2594`, a
`AddDirectLight`); traz o piso para a parede (`FillWallsFromFloors`,
`FUN_00ef0e68`: `brightWallFloorContribution` 1 para paredes com luz
direta, `darkWallFloorContribution` 0,5 para as outras); propaga; escurece
cantos junto de paredes escuras (com uma opção global); copia para as
páginas de parede. A fórmula da propagação na parede não foi lida (deve
ser a mesma do piso; não conferido).

### 5.3 Fora de casa: terreno e pisos ao ar livre (binário)

O lado de fora tem **duas camadas somadas** no light map de piso:

1. **A difusão** (§5.1) cobre também as amostras de fora: limpas a
   `outsideDiffusionAmbientColor · outsideFloorAmbient` ((1,1,1)·0,25) e
   semeadas pelas lâmpadas que estão fora (mesma regra: semente na sala da
   fonte, que fora é a sala 0, e propagação com `k = falloff ·
   floorThreshold`, só parando em parede). As fontes de fora usam
   `modifiers outsideObjects` (*diffusion*, *diffusion falloff*): de dia
   `0 0`, à noite `0,6` e `0,65`. **As poças de luz em volta das lâmpadas
   de jardim vêm daqui.**
2. **A luz direta do estado**, somada por cima (`AddToARGB32` soma no
   texel que a difusão deixou) por `AddDirectFloorLighting` (BV
   `0x708ebc`), que percorre os tiles de piso **da sala 0** (o gerenciador
   pede ao mundo os tiles do cômodo 0):

```
E(n) = FindIllumFromLightingState(n)                    (BV 0x702ae0)
     sem lightMap no estado (o caso de day/night):
       w_i = max(0, n · dir_i) · intensidade_i,  i = sun, sky, fill1, fill2 (as 4 primeiras)
       E   = (1 − Σ w_i) · skylight + Σ w_i · cor_i
     com `lightMap id` (só os estados de bairro): E = texel do mapa
       ambiente na posição ((n.x+1)/2, (n.y+1)/2) × a cor da luz guardada
texel += E(n) · terrainStrength (1) · escala do light map de piso
         (cLightMapManager vtable 0x5C(1))
```

   `n` é a normal do terreno, interpolada bilinearmente entre as normais
   dos vértices do tile a cada texel (`LightTerrainTile`, BV `0x704274`;
   meio tile `0x703de8`); um tile com piso ou fora do térreo usa `n` =
   (0,0,1) constante (`FUN_00700368` ou a soma direta). Com sombras por
   height map (`+0x6C`), as variantes "WithShadows" (BV `0x705068`,
   `0x702fd8`; "AlphaShadows" `0x705850`, `0x703740`, opção global
   `DAT_02c1beb0`) fazem o mesmo e põem a **sombra do sol no alfa** do
   texel: `alfa = 255 · min(1, s · hmSunLightTransitionZone)` com `s` a
   amostra do height map de sombra (terreno) ou a altura da sombra acima
   do tile (pisos).

A escala do light map de piso (`cLightMapManager` vtable `0x5C`, tipo 1,
BV `0x6f5350`) é o campo `+0x68`, que o construtor (BV `0x6fa6e0`) põe em
**0,5** (o de parede, tipo 0, `+0x6C`, também 0,5); `+0x1a8` do gerenciador
de luz multiplica junto (`terrainStrength`, inferido pelo nome lido no
`UpdateFromConfigFile`). Então `texel += E(n) · terrainStrength · 0,5`: com
o `2·textura·light map` do shader, a luz direta chega à tela como
`textura · E`. Não há a subtração do skylight que as luzes de objeto de
fora usam (§3).

A difusão é copiada sem escala (`CopyDiffusionToLightMaps` passa a mesma
escala 0,5, e `FUN_00ee9120` multiplica por `2·escala` antes do
`PackToARGB32`, que leva [0, 1] a [0, 255]), mas, com a flag `+0x40` da
grade ligada, as amostras de fora (`+0x1A`) **perdem o ambiente de fora**
antes da interpolação (`FUN_00ee9874`, `FUN_01808a08` = subtração de
`outsideDiffusionAmbientColor · outsideFloorAmbient`; amostra inexistente
vale o ambiente de fora ou de dentro). É o que o comentário do
`Lighting.txt` diz ("If auto-lighting is active, this has no absolute
effect, but it does factor into how quickly lights fall off"): o 0,25 só
molda a queda das poças. Inferido: a flag `+0x40` ligada no térreo (não se
achou quem a escreve). Não foi conferido se o parser normaliza a direção das
`light` (com `sun (1,5, 1,2, 2)` sem normalizar, Σw passaria de 1 de dia).

**Quem é "fora"**: no binário, os tiles da sala 0 (`AddDirectFloorLighting`
pede os tiles do cômodo 0; o render group de sala < 0 vira 0x8000). Os
lotes guardam pátios e a faixa da borda como salas com ID próprio no
`3ARY` (o que o código achou); como o jogo junta essas salas à sala 0 não
foi achado (as funções do `cRoomManager` não estão nomeadas no BV).

### 5.4 Janelas e portas: `FindPortalLightValues` (binário, BV `0x70a884`)

Para cada portal (`cPortalLight`: sala do outro lado `+8`, direção
`+0xC/+0x10`, supressão `+0x14`, posição `+0x18`):

```
outro lado é uma sala (≠ 0): modificadores insidePortals / insidePortalSpills
senão:                       outsidePortals / outsidePortalSpills
   (4 floats cada: practical, diffusion, practical falloff, diffusion falloff;
    practical × supressão)
direta = 0, f_direta = 1 ; spill = 0, f_spill = 0
iluminação ligada:
  portal de um cômodo para fora (lado 0):
     sem skylight no estado: direta = E(−direção do portal)
     com skylight: f_direta = max(0, sol · direção do portal) ;
                   direta = cor_sol · intensidade_sol ; spill = skylight ; f_spill = 1
                   portalSaturation < 1: f = (f − 1)·sat + 1 ; cores = lerp(E(−dir), cor, sat)
     direta += L ; spill += 0,5·L    (L = light map de parede fora, na posição do portal)
  outro caso: direta = 0,5·L ; com skylight: spill = direta, f_spill = 1
cor do portal (+0x30) = direta·f_direta + spill·f_spill
   (× supressão se portalSuppressionAffectsRoomLighting; porta/arco: f_direta × doorOrArchDirectMultiplier)
```

Essa cor alimenta a "LM_PortalDirect"/"LM_PortalSpill" (§4.1), a fonte de
difusão do portal e o rateio de `I[0..3]` (× `roomPortalStrength`).

Lido no BV (`SetAndAddPortalLights` → `FindPortalLightValues(gerenciador,
sala, portal, ...)`): os portais ficam na lista de cada cômodo (`+0x7C` do
`cRoomLightingDesc`), e "lado 0" é **a sala dona da lista** (`param_2 == 0`)
com o outro lado (`+8`) ≠ 0: a luz de fora entrando num cômodo. Os
modificadores escolhem-se pelo **outro lado**: ≠ 0 → `insidePortals(Spills)`,
0 → `outsidePortals(Spills)` (à noite 4 4: a luz de um cômodo saindo pela
janela faz poça no gramado). As luzes vão para o grupo da sala do outro
lado; a prática "LM_PortalDirect" tem intensidade `jitter · intensidade da
luz · practical · f_direta` e cor `cor da luz ⊙ direta`; a fonte de difusão
tem cor `cor da fonte ⊙ direta · diffusion · f_direta` e queda × *diffusion
falloff*, e outra igual para o spill com os modificadores `*Spills`. Cada
janela/porta tem uma luz por lado no modelo (`..._n_light`/`..._s_light`).

## 6. Luzes de objeto: `FalloffRate`, alcance e cone (binário)

`cStandardLightBase`: cor (`+0x50`), `DiffuseScale` (`+0x48`),
`FalloffRate` (`+0x60`), alcance (`+0x64`), cone (`+0x68`/`+0x6C`).
**`FalloffRate` é a própria constante `C`** da atenuação
`I / (1 + C·d²)`; o alcance é onde ela chega ao corte
(`cPointLight::OnLightRadiantPropertiesChanged`, BV `0xca0e48`; spot em
`0xca0ab8`):

```
I = |DiffuseScale · cor|            (norma do vetor RGB)
c = lightRadianceRangeCutoff = lightRangeCutoff do Lighting.txt (0,275; sem ele 0,5)
I/c ≤ 1 → alcance 0 ;  C ≤ 0 → alcance 10000
senão     alcance = sqrt((I/c − 1) / C)
spot: ângulo e penumbra em radianos, valor absoluto, a soma presa num
      máximo; cos externo = cos((ângulo + penumbra)/2), interno = cos(ângulo/2)
```

Change Light (`docs/primitives-mac.md`) muda a intensidade em % do nível
de cada luz do modelo; o renderizador faz o fade em
`objectLightTransitionTime`.

## 6.1 Cômodos sem luz: `FindUnlitRooms` / `BrightenUnlitRooms` (binário)

Endereços no BV (achados pela ordem das funções do `baserom.sym`, que o
symxfer não transferiu): `FindUnlitRooms` `0x701a28`, `BrightenUnlitRooms`
`0x707820`, o preenchimento de meio tile `0x700368`, a soma das luzes da
sala `0x70009c`/`0x700710`. Lidos de `unlitRoomColor (r, g, b) -threshold t`
do estado de luz (`Lighting.txt`: `(.3,.275,.275) -threshold .33`, ...;
o arquivo avisa "do not set threshold to 0").

```
FindUnlitRooms (só se a cor ≠ preto e a difusão está ligada, +0x54):
  para cada sala r ≠ 0:
      L = média das 6 cores de luz da descrição da sala (soma / 6)
      m = maior componente de L
      f[r] = m < t ? 1 − m/t : 0
  (t ≤ 0: f = um valor denormal ≈ 0 para as salas sem luz própria e 0 para
   as que têm: sem efeito visível — daí o aviso do arquivo)
  para cada sala com f > 0: soma cor · f às 5 primeiras cores de luz da
  descrição da sala (o que ilumina os objetos lá dentro)
BrightenUnlitRooms (se alguma sala tem f > 0):
  piso: para cada tile de piso da sala, soma cor · f · escala do light map
        de piso (vtable 0x5C(1)) a cada texel do tile (AddToARGB32, satura);
        tiles diagonais (tipos 3, 6, 12) só no lado da diagonal que é da
        sala (teste x·(±1) + y contra 0,99/−0,01 em coordenadas do tile)
  paredes: para cada lado de parede cuja sala tem f > 0 (o lado e o oposto,
        OppositeWallLocator), soma cor · f · escala do light map de
        parede (0x5C(0)) a todos os texels daquele lado
```

Ou seja, é um **piso de luz por sala**, somado depois da difusão, que some
quando a sala já tem luz ≥ t (no componente mais forte).

## 6.2 Luz direta nas paredes: `AddDirectWallLighting` (BV `0x708590`)

Para cada lado de parede (e o oposto) que dá para **fora** (sala 0) ou é do
tipo 3:

- calcula a iluminação externa no ponto do lado (`FindExternalIllumination`
  com a normal do lado, BV `0x70465c`: sol + céu do estado de luz) e a
  multiplica pela escala do light map de parede;
- com sombras ligadas (`+0x6C` e `+0x6D`) e o lado virado para o sol
  (sol · normal > 0): `LightWallTileWithShadows` (BV `0x704d54`), que
  sombreia texel a texel; senão soma a mesma cor a todos os texels do lado;
- parede do tipo 3 com sala do lado: pula as primeiras `3 · largura` linhas
  de texels do lado (só a parte de cima recebe a luz direta; inferido: muro
  baixo/meia parede).

## 7. O que o código atual faz diferente (branch `r6-lighting`: `src/formats/lot_lightmap.cpp`, `src/app/lot_lighting.cpp`)

| tema | jogo | `src/` hoje |
|---|---|---|
| terreno e pisos fora | difusão de fora (0,25 + lâmpadas de fora com `outsideObjects` *diffusion*) menos o ambiente de fora, **mais** `E(n)·terrainStrength·0,5` (sun, sky, fill1, fill2), sombra do sol no alfa (§5.3) | igual (direções normalizadas, inferido); `n` = gradiente das elevações no térreo sem piso, (0,0,1) nos pisos; sem a sombra do sol (alfa 255); luz de fora acima de um andar sem piso cai para o de baixo (inferido) |
| quem é "fora" | sala 0 do mundo (§5.3) | sala 0 ou sala em que metade dos quartos de tile não tem piso (inferido: os pátios do `3ARY`) |
| portais | por sentido: de fora para um cômodo `sol·max(0, sol·fora) + skylight + L` (+ spill 0,5·L), de um cômodo `L`; modificadores pelo outro lado (§5.4) | igual; a parede do portal é a amostra vizinha mais próxima que a luz não atravessa, `L` = light map do lado de onde vem depois da difusão das lâmpadas; sem a supressão entre janelas nem as práticas das janelas |
| irradiância do cômodo | piso e paredes × cor do material dessaturada por `roomSaturation` (§4.1) | igual: cor média da textura (`stdMatBaseTextureName`) do piso de cada quarto de tile e do papel de cada face |
| lâmpadas nos objetos | luz prática direta (`insideObjects`/`outsideObjects` *practical*) além da luz do cômodo (inferido) | `LightSet::points`: até 12 por sala (as mais fortes), `I/(1 + C·d²)` até o alcance de §6, por pixel |
| hora → estado | `UpdateLightingState` a cada hora (§2) | pelo relógio da VM, também durante os ticks das capturas `--run-vm` |
| difusão, `I[k]`, Delta, `unlitRoomColor` | §4–§5 | segue o doc |

## 8. O que falta

- O `thresholdLightsByArea` (a string existe; o uso não foi achado).
- A propagação nas paredes e a escrita dos texels de parede; a cópia
  exata das amostras do piso para os texels (interpolação).
- Os valores padrão da `cDiffusionLightExtension` (quando o modelo e os
  `lightAttribOverride` não dão um campo) e o leitor dela no CRES.
- A supressão entre janelas (`FindPortalSuppression`) e a "LM_PortalSpill".
- Como as salas de pátio do `3ARY` viram a sala 0 (§5.3); se o parser
  normaliza a direção das `light`.
- Luzes práticas: confirmar no binário que `modifiers insideObjects`
  multiplica a intensidade e o `FalloffRate` (`UpdateObjectLighting`).
- O comando `skylight` (se guarda cor × força).
- O height map de sombra (`UpdateHeightMapShadows`, `AddBlobToHeightMap`):
  a sombra do sol dos objetos de fora (estilo 3 do `cShadow`, §9) nos lotes.
- Quem lê o `projectiveShadowStrength` e a cor/direção de sombra do
  gerenciador de luz (vtable `0x78`/`0x7c`, §9).

**Rodada 12 (escolha visual, não do binário):** no app, a luz direta do
estado sobre o chão (terreno e pisos de fora) é puxada para o cinza quando é
fria, na proporção (B − R)/B: à noite E ≈ (0,32; 0,32; 0,8) virava calçada
lavanda forte com o `2·textura·light map`; de dia (luz quente) nada muda.

## 9. Sombras projetadas: `nTSSG::cShadow` (binário, rodada 15)

Dentro de casa **não há sombra de luz de cômodo, de lâmpada, de portal nem
nos light maps**: a difusão (§5) só para em paredes, e nenhuma luz de
objeto projeta sombra. O que escurece o chão em volta das coisas é o
`cShadow` de cada objeto (`cObject::CreateShadow`, BV `0x6549f0`), com um
de cinco estilos (`GetShadowStyleFromEdithMode` `0x65480c`,
`GetDesiredShadowStyle` `0x65492c`, `UpdateCurrentStyle` `0x678710`):

| estilo | quem | o que é | opção (`Graphics Rules.sgr`, bit do `+0x2c`) |
|---|---|---|---|
| 0 | — | nenhuma | — |
| 1 | **Sims** (tipo de objeto 2) | silhueta renderizada numa textura e projetada no chão | `simShadows` (Shadows `$High`) |
| 2 | objetos | os planos de sombra do modelo (`groundshadow`, `*wallshadow`: "GUOB", `InitGUOBs` `0x6747c8`) | `guob` |
| 3 | objetos de fora | a sombra do sol no height map (§5.3) | `heightMapShadows` |
| 4 | objetos com `+0x102 == 3` | como o 1, grupo "Props" | `objectShadows` |

O 2 e o 3 já existiam no código (planos do modelo; height map só no
bairro). O **estilo 1** (`cShadow::CreateShadow` `0x677390`,
`UpdateInsideOutsideSettings` `0x67538c`, `UpdateShadowLocation`
`0x676290`, `UpdateVisibility` `0x674260`, `OncePerFrameUpdate`
`0x678ba0`):

- Uma textura `shadowTexture%x` de `shadowResolution` × `shadowResolution`
  e um viewer ortográfico próprio que só desenha o grupo de render do
  Sim; os materiais desenham a silhueta na passada `kRenderTypeShadow`
  (`SimShadowDX7`/`DX8ShadowRendering`: cor `stdMatShadowColor` = (0,63,
  0,63, 0,85) do `sims.matShad`) sobre branco.
- Ajustes do gerenciador (lidos em `0x6f2840` com estes nomes; padrões em
  `0x6f3d30`): `shadowSize` 1, `shadowCameraDistance` 10,
  `indoorShadowBlurRadius` 3, `outdoorShadowBlurRadius` 2 (texels; o
  `shadowBlurRadius` os substitui), `shadowResolution` 128,
  `baseShadowResolution` 128, `shadowW0` 0,7, `shadowH0` −1, `shadowW1`
  0,7, `shadowH1` 2 (tiles). Nenhum arquivo do jogo os muda.
- **Dentro** (`IsOutside` falso): o viewer olha **para baixo** (o eixo Z,
  `DAT_02c0bbf8` = (0, 0, 1) negado), girado com o Sim, cobrindo ±W0 ×
  ±W1; a silhueta vai para um quad (`ShadowShader`, camada
  `simShadowLayer`·4 = 4, depois do opaco) na altura dos pés + 0,005, que
  **multiplica** o que já foi desenhado pela textura (`srcFactor(destColor)
  dstFactor(zero)`, sem escrita de z); o quad mostra só o miolo
  `(resolução − 2·blur)/resolução` da textura (a margem do borrão).
- **Fora**: o viewer olha ao longo de `−(direção de sombra do gerenciador
  de luz)` (vtable `0x78` do `cLightingManager`; inferido: a do `sun`),
  com a caixa ±W, alturas H0..H1 projetadas; a silhueta vai para
  sobreposições no **piso e no terreno** (`+0x48`/`+0x4c`,
  `OverlayShadowMaterial`: a sombra inteira na cor de sombra onde o alfa
  da textura passa de 100/255, a cor vem da vtable `0x7c` do gerenciador),
  e o quad de dentro fica escondido.
- `projectiveShadowStrength` (0,73 de dia, 0,4 à noite) é guardado pelo
  `cTSDiffusionParams`; quem o lê não foi achado.

**No código** (`Renderer::ShadowCaster`, `LotLighting::update`): cada Sim
na VM com a opção ligada (`GraphicsOptions::simShadows`, Shadows `$High`)
desenha a silhueta num atlas de 8 × 8 texturas de 128, como acima, e os
pisos e o terreno (as superfícies com light map voltadas para cima) a
uma altura entre 0,6 abaixo e 0,3 acima dos pés multiplicam a cor pela
silhueta borrada (dentro) ou pela cor de sombra onde ela cobre (fora).
Inferido: a cor de fora, `1 − força·(1 − (0,33, 0,33, 0,39))` com a força
= `projectiveShadowStrength` e a cor escura que o `sims.matShad` deixa
comentada; a direção de fora = a do `sun` do estado; o borrão como uma
caixa de 5 × 5 amostras bilineares; projetar nas superfícies em vez de
num quad plano (o quad do jogo flutua sobre um chão mais baixo, ex. o Sim
na escada); os estilos 3 e 4 nos lotes ainda faltam. Captura:
`docs/screenshots/r15-fov-sombras-sims.jpg`.
