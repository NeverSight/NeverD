// Built with line tables only (clang -gline-tables-only): the DWARF names
// the functions that hold inlined code and records the type of none, as
// rustc's -Cdebuginfo=1 and gcc's -g1 do.  That silence is no void return and
// no empty parameter list: the call passes what dlt_add reads, and dlt_add
// returns what it computes.

static inline long dlt_triple(long v) { return 3 * v; }

__attribute__((noinline)) long dlt_add(long a, long b) {
  return a + dlt_triple(b);
}

long dlt_caller(long x) { return dlt_add(x, 7) + dlt_triple(x); }
