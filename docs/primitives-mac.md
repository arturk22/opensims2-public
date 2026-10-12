# Primitivas de SimAntics: o que o binário de Mac confirma

Fonte: o executável da Aspyr para Mac, **The Sims 2 Bon Voyage 1.10
(v61383)**, universal i386 + ppc e **sem SafeDisc** (fora do repositório, em
`references/bv_mac/`). Usamos a fatia ppc, importada no projeto Ghidra
`~/sims2` como `/mac/sims2_bv.ppc`. Os nomes das funções vêm de
`tools/symxfer/` (ver o fim deste documento). Quando nada é dito, cada
afirmação abaixo é **lida no código decompilado** (marcada "binário"); o que
é dedução nossa aparece como "inferido". Onde o comportamento pode ser de
expansão, ele é marcado **"só BV"** (o opensims2 mira o jogo base de 2004).
Para o jogo base, o executável da 1.0 de Mac (`references/base_mac/
sims2_base_1.0.ppc`, Aspyr v2920, 2005) serve de conferência: está quase sem
símbolos, mas tem RTTI.

Convenção: "byte n" é o byte n dos 16 bytes de operandos da instrução (como
no resto do projeto e no SimPE); "u16 n" é um u16 little-endian que começa no
byte n; (dono, dado) é uma referência a variável, lida por
`cTSObject::InterpValue(dono, dado)`.
"Índice + 1 no STR#" quer dizer que o código soma 1 porque a API de STR#
do jogo conta a partir de 1: o operando é a entrada **a partir de 0**.

---

## 1. Como o jogo guarda e lê os operandos

- **`XPrimParam`** (16 bytes de operandos em `+4`, a "node version" em
  `+0x14`). `XPrimParam::Read` lê a versão de nó (só se o formato do BHAV é
  ≥ 0x8005; senão 0), 8 bytes e mais 8 (formatos < 0x8003 só têm 8: o resto
  fica 0). Binário.
- Cada primitiva tem uma subclasse (`SetToNextParam`, `NewReachParam`, ...),
  criada por `cTSBehaviorNode::GetNewPrimitiveByID(short)` (0x004885E0). O
  vtable tem 5 virtuais: `Write`, `Read`, `UpgradeVersion`, `GetVersion` e
  **`SwizzleParams`**. No Mac, `SwizzleParams` troca a ordem dos bytes dos
  campos de 16 e 32 bits (o arquivo é little-endian); `Write` chama ela antes
  e depois de gravar. Por isso **o corpo de `SwizzleParams` de cada classe dá
  exatamente quais bytes são u16/u32 e quais são u8** (tabela na §3).
  Binário.
- `UpgradeVersion` converte operandos de versão de nó antiga para a atual
  (só algumas classes: Relationship, Set to Next, Manage Inventory, Look At,
  Animate Overlay, Change Material, Budget, Dialog, Make Action String, Run
  Tree (chamada de árvore)). As versões antigas não aparecem nas árvores do
  jogo base que já olhamos, mas a VM precisa saber que o byte de versão de nó
  existe. Binário.
- Endereço útil: `cTSObject::InterpValue` é o slot `0x4D4` do vtable de
  `cTSObject`; os temps são lidos/escritos pelos slots `0x4C` (get temp n) e
  `0x50` (set temp n, valor) (inferido pelo uso: Test Object Type grava o GUID
  nos temps 0 e 1 por eles).

**Códigos de retorno** de um `Try<Primitiva>` (`cITSTreeSim::ReturnCode`):
0 = falso, 1 = verdadeiro, 2 = espera (rodar de novo no próximo tick),
3 = empilhou uma árvore (gosub). Binário (Sleep devolve 2 enquanto conta,
Run Functional Tree devolve 3 depois de `GosubObjectTree`).

**Erros**: a primitiva lança um `short` (código de erro). A tabela código →
texto está em `cTSObject::GetErrorString` (0x00B37720, um switch de 0x453
casos); a lista completa está no apêndice A. Exemplos: 0x05 "Primitive not
supported for this type of object", 0x08 "Stack number out of range", 0x15
"No stack object present when required", 0x17 "Could not find object
reference", 0x1B "Slot number out of range", 0x69 "Cant run Reach/Drop on
non Sim object".

## 2. Despacho: quem trata cada opcode

`cTSObject::TryElement` primeiro chama o agregado do objeto (objeto 3D) e
depois `cTSObject::OnTryElement` (0x00B07FB8). Se ninguém trata o opcode,
erro 0x05. Binário.

| opcode | primitiva (rótulo do editor) | handler no binário |
|---|---|---|
| 0x00 | Sleep | `cTSObject::TryIdle` |
| 0x01 | Generic Sims Call | `cTSObject::TryGenericSimCall` |
| 0x02 | Expression | `cTSObject::TryExpression` (o `nTSSG::cObject` antes só observa donos 3/4/0x12/0x13) |
| 0x03 | Find Best Interaction | `cTSPerson::TryFindBestAction` (só Sims) |
| 0x07 | Refresh | `cTSObject::TryUpdate` |
| 0x08 | Random Number | `cTSObject::TryRandom` |
| 0x0A | Tutorial | devolve verdadeiro sem fazer nada |
| 0x0B / 0x0C | Get Distance / Direction To | `TryDistanceTo` / `TryDirectionTo` |
| 0x0D | Push Interaction | `cTSObject::TryPushAction` |
| 0x0E | Find Best Object for Function | `TryFindFunctionalObject` |
| 0x0F | Break Point | `TryTreeBreak` |
| 0x10 | Find Location For | `TryFindGoodLocation` |
| 0x11 | Idle for Input | `cTSPerson::TryIdleForInput` (só Sims) |
| 0x12 | Remove Object Instance | `TryKillObject` |
| 0x13 | Make New Character | `nTSSG::cObject` (0x0065CB14, `TryMakeNewCharacter`) |
| 0x14 | Run Functional Tree | `TryCallFunctionalTree` |
| 0x16 | Turn Body Towards | `cTSRoutingObjectAggregate::TryTurnTowards` |
| 0x17 | Play / Stop Sound Event | `TryPlaySound` |
| 0x19 | Alter Budget | `TryBudget` |
| 0x1A | Relationship | `TryNewRelationship` |
| 0x1B | Go To Relative Position | `cTSRoutingObjectAggregate::TryGotoRelative` |
| 0x1C | Run Tree by Name | `TryCallNamedTree` |
| 0x1D | Set Motive Change | `cTSPerson::TrySetMotiveDelta` |
| 0x1E | Gosub Found Action | `cTSPerson::TryGosubAction` |
| 0x1F | Set to Next | `TrySetToNext` |
| 0x20 | Test Object Type | `TryTestObjectType` |
| 0x21 | Find 5 Worst Motives | `TryFind5WorstMotives` |
| 0x22 | UI Effect | `TryUIEffect` |
| 0x23 | Camera Control | `TryUserEvent` |
| 0x24 | Dialog | `TryDialog` |
| 0x25 | Test Sim Interacting With | `cTSPerson::TryTestInteractingWith` |
| 0x2A | Create New Object Instance | `TryCreateObject` |
| 0x2D | Go To Routing Slot | `cTSRoutingObjectAggregate::TryGotoRoutingSlot` |
| 0x2E | Snap | `TrySnap` |
| 0x30 | Stop ALL Sounds | `TryKillSounds` |
| 0x31 | Notify the Stack Object out of Idle | `TryNotifyStackObject` |
| 0x32 | Add/Change the Action String | `TryMakeActionString` |
| 0x33 | Manage Inventory | `TryInventoryAction` |
| 0x69–0x6C, 0x6E | Animate Object/Sim/Overlay/Stop, Look At | `nTSSG::cAnimatable::try_animate_obj/_sim/_overlay/_stop`, `try_look_at` |
| 0x6D | Change Material | `TryChangeMaterial` |
| 0x6F | Change Light | `nTSSG::cObject::TryChangeLight` |
| 0x70 | Effect Stop/Start | `TryChangeEffect` |
| 0x71 | Snap Into | `TrySnapInto` (o `cObject` antes atualiza o modelo) |
| 0x72 | Assign Locomotion Animations | `nTSSG::cLocomotable::try_assign_locomotion` |
| 0x73 | Debug | `TryDebug` |
| 0x74 | Reach/Put | `nTSSG::cAnimatable::try_new_reach` |
| 0x75 | Age | `nTSSG::cPerson::TryAge` |
| 0x76 | Array Operation | `TryArrayOperation` |
| 0x77 | Message | `TryMessage` |
| 0x78 | RayTrace | `nTSSG::cObject::TryRayTrace` |
| 0x79 | Change Outfit | `nTSSG::cObject::TryChangeOutfit` |
| 0x7A | On Timer | `TryOnTimer` |
| 0x7B | Cinematic | `TryCinematic` |
| 0x7C | Want Satisfy | `TryWantSatisfy` |
| 0x7D | Follow Sim (sem rótulo no editor do base) | `cTSRoutingObjectAggregate::TryFollowSim` — **só BV** |
| 0x7E | Run Script (Lua) | `TryRunScript` — **só BV** |

Sem handler (erro 0x05 se aparecer): 0x04–0x06, 0x09, 0x15, 0x18, 0x26–0x29,
0x2B, 0x2C, 0x2F, 0x34–0x68. Binário. `FollowSimParam` e `RunScriptParam` não
existem na RTTI do executável base 1.0 (as outras 62 classes de operando
existem), daí o "só BV". Binário (base e BV).

Nos opcodes 0x16, 0x1B, 0x2D (rotas) e 0x6A, 0x74 (animação/reach), se uma
flag do sistema de simulação (bit 0 do estado) está desligada, o handler não
roda: a primitiva devolve 2 (espera) e o objeto é marcado para tentar de
novo. Binário (o significado da flag é inferido: "o mundo 3D está pronto").

## 3. Largura dos campos de cada primitiva (`SwizzleParams`)

Bytes que não aparecem como u16/u32 são u8 (ou não usados). Binário (BV).

| opcode | classe | u16 (LE) nos bytes | u32 (LE) nos bytes |
|---|---|---|---|
| 0x00 | `IdleParam` | 0–1 | — |
| 0x01 | `GenericSimCallParam` | 0–1 | — |
| 0x02 | `ExpressionParam` | 0–1, 2–3 | — |
| 0x03 | `FindBestActionParam` | 0–1 | — |
| 0x07 | `UpdateParam` | 0–1, 2–3 | — |
| 0x08 | `RandomParam` | 0–1, 2–3, 4–5, 6–7 | — |
| 0x0A | `TutorialParam` | — | — |
| 0x0B | `DistanceToParam` | 0–1, 4–5 | — |
| 0x0C | `DirectionToParam` | 0–1, 2–3, 6–7 | — |
| 0x0D | `PushActionParam` | 6–7, 9–10, 12–13 | — |
| 0x0E | `FindFunctionalObjectParam` | 0–1, 4–5 | — |
| 0x0F | `TreeBreakParam` | 0–1, 2–3 | — |
| 0x10 | `FindGoodLocationParam` | 5–6, 8–9, 10–11, 12–13 | — |
| 0x11 | `IdleForInputParam` | 0–1, 2–3 | — |
| 0x12 | `KillObjectParam` | 0–1 | — |
| 0x13 | `MakeNewCharacterParam` | 4–5, 7–8 | 10–13 |
| 0x14 | `CallFunctionalTreeParam` | 0–1, 2–3 | — |
| 0x16 | `TurnTowardsParam` | 0–1, 3–4, 6–7 | — |
| 0x17 | `PlaySoundParam` | 0–1, 2–3, 7–8 | — |
| 0x19 | `BudgetParam` | 2–3, 4–5, 7–8 | — |
| 0x1A | `NewRelationshipParam` | 3–4, 6–7, 9–10 | — |
| 0x1B | `GotoRelativeParam` | 0–1, 4–5 | — |
| 0x1C | `CallNamedTreeParam` | 0–1, 2–3, 7–8, 10–11, 13–14 | — |
| 0x1D | `SetMotiveDeltaParam` | 4–5, 6–7 | — |
| 0x1E | `GosubActionParam` | 0–1 | — |
| 0x1F | `SetToNextParam` | 9–10, 11–12 | 0–3 |
| 0x20 | `TestObjectTypeParam` | 4–5 | 0–3 |
| 0x21 | `Find5WorstMotivesParam` | 4–5, 6–7 | — |
| 0x22 | `UIEffectParam` | 1–2, 3–4, 6–7, 8–9, 11–12 | — |
| 0x23 | `UserEventParam` | 0–1 | — |
| 0x24 | `DialogParam` | 13–14 | — |
| 0x25 | `TestInteractingWithParam` | 2–3 | — |
| 0x2A | `CreateObjectParam` | 7–8 | 0–3 |
| 0x2D | `GotoRoutingSlotParam` | 0–1, 2–3, 5–6, 7–8, 9–10 | — |
| 0x2E | `SnapParam` | 0–1, 2–3, 4–5, 6–7, 8–9 | — |
| 0x30 | `KillSoundsParam` | 0–1 | — |
| 0x31 | `NotifyStackObjectParam` | — | — |
| 0x32 | `MakeActionStringParam` | 0–1, 2–3, 12–13, 14–15 | 5–8 |
| 0x33 | `ManageInventoryParam` | 2–3, 11–12, 14–15 | 5–8 |
| 0x69 | `AnimateObjParam` | 0–1, 4–5, 7–8 | — |
| 0x6A | `AnimateSimParam` | 0–1, 4–5, 10–11 | — |
| 0x6B | `AnimateOverlayParam` | 0–1, 4–5, 7–8 | — |
| 0x6C | `AnimateStopParam` | 0–1, 4–5 | — |
| 0x6D | `MaterialChangeParam` | 0–1, 3–4, 6–7, 9–10, 11–12 | — |
| 0x6E | `LookAtParam` | 2–3, 6–7, 9–10, 12–13 | — |
| 0x6F | `ChangeLightParam` | 3–4, 5–6 | — |
| 0x70 | `EffectParam` | 2–3, 4–5, 7–8, 10–11, 13–14 | — |
| 0x71 | `SnapIntoParam` | 1–2, 4–5, 7–8 | — |
| 0x72 | `LocoParam` | 0–1 | — |
| 0x73 | `DebugParam` | 1–2, 4–5, 7–8, 10–11 | — |
| 0x74 | `NewReachParam` | 1–2, 4–5, 7–8, 11–12, 13–14 | — |
| 0x75 | `AgeParam` | — | — |
| 0x76 | `ArrayOperationParam` | 3–4, 6–7, 9–10 | — |
| 0x77 | `MessageParam` | 1–2, 6–7, 10–11, 13–14 | — |
| 0x78 | `RayTraceParam` | 2–3, 6–7, 9–10, 13–14 | — |
| 0x79 | `ChangeOutfitParam` | 2–3, 10–11 | 4–7 |
| 0x7A | `OnTimerParam` | 0–1, 3–4, 7–8, 10–11, 13–14 | — |
| 0x7B | `CinematicParam` | 0–1, 3–4, 7–8, 10–11 | — |
| 0x7C | `WantSatisfyParam` | 1–2, 8–9, 11–12 | 3–6 |
| 0x7D | `FollowSimParam` | 1–2, 3–4, 6–7 | — |
| 0x7E | `RunScriptParam` | 0–1, 2–3, 4–5, 7–8, 10–11, 13–14 | — |
| — | `AnimateOverlayVersion0Param` | 0–1, 4–5, 7–8, 10–11, 12–13 | — |
| — | `LookAtVersion0Param` | 2–3, 4–5, 6–7, 9–10, 12–13 | — |
| — | `NewRelationshipVersion0Param` | 6–7 | — |
| — | `RunPrivateTreeParam` | 1–2, 4–5, 7–8, 8–9, 10–11, 12–13, 14–15 | — |
| — | `UnusedParam` | 0–1, 2–3, 4–5, 6–7, 8–9, 10–11, 12–13, 14–15 | — |
| — | `XPrimParam` | 0–1, 2–3, 4–5, 6–7, 8–9, 10–11, 12–13, 14–15 | — |

Diferenças que pegam a VM hoje (ver as seções abaixo): Random Number tem os
**donos em u16** (dado u16 0, dono u16 2, dado u16 4, dono u16 6), como no
The Sims 1; Snap Into tem um u16 nos bytes 7–8; Reach/Put tem u16 em 1–2 e
7–8.

## 4. Primitivas em detalhe

### 0x74 Reach/Put (`try_new_reach`, `NewReachParam`)

Operandos (disassembler `CatNewReachParam` + `SwizzleParams`; binário):

| bytes | campo |
|---|---|
| 1–2 | u16 (não impresso pelo editor) |
| 3, 4–5 | o objeto: (dono u8, dado u16) — "Pick up Object in …"/"Drop onto Object in …" |
| 6 | número do slot (de contenção) do objeto, no modo 1 |
| 7–8 | u16 (não impresso) |
| 9 | flags: 0x01 slot no temp 0; 0x02 espera a mão (handedness) no temp 3; 0x04 filtra a animação do objeto pela idade do Sim |
| 10 | modo: 0 pegar o objeto, 1 pôr no slot do objeto, 2 pôr no chão |
| 11–12 | animação de pegar ("grasp"): **índice a partir de 0** no STR# 0x81 (adulto); 0xFFFF = nenhuma |
| 13–14 | animação do objeto: **índice a partir de 0** no STR# 0x86 (objeto); 0xFFFF = nenhuma |

Correção (r4): `cAnimatable::FindSpecificName` soma 1 ao índice porque a
API de STR# do jogo conta a partir de 1 (como os `STR#` do Mac); o operando
é a entrada a partir de 0, como os dados mostram (copo de brinde: grasp 0 =
`a2o-toastingGlass-grasp`).

Execução (binário): só roda em Sims (senão erro 0x69 "Cant run Reach/Drop
on non Sim object"; sem pessoa 3D 0x42, sem animador 0x41). Na primeira
execução zera o "reach object ID" e o "reach surface object ID" do Sim
(`cTSPerson::SetReachObjectID`/`SetReachSurfaceObjectID`) e guarda no stack
frame um ID novo de pedido de animação (`0xA0000000 | mão | contador <<
21`; a mão vem do temp 3 com a flag 0x02); enquanto o pedido existe, a
primitiva devolve 2 (espera). Modo 0 (pegar): objeto que não existe → os
dois IDs = 0xFFFF e **falso**; objeto que já está nessa mão do Sim → IDs =
0xFFFF e **verdadeiro** sem animar; senão reach object = o objeto e reach
surface = onde ele estava. Modos 1/2 (pôr): o objeto é o que está na mão
(`GetContainedObject(mão)`); modo 1 põe no slot (byte 6, ou temp 0 com
0x01) do objeto alvo, que vira o reach surface (slot fora do intervalo:
erro 0x1B); modo 2 põe no chão. A animação vem das tabelas dos bytes 11–14
(tipo de animação errado na string: erro 0x73; sem animação: 0x71; sem o
evento 0: 0x6E). No fim os dois IDs voltam a 0xFFFF. Inferido: a troca de
dono acontece no evento 0 da animação (por isso o erro 0x6E) e as árvores
leem os dois IDs pelos dados de pessoa.

### 0x14 Run Functional Tree (`TryCallFunctionalTree`)

Operandos: função u16 0 (índice + 1 no STR# 0xC9 "functions"); flags u16 2:
0x01 muda o ícone, 0x02 passa os parâmetros da árvore atual, 0x04 roda só a
check tree. Binário.

Execução (binário): o alvo é o **stack object** (sem stack object: erro
0x15). O índice da primitiva não é o índice da OBJf: é traduzido por uma
tabela fixa —

| u16 0 | 0–9 | 10 | 11 | 12 | 13 | 14 | 15 | 16 | 17–23 | 24 | 25 | 26 | 27 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| entrada da OBJf | 0x12–0x1B | 0x0E | 0x1C | 0x10 | 0x11 | 0x1D | 0x1F | 0x20 | 0x24–0x2A | (falso) | 0x2C | 0x2E | 0x35 |

(rótulos do STR# 0xC9: 0 "prepare food" … 9 "stand", 10 "serving surface",
11 "clean", 12 "garden", 13 "wash hands", 14 "repair", 15 "dry clothes", 16
"wash clothes", 17 "message handler", 18 "pre route", 19 "post route", 20
"goal check", 21 "reaction Handler", 22 "along route callback", 23
"awareness", 24 "reset" (sem entrada: sempre falso), 25 "lookatTarget", 26
"Utility State Change", 27 "Global Awareness")

— e pega a action tree dessa entrada (com 0x04, a check tree). Árvore 0 →
falso. Senão `GosubObjectTree(stack object, parâmetros, árvore, mudar ícone)`
e devolve 3 (a árvore roda empilhada, na thread de quem chamou, com o stack
object como "me"; inferido pelo nome). Se o gosub falha → falso.

### 0x1A Relationship (`TryNewRelationship`, `NewRelationshipParam`)

Operandos (versão de nó 1; binário): variável u8 0 (rótulo: STR# 0x102
índice + 1), flags u8 1, de (dono 2, dado u16 3), para (dono 5, dado u16
6), valor (dono 8, dado u16 9). Flags (disassembler): 0x01 "Fail if too
small", 0x02 "Use Neighbor IDs", 0x04 **"Get value from"** o valor (ou seja,
grava a variável), sem 0x04 **"Put value into"** o valor (lê a variável).

Execução (binário):

- 0x02: de/para são neighbor IDs; vizinho que não existe → erro 0x1C. A
  tabela é a do vizinho "de".
- sem 0x02 e **com 0x08**: só o "de" é procurado como objeto (erro 0x17 se
  não existe) e a tabela é a do próprio objeto (`+0x1AC`); o "para" é usado
  cru, sem procurar objeto nenhum (confirma o achado §7 do
  `data-findings.md`: "para" é um neighbor ID). A flag não aparece no
  disassembler.
- sem 0x02 e sem 0x08: os dois são objetos (erro 0x17 se faltar um). **Se os
  dois são Sims, a relação é guardada pelos neighbor IDs deles** (a mesma
  tabela que 0x02 usa); senão, na tabela do objeto "de", com o ID de objeto
  do "para" como chave.
- Sem tabela → erro 0x68 "The relationship matrix requested could not be
  found".
- Se a relação com "para" tem ≤ `variável` entradas: com 0x01 → falso; sem
  0x01 ela cresce para `variável + 1` entradas (zeradas) e segue.
- Ler (sem 0x04): o destino tem de ser gravável (erro 0x13) e o valor tem de
  caber no intervalo do destino (erro 0x14); verdadeiro. Gravar (0x04):
  verdadeiro, ou erro 0x14 se a tabela recusa.

Versão de nó 0 (`NewRelationshipVersion0Param`, convertida no load): byte 1
era um modo (0 eu→stack object, 1 stack object→eu, 2 stack object→local,
3 local→stack object) e com o bit 0x02 os IDs viravam person data 31
(neighbor ID) em vez de object data 11 (ID do objeto). Binário.

**Contradiz o código**: a VM falha com 0x08 e não trata "dois Sims por ID de
objeto = mesma relação que por neighbor ID".

### 0x2A Create New Object Instance (`TryCreateObject`)

Operandos (binário): GUID u32 0; posição s8 4 (STR# 0xA7 índice + 1); flags
u8 5; local u8 6 (posições 8 e 9); u16 7 (não impresso); slot u8 9
(posições 4 e 10); flags u8 10.

Flags do byte 5 (disassembler + código): 0x01 não duplica (falso se já há
um objeto do mesmo tipo no tile), 0x02 passa IDs ao Main (parâmetro 0 = meu
ID, 1 = ID do stack object), 0x04 Sim a partir do **vizinho no stack
object**, 0x08 falso se o tile não está vazio, 0x10 passa o temp 0 ao Main
(parâmetro 0), **0x20 (sem texto no editor) põe o objeto novo no mesmo nível
(andar) do stack object**, **0x40 GUID do token temporário de inventário**,
0x80 GUID nos temps 0 (baixo) e 1 (alto). 0x02 junto com 0x10 → erro 0x36
"More than one passing value specified". Flags do byte 10: 0x01 "Moving in a
new Sim" (vai para a criação), 0x02 copia os materiais de design do objeto
no temp 5 (objeto inexistente → erro 0x0A).

Posições (byte 4; rótulos do STR# 0xA7 e código; binário): 0 "in front of
me" (meu tile + 1 na direção em que olho); 1 "on top of me" e 2 "in my hand"
(os dois: no meu slot 0); 3 "in front of stack object"; 4 "in user defined
slot of stack object" (slot do byte 9); 5 "underneath me" (meu tile); 6 "out
of world" (sem testes de posição); 7 "below object in stack param 0" (no tile
do objeto cujo ID está no parâmetro 0; sem parâmetros → erro 0x08); 8 "below
object in local" (local do byte 6; fora do intervalo → erro 0x08); 9 "next to
me in direction of local" (local + minha direção, só N/L/S/O, virado para
mim); 10 "in user defined slot of object in temp 0" (slot do byte 9).

Falso (sem erro): GUID sem definição; definição "morta" (tipo 0x11); posição
inválida ou tile ocupado (flags acima); a criação falha; o objeto não cabe
(`TSObjectCanSetPosition`/`CanPlaceInSlot`: o objeto criado é apagado).
Definição de template (tipo 0x10) → erro 0x90. No fim, **o stack object
passa a ser o objeto novo** e devolve verdadeiro.

### 0x20 Test Object Type (`TryTestObjectType`)

Operandos (binário): GUID u32 0, objeto (dono u8 6, dado u16 4), flags u8 7.

- GUID `0x4C7CAB2B` = "the GUID of the temporary inventory token"
  (disassembler): o GUID testado é o do token temporário.
- 0x04 "Return GUID selected in Temp0/1": não testa objeto; se o GUID tem
  definição, temp 1 = metade alta, temp 0 = metade baixa e verdadeiro; senão
  falso.
- 0x02 "incoming ID is a neighbor ID": o objeto é procurado como vizinho.
- 0x01 "Checking against original, not current GUID".
- Sem 0x04: verdadeiro se o GUID é o da definição do objeto (com 0x01, da
  definição original) ou, se não for, o da definição "mestre" dela (slot
  `0x6C`; inferido: objetos multi-tile e trocas de tipo).
- **Objeto que não existe → falso (não erro).** Contradiz o comentário em
  `src/vm/primitives.cpp` ("error if it does not exist").

### 0x10 Find Location For (`TryFindGoodLocation`)

Operandos (binário): modo u8 0 (STR# 0xEF); local u8 1; flags u8 2; byte 3 =
**máscara de direções proibidas**; objeto (dono 4, dado u16 5); referência
(dono 7, dado u16 8); routing slot u16 10; u16 12 (não usado aqui).

Flags (disassembler): 0x01 começa no objeto do local (byte 1), 0x02 **não**
prefere tiles vazios (o texto ", prefer empty" sai quando o bit está
desligado), 0x04 só tiles editáveis, 0x08 objetos explícitos (bytes 4–9),
0x10 em terreno plano, 0x20 com borda vazia, 0x40 começa na frente da
referência, 0x80 com linha de visão até o centro.

Byte 3 (binário): o código usa `~byte3` como máscara das direções
permitidas: **bit n ligado = direção n proibida** (bit 0 N, 1 NE, 2 L, 3 SE,
4 S, 5 SO, 6 O, 7 NO). `0xFE` = só N, `0xAA` = só N/L/S/O, `0xFB` = todas menos
L, 0 = todas. O §11 do `data-findings.md` tinha a leitura invertida.

Execução (binário): sem 0x08 o objeto é o stack object (sem: erro 0x15) e a
referência sou eu, ou o objeto do local (0x01; local fora do intervalo:
0x33). Com 0x08 os dois vêm dos operandos (faltando: 0x17). Modo 1: põe fora
do mundo, verdadeiro. Modos 5–8 (slot no parâmetro / local / literal /
global, u16 10; fora do intervalo: 0x08 e 0x1B): tenta cada posição do slot;
se o slot não fixa a direção, tenta as 8 direções a partir de uma aleatória,
pulando as proibidas. Modos 3/4 (ao longo do vetor da referência / lateral
= +90°; sem referência: 0x3D): busca a partir da direção da referência.
Modos 0 e 2: busca normal; **o modo 0 tenta primeiro no mesmo cômodo e, se
não achar, em qualquer lugar; o modo 2 só no mesmo cômodo**. Achou →
posiciona, verdadeiro; não achou → falso.

### 0x1F Set to Next (`TrySetToNext`, `SetToNextParam`)

Operandos (binário): GUID u32 0; tipo de busca = byte 4 & 0x7F (STR# 0xA4);
**byte 4 bit 7: alvo explícito** = (dono byte 5, dado **byte 7**) — sem ele o
alvo é o stack object; local u8 6 (tipos 9 e 0x22); flags u8 8: 0x01 "including
disabled objects", 0x02 "where" object data (u16 9, rótulo STR# 0x8D) ==
valor u16 11. A versão de nó 0 não tinha as flags nem o filtro (zerados no
load).

Execução (binário):

- O alvo tem de ser gravável (erro 0x13). A busca recomeça **depois** do
  valor atual do alvo (0 = do começo) e grava o achado no alvo; nada achado →
  falso.
- Filtro comum (`SetToNextIsObjectEligible`): objeto válido; sem a flag 0x01,
  pula objetos com o bit 0 do object data 56 ligado; **sem a flag 0x04 (não
  impressa pelo editor), pula objetos com object data 107 ≠ 0**; com 0x02,
  object data [u16 9] tem de ser igual a u16 11. Os rótulos do jogo base
  (STR# 0x8D) só vão até o object data 87 e chamam o 56 de "dry clothes
  value": a flag 0x01 ("including disabled objects", string ausente no exe
  base 1.0) e o campo 107 são **só BV**.
- Lista de objetos do lote: 0 objeto; 2 não-pessoa; 6 categoria (object data
  59) = parâmetro 0; 12 categoria = temp 0; 25 objeto no cômodo do temp 0; 27
  "Utility Using Object" (object data 76 ou 77 ≠ 0); 33 "Hauntable" (campo
  0x1C da definição); 37 categoria de desejo = temp 0 (os rótulos do jogo base
  vão até o tipo 37). Tipo fora da lista → erro 0x1E.
- 1 pessoa: lista de pessoas do lote, a partir do Sim cujo ID está no alvo.
  Os tipos 0x28–0x2D (só BV) filtram pelo dado de pessoa 186 (espécie?:
  0x29 < 0, 0x2A 1–2, 0x2B 1, 0x2C 2, 0x2D 3), e **no BV o tipo 1 só aceita
  dado 186 = 0** (o jogo base não tem esse campo: 170 rótulos de dados de
  pessoa).
- 3 parte de multi-tile: a próxima parte do grupo do objeto do alvo.
- 4 objeto do tipo (GUID u32 0) e 18 GUID nos temps 0/1: definição pelo GUID
  (sem definição → falso), instâncias depois da atual.
- 5 neighbor ID (e 0x2E–0x33, só BV): o próximo vizinho pelo ID, com o
  mesmo filtro do dado 186 (tipo 5: = 0, só BV); se o vizinho está no lote, o
  objeto dele também passa pelo filtro comum.
- 7 vizinho do tipo: próximos vizinhos cuja definição tem o GUID u32 0.
- 8 objeto no mesmo tile: precisa de alvo ≠ 0 (senão falso); objetos do tile
  do alvo, no mesmo nível, depois dele.
- 9 objeto adjacente ao objeto do local (byte 6; fora do intervalo: erro
  0x08; objeto inexistente: 0x0A): o último objeto válido dos tiles +X, −X,
  +Y, −Y que esteja no mesmo cômodo.
- 10 carreira: sempre falso. 11 Lot ID: o próximo lote.
- 13 pessoa mais perto da pessoa do temp 0: grava a distância e a direção
  relativa no objeto (lidas pelos temps 4 e 5, pelo rótulo).
- 14/15 instância do "job object" pelo GUID nos dados de pessoa 89/90 do Sim
  no stack object / do vizinho (vizinho inexistente: erro 0x37).
- 16 veículo: a lista de veículos do lote (o GUID não é usado — confirma o
  que o `hood_bridge` já faz).
- 17 vizinho da família do temp 0 (dado de pessoa 61 = temp 0).
- 26 "Verify Stack Object", 29 "Verify Neighbor ID in Stack Object", 30
  "Verify Sim in Stack Object": só testam o stack object (o objeto existe e é
  válido / o vizinho existe / o objeto é um Sim); não mexem no alvo.
- 28, 35, 36: vizinhos depois do valor atual cuja relação com o vizinho do
  **temp 0** passa num teste (a tabela é a do vizinho do temp 0): 28 a
  variável 3 ≠ 0; 35 a variável 0 (diária) > **temp 1** (o rótulo diz "temp
  0 to me", o código usa temp 0 = de quem e temp 1 = limite); **36 a variável
  1 & temp 1 ≠ 0** (bits de relação, `docs/data-findings.md` §8). Vizinho do
  temp 0 inexistente ou sem tabela → falso.
- 38 e 39 (sem rótulo no jogo base; só BV): 38 percorre uma lista por índice
  comparando dois campos com os temps 0 e 1 (< 0 = qualquer) e devolve o GUID
  do achado nos temps 2 (baixo) e 3 (alto).

### 0x7A On Timer (`TryOnTimer`, `OnTimerParam`)

Operandos (binário): u16 0 = ticks até disparar ("Ticks till fire %d" no
editor; com a flag 0x08 vem do temp 1: "Getting ticks till fire from temp
1"); byte 2; u16 3 = árvore que o timer roda; flags u8 5;
três (dono 6, dado u16 7), (dono 9, dado u16 10), (dono 12, dado u16 13);
modo u8 15.

Execução (binário): pega o gerenciador de timers do lote (sem ele: erro
0x17). Ticks = u16 0, ou o temp 1 com a flag 0x08. Modo (byte 15) 0 (criar): argumentos =
os três (dono, dado) acima, ou, com a flag 0x01, os parâmetros da árvore
atual; chama o gerenciador com (u16 0, u16 3, flag 0x02, persistente?, n
argumentos) e **grava no temp 0 o ID do timer criado**. Modo 1: altera o
timer cujo ID está no temp 0 (ticks, árvore, flags 0x02 | 0x04→2; 0x04 =
"Reset Timer Ticks" pelo texto do editor). Modo 2: cancela o timer do temp
0 ("Start/Modify/Delete Timer" no editor). Sempre verdadeiro. Inferido:
0x02 repete. (Correção r4: a versão anterior trocava ticks e árvore.)

### 0x17 Play / Stop Sound Event (`TryPlaySound`)

Operandos (binário): som s16 0; flags byte 4; (dono 6, dado u16 7). O
binário de Mac não tem `CatPlaySoundParam` (o texto do editor não foi
achado).

Execução (binário): a fonte é o stack object com a flag 0x02 (e byte 4 ≠
0xFF), senão eu. O número escolhe a tabela de sons: |n| ≥ 20000 → tabela
**semi-global** (n − 20000), 10000–19999 → **global** (n − 10000), < 10000 →
privada. (Correção r4: o binário só mostra três slots da vtable do dono do
código da árvore — 0x38 para 20000+, 0x34 para 10000+, 0x3C para o resto —,
e a versão anterior chamou 0x38 de global; os dados decidem: "Pregnancy -
Make Pregnant", árvore global, toca 10002 = o som global
`sting_playin_make_preg`, e os telefones tocam 20001 sem tabela própria.) **n negativo: o índice vem da variável (dono 6, dado u16 7)** (a
tabela continua escolhida por |n|). O nome achado vai em minúsculas para o
sistema de áudio numa mensagem com o ID do objeto, a posição (para Sims, o
osso `root_rot` e os dados de pessoa 58, 65 e 88) e um de dois eventos
conforme a flag 0x40 (inferido: tocar em laço / uma vez). Object data 34
(flags de som): bit 0x02, ou 0x10 com nome começando por "vox", não manda a
posição. **Sempre verdadeiro** (o fim da função é `li r3,1`).

### 0x11 Idle for Input (`cTSPerson::TryIdleForInput`)

Operandos (disassembler + `SwizzleParams`; binário): parâmetro u16 0 com os
ticks ("for ticks in" parâmetro n); "allow push" u16 2 (≠ 0 = ", allow
push"); byte 4 bit 0 = **"Handle Sub Queue Interactions"** (nesse modo os
outros campos não são impressos). Confirma o §5 do `data-findings.md`.

Execução (binário; `cTSPerson::ShouldInterrupt` = "há algo para fazer"):

- Modo normal. Parâmetro fora do intervalo → erro 0x08. Sem nada pendente:
  contador ≠ 0 → decrementa e devolve 2; contador = 0 → desliga a flag 6
  ("notified by idle for input", bit 0x40 do object data 8) e devolve
  verdadeiro. **Espera exatamente N ticks** (a VM, como a FreeSO, espera N + 1:
  ela só devolve verdadeiro quando o contador fica negativo).
- Com algo pendente: liga a flag 6. Se "allow push" ≠ 0 e a fila tem
  entradas: dentro de uma interação de prioridade alta (o frame diz 1) a
  interação da frente só entra se puder interromper; senão verdadeiro. A
  entrada da frente sai da fila, as sub-ações são limpas, a check tree roda
  (`cEdithObjectTestSim::TestInteraction`); passou e tem action tree →
  `GosubObjectTree` no objeto dela, ela vira a ação atual, o contador vai a
  0 e a primitiva devolve **3** (a action tree roda em cima desta árvore);
  não passou → `ActionSkipped` e devolve 2. Sem "allow push" ou fila vazia →
  verdadeiro.
- Modo "Handle Sub Queue Interactions" (byte 4 bit 0): se há sub-ações
  (`CountSubActions`), tira a primeira, testa e empilha a action tree dela
  como sub-ação atual (devolve 3); falhou → limpa as sub-ações e verdadeiro.
  Sem sub-ações → verdadeiro.

### 0x0D Push Interaction (`TryPushAction`, `PushActionParam`)

Operandos (disassembler `CatPushActionParam`; binário):

| bytes | campo |
|---|---|
| 0 | número da interação (TTA) |
| 1 | onde está o objeto: parâmetro n, ou local n com a flag 0x02 |
| 2 | prioridade (STR# 0xE0) |
| 3 | flags: 0x01 ícone do objeto no local (byte 4); 0x02 objeto num local; 0x04 "continue as current"; 0x08 "use name"; 0x10 número da interação numa variável (dono 5, dado u16 6); 0x20 "Force run check tree"; 0x40 liga à interação cujo ID está em (dono 8, dado u16 9); 0x80 devolve o ID da interação nova em (dono 11, dado u16 12) |
| 4 | local com o objeto do ícone (flag 0x01) |
| 5, 6–7 | (dono, dado) do número da interação (flag 0x10) |
| 8, 9–10 | (dono, dado) do ID da interação a que liga (flag 0x40) |
| 11, 12–13 | (dono, dado) onde gravar o ID da interação nova (flag 0x80) |
| 14 | flags: 0x01 passa os 4 primeiros parâmetros; 0x02 interação = resultado do último Find Best Interaction; 0x04 ícone pelo GUID do seletor nos temps 4/5; 0x08 índice do ícone no temp 6 |
| 15 | índice do ícone (sem 0x08 no byte 14) |

A interação vai para a fila do **stack object** ("onto the stack object's
queue"). Resolve o §12 do `data-findings.md` (os três pares e os bits).

Execução (binário):

- Começa gravando 0 no meu object data 52 ("primitive result"); as falhas
  "esperadas" gravam ali um código e devolvem falso: 1 prioridade herdada
  mas eu não tenho prioridade (dado de pessoa 33 = 0), 2 a check tree
  recusou, 3 fila do alvo cheia (mais de 6 entradas e não deu para trocar),
  4 sub-fila cheia (interação aninhada, mais de 4), 6 o alvo recusou a
  entrada.
- O alvo (stack object) tem de existir (erro 0x15) e ser um Sim (erro 0x1C).
- Objeto: parâmetro do byte 1 (fora do intervalo: 0x08) ou local (0x02;
  0x33); com o byte 14 bit 1 (último Find Best Interaction) o objeto é o dado
  de pessoa 53 do alvo ("FindBestAction result Interaction Object ID") e a
  interação o object data 20 dele ("tree table entry"). Objeto inexistente →
  0x17; sem definição → falso; sem TTAB → 0x1F; entrada inexistente → 0x16.
- Prioridade (byte 2): 0 herdada = meu dado de pessoa 33 (objeto que não é
  Sim: 1), 1 máxima = 100, 2 autônoma = 20, 3 do usuário = 50; outro valor →
  erro 0x2D.
- Interação aninhada sem o alvo estar numa interação que permite → erro 0x6F.
- Flag 0x80: grava o ID da interação nova (fora do intervalo do destino:
  0x14). Flag 0x40: liga a nova à interação de ID (dono 8, dado u16 9).
  Byte 14 bit 0: copia os 4 primeiros parâmetros da árvore atual (ou menos).
- Se eu sou Sim (e não é o caso do Find Best Interaction), a interação herda
  algo da minha interação atual (slot 0xBC; inferido: o "dono"/grupo).
- Verdadeiro quando a entrada vai para a fila (ou sub-fila) do alvo.

### 0x00 Sleep (`cTSObject::TryIdle`)

Parâmetro u16 0 com os ticks (fora do intervalo: erro 0x08). Se o valor é 0
→ verdadeiro; senão decrementa o parâmetro e devolve 2 (espera). Valor
negativo vira 0. Binário.

### 0x03 Find Best Interaction (`cTSPerson::TryFindBestAction`)

Operandos: u16 0 = **máscara de motivos** (bit = número do motivo:
"Interaction - Fulfill Hunger" passa 0x0080, "Fulfill Bladder" 0x0200; o
`TryFindBestAction` guarda o u16 como máscara e o passa à busca de objetos
por anúncio; ver `docs/data-findings.md` §16) — 0 = os N piores motivos,
escolhidos pelo valor (rótulos dos motivos no disassembler: 0 "Shopping", 1 "Scratch-Chew" (só
BV), 3 humor, 4 meta-humor, 5 energia, 6 conforto, 7 fome, 8 higiene, 9
bexiga, 11 alerta, 13 ambiente, 14 social, 15 diversão); flags byte 3: 0x01
depois da primeira passada escolhe os motivos restantes entre os piores,
0x02 só objetos no cômodo do temp 0, 0x04 inclui objetos fora do mundo,
0x08 só interações aninhadas, 0x10 só no objeto da interação atual, 0x20 usa
a posição do objeto do temp 1. Execução (binário): `FBA_Init`, depois até
200 passadas de `FBA_FindStaticInteractions` → `FBA_ScoreStaticInteractions`
→ `FBA_ScoreDynamicInteractions` → `FBA_CheckInteractions`, e
`FBA_DoInteraction` decide o retorno (essas funções têm nome no mapa e são o
ponto de partida para o motor de autonomia). As curvas de motivo vêm de
`cTSPerson::LoadMotiveEffects` (um `cTSMotiveCurveSet` por Sim, pelo campo
0x33 da definição ou pela idade; o dado de pessoa 84 bit 0x200 escolhe a
tabela 0x298).
A pontuação (`FBA_ScoreInteraction`, `GetInteractionScore`,
`GetAdScoreForHuman`, `CalcAttenuation`), a disponibilidade por idade
(`GetAvailabilityBitset`, flags u32 do TTAB) e as constantes da FCNS 2
estão no `docs/data-findings.md` §16 e em `src/vm/autonomy.*`.

#### O laço completo do Find Best Interaction (binário, rodada 7)

Funções do BV: `TryFindBestAction` `0xae36cc`, `FBA_Init` `0xae0cf0`,
`FBA_FindStaticInteractions` `0xad4130`, `FBA_ScoreStaticInteractions`
`0xae337c`, `FBA_ScoreDynamicInteractions` `0xae3258`, `FBA_ScoreInteraction`
`0xae2c4c`, `FBA_RunCheckTree` `0xae24e4`, `FBA_CheckInteractions`
`0xae2978`, `FBA_DoInteraction` `0xad6658`, constantes em
`cTSAutonomyConstantsClient::UpdateConstants` `0xac8f80` (FCNS 2 global; o
**padrão do binário** vale quando a chave falta).

```
máscara = u16 0 (0 → piores motivos)
usados = 0; repetir:
    se máscara de entrada = 0: máscara = os N piores motivos fora de "usados"
        (FUN_00e1de28; N = "random worst motive count": FCNS 8, padrão 3)
    usados |= máscara;  com flag 0x01 a máscara de entrada só vale na 1ª passada
    se máscara ≠ 0: FindStatic → ScoreStatic → ScoreDynamic → CheckInteractions
continua enquanto (checks < máx e máscara ≠ 0 e veio dos piores)
          ou (ainda não há candidato pontuado e máscara ≠ 0 e veio dos piores)
FBA_DoInteraction
```

- **O limite de 200** ("max check tree in fba count", FCNS 200, padrão 200) é
  o contador `+0x85c`. Ele soma 1 a **cada check tree rodada**
  (`FBA_RunCheckTree`), tanto na pontuação quanto na escolha. Por ele o laço
  para de abrir novos grupos de motivos (mas continua enquanto nada pontuou).
  Dentro de `CheckInteractions`, ele interrompe a varredura quando já passou
  de 200, não é a primeira passada e já há um escolhido.
- **ScoreStatic**: para cada candidato do grupo, pula entradas do próprio
  Sim sem "allow on self" (vtable `0x54` da entrada), aplica os modos
  0x08/0x10 do operando, testa a disponibilidade (idade, vtable `0x128`) e
  conta (`+0x860`). Se o **BHAV da check tree tem as flags 2 ou 4**
  (`FUN_00acdbdc(…, 6)`, cache por árvore), a check tree roda **antes** de
  pontuar (ela pode mudar os anúncios, donos 0x0C/0x1C/0x1D) e só pontua se
  passar. Assim gasta o limite. Senão pontua direto com os anúncios da tabela.
- **ScoreDynamic**: as interações dinâmicas (os anúncios que a check tree
  escreveu, `+0x894`/`+0x8a0`) são pontuadas com a entrada da tabela
  ajustada.
- **Pontuação** (`FBA_ScoreInteraction`):
  ```
  Δ  = GetInteractionScore(entrada) − GetCurrentScore()          (motivos, ver acima)
  d  = distância até o objeto (vtable 0x1f8, com "autonomy floor to floor distance cost", padrão 10)
  at = CalcAttenuation = 1 / (1 + a·d), a = atenuação da interação (vtable 0x30:
       baixa/média/alta da TTAB → FCNS 0,1/0,3/0,6; visitante 0,01/0,02/0,03;
       com "autonomy attenuation formula" ≠ 0: min(1, tuning / (a·d + 1)))
  s  = at·Δ
  s  = s·(1 + posse) · memória · relacionamento · clima · viés (vtable 0x734)
  descarta se s < "min autonomy score" (família, ou visitante se vtable 0x5e0) ou s < 1e-5
  ```
  Padrões do binário: mínimo família 0,2, visitante 0,05. A FCNS dá 0.
- **Escolha** (`CheckInteractions`): ordena por pontuação decrescente
  (`qsort`, 0x44 bytes por item). Se o Sim não é visitante e o primeiro não está
  marcado, sorteia r em [0, min(n, "random selection count")) (FCNS 1 → sempre
  0; padrão 10). Depois anda na lista: os itens cuja check tree ainda não
  rodou (`+0x3c` = 0) rodam agora, e o **r-ésimo que passa** vira o escolhido
  (`+0x890`).
- **Por que a comida perde** (leitura do binário, para conferir no nosso
  motor):
  1. o primeiro grupo é só dos **N piores motivos** (3 por padrão, 8 na
     FCNS). Se a fome não está entre eles, a geladeira nem é candidata até
     um grupo seguinte, e os grupos seguintes só abrem enquanto o limite de
     200 check trees não estourou (num lote cheio, as check trees de pré-anúncio do
     primeiro grupo gastam o orçamento);
  2. Δ é a diferença na **curva de utilidade**, achatada perto do topo: com
     fome alta (> ~0), o ganho é pequeno, e a geladeira anuncia fome 0 + 5..20
     (×0,001 no anúncio, então pouco);
  3. a atenuação: a geladeira longe ou noutro andar (+10 por andar) cai por
     1/(1 + a·d);
  4. uma check tree que falha (ex.: sem comida, geladeira quebrada) descarta
     a entrada mesmo com a melhor pontuação.
- **Decaimento diário**: nenhuma função C++ lida aplica os "Decay … per Day"
  (93–101). `cTSPerson::OnReset` (BV `0xad39f0`) só põe **padrões quando todos
  são 0**: 93 fome −400, 95 bexiga −200, 96 energia −200, 97 higiene −100,
  101 diversão −100, 98 −50, 99 social −100, 94 conforto −30, 100 −30, 106
  −30 (106 só BV). Como antes, a aplicação segue inferida no motor.
  `cTSPerson::DayPassed` (BV `0xad133c`) só decrementa o dado 200 (até −10,
  só BV), chama `cTSObject::DayPassed` (a depreciação, ver
  `docs/buy-build-mac.md` §2) e `UpdateMemories`.

#### Sociais esperando o outro Sim ("Wait - Person B State", dados, rodada 7)

Não há código C++ para isto. É SimAntics no grupo `0x7FD90EDB` (`objects.package`),
lido instrução a instrução:

- As duas metades de uma social compartilham um **objeto de controle** (o
  stack object das duas threads): atributo 0 = ID do Sim A, 1 = ID do Sim B,
  **6 = estado de A, 7 = estado de B**, 8 = flag de cancelamento, 14/15 =
  "A/B esperando". "Get in Temp 0 - Who Am I?" (`0x2036`) compara os
  atributos 0/1 com o meu object data 11 (A = BCON 8960, B = 8961).
  Nenhum dos dois → **Break Point** (é o erro que a VM vê quando o
  stack object não é o objeto de controle).
- "Test - Person State" (`0x203F`): lê o atributo 6 ou 7 do outro e compara
  com o parâmetro 2 (`==` se param 1 = BCON 9216, `>=` se 9217).
- "Wait - Person X State Test Y" (`0x203E`) é um **laço de espera**: testa o
  estado do outro; se ainda não chegou, liga o "esperando" (14/15), roda
  "Idle System - Start", e a cada volta chama o global **"Idle" (`0x118`)
  com 1 tick** (que cede a thread), confere a flag de cancelamento (atributo 8)
  e o dado de pessoa **110 "Interactions Linked Deleted"** (o outro largou a
  interação ligada → sai com falso), processa comandos do controle
  ("Command - Process My Command", `0x2015`) e roda "Run Tree by Name"
  (0x1C) nos ganchos de espera. Ao sair, desliga o "esperando" e chama "Idle
  System - Stop".
- "Wait - Person B State - ==" (`0x2010`) chama essa espera, ou a versão "+
  Route" (`0x2067`) quando "Social - Dynamic Walk Enabled?" e o param 2 = 1.

Ou seja, um **alvo ocupado** só "responde" quando a metade B da social
começa a rodar na fila dele (Push Interaction, com a prioridade de quem pediu)
e escreve o estado no atributo 7. Até lá, A gira 1 tick por volta. O motor só
precisa de: Idle/Sleep cedendo a thread, Push Interaction na fila do outro,
os atributos do objeto de controle e o dado de pessoa 110 ligado quando a
interação ligada é apagada (ver Push Interaction e a fila). Se a metade B nunca
começa e ninguém liga o 110, A espera para sempre. É o "as sociais esperam
pelo outro Sim" do TODO de autonomia.

### 0x0E Find Best Object for Function (`TryFindFunctionalObject`)

Função u16 0 (STR# 0xC9) → (object data com o "valor" da função, entrada da
OBJf), binário: 0 prep (31, 0x12), 1 cook (32, 0x13), 2 flat surface (33,
0x14), 3 dispose (36, 0x15), 6 wash dish (37, 0x18), 7 eating surface (38,
0x19), 8 sit (26 "Sitting Value", 0x1A), 10 serving surface (61, 0x0E), 11
clean (39 "dirty level", 0x1C), 12 garden (64, 0x10), 13 wash hands (65,
0x11), 14 repair (15 "Repair Level", 0x1D), 15 dry clothes (sem valor: 50,
0x1F), 23 awareness (sem valor, 0x2A), 25 lookatTarget (sem valor, 0x2C), 27
Global Awareness (0x35, só BV); **as outras funções (4 eat, 5 pick up, 9
stand, 16–22, 24, 26) dão falso direto**.

**O laço inteiro (rodada 11, BV `0xe35a94`, binário).** Antes do laço:

- referência = eu, ou com a flag 0x08 o objeto (dono byte 3, dado u16 4)
  (inexistente: erro 0x17); o cômodo da referência só é lido com 0x04/0x20;
- com 0x10, o serviço de linha de visão do mundo (`WorldDB→0x94→0xA0(5)`) e
  a posição da referência;
- se eu sou um Sim (interface `cITSPerson`, IID `0x5a69d3d5`): o conjunto
  dos **cômodos acessíveis** (`cTSPerson::GetAccessibleRooms(conjunto, meu
  cômodo)`, BV `0xacd218`: o roteador preenche, a partir do meu cômodo) —
  não na Global Awareness — e `UpdateAutonomyBiasCaching(0)`;
- a lista: **os objetos que anunciam a entrada da OBJf** (o módulo de
  objetos guarda uma lista por entrada, `+0x80(entrada)`), não todos.

Para cada objeto da lista, na ordem (um teste que falha pula o objeto):

1. `ValidateObject`; object data 56 bit 0 desligado e object data 107 = 0
   (ou, na dry clothes, o byte alto do 107 = 2) — os dois só existem no BV
   (no base o 56 é "dry clothes value");
2. com o conjunto de cômodos acessíveis: **o cômodo do objeto tem de estar
   nele**;
3. awareness/lookatTarget: só o lockout (object data 25 ≥ 1 pula, salvo com
   0x80). As outras: **`IsOccupied`** (object data 8 bit 5, 0x20 — a flag 6
   das expressões) pula; `AreInteractionsDisabled(!0x80)` pula: lockout
   (od 25) ≥ 1 sem a flag 0x80, **od 87 "Disable All Interactions" ≥ 1** ou
   **od 82 "Placement Flags 2" bit 2 (0x04)** — esses dois mesmo com 0x80;
   **`IsBurning`** (od 8 bit 9, 0x200) pula;
4. a entrada da OBJf do objeto (`GetFnTable→0x2C(entrada)`) tem de ter
   árvore de ação ou de teste;
5. meu od 66 "function score" := od do valor do objeto (ou 50 sem valor);
   **0 pula** (só quando há valor);
6. se a entrada tem check tree: `RunOneTickTree(comportamento do objeto,
   rodando = a referência, rodando-sobre = o objeto, árvore = check tree,
   tipo 4, parâmetros = nulos)` (BV `0xafe424` → `cTSTreeSim`
   `0xe1ab54`): a árvore roda até o fim num tick, com **o stack object = o
   objeto testado**, "eu" = a referência (eu, ou o objeto da flag 0x08) e
   **os parâmetros = os meus temps** (`cTSTreeSim::OnInit` guarda o
   ponteiro dos temps como parâmetros padrão; o `Gosub` copia os 4
   primeiros); falso → pula. Na awareness/Global Awareness de um Sim, meu
   od 66 := od 112 do objeto (só BV). **De novo: od 66 = 0 pula** (a árvore
   pode zerar ou mudar o valor);
7. filtro de lugar — **só a primeira flag ligada conta**, nesta ordem:
   0x01 só fora (o mundo diz que o tile não é "dentro"), 0x02 só dentro,
   0x04 mesmo cômodo da referência, 0x20 cômodo diferente do da referência,
   **0x40 cômodo = o meu temp 0** (`vtable +0x4C(0)` é o temp 0, BV
   `0xaff7e8`; não é "o meu cômodo");
8. pontuação = od 66 × 1 / (1 + d / 5) com **d = `CalcShortDistance`(ref,
   objeto, 10) = √(distância² no plano + (10 × diferença de andares)²)**
   (constantes 10,0 em `0x277a900` e 5,0 em `0x277a904`); num Sim, × (1 +
   `GetOwnerShipBias`) × `GetAutonomyBias(objeto, nenhuma interação)`;
9. melhor que o melhor até agora (começa em −FLT_MAX) e, com 0x10, a linha
   de visão da referência até o objeto livre → vira o melhor.

O melhor vira o stack object e a primitiva devolve verdadeiro; nenhum →
falso (o stack object não muda).

Os vieses (binário, BV `0xace2dc`/`0xac9ca8`): `GetOwnerShipBias` = 0 salvo
se o objeto tem dono (bit da flag 0x19 do seu "lot object") e conta > 1 para
a minha família: 0,1 × contagem. `GetAutonomyBias` = 1 salvo se o objeto
está no mundo e a opção global bit 2 de `0x277a90c` está ligada (padrão 1:
desligada): então v = Σ peso × queda das entradas da tabela de viés do Sim
(`UpdateCachedAutonomyBiasTable`, tirada do inventário; cada entrada:
ponto, andar, cômodo, peso, raio interno, raio externo, máscara; queda 1
dentro do raio interno, linear até 0 no externo) e o viés é (v + 1)^4 para
v ≥ 0, (1 / (1 − v))^4 para v < 0 (constantes 4 e 2 em `0x277a994…9ac`).
Com a tabela vazia dá 1: o VM pode usar 1 e 0.

Diferenças do VM atual (`src/vm/reach.cpp`, `findBestObjectForFunction`):
olha todos os objetos (o binário só os que anunciam a entrada — dá no
mesmo); **não testa ocupado (od 8 0x20), pegando fogo (od 8 0x200), od 87,
od 82 0x04 nem os cômodos acessíveis**; roda o check tree com "eu" = eu
mesmo com a flag 0x08; aplica os filtros de lugar todos juntos (o binário
só o primeiro); trata 0x40 como "meu cômodo" (é o temp 0); a distância é
a soma em vez da hipotenusa; o filtro de lugar vem depois do check tree nos
dois (igual).

### 0x21 Find 5 Worst Motives (`TryFind5WorstMotives`)

De quem u16 4 (0 eu, 1 o Sim do stack object, 2 outro); tipo u16 6 (> 2 →
erro 0x1E). Ordena os motivos {5 energia, 6 conforto, 7 fome, 8 higiene, 9
bexiga, 13 ambiente, 14 social, 15 diversão} pelo valor (crescente) e grava
os 5 primeiros números de motivo nos temps 0–4; verdadeiro. **O tipo (todos/
mentais/físicos) é validado mas não filtra nada** no BV. Não-Sim → erro 0x0A.
Binário.

### 0x1D Set Motive Change e o motor de motivos

`cTSPerson::TrySetMotiveDelta` (binário): byte 3 bit 0 limpa todos os
deltas; senão motivo = byte 2 (só 0, 1, 5, 6, 7, 8, 9, 14, 15; outro → erro
0x1A), delta por hora = (dono byte 0, dado u16 4), para em (dono byte 1,
dado u16 6), e guarda {motivo, delta / 60 / 30 por tick, limite} na lista do
Sim (**confirma 30 ticks por minuto de jogo**); byte 3 bit 1 zera o dado de
pessoa "Decay … per Day" do motivo (5→96, 6→94, 7→93, 8→97, 9→95, 14→99,
15→101; 0→100 e 1→106 no BV).

`cTSPerson::OnSimulatorTick` (binário), a cada tick: dado de pessoa 27
("tick counter") += 1; se o dado 29 ("Motives Static?") < 2, aplica cada
delta da lista ao motivo (float): delta ≥ 0 sobe até o limite, < 0 desce até
o limite (sem passar); a cada 60 ticks, com o dado 29 = 0, atualiza as
memórias; zera o dado 1 ("Money Amount over Head"). **O decaimento em si não
está no código**: os "Decay … per Day" são lidos pelas árvores (inferido:
elas chamam Set Motive Change com esses valores), então a VM só precisa da
lista de deltas e deste laço.

### 0x24 Dialog (`TryDialog`)

Na primeira execução marca o frame (estado 1) e abre o diálogo; **devolve 2
(espera) enquanto ele está aberto**. Resposta (binário): botão 1 →
verdadeiro (entrada de texto: guarda o texto); botão 2 → falso; três
escolhas: grava 0/1/2 no temp `(byte 7 >> 4) & 7` (> 7: erro). Os tipos 8–10
e 0x1C (notificações TNS) não bloqueiam e usam o objeto do stack object
(ausente: erro 0x17); 0x16/0x19 (painel do tutorial) **não bloqueiam**: temp 1 = 0
esconde o painel, senão o mostra e devolve verdadeiro (`docs/tutorial-mac.md` §5).
Textos: STR# 0x12D da tabela escolhida pelas flags do byte 8 (0x01 privada,
0x40 global, senão semi-global), índice u16 13 (ou temp 0 com 0x02), título
pelo byte 6 (ou temp 3 com 0x10). Binário, com os detalhes de botão
inferidos pelo disassembler.

**Tempo esgotado (rodada 9, binário).** Os diálogos que bloqueiam não ficam
esperando para sempre. O `TryDialog` (BV `0xe2bb64`) põe o pedido na fila do
`cEdithObjectModule` (`EnqueueObjectDialog`, `0x527cf4`); o diálogo mostrado
é um `cTSObjectDialog` (tipos 0–4 do STR# 0xD9: 0 mensagem, 1 sim/não, 2
sim/não/cancelar, 3 entrada de texto, 4 tutorial). `cTSObjectDialog::SetParams`
(`0x1d2b20`) guarda o tipo (byte 5), o bit 0 do byte 7 e "não bloqueia" =
!(byte 7 & 0x80); o `SetupDialog` (`0x1d0da8`, chamado no `Show`) cria os
botões (ids 2, 3, 4 = botões 1, 2, 3 da primitiva) e, **salvo no tipo 4
(tutorial)**, chama `cTSWinPictureDialog::SetTimeout(último botão, ms)` com

    ms = "dialog TimeOut Default Milliseconds" (4000)
       + "dialog TimeOut Multipler Milliseconds" (40) × comprimento do texto

(dois `cTSFloatConstants`, padrões 4000 e 40 no código, existem iguais no
base 1.0). O "último botão" é o 1 no tipo 0 (e no 3: OK com o texto que
houver), o **2 no sim/não** (→ falso) e o **3 no sim/não/cancelar** (→ 2
no temp). O `cTSWinPictureDialog::Init` arma um `cRZCallbackTimer` de uma
vez com esse tempo (tempo **real**, não do jogo; inferido que não para com o
jogo pausado) e, ao estourar, o diálogo recebe o clique nesse botão
(`static_dialog_timeout_callback`, comando `0x287259F6`); a primitiva vê a
resposta como se o jogador tivesse clicado. Ex.: uma mensagem de 120
caracteres some em 8,8 s; um sim/não de 200 caracteres responde "não" em
12 s.

**A janela no app (rodada 13; refeita na rodada 14).** `app/dialog_window.*`
desenha cada tipo pela classe do binário (tabela abaixo): o `cTSObjectDialog`
como o `cTSWinPictureDialog::DoLayout` (medidas, moldura `0xA9500615`,
botões sobre `0xA9500630`, fontes GenHeader/GenDialogBody/GenButton, o
ícone composto em `0xE9000005`–`7`; a caixa de fechar usa a imagem do X dos
telefones `0xAC76E000`, porque `0x11110002` não está no `ui.package`,
inferido), o nome pelos painéis do `0x2D91050A` e os telefones pelos scripts
`0x8C159243`/`44`/`42` com as listas pelas constantes `0xCCCC` (fonte
PhoneBooksListBox, linha 20–21 px, texto escolhido verde; ícones de
amigo/inimigo e paixão/amor do relacionamento, a coluna de cada um é
inferida; o painel sem imagem sob a moldura pintado com a sua `fillcolor`,
inferido). A primeira versão (rodada 13) usava o script `0x6CA51466`, uma
escolha inferida que o binário desmentiu. Sem cabeça: `--dialog-demo
0|0i|0c|1|2|3|5|6|7|14|11|16|16f|17|17f|20|21|23|23g|23t|23r|24[:título[:mensagem]]`, `--dialog-type`,
`--dialog-click`; `s2pkg pie` lista a classe, as linhas e as caixas de nome
dos diálogos especiais (`r14-telefone-*.jpg`, `r14-dialogo-*.jpg`; rodada 14 dialogs3: `r14-comprar-roupa.jpg`, `r14-experimentar.jpg`, `r14-mudar-aparencia.jpg`, `r14-espelho-cabelo.jpg`, `r14-telefone-ordem.jpg`, `r14-aspiracao-descricao.jpg`).

**Diálogos especiais (tipo ≥ 5) não têm tempo (rodada 11, binário).** O
`TryDialog` trata à parte os tipos 8–10/0x1C (notificações), 0x16/0x19
(tutorial, ver `docs/tutorial-mac.md`) e 0x39 (recurso pelo u16 13). Todos
os outros, inclusive 0–4 e os especiais, vão pela fila
(`EnqueueObjectDialog`: a fábrica de janelas cria a classe pelo número do
tipo, IID `0x0797ea13`) e a primitiva devolve 2 (espera) até o diálogo
mudar de estado (0/1 espera, 2 = botão 1 → verdadeiro, 3/4 = botões 2/3 →
falso, no tipo 2 grava 0/1 no temp). Com o byte 7 bit 0 vai para a fila e
devolve verdadeiro na hora. Antes de enfileirar, os tipos 5–7, 0x0B–0x0D,
0x0F, 0x21, 0x33, 0x34 e 0x46 exigem que o stack object, se houver, tenha
pelo menos um object array (`vtable +0x418` = tamanho da lista em `+0x188`;
senão erro 0x6C "Bad Object Array"). **Só o `cTSObjectDialog`
(`SetupDialog`) chama `cTSWinPictureDialog::SetTimeout`**: nenhuma das 37
classes `cTSWinProc*Dialog` com `SetParams` (`cTSWinProcBaseObjectDialog` e
derivadas: telefone, agenda, roupas, aspiração, oferta de emprego, nome do
objeto, preço, ...) arma timer, e as strings "dialog TimeOut ..." só são
lidas ali. Os especiais esperam o jogador para sempre; no jogo eles só
aparecem em interações que o jogador escolheu (inferido). Para o VM sem
cabeça: responder sozinho é escolha nossa (opção), não do jogo.

**Classe de cada tipo (rodada 14, binário).** O
`cTSUIDialogManager::GetObjectDialog` (BV `0x21c5a8`) é um `switch` do tipo
para um ID de classe (CLSID); a classe de cada ID é a que vem logo depois do
seu `GZCOM_<classe>CLSID` (o getter fica na frente do `QueryInterface` da
classe). Tipos do jogo base:

| tipo | classe | script (`ui.package`) | devolve |
|---|---|---|---|
| 0–4, 0x13 e os não listados | `cTSObjectDialog` (CLSID `0x3798E2DB`) | nenhum: `cTSWinPictureDialog`, montado em código | botão / temp |
| 5, 0x0F | `cTSWinProcPhoneBookServices` (`0xCC16C63A`) | `0x8C159243` | array 0 = [ID do objeto] |
| 6 | `cTSWinProcPhoneBookNeighbors` (`0x8C16CA13`) | `0x8C159244` | array 0 = [ID do vizinho] |
| 7, 0x46 | `cTSWinProcPhoneBookParty` (`0xEC16CA68`) | `0x8C159242` | array 0 = convidados |
| 0x0B, 0x0D, 0x2B, 0x2D | `cTSWinProcSelectItemsDialog` | | array 0 (≠ −1 = escolhido) |
| 0x0C, 0x2C | `cTSWinProcJewelryPurchaseDialog` | `0xACDC3000` (não está no `ui.package` do jogo base) | |
| 0x0E, 0x1B, 0x1F, 0x2A | `cTSWinProcFillDialog` | | temp 0 |
| 0x10, 0x2E | `cTSWinProcClothingPurchaseDialog` | `0x8C159251` (`0x40489c`) | o carrinho no guarda-roupa da família, o total sai do dinheiro |
| 0x11, 0x31 | `cTSWinProcClothingDialog` | `0x8C159250` (`Initialize`) | troca a roupa da categoria do temp 0 |
| 0x14 | `cTSWinProcClothingTryOnDialog` | `0x8C159255` | a roupa de provar (0x400) gravada |
| 0x15 | `cTSWinProcVanityDialog` | `0x8C159280` (`0x8C159286` com temp 0 = 1, expansão) | o visual gravado no personagem |
| 0x17 | `cTSWinProcNameObjectDialog` (`0x2D910709`) | `0x2D91050A` | nomes |
| 0x18, 0x22 | `cTSWinProcSetAspirationDialog` | `0xCC16A49E` / `0xCAFE0045` | dado de pessoa 46 |

Os outros tipos de 0x1A em diante são classes das expansões. O `TryDialog`
não enfileira o 0x12 ("Tutorial Tasks"): devolve verdadeiro sem janela.

**`cTSObjectDialog` (binário).** Não usa script: o `Show` cria um
`cTSWinObjectDialogWrapper` (um `cTSWinPictureDialog`) e o `SetupDialog`
(`0x1d0da8`) monta: título (`SetTitle`, fonte GenHeader) se houver;
imagem (o ícone, `SetImage`); **botão 1** (`AddButton`, id 2) — ou, com o
bit 7 do byte 7 ("continue"), **uma caixa de fechar** (`AddCloseBox(2)`,
imagem `0x11110002`) no lugar dele; botão 2 (id 3) nos tipos 1/2; botão 3
(id 4) no 2; o texto (`AddText`, GenDialogBody); no tipo 3 uma caixa de
texto vazia (`AddEditBox("", 999)`); o tempo (salvo no tipo 4); e as
teclas: **Return = botão 1, Esc = o último botão** (o 1 na mensagem e na
entrada de texto), **espaço = botão 1** salvo no sim/não e no
sim/não/cancelar. Botões sem string usam o UIText STR# 0x8E (1 OK, 2
Cancelar, 3 Sim, 4 Não). O `DoLayout` (`0x25b540`): largura mínima 128,
texto quebrado começando na largura do título ou dos botões e crescendo de
32 em 32 px (até 700) enquanto passa de 64 px de altura (o limite cresce
32 por vez até 500); texto em x = 26 (com imagem, à direita dela: x =
largura da imagem + 34), y = 18 (com título, altura do título + 26);
caixa de texto entre o texto e os botões (x 26 a largura − 26); botões
com a largura do texto + 16, no mínimo 82, os que diferem menos de 20 px
do mais largo igualados a ele, 12 px entre eles, centrados, 34 px de
altura, a 12 px do texto (no mínimo em y = 118 − altura); altura = y dos
botões + 34 + 14; a janela centrada na tela. A moldura é a imagem
`0xA9500615` desenhada em nove partes (cantos = um terço da imagem); a
imagem do ícone fica em (26, 20) (com título, altura do título + 28).

**Ícone (byte 7 bits 1–3, STR# 0xF4).** 0 automático: o objeto que a
expansão de tokens devolve, senão o stack object; 1 nenhum; 2 vizinho: o
vizinho cujo ID está no stack object; 3 indexado: pela definição do objeto
da árvore (inferido); 4 nomeado: a string do STR# 0x12D no byte 1, "gz
<id>"/"gzi <id>" uma imagem de UI, "guid <n>" um objeto. A imagem
(`FUN_001d206c`) é a miniatura 40×40 (objeto: a do catálogo; Sim: a
cabeça) centrada sobre o fundo `0xE9000005`, com a moldura `0xE9000006` por
cima e a máscara de alfa `0xE9000007` (grupo `0x499DB772`).

**Entrada de texto (tipo 3).** Com o botão 1 o `BtnPressed` guarda o texto
da caixa 999 e o `TryDialog` (estado 2, tipo 3) o entrega ao simulador
(`cTSObject+0x48`, vtable `0x68`). Nenhuma árvore do jogo base usa o tipo 3
e quem lê esse texto não foi achado (no The Sims 1 era o nome do próximo
Make New Character; o `TryMakeNewCharacter` do BV não lê). A VM guarda em
`VM::enteredText`. **Rodada 14 (engine-re):** o `+0x48` é o `cTSSimSystem`
(interface em `+0x1c`, vptr `0x27c50a0`) e a vtable `0x68` é
**`cTSSimSystem::SetLastUserTypedName`** (BV `0x588db4`, guarda a string); os
leitores são `GetLastUserTypedName` (`0x60` por referência, `0x64`, BV
`0x588d70`/`0x588dac`). Nenhuma chamada direta a eles; as virtuais por esses
slots não puderam ser separadas das de outras classes (busca ambígua), então
quem lê o nome continua não achado (inferido: o nome de um Sim novo, como o
"last user typed name" sugere).

**Sim/não/cancelar.** O `TryDialog` grava no temp `(byte 7 >> 4) & 7`
**0 no "não" (estado 3) e 1 no "cancelar" (estado 4)**; o "sim" não grava
(STR# 0xDA diz o mesmo). A VM gravava 1/2.

**"Return" (byte 7 bit 0).** Enfileira e devolve verdadeiro na hora; a
janela fica até o jogador fechá-la (STR# 0xED 1/3: "return and block/continue
sim"). Sem o bit 7 o jogo continua pausado até ela fechar.

**Nomes (tipo 0x17, `cTSWinProcNameObjectDialog`).** `SetParams`
(`0x370a50`): os vizinhos nos **temps 0–3** (o 0 obrigatório); cada caixa
começa com o primeiro nome do Sim (selecionado); dado de pessoa 65 = 1
(menina) escolhe o ícone `0x1A`/`0x1B` (menino `0x2A`/`0x2B`); um só Sim
com dado 58 "Person Age" > 1 é um renomear. `onAttach` (`0x371cfc`): painel
`0xCDC2CAA4` (renomear), `0xCDC2CAA5` (um bebê), `0xCDC2CAA6` (gêmeos:
temp 1), `0xCDC2CAA7` (servo); `0xCDC2CAA8`–`B0` são de bichos (expansão).
Caixas `1`–`4`, OK `0xA000`, cancelar `0xA001`. O OK fica desligado com uma
caixa vazia (`ValidateUIStringRequired` do texto aparado); `OnOk`
(`ProceedOkButton`) põe o texto aparado de cada caixa como primeiro nome e
devolve verdadeiro; cancelar, falso.

**Telefone (tipos 5/0x0F, 6, 7).** `SetParams` (`0xa7af30`): título byte 6,
mensagem u16 13, botões bytes 3/4 (STR# 0x12D); as linhas vêm do **array 0
do stack object**: objetos nos serviços (`AddAvailableServices`: nome de
catálogo e miniatura), vizinhos nos outros (`CreateNeighborCallList`: nome,
lote da família, diária + vitalícia do relacionamento de quem liga). Os
serviços e o convite começam com a linha 0 escolhida; o texto de cima é
"Chamar %s" (UIText STR# 0xAB #16, ou a mensagem). `OnOk` limpa o array 0 e
grava o ID escolhido (0 se nenhum); a festa grava os convidados (no máximo
temp 5, lista da direita, botões `0x2000` incluir e `0x1000` tirar).
Títulos padrão: STR# 0xAB #9 "Serviços", #10 "Vizinhos". Com o stack object
sem object array, erro 0x6C antes de abrir. **Rodada 14 (dialogs3):** o livro de vizinhos
(`cTSWinProcPhoneBookNeighbors`) nasce no modo 5 (construtor): **slides**
(`0x9000`, as cabeças do script `0x8C159245` em `0x1006`, 7 × 4 de 52 px,
com as marcas amigo/inimigo e paixão/amor das janelas 3–6 e o overlay
apertado no escolhido; setas `0x1004`/5) **ordenados por nome**; o
`PreShow` escolhe o primeiro dessa ordem (`SetSelectedSim(0)`; a VM ordena
as linhas). `onBtnClickedID`: `0x1000` por nome (modo 2/5), `0x2000` por
relacionamento (1/4), `0x3000` slides, `0x4000` texto (`0x8000`); `0xB000`
química é de expansão. A festa ordena a lista da esquerda (`0x3000`/`0x4000`)
e a de convidados (`0x7000`/`0x8000`) com os predicados de lista
(`ListBoxSortPredicate_Name`/`_Relationship`). `CompareByName`: sobrenome,
depois nome (sem caixa, inferido); `CompareByRelationship`: quem tem
paixão ou amor primeiro, depois a maior diária + vitalícia, depois o nome.
**Rodada 15:** o `PreShow` da festa (BV `0x341688`) põe as linhas e chama `SetNeighborListBoxSort(1)` e `SetInviteListBoxSort(1)` (BV `0x3410c0`/`0x34120c`; 1 nome, 2 relacionamento, 3 química, que acendem `0x3000`/`0x4000`/`0xE000` e `0x7000`/`0x8000`/`0xF000`): as duas listas começam **por nome**, e a linha 0 da esquerda escolhida. O nome do lote
(`SetLotName`) é o do lote onde mora a família do vizinho (`VM::LotRecord::
name`, do nome do lote do bairro). A miniatura do serviço: o
`cTSUIThumbnailService` renderiza o objeto ou, sem modelo, uma imagem do
próprio arquivo (`GetEmbeddedThumbnail`); os "Phone Service - ..." não têm
modelo e o grupo deles (`0x7FF65712`) tem os IMG 1 (silhueta) e 2
(ferramentas): o app usa o IMG 1 do grupo do objeto (qual instância o jogo
lê não foi fechado, inferido).

**Guarda-roupa (tipo 0x11, `cTSWinProcClothingDialog`).** `SetParams`
(`0x330088`): a pessoa é o stack object (sem ele, o Sim da família
selecionado); o temp 0 é um literal do Change Outfit traduzido para o bit
da categoria (0 → 0x001, 1 nu → 0x080, 2 banho → 0x008, 3 ginástica →
0x200, 4 formal → 0x020, 5 pijama → 0x010, 6 provar → 0x400, 8 gestante →
0x100, 9/10 → 0x002/0x004, 11 íntima → 0x040, 13 → 0x1000); copia as peças
da categoria para a roupa de provar (0x400). Abas parte de cima `0xECC70000`
(peça 4), de baixo `0xECC70001` (0x10), conjunto `0xECC70002` (8); começa no
conjunto se o Sim veste um, senão em cima. `ApplyClothingChoice`: um
conjunto tira cima e baixo e vice-versa. `OnOk` copia a roupa de provar de
volta para a categoria e grava (`SimSystem` vtable 0x3C); devolve
verdadeiro. **A lista é o guarda-roupa da família** (rodada 14, dialogs3):
o `SetParams` percorre os itens BINX do bin da família (`cTSFamily`, dado
de pessoa 61) e fica com os da idade, gênero e categoria do Sim. No app:
essas peças (as do catálogo do CAS que estão no bin). **Rodada 15:** o bin
de uma família nova é feito como o `cTSFamily::AddMemberClothing` (BV
`0x47ec68`, vtable `0x24` da família: o AGED do personagem →
`AddClothingFromOutfitData`, BV `0x47e5ac`) o enche: para cada categoria de
roupa menos `0xC86` (0x002, 0x004, nu 0x080, provar 0x400, 0x800), as
partes 4 (cima), 0x10 (baixo) e 8 (conjunto) da roupa do Sim viram itens
BINX (`AddClothing` guarda uma de cada; a parte 0x20, acessório, vai pelo
serviço de bins e fica de fora no app). Quem chama o `AddMemberClothing` não
foi achado (só pela vtable): o app o faz ao gravar a família do CAS
(`writeNewFamily`, `NewSim::clothing` = `cas::familyBinClothing`;
`s2pkg cas-make --hood` também) e, para uma família ainda sem bin no lote,
com os Sims dela no lote (`familyBin` no app, guardado ao salvar; o momento
é inferido); um bin vazio ainda mostra o catálogo (inferido).
`r15-planejar-roupa-familia-nova.jpg` com a miniatura do
`CASThumbnails.package` no lugar da vista 3D do Sim (inferido), e o OK grava
o personagem com a roupa nova (`writeCharacter`, como o Sim que cresce) e o
veste de novo se ele usa essa categoria.

**Arara de roupas (tipo 0x10, `cTSWinProcClothingPurchaseDialog`, script
`0x8C159251`; rodada 14, dialogs3).** `SetParams` (`0x404ac8`): o Sim é o
stack object (as árvores "Interaction - Buy - Clothing Rack" e "DEBUG -
Interaction - Buy" do `ShoppingRackGlobals` põem o Sim lá); o temp 0 é o
tipo de roupa pelo mesmo `switch` do guarda-roupa, mas **6 vira 0x001 e é
tratado como 0** (separados); só 0, 9 e 10 (teste `0x601`) oferecem partes
de cima e de baixo (abre em cima), os outros só conjuntos (abre em
conjuntos); o **temp 1 é o preço por peça** (`cTSShoppingCart` slot 0x20 →
`+0x28`; o total, slot 0x48, é `quantidade × preço`). O byte 9 = `'.'` é a
loja própria das expansões (lê locais da árvore; não existe no jogo base).
A janela: abas cima/baixo/conjunto (`0xECC70000`–2), idade
(`0x200`–`0x204`) e gênero (`0x400`/`0x401`) que filtram a grade, grade
`0x1006` (cima 3 × 2 do `0x8C159252`, baixo/conjunto 4 × 2 do
`0x8C159253`/`54`, as constantes do script) com setas `0x1004`/5, "pôr no
carrinho" `0xECCDF663`, a aba do carrinho `0x8CCD0001` (grade 3 × 2,
"tirar" `0x6CCDF7D7`), "%s Itens no Carrinho" (`0xCCCDF42D`) e "Total: %s"
(`0xACCDF819`); na célula, a janela 7 (tique) marca o que está no carrinho e
a 8 (armário) o que a família já tem (inferido pelas imagens). `OnOk` →
`SaveCartStandard` (BV `0x406bfc`; **lido na rodada 15**): as partes de
cima e de baixo do carrinho (os conjuntos inteiros, `outfit` & 8, ficam de
fora) dão as idades pedidas por gênero (1 mulher, 2 homem); para cada um dos
7 bits de idade, o `cTSFamily::GetNumCompleteOutfits` (BV `0x47f258`: os
itens da idade, gênero, espécie 1 e categoria — ou sem categoria —, sem os
de conteúdo próprio com `allowCustomContent` falso; cima × baixo +
conjuntos) do bin com essas peças tem de passar o do bin sozinho em cada
gênero pedido; se alguma falha pergunta (sim/não do gerente de diálogos,
vtable `0x130` estilo 3, UIText STR# 0xCA #7; `0x5301814C` = sim): sim
compra, não fecha a arara sem comprar (o `OnOk` devolve verdadeiro dos dois
jeitos). O UIText do jogo base não tem o STR# 0xCA (é de expansão): o app
usa um texto seu (inferido). `r15-arara-pergunta.jpg`. Então
`cTSShoppingCart::CompletePurchase` (slot 0x6C) põe cada item no bin da
família (`cTSFamily::AddClothing`) e tira o total do dinheiro (vtable 0xB0,
despesa 6). Na VM o OK põe as chaves GZPS do carrinho
(`Dialog::cart`) no `VM::Family::wardrobe` (uma vez) e tira `itens × temp 1`;
o `hood_save` grava cada item novo como um BINX (`binidx` 1, `objectidx` 2,
o resto 0, como os do jogo) com o 3IDR da mesma instância (vazio, o bin
`0x6C4F359D` da família, o GZPS); instância: uma depois do maior BINX/3IDR
do pacote (inferido). A grade mostra o catálogo do CAS da idade/gênero/categoria (quem enche a
lista `+0x1a8` da grade não foi achado; inferido); o OK só liga com algo no
carrinho e dinheiro para pagar (inferido).

**Provar roupa (tipo 0x14, `cTSWinProcClothingTryOnDialog`, script
`0x8C159255`).** `SetParams` (`0x3bed44`): o Sim é o stack object; o temp 0
pelo mesmo `switch` (6 → 0x001, mas sem separados); copia as peças da
categoria para a roupa de provar (0x400). A janela tem as abas e a grade da
arara (sem carrinho nem idade/gênero; o fundo sem imagem pintado com a
`fillcolor`, inferido); escolher um conjunto tira cima e baixo e
vice-versa. `OnOk` grava o personagem (`SimSystem` vtable 0x3C) com a roupa
de provar; a árvore troca a roupa depois. No app o OK grava a categoria
0x400 com o escolhido.

**Penteadeira / espelho (tipo 0x15, `cTSWinProcVanityDialog`, script
`0x8C159280`).** `SetParams` (`0x2effa4`): o Sim é o stack object; temp 0 =
1 abre a cirurgia plástica (`0x8C159286`, das expansões). As abas
(`HandleTabSelect`): penteado `0x4CEDF125` → painel `0x2CEDF326` (cores
`0x8800`–`0x8803`, grade 3 × 4 de `0x8C159283`), maquiagem `0x4CEDF126` →
`0x2CEDF327` (blush `0x6CA52068`, delineador `0xAD474C78`, sombra
`0x6CA52069`, batom `0x6CA52067`, 4 × 1 de `0xCB980E93`), óculos
`0x4CEDF128` e maquiagem personalizada `0x4CEDF127` → `0x2CEDF328` (3 × 5),
sobrancelhas `0x4CEDF130`, fios de barba `0x4CEDF131` e barbas
`0x4CEDF129` → `0xAD4F1B1C` (3 × 4, com a fileira de cores
`0x2D4F258C`). `OnOk` (`0x2f2448`): o penteado em todas as categorias (menos
nu, provar e 0x800), grava o personagem (CAS vtable 0x20) e veste o Sim de
novo no que ele usa (dados de pessoa 8, 119, 120). No app as escolhas são as
do nosso CAS (`cas_mode.cpp`: sobreposições por subtipo e bin, óculos e
barbas de malha), o OK grava o personagem e veste de novo; a vista 3D do Sim
é a cabeça (inferido). **Rodada 15:** o `HandleTabSelect` (BV `0x2f50b0`)
mostra a fileira de cores da barba `0x2D4F258C` (`FUN_0093fed8(painel,
0x2D4F258C, 1)`) nas abas sobrancelhas, fios de barba e barbas; no
`onBtnClickedID` (BV `0x2f651c`) um id com `0x8800` dentro dessa fileira lê o
GUID da legenda do botão (o tom de cabelo "0000000N-…") e chama
`cTSUISimEditor::SetHairColor` (vtable `0x128` do editor; o único setter de
`cTSLID` de cor além do tom de pele) e atualiza os overlays: sobrancelhas e
barbas seguem a cor do cabelo, então a fileira muda a cor do cabelo também
(no app: a mesma escolha das cores do cabelo). "Personalizado" `0x8804` do
componente de cabelo (`cTSUICASComponentHair::HandleTabHairColor`, BV
`0x3e1e54`) liga a flag `+0x268` que o `UpdateFilter` passa ao filtro como
"custom": a grade mostra os cabelos de tom fora dos regulares
(`nTSCASCore::IsRegularHairTone`: GUID 1..5; no jogo base, os 81 de tom 6,
os tingidos); uma cor desliga. `r15-espelho-barba-cor.jpg`,
`r15-espelho-personalizado.jpg`.

**Joias (tipo 0x0C, `cTSWinProcJewelryPurchaseDialog`, script
`0xACDC3000`).** O script não está no `ui.package` do jogo base e nenhum
objeto do jogo base abre o tipo 12 (a árvore "Interaction - Jewelry Rack -
Browse" do `ShoppingRackGlobals` não tem objeto); fica uma caixa OK/Cancelar.

**Aspiração (tipo 0x18, `cTSWinProcSetAspirationDialog`, script
`0xCC16A49E`; 0x22 usa `0xCAFE0045`).** O vizinho é o ID no stack object;
os botões `0x0CDC000n` valem `1 << (n − 1)` (romance 1, família 2, riqueza
4, popularidade 0x10, conhecimento 0x20) e gravam o dado de pessoa 46 do
vizinho; `Validate` exige uma escolhida (≠ 0, 0x40, 0x100) e `OnOk` grava no
Sim do lote. O texto "%s não é mais criança" é o `0x2D9B8E78` (menina) ou
`0x2D9B8E7E` (menino). **O slide da descrição** (`RefreshInfoPanel`, `0x3cf954`): escolhida
uma aspiração, o slide `0x2000` sai para a direita (o `onTick` move o
`0x0DA39A45` até a largura do pai menos a sua; no app de uma vez,
inferido) com o nome (`0x4D9B8E8F`) e a descrição (`0xCDA394CA`) do STR#
0x91 do Live.package (um-baseado: romance 13/6, família 9/1, riqueza 11/3,
popularidade 10/2, conhecimento 12/4, 0x40 14/7, 0x80 40/42), a descrição
logo abaixo do nome e do tamanho do texto. **Rodada 15:** o catálogo
"Aspirações Altas" (`0xCDA395BB`) é um `cTSWinProcWantsCatalog` (criado no
`onAttach`, BV `0x3d14f8`); o `RefreshInfoPanel` o põe logo abaixo da
descrição, chama `SetAspiration` → `LoadPowerWantsByAspiration` (BV
`0xaa5b2c`: o simulador de desejos, vtable `0x64` = `GetLifetimeGoals`,
mapa da aspiração preenchido por `AddLifetimeGoal` com o `<lifetimeGoals>`
do `WantTuning.package`) e estica o slide até o fim do catálogo mais a
margem que ele tinha no script (`+0x48`, 18 px). A grade `0x1006` tem 5 × 2
células do script `0x49065095` (`CreateThumbnail`; `onAttach` do catálogo:
10 itens), e o `SetWant` (BV `0xaa6748`) liga a janela 1 (fundo de desejo)
ou 2 (de medo), a 3 (estouro de desejo "Power") pelo nível, a 4 com o ícone
primário sob a máscara de alfa da própria imagem (`ApplyAlphaMask`) e a 5
com o ícone secundário. No app: `DialogWindow::lifetimeWants` (os nomes do
XML viram bits como os botões do diálogo — Romance 1, Family 2, Wealth 4,
Reputation 0x10, Knowledge 0x20, Grow Up 0x40 —, inferido) e
`r15-aspiracao-desejos.jpg`; o espaçamento das células (margens 1 e 3 do
script) é inferido.

**Comida (tipo 0x0E, `cTSWinProcFillDialog`, script `0x2CB242F8`).**
`SetParams` (`0x294ca4`) lê 4 locais a partir do byte 11: unidades que tem,
custo de 50 unidades, entrega, máximo (os tipos 0x1B/0x1F/0x2A são de
expansões). `CalculateCostOfUnits(n) = (n × custo + 49) / 50 + entrega`; a
barra não passa do dinheiro da família; OK só com algo comprado e grava
`valor − unidades` no temp 0.

**Prateleiras (tipos 0x0B/0x0D, `cTSWinProcSelectItemsDialog`, script
`0x2CB242F6`).** Os objetos do array 0 do stack object em uma fileira de
miniaturas; temp 0 = 1 deixa escolher vários, temp 1 = 1 só o que a família
pode pagar (`UpdateFunds`; temps 2/3, 100 por padrão, não seguidos); `OnOk`
grava −1 nos itens não escolhidos.

**Notificações (tipos 8–10, 0x1C)**: o byte 10 é o tempo de vida
(`cTSUINotificationService::SetNotificationTimeOut`) e o byte 9 a prioridade
(`SetNotificationPriority`). A fila (`cTSUINotificationQueue`) conta com um
`cRZTimer` em **segundos** (`SetUnits(5)`, unidade base ms ÷ 1000) desde que
a notificação apareceu; `cTSWinNotification::IsDone`: some quando tempo ≠ 0
e decorrido ≥ tempo. 0 = fica até ser fechada (ou até o tipo 9 "Stop Text
Notification").

### 0x2E Snap (`TrySnap`)

**Quem se move sou eu**, em relação ao stack object (sem stack object: erro
0x15; eu inválido: 0x17). Modo u16 2 (STR# 0xCF): 0 slot do número no
parâmetro u16 0 (fora do intervalo: 0x08), 1 contido no stack object, 2 na
frente dele, 3 slot literal u16 0, 4 slot global; slot inexistente → 0x1B.
Falha → falso e grava no meu object data 52 ("primitive result") 1 (pediu a
um Sim no caminho para sair, flag 0x02), 0x0E, ou o código do erro de
posicionamento + 2. Com 0x10 só testa. Binário.

### 0x6F Change Light (`nTSSG::cObject::TryChangeLight`)

Objeto (dono byte 2, dado u16 3; inexistente: 0x17). Intensidade = byte 7
(ou temp 0 com o byte 1 bit 1) em %, **> 100 vira 50**; com o byte 1 bit 0 o
temp 1 é lido (duração). Luz byte 8 = 0xFF: todas as luzes do modelo; senão
o nome é a entrada do STR# 0x8E do stack object (sem stack object: 0x0A;
nome vazio: 0x4D). Sem modelo: 0x4C. Instantâneo (o fade é do renderizador),
verdadeiro. Binário.

### 0x79 Change Outfit (`nTSSG::cObject::TryChangeOutfit`)

Sim (dono byte 9, dado u16 10; inexistente: 0x32; não pessoa: 0x1C). Fonte
(byte 0): 0x01 a definição do stack object (stack object ausente = sem
fonte), 0x02 GUID u32 4, 0x40 GUID nos temps 0/1, nenhuma = as roupas do
próprio Sim; categoria = byte 8 (ou a variável (dono 1, dado u16 2) com
0x04); 0x20 zera os dados de pessoa 119 e 120; troca (0x10 reconstrói a
atual); 0x08 grava. Binário.

### 0x69/0x6A/0x6B Animate Object/Sim/Overlay (`try_animate_*`)

**O byte 3 não é flag: é a velocidade em 1/32** (0x20 = 1,0; com a flag 0x02
do byte 2, o temp 2). Isso explica o "0x20 em 96% dos usos" do código atual.
Animate Sim (binário): na primeira execução resolve o nome (lista byte 6;
0x80 = global da idade; senão a lista privada e, se não achar, as outras),
nome vazio → 0x43, tipo errado → 0x73; guarda o pedido no frame e **devolve
2 até a animação acabar, depois 1**; árvore de animação que não existe →
verdadeiro na hora ("missing animation tree"). Byte 13 / 100 é um peso de
blend (0 ou fora de 0–1 = 1,0). Animate Object/Overlay devolvem 1 logo depois
de começar (não bloqueiam); repetições no temp 1 com 0x20 (0 ou < −1 → erro
0x70). Sem animador: 0x41; sem pessoa (Sim): 0x42.

**Rodada 13: as regras de postura inferidas na rodada 11, conferidas** (BV:
`try_animate_sim` `0x69d634`, `try_animate_stop` `0x6a0554`,
`AddChoreoAnimRequest` `0x696644`, `CreateTransitionBlend` `0x68b4c4`,
`AbortAnimRequest` `0x68bac0`, `GlobalRootRotHeading` `0x68c320`,
`cObject::ApplyAnimationRotation`/`ApplyAnimationOffsets` `0x656b10`/`0x654b28`):

- **"Corpo inteiro substitui overlay de corpo inteiro": não existe no
  jogo** (rodada 13, anim-choreo, decifrado). O Animate Sim cria um pedido de
  tipo 2 (`tARType`: 1 objeto, 2 Sim, 3 overlay, 5 rota) e nada em
  `try_animate_sim` nem em `AddChoreoAnimRequest` mexe nos outros pedidos.
  A troca explícita é só a do `CreateTransitionBlend` (um pedido novo de
  tipo 5 faz o anterior de tipo 5 sair por blend). O que aparece é decidido
  **osso a osso** pela mistura dos canais (abaixo). A regra inferida da
  rodada 11 saiu do `animateSim`.
- **Mistura dos canais** (`nGZSceneDBAnim`): cada pedido é um conjunto de
  canais, um por osso que a animação mexe, e cada osso tem um
  `cChannelUberOutlet` que junta os canais "base" que o alimentam
  (`AddBaseChannel(canal, prioridade, tipo)`, BV `0xee6ed4`: a lista fica
  ordenada da prioridade maior para a menor). O misturador
  (`PrepareForBaseChannelBlender`, BV `0xee659c`, e o `ChannelData` do
  quatérnio, BV `0xd5e1c4`) faz, por osso: dos **níveis 8 a 0**, soma os
  pesos dos canais do nível (peso = função de blend do pedido × peso do
  pedido); se a soma ≥ 1, o nível leva o que sobrou inteiro (cada canal
  `peso × resto / soma`) e os níveis de baixo ficam com 0; se < 1, cada
  canal leva `peso × resto` e o resto vira `resto × (1 − soma)`. No fim, se
  o total ficou entre 0 e 1, tudo é normalizado (um canal sozinho entrando
  em fade já tem o osso inteiro); osso sem peso nenhum não é escrito
  (fica como estava). A combinação é uma média corrida por peso (slerp
  com `peso / acumulado`); os canais "delta" (aditivos) entram depois,
  multiplicando (ver "Canais delta" abaixo). **O nível de cada pedido** vem da primitiva
  (`AddChoreoAnimRequest` dá a ele também o nome de canal para depuração,
  as strings `sil/sim/...`):
  | primitiva | nível | nomes |
  |---|---|---|
  | Animate Sim (byte 12 = p) | `2p` (p < 0 → 0, p > 3 → 6) | `sil` 0–1, `sim` 2–3, `sih` 4–5, `siu` 6+ |
  | Animate Overlay (byte 11 = p) | `2p + 1` (p < 0 → 1; p ≥ 4 → 0) | `osl` 0, `ovl` 1–2, `ovm` 3–4, `ovh` 5–6, `ovu` 7+ |
  | Animate Object | 2 | `obj` |
  | rota (locomoção) | 2, 3 ou 5 | `rsm`, `rom`, `roh` |
  Nos dados: o Animate Sim usa p = 1 em 2.984 de 4.010 (nível 2), 2 em 392,
  0 em 329, 3 em 304; os idles do idle system e da cadeira usam overlay de
  prioridade 0 ou −1 (nível 1), e as poses de carregar bebê ("Pickup -
  Poses", "CT - Set Carry", "Feed - Baby - Loop") prioridade 2–3 (níveis
  5–7). Logo o get-out da cadeira (nível 2) passa por cima do idle sentado
  (nível 1) sem regra nenhuma, e a pose de carregar o bebê fica por cima
  do Animate Sim. No código: `vm::animlevel`, `Overlay::level`,
  `AnimationState::level/weight`, e o `AnimPlayer` mistura assim
  (`Options::level`, `test_anim` "channel levels"). Inferido: o nível da
  caminhada (usamos 3, `rom`) e o das poses de carregar do Reach/Put
  (usamos 5; elas vêm do `AddToAnimList`, não lido).
- **Canais delta (aditivos)** (rodada 14, front "anim-additive", decifrado
  no BV): o que faz um canal ser delta é o **bit 15 das flags do canal**
  (`cSharedChannelCubicKeysConstProxy4::IsBaseDataReturned`, BV `0xee4680`:
  `byte[0x12] >> 7` da palavra big-endian; aceso = base, apagado = delta; na
  mesma palavra a duração são os bits 0–14 e o tipo os bits 22–24). O
  `cActiveAnimSkeletal::SetupChannel` (BV `0xd5b8d4`) pergunta isso (vtable
  `0x6c` do canal) e põe o canal no outlet do osso com
  `AddBaseChannel(canal, prioridade, tipo)` (BV `0xee6ed4`) ou
  `AddDeltaChannel(canal, prioridade)` (BV `0xee6dc8`: uma segunda lista em
  `+0xb8`, ordenada da prioridade maior para a menor, sem níveis). Nos dados
  (`s2pkg anim-sweep`): 1.033 canais de rotação e 689 de translação delta,
  **todos** em alvos de tipo 6 (`cAnimTarget::AnimType`; os ciclos de andar
  são 5, quase todo o resto 1), e esses alvos não têm canal base nenhum:
  106 animações — as posturas `walk/run-altK-*Blend` (idosos, grávida,
  deprimido, feliz, bravo), as mudanças de pé da conversa
  `*-soc-talk-shift*-SWAT`, as quicadas no sofá `*-sofa-playful-*bouncee*`,
  a cadeira assombrada, `e-idle-bentSpine[-FK]` (a coluna curva do idoso),
  `*-spine-bend-*` das cadeiras, `a2o-book-carry-blend`,
  `a-react-nod/shake-expBlend`, `p-run-blend`. Muitas têm a flag de corpo
  inteiro (bit 0) mesmo assim.
  **A mistura** (`cChannelUberOutletQ::ChannelData`, BV `0xd5e1c4`): depois
  da média dos canais base, se o peso base total > 0 (ou o outlet tem
  `ApplyDeltaDataToIK`, abaixo) e há deltas, cada delta com peso `w`
  (vtable `0x20` do canal no tempo, > ε) vira `q^w` (meio ângulo
  `a = acos(q.w)`, `(sin(a·w)/sin(a)·xyz, cos(a·w))`; nada com `q.w` fora de
  (−1, 1); sem trocar de hemisfério) e acumula `acc = acc · q^w` na ordem da
  lista; no fim **`base · acc`** (produto de Hamilton, `FUN_00dc71f8`), pela
  "delta base order" do outlet = 1 (o construtor, `FUN_00ee7004`, põe
  `+0xc4 = 1`; com 0 seria `acc · base`; quem chamaria
  `SetDeltaBaseOrder(false)` não foi achado). Translação
  (`cChannelUberOutletXYZ`, BV `0xd6b0fc`): a média base normalizada e, só
  se houve base, `+ w · t` de cada delta. O `SetupChannel` liga
  `SetApplyDeltaDataToIK(true)` (`+0xc7`) nos outlets de rotação de `head`,
  `neck`, `spine0`, `spine1` e `spine2` (hashes `0x42CD1793`, `0x1A9F50FD`,
  `0xE446CAC7`, `0xE087D770`, `0xEDC4F1A9`): neles o delta vale mesmo sem
  canal base (o look-at mexe neles). No código: `Animation::Channel::isDelta`,
  `sampleDeltas`, `deltaPower` e o fim do `AnimPlayer::pose` (depois da
  mistura por níveis); `samplePose` e `keyedBones` ignoram os deltas (um alvo
  só de deltas não toma osso nenhum, nem com a flag de corpo inteiro, e as
  cadeias de IK dele não resolvem nada); o Animate Sim de um clipe só de
  deltas toca por cima do corpo, como um parcial. Inferido: o peso do delta
  como fade × peso do pedido (o `0x20` do `cChannelUber` não foi lido) e o
  espelho de um delta (pelo espelho da pose "repouso + delta"). Não feito:
  deltas nos nós de IK mexerem nas cadeias. Teste: `test_anim`
  "testDeltaChannels".
- **Onde fica o quadro da animação**: na fila de coreografia. A posição e a
  direção "globais" do objeto animado são as do osso raiz da fila
  (`GlobalRootTransPosition`, `GlobalRootRotHeading`: o vetor +Y do osso
  raiz girado, `atan2`), lidas no `snap_to_current_location`/`PostSnap` para
  deixar o objeto onde a animação o largou. Os objetos ainda têm um
  deslocamento e uma rotação de animação nos **dados de objeto 108–110**
  (x, y, z × uma constante) e **111** (ângulo × outra constante), aplicados
  por `cObject::ApplyAnimationOffsets`/`ApplyAnimationRotation`.
- **O giro pelo `heading` do cabeçalho do ANIM: o jogo aplica** (rodada 13,
  decifrado). `cActiveAnimSkeletal::SetupChannel` (BV `0xd5b8d4`) soma o
  `HeadingOffset` do pedido (`cChoreoRequest` vtable 0xd4) e o do recurso
  (`cAnimResource::HeadingOffset`, o campo do cabeçalho) e gira por esse
  ângulo o canal de rotação do osso marcado com um eixo de heading
  (`cBoneDataExtension::HeadingOffsetAxis`: X, Y ou Z). É o que o
  `AnimPlayer::Options::turn` faz (os filhos do osso do topo girados em Z).
  O heading **do pedido** vem do Animate Sim: com o bit 0x04 do byte 8 (sem
  texto no editor; 7 usos nos dados) é `−temp 4 × k` (`FUN_008b6860`; k é
  um global do `__bss`, não lido), senão 0: não feito.
- **Tempo de blend: T = 233,333 ms** (decifrado). O global `0x2c0ddd0` é
  escrito uma vez, pelo inicializador estático do `TSSGAnimatable.cpp`
  (BV `0x20b4ef0`, no `__StaticInit`; o mesmo valor vai para `0x2c0de34`),
  com o literal `233.33333` (7 quadros a 30/s). Unidade: ticks de
  animação (ms), como as durações com que o `CreateTransitionBlend` o
  compara. O Animate Stop usa **0** sem flag (os pedidos saem na hora,
  `cChoreoRequest::Abort` com status 3, vtable 0x228 da fila), **T** com a
  flag 0x02 ("blended Out") e **0,3 × T** com 0x20 ("short blended Out";
  `FLOAT_0274b670` = 0,3), pelo `AbortAnimRequest` (`FUN_00a47bf0`: o
  pedido ganha uma função de blend que desce a 0 nesse tempo e a regra de
  expiração 4). O mesmo T é o blend de entrada/saída do
  `CreateTransitionBlend` (com as flags "No Blend in/out" desligadas) e o
  que o Animate Sim usa para tirar o próprio pedido interrompido. No
  código: `vm::kBlendTicks`; o overlay começa a sair (evento 9997) T antes
  do fim, entra em T, e o Animate Stop passa ao app o fade de cada overlay
  tirado (`AnimationState::stoppedFades`).
- **Outros detalhes do Animate Stop** (`try_animate_stop`, BV `0x6a0554`):
  a prioridade do byte 8, se > 0, só para os pedidos cujo nível com o bit
  0 apagado é `2p − 2` (o Animate Sim de prioridade p − 1 e o overlay de
  mesma prioridade); só para os pedidos tocados espelhados como o operando
  diz (bit 0 do byte 2, ou o temp 3 com 0x08), os dois com 0x40; pedidos
  dos tipos 7 e 9 e os já expirados nunca param. Modos 4–11 comparam o
  `tOEType` (4→3 carregar … 8→2 normal, 9→7, 10→8, 11→9), como o código já
  fazia. Feito em `animateStop`.
- **O evento −1 no Animate Stop**: o `try_animate_stop` não chama árvore
  de evento nenhuma; o pedido parado na hora é abortado com status 3
  (`tCRStatus`) e quem avisa é o tratador de eventos do pedido
  (`cAnimEventCB` → `cCustomEventHandler2`, chave −1 → −1). Que o status 3
  vire a chave −1 continua inferido (bate com as árvores de idle); o
  pedido que sai por blend mantém o status e o evento dele no fim do blend
  não foi lido.

## 4b. Pendências do round 5 (binário)


**Veículos (rodada 12, VM).** O carro dirige pelo "Event Tree - Drive"
(CarGlobals `0x7F4EA230`, `0x2008`): no 9999 de cada volta de
`o-…-drive` decide parar ("Function - Should Stop?" pela distância até o
portal de parada) ou tocar a volta de novo com Animate Object. No
opensims2 isso travava de dois jeitos: (1) o limite "duas de um nome" do
Animate Overlay valia também para o Animate Object, e a terceira volta não
começava (nunca chegava o 9999, "Wait for Vehicle Stop" esperava para
sempre: a van da empregada e o ônibus escolar paravam no meio-fio o dia
todo); agora a mesma animação do Animate Object substitui a que toca. (2)
o motor move o objeto pela raiz da animação (`cAnimatable`); a VM não
movia, e a distância nunca diminuía: agora, no fim de cada volta, o objeto
(todas as partes do grupo) anda para a frente (direção do objeto) o
quanto o `root_trans` da animação anda (Y do modelo; ~1,95 tile em
`o-vehicleMinivan-drive`; inferido o momento e a direção). Prova:
`S2PKG_VEHICLES=1 s2pkg autonomy N001_Lot68 --ticks 24000`: a empregada
chega às 9:07, desce em (35,5; 16,5), trabalha e a van vai embora antes
das 10:30; o ônibus escolar chega às 8:00, espera ~25 minutos, buzina e vai
embora. Os adolescentes não sobem porque "Get My Riders" só chama quem
tem humor (motivo 3) > 0, e os Sims do lote 68 estão com humor muito
negativo (fome: frente da comida). A textura
`vehicleschoolbus-groundshadow-alpha_txtr` que o TXMT da sombra do ônibus
pede não existe nos dados do jogo base (só o nome no material).

### Conversão da versão 0 do Animate Overlay (rodada 13)

`AnimateOverlayParam::UpgradeVersion` (BV `0xac4af8`): na versão 0 (os
BHAVs do jogo base, 1/3 dos Animate Overlay, entre eles "CT - Set Carry" e
"Pickup - Poses") os bytes 10–11 e 12–13 são dois u16; a conversão põe no
byte 10 o byte baixo do primeiro, no **byte 11 (a prioridade) o byte baixo
do segundo** e zera 12–13. Sem isso a pose de carregar o bebê (versão 0,
`FF FF 02 00` em 10–13) ficava com prioridade −1 (nível 1) em vez de 2
(nível 5). Feito em `animateOverlay`.

### Conversão da versão 0 (BHAVs do base) de Look At e Run Tree by Name

O parâmetro guarda os 16 bytes de operando em `+4..+0x13` e a versão em
`+0x14` (0 = como veio do arquivo do base; 1 = convertido). A conversão roda
uma vez, ao ler (`XPrimParam::Read` → `UpgradeVersion`).

- **Look At** (`LookAtParam::UpgradeVersion`, BV `0xac4990`): a versão 0 tem
  um u16 no operando 4 (`LookAtVersion0Param::SwizzleParams`: u16 em 2, 4,
  6, 9 e 12; a versão 1 só em 2, 6, 9 e 12). Conversão:
  `op[4] = op[5] = (int16)u16_v0 / 2` (divisão com truncamento para zero) e,
  se o byte 0 tem o bit **0x08**, liga o bit **0x02 do byte 15**
  (`FUN_019a9768`). O resto fica igual.
- **Run Tree by Name** (`RunPrivateTreeParam::Read`, BV `0xac4608`): na
  versão 0, o byte 12 vira só o seu bit 0 (`op[12] = op[12] & 1`; os outros
  bits são apagados). A troca de bytes (`SwizzleParams`) tem duas formas,
  conforme um teste global (`FUN_016606e4`): u16 nos operandos 1, 4, 7, 10
  ou 8, 10, 12, 14.

### Remove Object Instance (0x12): os 7 GUIDs e o "cleanup all"

`cTSObject::TryKillObject` (BV `0xe40d68`): **não remove** e devolve
**falso** se o GUID do objeto é um destes (conjunto estático):

| GUID | objeto |
|---|---|
| `0xCC7B08F8` | Portal - Car - Service Start |
| `0x6C7B0905` | Portal - Car - Service Stop |
| `0xCC203477` | Portal - Car - Start |
| `0x865A6812` | Portal - Car - Stop |
| `0x81E6BEF9` | Portal - Pedestrian |
| `0xD20C0AEE` | Trash Can - Outside |
| `0x39CCF441` | MailBox |

(os pontos de chegada/saída do lote e os dois objetos fixos da calçada).
Removendo: `cEdithObjectModule::AddToKillQueue(id, cleanup)` com
`cleanup = !(byte 2 & 0x02)`. Ele marca o objeto (e as partes de um
multi-tile) para morrer, roda a **entrada 3 do OBJD ("cleanup")** dos
objetos contidos que não são Sims, e grava a flag de objeto **0x1000 = !cleanup**
(com o bit 0x02 ligado o objeto morre sem rodar a própria limpeza). Volta
**2** (espera; a thread morre junto) quando o objeto removido sou eu e o
byte 2 não tem 0x01; senão **1**. Objeto inexistente: falso.

### Push Interaction (0x0D): interação aninhada e o erro 0x6F

`cTSObject::TryPushAction` (BV `0xe33530`):

- O byte 3 bit **0x08** liga a flag 0x40 da interação nova (`+0x54`).
- Aninhada = a entrada do TTAB tem o bit **0x8000** dos flags (`cTSTreeTableEntry
  +0x30` bit 15, copiado em `cTSInteraction +0x2E`). Uma interação
  aninhada exige que o alvo tenha **interação atual** e que a entrada do
  TTAB dela tenha o bit **0x4000** (bit 14, `+0x2C`: "permite aninhar");
  senão **erro 0x6F**.
- Aninhada vai para a sub-fila (cheia com mais de 4 → resultado 4); não
  aninhada para a fila (mais de 6 e não deu para trocar → resultado 3).

### `SetReachObjectID` (`cTSPerson +0x8EA`) e `+0x8EC`

Não são dados de pessoa: são campos do `cTSPerson` (**reach object** e
**reach surface object**), escritos pelo Reach/Put (§4) e lidos só pela
animação: `cTSPerson::GetReachObjectSelectorGUID`/`GetReachSurfaceSelectorGUID`
e `cEventModifierBase::GetReachSelectorGUID` (BV `0xbf0d8c`) pegam a
definição desse objeto para os eventos da animação de pegar/pôr (prender
o objeto na mão no evento de "grasp", soltar na superfície). O SimAntics
não lê esses campos; não vão para o PERS.

### Animate Sim: como o jogo acha o nome da animação

`try_animate_sim` (BV `0x69d634`): índice = u16 do operando 0 (ou temp, com
o bit 0x04 do byte 2); a **tabela = byte 6**:

- `0x80`: `FindGlobalName` (BV `0x6901e0`): a tabela de animações globais
  da idade (abaixo), entrada = índice; se a animação não existe, tenta de
  novo com a tabela "de reserva" (`retry`).
- outro valor: a tabela é o **STR# com esse número**, procurado
  (`FindSpecificName`, BV `0x690700`) **no objeto do stack object** (ou, com
  o bit 0x10 do byte 8, no objeto que o Sim está usando) e, se a entrada
  estiver vazia, **no semi-global desse objeto**; o nome final é o
  **prefixo + a entrada**, e só vale se a animação existe. Se falhar, tenta
  com a tabela de reserva, e por fim `FUN_0068bcb0` troca a letra da idade
  do prefixo pelas alternativas (`d`, `f`, `w`, `l`, `k`).
- Só depois disso (BV `0x69da50..`): o nome é classificado pelo tipo
  (vtable `0x380`; inferido: pelo prefixo `a2o`/`o2a`/`a2a`/...); tipo não
  reconhecido → erro **0x73** ("Invalid animation type specified in
  animation string"); nome vazio → erro **0x43**.
- **Animação que não existe não é erro**: a busca acima falha em silêncio e
  fica com o último nome montado; o pedido de animação (vtable `0x2E0`)
  volta nulo, o Sim loga "missing animation tree" e a primitiva **devolve
  verdadeiro na hora**, sem duração (o mesmo no base 1.0, que tem a mesma
  string). Só um nome sem prefixo de tipo válido vira 0x73.

A tabela e o prefixo vêm de `DetermineAppropriateAnimTable` (BV `0x68f7c8`),
pela espécie (dado de pessoa 186, só BV; 0 = humano no base) e a idade
(**dado 58 "Person Age"**):

| idade (dado 58) | `0x81` (global) | prefixo `0x83` |
|---|---|---|
| ≥ 51 | `0x8B` idoso (reserva: `0x81`) | `e2` |
| 19–50 | `0x81` adulto | `a2` |
| 16–18 | `0x8A` adolescente (reserva: `0x81`) | `t2` |
| 3–15 | `0x82` criança | `c2` |
| 2 | `0x89` bebê que anda | `p2` |
| < 2 | `0x91` bebê | `b2` |

`0x83` é a tabela de **animações entre dois Sims**: o prefixo é
`<minha idade>2<idade do outro>` (`GetAnimPrefixStart`/`End`: o outro é o
Sim da interação; se não for um Sim → erro 0x5A), por exemplo `a2a` +
a entrada. Na reserva, a idade do Sim vira `a` (adulto).

**O "nome vazio" do "Interaction - Standard Reaction - SG"/"Wave" na nossa
VM** vem de ler a entrada no lugar errado: a árvore é de um semi-global e
a tabela (byte 6 ≠ 0x80) tem de ser procurada no objeto do stack object e
depois no semi-global dele, com o prefixo de idade; para a tabela global
(0x80/0x81) a tabela certa depende da idade (um adolescente/idoso usa
0x8A/0x8B e cai para 0x81 se não achar).

## 4c. Rodada 7 (VM): relógio, meia-noite, objetos fora do mundo, Make New Character

### Relógio e meia-noite (`cTSSimulator::SimulateOneTick`, BV `0x49a814`)

A cada tick que avança o relógio (segundo `+0x30` += 2):

```
segundo > 59 → segundo −= 60, minuto (+0x2e) += 1, ObjectModule.MinuteChanged
               mensagem 0x88 (minuto)
minuto > 59  → minuto −= 60, hora (+0x24) += 1, ObjectModule.HourChanged, flag "hora nova" (+0xdc)
               mensagem 0xD08962D0 (hora)
hora ≥ 24    → hora −= 24, dia (+0x5c) += 1, dia do mês (+0x26) += 1,
               dia da semana (+0x68) = dia mod 7, ObjectModule.DayChanged,
               um gerente diário (vtable 0x24 do sistema), histórico de 5 dias de
               tokens de inventário deslocado (o de hoje zera), negócios (só BV)
               dia do mês > 30 → 1, mês (+0x32) += 1 (> 12 → 1, ano +0x34 += 1)
               mensagem 0x89 (dia)
hora nova    → ComputeTimeOfDay (vtable 0xc0) e SetTimeOfDay (0xc4): a luz do dia
depois       → ObjectModule.PostSim (vtable 0x64) com "tick sem avanço"
```

`cEdithObjectModule` (vtable `0x68/0x6c/0x70`):
- **MinuteChanged** (BV `0x5250b8`): para cada Sim da lista de minuto,
  `PersonalTimeMinutePassed` (só decrementa o dado de pessoa 189 até 0, BV).
- **HourChanged** (BV `0x525144`): `PersonalTimeHourPassed` de cada Sim
  (dado 172, BV).
- **DayChanged** (BV `0x525a60`): `DayPassed` de **todos os objetos**:
  `cTSObject::DayPassed` (BV `0xb07a4c`) = `UpdateAge` + depreciação
  (`docs/buy-build-mac.md` §2) e contador `+0x140` −1 por dia;
  `cTSPerson::DayPassed` (BV `0xad133c`) = dado 200 −1 (até −10, BV) +
  o do objeto + `UpdateMemories`.

Nada no C++ da meia-noite toca motivos, empregos ou carona: **é SimAntics**.
As árvores leem o relógio (globais de hora e dia) e os controladores rodam
no Main. **Carona e ônibus** (dados): o semi-global de carreira do grupo
`0x7FBE051B` tem "CT - Get Carpool Time" (`0x200D`), "CT - Is Neighbor at Work"
(`0x200E`), "Function - Create Car" (`0x200A`), "Function - Is Job Car
Here?" (`0x201B`), "Sub - Return to World" (`0x201D`) e "Sub - Process"
(`0x201F`). O grupo `0x7F8F4EB6` tem "Sub - Carpool Warnings" (`0x2027`) e outro
"Function - Create Car" (`0x200B`). O carro é um objeto criado pela árvore
(Create Object) que chega pela rua (portais de carro, `docs/primitives-mac.md`
§5 "Portal - Car - …"). O Sim sai do lote pelas mesmas árvores. O motor só
precisa do relógio, de Create Object, das rotas e dos portais de carro.

### Objetos fora do mundo (`cEdithObjectModule::MakeNewOutOfWorldObject`, BV `0x533fb8`)

É o mesmo caminho para Create Object com "fora do mundo", para a compra no
modo compra (vtable `0x40` do módulo, ver `docs/buy-build-mac.md`) e para
objetos de inventário:

```
seletor → ConstructObject (vtable 0x3c) → AddObject (0x38) dá o ID
od 19 "lead tile object ID" := o próprio ID
OnInit(param)            ← a árvore init (entrada 0), com o parâmetro pedido
OnPostMultiTileJoin, JustBorn
se não "sem reset":  Reset(1,1,1,0) e **od 41 "current value" := OBJD campo 18 (preço)**
flag 0x80 ligada (vtable 0x68), sim flag do módulo (0x128)
multi-tile: cria cada parte do grupo (o mestre no índice 0), liga as partes ao líder,
            init em cada uma; uma parte que falha mata o grupo (KillOutOfWorldObject)
```

O objeto **não tem posição** (`IsInWorld` falso). Os erros de VM 0x34, 0x7B
e 0x96 (apêndice A) são os testes de "fora do mundo". Ele só entra no mundo
com `SetPosition`/colocação. `KillOutOfWorldObject` (BV `0x532d08`) tira do
mundo o que estiver dentro (vtable `0x2ac`), roda o cleanup e chama
`RemoveObject`, recursivo nos objetos contidos e nas partes. Room: um objeto
fora do mundo não tem tile. O "room −1" dos saves é a posição (−1, −1) do XOBJ
(`docs/save-mac.md`). O que o od 29 "room" vale nesse caso não foi lido
(aberto).

### Make New Character (0x13, `TryMakeNewCharacter`, BV `0x65cb14`)

**Assíncrona**: pede ao CAS (`nTSCASCore::CASSystem`) a geração e guarda os
IDs dos pedidos numa lista do elemento da pilha:

- **bebê** (byte 9 bit 0x01): pais pelos dois operandos (ID de objeto, ou
  neighbor ID com 0x02/0x04). O número de bebês vem do temp 0 do controle (1–10,
  senão o erro **0x76**). Os pais precisam ter sexo diferente ou o flag 6.
  Faz um pedido `CASSystem 0x18(pai, mãe, roupa)` por bebê, posta a mensagem
  `0xAD90F890` (nascimento; o byte 13 bit 0x01 a cala) e devolve **2**.
- **aleatório** (sem 0x01): idade = local do byte 5 (≤ 0 → sorteio de 1 <<
  rand(6)), sexo = local do byte 6 (≤ 0 → 50%), cor da pele = local do
  byte 18 (< 4). Com 0x40 vem o GUID de um "bin" de personagens (com 1 =
  "EP2 Downtownie Bin", só BV), e um bin com vários sorteia um. O pedido é
  `CASSystem 0x14(idade, sexo, pele, local do byte 4, GUID da roupa, 7, bin)`.
  Devolve 1 se o pedido foi aceito (−1 → falso).
- **De novo no tick seguinte** (o elemento marca "começado", vtable `0x48`):
  percorre os pedidos (`CASSystem 0x30(id, &pronto)`). Algum pendente → **2
  (espera)**, todos prontos → 1 (verdadeiro).
- Erros: 0x15 (sem stack object), 0x6C (sem pilha), 0x7A/0x94 (GUID de
  roupa inválido / de pet), 0x08 (local fora), 0x9E, 0x37 (pai/mãe
  inválidos).

O Sim pronto entra pelo `cTSNeighborManager::AddNewCharacter` (BV
`0x5386dc`) e vira um objeto fora do mundo como acima. **Para nós**: sem CAS,
basta criar o personagem na hora a partir de um modelo e devolver 1. O
resultado "2" só importa para quem espera o nascimento.

## 4d. Rodada 8 (VM): carona, trabalho e volta

### Slots nos ossos do modelo (`cTSRoute::GetPossibleSlotsFromModelRoutingSlot`, BV `0x475b98`)

Um slot de rota com a flag `0x20000` no u32 depois das proximidades
(`Slots::Slot::unknown10`) fica nos **ossos** do modelo: o primeiro u32 extra
do SLOT do TS2 é o índice no STR# 0x90 ("Model - Bones"), cuja entrada pode
listar vários ossos separados por vírgula (o "Multislot" do sedã). O binário
faz um objetivo por osso (contagem pela vtable `0x140` do objeto, cada ponto
pela `0x148`) e dá a cada um a direção **do objeto + a do osso**:
`objDir + (8 − k) mod 8` (k da vtable `0x14c`, convertido por
`FUN_008b6334`), seja qual for o campo de direção do slot. O `TrySnap` faz o
mesmo para um slot com `0x20000` (sem ela usa o campo do slot, e só se o slot
fixa a direção; senão fico com a minha). Aqui (`vm/slot_bones.cpp`): k = o
eixo +Y do osso na pose de repouso girado em Z, em oitavos de volta no
sentido anti-horário (inferido da conversão). Assim o carro da carona, que
anda com Snap no seu próprio slot 1 ("slot_travel_drive", 2 tiles à frente),
mantém o rumo; antes virava para o centro (direção "para o objeto") e ia e
voltava no lugar. Um objeto multi-tile que faz Snap leva as outras partes
junto (inferido).

### Snap: o código no object data 52

`TrySnap` (BV `0xe43624`): se falhar, od 52 := 0; o primeiro erro da lista
de colocação é 0x0C ("Can't intersect other objects") e havia uma pessoa
com a flag 0x02 → pede para ela sair, od 52 = 1; senão od 52 = **código do
erro + 2**: 0x0E para objeto no caminho, **4 para "Location out of bounds"**
(código 2). O "Drive - Drive Leave" do carro testa od 52 = 4 depois do Snap
de teste no slot 0 ("slot_travel_start", 8 tiles à frente) para saber que
chegou ao fim da rua; com 0x0E o carro ficava parado na borda.

### Find Location For fora do mundo e ao longo de um vetor

Modo 1 (fora do mundo): o binário chama a vtable `0x2ac` do objeto ("tirar
do mundo"), que também o tira do contêiner. O "Sub - Send Passengers Out of
World" do carro repete enquanto houver alguém nos slots: sem isso era um laço
infinito. A sala (od 29) de um objeto fora do mundo é **−1** (todos os
objetos fora do mundo dos lotes salvos têm −1; vale também para Create New
Object Instance com "fora do mundo"); o "Get Out TEST" do carro, que a "Sub -
Return to World" empurra, testa sala < 0. Modos 3/4 (ao longo do vetor /
lateral): `TryFindGoodLocation` entrega a busca à vtable `0x2d8` do objeto
com os parâmetros (não lida); aqui a máscara de direções proibidas vale
**relativa à direção da referência** (inferido): o carro sai do portal "Car -
Start" (só N, `0xFE`) virado para a rua como o portal, e a poça nova (`0xAA`)
fica num lado que o laço "girar até ficar reto" da árvore alcança. Com a
leitura absoluta o carro saía virado para o oeste, atravessado na rua.

### Set to Next 18 (GUID nos temps) e a escola particular

BV (`TrySetToNext` `0xe47288`): os tipos 4 e 0x12 percorrem a lista de
**instâncias da definição** do GUID; com valor atual ≠ 0 procuram o atual
nessa lista e seguem do próximo; se o atual não está na lista, **não acham**.
Jogo base 1.0 (`cTSObject_TrySetToNext` `0xb577b0`): o tipo 0x12 é outro
caso, que anda na lista geral de objetos do lote a partir do objeto atual
(comparando o GUID da definição, campo `0xca`); o tipo 4 é como no BV. O
"Sub - Is Stack Obj my Student?" dos JobData de escola faz Set to Next 18 a
partir do **aluno** (stack object) procurando o JobData da escola dele (dados
de pessoa 107/108). No BV não acha nunca; no 1.0 só acharia se o JobData
viesse depois do aluno na lista do lote, e os lotes salvos estão em ordem de
ID (o JobData particular do N001_Lot11 é o 70, o Alexander o 142). Depois
disso a árvore só aceita a escola pública (Test Object Type `0xB58867D0`)
para quem tem 107/108 = 0. Então, com esse estado, o jogo também não manda o
Alexander (escola particular) para a escola: não é um erro nosso. A VM segue
o BV.

### O dia de trabalho (só SimAntics)

Nada disso precisa de C++ além do que está acima: às 07:00 o "Sub - Process"
do JobData avisa, às 08:00 cria o carro (motorista NPC no slot), o carro
anda pela rua de Snap em Snap até parar perto do portal "Car - Stop", buzina,
o Sim recebe "Ir Trabalhar", anda até o carro, entra ("o-policeCar-getIn"),
o "Interaction - At Work" atualiza o desempenho (dado de pessoa 63) e põe o
Sim fora do mundo quando o carro sai do lote. O Sim fica esperando no "At
Work" (Wait For Notify), fora do mundo, com a hora passando no lote. Na hora
do fim do turno o JobData chama "Sub - Return to World": cria o carro, empurra
"Get Out" (sub-fila) e o carro traz o Sim de volta à calçada; o "Sub - Get
Paid" põe o salário nos fundos da família (globais de orçamento → FAMI). Não
há "trabalho fora do lote" em C++: o que acontece enquanto o Sim está fora
é a espera da árvore.

## 4e. Rodada 8: o Sim no trabalho e as contas (árvores dos dados + binário)

Não há código C++ para isso no binário: o trabalho e as contas são árvores.
Nomes pela lista de BHAVs (`objects.package`); donos de dados como na FreeSO
(`0x0E` meus motivos, `0x12` meus person data, `0x19` local, `0x1A`
constante de ajuste).

**Sair do mundo**: `Sub - Send out of World` (BHAV `0x2011` do grupo
`0x7F8F4EB6`, o semi-global das carreiras) é um **Find Location For no modo
1** (STR# `0xEF` #1 "out of world"): o Sim sai do mundo como os objetos de
§4c (sem tile; testes de "fora do mundo" do apêndice A). O carro tem
`Sub - Send Passengers Out of World` (grupo `0x7F4EA230`).

**Enquanto está fora** roda `Interaction - At Work` (`0x200E`, 54 instruções),
um laço por hora de jogo (`Function - End Hour?` `0x2002`, teste da hora de
saída; não lido em detalhe): `Sub - Work Idle` (`0x2039`), motivos do
trabalho (`Sub - Start Motive Deltas` `0x2015`, `Check Motives` `0x2023`),
carta de sorte (`Sub - Chance Card` `0x2003`), doença (`0x2026`), aspiração
de poder (`0x2038`), `TNS - Bad Mood` (`0x2012`) e **`Sub - Update Job
Performance`** (`0x205A`): `local0 = meus motivos[3] / 2` (dono `0x0E`, dado
3; inferido: humor), **person data 63 "job performance" += local0**, preso em
−100..100. No fim do turno: **`Sub - Get Paid`** (`0x2004`: salário do nível
pela `Budget - Make Transaction` global `0x0294`, PTO `0x201F`, promoção
`Sub - Level Change` `0x203F` quando `Function - Qualify for Level?` `0x2005`
passa, demissão `CT - Lose Job` `0x2025` com desempenho muito baixo) e
**`Sub - Return to World`** (`0x2016`: cria o carro, `Function - Create Car`
`0x200B`, empurra a interação de chegada no Sim, tenta trazer amigo
`0x202C`; falhando, `Sub - Return to World - Failsafe` `0x2063`). Depois:
recálculo dos desejos (`Wants - Refresh - Recalc`, global `0x03A5`).

**Contas** (objeto `MailBox`, grupo `0x7FE4B434`, e `Bills`, grupo
`0x7FBD8997`; rodada 9, árvores lidas por inteiro):

- **Quando.** O `Functional - Main` da caixa dorme 200 ticks (global
  `0x118`) e roda `process` (`0x1002`) enquanto há casa no lote (global 10
  ≠ 0). `process`: se **hora (global 0) = BCON 0x1000[2] = 9** e **minuto
  (global 5) > 50**: se o **dia da semana (global 34 = Days Running % 7)** é
  BCON 0x1001[1] = **1** ou [3] = **3**, roda `Bill due?` (`0x102E`, que
  procura a caixa `0x39CCF441`, chama `Appraise lot value` e sempre devolve
  verdadeiro) e então `Create Mailman` (`0x1004`: NPC carteiro pelos globais
  `0x2EC`/`0x2E4`), que traz as contas. No dia [6] = **6** nada. Nos outros
  dias, só se existe um objeto agendador `0xD840F488` com atributo 7 = 3
  (o que `CT - Reschedule Bills` `0x1017` cria, com atributo 1 = dias de
  atraso + 1). Ou seja: **contas duas vezes por semana, nos dias 1 e 3,
  entre 9:51 e 10:00**, mais as reagendadas.
- **Quanto.** `Create Bill` (`0x1006`) cria o objeto `Bills` e grava no
  atributo 0 o valor de `Appraise lot value` (`0x1033`):
  **4 × global 46 (valor do lote / 1000) + 6 × global 48 (valor dos objetos
  / 1000)** (BCON 0x1000[5] = 4 e [6] = 6 da caixa); valor 0 → a conta é
  apagada. Os globais vêm do `cTSSimulator`: `SetLotValue` (`0x498b80`) grava
  `+0xA4` e os globais 46/47 (÷1000, resto); `SetLotObjectsValue`/
  `SetFamilyObjectsValue` gravam `+0xA8`/`+0xAC` e `UpdateTotalObjectsValue`
  põe (`+0xA8` + `+0xAC`) ÷ 1000 e o resto nos globais 48/49. São os
  campos do SIMI (`docs/save-mac.md` §5) e os globais também vão salvos;
  quem chama esses `Set*` é o `cTSLot::ComputeAndStoreLotData` (rodada 10,
  `docs/neighborhood-mac.md` §11.1) com o `cEdithObjectModule::ComputeStats`
  (BV `0x52c638`), na virada do dia e quando o lote muda. O opensims2 refaz os
  globais 46–49 e o SIMI +0xA4/+0xA8/+0xAC (`vm::computeLotData`) ao
  carregar, na meia-noite (depois da depreciação), ao comprar, vender e
  apagar. Pelo decompilado, o `ComputeStats` soma o dado 41 de objeto líder no
  mundo (fora tipo 7 e o bit 0x400 do campo 40): `IsDeletedByEvict` (modo
  compra, campo 0x45 = 0, sem o bit 0x10 do dado 43) → família; senão objeto
  do modo construção, ou tipo 0xB/0xC (+ corrimão, não feito) → lote.
  `N001_Lot11`, 3 dias (`s2pkg autonomy --ticks 130000`): conta de 466 no dia
  1 às 10:18 (globais 46 = 4, 48 = 75) e de 400 no dia 3 (48 = 64, depois de
  duas depreciações).
- **A constante `0x81`** (dono `0x1A`: tabela 1 = BCON privado 0x1001,
  entrada 1) do `CT - Calculate Bill` (`0x1017` das contas, Σ valor atual
  / 10 dos objetos sem o bit 14 do dado 40) vale **10**, mas essa árvore é
  **código morto**: o `Function - Init` das contas só a chama na instrução
  7, que nenhuma outra alcança. O Init grava no atributo 1 o BCON 0x1001[0]
  = 4 (prazo em dias, inferido) e as cores (`CT - Set Bill Colour`)
  seguem o atraso.

## 4f. Rodada 11: comida e fogo (árvores dos dados + binário)

Donos como em §4e; flags das expressões contam de 1 (flag 6 = bit 5 =
0x20). "stk.slot[n]" é o dono 0x10 (objeto no slot de contenção n do stack
object); "stk.semi" o dono 5 (semi-atributos do stack object, `+0x84` do
objeto no binário).

### A cadeia da refeição e os balões de falha

`Food - Eat - _Core` (`0x2009` do semi-global da comida, grupo
`0x7F3C9675`): `Food - Eat - Prepare` (`0x2008`) falso → **`Food Chain -
Failure - Prepare`** (`0x205B`); verdadeiro → `Wait For Notify` → `Food -
Eat - Cook` (`0x200C`) falso → **`Food Chain - Failure - Cook`** (`0x205C`).
Então **"Failure - Cook" é a falha do passo de cozinhar** (fogão, micro-
ondas, forno elétrico), não do de preparar; os dois só mostram balões
(`Food Chain - Failure Balloon` `0x2059`, pelo tipo da comida nos semi-
atributos 5 e os BCONs `0x2181…0x218A`).

Os dois passos fazem o mesmo antes do Find Best Object (stack object := 0):
**temp 0 := tipo da comida** (preparo: semi-atributo 4 da comida; cozinha:
semi-atributo 0x22 & temp 2 dos tipos do `Function Test - Cook - Fridge`)
e **temp 1 := ID da comida** (a bandeja), depois FBO função 0 (preparo) ou
1 (cozinha) sem flags e `Run Functional Tree` da mesma função no achado. Se
a comida já tem um objeto preferido (semi 15 no preparo; semi 0x1E com
`Validate Preferred Cook Obj` na cozinha), vai direto nele. Como o check
tree recebe **os meus temps como parâmetros** (§4 FBO), param 0 = tipo,
param 1 = ID da comida.

**`Function - Prep - TEST`** (`0x200D` do semi-global dos balcões,
`0x7F12A804`; o processador de alimentos tem o seu) devolve verdadeiro só
com: atributo 0 do balcão ≠ BCON `0x2202` e não é peça de canto (`Test -
Is Corner?`: od 0 "graphic" ≠ `0x2382`); param 1 > 0 (param 1 < 0 ou = 0:
modo consulta — grava os tipos que sabe preparar no temp 2 com o operador
0x12 e devolve **falso**, por isso o `Function Test - Prep` vê falso de um
balcão livre); `In Use?` falso (semi-atributo 3 ≠ 1); param 0 igual a um
dos 10 tipos (BCONs `0x2081…0x208C`); sem eletrodoméstico embutido (`Slot
Test - In Counter?`: atributo 0 = `0x2200` e algo no slot 4); e os slots
livres pelo `Slot Test - Standard?` (slot 0 e 1, e 2–4 se o atributo 0 =
`0x2201`): **qualquer objeto nos slots de cima do balcão o descarta**, a
menos que seja a própria comida no slot 1 ou uma bandeja de ingredientes
(`Get in Temp 0 - Ingredients Tray`, que aceita a bandeja da comida). Daí o
laço do soak: cada falha deixa a bandeja num balcão, e esse balcão sai das
próximas buscas (além de bloquear o chão, quando cai lá).

**`Function - Cook - TEST`** (`0x2001` do semi-global dos fogões
`0x7F033984`) devolve falso se `Is Power On?` falha (**global 37 "Utility
Available Flags" flag 2** = luz cortada, posta por `Utility - Turn Off
Power` e `Power Failure` das contas), em modo consulta (param 1 ≤ 0),
`In Use?` (semi-atributo 3 do fogão = 1) ou com fogo no cômodo (`Test -
Fire in Room?`: algum Fire no mesmo od 29); senão confere o tipo (param 0)
contra os bits de cozimento (BCONs `0x2082…0x2088`) e o tipo do aparelho
(semi-atributo 1 = `0x2100…0x2102`), e exige, conforme o tipo, o slot 0
do aparelho livre e, se há algo no slot 1 ou 2 (panela, assadeira), que o
slot 0 **desse** objeto seja a própria comida (param 1); verdadeiro devolve
os tipos no temp 2. **Uma panela ou bandeja esquecida no fogão com outra coisa dentro
descarta o fogão.**

Para depurar no VM: além dessas árvores, o binário tira do FBO (§4) objetos
ocupados (od 8 bit 5), pegando fogo (od 8 bit 9), com od 87 ou od 82 bit
2, e fora dos cômodos acessíveis do Sim; o VM faz todos desde as rodadas 11
e 12 (§4g).

### Fogo (`Fire`, grupo `0x7FD0A790`; só SimAntics)

O motor só guarda flags: **pegando fogo** = od 8 bit 9 (0x200, flag 10;
`cTSObject::IsBurning`), **à prova de fogo** = od 8 bit 11 (0x800,
`IsFireproof`), **pode queimar** = od 40 bit 5 (`CanBurn`, flag 6 do od 40
nas árvores). Tudo o mais é árvore:

- **Início** (`Main - Setup` `0x1004`): o Fire nasce no tile do objeto (od
  11), acorda o cômodo, soma a inflamabilidade (od 84 dos objetos em volta)
  no atributo 3 → atributo 1 = 0–3 (< 40, < 70, …); chama =
  BCON `0x1003`[1] (80) + (200 − 80) / 4 × atributo 1 + aleatório 0–7 − 7
  (73–170); marca o objeto (od 8 flag 10) e desliga as interações dele;
  `CT - Fire - Start` no objeto (`stk.od[19]`) e, se ainda não havia fogo no
  cômodo, no `Controller - NPC` (só grava o dia: `stk.attr[12] := global 1`);
  câmera; empurra **Pânico** em todos os Sims que não estão ocupados;
  `Notify - Fire Alarm` acorda (Notify out of Idle) os **alarmes de fumaça
  do mesmo cômodo** (od 29).
- **Laço** (`Function - Main` `0x1000`): a cada `Idle(3)` o atributo 4 +=
  3; `Burn - _Simulate` a cada 20 acumulados: idade (atributo 0) += 1,
  contador de propagação (atributo 5) −= 1 e **chama (atributo 2) −= 1**
  (`Burn - Decrease Flame`; ≤ 0 apaga; com a flag 0x502 apaga já em ≤ 35):
  ~1 ponto a cada 20 ticks, **uma chama de 170 dura ~3.400 ticks ≈ 1,9 h
  de jogo**. Com chama > 10, atributo 5 < 0 e idade > 45: 1 chance em 8
  (`Random` com o BCON 0x1003[6] = 8) de **propagar** (`Burn - Spread`:
  novo Fire num vizinho inflamável), atributo 5 := 10. Idade > 75 sem a
  flag 0x505 → `Burn - Destroy` (o objeto queima: troca por cinzas, mata o
  conteúdo dos slots que pode). A cada 30 voltas empurra Pânico de novo e
  seca poças.
- **Apagar** (`Interaction - Extinguish - TEST` `0x1012`): nunca criança;
  bombeiro (dado de pessoa 155 = o tipo NPC do BCON `0x5910`) sempre;
  outros: falso para visitantes (dado de pessoa 32 ≠ `0x4200`) e **falso
  quando param 0 = 1 (autônomo, inferido)**; com a flag 0x502 do fogo
  também falso. Ou seja, **os Sims da casa só apagam por ordem do
  jogador**; sozinhos só entram em pânico.
- **Bombeiros**: o `Alarm - Smoke` (`0x7F97EA32`), acordado pelo fogo do
  seu cômodo, roda `Sub - Call Fire Fighter` (agenda o NPC 0x5910 se não há
  um no lote); fora isso, o telefone. Sem alarme no cômodo do fogo, nenhum
  bombeiro vem.

Então o "Fogo!" de 7–8 h do soak (`N001_Lot49`) é o esperado de um lote
**sem alarme no cômodo** e sem jogador: cada Fire dura ~2 h e se propaga
enquanto houver vizinhos inflamáveis. A conferir no VM: alarme no cômodo,
`Notify out of Idle` acordando o alarme, e o agendamento do NPC.

### Rodada 14 (front "engine-re"): pânico, fogo do fogão e quem pega fogo

Lido nas árvores (`tools/datahunt/bhavs.py`; nas listagens de assinatura ≥
0x8007 o alvo `0xFFFD` é verdadeiro e `0xFFFE` falso). Constantes `dono
0x1A`: dado >> 7 escolhe a tabela (< 64 privada `0x1000 + t`, < 128
semi-global `0x2000 + t − 64`, senão global `0x100 + t − 128`), os 7 bits de
baixo a entrada.

**`Interaction - Panic` (`0x1013` do Fire) tira o Sim de perto do fogo.**
Roda no Sim com o Fire como stack object:

1. Fora do lote (od 29 < 0) devolve verdadeiro. `Memory - Add`, `Want
   Satisfy - Fire`. **Pegando fogo** (od 8 flag 10) → `Panic - On Fire!`.
2. **Criança de colo** (`Age - Am I a Toddler?`): só vira para o fogo e
   toca a reação; não foge.
3. Os outros: guarda o od 17 (estilo de andar) num local e põe **od 17 := 1
   (corre)**; com o dado de pessoa 32 igual à constante global `0x104`[0]
   (0x4200; inferido: morador) faz `Go To Routing Slot` do **slot global 9**
   (`SLOT` global `0x64`: alvo, 2–5 tiles do fogo, ótimo 2, flags `0x55` =
   as quatro diagonais/retas pares, resolução 1); senão o **slot 1 do
   próprio Fire** (3–6 tiles, ótimo 3, flags `0xff`, resolução 0,5). Os dois
   com a flag 0x08 (permite outra altura).
4. A rota falhou: `Get Distance To` o fogo; a < 10 tiles ainda vira para ele
   e entra em pânico ali, a ≥ 10 não vira. Depois devolve o od 17, zera os
   dados de pessoa 27/28, empurra `Push - Save Baby/Toddler` (bebês e
   crianças de colo do cômodo) e entra no laço: uma de quatro animações de
   pânico ao acaso (`Random 8`), `Divide Tick Counts`, o dado de pessoa 28
   somando o tempo, até passar de **`Tuning - Panic`[0] = 15**; `Wait For
   Notify` entre as voltas; se pegou fogo no meio → `Panic - On Fire!`.
5. O Fire empurra o pânico de novo (`Push - Panic - All Sims Not
   Interacting`, `0x102c`) no começo e **a cada 30 voltas do laço** (`Idle
   3`), para todos os Sims do lote que não estão numa interação (com os
   filtros de idade/NPC do `0x102c`); com `Hurry - Start`.

Ou seja: **sim, o pânico afasta o Sim** (correndo, 2–6 tiles) antes de
entrar em pânico parado. TODO do opensims2: conferir num soak que o `Go To
Routing Slot` global 9 / slot 1 do Fire leva os Sims para longe (o `SLOT`
global com flags de direção e resolução) e que o od 17 volta.

**Quem pega fogo** (`Function - Main` `0x1000` do Fire):

- **Sims no tile do Fire** (`Main - Set on Fire - Sims on Tile`, `0x1051`):
  a cada volta do laço (a cada `Idle 3`), toda pessoa no tile do fogo ganha
  od 8 flag 10 (pegando fogo), sem sorteio.
- **Um Sim vizinho** (`Main - Set on Fire - Sim on Adjacent Tile`,
  `0x1040`): só uma vez, no começo do Fire: `random(100) == 1` (o `Tuning -
  Burn`[7] = 100), isto é **1%**, um Sim ao acaso da lista de vizinhos.
- O Fire liga a sua flag 2 e desliga a 5 do od 8: `CanIntersectPeople` falso
  (o teste por passo não deixa entrar no tile do fogo; §6.1 do
  `routing-mac.md`).
- **`Panic - On Fire!`** (`0x102a`): câmera, efeito de fogo no corpo pela
  idade, motivos (`Tuning - On Fire!`: −200/−90 por motivo), contador local
  = aleatório entre **5 e 10** (`Tuning - On Fire!`[16..17]) voltas de
  animação; acabou o contador ainda pegando fogo → zera os motivos 5–9, 14,
  15 e **`Kill Sim`** (morte por fogo). Alguém apagar o Sim (a flag 10
  limpa) para o laço. Bebês/crianças de colo têm ramo próprio.

**Fogo do fogão: a chance por cozinhada** (semi-global da comida
`0x7F3C9675`):

- `Get in Temp 0 - Skill (Hi/Med/Lo)` (`0x2057`): a habilidade de cozinhar
  do Sim em 0–100 (`Sub - Get Skill Based Value`), **≥ 80 alta, ≥ 40 média,
  senão baixa** (`SG - Tuning` `0x2008`[6] = 80, [7] = 40).
- `Test - Food Failure?` (`0x2045`): falha com **5% (alta), 10% (média),
  30% (baixa)** (`SG - Tuning`[10..12] = 5, 10, 30): `random(100) ≥ 100 −
  chance`.
- `Main - Cook - Loop` (`0x2032`): falhou → o ponto de queima é
  `tempo · (100 − (1 + random(50) + 50))/100` do cozimento (`SG -
  Tuning`[9] = 50); ao chegar nele, `Sub - Set Burnt` e **`random(100) < 20`
  (`SG - Tuning`[15] = 20) → `Sub - Set Fire`** (cria um Fire no aparelho).
  Então por cozinhada: **1% com habilidade alta, 2% média, 6% baixa** (a
  habilidade 0 de quem nunca cozinhou é a baixa).
- Comida pronta esquecida (`Function - Main` `0x2000`): sem ninguém
  interagindo com ela, queima em **10 min** (`Tuning - Cook Failure Time`) e
  **pega fogo com certeza em 30 min** (`Tuning - Cook Fire Time`), contados
  de minuto em minuto (`Idle - Minutes 1`).
- `Function - Cook - TEST` não deixa cozinhar com fogo no cômodo.

**O motor e o fogo** (binário): `cTSObject::IsBurning` (od 8 bit 9, `+0xaa`),
`CanBurn` (od 40 bit 5, `+0xea`) e `IsFireproof` (od 8 bit 11) só são
chamados pela vtable (slots `0x214`/`0x218`/`0x21c` do `cTSPerson`/
`cTSObject`); nenhuma chamada direta. O que se sabe: o FBO tira objetos
pegando fogo (§4); o teste por passo trata o Fire como objeto que não deixa
pessoas entrarem. Nada mais no motor reage ao fogo (sem desvio de rota, sem
alarme, sem bombeiro no código C++): pânico, propagação, mortes, alarme e
bombeiros são todos árvores. (Busca por chamadas virtuais nesses slots:
ambígua, outras classes usam os mesmos deslocamentos; não fechada.)

Hipótese para as mortes do soak (a conferir pela causa da morte): um Fire
dura ~2 h e empurra pânico a cada 30 voltas a todos que não estão em
interação; Sims em pânico repetido por horas não comem nem dormem.

**Conferido nos soaks (rodada 14, sobrevivência 4; `S2PKG_FIRES=1` no
`s2pkg autonomy` imprime cada Fire com o tile e onde cada Sim está ao
trocar de interação, "burning" com a flag 0x200 do od 8).** Em 20 rodadas
de 3 dias (10 lotes que perdiam gente × sementes 1 e 2): 39 mortes, **26 de
fogo** e 13 de fome. Todas as de fogo: o Sim estava pegando fogo e **no
tile de um Fire** (uma a 1 tile, na borda), depois de "Fogo!" — ninguém
pegou fogo fora do tile. Os motivos dessas mortes variam (o Daniel do
`N001_Lot68` tinha todos acima de −60): é o fogo, não os motivos do pânico
repetido. O pânico **afasta**: o "Interaction - Panic" vai ao slot global 9
(SLOT 0x64: direções 0x55, distâncias 2/2/5) do Fire e para a 2 tiles dele
(conferido no trace). Como se morre então: o fogo se espalha (10 a 32 Fires
vivos nos lotes sem detector, horas a fio) e o pânico seguinte é empurrado
para **outro** Fire (o "Push - Panic" escolhe o Fire pelo Set to Next): o
objetivo a 2 tiles dele pode cair no tile de um terceiro Fire, porque o Fire
tem a flag 0x04 do od 8 ("allow object and person intersection") e não
bloqueia nem rota nem objetivo (o `EvalTileForGoal`, BV `0x475074`, testa
ocupação com o `TSObjectCanSetPositionAndOrientation` do Sim, que essas
flags liberam: o mesmo no jogo). Chance por cozinhada: 11 começos de
incêndio para ~170 "Cozinhar" (~6,5%, perto dos 6% de quem não tem
habilidade, mais a comida esquecida: há dos dois casos), não por tick. As
mortes de fome são outra coisa: refeições que falham (a rota ao fridge
interrompida por reações empurradas, "Cheira Mal", "Ficar com Nojo", baratas
"Pisar"), e eram muitas mais com a pegada nova antes de o teste por passo e
o Snap usarem a pegada também (`docs/routing-mac.md` §2).

## 4g. Rodada 12: Sims vivos e destravados (binário + árvores)

### Erros reiniciam o objeto (`cTSObject::HandleError`, BV `0xb05510`)

Fora do depurador, um erro de SimAntics não deixa o Main morto: o
`cTSObject::Error` liga a flag `0x100` de `+0x214` e o `HandleError`, sem
depurador (`+0x44` = 0 e o módulo sem modo debug), chama a vtable `0x534`
(`UnLinkAllInteractions`), avisa o gerente de objetos (`+0x1b4`, vtable
200) e chama a vtable `0x320` com (nenhum, falso): o **`Cleanup(cITSObject*,
bool)`** (BV `0xb09838`; o nome do `symxfer` confere com a descrição do
editor da entrada 3: "chamada quando o objeto vai ser apagado ou quando a
interação de uma pessoa com ele foi abortada"). Com "nenhum" ele limpa o
próprio objeto (rodada 13):
- **pessoa** (`cTSPerson::OnCleanup`, BV `0xadcbcc`, vtable `0x530`, que
  devolve verdadeiro e encerra): limpa a pilha e as interações, tira do
  mundo e põe de volta num lugar livre perto (`0x2d8`), zera o dado de
  pessoa 34, reinicia a árvore no Main e roda a **árvore de cleanup** (OBJf
  entrada 3);
- **objeto com árvore de cleanup** (vtable `0x170` com 3 ≠ 0): roda-a
  (`RunTree(3)`, vtable `0x3b4`) e reinicia a árvore no Main; os dados ficam;
- **objeto sem ela**: tira do mundo, `cTSObject::Reset(1, verdadeiro, falso,
  falso)` (BV `0xb0ab9c`) e volta ao lugar (ou ao slot do contêiner).
  O `Reset` copia os 113 dados de objeto (`+0x9a`), zera todos e devolve
  só 1 direção, 2/3 contêiner e slot (do lugar lido antes), 11 ID, 19 líder,
  21/24/45/46/47 data de nascimento, 29 cômodo, 41 valor atual, 55, 67
  contagem de slots, 79 semi-atributos, 80/81 e 105–107 ligações; põe 71
  (render flags) = 0, 57 e 72–75 (alfa e RGB) = 255, 23 (rotation notches)
  = 2, 58 (versão) e 78 (look at score) = rótulos 49 e 13 do OBJD, e com
  "Use Default Placement flags" (rótulo 12) 4/5/13/42 = rótulos 6/3/5/4
  (alturas, paredes vizinhas, colocação na parede, colocação); 7 vem da
  virtual `0x578`, **`cTSObject::GetDefaultNicenessMultiplier`** (BV
  `0xb00b64`, confirmado na rodada 14): o **campo 45 do OBJD**
  ("NicenessMultiplier" do catálogo), gravado como inteiro (`sth` em
  `+0xa8`). Os atributos só são zerados com o
  terceiro argumento (aqui falso); os 8 temps do objeto (`+0x8a`) e
  `+0x1a0` são zerados. Para um objeto que não é parte de multi-tile
  (vtable `0x19c`) roda o **Init** (`RunTree(0)`), depois a árvore volta ao
  Main.
O jogo grava o `ObjectError` e segue. A VM (`VM::resetAfterError`) faz o
mesmo desde a rodada 14 (sobrevivência 4): o dado 7 é o campo 45 do OBJD, e
a pessoa sai do slot e volta a um lugar livre perto (`VM::placeNearFree`:
onde está se a caixa dela não toca a pegada de nenhum objeto nem outro Sim
a menos de 0,5, senão o centro do tile livre mais perto, no mesmo cômodo
primeiro, até 10 tiles; o `0x2d8`/`FUN_00af87c8` do binário não foi lido:
inferido), o dado de pessoa 34 zerado e a árvore de cleanup rodada; os
dados além do 87 não existem na VM. `S2PKG_NO_RESET=1` mantém o
Main morto para depurar. O orçamento de instruções (20000 por turno, a "Too
many iterations" 0x44d do jogo, cujo limite não foi lido) também reinicia.

### Set to Next: objetos que vão morrer não contam (`SetToNextIsObjectEligible`)

`ValidateObject` (BV `0x1be5460`) recusa objetos com a flag interna
`0x10000` (posta por `AddToKillQueue`/`ProcessObjectForKillQueue`) ou
`0x20000`; o Set to Next e o Find Best Object for Function pulam esses
objetos. Sem isso o "Reaper - Scan Lot For Dead People" achava de novo o Sim
que tinha virado urna no mesmo tick e o Main do Ceifador morria no "Wants
Satisfy - Death of Sim". O filtro 0x02 (dado de objeto u16 9 == u16 11) é
aplicado só em nós de versão 1 (rodada 13: na versão 0 o `SetToNextParam` zera esses bytes no load; lidos como filtro, o "New Baby" do túmulo de depuração não achava nada e ninguém cuidava do bebê em `N002_Lot10`); o 56 bit 0 e o 107 ficam de fora (só BV).

**Tipo 3 (parte de multi-tile)**: o binário exige que o alvo seja parte de
multi-tile (vtable `0x19c`) e anda no grupo (`+0x1c` próxima, `+0x18`
primeira): um grupo que ficou com uma só parte devolve o próprio alvo. O
"Burn - Spread - MT" laça no Set to Next 3 até voltar ao início (falso volta
ao mesmo nó): com o "não há outra parte" a VM estourava o orçamento num
objeto meio queimado (`N001_Lot36`, 51 vezes em 3 dias). A VM toma "é
multi-tile" pelo campo 10 da definição ("Multi-Tile master id"), o mesmo que
a árvore testa.

### Notificação de texto sem stack object (`TryDialog`)

Nos tipos de notificação, o objeto do texto é o objeto **dono da árvore**
do frame (vtable `0x0C` do `cITSTreeStackElem`; o log do `TryKillObject`
chama-o "the tree owning object"), o stack object só se ele não existir; o
erro 0x17 só sem os dois. O stack object só dá o ícone. (A Circe de
`N002_Lot3` morria numa ligação cujo interlocutor já tinha saído.)

### Force Recalc of Wants do Ceifador (neighbor 0)

O "Wants - Refresh All - Verify" chama a Generic Sims Call 39 (op 14) com o
dado de pessoa 31 de cada Sim do lote; o binário dá erro 0x37 se o vizinho
não existe. O Ceifador é criado por Create New Object Instance (sem a flag
0x04) e ficava com o vizinho 0. O bairro tem um vizinho sem arquivo de
personagem para cada NPC único (N002: #1 terapeuta, #34 carrinho, #35
**Ceifador** GUID `0x84EC24A8`, #225 dançarina, #361 polinizador). A VM
liga o Sim criado ao vizinho de mesmo GUID, ou cria um com o próximo ID
(`VM::neighborForGuid`; **inferido**: no binário isso fica atrás de
chamadas virtuais não seguidas). Com isso e o filtro acima, o Ceifador
recolhe os mortos.

### Find Best Object for Function: cômodos acessíveis

`cTSPerson::GetAccessibleRooms` (BV `0xacd218`) pede o conjunto ao roteador
(`this+0xf8 → 0xd4`, functor com a pessoa e o cômodo); a versão antiga
(`GetAccessibleRoomsOld`, `0xad4abc`) é uma busca em largura pelos cômodos
ligados por portais (portas, escadas; com a flag 0x04 do dado 22 testa se a
pessoa pode passar). A VM monta, uma vez por planejador, os grupos de células
livres do mapa estático (paredes, chão, aberturas de portas) ligados pelos
portais, e o conjunto é o dos cômodos do grupo do Sim
(`PathPlanner::accessibleRooms`; **inferido**: objetos, Sims e travas de
portal ficam de fora). Um Sim só olha objetos desses cômodos. Achado com
isso: **portas diagonais** ("Door - 1 Tile - Diagonal") são uma parte só,
na parede diagonal do tile, e o carregador só abria portas com outra parte
atrás: os quartos de cima de `N001_Lot68` e o quarto/banheiro de
`N003_Lot52` estavam fechados.

### Slots de osso de objetos de várias partes

O modelo de um objeto multi-tile é o da parte líder: os ossos dos slots de
rota ficam a partir do ponto e da direção do líder, qualquer que seja a
parte do slot (**inferido**). A escada reta (`Stairs - Straight`) tem os
ossos "route_staircasestraight_*_top" no "Hi 0,0 Pad", 3,7 tiles acima do
líder "Lo 0,4": tomados do próprio pad caíam 3,7 tiles além do topo, o Snap
do "Portal - Do" falhava e ninguém subia (`N001_Lot11`: o "Dormir" falhava
sempre e os Sims dormiam no chão).

### Pedir para sair do objetivo: tipo 1

O `DoBlockedGoalStillAvailable` pede com **tipo 1** (1 a 3 tiles, direções
±45°/±90°) quando a rota tem objeto de destino e o bloqueador está a menos
de 1,5 tile dele (`docs/routing-mac.md` §8.3); a VM só usava o tipo 0 (0,3
tile). O cozinheiro no cooktop saía 0,3 tile e ficava no slot do forno do
mesmo fogão; a outra Sim desistia de tirar a comida do forno (`N002_Lot3`)
e ela pegava fogo 40 minutos depois. Direções tomadas em relação à de quem
pede (inferido).

### Idle for Input dentro de uma interação

Com "allow push", dentro de uma interação a entrada da frente roda **por
cima** da atual quando pode interrompê-la (prioridade maior); quando volta,
a interrompida continua (`TryIdleForInput`, §4). A VM só empurrava fora de
interações. A criança que a assistente social leva fica em "Chorar por Filho
Perdido" (`Interaction - Cancel` do carro, prioridade 50, um Wait For Notify
sem fim com allow push) e só sai pelo "Entrar" (100) da assistente.

### Comida no forno: o que a protege de "Limpar"

Ninguém marca a comida assando como em uso (nem od 8 flag 6 nem o semi 3,
que é "refeição em grupo"). A proteção é a árvore do prato (`Interaction -
Clean Up - TEST` do semi-global `0x7F8834C8`): autônomo, uma refeição
individual intacta (semi 2 == semi 1 da comida) só pode ser limpa se o "CT -
Stats" (nome 42 da tabela global de árvores nomeadas) disser que está
estragada (semi 17 flag 2). Com as datas de nascimento da rodada 11, a
comida nova não está estragada e ninguém mais tira a comida do forno: não
se repetiu nesta rodada.

### Cozinheiros que saem do forno

Com o "Free Will" (global 30, `cTSSimSystem::SetFreeWill`: o booleano das
opções, ligado por padrão) o "Cook - _Type - Bake" põe a comida e sai. A
comida tem dois relógios (BCONs do semi-global `0x7F3C9675`):
- **queima durante o cozimento** (`Main - Cook - Loop`): o "Test - Food
  Failure?" sorteia a falha pela habilidade do cozinheiro (semi 23 = dado de
  pessoa 10 no início, pelo "Sub - Get Skill Based Value"; abaixo de 40 é
  "baixa", 30% de falha; as faixas média/alta usam as constantes seguintes,
  não lidas)
  e, falhando, num ponto do cozimento a comida queima e **20% das vezes pega
  fogo** — esteja o cozinheiro lá ou não;
- **esquecida depois de pronta** (`Function - Main`): "Tuning - Cook Failure
  Time" (0x2011, 10 min) até queimar e "Tuning - Cook Fire Time" (0x2012, 30
  min) até pegar fogo, se ninguém interage com ela.
O "Push - Get Food" chama o cozinheiro com prioridade de usuário (50) assim
que fica pronta. Ele só vem se a interação dele tem prioridade menor que 50
(`ShouldInterrupt`); comendo outra coisa empurrada a 50 ele termina primeiro
(25+ min num pedaço de pizza, 30% de mordida a cada volta). Nada no motor
atrasava além disso, salvo os dois bugs de rota acima (escada e slot do
forno ocupado).

### Change Material sem grupo de malha (`TryChangeMaterial`, BV `0xe2f128`; rodada 13)

O material (STR# 0x88) e o grupo de malha (STR# 0x87) são procurados como
strings preguiçosas (grupo, tabela << 16 | índice + 1, língua): o material
vem do grupo global (`0x7FD46CD0`) com a flag `0x02`, do semi-global do
objeto com `0x04` (sem semi-global: sem nome), senão da tabela do próprio
objeto e depois da do grupo do código; o grupo de malha igual com `0x40`
(global) e `0x80` (semi-global). Material vazio é o erro **0x46**
("Missing Material / Mesh Group Name"); o nome do grupo de malha é
procurado e **nunca lido** (o registrador só volta ao destrutor): faltar a
entrada não é erro. Os arbustos ("Shrub - Creeping Phlox", "Hydrangea")
pedem o grupo 0 de um STR# 0x87 que o grupo deles não tem a cada hora no
"Function - Main" do semi-global `0x7F1A9ECC`; a VM dava erro e os
reiniciava. Agora o material vale para o modelo todo quando o grupo não tem
nome (inferido; no BV o nome nunca é usado). O byte 13 com `0x02`, `0x04`
ou `0x20` pula o material (snapshot, "Moving Texture"; não feito).

### Quem chama os bombeiros (rodada 13)

Ninguém da casa liga sozinho. O "Ligar..." do telefone (`7FB208FA`, TTAB 2,
entrada 0, ação `0x2030`) não tem **nenhum anúncio de motivo** (ads = 0),
então o Find Best Interaction nunca o escolhe; o "Emergency - Fire" do
objeto de chamada `7F0884BA` só roda pelo menu. O "Interaction - Panic" do
fogo (`0x1013`) só anima, empurra "Salvar Bebê" e grava a memória; não chama
ninguém. O motor não tem código de bombeiro (nenhum símbolo no BV). O
único caminho autônomo é o alarme de fumaça **no cômodo do fogo** (`Alarm -
Smoke` → `Sub - Call Fire Fighter`, §4f). Sem alarme no cômodo, o fogo só
termina sozinho (a chama de cada Fire dura ~2 h) ou pelo jogador.

### O carro da assistente social (rodada 13)

O carro (`7FA5FB07`, semi-global `CarGlobals` `7F4EA230`) anda e vai embora
desde a rodada 12 (laço do Animate Object). A criança levada fica em "Sub -
Create Orphan( Kill Kid )" (um Sleep de 250 ticks que volta a si mesmo) até
o "Sub - Remove Vehicle" do carro chamar o "Sub - Send Passengers Out of
World": para esse tipo de veículo (semi-atributo 5 igual a uma constante do BCON, não conferida) os
passageiros são **removidos** com ele. A VM já fazia isso; o "parado em
Levando a criança" do soak era o soak, que não via um Sim sumir (agora o
`s2pkg autonomy` imprime "is gone (removed from the lot)").

## 5. Outras primitivas: operandos pelo disassembler

Resumo do que o disassembler do editor (`cEdithLanguage::Cat*`, mesmo texto
que as strings do `Sims2.exe`, mas agora com o byte de cada campo) e o
`SwizzleParams` dizem. Binário, salvo indicação.

- **0x02 Expression**: esquerda (dono byte 6, dado u16 0), operador byte 5
  (STR# 0x88), direita (dono byte 7, dado u16 2).
- **0x08 Random Number**: destino (dado u16 0, **dono u16 2**), limite (dado
  u16 4, **dono u16 6**): "dest := random from 0 to < limite". Execução:
  **limite 0 grava 0 no destino e devolve verdadeiro** (o erro 0x04 "Random
  number range evaluated to zero" não é lançado aqui; a VM hoje lança);
  destino só leitura → 0x13; resultado fora do intervalo do destino → 0x14;
  um destino float (motivo) também é aceito. Sempre verdadeiro.
- **0x0B Get Distance To**: destino = temp (u16 0); flags byte 6: 0x02 em
  centésimos de tile; distância de mim (ou do objeto do operando) ao stack
  object.
- **0x0C Get Direction To**: destino (dado u16 0, dono u16 2); flags byte 8:
  0x02 em graus.
- **0x0E Find Best Object for Function**: função u16 0 (STR# 0xC9); flags
  byte 2: 0x01 só fora, 0x02 só dentro, 0x04 só no cômodo da referência,
  0x20 fora do cômodo da referência, 0x40 no cômodo do temp 0 (só a primeira
  dessas cinco conta), 0x08 relativo ao objeto (dono 3, dado u16 4), 0x10
  com linha de visão, 0x80 ignorando "lockout" (§4).
- **0x0F Break Point**: (dado u16 0, dono u16 2) "if != 0"; byte 4 bit 0 =
  ignorar no jogo final.
- **0x12 Remove Object Instance**: alvo u16 0 (STR# 0x9C: 0 eu, 1 stack
  object, 2 "target - UNUSED" = ID 0; > 2 → erro 0x09); byte 2: 0x01 volta na
  hora, sem 0x02 "cleanup all" (passado à remoção). Objetos cujo GUID está
  numa lista fixa de 7 GUIDs não são removidos (§4b).
- **0x16 Turn Body Towards**: tipo u16 0 (STR# 0xD8 dos globais).
- **0x1B Go To Relative Position**: lugar s8 2 (STR# 0x82, índice + 3),
  direção s8 3 (STR# 0x83, índice + 3), flags byte 6: **0x02 sem árvores de
  falha, 0x04 permite outro nível**. Contradiz o `routing.cpp`, que usa os
  bits do The Sims 1 (0x01 altitude, 0x02 sem falha).
- **0x1C Run Tree by Name**: flags u16 2 (0x01/0x02 de onde vem o nome,
  0x04 e 0x08 impressos como true/false, 0x10 passa três (dono, dado) em 6,
  9, 12, 0x20 passa os parâmetros da árvore atual); modo byte 5 (STR# 0xDE).
- **0x1D Set Motive Change**: motivo byte 2 ("motive" n), += (dono byte 0,
  dado u16 4) por hora, até (dono byte 1, dado u16 6); byte 3: 0x01 limpa
  todos, 0x02 zera a "Motive Decay" dos dados de pessoa.
- **0x1E Gosub Found Action**: u16 0 (STR# 0x1FE).
- **0x21 Find 5 Worst Motives**: u16 4 (STR# 0x99) e u16 6 (STR# 0xA5),
  resultado nos temps 0–4.
- **0x22 UI Effect** (o que cada modo faz: `docs/tutorial-mac.md` §2): modo s8 0 (0 aperta controle, 1 desliga, 2 liga, 3
  efeito, 4 árvore de evento, 5 reseta todos, 6 desliga todos, 7 reseta
  árvores, 8 árvore de evento em nó TNS, 9 mostra, 10 esconde), controle u16
  3, janela u16 1, flags byte 5, efeito u16 6, árvore u16 8 com o lugar s8 10
  (0 privada, 1 semi-global, 2 global), (dono 13, dado u16 11).
- **0x23 Camera Control**: byte 4: 0x01 mostra o stack object, 0x08 centra,
  0x20 timeout no temp 0 (senão u16 0), sem 0x40 "slow down"; zoom byte 3
  (1 longe, 2 médio, 3 perto).
- **0x24 Dialog**: tipo s8 5 (0 mensagem, 1 escolha, 2 três escolhas, 3
  texto, 4 tutorial, 5–0x46 diálogos especiais: telefone, revistas, roupas,
  aspirações, ...), mensagem u16 13 (STR# 0x12D), temp do texto = (byte 7 >>
  4) & 7, ícone (byte 7 >> 1) & 7 com o índice no byte 1, flags byte 8
  (0x01/0x02/0x40 de onde vem o texto: privado/semi-global/global, 0x04–0x20
  índices de botões e título no temp 0), prioridade byte 9 + 1, timeout byte
  10, tipo de notificação s8 12.
- **0x2D Go To Routing Slot**: slot u16 0; de onde u16 2 (0 parâmetro, 1
  literal, 2 global, 3 "???"); flags byte 4: 0x01 sem árvores de falha, 0x02
  slot no temp 0 (com u16 2 = 1), **0x04 ignora a área do objeto de
  destino, 0x08 permite outro nível**. Resolve o "0x02–0x10 not understood"
  do `routing.cpp`.
- **0x2E Snap**: modo u16 2 (STR# 0xCF); slot u16 0 (modos 0, 3 e 4); flags
  u16 4: 0x02 pede para a pessoa sair, 0x08 slot no temp 0, 0x10 só testa
  ("TEST ONLY"); u16 8 = 1 "from slot in temp 1".
- **0x30 Stop ALL Sounds**: u16 0: 0 os meus, ≠ 0 os do stack object.
- **0x32 Add/Change the Action String**: modo s8 9 (0 texto, ≠ 0 ícone);
  flags u16 2 (0x04/0x08 tabela global/semi-global, 0x10 índice no temp 0,
  0x100/0x200 desligada propagando/não, 0x1000 para a sub-fila, 0x20/0x40/
  0x80 no modo ícone); texto u16 14; ícone: objeto (dono 11, dado u16 12),
  GUID u32 5, índice byte 10.
- **0x69/0x6B Animate Object/Overlay** e **0x6A Animate Sim**: animação u16
  0; flags byte 2: 0x01 espelhada, 0x02 velocidade no temp 2, 0x04 índice num
  parâmetro ("anim id in s.v."), 0x08 interrompível, 0x10 começa na tag do
  temp 0, 0x20 (Sim: "Trans to Idle"; objeto/overlay: repetições no temp 1),
  0x40 sem blend de saída, 0x80 sem blend de entrada; árvore de evento u16 4
  com o lugar s8 7 (Sim) / s8 9 (objeto) / s8 14 (overlay); lista byte 6 (Sim)
  / byte 9 (overlay): 0x80 global, 0x81 adulto, 0x82 criança, 0x83 social,
  0x89 bebê que anda, 0x8A adolescente, 0x8B idoso, 0x91 bebê, 0x8C gato,
  0x8D cão (os dois já existem no exe base 1.0); 0x99/0x9A filhote de cão/
  gato: **só BV**; objeto (dono 6, dado u16 7)
  (objeto/overlay). Sim: flags byte 8 (0x01 espelhamento no temp 3, 0x02
  sincroniza com a última animação, 0x10 usa o objeto controlador como fonte,
  0x20 não apressável), objeto IK (dono 9, dado u16 10), prioridade s8 12 (0
  baixa, 1 média, 2 alta). Objeto: flags byte 10 (0x01 espelhamento no temp 3,
  0x04 sincroniza com a animação de quem chama, 0x08 alinha o fim, 0x80 não
  apressável). Overlay: flags byte 15 (0x01, 0x10, 0x20 como acima, 0x40 usa a
  lista do dono da árvore), byte 12 bit 0 não apressável, prioridade s8 11.
- **0x6C Animate Stop**: modo s8 7 (0 uma animação: u16 0 da lista byte 6;
  1 overlays, 2 corpo inteiro, 3 todas, 4 poses de carregar, 5 idle, 6
  gestos, 7 reações, 8 normais, 9 faciais, 10 faciais idle, 11
  receptividade); objeto (dono 3, dado u16 4); flags byte 2 (0x01 espelhada,
  0x02 com blend, 0x20 blend curto, 0x08 espelhamento no temp 3, 0x40 para as
  normais e as espelhadas); prioridade s8 8 (0 qualquer, 1 baixa, 2 média, 3
  alta).
- **0x6D Change Material**: material u16 0; byte 2: 0x08 o material é
  procurado no objeto (dono 8, dado u16 9) em vez de em mim, 0x10 índice do
  material no temp 0, 0x02 tabela do grupo global, 0x04 do semi-global
  (rodada 13); 0x01 o grupo de malha é
  procurado no objeto (dono 8, dado u16 9), 0x20 índice do grupo no temp 1,
  0x40/0x80 tabela global/semi-global do grupo (o nome do grupo não é
  usado no BV, §4g); grupo de malha s16 6 (< 0 = o
  modelo todo); afeta (dono 5, dado u16 6); byte 13: 0x01 índice de "Moving
  Texture" em vez de material, 0x02 usa o material gerado por snapshot.
- **0x6E Look At**: flags byte 0 (0x01 parar, 0x80 câmera, 0x02 sem saída
  antecipada, 0x04 inclui a coluna, 0x08 velocidade no temp 1, 0x10 duração
  no temp 0, 0x20 ignora cômodo, 0x40 ignora o frustum); alvo (dono 1, dado
  u16 2); velocidades byte 4 / byte 5 × 2 °/s; slot byte 8 do tipo s8 14 (0
  altura 3/4, 1 alvo, 2 rota, 3 contenção); árvore u16 9 com lugar s8 11;
  byte 15 (0x01/0x02 velocidade de volta nos temps 1/2, 0x04 não apressável).
- **0x6F Change Light**: objeto (dono 2, dado u16 3); luz s8 8 (−1 todas);
  duração u16 5 (ou temp 1 com byte 1 & 0x01); intensidade byte 7 % (ou temp
  0 com byte 1 & 0x02).
- **0x70 Effect Stop/Start**: modo byte 0 (0/1 começa suave/forte, 2/3 para
  suave/forte, 4/5 para todos, 6 dispara e esquece, 7 interroga o osso, 8
  para tudo e limpa a fila, 9 para TODOS, 10–13 estado 1–4, 14 para TODOS
  suave); objeto (dono 1, dado u16 2); efeito u16 4 (byte baixo −1 = o efeito padrão);
  slot byte 6 do tipo s8 9; flags u16 10 (0x01 STR# 0x8F global, 0x02
  semi-global, 0x08 ID do efeito no temp 0, 0x40 temp 1 passado ao início,
  0x80 fila de prioridade, 0x04/0x10/0x20/0x400/0x800/0x1000/0x2000 ícones);
  (dono 12, dado u16 13) = variável do ícone; byte 15 = entrada do STR# 0x95
  (atlas do ícone). Detalhes em `docs/effects-mac.md` §2.
- **0x71 Snap Into**: objeto (dono 0, dado u16 1) no objeto (dono 3, dado u16
  4), slot byte 6 (ou temp 0 com 0x01); flags byte 9: 0x01 slot no temp 0,
  **0x02 só testa ("TEST ONLY")**, 0x04 reseta os ossos raiz. Resolve o
  "0x02 not understood" do `routing.cpp`.
- **0x72 Assign Locomotion Animations**: modo u16 0 (0 tira todos, 1 tira o
  último, n ≥ 2 empilha o grupo n − 1); byte 2: 0x02 semi-global, 0x04
  global (senão privado).
- **0x73 Debug**: ação byte 13 (0 imprime, 1–7 janelas/cheats), quatro (dono,
  dado) em 0, 3, 6, 9, byte 12, flags byte 14.
- **0x75 Age**: idade byte 0 (0 adulto, 1 criança, 2 bebê que anda, 3
  adolescente, 4 idoso, 0xD bebê, 0xE jovem adulto — **0xE só BV**: "Young
  Adult" não existe no exe base 1.0), byte 1 bit 0 = idade no temp 0.
- **0x76 Array Operation**: de quem byte 2 (0 meu, ≠ 0 do stack object);
  array u16 3; operação byte 1 (0 limpa, 1 tamanho, 2 redimensiona, 3
  preenche, 4/5 insere na frente/atrás, 6 insere no iterador, 7/8 tira da
  frente/de trás, 9 tira no iterador, 10 avança o iterador até o valor, 11
  troca, 12/13 ordena); (dono 5, dado u16 6) e (dono 8, dado u16 9).
- **0x77 Message**: ID da mensagem (dono 15, dado u16 1); prioridade byte 8;
  alvo: com byte 4 & 0x02 um alvo (dono 5, dado u16 6), senão o conjunto byte
  3 (0 Sims selecionáveis, ..., 6 objetos, 7 tudo) e o filtro byte 0 (0 mesmo
  cômodo, 1 mesmo nível, 2 lote, 3 dentro, 4 fora); dados (dono 9, dado u16
  10) e (dono 12, dado u16 13).
- **0x79 Change Outfit**: flags byte 0 (0x10 reconstrói a roupa atual, 0x01/
  0x02/0x40 fonte: GUID u32 4, stack object, temps 0/1; 0x04 índice numa
  variável (dono 1, dado u16 2), senão byte 8 (STR# 0xFA); 0x20 limpa os GUIDs
  dos dados de pessoa; 0x08 grava no arquivo); Sim (dono 9, dado u16 10).
- **0x7C Want Satisfy**: Sim (dono 0, dado u16 1), desejo u32 3; tipo do
  desejo (0 valor, 1 pessoa, 2/3 objeto, 4 habilidade, 5 carreira) com (dono 7,
  dado u16 8) e nível (dono 10, dado u16 11).
- **0x13 Make New Character**: sem byte 9 & 0x01: idade/sexo/cor da pele nos
  locais dos bytes 1, 2, 0; com 0x01: bebê dos pais (dono 6, dado u16 7) e
  (dono 3, dado u16 4), 0x02/0x04 por neighbor ID; 0x08/0x10 dados do objeto
  com GUID nos temps 0/1 / no token; 0x20 personagem do "Bin"; 0x40 GUID
  externo para a roupa da miniatura (0x80: nos temps 2/3, senão u32 10).

## 5b. Conferência com o jogo base (Mac 1.0)

O executável de Mac 1.0 (`/mac/sims2_base_1.0.ppc` no mesmo projeto Ghidra,
auto-analisado) não tem nomes, mas tem RTTI: `paramvtables.py` acha as 64
classes de operando dele, e o `OnTryElement` (0x00B3A8F4, slot do vtable de
`cTSObject`) dá os handlers. Binário (base):

- O despacho é o mesmo do BV para 0x00–0x7C (0x0A também devolve verdadeiro);
  0x7D Follow Sim e 0x7E Run Script não existem.
- `SwizzleParams` dá **a mesma largura de campos** que no BV para todas as
  classes, menos: Set to Next (no base só o GUID u32 0: os u16 9 e 11 do
  filtro "where" são **só BV**) e Add/Change the Action String (o u16 14 é só
  BV); Run Tree (chamada de árvore) muda a conversão de versão.
- Classes com conversão de versão no base: Animate Overlay, Dialog, Look At,
  Relationship e Run Tree. **O BV acrescenta versões novas de Alter Budget,
  Add/Change the Action String, Manage Inventory, Change Material e Set to
  Next**: nessas o BV tem campos que os BHAVs do base (versão 0) não têm
  (zerados no load).
- Conferidos iguais ao BV no código do base: Relationship (inclusive 0x08 e
  "dois Sims = pelos neighbor IDs"), Create New Object Instance (0x20, 0x40,
  0x80, erro 0x36), Test Object Type (`0x4C7CAB2B`, flags 1/2/4), Random
  Number (limite 0 → 0), Find Location For (o modo 0 tenta o mesmo cômodo e
  depois qualquer lugar), Play Sound (tabelas por 10000/20000), Set to Next
  (alvo explícito no byte 4 bit 7; tipos 0–37, sem filtro de espécie).
- Run Functional Tree: a tabela é a mesma, mas no base a função 27 "Global
  Awareness" (→ 0x35) não existe (dá falso).
- Set to Next: nos tipos que percorrem listas (1 pessoa, 4/18 GUID, 3, 16, …)
  **o alvo vira 0 quando nada é achado** (base e BV). Contradiz o "Set to
  Next sem achar não mexe no alvo" do `TODO.md`.
- Strings do editor que só existem no BV (ausentes no exe base): "Young
  Adult", "Kitten list"/"Puppy list", "Scratch-Chew", "including disabled
  objects", "To Sub Queue", "Disabled (NonPropagating)", "Receptivity
  Animations", "Transportation Picker", "Hire Employees", erros 0x98–0x9E
  ("Another cinematic…", "Bad family id", "Invalid species", …).

## 6. `tools/symxfer`: como os nomes chegaram ao binário

O executável só tem ~20 mil símbolos (externos e weak, todos no
`__textcoal_nt`); o `__text` (25 MB, 33.101 funções) não tem nome nenhum.
`references/sims2_mac_decomp/baserom.sym` lista 82 mil funções de **outro**
build ppc do mesmo port (dezembro de 2007), ligado de outro jeito (inlines
no meio do objeto, objetos em outra ordem). O que liga os dois é o
`__eh_frame`: uma FDE por função, com início e tamanho exatos.
`tools/symxfer/symxfer.py` alinha, por seção, a sequência de tamanhos das
nossas funções com a do `baserom.sym` (âncoras: nomes iguais no coal e 4
tamanhos seguidos únicos nos dois lados; cadeias de âncoras; DP entre elas;
funções que mudaram de lugar dentro do objeto; thunks `addi r3,r3,-k; b
alvo` nomeados pelo alvo).

Cobertura (rodada 3): 22.461 das 33.101 funções do `__text` (68%). **Rodada
9**: com as vtables (RTTI) e um segundo alinhamento sobre os nomes já
postos, **27.300 (82,5%)**, `exact` confirmado pelos thunks em 95%; detalhes,
classes de confiança e medidas em `docs/symxfer.md`. A tabela de despacho
(§2) bate com o nome de todos os 40 handlers que ela chama, e cada classe de
operando bate com a primitiva. Há erros isolados mesmo em `exact`: conferir
pelo código antes de confiar num nome.

- `tools/symxfer/apply_clone.sh <pasta>`: clone do projeto com os nomes
  atuais (`docs/symxfer.md` §1).
- `tools/symxfer/run.sh [mapa] [--ghidra]`: gera o mapa (`addr nome
  confiança`, ~5 MB, fora do repositório) e, com `--ghidra`, reimporta a
  fatia ppc em `~/sims2:/mac/sims2_bv.ppc` aplicando os nomes
  (`ApplySymxferMap.java`). `map_simantics.txt` é o recorte da VM.
- `manual.txt`: nomes postos à mão (com a justificativa);
  `paramvtables.py`: vtables das classes de operando pela RTTI (nomes de
  `SwizzleParams`, `UpgradeVersion`, ...).
- `vtables.py`: vtable de uma classe com o nome de cada slot (para resolver
  `(**(code **)(*this + 0x4d4))`); `annotate.py` põe esses nomes no C do
  Ghidra; `DecompileList.java <saída> <padrão|0xEND>...` decompila em lote.

## Apêndice A. Códigos de erro da VM (`cTSObject::GetErrorString`)

| código | texto |
|---|---|
| 0x0 | No Error |
| 0x1 | Bad operator. |
| 0x2 | Temporary reference out of range. |
| 0x3 | Bad parameter to update primitive. |
| 0x4 | Random number range evaluated to zero. |
| 0x5 | Primitive not supported for this type of object. |
| 0x6 | Attribute number out of range. |
| 0x7 | Data number out of range. |
| 0x8 | Stack number out of range. |
| 0x9 | Illegal owner field in data reference. |
| 0xa | Reference to tree tree parameter when no parameter exists. |
| 0xb | Gosub?? |
| 0xc | Reference to target object when none exists. |
| 0xd | Flag reference is out of range. |
| 0xe | Undefined transition found. |
| 0xf | Reference to stack object out of range. |
| 0x10 | Bad alert tree number. |
| 0x11 | Bad gosub tree number. |
| 0x12 | Zero distance in finding direction. |
| 0x13 | Expression attempting to write in a read-only area. |
| 0x14 | Data value out of range. |
| 0x15 | No stack object present when required. |
| 0x16 | Reference to tree table entry that does not exist. |
| 0x17 | Could not find object reference. |
| 0x18 | Standard number for a weight assignment is out of range. |
| 0x19 | Weight number for a weight assignment is out of range. |
| 0x1a | Motive number out of range or not an acceptable value. |
| 0x1b | Slot number out of range. |
| 0x1c | Object referred to must be a person. |
| 0x1d | Balloon Type Is Invalid. Values are 0:thought, 1:scream, 2:speak. |
| 0x1e | Search type is invalid. Types are 0: all motive, 1:mental motives, 2:physical motives. |
| 0x1f | Tree table not found |
| 0x20 | Bad target slot specified in routing slot. |
| 0x21 | Person data field is out of range. |
| 0x22 | Show hide parameter is out of expected range. |
| 0x23 | Index into object definition out of range. |
| 0x24 | Invalid snapping mode specified |
| 0x25 | Invalid routing slot specified |
| 0x26 | Divide by zero. |
| 0x27 | Bad parameter in look towards primitive. |
| 0x28 | Bad room id specified |
| 0x29 | Bad room value requested. |
| 0x2a | Invalid global sim call. |
| 0x2b | Named tree does not exist. |
| 0x2c | Bad parameter to call named tree. |
| 0x2e | Invalid constant. |
| 0x2f | Invalid suit location. |
| 0x30 | Invalid suit index. |
| 0x31 | Invalid dynamic sprite index |
| 0x32 | Invalid object id. |
| 0x33 | Local out of range. |
| 0x34 | Stack object for routing either does not exist or is out of world. |
| 0x35 | Invalid action in tutorial primitive. |
| 0x36 | More than one passing value specified. |
| 0x37 | Missing neighbor for data access. |
| 0x38 | Bad expense type specified. |
| 0x39 | Career id not found. |
| 0x3a | Job data not found. |
| 0x3b | Invalid skin color. |
| 0x3c | Neighborhood data index out of range. |
| 0x3d | Start object required. |
| 0x3e | Tree break encountered. |
| 0x41 | 3D object is not animatable. |
| 0x42 | 3D object is not a person. |
| 0x43 | Animation name is empty string. |
| 0x44 | IsGlobal flag not implemented yet. |
| 0x45 | Material Change on Non 3D object. |
| 0x46 | Missing Material / Mesh Group Name. |
| 0x47 | Animation Primitive called on non Animating object - look at 3DObjectID in obj def. |
| 0x48 | Change graphic expression called on non 3D object |
| 0x49 | Event Tree handler called on deleted object or invalid object ID |
| 0x4a | Trying to use a Global Animation on a non Edith Person object. Naughty naughty! |
| 0x4b | Trying to look_at a non 3D object |
| 0x4c | Trying to change a light on a non 3d Object |
| 0x4d | Missing Light Name |
| 0x4e | Requesting to play an animation on an object for the third time |
| 0x4f | Missing Effect Name |
| 0x50 | Trying to change an effect on a non 3d Object |
| 0x51 | Turn rate must be positive |
| 0x52 | Trying to change an effect on an object with too many Particle Emitters |
| 0x53 | Missing requested bone for slot |
| 0x54 | No bone on model corresponding to name in routing slot on destination object |
| 0x55 | Can't place in world for some reason |
| 0x56 | Temporary token field out of range. |
| 0x57 | Inventory doesn't exist for ID passed to Manage Inventory. |
| 0x58 | Index passed to Manage Inventory out of bounds. |
| 0x59 | Bad parameter value set in Manage Inventory Primitive. |
| 0x5a | Trying to perform an animation from the Social Interaction list against a non person. |
| 0x5b | Hit Break Point Primitive. |
| 0x5c | Missing table for model bone names. |
| 0x5d | Missing (or empty) model bone name. |
| 0x5e | Missing Action String in table. |
| 0x5f | 3D object is not locomotable. |
| 0x60 | Invalid pop: locomotion stack is empty |
| 0x61 | Locomotion stack is too deep: check for imbalanced push/pops |
| 0x62 | IK target animation requires other object |
| 0x63 | IK target animation requires 3D model on other object |
| 0x64 | IK target animation cannot find bone on other object |
| 0x65 | IK target animation cannot find bone on self |
| 0x66 | Trying to modify variable in const type tree |
| 0x67 | Requesting a look_at operation that is already running on the object |
| 0x68 | The relationship matrix requested could not be found. |
| 0x69 | Cant run Reach/Drop on non Sim object. |
| 0x6a | No Object for Reach to work on. |
| 0x6c | Bad Object Array |
| 0x6d | Trying to access array element via iterator that does not exist. |
| 0x6e | Reach animation does not contain event 0. |
| 0x6f | Trying to push nested interaction on someone not in an 'allow nested' interaction |
| 0x70 | Invalid number of loops |
| 0x71 | Missing reach animation. |
| 0x72 | Missing Job Title / Phone Services Table (table 147) in Object. |
| 0x73 | Invalid animation type specified in animation string |
| 0x74 | Inventory token index out of range. |
| 0x75 | Inventory token propert index out of range. |
| 0x76 | Request for babies count out of range. |
| 0x77 | Temp0 does not point at a Sim ObjectID. |
| 0x78 | Bad destination for a 32bit write. |
| 0x79 | Could load the specified memory icon. |
| 0x7a | You sent me a crappy GUID, please fix it. |
| 0x7b | Trying to route while out of world. |
| 0x7c | Not enough local vars. |
| 0x7d | Bad or empty button or window GUID. |
| 0x7e | Animation speed cannot be zero |
| 0x7f | Couldn't load memory icon |
| 0x80 | Trying to route while in an pre route callback. |
| 0x81 | Trying to route while in an pre route callback. |
| 0x82 | Trying to route while in an post route callback. |
| 0x83 | Trying to run default effect - not supported anymore. |
| 0x84 | Invalid Lot ID. |
| 0x85 | Hit data break point - Value Changed! |
| 0x86 | Person is not selectable. |
| 0x87 | Cinematic scene not found |
| 0x88 | Cinematic animation not found |
| 0x89 | Required cinematic actor is missing |
| 0x8a | Required cinematic actor is invalid |
| 0x8b | Required cinematic actor has no 3D model |
| 0x8c | Cinematic origin actor (for camera and light rig) is invalid |
| 0x8d | Cinematic origin actor (for camera and light rig) has no 3D model |
| 0x8e | Cinematic autoCreate actor is not 0 |
| 0x8f | Trying to animate a cinematic actor that is not animatable |
| 0x90 | Trying to create instance of Template Object |
| 0x91 | Last Find Best Action was Not Successful. Quit trying to run gosub. |
| 0x92 | Attempt to change light on object that has no model. |
| 0x93 | Attempting to Save UI Mode to mode it's already saved at. |
| 0x94 | Attempting to create object type that's been unlinked (ie is dead, and has had data removed from it's .package file). |
| 0x95 | Attempted too many look_at requests in one simulator turn on this Sim |
| 0x96 | Target object out of world. |
| 0x97 | Unknown cinematic error. |
| 0x98 | Another cinematic already is active. |
| 0x99 | Cinematic camera does not have a choreo queue. |
| 0x9a | Bad family id. |
| 0x9c | Trying to save a temp token with instance data into inventory. |
| 0x9d | Trying to overwrite an inventory token with instance data. |
| 0x9e | Invalid species. |
| 0x9f | Bad number of function parameters. |
| 0xa0 | NULL tree table entry. |
| 0x3e8 | Stack overflow. |
| 0x3e9 | Stack underflow. |
| 0x44c | Undefined Transition. |
| 0x44d | Too many iterations. |
| 0x44e | Behavior not found. Probably a reset() problem. |
| 0x44f | Tree not found. Probably a reset() problem. |
| 0x450 | Transition to node that does not exist. |
| 0x451 | Check tree primitive blocked completion. |
| 0x452 | Max stack depth exceeded. |

Os outros códigos até 0x452 caem em "Unrecognized error condition : %d". Binário.
