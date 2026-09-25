# DistCompile

A small distributed C/C++ build engine: a coordinator (gRPC + PostgreSQL)
splits a build's dependency graph across workers, caches every result by
content hash, and only recompiles what actually changed.

```bash
docker compose up -d --build                             # postgres, coordinator, 3 workers
docker compose run --rm client dcc build -C examples/calc
./examples/calc/out/calc
```
