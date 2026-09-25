# DistCompile

A small distributed build engine for C/C++. A coordinator splits a build's
dependency graph across workers over gRPC, keeps the scheduling state in
PostgreSQL, caches every result by content hash, and only recompiles what
actually changed.

```
  dcc (client) ──gRPC──►  dc-coordinator  ◄──gRPC──  dc-worker × N
  preprocess, hash,        │    └─ blob store        claim, fetch, gcc/ar,
  upload, submit           └────── PostgreSQL        upload result
```

It's hackathon-sized (~3k lines of C++), not Bazel. It does the parts that make
distributed builds interesting, and each is small enough to read.

- **Dependency-aware scheduling.** Compile, archive, and link run as a DAG.
  Tasks on the critical path go first (upward rank), and the graph is
  partitioned so the worker that compiled a library's objects usually
  archives them too.
- **Content-addressed caching.** Compile results are keyed by
  hash(preprocessed source + flags + toolchain), and archive/link by the hashes
  of their inputs. Anything built once, by anyone, is never built again.
- **Incremental invalidation.** The client remembers which headers each file
  read and skips the preprocessor when none changed. When something did
  change but the preprocessed text is the same (comment edits, reverts, branch
  switches, `touch`), the cache catches it.
- **Retry-safe execution.** Leases with heartbeats and a reaper, idempotent
  completions, compile errors that aren't retried, and a flaky worker kept off
  its own failures.
- **Concurrent metadata.** Many gRPC threads on a Postgres pool: job claims
  with `FOR UPDATE SKIP LOCKED`, row-locked countdowns, a consistent lock
  order, and deadlock retry.

## Run it

Needs Docker.

```bash
docker compose up -d --build                                   # postgres + coordinator + 3 workers
docker compose run --rm client dcc build -C examples/calc
./examples/calc/out/calc
docker compose run --rm client dcc build -C examples/calc      # again: 9/9 tasks from cache
docker compose run --rm client dcc stats                       # coordinator profile
```

```
dcc: 6 units, 6 preprocessed, 0 reused unchanged
     9 tasks: 0 from cache, ran 6 compile / 2 archive / 1 link on 3 worker(s)
     uploaded 6 blobs (117.9 KB), downloaded 23.4 KB
     keys 44 ms, upload 5 ms, remote 114 ms, fetch 49 ms, total 241 ms
```

A project is described by a `DCBUILD` file:

```
cc      gcc
cflags  -O2 -Wall -Iinclude
ldflags -lm
lib mathx src/mathx/*.c
lib strx  src/strx/*.c
bin calc  src/main.c : mathx strx
```

Building without Docker: install `libgrpc++-dev protobuf-compiler-grpc
libprotobuf-dev libpq-dev libssl-dev` (Ubuntu 24.04), then
`cmake -S . -B build && cmake --build build`. Start `dc-coordinator --db "<libpq
conninfo>"`, one or more `dc-worker --coordinator host:7070`, and run `dcc build`.

## Numbers

Details and method: [docs/performance.md](docs/performance.md).

- **Compile work.** A fixed 20-step edit workload on a 121-file project,
  built after every step with both make (`-MMD` dependencies) and dcc, with
  both binaries checked to print the same checksum. make ran the compiler
  1,235 times and dcc 194 (84% fewer). On steps with new code the two do the
  same work. The savings are all repeats: reverts, branch switches, touched
  files, clean CI checkouts, comment-only edits.
- **Coordinator profiling** on a 2,001-file build: batching the submit
  (539 -> 45 ms), index-backed claims (2.2 -> 0.7 ms), dropping an O(n²)
  progress check, and async commit brought `CompleteTask` from 4.0 to 1.8 ms
  and the remote phase from 14.9 to 12.7 s.
- **Locality.** Partitioning cut cross-worker object downloads from 393 to 115
  on a 600-file build.

## Tests

- `build/unit_tests`: hashing, blob store (including 8 concurrent writers),
  key normalization, manifest parsing, DAG ordering/rank/partitioning, SQL
  array encoding, subprocess handling.
- `tests/e2e.sh`, against a live cluster (CI runs it on every push with a
  real Postgres):
  - the example builds and runs
  - a no-op rebuild does nothing
  - a fresh checkout elsewhere compiles nothing
  - a compile error fails once and shows gcc's output
  - a worker killed mid-task is recovered by lease expiry
  - a worker failing 50% of its tasks doesn't break the build
  - 4 concurrent builds finish with no task claimed twice

  Every build that runs through failures is checked against a make build of
  the same tree.

```bash
COORDINATOR_ARGS="--lease 3" docker compose up -d
docker compose run --rm client bash tests/e2e.sh
```

Testing hooks: `dc-worker --crash-after N` (die while holding the Nth task) and
`--fail-rate P` (report a fraction of tasks as infrastructure failures).

## Layout

```
proto/distcompile.proto   the gRPC API
sql/schema.sql            tables and indexes (applied by the coordinator at startup)
src/coordinator.cpp       scheduling, cache, leases, all the SQL
src/worker.cpp            claim -> fetch -> run -> upload -> complete
src/dcc.cpp               client: keys, incremental state, upload, submit, collect
src/preprocess.cpp        gcc -E, header deps from line markers, cache keys
src/graph.cpp             topological order, upward rank, partitioning
src/db.cpp, cas.cpp       libpq pool + transactions, content-addressed blob store
tools/                    project generator, make-vs-dcc workload, gcc wrapper that counts
tests/                    unit tests and the e2e script
docs/                     design.md (how it works and why), performance.md (measurements)
```

## Limits

One coordinator (restartable, but not replicated). A local blob store with no
eviction. No auth, so workers run only allowlisted compiler drivers plus `ar`;
use it on a trusted network, like distcc. Client and workers must have the
same compiler version. Preprocessing happens on the client. All of it is
discussed in [docs/design.md](docs/design.md).

MIT licensed.
