# Telhados, cercas, água e sombras de objetos no binário de Mac

Rodada n06 (2026-10-15), linha 6 do `docs/inventario-binario.md` §5.
Binário: Bon Voyage 1.10 (`sims2_bv.ppc`), endereços BV. Modo: **Ghidra MCP**
(só leitura) para memória e nomes, e **Ghidra headless** num clone APFS do
projeto (`-readOnly`, nada salvo) para as funções que o projeto não tem
definidas (as do `nTSSG` de telhado em `0xc6…`/`0xc7…`/`0xec…` não são
funções no projeto: o script cria a função só na memória e decompila). Os
nomes do symxfer nesta área erram muito ("moved"/"near"): o
`cConicalRoofSnow::BuildConicalRoof @ 0xc689cc` é o `SetVertexProperties`,
o `cHippedRoofType::ComputeGridElevation @ 0x180a4b0` é um destrutor; os
nomes abaixo vêm das vtables (RTTI) e do `baserom.sym`.

## 1. Telhados

### 1.1 As classes

Dois objetos por telhado:

- **Mundo** (`nTSWorld::c…RoofType`, criados por
  `cTSUserToolRoof::RoofFromType @ 0x1d38f0`): dão a altura do telhado
  (`GetElevation`, `GetSampledElevationAt`, `IsOnPeak`, `IsOnDiagonal`) para
  colocação, rotas e o height map do sol.
- **Desenho** (`nTSSG::c…Roof`, com uma cópia do tipo do mundo embutida em
  `+0x14`): monta a malha num `cIIndexedMeshBuilder`
  (`FillMeshBuilderData`).

Tipo do jogo (`tRoofType`) → classe (vtable da classe do mundo), para os tipos
da interface do jogo base (`Lot::RoofType` 1–6 no nosso código):

| tipo | nome no catálogo | classe do mundo | ctor BV | grade (`ComputeGridElevation`) |
|---|---|---|---|---|
| 1 | Shed Gable | `cHalfGableRoofType` (`0x2808cc0`) | `0x9d0870` | `0x9c52a0` → `0x9d3ad8` (rampa de um lado, +0,08) |
| 2 | Shed Hip | `cHalfHippedRoofType` (`0x2808db0`) | `0x9c8a6c` | `0x9d0094` (metade de uma grade de quatro águas com o dobro do tamanho) |
| 3 | Short Gable | `cTallGableRoofType` (`0x2808ea0`) | `0x9c893c` | `0x9c5f20` (como a de duas águas, sem deslocamento) |
| 4 | Long Gable | `cGableRoofType` (`0x2808a00`) | `0x9d1be0` | `0x9c4d98` → `0x9d3c7c` |
| 5 | Hip | `cHippedRoofType` (`0x2808b60`) | `0x9d1228` | `0x9d2bfc` → `0x9c642c` |
| 6 | Mansard | `cMansardRoofType` (`0x2808c50`) | `0x9c8b9c` | `0x9c6728` → `0x9c6568` |

(7 cone, 8 cúpula, 9 octogonal, 10 diagonal…, 0x10/0x11/0x12 pagode de
quatro águas/duas águas/meia água, 0x16 estufa.) A orientação (`+0x2d`) é
decidida no ctor: na de duas águas (4), na de quatro águas (5) e na de meia
água (1), `|Δy| <= |Δx|`; na "curta" (3) e na meia de quatro águas (2),
`|Δx| < |Δy|`. Por isso a 3 e a 4 diferem só no lado da cumeeira.

### 1.2 A grade de alturas

A base (`FUN_019734cc`, chamada pelo ctor comum `FUN_009c8148`) guarda os
dois cantos (`+0x8/+0xc` e `+0x14/+0x18`, mais o andar), o tipo (`+0x20`),
"com beiral" (`+0x24`), a orientação (`+0x25`), a inclinação (`+0x28`,
`SetSlopeFactor @ 0x9c4d68`) e **o beiral: 0,5 tile nos dois eixos**
(`+0x30`, `+0x34`). Os limites da grade (`FUN_015aafb8`/`015aaf40` mínimo,
`015ab0a8`/`015ab030` máximo) crescem 0,5 de cada lado quando há beiral (a
meia água não cresce do lado alto). A grade tem `2·largura + 1` pontos por
eixo (`0x197303c`, `0x1972f3c`): um ponto a cada **meio tile**.

Os valores da grade são "entalhes" (múltiplos da inclinação), contados da
borda:

- duas águas (`0x9d3c7c`): distância à borda mais próxima no eixo que desce,
  `0, 1, 2, …` até o meio;
- quatro águas (`0x9c642c`): `min(dx, dy)` às bordas;
- mansarda (`0x9c6568`): `min(dx, dy, k)` com
  `k = max(1, min(7, min(w/2, h/2) − 1))` (w, h em pontos da grade); o topo
  é plano depois de `k`;
- meia água (`0x9d3ad8`): `i + 0,08` de um lado ao outro (o 0,08 é o único
  deslocamento: a de duas águas chama a mesma rotina com 0).

A altura de um ponto (`AddPositionsToMeshBuilder @ 0xc57a3c`,
`cBaseRoofType::GetElevation @ 0x9c6308`):

    z = base + s·g − 0,42 + f(s),   f(s) = 0 se s ≥ 0,499, senão 0,5 − s

com `base` = a elevação do andar no primeiro canto (`FUN_009cff5c`, `+0x4c`),
`s` a inclinação (0,5 em todos os 275 telhados dos 68 lotes) e `g` o
valor da grade. O telhado do desenho tem beiral, então `g = 0` é a ponta do
beiral: com s = 0,5 a borda do beiral fica 0,42 abaixo do andar e a linha da
parede 0,08 acima. O `GetElevation` do tipo do mundo sem beiral usa +0,42.
O `GetMaxElevation` (`0x9c6b34`) é o mesmo no meio da grade; o da mansarda
(`0x9c67a4`) usa `min(7, min(w/2, h/2) − 1)·0,5`.

### 1.3 A malha (`cGableRoof::FillMeshBuilderData @ 0xc545d0`)

1. Três camadas de posições, todas da mesma grade: o topo (`−0,42`), a
   camada de baixo da faixa de cima (`−0,5075`) e a de baixo de tudo
   (`−0,67`): **a faixa de cima tem 0,0875 e o telhado todo 0,25 de
   espessura**.
2. Normais fixas, não as do triângulo: as quatro águas a 45°
   (±0,7071, 0, ±0,7071) ou (0, ±0,7071, ±0,7071), conforme a orientação,
   e as quatro horizontais (±1, 0, 0), (0, ±1, 0) para as faixas.
3. 100 coordenadas de textura (`AddTexCoordToMeshBuilder @ 0xc543c4`): a
   tabela 10×10 de (u, v) com u, v em `0, 0,1992, 0,1992, 0,3984, 0,3984,
   0,5977, 0,5977, 0,7969, 0,7969, 0,9961`: as bordas das 5×5 células de
   102/512 da textura do topo. Um triângulo escolhe a célula por um índice
   base (`+0x34`, `índice = 10·linha + coluna`).
4. Subconjuntos: `subset0` (topo, `ConnectHeightField(0, 0)`),
   `subsetlower` (baixo, camada 3, normais 2–3), `subsetroofupperstrip`
   (faixa entre as camadas 1 e 2) e `subsetroofstrip` (entre as camadas 2 e
   3), as duas pelo `DrawOuterStrips @ 0xc66504` (quatro bordas da grade,
   quad por meio tile, UV 0/1/9/8 = a primeira linha da tabela; na mansarda
   também o anel em volta do topo plano, linhas/colunas 7 e n−8).
5. Materiais (`cRoofManager::SetRoofPatternProduct @ 0x6bdefc`): `RoofTop`
   (textura do topo + bump), `RoofUnder`, `RoofTrim`, `RoofEdges`, e
   `RoofMaterialStrips` com a textura do topo.

**Triângulos** (`ConnectHeightField`: duas águas `0xc7a188`, quatro águas
`0xc7bef8`, mansarda `0xc7db54`, meia água `0xc7e908`, meia de quatro águas
`0xc804dc`, curta `0xc821e4`): cada célula de meio tile vira dois
triângulos, com a diagonal alternada pela paridade da posição **no lote**
(`(int(2x) + int(2y)) & 1`): par → diagonal (x, y)–(x+1, y+1)
(`cEvenTriangleFirst/Second*`), ímpar → (x+1, y)–(x, y+1)
(`cOddTriangle*`). Cada triângulo:

1. é escondido se o centro fica **abaixo de outro telhado** do mesmo andar
   (`IsTriangleCenterLessThanAnyOtherRoofTriangleCenter @ 0xc63558`: altura
   do outro > a deste + 0,01; ignora cone, cúpula, octogonal, pagodes e
   estufa (máscara `0x470380`); entre duas águas da mesma orientação no
   mesmo andar só esconde fora da faixa da cumeeira do outro);
2. ganha **propriedades de vértice** (`SetVertexProperties @ 0xc689cc`,
   `SetVertexPropertiesEx @ 0xc67ca8`): para cada vértice, quais outros
   telhados passam por ele na mesma altura, se ele está numa cumeeira
   (`IsOnPeak`, vtable `0x84`) ou numa diagonal de quatro águas
   (`IsOnDiagonal`, `0x88`); `FUN_00c63b04` ainda pode esconder o triângulo;
3. escolhe a célula do atlas (`ComputeBaseTextureIndex` e
   `ComputeTextureIndex{Positive,Negative}{X,Y}`, uma classe por tipo de
   telhado × par/ímpar × primeiro/segundo, p.ex. `cOddTriangleFirstGable`
   vtable `0x285ca70`): 60 (`0x3c`) normal, 4 com vértice compartilhado, e
   para triângulos na cumeeira/vale compartilhados 0x14/0x1a/0x16/0x2a/0x2e/6
   conforme o lado; o normal do triângulo (x, y fora de ±0,1) escolhe qual
   das quatro `TexCoordIndicesForNormal…` monta os 3 índices (base, base+1,
   base+10, base+11, com espelho `+0x38`); no `subsetlower` os índices são
   0/9 ou 99/0x5a (a textura de baixo inteira).

### 1.4 O que o nosso código faz

Desde a rodada n13, os tipos 1–6 (os do jogo base, os únicos nos 63 lotes)
seguem o caminho do jogo lido no §1.5, em `src/formats/lot_roof_game.*`:
grade de alturas por meio tile de cada tipo, elevação pelos triângulos da
grade, `IsOnPeak`/`IsOnDiagonal`, a triangulação do `ConnectHeightField`
(dois triângulos por célula, diagonal pela paridade no lote), o triângulo
escondido sob outro telhado e a escolha de quem perde nos vértices
compartilhados, a célula do atlas de cada classe de triângulo, as UVs fixas
do avesso, as normais fixas de cada tipo e as faixas da borda. O
`heightAt` (paredes de sótão, height map do sol) usa a elevação do jogo
sobre o retângulo das paredes, como o `FillFromRooves`. O porte do OpenTS2
em `lot_roof.cpp` ficou só para os tipos diagonais e pagodes (nenhum nos
lotes do jogo base; a interface do jogo base não os oferece).

Ainda inferido ou sem portar:
- a inclinação global do `cWorldRoof` (`+0x40`, decide o "estrito" do teste
  de esconder): tomada como 0,5, a de todos os telhados dos lotes;
- o `IsTriangleCenterSupportedByOtherRoof` (`0xc660d0`), que esconde também
  o anel de faixas em volta do topo da mansarda: não portado (só o teste de
  estar abaixo de outro telhado);
- a ordem dos getters `0xdc`/`0xe0` do produto (qual textura vai para
  `RoofEdges` e `RoofTrim`) pela ordem dos membros;
- os tipos diagonais e pagodes não entram no mundo do jogo: não escondem
  nem são escondidos pelos outros (no jogo entram, com as próprias classes);
- o nível subtraído do andar em `AddRoofIdToArray` (`vtable 0x140` do
  mundo) tomado como 0 (os telhados dos andares L−1..L+2).

Provas (antes = porte do OpenTS2, depois = caminho do jogo; capturas sem
janela, `--lot … --zoom 2 --tilt 25`): `docs/screenshots/n13-telhado-duas-aguas-e-encontros.jpg`
(N001_Lot68, N003_Lot128), `n13-telhado-quatro-aguas-N001_Lot11.jpg`
(N001_Lot82, N001_Lot11), `n13-telhado-mansarda-N002_Lot10.jpg`,
`n13-telhado-meia-agua-N002_Lot31.jpg` (e as curtas do N001_Lot59) e
`n13-telhado-lotes-inteiros.jpg`. Por cima a imagem muda pouco (o OpenTS2
imitava o resultado); as diferenças são as faixas da borda (a de cima com
`textureedges`), o topo da mansarda, as células nos encontros e as normais.
O teste `testGameRoofs` (`tests/test_formats.cpp`) confere a triangulação,
as alturas, as células da cumeeira (4) e do resto (60), as do espigão e os
triângulos escondidos entre duas águas que se cruzam.

Efeitos que vêm do binário e parecem estranhos (mantidos): o triângulo
plano usa o índice 60 três vezes nas células pares e a base nas ímpares (o
topo da mansarda sai xadrez quando a base é 4); a meia água tem sempre a
normal 0 (−y) e a curta usa as normais genéricas (−y/+y) mesmo com as águas
em x; a grade de quatro águas da meia de quatro águas tem pico pelo
`GetMaxElevation` próprio (`min(min(W, H) − 1, max(W/2, H/2))`), que pode
não existir na grade.

### 1.5 O caminho do jogo, lido inteiro (rodada n13)

Rodada n13 (2026-10-15), linha 13 do `docs/inventario-binario.md` §6. Modo:
**Ghidra MCP** (só leitura) para o que o projeto tem como função, e
**Ghidra headless** num clone APFS do projeto (`-readOnly`, nada salvo) para
as funções sem definição (`nTSSG` em `0xc6…`/`0xc7…`/`0xec…`, as do tipo do
mundo em `0x9c…`/`0x9d…`): o script cria a função só na memória e decompila,
e um segundo script lista o assembly (o decompilador perde atribuições nas
funções de textura; tudo abaixo que decide célula ou visibilidade foi lido no
assembly). Os começos das funções vêm do `__eh_frame` (`dwarfdump`); as
funções-folha sem FDE (`0xec6ce4..0xec7558`) foram dadas à mão.

**Objeto do telhado (base, `FUN_019734cc`, no `+8` do tipo do mundo):**
`+0x08/+0x0c` canto 0 (x, y), `+0x10` andar, `+0x14/+0x18` canto 1,
`+0x20` tipo, `+0x24` com beiral, `+0x25` orientação, `+0x26` = x1 ≥ x0,
`+0x27` = y1 ≥ y0, `+0x28` inclinação, `+0x30/+0x34` beiral (0,5).
Orientação (ctor de cada tipo): duas águas (4), quatro águas (5) e meia água
(1): `|Δy| ≤ |Δx|`; curta (3) e meia de quatro águas (2): `|Δx| < |Δy|`;
mansarda (6): 0. Orientação 0 = as águas descem em x (cumeeira ao longo de y).

**Limites** (`FUN_015aafb8` x mín., `015aaf40` y mín., `015ab0a8` x máx.,
`015ab030` y máx.): o canto ± o beiral, menos do lado alto da meia água
(tipo 1/0x12 com orientação 0, ou tipo 2 com orientação ≠ 0, no eixo x; os
inversos no eixo y): sem beiral no lado mín. quando `+0x26`/`+0x27` = 0 e no
lado máx. quando = 1. Grade: `2·tamanho + 1` pontos por eixo.

**Grades** (`ComputeGridElevation`, slot `0x1c`; valores em entalhes):
- duas águas e curta (`0x9d3c7c`, `0x9c5f20`): orientação 0 → `min(i, W−1−i)`
  em todas as linhas; ≠ 0 → `min(j, H−1−j)`;
- quatro águas (`0x9c642c`): `min(i, W−1−i, j, H−1−j)`;
- mansarda (`0x9c6568`): o mesmo limitado a `k = max(1, min(min(W/2, H/2) − 1, 7))`;
- meia água (`0x9d3ad8`, último argumento 1): `0,08 + i` (ou `W−1−i` quando
  x1 < x0; no eixo y com y1 < y0);
- meia de quatro águas (`0x9d0094`): `W' = 2·int(|Δx| + 1) + 1`,
  `H' = 2·int(|Δy| + 1) + 1` (com beiral); o eixo cortado (x se orientação
  ≠ 0) dobra para `2W' − 1`; a grade de quatro águas desse tamanho é copiada a
  partir da coluna `W'` (lado alto em x mín., x1 < x0) ou da 0. Na grade real
  (R pontos no eixo cortado): `min(R − 1 − c | c, distâncias no outro eixo)`.

**Altura** (`GetElevation(x, y, margem)`, slot `0x28`, `FUN_009d2cf4`):
`−FLT_MAX` fora dos limites encolhidos pela margem; senão o ponto da grade
`(⌊2(x − xmín)⌋, ⌊2(y − ymín)⌋)` e o triângulo da célula pela paridade
`(i + j) & 1` (par: diagonal (i,j)–(i+1,j+1)), interpolação baricêntrica das
alturas `GetElevation(i, j)` (slot `0x54`, `0x9c6308`: base + s·g ± 0,42 + f(s)).
`Contains` (slot `0x40`, `0x9cb4d4`): dentro dos limites e da grade.
`IsInEaves` (`0x9c57e8`): com beiral, dentro, e fora do retângulo das paredes.
`IsOnPeak` (slot `0x38`): o ponto mais próximo da grade tem g > 0 e g = máximo
(`GetMaxElevation`, slot `0x3c`) → 3 (águas ±x) com orientação 0, `0xC` (±y)
com ≠ 0; meia água: `0xC` se H ≤ W; mansarda (`0x9cc0cc`): `0x10` | 2 (i = k) |
8 (j = k) | 1 (i = W−k−1) | 4 (j = H−k−1). `IsOnDiagonal` (slot `0x34`):
duas águas/curta/meia água 0; quatro águas (`0x9d3728`, m = min(W/2, H/2)):
i = j ≤ m → `0xA`, i > m e j = W−i−1 ≤ m → 9, j > m e i = H−j−1 → 6, ambos > m
e W−i = H−j → 5; meia de quatro águas (`0x9d37d4`): o mesmo na grade dobrada
(eixo dobrado = o menor, deslocado de `W'/2` quando x1 < x0).
Máscaras de direção (`NormalType`, `0xc4e180`): x < −0,1 → 2, y < −0,1 → 8,
x > 0,1 → 1, y > 0,1 → 4 (a normal "para cima" do triângulo, em entalhes).

**Malha** (`cGableRoof::FillMeshBuilderData @ 0xc545d0`; os outros tipos
`FUN_00c54f44`, chamado pelo `MakeTessellatedSolidRoof` de cada classe):
três camadas de posições (−0,42, −0,5075, −0,67), 100 UVs
(`AddTexCoordToMeshBuilder @ 0xc543c4`: índice k → u = t[k % 10],
v = t[k / 10], t = 0, ,1992, ,1992, ,3984, ,3984, ,5977, ,5977, ,7969, ,7969, ,9961),
normais: duas águas 0..3 = as duas águas (−x, +x ou −y, +y conforme a
orientação) e o avesso delas, 4..7 = −x, −y, +x, +y; demais tipos 0..3 =
−y, +y, −x, +x (a 45°), 4..7 o avesso, 8..11 = −x, −y, +x, +y. Subconjuntos:
`subset0` = `ConnectHeightField(0, 0)`, `subsetlower` = `(2n, 2)` nas duas
águas, `(2n, 4)` nos outros, `subsetroofupperstrip` = `DrawOuterStrips(n, 4|8, 0)`,
`subsetroofstrip` = `(n, 4|8, n)`. Materiais (`MakeTessellatedSolidRoof`,
`SetRoofPatternProduct @ 0x6bdefc`): `RoofTop` → subset0, `RoofUnder` →
lower, **`RoofEdges` → faixa de cima** (camadas 1–2, 0,0875) e **`RoofTrim` →
faixa de baixo** (camadas 2–3, 0,1625); `RoofEdges` recebe `textureedges` e
`RoofTrim` `texturetrim` (membros `+0x60`/`+0x64` do produto lidos no
`ReadProperties @ 0x84fc90`; a ordem dos getters `0xdc`/`0xe0` é inferida).

**`ConnectHeightField`** (slot `0x3c`; duas águas `0xc7a188`, quatro águas
`0xc7bef8`, mansarda `0xc7db54`, meia água `0xc7e908`, meia de quatro águas
`0xc804dc`, curta `0xc821e4` com as classes de duas águas): para cada célula
(i, j) de meio tile, em lote x = xmín + i/2, y = ymín + j/2, paridade
`(int(2x) + int(2y)) & 1`:
- par: 1.º triângulo (i,j), (i+1,j+1), (i,j+1) (centro x+1/6, y+1/3);
  2.º (i,j), (i+1,j), (i+1,j+1) (centro x+1/3, y+1/6);
- ímpar: 1.º (i,j), (i+1,j), (i,j+1) (x+1/6, y+1/6); 2.º (i+1,j), (i+1,j+1),
  (i,j+1) (x+1/3, y+1/3).
Cada um: escondido se `IsTriangleCenterLessThanAnyOtherRoofTriangleCenter`;
senão `SetVertexProperties` (`0xc689cc`) com a `NormalType` da normal do
triângulo, visível se ela devolve 1; o triângulo recebe as propriedades dos 3
vértices e a contagem de vértices compartilhados do **outro** triângulo da
mesma célula (0 se ele foi escondido). Normal do vértice: duas águas = base +
(i ≥ W/2 com orientação 0, j ≥ H/2 com ≠ 0); outros = base + (x > 0,1 → 3,
x < −0,1 → 2, y > 0,1 → 1, senão 0) (`0xc4f7f8`). O avesso usa a mesma
visibilidade (o teste é sempre com o último argumento 0) e a ordem inversa.

**Esconder sob outro telhado** (`IsTriangleCenterLess… @ 0xc63558`, todo no
assembly): se o próprio telhado não cobre o centro (`IsRoofAtLocation`),
visível. `e` = a altura do próprio no centro (`GetSampledElevationByRoofID`,
margem 0,5 para telhados de andar acima do ponto); "estrito" = último
argumento 0 e a inclinação global do `cWorldRoof` (`+0x40`) = 0,5. Para cada
outro telhado dos andares L−1..L+2 (menos cone, cúpula, octogonal, pagodes e
estufa, máscara `0x470380`): `eo` = a altura dele no centro; se ele é diagonal
ou de outra inclinação: pula se o centro está no beiral dele, senão
`eo −= s` (2·s com o último argumento ≠ 0); se é da mesma inclinação e
`eo > e + 0,01` e ele é de duas águas/curta/meia água, do mesmo andar e o
centro está no beiral dele: só segue se o próprio é 4/3/1/0xF com a mesma
orientação e o centro está fora das paredes do próprio do lado em que os dois
têm a mesma parede (orientação 0: y < ymín ou y > ymáx com o mesmo valor nos
dois; ≠ 0 o mesmo em x). Esconde se `eo > e + 0,01` e (estrito, ou mesmo
andar, ou `eo > e + s`).

**Telhado com telhado** (`SetVertexProperties @ 0xc689cc`,
`FUN_00c63b04`, `SetVertexPropertiesEx @ 0xc67ca8`): por vértice, os bits
pico (0), diagonal (1) e compartilhado (2), o conjunto de telhados que passam
pelo vértice na mesma altura (`≈` de 32 ULPs, `FUN_014cbe6c`) e, por telhado,
os bits e as máscaras de pico (`+5`) e diagonal (`+4`). `FUN_00c63b04`
decide quem perde: se os 3 vértices são compartilhados, para cada outro
telhado que passa pelos 3: sem pico/diagonal nele → guarda o menor id; com →
as contagens de diagonal/pico (`TriangleFlagCount`) atualizam um mínimo; o
telhado sai do conjunto dos vértices (o bit 2 cai onde só sobra o próprio).
Senão, sai do conjunto todo telhado em que algum vértice é pico/diagonal na
direção da normal do triângulo. Se sobram ≥ 2 vértices compartilhados e algum
telhado saiu, e nenhum passa por todos eles, o bit 2 fica só nos vértices do
primeiro telhado do conjunto. Escondido: com um telhado "liso" nos 3
vértices, o de id maior perde (o menor só se o centro está abaixo do outro);
senão uma tabela das contagens de pico/diagonal do próprio contra o mínimo
achado (no código, `roofMeetsRoofHidden`). Se o conjunto mudou, o
`SetVertexPropertiesEx` refaz os bits: compartilhado quando outro telhado
passa pelo vértice na mesma altura e não é pico/diagonal na direção da normal.

**Célula do atlas** (classes `c{Even,Odd}Triangle{First,Second}{Gable,Hipped,
Mansard,HalfGable,HalfHipped}`, `0xec1a50..0xec9600`; curta usa as de duas
águas): no topo, `ComputeBaseTextureIndex` e depois os 3 índices por
`TexCoordIndicesForNormal{NegX,PosX,NegY,PosY}` pela normal (|x|, |y| ≤ 0,1 →
o mesmo índice 3 vezes: 60 nas pares, a base nas ímpares). Base das duas
águas (e meia água): 60, ou 4 sem espelho com algum vértice de pico; com 1 ou
2 vértices compartilhados, `ComputeTextureIndex{PosX,NegX,PosY,NegY}` pela
normal troca para 0x14/0x1a (um vértice), 6/0x2e (dois, ou o irmão com
dois) ou 0x16/0x2a (a diagonal da célula compartilhada) com o espelho de cada
caso (tabela `kCellRules` no código; o par 1/2 de cada número é "algum vértice
de pico"). Quatro águas, mansarda e meia de quatro águas: com 1 ou 2 vértices
de diagonal, `UpdateTextureMirrorFor{One,Two}DiagonalVertex` (bases 2/0x42,
0x28/0x2a, 0x18/0x56, 0x3e/0x40, 8, 0x2c, 0x52, 0 conforme normal, vértices e
irmão), senão a das duas águas. Mapeamento dos cantos (sem espelho; o espelho
troca u): par 1.º −x (c11, c00, c01), +x (c00, c11, c10), −y (c01, c10, c00),
+y (c10, c01, c11); par 2.º −x (c11, c10, c00), +x (c00, c01, c11), −y (c01,
c11, c10), +y (c10, c00, c01); ímpar 1.º −x (c11, c10, c01), +x (c00, c01,
c10), −y (c01, c11, c00), +y (c10, c00, c11); ímpar 2.º −x (c10, c00, c01),
+x (c01, c11, c10), −y (c11, c10, c00), +y (c00, c01, c11), com
c00 = base, c10 = base+1, c01 = base+10, c11 = base+11. Avesso: UVs fixos por
classe (par 1.º 0/9/90, par 2.º 9/99/90, ímpar 1.º 0/99/90, ímpar 2.º 0/9/99:
a textura de baixo inteira em cada triângulo).

**Faixas** (`DrawOuterStrips @ 0xc66504`): um quad por meio tile em cada uma
das 4 bordas da grade, entre a camada de cima e a de baixo da faixa, escondido
se o meio do segmento está abaixo de outro telhado (o mesmo teste); normal −x,
−y, +x, +y; UVs 0, 1, 9, 8 (v = 0). Mansarda: também o anel em volta do topo
plano (linhas/colunas 7 e n−8), escondido também se
`IsTriangleCenterSupportedByOtherRoof` (`0xc660d0`).

## 2. Cercas (`nTSSG::cFenceGeometryNode`)

Os métodos do `cFenceGeometryNode` no BV estão quase todos sem nome ou com
nomes trocados (o symxfer põe `cWallManager::…` e
`cWaterGeometryBuilder::ProcessHeightChangesForGeomUpdate` nesta área,
`0x753b00..0x75b000`); achados pelo que fazem:

- **Trilho** (`AddRailImpl`, BV `FUN_00759e38`): as duas pontas do
  segmento viram pontos do mundo com `SampledFloorElevation` (`FUN_008a5fc0`);
  a matriz é **translação na primeira ponta × rotação em z por
  `atan2(Δy, Δx)` × cisalhamento em z de `Δz / comprimento horizontal`**
  (o modelo sobe com o x local até o chão da outra ponta), escala 1 (o
  trilho diagonal é outro modelo, `FenceGeometryResource(estilo, diagonal,
  …)` `FUN_00759cc4`, escolhido pelos bits `0x30` do segmento). Mais
  **0,75** em z quando as duas pontas tocam um quarto de piso que a
  consulta do mundo (`FUN_00164a7c`, vtable `0xc8` da interface do
  `cWorldDB`) devolve 4; o que é esse 4 não foi resolvido (a interface tem
  outra vtable; pela posição pode ser o `DeckFoundationStyle`).
- **Poste** (`AddPostImpl`, BV `FUN_0075a4c0`): no vértice, na altura do
  `SampledFloorElevation`, **sem rotação**, escala 1; o mesmo +0,75 da
  regra acima (só para cercas e meias paredes fora de um caso); para uma
  meia parede o recurso é o poste da meia parede (`FUN_0075a350`, tipo 3).

**No código** (`lotFencePieces` em `lot_model.cpp`): o trilho já é isso
(rotação, cisalhamento pela altura do chão nas pontas; o comprimento
dividido por √2 no diagonal dá 1) e o poste também. Fica de fora o +0,75
(condição não resolvida). Nada mudou no código das cercas nesta rodada.

### 2.1 Lido inteiro (rodada n25)

Rodada n25 (2026-10-15), linha 25 do `docs/inventario-binario.md` §6.
Modo: **Ghidra MCP**, só leitura (decompilar, ler memória,
`disassemble_bytes` com `dry_run` para o que não é função no projeto); os
nomes do `cFenceManager` vêm do symxfer ("exact" nesta área) e os do
`cFenceGeometryNode` da ordem das funções no `baserom.sym` (o outro build ppc
com símbolos, `0xc69364..0xc71ba4`) e do que cada uma faz. Corrige a
rodada n06 em um ponto: o `FUN_0075a350` com a parte 3 é o **poste do arco**
(o teste antes dele, que o symxfer chama `Sims1AbsoluteDirectionToSims2Angle`,
é o `PartitionStyleIsFenceArch`), não um poste de meia parede.

**O gerente e os nós** (`nTSSG::cFenceManager`, vtable primária
`0x27e1c78`). Cada andar tem uma forma (`cIShapeRefNode`) com **13 nós
de geometria** (`CreateFenceNodesOnLevel` `0x6ccf60`):

| nó | o que vai | quem escolhe |
|---|---|---|
| 0–4 | trilhos de cerca | `Hash32(x0 + y0, x1 + y1) mod 5` das pontas (`0x6c9380`, só para repartir) |
| 5 | postes de cerca | `DoMessage` (`0x6cf578`): estilo de cerca → 5 |
| 6 | arcos (`AddArchCenter`, vtable `0x34` do nó) | estilo de arco |
| 7–11 | trilhos de meia parede (`PartitionStyleIsHalfWall`, `PartitionType` 3) | o mesmo hash + 7 |
| 12 | postes de meia parede | estilo de meia parede → 12 |

`AddFence` (`0x6cc134`) só aceita um localizador que existe no gerente de
partições (`0xac`), com um dos dois tiles ao lado **no lote** (`IsOnLot`,
`0xf8`) e estilo de cerca (`PartitionStyleIsFence`: `PartitionType` 1 ou
3); aí `AddRail` (`0x6caa20`) põe o trilho no nó do hash. Os postes vêm da
**camada de postes** do mundo (o FPL do lote): ao carregar (`DoMessage`,
mensagem `0xcc350cdd`) o gerente refaz todas as cercas (`0x18`), os arcos e,
para cada poste da lista do gerente de partições (`0xf4`, estilo pela
`0x104`), `AddPost(estilo, local, 0)` no nó 5 ou 12, salvo onde um portão
esconde o poste (`FUN_006c9448`). O app já desenha os postes do FPL.

**Modelos** (`FenceGeometryResource`, vtable `0x164` do nó, BV `0x754000`,
não é função no projeto): a entrada de cerca do estilo (catálogo `0x68`),
senão o produto do estilo (`0x7c`), senão **o produto de cerca padrão**
(`0x6c`, o `default` do `fences.txt`: a cerca 2, `fencePicketRail`); trilho
→ `Rail` (`0x174`) ou `DiagRail` (`0x178`) conforme o bit diagonal, poste →
`Post` (`0x17c`); depois pelo nome (`0x753ea0`). Um modelo vazio não desenha
nada (o "Ancient Guardian Rock Wall" e o "FauxStone Wall" não têm poste; o
diagonal **não** cai para o trilho reto). Arcos (`0x754130`): parte 0
`archCenterModel`, 1 `archEastModel`, 2 `archWestModel`, 3 `archPostModel`.

**Trilho** (`AddRailImpl` `0x759e38`, chamado por `AddRail` `0x75a240` e por
`UpdateRailsAndPosts` `0x75a8d0`): o que a n06 leu (translação na ponta `a`,
rotação `atan2(Δy, Δx)`, cisalhamento `z += (Δz / comprimento horizontal) ·
x`, escala 1; a matriz é de vetor-coluna, `FUN_000eac50` põe a translação na
coluna 3, e o cisalhamento vai no elemento [2][0]), o diagonal pelos bits
`0x30` do localizador. O **+0,75** vai na translação quando
`FUN_00166414` diz que as **duas pontas** tocam um quarto "4":

- `FUN_00164a7c(vértice)`: os quatro tiles em volta do vértice, no andar do
  vértice (tabela `0x27324e0`: tiles `(x−1, y−1)`, `(x, y−1)`, `(x, y)`,
  `(x−1, y)` com os quartos que tocam o vértice, máscaras `6` = norte e
  leste, `3` = oeste e norte, `9` = oeste e sul, `12` = leste e sul, na
  ordem oeste/norte/leste/sul dos pisos), cada tile **no lote** (`0xf8`), e
  algum desses quartos com **`DeckFoundationStyle` = 4** (`cIWorldDB`
  `0xc8`, thunk `0x1511944` → `cWorldDB::DeckFoundationStyle` `0xd2d08`: o
  marcador do quarto no 3ARY `0x14`, `u32` por quarto, −1 nenhum).
- É o **estilo cru** (o ID do produto do `deckfoundations.txt`) igual a 4,
  não o tipo: o `nTSWorld::HasDeck` (`0x168400`) é que pergunta o tipo
  (`IsDeckFoundationOfType(estilo, 4)`, `0x163250`: produto do estilo,
  vtable `0xdc`). O `deckfoundations.txt` do jogo base só tem os produtos
  0 (fundação), 1 e 2 (decks) e 3 (piscina), e os 3ARY `0x14` dos 69 lotes
  só têm 0, 1, 2 e 3 (31 888, 1 542, 374 e 2 656 quartos): **no jogo base
  nenhuma cerca ganha o +0,75**. O parser do BV conhece uma quarta palavra,
  `stage` (`0x1046dac`, ao lado de `deck`/`foundation`/`pool`): o estilo 4
  deve ser o palco de uma expansão (o nó de parede 4, "parede de palco",
  tem o topo no chão + 0,75; inferido).

**Poste** (`AddPostImpl` `0x75a4c0`): estilo de cerca → o `Post` do estilo;
estilo de arco → a parte 3 do arco (`0x75a350`), **sem** +0,75. No vértice,
`SampledFloorElevation`, sem rotação, escala 1, mais **0,75** quando
`FUN_00164a7c(vértice)` (**um** vértice, não dois) e, se o estilo é de meia
parede, `FUN_00a7098c(vértice, estilo)` (o symxfer diz
`cAlbumEntry::~cAlbumEntry`): alguma parede do grafo `0xb` do gerente de
partições naquele vértice tem o mesmo estilo.

**Arco** (`AddArchCenterImpl` `0x75b8a8`, chamado por
`AddRemoveFenceSegments` `0x75bed4`): o centro (parte 0) na ponta `a`
(`SampledFloorElevation`), girado por `atan2(Δy, Δx)`, **sem
cisalhamento**; a parte oeste (2, `AddArchWest` `0x75ae78`) no vértice `a`
**só se não há arco** no segmento `a → a − d` (o gerente de partições,
vtable `0x118` = `cPartitionManager::FenceArchExists` `0xc2f360`, o grafo
de arcos `+0x58`), senão tira a peça dali (`FUN_00756bac`); a parte leste
(1, `AddArchEast` `0x75b0c8`) em `b` só se não há arco em `b → b + d`. As
duas com a rotação do arco, sem +0,75. Arcos em fila juntam-se sem as
pontas do meio.

**Modos de parede** (`SetLevelUpDownState` `0x6cf32c`, da mensagem
`0xcb46c21d` por andar): com as paredes baixadas o andar **tira os nós
7–12** (trilhos e postes de meia parede) e solta o nó dos modelos presos às
meias paredes (`+0xa0` do andar); erguidas, põe de volta. Os nós 0–6
(cercas, postes e arcos) **não mudam** em modo nenhum. `ActivateWallsUpSubsets`
(`0x6cee10`) troca o material de todo subconjunto com `togglewalls` no nome
entre `…wallsup` e `…wallsdown`: nenhum SHPE/TXMT do `Sims3D` do jogo base
tem essas palavras. O gerente não ouve as mensagens do corte (cutaway):
**cerca nenhuma é cortada**, como o app já fazia. Portões
(`cObject::HideFenceAtPortalLocation` `0x653e6c` → `AddGate`, vtable
`0x24` do gerente) escondem o trilho e o poste onde ficam; o base não tem
portões.

**Nos dados do jogo base:** 5 549 postes no FPL, todos de cercas do
catálogo; nenhum segmento de cerca com estilo desconhecido; nenhuma cerca
com revestimento (meia parede: o catálogo base não tem produto de cerca do
tipo 3); nenhum arco nos lotes; 3ARY `0x14` sem o estilo 4. Ou seja, as
regras acima dão nos lotes do jogo **a mesma imagem** que o código já
desenhava; elas só mudam lotes com conteúdo de expansão ou feitos à mão.

**No código (rodada n25):**

| Peça | Antes | Agora (binário) |
|---|---|---|
| Marcadores de deck/fundação | não lidos | `Lot::deckFoundationStyles` (3ARY `0x14`, do andar mais baixo até o de baixo do topo: os lotes guardam um plano a menos que as alturas) |
| +0,75 | de fora (condição não resolvida) | trilho com as duas pontas, poste com o ponto tocando o estilo 4 (`touchesRaisingStyle` = `FUN_00164a7c`) |
| Estilo sem cerca no catálogo | não desenhado | a cerca padrão do `fences.txt` (`PatternCatalog::fenceOrDefault`) |
| Diagonal sem modelo diagonal | o trilho reto esticado √2 | nada; escala sempre 1 |
| Postes | do FPL, os de estilo de arco contados como "desconhecidos" | do FPL, os de estilo de arco deixados para o arco |
| Arcos | as duas peças das pontas sempre; postes girados com o arco | a peça da ponta só onde nenhum arco continua a linha; postes sem giro |
| Meia parede (cerca coberta) no estilo 4 | nó 3 | nó 5 (0,1 de espessura, +0,75), baixada com as paredes |

Teste: `testFencePieces` (catálogo de mentira com o `fences.txt` e o
`fencearches.txt`: rampa, diagonal, sem diagonal, +0,75 nas duas pontas e
num só ponto, poste de arco, arcos em fila, meia parede no nó 5) e
`testDeckFoundationMarkers` (com os dados: os marcadores do `N001_Lot68`).
Antes/depois (antes à esquerda), `--lot` sem tela: das 12 capturas, 10 saem
com **os mesmos bytes**; as outras duas (`N001_Lot77`, `N001_Lot68` com as
paredes baixas) diferem em 2 e 65 pixels de árvores, e o mesmo binário rodado
duas vezes também difere ali (não é das cercas):

- `docs/screenshots/n25-cercas-antes-depois.jpg`: grade de ferro e alpendre
  sobre a fundação (`N001_Lot11`), muro de tijolo subindo ao lado da escada
  (`N001_Lot77`), canteiro em zigue-zague diagonal (`N003_Lot48`), ferro com
  diagonais e o muro de pedra sem postes (`N001_Lot79`), cerca de estacas no
  morro (`N002_Lot10`);
- `docs/screenshots/n25-cercas-modos-antes-depois.jpg`: o parapeito de
  tijolo no telhado (`N002_Lot31`), o muro de tijolo na borda do lote
  (`N003_Lot52`) e `N001_Lot68` com as paredes erguidas, baixas e em corte
  (as cercas e o guarda-corpo do alpendre não mudam).

**Inferido ou de fora:** o estilo 4 como palco (só o nome `stage` no
parser); a meia parede como "cerca com revestimento" (o jogo pergunta o
`PartitionType` 3 do estilo; o catálogo base não tem nenhuma) e, por isso,
a segunda condição do poste de meia parede (`FUN_00a7098c`) e os nós 7–12
escondidos no modo baixo; os postes dos arcos (o jogo os tira do FPL com o
estilo do arco; o modo Construção do app não grava esses postes, então o
desenho põe um em cada ponta); portões (`AddGate`) e os subconjuntos
`togglewalls` (nada disso no jogo base); o modo Construção não grava o 3ARY
`0x14` (decks, fundações e piscinas novos ficam sem marcador).

## 3. Água do terreno e piscinas

### 3.1 Água do terreno (`nTSSG::cWaterGeometryBuilder`)

Os nomes do symxfer nesta classe também erram; o que foi lido:

- **`ReadWaterRenderParameters`** (BV `FUN_00c42638`): lê
  `TSData/Sys/Terrain.ini` `[WaterRenderParameters]`: `AlphaVsDepthCurve`
  (sem ela, a reta (0, 0)–(4, 255)), `ShallowPaintDepthMin/Max`,
  `DeepPaintDepthMin/Max`, `BeachPaintStartDepth/PeakDepth/EndDepth`,
  `MaxBeachWidth` e `BeachIntensityFactor` (padrões −0,1, 0,6, 0,4, 1,2,
  −0,2, −0,02, −0,2, 0,8 e 1 sem o ini). O jogo base traz
  `.25,62, 10,110`, 0/5, 0,5/2, −0,35/−0,1/0,3, 1,5 e 3,2.
- **Profundidade e alfa** (`SetWaterDepth`, `FUN_00c41644`): para cada
  ponto da grade, profundidade = nível da água − terreno; a cor do vértice
  da água é branca com **alfa = curva(profundidade)** (62/255 na margem,
  110/255 a 10 de fundo).
- **A malha**: a grade de água tem os cantos e o centro de cada tile; cada
  tile são 4 triângulos (centro + aresta), cada um mostrado se algum dos 3
  vértices tem água acima do terreno (`IsWaterTilePartUnderTerrain`,
  `FUN_00c41488`).
- **O material** (`TerrainWater`, `terrain.matShad`): passada 1 =
  reflexo de `reflectionoutdoorwater-envcube` × `colorScalar (1,1,1)`,
  alfa do vértice, `srcAlpha/invSrcAlpha`. Com pixel shader (o Mac tem,
  `maxPixelProgramVersionMajor` = 1) há a passada 2: `water-1-bump` ×
  `water-2-bump` animadas (translação senoidal 0,04 Hz em sentidos
  opostos, escala `$waterbumpMapScale` = 4) com alfa 0,3.
- **O leito e a praia** (`PaintUnderWaterTerrain`, `FUN_00c44670`, chamado
  por `FUN_00c44be4` para os tiles cuja profundidade mudou mais de 0,01):
  três camadas de pintura do terreno, `terrain-seabed-shallow`,
  `terrain-seabed-deep` e `terrain-beach` (`+0x140..0x142` do
  construtor), na resolução da pintura. Cada texel pega a profundidade
  bilinear dos 4 cantos do tile e: o raso sobe de 0 em `ShallowMin` a 254
  em `DeepMin` e desce a 0 em `ShallowMax`; o fundo sobe de `DeepMin` a
  254 em `DeepMax` (255 depois); a praia sobe de `BeachStart` a 255 em
  `BeachPeak` e desce a 0 em `BeachEnd`; acima da água (profundidade < 0)
  ainda × (1 − d / `MaxBeachWidth`) × `BeachIntensityFactor`, com d a
  distância ao ponto da grade debaixo d'água mais perto (só os que estão a
  até `round(MaxBeachWidth)` tiles do tile).

**No código** (`src/formats/lot_water.*`, `addWater` e a lista de pinturas
em `lot_model.cpp`, o tipo `TerrainWater` em `model.cpp`/`renderer.cpp`):
a curva e as três pinturas são as do binário, com o `Terrain.ini` lido; a
água é só o reflexo com o alfa do vértice. **Inferido**: os quads da água
sem o vértice do centro (o terreno nosso também é de 2 triângulos por
tile), a ordem das três pinturas (depois das do lote), a grade da pintura
quando o lote não tem pintura (4 pontos por tile). **Falta**: a passada 2
(as duas texturas de bump animadas multiplicadas). Antes e depois:
`docs/screenshots/n06-agua-lago-antes-depois.jpg` (N001_Lot79).

### 3.2 Piscina (`nTSSG::cPoolGeometryNode`)

Nomes contíguos em `0x73f5ac..0x742ff0`. `CreatePoolWater` (BV `0x741eb0`,
chamado pelo `BuildGeometry` `0x742b38`) monta, para cada tile de piscina
(`AddFloorTileGeometry` `0x7415b8`: os quartos do tile partidos nos pontos
médios, na altura do `SampledFloorElevation`):

- a **superfície** `poolwater-1`: o material `poolWater-1` do
  `water.matShad` (`PoolWaterSurfaceMaterial`), que no caminho DX8 do Mac
  é o **`RegularWavesHLSL`**: `one + one`; duas ondas senoidais (fase =
  posição · (repeat, 0, altura) + tempo · velocidade; no `poolWater-1`
  repeat 5 e 6, altura 0,01, velocidade 3) inclinam a normal; cor =
  `swimming_pool-envcube` ao longo do reflexo × (1 − N·V) × as luzes de
  textura × 0,5, mais o brilho `(saturate(R·L) + 0,1)^15` da primeira luz
  (sem a cor da luz);
- **três camadas de fundo** `poolwaterlayer-1..3`
  (`AddFloorTileGeometryForPoolLayer` `0x740ab4`): a mesma malha 0,75 ×
  k abaixo da superfície, com `PoolDepthLayerMaterial`:
  `swimming_pool_deeplayer` × `poolLayerColour` (0, 0,5, 1), alfa × 0,9,
  misturadas, sem luz; `AddPoolDepthLayerColors` (`0x740fa0`) dá aos
  vértices nas paredes da piscina outra coordenada de textura (a textura é
  branca com alfa 68 no meio e 0 na borda: as camadas somem junto às
  paredes); os três valores ficam em dados preenchidos em tempo de
  execução (`0x2c1f584..0x2c1f594`), não lidos;
- a camada 0 (`poolWaterLayer-0`) só gera as cáusticas.

**No código** (`addWater`, materiais `PoolWaterSurface`/`PoolWaterLayer`,
modo `kPoolWater` do shader): superfície e camadas como acima (grade 3 × 3
por tile). **Inferido**: as coordenadas das camadas (parede = canto
transparente, resto = centro), o tempo das ondas em segundos, a "primeira
luz" = a primeira luz do estado (o `sun`); à noite o brilho do `sun` da
noite fica grande e branco nesta vista, como a conta manda. **Falta**: as
cáusticas. Antes e depois: `docs/screenshots/n06-piscina-antes-depois.jpg`.

## 4. Sombras de objetos

As sombras dos objetos são os planos GUOB do próprio modelo (seis
subconjuntos, tabela `0x277a8c0`: `groundshadow`, `wallshadow`,
`northwallshadow`, `eastwallshadow`, `westwallshadow`,
`southwallshadow`). `cShadow::UpdateGUOBs` (BV `0x678488`) liga, no
estilo 3: o do chão sempre, o `wallshadow` conforme uma consulta da vista
(vtable `0x6c`), e os de parede onde há parede daquele lado
(`FindObjectWallPresence` `0x675d20`); com o bit 9 das opções todos;
`EnableGUOB` (`0x67417c`) troca o material do subconjunto pelo do objeto,
por `VisGUOBMaterial` (bit 7, depuração) ou por `GUOBOff`. O
`InitGUOBs` (`0x6747c8`) guarda o material original de cada subconjunto
(`FindGUOBMaterialFromSubset`: um `GUOBOff` salvo é tratado como sem
material).

**No código**: os planos já eram desenhados com o material salvo no lote
(o estado que o jogo deixou ao salvar). Conferido nesta rodada que eles
aparecem nos lotes (`N001_Lot11`: diferença de até 136/255 por pixel com e
sem eles; são sombras claras na grama). Corrigido: as pinturas do terreno
iam na mesma camada (7) que os planos e, ordenadas pela distância,
podiam ser desenhadas por cima deles; agora vão com o terreno (camada 6),
como no jogo, onde são passadas do shader do terreno. **Inferido / falta**:
recalcular quais GUOB de parede ligam quando um objeto muda de lugar (hoje
vale o estado salvo) e a condição do `wallshadow` e do `this+0xc`.
