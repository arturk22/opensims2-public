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
