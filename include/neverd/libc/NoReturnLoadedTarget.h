//===- NoReturnLoadedTarget.h - Loaded no-return targets --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIBC_NORETURNLOADEDTARGET_H
#define NEVERD_LIBC_NORETURNLOADEDTARGET_H

#include "neverd/Common.h"

namespace neverd {

struct BinaryImage;

namespace libc {

class NoReturnTargetIndex;

/// Whether a call through the pointer stored at Slot cannot return. Loader
/// import bindings retain their declared contract; an ordinary pointer needs
/// the shared immutable code-pointer proof and its target's no-return fact.
/// This does not give a direct call to the storage address that contract.
bool isNoReturnLoadedTarget(const BinaryImage &Image, va_t Slot,
                            const NoReturnTargetIndex *Index = nullptr);

} // namespace libc
} // namespace neverd

#endif // NEVERD_LIBC_NORETURNLOADEDTARGET_H
