//===- darwin_exit_leaf.cpp - Original leaf global destruction ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file SDK-free C/C++ registration, ordering and original-object controls.
#include "darwin_exit_startup.h"
extern "C" {
const char *ExitMode;
}
__attribute__((constructor(101))) static void capture(int A, char **V) {
  if (A != 2 || !V || !V[1])
    _exit(93);
  ExitMode = V[1];
}
static void leaf_c() { emit('1'); }
static void from_leaf_tls() { emit('U'); }
static void leaf_tls(void *O) {
  if ((unsigned long)O != 0x11)
    _exit(94);
  emit('v');
  reg(from_leaf_tls);
}
struct Leaf {
  int Value;
  Leaf() : Value(17) {
    emit('L');
    reg(leaf_c);
    _tlv_atexit(leaf_tls, (void *)0x11);
  }
  ~Leaf() {
    if (Value != 17)
      _exit(95);
    emit('l');
  }
};
static Leaf Object;
extern "C" int leaf_value() { return Object.Value; }
