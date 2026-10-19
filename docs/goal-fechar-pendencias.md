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

- **5 (Buy §1.4, "6 de 53"):** `[BLOQUEADO: Artur — a captura era instalação limpa, sem
  Downloads/CC?]`; em paralelo, tentar fechar por RE (campo 94 & 1 / leitura do knob).
- **6 (resto):** pessoa 164, ActionSkipped "evento 4", testes `0x214`/`0x2e8`, NGBH u32
  `+0x34`/`+0x38`, registros `+0x60c` e a troca de fila pela vtable `0x1FC` — todos pedem RE
  do binário **Mac PPC** (indisponível nesta máquina).
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
2. **13b — fechar o modelo 0x53+** (investigação; sem commit se não houver mudança de
   código): (a) rodar `ttab_model2.py` com saída em arquivo/exit code; (b) atacar o
   (X=0, Y=8): dump cru do FD9 em 362..736, procurar entrada intermediária não detectada e
   testar leituras alternativas dos contadores; (c) explicar o 0x53 "untitled"; (d) validar
   o parse em **100%** dos 901 (ou documentar cada resíduo com bytes + hipótese); (e)
   conferir que os anúncios extraídos batem com o trio do formato antigo em amostra grande.
   *Pronto quando:* o parse fecha em todos os arquivos aceitos e o resíduo (se houver) está
   documentado — sem aceitar dado desconhecido em silêncio.
3. **13c — leitor novo no port:** implementar 0x53+ em `InteractionTable::parse` (fixo +
   cauda por X/Y; anúncios → `motives`), testes sintéticos (0x53/0x54/0x55, cauda vazia e
   multi-grupo), doc em `data-findings.md`, TEMP fora, commit.
   *Pronto quando:* decode-all do AL sem falha de versão; um objeto do AL com TTAB 0x54/0x55
   carrega as interações (`lot-run`/app); ctest 39/39.
4. **8 — N002_Lot32 / pacote vazio:** `Lot::load` distingue pacote vazio (stub de 96 B) de
   erro; contagem separada no `soak-report`; teste; commit.
5. **14 — vizinhanças/lotes do AL:** NGBH 0xCB, FAMI 0x55, SDSC 0x36, NID +20 e `lot info`
   de apartamento, cada leitor com versão medida nos arquivos do UC; teste por formato;
   `lot-run` do `E001_Lot127` avançando; commit por formato (ou um commit coeso se forem
   pequenos).
6. **7 — performance:** um sub-item por vez (índice por tile, notificações por tick, Set to
   Next, texturas), medidor antes/depois no log; 1 commit por sub-item.
7. **10 — painel do Build:** comparar com as capturas 30–34 como no item 4; corrigir;
   commit.
8. **RayTrace 0x78:** mesma convenção de slot do Look At; teste + sweep; commit.
9. **12 — 0x7D/0x7E:** RE no exe do PC (handler do aggregate + gate de tipo de objeto),
   implementar, derrubar os 2.213 `0x7E` do sweep; testes; commit.
10. **6-resto / 5:** só com Mac PPC (não há) ou com a resposta/RE do Artur — ficam por
    último, bloqueados, sem travar a fila.

## Critérios de sucesso

- [ ] Item 13 completo: 1.177 TTABs do AL lidos (0x4F..0x55) e interações visíveis no app.
- [ ] Build + ctest 39/39 ao fim de **cada** item (evidência no log).
- [ ] Item 8 fechado com o soak-report citando o caso; itens 7 e 10 com medição antes/depois.
- [ ] Item 14: `E001_Lot127` do AL carrega (ou o residual está documentado com bytes).
- [ ] RayTrace e 12 fechados com sweep/teste; 5 e 6-resto explicitamente bloqueados.
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
