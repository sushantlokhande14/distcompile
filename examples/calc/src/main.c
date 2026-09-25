#include <stdio.h>

#include "mathx.h"
#include "strx.h"

int main(void) {
  char word[] = "distcompile";
  printf("add(2, 3) = %ld\n", add(2, 3));
  printf("ipow(3, 4) = %ld\n", ipow(3, 4));
  printf("hypot2(3, 4) = %.1f\n", hypot2(3, 4));
  reverse(word);
  printf("reverse = %s\n", word);
  upper(word);
  printf("upper = %s, %zu x 'I'\n", word, count_char(word, 'I'));
  return 0;
}
