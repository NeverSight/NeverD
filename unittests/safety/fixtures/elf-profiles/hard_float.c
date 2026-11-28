/* Cross-compiled and inspected as data. The two frame cases differ only in
 * whether the input was stored before the load. */
__attribute__((noinline)) double first(double input) {
  volatile double local;
  return local;
}

__attribute__((noinline)) double second(double input) {
  volatile double local = input;
  return local;
}

/* The caller passes two floating arguments and consumes the floating result. */
__attribute__((noinline)) double signed_second(double left, double right) {
  volatile double saved = right;
  return __builtin_copysign(saved, left);
}

void write_result(double *output, double left, double right) {
  *output = signed_second(left, right);
}
