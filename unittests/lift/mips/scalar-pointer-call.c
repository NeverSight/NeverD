//===- scalar-pointer-call.c - Pointer across a native call ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Owned DWARF fixture: a pointer spill beside the saved incoming link.
//===----------------------------------------------------------------------===//

struct Device {
  unsigned flags;
  unsigned value;
};

#ifdef PRIVATE_CALLEE
static
#endif
    __attribute__((noinline)) void touch(void) {
  __asm__ volatile("" ::: "memory");
}

__attribute__((noinline)) unsigned probe(struct Device *d) {
  struct Device *volatile saved = d;
  touch();
  return saved->value;
}
