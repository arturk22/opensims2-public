# O corpo do Sim no binário de Mac: fila de coreografia, mistura e IK

Leitura do binário de Mac (Bon Voyage 1.10, `sims2_bv.ppc`) para a rodada
n02 (`docs/inventario-binario.md` §5, linha 2), 2026-10-15. Endereços BV.

Modo: **Ghidra MCP** (só leitura: nada renomeado, retipado nem salvo no
projeto do usuário) e, para as funções que o projeto não tem como função
(`0xd195a0`, `0xd37848`, ...), **Ghidra sem janela** numa importação nova e
descartável do binário (`-noanalysis`, com o mapa do `tools/symxfer`
aplicado e o `DecompileList.java`). Os nomes vêm do symxfer; a confiança de
cada um vai entre parênteses quando não é "exact".

Complementa o `docs/primitives-mac.md` §4 (níveis dos pedidos, canais delta,
tempo de blend `T` = 233,333 ms, Animate Stop) e o `docs/seat-mac.md` (as
partes E, G e I da issue 001 ficaram para esta rodada).

## 1. A ordem de um quadro (`cChoreoQueue::AnimUpdate`)

`cChoreoQueue::AnimUpdate(tempo, passo, b1, b2, marcar)` (BV `0xd195a0`,
"vtsize"; não é função no projeto, lida na listagem) faz, nesta ordem:

1. **Os pedidos** (`+0x30`, vetor de `cIChoreoRequest`): cada um recebe
   `AnimUpdate(tempo)` (vtable `0x0c`): estado (`+0x0C`: 0 parado, 1 tocando,
   2 saindo, 3 expirado), o início do blend de saída e a porção feita
   (`cChoreoRequest::AnimUpdate`, `0xd93bfc`).
2. **Os tratadores** (`+0x3C`: o esqueletal, o de IK, o de olhar): cada um
   `AnimUpdate(tempo)` (vtable `0x14`). O de IK (`cChoreoHandlerIK`) cria um
   `cActiveAnimIK` por cadeia de IK do alvo do pedido
   (`HandleChoreoRequest`, `0xd3ca08`) e tira os de pedidos que expiraram
   (`RemoveExpiredActiveAnimIKs`, `0xd3c2f0`, que também tira os outlets de
   IK do segundo passo).
3. **Os outlets** (um `cChannelUberOutlet` por osso; `+0x54` no primeiro
   passo, `+0x60` no segundo): cada um mistura os canais que o alimentam
   (vtable `0x10`). Os ossos das cadeias de IK ficam no **segundo passo**
   (`AddToSecondPass`): primeiro o corpo inteiro se mistura, depois o IK
   resolve a partir dele e os ossos das cadeias se misturam de novo com os
   canais de IK.
4. Com o último argumento ligado, os pedidos **expirados** (vtable `0xfc`)
   são marcados para sair (vtable `0x24`) e a fila os tira
   (`RemoveMarkedChoreoRequests`, `0xd1915c`).

Quem chama: o `nTSSG::cAnimatable::OnPreSimulatorTick` (`0x6980d8`, "near"),
a cada tick da simulação, roda a fila com "marcar" ligado e depois, para
cada `cAnimRequest` do TS cujo pedido de coreografia expirou, roda a fila de
novo no passo 2 e **tira o pedido da lista** (vtable `0x2ec`). Não há um
"guardar o último expirado": o `MarkExpiredRequestsForRemovalExceptMostRecent`
(`0xd1864c`) existe mas este caminho usa o comum (`0xd180f8`).

O mesmo `OnPreSimulatorTick` escala a velocidade dos pedidos pelo **dado de
pessoa 85** (em %; `v = 1 + p/100` se p > 0, `1/(1 − p/100)` se p < 0) — não
feito, fora desta rodada (`TODO.md`).

## 2. Quando um pedido expira (`cChoreoRequest::IsExpired`)

`cChoreoRequest::IsExpired` (`0xd93a18`) põe o estado 3 conforme a **regra
de expiração** (`+0x30`):

| regra | expira quando |
|---|---|
| 2 | na hora |
| 3 | quando a porção feita chega ao fim (`porção ≥ fim − 1e-6`) |
| 4 | quando já começou (`+0xAC`) e o peso (`+0x38`) ≤ 0,001 |
| 5 | quando o peso ≤ 0,001 **e** a porção chegou ao fim |

O Animate Sim/Overlay salvos usam 5 com blend de saída e 3 sem
(`docs/save-mac.md`); o Animate Stop com blend usa 4 (`AbortAnimRequest`).
Um pedido em loop nunca chega ao fim pela porção. A locomoção
(`cChoreoRequestLoco::IsExpired`, `0xd539f8`) é igual, com o tempo e o fim do
caminho.

**O que acontece com os ossos.** O pedido expirado sai da fila no mesmo tick
(§1). Os canais dele saem dos outlets; um outlet sem peso nenhum **não
escreve o osso** (`docs/primitives-mac.md` §4), e o osso fica com o último
valor escrito. Ou seja:

- um Animate Sim que tocou uma vez e acabou deixa o corpo **parado na última
  pose misturada** onde nada mais toca;
- onde um pedido de nível mais baixo ainda toca (uma idle da cadeira, nível
  1), esse pedido **toma o osso na hora**, sem blend (o expirado já não pesa
  nada);
- a "pose segurada" não é de nenhum pedido: é a memória do osso. Se o corpo
  estava misturando dois pedidos quando o último expirou, fica a mistura.

No `AnimPlayer` isso era feito com o pedido terminado posto num nível −1
abaixo de todos (inferido, rodada 15/21), e o osso que nenhuma camada
escrevia voltava à pose base (a de repouso, num Sim). **Feito** (rodada
n02): cada osso que nenhuma camada escreve fica com o valor do quadro
anterior (`AnimPlayer::held_`; só os canais base contam: um delta sem canal
base, o da cabeça, não se acumula), também depois de um salto da raiz
(`stayInWorld`). Teste `test_anim` "held bones". **Continua inferido** o
pedido terminado no nível −1: é o nosso modelo da sobreposição no fim de um
pedido (o `CreateTransitionBlend` e o fim da ação do primitivo bloqueante,
que deixam o pedido velho sair por blend enquanto o novo entra, não foram
lidos); tirá-lo na hora faria cada troca de animação saltar, porque um
canal sozinho entrando já toma o osso inteiro (normalização).

## 3. O peso de um canal (`cChannelUber::Weight`)

`cChannelUber::Weight(float* resto)` (`0xedbb90`, "fill"; o código bate com
o nome):

- peso = `Weight()` do pedido (vtable `0xbc`, o float `+0x38` do
  `cChoreoRequest`), que a função de blend do pedido (`cBlendFunction2`)
  atualiza;
- para os canais de IK (tipo do canal `+0x10` entre 4 e 6), multiplica pelo
  **peso de IK** da cadeia (o canal de peso de IK do ANIM, `+0x2C`,
  limitado a 0–1): o canal de IK pesa `peso × w`; o canal de FK do mesmo
  osso (flag `+0x30` = 0) pesa `peso × (1 − w)`. Por isso `applyIk`
  mistura a solução com a pose da animação por `w` (nlerp), como já fazia;
- o canal de tipo 6 ainda multiplica pelo canal de **contato** (`+0x4C`) do
  alvo: os vários alvos de uma cadeia entram no outlet do nó do alvo com
  peso `peso × contato`, e o outlet faz a média normalizada (§5).

**A forma do blend.** `cBlendFunction2::Init` (`0xd3363c`, "near") monta a
função de peso em intervalos (`cBlendInterval`, CLSID `0x4b96aa16`): entrada
(tipo 0, de `inicial` a `alvo`), começo em rampa (5, opcional), sustentação
(1), fim em rampa (4) ou meia-transição (3), saída (2, até `final`); com a
flag de "rampa" e sem duração de rampa, as rampas viram 50 ticks no máximo,
metade tirada de cada lado. `Value(t)` (`0xd32498`) acha o intervalo pelo
tempo desde o início (`+0x18`) e pede o valor a ele (vtable `0x34`).
**Não lido:** o `cBlendInterval::Value` (no baserom, `0x3436c8`, 20
instruções; no BV não achei o endereço: a vtable do intervalo não foi
localizada). Fica **inferido** que entrada e saída são lineares (o
`AnimPlayer` faz assim).

## 4. O solucionador de IK de duas juntas (`cIKStrategy7DOF`)

A estratégia `0x48EE7A41` ("7DOF"; vtable `0x28326a8`). Cada elemento da
cadeia ganha um **tipo de canal de IK** (`SetUberChannelIKTypes`,
`0xd36d38`), pela posição:

| cadeia | elementos (osso) e tipos |
|---|---|
| 3 ossos (pernas) | coxa **1**, panturrilha **4**, pé **6** |
| 5 ossos (braços) | braço **2**, bíceps **3**, antebraço **4**, pulso **7**, mão **8** |

**Translação** (`ComputeIKTranslationSolution`, `0xd37848`, lida no Ghidra
sem janela). Entradas: o alvo `G` e o vetor de torção (polo) `P`, os dois
lidos dos *outlets* dos nós (`cActiveAnimIK::ComputeIKTranslationSolution`,
`0xd1e698`: só resolve se os dois outlets têm peso > 0), no quadro do osso
de começo (o pai dele no mundo, mais a translação do osso).

1. Comprimentos: `a` = soma do Y da translação local (de repouso) dos
   elementos de tipo < 5 depois do primeiro (bíceps + antebraço; a
   panturrilha), `b` = a dos de tipo ≥ 5 (pulso + mão; o pé). Os ossos do
   esqueleto ficam ao longo do +Y.
2. `d = |G|`; `n = normaliza(G × P)`; `m = normaliza(n × G)` (no plano de G
   e P, do lado do polo).
3. `cos A = (a² + d² − b²) / (2 a d)`. Se `cos A > 0`: a direção do osso de
   cima é `v = normaliza(G + m · d · tan A)` (A a partir de G, para o lado do
   polo); senão `v = m` (a 90°; com `a > b`, como em braços e pernas, não
   acontece).
4. Rotação do osso de cima: a menor rotação que leva +Y a `v`, e depois um
   giro em torno do **próprio Y** do osso (`θ = acos(−n·X)`, com o sinal de
   `n·Z`) que deixa o eixo X do osso perpendicular ao plano de G e P: a
   dobradiça.
5. A junta (tipo 4) ganha **só um giro em X**: `π − acos((a² + b² − d²) / (2 a b))`
   (limitado a −1..1). É uma dobradiça: nenhum outro eixo.
6. Braços (tipo 2): **65 %** (`FLOAT_0274b6cc` = 0,65) do giro em Y do braço
   passa para o bíceps (tipo 3: `(0, 0,65 y, 0)`), e o braço fica com o resto
   (`Ry(−0,65 y)` aplicado de volta).

**Rotação** (`ComputeIKRotationSolution`, `0xd36ecc`; o `cActiveAnimIK` em
`0xd1e900` lê a rotação do outlet do nó de rotação do alvo): a rotação do
alvo no quadro do pai do primeiro osso de baixo (o antebraço; a
panturrilha). O pé (tipo 6) recebe a rotação inteira; no braço, **75 %** do
giro em Y vai para o pulso (tipo 7: `(0, 0,75 y, 0)`) e a mão (tipo 8) fica
com o resto (`Ry(−0,75 y)` antes).

Os valores saem como ângulos de Euler (`ConvertToEulerAngles`, ordem
`0x01020100`) para os canais de IK dos ossos, que entram no outlet do osso
com o peso da §3. A ordem exata dos eixos de Euler não foi decifrada (a
biblioteca `Math::` é de templates de expressão): **o "giro em Y" usado na
partilha de torção é inferido** como a torção em torno do eixo do osso
(decomposição giro-torção).

**Diferença no nosso código** (`formats/anim.cpp`, `applyIk`, antes desta
rodada): os dois primeiros ossos giravam pela menor rotação até o cotovelo e
até o alvo, mantendo a torção que a animação dava (o braço e a junta podiam
girar em qualquer eixo); a mão ficava com a rotação toda e o bíceps e o
pulso só "iam junto". O cotovelo caía no mesmo lugar (mesma fórmula de `A`),
mas a torção dos ossos e da pele não era a do jogo. **Feito** (rodada n02):
o `applyIk` usa o solucionador acima (tipos por posição, comprimentos pelo
Y, dobradiça em X, giro do braço pelo polo, 65 %/75 % da torção no bíceps e
no pulso); testes `test_anim` "arm IK" (dobradiça, partilha da torção).
Continua nosso: sem nó de polo, o polo é a junta como está.

## 5. Vários alvos e o que o IK lê

- `cActiveAnimIK` lê o alvo e o polo pelos **outlets** dos nós (base ×
  delta, já feito na rodada 15). Os alvos de uma cadeia (`cIKTarget`, tipos
  0/1/2) entram no outlet do nó do alvo com peso `peso do pedido × contato`;
  o outlet faz a média normalizada por peso — o que o `applyIk` já fazia
  (posição linear, rotação por nlerp, a soma normalizada). **Confirmado.**
- Sem peso nenhum no outlet (todos os contatos em 0), o `cActiveAnimIK` não
  resolve naquele quadro (`ComputeIKTranslationSolution` devolve falso) e os
  canais de IK ficam com a última solução. O `applyIk` usa o primeiro alvo
  nesse caso (inferido; continua assim: guardar a última solução por cadeia
  fica no `TODO.md`).
- O alvo é o do **outlet do nó**, isto é, a mistura de todos os pedidos que
  chaveiam aquele nó, não o do pedido sozinho. O `AnimPlayer` lê o alvo da
  própria camada (com os deltas do outlet). Igual quando só uma camada
  chaveia o nó (o caso comum); continua inferido no resto.

## 6. Partes E, G e I da issue 001

- **E. Sentado e parado.** Não existe um "sistema de idle" no motor: as
  idles são pedidos que as árvores tocam (Animate Overlay de nível 1). Entre
  duas idles, ou com a fila vazia, nenhum canal escreve os ossos e eles
  ficam com o último valor (§2). A regra "segura a pose" do `SeatModule`
  (regra 4) é isso, e vale para qualquer osso que nenhuma camada escreve,
  sentado ou em pé (§7). A idle em pé
  que o app toca quando a VM não toca nada (`LotSimulation::sampleAll`,
  `b.idle`) continua um substituto **nosso** das árvores de idle (inferido).
- **G. IK das mãos e pés.** §4 e §5. `HandleChoreoRequest` cria os
  `cActiveAnimIK` de cada cadeia; `FindIKTargetBone`/`ValidateIKTargetBones`
  (`0x94b4f4`/`0x94b7fc`) acham os ossos dos alvos de tipo 2 no objeto de IK
  (já feito na rodada 15).
- **I. Sustos e expressões sentado.** O motor não troca a animação por uma
  versão sentada: o `try_animate_sim` (rodada 13) cria o pedido sem olhar o
  slot, e a escolha do nome (`AnimPrefix` `0x68f440`,
  `DetermineAppropriateAnimTable` `0x68f7c8`) só olha espécie, idade e sexo
  (dados de pessoa 186 e 58: `a-`/`c-`/`p-`/`e-`, `d-`/`l-`/`f-`, "male-"/
  "female-"). Se a árvore toca o susto em pé com o Sim contido, o jogo também
  toca em pé, a partir da raiz no slot. Nada a mudar no motor; se a cena do
  `N001_Lot11` ficar estranha, é a árvore (a VM roda as árvores do jogo). A
  "a-noExpression" (só o rosto) toca por cima do corpo como parcial, pela
  mistura por ossos — igual ao jogo.

## 7. O que mudou no código e o que sobrou

Sobras (no `TODO.md`): a forma dos intervalos de blend
(`cBlendInterval::Value`), a ordem de Euler da partilha de torção, a última
solução de IK guardada quando o contato é 0, o alvo misturado do outlet (e
não o da camada), a velocidade pelo dado de pessoa 85, e as transições de
locomoção (`cActiveAnimLoco::PushTurnBlendCycles`, `StopLocomotion`).
