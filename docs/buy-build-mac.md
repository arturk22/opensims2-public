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
ponteiros para globais. **Aberto**: a que sons eles apontam.

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

O preço cheio só volta pelo **desfazer** (a mudança gravada sai com o custo).

**Desfazer** (`ToggleChange`, BV `0x30c134`): percorre as mudanças de trás
para frente e troca o estado (`+100`: 1 = aplicado, 2 = desfeito); paredes
voltam com os padrões das duas faces gravados na mudança.

### 7.3 Salas (`cRoomManager`) e o que é refeito

O `cPartitionManager` tem **três** gerenciadores de sala (`+0x40`, um por
`tRoomDefinition` 0, 1, 2; `RoomContaining`/`InsideRoomIDSet` BV `0xc2f920`/
`0xc2f988` escolhem pelo índice). Inferido: são os três mapas de sala do lote
(3ARY `0x09`, `0x0A`, `0x0B`, o que explica os "outros IDs por quarto de tile"
do TODO). Cada sala é um ID `short` por **quarto de tile** (4 triângulos por
tile; o `cFloorTile` leva a máscara dos quartos: 1, 2, 4, 8; uma parede
diagonal separa os pares 3/12 ou 6/9). `RoomContaining(tile)` (vtable `0x38`,
BV `0xc284b8`) devolve o ID do primeiro quarto da máscara (0xFFFF fora do
mapa). "Dentro" (`InsideRoomIDSet`, vtable `0x44`, BV `0xc28ce0`) = **todos
os IDs ≠ 0 e ≠ −1** que existem no mapa; a sala 0 é a de fora.

Recalcular (nomes do `baserom.sym`; o corpo não foi lido em detalhe):
`AddSeed(cWallLocator, sala)` semeia os quartos dos dois lados de cada parede
mudada, `FloodRoomFromSeed`/`FloodRoomRecurse` inundam os quartos ligados sem
cruzar partições que dividem salas, `RefreshMap` refaz os IDs e
`FlushRoomChangedNotifications` (BV `0xc28638`) envia uma mensagem por sala
mudada: `0x2B4D18FB` (definição 0), `0xAC2A4F4E` (1), `0x2B4D18F6` (2).

`cTSUserToolUndoableWall::UpdateRooms` (BV `0x30bec8`, chamado no
`CommitSelf`) sempre pede o recálculo ao gerenciador de partições (vtable
`0x98`). Depois avisa o mundo (vtable `0x54` do undoable) com **1** se
alguma mudança foi parede (não cerca nem arco) ou meia-parede, e com **2**
se só houve cercas; parede de piscina também atualiza o scenegraph. Ou seja:
**cercas não mudam salas; paredes (e meias-paredes no BV) mudam.**

Quem escuta a mensagem de sala 0 (`FindConst`): `nTSSG::cWallManager`
(render groups por sala), `cFenceManager`, `cFloorManager`, `cObjectModule`,
o gerenciador de light maps (`FUN_006f3d30`, perto de
`cLightMapManager`) e outros dois do scenegraph. A de definição 2 vai para o
`cEdithObjectModule` (sala dos objetos para a VM). O roteamento escuta as
mensagens de mudança do mundo no `cTSRouteManager::DoMessage` (BV
`0x4f8038`; IDs `0x0B4CF313`, `0x2B4CF319`, `0x2B4CF327`, `0x2B4CF334`,
`0x0B537176`, `0x2B4D1900`…) e refaz os retângulos de parede
(`AddWallRectangle`/`RemoveWallRectangles`, `docs/routing-mac.md` §2).

Para implementar: depois de cada operação de parede, refazer o mapa de salas
(flood fill por quarto de tile, paredes como barreiras, cercas não), as
paredes do roteamento e os light maps das salas afetadas.

### 7.4 Pisos (`cTSUserToolFloorPattern`, BV vtable `0x27b1648`)

- **Modos** (`+0x84`/`+0x88`, por `StateChange` `0x2ffe3c`): 0 pintar
  retângulo, 1 apagar retângulo (Ctrl), 2 encher a sala (Shift), 3 apagar a
  sala (Ctrl+Shift). Apagar usa o "sem piso" do `WorldDB` (vtable `0x98`).
- **Atualização** (BV `0x300b2c`): com início (`+0x64`) e cursor (`+0x74`)
  diferentes, cada tile do retângulo recebe `UserSetFloorPattern` (BV
  `0x9a4e80`) com a máscara 0xF (tile inteiro); num clique sem arrastar, o
  tile do cursor com a máscara de quartos que veio do ponto do mouse
  (`+0x80`). Encher a sala = `UserSetFloorPatternRoom` (BV `0x9a4f90`).
- **Girar** o padrão: tecla `0xACB3A629` (`,`/`.`) cicla o modo de rotação do
  `cTSMainToolbar` 1 → 2 → 4 → 8. A tecla `0xB1265EF3` (alterna o modo de
  quarto de tile no sim system, vtable `0xcc`/`0xdc`) é **só BV**.
- **Ao soltar** (BV `0x301324`): se pintou (modo 0 ou 2), cria um undoable de
  elevação como pré-operação e **aplaina o terreno** sob o retângulo (a altura
  do tile inicial, `LevelRect`) ou sob a sala; submete e toca o som.
- **Regras** (`UserMayPlaceFloorPattern`, BV `0x9a46cc`): área travada/fora do
  lote → `0x51`; água → `0x57`; praia (BV) → `0xA4`; terreno não plano no
  térreo (salvo `UnevenTilesFloorable` do `WorldDB`) → `0x53`; tile não
  "floorable" (no andar de cima, sem parede/apoio) → `0x52`; objeto no tile
  sem a flag od `0x2A` bit 1 → `0x55`.
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
   base = nome "objectCutout" da extensão de dados do CRES do objeto
   dx = "%dE"/"%dW"/"0", dy = "%dN"/"%dS"/"0" (deslocamento do trecho em relação ao objeto)
   dir ∈ S W N E NW SE SW NE   (o lado/diagonal do trecho)
```
Ex. nos dados: `windowdesigner_1e_0_1_n_wallmask_txmt`,
`doorclub2diagonal_1e_1n_0_nw_wallmask_txmt`. `SetWallCutout` passa o nome
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
- Construção: o corpo de `RefreshMap`/`AddSeed` (quais partições dividem cada
  uma das três definições de sala, e como as salas de pátio viram a sala 0);
  o fechamento do cômodo retangular (`FUN_009a7dd8`); como o ponto do mouse
  vira a máscara de quartos do piso (`FUN_003000cc`) no base; o custo por
  unidade de altura do terreno (`SetCostForUnitHeightChange`, não decompila).
