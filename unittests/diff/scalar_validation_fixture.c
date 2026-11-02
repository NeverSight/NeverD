//===- scalar_validation_fixture.c - Owned scalar return controls --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Pure integer and memory controls. Tests never execute the produced image.
//===----------------------------------------------------------------------===//
#ifndef PATCH_RESULT
#define PATCH_RESULT 0
#endif

__attribute__((visibility("default"), noinline)) unsigned
scalar_entry(unsigned x) {
  return (((x + 3u) ^ 23u) * 7u) + 9u + PATCH_RESULT;
}

__attribute__((visibility("default"), noinline)) unsigned
scalar_memory(const unsigned *p) {
  return *p + 3u;
}
