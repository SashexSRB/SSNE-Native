#!/bin/sh
# Lists how the compiler of a configured build directory names and hashes the runtime's functions for PGO
# ("name hash counters" per line), for tools/pgo_retarget.py. Each runtime source is compiled once more with the
# build's own command, instrumented, to unoptimized IR (nothing is written to the build directory).
#
#   tools/pgo_target_names.sh <build dir> > names.txt
set -e
B=$1
[ -f "$B/build.ninja" ] || { echo "usage: $0 <build dir configured with Ninja>" >&2; exit 1; }
ninja -C "$B" -t commands ssne-runtime | grep -E -- " -c [^ ]*runtime/src/[^ ]*\.cpp" | while read -r cmd; do
  # Same command, output to stdout, no dependency file, without PGO options of an earlier configuration.
  cmd=$(printf '%s\n' "$cmd" | sed -E 's/ -o [^ ]+/ -o -/; s/ -MD -MT [^ ]+ -MF [^ ]+//; s/ -fprofile-instr-(use=[^ ]+|generate)//g')
  (cd "$B" && eval "$cmd -fprofile-instr-generate -emit-llvm -S -Xclang -disable-llvm-passes -w") |
    awk '
      # @__profn_<var> = private constant [N x i8] c"<profile name>"
      /^@__profn_/ { v = $1; s = $0; sub(/^[^"]*c"/, "", s); sub(/"$/, "", s); name[v] = s }
      # call void @llvm.instrprof.increment(<ptr to @__profn_var>, i64 <hash>, i32 <counters>, i32 <index>)
      # (opaque pointers: "ptr @__profn_x"; clang 14: "i8* getelementptr (..., [N x i8]* @__profn_x, i32 0, i32 0)")
      /llvm\.instrprof\.increment\(/ {
        if (!match($0, /@__profn_[^ ,)]+/)) next
        v = substr($0, RSTART, RLENGTH)
        if (!match($0, /i64 -?[0-9]+, i32 [0-9]+, i32 [0-9]+\)/)) next
        split(substr($0, RSTART, RLENGTH), p, ", ")
        h = p[1]; sub(/^i64 /, "", h); n = p[2]; sub(/^i32 /, "", n)
        if (!(v in hash)) { hash[v] = h; cnt[v] = n }
      }
      END { for (v in hash) if (v in name) print name[v], hash[v], cnt[v] }'
done | sort -u
