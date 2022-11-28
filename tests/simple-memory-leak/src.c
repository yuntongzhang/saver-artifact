#include <stdio.h>

int foo() {
  int *q = malloc(4);

  return 0;
}

int main() {
  int *p = malloc(4);

  foo();

  return 0;
}
