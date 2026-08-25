/* Minimal Darwin AArch64 call-ABI probes; no SDK headers are required. */

typedef unsigned long long u64;
typedef double (*double_fn)(double);

typedef struct {
  u64 first;
  u64 second;
} Pair;

extern Pair make_external_pair(void);
extern u64 sum_external_varargs(u64 first, ...);
extern int *__error(void);
extern int printf(const char *, ...);
extern int vprintf(const char *, __builtin_va_list);

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

__attribute__((noinline)) int forward_va_list(const char *fmt, ...) {
  __builtin_va_list ap;
  __builtin_va_start(ap, fmt);
  int result = vprintf(fmt, ap);
  __builtin_va_end(ap);
  return result;
}

__attribute__((noinline)) int forward_va_list_with_context(int context,
                                                     const char *fmt, ...) {
  __builtin_va_list ap;
  __builtin_va_start(ap, fmt);
  int result = vprintf(fmt, ap);
  __builtin_va_end(ap);
  return result + context;
}

__attribute__((noinline)) int call_forward_va_list_with_context(int context,
                                                       const char *value) {
  return forward_va_list_with_context(context, "%s", value);
}

__attribute__((noinline)) int external_error_compute(int a, int b, int c,
                                                     int d, int e, int f,
                                                     int g) {
  int x = a + b;
  int y = c + d;
  int z = e + f + g;
  if ((x ^ y ^ z) != 0)
    return *__error();
  return z;
}

__attribute__((noinline)) int external_error_simple(void) {
  return *__error();
}

__attribute__((noinline)) int joined_external_varargs(int flag,
                                                     const char *value) {
  const char *selected;
  int separator;
  if (flag) {
    selected = value;
    separator = ':';
  } else {
    selected = value;
    separator = ',';
  }
  return printf("%s%c", selected, separator);
}
__attribute__((noinline, noreturn)) void neverd_fail(void) { __builtin_trap(); }

__attribute__((noinline)) int neverd_after_fail(int value) {
  if (value != 0)
    neverd_fail();
  return 7;
}
