//===- X86Hle.h - HLE memory and register publication ---------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// The source-step contract adapts an original GPR bank to the native HLE
/// interface. PAUSE ends elision before publishing any state. Entry is outside
/// external HLE/RTM; only nonfaulting normal completion is represented.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_INTRINSICS_X86_X86HLE_H
#define NEVERD_IR_INTRINSICS_X86_X86HLE_H

#include "neverd/ir/intrinsics/Intrinsics.h"

#include <array>
#include <cstdint>
#include <optional>

namespace neverd {

// Values are the llvm.x86.hle.context ABI, also transported by older SDKs.
enum class X86HleOperation : uint8_t {
  Add,
  Adc,
  And,
  Btc,
  Btr,
  Bts,
  Cmpxchg,
  Cmpxchg8b,
  Dec,
  Inc,
  Neg,
  Not,
  Or,
  Sbb,
  Sub,
  Xor,
  Xadd,
  Xchg,
  Mov
};

inline constexpr unsigned X86HleAcquire = 0x100;
inline constexpr unsigned X86HleAbortBeforePublication = 0x200;
inline constexpr unsigned X86HleNativeControlMask = 0x3ff;
inline constexpr unsigned X86HleWidthShift = 10;
inline constexpr unsigned X86HleSourceShift = 12;
inline constexpr unsigned X86HleControlMask = 0x7ffff;
inline constexpr unsigned X86HleNativeGprBytes = 32;

constexpr bool x86HleNativeControlIsValid(uint64_t Control, uint64_t Bytes) {
  using O = X86HleOperation;
  const auto Op = static_cast<O>(Control & 0xff);
  if ((Control & ~uint64_t{X86HleNativeControlMask}) ||
      (Control & 0xff) > unsigned(O::Mov) ||
      (Bytes != 1 && Bytes != 2 && Bytes != 4 && Bytes != 8))
    return false;
  if (Op == O::Mov && (Control & X86HleAcquire))
    return false;
  if (Op == O::Cmpxchg8b)
    return Bytes == 8;
  return Bytes != 1 || (Op != O::Btc && Op != O::Btr && Op != O::Bts);
}

struct X86HleCopy {
  unsigned NativeOffset = 0;
  unsigned OriginalOffset = 0;
  unsigned Bytes = 0;
};

struct X86HlePlan {
  unsigned NativeControl = 0;
  unsigned Bytes = 0;
  bool ExplicitSource = false;
  std::array<X86HleCopy, 2> Outputs{};
};

/// Packed source-step control: native bits [9:0], log2(bytes) [11:10],
/// original XADD/XCHG source byte offset [18:12]. Other ops require offset 0.
constexpr std::optional<X86HlePlan> x86HlePlan(uint64_t Control) {
  using O = X86HleOperation;
  const auto Op = static_cast<O>(Control & 0xff);
  const unsigned Native = Control & X86HleNativeControlMask;
  const unsigned Bytes = 1u << ((Control >> X86HleWidthShift) & 3);
  const unsigned Offset = (Control >> X86HleSourceShift) & 127;
  if ((Control & ~uint64_t{X86HleControlMask}) ||
      !(Control & X86HleAbortBeforePublication) ||
      !x86HleNativeControlIsValid(Native, Bytes))
    return {};
  X86HlePlan P{Native, Bytes, true, {}};
  if (Op == O::Xadd || Op == O::Xchg) {
    if ((Offset % 8) && !(Bytes == 1 && Offset < 32 && Offset % 8 == 1))
      return {};
    P.Outputs[0] = {8, Offset, Bytes == 4 ? 8 : Bytes};
  } else if (Offset) {
    return {};
  }
  if (Op == O::Cmpxchg)
    P.Outputs[0] = {0, 0, 8};
  if (Op == O::Cmpxchg8b) {
    P.ExplicitSource = false;
    P.Outputs = {{{0, 0, 8}, {16, 16, 8}}};
  }
  if (Op == O::Dec || Op == O::Inc || Op == O::Neg || Op == O::Not)
    P.ExplicitSource = false;
  return P;
}

struct X86HleShape {
  Arch TargetArch = Arch::Unknown;
  bool ValidId = false;
  uint16_t OutputSize = 0;
  unsigned Arguments = 0;
  std::array<uint16_t, 5> InputSizes{};
  std::array<bool, 5> ScalarInputs{};
  NdMemoryOrdering MemoryOrdering = NdMemoryOrdering::None;
  NdMemoryAddressSpace MemoryAddressSpace = NdMemoryAddressSpace::Default;
  std::optional<uint64_t> Control;
};

constexpr bool x86HleShapeIsValid(const X86HleShape &S) {
  if (!S.ValidId ||
      (S.TargetArch != Arch::Unknown && S.TargetArch != Arch::X64) ||
      S.OutputSize || S.Arguments != 5 ||
      S.MemoryOrdering != NdMemoryOrdering::None ||
      S.MemoryAddressSpace != NdMemoryAddressSpace::Default || !S.Control ||
      !x86HlePlan(*S.Control))
    return false;
  for (unsigned I = 0; I != 5; ++I)
    if (!S.ScalarInputs[I] || S.InputSizes[I] != (I == 4 ? 4 : 8))
      return false;
  return true;
}

} // namespace neverd

#endif // NEVERD_IR_INTRINSICS_X86_X86HLE_H
