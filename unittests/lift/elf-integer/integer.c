/* Freestanding arithmetic, memory and calling-convention fixtures. */
typedef unsigned long word;

__attribute__((noinline)) word mix(word a, word b) { return (a + b) ^ (b << 3); }

word product(word a, word b) { return a * b; }

word call_mix(word a, word b) { return mix(b + 1, a ^ 7) + b; }

word branch(word a, word b) {
  if (a < b)
    return a + b;
  return a - b;
}

word access_word(word *p, word a) {
  word b = p[1];
  p[1] = a;
  return b + a;
}

word atomic_word(word *p, word a) {
  return __atomic_fetch_add(&p[1], a, __ATOMIC_ACQ_REL);
}
