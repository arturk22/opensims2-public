# Relacionamentos e memórias: o que o binário de Mac faz

Convenções como em `docs/buy-build-mac.md` (endereços do BV 1.10 ppc). Lido
na rodada n08 (Ghidra MCP para buscas e memória; as funções desta área quase
não têm nome nem função criada no projeto, então foram descompiladas num
clone do projeto pelo Ghidra headless, só leitura). Os nomes do symxfer
nesta faixa (`0xac64e0`…`0xac8600`) estão **errados** (`ValidateObject`,
`cTSShoppingCartBase::*`, `AddTerrainCutoutObject`): as classes foram
achadas pela RTTI (`20cTSRelationshipTable` em `0x2765850`, typeinfo
`0x2ab0898`).

## 1. A tabela de relacionamentos (`cTSRelationshipTable`)

Vtable principal BV `0x2811d48` (a segunda interface, a de recurso
persistente, em `0x2811da8`). Cada objeto tem a sua (getter da vtable do
objeto `+0x1AC`), inclusive os Sims (tabela do Sim como objeto); cada
vizinho tem outra (a que o SDSC guarda). É um `hash_map<u32, entrada>`
(`GUIDHash`) em `+0x1C`; a chave é **um u32 cru**: a tabela não sabe se é um
ID de objeto ou um neighbor ID.

| vtable | BV | o que faz |
|---|---|---|
| +0x0C | `0xac6570` | Save(stream, id): guarda o id em `+0x14` e chama Write |
| +0x10 | `0xac65a0` | Load(stream, id): guarda o id e chama Read |
| +0x14 | `0xac820c` | GetEntrySize(para) (0 sem entrada) |
| +0x18 | `0xac8328` | SetEntrySize(de, para, n): cria a entrada se falta (`CreateNewEntry` `0xac7b6c`) e redimensiona (novos = 0); marca modificado |
| +0x1C | `0xac8494` | RemoveEntry(para): **apaga o registro SREL do banco** (`SimSystem +0x40`) e tira a entrada |
| +0x20 | `0xac7614` | RemoveAllEntries(apagar registros) |
| +0x24 | `0xac80e8` | GetValue(para, i) (0 fora do tamanho) |
| +0x28 | `0xac7fb4` | SetValue(para, i, v): só dentro do tamanho (falso fora) |
| +0x2C | `0xac66d4` | contagem; +0x30/+0x34 primeira/próxima chave (`0xac6800`/`0xac6858`; −1 no fim) |
| +0x4C | `0xac6f04` | Write(stream) |
| +0x50 | `0xac7738` | Read(stream) |
| +0x54 | `0xac65d0` | tipo do registro: `0xCC364C2A` (SREL) |

**Write** (`0xac6f04`), dentro do registro do objeto (XOBJ/PERS): `i32 3`
(versão), `u32 n`, `n × u32 chave` (na ordem do hash_map), e para cada
entrada **carregada** grava o registro SREL dela no pacote (chave = tipo
SREL, grupo do registro do objeto, instância `id << 16 | chave`). Entradas
que nunca foram lidas não são regravadas: o registro antigo fica.

**Read** (`0xac7738`): `i32 versão`; versão < 0 (formato antigo): `i32 n` e,
por entrada, `i32` presente; se presente `u32 chave` e a entrada inline
(`+0x50` da entrada: `i32 n` + n valores). Versão 2 ou 3: `u32 n` e `n`
chaves, cada uma inserida **sem** valores: a entrada é lida do SREL só quando
alguém a pede (`GetRelationshipEntry` `0xac7d74`: cria a entrada e abre o
registro; sem registro, a entrada fica com 0 valores). Outra versão: a
tabela fica vazia. Versão 3 usa o grupo do registro do objeto; versão 2 o
tipo dele e o grupo `0xAC3A7122` (só formato velho).

Consequência: **um SREL do lote só vale se a tabela do objeto "de" lista a
chave**. Registros órfãos (nenhuma tabela os lista) são ignorados.

## 2. O registro SREL (`cTSRelationshipEntry`)

Vtable BV `0x2811c90`, objeto de 0x2C bytes (ctor `0xac6b00`): chave do
recurso em `+0x10…+0x1C`, `vector<u32>` de valores em `+0x20`.

- **ResourceWrite** (`0xac68ec`): `u32 2`, `u32 n`, `n × u32`. Nada depois.
- **ResourceRead** (`0xac6be0`): `u32 versão`; só a 2 é aceita (outra: falha
  e a entrada fica vazia); `u32 n` e `n` valores. **Bytes depois dos `n`
  valores são ignorados** (124 dos 18.594 SREL dos quatro bairros os têm; o
  jogo os perde na próxima gravação da entrada).
- GetEntry/SetEntry (`0xac6758`/`0xac66dc`) só dentro do tamanho;
  SetEntrySize (`0xac6e50`) redimensiona com zeros.

Dados (N001–N003 + Tutorial, lotes e bairros): todos os 18.594 SREL são
versão 2, grupo `0xFFFFFFFF`.

## 3. Quem usa a tabela

- **`TryNewRelationship`** (`0xe2dd10`): com `0x02` a tabela do vizinho
  "de"; sem `0x02` e com `0x08`, a tabela do objeto "de" com o "para" cru;
  sem os dois, os dois são objetos: dois Sims (tipo 2) → tabela de vizinho
  pelos neighbor IDs; senão a tabela do objeto "de" com o **ID de objeto** do
  "para". Ou seja, a chave de `0x08` e a de objeto caem **na mesma tabela e
  no mesmo espaço de chaves**. Tamanho ≤ variável: com `0x01` falso, senão
  `SetEntrySize(de, para, variável + 1)`.
- **Dono do objeto** (`cTSPerson::UpdateObjectOwnership` `0xad2ebc`,
  `ObjectIsMine` `0xad45a4`, `GetOwnerShipBias` `0xac9ca8`): tabela do objeto
  (do líder do multi-tile), chave = **neighbor ID** do Sim (vtable 0x64C),
  variável 1. `ObjectIsMine` compara com o máximo da variável 1 das **outras
  chaves da mesma tabela**, quaisquer que sejam.
- **Morte de objeto** (`cEdithObjectModule::KillOutOfWorldObject`,
  `0x532d08`): depois de `RemoveObject`, cada objeto que sobrou faz
  `RemoveEntry(id do morto)` na sua tabela (apaga o SREL). Como a chave é
  crua, isso também tira uma entrada de dono cujo neighbor ID é igual ao ID
  do objeto morto. As tabelas de vizinhos não são tocadas.
- `cTSPerson::OnPostLoad` (`0xad3730`, versão de arquivo < 0x32): tira da
  tabela do Sim as chaves que são Sims (formato velho).

## 4. Memórias

Uma memória é um **token singular do inventário do vizinho** com o flag 2
(memória) e mais de 11 propriedades (`TokenIsMemory` `0x537c7c`). GUID do
token = o objeto de memória (OBJD tipo 15). Propriedades que o motor lê:

| prop | uso |
|---|---|
| 0 | tipo (vai para o `cMemoryInfo`; a instância mapeia 0→0, 1, 2 "bom", 4/5/6 "ruim", o resto 0) |
| 5, 6 | GUID do assunto (6 alto, 5 baixo) |
| 7 | valor inicial (peso no total da pontuação) |
| 8 | valor final |
| 9 | passo diário (o módulo) |
| 10 | valor atual |
| 11 | máscara de aspiração (a do dado 46 do Sim dobra o peso) |

`cTSPerson::UpdateMemories(decair, avisar)` (`0xad0a34`):

- `decair` (só de `DayPassed`, `0xad133c`): se o inventário tem mais de 1000
  tokens, `PruneMemories`; depois, para cada memória com prop 9 ≠ 0, prop 10
  anda |prop 9| de 7 para 8 (sinal trocado se 8 < 7), presa entre min(7, 8)
  e max(7, 8).
- sem `decair` (de `OnSimulatorTick` a cada 60 ticks, se o dado 29 "Motives
  Static?" é 0, e de `GetMemoryInfo`): só refaz se o inventário mudou ou
  nunca foi feito.
- Junta as memórias **visíveis** (flag 1) numa lista `cMemoryInfo` (0x1C
  bytes: GUID, assunto, prop 0, prop 10, prop 7, prop 8, prop 11) em
  `+0x8F4` e recalcula o **dado de pessoa 54 "Memory Score"** (`0xad05c8`).
- `avisar`: mensagem `0x0EA990D2` com o ID do Sim (o painel se atualiza).

`PruneMemories` (`0xad0380`): do começo da lista, enquanto restam ≥ 100
tokens a olhar, marca para apagar (flag 4) as memórias **invisíveis** já no
valor final (prop 10 = prop 8); depois o inventário apaga os marcados.

**Memory Score** (`0xad05c8`), com `asp` = dado 46 e peso 1,2 para memória
cuja prop 11 & `asp` ≠ 0 (1 nas outras):

1. As 3 de maior valor atual: as positivas somam `peso` em `P` e
   `atual × peso` em `S`; as 3 de menor: as negativas somam `peso` em `N` e
   `atual × peso` em `S`.
2. `média = S / max(P, N)`.
3. `T` = soma de todas de `inicial` (× 2 se a aspiração casa).
4. `T ≤ 0`: `pontos = média − T × (média + 100) / (T + 500)`; senão
   `pontos = média + T × (100 − média) / (T + 500)` (floats), truncado para
   inteiro. Sem memórias visíveis: 0.

**Humor** (`cTSMotives`, vtable `0x27c0a60`, funções sem nome no BV):
`UpdateMood` (`0x486b78`) = média ponderada das curvas (motivos 2, 10, 11,
12 e os de `dado 118` excluídos) **+ dado de pessoa 163**, entre −100 e 100;
`UpdateMetaMood` (`0x485e68`): **Humor+Memória (motivo 4) = Humor**, entre
−100 e 100. A pontuação de memória não entra no motivo 4.

**Para o painel** (`cTSMemorySimulator::GetMemories`, `0x5372b0`): a lista
`cMemoryInfo` do Sim (vtable 0x238) é percorrida **de trás para a frente**
(a mais nova primeiro); cada uma vira um `cTSMemoryInstance`
(`Initialize` `0xb18538`, `SetDefinition` `0xb18ec8`) com a definição do
GUID (`cTSMemoryDefinition::Load`, `0xb19bb0`, guardada num mapa):

- a definição só existe se o OBJD tem tipo 15; campo 0x20 → `+0xC`; campo
  0x21 = 0 / = 1 → dois bools; nome e descrição do catálogo; ícone
  `0x856DDBAC`, instância 1, grupo do OBJD; textos: STR# do objeto, índice
  = campo 0x29 (versão masculina) e 0x29 + 1 (feminina), título e
  descrição de cada; um vazio copia o outro.
- a instância usa a versão feminina quando o **assunto** é uma pessoa com
  dado 65 (gênero) = 1, e troca as variáveis do texto (`0xb18ab0`); marca
  "aspiração" quando prop 11 ≠ 0 e é igual ao dado 46 do Sim.
