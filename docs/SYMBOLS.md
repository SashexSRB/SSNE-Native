# Function naming (symbols)

`data/symbols/G3BE9G_r1.sym` names **608 of the 7,921 functions** in the USA Rev 1 `main.dol`, including every
Dolphin SDK function that the next milestones hook (threads, alarms, ARAM, DVD, VI, PAD, GX, CARD). The recompiler
uses it (`--map`) to:
* give generated functions readable names (`f_801C31B4_ARInit`), which also makes backtraces readable
* add function starts that heuristics missed
* install native replacements by name (`runtime/src/hle.cpp`)

You don't need to regenerate it. It's committed. Rerun `ssne-symbols` only for a new disc revision/region, or to
pick up improvements.

## How names are found
| Source | Count | Method |
|---|---|---|
| `manual` | 4 | Identified by hand during bring-up |
| `dsy` | 344 | **Dolphin's signature database** (`Sys/totaldb.dsy` in any Dolphin install). Same opcode-only hash as Dolphin's `HashSignatureDB`. Only accepts hashes that are unique in this DOL and ≥16 bytes (≥64 bytes in game code), and names that fit the neighbouring library family |
| `ref` | 213 | **Reference layouts** from decompilation projects (Wind Waker, Pikmin 2, Melee, Sunshine, MKDD). SDK objects link in a fixed order, so the layout between two known functions often matches. Three rules: equal-distance alignment, outward walks with exact size checks, and "sandwich" (one function between known neighbours, size ±25%). Every reference must agree, and the library-family check applies |
| `string` | 47 | Functions that print their own name (`"CcEngine::Render ..."`, `"Name(): ..."`) |

Validation: the names were cross-checked against the call graph (e.g. `GXInit` calls `GXInitFifoBase` and
`__GXFifoInit`; `OSCreateThread` calls `OSInitContext` and has the reference size exactly). Of the 459 SDK-prefixed
names, only 2 call into an unexpected library, and both are legitimate (`OSInit`, `OSResetSystem`).

Notable: **`OSMutex.c` and `OSMessage.c` are not linked** in this game (no SDK mutexes or message queues). `DVDOpen` is also
absent (the game uses `DVDFastOpen`).

## Regenerating
```sh
# Reference symbol tables (sparse clones, a few MB):
for r in zeldaret/tww projectPiki/pikmin2 doldecomp/melee doldecomp/sms doldecomp/mkdd; do
  git clone --depth 1 --filter=blob:none --sparse https://github.com/$r refs/$(basename $r)
  git -C refs/$(basename $r) sparse-checkout set --no-cone '/config/*/symbols.txt'
done

ssne-symbols game_data/sys/main.dol data/symbols/G3BE9G_r1.sym \
  --dsy "C:/Program Files/Dolphin/Sys/totaldb.dsy" \
  --manual data/symbols/G3BE9G_r1.sym --sdk-start 801B0000 \
  $(for f in refs/*/config/*/symbols.txt; do echo --ref $f; done)
```
Lines tagged `manual` (or with no tag) survive regeneration. Add hand-identified names that way.
