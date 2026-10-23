# Os scripts Lua do jogo (`ObjectScripts`): o formato, a API e o que eles governam (23/10)

O `0x7E` (Run Script) do jogo roda **scripts Lua** presos aos objetos (o RE do PC está no
`re-pc-ghidra.md` §(g)/(h), e o custo da recusa no `data-findings.md` §21). Este doc é o
inventário do que existe nos dados e o plano para tirar o port do `false`.

## Onde estão

- `Double Deluxe\EP2\TSData\Res\ObjectScripts\`: **37 arquivos soltos** — a Nightlife (EP2)
  é onde o sistema nasceu. São **25** `<nome>.objLua` (os scripts) e **12**
  `<nome>.globalObjLua` (os prelúdios compartilhados: `Global`, `Neighbor`,
  `NPC_Constants`, `ObjectData`, `ObjectDefinition`, `OutfitData`, `ConnectedColumns`,
  `PersonData`, `DrivewayGlobals`, `GarageGlobals`, …).
- Os EPs seguintes trazem `TSData\Res\ObjectScripts\ObjectScripts.package` (o AL:
  **561 recursos**), com os tipos `0x9012468A` e `0x9012468B` e o TGI **hasheado pelo
  nome** (`nameInstanceHash`/`nameInstanceHiHash` do `resindex`): as versões que
  substituem as da Nightlife.
- **O nome do script**: o `0x7E` pega o id do script **1-based** no STR# 0x130 ("Lua
  Scripts", 7.233 entradas) e a entrada (id − 1) é o nome do arquivo. O "NPC - Is Type?"
  do pool de NPCs usa o id 11 → a entrada `#10` = `NPC_IsType` → `NPC_IsType.objLua`
  (conferido nos dados).

## O formato: bytecode, não texto

Os arquivos começam com `1B 4C 75 61 50` — o chunk **pré-compilado do Lua 5.0**
(`ESC "Lua"`, versão 0x50 = 5.0; formato 1; `int` 4; `size_t` 4; `Instruction` 4;
`Number` 8 = double). Não dá para "ler o script" como texto: ou se porta um
**interpretador de Lua 5.0** (a 5.4 não lê bytecode 5.0) ou se traduz cada script à mão.
As constantes dos chunks dão o que cada um faz e quais funções de motor chama.

## A API que os scripts chamam

Das constantes dos 37 arquivos (o resto são nomes de campos de dados):

- chamadas: `GetTreeParameter`, `SetScriptReturnValue`, `GetPersonData`,
  `GetObjectDefinitionField`, `NPCTypeToGUID`, `ConstantAccess`, `getGUID`,
  `getPersonIdFromGUID`, `MoveObjectToLocation`, `GetObjectLocation`,
  `BitShiftRight`/`BitwiseAnd`/`BitwiseOr`, `CalculateTurnOnOffCount`/
  `CalculateTurnOnOffScore`, `countProperties`, `CheckForwards`/`CheckBackwards`,
  `CanConnectDir`/`ConnectByDir`, `DaysTillBreak`;
- tabelas de nomes: `PersonData` ("Person Age", "NPC Type", "family number", …),
  `ObjectDefinition`, `ObjectData`, `OutfitData`, `Neighbor`, `ConstantAccess`;
- o prelúdio (`.globalObjLua`): as classes `GlobalObjManager`, `ObjectManager`,
  `CachedNeighbor`, `CachedPerson`, `new`, `__index` — tudo em Lua, por cima da API.

São ~20 funções de motor mais as tabelas de nomes que o port **já tem** nos STR# (os dados
de pessoa 0xC8, os dados de objeto 0x8D, os campos do OBJD, as constantes).

## O que fica inerte hoje (o custo do `false`)

Com o `0x7E` devolvendo false, **nenhum** desses sistemas roda:

- o **pool de NPCs do AL** ("NPC_IsType") → os carros de serviço sem motorista, a
  empregada que não chega (`soak-report.md` round 20);
- a **pontuação de atração** da Nightlife (`AttractionScore`, `TurnOnOffCount`,
  `PersonalTraits`);
- o **pôquer** (`PokerCompareHands`, `PokerScoreHand`);
- as **entradas de garagem** (`DrivewayAdjacency`, `GaragePlacement`, `CarBlockingStreet`,
  `DrivewayGlobals`);
- a **ordenação de mesas** (`TableSort`, `DiningSort`);
- o **init dos multi-tile** (`MTInit`), o `MoveObjectToLocation`, o
  `TransitionsEnabled`/`SaveEnabled`, o `GetValidDowntown`, o `InteractionQueue`…

## O `s2pkg lua` (23/10)

`src/formats/lua50.cpp` + `cli/s2pkg/script_cmds.cpp`: o leitor do chunk (o formato do
`lundump.c`/`lopcodes.h` do 5.0.3, com os tamanhos que o header declara: int 4, size_t 4,
Instruction 4, Number 8) e o desmonte de cada instrução com a linha e os constantes
(RK ≥ 250 = `Kst`). `s2pkg lua <file.objLua>` no `NPC_IsType.objLua` (85 instruções,
debug já removido: 0 linhas/locais/upvalues) mostra a lógica do pool:

- `npc = GetTreeParameter(0)`, `tipo = GetTreeParameter(1)`;
- `CachedNeighbor.new(npc)` e `GlobalObjManager:getPersonIdFromGUID(pessoa:getGUID())`;
- com pessoa: `GetPersonData(id, "Person Age") <= 0` → false; `GetPersonData(id, "NPC
  Type") ~= tipo` → false; a família 32767 (sem família) ou ≠ 0 → false; senão
  `SetScriptReturnValue(true)`;
- sem pessoa (id 0): o mesmo pelo `pessoa:getPersonData` e
  `GetObjectDefinitionField(guid, 203) == NPCTypeToGUID[tipo]`.

É a referência para o primeiro alvo do VM (o pool de NPCs → o motorista do carro da
empregada).

## O recurso empacotado (23/10)

As versões dos EPs não são o chunk puro: `0x9012468B` (os `.objLua`) e `0x9012468A` (os
`.globalObjLua`) trazem `u32 0 + u32 tamanho + o nome` e **o chunk em seguida** (ex.:
`NPC_IsType` + `LuaP...`). O TGI é o hash do nome, mas **não** é o `nameInstanceHash` do
port (`NPC_IsType` = `FF9573E2` no pacote e `FFB38405` na função do port) — o lookup do
`0x7E` tem de montar um **mapa nome → TGI** varrendo esses recursos uma vez (como o
`byName_` dos BHAVs) e ler o chunk depois do nome. O `s2pkg dump` de um recurso
`0x9012468A`/`0x9012468B` já imprime `script "<nome>"` e o desmonte.

## O plano

1. ~~Vendorizar o Lua 5.0.3~~ **FEITO (23/10)**: `third_party/lua50` (`src/`, `include/` e o
   `COPYRIGHT`; a lib `os2_lua50` no CMake) e o `test_lua50`: um chunk de **fonte**, o
   **dump**, o dump **carregado e rodado** (o caminho dos scripts do jogo) e a biblioteca
   padrão que eles usam (`ipairs`, `table.sort`, `math`, `table.getn` — o `#` só veio no
   5.1). **40/40** no ctest. Falta o `0x7E`: resolver o nome (STR# 0x130 → o recurso
   `0x9012468B` no pacote do EP ou o arquivo solto da Nightlife), carregar os prelúdios
   (`.globalObjLua`) + o script e registrar a API.
2. Registrar a **API** (as ~20 funções) sobre o `Context`/`VM`, com os nomes dos campos
   resolvidos pelos STR# (`PersonData["NPC Type"]` → o número do campo no STR# 0xC8, que o
   port lê hoje com `persondata::` só para os que o motor usa).
3. Carregar os **prelúdios** (`Global.globalObjLua` e os do EP) antes do script, como o
   jogo; os scripts dos EPs vêm do `ObjectScripts.package` (hash do nome) e os da Nightlife
   dos arquivos soltos.
4. Primeiro alvo medível: o **`NPC_IsType`** (o pool de NPCs → o motorista do carro da
   empregada; o antes/depois está no `soak-report.md` round 20), depois os sistemas da
   Nightlife (atração, pôquer, garagem).
