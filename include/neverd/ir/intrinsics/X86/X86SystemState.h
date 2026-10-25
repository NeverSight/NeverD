//===- X86SystemState.h - System instruction contracts -------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Operand widths and completion for bounded x86 system instructions. Hidden
/// processor state remains owned by the instruction, including faults and
/// conditional descriptor accesses. This is not a software CPU-state model.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_INTRINSICS_X86_X86SYSTEMSTATE_H
#define NEVERD_IR_INTRINSICS_X86_X86SYSTEMSTATE_H

#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/ir/intrinsics/X86/X86SegmentedBlock.h"

#include <array>
#include <cstdint>
#include <optional>

namespace neverd {

struct X86SystemStateShape {
  Arch TargetArch = Arch::Unknown;
  NdMemoryOrdering MemoryOrdering = NdMemoryOrdering::None;
  NdMemoryAddressSpace MemoryAddressSpace = NdMemoryAddressSpace::Default;
  std::optional<uint16_t> SegmentedBlockSelector;
  std::optional<uint8_t> TransactionAbortCode;
  bool ValidId = false;
  bool WritableOutput = false;
  uint16_t OutputSize = 0;
  uint8_t Arguments = 0;
  std::array<uint16_t, 3> InputSizes{};
  std::array<bool, 3> ScalarInputs{};
};

struct X86SystemStateLayout {
  uint16_t OutputSize = 0;
  uint8_t Arguments = 0;
  std::array<uint16_t, 3> InputSizes{};
  // Two-output forms pack the value below a byte containing ZF. Flag-only
  // forms return a byte containing their documented completion flag.
  uint16_t ValueSize = 0;
  bool HasFlag = false;
};

inline std::optional<X86SystemStateLayout>
x86SystemStateLayout(Intrinsic Id, Arch TargetArch, uint16_t Width,
                     std::optional<uint16_t> Selector = std::nullopt) {
  if (TargetArch != Arch::Unknown && TargetArch != Arch::X86 &&
      TargetArch != Arch::X64)
    return std::nullopt;
  const auto C = x86SystemStateContract(Id);
  if (!C.Mnemonic)
    return std::nullopt;
  using K = X86SystemStateKind;
  switch (C.Kind) {
  case K::SegmentedBlock: {
    if (TargetArch == Arch::X64 || !Selector ||
        !x86SegmentedBlockSelectorIsValid(*Selector))
      return std::nullopt;
    const bool Flag = x86SegmentedBlockHasFlag(*Selector);
    return X86SystemStateLayout{
        static_cast<uint16_t>(Flag), 3, {2, 4, 4}, 0, Flag};
  }
  case K::ReadPid:
    if ((Width != 4 && Width != 8) || (TargetArch == Arch::X86 && Width != 4) ||
        (TargetArch == Arch::X64 && Width != 8))
      return std::nullopt;
    return X86SystemStateLayout{Width, 0, {}, Width};
  case K::ReadBase:
  case K::WriteBase:
    if (TargetArch == Arch::X86 || (Width != 4 && Width != 8))
      return std::nullopt;
    if (C.Kind == K::ReadBase)
      return X86SystemStateLayout{Width, 0, {}, Width};
    return X86SystemStateLayout{0, 1, {Width}};
  case K::Void:
    return X86SystemStateLayout{};
  case K::TransactionAbort:
    return X86SystemStateLayout{0, 1, {1}};
  case K::Void64:
  case K::TestUserInterrupt:
  case K::SendUserInterrupt:
  case K::LoadKernelGs:
    if (TargetArch == Arch::X86)
      return std::nullopt;
    if (C.Kind == K::Void64)
      return X86SystemStateLayout{};
    if (C.Kind == K::TestUserInterrupt)
      return X86SystemStateLayout{1, 0, {}, 0, true};
    if (C.Kind == K::SendUserInterrupt)
      return X86SystemStateLayout{0, 1, {8}};
    return X86SystemStateLayout{
        0, 1, {C.MemoryArgument == 0 ? uint16_t{8} : uint16_t{2}}};
  case K::HistoryReset:
    // Intel specifies that imm8 is ignored. Only EAX controls the reset.
    return X86SystemStateLayout{0, 1, {4}};
  case K::InvalidateBroadcast:
    if ((Width != 4 && Width != 8) || (TargetArch == Arch::X86 && Width != 4))
      return std::nullopt;
    // rAX contains the address AND control bits; it is not a memory operand.
    return X86SystemStateLayout{0, 3, {Width, 4, 4}};
  case K::Monitor:
  case K::CacheZero:
    // Addresses use LowIR's canonical 64-bit carrier on both architectures.
    return X86SystemStateLayout{0, 1, {8}};
  case K::Wait:
    return X86SystemStateLayout{1, 3, {4, 4, 4}, 0, true};
  case K::AccessRights:
    if ((Width != 2 && Width != 4 && Width != 8) ||
        (TargetArch == Arch::X86 && Width == 8))
      return std::nullopt;
    return X86SystemStateLayout{
        static_cast<uint16_t>(2 * Width),
        1,
        {C.MemoryArgument == 0 ? uint16_t{8} : uint16_t{2}},
        Width,
        true};
  case K::AdjustRpl:
    if (TargetArch == Arch::X64)
      return std::nullopt;
    if (C.MemoryArgument == 0)
      return X86SystemStateLayout{1, 2, {8, 2}, 0, true};
    return X86SystemStateLayout{4, 2, {2, 2}, 2, true};
  case K::ShadowWrite:
  case K::DirectStore:
    if ((Width != 4 && Width != 8) || (TargetArch == Arch::X86 && Width != 4))
      return std::nullopt;
    return X86SystemStateLayout{0, 2, {8, Width}};
  case K::Enqueue:
    // Non-long-mode ES bases and limits need an explicit segment contract.
    if (TargetArch == Arch::X86)
      return std::nullopt;
    return X86SystemStateLayout{1, 2, {8, 8}, 0, true};
  case K::DirectStore64B:
    if (TargetArch == Arch::X86)
      return std::nullopt;
    return X86SystemStateLayout{0, 2, {8, 8}};
  case K::WriteMsr:
    // ECX selects the MSR; EAX and EDX supply its low and high halves.
    return X86SystemStateLayout{0, 3, {4, 4, 4}};
  case K::MsrList:
    if (TargetArch == Arch::X86)
      return std::nullopt;
    // Two linear table addresses and a read/write mask. The instruction owns
    // every table access and restartable partial completion in RCX.
    return X86SystemStateLayout{8, 3, {8, 8, 8}, 8};
  }
  return std::nullopt;
}

inline uint16_t x86SystemStateWidth(Intrinsic Id,
                                    const X86SystemStateShape &Shape) {
  switch (x86SystemStateContract(Id).Kind) {
  case X86SystemStateKind::ShadowWrite:
  case X86SystemStateKind::DirectStore:
    return Shape.InputSizes[1];
  case X86SystemStateKind::WriteBase:
  case X86SystemStateKind::InvalidateBroadcast:
    return Shape.InputSizes[0];
  case X86SystemStateKind::AccessRights:
    return Shape.OutputSize / 2;
  default:
    return Shape.OutputSize;
  }
}

inline uint16_t x86SystemStateMemoryBytes(Intrinsic Id, uint16_t Width) {
  switch (x86SystemStateContract(Id).Kind) {
  case X86SystemStateKind::Monitor:
  case X86SystemStateKind::CacheZero:
    // CLZERO owns the containing cache line, whose size is CPU-defined. This
    // carrier must not cause an eager load, an alignment guard or a fixed-size
    // software clear of bytes starting at the possibly unaligned address.
    return 1;
  case X86SystemStateKind::ShadowWrite:
  case X86SystemStateKind::DirectStore:
    return Width;
  case X86SystemStateKind::Enqueue:
  case X86SystemStateKind::DirectStore64B:
    return 64;
  case X86SystemStateKind::MsrList:
    // A table entry, not an unconditional access to the whole table. Sparse
    // and empty masks must not cause eager loads or page/alignment checks.
    return 8;
  default:
    return 2;
  }
}

inline bool x86SystemStateShapeIsValid(Intrinsic Id,
                                       const X86SystemStateShape &Shape) {
  const auto C = x86SystemStateContract(Id);
  const auto L =
      x86SystemStateLayout(Id, Shape.TargetArch, x86SystemStateWidth(Id, Shape),
                           Shape.SegmentedBlockSelector);
  if (!L || !Shape.ValidId || Shape.MemoryOrdering != NdMemoryOrdering::None ||
      !isKnownMemoryAddressSpace(Shape.MemoryAddressSpace) ||
      ((C.MemoryArgument < 0 || C.Kind == X86SystemStateKind::MsrList) &&
       Shape.MemoryAddressSpace != NdMemoryAddressSpace::Default) ||
      Shape.OutputSize != L->OutputSize ||
      (Shape.OutputSize && !Shape.WritableOutput) ||
      Shape.Arguments != L->Arguments)
    return false;
  if (C.Kind == X86SystemStateKind::TransactionAbort &&
      !Shape.TransactionAbortCode)
    return false;
  for (uint8_t I = 0; I < L->Arguments; ++I)
    if (!Shape.ScalarInputs[I] || Shape.InputSizes[I] != L->InputSizes[I])
      return false;
  return true;
}

} // namespace neverd

#endif // NEVERD_IR_INTRINSICS_X86_X86SYSTEMSTATE_H
