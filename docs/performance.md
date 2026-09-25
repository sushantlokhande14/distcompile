# Performance

All numbers are from one laptop: Intel Core Ultra 9 185H (16 cores, 22
threads), Docker Desktop on WSL2, `docker compose` with Postgres 16, one
coordinator and 3 workers with 2 slots each. Projects come from
`tools/gen_project.py`. Cluster runs start from `docker compose down -v`, so
caches are cold.

## 1. How much compile work caching saves

`tools/workload.py` replays a fixed list of 20 edits on a 121-file project
(4 libraries + main). After each step it builds the same tree twice: with the
generated Makefile (GNU make, `gcc -MMD` header dependencies, the usual
incremental setup) and with `dcc`. It counts compiler runs and requires both
binaries to print the same checksum. The step list is in the script and was
fixed before any results existed.

```
python3 tools/gen_project.py --units 120 --libs 4 --out bench/proj
python3 tools/workload.py --project bench/proj --out docs/results
```

| step | make | dcc |
|---|---:|---:|
| initial clean build | 121 | 121 |
| edit one .c file | 1 | 1 |
| add a declaration to a library header | 31 | 31 |
| comment-only edit in a .c file | 1 | **0** |
| comment-only edit in common.h | 121 | **0** |
| revert the library header | 31 | **0** |
| switch to feature branch | 121 | 34 |
| edit two files on the branch | 2 | 2 |
| switch back to main | 36 | **0** |
| switch to feature again | 36 | **0** |
| touch every file (fresh checkout / restored CI cache) | 121 | **0** |
| clean build on a new machine | 121 | **0** |
| edit one .c file | 1 | 1 |
| unused macro added to common.h | 121 | **0** |
| declaration added to common.h | 121 | 121 |
| revert common.h | 121 | **0** |
| edit three .c files | 3 | 3 |
| revert those three | 3 | **0** |
| edit one .c file | 1 | 1 |
| switch to main | 121 | **0** |
| clean build on a new machine | 121 | **0** |
| **after the initial build** | **1235** | **194** |

**84.3% fewer compiler runs** than make over this workload (raw data:
[results/workload.json](results/workload.json)).

How to read that number:

- On steps that introduce code nobody has compiled before, dcc does exactly
  what make does (160 vs 160 over those 7 steps). A cache can't skip real
  work.
- The switch to the feature branch is mixed. Make recompiled 121 files,
  because every file main had changed since the branch point was rewritten.
  dcc compiled 34: the branch's genuinely new files.
- Every other step is a *repeat*: content that was compiled before, or text
  whose preprocessed form didn't change. Make recompiled 954 units there,
  dcc none.
- So across the run, dcc never compiled the same thing twice. The percentage
  depends entirely on how much of a real week looks like the repeat steps.
  Branch switching and CI checkouts are where it matters most.

The same effect on the example project, from `tests/e2e.sh`: a no-op rebuild
doesn't run the preprocessor, touching a header re-preprocesses only its
includers and compiles nothing, and a fresh checkout in another directory
compiles nothing.

## 2. Coordinator profiling

`dcc stats` prints the coordinator's counters: calls, total time and bytes per
RPC, per SQL statement (every query is tagged), for the blob store, and as
reported by workers (fetch/exec/upload). Profiling a clean 2001-file build
(`gen_project.py --units 2000 --libs 8 --seed 7`) turned up five problems:

| | before | after | fix |
|---|---:|---:|---|
| `SubmitBuild` | 539 ms | 45 ms | one INSERT per task and per edge (4,000+ round trips) became one `INSERT ... SELECT FROM unnest($arrays)` per table |
| claim query, avg | 2.24 ms | 0.70 ms | `ORDER BY (part = $3) DESC, priority DESC` can't use an index, so every claim sorted all ready tasks. It's now two index-backed queries (own partition, then any) on partial indexes |
| "is the build done?" checks | 2,011 | 2 | a `count(*)` over the build ran after every completion, O(n²) overall. Only sink tasks can finish last, so only they check |
| commit, avg | 1.26 ms | 0.18 ms | WAL fsync per commit. Async commit is safe here because a lost commit only means a rerun with identical output (see design.md) |
| `GetBuild` poll, avg | 4.7 ms | 1.7 ms | the client polled 10x/s and got every task row each time. Polls now return counts, and only the final call returns rows |
| `CompleteTask`, avg | 4.0 ms | 1.8 ms | (sum of the above) |
| remote phase of the build | 14.9 s | 12.7 s | |

The workers spent 63.6 s compiling in total, so with 6 slots the remote phase
can't go below ~10.6 s. Scheduling overhead on top of that went from ~4.3 s to
~2.1 s. The raw before/after profiles came from `dcc stats` on the same build
against a fresh cluster each time.

Client side of the same build: preprocessing is the biggest cost (2001 x
`gcc -E`, ~50 ms each, run on all local cores: 6 s wall). Serializing the
2010-task graph is 1.8 ms / 390 KB. Hashing is 3 ms per unit. Peak RSS is
about 180 MB, mostly the preprocessed text held for upload.

## 3. Partitioning and data locality

600-file build (`--units 600 --libs 6 --seed 21`), fresh cluster each time:

| | `--partitions 1` (off) | default (3) |
|---|---:|---:|
| blob downloads by workers | 994 | 716 |
| ...of which objects/archives from another worker | 393 | 115 |
| claims from the worker's own partition | 186 | 590 |
| worker fetch time, total | 954 ms | 596 ms |

Every compile has to download its preprocessed source (601 of the fetches
either way). What partitioning changes is the archive step: the worker that
compiled a library's objects usually archives them too, straight from its local
cache. Bytes barely change because objects are small next to preprocessed
sources.

## 4. Failure handling costs

From `tests/e2e.sh` (lease shortened to 3 s):

- a worker killed while holding a task: that task re-runs after the lease
  expires (~3 s later) and the build finishes with a binary identical to make's
- a worker that fails half its tasks: 9-13 tasks retried per 60-file build,
  output identical
- Postgres restarted under a 600-file build: 9 claims failed with
  `UNAVAILABLE` and were retried, and the build finished normally
