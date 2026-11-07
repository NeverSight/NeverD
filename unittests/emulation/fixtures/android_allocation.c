//===- android_allocation.c - Explicit heap-content experiments ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Copy allocation contents through an external memcpy, including cross-page
/// bytes, preserved realloc prefixes and failed allocation ownership.
typedef unsigned long U64;
extern void *malloc(U64);
extern void *calloc(U64, U64);
extern void *realloc(void *, U64);
extern void free(void *);
extern void *memcpy(void *, const void *, U64);
extern int *__errno(void);
extern void *mmap(void *, U64, int, int, int, long);
extern int mprotect(void *, U64, int);

U64 allocation_contents(unsigned char *Out) {
  unsigned char *First = malloc(4099);
  if (!First)
    return 1;
  memcpy(Out, First, 4099);
  for (unsigned I = 0; I < 8; ++I)
    First[I] = 0x30 + I;
  unsigned char *Grown = realloc(First, 4115);
  if (!Grown)
    return 2;
  memcpy(Out + 4099, Grown, 4115);
  void *Zero = calloc(3, 7);
  if (!Zero)
    return 3;
  memcpy(Out + 8214, Zero, 21);
  free(Zero);
  unsigned char *Small = realloc(Grown, 5);
  if (!Small)
    return 4;
  memcpy(Out + 8235, Small, 5);
  void *New = realloc((void *)0, 11);
  if (!New)
    return 5;
  memcpy(Out + 8240, New, 11);
  free(New);
  if (realloc(Small, ~(U64)0) || *__errno() != 12)
    return 6;
  memcpy(Out + 8251, Small, 5);
  if (malloc(~(U64)0) || *__errno() != 12 || calloc(~(U64)0, 2) ||
      *__errno() != 12)
    return 7;
  free(Small);
  void *Empty = malloc(0), *Other = calloc(0, 13);
  if (!Empty || !Other || Empty == Other)
    return 8;
  free(Empty);
  free(Other);
  return 0;
}

U64 allocation_hold(void) { return (U64)malloc(4099); }

U64 allocation_foreign(unsigned char *Out, unsigned Mode) {
  void *Pointer = Out;
  if (Mode == 1)
    Pointer = (unsigned char *)malloc(16) + 1;
  if (Mode == 2) {
    Pointer = malloc(16);
    free(Pointer);
  }
  if (Mode == 3)
    Pointer = mmap(0, 4096, 0, 0x22, -1, 0);
  if (Mode == 4) {
    Pointer = malloc(16);
    if (!Pointer || mprotect(Pointer, 4096, 0))
      return 1;
  }
  return (U64)realloc(Pointer, 32);
}
