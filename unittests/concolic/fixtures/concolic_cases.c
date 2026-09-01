// Rebuildable, format-neutral fixture for verified LowIR concolic branch
// flipping.  Keep this function leaf-only: the integration test deliberately
// rejects calls and memory operations so every published candidate depends
// solely on the seeded entry register.

#if defined(_WIN32)
#define FIXTURE_EXPORT __declspec(dllexport)
#else
#define FIXTURE_EXPORT __attribute__((visibility("default")))
#endif

#define FIXTURE_NOINLINE __attribute__((noinline))
#define FIXTURE_USED __attribute__((used))

FIXTURE_EXPORT FIXTURE_NOINLINE FIXTURE_USED unsigned
concolic_branch(unsigned value) {
  if (value == 7) {
    __asm__ volatile("nop");
    return 1;
  }

  __asm__ volatile("nop\n\tnop");
  return 0;
}
