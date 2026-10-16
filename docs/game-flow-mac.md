# Fluxo do jogo: do menu principal ao lote e de volta

Convenções como em `docs/neighborhood-mac.md`: "dados" = lido nos arquivos
do jogo instalado (base 2004, `data/game`), "binário" = lido no código do
Mac, "inferido" = dedução nossa.

## 1. Pasta do jogador (`src/app/user_data.*`)

O jogo salva na pasta do usuário ("EA Games/The Sims 2/Neighborhoods" no
PC), nunca nos dados do CD. Aqui:

```
~/Library/Application Support/opensims2/      (--user-dir <pasta>, $OS2_USER_DIR)
  Neighborhoods/N001/ N002/ N003/ Tutorial/   cópias dos bairros jogados
  Options.txt                                  opções (chave = valor)
```

Um bairro é copiado na primeira vez que é aberto, um por vez: com
`clonefile` (APFS), a cópia não ocupa disco até um arquivo mudar (os 4
bairros somam 243 MB lógicos: N001 72, N002 65, N003 93, Tutorial 13); fora
do APFS, cópia comum. Copia para `<nome>.copying` e renomeia. Dali em diante
o bairro e os lotes carregam da cópia e o save escreve nela; um caminho
dentro de `data/game` é recusado (`UserData::inGameData`, `saveGame`).

## 2. Cenas (`src/app/game_flow.*`, `main_menu.*`, `main.mm`, `scene_setup.mm`, `scene_lot.mm`, `flow_host.mm`)

Uma cena é escolhida pelos argumentos, então as mesmas flags servem na
partida e numa troca:

| cena | flags |
|---|---|
| menu principal | `--menu` (ou nenhuma flag de cena nem de conteúdo) |
| bairro | `--hood <N001\|pasta>` |
| lote | `--hood <N001\|pasta> --hood-lot <id> --vm --ui live` |

Trocar de cena (`requestScene`) descarrega a atual (`unloadScene`: VM, UI,
áudio, atores, objetos, luz) e cria um `Renderer` novo, depois carrega a
próxima — no mesmo processo (antes o app se reiniciava com `execv`). Com
janela a troca roda no próximo giro do run loop (`dispatch_async`), fora do
evento que a pediu. A carga roda numa thread de fundo atrás da tela de carregamento
Reia, com a janela respondendo (`docs/loading-mac.md`). Os caminhos do jogo (`cTSGameStateController`,
`docs/neighborhood-mac.md` §6):

- partida (com janela): os vídeos de abertura (`app/movie_host.hpp`,
  `introMovies()`) antes do primeiro menu, que carrega por trás;
- menu → clique numa miniatura → "NH Preview" → Usar → bairro;
  "Aprenda a Jogar" → "Jogar Tutorial" → bairro `Tutorial`; Sair fecha o app;
- bairro → clique num lote → "Lot Info Dialog" → Entrar no Lote → lote;
  "Menu Principal" (`0x4BE6ED7E` filho 2) → menu com Fechar (volta ao
  bairro) e Sair;
- lote → Opções → Vizinhança (ou Esc) → "Salvar?" Sim (salva) / Não →
  bairro, Cancelar fica; Opções → Sair → "Sair?" Salvar e Sair / Sair sem
  Salvar / Não Sair.

O menu principal (`0x49001017`) é o escolhedor de vizinhança; o "NH
Preview" (`0x49001021`) mostra nome e descrição do CTSS 1 do pacote do
bairro e a foto `<P>_Neighborhood.png`. Inferido (o window proc
`cTSWinProcNeighborhoodMainMenu` não foi decodificado): três miniaturas por
página na faixa `0x1006`, foto 4:3 sobre o nome; os diálogos na origem da
raiz do menu; o fundo `0x0DA36C7D` (arte de 1024×768 com 10 px de sangria)
cobrindo a tela.

## 3. Salvar (`src/app/game_save.*`)

O que `cTSSaveLotController` faz (`docs/save-mac.md` §6), pelos gravadores
existentes: `captureLot` + `captureLotRecords` gravam o lote no seu próprio
pacote; `saveNeighborhood` grava FAMI/SDSC/SREL/NGBH no pacote do bairro.
Cada arquivo vai para `<nome>.saving` e é renomeado por cima. Os textos da
pergunta são do jogo (UIText STR# 0x99: 3 "Salvar", 4 a pergunta de sair
para o bairro, 5/6 a de sair do jogo, 24–26 os botões dela; Sim/Não/Cancelar
da 0x8E 2/3/1, inferido); a caixa é a dos diálogos da VM (menu de pie),
não a do jogo.

**Pasta da sessão** (rodada 16): os arquivos de personagem escritos em
jogo (Sim que envelheceu, bebê, NPC em `NPCs/`) ficam em
`$TMPDIR/opensims2-characters-<pid>` até o salvar copiá-los para o bairro.
A pasta é apagada na saída do app (`atexit`) e, ao abrir, o app apaga as de
sessões que não estão mais rodando (processo morto ou travado;
`removeStaleSessionCharacters`): eram ~940 pastas esquecidas no `$TMPDIR`.

O laço inteiro sem tela (N001_Lot11, um dia de jogo, salvar ao voltar ao
bairro e entrar de novo):

```
opensims2 --user-dir /tmp/u --no-audio --flow-shots --hood N001 \
  --hood-lot-click 11 --click 595,592 --wait 43200 --click 139,690 \
  --click 236,628 --click 528,398 --click 736,562 --click 595,592 \
  --wait 30 --screenshot out.png
```

O salvar grava 6 NPCs feitos no dia (motorista, jornaleiro, carteiro, dois
coelhos do apoio social) com os arquivos; o lote volta com 5 Sims (um
coelho estava no lote). `docs/screenshots/r16-save-loop-reload.jpg`.

## 4. Opções (`src/app/live_ui.*`, `options_panel.*`)

Botão Opções do puck (`0x67D97B2F`): o painel `0x49060001` é o do painel
vivo (`LiveUi`, preso ao puck pela âncora `4bdeffaa:bl`). Fazem algo:
Vizinhança (só num lote vindo de um bairro; pergunta "Salvar?"), Sair
("Sair?"), Salvar, e os botões de "Opções de Áudio" `0x49060F01`
(sliders `0xB31` vozes, `0xB01` efeitos, `0xB41` música, `0xB21` ambiente
→ grupos do mixer; caixas Mudo `0x10`–`0x13`) e "Opções de Jogo"
`0x49060F02` (`radio:edgeScrolling`, `0xA1`), que abrem as janelas de
`options_panel.*`. Inferido: a janela abre com o canto inferior esquerdo no
do painel (a aba da arte, da altura do painel, fica sob ele; o painel
continua desenhado por cima) e fecha com o mesmo botão ou quando o painel
fecha.

Rodada 13 (passada 7 da QA): o painel do lote também abre Gráficos `0xA241`,
Câmera `0xA245` e Créditos `0xA246` (antes só áudio e jogo). Sombras, Reflexos
e Iluminação valem na hora: o mapa de luz do terreno do bairro é refeito ao
mudar a opção (`HoodView::refreshLightMap`) e os planos de sombra dos objetos
(`ObjectModel::Part::shadowPlane`) são pulados ao desenhar enquanto `guob` está
desligado (reflexo e `specHighlights` já eram lidos a cada quadro). Esc no lote:
fecha o menu de pie aberto (o Esc que o fechou não faz mais nada), depois a
janela de opções aberta, e só então faz a ação do jogo (cancelar ferramenta,
"Salvar?" ao voltar ao bairro). Sem tela: `--key escape`
(`docs/screenshots/r13-options-shadows-hood.jpg`,
`docs/screenshots/r13-options-graphics-lot.jpg`).

Rodada 14 (passada 8 da QA): a janela de opções abre com o canto inferior
esquerdo no canto superior esquerdo do painel de opções (antes no inferior
esquerdo: a janela cobria o painel e metade do "Aplicar Mudanças"; inferido, o
script não traz posição), no lote e no bairro, e o corpo dela (não a coluna
esquerda de 156 px, onde só a aba é arte, atrás do puck) consome todo clique
que cobre antes do puck, dos painéis e do mundo (o clique em "Aplicar Mudanças"
enfileirava um ícone). `opensims2 --help` / `-h` imprime o uso e sai.
`docs/screenshots/r14-options-apply-lot.jpg`, `r14-options-graphics-hood.jpg`.

## 5. Sem tela

`--click x,y`, `--right-click x,y` e `--wait <ticks>` formam um roteiro que
atravessa as cenas: cada cena consome os passos até um deles trocar de cena,
e a próxima recebe o resto. `--flow-shots` grava cada cena logo antes de
sair (`<shot>-1.png`, `-2`, ...); a última vai para o `--screenshot`. O menu
imprime o retângulo de cada miniatura e o bairro a posição de cada lote na
tela de 1280×720. Exemplo (menu → Belavista → lote 11 → 600 ticks → Opções
→ Vizinhança → Sim → bairro → lote 11 de novo):

```
opensims2 --user-dir /tmp/u --no-audio --flow-shots \
  --click 449,347 --click 598,592 --click 712,512 --click 595,592 \
  --wait 600 --click 139,690 --click 236,628 --click 528,398 \
  --click 712,512 --click 595,592 --screenshot out.png
```

O lote volta às 17:22 com 628 objetos (abriu às 17:02 com 627).

Rodada 20 (`r20-wallcarry`): cada troca de cena cria um renderizador novo, então
nada guardado entre cenas pode ter ID de modelo ou de instância dele. As peças de
cerca guardavam os do lote anterior e no segundo lote tomavam 336 instâncias de
objetos do lote novo (Sims, grades e muretas sumiam; o relógio de pêndulo do
`N002_Lot3` virava um bloco de chaminé no lugar de uma grade). Os caches por
cena ficam em globais que `unloadScene` limpa; teste `scene_switch`
(`N002_Lot10` → bairro → `N002_Lot3`, `docs/screenshots/r20-wallcarry-*.jpg`).

Rodada 22 (`r22-crash`): os efeitos do lote (`gEffects`) também são da cena
(desenham pelo gancho do renderizador) e saem no `unloadScene`; uma janela
desenhada de cópia local (o popup do painel vivo, a coluna da família) sai dos
retângulos antes da cópia sumir; o app sobe o limite de arquivos abertos (256
quando aberto pelo Finder) e a leitura paralela das partes de Sim relança erros
no chamador. Teste `scene_cycle` (`N002_Lot10` → bairro → `N002_Lot3` → bairro →
`N002_Lot10`, popup de humor aberto, `ulimit -S -n 256`), que deve sair com 0
também num build com AddressSanitizer.

Rodada 23 (`r23-scenestate`): todo o estado
da cena fica num objeto só, `SceneState gScene` (`app/app_state.hpp`/`.cpp`); os
globais de antes (`gBuild`, `gLotSims`, `gEffects`...) são referências para
os membros dele. `resetSceneState()` troca o objeto por um novo (atribuição
por movimento: os membros são zerados **na ordem em que estão declarados**,
que é a ordem de desmontagem: modos e vistas antes da VM, efeitos antes do
renderizador da cena seguinte). Ela roda num lugar só: no `unloadScene` a
cada troca (lote → bairro, bairro → bairro, lote → lote) e num `atexit` ao
sair (antes dos estáticos, com o índice e o áudio do jogo ainda vivos).
Entraram também os globais que o `unloadScene` esquecia (`gWallHung`,
`gCarriers`, `gLotCharactersDir`, `gStaticShadowLight`, o relógio da
cinemática...) e os caches `static` de função que eram por cena: as edições
do espelho (por ID de Sim), os guarda-roupas preenchidos (por número de
família), os arquivos de personagem do bairro, os relatórios de depuração por
objeto/Sim e a última resposta do cursor ("tem interações?"). Os caches
`static` que ficam são do jogo (por GUID, nome de recurso ou arquivo de
`TSData`) e dizem isso num comentário. Os detectores de depuração (pulos,
deslizes, assentos) somam a sessão inteira mas esquecem os Sims a cada cena
(`gSceneGeneration`). Na VM, o "uma vez por tick" do
`flushMoveOutOfWayDestinations` saiu de um mapa por endereço de VM para um
campo da VM. Global novo de cena vai no `SceneState`.
`OS2_DEBUG_SCENE_STATE=1` imprime os tamanhos antes e depois de cada
descarga; `OS2_PROBE_SCENE_CACHES=1` (ajuda de teste) esvazia o guarda-roupa
da família do primeiro Sim e abre guarda-roupa e espelho dele. O
`scene_cycle` usa os dois: depois de cada descarga tudo vazio, e as três
entradas em lote (incluindo a segunda no `N002_Lot10`) começam com 0 edições
de espelho e 0 guarda-roupas preenchidos (antes, na segunda visita, o
guarda-roupa da família não era preenchido).

Rodada 24 (`r24-appcaches`): os caches do
jogo que ficaram `static` de função (partes de Sim, catálogo + face +
miniaturas do CAS, prévias do espelho, miniaturas de objetos dos diálogos,
biblioteca de vontades, ícones de memória, malhas e máscaras de pick)
estão num objeto só, `GameDataCaches gGameData` (`app/game_data_caches.hpp`),
ao lado do `gScene`. Ele é construído antes do `main` (não é `static` de
função), então o runtime só o destrói depois do handler de saída, e
`releaseAppState()` o esvazia na ordem do `clear()` logo depois de
`resetSceneState()`: com janela no `applicationWillTerminate:` (antes de o
`exit()` destruir qualquer estático), sem tela no `atexit` que o `main`
registra primeiro. Antes, um `static` de função criado durante o jogo era
destruído **antes** desse `atexit`, ou seja, antes da desmontagem da cena.
Sair durante uma carga continua terminando o processo na hora
(`applicationShouldTerminate`). Ninguém usa esses caches de duas threads ao
mesmo tempo: só a thread dona da cena (a de carga enquanto carrega, a
principal no resto; o `BackgroundJob` cria e junta a thread); as leituras
paralelas de dentro da carga só enchem vetores locais. A cada troca de cena
`trim()` solta as imagens decodificadas que passam de 64 MB (nada aponta
para elas depois da cena; `OS2_PICTURE_CAP_MB` muda o teto): as miniaturas
do CAS têm 256 KB cada, ~13 MB por guarda-roupa aberto, e com
`OS2_PROBE_APP_CACHES=1` (todos os guarda-roupas, grupos do espelho e abas
de memória dos Sims de cada lote) chegaram a 227 MB nas 5 famílias de N002.
`OS2_DEBUG_SCENE_STATE=1` imprime também os caches (entradas e KB) e a
memória residente depois de cada descarga, e o que foi liberado ao sair. O
`scene_cycle` (com `OS2_PICTURE_CAP_MB=120`) confere que as imagens
atravessam uma troca abaixo do teto, somem acima dele, e que ao sair os
caches estavam cheios e ficaram vazios; limpo também num build ASan, como
o `scene_switch` e as saídas do bairro, do CAS e do menu.

## 6. Jogar o bairro (rodada 9)

O puck do bairro abre Famílias e Lotes & Casas (`app/hood_bins.*`,
`docs/neighborhood-mac.md` §6.1/§10). Mudar uma família e confirmar entra no
lote (a mesma troca de cena de "Entrar no Lote"); "Criar Nova Família" chama
`HoodMode::onCreateFamily(pasta do bairro)` e o CAS devolve com
`HoodMode::familyCreated(id)`. Sem tela: `--hood-panel families|lots`,
`--hood-family <id>`, `--hood-tool evict|bulldoze`, `--hood-lot-click <id>`
(o bairro imprime "dialog Accept at (x, y)" para o `--click` seguinte).
Exemplo (Subúrbio para o lote 24, entra, 300 ticks, Opções → Salvar):

```
opensims2 --user-dir /tmp/u --no-audio --hood N001 --hood-panel families \
  --hood-family 8 --hood-lot-click 24 --click 605,423 --wait 300 \
  --click 139,690 --wait 2 --click 208,678 --wait 2 --screenshot out.png
```

Lotes (rodada 10, `docs/neighborhood-mac.md` §12): `--hood-place
empty:<0-9>|bin:<n>|move:<lote>` escolhe a ferramenta, `--hood-place-at x,y`
mostra a pegada (verde/vermelha) com o canto no tile (x, y) e
`--hood-place-click x,y` põe o lote; a vista é refeita por uma troca de cena
(`HoodMode::onReload`). `--hood-tool move|tobin` dão as ferramentas de mover e
de mandar para a caixa.

## 7. Carregar um lote no binário (rodada 12, Mac BV)

### 7.1 Os objetos globais ("is global sim object") e a cortina

**Quem cria o "Controller - Lot Load - Curtain Pull-Down" (`0xED7580EF`):
o motor, ao ler os objetos de qualquer lote, por um campo do OBJD.** Nenhuma
árvore o cria (as 9 BHAVs que têm a constante só o procuram com "Set to
Next" por GUID, opcode `0x1F` flags `0x84 0x0A`: "CT - Lot Loaded",
"Curtain - Hold/Ready/Up?", "Lot - Load Complete?", os Mains do
tutorial e do Game Scripting, "Main - Display TNS Messages", "Event - Pause
Game") e o código não tem a constante (nem literal, nem `lis`/`addi`), mas
o OBJD dele tem o campo 42 = 1.

- Campos do OBJD (rótulos do STR# 0xCC do grupo `0x7FE59FD0`; campo *i* no
  offset `0x40 + 2i`): **42 "is global sim object"**, **26 "Ignore
  GlobalSim field in CAS Lot"**, 9 "type". Na memória
  (`cEdithObjectDefinition`, BV `0x58d0c0` `GetFieldAddress`) o campo *t*
  fica em `this + 4 + 2t` e `GetFieldByTag(t)` usa o mesmo índice.
- `cEdithObjectModule::UpdateSimObjects(mask)` (BV `0x52f040`, vtable `0x20`
  do módulo de objetos):
  1. junta os objetos existentes cujo OBJD tem `campo42 & mask`;
  2. percorre todos os seletores (OBJDs) do `cTSObjectFolder`; para cada um
     com **tipo (campo 9) = 7** e `campo42 & mask`:
     - "não deve existir" = estamos num lote de CAS (um booleano global,
       inferido como "lote do CAS") **e** campo 26 ≠ 0;
     - mantém a **primeira** instância existente desse seletor e mata as
       outras (`KillObject`, vtable `0x44`); se "não deve existir", mata
       todas;
     - se não sobrou nenhuma e deve existir: **cria uma fora do mundo**,
       `MakeNewOutOfWorldObject(seletor, false, 0, false)` (vtable `0x40`;
       roda o Init e o Main como qualquer objeto criado).
- Quem chama: `cEdithObjectModule::SplitLoad` (BV `0x52bc4c`, a leitura
  dos objetos do lote em passos), **passo 6: `UpdateSimObjects(1)`**, depois
  de ler e retomar os objetos (passos 1–5) e antes do passo 7 (o `ObjM`
  `0x4F626A4D`). `cTSLoadLotController::LoadLot` chama o `SplitLoad` nos
  passos 8 (lote salvo) e 0x10 (lote novo, depois do modelo
  `LotTemplate%dx%d`/`CommunityLotTemplate`/`BeachLotTemplate`/
  `BeachCommunityLotTemplate`/`DormTemplate`/`FraternityTemplate`): **os
  globais entram em toda carga**, de lote salvo ou de modelo. O valor do
  campo 42 é 1 em todos, então todos os 125 OBJDs com o campo ligado e tipo
  7 entram (a máscara 1 é a única usada no base).
- Os OBJDs com campo 42 = 1 (`objects.package`, tipo 7 salvo indicação):
  todos os "Controller - …" que nenhuma árvore cria (Game Scripting, Light,
  Locomotion, Maintenance, Weather, Move In Cinematic, Emit - Timing,
  Short Term Memory Effects, Aspiration Score Decay, Relationship Decay,
  NPC Social Worker, PlumbBob, Tutorial - Main, Tutorial - Death Checker,
  **Lot Load - Curtain Pull-Down**), mais os que também têm árvores
  (Scenarios, Lot Transitions, Inheritance, Disease, NPC Pool, Chat, Age,
  Motive/NPC Handler, CLP - Community Lot - Populator, NPC, Visitor),
  Wants - Controller e Wants - Check Trees, Fitness - Controller, Zits
  Controller, Pool - Controller, Global Sim - Temp Array, Visit Generator,
  Job Finder, os 31 "JobData - …", os 10 "Personality - …", os 5 canais de
  TV, os menus de comida ("Food - … - Menu", 26), os serviços e chamadas de
  telefone (Phone Service/Emergency/Call/Delivery/Transport/Party/Work,
  ~25), Accessory - Glass - Water - MENU, e alguns de **outro tipo, que o
  `UpdateSimObjects` ignora** (tipo 4: Motive - Helper - Bladder/Energy/
  Hunger/Hygiene, Overhead Meter, Routing - Idle, _TEST FAMILY INVENTORY;
  tipo 11: Scheduler 2; tipo 0: os objetos de revista e de CD das
  prateleiras). Os modelos de lote (`LotTemplates/*.package`) têm só os 8
  objetos do lote vazio (portais, lixeira, caixa de correio); a tabela
  `objt` deles lista os tipos, inclusive a cortina, mas sem instância.
- No opensims2 isso **não existe ainda**: o lote só tem os controladores
  que foram gravados nele. Os lotes salvos confirmam o mecanismo: em
  `N001_Lot11` estão 119 dos 125 globais tipo 7; faltam só Tutorial - Main,
  Tutorial - Death Checker, Game Scripting, CLP - Community Lot - Populator,
  PlumbBob e a cortina (inferido: estes se apagam depois de rodar, ou o
  jogo não os grava; a cortina e o Game Scripting reaparecem na carga
  seguinte pelo `UpdateSimObjects`). Fazer: ao carregar um lote (salvo ou
  modelo), depois de ler os objetos, criar fora do mundo uma instância de
  cada OBJD tipo 7 com campo 42 ≠ 0 que ainda não tem nenhuma, matar as
  duplicatas e, num lote de CAS, matar as que têm o campo 26.

O que a cortina faz (grupo `0x7FE50A0E`): Init põe o atributo 0 = 1; Main
espera (`0x0118` = Sleep) e reage ao "reset lot" (`1003` põe o atributo
0x1A), "CT - Curtain Hold/Ready" (atributo 0 = 3/4), "Ready to Raise
Curtain?", "Add Sims To UI", "Move In", "Cinematic - Move In" (`moveIn`),
"Memory - Move In" e "Generate Family". Nunca se apaga: é recriada (ou
mantida) a cada carga.

### 7.2 Os passos do `cTSLoadLotController::LoadLot` (BV `0x8804ec`)

Uma máquina de estados (`this + 0x24`), um passo por chamada, com o
simulador (`+0x7c`, interface `cITSSimulator`: `SetGlobal` vtable `0x40`,
`Notify` `0x34`, `Pause/Resume` `0x60/0x64`, `SetCurrentTime` `0xa4`,
`SetTimeOfDay` `0xc4`, "período do dia pela hora" `0xc8`) e o módulo de
objetos (`+0x80`, primário: `SplitLoad` `0x1c`, `UpdateSimObjects` `0x20`,
`InitForNewLot` `0x24`, `PostLoad` `0x28`, `KillObject` `0x44`,
`FlushKillQueue` `0x54`, `BroadcastMessage(tipo)` `0xd8`). Resumo:

| passo | o que faz |
|---|---|
| 0 | mudando (`+0x34`): família já no lote? → `+0x35`; senão liga família e lote |
| 1 | mensagem `0xB2`; `TS::SetCheckpoint(lote, bairro)` |
| 2 | abre o pacote do lote; lote salvo → 3, novo → 0xB |
| 3–7 | lote salvo: lê o mundo, `SetCommonSimulatorGlobals` (globais 20 Game Edition, 10 Current House = lote, 41 Lot Zoning = tipo do lote, 40 Current Neighborhood ID e 54 do bairro, 43 Last Neighborhood ID), terreno, `SetGlobal(9, 0)`, mensagem `0xBB` |
| 8 | `SplitLoad` até terminar (inclui `UpdateSimObjects(1)`) |
| 9–10 | recursos `0x45585069` e materiais (TXMT) do lote |
| 0xB–0x11 | lote novo: `InitForNewLot`, globais, abre o modelo `"%s%dx%d"` pelo tamanho/tipo (girado conforme a orientação), terreno, `SetGlobal(9, 0)`, mensagem `0xBD`, `SplitLoad` do modelo, posiciona os objetos do modelo girados |
| 0x13–0x16 | mensagens `0xAD` e `0x94`; `Notify(0)`; **`SetTimeOfDay(período(hora))`** |
| 0x17–0x1a | câmera/vista; `SetGlobal(44 Lot Entry Method, …)`; mensagem `0xB3`; lista as pessoas e as famílias |
| 0x1b | mudando para um lote que não era da família: **fundos −= preço do lote**; `SetGlobal(9, família)` (§ `docs/neighborhood-mac.md`) |
| 0x1c | `CleanupThingsRelatedToEvictedFamily` |
| 0x1e | `PostLoad(0xCE)` |
| 0x1d | se o lote tinha o recurso do passo 9: `BroadcastMessage(0, …)` e o mesmo para os inventários pessoais |
| 0x1f–0x20 | primeiro dia da família (`+0x78` vtable `0x44`, o `SetupFamilyFirstDay` abaixo) quando é mudança ou o lote tem a flag 4 |
| 0x21 | mata as pessoas que não pertencem mais ao lote; `FlushKillQueue` |
| 0x22 | mensagem `0x9A`; câmera inicial; mensagem `0xCBCCDECE`; `dormSpecificToolsDisabled` |

**Não há simulação adiantada**: nenhum passo roda ticks; o relógio é o
dos globais salvos do lote (`cTSSimulator::Read`, BV `0x499c20`: globais 0
hora, 1 dia, 4 período, 5 minuto, 7 mês, 8 ano; versões antigas caem em
5:55) e o passo 0x16 só recalcula o período do dia (6h → 3, 7–17h → 0,
18h → 1, senão 2; `0x4984d4`), que manda a mensagem `0x8D` (luz).

### 7.3 A hora da mudança: 8:00

`cTSNeighborhood::SetupFamilyFirstDay(família, simulador, objetos)` (BV
`0x4db0c0`): família vtable `0x120(0)`; **`SetCurrentTime(hora = "new family
start hour", minuto = 0, resto −1 = sem mudar)`**; `ClearHistory` (as
despesas); `Resume(1)`; `BroadcastMessage("reset lot", 0, 0, 0, 0)`.
"new family start hour" vem das constantes do bairro
(`cTSNeighborhoodConstants::UpdateConstants`, BV `0x4d6250`,
`cTSFloatConstants` com padrão 7,0) e o recurso **FCNS `0x46434E53`, grupo
`0x7FD46CD0`, instância 4** do `objects.package` diz **8,0** (o mesmo
recurso tem "money for new family" = 20000, "architecture value
multiplier" = 0,7, "perfect family friend count" = 15, "yard score
multiplier" ≈ 0,33, ...). Então a família que se muda começa às **8:00**,
no dia/mês/ano que o lote já tinha, e o "reset lot" que o opensims2 já
manda (rodada 11) vem logo depois de pôr o relógio.

### 7.4 O relógio (`cTSSimulator::SimulateOneTick`, BV `0x49a814`)

Os globais do simulador são shorts em `this + 0x24 + 2i` (rótulos do STR#
0x81 do grupo `0x7FE59FD0`). Com o relógio ligado (flag `+0xde` bit 2), a
cada tick: global 6 "Second" += 2; passando de 59 volta (−60) e global 5
"Minute" +1 (mensagem `0x88`); minuto > 59 → global 0 "Hour" +1 (mensagem
`0xD08962D0`); hora 24 → global 28 "Days Running" +1, global 1 "Day of
Month" +1, global 34 "Day Of Week" = dias % 7 (mensagem `0x89`); dia > 30
→ dia 1 e global 7 "Month" +1 (> 12 → 1 e global 8 "Year" +1); global 29
"Maximum Day Number" fica em 30. **Confirma o inferido: 30 ticks por
minuto, meses de 30 dias, 12 meses.** O período do dia (global 4) é
recalculado pela hora (`SetTimeOfDay`, §7.2).

### 7.5 Lote sem Sims: abre em Comprar, sem Viver (rodada 22, Ghidra headless)

**`cTSMainToolbar::ApplySavedUIState`** (BV `0x1ac250`, chamado depois da carga
do lote, só nos estados do jogo `0x0BEA28C2` / `0x2C1805B0`) escolhe o modo
inicial: com Sims selecionáveis (a verificação de `game→0x3c→0x24`) chama
`SetMode(2 = vivo)`; **sem eles** chama `SetMode(3 = comprar)`, ou
`SetMode(4 = construir)` num caso secundário (o estado `→0x24→0x7c == 2` e uma
propriedade booleana do estado, nome em `DAT_027a89f0`; **não identificado**:
o app usa sempre Comprar). Depois lê `startPaused`/`startUnpaused` do
estado e chama `SetSimulatorPaused`; o relógio não tem outro tratamento (o
`SimulateOneTick` de §7.4 não olha se há Sims).

**O botão Viver**: no `cTSWinProcUCP`, `this[0x80]` = "o lote tem Sim
selecionável" (`onSelectedPersonChanged`, BV `0x1a131c`: `!lote→0x94()`; sem
Sims também zera `this[0x84]`, o tipo de painel). O mesmo método chama
`SetEnabled(0x37D97B25, this[0x80])`: **o botão Viver fica desabilitado
(desenhado apagado, não escondido)**, e o `HandleLiveModeButton` (BV `0x1a0654`,
o clique) **não faz nada** sem `this[0x80]`. O `onModeChanged` só acende o Viver
no modo 2/11 com `this[0x80]`. Comprar e Construir seguem normais; a janela de
valor do lote sem Sims (`cTSWinProcSimlessLotValueUI`, `SetInBuyMode/BuildMode/
SetUCPVisible`) aparece nesses modos. Lote comunitário sem família: a mesma
regra (não há exceção por tipo no `ApplySavedUIState`).

No app: `os2::lotStartMode` (`app/game_flow.*`) dá `"buy"` para o lote do bairro
sem família (`hood::Lot::families` vazio) e `--mode` ganha; o `app_args.mm` (`startMode`) usa isso
no lugar de `--mode` ao entrar (Enter do bairro ou `--hood-lot`) e
`LiveUi::setLiveEnabled(false)` apaga o botão Viver (estado 0) e ignora o clique.
Mudar uma família para o lote antes de entrar o deixa ocupado (abre em Viver).
**Inferido**: o nome de `game→0x3c→0x24` (lido como "há Sim selecionável").

**O caso Construir é das expansões (rodada n27, Ghidra MCP).** No BV o
`ApplySavedUIState` pega `controle de estado→0x24→0x7c` (o tipo do lote) e,
só quando ele é **2** (dormitório da Universidade), lê a propriedade
booleana `"dormSpecificToolsDisabled"`: ligada → `SetMode(4)`, desligada →
`SetMode(3)`. O mesmo método no **Mac base 1.0** (`FUN_00c9fe50`, achado
pela string `startPaused`, a única que o referencia) não tem esse ramo:
`SetMode(2)` com Sims selecionáveis, senão `SetMode(3)`, e nada mais. O app
abrir sempre em Comprar é o jogo base. `startPaused`/`startUnpaused` só são
lidos com `testingCheatsEnabled` ligado (globais vtable `0x7c` no BV, `0x70`
no base); fora disso vale o estado salvo do §7.6.

### 7.6 O estado da UI salvo no lote (rodada n27, Ghidra MCP + headless)

`ApplySavedUIState` aplica o que o `cTSMainToolbar::Read` (BV `0x1aadf0`)
leu do lote. O `cTSMainToolbar::Write` (BV `0x1aacf8`) grava quatro u32:
versão (3 no BV), **pausado** (`this+0x7c`), **andar** (`GetCurrentLevel`,
vtable `0x90`, guardado em `+0x5c`) e **modo de parede** (`GetWallMode`,
`0x98`, em `+0x60`). O `Read` aceita versão 0 (pausado = 1, mais um u32
descartado), e a versão 2 tem um quinto u32 que ele lê e descarta. No
pacote do lote é o recurso **`0x8B0C79D6`, instância 2** (cabeçalho de 64
bytes com o nome, depois os u32): nos 68 lotes salvos do jogo é versão 2
em 67 (pausado 0 em 27 e 1 em 40; andar 0, 1, 2 ou 3; parede 1 ou 2, uma
vez 3) e versão 1 em um (pausado 1, andar 2, parede 3).

Depois de escolher o modo, `ApplySavedUIState`:

1. `SetSimulatorPaused(pausado, false)` (vtable `0x80`; `0xa0` no base);
2. `SetWallMode(parede)` (`0x9c`; `0xc0` no base);
3. prende o andar salvo entre o menor e o maior andar do mundo
   (`WorldDB` vtables `0x140`/`0x144`; no base `0x110`/`0x114` do serviço
   `0x4a037ee3`), chama `SetCurrentLevel(menor)` e então
   `SetCurrentLevel(salvo)` (`0x94`; `0xb8` no base);
4. liga `this+0x2c` (no base `+0x2c` também).

O andar do jogo começa em 1 no chão (o `FUN_008b6374` tira 1 do andar da
posição); o do app em 0. **Antes desta rodada** o app abria o lote no andar
do primeiro Sim, com as paredes cortadas e a pausa do global 17 do SIMI.

O pausado salvo bate com o global 17 em 65 dos 68 lotes; os três que não
batem são os lotes 14, 18 e 19 do Tutorial (global 17 = 1, barra = 0): o
"o tutorial começa rodando" é o estado salvo do jogo nesses três. Os lotes 1
e 4 do Tutorial têm a barra pausada e **abrem pausados** (rodada n45): o
`PlayEpisode` (BV `0x217740`) entra no lote pela mesma mensagem `0x2bea2063`
do bairro e o `ApplySavedUIState` não tem ramo de tutorial
(`docs/primitives-mac.md` "Dialog", rodada n45); pausado, o jogo só roda os
objetos com o `SimFlag` 2 (no app, os controladores do tutorial, inferido).
O `ApplySavedUIState` é chamado na mensagem `0xCC` (o
tratador de mensagens do `cTSMainToolbar`, BV `0x1af104`), que nenhum passo
do `LoadLot` manda (inferido: vem com a UI do lote, depois da carga e do
`Resume` do primeiro dia): uma família que se muda para um lote salvo
pausado começa pausada.

**No app (rodada n27):** `formats/lot_ui_state.*` lê e escreve o recurso;
`applySavedUiState` (`app/lot_vm_setup.mm`) põe o global 17, o modo de
parede e o andar da câmera (`savedUiLevel`, `app/game_flow.*`: preso entre o
menor e o maior andar do lote) antes de entrar em Comprar/Construir (que
guardam a pausa que acham para o Viver); Comprar e Construir de partida
começam no andar salvo; no Viver a câmera segue o andar do Sim selecionado
como já fazia. Salvar (`saveGameNow`) grava pausado, andar e modo de parede
(versão 2, como os lotes do jogo; em Comprar/Construir a pausa é a que o
lote tinha, inferido). Sem tela: `opensims2 ... --hood N001 --hood-lot 11
--vm --ui live --key walls-2 --click 139,690 --click 208,678` salva paredes
2 e a volta ao lote imprime `saved UI state: running, floor 1 (app 0),
walls 2`.
