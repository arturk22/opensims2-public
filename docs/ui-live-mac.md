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
- sub-interações empilhadas para baixo em `x = kLocationX`, a primeira
  deslocada pelo `GetSubActionOffset` do ícone;
- conector: o primeiro ícone de nível superior e as sub-interações depois
  da primeira mostram o traço de ligação.

O layout de um ícone vem do UI script `0x0609A1C0` (filhos: 3 imagem do
ícone; 4 "cancelado"; 5 moldura de "em execução"; 7 marca extra; 1, 2
botões; janelas `0x100`/`0x200` = variante normal/sub-interação, cada uma
com a máscara circular no filho 3).

### 6.2 Ícone (`cTSWinActionQueueIcon::Refresh`, BV `0x9d6bc4`)

- Fonte: o ícone da interação (o objeto alvo, ou o Sim alvo pelo seu
  retrato: miniaturas do serviço de thumbnails), recortado pela máscara
  circular (`ApplyAlphaMask`).
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

---

## 7. O que falta

- O nome exato do serviço de thumbnails e da função de desaturação.
- Cores dos textos/tooltips do painel (vêm dos scripts).
- O conteúdo das imagens de gradiente dos motivos (agente d: ler os filhos
  4 das barras no script do painel de motivos).
