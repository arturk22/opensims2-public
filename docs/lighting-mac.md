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
   direcionais "Delta" calculadas da irradiância da sala em 6 direções (§4).
   Paredes, pisos e terreno não usam luzes: usam os light maps (§5).
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

A tabela das 6 direções é montada num inicializador estático (`0x20e6a90`,
não decodificado). Todas essas luzes recebem o ID da sala
(`SetRenderGroupIDAndCompareMode`): **só objetos daquela sala as recebem**.

De onde vem `I`: `GatherRoomLighting`/`AddLightsToRoom*` somam as luzes de
objeto da sala (lâmpadas, com o nível do Change Light), as janelas
(`modifiers insidePortals`/`insidePortalSpills`: luz de fora entrando,
"LM_PortalDirect", "LM_PortalSpill") e `unlitRoomColor` quando a sala fica
abaixo do `-threshold`. **Não decompilado** (BV `FUN_0070ae3c`,
`FUN_007149fc` e vizinhos).

## 5. Light maps de parede, piso e terreno (nomes + texto)

`cLightMapManager` mantém as texturas `floorLightMap_<página>` (512²,
`floorTextureSize`), `wallLightMap_<página>` (256², tiles de parede 8×24
texels: `wallTileSizeX/Y`), e os mapas de incidência (`*IncidenceMap_*`,
direção da luz para o bump). A página 0 de piso cobre o lote (o terreno
usa UV = posição/64). `cDiffusionManager` propaga a luz numa grade por
cômodo (`floorPropagationSteps`/`wallPropagationSteps` 50), com fontes das
luzes de objeto (`diffusionSourceIntensity`, `...WallIntensityScale`,
`...FloorIntensityScale`, `...IsDirectional`, `...Falloff`), das janelas e
do ambiente de fora (`outsideFloorAmbient`/`outsideWallAmbient` 0,25,
`outsideDiffusionAmbientColor`) e de dentro (`diffParam
insideAmbientColor/insideFloorAmbient/insideWallAmbient`, limiares
`floorThreshold`/`wallThreshold`). `LerpLightMaps` interpola na troca de
estado. O algoritmo da difusão não foi decompilado.

## 6. Luzes de objeto e Change Light

`cStandardLightBase` (binário, nomes): cor, `DiffuseScale`,
`RelSpecularScale`, `FalloffRate`, `Range` (calculado por `ComputeRange`),
`ConeAngle`, `ConePenumbra`. No vertex shader (ver `docs/shaders-mac.md`
§4.2) a atenuação é `1/(1 + C·d²)` com corte em `d² ≤ alcance²`; `C` sai
de `FalloffRate` e o alcance do `lightRangeCutoff` 0,275 (inferido:
alcance onde a atenuação cai a 0,275). Change Light
(`docs/primitives-mac.md`) muda a intensidade em % do nível de cada luz do
modelo; o renderizador faz o fade em `objectLightTransitionTime`.

## 7. O que o código atual faz diferente

| tema | jogo | `src/` hoje |
|---|---|---|
| hora → estado | `day` 7–18 h, `night` 19–6 h | estado fixo escolhido em `main.mm` |
| luzes de fora | só sala 0; cor − skylight; `simFill*` só Sims | todas as luzes do estado em todos os objetos, sem subtrair o skylight; `objectLights()` tira as `simFill*` de todos |
| dentro de casa | ambiente + 4 "Delta" da irradiância da sala, por grupo (Sims 0,8, objetos 0,5) | as mesmas luzes de fora |
| paredes/pisos/terreno | light maps | luzes diretas |
| lâmpadas | entram na irradiância da sala e nos light maps | não iluminam |

## 8. O que falta

- `GatherRoomLighting` (de onde vem a irradiância de 6 direções), as luzes
  de janela, `BrightenUnlitRooms`, e a difusão dos light maps.
- A tabela de direções do §4 e o vetor `V`.
- A conversão `FalloffRate` → constante de atenuação e o alcance.
- O comando `skylight` (se guarda cor × força).
