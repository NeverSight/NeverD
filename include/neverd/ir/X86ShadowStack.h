//===- X86ShadowStack.h - Conditional shadow stack reads -------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_X86SHADOWSTACK_H
#define NEVERD_IR_X86SHADOWSTACK_H

#include "neverd/ir/intrinsics/Intrinsics.h"

namespace neverd {

/// RDSSP defines the complete destination. With shadow stacks disabled the
/// old value survives, including the high half of an x64 RDSSPD destination.
/// Inputs are [ID:2, old full GPR, encoded read width:1]; one full GPR result.
struct X86ShadowStackReadShape {
  Arch TargetArch = Arch::Unknown;
  NdMemoryOrdering MemoryOrdering = NdMemoryOrdering::None;
  NdMemoryAddressSpace MemoryAddressSpace = NdMemoryAddressSpace::Default;
  unsigned NumInputs = 0;
  bool IdIsConst = false;
  unsigned IdSize = 0;
  bool OutputIsWritable = false;
  unsigned OutputSize = 0;
  bool OldIsScalar = false;
  unsigned OldSize = 0;
  bool WidthIsConst = false;
  unsigned WidthSize = 0;
  uint64_t ReadWidth = 0;
  bool HasAuxiliaryOutputs = false;
  bool ArchitectureMatchesOperands = true;
};

constexpr bool
x86ShadowStackReadShapeIsValid(const X86ShadowStackReadShape &S) {
  const unsigned Word = S.TargetArch == Arch::X64 ? 8 : 4;
  return (S.TargetArch == Arch::X86 || S.TargetArch == Arch::X64) &&
         S.ArchitectureMatchesOperands &&
         S.MemoryOrdering == NdMemoryOrdering::None &&
         S.MemoryAddressSpace == NdMemoryAddressSpace::Default &&
         S.NumInputs == 3 && S.IdIsConst && S.IdSize == 2 &&
         S.OutputIsWritable && S.OutputSize == Word && S.OldIsScalar &&
         S.OldSize == Word && S.WidthIsConst && S.WidthSize == 1 &&
         (S.ReadWidth == 4 || (Word == 8 && S.ReadWidth == 8)) &&
         !S.HasAuxiliaryOutputs;
}

} // namespace neverd

#endif
