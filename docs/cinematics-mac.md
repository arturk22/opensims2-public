# Vídeos, animações de UI e cinemáticas (binário de Mac BV 1.10)

Fontes: os dados do CD base de 2004 (`TSData/Res`) e o executável PowerPC do
Bon Voyage para Mac (`references/bv_mac/sims2_bv.ppc`, projeto Ghidra
`~/sims2`, lido em cópia APFS com `analyzeHeadless -readOnly -noanalysis`).
Endereços são do `sims2_bv.ppc`. "Inferido" marca o que não foi confirmado no
código.

## 1. `.movie`: vídeo MAD da EA + som EA ADPCM

Os 70 arquivos `.movie` (`Res/Movies/**`, o logo da EA em
`Res/Locale/<idioma>/Movies/ealogo_audio.movie`) **não são VP6**: são o vídeo
"Madcow" (MAD) da EA num fluxo de chunks EA, o mesmo que o FFmpeg chama de
`ea` (demuxer) + `eamad` (vídeo) + `adpcm_ea_r1` (som). Decodificador próprio
em `src/formats/movie.*`.

Chunks: tag ASCII de 4 bytes, u32 LE com o tamanho incluindo os 8 bytes do
cabeçalho.

| tag | conteúdo |
|---|---|
| `MADk` | quadro intra |
| `MADm` | quadro inter; vira a referência |
| `MADe` | quadro inter que é mostrado mas não vira referência (TVs) |
| `SCHl` | cabeçalho do som: `PT\0\0` + valores marcados |
| `SCCl` | u32: número de `SCDl` |
| `SCDl` | bloco de som |
| `SCEl` | fim |

Quadro MAD (depois dos 8 bytes): 4 bytes 0; u32 duração do quadro em ms,
ponto fixo 16.16 (`0x0042AAAB` = 66,67 ms = 15 qps; `0x00215555` = 30 qps na
intro); u16 largura, u16 altura; um byte (2); a escala do quantizador; 2 bytes;
e o fluxo de bits em palavras de 16 bits little-endian lidas do bit mais alto.
Macroblocos 16x16 em ordem de varredura, 4:2:0 (4 Y, U, V). Em quadros inter,
cada macrobloco começa com `1` (os 6 blocos copiados), `01` + 6 bits (máscara
de blocos copiados) ou `00` (todos intra); o vetor de movimento vem em seguida
(cada componente: `0` = 0, senão `1`, sinal, 4 bits: ±1..16 pixels; o croma
anda metade, truncado para zero) e cada bloco copiado traz o seu deslocamento
de DC no mesmo código, vezes 2. Bloco intra: DC de 8 bits com sinal (+128) ×
`q[0]`; AC com a tabela B.14 de run/level do MPEG-1 (sem a forma curta do
primeiro coeficiente), fim de bloco `10`, escape `000001` + nível de 10 bits
com sinal + run de 6 bits; nível × `q[i]` >> 4, forçado a ímpar. `q` é a
matriz intra padrão do MPEG-1 escalada pelos fatores AAN e pela escala do
quantizador; IDCT inteira da EA (a das TGQ/TQI). Cores: YCbCr BT.601 de vídeo
(16–235; inferido, as imagens saem certas).

Som (`SCHl`): depois de `PT\0\0`, pares (tag, tamanho, valor big-endian); a
tag `0xFD` abre o sub-cabeçalho: `0x80` revisão (1), `0x82` canais (2),
`0x83` compressão (ausente: pela revisão), `0x84` taxa (ausente: 22.050 Hz),
`0x85` amostras, `0xFF` fim. `SCDl` (EA ADPCM revisão 1): u32 amostras por
canal, u32 deslocamento de cada canal (a partir do fim dessa tabela), e por
canal s16 atual, s16 anterior e quadros de 15 bytes = 28 amostras (byte de
coeficientes/shift + 14 bytes de nibbles; coeficientes 0/240/460/392 e
0/0/−208/−220, /256).

Conteúdo: o logo da EA (512x384, 3,2 s, 15 idiomas), a intro
(`intro_eng_audio`, 720x544, 30 qps, 47,5 s; o som dura 48,6 s), 6 comerciais
e ~25 programas para as TVs e os fliperamas (128x128, 15 qps). O som de
`tv_mvid_jumpers` dura 37 s para 230 s de vídeo (dados assim). O exe só traz
os nomes `intro_eng_audio.movie` e `ealogo_audio.movie`; a ordem logo → intro
é inferida.

## 2. Reia: animações de UI

Sete recursos do `ui.package` (tipo 0, grupo `0x499DB772`, instâncias
`0x8DA3ADE7`–`0x8DA3ADED`; extensão `.reia` no Mac). Decodificador em
`src/formats/reia.*`.

- RIFF, forma `Reia`. Chunk `head` (24 bytes): versão (1), largura, altura,
  escala de tempo (1.000.000), duração do quadro nessa escala (33.333 =
  30 qps), número de quadros. `cTSWinReiaPlayer::SetSource` (0x001f4e58)
  confere `RIFF`/`Reia`/`head`, tamanho ≥ 24 e versão 1, e cria uma textura
  com largura e altura arredondadas para múltiplos de 32.
- Um chunk `frme` por quadro: blocos de 32x32 da esquerda para a direita e de
  cima para baixo (os da borda inteiros, cortados ao desenhar). Por bloco um
  byte: 0 = igual ao quadro anterior; 1 = seguem 1.024 pixels BGR em PackBits
  (byte c < 128: c + 1 pixels literais; c ≥ 128: um pixel repetido 257 − c
  vezes) que **se somam** ao quadro anterior, por canal, módulo 256 (o primeiro
  quadro soma ao preto). Sem som.

Onde o jogo usa: `cTSWinReiaPlayer` (CLSID `0x8DA279CB`) não aparece em nenhum
script de UI; é criado pelo código:

- `cTSWinProcGameLoad::onTick` (0x002308b8): a tela de carregamento do jogo
  toca `0x8DA3ADE7` (800x600, 5,1 s).
- `cTSWinProcLotIntro::onAttach` (0x0023be6c) → `SetupWindow(reia, ícone,
  buffer)`: a tela de entrada no lote toca uma de `0x8DA3ADE8`–`ED` (820x620,
  3,7 s, que terminam no quadro em que começam) conforme o modo de entrada
  (campo +0x14: casos 0, 1, 3/5, 6, 7) e o tipo de lote (comunitário,
  vazio, da família, de outra família), com um ícone IMG do mesmo grupo
  (`0x8DA3ADF0`–`FA`, p. ex. `F2` = ferramentas de construção) ou uma
  imagem do lote/família (`0xACDC0021`–`35`) no ladrilho do meio. O mapa exato
  caso → animação não foi seguido.

## 3. CINE (`cCinematicScene`)

Tipo `0x4D51F042`, os 23 em `Sims3D/Sims01.package` (grupo `0x1C0532FA`),
nome `<cena>_cine`. RCOL com um bloco `cCinematicScene` (versão 4). Leitura e
escrita: `FUN_00d36774` (ReadFrom) e `FUN_00d35d1c` (WriteTo), achadas pela
vtable em 0x02832358 (typeinfo `N14nGZSceneDBAnim15cCinematicSceneE`); o
WriteTo dá o nome de cada campo (`ActorName(%d)`, ...). Decodificador em
`src/formats/cinematic.*`, `s2pkg cine-list` (os 23 sem falha).

```
u32 versão                       (4 nos dados)
cSGResource                      (nome "<cena>_cine")
str7 animação da câmera          ("o-cinecam-<cena>")
[versão 3: dois str7 lidos e descartados]
u32 número de atores, por ator:
  str7 ActorName                 ("kisser", "sim", "baby", "tsCineLightRig.i")
  u8   IsActorRequired
  str7 ActorSGObjectName         ("auskel::1": esqueleto :: instância)   [versão ≥ 3]
  u32  ActorCinematicRenderGroups                                      [versão ≥ 3]
  u8   IsActorAutoCreate                                               [versão ≥ 3]
  u8   IsActorAutoOrigin                                               [versão ≥ 3]
  str7 ActorAutoModel            ("cineLightRig-template", "vehicleTaxi")[versão ≥ 3]
  str7 ActorAutoAnimList         (separada por vírgulas, uma por sequência)[versão ≥ 3]
u32 OriginActorIndex             (o ator que a câmera e o rig de luz seguem;
                                  1 em ProposeAccept, 0 nos outros)
u32 NumSequencedAnimations       [versão ≥ 2] (2 em alienBirth/hotTub: "-1_anim,-2_anim")
```

As cenas: envelhecimento (bebê→criança, criança→adolescente M/F,
adolescente→adulto M/F, adulto→idoso), primeiro beijo (adulto/adolescente),
pedido aceito/recusado (e `ProposeMaleAccept`), casamento aceito/recusado,
mudança (`moveIn`, até 8 Sims + táxi criado), nascimento em casa e
alienígena, abdução e volta, banheira (2 variantes), "fazer amor" na cama, e
`caughtCheating`.

A animação da câmera (`o-cineCam-<cena>_anim`, esqueleto `cinecam`) tem três
alvos: `cinecam` (junta `cinecam`: rotação Euler XYZ em graus e translação,
com o Z para cima como as dos Sims), `viewersGroup` (`fov` em radianos, ~1,04
= 60°; `nearFarClip` 0,01/1000) e `postprocessingmaterial`
(`cameraletterbox` 0–0,5, `camerafilmgrain`, `cameraVignette`,
`cameraBloom`); eventos tipo 8 `effectName=crossFadeSnapShort` marcam os
cortes. O rig de luz (`o-cineLightRig-<cena>_anim`, esqueleto `lightrig`)
anima luzes pontuais e spots.

## 4. Primitiva Cinematic (0x7B, `cTSObject::TryCinematic` 0x00e24960)

Operandos (o `XPrimParam` tem os operandos a partir do byte 4; u16 nos
operandos 0–1, 3–4, 7–8 e 10–11, `CinematicParam::SwizzleParams`):

| operando | uso |
|---|---|
| 0–1 | índice da cena (a partir de 0) no STR# `0x97` "Cinematics" |
| 5 | flags: `0x01` espelhar as animações, `0x02` espelhar se temp 0 ≠ 0, `0x04`, `0x08`, `0x10` índice da cena vem da variável 6/7–8, `0x20`/`0x40` de qual STR# 0x97 (inferido: global / semi-global; sem eles, o do grupo do código), `0x80` |
| 6, 7–8 | variável (dono, dado) com o índice da cena (flag `0x10`) |
| 9, 10–11 | variável com o ID do objeto cujo array guarda os atores ("Using array found in objID in") |

Strings do editor (Sims2.exe): "Using Scene", "Using scene name index found
in", "Using array found in objID in", "Flip Cinematic Anims Horizontally",
"Pass in Flip ... value in temp 0", "Start Animations Now", "Show Entire
House"; as flags `0x04`/`0x08` devem ser essas duas últimas (inferido pela
ordem dos argumentos de `PrepCinematicScene(nome, atores, espelhar, b2, b3,
b7)`). O objeto dos atores tem de existir (erro `0x17`) e ter pelo menos um
ator (erro `0x6C`). Cena + `_cine` é o recurso CINE. Os 15 STR# 0x97 dos
objetos têm o título "Cinematics" e os nomes (`firstKiss`, `firstKissTeen`;
`ProposeAccept`, `ProposeReject`, `ProposeMaleAccept`; `ageTrans...`). 26 usos
nos BHAVs (prim-usage), todos com o índice literal: "Cinematic - First Kiss",
"Cinematic - Propose Accept", "Birth Cinecam", "Stargaze - Abduction",
"Camera - Play In", "CT - Lot Loaded" (flags `0x24`)...

Erros (tabela da VM): `0x87` cena não encontrada, `0x88` animação não
encontrada, `0x89`–`0x8B` ator obrigatório ausente/inválido/sem modelo 3D,
`0x8C`/`0x8D` ator de origem inválido/sem modelo, `0x8E` ator auto-criado não
é 0, `0x8F` ator não animável, `0x97` erro desconhecido, `0x98` já há uma
cinemática ativa, `0x99` câmera sem fila de coreografia.
