#!/usr/bin/env bash
# End-to-end checks against a running cluster.
#
#   DC_COORDINATOR=host:port                                coordinator address
#   DC_PSQL="host=... dbname=distcompile user=postgres"     for checking invariants in the database
#
# Run the coordinator with a short lease (--lease 3) or the crash test waits
# for the default 20 s. dcc, dc-worker, psql, make, gcc and python3 must be
# on PATH. The script starts its own crashing and flaky workers next to the
# normal ones.
set -euo pipefail
: "${DC_COORDINATOR:?}" "${DC_PSQL:?}"
here=$(cd "$(dirname "$0")/.." && pwd)
work=$(mktemp -d)
extra=()
trap 'for p in "${extra[@]}"; do kill "$p" 2>/dev/null || true; done' EXIT

pass() { echo "ok   $*"; }
fail() { echo "FAIL $*"; exit 1; }
q() { psql "$DC_PSQL" -tAc "$1"; }
field() { python3 -c "import json,sys; print(json.load(open(sys.argv[1]))[sys.argv[2]])" "$1" "$2"; }
build() { dcc build -C "$1" --json "$2" > "$2.log" 2>&1; }

# 1. the example builds and runs
cp -r "$here/examples/calc" "$work/calc"
rm -rf "$work/calc/out" "$work/calc/.dcstate"
build "$work/calc" "$work/1.json" || { cat "$work/1.json.log"; fail "example build"; }
out=$("$work/calc/out/calc")
[[ "$out" == *"ipow(3, 4) = 81"* && "$out" == *"upper = ELIPMOCTSID, 2 x 'I'"* ]] || fail "calc printed: $out"
pass "example builds on the cluster and runs"

# 2. a no-op rebuild does no work at all
build "$work/calc" "$work/2.json"
[[ $(field "$work/2.json" cached) == $(field "$work/2.json" tasks) ]] || fail "rebuild wasn't fully cached"
[[ $(field "$work/2.json" preprocessed) == 0 ]] || fail "rebuild ran the preprocessor"
pass "no-op rebuild: every task from cache, preprocessor not run"

# 3. same sources, different directory, no local state: the shared cache still hits
cp -r "$here/examples/calc" "$work/elsewhere"
rm -rf "$work/elsewhere/out" "$work/elsewhere/.dcstate"
build "$work/elsewhere" "$work/3.json"
[[ $(field "$work/3.json" compiled) == 0 ]] || fail "fresh checkout recompiled"
pass "fresh checkout in another directory: 0 compiles"

# 4. a compile error fails the build once (no retries) and shows the compiler output
cp "$work/calc/src/mathx/add.c" "$work/add.c.orig"
echo "int broken(" >> "$work/calc/src/mathx/add.c"
if build "$work/calc" "$work/4.json"; then fail "broken code built"; fi
grep -q "error" "$work/4.json.log" || fail "compiler message missing"
[[ $(q "SELECT max(attempts) FROM tasks WHERE state = 'failed'") == 1 ]] || fail "compile error was retried"
cp "$work/add.c.orig" "$work/calc/src/mathx/add.c"
build "$work/calc" "$work/4b.json" || fail "fixed code didn't build"
pass "compile error: reported with gcc's message, not retried; fix builds"

# 5. a worker dies while holding a task: its lease runs out and the task runs elsewhere
python3 "$here/tools/gen_project.py" --units 40 --libs 2 --seed 11 --out "$work/p5" > /dev/null
dc-worker --coordinator "$DC_COORDINATOR" --id crashy --slots 1 --crash-after 2 > /dev/null 2>&1 &
extra+=($!)
sleep 1
build "$work/p5" "$work/5.json" || fail "build with a crashing worker"
b5=$(field "$work/5.json" build_id)
retried=$(q "SELECT count(*) FROM tasks WHERE build_id = $b5 AND attempts > 1")
[[ $retried -ge 1 ]] || fail "expected the crashed task to be retried"
make -s -C "$work/p5" > /dev/null
[[ $("$work/p5/out/app") == $("$work/p5/build/app") ]] || fail "p5: dcc and make binaries differ"
pass "worker crash mid-task: $retried task(s) re-ran after lease expiry, output matches make"

# 6. flaky workers: half of their tasks fail with infrastructure errors
python3 "$here/tools/gen_project.py" --units 60 --libs 3 --seed 12 --out "$work/p6" > /dev/null
dc-worker --coordinator "$DC_COORDINATOR" --id flaky --slots 2 --fail-rate 0.5 > /dev/null 2>&1 &
flaky=$!
extra+=($flaky)
build "$work/p6" "$work/6.json" || fail "build with a flaky worker"
kill $flaky
b6=$(field "$work/6.json" build_id)
retried=$(q "SELECT count(*) FROM tasks WHERE build_id = $b6 AND attempts > 1")
make -s -C "$work/p6" > /dev/null
[[ $("$work/p6/out/app") == $("$work/p6/build/app") ]] || fail "p6: dcc and make binaries differ"
pass "flaky worker (50% failures): build ok, $retried task(s) retried, output matches make"

# 7. four clients at once on a project nobody has built: everything races for the same rows
python3 "$here/tools/gen_project.py" --units 80 --libs 4 --seed 13 --out "$work/p7" > /dev/null
for i in 1 2 3 4; do cp -r "$work/p7" "$work/c$i"; done
pids=()
for i in 1 2 3 4; do build "$work/c$i" "$work/7-$i.json" & pids+=($!); done
for p in "${pids[@]}"; do wait "$p" || fail "a concurrent build failed"; done
ids=$(for i in 1 2 3 4; do field "$work/7-$i.json" build_id; done | paste -sd, -)
[[ $(q "SELECT count(*) FROM builds WHERE id IN ($ids) AND state = 'done'") == 4 ]] || fail "not all builds done"
[[ $(q "SELECT count(*) FROM tasks WHERE build_id IN ($ids) AND state NOT IN ('done', 'cached')") == 0 ]] ||
  fail "unfinished tasks left behind"
# with no failures injected, a task that ran twice would mean it was handed out twice
[[ $(q "SELECT count(*) FROM tasks WHERE build_id IN ($ids) AND attempts > 1") == 0 ]] ||
  fail "some task was claimed more than once"
sums=$(for i in 1 2 3 4; do "$work/c$i/out/app"; done | sort -u | wc -l)
[[ $sums == 1 ]] || fail "concurrent builds produced different binaries"
pass "4 concurrent builds: all done, no task claimed twice, identical outputs"

echo "all e2e checks passed"
