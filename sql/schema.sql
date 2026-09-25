-- DistCompile metadata. The coordinator applies this at startup (idempotent).

CREATE TABLE IF NOT EXISTS builds (
  id          BIGSERIAL PRIMARY KEY,
  toolchain   TEXT NOT NULL,
  state       TEXT NOT NULL DEFAULT 'running',   -- running, done, failed
  total       INT NOT NULL,
  created_at  TIMESTAMPTZ NOT NULL DEFAULT now(),
  finished_at TIMESTAMPTZ
);

CREATE TABLE IF NOT EXISTS tasks (
  uid          BIGSERIAL PRIMARY KEY,
  build_id     BIGINT NOT NULL REFERENCES builds(id) ON DELETE CASCADE,
  id           INT NOT NULL,                     -- index inside the build
  kind         SMALLINT NOT NULL,                -- 0 compile, 1 archive, 2 link
  name         TEXT NOT NULL,
  key          TEXT,                             -- cache key (null until known)
  input        TEXT,                             -- compile: preprocessed source digest
  args         TEXT NOT NULL DEFAULT '',         -- one per line
  inputs       TEXT,                             -- digests of dependency outputs, one per line
  input_names  TEXT,                             -- file names for them on the worker
  toolchain    TEXT NOT NULL,
  cost         BIGINT NOT NULL DEFAULT 0,
  priority     BIGINT NOT NULL DEFAULT 0,        -- critical-path cost, bigger runs first
  part         INT NOT NULL DEFAULT 0,           -- preferred partition
  state        TEXT NOT NULL,                    -- waiting, ready, running, done, cached, failed
  pending_deps INT NOT NULL,
  attempts     INT NOT NULL DEFAULT 0,
  lease_owner  TEXT,
  lease_until  TIMESTAMPTZ,
  output       TEXT,
  log          TEXT,
  exec_ms      DOUBLE PRECISION,
  UNIQUE (build_id, id)
);

-- ClaimTask scans only ready tasks
CREATE INDEX IF NOT EXISTS tasks_ready ON tasks (toolchain, priority DESC) WHERE state = 'ready';
-- ... and this one lets a worker find the best task in its own partition without a sort
CREATE INDEX IF NOT EXISTS tasks_ready_part ON tasks (toolchain, part, priority DESC) WHERE state = 'ready';
CREATE INDEX IF NOT EXISTS tasks_running ON tasks (lease_until) WHERE state = 'running';

CREATE TABLE IF NOT EXISTS task_deps (
  build_id BIGINT NOT NULL,
  task     INT NOT NULL,   -- this task ...
  dep      INT NOT NULL,   -- ... waits for this one
  pos      INT NOT NULL,   -- position among the task's inputs (link order matters)
  PRIMARY KEY (build_id, dep, task)
);

-- action cache: cache key -> output blob
CREATE TABLE IF NOT EXISTS cache (
  key        TEXT PRIMARY KEY,
  output     TEXT NOT NULL,
  size       BIGINT NOT NULL,
  created_at TIMESTAMPTZ NOT NULL DEFAULT now(),
  hits       BIGINT NOT NULL DEFAULT 0
);

CREATE TABLE IF NOT EXISTS workers (
  id        TEXT PRIMARY KEY,
  toolchain TEXT NOT NULL,
  slots     INT NOT NULL,
  part      INT NOT NULL,
  last_seen TIMESTAMPTZ NOT NULL DEFAULT now()
);

-- added after the first version; ALTER keeps existing databases working
-- a task isn't handed straight back to the worker that just failed it
ALTER TABLE tasks ADD COLUMN IF NOT EXISTS last_failed_by TEXT;
ALTER TABLE tasks ADD COLUMN IF NOT EXISTS ready_at TIMESTAMPTZ;
