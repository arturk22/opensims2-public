# Estado salvo do lote (XOBJ, PERS, ObjM): o que o binário de Mac lê e escreve

Fontes e convenções como em `docs/primitives-mac.md`. Funções:
`cEdithObjectModule::Save`/`write_pre_object`/`write_post_object`,
`cTSObject::Write`/`Read`/`ReadHeader`/`WriteTimers`, `cTSTreeStackElem::Read`,
`cTSPerson::Write`, `cTSInteraction::Read` (BV). Cobre o formato que
o BV escreve (versão do módulo 0xCE), as condições de versão do leitor
(como ler e **escrever** os lotes do jogo base, versão 0xAD), o PERS inteiro
(§3), a tabela `objt`, o `SIMI`, o estado de rota e a lista do que um save
de lote contém (§5–§6). Cada recurso começa pelo nome de 64 bytes do arquivo (como
`src/formats/lot_objects.cpp` já faz).

## 1. A versão escolhe o layout (binário)

Todos os leitores pegam a versão do contexto do stream
(`(*stream->ctx)[0x134]()`), que é a **versão do módulo de objetos do lote**
(o u32 depois do 0 no ObjM 1). Ela vale para todos os XOBJ/PERS do lote: o
jogo não decide pelo tamanho. Os XOBJ "do layout velho" que o código atual
acha por tentativa (87 campos e frames sem "running as/on") são estados
órfãos que o ObjM não lista e o jogo nunca lê.

Número de campos de object data lidos (`cTSObject::Read`; o resto até 114
fica zero):

| versão | campos | versão | campos |
|---|---|---|---|
| ≤ 0x7C | 72 | 0xB2 | 98 |
| 0x7D | 78 | 0xB3–0xB5 | 99 |
| 0x7E–0x85 | 79 | 0xB6–0xBA | 101 |
| 0x86–0x88 | 80 | 0xBB | 102 |
| 0x89–0x90 | 83 | 0xBC–0xBE | 103 |
| 0x91–0x93 | 84 | 0xBF | 104 |
| 0x94–0x97 | 85 | 0xC0–0xC5 | 108 |
| 0x98 | 86 | 0xC6–0xC7 | 111 |
| 0x99–0xA2 | 87 | 0xC8 | 112 |
| **0xA3–0xB1** | **88** (jogo base: 0xAD) | ≥ 0xC9 | 113 (o BV escreve 0xCE) |

Outros portões do leitor: semi-atributos só se versão > 0x79; os 8 campos
do token temporário + o token se > 0x65; arrays e a lista de u16 depois
deles se > 0x70; o u32 de posse se ≥ 0x8F (0x8E–0x8F têm um u32 a mais
antes); nomes de material se > 0x80; a flag persistente u16 é sempre lida e
só vale se ≥ 0xAB; efeitos novos se ≥ 0x93 (0x65–0x92: formato legado);
overrides de material se > 0x94; timers se > 0x95. No frame da pilha de
árvores (`cTSTreeStackElem::Read`): o u16 do ID do objeto só se > 0x19, e
**os u16 "rodando como"/"rodando sobre" só se ≥ 0xA5** (antes valem o dono
do frame).

Correção para `dataCount()` do código atual: 0xA4 lê 88 campos mas frames
**sem** os campos de "running" (0xA3–0xA4); 0xD8 (expansão) lê 113, não 114.

## 2. XOBJ (`cTSObject::Write`, BV)

```
cabeçalho: 4 × f32 0; f32 y; f32 x (posição em tiles × fator); u32 nível;
           u16 1; f32 elevação; u32 grupo multi-tile; u16 (+0x298)
u16 n, n × i16 atributos ; u16 n, n × i16 semi-atributos ; 8 × i16 temps
N × i16 object data (tabela acima) ; 8 × i16 campos do token temporário
token (GUID, flags, ...) ; u16 n arrays, cada um u16 n + n × i16
u16 n + n × u16 (lista em +0x194) ; u32 posse (+0x184)
u16 n + n strings (nomes de material) ; u16 flag persistente
pilha de árvores (cTSTreeStack) ; tabela de relacionamentos (vtable 0x1AC)
slots: u16 −101 (marcador), u16 n, n × u16
efeitos: u16 0 | 1 + dados ; overrides de material: u16 n + n × 3 strings
timers (WriteTimers): u32 n; por timer u32(+0x10), u32(+4), u16(+0x14),
           u32(+0xC), u32(+8), u32(+0), 8 × u16 parâmetros
```

Isso bate com a ordem que `parseTail` já lê; os nomes de cada campo do
timer não foram decifrados.

## 3. PERS (`cTSPerson::Write`, BV)

Depois do XOBJ inteiro: o bloco do animador (`WriteSAnimator`, começa pelo
tag 0xDEADBEEF), 16 motivos f32 (vtable 0x5CC), outros 16 f32 (vtable
0x5D0), **204** dados de pessoa no BV (o base tem 170), u32 0, dois objetos
(+0x5D4, +0x5D8: as filas — interação e sub-interação, inferido), vetores e
listas de registros (`cMotiveInc`: u32 motivo, f32, f32; `cObjectRecord`:
objeto, u32, u8), u16, u16, objeto opcional (u32 0/1 + dados), mais um
vetor, uma string, 3 × u32, 8 × u16, u32, um vetor e 2 × u32.

Ordem exata do leitor (`cTSPerson::Read`, BV `0xadf658`; `v` = versão do
módulo; `u16`/`u32`/`f32` pelos métodos 0x18/0x20/0x30 do stream, string
pelo 0x3C):

```
XOBJ inteiro (cTSObject::Read)
v < 0x0B: 28 × f32 legados (descartados)
v ≥ 3:  animador (ReadSAnimator)
v > 3:  16 × f32 motivos            (vtable 0x5CC)
v > 7:  16 × f32 (vtable 0x5D0; v ≤ 7: zera)
v > 10: N × i16 dados de pessoa, o resto até 204 zerado
        N por versão: ≥0xCE 204 | 0xCC–0xCD 201 | 0xC7–0xCB 200 | 0xC3–0xC6 199 |
        0xBD–0xC2 195 | 0xBC 194 | 0xBA–0xBB 193 | 0xB9 192 | 0xB8 189 | 0xB7 186 |
        0xB3–0xB6 180 | 0xB0–0xB2 179 | 0xAE–0xAF 178 | **0xA8–0xAD 170** | 0xA2–0xA7 167 |
        0xA0–0xA1 166 | 0x9F 165 | 0x9C–0x9E 163 | 0x9A–0x9B 162 | 0x99 161 | 0x97–0x98 158 |
        0x8B–0x96 157 | 0x8A 144 | 0x88–0x89 124 | 0x85–0x87 123 | 0x83–0x84 122 | 0x82 121 |
        0x80–0x81 119 | 0x7F 118 | 0x7D–0x7E 112 | 0x7C 111 | 0x7B 107 | 0x78–0x7A 104 |
        0x77 102 | 0x75–0x76 92 | 0x73–0x74 91 | 0x6E–0x72 89 | 0x6D 88 | 0x6C 83 |
        0x6A–0x6B 81 | 0x29–0x69 80 | ≤0x28 64
        (sem nenhum dos motivos-espelho 0x5D–0x6A em ≠ 0, o leitor põe valores padrão neles)
v > 0x0C: u32 (sempre 0 no BV)
v > 0x2A: interação atual (+0x5D4, GetCurrentAction)      — cTSInteraction, sempre presente
          última interação (+0x5D8, GetLastAction)          — idem
          u32 n + n × interação: a FILA (+0x584, as "top actions")
          u32 n + n × cObjectRecord (+0x60C, objetos que as ações referenciam)
v > 0x2B: u32 n + n × cMotiveInc (+0x618)
v > 0x2C:
  v > 0x9A: u16 (+0x644, mexido por ClearSubActions/SetCurrentSubAction)
  v ≥ 0x9E: u16 (+0x646, mexido por SetCurrentAction)
  v > 0x71: u32 tem_sub; tem_sub ≠ 0 → interação: sub-ação atual (+0x5DC)
            u32 n + n × interação: fila de sub-ações (+0x5AC)
  v ≥ 0x9D: string (+0x900)
  v > 0xA8: u32 +0x90C, u32 +0x910, u32 +0x914, 8 × u16 +0x918 (último "find best
            action": ícone/dados, GetLastFBAIconData)
v > 0xB3: u32 (+0x930)                                 — só BV
v > 0xB4: u32 n + n × interação (+0x5E0, candidatas do FBA)  — só BV
v > 0xCC: 2 × i32 (+0x938, +0x93C)                       — só BV
```

**Jogo base (0xAD)**: termina depois dos 8 × u16 do FBA. Ou seja, PERS base =
XOBJ + animador + 16 f32 + 16 f32 + 170 i16 + u32 + 2 interações + fila +
registros de objeto + incrementos de motivo + u16 + u16 + u32 (+ sub-ação)
+ fila de sub-ações + string + 3 u32 + 8 u16. O escritor do BV
(`cTSPerson::Write`) segue a mesma ordem com tudo da versão 0xCE.

### Interação na fila (`cTSInteraction::Read`, campos por versão)

```
u32 ; u32 GUID único (contador global) ; u16 +0xC (pessoa?) ;
u16 stack object (+0xE) ; u16 alvo (+0x10) ; [> 0xA5: u16 quem iniciou (+0x12)] ;
u32 entrada do TTAB (+0x14) ; 4 × u16 (+0x18, parâmetros) ; u32 (+0x20) ;
u16 (+0x24) ; f32 (+0x28)
[≥ 0x3E: u32 (+0x54)] [> 0x6E: u16 (+0x30)] [≥ 0x72: u16 +0x2C, u16 +0x2E]
[≥ 0x7C: u16 +0x40, u16 +0x42] [> 0x96: string nome (+0x44), u32 +0x38, u32 +0x3C]
[> 0xBA: u16 +0x32]
```

**IDs** (rodada 9): o u16 +0x40 é o ID da interação (`GetGUID`, vtable
0x110; o que `FindInteraction` e `UnLinkInteractions` procuram) e o u16 +0x42
o da interação ligada. `AssignUniqueGUID` (BV `0x4c5b20`) pede o próximo ao
simulador (BV `0x4989e0`): o global 36 "Current Interaction GUID" (pula 0,
soma 1), salvo com os outros globais no SIMI; por isso os IDs seguem de onde
pararam depois de carregar. O u32 +0x50 é outro contador: ao ler, um global
do jogo (`DAT_027797f4`) fica com o maior valor lido.

Os nomes (+0xE stack object, +0x10 alvo, +0x12 quem iniciou, +0x14 entrada
do TTAB, +0x44 nome) vêm dos getters (`GetStackObject`, `GetTargetObject`,
`GetWhatInitiated`, `GetTreeTabEntry`, `SetName`).

## 4. ObjM (`cEdithObjectModule`)

- Instância 1 (`write_pre_object`): u32 0, u32 versão (0xCE no BV), tag
  `ObjM`, u32 n, n × (u32 ID do objeto, u32 save type).
- Instância 2 (`write_post_object`): u32 n + n × u32 (tabela em +0x1070,
  só entradas cujo objeto existe), u32 contagem, u32 n + n IDs de objeto
  (lista em +0x1084), u32 0, string (o nome do lote/arquivo), assinantes de
  mensagens (`WriteSubscribers`). Os grupos de objetos (`WriteObjectGroups`:
  u16 2, u32 n, n × (u32 ID, grupo)) também são escritos.

## 5. Os outros registros do estado (binário)

**`objt`** (tipo `0x6F626A74`, instância 0; `ObjectSaveTypeTable::Write`,
BV `0x4e6f64`): tag `objt`, u32 versão **0x55**, u32 0; para cada definição
de objeto com save type ≠ 0 (seletores do lote e depois os globais):

```
u32 GUID ; u32 versão da árvore Init ; u32 versão da Main ;
u32 campo 0x3A do OBJD ; u32 campo 0x3B do OBJD ;
u32 nº de strings do STR# de catálogo (instância = campo do OBJD, grupo 0x100) ;
u16 save type ; u16 campo 9 do OBJD (tabela de interações) ;
string (nome do catálogo/objeto) ; u32 tipo (0 pessoa, 1 objeto, 3 o outro
caso da vtable 0x24) ; u32 campo 0xD0 do OBJD
```

e um u32 0 no fim. O save type liga o ObjM (`ID → save type`) à definição.

As duas "versões de árvore" (`cTSObjectSelector::GetInitTreeVersion`/
`GetMainTreeVersion`, BV `0x51d6bc`/`0x51d76c`) vêm da árvore que a tabela
de funções dá para Init (entrada 0) e Main (entrada 1; 0 se o objeto não
tem Main), lidas uma vez e guardadas no seletor, por `cTSBehavior` vtable
0x74 (BV `0x48d9f0`): o campo *tree version* do cabeçalho do BHAV raiz nos
16 bits de cima **mais a soma** dos *tree version* da raiz e de todas as
árvores que ela alcança (cada nó com opcode ≥ 0x100 empilha a árvore
chamada; cada uma conta uma vez). Conferido contra os lotes
(`vm/save_types.cpp`, `s2pkg objt-check`): dos 25.608 registros com
definição, 20.807 batem em todos os campos; os outros são de definições
mais velhas que as do jogo lançado. Os outros campos: 0x3A/0x3B são os
campos do OBJD nos offsets 0xB4/0xB6; o nº de strings é o do STR# 0x100 do
grupo semi-global (os rótulos dos semi-atributos); o nome é a string 0 do
CTSS do campo 0x29 do OBJD, ou "*" + nome do OBJD sem catálogo (os Sims: o
primeiro nome); o tipo é 1 (sem nome) nas partes de multi-tile, 0 nos
outros (3 em 12 árvores, não decifrado); o campo 0xD0 é o próprio GUID
(nos Sims, o GUID original do OBJD, 0x7FD96B54).

**XOBJ / XMTO / PERS**: o tipo do registro é o CLSID da classe do objeto
(`GetGZCLSID`): `XOBJ` (`0x584F424A`) objeto comum, `XMTO` (`0x584D544F`)
objeto de vários tiles (`cEdithMTObject`, que escreve depois do XOBJ os
dados de multi-tile e o ID do líder), `PERS` (`0x50455253`) Sim. Instância =
ID do objeto, grupo `0xFFFFFFFF`. Os pacotes guardam cópias velhas de outro
tipo para o mesmo ID; vale só a do tipo da classe.

**Por objeto**, `cTSObject` vtable 0x410 (BV `0xb00fb0`) grava: o estado
acima; o registro `0x3053CF74` (`cTSDataTables`, instância = ID) se o
objeto tem tabelas de dados, e **apaga** esse registro se não tem; e a parte
do grafo de cena (`cObject`, tipo `0xFA1C39F7`, que o código já lê).

**`cObject` e a pose salva** (rodada 13; `nTSSG::cObject::LoadFromStream`,
BV `0x65fae8`): depois dos skins/materiais (v > 0xE) vem posição (3 f32) e
orientação (quatérnio x, y, z, w), e então `u32 n` nós de transformação,
cada um `string nome ; 3 f32 translação ; 4 f32 quatérnio` (+ um f32 se
v < 0xF), que o jogo aplica ao nó de mesmo nome (vtable 0xf8). O primeiro é o
nó do próprio objeto (nome = arquivo do OBJD + `_objt`, na posição do
objeto: o que o código chamava de "definição"); os outros são **os ossos do
modelo como estavam posados ao salvar**. Por isso uma cadeira de jantar
empurrada para baixo da mesa (semi-atributo 11 = 2, "Chair - Push In" do
semi-global `0x7F0B61F3`) volta empurrada: o `slot_chairdining_trans` salvo
tem y = 0,373 (o último quadro do `o2c-chairDining-pushIn`), e o "Start Live
Mode" não toca de novo a animação quando o semi-atributo já é 2. O
`cAnimatable` (envoltório, tag `0x7D4F2BB8`) põe a fila de animações depois
(§7c). O app desenha o objeto com essa pose (`Lot::Object::nodes`,
`savedPose`/`posedModel` em `formats/skinning`), os slots e o assento do Sim
seguem os ossos posados (`VmObjectView::localPose`) e o ator de animação do
objeto começa dela e a mantém quando as animações param
(`AnimPlayer::setBasePose`/`setHoldPose`).

**Estado de rota** (`0xBC66BAEC`, instância = ID do Sim;
`cTSRoutingObjectAggregate::SaveToStream`, BV `0x56eff0`):

```
u32 0xBC66BAEC ; u32 versão 6 ; string "cTSRoutingObjectAggregate" ;
u32 (i16 +0x44) | 0x10000 se +0x48 ; f32 +0x4C ;
u32 pausa_marcada ; [se ≠ 0: 4 × u16] ;
u32 n + n × u16 (lista +0x70) ;
u32 tem_rota (0/1) ; [cTSRoute::SaveToStream da rota de base]
```

Para um Sim parado: `tem_rota` 0, lista vazia, pausa 0.

**`SIMI`** (instância 1; `cTSSimulator::Write`/`Read`, BV `0x49985c`/
`0x499c20`). Layout (little-endian no arquivo), com o que depende da versão
(= versão do módulo de objetos, 0xAD no base, 0xCE no BV):

```
u32 "SIMI" ; u32 versão ; u32 0
N × i16 globais (STR# 0x81), N pela versão (Read):
    v<0x12: 16 (+ u32 velocidade) ; <0x40: 32 ; <0x70: 33 ; <0x75: 34 ;
    <0x79: 35 ; <0x7C: 36 ; 0x7C: 37 ; 0x7D–0x8B: 40 ; 0x8C: 43 ; 0x8D: 44 ;
    0x8E–0x91: 45 ; 0x92–0xA0: 46 ; 0xA1–0xA2: 50 ; 0xA3–0xA6: 51 ;
    0xA7–0xA9: 52 ; 0xAA: 53 ; 0xAB–0xAE: 54 ; 0xAF–0xC4: 55 ;
    0xC5–0xC9: 56 ; 0xCA: 57 ; ≥0xCB: 58
  → base (0xAC/0xAD): **54**, como o código lê; BV (0xCE): 58.
u32 +0xA0  contador de ticks que avançaram o relógio (negativo → 0)
u16        → +0x11C (o Write grava +0x11E: conferir)
u32 +0x98  fundos da família (cópia; v>1)
u32 +0xB4  semente do gerador aleatório do simulador (v>0xD)
u32        semente do gerador aleatório global (serviço vtable 0xDC; v>0xD)
u32 +0x9C  variação de fundos pendente (SetFunds: novo − +0x98; somada no
           próximo tick; v≥0xF)
v≥0x25: u32 +0xA4 valor do lote (padrão 4500) ; u32 +0xAC valor dos objetos
        da família ; [v≥0xAD: u32 +0xA8 valor dos objetos do lote] ;
        u32 +0xB0 (não identificado)
v>0x25: 6 relatórios de despesa: u32 tipo + valores (Read: tipo 0 → 7,
        tipo 1 → 8, outro → 10; o BV grava tipo 2 + 10). Os lotes base
        (0xAD) têm tipo 1 + 8 u32 (36 bytes cada): 76 + 108 + 38 + 216 = 438
        bytes, e 434 nos 0xAC (sem o +0xA8), como nos pacotes de N002.
```

Ao ler, os globais 25 e 53 **não** são tirados do arquivo (o jogo mantém
os da sessão), o 34 (dia da semana) é recalculado como dia % 7, o 15 vem de
um serviço e o 24 dos globais do jogo; a velocidade (global 16) fora de
2..4 vira 2. Os globais 13/14 = fundos % 10000 e / 10000 (`SetFunds`),
46/47 = valor do lote / 1000 e % 1000 (`SetLotValue`), 48/49 = valor dos
objetos (`UpdateTotalObjectsValue`).

O objeto `FUN_0007f178(stream, versão)` passado pela vtable 0x4C do
stream (no `SIMI` e no `objt`) só marca a versão no contexto: não escreve
bytes. O nome de 64 bytes do começo de cada registro vem da camada de
gravação (`cTSSimSystem` vtable 0x118: arquivo, stream, tipo, instância).

## 6. Escrever um lote que o jogo aceite (receita)

O save de lote (`cTSSaveLotController::SaveLot`, BV `0x884ec4`) é uma
máquina de estados (um passo por chamada); o que importa para o pacote:

| passo | o que faz |
|---|---|
| 0–2 | pergunta se salva (diálogo); sem salvar pula para o fim |
| 5 | atualiza a família/lote na vizinhança (fundos, `cTSLotInfo`) |
| 6 | abre o pacote do lote para gravação (CLSID `0x0B54E37D`) |
| 7 | mensagem **`0xCD94C8EE`** ("vai salvar": os objetos se preparam) |
| 8 | o persist system liga o pacote ao lote (`0x120`) |
| 9 | **apaga do pacote** todos os recursos dos tipos `0x2DB5C0F4` e `0x6E9C59CF` (dados de importação/exportação de família/vizinhança) e registra um contexto `0xCDCBE0E3` |
| 10 | se for modelo de lote: mensagem `0xDC` |
| 11 | `WorldDB` grava o mundo (paredes, pisos, terreno, telhados, `HOUS`, ...) |
| 12 | mensagem **`0xBC`** com o pacote e o lote: cada subsistema grava os seus registros (módulo de objetos §4–5, simulador `SIMI`, scene graph `0xBA353CE1`, relacionamentos `SREL`, ...) |
| 13 | a vizinhança grava (`+0x3C`→`0x14(pacote)`) |
| 14–15 | `cTSLotInfo` (`0x6C589723`): alturas do terreno amostradas a cada 10 unidades e a miniatura `0x3B2271C0` gravada no pacote do lote |
| 16+ | miniaturas/imagem do lote e fecha |

**Vizinhança** (o que muda fora do lote):

- `FAMI` (`cTSFamily::Write`, BV `0x47c258`; versão 0x55 no BV, **0x4E**
  no base): u32 tag ; u32 versão ; u32 0 ; u32 instância do lote (+0x30) ;
  [v≥0x50 u32 lote de negócio] ; [v≥0x55 u32 lote de férias] ; u32 +0x40 ;
  **u32 fundos** ; u32 +0x10 ; u32 +0x60 (flags; v≥7) ; u32 n + n membros
  (ID de Sim, u32) ; u32 +0x3C (v>0x4A) ; [v>0x4E u32 +0xC] ;
  [v≥0x51 u32 fundos de negócio]. Os fundos gravados são os do
  **simulador** quando a família é a do lote aberto (`cTSFamily::Funds`), ou
  seja, o global 13/14 do lote. Conferido contra os `FAMI` de N001 (52/56/44
  bytes, v 0x4E).
- `SDSC` (`cTSNeighbor`; versão 0x20 no base): ao sair do lote cada Sim
  copia para o seu SDSC os dados de pessoa da lista
  `cTSNeighbor::sGetPersistentDataFields` (BV `0x492524`, 143 pares
  (dado de pessoa, versão do SDSC em que entrou); os de versão ≤ 0x20 são os
  do base: 2–7, 70, 9–18, 58, 60, 61, 56, 57, 63, 65, 67–69, 74, 81–101,
  106–153, 155, 156, 159, 46–51, 22, 23, 160–162, 165, 92), mais os dois
  blocos `0x2A4(1)`/`0x2A4(2)` da pessoa (não identificados) e as relações
  (`cTSNeighborhood::SavePersistentData`, BV `0x4d7ff8`).
- `NGBH` (`cTSNeighborhood::Write`, BV `0x4dc43c`; 0xCB no BV, **0x6F** no
  base): tag, versão, 0, dois u32, uma string, 16 i16 (globais da
  vizinhança), e três mapas (ID → registro). O save de lote só precisa
  mexer nele se mudar o que ele guarda (inventário/memórias da vizinhança).

O caminho seguro para um save nosso:

1. Copiar todos os registros do pacote original (terreno, paredes, pisos,
   telhados, `HOUS`, imagens, `3IDR`, etc.): eles só mudam no modo
   construção.
2. Reescrever, com a **mesma versão do módulo** do original (0xAD no base;
   o leitor decide tudo por ela, §1): `ObjM` 1 e 2, `objt`, um
   `XOBJ`/`XMTO`/`PERS` por objeto vivo (instância = ID), o `cObject`
   (`0xFA1C39F7`) de cada objeto, `0x3053CF74` de quem tem tabelas, o
   estado de rota (`0xBC66BAEC`) de cada Sim, as `SREL` do lote
   (`0xCC364C2A`, instância = de << 16 | para) e o `SIMI`.
3. **Apagar** os registros de estado de objetos que já não existem e as
   cópias de tipo errado (o jogo apaga o `0x3053CF74` vazio; os restos que
   os pacotes originais guardam mostram que ele não apaga os outros, mas um
   pacote limpo é aceito do mesmo jeito: o leitor só procura o que o ObjM
   lista).
4. Fora do pacote do lote: o bairro guarda o que o jogo mostra fora do
   lote (fundos da família no `FAMI`, dados de pessoa no `SDSC`, a
   miniatura e o resumo do lote, `cTSNeighborhood::SaveLotInfo`); um save
   que mude fundos/Sims precisa atualizar o pacote do bairro também.

O que o nosso save grava (`s2pkg lot-save`, `s2pkg hood-save`;
`vm/lot_save.*`, `vm/save_types.*`, `vm/lot_records.*`, `vm/hood_save.*`):
os passos 1–4 acima, com o `SIMI` campo a campo (os globais e a cópia dos
fundos em +0x98 da VM, o resto como lido), um registro `objt` construído
do OBJD para cada tipo novo (§5), o `cObject` reposicionado de quem andou
e copiado de outro lote para quem foi criado, o estado de rota parado
(versão 4 do base, 12 bytes zero) para Sims novos, as `SREL` do lote
iguais às relações da VM e, no bairro, `FAMI`/`SDSC`/`SREL`/`NGBH`. Sem
`0x3053CF74` (nenhum lote do jogo tem). Não grava rotas em andamento
(`cTSRoute`), as tabelas de dados nem a miniatura/`cTSLotInfo`.

Não verificado: se o jogo recusa um pacote sem os registros de partes que
não mudaram (por exemplo sem `0x3053CF74` para objetos que nunca tiveram).

## 6b. Rodada 16 (f): jogar → salvar → bairro → recarregar

- **Vizinhos feitos em jogo** (bebês, NPCs, moradores): gravados como
  qualquer vizinho (`cTSNeighborManager::WriteNeighbors`, BV `0x53a76c`):
  SDSC, SDNA, membro do FAMI e, do bebê, a árvore da família; os arquivos
  de personagem copiados da sessão para `Characters` com o nome atual
  (`docs/cas-mac.md` §7.2).
- **Valores do lote no save** **[binário]**: o passo 7 do `SaveLot` manda a
  mensagem `0xCD94C8EE`, que o `cTSLot::DoMessage` atende com o
  `ComputeAndStoreLotData` (`docs/neighborhood-mac.md` §11.1): os globais
  46–49 e o SIMI +0xA4…+0xB0 são refeitos antes de gravar. O `captureLot`
  chama `computeLotData` no começo; antes, depois de um dia de jogo, o
  global 49 (valor dos objetos) do lote salvo diferia do recarregado
  (`lot-save --check`: "global 49 742 vs 977").
- **`s2pkg play-loop <bairro> <lote> <saída> [--ticks N] [--birth mãe,pai]
  [--npc GUID[:roupa]]`**: copia o bairro para `<saída>/<P>`, joga o lote
  com os escritores de personagem instalados, salva pelo `saveGame` do app,
  lê o bairro de volta (família do lote e vizinhos novos com arquivo, o que
  a tela de carregamento e o bairro mostram) e recarrega o lote comparando
  a VM (`compareLotState` + `compareNeighborhoodState`, que agora confere
  também nomes e DNA dos vizinhos). N001_Lot11, 43.200 ticks (um dia), um
  bebê roteirizado: 3 NPCs feitos (jornaleiro, motorista, carteiro, com nomes
  sorteados), tudo igual depois de recarregar. `hood-save` também instala os
  escritores e copia os arquivos novos: `hood-save <saída>/N001 --ticks
  43200 --check` dá 25 de 26 lotes iguais (o N001_Lot36 difere num Sim no
  meio de uma interação, como já acontecia no `lot-save --check` do main).

## 7. O que falta

- Os campos do token, da tabela de relacionamentos e dos timers; o
  `cTSRoute::SaveToStream` (rota em andamento).
- Quem responde à mensagem `0xBC` além do módulo de objetos e do simulador
  (a ordem exata) e o gravador das `SREL` do lote.
- O campo +0xB0 do `SIMI` e os blocos `0x2A4` da pessoa no SDSC.
- ~~Depois de um dia, um Sim salvo no meio de uma interação às vezes volta
  com dados de pessoa diferentes (N001_Lot36)~~: não se repete no main da
  rodada 17 (`lot-save --check` de 43.200 ticks e `hood-save N001 --ticks
  43200 --check`: 26 de 26 lotes iguais). O que faltava para o lote
  recarregado *seguir* como o que salvou está na §7b.
- ~~Animações e rotas em andamento recomeçam ao recarregar~~: §7c (rodada
  17, frente r17-saveanim).

## 7b. Rodada 17: o lote recarregado segue como o que salvou

`s2pkg lot-save <lote> <saída> --ticks N --continue M`: depois do `--check`,
as duas VMs (a que salvou e a recarregada) rodam mais M ticks com os mesmos
números aleatórios (o gerador do VM não é o do jogo e não é gravado: a
recarregada recebe o estado do primeiro) e são comparadas a cada tick; a
saída dá o primeiro tick que difere, os objetos e o erro de quem foi
reiniciado só numa delas (`OS2_CONTINUE_TRACE=id` imprime a pilha desse
objeto a cada tick). Corrigido com isso **[binário]**:

- **Frações dos motivos**: os 16 f32 do PERS (vtable 0x5CC) são os do motor
  de motivos; o VM arredondava ao carregar e o decaimento seguia do inteiro
  (a fome do N001_Lot36 já diferia em 30 ticks). Agora o VM recarrega o
  float e grava o float do motor.
- **Set Motive Change em andamento** (`cTSPerson +0x618`): `cMotiveInc::Write`
  (BV `0xad5c44`) e `Read` (`0xad2764`) gravam `u32 motivo ; f32 variação por
  tick ; f32 limite` (o `TrySetMotiveDelta`, BV `0xad24e8`, faz variação =
  valor por hora / 60 / 30, como o VM; o `OnSimulatorTick`, `0xad1040`, soma
  até o limite). Eram descartadas: um Sim comendo ou dormindo durante o save
  parava de ganhar depois de recarregar.
- **Todos os atributos**: o `cTSObject::Write` grava a contagem do objeto (a
  do OBJD, no mínimo 8, ou a lida do save). O VM cortava os zeros do fim: o
  monte de lixo da lata chutada feito em jogo voltava com 7 de 8 atributos
  e a árvore de um tick dava erro ("attribute 7 of 7") e o reiniciava. Os
  semi-atributos também vão todos.

O que sobra (`--continue` em N001_Lot11, N001_Lot36, N002_Lot2, N002_Lot3,
N003_Lot52 depois de meio dia ou um dia): **a animação recomeça** (o primeiro
desvio em quatro dos cinco lotes é um Animate Sim, 0x6A, que volta ao
início do clipe) e **a rota recomeça** (N003_Lot52: o Romeo andando).
O jogo grava as duas: a fila de animações no fim do `cObject` de quem anima
(`nTSSG::cAnimatable::SaveToStream`, BV `0x68c868`: tag `0x7D4F2BB8`, versão
0x10, "cAnimatable", o `cObject`, um f32 do nó de animação (velocidade?),
u32 tem-nó + dois valores dele (vtables 0x17C/0x180, não decifrados), dois
u32 globais, u32 +0x13C, u32 +0x140, f32 +0x144, u32
+0x148, u32 +0x14C, uma string + +0x16C, u32 n e n pedidos pela vtable
0x36C → `on_save_anim_request` (BV `0x695f34`: u32 0 nenhum, 1 pedido de
animação + `on_save_choreo_request_anim` `0x68d640`, 2 pedido com olhar +
`on_save_choreo_request_look_at` e o alvo); o leitor é o `LoadFromStream`
`0x695884` com os ramos por versão 8..0x10) e a rota pelo
`cTSRoute::SaveToStream` dentro do registro `0xBC66BAEC`. Ler os pedidos
(`cIAnimRequest`, classe `0x9D0CE60`, e os `cIChoreoRequestAnim`) e ligar ao
Animate Sim que retoma (`cAnimatable::OnTryElement`, BV `0x689f34`) fica para
uma frente própria.

## 7c. Rodada 17 (r17-saveanim): animação e rota em andamento **[binário]**

**A fila de animações** (`formats/scene_anim.*`). No registro `cObject`
(`0xFA1C39F7`) de um objeto animável, os cabeçalhos vêm de fora para dentro
(um Sim: `cPerson` v3, `cLocomotable` v4, `cAnimatable` v0x10, `cObject`
v0x11) e os dados de cada classe depois dos da base: o `cObject`
(`LoadFromStream` BV `0x65fae8`: modelo, skins, pose, nós, e com v > 0xF
`u32 n` + n × (string, string, f32) dos morphs do rosto), depois o
`cAnimatable` (`LoadFromStream` `0x695884`; o `SaveToStream` é o `0x68c868`,
que o symxfer chama de `DetermineAppropriateSocialAnim`):

```
f32 estica-esqueleto ; u32 tem-nó [2 × transformação] ;
[v < 0xF: 2 × (u32 tem [choreo + string + u32]) ; u32 ; u32]
u32 contador de pedidos (DAT_02779c34) ; [v > 0xB: u32 DAT_02779c30] ;
[v < 0xA: u32] ; [v > 8: u32 +0x13C] ; [v > 0xA: u32 +0x140] ;
[v > 0xC: f32 +0x144] ; [v > 0xD: (v = 0x10: u32 +0x148) ; u32 +0x14C] ;
transformação +0x160 ; u32 n ; n pedidos
```

(transformação = 3 f32 + quatérnio). Cada pedido (`on_save_anim_request`
`0x695f34` / `on_load_anim_request` `0x69ab40`): u32 tipo 0 nenhum, 1
animação, 2 olhar. O tipo 1 é o `cAnimRequest` (tag `0x09D0CE69`, v11;
`0x6abc9c`/`0x6ab788`; nomes pela ordem dos getters na vtable da classe):
id, nome, canal ("sil/sim/sih/siu" do Animate Sim, "osl/ovl/ovm/ovh/ovu" do
Animate Overlay, "obj" do Animate Object), u32 +0x24 (5 no Animate Sim, 4 ou
6 nos overlays), u32 +0x28 tOEType (2 normal, 4 idle, 7 facial: os números
do `vm::oetype`), u16 objeto da pilha, u16 árvore de eventos, interrompível,
f32 pressa, f32 ×2 eventos de ação, transição de entrada/saída, +0x4C,
TargetID (o objeto de IK), transformação, slot, só-no-cômodo, GUID do
objeto da árvore, State, MovedObjectID, string, u32 tem-callback (com
callback o layout não é conhecido: a fila não é lida), +0x94/+0x98, +0x9C;
depois o `cChoreoRequestAnim` (tag `0xC9DE69B6`, v1 nos lotes do base, v2 no
BV com um f32 +0xE4; `0xd52b0c`/`0xd52a6c`) com o `cChoreoRequest` (tag
`0x3AFF30DB`, v7; `0xd9404c`/`0xd94588`): estado, início (relógio do motor,
ms), fim, porção feita, porção final, regra de expiração (5 com
blend de saída, 3 sem), prioridade (= nível do canal), peso, a função de
peso (`cBlendFunction2` `0x8B969D64`: entrada, sustentação, saída), espelho,
voltas, velocidade, desvio de direção, 3 × (transformação + f32), **agora**
(+0xA8), +0xAC/+0xAD, +0xB0, +0xB4, externo, e **+0xBC, o quanto já tocou
(ms de animação)**; por fim o nome do recurso ("…_anim") e o tipo do
tratador de eventos. O tipo 2 é o `cAnimRequest` + `cChoreoRequestLookAt`
(tag `0x2A6F2E97`, v5; `0xd68284`): o choreo de animação, string do alvo,
f32 (×2 com v ≥ 5), 3 u32, [v > 3 u32], o u32 do
`on_save_choreo_request_look_at` e o ponto, u32, f32.

**Animate Sim retomado.** O `try_animate_sim` (BV `0x69d634`) guarda o ID do
pedido no estado primitivo do elemento da pilha (`cITSTreeStackElem`
vtable 0x4C/0x48): `0x40000000 | 1 se espelhado | (++DAT_02779c34 & 0xFF)
<< 21`; com o estado ≠ 0 ele procura o pedido (`AnimRequestByID`) em vez de
criar outro. O VM faz o mesmo (`AnimationState::requestId`,
`Frame::primitiveState`, que agora vai e volta no save), grava o pedido do
Animate Sim em andamento na fila do Sim e, ao recarregar, o quadro do topo
parado num Animate Sim cujo estado é o ID de um pedido da fila continua dele
(`resumeAnimateSim`): mesma animação, o tempo tocado (+0xBC), os eventos
ainda não alcançados, os ticks que faltam. Nos lotes do jogo há 9 pedidos de
Animate Sim, um só com o quadro esperando por ele (Tutorial_Lot19, o piano).

**Overlays.** Os pedidos que não são de Animate Sim voltam como overlays
(os idles e rostos do Animate Overlay, cujas árvores de eventos o sistema de
idles espera: sem eles o 9999 nunca chegava e os flags "tocando" ficavam
presos; as poses de carregar; o Animate Object com a sua árvore de
eventos). A árvore do evento roda no próprio objeto animado (rodada 18:
`nTSSG::cCustomEventHandler2`, BV `0x780ad4`), então o MovedObjectID (+0x80)
fica 0, como em todos os pedidos dos lotes do jogo; o código vai pelo GUID.
Objetos cujo `cObject` é nu (sem `cAnimatable`, como os pratos de comida nos
lotes do jogo) não têm onde guardar as poses: ficam sem elas.

**Rota.** O `cTSRoute::SaveToStream` (BV `0x4731dc`, dentro do registro de
roteamento) **não grava caminho**: versão 3, os IDs (quem anda, alvo, outro),
o slot (`WorkOutSlotIndex`) ou, sem slot, os parâmetros do alvo, e alguns
campos (+0x124, +0xAC/+0xAD, 4 f32, u32, 4 u16); o `LoadFromStream`
(`0x4736bc`) refaz a lista de objetivos (`CreateBestGoalPointList`): o jogo
planeja de novo de onde o Sim está. O VM faz isso para um save sem mais nada
(`resumeRoutes`): a instrução de rota no topo planeja de novo
(`Frame::routeResume`, sem a árvore pré-rota, que já rodou), e o estado do
quadro sob uma árvore da rota é refeito pela árvore (pré-rota: o resultado
dela segue; "CT - Route Failure Feedback": a falha, parâmetro 0, segue para
a pós-rota; pós-rota: chegada com 0, falha com o resultado; as outras —
checagem de objetivo, ao longo da rota, portal, rota bloqueada — planejam de
novo quando voltam). Para o lote recarregado seguir **igual** ao que salvou,
o VM grava também um registro próprio ao lado dos do jogo (tipo
`0x4F533256` "OS2V", grupo `0xFFFFFFFF`, instância = ID do objeto;
`vm/thread_state.*`; o jogo ignora tipos que não conhece): o estado de rota
dos quadros, a rota com o caminho e a locomoção (a curva pelo que a fez), o
atrator de olhar, a parte de destino (o slot sintético do "Move Out Of Way",
"gerenciado"/"seguro" do Go Here), e na instância 0 o ruído do A* do
planejador e a contagem de ticks do VM. Só vale se a pilha salva for a
mesma (árvores e nós); senão, o caminho do jogo.

**Conferido** (`lot-save <lote> --ticks 21600 --continue N`): antes o
primeiro desvio era o Animate Sim que recomeçava (tick 7 no N001_Lot11, 12
no N003_Lot52) ou a rota (N002_Lot2, tick 1); agora N003_Lot52 segue igual
por 1657 ticks (até o "Controller - Age"), N001_Lot11 e N002_Lot2 até o
tick 27, quando o "Script - Pleasantville - …" feito de novo ao recarregar
(objeto global, `updateSimObjects`) reescreve os motivos: o lote salvo sem o
bairro (`lot-save`) perde a marca de que o roteiro já rodou; N002_Lot3 até o
tick 121 (o "Controller - Short Term Memory Effects", que lê as memórias do
bairro, que o `lot-save` não grava). `OS2_CONTINUE_ALL=1` segue até o fim e
conta as diferenças por minuto; `OS2_CONTINUE_WATCH=id:motivo` mostra a
instrução que mudou um motivo; `OS2_DEBUG_ROUTE_DIFF=1` imprime as duas
rotas quando o estado de rota difere. Os lotes do jogo: `lot-save --check`
sem ticks deixa iguais todos os `cObject` que não mudaram (a fila só é
reescrita quando o que toca difere do que ela diz).

Inferido / não feito: o callback de fim (+0x90 do `cAnimRequest`, classe
`0x6C9EAF21`) e os campos +0x24, +0x4C, +0x94…+0x9C não decifrados; os
registros de rota do jogo continuam como lidos (o VM não grava a sua rota no
`cTSRoute`); `cLocomotable`/`cPerson` (8 bytes depois da fila) intocados; os
flags de empurrar da fila (dado de pessoa 72 "continue as current") ainda
não vão no save (N001_Lot36: o Beau volta com 72 diferente).

