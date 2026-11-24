/* Freestanding arithmetic, memory and calling-convention fixtures. */
typedef unsigned long word;

__attribute__((noinline)) word mix(word a, word b) {
  return (a + b) ^ (b << 3);
}

word product(word a, word b) { return a * b; }

word unsigned_quotient(word a, word b) { return a / b; }

long signed_quotient(long a, long b) { return a / b; }

#if defined(__powerpc__)
word independent_divides(word a, word b) {
  word first, second;
  __asm__ volatile("divw %0, %1, %2" : "=r"(first) : "r"(a), "r"(b));
  __asm__ volatile("divw %0, %1, %2" : "=r"(second) : "r"(a), "r"(b));
  return first ^ second;
}

word recorded_divide(word a, word b) {
  word quotient, cr;
  __asm__("divw. %0, %2, %3; mfcr %1"
          : "=r"(quotient), "=r"(cr)
          : "r"(a), "r"(b)
          : "cr0");
  return quotient ^ ((cr >> 29) & 7);
}

word divide_flags(word a, word b) {
  word quotient, cr;
  word xer = (a & 1) << 31;
  __asm__("mtxer %2; divwo. %0, %3, %4; mfcr %1"
          : "=r"(quotient), "=r"(cr)
          : "r"(xer), "r"(a), "r"(b)
          : "cr0", "xer");
  return (unsigned)cr >> 28;
}
#endif

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
