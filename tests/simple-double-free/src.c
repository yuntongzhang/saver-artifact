#include <stdio.h>

void *foo() {
  int *q = malloc(4);

  free(q);
  return q;
}

int main() {
  int *p;
  p = foo();

  free(p);

  return 0;
}
