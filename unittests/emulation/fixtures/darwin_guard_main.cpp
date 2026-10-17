//===- darwin_guard_main.cpp - Default guards across TLS and exit --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "darwin_guard_startup.h"
__attribute__((noinline)) static GuardItem &startup() {
  static GuardItem Item('S', 's', 7);
  return Item;
}
__attribute__((constructor(101))) static void before_main() {
  emit('C');
  if (startup().Value != 7)
    _exit(96);
  startup().Value = 9;
  if (same(GuardMode, "ctor-exit"))
    exit(37);
  if (same(GuardMode, "ctor-raw"))
    _exit(38);
}
__attribute__((noinline)) static GuardItem &from_tls() {
  static GuardItem Item('U', 'u', 41);
  return Item;
}
static void after_tls() { emit('Y'); }
struct ThreadItem {
  ThreadItem() { emit('T'); }
  ~ThreadItem() {
    emit('t');
    if (from_tls().Value != 41 || &from_tls() != &from_tls())
      _exit(97);
    if (atexit(after_tls) != 0)
      _exit(98);
  }
};
static thread_local ThreadItem ThreadObject;
__attribute__((noinline)) static GuardItem &from_global() {
  static GuardItem Item('V', 'v', 43);
  return Item;
}
static void global_late() {
  emit('X');
  if (from_global().Value != 43 || &from_global() != &from_global())
    _exit(99);
}
static void global_raw() {
  emit('Q');
  _exit(38);
}
static void global_reentrant() {
  emit('E');
  exit(99);
}
struct MainItem;
static MainItem &local();
struct MainItem : GuardItem {
  MainItem() : GuardItem('H', 'h', 53) {
    if (same(GuardMode, "recursive"))
      (void)local();
  }
};
__attribute__((noinline)) static MainItem &local() {
  static MainItem Item;
  return Item;
}
static void manual_guard() {
  struct alignas(8) Storage {
    unsigned long Before, Word, After;
  } G{0x1122334455667788UL, 0, 0x8877665544332211UL};
  *__error() = 83;
  if (__cxa_guard_acquire(&G.Word) != 1 || *__error() != 83 ||
      (G.Word & 0xffffffffUL) != 0x200 || !(G.Word >> 32))
    _exit(100);
  const unsigned long Pending = G.Word;
  *__error() = 84;
  __cxa_guard_abort(&G.Word);
  if (G.Word || *__error() != 84)
    _exit(101);
  *__error() = 85;
  if (__cxa_guard_acquire(&G.Word) != 1 || G.Word != Pending ||
      *__error() != 85)
    _exit(102);
  if (same(GuardMode, "foreign"))
    G.Word ^= 1UL << 32;
  *__error() = 86;
  __cxa_guard_release(&G.Word);
  const auto Complete = (Pending & 0xffffffff00000000UL) | 0x101;
  if (G.Word != Complete || *__error() != 86)
    _exit(103);
  *__error() = 87;
  if (__cxa_guard_acquire(&G.Word) != 0 || G.Word != Complete ||
      *__error() != 87 || G.Before != 0x1122334455667788UL ||
      G.After != 0x8877665544332211UL)
    _exit(104);
  emit('R');
}
extern "C" int main(int A, char **V) {
  if (A != 2 || !same(V[1], GuardMode) || guard_leaf_value() != 19 ||
      guard_parent_value() != 31 || startup().Value != 9)
    return 105;
  emit('M');
  (void)ThreadObject;
  if (local().Value != 53 || &local() != &local())
    return 106;
  local().Value = 55;
  if (local().Value != 55)
    return 107;
  manual_guard();
  if (atexit(global_late) != 0)
    return 108;
  if (same(GuardMode, "global-raw") && atexit(global_raw) != 0)
    return 109;
  if (same(GuardMode, "global-reentrant") && atexit(global_reentrant) != 0)
    return 110;
  if (same(GuardMode, "exit"))
    exit(37);
  if (same(GuardMode, "raw"))
    _exit(38);
  return 37;
}
