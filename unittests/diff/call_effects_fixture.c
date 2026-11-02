//===- call_effects_fixture.c - Owned ELF import-call fixture -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Real import slots and internal call paths. Tests never execute this image.
//===----------------------------------------------------------------------===//
#ifndef PATCH_RESULT
#define PATCH_RESULT 17
#endif

extern void *memcpy(void *destination, const void *source, unsigned long size);
extern void free(void *object);
extern void project_release(void *object);

__attribute__((noinline, visibility("hidden"))) int
effect_helper(void *destination, const void *source, unsigned long size,
              void *owner) {
  memcpy(destination, source, size);
  free(owner);
  project_release(destination);
  return PATCH_RESULT;
}

__attribute__((noinline)) int effect_entry(void *destination,
                                           const void *source,
                                           unsigned long size, void *owner) {
  return effect_helper(destination, source, size, owner) + 3;
}
