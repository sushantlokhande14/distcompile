#include <ctype.h>

#include "strx.h"

void upper(char *s) {
  for (; *s; s++) *s = (char)toupper((unsigned char)*s);
}

size_t count_char(const char *s, char c) {
  size_t n = 0;
  for (; *s; s++) n += *s == c;
  return n;
}
