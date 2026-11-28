// Two callees whose DWARF, built with -g, declares every parameter: one takes
// the address of its caller's buffer as an integer, the other as a pointer.
// Their callers are in test_debug_frame_argument_callers.s.
__attribute__((noinline)) long dfa_sum(long buffer, long n) {
  return *(volatile char *)buffer + n;
}

__attribute__((noinline)) long dfa_first(const char *buffer, long n) {
  return buffer[0] + n;
}
