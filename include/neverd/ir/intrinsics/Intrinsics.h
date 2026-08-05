//===- Intrinsics.h - Intrinsic function definitions --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Defines the Intrinsic enumeration for INTRINSIC operations and
/// provides name/metadata query functions for each intrinsic.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_INTRINSICS_INTRINSICS_H
#define NEVERD_IR_INTRINSICS_INTRINSICS_H

#include "neverd/Common.h"

namespace neverd {

/// Intrinsic IDs for INTRINSIC operations.
///
/// Each architecture gets a 5000-slot range so entries never collide:
///   Common   :     0 -   999
///   x86/x64  :  1000 -  5999
///   AArch64  :  6000 - 10999
///   ARM32    : 11000 - 15999
enum class Intrinsic : uint16_t {
  None = 0,

  Syscall,

#include "neverd/ir/intrinsics/intrinsics_aarch64.inc"
#include "neverd/ir/intrinsics/intrinsics_arm.inc"
#include "neverd/ir/intrinsics/intrinsics_x86.inc"

  _Count = 16000
};

const char *intrinsicName(Intrinsic Id);
const char *intrinsicCName(Intrinsic Id);
const char *intrinsicAsmMnemonic(Intrinsic Id);
const char *llvmIntrinsicToCName(const char *LLVMName);
Intrinsic intrinsicFromName(const char *Name);
bool isSideeffectIntrinsic(Intrinsic Id);
uint8_t intrinsicOutputCount(Intrinsic Id);

} // namespace neverd

#endif // NEVERD_IR_INTRINSICS_INTRINSICS_H
