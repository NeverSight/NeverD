//===- X86Virtualization.h - VMX/SVM instruction contracts ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Explicit operands and completion of architectural virtualization operations.
/// VMCS/VMCB state remains opaque. Memory operands remain addresses so
/// privilege, interception, validation and conditional-access order belong to
/// the instruction.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_INTRINSICS_X86_X86VIRTUALIZATION_H
#define NEVERD_IR_INTRINSICS_X86_X86VIRTUALIZATION_H

#include "neverd/ir/intrinsics/Intrinsics.h"

#include <array>
#include <cstdint>
#include <optional>

namespace neverd {

/// Flags are two bytes containing CF and ZF, respectively. The read form
/// returns a natural-width value followed by these bytes in a double-width
/// carrier. The value is used only on VMsucceed; failure preserves the
/// architectural destination through a separate SELECT in the lifter.
struct X86VirtualizationShape {
  Arch TargetArch = Arch::Unknown;
  NdMemoryOrdering MemoryOrdering = NdMemoryOrdering::None;
  NdMemoryAddressSpace MemoryAddressSpace = NdMemoryAddressSpace::Default;
  bool ValidId = false;
  bool WritableOutput = false;
  uint16_t OutputSize = 0;
  uint8_t Arguments = 0;
  std::array<uint16_t, 5> InputSizes{};
  std::array<bool, 5> ScalarInputs{};
  std::optional<uint64_t> AddressWidth;
};

constexpr bool
x86VirtualizationShapeIsValid(Intrinsic Id,
                              const X86VirtualizationShape &Shape) {
  const auto C = x86VirtualizationContract(Id);
  if (!C.Mnemonic || !Shape.ValidId ||
      (Shape.TargetArch != Arch::Unknown && Shape.TargetArch != Arch::X86 &&
       Shape.TargetArch != Arch::X64) ||
      Shape.MemoryOrdering != NdMemoryOrdering::None ||
      !isKnownMemoryAddressSpace(Shape.MemoryAddressSpace) ||
      (C.MemoryArgument < 0 &&
       Shape.MemoryAddressSpace != NdMemoryAddressSpace::Default) ||
      Shape.Arguments != C.Arguments)
    return false;
  if (Id == Intrinsic::X86SvmRunContext)
    return Shape.TargetArch != Arch::X86 && !Shape.OutputSize &&
           Shape.MemoryAddressSpace == NdMemoryAddressSpace::Default &&
           Shape.ScalarInputs[0] && Shape.ScalarInputs[1] &&
           Shape.InputSizes[0] == 8 && Shape.InputSizes[1] == 4 &&
           (Shape.AddressWidth == 32 || Shape.AddressWidth == 64);
  uint16_t Word = Shape.TargetArch == Arch::X86 ? 4 : 8;
  if (Shape.TargetArch == Arch::Unknown && C.Arguments &&
      C.MemoryArgument != 0 && !C.PhysicalAddress &&
      Id != Intrinsic::VmxFunction)
    Word = Shape.InputSizes[0];
  if (Word != 4 && Word != 8)
    return false;
  const uint16_t ExpectedOutput =
      C.Result == X86VirtualizationResult::Read    ? 2 * Word
      : C.Result == X86VirtualizationResult::Flags ? 2
      : C.Result == X86VirtualizationResult::Value ? Word
                                                   : 0;
  if (Shape.OutputSize != ExpectedOutput ||
      (ExpectedOutput && !Shape.WritableOutput))
    return false;
  for (uint8_t I = 0; I < C.Arguments; ++I) {
    if (!Shape.ScalarInputs[I])
      return false;
    const uint16_t Size = Shape.InputSizes[I];
    if (C.PhysicalAddress && I == 0) {
      if (Size != 4 && Size != 8)
        return false;
      if (Shape.TargetArch == Arch::X86 && Size != 4)
        return false;
    } else if (Size !=
               (I == C.MemoryArgument ? 8
                : Id == Intrinsic::VmxFunction || Id == Intrinsic::SvmInvalidate
                    ? 4
                    : Word))
      return false;
  }
  return true;
}

} // namespace neverd

#endif // NEVERD_IR_INTRINSICS_X86_X86VIRTUALIZATION_H
