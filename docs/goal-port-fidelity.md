# Goal — Portar com fidelidade (OpenSims2 · Windows x64 + núcleo compartilhado)

- **Status:** **em andamento** — aberto em 17/10/2026; roda em **loop autônomo** por itens
  verificados; log no fim. Segue até o Artur mandar parar.
- **Mandato:** Artur — "Faça um goal para gente portar com fidelidade, fique rodando em
  looping para gente fazer história, vamos pra cima." (17/10/2026)
- **Branch:** `windows-x64-ghidra-pc` (17 commits à frente de `origin/main`). Um commit por
  item; mensagem única em inglês (padrão do repo para código) / pt-BR sem acentos (padrão dos
  commits de docs), prefixo de área.
- **Fonte de verdade:** docs de RE deste repo (`re-pc-ghidra.md`, `buy-build-mac.md`,
  `inventario-binario.md`, `*-mac.md`, relatórios) — nada se implementa "por memória": cada
  mudança aponta a âncora/medida que a justifica.

## Objetivo

Fechar, um a um e **com verificação**, os itens de fidelidade do port ao original, até:

1. `tools\build_windows.cmd` verde (build + os 39 ctest do Windows) **a cada item** — nada
   entra quebrado.
2. Residuais documentados fechados com evidência reprodutível (número medido, teste, diff
   determinístico, screenshot comparado à medida do doc).
3. Itens que exigem decisão do Artur marcados `[BLOQUEADO: …]` e seguidos adiante — o loop
   **nunca** fica parado esperando resposta.
4. Fila zerada ou bloqueada; cada ciclo deixa 1 bullet no log e (quase sempre) 1 commit.

## Protocolo de loop (o agente trabalha sem pedir licença entre passos)

1. **Escolher** o próximo item da fila. Prioridade: item em curso > verificável no Windows >
   RE de apoio (quando falta dado).
2. **Entender**: comportamento atual no código vs. original (doc/âncora de RE). Se faltar dado,
   virar mini-RE (Ghidra `pcb` ou consulta ao `re-pc-ghidra.md`).
3. **Implementar** a mudança mínima fiel; seguir o estilo do repo (K&R compacto, ~120 col;
   não existe clang-format).
4. **Verificar**: `cmake --build build-win` + `ctest --test-dir build-win --output-on-failure -j 8`;
   quando aplicável, harness (`s2pkg lot-run`/`autonomy` determinístico, `opensims2_win.exe
   --screenshot` + comparação com a medida do doc, soak curto). Anotar comando + resultado.
5. **Commitar**: 1 item = 1 commit (mensagem com os números).
6. **Logar** 1 bullet aqui e **voltar ao passo 1**.

Comandos do loop (Windows):

- build+testes: `tools\build_windows.cmd` (configura + compila + ctest) ou incremental:
  `cmake --build build-win` e `ctest --test-dir build-win --output-on-failure -j 8`.
- rodar: `jogar.cmd` / `jogar-bairro.cmd` (dados em
  `E:\Games\The Sims 2 Ultimate Collection\Double Deluxe\Base\TSData\Res`).
- screenshot: `build-win\opensims2_win.exe "<Res>" --screenshot frame.png` (sidecars
  `frame_*.png`/`frame_vm.txt` ao lado — gerados pelo próprio app).

Regras de convivência: com o Artur na máquina, **nada de Ghidra/RE pesado em paralelo** (ele
avisa quando liberar — "força total"). Untracked pré-existentes (`app_00.png`, `frame*.png`,
`frame_vm.txt`, `save-test/`, `vc140.pdb`) **não se toca**.

## Não-objetivos

- Sistemas que os docs marcam como "precisa de análise dinâmica" (ficam para sessão com o jogo
  aberto/interativo).
- Rodar os testes black-box de macOS (`tests/*.sh`) — só existem no alvo Mac.
- Refatoração de gosto; performance sem medição antes/depois.
- Mudar docs de RE sem achado novo.

## Fila inicial (ordem de ataque; reordenável pelo log)

- [x] **1. Cross-check REA** (herdado do `goal-ghidra-pc.md`): **FECHADO 17/10 22:38** — REA
      magro (12G + 19 analisadores desligados; ~2 min de análise) devolveu **3/3 âncoras exatas**:
      `TSCatalog` @ `0x0124AE10`, `FunctionSubsort` @ `0x012358AC`, print dos bins @ `0x01262534`;
      evidence `rea-crosscheck.success.json` (`ev_6b0910b0…` em `..\..\rea-snapshots\ghidra-pc\`).
- [x] **2. Baseline Windows**: **FECHADO 17/10 22:39** — `tools\build_windows.cmd`: configure ok,
      `ninja: no work to do` (build já em dia) e **39/39 ctest verdes em 3,29 s**. Sem skips.
- [x] **3. RE relâmpago** (Ghidra `pcb`): **FECHADO 17/10 22:57** — bins = **`FUN_00cdf160`**
      (criada 17/10; dump Frag/Bin, nome resolvido pela STR# `0xFB4CB0DE`); handlers:
      `0xBDF83B` (corpo `FUN_00bdf845` — begin/end_group ignorados; hash→mapa; warning de id
      duplicado), `FUN_00bdd8f2` e `FUN_00bde67d` (corpo `FUN_00bde687`); `FUN_0040d883` =
      `std::string::operator+=(const char*)`. Detalhes em `re-pc-ghidra.md` (§a/§b).
- [x] **4. Buy §1.6 — o painel na tela** (fechado 18/10): a sobra de 1 px na costura
      era leitura equívoca — o endcap (`0x4CC48AA4`, 23 px) fica em 532..555 cobrindo a emenda
      532..536 com o cap da janela em 537 (o slot do script +5) e a coluna 1 em
      589 = 537 + 52 (kCellInX 4→0, o canto (52,19) da própria janela) nas duas
      resoluções; a linha 1 desce para `altura − 129`; o trilho vai de 589 ao navy
      em 1207 (619 = colunas×48−5) e a arte do slider virou barra de cantos r=4
      (trilho com 1 px de navy no topo/base e o fill até as pontas; knob com 2 px de
      navy em todas as bordas e 1 px de (157,171,241) por dentro nas pontas);
      fechado, o endcap vai ao fim do sort (588) e o repeat (`0x4CC48AA6`) estica
      até lá (265..503); o pod sobe 3 px (a pílula em 516/396) em Buy, Build e Live.
- [ ] **5. Buy §1.4 — "6 de 53"** (cadeiras B120): `[BLOQUEADO: perguntar ao Artur se a
      captura do original era de instalação limpa, sem Downloads/CC]`; tentar fechar também
      por RE (o que o campo 94 & 1 esconde / a leitura do knob da barra). Não trava a fila.
- [ ] **6. `inventario-binario.md` §5** — itens portáveis e verificáveis no Windows, em ordem
      de facilidade de verificação (ex.: fila — pessoa 164, evento 4 do ActionSkipped, testes
      `0x214`/`0x2e8`; reset do od 7; slots byte 14 do Look At; NGBH u32 `+0x34`/`+0x38`;
      registros `+0x60c` dos objetos da pessoa). Cada um = 1 commit.
- [ ] **7. `performance.md` "What is left"** — índice por tile, `notifyRoomChanges`/
      `tickOverlayEvents` por tick, "Set to Next" com `std::function`, texturas — com medidor
      antes/depois.
- [ ] **8. Rapas pequenas**: `soak-report` (1 load error recorrente N002_Lot32), `audio.md`
      (SFX que soam como ruído — "sem nome para conferir"), itens corrigíveis do
      `live-qa-report.md`.
- [x] **9. Regressão `1020fd3` — BHAV 288**: **FECHADO 18/10 ~02:30** — não era alvo fora da
      contagem: o cabeçalho **0x8009** (Apartment Life em diante) tem **1 byte a mais** depois
      do *tree version*, e lido como 0x8007 o port desalinhava todas as instruções (o 288 =
      `0x0120` era a primeira palavra lida como alvo). Medido no `objects.package` do AL
      (46.832 BHAVs): **29.014** são 0x8009 e **todas** caem nos alvos com o byte pulado
      (nenhuma com o layout antigo; as 0x8007/0x8008 do mesmo pacote só caem com o layout
      antigo). Conserto em `src/formats/bhav.cpp` (+ teste em `test_formats.cpp`, comentário
      do cabeçalho em `bhav.hpp` e o achado na §1 do `primitives-mac.md`); o `bhav-sweep` do
      AL roda as **46.880 árvores / 11,4 M de instruções** sem nenhum erro de carga e o lote
      com as 17 árvores passa da carga — e mostra o **diálogo de boas-vindas do Pacote de
      Expansão (Vida de Apartamento)**, que antes nem carregava. Os irmãos `JobDataGlobals`
      0x1000/0x1001/0x1002 listam primitivas reais (`Manage Inventory`, `Random Number`,
      `Want Satisfy`). Aberto por isso (fila): 11, 12 e 13.
- [ ] **10. Painel do Build vs capturas 30-34** (`orig2/`): comparar o painel do Build
      mode com as capturas `30_build_top`..`34_build_pisos` como no item 4 (pode ter as
      mesmas sobras de alinhamento).
- [ ] **11. Globais dos EPs** (achado do item 9): com as 17 árvores, semi-globais dos EPs
      leem globais além da tabela carregada — `global 54 out of range (54)` em
      `0x7FD46CD0/0x03E7` ("Lot - Is College Lot? - EP1"), `0x7FD46CD0/0x05DA` ("Neighborhood
      - Is Vacation? - EP6") e `0x7FA903B8/0x1000` (Function - Main do Controller - Ninja),
      cada um resetando o objeto no lote; 456 casos no `bhav-sweep` do AL. Decidir como o
      original junta as tabelas GLOB das 17 árvores (grupo/ordem) e carregar todas.
- [ ] **12. Primitivas `0x7E` (Run Script) e `0x7D` (Follow Sim)** (achado do item 9): o port
      registra as duas como "sem handler" (erro 0x05), mas o AL usa `0x7E` em controladores —
      no lote resetam `Controller - Fish Pond Effects` (`Sub - Remove All Effects` #6),
      `- Business - Customer Loyalty` e `- Employee Management` (`Function - Main`); 2.213
      casos no sweep. A §2 do `primitives-mac.md` dá os handlers **de routing aggregate**
      (`TryRunScript`/`TryFollowSim`, "só BV") — implementar com o gate de tipo de objeto do
      original.
- [ ] **13. TTAB 0x50/0x51/0x54/0x55 dos EPs** (achado do item 9): `interactions (TTAB) …:
      unsupported version 0x50/0x51/0x54/0x55` — as interações desses objetos ficam de fora
      (20 recursos só no `objects.package` do AL). Estender o leitor de TTAB às versões
      novas.

## Critérios de sucesso

- [ ] Critério do goal anterior fechado (cross-check REA de ≥1 achado, evidence JSON).
- [ ] Build + ctest Windows verdes **ao fim de cada item** (evidência no log).
- [ ] §1.6 fechado com screenshot; §1.4 fechado ou mantido com a resposta do Artur.
- [ ] RE relâmpago (bins/handlers/tsdata) documentado em `re-pc-ghidra.md`.
- [ ] ≥1 item do §6 e ≥1 do §7 fechados com medição.
- [ ] Fila atualizada a cada ciclo; nenhum processo pendurado ao final de cada ciclo.

## Riscos

- Ghidra/REA ocupam a máquina → RE pesado só solo (ou com aval do Artur).
- Um item pode "abrir buraco" (depende de sistema ainda não portado) → marcar bloqueado e seguir.
- Sem os dados do jogo em `E:\…\Base\TSData\Res`, verificação de runtime fica limitada ao
  `s2pkg`/testes (anotar quando isso ocorrer).

## Log

- (17/10 ~21:50) Goal aberto; fila montada a partir de `goal-ghidra-pc.md` (pendências),
  `buy-build-mac.md` §1.4/§1.6, `inventario-binario.md` §5 e `performance.md`. O item 1 já está
  em curso (cross-check REA).
- (17/10 ~22:26) Retry #2 do cross-check cortado aos 55 min ainda no meio da análise — o REA
  rodou ~5-6× mais lento que o run manual do `pcb` (17,4 min); culpados prováveis: GC/JIT
  hardcoded em 2 threads e os pesos-pesados ligados (Callee Purge 255 s, Data Reference/
  Reference ~212 s cada...). Tentativa #3 lançada ~22:36 com 19 analisadores desligados +
  watcher `rea-watch-r3.log` (CPU a cada 30 s + cópia do `ghidra.log` do runtime + kill em
  stall); o patch de threads do cache do npx pegou o cache errado (há dois) — replicado no
  certo, a #3 ainda roda com 2 threads. Item 1 segue em curso.
- (17/10 ~22:38) **Item 1 FECHADO**: cross-check REA deu **3/3 âncoras exatas** em ~2 min de
  análise (bullet no `goal-ghidra-pc.md` + seção nova em `re-pc-ghidra.md`); evidence
  `rea-crosscheck.success.json`. O `goal-ghidra-pc.md` fechou (status CONCLUÍDO).
- (17/10 ~22:39) **Item 2 FECHADO**: baseline Windows — `tools\build_windows.cmd` com configure
  limpo, `ninja: no work to do` e **39/39 ctest verdes (3,29 s)**; sem skips. Próximo: item 3
  (RE relâmpago no `pcb` — consultas curtas; máquina livre).
- (17/10 ~22:40-22:57) **Item 3 FECHADO** (RE relâmpago no `pcb`, 6 rodadas headless curtas):
  - **Bins** (`0xCDF41B`+ → era a última pendência do goal (a)): os formatos `Bin Prod ID/...`
    vêm da **`FUN_00cdf160`** (entrada `0xCDF160`, SEH) — dump de Frag/Bin: chave de 12 bytes
    `0x%08X…` ou nome resolvido pela STR# do catálogo (hash `0xFB4CB0DE`); Frag/Bin Prod
    ID/Sort Idx/Type; fonte = vtbl `+0xC/+0x4C/+0x54/+0x64/+0x6C`, sink = vtbl `+0x20/+0x4C`.
  - **Handlers**: `0xBDF83B` (DBSegments; corpo `FUN_00bdf845` = ignora begin/end_group; hash
    do id → mapa; warning `duplicate id %08x = %s` em `TSAudioResourceManager.cpp:0x477`);
    `FUN_00bdd8f2` (SampleData/TrackDef); `FUN_00bde67d` (MusicDirectories; corpo
    `FUN_00bde687`). `FUN_00e5c86c` = `__SEH_prolog` (prólogo comum `MOV EAX,<handler>; CALL`).
    Registro do callback em `FUN_00bdf973` (`[vtbl+0x34](…, 0xBDF83B, 0x142C7F4)`).
  - `FUN_0040d883` = `std::string::operator+=(const char*)` (strlen + append em
    `FUN_00405f2a`) — monta os paths `"tsdata/"`/`"/tsdata/res/locale/"`.
  - Toolkit novo em `rea-snapshots\ghidra-pc\scripts\`: `RedoFrom.java` (limpa garbage e
    re-disassembla a partir de uma entrada) e `ExtendFn.java` (redefine o body). Lição:
    função com `__SEH_prolog` decompila só até o `CALL` do prólogo — o corpo sai criando
    função nova depois do prólogo (encolher o body do wrapper com `ExtendFn` + `MakeFns`).
  - Edits em `re-pc-ghidra.md` (§a Bins + §b handlers/`FUN_0040d883`); build+ctest
    **39/39 verdes (2,19 s)**.
- (17/10 22:57) Próximo da fila: **item 4** — §1.6 do `buy-build-mac.md`, o pixel da costura
  do catálogo (532×533); medir com `--screenshot` no Windows.
- (18/10 ~01:30) **Item 4 FECHADO** — §1.6 medido e corrigido no app e no doc. As capturas
  do jogo (800x600 e 720) saem com o cliente em +(3,29) (o port, em (0,0)): aplicando o
  offset, o catálogo está 5 px à direita do slot do script (cap em 537, não ≈533), a coluna
  1 em 589 = 537 + 52 (kCellInX 4→0 — o "+4+8" antigo era o canto da janela da coleções), a
  linha 1 em `altura − 129` (não −126) e o trilho de 589 ao navy em 1207 (619 = colunas×48−5,
  não 591..1211/620). O endcap (`0x4CC48AA4`) fica em 532..555 cobrindo a emenda 532..536 (o
  joint `0x4CC48AA8` não desenha em estado nenhum) e, fechado, vai ao fim do sort (588) com o
  repeat (`0x4CC48AA6`) esticado do 265 até lá; o pod sobe 3 px em Buy, Build e Live (a
  pílula em 516/396 sobre os 519/399 do script). A arte do slider (achada na verificação) não
  era um estádio: é uma barra de cantos r≈4 (a coluna extrema abrange 8 das 12 linhas) —
  trilho com 1 px de navy no topo/base e o fill até as pontas, knob com 2 px de navy em todas
  as bordas e 1 px de (157,171,241) por dentro nas pontas. Verificação: capturas do port em
  720 (aberto — `cmp5` com o slider novo — e fechado) contra o jogo e o script; o botão do
  grid bate linha a linha (coluna da cadeira 591..616 = 500..525 do jogo) e a barra fecha no
  mesmo navy (605 no cliente) nos dois; diferenças de tom pequenas (arte Mac × Win). Falsos
  positivos derrubados: o "verde × azul" dos subsorts do jogo (é o painel 0xB100 do Seating
  com o B120 selecionado vs o port forçando Decoração — estados diferentes) e o "frame 0" do
  Comfort (o jogo desenha o frame 1 azul como o port). Fica de polimento: as bevels do knob e
  o raio das placas (as artes do jogo divergem ±1 px nas colunas extremas). build+ctest
  39/39. Próximo: item 9 (regressão `1020fd3`), depois 5 (bloqueado) / 6 / 7 / 8 / 10.
- (18/10 ~02:30) **Item 9 FECHADO** — a regressão da carga com as 17 árvores não era alvo
  fora da contagem: o cabeçalho **0x8009** tem 1 byte a mais depois do *tree version* e o
  leitor (que já lia 0x8007) desalinhava todas as instruções. Varredura do `objects.package`
  do AL: 29.014 de 46.832 árvores são 0x8009, todas caem nos alvos com o byte pulado e
  nenhuma com o layout antigo; conserto em `bhav.cpp` + teste em `test_formats.cpp`
  (build+ctest **39/39**). Verificação: `bhav-sweep` do AL = 46.880 árvores / 11.379.667
  instruções, zero erro de carga (17.589 verdadeiros, 15.531 falhas de semântica, 8.733
  falsos, 5.027 yields); o lote com o Double Deluxe + as 17 árvores (sem junction) passa da
  carga e mostra o **diálogo de boas-vindas do Vida de Apartamento**. Achados enfileirados:
  11 (globais dos EPs), 12 (`0x7D`/`0x7E` em routing aggregates), 13 (TTAB 0x50+). Próximo:
  6 (seguido de 7 / 8 / 10; 5 bloqueado).
