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

- `UpdateParticlePhysicsTimeStep` @ 0x00eb6d50: passos de no máximo uma
  constante (usamos 0,1 s **[inferido]**): posição += v·dt; v += dt × força do
  efeito; depois `ApplyForces`: colliders e `v *= 1 − dt × arrasto` (campo
  +0xE8 da descrição), terreno/água e morte fora dos limites.
- `InitParticle` @ 0x00eb746c: posição aleatória no volume (`RandomVector3`
  na caixa; elipsoide com a flag 8), direção aleatória na caixa de direção
  escalada para uma velocidade do intervalo (`ScaleToMagnitude`), girada
  pela transformação do componente; tamanhos/aspecto/cores com
  `FxVary` = valor × (1 ± variação).

Escolhas nossas **[inferido]**, conferidas com `s2pkg effect-sim` e capturas:

- Arrasto = primeiro float do segundo grupo (fumaça do fogão 1,8: sobe a
  ~0,2 m/s; balões 0–3); a força é o primeiro vetor (fumaça +Z, Zzz −0,015).
- A força fica no referencial do efeito, a rotação do componente só gira a
  emissão (os componentes da fumaça do fogão têm rotação de 90° em X: sai pela
  frente e sobe).
- Bits 0/1 das flags: partículas presas ao efeito (balões, brilho do
  PlumbBob; inferido). **[bin]** (fix-plumbob) o que o jogo faz com eles é a
  emissão: `GenerateNewParticles` (`0xeb8c90`) testa o bitset de flags
  (`std::bitset<38>` em +0x158 da descrição): bit 1 → `MaintainParticles`
  (`0xeb822c`), bit 0 → `InjectParticles` (`0xeb83f4`), nenhum →
  `AddRateCurveParticles`. Ver §4d; bit 20: passeio aleatório ligado (todos os efeitos com passeio
  real; o valor padrão sem ele é 5/50); bit 24: a textura é o ícone do balão.
- Pre-roll só nos emissores contínuos (ciclos 0): os balões têm vida 4 e
  pre-roll 4 e sumiriam.
- Meta-partículas (rodada 10): partículas de verdade, cada uma com o seu
  efeito carregado (uma instância própria) que se move com ela; as curvas de
  tamanho/alpha escalam o efeito pela vida (o balão que "estoura" de 0 a 1,2
  e some no fim); morta, o efeito acaba. Lidos agora do bloco (layout do
  OpenTS2): alinhamento, segundo grupo (vetor, f) com o f como arrasto,
  wiggles, os dois passeios aleatórios (atraso, força, 3 floats de giro),
  direção preferida, amortecimento/inclinação, atrator (ponto, curva de
  força, alcance) e pontos de tração. Inferido pelos dados:
  - bit 0x01 do byte alto das flags de 64 bits: a taxa é **quantas ficam
    vivas** (um peixe por cadeia, uma barata; vida 60000 e taxa 1/s dariam
    milhares);
  - 0x18 no byte alto: o componente **continua** a meta-partícula anterior
    do mesmo efeito: uma cadeia de passos. Com a vida de um passo no fim, a
    partícula passa ao seguinte mantendo posição e velocidade (as baratas
    `cockroachfloorbehaviour*`: parada → circulando → andando → correndo);
    o estado do efeito (modos 10–13) põe todas no passo do estado (cada
    peixe do aquário: nadando, comendo, morto; o morto vive 2 s). Só nascem
    no primeiro passo, no estado 0; passos de vida sem fim (> 1000 s) voltam
    ao 0 quando o estado volta a 0;
  - bit 0x02 da segunda u32 (bit 33): presas ao volume de emissão do
    primeiro passo (o tanque, ±0,4 × ±0,2 × ±0,15), voltando nas bordas; as
    baratas (bit 32) não;
  - passeio 1: a cada atraso (negativo = o mínimo, 0,05 s) uma nova
    direção, girada até `giro[0]` volta da anterior, subindo até `giro[2]`
    (inclinação), na velocidade do intervalo de força; o `giro[1]` não é
    usado; atrator: aceleração para o ponto (no referencial do componente)
    pela curva de força, dentro do alcance; alinhamentos 2 (o +X do modelo
    na velocidade, com inclinação: os peixes são compridos em X) e 3 (só o
    rumo, nivelado), virando no máximo `amortecimento` voltas/s (padrão 1).
  - Não feitos: wiggles, pontos de tração, o segundo passeio, inclinação
    pelo vento.
- LOD: escalas do fim (detalhe máximo). Alinhamentos: 0 câmera, 1 chão
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
O app sobe o balão 0,2 m sobre o `hat_grip` **[inferido]** e ordena as
partes de um balão pela posição do efeito, na ordem dos componentes.
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
  Feito em `EffectSystem::stepEmitter` (`kMaintain`), depois da física,
  como o jogo. O bit 0 (`InjectParticles`: rajadas de `taxa × escala` por
  ciclo, 524 partículas: balões, ícones de aspiração) segue como antes.
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
  a flag `bit 23 das flags | 0x40` (o brilho tem o bit 23). **Não lido**: a
  ordem dentro da camada 7 entre o diamante e as partículas; o app desenha
  as partículas depois de tudo, então o ponto branco pequeno fica por cima
  do diamante (inferido; `TESTAR.md`).
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
- Densidade: com os bits 0/1, `ApplyEffect` multiplica a taxa e o tamanho
  pela densidade de partículas do gerente (se a curva[0] > 20 no inject);
  não fizemos (a densidade padrão é 1, **[inferido]**).

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

## 5. Uso

- `s2pkg effects <effects.package>`: contagens e todos os nomes;
  `s2pkg effects <pacote> <nome>...`: árvore do efeito com os parâmetros.
- `s2pkg effect-sim <pacote> <nome> [s]`: roda o efeito na origem e lista
  partículas.
- `s2pkg effect-tables <Res> <pacote> [grupo[:STR#]]`: STR# que nomeiam
  efeitos (0x8F), ou a lista de um grupo.
- App: `--effect <nome>[~<estado>][@<objeto>[:<índice no STR# 0x90>] | @x,y,z]`
  (repetível); `OS2_DEBUG_FX=1` imprime os efeitos que a VM liga.
- Captura (rodada 10): `docs/screenshots/r10-fx-aquario-peixes.jpg`
  (`--lot N001_Lot11 --effect aquarium_fish_effects@6.5,17.5,1.0 --pivot
  6.5,17.5 --zoom 9 --orientation 4 --time 6 --frames 3`; antes: a poeira do
  efeito 0 no lugar dos peixes).
