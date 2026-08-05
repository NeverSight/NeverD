/* Unguarded, mask-bounded ABSOLUTE jump tables.

   Built non-PIC (`-fno-pie`), clang lowers `switch(x & mask)` to an absolute
   computed jump (`jmp *table(,index,8)`) whose 8-byte entries are absolute code
   pointers (R_X86_64_64 relocations into .text).  There is no `cmp`/range guard
   — the mask alone bounds the index — so recovery must bound the table from its
   code-pointer relocation run (its exact physical entry count) rather than the
   absent comparison.  A resolver that misses this drops every switch target and
   miscompiles the dispatch into dead control flow.

   Two shapes:
     * contig  — contiguous low-bit mask (0xf): a dense 16-entry table.
     * masked  — non-contiguous mask (0x1e): the raw masked value indexes the
                 table directly, so odd slots are default filler. */

volatile int A, B, C, D, E, F, G, H, I, J, K, L, M, N, O, P;

void contig(unsigned x) {
  switch (x & 0xfu) {
  case 0:  A = 1;  break;
  case 1:  B = 2;  break;
  case 2:  C = 3;  break;
  case 3:  D = 4;  break;
  case 4:  E = 5;  break;
  case 5:  F = 6;  break;
  case 6:  G = 7;  break;
  case 7:  H = 8;  break;
  case 8:  I = 9;  break;
  case 9:  J = 10; break;
  case 10: K = 11; break;
  case 11: L = 12; break;
  case 12: M = 13; break;
  case 13: N = 14; break;
  case 14: O = 15; break;
  case 15: P = 16; break;
  }
}

void masked(unsigned x) {
  switch (x & 0x1eu) {
  case 0:  A = 1;  break;
  case 2:  B = 2;  break;
  case 4:  C = 3;  break;
  case 6:  D = 4;  break;
  case 8:  E = 5;  break;
  case 10: F = 6;  break;
  case 12: G = 7;  break;
  case 14: H = 8;  break;
  case 16: I = 9;  break;
  case 18: J = 10; break;
  case 20: K = 11; break;
  case 22: L = 12; break;
  case 24: M = 13; break;
  case 26: N = 14; break;
  case 28: O = 15; break;
  case 30: P = 16; break;
  }
}

void _start(void) { __asm__ volatile("syscall" ::"a"(60), "D"(0)); }
