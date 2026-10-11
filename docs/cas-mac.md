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
  pedido ou um sorteio.

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
"estrutura facial": os pesos que geraram a malha). Cabeçalho visto num
personagem (`N001_User00001`): `u32 3` (versão), `u32 0x18`, 24 entradas de
`(u16, u16, u16, u16)` e depois blocos de floats por região. O formato
completo não foi decodificado (o leitor/gravador fica no editor do Sim,
`TS::EdithEditor`, vtable `0xfc`/`0x14`).

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
6. nome padrão do STR# `0x7F96C284` (#0x37/0x38 bebê menino/menina, ...) +
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

- O formato do LxNR (os pesos por modificador/região; pistas no §9b) e a
  mistura exata de arquétipos e modificadores (`BlendDeltas`).
- As chaves das miniaturas de tons de pele/olhos e de roupas salvas (§9b).
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
