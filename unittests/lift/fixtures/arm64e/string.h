//===- string.h - Freestanding arm64e fixture declarations ------*- C -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// C declarations for compile-only source projections without a Darwin SDK.
/// Clang's resource headers supply stddef/stdint and the ARM intrinsic types.
/// No library implementation or native arm64e execution is provided here.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_UNITTESTS_LIFT_FIXTURES_ARM64E_STRING_H
#define NEVERD_UNITTESTS_LIFT_FIXTURES_ARM64E_STRING_H

#include <stddef.h>

void *memcpy(void *, const void *, size_t);
void *memmove(void *, const void *, size_t);
void *memset(void *, int, size_t);

#endif // NEVERD_UNITTESTS_LIFT_FIXTURES_ARM64E_STRING_H
