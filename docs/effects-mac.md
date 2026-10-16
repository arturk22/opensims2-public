# Efeitos de partículas e balões (Mac BV + dados)

O que o opensims2 sabe do sistema de efeitos do jogo ("Swarm", de Andrew
Willmott) e de onde veio. Código: `src/formats/effects.*` (recurso),
`src/render/particle_system.*` (simulação, C++ puro, lib `os2_fx`),
`src/render/particle_layer.*` (desenho Metal), `src/app/lot_effects.*`
(ligação com a VM), `src/vm/animation.cpp` (`effectStopStart`).
Ferramentas: `s2pkg effects`, `s2pkg effect-sim`, `s2pkg effect-tables`.

Legenda: **[bin]** lido no binário de Mac (Ghidra headless, `sims2_bv.ppc`,
nomes pelo `tools/symxfer`); **[dados]** conferido nos arquivos do jogo;
**[OpenTS2]** layout do OpenTS2 (`EffectsCodec.cs`); **[inferido]** escolha
nossa, a confirmar.

## 1. O recurso

- Um só recurso, tipo `0xEA5118B0`, grupo `0xEA5118B1`, instância 1, em
  `TSData/Res/Effects/effects.package` (1,9 MB descomprimido). **[dados]**
- Layout do OpenTS2, que decodifica o arquivo do jogo base inteiro até o
  nome `end`: versão u16 0; blocos de partículas, meta-partículas, decalques,
  pincéis (0), scrubbers (0), sequências, sons, câmeras, "game" (0), modelos,
  telas, água, luzes (0); versão u16 dos efeitos visuais (3) e os efeitos;
  depois pares (string u32, índice u32) até `end`. **[OpenTS2][dados]**
- Diferença do jogo base: o collider é sempre `flags u32, nome, 3 vetores, 4
  floats, 2 nomes` (o OpenTS2, feito para a Ultimate Collection, lê um byte e
  um float opcionais que aqui não existem). Todas as 1.852 partículas são da
  versão 3 (flags u32). **[dados]**
- Contagens: 1.852 partículas, 322 meta-partículas, 28 decalques, 84
  sequências, 110 sons, 46 câmeras, 69 modelos, 27 telas, 5 águas, 1.636
  efeitos visuais, 1.557 nomes. **[dados]**
- Um efeito visual é uma lista de componentes: nome do bloco, tipo (0
  efeito, 1 partículas, 2 meta, 3 decalque, 6 sequência, 7 som, 8 câmera, 10
  modelo, 11 tela, 12 água), transformação (byte de flags, escala, matriz 3×3,
  offset), LOD início/fim, escalas de emissão/tamanho/alpha (início/fim do
  LOD), grupo e chance de seleção (u16), escala de tempo, índice. **[OpenTS2]**
- Um componente que é outro efeito visual (tipo 0) o **nomeia**: o índice
  guardado é sempre 0 (as 240 referências do jogo base). Lido pelo índice,
  todo efeito aninhado virava o efeito 0, a poeira do cursor de construção
  (o aquário mostrava uma coluna de poeira); rodada 10: resolvido pelo nome
  ao carregar. **[dados]**

### 1.1 Campos das partículas usados

`life` (s, aleatória no intervalo), `preRoll`, curva de taxa (partículas/s)
sobre `rateCurveTime` s repetida `rateCurveCycles` vezes (0 = sempre),
caixa de direção e intervalo de velocidade, volume de emissão (caixa; bit 8
das flags = elipsoide), curvas de tamanho (m) e aspecto com variação, curvas
de rotação (voltas), curva de alpha e lista de cores com variação, textura
(TXTR `<nome>_txtr`), atlas `tileU×tileV` com quadro inicial/contagem/
velocidade, alinhamento, tipo de desenho, camada, força (vetor), segundo
grupo (vetor, f, f), passeio aleatório (atraso, força). **[OpenTS2][dados]**

Tipos de desenho (byte): 0 decal (alpha), 1 decal invertendo profundidade, 2
decal sem profundidade, 3 "depth decal", 4 aditivo, 5 aditivo invertido, 6
aditivo sem profundidade, 7 modulado. **[OpenTS2]** No jogo base: 1.170 tipo
0, 565 tipo 4, 52 tipo 3, 31 tipo 1, 18 tipo 2, 16 tipo 6. **[dados]**

## 2. Effect Stop/Start (0x70) — `cTSObject::TryChangeEffect` @ 0x00e44a6c **[bin]**

Operando (bytes do operando; o XPrimParam do binário soma 4):

| byte | campo |
|---|---|
| 0 | modo: 0/1 começa (suave/forte), 2/3 para, 4/5 para todos, 6 dispara e esquece, 7 interroga osso, 8 para tudo e limpa a fila, 9 para TODOS, 10–13 estado 1–4 (float param 0xF = 0..3), 14 para TODOS suave |
| 1, 2–3 | objeto (dono, dado) |
| 4–5 | efeito (índice); −1 só vale nos modos de parar tudo (senão erro 0x83 "default effect") |
| 6 | slot |
| 9 | tipo do slot |
| 10–11 | flags |
| 12, 13–14 | variável do ícone (dono, dado) |
| 15 | entrada do STR# 0x95 (ícones de atlas) |

- Nome: `GetString(STR# 0x8F, efeito + 1)` (os índices de string do jogo
  contam de 1; no nosso `StringTable` é a entrada `efeito`). Tabela: flag
  0x01 = a do grupo global, 0x02 = a do semi-global, senão a do grupo da
  árvore que roda. Nome vazio = erro 0x4F. Conferido: "Balloon - Display
  Speech Balloon (w/X) of GUID" usa 0x2D = `uiHeadlineGroup_
  ConversationRuntimeThumb_start_x` do STR# 0x8F global. **[bin][dados]**
- 0x08: índice do efeito no temp 0 (não 0x40 como o doc antigo dizia); 0x40
  passa o temp 1 à chamada de início; 0x80 fila de prioridade. **[bin]**
- Slot (tipo no byte 9): 0, 1 e 2 chamam três funções virtuais do objeto
  (0x3E8, 0x3DC, 0x10C) que dão o nó; com tipo 2 e slot 4 o jogo consulta a
  opção `ShowHeadlines` (balões). **[bin]** Pelos dados: tipo 2 = índice no
  STR# 0x90 ("Model - Bones") do objeto — slot 4 dos Sims = `hat_grip`
  (TemplatePerson, grupo `0x7FEDFE16`), o "Events - Fry" do fogão usa slot 1 =
  `slot_stove_0`; tipo 0 = slot de contenção (SLOT tipo 0). O SLOT do fogão
  ainda tem uma entrada de tipo 4 que nomeia `slot_range_smoke_fx` (slot de
  efeito). **[dados][inferido]**
- Ícones (depois de começar, modos 0/1): **[bin]**
  - 0x1000: ícone por ID no temp 6;
  - 0x04: miniatura de objeto (variável 12/13; 0x100 = outro objeto do
    próprio; 0x400 = GUID nos temps 4/5; 0x800 = temp 6; bit 0x200 passado);
  - 0x20: `SetFloatParams(0xD, variável)` e textura = STR# 0x95 (do grupo da
    árvore, semi-global, global) na entrada do byte 15: "Set FX - Topic -
    Speech Balloon" passa 4–9 = `uieffects-headlines-conversationshared01..06`
    (atlas de 16 ícones de 64 px) e a variável é o tópico (a célula);
  - 0x10: retrato de Sim (variável 12/13);
  - 0x2000: ícone de desejo/medo ("BalloonEffectThumbnail_WantFear_0x%08x").
- Estados (modos 10–13, rodada 10): o efeito do índice já ligado recebe o
  estado 0–3 (float param 0xF). O aquário (grupo `0x7F9A625F`) usa 11 em
  "Interaction - Feed Fish" e 12 em "Kill Fish"; o estado escolhe o passo das
  cadeias de meta-partículas (§3). **[dados][inferido]**
- Erros do jogo: 0x17 objeto inválido, 0x4F nome vazio, 0x50 não é objeto
  3D, 0x1B slot inválido, 0x6C interroga osso sem ossos. **[bin]**

Nossa VM guarda `vm::ActiveEffect` (índice, nome, slot, flags, ícone,
série, one-shot) em `AnimationState::effects`; começar de novo o mesmo efeito
troca a entrada (série nova), parar remove, modo 6 entra como one-shot (o
app para depois de 4 s, **[inferido]**).

## 3. Simulação — `cTSParticlesEffect` **[bin]**

Desde a rodada n15 a emissão, a física, o referencial das partículas, as
meta-partículas e a ordem na camada 7 seguem o binário: ver §4e (o que foi
lido) e §4e.8 (o que mudou no código e o que segue inferido). As escolhas
antigas desta seção (arrasto = primeiro float do segundo grupo, bits 0/1 =
"presas ao efeito", pre-roll só nos contínuos, passos de 0,1 s, bits
"quantas ficam vivas"/"cadeia"/"presas ao tanque" das metas, a subida de
0,2 m dos balões) saíram; o que o binário diz no lugar de cada uma está em
4e.1–4e.7.

Outros pontos do desenho e dos componentes:

- LOD: pela distância da câmera desde a rodada n38 (§4f.2; antes, as
  escalas do fim sempre). Alinhamentos: 0 câmera, 1 chão
  (ondas, manchas), 2 velocidade (chuva), 6 eixo Z (jatos de água, feixes);
  5 (anéis, peças do despertador) desenhado como 0.
- Decalques (rodada 9): um quadrado no chão sob o componente, tamanho,
  alfa, rotação e cor pela vida (vida ≤ 0: enquanto o efeito roda);
  modelos: o CRES do bloco desenhado no componente enquanto o efeito roda
  (`LotEffects::placeModels`; peixes do aquário, peças do xadrez), que
  andam com as meta-partículas que os levam. Câmeras e
  água não; as telas (modo 4 = crossfade de instantâneo) só nas cinemáticas.
- Sons (rodada 10): o `audioId` do bloco de som é a **instância** da track
  (CRC24 do nome | 0xFF000000; 31 dos 33 ids do jogo base batem com uma hit
  list, todas de `Sfx/Tracks`: fogo `FF5EF8A5`, moscas `FFFD5C9D`, baratas
  `FF68CBE0` — 189 componentes —, faíscas, fogos de artifício, confete das
  contas, sirenes, a pia quebrada). `EffectSystem::collectSounds` dá os sons
  devidos (do atraso do componente até o efeito parar ou o item da sequência
  acabar); `LotAudio::effectSounds` toca a track pela chave, na posição do
  componente, acompanha o efeito e, se a track faz laço, a esvazia em 0,5 s
  quando deixa de ser devida. Laço inferido: as tracks em laço têm a chave
  `0xff595ffd` nas track settings (`lot_outside_lp`, `water_lp`, fogo,
  moscas, água da pia), as de uma vez não. `flags`, `playTime` e `volume`
  dos blocos (0, 0, 0 em quase todos; `soundtest` tem `playTime` 5) e o
  `locationUpdateDelta` (0,5 s) não são usados. Sem track: `FF14E140`
  (`fxambsiren`) e `FFED2D23` (bola de alma do Ceifador).

## 4. Balões

São efeitos `uiHeadlineGroup_*` no slot `hat_grip` (tipo 2, slot 4) dos
Sims: meta-partícula com a bolha (`uieffects-headlines-conversationballoon`
ou `-thoughtballoon`) e a partícula do ícone (bit 24, atlas 16×1 ou miniatura).
Até a rodada n15 o app subia o balão 0,2 m sobre o `hat_grip` (inferido);
agora quem o sobe é o caminho das meta-partículas (bit 30,
`SnapParticleToPath`: de 0,15 a 0,35 m acima do `hat_grip` em 0,18 s), e o
efeito é `rigid` (anda com o Sim) e `viewRelative`/`cameraFacing` (vira
para a câmera do lote). As partes de um balão vão juntas na ordem dos
componentes (cada emissor é um grupo na display list, §4e.6).
Captura: `docs/screenshots/r8-effects-balloon.jpg` (`--lot N001_Lot11
--run-vm 145 --pivot 6.5,22.5 --zoom 5`).

## 4b. Rodada 9: ícones, slots, efeitos salvos, eventos de animação

- Ordem dos ícones no `TryChangeEffect` **[bin]**: 0x1000, 0x04, 0x20,
  0x10, 0x2000. 0x10: a variável (bytes 12–14) tem o **neighbor ID** do Sim
  (os balões das memórias e dos sociais, 79 usos; inferido pelos valores) e
  o app desenha o retrato dele (IMG pequeno do arquivo do personagem; de um
  vizinho fora do lote, o arquivo da vizinhança com o GUID dele). 0x2000:
  desejo (temp 4 = 0) ou medo do Sim da variável, slot no temp 5 → ícone da
  definição (nenhum uso no jogo base). 0x1000: miniatura pelo ID no temp 6
  ("CT - Handle Feedback - Score" dos desejos; guardado, não desenhado).
  0x04 com 0x100: a miniatura do próprio objeto.
- Rodada n15, as tiras no balão (relato do usuário): a partícula da foto
  (`*objectthumbnailprimitive`, atlas 1×1, bit 24) tem como textura própria
  um atlas de 16 ícones (`conversationshared01`, `motives-thought-02`); o
  `TryChangeEffect` (0x04) pede a foto ao serviço de miniaturas, que no jogo
  sempre devolve uma (ele renderiza a que falta) e a troca
  (`SetUnknownParam(0, nome)`). Aqui, quando o `ObjectThumbnails.package`
  não tinha a foto (o GUID de um Sim, de uma memória), ficava a textura
  própria: o atlas inteiro espremido. Agora: a foto do catálogo, senão o
  ícone da memória (UI image 1 do grupo, como o painel), senão o retrato do
  Sim (personagem com esse GUID); sem nenhuma, o ícone não é desenhado
  (inferido). A célula do atlas dos `*icon16primitive` (16×1) vem do
  `SetFloatParams(0xD)` → +0x16c, o quadro inicial (lido, `0xeb62f0`), que
  o app já seguia. `--effect <nome>#<guid>` dá a foto de um GUID ao balão.
  Captura: `docs/screenshots/n15-balao-icones.jpg`.
- Slot tipo 0 = `GetObjectSlot` (vtable 0x3E8), tipo 1 = `GetTargetSlot`
  (0x3DC), tipo 2 = osso (0x10C) **[bin]**. O PlumbBob só tem uma entrada
  SLOT de tipo 4 (`slot_plumbob_0_fx`) e as flores `slot_flowers_0` para os
  insetos: tipo 0 sem slot de contenção e tipo 1 caem nas entradas de tipo
  4 **[inferido]** (os efeitos salvos nomeiam esses nós).
- **Efeitos salvos** (`cTSObject::ReadEffects`, **[bin][dados]**): por
  efeito, nome (str7), nó (str7) e 8 bytes; o `N001_Lot11` guarda
  `fxPlumbobGlow_100` em `slot_plumbob_0_fx` e `fxBees`/`fxButterFlies`
  das flores. O brilho do PlumbBob é ligado pelo "Set Mood Effect &
  Material" (Effect Stop/Start com flag 0x08: índice = estado de humor no
  temp 0) só **quando o estado muda** (compara com o atributo 0), então num
  lote carregado ele vem do arquivo: `restoreEffects` (vm/lot_resume) põe
  os efeitos salvos de volta na VM. O brilho do PlumbBob escondido segue o
  plumbob que o app desenha sobre o Sim selecionado.
- Eventos de efeito das animações (tipo 8, `effectName`/`boneName`/`op`):
  `LotEffects::animationEffects` para Sims, objetos animados e atores das
  cinemáticas; param no evento de parada ou no fim da animação.
- Capturas: os balões seguem os ossos dos Sims também entre quadros
  (`--frames`: os atores são posados em cada passo da VM).

## 4c. Rodada 16: partículas de malha e texturas que faltam

- **Partícula de malha** **[dados]**: 137 componentes de partículas do
  base têm como "textura" um SHPE, `<cres>_<forma>_shpe`
  (`clothesChangeSwirl_clotheschangeswirl_shpe` das 12 trocas de roupa,
  `effectsSmallSteam_smallsteam_shpe` da fumaça das panelas e cinzas,
  `effectHeart_effectheart_shpe` dos corações do primeiro beijo, os pássaros,
  o raio, os ícones de memória `*Gold_*gold_shpe`). Não há TXTR com esses
  nomes: o SHPE existe (Objects06) e o CRES é o nome até um dos `_` (o mais
  longo que existe: `neighborhood_plumbobShape_..._shpe` →
  `neighborhood_plumbobShape`, `lightning_root_rot_shpe` → `lightning`).
  `ParticleLayer::mesh` desenha a malha da forma (as partes do CRES com
  esse SHPE) em cada partícula, com a textura base do material dela (sem
  textura: a cor difusa), escala = o tamanho da partícula, girada em torno
  do Z pela rotação da partícula, cor/alfa da partícula, a mistura do
  componente **[inferido: o binário não foi lido; as malhas têm ~1 m e as
  curvas de tamanho 0,6–1, o que bate]**. Capturas:
  `docs/screenshots/r16-fx-troca-de-roupa.jpg` (`--lot N001_Lot11 --effect
  clotheschangeswishadult1@6.5,17.5,0 --pivot 6.5,17.5 --zoom 9
  --orientation 4 --time 0.3 --frames 3`).
- **Texturas ausentes do base** (procuradas como TXTR e TXMT em todos os
  pacotes de `Res`, sem diferença de caixa): `waterStream` (o
  `shower_spray3` dos chuveiros; só existe `waterfaucet-waterstream`),
  `waterFaucet_waterStream`, `babyDuck`/`babyduck`, `babyRattle`,
  `pacifier`, `blueBirdPLAGERIZED`, `brokenLampFlash` (há
  `brokenlampflash-alpha`), `fireFlies2` (há `fireflies`),
  `fountain_spoutShader`, `fxViewTarget`, `impact_flash_3b`,
  `lightningSpark`, `sparks`, `balloon`,
  `uieffects-headlines-dreamballoon` (e `-leader1/2`),
  `uieffects-headlines-motives-dream-02`. Ficam brancas (o quadrado do
  componente com a cor e o alfa dele); trocar por nomes parecidos seria
  invenção.

## 4d. fix-plumbob: a "luz estranha no meio" do PlumbBob **[bin]**

O brilho é o `fxPlumbobGlow_<humor>` (`_00`, `_25`, `_50`, `_75`, `_100`,
`_platinum`) no `slot_plumbob_0_fx`: 5 componentes aditivos (tipo de
desenho 4) com a textura `plumbBobGlow`/`plumbBobShine`, todos com flags
`0x800002`, taxa 1, vida 2,5 (os três `glow`) ou 4 (os dois `shine`):
`large` 1,5 m alfa 0,25 verde, `med` 0,75 m alfa 0,45 verde, **`small`
0,33 m alfa 0,8 branco**, e dois `shiner` 0,95 m alfa 0,15 girando ±0,5.

- **A causa**: tratávamos a taxa como partículas por segundo; com vida 2,5–4
  ficavam 2–4 cópias de cada componente somando por cima (aditivo), e o
  `small` branco estourava numa mancha branca do tamanho do diamante.
- **O jogo**: o bit 1 das flags manda para `MaintainParticles`, que mantém
  vivas `round(curva de taxa)` partículas (o primeiro valor da curva,
  `FUN_01c2a6ac(0, desc+0x10)`), repondo as mortas na hora e já passando
  a física nelas. Então é **uma** partícula por componente. 151 partículas
  do jogo base têm o bit 1: o brilho do PlumbBob, os postes do cursor do
  modo construir (vida 0,1: com a taxa por segundo apareciam 10% do tempo),
  as setas do terreno, o OVNI e as estrelas da abdução (100 e 300 vivas).
  Feito em `EffectSystem::maintainParticles` (antes `stepEmitter`),
  depois da física, como o jogo. O bit 0 (`InjectParticles`, 524 partículas: balões, ícones de
  aspiração) dá rajadas de `ceil(curva[0] × escala)` por ciclo (§4e.3).
- **Material das partículas** (`cTSParticlesRenderable::SetupTextureParticle`
  `0xeb3254`): `StandardParticlesMaterial` com `stdParticleTexture`; tipo de
  desenho 1 `stdPMInvertDepth`, 2 `stdPMDoDepthTest false`, 3
  `stdPMDoDepthWrite`, 4 `stdPMAdditive`, 5 aditivo + invertido, 6 aditivo
  sem teste de profundidade, 7 `stdPMModulate`; `stdPMLayer` = camada × 8 +
  7 (com o bit 31 das flags, a camada crua). O `particles.matShad`
  (`Materials.package`) põe a partícula na camada 7 por padrão, sem escrever
  profundidade e com teste: **a mesma camada dos materiais com blend**
  (`plumbbob_corecolor*` é StandardMaterial com `blend alpha`, alfa 1,
  camada 7). O `AddElementsToDisplayList` (`0xeba5cc`) passa à display list
  a flag `bit 23 das flags | 0x40` (o brilho tem o bit 23). A ordem dentro
  da camada 7 entre o diamante e as partículas foi lida na rodada n15
  (§4e.6): o diamante conta pelo canto mais perto da caixa, o brilho pelo
  centro, então o ponto branco pequeno fica **atrás** do diamante (antes o
  app desenhava as partículas depois de tudo, por cima).
- Capturas: `docs/screenshots/fix-plumbob-brilho.jpg` (antes, depois,
  depois à noite) e `fix-plumbob-giro.jpg` (o giro entre 4,4 e 5,0 s, depois
  da primeira volta). O giro e o dono estão em `primitives-mac.md`, seção
  0x69/0x6A/0x6B (Animate Object com temp 1 = −1; Generic Sims Call 25).

## 4e. Rodada n15: o sistema de partículas lido no binário **[bin]**

Ghidra MCP (`sims2_bv.ppc`, só leitura) e Ghidra headless num clone
descartável para as funções que o projeto não tinha criado (o
`ApplyEffect` das partículas, os leitores das descrições, os comandos do
script). Nomes pelo mapa do `tools/symxfer` regerado; vários nomes "fill",
"gap" e "moved" do mapa estão trocados nesta região (por exemplo
`cTSScreenEffect::Start` no endereço de `cBoundingBox::Translate`), então
cada função citada abaixo foi conferida pelo código. Endereços do BV.

### 4e.1 A descrição das partículas (`cTSParticlesDescription`)

`cTSCompDescriptionBaseT<cTSParticlesDescription,1>::Read` (vtable
`0x2acce10`, entrada `0x1c1e698`) lê a versão u16 (1–11) e chama o
`ReadDescription` das partículas (`0xead010`; o "ReadDescription" que o
mapa põe em `0xf67848` é outra coisa). Ordem do arquivo → deslocamento na
descrição → nome no script do Swarm (dos comandos `cParticle*Command`,
vtables no `ppc_syms.txt`, entrada 4 de cada uma):

| arquivo (nosso nome antigo) | desc | comando / opção |
|---|---|---|
| flags u32 (v ≤ 4; depois `bitset<38>`) | +0x158 | ver 4e.2 |
| life (V2) | +0x4 | `life` |
| preRoll | +0xc | `-preroll`? (usado no 1º passo do `ApplyEffect`) |
| rateDelay (V2) | +0x28 | `rate -delay`: espera antes de começar |
| rateTrigger (V2) | +0x30 | `rate -trigger`: espera entre os ciclos (e no começo, se o delay < 0) |
| emitDirection, emitSpeed, emitVolume, torus | +0x38, +0x50, +0x58, +0x70 | `emit`, `source` |
| rateCurve, rateCurveTime, rateCurveCycles (u16) | +0x10, +0x20, +0x24 | `rate` (`-loop n`, `-single`) |
| rateSpeedScale | +0x160 | `rate -speedScale`: taxa × (1 + escala × velocidade do emissor) |
| size, sizeVary, aspect, aspectVary | +0x74, +0x84, +0x88, +0x98 | `size`, `aspect` |
| rotateAxis (V3), V3, u8 | +0x164, +0x170, +0x17c | `rotate -axis`; o u8 é quantos `rotate` há (1 ou 2) |
| 2 floats que pulávamos (v < 6) | +0x180..+0x18c | por rotação: x > 0 → multiplicador aleatório em [−x, x] por partícula; senão 1 |
| rotateOffsetX, rotateOffsetY | +0x190, +0x194 | deslocamento fixo (voltas) da 1ª e da 2ª rotação |
| rotateCurveX, rotateCurveY | +0x198, +0x1a8 | as curvas das duas rotações |
| alpha, alphaVary, colors, colorVary | +0xb4, +0xc4, +0x9c, +0xa8 | `alpha`, `color` |
| material, tileU, tileV, alignment, drawType, layer, frameSpeed, frameStart, frameCount | +0x1d0, +0x1d4, +0x1d5, +0xec, +0x1d6, +0x1d8, +0x1dc, +0x1e0, +0x1e1 | `texture -tile -draw -layer`, `frames`, `alignment` |
| force (V3) | +0xc8 | `force -gravity g` (z −= g) e `-wind v s` (somados num vetor só) |
| windStrength | +0xd4 | `force -globalWind`: × o vento do mundo |
| **drag** (era o arrasto) | +0xd8 | `force -bomb s`: velocidade radial na emissão |
| **force2** (V3) | +0xdc | `force -bomb s ponto`: o centro da bomba |
| **force2a** | +0xe8 | `force -drag`: **o arrasto** |
| force2b | +0x1e4 | `stretch` (esticar pela velocidade) |
| screw, wiggles, bloom | +0xf0, +0xf4, +0x100 | `force -screw`, `wiggle`, `screenBloom` |
| colisores, nome | +0x130, +0x21c | `collide`, `deathEffect` |
| terreno (6 floats), killHeight, 2 floats, V2 | +0x104..+0x124 | `terrainCollide`, `terrainRepel` |
| randomWalkDelay, randomWalkStrength, turnX, turnY | +0x22c, +0x234, +0x23c, +0x240 | `randomWalk -delay -strength -turn a b` |
| atrator (v < 8): V3, curva, alcance | +0x13c | `attractor` |

A meta (`0xeae76c`, versões 1–4) tem a mesma forma até o grupo de forças;
os dois passeios aleatórios são `cRandomWalk` de 7 floats (atraso V2,
força V2, giro, giro mínimo, mistura vertical) em +0x17c e +0x198.

### 4e.2 Os bits

- **Partículas** (`bitset<38>`; v ≤ 4 lê um u32 = bits 0–31): 0 `inject`, 1
  `maintain`, 2 `rate -sustain` (no fim dos ciclos segura o último em vez
  de parar), 3/4 colisores, 5 `emit -base`, 6 `emit -radial` (a direção é a
  posição), 8 elipsoide, 9 sem translação (nenhum uso no jogo base), 10
  altura, 11/12/13 `rate -sizeScale/-areaScale/-volumeScale`, 14
  `source -scaleParticles`, 15/17/18 terreno, 20 `randomWalk`, 21 material
  próprio, 22 forma (malha), **23 `texture -light`** (iluminada; 1.441 do
  base), 24 `-acceptOverride` (a textura pode ser trocada: os ícones dos
  balões), 25 `-acceptComposite`, 26 `force -attractor`, 28
  `emit -scaleExisting`, 29 `source -resetIncoming`, 30 `rate -cycleKill`,
  31 `texture -directLayer` (a camada crua, sem × 8 + 7), 33/34/35 caminhos.
  **Os bits 0/1 não querem dizer "presas ao efeito"** (era a nossa
  inferência).
- **Meta** (`bitset<34>`; `operator>>` `0x1c229d0` lê o **primeiro u32 como
  os bits 32–33** e o segundo como os bits 0–31: o nosso "bit 32 + k" é o bit
  k do binário): 0 `inject`, 1 **`maintain`** (o nosso "presas ao volume",
  bit 33), 2 `sustain`, 4 `base`, 5–10 colisão/terreno, 11 elipsoide, 12
  sem translação, 22 `scaleParticles`, 23 `randomWalk -sync`, **24
  `randomWalk`** (o nosso "a taxa é quantas ficam vivas", bit 56), 25
  `randomWalk -wait`, 27 `attractor`, 28 opção do atrator, 29 trator, 30
  **caminho** (`path`: a partícula segue os pontos do caminho, que o nosso
  parser chama de "tractor points"), 31 `alignment -windBank`, 32
  `resetIncoming`, 33 `limitHeight`.
- **Efeito visual** (`effect`, parser `0x1a7e510`): 0 `viewRelative`, 1
  `cameraFacing`, 2 `cameraAttached`, 3 `noAutoStop`, 4 `hardStop`, **5
  `rigid`**, 6 `noPropagate`/`ignoreParams`, 7 `applyCursor`, 8
  `ignoreScale`, 9 `ignoreOrientation`, 10 `orientZPole`, 11 `gameTime`, 12
  `realTime`, 13 `detach`, 14 `notify`, 15 `clampScreenSize`, 16
  `noSmoothClamp`. O brilho do PlumbBob é `0x1020` (rigid, realTime); os
  balões `0x18223`.
- **Componente** (u32 de flags de cada componente;
  `CreateComponentFromDescription` `0xc042e4`, `UpdateVisualEffect`
  `0xc04544`): 0 `ignoreLength`, 5 `ignoreParams`; **1 corrente**: o
  componente é o "efeito de corrente" do componente de corrente anterior
  (`SetChainEffect`), e o 2 fecha a corrente; **3 corrente de estados**: só
  o primeiro é criado, os seguintes com o bit 3 são as descrições que o
  estado (parâmetro 0xF, `SelectDescriptionState` `0xeb04f4`) troca
  (`SwitchDescription`), até um com o bit 4. As baratas são `0x2`, os peixes
  `0x8`. Quem é alvo de corrente não emite (`SetChainEffect` liga o +0x1e8
  das partículas / +0x16c da meta, que o `ApplyEffect` testa antes de
  `GenerateNewParticles`).

### 4e.3 Emissão (`ApplyEffect` `0xebb7c8`, `GenerateNewParticles` `0xeb8c90`)

- `Start(t)` (`0xeb726c`): com t ≠ 0 (começo forte) um emissor de ciclos
  finitos não começa, salvo com `sustain`; zera o relógio; espera inicial =
  `RandomRange(delay)` se delay ≥ 0, senão `RandomRange(trigger)` se ≥ 0;
  marca o pre-roll **só no começo forte** (`this[0x3c] = t ≠ 0`). Com
  `randomWalk`, os dois ângulos do passeio começam em [−0,5, 0,5] volta.
- `ApplyEffect(dt)`: no primeiro passo depois de um começo forte, dt +=
  preRoll. Primeiro anda as partículas vivas, depois gera as novas (salvo
  alvo de corrente), depois o passeio aleatório do efeito.
- `GenerateNewParticles(dt)`: desconta a espera (+0x88); com preRoll > 0, o
  dt da geração fica ≤ preRoll − 0,01; bit 1 → `MaintainParticles`, bit 0
  → `InjectParticles`, senão `AddRateCurveParticles`.
- `AddRateCurveParticles` (`0xeb8728`): taxa × escala de emissão (LOD,
  componente; × s, s² ou s³ com os bits 11–13); com speedScale, × (1 +
  speedScale × |deslocamento do emissor| / dt). Em fatias de no máximo 1/15
  s: o relógio em ciclos (+0x40) avança dt / rateCurveTime; ao cruzar um
  ciclo: `cycleKill` limpa; no último ciclo para (Stop suave) ou, com
  `sustain`, segura o fim; com trigger ≥ 0 a fatia acaba no fim do ciclo e
  depois se espera `RandomRange(trigger)`. A quantidade é
  `escala × fatia × curva(fração do ciclo) + resto`, o inteiro de baixo; o
  resto fica para a próxima. Cada nova nasce espalhada pela fatia: a
  posição recua pelo deslocamento do emissor na proporção do tempo que
  falta, e já anda (`UpdateParticlePhysics`) esse tempo.
- `InjectParticles` (`0xeb83f4`): em cada início de ciclo, `ceil(curva[0] ×
  escala)` partículas de uma vez, todas com a mesma idade (o tempo desde o
  início do ciclo), se ela for menor que a vida máxima; para depois de
  `cycles` ciclos.
- `MaintainParticles` (`0xeb822c`): `ceil(curva[0])` vivas (sem a escala de
  emissão), as que faltam criadas na hora e já andadas dt.
- Densidade: corrigido na rodada n38 (§4f.2): são os emissores de curva de
  taxa (e o `inject` acima de 20) que levam a densidade e a escala do
  gerente, as duas 1 no `Graphics Rules.sgr`; nada a fazer.

### 4e.4 Física

- `UpdateParticlePhysics` (`0xeb6ecc`): idade += dt; morta se idade ≥ vida.
  Sem nenhum dos bits 3, 4, 15, 17, 26 (máscara da `__StaticInit`
  `0x26447f4`) e com dt × arrasto ≤ 0,05, **um passo só** (`0x1c2aca0`):
  v' = (v + dt·a)(1 − dt·arrasto), posição += (v + v')·dt/2. Senão
  `UpdateParticlePhysicsTimeStep` (`0xeb6d50`): passos de **no máximo 1/15 s**
  (`0x3d888889`; usávamos 0,1): posição += passo·v; v += passo·a; depois
  `ApplyForces` (`0xeb6604`): v += passo × atrator; v ×= 1 − passo ×
  arrasto (o +0xe8: o nosso `force2a` era mesmo o arrasto); colisores,
  terreno, morte fora do lote com o bit 18.
- A aceleração do efeito (+0x58, `ApplyEffect`) = `force` + `globalWind` ×
  vento do mundo + o vetor do passeio aleatório. A força vale **no
  referencial das partículas** (o mundo, ou o do efeito com `rigid`), sem
  girar pelo componente: confirma a nossa escolha antiga.
- **Passeio aleatório das partículas** (`UpdateRandomWalk` `0xeb70dc`, bit
  20): **um por efeito, não por partícula**: a cada `RandomRange(delay)` os
  dois ângulos (rumo, elevação, em voltas) mudam de `aleatório(−turnX,
  turnX)` mais ±turnY no mesmo sentido, e o vetor = força × (cos e cos r,
  cos e sen r, sen e) entra na aceleração de todas.
- `InitParticle` (`0xeb746c`): vida = `RandomRange(life)`; posição na caixa
  (ou elipsoide); direção na caixa de direção (ou a própria posição com
  `radial`) com o tamanho = escala de velocidade × `RandomRange(speed)`;
  **bomba**: v += s × (p − centro)/|p − centro|; depois a posição passa pela
  transformação do emissor e a velocidade pela rotação dela. Variações por
  partícula: tamanho × (1 ± vary) × escala de tamanho, aspecto, alfa, cor (3
  canais), multiplicadores das rotações (4e.1).

### 4e.5 Onde as partículas vivem

`cTSVisualEffect::UpdateTransforms` (`0xeb0b8c`) põe a transformação do nó
(osso ou slot, ligado por `SetUnknownParam(2, nó)`) na **transformação do
efeito**; `cTSVisualEffect::SetTransforms` (`0xeb1760`) dá a cada componente
t1 = componente ∘ efeito e t2 = fonte (normalmente a identidade). As
partículas nascem em t1 (no mundo) e são desenhadas por t2: **não seguem o
efeito**, ficam onde nasceram (a fumaça do fogão, o fogo). Com o bit 5 do
efeito visual (**`rigid`**), `SetTransforms` passa a transformação do efeito
para a fonte e deixa a do efeito em identidade: as partículas vivem no
referencial do efeito e andam com ele (o brilho do PlumbBob, os balões,
489 efeitos do base). Um efeito aninhado herda: basta um nível `rigid`. As
filhas das meta-partículas são efeitos com a transformação da
meta-partícula e a fonte da meta (`UpdateActiveParticles` `0xf5f5b0`).

### 4e.6 Desenho e ordem dentro da camada 7

- O vértice (`0xeb8dbc`, o caminho sem vertex program): t = idade / vida;
  tamanho = vary × curva(t) × escala; altura × aspecto(t) × vary; rotação =
  (curva de rotação(t) × multiplicador + deslocamento) voltas; cor =
  cores(t) × vary × cor do efeito; alfa = vary × alfa(t) × alfa do efeito;
  quadro do atlas = `int(frameSpeed × idade + frameStart)`, dentro de
  `frameCount` se ≠ 0, célula pelos bits (u = quadro & (tileU − 1)). Uma
  chamada de desenho por componente, as partículas na ordem da lista (sem
  ordenar entre si).
- A display list (`cDisplayList::AddGeometry` `0xca8598`) guarda por parte
  a chave (camada × 8 + 7 para as com blend, ímpar) e uma profundidade; o
  comparador `cLessGeomPartInfo` (`__median` `0x1af5960`) ordena por chave
  e, **com chave ímpar, só pela profundidade, a maior primeiro**. As
  partículas vão com a flag 0x40 (`AddElementsToDisplayList` `0xeba5cc`):
  profundidade = z/w do **centro** da caixa das partículas (`0x86ed4`); as
  partes dos objetos = o **menor** z/w dos 8 cantos da caixa (`0x86d4c`),
  o canto mais perto. Partes de uma mesma geometria ganham 1e−5 × índice.
- Daí o PlumbBob: o diamante (`plumbbob_corecolor`, blend alpha, camada 7)
  conta pelo canto mais perto, o brilho pelo centro; os dois em volta do
  mesmo ponto, o diamante fica mais perto e é desenhado **depois**: o
  ponto branco pequeno fica **atrás** do diamante.

### 4e.7 Meta-partículas

- Física (`0xf62d10`, `0xf63184`): os mesmos passos de 1/15 e o caminho
  rápido; com o bit 24 cada meta-partícula tem dois `cTSRWState`
  (`Update` `0xf60d9c`): a cada `RandomRange(delay)`, força s =
  `RandomRange(strength)`; mistura m = giro[2]: m ≥ 0,5 → vertical s,
  horizontal 2s(1 − m), senão horizontal s, vertical 2sm; rumo e elevação
  mudam de `aleatório(−giro[0], giro[0])` ± giro[1] voltas; o vetor
  (`0xf5e5bc`) é uma **aceleração**. Bit 29: trator; bit 30: caminho
  (`SnapParticleToPath` `0xf5fb18`: posição e velocidade por Hermite entre
  os pontos, em t1 a cada passo, então segue o emissor); bit 9 mata fora do
  lote.
- `InitParticle` (`0xf63310`): igual às partículas (caixa, direção,
  velocidade, bomba, t1), e cria a filha (`CreateVisualEffect` da base) com
  **começo suave** (sem pre-roll) e a transformação só de translação.
- `HandOffParticleToChainSystem` (`0xf63d30`): no fim da vida a partícula
  vai para o efeito de corrente com vida nova, troca a filha se a base muda
  e, com `resetIncoming`, é reiniciada.

### 4e.8 O que mudou no código (rodada n15) e o que segue inferido

- **Nomes** (`formats/effects.*`): os campos com os nomes do script (4e.1:
  `bomb`, `bombCenter`, `drag`, `stretch`, `globalWind`, `rotateCount`,
  `rotateRange`, `rotateOffset`), as flags da meta como o `bitset<34>` do
  binário, os pontos de caminho, os colisores lidos inteiros; os bits têm
  nome (`pflag`, `mflag`, `vflag`, `cflag`).
- **Emissão e física** (`render/particle_system.*`): `startEmitter`
  (começo forte/suave), `applyEmitter`, `generate`,
  `addRateCurveParticles`, `injectParticles`, `maintainParticles`,
  `initParticle`, `updatePhysics` (1/15 s, o passo único),
  `updateRandomWalk` (um por efeito), o atrator com ponto e o gerador
  aleatório do jogo (`FUN_00171b0c`, LCG `0x278dde6d`). As partículas vivem
  no mundo salvo com `rigid`: a fumaça e o fogo ficam onde nasceram.
- **Meta**: cadeias pelas flags dos componentes (0x2, 0x8), `maintain`,
  `inject`, os dois `cTSRWState`, atrator, colisores planos (as seis
  paredes do aquário no lugar da caixa inferida), caminhos (Hermite),
  alinhamentos 2/3/4 com amortecimento, curva de rotação, filhas com
  começo suave, efeitos que encaram a câmera (`EffectSystem::setView`, do
  `gCamera`).
- **Camada 7** (`render/particle_layer.*`, o renderer): cada emissor é um
  grupo com a profundidade do centro da caixa, as partes com blend a do
  canto mais perto, ordenados juntos (`setExternalBlended`); a caixa das
  partes animadas segue os vértices posados.
- **Balões**: sem a subida de 0,2 m (o caminho sobe); a foto do ícone com
  as alternativas da §4b.
- **Ainda inferido** (marcado no código): ~~os itens de uma sequência
  começam suaves~~ (n38: lido, §4f.4); ~~os eixos depois do X nos efeitos
  que encaram a câmera~~ (n38: lidos, §4f.5); atrator sem ponto próprio
  (puxa para o alvo do efeito, que nada liga aqui) não puxa; ~~sem vento
  do mundo no lote~~ (n38: zero, §4f.2); o rumo amortecido da meta a cada
  passo de 1/15 s; ~~nas cadeias de estado só nascem partículas no estado
  0~~ (n38: §4f.5); no `lot_effects`, o SLOT tipo 4 como "alvo", os efeitos
  salvos voltando com começo forte, as alternativas da foto do balão; a
  malha das partículas de forma girada só no Z.
- **Não feito** (rodada n15): ~~colisores das partículas~~ e terreno
  (bits 3/4, 15/17/18), ~~`wiggle`, `screw`, a escolha de LOD pela
  distância da câmera, a iluminação (bit 23 `light`), o trator das metas
  (bit 29), `cTSSequenceEffect`, efeitos de colisão e de morte, a
  densidade de partículas do gerente~~ (rodada n38, §4f).
- **Provas** (antes = main `4e9084e5`, depois = a branch; `--run-vm` em
  passos, mesma câmera): `docs/screenshots/n15-particulas-1.jpg` (PlumbBob
  de perto: o ponto branco atrás do diamante; balão; fogo da lareira; vapor
  do chuveiro) e `n15-particulas-2.jpg` (fumaça do fogão, moscas, abelhas,
  faíscas da lâmpada quebrada, peixes do aquário). As abelhas e as faíscas
  ficam menos numerosas (taxa por ciclo em fatias de 1/15 s, rajadas de
  `ceil(taxa)`), o vapor e a fumaça sobem como uma nuvem só (um passeio
  aleatório por efeito), os peixes nadam dentro das paredes do tanque.
- Ferramentas: `s2pkg effects` conta os bits e lista os pontos de caminho; `s2pkg effect-usage` (rodada n38) diz quem usa cada coisa;
  `s2pkg effect-sim <pacote> <nome>~soft` roda com começo suave (sem
  `~soft`, forte, com o pre-roll).

## 4f. Rodada n38: o resto das partículas lido no binário **[bin]**

Ghidra MCP (`sims2_bv.ppc`, só leitura) e Ghidra headless num projeto
descartável importado sem análise (as funções que o projeto do MCP não
criou: `cDisplayList`, os colisores do mundo, o `cTSSequenceEffect`, o
leitor das propriedades). Endereços do BV. `s2pkg effect-usage <pacote>
colliders|terrain|wiggle|screw|stretch|unlit|tractor|lod|sequences` lista
os blocos que usam cada coisa e os efeitos que os levam (conta abaixo).

### 4f.1 Iluminação (bit 23, `texture -light`)

- O parser do comando `texture` (`FUN_01c1cad4`) liga o bit 23 com a
  opção `-light`: **1.441 das 1.852 partículas são iluminadas**; as 411
  sem ela são as da interface (balões, ícones, cursores).
- `AddElementsToDisplayList` (`0xeba5cc`) passa à display list a flag
  `bit 23 | 0x40`: 1 = a geometria recebe as luzes; sem ela, o
  `kGeomSkipIllumination` que o `particles.matShad` cita ("the particle
  system takes care of whether lighting is applied"). As partículas de
  malha (o renderable `+0xac`) vão sempre com 1.
- O que a luz faz no `StandardParticlesMaterial`: só
  `numLightsOfType(environmentCube)`: com uma luz dessas,
  `colorScalar (1,1,1,1) -applyTextureLightColor 0 0` e um estágio que
  multiplica a cor e o alfa da textura pelo `colorScalar` ("multiply in
  environment tint"); as luzes comuns ficam de fora
  (`LightingStatesNoStdLights`). Com textura composta, nada.
- A luz `environmentCube` do lote é a "LM_Environment" que o
  `TSCreateLightsFromLightingStateWithAmbient` (`0xd741dc`) cria do campo
  `environment` do estado de luz (bit 9; o `particles`, bit 10, faria uma
  "LM_Particles", mas nenhum estado do `Lighting.txt` o tem). No
  `Lighting.txt`: dia `(1,1,1) 1`, noite `(1,1,1) .8` (o comentário da
  Maxis diz "does not work"; o binário a cria igual), manhã/tarde
  `(0.45, 0.25, 0.35) 2` (só com `morningEvening`, desligado). Então à
  noite as partículas iluminadas ficam 20% mais escuras; de dia nada muda.

### 4f.2 LOD pela distância da câmera

- `cTSCameraSmoothSims1Controller::Update` (`0x86ffb8`), no fim: com o
  gerente de efeitos (`+0x434`), acha o nível da zoom table da distância
  atual (`vtable 0x118` = `FUN_0086b56c`: o último nível cuja distância
  passa a atual e a fração até o seguinte, presa em [0, 1]) e chama o
  `SetLOD(float)` do gerente (`vtable 0x44` = `0xc061b0`) com **nível +
  fração + 1**. Com `70 40 25 15 8 3` do `LiveCamera.txt`: 70 → 1, 40 → 2,
  25 → 3, 15 → 4, 8 e mais perto → 4,9999 (o máximo). O mesmo número que o
  `LotCamera::zoom()` do app + 1.
- `SetLOD(float)`: LOD = valor − `particleLODOffset` (`+0xac0`; 0 em
  todas as qualidades do `Graphics Rules.sgr`), preso em [1; 4,9999]; o
  inteiro (`+0xaac`) e a fração (`+0xb30`). Mudou o inteiro:
  `FUN_00c05240` refaz todos os efeitos (`UpdateVisualEffect`, `0xc04544`).
  (`SetLOD(int)`, `0xc06378`, é o caminho sem suavização; ninguém do lote
  o usa.)
- `UpdateVisualEffect`: um componente existe só com `lodBegin ≤ LOD <
  lodEnd` (o parser do `effect`, `FUN_00bff648`, dá 1 e 5 sem `-lod`); os
  que saem são parados (forte, se `effectLODSoftChange` for falso; o valor
  padrão dessa propriedade não foi achado) e os que entram são criados.
- `SetCurveCoefficients` (`0xc021c8`): com `lodBegin < lodEnd − 1`, as
  escalas (emissão, tamanho, alfa) em `(LOD − begin)/(end − begin)` e em
  `(LOD − begin + 1)/(end − begin)` entre as do começo e as do fim do
  componente; com a faixa de um nível, as do começo e as do fim mesmo. O
  efeito recebe os dois trios (`SetFloatParams 0x101`, `0xeb62f0`: `+0x13c`
  … `+0x150`) e o `ApplyEffect` (`0xebb7c8`) mistura pela fração: no fim,
  **escala = lerp(começo, fim, (LOD − begin)/(end − begin))**. 1.935
  componentes do jogo base têm escalas de LOD (o mais comum: emissão 0,25,
  tamanho 2 e alfa 2 no LOD 1: de longe menos partículas, maiores e mais
  opacas), alguns só existem do LOD 2 ou 3 para cima.
- Densidade e escala do gerente: o contexto que o `ApplyEffect` lê
  (`+0xb24` do gerente) é `particleDensity` (1), `particleScale` (1), uma
  constante, a fração do LOD e o vento do mundo. O `ApplyEffect`
  multiplica a emissão pela densidade e o tamanho pela escala **nos
  emissores de curva de taxa** (nem bit 0 nem bit 1); no `inject` só com
  curva[0] > 20 (senão a escala de emissão vira 1); no `maintain` nada.
  Com os dois em 1 no jogo, nada muda. Corrige a §4e.3.
- O vento do mundo (`+0xb34`) não tem quem escreva no jogo base (nenhum
  `stfs`/`stw` em `+0xb34`): zero, como o app já fazia.

### 4f.3 Colisores das partículas e efeitos de colisão/morte

- `ApplyForces` (`0xeb6604`), a cada passo de 1/15 s: atrator, arrasto,
  depois, com os bits 3/4, `ApplyColliderForces` (`0xeb3064`): o raio da
  partícula = tamanho dela × curva de tamanho × max(1, aspecto) × (1 +
  |variação do aspecto|) × as escalas do efeito; então a resposta comum
  `FUN_00ebdb5c` para cada colisor; fora do lote com o bit 18 morre; com os
  bits 15/17, `ApplyTerrainForces`.
- O registro do colisor (`cTSColliderInfo`, leitor `FUN_00ebd270`, cópia
  `FUN_01c12158`): flags, nome, 3 vetores, e os floats `+0x2c` quique,
  `+0x30` deslize, `+0x34` "gruda" (lido só com o bit 1 das flags e um
  byte; nenhum no jogo base: 0), `+0x38` escala do raio, `+0x3c` chance de
  morte, `+0x40` efeito de colisão, `+0x44` efeito de morte. Os quatro
  floats do arquivo são os de `+0x2c`, `+0x30`, `+0x38`, `+0x3c`.
- A resposta (`FUN_00ebdb5c`): o teste do colisor (`vtable 0x14`) acerta →
  chance de morte 1, ou sorteio ≤ chance: o efeito de morte (se houver)
  nasce ali e a partícula morre; senão o efeito de colisão nasce ali e,
  movendo-se contra a normal, v' = v − (1 + quique)·vₙ − (1 − deslize)·vₜ
  (com "gruda" > 0: parada abaixo de √gruda, senão encolhida). O efeito
  novo (`FUN_00ebd9ac`): `CreateEffect(nome)`, só a translação no ponto,
  a fonte do efeito, **começo suave**, e fica por conta do gerente até
  acabar.
- Colisores do jogo base (321 blocos de partículas): `plane` 426
  registros, `floors` 142, `walls` 54, `wallsAndFloors` 53. Efeitos de
  morte: `throwupPuddleAdult` (vômito), `wateringCanSplash`,
  `sinkLeakyFaucetSplash`, `bathtubLeakyFaucetSplash`,
  `sloppyShowerSplash`, `roachProFumes`, `aquarium_fish*_death`. Usam
  `floors`: confetes (aniversário, contas), vômito, comida derramada, o
  despertador quebrado, o extintor, moscas, abelhas, baratas, a fumaça da
  morte; `plane`: o vapor do chuveiro/banheira (o teto da caixa), o vapor
  do freezer, as bolhas do aquário, as gotas da torneira pingando.
- `plane` (`cTSPlaneCollider`, teste `0xebce60`): plano e 4 cantos
  montados uma vez, no primeiro `ApplyEffect` (`+0x1d0`), dos 3 vetores no
  quadro do emissor; acerta movendo-se contra a normal (vₙ < −1e-5) com t =
  (raio × `+0x38` + distância)/vₙ em [0, 1] e o ponto dentro do quadrilátero
  (paridade no plano); o ponto novo é o do contato.
- `floors` (`TSSGEffectInteraction.cpp`, `cFloorsCollider`, teste
  `0x1bc2a58` via a base `0x1bc3248`, que leva os pontos ao mundo e usa
  raio × `+0x38` × 0,5): o andar abaixo do ponto
  (`nTSWorld::LevelBelow`, `0x1670cc`: do andar de cima para baixo, o
  primeiro cujo piso em (x, y) está abaixo de z + 0,01) e o de cima; o piso
  é o `SampledFloorElevation` do andar e vale se o tile tem piso
  (`WorldDB` `0xb8` com o quarto do ponto, `FUN_0015d84c`). Cruzou o piso
  de baixo descendo (ou está sob ele): fica a raio acima, normal +Z;
  cruzou o de cima subindo: a raio abaixo, normal −Z.
- `walls` (`0x1bc2778`): a consulta de paredes do `WorldDB` (`0xa0(3)`) com
  um teste de esfera (`0x54`) e de segmento (`0x4c`) que dão o ponto e a
  normal; o ponto fica a raio + `+0xc` da parede. As duas consultas do
  `WorldDB` não foram lidas. `wallsAndFloors` = os dois.
- Meta-partículas: a mesma resposta (`ApplyColliderForces` da meta,
  `0xf5e804`); ao morrer sem corrente (`FUN_00f64084`) a filha é parada
  (suave por padrão, `+0x10c`) e **continua até as partículas dela
  acabarem** (lista `+0x1c0`), e se existe o efeito `<base>_death` (o
  `InitFromDescription` da meta, `0xf60654`, procura `%s_death`,
  `%s_terrain`, `%s_water`) ele nasce no ponto com começo suave:
  `flashCamPartEffect_death` (o flash das câmeras), `roachSprayPro2_death`,
  `boltEfx_death`.

### 4f.4 `cTSSequenceEffect` (vtable `0x286d538`)

- Flags do `sequence` (o `ReadDescription` do comando, `FUN_01c0f0a0`): bit 0 `-loop`, bit 1
  `-hardStart`, bit 2 `-hardStop`/`-noOverlap`, bit 3 `-noStop`/`-overlap`.
  No jogo base: 59 sem flags, 20 com `loop` (as pias, a banheira, as
  abelhas/borboletas, o pinball), 5 com `hardStop`.
- `Start` (`0xf5a330`): sem itens não corre; num começo suave (ou o
  primeiro) zera o relógio, o item e o próximo tempo.
- `ApplyEffect` (`0xf5ac84`): relógio += dt; o item atual acaba quando o
  relógio passa o próximo tempo, ou, num item de tempo < 0, **quando o
  efeito filho morre sozinho**. Então: o item seguinte (passado o último:
  com `loop` volta ao primeiro, sem ele a sequência para); o filho
  anterior é parado (forte com `hardStop`; suave fica até acabar) — com
  `overlap` não é parado; o tempo do novo = `RandomRange(tempo)` somado ao
  próximo tempo; nome vazio = uma pausa. O filho: `CreateEffect`, as
  transformações da sequência, `Start(hardStart)`.
- Os itens começam **suaves** (sem `hardStart`, todos no jogo base):
  confirma a escolha antiga.

### 4f.5 O resto

- **Cadeias de estado** (`SelectDescriptionState` `0xeb04f4` →
  `cTSMetaParticlesEffect::SwitchDescription` `0xf60b58`): o estado troca a
  descrição **do efeito inteiro** (`+0x1c` e `InitFromDescription`): as
  novas nascem e todas andam pela descrição do estado; a emissão segue em
  qualquer estado. Um estado > 0 marca o efeito para voltar ao 0 num
  recomeço (`cTSVisualEffect::Start` `0xeb102c`).
- **Trator** (bit 29 da meta sem o 30): `ApplyTractorForce` (`0xf5fef0`)
  no lugar de "posição += v·dt": o ponto do caminho seguinte (o de tempo >
  idade; tempo 0 = o fim da vida) no quadro do emissor; com o tempo que
  falta T = (tempo do ponto − idade) + dt, posição e velocidade andam dt
  por uma Hermite da partícula até o ponto em T (se ela está mais longe que
  T × `+0x1cc`, o último float da descrição, T cresce pelo trecho já
  andado); passado o último ponto, só anda. Com o bit 30 vale o caminho
  (`SnapParticleToPath`) e o trator não. No jogo base: o pinball
  (`fxTableLightHolder`), o chocolate e as câmeras dos bairros; os balões
  têm os dois bits (vale o caminho).
- **Parafuso e tremido** (`0xeb8dbc`, o preenchimento dos vértices; não
  mexem na física): `screw` gira a posição desenhada em volta do eixo
  vertical do emissor por (z − z do emissor) × `screw` radianos (91
  partículas: moscas, as trocas de idade, os corações); `wiggle`
  (`WigglePoint`) desloca a posição pelo relógio do efeito (8 partículas:
  o coração do primeiro beijo, os do bairro).
- **Esticar** (`stretch`, `+0x1e4`, 540 partículas): no mesmo lugar, o
  quadrado gira para a velocidade projetada nos eixos da câmera e, com
  |v| > `stretch`, a altura cresce × |v|/`stretch`.
- **Encarar a câmera** (`cTSVisualEffect::SetTransforms` `0xeb1760`): o
  viewer olha para o +Y local, o +Z é o alto (`camera-ui-mac.md`); bit 1:
  X = do efeito para a câmera, Y = alto da câmera × X, Z = X × Y (o que o
  app já fazia: confirmado); só o bit 0: um giro em Z pelo rumo do **+Y da
  câmera** (a direção da vista, para longe dela; o app virava o X para a
  câmera, ao contrário).
- **Atrator sem ponto próprio**: o ponto vem dos `SetFloatParams` 7/8/9 do
  efeito (`0xeb62f0`, alvo do efeito); o `TryChangeEffect` não os passa
  (só 0xD e 0xF), quem os passa não foi achado: segue sem puxar.

### 4f.6 O que mudou no código (rodada n38) e o que segue inferido

- **LOD** (`EffectSystem::setLod`, `lodScales`, `updateLod`): o app passa o
  `zoom()` da câmera + 1 a cada quadro; cada parte guarda o caminho de
  componentes até ela e toma as escalas do LOD; fora da faixa não emite (e
  volta quando a câmera volta); com `-scaleExisting` (bit 28, 166 blocos)
  a escala de tamanho vale para as vivas. O padrão dos componentes é 1–5
  (o do parser). Inferidos: as escalas de efeitos aninhados multiplicam; a
  saída e a entrada na faixa suaves (o padrão do `effectLODSoftChange` não
  foi achado).
- **Iluminação**: `ParticleQuad::lit` (bit 23) e o `LotEffects` multiplica a
  cor pela luz `environment` do estado de luz do renderer. Partículas de
  malha (sempre iluminadas pelas luzes comuns) não feitas.
- **Colisores** (`EffectSystem::collide`, `hitPlane`, `hitFloors`,
  `ColliderWorld`): partículas e metas com a mesma resposta; `plane`
  montado uma vez; `floors`/`walls` pelo `LotColliders` do app (níveis,
  piso por quarto de tile, o chão vale em todo o lote). Inferidos: o quarto
  do ponto (`FUN_0015d2f0`), o terreno como piso, as paredes (um andar de
  altura, sem cercas, ponto e normal nossos: as consultas `0x4c`/`0x54` do
  `WorldDB` não lidas), o raio da meta (o `ApplyColliderForces` da meta não
  lido). Os efeitos de colisão/morte nascem suaves no ponto (no máximo 256
  instâncias, guarda nossa).
- **Meta**: a filha de quem morre para suave e termina (`dying`), o
  `<base>_death`; cadeias de estado pela descrição do estado (emissão e
  física), a filha e a vida ficam; o trator; `tractorReset` lido.
- **Sequências** (`SequencePart`, `stepSequence`): cada item uma instância
  filha; `-loop`, `-hardStart`, `-hardStop`, `-overlap`, itens < 0
  esperando o filho. O relógio do app avança no máximo 1 s por
  atualização (nas capturas com `--frames` espaçados, a sequência anda 1 s
  por quadro).
- **Desenho**: `screw` e `wiggle` na posição desenhada (`collectInstance`),
  `stretch` no `ParticleLayer` (alinhamentos 0 e 5).
- **Encarar a câmera**: só o bit 0 vira pelo +Y da câmera (para longe dela).
- **Segue não feito**: terreno das partículas (bits 15–18: só as pombas do
  casamento e o riacho do bairro) e das metas (bits 6/8, `%s_terrain`,
  `%s_water`), matar fora do lote (bit 18 / meta bit 9), o "gruda"
  (`+0x34`, 0 no jogo base), o `screenBloom` (bytes `+0x100`), a cor da
  meta passada à filha (`SetFloatParams` 3/4: só o alfa aqui), o meta
  `0x101` (só dois floats), a meta com bit 15 (altura do trator pelo
  terreno), o atrator do alvo (`SetFloatParams` 7/8/9).
- **Provas** (antes = o build do main `13168cc5`, efeitos iguais aos da
  base; depois = a branch; `N001_Lot11`, um efeito por vez em (29, 10) com
  `--effect`, câmera a 8, 40 e 70 m): `docs/screenshots/n38-particulas-lod.jpg`
  (fogo da lareira e fumaça do fogão: de longe menos partículas, maiores e
  mais opacas; as brasas e faíscas só de perto) e `n38-particulas-2.jpg`
  (fogo à noite × 0,8; o jato do chuveiro esticado pela velocidade; o
  confete parando no chão; a pia lavando louça: esfrega, pausa e de novo
  pelo `-loop`). Testes: `test_effects` (LOD, luz, colisores, morte,
  sequência, estado, trator, parafuso, encarar).

## 5. Uso

- `s2pkg effects <effects.package>`: contagens e todos os nomes;
  `s2pkg effects <pacote> <nome>...`: árvore do efeito com os parâmetros.
- `s2pkg effect-sim <pacote> <nome> [s]`: roda o efeito na origem e lista
  partículas.
- `s2pkg effect-usage <pacote> colliders|terrain|wiggle|screw|stretch|unlit|tractor|lod|sequences`:
  os blocos que usam cada coisa do runtime e os efeitos (nomes) que os levam.
- `s2pkg effect-tables <Res> <pacote> [grupo[:STR#]]`: STR# que nomeiam
  efeitos (0x8F), ou a lista de um grupo.
- App: `--effect <nome>[~<estado>][#<guid>][@<objeto>[:<índice no STR# 0x90>] | @x,y,z]`
  (repetível; `#<guid>` dá a foto desse GUID ao ícone de um balão); `OS2_DEBUG_FX=1` imprime os efeitos que a VM liga.
- Captura (rodada 10): `docs/screenshots/r10-fx-aquario-peixes.jpg`
  (`--lot N001_Lot11 --effect aquarium_fish_effects@6.5,17.5,1.0 --pivot
  6.5,17.5 --zoom 9 --orientation 4 --time 6 --frames 3`; antes: a poeira do
  efeito 0 no lugar dos peixes).
