//===- darwin_guard_startup.h - Original guarded C++ graph ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file SDK-free default static guards and process-lifetime observations.
extern "C" {
long write(int, const void *, unsigned long);
int atexit(void (*)(void));
int *__error(void);
void exit(int) __attribute__((noreturn));
void _exit(int) __attribute__((noreturn));
int __cxa_guard_acquire(unsigned long *);
void __cxa_guard_release(unsigned long *);
void __cxa_guard_abort(unsigned long *);
extern const char *GuardMode;
int guard_leaf_value(void);
int guard_parent_value(void);
}
static void emit(char C) {
  if (write(1, &C, 1) != 1)
    _exit(90);
}
static bool same(const char *A, const char *B) {
  for (unsigned I = 0; A && B && I < 128; ++I) {
    if (A[I] != B[I])
      return false;
    if (!A[I])
      return true;
  }
  return false;
}
struct GuardItem {
  int Value;
  char Destruction;
  GuardItem(char Construction, char Destruction, int Value)
      : Value(Value), Destruction(Destruction) {
    emit(Construction);
  }
  ~GuardItem() { emit(Destruction); }
};
