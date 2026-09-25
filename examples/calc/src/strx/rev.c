#include <string.h>

#include "strx.h"

void reverse(char *s) {
  size_t n = strlen(s);
  for (size_t i = 0; i < n / 2; i++) {
    char t = s[i];
    s[i] = s[n - 1 - i];
    s[n - 1 - i] = t;
  }
}
