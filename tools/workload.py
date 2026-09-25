#!/usr/bin/env python3
"""Replay a fixed sequence of everyday edits and count compiler runs.

After every step the same tree is built twice:
  * make   - the generated Makefile, mtime-based with gcc -MMD header deps
  * dcc    - content-addressed cache + incremental key reuse
and both binaries must print the same checksum. The step list is fixed in
this file; it's meant to look like a week of work on a branch plus CI, not
to flatter either tool. Per-step numbers are printed so you can judge that.

    python3 tools/workload.py --project bench/proj --out bench/results
"""
import argparse
import json
import os
import re
import shutil
import subprocess
import time

HERE = os.path.dirname(os.path.abspath(__file__))


class Tree:
    def __init__(self, root):
        self.root = root
        self.n = 0

    def path(self, rel):
        return os.path.join(self.root, rel)

    def read(self, rel):
        with open(self.path(rel)) as f:
            return f.read()

    def write(self, rel, text):
        with open(self.path(rel), "w") as f:
            f.write(text)

    def files(self):
        out = []
        for d, _, fs in os.walk(self.root):
            if "/build" in d or "/out" in d:
                continue
            out += [os.path.relpath(os.path.join(d, f), self.root) for f in fs if f.endswith((".c", ".h"))]
        return sorted(out)

    # --- edit kinds ---
    def tweak(self, rel):  # real code change
        self.write(rel, re.sub(r"(\d+); /\* tweak \*/", lambda m: f"{int(m.group(1)) + 1}; /* tweak */",
                               self.read(rel), count=1))

    def comment(self, rel):  # comment-only change that shifts line numbers
        self.n += 1
        self.write(rel, f"// note {self.n}\n" + self.read(rel))

    def declare(self, rel):  # add a declaration to a header: changes every includer's source
        self.n += 1
        self.write(rel, self.read(rel).replace("#endif", f"long extra_{self.n}(long);\n\n#endif", 1))

    def unused_macro(self, rel):  # add a macro nobody uses: text changes, preprocessed output doesn't
        self.n += 1
        self.write(rel, self.read(rel).replace("#endif", f"#define UNUSED_{self.n} {self.n}\n\n#endif", 1))

    def touch_all(self):
        for rel in self.files():
            os.utime(self.path(rel))


def run(cmd, cwd, env=None):
    r = subprocess.run(cmd, cwd=cwd, env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    if r.returncode != 0:
        raise SystemExit(f"{' '.join(cmd)} failed:\n{r.stdout}\n{r.stderr}")
    return r.stdout


def build_both(root, log):
    open(log, "w").close()
    env = dict(os.environ, CC_LOG=log)
    t0 = time.time()
    run(["make", "-j8", "-s", f"CC={HERE}/cc-count.sh"], root, env)
    make_s = time.time() - t0
    with open(log) as f:
        make_compiles = sum(1 for _ in f)
    js = "/tmp/dcc-step.json"
    run(["dcc", "build", "-C", root, "--json", js], root)
    with open(js) as f:
        d = json.load(f)
    a = run(["./build/app"], root).strip()
    b = run(["./out/app"], root).strip()
    if a != b:
        raise SystemExit(f"make and dcc binaries disagree: {a} vs {b}")
    return {"make": make_compiles, "make_s": round(make_s, 2), "dcc": d["compiled"], "dcc_pre": d["preprocessed"],
            "dcc_cached": d["cached"], "dcc_s": round(d["total_ms"] / 1000, 2), "checksum": a}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--project", required=True)
    ap.add_argument("--out", required=True)
    args = ap.parse_args()
    root = os.path.abspath(args.project)
    t = Tree(root)
    log = "/tmp/cc-count.log"
    for d in ("build", "out"):
        shutil.rmtree(t.path(d), ignore_errors=True)
    if os.path.exists(t.path(".dcstate")):
        os.remove(t.path(".dcstate"))

    srcs = [f for f in t.files() if f.startswith("src/lib")]
    pick = lambda i: srcs[(i * 37) % len(srcs)]  # deterministic spread over the tree

    # the "feature" branch: 6 files and one shared header differ from main
    main_branch = {f: t.read(f) for f in t.files()}
    for i in range(6):
        t.tweak(pick(100 + i))
    t.declare("include/util1.h")
    feature_branch = {f: t.read(f) for f in t.files()}
    for f, text in main_branch.items():
        t.write(f, text)

    def checkout(branch):  # like git: rewrite only the files that differ
        for f, text in branch.items():
            if t.read(f) != text:
                t.write(f, text)

    saved = {}

    def remember(*files):
        saved.update({f: t.read(f) for f in files})

    def revert(*files):
        for f in files:
            t.write(f, saved[f])

    def clean():  # fresh CI machine: local outputs and state gone, the shared cache isn't
        for d in ("build", "out"):
            shutil.rmtree(t.path(d), ignore_errors=True)
        os.remove(t.path(".dcstate"))

    steps = [
        ("edit one .c file", lambda: t.tweak(pick(1))),
        ("add a declaration to a library header", lambda: (remember("include/lib1.h"), t.declare("include/lib1.h"))),
        ("comment-only edit in a .c file", lambda: t.comment(pick(2))),
        ("comment-only edit in common.h", lambda: t.comment("include/common.h")),
        ("revert the library header", lambda: revert("include/lib1.h")),
        ("switch to feature branch", lambda: checkout(feature_branch)),
        ("edit two files on the branch", lambda: (t.tweak(pick(3)), t.tweak(pick(4)))),
        ("switch back to main", lambda: (feature_branch.update({f: t.read(f) for f in t.files()}),
                                         checkout(main_branch))),
        ("switch to feature again", lambda: (main_branch.update({f: t.read(f) for f in t.files()}),
                                             checkout(feature_branch))),
        ("touch every file (fresh checkout / restored CI cache)", t.touch_all),
        ("clean build on a new machine", clean),
        ("edit one .c file", lambda: t.tweak(pick(5))),
        ("unused macro added to common.h", lambda: t.unused_macro("include/common.h")),
        ("declaration added to common.h", lambda: (remember("include/common.h"), t.declare("include/common.h"))),
        ("revert common.h", lambda: revert("include/common.h")),
        ("edit three .c files", lambda: (remember(pick(6), pick(7), pick(8)),
                                         [t.tweak(pick(i)) for i in (6, 7, 8)])),
        ("revert those three", lambda: revert(pick(6), pick(7), pick(8))),
        ("edit one .c file", lambda: t.tweak(pick(9))),
        ("switch to main", lambda: (feature_branch.update({f: t.read(f) for f in t.files()}),
                                    checkout(main_branch))),
        ("clean build on a new machine", clean),
    ]

    results = [{"step": "initial clean build", **build_both(root, log)}]
    print(f"{'step':<55} {'make':>5} {'dcc':>5}  {'dcc re-pp':>9}")
    print(f"{results[0]['step']:<55} {results[0]['make']:>5} {results[0]['dcc']:>5}  {results[0]['dcc_pre']:>9}")
    for name, fn in steps:
        time.sleep(0.01)  # distinct mtimes, as between real edits
        fn()
        r = {"step": name, **build_both(root, log)}
        results.append(r)
        print(f"{name:<55} {r['make']:>5} {r['dcc']:>5}  {r['dcc_pre']:>9}", flush=True)

    rest = results[1:]
    make_total = sum(r["make"] for r in rest)
    dcc_total = sum(r["dcc"] for r in rest)
    summary = {
        "units": results[0]["make"],
        "steps": len(rest),
        "make_compiles": make_total,
        "dcc_compiles": dcc_total,
        "reduction": round(1 - dcc_total / make_total, 4) if make_total else 0,
    }
    print(f"\nafter the initial build: make ran the compiler {make_total} times, dcc {dcc_total} times "
          f"({100 * summary['reduction']:.1f}% fewer)")
    os.makedirs(args.out, exist_ok=True)
    with open(os.path.join(args.out, "workload.json"), "w") as f:
        json.dump({"summary": summary, "steps": results}, f, indent=2)


if __name__ == "__main__":
    main()
