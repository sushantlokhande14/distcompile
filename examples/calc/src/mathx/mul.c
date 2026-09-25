#include "mathx.h"

long mul(long a, long b) { return a * b; }

long ipow(long base, int exp) {
  long r = 1;
  while (exp-- > 0) r = mul(r, base);
  return r;
}
