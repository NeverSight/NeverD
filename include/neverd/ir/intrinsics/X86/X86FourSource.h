//===- X86FourSource.h - Four-source memory contracts --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Explicit source values for one four-iteration architectural instruction.
//===----------------------------------------------------------------------===//
#ifndef NEVERD_IR_INTRINSICS_X86_X86FOURSOURCE_H
#define NEVERD_IR_INTRINSICS_X86_X86FOURSOURCE_H

#include "neverd/ir/intrinsics/Intrinsics.h"

#include <array>
#include <cstdint>
#include <optional>

namespace neverd {
enum class X86FourSourceKind : uint8_t {
  FMAddPS,
  FNMAddPS,
  FMAddSS,
  FNMAddSS,
  DPWSSD,
  DPWSSDS,
};
inline constexpr unsigned X86FourSourceCount = 4;
inline constexpr unsigned X86FourSourceZero = 0x80;
inline constexpr unsigned X86FourSourceStackReference = 0x20;
inline constexpr unsigned X86FourSourceSegmentShift = 8;
inline constexpr const char *X86FourSourceMalformed =
    "invalid x86 four-source memory contract";

struct X86FourSourceControl {
  X86FourSourceKind Kind;
  bool Zero;
  bool StackReference = false;
  unsigned SegmentPrefix = 0;
  constexpr bool scalar() const {
    return Kind == X86FourSourceKind::FMAddSS ||
           Kind == X86FourSourceKind::FNMAddSS;
  }
  constexpr bool integer() const {
    return Kind == X86FourSourceKind::DPWSSD ||
           Kind == X86FourSourceKind::DPWSSDS;
  }
  constexpr unsigned bytes() const { return scalar() ? 16 : 64; }
  constexpr unsigned sourceBytes() const { return scalar() ? 4 : 64; }
  constexpr unsigned lanes() const { return scalar() ? 1 : 16; }
  constexpr unsigned maskBytes() const { return scalar() ? 1 : 2; }
  constexpr const char *mnemonic() const {
    constexpr const char *Names[] = {"v4fmaddps",  "v4fnmaddps", "v4fmaddss",
                                     "v4fnmaddss", "vp4dpwssd",  "vp4dpwssds"};
    return Names[static_cast<unsigned>(Kind)];
  }
};
constexpr std::optional<X86FourSourceControl> x86FourSourceControl(uint64_t C) {
  const unsigned Segment = C >> X86FourSourceSegmentShift;
  const auto Kind = (C & UINT8_MAX) &
                    ~uint64_t(X86FourSourceZero | X86FourSourceStackReference);
  if (C > UINT16_MAX ||
      (Segment && Segment != 0x26 && Segment != 0x2e && Segment != 0x36) ||
      (Segment && (C & X86FourSourceStackReference)) ||
      Kind > static_cast<unsigned>(X86FourSourceKind::DPWSSDS))
    return {};
  return X86FourSourceControl{static_cast<X86FourSourceKind>(Kind),
                              bool(C & X86FourSourceZero),
                              bool(C & X86FourSourceStackReference), Segment};
}
struct X86FourSourceOperandUse {
  uint64_t LaneMask;
  bool Address;
  bool Sources;
  bool Accumulator;
};
constexpr X86FourSourceOperandUse
x86FourSourceOperandUse(X86FourSourceControl C,
                        std::optional<uint64_t> KnownMask = std::nullopt) {
  const uint64_t All = (UINT64_C(1) << C.lanes()) - 1;
  const bool Active = !KnownMask || (*KnownMask & All) != 0;
  // The accumulator participates in active arithmetic. SS always retains
  // its upper 96 bits, even when the low lane is masked off with zeroing.
  return {All, Active, Active, Active || !C.Zero || C.scalar()};
}
struct X86FourSourceShape {
  Arch TargetArch = Arch::Unknown;
  NdMemoryOrdering MemoryOrdering = NdMemoryOrdering::None;
  NdMemoryAddressSpace MemoryAddressSpace = NdMemoryAddressSpace::Default;
  bool ValidId = false;
  bool WritableOutput = false;
  unsigned OutputSize = 0;
  unsigned Arguments = 0;
  std::array<unsigned, 5> Sizes{};
  std::array<bool, 5> Scalars{};
  std::optional<uint64_t> Control;
};
constexpr bool x86FourSourceShapeIsValid(const X86FourSourceShape &S) {
  if (!S.ValidId || !S.Control || !x86FourSourceControl(*S.Control) ||
      (S.TargetArch != Arch::Unknown && S.TargetArch != Arch::X86 &&
       S.TargetArch != Arch::X64) ||
      S.MemoryOrdering != NdMemoryOrdering::None ||
      !isKnownMemoryAddressSpace(S.MemoryAddressSpace) || S.Arguments != 5 ||
      !S.WritableOutput)
    return false;
  const auto C = *x86FourSourceControl(*S.Control);
  if (C.SegmentPrefix &&
      ((S.TargetArch != Arch::Unknown && S.TargetArch != Arch::X86) ||
       S.MemoryAddressSpace != NdMemoryAddressSpace::Default))
    return false;
  if (C.StackReference &&
      (S.TargetArch == Arch::X86 ||
       S.MemoryAddressSpace != NdMemoryAddressSpace::Default))
    return false;
  if (S.OutputSize != C.bytes() || S.Sizes[0] != 8 || S.Sizes[1] != C.bytes() ||
      S.Sizes[2] != X86FourSourceCount * C.sourceBytes() ||
      S.Sizes[3] != C.maskBytes() || S.Sizes[4] != (C.SegmentPrefix ? 2u : 1u))
    return false;
  for (bool Scalar : S.Scalars)
    if (!Scalar)
      return false;
  return true;
}
} // namespace neverd
#endif // NEVERD_IR_INTRINSICS_X86_X86FOURSOURCE_H
