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

- ~~o **ambiente** (motivo 13)~~ (rodada n33, §8);
- ~~o tique em que o primeiro passo de 60 cai depois de carregar~~
  (rodada n33, §8.5);
- com pesos somando 0 o jogo divide por 0; aqui o humor fica só o dado 163.

## 8. Ambiente (motivo 13): `cTSEnvironmentScoreManager`

Rodada n33 (Ghidra MCP, só leitura; base 1.0 e BV). Endereços do base
pelo RTTI (`26cTSEnvironmentScoreManager`, vtable base `0x15dddf0`); nomes
do `baserom.sym` e do BV (`GetEnvironmentRoomID` BV `0x57ffa8`, construtor
BV `0x5805a0` / base `0xc8342c`). O base e o BV fazem a mesma conta; o BV
só desloca os campos do `cRoomInfo` (+4) e tem uma escolha a mais na
parede (abaixo). **Vale o base.**

### 8.1 O gerenciador

Um serviço global (`cTSGlobals` vtable `0x50`). Vtable base: `0x0c`
`DoMessage` (`0xc85560`), `0x10` init, `0x18` `InitTuningCurves`
(`0xc83698`), `0x1c` `SetTuningConstant(i, f)` (`+4 + 4i`), `0x20`
`GetEnvironmentScore(pessoa)` (`0xc841ac`), `0x24`
`GetEnvironmentRoomID(sala, local)` (`0xc84620`).

Constantes do construtor (`+4..+0x24`, nada as muda no jogo base):

| | valor | uso |
|---|---|---|
| `c0` `+0x04` | 14 | teto da niceness média de parede |
| `c1` `+0x08` | 23 | teto da niceness média de piso |
| `c2` `+0x0c` | 0,25 | peso do piso |
| `c3` `+0x10` | 0,25 | peso da parede |
| `c4` `+0x14` | 0,2 | peso da luz |
| `c5` `+0x18` | 0,55 | luz "escura" |
| `c6` `+0x1c` | 0,9 | luz "clara" |
| `c7` `+0x20` | 0,5 | (não usado na conta) |
| `c8` `+0x24` | 5 | lado da célula de fora, em tiles |

A curva (`+0x28`, um `cTSPiecewiseFunction` de até 32 pontos) sai do texto
1 (o primeiro, o STR# conta de 1) do **STR# `0x2BC` "Environment Score
Curves" do grupo global `0x7FD46CD0`**: `(0;0) (1000;1000000) -- Max total
object niceness vs Room Size`, ou seja `1000 · x`. (O texto 2, "OLD", não
é lido.)

Mensagens (`DoMessage`): `0x8b` entrar no modo vivo
(`OnEnteringLiveMode` `0xc83790`: liga `+0x38`, esvazia o mapa, refaz
paredes, pisos, objetos e as notas), `0x8c` sair (desliga `+0x38`),
`0x8d`/`0x8a` hora do dia (`UpdateTimeofDay` guarda em `+0x39` se o
relógio diz 2; **ninguém lê `+0x39`**; refaz só as notas), e, com o
modo vivo ligado, `0x90` sala de ambiente do objeto mudou
(`OnObjectsEnvRoomChanged`: tira a contribuição da sala velha e põe na
nova), `0x91` niceness do objeto mudou (soma `preço · (nova − velha)/100`,
cada uma presa em ≥ 0), `0x92` crap do objeto mudou (soma `nova − velha`),
`0xcdbd0900` "tem modelo" mudou (põe ou tira o objeto). Quem manda:
`cTSObject::OnEnvScoreRoomChanged` (BV `0xb04f10`, quando `+0x288` ≠
`+0x28c`), `OnCrapScoreChanged` (BV `0xb05288`), `SetHasModel` (BV
`0xafbe64`). Paredes e pisos só são lidos ao entrar no modo vivo (no modo
vivo eles não mudam). Então a soma incremental é a mesma conta refeita do
zero com o estado do momento.

### 8.2 A sala de ambiente (`GetEnvironmentRoomID`)

```
sala −1      → −1
sala r > 0   → r + 0x10000
sala 0 (fora) → célula: cx = (int)((x − origem_x) / max(c8, 1)), cy idem (trunca),
                cada uma presa em 0..255; chave = cx + 256 · cy
```

A origem é a do lote no mundo (`cWorldDB` `0xdc`/`0xe0` = mínimo do
lote × tamanho do tile: 0 nos lotes). A sala é a de colocação (definição
2, 3ARY `0x09`, a mesma do dado de objeto 29): um quintal cercado é uma
sala, não "fora". A chave do objeto fica em `cTSObject +0x288` (BV),
posta junto com a sala (`SetRoom`, vtable `0x98`) por quem acha a sala do
objeto (`cEdithObjectModule::UpdateObjectsInRoom` BV `0x52e170` chama a
`0x24` com a sala e o local do objeto).

### 8.3 O `cRoomInfo` (um por chave, `std::map<long, cRoomInfo>` em `+0x2c`)

Campos (base, a partir do nó `+0x10`; no BV +4): `+0x10` fora, `+0x11`
fora já decidido (só informativos), `+0x14` nº de objetos de categoria
`0x16` (janelas), `+0x18` de categoria `0x17`, **parede** `+0x1c` niceness
`+0x20` crap `+0x24` contagem `+0x28` nota, **piso** `+0x2c` niceness
`+0x30` crap `+0x34` área `+0x38` nota, **objetos** `+0x3c` niceness
`+0x40` crap `+0x44` contagem `+0x48` nota. A entrada nasce ao ser pedida
(`RoomInfo` `0xc8474c`).

**Paredes** (`UpdateWallScores` `0xc84840`): cada parede do grafo 5 (o de
colocação: paredes e cercas, sem as saias de deck), os dois lados. Lado A
(o do localizador das pontas, à direita de a → b): a sala desse lado do
grafo; o ponto `meio + 0,05 · (dy, −dx)` dá a chave (só importa fora);
o revestimento desse lado (`cWorldDB` `0x88`) → produto de parede do
catálogo (tipo 0); **sem produto, o lado não conta**. Com produto:
`niceness += fmadds(preço, NicenessMultiplier)`, `crap += CrapScore`,
`contagem += 1`. Lado B igual, com o localizador oposto e `meio + 0,05 ·
(−dy, dx)`. (O BV, se o estilo da parede não bloqueia colocação, lê o
revestimento por outra vtable; no base não há essa escolha.)

**Pisos** (`UpdateFloorScores` `0xc84d8c`): cada sala `r` de 0 até o nº
de salas, cada tile da lista da sala (todos os andares, x por fora, y por
dentro; um tile com a máscara dos quartos que são da sala; a sala 0 tem os
tiles de fora de **todos** os andares): chave pelo local do tile;
`FloorTileNicenessAndCrapScores` (`0x103929c`): tile no lote → padrão do
piso → produto de piso (tipo 1) → `niceness = preço · NicenessMultiplier`
(fmuls), `crap = CrapScore`; sem produto, 0 e 0. A área soma 1 **sempre**.

**Objetos** (`UpdateObjectScores` `0xc8504c` + `AddObjectsContribution`
`0xc83830`), os que `ShouldProcessObject` (`0xc851bc`) deixa: no mundo
(`IsInWorld`), com modelo (`HasModel`, `+0x1d2` do BV, posto pela cena),
tipo do OBJD ≠ 2 (pessoa) e ≠ 6, e de um multi-tile só o líder. Na chave
`+0x288` do objeto:

```
m = fdivs((float)dado 7 "Niceness Score", 100)   (< 0 → 0, com aviso)
c = (float)dado 86 "Crapness Score"                (< 0 → 0, com aviso)
niceness = fmadds((float)preço, m, niceness)       (preço = campo 0x12 do OBJD, short)
crap += c;  contagem += 1;  categoria (dado 59) 0x16/0x17 → contadores
```

O preço é o `GetPrice` do objeto (base vtable `0xbc`: o campo `0x12` da
definição do próprio objeto, `+0x1e8`). Num multi-tile ele é
`cEdithMTObject::GetPrice` (BV `0xaf04ec`): o campo `0x12` da definição
**mestra** do grupo (a do catálogo; as partes têm 0). Sem isso a banheira
do N001_Lot18 não contava e o cômodo ficava 45 abaixo do salvo (§8.6).

**Notas** (`UpdateEnvScoreForRoom` `0xc85364`, float):

```
objetos = área > 0 ? clamp01(niceness_obj / curva(sqrt(área))) · ((1 − c2) − c3) · 100 : 0   (curva ≤ 0 → 0)
piso    = área > 0 ? min(1, (niceness_piso / área) / c1) · c2 · 100 : 0                      (≥ 0)
parede  = contagem > 0 ? min(1, (niceness_parede / contagem) / c0) · c3 · 100 : 0            (≥ 0)
base(chave) = clamp(parede + objetos + piso, ±100) − (crap_parede + crap_obj + crap_piso)    (0 sem entrada)
```

(`GetBaseEnvironmentScore` `0xc84558`; o crap não é preso.) Os máximos
dão 25 + 25 + 50 = 100.

### 8.4 A nota do Sim (`GetEnvironmentScore` `0xc841ac`)

```
k = chave do Sim (+0x288);  k < 0 → 0
luz = AggregateIllumination(local do Sim)
se k < 0x10000 (fora):
    fx = (x − origem)/c8 − 0,5;  fy idem;  ix = floor(fx), iy = floor(fy)
    s = Σ_{yy = iy−2..iy+2, 0 ≤ yy < 256} Σ_{xx = ix−2..ix+2, 0 ≤ xx < 256}
          w · base(xx + 256 yy),  w = tenda((fx − xx)·0,5) · tenda((fy − yy)·0,5)
    (tenda(t) = 1 − |t| até |t| = 1: base 0xc8338c; **sem normalizar**: os pesos somam 4)
senão s = base(k)
t = (luz − c5)/(c6 − c5)  (0,5 se c5 = c6), preso em 0..1
nota = clamp(s + (t − 1) · c4 · 100, ±100)
```

O termo da luz vai de 0 (luz ≥ 0,9) a −20 (luz ≤ 0,55).

**A luz.** `cTSObject::AggregateIllumination` (BV `0xafa538`) pergunta ao
objeto de cena do Sim (interface `0xdf8ec21`, `nTSSG::cObject`) a
`Illumination(local)` (BV `0x653d10`): a `LocationIlluminationStrength`
do `cLightingManager` (vtable `0x88`, BV `0x70051c`) =
`ColourToIlluminance(cLightMapManager::LookupFloorLightMap(local))`.
`LookupFloorLightMap` (BV `0x6f6a24`) acha a página de piso do tile do
local, o texel `(⌊frac(x)·8⌋, ⌊frac(y)·8⌋)` do tile (8 por tile,
`+0x50`), e devolve a cor do texel × `+0x70` (2, o construtor `0x6fa6e0`:
desfaz a escala 0,5 da página); sem página, preto.
`ColourToIlluminance` (BV `0x1808dd4`) = `fmadds(b, 0,11, fmadds(r, 0,3,
g · 0,59))`. Sem objeto de cena, 0. Ou seja: **o light map de piso
debaixo do Sim**, com a luz direta e a do sol já somadas.

### 8.5 Quem chama e quando

`cTSMotives` vtable `0x10` (§3): depois do decaimento, `motivo 13 =
GetEnvironmentScore(pessoa)` se o gerenciador existe; depois a trava e o
conjunto antigo.

**O tique.** `cTSPerson +0x264` (base; `+0x2e4` BV) é zerado no
construtor (BV `0xae1670`) e não é gravado nem lido do arquivo. O
simulador passa a cada objeto o seu contador de tiques (`cTSSimulator
+0xa0`, `TickAllObjects` BV `0x498544`), que **vem do SIMI**
(`cTSSimulator::Read` BV `0x499c20`: o u32 depois dos globais, preso em ≥ 0)
e sobe 1 depois de todos os objetos terem rodado
(`SimulateOneTick` BV `0x49a814`, só com o relógio andando). Então, ao
carregar um lote salvo com contador ≥ 60, o primeiro passo de 60 cai **no
primeiro tique** e os seguintes a cada 60 a partir dele.

### 8.6 No código (rodada n33)

`src/vm/environment.*`, `src/formats/environment_catalog.*`,
`src/vm/motives.cpp`, `src/vm/lot.cpp` (`setLotRooms`),
`src/vm/lot_resume.cpp`, `src/vm/lot_save.cpp`, `src/app/lot_lighting.cpp`:

- **Paredes e pisos** (`setLotEnvironment`, chamado por `setLotRooms` ao
  carregar e depois de cada mudança do modo construir): as somas por chave
  em `VM::environmentArchitecture`, das linhas do grafo 5 (os dois lados,
  revestimento → produto) e dos tiles de cada sala do 3ARY `0x09` em todos
  os andares. Produtos (`patternScores`): os XML do catálogo (`cost`,
  `nicenessmultiplier` padrão 1, `crapscore` padrão 0, mesmo escondidos ou
  obsoletos) e os scripts antigos (`floorPatterns.txt`, `wallPatterns.txt`:
  só o `crapScore`, "blank" 0,1 e "floor_plywoodFoundation" 1; as linhas de
  texto de catálogo estão comentadas no base, então o preço é 0).
- **Objetos e notas** (`environmentRooms`): somados quando a nota é pedida
  (uma vez por tique em que algum Sim dá o passo), a mesma conta das
  mensagens do jogo. **Inferido**: `HasModel` verdadeiro para todo objeto
  (os sem modelo do base são controladores de preço 0); o padrão de um tile
  com vários quartos da sala é o do primeiro quarto da máscara (o
  `FloorPattern` de um tile mascarado não foi lido até o fim).
- **Nota do Sim** (`environmentScore`): a conta do §8.4; um Sim fora do
  mundo (no trabalho, na escola) tem sala −1 e nota 0 (inferido: a sala que
  o jogo dá a quem sai do mundo). A luz vem de `VM::floorIllumination`, que
  o app liga ao light map de piso mostrado (`LotLighting::floorIllumination`:
  o texel em `floorTexel`, os dois mapas na mistura do crossfade, × 2,
  `ColourToIlluminance`; os light maps são feitos antes do primeiro tique).
  **Sem host (s2pkg, testes) a luz fica de fora**: o termo é 0 (como um
  cômodo claro).
- **O passo de 60** usa `VM::simulatorTicks` (o `+0xa0` do simulador: lido
  do SIMI, sobe no fim de cada tique, gravado de volta pelo lot-save) contra
  `Person::memoriesTick` (0 ao carregar, não gravado). Nos lotes do jogo o
  contador salvo vai de 1522 a 81662 (os com Sims): o primeiro passo cai no
  primeiro tique (antes: no 60º). Numa VM que começa do 0 (testes), no 61º.
- `OS2_DEBUG_ENV=1` imprime cada passo (Sim, sala, nota, luz).

**Provas** (`s2pkg env-sweep`, ctest `env_sweep` com `OS2_DATA`): os 55 Sims
dos 69 lotes do jogo, nota calculada do lote carregado contra o ambiente
salvo. 14 batem exatamente (17 até 0,5); 36 ficam até 20 abaixo da nota do
cômodo, o termo da luz que o headless não tem (8 exatamente −20, cômodos
escuros na hora em que o lote foi salvo; os outros dão luz entre 0,58 e
0,89); 2 não se explicam (Johnny e Jill no N002_Lot10, 21,3 abaixo: talvez
estivessem noutro lugar no último passo). No app (headless, `OS2_DEBUG_ENV`),
no N001_Lot68 salvo às 07:08, a Lilith e o Daniel no quarto escuro ficam com
luz 0,44 e 0,48, termo −20 e nota 47,236, **o salvo exato**. Com a luz do
app no primeiro passo (o app headless com `--run-vm 2` e `OS2_DEBUG_ENV`
em 14 dos 17 lotes com Sims; os 3 do tutorial não dão passo em 2 tiques),
dos 50 Sims **24 batem exatos** com a luz (N003_Lot46: Patrizio e Isabella
com luz 0,87 e 0,85; N003_Lot52: Romeo com 0,61; os quartos escuros com
−20) e 38 ficam a menos de 2 (os de fora do N003_Lot192 0,05 acima; o
Johnny e a Jill, no quarto escuro, 1,3 acima); o resto difere pelos nossos
light maps (Angela −12,6, Circe −14,9: a luz deles não é a do jogo naquele
ponto, ou as lâmpadas salvas acesas/apagadas diferem).

3 dias de jogo antes (main) e depois (`s2pkg autonomy --ticks 129600`,
semente padrão): no N001_Lot11 o ambiente passa de fixo (46 e 34) a variar
com o cômodo (médias 55, 30, 21, 33; 0 na escola; −100 do lado de fora junto
da lixeira, x ≈ 30: salvo no tique 58400, a célula tem a lixeira cheia, um
monte de lixo e três baratas, crap 175, base −173,8; o térreo, com pratos e
comida largados, cai a −38,5); no N001_Lot68 de fixo (66, 54, 47, 47) a
médias 24 a 62. Autonomia: no N001_Lot68, com 4 sementes, o depois teve
**incêndio no fogão nas 4** (antes, em nenhuma), com mortes: o cozinheiro
larga a comida no fogão entre um "Cozinhar" e o seguinte (o "Continuar a
Cozinhar" é um anúncio autônomo que perde para outros quando os motivos
mudam). Com o ambiente desligado as mesmas sementes não pegam fogo. A
leitura da rodada n32 (o fogo do Lot68 é do jogo) vale. **Medido antes do
rebase sobre a rodada n31b; na base atual não se repete** (rodada n33b,
§8.7: 2 incêndios em 13 sementes com o ambiente, 4 sem ele).

### 8.7 O ambiente e o fogão do N001_Lot68 (rodada n33b)

A rodada n33 viu o fogão do N001_Lot68 pegar fogo nas 4 sementes de 3 dias
"depois" e em nenhuma "antes". **Não se repete na base atual**: as medidas
da n33 foram feitas antes de ela ser rebaseada sobre a rodada n31b (rotas:
um goal check por tique, o passo bloqueado), que mudou a linha do tempo dos
dois lados. Com a base de agora (`s2pkg autonomy --ticks 129600`,
`S2PKG_FIRES=1`, sementes 0–12; "incêndios" = começos de fogo):

| N001_Lot68, 13 sementes | incêndios | mortes | de onde |
|---|---|---|---|
| main (sem a n33) | 4 (sementes 1, 3, 4, 9) | 19 | 1 cozinhando (o sorteio), 3 comida esquecida (`Sub - Set Burnt` pelo `Function - Main`) |
| n33 | 2 (sementes 7, 9) | 12 | 2 cozinhando (o sorteio) |
| n33 com luz de noite escura (`S2PKG_LIGHT=0.9:0.55`, sementes 1–12) | 2 (3, 7) | 11 | 1 cozinhando (a 7, igual à da n33: de dia não há termo), 1 comida esquecida (a congelada no forno, 20:45) |

No N001_Lot11 (sementes 1–6) nenhum incêndio nem morte em nenhum dos três.
Cada semente é caótica (o passo de 60 no tique do simulador já muda tudo
depois do primeiro tique), mas o ambiente calculado não aumenta os
incêndios; a luz que falta sem o app (até −20 nos cômodos escuros) também
não (o teste acima põe todo Sim no escuro à noite: −20 sempre depois das
19:00).

**O que tira o cozinheiro do fogão** (árvores; conferido no trace):

- **Assar** (`Cook - _Type - Bake`, semi-global dos fogões `0x7F033984`
  `0x201E`): põe a comida no forno, `Sub - Cook Start`, `Wait For
  Notify(0, 0)` (global `0x119`: ninguém notificou ainda: od 85 = 0, nada
  pendente no Idle for Input de 0 ticks, dado de pessoa 110 ≠ 1 → falso na
  hora) e então **global 30 ≠ 0** (o "Free Will", ligado por padrão) →
  `_Reserve Tile - Remove` e falso: **o cozinheiro sai do forno por
  desenho**, `Food Chain - Failure - Cook` sem balão, e o "Cozinhar" acaba
  ~180 ticks depois de começar. Com o Free Will desligado ele ficaria no
  laço `Idle` → `Get in Temp 0 - Cook Progress`. Quem o traz de volta é a
  comida: pronta, o `Push - Get Food` (`0x7F3C9675:0x2034`) empurra "Comer"
  nele com prioridade 50 (se ninguém interage com ela); o "Continuar a
  Cozinhar" é só um anúncio autônomo (ganha nas buscas quando a fome manda:
  escolhido 4–14 vezes por semente).
- **Fritar/cozinhar nas bocas**: o cozinheiro fica no laço até acabar; sai só
  se a interação dele é cancelada ou se `ShouldInterrupt` (BV `0xad6388`)
  diz sim (dado de pessoa 71 "non-interruptible" = 0 e a primeira da fila
  com prioridade maior que o dado 33; perguntado pelo Idle for Input com
  "allow push", `TryIdleForInput` BV `0xade664`). Quem o tira é **outro Sim
  indo ao forno do mesmo fogão**: o "Route - To Oven" (`0x2058`) chama `Sub -
  Shoo Sims off Tile` (`0x2056`) → global `Shoo - Sims from tile Object X is
  on` (`0x2FC`, X = o próprio Sim, od 11): Set to Next 8 acha quem está no
  tile dele com dado de pessoa 39 "route status" = 0 e empurra a interação
  221 do próprio Sim ("Afastar-se") com **prioridade 1 = 100**. (O 39: no
  base 1.0 o `UpdatePersonDataFields` do roteador, `FUN_00c64a70`, grava
  40/37/55/38 e, quando pedido, o 39: 1 em `0xc57cb4` e 0 em `0xc58130`, no
  começo da rota, e 0 em `0xc55514`, quando a pilha de rotas esvazia; no BV,
  `cTSRoutingObjectAggregate::UpdatePersonDataFields` BV `0x570674`, o 39
  sumiu. Então no jogo base só quem **anda** escapa do "Afastar-se"; quem
  frita parado leva, como no VM, que deixava o 39 em 0 sempre. Rodada n41:
  o VM o mantém como o base, `primitives-mac.md` §4j; o 1 do começo vem do
  `ProcessGoto`, e no BV o `PopRoute` ainda grava o 0.) No
  N001_Lot68 o lugar do forno (21,5; 25,25) e o das bocas (21,5; 25,75)
  ficam no mesmo tile, e o "Afastar-se" interrompe quem frita: a panela fica
  no fogão, queima e pega fogo 30 min depois (`Tuning - Cook Fire Time`).
  Foi assim nos três incêndios de comida esquecida da main (semente 3:
  Mary-Sue busca a comida congelada às 21:18, a Lilith frita o macarrão e é
  enxotada duas vezes, a panela queima às 21:33 e o fogo vem às 22:02;
  semente 9: o hambúrguer da Angela às 12:11). Tudo árvore do jogo.
- **O sorteio**: `Main - Cook - Loop` (`0x2032`), 6% por cozinhada com
  habilidade baixa (§4f do `primitives-mac.md`): os dois da n33.

**O motor** (Ghidra MCP, só leitura): o Find Best Interaction só roda da
árvore de ócio, então uma interação em curso nunca concorre com as novas; o
`FBA_ScoreInteraction` (BV `0xae2c4c`) multiplica a diferença dos motivos
pela atenuação, posse, `GetAutonomyBias`, relacionamento, tempo e
temperatura, e nada ali olha a interação atual. O ambiente (13) entra na
conta: a `GetCurrentScore` do base (`0xb92a68`) soma os motivos fora da
máscara `0x1C07`, do 4 e do 11, isto é 3, 5–9 e 13–15, vezes `1/(16 − 6)`.
