//===- n32-pointer-call.c - Narrow pointer across a native call -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Owned n32 DWARF fixture: four-byte pointer spill, eight-byte saved link.
//===----------------------------------------------------------------------===//

struct Device {
  unsigned flags;
  unsigned value;
};

__attribute__((noinline)) void touch(void) {
  __asm__ volatile("" ::: "memory");
}

__attribute__((noinline)) unsigned probe(struct Device *d) {
  struct Device *volatile saved = d;
  touch();
  return saved->value;
}
