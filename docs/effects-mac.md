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
  PlumbBob); bit 20: passeio aleatório ligado (todos os efeitos com passeio
  real; o valor padrão sem ele é 5/50); bit 24: a textura é o ícone do balão.
- Pre-roll só nos emissores contínuos (ciclos 0): os balões têm vida 4 e
  pre-roll 4 e sumiriam.
- Meta-partículas: uma por componente (o caso dos balões: taxa 1, um ciclo),
  e as curvas de tamanho/alpha dela escalam o efeito carregado pela vida
  (balão que "estoura" de 0 a 1,2 e some no fim); morta, o efeito acaba.
- LOD: escalas do fim (detalhe máximo). Alinhamentos: 0 câmera, 1 chão
  (ondas, manchas), 2 velocidade (chuva), 6 eixo Z (jatos de água, feixes);
  5 (anéis, peças do despertador) desenhado como 0.
- Decalques, modelos, sons, câmeras, telas e água não são desenhados.

## 4. Balões

São efeitos `uiHeadlineGroup_*` no slot `hat_grip` (tipo 2, slot 4) dos
Sims: meta-partícula com a bolha (`uieffects-headlines-conversationballoon`
ou `-thoughtballoon`) e a partícula do ícone (bit 24, atlas 16×1 ou miniatura).
O app sobe o balão 0,2 m sobre o `hat_grip` **[inferido]** e ordena as
partes de um balão pela posição do efeito, na ordem dos componentes.
Captura: `docs/screenshots/r8-effects-balloon.jpg` (`--lot N001_Lot11
--run-vm 145 --pivot 6.5,22.5 --zoom 5`).

## 5. Uso

- `s2pkg effects <effects.package>`: contagens e todos os nomes;
  `s2pkg effects <pacote> <nome>...`: árvore do efeito com os parâmetros.
- `s2pkg effect-sim <pacote> <nome> [s]`: roda o efeito na origem e lista
  partículas.
- `s2pkg effect-tables <Res> <pacote> [grupo[:STR#]]`: STR# que nomeiam
  efeitos (0x8F), ou a lista de um grupo.
- App: `--effect <nome>[@<objeto>[:<índice no STR# 0x90>] | @x,y,z]`
  (repetível); `OS2_DEBUG_FX=1` imprime os efeitos que a VM liga.
