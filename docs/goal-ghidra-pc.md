# Goal — Noite de RE do lado PC (`Sims2EP9RPC.exe`)

- **Status:** **fechado o essencial** — 17/10 (dia): projeto `pcb` analisado e salvo; goals
  (a)/(b)/(c) documentados em `docs/re-pc-ghidra.md`; falta só o cross-check REA (1 rodada solo)
  e a limpeza de resíduos.
- **Mandato:** Artur — "Arrocha, use o REA + Ghidra, pode usar mais capacidade, vou dormir, rode por no mínimo 8h, faça um goal." (noite de 16→17/10/2026)
- **Branch:** `windows-x64-ghidra-pc`
- **Cutoff:** ~08:00 de 17/10. Parar limpo: nenhum processo pendurado, nada pela metade sem nota.

## Objetivo

1. **Infra de RE do exe PC permanente**: projeto Ghidra persistente (análise 1×; consultas em
   segundos) + scripts de consulta — o que o REA não dava (ele reimporta a cada chamada com
   teto de 1 h por operação).
2. Usar essa infra, com REA como cross-check, para as perguntas PC-side abertas:
   - **Catálogo/knob de Compra** (doc `buy-build-mac.md` §1.4 "por conferir"; residual 53×47):
     localizar no exe o pipeline da contagem (candidatos + pseudocódigo).
   - **Varredura de `TSData\Res`**: como o exe acha as árvores (valida a resolução multi-pack do port).
   - **Enumeração de `Neighborhoods`**: como as EPs são lidas (pré-requisito do próximo item do port).
3. Documentar tudo em `docs/` com endereços, pseudocódigo e como reproduzir.

## Não-objetivos

- Mexer no código do port nesta noite (foco é RE).
- Symxfer Mac→PC (inviável: PPC vs MSVC x86, sem RTTI no PC).
- Análise dinâmica (exige a máquina em uso; fica para outra sessão).

## Fatos que motivam (verificados 16/10)

- Os dois exes PC **não têm símbolos** (zero nomes mangled/RTTI de verdade; sem PDB na UC):
  no PC não existe "buscar `cTSCatalog::LoadObjects` por nome".
- REA: operações pesadas reimportam a cada chamada (30–60+ min) e estouraram o teto de 1 h;
  busca de strings (~30 min) passa.
- Ghidra 12.1.4 + JDK 21 + Python 3.12 já instalados; máquina nunca suspende em AC.

## Critérios de sucesso

- [x] Projeto `C:\Users\Artur\sims2-re\pcb` (`rpc`) criado, salvo, reabrível (`-process ... -noanalysis`).
- [x] Scripts de consulta funcionando (testados antes no exe pequeno `s2pkg.exe`).
- [x] `functions.tsv` + `strings.tsv` + `symbols.tsv` do exe gerados.
- [x] `TSData\Res`: varredura localizada e documentada (ordem/prioridade).
- [x] `Neighborhoods`: idem.
- [x] Catálogo/knob: candidatos localizados + pseudocódigo (residual 53×47 fechado).
- [ ] Cross-check REA de ≥1 achado (evidence JSON em `rea-snapshots`).
- [x] Docs + commits na branch; log abaixo atualizado.
- [x] Máquina limpa ao final (sem processos pendurados).

## Riscos

- Análise do exe pode ser longa (o "Decompiler Parameter ID" é o suspeito nº 1 — tentativa de
  desabilitar via opções; se não der, aceitar e esperar).
- REA pode estourar o teto de 1 h de novo → perde-se só o cross-check, não o trabalho.
- RAM de 24 GB: rodar REA e Ghidra **em série**, não em paralelo.

## Log

- (16/10 ~23:55) Branch criada; sono/hibernação conferidos (nunca em AC); goal escrito;
  reconhecimento do exe feito (sem símbolos); javap confirmou a API de análise.
- (17/10 ~00:20) Dúvida do Artur sobre o alvo ("não sei se é o exe original do game"):
  investigado e resolvido — `Sims2EP9RPC.exe` é o exe do launcher **Sims 2 RPC** (Voicemxil,
  v1.15.1 na pasta; `RPCFilesystem.txt` o lista como arquivo do launcher e o changelog cita
  "CreateProcess ... on Sims2EP9RPC.exe"). O `Sims2EP9.exe` local foi patchado (hash difere
  dos dois backups idênticos entre si). Artur confirmou: "pode usar o rpc mesmo". Alvo da
  noite = `targets/Sims2EP9RPC.exe` (sha256 cc4bb4e1...).
- (17/10 ~00:45) Smoke dos scripts ok (DumpInventory/MemoryInfo/ListAnalysisOptions compilam
  e rodam). Descobertas: (a) a análise default mastiga PDB/debug embutido por muito tempo
  (s2pkg com `.pdb`: >15 min; no RPC é o flood de "TypeDescriptor" que derrubou o REA);
  (b) opções de análise via `program.getOptions("Analysis")` vieram vazias no headless →
  decisão: rodar a análise completa do RPC com teto de segurança
  `-analysisTimeoutPerFile 10800` em vez de brigar pra desabilitar analisador; MAXMEM default
  = 8G; (c) **Ghidra proíbe diretório com ponto no caminho** — workspace fica sob `.gemini`,
  então os projetos vivem em `C:\Users\Artur\sims2-re\`; (d) REA #1 (strings
  `catalog|subsort|neighborhoods|tsdata`) rodando, teto ~00:55.
- (17/10 ~01:30) Anchors do exe mapeados via dump Python próprio (strings.py): `TSCatalog` 0xE4AE10,
  `subsort` 0xE4B5F4 (cluster do parser de props do catálogo em 0xE4B498: catalogTextResource/Index,
  notInCatalog, buildTipIndex...); `FunctionSubsort`/`FunctionFlags`/`RoomFlags` 0xE358AC+;
  prints de debug dos bins `Bin Prod ID/Type/Sort Idx/Key/Name` + `Frag Prod ID/Sort Idx` 0xE62535+;
  paths `/tsdata/` 0xE51638, `/tsdata/res/locale/` 0xE516F0, `Neighborhoods/` 0xEAA934,
  `Catalog/` 0xEDA304, `UserData/` 0xEAA944, `Starting BuildResourceMaps.` 0xE5173C,
  `CreateGameDirectoryFileSegment` 0xE51530, `BuildModeThumbnails.package` 0xED9F4C.
  Run grande (import+análise, PID 23352) iniciado ~01:02 com `-analysisTimeoutPerFile 10800`
  (cap 3h) e postScripts DumpInventory+MemoryInfo; consultas preparadas em
  `rea-snapshots/ghidra-pc/run-queries.ps1`. REA #1 seguia rodando (2.2 GB WS, 40+ min CPU).
- (17/10 ~02:05) REA #1 terminou em **falha** (`provider_timeout`, "Ghidra startup deadline
  elapsed", 1 h) — terceira falha no exe RPC; hipótese: contenção (rodou junto com os smokes
  Ghidra e com o run grande). Comparação: o `search_strings` "PieMenu" que passou levou ~30 min.
  Plano: ao fim do run grande, tentar **um REA magro sozinho** como cross-check
  (`Bin Prod ID|TSCatalog|subsort`) e comparar endereços; se falhar, documentar como
  infactível neste exe.
- (17/10 ~08:35→09:00) **Correção do registro acima**: o "02:05" foi estimativa errada (sem
  relógio) — os arquivos `pc\console.txt`/`analyze.log` **pararam às 01:22:22** e o processo
  ficou pendurado ~7 h até ser morto na manhã, **sem salvar** (`rpc.rep` = 4 arquivos/0 MB;
  `pc\out` vazio). Causa provável: analyzers do decompiler em loop (mesmos warnings na mesma
  função por horas) agravados por **GC capado a 2 threads** (default do `analyzeHeadless.bat`)
  com heap 8G. Nada a recuperar do run; projeto `pc` ficou inútil (pode apagar no fim).
- (17/10 ~08:40→09:05) **Diagnóstico rendeu correções (todas validadas no `s2pkg.exe`)**:
  - `GHIDRA_HEADLESS_MAXMEM` (não `MAXMEM`) e `GHIDRA_HEADLESS_JAVA_OPTIONS` funcionam →
    12G heap / `-XX:ParallelGCThreads=6` (MemoryInfo confirmou 12288 MB).
  - **Mecanismo p/ desabilitar analyzers**: flags = `program.getOptions("Analyzers")`
    (chave = nome do analyzer), sincronizadas por `AutoAnalysisManager.initializeOptions()`
    (achado via bytecode; `registerAnalyzerOptions()` sozinho não basta e "Analysis" é
    categoria errada). `scripts/OffAnalyzers.java` (preScript) aplica; teste: PDB Universal
    desligado → linhas PDB 154→6.
  - **Bugs dos meus scripts corrigidos**: `Data` vive em `ghidra.program.model.listing`
    (meus `import ...model.data.Data` não compilavam); args com `|` quebram o cmd via `.bat`
    → `FindString.java` agora lê regexes de arquivo (`<outfile> <patternFile> [maxHits]`).
    Validado: `DumpInventory` no s2pkg = 31.816 funções/4.274 strings/203.995 símbolos;
    `FindString` achou `subsort`/`catalog` com refs (`FUN_1402039a0`, `FUN_1407874d0`).
- (17/10 ~09:05) **Retry do RPC lançado em andamento**: projeto novo
  `C:\Users\Artur\sims2-re\pcb` (`rpc`), disables = PDB Universal + Decompiler Parameter ID +
  Decompiler Switch Analysis + Call Convention ID, cap 60 min (timeout salva parcial —
  comprovado no s2pkg), 12G/6 GC, postScripts DumpInventory+MemoryInfo. Lançado **destacado**
  (WMI; imune a fim de sessão) via `C:\Users\Artur\sims2-re\pcb-run.cmd`; watcher avisa no fim.
  Consultas prontas em `run-queries.ps1` (4 lotes: catalog/bins/paths/ui → `pcb\out\refs-*.tsv`).
- (17/10 ~09:40) **Run `pcb` concluído e salvo** (~16-20 min, exit 0): `DumpInventory` =
  101.530 funções / 25.268 strings / 513.110 símbolos → `pcb\out\{functions,strings,symbols}.tsv`.
  Consultas readonly em ~24 s (4 lotes → `refs-catalog/bins/paths/ui.tsv` + `decomp-main.c`,
  `decomp-handlers.c`). Achados iniciais: serializer do Exchange (Price=0x12, RoomFlags=0x27,
  FunctionFlags=0x28, FunctionSubsort=0x5E/**94**, CommunitySort=0x64) e pares read/write dos
  objetos de build; tabela de diretórios em `FUN_01032f20`; módulo nGameState.
- (17/10 tarde) **Regiões cegas consertadas** (`0xBDF100-0xBE0A77` etc. — código só alcançado
  por vtable): scripts novos `DisasmFix`/`WhoRefs`/`RedoFns`/`NoRetOff`/`FnAt` (ver doc);
  descoberto o prólogo SEH `MOV EAX,<handler>; CALL 0xE5C86C` (helper que prepara
  `ExceptionList` e **retorna**) antes das funções grandes; criadas e decompiladas
  `FUN_00bdf275` (CreateGameDirectoryFileSegment), `FUN_00be0141`, `FUN_00bdfa42`,
  `FUN_00be0787` (BuildResourceMaps) + `FUN_00bdf753` (switch do ResourceManager) e
  `FUN_00cf5a70` (bitmask de EPs).
- (17/10 tarde) **`re-pc-ghidra.md` preenchido**: (a) tabela de props de reflexão em 0x124B470
  (9 props, getters/setters; read-only = `notInCatalog`/`subsort`/`thumbnailResource`/
  `deprecated`), STR# do catálogo (hash `0xFB4CB0DE`), **campo 94 = `FunctionSubsort`** (residual
  53×47 do port fechado); (b) cadeia `BuildResourceMaps` → packed → directory files, seções ini
  (`DBSegments`/`DBSegmentDirectoryFiles`/`PackedDBSegments`/`SampleDataSegments`/
  `TrackDefSegments`/`MusicDirectories`), `%DataDirectory%`/`%Language%\\`, paths `tsdata/` +
  `/tsdata/res/locale/`, `Hitlab.ini`; (c) `Neighborhoods/` pela tabela de segmentos + EPs via
  nGameState (`IsEPInstalled` = `1<<n` sobre a máscara `DAT_01499168`).
- Pendências deixadas: (1) cross-check REA solo (tentativa final — 3 falhas anteriores);
  (2) região dos bins `0xCDF41B`+ ainda sem função definida; (3) handlers de linha
  `0xBDF83B`/`FUN_00bdd8f2`/`FUN_00bde67d` não decompilados.
- (17/10 tarde) Resíduos limpos (projeto morto `pc`, testbeds) e docs commitados.
