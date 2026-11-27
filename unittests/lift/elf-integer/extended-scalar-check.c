// Independent scalar oracle for the linked Linux ELF and each recompiled body.
typedef unsigned long Word;

static const Word Inputs[] = {0,
                              1,
                              31,
                              32,
                              63,
                              64,
                              127,
                              0x7fffffff,
                              0x80000000,
                              0xffffffff,
                              (Word)0x12345678abcdef81ULL,
                              (Word)0xffffffff80000001ULL,
                              ~0UL};
#ifdef TEST_PPC64_NOTOC
extern Word notoc_cross(Word);

int check(void) {
  for (unsigned I = 0; I < sizeof(Inputs) / sizeof(Word); ++I)
    if (notoc_cross(Inputs[I]) != 3 * (Inputs[I] + 1) + 93 + 19)
      return 1;
  return 0;
}
#elif defined(TEST_RISCV_ZICOND)
extern Word eval(Word, Word, Word);

int check(void) {
  const Word Conditions[] = {0, 1, ~0UL, 1UL << (sizeof(Word) * 8 - 1),
                             (Word)0x100000000ULL};
  for (unsigned I = 0; I < sizeof(Inputs) / sizeof(Word); ++I)
    for (unsigned J = 0; J < sizeof(Inputs) / sizeof(Word); ++J)
      for (unsigned K = 0; K < sizeof(Conditions) / sizeof(Word); ++K) {
        Word C = Conditions[K];
        if (eval(Inputs[I], Inputs[J], C) != (C ? Inputs[I] : Inputs[J]))
          return 1;
      }
  return 0;
}
#elif defined(__riscv)
extern Word eval(Word, Word);

static Word expected(Word X, Word Y) {
  const unsigned Bits = sizeof(Word) * 8;
  const Word HighBit = 1UL << (Bits - 1);
  const Word IndexX = 1UL << (X % Bits), IndexY = 1UL << (Y % Bits);
  Word T0 = 2 * X + Y, T1 = 4 * Y + T0, T2 = 8 * X + T1;
#if __riscv_xlen == 64
  T0 = (unsigned)T0 + T2;
  T1 = 2UL * (unsigned)X + T0;
  T0 = 4UL * (unsigned)Y + T1;
  T2 = (8UL * (unsigned)T2 + T0) ^ ((Word)(unsigned)X << 33);
#endif
  T1 = ((T2 | IndexY) & ~HighBit) ^ IndexX;
  T0 = ((((T2 & IndexY) != 0) | HighBit) & ~IndexX) ^ 8;
  return (T1 + T0) ^ ((T2 & HighBit) != 0);
}

int check(void) {
  for (unsigned I = 0; I < sizeof(Inputs) / sizeof(Word); ++I)
    for (unsigned J = 0; J < sizeof(Inputs) / sizeof(Word); ++J)
      if (eval(Inputs[I], Inputs[J]) != expected(Inputs[I], Inputs[J]))
        return 1;
  return 0;
}
#else
extern Word eval(Word, unsigned char *);

int check(void) {
  Word Previous = 0;
  for (unsigned I = 0; I < sizeof(Inputs) / sizeof(Word); ++I) {
    union {
      unsigned char Bytes[16];
      Word Align;
    } Actual = {.Bytes = {[15] = 0xa5}}, Expected = Actual;
    Word X = Inputs[I] - 0x18001;
    unsigned int W = (unsigned)(X ^ 0x100008001UL);
    __builtin_memcpy(Expected.Bytes, &X, sizeof(X));
    __builtin_memcpy(Expected.Bytes + 8, &W, sizeof(W));
    X = (X + (int)W) ^ W;
    unsigned short H = (unsigned short)X;
    __builtin_memcpy(Expected.Bytes + 12, &H, sizeof(H));
    X = (X ^ (short)H) + H;
    Expected.Bytes[14] = (unsigned char)X;
    Word Next = X + Expected.Bytes[14];
    Word Result = Next ^ Previous ^ 0x87456321abcd0001UL;
    Previous = Next;
    if (eval(Inputs[I], Actual.Bytes) != Result)
      return 1;
    for (unsigned K = 0; K < sizeof(Actual.Bytes); ++K)
      if (Actual.Bytes[K] != Expected.Bytes[K])
        return 2;
  }
  return 0;
}
#endif
