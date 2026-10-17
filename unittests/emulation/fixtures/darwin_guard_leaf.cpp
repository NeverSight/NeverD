//===- darwin_guard_leaf.cpp - Guarded dependency constructor ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "darwin_guard_startup.h"
extern "C" {
const char *GuardMode;
}
__attribute__((constructor(101))) static void capture(int A, char **V) {
  if (A != 2 || !V || !V[1])
    _exit(91);
  GuardMode = V[1];
}
__attribute__((noinline)) static GuardItem &local() {
  static GuardItem Item('A', 'a', 17);
  return Item;
}
struct Leaf {
  Leaf() {
    emit('L');
    if (local().Value != 17 || &local() != &local())
      _exit(92);
    local().Value = 19;
  }
  ~Leaf() {
    if (local().Value != 19)
      _exit(93);
    emit('l');
  }
};
static Leaf Object;
extern "C" int guard_leaf_value() { return local().Value; }
