/* Computed-goto (threaded-dispatch) label tables — the `goto *tab[idx]` shape.

   At -O0 the label address table is a function-local constant array in
   .data/.rodata carrying a run of absolute code-pointer relocations, and the
   dispatch is *decoupled* from the table load by a spill/reload relay: each
   goto site loads `tab[idx]`, spills the target to a frame slot, and jumps to a
   shared block that reloads it and performs the indirect branch.  A recovery
   that only inspects the branch's own instruction (or a single-predecessor
   path) never reaches the table load in a many-goto-site dispatch, so the
   branch would degrade to an indirect tail call and lose every label target.

   Recovery must instead anchor on the code-pointer relocation run at the
   table's constant base — the verifiable label-table signature — and rebuild a
   real multi-way dispatch regardless of how many goto sites share it. */

volatile int S;

/* Single dispatch site. */
int cg_single(int sel) {
  static const void *tab[] = {&&A, &&B, &&C, &&D, &&E};
  goto *tab[sel % 5];
A: S += 1; return 1;
B: S *= 3; return 2;
C: S ^= 7; return 3;
D: S -= 2; return 4;
E: S <<= 1; return 5;
}

/* Loop with many goto sites all feeding one shared indirect branch. */
int cg_loop(int *ops, int n) {
  static const void *tab[] = {&&L0, &&L1, &&L2, &&L3, &&L4, &&L5};
  int i = 0, acc = 0;
  if (n <= 0)
    return 0;
  goto *tab[ops[0] % 6];
L0: acc += 1;  if (++i < n) goto *tab[ops[i] % 6]; return acc;
L1: acc *= 3;  if (++i < n) goto *tab[ops[i] % 6]; return acc;
L2: acc ^= 7;  if (++i < n) goto *tab[ops[i] % 6]; return acc;
L3: acc -= 2;  if (++i < n) goto *tab[ops[i] % 6]; return acc;
L4: acc <<= 1; if (++i < n) goto *tab[ops[i] % 6]; return acc;
L5: acc |= 4;  if (++i < n) goto *tab[ops[i] % 6]; return acc;
}

void _start(void) {
  int a[4] = {1, 2, 3, 4};
  S = cg_single(2) + cg_loop(a, 4);
  for (;;) {
  }
}
