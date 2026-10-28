#!/bin/sh
# Trains the runtime's profile for profile-guided optimization: data/pgo/runtime.proftext, which the release scripts
# convert for their compiler (tools/pgo_build_profile.sh). Re-run it after larger runtime changes (functions changed
# since training are simply not optimized with profile data).
#
# Builds the headless runner with an instrumented runtime (host clang) and runs it on the GPU renderer (headless EGL):
# boot with intros and menus, then gameplay from each given save state (F5 in the game, states/<disc id>/slotN.state),
# loaded 12 s after start and played for ~30 s with movement, firing and weapon switches, all runs in parallel.
# The profile holds only the runtime's function names and execution counts, nothing from the game.
#
#   tools/make_pgo_profile.sh <recompiled dir> <game_data dir> [save state ...]
set -e
REC=$1; DATA=$2
[ -d "$REC" ] && [ -d "$DATA" ] || { echo "usage: $0 <recompiled dir> <game_data dir> [save state ...]"; exit 1; }
shift 2
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BUILD=$ROOT/build-pgo-train
CXX=${CXX:-clang++}; CC=${CC:-clang}
PROFDATA=${LLVM_PROFDATA:-$(dirname "$(command -v "$CXX")")/llvm-profdata}
[ -x "$PROFDATA" ] || PROFDATA=llvm-profdata
REC=$(cd "$REC" && pwd); DATA=$(cd "$DATA" && pwd)

cmake -S "$ROOT" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER="$CC" -DCMAKE_CXX_COMPILER="$CXX" \
      -DSSNE_RECOMPILED_DIR="$REC" -DSSNE_BUILD_TESTS=OFF -DSSNE_BUILD_LAUNCHER=OFF -DSSNE_PGO_GENERATE=ON -DSSNE_PGO_PROFILE= >/dev/null
cmake --build "$BUILD" --target ssne-headless

RUN=$BUILD/train
rm -rf "$RUN"; mkdir -p "$RUN"
H=$BUILD/bin/ssne-headless
export LLVM_PROFILE_FILE="$RUN/%p.profraw"
echo "training: boot + menus, and $# save state(s); about 90 s"
"$H" --data "$DATA" --gpu --scale 1 --memcard "$RUN/card-boot.raw" --press A@60,A@64,A@68,A@72,A@76 --seconds 90 \
     > "$RUN/boot.log" 2>&1 &
n=0
for st in "$@"; do
  n=$((n + 1))
  "$H" --data "$DATA" --gpu --scale 1 --skip-intros --memcard "$RUN/card-$n.raw" --load-state "12:$st" \
       --press R@14:6,SLEFT@16:3,SRIGHT@20:3,SUP@22:4,X@24,R@25:3,X@28,R@29:3,Y@32,R@33:3,CLEFT@36:2 --seconds 42 \
       > "$RUN/state-$n.log" 2>&1 &
done
wait
ls "$RUN"/*.profraw >/dev/null 2>&1 || { echo "no profile written (see $RUN/*.log)"; exit 1; }
mkdir -p "$ROOT/data/pgo"
"$PROFDATA" merge --text -o "$ROOT/data/pgo/runtime.proftext" "$RUN"/*.profraw
echo "wrote data/pgo/runtime.proftext ($(grep -c '^# Func Hash:' "$ROOT/data/pgo/runtime.proftext") functions)"
