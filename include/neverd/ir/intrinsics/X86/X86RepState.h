//===- X86RepState.h - Explicit REP machine completion --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// A REP consumes three natural-width operands, packed flags/DF and one
/// versioned constant descriptor. Hardware returns every modified string
/// register, including high halves under an address-size override.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_INTRINSICS_X86_X86REPSTATE_H
#define NEVERD_IR_INTRINSICS_X86_X86REPSTATE_H

#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/ir/intrinsics/X86/X86ArithmeticFlags.h"

#include <array>
#include <cstdint>
#include <optional>

namespace neverd {

inline constexpr uint32_t X86RepDescriptorVersion = 1u << 24;
inline constexpr uint32_t X86RepDescriptorMask =
    X86RepDescriptorVersion | 0xff7f;
inline constexpr unsigned X86RepAddressShift = 2;
inline constexpr unsigned X86RepRepeatShift = 6;
inline constexpr unsigned X86RepSegmentShift = 8;
inline constexpr unsigned X86RepDirectionShift = 16;
inline constexpr uint16_t X86RepFlagsMask = X86ArithmeticFlagsMask;
inline constexpr uint16_t X86RepStatusBytes = 4;
inline constexpr uint16_t X86RepDescriptorBytes = 4;
inline constexpr uint16_t X86RepFlagBytes = X86ArithmeticFlagsBytes;
inline constexpr auto X86RepFlagBits = X86ArithmeticFlagBits;

struct X86RepContract {
  const char *Name;
  unsigned WordResults;
  bool Compare;
  bool Source;
  bool Port;
  constexpr unsigned auxiliaryOutputs() const {
    return WordResults - 1 + Compare;
  }
};

constexpr std::optional<X86RepContract> x86RepContract(Intrinsic Id) {
  switch (Id) {
#define NEVERD_X86_REP_STATE(Id, Name, Words, Compare, Source, Port)           \
  case Intrinsic::Id:                                                          \
    return X86RepContract{Name, Words, Compare, Source, Port};
#include "neverd/ir/intrinsics/X86/X86RepState.def"
#undef NEVERD_X86_REP_STATE
  default:
    return std::nullopt;
  }
}
constexpr bool isX86RepStateIntrinsic(Intrinsic Id) {
  return x86RepContract(Id).has_value();
}

struct X86RepControl {
  unsigned ElementBytes;
  unsigned AddressBytes;
  unsigned Segment;
  bool NotEqual;
};

struct X86RepAccessSpan {
  uint64_t Begin;
  uint64_t Bytes;
};

/// The complete nonempty access of a bounded MOVS/STOS, inside one mapping.
/// Control must already be validated; callers own flat/native-width admission.
constexpr std::optional<X86RepAccessSpan>
x86RepAccessSpan(uint64_t Address, uint64_t Count, const X86RepControl &Control,
                 bool Backward, uint64_t StorageBegin, uint64_t StorageBytes) {
  if (!Count || StorageBytes > UINT64_MAX - StorageBegin ||
      Count > StorageBytes / Control.ElementBytes || Address < StorageBegin)
    return std::nullopt;
  const uint64_t Bytes = Count * Control.ElementBytes;
  const uint64_t Before = Backward ? Bytes - Control.ElementBytes : 0;
  const uint64_t Offset = Address - StorageBegin;
  if (Offset < Before || Offset - Before > StorageBytes - Bytes)
    return std::nullopt;
  return X86RepAccessSpan{Address - Before, Bytes};
}

constexpr std::optional<X86RepControl> x86RepControl(Intrinsic Id, Arch A,
                                                     uint32_t Descriptor) {
  const auto C = x86RepContract(Id);
  if (!C || (A != Arch::X86 && A != Arch::X64) ||
      (Descriptor & X86RepDescriptorVersion) != X86RepDescriptorVersion ||
      (Descriptor & ~X86RepDescriptorMask))
    return std::nullopt;
  const unsigned Element = 1u << (Descriptor & 3);
  const unsigned Address = (Descriptor >> X86RepAddressShift) & 15;
  const unsigned Segment = (Descriptor >> X86RepSegmentShift) & 255;
  const bool NotEqual = (Descriptor >> X86RepRepeatShift) & 1;
  if ((Element == 8 && (A != Arch::X64 || C->Port)) ||
      !(A == Arch::X64 ? (Address == 4 || Address == 8)
                       : (Address == 2 || Address == 4)) ||
      !(Segment == 0 || Segment == 0x26 || Segment == 0x2e || Segment == 0x36 ||
        Segment == 0x3e || Segment == 0x64 || Segment == 0x65) ||
      (!C->Source && Segment) || (!C->Compare && NotEqual))
    return std::nullopt;
  return X86RepControl{Element, Address, Segment, NotEqual};
}

constexpr std::optional<uint32_t>
x86RepDescriptor(Intrinsic Id, Arch A, unsigned Element, unsigned Address,
                 unsigned Segment, bool NotEqual) {
  if ((Element != 1 && Element != 2 && Element != 4 && Element != 8) ||
      Address > 15 || Segment > 255)
    return std::nullopt;
  const uint32_t Descriptor = X86RepDescriptorVersion |
                              (Element == 8   ? 3u
                               : Element == 4 ? 2u
                               : Element == 2 ? 1u
                                              : 0u) |
                              (Address << X86RepAddressShift) |
                              (Segment << X86RepSegmentShift) |
                              (unsigned(NotEqual) << X86RepRepeatShift);
  return x86RepControl(Id, A, Descriptor) ? std::optional<uint32_t>(Descriptor)
                                          : std::nullopt;
}

struct X86RepShape {
  Arch TargetArch = Arch::Unknown;
  NdMemoryOrdering MemoryOrdering = NdMemoryOrdering::None;
  NdMemoryAddressSpace MemoryAddressSpace = NdMemoryAddressSpace::Default;
  bool ValidId = false;
  bool WritableOutput = false;
  uint16_t OutputSize = 0;
  unsigned Arguments = 0;
  std::array<uint16_t, 5> InputSizes{};
  std::array<bool, 5> ScalarInputs{};
  std::optional<uint32_t> Descriptor;
};

constexpr bool x86RepShapeIsValid(Intrinsic Id, const X86RepShape &S) {
  const auto C = x86RepContract(Id);
  if (!C || !S.ValidId || !S.WritableOutput || !S.Descriptor ||
      S.Arguments != S.InputSizes.size() ||
      S.MemoryOrdering != NdMemoryOrdering::None ||
      S.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
      (S.OutputSize != 4 && S.OutputSize != 8))
    return false;
  const Arch A = S.TargetArch == Arch::Unknown
                     ? (S.OutputSize == 8 ? Arch::X64 : Arch::X86)
                     : S.TargetArch;
  if ((A == Arch::X64 ? 8 : 4) != S.OutputSize ||
      !x86RepControl(Id, A, *S.Descriptor))
    return false;
  for (unsigned I = 0; I < S.InputSizes.size(); ++I) {
    const unsigned Size = I == 2 && C->Port ? 2
                          : I == 3          ? X86RepStatusBytes
                          : I == 4          ? X86RepDescriptorBytes
                                            : S.OutputSize;
    if (!S.ScalarInputs[I] || S.InputSizes[I] != Size)
      return false;
  }
  return true;
}

} // namespace neverd

#endif // NEVERD_IR_INTRINSICS_X86_X86REPSTATE_H
