//===- darwin_guard_parent.cpp - Guarded dependent constructor -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "darwin_guard_startup.h"
__attribute__((noinline)) static GuardItem &local() {
  static GuardItem Item('B', 'b', 29);
  return Item;
}
struct Parent {
  Parent() {
    emit('P');
    if (guard_leaf_value() != 19 || local().Value != 29)
      _exit(94);
    local().Value = 31;
  }
  ~Parent() {
    if (local().Value != 31)
      _exit(95);
    emit('p');
  }
};
static Parent Object;
extern "C" int guard_parent_value() { return local().Value; }
