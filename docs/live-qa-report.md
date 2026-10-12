# QA da janela real (rodada 9)

Como foi feito: app aberto de verdade (menu → bairro → lote), cliques por eventos
`CGEvent` (os cliques do System Events não chegam à view Metal) e
`screencapture -R` só na janela. Coordenadas = ponto da janela (inclui a barra de título, ~28 pt).
Cuidado ao automatizar: dois processos `opensims2` abertos por agentes diferentes disputam a "janela 1";
selecionar o processo pelo pid.

| # | Passo | Resultado |
|---|-------|-----------|
| 1 | Intro (Esc pula), menu, escolher Belavista, painel com Usar, vista do bairro, clicar um lote, Entrar | passa |
| 2 | Lote: zoom pela roda, painéis Necessidades / Habilidades e Carreira / Recompensas, tooltips, botão de pausa (borda vermelha) | passa |
| 2 | Pie menu: cliques em caixa de correio, lixo, escada abrem "no interactions available"; nenhum Sim ficou ao alcance do clique | não verificado |
| 2 | Atalhos de teclado (P, 1-3 por keystroke) | inconclusivo (log com buffer e evento atrasado) |
| 2 | Pan com botão direito, rotação com botão do meio, fila (cancelar por clique) | não testado (sem tempo/sem Sim a mão) |
| 3 | Viver ↔ Comprar ↔ Construir ↔ Opções | passa |
| 4 | Comprar: categoria, item, colocar (§80 debitado), desfazer pelo botão, Page Up/Down | passa |
| 4 | Construir: parede arrastada (6 seções por §420), desfazer | passa |
| 4 | Vender, refazer, piso, papel de parede, porta | não testado |
| 5 | Opções → Salvar, a partir de Construir/Comprar | **falhava, corrigido** (`6e1d64c`) |
| 5 | Opções → Vizinhança → "Salvar?" → Sim → bairro → reentrar no lote | passa |
| 5 | Opções → Sair abre o diálogo; Cancelar ("Não Sair") volta; Esc no lote abre "Salvar?" | passa |
| 6 | `--cas`: tela do passo 1, gênero, passo 2 (Mudar Cabeça) | passa, ver os itens abaixo |

## Corrigido

- O painel de Opções flutua sobre o painel de Construir/Comprar, mas o clique ia antes para o fundo
  do painel de baixo (mesma classe do bug do puck): Salvar, Vizinhança e Sair não respondiam quando
  as Opções eram abertas a partir de Construir ou Comprar. Agora as Opções recebem o clique primeiro
  (`main.mm`, `mouseDown` e o caminho `--click`). Checagem sem tela:
  `--lot N001_Lot11 --vm --ui live --click 170,612 --click 138,692 --click 208,682` mostra `live UI: save`.
- A dica de recusa do modo construção ("cannot: ...") saía pela borda direita da janela; agora fica
  dentro dela.

## Não corrigido

Ver as entradas "QA ao vivo" no `TODO.md` (com passos).

# Rodada 10

Mesmo método (CGEvent com deltas, `screencapture -R` só na janela, pid próprio). Lotes: Belavista lote 68 (família
com 4 Sims, pelo menu) e `--hood N001 --hood-lot 24`.

| Passo | Resultado |
|-------|-----------|
| Atalhos 3 e P (velocidade 3 acende, pausa com borda vermelha) | passa |
| Rotação com botão do meio (precisa de deltas no evento) e pan com botão direito | passa |
| Pie menu num objeto da casa: abre, escolher "Ver" enfileira (ícone na fila), clicar o ícone tira da fila | passa |
| Pie menu em Sim | não testado (Sims dentro de casa, sem corte de telhado) |
| Comprar: escolher item, mostrar recusa, refazer/desfazer pelos botões | passa; vender não testado (ver TODO) |
| Construir: piso (§72 debitado), desfazer volta §2659, refazer §2587 | passa |
| Construir: parede, porta (fantasma vermelho + recusa), papel de parede | recusas mostram só o texto do jogo; papel de parede e porta dentro da casa não testados |
| Opções sobre Construir: painel de baixo escondido, só o botão das Opções aceso | passa (corrigido) |
| Opções → Vizinhança → Sim a partir do lote 24 (`--hood`) e do lote 68 | grava (a falha da rodada 9 era do clique já corrigido em `6e1d64c`) |
| Menu → Belavista → Famílias → Criar Nova Família → CAS (nome, gênero, arquétipo, 2º Sim, Fazer um Filho, árvore genealógica) → Aceitar | passa |
| Família na bandeja → mover para o lote 24 (§17.621 debitado; sobra §2.379) → jogar → Opções → Vizinhança → Sim → lote mostra a família (patrimônio §20.000) | passa |
| Retrato do Sim e barra da família na família nova | estavam em branco; corrigido (o CAS agora grava os retratos) |

## Corrigido na rodada 10

- Recusa do modo construção só com o texto do jogo; a dica de Comprar também fica dentro da janela.
- Opções por cima de Comprar/Construir: os painéis escondem e os cliques não vazam; só o botão das Opções fica aceso.
- Criar um Sim grava os retratos (IMG 0x08 e 0x800 do Sim) nos arquivos de personagem.

## Não corrigido

Ver as entradas "QA ao vivo" do `TODO.md`: ícone da fila em branco, foto da família nova em branco na bandeja,
falta de corte de paredes/telhado, ferramentas de Comprar sem ação.

# Rodada 11

Mesmo método (CGEvent, pid próprio). Novidade do método: outros agentes abriram janelas do app por cima da minha
(e o macOS tirou a minha da área visível mais de uma vez), então cada clique agora é precedido de "trazer a janela
do meu pid para a frente e conferir que ela está em 95,49" e só então vale o clique; captura só da região da janela.
Lotes: Belavista lote 68 (`--hood N001 --hood-lot 68 --vm --ui live`), bairro (`--hood N001`), `--cas`, menu.

| Passo | Resultado |
|-------|-----------|
| Leque de paredes do puck (baixo / cortada / alta / telhado), tooltip "Paredes Cortadas - Origem/Fim" | passa; o telhado só aparece no último andar (como o `WallView::topLevel` diz), no andar 2 "telhado mostrado/escondido" não muda a imagem |
| Home/End, Page Up/Down (andar 1 ↔ 2, paredes altas/cortadas) | passa |
| Corte acompanhando a rotação (botão do meio) | passa: o modelo é refeito só quando um lado de parede vira (2,3 ms de construção + 1 ms de envio) |
| Suavidade ao girar | passa: CPU 3,5 ms em média por quadro (pior 19 ms) com `OS2_PERF=1` |
| Clicar num Sim dentro de casa (sem telhado) abre o pie menu dele | passa |
| Comprar: conta-gotas (objeto da casa) | funciona; ver os itens abaixo (pega a árvore na frente; "não está no catálogo" para poltrona/janela) |
| Comprar: etiqueta (vender) num objeto visível: escada §1.150 → §3.400, a escada some; desfazer volta §2.250 | passa |
| Bairro: Lotes & Casas abre (casas na lixeira, lotes vazios dos 10 tamanhos), escolher um tamanho, o fantasma verde segue o mouse, vermelho sobre casa, clicar põe o lote (a vista é refeita: "27 lots") | passa |
| Bairro: Famílias → escolher a família → clicar lote ocupado → "Este lote já está ocupado" | passa |
| CAS: criancinha (sentada), criança, adulto, idoso em pose natural, rosto com cor | passa |
| CAS: adolescente | **falhava (rosto branco)**, passa com `d4ddeb1` do main depois do rebase |
| Menu principal → "Aprenda a Jogar" | abre o painel, mas as listas "O Básico" / "Além do Básico" estão vazias e o rótulo "Tutorials" fica em inglês: a frente do tutorial não está no main |
| Bebê, aniversário, contas na caixa de correio, TV com som, música por modo, despejar/mudar de família (dia 1 às 7:00), Design | não testado nesta rodada (sem tempo e sem ouvido; ver os testes sem tela das frentes) |

## Corrigido na rodada 11

- Bairro: o cartão do lote (o que abre ao passar o mouse num lote) aparecia por cima do puck e dos painéis de
  Famílias e Lotes & Casas quando o mouse estava sobre eles, porque o lote atrás deles era pego pelo raio. Agora o
  mouse sobre o puck, os painéis ou o diálogo não passa o lote atrás (`HoodUi::covers`).

## Não corrigido

- Conta-gotas de Comprar: com o conta-gotas, clicar na mesa de jantar pegou "Tree - Birch" (a caixa da
  copa da bétula na frente); e clicar no sofá "Sofa - Loveseat - Art Nouveau" deu "is not in the catalog" (a venda do
  mesmo objeto pela etiqueta funciona, §1.250). Ver o TODO.
- Depois de pôr um lote vazio no bairro a câmera volta ao ponto de partida (a vista é refeita por troca de cena).
- No lote ao vivo o quadrado de ícone da fila com um X vermelho (a ação atual do Sim) segue sem miniatura.
- Quadro branco ao lado do Sim no CAS (o espelho/cavalete sem textura).

# Rodada 11, passada 4

Mesmo método (CGEvent, pid próprio, janela trazida para a frente e posição conferida antes de cada clique, captura só da região da janela).
Lote: Belavista lote 68 (`--hood N001 --hood-lot 68 --vm --ui live`) e bairro (`--hood N001`), pastas de usuário descartáveis. Sem
ouvido: o som foi conferido pelo log de saída (`OS2_SOUND_LOG=1`, `--sound-mix`, `--music`). Problema de ambiente: o disco ficou cheio
(1,7 GB livres) e o primeiro `git worktree add` falhou no meio; foi refeito com `GIT_LFS_SKIP_SMUDGE=1` e o app usa `--data` do repo principal.

| Passo | Resultado |
|-------|-----------|
| Velocidade 3 pelo botão, relógio passa de quarta 07:26 para quinta 10:21 em ~1 min, escuridão à noite | passa |
| Caixa de correio: pie menu "Receber Correio", Sim vai buscar | passa (enfileirou); nenhuma conta chegou no dia 1 (o binário só traz contas nos dias 1 e 3 às 9:51; `§2.670` não mudou) — não conclusivo |
| Casa começa às 7:00 | **falha**: o lote salvo (lote 68) abre às 07:08 e ao entrar já passaram ~18 minutos (07:26 após 25 s); a regra de 7:00 é só da mudança de família, não testada ao vivo |
| Ônibus escolar | **falha**: o veículo branco (`vehicleschoolbus`) chega às ~9:36 e continua parado na rua às 19:47 e às 10:21 do dia seguinte (nunca vai embora); textura `vehicleschoolbus-groundshadow-alpha_txtr` não achada |
| Incêndio | às quinta 10:21 a casa está pegando fogo, com todos os Sims na vermelha (sem ninguém agindo): é da frente de fogo, só anotado |
| Música: `--music pop` no lote | toca `26B3DB7C-FFC55552`; sem `--music` nada ("music: none") no lote sem som ligado; TV: "0 TV shows heard" em 840 s sem ninguém ver TV (não exercitado) |
| Som: rodada de 840 s sem tela | 16 sons da VM, 2121 eventos de animação, 2418 vozes, 280 notas de ambiência; passos/arrastar/tapa (`slap_hand_back` ×220) tocam |
| Construir: Design (0xCC607030), dia/noite (0xCC607035), etiqueta | **falha**: clicar nos ícones de baixo (etiqueta, "%" e seta) não faz nada e não loga; imagem idêntica antes/depois |
| Bairro: Lotes & Casas abre, bandeja com miniaturas de lote | passa |
| Bairro: mover lote (mão): pegar o lote 57 destaca a base e o fantasma segue o mouse; vermelho em lugar inválido | passa (não achei um lugar válido para soltar; fica sem teste de soltar) |
| Bairro: girar lote com família: diálogo "a família será movida com o lote e perderá os relacionamentos" | passa o diálogo; **depois de aceitar o lote não muda visualmente** e nada é logado (diferença de imagem vazia) |
| Bairro: demolir (escavadeira) o lote 57 vazio: diálogo "Tem certeza de que quer demolir o lote", Sim remove o `N001_Lot57.package` e a casa some | passa |
| Bairro: enviar casa para a lixeira e recolocar | não achado (o botão de seta em 900,1130 da bandeja não responde; clicar numa casa abre o cartão com a família, sem a opção) |
| Bolo, bebê, despejar, foto da família na lixeira, mudança às 7:00 | não testado ao vivo (sem tempo; `3a1656e`, `c716aec` trazem testes sem tela) |
| Menu "Aprenda a Jogar" | não refeito (a frente do tutorial só tem a documentação em `f91bebb`) |

# Rodada 12, passada 5

Mesmo método (CGEvent, pid próprio, janela trazida para a frente e posição conferida antes de cada clique, captura só da região da janela).
Lote 68 (`--hood N001 --hood-lot 68 --vm --ui live`), bairro (`--hood N001`) e o menu com "Aprenda a Jogar". Problemas de ambiente: um
diálogo do macOS ("s2pkg would like to access files in your Documents folder", de outro agente) e um aviso de reabrir janelas após
"crash" ficaram por cima do centro da janela o tempo todo (atrapalham a leitura do centro da cena, não os cliques); depois de matar
o app por pid o macOS passa a perguntar "Reopen/Ignore" ao abrir de novo: abrir com `-ApplePersistenceIgnoreState YES`.

| Passo | Resultado |
|-------|-----------|
| Menu → Aprenda a Jogar: listas O Básico (3) / Além do Básico (2), miniatura e texto mudam com a seleção, Jogar | passa |
| Lição 1: painel "Passo n de 32", Anterior/Avançar (Anterior volta um passo), Avançar apagado nos passos que esperam um clique | passa |
| Lição 1: clicar o controle indicado avança (zoom +, girar, modo Simulação, cabeça do João, modo Compra); câmera segue o zoom/giro; Vir Aqui (passo 12) manda o Sim andar e avança | passa |
| Lição 1: controles desligados ficam sombreados e inertes (botão de ação do Sim) | passa |
| Lição 1: seta vermelha saltitante | **falhava, corrigido**: a seta ficava uma seta inteira longe do controle e no passo 2 apontava para o botão vizinho (Comprar) em vez do Zoom |
| Lição 1: "Sair" pelas Opções → "Sair sem Salvar" | fecha o app (o jogo também sai) em vez de voltar ao menu; fim da lição (passo 32, "Exit Lot") não alcançado |
| Lição 2: abre com 13 passos, dois Sims no seletor, seta na miniatura do João | passa |
| Comprar: Design (0xCC607030): diálogo, 4 amostras, "Custo para Atualizar: §15", Aceitar só acende com mudança, Aceitar cobra §15 (§2.250 → §2.235), desfazer devolve | passa |
| Comprar: dia/noite | **falhava, corrigido**: mostrava o estado anterior um clique atrasado (o modo pausa o relógio da cena em que o fade corre) |
| Comprar: conta-gotas na mesa de jantar da lote 68 | pega "A Mesa Conversa-Fiada" (§275), não a bétula; passa |
| Comprar: conta-gotas no sofá Art Nouveau | não testado (ficou atrás do diálogo do macOS) |
| Convés, telhado automático, sons do Construir | não testado (sem tempo; sons só pelo log no encerramento) |
| Sentar de frente para a cadeira e sem flutuar | não observado ao vivo (Sims dentro de casa, paredes altas, câmera cobrindo a casa) |
| Vir Aqui no gramado: pie com "Vir Aqui" / "Vir Saltitando Aqui", o Sim anda até lá | passa (Sim parado em pé durante a espera: não avaliado) |
| Bairro: cartão do lote, painel Informações, Opções (Salvar/Sair/Vizinhança...), painel de terreno | abrem e fecham sem erro |
| Bairro: puck sobre painéis, câmera mantida após pôr lote, diálogo de nome, lixeira com Delete e filtros | não testado |
| Vida: bolo de aniversário e contas | não testado |

## Corrigido na rodada 12, passada 5

- A seta do tutorial usava o canto do controle na direção da seta; o binário (`docs/tutorial-mac.md` §3) a põe no canto **oposto**.
  Agora a ponta cai no centro do controle quando a seta está parada e se afasta no salto.
- O botão Dia/Noite do Comprar/Construir força o estado de luz sem fade (o fade andava pelo relógio da cena, parado nesses modos).

## Não corrigido

Ver as entradas "QA ao vivo" do `TODO.md` (rodada 12, passada 5).

# Rodada 12, passada 6

Mesmo método (CGEvent, pid próprio, janela conferida em 95,49 antes de cada clique, captura só da região da janela, `-ApplePersistenceIgnoreState YES`).
Lotes: N001 lote 11 e 68, N002 lote 10 (`--hood ... --vm --ui live`), bairro, CAS, menu. Ambiente: o diálogo do macOS "s2pkg would like to access
files in your Documents folder" (de outro agente) ficou por cima do centro da janela o tempo todo; não foi clicado. Só uma passada curta, sem fim de lição.
Obs.: o cabeçalho "r12-ui-polish" já estava no main depois do rebase (ícones da fila e diálogo Design abaixo).

| Passo | Resultado |
|-------|-----------|
| Leque de paredes do puck: baixas / cortadas / altas / telhado | passa (os quatro modos mudam a imagem; telhado só no último andar e em "altas + telhado") |
| Home/End e Page Up/Down | passa |
| Corte acompanha a rotação (setas ←/→): paredes internas e as externas voltadas para a câmera cortadas | passa em N002 lote 10 (cozinha e Sim visíveis por dentro) |
| Telhado colocado no Construir aparece de uma vez | não testado ao vivo (coberto pelo teste sem tela do `6f0d6c3`) |
| Recortes de porta/janela nas paredes, platibandas e varanda do N002_Lot10 | janelas e arcos aparecem recortados na cozinha do lote 10; platibandas não inspecionadas de perto |
| Caixa amarela com seta vermelha do N001_Lot11 | sumiu |
| Cor do calçamento à noite | não conferido (a velocidade 3 passou direto; câmera fora de cena) |
| Ícones da fila sem X vermelho na ação atual | passa (nenhum X depois de pôr o Sim para andar; clicar no ícone marca) |
| Diálogo Design (Comprar → ferramenta Design na mesa) | abre com fundo e nome em PT-BR; o caso de dois quadros não foi achado |
| Sentar de frente para a cadeira / sem flutuar | não observado (Sims fora de quadro; ver bugs abaixo) |
| Tutorial: menu → Aprenda a Jogar → lição 1 → Jogar | carrega o lote; passos 1–5 não refeitos ao vivo nesta passada (tutorial coberto na passada 5) |
| Bairro: Decorações (F3): pôr flora, desfazer ("decoration undo done") | passa; recusa "may not stand in a lot" só no log |
| Bairro: história (F4), digitar | passa: o texto digitado aparece no editor; Esc fecha |
| Bairro: Famílias (barra de famílias) | abre e mostra as famílias |
| Bairro: "Ver Amigos", puck sobre painéis, pôr lote, lixeira (Delete/filtros) | não testado |
| CAS: espelho | passa: azul claro liso, não branco, mas ainda sem reflexo |
| CAS: botão de gênero troca o Sim; janela de biografia abre com cursor | passa |
| CAS: braços do alienígena | não testado |
| Vida: bolo de aniversário, contas | não testado |

## Bugs achados (não corrigidos)

- Diálogo de sim/não com o título cru "yes-no" em inglês (visto em N002 lote 10 com velocidade 3, "Ei Jeanie, Jill pode ..."): o título deveria vir
  do binário/PT-BR; repro: `--hood N002 --hood-lot 10 --vm --ui live`, velocidade 3 por ~1 min de relógio de jogo.
- Teclas ←/→ giram, mas ↑/↓ andam a câmera: depois de várias o lote some do quadro (esperado), sem atalho para recentrar.
- Espelho do CAS é um quadro azul liso (sem textura de reflexo).

# Rodada 13, passada 7

Mesmo método (CGEvent, pid próprio, janela conferida antes de cada clique, captura só da região da janela, `-ApplePersistenceIgnoreState YES`, cópia do `.app` do `main` em `461ec3d`).
Lotes: N001 lote 68, 11 e 82, bairro N001, CAS. Ambiente: o diálogo do macOS "s2pkg would like to access files in your Documents folder" (e o aviso de reabrir) ficou por cima do centro da janela o tempo todo (não foi clicado; a janela foi movida para ler o que estava atrás); o Dock apareceu em duas capturas movidas, apagadas. Tempo curto: só parte da lista.
Obs.: Esc dentro do lote abre "Salvar?" (esperado, `docs/game-flow-mac.md`), então não serve para fechar menus de pie; clicar com o botão direito fecha.

| Passo | Resultado |
|-------|-----------|
| Sentar: cadeiras de jantar empurradas sob a mesa, Sim puxa a cadeira, senta de frente, levanta ao lado | não conferido ao vivo (a 6 capturas, Sims não chegaram a sentar no lote 68 dentro do tempo; cadeiras da mesa de jantar aparecem encostadas na mesa no corte) |
| "Dormir de Pijama" no N001_Lot11 (Mortimer, cama de casal) | **não conclusivo**: enfileirou ("Dormir de Pijama", Bed - Double - Expensive), mas em ~1 min de relógio (velocidade 2) ele só chegou ao andar de baixo; sem ver subir/trocar/deitar; o binário usado já tem `37f961f` (slots de multi-tile) |
| Espelho do CAS | passa: o espelho de pé reflete a Sim e a parede de tijolos (era um quadro azul liso na passada 6) |
| Espelho de banheiro do N001_Lot82 e Reflexo desligado | não testado (o botão Gráficos do Opções do lote não abriu a janela nas duas tentativas; no bairro abre e funciona) |
| Clique nos Sims pelo corpo | passa: clique no tronco do Sim abre o pie do Sim (Conversar/Beijar...); clique entre as pernas e ao lado da perna cai no objeto atrás (menu da TV/lâmpada), sem pegar o Sim |
| Bairro: girar lote ao mover (arrastar com o botão preso) | passa: pegar o lote 57 (mão), arrastar gira o lote ("lot turned to 3 by the drag"), o fantasma segue |
| Bairro: Ver Amigos | passa: no lote 49 (nas Nuvens) dois arcos verdes saem para os lotes 11 e 68, com os lotes destacados |
| Bairro: decoração recusada mostra dica em PT-BR | passa: "Não pode ser colocado dentro de lotes." no canto (flora sobre um lote) |
| Bairro: Sombras Baixo tira as sombras dos morros | passa: com `option.Shadows = 1` a mancha escura grande no canto superior direito some, mas só **depois de reabrir** o bairro (o mapa de luz é feito na carga; ver abaixo) |
| Bairro: fotos de cabeça no diálogo do lote | passa: o cartão do lote 49 mostra os dois moradores de corpo inteiro até a cabeça (cabeça de Sim do cartão); a barra de famílias mostra miniaturas de cabeça |
| Bairro: pôr lote vazio (nome, estradas, câmera mantida) | parcial: clicar numa família da bandeja mostra o cartão e o diálogo "Aceitar"; mudar a família para o lote 57 mostra "Você não pode pagar por este lote." (§20.900 contra §28.166), então a cena de mudança não rodou; lote vazio novo não testado |
| Vida: cena de mudança (8:00, cortina) | não testado (família sem dinheiro para o lote; ver acima) |
| Vida: ônibus escolar no lote 68 (manhã, velocidade 3) | passa: o ônibus **amarelo** chega à rua na quinta às 08:08, fica parado até ~08:48 (a quarta passou sem aparecer na janela da câmera) e vai embora antes das 09:00 (depois passa uma van branca); buzina não conferida (sem som) (`docs/screenshots/r13-qa7-onibus.jpg`) |
| Vida: bolo de aniversário | não testado |
| Cor do calçamento à noite | não conferido (câmera na rua à noite: a rua escura parece uniforme, sem comparação com o dia) |
| Telhado no Construir | não testado |
| Tutorial, fim da lição 1 (passo 32) | não testado |

## Bugs achados (não corrigidos)

- Sombras Baixo só vale depois de reabrir o bairro: o `applyGraphics()` troca `heightMapShadows`, mas o mapa de luz do terreno é calculado na carga (`hood_view.cpp`, `computeHoodLightMap`); trocar a opção com o bairro aberto não refaz. Repro: bairro, Opções, Gráficos, Sombras "Desligado" ou "Médio" no 1 (log `options: Shadows 1`), Aplicar Mudanças: nada muda; reabrir mostra sem sombra.
- Opções do lote: o botão de Gráficos (monitor, 530,1030 em 2000 px) não abriu a janela de Gráficos duas vezes no lote 82 (só o painel de Opções abriu); no bairro o mesmo botão abre. Repro: `--hood N001 --hood-lot 82 --vm --ui live`, Opções, Gráficos.
- Mover/aceitar mudança de família sem dinheiro: o diálogo só diz "Você não pode pagar por este lote." (ok), mas a família do bin fica selecionada e o painel do cartão do lote continua aberto; não é bug do binário, só anotado.
- Dormir no N001_Lot11 segue lento de conferir: sem uma forma de "pular" (cheat), o Mortimer leva mais de 1 min de relógio de jogo para chegar à escada; recomendo conferir com um `s2pkg` sem tela até a cama.

# Rodada 14, passada 8

Mesmo método (CGEvent, pid próprio, janela conferida em 95,49 antes de cada clique, captura só da região da janela, `-ApplePersistenceIgnoreState YES`, cópia do `.app` do `main` em `20b1297`). Só N002_Lot10 (`--hood N002 --hood-lot 10 --vm --ui live`). Ambiente: os helpers `win2`/`k` das passadas anteriores tinham sido apagados (refeitos num `qa` novo); o diálogo do macOS "s2pkg would like to access files" ficou por cima do centro o tempo todo e, no fim, um aviso "opensims2 quit unexpectedly" de outro agente cobriu o botão Cancelar do diálogo Salvar (não foi clicado). A tela é 1470x956, então a janela de 752 px não sai de trás do diálogo. Tempo curto: só parte da lista.
Dica: duplo clique na miniatura de um Sim no painel esquerdo centra a câmera nele; o Sim "Pollination Tech#9" (alienígena, 1º do painel) é NPC e só oferece "Entrar" no chão (usar os outros três para Vir Aqui).

| Passo | Resultado |
|-------|-----------|
| Andar: Vir Aqui no chão da sala (João), sequência de capturas | passa: o Sim sai de trás da TV andando, pernas alternam, sem deslizar visível; a parada/virada final não foi medida |
| Andar: Vir Saltitando Aqui, corrida em trajeto longo | não testado: o pie do chão mostrou só "Vir Aqui" (e "Ver Nuvens" no gramado); "Saltitando"/"Correr" não apareceram nas posições tentadas |
| Mistura de animações: sentar na cadeira da mesa de jantar e levantar (alienígena) | passa: senta de frente para a mesa, sem flutuar, corpo na cadeira; ao cancelar na fila levanta e anda em 12 capturas seguidas sem T-pose nem salto |
| Diálogo de sim/não da VM (telefone N002_Lot10, velocidade 3) | não testado; o diálogo "Salvar" (Esc no lote) usa o quadro do jogo e botões Sim/Não/Cancelar em PT-BR |
| Dicas sobre o painel ao vivo no estilo azul claro | passa: "Opções - F5" aparece em azul claro |
| Opções no lote: Gráficos abre | passa (era falha na passada 7); o painel de Gráficos fica atrás da barra de Opções (sobreposição estranha), com o botão "Aplicar Mudanças" meio coberto |
| Opções: Sombras Desligado | log `options: Shadows 1`; efeito imediato não conferido a olho (cena escura, sem comparação) |
| Esc fecha primeiro a janela de Gráficos, depois abre "Salvar" | passa; "Esc fecha o pie primeiro" não conferido (o clique do pie caiu atrás de outro diálogo) |
| Sentar de frente para a cadeira | passa na cadeira de jantar do lote 10 |
| Dormir (N001_Lot11), espelho do banheiro (N001_Lot82) e Reflexo, lote vazio novo no bairro, calçamento à noite, telhado no Construir, fim do tutorial (passo 32) | não testados (tempo) |
| Desempenho: girar com as setas e andar | sem engasgos visíveis; log `OS2_PERF` do próprio app: 2–7 ms de CPU por quadro médio, pior ~60 ms uma vez (ao reconstruir paredes) |

## Bugs achados (não corrigidos)

- Clicar em "Aplicar Mudanças" do Gráficos no lote também cria um ícone na fila ("queue icon added" no log): o clique parece passar para o mundo por baixo da janela/da barra de Opções.
- A janela de Gráficos aparece parcialmente sob a barra de Opções do lote (ordem de desenho).

# Rodada 14, passada 9

Mesmo método (CGEvent, pid próprio, janela conferida antes de cada clique, captura só da região da janela, `-ApplePersistenceIgnoreState YES`, `build/` do `main` em `80a5f38`, mais novo que o commit). N002_Lot10 (`--vm --ui live`), bairro N002, e a noite (`--lighting night`).
Ambiente: os diálogos do macOS ("opensims2 quit unexpectedly" e "s2pkg would like to access files") cobriram o centro da tela o tempo todo (não clicados). Truque que funcionou: redimensionar a janela por System Events para 600x640 em x=2 (o diálogo fica à direita), mas a interface do jogo não escala e a metade direita dos painéis é cortada (a barra de Construir e a janela de Gráficos ficam cortadas); capturar com x negativo dá imagem errada (não usar). O Dock aparece na captura se a janela passa de y≈100 com 752 de altura. O `qa` não tem tecla, foi feito um `k` (CGEventPost) novo. Só parte da lista cabia.

| Passo | Resultado |
|-------|-----------|
| Andar: Vir Aqui no gramado | passa: o Sim sai da casa e chega ao ponto; em trajeto longo anda muito mais rápido que andando (corrida, ~25 tiles em ~3 s) |
| Andar: cantos suaves, partida/parada por ângulo, mistura de ciclos, parada ao ser bloqueado | não conclusivo: com capturas soltas (≥0,15 s) e o Sim atrás do diálogo não dá para julgar mistura; nenhum T-pose, salto ou deslize visível em ~25 quadros |
| Andar: "Vir Saltitando Aqui" / "Correr Aqui" | falha/não oferecido: o pie do chão só mostra "Vir Aqui" (e "Entrar", ver abaixo) |
| Diálogos: janela simples feita em código (Salvar? ao apertar Esc no lote) | passa: moldura, título "Salvar", texto, três botões Sim/Não/Cancelar; Esc fecha (Cancelar, log `dialog "Salvar": Cancelar`); Return/espaço não testados (sairiam do lote) |
| Diálogos: telefone (listas de ligar/convidar/festa), guarda-roupa pelo armário, ícone do diálogo | não testados: o telefone e os armários estão no andar de cima (nível 2/3 do N002_Lot10) e o clique neles abriu o pie "Trocar para..." (ver bugs) |
| Opções: janela de Gráficos abre acima da barra, "Aplicar Mudanças" visível | passa (`docs/screenshots` não guardada, janela de 600 px corta a direita) |
| Opções: clique em Aplicar não chega ao mundo | passa: nenhum "queue icon added"/"go here" novo no log |
| Opções: Esc fecha primeiro o pie | passa (Esc fechou o pie de Vir Aqui e o do bar, sem abrir "Salvar") |
| Opções: Esc fecha a janela de Gráficos primeiro, depois "Salvar" | passa (Esc, Esc... a barra de Opções ainda ficou aberta depois de duas Esc; só a terceira abriu "Salvar") |
| Opções: listas "Tamanho da Tela" e "Freqüência de Atualização" | vazias (caixas sem itens) |
| Construir: piscina só em retângulos, Ctrl-arrastar apaga, caixa ciano, arco sem cerca, tiles da pista | não testado: não achei a ferramenta de piscina (a gota d'água do terreno não criou nada com arrastar) e o painel fica cortado em janela pequena |
| Bairro: Ver Amigos (arcos/tubos verdes) | passa: dois arcos verdes do lote Silva para os outros lotes, com os lotes destacados (`docs/screenshots/r14-qa9-ver-amigos.jpg`) |
| Bairro: cartão do lote com a família | passa: a família de corpo inteiro (Johnny, Silva etc.) sem cortes; as miniaturas do painel esquerdo no lote mostram só a cabeça, bem enquadradas |
| Bairro: barra de famílias | mostra a família de corpo inteiro (como no jogo) |
| Calçamento à noite | passa: a calçada fica cinza escura, no mesmo tom do gramado/paredes (não branca) |
| Dormir no N001_Lot11, espelho do N001_Lot82 e Reflexo off, lote vazio novo, telhado no Construir, passo 32 do tutorial | não testados (tempo; o espelho do lote 82 está no andar de cima, (28.5, 29.5, nível 2)) |

## Bugs achados (não corrigidos)

- Clicar em objetos do andar de cima (armário, telefone) com o andar 2 visível (Page Up) abre o pie "Trocar para..." (o do Sim) em vez do objeto. Repro: `--hood N002 --hood-lot 10 --vm --ui live`, Page Up, clique no armário escuro do quarto (nível 3 pelo `s2pkg lot-run --list`). Pode ser um Sim de baixo pego pelo raio ou o objeto de cima não pego; não investigado.
- O texto da entrada do pie do chão varia: "Vir Aqui" ou "Entrar" na mesma TTA 2 do "Destination - For Go Here & Shoo" (log `queued "Entrar" (... TTA 2)`), mesmo com o Sim dentro do lote; "Vir Saltitando"/"Correr" nunca saem. Repro: selecionar o Johnny, clicar no gramado a leste da varanda.
- Um objeto pequeno parado no gramado (ao lado da árvore a leste, objeto 173) pega os cliques ("no interactions available (its tooltip)") e atrapalha o Vir Aqui perto dele; não vi o que é (possível destino que sobrou).
- Duas Esc seguidas com a barra de Opções aberta não fecharam a barra; só a terceira abriu "Salvar".
- Listas "Tamanho da Tela"/"Freqüência de Atualização" vazias na janela de Gráficos do lote.

# Rodada 15, passada 10

Mesmo método (CGEvent, pid próprio, janela conferida antes de cada clique, captura só da região da janela, `-ApplePersistenceIgnoreState YES`, `build/` do `main` em `c8b1755`, binário das 05:26, mais novo que o commit). N002_Lot10 (`--vm --ui live`, `OS2_DEBUG_PICK=1` para ver o objeto pego).
Ambiente: o diálogo do macOS "opensims2 quit unexpectedly" (e o do `s2pkg`) continua no centro da tela. Dois jeitos de trabalhar: janela de 600x860 em x=2 (o diálogo fica à direita, mas a barra é cortada) ou a janela cheia (1280x720 em 95,49), onde a barra de Construir/Comprar fica inteira e só o centro-alto da cena é coberto (Construir foi testado assim). Notas de ferramenta: `qa drag` não leva modificador; foi feito um `md` (CGEvent com Ctrl) e depois um toque na tecla Control (`k 59`) solta o Ctrl que ficava "preso" no jogo (provavelmente do próprio `md`). As capturas todas mostravam o diálogo do macOS e foram apagadas (não há screenshot nesta passada). Só parte da lista coube.

| Passo | Resultado |
|-------|-----------|
| Cliques (r14-click-fix): Page Up, armário do andar de cima (obj 519) | passa: pie próprio com "Trocar para..." e o submenu (Roupa Formal / de Ginástica / Pijama / de Banho / Roupas Íntimas), o `s2pkg pie` bate; "Planejar Roupa" não aparece (nem no `--all`) neste armário |
| Cliques: outros objetos de cima | passa: caixa de som (obj 136) abre "Ligar", cama e mesa de cabeceira são pegos como eles mesmos |
| Cliques: telefone do andar de cima (obj 145/253 em 10,5 30,5) e do térreo (46/227) | **não achado**: ficam atrás de paredes do corte e vários cliques caíram em lâmpada/armário/mesa; os diálogos do telefone não foram testados |
| Cliques: chão do andar de cima | passa: "Vir Aqui" (log `go here: destination 171 at (12.40, 25.81, 3)`), nunca o pie do Sim |
| Cliques: gramado | passa: "Vir Aqui", não "Entrar" |
| Andar: Vir Aqui da cozinha até a sala (câmera perto, 8 capturas a 0,1 s) | passa: o Sim vira de costas para a frente, braços balançam, sem T-pose, salto ou deslize visível; de longe vi o Sim descer a escada sem problema |
| Andar: idoso corcunda (Isabella N003_Lot46), Sim Playful saltitando, partida/parada por ângulo | não testado (só N002_Lot10; o Johnny não tem "Vir Saltitando" no pie) |
| Opções: listas "Tamanho da Tela" e "Freqüência de Atualização" | passa: 1280x832, 1470x918, 1470x956 (marcado), 1710x1068, 1710x1112... e "60 Hz" |
| Opções: Esc fecha a barra na primeira vez | passa: um Esc fechou a janela de Gráficos e a barra juntas |
| Construir: piscina só em retângulos | passa: arrasto 3x3 = "a pool of 9 tiles for §675"; um segundo arrasto que daria um L foi recusado ("the pool and the drag must make one rectangle") |
| Construir: Ctrl-arrastar apaga | passa: arrasto grande sobre a piscina = "9 pool tiles erased for §540 back" e a piscina sumiu (ver bug do clique pequeno abaixo) |
| Construir: caixa ciano na borda do lote | passa: nenhuma caixa ciano nas três posições na borda leste e na calçada (só o quadrado verde/vermelho do cursor) |
| Construir: tiles da pista | passa: arrasto de piscina na faixa da calçada/rua = recusado (0x45, "not editable at (13,44)") |
| Construir: arco sem a cerca | não testado |
| Diálogos: Planejar Roupa, espelho/toucador, cabide da loja comunitária, painel de aspiração | não testado (o armário daqui não oferece Planejar Roupa; o painel de aspiração não foi achado: o ícone do lápis ao lado de "Forma Física" abre "Biografia do Sim") |
| Dormir (N001_Lot11), espelho do Lot82 e Reflexo off, lote vazio novo, telhado no Construir, passo 32 do tutorial | não testados (tempo) |
| Desempenho | `OS2_PERF` do app: 4 ms de CPU por quadro em média, pior 28 ms |

## Bugs achados (não corrigidos)

- Ctrl-arrastar (ou Ctrl-clicar) com a ferramenta de piscina sobre a piscina já cavada diz "no pool there" mesmo com o cursor visivelmente dentro dela: o ponto do chão é pego na profundidade do fundo e o quadrado vermelho cai no fundo, mas o tile não bate com o visível (a piscina aparece uns 2 tiles abaixo de onde se arrastou). Um arrasto grande que cobre a piscina inteira apaga. Repro: Construir, Diversos, ferramenta de piscina, arrastar 3x3 no gramado, Ctrl-arrastar 2 tiles dentro da água. Não investigado (`BuildMode::groundPoint` itera `floorHeight` 3 vezes; talvez oscile entre o gramado e o fundo).
- Mensagens da piscina em inglês na dica ("cannot dig a pool: ...", "9 pool tiles erased ...") em vez de PT-BR; só a recusa 0x45 vem do texto do jogo ("Impossível reservar espaço acima das escadas", que é o texto do binário para esse código).
- O telefone e várias coisas no andar de cima ficam escondidos pelas paredes do corte (`walls cutaway`) em `Page Up`: sem girar a câmera não se acha o telefone para clicar; vale uma passada com o telefone achado pelo `s2pkg route`.
- Quadrado do cursor da ferramenta de piscina fica verde sobre a pista e fora do lote (a recusa só vem no arrasto).

# Rodada 15, passada 11

Mesmo método (CGEvent, pid próprio, janela conferida antes de cada clique, captura só da região da janela, `-ApplePersistenceIgnoreState YES`, `build/` do `main` em `97b9447`, binário das 06:16, mais novo que o commit). N002_Lot10 (`--vm --ui live`). Os diálogos do macOS ("opensims2 quit unexpectedly" e o do `s2pkg`) seguem no centro da tela; para ver os diálogos do jogo usei a janela de 620x760 em 2,40 (os diálogos do jogo ficam inteiros, a barra e os painéis laterais ficam cortados) e o `--dialog-demo` (5 serviços, 6 vizinhos, 7 festa, 21 espelho, 17 Planejar Roupa) em vez de achar o telefone. Navegar a câmera com as setas/a/d só gira (não faz pan) e foi lento; o telefone do térreo (obj 46, 15,5/23,5 nível 2) e o de cima (145, 10,5/30,5 nível 3) não foram clicados. Sem screenshots guardados (todos mostravam o diálogo do macOS).

| Passo | Resultado |
|-------|-----------|
| Piscina: cursor vermelho sobre a pista e fora do lote | passa (quadrado vermelho na rua e no vazio; verde no gramado) |
| Piscina: recusas em PT-BR | passa: "Não é possível cruzar objetos", "Local fora dos limites", "Piscinas somente são permitidas sobre o terreno", "Somente terreno exposto pode ser editado" |
| Piscina: Ctrl-arrastar 2 tiles apaga | passa só se o arrasto é no tile onde a piscina foi cavada: ver o bug de posição abaixo; sobre a piscina visível dava "no pool there" |
| Piscina: cavar por arrasto | **falha visual**: a piscina de 2 tiles aparece ~3 tiles mais perto da câmera (70-100 px a 2000 px) do que o quadrado do cursor/arrasto |
| Espelho (`--dialog-demo 21`): fileira de cor da barba | passa: 4 cores aparecem na aba das barbas (a 5ª, "Personalizado", fica fora da janela estreita; não conferida) |
| Telefone: listas de serviços (5), vizinhos (6, grade e lista) e festa (7) | abrem bonitas, botões de ordenar A-Z/humor e lista ordenada por nome; na festa a lista de convidados mostra "Mãe Rainha" duas vezes (do demo) e os nomes longos ficam cortados ("Drone Colono 1000"); os serviços do demo listam objetos do lote, então imagens/nomes de lotes não foram vistos |
| Planejar Roupa (`--dialog-demo 17`) | abre com a roupa do Sim, setas e as 3 abas; não testado no armário real |
| Painel de aspiração "Big Life Moments" | não testado (não achei onde abre) |
| Andar: idosa corcunda, Vir Saltitando, partida/parada | não testado |
| Dormir (N001_Lot11), espelho do Lot82, lote vazio novo, telhado, arco sem cerca, passo 32 do tutorial | não testados (tempo) |

## Bugs achados (não corrigidos)

- Piscina cavada por arrasto aparece deslocada para a câmera. Repro: Construir, Diversos, ferramenta de piscina, escolher o 1º item, arrastar ~30 px num gramado livre do N002_Lot10 (a leste da varanda): "a pool of 2 tiles", mas a água aparece 70-100 px abaixo do arrasto; Ctrl-arrastar no lugar do arrasto apaga (o tile está certo), Ctrl-arrastar na água visível diz "no pool there". Suspeita: o desenho do buraco/água da piscina nova (`lot_model.cpp`, `water:pool`) ou a altura -3 do fundo, não o picking.
- Piscina existente do lote 10: o cursor sobre a água cai ~1 tile abaixo do mouse.
- Câmera: as setas e a/d giram em vez de mover; não achei pan por teclado.

# Rodada 16, passada 12

Mesmo método (CGEvent, pid próprio, janela conferida antes de cada clique, captura só da região da janela, `-ApplePersistenceIgnoreState YES`, `build/` do `main` em `cc8cfe1`, binário das 08:08, mais novo que o commit). Janela 1280x752 em 95,49 e, para as medidas de enquadramento, 620x760 em 2,40 (redimensionada logo depois de abrir). O diálogo do macOS "opensims2 quit unexpectedly" cobriu o centro nas primeiras capturas e sumiu no meio da passada; todas as capturas foram apagadas (não há screenshot guardado). Notas de ferramenta: um `k` (tecla) segura a tecla tempo demais e desloca a câmera (z/x "zoom" derivava a cena); usei um `kh code ms` com tempo de pressão; o Ctrl-arrastar é o `md` com a flag 0x40000, e depois um toque na tecla Control (`k 59`).

| Passo | Resultado |
|-------|-----------|
| Câmera: zoom pela roda em todos os níveis no lote (N002_Lot10, janela estreita e larga) | passa: do zoom padrão (mostra a casa quase toda, com o terreno) até o mais perto sem saltos nem a cena fugindo do centro |
| Câmera: no zoom mais perto um Sim cabe da cabeça aos pés | passa (adulto e criança inteiros, ~200 px de altura numa janela de 760 px) |
| Câmera: janela larga mantém a altura 4:3 | passa (1280x752 mostra o lote inteiro no padrão, sem esticar; o redimensionamento ao vivo logo depois de abrir recompõe) |
| Câmera: bairro, todos os níveis de zoom | passa (a roda leva ao mais perto e volta; a cena fica enquadrada) |
| Câmera: WASD/setas movem, `,` e `.` giram | passa com uma tecla segurada por ~0,5 s (`d` desloca a cena, `.` e `,` giram em sentidos opostos) |
| Sombras: Sim no chão dentro de casa | não conclusivo: não vi sombra sob os pés (tapete marrom escuro; a sombra do jogo é fraca, ver `r15-fov-sombras-sims.jpg`) |
| Sombras: Sim no gramado (varanda/deck) | não conclusivo: sem sombra visível nas capturas de cima |
| Sombras: casa no gramado, N002_Lot10 (10:16, 10:30, 17:47) e N001_Lot82 (08:00, pátio) | **falha (não vi)**: nenhuma sombra do sol da casa, das paredes ou das árvores no gramado nem no pátio, mesmo com as paredes em "up"; a captura `r15-sombra-sol-height-map.jpg` mostra uma sombra grande (com telhado); aqui os telhados ficam desligados ("roof off") |
| Cabelo dos Sims | passa (loiro de adulto e maria-chiquinha de criança, sem partes faltando ou pretas) |
| Dia e noite | passa: tarde clara, noite azulada com janelas acesas e a piscina escura, sem artefatos |
| Piscina: a piscina cavada bate com o arrasto e com o quadrado do cursor | passa (arrasto 100x70 px = "a pool of 8 tiles for §600" e a água aparece exatamente onde se arrastou) |
| Piscina: Ctrl-arrastar na água apaga | passa: o Ctrl-arrastar na água acha a piscina (a recusa "no pool there" só saiu fora da água); apagar um pedaço da piscina recusou com "Impossível excluir a piscina" (restaria uma faixa de 1 tile) |
| Piscina: recusas em PT-BR | passa na tela ("Impossível estender a piscina", "Impossível excluir a piscina"); o detalhe em inglês só vai para o log |
| Piscina: arrasto começando na varanda/fundação | recusa certa (0x86) |
| Sims: família do N001_Lot11 a velocidade 3 (cerca de 1 dia de jogo) | passa/sem travar: saem para o trabalho, voltam, trocam de roupa, o Sim sobe a escada; nenhum erro de VM no log |
| Sims: a velocidade 3 volta sozinha para 1 (depois de "Sim 49 grew into age", N001_Lot11) | visto 2x (também no N002_Lot10, depois do aniversário do Johnny); parece o "Set Simulator Speed" do roteiro (não é bug provável) |
| Sims: Vir Aqui no chão | passa: agora saem "Vir Aqui" e "Vir Saltitando Aqui" (Sim brincalhão) |
| Telefone: chamada que chega ("Ei Jill, Jeanie pode conversar agora?", botões Sim/Não) | passa: diálogo PT-BR com o retrato, botões e o jogo continua depois; as listas de ligar/convidar não foram abertas (o telefone de parede do térreo, obj 46/227 em 15,5/23,5, não foi achado na vista) |
| Dormir (N001_Lot11) | não conclusivo: a cama de solteiro mostra só "Arrumar Cama" e "Relaxar"; `s2pkg pie` mostra "Dormir" (TTA 5) como false para a Cassandra às 16:30 (o teste "Interaction - Sleep - Autonomous - TEST" dá falso); não verifiquei se o jogo esconde com energia cheia |
| Espelho do Lot82, Reflexo off | não testado: o lote 82 aberto assim não tem família ($0, sem Sims), não há quem clique o espelho |
| Lote vazio novo no bairro | não testado |
| Construir: telhado | passa: no Lot82 a ferramenta de telhado tipo 1 arrastada deu "a roof of type 1 over (20,13)-(23,24)" e o telhado aparece |
| Construir: arco sem cerca | não testado (não achei o item depois da troca para o painel de telhado) |
| Tutorial passo 32 | não testado |

## Bugs achados (não corrigidos)

- Sem sombra do sol da casa no gramado nem no pátio (ver a linha da tabela). Repro: `--hood N002 --hood-lot 10 --vm --ui live`, paredes "up", olhar o gramado a leste da varanda às 10:00 e às 17:00; ou `--hood N001 --hood-lot 82`, pátio da piscina às 08:00. O tamanho padrão dos shadows ($High) vem ligado.
- N001_Lot82: o rio/fosso ao lado da casa aparece como um buraco azul-marinho escuro com margem de areia borrada, sem a superfície da água (o lago do `r12-construcao-lago.jpg` era azul claro). Repro: `--hood N001 --hood-lot 82`, olhar o canto nordeste do lote. Pode ser a água escura pela iluminação nova ou profundidade.
- Bairro N002: uma casa ao sul (perto das duas antenas parabólicas) aparece como polígonos pretos soltos (texturas/modelos faltando). Repro: `--hood N002`, vista padrão, a casa no canto inferior.
- Log do lote N002_Lot10 e N001_Lot11: "snapped ...: animation not found: o2a-bookcase-get-middle_anim" (tapete/banquinho/dardos), "effect texture clothesChangeSwirl_clotheschangeswirl_shpe / waterStream: not found", "snap model foodTray: CRES not found", "snap model doll: CRES not found" e Sims de vizinhos sem arquivo de personagem ("Sim 224 (neighbor 308), 151 (310), 126 (311), 229 (312), 238 (363): no character file, not drawn"): vizinhos que aparecem como visitantes não são desenhados.
- Teclas: um toque curto de tecla da ferramenta de teste dispara a câmera por muito tempo; não é do jogo (só do QA).

# Rodada 16, passada 13

Mesmo método (CGEvent, pid próprio, janela conferida antes de cada clique, captura só da região da janela, `-ApplePersistenceIgnoreState YES`, `build/` do `main` em `a7000f3`, binário das 09:26, mais novo que o commit). Janela cheia 1280x752 em 95,49; sem diálogos do macOS (os de antes foram fechados). Desta vez as capturas que passam ficam guardadas em `docs/screenshots/r16-qa13-*.jpg` (reduzidas a 1600 px).

| Passo | Resultado |
|-------|-----------|
| Sentar/comer: Cassandra e Mortimer no N001_Lot11 (Jantar/Sentar na cadeira da mesa) | passa em parte: o Sim vai à mesa, senta de frente para a mesa (cadeira da ponta, torso e cabeça acima da mesa) e depois levanta e sai; `r16-qa13-dining-sit-cassandra.jpg`. Não conclusivo: o "Sentar" do Mortimer à noite mostrou só a cabeça atrás do encosto da cadeira do fundo (`r16-qa13-dining-sit-head-only.jpg`, corpo escondido pela cadeira, não deu para dizer se a cadeira foi puxada); não vi a cadeira se mexer em nenhuma das capturas |
| Dormir: Sim cansado sobe a escada e deita | passa: o Sim (energia vermelha) sobe a escada, vai ao quarto e deita na cama de solteiro com a cabeça no travesseiro (extremidade branca), coberto; `r16-qa13-sleep-bed-full.jpg` e `-crop.jpg`. "Dormir" não aparecia no pie com a energia cheia (certo) |
| Sombras: sol da casa e das paredes no gramado | passa fraco: de dia (Terça 14:49, telhado "off") só há um escurecimento suave junto ao pé da parede sul, visível só com contraste aumentado (`r16-qa13-day-lawn.jpg`); no N001_Lot68 em nível -1 (casa escondida) o gramado mostra a sombra inteira da casa, escura e em degraus de tile (`r16-qa13-pool-level-minus1-shadow.jpg`), então o height map funciona, mas a sombra de dia com a casa visível é quase invisível |
| Sombras: paredes sob a varanda mais escuras | não conclusivo (a varanda do Lot11 não aparece clara nas capturas) |
| Sombra do Sim | não conclusivo: não vi sombra sob os pés dentro da casa nem fora, a esse zoom |
| Fosso do N001_Lot82 com água | passa: água azul clara com margem de areia e a ponte (`r16-qa13-lot82-moat-water.jpg`) |
| Cone de luz da lâmpada à noite | não conclusivo: a noite do Lot11 mostra as luminárias acesas dentro da casa e a luz do poste no gramado (`r16-qa13-night-lawn-lamps.jpg`), mas não achei um spot com cone no chão |
| Câmera do CAS: primeira pessoa nos locators, glide, zoom no rosto e nas roupas | passa: passo 1 de corpo inteiro com o estúdio, espelho e quadros; ao ir ao passo 2 a câmera desliza (3 quadros seguidos, sem salto) para o rosto, perto, e no passo 3 (rosto, sliders) fica no rosto; no passo 5 (roupas) volta ao corpo inteiro (`r16-qa13-cas-step1.jpg`, `-step2-face.jpg`, `-step3-face-sliders.jpg`, `-step5-clothes.jpg`). Textos "Enter First Name" e "Cheek Size" ainda em inglês |
| Piscina: Sim mergulha do trampolim do N001_Lot68 | **falha**: o pie do trampolim mostra "Mergulhar"; o Daniel veste a roupa de banho, a vista muda sozinha para o nível -1 (casa escondida) e ele fica parado em T-pose, sem cabeça visível, na borda do lote (no trampolim?) por mais de 1 minuto de jogo; não mergulhou, nadou nem saiu pela escada. Cinco ícones de fila do mesmo "Mergulhar" |
| Telefone (obj 46 em 15,5/23,5 nível 2 no N002_Lot10) | não testado (tempo) |
| Espelho do Lot82 e Reflexo off | não testado (Lot82 sem família) |
| Lote vazio novo no bairro, arco sem cerca, passo 32 do tutorial | não testados (tempo) |

## Bugs achados (não corrigidos)

- Mergulhar no N001_Lot68 deixa o Sim em T-pose no trampolim para sempre. Repro: `--hood N001 --hood-lot 68 --vm --ui live`, zoom padrão, clicar o trampolim branco (borda leste da piscina, ~(1262,431) em 2000 px) e escolher "Mergulhar" no pie com o Daniel selecionado. A câmera troca para o nível -1 sem pedir. Log: nenhuma "animation not found" do mergulho, só `queued "Mergulhar" (Pool Diving Board - 1,0,4 - Lead, TTA 0)`.
- A vista cai para o nível -1 (`lot view: ... level -1`) quando um Sim vai ao trampolim: some a casa e só sobra o terreno com a piscina.
- O "Sentar" na cadeira de jantar do Lot11 deixa só a cabeça à mostra atrás do encosto em uma captura; conferir se o Sim e a cadeira ficam alinhados (a cadeira não foi vista sendo puxada).

# Rodada 17, QA 14

Mesmo método (CGEvent, pid próprio, janela conferida antes de cada clique, captura só da região da janela, `-ApplePersistenceIgnoreState YES`), binário do ramo `r17-qa14` feito do `main` em `8757a0c`. Janela 1280x752 em 95,49, sem diálogos do macOS. O Mac dormiu (tampa fechada) das 11:20 às 12:30: nesse tempo a sessão ficou bloqueada e o `screencapture` falhava ("could not create image from rect"); para não depender dele foi feito `OS2_LIVE_SHOT=<arquivo.png>` (cada `SIGUSR1` grava o próximo quadro que a janela desenha, cena e UI, no tamanho do drawable). Partes do roteiro foram conferidas sem tela (`--screenshot`) onde a janela não acrescentava nada; a coluna diz qual.

| Passo | Resultado |
|-------|-----------|
| Piscina, N001_Lot68: "Mergulhar" no trampolim (ao vivo) | passa: um só "Mergulhar" na fila (os cinco ícones da passada 13 não se repetiram com um clique só); o Daniel troca de roupa, sobe no trampolim, mergulha e nada na superfície da água, com o corpo inteiro (não há mais T-pose) e a câmera fica no nível 1 com a casa à mostra (`r17-qa14-pool-dive.jpg`, `-pool-swim.jpg`, `-pool-swim-board.jpg`). Não conclusivo: não vi o Sim chegar à escada e sair (a captura parou com a tela bloqueada) |
| CAS: tom do rosto igual ao do corpo, 4 tons x mulher/homem (sem tela, passo 2) | passa: rosto e pescoço do mesmo tom do corpo nos 8 casos (`r17-qa14-cas-face-tones.jpg`) |
| CAS: rostos das pontas dos sliders e textos em PT-BR (sem tela, passo 3) | passa: as abas mostram os rostos das pontas e os títulos em PT-BR ("Mudar Olhos" etc.). **Corrigido:** nas abas sobrancelha/olhos/nariz/boca/queixo a cabeça "alien" era uma célula vazia com o nome em inglês (o retrato feito só existia no passo 2); agora o passo 3 faz o mesmo retrato (`r17-qa14-cas-eyes-alien-shot.jpg`) |
| CAS: câmera desliza e dá zoom | não refeito ao vivo (passou na passada 13) |
| Portas: arcos da frente do N001_Lot82 (ao vivo) | passa: os dois arcos são vãos limpos; o da esquerda tem a porta de madeira fechada inteira no vão, o da direita é aberto e mostra a escada (`r17-qa14-lot82-front-arches.jpg`) |
| Portas: Sim passando por porta no Lot68/Lot11 (ao vivo) | passa em parte: no Lot11 a Sim vai pelo gramado, entra pela porta dupla de vidro (cortinas vermelhas) e chega à sala; no Lot68 o Daniel entra pela frente. Não conclusivo: nas capturas a 0,6 s a folha não aparece aberta (fecha rápido ou fica escondida pelo Sim) |
| Telefone do andar de cima, N002_Lot10 (obj 253, nível 3; sem tela) | passa: pie com Ligar.../Sim, Serviços, Emergência, Entregas, Transporte, Dar Festa... e Convidar Diretor; "Serviços" abre "Números de Serviços" com Garçom, Técnico, Empregada, Babá, Jardineiro, Exterminador; contratar a Empregada dá OK e a NPC vem do arquivo salvo do vizinho 363 (`N002_User00213.package`), que na passada 12 não tinha arquivo |
| Espelho (N001_Lot68, obj 178; sem tela) | passa: pie Praticar Discurso, Praticar Namoro, Dar uma ajeitada, Mudar Aparência; "Mudar Aparência" abre o diálogo com a cabeça do Sim e a grade de cabelos do tom dele; "Personalizado" mostra só os 4 cabelos tingidos (tom 6) (`r17-qa14-mirror-hair-custom.jpg`). Não vi cabelo de tom 0 entre os do homem adulto (careca e chapéus ficam na grade da cor) |
| Lote vazio novo (ao vivo) | passa em parte: Lotes, lote vazio, o retângulo fica verde onde cabe e vermelho na rua; o lote entra no bairro com casa ("Ranch Retreat - 2BR 1BA", N001_Lot83), "À Venda", entrar, Comprar abre com fundos §0; voltar salvando; mudar a família Bonevelho (§10.387) e ela entra no lote com §10.513 (`r17-qa14-new-lot-move-in.jpg`). **Bug corrigido:** a cadeira recusada ("not enough funds") ficou presa ao cursor e o Salvar -> Vizinhança a gravou no gramado; agora o salvar larga o que a ferramenta segura. Bugs abertos abaixo |
| Passo 32 do tutorial | não testado: `--tutorial-auto` depois do clique no menu não avança no app sem tela, e `s2pkg tutorial --auto` para no passo 12/32 (espera um clique no João) |
| Dicas dos desejos (ao vivo) | passa: passando o mouse no 1º desejo aparece "Oba-Oba com Catarina / Desejo: +3500 pontos de aspiração / texto" depois de ~0,75 s (`r17-qa14-want-tooltip.jpg`); a dica some depois de 2,25 s, como no jogo |
| Sofá: "Brincar" (pular no sofá; sem tela) | passa: o Daniel sobe e pula no sofá do Lot68, pés no ar e agachado entre os pulos (`r17-qa14-sofa-bounce.jpg`) |
| Abraço, bebê mamando | não testados (o `--pie` sem tela não abre o menu de um Sim; os lotes usados não têm bebê) |

## Bugs achados (não corrigidos)

- Lote sem família: comprar recusa por falta de fundos (o Comprar abre com §0 no lote novo N001_Lot83). No jogo, construir/mobiliar um lote desocupado não cobra (o custo vai para o valor do lote; lembrança do jogo, não conferido no binário). Repro: bairro N001, Lotes, lote vazio, pôr num gramado, entrar, Comprar, Cadeira de Jantar PatioPlástico, clicar no gramado: "not bought: not enough funds". Conferir no binário o `CanAfford`/`NoCharge` (BV `0x9b38fc`, vtable 200) com a família 0.
- Preço do lote novo: o "À Venda" mostrou §10.857 e o "Mudar família" logo depois §10.387 para o mesmo lote (depois de salvar na saída). Conferir qual valor o jogo mostra.
- `--pie <id de Sim>` sem tela termina sem abrir o menu (o `findInteractionObject`/`anchor` não acha o Sim); para testar sociais sem tela só o `s2pkg pie lote sim alvo` lista.

# Rodada 17, QA 15

Mesmo método (CGEvent, pid próprio, janela conferida antes de cada clique, captura só da região da janela, `-ApplePersistenceIgnoreState YES`), binário do ramo `r17-qa15` feito do `main` (refeito depois de entrarem r17-chair, r17-chair2 e r17-emptylot). Janela 1280x752 em 95,49. Velocidade 3 na maior parte; `OS2_LOG_ANIMS=1` para conferir no log o que cada Sim animou. Famílias: Caixão (N001_Lot11, com o Alexandre criança), dos Prazeres (N001_Lot68, gêmeas adolescentes), Silva (N002_Lot10, aniversário do João) e Malpaga (N001_Lot36, o Beau criança pequena). Cerca de 3 dias de jogo somados.

| Passo | Resultado |
|-------|-----------|
| Carona do trabalho (Lot11, segunda) | passa: o sedã para no meio-fio às 8:05, a Cassandra entra às 8:36 ("job performance 0 -> 16") e volta às 15:07; o dinheiro sobe §511.632 → §512.151 (14:02) → §512.579 (15:06, salário). No Lot68 o Daniel vai e volta (§2.250 → §2.652) |
| Motorista da carona e do ônibus | **bug corrigido**: depois que o carro ia embora, o motorista (Sim 222, "School Bus Driver") ficava sentado no ar no meio da rua, e a cada viagem um a mais (`r17-qa15-motorista-sentado-na-rua.jpg`); a empregada do Lot68 ficou um dia ajoelhada junto da poça e a visita (Don) parada na esquina do lote. A VM já tinha tirado esses objetos; o app deixava o ator na última pose (e com o ID reaproveitado ele seguia outro objeto). Agora o ator sai ("Sim 222 leaves the lot (its object is gone)") |
| Sim no trabalho | **bug corrigido**: o plumbob da Cassandra no trabalho ficava no céu sobre o fim da rua (ela segue no slot do carro que saiu do mundo); agora Sim fora do mundo, ou dentro de algo fora do mundo, não é desenhado nem tem plumbob |
| Criança para a escola | não passa no Lot11: o Alexandre (escola particular) não vai; conhecido (`docs/primitives-mac.md`, Set to Next 18: só a escola pública é aceita). No Lot68 o ônibus (sem a textura da sombra, só no jogo base) chega às 8:00 de quinta; não vi as gêmeas subirem |
| Contas | passa em parte: a conta chega às 10:20 (§466 no Lot11; §406 dois dias depois) e a Cassandra pega o correio sozinha às 15:14 (ao vivo e sem tela). Pagar ao vivo não testado: a caixa de correio quase não pega o clique (ver TODO). Cobrador não alcançado |
| Sociais (N002_Lot10) | passa: o João abraça a Jenny ("Abraçar.../Amigável"), a Jenny beija a filha no rosto, conta piada ao João (gestos, relação ++ e pontos de aspiração) e conversam (balões com ícones; `r17-qa15-piada-abraco.jpg`); no log `a2t-soc-hugFriendly`, `a2t-soc-kissFamily`. Ligação que chega ("Ei 9 de Souza, João pode conversar agora?") aparece e fecha sozinha |
| Aniversário com bolo (N002_Lot10) | passa: o aviso "João está pronto para se tornar um homem!", "Soprar" no bolo, `a2o-bdayCake-blowOut-table`, "Sim 389 grew into age 0x08" e `t-react-growUp-goodMemory`; o João adulto aparece inteiro (`r17-qa15-aniversario-adulto.jpg`) |
| Mesa de jantar (pedido do coordenador, r17-chair/chair2) | passa: Daniel e Angela "Sentar" nas cadeiras do deck: chegam, as mãos vão à cadeira e à mesa ao entrar, ficam sentados de frente para a mesa, levantam sem sentar no ar (`r17-qa15-mesa-sentar-bracos.jpg`); a velocidade 3 também |
| Andar com prato/caneca (Lot68) | **falha**: o Daniel anda inclinado para trás, pés à frente, antes de sentar e depois de levantar carregando (`r17-qa15-andar-inclinado.jpg`); overlays de carregar ficam depois de largar a caneca (ver TODO) |
| Criança pequena (N001_Lot36) | passa: a Brandi "Aninhar" o Beau, pega no colo (`a2p-soc-pickup-carrier`, `p2a-soc-pickUp-carryee`) e abraça (`r17-qa15-crianca-colo.jpg`) |
| Salvar → bairro → recarregar (N001_Lot36) | passa: Esc → "Sim" grava o lote (288 objetos) e o bairro; o bairro abre; o lote recarregado volta às 12:08 com os Sims onde estavam (o Beau junto do berço) e os motivos (`r17-qa15-antes-de-salvar.jpg`, `-depois-de-recarregar.jpg`) |
| Piscina (Lot68) | não passa / não conclusivo: o clique na água abre o "Destination - For Swim Here" só com "Vir Aqui"/"Correr Aqui"; sair pela escada não testado |
| Telefone, visitas | só sem tela: "Ligar.../Sim" abre o "Phone Book - Invite"; ao vivo não achei o telefone de parede na vista |
| Gravidez, bebê mamando, morte, fogo | não alcançados (nenhuma grávida ou bebê nos lotes do jogo base; nada pegou fogo nem morreu) |
| Desejos/medos | passa: rolam e se cumprem ("FULFILS want 4: Brincar Com Vladmir") |

## Bugs achados (não corrigidos)

Ver o item "QA ao vivo (rodada 17, QA 15)" do `TODO.md`: andar inclinado carregando e overlays de carregar que sobram; quatro "*Push Look At" na fila do Daniel; caixa de correio difícil de clicar; água da piscina sem "Nadar Aqui"; Sim no trabalho selecionável; a velocidade volta a 1 na chegada da carona.

## Rodada 18 (`r18-pick`): o que veio da QA 15

Caixa de correio (poste de 2 px, raios de tolerância para objetos pequenos; objetos que a VM cria agora são pegos), contas pagas sem tela, "Entrar" na água da piscina, Sim no trabalho desativado, velocidade que volta a 1 na carona (script do jogo). Detalhes no `TODO.md`.
# Rodada 18, vida sem tela (r18-life)

O que o QA 15 não alcançou em 3 dias de jogo, montado sem tela (`s2pkg pie` com `S2PKG_PIE_ALSO`, `OS2_SEED`, `S2PKG_SHOW_CREATED`, `S2PKG_WATCH`; o app com `--vm-create`/`--vm-queue`) e rodado na VM.

| Evento | Resultado |
|--------|-----------|
| Escola particular (N001_Lot11) | **corrigido**: o Alexander nunca ia (o Set to Next 18 dos JobData de escola achava o JobData pelo ID depois do aluno; `docs/primitives-mac.md` "Set to Next 18"). Agora põe o uniforme, anda até o ônibus às 08:07, "Na Escola" 08:26–15:02, nota 12 → 13, nos dois dias; a Jill (N002_Lot10) também passou a ir (`r18-life-escola-particular.jpg`) |
| Tentar fazer bebê (N003_Lot192, Titânia e Oberon) | passa na VM: os dois "Relaxar" na cama de casal, "Tentar Fazer um Bebê" A e B: convite, aceite, edredom (`a2a-bed-cuddler-playin-*`); 60% de chance ("Pregnancy - Try To Get Pregnant": na semente 8 o "Controller - Pregnancy" nasce). A Jenny (N002_Lot10) não engravida: faltam 73 h para ela envelhecer e o jogo pede 75 ("CT - Young Enough For Pregnancy?"); o Daniel e a Mary-Sue (Lot68): ela recusa (relação). **Falta**: no pie do jogador essas entradas da cama saem escondidas (a guarda "Relax - Cuddle - TEST" chama "Hide Menu" em todo caminho verdadeiro); ver TODO |
| Gravidez → parto | passa: "*Pregnant Half Way" no 2º dia, "*Pregnant Fully" no 3º, cinemática "homebirth", gêmeos, diálogo "Baby Name" (tipo 23, dois nomes), "Titânia recebeu mais 3 dias de folga", família vê o bebê, carrega e acaricia (`r18-life-gravida.jpg`, a barriga no 3º dia) |
| Bebê → criança pequena | passa: o "New Baby" do túmulo de depuração (N002_Lot10) ganha mamadeira e envelhece (dado de pessoa 58: 1 → 2) depois de ~2,4 dias |
| Morte e Ceifador (N001_Lot68, "Die of Starvation" do "Rodney's Death Creator") | passa: o Daniel cai de fome (`a-motfail-hunger`), o Ceifador aparece com fumaça, toca as idles (desapontado, prancheta, celular), a urna aparece onde o Daniel estava, o Ceifador some, memórias de morte, a Mary-Sue chora e "Lamentar" (`r18-life-ceifador.jpg`). "Death by Fire": `a-burn-die` e o mesmo |
| Fogo e bombeiros (N001_Lot11, Fire no fogão com `--create "0x24C95F99@17,20,2!"`) | **corrigido**: o caminhão parava 9 tiles depois do portal e o bombeiro não descia (o Snap da porta caía fora do lote) nem voltava. Agora: pânico, o fogo se espalha, o alarme de fumaça chama, o bombeiro desce, apaga os três Fires, se despede e vai embora (`r18-life-bombeiros.jpg`). Pelo telefone ("Ligar.../Emergência" → "Corpo de Bombeiros") ele vem; sem fogo, dá bronca ("Bronca") e vai embora |
| Contas e cobrador (N001_Lot11) | passa: a conta chega (§490), "Receber Correio" na caixa, "Pagar Contas: § 490" na conta (−490, a conta volta para a caixa); sem pagar: "Aviso de Cobrança" depois de ~3 dias, o cobrador chega de caminhão, leva uma cadeira e um abajur e vai embora |
| Carro "flutuando sobre a cozinha" do N002_Lot3 (aviso do r18-chairs) | não reproduzido; o único veículo daquela hora é o caminhão de bombeiros (fogo na cozinha às ~963 s), e com a correção do percurso ele para na rua x = 4,5 (conferido com captura). Pode ter sido o percurso a mais de antes |

# Rodada 18, QA 16

Mesmo método (CGEvent, pid próprio, janela conferida antes de cada clique, captura só da região da janela, `-ApplePersistenceIgnoreState YES`), binário do ramo `r18-qa16` feito do `main` em `03545ff5` (com r18-carry e r18-pick; o r18-visitfx entrou no `main` durante a QA). Janela 1280x752 em 95,49. Lote N001_Lot68 (dos Prazeres), cinco sessões de cerca de um dia de jogo cada, velocidades 1 a 4, `OS2_LOG_ANIMS=1` (e `OS2_ROUTE_FAILS=1`, `OS2_DEBUG_BLOCK=1` nas últimas). Capturas em `docs/screenshots/r18-qa16-*.jpg`.

| Passo | Resultado |
|-------|-----------|
| Visitas e passantes num dia (build antes do r18-visitfx) | **falha, corrigido no `main` durante a QA**: 10 passantes num dia e meio; quatro ficaram mais de 20 h agindo como moradores: cozinham na geladeira, dormem nas camas, tomam banho de espuma, pegam o correio e entram no carro (a Brandi dos Malpaga dormiu das 23 h às 8 h e fez a cama). O Person Type (dado de pessoa 32) dos vizinhos vinha 0 do lote deles; corrigi aqui pondo 1 para quem não é da família e descartei ao ver que o `d98f8fdc` (r18-visitfx) faz o mesmo pelo binário (o handler de objeto criado, BV `0x4700c8`) |
| Visitas depois da correção (build com a correção equivalente) | passa em parte: 9 passantes num dia, todos vão embora em 1–2 h, nenhum cozinha nem pega fogo; **nenhum é cumprimentado** (ninguém da família vai até eles; ver o convite abaixo) |
| Telefone: convidar um amigo | passa em parte: o pie do telefone de parede tem "Ligar..." e "Convidar Diretor"; "Ligar.../Sim" abre "Números Pessoais" (`r18-qa16-telefone-numeros.jpg`), OK, depois "Quer conversar com um Sim? Convidá-lo para vir para cá?" → Convidar. A Catarina Langerak chega 40 min depois a pé, **toca a campainha e ninguém atende**: ela volta para a calçada ("Go To Front Door" falha com 2 de (33,1; 18,7)) e vai embora ~1 h depois. A segunda pergunta some sozinha em ~6 s (o tempo de 4 s + 40 ms por caractere do `cTSObjectDialog`, do binário). O telefone tem área de clique pequena e a lareira do lado pega o clique |
| Andar inclinado com caneca (mesa do deque) | passa: o Daniel faz café, senta com a caneca na cadeira de jantar, levanta (`a2o-chairDining-getOut-rightHand-fromOut-SE`) e anda ereto; a inclinação só aparece na própria get-out (`r18-qa16-levantar-caneca-andar.jpg`) |
| Caixa de correio e contas | passa: o primeiro clique na caixa dá "Receber Correio"; o Daniel pega as contas e as larga no chão em (16,2; 21,8; 2) (sem mesa no caminho); clicar nelas dá "Pagar Contas: § 432" (`r18-qa16-contas-pie.jpg`), ele as leva de volta à caixa e o dinheiro cai §2.649 → §2.217. As contas no chão são um traço cinza quase invisível (ver TODO) |
| Piscina | **corrigido**: o clique na água dava "Entrar" mas a rota falhava na hora (resultado 10, "altitudes don't match": o destino ficava no andar da água e quem nada anda no fundo, um andar abaixo). Agora o destino vai para o fundo: o Daniel vai ao trampolim, troca de roupa, mergulha e nada até o ponto (`r18-qa16-piscina-entrar-nadando.jpg`); "Nadar Aqui" de dentro roda ("Swim - Random Wander" perto do ponto) e "Vir Aqui" no gramado o faz sair pela escada (`a2o-poolLadder-climbOut`) |
| Sim no trabalho | passa: Daniel e Lilith no trabalho ficam com a miniatura cinza, o clique nela não seleciona e a seleção passa para a Angela (`r18-qa16-sim-no-trabalho-cinza.jpg`) |
| Mesa de jantar com caneca/prato | passa: senta e levanta com a caneca sem sentar ao lado da cadeira; a cadeira volta |
| Porta de perto | passa em parte: a folha branca da porta do quarto de cima aparece aberta com a Lilith no vão (`r18-qa16-porta-impasse.jpg`); não capturei a folha fechando |
| Fogo | visto (build antes do r18-visitfx): fogo do fogão às ~22:40 com a Lilith cozinhando; o lote não tem detector de fumaça e ninguém chama os bombeiros (fiel: moradores só entram em pânico); o fogo passa ao deque e dura ~10 h de jogo; morrem 6 (Angela, Lilith, Daniel e 3 visitas que moravam ali), o Ceifador vem a cada um (`r18-qa16-fogo-f2.jpg`, `-f4.jpg`, `-f6.jpg`). O alarme e o caminhão do `14d49698` não estavam neste build |
| Impasse na porta | **bug**: Daniel e Lilith presos na porta do quarto de cima ~3 h de jogo, os dois em "a-blockedPath-long" (ver TODO) |

## Bugs achados (não corrigidos)

Ver o item "QA ao vivo (rodada 18, QA 16)" do `TODO.md`: ninguém atende a campainha nem cumprimenta passantes; impasse de dois Sims na porta do andar de cima; fogo sem alarme de 10 h; telefone difícil de clicar; contas no chão quase invisíveis.

# Rodada 19, a cama de casal (r19-bed)

O que a rodada 18 deixou na cama e nas filas, sem tela (`s2pkg pie` com `S2PKG_PIE_ALSO`, `S2PKG_ALSO_TRACE`, `OS2_LOG_PUSH`; o app com `--pie-step <objeto>:open`).

| Item | Resultado |
|------|-----------|
| Socials da cama para o jogador (N002_Lot10, Jenny e Pollination Tech) | **corrigido**: o jogo os dá **clicando no parceiro deitado**: a entrada 183 da pessoa, "*Object Menu Subqueue" (imediata, só para o jogador), roda por nome o "CT - Object Menu Subqueue" da cama no objeto que o Sim clicado usa, que acrescenta Acariciar / Beijar / Dar um Amasso / Oba-Oba / Tentar Fazer um Bebê conforme o relacionamento; escolhido, empurra o par A/B nos dois. Nosso pie não rodava a check tree de nome com '*' e perdia os textos de árvore rodada por nome. Sem tela: menu com as 5 entradas, Oba-Oba escolhido → convite, aceite e `a2a-bed-cuddler-playin-*` (`r19-bed-pie-parceiro.jpg`). As entradas 5–17 da cama continuam escondidas no menu da cama, como no jogo |
| Dois Sims para a mesma cama ao mesmo tempo | **corrigido**: os dois se encontravam de frente, paravam um para o outro e o primeiro desistia (14); agora o "bloqueado por mim" vem do locomotor do outro e os dois deitam (teste em `test_hood_vm`). O segundo ainda tenta primeiro o lado ocupado e a árvore da cama o manda ao outro |
| Itens internos repetidos na fila | o "*Stop Morning Sickness" e os "*Get Sick" 6–10 são imediatos (flag 0x04: a regra da rodada 18 já os tira da fila; agora também aninhados); os "*Get Sick" 1–5 têm "Allow Consecutive" e esperam. A fila agora é como o `AddAction` do binário: a nova pula as de prioridade menor que esperam |
| Adolescentes do Lot68 e o ônibus | **do jogo**: o "Get My Riders" do carro/ônibus (`7F4EA230/0x203C`) só leva quem tem humor > 0 (nó 8, motivo 3 do stack object); Angela (−50) e Lilith (−3) ficam, a visitante Meadow (humor positivo) recebe "Ir para a Escola" |
| Nome dos gêmeos | **corrigido**: um nome por bebê (o temp 2 repetia o segundo) |
| Pie: entradas novas | com a regra do '*' corrigida aparecem "Mudar para..." no som, "Entrar" na escada da piscina, "Encorajar..." nos adolescentes (conferir na janela) |

# Rodada 19 QA

Mesmo método (CGEvent, pid próprio, `-ApplePersistenceIgnoreState YES`, captura só da região da janela), binário do ramo `r19-qa` = `main` em `62c23d39`. O `--pivot` é ignorado ao vivo (a câmera segue o Sim); para enquadrar, `OS2_PICK_MAP=<png> OS2_PICK_MAP_STEP=2 OS2_PICK_FIND=46` sem tela diz onde o objeto aparece e se algum raio o acerta, e `--orientation 3 --zoom 4` ao vivo mostra o telefone.

| Passo | Resultado |
|-------|-----------|
| Telefone do N002_Lot10 (obj 46, "Phone - Handset", 15,57/23,46) | passa: é um aparelho pequeno quase no chão atrás do sofá; com `--orientation 3 --zoom 4` (única vista em que um raio o acerta) o clique nele e a ~16 px ao lado abrem o pie do telefone (Ligar..., Dar Festa..., Convidar Diretor), não o da lareira nem o do abajur; `r19-qa-phone-pie.jpg`. Um clique a ~70 px no chão dá "Vir Aqui" (certo). A lareira (obj 929) continua escolhida no corpo (sem tela, `--click`: "Acender Lareira") |
| Portas depois da fusão das camadas (N001_Lot11, porta da frente) | **falha, mas não é da fusão**: a porta abre quando a Cassandra entra e depois **fica aberta para sempre** (mais de 1 min real, com o lote a velocidade 3; passou entre fechar e reabrir no mesmo quadro?: nos quadros a porta aparece fechada, aberta com a Sim no vão, e depois aberta sem ninguém). O mesmo com `OS2_NO_LAYER_MERGE=1` (`r19-qa-door-open-merge.jpg`, `r19-qa-door-open-nomerge.jpg`). Só vi um ciclo de abrir; não vi o fechar |
| Cadeira de jantar sentar/levantar | não testado (orçamento) |
