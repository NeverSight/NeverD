// The same functions built with -g, and one that takes nothing and returns
// nothing: every declaration the DWARF records stays authoritative,
// dfs_none's `void dfs_none(void)` included.

volatile int dfs_counter;

__attribute__((noinline)) void dfs_none(void) { ++dfs_counter; }

__attribute__((noinline)) long dfs_add(long a, long b) { return a + 3 * b; }

long dfs_caller(long x) {
  dfs_none();
  return dfs_add(x, 7) + 1;
}
