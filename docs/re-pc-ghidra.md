# RE do exe PC (`Sims2EP9RPC.exe`) — infra Ghidra + achados

Noite de 16→17/10/2026. Alvo: **o exe que o jogo roda de verdade** — `Sims2EP9RPC.exe`
(46.208.224 B, sha256 `CC4BB4E1909E27422FC191501274FD222C933D18B6573CFA7F95E170FD9F4722`),
o exe do launcher da comunidade **Sims 2 RPC** (v1.15.1; `RPCFilesystem.txt` o lista e o
changelog cita o `CreateProcess` dele). O `Sims2EP9.exe` "puro" (17 MB) está patchado na
pasta local e não é o que o launcher executa. Os dois exes **não têm símbolos**.

Ver também: `docs/goal-ghidra-pc.md` (goal/log da noite), `docs/buy-build-mac.md` (o lado Mac).

## Infra

- **Projeto Ghidra persistente:** `C:\Users\Artur\sims2-re\pcb` (programa `Sims2EP9RPC.exe`).
  Fica fora do workspace porque o Ghidra **rejeita caminho com elemento começando por ponto**
  (`.gemini`) e o workspace vive sob ele. (O projeto `pc` da primeira tentativa da noite ficou
  inútil: o run pendurou sem salvar — ver log do goal.)
- **Env p/ todo run headless** (senão o default é 8G heap e GC capado a 2 threads → GC thrash):
  ```powershell
  $env:GHIDRA_HEADLESS_MAXMEM = '12G'
  $env:GHIDRA_HEADLESS_JAVA_OPTIONS = '-XX:ParallelGCThreads=6'
  $env:JAVA_HOME = 'C:\Program Files\Microsoft\jdk-21.0.12.101-hotspot'
  ```
  (`GHIDRA_HEADLESS_MAXMEM`, não `MAXMEM` — esse só vale via `launch.bat` direto.)
- **Import+análise one-time** (em `C:\Users\Artur\sims2-re\pcb-run.cmd`; lançado destacado via WMI):
  `analyzeHeadless <proj> rpc -import <exe> -scriptPath <scripts>
  -preScript OffAnalyzers.java "PDB Universal" "Decompiler Parameter ID"
  "Decompiler Switch Analysis" "Call Convention ID"
  -analysisTimeoutPerFile 3600 -postScript DumpInventory.java <out> -postScript MemoryInfo.java`
  - `OffAnalyzers` zera o flag do analyzer em `program.getOptions("Analyzers")` (chave = nome)
    e chama `AutoAnalysisManager.initializeOptions()` p/ sincronizar o scheduler (mecanismo
    achado via bytecode; `registerAnalyzerOptions()` sozinho não basta).
  - Por que esses 4: o **PDB Universal** mastigava o debug embutido por ~28 min só de
    "Unprocessed TypeDescriptor" (sem símbolos aproveitáveis — o exe não tem PDB externo); os
    3 de **decompiler** são os suspeitos do loop que pendurou o run da noite.
  - `-analysisTimeoutPerFile` é rede de segurança; **no timeout o programa é salvo parcial**
    (comprovado no s2pkg) — consultas funcionam no que já foi analisado.
- **Consultas (segundos, readonly)** — `FindString` lê o regex de um **arquivo** (1 padrão por
  linha, `#` comenta; alternância/`^$` livres) porque `|`/`^` no argumento quebram o cmd quando
  se passa pelo `.bat`:
  `analyzeHeadless <proj> rpc -process Sims2EP9RPC.exe -noanalysis -readOnly -scriptPath <scripts>
  -postScript FindString.java <out.tsv> <patternFile> [maxHits]`
  (+ `XrefsAt.java <va>`, `DecompAt.java <va> <out.c>`). Lote pronto: `rea-snapshots/ghidra-pc/run-queries.ps1`
  (4 lotes: catalog/bins/paths/ui → `pcb\out\refs-*.tsv`).
- Scripts em `rea-snapshots/ghidra-pc/scripts/`; dump de strings cru em
  `rea-snapshots/ghidra-pc/strings.py` + `out/strings-ascii.txt` (offsets de **arquivo**).
  Pitfall de compilação dos scripts: `Data` é `ghidra.program.model.listing` (não `model.data`).
- **Conserto de regiões "cegas"** (código alcançado só por ponteiro/vtable, que a análise não
  disassembla): `DisasmFix.java <va> <len> [cap]` (varredura linear via
  `DisassembleCommand(set,set,true)`), `FnAt.java <va>…` (cria função), `RedoFns.java <va>…`
  (remove+recria p/ corpo novo), `NoRetOff.java <va>` (limpa a flag `setNoReturn`),
  `WhoRefs.java <ini> <fim>` (quem aponta pra região — slots de vtable).
- **Padrão MSVC** das funções grandes do jogo: `MOV EAX,<handler em .text>; CALL 0xE5C86C`
  (prólogo SEH compartilhado — `0xE5C86C` = `ExceptionList = local; return`, **retorna**,
  apesar do decompiler dizer "does not return") seguido de `SUB ESP,imm; PUSH regs…`.
  O flow de criação de função para no CALL → criar a função **no `SUB ESP`** se necessário
  (perde só o prólogo). Os "stubs" de 10 B espalhados pelo exe são esses prólogos.
- Regiões consertadas nesta sessão: `0xBDF100-0xBE0A77` (módulo de filesystem/segmentos).
  Funções criadas: `FUN_00be0787` (BuildResourceMaps), `FUN_00be0141`, `FUN_00bdfa42`,
  `FUN_00bdf275` (CreateGameDirectoryFileSegment); `FUN_00bdf753` (switch de categoria do
  ResourceManager de áudio); `FUN_00e5c86c`/`0xCF5A70` decompilados (ver seções).

## Âncoras de strings (offset de arquivo; VA a registrar via Ghidra)

| offset | string | uso |
|---|---|---|
| 0xE4AE10 | `TSCatalog` | catálogo (única) |
| 0xE4B498..0xE4B718 | `catalogTextResource`, `catalogTextIndex/Indices`, `buildTipIndex/Indices`, `notInCatalog`, `subsort`, `thumbnailResource`, `deprecated`, `Failed to set/parse catalog text` | parser de props do catálogo |
| 0xE680A8 | `showincatalog` (+ `crapscore`, `nicenessmultiplier`, `routingcost`, `surfacetype`…) | mesmo parser (props de objeto) |
| 0xE358AC..0xE358CC | `FunctionSubsort`, `FunctionFlags`, `RoomFlags`, `CommunitySort`, `Price` | idem (campo 94 = FunctionSubsort) |
| 0xE62535..0xE625D8 | `Bin Prod ID/Type/Sort Idx/Key/Name`, `Frag Prod ID/Sort Idx`, `Obj/Bin/Frag Key:` | debug de bins/fragmentos do catálogo |
| 0xE62602 | `@CcTSDataTables` | tipo embutido (debug) |
| 0xE51638 / 0xE51644 / 0xE516F0 | `/tsdata/`, `tsdata/`, `/tsdata/res/locale/` | normalização de caminhos do res |
| 0xE5173C | `Starting BuildResourceMaps.` | pipeline do índice de recursos |
| 0xE51530 | `CreateGameDirectoryFileSegment : …` | segmentos de diretório (DBSegments) |
| 0xEDA000..0xEDA4E4 | tabela `Catalog/`, `Text/`, `Bins/`, `Downloads/`… + `Directories` | tabela de diretórios do jogo |
| 0xEAA868..0xEAA950 | `AllUsersData/`, `NeighborhoodTemplate/`, `LotCatalog/`, `Collections/`, `Neighborhoods/`, `UserData/` | pastas do user data |
| 0xEAA934 | `Neighborhoods/` | enumeração de bairros |
| 0xED9F4C | `BuildModeThumbnails.package` | pacote de thumbnails do buy/build |
| 0xDF4080 | `UIPieMenu` (+ `kPieMenu*`) | pie menu (contexto) |
| 0xE50DF8 | `Hitlab.ini` | ini base das configs de segmento |
| 0xE50E04..0xE50E80 | `MusicDirectories`, `ResourceFactories`, `DBSegmentDirectoryFiles`, `TrackDefSegments`, `SampleDataSegments`, `PackedDBSegments`, `DBSegments` | seções ini do índice de recursos |
| 0xE50E8C | `%DataDirectory%` | macro do diretório base |
| 0xE51234 | `.\source\TSAudioResourceManager.cpp` | TU de origem dos logs |
| 0xE53F00 | `TSAudio` | módulo de log |

## (a) Catálogo / bins

**Tabela de descritores de props (metadados de reflexão) — VA `0x124B470`.**
Cada registro = **10 ponteiros (40 B) + nome ASCIIZ inline (padded a 4)**. Layout observado:
`{p0=type, p1=?, p2=?, p3=?, p4=setter, p5=getter-do-nome, p6..p9=handlers genéricos}`.
Os 9 registros do objeto de catálogo, na ordem, com getter (`p5`) e setter (`p4`):

| prop | getter-nome (p5) | setter (p4) |
|---|---|---|
| catalogTextResource | 0xBC554E | 0xBC5F02 |
| catalogTextIndex | 0xBC5554 | 0xBC5F63 |
| catalogTextIndices | 0xBC555A | 0xBC6031 |
| buildTipIndex | 0xBC5560 | 0xBC6117 |
| buildTipIndices | 0xBC5566 | 0xBC6173 |
| notInCatalog | 0xBC556C | 0xBC5572 (stub) |
| subsort | 0xBC55D6 | 0xBC55DC (stub) |
| thumbnailResource | 0xBC5650 | 0xBC5656 (stub) |
| deprecated | 0xBC5713 | 0xBC5719 (stub) |

- Getters são `MOV EAX,<ptr do nome>; RET` (daí `DATA from 0x124B4xx` nas xrefs).
- "Stub" = `MOV EAX,<ptr>; CALL 0xE5C86C` (ver Infra: é o prólogo SEH compartilhado, **retorna**)
  ⇒ `notInCatalog`, `subsort`, `thumbnailResource`, `deprecated` são **read-only** (sem setter real).
- p0/p1/p2/p3/p6/p7/p8/p9 compartilhados: `0xCFAC60, 0x85EF10, 0xCFACA0, 0xBCD20F, 0x68F2B0,
  0x68CDF0, 0x1094750, 0xBCD8B1` (handlers genéricos tipo/compare/serialize); a próxima tabela
  (outro tipo) começa em `0x124B698`.

**Texto do catálogo = recursos STR#.** `FUN_00bc57a0` monta um registro `{tipo='STR#' (0x53545223),
hash 0xFB4CB0DE}` e registra via singleton `FUN_00cd42b0` (vtable `+0x28` por nome → `+0x14`
registra); guarda o índice em `this+0x154`. `FUN_00bc5787` = "get get-count/index" (`+0x18`/`+0x3C`).

**Serializer do Exchange (metadados de objeto de compra).** `FUN_009e7060`, tabela de slots em
`.data` `0x140FE60..0x140FED0` (Name/Description/Price/FunctionFlags/FunctionSubsort/RoomFlags/
CommunitySort/Thumbnails/PackageFile/SKU…), escrita por `FUN_009e1370(campo, valor)`. Números de
campo (batem com o formato do port):

| campo | nº |
|---|---|
| Price | 0x12 (18) |
| RoomFlags | 0x27 (39) |
| FunctionFlags | 0x28 (40) |
| **FunctionSubsort** | **0x5E (94)** |
| CommunitySort | 0x64 (100) |

⇒ **Residual do port (53×47) fechado: campo 94 do engine = `FunctionSubsort`** — o campo 94 do
port está certo.

**Parser read/write dos objetos de build (.package).**
- write: `FUN_00d2b0f0`/`FUN_00d2b760` (tipos `floor/wall/roof/fence/canh/terrainPaint`);
- read: `FUN_00d2d1f0`/`FUN_00d2d750` → `deprecated`→`FUN_00d29180` (+0x30),
  `showincatalog`→`FUN_00d29190` (+0x34), categoria→`FUN_00ab9ed0` (+0x8),
  `nicenessmultiplier`→`FUN_00ce1940` (+0x38), `crapscore`→`FUN_00883ab0` (+0x3C);
  strings em +0xC/+0x18/+0x64/+0x7C/+0x88 (`material/subsort/surfacetype/routingcost/brightness/filename`).

**Bins**: os padrões `Bin Prod ID/Type/Sort Idx/Key/Name`, `Frag Prod ID/Sort Idx`
(offsets 0xE62535+) são usados pela **`FUN_00cdf160`** (entrada `0xCDF160`, SEH, criada 17/10 —
era a "região `0xCDF41B`+" sem função; os `PUSH` dos formatos ficam em 0xCDF2F4/0xCDF41A): é o
**dump de Frag/Bin** — recebe a fonte dos dados (vtbl `+0xC/+0x4C/+0x54/+0x64/+0x6C`) e um sink
de impressão (vtbl `+0x20/+0x4C`, uma linha por `[vtbl+0x4C](&string)`):
- cabeçalho (`Frag Key:`) + buffers de 0xB bytes via `FUN_00ec685a`;
- se a propriedade `0x6C4F3599` existe em A (`[vtbl+0x14]`): pega a **STR# do catálogo**
  (hash `0xFB4CB0DE`, `[vtbl+0x40]`) e resolve o texto (`[vtbl+0x1C]`) → `\nBin Name = %s`;
  senão `\nBin Key = 0x%08X 0x%08X 0x%08X` (a chave de 12 bytes);
- `\nFrag Prod ID = %d\nFrag Sort Idx = %d` e depois `\nBin Prod ID = %d\nBin Type = %s\n
  Bin Sort Idx = %d`, com `FUN_00cd42b0`/`FUN_00cdedd0` e `[vtbl+0x30/+0x50/+0x58]`;
- formatação por `FUN_0040cd66` (sprintf-like) / `FUN_0040bc29` (concat).
⇒ fechou a última parte em aberto do goal (a).

## (b) Varredura de `TSData\Res`

**Tabela de segmentos de diretório** — `FUN_01032f20` (o "DBSegments" do jogo): registra cada
diretório/arquivo com ID+parentID (`Neighborhoods/`, `Collections/`, `Downloads/`
(+`Patterns/`,`Skins/`,`Objects/`), `ContentRegistry`, `LotCatalog/`, `UserData/`, `Res/`, `Sys/`,
`UI/`, `3D/`, `BuildModeThumbnails.package` (+DesignMode/CAS/Object/CANHObjects),
`TS2DownloadsDir`/`TS2Downloads`). É a base da resolução multi-pack do port.

**Cadeia de startup** (entradas descobertas nesta sessão; a região `0xBDF100-0xBE0A77` estava
"cega" — disassemblada e com funções criadas, ver Infra):

`FUN_00be0787` = **BuildResourceMaps** (orquestrador):
1. log `Starting BuildResourceMaps.` (0x125173C; módulo de log `TSAudio` 0x1253F00; TU de origem
   `.\\source\\TSAudioResourceManager.cpp` 0x1251234);
2. registra as classes de segmento (`FUN_00bdd89f` ×3, `FUN_00bdd613`) e ids fixos;
3. itera a lista `[vtbl+0x8C](0xADD550A7)` → para cada entrada chama **`FUN_00bdfa42`** (packed segments);
4. itera a lista `FUN_00cd2990(0xEC43275C, …)` → para cada raiz: `FUN_0040ddfd("TSAudio",7,1)`,
   checa `Hitlab.ini` (`FUN_0040cd43`), monta o path e chama **`FUN_00be0141`** (directory files);
5. fecha logando `Audio resource initialization took %d milliseconds.`

`FUN_00bdf275` = **CreateGameDirectoryFileSegment** (prólogo SEH em `0xBDF26B`):
- logs `CreateGameDirectoryFileSegment : Skipping creation of non-existent/empty directory file
  segment %s` (linhas 0x573/0x57A) e `Failed to create directory file segment object.` (0x58E);
- cria o objeto pela fábrica `[vtbl+0xC](0xA079CE1B, 0x856C8CF7, &out)`, seta flags
  (`+0x70`, `+0x74`, `+0x3C`, `+0x40`) e registra os padrões de arquivo;
- parseia a **lista de extensões separada por `,`** (`FUN_0040e487(',')`, padrões tipo `*.<ext>`)
  e registra cada um (`[vtbl+0xC]`, opcional `[vtbl+0xC4]`);
- inicializa/abre (`+0xC`, `+0x14(1,0)`), loga `Failed to initialize/open directory file segment
  at %s` (0x5C6/0x5CC); callback final opcional ([EBP+0x14]/[EBP+0x18]).

`FUN_00be0141` = **registrador dos segmentos de diretório da instalação** (prólogo em `0xBE0137`):
- constrói `"tsdata/"` (0x1251644) e `"/tsdata/res/locale/"` (0x12516F0) sobre a raiz;
  `FUN_0040d883("tsdata/")` = **`std::string::operator+=(const char*)`** (strlen + `FUN_00405f2a`
  = append(first,last): cresce com `FUN_0040e923`, preserva o conteúdo antigo e copia o 1º char
  por último p/ self-append) — o montador de path que anexa cada fragmento;
- lê as seções ini **`DBSegments`**, **`DBSegmentDirectoryFiles`**, `SampleDataSegments`,
  `TrackDefSegments` (×2), `MusicDirectories` (ponteiros em `0x142C7DC..0x142C7F8`; handler de
  linha das duas primeiras = `0xBDF83B`, logo após `FUN_00bdf753`);
- expande o token **`%Language%\\`** com o idioma corrente (`FUN_0040E0D6`/`_strlen`/
  `FUN_0040D2A1`/`FUN_0040E0AB`);
- por entrada chama `FUN_00bdf26b(...)` = CreateGameDirectoryFileSegment (tipo `0x656B8F0C`);
  logs `Segment at %s has no extensions defined in section %s` (0x3B0) / `Failed to create
  segment at %s` (0x3D9).

`FUN_00bdfa42` = **registrador dos packed segments** (prólogo em `0xBDFA38`; chamado pelo passo 3):
- o .ini pode vir **como recurso** (log `Failed to find segment containing .ini resource
  %08x:%08x:%08x%08x`, linha 0x325);
- mesmos paths `"tsdata/"`/`"/tsdata/"`; seções **`PackedDBSegments`**, `SampleDataSegments`,
  `TrackDefSegments` (×2), `MusicDirectories`;
- cria os segmentos via `[vtbl+0x58](0x656B8F0C, &out, 2 /*packed*/, nome,
  FUN_00c02519(1,1))`; log `Failed to create segment for %s` (0x377).

**Chaves/macros**: `%DataDirectory%` (0x1250E8C), `%Language%\`, ini `Hitlab.ini` (0x1250DF8);
`,` = separador de extensões, `;` = comentário ini, `:` = separador. Handlers de linha
(decompilados 17/10; `0xBDF83B`/`FUN_00bde67d` têm prólogo MSVC SEH `MOV EAX,<__ehhandler>;
CALL __SEH_prolog`):
- `FUN_00e5c86c` = **`__SEH_prolog`** (EAX = handler; monta EXCEPTION_REGISTRATION, `FS:[0]=ESP`,
  EBP = framebase; volta com `PUSH EAX/RET`);
- `0xBDF83B` (DBSegments/DBSegmentDirectoryFiles; corpo = `FUN_00bdf845`, 302 B): linha
  `begin_group`/`end_group` → ignora; senão hash do id (`[vtbl+0x14]` → `FUN_00410720`) e
  insere no mapa do 3º param (`FUN_00415585`); id duplicado → log nível 2
  `Ini file section has duplicate id %08x = %s\n` (`TSAudioResourceManager.cpp:0x477`).
  O registro do callback fica em `FUN_00bdf973` (`[vtbl+0x34](…, 0xBDF83B, 0x142C7F4)` + caso
  especial do hash `0x5B945038`), usado por `FUN_00be0141`/`FUN_00bdfa42`;
- `FUN_00bdd8f2` (SampleData/TrackDef): se 3º arg ≠ 0 → hash (`FUN_00410720`) do id em `this+4`
  → `FUN_00bdd887(&hash)`;
- `FUN_00bde67d` (MusicDirectories; corpo = `FUN_00bde687`, 102 B): se 3º arg ≠ 0 → nome do
  1º arg (`[vtbl+0x14]` → `FUN_00425bbc`) + id do 2º (`thunk_FUN_0047b878`) →
  `arg3->[vtbl+0xC](nome, id)`.

**Para o port** (`src\formats\resindex.cpp`, `src\app\user_data.cpp`): ordem *packed → directory
files*; tudo relativo a `tsdata/`, com o ramo de idioma em `/tsdata/res/locale/`; a tabela de
`FUN_01032f20` lista os segmentos de user-data que o port já modela.

## (c) `Neighborhoods` / EPs

- **`Neighborhoods/`** vem da tabela `FUN_01032f20` (junto de `AllUsersData/`,
  `NeighborhoodTemplate/`, `LotCatalog/`, `Collections/`, `UserData/` — âncoras 0xEAA868+).
  A enumeração no jogo passa pelo mesmo sistema de segmentos acima (resolver o segmento →
  varrer `.package`s), não por varredura ad-hoc de diretório.
- **EPs**: o módulo de script **`nGameState`** é registrado por `FUN_0090d3c0` via
  `FUN_00cd3b60(this, "nGameState", &DAT_0148a358, 8)`; tabela `0x148A358`:

  | native | endereço |
  |---|---|
  | GetLotTripInfo | 0x8FC510 |
  | GetTransitionsEnabled | 0x8FC590 |
  | GetSaveLotEnabled | 0x8FC5C0 |
  | GetCurrentState | 0x8FC5F0 |
  | **GetInstalledEps** | **0x8FC630** |
  | **IsEPInstalled** | **0x8FC670** |

  (+ `GetProperty`/`SetProperty` = 8 no total.)
- Implementação: natives da VM (args via helpers `0xCF5A70`/`0xCF5B40`); `IsEPInstalled` faz
  `1 << n` e testa via vtable do VM (`+0xDC`); o backend da máscara é **`FUN_00cf5a70`**, que
  cacheia em **`DAT_01499168`**: para i em 0..31, `FUN_00cd2930(0x4BCFCE5E, …, 1, 0, 1<<i)` →
  OR no bit; no fim força o bit `0x1000`.
- **Conclusão p/ o port**: "EP instalada" = bit do bitmask (com 0x1000 forçado); listar bairros =
  iterar o segmento `Neighborhoods/` do sistema de segmentos.

## Cross-check REA (fechado em 17/10, ~22:38)

Critério do goal da noite: o REA (ferramenta independente) confirma ≥1 achado do projeto `pcb`.
Fechado com **3/3 âncoras exatas** via `search_strings` (regex
`Bin Prod ID|Bin Sort Idx|TSCatalog|FunctionSubsort`):

| âncora (projeto `pcb`, VA) | REA (rodada final) | match |
|---|---|---|
| `FunctionSubsort` @ `0x012358AC` | `0x12358ac` | sim |
| `TSCatalog` @ `0x0124AE10` | `0x124ae10` | sim |
| `" Bin Prod ID = %d Bin Type = %s Bin Sort Idx = %d"` @ `0x01262534` | `0x1262534` (`\nBin Prod ID = %d\nBin Type = %s\nBin Sort Idx = %d`) | sim |

- **Evidence**: `rea-snapshots/ghidra-pc/rea-crosscheck.success.json` — `evidence_id`
  `ev_6b0910b0a3bef8159244960ac31bf15bb885e21e3fadd7e7a354dfa128b899a5`, provider Ghidra 12.1.4,
  target sha256 `cc4bb4e1…`, `operation: search_strings`, `confidence: observed`.
- **Receita reproduzível** (~2 min de análise; rodar solo — nada de Ghidra em paralelo):
  - `GHIDRA_INSTALL_DIR` + `JAVA_HOME` como em `run-queries.ps1`;
  - `GHIDRA_HEADLESS_MAXMEM=12G` (o default do REA é 2G);
  - `REA_GHIDRA_STARTUP_TIMEOUT_MS=3300000` (o deadline de startup cobre a análise inteira);
  - `REA_GHIDRA_DISABLED_ANALYZERS` = os 19 nomes de `analyzer-list.txt` (todos os pesos-pesados
    do resumo de análise do `pcb`; `ASCII Strings` fica ligado — é o que a busca usa);
  - `npx -y rea-agents@6.5.0 search '<exe>' --kind strings --pattern '...' --mode regex --format json`.
- **Histórico das falhas** (todas por tempo, nenhuma por resultado): a rodada de 1 h caiu no
  default de 3600 s do headless (`analysis_timeout`); as rodadas de 15 e 55 min caíram no deadline
  de startup do REA **ainda no meio da análise** com os analyzers pesados ligados (X86 Function
  Callee Purge 255 s, Data Reference 213 s, Reference 211 s, x86 Constant Reference 90 s,
  Stack 71 s…; o run manual do `pcb` fecha em 17,4 min). Com o pacote pesado desligado a análise
  cai para ~2 min mesmo com as 2 GC threads hardcoded do REA (`GhidraJavaLaunch.js`; há **dois**
  caches do npx — patch com backup `.orig` nos dois, ver log do goal).
- **Limitações do envelope** (relevantes p/ consultas futuras): o inventário cobre só strings
  *definidas* (`defined-data-coverage-v1`); projeto efêmero (`-readOnly`, `-deleteProject` — nada
  persiste); o deadline do REA cobre o ciclo inteiro (import → análise → operação).
