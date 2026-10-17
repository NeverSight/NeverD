//===- darwin_exit_startup.h - Global-exit controls ------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file SDK-free C/C++ registration, ordering and original-object controls.
extern "C" {
long write(int, const void *, unsigned long);
int atexit(void (*)(void));
int __cxa_atexit(void (*)(void *), void *, void *);
void _tlv_atexit(void (*)(void *), void *);
int *__error(void);
void exit(int) __attribute__((noreturn));
void _exit(int) __attribute__((noreturn));
extern const char *ExitMode;
int leaf_value(void);
int parent_value(void);
}
static void emit(char C) {
  if (write(1, &C, 1) != 1)
    _exit(90);
}
static bool same(const char *A, const char *B) {
  if (!A || !B)
    return false;
  for (unsigned I = 0; I < 128; ++I) {
    if (A[I] != B[I])
      return false;
    if (!A[I])
      return true;
  }
  return false;
}
static void reg(void (*F)(void)) {
  *__error() = 83;
  if (atexit(F) != 0 || *__error() != 83)
    _exit(91);
}
static void reg_object(void (*F)(void *), void *O, void *DSO) {
  *__error() = 84;
  if (__cxa_atexit(F, O, DSO) != 0 || *__error() != 84)
    _exit(92);
}
