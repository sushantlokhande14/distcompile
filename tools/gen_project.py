#!/usr/bin/env python3
"""Generate a synthetic C project for benchmarking.

    python3 tools/gen_project.py --units 120 --libs 4 --out bench/proj

Layout: include/common.h (everyone includes it), include/util{0..2}.h (each
included by a random ~25% of files), include/lib{k}.h (per library),
src/lib{k}/f{i}.c, src/main.c. Writes both a DCBUILD and a Makefile with
gcc -MMD header tracking, so dcc can be compared against plain make on the
exact same tree.

Every function has a `/* tweak */` constant the workload script can change
to make a real code edit, and main() prints a checksum over all of them, so
two builds of the same tree must print the same number.
"""
import argparse
import os
import random


def write(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as f:
        f.write(text)


def function(rnd, lib, i):
    k = rnd.randint(3, 97)
    body = []
    for s in range(rnd.randint(3, 6)):
        kind = rnd.choice(["loop", "switch", "mix"])
        if kind == "loop":
            body.append(f"  for (int j = 0; j < {rnd.randint(3, 12)}; j++) acc += mix(acc, j * {rnd.randint(2, 9)});")
        elif kind == "switch":
            cases = "\n".join(f"    case {c}: acc ^= {rnd.randint(1, 999)}; break;" for c in range(rnd.randint(3, 8)))
            body.append(f"  switch (acc & 7) {{\n{cases}\n    default: acc += 1;\n  }}")
        else:
            body.append(f"  acc = CLAMP(acc * {rnd.randint(2, 7)} + x, -1000000, 1000000);")
    body = "\n".join(body)
    return f"""
static long helper_{i}(long v) {{
  long r = v;
  for (int n = 0; n < 4; n++) r = (r * 31 + n) % 100003;
  return r;
}}

long lib{lib}_f{i}(long x) {{
  long acc = x + {k}; /* tweak */
{body}
  return helper_{i}(acc);
}}
"""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--units", type=int, default=120)
    ap.add_argument("--libs", type=int, default=4)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--out", required=True)
    a = ap.parse_args()
    rnd = random.Random(a.seed)
    root = a.out
    per_lib = max(1, a.units // a.libs)

    write(f"{root}/include/common.h", """#ifndef COMMON_H
#define COMMON_H

#include <stdint.h>
#include <string.h>

#define CLAMP(v, lo, hi) ((v) < (lo) ? (lo) : (v) > (hi) ? (hi) : (v))

static inline long mix(long a, long b) { return (a ^ (b << 3)) + (b * 2654435761L % 1000); }

#endif
""")
    for u in range(3):
        write(f"{root}/include/util{u}.h", f"""#ifndef UTIL{u}_H
#define UTIL{u}_H

#define UTIL{u}_SCALE {u + 2}
static inline long util{u}_step(long v) {{ return v * UTIL{u}_SCALE + {u}; }}

#endif
""")

    funcs = []
    for lib in range(a.libs):
        protos = []
        for i in range(per_lib):
            name = f"lib{lib}_f{i}"
            protos.append(f"long {name}(long x);")
            funcs.append(name)
            incs = ["common.h", f"lib{lib}.h"] + [f"util{u}.h" for u in range(3) if rnd.random() < 0.25]
            text = "".join(f'#include "{h}"\n' for h in incs) + function(rnd, lib, i)
            write(f"{root}/src/lib{lib}/f{i}.c", text)
        write(f"{root}/include/lib{lib}.h",
              f"#ifndef LIB{lib}_H\n#define LIB{lib}_H\n\n" + "\n".join(protos) + "\n\n#endif\n")

    calls = "\n".join(f"  sum = (sum * 7 + {f}(sum % 1000)) % 1000000007;" for f in funcs)
    includes = "".join(f'#include "lib{lib}.h"\n' for lib in range(a.libs))
    write(f"{root}/src/main.c", f"""#include <stdio.h>

#include "common.h"
{includes}
int main(void) {{
  long sum = 1;
{calls}
  printf("checksum %ld\\n", sum);
  return 0;
}}
""")

    libs = " ".join(f"lib{k}" for k in range(a.libs))
    dcbuild = ["cc      gcc", "cflags  -O2 -Wall -Iinclude", "ldflags -lm", ""]
    dcbuild += [f"lib lib{k} src/lib{k}/*.c" for k in range(a.libs)]
    dcbuild += [f"bin app src/main.c : {libs}"]
    write(f"{root}/DCBUILD", "\n".join(dcbuild) + "\n")

    mk = ["CC ?= gcc", "CFLAGS := -O2 -Wall -Iinclude", "LDFLAGS := -lm", "", "all: build/app", ""]
    for k in range(a.libs):
        mk.append(f"LIB{k}_OBJS := $(patsubst %.c,build/%.o,$(wildcard src/lib{k}/*.c))")
        mk.append(f"build/liblib{k}.a: $(LIB{k}_OBJS)\n\tar rcs $@ $^")
    mk.append("")
    archives = " ".join(f"build/liblib{k}.a" for k in range(a.libs))
    mk.append(f"build/app: build/src/main.o {archives}\n\t$(CC) -o $@ $^ $(LDFLAGS)")
    mk.append("")
    mk.append("build/%.o: %.c\n\t@mkdir -p $(dir $@)\n\t$(CC) $(CFLAGS) -MMD -MP -c $< -o $@")
    mk.append("")
    mk.append("-include $(shell find build -name '*.d' 2>/dev/null)")
    write(f"{root}/Makefile", "\n".join(mk) + "\n")
    print(f"{root}: {len(funcs) + 1} units, {a.libs} libraries")


if __name__ == "__main__":
    main()
