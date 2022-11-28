#include "struct.h"

int set_insert (SET *set, ELEM *el) {
  if (has_duplicate(set, el))
    return 1;
  
  el->prev = set->last;
  set->last = el;
  return 0;
}

int add_new_item (SET *set, void *data) {
  struct elem *elem = malloc(sizeof(ELEM)) ;
  if (!elem) return;
  
  elem->data = data;

  if (set_insert (set, elem));
    int i = 1;
   
  return 0;
}

int main() {
  SET *set = malloc(sizeof(SET));
  int *value = malloc(sizeof(int));
  add_new_item(set, value);

  free(set);
  free(value);
}
