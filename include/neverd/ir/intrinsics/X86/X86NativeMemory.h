//===- X86NativeMemory.h - Native segment-offset roles --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Classify native CS/ES/SS address controls independently of IR storage.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_INTRINSICS_X86_X86NATIVEMEMORY_H
#define NEVERD_IR_INTRINSICS_X86_X86NATIVEMEMORY_H

#include "neverd/ir/intrinsics/X86/X86AtomicMemory.h"
#include "neverd/ir/intrinsics/X86/X86CompressExpand.h"
#include "neverd/ir/intrinsics/X86/X86FourSource.h"

#include <cstdint>

namespace neverd {
/// NumInputs includes the intrinsic ID; Control is the final constant input.
constexpr bool x86NativeMemoryHasSegmentOffset(Intrinsic Id, unsigned NumInputs,
                                               uint64_t Control) {
  if (isX86AtomicIntrinsic(Id))
    return NumInputs == x86AtomicAddressControlIndex(Id) + 1 &&
           x86AtomicSegmentPrefix(Control);
  if (isX86CompactMemory(Id) && NumInputs == 5) {
    const auto C = x86CompactControl(Id, Control);
    return C && C->SegmentPrefix;
  }
  if (Id == Intrinsic::X86FourSourceMemory && NumInputs == 6) {
    const auto C = x86FourSourceControl(Control);
    return C && C->SegmentPrefix;
  }
  return false;
}
} // namespace neverd

#endif // NEVERD_IR_INTRINSICS_X86_X86NATIVEMEMORY_H
