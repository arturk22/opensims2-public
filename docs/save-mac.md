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

Não verificado: se o jogo recusa um pacote sem os registros de partes que
não mudaram (por exemplo sem `0x3053CF74` para objetos que nunca tiveram).

## 7. O que falta

- Os campos do token, da tabela de relacionamentos e dos timers; o
  `cTSRoute::SaveToStream` (rota em andamento).
- Quem responde à mensagem `0xBC` além do módulo de objetos e do simulador
  (a ordem exata) e o gravador das `SREL` do lote.
- O campo +0xB0 do `SIMI` e os blocos `0x2A4` da pessoa no SDSC.
