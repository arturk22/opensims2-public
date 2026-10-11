# `tools/symxfer`: nomes do `baserom.sym` no binário de Mac

O executável ppc do Bon Voyage 1.10 (`references/bv_mac/sims2_bv.ppc`) só
tem ~20 mil símbolos (externos e weak, todos no `__textcoal_nt`); as 33.101
funções do `__text` não têm nome. `references/sims2_mac_decomp/baserom.sym`
lista 82 mil funções de **outro** build ppc do mesmo port (dezembro de
2007), ligado de outro jeito (inlines no meio do objeto, objetos em outra
ordem) e com pequenas diferenças de código. O `__eh_frame` dá início e
tamanho exatos de cada função nossa; o `baserom.sym` dá a ordem e (pela
distância até o próximo símbolo) um tamanho máximo de cada uma das dele.
O `symxfer` casa as duas sequências de tamanhos e, desde a rodada 9, usa as
vtables (RTTI) e os nomes já postos como âncoras.

## 1. Como roda

```
REFS=<pasta references> tools/symxfer/run.sh mapa.txt     # ~2 min, imprime as métricas
tools/symxfer/apply_clone.sh <pasta> [mapa.txt]           # clone APFS de ~/sims2 com os nomes
```

O mapa (`endereço nome confiança`, ~5 MB) fica fora do repositório (nomes de
terceiros); `map_simantics.txt` é o recorte da VM que fica no repo.
`apply_clone.sh` copia `~/sims2.{gpr,rep}` com `cp -c` para uma pasta sua,
gera o mapa se não for dado e aplica com `ApplySymxferMap.java ... reset`:
os nomes que um mapa antigo tinha posto (comentário "symxfer: ...") e o novo
não dá voltam a `FUN_...`. Depois é usar essa pasta no lugar de `~` no
`analyzeHeadless` (sempre `-readOnly -noanalysis` para só ler). Atalho sem
gravar nada: no mesmo `analyzeHeadless -readOnly`, `-postScript
ApplySymxferMap.java mapa.txt reset -postScript DecompileList.java ...`
aplica os nomes só na memória e decompila com eles (~10 s).

## 2. O algoritmo

Estágio 1 (rodada 3, sem mudanças): por seção, âncoras (nomes iguais no
coal; 4 tamanhos seguidos únicos nos dois lados), cadeias de âncoras, DP
entre elas (pular, tamanhos próximos, várias funções num buraco sem FDE),
funções que mudaram de lugar dentro do objeto (`moved`) e thunks `addi
r3,r3,-k; b alvo` nomeados pelo alvo.

Estágio 2 (rodada 9, `--v1` desliga), repetido até parar de mudar (3 voltas):

1. **refill** (`symxfer.py`): os nomes já postos (todas as classes fortes,
   inclusive os das vtables) viram âncoras de uma entrada, junto com os
   4-gramas únicos de tamanho; cadeias valendo ≥ 3 (nome forte 1, `exact`
   0,5, um 4-grama de funções ≥ 64 bytes somando ≥ 800 bytes 3) são
   alinhadas pelo mesmo DP entre âncoras consecutivas e estendidas nas
   pontas. Isso alcança as ilhas que o estágio 1 jogava fora (cadeia de um
   4-grama só, como a do `UserAddPartitions`) e o código que mudou um pouco
   entre os builds. Um nome fraco (`near`/`moved`) fora da ordem entre duas
   âncoras próximas é tirado, e um nome fraco perde para um tamanho exato.
2. **between**: r funções sem nome cujos vizinhos com nome estão a r + 1
   posições no `baserom.sym`, com os r nomes do meio sem uso, recebem esses
   nomes um a um (r ≤ 3), qualquer que seja o tamanho.
3. **vtables** (`rtti.py`, `vtnames.py`): todas as typeinfos (par de
   palavras no `__DATA` cuja segunda aponta para um nome de tipo mangled) e
   vtables (deslocamento-para-o-topo, typeinfo, ponteiros para código) do
   binário: 6.671 typeinfos, 5.996 vtables, com as bases (`__si`/`__vmi`,
   com o deslocamento de cada base). Cada slot de vtable pertence a um
   grupo = (classe que introduziu o slot, índice); vtables secundárias
   entram pelo alvo dos thunks. Todas as funções de um grupo implementam o
   mesmo método virtual: o método e a assinatura vêm da maioria dos nomes do
   grupo, a classe vem da vtable que tem a função (a mais básica, se várias),
   e o nome derivado só é usado se existe no `baserom.sym` (`vtable`). Um
   grupo com um voto só vale se o nome que ele implica para outra função do
   grupo existe no `baserom.sym`. Os virtuais próprios de uma classe (grupo
   sem nome nenhum) recebem o único nome dessa classe no `baserom.sym` com o
   mesmo tamanho exato (`vtsize`).
4. thunks de novo (alvos novos), nomes manuais, e um nome usado duas vezes
   fica com a melhor classe.

## 3. Medidas (antes → depois da rodada 9)

As medidas são independentes de como os nomes foram postos
(`evalmap.py`, que o `run.sh` roda no fim):

- **thunks**: um thunk que ajusta `this` com alvo chamado `X::f` dá o nome
  `__ZThn<k>_X::f`; esse nome existir no `baserom.sym` confirma o nome do
  alvo (taxa por classe de confiança do alvo);
- **vtables**: um nome num slot é coerente se tem o método da maioria do
  grupo e a classe da vtable ou de um ancestral. Não vale para as classes
  `vtable`/`vtsize`, que saem disso (para elas, só os thunks).

| | rodada 3 | rodada 9 |
|---|---|---|
| funções do `__text` com nome | 22.463 / 33.101 (67,9%) | **27.300 (82,5%)** |
| idem, estimadas certas (Σ classe × taxa dos thunks) | ~19.200 | **~25.500** |
| `__textcoal_nt` com nome | 30.726 | 32.429 |
| nomes em trechos sem FDE do `__text` | 4.665 | 6.815 |
| total de nomes | 57.855 | 66.546 |

Confiança por classe (thunks; vtables entre parênteses):

| classe | rodada 3 | rodada 9 | n (`__text`) |
|---|---|---|---|
| `exact` | 91% (93%) | 95% (99%) | 17.914 |
| `gap` | 93% (99%) | 95% (99%) | — |
| `near` | 68% (59%) | 93% (86%) | 1.604 |
| `moved` | 50% (58%) | 66% (96%) | 425 |
| `vtable` | — | 95% | 1.923 |
| `vtsize` | — | 97% | 1.868 |
| `fill` | — | 92% (96%) | 2.173 |
| `fillnear` | — | 73% (78%) | 1.103 |
| `between` | — | 82% (100%) | 272 |

`exact` e `near` melhoraram porque os nomes errados nos slots de vtable
(a classe vizinha no objeto, comum em `AddRef`/`Release`/`QueryInterface`)
foram trocados pelo derivado da vtable e os `near`/`moved` fora de ordem
foram tirados. Recuperados como na rodada 8 (casados à mão então):
`UserAddPartitions` (BV `0x9aa90c`), o resto do `TSUserWallPlacement`
(`PlaceWallLine`, `TryPlacePartition`, `UserMayPlacePartition`...),
`CreateNeighborhoodLightMap` (`0x706920`).

Ainda errados conhecidos: `cTSUserToolAutoRoof::RemoveAllRoofs` em
`0x9a7384` (é `UserMayPlacePartitionCommon`, `near`), `AddSeed` do
`cRoomManager` em `0xc28ffc` com nome de telhado (`fillnear`) e `RefreshMap`
(`0xc2a36c`) sem nome (`docs/buy-build-mac.md` §7.3); classes `fillnear`,
`moved` e `between` pedem conferir pelo código. As ~5.800 funções sem nome
do `__text` são, na maioria, isoladas entre vizinhos com nome (código que
mudou entre os builds, ou que não existe no `baserom.sym`).

## 4. Ferramentas

- `symxfer.py`, `run.sh`, `apply_clone.sh`, `ApplySymxferMap.java`
  (`reset`, `labels-only`), `DecompileList.java`.
- `rtti.py <bin> [mapa] [--class 9cTSObject]`: typeinfos e vtables com o
  nome de cada slot; `vtables.py` (só uma classe, offsets para o
  decompilador) e `annotate.py` continuam.
- `evalmap.py <bin> <baserom.sym> <nm> <mapa>...`: as medidas acima;
  `EVAL_BAD=arquivo` lista os slots incoerentes.
- `manual.txt` e `paramvtables.py` como antes.
