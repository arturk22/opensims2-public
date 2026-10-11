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

Escolha da animação e do ícone (inferido: o mapa caso → animação do
`cTSWinProcLotIntro::onAttach` não foi seguido; escolhido pelo que cada
animação e ícone mostram):

| cena | Reia | ícone no ladrilho do meio |
|---|---|---|
| partida / menu principal | `8DA3ADE7` (800x600) | — |
| bairro | `8DA3ADED` (casas, plantas) | `8DA3ADF3` (casas) |
| lote de uma família | `8DA3ADE8` (Sims, ícones sociais) | `8DA3ADF0` (família) |
| lote residencial vazio | `8DA3ADE9` (construção, jardim) | `8DA3ADF2` (ferramentas) |
| lote comunitário | `8DA3ADEA` (móveis, plantas) | `8DA3ADF1` (o táxi) |
| Create-a-Sim | `8DA3ADEC` (roupas) | `8DA3ADF0` (família) |

Só `F0`–`F4` existem no `ui.package` da base (`F4` = caixas de mudança, sem
uso aqui). O ladrilho do meio é o mesmo em todos os quadros das seis
animações de lote: trapézio de x 221–596, y 195–413 na imagem de 820x620;
o ícone vai reto, reduzido para caber em 250x165, centrado em (409, 300). O
tipo do lote vem do `HoodLot` clicado no bairro (`gLotEntry`); um lote
aberto direto pelas flags usa a de família. Na partida a intro toca antes;
se a carga ainda não acabou quando ela termina (ou é pulada), entra a tela
de carregamento.

Captura: `docs/screenshots/r9-telas-de-carregamento.jpg` (partida, bairro,
lote no meio da carga; o lote na janela logo depois).
