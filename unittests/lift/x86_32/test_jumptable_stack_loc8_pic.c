/* Match the i386 semantic computed-goto input: Clang -O0 emits a PIC GOT-base
   sequence and an unresolved memcpy of this eight-entry initializer into the
   stack slot.  NeverD lifts this relocatable object, not the linked image used
   only to execute the original function under Unicorn. */
int jt_i386_stack_loc8_budget(int a) {
  const void *tab[8] = {&&L0, &&L1, &&L2, &&L3, &&L4, &&L5, &&L6, &&L7};
  unsigned acc = (unsigned)a;
  int pc = 0;
  for (int i = 0; i < 80; ++i) {
    goto *tab[pc & 7];
  L0:
    acc += 0x9E3779B9u;
    pc = (int)(acc & 7);
    continue;
  L1:
    acc ^= (acc << 13) | (acc >> 19);
    pc = (int)((acc >> 7) & 7);
    continue;
  L2:
    acc *= 2654435761u;
    pc = (int)((acc >> 11) & 7);
    continue;
  L3:
    acc -= acc >> 5;
    pc = (int)((acc >> 3) & 7);
    continue;
  L4:
    acc += 0x85EBCA6Bu;
    pc = (int)((acc >> 2) & 7);
    continue;
  L5:
    acc ^= acc >> 15;
    pc = (int)((acc >> 1) & 7);
    continue;
  L6:
    acc = (acc << 5) | (acc >> 27);
    pc = (int)((acc >> 9) & 7);
    continue;
  L7:
    acc += acc << 1;
    pc = (int)((acc >> 4) & 7);
    continue;
  }
  return (int)acc;
}
