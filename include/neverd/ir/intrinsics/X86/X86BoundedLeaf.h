//===- X86BoundedLeaf.h - Bounded SGX and SMX completion --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// A proven constant natural-width AX selects a normal-completion leaf.
/// Native execution owns privilege, hidden processor state and memory faults.
/// This contract supplies no enclave entry/exit or SMX launch continuation.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_INTRINSICS_X86_X86BOUNDEDLEAF_H
#define NEVERD_IR_INTRINSICS_X86_X86BOUNDEDLEAF_H

#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/ir/intrinsics/X86/X86ArithmeticFlags.h"

#include <array>
#include <optional>

namespace neverd {

inline constexpr unsigned X86BoundedLeafAuxiliaryOutputs = 4;
inline constexpr unsigned X86BoundedLeafRegisterCount = 4;
inline constexpr unsigned X86BoundedLeafFlagOperand = 5;
inline constexpr const char *X86BoundedLeafUnproven =
    "x86 ENCLU/GETSEC requires a proven bounded leaf and complete state";

constexpr bool isX86BoundedLeafPending(Intrinsic Id) {
  return Id == Intrinsic::X86EncluPending || Id == Intrinsic::X86GetsecPending;
}
constexpr bool isX86BoundedLeafIntrinsic(Intrinsic Id) {
  return Id == Intrinsic::X86EncluBounded || Id == Intrinsic::X86GetsecBounded;
}
constexpr bool isX86EncluLeaf(Intrinsic Id) {
  return Id == Intrinsic::X86EncluPending || Id == Intrinsic::X86EncluBounded;
}
constexpr Intrinsic x86BoundedLeafResolvedId(Intrinsic Id) {
  return isX86EncluLeaf(Id) ? Intrinsic::X86EncluBounded
                            : Intrinsic::X86GetsecBounded;
}

enum class X86BoundedLeafFlags { Preserve, Define, Capture };
struct X86BoundedLeafContract {
  // AX/BX/CX/DX bits. Reads include old values of conditionally written
  // registers; preserved registers keep their original SSA definitions.
  unsigned Reads;
  unsigned Writes;
  X86BoundedLeafFlags Flags;
};

constexpr std::optional<X86BoundedLeafContract>
x86BoundedLeafContract(Intrinsic Id, uint64_t Ax) {
  if (!isX86BoundedLeafIntrinsic(Id) && !isX86BoundedLeafPending(Id))
    return std::nullopt;
  using F = X86BoundedLeafFlags;
  if (isX86EncluLeaf(Id)) {
    switch (static_cast<uint32_t>(Ax)) {
    case 0:
      return X86BoundedLeafContract{0xe, 0, F::Preserve}; // EREPORT
    case 1:                                               // EGETKEY
    case 5:                                               // EACCEPT
    case 11:
      return X86BoundedLeafContract{0x6, 1, F::Define}; // EGETKEY256
    case 6:
      return X86BoundedLeafContract{0x6, 0, F::Preserve}; // EMODPE
    case 7:                                               // EACCEPTCOPY
    case 10:
      return X86BoundedLeafContract{0xe, 1, F::Define}; // EREPORT2
    // SDM 093's pseudocode writes flags while its Flags Affected says None.
    // Transport the actual native flags, with the original flags as input.
    case 8:
      return X86BoundedLeafContract{0x2, 1, F::Capture}; // EVERIFYREPORT2
    case 9:
      return X86BoundedLeafContract{0, 0, F::Preserve}; // EDECCSSA
    default:
      return std::nullopt;
    }
  }
  switch (static_cast<uint32_t>(Ax)) {
  case 0:
    return X86BoundedLeafContract{0x2, 1, F::Preserve}; // CAPABILITIES
  case 6:
    return X86BoundedLeafContract{0x6, 7, F::Preserve}; // PARAMETERS
  case 7:
    return X86BoundedLeafContract{0x2, 0, F::Preserve}; // SMCTRL
  case 8:
    return X86BoundedLeafContract{0, 0, F::Preserve}; // WAKEUP
  default:
    return std::nullopt;
  }
}

struct X86BoundedLeafShape {
  Arch TargetArch = Arch::Unknown;
  NdMemoryOrdering MemoryOrdering = NdMemoryOrdering::None;
  NdMemoryAddressSpace MemoryAddressSpace = NdMemoryAddressSpace::Default;
  bool ValidId = false;
  bool WritableOutput = false;
  uint16_t OutputSize = 0;
  unsigned Arguments = 0;
  std::array<uint16_t, 5> InputSizes{};
  std::array<bool, 5> ScalarInputs{};
  std::optional<uint64_t> Ax;
};

constexpr bool x86BoundedLeafShapeIsValid(Intrinsic Id,
                                          const X86BoundedLeafShape &S) {
  const bool Pending = isX86BoundedLeafPending(Id);
  if ((!Pending && (!isX86BoundedLeafIntrinsic(Id) || !S.Ax ||
                    !x86BoundedLeafContract(Id, *S.Ax))) ||
      !S.ValidId || !S.WritableOutput || S.Arguments != S.InputSizes.size() ||
      S.MemoryOrdering != NdMemoryOrdering::None ||
      S.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
      (S.OutputSize != 4 && S.OutputSize != 8) ||
      (S.TargetArch != Arch::Unknown && S.TargetArch != Arch::X86 &&
       S.TargetArch != Arch::X64) ||
      (S.TargetArch == Arch::X86 && S.OutputSize != 4) ||
      (S.TargetArch == Arch::X64 && S.OutputSize != 8) ||
      (S.Ax && S.OutputSize == 4 && *S.Ax > UINT32_MAX))
    return false;
  for (unsigned I = 0; I < S.InputSizes.size(); ++I)
    if (!S.ScalarInputs[I] ||
        S.InputSizes[I] != (I == 4 ? X86ArithmeticFlagsBytes : S.OutputSize))
      return false;
  return true;
}

} // namespace neverd

#endif // NEVERD_IR_INTRINSICS_X86_X86BOUNDEDLEAF_H
