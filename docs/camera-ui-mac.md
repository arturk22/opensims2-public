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
meia-largura = d·0,2. `clip` = perto/longe por nível. O FOV vai para o
viewer do scene graph (`cViewerRefNode::SetFieldOfView`, BV `0x1b33d94`:
guarda o ângulo em radianos no volume de visão e marca o volume sujo); o
`cTSWinSimView::SetFOV` (BV `0x1a590c`) passa `2·f·π/180`, ou seja, recebe
o **meio ângulo** em graus (só a janela do Sim do CAS/UI usa esse caminho).
A câmera do lote passa o valor direto: `cTSCameraSmoothSims1Controller::Update`
(BV `0x86ffb8`) interpola a tabela `fov` do nível (em radianos; sem tabela,
`60 · π/180`) e chama `SetFieldOfView` (vtable `0x210` do viewer) e depois o
perto/longe (`0x218`); `fov` 0 vira ortográfica (`0x220`).

**Rodada 15 — a matriz (binário, BV `0xccd254`, o montador da matriz do
volume de visão, tipo 0 = perspectiva):**

```
a  = (largura/altura do viewport) · aspecto do alvo (+0xf4)   (com usePixelAspectRatio: ÷ aspecto do pixel)
m[0]  = 1 / tan(fov/2)            escala de x
m[5]  = a / tan(fov/2)            escala de y = escala de x · largura/altura
m[10] = longe / (longe − perto) ;  m[11] = −perto·longe / (longe − perto) ;  m[14] = 1
```

Ou seja, **o FOV é o ângulo horizontal inteiro** (não o vertical do
`PerspectiveFov` do D3D, como se tinha inferido): numa janela mais larga
se vê menos altura ("Vert-", o que os guias de tela larga dizem do The
Sims 2). Com o `fov 45` do `LiveCamera.txt`, numa janela 16:9 o ângulo
vertical é `2·atan(tan(22,5°)·9/16)` ≈ 26,4° (antes saíam 45° na
vertical, ~73° na horizontal: a vista ficava bem mais aberta que a do
jogo). Corrigido em `LotCamera::projection` e na câmera das cinemáticas
(o `fov` do `viewersGroup` vai para o mesmo viewer); a câmera do CAS
(`CasMode::makeCamera`) é nossa e fica com o seu FOV vertical
(`CameraDef::verticalFov`).

**Rodada 15 — releitura (binário) e o enquadramento em tela larga:**

- O aspecto da matriz vem do alvo de renderização: `FUN_00ccd7b8` (o
  `ViewDefinition` do viewer) relê o `ImplRenderTargetDef` (BV `0xecc860`:
  largura, altura e aspecto do pixel do alvo/janela) e, se mudou,
  `FUN_00cccfd8` guarda `+0xf4 = largura/altura` (com `usePixelAspectRatio`,
  opção do painel de desempenho, ÷ aspecto do pixel) e marca os volumes de
  visão sujos. O viewport do volume é normalizado (0..1, preso a 1), então
  `a` = (w/h do viewport normalizado = 1 na tela cheia) × largura/altura
  em pixels. Nenhum chamador converte o FOV: o `Update` (`0x86ffb8`)
  interpola a tabela `fov` (radianos) por nível e passa direto. **O FOV é
  horizontal para o aspecto real da janela, sem letterbox nem ajuste:** o
  jogo é "Vert-".
- Resoluções: `FUN_00abc90c` lê `ScreenModeResolution` (padrão **800×600**,
  `defaultResWidth/Height`, máx. `maxResWidth/Height`) e aceita qualquer
  modo ≥ 800×600 com largura > altura (`FUN_00abc4d4`), então tela larga
  existe, mas o jogo foi feito e visto em 4:3.
- Os números da câmera são os do jogo, lidos do `LiveCamera.txt` sem
  ajuste: zoom `70 40 25 15 8 3` (distância 3D), pitch por nível
  `(30..2)`/`(40..65)`, perto `0,6..0,1` e longe `2000` interpolados por
  nível no `Update` (sem tabela, 1 e 100), pivô a `pivotheightabovefloor`
  1 sobre o piso. Nada foi afinado para o FOV vertical antigo.
- No 4:3 com `fov 45`, a vertical é `2·atan(tan 22,5°·3/4)` ≈ 34,7°: no
  zoom mais perto (d = 3, pivô 1 sobre o piso, pitch ≈ 11°) se vê ±0,94
  em volta do pivô, ~0,06 a 1,94 do piso, o Sim inteiro, como no jogo. Em
  16:9 com o FOV horizontal puro seriam 26,4° (±0,70: cabeça e pés
  cortados, o que a rodada 15 mostrava).
- **O que o app faz (nosso, `fovScales` em `render/camera.hpp`)**: até 4:3
  exatamente o binário (FOV horizontal); mais largo que 4:3, mantém a
  altura do 4:3 e abre os lados ("Hor+", o que as correções de tela larga
  do jogo fazem). Vale para o lote, o bairro (`fov 33`) e as cinemáticas.
  O CAS usa a câmera do jogo (§1.6, rodada 16).
  Captura: `docs/screenshots/r15-camera-fov-4x3.jpg`.

### 1.6 A câmera do CAS: `cCameraCreateASimController` (binário, rodada 16)

`CASCamera.txt` define câmeras `CreateASim` (`CAS`, `CAS_Family`,
`CAS_Face<idade>`, `CAS_Clothes<idade>`, `CAS_ZoomDisabled`) com `fov`,
`clip` e, dentro de `if (maxVersion() >= 3)`, os `setparam`. O 1.0 de Mac
já lê esses parâmetros (as strings estão no binário base), então o jogo
base entra nesses blocos (inferido: o `maxVersion()` é o `+0x54` do parser,
`FUN_015305ec`, cujo valor não foi achado; o parser do app aceita
`maxVersion() >= N` com N ≤ 3 e pula os outros `if`).

- `SetCameraSpec` (BV `0x7a8fb8`): `interpolatespeedto/from` (`+0x2f4/+0x2f8`,
  0,1 sem o parâmetro), `minzoom`/`maxzoom` (`+0x2dc/+0x2e0`, 0),
  `keyboardzoomspeed` (`+0x2e8`, 0,05), `zoomheading` em radianos
  (`+0x350/+0x354`), `zoomelevation` em radianos **negado**
  (`+0x358/+0x35c`), `mousewheelsensitivity` (`+0x2f0`, 1).
- É uma câmera de primeira pessoa: fica num lugar e numa rotação de
  destino (o CAS os dá; no estúdio, os localizadores `camera_face_<idade>`,
  `camera_clothes_<idade>`, `camera_family_0` do `CASPodiumArea`/
  `CASFamilyArea`; a frente da câmera é o **+Y local** do localizador, o
  `DAT_02c2fbf8` = (0, 1, 0), e o +Z o "para cima"; os localizadores do
  pódio olham para o Sim com o +Y). Quem pareia spec e localizador não está
  no binário nem nos scripts de UI: o app usa os nomes (família →
  `CAS_Family` + `camera_family_0`; passos de cabelo, rosto e cosméticos →
  `CAS_Face<idade>` + `camera_face_<idade>`; os outros → `CAS_Clothes<idade>`
  + `camera_clothes_<idade>`; bebê como criança pequena, idoso como adulto;
  inferido).
- `SetActive` (`0x7a9898`): com outra câmera CreateASim ativa antes, guarda
  dela o lugar, a rotação, o FOV, o zoom, o heading e a elevação atuais e a
  sua `interpolatespeedfrom` (`+0x304`), e começa a transição t = 0; sem
  câmera antes, t = 1 e o zoom = `minzoom`.
- `Update` (`0x7a9d68`): na transição, mede uma vez o caminho do olho
  (|olho(t = 0,99) − olho(0)|, mínimo 0,001, senão 1) e avança
  `t += max(lerp(speedFrom, speedTo, t)/caminho, 0,001)·dt/50`; com uma das
  velocidades 0, corte (t = 1). Ao chegar, o zoom volta ao alvo da nova
  câmera (`+0x34c`, 0). Fora da transição o zoom segurado anda
  `±keyboardzoomspeed` e o valor mostrado segue o alvo por `FUN_008c77d4`
  (10 passos, sem mínimo).
- Pose: s = t²(3 − 2t); posição = lerp (`0x7a928c`), rotação interpolada
  (`0x7a93c8`), FOV = lerp (`FOV`, `0x7a970c`; ângulo horizontal, o mesmo
  viewer), zoom = lerp(zoom de antes, 0) na transição; **olho = posição +
  zoom·direção**, direção = `Rz(heading)·Rx(elevação)` aplicados à frente da
  câmera já no mundo (`0x7aa284`; as rotações em torno dos eixos do mundo:
  a ordem das multiplicações e a convenção linha/coluna da GZ ficaram como
  a leitura que leva a câmera das roupas para o tronco e o rosto; inferido).
  A rotação da câmera não muda com o zoom.
- `DoMessage` (`0x7aa0f8`): zoom + / − = ±`keyboardzoomspeed`, preso em
  [`minzoom`, `maxzoom`]; segurar = `+0x2e4` = ±velocidade até soltar.

No código: `CasCamera` (`render/cas_camera.*`) e `LotCamera::FirstPerson`;
`CasMode::applyCasCamera` escolhe spec e localizador, troca com transição e
corta nas capturas roteirizadas. Os botões de zoom do CAS dão 10 passos por
clique (meio segundo segurado; inferido); a roda, entalhes ×
`mousewheelsensitivity` (inferido). Sem o estúdio (`--cas-no-studio`) fica a
órbita antiga. Captura: `docs/screenshots/r16-cas-camera.jpg`.

**Lateralidade** (rodada 15): o mundo do jogo é destro com Z para cima,
como o do código. Provas: no esqueleto `auskel` o Sim olha para +Y e os
ossos `r_` ficam em +X (os `l_` em −X), o que só é a direita de quem olha
para +Y num sistema destro; a planta dos lotes bate com as miniaturas do
próprio jogo; o reflexo dos espelhos é montado por
`RightHandedReflectionTransform` (BV `0xcf9cd4`).

### 1.2 Suavização (binário, `FUN_008c77d4` escalar, `FUN_008c78a0` ângulo, `FUN_008c7c64` vetor)

Cada valor animado tem (passos N, mínimo m) e a cada atualização com Δt:

```
r    = alvo − atual
step = r · Δt / 50 / N            (Δt em ms: N = nº de "quadros de 50 ms")
se step² < m²:  se r² ≤ m² → atual = alvo; senão step = sign(r)·m
se step² > r²:  atual = alvo
senão atual += step
```

É uma aproximação exponencial com velocidade mínima, não uma duração fixa.
**Δt é em milissegundos** (binário): é um `unsigned long` inteiro, a
diferença entre duas leituras do relógio do jogo (`Update(unsigned long Δt,
viewer)`), e a mesma função anima a UI (`cTSWinActionQueueIcon::GZOnTick`
passa `agora − antes` do timer em ms).
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
  andar visível passa a ser o dele (segue o andar do Sim). No app, antes da
  VM escolher um Sim (capturas sem `--vm`), a câmera começa no andar do
  primeiro Sim do lote, o nível cujo piso sob ele fica mais perto da sua
  altura (`LotCameraFollow::startLevel`; numa fundação o piso da casa,
  0,75 acima do terreno); sem Sims, o térreo. Antes o pivô amostrava o
  nível 0 e ficava 0,75 baixo demais nos lotes com fundação (rodada 16).
  Rodada 16 (binário, `cCameraLive::Update`, BV `0x6a5450`): o andar visível
  **sobe** sempre para o do Sim, mas só **desce** quando o local do Sim está
  num cômodo (`cPartitionManager::RoomContaining(local, definição 0)`, vtable
  `0x13c` do serviço `WorldDB→0x94`, diferente de 0); no app, o cômodo do Sim
  (dado 29). Além disso o andar da piscina (−1) nunca é mostrado por seguir
  um Sim: quem nada deixa a vista no térreo, com a casa e a água (inferido:
  o setter do andar, vtable `0x68` de outro serviço, não foi lido; o cômodo
  do fundo da piscina não é 0, então só a regra do binário trocava para −1 e
  sumia a casa, QA ao vivo passada 13).
  Rodada 20 (`r20-pit`): os botões de andar (Page Up/Down) também param no
  térreo, nunca no −1 (inferido; antes mostravam só o terreno). O térreo de
  um lote com casas em fundação é mostrado com o nível 1, mas só as partes
  do nível 1 que não são um andar sobre o chão (`storyOverGround`: piso 3
  acima do nível de baixo nos 4 cantos do tile; inferido): as paredes de
  sótão do prédio em serra do N002_Lot31 não aparecem mais no térreo.
- `constraintwidth` (−10, `+0x494`): o pivô fica preso ao retângulo do
  mundo do lote (`WorldDB` vtable `0x10c`/`0x110`/`0x114`/`0x118` = x/y
  mínimos e máximos) **encolhido** dessa largura (BV `FUN_006a456c`:
  `x = clamp(x, mínX + w, máxX − w)`, idem y): com −10 o pivô pode ir até
  **10 tiles para fora** das bordas do lote. `SetEdgeConstraints(l, t, r,
  b)` só aceita margens com soma < 1.
- `subjecttrackingdeadzonemagnitude` 30 (`+0x438`) /
  `centerTrackingDeadZoneMagnitude` (30 por padrão): seguindo um Sim
  (`cTSCameraSmoothSims1Controller::Update`, BV `0x86ffb8`), a câmera
  projeta na tela o pivô atual e o Sim (menos um deslocamento `+0x1d4`,
  zero aqui) e, se a distância entre os dois pontos (pixels, sem o z) passa
  da zona morta, o Sim vira o alvo do pivô (vtable `0xfc`, suavizado).
- `defaultcamerapitch`: padrão 0,15 (t ao entrar no lote).

### 1.4 Entrada (dados + binário)

Teclas (`ui.package` `0x6A9362F2`, modo vivo):

| tecla | ação |
|---|---|
| ←/→/↑/↓ | mover (início/fim); com Shift, 2× |
| `a`/`d`/`w`/`s` | mover também (`0x6A9362F3` e `0x6A9362F5`: "Start Move Left/Right/Forward/Back"; `0x6A9362F7` ainda tem `q`/`e` "lower/raise camera height") |
| `z` / `x`, NumPad+/− | zoom um nível |
| `=` / `-` | zoom contínuo enquanto segura |
| `,` / `.` | girar esquerda/direita enquanto segura (nenhuma tabela liga setas ou `a`/`d` a girar) |
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
| FOV vertical | **horizontal** (binário, BV `0xccd254`); em janelas mais largas que 4:3 mantemos a altura do 4:3 (nosso) |
| limitar ao lote | lote encolhido de `constraintwidth` (−10 = até 10 tiles para fora) |
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

## 2.5 Botões do mouse (binário, `cTSMainToolbar::AddDefaultTools`, BV `0x1a9f44`)

O modo vivo liga ferramentas a códigos de botão (`0x80000000 | botão`,
modificadores nos bits 24–25; botão 1 esquerdo, 2 direito, 4 do meio):

| código | ferramenta (CLSID) | o que faz |
|---|---|---|
| `0x03000001` (esquerdo) | `0x692EE271` (ação padrão) | escolher, menu de pie |
| `0x80000002` (direito) e aliases `0x82000002`, `0x83000002` | `cTSUserToolCameraRightClick` (`0x692EE7EE`) | **pan contínuo**: segura e afasta o mouse do ponto onde clicou (mostra a âncora); a câmera anda com velocidade = deslocamento em pixels × 0,25 (`DragCamera`, BV `0x34c764`, evento `0x9B66110A`); soltar manda velocidade 0 |
| `0x80000004` (meio) e aliases `0x81000004`, `0x82000004`, `0x83000004`, **`0x81000002` (direito + modificador 1)** | `cTSUserToolCameraMiddleClick` (`0x4BB0EF8E`) | **girar**: no clique manda `0x9C17E418` (começo) e mostra o cursor de giro; cada movimento manda `0xBC17E41C` com (dx, dy) desde o clique (dx invertido por uma opção global) e devolve o cursor ao ponto do clique (arrasto infinito); o `cCameraLive` faz o giro de §1.4 |
| — | `cTSUserToolCameraHandDrag` (`0x69518882`) | pan 1:1 pelo deslocamento em pixels entre movimentos (mesmo evento `0x9B66110A`); usada quando outra parte da UI a ativa |
| roda | `cCameraLive::FillDeviceMap` (BV `0x6a4474`): eixo da roda × 1/120 por "clique" (`0xBC17E412`, `0x9C17E415`) | zoom (§1.4) |

O modificador 1 não foi identificado no binário; o tutorial ("O Básico -
Parte Um", passo 8: "inclinar e rotacionar a câmera usando CTRL + botão
DIREITO do mouse") diz que é o **Ctrl** (inferido do texto).

**No app (rodada 21, `r21-camtilt`, `CameraMouseInput::rotatePress`):** a
ferramenta de girar/inclinar é o botão do meio, o direito com Ctrl (ou
Option, nosso, mantido) e o esquerdo com Ctrl sobre o lote (nosso: no
trackpad ou num mouse de um botão o Ctrl-clique faz o papel do direito; o
AppKit o entrega como `mouseDown` com Ctrl). O arrasto manda (dx, dy) ao
pedido de girar (§1.4: só o eixo dominante; horizontal gira, vertical
inclina dentro da faixa de pitch do nível). O Ctrl + esquerdo que não se
mexe (≤ 3 pontos) é repetido como clique comum ao soltar (menu de pie); a
UI (puck, painel do tutorial) e as ferramentas com Ctrl do modo Construir
pegam o clique antes. Antes só o Option + direito girava: Ctrl + direito
fazia o pan.

## 2.6 O que o clique pega (binário, `cTSWinSceneGraph::ObjectAtGameWindowPoint`, BV `0x1f876c`; rodada 14)

`cTSUserToolBase::ObjectAtGameWindowPoint` chama o da janela da cena
(vtable `0x5c`). Ela faz o raio pela janela (vtable `0x2d0`) e pede ao grafo
de cena o **nó mais perto** (vtable `0x16c`, devolve o nó e o parâmetro `t`),
em laço:

- o nó é de um objeto (QueryInterface `0xFA1C39ED` subindo pelos pais) → é
  esse (Sim ou objeto: nenhum tem preferência; o mais perto ganha);
- tipo `0x12` (piso/chão): acha o local do ponto; se lá há piso
  (`FUN_0016b59c`: tile de piso, ou "floorable" com o parâmetro, e plano
  quando acima do chão) **para sem nada**; sem piso (abertura de escada) e
  com um objeto no local (`ObjectAtLocation`, flag `0x62` bit 0) → esse;
  senão segue o raio um pouco além e repete;
- tipo `0x13` (parede?): segue, mas o próximo nó que não for objeto para;
- tipo `0x19`: para.

No app (`PieMenuHost::pick`): o mais perto entre os Sims (triângulos
posados) e os objetos (triângulos do modelo, com a máscara de alfa), e
`floorHit` (`main.mm` `floorUnderRay`: o primeiro piso dos andares
mostrados, de cima para baixo, onde o tile tem piso) corta o raio (5 cm de
folga para o que está deitado no piso). Antes os Sims vinham antes de tudo
e o raio atravessava o piso: clicar no piso de cima pegava o Sim de baixo.
Inferido: o "tipo `0x12`" como piso/chão e o `0x13` como parede (pelos usos).

## 2.7 Cursores do mouse (dados + strings do binário; rodada 21, r21-cursor)

**Arquivos:** `TSData/Res/UI/Cursors`, cursores do Windows (`.cur`; a ampulheta
`Hourglass_8.ani` é animada). Quase todos têm 32×32; os `_8` usam 8 bits com
paleta e os `_1` e os sem sufixo são preto e branco.

**Tabela de nomes e ids (rodada 22, lida no binário):** a
`cTSUIServiceManager::SetupCursors` (BV PPC `0x253260`) registra cada nome
com um **id explícito** (a regra "id = índice + 18" da rodada 21 era um chute
e estava errada, só coincidia para alguns): 1 arrow, `0xF` hourglass (`_7`,
a ampulheta animada), 3 edit, `0x12` finger, `0x15` simsdelete, `0x16`
simslevel, `0x17` simslower, `0x18` simsraise, `0x19` simsmove, `0x1A`
simsplace, `0x1B` simsrotate, `0x1C`-`0x1F` simsrotate nw/ne/sw/se,
`0x20`-`0x27` left, right, up, down, upright, upleft, downright, downleft,
`0x28` explore, `0x29` deadexpl, `0x2A` pan, `0x2B` livenothing, `0x2C`
liveperson, `0x2D` liveobjectavail, `0x2E` liveobjectunavail, `0x2F` addpts,
`0x30` movein, `0x31` bulldoze, `0x32` evictFamily, `0x33`
Cursor_EvictStudents, `0x34` moveLotToCatalog, `0x35` taxi, `0x36` simsflip,
`0x37` removepts, `0x38` eyedropperTool, `0x39` designTool, `0x3A`
railingtool e, só no Bon Voyage, `0x3B` forsale e `0x3C` Sledgehammer (a pasta
do jogo base não tem arquivo para esses dois). O arquivo de cada nome é
`<nome>_8.cur`; se não existir, `<nome>_1.cur`; se não, `<nome>.cur`
(inferido pelos sufixos, "_8"/"_1" vêm do bit de cor do monitor). O hotspot
de cada um está no `.cur` (os de rotação do lote e do mover ficam em (0, 0), os
das setas de borda no meio). `hand_8`, `closedhandgreen/red_8`,
`worldgreen/red_8`, `rotateview`, `selectsim` e `walkhere` existem na pasta,
mas não estão na tabela (o jogo base não os usa). `simsdelete` é um sinal de
"proibido" (círculo cortado), não a marreta.

**Como as ferramentas escolhem (binário, rodada 22).** A ferramenta troca o
cursor com `Set2DCursor(id)` (`cTSUserToolBase`, slot virtual `0x60`; por
dentro `+4` do gerenciador, slot `0x44` = `cTSWinSceneGraph::GZSetCursor` /
`SetCursorAtPriority`); `OnButtonUp` do martelo repõe `Set2DCursor(slot 0xE4)`,
que devolve o id da ferramenta (`li r3,0x3c`). Ids literais achados no código
das ferramentas (varredura de todos os `li r4,id` antes da chamada):

| Ferramenta (BV) | id | Onde |
|---|---|---|
| `cTSUserToolObjectDesign::Activate` e `cTSUserToolFloorPattern::OnAnyMouseEvent` | `0x39` designTool | a mesma família de ferramentas Design (a de piso usa o mesmo cursor) |
| `cTSUserToolStairsRailing::Init` | `0x3A` railingtool | ferramenta de corrimão |
| `cTSUserToolSledgeHammer` (slot `0xE4`, `OnButtonUp`) | `0x3C` Sledgehammer | só no BV |
| `cTSUserToolEyedropper`/`cTSUserToolNHoodEyedropper` | `0x38` eyedropperTool | conta-gotas (Comprar e Decorações do bairro) |
| `cTSUserToolRoof::OnMove` | `0x1B` simsrotate | enquanto o arrasto passa de 16 pixels (escolhe a direção do telhado) |
| `cTSUserToolObject::Activate`/`TryGrab`, `cTSUserToolFireplace` | `0x1A` simsplace | mais o cursor guardado no campo `+0xC8` da ferramenta de objeto |
| `cTSUserToolPlopNeighborhoodObject` | `0x19` ao mover, `0x1A` ao apertar | ferramenta de Decorações do bairro |
| `cTSUserToolMoveLot` (neighborhood-mac §15.1) | `0x19`, `0x1A`, depois `0x1C`/`0x1E`/`0x1F`/`0x1D` para a frente 0/1/2/3 | nw, sw, se, ne |
| `cTSUserToolCameraHandDrag::Activate` | `0x2A` pan (1 = arrow ao sair) | a ferramenta "mão" da câmera |
| `cTSUserToolCameraMiddleClick::ShowRotateCursor` | nenhum cursor | cria uma janela de interface (recursos `0xCF092010/11`, `0x20732180`) e a move com o mouse |

Os cursores de parede, piso, telhado (sem arrasto), cerca, fundação, piscina e
escada não aparecem: essas ferramentas desenham um marcador 3D no chão
(`cTSUserToolGroundCursor`, `wallDrawCreate`, `halfWallDrawCreate`,
`fenceDrawCreate`, `glassWallDrawCreate`... `SetupCursorsByStyle`), então o
cursor do mouse fica a seta. Não achei no binário quem escolhe `liveperson`,
`liveobjectavail` e companhia (nenhum `li` literal; o id vem de dado) nem as
setas de borda (`left`...`downleft`; `DoWindowEdgeScrolling` não os põe por
literal): esses mapeamentos abaixo são **inferidos** pelos nomes.

**No app** (`app/game_cursors.*`: leitor de `.cur` e escolha; `main.mm`
cria os `NSCursor` com o hotspot do arquivo e troca o cursor a cada evento
do mouse; os ids de `gameCursorId` são os da tabela acima):

| Estado | Cursor | Fonte |
|---|---|---|
| Sobre a UI (painéis, puck, janela do tutorial, pie, diálogo, painel de Construir, painéis do bairro), menus | `arrow_8` (a seta azul do jogo) | |
| Carregando | `HOURGLASS.cur` (o `.ani` não é lido) | id `0xF` |
| Comprar, mão sobre um objeto que ela pega | `simsmove_8` (mão aberta) | analogia ao mover (inferido) |
| Comprar, segurando um objeto | `simsmove_8` (a figura do passo 20 do tutorial) | tutorial |
| Comprar, botão apertado sobre o que segura | `simsplace_8` (mão fechada) | `0x1A` na ferramenta de objeto |
| Comprar, o arrasto já girou o objeto | `simsrotate_8` | inferido |
| Conta-gotas / Ferramenta Design (Comprar) | `eyedroppertool_8` / `designtool_8` | `0x38` / `0x39` |
| Comprar, mão sobre o vazio | `arrow_8` | o mover de lote põe o cursor 0 |
| Construir, ferramenta Design | `designtool_8` | `0x39` |
| Construir, ferramenta de corrimão | `railingtool_8` | `0x3A` |
| Construir, telhado, enquanto arrasta | `simsrotate_8` | `0x1B` (`cTSUserToolRoof::OnMove`) |
| Construir, terreno: levantar / baixar / nivelar | `simsraise` / `simslower` / `simslevel` | **inferido** pelos nomes (o pincel 3D do jogo também aparece) |
| Construir, porta ou janela na mão | `simsmove_8`, como em Comprar | inferido |
| Construir, o resto (parede, quarto, piso, cerca, fundação, piscina, escada, tinta) | `arrow_8` | o jogo usa um marcador 3D |
| Ctrl em Construir (apagar) | continua `arrow_8` | a marreta do jogo (`0x3C`) é do BV; `simsdelete` é "proibido" |
| Viver, mouse sobre uma pessoa / um objeto com ações / um objeto sem ações / o resto do mundo | `liveperson_8` / `liveobjectavail_8` / `LiveObjectUnavail` / `livenothing_8` | **inferido** pelos nomes (um conjunto de setas com um selo) |
| Borda da janela (câmera rola, qualquer modo com câmera, fora da UI) | `left`/`right`/`up`/`down`/`upleft`/`upright`/`downleft`/`downright` | **inferido** pelos nomes |
| Botão direito arrastando a câmera (sem Ctrl/Opção) | `Pan` | `0x2A` é o da mão da câmera (`CameraHandDrag`); aplicar ao arrasto do botão direito é **inferido** |
| Girar a câmera (Ctrl + direito, botão do meio) | `arrow_8` (o cursor fica preso) | o jogo mostra uma janela, não um `.cur` |
| Bairro, ferramenta Mover Lote, mouse sobre um lote | `simsmove_8` | `0x19` (§15.1) |
| ... botão apertado no lote | `simsplace_8` | `0x1A` (nosso lote já fica na mão no clique) |
| ... com o lote na mão (ou lote novo / da caixa), botão solto | `arrow_8` | cursor 0 |
| ... botão apertado, antes do arrasto girar | `simsrotate_8` | `Set2DCursor` sem id lido: **inferido** |
| ... botão apertado, frente 0 / 1 / 2 / 3 | `SimsRotateNW_8` / `SW_8` / `SE_8` / `NE_8` | `0x1C` / `0x1E` / `0x1F` / `0x1D` |
| Bairro, Decorações | `simsmove_8` (conta-gotas: `eyedroppertool_8`) | `0x19` / `0x38` |
| Bairro, Mudar-se / Despejar / Demolir / Mandar para a caixa, sobre um lote | `MOVEIN` / `EvictFamily_8` / `bulldoze_8` / `MoveLotToCatalog_8` | **inferido** pelos nomes |

Pixels mascarados ficam transparentes; nos de 1 bit, os pixels que invertem a
tela viram preto (o Mac não tem cursor que inverte). Teste: `test_cursors`
(a escolha por estado, os ids, e com `OS2_DATA` todos os arquivos: tamanho
32 × 32, hotspot, forma); folha de contato com os 33 cursores usados
`OS2_CURSOR_SHEET=<png> test_cursors`, em
`docs/screenshots/r22-cursor-folha.jpg` (a de rodada 21 tinha 7).

## 3. O que falta

- O nome do modificador `0x01000000` dos códigos de botão.
- A montagem da matriz de projeção (para confirmar FOV vertical).
- A tabela de offsets do item "mais" e a cor de desabilitado.

### Teclado conferido (rodada 15, r15-pool-draw)

A passada 11 do QA viu as setas e `a`/`d` girarem a câmera. O código segue as
tabelas acima (setas e WASD → `LotCamera::pan`, `,`/`.` → `rotateHeld`) e o
passo headless `--key hold-<left|right|up|down|a|d|w|s|comma|period>:<s>`
(a tecla segurada quadro a quadro de 50 ms, como o laço de desenho) mostra o
pan sem giro: `key left held 0.50 s: pivot (15.00, 25.00) -> (35.74, 10.48),
yaw 55.0 -> 55.0`, e só `comma` muda o yaw. Não reproduzido sem janela; o
pan é rápido (`keyboardtranslationspeed` 50 px por quadro de 50 ms) e com a
perspectiva pode parecer um giro. Teste: `test_camera` "pan right".
