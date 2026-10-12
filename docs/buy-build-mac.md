# Modos compra e construção: o que o binário de Mac faz

As fontes e convenções são as de `docs/shaders-mac.md`. Os endereços são do BV 1.10 ppc
(`/mac/sims2_bv.ppc`, nomes do symxfer conferidos no código). "base" é o
Mac 1.0 (`sims2_base_1.0.ppc`, sem símbolos: só foi possível conferir strings e
constantes). "dados" são os arquivos do jogo instalado (`data/game`, base
2004). "inferido" é dedução nossa e "só BV" marca o que não existe no jogo base.

**Campos do OBJD.** O código lê os campos com `cITSObjectDefinition::GetField(i)`
(vtable `+0x10`). Aqui *i* é o **índice do rótulo** do editor (STR#
`0x7FE59FD0`/0xCC em `objects.package`; o campo *i* fica no offset
`0x40 + 2i`, como em `src/formats/simantics.hpp`). Ex.: `GetField(0x27)` =
rótulo 39 "room sort flags" = offset `0x8E`. Os object data (dados de
instância) são lidos por `cITSObject::GetObjectData(i)` (vtable `+0x34`), com os
rótulos do STR# 0x8D (ex.: 42 "placement flags", 4 "allowed height flags",
41 "current value").

O modo compra já está no app (`src/app/buy_mode.cpp`); a construção (§7)
ainda não.

---

## 1. Catálogo (o que entra no modo compra)

### 1.1 Produtos (`cTSCatalog`, `cTSProductObject`)

- `cTSCatalog::LoadObjects` (BV `0xb312ec`) percorre **todos** os seletores de
  objeto carregados e chama `AddObjectProduct` (BV `0xb30f20`) para cada um.
  Um produto não é criado quando o seletor é de pessoa
  (`GetIsPerson`, vtable `0x98`) ou é uma parte não-líder de multi-tile
  (`GetIsMultiTileSubObject`, `0x94`). A lista é ordenada por
  **preço crescente** (`CatalogSortPred`, BV `0xb2a2d0`, compara
  `Price()`; `std::sort`, que não é estável, então a ordem entre preços iguais não está definida).
- `cTSProductObject::SetSelector` (BV `0xe501f8`) preenche o produto a
  partir do OBJD e das strings:
  - nome e descrição = `GetCatalogName` / `GetCatalogDescription` do
    seletor (abaixo); preço = **campo 18 "price"**;
  - `CatalogIDString` (`+8`) = nome do objeto (`GetObjectName`);
  - `NicenessMultiplier` = **campo 45** (float);
  - dica de construção: `BuildTipStringIndex` (BV `0x51ddf8`) só vale para
    objetos de construção (campo 69 "build mode type" = 1 ou 8 e campo 74
    "build Mode Subsort"), uma string do STR# `0x91`/`200` no grupo
    `0x7F606C7A`. Objetos de compra não têm dica.
- Getters do produto (vtable do `cTSProduct`): `0x5c` Price (`+0x44`), `0x60`
  NicenessMultiplier, `0x68` ShowInCatalog (`+0x50`), `0x78` Deprecated
  (`+0xa8`), `0x7c` IsPlaceholder, `0x80` IsCustom (`+0xaa`).

### 1.2 Strings do catálogo (binário + dados)

`cTSObjectSelector::GetCatalogResource` (BV `0x51e914`) carrega um
`cTSCatalogResource` com o **grupo do objeto** e a **instância = campo 41
"catalog strings id"**. Nos dados ele é o CTSS (`0x43545353`) do mesmo grupo.
Dos 485 OBJDs com room sort ≠ 0, 481 têm esse CTSS (4 não). Entrada 0 = nome,
1 = descrição (por idioma; o jogo usa o idioma atual). Ex.: grupo
`0x7FBBE64F`, `0x41A7`: "Gagmia Simore "RefuseNik" Trash Compactor", §375.

### 1.3 Quem aparece num botão (`cTSWinProcCatalog`)

`LoadObjectsByFunction(função, subsort)` (BV `0xa32bec`), `LoadObjectsByRoom(sala,
função)` (BV `0xa33038`) e `LoadObjectsByCommunity` (BV `0xa32e58`) percorrem os
produtos e aceitam um objeto quando **todas** as condições abaixo valem:

```
lote residencial: campo 39 (room sort) ≠ 0              ← isto é o que torna o objeto "comprável"
lote comunitário: campo 100 (community sort) ≠ 0 (ou room sort ≠ 0 em modo atacado, só BV)
por função: campo 40 (function sort) & função ≠ 0
            e, se subsort ≠ 0: campo 94 (function sub-sort) & subsort ≠ 0
por sala:   campo 39 & sala ≠ 0 e, se função ≠ 0: campo 40 & função ≠ 0
ValidForCatalog (BV 0xa2e818):
   ObjectUnlocked: campo 16 "Item Is Unlockable" = 0, ou o GUID está numa das
                   duas listas de desbloqueados (recompensas)
   e (só BV) campo 105 & (1 << código do EP atual) = 0
AddProduct (BV 0xa30b28): ShowInCatalog, não Deprecated; conteúdo
                   personalizado (IsCustom) só se o painel permitir
```

Bits (rótulos STR# 0xCD/0xCE; conferidos com os botões abaixo):

| bit | room sort (39) | function sort (40) |
|---|---|---|
| 0x001 | cozinha | Conforto |
| 0x002 | quarto | Superfícies |
| 0x004 | banheiro | Utensílios (cozinha) |
| 0x008 | sala de estar ("family room") | Eletrônicos |
| 0x010 | exterior | Hidráulica |
| 0x020 | sala de jantar | Decoração |
| 0x040 | diversos | Diversos |
| 0x080 | estudo | Iluminação |
| 0x100 | crianças | Hobbies |
| 0x200 | — | Recompensas de aspiração |
| 0x400 | — | Recompensas de carreira |

O base tem 485 objetos com room sort ≠ 0 nos dados. As funções 0x400 e 0x800
aparecem em 9 e 10 OBJDs.

### 1.4 Botões do painel (UI script `0x49060004`, `cTSWinProcBuyPanel`)

O layout está no script (`s2pkg ui-dump … 49060004`). O código
(`onBtnClickedID`, BV `0x22c60c`; `SetFunctionSort` `0x228e70`,
`SetFunctionSubSort` `0x2283e4`, `SetRoomSort` `0x229c80`, `SetRoomSubSort`
`0x229818`) mapeia os IDs assim:

- **Abas**: `0x2C584E6C` = por função (mostra `0xA000`), `0x4C578F23` = por
  sala (`0xC000`), `0x2CACE0CD` = coleções (só BV), `0x0D87DCF9` =
  comunitário (`0xF000`). Ferramentas: `0xCC607031` mão, `0xCC607032`
  conta-gotas, `0xCC607030` design, `0xCC607033`/`34` desfazer/refazer,
  `0xCC607035` dia/noite, `0xCC607036` vista de cima.
- **Função** `0xA100…0xA900`: cada botão abre o painel `0xB<k>00` da função
  bit `1 << (k−1)`: B1 Conforto, B2 Superfícies, B3 Utensílios, B4
  Eletrônicos, B5 Hidráulica, B6 Decoração, B7 Diversos, B8 Iluminação, B9
  Hobbies (`SetFunctionSort` mostra `0xB100` para 1, `0xB200` para 2, …).
  O botão A da função é o do caption (A100 Comfort, A200 Surfaces, A300
  Appliances, A400 Electronics, A500 Plumbing, A600 Decorative, A700 Misc,
  A800 Lighting, A900 Hobbies).
- **Subsort** dentro de `0xB<k>00`: `…10` voltar; **`…70` = Tudo (subsort
  0, sem filtro)**; `…60` = Diversos (0x80); os outros pelo binário:

| painel | 20 | 30 | 40 | 50 | 80 | 90 | A0 |
|---|---|---|---|---|---|---|---|
| B1 Conforto | 1 cadeiras de jantar | 2 poltronas | 4 sofás | 8 camas | 0x10 reclináveis | | |
| B2 Superfícies | 1 bancadas | 2 mesas | 4 mesas de canto | 8 escrivaninhas | 0x10 mesinhas | | |
| B3 Utensílios | 1 fogões | 2 geladeiras | 4 pequenos | 8 grandes | | | |
| B4 Eletrônicos | 1 entretenimento | 2 TV/PC | 4 áudio | 8 pequenos | | | |
| B5 Hidráulica | 1 vasos | 2 chuveiros/banheiras | 4 pias | 8 hidromassagem | | | |
| B6 Decoração | 1 pinturas | 2 esculturas | **0x20** cortinas | 8 plantas | 0x10 espelhos | **4** tapetes (oculto) | |
| B7 Diversos | — | — | **8** festa | **2** guarda-roupas | (B760 = **0x10** crianças) | 0x20 | 0x40 |
| B8 Iluminação | 1 mesa | 2 chão | **8** teto | **4** parede | 0x10 externas | | |
| B9 Hobbies | 1 criatividade | — | **2** conhecimento | **4** exercício | 8 recreação (inferido pelos dados) | | |

  Em negrito estão as exceções ao padrão 20→1, 30→2, 40→4, 50→8, 80→0x10. No B7, o
  `…60` é crianças (0x10) e o "Diversos" é `B770` (0x80); o "Tudo" é `B780`.
  O histograma do campo 94 nos dados confere com esses bits.
- **Sala** `0xC100…0xC900`: C100 sala de estar = 8, C200 jantar = 0x20,
  C300 exterior = 0x10, C400 diversos = 0x40 (oculto), C500 estudo = 0x80,
  C600 quarto = 2, C700 cozinha = 1, C800 crianças = 0x100, C900 banheiro
  = 4. Depois de escolher a sala, `0xD100…0xD900` filtram por função
  (D100 = 1 … D900 = 0x100, na ordem dos bits) e `0xDA00` volta. Os
  cabeçalhos `0xE100…0xE900` são as imagens da sala escolhida. Se a
  combinação sala+função não tiver itens (`AnyObjectsByRoom`), o subsort volta a −1.
- Sem item escolhido, o painel volta à ferramenta mão (`SelectTool
  0x492DEF1C`).

### 1.5 Miniaturas e preço na grade (`cTSWinProcCatalogThumbnail`)

- `SetProduct` (BV `0xdf3cac`): o texto do item é o **preço formatado** (com o
  cheat `showCatalogID` vira `"%s (%s)"` = CatalogIDString + preço). Há um
  ícone extra para conteúdo personalizado (`IsCustom`).
- `UpdateAffordability` (BV `0xdf3264`): se o preço cabe nos fundos da
  família, o fundo do botão segue os estados normal/realçado (faixa de 4
  quadros; `SetHilited` usa o quadro 3, normal o 1). Se não cabe, fica no quadro
  0 (**inferido**: o "apagado").
- A imagem vem de `cTSUIThumbnailService::GetProductThumbnail` (BV
  `0x2a9e88`). Para produto de objeto ela chama `GetObjectThumbnail(seletor)`, que
  **renderiza o modelo do objeto** com a fábrica de objetos (chave
  `0xAC2950C1`), com o rig de luz `UIObjectThumbnails` (`Lights/LightRig.txt` →
  recurso `Thumbnails`), e guarda o resultado como imagem `0x856DDBAC` no cache
  `Thumbnails/ObjectThumbnails.package` do usuário. Na próxima vez lê do cache. A
  falha mostra o texto "No Pic". (As miniaturas da fila de ações usam o
  mesmo serviço; ver `docs/ui-live-mac.md`.)
- A descrição (`cTSWinProcCatalog::ShowDescription`, BV `0xa2ef74`) usa os
  UI scripts "catalog description" (`0x49063003`…); as estrelas de motivo
  vêm de `GetCatalogRating` (BV `0x51e348`). Os índices 0–5 são os campos 82–87
  (fome, conforto, higiene, bexiga, energia, diversão), 8 é o campo 88
  (ambiente), 9–0xE e 0xF/0x16 são os bits 1…0x20 e 0x40/0x80 do campo 89 (perícias), e
  0x10–0x15 são os bits 1…0x20 do campo 17 (catalog use flags). Os índices 6/7 (campos
  103/104) são só BV.

**Para nós**: renderizar a miniatura com o nosso renderer (uma câmera fixa
de 3/4 e uma luz) e guardar em cache. Não é preciso ler o pacote do usuário.

---

## 2. Preço, fundos, valor e depreciação

- **Comprar** (`cTSUserToolUndoableObject::TryCreateImpl`, BV `0x35f178`):
  custo = campo 18. Recompensas de aspiração e de carreira custam 0. O
  objeto é criado na hora por `cEdithObjectModule::MakeNewOutOfWorldObject`
  (vtable `0x40` do módulo, BV `0x533fb8`; ver `docs/primitives-mac.md` §4c):
  roda o init (`OnInit`), `JustBorn` e `Reset`, e grava **od 41 = preço**.
  Fica fora do mundo, "segurado" pelo cursor.
- **Cobrar** (`cTSUserToolBuyBuild::SubmitUndoable`, BV `0x9b3b94`): ao
  confirmar uma operação, se a ferramenta não está em "sem custo"
  (`NoCharge`, vtable `200`, cheat/tutorial), **fundos += −TotalCost()**
  (`cTSUserToolUndoable::TotalCost`, BV `0x9b5568`, soma o custo próprio e o das
  pré/pós-operações). `CanAfford(c)` (BV `0x9b38fc`) = c ≤ 0 ou c ≤ fundos
  da família.
- **Vender** (o handler de remoção do undoable, BV `0x35f030`, nomeado errado
  `cTSUserToolFloorLevel::CalculateCost`): um objeto que já existia no lote
  (tipo 3 = movido) passa a custar **−GetCurrentValue()**, ou seja, o reembolso é o
  valor atual (0 para recompensas). Um objeto comprado nesta sessão (tipo 1)
  é destruído na hora com custo 0, e o preço volta porque a operação de compra sai
  da pilha. **Não há fator de revenda para objetos.** `cTSCatalog::RefundRate`
  = **0,8** (BV `0xb2a66c`) é para construção.
- **Valor atual** (`cTSObject::GetCurrentValue`, BV `0xafe9b8`) = object data
  41 "current value", **metade se quebrado** (`IsBroken`). Em
  `GetRawValue` (BV `0xafea2c`), as flags de venda do od 106 são só BV.
- **Depreciação diária** (`cTSObject::DayPassed`, BV `0xb07a4c`, chamado por
  `cEdithObjectModule::DayChanged`, BV `0x525a60`, para todos os objetos):
  ```
  se campo 37 "self depreciating" = 0 (e uma condição global da sim, vtable 0x54, for falsa):
      d = campo 35 "initial depreciation" se ≠ 0, senão campo 36 "daily depreciation"
      valor = valor − d
      se valor < campo 38 "depreciation limit": valor = campo 38
  ```
  Isto foi conferido no assembly (BV `0xb07c6c…0xb07d04`): o binário usa o
  campo 35 todo dia quando ele é ≠ 0. Nos dados, os 377 objetos compráveis com
  os dois campos têm inicial ≈ 15% do preço e diária = 10%, e o limite ≈ 40%. Ao carregar
  (`cTSObject::PostLoad`, BV `0xb0b3dc`), um valor abaixo do limite é
  corrigido para o **preço** (campo 18). Os multi-tile usam o OBJD do líder.
  O valor inicial é gravado na criação: `MakeNewOutOfWorldObject` põe od 41
  = campo 18 (preço). A depreciação diária começa na primeira meia-noite.
- Campo 34 "sale price" vale 35 em todos os objetos dos dados. Ele não é lido
  no caminho de compra/venda acima (aberto).

---

## 3. Colocação

### 3.1 Fluxo da ferramenta (`cTSUserToolObject`)

```
clicar no catálogo → TryCreate (BV 0x20ebf0): cria o undoable e o objeto (tipo 1)
mover o mouse     → OnMove/TryPlace (BV 0x213118) a cada mudança de célula
soltar o botão    → OnRelease (BV 0x208668) → TryDrop (BV 0x2111b4)
                     → SubmitUndoableForPlace (BV 0x20ccbc) → cobra (§2)
                     → se campo 96 "Keep Buying" ≠ 0 (ShouldKeepBuying, BV 0x203c44): cria outro igual
pegar no lote     → TryGrab (BV 0x20fe08): o undoable passa a ser tipo 3 (movido)
Delete/arrastar à lixeira → DeleteCurrentObject (BV 0x2083c8) / TryDelete (BV 0x211e40)
```

**TryPlace(loc)**, em ordem:
1. Ajusta a posição (vtable `0x168`; meio tile), escolhe o nível e alinha à
   calçada/estrada se o objeto pede (`OrientPerpendicularToRoad`,
   `ShouldObjectAlignWithSidewalk`).
2. `ShouldAutoRotate` → `AutoRotateObjectToMatchWalls` (BV `0x209e14`).
3. Prepara pisos (`TryPrepareFloors`), tenta colocar no telhado ou em
   moirão de cerca (ganchos vtable `0x11c`/`0x120`).
4. **Slot**: `FindFirstAvailableSlot` (BV `0x202e6c`) procura, no tile sob o
   cursor, um objeto com slot livre que aceite o objeto segurado. Se o achar e
   `TSObjectCanPlaceInSlot` passar (`FUN_009eba64`), coloca no slot
   (`TryPlaceInSlot`) e lembra o recipiente e o slot, para alternar entre os slots
   no próximo clique.
5. Senão, no chão: roda a árvore **"user placement"** (`RunUserPlacement`,
   BV `0x20dec4` → `OnUserPlacement` = **entry point 11**, se o bit 9 do od
   40 "flag field 2" estiver ligado), testa a posição
   (`FUN_009eb860`, abaixo) e, se passar, chama `TrySetPosition` e roda a árvore de novo.
   Se não passar, o objeto sai do mundo e o cursor mostra o fantasma na superfície
   (`SurfacePositionForObject`) com contorno vermelho.

**Teste de posição** (`FUN_009eb860`, BV `0x9eb860`). Com o cheat
`moveObjects` ligado (vtable `0xa4` do serviço da sim; a string existe no
base), tudo passa. Sem o cheat:
`TSObjectIsInLockedArea` → `TSObjectCanSetPosition` (BV `0x8ae264`) →
`TSObjectCanSetPositionAndOrientation` (BV `0x8ae104`). Os multi-tile vão para
`TSMTObjectCanSetPosition` (BV `0x8add68`), que testa cada parte deslocada e exige
que as partes não fiquem metade dentro e metade fora de uma coluna.
Para os outros objetos, `TSObjectCanSetPositionImpl` (BV `0x8adb34`) faz
`TestPlaceInWorld` (BV `0x8ad754`):

```
TestPlacementCommon(obj, loc, dir)                (sempre)
TestPlacementWithRespectToWalls(obj, loc, dir)    (se o tipo do objeto ≠ 2 pessoa)
TestPlacementWithRespectToObjects(obj, loc, dir)  (footprints e allow-intersection)
od 4 "allowed height flags" & máscara de altura do local → senão erro 0x19 (lista de lugares)
```

Sem lista de erros, o primeiro teste que falha encerra a função. Com lista
(no clique), todos os erros são juntados e mostrados.

### 3.2 `TestPlacementCommon` (BV `0x8aad90`), com p = od 42 "placement flags", p2 = od 82 "Placement Flags 2"

Os bits de p são os rótulos do STR# 0xCA ("Allowed Height Flags", na verdade
as flags de colocação; o mesmo enum da FreeSO `VMPlacementFlags`):
1 chão, 2 terreno, 4 sob a água, 8 preso à orientação do slot, 0x40 tile
travado, 0x80 solto do chão, 0x100 rampa, 0x200 no ar, 0x400 parede, 0x800
piscina, 0x1000 teto, 0x2000 moirão de cerca, 0x4000 superfície d'água,
0x8000 telhado.

| ordem | teste | erro (código → texto do UIText STR# 0x89) |
|---|---|---|
| 1 | local dentro do lote (multi-tile: cada retângulo da pegada) | 2 "Location out of bounds" |
| 2 | nível entre o mínimo e o máximo do lote | 3 "Level out of bounds" |
| 3 | pessoa (tipo 2): precisa de piso ou p&0x200; outros: piso sob a pegada, salvo p&0x200 ou p2&4 | 0x8E (lista de lugares) |
| 4 | parcialmente na água sem p&4 nem p&0x4000 | 0x90 (lista) |
| 5 | piscina sem p&0x800 | 0x8F (lista) |
| 6 | **terreno não plano** sem p&0x100 (multi-tile: todo canto da pegada) | 0x12 "Can't place on slope" |
| 7 | p&1 = 0 e o tile tem piso → não; p&2 = 0 e é terreno → não | 0x8C/0x8D (lista) |
| 8 | od 30 "room placement": 1 = só fora (erro se dentro), 2 = só dentro | 0x0A "Must be placed outside" / 0x0B "…inside" |
| 9 | p&0x2000 exige moirão de cerca | 0x79 |
| 10 | `FUN_008a69e8` (altura livre) e `TSObjectCeilingTooLow` / teto acima ocupado (salvo p2&0x20) | 0x2F "Ceiling too low" / 0x30 |
| 11 | pegada cruza poste de toldo (só BV) | 0x0C |
| 12 | escada ou elevador sobre deck | 0x99 |
| 13 | p2&2 / p2&0x10 / p2&8: precisa de/proíbe zona, lagoa (só BV) | 0x3C/0x3F/0x3E |

As praias (p2&0x40/0x80/0x100, códigos 0x9E–0xA0, 0xA5) são só BV.

**Texto do erro** (`cTSSimSystem::PlacementErrorString`, BV `0x58a6d4`): o
código vira um índice **1-based** da string em `UIText.package` STR# **0x89**
(ex.: código 2 → 1 → "Location out of bounds", 0x12 → 0x11 → "Can't place on slope",
0x19/0x8C…0x91 → 0x5B → "Must place in one of these places:" mais a lista das
superfícies permitidas, montada a partir de od 4/od 42 por `PlacementSurfaceTypeString`).
Para o resto da tabela, ver a função. Um erro do tipo 1 é texto do próprio
objeto (`cTSObject::PlacementErrorString`, BV `0xafac28`: STR# do grupo do objeto,
índice = código, chave `0x98xxxx`). O tipo 2 é a lista de superfícies.
Os 98 textos de erro existem nos dados base (entradas 0–97). As vazias
correspondem a códigos de EP.

### 3.3 Paredes (`TestPlacementWithRespectToWalls`, BV `0x8a8550`)

- **Flags de parede** (`FUN_008a7a60`, BV `0x8a7a60`, com f =
  `GetWallPlacementFlags`, os rótulos do STR# 0xE5): para as 6 direções
  (frente, direita, trás, esquerda, diagonal frente-direita, diagonal
  frente-esquerda) o código olha se há parede no lado do tile:
  - bits "exige parede" `1, 2, 4, 8, 0x10, 0x40`: sem parede ali → erro 7
    "Must be against wall" (ou 0x3B se o lado tem algo que não é parede
    utilizável). Com f&0x1000 (exige cerca), o erro é 0x88 "Must be placed on a fence";
  - bits "não permite" `0x100, 0x200, 0x400, 0x800` (frente…esquerda) e,
    nas diagonais, "permite" `0x20, 0x80` (o teste é invertido): parede ali →
    erro 8 "Can't intersect wall";
  - uma parede que vai do chão ao teto exige a altura livre do teste 10;
  - cercas com altura ≤ 0,35 (BV `0x274b824`) não contam como parede, e
    estufas exigem objeto de estufa (só BV).
- **Atravessar paredes** (`IntersectsWalls`, BV `0x8a7f1c`): salvo p&0x400 ou
  p&0x2000, nenhum retângulo da pegada pode cruzar um segmento de parede do
  nível → erro 8. O mesmo teste vale para arcos de cerca.
- Portas e janelas (tipo 8/9) usam o próprio teste de portal (`IsWallPortal`).

### 3.4 Objetos (`TestPlacementWithRespectToObjects`, BV `0x8ace04`)

Para cada objeto que a pegada do objeto segurado toca
(`TSObjectWorldSpaceFootprint`, BV `0x8aa40c`, que gira a pegada do OBJD pela
direção), `TestPlacementWithRespectToObject` (BV `0x8ac158`) testa:
- se as pegadas se cruzam (a do objeto parado e a do segurado);
- as flags do od 8 (STR# 0x8E: 0x2 "allow object intersection", 0x4
  "…object and person", 0x10 "…person") e o od 40 bit 0x4000 ("Low Small
  Object");
- as "exclusive placement flags" (od 63, STR# 0xFB: chão, parede, "nenhum
  outro do mesmo tipo", teto). Assim um quadro de parede e uma mesa podem dividir o tile;
- se tudo passou, a árvore **"allow intersection" (entry point 5)** dos dois
  objetos tem a última palavra (parâmetro 0 = o outro objeto).

A falha dá o erro 0x0C "Can't intersect other objects".

### 3.5 Rotação

- A direção é um valor de **0 a 7 (passos de 45°)**. `TryTurn(n)` (BV
  `0x204158`) soma n à direção, faz o resultado dar a volta em 0..7 e chama
  `TryOrient`, que testa `TSObjectCanOrient` (BV `0x8ae2fc`, o mesmo teste
  completo com a nova direção). Objetos que não aceitam diagonal só usam
  valores pares (`SwapDiagonalObject` troca o objeto pelo "diagonal
  selector" do OBJD, campos 21–24, que é só BV).
- O auto-giro só acontece com `ShouldAutoRotate`, encostando o objeto na parede.

### 3.6 Níveis

A posição tem nível (`cLocation +8`). O teste 2 limita o nível ao lote, e
objetos no andar de cima precisam de piso (teste 3). Tetos e paredes vêm do
nível. "Must be on first level" é o código 0x22 (`TSObjectCanDropImpl`).

**No app** (rodada 8): as teclas são **Page Up / Page Down** ("Subir um Andar
(Page Up)", "Descer um Andar (Page Down)": STR# `0x8A` do `UIText`, e as
dicas dos botões `0x2001`/`0x2000` do puck `0x4905F85B`, `btnclicksnd`
`ui_*` do script). No modo compra mudam o andar onde os objetos entram
(`BuyMode::changeLevel`, entre o primeiro e o último nível do lote) e a
câmera segue; no modo vivo só a câmera troca de andar, até o Sim selecionado
mudar de andar. Os dois botões do puck fazem o mesmo.

---

## 4. O que roda na VM

| momento | código | entry point (STR# 0xF5) |
|---|---|---|
| comprar (criar) | `MakeNewOutOfWorldObject` (BV `0x533fb8`) | 0 init (`OnInit`), depois `JustBorn` e `Reset`; od 41 = preço |
| clicar para colocar | `OnUserPlacement` (BV `0xafed44`) | **11 "user placement"** |
| mudar de posição | `SetPosition` → árvore de colocação | 9 "placement" |
| tirar do mundo | `RemoveFromWorld` | 10 "pickup" |
| pegar com a mão | `OnUserPickup` (BV `0xaf4bcc`) | **12 "user pickup"** |
| vender/apagar | `OnUserDelete` (BV `0xaf4c44`) | **50 "user delete"** |
| destruir | `KillObject` (BV `0x35cb04`) → gerente de objetos (vtable `0x44`) | 49 "delete", 3 "cleanup" |
| testar slot | recipiente | 52 "prevent place in slot" |
| todo dia | `DayPassed` | — (depreciação, §2) |

Multi-tile: `OnUserPickup`/`OnUserDelete` só rodam no objeto se ele não for
multi-tile (o grupo trata). Entry points são os índices do STR# 0xF5 que o
motor passa a `RunEntryPoint` (vtable `0x3b4`); a tabela é a mesma da TTAB/OBJf
(init 0, main 1, …).

---

## 5. Desfazer (`cTSUserToolUndoable*`, `cTSUserToolUndoManager`)

- Cada operação do jogador é um **undoable** com um custo e uma lista de
  pré e pós-operações (piso aplainado, terreno, paredes de sauna…). O
  undoable de objeto guarda o objeto (`+0x6c`), o tipo de mudança (`+100`: **1
  criado, 2 apagado, 3 movido**) e a posição original (`RecordPlacement`).
- **Desfazer** (`UndoSelf` → `ToggleChange`, BV `0x35eb6c`): o criado sai do
  mundo e vira "apagado". O apagado volta à posição/slot original e vira
  "criado". O movido troca a posição atual pela gravada.
- Um objeto **apagado não é destruído na hora**: ele sai do mundo
  (`RemoveImpl` = `RemoveFromWorld` + `OnUserDelete`). Ele só morre quando o
  histórico é zerado (`ResetSelf`, BV `0x35f4dc`, tipo 2 → `KillObject`), ao sair
  do modo compra. Por isso é possível desfazer uma venda.
- Comprar e apagar um objeto criado na mesma sessão o destrói na hora
  (custo 0).
- Recompensas: confirmar marca a recompensa de carreira como usada ou cobra
  os pontos de aspiração, e desfazer devolve (`nTSRewards::*`).
- Atalhos: Ctrl-Z / Ctrl-Y (tooltips do script). `UndoLastCommand`, BV
  `0x30251c`.

---

## 6. Sons

`OnRelease` toca um som de UI ao soltar um objeto
(`RZ::WCM()->vtable 0x28`, ID em `PTR_DAT_02788d3c/44/50`): um para porta/janela
(tipo 8/9), um para plantas (`IsObjectOfFoliageType`) e um para o resto. Girar
(`TryTurn`) toca outro e "sem dinheiro" (`TryDrop`) outro. Os IDs são
ponteiros para globais. **Aberto**: a que sons eles apontam. No app (inferido pelos nomes `ui_*`
do executável, todos existem no `Sfx`): porta/janela `ui_build_door_plop`,
planta (função decoração com subsort plantas) `ui_build_garden_plop`, o resto
`ui_object_plop`; girar `ui_object_rotate`; sem dinheiro ou posição
recusada `ui_error`; pegar com a mão `ui_object_pickup`; vender
`ui_object_sell`; desfazer/refazer `ui_undo`/`ui_redo`; expandir/recolher o
catálogo `ui_panel_expand`/`ui_panel_collapse`.

---

## 7. Construção

Base comum: cada ferramenta é um `cTSUserToolBuyBuild` com um ou mais
undoables (§5), e o custo é cobrado no `SubmitUndoable` (§2: fundos −=
`TotalCost`, que soma o custo próprio e o das pré/pós-operações). Os nomes do
`baserom.sym` (o outro build ppc com símbolos, `references/sims2_mac_decomp`)
cobrem muitas funções que o symxfer não nomeou ou nomeou errado no BV; aqui
cada endereço BV vem com o nome do `baserom.sym` quando ele foi casado pelo
tamanho e pela ordem das funções no arquivo (ex.: o BV chama de
`Sims1AbsoluteDirectionToSims2Angle` o que é `PartitionStyleIsFenceArch`, e
de `cTSUserToolAutoRoof::RemoveAllRoofs` o que é `UserMayPlacePartitionCommon`).

Para saber se algo existe no base, `tools/re/constscan.py` conta onde o
código monta uma constante de 32 bits (ID de mensagem, CLSID) num binário ppc:
0 no base 1.0 e ≥ 1 no BV = só BV.

### 7.1 Catálogos de construção (dados)

Os produtos de construção não vêm de OBJDs, e sim de scripts de texto em
`TSData/Res/Catalog/Scripts/` (`walls.txt`, `fences.txt`, `fencearches.txt`,
`floorPatterns.txt`, `wallPatterns.txt`, `deckfoundations.txt`,
`modularstairs.txt`, `roofs.txt`, `roofPatterns.txt`, `terrainPaints.txt`,
`terrainSculptBrushes.txt`, `userToolProxies.txt`, `fireplaces.txt`). Cada
arquivo explica a própria sintaxe nos comentários. Eles são lidos por
`cTSCatalog::LoadWalls/LoadFences/...` (BV `0xb2fb9c`, `0xb325c0`...) com um
`cTSProduct*Parser`, mais os produtos empacotados em
`Catalog/Patterns/catpatterns.bundle.package`: XMLs `cGZPropertySetString` dos
tipos `0x2CB230B8` (cerca, 32 no base), `0xCCA8E925` (padrões de parede, 330,
e de piso, 146), `0xACA8EA06` (padrões de telhado, 8) e `0x4DCADB7E`
(pinturas de terreno, 14). Chaves dos XMLs: `cost`, `name`, `description`,
`showincatalog`, `deprecated`, `subsort`, `surfacetype`, `routingcost`,
`material`/`filename` (piso/parede), `rail`/`diagrail`/`post`/`height`/
`maywalkover` (cerca), `guid`.

**Preço** = chave `cost` (empacotados) ou, nos scripts de texto, a entrada
`3·i` do STR# indicado por `catalogTextResource build N` em
`TSData/Res/Text/Build.package` (grupo `0xFFFFFFFF`, instância N): para o
índice de catálogo *i*, a string `3i` é o preço ("70 -- Price - DO NOT
TRANSLATE"), `3i+1` o nome e `3i+2` a descrição. Preços do base:

| produto | STR# | preço |
|---|---|---|
| ferramenta de parede / de cômodo (`walls.txt`, índices 0 e 1) | 0x81 | **70** por trecho de parede |
| telhados (tipos) | 0x8C | 0; águas-furtadas 560/840/1120 (estreita/média/larga) |
| padrões de telhado | 0x8D | 0 |
| fundação / fundação diagonal | 0x8A | 4 por tile |
| deck com treliça / diagonal | 0x8A | 3 por tile |
| deck com colunas / diagonal | 0x8A | 2 por tile |
| piscina | 0x8A | 75 por tile |
| escadas | 0x8B | 40–85 (Wooden 40, Nouveau 70, Country 50, Chic 60, Steadfast 45, Holy Smoke 80, WroughtWright 85, Arboreal 40); corrimão 3 |
| terreno: subir/descer/nivelar/suavizar | 0x80 | 1 |
| água (lago) | 0x85 | 1 |
| lareiras | 0x90 | 600–3000 |
| pisos (`cost` do XML) | — | 2–21 por tile |
| revestimentos de parede (`cost`) | — | 2–15 por face de trecho |
| cercas (`cost`) | — | 5–250 por trecho |
| pinturas de terreno (`cost`) | — | 0 |

Os IDs de estilo de partição (`wall N`, `fence N`, `fenceArch N`) dividem o
mesmo espaço (< 255). No base: 1 parede normal (a única no catálogo), 3
parede de sótão (`floorToRoof`, apagada pela ferramenta de telhado), 23
parede de fundação, 16/24/4 saias de deck, 29 parede de piscina; 2 a cerca de
estacas escondida; o resto das cercas vem do pacote. As propriedades
(`mayChangeSurface`, `mayAttachObjects`, `mayCutAway`, `requiresFlatBottom`,
`requiresFlatTop`, `submersible`, `mayPlaceDiagonally`, `verticalSpan`,
`blocksPlacement`, `bearsLoad`, `blocksLight`, `deleteTool`,
`requiresSupportUnderneath`) viram os `nTSWorld::PartitionStyle*` (BV
`0x8b566c`…`0x8b5c48`) que as regras abaixo consultam.

**Meia-parede, estufa, parede de palco, espaçamento de postes** (chaves
`ishalfwall`, `isgreenhouse`, `stageWall`, `posttilespacing`) são **só BV**:
as strings não existem no binário base e nenhum XML do base usa essas chaves.
No base, toda cerca tem poste em cada vértice.

### 7.2 Ferramenta de parede (`cTSUserToolWall`, BV `0x1e0bbc`…)

**Modos** (`+0x70`, o atual; `+0x74`, o padrão da ferramenta):

| modo | como se chega | `UserAddPartitions` tipo |
|---|---|---|
| 0 parede em linha | produto "Wall Tool" (variante 0) | 0 |
| 1 demolir | Ctrl (estado 2) ou Ctrl+Shift (estado 4) | — (`UserRemovePartitions`) |
| 2 cômodo retangular | produto "Room Tool" (variante 1), ou Shift (estado 3) com a ferramenta de parede | 1 |
| 3 cômodo diagonal | variante 2 (só BV) | 2 |
| 4 cômodo de tamanho fixo girável | variante 3 (só BV; teclas de tamanho e giro) | 3 |

`StateChange` (`0x1e0e40`) recebe o estado dos modificadores (1 nenhum, 2
Ctrl, 3 Shift, 4 Ctrl+Shift, 5/6 só BV) e põe o modo; o estado 1 volta ao
padrão. As teclas `0x714798EE`, `0x314798AC`, `0x51462721`, `0x9146274C` do
`KeyCommand` (`0x1e0bbc`) não existem no base (constscan = 0) e não têm atalho
nos arquivos de teclado do base. Cada `LoadWalls` cria 4 produtos por parede
(variante 0–3, vtable `0x150` do produto = o modo), mas o texto do base só tem
as variantes 0 e 1 (nome/preço/miniatura).

**Arrastar** (`OnClick` `0x1e25b4` → `OnDrag` `0x1e1c88` → `OnRelease`
`0x1e1fd0`): o clique guarda o ponto inicial (`+0x7c`, vértice) e o nível; a
cada mudança de vértice, `ChangeWallsAndPlaceCursorAtEnd` (`0x1e1828`) desfaz
a prévia (vtable `0x20` dos três undoables) e refaz tudo do início até o
cursor com `UserAddPartitions` (BV `0x9aa90c`) ou `UserRemovePartitions`
(`0x9ac6b8`), mostra o custo (`ShowCostPreview` = custo da parede + da
terraplanagem) e marca "sem dinheiro" se `CanAfford` falhar (a soltura então
mostra o erro e descarta). Soltar: se houve ≥ 1 mudança, o undoable de
terraplanagem vira **pré-operação** e o de toldos (só BV) pós-operação, e a
operação é submetida (cobrança); toca o efeito `build_mode_tool_poof`.

Os três undoables da ferramenta: `+0x60` parede (`cTSUserToolUndoableWall`,
CLSID `0xEC167825`), `+0x64` elevação do piso/terreno
(`cTSUserToolUndoableFloorElevation`, `0x6C167887`), `+0x68` toldos
(`0x303AB8DF`, só BV).

**Geometria do traço** (`UserAddPartitions`, `0x9aa90c`):

```
tipo 0 (linha):
   dir = round((atan2(dy, dx) + π) / (π/4)) mod 8           # 8 direções, inclui diagonais
   passo[dir] = (−1,0) (−1,−1) (0,−1) (1,−1) (1,0) (1,1) (0,1) (−1,1)   # tabela BV 0x2738240
   n = max(|dx|, |dy|) (arredondados)
   PlaceWallLine(estilo, dir, n, início)
tipo 1 (retângulo): se dx = 0 ou dy = 0 vira uma linha; senão tenta os dois
   "L" (pelo canto (x1,y0) e pelo canto (x0,y1)) e só aceita se os dois passam;
   depois fecha o cômodo (FUN_009a7dd8)
tipo 2 (diagonal, só BV): se |dx| = |dy| vira o tipo 0; senão um losango
tipo 3 (só BV): octógono de lado ≥ 3 (AddOctagonToWallList)
```

O cursor só pousa em vértices (cantos de tile). Um traço diagonal anda em
(±1, ±1) por trecho: o trecho é a diagonal de um tile.

**`PlaceWallLine`** (`0x9a9fec`): para cada um dos *n* trechos monta o
`cWallLocator` (dois vértices + nível) e chama **`TryPlacePartition`**
(`0x9a9774`): primeiro `TryAdjustFloorElevationForWall` (`0x9a7f48`: nivela o
terreno sob o trecho, nos dois tiles de cada lado, pelo undoable de elevação),
depois **`UserMayPlacePartition`** (`0x9a95cc`) e, se passou,
`cTSUserToolUndoableWall::TryCreateAndInsertWall` (BV `0x30d75c`, nomeado
errado `RemoveFromLotObjectsThatShouldBeRemoved`). Uma falha que não seja o
código `0x41` interrompe a linha. Para cercas, depois da linha, um poste é
criado em cada vértice do traço que ainda não tem (`+ início e fim`; no BV a
cada `PostTileSpacing`).

**`UserMayPlacePartition`** (`0x9a95cc`), em ordem:

1. `UserMayPlacePartitionCommon` (`0x9a7384`), com o código de erro (texto em
   `cTSSimSystem::PlacementErrorString`, BV `0x58a6d4`, que mapeia o código
   para o índice 1-based do STR# 0x89 do `UIText`; ver §3.2):
   - o tile de um lado fora do mundo (parede) ou os dois (cerca) → `0x40`
     (texto padrão "Location out of bounds");
   - cruza outra partição no mesmo tile (BV `0x9a6b20`, nomeado errado
     `TSObjectUserCanPickup`; inferido: `PartitionBlockedByPartition`/
     `WallCrossesAnotherInSameTile`);
   - trecho diagonal e o estilo não aceita diagonal (`mayPlaceDiagonally`)
     → `0x4C` "Cannot build diagonally.";
   - sem o cheat de construir em qualquer lugar (sim system vtable `0xd4`):
     um lado (parede) ou os dois (cerca) fora da área editável do lote ou
     travados (`WorldDB` vtable `0xc4`) → `0x45`;
   - `requiresFlatBottom` e as pontas em elevações diferentes → `0x49`
     ("Can't place on slope");
   - `requiresSupportUnderneath` e sem apoio embaixo → `0x48` ("Needs
     something to support it");
   - os dois lados reprovados por `FUN_001648ac` (um teste de tile também
     usado pelos pisos) → `0x46` (texto vazio no base);
   - os dois lados na água e o estilo não é `submersible` → `0x47`.
2. Arco de cerca → `UserMayPlaceFenceArch` (`0x9a92fc`). Estilo 300 (parede
   invisível, BV) → sempre passa.
3. `MayPlaceWallWithRespectToObjects` dos dois lados (`0x9a86a0` +
   `0x9a8bd0`): para cada objeto no tile do lado (salvo os de od 0x22 ≠ 0):
   um **Sim** (tipo 2) → `0x4B` "Wall too close to Sim"; objeto que proíbe
   parede naquele lado (`NumWallOrFenceProhibitedFlags`, vtable `0x270`) ou
   cuja pegada é cruzada pelo trecho → `0x0C` "Can't intersect other objects".
4. Parede ou meia-parede: `UserMayPlaceWall` (`0x9a71f8`): os tiles dos dois
   lados precisam passar no teste de lote do `WorldDB` (vtable `0x108`), senão
   `0x45`. Cerca: `UserMayPlaceFence` (`0x9a6dd4`): inclinação
   |Δz| / comprimento horizontal > √3 (60°) → `0x31` "Terrain too steep";
   cerca em nível < 0 → `0x1C`; no térreo, um teste das salas dos dois lados
   (`0x0C`; não lido em detalhe); estufa (BV) → `0x9D`.

**Custo de colocar** (`TryCreateAndInsertWall`): preço do produto do estilo
(`cTSCatalog` vtable `0x4c` → `Price`, vtable `0x5c`) **por trecho**; o
undoable grava a mudança (`cWallChange`, vetor `+0x68`) e soma o preço ao
seu custo. Parede = 70; cerca = `cost` do XML.

**Apagar** (`UserRemovePartitions` `0x9ac6b8` → `EraseWallLine` `0x9ac0e8`
→ `TryErasePartition` `0x9abec8`): mesma linha de 8 direções. Para cada
trecho:
- `UserMayEraseWallOrFence` (`0x9abd20`): não há parede/arco ali → `0x42`
  (ignorado em silêncio: a linha continua); objeto **em uso** encostado na
  parede, em qualquer lado (od `0x3E` > 0 ou `IsOccupied`, também nas partes
  de um multi-tile) → `0x1B` "Wall next to object in use"; Sim no tile →
  `0x4B`;
- `TryRemoveWallOrFence` (BV `0x30d41c`, nomeado errado
  `cTSUserToolTerrainSculpt::SmoothenBrushFn`): se o produto do estilo não é
  "deleteTool levelRoom", **vende os objetos presos à parede** dos dois lados
  (`EraseWallAttachedObjects`, BV `0x30c514`: objeto que se encosta naquele
  trecho, vtable `0x26c`; cada um vira um undoable de objeto do tipo
  apagado, como em §2/§5, com o reembolso do valor atual) e, para cerca,
  apaga os postes das duas pontas que ficaram com menos de 2 cercas; tira a
  partição e grava a mudança com
  **reembolso = int(preço × `RefundRate` 0,8)** (`FullRefundMode` dá o preço
  inteiro, mas só as paredes de sauna do BV o ligam). Parede: 56 por trecho.
- Paredes de palco e invisíveis (estilos 301/300, BV) têm caminhos próprios.

Rodada 9 (lido): `UserMayEraseWallOrFence` testa os dois lados com
`FUN_009ab944` (BV): para cada objeto do tile do lado (salvo od `0x22` ≠ 0),
um Sim → `0x4B`; um objeto **preso àquele trecho** (vtable `0x26c` com o
locator; para multi-tile, qualquer parte presa conta pelo grupo) e em uso
(`FUN_009ab5f8`) → `0x1B`. Ou seja, o `0x1B` só vale para objetos presos à
parede, não para qualquer vizinho. `EraseWallAttachedObjects` (BV `0x30c514`,
nomeado errado `cTSUserToolMoveLot::DoMouseMove`) percorre os objetos do tile
do lado (`WorldDB` vtable `0xe4`/`ObjectOnTileAtIndex`) e apaga
(`cTSUserToolUndoableWall::EraseObject`) os que se prendem ao locator
(vtable `0x26c`), salvo os de categoria od `0x3B` = `0x7D` com vtable `0x348`
≠ 0. Aqui (inferido, o corpo do `0x26c` não foi lido): preso = porta/janela
com o trecho na frente ou atrás, ou objeto cujas flags de parede (od 13)
exigem parede do lado do trecho; em uso = od `0x3E` > 0 ou um Sim num slot
dele.

**Terraplanagem sob a parede** (`TryAdjustFloorElevationForWall`,
`0x9a7f48` → `FUN_009a79d0` por vértice): com `requiresFlatBottom`, cada
vértice do trecho (os dois da parede reta; a caixa do tile para a diagonal)
vai para uma altura dada (`FUN_009a5a68`); com `requiresFlatTop`, o vértice do
andar de cima também. Aqui a altura é a do início do traço e um vértice preso
por piso ou por outra parede não se move (inferido; o leveler não foi lido).

O preço cheio só volta pelo **desfazer** (a mudança gravada sai com o custo).

**Desfazer** (`ToggleChange`, BV `0x30c134`): percorre as mudanças de trás
para frente e troca o estado (`+100`: 1 = aplicado, 2 = desfeito); paredes
voltam com os padrões das duas faces gravados na mudança.

### 7.3 Salas (`cRoomManager`) e o que é refeito

**Grafos e definições de sala (rodada 9, conferido no código).** O
`cPartitionManager` guarda **12 grafos de parede** (`cWallGraph`, um grafo
planar quad-edge: arestas com `Orig/Dest/LNext/Sym`, faces = salas) em
`+0x4c..+0x78`. `AddWallToGraph` (BV `0xc2e7cc`) põe cada partição nos grafos
conforme as propriedades do **estilo** (o `cTSProductPartition` lido de
`walls.txt`/`fences.txt`/`fencearches.txt`: `blocksPlacement`, `bearsLoad`,
`blocksLight`, `verticalSpan`, `notRendered`); `cPartitionManager::Save` dá
a instância do WGRA (`0x0A284D0B`) de cada um:

| grafo (`WallTopology(tipo)`) | entra | WGRA |
|---|---|---|
| `+0x4c` (0) | renderizada, `verticalSpan floorToFloor` | `0x16` |
| `+0x50` (1) | renderizada, `floorToFloorBasedCeiling` (decks) | `0x08` |
| `+0x54` (2) | renderizada, `floorToRoof` (sótão, estilo 3) | `0x17` |
| `+0x58` (6) | arcos de cerca (`PartitionType` 2) | `0x13` |
| **`+0x5c` (3)** | **`blocksLight`** | **`0x07`** |
| **`+0x60` (4)** | **`bearsLoad`** | **`0x06`** |
| **`+0x64` (5)** | **`blocksPlacement`** | **`0x05`** |
| `+0x68` (7) | tudo menos arcos e estilos 300/301 | `0x18` |
| `+0x6c`/`+0x78` (8/11) | meias-paredes (só BV) | `0x19`/`0x1f` |
| `+0x70` (9) | não renderizada de tipo 4 (parede invisível de luz) | `0x1a` |
| `+0x74` (10) | parede de palco (estilo `0x12d`, só BV) | `0x1c` |

Só três grafos têm gerenciador de sala (`InitWallGraphs`, BV `0xc2f644`):
`+0x5c` ← `tRoomDefinition` **0**, `+0x60` ← **1**, `+0x64` ← **2**. E
`cRoomManager::Save` grava cada um num 3ARY (`0x2A51171B`): definição 0 →
**0x0B**, 1 → **0x0A**, 2 → **0x09**. Ou seja:

- **3ARY 0x09 (definição 2) = salas de colocação**: as paredes que
  `blocksPlacement`. Mensagem `0x2B4D18F6`, escutada pelo
  `cEdithObjectModule` (sala dos objetos na VM). É o mapa da VM.
- **3ARY 0x0A (definição 1) = salas de carga** (`bearsLoad`).
- **3ARY 0x0B (definição 0) = salas de luz** (`blocksLight`). Mensagem
  `0x2B4D18FB`: paredes, pisos, cercas e light maps (`docs/lighting-mac.md`:
  "fora" = sala 0 **deste** mapa).

Pelos scripts de catálogo: parede comum: as três (padrões `true`); sótão
(3): sem `bearsLoad`; fundação (23): as três; deck (16) e piscina (29): sem
`blocksLight`; saia de deck (4): sem `blocksLight` nem `blocksPlacement`;
**cercas: só `blocksPlacement`** (padrões do `fences.txt`: `bearsLoad` e
`blocksLight` falsos); arcos de cerca: nenhuma. Portanto **as cercas dividem
as salas do 3ARY 0x09 e não as de 0x0A/0x0B**. Conferido no `N001_Lot11`:
em 0x09 o quintal dentro da cerca é a sala 17 (e as varandas cercadas, 20 e
22), a faixa entre a cerca e a borda do lote é a sala 0; em 0x0A e 0x0B o
quintal inteiro é 0 e só a casa tem salas. É assim que "as salas de pátio
viram a sala 0": não viram; elas só existem no mapa de colocação, e luz e
"fora" usam o 0x0B. (Corrige o que estava aqui: "cercas não mudam salas"
vale para 0x0A/0x0B; o aviso 1/2 do `UpdateRooms` é para o render.)

**Cada sala é um ID `short` por quarto de tile** (4 triângulos por tile; o
`cFloorTile` = x, y, nível + máscara de quartos 1, 2, 4, 8; uma parede
diagonal separa as metades 3/12 ou 6/9). `RoomIDInFloorTile` (BV
`0xc283fc`) devolve o ID do primeiro quarto da máscara (0xFFFF fora);
`RoomContaining` (vtable `0x38`, `0xc284b8`); "dentro" (`InsideRoomIDSet`,
vtable `0x44`, `0xc28ce0`) = todos os IDs ≠ 0 e ≠ −1 do mapa; a sala 0 é a
de fora. Slots do `cIRoomManager` (vtable `0x281ba30`): `0x24` `AddSeed`
(`0xc28ffc`), `0x28` `RefreshMap` (`0xc2a36c`), `0x2c`
`FlushRoomChangedNotifications`, `0x30` `SetRoomIDInFloorTile`
(`0xc29618`, também mantém a lista de quartos por sala), `0x3c`
`FloorTilesInRoom`, `0x40` `FloorTileInRoom`, `0x48`
`InsideRoomIDSetForLevel`, `0x4c` `RandomFloorTileInRoom`.

**Quem decide os IDs é o grafo, não o flood fill.**
`cWallGraph::SetRoomsPostAttach` (BV `0xc1f1bc`, depois de cada aresta
nova):

1. Anda o ciclo da face à esquerda da aresta nova (`LNext`). Se volta pela
   própria aresta oposta (`Sym`), a aresta **não fecha** nenhum ciclo: as duas
   faces ficam com a sala que contém o meio da aresta (`RoomContaining`) e
   nada muda no mapa.
2. Se fecha, a face se divide: `CreateRoom` (`0xc1f070`) cria uma sala com o
   **menor ID livre** (a lista de salas é ordenada; o primeiro buraco, não o
   maior + 1). A sala nova fica com o ciclo à esquerda da aresta, **salvo**
   quando a sala velha é a 0 (fora) e o ciclo da esquerda não cerca a
   aresta (`FUN_00c1c480`: teste de paridade de raio contra as arestas do
   ciclo): aí a nova vai para o ciclo da direita. Ou seja: cercar um pedaço
   de fora sempre cria a sala do lado de dentro, e o resto continua 0;
   dividir uma sala ≠ 0 dá o ID novo ao lado esquerdo da primeira aresta
   (não ao menor pedaço).
3. Para cada aresta do ciclo da sala nova: a face vira a sala nova e
   `AddSeedToRoomManager` (`0xc1a454`) chama `AddSeed(locator do lado,
   ID)`; depois `RefreshMap` e `AttachWallsToRoom` (`FUN_00c1b254`: toda
   seção cujo lado virado para a sala tem esse ID ganha a face).

`cWallGraph::SetRoomsPreDetach` (`0xc1de18`, antes de tirar uma aresta) é o
inverso: se as duas faces têm salas diferentes, uma morre: sobrevive a da
direita, **mas a sala 0 sempre sobrevive** (tirar uma parede entre uma sala
e o lado de fora devolve a sala inteira à 0). Cada aresta do ciclo da que
morre recebe a sobrevivente e semeia o ID dela; `RefreshMap`; os quartos que
ainda têm o ID morto (bolsões que a inundação não alcançou) recebem a sala
do lado da parede mais próxima (`NearestWall` + sala virada para o tile); a
sala sai da lista.

**`AddSeed`/`RefreshMap`.** `AddSeed(locator, ID)` guarda num mapa
`cFloorTile → {ID, já coberto}` (`+0x1c`) o quarto (parede reta) ou a
metade (diagonal) do lado do locator; se um quarto vizinho já tem semente,
a nova não entra (ou a metade substitui os quartos dela) e as vizinhas
ficam marcadas "já coberto". `RefreshMap`: (1) lê o ID velho no tile da
primeira semente; (2) pinta **todas** as sementes com o ID delas
(`SetRoomIDInFloorTile`); (3) para cada semente não marcada,
`FloodRoomFromSeed(velho, novo, tile)` (`FUN_00c289e8`) parte dos **dois
quartos vizinhos dentro do tile** da semente (não do outro lado da aresta,
onde está a parede) e `FloodRoomRecurse` (`FUN_00c2887c`) segue: um quarto
com o ID velho vira o novo e espalha para os 3 adjacentes (`FUN_0015ddc0`:
os 2 do mesmo tile e o do outro lado da aresta do tile) que estejam no lote
(a única condição é `cWorldDB::IsOnLot(x, y)`, conferido no assembly).
**A inundação não testa paredes**: como só o ciclo da sala nova é semeado e
toda aresta dele tem o quarto de dentro já pintado, a inundação nunca chega
a um quarto de onde atravessaria uma parede. (4) limpa as sementes e marca a sala para o
`FlushRoomChangedNotifications` (`0xc28638`), que manda `0x2B4D18FB`
(definição 0), `0xAC2A4F4E` (1) ou `0x2B4D18F6` (2) por sala mudada.

`cTSUserToolUndoableWall::UpdateRooms` (BV `0x30bec8`, chamado no
`CommitSelf`) pede o recálculo ao gerenciador de partições (vtable `0x98`)
e avisa o mundo (vtable `0x54` do undoable) com **1** se alguma mudança foi
parede ou meia-parede e com **2** se só houve cercas; parede de piscina
também atualiza o scenegraph.

Quem escuta a mensagem de sala 0 (`FindConst`): `nTSSG::cWallManager`
(render groups por sala), `cFenceManager`, `cFloorManager`, `cObjectModule`,
o gerenciador de light maps (`FUN_006f3d30`, perto de
`cLightMapManager`) e outros dois do scenegraph. A de definição 2 vai para o
`cEdithObjectModule` (sala dos objetos para a VM). O roteamento escuta as
mensagens de mudança do mundo no `cTSRouteManager::DoMessage` (BV
`0x4f8038`; IDs `0x0B4CF313`, `0x2B4CF319`, `0x2B4CF327`, `0x2B4CF334`,
`0x0B537176`, `0x2B4D1900`…) e refaz os retângulos de parede
(`AddWallRectangle`/`RemoveWallRectangles`, `docs/routing-mac.md` §2).

Para implementar: depois de cada operação de parede, refazer os **três**
mapas, cada um só com as partições do seu grafo (WGRA 5/6/7 ↔ 3ARY
0x09/0x0A/0x0B): cercas dividem só o 0x09. Fechar um ciclo cria o menor ID
livre do lado de dentro (ou à esquerda); abrir junta na da direita ou na 0.
Um flood fill por componentes com as linhas do grafo como barreiras dá as
mesmas regiões; só a escolha de IDs difere (ver acima). Depois as paredes
do roteamento e os light maps das salas afetadas.

### 7.4 Pisos (`cTSUserToolFloorPattern`, BV vtable `0x27b1648`)

- **Modos** (`+0x84`/`+0x88`, por `StateChange` `0x2ffe3c`): 0 pintar
  retângulo, 1 apagar retângulo (Ctrl), 2 encher a sala (Shift), 3 apagar a
  sala (Ctrl+Shift). Apagar usa o "sem piso" do `WorldDB` (vtable `0x98`).
- **Atualização** (BV `0x300b2c`): com início (`+0x64`) e cursor (`+0x74`)
  diferentes, cada tile do retângulo recebe `UserSetFloorPattern` (BV
  `0x9a4e80`) com a máscara 0xF (tile inteiro); num clique sem arrastar, o
  tile do cursor com a máscara de quartos que veio do ponto do mouse
  (`+0x80`). Encher a sala = `UserSetFloorPatternRoom` (BV `0x9a4f90`).
- **Máscara do clique** (`FUN_003000cc`, lido na rodada 9): se o tile tem
  parede diagonal (`WorldDB` vtable `0xbc`/`0xc0`), a metade do lado do
  mouse; senão o tile inteiro (0xF), salvo com o modo de quarto de tile (só
  BV); no caminho de cima, os quartos da mesma sala do quarto clicado.
- **Girar** o padrão: tecla `0xACB3A629` (`,`/`.`) cicla o modo de rotação do
  `cTSMainToolbar` 1 → 2 → 4 → 8. A tecla `0xB1265EF3` (alterna o modo de
  quarto de tile no sim system, vtable `0xcc`/`0xdc`) é **só BV**. O
  `ChangeTile` compara o padrão (`WorldDB` vtable `0x9c`) **e** a rotação
  (`0xa0`) de cada quarto com os da ferramenta (toolbar vtable `0x78`). Nos
  lotes a rotação fica no **3ARY `0x15`**, um byte por quarto de tile, 1 em
  quase todos os quartos dos 62 lotes (inferido pelos dados: os valores do
  modo da toolbar; nenhum lote gira um piso).
- **Ao soltar** (BV `0x301324`): se pintou (modo 0 ou 2), cria um undoable de
  elevação como pré-operação e **aplaina o terreno** sob o retângulo (a altura
  do tile inicial, `LevelRect`) ou sob a sala; submete e toca o som.
- **Regras** (`UserMayPlaceFloorPattern`, BV `0x9a46cc`): área travada/fora do
  lote → `0x51`; água → `0x57`; praia (BV) → `0xA4`; terreno não plano no
  térreo (salvo `UnevenTilesFloorable` do `WorldDB`) → `0x53`; tile não
  "floorable" (no andar de cima, sem parede/apoio) → `0x52`; objeto no tile
  sem a flag od `0x2A` bit 1 → `0x55`.
- **`IsFloorable`** (BV `0x16a990`): acima do térreo o tile tem que ser plano
  e editável; passa se `HasSupport(tile)`; senão, se um dos 4 vizinhos está
  `SupportedUnderneath` (o quarto virado para o tile); e, se algum vizinho tem
  piso (`WorldDB` vtable `0xb4`), se algum tile a até 2 (|dx| + |dy| < 4,
  fora os cantos ±2/±2) está apoiado, ou um do 3×3 passa em `FUN_001654b4`.
  Aqui `HasSupport`/`SupportedUnderneath` = piso no tile de baixo ou parede
  que carrega peso numa aresta dele (inferido).
- **Custo** (`cTSUserToolUndoableFloorPattern::ChangeTile`, BV `0x3ee7e0`):

```
FloorPatternCost(p, reembolso) = floor(preço(p) × (reembolso ? 0,8 : 1))   (BV 0x9a3e50; sem produto = 0)
para cada quarto da máscara cujo padrão ou rotação muda:
   pintar:            custo += 0,25 × FloorPatternCost(novo, não)    (o piso velho não é reembolsado)
   apagar/restaurar:  custo −= 0,25 × FloorPatternCost(velho, sim)
```
  O custo é acumulado em float e convertido no `Cost`.

### 7.5 Revestimento de parede (`cTSUserToolWallpaper`, BV `0x27ec9c`…)

- **Modos** (`+0x54`): 0 uma face, 1 tirar de uma face (Ctrl), 2 todas as
  faces ligadas (Shift), 3 tirar de todas (Ctrl+Shift). `Update` (`0x27f3a4`)
  roda a cada movimento: desfaz a prévia, aplica na face sob o mouse e mostra
  o custo; soltar submete (se tem dinheiro).
- **Uma face** = um `cWallLocator` (trecho + lado). `UserSetWallPattern` (BV
  `0xa5406c`): só para estilos de parede com `mayChangeSurface`; não faz nada
  se a face já tem o padrão.
- **Shift (flood)**, `UserSetWallPatternFlood` (BV `0xa5428c`): parte da face
  clicada e anda pelo anel de faces da sala (camada de paredes do `WorldDB`,
  vtable `0xa0` com 7 = lado A, 10 = lado B; próximo `+0x3c`, anterior
  `+0x40`) nos dois sentidos, pintando cada face **enquanto ela tiver o mesmo
  padrão que a face clicada tinha**; para no primeiro diferente.
- **Tirar** = pôr o padrão padrão do estilo (`defaultPattern`, "blank" na
  parede normal; `FUN_00a53f20`).
- **Custo** (`ChangeWallPattern`, BV `0x394914`; `WallPatternCost` BV
  `0x3941e0`): por face, `+ preço(novo)`; quando o novo é o padrão em branco,
  `− int(preço(velho) × 0,8)`. Pintar por cima não reembolsa o antigo.

### 7.6 Portas, janelas e arcos

São **objetos** (OBJD tipo 8 porta, 9 janela; arcos são portas sem folha),
postos pela ferramenta de objeto (§3), no catálogo de construção pelo campo
69 "build mode type" e 74 "build Mode Subsort" (`LoadBuildObjectsByFunction`,
BV `0xa32a24`: tipo igual e `subsort & campo 74 ≠ 0`). Regras: as de §3.3
com os bits "exige parede" das wall placement flags (sem parede → `7`
"Must be against wall") e a permissão de cruzar parede (p&0x400), mais o
teste de portal da cerca para portões (`MayPlaceFencePortalObject`, BV
`0x8a88a4`: o trecho tem que ter cerca; não pode cruzar arco → `0x85`).

**Recorte na parede**: o objeto tem uma máscara de 8 bits de lados com recorte
(`cTSObject +0x124`; bits 1/2/4/8 = os 4 lados, 0x10–0x80 = as diagonais;
contagem = vtable `0x27c`, BV `0xaf8124`; o i-ésimo trecho relativo = vtable
`0x29c`, BV `0xaf815c`, meio tile para o lado do bit). Para cada parte do
objeto e cada trecho, `nTSSG::cObject::ComputeWallCutoutLocators` (BV
`0x6612d8`) monta o nome do material da máscara com
`WallCutoutMaterialName` (BV `0x658428`):

```
<base>_<dx>_<dy>_<nível>_<dir>_wallMask
   base = STR# 0x85 (nomes de modelo) do objeto, entrada (tag 0xCC + 1)
   dx = "%dE"/"%dW"/"0", dy = "%dN"/"%dS"/"0" (deslocamento do trecho em relação ao objeto)
   dir ∈ S W N E NW SE SW NE   (o lado/diagonal do trecho)
```
Ex. nos dados: `windowdesigner_1e_0_1_n_wallmask_txmt`,
`doorclub2diagonal_1e_1n_0_nw_wallmask_txmt`.

**Correção (rodada 12):** a `base` **não** é o "objectCutout" do CRES (só as
5 pias de bancada têm essa propriedade, para o recorte do tampo).
`ComputeWallCutoutLocators` (BV `0x6612d8`) monta um texto de recurso
(`0xda590`: STR# `0x85`, índice = `GetFieldByTag(0xCC)` do objeto + 1,
língua 1) e usa esse texto; se for vazio, não há recorte. Nos dados o STR#
0x85 de portas e janelas é `["", "<nome do modelo sem _cres>"]` (ex.
`windowQuaint`, `windowQuaintDiagonal`), então a base é a entrada 1 (tag
0xCC = 0, inferido: o índice do modelo atual). Com essa base, **os 100
grupos de OBJD "Window…"/"Door…" do `objects.package` têm os TXMTs
`…_wallmask`** (453 nomes de máscara nos dados; 91 conferidos pelo nome
exato, os outros 9 também existem com o prefixo de tamanho na varredura).
O pré-teste `nTSWorld::ObjectHasWallCutouts` (BV `0x16a064`): a máscara de
lados (vtable `0x27c`) do objeto ou, se for multi-tile (vtable `0x19c`), de
alguma parte, > 0. Para cada parte e cada lado da máscara: dx/dy = posição
da parte menos a do objeto líder, nível = nível da parte + 1, e o nome vai
para `cWallManager::SetCutoutMaterial`. No app (rodada 12,
`formats/lot_model`): a base é o nome do modelo (o CRES que o lote guarda);
para cada trecho reto que a porta/janela atravessa, dx = tiles ao longo do +x
do objeto, nível = andares acima do objeto, e a textura do TXMT (alfa 0 =
buraco; as duas faces usam a mesma) vai na face com u = posição ao longo do
+x do objeto a partir do começo do tile e v de 3 tiles acima do piso até ele
(inferido pelas texturas: as metades de um arco de 2 tiles são "0" e "1e"),
teste de alfa no shader. Diagonais ficam com o recorte retangular.
`SetWallCutout` passa o nome
ao `cWallManager::SetCutoutMaterial` (BV `0x6c5d6c`), que troca o material
daquela face pelo TXMT da máscara (alfa = buraco). Ao apagar uma parede, os
objetos presos a ela são vendidos (§7.2), portas e janelas inclusive.

Portas viram portais do roteamento (`cTSRouteManager::AddDoorPortal`, BV
`0x4f1ea8`) e da luz (portais de `docs/lighting-mac.md` §5.4); não dividem
salas (só partições dividem).

### 7.7 Desfazer na construção

Igual a §5: cada operação é um undoable com pré/pós-operações; `CommitIt`
(BV `0x9b4ff0`) roda pré, a própria, pós absolutas e pós, e grava o custo
total. Pisos e revestimentos guardam por tile/face o padrão (e a rotação)
antigo e trocam com o mundo (`SwapStateWithWorld`/`SwapOneTile`, BV
`0x3ee340`/`0x3ee268`); a terraplanagem guarda as alturas dos vértices.
Ctrl-Z/Ctrl-Y são os IDs `0x0CA297F4`/`0x6CA297FC` do arquivo de teclado
(`ui.package`, tipo `0xA2E3D533`); os botões `0xA1`/`0xA2` do painel chamam
o gerente de desfazer (vtable `0x20`/`0x14` e `0x24`/`0x18`).

### 7.8 Painel de construção (UI script `0x49060003`, `cTSWinProcBuildPanel`)

`onBtnClickedID` (BV `0x287898`): ferramentas `0xA5` mão (ferramenta 7),
`0xA4` conta-gotas (`0x2D`), `0xAB` design (`0x2F`), `0xAA` dia/noite,
`0xA3` vista de cima, `0xA1`/`0xA2` desfazer/refazer, `0xA6` volta ao painel
de categorias (`0xA000`), `0xA7` coleções (só BV). Categorias
`0xA100`…`0xAA00` → `SetFunctionSort(1…10)` (A100 parede, A200 fundações e
varandas, A300 portas e janelas, A400 pisos, A500 revestimentos, A600
escadas, A700 terreno, A800 jardinagem, A900 telhados, AA00 diversos). Cada
uma abre o painel `0xB<k>00`, cujo `…10` é "voltar". Subsorts
(`SetFunctionSubSort(código)`, BV `0x283b54`):

| painel | botões → código |
|---|---|
| B300 portas/janelas | B320 portas, B330 janelas de 2 andares `0x13`, B340 de 1 andar `0x14`, B350 arcos `0x16`, B360 tudo `0x17` |
| B400 pisos | B420 pedra `0x25`, B430 tijolo `0x26`, B440 madeira `0x27`, B450 carpete `0x28`, B460 ladrilho `0x2A`, B470 linóleo `0x29`, B480 moldado `0x2B`, B490 outros `0x2C`, B4A0 tudo `3` |
| B500 revestimentos | B520 tijolo `0x1C`, B530 alvenaria `0x1D`, B540 madeira (siding) `0x1E`, B550 moldado `0x1F`, B560 pintura `0x20`, B570 papel `0x21`, B580 ladrilho `0x22`, B590 painéis `0x23`, B5A0 tudo `1` |
| B600 escadas | B630 escadas `0xE`, B640 de ligação (modulares) `0x10`, B620 ferramenta de corrimão (ferramenta `0xF`) |
| B700 terreno | B720 elevação `0xB`, B730 cobertura `9`, B740 água `10`; B751–B753 pincel P/M/G |
| B800 jardim | B820 árvores `0x18`, B830 arbustos `0x19`, B840 flores `0x1A`, B850 tudo `0x1B` |
| B900 telhados | B920 tipos `6`, B930 padrões `0xD`, B940 águas-furtadas `0xC`, B950 automáticos `0x30` |
| BA00 diversos | BA20 colunas `0x12`, BA30 lareiras `2`, BA40 piscinas `0x2E`, BA50 cercas `0x11`, BA60 portões `0x15` |

Pisos e revestimentos filtram pela chave `subsort` do XML (`"stone"`,
`"brick"`, `"wood"`, `"carpet"`, `"tile"`, `"lino"`, `"poured"`, `"other"`
nos pisos; `"brick"`, `"masonry"`, `"siding"`, `"poured"`, `"paint"`,
`"wallpaper"`, `"tile"`, `"paneling"` nas paredes; `"all"` = todos;
`cTSWinProcCatalog::LoadFloorPatterns/LoadWallPatterns`, BV
`0xa320bc`/`0xa32238`). O painel de telhados automáticos é o `0xAC` (C100 sem beiral, C200
triangular curto, C500/C600 mistos, C300 remover, C700 laje, C400 preservar).

### 7.9 O resto, resumido

- **Escadas** (`cTSUserToolModularStairs`): `modularstairs.txt` define cada
  escada por cinco objetos (patamar, marcador, degrau, degrau de cima, de
  baixo); a ferramenta monta degraus como objetos. Textos de erro próprios
  (STR# 0x89 #58–#71: patamar, degraus, "No ground at top/bottom"). Preço por
  escada (tabela).
- **Telhados** (`cTSUserToolRoof`, `cTSUserToolAutoRoof`): de graça (0),
  salvo águas-furtadas; auto-telhado por sala (`CreateAutoRoofs`,
  `AppendRoomTilesIfValid`); paredes de sótão (estilo 3) acompanham.
- **Fundações, decks e piscinas** (`cTSUserToolDeckFoundation`,
  `deckfoundations.txt`): cada tipo diz o estilo das paredes laterais
  (fundação 23, deck 16/4, piscina 29) e o piso; preço por tile (tabela).
  Os textos "Foundations only allowed on terrain" (código `0x1A`) e "Pools
  only allowed on terrain" (`0x8A`) existem; os testes não foram lidos.
- **Terreno** (`cTSUserToolTerrainSculpt`): só terreno exposto do térreo
  (senão `0x8B` "Only exposed terrain can be edited"); o pincel é aplicado a
  cada **100 ms** com o botão apertado (`ApplyTerrainSculptBrush`, BV
  `0x32a214`; largura 6, força 0,0002/0,00002 do `terrainSculptBrushes.txt`);
  altura arredondada ao passo da ferramenta. Custo (BV `0x4003c8`):
  `round(Σ|Δaltura| × custo por unidade)`, cobrado **na hora** a cada
  aplicação (não no soltar) e recusado se faltar dinheiro.
  `FloorElevationUnitChangeCost` devolve 0. Pintura do terreno
  (`cTSUserToolTerrainPaint`) custa 0.

### 7.10 Códigos de erro de colocação e as fundações/decks/piscinas (rodada 9, binário)

**Tabela completa** do `cTSSimSystem::PlacementErrorString` (BV `0x58a6d4`):
código → índice 1-based do STR# 0x89 do `UIText.package` (o texto é o
`#índice−1` em inglês). Código fora da tabela (ex.: 2) → 1 "Location out of
bounds". Códigos cujo texto é vazio no jogo base (provavelmente só BV):
0x14, 0x15, 0x1d, 0x1e, 0x1f, 0x3c, 0x3d, 0x3e, 0x3f, 0x46, 0x47, 0x54, 0x55, 0x56, 0x57, 0x5d, 0x5e, 0x5f, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0x9b, 0x9c, 0x9d, 0xa5.

| código | STR# 0x89 | texto |
|---|---|---|
| `0x01` | 74 | Must be against a pool wall |
| `0x03` | 2 | Level out of bounds |
| `0x04` | 3 | Must be at center of tile |
| `0x06` | 5 | Cannot create level above highest level |
| `0x07` | 6 | Must be against wall |
| `0x08` | 7 | Can't intersect wall |
| `0x0a` | 9 | Must be placed outside |
| `0x0b` | 10 | Must be placed inside |
| `0x0c` | 11 | Can't intersect other objects |
| `0x0e` | 13 | Can't find a place to put it |
| `0x0f` | 14 | Can't support the weight |
| `0x10` | 15 | Can't support the size |
| `0x11` | 16 | Can't place on top |
| `0x12` | 17 | Can't place on slope |
| `0x13` | 32 | Needs something to support it |
| `0x16` | 21 | Insufficient funds |
| `0x17` | 22 | Must be against unused wall |
| `0x18` | 23 | Must be on diagonal |
| `0x19` | 91 | Must place in one of these places: |
| `0x1a` | 25 | Foundations only allowed on terrain |
| `0x1b` | 26 | Wall next to object in use |
| `0x1c` | 63 | No ground at bottom |
| `0x21` | 32 | Needs something to support it |
| `0x22` | 33 | Mantle rotation must match chimney |
| `0x23` | 34 | Hearth can have only one mantle |
| `0x24` | 35 | Can't delete object |
| `0x25` | 91 | Must place in one of these places: |
| `0x26` | 37 | Can't pick up |
| `0x27` | 38 | Can't select object in use |
| `0x28` | 39 | Can't pick up—location out of bounds. |
| `0x29` | 40 | Fireplace mantle must be all in one room |
| `0x2a` | 41 | Must connect to upper floor |
| `0x2b` | 42 | Can't intersect upper-floor object |
| `0x2c` | 43 | Can't rotate |
| `0x2d` | 44 | Can't delete pool with Sims in it |
| `0x2e` | 45 | Must first delete ladder or diving board |
| `0x2f` | 8 | Ceiling too low |
| `0x30` | 97 | Ceiling too high |
| `0x31` | 4 | Terrain too steep |
| `0x32` | 91 | Must place in one of these places: |
| `0x33` | 91 | Must place in one of these places: |
| `0x34` | 91 | Must place in one of these places: |
| `0x35` | 91 | Must place in one of these places: |
| `0x37` | 91 | Must place in one of these places: |
| `0x38` | 93 | Can't delete object with something on it or in it |
| `0x39` | 38 | Can't select object in use |
| `0x3a` | 38 | Can't select object in use |
| `0x3b` | 98 | Cannot be placed on a diagonal wall |
| `0x41` | 7 | Can't intersect wall |
| `0x43` | 7 | Can't intersect wall |
| `0x44` | 6 | Must be against wall |
| `0x48` | 32 | Needs something to support it |
| `0x49` | 17 | Can't place on slope |
| `0x4a` | 76 | Can't place wall |
| `0x4b` | 75 | Wall too close to Sim |
| `0x4c` | 47 | Cannot build diagonally. |
| `0x4d` | 48 | Can't place next to arch |
| `0x4e` | 49 | Can't delete arch |
| `0x4f` | 50 | Can't remove object on fence |
| `0x50` | 94 | Can't place next to wall |
| `0x52` | 32 | Needs something to support it |
| `0x53` | 17 | Can't place on slope |
| `0x58` | 85 | Must fill within enclosed area or on existing tiles |
| `0x5a` | 51 | Covered by floor tile |
| `0x5b` | 52 | Attached to wall |
| `0x5c` | 53 | Attached to wall |
| `0x61` | 54 | Can't move occupied terrain |
| `0x63` | 55 | Can't raise floor any closer to floor above |
| `0x64` | 56 | Can't lower floor any closer to floor below |
| `0x65` | 57 | Can't lower terrain further |
| `0x66` | 58 | Can't raise terrain further |
| `0x68` | 59 | Can't raise terrain above stairs |
| `0x69` | 60 | Can't raise floor above |
| `0x6a` | 61 | Can't lower floor below |
| `0x6b` | 55 | Can't raise floor any closer to floor above |
| `0x6c` | 62 | No ground at top |
| `0x6d` | 63 | No ground at bottom |
| `0x6e` | 64 | Can't level top landing |
| `0x6f` | 65 | Can't level bottom landing |
| `0x70` | 66 | Can't place top landing |
| `0x71` | 67 | Can't place bottom landing |
| `0x72` | 68 | Can't reserve space above stairs |
| `0x73` | 69 | Can't place step |
| `0x74` | 69 | Can't place step |
| `0x75` | 70 | Can't scoop out terrain |
| `0x76` | 7 | Can't intersect wall |
| `0x77` | 71 | Can't reach ground |
| `0x78` | 72 | Must have at least one step |
| `0x79` | 79 | Must be placed on a fence. |
| `0x7a` | 11 | Can't intersect other objects |
| `0x7b` | 73 | End points at different elevation |
| `0x7c` | 73 | End points at different elevation |
| `0x7f` | 11 | Can't intersect other objects |
| `0x80` | 73 | End points at different elevation |
| `0x81` | 12 | Can't level terrain |
| `0x82` | 76 | Can't place wall |
| `0x83` | 11 | Can't intersect other objects |
| `0x84` | 7 | Can't intersect wall |
| `0x85` | 77 | Can't place gate across arch |
| `0x86` | 78 | Can't extend pool |
| `0x87` | 95 | Can't delete pool |
| `0x88` | 79 | Must be placed on a fence. |
| `0x89` | 86 | Can't place gate on short fence |
| `0x8a` | 90 | Pools only allowed on terrain |
| `0x8b` | 89 | Only exposed terrain can be edited |
| `0x8c` | 91 | Must place in one of these places: |
| `0x8d` | 91 | Must place in one of these places: |
| `0x8e` | 91 | Must place in one of these places: |
| `0x8f` | 91 | Must place in one of these places: |
| `0x90` | 91 | Must place in one of these places: |
| `0x91` | 91 | Must place in one of these places: |
| `0x92` | 96 | Can't delete objects in rewards mode |

Os códigos 0x19, 0x25, 0x32–0x35, 0x37 e 0x8C–0x91 dão "Must place in one of
these places:" seguido da lista de lugares (§3.2).

**Fundações, decks e piscinas** (`cTSUserToolDeckFoundation`; no base a
piscina é um tipo de deck, `deckfoundations.txt`). Cada tile da seleção
passa por `CanPlaceTile` (BV `0x34f5a8`): tile com água (`IsPartWater`) →
`0x1E` (sem texto no base); praia (só BV) → `0x86`; um objeto no tile que
não tem o od 0x22 nem o bit 0x200 ("no ar") da od 42 → `0x0C` "Can't
intersect other objects"; e para cada um dos 4 lados, se o vizinho já tem
deck/fundação de um tipo que não casa com o lado (tabela por direção do tipo
do vizinho: 1 → só 0, 2 → 4/6, 4 → 2/4/6, 8 → 0/2/6) → `0x86` "Can't extend
pool". `MayPlaceQuadrilateral` (`0x352a30`) antes recusa seleção degenerada,
cantos fora do lote (código 2), e vértices cuja altura difere da altura da
ferramenta quando ela não pode aplainar ali (`FUN_009e9550`).

**Custo mostrado** (`PreviewCost`, `0x350830`): soma, para cada tile (ou
quarto) da seleção cuja altura atual difere da da ferramenta,
(`DeckFoundationCost(altura da ferramenta)` − `DeckFoundationCost(altura
atual)`) × fator (0,25 por quarto, 0,5 por metade diagonal, 1 por tile
inteiro; os preços por tile são os do §7.1: fundação 4, deck 2–3, piscina 75),
e arredonda; `CanAfford` decide o cursor.
O resto das regras (escavar piscina, telhados, aplainar terreno) não foi lido.

### 7.11 Terreno, escadas, telhados e piscinas (rodada 9, lido e inferido)

- **Pincel de terreno** (`cTSUserToolTerrainSculpt`): `ApplyTerrainSculptBrush`
  (BV `0x32a214`) roda a cada 100 ms (`OncePerFrameUpdate`). A largura do
  pincel é `W0 + (10 − W0) × 0,5 × escolha` (`SetBrushWidthChoice`, escolha
  0–2; tamanhos do cursor 2, 5,5, 10; aqui W0 = 2, inferido). O perfil é a
  imagem `profileTextureID` do `terrainSculptBrushes.txt`: TGA 32×32 cinza
  (tipo 3) no `terrain.package`, grupo `0x4B72DA28`, tipo `0x856DDBAC`.
  `RaiseBrushFn`/`LowerBrushFn`: altura nova = (altura ± perfil × força) / passo,
  arredondada para cima (subir) ou para baixo (descer), × passo; o passo é a
  vtable `0x158` do `WorldDB` (não lido; os lotes salvos têm alturas em
  múltiplos de 0,00625, que é o que usamos). `ChangeTerrainElevation` (BV
  `0x4010e0`): vértice que não pode mudar (`FUN_009e9550`) não muda;
  `UpdateFundsForHeightChange` acumula |Δ| e cobra `round(Σ|Δ| × custo por
  unidade)` − o já cobrado, recusando sem fundos; `ChangeFloorElevationCoupled`
  leva os andares de cima junto. As funções de nivelar/suavizar não têm nome
  (aqui: até a altura do início do traço / pela média dos vizinhos).
- **Pinturas**: `DiminishAllPaints` tira das outras o que a pintura ganha;
  máscaras de 4 pontos por tile (`2ARY 0x5CBC + i`, `LTTX` com a base e os
  nomes).
- **Escadarias**: as do jogo base são objetos ("Stairs - Straight - *", tipo
  de construção 1, subsort 0x20); as partes "Hi" ficam um andar acima pelo
  campo de OBJD 0xB0 "level offset" (o mesmo põe a parte da escada de piscina
  no fundo, −1). As escadas modulares (`modularstairs.txt`) são as de ligação.
- **Telhados**: o ROOF guarda id, retângulo, nível (o andar de cima das
  paredes), tipo, padrão (um só no lote), ângulo (v2) e estilo (v3). Empenas
  curtas/longas têm paredes de sótão (estilo 3) nas pontas; a regra "pontas
  não cobertas por outro telhado do andar" dá 788 das 905 paredes de sótão dos
  62 lotes (`MakeShedWalls` não foi lido).
- **Fundações**: o andar 1 fica a 0,75 acima do terreno nos vértices da
  fundação (e os de cima junto), paredes 23 na borda, piso
  "floor_plywoodFoundation".
- **Piscinas**: POOL = id, (x, y) do canto, nível 0, largura, altura, 1,
  profundidade da água (0,25), 0; o lote tem o andar −1 (elevações −3), o
  fundo tem o piso "floor_poolroundedlipblue" e as paredes 29 ficam no andar
  −1. Na colocação, tile de piscina sem od 42 0x800 → `0x8F`.
- **Rotação de piso**: 3ARY `0x15`, um byte por quarto (1/2/4/8).

### 7.12 Escadas modulares, auto-telhado e a ferramenta Design (rodada 11, binário)

**Escadas modulares** (`cTSUserToolModularStairs`; o posicionamento é de
`TSUserModularStairsPlacement.cpp`: `UserPlaceModularStairs` = BV
`0x9edcf8`, `UserPlaceModularStairsFindEnd` = `0x9ece38`, o auto =
`0x9ef328`, sem nome no symxfer). Cada produto do `modularstairs.txt` dá
cinco objetos, nesta ordem (`produto→0xB0(i)`): 0 `landing` (patamar), 1
`placeholder` (reserva de espaço), 2 `step`, 3 `topStep`, 4 `bottomStep`
(0 = usa o `step`). Todos os produtos usam o mesmo patamar `0xAC400DE1` e
reserva `0x0C400DDD`.

- Clique no **tile de cima** (um tile de piso com borda livre): o tile tem
  de ser válido (`WorldDB→0xB8(tile, 0xF, 1)`; senão `0x6C`) e não pode ser
  água. A direção é a da borda livre do tile (byte `+0xC`: 1 → direção 2,
  2 → 0, 4 → 6, 8 → 4, as direções de 8 do mundo).
- **Sem arrastar** (`TryAutoPlaceStairs` → `FindEnd`): anda tile a tile na
  direção, andar por andar para baixo, até achar onde o último degrau toca
  o chão (para em água `0x1D`, piscina `0x1C`, parede `0x71`, objeto
  `0x74`); depois `UserPlaceModularStairs` até lá.
- **Arrastando até um ponto**: o ponto tem de estar na mesma linha ou coluna
  do tile de cima e na direção da borda livre (senão `0x71`); mesmo tile →
  `0x78`. **n degraus** = arredondado((d − 0,5) × 4 + 0,5), d = distância no
  plano do centro do tile de cima ao ponto: **4 degraus por tile, cada um
  com 0,25 de avanço**; com a opção de tiles inteiros (`param 3`) n sobe ao
  múltiplo de 4. Cada degrau desce `WorldDB→0x158` (altura de um degrau; o
  andar inteiro em 4 tiles, inferido), começando na altura do piso de cima
  (+0,75 se o tile de cima é de fundação).
- Por degrau: se o terreno fica a menos de 0,75 abaixo e há deck ali →
  `0x78`; se fica acima do degrau (folga −0,09375) o **terreno é rebaixado**
  (`ChangeFloorAndWaterElevationCoupled` nos 4 vértices do quarto de tile,
  na altura do degrau; vértice travado → `0x6F`/`0x75`); no último degrau os
  vértices do tile seguinte vão à altura final. Objetos: o `topStep` no
  primeiro, `step` no meio, `bottomStep` no último (falha: `0x73`/`0x74`);
  a cada andar atravessado, no degrau de índice 2 do tile, um `placeholder`
  em cada andar entre o de baixo e o de cima (falha: `0x72`).
- **Patamares**: um `landing` no tile de cima (direção da escada; falha
  `0x70`), outro meio tile além do último degrau, virado 180°
  (`(dir + 4) % 8`; falha `0x71`). O tile de baixo tem de ser válido e sem
  piso alto (`0x6D`), o último degrau a no máximo 0,09375 do chão (senão
  `0x77` "Can't reach ground"), sem parede no caminho (`0x76`), e o tile de
  baixo nivelável (`0x6F`).
- Os objetos vão para um `cTSUserToolUndoableModularStairs` (`AddObject`
  BV `0x3ffc28`: põe o od 13 e o od 41 do objeto, testa
  `TSObjectUserCanSetPositionAndOrientation`, gira e posiciona) e o
  `LinkObjects` (`0x3ff4a4`) liga cada objeto ao anterior e ao seguinte (os
  IDs vizinhos, −1 nas pontas) — é a cadeia que a VM e o roteamento usam.
  Apagar (marreta `DeleteModularStairs` → `UserRemoveModularStairs`, BV
  `0x9ebd54`: se `TSObjectGroupUserCanDelete(grupo)`, chama
  `RemoveFlight(grupo)`, BV `0x3fff3c`, vtable `0x70` do undoable) tira a
  escada inteira pelo grupo: para cada objeto do grupo cria a
  suboperação de apagar objeto (a devolução normal do objeto) e, se o
  **modelo do corrimão** do objeto (`ObjectModel`, BV `0x3ff17c`: o dado 0
  do objeto depois da mensagem `0x30`) é > 1, **soma à devolução o preço do
  corrimão** (o produto `0x2C7BF690`, vtable `0x5c`): 2× se o modelo é 4
  (corrimão dos dois lados), 1× nos modelos 2–3, nada nos 0–1 — o mesmo
  `RailingCost` do lote (`docs/neighborhood-mac.md`). (Corrige o "2× se o
  grupo tem 4 partes" da rodada 11.) O custo mostrado é
  `CalculatePreviewCost`.

**Auto-telhado** (`cTSUserToolAutoRoof::CreateAutoRoof(tipo, ...)`, BV
`0x2560ac` → `CreateAutoRoofs(andar, tipo, ...)` `0x9a0df0` para cada andar
1…topo):

1. Marca numa grade os quartos de tile dos **cômodos do andar de baixo**
   (`AppendRoomTilesIfValid`, `0x99947c`) que são dentro, cujo tile acima
   não é dentro, sem piso no andar e sem telhado já cobrindo
   (`RoofManager→0x7C`).
2. Tipo 5 (mansarda): só **põe o piso do telhado** (`roofFloorPattern` do
   `roofs.txt`, `CCC26098`) em cada tile marcado (`0x99977c`).
3. Outros: `cAutoRoofData::ComputeRoofRects` (`0x9a0534`) decompõe a grade
   em retângulos por varredura: em cada linha acha os trechos marcados;
   `AddToActiveRects` estende para baixo o retângulo ativo com o mesmo
   trecho x, corta os que se sobrepõem em parte; os que não continuam saem
   da lista ativa. `0x9a0a08` (com 2,0) tira os retângulos redundantes:
   conta a cobertura de cada célula e descarta um retângulo maior que 2,0
   cujas células todas continuam cobertas por outros.
4. Para cada retângulo de área > 0, o tipo do ROOF sai de `0x997f8c(tipo
   da ferramenta, retângulo)`: 1 → 5; 2 → 4; 3 → 5 se altura ≤ largura,
   senão 4; 4 → 3 se altura < largura, senão 4; outro → 0 (tipos do ROOF:
   tabela abaixo). Cria o telhado
   (`cTSUserToolUndoableRoof→0x6C(padrão, mín, máx, tipo)`) no andar; os
   tipos 3 e 4 (empenas) ganham as **paredes de sótão** nas pontas
   (`0x9990f4` → `MakeShedWalls`).
5. Antes, sem a flag "manter", remove os telhados auto anteriores
   (`0x99eafc`); custo total no fim, recusado sem fundos.

**Botões do painel de auto-telhado** (`cTSWinProcBuildPanel::
onBtnClickedID`, BV `0x287f18`…): cada botão manda à ferramenta o valor
`2·tipo + (C400 marcado)`, e o `cTSUserToolAutoRoof::Init` (BV `0x2571c4`)
usa `tipo = valor >> 1`, "manter os telhados" = bit 0 (valor 0 →
`RemoveAllRoofs`, `0xDEADBEEF` → nada). Conferido no código: **C100 → 1
(tudo de quatro águas, ROOF 5), C200 → 2 (tudo empena, ROOF 4), C500 → 3
(quatro águas onde altura ≤ largura, senão empena), C600 → 4 (empena curta
ROOF 3 onde altura < largura, senão empena longa ROOF 4), C700 → 5 (laje:
só o piso de telhado), C300 → 0 (remover)**, C400 = a caixa "preservar".
É o mapeamento que o `build_mode.cpp` já usa.

**Tipos do ROOF** (`nTSSG::cRoofManager::AddRoof(id, mín, máx, tipo)`, BV
`0x6bf488`; classes conferidas pelo `ComputeGridElevation` que cada
construtor chama, porque o symxfer troca alguns nomes de construtor):

| tipo | classe | `roofs.txt` / nome no catálogo (STR# 0x8C do `Build.package`) |
|---|---|---|
| 1 | `cHalfGableRoofType` | produto 0 `roof-ui-gable-shed`, "Shed Gabled Roof" |
| 2 | `cHalfHippedRoofType` | produto 1 `roof-ui-hipped-shed`, "Shed Hipped Roof" |
| 3 | `cTallGableRoofType` | produto 2 `roof-ui-gable-tall`, "Short Gabled Roof" (cumeeira no eixo curto) |
| 4 | `cGableRoofType` | produto 3 `roof-ui-gable`, "Long Gabled Roof" (cumeeira no eixo longo) |
| 5 | `cHippedRoofType` | produto 4 `roof-ui-hipped`, "Hipped Roof" |
| 6 | `cMansardRoofType` | produto 5 `roof-ui-mansard`, "Mansard Roof" (tem `roofFloorPattern`) |
| 7 / 8 / 9 | cônico / cúpula / octogonal | (não estão no `roofs.txt` do base) |
| 10–14 | diagonais: empena, empena alta, quatro águas, meia-empena, meia-quatro-águas | `IsDiagonalRoofType` (BV `0x163c6c`): máscara `0x387C00` = 10–14 e 19–21 |
| 15 | `cGlassGableRoofType` (estufa) | `IsGlassRoofType` (`0x163c5c`) |
| 16–18 | pagode: quatro águas, empena, meia-empena | |
| 19–21 | pagode diagonal: quatro águas, empena, meia-empena | |
| 22 | octogonal (outra variante, mesmo construtor do 9) | |

O produto *k* do `roofs.txt` é o tipo *k* + 1 (inferido da ordem:
catálogo e `AddRoof` batem um a um, e o produto 5, a mansarda, é o único
com `roofFloorPattern`). Os lotes do base só usam 1–6.

**Águas-furtadas** (roofs 6–11 do `roofs.txt`: 1, 2 e 3 de largura, com
empena ou de quatro águas) são produtos de telhado comuns
(`cTSWinProcCatalog::LoadDormers`); diferente dos telhados normais, têm
preço (§7.9).

**Ferramenta Design** (botão `0xCC607030` do modo compra;
`cTSUserToolObjectDesign`, BV `0x3022fc…`): cursor `0x39`. Botão apertado
sobre um objeto: se ele **não tem partes recoloríveis** → erro (STR# 0x8D
#16 do grupo `0x7FA7DC7C`); se não há dinheiro para o **preço da
ferramenta** (o produto do catálogo da ferramenta, `+0x4C`, lido no
`Init`) → erro #15. Textos (rodada 12, `Text/UIText.package`, STR# 0x8D, grupo local
= `0x7FA7DC7C`; os números são 1-based como os do código, entradas 14 e 15
do dump): #15 "Insufficient Funds!", #16 "This object is not designable."
(o comentário da EA: "message appears when trying to use design mode tool
on non-designable object"). Soltar sobre o mesmo objeto abre o
`cTSWinProcObjectDesignDialog::Show(objeto, preço, x, y)` (BV `0xa16e64`).

- "Recolorível" (`nTSSG::GetDesignModeInfo`, BV `0x9e2904`): percorre os
  shapes do modelo; um subconjunto (grupo de material) entra se a
  propriedade **`tsDesignModeEnabled`** do shape lista o nome dele e o
  nome não contém "shadow"; o primeiro vira o **material 1** e o segundo
  (de nome diferente) o **material 2**; para cada um, o nome da malha de
  materiais vem da propriedade **`tsMaterialsMeshName`** do shape (+
  `_cres`). No máximo dois.
- O diálogo: título = STR# 0x93 #0x15 com o nome do objeto (`sprintf`),
  nome em `0xA3000`, duas grades de opções (controles `0xA1000`/`0xA2000`;
  a segunda some se o objeto só tem um material), carregadas pela malha de
  materiais e o subconjunto (`→0x28(objeto, malha, subconjunto)`); a
  janela é arrastável pela barra `0xA7000`. **Cada clique numa opção
  (comando `0xEC58BA86`) já aplica no objeto** (`ApplyChanges`: os dois
  materiais escolhidos como override, `objeto→0x478`, e redesenho
  `→0x490(1)`): pré-visualização ao vivo; `0xA6002` (aceitar) fica
  habilitado depois de uma mudança.
- Aceito: um `cTSUserToolUndoableObjectDesign` (`0x8D0FA620`) guarda os
  overrides antigos (`SetObject`: a lista `objeto→0x47C/0x480`) e os novos
  (`ApplyMaterialOverride`), cobra **−preço** dos fundos no ponto do
  clique e entra no desfazer; cancelado, volta os materiais
  (`ResetChanges`). O objeto com o override salva o material escolhido
  (lista de overrides do objeto; ver `docs/save-mac.md`).

### 7.13 Escadas modulares no código (rodada 12, `vm/build_edit` `placeStairs`)

Lido de `UserPlaceModularStairs` (BV `0x9edcf8`), `FindEnd` (`0x9ece38`),
`CalculatePreviewCost`, `AddObject`, `LinkObjects` e `RemoveFlight`, e
conferido com os lances salvos (`N001_Lot38`: refazer o lance dá os mesmos
objetos, níveis, elevações, direções e elos):

- Constantes: degrau = 0,25 de avanço e 0,1875 de altura (`WorldDB→0x158`;
  inferido dos lotes); tolerâncias `0x274b870` = 0,09375 (último degrau até o
  chão), `0x274bbb8` = −0,09375 (terreno acima do degrau → cavar),
  `0x274b474` = 0,001 (regra do deck).
- n degraus = ceil((d − 0,5) × 4), d = distância do centro do tile de cima ao
  ponto do arraste (o truque de conversão `+ 1,5·2²³` com o ajuste de inteiro
  exato do código); a prévia de custo usa round e multiplica pelo preço **por
  degrau** do produto (STR# 139 3i; o PT-BR deixa vazio o preço de DeFumê e
  Forja: vale o inglês, 80 e 85).
- O ponto do arraste é onde fica o **último** degrau. O degrau i fica em
  borda + u × 0,25 × (i + 1), z = topo − 0,1875 × (i + 1); o de cima está no
  nível do tile de cima, os outros no nível de baixo; um `placeholder` no
  degrau de índice 2 de cada tile, em cada nível de baixo+1 até o de cima;
  patamar no centro do tile de cima (direção da escada) e meio tile depois do
  último degrau (direção + 4).
- Cavar: se o terreno a ±0,5 do degrau (de lado) está mais de 0,09375 acima
  dele, os cantos do tile do degrau descem: os de baixo à altura do último
  degrau do tile, os de cima (salvo k = 0) à do primeiro. O tile depois do
  último degrau vai **sempre** à altura dele (também sobe: um arraste curto
  ergue o chão). Vértice preso → `0x6F`.
- Elos: `LinkObjects` só liga os degraus (lista `+0x68`): od **80 = próximo,
  81 = anterior** (−1 nas pontas); patamares e placeholders ficam na lista
  `+0x74`, sem elos. O degrau de cima é XMTO. Valor (od 41) dos degraus = o
  preço por degrau (os salvos têm 40 no Madeira); a marreta devolve o od 41.
  `RemoveFlight` soma o preço do corrimão (`0x2C7BF690`): 2× com o modelo de
  corrimão 4, 1× com 2–3, nada com 0–1 (rodada 13: feito, ver §7.13.1).
- Estado salvo: a "elevation" do degrau é a altura sobre o piso do nível de
  baixo, também a do degrau de cima (inferido: o terreno é 0 sob todos os
  lances salvos); patamares e placeholders 0.
- Inferido aqui: os pisos do andar de cima sobre os degraus são tirados (como
  a escadaria-objeto); a parede no caminho conta só paredes e cercas (as bordas
  de fundação/deck não); objetos que bloqueiam = camada de piso (od 63 0 ou
  bit 1; janelas não); a direção sem arraste = o lado do tile mais perto do
  clique (o quarto do `cFloorTile`); a busca do fim (`FindEnd`) simplificada
  (anda degrau a degrau até o último tocar um piso abaixo).
- `FUN_009e9550` (pode o vértice ir a essa altura?) tem uma lista de testes por
  máscara: limites de altura do terreno `WorldDB→0x150/0x14C` (`0x65`/`0x66`;
  rodada 13, `FUN_009e7700`: `MinTerrainElevation` = −150 ×
  `FloorElevationUnit` e `MaxTerrainElevation` = +150 ×
  `FloorElevationUnit`, com a unidade = **0,1875** (`cWorldDB +0x44`, posta no
  construtor `FUN_000d4c98`), ou seja **−28,125 / +28,125**; altura <
  mínimo → `0x65` "Can't lower terrain further", > máximo → `0x66` "Can't
  raise terrain further"; o mesmo construtor põe `+0x3c` = 4, `+0x40` = 5),
  folga para o andar de baixo/de cima (`0x69`/`0x6A`), objetos que prendem o
  chão (`0x61`), objetos de teto (`0x60`), praia. Aqui: "livre de piso,
  parede e fundação" e, desde a rodada 13, os limites de altura
  (`terrainLimitError`): o pincel de terreno e de água deixa o ponto onde está
  e dá `0x65`/`0x66` quando nenhum se move, as escadas modulares recusam com
  `0x65`/`0x66` em vez de `0x6F`, e o nivelamento das paredes, fundações e
  piscinas não move um ponto além deles.

### 7.13.1 O modelo do corrimão e a ferramenta de corrimão (rodada 13, binário)

O "modelo do corrimão" não é um campo do objeto: `RailingObjectModel` (BV
`0x523d10`, igual ao `0x3ff17c` do undoable das escadas) roda a **entrada 48
do OBJf** (`cTSObject::RunTree(0x30, …)`, vtable `0x3b4`) e, se ela devolve
verdadeiro, lê o **temp 0** do objeto (vtable `0x4c` = `GetTemp`, os 8 temps
em `+0x8a`; o vizinho `0x60` é `SetTemp`); senão 1. Nas escadas modulares a
entrada 48 é o semiglobal `0x2018` "Function - Get Model Type" do
`StairModularGlobals`: gráfico (od 0) = 0 → 1, senão (gráfico − campo `0x48`
do OBJD) / 3 + 1. A entrada 47 (`0x2001` "Function - Set Model by Type",
parâmetro 0 = modelo 1–4) põe gráfico = campo `0x48` + 3·(modelo − 1): o
gráfico é o índice no STR# `0x85` (1–3 sem corrimão, 4–6 "Left Railing",
7–9 "Right Railing", 10–12 "Both Railing"; meio, base e topo). Mudar o od 0 é
o "change graphic" do motor (erro `0x48` da VM): o modelo 3D passa a ser o
dessa entrada. `RailingCost` (BV `0x523da4`): 0/1 → 0, 2–3 → 1×, 4 → 2× o
preço do produto `0x2C7BF690` = a "Railing Tool" do `userToolProxies.txt`
(STR# 139 entrada 0: §3). `ComputeStats` o soma ao valor das escadas (tipos
0xB/0xC) e `RemoveFlight` à devolução de cada parte.

**Ferramenta de corrimão** (`cTSUserToolStairsRailing`, botão B620, ferramenta
`0xF`; `Init` lê o preço do produto `0x2C7BF690`, cursor `0x3A`):
`OnAnyMouseEvent` acha o objeto sob o mouse; tipos 10–12 (escadaria-objeto,
degrau modular, portal) e o lado = `PointIsOnLeftSideOfObject` (para o tipo 10,
o contrário). `ChangeRailings`: se o objeto tem grupo (lance), todos os do grupo
de tipo 0xB–0xC; senão só ele. `ChangeOneRailing`: modelo atual m
(`ObjectModel`: o temp 0 qualquer que seja o retorno; 0 → 4 no tipo 10, senão 1);
pôr (sem Ctrl): m = 1 → 2 (lado falso) ou 3 (verdadeiro); m = o outro lado → 4;
já tem → nada; tirar (Ctrl, preço negativo): m = 4 → 3 (lado falso) ou 2
(verdadeiro); m = o do lado → 1. Cada troca é um `TrySwapModel` (roda a
entrada 47 com o modelo novo e soma o preço ao custo do undoable). Os lances
dos lotes salvos têm gráfico 0 com modelos de corrimão no cObject (feitos antes
desse mecanismo): para o jogo são modelo 1 (sem devolução de corrimão).

No código (`vm/placement` `railingObjectModel`/`railingCost`/`setRailingModel`,
`BuildEditor::changeRailing`, `VM::railingPrice`): a marreta devolve o
corrimão, o valor do lote o conta, a ferramenta de corrimão (B620 no app,
headless `--railing id,lado[,delete]` no app e no `lot-save`) põe e tira com
desfazer; o app troca o modelo quando o od 0 muda durante a sessão
(`VmObjectView::updateGraphic`) e o `lot-save` grava o modelo do gráfico num
cObject novo. Inferido: o lado é o do eixo da direção do objeto (o binário
compara x ou y conforme a direção, nos eixos dele), e o cObject já salvo de um
degrau não troca o nome do modelo (o od 0 vai no estado).
`docs/screenshots/r13-construcao-corrimao.jpg` (antes/depois: corrimão dos
dois lados).

### 7.14 Águas-furtadas no código (rodada 12, `placeDormer`)

`cTSUserToolRoof`: o estilo da ferramenta (`+0x88`, o id do produto do
`roofs.txt`) é água-furtada se `FUN_00995400` (= `IsStyleDormer`) diz que sim
(6–11). `Init` põe como cursor o modelo do `defaultRoofPattern`
("dormerpreview-2wide") deslocado de largura × 0,5 e liga as teclas de girar;
no soltar, `FUN_001d5d20` (= `MakeDormer`):

- largura w = (estilo − 6) % 3 + 1, fundo w + 2; nível = o do local clicado
  (o sótão); se o de cima não existe é criado (WorldDB `0x1C`), acima do topo
  → `0x06`;
- direção (`+0xB4`): 6 (padrão) w × fundo em y × x = fundo × w; 2 igual
  começando `fundo` tiles a −x; 0 = w em x × fundo em y; 4 igual começando
  `fundo` a −y;
- `FUN_001d5a5c` testa as 4 linhas (vértice com parede → `0x08`;
  `UserMayPlacePartition`), `FUN_00998cd8` põe paredes **estilo 1** nas 4
  linhas no nível do sótão (dir 0 anda +y, 6 anda +x) e depois as paredes de
  sótão (estilo 3) nas pontas curtas;
- `UserMayAddRoof(ângulo, canto, canto oposto, tipo)` no nível de cima: tipo
  **4** (empena longa) para 6–8, **5** (quatro águas) para 9–11.

Não há "água-furtada" no lote salvo: ela vira paredes e um ROOF comum (nenhum
dos 62 lotes tem estilo ≠ 0). O preço do catálogo (560/840/1120) é igual a
8/12/16 trechos × §70: as paredes entram como pré/pós-operações do undoable do
telhado e o `TotalCost` soma o custo delas (o telhado é de graça); aqui cada
trecho novo custa §70. Inferido: o teste de suporte da parede fica de fora
(a água-furtada fica sobre o telhado), o topo é o nível 4 do lote, e o canto é
o ponto de grade sob o mouse (o jogo usa o cursor deslocado).

### 7.15 Retângulos do auto-telhado (rodada 12, `autoRoofRects`)

Lido no código (`ComputeRoofRects` `0x9a0534`, `AddToActiveRects`
`0x99dd80`, `MoveInactiveRects`, `0x9a0a08`, `FUN_01969950`):
`cRoofRect` = [x0, x1) × [y0, y1) + a linha em que cresceu por último; os
ativos num `set` (`cRectLessThan`). Para cada trecho marcado da linha y:
todo ativo **contido** no trecho cresce uma linha; todo ativo que só
**cruza ou encosta** (x0 ≤ fim e início ≤ x1) dá um retângulo novo com a
interseção em x, do topo dele até esta linha (se nenhum que cresceu tem o
mesmo x0/x1); se sobra célula do trecho em nenhum dos dois, o trecho inteiro
entra como ativo. Depois da linha, os ativos que não cresceram nela vão para a
lista final. Ou seja, os retângulos **se sobrepõem** (um L dá o braço largo e
o estreito do topo até o fim). Depois, com 2,0: a cobertura de cada célula
é contada sobre todos; a lista é ordenada pela razão de aspecto
(max(l/a, a/l), lados ≥ 1) decrescente e percorrida da frente: um retângulo
com razão ≤ 2 fica; um mais alongado tem a cobertura das suas células
diminuída e só fica se alguma célula chega a < 1.

### 7.16 Telhados diagonais (rodada 12)

`cTSUserToolRoof::MakeRoof` (BV `0x1e0480`) escolhe a geometria pelo
**estilo** da ferramenta (o id do produto do `roofs.txt`): 0–5 → tipos 1–6;
`0x0C`/`0x11` cônico, `0x0D` cúpula, `0x0E–0x10` octogonal; `0x12–0x16` →
diagonais 10–14 (`DiagonalRoofFromType`, `IsDiagonalRoofStyle`); `0x17–0x19`
vidro; `0x1A–0x20` pagode. O `roofs.txt` do jogo base só tem os produtos 0–11:
**no jogo base não há como fazer telhado diagonal** (nem o auto-telhado: a
grade dele é de tiles inteiros). Aqui a ferramenta aceita os tipos 10–14 só
pelo headless (`--roof x0,y0,x1,y1,10..14`: arrastar de um canto do losango
ao oposto), com a geometria do OpenTS2 que já desenhava os tipos; as paredes
de sótão diagonais (`CheckWallLegalityDiagonal`) não são feitas.

### 7.17 Lagos: a ferramenta de água (rodada 12)

`cTSUserToolWater` (BV `0x1e600c…`) é um pincel de terreno: só no térreo
(senão `0x8B`), aplicado a cada 100 ms com o botão apertado
(`OncePerFrameUpdate` → `ApplyWaterBrush`, temporizador + 100), largura
`W0 + (10 − W0) × 0,5 × escolha` e cursor 2/5,5/10 (`DAT_02732980`), perfil
`spWaterBrushProfile` (TGA `0x8B731811`). `WaterBrushFn` (`0x1e61a0`):
altura nova = floor((h − (perfil − limiar) × força) / passo) × passo, com o
contexto do `InitBrushContext` (limiar 0, força 1e-5) — **cava**;
`cTSUserToolUndoableWater::ChangeTerrainHt` testa o vértice
(`FUN_009e9550`), move o terreno (`ChangeFloorElevationCoupled`) e anota o
vértice; `UpdateFundsForVertexEdit` cobra pelo preço do catálogo (STR# 133:
§1). Depois de cada aplicação `AdjustWaterElevations` →
`cWorldTerrain::UpdateWaterElevations` (`0x8b185c`): os vértices anotados mais
a água já ligada a eles (8 vizinhos, `DAT_02737e00/20`);
`ChooseWaterElevationsForPondWithVertex` (`0x8b11ec`) inunda cada grupo e põe a
água 0,25 (`DAT_0277a578`) abaixo do menor terreno da borda (vizinhos fora
do grupo); vértice com terreno mais de 0,25 acima dessa água fica seco
(água = terreno − 1,0, `DAT_0277a57c`); `AdjustWaterElevationsAtNonPondVertices`
(`0x8afbe4`): o seco ao lado de um lago fica com a água do vizinho − 0,1
(`0x274b680`), os outros com terreno − 1,0. A água é o 2ARY `0x3B76`
([y][x], (L+1) × (A+1)), agora gravado (os 62 lotes voltam iguais).
Inferido: o botão da ferramenta no painel (B740); o que `UpdatePondFlag` faz
além de marcar o lago.

### 7.18 Piscinas que não são retângulos (rodada 12)

No jogo base a piscina é um tipo do `cTSUserToolDeckFoundation` (tile a
tile, `CanPlaceTile`, o estilo por tile no `WorldDB::DeckFoundationStyle`);
as piscinas curvas (`cTSUserToolCurvedPool`) são só BV. Os 7 POOL dos lotes
salvos são todos retângulos. Aqui: um retângulo arrastado sobre ou junto de
uma piscina a **estende** (os tiles que já são piscina ficam e não se pagam;
o chão vai à altura da piscina), as paredes de piscina seguem a borda da união
e os tiles novos viram retângulos POOL sem sobreposição (linhas de mesmo
trecho juntadas), para a água cobrir só os tiles; Ctrl apaga a piscina inteira
(os POOL ligados por uma aresta). Inferido: o POOL de uma piscina em L no jogo
(nenhum lote salvo tem); a recusa `0x86` só vale para deck/fundação junto de
piscina (não feita).

### 7.19 Arcos de cerca (rodada 12)

`fencearches.txt` tem um só arco no jogo base (`fenceArch 26`, "Arco de
Madeira em Treliça", modelos `fenceDeckAgedWoodArchTrellis{Center,East,West,Post}`,
STR# 143 índice 18: §50). É uma partição de `PartitionType` 2:
`AddWallToGraph` a põe só no grafo `+0x58` (WGRA **0x13**), em nenhum outro
(nem no 5/0x18 com os IDs da camada de paredes), então ela fica na aresta de
uma cerca sem tirar a cerca; nenhum dos 62 lotes tem uma. Erros: `0x79`/`0x88`
"Must be placed on a fence.", `0x4D` "Can't place next to arch"
(`cPartitionManager::FenceArchAdjacent`), `0x4E` "Can't delete arch",
`0x85` "Can't place gate across arch". O corpo de `UserMayPlaceFenceArch` (rodada 13,
§7.19.1) está em `0x9a92fc`. Aqui: o arco
vai numa aresta com cerca (`0x79`, inferido: o binário não o testa ali), depois
os testes do `UserMayPlaceFenceArch` (rodada 13, `fenceArchBlocked`: `0x45`
nenhum dos dois tiles dentro do `IsInBounds` estrito e da área editável;
`0x4F` objeto no moirão de uma ponta; `0x0C` arco numa ponta ou objeto de um
dos 6 tiles da aresta e das extensões cuja pegada encosta num poste: as
pegadas são tiles inteiros aqui, então qualquer objeto dos quatro tiles em
volta de um poste, inferido), custa o preço do catálogo, Ctrl tira (0,8 de volta) e apagar a
cerca leva o arco; gravado como linha do grafo 0x13 (lido de volta como o
estilo 26: inferido). Desenho (inferido pelos nomes): o centro sobre a aresta,
um poste em cada ponta, a parte oeste no poste do início e a leste no do fim.

#### 7.19.1 `UserMayPlaceFenceArch` no binário (rodada 13)

`UserMayPlacePartition` (BV `0x9a95cc`) manda os estilos de arco para
`UserMayPlaceFenceArch` (BV **`0x9a92fc`**, o "near" do symxfer em
`0x2972fc` era outra função), que **não olha se há cerca** (o `0x79`/`0x88`
"Must be placed on a fence." e o `0x4D` "Can't place next to arch" não
saem daqui nem de nenhum `li r4, código` antes dos construtores de erro do
binário; inferido: vêm da ferramenta da interface, por outro caminho).
Ordem (com lista de erros, segue juntando erros; sem lista, sai no primeiro):

1. os tiles dos dois lados da aresta (`FUN_008b4e9c`/`FUN_008b51d4`):
   nenhum passa no `WorldDB` vtable `0x108` (na interface `cIWorldDB`, o
   nome do symxfer é `IsInBounds(tile)`; `0xf8` é `IsOnLot(tile)`) → `0x45`;
2. para cada ponta da aresta, um **objeto no moirão** daquele vértice
   (`FUN_00163758`, a mesma busca de `EraseFencePostObject` e
   `TryPlaceOnFencePost`) → `0x4F` "Can't remove object on fence" (um erro
   por ponta);
3. `MayPlaceFenceArchWithRespectToObjects` (BV `0x9a8cb8`) falso → `0x0C`
   "Can't intersect other objects" e falha.

`MayPlaceFenceArchWithRespectToObjects`:

- falha se o módulo de cercas do grafo de cena (`TSSGSystem` vtable `0x8c`,
  depois vtable `0x48(vértice)`) diz sim para uma das duas pontas (inferido:
  já há um arco no vértice, `nTSSG::FenceArchAtVertex`; é aqui que um arco
  vizinho é recusado, com `0x0C`);
- três trechos: a própria aresta e um trecho de **1 tile** que a prolonga
  em cada ponta (pelo eixo dela: se |x0 − x1| < 1, a aresta corre em y);
  para os dois tiles de cada trecho (um de cada lado), cada objeto, salvo os
  de od `0x22` ≠ 0 ou com o bit `0x400` da od `0x2a`: se a pegada dele
  (`GetFootprint` na posição e orientação dele) cruza a caixa de **±0,05**
  em volta de uma das duas pontas → falha. Ou seja, só os objetos que
  encostam nos **postes** do arco o impedem; o meio da aresta pode ter
  objetos.

`TryRemoveFenceArch` (BV `0x30c514`): apaga os objetos do tile da aresta
presos àquela aresta (vtable `0x26c(aresta)`), salvo os de od `0x3b` =
`0x7d` com a vtable `0x348` verdadeira. `AddFenceArch` (BV `0xc348cc`): os
dois tiles têm de existir; `AddWallToGraph`, guarda o estilo do arco num
mapa (`this+0xa4`, chave = o ID da aresta no grafo `+0x58`) e, com a
mensagem ligada no `WorldDB`, manda `0x0B4CF313`. `FenceArchAdjacent(vértice)`
= o grafo `+0x58` tem algo no vértice (vtable `0x24`).

### 7.20 A área editável do lote (rodada 12)

`cWorldDB::IsOnLot(tile/vertex/location)` (BV `0xd1538…`) compara com os
limites que `SetLotBounds(x0, y0, x1, y1)` guarda (`+0x24…+0x34`, mais os
níveis); quem chama `SetLotBounds` vai pela vtable (não achado). O lote do
bairro é o tamanho mais uma faixa de rua de um tile do bairro (10 tiles do
lote) em cada borda com rua (`emptyLotRect`, `docs/neighborhood-mac.md`); nos
lotes salvos essa faixa é calçada + rua + calçada (ex.: `N001_Lot38`, linhas
20–29). Aqui: uma borda cujos 10 tiles de fora têm piso "road_*" sai da área
editável (inferido dos dados), e paredes/cercas (`0x45`), pisos, fundações,
piscinas, terreno e escadas ficam dentro dela. Os objetos (modo compra) não
foram mudados.

**Rodada 13 (binário + dados).** Os limites vêm do **save**: o
`cWorldDB::Load` (BV `0xd7758`) lê o recurso `0x49FF7D76` (o "cWorldDB" do
lote, que o opensims2 listava como sem uso) e chama `SetLotBounds` (vtable
`0x4c`, BV `0xd65e4`) com os valores lidos. Formato (`cWorldDB::Write`, BV
`0xd91c8`; o base grava a versão 3, o BV a 5): 64 bytes zerados, u32 TGI
tipo (`0x49FF7D76`), u32 versão, string (u8 tamanho + "cWorldDB"), depois
s32 x0, s32 y0, s32 nível mínimo, s32 x1, s32 y1, s32 nível máximo, u8
flag (≠ 0 → segue uma string, só no BV). Exemplos de N001: lote 38 →
(0, 0, 0)–(29, 29, 2); lote 11 → (0, 0, 0)–(39, 29, 4); lote 16 → (0, 0,
0)–(29, 19, 3). Ou seja, **os limites são o mundo inteiro do lote,
inclusive a faixa de rua** (no lote 38 as linhas 20–29 são a rua e estão
dentro).

- `IsOnLot(tile)` (BV `0xd1604`): x0 ≤ x ≤ x1, y0 ≤ y ≤ y1, nível no
  intervalo (inclusivo).
- `IsInBounds(tile)` (BV `0xd1a28`, vtable `0x108` da `cIWorldDB`, a
  interface que `nTSWorld::WorldDB()` devolve): **estrito** em x e y (x0 <
  x < x1, y0 < y < y1), inclusivo no nível: tira o anel de tiles da borda
  do mundo. É esse o teste do `0x45` de paredes, cercas e arcos.
- **Atenção às vtables:** `WorldDB()` devolve o subobjeto `cIWorldDB` (o de
  deslocamento −28 da `cWorldDB`, vptr `0x27a3420`), não o primário; os
  slots desta seção e do §7.19.1 são dessa interface (`0x94`
  `PartitionManager`, `0xc4` `HasFloorTest(tile, nível, bool, bool)`, `0xe4`
  `NumObjectsOnTile`, `0xf8` `IsOnLot`, `0x108` `IsInBounds`, `0x14c`/`0x150`
  `Max`/`MinTerrainElevation`). O "travados (`WorldDB` vtable `0xc4`)" do
  §7 (paredes) foi lido no vtable primário; nessa interface o slot `0xc4` é o
  `HasFloorTest` (a conferir).
- O que tira a faixa de rua da construção **não foi achado** (os limites a
  incluem; candidatos: o `HasFloorTest` com o piso de rua, os tiles
  travados do `cWorldDB` (`IsLocked`/`SetLockedImpl`, BV `0xd2a90`/
  `0xd3424`), ou as bordas com rua do lot info). A regra inferida da
  rodada 12 (10 tiles com piso "road_*") continua inferida.
- No código (rodada 13): a área editável é `vm::lotEditableArea(lot)`
  (`World::editable`), a mesma para as ferramentas de construção e para o
  **modo compra**: `checkPlacement` recusa um tile fora dela com `0x02` "Local
  fora dos limites" (o código do `IsOnLot` falso no `TestPlacementCommon`;
  inferido que o jogo recusa a faixa de rua com ele também).

---

## 8. Base vs BV

Base (strings no Mac 1.0): `ObjectThumbnails.package`, `UIObjectThumbnails`,
`showCatalogID`, `moveObjects`. As tabelas de rótulos e os textos de erro estão
nos dados base. Só BV: modo atacado/negócios (OFB: `nTSBusiness`, od 105/106),
coleções, praias, lagoas, estufas, toldos, saunas e portões de garagem, a
máscara de EP no campo 105, as notas 6/7 do catálogo e os seletores
diagonal/alinhado (campos 21–24, `SwapDiagonalObject`). Comunitário: os botões
existem no script base. O campo 100 é lido no BV, sem conferência no base.

Construção (constscan e strings no base 1.0): meias-paredes, estufas,
paredes de palco/invisíveis, espaçamento de postes, toldos (CLSID
`0x303AB8DF`), os cômodos diagonal e octogonal (teclas `0x714798EE` etc.) e o
modo de quarto de tile (`0xB1265EF3`) são só BV. Existem no base: a ferramenta
de parede e a de cômodo, o recorte `_%s_%s_%d_%s_%s` + `wallMask`, os
undoables de parede, elevação e piso (`0xEC167825`, `0x6C167887`,
`0x4C16787B`), a rotação de piso (`0xACB3A629`), os arcos de cerca e os
scripts de catálogo.

## 9. Para implementar (ordem sugerida)

1. Catálogo: produtos de todos os OBJDs líderes com campo 39 ≠ 0, nome e
   descrição do CTSS (grupo, campo 41), preço = campo 18, ordem por preço,
   filtros §1.3 e botões §1.4.
2. Ferramenta: criar o objeto ao escolher, seguir o cursor com passo de
   tile, girar em passos de 2 (90°), testar com §3.2–3.4 (começar por 1, 2,
   6, 8, paredes e pegadas), slots (§3.1 passo 4), e cobrar ao soltar.
3. Vender = reembolso do od 41 (metade se quebrado). Desfazer guardando o
   objeto fora do mundo até sair do modo.
4. Depreciação diária no `DayPassed` (§2).
5. Construção: parede em linha de 8 direções a partir de vértices (§7.2), 70
   por trecho, apagar com Ctrl a 56 por trecho vendendo o que está preso;
   cômodo com Shift; testes `0x4C`, `0x4B`, `0x0C`, `0x1B`; terraplanagem sob
   a parede. Depois pisos (§7.4: retângulo, sala com Shift, 0,25 por quarto,
   aplainar ao soltar), revestimentos (§7.5: face, flood com Shift), salas
   (§7.3) e recortes (§7.6).

## 10. Aberto

- O papel do campo 34 "sale price" (35 em todos): no BV, a única chamada
  `GetField(0x22)` achada (`ArgCall`, `li r4,0x22` + vtable `0x10`) é de outra
  classe (`FUN_00e998bc`, cópia de propriedades); o caminho de compra/venda não
  o lê. Provavelmente não usado.
- Mapa exato B7/B9 (Diversos, Hobbies "recreação") e os IDs dos sons.
- Conferir `TestPlacementCommon` no base 1.0 (sem símbolos).
- O que o teste de altura od 4 compara (máscara do serviço `0xBA4DA53`, vtable `0x2c`).
- Construção: o fechamento do cômodo retangular (`FUN_009a7dd8`); o corpo de
  `HasSupport`/`SupportedUnderneath`, do leveler `FUN_009a5a68` e do "preso
  à parede" (vtable `0x26c`); o custo por
  unidade de altura do terreno (`SetCostForUnitHeightChange`, não decompila).
