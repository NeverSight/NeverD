/* Minimal Darwin AArch64 call-ABI probes; no SDK headers are required. */

typedef unsigned long long u64;
typedef double (*double_fn)(double);

typedef struct {
  u64 first;
  u64 second;
} Pair;

extern Pair make_external_pair(void);
extern u64 sum_external_varargs(u64 first, ...);

__attribute__((noinline)) double indirect_double_call(double value,
                                                      double_fn fn) {
  return fn(value);
}

__attribute__((noinline)) double indirect_double_consumer(double value,
                                                          double_fn fn) {
  return indirect_double_call(value, fn) + 2.0;
}

__attribute__((noinline)) u64 direct_external_pair_sum(void) {
  Pair value = make_external_pair();
  return value.first + value.second;
}

__attribute__((noinline)) u64 direct_external_varargs(void) {
  return sum_external_varargs(0x10ULL, 0x20ULL, 0x30ULL);
}
