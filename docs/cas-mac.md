# Create-a-Sim (CAS): o que o binário de Mac faz

Fontes e convenções como em `docs/shaders-mac.md` e `docs/buy-build-mac.md`:
endereços do BV 1.10 ppc (`/mac/sims2_bv.ppc`), nomes do symxfer conferidos no
código ou, quando o BV não tem nome, casados com o `baserom.sym` (o outro build
ppc com símbolos) pelo tamanho e pela ordem das funções. "dados" = os arquivos
do jogo base (`data/game`). "só BV" = não existe no jogo base (animais, traços,
EPs). Nada disto existe ainda em `src/` (há só a leitura do personagem pronto,
`formats/sim_character`).

## 1. Fluxo e telas

Textos em `TSData/Res/Text/CAS.package` (STR# grupo `0xFFFFFFFF`):
- 0x80: Create Family / Create A Sim / Choose Existing Sim / Make A Child /
  Family Relationships / Exit.
- 0x81, os passos de criar um Sim: **1** nome, idade, sexo, tom de pele,
  forma física; **2** cabeças (arquétipos); **3** cabelo e rosto (os
  modificadores por região); **4** maquiagem, barba/sobrancelhas e óculos;
  **5** roupas; **6** personalidade e aspiração.
- 0x85 sexo/idade, 0x87 cores de cabelo (Black, Brown, Blonde, Red, ...),
  0x88 categorias de acessórios (Makeup, Jewelry, Glasses, Facial Hair &
  Eyebrows, Costume Makeup, Lipstick, ...), 0x89 roupas (Tops, Bottoms,
  Outfits; Everyday, Formal, ...), 0x8A signos (Aries…Pisces), 0x8B
  aspirações, 0x8C confirmações e erros da família, 0x8D títulos dos passos
  ("Change Head", "Change Face", "Change Brow", "Change Eyes", "Change Nose", …).

Telas (UI scripts em `ui.package`, grupo `0xA99D8A11`): `CB980E81` passo 1
("Name / Genetics Step 1"), `CB980E82` detalhes do rosto/genética, `CB980E83`
personalidade, `CB980E86` bin de Sims prontos, `CB980E87` "make a baby",
`CB980E88` bin de cabeças, `CB980E92` tom de pele (e os outros `CB980Exx`
sem nome). O código das telas é o `cTSUISimEditor` (BV `0x1b438c`…) mais um
**componente** `nTSUICAS::cTSUICASComponent*` por painel: `Bin`/`SimBin`
(Sims e cabeças prontos), `Archetypes` (cabeças), `Modifiers` (sliders do
rosto), `Overlays` (maquiagem, sobrancelhas, barba, óculos), `Genetics`,
`BabyMaker`, `BodyOptions`, `Hair`, `SkinTone`, `Clothing`, `Accessories`,
`Personality` (só BV: `Traits`, `Pet*`). Cada tela lê constantes de janelas de
texto escondidas no próprio script (`kMaxPersonalityTraitsValue=2500`,
`kZodiacAriesBMP=0xca200451`, `kCAFModifierBinName`, `kCAFArchetypeBinName`,
...; `cTSUIWinHandler::GetPropertySint`).

**Atalhos** (arquivo de teclado do CAS, `ui.package` tipo `0xA2E3D533`
instância `6A9362F4`): Ctrl-Z/Ctrl-Y `0xCD3C5A8F`/`0xCD3C5A97`
(desfazer/refazer do editor: `PushSimState`/`PopSimState` guardam cópias do
estado do Sim), `.`/`,` giram o Sim (`0x4D12E5F6` ±1 ao apertar,
`0xAD12E657` ao soltar), `C` foto.

**Validar a família** (`cTSUISimEditor::ValidateHouseHold`, BV `0x1be9a8`):
o nome da família não pode ficar vazio (com o pedido de validar e só
crianças, o jogo põe um nome padrão do STR# `0x7F3A262A`/0xD7 #11); ≥ 1
membro; precisa de pelo menos um Sim de índice de idade adulto ou
jovem-adulto (índices 3/7, abaixo) quando há membros que não são
adolescente/adulto/idoso → texto STR# 0x8C #10 ("Please add an Adult or
Elder to your Family."); todo Sim com nome (#9). O limite de 8 membros não
foi achado nesta função.

## 2. Idade, sexo, forma física e tom de pele

- **Bits de idade** (AGED/GZPS `age`): `0x20` bebê, `0x01` criança pequena,
  `0x02` criança, `0x04` adolescente, `0x08` adulto, `0x10` idoso, `0x40`
  jovem adulto (só BV). O índice de idade do editor (`ProcessSaveSimRequest`,
  BV `0x55d2f8`): 1→0, 2→1, 4→2, 8→3, 0x10→4, 0x20→5, 0x40→7.
- **Person data gravados ao criar** (`ProcessCreateSimRequest`, BV
  `0x561cac`, estado 1): 58 "Person Age" = bebê 1, criança pequena 2,
  criança 3, adolescente 16, adulto 19, jovem adulto 19 (e o dado 0xAF = 1),
  idoso 51; 65 "Gender" = 1 se mulher; 0xBA = espécie (BV).
- **Forma física** (person data 82 "Fatness [0-1000]" e 81 "Body Flags"):
  normal = 500/0, magro = 0/0x10, gordo = 1000/1 (criação aleatória, estado 2).
  `SetFitnessLevel` (BV `0x1b5b24`): dado 82 = valor, dado 81 = 1 se ≥ 502
  (`0x1F6`), e troca as roupas para a versão gorda (`fitness` 2 do GZPS)
  ou normal (0).
- **Tom de pele**: `skintone` do GZPS/`skincolor` do AGED = família do
  catálogo de pele, `00000001-…` Light, `…02` Medium, `…03` Medium-Dark,
  `…04` Dark (os IDs `sN` de 1 a 4; ver §5). Criação aleatória: 1–4 do
  pedido ou um sorteio. A família **Alien** (`6baf064a-85ad-4e37-8d81-a987e9f8da46`,
  genetic 0) usa as texturas `s5` (visto nos dados: os GZPS `*_alien` do
  `Skins`, como `amtopnaked_alien`, apontam para `amtopnaked_nude_s5_txmt`;
  existem `<p>bodynaked-nude-s5`, `amface_s5_txmt` etc.). Rodada 12: o
  `readCharacter` lia o `skincolor` como número hexadecimal (o alienígena
  virava `s1807680074`) e a pele por baixo das roupas saía preta/branca nos
  braços do 9 de Souza (Pollination) e do João (Johnny); agora
  `cas::skinToneNumber` dá 1–4 para `0000000N`, 5 para o Alien e 0 para os
  manequins (`r12-alien-pele-s5.jpg`: antes/depois; `r12-aniversario-johnny-alien.jpg`: o Johnny adulto na cena do aniversário, repro do `docs/cinematics-mac.md` §5).

## 3. Rosto: arquétipos, modificadores, regiões

Dados (`TSData/Res/Catalog/Skins/Skins.package`, XMLs `cGZPropertySetString`;
os tipos são os `cTS*Entry` do `nTSCASCore::cTSSkinCatalog`):

| tipo | `type` | qtd. base | o que é |
|---|---|---|---|
| `0x8C93E35C` | `facearchetype` | 210 | cabeça pronta (`name` "tfarchbase_base", `shapekeyidx` → malha/morph) |
| `0x0C93E3DE` | `facemodifier` | 143 | um slider: `name` ("face gaunt-plump"), `negativelimit`/`positivelimit` (±1,5), `negativescale`/`positivescale`, `groupindex`, `compareage`/`comparegender`, imagens |
| `0x8C93BF6C` | `faceregion` | 34 | máscara de região (`name` = nose, brow, mouth, eyes, jaw, ears, cheek, ...) |
| `0x6C93B566` | `faceneutral` | 9 | rosto neutro por idade/sexo ("puFace", …) |
| `0x2C1FD8A1` | `textureoverlay` | 149 | textura por cima do rosto: `subtype` 0–7 (3 = olhos; sobrancelhas, maquiagem, batom, …), `layer`, `materialkeyidx`, `genetic` |
| `0x0C1FE246` | `meshoverlay` | 107 | malha por cima (barba: `override0subset` "beard"; óculos) |
| `0x4C158081` | `skintone` | 7 | tom de pele, `genetic` = valor genético |
| `0x8C1580B5` | `hairtone` | 6 | cor de cabelo, `genetic` |

As malhas de rosto (neutros, arquétipos e os deltas dos modificadores) estão
em `TSData/Res/Sims3D/CASFace.package` (685 GMDC/CRES/SHPE). O editor
(`cTSUICASComponentArchetypes`: `MorphBlend`, `BlendDeltas`,
`MorphCacheFrom/Target` no `baserom.sym`) troca de arquétipo interpolando os
deltas; os modificadores somam deltas com peso no intervalo do slider. O
resultado é exportado (`cTSCASSystem::ExportFace`, BV `0x558e4c`) como a
malha de rosto do personagem + o **LxNR** (`0xCCCEF852`, instância 1, a
"estrutura facial", §13).

## 4. Overlays, maquiagem, barba: o TXMT do rosto

O rosto do personagem tem um TXMT `SimSkin` com a lista de texturas a compor
e as escolhas do CAS (visto nos dados):

```
baseTexture0 = efface-s3            (pele do rosto; s3 = tom 3)
baseTexture1 = uuface-eye-dkbrown   (olhos)
baseTexture2 = uuface-lip-terracotta
baseTexture3 = uuface-makeup2
baseTexture4 = uuface-eyelinerlight
baseTexture5 = uuface-browtweased-grey
numTexturesToComposite = 6
compositeBaseTextureName = efface_s3_uuface_eye_dkbrown_..._uuface_browtweased_grey-diffuse
cafSkinTone  = 00000003-0000-0000-0000-000000000000
cafHairTone  = 00000005-0000-0000-0000-000000000000
cafNumOverlays = 5
cafOverlay0..4 = <família do textureoverlay>,   (GUID do `family` do XML; o 1 = a cor dos olhos)
```

Ou seja, `cafOverlayN` são as famílias dos `textureoverlay` escolhidos, na
ordem das camadas (`layer` do XML), e o jogo compõe a pele do rosto com elas
numa textura só (`compositeBaseTextureName`). Barba e óculos são
`meshoverlay` (malha própria). `cTSCASSystem::ApplyOutfitOverlay` (BV
`0x565c14`) aplica um overlay a uma roupa.

## 5. Genética (`nTSGenetics`)

Cada Sim tem um DNA (`cTSSimDNA`, BV `0x5464f8`; property set guardado por
neighbor ID no `cTSGeneticsManager`, BV `0x5462f4`, e salvo na tabela
`cTSNeighborGeneticDataTable` do bairro). O DNA é uma lista de **alelos por
tipo**, cada alelo = um par de strings (dominante, recessivo). Tipos
(pelo uso no `PickGene`/`DetermineGenetics`):

| tipo | o que é |
|---|---|
| 1 | cor do cabelo (família do `hairtone`) |
| 2 | tom de pele (família do `skintone`) |
| 3 | cor dos olhos (família do `textureoverlay` de olhos) |
| 5 | regiões do rosto dominantes (string com os nomes) |
| 6 | auxiliar da pele |
| 4, 0xB | não sorteados |
| 7–10, 0xC–0xE | sorteados 50/50 (sem uso conhecido no base) |
| 0x400+ | camadas de pelagem (só BV, animais) |

**Sim novo (não bebê)**: o DNA vem da roupa (`SetFromOutfitData`, flags
`0xD`: as próprias cores nos dois lados) e as **regiões dominantes** são
sorteadas (`SetupDominantRegions`, BV `0x54ade8`): para cada região de
{nose, brow, mouth, cheek, cheekbones, eyes, jaw, ears}, ela entra na lista
se `rand(100) ≥ limite`, com limites (tabela em BSS iniciada em
`0x200131c`, `kDominantRegionMasks`):

| região | mulher (dado 65 = 1) | homem |
|---|---|---|
| nose | 50 | 66 |
| brow | 50 | 66 |
| mouth | 66 | 50 |
| cheek | 50 | 50 |
| cheekbones | 66 | 50 |
| eyes | 66 | 50 |
| jaw | 50 | 66 |
| ears | 50 | 50 |

**Bebê** (`cTSCASSystem::ProcessCreateBabyRequest`, BV `0x55a8f4`; pedido
do Make New Character, `docs/primitives-mac.md` §4c):
1. carrega os LxNR dos dois pais;
2. idade bebê (`0x20`), dado 58 = 1, gordura 500, flags 0; **sexo**: com um
   pai só (mesmo ID), o do pai; senão 50% (`rand ≤ 0,5`);
3. `DetermineGenetics` (BV `0x548d68`): para cada tipo 1–15 que algum pai
   tem, **`PickGene`** (BV `0x5470e0`):

```
alelo de cada pai = dominante ou recessivo, 50/50   (a, b)
valor(x) = "genetic" do catálogo
cabelo (1) e olhos (3): o de MENOR genetic fica dominante (Black/Brown 1 > Blond/Red 2;
     olhos dkbrown/dkblue/alien 1 > gray/green/ltblue 2); empate = ordem sorteada
pele (2): se os dois têm genetic ≠ 0, sorteia um tom com genetic entre os dos pais
     (Light 0,1, Medium 0,3, Medium-Dark 0,6, Dark 0,9) e grava no tipo 6; os
     alelos ficam (a, b); com genetic 0 (manequim, alienígena) entra a regra de
     igualdade/50%
outros: ordem 50/50
```
   O que aparece é o **dominante** (`GetExpressedHairColor`, BV `0x493454`).
4. **rosto** (`DetermineDescendantFace`, BV `0x548404`): para cada região de
   {nose, brow, mouth, eyes, jaw, ears} escolhe o rosto de um dos pais e
   monta um script `"<região>,<i>"` que o editor (`EdithEditor` vtable
   `0xfc`) usa para misturar os dois rostos por região; "eyes" leva também
   "cheekbones" e "mouth" leva "cheek". A escolha: sorteio 0–99; se a região é
   dominante só no segundo pai, ele ganha com `r ≤ 66`; nos outros casos
   50/50; a região fica dominante no filho se era dominante no pai escolhido.
   (Pela leitura, o caso "dominante só no primeiro pai" fica em 50/50: o
   código não é simétrico.)
5. roupa: uma roupa de bebê sorteada entre as do bin que não têm a flag 1;
   cabelo do bebê pela cor genética;
6. nome padrão do Live.package (`0x7F96C284`, STR# 0x95/0xE6 pela idade; §17.4) +
   sobrenome da família; entra no neighbor manager.

## 6. Personalidade, signo e aspiração

- **Pontos**: 5 traços em person data, 0–1000 cada (**100 por ponto**),
  `kMaxPersonalityTraitValue=1000`, total `kMaxPersonalityTraitsValue=2500`
  (25 pontos). `UpdatePersonality` (BV `0x44c5a8`) recusa passar do total
  (tira o excesso do traço que mudou).
- **Signo** (`ComputeZodiacSign`, BV `0x8cbf90`): vetor do Sim = (Outgoing
  [dado 6], Nice [2], Playful [5], Neat [7], Active [3]) × 0,01; o signo é o
  de **menor distância euclidiana ao quadrado** entre 12 vetores (tabela BV
  `0x277a68c`), 1 = Aries se nenhum. Escolher um signo (`SetZodiacSign`, BV
  `0x8cc184`) grava os pontos dele × 100. Person data 70 "zodiac" = 1–12.

| signo | Outgoing | Nice | Playful | Neat | Active | atrai (BV `0x277a62c`) | repele (`0x277a5cc`) | aspiração sugerida |
|---|---|---|---|---|---|---|---|---|
| 1 Aries | 8 | 3 | 3 | 5 | 6 | 3, 2 | 4, 7 | 0x10 |
| 2 Taurus | 5 | 4 | 8 | 5 | 3 | 1, 7 | 6, 4 | 0x01 |
| 3 Gemini | 7 | 3 | 3 | 4 | 8 | 12, 6 | 10, 1 | 0x04 |
| 4 Cancer | 3 | 6 | 4 | 6 | 6 | 2, 8 | 3, 1 | 0x02 |
| 5 Leo | 10 | 3 | 4 | 4 | 4 | 9, 4 | 10, 3 | 0x10 |
| 6 Virgo | 2 | 5 | 3 | 9 | 6 | 11, 9 | 5, 2 | 0x20 |
| 7 Libra | 8 | 7 | 6 | 2 | 2 | 6, 4 | 12, 8 | 0x01 |
| 8 Scorpio | 5 | 3 | 3 | 6 | 8 | 12, 5 | 7, 11 | 0x04 |
| 9 Sagittarius | 3 | 4 | 7 | 2 | 9 | 12, 10 | 7, 8 | 0x20 |
| 10 Capricorn | 4 | 5 | 8 | 7 | 1 | 11, 2 | 5, 3 | 0x04 |
| 11 Aquarius | 4 | 6 | 7 | 4 | 4 | 10, 9 | 8, 6 | 0x02 |
| 12 Pisces | 3 | 7 | 3 | 5 | 7 | 8, 3 | 5, 1 | 0x20 |

  (Atrai/repele = `GetCompatibleSigns`/`GetIncompatibleSigns`, BV
  `0x8cc79c`/`0x8cc710`; "aspiração sugerida" = `ComputeAspirations`, BV
  `0x8cc150`; o uso dela não foi lido. Os pares de compatibilidade
  conferem com o CAS: Aries atrai Gemini e Taurus e repele Cancer e Libra.)
- **Aspiração** (person data 46): botões `0x0CDC000k` do `CB980E83` →
  `1 << (k − 1)`: Romance 0x01, Family 0x02, Fortune 0x04, Reputation 0x10,
  Knowledge 0x20, Grow Up 0x40. Só adolescente, adulto, jovem adulto e idoso
  escolhem (`HandleButtonAspirations`, BV `0x44a954`, índices 2, 3, 7, 4);
  os mais novos ficam com Grow Up.

## 7. Criar o personagem e gravar o arquivo

**Make New Character / Sim novo** (`cTSCASSystem::CreateNewCharacter`, BV
`0x562f7c`): cria um vizinho a partir de um **modelo** (GUID do pedido, ou o
padrão da espécie: humano `0x7FD96B54`; `0x0F83C946`/`0x111D8FFF`/`0x911D900A`
são as outras espécies, BV) e um `cCreateSimRequest`. O pedido é uma máquina
de estados que **avança um estado por tick** (`OnTick`, BV `0x563988`) — é
isso que o Make New Character espera (`docs/primitives-mac.md`, a primitiva
devolve 2 enquanto algum pedido não acabou). Com `bSync` o laço roda tudo de
uma vez e já salva.

`ProcessCreateSimRequest` (BV `0x561cac`):

| estado | faz |
|---|---|
| 0 | cria o property set AGED (`0xAC598EAC`, instância 1, grupo do personagem) |
| 1 | espécie; idade/sexo aleatórios se −1 (por um sorteio do catálogo); person data 58 (idade), 65 (sexo) |
| 2 | monta a roupa: partes iguais de outros pedidos com a flag 8; tom de pele 1–4 do pedido ou sorteado; forma física → dados 81/82 |
| 3 | humano: o DNA a partir da roupa (`SetFromOutfitData`, flags `0xD`) |
| 4 | grava o property set no personagem e o registra |
| 5 | com a flag 4 do pedido: confere o sexo e roda o "random clothing" do CAS; nome = primeiro nome + " " + sobrenome |
| 6 | entrega ao neighbor manager (`AddNewCharacter`) |
| 7 | pronto (`+0x18` = 1) |

**Gravar o arquivo do personagem** (`ProcessSaveSimRequest`, BV `0x55d2f8`;
o "segmento" `0x656B8F0C` = o pacote `Characters/<bairro>_User#####.package`):

1. Lê/cria o AGED; calcula o índice de idade e o sexo; manda o editor
   exportar o rosto (`EdithEditor` vtables `0x34`, `0x20`, `0x1c`, `0x14`): a
   malha (GMDC/GMND/SHPE/CRES do rosto, com nome `#0x7F861BC1!age3_0_cres`
   etc.), o **LxNR** (`0xCCCEF852`, instância 1) e o TXMT composto (§4).
   Grava os campos 0x1FFF do personagem.
2. **Apaga** do pacote os recursos antigos destes tipos antes de regravar:
   GZPS `0xEBCF3E27`, AGED `0xAC598EAC`, CRES `0xE519C933`, `0xFC700D40`,
   GMND `0x7BA3838C`, SHPE `0xFC6EB1F7`, `0x65245517`, TXMT `0x49596978`,
   TXTR `0x1C4A276C`, 3IDR `0xAC506764`, IMG `0x856DDBAC`.
3. Junta os GZPS da idade/sexo e grava as roupas por categoria (13 bits de
   categoria × 4 combinações de flags; a categoria `0xC80` fica de fora).
4. Grava o AGED (vtable `0x3c`) e fecha o pacote (`0xb0`).
5. Atualiza o vizinho/genética.

O que fica num personagem do base (`N001_User00001`): AGED, LxNR, 2 GZPS de
rosto (`type` "skin": um para a idade atual e um para a próxima, ex. adulto
8 e idoso 16; `hairtone`, `skintone`, `resourcekeyidx`/`shapekeyidx` no
3IDR), 2 3IDR, CRES/SHPE/GMND/GMDC do rosto (14 GMDC), 2 TXMT, IMG da
miniatura (`0x8` e `0x800`), STR#/CTSS e o "objeto do Sim" (OBJD/OBJf/
GLOB/SLOT/BHAV/TREE/TPRP do modelo). O AGED guarda `age`, `gender`,
`species`, `fitness`, `skincolor`, `haircolor`, `eyecolor` (família do
overlay de olhos), `skeletonkeyidx`, `listcnt`/`lk<i>`/`le<i>_0`/`ls<i>`
(roupas, como já descrito no TODO), `validparts`, `stretch`, `activecat`.

### 7.1 Rodada 16: os NPCs feitos em jogo (arquivo de personagem)

Os Sims que o Make New Character faz de um **modelo** (sem a flag 0x01)
não tinham arquivo de personagem, então o app não os desenhava ("Sim 238
(neighbor 363): no character file, not drawn": jornaleiro, carteiro,
empregada, motorista do ônibus, coelho do apoio social). No base todos vêm
com a flag `0xC8` (modelo nos temps 0/1, roupa nos temps 2/3) de "Generate
- Make Character" (`7F8CE369`/`0x100E`) e "NPC - Make New Character -
Specific" (`7FB55274`/`0x1010`). O GUID da roupa é um **objeto de roupa**
do `objects.package` (**[dados]**): `0x6CC1322B` "Outfit - NPC -
PaperDelivery" (grupo `7FA8111B`), `0xECD0F3FC` "Outfit_NPC_Mail Outfit -
Template" (`7F76094A`), `0x6DC38A6C` "Outfit_NPC_Maid_test" (`7FE81E93`),
`0x8CC13127` "Outfit - NPC - BusDriver" (`7FC4E4A7`), `0xAE028CB9`
"Outfit_SocialBunny_Yellow" (`7FC16D15`). O grupo de cada um guarda um
**AGED + 3IDR por idade e sexo** (instâncias 1, 2, ...), no mesmo formato do
AGED de um personagem: as listas de roupa apontam os GZPS de uniforme do
`Skins.package` (flag 1, os que o CAS não oferece: `afbodydressmaid`,
`afhairhatmaid_*`, ...).

O opensims2 (`vm/baby_character.cpp`, `npcCharacterWriter`) escreve o
arquivo como o `ProcessCreateSimRequest` monta o Sim: idade, sexo e tom de
pele sorteados pela primitiva; o resto ao acaso como o botão aleatório do
CAS (arquétipo de rosto, cor do cabelo, olhos, cabelo, sobrancelhas e
roupas: **[inferido]**, o `RandomizeForHumans` do binário não foi seguido
passo a passo), semeado pelo GUID do vizinho (os números aleatórios da VM
ficam como estavam); depois, de um AGED do objeto de roupa que sirva à
idade e ao sexo (um ao acaso quando vários servem), as partes de cada
categoria que ele lista substituem as sorteadas (um corpo inteiro tira a
parte de cima e a de baixo e vice-versa; **[inferido]**). O objeto do Sim
no arquivo é o do **modelo do NPC** (OBJD com o GUID novo, BHAVs e o resto
do grupo dele), como o `AddNewCharacter` faz; os do personagem comum
seguem vindo do TemplatePerson. O arquivo vai para `NPCs/` na pasta de
personagens da sessão; o salvar copia os `<P>_User` de lá para o bairro
junto com os vizinhos (§7.2). Captura:
`docs/screenshots/r16-npc-carteiro.jpg` (`--lot N001_Lot11 --run-vm 31500
--track-sim-id 151 --zoom 5`: o carteiro de uniforme, boné e bolsa).

### 7.2 Rodada 16 (f): nomes dos NPCs e o que o salvar grava

**Nomes** **[binário]** (`cTSCASSystem::PopulateSimNames`, BV `0x559c68`,
chamado pelo pedido do CAS): escreve os nomes no CTSS do personagem novo
(instância = campo 0x29 do OBJD). Se o modelo tem o campo **0x30 "No New
Name from Template"** (rótulo do editor, STR# `0x7FE59FD0`/`0xCC`), só
esvazia a string 2 (sobrenome) em todas as línguas: o nome é o do CTSS do
modelo. No base isso vale para o coelho do apoio social ("Social Bunny"),
o cobrador ("Repo Man") e a assistente social ("Social Worker") **[dados]**
— o que os bairros do jogo mostram ("Coelho do Apoio Social", "Cobrador",
"Assistente Social"). Os outros sorteiam um índice (gerador do próprio CAS,
`this+0x88`, não o do simulador) e escrevem, nas 44 línguas, o nome do STR#
do `Live.package` (grupo `0x7F96C284`) **0x96** "Random Male Sim Names" ou
**0x97** "Random Female Sim Names" (pelo dado de pessoa 65) e, com outro
sorteio, o sobrenome da **0x98** "Random Family Names". O BV tira o
sobrenome de uma mulher da `0xCC` (línguas com sobrenome feminino), que o
`Live.package` do base não tem: aqui a 0x98 para os dois **[inferido]**, como
mostram as mulheres dos bairros do base ("Lúcia Hanbi"). A tabela pt-BR tem
58 sobrenomes vazios de 201 (e nomes vazios): um NPC pode sair só com o
nome, como no jogo. O BV também troca as tabelas para os bichos (0xEA–0xED,
campo 0xCB): fora do base. No opensims2: `nameNewSim` em `vm/characters.cpp`
(sem as tabelas, nos testes, o título do modelo).

**Salvar** **[binário]**: `cTSNeighborManager::AddNewCharacter` (BV
`0x5386dc`) cria o arquivo `<P>_User%05d` na hora (o primeiro número livre
a partir do contador do gerenciador, até 32000) e o vizinho; o
`cTSNeighborManager::Save`/`WriteNeighbors` (BV `0x53a9e8`/`0x53a76c`) grava
**todos** os vizinhos (os sujos, ou todos num save completo), novos também.
No opensims2 (`vm/hood_save.cpp`): todo vizinho que o pacote não tem e que
tem GUID ganha SDSC (versão 0x20), SDNA (de um NPC: o DNA do visual sorteado,
`genetics::dnaFromLook`, como o CAS dá a um Sim novo que não é bebê), entra
nos membros do FAMI da família dele (os NPCs na `0x7FFF`, pelo Generic Sims
Call 4 das árvores) e, se é bebê, na árvore da família. O arquivo vai da
pasta `NPCs/` da sessão para `Characters` (`copySessionCharacters`,
`app/game_save.cpp`), com o nome atual do vizinho no CTSS
(`cas::setCharacterNames`: o recém-nascido que o jogador nomeou). O OBJD
que o Make New Character fez em memória passa a ter o nome do arquivo
("N001_User00171 - Bebezinha", `CodeLibrary::renameDefinition`), porque o
cPerson do lote salvo guarda esse nome e o recarregar acha o arquivo por
ele (antes o bebê voltava como a mãe, de quem o OBJD era cópia). Conferido:
`s2pkg play-loop` (abaixo) e o app sem tela (`docs/game-flow-mac.md` §3).

**Os NPCs únicos** **[dados]**: vizinhos sem arquivo (`s2pkg hood`: "no
character file") cujo OBJD é um "NPC - Unique" do `objects.package` guardam
o personagem **no grupo do objeto**: o Ceifador (`0x84EC24A8`, grupo
`7F404283`: AGED binário + 3IDR sobre partes do catálogo, o corpo de todo
dia é `aubodyreaper`, com o colar havaiano; `aubodyreaper_nolei` é o de
outras categorias), o Técnico de Polinização (`0x2E17B9FC`, `7F45284D`: um
personagem inteiro, rosto GZPS/CRES/SHPE/GMND/GMDC, TXMT e LxNR). O app
(`cas::writeEmbeddedCharacter`) copia o grupo para um arquivo de personagem
(grupo `0xFFFFFFFF`, as referências dos 3IDR ao grupo feitas locais) em
`NPCs/Unique_<GUID>.package` e desenha o Sim dele. Captura:
`docs/screenshots/r16-npc-ceifador.jpg` (`--character` com esse arquivo).

## 8. Base vs BV

Base: todo o fluxo humano, os XMLs do `Skins.package`, signos e as seis
aspirações (Grow Up = 0x40 é do base). O jovem adulto (bit de idade `0x40`,
índice 7, dado 0xAF) é do University, logo só BV. Só BV: animais
(`Pet*`, pelagem, alelos 0x400+), traços (`cTSUICASComponentTraits`,
`ComputeTraits`), "EP2 Downtownie Bin", `kCAFArchetypeBinNameEP6`, a espécie
no person data 0xBA.

## 9. Para implementar (ordem sugerida, rodada 9)

1. Catálogo de pele: ler os XMLs do `Skins.package` (tipos acima) por
   idade/sexo.
2. Sim novo: AGED + DNA da roupa + regiões dominantes (§5), dados 58/65/81/82,
   nome; rosto = arquétipo + modificadores (pesos ±1,5) sobre o neutro.
3. Personalidade 25 pontos, signo pela distância (§6), aspiração.
4. Gravar o personagem (§7) com os tipos que o jogo apaga e regrava.
5. Bebê: `PickGene` e `DetermineDescendantFace` (§5); o Make New Character
   pode continuar síncrono enquanto não houver espera visível.

## 9b. Miniaturas do CAS (`CASThumbnails.package`, rodada 9, binário + dados)

`TSData/Res/UserData/Thumbnails/CASThumbnails.package`: 5623 imagens tipo
**`0x0C7E9A76`**, grupo **`0xEC7E9A54`** (o grupo do serviço,
`cTSUIThumbnailService` `+0xAC`; `FixInstanceOnlyCASKey` completa tipo e grupo
de uma chave só com instância). O índice do pacote tem entradas de 20 bytes
(sem instância alta): a instância guardada é a **parte baixa** do u64 que o
jogo calcula, sempre um **CRC-32** (o mesmo `crc32Lower` do
`formats/resindex`: polinômio 0x04C11DB7, início 0xFFFFFFFF, sem xor final,
texto em minúsculas; `FUN_00120ff8`) de um nome montado assim
(`nTSUICAS::Create*ThumbnailInstanceId`, BV `0x983be0`/`0x983d30`/`0x983ea0`):

- prefixo `CreateCASFileName(idade, sexo, espécie)` (`0x983994`): idade
  `toddler`/`child`/`teen`/`youngadult`/`adult`/`elder` + `male`/`female`
  (+ `dog`/`smalldog`/`cat` no BV), colados: `adultfemale`;
- **roupa** (GZPS sem `hairtone`): `"<prefixo>_<grupo>-<tipo>-<instância>"`
  da chave do GZPS, em **decimal com sinal** (`%d`), tipo `0xEBCF3E27`;
  ex. `adultfemale_739751754-...`; a parte alta do u64 é a instância do GZPS;
- **cabelo** (GZPS com `hairtone`): `"<prefixo>_<chave como acima>_<GUID do
  hairtone>"` (o texto do GUID como está no GZPS, `00000005-0000-...`);
- **overlays** (XTOL `0x2C1FD8A1` / XMOL `0x0C1FE246`): `"<prefixo>_<GUID
  family>_<GUID hairtone>"` (GUID nulo `00000000-0000-0000-0000-000000000000`
  quando não tem); a parte alta é o CRC-24 do mesmo texto.

Conferido nos dados: roupa 1590, cabelo 1213 e overlays 1180 das 5623
instâncias batem (uma por idade/sexo em que a peça vale); as outras ~1640 são
de outros caminhos (tons de pele/olhos `GetCASSkinEntryThumbnailContext`,
roupas de Sims salvos `GetCASOutfitDataThumbnailContext`: `"%x-%x-%x-%d"` da
chave da roupa + flag `0x20`, CRC-24 e CRC-32), não conferidos. Uma miniatura
que falta é gerada na hora (`cTSUIThumbnailFactoryCAS`, câmeras de
`casThumbnailCameras2_cres`, "camera_thumbnail_adult_entireSim_cropH"…).

**LxNR (ainda aberto).** O tipo `0xCCCEF852` é registrado em
`cTSCASBodyPartSystem::Init` (BV `0x7f2e10`) com a classe `0x6CCEF9F2` (fábrica
genérica). Na memória é um `cTSBodyPartData`: um mapa chave → `vector` de
`cVector3f` por parte (`SetPartData`, `BuildDataKey(…, int, …, bool)` /
`SplitDataKey`), o que casa com o cabeçalho visto (`u32 3`, `u32 0x18` = 24
entradas de 4 × u16 = as chaves, depois os vetores de floats). O leitor não foi
achado (a fábrica usa uma vtable importada).

## 10. Aberto

- A mistura exata de arquétipos e modificadores (`BlendDeltas`). (A ordem
  dos cantos do LxNR no nível alto foi resolvida na rodada 12, §13.)
- As chaves das miniaturas de tons de pele/olhos e de roupas salvas (§9b;
  olhos e maquiagem o jogo renderiza na hora, §12).
- Como o tom de pele "do meio" (tipo 6) vira o tom expressado; o uso da
  aspiração sugerida por signo.
- As categorias de roupa de cada passo e o "random clothing" do CAS
  (`MatchHairStyle`, `MatchJewelry`, `GetMatchingHair`, BV `0x55cf50`…).
- Base 1.0: as tabelas de signo e de regiões não foram conferidas no base
  (sem símbolos), só a existência das strings.

## 11. O que está implementado (rodada 8)

- Catálogo: `src/formats/cas_catalog.*` (`s2pkg cas-catalog`). A flag 0x01 do
  GZPS esconde a parte do CAS (uniformes de carreira e NPC, alienígena,
  manequim, cópias de cabelo por categoria); as partes "nu" dos personagens
  são as `CASIE_<p>topnaked_nude_s<N>`/`bottomnaked`/`hairbald_skin`.
- Rosto: `src/formats/cas_face.*`. Todas as malhas de rosto são a mesma
  cabeça (~521 vértices) com grupos e ordem diferentes; os vértices casam
  pela coordenada de textura. Arquétipo = cabeça absoluta (amArchBase =
  amFace); modificador = cabeça unissex, direção = modificador − neutro do
  `compareage`/`comparegender` (afFace); regiões de outro arquétipo
  misturadas pelas máscaras `faceregion` (cor do vértice = peso, inferido).
  Sem o formato do LxNR, o rosto é assado direto no GMDC copiado do neutro.
- Personagem: `src/formats/character_write.*` (`s2pkg cas-make`), no layout
  do §7 (AGED/3IDR, GZPS de rosto da idade e da próxima, cadeia
  CRES/SHPE/GMND/GMDC com nomes `#0x7fe09489!0x<id>_age<n>_0_*`, TXMT
  SimSkin com `caf*`, objeto do TemplatePerson, CTSS). Não grava LxNR nem
  miniaturas.
- Genética: `src/vm/genetics.*` (§5); o Make New Character 0x01 usa.
- Personalidade/signo/aspiração: `src/formats/cas_personality.*` (§6).
- Família no bairro: `src/vm/family_write.*` (SDSC 0x20, SDNA, FAMI 0x4E com
  `kDefaultFamilyFunds` 20000, STR# do nome, inventários no NGBH).
- Telas: `src/app/cas_mode.*` (`--cas`); os painéis ficam presos à âncora
  que a janela oculta `0xAAAA` de cada um nomeia ("2d050914:bl").

## 12. Miniaturas do CAS (rodada 9)

`TSData/Res/UserData/Thumbnails/CASThumbnails.package`: 5.623 JPEGs 256×256
com o segmento ALFA, tipo `0x0C7E9A76`, grupo `0xEC7E9A54` (o
`cTSUIThumbnailService` guarda o grupo em `+0xAC`; `FixInstanceOnlyCASKey`, BV
`0x29eabc`, completa tipo e grupo de uma chave "só instância"). A instância é
um hash que a UI do CAS monta por tipo de entrada:

- `CreateCASFileName` (BV `0x983994`): idade `toddler`/`child`/`teen`/
  `youngadult`/`adult`/`elder` (nada para bebê) + `female`/`male` + espécie
  (`dog`/`smalldog`/`cat`, nada para humanos), p. ex. `adultfemale`.
- Roupas (`CreateClothingThumbnailInstanceId`, BV `0x983ea0`; GZPS com
  `hairtone` nulo): `crc32("<arquivo>_<grupo>-<tipo>-<instância>")`, a chave
  do GZPS em `%d` (com sinal).
- Cabelo (`CreateHairThumbnailInstanceId`, BV `0x983d30`; GZPS com `hairtone`):
  `crc32("<arquivo>_<grupo>-<tipo>-<instância>_<GUID do hairtone>")`.
- Overlays de textura e de malha (`CreateOverlayThumbnailInstanceId`, BV
  `0x983be0`, chamado pelo `GetCASOverlayThumbnailContext`, BV `0x987918`):
  `crc32("<arquivo>_<GUID da família>_<GUID do hairtone>")`.
- Arquétipos (`GetCASArchetypeThumbnailContext`, BV `0x988f30`):
  `crc32("<arquivo><GUID da família><região>")`, sem `_`, região vazia (rosto
  inteiro) ou `brow`, `eyes`, `nose`, `mouth`, `jaw`.
- Sims prontos (`GetCASOutfitDataThumbnailContext`, BV `0x988264`):
  `"%x-%x-%x"` de três campos do `cITSSimOutfitData` + `"-%d"` (bit 0x20),
  com crc24 e crc32 (não conferido nos dados).

`crc32` = o CRC-32 MSB-first do texto em minúsculas (`FUN_00120ff8`, o mesmo
do `nameInstanceHiHash`); os GUIDs como `%08x-%04x-%04x-%04x-%04x%08x`
(`FUN_015a40ac`). Conferido no pacote: 2.803 GZPS, 450 overlays (barbas,
sobrancelhas, maquiagem de fantasia, óculos) e 1.224 arquétipos (4.477 de
5.623). Olhos, batom, blush, sombra e delineador não estão no cache: o jogo os
renderiza na hora (`GetSkinOverlayThumbnail`, BV `0xdf7d1c` → fábrica de
miniaturas do CAS); aqui viram a textura do overlay sobre a pele do rosto,
recortada na parte que pinta (`cas::overlayPreview`).

## 13. LxNR: a estrutura facial (rodada 9)

É o `nTSCASCore::cTSBodyPartData` gravado (gravador BV `0x18c79b4`, leitor
`0x18c6ae0`; sem nome no BV, achados pelas marcas de quatro letras):

```
u32 versão              3 ou 4 nos personagens do jogo base (5 no BV); o leitor
                        aceita 1..5, e 3..5 têm este layout
u32 n, u32 chave[n]
u32 'keye' (0x6B657965)
u16 qtd, qtd × (f32, f32, f32)   um "pool" de vetores distintos (o gravador
                        junta os iguais, |d| < epsilon)
u32 'mstr' (0x6D737472)
por chave: u16 m, m × u16        índices no pool
u32 'data' (0x64617461)
```

Os vetores de uma chave vêm em pares por vértice da malha de rosto
(`ConvertFacialDataToDeltas`, BV `0x805854`): (posição − posição do rosto
neutro, (u, v, 0)). Chaves: byte baixo = índice de idade do editor (0
criança pequena, 1 criança, 2 adolescente, 3 adulto, 4 idoso, 5 bebê),
`0x100` = homem, `0xF0000` = o nível de detalhe baixo (as malhas
`<x>LOD15_tslocator_gmdc` ao lado de `<x>_tslocator_gmdc`). Todo Sim guarda
o rosto em **todas as idades e nos dois sexos** (o bebê só quando foi feito
bebê); é assim que o rosto sobrevive ao crescer. A chave do outro sexo tem
os mesmos deltas por UV (conferido em `N001_User00001`: mediana 0). Cada
"vértice" é um canto de triângulo, na ordem das faces: 2.211 = 3 × 737 no
rosto adulto, 1.335 no LOD15, 2.052/1.176 no bebê. No LOD15 a ordem é a das
faces do GMDC (igual nos dados); no nível alto os triângulos do jogo vêm em
outra ordem (blocos permutados; há duas ordens diferentes entre os
personagens de N001), então aqui a leitura casa pelos UVs e a gravação usa
a ordem das faces do GMDC.

**A ordem no binário (rodada 12).** `ConvertFacialDataToDeltas` (BV
`0x805854`) grava, para cada chave, os pares na ordem do `cMeshEntry` de
`GetMesh(idade, sexo, parte, nível)`: um `vector` de grupos (passo de 3
palavras: início, fim, capacidade), cada um um `vector` de registros de
vértice de `0x68` bytes (posição em `+0x10`, UV em `+0x40`), já
desindexados (um registro por canto de triângulo). O `cMeshEntry` é montado
por `BuildBaseMesh` (BV `0x801c00`) a partir do **rosto neutro**
(`GetNeutralShapeEntry(idade, sexo, parte)` → `+0xc` → vtable `0xb8(nível)`
→ vtable `0x98`, que achata a geometria em cantos; o resto da função o
Ghidra joga fora como inalcançável, por isso não decompilava). Então a
ordem dos blocos no nível alto é a ordem dos grupos/faces da malha neutra
que o jogo tinha carregada quando gravou (inferido: por isso dois
personagens de N001 têm ordens diferentes). **A leitura não depende da
ordem**: `ConvertFacialDataToPositionsUVs` (BV `0x804f98`), o caminho dos
dados com (delta, UV), percorre os vértices do `cMeshEntry` e, para cada
um, procura nos pares o primeiro com o mesmo UV (|Δu| e |Δv| < 0,0001,
comparando também o sinal de cada coordenada com 0) e soma o delta à
posição; não soma nada no modo 3 para o bebê e para (idade 4, sexo 1, nível
`0xF`), nem no modo 4 para o bebê (índices de idade do editor, sexo 1
inferido como homem). Só o `ConvertFacialDataToPositions` (BV `0x805498`, formato sem
UV) casa por índice. O que o opensims2 faz (casar por UV ao ler, gravar na
ordem das faces do GMDC) é o mesmo que o jogo aceita.

Conferido (`s2pkg cas-lxnr <Res> <personagem>`): regravar o LxNR de
`N001_User00001` dá os mesmos 184.846 bytes; o rosto refeito do neutro +
deltas casa com a malha do próprio personagem (distância média 0,00007, LOD15
0,000004). O `--cas-edit <personagem>` abre um Sim gravado no CAS
(`cas::readCharacterSpec`: AGED, listas de roupa, `cafOverlay*` do TXMT do
rosto, LxNR, CTSS), e o `writeCharacter` grava o LxNR (todas as idades que o
arquétipo tem, os dois sexos, os dois níveis).

**Sims prontos** (`Catalog/Bins/globalcatbin.bundle.package`): 208 Sims,
cada um num grupo `0x7F…` com AGED, 3IDR, GZPS do rosto, cadeia
CRES/SHPE/GMND/GMDC, TXMT e LxNR; um BINX (`0x0C560F39`) por Sim com
`objectidx` → o AGED e `binidx` → a coleção (`0x6C4F359D`). Miniaturas
(`GetCASOutfitDataThumbnailContext`): `crc32("ac598eac-<grupo>-1-<f>")`
(`%x` da chave do AGED), f = 0 o rosto, f = 1 o corpo inteiro (417 no
pacote).

## 14. O estúdio (`GlobalLots/CAS!.package`, rodada 9)

Um lote de 60×50 com um loft de tijolos; quase tudo do estúdio está em dois
objetos de modelo grande: `CAS - Podium` (`CASPodiumArea`: o tapete redondo,
o espelho, as luzes do pódio) e `CAS - Backdrop` (`CASFamilyArea`: o fundo
cinza, o sofá, a planta). Os CRES deles têm localizadores:

| modelo | localizador | o que é |
|---|---|---|
| CASPodiumArea | `slot_casstudio_simmark_podium` | onde o Sim editado fica, e o giro dele |
| CASPodiumArea | `camera_face_<idade>`, `camera_clothes_<idade>` | câmeras do rosto e das roupas (toddler, child, teen, adult) |
| CASFamilyArea | `slot_casstudio_simmark_0..7` | a família: fila de trás 0–3, da frente 4–7, cada um virado para o centro |
| CASFamilyArea | `slot_casstudio_simmark_baby_0/1` | bebês |
| CASFamilyArea | `camera_family_0`, `camera_thumbnail_family_0` | a câmera da família |

Luzes: lâmpadas dos próprios modelos (`CASPodiumArea_podiumKey7`, `...Fill7`,
`...Bounce7`, `...ObjectsOnly7`, `CAS_FamilyAreaKey9`, `CASFamilyArea_Fill9`,
`_leftrim9`, `_rfill9`, `_bouncelight9`, halógenas com intensidade 0) com os
atributos de `Lights/CAS_lighting.txt` ("should only use practical light
keep diffusion at zero") e `CAS_lights.txt`, mais `LotLights.txt` →
`cas/casStudio.txt` (as lâmpadas de estúdio: só a "Boom" acesa, 1,25). O
esqueleto que dá as posições é o do `loadObjectModel` (o `fromCres` puro
perde o deslocamento das juntas `platform_trans`). A ordem em que os membros
ocupam as marcas é inferida (do meio para fora, trás e depois frente).

## 15. Overlays de malha no personagem (óculos, barbas com malha; rodada 9)

Visto nos personagens do jogo (`N001_User00002`, Cassandra de óculos): cada
overlay de malha vira uma **parte de rosto a mais**, com GZPS e cadeia
próprios por idade: `age3_1_cres` → `age3_10_shpe` (as partes `lens`/`frame`
com os materiais globais do overlay, `afaccessoryeyeglassesthinrims_lensclear`)
→ `age3_1_gmnd` → `age3_1_gmdc` (a malha já deformada para o rosto; 303
vértices). Os GZPS se numeram por idade: 1 o rosto, 2 o overlay (`category`
do overlay, 807; `outfit` 2, `family` vazio), 3 o rosto da próxima idade, 4
o overlay dela. No AGED a lista do rosto de cada categoria tem `ls` = 2 e
`le<i>_0` = o rosto, `le<i>_1` = o overlay (a categoria nu não tem rosto).
Os overlays de malha do catálogo: subtipo 5 bin 15 = óculos (82), subtipo 0
bin 2 = barbas/bigodes com malha (19, por cor de cabelo); flag 1 = fora do
CAS. Cada um tem CRES/SHPE, uma "máscara" (`maskresourcekeyidx`, não usada
aqui) e os TXMT por subconjunto.

Aqui: o `writeCharacter` grava assim (a malha movida pelo deslocamento do
vértice neutro mais próximo do rosto assado, inferido; o GZPS leva a
`family` para reabrir); o `loadCharacterSim` carrega as entradas extras da
lista do rosto (os óculos dos Sims do jogo aparecem); o `readCharacterSpec`
acha o overlay de um GZPS sem família pelos hashes dos materiais do SHPE
contra as referências TXMT do overlay.

## 16. No fluxo do jogo (rodada 9)

Bairro → "Criar Nova Família" (bin de famílias) → CAS (`Scene::Kind::Cas`,
`--cas --cas-hood <pasta do bairro> --cas-return <bairro>`) → Aceitar grava a
família na pasta do bairro do usuário (`vm::writeNewFamily(..., inPlace)`;
uma pasta sob os dados do jogo nunca é escrita: vai para uma cópia sob
`--cas-out`) → `CasMode::onFamilySaved(família)` → volta ao bairro, que chama
`HoodMode::familyCreated` (relê o bairro, abre o painel de famílias e
seleciona a nova, com os 20.000 de `kDefaultFamilyFunds`) → mudar para um
lote → jogar. O Sair do puck chama `CasMode::onExit` (volta sem família).

## 17. Envelhecer, nascer e os laços da família nova (rodada 10, binário + dados)

Lido no BV 1.10 (Ghidra headless com os nomes do `symxfer`) e nas árvores do
`objects.package` (`s2pkg dump`, `s2pkg prim-usage`).

### 17.1 A primitiva Age (0x75, `nTSSG::cPerson::TryAge`, BV `0x673238`)

Lê só o byte 0 do operando (não há leitura de temp no código; os 11 usos do
jogo têm os outros bytes 0) e o converte em (dado de pessoa 58, bit de idade):

| byte 0 | dado 58 | idade |
|---|---|---|
| 0 adulto | 0x13 (19) | 0x08 |
| 1 criança | 3 | 0x02 |
| 2 bebê que anda | 2 | 0x01 |
| 3 adolescente | 0x10 (16) | 0x04 |
| 4 idoso | 0x33 (51) | 0x10 |
| 0xD bebê | 0 | 0x20 |
| 0xE jovem adulto (só BV) | −1 → 0x13 | 0x40 |
| outro | nada | — (devolve verdadeiro) |

Em ordem:
1. tira do Sim os objetos dos slots dele (vtable `0x2ac`) e guarda a lista;
2. se o AGED preparado (`cPerson +0x340`) não é da idade nova (ou não
   existe), chama `ComputeAging(dado 58 novo)` (vtable `0x4a0`, §17.2);
3. dado 175 = 1 só para jovem adulto; o AGED do personagem (`0xAC598EAC`,
   instância 1, grupo do personagem) recebe o preparado por
   `MergeOutfits` (vtable `0x17c`, com a máscara `0x110` em 0 antes e `0xFFFF`
   depois) e o `+0x340` é solto;
4. dados 119 e 120 ("Outfit Source GUID1/2") = 0, **dado 58 = o valor da
   tabela**; solta a roupa em uso (`+0x33c`) e refaz o modelo (vtable
   `0x138(0, 1)`);
5. **rosto**: se o pacote do personagem não tem rosto dessa idade
   (`HasFace(chave, idade)`, BV `0x66ca78`: procura no segmento `0x656B8F0C`
   do personagem um GZPS de rosto da idade), `UpdateFaces` (BV `0x672eb4`):
   o editor gera o rosto e o CAS grava o personagem com o pedido de
   gravação de flags `0xB` (`cTSCASSystem` vtable `0x20`; é o
   `ProcessSaveSimRequest` do §7, que exporta o GMDC/CRES do rosto da idade
   nova e o **LxNR**), depois `ResetOutfit`;
6. vtable `0x498`; esvazia a fila de animações da interface `0x1AFF3BD4`;
   devolve os objetos aos slots;
7. mensagem **`0x97`** (idade mudou) com o ID do vizinho e o dado 61
   (família); o painel vivo responde com `onSelectedPersonAgeChanged`.

Devolve sempre verdadeiro. O opensims2 (`vm/person_prims.cpp`) já grava os
dados 58/119/120/175 assim; o resto (§17.2) é o trabalho do renderizador.
O "byte 1 bit 0 = idade no temp 0" do `docs/primitives-mac.md` (operandos, 0x75) vem das
strings do editor: o código do Mac não lê temp.

### 17.2 Roupa e cabelo da idade nova (`ComputeAging` e `MakeNewAge`)

`nTSSG::cPerson::ComputeAging(dado 58)` (BV `0x6729cc`) mapeia a idade
como a animação (`< 2` bebê 0x20, 2 bebê que anda 1, 3–15 criança 2, 16–18
adolescente 4, 19–50 adulto 8, `> 50` idoso 0x10; jovem adulto 0x40 com −1) e,
se o `+0x340` não é dessa idade, cria um AGED novo (`GZCOM` `0xAC598EAC`) e
chama **`cTSSimOutfitData::MakeNewAge(idade, novo, true)`** (BV `0x8210f4`,
vtable `0x190`) do AGED do personagem; avisa a interface (miniatura).
É o que o **Generic Sims Call 24 "Preload New Sim Age"** faz antes da hora
(`TryGenericSimCall` caso 0x18: temp 0 = dado 58 novo, pela interface
`0x1A34F3F4` do `cPerson`, vtable `0x300`; não faz nada com a velocidade do
jogo > 3); a árvore "Preload - Next Age Skin" (`0x1020` do Controller - Age)
o chama ~antes da transição, então o `TryAge` normalmente já acha o AGED
pronto.

`MakeNewAge(idade, saída, sortear)`:
1. saída = cópia do AGED (máscara `0xFFFB`), `SetAge(idade)`; **idoso:
   `SetHairColor(LID nulo)`** (o cabelo grisalho; inferido); `DetermineSkeleton`.
2. Lê o pacote do personagem (`0x656B8F0C`): os GZPS de **mesma idade e
   sexo** que a saída entram como rosto (parte 2) e vão para todas as 13
   categorias (`SetEntries`, só as partes de bits < 0x40 nos humanos) — são
   os "2 GZPS de rosto" do §7 (o da idade atual e o da próxima).
3. `SetupNakedEntries`. **Cabelo**: para cada categoria menos a nu (`0x80`),
   apaga o cabelo (parte 1) e, para cada cabelo da idade velha, procura
   (`FUN_00820508`) um GZPS da **mesma `family`** na idade nova e categoria
   (idoso: com o hairtone fixo do idoso em BSS `0x2c359d0`); se não acha,
   afrouxa os filtros (exclui flags 9, depois sem a flag 0x40/0x400 do
   filtro); bebê → `FindRandomHairForAge` (BV `0x8132e0`: categoria
   `0x1779`, exclui flags `0xB`) quando a categoria (`& 0x1779`) ficou vazia.
4. **Roupas sorteadas de novo** (o `true` do `ComputeAging`): humanos apagam
   as partes `0x1C` (top 4, body 8, bottom 0x10) de todas as categorias menos
   a nu, depois `SetValidFlags(0xFFDF)` + `Randomize(4, 0)` e
   `SetValidFlags(0xFFDF)` + `Randomize(−1, 9)`. Os bits "válidos" dizem o que
   o `RandomizeForHumans` (BV `0x818734`) **não** sorteia (0x80 idade, 0x100
   sexo, 0x40 forma física, 1 pele, 2 cor do cabelo, 4 cabelo, 8/0x10 rosto,
   **0x20 roupas**): com `0xFFDF` só as roupas. Para cada categoria (menos
   `0x80` e `0x800`, e as que o catálogo marca com o bit 2), 49 % das vezes
   tenta primeiro uma roupa inteira (parte 8), senão top + bottom; os
   argumentos filtram as flags do GZPS (`Randomize(incluir, excluir)`:
   primeiro só as com a flag 4, depois todas sem as flags 1|8).
5. Acessórios (categoria `0x800`, duas partes de `0x27375e0`): os que valem
   para a idade ficam; os outros são trocados pelo item do catálogo
   equivalente.

### 17.3 A transição no jogo (dados: "Controller - Age")

Tudo em SimAntics, no objeto **"Controller - Age"** (OBJD `0x2C085A2B`, grupo
`0x7F07FBBC`, tipo 7): "Age Transition" (`0x1009`) é a única árvore que
chama Age fora das de depuração (bytes 1, 3, 0, 4: criança, adolescente,
adulto, idoso; o bebê → bebê que anda é "Interaction - Person B - Baby",
`7F63E31E`/`0x1001`). Ela escolhe a animação pela idade
(`a2b-/b2a-growUpBaby-*`, `*-react-growUp-good/badMemory` do STR# "Anims"),
roda a cinemática (STR# 0x97 do grupo: `ageTransTeen2AdultMale/Female`,
`ageTransToddler2Child`, `ageTransChild2TeenMale/Female`,
`ageTransAdult2Elder`; "Cinematic - …" `0x102E`/`0x1031`–`0x1033`, câmera
`o-cineCam-Age*`), "Sub - Post Age FX & Sound" (`0x101E`: som + 5 efeitos
`*_grip_ageTransitionGroup<Adult|Teen|...>` nos ossos root/neck/pulsos/
chapéu, STR# "Effects"), "Transition Relationships - Child to Teen/Teen to
Adult/Adult to Elder" (`0x1021`–`0x1023`), "Want Satisfy - Grow Up",
"Memory - Grow Up", "Work - Retire Notification" e o diálogo "has grown into
…". O bolo: "BirthdayCakeList - Add/Remove/Find/Empty?" (`0x100E`–`0x1013`),
"Sub - Kill the Cake", "Sub - Is Cake Day?", "Get in Temp 0 - Age Duration"
(`0x1014`) e o aviso "will grow up in just one day at 6 p.m.". Rodar essas
árvores na VM dá o fluxo; o motor só precisa do §17.1/17.2.

### 17.4 Bebê novo: o pedido do CAS (`ProcessCreateBabyRequest`, BV `0x55a8f4`)

Complementa o §5 (genética): a máquina de estados avança um estado por tick:

| estado | faz |
|---|---|
| 0 | pede (assíncrono, prazo 60 000 ms) os **LxNR** (`0xCCCEF852`, instância 1) dos dois pais, no grupo do personagem de cada um, se humanos |
| 1 | sexo (§5); dado 58 = 1, dado 0xBA = espécie (BV), dado 65 = sexo, 82 = 500, 81 = 0; AGED novo com idade 0x20, sexo, espécie, forma `FLOAT_0274b470`, grupo e chave do personagem novo; registra no sim system |
| 2 | o editor (`EdithEditor`) carrega o AGED; o gerente de genética (`FUN_000c7fb0()` vtable `0x6c`, slot `0x28`) monta o DNA e o rosto do filho a partir dos dois pais e do ID novo (`"%d"`) — §5 `DetermineGenetics`/`DetermineDescendantFace`, o editor mistura os dois LxNR por região; humanos: cor do cabelo pela genética (`GetBodyPartSystemHairColor(0x20, sexo, cor)`), olhos, e **uma roupa de bebê sorteada** entre os itens do bin (`UIServiceManager` `0x68`, idade 0x20) sem a flag 1; o editor exporta para o personagem (vtable `0x30` com o ID) |
| 3 | `ClearAllEntries`, `SetupNakedEntries`, `SetValidFlags(0x3DB)`, `Randomize(−1, 8)` |
| 4 | nome (rodada 11, BV `0x55b8a0`–`0x55b908`, índices a partir de 1 no grupo `0x7F96C284` do Live.package): pela idade do AGED, bebê que anda (1) STR# 0x95 #1 menino / #2 menina ("Baby Boy"/"Baby Girl"), criança e adolescente (2, 4) STR# 0xE6 #0x38 menino / #0x37 menina, adulto (8) 0xE6 #0x43 / #0x44, outra idade — o bebê, 0x20 — 0xE6 #0; + sobrenome do pai do primeiro ID. A tabela 0xE6 é do BV (o Live.package do jogo base não tem): o opensims2 dá ao bebê os nomes do bebê que anda (0x95, cuja nota diz que eram os do diálogo "Nomear Bebê"; inferido): "Bebezinho"/"Bebezinha" em português |
| 5 | humanos: o neighbor manager grava o personagem (vtable `0xa0(vizinho, 0, 1)`: o `ProcessSaveSimRequest` do §7, que escreve GZPS/AGED/CRES/GMND/SHPE/TXMT/TXTR/3IDR/IMG e o **LxNR** exportado pelo editor) |
| 6 | pronto |

`CreateNewBaby(pai1, pai2, modelo, ...)` (BV `0x5634b8`): sem modelo,
sorteia qual pai dá a espécie (`rand(100) < 51` → o segundo) e usa o modelo
padrão dela (humano `0x7FD96B54`).

### 17.4b O bebê no lote: colo, mamadeira, berço e nome (rodada 12, binário + VM)

- **Colo.** "Pickup - Do Carry Sim" (global `0x276`) empurra "Interaction -
  Carry" no adulto e "Be Carried" no bebê; o adulto cria o controlador,
  empurra no bebê a interação aninhada "Be Picked Up" e espera em
  "Pickup - Wait For Current Interaction" (`0x23E`, `0x0118` = Idle) até os
  dados de pessoa **102/103** ("ci: Current Running Interaction Index /
  Object ID") do bebê serem os dessa sub-ação. No binário
  (`cTSPerson::SetSubInteractionData`, BV `0xadaa30`) 102/103 são os da
  sub-ação que roda, senão os da ação (o `SetCurrentAction` copia 21/80
  neles), e 75/76 (mestre da sub-fila) são sempre os da ação atual; o
  opensims2 não mantinha o 103 e punha 102 = ação: o adulto esperava para
  sempre. Agora `syncQueueData` faz como o binário.
- **Pôr no chão.** "Marker - Snap Into" (o event tree do overlay do bebê no
  "Drop - Sim - Animate - Drop") só encaixa o bebê no marcador no evento
  **9998** (BCON global `0x124` "G - Anim Events - Std": 9997, 9998, 9999,
  −1). O tratador de eventos de animação do binário
  (`nTSSG::cCustomEventHandler2`, BV `0x780ad4`) mapeia as chaves de evento
  do motor: tipo 7 → 9997, **6 → 9998**, 3 → 9999, −1 → −1, 1005 → o
  "event=N". Quem emite o tipo 6 não foi achado (as animações não têm essa
  chave); inferido: o fim da ação do overlay, mandado logo antes do 9999.
  Rodada 15: conferido que **nenhum dos 11.757 ANIM** tem chave de tipo 3, 6
  ou 7 (só 1014 som, 1005 "event=N" e 8): essas chaves são feitas pelo
  motor; o `cEventKey::SetType` (BV `0xd0d458`) só é chamado pela vtable e
  o `HandleEvents` (BV `0x780ad4`) lê o tipo de cada chave (vtable `0x30`)
  sem saber quem a criou. Continua inferido.
  Sem isso o adulto tentava pôr o bebê no chão para sempre.
- **Berço.** "Interaction - Put In Crib - Subqueue" empurra as entradas do
  berço a partir da parte 0,0, que não tem TTAB (só o mestre "Crib - Cheap"
  tem, e o grupo de interação é 0): a parte de índice 0 de um multi-tile sem
  grupos usa a tabela do mestre (inferido; antes o Main do adulto morria
  com "Tree table not found"). Prova: `s2pkg pie N002_Lot10 Jenny Tombstone
  --create 0x0CB53A3A@24,21,2 --create 0x7a03bdef@20,24,2 --all --select
  197 --ticks 6000 --anims`: Johnny pega o bebê, dá mamadeira
  (`a2b-babybottle-feeder-*`), põe no berço (`a2b-crib-put-putter`), o bebê
  toca `b2o-crib-idle-*`, tira de novo para ninar e devolve.
- **Nome.** O "Interaction - Home Birth" abre o diálogo tipo 23 ("Baby
  Name", STR# `0xD9`) com os vizinhos dos bebês nos **temps 0 e 1** (a
  árvore põe o dado de pessoa 31 dos bebês dos locais 2 e 1; o "Rename Sim"
  põe o do próprio Sim no temp 0). A classe é o
  `cTSWinProcNameObjectDialog` (rodada 14, `docs/primitives-mac.md`
  "Nomes"): um painel por caso (um bebê, gêmeos `0xCDC2CAA6` com duas
  caixas, renomear), e o OK dá a cada Sim o nome aparado da sua caixa. Até a
  rodada 13 (inferido): um botão OK, sem tempo limite (é especial), o
  `Dialog::text` vem com o nome padrão ("Bebezinho"/"Bebezinha") e, se o
  anfitrião mudar o texto, o OK renomeia o vizinho do bebê (inferido). Sem
  cabeça, `--dialog-answer 0` aceita o padrão. **Caixa de texto no app
  (rodada 13)**: o tipo 23 usa o painel "baby" do script `0x2D91050A`
  (`ui.package`, 330x220: "Nomear Recém-Nascido", o texto "O recém-nascido
  chegou e precisa de um nome!" se o diálogo não traz o seu, o ícone de
  menina/menino `0xA1230060`/`0xA1230055`, a caixa `0x14500157` com o
  `GZWinTextEdit` de `maxtext=18`, e o botão Aceitar de tique). Digitar
  substitui o nome padrão (vem selecionado), Backspace apaga, Return
  aceita; o cursor pisca a cada 0,5 s (`caretperiod=1000`). Sem cabeça:
  `--dialog-demo 23[g]` (põe o diálogo), `--dialog-type "Ana"`,
  `--dialog-click return|escape|x,y`.
- **No app**: um Sim no slot de outro (bebê no colo) é desenhado no osso do
  slot do carregador (`BellyTarget_surface`, onde as animações de pegar
  prendem a raiz do bebê, com a rotação do osso), depois do carregador
  (`docs/screenshots/r12-bebe-colo.jpg`). **Mamadeira (rodada 15)**: o
  "adulto inclina e o bebê some no peito" eram as mãos: as cadeias de IK
  da `a2b-babybottle-feeder-*` e da `a2b-carry-belly` miram ossos **do
  bebê** (alvos de IK de tipo 2: `head_grip` para a mão da garrafa,
  `bottomtarget_surface` para a mão de baixo), que o objeto de IK do
  pedido dá (Animate Sim: dono do byte 9; Animate Overlay: temp 6 com o
  bit 0x08 do byte 15; `docs/primitives-mac.md` §4); o opensims2 lia esses
  nomes no próprio esqueleto do adulto e levava a garrafa ao rosto dele.
  Agora o adulto segura o bebê no braço e leva a garrafa à boca dele. A
  garrafa na mão toma a pose do `snapObjectPose` do snap
  (`o2a-fridge-babyBottle-get-babyBottlePose`), e o mesmo modelo
  encaixado de novo com outro nome (`babyBottle` → `o2a`) não aparece
  duas vezes. Repro sem cabeça: `opensims2 --lot N002_Lot10 --sim-id Jenny
  --vm-create 0x0CB53A3A@24,21,2 --vm-create 0x7a03bdef@20,24,2
  --vm-queue "Tombstone:New Baby" --track-sim-id 160 --orientation 2
  --zoom 6 --run-vm 740 --frames 6 --frames-from 21` (quem dá a mamadeira
  no app é o alienígena, 160; `OS2_LOG_ANIMS=1` lista os pedidos e os
  objetos de IK). Captura: `docs/screenshots/r15-mamadeira-ik.jpg`.

### 17.5 Relacionamentos de uma família nova (dados)

Não há código C++: quando um Sim da família do lote começa o Main ("Function
- Main" da pessoa, `7F01EC29`/`0x2001`, instrução 0: dado 61 == global 9),
chama **"Init - Relationships"** (`0x2013`, semi-global da pessoa). Ela chama
"Family - Relationships - Init" (`0x20E`, global: grava o parentesco, var 3)
e, para cada outro membro da família (Set to Next por dado 61), pega o
parentesco ("Family - Get Relation", `0x1DA`: o que o outro é para mim) e
grava as 4 variáveis **diária e vitalícia, nos dois sentidos** = mínimo + rand
(faixa), com o mínimo no BCON `0x201E` "Relationships - Resident Init - Min"
e a faixa no `0x2031` "... - Range" (grupo `0x7F01EC29`):

| o outro é | eu→outro diária | eu→outro vitalícia | outro→eu diária | outro→eu vitalícia |
|---|---|---|---|---|
| 1 filho | 30 + r30 | 30 + r30 | 35 + r10 | 20 + r10 |
| 2 pai/mãe | 35 + r10 | 20 + r10 | 30 + r30 | 30 + r30 |
| 3 irmão | −10 + r59 | 1 + r48 | −10 + r59 | 1 + r48 |
| 9 cônjuge | 70 + r20 | 70 + r10 | 70 + r20 | 70 + r10 |
| 4 neto | 30 + r30 | 30 + r30 | 10 + r30 | 10 + r30 |
| 5 avô | 10 + r30 | 10 + r30 | 30 + r30 | 30 + r30 |
| 6 sobrinho | 1 + r48 | 1 + r48 | 1 + r48 | 1 + r48 |
| 7 tio | 1 + r48 | 1 + r48 | 1 + r48 | 1 + r48 |
| 8 primo | 1 + r48 | 1 + r48 | 1 + r48 | 1 + r48 |
| 0/outro (colega) | 10 + r35 | 0 + r20 | 10 + r35 | 0 + r20 |

(r n = Random Number 0…n−1; "eu→outro" = as locais 2/3 que o Relationship
grava de mim para o stack object, inferido pela ordem dos operandos.) Depois
compara as diárias com o BCON global `0x102`[0] e as vitalícias com
`0x102`[15] e liga as flags 5/6 do BCON `0x11C` ("G - Relationship - UI
Flags": amigo / melhor amigo) na var 1 dos dois lados. Um bebê novo passa
pelo mesmo Main, então ganha esses valores com pais e irmãos.

## Rodada 12: personagens salvos antes da correção do rosto

Os personagens gravados pelo nosso CAS antes da rodada 11 têm no TXMT do
rosto `stdMatBaseTextureName`/`baseTexture0` = `<p>face-s<n>` (ex.
`tfface-s2`), textura que o jogo não tem para toda idade/sexo (as
adolescentes usam `afface-s<n>`, a que o `tfface_s<n>_txmt` do jogo nomeia).
O `loadCharacterSim` agora, quando essa textura não existe no índice, lê o
nome pelo material de rosto do jogo do mesmo prefixo e tom
(`cas::faceSkinTexture`), como um rosto salvo hoje; o arquivo não é
regravado (abrir o Sim no CAS e salvar regrava com o nome novo). Inferido:
o jogo não teria esses arquivos, então não há comportamento original a
copiar; é uma migração nossa (`r12-rosto-antigo-migrado.jpg`).

`--cas-gender` aceita `m`/`male` e `f`/`female` e passa pelo mesmo
`CasMode::setGender` dos botões 0x401/0x400 (cabeça base do sexo novo,
roupas, overlays, modificadores e regiões limpos); antes `male` caía em
mulher e o script não limpava os modificadores.

**Biografia** (rodada 12): no `CB980E81` o botão 0x8CB7C11F ("Biografia do
Sim", `cTSUISimEditor::HandleButtonShowSimBio`, não decompilado) mostra a
janela escondida 0x4CD0FDD9 ("Biography", fundo `ca2b0180` 350×309) com o
`GZWinTextEdit` 1 ("Please enter a biography for this Sim . . ."). Antes o
botão levava ao passo 2. O texto é o `description` do personagem (CTSS
0x7D0 #2, já gravado e lido pelo `character_write`). Inferido: o limite de
512 caracteres, fechar a janela ao trocar de passo/Sim, Return tira o foco.
`--cas-bio <texto>` abre a janela e preenche (`r12-cas-biografia.jpg`). O
`HandleButtonShowHouseholdBio` (bio da família) não foi feito.

**Peles personalizadas** (botão 0x2CB70004, "Peles Personalizadas"): no
base os 7 `skintone` do `Skins.package` são os 4 normais (flags 0) e
manequim, Alien e manequim do CAS (flags 1, escondidos), então a lista do
botão sai vazia sem conteúdo baixado (Body Shop/Downloads); não feito.

