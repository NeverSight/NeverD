// Sink leaves live in a second optimized translation unit so callers retain
// ordinary call/return edges even though these analysis-only leaves terminate.

#if defined(_MSC_VER)
#define FIXTURE_HIDDEN
#define FIXTURE_NOINLINE __declspec(noinline)
#elif defined(__GNUC__)
#define FIXTURE_HIDDEN __attribute__((visibility("hidden")))
#define FIXTURE_NOINLINE __attribute__((noinline))
#else
#define FIXTURE_HIDDEN
#define FIXTURE_NOINLINE
#endif

extern char *strcpy(char *, const char *);

#line 80 "interproc_cases.c"
// Hunt: a deep exported path separates structural reachability from the
// attacker-control fixed point.  Keep both forwarding layers local and
// non-inline so every matrix image retains the same two internal call edges.
FIXTURE_HIDDEN FIXTURE_NOINLINE void interproc_forward_inner(const char *s) {
  char buf[16];
  strcpy(buf, s);
  __builtin_trap();
}

#line 100 "interproc_cases.c"
FIXTURE_HIDDEN FIXTURE_NOINLINE void interproc_isolation_leaf(const char *s) {
  char buf[16];
  strcpy(buf, s);
  __builtin_trap();
}
