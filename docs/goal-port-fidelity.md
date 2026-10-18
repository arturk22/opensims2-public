# Goal — Portar com fidelidade (OpenSims2 · Windows x64 + núcleo compartilhado)

- **Status:** **em andamento** — aberto em 17/10/2026; roda em **loop autônomo** por itens
  verificados; log no fim. Segue até o Artur mandar parar.
- **Mandato:** Artur — "Faça um goal para gente portar com fidelidade, fique rodando em
  looping para gente fazer história, vamos pra cima." (17/10/2026)
- **Branch:** `windows-x64-ghidra-pc` (11 commits à frente de `origin/main`). Um commit por
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

- [ ] **1. Cross-check REA** (herdado do `goal-ghidra-pc.md`): retry #2 rodando (12G + 4
      analisadores desligados + deadline de 55 min). Ao terminar: comparar âncoras
      `TSCatalog` @ `0x0124AE10`, `FunctionSubsort` @ `0x012358AC`,
      ` Bin Prod ID = %d Bin Type = %s Bin Sort Idx = %d` @ `0x01262534` e arquivar a
      evidence (`rea-crosscheck.json` em `..\..\rea-snapshots\ghidra-pc\`).
- [ ] **2. Baseline Windows**: build + 39 ctest verdes; anotar skips legítimos (sem
      `OS2_DATA`) e qualquer dívida de teste visível. É o chão para o resto do loop.
- [ ] **3. RE relâmpago** (Ghidra `pcb`, consultas curtas): região dos bins `0xCDF41B`+;
      handlers `0xBDF83B`/`FUN_00bdd8f2`/`FUN_00bde67d`; papel de `FUN_0040d883("tsdata/")`
      → atualizar `re-pc-ghidra.md`.
- [ ] **4. Buy §1.6 — a sobra de 1 px** na costura do catálogo (532 no app vs 533 do
      original): medir com `--screenshot` no Windows e fechar a conta pixel a pixel.
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
  em curso: retry #2 do cross-check REA (12G + 4 analisadores off + 55 min) rodando desde
  21:32 — vira o primeiro ciclo assim que fechar.
