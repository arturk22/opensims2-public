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
2. Aliases (property set `aliases`, 121 chaves) redirecionam o nome composto: `foot_run_cpet_leath` → `foot_run_cpet_rub`, e o `foot_step` sem modificadores → `foot_step_cpet_bare`. Se a track não existe, os sufixos caem do fim. Alguns objetos pedem tracks que o jogo base não tem (`fridge_idle_run_lp_exp`; o jogo mostra "Missing sound info table entry").
3. Uma amostra da hit list, sorteada (a escolha do jogo não foi decodificada).

Os soundinfo (STR# 0x94) têm 4 itens: superfície que o objeto é/oferece (`dest`/`destination` = nenhum), superfície do objeto segurado (`source` = nenhum), qualidade e partição de ambiência (`flora`, `object:tombstone`).

## Ambiência
Property sets com `Name` (17 camadas, listadas pelo `LayerIds` das configurações de ambiência): `EnabledPrimitiveIds` (booleanos `islot`/`isneighborhood` e valores como `lightofday` = hora/24 passados pela curva nomeada logo depois, p. ex. `curveismorning` = 6h–12h), partições `AND`/`NOT` (`isflora`, `iswater`, `isinside`, `istombstone`), um laço (`AmbientLoopName`: `lot_outside_lp`, `nite_lp`, `water_lp`, `frog_nite_lp`) e/ou uma sequência FSC (`FreshnessScoreName` = nome do recurso de texto FSC `version 7`: `midday`, `morning`, `night`, `dusk`, `frogs`, `owls`, `empty_day`, `cemetary_ghost`, ...), com volume pelos pares valor/curva de `LoopVolumePrimitives`/`FreshnessVolumePrimitives` (`outsideness`, `zoom`). FSC: cabeçalho (volume, tempo em BPM) e notas de 20 colunas (volume /1024, "rand" = início de segmento para onde o tocador pula, pan /1024, duração em batidas, probabilidade em 16, amostra `*.xa`), como o `FSC.cs`/`FSCPlayer.cs` da FreeSO. `AmbiencePlayer` toca isso com a hora do relógio da VM, a parcela ao ar livre (quartos de tile sem sala) num raio de `LotAmbienceZoomRadiusCurve` (20 tiles longe, 8 perto) em volta do pivô da câmera e o zoom. Inferido: partições como flags do lote inteiro (no jogo são áreas: `LotPartitionSize 3`), `NOT isinside` só desliga quando nada ao redor é ao ar livre, pitch das notas FSC ignorado.

## Música
Categorias no property set `musiccategories` (`Build`, `Buy`, `CAS`, `Nhood`, `hiphop`, `metal`, `pop`, `rnb`, `salsa`, `techno`) = pastas `Music/<categoria>` (MP3). `MusicPlayer` toca as músicas da categoria em ordem embaralhada, volume 0,7 e fade de saída de 3 s (`defaultmusicvolume`, `trackfadeouttime` das configurações do TrackLogic). Os aparelhos de som tocam eventos `stereo_<estação>` (`stereo_pop`, ...), que não são tracks: tocam a categoria de mesmo nome a partir do objeto (inferido; `stereo_classical` não tem músicas no jogo base).

## No app
`--no-audio` desliga, `--mute` deixa mudo; com `--screenshot` nenhum dispositivo é aberto e `--sound-mix out.wav` grava o que teria tocado (mixer renderizado junto com os ticks da VM). `--music <categoria>` toca música de fundo. O log de saída resume: sons da VM, eventos de animação, nomes tocados e que faltaram, camadas de ambiência ativas e a hora.
