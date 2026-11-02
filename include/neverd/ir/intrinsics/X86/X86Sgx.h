//===- X86Sgx.h - Bounded SGX supervisor completion -----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// ENCLS/ENCLV retain native enclave memory, privilege and fault ordering.
/// Their primary result is rAX; auxiliary results are rBX and CF:ZF bytes.
/// ENCLU transitions require a separate architectural continuation contract.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_INTRINSICS_X86_X86SGX_H
#define NEVERD_IR_INTRINSICS_X86_X86SGX_H

#include "neverd/ir/intrinsics/Intrinsics.h"

#include <array>
#include <cstdint>

namespace neverd {

constexpr bool isX86SgxIntrinsic(Intrinsic Id) {
  return Id == Intrinsic::X86Encls || Id == Intrinsic::X86Enclv;
}

// Intel SDM Vol. 3D, ENCLS leaf functions. These leaves leave arithmetic
// flags unchanged; every other defined supervisor/VMM leaf clears PF/AF/SF/OF.
inline constexpr std::array<uint32_t, 5> X86SgxPreservedFlagLeaves = {
    0x00, 0x01, 0x06, 0x0a, 0x0d};
inline constexpr unsigned X86SgxAuxiliaryOutputs = 2;
inline constexpr uint16_t X86SgxFlagBytes = 2;

struct X86SgxShape {
  Arch TargetArch = Arch::Unknown;
  NdMemoryOrdering MemoryOrdering = NdMemoryOrdering::None;
  NdMemoryAddressSpace MemoryAddressSpace = NdMemoryAddressSpace::Default;
  bool ValidId = false;
  bool WritableOutput = false;
  uint16_t OutputSize = 0;
  unsigned Arguments = 0;
  std::array<uint16_t, 4> InputSizes{};
  std::array<bool, 4> ScalarInputs{};
};

constexpr bool x86SgxShapeIsValid(Intrinsic Id, const X86SgxShape &S) {
  if (!isX86SgxIntrinsic(Id) || !S.ValidId || !S.WritableOutput ||
      S.Arguments != S.InputSizes.size() ||
      S.MemoryOrdering != NdMemoryOrdering::None ||
      S.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
      (S.OutputSize != 4 && S.OutputSize != 8) ||
      (S.TargetArch != Arch::Unknown && S.TargetArch != Arch::X86 &&
       S.TargetArch != Arch::X64) ||
      (S.TargetArch == Arch::X86 && S.OutputSize != 4) ||
      (S.TargetArch == Arch::X64 && S.OutputSize != 8))
    return false;
  for (unsigned I = 0; I < S.InputSizes.size(); ++I)
    if (!S.ScalarInputs[I] || S.InputSizes[I] != S.OutputSize)
      return false;
  return true;
}

} // namespace neverd

#endif // NEVERD_IR_INTRINSICS_X86_X86SGX_H
