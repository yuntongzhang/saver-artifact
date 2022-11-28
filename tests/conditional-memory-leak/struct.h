#include <stdio.h>
#include <stdlib.h>

typedef int bool;
void *g;

typedef struct {
  int *f;
  int *g;
  struct {
    int *h;
  } sub;
} ITEM;

typedef struct queue {
  ITEM *item;
  struct queue *next;
} QUEUE;

typedef struct ssl {
  QUEUE *q;
  ITEM *f;
} SSL;

void *Alloc(int size) {
  void *ret = malloc(size);
  if(ret == NULL)
    exit(1);
  return ret;
}

int *AllocArg(void **arg, int size) {
  if(arg == NULL)
    exit(1);
  if((*arg = malloc(size)) == NULL)
    exit(1);
  return 0;
}

void Use(void *p) { printf("%x\n", p); } 

typedef struct elem {
  struct elem* prev;
  void* data;
} ELEM;

typedef struct set {
  ELEM *last;
} SET;

bool has_duplicate (SET *s, ELEM *el) {
  return 1;
}

