# Design

```
                 gRPC                                gRPC
  dcc (client) ───────────►  dc-coordinator  ◄─────────────  dc-worker × N
  preprocess, hash,          │    │                           claim, fetch inputs,
  upload, submit, wait       │    └── blob store (disk)       run gcc/ar, upload output
                             └─────── PostgreSQL: builds, tasks, deps, action cache, workers
```

Three binaries. The coordinator is the only one that talks to Postgres and
owns the blob store. Workers and clients only speak gRPC.

## Life of a build

1. **Read DCBUILD.** It lists libraries and binaries and their sources, and
   becomes a graph: one compile task per distinct source, one archive task
   per library, one link task per binary.
2. **Keys.** For each source, `dcc` either reuses the key from last time
   (nothing it depends on changed, see below) or runs `gcc -E` and hashes
   the result.
3. **`CheckCache(keys)`.** The coordinator answers with the keys it already
   has results for. Those units need no upload and no compile.
4. **Upload.** For the rest, `FindMissing(digests)` returns which
   preprocessed sources the coordinator doesn't have yet, and `PutBlobs`
   sends them in batches.
5. **`SubmitBuild(graph)`.** In one transaction the coordinator marks cached
   tasks done, works out which archive/link tasks are now satisfied from the
   cache as well, and inserts everything else as `ready` or `waiting`.
6. **Workers claim, run, complete.** Each completion counts down its
   dependents. A dependent that reaches zero gets its own key computed from
   its inputs' digests: a cache hit finishes it immediately, a miss makes it
   `ready`.
7. **`dcc` polls `GetBuild`**, then downloads the archives and binaries it
   doesn't already have (compared by hash) into `out/`.

## Content addressing

Every blob (preprocessed source, object, archive, binary) is stored under the
sha256 of its bytes: `cas/ab/cdef...`. Two consequences carry most of the
design:

- **Writes can't conflict.** Two workers uploading the same object write the
  same bytes to the same name. Files are written to a temp name and renamed,
  so readers never see half a file. `PutBlobs` rejects a blob whose bytes
  don't match its claimed hash.
- **Doing something twice is harmless.** Running a task again produces a
  blob with the same name. That is what makes retries and duplicate
  completions safe (see below).

**The action cache** is a table `key -> output digest`. A key describes
everything that determines the output:

| task | key = sha256 of |
|---|---|
| compile | toolchain id, language, compile flags (not `-I`/`-D`: preprocessing already applied them), preprocessed text |
| archive / link | kind, command args, and each input's file name + digest, in order |

The toolchain id is the first line of `gcc --version` plus the target triple.
A worker only claims tasks whose toolchain matches its own, so an object built
by gcc 13 never lands in a gcc 12 build.

**Normalizing the text (C only).** `gcc -E` output has line markers
(`# 12 "foo.h" 2`) and a blank line for every comment or directive. Neither
changes the object file unless `-g` is on. For C, the key is computed with both
removed, so adding a comment to `common.h` doesn't recompile the 121 files that
include it. The worker still compiles the original text, so warnings point at
the right lines. C++ keeps the exact text: a raw string literal can span
lines, so a blank line (or one that looks like a marker) can be part of a
token, and dropping it could give two different programs the same key. A wrong
cache hit means a wrong binary, so for C++ correctness wins.

## Incremental invalidation

Two levels of "skip", cheapest first:

1. **Skip the preprocessor.** `.dcstate` stores, per unit, its key plus the
   `(mtime, size)` of every file the preprocessor read. The list comes from
   the line markers, so it includes every header, even system ones. If none
   of them changed, the old key is still right and `gcc -E` doesn't run. When
   a header changes, only the units that actually included it are redone.
2. **Skip the compiler.** A unit that was redone but produces the same
   normalized text gets the same key and hits the cache. This catches what
   timestamps can't: touched files, reverted edits, branch switches back to
   something already built, comment-only changes, a macro nobody uses.

make only has level 1, and without the content check behind it. That's the
whole difference measured in [performance.md](performance.md).

## Scheduling

**Priorities: upward rank.** Each task's rank is its own cost plus the
highest rank among the tasks waiting on it. That's the length of the longest
chain from it to the end of the build (the critical path, as in HEFT). Cost is
the preprocessed size for compiles and a constant for archive/link. Claims take
the highest rank first. Objects that feed an archive that feeds the link rank
above objects that go straight into the link, and big files start early. Both
help avoid a build whose tail is one worker compiling one big file while the
rest sit idle.

**Partitions: locality.** The graph is split into as many groups as there are
live workers. Each archive goes with the objects that go into it, and groups
are balanced by cost (longest-processing-time first: biggest group to the
lightest partition). Each worker has a partition and prefers tasks from it, so
the worker that compiled a library's objects usually archives them from its own
local cache, without downloading anything. In a 600-file build, cross-worker
object fetches dropped from 393 to 115.

**Pull, with stealing.** Workers ask for work (`ClaimTask`, long-polling up
to 2 s when idle) instead of being assigned it. A worker whose partition has
nothing ready takes the best task from any partition. A slow or dead worker
therefore only delays what it's actually holding.

The claim itself is the classic Postgres queue:

```sql
UPDATE tasks SET state = 'running', lease_owner = $1, attempts = attempts + 1, lease_until = now() + ...
WHERE uid = (SELECT uid FROM tasks
             WHERE state = 'ready' AND toolchain = $2 AND part = $3
             ORDER BY priority DESC LIMIT 1
             FOR UPDATE SKIP LOCKED)
RETURNING ...
```

`FOR UPDATE` locks the chosen row, and `SKIP LOCKED` makes a concurrent claimer
skip rows someone else is taking instead of waiting. So many claimers run at
once and a row is never handed out twice. A partial index on
`(toolchain, part, priority DESC) WHERE state = 'ready'` makes it an index scan.

## Retry safety

What can go wrong, and what happens:

| failure | handling |
|---|---|
| worker dies mid-task | Its lease (default 20 s, renewed by heartbeats every 3 s) runs out. A reaper thread puts the task back to `ready` and someone else runs it |
| worker hits an infrastructure error (fetch/upload failed, disk) | It reports `retryable`, and the task goes back to `ready` with `attempts` kept |
| same task finished twice (slow worker came back after its lease expired) | `UPDATE ... WHERE uid = $1 AND state IN ('ready','running')`: only the first completion matches. The second gets `accepted = false` and is dropped, and both produced the same blob anyway |
| a worker that fails instantly keeps grabbing its own failures | That worker skips a task it just failed for 3 s |
| real compile error | Not retried. The task fails, the build fails, and `dcc` prints gcc's output |
| too many attempts (default 3) | The task fails with the reason in its log |
| coordinator unreachable | All RPCs retry with exponential backoff on `UNAVAILABLE`/`DEADLINE_EXCEEDED`. Every call is safe to repeat |
| Postgres restarts | Pool connections reconnect, and tasks in flight fall back to lease expiry |

## Concurrent metadata access

Many gRPC threads, each borrowing a connection from a fixed pool, all run
transactions on the same tables:

- **Counting down dependents.** When 250 objects of one library finish at
  about the same time, they all decrement the same archive row. Each
  transaction takes that row's lock, and exactly one of them sees the count hit
  zero (`RETURNING pending_deps`), so exactly one makes the archive ready.
- **No deadlocks by construction.** A completion that has to update several
  dependent rows locks them first with `SELECT ... ORDER BY id FOR UPDATE`, so
  everyone takes locks in the same order. Any transaction that still hits
  `40P01`/`40001` is rolled back and re-run by `transaction()` in `db.h`.
- **Cache writes** are `INSERT ... ON CONFLICT (key) DO NOTHING`: two workers
  finishing the same key is fine.
- **Async commit.** The coordinator's sessions use
  `synchronous_commit = off`. If Postgres crashes, the last few hundred ms of
  commits can be lost. Those tasks go back to an earlier state and run again,
  and since outputs are content-addressed the rerun produces identical bytes.
  So a lost commit costs a recompile, never a wrong result. `--durable` turns
  it off.

## What's deliberately simple

- One coordinator, no failover. It's stateless apart from Postgres and the blob
  directory, so a restart is fine, but there's only one.
- The blob store is a local directory with no eviction (a real one would do
  LRU by the `cache.hits` / timestamps it already records).
- No authentication. The coordinator tells workers what to run, so workers
  only execute an allowlisted set of compiler drivers plus `ar`. Run it on a
  network you trust, like distcc.
- Client and workers must have the same compiler version. They don't
  sandbox it or ship it.
- Preprocessing happens on the client (like classic distcc), which needs the
  headers. Shipping headers to workers (distcc "pump" mode, Bazel remote
  execution) would move that work off the client.
