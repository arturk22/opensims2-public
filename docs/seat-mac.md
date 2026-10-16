# Sentar no binário de Mac (issue 001)

Leitura do binário de Mac (Bon Voyage 1.10, `sims2_bv.ppc`) para a issue
`docs/issues/001-sentar-fiel-ao-binario.md`, rodada n01 (2026-10-15).
Modo: **Ghidra MCP** (só leitura; nada renomeado nem salvo), mais o mapa de
nomes do `tools/symxfer` (`vtables.py` para as vtables). Endereços BV.

Atenção: vários nomes do symxfer nesta área estão **errados** ("moved"):
o `ScanConeForSittingGoals @ 0x4f64e8` do Ghidra é uma função de telhado.
A verdadeira é `FUN_004788cc` (achada por quem chama o callback
`ScanConeForChairRoutingSlotsCB`). Os nomes com "exact" bateram.

## Estruturas usadas

**Objetivo de rota** (`cTSRouteGoal`, 0x1C bytes; vetor em `cTSRoute+0xC8`):

| campo | o que é |
|---|---|
| +0x00, +0x04 | x, y (`FTilePt`) |
| +0x08 | andar (s16) |
| +0x0C | placar (float; 100 − penalidades + bônus; maior é melhor) |
| +0x10 | **a cadeira** de um objetivo de sentar (ID; 0 em pé) |
| +0x12 | direção (s16) |
| +0x14 | índice do osso no slot (o "multi slot index", dado de pessoa 38) |
| +0x16 | o contêiner de um objetivo de contenção (ID) |
| +0x18 | o slot de contenção + 1 |
| +0x1A | ativo (1) |
| +0x1B | já tentado |

**Flags do slot de rota** (`cTSRoutingSlot+0x30` = o u32 `flags` do SLOT):
bits 0–7 direções; `0x2000` sorteio; `0x200000` "associado a um slot de
contenção" (vtable `0xA8`); `0x200000 | 0x2000000` "objetivo de contenção"
(vtable `0xB4`). Os pesos: vtable `0xD8(i)` = em pé (0), sentado (1), chão (2).

Levantamento nos dados (`objects.package` e os do bairro): nenhum slot de rota
tem `0x2000000`, então **o objetivo de contenção (`CreateContainmentGoalPoint`)
não roda no jogo base**. Só 14 têm `0x200000` (escada e trampolim da piscina,
"Route - Step Over", dois da poltrona Club). Têm **peso de sentar** 51 slots
sem osso (TVs: sentado 1000 contra 1 em pé; lareiras; contas; congregador;
"Social - Talk - Group Talk" 500; bingo; teleprompter; arco de casamento;
o slot 30 do motorista do ônibus e dos NPCs) e 4 slots de osso (cadeirão,
os dois vasos, caixa de encaixar).

## A. Objetivos de sentar

**Gerador de objetivos** (`BV 0x478d2c`, sem função no Ghidra; trecho
`0x479100`–`0x479880`):

1. Pesos: `r22 = sentado × 10`, `r24 = max(em pé, chão) × 10`; sem nenhum
   dos dois, `r24 = 10`.
2. Slot de contenção (`0xB4`): só `CreateContainmentGoalPoint` (não roda no
   jogo base, ver acima).
3. Slot de osso (`0x20000` do descritor): `GetPossibleSlotsFromModelRoutingSlot`
   (`0x475b98`) com os dois pesos.
4. Varredura em cone: o raio do roteador (vtable `0xB4` do roteador; negativo
   com a vtable `0x5C` do slot; 0 com a `0x64`) soma às distâncias mín.,
   preferida e máx. (máx. limitada a 32). Depois:
   - os dois pesos > 0: se sentado ≥ em pé, **primeiro os de sentar**
     (limites 48 aceitos / 100 candidatos), depois os em pé com os limites
     +16; senão o contrário;
   - só um peso: só aquela varredura.
   - Em pé: `0x478518` (varredura `0x4778b4`, callback `0x4740d8`); soma
     `em pé × 10` ao placar de cada objetivo (+ sorteio, +25 fora da máscara
     de paredes, +20 da sala).
   - Sentado: `ScanConeForSittingGoals` = **`0x4788cc`**.

**`ScanConeForSittingGoals` (`0x4788cc`)**: a mesma varredura `0x4778b4`, com
a largura extra do cone (`+0x44`) = 1 e a resolução (`+0x38`) = 1 tile, no
modo "objetos" (`param_5 = 1`): cada ponto do cone dá os objetos do tile
(`ObjectOnTileAtIndex`) ao callback. Com largura > 0 as distâncias mín./máx.
abrem 1 tile cada lado e cada fileira fica `|lateral| ≤ d·tan(ângulo/2) + 1`.

**`ScanConeForChairRoutingSlotsCB` (`0x4776fc`)**, para cada objeto:
`IsChair` (OBJf 26 "sit" tem árvore, `0xaff0c8`), **a direção da cadeira =
direção do cone + 4** (a cadeira olha para o objeto), não à venda (od 106),
`AreInteractionsDisabled(false)` falso (od 87 < 1 e od 82 bit 2 desligado),
e o slot 0 dela vazio **ou ocupado pelo próprio roteador**: então
`GetPossibleSittingSlotsFromModel(cadeira, pesos)`. Ocupada por outro: marca
"ocupado" e guarda o ocupante (resultado 7 se nada sobrar).

**`GetPossibleSittingSlotsFromModel` (`0x4763e4`)**: só se a cadeira tem a
árvore "stand" (OBJf 27, `HasStandFunctionalTree`). O **primeiro** slot de
rota de osso (`0x20000`) sem `0x200000` cujo rótulo tem **mais de um osso**
vai a `GetPossibleSlotsFromModelRoutingSlot` (o binário chama de novo o mesmo
slot depois do laço: objetivos em dobro, sem efeito).
`GetPossibleSittingSlotsFromWholeModel` (`0x47664c`) faz isso em cada parte
de um multi-tile.

**`GetPossibleSlotsFromModelRoutingSlot` (`0x475b98`)**: um objetivo por osso;
`+0x10` = **ID do objeto se peso de sentar > 0, > peso em pé e o objeto é
cadeira** (`IsChair`); placar = 100 − penalidades do `EvalTileForGoal` + 25
(direção fora da máscara; na varredura de cadeira a máscara é 0, então
sempre) + 20 (`HasStationaryBonus` e mesma sala).

De volta em `0x4788cc`, para cada objetivo: `EvalTileForGoal`; válido →
`placar += peso_sentado×10 − penalidade` e, com gradiente > 0, `− |d −
preferida|·16·gradiente − 0,01·gradiente·|lateral|` (d e lateral do cone).

**Chegada** (`InitRoute`, `0x5737cc`): depois do `FindPath`, se
`IsPersonAtContainmentGoal` (`0x471b08`: contêiner do Sim = `+0x16`) ou
**`IsPersonSittingOnChairGoal` (`0x471984`: o Sim já está contido na cadeira
`+0x10` de um objetivo ativo)** → a rota chega na hora (esse objetivo vira o
atual). Senão `IsPersonAlreadyAtGoal` (`0x471f90`): no ponto e andar do
objetivo, e (contenção igual) ou (cadeira = o contêiner do Sim e ele
sentado) ou (sem cadeira, solto e em pé), e o placar até 1 abaixo do melhor.

**Pós-rota** (`TriggerPostRouteTree`, `0x577cf4`): parâmetro 0 = `+0x10` do
objetivo atual (a cadeira), 1 = resultado. A tabela da
`docs/routing-mac.md` §8.3 trocava os dois; o código já usava esta ordem.
A "Function - Post Route" da pessoa (`7F01EC29`/`0x2048`) roda a função
"sit" da cadeira com ela.

**Diferença no nosso código** (`vm/routing.cpp` `routingGoals`): não havia
objetivos de sentar; o parâmetro 0 da pós-rota era sempre 0; os pesos não
entravam no custo; o Sim sentado no sofá refazendo "Ver TV" levantava.
`DoesPathStartFromContainment` (`0x4715c4`, vtable `0x4C`): o caminho tem
≥ 2 nós e o primeiro é de contenção (`+0x40 == 1`, `+0x44 == 3`); usado
pela locomoção, não decifrado mais.

## B. Snap Into e colocar no slot

**`cTSObject::TrySnapInto` (`0xe4253c`)**: operandos como já lidos (objeto
em 0/1–2, contêiner em 3/4–5, slot no byte 6 ou temp 0 com a flag 0x01 do
byte 9). Passos:

1. meus od 52 e 54 = 0;
2. objeto ou contêiner que não existe → erro 0x17;
3. `TSObjectCanPlaceInSlot` (`0x8aa14c`) falha → **od 52 = primeiro erro + 2**,
   falso;
4. senão, sem a flag 0x02 (teste): `PlaceInSlot`; com 0x04, zera os ossos de
   raiz (animatable vtable `0x154`); verdadeiro.

**`TSObjectCanPlaceInSlot` / `TestPlaceInSlot` (`0x8a6264`)**, os erros (o
primeiro conta):

| teste | erro |
|---|---|
| objeto multi-tile | 0x11 |
| o contêiner não tem esse slot | 0x0E |
| slot ocupado por outro | 0x0C |
| od 28 do contêiner ("support strength") < od 27 do objeto ("weight") | 0x0F |
| o objeto é o contêiner ou um dos contêineres dele | 0x0C |
| od 4 do objeto ("allowed height flags") & máscara da altura do slot = 0 | 0x25 (altura 13 = todas; senão `1 << (altura − 1)`) |
| árvore 0x34 do contêiner (OBJf, "allow in slot") devolve ≠ 0 | o temp 0 dela |

**`cTSObject::PlaceInSlot` (`0xafcc28`)**: `RemoveFromWorld` (sai do tile e do
slot anterior), mensagem, `AggregateOnPlaceInSlot` (`0xafb7b0`: os
`OnPlaceInSlot` do `nTSSG::cObject`/`cAnimatable`), o slot guarda o ocupante,
od 2 = contêiner, od 3 = slot, **posição = a do contêiner**, **direção =
`AggregateDirection` ou a do contêiner**, depois `OnPlacement`.
`RemoveFromWorld` (`0xafc508`) zera o ocupante do slot antigo, od 2 = 0,
od 3 = −1.

**Diferença** (corrigida nesta rodada, `vm/routing_walk.cpp` `snapInto`,
`vm/placement.cpp` `slotPlacementError`/`placeInSlot`): o `snapInto` não
punha a direção, dava erro de VM com slot inexistente (o jogo dá falso com
od 52 = 16), não escrevia od 52, e não tinha os testes de peso, de cadeia e
de altura. O teste por árvore 0x34 fica de fora (não conferido se algum
objeto base a usa). O teste de multi-tile (0x11) é aproximado: uma parte
cujo líder é outro objeto.

## C. Onde fica o corpo no slot

`nTSSG::cObject::OnPlaceInSlot` (`0x668114`): o nó de cena do objeto vira
**filho do osso do slot** do contêiner (o nome do osso pelo índice do slot,
`ModelBoneName`), com o deslocamento `ObjectSlotOffset` (`0xaf9a50`: zero nos
slots de osso; o vetor do slot nos outros), depois
`ApplyAnimationOffsets`/`ApplyAnimationRotation`. **A VM só guarda a posição
do contêiner**; o lugar fino é do grafo de cena. É o que o app faz
(`SeatModule::place`, `slotPlacement`): **confirmado**, sem mudança.

`cTSPerson::UpdateSlotDisplacement` (`0xac9bc8`): só com o dado de pessoa
186 ≠ 0 (por idade, 0,0709 / 0,184 / 0,38). No jogo base é 0: nada.

## D. Salto da raiz

`snap_to_current_location` (`0x68b9ec`) = `PreSnap(false)` + `PostSnap`
(`0x68f204`, passa o quadro em `+0x160` do animatable ao componente de
coreografia). `ApplyAnimationOffsets` (`0x654b28`): translação local +=
(od 108, 109, 110) × 0,01; `ApplyAnimationRotation` (`0x656b10`): gira pelo
od 111 × k. Chamadas no `OnPlaceInSlot`. **Nenhuma árvore do jogo escreve os
od 108–111** (varredura das 22 396 BHAVs: zero escritas de dados de objeto ≥
100 com dono "meu"/"stack object"), então valem 0: o "não aplicado" da
issue não muda nada. O `stayInWorld` continua a nossa leitura do `PostSnap`
(inferido; o componente de coreografia é da rodada 2).

**Rodada n44 (Ghidra MCP, só leitura; BV): o que o `PostSnap` faz.**

- **Cada pedido tem o seu quadro.** `AddChoreoAnimRequest` (BV `0x696644`)
  dá ao pedido o `ObjectTransform` = a transformação **local** do nó do
  objeto (vtable `0xd4`) e o nó pai dele (vtable `0x80`)
  (`cChoreoRequest::SetObjectTransform`, `0xd937b8`: `+0x50..+0x68` e o pai
  em `+0x6c`). O `cChoreoQueue::AddChoreoRequest` (`0xd1d194`) põe nos deltas
  da raiz do pedido `RootTransDelta = O·P·N⁻¹` e `RootRotDelta = R·(O·P·N⁻¹)·R⁻¹`
  (`SetRootTransDeltaTransform`/`SetRootRotDeltaTransform`, vtable
  `0x64`/`0x68`; `O` o quadro do pedido, `P` o global do pai, `N` o global do
  nó, `R` o local do `root_trans`; o produto `FUN_000895f4` é "a e depois
  b", a inversa `FUN_0008a8d4`): a identidade quando o pedido nasce. A cada
  quadro os canais de `root_trans`/`root_rot` de um pedido são multiplicados
  pelo delta dele (BV `0xd25e5c`: com a flag `+0xac` do canal, `+0xad`
  escolhe o de translação ou o de rotação). É a "mistura em vários quadros"
  (`ResetMultiCoordinateBlending`).
- **Quem recalcula os deltas**: só o `cChoreoQueue::UpdateObjectTransform`
  (BV `0xd1d8a8`; vtable `0x104`, `0xec` pela interface `0xfb87c2b9`), para
  **todos** os pedidos com pai (laços também): o quadro de cada pedido volta
  a ser o do seu nascimento, relativo ao nó como está agora; os ossos
  `root_trans` (translação) e `root_rot`/`0xedb8021a` (rotação) ganham o
  delta velho→novo (`G0·N⁻¹`), e a fila guarda o local e o global novos do
  nó (`+0x70`, `+0x8c`). Quem chama: o `PostSnap` (`0x68f204`; o `PreSnap`,
  `0x69780c`, guarda em `+0x160` o `GlobalTransform` do nó antes, e com o
  `bool` ligado roda a fila até agora e marca os expirados). Os pares
  `PreSnap`/`PostSnap` estão em `snap_to_current_location` (`Reset` do
  `cTSObject`/`cEdithMTObject`, `PostLoad`, `ForceToEdithLocation`), no
  **`cLocomotable::LocoPlaceFromSceneGraph`** (BV `0x6b2ee0`, vtable `0x41c`:
  `PreSnap`, `SetPosition` + `Orient`, `PostSnap`), que o `FollowOneStep`
  chama a cada tick (`0x6b8704`, `0x6b8724`) e **na entrega do fim do
  caminho** (`0x6b8ae0`, o Sim no fim com a direção final), e no
  `KillLocomotion` (`0x6af720`).
- **O Snap da VM não recalcula.** `cTSObject::TrySnap` (`0xe43084`) faz
  `BeginEditing`, `SetPosition`/`PlaceInSlot` com o `bool` **falso**,
  `Orient`, `FinishEditing`; o `cAnimatable::OnSetPosition`/`OnOrient`/
  `OnPlaceInSlot` (`0x689d2c`/`0x689c94`/`0x689ddc`, iguais nas seis classes)
  só move o nó (`cObject::OnSetPosition` `0x65a7d0`: translação e
  `ApplyAnimationOffsets`; `OnPlaceInSlot` `0x668114`: filho do osso do slot)
  e só com o `bool` chama o `ResetMultiCoordinateBlending` (`0x68b958` →
  `InitObjectTransformFromSkeleton`, que só guarda o quadro do nó). Ou seja,
  como lido, depois de um Snap, Snap Into ou contenção os pedidos e os
  ossos segurados **seguem o nó**.
- **Snap Into 0x04** ("reset root bones") é o `ResetRootBones` (vtable
  `0x154`; `cObject` `0x6550d0`: translação do `root_trans` = 0, rotação do
  `root_rot` = identidade; `cAnimatable` `0x69a678`: e um pedido
  `o-generic-resettozero-pose`, que segura os dois em zero). No jogo base:
  comida e louça postas em slots ("CT - Put", "Sub - Presto - Dish") e Sims
  no carro ("Sub - Sit in Car"); as cadeiras não usam.
- **`GlobalRootTransPosition`** (`0x68c1d0`, vtable `0x3bc`): a posição no
  mundo do nó `root_trans` da fila (`0x188`), ou a do objeto. Quem usa: o
  `KillLocomotion`, que põe o Sim de uma rota interrompida onde a raiz da
  animação está (no ponto seguinte do caminho se a menos de 0,035) com a
  direção do `GlobalRootRotHeading` (`0x68c320`), entre `PreSnap` e
  `PostSnap`.
- As árvores fazem o Snap **antes** da animação que começa no lugar velho:
  "Chair - Animate - Get In" (`0x7F0B61F3 0x202E`) faz o Snap Into e depois
  toca o get-in feito no quadro do assento; o "Get Out" (`0x2045`) faz o Snap
  ao canto e o "Snap - Away" e depois toca o get-out que começa sentado; a
  escada reta ("Function - Portal", `0x7F429113 0x1001`) faz Snap ao pé,
  sobe (`c2o-stairsStraight-up`, raiz a 4,4 adiante e 3,6 acima) e faz o
  "Snap - Away" quando o Animate Sim volta (no começo do blend de saída).

**O que mudou**: na entrega do fim do caminho (o `handOff`) o
`stayInWorld` agora vale para **todas** as camadas, laços também (o
`UpdateObjectTransform`; antes os laços seguiam a raiz, inferido).
**Continua inferido** o `stayInWorld` nos pulos da VM (Snap, contenção,
fim de escada): como lido, o jogo deixa o pedido que ainda está saindo
seguir o nó; aqui ele fica no mundo. Seguir o nó deixa o fim da subida da
escada reta 5,8 tiles adiante do topo por dois quadros (bateria de assento,
caso `bed`, `OS2_NO_STAY_IN_WORLD=1`), o que o jogo não mostra: falta achar
o que no jogo impede isso (candidatos: o `FinishEditing` de uma subclasse
de pessoa, o `ForceToEdithLocation`, vtable `0x150`, chamado de algum lugar
não achado). O Snap Into 0x04 não é feito (os objetos em slot são postos
no osso pelo app).

## E. Sentado e parado

Lido na rodada n02 (`docs/anim-mac.md` §2 e §6): o motor não tem idle
próprio; um pedido expirado sai da fila no mesmo tick e o osso sem canal fica
com o último valor. A regra "segura a pose" é a do jogo (e o `AnimPlayer` a
aplica a qualquer osso que nenhuma camada escreve).

## F. Animação de entrar e sair (STR# 0x92)

`ComputeReachAnimIndex` (`0x68ba50`) confirmado: faixa pela altura
(< 0,4 / 0,7 / 0,9) + 4 se a distância > 0,8 + 8 para pôr. No
`try_new_reach` (`0x69111c`):

- a **superfície** é o contêiner do objeto pego (modo 0) ou o alvo onde se
  põe (modo 1); no chão (modo 2) não há superfície;
- o ponto do alcance: o osso do slot (slot de osso), o deslocamento do slot,
  ou 0,7 à frente do Sim no chão; a **altura** é a desse ponto no quadro da
  raiz do Sim (metros) e a **distância** a horizontal até ele;
- com a flag 0x08 do byte 0x0D do pedido o índice vem do byte 0x0F (não
  calculado; não feito: o operando não foi casado com o nosso layout);
- a STR# 0x92 (laço de 5 casos em `0x692610`, caso 1): **a lista da
  superfície, depois a do semi-global dela, depois a global**. A lista do
  objeto pego não entra (dele vem só a animação do objeto, STR# 0x86).

**Mudado**: o `reachListAnimation` olhava primeiro a lista do objeto pego
(inferido); agora só a superfície, como o binário. **Continua inferido**: a
faixa pela classe de altura do SLOT (e não a altura em metros no quadro da
raiz) e a distância até o centro da superfície (e não até o ponto).

**Rodada n44 (Ghidra MCP; BV `0x69111c`, `0x68ba50`), lido inteiro:**

- O operando é o `NewReachParam` com os bytes do operando a partir de
  `+4`: flags no byte 9 (`+0xd`), modo no 10, slot no 6. **Com a flag 0x08
  o índice é o byte 15** (`0x692698`: `lbz 0xf` do operando); sem ela,
  `ComputeReachAnimIndex(altura, distância, pegar)` (`pegar` = modo 0).
  O índice é 0-based (a busca `FUN_000da590` recebe índice + 1).
- **O ponto do alcance**: modo 0 com contêiner, o slot do objeto no
  contêiner (o osso do slot se o descritor tem `0x20000`, senão o
  deslocamento do slot pelo global do contêiner); modo 0 sem contêiner, a
  posição do objeto; modo 1, o slot do alvo; modo 2 (chão), 0,7 à frente
  do Sim no andar dele. Esse ponto vai para o quadro do Sim (o inverso do
  `GlobalTransform` do nó dele, `FUN_0008a8d4`): **altura = z nesse
  quadro, distância = |(x, y)|**. Um Sim sentado tem o nó no osso do
  assento: a altura conta dali.
- **A superfície** é o contêiner do objeto (modo 0) ou o alvo (modo 1),
  pessoa ou não; a entrada vem da STR# 0x92 dela, senão do semi-global,
  senão da global (`0x7FD46CD0`); uma entrada **vazia** passa à lista
  seguinte (comprimento 0). Não há "longe vazio cai no perto" nem "." como
  vazio: o nome vai ao `DetermineAnimNameAndOEType` (`0x68d934`) e ao
  recurso pela idade (`AnimPrefix`), e sem recurso é o erro 0x71.
- **A bandeja e o copo do bar** (`a2o-barstool-seated-get/put`, as listas de
  2 entradas dos grupos `0x7F711E0D`, `0x7F67FE05`, `0x7F83CAAE`): pelo
  índice explícito: "Have a Drink - Grab Drink from Bar" (`0x7F711E0D
  0x100F`) pega com 0x08 e índice 0. A entrada 1 (pôr) não é usada por
  nenhuma árvore do jogo base (um índice calculado de pôr é ≥ 8, além da
  lista, e cai na global). Também pelo índice explícito: o xadrez ("Chess -
  Reach for Piece", índices 2, 3, 8, 9 para pegar e 8–11 para pôr: os
  cantos), os telefones (as entradas "." nunca são alcançadas; 18/19
  `c2o-Counter-get/put`), o livro do chão (17), a urna quebrada (16).

**Mudado**: o índice explícito, a altura e a distância do ponto do alcance
no quadro do Sim (o andar como 3 de altura e o terreno plano: inferido), a
superfície pessoa, sem o "longe vazio → perto" e sem o "." como vazio.

## G. IK

Lido e feito na rodada n02 (`docs/anim-mac.md` §4–5): o solucionador de
duas juntas do `cIKStrategy7DOF` (dobradiça em X, giro do braço pelo polo,
65 %/75 % da torção no bíceps e no pulso) em `applyIk`; a mistura dos alvos
pelo contato conferida.

## H. Levantar

**`cTSObject::TrySnap` (`0xe43624`)**, Snap para um slot de rota: o lugar é
**um ponto só**, `TotalRoutingSlotOffsetFromContainer` (`0xaf7d90`: o
**primeiro osso** do slot, mais o raio do Sim) girado pela direção do alvo;
direção = a do alvo + a do slot quando o slot "snapa a direção"
(`0xE4`; osso: `SlotDirectionFromContainer`; senão `GetSnapDirection`),
senão a do Sim. O teste (`0xe43084`): parede entre o lugar velho e o novo no
mesmo andar → erro 8; `TSObjectCanSetPositionAndOrientation`; passou →
`SetPosition` (`0xafd048`, que chama `RemoveFromWorld`: **od 2 = 0, od 3 =
−1, o slot antigo fica livre**) e `Orient`. Falha: od 52 = erro + 2 (ou 1
quando pediu a um Sim para sair, flag 0x02).

**Rodada n18 (`routing-mac.md` §9.1): feito.** O `snap` vai a um ponto só (o
primeiro osso; sem osso, o deslocamento do slot girado; "na frente", o tile
adiante) e faz os testes do binário na ordem (parede a 0,3 no tile do
ponto, lote e sala sem a RS `0x800`, parede no meio sem a `0x80000`,
pegadas e Sims a 0,283, sem exceção do alvo); a varredura e o anel saíram
(também das rotas: `OS2_BONE_RING=1`/`OS2_SNAP_SCAN=1` para comparar).
Inferido: a sala como a do tile (o binário pergunta ao gerente de
partições, `0x13c`) e o erro dela como 2; paredes com porta não contam aqui
(o binário as conta só pelas pontas). **A direção é a do binário**: só muda
com a flag 0x1000.

## I. Sustos e expressões sentado

Lido na rodada n02 (`docs/anim-mac.md` §6): o Animate Sim não olha o slot e a
escolha do nome só olha espécie, idade e sexo; o jogo toca o que a árvore
pede, a partir da raiz no slot. Nada a mudar no motor.

**Rodada n44: quem tira o Sim do slot.** Nenhuma primitiva nem o motor de
animação: as árvores. O "Function - Pre Route" da pessoa (`0x7F01EC29
0x2047`) faz o Sim contido se levantar antes de andar (o "Force - Stand - In
Place" `0x2D9` ou a função "stand" do contêiner pelo Run Functional Tree);
uma interação que não anda não passa por ali. O "Interaction - Be Scared"
das urnas (`0x7F38B8A7 0x201F`, empurrado pelo "Sub - Haunt Loop - Scare
Sim - Push Be Scared", `0x2060`) não anda nem chama o "stand": toca o susto
(Animate Sim) onde o Sim está. Não há flag do TTAB para "não sentado" (as
strings do editor, `0x7FE59FD0`, só têm "Sitting?" na pessoa). Então o jogo
também toca o susto em pé com o Sim contido na cadeira; a VM roda as mesmas
árvores. **Confirmado, sem mudança.**

## O que esta rodada mudou e o que sobrou

Feito (rodada n01, cada parte um commit, testes verdes):

1. **A**: objetivos de sentar nas varreduras com peso de sentar, os pesos
   no custo, a cadeira no parâmetro 0 da pós-rota, chegada na hora com o Sim
   já na cadeira do objetivo, e a cadeira dos slots de osso que pesam sentar
   (cadeirão). Teste `sit_goals` (N001_Lot49: TV → namoradeira;
   `docs/screenshots/i001-tv-sofa.jpg`, "Assistir" no Darren, sem janela). Autonomia de
   14400 ticks em N001_Lot11/68/49: igual (nenhum Sim escolheu a TV).
2. **B**: Snap Into com od 52 = erro + 2, slot inexistente = falso (não erro
   de VM), os testes do `TestPlaceInSlot` (ocupado, peso, cadeia, altura) e a
   direção do contêiner no `PlaceInSlot` (também no Reach/Put e no Snap para
   o slot 0). `test_routing`.
3. **H**: o Snap só muda a direção com slot que "snapa a direção" (0x1000).
   `test_reach`.
4. **F**: a lista 0x92 só da superfície.
5. **C** e **D**: confirmados sem mudança (o app prende ao osso, como o
   grafo de cena; os od 108–111 nunca são escritos).

Sobras (no `TODO.md`): ~~o Snap de um ponto só, a parede entre o lugar velho e o
novo (erro 8)~~ e ~~o +25 dos objetivos em pé~~ (feitos na rodada n18,
`routing-mac.md` §9.1), a árvore 0x34 "allow in slot",
a altura em metros do alcance e o índice explícito do Reach (flag 0x08). As
partes E, G e I foram feitas na rodada n02 (`docs/anim-mac.md`).
