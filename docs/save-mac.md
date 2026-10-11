# Estado salvo do lote (XOBJ, PERS, ObjM): o que o binário de Mac lê e escreve

Fontes e convenções como em `docs/primitives-mac.md`. Funções:
`cEdithObjectModule::Save`/`write_pre_object`/`write_post_object`,
`cTSObject::Write`/`Read`/`ReadHeader`/`WriteTimers`, `cTSTreeStackElem::Read`,
`cTSPerson::Write`, `cTSInteraction::Read` (BV). **Cobertura parcial**: o
formato que o BV escreve (versão do módulo 0xCE) e as condições de versão do
leitor, que dizem como ler os lotes do jogo base (versão 0xAD) e os mais
velhos. Cada recurso começa pelo nome de 64 bytes do arquivo (como
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

## 5. O que falta

- `cTSPersistDB*`/`cTSSaveGameInfo` (o que entra no pacote do lote e do
  bairro além do módulo de objetos) não foram vistos.
- Os campos do token, da tabela de relacionamentos e dos timers, e o resto
  do PERS depois dos dados de pessoa no formato base (o leitor
  `cTSPerson::Read`, 4140 bytes, tem os portões por versão).
