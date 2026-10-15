# UI do modo vivo: o que o binário de Mac desenha em código

Fontes e convenções como em `docs/shaders-mac.md` e `docs/camera-ui-mac.md`
(endereços do BV 1.10 ppc; "base" = `sims2_base_1.0.ppc`). Este documento
cobre o que **não** está nos UI scripts de `ui.package`: cores, faixas,
amostragem, ícones, velocidades do jogo. O layout das janelas (posições,
IDs, imagens) vem dos scripts e as classes abaixo só encontram filhos por ID.

Chaves de recurso citadas: `ui.package` = `TSData/Res/UI/ui.package`.
Imagens são do tipo `0x856DDBAC`; UI scripts do tipo `0x00000000` grupo
`0xA99D8A11` (os "Constants Table" são `GZWinTextEdit` com
`caption="kNome=valor"`, lidos por `cTSUIPropertyTable`).

---

## 1. Velocidade do jogo e relógio (binário, `cTSSimulator`, `cTSMainToolbar`)

### 1.1 Velocidades

- Valores do simulador (string do console `simSpeed`): **0 = pausado, 2 =
  médio, 3 = rápido, 4 = ultra**. 1 não é usado pela UI. O pause é um flag
  à parte (`cTSSimulator+0x46`, `Pause`), a velocidade fica guardada
  (`+0x44`) e volta ao despausar.
- Cada velocidade dá uma escala ao relógio do scene graph
  (`OnSimulatorSpeedChange` → `cTSSGSystem`), lida de
  `TSData/Res/Config/TS2Config.txt`:
  `AnimSpeedMedium 1.0`, `AnimSpeedFast 3.0`, `AnimSpeedUltra 21.0`
  (`cTSSGSystem::ConfigParserUpdate`, BV `0x7b38b4`). As animações correm
  nesse relógio escalado, portanto também ficam 3×/21× mais rápidas.

### 1.2 Ticks por segundo (`cTSSimulator::Simulate`, BV `0x49c244`; base `0xb2162c`, igual)

```
backlog += Δ(relógio escalado)          (ms; começa em 42 ao (re)iniciar)
pendentes = backlog / 42
n = média(ticks dos últimos 10 quadros) + ceil(pendentes · 0,25)
n = clamp(n, 0, 125);  em ultra (4): n = 125 e backlog ≤ 5250 (= 125·42)
pausado (ou sim parado): n = 1, tick sem avançar o relógio
laço: até n ticks ou 250 ms de tempo real neste quadro;
      cada tick que avança o relógio consome 42 do backlog
```

- **Um tick = 42 ms do relógio escalado.** Em velocidade média são
  **≈23,8 ticks/s** (1000/42); rápido ≈71,4; ultra ≈500, na prática limitado
  por 125 ticks por quadro e 250 ms de CPU por quadro.
- Cada tick avança o segundo do jogo em **2** (`SimulateOneTick`, BV
  `0x49a814`: `+0x30 += 2`, minuto ao passar de 59) → **30 ticks por minuto
  do jogo**, ou seja, **1 minuto do jogo = 1,26 s** reais em velocidade média
  (não 1 s). Hora `+0x24`, minuto `+0x2e`, dia da semana `+0x68` (dia mod 7).
- Ao mudar a velocidade, o histórico de 10 quadros é reescalado pela razão
  nova/antiga (para não dar um salto).

**Diferença com o código atual**: `src/app/lot_vm.hpp` roda a VM a 30
ticks/s ("um minuto do jogo por segundo"). O jogo roda a ≈23,8 ticks/s em
velocidade normal; 30 ticks por minuto do jogo está certo.

### 1.3 Botões de velocidade (`cTSWinProcLivePanelBase`, BV `0xa516a4`/`0xa5191c`)

| ID do botão | ação | marcado quando |
|---|---|---|
| `0x27D7258F` | alterna pause (`toolbar->SetPaused(!pausado)`) | pausado |
| `0x57E01CC1` | velocidade 2 | velocidade == 2 |
| `0x37E01CC3` | velocidade 3 | velocidade == 3 |
| `0x47E01CC5` | velocidade 4 | velocidade == 4 |

`cTSMainToolbar::SetSimulatorSpeed(v, despausar, som)` (BV `0x1ad7e0`): se
já está em v e pausado, só despausa; senão despausa (se pedido) e manda a
mensagem `0x4B46AEBF` com v. Som ao mudar (só se não pausado): um evento por
velocidade (`PTR 0x02788c94` v2, `0x02788c48` v3, `0x02788c38` v4); pausar
toca `0x02788c24`. Pausar manda a mensagem `0xCBCCDECE`.

Indicador de pause (`cTSUIPauseIndicator`): carrega o UI script
`0x8C364B93`, cujas 8 peças (IDs `0x6C364770`..`0x6C364777`) formam a moldura
na borda da tela; `SetMode(1)` liga o flag 2 da janela (variante).

### 1.4 Relógio (`cTSWinProcLivePanel::onTimeChanged`, BV `0x266ab0`)

Texto da janela do relógio = serviço de formato de hora (locale: 12/24 h)
com a hora (global 0) e o minuto (global 5) do simulador, e uma linha com o
nome do dia da semana (global `0x22` + 1, string do jogo). Não há ponteiro
desenhado em código.

---

## 2. Dinheiro (`cTSWinProcUCP::onFundsChanged`, BV `0x1a0a7c`; formatador BV `0x8c62b0`)

`texto = FormatMoney(fundos da família, símbolo "§")` pelo serviço de
formatos do locale (separador de milhar do idioma); se o serviço não existir,
`"$%d"`. Atualizado na mensagem de mudança de fundos, não por tick.

---

## 3. Barras de motivos

### 3.1 Painel (`cTSWinProcPanelMotives::onAttach`, BV `0x26fe68`)

Uma janela-grupo por tipo de Sim, e dentro dela uma barra por motivo
(IDs dos filhos → motivo, números do STR# 0x86 / `motive::` em
`src/vm/motives.hpp`):

| filho | motivo |
|---|---|
| `0xA1` | 7 fome |
| `0xA2` | 6 conforto |
| `0xA7` | 8 higiene |
| `0xA3` | 9 bexiga |
| `0xA4` | 5 energia |
| `0xA5` | 15 diversão |
| `0xA6` | 14 social |
| `0xA8` | 13 ambiente |

Grupos: `0xA003` bebê (sem conforto e ambiente), `0xA004` criança pequena
(sem ambiente), `0xA005` demais humanos (os oito); `0xA006`/`0xA009`
(servo, plantsim) e `0xA007`/`0xA008` (animais) são de expansões. Só o grupo
do Sim selecionado fica visível (`RefreshMotives`).

### 3.2 Uma barra (`cTSWinProcMotiveBar`, BV `0x26ea24`..`0x271ebc`)

- Valor: `motivo` (float da pessoa) truncado para inteiro.
- Preenchimento: um `cTSWinProcGradientFillBar` (CLSID `0x4BDF049E`, §5) no
  filho 7 da barra, faixa **−100..100**.
- **Setas de tendência**: a cada **500 ms** (só com a barra visível) guarda
  o valor; mantém os 10 últimos; guarda também a média corrente deles
  (arredondada). `d = média mais nova − média mais velha` (≈5 s de janela),
  arredondado:

  | d | setas |
  |---|---|
  | 0 | nenhuma |
  | ±1..2 | 1 |
  | ±3..4 | 2 |
  | ±5 ou mais | 3 |

  Positivo mostra os filhos 3, 2, 1 (nessa ordem); negativo, os filhos
  4, 5, 6. Trocar de Sim limpa o histórico.
- Caption/tooltip: tabela de strings de instância `0x82` (chamada com
  `0x7F96C284`, `0x2026960B`), linhas por motivo; ex. energia → `0x1D`/`0x1E`; bexiga com dado de
  pessoa `0xBA` 1–2 → `0x25`/`0x26`, 3 → `0x23`/`0x24`).

---

## 4. Humor

### 4.1 Medidor de humor do painel (`cTSWinProcLivePanel::onMoodChanged`, BV `0x262f90`)

Uma barra de preenchimento (gradiente, §5) com o **motivo 3 (humor)**,
−100..100. Duas variantes (painel aberto/fechado: `+0x138`/`+0x13c`), só a
visível é atualizada. Clicar mostra o `cTSWinProcMoodPopup` ancorado na
janela `0x4906480E` (ou `0x4906480D` no painel fechado), com **500 ms** de
atraso.

### 4.2 Cor do humor no seletor de Sims (`cTSWinProcSimChooserThumbnail::UpdateMood`, BV `0x298dbc`)

```
m   = clamp(humor, −100, 100)            (motivo 3, inteiro)
idx = m + 100                            (0..200)
y   = H − round(H · idx / 200), limitado a H−1     (H = altura da imagem)
cor = pixel (0, y) da imagem de gradiente de humor
```

Imagem: `ui.package` tipo `0x856DDBAC`, grupo `0x499DB772`, instância
**`0xE9100002`** (topo = humor +100, base = −100). A cor tinge o fundo do
retrato (filho `0x1000` da miniatura). Se a aspiração do Sim passa de 74
(platina, `> 0x4A`), o humor não é pintado e o ícone `0x9000` aparece.

### 4.3 Seletor de Sims

- Clique esquerdo numa miniatura de outro Sim: seleciona esse Sim. Clique
  no Sim já selecionado: centraliza a câmera nele (inferido: chamadas do
  toolbar `0x94` e `0xA0(0,0)`).
- Clique direito: liga a câmera seguindo o Sim (`toolbar 0xA0(Sim, 1)`);
  de novo no mesmo Sim, desliga (`0xA0(0, 0)`).
- Botões dentro da miniatura: IDs `0x2000` e `0x3000`.

---

## 5. A barra de gradiente (`cTSWinProcGradientFillBar`, BV `0x35ff04`..`0x3627bc`)

É o que pinta motivos e humor. Não há cores no código: elas vêm de imagens
filhas da janela da barra.

| filho | papel |
|---|---|
| 3 (imagem) | máscara alfa da barra; uma cópia é o buffer de saída |
| 4 (imagem) | **gradiente** (tabela de cores por valor) |
| 5 (imagem) | quadro de animação (opcional) |
| 1, 2 (botões) | −/+ (sliders, ex. personalidade); sem uso nos motivos |
| 6 | "Constants Table": `kAnimationDuration` (padrão 1000 ms), `kFillType` `vertical`/`horizontal` (BV; o base não tem `kFillType`) |

Faixa padrão 0..100 (os motivos usam −100..100). Para um valor v:

```
i = round(N · (v − min) / (max − min)),  limitado a N−1
modo "horizontal" (padrão): N = altura do gradiente; cada linha da saída
    recebe a linha i do gradiente
modo "vertical": N = largura; cada coluna da saída recebe a coluna i
depois: aplica a máscara alfa (filho 3)
```

Ou seja, **a barra inteira é uma linha (ou coluna) do gradiente escolhida
pelo valor**: o comprimento e a cor do preenchimento estão desenhados na
própria imagem do gradiente (uma linha por valor). Com animação, ao mudar
de valor interpola linearmente de v antigo a v novo em `kAnimationDuration`
ms e rola o quadro de animação (1 pixel a cada 33 ms, para o lado da
mudança).

---

## 6. Fila de interações (`cTSUIActionQueue`, `cTSWinActionQueueIcon`)

### 6.1 Layout (`cTSUIActionQueue::Initialize`/`OnTick`, BV `0x1e38f0`/`0x1e44b0`)

Constantes: UI script `0xCCD02691` → `kLocationX=50`, `kLocationY=10`,
`kIconMarginX=4` (padrões no código: 60, 10, 5). Pool de 12 ícones.

Ordem, a cada tick, do Sim selecionado: interação **em execução**, sub-
interação em execução, depois a fila principal e a fila de sub-interações
(só as interações marcadas como visíveis na fila). Posição:

- ícones de nível superior lado a lado: `x = kLocationX`, depois
  `x += largura + kIconMarginX`, `y = kLocationY`;
- sub-interações empilhadas para baixo em `x = kLocationX`: `y += altura`
  do ícone, a primeira também `+= GetSubActionOffset` = topo da janela
  `0x200` − base da `0x100` no script `0x0609A1C0` (78 − 82 = **−4**);
- conector (`SetHasConnectedIcon`, só quando há ícones dos dois tipos): o
  primeiro ícone de nível superior e as sub-interações depois da primeira
  mostram o filho 2 em vez do 1 (nível superior "w/Nest cutout"; aninhada
  "mid level").
- Animação dos ícones: o script tem `kAnimationSteps=4`, `kAnimationDelta=1`
  (padrões 6 e 1 no código, `Initialize` BV `0x9d66d4`).

O layout de um ícone vem do UI script `0x0609A1C0` (filhos: 3 imagem do
ícone; 4 "cancelado"; 5 moldura de "em execução"; 7 marca extra; 1, 2
botões; janelas `0x100`/`0x200` = variante normal/sub-interação, cada uma
com a máscara circular no filho 3).

### 6.2 Ícone (`cTSWinActionQueueIcon::Refresh`, BV `0x9d6bc4`)

- Fonte: o ícone da interação (o objeto alvo, ou o Sim alvo pelo seu
  retrato: miniaturas do serviço de thumbnails), recortado pela máscara
  circular (`ApplyAlphaMask`). O objeto é o alvo da interação (vtable
  `0xfc`); um par (vizinho, ...) da vtable `0x124` faz usar a miniatura do
  Sim (`GetSimThumbnail`). Objeto de várias peças: a miniatura é a da peça
  líder (`GetObjectThumbnail(cITSObject*)`, BV `0x29e780`).
- **Chave da miniatura de objeto** (`cTSUIThumbnailService::
  GetObjectThumbnailGUIDAndModelName`, BV `0x2a0ba8`; `GetThumbnail`
  `0x2a53fc`): tipo `0xAC2950C1`, grupo do serviço (`+0xa0`; `+0xa8` com
  materiais trocados), instância = `FUN_00120ff8(texto, minúsculas)` =
  **CRC32** (polinômio `0x04C11DB7`, MSB primeiro, início `0xFFFFFFFF`,
  sem xor final) do texto `"<grupo do OBJD em decimal com sinal><nome do
  modelo>"` (+ os nomes dos materiais trocados, se houver); o nome do
  modelo é a entrada do STR# `0x85` do grupo indicada pelo OBJD (o campo
  "model index", offset `0xD0`); `"embedded"` usa a miniatura embutida.
  Ex.: compactador de lixo, grupo `0x7FBBE64F` → `"2143020623trashCompactor"`
  → `0x4C413C51`. Em `UserData/Thumbnails/ObjectThumbnails.package` (grupo
  `0x6C2A22C3`, um cache que o jogo gera ao renderizar o objeto; JPEG com
  ALFA), 531 das 532 entradas batem com algum OBJD. Sem miniatura em
  cache o jogo renderiza o objeto (`cTSUIThumbnailFactoryObject`); aqui o
  ícone fica vazio.
- Filho 7 ("icon background") só aparece atrás de miniatura embutida
  (`Refresh`: `local_58`); sem miniatura, só a moldura.
- O quadro "active" (filho 5) aparece na interação/sub-interação corrente,
  **menos** quando o ícone está destacado (filho 6, mouse em cima).
- Desaturado (cinza) enquanto a interação ainda não começou (o Sim não
  confirmou/está na fila); colorido quando é a que está rodando (inferido do
  teste `0x1F0` do Sim; nome da função de cinza não confirmado).
- Moldura "em execução" (filho 5) quando é a interação/sub-interação
  corrente do Sim; filho 4 quando já foi cancelada.
- Animação de posição: a mesma suavização do §1.2 de
  `docs/camera-ui-mac.md` (`kAnimationSteps` padrão 6, `kAnimationDelta`
  padrão 1), por eixo, com Δt em ms.

### 6.3 Cancelar (`CancelAction`, BV `0x9d5ea8`)

**Clique esquerdo ou direito** no ícone cancela (botão pressionado, não
solto). Só uma vez: pede ao Sim para cancelar a interação pelo ID
(`person->CancelInteraction(id)`, vtable `0x1EC`); se aceitou, marca o
ícone como cancelado, toca o som `PTR 0x0278a368` e avisa o toolbar. A
interação some quando a árvore termina (o Sim decide quando parar). Ao entrar
um ícone novo na fila toca o som `PTR 0x02788cec`.

## 7. Painéis do modo vivo (rodada 7, binário + dados)

As strings citadas são de `TSData/Res/Text/Live.package` (o grupo global
`0x7F96C284` nos caminhos do binário). `FUN_000da4b0(buf, STR#, índice, grupo)`
usa o **índice a partir de 1**, como o `tipres` dos scripts.

### 7.1 Relacionamentos (`cTSWinProcPanelRelationships`, `cTSWinProcRelationshipThumbnail`)

- **Dados** (`cTSUIBridgeRelationshipInfo::Init`, BV `0x1807b8`), lidos do
  SREL de mim → outro: variável 0 = **diário** (STR, `+0xc`), 2 =
  **vitalício** (LTR, `+0x10`), 1 = **flags** (`+0x24`), 3 ≠ 0 = "já se
  conhecem" (`+0x2c`), "da família" pelo gerente de famílias (`+0x2d`), |LTR|
  > 4 (`+0x2e`), dado de pessoa 0x44 bit 0 do outro (`+0x30`), e dois valores
  do vizinho (`vtable 0xb4`, 1/0) e dois floats (`0xb8`/`0xbc`, o 2º é o
  "amizade" do ícone diário).
- **Bits da variável 1** (pelos getters da vtable e pelos ícones): 0 paixão
  (crush), 1 amor, 2 noivos, 3 casados, 4 amigo, 5 melhor amigo, 6
  namorando, 7 inimigo. Os bits 9–13 e 15 têm getters, mas o painel base não
  os usa.
- **Ícones**: uma tira de **11 quadros** (largura/11), com tooltip do STR#
  0x84 do `Live`. Quadro 0 amor ("Love"), 1 paixão ("Crush"), 5 casados
  ("Married/Joined"), 6 noivos ("Engaged"), 7 amigo ("Friend"), 8 inimigo
  ("Enemy"), 9 melhor amigo ("Best Friend"), 10 namorando ("Going Steady").
  - ícone do **diário** (`UpdateSTRUI`, BV `0x3a2080`): paixão se bit 0;
    o segundo ícone é amigo (7) se o bit 4 está ligado ou o float de amizade
    passa de **0,001**; senão some;
  - ícone do **vitalício** (`UpdateLTRUI`, BV `0x3a228c`), por prioridade:
    casados (bit 3) > noivos (2) > namorando (6) > amor (1). O segundo: melhor
    amigo (5) > inimigo (7);
  - com pet envolvido (só BV) os ícones somem.
- **Barras**: o valor −100..100 de cada variável vai para uma barra
  (`vtable 0x20`) e para a cor (`FUN_008c0488`, a mesma rampa das barras de
  motivo, §3.2).
- **Filtros** (`LoadRelationships`, BV `0x3a4140`, botões `0x0BA16775…79`):
  1 = família (`+0x2d`), 2 = conhecidos (`+0x2c`), 3 = amigos (bit 4 ou 5,
  ou amizade > 0,001), 4 = um valor > 4 (`0x78`; química, só BV), 5 = todos.
  Ordem: `cFriendshipSortPredicate` (no filtro 4,
  `cChemistrySortPredicate`). Sims mortos e sem SREL (vtables `0x64`/`0x68`
  do info) não entram.
- Tooltip do retrato: "%s %s" + a relação familiar ("%s %s is the %s of
  %s.", STR# 0x84 #19), "Family Tree of %s" (#33).

### 7.2 Emprego (`cTSWinProcPanelJob::ShowJobInfo`, BV `0x24679c`)

Janelas por ID (script do painel de emprego):
- `0x77E74B47` título do cargo ("%s|%s", STR# 0x89 #18 = título|descrição);
- `0x27E74B64` horário: "%s to %s" (#11) com início e início+duração, e
  **a carona = 1 hora antes do início** (início 0 → 23 h), no tooltip "Hours|
  %s-%s\rCarpool arrives: %s" (#13; escola: #17 "Bus arrives");
- `0x47E74B6D` salário (o mesmo formatador de dinheiro do §2);
- `0xEC2CFCFD` desempenho: se o dado de pessoa 0xC3 = 0, valor do desempenho
  ÷ 100, arredondado e ≥ 0, com a cor da rampa e o texto do STR# 0x88 #12;
  senão mostra o nome de um item do gerente de carreiras;
- amigos da família: mostrado só se tem menos que o exigido (`0x104 <
  0x108`): "( + %s )" (#20) com os que faltam; "Job Performance|…" (#21) é o
  tooltip.
- Dias de trabalho (`ShowDaysInfo`), adolescente/escola/idoso
  (`ShowTeenInfo`/`ShowSchoolInfo`/`ShowElderInfo`) e aposentadoria usam as
  entradas #23/#24/#28 do mesmo STR#.

### 7.3 Personalidade e habilidades

- `cTSWinProcPanelPersonality::UpdatePersonalityBar` (BV `0x37389c`) põe o
  dado de pessoa da personalidade (2 gentil, 3 ativo, 5 brincalhão, 6
  extrovertido, 7 arrumado; o 4 não é usado) numa **barra em degraus**
  (`cTSWinProcStepFillBar`); os limites e o número de degraus vêm da barra.
- `cTSUISkillsCtrl::UpdateUI` (BV `0xa86188`): **dados de pessoa 9 a 18**,
  um por barra em degraus (9 limpeza, 10 culinária, 11 carisma, 12 mecânica,
  15 criatividade, 17 corpo, 18 lógica; 13/14/16 são "xxxi" sem uso e o script
  os esconde).
- **Barra em degraus** (`cTSWinProcStepFillBar::ComputeValue`/`RefreshFillBar`,
  BV `0x2be900`/`0x2bef40`): valor preso a [mín, máx] (`+0x68`/`+0x6c`)
  mapeado linearmente na largura da imagem e **arredondado para o degrau**
  (largura / nº de degraus `+0x78`). **Inferido**: personalidade e habilidade são 0..1000 em 10 degraus. Horizontal (`+0x5c` = 0) ou vertical (1, invertida com `+0x60`
  = 3).

### 7.4 Desejos e medos (`cTSWinProcPanelWantsAndFears`)

O painel tem subpainéis por quantidade de slots (IDs `0x2E9E02BA`,
`0xCE9C9E38`, `0xEE9C9E55`, `0xCE9C9F29`, `0xCE9C9F2D`, `0xCE9C9F39`,
`0xCE9C9F40`) e anima a troca (esconde um e mostra outro: `HideWFSubpanel`/
`ShowWFSubpanel`). No jogo base, **4 desejos e 3 medos**. Os slots
extras e o cadeado (`LockWant`, `CheckForTooManyLocks`) são só BV. O som
de brilho é `ui_wf_want_glow`/`ui_wf_fear_glow`, e o caça-níquel da troca é
`kAnimationSound=ui_wf_slotmachine_lp`/`kEndSound=ui_wf_slotmachine_stop` (constantes do
script).

### 7.5 Ícones da fila e miniaturas (`cTSWinActionQueueIcon::Refresh`, BV `0x9d6bc4`)

- O objeto do ícone é o **"action icon" da interação** (`vtable 0xfc`: o alvo,
  ou o que a árvore pôs com o Generic Sims Call 2 "set action icon to stack
  object"), mais um par (GUID, ?) da interação (`vtable 0x124`).
- Com GUID válido (≠ 0, −1): miniatura do **seletor desse GUID**
  (`GetObjectThumbnail(seletor)`). Senão: miniatura do objeto do ícone pelo
  serviço (`vtable 0x78`; Sims → retrato). É o mesmo serviço e cache das
  miniaturas do catálogo (`docs/buy-build-mac.md` §1.5): renderizadas na hora
  com o rig `UIObjectThumbnails` e guardadas em
  `Thumbnails/ObjectThumbnails.package` (imagem `0x856DDBAC`, chave da fábrica
  `0xAC2950C1`). Quando o par muda, o ícone é refeito.
- A máscara redonda é a imagem do filho 3 (`0xC12CEA13`, `ApplyAlphaMask`).
  Os quadros 1/2/4–7 da moldura vêm do mesmo script.

### 7.6 Tooltips (`cGZWinToolTipMgr`, BV `0x91f360`…)

Um temporizador de **750 ms** (`0x2EE`). Ao entrar num controle com tooltip,
a contagem zera: aparece no **1º tique (0,75 s)** e some no **3º (2,25 s)**.
Com a flag 1 do controle aparece na hora, e com a flag 2 não some pelo tempo.
Mensagens `0x22C010D3` (mostrou) e `0x22C010D4` (escondeu).

**Visual (rodada 13, inferido).** A janela da dica não é um script: o jogo a
desenha em código, com as fontes `HoverTipTitle` (12, título) e `TooltipText`
(10, corpo) do `FontStyle.ini`. `drawGameTooltip` (`app/ui_panel_util.cpp`)
faz uma caixa azul-clara com borda e texto azul-escuros, o título acima do
corpo (quebrado em 260 px), a 12 px à direita e 18 px abaixo do mouse; é a
mesma das dicas do modo vivo, do bairro (a recusa de decoração) e dos
painéis. `r13-tooltip-jogo.jpg`.

### 7.7 Sons dos botões (dados + binário)

Cada botão do script tem **`btnclicksnd={0,0xFFxxxxxx}`** (e às vezes
`btnupsnd`). Os 24 bits baixos são o **CRC24 do nome do som**, o mesmo do
índice de recursos: `ui_tertiary_button` 623 botões, `ui_secondary_button`
517, `ui_primary_button` 236, `ui_pane_primary` 18, zoom in/out 7 cada,
velocidades `ui_button_ffw/fw/play/pause`; `btnupsnd` = 
`ui_text_notification_close`, `ui_secondary_button`, `ui_button_pause`.
`nTSUI::PlaySound(nome)` (BV `0x8c1254`) toca `0xFF000000 | CRC24(nome)`
pelo gerente de som (`vtable 0x28`, tipo 1). As listas tocam
`ui_secondary_button` ao escolher uma linha. Há 82 nomes `ui_*` no binário
(`ui_queue`, `ui_queue_delete`, `ui_panel_open/close`, `ui_error`,
`ui_undo/redo`…).

**No código**: os painéis, tooltips e sons estão em `src/app/live_ui.*` e `src/app/live_panels.*` (§8).

---

## 8. Subpainéis no app (`cTSWinProcLivePanel::CreateAndAddSubPanel`, BV `0x262d04`)

| nº | script | classe | botão (`ButtonHolder`) |
|---|---|---|---|
| 0 | `0x49064905` | `cTSWinProcPanelMotives` (necessidades) | `0x0D8AB291` |
| 1 | `0x49065002` | `cTSWinProcPanelRelationships` | `0x17DEFBF5` |
| 2 | `0x49065003` | `cTSWinProcPanelJob` + `cTSUISkillsCtrl` | `0x57DEFBF4` |
| 3 | `0x49064820` | `cTSWinProcPanelSimology` (personalidade, interesses, lembranças) | `0x47DEFBF2` |
| 4 | `0x49064800` | `cTSWinProcPanelRewards` (recompensas de carreira; as de aspiração são de expansão) | `0x8B85BA4E` |

### 8.1 Relacionamentos

- Miniatura: script **`0x49065005`** (`CreateThumbnail`, BV `0x3a4cf8`),
  6 colunas × 1 linha recolhido (2 linhas expandido; "Constants Table"
  `0x3000`), na janela de grade `0x1006`; páginas com `0x1004`/`0x1005`.
- Filtros (`LoadRelationships`, BV `0x3a4140`): botões `0x0BA16776` = 1
  só da família (casa), `0x0BA16779` = 2 parentes e família, `0x0BA16778` =
  3 amigos, `0x0BA16777` = 5 todos; o atual fica pressionado. Ordem:
  `cFriendshipSortPredicate` (aqui: permanente, depois diário, do maior;
  inferido); filtro 4 ordena por química (expansão).
- Uma miniatura (`onAttach` BV `0x3a2bc4`): `0xA3` retrato, `0xA4` botão
  (moldura), `0xA1`/`0xA2` barras de gradiente do diário/permanente
  (−100..100, máscara no filho 3, gradiente no 4), `0xA9`/`0xAA` os números,
  `0xAB` ícone de família, `0xA5`–`0xA8` ícones: quadros da tira
  `a9300037` (11 quadros de 15 px; `SetRelationshipIcon` BV `0x3a1c1c`,
  textos do STR# `0x84`, refs com base 1): 0 amor, 1 paixão, 5 casados, 6
  noivado, 7 amizade, 8 inimizade, 9 amizade especial, 10 namoro firme.
  `UpdateSTRUI`: `0xA5` paixão; `0xA6` amizade (amigo, ou o diário passa de
  um limiar não lido; aqui só o bit de amigo). `UpdateLTRUI`: `0xA7` casados
  > noivado > namoro > amor; `0xA8` amizade especial > inimizade.
  Bits do SREL: 0 paixão, 1 amor, 2 noivado, 3 casados, 4 amigo, 5 melhor
  amigo, 6 namoro, 7 inimigo. Dica: o nome do Sim (`UpdateTooltip`).

### 8.2 Habilidades e carreira

- Grupo por idade: `0xA1` bebê, `0xA2` criança, `0xA3` adolescente (`0xB1`
  escola sem emprego, `0xB3` emprego), `0xA4` adulto (`0xA5` sem emprego),
  `0xA9` idoso (`0xC2` aposentado, `0xC3` emprego).
- Emprego: GUID do objeto de emprego = dado de pessoa 89 | 90 << 16 (ex.:
  Cassandra → `0x0C9EBD47` "JobData - Adult - Science"), nível = 57,
  desempenho = 63 (barra `0xA7`, gradiente por colunas, −100..100), folga =
  69 / 100. Do objeto: título = STR# `0x93` entrada 2·nível − 1 (as pares são
  as descrições), salário BCON `0x1000`[nível], início `0x1001`, horas
  `0x1002`, amigos da família `0x1003` (o "( %s )" mostra o do próximo
  nível; inferido), dias `0x101A` (bit 0 = domingo; inferido). Textos do STR#
  `0x88`/`0x89` ("Dias de folga: %s", "%s até %s").
- Habilidades (`cTSUISkillsCtrl::Init` BV `0xa85e00`): barras 0..1000 em 10
  divisões; filho 3 = a barra cheia, cortada. Janelas pelas legendas:
  `0x4BDEFF01` culinária (dado 10), `02` mecânica (12), `03` carisma (11),
  `04` físico (17), `05` lógica (18), `06` criatividade (15), `07` limpeza (9).

### 8.3 Simologia

- Abas `0x10000000` personalidade, `0x20000000` interesses, `0x30000000`
  lembranças (botões `0x40000000`/`0x50000000`/`0x60000000`).
- Personalidade (`CreatePersonalityFillBar` BV `0x374de4`,
  `UpdatePersonalityBar` `0x37389c`): 0..1000 em 10 divisões, a barra = o
  valor atual, o contorno `0xAA` = o original. `0x1001` ordeiro (dado 7,
  original 47), `0x1003` sociável (6/50), `0x1005` ativo (3/49), `0x1004`
  brincalhão (5/51), `0x1002` gentil (2/48).

### 8.4 Indicador de pausa (`cTSUIPauseIndicator::SetArea`, BV `0x96ea30`)

Peças de `0x8C364B93`: `…70` canto superior esquerdo (botão, 4 quadros),
`…71` borda de cima, `…72` canto superior direito, `…73` borda esquerda,
`…74` borda direita, `…75` canto inferior esquerdo, `…76` borda de baixo,
`…77` canto inferior direito. Os cantos vão aos cantos do retângulo (a
tela); as bordas esticam entre eles.

### 8.5 Sons e dicas (no app)

Os botões tocam o som do próprio script (`btnclicksnd`, CRC24 do nome,
§7.7; `uiButtonSound` acha o nome entre os 84 `ui_*` do executável): as
velocidades `ui_button_play`/`_fw`/`_ffw`/`_pause`, os botões dos
subpainéis o deles. Sem `btnclicksnd`: cancelar na fila `ui_queue_delete`,
filtros/páginas/abas dos subpainéis `ui_secondary_button` (inferido).
Dicas: o `tipres` da janela, ou o `captionres` "Título|Texto" das barras e
grupos; os campos depois do texto (`|||0x4906501c`, `|0xab211110|…`) são
figuras e o script do balão (não desenhados). Aparecem 0,75 s depois que o
mouse entra no controle e somem aos 2,25 s (§7.6); sem tela, `--ui-hover`
conta do começo (a foto em 1,26 s mostra a dica). Chave da miniatura de
objeto: o grupo vai com sinal (`%d`), o `thumbnailKey` do `catalog.*` o
escreve sem sinal (igual para todos os grupos do jogo).

### 8.6 Rodada 8: signo, nota, popups, fila (dados dos scripts; o que é inferido está marcado)

- **Signo** (Simologia, janela `0x1006`): o dado de pessoa 70 (1..12,
  inferido) em Aries..Peixes, na ordem do STR# `0x90` do `Live` (cada
  entrada é "Nome|Descrição", a dica da janela). As 12 figuras (45x32) são
  `ui.package` grupo `0x499DB772`, instâncias `0xAB202500..0xAB202555` de 5
  em 5; a ordem delas não é a dos signos, então a tabela
  (`kZodiacImages` em `live_panels.cpp`) foi feita olhando cada imagem.
  Há um segundo conjunto de 12 (`0xCA200412..466`) que não é usado aqui.
- **Nota escolar** (janela `0xEC2D3BBA` do painel de emprego, só criança e
  adolescente sem emprego): as letras são as entradas 6 a 18 do STR# `0x8B`
  (A+, A, A-, B+ ... D-, F; vazias no PT-BR, então entram as do inglês), o
  dado de pessoa 56 vira a letra pelos limites 97, 93, 90, 87, 83, 80, 77,
  73, 70, 67, 63, 60 (**inferido**: a escala do dado e os limites não estão
  nos dados; as crianças de `N001_Lot11` têm 12 e saem com F).
- **Popup do humor** (§4.1): o script `0x49064908` (título, texto, "Bônus"
  e a ponta `0xA9600338` embaixo, filho 2). Clicar no medidor (botão
  `0x49064809`) abre 500 ms depois; a ponta fica na âncora `0x4906480D`
  (o topo do botão); um clique em qualquer lugar fecha (**inferido**).
  Sons `ui_text_description_open/close` (nomes entre os `ui_*` do
  executável; **inferido** que são os dos popups).
- **Popup da relação** (script `0x4906502B`: nome em `0xA1`, descrição
  `0xA2`, tipo `0xA3`, ponta no filho 2): clicar na miniatura de um
  relacionamento. Textos do STR# `0x84`: descrições nas entradas 2 a 12
  (casados, noivos, namoro firme, amor, paixão, amizade especial, inimigo,
  amigo, conhecido, acabou de conhecer, família), tipos nas 23 a 34. **Inferido**:
  a descrição de cada estado (pelo sentido do texto) e a prioridade: casados,
  noivos, namoro firme, amor, paixão, família, amizade especial, inimigo,
  amigo, conhecido, recém-conhecido. A caixa do script cabe 2 linhas e a
  descrição tem 4: a caixa cresce.
- **Fila**: os ícones deslizam até o lugar em `kAnimationSteps` = 4 passos
  (do script `0xCCD02691`); o tempo de cada passo (1/30 s) e o ponto de
  partida de um ícone novo (o lugar do anterior; o primeiro vem da esquerda)
  são inferidos. Um ícone novo toca `ui_queue` (não na primeira vez que o
  painel desenha o Sim, nem ao trocar de Sim).

---

## 8b. Paredes e telhado no puck (dados do `ui.package`, rodada 10)

O script do puck (`0x4905F85B`) tem quatro botões no mesmo lugar, `(28,66)-(68,98)`:
`0x1100` "walls down" (dica "Sem Paredes - Origem/Fim"), `0x1101` "walls cutaway"
("Paredes Cortadas - Origem/Fim"), `0x1102` "walls up" ("Paredes Erguidas - Origem/Fim",
o único visível no script) e `0x1103` "roof" ("Telhado - Origem/Fim"). Junto, o
`GZWinGen 0x4000` `(61,54)-(149,123)` (imagem `0xac76ee55`) por cima dos controles
da câmera, com os botões `1` "Sem Paredes", `2` "Paredes Cortadas", `3` "Paredes
Erguidas" e `4` "Mostrar/Esconder Telhado" (estilo toggle, tiras de 4 quadros) e um
`GZWinFlatRect 5` escondido que cobre o puck inteiro (inferido: pega o clique fora do
leque para fechá-lo).

### 8c. Modos de parede e o corte (binário, rodada 11)

**Modos** (`cITSMainToolbar::eWallMode`, pelo `cTSWinProcUCP::onWallChanged`,
BV `0x19f740`, que acende o botão do modo atual): **0 = sem paredes**
(`0x1100`), **1 = cortadas** (`0x1101`), **2 = erguidas** (`0x1102`), **3 =
erguidas com telhado** (`0x1103`). São quatro escolhas exclusivas, não um
liga/desliga do telhado: clicar num dos quatro botões só abre o leque
(`onBtnClickedID`: IDs `0x1100–0x1103` → `this+0x2C→0xC`, depois
`onWallChanged`). Lote novo começa em 1 (`InitForNewLot`); o modo é salvo
com a UI do lote (`Write`/`Read`, `ApplySavedUIState`).

`cTSMainToolbar::SetWallMode(modo)` (BV `0x1aec14`): **mostra os telhados só
no modo 3** (`WorldDB→0x88→0x4C(modo == 3)` e `WorldDB→0x170→0x20(modo ==
3)`); se a opção "renderizar o andar do Sim selecionado" (`+0xB4`) está
ligada guarda o modo e chama `RenderSelectedSimLevel(andar, modo == 2, modo
== 1)`; senão, saindo do modo 1 pede ao sistema de cena o redesenho do andar
(`TSSGSystem→0x88→0x58(andar)`), posta a mensagem `0xCB46C21D` {andar,
modo ≠ 0, modo} e guarda o modo; por fim avisa o ouvinte (`0x8953BB69`,
modo). `nTSSG::cPerson::SetWallMode` posta a mesma mensagem.

**Corte** (`nTSSG::cWallCutawayManager::OncePerFrameUpdate`, slot 0x34 da
vtable, BV `0xc8e1cc`; o symxfer o chama de
`cDiffusionWallSegment::CopyToFinalColor`): **só roda no modo 1**. A cada
quadro:

1. `SetSelectedSimLocation`: se o Sim selecionado está no mundo e **no andar
   que se vê**, guarda o tile dele; "mudou" (`CutawayLocationChanged`) =
   fora de casa (cômodo 0) mudou de tile, dentro mudou de cômodo.
2. `SetCameraTransform`: "mudou" se a matriz da câmera mudou.
3. Só recalcula se algo mudou: Sim, câmera, paredes (mensagens `0x2B4CF319`
   / `0x0B4CF313`), `0x2B4D18FB` (zera a memória de cômodos) ou alvos novos.
4. `CollectWallsEligibleForCutaway(andar atual)`: todas as paredes do andar,
   menos as do serviço do mundo 9 (inferido: as que não são paredes de
   verdade), cujo estilo de partição aceita corte
   (`nTSWorld::PartitionStyleSupportsCutaway`; cercas, por exemplo, não).
5. `KeepOnlyFrontFacingOutsideWalls(usarSim, posSim, câmera)` (BV
   `0xc8c868`), para cada elegível, com o cômodo de cada face (≠ 0 =
   dentro):
   - **as duas faces dentro (parede interna): sempre corta**;
   - **uma face fora (parede externa): corta se a face de fora está virada
     para a câmera** (produto vetorial 2D da direção da parede, orientada
     a partir da face de dentro, com (início da parede − posição da câmera)
     < 0);
   - as duas fora (parede solta no quintal): nunca corta;
   - com `usarSim` (opção `cutawayForSelectedSim` ligada, Sim guardado e no
     andar atual) também corta as paredes que **cruzam a linha da câmera
     ao Sim** (consulta de raio do serviço do mundo 3); a opção não está no
     `globalProps.xml`, mas é um tweak com **padrão ligado**: `FUN_00c8d74c`
     (BV, o registro das propriedades do `cWallCutawayManager`) registra
     `cutawayOutside` (bool, 1, id `0x0BF487B2`), `cutawayMemorySize` (int,
     3, `0xCBF487C6`) e `cutawayForSelectedSim` (bool, **1**, `0x8D9B7CE1`);
     `FUN_00c8b290` os lê de volta (rodada 13: a "inferida desligada" estava
     errada).
6. `UpdateSceneGraph`: compara o conjunto novo com o anterior e manda ao
   gerenciador de paredes cortar/erguer só as diferenças, as duas faces de
   cada parede (`OppositeWallLocator`), e ajusta os arcos das cercas nos
   vértices (`cFenceManager`).

Com câmera de cinemática (`SetCinematicsCutaway`, slot 0x2C) o passo 5 vira
`Expose(alvos do andar)` (`AddTargetLocation`, slot 0x24): alvo fora de casa
→ `SelectNearSemicircleBlockers` (raio 2,0) se `cutawayOutside` (padrão
true no `globalProps.xml`); alvo dentro → o cômodo entra numa memória dos
últimos `cutawayMemorySize` (3) cômodos e, para cada um,
`SelectRoomBlockers` corta as paredes desse cômodo viradas para a câmera.

No app (rodada 12, `app/wall_view`, `formats/lot_model`): os quatro modos
como no jogo (o telhado só no modo 3; o leque acende um dos quatro; Home/End
andam em ciclo pelos quatro, inferido); no modo 1 as paredes internas do
andar são todas cortadas, as externas só com a face de fora virada para a
câmera (pela direção da câmera, que está longe, em vez da posição), as soltas
no quintal nunca, e só as de estilo `mayCutAway` (paredes normais e de sótão;
cercas — padrão falso, inferido do `fencearches.txt` —, fundação, piscina e
saias de deck não); os cômodos das faces vêm do mapa de luz (3ARY `0x0B`),
em que quintais cercados são fora. Recalcula quando a câmera passa de um
lado de uma direção de parede, o andar ou o modo mudam, ou as paredes mudam;
o Sim selecionado entra desde a rodada 13 (`LotViewOptions::simLine`): com o
Sim selecionado no andar mostrado, as paredes elegíveis que cruzam, na planta,
o segmento da câmera (posição) ao Sim também são cortadas (inferido: o jogo
usa uma consulta de raio do mundo, aqui a interseção 2D); o modelo é refeito
quando o Sim ou a câmera muda de tile.

### 8d. Corte: posição da câmera, raio do Sim, alvos e memória (binário, rodada n05)

Lido pelo Ghidra MCP no `sims2_bv.ppc` (nomes pelo `baserom.sym`, casados
pela ordem das funções do `cWallCutawayManager`; vtable BV `0x0281f4f0`).
Endereços BV:

| Função (baserom) | BV |
|---|---|
| `OncePerFrameUpdate` (slot 0x34) | `0xc8e1cc` |
| `SetSelectedSimLocation` | `0xc8af40` |
| `CutawayLocationChanged` | `0xc8aca8` |
| `SetCameraTransform` | `0xc8ade4` |
| `CollectWallsEligibleForCutaway` | `0xc8b778` |
| `SelectWallsBlockingPoints` | `0xc8bc0c` |
| `SelectRoomBlockers` | `0xc8bf10` |
| `SelectNearSemicircleBlockers` | `0xc8c300` |
| `KeepOnlyFrontFacingOutsideWalls` | `0xc8c868` |
| `UpdateSceneGraph` | `0xc8cd90` |
| `Expose` | `0xc8de78` |
| `AddTargetLocation` (slot 0x24) / `RemoveAllTargetLocations` (0x28) / `SetCinematicsCutaway` (0x2C) | `0xc8b94c` / `0xc8b244` / `0xc8abd8` |
| construtor | `0xc8dbf8` |
| `nTSWorld::cWallGraph::WallsBetweenPointSetAndPoint` (vtable `0x0281b6d8`, slot 0x58) | `0xc1d6ec` |

**Posição da câmera.** `KeepOnlyFrontFacingOutsideWalls` e as outras
seleções pegam a posição da câmera (`viewer→0x230`, x, y, z) e testam cada
parede contra ela, não a direção da vista: com a direção `d` da parede
orientada a partir da face de dentro, corta se `d.y·(a.x − c.x) − d.x·(a.y −
c.y) < 0` (`a` = início da parede, `c` = câmera). É o mesmo que "a câmera
está do lado de fora da reta da parede". `SetCameraTransform` marca
"mudou" a cada mudança da matriz da câmera, então o corte é refeito a cada
movimento da câmera (o `UpdateSceneGraph` só mexe nas diferenças).

**O Sim.** `SetSelectedSimLocation` (só sem cinemática) pega a posição do
Sim selecionado (x, y exatos e `NearestLevel`), e só se ele está no andar
mostrado (`toolbar→0x64`) guarda a posição; "mudou" (`CutawayLocationChanged`)
= dentro de casa mudou de cômodo, fora (cômodo 0) mudou de tile. O corte só
é refeito quando algo mudou, e então usa a posição guardada naquele quadro.
Com `cutawayForSelectedSim` (+0x133, padrão 1), o Sim guardado (+0xd8) e no
andar atual (+0xd4), a posição vira um ponto com a altura do piso
(`SampledFloorElevation`) e vai para `WallsBetweenPointSetAndPoint(pontos,
câmera, andar)`:

- para cada ponto P, o raio vai de P para a câmera, **cortado na altura de
  3,0 acima de P** (`0x0274b48c`): se `câmera.z − P.z > 3`, o fim E é `P +
  (câmera − P)·3/(câmera.z − P.z)`; senão E = câmera. É esse o "raio do
  Sim": o alcance no plano é 3 × (distância horizontal / altura) da câmera;
- os tiles do caminho de P a E, num passo de eixos (DDA) próprio: começa no
  tile de P (`floor`), **menos 1 em cada eixo em que a câmera está no lado
  negativo**; o eixo maior é o de |dx| ou |dy| maior; o erro começa em
  |menor|; a cada passo, se |maior| > erro anda no eixo maior e soma |menor|
  ao erro, senão anda no menor e tira |maior|; marca o tile atual enquanto
  o eixo maior não chegou ao tile de E (o tile de E não entra); fora do lote
  não marca;
- depois, para cada tile marcado, entram as paredes do tile no andar: as
  diagonais (máscaras 4, (x,y)–(x+1,y+1), e 8, (x,y+1)–(x+1,y)) sempre; a do
  lado x (máscara 1, (x,y)–(x,y+1)) só se o tile x−1 também está marcado
  (ou x é a borda do lote); a do lado y (máscara 2, (x,y)–(x+1,y)) só se o
  tile y−1 também está marcado (ou y é a borda);
- o resultado só corta paredes elegíveis (a própria ou a face oposta).

Do jeito que está, com a câmera no lado negativo de um eixo a parede colada
ao Sim desse lado não entra (o tile do Sim não é marcado): é o que o código
faz, e o app copia.

**Cinemáticas.** `cTSSGSystem` Start (`FUN_007c3898`, `0x7c4b18–0x7c4e24`):
guarda o andar (`toolbar→0x64`) e o modo das paredes (`→0x6c`); salvo se o
flag +0x194 está ligado (lido de uma propriedade em `0x7c17c4`, padrão 0;
não identificado), leva o andar ao do primeiro ator (ou ao mais alto com
"mostrar a casa inteira") e o modo a 1 (ou 3 com a casa inteira); com o
flag desligado também zera os alvos (`RemoveAllTargetLocations`), põe a
posição de cada ator como alvo (`AddTargetLocation`, x, y e andar) e liga
`SetCinematicsCutaway(true)`; Stop (`FUN_007c56b4`) devolve andar e modo e desliga a cinemática e os alvos (`SetCinematicsCutaway(false)`, `RemoveAllTargetLocations`). Com cinemática, o `OncePerFrameUpdate` troca o passo 5 por
`Expose(alvos do andar atual)` com a posição da câmera da cena:

- alvo **fora** (cômodo 0): se `cutawayOutside` (padrão true),
  `SelectNearSemicircleBlockers(2,0, câmera, alvo)`: os vértices do lote num
  quadrado de raio 2 (arredondado) em volta do tile do alvo, só os que não
  tocam nenhum cômodo, a até 2,0 do alvo e não mais longe que ele na direção
  da câmera ((v − c)·(alvo − c)/|alvo − c| ≤ |alvo − c|), viram pontos de
  `SelectWallsBlockingPoints` (a consulta acima);
- alvo **dentro**: o cômodo entra na memória (uma fila; se não está nela e
  ela tem `cutawayMemorySize` = 3, sai o mais antigo) — a memória só é
  zerada pela mensagem `0x2B4D18FB`, então vale entre cenas;
- para cada cômodo da memória, `SelectRoomBlockers`: as paredes elegíveis
  com uma face nesse cômodo e a outra não, com a face de fora do cômodo
  virada para a câmera, são cortadas (as com as duas faces no cômodo
  também), e as pontas delas viram pontos de `SelectWallsBlockingPoints`
  (corta o que tapa essas paredes).

## 9. O que falta

- O nome da função de desaturação (o símbolo transferido diz `LotFamilyAccessor::GetDataStr`, errado).
- Cores dos textos/tooltips do painel (vêm dos scripts).
- O conteúdo das imagens de gradiente dos motivos (agente d: ler os filhos
  4 das barras no script do painel de motivos).
