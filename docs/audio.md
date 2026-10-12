# Áudio do The Sims 2 (jogo base)

Levantamento dos recursos de som em `TSData/Res/Sound/*.package` (21 pacotes, ~1 GB) e `TSData/Res/Locale/<idioma>/Sound/Sfx2.package`. Contagens do jogo base PT-BR (`s2pkg sound-sweep data/game/TSData/Res`).

## Tipos

| Tipo | Nome | Conteúdo |
|---|---|---|
| `0x2026960B` | AUDIO | amostras (XA, SPX, MP3) e também texto (INIs do `TSData/Sys`, fontes de HIT, FSC, scripts de UI) e HIT compilado |
| `0x0B9EB87E` | track settings | property set XML (`cGZPropertySetUint32`) de uma *track*: chaves em hash (`0xff099db2` = taxa de amostragem, `0xff1cdf6a` = `"Track"`, `0xff3c2160`/`0xff6f9546` = instance, `0xff99d2d5` = instanceHi; o resto não foi identificado) |
| `0x7B1ACFCD` | hit list (`.hls`) | u32 versão (56), u32 contagem, pares (instance, instanceHi) das amostras que a track sorteia |
| `0x9D796DB4` | track logic (`.tlo`) | binário, 6 no `Misc.package`, não decodificado |
| `0xEBFEE33F` | property set XML | ambiência (`LotDay`, curvas, `Freshness*`), categorias de música (`0xFFD3706D`, grupo `0x4C1940C5`), `aliases`, `shoetypes`, `events`; também nos `*_Neighborhood.package` |
| `0xEBFEE345` | property set XML (`.xmi`) | 1 (`Content Handler Audio Test Settings`) |
| `0x8DB5E4C2` | ? | 1 (`expverbtest3`, grupo Effects), binário "FX" — parece um preset de efeito (reverb) |

QFS: as track settings são sempre comprimidas; das amostras, ~3,4 mil (SPX de voz e XA de SFX) estão em QFS. O `DbpfPackage::read` já descomprime.

## Nomes e grupos

O nome de uma track ou amostra vira a chave como no scenegraph: instance = CRC24(nome em minúsculas) | `0xFF000000`, instanceHi = CRC32 (MSB, sem xor final) — confirmado com 90 nomes do `Sims2.exe` (`ui_button_play`, `ui_album_open`, `splash`, `credits_lp`, ...). O grupo diz de que pasta veio o arquivo (`FileNameAsInstanceID` no INI `TSAudio`, grupo `0xADD550A7` do `Misc.package`, seção `[DBSegments]`): por exemplo `0xEB8AB35D` = `UI/Tracks`, `0x6B8AB3D7` = `UI/Samples`, `0x0B8AB3CD` = `Sfx/Samples/A_M`, `0x2DA1F2EC` = `Vox/Samples/A_G`, `0xFF23C3BF` = `Music/pop` (tabela completa em `soundGroupName`, `src/audio/sound_bank.cpp`).

Uma *track* (o que o jogo toca pelo nome) é o par track settings + hit list de mesmo grupo/instância num grupo `*/Tracks`; a hit list aponta as amostras (grupo `*/Samples`) só por (instance, instanceHi). 15.935 hit lists, 50.068 entradas, 49.998 resolvem; as 70 que faltam são de voz (`Vox/Tracks/*`) e não existem nos pacotes do jogo base.

Um INI do grupo `Sys` (`26B3DB7C-FFC55552`, tunables de bairro) tem a mesma chave de uma música do `Music/pop`, por isso a busca por nome ignora os grupos de texto (`Sys`, `TrackLogic`, `HitLabUI`, `PropertySets`).

## Formatos das amostras (AUDIO)

| Formato | Qtde | Decodificadas | Horas | Variantes |
|---|---|---|---|---|
| XA (EA XA ADPCM) | 7.398 | 7.398 | 3,51 | `XA\0\0` 22050/32000 Hz mono (7.031) e estéreo (20); `XA\x12\0` (303, SFX/ambiência); `XAJ\0` (44, `Ambience/Emptyness`) |
| SPX (Speex) | 42.962 | 42.962 | 38,77 | todas modo 2 (ultra-wideband, 32 kHz), 640 amostras por frame, mono |
| MP3 | 143 | 143 | 3,87 | músicas (rádio, Build/Buy/CAS/Nhood) 32/44,1 kHz estéreo; 7 mono (SFX/stings) |
| WAV / UTK | 0 | – | – | aceitos no INI (`wav`), mas nenhum no jogo base |
| HIT | 1 | – | – | `HIT!` (`TRAX`, `ENTP`), grupo `TrackLogic` |
| texto | 48 | – | – | INIs (`TSAudio*`, `[DBSegments]`, tunables), FSC (`version 7`, ambiência), `.scr` do HitLab, tabelas `.str` |
| outros | 4 | – | – | binários do `TrackLogic` (HIT compilado / tabelas de símbolos) |

### XA
Cabeçalho de 24 bytes: `"XA"`, 2 bytes de variante, u32 tamanho em bytes do PCM decodificado, WAVEFORMATEX do PCM (tag 1, canais, taxa, bytes/s, align, 16 bits). Depois blocos de 15 bytes por canal: 1 byte de cabeçalho (nibble alto = preditor 0..3, nibble baixo = shift − 8) e 14 bytes de resíduos de 4 bits (nibble alto primeiro), 28 amostras por bloco; `s = ((r << 28) >> shift + cur·c1 + prev·c2 + 0x80) >> 8`, com (c1, c2) = (0, 0), (240, 0), (460, −208), (392, −220). No estéreo os dois cabeçalhos vêm primeiro e os resíduos se alternam byte a byte (L, R) em blocos de 30 bytes. 13 arquivos estéreo terminam com meio bloco (15 bytes); decodificado com zeros e cortado pelo tamanho do cabeçalho. Alguns SFX que soam como ruído usam quase só o preditor 0 (provavelmente água/chuva; o sinal decodificado é ruído de banda larga, sem nome para conferir).

Confirmar o que foi inferido: o que muda nas variantes `0x12` e `J` (o decodificador é o mesmo e o som sai coerente; talvez *loop* ou tipo de mixagem), e se o meio bloco final é mesmo enchimento.

### SPX
Cabeçalho de 15 bytes: `"SPX1"`, u8 canais (1), u32 tamanho em bytes do PCM, u32 modo do Speex (2), u16 amostras por frame (640). Depois frames de u8 comprimento + um pacote Speex. Decodificado com a libspeex 1.2.1 em `third_party/speex` (BSD), com o *perceptual enhancement* padrão ligado. Confirmar o que foi inferido: se o jogo liga o enhancement.

### MP3
MPEG-1/2 Layer III sem ID3. Decodificado pelo AudioToolbox do macOS (`AudioFileOpenWithCallbacks` + `ExtAudioFile`, sem gravar em disco).

## Ferramentas
```
s2pkg sound-list <Res|pasta> [filtro]        # amostras: chave, grupo, tamanho, formato/taxa/duração do cabeçalho
s2pkg sound-info <Res|pasta> <nome>          # track settings, hit list e amostras de um nome
s2pkg sound-extract <Res|pasta> <nome|TGI> out.wav [n]
s2pkg sound-sweep <Res|pasta>                # decodifica tudo: contagens por formato e as hit lists que não resolvem
opensims2 --play-sound <nome|TGI> [--sound-index n] [--sound-wav out.wav]   # toca e sai
```

## Reprodução
`SoundPlayer` (`src/audio/player.hpp`, C++ puro) toca PCM de 16 bits numa `AudioQueue` do AudioToolbox na saída padrão (3 buffers de 0,25 s, reabastecidos na thread do próprio AudioToolbox; para quando o último volta). `--play-sound` no app resolve o nome, imprime taxa/canais/duração/pico/RMS, grava o WAV se pedido, toca até o fim e sai, sem abrir janela. Sem como ouvir aqui, a verificação é pelo WAV (duração e níveis) e pelo tempo de reprodução: `ui_button_play` (0,645 s) toca em ~0,98 s com o fim da fila, uma fala SPX de 5,92 s em 6,13 s.

## Eventos, modificadores e aliases
O jogo toca *eventos* por nome (da VM, pelo STR# 0x4132 "Sound ID Table" do objeto; das animações, eventos ANIM tipo 1014 `sound_name=...`). O nome final da track sai assim (`SoundSystem::resolveEvent`):
1. Modificadores do property set `events` (`EBFEE33F-4C1940C5-C48BBB17-FFD6C8B9`, 211 eventos): cada um acrescenta `_<valor>` na ordem listada. `destfootstep`/`destsurfacetype` = superfície (piso: `surfaceType` do `Catalog/Scripts/floorPatterns.txt` ou `surfacetype` do catálogo — `cment`, `cpet`, `gravel`, `lino`, `marble`, `wdeck`, `wood` —; terreno: `terrainTypeSoundSuffix`, `grass`; para objetos, o item 0 do STR# 0x94 "soundinfo"), `sourcefootstep` = sapato (`shoe` do GZPS da roupa, 1–6 pelo property set `shoetypes`: bare, leath, heel, rub, sand, slip), `sourcesurfacetype` = item 1 do soundinfo, `objectquality` = item 2 (`exp`, `chp`, `wdglass`, `gmetalexp`, ...), `vox` = idade+gênero+voz (`ama`, `tfa`), `physicalskill` = `ls`/`ms`/`hs`, `creativeskill` = `a`..`j` (curvas do property set de perícias). Ex.: `foot_step` → `foot_step_grass_leath`, `chair_liv_sit` → `chair_liv_sit_cloth`, `clock_alarm_lp` → `clock_alarm_lp_exp`, `obj_put` → `obj_put_marble_plate` (conferidos com `s2pkg sound-names`).
   **Vozes** (`cEMVoxModifier`, binário de Mac): os eventos `vox_*` (das animações sociais e de reação, `vox_soc_agree_hiA`, e do "Vox - Choose and Play" da VM) levam o sufixo de voz do Sim que fala **por último**, depois dos modificadores listados (`vox_exmach_pec_stressA` = `physicalskill` → `vox_exmach_pec_stressA_hs_ama`); o property set `events` só lista `vox` para 43 eventos `cine_vox_*`, e as tracks dos outros têm o sufixo (inferido que vale para todo `vox*`). O sufixo (`GetSuffixAgeLetterCode` + `BuildVoiceSuffixFromObjectId`): letra da idade (dado de pessoa 58: < 2 `b`, 2 `p`, < 16 `c`, < 19 `t`, > 50 `e`, senão `a`); bebê e criança pequena só a idade; criança idade + voz; adolescente em diante idade + gênero (dado 65: `m`/`f`) + voz. Voz: `g` com o bit 0 do dado 68 ("Ghost Flags"), senão `'a'` + dado 88 ("Voice Type") módulo o número de vozes da idade (tabela no binário: limites 1, 2, 15, 18, 50, máx.; 1, 1, 1, 1, **2**, 1 vozes) — `ama`, `amb`, `afa`, `afb`, `tma`, `tfa`, `ema`, `efa`, `ca`, `p`, `b`. Fantasmas (`amg`) não têm gravações no jogo base: aqui caem na voz `a` (inferido). Os dados 0xAF e 0xBA que o binário também lê são de expansões. `physicalskill`/`creativeskill` vêm das faixas do property set de perícias (`Physical` = `0,ls,333,ms,666,hs,1000` sobre o dado 17 "Body Skill"; `Creative` `a`..`j` sobre o 15).
2. Aliases (property set `aliases`, 121 chaves) redirecionam o nome composto: `foot_run_cpet_leath` → `foot_run_cpet_rub`, e o `foot_step` sem modificadores → `foot_step_cpet_bare`. Se a track não existe, os sufixos caem do fim. Alguns objetos pedem tracks que o jogo base não tem (`fridge_idle_run_lp_exp`; o jogo mostra "Missing sound info table entry").
3. Uma amostra da hit list, sorteada (a escolha do jogo não foi decodificada).

Os soundinfo (STR# 0x94) têm 4 itens: superfície que o objeto é/oferece (`dest`/`destination` = nenhum), superfície do objeto segurado (`source` = nenhum), qualidade e partição de ambiência (`flora`, `object:tombstone`).

## Ambiência
Property sets com `Name` (17 camadas, listadas pelo `LayerIds` das configurações de ambiência): `EnabledPrimitiveIds` (booleanos `islot`/`isneighborhood` e valores como `lightofday` = hora/24 passados pela curva nomeada logo depois, p. ex. `curveismorning` = 6h–12h), partições `AND`/`NOT` (`isflora`, `iswater`, `isinside`, `istombstone`), um laço (`AmbientLoopName`: `lot_outside_lp`, `nite_lp`, `water_lp`, `frog_nite_lp`) e/ou uma sequência FSC (`FreshnessScoreName` = nome do recurso de texto FSC `version 7`: `midday`, `morning`, `night`, `dusk`, `frogs`, `owls`, `empty_day`, `cemetary_ghost`, ...), com volume pelos pares valor/curva de `LoopVolumePrimitives`/`FreshnessVolumePrimitives` (`outsideness`, `zoom`). FSC: cabeçalho (volume, tempo em BPM) e notas de 20 colunas (volume /1024, "rand" = início de segmento para onde o tocador pula, pan /1024, duração em batidas, probabilidade em 16, amostra `*.xa`), como o `FSC.cs`/`FSCPlayer.cs` da FreeSO. `AmbiencePlayer` toca isso com a hora do relógio da VM, a parcela ao ar livre (quartos de tile sem sala) num raio de `LotAmbienceZoomRadiusCurve` (20 tiles longe, 8 perto) em volta do pivô da câmera e o zoom. No bairro (rodada 10) tocam as camadas `isneighborhood` (`nhood_amb_lp`, vento `nwind*`, pássaros), com `iswater`/`isempty` ligados para a vista inteira e a hora fixa ao meio-dia (o bairro não tem relógio; inferido). `OS2_SOUND_HOUR=<h>` força a hora da ambiência do lote (teste: 02:00 `EmptynessNight LotNight LotNightLoop LotOwls`, 06:30 `LotDayLoop LotMorning`, 12:00 `EmptynessDay LotDay LotDayLoop`, 20:00 também `LotEvening`). Inferido: partições como flags do lote inteiro (no jogo são áreas: `LotPartitionSize 3`), `NOT isinside` só desliga quando nada ao redor é ao ar livre, pitch das notas FSC ignorado.

## Música
**Entre cenas** (rodada 10, `GameAudio` em `app/lot_audio.*`): os estados de música do binário (seção "Rodada 10" abaixo) — menu principal = abertura (`Music/splash`), bairro = `NHood` (a abertura some em 6 s; voltar do lote para tudo e recomeça), CAS, compra, construção e o estado do lote (só aparelhos de som, pausados em compra/construção); trocar de estado pausa a música do que sai e retoma a do que entra (`AudioMixer::setPaused`). Música 2D a `defaultmusicvolume` × `2dMusicVolumeScalar` (0,75, do property set do gerenciador; inferido que vale para a música 2D). Músicas decodificadas numa thread de fundo (a próxima enquanto uma toca), sem cache.

Categorias no property set `musiccategories` (`Build`, `Buy`, `CAS`, `Nhood`, `hiphop`, `metal`, `pop`, `rnb`, `salsa`, `techno`) = pastas `Music/<categoria>` (MP3). `MusicPlayer` toca as músicas da categoria em ordem embaralhada, volume 0,7 e fade de saída de 3 s (`defaultmusicvolume`, `trackfadeouttime` das configurações do TrackLogic). Os aparelhos de som tocam eventos `stereo_<estação>` (`stereo_pop`, ...), que não são tracks: tocam a categoria de mesmo nome a partir do objeto (inferido; `stereo_classical` não tem músicas no jogo base).

## No app
`--no-audio` desliga, `--mute` deixa mudo; com `--screenshot` nenhum dispositivo é aberto e `--sound-mix out.wav` grava o que teria tocado (mixer renderizado junto com os ticks da VM). `--music <categoria>` toca música no modo vivo; cenas sem relógio (menu, bairro, CAS) tocam `--sound-dwell` s (5) no `--sound-mix`, que cobre o fluxo inteiro; `OS2_SOUND_LOG=1` lista tudo o que tocou. O log de saída resume: sons da VM, eventos de animação, nomes tocados e que faltaram, camadas de ambiência ativas e a hora.

## Rodada 10: o que o binário de Mac diz (BV 1.10, Ghidra headless)

### Sufixo `vox` (`cEMVoxModifier`, BV `0xbf2b64`)
O modificador `vox` acrescenta `_<sufixo>` ao nome do evento; só vale para pessoas (senão o log "Vox event %s called on non-person object %s" e nada muda). O sufixo (BV `0x1a6f0f4`, que o `symxfer` chama por engano de `GetFloorTypeSuffixFromObjectLocation`) depende do dado 58 (idade):

| idade (dado 58) | sufixo | exemplo |
|---|---|---|
| < 3 (bebê, bebê que anda) | idade | `b`, `p` |
| 3–15 (criança) | idade + voz | `ca` |
| ≥ 16 | idade + sexo + voz | `tfa`, `ama`, `amb`, `efa` |

- **Idade** (`GetSuffixAgeLetterCode`, BV `0xbf07b0`): espécie (dado 0xBA) ≠ 0 → `n` (animais, BV); dado 58 < 2 → `b`; 2 → `p`; 3–15 → `c`; 16–18 → `t`; ≥ 19 com o dado 175 (jovem adulto, BV) → `t`; 19–50 → `a`; ≥ 51 → `e`.
- **Sexo** (BV `0x1a6ee40`): humano: dado 65 = 0 → `m`, 1 → `f`, outro `?`; espécie 1 → `d`, 2 → `l`, 3 → `k` (BV).
- **Voz** (BV `0x1a6ef00`): dado 68 ("Ghost Flags") bit 0 → `g`; humano sem jovem adulto: `'a' + (dado 88 "Voice Type" mod n)`, com n pela faixa de idade (tabela BV `0x2739a00`: limites 1, 2, 15, 18, 50, ∞ e n = 1, 1, 1, 1, **2**, 1): **só adultos têm duas vozes (`a`/`b`)**; os outros e o jovem adulto → `a`.

### Efeitos de som (`cTSSoundEffect`, componente tipo 7)
Bloco: versão u16, flags u32, **`audioId` u32**, intervalo de posição (s), duração (s), volume (como no OpenTS2). `Start` (BV `0xf55fcc`) manda ao sistema de áudio o evento `0xEA451903` com `0x0BD63077` = `audioId`, `0x6BD63081` = ID da instância (o próprio efeito), `0x8BD63096` = bit 0 das flags e, com o bit 1, o volume em `0x0BD63084`; `ApplyEffect` (BV `0xf55d60`) manda `0xCA45190F` com a posição (`0x6BD63530`) a cada "intervalo" segundos e para o efeito quando passa a duração (> 0); `Stop` manda `0x6A451908` (`0x8BD63096` = parada imediata). **O `audioId` é a instância da track** (CRC24 | `0xFF000000` do nome): 107 dos 110 sons do `effects.package` acham uma hit list com essa instância (faltam `0xFFED2D23` ×2 e `0xFF14E140`); todos têm flags 0 (sem volume próprio), intervalo 0,5 s e duração 0 (um tem 5 s).

### Música (`cTSAudioMusicManager`)
Cinco **estados** (`Init`, BV `0x617760`), cada um com as próprias instâncias tocando: `0xEC313961` bairro, `0xEC313964` CAS, `0xEC313967` lote (vivo: só aparelhos de som, sem música de fundo), `0xEC31396A` construção, `0x0D8A50AD` compra. Trocar de estado (`_SetCurrentMusicState`, BV `0x61723c`) **pausa** as instâncias do estado que tocava e **retoma** as do novo (fade 0 s, `FLOAT_0274b480`): voltar ao modo compra continua a música de onde parou, voltar ao vivo retoma os rádios. Quem troca:
- modo de jogo (BV `0x611e38`): 2 vivo → estado lote; 3 compra → estado compra + `Start("Buy", "exp")`; 4 construção → estado construção + `Start("Build", "exp")`; 5 bairro → estado bairro + `Start("NHood", "exp")`; 6 → estado 0 (silêncio); se o estado atual não é o do lote, compra/construção só ficam enfileirados (`QueueMusicState`);
- mensagem `0xB4` (bairro sem lote): para tudo, bairro + `Start("NHood")`; `0xB6`: a música de abertura (`Res/Sound/Music/Splash/`, instância `+0x8c`) **some em 6000 ms** e começa a do bairro;
- `0x2BEA2063` (ir para um lote): lote `CAS!`/`YACAS!` → estado CAS e `Start("CAS")` se nada toca; outro lote → o estado lote fica como atual sem trocar o que toca (a música do bairro segue durante a carga); `0xB3` marca e `0x4C8E7905` (o lote ficou pronto) faz a instância `+0x8c` sumir em **6 s** e troca para o estado lote.

`_Start(instância, sala, nível, tipo, qualidade)` (BV `0x612f28`): a playlist da pasta do tipo (`musiccategories`); com instância ≠ 0 (um objeto) a música toca **posicionada** no tile/nível e na direção do objeto (senão é 2D); qualidade **`cheap` → passa-baixa 0x2134 (8500) e passa-alta 0xDAC (3500)**, outra (`exp`) sem filtro (0xFFFF/0). Os eventos de aparelho de som (`_HandleStartStereoEvent`, BV `0x613920`) leem do nome tipo e qualidade e aceitam comandos `volume`, `lopass`, `hipass` (com o valor do evento) e `wall` (acha o alto-falante de parede, `_FindWallSpeakerSource`); sala e nível vêm das chaves `0x0C683D79`/`0x0C683D7D` do evento. Constantes do TrackLogic usadas: `defaultmusicvolume`, `musicpausefadetime`, `musicmin/maxdistance`, `musicpriority`, `music/vox/sfxdippingvolume` (abaixar a música durante falas/efeitos; não seguido).

### Som da TV (`cVideoBroadcastChannel`)
O canal abre o filme (decodificador `0x8C361FD8` se o arquivo começa com `MAD`, senão `0x4C574D00`) e liga a saída de som do decodificador a um **proxy** (`cVideoBroadcastSoundEmitterProxy`) cujo emissor é o som posicionado do **primeiro receptor** do canal (`GetMasterSoundEmmiterForReceivers`, BV `0xd8d214`: o emissor do objeto com o ID do primeiro receptor no mapa): todas as TVs no mesmo canal mostram o mesmo quadro, mas **o som sai de uma só** (a de menor ID). Ao começar (`Start`, BV `0xd8e63c`) manda o evento `0x2D41D87E` (`0x0BD63077` = ID do canal, `0x2D41D97A` = nome). Ordem (`ChooseNextMovieKey`, BV `0xd8e2ac`): a lista de `+0x38` em **ordem** (índice que dá a volta), depois um item **sorteado** da lista de `+0x44` sem repetir o último, e assim alternando; `IsGames` toca o `File` fixo. As listas são as chaves dos grupos da pasta e da pasta de comerciais (`InitializeBroadcastKeyLists`); qual é qual é **inferido** (`+0x38` = programas, `+0x44` = comerciais), e o `CommercialInterval` não é lido nesse caminho: o comercial entra entre cada dois programas. O opensims2 (`app/tv_broadcast`) sorteia os programas e põe comercial a cada 60 s, sem som.

## Rodada 11: filtros dos aparelhos, abaixamento da música, sons do CAS
- **CAS**: os botões tocam o `btnclicksnd` do script (`ui_primary_button`, `ui_secondary_button`, `ui_tertiary_button`, `ui_button_zoom_in/out`), as células das listas `ui_secondary_button` e a barra de personalidade `ui_cas_personality<one..ten>` pelo ponto clicado. `ui_cas_randomize_mup` não existe no jogo; `ui_cas_camera` fica sem uso (nenhum script o nomeia). `--cas-click x,y` (com `--screenshot`) simula o clique.
- **Filtros** (`AudioMixer::Params::lopass/hipass`, biquad Butterworth por voz): o aparelho `cheap` toca com passa-baixa 8500 Hz e passa-alta 3500 Hz (BV `_Start`). Os eventos de aparelho são `stereo_<estação>` (a Sound ID Table dos 8 aparelhos só lista isso, mais `stereo_wall_speaker`); `parseStereoEvent` lê, depois da estação, `cheap`/`chp`/`expensive`/`exp`, `wall`, `lopass<Hz>`, `hipass<Hz>`, `volume<0..100>` (a grafia é inferida: nenhum evento do jogo base usa os comandos; a qualidade também vem do `objectquality` `chp` do soundinfo se o objeto tiver). Caixa de parede (`stereo_wall_speaker`): toca a estação do aparelho mais próximo (mesmo andar), em outra posição, e cala com ele (inferido do nome `_FindWallSpeakerSource`; as músicas não ficam em sincronia). O volume do Play Sound também vale para o aparelho.
- **Abaixamento** (`TrackLogic`: `musicdippingvolume` 0,1, `dippingstarttime` 0,5, `dippingstoptime` 5; `sfxdippingvolume` 0,2 e `voxdippingvolume` 0,3 ficam sem uso): enquanto há voz do grupo Vox tocando, o fator da música (`AudioMixer::setGroupDuck`, separado do volume das opções) cai a 0,1 em 0,5 s e volta em 5 s depois que a fala acaba (`stepDuck`). A leitura "cai *para* 0,1" é inferida.
