# Câmera do lote e menu de pie: o que o binário de Mac faz

Fontes e convenções como em `docs/shaders-mac.md`. Classes: `nTSSG::cCameraLive`
(controlador "Live" do `LiveCamera.txt`), sua base
`cTSCameraSmoothSims1Controller`, e `cTSWinPieMenu`. Dados: `TSData/Res/
UserData/Cameras/*.txt`, as teclas em `UI/ui.package` (tipo `0xA2E3D533`,
grupo `0x8CA033A2`) e as constantes do pie no mesmo pacote (UI script
`0xA99D8A11`/`0x090617B7`, "Constants Table").

---

## 1. Câmera "Live"

### 1.1 Posição (binário, `ViewerPositionAndOrientationFromDistanceAlongCurve`)

```
d     = distância da zoom table (interpolada entre níveis)      ← distância 3D, não horizontal
s     = tan(lerp(pitchMin[z], pitchMax[z], t))                 (sem tabela de pitch: s = slope)
h     = heading + offset
horiz = d / sqrt(1 + s²)
olho  = pivô + (cos h · horiz,  sin h · horiz,  s · horiz)     (Z para cima)
```

e a câmera olha para o pivô. Ou seja: **o número da zoom table é a
distância do olho ao pivô ao longo da linha de visão**, e o heading h põe o
olho na direção (cos h, sin h) a partir do pivô (a câmera olha para −(cos h,
sin h)). `t` ∈ [0, 1] é a posição dentro da faixa de pitch do nível
(arrastar na vertical muda `t`, §1.4). Ângulos da config em graus
(convertidos por π/180 no `SetPitchAngles`/`SetOrientations`).

`fov` por nível (graus; sem tabela: **60°**); `fov` 0 = ortográfica com
meia-largura = d·0,2. `clip` = perto/longe por nível. Não confirmado se o
FOV é vertical ou horizontal (vai para o viewer do scene graph).

### 1.2 Suavização (binário, `FUN_008c77d4` escalar, `FUN_008c78a0` ângulo, `FUN_008c7c64` vetor)

Cada valor animado tem (passos N, mínimo m) e a cada atualização com Δt:

```
r    = alvo − atual
step = r · Δt / 50 / N            (Δt em ms: N é o número de "quadros de 50 ms"; inferido)
se step² < m²:  se r² ≤ m² → atual = alvo; senão step = sign(r)·m
se step² > r²:  atual = alvo
senão atual += step
```

É uma aproximação exponencial com velocidade mínima, não uma duração fixa.
`smoothing 5 8` = N do zoom e do giro; `defaultminchange` (0,02) é o m de
todos; `keyboard*lerpsteps`/`lerpminchange` sobrescrevem os do teclado. O
ângulo usa o caminho mais curto (−π..π). O código atual interpola por
duração em ticks de 30 Hz.

### 1.3 Pivô, andar e limites (binário, `cCameraLive`)

- `pivotheightabovefloor` (1): altura do pivô sobre o piso.
  `ConstrainSubjectHeight` amostra a altura do piso (`HeightSample`:
  `floorheightsamplerange` 6, `...innerrange` 2, `cornersampleoffset`/
  `cornersampleratio` 0,5) e sobe o pivô se o olho ficaria abaixo do piso.
- `Update`: se o Sim selecionado está noutro andar que o andar visível, o
  andar visível passa a ser o dele (segue o andar do Sim).
- `constraintwidth` (−10): o pivô fica preso ao retângulo do lote somado
  dessa largura (negativo = para dentro); `SetEdgeConstraints(l, t, r, b)`
  só aceita margens com soma < 1.
- `subjecttrackingdeadzonemagnitude` 30 / `centerTrackingDeadZoneMagnitude`
  (30 por padrão): seguindo um Sim, a câmera só recentra quando a projeção
  dele se afasta mais que isso (pixels) do centro.
- `defaultcamerapitch`: padrão 0,15 (t ao entrar no lote).

### 1.4 Entrada (dados + binário)

Teclas (`ui.package` `0x6A9362F2`, modo vivo):

| tecla | ação |
|---|---|
| ←/→/↑/↓ | mover (início/fim); com Shift, 2× |
| `z` / `x`, NumPad+/− | zoom um nível |
| `=` / `-` | zoom contínuo enquanto segura |
| `,` / `.` | girar esquerda/direita enquanto segura |
| Ctrl+4..9 / 4..9 / Shift+4..9 | salvar / voltar (suave) / pular para a posição de câmera 0–5 |
| Tab | trocar de controlador de câmera |

Pedidos internos (`HandleRequest`, binário):

- **Girar** (dx, dy em pixels, do mouse ou do teclado): só o eixo dominante
  conta (|dx| > |dy| → só dx; menor → só dy). `heading −= clamp(dx·0,003,
  −3, 3)` rad (giro livre, sai da tabela de orientações); `t = clamp(t −
  dy·0,005, 0, 1)` (pitch dentro da faixa do nível).
- **Mover** (dx, dy na tela): projeta o pivô na tela, desloca, desprojeta
  e gira pelo heading → velocidade do pivô. A velocidade em metros cresce
  com a distância porque o passo é em pixels (bate com "pan ∝ zoom").
  `keyboardtranslationspeed` 50.
- **Roda do mouse**: `d += Δ·wheel·(d + 10)/(−50)` (contínuo, preso entre
  o primeiro e o último nível), e o nível atual passa a ser o primeiro com
  distância ≤ d. `wheel 5`.
- **Teclado**: `keyboardrotationspeed` 200, `keyboardzoomspeed` 1,
  `keyboardzoomscale` 2 alimentam esses pedidos por lerps próprios.

### 1.5 O que difere do código atual (`src/render/camera.*`, `src/app/camera_input.*`)

| item `TODO.md` | jogo |
|---|---|
| yaw 0 = +X | heading h põe o olho em (cos h, sin h) do pivô; a câmera olha para −(cos h, sin h) |
| distância da zoom table horizontal | distância 3D ao longo da visão |
| suavização em ticks de 30 Hz | exponencial em quadros de 50 ms com velocidade mínima |
| pan ∝ zoom | sim, por ser em pixels de tela |
| FOV vertical | não confirmado |
| limitar ao lote | lote + `constraintwidth` (−10 = 10 tiles para dentro) |
| seguir altura do piso/Sim | sim: andar do Sim e amostra de altura do piso |

---

## 2. Menu de pie (`cTSWinPieMenu`)

### 2.1 Constantes (dados, "Constants Table" `0x090617B7`)

`kItemRadius=68`, `kCancelBoundary=30`, `kGesturePickBoundary=35`,
`kHeadAreaInflateForItemOverlap=-60`, `kCameraTranslationFactor=1.6`,
`kPieMenuTextColorNormal=178,193,255`, `kPieMenuTextColorHighlited=0,255,0`,
`kPieMenuFont=0x2966548b`, `kMainButtonSound=ui_piemenu_primary`,
`kSubmenuButtonSound=ui_piemenu_secondary`, `kEnableItemAnimations=false`.
`kPieMenuTextColorDisabled` não está na tabela (cor padrão do código).

### 2.2 Layout (binário, `layout`, `determine_item`, `position_item`)

- No centro, a janela da cabeça do Sim (`get_person_window`: um
  `cTSWinCalloutBox` com o retrato), centrada no clique.
- n itens, passo Δ = 2π/n; o ângulo começa em 0 e cresce; o item i fica em
  `(cx + R·sin a, cy − R·cos a)`, R = `kItemRadius` → **começa no topo e
  segue no sentido horário**. Se a posição já está ocupada, tenta a + Δ.
- Canto de cima-esquerda da janela do item (largura w) pelo ângulo do ponto
  (classe de `determine_item`, deslocamentos de `position_item`):

  | ângulo a | x do item | y do item |
  |---|---|---|
  | 0 (topo) | ponto − w/2 | ponto − 48 |
  | (0°, 45°) | ponto − 14 | ponto − 26 |
  | [45°, 135°] | ponto − 14 | ponto − 21 |
  | (135°, 165°] | ponto − 14 | ponto − 16 |
  | (165°, 195°) | ponto − w/2 | ponto − 21 |
  | [195°, 225°) | ponto − (w − 14) | ponto − 16 |
  | [225°, 315°] | ponto − (w − 14) | ponto − 21 |
  | (315°, 360°) | ponto − (w − 14) | ponto − 26 |
  | item no centro (o "voltar" do submenu) | centrado | centrado |

  Isto é: do lado direito o item cresce para a direita a partir do ponto, do
  lado esquerdo para a esquerda (o recuo de 14 px é o ícone/bolinha do item,
  inferido).
- Depois do layout o menu todo é deslocado para caber na tela
  (`fit_on_screen`).

### 2.3 Interação (binário)

- Botão esquerdo: escolhe no **soltar**; soltar fora de um item cancela.
  Arrastar e soltar (gesto) também escolhe o item sob o cursor.
  `kCancelBoundary`/`kGesturePickBoundary` são as distâncias (px) do gesto.
- Botão direito ou qualquer tecla (menos Shift) cancela; Tab pede o menu
  "mais" (um filho com um item especial −3).
- Item com filhos: abre um **pie filho centrado no item** (a cabeça vai
  junto); o filho tem um item "voltar" no centro que reabre o pai.
- Som ao escolher (`WCM` com o evento do botão). O menu é cancelado sozinho
  se o Sim deixar de estar selecionável (`GZOnTick`).
- `kEnableItemAnimations=false`: os itens não animam ao abrir.

### 2.4 Diferenças do código atual (`src/app/pie_menu.*`)

Começo no topo e sentido horário: iguais. Faltam: raio 68 px, as cores
(normal 178/193/255, destaque verde), a cabeça do Sim no centro, os
alinhamentos por ângulo, submenu centrado no item com "voltar" no centro,
escolha no soltar e o gesto, e o ajuste à tela.

## 3. O que falta

- O mapa de dispositivo do mouse (`FillDeviceMap`): qual botão gira/move.
- Se o FOV é vertical; a unidade de Δt do lerp (ms, inferido).
- A tabela de offsets do item "mais" e a cor de desabilitado.
