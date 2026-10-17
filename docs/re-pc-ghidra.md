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

## (a) Catálogo / bins — PENDENTE

## (b) Varredura de `TSData\Res` — PENDENTE

## (c) Enumeração de `Neighborhoods` — PENDENTE
