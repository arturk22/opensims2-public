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
  pódio olham para o Sim com o +Y). Quem pareia spec e localizador é o
  SimAntics do objeto "CAS - Backdrop" (efeitos de câmera
  `CASCam_<spec>_Effect` nos ossos `camera_*` do estúdio; `docs/cas-mac.md`,
  rodada n21): família → `CAS_Family` + `camera_family_0`; passos de
  cabeças, cabelo e rosto e maquiagem → `CAS_Face<idade>` +
  `camera_face_<idade>`; os outros → `CAS_Clothes<idade>` +
  `camera_clothes_<idade>`; bebê como criança pequena, idoso como adulto
  (conferido na rodada n21).
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
corta nas capturas roteirizadas. Os botões de zoom do CAS dão zoom enquanto
seguros (`keyboardzoomspeed` × 50 / Δt por atualização); a roda, entalhes ×
`mousewheelsensitivity` (os dois lidos na rodada n21, `docs/cas-mac.md`). Sem o estúdio (`--cas-no-studio`) fica a
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
  `centerTrackingDeadZoneMagnitude` (30 por padrão): seguir um Sim, ver §1.8
  (rodada n11: é o alvo do pivô que se compara, não o pivô atual).
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

| item do `TODO.md` ou do `docs/TODO-HISTORICO.md` | jogo |
|---|---|
| yaw 0 = +X | heading h põe o olho em (cos h, sin h) do pivô; a câmera olha para −(cos h, sin h) |
| distância da zoom table horizontal | distância 3D ao longo da visão |
| suavização em ticks de 30 Hz | exponencial em quadros de 50 ms com velocidade mínima |
| pan ∝ zoom | sim, por ser em pixels de tela |
| FOV vertical | **horizontal** (binário, BV `0xccd254`); em janelas mais largas que 4:3 mantemos a altura do 4:3 (nosso) |
| limitar ao lote | lote encolhido de `constraintwidth` (−10 = até 10 tiles para fora) |
| seguir altura do piso/Sim | sim: andar do Sim e amostra de altura do piso |

### 1.7 Vista de cima (T), a câmera "Build" (binário, rodada n11)

**Teclas** (`ui.package` `0xA2E3D533`, grupo `0x8CA033A2`): `T` = "Top down view"
(`0x2CEE0269`), `Tab` e `Y` = "Cycle Camera Controller" (`0x0CA8D3F3`, §1.8),
`Space` e `N` = "Select Next Sim" (`0x4CA29B60`), `Return` = "Center on active
Sim" (`0xACECA4EE`). Só o arquivo `0x6A9362F5` (o do modo vivo e de construção)
tem `T` e `Y`; o de bairro (`...F3`) e os de CAS não têm.

**Quem trata o T.** Só os painéis de Comprar e de Construir: `cTSWinProcBuildPanel::
DoMessage` (BV `0x286f80`) e o de Comprar (`0x22bc8c`) respondem a `0x2CEE0269`
trocando o estado do botão "vista de cima" (Construir `0xA3`, Comprar
`0xCC607036`) para o contrário de `cITSMainToolbar::GetTopDown`; o
`onBtnToggledID` do painel (`0x27fd94` e `0x223ca4`) chama `SetTopDown(estado)`
da barra principal (vtable `0x10c`). No modo Viver nenhum painel trata a tecla:
**T não faz nada no Viver**.

**`cTSMainToolbar::SetTopDown(bool)`** (BV `0x1a9674`; o slot `0x138` da
vtable primária é o mesmo código): chama o gerente de câmeras
(`cTSCameraControllerManager::SetCurrentController(char*)`, vtable `0x54`)
com a string em `0x101a328` ("Build") se ligar, ou `0x101a330` ("Live") se
desligar; ao ligar pela primeira vez no jogo roda uma vez
`Update(-1)` do gerente e um método da barra (vtable `0xc0`); por fim guarda o
bool em `+0xa8`.

**`cTSMainToolbar::SetMode(modo, bool)`** (BV `0x1ade24`), para os modos 2
(Viver), 3 (Comprar) e 4 (Construir): com o flag `+0xa8` desligado chama
`SetTopDown(0)` (e, em Comprar e Construir, anota `+0x11a`, a visita; ao
voltar ao Viver ela dispara uma atualização no `WorldDB`, não de câmera);
com o flag ligado, Comprar e Construir chamam `SetTopDown(1)`, e Viver chama
`SetTopDown(0)` **e põe o flag de volta em 1**: o Viver mostra a câmera "Live",
mas o próximo Comprar ou Construir volta à vista de cima. Código:
`CameraSelector` (`app/camera_mode.hpp`; teste `test_camera`).

**A câmera "Build".** `cTSCameraParserState::Init` (BV `0x85a3cc`) registra
`SmoothSims1`, `Neighborhood`, `Live` e `Build` com o mesmo tipo (`0xaf75334`):
"Build" é um `cCameraLive` com a configuração do `BuildCamera.txt`
(`zoom 1 (75 32 24 16 8)`, `fov (33.2 28.6 24.1 23.0 21.8)`, pitch fixo de
84° em todos os níveis, orientações de 45 em 45 graus a partir de 0, sem
`pivotheightabovefloor`, `constraintwidth` nem `defaultcamerapitch`: valem os
padrões do construtor `0x6a5154`, `constraintwidth 0` (o pivô fica dentro do
retângulo do lote, sem os 10 tiles de folga do Live) e `defaultcamerapitch`
0,15). **Inferido:** `pivotheightabovefloor` do Build; o construtor do
`cCameraLive` não o inicializa (`+0x490`), o app usa 1 como no Live (a 84°
a diferença é de décimos de tile).

**A troca.** `cCameraLive::SetActive` (BV `0x6a56e0`), quando o controlador
anterior é "Build" ou "Live" (nome pelo vtable `0x6c`), chama a cópia de
estado `FUN_0086bcd4` (vtable `0x114`): copia do anterior, como números, o
rumo (`+0xcc/+0xd0`), a distância 3D (`+0xe8/+0xec`), o FOV, os cortes, a
fração de pitch (`+0x158/+0x15c`) e o vetor do pivô, e em seguida faz o
"snap" (`FUN_0086b7f8`: cada atual recebe o alvo). A distância **não** é
movida para a tabela de zoom da nova câmera (do Live no zoom mais perto, 3,
o Build mostra a 3 unidades); as tabelas de fov, perto/longe e pitch passam a
ser as do Build pelo nível de zoom da distância. Cada controlador guarda
as suas posições de câmera salvas (`SaveCameraPosition`, `0x6a3a08`).
No app: `LotCamera::setDefinition`, `applyCameraSelection` (`live_setup.mm`),
a tecla T (`app_view.mm`), os botões de vista de cima dos painéis de Comprar
e Construir (pressionados enquanto a vista está ligada) e o passo
headless `--key t` (captura: `docs/screenshots/n11-camera-vista-de-cima.jpg`,
Tutorial_Lot14 em Construir, nível de zoom 1 do Build: 40 de distância, pitch 84°).

### 1.8 Seguir o Sim (autotrack) e centrar (binário, rodada n11)

**Quem liga.** O botão **direito** da miniatura de um Sim da família
(`cTSWinProcSimChooserThumbnail::onBtnRightClickedID`, BV `0x298940`; botões
`0x2000`/`0x3000`): se o GUID da miniatura é o que a barra guarda como
"Sim seguido" (`GetAutotrackSimGUID`, `cTSMainToolbar+0xb0`), chama
`AutotrackSim(nulo, false)`; senão, se o Sim está no mundo,
`AutotrackSim(Sim, true)`. O botão **esquerdo** numa miniatura já selecionada
(`onBtnClickedID`, `0x298a44`) faz `CenterOnSim` e depois
`AutotrackSim(nulo, false)`; o de uma não selecionada só seleciona.

**`cTSMainToolbar::AutotrackSim(pessoa, bool)`** (BV `0x1ad1f0`): ligando,
chama `CenterOnSim(pessoa)` (vtable `0xbc`), guarda o GUID da pessoa em
`+0xb0` (desligando, zero) e manda ao controlador de câmera atual
(`cITSCameraSmoothSims1Controller`, IID `0xeaf75329`) `SetTracking(bool)`
(`0x86ac2c`: guarda o bool em `+0x1f6`; desligando, esquece o sujeito
atual `+0x1c8`). `AutotrackSelectedSim` (`0x1ad394`) é o mesmo para o Sim
selecionado. O GUID **não** é zerado quando um movimento do jogador acaba
com o seguimento (abaixo), então depois de arrastar a câmera o primeiro
clique direito na mesma miniatura "desliga" (sem efeito) e só o segundo liga.

**`CenterOnSim(pessoa)`** (`cTSMainToolbar`, BV `0x1acbfc`; Return e
`CenterOnSelectedSim`, `0x1a9198`, chamam com o Sim selecionado): se a pessoa
está no mundo, pega o nível do Sim e o cômodo do local
(`RoomContaining`): com cômodo, o andar mostrado passa a ser o do Sim se o
nível dele for > 0; sem cômodo, só se o andar mostrado for menor que o do Sim
(a mesma regra do `cCameraLive::Update`, §1.3). Depois
`SetSubject(nó, "root_trans", 0xf)` no controlador e o pedido `0xbb661105`
com a posição do osso `root_trans`.

**`HandleRequest`** (`cTSCameraSmoothSims1Controller`, BV `0x870cb0`): os
pedidos que movem o pivô chamam antes `SetTracking(false)` (vtable `0x80`):
`0xbb661105` (centrar; depois `0xfc` = o novo alvo do pivô, suavizado),
`0xbb661106` (pular para o ponto: atual, alvo e snap), `0x9b66110a` (arrastar
com o botão direito: ver §2.5) e os dois da movimentação por teclado
(`0x3b6c780f` e `0xbb6c7808`). Girar (`0xbc17e41c`), a roda e o zoom
**não** param o seguimento.

**`Update`** (`0x86ffb8`), com o seguimento ligado, um sujeito e o viewer:
projeta na tela (`cIViewerRefNode` vtable `0x238`) o **alvo** do pivô
(`+0x180..0x188`; o pivô tem atual `+0x174..` e alvo `+0x180..`, pares do
`FUN_008c760c`/`FUN_008c7c64`) e a posição do osso do sujeito menos o
deslocamento do pivô (`+0x1d4..0x1dc`, (0, 0, `pivotheightabovefloor`):
o ponto do piso sob o Sim); zerando o z de ambos, se a distância passa de
`+0x438` (`subjecttrackingdeadzonemagnitude` 30) chama `0xfc` com o ponto do
Sim. Como o que se compara é o alvo, e não o pivô a caminho dele, a câmera
anda em saltos: fica parada até o Sim se afastar 30 pixels do último alvo e
então vai (em `zoomSmoothing` quadros) até onde o Sim está.

No app: `LotCameraFollow` (`setTracked`, `follow`, `centre`) e
`LotCamera::userMoves`/`trackTo`/`moveTo`: o seguimento acaba quando o
contador de movimentos do jogador muda (teclas, borda da janela, arrasto,
centrar); o clique direito centra e depois segue; o esquerdo na miniatura
selecionada centra e para (`live_ui.cpp`); Return centra com a regra de
andar. A comparação usa o ponto do piso do Sim no andar dele
(`Lot::elevationAt`) e o tamanho real da janela em pixels (o jogo, em
800×600: a zona morta de 30 pixels vale mais, relativamente, aqui).
**Inferido:** o osso `root_trans` é a posição lógica do Sim na VM. (O
`0x3b6c780f`/`0xbb6c7808` é o passo da borda da janela, `sqrt(fov·30)`, não da
largura; o teclado vai pelo pedido de mover: §1.11, rodada n39.)

### 1.9 Tab: a câmera livre "CameraMan" (binário, rodada n11)

**Quem troca.** `Tab` e `Y` mandam o evento `0x0CA8D3F3` ("Cycle Camera
Controller"). Quem o trata é o controlador atual:

- `cCameraLive::DoMessage` (BV `0x6a3df0`; vale para a "Live" e para a
  "Build", a mesma classe): se o serviço de estado do jogo (`FUN_000c8370`,
  `vtable 0x14`) é o **lote** (`0xbea28c2`), chama `ShowPIP(false)` na barra
  principal (`0x1599358`, vtable `0xac`) e pede ao gerente de controladores
  (`cTSCameraControllerManager::SetCurrentController(char const*)`, vtable
  `0x54`) o controlador **"CameraMan"**; se é o **bairro** (`0xABEA28C7`), o
  "CameraManNH" (a `cCameraNeighborhood` faz o mesmo, `0x765134`; não feito).
- `cCameraCameraManController::DoMessage` (`0x7a6878`): Tab (`0x0CA8D3F3`) **ou
  Escape** (`0x8CA297E2`, "Cancel") pedem ao gerente o controlador cujo nome
  guarda em `+0x324`; o `SetActive` (`0x7a7d2c`) o preenche com o nome do
  controlador anterior (`vtable 0x6c`) e liga o mapa de teclas `0x6A9362F7`
  (desliga ao sair).
- `cCameraFirstPersonController::DoMessage` (`0x7aeff8`), a "First" (câmera na
  cabeça do Sim selecionado, nenhuma tecla chega a ela): Tab ou `0x8CA297E2`
  voltam a "Live" no lote e a "Neighborhood" no bairro.

Tab vale nos modos Viver, Comprar e Construir (e na vista de cima: o controlador
é um `cCameraLive` em todos): o `CameraSelector` guarda de qual veio (Live ou
Build) e Tab ou Escape voltam a ele; qualquer `SetTopDown`/`SetMode` seguinte pede "Live" ou
"Build" ao gerente e acaba com a câmera livre (`camera_mode.hpp`).

**Teclas na câmera livre** (`ui.package` `0x6A9362F7`, por cima do mapa do
lote): `z`/`x`, `=`/`-`, NumPad `+`/`−` (com ou sem Shift): "Zoom in" /
"Zoom Out" (`0x8D377398` / `0x8D37738A`), um passo na tabela de FOV por
toque; `a d w s` (e as setas, do mapa de baixo): mover (os eventos
`0x4CA2994F` esquerda, `0x8CA29985` direita, `0xACA299BB` frente,
`0x2CA299BD` trás; Shift manda o argumento 2, mas o tratamento
(`FUN_00d913b0`) só põe ±1: **Shift não acelera**); `q` baixa e `e` sobe a
câmera (`0xCCC365B5`, `0xCCC365C9`); `Shift + a d w s` = "orbit"
(`0x2CEBECBD`, `..C1`, `..C5`, `..C8`); F1–F6 "Disabled" (`0x00000001`);
Ctrl+4..9 salva e 4..9 volta às posições 1–6 (`0xACA2982A`, `0xCCA29895`;
`SaveCameraPosition`, BV `0x7a61b0`, guarda além da câmera o **modo de jogo**
e o **andar** atuais e `Restore` (`0x7a626c`) os devolve).

**Números** (`FirstPersonCameras.txt`, "CameraMan"; lidos em
`cTSConfigDeviceSettings::SetDisplayMode` [nome errado do símbolo] `0xd92340`,
a base de primeira pessoa, e `SetCameraSpec` `0x7a5f94`): tabela de FOV `20 25
... 90` (graus), `clip 0.15 2000`, `maxvelocity 10`, `velocitysensitivitymin
0.1`, passos/mudança mínima: translação 7 / 0,01, velocidade 5 / 0,1,
orientação 5 / 0,1, FOV 3 / 0,001, `mincameraheight 1`, `maxcameraheight 50`,
`ticklimit 50`, `constraintwidth -10`, `upperpitchlimit 35`,
`lowerpitchlimit -80`, `mouseheadingsensitivity 0.0005` (a "CameraManNH" tem
0,0015 e alturas 5..500; a "First", 0,00005).

**A conta** (`Update`, `0x7a7ac4`; base `0xd90a4c` e `0xd90664`), uma vez por
quadro com `dt = min(dt, ticklimit)` (o próprio gerente já corta o `dt` em
100 ms, `0x864e94`):

```
escala        = s + (1 - s) * h            s = velocitysensitivitymin, h = fração da altura
alvo_veloc    = maxvelocity * escala * (dir.x, dir.y, dir.z)    dir: direita/frente/subir = ±1
alvo_posição  = restrito(posição + Rz(rumo) * velocidade_atual)  (x direita, y frente, z para cima)
suavizar      = a mesma do §1.2 ao longo da reta (FUN_008c7c64): posição (7, 0,01), velocidade (5, 0,1), FOV (3, 0,001)
altura        : z preso a [piso + 1, piso + 50] sob a câmera (HeightSample); h = (z - lo) / (hi - lo)
```

Cada quadro desloca a posição por `v · dt / (50 · 7)`: com a velocidade já
subida (constante de 250 ms) são `maxvelocity · 1000 / 350 = 28,6 tiles por
segundo` na altura máxima e um décimo disso (2,9) na mínima. A restrição
(`ConstrainTranslation`, `0x7a6330`) é o retângulo do lote encolhido de
`constraintwidth`, só no estado de lote.

**Volta.** Voltar a "Live" (ou "Build") chama `cCameraLive::SetActive`, que
com o anterior "CameraMan" faz `MatchThisControllerWith(viewer)`
(`cTSCameraSmoothSims1Controller`, `0x86c05c`): rumo da visão, pitch preso à
faixa do primeiro nível, distância `(z do olho − pivotheightabovefloor) /
sin(pitch)` presa entre o primeiro e o último nível, o pivô a essa distância
ao longo da visão, o nível de zoom e a fração de pitch daí. No app:
`LotCamera::matchView`.

**No app:** `FreeCamera` (`render/free_camera.*`, a conta acima, testada em
`test_camera`), `CameraSelector` (Tab, Y, Escape), `applyCameraSelection`
(`live_setup.mm`; o `LotCamera` passa a `setFirstPerson`), as teclas e o olhar
em `app_view.mm`, os passos headless `--key tab`, `--key hold-<w|a|s|d|q|e>:<s>`,
`--key look:<dx>,<dy>` e `--key fov:<±n>` (captura:
`docs/screenshots/n11-camera-camera-livre.jpg`, a 4 de altura, olhando o alpendre da `Tutorial_Lot14`). Sem a câmera livre o seguimento do
andar do Sim (`followSelectedSim`) pára (é outro controlador).
**Inferido:** o pitch preso a 35°/−80°; a posição e a direção de partida (os
da câmera que sai) e o FOV de partida (o mais perto de 45 da tabela). O olhar
com o mouse, a órbita e a "CameraManNH" foram feitos na rodada n39 (§1.11).
**Não feito:** a "First" (nenhuma tecla chega a ela), o `ShowPIP(false)` e o
modo/andar nas posições salvas.

### 1.10 Fade de objetos perto da câmera (`objectFade*`; binário, rodada n11)

**Parâmetros** (`LiveCamera.txt`): `objectFadeDistance 10`, `objectFadeFalloff
4`, `objectFadeAllObjects 0`, `objectFadeCineCam 0`; a câmera "Animated" das
cinemáticas (`CineCamCamera.txt`) tem 1, 2, 0 e 1; as câmeras Build, CameraMan
e as outras não têm nenhum. Quando o controlador atual ganha o seu
`cTSCameraSpec`, o `cTSSGSystem` (função em `0x7bee00`, dentro do que o
Ghidra não definiu como função) lê os quatro: `+0x364` distância, `+0x368`
queda, `+0x36c` = (allObjects > 0), `+0x36d` = (cineCam > 0 e a flag `+0x165`
do sistema), e `+0x360` = se `objectFadeDistance` existe.
`cTSSGSystem::ObjectFadeParams` (BV `0x7b35c4`) devolve os cinco campos a
quem pergunta (0 sem `+0x360`) **só no estado de lote** (`0xbea28c2`).

**Quem usa.** `cObject::UpdateVisibilityAndRendering` (BV `0x661e5c`, uma vez
por quadro e objeto): sem a câmera de cinema (`+0x36d` = 0), o objeto só entra
na conta se `allObjects` ou se o dado de objeto 40 ("Flag Field 2", STR#
`0xD6` do `objects.package`) tem o **bit 3, "Obstructs View"** (`0x8`). O ponto
é o **centro da caixa envolvente em mundo** (`vtable 0x118` do objeto,
`FUN_000e7468`), levado ao espaço do viewer (`cIViewerRefNode` `vtable
0x230`: posição e quaternion): a profundidade é o `y` desse ponto, a
distância ao longo do eixo de visão (o viewer olha para o +Y local).

```
alpha = (profundidade / distância) ^ queda      se 0 < profundidade < distância
alpha = 1                                        senão
```

(com 10 e 4: a 5 tiles do olho 6%, a 8 tiles 41%, de 10 em diante inteiro).
Depois `ParentFadeAlpha` (`0x662c30`) pode baixar o alfa ao do objeto pai
(só para quem passou pelo teste acima). Com alfa < 1 o objeto vai para o
estilo de renderização 4 (`SetRenderStyle`, `0x65e7ec`: mistura por alfa) e a
cor ARGB do objeto (`SetRenderARGB`, `0x65edd4`) recebe o alfa vezes o
original. No jogo base os objetos com "Obstructs View" são, por exemplo, as
árvores (`Tree - Birch`) e as luminárias de teto (`Lamp - Ceiling`): na
`Tutorial_Lot14`, 4 objetos, e no zoom mais perto duas luminárias de teto ficam
a 66% e 3%.

**Câmera de cinema** (`+0x36d`; lido na rodada n39, não feito): quem entra na
conta é outro teste — todo objeto sem os bits 0 e 2 do `+0x108` (com
`allObjects`, todos) — e o alfa sai de `FUN_0065deb0` (BV `0x65deb0`; o sistema
dá o olho `0x140`, a direção de visão `0x144` e uma caixa `0x148`):

```
só se |posição do objeto − olho| < distância + 4 e o objeto não tem o bit 1 de +0x108 (… & 2 do +0x42·4)
t      = (centro da caixa do objeto − olho) · direção          (a profundidade do centro)
razão  = 1;  t < 1: t' = max(t, 0,1), razão = t' / (5·(1 − t') + 1)   (t < 0,1: 1 − t' = 0,9)
t > distância: razão = min(2·(t − distância) + 1, 4) e t = distância
P      = olho + direção · t ;  r = max(t · queda, 0,5)
se |centro − P| < r + |meia-diagonal da caixa|:
    f  = a fração do segmento centro → P que fica fora da caixa (os planos das faces, 1 − entrada)
    s  = f · razão · |centro − P|
    s ≥ r: alfa = 1
    senão a = clamp((s − r/4) / (3r/4), 0, 1); g = min sobre os eixos da folga entre a caixa 0x148 e a do
          objeto no sentido da visão; g > 0,1: b = 1 − min(g − 0,1, 0,4) · (1 − a) / 0,4 (b < 1 conta)
    alfa = max(a, b)
senão alfa = 1
```

Depois o `ParentFadeAlpha` (abaixo) e, só com a câmera de cinema, os bits do
`+0x108`: bit 1 esconde sempre; sem o bit 3 o objeto some com alfa 0; com o
bit 3 é tudo ou nada com histerese (some em ≤ 0,75 e marca o bit 4, volta em
≥ 1), e o alfa vira 0 ou 1. Quem preenche a caixa `0x148` e os bits do
`+0x108` não foi lido (as opções das cenas, `docs/cinematics-mac.md` §8:
"ObjectHiding"); sem isso a conta não pode ser feita, e o app segue sem fade
durante as cinemáticas.

**`ParentFadeAlpha`** (BV `0x662c30`, rodada n39): para quem entrou na conta, o
alfa vira o mínimo entre o seu e o do objeto que o contém (o `0x1f8` do pai
depois do seu próprio `UpdateVisibilityAndRendering`; 1 sem pai ou se o pai
não é um `cObject`). Feito no app (`lot_actors.cpp`: o contêiner é o dado de
objeto 2, e conta só se a regra também o desbota).

No app: `object_fade.hpp` (a regra, sem Metal, testada em `test_camera`),
`Renderer::setInstanceAlpha`/`instanceBounds` (alfa por instância: mistura
por alfa, sem escrever a profundidade, o alfa dos materiais vezes o fade) e
`updateObjectFade` (`lot_actors.cpp`, a cada `poseActors`; a linha `object fade:
...` do log e `OS2_FADE_LOG=1` mostram o que desbota). **Inferido:** que o
eixo de profundidade do viewer é o `y` local (o código usa o segundo
componente do ponto no espaço do viewer; o viewer olha para +Y como o resto do
jogo).

### 1.11 Teclado, borda da janela, giro e órbita (binário, rodada n39)

Ghidra MCP (`sims2_bv.ppc`, só leitura); as constantes em `.bss` saíram do
inicializador estático `0x2170d0c..0x2170d7c` (lido com um desmontador próprio,
o Ghidra não tinha instruções ali).

**Os campos do controlador** (`cTSCameraSmoothSims1Controller`; cada valor
suavizado é a estrutura do `FUN_008c77d4`: `+4` passos, `+8` mínimo², `+0xc`
mínimo, `+0x10` **atual**, `+0x14` **alvo**, `+0x18` iniciado; a vetorial do
`FUN_008c7c64` tem atual `+0x10..0x18`, alvo `+0x1c..0x24`):

| campo | o que é | passos / mínimo |
|---|---|---|
| `+0xbc` (atual `+0xcc`, alvo `+0xd0`) | rumo | `smoothing` (2º número, `SetSpinDuration` `+0xc0`) / `defaultminchange` |
| `+0xd8` (atual `+0xe8`) | distância 3D | `smoothing` (1º, `SetZoomDuration` BV `0x86af20`, que põe o mesmo N em `+0xdc`, `+0xf8`, `+0x114` e `+0x1bc`) |
| `+0x148` (atual `+0x158`) | fração do pitch | 7 (`Reset`) |
| `+0x164` (atual `+0x174`, alvo `+0x180`) | pivô (ponto do piso) | **7 sempre** (`Reset` BV `0x86d688`; nada mais o muda) / `defaultminchange` |
| `+0x1b0..0x1b8` | velocidade do pivô (mundo) | — |
| `+0x1c0` | contador de repetição dos passos do teclado/borda | 1 no `Reset` |
| `+0x200` (atual `+0x210`) | velocidade do giro pelo teclado (px) | `keyboardrotationlerpsteps` / `...minchange` (`Reset`: 20 / 0,05) |
| `+0x21c` (atual `+0x22c`) | velocidade do zoom pelo teclado | `keyboardzoomlerpsteps` / `...minchange` |
| `+0x238` (atual `+0x248..`) | velocidade da translação pelo teclado (px) | `keyboardtranslationlerpsteps` / `...minchange` |
| `+0x264`, `+0x268`, `+0x26c`, `+0x270` | `keyboardrotationspeed`, `keyboardzoomspeed`, `keyboardzoomscale`, `keyboardtranslationspeed` (`SetCameraSpec` BV `0x86f310`) | |

**Teclado: as teclas dos aceleradores** (`ui.package` `0x6A9362F2`/`F5`:
setas e `a d w s` "Start/Stop Move ...", argumento 1, Shift + seta 2).
`cTSCameraSmoothSims1Controller::DoMessage` (`FUN_00871c0c`) só mexe no
**alvo da velocidade em pixels** (`+0x238`): esquerda `x = −speed·arg`, direita
`x = +speed·arg`, frente `y = −speed·arg`, trás `y = +speed·arg`, o "Stop" do
eixo zera-o (`speed` = `keyboardtranslationspeed`, 50). O `Update` (BV
`0x86ffb8`), a cada atualização: guarda o valor atual, suaviza (`dt`), e **se o
valor de antes não era zero** manda o pedido de mover `0x9b66110a` com o valor
novo (o mesmo do arrasto com o botão direito). Depois chama
`UpdateSubjectMotion` (vtable `0x100`, BV `0x86b9d4`).

**O pedido de mover `0x9b66110a`** (`HandleRequest`, BV `0x870cb0`): projeta o
pivô **atual** (`+0x174`) na tela (viewer `0x238`), soma `(dx, dy)` pixels na
mesma profundidade, desprojeta os dois (viewer `0x240`), leva a diferença ao
espaço do viewer (a rotação do viewer, `0x230`; x direita, y fundo, z para
cima) e põe no chão pelo rumo atual `h` (+ o deslocamento `+0x1f0`, 0):

```
vel = −lz · (cos h, sin h) + lx · (−sin h, cos h),   vel.z = 0     (+0x1b0..0x1b8)
```

Ou seja, a parte "para cima na tela" vira "para a frente" sem esticar pelo
pitch (não é um raio contra o piso). O pedido **guarda uma velocidade**, não
desloca nada; chama `SetTracking(false)`.

**`UpdateSubjectMotion`** (BV `0x86b9d4`): com `|vel| > 0,001` o alvo do pivô
vira `pivô atual + vel · N` (`N` = `+0x1bc`, o `smoothing` do zoom: 5 no Live e
no Build), passando pela restrição do lote (`SetSubjectTarget`, vtable `0xfc`,
`FUN_006a456c`); senão reaplica o alvo. Com o pivô suavizado em 7 passos de
50 ms, a velocidade de regime é `vel · N / (7 · 50 ms)`: com `vel` = 50 px na
profundidade do pivô, FOV 45° numa tela de 800, a 40 de distância, ~29 tiles/s.

**Borda da janela** (`cTSWinSceneGraph::DoWindowEdgeScrolling`, BV `0x1fbe2c`,
chamada pelo `GZPaint`, **uma vez por quadro**): margem de 4 px, ou de 16 px
quando já rolava num dos eixos (histerese); à esquerda `h = +rate`, à direita
`−rate`, em cima `v = +rate`, embaixo `−rate`, `rate` = `horzedgescrollrate` /
`vertedgescrollrate` (padrão **1,25**, registrados em `0x1ff3a0`; o binário de
Mac tem as duas strings). Enquanto um eixo é não nulo, ou era no quadro antes,
manda o pedido do eixo com o valor (0 uma vez ao sair da borda): `0xbb6c7808`
(lado) e `0x3b6c780f` (frente).

**Os pedidos de passo `0x3b6c780f` / `0xbb6c7808`** (`HandleRequest`):
`SetTracking(false)`; `+0x1c0 += 1`, e com o eixo 0 volta a 1; então

```
passo = sqrt(fov · 30) · (−eixo · distância · k) · contador     fov em radianos (viewer, volume 0, +0x3c)
frente (0x3b6c780f, k = 0,0032):  alvo do pivô = pivô atual + (cos h, sin h) · passo
lado   (0xbb6c7808, k = 0,002):   alvo do pivô = pivô atual + (−sin h, cos h) · passo
```

(`k` = `DAT_02c426dc` / `DAT_02c426d8`). Como o contador só sobe enquanto os
pedidos chegam, a borda **acelera**: um passo maior a cada quadro (depende da
taxa de quadros, como no jogo). Com o eixo 0 o alvo vira o pivô atual: a câmera
pára onde está.

**O mapa de dispositivo** (`FillDeviceMap` do controlador, BV `0x872450`;
`cTSCameraControllerManager::CreateDeviceMap` `0x8656cc`, usado pelo
`DoMessage(cGZMessage&)` do gerente `0x8655f0`, que recebe as mensagens cruas
do `cTSWinSceneGraph::DoMessage` `0x1f67d0`): teclas virtuais do Windows —
`VK_LEFT`/`VK_RIGHT` → `0xbb6c7808` com +1/−1 (Shift ±2), `VK_UP`/`VK_DOWN` →
`0x3b6c780f` com +1/−1 (±2), nos tipos 1 e 2 (descer/repetir), 0 no tipo 4
(subir); `=`/`-`/NumPad± → zoom de um nível; `,`/`.` → orientação anterior/
seguinte; 4..9 com Ctrl/nada/Shift → posições de câmera. **Inferido:** as setas
nunca chegam aqui no lote, porque o acelerador (`cRZWinKeyAccelerator::
ProcessAccelerator` `0x93116c`, procura exata por tecla + modificadores em
`0x9313c0`) as consome antes; o app não faz este caminho.

**Giro pelo teclado.** `,` / `.` mandam "Start Rotate Left/Right"
(`0x0CA29915` / `0x4CA2991E`) e os "Stop" ao soltar. `cCameraLive::DoMessage`
(BV `0x6a3df0`) **troca os dois** quando a opção `rotateCameraSims1Mode` (barra
principal `+0xad`, interface vtable `0x88`; padrão **0**, registrada em
`0x1af87c`) está desligada. Na base: Left → alvo da velocidade `+0x200` =
`−keyboardrotationspeed`, Right → `+`. O `Update` manda a cada atualização o
pedido de girar `0xbc17e41c` com `dx = int(velocidade atual)`, `dy = 0`.

**O pedido de girar `0xbc17e41c`**: se `dx` e `dy` são não nulos, com
`|dx/dy| < 1` só `dy` conta, com `> 1` só `dx`, e com `= 1` os dois; o alvo do
rumo = **rumo atual** − clamp(dx · 0,003, ±3) (não o alvo anterior), e o da
fração do pitch = clamp(**atual** − dy · 0,005, 0, 1). Com 200 px, o alvo fica
0,6 rad à frente do rumo atual e o rumo anda `0,6 · dt / (50 · 8)` por
atualização: **1,5 rad/s** (86°/s), sem depender da taxa de quadros. Padrão
(opção desligada): `.` aumenta o rumo, `,` diminui.

**Giro pelo mouse** (`cTSUserToolCameraMiddleClick::MouseEvent`, BV
`0x34cf1c`): `dx` desde o clique é **negado** com `rotateCameraSims1Mode`
desligado (o padrão), então arrastar para a direita aumenta o rumo.

**Zoom pelo teclado** (`=`/`-` "Start/Stop Zoom in/Out", `0x6D41F7AB`/
`0xAD41F8D8`): alvo da velocidade `+0x21c` = ±`keyboardzoomspeed` (1). O
`Update` guarda o valor antes, suaviza e, se o de antes não era zero, manda o
pedido da roda `0x9c17e415` com a posição `int(roda + valor)` e a
sensibilidade trocada por `keyboardzoomscale` (2): **por atualização**,
`d += Δ · 2 · (d + 10) / −50`, com `Δ` = ±1 (`keyboardzoomlerpminchange` 1 faz o
valor saltar para ±1).

**Zoom de um nível depois da roda.** O pedido da roda deixa o nível de zoom
(`+0x70`) no primeiro nível não mais longe que a distância a que chegou e liga
o flag `this[500]` (com ele o `Update` usa a distância da roda, `+0x74`). O `z`
(`0x9b65fb1f`) com o flag ligado só o apaga: a câmera vai àquele nível (nada,
se a roda parou num nível, como no mais longe) e só o `z` seguinte avança. O `x`
(`0x7b65fb6b`) chama `SetZoomLevel(nível − 1)` (BV `0x86b74c`, que prende o nível
à tabela e apaga o flag): o nível mais longe seguinte, como antes. No app:
`LotCamera::zoomBy`/`wheelBy`.

**A câmera "Build"**: `cCameraLive::Reset` (BV `0x6a3c70`) põe
`pivotheightabovefloor` (`+0x490`) = **1,2** antes de ler a especificação; o
`BuildCamera.txt` não tem o parâmetro, então o Build usa 1,2 (era inferido 1).
O mesmo `Reset` põe `+0x440..0x44c` = 0,5, 0,5, 6, 2 (os padrões da amostra de
altura do piso).

**A órbita da câmera livre** (Shift + `a d w s`, "Start orbit left/right/up/
down" `0x2CEBECBD/C1/C5/C8` do mapa `0x6A9362F7`). `cCameraCameraManController::
DoMessage` (BV `0x7a6878`): cada um desses eventos (também os repetidos da
tecla segurada) chama `PickOrbitCenter` (BV `0x7a6590`: um raio do olho ao
longo da frente do viewer, `(0, 1, 0)` girado pela orientação, contra a cena,
`+0x16c` do grafo; o ponto mais perto atingido) e liga `+0x2f0` se atingiu; os
"Stop Move ..." (`a d w s` soltos, com qualquer modificador) e os de subir/
descer o desligam. Os eventos de órbita **não** mexem na direção (`+0x2ac/+0x2b0/
+0x2b4`): só as teclas sem Shift ("Start Move ...", `q`/`e`) a põem. Com `+0x2f0`
ligado, o `Update` (BV `0x7a7ac4`) chama a conta da órbita (BV `0x7a7784`) em vez
da translação:

```
escala  = s + (1 − s) · h                                   (como a translação)
alvo da velocidade (+0x204) = maxvelocity · escala · direção
M       = FUN_000ea0dc(k · vel.z, 0, k · vel.x)             k = π/180 (DAT_02c2ea4c): graus por atualização
r       = M · (centro − posição atual)
alvo da posição = centro + r ;  frente = r (FUN_000ea3a0 com (0, 0, 1) para cima), rumo e pitch tirados dela (FUN_000eaac4)
```

(`FUN_000ea0dc(a, b, c)` monta `[cb·cc, sa·sb·cc − ca·sc, ca·sb·cc + sa·sc;
cb·sc, sa·sb·sc + ca·cc, ca·sb·sc − sa·cc; −sb, sa·cb, ca·cb]`, ou seja
`Rz(c)·Ry(b)·Rx(a)` para um vetor coluna; se o `operator*` do vetor pela
matriz a aplica como linha, é a inversa: o sentido do giro fica inferido.) Lido ao pé da letra: `vel.x` (a/d) gira em torno de Z e
`vel.z` (q/e) em torno de X; `vel.y` (w/s) não entra; e o alvo fica do outro
lado do centro (`centro + M·(centro − posição)`, não `centro + M·(posição −
centro)`), olhando para longe dele. Com a posição suavizada (7 passos) isso
leva a câmera para dentro do centro escolhido, e cada repetição da tecla
escolhe um centro novo à frente.

**O olhar com o mouse da câmera livre.** O `FillDeviceMap` da CameraMan é o da
base de primeira pessoa (`FUN_00d90018`, vazio): o mouse não passa por mapa
nenhum. Quem olha é o `HandleNotification` da base (`FUN_00d92cd4`): na
notificação de mouse `0x1b637482` (as `OnLButtonDown`/`OnMouseMove`... da
janela, BV `0x5dbxxx`) com tipo `0xd` (mover), com o olhar ligado (`+0x2a6`, 1 no
construtor) e sem o bloqueio `+0x2d5`, se o cursor saiu do meio da janela
(`+0x294/+0x298` menos `+0x29c/+0x2a0`): **rumo −= dx · `mouseheadingsensitivity`**
e **pitch −= dy · `mousepitchsensitivity`** (pixels desde o meio; `+0x28c`/`+0x290`,
padrões do construtor **0,0045** e **0,001** em `0xd92110`; a CameraMan só tem o do
rumo, 0,0005, então o pitch anda 0,001 por pixel). `FUN_00d90164`, a cada
atualização, põe o cursor de volta no meio e o esconde enquanto o app está
ativo (`+0x2d6`, a notificação `0x1b637474` do `OnActivateApp`): **o olhar está
sempre ligado, sem botão**. A "First" usa a mesma base (0,00005 / 0,00001).

**A câmera livre do bairro.** `cCameraNeighborhood::DoMessage` (BV `0x765134`)
pede a "CameraManNH" no Tab (o mapa de teclas do bairro, `0x6A9362F3`, só tem
Tab, não Y) e também troca `,`/`.` como o `cCameraLive`; a `ConstrainTranslation`
(`0x7a6330`) só segura a câmera livre no estado de lote.

**No app (rodada n39).** `LotCamera` (`render/camera.*`): `setKeyMove`/
`setKeyRotate`/`setKeyZoom` (as velocidades), `moveRequest` (o `0x9b66110a`, o
arrasto com o botão direito também, 0 ao soltar), `stepSideways`/`stepForward`
(os passos da borda, com o contador), `rotateRequest`/`mouseRotate` e
`setRotateSims1Mode` (o app não tem a opção na tela: fica desligada, o padrão);
o pivô e o pitch suavizados em 7 passos; `pivotheightabovefloor` 1,2 sem o
parâmetro nas câmeras Live/Build e 0 nas outras. `CameraMouseInput::edgeSteps`
(`app/camera_input.*`): margens 4/16 e 1,25. `FreeCamera` (`render/free_camera.*`):
`setOrbit` e a conta da órbita ao pé da letra, `look` sem o fator 6 e com o
0,001 do pitch; `app_view.mm` esconde e prende o cursor enquanto a câmera livre
está ligada e a janela é a principal, e escolhe o centro da órbita no piso sob
o raio da vista (`floorUnderRay`; inferido: o jogo também pára nos objetos).
`--hood <N> --key tab` mostra a CameraManNH (captura
`docs/screenshots/n39-camera-livre-bairro.jpg`). Testes: `test_camera`
(pedido de mover, passos da borda, giro e troca, zoom segurado, pivô do Build,
órbita, olhar) e `test_camera_input` (margens da borda). Medido sem janela
(`--lot N001_Lot11 --ui live --pivot 20,20`): seta para cima 0,5 s leva o pivô
~20 tiles (com o deslizar depois de soltar, o alvo ficava `v·5` à frente);
`.` 0,5 s gira 77°. **Inferido / não feito:** as setas do mapa de dispositivo
(o acelerador as consome); a taxa de quadros (a borda e o zoom segurado andam
por quadro, como no jogo, e o app desenha a 60 Hz); o sentido da órbita (a
convenção do `operator*` vetor × matriz).


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
| `0x80000004` (meio) e aliases `0x81000004`, `0x82000004`, `0x83000004`, **`0x81000002` (direito + modificador 1)** | `cTSUserToolCameraMiddleClick` (`0x4BB0EF8E`) | **girar**: no clique manda `0x9C17E418` (começo) e mostra o cursor de giro; cada movimento manda `0xBC17E41C` com (dx, dy) desde o clique (dx invertido com a opção `rotateCameraSims1Mode` desligada, o padrão: §1.11) e devolve o cursor ao ponto do clique (arrasto infinito); o `cCameraLive` faz o giro de §1.4 |
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
`floorHit` (`lot_view.mm` `floorUnderRay`: o primeiro piso dos andares
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

**No app** (`app/game_cursors.*`: leitor de `.cur` e escolha; `app_view.mm`
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
yaw 55.0 -> 55.0`, e só `comma` muda o yaw. (Rodada n39: a conta do binário, §1.11, trocou
os 50 px por quadro de 50 ms.) Não reproduzido sem janela; o
pan é rápido (`keyboardtranslationspeed` 50 px por quadro de 50 ms) e com a
perspectiva pode parecer um giro. Teste: `test_camera` "pan right".
