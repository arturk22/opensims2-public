# Shaders e materiais: o que o binário de Mac faz com os `.matShad`

Fontes: o executável de Mac **Bon Voyage 1.10** (`/mac/sims2_bv.ppc` no
projeto Ghidra `~/sims2`, nomes de `tools/symxfer/`), conferido com o **Mac
1.0 base** (`/mac/sims2_base_1.0.ppc`), e o texto dos 67 scripts de material
de `TSData/Res/Materials/Materials.package` (tipo `0xCD7FE87A`, nomes no
recurso `0x4E6D6150`; `config.matShad` é a raiz). Mesmas convenções de
`docs/primitives-mac.md`: "binário" = lido no código decompilado, "texto" =
lido nos `.matShad`, "inferido" = dedução nossa, "só BV" = ausente no 1.0.

Resumo para quem implementa o renderer Metal:

1. **O Mac roda o caminho "DX8" dos `.matShad`, não o "DX9".** A camada
   Direct3D da Aspyr declara `vs_1_1` e `ps_1_1` em qualquer placa (§1). Com
   isso `DetermineHardwareSupport()` liga `useHWShader1Path`, e os materiais
   seguem `DX8HardwarePath()`, `PS1WallRendering()`, `PixelShaderFloors()` e,
   para os Sims, o caminho de função fixa (`CreateSimStandardMaterialFixedFunction`).
   (Rodada n22, §13: espelhos, normal map, recorte e CelShaded.)
   Tudo o que o código atual tirou de `DX9HardwarePath`/`PS2WallRendering`/
   `DX9SimMaterials` não é o que o jogo de Mac desenha.
2. **Iluminação por vértice com escala "HDR" ×2** (§4): a cor difusa sai do
   vertex shader pela metade e saturada, e o pixel shader multiplica por 2.
   O resultado satura em **2× a textura**, não em 1× como hoje.
3. **Paredes, pisos e terreno não têm luzes diretas**: são textura × *light
   map* × 2 (§5, §6, §7). Os light maps (`wallLightMap_<página>`,
   `floorLightMap_<página>`) são calculados pelo motor (ver
   `docs/lighting-mac.md`).
4. Especular só num passe aditivo separado, por vértice, Phong (R·L) com
   expoente `stdMatSpecPower × 0,4`, e só quando `stdMatSpecPower > 1` e a
   soma de `stdMatSpecCoef` > 0,3 (§4.3).

---

## 1. A camada Direct3D da Aspyr (`IDirect3D*_Mac`)

O jogo de Mac é o código de Windows compilado contra uma reimplementação de
Direct3D 9 sobre OpenGL (`IDirect3DDevice_Mac`, `IDirect3DVertexShader_Mac`,
...). Os `.matShad` são os mesmos do PC; quem muda é o que o "dispositivo"
declara e como os shaders viram OpenGL.

### 1.1 Capacidades declaradas (binário, base e BV)

A função sem nome em `0x1c150` (BV; `0x16700` no 1.0) preenche o
`D3DCAPS9` de cada adaptador, depois de criar um contexto AGL de teste:

| campo D3DCAPS9 | valor |
|---|---|
| `VertexShaderVersion` (+0xC4) | `0xFFFE0101` (vs_1_1) **se** `GL_ARB_vertex_program`; senão 0 |
| `MaxVertexShaderConst` (+0xC8) | `GL_MAX_PROGRAM_ENV_PARAMETERS_ARB` do contexto |
| `PixelShaderVersion` (+0xCC) | `0xFFFF0101` (**ps_1_1, sempre**) |
| `MaxSimultaneousTextures` | `max(8, GL_MAX_TEXTURE_UNITS)` |

O scan de todo o `__text` por `lis 0xFFFF/0xFFFE` + `ori 0x200/0x300` só
acha essas duas escritas na camada D3D (as outras ocorrências, em
`0xb76a18..0xb76cec`, são a tabela de versões do `cShaderProgramsManagerDX`).
No 1.0 é igual (`0x167cc`/`0x167f4`). Logo, para os `.matShad`:

- `queryIntegerGraphicsDeviceCap(maxPixelProgramVersionMajor)` = 1;
- `queryIntegerGraphicsDeviceCap(maxVertexProgramVersionMajorHW)` = 1 (0 sem
  `GL_ARB_vertex_program`).

A tabela de nomes de capacidade (`0x27a4218`, binário) é: booleanas
`hasFixedFunctionVPHW` 0, `hasProgrammableVPHW` 2, `hasUByte4VertexType` 4,
`hasCubeMapSupport` 6; inteiras `maxVertexProgramVersionMajorHW` 4,
`...MinorHW` 5, `maxPixelProgramVersionMajor` 8, `...Minor` 9,
`numSimultaneousRTs` 1, `maxSimultaneousTextures` 3.

### 1.2 Seleção do caminho (`DetermineHardwareSupport`, texto `vsfragments.matShad`)

```
psVersion = maxPixelProgramVersionMajor ; vsVersion = maxVertexProgramVersionMajorHW
if kUseShaderPrograms:
   if psVersion = 2 and not kForceNoDX9   -> useHWShader2Path   (DX9)
   elif psVersion >= 1 and not kForceNoDX8 -> useHWShader1Path  (DX8)   <- Mac
   elif (not ffgpHW and psVersion = 0) or kForceNoDX7 -> useSWVertexShaderPath
default: useFixedFunctionPath
```

`kUseShaderPrograms` vale `true` no script; `cTSConfigMaterialSettings::
SetUseShaderPrograms(false)` (binário) cria a sobreposição
`kUseShaderPrograms = "0"` (opção `useShaders` das regras gráficas; a Aspyr
também tem o argumento `-noShaders`). Os outros ajustes que o
`cTSConfigMaterialSettings` empurra para os scripts: `bumpMapping`,
`specHighlights`, `reduceBoneWeights`, `morph`, `vs2LoopsFunctional`,
`simpleTerrain`, `flagSkinnedObjects`, `gpuCompositing`, `causticsEnabled`,
`useLODs`, `kUsePS30`, `kRaiseLightSnowFF`, `kUseMacPorts` (este só existe
nos scripts de expansão).

Os blocos `#<` ... `#>` são **comentários de bloco** (inferido, mas forte: em
`SimSkin()` um `#<` engole um `else`/`endif` cujo `if` está comentado com
`#`; sem isso o script não fecharia). Em particular o trecho de
`DetermineHardwareSupport` que força `useHWShader1Path true` é comentário.

### 1.3 Shaders → OpenGL (binário)

- **Vertex shaders** (`vs_1_1` assemblados dos `shaderSource`/fragmentos):
  `D3DShader_CompileToARB` (`FUN_00057a28`) traduz o bytecode para
  `!!ARBvp1.0` (constantes em `program.env[0..N]`, `ATTRIB vN =
  vertex.attrib[k]` com `#pragma dcl_*`, `oTn.z = 0`/`oTn.w = 1` quando o
  shader não escreve). `FUN_00018e50` (criação do `IDirect3DVertexShader_Mac`)
  aceita texto que já começa com `!!ARB`; se a tradução falhar e o shader não
  puder ir para o emulador por software (`CanEmulateSoftwareShader`,
  `FUN_00048178`: só `vs_3_0` com opcodes da tabela `PVE::*`), usa
  `HardcodedTranslationToARB` (abaixo).
- **Pixel shaders** `ps_1_x`: traduzidos para `GL_ATI_text_fragment_shader`
  (Radeon 8500/9000/9200, conferido pelo nome do renderer), senão
  `GL_NV_register_combiners`, senão `GL_ARB_fragment_program`
  (`IDirect3DDevice_Mac::Create`, flags em `+0xf123..0xf125`).
- **HLSL** (`shaderProgram -method compile`): `_D3DXCompileShader` **não
  compila**. Lê `<nome do material>.bin` dos recursos do bundle
  (`MacGetMaterialNameHint`, `FUN_00047cfc`, posto por `MacSetMaterialNameHint`
  em `FUN_00047c88`), desfaz o endianness do bytecode e da tabela `CTAB` e
  passa para o tradutor. Só 7 programas usam `-method compile` (piscina
  `RegularWavesHLSL`, cáusticas, animais, pós-processamento, reflexo). A
  nossa cópia do `.app` não tem os `.bin`; para o Metal isso não importa (o
  HLSL está no próprio `.matShad`).

`HardcodedTranslationToARB` (`FUN_00048100`, **só BV**: o 1.0 não tem esses
textos) devolve um `!!ARBvp1.0` escrito à mão para 4 hashes de bytecode
(`0x0EFC024E`, `0x0BB4C76C`, `0x88D497D4`, `0xC3BAFF2B`): são os vertex
programs **de transformação pura + cor do vértice** dos passes de sombra/cor
chapada (`DX8ShadowRendering`, passe de `ShapeColor`): `oPos = v1 ×
c[4..7]` (`geomToClip`), `oD0 = v2`; a segunda variante também escreve
`oT1.xy = pos.zw` (profundidade); as duas últimas são as mesmas com
skinning de 4 ossos (paleta em `c[56 + 3·índice]`, 3 linhas por osso, pesos
`x,y,z` e o 4º = 1 − soma, `c50 = (765.01, −1, 1, 4)`, `c51 = (255, 3, 0.5,
1e-4)`, `c52 = (0, 0.5, 1, 2)`). Nada disso muda a imagem: é contorno de
bug de driver/tradutor. Para o Metal basta saber o que esses passes fazem.

---

## 2. A linguagem dos `.matShad` (texto + nomes no binário)

O motor (`nGZCommandParser::cBaseParser`, `nGZSceneGraph::cMaterialParser`)
lê um script de comandos, um por linha, `#` comenta até o fim da linha.

- **Variáveis**: `set nome valor` (texto), `seti`/`setf`/`setb`/`setc`
  (cor)/`sete nome valor (opções)` (enum)/`setv2`/`setv3`/`setv4`. Uso:
  `$nome`, `${nome}` dentro de texto, `&nome`/`&{nome}` para parâmetros de
  macro. Expressões entre parênteses: `+ - * /`, `= != < > <= >=`, `and or
  not`, `?:`, swizzles (`($v).xyz`, `(a, 1).xxxy`) e funções: `strcmp`,
  `varExists`, `hasVertexFormat(tipo, índice)`, `numLightsOfType(tipo)`,
  `isEnabled(skinning|morphing)`, `maxBonesPerVertex`, `numPoseTransforms`,
  `queryIntegerGraphicsDeviceCap`, `queryBooleanGraphicsDeviceCap`,
  `viewerRenderType`.
- **Macros**: `define Nome(params) ... enddef`; `create Nome(args)` expande.
  `include arquivo.matShad`.
- **Fluxo**: `if/elseif/else/endif` avaliados na hora de criar o material
  (não por pixel).
- **Definições**: `materialDefinition nome` + `setDefinition Macro` +
  `addParam p v` (é o que o TXMT faz: `cMaterialDefinition` com tipo = nome
  da macro e propriedades = variáveis). O material é a expansão da macro
  com as propriedades do TXMT por cima dos defaults `set*` globais.
- **Estrutura**: `material` → um ou mais `shader [-layer n]` → `pass
  [-fixedFunction] [-modifiedEachFrameHint] [-clipAlways]` → estados +
  `stage` (função fixa) ou `shaderProgram` + `sampler n` (programável).
  Um material com vários `shader` = **alternativas**: o motor usa a
  primeira cujos predicados (`viewerRenderTypePred`, `vertexFormatPred`,
  `validateRenderShaderContext -numLights/-vertexFormat/-viewerRenderType`,
  `supportedRenderShaderContexts`) batem com o contexto de desenho e que
  valida no dispositivo (inferido pelas mensagens "Shader validation",
  "...failed on cIShaderController::InitDeviceResourcesAndValidate() at pass
  %d", "...failed -- render shader context does not have required flag %d
  set"). Ex.: `DX8HardwarePath` cria o shader duas vezes, a segunda com
  `reduceLights` (uma luz spot a menos no 2º passe especular) como reserva.
- **Estados**: `alphaBlend srcFactor(f) add|sub dstFactor(f)`, `alphaTest
  bool ref` + `alphaTestFunction acceptIfGreater|acceptIfGreaterOrEqual|
  acceptIfEqual|acceptIfLess`, `depthTest bool -enableDepthWrite bool`,
  `depthTestFunction accept|acceptIfLess|acceptIfLessOrEqual|acceptIfEqual`,
  `cullmode none|cullClockwise|cullCounterClockwise`, `fillmode`,
  `ffDepthOffset n`, `stencil*`, `colorScalar (r,g,b) a [-applyShapeColor i]
  [-applyTextureLightColor i j] [-animAlpha onda vel fase (a,b)]`.
- **Função fixa**: `ffLighting -enable -localViewer -normalizeNormals`,
  `ffMatCoef -amb -diff -diffAlpha -spec -specPow -emit [-applyShapeColor
  i]`, `addSpecular`, `stage` com `texture`, `textureAddressing`,
  `ffTextureCoordsSource n|fromReflectionVector|fromPosition`,
  `ffTextureMatrix -scalev/-trans/-invert/-cameraToGlobal/-orientCameraToGlobal`,
  `textureTransformType vector2|vector3`, `textureBlend <cor>(a b)
  <alfa>(a b)` com operações `select`, `multiply`, `multiplyScale2`,
  `multiplyScale1`, `add`, `multiplyAdd`, `lerpTextureAlpha`,
  `lerpColorScalarAlpha` e argumentos `texture`, `diffuse`, `outRegister`
  (resultado do estágio anterior), `colorScalar[:invert]`.
- **Programável**: `shaderProgram -target vertexProgram|pixelProgram
  -method assemble|link|compile`. `assemble`: o texto entre `shaderSource`
  e `endShaderSource` é assembly D3D (`vs_1_1`, `ps_1_1`, ...). `link`: o
  vertex program é montado de **fragmentos** (`shaderFragment Nome`), cada
  um um `vertexfragment Nome = asm_fragment { ... };` com registradores
  simbólicos (`r_diffuse_r2`, `c_lightColors[a0.x]`, ...) ligados pelo
  `cShaderProgramsManagerDX`/`IDirect3DFragmentLinker_Mac::LinkShader`.
  `bindConstants reg -bindingID id [-constantCount n] [-data (...)]` liga
  constantes; ids (ordem da tabela no binário, `0x10483a0`): `geomToCamera`,
  `cameraToGeom`, `globalToCamera`, `cameraToGlobal`, `geomToGlobal`,
  `globalToGeom`, `clipToCamera`, `cameraToClip`, `clipToViewTarget`,
  `geomToClip`, `geomToClipFromParentView`, `clipToGeom`, `globalToClip`,
  `clipToGlobal`, `cameraToClipWithDepthOffset`, `posesToCamera`,
  `posesToCameraPacked`, `morphWeights`, `cameraToLightClip(Adj)`,
  `frameInfo`, `frameTimeInfo`, `sunLightDir`, `ambientLight`,
  `cursorTerrainPosition`, `immediateData`, `shapeColorIndex0`,
  `allTextureLights`, `allStandardLightData`, `allStandardLightData2`
  (exige 32 constantes), `allStandardLightCameraSpaceDirections/Positions`,
  `allStandardLightColors(Half)`, `allStandardLightSquaredRangesPacked`,
  `allStandardLightFalloffRatesPacked`, `allStandardLightSpotHelper1/2Packed`,
  `allStandardLightLoopDataPerVertex/PerPixel`, `allStandardLightExtraData`
  (8), `perPixelLightColors`.
- **Animação de textura**: `textureMatrixAnimation -targetType
  fixedFunction|vsConstants -targetRegister 11 -aorigin -arot onda vel fase
  (início,fim) -ascale ... -atrans ...`, `textureTileAnimation vel -tiles
  (u,v)`. No DX8 a matriz vai para `c11..c13` e o fragmento
  `TextureMatrix2DTransformTex0` a aplica.
- **Camada** (`shader -layer n`): ordem de desenho; menor primeiro. Valores
  usados: StandardMaterial `stdMatLayer·8 (+7 se blend)`, Sims `+3` cabelo
  com blend / `+7` outros, paredes 0, máscara de parede −16, pisos 0, grade
  3, terreno: canvas `(−2−3)·8 = −40`, pintura −32, luz −24; água
  `stdMatLayer + 5`; sombra de Sim 4; overlay de sombra 2; skirt −2.
- **Tipos de luz** para `numLightsOfType`: `directional`, `point`, `spot`,
  `environmentCube` ("texture lights": cor que escurece reflexos à noite),
  `drawState`, `linearFog`, `kShapeColorLight` (fantasma/destaque),
  `kAdditiveLight`, `kShadeLight`.

`viewerRenderType`: `kRenderTypeNormal`, `kRenderTypeShadow` (passe que
desenha só a silhueta na cor `stdMatShadowColor`), `kRenderTypeReflection`,
`kRenderTypeMirror`, `kRenderTypeThumbnail`.

---

## 3. Função fixa: o que o "D3D" faz (para os passes `-fixedFunction`)

É D3D7/8 clássico, emulado pela Aspyr com GL 1.x (`MacApplyLightSettings`,
`MacSetColorCombine`). Para reproduzir:

- Cor do vértice = `emit + amb·ambienteGlobal + Σ diff·cor_luz·max(N·L,0)·aten`,
  **saturada por canal em [0,1]** antes da interpolação.
- `multiplyScale2(texture diffuse)` = `saturate(2·tex·cor)`. Quase todos os
  materiais de função fixa usam ×2 e põem `diff = amb = coef·0,5` (ou
  `coef·0,4` nos Sims), então o efeito é o mesmo do caminho DX8: luz até 2×.
- `AttenuatedMatCoef(s)` = `amb = diff = stdMatDiffCoef·s`, `emit =
  stdMatEmissiveCoef·s`, especular sem escala.

---

## 4. StandardMaterial no caminho DX8 (`DX8HardwarePath`, texto)

### 4.1 Modo de iluminação (`DetermineLightingMode`)

- Sem normais ou `stdMatLightingEnabled = false` → `kNoLighting`.
- Com luz: soma de `stdMatDiffCoef` e de `stdMatSpecCoef` = 0 → sem luz;
  senão `kFullLighting`; sem nenhuma luz direcional/ponto/spot no contexto →
  `kAmbientOnly`.
- `stdMatAlphaBlendMode = additive` → sempre `kNoLighting`.
- Sem `texcoord 0` → textura base, máscara especular e normal map
  desligados.

### 4.2 Passe difuso (`StandardMaterialDiffusePassDX8`)

Até **4 luzes por passe**. Com mais de 4, o 1º passe leva até 4
direcionais, depois pontos e spots até completar 4; o 2º passe (aditivo,
`one + one`, `depthTest ≤` sem escrita) leva o resto (no máximo 4 de novo,
já que o contexto tem no máximo 8). O ambiente só entra no 1º passe.

Vertex shader (fragmentos `vslightfragments.matShad`, registradores em
`vsfragments.matShad`: `c14` difusa do material (w = `stdMatAlphaMultiplier`),
`c15` especular (w = `stdMatSpecPower·0,4`), `c16` ambiente, `c17`
emissiva, `c18..c25` direções, `c26..c33` cores (w = escala especular
relativa), `c34..c41` posições, `c42..43` atenuação (uma componente por
luz), `c44..45` alcances², `c46`/`c48` auxiliares do cone):

```
para cada luz i do passe (espaço de câmera, N normalizada):
   direcional:  d_i = max(N·L_i, 0)
   ponto/spot:  v = P_i − pos; r² = v·v; L = v/|v|
                aten = (r² ≤ alcance_i²) / (1 + C_i·r²)
                spot: aten *= clamp((L·D_i − h1_i)·h2_i, 0, 1)
                d_i = max(N·L, 0)·aten
difusa = Σ d_i·cor_i
1º passe: oD0.rgb = 0.5·((difusa + ambiente)·difMaterial + emissiva)
2º passe: oD0.rgb = 0.5·(difusa·difMaterial)          (sem ambiente nem emissiva)
oD0.a = stdMatAlphaMultiplier        (saturado pelo hardware, como toda cor de vértice)
```

A emissiva **não** é multiplicada pela difusa do material. `C_i`, os
alcances e os auxiliares do cone vêm do motor (`docs/lighting-mac.md`).

Pixel shader (`DX8StandardMaterialDiffusePixelShader`, `ps_1_1`):

```
com textura:  r.rgb = tex.rgb·v0.rgb        (AlphaReplicate: r = tex.a·v0)
              r.a   = tex.a·stdMatAlphaMultiplier
              cutout: r.a = cutout.a ; alphaTest ≥ 127
sem textura:  r.rgb = v0.rgb ; r.a = stdMatAlphaMultiplier·stdMatUntexturedDiffAlpha
r.rgb = 2·r.rgb                              ("HDR": mov_x2)
env cube:     r.rgb += stdMatEnvCubeCoef·cube(R)  [·allTextureLights se houver luz environmentCube]
ShapeColor:   r *= shapeColorIndex0
passes 2+ com blend: r.rgb *= r.a (pré-multiplicado, somado ao fundo)
saída saturada em [0,1]
```

Resultado prático: `cor = saturate(2·tex·saturate(0,5·(luz·dif + emis)))`,
isto é, o lit satura em **2× a textura** e não em 1×.

Estados: `blend` → `srcAlpha/invSrcAlpha`, sem escrita de z; alpha test do
TXMT só no 1º passe (`acceptIfGreaterOrEqual`, ref `stdMatAlphaRefValue`);
`additive` → `srcAlpha/one`; `addNoSrcAlphaScale` → `one/one`.

Normal map (`bumpMapping` ligado nas opções e `stdMatNormalMapTextureEnabled`):
até 2 luzes por pixel (a 1ª direcional e/ou o 1º ponto/spot) com `dp3_sat`
entre o normal map e a direção da luz no espaço tangente (passada em `v1`/
`t2`), cores em `allStandardLightColorsHalf`; as outras luzes ficam no
vértice. `r = 2·tex·dif·(v0 + Σ N·L·cor/2)`. Normal map desliga o env cube
salvo no passe extra `BumpyShinyPass` (`texm3x3vspec`).

### 4.3 Passe especular (`StandardMaterialSpecularPassDX8`)

Só se `stdMatSpecPower > 1` **e** `Σ stdMatSpecCoef > 0,3` (`CheckForSpecular`;
abaixo disso a potência é zerada). Aditivo (`one + one`, sem escrita de z),
as mesmas divisões de 4 luzes:

```
R = reflexo do vetor de visão em N (espaço de câmera)
s_i = pow(max(R·L_i, 0), stdMatSpecPower·0,4)·(N·L_i − 1e-4 > 0)·aten_i·corLuz_i.w
oD0.rgb = (Σ s_i·corLuz_i)·stdMatSpecCoef           (sem ×0,5 e sem ×2)
pixel: r.rgb = v0 ; com blend/alpha test r.a = tex.a·alfa e r.rgb *= r.a
```

É Phong (R·L), por vértice, com expoente 0,4× o do TXMT. O código atual faz
Blinn (N·H) por pixel com o expoente cheio.

### 4.4 Sem luz / só ambiente

Um passe: `AmbientLighting` → `oD0 = 0,5·ambiente·difMaterial`; pixel
`2·tex·v0` (HDR). `kNoLighting` → `UnlitPixelShader`: `tex·stdMatDiffCoef`
(alfa `tex.a·stdMatAlphaMultiplier`), sem ×2.

### 4.5 Fantasma/destaque (`kShapeColorLight`)

Com essa luz no contexto: blend ligado, layer +7, e um passe extra antes
que só escreve z (`srcFactor(zero) + dstFactor(one)`, `ps` devolve 0); a
cor final é multiplicada por `shapeColorIndex0`.

### 4.6 Sombra (`viewerRenderType = kRenderTypeShadow`)

`DX8ShadowRendering`: transforma (com skinning/morph) e pinta
`stdMatShadowColor`, alfa 1. Os Sims usam `SimShadowDX7` (função fixa, a
mesma cor).

### 4.7 Tipos obsoletos (`deprecated.matShad`)

Iguais ao que o código já faz (`kDeprecated` em `src/formats/model.cpp`):
cada tipo força parâmetros e chama `StandardMaterial`. O caminho DX8 vale
para eles também.

---

## 5. Paredes (`wall.matShad`, `pixelShaderWalls.matShad`)

`WallpaperAndMask` com `useHWShader1Path` → `PS1WallRendering`:

- **Sem máscara** (`wallMaskTextureName` ausente) →
  `PS1MasklessWallRendering`: `cor = 2·papel·wallLightMap_<page>` (sem
  `page`: só o papel).
- **Com máscara, sem bump** → `PS1BasicWallRendering`, um passe: alpha test
  `> 127` no alfa da máscara (UV 1, `clamp`, espelhada por
  `ffTextureMatrix -scalev (−1,1) -trans (1,0)` quando `wallMaskMirrorFlag`),
  `cor = 2·papel(UV 0, tile)·lightmap(UV 2)`. **Sem luzes diretas, sem
  difusa do material, sem normais.** Sem `page`: `cor = 2·papel`.
- **Com bump** (`bumpMapping` e `wallpaperNormalMapTextureEnabled` e
  `wallNormalMappingEnabled`) → `PS1BumpMapWallRendering`, 2 passes: 1º
  escreve `dp3_sat(normal map, wallIncidenceMap_<page>)` com o recorte da
  máscara; 2º multiplica o framebuffer (`destColor·zero`, z igual) por
  `2·papel·lightmap`.
- Destaque (`wallHighlightOn`): `papel·t3.a + wallHighlightIntensity`
  (`wall_selection_colors`). Cáusticas de piscina somadas quando
  `tsCausticsPass` (só `wall-poolroundedlipblue-base`).
- `Wallpaper`/`WallpaperBump`/`WallpaperPool` (paredes sem máscara, ex.:
  catálogo) → `PixelShaderWallTextureShader`: `2·base·lightmap`, depth test
  desligado. `WallMask` não desenha nada (shader vazio).

---

## 6. Pisos (`floor.matShad`, `pixelShaderFloors.matShad`)

`Floor`/`FloorPool` com DX8 ou DX9 → `PixelShaderFloors` (o mesmo nos dois):

```
oT0 = uv0 · (1/floorMaterialScaleU, 1/floorMaterialScaleV)   (tile)
oT1 = uv1                                                   (espaço do light map)
cor = 2·base·floorLightMap_<page>·stdMatDiffCoef ; alfa 1
bump: cor = 2·base·lightmap·dif·dp3_sat(normal map(oT0), floorIncidenceMap(oT1))
cáusticas (piscina): 2·base·lightmap·dif + cáustica[·allTextureLights]
sem page/uv1: cor = base
```

Sem luzes diretas, sem especular. Grade de construção: `TileGridMaterial`
(camada 3, alfa `floorGridAlpha` 0,775).

---

## 7. Terreno (`terrain.matShad`) e skirt

Três materiais desenhados por cima um do outro, **todos sem luzes**:

1. `TerrainCanvasShader` (camada −40): textura do tipo de terreno com UV ×
   (1/20) (`canvasBaseTextureScale` 20, matriz invertida) e `<textura>_detail`
   × (1/4) por cima com `lerpTextureAlpha` (a textura de detalhe decide pelo
   seu alfa).
2. `TerrainPaint` (−32): cada pintura, blend `srcAlpha/invSrcAlpha`, alpha
   test > 10; cor = pintura (escala 20, detalhe 4), alfa do `alphaMap`
   (`clamp`, escala `alphaMapScaleU/V`).
3. `TerrainLighting` (−24): multiplica o framebuffer (`destColor·zero`) por
   `2·floorLightMap_0(pos/64)·terrainMatDiffCoef(0,9)`. A página 0 do light
   map de piso cobre o lote; UV = posição em tiles / 64.

O código atual ilumina o terreno como StandardMaterial e repete a textura a
cada 4 tiles; o jogo usa 1/20 + detalhe 1/4 e o light map.

---

## 8. Sims (`sims.matShad`, `DX7SimMaterials.matShad`)

Com `useHWShader1Path`, `SimSkin`, `SimStandardMaterial` e `SimHair` vão
**todos** para `CreateSimStandardMaterialFixedFunction` (o caminho
`CreateSimStandardMaterialDX8` está comentado no script):

- Textura de pele/roupa já composta na CPU (`useCPUCompositedTextures`
  true; `compositeBaseTextureName` vira `stdMatBaseTextureName`).
- Luz de função fixa com `amb = diff = stdMatDiffCoef·simMaterialLightingScale·0,5`
  = **dif·0,4**, sem especular ("no sim material can have specular"),
  emissiva 0. Estágio `multiplyScale2(texture diffuse)`, alfa
  `texture·diffuse`, `clamp` em U/V. Resultado: `saturate(2·tex·saturate(0,4·
  dif·(ambiente + Σ luzes)))`.
- `SimHair` com `blend`: `srcAlpha/invSrcAlpha`, **escreve z**, alpha test
  `> 60`; com `additive`: `srcAlpha/one`. Camada `stdMatLayer·8 + 3` (cabelo)
  ou `+7` (outros com blend). **Quem é `SimHair`** (binário, rodada 15,
  `cTSSimOutfitData`, BV `0x82bb70`, ao montar a roupa): nenhum TXMT diz
  `SimHair`; para uma parte do tipo 1 (cabelo, chapéus inclusive, o
  `outfit=1` do GZPS) o código troca a definição de cada material
  `SimStandardMaterial` para `"SimHair"` (vtable `0x2c` do material); na
  camada composta, um material já `SimHair` volta a ser `SimHair`. Numa
  roupa do tipo `0x20` (com `param_5 & 3`) os `StandardMaterial` viram
  `SimStandardMaterial`. No código: `markHairPart` (`formats/sim.cpp`), no
  lugar do "hair no nome da textura" inferido antes (as capturas do CAS
  saem idênticas).
- Env cube (`stdMatEnvCubeMode reflection`, sem blend): passe extra
  `srcAlpha/one` com `cube(R)·stdMatEnvCubeCoef`.
- Reserva (2º shader): só a cor `2·dif·0,4·luz` sem textura.
- Sombra: `SimShadowDX7`. "Fantasma" (`kShapeColorLight`): `DX7GhostPass`.

`simMaterialLightingScale 0,8` já está em `renderer.cpp`; falta o resto
(×0,5 e ×2, a saturação no vértice).

---

## 9. Água, piscina, sombras de chão, overlays

- **Objetos de água** (`WaterAnimatingTextures`, caminho DX8 →
  `LowEndWater`): função fixa, luz `amb = diff = stdMatDiffCoef`; dois
  estágios da mesma textura com escala animada `sawtooth` (`waterSpeed`,
  fase 0 e 0,5, escala 1→2) misturados por um alfa `triangular` do
  `colorScalar`, vezes a cor do vértice; blend `srcAlpha/invSrcAlpha`, sem
  escrita de z (reserva: `TwoPassWater`, os dois estágios em passes).
- **Piscina** (`PoolWaterSurfaceMaterial`): no DX8 (`useHWShader1Path`) usa
  `RegularWavesHLSL` (vertex program HLSL 1.1: ondas por `frameInfo`,
  reflexo do env cube, `one + one`); a reserva é `SimplifiedWaterSurface`
  (`(0,2, 0,3, 1,0)` alfa 0,4). As camadas de profundidade
  (`poolWaterLayer-1`, `swimming_pool_deeplayer` × `poolLayerColour`, alfa
  0,9 × `allTextureLights`) e o gerador de cáusticas só existem fora do
  caminho FF/SWVS, ou seja, **existem no Mac**.
- **Ordem** (rodada 16): a camada de água (5) é contada em unidades de
  `stdMatLayer` (no app `(stdMatLayer + 5)·8`): depois do terreno e das
  suas pinturas misturadas (camada 0 + 7). Antes a água ia na camada 5 da
  escala ×8, antes das pinturas, sem escrever z, e o fosso de `N001_Lot82`
  saía coberto pela areia e pelo leito (QA ao vivo da rodada 16;
  `docs/screenshots/r16-fosso-agua.jpg`).
- **Água do terreno** (`TerrainWaterShader`): reflexo de
  `reflectionoutdoorwater-envcube` (`fromReflectionVector`,
  `-orientCameraToGlobal`, sem mipmap) × `colorScalar(1,1,1)` ×
  `allTextureLights`, alfa da cor do vértice.
- **Sombra de objeto/Sim no chão** (`ShadowShader`, camada 4):
  multiplica o framebuffer (`destColor·zero`) pela textura da sombra
  (`clamp`, sem mipmap), sem escrita de z.
- **Overlay de sombra** (`OverlayShadowMaterial`, camada 2): no DX8 sem
  `lmIntegratedShadows`, a sombra é misturada com o light map de piso
  (`lerpTextureAlpha(outRegister diffuse)` com `floorLightMap_0`).
- **Espelho** (`SimpleMirrorReflection`, `mirrors.matShad`; o espelho de pé
  do CAS, quad `caspodiumarea_simple_mirror_reflection` do
  `CASPodiumArea`): com `$mirrorReflectionsEnabled` falso (o
  `config.matShad` faz `setb mirrorReflectionsEnabled false`) o quad é o
  `BlankMirrorMaterial`: uma cor só, `colorScalar (0.5, 0.6, 0.75)`, sem
  luz nem textura; ligado, o `MirrorReflectionMaterial` marca o stencil
  (camada −9998, x de clip negado) e desenha a cena de uma câmera
  refletida no alvo `ScreenReflection` (camada 6). Quem liga a flag (rodada 13, binário): o
  `cTSConfigTargets` registra as propriedades de configuração
  (`FUN_008689d8`) e, quando uma muda (`FUN_008676d4`, BV `0x8676d4`), a
  propriedade **`reflectionWithExtraViewer`** (ID `0xCDD2E813`) põe a
  variável de matShad `mirrorReflectionsEnabled` em `"true"`/`"false"`
  (vtable `0xd8(0, nome, valor)` do sistema de variáveis); nada mais a
  escreve. No `Graphics Rules.sgr`, a opção **Reflection** `$On` liga
  `reflectionWithExtraViewer` e `nhoodWaterReflection` (esta, ID
  `0xED2B6003`, refaz os alvos de render, `FUN_00867120`); as classes de
  placa alta põem `Reflection $On`, as média/baixa `$Off`. Ou seja, o
  espelho reflete de verdade quando a opção de reflexos do jogo está
  ligada. Rodada 12: desenhamos o `BlankMirrorMaterial` (antes o quad
  saía branco, material desconhecido sem textura; `r12-cas-espelho-masculino.jpg`).
  Rodada 13: com a opção **Reflection** ligada (`GameSettings::reflections`,
  opção `reflection` do usuário, ligada por padrão — inferido: um Mac de
  hoje é placa de classe alta; `--reflections on|off` força numa execução)
  o renderizador faz o reflexo: escolhe o espelho mais perto da câmera que
  está em vista, desenha antes a cena numa textura do tamanho da tela (o
  alvo `ScreenReflection`) com a câmera refletida no plano do quad
  (`view · R`, R = I − 2nnᵀ com a translação −2dn; enrolamento invertido;
  `clip_distance` corta o que está atrás do espelho; os espelhos ficam de
  fora) e no passo normal o quad do espelho mostra essa textura no próprio
  pixel (o stencil do jogo vira "só os pixels do quad"). Partículas e o
  overlay não entram no reflexo. Rodada n22: todos os espelhos em vista
  refletem, cada um com o stencil do jogo (§13.1).
  `r13-espelho-cas-reflexo.jpg` (CAS desligado/ligado),
  `r13-espelho-lote-reflexo.jpg` (`N001_Lot82`, a porta vermelha no
  espelho do banheiro).

---

## 10. O que o código atual faz diferente

| tema | jogo (Mac, DX8) | `src/` hoje |
|---|---|---|
| saturação da luz | `saturate(2·tex·saturate(0,5·L))`, até 2× | `tex·saturate(L)`, até 1× (`shaders.metal`) |
| luz | por vértice, 4 por passe, 2 passes | por pixel, até 8 de uma vez |
| emissiva | somada à luz já × difusa, não multiplicada | somada antes de saturar, ok; mas sem o ×0,5/×2 |
| especular | passe aditivo, Phong R·L, expoente ×0,4, só se pot. > 1 e Σcoef > 0,3 | Blinn N·H, expoente cheio, sempre que pot. > 0 |
| paredes | `2·papel·lightmap`, máscara alpha test > 127 | iluminadas como StandardMaterial |
| pisos | `2·base·lightmap·dif` | iluminados como StandardMaterial |
| terreno | canvas 1/20 + detalhe 1/4, pinturas, × `2·0,9·lightmap` | textura a cada 4 tiles, iluminada |
| Sims | FF, `2·tex·saturate(0,4·dif·L)` | `0,8·dif` e satura em 1 |
| env cube | só se Σcoef ≥ 0,003; × `allTextureLights` à noite | igual sem a luz de textura |
| cabelo | alpha test > 60 com blend, escreve z | igual (`SimHairAlphaState`) |
| `TODO.md` "no caminho DX9 a difusa interpolada não é saturada" | não se aplica: o Mac usa DX8, que satura no vértice | — |
| normal map | 1ª direcional e 1ª ponto/spot por pixel, sem tangentes no Mac (§13.2) | igual desde a rodada n22 (a força da conversão segue inferida) |

## 11. O que falta

- Como o motor calcula os light maps de parede/piso (`cLightMapManager`,
  `cDiffusionManager`) e as constantes de luz (`allStandardLightData2`):
  `docs/lighting-mac.md`.
- O parser em si não foi decompilado (a maior parte está no `__textcoal_nt`
  sem nome); a semântica acima vem do uso nos scripts e das strings. Em
  especial `#<`/`#>` = comentário é inferido.
- Os passes de reflexo (`reflection.matShad`, espelhos), imposters,
  pós-processamento e efeitos não foram descritos.

## 12. Rodada 16: normal map, env cube e luz do CelShaded

- **Força do normal map.** Cada sub-imagem de um `cImageData` (TXTR,
  versão ≥ 8) traz um float depois do `creator`: é o **bump scale**
  (`cImageData::SetBumpScale`/`BumpScale`, BV `0xc976a4`/`0xc97768`, no
  `+0x38` da sub-imagem; o OpenTS2 o lê com 10 de padrão). Nos pacotes
  `Sims3D/*.package` do jogo base ele vale **10 em todas** as 8.244
  texturas (512 de bump). Quem o usa é a conversão de altura para normal
  ao carregar (`nGZGraphic4::ConvertHeightMapToNormalMapFast(src, dst,
  escala)` → `GenerateBumpMap_L8_to_A8R8G8B8(..., escala, tabela)`, nomes do
  `baserom.sym`); o corpo dessas funções não foi achado no BV (o arquivo
  de manipulação de imagem do Win32 parece não estar lá). O app segue com
  a força 2 por texel nas derivadas de tela (`kBumpStrength`, inferido);
  falta a fórmula com a escala 10.
- **Env cube.** As faces dos cubos do jogo vêm na ordem do D3D (+X, −X,
  +Y, −Y, +Z, −Z): no `outdoordaytime-envcube` e no
  `reflectionoutdoorwater-envcube` o céu (azul claro, a face mais clara) é a
  **face 4 (+Z)** e o chão a 5 (−Z). Com o mundo Z para cima, amostrar
  com o vetor de reflexão do mundo como está (o que o app faz) põe o céu
  em cima: conferido nos dados. O reflexo × a luz `environment` do estado
  (1 de dia, 0,8 à noite) já estava feito.
- **Luz do CelShaded.** O `celshading.matShad` faz `dp3` da normal em
  espaço de câmera (`geomToCamera`) com `csCameraLightDir` (1, 0, 0,5)
  sem normalizar. O espaço de câmera do jogo é o do D3D, de mão esquerda
  com a câmera olhando para +z: a matriz do volume de visão (BV
  `0xccd254`, §1 de `camera-ui-mac.md`) tem `m[14] = 1`, ou seja w = +z da
  câmera. No espaço de visão do app (mão direita, olhando para −z) a luz
  é (1, 0, −0,5), o que o shader já usa: conferido.

## 13. Rodada n22: espelhos, normal map, recorte e CelShaded (Ghidra MCP)

Lido com o Ghidra MCP no BV (`sims2_bv.ppc`, só leitura) e no texto dos
`.matShad` (`mirrors.matShad`, `standardMaterialDX8.matShad`,
`vslightfragments`/`pixelLightingFragments`, `celShading.matShad`,
`Graphics Rules.sgr`).

### 13.1 Espelhos: um observador por espelho, um alvo, o stencil

- **Cada espelho tem o seu observador.** O quad do espelho vem com um nó
  `cViewerRefNodeRecursive` no CRES. Quando a lista de desenho da vista
  principal chega nele, `cViewerRefNodeRecursiveImpl::
  ImplAddElementsToDisplayList` (BV `0xcfa60c`; o nome no `baserom.sym` é
  `0x574e08`) faz, uma vez por quadro (contador `+0x7c`): se a flag 1 do nó
  está ligada, só segue com `reflectionWithExtraViewer` verdadeiro (a opção
  Reflection); monta a câmera refletida pelo plano do nó
  (`RightHandedReflectionTransform`, BV `0xcf9cd4`) e, com a flag 2, o
  retângulo do espelho na tela (a caixa projetada, presa a [−1, 1]): a
  projeção é recortada para esse retângulo (escala `2/(x1−x0)`, deslocamento)
  e a viewport é o mesmo retângulo em [0, 1]. Com menos de 1 pixel de largura
  ou altura não desenha; senão põe o observador na lista (vtable `0x5c` da
  lista) com o alvo de render do nó (`ImplRenderTargetDef`). Não há limite de
  um espelho: **todo espelho em vista reflete, cada um no seu retângulo do
  mesmo alvo `ScreenReflection`**.
- **O desenho de um espelho** (`cDisplayListDynamicRenderStrategy::
  RenderMirror`, BV `0x633e0`; `RenderGeometryForMirror` BV `0x5d5e4`), no
  modo sem retângulos sujos: limpa profundidade e stencil do alvo (`Clear`
  da vtable `0x34` com 6 = z + stencil, z = 1; a cor não é limpa), põe a
  viewport inteira e desenha **só as partes da camada −9998** (os quads de
  todos os espelhos, `MirrorReflectionMaterial` 1º shader) com as matrizes
  da vista principal (`geomToClipFromParentView`, x de clip negado porque a
  câmera do espelho é a de mão igual com o x virado), z testado e gravado,
  sem cor; volta a viewport do retângulo, limpa só a profundidade (2) e
  desenha o resto (partes de camada ≠ −9998) com a câmera refletida e o
  `defaultState kRenderTypeMirror`: stencil `acceptIfEqual 1`, sem escrita.
- **O stencil dos quads** (`cApplyStencilStateForOverlappingReflections::
  ApplyDynamicState`, BV `0xcc7de8`, sem função definida no Ghidra, lido em
  assembly): stencil ligado; se o alvo do observador (vtable `0x1d8` do
  observador do contexto `+0xc`) é o próprio renderable da parte (`+0x2c` da
  info da parte), função `7` com ref **1**, senão ref **0**, máscara −1;
  operação (keep, keep, replace). Ou seja, onde o quad do espelho deste
  observador é o mais perto (z), o stencil vira 1; onde outro espelho está na
  frente, 0. Espelhos que se sobrepõem na tela ficam cada um com os seus
  pixels.
- **No passo normal** o quad (camada 6, depois dos opacos e antes dos
  transparentes e da censura) lê o `ScreenReflection` no próprio pixel
  (`oT0 = clip · (−0,5, −0,5) + 0,5·w`, dividido no estágio), sem luz.
- O filtro de visibilidade do observador do espelho
  (`cReflectionViewerVisibilityQueryFilter::TestNode`, BV `0x17c0ae0`)
  descarta nós pela máscara `0x07440100` e pelos grupos de render (bits não
  resolvidos). O app segue deixando de fora os espelhos e as partículas.
- Censura no espelho: §"Censura" do `cas-mac.md` (rodada 22).
- **No app** (rodada n22, `Renderer::encodeFrame`): para cada espelho em
  vista, de frente para o olho e com retângulo de pelo menos 1 pixel
  (`Renderer::mirrorRect`; os 8 mais perto, limite nosso): um passe desenha
  os quads de **todos** os espelhos em vista com a câmera da vista, sem cor,
  z testado e gravado, e o stencil = 1 no quad dele, 0 nos outros
  (`mirrorMarkState_`, alvo `Depth32Float_Stencil8`); o passe seguinte limpa
  só o z, recorta no retângulo (scissor; o jogo recorta a projeção, dá os
  mesmos pixels) e desenha a cena refletida onde o stencil é 1, com a
  censura do espelho por cima no mesmo stencil. A cor do alvo só é limpa no
  primeiro espelho do quadro. No passo normal cada espelho que refletiu lê o
  alvo no próprio pixel — também nos ladrilhos da censura da vista: o
  mosaico de um Sim na frente de um espelho mostrava o azul do
  `BlankMirrorMaterial` (o quadrado azul do `r22-censor-espelho.jpg`), agora
  mostra o reflexo (o `Frame` leva a `viewProj` da vista em `screen`).
  `OS2_DEBUG_MIRRORS=1` lista os espelhos e retângulos de cada quadro.
  Testes: `censor` (o retângulo) e `render` (dois espelhos lado a lado
  refletem o chão vermelho; desligado, a cor do espelho).
  `docs/screenshots/n22-shaders-dois-espelhos.jpg` (`N003_Lot46`, antes: só
  o espelho de pé reflete, o de parede fica azul; depois: os dois),
  `n22-shaders-censura-espelho.jpg` (`N001_Lot68`, Daniel no espelho de pé
  com `OS2_FORCE_CENSOR=3`: antes o mosaico tinha o azul do espelho, depois
  o reflexo).

### 13.2 Normal map no caminho DX8

`StandardMaterialPassesDX8` liga o bump (`localBumpMapFlag`) com
`bumpMapping` (MaterialDetail Alto), `stdMatNormalMapTextureEnabled` e sem
`ShapeColor`; aí o env cube sai do passe difuso e vai para o
`BumpyShinyPass`. `DX8DiffusePassLightFragments`:

- **Luzes por pixel**: só a 1ª direcional e a 1ª ponto/spot; as outras
  ficam no vértice. O vértice (`CompleteVertexDiffuseAddAmbientForPixelLighting`)
  dá `v0 = sat(0,5·(Σ luzes do vértice + ambiente + emissiva))`, **sem** a
  difusa do material.
- **Pixel** (`DX8StandardMaterialDiffuseBumpPixelShader`, ps_1_1):
  `r = (sat(n·L1)·c1 + sat(n·L2)·c3 + v0) · difusa; cor = sat(2·r·tex)`,
  alfa `tex.a · stdMatAlphaMultiplier`; `c1..c3` = cores das luzes 0–2 pela
  metade (`allStandardLightColorsHalf`); `n` = normal map ×2−1.
- **L em espaço tangente**: `(L·T, L·(T×N), L·N)` no vértice, com T do
  elemento `tangent` do vértice; L1 vai em `oD1` (×0,5 + 0,5). A 2ª luz é
  escrita em `oT3` (`WriteLightVectorToTex3`) mas o pixel a lê de `t2`
  (`texcoord t2`, o `t3` está comentado): `t2` não é escrito nesse passe,
  vale o padrão (0, 0, 0, 1) → `t2_bx2` = (−1, −1, −1), e a 2ª luz soma
  `sat(−(nx+ny+nz))·c3` (inferido pelo padrão do registrador). A 1ª
  ponto/spot de um objeto com bump quase não ilumina.
- **Sem tangentes no Mac**: o `Graphics Rules.sgr` faz
  `skipTangentsInVertexData true` quando `maxPixelProgramVersionMajor < 2`
  ("since we don't do bumpmapping on less ps2.0-capable hardware") — no Mac
  sempre (§1.1). O binário lê a propriedade (`SkipTangentsInVertexData`,
  BV `0x1d90a0` no gerador dos telhados, `0x1b093b8` no arquivo do
  `cGeometryNode`). Sem o elemento, `v_TANGENT` lê o atributo genérico
  padrão do GL, (0, 0, 0) (inferido) → L em espaço tangente = (0, 0, N·L)
  e o pixel vira `sat(n_z · N·L1)`: o relevo só escurece onde o normal map
  se inclina, sem direção de luz.
- **Altura → normal**: o `-bump` é um mapa de altura cinza
  (`fireplacehearthvalue-bump`: branco com rebites pretos). A conversão
  (`ConvertHeightMapToNormalMapFast`/`GenerateBumpMap_L8_to_A8R8G8B8` com o
  bump scale 10 do TXTR) não foi achada no BV (o `_D3DXComputeNormalMap` da
  Aspyr no `baserom.sym` tem 12 bytes, um stub), mas está no base 1.0:
  §13.5 (rodada n50).
- **No app** (rodada n22, `shaders.metal`): o passe de bump do DX8 como
  acima, com as tangentes do Mac (nenhuma): `n·L1 = sat(n_z · N·L1)` para a
  1ª direcional, a 1ª ponto/spot fora do vértice e somando
  `sat(−(nx+ny+nz)) · cor da luz 2 / 2`, `v0 = sat(0,5·(luzes do vértice +
  ambiente + emissiva))` e `cor = sat(2·tex·difusa·(v0 + n·L1·cor1/2 + ...))`
  (a emissiva agora vezes a difusa, como no pixel shader); o especular e o
  env cube usam a normal do vértice (o `BumpyShinyPass` com tangente zero
  reflete na normal do vértice). O normal map sai do mapa de altura em
  espaço de textura (`heightNormal`, força 2 por texel, inferida; antes o
  relevo seguia a direção da luz pelas derivadas de tela). Só com
  `bumpMapping` (MaterialDetail Alto, `GraphicsOptions::bumpMapping`, ainda
  sem botão). Teste `render` (bump num mapa plano: a luz do vértice e o sol
  por pixel). `docs/screenshots/n22-shaders-normal-map.jpg` (a lareira
  `fireplacehearthvalue`: antes os furos com contorno escuro pela luz,
  depois só o escurecimento do `n_z`).

### 13.3 Passe de recorte (`stdMatCutoutTextureName`)

Nenhum TXMT do jogo traz `stdMatCutoutTextureName` (padrão `"0"` = sem
recorte). Quem o põe é o código: `nTSSG::cObject::AddObjectCutout` (BV
`0x667894`) pega o nome `objectCutout` da extensão de lista de dados do
objeto (a pia: `ObjectCutoutTextureName`, BV `0x65a5c8`) e
`ChangeObjectCutoutBinding` (BV `0x667484`) acha no balcão embaixo o subconjunto
nomeado pela extensão `cutoutSubset` e chama `ApplyCutoutMaterial`
(BV `0x659d0c`) → `CreateAndRegisterObjectCutoutMaterial` (BV `0x6591f8`):
clona o material do subconjunto com o nome `<material>-cutout-<textura>` e
`stdMatCutoutTextureName = <textura>`; `RemoveObjectCutout` (BV `0x667828`)
faz o mesmo com `"0"`. Os lotes salvos guardam esse nome
(`..._clean-cutout-sink...`, `..._clean-cutout-0`). No DX8: o 1º passe lê a
textura de recorte com o UV 0 (`Copy2DTexcoordsToTexTwo`, `clamp`), troca o
alfa pelo dela (`mov r0.a, t2.a`) e faz alpha test `≥ 127`; os passes
seguintes (2ª leva de luzes, especular) não recortam.

Ao carregar (`nTSSG::cObject::LoadFromStream`, BV `0x65fae8`), cada
material salvo passa por `HasObjectCutoutTexture` (BV `0x65c0c8`): um
`<base>-cutout-<textura>` guarda `subconjunto → textura` no mapa do
`cObject` (`+0xe8`) e o subconjunto recebe `<base>` pela vtable `0x19c`, que
aplica o recorte de novo; por isso o recorte sobrevive às trocas de
material. As texturas dos lotes do jogo são `sinkcountervalue-mask`,
`sinkcountercolonial-mask`, `sinkcounter-quaint-mask`, `sinkcounter-club-mask`
e `sinkcounter-loft-mask`: o alfa branco na metade de cima do UV (onde fica
o tampo dos balcões, `v` 0,005–0,495) com o buraco da cuba, preto embaixo.

**No app** (rodada n22): `loadObjectModel` aceita o `<material>-cutout-<textura>`
salvo quando `<material>` existe (antes caía no material do próprio
modelo: o balcão perdia o design, ex. o `beechspeckle` do `N001_Lot68`, e
cobria a cuba) e põe `stdMatCutoutTextureName = <textura>` no clone
(`"0"`: sem recorte); `StandardMaterial::cutoutTexture`, `kCutout` e a
textura 20 do shader fazem o recorte só com luz (o `UnlitPixelShader` não
recorta). O `VmObjectView` mantém o sufixo quando a VM troca o material do
subconjunto (o `Change Material` dos balcões no init), como o mapa `+0xe8`.
Teste `render` (o buraco mostra o que está embaixo, a borda fica).
`docs/screenshots/n22-shaders-pia-recorte.jpg` (`N001_Lot68`, `--vm`: antes
o tampo verde tampava a cuba, depois a cuba aparece).

### 13.4 CelShaded (`celShading.matShad`)

Todos os 182 TXMTs `CelShadedMaterial` do jogo base são os ícones 3D de
memórias e desejos (`leftatthealtar_ring`, `firstbestfriendgold`...). Script:

- camada `csMatLayer · 8` (23 em 179 deles);
- rampa (`csRampTexture`, ponto, `clamp`) em `u = max(N·L, 0)` com a luz
  presa à câmera `csCameraLightDir` (1, 0, 0,5) sem normalizar e `v` = alfa
  da cor do vértice (`csRouteColorFromAlpha`, quando há cor no vértice),
  senão o `v` do UV 1 (com textura base e UV 1) ou do UV 0;
- cor `rampa · difusa` (com textura base: `· 0,25`, e o estágio da textura
  faz `multiplyScale4`, ou seja `4·tex·rampa·difusa·0,25`);
- borda (`csAddEdge`, 1 TXMT): rampa em `u = N·E + csEdgeOffset (+
  csEdgeLightDamping·N·L)`, `v = 0`; `csEdgeRampTextureAlphaInvert` escolhe
  `lerp(rampa, cor, rampa.a)` ou `lerp(cor, rampa, rampa.a)`;
- especular (`csAddSpecular`, 1 TXMT): rampa `csSpecRampTexture` em
  `u = (N·H)^csShininess` (via `lit`), por cima pelo alfa dela;
- **contorno** (`csAddOutline`, 178): um passe antes, com as faces de trás
  (`cullCounterClockwise`), a posição empurrada `csOutlineWidth` ao longo da
  normal em espaço de câmera e `z += 0,005·w`, cor `csOutlineColor`, alfa
  `csOutlineAlpha` (blend só se < 1), z `<`; o passe principal usa `≤`;
- o env map (`csAddEnvironmentMap`) é 0 em todos.

**No app** (rodada n22): o shader segue o script — a linha da rampa pelo
alfa do vértice (só com cor no vértice) ou pelo `v` do UV 1/UV 0 (antes
`v = 0,5` fixo), `sat(4·tex·sat(0,25·difusa·rampa))`, a borda com
`csEdgeLightDamping` e o alfa invertido ou não, a rampa especular
(`csSpecRampTexture`, `csShininess`, textura 21) e o contorno como um item a
mais antes da parte (faces de trás, posição empurrada `csOutlineWidth` ao
longo da normal, `z += 0,005·w`, cor e alfa do contorno). O app ainda não
desenha esses ícones 3D (os painéis de desejos e memórias usam as imagens
2D), então a prova é o teste `render` (a linha e a coluna da rampa).

### 13.5 A conversão de altura em normal map, achada no base 1.0 (rodada n50)

Lido com o Ghidra MCP no base 1.0 (`sims2_base_1.0.ppc`, só leitura; o BV
não tem os nomes, e as funções não foram achadas lá). A conversão **está no
binário de Mac** (o §12 e o §13.2 diziam que não):

- **Quem converte**: a criação de uma textura a partir de um `cImageData`
  (`FUN_003218ac`: formato 6 ou 7 marca a textura como convertida e cria o
  descritor, `FUN_00324978`) e o envio (`FUN_00322058`): numa textura 2D
  (tipo 4) com a imagem no **formato 6** (`Raw8`, luminância de 8 bits,
  `cImageData::PixelFormat` BV `0xc97358`) **ou 7**, chama a entrada `0x34`
  do serviço de imagem (`FUN_00251020`, vtable `0x158ac00`: `0x10a410`)
  com o **bump scale da imagem** (vtable `0x90`); as outras vão pela `0x1c`
  (cópia). Os cubos (tipo 6) não convertem. Ou seja: **toda** TXTR `Raw8`
  vai para a placa como normal map, seja qual for o uso (o compositor de
  pele, `FUN_0020c3a8`, também converte o formato 6 antes de compor).
- **Por nível** (`FUN_00103c40`): cada nível de mip convertido a partir do
  seu próprio nível de altura, por `FUN_0010379c`
  (`GenerateBumpMap_L8_to_A8R8G8B8`):
  - `f = 1020 / bump scale` (constante `1020` em `0x15816c8`; 102 com o 10
    de todas as TXTR do jogo base), tabela de 16384 entradas
    `T[i] = 127,5 / sqrt(64 i + f²)` pela raiz inversa rápida (constante
    mágica `0x5f400000`, um passo de Newton `y·(1,5 − 0,5·x·y²)`, em
    precisão simples: o valor sai ~1,2% abaixo do exato);
  - por texel, com as alturas `H` dos quatro vizinhos diagonais (as bordas
    dão a volta): `a = H(x−1,y−1) + H(x−1,y+1) − H(x+1,y−1) − H(x+1,y+1)`,
    `b = H(x−1,y−1) + H(x+1,y−1) − H(x−1,y+1) − H(x+1,y+1)`,
    `s = T[(a² + b²) >> 6]`;
  - `R = 128 + round(a·s)`, `G = 128 + round(b·s)`, `B = 128 + round(f·s)`
    (o truque de 1,5·2²³: arredonda para o mais próximo), cada um preso em
    0–255, `A = 255`. Uma altura plana dá (128, 128, 254).
  Em altura de 0 a 1, a normal é ∝ (−10·∂h/∂x, −10·∂h/∂y, 1) com o bump
  scale 10: cinco vezes a força 2 por texel que o app usava (inferida).

**No app**: `heightMapToNormalMap` (`formats/image.*`) faz a mesma conta
(a tabela com a mesma raiz rápida) e o `Renderer::texture` a aplica a toda
TXTR `Raw8` 2D antes de enviar; o bump scale vem da TXTR
(`ImageData::Texture::bumpScale`, que o leitor chamava `unknown`). O pixel
shader lê o normal map como o `_bx2` do ps_1_1 (`2t − 1`), sem a derivada
de altura de antes (`kBumpStrength` saiu). Testes: `test_formats` (plano,
rampa, a volta na borda, um nível pequeno), `test_render` (o passe de bump
com o normal map plano do jogo). Os objetos com bump mudam pouco no
caminho DX8 do Mac (o relevo só entra pelo `n_z` e pelo `−(nx+ny+nz)` da
2ª luz, §13.2): N002_Lot10, os telhados até 3 de um canal; N001_Lot11, 60
pixels.
