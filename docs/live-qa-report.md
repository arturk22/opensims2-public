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
