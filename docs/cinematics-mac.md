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
  imagem do lote/família (`0xACDC0021`–`35`) no ladrilho do meio. O mapa
  caso → animação está no §2.1 (rodada 10).

### 2.1 Tela de entrada: caso → Reia, ícone e título (rodada 10, BV `0x23be6c`)

`SetupWindow(reia, ícone, buffer)`; "família" = a família do lote (ou a
selecionada); títulos são chaves (grupo, instância, índice) de texto:
`T(i, n)` = (`0x7FA7DC7C`, i, n), `U(i, n)` = (`0x7F3A262A`, i, n); o tipo de
bairro 2 (BV) troca `T(7, …)` por `T(200, …)`.

| caso (`+0x14`) | condição | Reia | ícone / imagem | título |
|---|---|---|---|---|
| 0 | o lote tem família | `0x8DA3ADE8` | — (foto da família, `CreateFamilyThumbnail`: imagem 320×320 da família mostrada em 360×360) | `T(7, 4)` com o nome da família; subtítulo `U(0x84, 0x10)` |
| 0 | sem família, lote tipo 4/6 (BV) | `ADEA` | `0xACDC0023` com a imagem do lote (`GetCommunityLotImage`) se achou, senão `ADF1` | nome do lote |
| 0 | sem família, residencial (tipo ≠ 1, 5) | `ADE9` | `ADF2` (ferramentas de construção) | nome do lote |
| 0 | sem família, comunitário | `ADEA` | sem lotes comunitários no bairro (`+0x5c`→`0x68` = 0) `ADF2`; com o objeto `0x9327CE01` no lote `0xACDC0023` + imagem; com o `0x13203C11` `0xACDC0035`; senão `ADF1` | nome do lote |
| 1 | — | `ADEC` | `ADF0` (`ADF5` no bairro tipo 2) | `T(7, 3)` |
| 3 / 5 | bairro atual (tipo `0x48`) | `ADEB` | tipo 2 → `ADF6`; 3 → `ADF8` (caso 3) / `ADF7` (5); 4 com `0x58` = 3 → `ADF9`/`ADFA`; 5–7 → `0xACDC0021`/`22`; outro (o jogo base) → `ADF3`; bairro "Tutorial" → `ADF4` sem títulos | nome do bairro; subtítulo `U(0x84, 0x10)` (caso 3) ou `U(0xCD, 0x13)` (5) |
| 3 / 5 | o jogo guarda um lote de destino diferente do atual | `ADEA`/`ADEB` com a lógica da imagem comunitária acima | | |
| 6 | com família | `ADEB` | — (foto da família) | `T(7, 4)` com o nome |
| 6 | sem família | `ADEB` | `ADF3` | nome do bairro, `U(0x84, 0x10)` |
| 7 | — | `ADED` | `ADF4` | `U(0x84, 0x2C)`, `U(0x84, 0x10)` |

A imagem do lote comunitário (`GetCommunityLotImage`, BV `0x23b970`) é a
miniatura 320×320 (`0x20732180`) de um objeto do lote cujo catálogo tem o
bit `0x20` do campo `0x5E`, criada pela câmera de miniaturas. Os números de
caso vêm de quem cria a janela (não seguido): pelo conteúdo, 0 = entrar num
lote, 3/5 = carregar um bairro, 6 = voltar ao bairro com uma família, 7 =
tutorial (inferido).

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

## 5. No opensims2

- `.movie` e Reia: `formats/movie.*`, `formats/reia.*`; tocados em tela cheia
  por `app/movie_host.hpp` (`playMovie(nome, onDone)`, `playMovies`,
  `introMovies()`, `"reia:<instância>[:loop]"`), com o som numa saída
  própria e a imagem pelo relógio do som (`media/movie_player.*`). `--movie`,
  `--intro`; `s2pkg movie-*`, `reia-*`.
- CINE: `formats/cinematic.*`, `s2pkg cine-list`. A primitiva grava
  `vm::cinematicRequests()` (cena e atores resolvidos); o app
  (`app/cinematic_camera.*`) põe a câmera na junta `cinecam` composta com a
  colocação do ator de origem (Sim: a do `SimActor`; objeto: posição da VM e
  direção do dado 1), olhando pelo +Y da junta com +Z para cima (inferido: com
  −Z/+Y de câmera do Maya o primeiro beijo olharia para o chão; com +Y os
  planos saem por cima do ombro, em altura de cabeça), FOV do
  `viewersGroup` (radianos; rodada 15: ângulo horizontal no 4:3, a altura
  do 4:3 mantida em janelas mais largas, `docs/camera-ui-mac.md` §1.1) e as
  barras do letterbox (pós-processamento por cima da imagem, não mudam a
  projeção).
- Rodada 9 (`app/cinematic_player.*`, `render/screen_fx_layer.*`): a cena
  inteira como o §6 descreve: atores auto-criados (táxi, OVNIs, plumbobs
  da cena) com as animações da lista em sequência a partir da origem, as
  animações automáticas de atores existentes (bebê, OVNI, telescópio, cama:
  objetos pelo mesmo caminho do Animate Object), o rig de luz (não é
  desenhado: as luzes `point/spot{1,2,3}{a,b}` dele, posição pelos ossos +
  canais de mesmo nome, cor = `diffuse` × `diffuseScale`, entram como luzes
  pontuais dos atores cujos grupos de render têm o bit 1/2/4; spots como
  pontos, inferido; rodada 15, binário: as luzes do rig no CRES são
  `DiffuseScale 0, RelSpecularScale 1, cor (1,1,1), FalloffRate 0` +
  alcance 20 (pontos) ou cone 0,698 rad (40°), penumbra 0,1745 (10°),
  alcance 0 (spots) — a ordem de `cStandardLightBase::ReadFrom`, BV
  `0xca2fb4`/`0xca3358`/`0xca3208`; o alcance do arquivo é refeito pela
  intensidade, e sem queda é 10000: as luzes do rig não caem com a
  distância. Antes se lia o 2º valor como intensidade e o alcance 20),
  as câmeras em sequência (`-1_anim`, `-2_anim`), os
  cortes (chaves na mesma hora), o espelhamento (flag 0x01, ou 0x02 com o
  temp 0 ≠ 0: a câmera, o rig e, desde a rodada 10, os ossos dos atores),
  pós-processamento (letterbox, vinheta, granulado e um "bloom" barato:
  cena × w + rgb como clareamento, inferido), crossfade de instantâneo
  (tela modo 4, 2 s, força 1 → 0) nos eventos `crossFadeSnap*` da câmera e
  quando a cena acaba, e os eventos de efeito (tipo 8) das animações de
  todos os atores (`LotEffects::animationEffects`: `firstKissSpiralFx`,
  `kissMomentTeen`, os feixes dos OVNIs). Tudo no relógio da cena. Sem
  "Start Animations Now" (0x04) a cena espera até um ator começar uma
  animação (onSimAnimation); a espera de 1 s que havia aqui era nossa e saiu
  na rodada n34 (§9: quem começa a cena é o `try_animate_sim`). `--cinematic
  <cena>[@id,id...][:mirror]` (atores na ordem do CINE; sem ids, os Sims do
  lote nos atores Sim e um objeto do lote com o modelo do ator, p. ex. a
  caixa de correio; como ajuda de teste os Sims tocam a parte deles,
  `<idades>-cinematic-<ator>-<cena>_anim`, que no jogo vem da árvore).
  Capturas: `docs/screenshots/r9-cinematic-firstkiss.jpg`,
  `r9-cinematic-abduction.jpg`, `r9-cinematic-movein.jpg`.
- Rodada 10:
  - **Espelhamento dos ossos**: as animações da cena (as listas automáticas
    dos atores criados, dos Sims e dos objetos) tocam espelhadas com a
    câmera e o rig. O espelho é feito no espaço do modelo, pelo plano YZ do
    esqueleto (+Y é a frente do Sim): cada osso toma o reflexo do movimento
    do seu par (`l_*` ↔ `r_*`) em relação à pose de repouso,
    W'ᵦ = S (Wₚ Rₚ⁻¹) S Rᵦ (S o reflexo), e volta a local; assim a malha
    sai a imagem espelhada da não espelhada qualquer que seja a convenção
    dos eixos das juntas (`mirrorPose`, `AnimPlayer::Options::mirror`; a IK
    da camada roda antes do espelho). A VM também guarda a flag de espelho
    do Animate Sim (byte 2 `0x01`, ou com o byte 8 `0x01` o temp 3; inferido
    que o temp substitui a literal) e o app toca a animação espelhada.
  - **"Mostrar a casa inteira"** (flag `0x08`; `movein_cine` sempre): na
    cena as paredes sobem (o modelo do lote é refeito com paredes altas se o
    lote estava em corte/baixas) e a câmera vai ao andar mais alto; a
    **velocidade** é forçada (normal e sem pausa, inferido: pausado, o relógio
    da cena pararia) durante a cena inteira; tudo volta no fim (Stop).
    `--cinematic <cena>[:mirror][:house]`.
  - O `moveIn` que "terminava dentro da casa" no `N001_Lot11` não era a
    orientação: sem a VM (`--cinematic` sem `--run-vm`) a caixa de correio
    não tinha posição e a cena caía no primeiro Sim (no jardim); agora usa a
    posição do objeto carregado. Com a caixa em (31,5, 14,5) o táxi para na
    rua e a câmera termina na calçada olhando a casa (com e sem a VM).
  - Capturas: `docs/screenshots/r10-cinematic-movein.jpg` (antes/depois,
    `--lot N001_Lot11 [--walls down] --cinematic moveIn --time 8 --frames 4`),
    `r10-cinematic-espelhada.jpg` (`--cinematic firstKiss:mirror`: antes o
    beijador de costas para a câmera espelhada, depois a imagem espelhada da
    cena normal).
- Rodada 11 (cinemáticas no jogo de verdade):
  - **Por que ficava "esperando os atores"**: sem tela, o app só olhava os
    pedidos da VM (`updateCinematic`) uma vez por captura, depois de rodar
    todos os ticks até ela: a cena do bolo (tick 1199) era preparada na
    captura seguinte e nunca começava. Agora `updateCinematic` roda a cada
    passo da VM entre as capturas, e a cena começa **no momento do tick em
    que a árvore a pediu** (`LotSimulation::tickTime`), mesmo que o app a
    veja depois; sem "Start Animations Now" ela começa com a primeira
    animação de um ator a partir desse tick (o `onSimAnimation` lembra a
    última de cada Sim, porque a do Sim chega no mesmo tick do pedido) ou um
    segundo depois. Os pedidos levam um número de série (a lista guarda só
    os 32 últimos: depois de 32 cenas o app deixava de ver as novas).
  - **Paredes pela câmera da cena** (`cinematicLotView`): enquanto a cena
    toca, o corte das paredes é o da direção da câmera da cena, não o da
    câmera do jogo; "mostrar a casa inteira" continua com o
    `forceCinematicState` da rodada 10. Durante a cena não há UI (a
    não ser um diálogo que a VM espera) nem o plumbob do Sim selecionado
    (inferido: as cenas que mostram um plumbob trazem o seu,
    `plumbobCinematic`). O som abaixa (efeitos 0,2, vozes 0,3, música 0,1)
    no começo e volta no fim, como o `PlayCinematicScene`/
    `UpdateCinematicScene` (`docs/audio.md`).
  - Cenas alcançáveis na VM: o **aniversário com festa** (`--vm-create
    0x2C91C16A:2=1 --pie "Birthday Cake"`: `ageTransTeen2AdultMale`, o Johnny
    toca `t-cinematic-teen-…` e vira adulto na cena) e o **nascimento**
    pela lápide de teste (`--vm-create 0x0CB53A3A@24,21,2 --vm-queue
    "Tombstone:CINECAM BABY"`, "Birth Cinecam": a árvore faz o bebê e pede
    `homebirth`; os plumbobs da cena e o bebê como ator). O `moveIn` pela VM
    depende do "Controller - Lot Load - Curtain Pull-Down" (GUID
    `0xED7580EF`, que o "CT - Lot Loaded" procura); nenhuma árvore o cria e a
    constante não está no código do Mac: falta achar quem o põe no lote. O
    primeiro beijo não foi tentado (falta um par com o relacionamento certo).
  - Ajudas de teste: `--frames-from <s>` (as capturas espalhadas a partir
    desse momento), `--vm-create GUID[:attr=v]@x,y[,andar]` (põe no tile),
    `--vm-queue <objeto>:<entrada>` (o Sim selecionado enfileira a entrada,
    também as de depuração, de um objeto achado por ID ou nome, mesmo fora
    do mundo); `OS2_DEBUG_CINE=1` lista os pedidos.
  - Capturas: `docs/screenshots/r11-cinematic-aniversario.jpg` (`--lot
    N002_Lot10 --sim-id Johnny --vm-create 0x2C91C16A:2=1 --pie "Birthday
    Cake" --pie-select 0 --run-vm 1700 --pivot 19.8,23.2 --zoom 4 --frames 8
    --frames-from 51`: a cena do tick 1199 até o fim e a volta à câmera do
    jogo), `r11-cinematic-nascimento.jpg` (`--sim-id Jenny --vm-create
    0x0CB53A3A@24,21,2 --vm-queue "Tombstone:CINECAM BABY" --run-vm 600
    --frames 8 --pivot 24,20.5 --zoom 5`).

- Rodada 12 (vida 3; binário + VM):
  - **Mudança (`moveIn`) pela VM.** O "Controller - Lot Load - Curtain
    Pull-Down" (`0xED7580EF`) é um "global sim object" (OBJD tipo 7, campo
    42): o motor faz uma instância fora do mundo de cada um em toda carga
    (`cEdithObjectModule::UpdateSimObjects(1)`, BV `0x52f040`, passo 6 do
    `SplitLoad`; agora `vm::updateSimObjects`, chamado no fim do
    `startSavedLot`). Quem toca a cena é a árvore global **"CT - Lot
    Loaded"** (`0x36D`): no jogo base (Mac 1.0, `0x864e0c`) o
    `cTSGameStateController` a roda, pelo nome, no primeiro objeto da lista
    quando chega a mensagem `0x4C8E7905` ("lote pronto") com o estado do
    jogo em `0x0BEA28C2` (lote); o BV troca isso por "CT - Move In
    Cinematic - Prep" no primeiro global sim object. A árvore acha a
    cortina e, se o "reset lot" da mudança marcou o atributo 1, pede
    `moveIn` (flags `0x24`, atores caixa de correio + Sims). No opensims2:
    `vm::runLotLoaded` (um tick, no menor ID: inferido que é a ordem da
    lista) depois do "reset lot" no app (`LotSimulation`), no `s2pkg
    autonomy` e no `s2pkg pie`. O "Cinematic - Move In" da cortina é código
    morto (GSC 37 "lot transition done" e volta). Prova: `s2pkg
    hood-move-in <cópia>/N001 8 24` e `opensims2 --lot <cópia>/N001/Lots/
    N001_Lot24.package --vm --run-vm 190 --frames 4`
    (`docs/screenshots/r12-mudanca-movein.jpg`: táxi, família na calçada,
    câmera da cena até a porta).
  - **Primeiro beijo pela VM**: um casal novo do CAS (`s2pkg cas-make ...
    --member Bruno:adult:m:spouse`, `hood-move-in`) tem "Beijar.../Primeiro
    Beijo" no menu depois que o Main roda o "Init - Relationships"; `s2pkg
    pie <lote> Bruno <id> --select 0` com `S2PKG_PIE_ALSO="Bruno,<id>,17,600"`
    pede `firstKiss` (atores beijador e beijado) e os dois tocam
    `a2a-cinematic-kisser/kissee-firstKiss` no mesmo tick.
  - **Nascimento de verdade** (gravidez de 3 dias): `S2PKG_HOLD_MOTIVES=80
    s2pkg pie N002_Lot10 Jenny Tombstone --create 0x0CB53A3A@24,21,2
    --select 197 --ticks 200000 --user-dir <pasta>` ("Get Pregnant with
    Pollination"): meia gravidez no tick ~45000, gravidez cheia ~90000,
    trabalho de parto ~135000, `homebirth`, diálogo "Baby Name" (tipo 23),
    o bebê no colo e "Entregar Recém-Nascido a".

## 6. O que o binário faz com a cena (Mac BV)

- `TryCinematic` (0x00e24960) **não espera**: chama
  `TSSGSystem->PrepCinematicScene` (vtable +0x6C, `FUN_007c82ec`) com
  (nome, atores, espelhar, flags>>2&1, flags>>3&1, flags>>7) e devolve
  verdadeiro se ela deu certo, falso se não, ou lança o erro que ela
  devolveu. A árvore segue na hora; o fim da cena não volta para a VM
  (as árvores tocam as animações dos Sims, da mesma duração).
- `PrepCinematicScene`: carrega `<cena>_cine` e as animações da câmera
  (`"%s-%1d"` quando há mais de uma sequência), resolve os atores (erros
  0x87–0x8F), guarda espelhar (+0x167), 0x08 (+0x164: "mostrar a casa
  inteira": nível mais alto e paredes em modo 3, desliga "ObjectHiding") e
  0x80 (+0x165); a cena `movein_cine` força os dois. Marca "pronta"
  (`IsCinematicSceneReadyToPlay`, +0x166) e, com 0x04, já chama Start.
- Start (`FUN_007c3898`, vtable +0x70): por ator, carrega o modelo
  automático, põe na origem os de auto-origem, encadeia as animações da
  lista (cada uma começa no fim da anterior, espelhadas pela flag), liga os
  grupos de render `Cinematic1/2/3` pelos bits 1/2/4; guarda e força a
  velocidade do jogo, guarda o nível/corte das paredes, encadeia as
  câmeras a partir da origem e guarda o pedido da última em +0x128.
- `IsCinematicSceneActive` = há pedido da câmera e ele não acabou. Stop
  (`FUN_007c56b4`, +0x74) restaura velocidade, nível, paredes e
  ObjectHiding e tira os grupos de render.
- `cEventHandlerCinematics::HandleEvents`: eventos tipo 8 com
  `effectName=crossFadeSnapShort/Standard/Long` disparam o efeito (bloco
  de tela modo 4); tipo 0x3F6 é som.

## 7. TVs e fliperamas (`cVideoBroadcastManager`)

- O binário tem `nTSSG::cVideoBroadcastManager` (`ParseIniFilesInSegment`,
  `AddReceiver`, `GetGroupIdFromPath`) e `cVideoBroadcastChannel`
  (`InitializeBroadcastKeyLists`, `ChooseNextMovieKey`, `Start`/`Stop`,
  `CreateProceduralTexture`, um emissor de som "proxy" para os receptores)
  **[bin, nomes]**. Os canais vêm de `Res/UserData/Config/Broadcast.ini`
  (`tvstation_sbn`, `_music`, `_children`, `_food`, `_workout`, estática;
  `Folder`, `CommercialFolder`, `CommercialInterval=60`) e de
  `Res/Movies/Games/INI/*.ini` (fliperamas: `IsGames=1`, um `File`).
- Uma TV liga um canal com **Change Material**: o "CT - Turn TV On" usa o
  STR# 0x88 do objeto do canal (grupo `0x7F8BAE91`: `tvstation_sbn --
  This is defined in Broadcasts.ini`) no grupo de malha da tela **[dados]**.
- No opensims2 (`app/tv_broadcast.*`, rodada 9): um material com nome de
  canal vira um material sem luz cuja textura (`movie:<canal>`) é uma
  textura dinâmica do renderizador (`Renderer::setDynamicTexture`) com o
  quadro do canal: os programas da pasta em ordem (rodada 10, como o
  `ChooseNextMovieKey` do binário, `docs/audio.md`), com um comercial
  sorteado (sem repetir o último) entre cada dois — o `CommercialInterval`
  não é usado —, ou o arquivo do fliperama em laço; todas as TVs no mesmo canal
  mostram o mesmo quadro. Som (rodada 10): o som do próprio filme
  (`SCHl`/`SCDl`), decodificado quando o filme começa e tocado, posicional,
  do **primeiro receptor** do canal (a TV de menor ID cujo material é o
  canal, `vm::Object::materials`; as outras ficam mudas, como o proxy
  `cVideoBroadcastSoundEmitterProxy` do binário, `docs/audio.md`), a partir
  de quanto o programa já andou; o próximo programa recomeça, a TV
  desligada (ou noutro canal) esvazia em 0,2 s (`TvBroadcast::sound`,
  `LotAudio::tvSounds`). Nossos: o grupo de mixagem (efeitos) e o volume
  cheio; com a velocidade do jogo acima de 1 a imagem segue o relógio da
  cena e o som não (só ressincroniza no programa seguinte). Alguns filmes
  (`tv_news_police.wav.movie`) não têm som.
  Captura: `docs/screenshots/r9-tv-canal.jpg` (`--lot N001_Lot49 --sim-id
  347 --pie "Television - Floor - Moderate" --pie-select 0 --run-vm 1500
  --frames 3 --walls cutaway --pivot 24.5,7.5 --zoom 5 --orientation 2`).

## 8. Rodada n23: o que o binário faz no Start/Stop e o que sobrou

Leitura de 2026-10-15 (`docs/inventario-historico.md` §6, linha 23). Modo:
**Ghidra MCP** e **Ghidra sem janela** (clone APFS do projeto, só leitura:
`FUN_007c3898` Start, `FUN_007c56b4` Stop e `FUN_007c82ec` Prep não existem
como funções no projeto). Endereços BV.

- **O andar.** O Start guarda o andar atual (`+0x154`) e põe o do **ator de
  origem** (`+0x12C`, o `OriginActorIndex` do CINE: o ator que a câmera e o
  rig seguem; vtable `0x90` do objeto, menos 1 se > 0), ou o mais alto do
  `WorldDB` (vtable `0x144`) com "mostrar a casa inteira" (`+0x164`). O
  `opensims2` usava o primeiro ator (no `ProposeAccept`, cuja origem é o
  ator 1, o andar errado). As paredes: guarda (`+0x158`) e põe o modo **3**
  (erguidas com telhado) com a casa inteira, senão **1** (cortadas; os modos
  do `cITSMainToolbar::eWallMode`, `ui-live-mac.md` §8c). Uma propriedade
  (`+0x194`, desligada) pula o andar e as paredes — confere com o que o
  `forceCinematicState` já fazia.
- **"ObjectHiding".** É a opção gráfica "Object Hiding" (o grupo `0xA` da
  janela de opções, lida em `ApplyChanges`/`RestoreDefaultsWithoutApplying`
  do painel): com a casa inteira o Start guarda o valor (`+0x160`) e o põe
  em **1**; o Stop devolve. Fora isso não mexe. No `opensims2` o fade de
  objetos não depende da opção e fica desligado durante as cenas (a câmera
  de cinema tem o seu, `objectFadeCineCam`, `cObject::ComputeCinematicFade`,
  não feito): nada a mudar até o fade da câmera de cinema existir.
- **A "velocidade forçada".** O Start **não** chama o `cTSSimulator`
  (`SetSpeed`/`Pause` ficam de fora): ele guarda o valor da vtable `0x74`
  do objeto em `+0xB4` e chama a `0x5C(1)`; o Stop chama a `0x60(true)` e
  devolve o valor. Se esse objeto é o `cCustomAnimTimer2` (o relógio de
  animação, vtable `0x27e4fd4`), são o `MaxElapsedTicksPerFrame` e o
  `SetTimeClampingEnabled`: o tempo que a animação pode andar por quadro
  (`UpdateFrameTime`, BV `0x73ea10`). O tipo do objeto não foi confirmado.
  O `opensims2` segue pondo o jogo em velocidade normal e sem pausa durante
  a cena (**inferido**, nosso).
- **Espelho pelo temp 3.** Confirmado: o `try_animate_sim` (BV `0x69d634`)
  com a flag `0x01` do byte 8 usa `temp 3 ≠ 0` no lugar do bit 0 do byte 2;
  o `try_animate_overlay` (BV `0x69ea54`) faz o mesmo com a flag `0x01` do
  byte 15. Era inferido.
- **Sem "Start Animations Now"** (0x04): o Prep só marca a cena pronta
  (`+0x166`); quem chama o Start depois não foi achado (a espera por um
  ator que anime, ou 1 s, segue inferida). **Rodada n34 (§9):** é o
  `try_animate_sim`; a espera de 1 s saiu.
- **Spots do rig.** A direção do cone não foi lida (o rig continua com os
  spots como pontos; a luz das cenas é frente de render). **Rodada n34
  (§9):** o −Z do nó, pela luz do `cSpotLight` num ponto.
- **A câmera e o rig agora pelas regras do ANIM** (`anim-mac.md` §9): as
  rotações da `cinecam` e dos ossos do rig amostradas por quadro e com
  slerp, as tangentes por tick e as chaves assadas a cada 33,3 ticks — o
  que muda o caminho da câmera nas curvas assadas com menos chaves que a
  duração do canal.
- Capturas (antes = o main da rodada, depois = n23; `--lot N001_Lot11
  --cinematic <cena> --time 8 --frames 4`): `docs/screenshots/
  n23-cinematic-movein.jpg` (o táxi e os enquadramentos mudam pouco: até
  2,4 % dos pixels) e `n23-cinematic-firstkiss.jpg` (quase igual).

## 9. Rodada n34: quem começa a cena, o relógio e o eixo dos spots

Leitura de 2026-10-15 (`docs/inventario-historico.md` §7, linha 34). Modo:
**Ghidra MCP** e **Ghidra sem janela** (clone APFS, só leitura). Endereços
BV.

- **Quem começa a cena sem 0x04.** O `TryCinematic` (BV `0xe24960`) chama o
  `Prep` do `cTSSGSystem` (vtable `0x6c`, BV `0x7c82ec`) com o espelho, a
  flag 0x04, a 0x08 e a 0x80; o `Prep`, no fim (`0x7c9064`), com a cena
  pronta (`+0x166`) e a 0x04 chama o `Start` (vtable `0x70`) na hora; sem a
  0x04 só deixa a cena pronta e devolve verdadeiro; sem cena pronta chama o
  `Stop(1)` e o erro `0x97`. Quem a começa depois é o **`try_animate_sim`**
  (BV `0x69d634`, em `0x69e6d8`): depois de pôr a animação do Animate Sim,
  se o Sim está numa cinemática (vtable `0x1fc` do `cAnimatable`, o mesmo
  teste que faz o `try_look_at` dar falso) e o `cTSSGSystem` tem uma cena
  pronta (`IsCinematicSceneReadyToPlay`, vtable `0xdc`, `+0x166`), chama o
  `Start(deslocamento de tempo)` e lança o erro dele. Não há espera de 1 s
  (era nossa, inferida): a cena começa no primeiro Animate Sim de um ator.
- **O relógio.** O objeto em `+0xB4` do `cTSSGSystem` é o relógio de
  animação do scene graph (o `0xd0` do `cTSSGSystem`, BV `0x7b3788`, devolve
  `+0xB4`; o mesmo que o `try_look_at` e a locomoção leem pelo `0x38` da
  base virtual), um `nTSSG::cCustomAnimTimer2` (vtable `0x27e4fd4`, o nome
  do typeinfo `0x2aa40b8`). Vtable `0x74`/`0x5c` lê/escreve `+0x24`, `0x60`
  escreve o byte `+0x58`. No `UpdateFrameTime` (BV `0x73ea10`, `+0x1c` da
  vtable): `+0x58` liga o corte; com ele, o tempo real de um quadro (ms do
  `cRZTimer` em `+8`) fica em no máximo `+0x24`, o que passar de `+0x20` vai
  para os quadros seguintes (até 1000), e o tempo de animação do quadro
  (× a escala `+0x1c`) em no máximo 200. O `Start` guarda `+0x24` em
  `+0x168` do `cTSSGSystem` e põe **1**; o `Stop` (sem o argumento) chama
  `0x60(0)` (desliga o corte) e devolve o `+0x24`; o outro ramo do `Stop`
  chama `0x60(1)`. O simulador (`cTSSimulator`) não é tocado. Quem liga o
  corte fora daí e a escala `+0x1c` não foram seguidos: o efeito na
  velocidade da cena segue sem conclusão, e o `opensims2` mantém a sua
  (velocidade normal, sem pausa durante a cena; nossa).
- **O eixo dos spots.** O `cSpotLight` (vtable `0x2822a0c`) mede a luz num
  ponto no quadro da luz (vtable `0xa4`, BV `0xc9f3ac`, usada pelo
  `RepresentativeIrradiance`, `0xc9f908`): d² = |p|², cos = −p.z / d; cheia
  a partir do cosseno interno (`+0x78`), nada abaixo do externo (`+0x74`),
  linear entre eles, × I / (1 + C·d²) dentro do alcance. **O spot aponta
  para o −Z do nó** (era inferido dos modelos): vale para os spots do rig
  das cinemáticas e para as luzes práticas dos objetos. (À parte: o
  `cDiffusionManager::BuildLightFromObjectLight`, BV `0xd7d404`, usa o +Z
  do nó — `DAT_02d59e84` = (0, 0, 1), inicializador `0x2534a04` — só para a
  direção no chão das luzes nos mapas de luz; e o `MacApplyLightSettings`
  do OpenGL, BV `0x21db0`, passa o Theta do D3D como `GL_SPOT_CUTOFF` em
  graus, ≤ 90, com expoente; isso é da frente de luz.)
