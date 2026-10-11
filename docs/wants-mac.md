# Desejos e medos: o que o binário de Mac faz

Convenções como em `docs/buy-build-mac.md` (endereços do BV 1.10 ppc; o
`cTSWantSimulator` está bem nomeado no BV, BV `0x503874`…`0x516000`). "dados"
= `data/game` (base 2004). O painel está em `docs/ui-live-mac.md` §7.4. No
código hoje: a primitiva Want Satisfy (`src/vm/appearance.cpp`) só confere o Sim
e devolve verdadeiro; não há simulador de desejos nem leitura do SWAF.

## 1. Dados

### 1.1 Definições (`TSData/Res/Wants/Wants.package`, tipo `0xED7D7B4D`, 493)

XMLs `cGZPropertySetString version="6"`, um por desejo, instância = `id`:

| chave | o que é |
|---|---|
| `id` | GUID do desejo |
| `folder`, `nodeText` | só do editor ("Marriage", "Marry") |
| `primaryIcon`/`secondaryIcon` | ícones do painel (o secundário quando o assunto é uma pessoa) |
| `stringSet` | STR# (instância de 15 bits) com o texto |
| `checkTree` | BHAV que decide se o desejo pode ser oferecido ("CT - Test - Get Married") |
| `feedbackTree` | BHAV rodado ao cumprir ("CT - Handle Feedback") |
| `simArrayTree` | BHAV que lista os Sims candidatos a assunto (vazio na maioria) |
| `level` | `Transitory` (291), `Power` (147), `Memorable` (55) |
| `score` | pontos de aspiração; **negativo = medo** (115 dos 493) |
| `objectType` | tipo do assunto: `None` 253, `Sim` 176, `Guid` 29, `Skill` 15, `Career` 13, `Category` 7 |
| `integerType`/`integerOperator`/`integerMultiplier` | desejos com número (`Number`; `Equals`, `Greater_Then_Or_Equals`, `Less_Then`) |
| `eventRequiresSim`/`eventRequiresObject`, `deprecated` | |

`IsFear` (BV `0x4fb240`) vem da chave `isFear` se existir; nenhum XML do base
a tem, e o padrão é o **bit de sinal do `score`** (`ReadProperties`, BV
`0xe4eaec`): medo ⇔ `score < 0`.

### 1.2 Árvores (`WantTrees.package`, tipo `0x6D814AFE`, 211)

XML `<tree version="7">` com um property set (`id`, `folder`) e um
`<wantNode key="root">` com filhos `<children><wantNode key="node">…`. Cada
nó:

- `wantId` (a definição), `id` (do nó);
- `visible` (pode ser oferecido; os nós invisíveis só servem de gatilho),
  `lifetime` (por quanto tempo o filho fica disponível depois do pai ser
  cumprido; 0 = sempre), `frequency` (periódico), `regeneration`;
- `objectField`/`integerField` (fixam o assunto), `inheritSubject` (usa o
  assunto do pai);
- `minScore`/`maxScore`: intervalo da **prioridade**;
- 6 blocos `<ads key="…" weight="w" superZero="b" locked="b">`: `age`
  (Baby…Elder), `aspiration` (Family, Romance, Fortune, Knowledge, Popularity,
  GrowUp), `personality` (Mean/Nice, Sloppy/Neat, Shy/Outgoing, Lazy/Active,
  Playful/Serious), `aspirationScore`, `relationship`, `mood` (os três com
  `<range min max>` em −100..100); `superZero` vai para o fator `+0x14`
  (lido como "exclusivo": fator ≤ 0 zera a prioridade; inferido pelo nome do
  getter `IsExclusionary`).

Exemplo: a árvore `0x0DB3A3E7` (Family) tem a raiz "Have Child" invisível e o
filho "Trigger Sim Becomes Toddler" com `lifetime` 200 e `inheritSubject`.

### 1.3 Ajuste (`WantTuning.package`, `UI_00000000-CDA53B6F-00000000-2D7EE26B`)

XML `<wantSimulator>` lido por `ReadTunableValues` (BV `0x50b598`):

- `<levels>`: 7 limiares de pontuação crua por idade (do pior ao melhor:
  rock bottom, severe failure, neutral, bronze, silver, gold, perfect):

| idade | rock bottom | severe | neutral | bronze | silver | gold | perfect |
|---|---|---|---|---|---|---|---|
| bebê | 0 | 2000 | 4000 | 6000 | 8000 | 10000 | 12000 |
| criança pequena | 0 | 150 | 300 | 600 | 1200 | 1950 | 3000 |
| criança | 0 | 300 | 600 | 1200 | 2400 | 3900 | 6000 |
| adolescente | 0 | 450 | 900 | 1800 | 3600 | 5850 | 9000 |
| adulto | 0 | 600 | 1200 | 2400 | 4800 | 7800 | 12000 |
| idoso | 0 | 750 | 1500 | 3000 | 6000 | 9750 | 15000 |

  Sem o arquivo, o binário usa −1000, −500, 0, 4000, 8000, 12000, 16000
  (inferido: o vetor fica em ordem crescente, como o padrão; o XML lista do
  melhor ao pior).
- `<categories>` (53): categorias de objeto dos desejos `Category` (id 10000+,
  nome, ícones).
- `<skills>`, `<careers>` (gerados): IDs de habilidade (person data 9 Cleaning,
  10 Cooking, 11 Charisma, 12 Mechanical, 15 Creativity, 17 Body, 18 Logic) e
  GUIDs de carreira adolescente/adulto.
- `<lifetimeGoals>` por aspiração (a string existe no base, mas o SWAF do base
  não guarda desejo vitalício: ver §4).

## 2. Escolher desejos e medos

`OnTick` (BV `0x512bf8`) avança o relógio de desejos (`+0x14C` += 2 por tick
da sim, reinicia acima de 2^30) e chama `DoWork(5, 0)`, uma máquina de estados
com **um Sim por vez** (BV `0x512100`):

| estado | faz |
|---|---|
| 0 | pega o próximo pedido de atualização (o Sim selecionado primeiro), monta o `MetaPerson`, lê o histórico e o tempo pessoal dele |
| 1 | `WorkOnFindingGoals` (BV `0x50ec6c`): junta os candidatos; fatiado no tempo (a cada 10 nós, para se passou do orçamento de ms e continua no tick seguinte) |
| 2 | `PrioritizeGoals` (BV `0x50eb8c`): prioridade de cada candidato e ordenação decrescente |
| 3 | `SelectBestWantsAndFears` (BV `0x50db30`); escolhe o desejo vitalício se não houver (só com o recurso) |
| 4 | avisa a UI (mensagem `0x2D2FA4E6` por slot que mudou) e, se pedido, limpa o histórico |

**Candidatos** (`WorkOnFindingGoals`): para cada raiz das árvores: se ainda
não foi cumprida (ou é "evergreen") e é visível, vira candidata com um
assunto escolhido por `PickSubject` (BV `0x503d0c`). Para cada conquista do
histórico que corresponde a um nó, os **filhos** desse nó viram candidatos se
o tempo desde a conquista ainda está dentro do `lifetime` do filho (≤ 0 =
sem prazo; tempo = relógio pessoal do Sim no histórico), herdando o assunto
quando pedido; filhos invisíveis cumpridos abrem os netos do mesmo jeito.

**Prioridade** (`CalculateWantPriority`, BV `0x509c54`; os índices de fator
são os do `GetWeight`/`UseFactor` do nó):

```
f_idade      = 1 se o nó serve à idade do Sim, senão 0
f_aspiração  = 1 se serve à aspiração (person data 46), senão 0
f_humor      = rampa do humor (person vtable 0x34(3)) no [min,max] do nó  (com min>max a rampa desce)
f_personal.  = média, para cada traço marcado, de clamp((500 − valor)/500) para Mean/Sloppy/Shy/Lazy/Serious
               e clamp((valor − 500)/500) para Nice/Neat/Outgoing/Active/Playful  (person data 2, 7, 6, 3, 5; 0..1000)
f_relação    = rampa do relacionamento (diário) do Sim com o assunto no [min,max]
f_aspScore   = rampa da pontuação de aspiração normalizada (−100..100)
f_dadoPessoa = rampa de um person data (índice 1..0xCC do nó)
fator usado e "exclusivo" com valor ≤ 0 → prioridade 0
m = Σ peso_k · f_k / Σ peso_k   (1 se Σ peso = 0)
prioridade = (minScore + m · (maxScore − minScore)) × fator_de_tempo
fator_de_tempo = (lifetime − decorrido)/lifetime, ou (frequency − decorrido mod frequency)/frequency
                 se o nó tem frequency; 0 depois do prazo; 1 sem conquista de origem
```

**Escolha** (`SelectBestWantsAndFears` + `SelectOneWantOrFear`, BV
`0x50d57c`): os slots atuais que não estão travados (cadeado: só BV) saem se
o tipo de atualização pede troca (`< 2` troca desejos, `≤ 2` troca medos) ou
se não passam mais no teste de versão do jogo e na **check tree**. Depois, na
ordem de prioridade, cada candidato com prioridade > 0 vai para um slot livre
do seu tipo (desejo/medo), sem repetir um desejo igual ou um mais específico
já presente; um desejo que já estava em certo slot antes volta para o mesmo
slot. **Slots no base: 4 desejos e 3 medos** (padrões do `Read`, e o painel
do base tem só esse layout).

**Check tree** (`GoalPassesEdithCheck`, BV `0x50d174`): roda a `checkTree` do
desejo no **objeto controlador de desejos** (`GetWantControllerObject`) na
hora, com 8 parâmetros: o neighbor ID do dono e os dados do assunto (inferido:
GUID em duas metades, ID de vizinho, inteiro). Resultado guardado por
instância durante a seleção (`+0x188`). O cheat `ToggleSkipCheckTrees` pula.

**Quando atualizar** (`RequestWantsUpdate`, vtable `0x84`, BV `0x50bf44`):
fila de (Sim, tipo); o simulador pede o tipo 3 (manter o que ainda vale e
completar) depois de cumprir um desejo, ao mudar o número de slots, ao ler o
Sim e no `RecheckGoals`. Os outros tipos vêm de fora (árvores e jogo; não
rastreado aqui). As mensagens do `DoMessage` (BV `0x51325c`): vizinho
apagado (`DeletePerson`), relógio zerado, limpeza total.

## 3. Cumprir um desejo e a pontuação de aspiração

`MarkWantAsAchieved(sim, wantId, assunto)` (BV `0x514070` →
`MarkOneWantAsAchieved`, BV `0x513760`; chamado pela primitiva **Want
Satisfy 0x7C**, `docs/primitives-mac.md`):

1. grava uma conquista no histórico do Sim (sempre: é ela que abre os filhos
   na árvore);
2. se bate com o desejo vitalício (BV) ou com desejos/medos nos slots
   (`FindAchievementInVector`, mesmo desejo e assunto compatível), soma a
   pontuação de cada um, marca a hora e **esvazia o slot**;
3. só se bateu com algum: `ChangeScore(total)`, pede atualização tipo 3 e
   avisa a UI (`0x2D2FA4E7`); a `feedbackTree` roda pelo `NotifyEdithOfScore`.

`ChangeScore` (BV `0x5076c0`):

```
pd 162 "Aspiration Score Raw (/10)":  raw = clamp(raw·10 + pontos, limiar[0], limiar[6]) / 10
pd 160 "Aspiration Score":            normalizada (abaixo)
pd 161 (pontos de recompensa, /10):   += pontos/10, preso em 0..32767   (o rótulo diz "Spent")
pd 178 influência e o traço "Boundless Influence": só BV (Nightlife)
```

Normalizada (`NormalizedAspirationScore`, BV `0x50717c`): acha o maior
limiar `i ∈ 1..5` com `limiar[i] ≤ raw` e interpola linearmente entre os
valores do medidor `−100, −50, 0, 25, 50, 75, 100` (tabela BV `0x2734980`),
presa em ±100. Nível (`GetAspirationLevel`, BV `0x5074a4`) = esse `i`
(0 se abaixo do limiar 1). Ex. adulto: raw 1200 (neutral) → 0; 0 → −100;
12000 → +100.

## 4. O SWAF (`0xCD95548E`, um por Sim no pacote do bairro, instância = neighbor ID)

`cTSWantSimulator::Read` (BV `0x514264`) / `Write` (BV `0x51540c`), inteiros
little-endian no arquivo:

```
u32 versão                      (base: 1; o BV grava 6; lê até 6)
v ≥ 3: u32 tem_vitalício; se 1: uma instância (o desejo vitalício, University)
v ≥ 2: u32 nº de slots de desejo   (senão 4)
u32 nº de desejos guardados; instâncias
v ≥ 2: u32 nº de slots de medo     (senão 3)
u32 nº de medos guardados; instâncias
v ≥ 4: u16 nº de cadeados          (BV)
```

Os 280 SWAF do `N001` têm versão 1 (4 desejos e 3 medos). O histórico de
conquistas (`cTSWantAchievementHistory`, com o relógio pessoal) segue no mesmo
recurso (BV `WritePerson`); o formato dele não foi lido.

**Instância** (`cTSWantInstance` Write/Read, BV `0xae8f84`/`0xae9140`;
versão 10 no BV, 8 nos dados do base):

```
u32 versão (≤ 10)
u16 dono (neighbor ID)
u32 GUID do desejo
u8  tipo do assunto: 0 nenhum, 1 Sim, 2 GUID, 3 categoria, 4 habilidade, 5 carreira, 6 (BV)
assunto: tipo 1/4: u16; 2/3/5: u32 (o 3 só com v > 1); 6: u16 (v < 10, traduzido) ou u32
u16 dado extra do assunto (tipo 0 com v > 6, tipo 1 com v > 7, os outros com v > 2)
u32 hora da conquista            (v < 4: lida e × 2)
v ≥ 6: u32 pontuação              (senão a da definição)
v ≥ 9: u32 influência             (senão a da definição; BV)
v ≥ 5: u8  flags (travado, apagável, ...)
```

## 5. Base vs BV

Base: o simulador inteiro, 4/3 slots, as três fontes de dados, o SWAF v1.
Só BV: cadeados e slots extras (`SetNumWantAndFearSlots`/locks, SWAF v4),
desejo vitalício gravado (v3; University), influência e "Boundless Influence"
(Nightlife), o tipo de assunto 6.

## 6. Para implementar

1. Ler definições e árvores; o SWAF (v1) dos Sims para mostrar o painel.
2. `MarkWantAsAchieved` na Want Satisfy: histórico + slots + `ChangeScore`.
3. A seleção: candidatos pelas árvores, prioridade, check tree no controlador,
   4/3 slots; um Sim por tick, como o `DoWork`.

## 7. Aberto

- O formato do histórico de conquistas e a unidade do relógio pessoal (o
  `lifetime` 200 do exemplo).
- Quem pede atualização com os tipos 0–2 (reroll ao acordar etc.).
- Os parâmetros exatos da check tree e o objeto controlador (qual GUID).

## 8. Rodada 8: o que o código faz e o que se achou depois (Ghidra headless, BV)

Implementado em `src/formats/wants.*` e `src/vm/wants.*`; painel em
`src/app/wants_panel.*`. Fechando o §7 e corrigindo detalhes:

- **Histórico de conquistas** (escrito por `WritePerson` logo depois do
  `Write`, FUN_00aee07c): `u32 versão (2)`, `i32 relógio pessoal`,
  `u32 nº de entradas`, por GUID de desejo (ordem crescente, há entradas com
  0 instâncias): `u32 GUID`, `u32 n`, `n` instâncias. Depois dele, 611 dos
  1.045 SWAF do base têm **bytes velhos** (o jogo regrava o recurso sem
  cortar): ignorados na leitura, guardados para regravar igual. Os 1.045
  voltam byte a byte (`s2pkg wants-sweep`).
- **Relógio**: `OnTick` soma 2 por tick da simulação; o nó lê `lifetime` ×
  **0xE10** (3600 por hora de jogo). O relógio pessoal do Sim (histórico +0/+4)
  anda com o do simulador sempre que é lido (FUN_00aedfd0). O leitor do nó
  procura `adFrequency`, que nenhum XML tem: o `frequency` dos XMLs não é
  usado; com `regeneration` e prazo, o prazo vira o período (e o prazo 0).
  `regeneration` é o "evergreen" (oferecido de novo depois de cumprido).
  Padrões do nó: visível, herda o assunto, `maxScore` 100, `scoreOverride`
  ausente = sentinela 0x42424242 (vale o `score` da definição).
- **Fatores** (`cTSWantNode`, vtable 0x68..0x90): `UseFactor(k)` = peso ≥ 1
  ou exclusivo; os intervalos (humor, relação, pontuação, dado de pessoa)
  só contam se min ou max ≠ 0; a soma ponderada é acumulada num **inteiro**
  (truncada a cada fator); relação = variável 2 (vitalícia) do SREL do Sim
  para o assunto; humor = motivo 3 do Sim no lote (0 fora dele). Enumerações
  das chaves `bool`: idade Adult 0, Child 1, Toddler 2, Teen 3, Elder 4,
  Baby 13; aspiração = os valores do dado 46 (Romance 1, Family 2, Fortune 4,
  Popularity 16, Knowledge 32, Grow Up 64); personalidade Nice 1, Mean 2,
  Active 3, Lazy 4, Playful 7, Serious 8, Outgoing 9, Shy 10, Neat 11,
  Sloppy 12.
- **Escolha**: depois de escolher, os slots soltam a conquista de origem
  (vtable 0x94); a ordenação é `std::sort` (aqui estável: inferido para
  empates). Nenhum número aleatório entra na escolha.
- **Check tree**: as árvores ficam no código do objeto "Wants - Check Trees"
  (grupo `0x7F739728`, que não está no lote) e rodam no "Wants - Controller"
  (GUID `0x6D9E58F0`, está nos lotes): me = stack object = o controlador. 8
  parâmetros: dono, GUID (baixo, alto), neighbor ID do assunto (Sim), GUID do
  assunto objeto/carreira (baixo, alto), o short do assunto (+4 do
  `cTSWantSubject`: o valor de Sim/habilidade, a metade alta dos de 32 bits) e
  o número. A **feedback tree** recebe 15: esses 8, cumprido, pontos, o vtable
  0x24 da definição (inferido: o nível), pontuação crua antiga / 10, a
  normalizada antiga, o objeto da Want Satisfy e a influência.
- **Sim array tree** (FUN_00508b10): roda no controlador com o dono no
  parâmetro 0; a lista é a array 0 do objeto que fica no temp 0 (inferido).
- **Quem pede atualização**: Generic Sims Call **39** "Force Recalc of Wants"
  (`TryGenericSimCall` caso 0x27): temp 0 = operação (+10: Sim = vizinho do
  temp 1, argumento no temp 2; senão o Sim sou eu, argumento no temp 1);
  0 pedir atualização do tipo do argumento (< 4, senão 0), 1 limpar o
  histórico, 2/3 pôr a pontuação (normalizada / crua ÷ 10), 4 `RecheckGoals`,
  5/6 pôr/somar o relógio pessoal (horas × 3600), 7 desejo vitalício (BV), 8
  alternar o pulo das check trees. Os chamadores são os globais "Wants -
  Refresh - Recalc/Verify", o "Function - Main" das pessoas, a falha de
  aspiração e o "Decay" (que baixa a pontuação ao longo do dia).
  `ReadPerson` pede o tipo 3 e grava a conquista "Is Born" (`0xAD9ACD9A`) se
  faltar.
- **Want Satisfy** (`TryWantSatisfy`): byte 13 bit 0 = o Sim é neighbor ID
  (senão objeto), bit 1 = o assunto Sim é neighbor ID; o tipo vem da
  definição; categoria e GUID viram GUID do objeto (o tipo 3 vira 2); erros
  0x1C (sem vizinho), 0x07 (sem desejo), 0x17 (sem objeto); devolve verdadeiro.
- **Categoria de um objeto** (desejos `Category`, Set to Next 37): campo
  **0x2F** do OBJD (offset 0x9E), 10000..10052 em 689 definições.
- `NormalizedAspirationScore` arredonda ao par mais próximo (truque do float
  1,5·2²³): o Sim 17 de N001 tem cru 1050 → −12,5 → **−12**, como no SDSC.
