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
