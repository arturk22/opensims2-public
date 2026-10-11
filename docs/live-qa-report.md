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
