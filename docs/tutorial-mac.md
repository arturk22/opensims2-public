# Tutorial ("Aprenda a Jogar"): o que o binário de Mac faz

Rodada 11, frente de RE. Fonte: o executável ppc do Bon Voyage 1.10 com os
nomes do `tools/symxfer` (`docs/symxfer.md`), decompilado sem cabeça; os
dados são os da instalação (`TSData/Res/Tutorial/EpisodeDefinitions.txt`,
vizinhança `Tutorial`, `objects.package`). "Binário" = lido no código;
"inferido" = deduzido do uso. O base 1.0 tem as mesmas strings de diálogo
("Tutorial Tasks", "Tutorial Next", "Tutorial Next Modal") e o erro 0x35,
mas não a string `EpisodeDefinitions.txt` (o sistema de episódios abaixo é
do BV; os dados que temos já são desse formato).

Resumo: o tutorial é **SimAntics + um serviço de UI** (`cTSTutorialManager`).
Os controladores (`Controller - Tutorial - Main`, grupo `0x7FA412A9`, e o
semi-global `TutorialScriptingGlobals`, grupo `0x7F63E3F2`) dirigem tudo
com três peças do motor: a primitiva **UI Effect (0x22)** (realça, trava,
esconde e "aperta" botões, e liga árvores aos cliques), o **Dialog (0x24)
tipos 0x16/0x19** (o painel de passos do tutorial) e o global 26 "Tutorial
State" + o Generic Sims Call 0 "Exit Lot" para terminar. **A primitiva
Tutorial (0x0A) não faz nada.**

## 1. A primitiva 0x0A "Tutorial"

`cTSObject::OnTryElement` devolve verdadeiro sem ler os operandos
(`TutorialParam::SwizzleParams` não converte nada); no editor
(`CatTutorialParam`) ela mostra "tutorial: " + o texto do byte 0 + 1 no
STR# 0xEE, que é a tabela **"situation action descriptions"** (`begin`,
`end`: resto das "situações" do The Sims Online). **Nenhum BHAV dos dados
usa o opcode 0x0A** (varredura de todos os BHAVs de `TSData/Res`). O erro
0x35 "Invalid action in tutorial primitive." existe na tabela mas nada o
lança. O VM pode manter 0x0A = verdadeiro.

## 2. UI Effect (0x22): o que cada modo faz (`cTSObject::TryUIEffect`, BV `0xe2b310`)

Operandos (binário; nomes do `CatUIEffect`):

| byte | campo |
|---|---|
| 0 | modo (0–10) |
| 1–2 (u16) | índice da string da **janela** no STR# 0x96 |
| 3–4 (u16) | índice da string do **controle** no STR# 0x96 |
| 5 | flags: 0x01 janela na tabela global (`0x7FD46CD0`), 0x02 na semi-global; 0x04 controle na global, 0x08 na semi-global (sem bit: privada); 0x20 aparece nos dados (não lido pelo `TryUIEffect`) |
| 6–7 (u16) | parâmetro do efeito (o editor o imprime no modo 3; o motor não lê) |
| 8–9 (u16) | árvore (modos 4 e 8) |
| 11–12 (u16), 13 | (dado, dono) do nó TNS (modo 8) |

O STR# 0x96 do semi-global do tutorial chama-se **"Button GUIDs"**: cada
entrada é o ID de uma janela ou botão em hexadecimal, com o nome no
comentário (ex. 2 `0x480e8304` "W - Main Window", 3 `0x37d97b25` "B - Live
Mode", 5 `0x77d97b28` "B - Build Mode", 63 `0x4d2b68a1` "W - NEXT Tutorial
Dialog", 64/65 `0xa9000005`/`0xa9000006` "Back"/"Next", 29 `0x00000000`
"RESET GUID"). Os índices são base 0 no `TryUIEffect` (o editor soma 1).

Execução, salvo no modo 8:

1. lê as duas strings; **string vazia → erro 0x7D**; converte com
   `strtoul(…, 0)` (aceita "0x…");
2. janela = 0 e controle = 0: só os modos 3 (tira o realce), 4 (árvore sem
   controle) e 7 valem; outros → erro 0x7D;
3. senão acha a janela pelo ID entre as janelas de topo do gerenciador de
   recursos (`RZ::RM()→0xC→0x9C(id)`) e o controle dentro dela
   (`→0x9C(id)`); não achou → **erro 0x7A**;
4. chama o `cITSTutorialManager` (`FUN_000c8280()→0x30`):

| modo | rótulo do editor | chamada | efeito (binário) |
|---|---|---|---|
| 0 | aperta o controle | `ActivateButton(c)` | manda ao pai a mensagem de clique (`0x287259F6`, comando 3) com o ID do controle: **simula um clique** |
| 1 | desliga | `SaveControlEnableState(c)`, `SetControlEnableState(c, falso)` | guarda o bit "habilitado" (flag 2 da janela) numa propriedade `0xACDB44D4` da janela (só se ainda não há uma) e desabilita |
| 2 | liga | idem com verdadeiro | habilita |
| 3 | efeito | `SetTutorialHighlight(c)` | põe a **seta** apontando o controle (§3); controle nulo tira |
| 4 | árvore de evento | `SetControlCallback(c, meu ID, árvore)` | liga a árvore ao clique do controle (§4) |
| 5 | reseta | `RestoreControlEnableState(c)` | volta o "habilitado" guardado e apaga a propriedade (falso se não havia) |
| 6 | desliga todos | — | **nada** |
| 7 | reseta árvores | `ResetControlCallbacks()` | tira todos os ouvintes postos pelo modo 4 |
| 8 | árvore em nó TNS | serviço de notificações `→0x14(valor, árvore, meu ID)` | valor = (dono byte 13, dado u16 11); liga a árvore a uma notificação |
| 9 | mostra | `SetControlVisible(c, verdadeiro)` | `Show(1)` |
| 10 | esconde | `SetControlVisible(c, falso)` | `Hide()` |

A primitiva sempre devolve verdadeiro (não espera).

## 3. A seta de realce (`SetTutorialHighlight` / `AnimateTutorialHighlight`, BV `0x216c9c` / `0x2168a0`)

- Retângulo do controle em coordenadas de tela. A seta (janela gerada
  `0x20732180` com a imagem do grupo de UI `0x499db772`) muda conforme o
  canto: controle a menos de 64 px da borda direita da tela e do topo →
  imagem `0x8cd80002`, direção (−1, +1); só da direita → `0x8cd80001`
  (−1, −1); não da direita e com topo ≥ 64 → `0x8cd80000` (+1, −1); senão
  `0x8cd80002` (+1, +1). A seta é posta no canto do controle oposto à
  direção.
- Afastamento base = min(largura, altura) / 2 + 18 px.
- A cada tick do serviço (`OnTick`): t = tempo do timer em ms (reinicia a
  cada **1300 ms**), f = 2t/1300 − 1, salto = (1 − f²) × **25** px; a seta
  vai para canto + (base + salto) × direção − metade do tamanho da seta: um
  quique parabólico de 0 a 25 px, 1,3 s por ciclo.
- A seta só fica visível se o controle e todos os pais estão visíveis.
- `SetTutorialHighlight(nulo)` para o timer e remove a seta. Só existe uma
  seta.

## 4. Árvores nos cliques (`SetControlCallback`, `DoMessage`)

- `SetControlCallback(controle, objeto, árvore)`: tira o ouvinte anterior
  do mesmo controle e põe um `cTSEdithWinEventListener` {mensagem
  `0x287259F6` (clique de botão), ID do controle, objeto, árvore} no
  win-proc do controle; a tabela fica no gerenciador (`+0x1C`, chave = o
  controle).
- No clique o ouvinte posta a mensagem `0x8D86BF62`; o `DoMessage` do
  gerenciador acha o objeto pelo ID e roda **a árvore nele, até o fim, num
  tick** (`tree sim→0x3C(comportamento do objeto, 0, árvore, objeto, tipo
  3, parâmetros zerados, 0)`; tipo 3 inferido "evento"). O botão continua
  fazendo o que faz normalmente (o ouvinte é adicional; inferido).
- A mensagem `0xCDAFDA21` faz `ResetControlCallbacks` (inferido: troca de
  lote/modo).
- Nos dados: `Event - Forward Button Press`, `Event - Back Button Press`,
  `Event - Generic Button Press (+1/−1)`, `Event - Remove Arrow` etc. do
  controlador são essas árvores (ex. `UI Effect 04 0200 0500 2A 0100 3110`:
  modo 4, janela 2 = Main Window, controle 5 = Build Mode, árvore
  `0x1031`).

## 5. O painel do tutorial: Dialog 0x16 / 0x19 (`TryDialog`, BV `0xe2bb64`)

Rótulos do editor (`CatDialogParam`): "Tutorial Tasks", "Tutorial Next",
"Tutorial Next Modal". O `TryDialog` trata os tipos **0x16** e **0x19**:

- **temp 1 = 0 → esconde o painel** (`HideProgressDisplay`) e devolve
  verdadeiro;
- senão monta (STR# 0x12D da tabela escolhida pelas flags do byte 8 como
  nos outros diálogos; índices base 1):
  - texto A = byte 6 (ou temp 3 com a flag 0x10) — no controle
    `0xA9000001` (título);
  - texto B = u16 13 (ou temp 0 com a flag 0x02) — o corpo, com quebra de
    linha (controles `0xA9000004`/`0xA900000B`);
  - imagem = a string de índice (byte 0 | byte 2 << 8) (ou temp 4 com a
    flag 0x20), um ID hexadecimal sem "0x" (ex. `2d8fac21`) de imagem do
    grupo de UI `0x499db772`, no controle `0xA9000003` (0: sem imagem);
  - chama `ShowProgressDisplay(texto A, texto B, imagem, temp 1, temp 2,
    modal = tipo 0x19)` e **devolve verdadeiro na hora: o painel não
    bloqueia** (corrige o "só bloqueiam com temp 1 ≠ 0" de
    `docs/primitives-mac.md`; não tem tempo esgotado: não é um
    `cTSObjectDialog`).
- `cTSWinProcTutorialProgressDialog::Show` (BV `0x9f2bb0`): script de UI
  `0x2CDB7CBD`; **"passo temp 1 de temp 2"**: se temp 2 > 1, `sprintf` com
  o texto do controle `0xA9000007` como formato (temp 1, temp 2); modal
  (0x19): esconde o botão Voltar `0xA9000005` e o Próximo `0xA9000006`
  recebe o texto 3 do STR# 0xCB do grupo `0x7F3A262A`. O título da janela
  usa o nome do episódio (`<EpisodeName>` se não há episódio).
  `SetProgressDisplayVisibility` (gerenciador `+0x9D`) liga/desliga o
  painel inteiro. Os botões Voltar/Próximo não fazem nada sozinhos: as
  árvores os ouvem pelo UI Effect modo 4 (`W - NEXT Tutorial Dialog`
  `0x4d2b68a1` nas "Button GUIDs").

## 6. Episódios, a lista no menu e o fim de um episódio

**Definições** (`LoadEpisodeDefinitions`, `cTSTutorialParser`): arquivo
`Tutorial/EpisodeDefinitions.txt` (texto; `Episode n`, `TextResource
<arquivo> <stringset>`, `TitleTextIndex`, `DescriptionTextIndex`,
`ThumbnailResource <grupo> <instância>`, `NeighborhoodPrefix`, `LotName`,
`RewardObjectGUID`, `EpisodeGroup` 1 "Learn Basic Features" / 2 "Learn
Advanced Strategies", `Prerequisite`; **a ordem do arquivo é a ordem do
jogo**). Os dados têm 5: lotes `*14`, `*18`, `*1` (grupo 1), `*19`, `*4`
(grupo 2), todos na vizinhança "Tutorial", textos do `Text/Tutorial.package`.

**Estado** (`RefreshEpisodeInfo`, BV `0x21831c`, uma vez por sessão):
para cada episódio, estado 0; se o prefixo não é "CAS", acha o pacote do
lote `<prefixo>_<lote>` e lê do seu SIMI o **global 26 "Tutorial State"**
(o valor vira o estado); prefixo "CAS": estado 2 se a vizinhança Tutorial
tem exatamente um personagem válido (ID 1–0x7FD9, sem "Sims" no nome)
(`CASEpisodeFinished`).
Depois: **um episódio fica 4 (trancado) se algum anterior não está em 2
(concluído)** — o primeiro nunca tranca; `Prerequisite` não é lido. Estados
vistos: 0 disponível, 2 concluído, 4 trancado.

**Listas**: `cTSWinProcNeighborhoodLearn::LoadTutorialsAndFillListBoxes`
põe os títulos em duas listas (grupo 1 e grupo 2). O diálogo de episódios
(`cTSWinProcTutorialDialog`, aberto pelo menu da vizinhança
`ShowTutorialDialog` ou pelo diálogo de boas-vindas
`cTSWinProcSims2IntroDialog`, botões `0xA1`/`0xA2`/`0xA3` → grupo 0/1/2,
preferência "ShowTutorialWelcome") mostra miniatura, título e descrição; o
botão `0xA007` (jogar) fica **desabilitado se o estado é 2 ou 4**.

**Jogar** (`PlayEpisode`, BV `0x217740`): exige prefixo e lote; guarda o
episódio atual e posta a mensagem `0x2BEA2063` com `"<prefixo>;<lote>"` e
−2 se `AutoMoveInFamily` (−1 senão): o controle de estado carrega a
vizinhança e o lote. Ao entrar num lote da vizinhança Tutorial,
`cTSGameStateController::ResetTutorialEpisode` **copia o pacote do lote
da instalação por cima do do usuário** (cada episódio começa limpo);
`ResetTutorialNeighborhood` copia a vizinhança inteira da instalação
(lotes e personagens) e a registra (`AddSpecialTutorialNeighborhood`).

**Fim**: `Simulation - Exit Tutorial` (`0x202A` do semi-global) faz global
26 := parâmetro 0 e o **Generic Sims Call 0 "Exit Lot"**: temp 0 = global
43 (vizinhança anterior), temp 1 ≠ 0 despeja a família, temp 2 ≠ 0 salva o
lote (é assim que o global 26 chega ao SIMI que o `RefreshEpisodeInfo`
lê), temp 3 ≠ 0 reseta o episódio (rótulos do STR# 0xDC; o caso 0 do
`TryGenericSimCall` posta a mensagem `0xCBEA26D7`).

## 7. O que falta (aberto)

- O formato exato de cada palavra-chave do parser (só pelos comentários do
  arquivo) e o `RewardObjectGUID` (não visto em uso).
- O significado do tipo 3 do tree sim nas árvores de clique e se o clique
  original segue (inferido que sim).
- Os modos de UI Effect nos dados que dependem de janelas que ainda não
  existem no nosso app (IDs nas "Button GUIDs").
