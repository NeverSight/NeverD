//===- darwin_exit_parent.cpp - Original parent global destruction ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file SDK-free C/C++ registration, ordering and original-object controls.
#include "darwin_exit_startup.h"
static void parent_c() { emit('2'); }
struct Parent {
  int Value;
  Parent() : Value(29) {
    if (leaf_value() != 17)
      _exit(96);
    emit('P');
    reg(parent_c);
  }
  ~Parent() {
    if (Value != 29)
      _exit(97);
    emit('p');
  }
};
static Parent Object;
extern "C" int parent_value() { return Object.Value; }
