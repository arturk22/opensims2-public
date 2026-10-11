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

## 2. Cenas (`src/app/game_flow.*`, `main_menu.*`, `main.mm`)

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
