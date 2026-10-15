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

`src/formats/lot_roof.cpp` é o porte do OpenTS2 (faces inclinadas com
tiles do atlas cortados onde as faces se cruzam). Comparado ao binário:

- **igual**: beiral de 0,5 tile, tile de meio tile, atlas 5×5 de 102/512,
  inclinação por meio tile, altura do beiral e da linha da parede (a menos
  de 0,005 com s = 0,5);
- **corrigido nesta rodada**: espessura (0,0875 + 0,1625 = 0,25 em vez de
  0,075 + 0,15), a altura com a regra `−0,42 + f(s)` (muda só para
  inclinações ≠ 0,5), o +0,08 da meia água e o topo plano da mansarda em
  `k` (os dois do §1.2);
- **inferido, mantido**: a triangulação (o OpenTS2 corta cada tile em 4
  triângulos com vértice no centro; o jogo usa 2 com a diagonal alternada),
  a escolha da célula do atlas por tile (o jogo escolhe por triângulo com
  as propriedades de vértice) e o encontro de telhados (o OpenTS2 corta
  tiles face contra face; o jogo esconde o triângulo cujo centro fica
  abaixo de outro telhado). Portar isso é trocar o `lot_roof.cpp` inteiro:
  fica no `TODO.md`.
