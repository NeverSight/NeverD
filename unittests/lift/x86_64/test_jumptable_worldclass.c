/* Adversarial jump-table shapes that stress the parts of switch recovery a
   path-intersection recovery model must get right.  Every case body performs a
   DISTINCT, non-linear side effect on a volatile so the compiler cannot fold
   the switch into branchless arithmetic or a value-lookup data table — it must
   emit a real indexed jump through a code-pointer table.

     * wc_merge       — the switch variable is a merge (PHI) of two computations
                        reaching the dispatch from different predecessors, so a
                        single-path backward slice can latch the wrong reaching
                        definition; the recovered switch variable must be the
                        value common to every path to the indirect branch.
     * wc_guard_default — a genuinely guarded switch whose default is a distinct,
                        side-effecting block (not a mask that makes the default
                        unreachable); the out-of-range edge must stay wired to
                        the default computation.
     * wc_signed_gap  — a signed switch with a negative base and an interior gap
                        (case -2 absent); labels must present the true signed
                        values, and the absent case must route to the default.
     * wc_nested      — two real jump tables where the inner scrutinee is derived
                        from the outer switch value (overlapping index dataflow).
*/

volatile int SINK;

int wc_merge(int sel, int a, int b) {
  int k = sel ? a + 1 : b + 2;
  switch (k) {
  case 0: SINK += 1; break;
  case 1: SINK *= 3; break;
  case 2: SINK ^= 0xaa; break;
  case 3: SINK -= 7; break;
  case 4: SINK <<= 2; break;
  case 5: SINK |= 0x11; break;
  case 6: SINK &= 0x7f; break;
  case 7: SINK = 66; break;
  case 8: SINK += 99; break;
  default: SINK = -1; break;
  }
  return SINK;
}

int wc_guard_default(unsigned x, int y) {
  switch (x) {
  case 0: SINK += y; break;
  case 1: SINK *= 3; break;
  case 2: SINK ^= 0x55; break;
  case 3: SINK -= 9; break;
  case 4: SINK <<= 1; break;
  case 5: SINK |= 0x22; break;
  case 6: SINK &= 0x3f; break;
  case 7: SINK = y + 7; break;
  default:
    SINK = y * 3 + 1;
    break;
  }
  return SINK;
}

int wc_signed_gap(int x) {
  switch (x) {
  case -4: SINK += 1; break;
  case -3: SINK *= 5; break;
  /* case -2 intentionally absent (interior gap) */
  case -1: SINK ^= 0x0f; break;
  case 0: SINK -= 3; break;
  case 1: SINK <<= 2; break;
  case 2: SINK |= 0x40; break;
  case 3: SINK &= 0x7e; break;
  case 4: SINK += 88; break;
  default: SINK = -1; break;
  }
  return SINK;
}

int wc_nested(int outer, int inner) {
  switch (outer) {
  case 0: SINK += 1; break;
  case 1: SINK *= 3; break;
  case 2:
    switch (inner) {
    case 3: SINK ^= 0x11; break;
    case 4: SINK -= 5; break;
    case 5: SINK <<= 3; break;
    case 6: SINK |= 0x88; break;
    case 7: SINK &= 0x3c; break;
    default: SINK = -7; break;
    }
    break;
  case 3: SINK -= 2; break;
  case 4: SINK ^= 0x24; break;
  case 5: SINK += 55; break;
  default: SINK = -1; break;
  }
  return SINK;
}

/* Portable entry (no arch-specific asm) so the one source compiles for x86-64,
   AArch64, and ARM32 alike.  References every switch so none is dead-stripped;
   the infinite loop stands in for process exit (the tests only lift, never
   run). */
void _start(void) {
  SINK = wc_merge(1, 2, 3);
  SINK += wc_guard_default(4, 5);
  SINK += wc_signed_gap(-1);
  SINK += wc_nested(2, 5);
  for (;;) {
  }
}
