# Achados nos dados do jogo

Itens do `TODO.md` marcados como "confirmar o que foi inferido" que dava para
resolver olhando os próprios arquivos do jogo: rótulos do editor de SimAntics
que vieram no `objects.package` (STR# do grupo `0x7FE59FD0`), strings do
`Sims2.exe` (o disassembler do editor, `cEdithLanguage::Cat*`, fica em
≈0xB67000–0xB69F00, com as strings de cada função em ordem inversa à do
código), a tabela de símbolos do binário do Mac (`references/sims2_mac_decomp`)
e varreduras estatísticas sobre todos os pacotes.

Ferramentas (reutilizáveis):

- `tools/datahunt/`: leitor DBPF/QFS/STR# mínimo em Python (`dbpf.py`), cache de
  todas as BHAVs (`bhavs.py`), histograma dos operandos de uma primitiva
  (`ophist.py 0x1C`), rótulos do editor (`labels.py cc de`), parentescos do
  SREL contra a árvore genealógica (`famrel.py`) e tick contra som
  (`tick_vs_sound.py`). Rodar de uma pasta de rascunho (gravam caches no
  diretório atual); a raiz dos dados vem de `$OS2_GAME`.
- `s2pkg anim-events <Res> [filtro]`: uma linha por evento de ANIM (nome,
  duração, distância/velocidade de locomoção, tick, tipo, dados).

Contagens de BHAV são de árvores únicas por (grupo, instância): 22.396 BHAVs
em `TSData/Res` fora de `Locale`.

---

## 1. Tipos sem nome: `0xAC598EAC` e `0xEBFEE342`

**Pergunta.** O que são esses dois tipos, que não constam nas referências?

**Método.** Censo de tipos em todos os 807 pacotes (onde aparecem, instâncias,
conteúdo), referências (`OpenTS2/.../GroupsTypes.cs`) e símbolos do Mac.

**Evidência.**

- `0xAC598EAC`: 916 recursos (292 no `objects.package`, 208 em
  `Catalog/Bins`, 398 nos personagens). É um `cGZPropertySetString` XML com
  `age`, `gender`, `species`, `skeltype/skelgroup/skelinst`, `listcnt` e as
  listas `le*/lk*/ls*` (as partes de corpo por categoria). O OpenTS2 já o
  nomeia: `SIM_APPEARANCE = 0xAC598EAC` (o SimPE chama de AGED, "Age Data").
- `0xEBFEE342`: 635 recursos, um por pacote de personagem e de lote, com
  grupo `0xFFFFFFFF` e instância 1 (o do `objects.package` usa o grupo
  `0x7F45284D`). É um property set binário
  (magic `0xCBE750E0`) com as chaves `"0"` = `"The Sims 2"`, `"1"` = versão
  (`0.14.0.842` … `1.0.0.917`; o `objects.package` tem `1.0.0.913`) e, a
  partir de `1.0.0.913`, `"2"` = 1 (u32). Os símbolos do Mac têm exatamente
  isto: `cTSContentRegistry::WriteIdentifierRecordIntoSegment(segment,
  vector<pair<string,string>>)`, `ReadIdentifierRecordFromSegment`,
  `SegmentIdentifierValueWithKey(segment, char const* key, …)`,
  `VersionForSegment(segment, string&)` e
  `MarkSegmentAsBelongingToProduct(crc, segment, unsigned short product)`.

**Conclusão.** Confirmado: `0xAC598EAC` = aparência do Sim por idade (AGED,
`SIM_APPEARANCE` no OpenTS2). `0xEBFEE342` = registro de identificação do
pacote do `cTSContentRegistry`: produto, versão do jogo que gravou o pacote e
(provavelmente) o número do produto (1 = jogo base). Não há código que
precise dele para ler os dados.

## 2. OBJD: campo `0xD0`

**Pergunta.** O offset `0xD0` é o índice do modelo (STR# 0x85) ou o "object
model guid 1" dos rótulos do editor?

**Método.** Rótulos do STR# `0x7FE59FD0`/0xCC (campo *i* no offset
`0x40 + 2i`); para os 3.314 OBJDs do `objects.package`, resolver
`STR# 0x85[campo 72]` contra os nomes de todos os CRES do jogo (4.751).

**Evidência.** `0xD0 = 0x40 + 2·72`: o rótulo 72 é "object model guid 1 -
Read Only" e o 73 "object model guid 2 - Read Only". O campo 73 vale 0 nos
3.314 OBJDs. Dos 2.657 OBJDs com STR# 0x85, 2.531 apontam para um CRES que
existe; 105 apontam para uma entrada vazia, `noModel` ou `null` (controladores,
emissores, menus: objetos sem modelo de propósito); 21 apontam para modelos
que não existem no jogo (conteúdo cortado: `sofaSantaFe`, `cineCam`,
`staircaseStraightValueWithNoRailing`, …), 2 para fora da tabela.

**Conclusão.** Confirmado: o campo 72 (offset `0xD0`) é o índice do modelo no
STR# 0x85 (99,2% coerente; o resto é sem modelo de propósito ou conteúdo
cortado). O rótulo do editor é herança do The Sims 1; o "guid 2" (73) não é
usado.

## 3. Duração do tick das animações

**Pergunta.** 1 tick = 1 ms? Com isso a caminhada sai rápida.

**Método.** `s2pkg anim-events` lista os 17.797 eventos de ANIM; 11.730 são
sons (tipo 1014, `sound_name=…`). Para as animações com um único som perto de
t = 0 e cujo clipe tem uma duração só (variantes a ±5%, ≥ 1,5 s), comparar a
duração da animação em ticks com a do som em ms
(`tools/datahunt/tick_vs_sound.py`).

**Evidência.** 144 animações; razão duração/som: 92 entre 0,9 e 1,1 (pico em
1,0), as outras espalhadas (sons que terminam antes, veículos). Laços feitos
para casar com o som batem quase exatos: `a2o-polygraph-repair-loopTinker`
3.942 ticks × `pgraph_repair_tinker` 3,95 s; `a2o-neat-cleanWindow-loop2`
1.883 × 1,89 s; `t2o-aquarium-clean-loopScrubWipe` 2.033 × 2,055 s;
`a2o-sink-washhands-rinseHands` 5.275 × 5,255 s. Com outro tamanho de tick a
razão sairia longe de 1.

As caminhadas (cabeçalho da ANIM: distância por ciclo e velocidade =
distância / duração): `a-male-walk-alt0-normal` e `a-female-walk-alt0-normal`
andam 1,39–1,46 m por ciclo de 691–725 ticks, com dois eventos `foot_step`
(0 e 350): 2,0 m/s e 2,9 passos/s de 0,70 m. O 1,37 m/s vem de outras
caminhadas: `*-walk-reaper` (0,00137/tick) e as de idoso `e-*-walk-alt0`
(1,36 m/s); criança `c-walk-alt0` 1,6 m/s.

**Conclusão.** Confirmado: 1 tick de animação = 1 ms (o "1,45 passos/s" do
TODO eram ciclos, de dois passos cada). A caminhada normal de adulto anda
2,0 m/s; 1,37 m/s é a do Ceifador/idoso. **Para a VM/roteamento:** usar a
velocidade da animação de caminhada escolhida (cabeçalho `velocity` ×
1000/s), não um 1,37 m/s fixo.

## 4. TTAB com 96 bytes não explicados

**Pergunta.** O que são os 96 bytes da TTAB 0x48 "Reaction Interaction
Table" (grupo `0x7F9FC607`)?

**Método.** Reler as 836 TTABs do `objects.package` em Python com o layout do
`InteractionTable::parse` e ver o que sobra antes do nome.

**Evidência.** Só essa tabela sobra: versão 0x48, uma interação (ação
`0x2019`, guarda `0x0109`), contagens de anúncios (16, 0, 0, 0, 0, 0, 0), e
depois dos 16 anúncios lidos vêm mais 96 bytes zerados antes do nome
(u8 26 + "Reaction Interaction Table"). 96 = 16 × 6 bytes (um anúncio:
mínimo, delta, personalidade).

**Conclusão.** Explicado: são 16 anúncios vazios a mais (provável sobra do
editor gravando a tabela da segunda faixa etária com contagem 0). Corrigido:
o parser pula zeros em múltiplos de 6 antes do nome (`src/formats/simantics.cpp`,
teste em `test_formats`).

## 5. Idle for Input (0x11): onde fica o "allow push"

**Pergunta.** Na FreeSO (The Sims 1) o "allow push" é o u16 no byte 2; no TS2
é o 2 ou o 4? E o modo "Handle Sub Queue Interactions"?

**Método.** Histograma dos operandos das 101 instruções; strings do
disassembler; nomes das árvores.

**Evidência.** Byte 0–1: parâmetro decrementado (0, uma vez 2). Byte 2: 0 em
89, 1 em 12. Byte 4: 1 em 96, 0 em 5. Os 5 com byte 4 = 0 são as árvores de
espera por notificação ("Wait For Notify", "Wait for Notify - Ignore Hurry",
"Interaction Canceled?"), onde faz sentido não aceitar interrupção. Os 12 com
byte 2 = 1 são interações com sub-fila: "Interaction - Idle In Chair" (×2),
"Give Bottle", "Carry", "Play With", "Teach - Pupil", "Trade Off", "Friend -
Come Home", "Report Card" e uma das duas instruções de "Wait For Notify". O
disassembler (ordem do código): "Handle Sub Queue Interactions", "for ticks
in ", ", allow push" / ", do not allow push". Os bytes 8–10 (`02 00 03`) só
aparecem em 5 instruções, como sobra de outra primitiva.

**Conclusão (revista).** O u16 no byte 2 é o "allow push", como no The
Sims 1, e o byte 4 seria o "Handle Sub Queue Interactions". A leitura
anterior (byte 4 = allow push) não se sustenta no Main dos Sims:

- O Main de pessoa ("Function - Main - Check Queue", semi-global de pessoa
  0x205A) lê o dado de pessoa 79 "cn: Next Queued Interaction Object ID" e,
  se há algo na fila, chama "Wait For Notify" (global 0x119) com o
  parâmetro 1 = 1. Esse caminho vai à instrução 5, o Idle for Input
  `00 00 01 00 00 ...`: byte 2 = 1, byte 4 = 0. É o único Idle for Input
  desse caminho, então é nele que a interação da fila tem de entrar. Com o
  byte 4 como "allow push", o Main nunca rodaria a fila.
- Teste na VM (`s2pkg pie <N001_Lot11> Mortimer Newspaper --select 0
  --ticks 300 --motives 60`): lendo o byte 2, o Mortimer começa "Procurar
  Emprego" no tick 3 e anda até o jornal; lendo o byte 4, a entrada fica
  parada na fila e ele não sai do lugar.
- O byte 4 = 1 está nas esperas de dentro das interações ("_Handle SubQueue",
  "Interaction - Buy", "Pickup - Wait On Subqueue Interaction"), onde o
  "allow push" do The Sims 1 já não roda nada (a FreeSO só empurra fora de
  uma árvore de ação no TSO, e no TS1 empurraria uma interação aninhada,
  o que o TS2 chama de sub-fila). "Handle Sub Queue Interactions" nessas
  árvores é coerente com os nomes delas. A interrupção dentro de uma
  interação (cancelar, algo mais importante na fila) não depende do "allow
  push": o Idle for Input volta quando a interação é cancelada.
- O operando do TS2 mantém o layout do TS1 (como o Push Interaction) e
  acrescenta o campo novo no fim (byte 4); a ordem das strings do
  disassembler não diz em que byte cada campo está.

**Para a VM:** "allow push" no u16 do byte 2 (`src/vm/queue.cpp`). O modo
do byte 4 (sub-fila: dados de pessoa 26, 75–78, 157) não está feito.

## 6. Run Tree by Name (0x1C): modos e bytes 2–3

**Pergunta.** Os modos 0/1/2 e o u16 nos bytes 2–3.

**Método.** Rótulos do editor; histograma das 1.846 instruções; correlação do
byte 2 com o nome chamado, o modo, a tabela e a árvore que chama.

**Evidência.** STR# `0x7FE59FD0`/0xDE "how to call named tree": 0 "run in my
stack", 1 "run in stack object's stack", 2 "push onto my stack" (os mesmos
nomes e a mesma semântica da `VMRunTreeByName.cs` da FreeSO: 0 roda síncrono
e desvia pelo resultado, 2 empilha como subrotina, 1 roda na thread do stack
object). Byte 3 é sempre 0. O byte 2 vai de 0 a 46 sem relação com o que se
chama: "CT - Add Memory" aparece com 7 valores diferentes, "CT - Update Dirty
State" com 8; 103 dos 573 nomes têm mais de um valor; nada a ver com o modo, a
tabela, o número de argumentos/locais da árvore que chama ou a versão do
BHAV. Dentro da mesma árvore o valor se repete em 66% dos casos (cópia e
cola no editor). O disassembler não imprime esse campo (só tabela, nome, modo
e ", Passing in params …").

**Conclusão.** Modos confirmados pelos rótulos (0 ≠ 2). Os bytes 2–3 são
lixo do editor e podem ser ignorados. **Para a VM:** o modo 0 deve rodar a
árvore até o fim e desviar pelo resultado (como uma check tree), o 2 empilhar.

## 7. Relationship (0x1A): flag 0x08

**Pergunta.** O que é a flag 0x08 (e os relacionamentos dos lotes)?

**Método.** As instruções com 0x08; rótulos dos relacionamentos (STR# 0x102);
SREL dos pacotes de lote comparados com os IDs de objeto do lote
(`s2pkg lot-run --list`) e os neighbor IDs do bairro.

**Evidência.** Os 15 usos de 0x08 (com 0x04 = set, 0x01 = fail) são todos
de posse, sempre na variável 1: "Ownership - Get Amount in Temp 0",
"Ownership : Does Sim Own Object", "Ownership - Get Primary Owner NID",
"Ownership: Set Ownership to object", "Debug - Increment/Decrement
Ownership", "Interaction - Set Sim to Owner", "Init - Ownership Array",
"Bed Rel. - Adjust Ads". O "de" é um ID de objeto (Stack Object ID, meu
object data 11) e o "para" um neighbor ID (meu person data 31, local,
parâmetro). Os rótulos globais (`0x7FD46CD0`, STR# 0x102) chamam a variável 1
de "UI Flags/Ownership Value" (e as tabelas de alguns objetos, "Owned By (UI
Flags)"). Nos lotes, os SREL (`instance = de << 16 | para`) ligam IDs de
objetos do lote (133 de 164 "de" e 113 de 121 "para" em `N001_Lot11` são
objetos do lote); os que apontam para 1, 15 e 16 (a família Caixão, dona do
lote) têm 2 variáveis e a 1 vale 10, 15, −25, 30…

**Conclusão.** Confirmado: 0x08 = relacionamento de objeto com Sim: "de" é um
ID de objeto e "para" um neighbor ID; a variável 1 é o valor de posse. Esses
relacionamentos, e os entre objetos/Sims do lote por ID de objeto (sem 0x02),
ficam nos SREL do pacote do lote; os de 0x02 (neighbor IDs) no bairro.
**Para a VM:** implementar 0x08 em vez de falhar, e carregar os SREL do lote
por ID de objeto. *Feito* (`relationship` em `src/vm/primitives.cpp`,
`vm::loadLot`): SREL do lote com dois valores = posse (objeto → neighbor ID;
os "para" 1, 15 e 16 de `N001_Lot11` são um fone e dois controladores, não
donos); os outros por ID de objeto, Sims pelo neighbor ID; os de Sim para Sim
ficam com os do bairro.

## 8. SREL: parentescos 4–8 e bits de flags

**Pergunta.** Os parentescos 4–8 (tirados da ordem das strings do exe) e os
bits da variável 1 (nomes do SimPE).

**Método.** `tools/datahunt/famrel.py`: para cada SREL dos quatro bairros com
parentesco 1–9, testar se o "para" é filho/pai/irmão/cônjuge/neto/avô/
sobrinho/tio/primo do "de" pela árvore genealógica (`0x8C870743`). Bits:
contagem, valores de STR/LTR por bit, e as strings do exe com as mais curtas
(< 5 letras) incluídas.

**Evidência.**

| valor | SRELs | casa com | outro parentesco |
|---|---|---|---|
| 1 | 301 | filho 252 | 0 (2 "sobrinho" por cônjuge) |
| 2 | 296 | pai/mãe 254 | 0 |
| 3 | 188 | irmão 98 | 0 |
| 4 | 297 | **neto 256** | avô 0 |
| 5 | 288 | **avô 256** | neto 0 |
| 6 | 82 | **sobrinho 61** | tio 2 |
| 7 | 81 | **tio 61** | sobrinho 0 |
| 8 | 42 | primo 36 | 0 |
| 9 | 196 | cônjuge 185 | 0 |

O resto não tem caminho na árvore (pais que ela não guarda). As strings do exe
(0xB6B184–0xB6B224), em ordem de código: "Crush", "Love", "Engaged",
"Married", "Friend", "Best Friend", "Going Steady", "Enemy", "Parent",
"Sibling", "GrandParent", "GrandChild", "Uncle/Aunt", "Nephew/Niece",
"Cousin", "Spouse" ("Child" deve ter sido unificada com outra string igual).
Elas dizem o que *este* Sim é para o outro (1 = sou pai, 4 = sou avô), por
isso a ordem parecia trocada. Bits (2.589 SRELs): bit 3 só com parentesco 9
(26 de 26); bit 7 com STR mediano −50 (97); bit 6 com STR/LTR altos (6); bits
0, 1, 4, 5 com STR/LTR altos; 14 e 15 em quase todos.

**Conclusão.** Refutado em parte e corrigido: 4 = neto, 5 = avô, 6 =
sobrinho, 7 = tio (o inferido tinha cada par trocado); 1, 2, 3, 8, 9
confirmados (`familialName` em `src/formats/neighborhood.cpp`). Bits da
variável 1 confirmados pela ordem das strings e pelos dados: 0 paixão
(crush), 1 amor, 2 noivado, 3 casamento, 4 amigo, 5 melhor amigo, 6
namorando ("going steady"), 7 inimigo; 14 e 15 continuam pelo SimPE (família,
conhecido).

## 9. Test Object Type (0x20): flags do byte 7

**Pergunta.** As flags do byte 7 (pelas strings: 1 = GUID original, 2 = ID
de vizinho, 4 = GUID do token temporário).

**Método.** Histograma das 1.355 instruções e nomes das árvores por flag.

**Evidência.** 0x04 (322): "Wants / Mem. - Set - Marriage & Children", "Wants
/ Mem. - Cleanup", "CT - GUID - Menu", "NPC - Get Outfit/Template GUID for
Type": árvores de tokens (desejos, memórias). 0x02 (11) e 0x03 (34): "Check
Neighbors - Unique NPC?", "NPC - Is Type?", "NPC - Goes into Main?": o ID
testado é de vizinho (parâmetro/stack object). 0x01 (53): "Common Entry -
Post Entry Calls", "Standard Bunny TEST", "Message - Handle Bunny Social",
"Function - Portal - TEST".

**Conclusão.** Confirmado pelos dados: 0x01 compara com o GUID original,
0x02 o ID é um neighbor ID, 0x04 testa o GUID do token temporário.
*Feito* (`testObjectType` em `src/vm/primitives.cpp`; o token é o meu).

## 10. Create New Object Instance (0x2A): flags do byte 5

**Pergunta.** A flag 0x80 e as outras.

**Método.** Por bit: usos, GUID literal zerado e nomes das árvores.

**Evidência.** 0x04 (48): GUID literal 0 em 45; "Lot Transport - Generate
Sims", "Sub - Create Driver", "Interaction - Normalize NPC/Townie": cria um
Sim a partir do vizinho no stack object (como na FreeSO). 0x80 (9): GUID 0 em
8; "GUID - Get in Temp 0 - Object of GUID", "Get in Temp 0 - Object of
Guid", "Function - Create Car", "Memory Marker - Target has this marker":
o GUID vem dos temps 0/1 (string do editor "from GUID in temp0/1"). 0x08 (9):
"Find Fall Location", "Pool - Find Float Location" ("fail if tile is
non-empty"). 0x10 (61) passa o temp 0 ao Main, 0x02 (13) passa IDs, 0x01 (54)
não duplica. 0x20 (28: "Sub - Flight Path", "Be Fed - Find Drop Slot") e
0x40 (7: "Create Game Objects From Inventory", "Dump Shopping Stuff", "Sub -
Create Passengers/Taxi") ficam sem nome certo; as strings que sobram são
"in obj in temp 0's slot", "from GUID in temp Inventory Token", "Moving in a
new Sim" e "copying design mode materials from object in temp5".

**Conclusão.** 0x80 = GUID nos temps 0/1 (confirmado pelos dados); 0x04 =
vizinho no stack object e 0x08 = falha com o tile ocupado (coerentes);
0x20/0x40 em aberto. **Para a VM:** com 0x80 ler o GUID de temp 0 (baixo) e 1
(alto) em vez do literal. *Feito*, junto com as posições do STR# 0xA7 (o slot
das posições 2, 4 e 10 é o byte 9: "Init - Fill Slots with Bottles" cria nos
slots 0–4) e as flags 0x01, 0x02, 0x04, 0x08 e 0x10 da FreeSO
(`createObjectInstance` em `src/vm/primitives.cpp`).

## 11. Find Location For (0x10): flags e byte 3

**Pergunta.** As flags além de 0x08 e o byte 3.

**Método.** Histograma das 640 instruções; strings do disassembler.

**Evidência.** Strings em ordem de código: "start at ", "find location for
object in ", "relative to object in ", as quatro formas de routing slot,
", facing" seguido de " N", " NE", " E", " SE", " S", " SW", " W", " NW",
depois ", user editable", ", prefer empty", ", on level ground", ", with
empty border", ", begin in front of refobj", ", with line of site to center".
Byte 3: 0 em 466, `0xFF` em 114, `0xFE` 26, `0xAA` 21, `0xFB` 6, `0xEF` 4,
`0xBF` 3: uma máscara de 8 direções ("Interaction - Get Out" usa `0xFB`, todas
menos uma). Flags no byte 2: 0x08 (497 usos) é sempre a forma do TS2 com os
dois objetos explícitos e só ela tem o u16 nos bytes 12–13 (31, 35, 38,
0xFFFF, 1.600–3.150; não são IDs de BHAV); 0x40 em 7 usos, 0x10 em 5.

**Conclusão.** Provável: byte 3 = direções permitidas ao virar o objeto
(bit 0 = N … bit 7 = NW, 0 = qualquer). Os bits 0x10/0x40 casam com as
strings "on level ground"/"with empty border"/"begin in front of refobj"/
"line of sight", sem como separar com tão poucos usos. Bytes 12–13 em aberto.
*Feito* na VM: a direção é mantida se a máscara deixa, senão a permitida mais
próxima (`findLocationFor` em `src/vm/routing.cpp`).

## 12. Push Interaction (0x0D): o que os dados dizem

**Pergunta.** Bytes e bits citados pelas strings (ícone, "Returing ID in",
"Linking to interaction with ID in", "Passing first 4 params in", "Force run
check tree", "use name", "continue as current").

**Método.** Histograma das 761 instruções e cruzamento dos bits.

**Evidência.** Byte 0: interação (0–0xDD). Byte 1: 0–10, pequeno (local/temp
como no TS1?). Byte 2: prioridade 0–3 (STR# 0xE0: herdada, máxima, autônoma,
do usuário). Byte 3: bits, 0x02 em 92% dos usos. Byte 4: 0–8, 0xB (índice de
ícone/temp?). Bytes 5–7, 8–10 e 11–13: três pares (dono u8, dado u16): o
objeto (stack object em 490), e dois que casam com "Returing ID in" e
"Linking to interaction with ID in" (dono `0x0A`, `0x19` local, `0x08` temp).
Bytes 14–15: bit 0 em 143/306 usos. O bit 0x80 do byte 3 aparece nas árvores
"As Continuation"/"Object Menu Subqueue Command", o que lembra o "push tail
continuation" do TS1 (FreeSO: byte 3, 1 ícone próprio, 2 objeto num local,
4 continuação na frente, 0x80 continuação atrás).

**Conclusão.** Em aberto: os três pares (dono, dado) a partir do byte 5 são
os mais seguros; os bits do byte 3 e dos bytes 14–15 não ficaram separados.

## 13. GUIDs das classes de UI

**Pergunta.** O que são `0x4ca92f03`, `0x49495e7f`, `0x0c683787`, …

**Método.** Procurar os valores no `Sims2.exe` e nos símbolos do Mac;
atributos das janelas de cada classe nos `.uiScript`.

**Evidência.** Os valores não estão em nenhuma seção legível do exe (ficam no
`.text` cifrado). O binário do Mac só tem nomes (`GZCOM_cTSWinTextLabelCLSID`,
`GZCOM_cTSWinAnimatedBMPCLSID`, `cTSWinBMP`, `cTSWinBtn`, `cTSWinCalloutBox`,
`cTSWinFlatRectTransp`, `cTSWinGenTransl`, `cTSWinGenTransp`,
`cTSWinListBox`, `cTSWinPersonHead`, `cTSWinPieMenu`, `cTSWinSimView`,
`cTSWinTextNotification`, …), não os valores. Pelos atributos: `0x4ca92f03`
(204 janelas) tem fonte, cores de texto e legendas de uma letra (dias da
semana); `0x4d9ccdb1` tem `image`, `edgeimage`, `wparam`, `notify` e legendas
"age change glow", "plumbob", "achieve animation"; `0x49495e7f` tem
`defaultkeys`, `closevisible`, `gobackvisible` (diálogos); `0x0c683787` tem
`colorleft/top/right/bottom` e `style`.

**Conclusão.** Em aberto (sem os valores no exe). Palpites pelos atributos:
`0x4ca92f03` = `cTSWinTextLabel`, `0x4d9ccdb1` = `cTSWinAnimatedBMP`.

## 14. Tipos de lote sem uso

**Evidência.** Os recursos começam com 64 bytes de nome zerados.
`0x6C589723` (1 por lote): versão 6 e dois endereços de rua em strings u8
("165 Sim Lane"/"110 Sim Lane", "30 Lakeside Drive"/"170 Main Street").
`0xEC44BDDC` (3 por lote, instâncias `0x5129`, `0x6C7A`, `0xC284`, e 1 no
bairro): `0x5129` traz uma posição (22,2; 21,3; 15,1), um quaternion e 5,0;
os outros, ângulos e distâncias: provavelmente o estado da câmera do lote.
`0x8B0C79D6`: 5 u32 (2, 0, 0–2, 1, 0). `0xCD8B6498`: `01 00 01 00 00 00`.

**Conclusão.** Em aberto; registrado o que cada um tem.

## 15. Change Material (0x6D) e Manage Inventory (0x33): só pistas

**Change Material, flags do byte 2** (128 de 1.128 usos têm alguma): 0x10
(44 usos, material 0 em 40, 26 em "CT - Decorate") lembra "using material
index from temp 0"; 0x01 (17: "Paint - Update Texture", "Paint - New Canvas",
"Screen Capture") lembra "using snap shot generated material"; 0x20 (16, 8 em
"Function - Wall Adjacency"), 0x04 (25: "Enter Bath", "Sponge Bath"), 0x08 e
0x80 sem palpite. O byte 8 é um dono (`0x0A` em 664 usos, inclusive sem flag),
provavelmente o "found in obj in".

**Manage Inventory, bits 0x08/0x10 do byte 0** (124 e 122 usos, 73 juntos):
quase sempre na operação 3 (procurar), em árvores de memória e desejo ("Sub -
Add Memory Subject as nID", "Memory - Already has Woohoo?", "Memory Marker -
Target has this marker?", "Want Satisfy - Never..."): são critérios de busca
(`cEdithLanguage::CatManageInventoryMatchFlags`), provavelmente casar também
pelas propriedades do token (o sujeito da memória). Em aberto.
