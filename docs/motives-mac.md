# Motivos, humor e curvas: o que o binário de Mac faz

Rodada n17 (Ghidra MCP para achar, Ghidra headless numa cópia APFS do
projeto, só leitura, para decompilar o que o projeto não tem como função).
Endereços do **jogo base 1.0** (`/mac/sims2_base_1.0.ppc`, "base") e do BV
1.10 (`/mac/sims2_bv.ppc`, "BV"). O base não tem nomes; as classes saem
pelo RTTI (typeinfo → vtable). Onde os dois diferem, **vale o base** (é o
jogo que os dados do `data/game` rodam) e o BV fica anotado.

Antes desta rodada o código seguia três ideias inferidas que o binário
desmente: o decaimento diário "aplicado pelo motor por tick", o humor
"recalculado todo tick" e o humor "preso em ±100" (§3–§5).

## 1. `cTSPiecewiseFunction` (curva linear por pedaços)

Vtable base `0x15d9d58` (como `cTSMotiveCurve`, que herda dela; a segunda
interface, `cITSMotiveCurve`, fica em `+0x24` com thunks `0x14b06cc..`).
BV: vtable `0x27c34e0`, funções em `0xa90d18..0xa91ed4`. Objeto:

| campo | o que é |
|---|---|
| `+0x08` | `vector<cPoint>` (x, y em float), em ordem crescente de x |
| `+0x14` | `vector<float>` inversos das larguras: `1 / (x[i+1] − x[i])` |
| `+0x20` | máximo de pontos (−1 = sem limite; os conjuntos põem 32) |
| `+0x28` | (cTSMotiveCurve) o número do motivo |

- **Pôr ponto** (vtable `0x14`, base `0xc42b9c`, BV `0xa919a4`): se já
  há o máximo de pontos, nada. Procura do começo o primeiro ponto com
  x ≥ o novo; **x igual = o ponto novo é ignorado** (vale o primeiro);
  insere ali e refaz os inversos (`0xc431e0`: `1.0f / (x1 − x0)` em
  float).
- **Ler do texto** (vtable `0x18`, base `0xc42f00`, BV `0xa91d74`): para
  cada trecho de `(` até o `)` seguinte, `sscanf("(%f;%f)")`; com os dois
  números, põe o ponto; trecho que não lê é pulado. Assim pontos fora de
  ordem entram ordenados e x repetido fica com o primeiro. (O BV ainda
  tira pontos do meio de trechos planos, `0xa91b0c`; o valor não muda.)
- **Valor em x** (base `0xc43110`, BV `0xa910c4`): sem pontos → 0. Anda
  do último ponto para trás até achar `d = x − x[i] > 0` (float).
  `i` = o último → `y[último]`; nenhum (x ≤ x[0]) → `y[0]`; senão
  `y[i] + (d · inv[i]) · (y[i+1] − y[i])`, tudo em float, nessa ordem.
  Em x exatamente sobre um ponto interno a conta cai no trecho da
  esquerda (dá `y[i]` a menos do arredondamento).

## 2. Conjuntos de curvas

`cTSMotiveCurveSet` (base vtable `0x15d9d28`; BV `0x27c3560`): vetor de
`cTSMotiveCurve` (0x2C bytes cada). No base a classe concreta é
`cTSMotiveCurveArray<16>` (sempre 16 curvas).

**Carregar** (base `0xb910dc`, BV `0x51cadc`; `STR#` do grupo dado): para
cada texto `i = 1..nº de textos` (o STR# conta de 1), a curva `i − 1` é
limpa e, se o texto não é vazio, lida dele (§1). Texto vazio = curva sem
pontos (vale 0). Se o STR# tem menos textos que curvas, aviso
"cTSMotiveCurveSet::LoadFromFile : String table is missing motive curves."
e as que faltam ficam **constantes 1** (um ponto (0; 1)).

**Humor** ("Happy Curve Data"): um conjunto por idade, carregados uma vez
(BV `0x485fb4`: STR# globais 0x226 idoso, 0x227 adulto, 0x228 adolescente,
0x229 criança, 0x22A criança pequena, 0x22B bebê, 0x22C/0x22D animais;
cada curva com máximo 32 pontos). O do Sim (base `0xb75484`, BV
`0x486544`) sai dos testes de idade da pessoa (base vtable `0x690..0x6a4`
sobre o dado de pessoa 58): bebê 0–1, criança pequena 2, criança 3–15,
adolescente 16–18, adulto 19–50, idoso > 50; **nenhum (idade negativa) =
adulto**.

**Pontuação da autonomia** ("Interaction Scores Curve"): um
`cTSMotiveEffects` por Sim (base vtable `0x15d9dbc`, em `cTSPerson +0x554`;
BV `+0x624`). Ao ligar o Sim (base `0xb925d8`) **cada curva começa como
a identidade** (−100; −100)–(100; 100), máximo 32 pontos; depois
`LoadMotiveEffects` (base `0xb773b0`, BV `0xad3454`) carrega: campo 0x33 do
OBJD ≠ 0 → STR# desse número no grupo do próprio objeto; senão pela idade
(os mesmos testes): idoso 0x28A, adulto 0x28B, adolescente 0x28C, criança
0x28D, criança pequena 0x28E, bebê 0x28F, animais 0x290/0x291, nenhum =
0x28B. (Só BV: o dado de pessoa 84 bit 0x200 carrega 0x298.)

## 3. `cTSMotives` e o laço de 60 ticks

Vtable base `0x15d9e38` (BV `0x27c0a60`), em `cTSPerson +0x6e8` (BV
`+0x83c`). Dois conjuntos de 16 motivos em float: **atual** em `+0x08`,
**antigo** em `+0x48` (é o "second set" do PERS). Vtable base: `0x0c`
padrões (`0xb94528`: tudo 0, energia 70, fome −40), `0x10` atualizar,
`0x14` contagem (16), `0x20`/`0x24` ler atual/antigo, `0x28`/`0x2c`
gravar atual/antigo (**sem prender**), `0x30`/`0x34` referências.

**Atualizar** (vtable `0x10`, base `0xb945f0`, BV `0x486718`):

```
para (motivo, dado de decaimento) em (15,101) (8,97) (9,95) (14,99) (7,93) (5,96) (6,94):
    motivo += (float(dado) / 24) / 30          (float; o BV: dado · (1/720) e também 0 ← 100, 1 ← 106)
ambiente (13) = cTSEnvironmentScoreManager (vtable 0x20 com a pessoa), se houver
para os 16 motivos: prende em [−100, 100] e copia para o conjunto antigo
(BV: chama UpdateMood (0x34) e UpdateMetaMood (0x38) em seguida)
```

**Quem chama**: `cTSPerson::OnSimulatorTick(tick, ativo)` (base
`0xb876bc`, BV `0xad1040`), por tick e por Sim, com `ativo`:

```
se (último + 60 ≤ tick):             (último em cTSPerson +0x264 base / +0x2e4 BV)
    último = tick
    se dado 29 "Motives Static?" == 0:
        motivos.Atualizar()
        base: UpdateMood (abaixo), que chama UpdateMemories(falso, avisar)
        BV:   UpdateMemories(falso, avisar) (o humor já veio da Atualizar)
dado 27 "tick counter" += 1
se dado 29 < 2: cada Set Motive Change soma o seu passo ao motivo (float) até o limite, sem passar
se dado 1 "Money Amount over Head" ≠ 0: dado 1 = 0
```

Então o **decaimento diário é do motor**, em degraus de 60 ticks: com 30
ticks por minuto são 720 passos por dia de 24 h, cada um de
`dado / 720`: o valor do dado é por dia de jogo, como se supunha, mas
aplicado de 2 em 2 minutos. Os motivos só são presos a ±100 nesse passo:
entre dois passos uma árvore ou um Set Motive Change podem deixá-los fora.

## 4. Humor (`UpdateMood`)

**Base** (`cTSPerson`, `0xb76208`):

```
soma = 0; pesos = 0
para m em 0..15:                       (16 curvas do conjunto de humor do Sim)
    pula m ∈ {0, 1, 2, 10, 11, 12}     (máscara 0x1C07)
    pula se o bit m do dado de pessoa 118 "Motives Not Included in Mood" está ligado
    v = motivo atual m (float);  w = curva_m(v)
    soma = fmaf(v, w, soma);  pesos = pesos + w        (fmadds/fadds)
humor = soma / pesos + float(dado de pessoa 163 "Mood Booster")     (sem teste de pesos = 0)
motivo 3 = humor                       (gravar sem prender)
UpdateMemories(falso, avisar)
motivo 4 (Humor+Memória) = motivo 3
```

**BV** (`cTSMotives` vtable `0x34`, `0x486b78`): pula 2, 10, 11, 12 e os
do dado 118 (inclui 0 e 1), não divide com pesos = 0, prende o humor em
±100, e com uma expansão (`FUN_000ace90(0x80)`) ainda limita o humor por
uma tabela de constantes globais 0x17F pelo dado de pessoa 20.
`UpdateMetaMood` (`0x485e68`) = motivo 4 ← motivo 3 preso. Nos dados do
base as curvas de humor de 0–5 e 10–12 valem 0, então a lista de motivos
não muda a conta; o que muda é o **dado 163** e a **trava**.

**Conferido nos lotes**: o Mortimer (N001_Lot11, #197) tem dado 163 = 200
e humor salvo **232,1** (motivo 3 e 4), conjunto antigo 100,0: o humor do
base não é preso (o antigo é o atual preso pela Atualizar seguinte). Quem
mexe no dado 163: "Sub - Boost Mood" (grupo `0x7FA323EB`) põe, "Sim - Mood
Boost - Update" (global 0x393) baixa.

## 5. Motivos vistos pelas árvores

`cTSObject::InterpValue` (BV `0xb15f2c`, donos 0x0E/0x0F/0x11): lê o float
do motivo e arredonda **metade para longe do zero** (`(int)(v ± 0,5)`),
**sem prender**; a escrita grava o valor no float (o ponteiro de float
volta junto). O Humor que as árvores leem pode passar de 100 (Mortimer:
232).

Nos desejos, o fator de humor do `CalculateWantPriority` (BV `0x509c54`)
lê o motivo 3 em float e **trunca** para inteiro (`(int)`), não arredonda.

## 6. Pontuação da autonomia (`cTSMotiveEffects`)

- **Pontuação atual** (vtable `0x34`, base `0xb92a68`): soma, para
  m ∈ {3, 5, 6, 7, 8, 9, 13, 14, 15} (pula a máscara 0x1C07 e também 4 e
  11), de `curva_m(motivo atual m)`, em float; vezes `1.0f / (16 − 6)`.
- **Pontuação da interação** (vtable `0x38`, base `0xb92b80`): os mesmos
  motivos; anúncio da entrada para a idade do Sim (TTAB vtable `0x10c`,
  struct {tipo de personalidade, mín, faixa}; ausente = 0): tipo 0 →
  `float((short)(mín + faixa))`; senão `fmaf(0,001f, float(p) · float(faixa),
  float(mín))` com `p` = dado de pessoa da tabela base `0x114c4e8` (tipo →
  dado, invertido): 1/2 → 2, 3/4 → 3, **5/6 → 4**, 7/8 → 5, 9/10 → 6,
  11/12 → 7 (os pares invertidos: 1000 − p), 13 → 9 … 22 → 18; depois
  `anúncio · 0,001f` e soma `curva_m(motivo + anúncio)`; vezes
  `1.0f / 10`. O FBA usa a diferença das duas.
- O BV (`0xe1dd08`/`0xe1dbb8`, `0x1be21b4`) soma também o motivo 0 nos
  humanos (e 1 nos animais, sem o 13).
- **Piores motivos** (vtable `0x3c`, base `0xb92de0`): os motivos fora de
  0x1C07, 3, 4, 11 e da máscara pedida, em ordem crescente de valor; os N
  primeiros que a tabela de permitidos deixa.

## 7. No código (rodada n17)

`src/vm/motives.*`, `src/vm/autonomy.cpp`, `src/vm/lot_resume.cpp`,
`src/vm/lot_save.cpp`:

- **Curvas** (`MotiveCurve`): leitura por `sscanf`, ponto inserido no
  lugar com o primeiro x valendo, até 32, inversos guardados, valor pelo
  caminho do jogo em float (sem `fma` implícito: `#pragma clang fp
  contract(off)`); STR# com menos de 16 textos (ou sem STR#) = constante 1;
  texto vazio = 0; idade negativa = adulto; curvas próprias pelo campo 0x33.
- **Set Motive Change**: cada pedido entra no fim da lista, mesmo com outro
  do mesmo motivo (`TrySetMotiveDelta` BV `0xad24e8` faz `push_back`): os
  dois correm (antes o novo trocava o velho, inferido).
- **Motor** (`tickMotives`): a cada 60 ticks (o relógio das memórias,
  `Person::memoriesTick`), com o dado 29 = 0: `updateMotives` (decaimento
  `dado/24/30` na ordem do base, trava ±100, conjunto antigo
  `Person::oldMotives`), humor, memórias, Humor+Memória; depois dado 27,
  Set Motive Change (sem trava) e dado 1. As árvores leem o float
  arredondado (metade para longe do zero) e sem trava (`motiveView`); o que
  escrevem vale como está até a próxima atualização. O lote carrega e grava
  os dois conjuntos sem prender.
- **Humor** (`computeMood`/`moodFrom`): a conta do base (§4) com `fmaf` e o
  dado 163, sem trava.
- **Autonomia** (`scoreAds`, `adValue`): motivos 3, 5–9, 13–15 em float, as
  duas somas × `1.0f/10` e depois a diferença, anúncio por `fmaf`, tipos
  5/6 no dado 4.

**Provas** (`s2pkg mood-sweep`, ctest `mood_sweep` com `OS2_DATA`): nos 69
lotes que vêm com o jogo há 55 Sims; o humor salvo sai do conjunto antigo
salvo + dado 163 **exatamente** (mesmo float) em 47 e com diferença de no
máximo 3,05·10⁻⁵ nos outros 8 (os bairros foram salvos pela versão de PC,
que arredonda diferente); 5 têm dado 163 = 200 e humor acima de 100
(N001_Lot11 #197, N002_Lot10 #160, N003_Lot46 #135, Tutorial_Lot19 #146 e
#148); Humor+Memória = Humor nos 55. Pelo conjunto atual só 44 batem: o
humor é do antigo.

`s2pkg autonomy … --ticks 14400` antes/depois (N001_Lot11, Lot68, Lot49):
o Mortimer (#197) começa com humor 232 (antes caía para 33 em uma hora
porque o 163 não entrava) e fica acima de 220 até as 19:42; às 20:02 o
dado 163 já voltou a 0 (pelas árvores do "Mood Boost") e o humor é 24; o
decaimento por dia é o mesmo (Lot49 termina igual, ±2 nos
motivos); os desejos empatados mudam de slot (Lot68: os dois empregos do
Adriano Jonas trocam de lugar; Lot49: o medo "Cobrador!" do Duque vai do
slot 2 para o 3); as escolhas de autonomia mudam onde os empates e as
frações mudam (Lot11, Lot68 depois da 1ª hora).

Fica fora (no `TODO.md`):

- o **ambiente** (motivo 13): `cTSEnvironmentScoreManager` (BV `0x5805a0`,
  `GetEnvironmentRoomID` `0x57ffa8`, constantes no construtor) não foi
  lido; o motivo fica com o valor salvo;
- o tique em que o primeiro passo de 60 cai depois de carregar (se o
  `+0x264` volta do arquivo e qual relógio o simulador passa não foi
  conferido); aqui é o mesmo relógio das memórias, que começa em 0;
- com pesos somando 0 o jogo divide por 0; aqui o humor fica só o dado 163.
