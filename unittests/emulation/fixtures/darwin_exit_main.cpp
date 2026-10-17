//===- darwin_exit_main.cpp - Original global and static destruction ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file SDK-free C/C++ registration, ordering and original-object controls.
#include "darwin_exit_startup.h"
static void from_tls() { emit('Y'); }
struct TLS {
  int Value;
  TLS() : Value(43) { emit('T'); }
  ~TLS() {
    if (Value != 47)
      _exit(98);
    emit('t');
    reg(from_tls);
  }
};
static thread_local TLS ThreadObject;
static void boot() { emit('0'); }
__attribute__((constructor(101))) static void before_main() {
  emit('B');
  if (ThreadObject.Value != 43)
    _exit(99);
  ThreadObject.Value = 47;
  reg(boot);
  if (same(ExitMode, "ctor-exit"))
    exit(37);
  if (same(ExitMode, "ctor-raw"))
    _exit(38);
}
static void global_c() { emit('3'); }
struct Global {
  Global() {
    emit('G');
    reg(global_c);
  }
  ~Global() { emit('g'); }
};
static Global Object;
static void static_c() { emit('4'); }
struct Local {
  int Value;
  Local() : Value(71) {
    emit('S');
    reg(static_c);
  }
  ~Local() {
    if (Value != 73)
      _exit(100);
    emit('s');
  }
};
__attribute__((noinline)) static Local *local() {
  static Local S;
  return &S;
}
static void ordinary() { emit('N'); }
static void global_raw() {
  emit('R');
  _exit(38);
}
static void global_reentrant() {
  emit('E');
  exit(99);
}
static void nested() { emit('n'); }
static void ignored_tls(void *) { emit('X'); }
static void late() {
  emit('L');
  reg(nested);
  _tlv_atexit(ignored_tls, 0);
}
static void opaque(void *O) {
  if ((unsigned long)O != 0xfedcba9876543210UL)
    _exit(101);
  emit('I');
}
static void null_object(void *O) {
  if (O)
    _exit(102);
  emit('Z');
}
struct StackCanary {
  unsigned long Words[64];
};
static void stack_cleanup(void *O) {
  const volatile StackCanary *K = (const volatile StackCanary *)O;
  for (unsigned I = 0; I < 64; ++I)
    if (K->Words[I] != 0x1122334455667700UL + I)
      _exit(103);
  emit('K');
}
__attribute__((noinline)) static void deep_exit() {
  volatile unsigned long Pad[128];
  for (unsigned I = 0; I < 128; ++I)
    Pad[I] = I + 33;
  __asm__ volatile("" : : "r"(Pad) : "memory");
  exit(37);
}
__attribute__((noinline)) static void with_stack() {
  volatile StackCanary K;
  for (unsigned I = 0; I < 64; ++I)
    K.Words[I] = 0x1122334455667700UL + I;
  reg_object(stack_cleanup, (void *)&K, (void *)0xffffffffffffffffUL);
  deep_exit();
}
extern "C" int main(int A, char **V) {
  if (A != 2 || !same(V[1], ExitMode) || leaf_value() != 17 ||
      parent_value() != 29 || ThreadObject.Value != 47)
    return 104;
  emit('M');
  reg(ordinary);
  Local *First = local();
  if (First->Value != 71 || local() != First)
    return 105;
  First->Value = 73;
  if (local()->Value != 73)
    return 106;
  reg(late);
  if (same(ExitMode, "opaque")) {
    reg_object(opaque, (void *)0xfedcba9876543210UL,
               (void *)0xffffffffffffffffUL);
    reg_object(null_object, 0, (void *)0xfedcba9876543210UL);
    reg(nested);
    reg(nested);
  }
  if (same(ExitMode, "global-raw"))
    reg(global_raw);
  if (same(ExitMode, "global-reentrant"))
    reg(global_reentrant);
  if (same(ExitMode, "deep-exit"))
    with_stack();
  if (same(ExitMode, "exit"))
    exit(37);
  if (same(ExitMode, "raw"))
    _exit(38);
  return 37;
}
