# Goal — Fechar as pendências do port (item 13 e a fila restante)

- **Status:** **aberto em 18/10/2026** — continua o `goal-port-fidelity.md` (itens 1–4, 9 e
  11 fechados; 6 parcial; 5, 6-resto, 7, 8, 10, 12, 13 e 14 abertos). Roda em **loop
  autônomo**, sem pedir licença entre passos, até a fila zerar ou só restarem itens
  bloqueados com critério escrito.
- **Mandato:** Artur — "Faça um goal para resolver próximos passos e pendências"
  (18/10/2026).
- **Branch:** `windows-x64-fidelity` — os 21 commits do nosso trabalho rebased sobre o
  `origin/main` novo (**v0.5.0**, `244cc68`); o antigo `windows-x64-ghidra-pc` ficou em
  `backup/ghidra-pc-v040`. Um item = um commit, linha
  única: código em inglês, docs em pt-BR **sem acentos** na mensagem; área como prefixo.
- **Regras herdadas (valem para todo passo):**
  - Toda mudança aponta a âncora/medida que a justifica; o que não estiver medido fica
    marcado **"inferido"** no código/doc.
  - `tools\build_windows.cmd` (configure + build + ctest) **39/39 verdes a cada item**.
  - Nunca tocar nos untracked pré-existentes: `app_00.png`, `frame*.png`, `frame_vm.txt`,
    `save-test/`, `vc140.pdb`.
  - Reverter **toda** instrumentação TEMP antes de commitar (hoje: o dump de TTABs em
    `cli/s2pkg/main.cpp`, env `S2PKG_TTAB_RAW` — uncommitted).

## Objetivo

Tirar a fila do port do ponto morto, começando pelo maior ganho funcional já medido — os
**1.177 TTABs** do `objects.package` do Apartment Life que hoje são recusados e deixam os
objetos sem interações — e fechar (ou bloquear com critério) o resto: pacote vazio do soak,
vizinhanças/lotes do AL, performance, painel do Build, `RayTrace` e as primitivas
`0x7D/0x7E`.

## Ponto de partida medido (18/10/2026)

### Item 13 — TTAB

- Censo no `objects.package` do AL (decode-all): **1.177** TTABs recusados —
  **0x4F 149, 0x50 106, 0x51 21, 0x53 7, 0x54 440, 0x55 454** (no log do decode-all são
  1.176 linhas; o dump TEMP gravou 1.177 arquivos). O gate do port hoje é `0x3F..0x4E`
  (`InteractionTable::parse`, `src/formats/simantics.cpp`).
- **0x4F / 0x50 / 0x51 (276 arquivos): layout idêntico ao 0x4E** — parse byte-exato em
  **276/276**: contagens, entradas e nome final fecham sem sobra. Pronto para portar.
- **0x53 / 0x54 / 0x55 (901 arquivos): formato novo, estrutura medida:**
  - Cabeçalho igual ao antigo (nome 64 B, typeCode, versão, `n` u16 em +76).
  - Entrada = **54 bytes fixos** + cauda. Campos fixos medidos por assinatura estrita
    (`join = -1` em +24, `ui = 0` em +28, `mem = 0.5f` em +34, `obj = 0` em +38,
    `model = 1` em +42) em milhares de entradas:
    action u16 +0 · guard u16 +2 · flags u16 +4 · flags2 u16 +6 · stringIndex u32 +8 ·
    attCode u32 +12 · attValue f32 +16 · autonomy u32 +20 · joinIndex i32 +24 ·
    uiDisplayType u16 +28 · facialAnimation u32 +30 · memoryMultiplier f32 +34 ·
    objectType u32 +38 · modelTableId u32 +42 · **X** u16 +46 (e 0 em +48) · **Y** u16 +50
    (e 0 em +52).
  - Cauda = **X × (4 + 6·Y)** bytes: X grupos, cada um com **header de 4 bytes + Y anúncios
    de 6 bytes** (o trio `min, delta, personality` do formato antigo). Evidência (tamanho
    da entrada − 54): (5,10)→320, (5,14)→440, (5,7)→230, (5,16)→500, (5,6)→200,
    (5,1)→50, (1,16)→100, (1,8)→52 — os casos com **X≠5** derrubam o modelo anterior
    `4X + 30Y` (que só coincide quando X=5); no E2F o trio `(50,10,0)` aparece a cada
    **64 = 4 + 6×10** bytes, no fim de cada grupo.
  - **Status do parse em nível de arquivo:** fecha exatamente (n entradas + nome final) em
    **255/440** dos 0x54 e **300/454** dos 0x55 — **555 arquivos**; em nível de entrada, o
    modelo `4X+30Y` já fechava **4.217/4.934** e o refinamento `X×(4+6Y)` corrige os casos
    X=1 (100/94/88/52).
  - **Anomalias abertas:** (a) entradas lidas como **(X=0, Y=8)** com cauda de **320 bytes**
    (o modelo daria 0) — 79 ocorrências de um mesmo tamanho, hipótese principal: entrada
    anterior não detectada pela assinatura estrita ou contadores lidos fora de lugar;
    (b) os **7 arquivos 0x53** não fecham (um "untitled tree table" tem quatro u32 = 16 na
    região fixa e não bate na assinatura); (c) semântica do header de 4 bytes do grupo
    (índice? contagem?) e mapeamento para os `motives` do port ainda **inferidos**.
  - Ferramentas: dumps em `%TEMP%\opencode\i6e\ttab\` (**1.177 `.bin`**) e analisadores
    `ttab_*.py` na mesma pasta; o `ttab_model2.py` (validação do modelo) saiu **sem
    saída** — primeiro passo é rodá-lo com log em arquivo e traceback.
- **Fio do Ghidra (PC):** o leitor de TTAB no `Sims2EP9RPC.exe` ainda não foi localizado
  (`ScanConsts` poluído por strings/GUIDs; `FUN_0082fa60` não inspecionada a fundo). Próxima
  tentativa: filtrar escalares por **bitLength == 32** e varrer o type id **0x54544142** +
  as versões `0x4F..0x55`; se o modelo de dados fechar antes, o Ghidra vira só confirmação.

### Outras pendências

- **5 (Buy §1.4, "6 de 53"):** **FECHADO 21/10** — o census provou que os mods desta máquina
  (TS2 Extender, Lighting Mod UC, `userStartup.cheat`, Starship UI — o `README_MODS.txt` do
  backup `_MOD_BACKUP_20261016`) **não** acrescentam objetos de catálogo: o
  `ObjectScripts.package` do Extender tem 0 OBJD, os `Overrides` do mod de luz só repetem
  GUIDs já contados e o tema de UI não tem objetos. Os 6 a mais da captura eram conteúdo do
  `Downloads` do usuário, que esta máquina não tem mais (o backup guardou só os arquivos
  substituídos). O port conta a instalação: 47 cadeiras, 1666 compráveis.
- **6 (resto):** pessoa 164, ActionSkipped "evento 4", testes `0x214`/`0x2e8`, NGBH u32
  `+0x34`/`+0x38`, registros `+0x60c` e a troca de fila pela vtable `0x1FC` — todos pedem RE
  do binário **Mac PPC**, que fica **com o mantenedor do Mac** (decidido pelo Artur em
  21/10: "mac é com o mantenedor"). Não bloqueia mais nada aqui.
- **8:** `soak-report` — erro recorrente do `N002_Lot32` (stub DBPF de 96 bytes, pacote
  vazio); fechar distinguindo/countando pacote vazio.
- **7:** `performance.md` "What is left" — índice por tile, `notifyRoomChanges`/
  `tickOverlayEvents` por tick, "Set to Next" com `std::function`, texturas — com medidor
  antes/depois.
- **10:** painel do Build vs capturas `30_build_top`..`34_build_pisos` (mesmo método do
  item 4).
- **12:** primitivas `0x7D`/`0x7E` — os docs só dão os handlers BV ("só BV"); o AL usa `0x7E`
  em 2.213 casos, então o comportamento tem de sair do **exe do PC** (routing aggregate)
  antes de implementar.
- **14:** lotes/vizinhanças do AL — `NGBH 0xCB`, `FAMI 0x55`, `SDSC 0x36`, `NID +20` bytes e
  `lot info` de apartamento; medir nos arquivos do UC e estender os leitores.
- **RayTrace (0x78):** usa a mesma convenção de slot do Look At (censo de 35 leituras
  tipadas no AL); o port ainda aproxima a linha de tile.

## Fila (ordem de ataque)

Cada item só fecha com **build + ctest 39/39**, evidência medida no log e **1 commit**.

1. **13a — gate 0x4F..0x51** (o ganho barato): ✅ **FECHADO 18/10** — o gate
   aceita `0x3F..0x51` com o layout 0x4E; `testInteractionTableVersions` cobre
   0x4E..0x51 (campos, anúncios, nome) e recusa 0x52/0x53/0x55; TEMP revertido.
   Medição: decode-all do AL com **901** falhas (só 0x53 7 / 0x54 440 / 0x55 454)
   e **1.208** ok; build+ctest 39/39; `data-findings.md` §17.
2. **13b — fechar o modelo 0x53+**: **FECHADO 19/10** (o 0x53 fechou em 20/10, ver a §19) — o RE
   do leitor de TTAB no `Sims2EP9RPC.exe` (`FUN_0087c720` @ `0x87c720`, no projeto Ghidra
   `C:\Users\Artur\sims2-re\pcb`) deu o layout: 0x44–0x52 = 7 tabelas, **0x53 = 11
   tabelas**, **≥ 0x54 = 46 bytes fixos + dois arrays de grupos de anúncios** (o primeiro,
   `[u32 grupos]` e por grupo `[u32 m]` + `m` anúncios de 6 bytes; o segundo, por grupo,
   `[u32 X]` + `X−1` itens de 4 bytes de conteúdo desconhecido + `[u32 m]` + `m` anúncios).
   Com esse layout o leitor fecha **878 dos 894 arquivos 0x54+** (o modelo anterior fechava
   555 dos 901). Restam 12 com o `X` do segundo array ambíguo (lido como u32 dá
   `0x00050000`; os bytes sugerem um par menor que 4 bytes) — os 7 arquivos 0x53 fecharam
   em 20/10 (item 1 da fila de 20/10).
3. **13c — leitor novo no port:** **FECHADO 19/10** para 0x54/0x55 e **20/10** para 0x53 —
   `InteractionTable::parse` ganhou o caminho novo (46 bytes fixos + os dois arrays, cada
   grupo virando uma tabela de motivos; no 0x53, o layout do 0x4E com **11** tabelas). O
   teste `testInteractionTableVersions` cobre 0x4E..0x51, 0x53 (11 tabelas) e 0x54/0x55, e
   recusa 0x52 e acima de 0x55. Medido no `objects.package` do Apartment Life: **2.097 ok
   / 12 falhas** (antes 1.208 ok / 901 falhas; 2.090/19 antes do 0x53) — as 12 são o `X`
   ambíguo (9 + 2) e 1 "693 trailing bytes" (`0x7FB36E92`, grupo `0x80`). `data-findings.md`
   §18 e §19 registram o RE, os layouts e as medições.
4. **8 - N002_Lot32 / pacote vazio:** ✅ **FECHADO 20/10** — `Lot::load` distingue o pacote
   vazio (o stub de 96 bytes sem recursos, `EmptyLotPackage`) do erro "no elevation array", o
   `s2pkg` responde "empty lot package" (sem "error:") e o `soak.sh` conta os lotes vazios à
   parte (`empty|autonomy|...`, `empty|lot-save|...`); rodada do lote afetado medida em
   `docs/soak-report.md` (round 19); teste `testEmptyLotPackage` (o stub de 96 B escrito pelo
   `DbpfWriter`); build+ctest 39/39.
5. **14 — vizinhanças/lotes do AL:** ✅ **FECHADO 20/10** — NGBH 0x6F/0x70/0xBE/0xC2/0xCB,
   FAMI 0x4E..0x55 (gates 0x4F/0x50/0x51/0x55), SDSC 0x20..0x36 (com o 0x35 do template
   E002) e a cauda BV, NID 3..0x0A e lot info 18/base 11 (cabeçalho de apartamento, cauda
   em `rest`); teste `test_neighborhood` por formato (ida e volta), `hood-sweep` do E001 em
   **234 Sims / 21 famílias / 56 lotes / 0 erros** (antes 0/0/0/313) e `lot-run` do
   `E001_Lot127` por 60 ticks; as versões foram medidas nos 20 pacotes do UC
   (`data-findings.md` §20).
6. **7 — performance:** um sub-item por vez (índice por tile, notificações por tick, Set to
   Next, texturas), medidor antes/depois no log; 1 commit por sub-item. **Primeiro sub-item
   FEITO 21/10** — o `VM::tick` não reconsulta mais o mapa das threads: ele guarda os
   **iteradores** (os nós de um `std::map` são estáveis) num vetor que é membro da VM, e o
   `deleteGroup` passa a **matar** a thread do objeto removido e a anotá-la (ID + serial)
   em `VM::deadThreads` para o tick a apagar no fim, em vez de apagar durante (o
   comportamento é o de antes: as threads criadas no tick só recebem a vez no tick
   seguinte, e a thread de um objeto removido não roda). Medido no Windows (`autonomy
   N001_Lot11 --ticks 43200`, `S2PKG_NO_LIGHT=1`): **22,3 s / 21,3 s → 19,5 s / 18,2 s**
   com a saída **idêntica** (as 516 linhas); build + ctest 39/39. A mudança também
   conserta um **comportamento indefinido**: o `deleteGroup` apagava a thread do objeto
   removido *enquanto ela rodava* (o `Thread&` do `VM::run` ficava pendurado); agora a
   thread é **morta** (o status `Killed`, a regra do jogo) e apagada no fim do tick. Por
   isso o `bhav-sweep` do AL muda em **3 das 46.880 árvores** (18.625 → 18.622 true,
   14.126 → 14.129 falhas, e 4 "Slot number out of range" a mais: as árvores cuja thread
   foi morta no lugar de continuar com a memória liberada). Fica de fora: o índice por
   tile, `notifyRoomChanges`/`tickOverlayEvents` por tick e as texturas. O sub-item do
   "Set to Next sem `std::function`" foi **medido sem ganho** (22,2/22,5 s contra
   22,3/21,3 s) e descartado — o registro está em `performance.md`.
7. **10 — painel do Build:** **FEITO 21/10 pelo método do item 4** (as capturas 30–34 não
   existem nesta máquina): o painel foi auditado contra a tabela do `buy-build-mac.md` §7.8 e
   contra o censo novo do catálogo de construção (`s2pkg catalog --installation --build`), e
   a auditoria achou **três lacunas**, corrigidas: a categoria **A800 jardinagem** inteira
   (não havia `case 8` — as árvores, os arbustos e as flores, o build type 4 com os bits
   0x01/0x02/0x04), as **colunas BA20** (o subsort 2 do painel de diversos, o build type 1
   com o bit 0x200) e os **pincéis B751–B753** (o subsort 5 do painel de terreno, as
   ferramentas de jardim, o build type 4 com o bit 0x10). O `B620` (a ferramenta de
   corrimão) já estava no clique do subsort, e o resto da tabela bate com o port.
8. **RayTrace 0x78:** ✅ **FECHADO 20/10** — o 0x78 resolve o ponto pelo slot do mesmo
   jeito que o Look At (byte 4 = 1/2/3 → os SLOT de tipo 4/3/0, byte 5 = o índice dentro
   deles, pasto a contagem erro 0x1B; o alvo nos bytes 11/12) e, sem kind, pelo tile do
   objeto; antes o port usava o tile dos dois, o que não batia com o jogo. Teste novo
   `testRayTraceSlots` (o slot, o caso sem slot e o 0x1B) e sweep do AL medido (4 erros
   0x1B a mais, as árvores globais que pedem um slot que o objeto não tem — o que o jogo
   também dá); build+ctest 39/39.
9. **12 - 0x7D/0x7E:** **RE do PC FEITO 20/10, implementação bloqueada no último passo** — no exe
   do PC (o build do AL) as duas primitivas estão na **classe base** (mesma tabela de despacho
   do RayTrace 0x78 e do Change Outfit 0x79), e não só no routing aggregate como no Mac: o
   gate de tipo de objeto é "qualquer objeto". Medidos: os handlers (`FUN_008b9bd0` Follow Sim
   e `FUN_008b9e00` Run Script), o layout dos parâmetros (id u16, valor u16, byte de flags
   do escopo Global/Semi-Global/Private e três pares (tipo, valor)), o id empacotado
   `(op0-1 << 16) | op2-3` e o fallback para `.objLua`; no AL são 2.876 chamadas de 0x7E (id
   sempre 304) e 21 de 0x7D. **O RE avançou em 21/10 (leitura em assembly do handler
   `FUN_008b9e00`)**: do byte de flags só o **bit 0** é lido (os bits 1-3 são recomputados
   a partir dele e valem 0, e o ramo "Private" é código morto neste build); o escopo é o
   **grupo global** por padrão e o **semi-global** quando o byte em `[ESP+0x12]` — que o
   `vfunc 0x1c` do **objeto** preenche — é diferente de zero; o **nome** do script vem do
   objeto (`vfunc 0x1c` e `vfunc 0x18`), não de um índice; e o handler faz **dois pedidos**,
   com os pais `0x2026960B` e `0x4D45F8E6` ("defined in description" × "defined in objLua
   file"). **Experimento descartado (21/10)**: implementar o 0x7E como "a árvore pelo índice
   na lista de nomes do grupo" (STR# 0x12F) até reduzia as falhas do AL em 2.522 (12.894.214 →
   13.175.936 instruções, 18.625 → 19.271 true, 14.126 → 11.604 falhas), mas criava 232
   falhas novas ("tree has no instructions") e 11 recursões: a instrumentação mostrou que
   4.994 das chamadas caem no semi-global do grupo global (que não existe) e as do escopo
   global não acham a árvore. Como o nome vem do objeto no binário, o modelo foi revertido e
   o port **continua recusando o 0x7E** (erro 0x05). **Falta**: o sentido do byte em
   `[ESP+0x12]` e o `vfunc +0xC` do gerenciador de recursos (a busca do recurso de tipo
   0x89B7DC8), que é onde o alvo é resolvido; e o 0x7D ("Follow Sim"), cujo handler pede o
   array de objetos e a referência `0x2026960B` (o que ele faz com o array não foi medido).
   `docs/re-pc-ghidra.md` §(d) e `docs/data-findings.md` §21 têm o registro completo.
10. **6-resto:** só com o binário **Mac PPC**, que fica com o mantenedor do Mac (decidido em
    21/10); o item 5 fechou por censo. Ficam por último, sem travar a fila.

## Critérios de sucesso

- [ ] Item 13 completo: 1.177 TTABs do AL lidos (0x4F..0x55) e interações visíveis no app.
- [ ] Build + ctest 39/39 ao fim de **cada** item (evidência no log).
- [x] Item 8 fechado com o soak-report citando o caso; itens 7 e 10 com medição antes/depois.
- [x] Item 14: `E001_Lot127` do AL carrega (ou o residual está documentado com bytes).
- [ ] RayTrace e 12 fechados com sweep/teste; 5 e 6-resto explicitamente bloqueados.
  (RayTrace **fechado em 20/10**; o 12 ainda espera o RE do exe do PC.)
- [ ] Nenhum TEMP no diff final; nenhum untracked pré-existente tocado.

## Riscos

- O modelo 0x53+ pode ter mais de uma variante (0x53 diferente de 0x54/0x55) → aceitar só o
  que fechar e manter o resto recusado com erro claro (nada de parse "otimista").
- O Ghidra pode continuar sem achar o leitor; o modelo de dados é a fonte primária — o exe
  entra como confirmação, não como bloqueio.
- Itens de Mac PPC e a pergunta do item 5 não têm prazo; ficam registrados como bloqueados.
- A máquina é compartilhada com o Artur: Ghidra pesado só solo.

## Log

- (18/10) Goal aberto a partir do estado do item 13 (1.177 TTABs; modelo 0x53+ medido até
  `X×(4+6Y)`, 555 arquivos fechando) e da fila restante do `goal-port-fidelity.md`. Primeiro
  passo: reverter o TEMP e fechar 13a.
- (18/10) **13a FECHADO** — `InteractionTable::parse` aceita 0x4F..0x51 com o layout 0x4E
  (276 recursos medidos); `testInteractionTableVersions` (0x4E..0x51 ok; 0x52/0x53/0x55
  recusados); TEMP revertido; decode-all do AL: **901 falhas** (0x53 7 / 0x54 440 / 0x55
  454) e 1.208 ok; build+ctest **39/39**; `data-findings.md` §17 registra a medição.
  Próximo: **13b** (fechar o modelo 0x53+ — anomalias (0,8) e os 7 arquivos 0x53).
- (18/10) **Reconciliação com o upstream v0.5.0** — o `main` do `opensims2` andou 48 commits
  desde `fc6694f` (v0.4.0) e já traz o **nosso** port Windows (o `52af245`, PR #1 aplicado
  pelo mantenedor, que fechou o PR sem merge). Os 21 commits nossos que ficaram de fora
  (catálogo da instalação, Buy mode medido, câmera, renderer, BHAV `0x8009`, TTAB
  `0x4F..0x51`, Look At, conjuntos de variáveis, reset do od 7 e os docs dos goals) foram
  rebased sobre `244cc68` no branch `windows-x64-fidelity`; um único conflito, na linha dos
  Erros do `inventario-binario.md`, resolvido com o texto novo do upstream + a correção do
  reset do object data 7. Diff final contra o upstream: **33 arquivos, +1.893/−135**.
  O que o upstream fechou **para nós** (núcleo compartilhado, o port herda): a rodada n46
  ("VM, resto") levou os registros da pessoa (`cTSPerson +0x60c`), o 109/164, o
  `UpdatePersonDataFields` (37/38/40/55), o `cTSGlobals`/`cTSDataTables` e as sobras da fila
  (evento 4 do `ActionSkipped`, `0x214`/`0x2e8`, a troca da fila cheia `0x1FC`) — ou seja,
  o **item 6 inteiro** da fila do `goal-port-fidelity.md`; e a n47 ("Salvar, resto") mediu o
  `NGBH` (`+0x34`/`+0x38` = tiles, `+0xFD` um bool sem uso, o dono `0x22` só de leitura 1–15)
  e a transação do persist system. O que **continua nosso** (o upstream não tocou):
  item 5 (bloqueado), 7, 8, 10, 12 (`0x7D`/`0x7E`), 13b, 14 e RayTrace — o item 14 segue
  exclusivo nosso: o upstream ainda só lê NGBH `0x6F`, SDSC `0x20` e FAMI `0x4E`, e o AL
  usa `0xCB`/`0x36`/`0x55`. A fila do upstream agora é 48 (colocação), 49 (rotas),
  50 (água/objetos/cinema), 51 (CAS) e 52 (bairro) —reas de 48/49 tocam colocação e rotas,
  onde o nosso item 7 (performance) mede.
- (19/10) **13b/13c FECHADOS para 0x54/0x55** — RE do leitor de TTAB no `Sims2EP9RPC.exe`
  (Ghidra, projeto `C:\Users\Artur\sims2-re\pcb`; o type id `0x54544142` aparece 15 vezes no
  binário e o leitor é `FUN_0087c720` @ `0x87c720`, na vtable da classe em `0x1222cfc`):
  a entrada 0x54+ tem **46 bytes fixos** (as flags do 0x4E, sem as contagens de motivo) e
  depois **dois arrays** de grupos de anúncios — o primeiro `[u32 grupos]` e, por grupo,
  `[u32 m]` + `m` anúncios de 6 bytes; o segundo, por grupo, `[u32 X]` + `X−1` itens de
  4 bytes (conteúdo desconhecido) + `[u32 m]` + `m` anúncios. Os dois arrays usam
  funções diferentes no binário (`FUN_00879690` e `FUN_00879800`), o que explica os
  formatos diferentes dos itens. O RE também disse que **0x53 tem 11 tabelas** (e não 7).
  Medido: o leitor fecha **878 dos 894 arquivos 0x54+**; o `decode-all` do
  `objects.package` do Apartment Life passa de **1.208 ok / 901 falhas** para
  **2.090 ok / 19 falhas** nas TTABs (as 19: 7 de 0x53, que segue recusado, e 12 com o
  `X` do segundo array ambíguo). `simantics.cpp` tem o caminho novo, o teste
  `testInteractionTableVersions` cobre 0x54/0x55, build+ctest **39/39**, e a
  `data-findings.md` §18 registra o RE, o layout e os bytes do resíduo.
- (19/10) **Rebase sobre o upstream v0.6.0** (`ff4f71a`) — o mantenedor andou **53 commits**
  desde o v0.5.0 e **implementou o port Windows em paralelo**: o renderer com GUOB/bounds/
  planos de luz (`34db5be`), o refactor da câmera (`setKeyMove`/`setKeyZoom`/`setKeyRotate`),
  o host que constrói no macOS também (`ce98217`, com `OS2_WINDOWS_CHECK`) e as releases
  `opensims2-windows-x64.zip` (o `main` agora anuncia macOS **e** Windows). Por isso os
  **dois commits nossos** de câmera e renderer foram **omitidos** no rebase (o upstream já os
  tem, com a mesma medição) e o branch ficou com 21 commits. O que continua só nosso: o
  catálogo da instalação (448 → 1.666), o Buy mode medido, o BHAV `0x8009`, o Look At com
  slots, os conjuntos de variáveis, o reset do od 7 e **as TTABs 0x4F..0x55** (o upstream
  ainda recusa tudo acima de 0x51). Build+ctest **39/39** local e no CI do Windows; o
  `decode-all` do AL mantém **2.090 ok / 19 falhas** nas TTABs. PR #2 (`windows-x64-fidelity`
  → `main`) mergeável, com o CI verde.
- (20/10) **0x53 FECHADO** (item 1 da fila de 20/10) — o 0x53 é o **layout do 0x4E com 11
  tabelas de motivos**: `action` u16, `guard` u16, **11 contagens u32** e depois os mesmos
  campos do 0x4E, com as 11 tabelas de anúncios (6 B cada) no fim. Os 7 recursos do AL
  (todos "untitled tree table", conteúdo idêntico, 2.387 B) fecham exatamente no nome
  final: entradas de 570 B (contagens `[16,0,0,0,0,0,0,16,16,16,16]` → 80 anúncios) e de
  1.146 B (11×16 = 176 anúncios na terceira); os anúncios são zero neste recurso, por isso
  a cauda parecia padding. `InteractionTable::parse` passou a ler o 0x53 no caminho do
  0x4E (11 contagens) e a recusar 0x52 e > 0x55 (nada acima de 0x55 aparece no conteúdo);
  `testInteractionTableVersions` cobre 0x4E..0x51 e 0x53 e recusa 0x52/0x56; build+ctest
  **39/39**; `decode-all` do AL: **2.097 ok / 12 falhas** (antes 2.090/19). As 12 restantes
  são o `X` do segundo array do 0x54/0x55 (9 "bad TTAB item count" + 2 "bad advertisement
  count") e 1 "693 trailing bytes" (`0x7FB36E92`, grupo `0x80`). `data-findings.md` §19
  registra o layout e a medição. Próximo: item 2 da fila (0x53 fechado; segue **14 —
  vizinhanças/lotes do AL**).
- (20/10) **14 FECHADO** — as versões das vizinhanças do AL entraram: **FAMI** 0x4E..0x55
  (0x4F põe o bairro depois do álbum, 0x50 o último lote jogado, 0x51 o business funds e
  0x55 o lote de férias; o 0x50 do B001 tem o último lote e não o business), **SDSC**
  0x20..0x36 com a tabela de *person data* (170/179/195/199/200/201/204/228/**230** no
  0x35 do E002/231) e a cauda do BV (u64 de colecionáveis), **NGBH** 0x6F/0x70/0xBE/0xC2/
  0xCB (EP ready marker, versão por slot, flags2, número de inventário e o u16 do Seasons),
  **NID** 3..0x0A (os 20 bytes do E001) e **lot info** 18/base 11 (cabeçalho de
  apartamento; a cauda de sublots fica em `rest`). Medido: `hood-sweep` do E001 passa de
  **0 Sims / 0 famílias / 0 lotes / 313 erros** para **234 Sims / 21 famílias / 56 lotes /
  0 erros**; N001–N003, F001, G001 e os 13 templates decodificam com 0 failed;
  `lot-run` do `E001_Lot127` por 60 ticks carrega 485 objetos (483 no VM; prédio de
  apartamentos sem família) e só esbarra nos 0x7E/Set to Next 38 do item 12; build+ctest
  **39/39**; `test_neighborhood` cobre as versões novas; `data-findings.md` §20 registra
  os layouts. Próximo: item 3 da fila (**12 — 0x7D/0x7E**).
- (20/10) **RE do item 12 (0x7D/0x7E) no exe do PC** — as strings do binário
  (" Follow Sim in ", "defined in objLua file", "Semi-Global", "Global", "Primitive not supported
  for this type of object.") levam aos handlers `FUN_008b9bd0` (Follow Sim) e `FUN_008b9e00`
  (Run Script); a tabela de despacho por opcode (thunks de 35 B a partir de `0x8ba900`, tabela
  em `0x8bab34+`) mostra as duas **na classe base** (mesma tabela do RayTrace 0x78 e do Change
  Outfit 0x79) — no Mac base elas não existem, e no BV são do routing aggregate; o gate de tipo
  de objeto é "qualquer objeto". Medidos: o layout dos parâmetros (id u16 em op0-1, valor u16
  em op2-3, byte de flags em op4 com os bits Global/Semi-Global/Private e mais dois de cache,
  e três pares (tipo u8, valor u16) em op6/op7-8, op9/op10-11 e op12/op13-14), o id empacotado
  do pedido `(op0-1 << 16) | op2-3` e o fallback do 0x7E para um arquivo `.objLua` (o registro
  de scripts Lua, `FUN_007fd6c0`). Censo do AL: **2.876** instruções 0x7E (em 567 grupos, id
  sempre 304) e 21 de 0x7D. **Bloqueio**: o id 304 (`0x130`) não é instância de árvore em
  nenhum escopo (no grupo do chamador não existe a 0x130; a única é a global "Family -
  Relationships - Reset") e nenhum op2-3 bate com uma instância do grupo — falta resolver a
  que o pedido é resolvido (RE de `FUN_00407f8a` e do registro de tipos). Registro completo em
  `docs/re-pc-ghidra.md` §(d) e nota no `primitives-mac.md` §2; o port continua recusando as
  duas (erro 0x05) e as 2.876 chamadas do AL continuam falhando.
- (20/10) **RayTrace 0x78 FECHADO** — o 0x78 resolvia o ponto pelo tile do objeto (uma
  aproximação); agora usa a mesma convenção de slot do Look At: byte 4 = 1/2/3 → os SLOT de
  tipo 4/3/0 (targeting/routing/containment) e byte 5 = o índice dentro deles, pasto a
  contagem erro **0x1B** (o mesmo do Look At), e o alvo pelos bytes 11/12; sem kind, o meio
  da caixa (no port, o tile do objeto). Medido no AL (`bhav-sweep`): antes 12.894.214
  instruções, 18.625 true e 14.126 falhas; depois 12.894.204, 18.622 true e 14.129 falhas —
  as 4 a mais são **"Slot number out of range"** (0x1B), as árvores globais que pedem um
  slot que o objeto não tem, que é o que o jogo dá. O censo do AL: 47 instruções 0x78 (o
  kind da origem em 3/0/1/255 e o do alvo em 3/2/1/0) e 1.052 SLOTs com no máximo 1
  entrada de cada tipo (2 de contenção), o que confirma o 0x1B nos índices altos. Teste novo
  `testRayTraceSlots` (o slot, o caso sem slot e o 0x1B); build+ctest **39/39**. No jogo base
  o mesmo desenho (22.861 árvores): antes 7.234.322 instruções, 11.110 true e 2.458 falhas;
  depois 7.234.312, 11.107 true e 2.461 falhas, com os mesmos 4 "Slot number out of
  range" a mais (0x1B).
- (20/10) **Item 8 FECHADO** — o `N002_Lot32` (stub DBPF de 96 bytes, sem recursos) deixava de
  ser erro de carga: `Lot::load` agora lança `EmptyLotPackage` (novo tipo em `formats/lot.hpp`)
  quando o pacote não tem nenhum recurso, o `s2pkg` responde "empty lot package: the lot
  package has no resources (the empty lot stub)" (sem "error:", sai com 0) e o `tools/soak.sh`
  conta os lotes vazios à parte (`empty|autonomy|...` e `empty|lot-save|...`, nova categoria
  "empty" no relatório). Medido: rodada do lote afetado (`soak.sh --one N002_Lot32`,
  `SOAK_TICKS=1800`, `SOAK_NO_APP=1`) com as duas linhas "empty" e nenhum "error";
  `docs/soak-report.md` round 19; teste novo `testEmptyLotPackage` (o stub de 96 B escrito
  pelo `DbpfWriter`); build+ctest **39/39**.
