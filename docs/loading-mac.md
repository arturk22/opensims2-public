# Carregamento em segundo plano e telas de carregamento

Convenções como em `docs/neighborhood-mac.md`: "dados" = lido nos arquivos
do jogo, "binário" = lido no código do Mac, "inferido" = dedução nossa.

## 1. O que roda onde (`src/app/scene_loader.*`, `main.mm`)

Com janela, cada cena (menu principal, bairro, lote, Create-a-Sim) carrega
em duas partes:

| parte | thread | o que faz |
|---|---|---|
| `prepareScene` | **worker** (`BackgroundJob`, pthread com pilha de 64 MB) | `ResourceIndex` e pasta do jogador (só na primeira cena), decodificação de recursos, `Lot::load`, `buildLotModel` (light maps incluídos), modelos e texturas dos objetos e dos Sims (`Renderer::addObject`: buffers e texturas Metal), a VM (`LotSimulation`), luzes do lote, áudio do lote, live UI, modos compra/construção, a câmera da cena; bairro: `HoodMode`; menu: `MainMenu`; CAS: `CasMode` |
| `finishScene` | principal | o que toca AppKit ou a view: cor de fundo, título da janela, `attach` das camadas de UI, overlays do renderizador, os passos `--click`/`--wait` e o `--screenshot` (sem tela) |

Por que é seguro (inferido do código, não há trava):

- **Renderizador**: a troca de cena cria um `Renderer` novo na thread
  principal antes do worker começar; o worker o enche e ninguém o desenha
  até a carga acabar. Criar buffers, texturas e pipelines num `MTLDevice` é
  seguro em qualquer thread.
- **Globais da cena** (`gSimulation`, `gHood`, `gLive`, `gPie`, ...) e a
  câmera da view: só o worker os escreve durante a carga. Enquanto
  `gSceneLoading`, o `drawInMTKView` só desenha a tela de carregamento (ou
  a intro) e todo handler de entrada volta logo (Esc/Espaço/Enter/clique só
  pulam a intro, não a tela de carregamento). `BackgroundJob::collect` faz
  `pthread_join`, o que ordena as escritas do worker antes das leituras da
  thread principal.
- **`ResourceIndex`** não é thread-safe (`read` mexe na lista de arquivos
  abertos): durante a carga só o worker o usa. As telas de carregamento leem
  o `ui.package` direto (`readUiReia`, `readUiIcon`), sem o índice. Leitura
  em paralelo dentro de um carregador: `ResourceIndex::readConcurrent`
  (`DbpfPackage::readConcurrent`, `pread` num descritor próprio do pacote,
  aberto uma vez).
- **Saída do app** durante a carga: o worker não para no meio, então
  `applicationShouldTerminate` encerra o processo na hora (nada está sendo
  salvo: o save roda na thread principal).
- Erro na carga: a exceção do worker volta em `collect`; aparece um alerta e
  o app vai para o menu principal (ou fecha, se o menu falhou).

Sem tela (`--screenshot`), as duas partes rodam em sequência na thread
principal, como antes (as capturas saem byte a byte iguais); com
`--loading-shot <png>` a primeira cena carrega no worker como na janela e a
tela de carregamento é gravada quando o progresso passa de `--loading-at`
(padrão 0,5) — também confere que a carga no worker dá a mesma cena (a
captura final de `--hood N001 --hood-lot 11 --vm --ui live --run-vm 600`
sai igual com e sem `--loading-shot`). `--loading-hold <s>` (teste) segura a
tela de carregamento esse tempo depois da carga, para olhar na janela.

## 2. Telas de carregamento (`src/app/loading_screen.*`, `movie_host.*`)

Os Reia (`docs/cinematics-mac.md` §2) têm a barra de progresso desenhada na
própria animação (a barra no alto da `0x8DA3ADE7`; os segmentos do alto e a
coluna de ladrilhos que acende nas `0x8DA3ADE8`–`ED`). Então a tela não
roda em laço: `playLoadingScreen` toca o Reia uma vez com o relógio preso ao
progresso (`progresso × duração`; corre em dobro quando está mais de 0,5 s
atrás), e no fim da carga toca o resto depressa (3x ou mais, no máximo
0,5 s) e acaba; daí a cena aparece. O progresso vem das voltas do
`OS2_PERF` (`perfLap` chama `loading::phase`), com a fração de cada fase
numa carga típica de lote (`scene_loader.cpp`); entre fases ele avança
devagar (até 10% a mais, nunca além de 97%).

Escolha da animação, do ícone e do título (rodada 11, pelo mapa do
`cTSWinProcLotIntro::onAttach` de `docs/cinematics-mac.md` §2.1; a rodada 9
tinha escolhido pelo que cada animação mostra):

| cena | Reia | ícone / imagem no ladrilho do meio | título |
|---|---|---|---|
| partida / menu principal | `8DA3ADE7` (800x600) | — | — |
| bairro (caso 3, o jogo base) | `8DA3ADEB` | `ADF3` (casas) | o nome do bairro (CTSS) + `U(0x84, 0x10)` ("Carregando") embaixo |
| lote com família (caso 0) | `8DA3ADE8` | a foto da família (`<N>_FamilyThumbnails.package`, tipo `0x8C3CE95A`); `ADF0` se faltar | "A Família X" (UIText 7 #4) |
| lote residencial vazio | `8DA3ADE9` | `ADF2` (ferramentas) | o nome do lote |
| lote comunitário | `8DA3ADEA` | a miniatura do catálogo de um objeto do lote (descoberta na carga, ver abaixo), senão `ADF1` (o táxi) | o nome do lote |
| Create-a-Sim (caso 1) | `8DA3ADEC` | `ADF0` (família) | "Criar uma Família" (UIText 7 #3) |

Só `F0`–`F4` existem no `ui.package` da base. O ladrilho do meio é o mesmo
em todos os quadros das seis animações de lote: trapézio de x 221–596, y
195–413 na imagem de 820x620; o ícone vai reto, reduzido para caber em
250x165, centrado em (409, 300). O título (fonte `GenBodyMedium`, 30 px,
branco com sombra) vai centrado em (409, 470): o lugar e a fonte são
inferidos (a janela de texto do `TSWinProc` não foi seguida). O caso, o nome
do lote, a família e o pacote das fotos vêm do `HoodLot` clicado no bairro
(`gLotEntry`); um lote aberto por flags (`--hood N --hood-lot id`) lê o mesmo
da `Neighborhood` (`fillLotEntryFromFlags`). Rodada 12: o bairro Tutorial usa `ED`/`F4` com `U(0x84, 0x2C)` e o
subtítulo (inferido: caso 7 = tutorial); o subtítulo vai também no lote com
família; o caso 6 (voltar do Create-a-Sim com a família nova) usa `EB`, a
foto e "A Família X". Ficam de fora: o tipo de bairro das expansões e o lote
de destino diferente do atual. Capturas `r12-carregamento-*.jpg`.

A imagem do lote comunitário: a miniatura 320x320 do jogo vem da câmera de
miniaturas; aqui é a imagem de 128x128 do catálogo
(`ObjectThumbnails.package`, `Catalog::thumbnail`), achada pela carga: depois
do `Lot::load`, `offerCommunityLotImage` (`lot_loading.mm`) percorre os objetos do
lote e oferece (`offerLoadingIcon`) o primeiro cujo catálogo tem o bit
`0x20` do campo `0x5E` do OBJD (= o "function subsort", 0x5E = (0xFC-0x40)/2;
leitura nossa do "campo 0x5E" do doc). A tela de carregamento troca o ícone
quando ele chega (`LoadingScreen::lateIcon`). **Não achou nenhum objeto no
Parque Bosque (N001) nem em outros lotes olhados: a regra não está
confirmada e na prática o táxi aparece.**

## 2.1 Fases do progresso

O progresso vem das voltas do `OS2_PERF` (`perfLap` chama `loading::phase`)
e, nas cenas que não são lote, de chamadas `loading::phase` dentro dos
construtores (`hood: terrain/lots/ui/bins` no `HoodView`/`HoodMode`, `menu:
scripts`, `cas: catalog/ui/studio` no `CasMode`). A fração de cada fase está
na tabela de `scene_loader.cpp`, tirada de uma carga típica (com `OS2_PERF=1`
cada fase mostra em que ms da carga chegou: `progress 40% at 262 ms (hood:
terrain)`): bairro N001 0,9 s (terreno 0,26 s, lotes 0,58 s, UI 0,79 s,
bins 0,85 s); CAS 0,9 s (catálogo 0,37 s, studio 0,65 s, o resto é o
`newSim`); o menu principal leva 0,2 s.

Capturas: `docs/screenshots/r9-telas-de-carregamento.jpg` (rodada 9) e, da
rodada 11, `r11-carregamento-{familia,vazio,comunitario,bairro,cas}.jpg`.
