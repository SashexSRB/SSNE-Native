#!/bin/sh
# Converts the trained runtime profile (data/pgo/runtime.proftext) for the compiler of a configured build directory:
# its names and hashes (tools/pgo_target_names.sh) -> tools/pgo_retarget.py -> that compiler's llvm-profdata.
# Prints the path of the .profdata on success; on any problem (no clang, no llvm-profdata, no python3, nothing
# matched) it explains why on stderr and exits non-zero, and the caller builds without profile-guided optimization.
#
#   tools/pgo_build_profile.sh <build dir> <llvm-profdata> <out.profdata>
B=$1; PROFDATA=$2; OUT=$3
ROOT=$(cd "$(dirname "$0")/.." && pwd)
SRC=$ROOT/data/pgo/runtime.proftext
fail() { echo "PGO: $1, building without profile-guided optimization" >&2; exit 1; }
[ -f "$SRC" ] || fail "no trained profile ($SRC)"
grep -qs 'set(CMAKE_CXX_COMPILER_ID "Clang")' "$B"/CMakeFiles/*/CMakeCXXCompiler.cmake || fail "not a clang build"
command -v "$PROFDATA" >/dev/null 2>&1 || [ -x "$PROFDATA" ] || fail "llvm-profdata not found ($PROFDATA)"
command -v python3 >/dev/null 2>&1 || fail "python3 not found"
TMP=$(mktemp -d) || fail "no temporary directory"
trap 'rm -rf "$TMP"' EXIT
"$ROOT/tools/pgo_target_names.sh" "$B" > "$TMP/names.txt" 2> "$TMP/names.log" || { cat "$TMP/names.log" >&2; fail "listing the compiler's function names failed"; }
[ -s "$TMP/names.txt" ] || fail "the compiler listed no instrumented functions"
python3 -I "$ROOT/tools/pgo_retarget.py" "$SRC" "$TMP/names.txt" "$TMP/target.proftext" >&2 || fail "retargeting failed"
[ -s "$TMP/target.proftext" ] || fail "no profiled function matched this compiler"
"$PROFDATA" merge -o "$OUT" "$TMP/target.proftext" >&2 || fail "llvm-profdata merge failed"
echo "$OUT"
