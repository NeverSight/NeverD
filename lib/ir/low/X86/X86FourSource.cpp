//===- X86FourSource.cpp - Four-source numerical execution -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// One memory transaction followed by four sequential architectural updates.
//===----------------------------------------------------------------------===//
#include "neverd/Limits.h"
#include "neverd/ir/low/NdOpEmulator.h"
#include "neverd/ir/low/X86/X86FourSourceShape.h"
#include "neverd/lift/X86/X86Regs.h"

#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/APInt.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <vector>

namespace neverd {
bool NdOpEmulator::executeX86FourSource(const LowOp &Op) {
  const auto Id = static_cast<Intrinsic>(Op.Inputs[0].Offset);
  const bool Explicit = Id == Intrinsic::X86FourSourceMemory;
  X86FourSourceControl C{X86FourSourceKind::FMAddPS, false};
  unsigned GroupBase = 0;
  if (Explicit) {
    if (!x86FourSourceShapeIsValid(x86FourSourceLowShape(Op, Img.Arch)))
      return false;
    C = *x86FourSourceControl(Op.Inputs[5].Offset);
  } else {
    if ((Id != Intrinsic::X86FourFMA && !isX86VP4DPIntrinsic(Id)) ||
        (Img.Arch != Arch::X86 && Img.Arch != Arch::X64) || Op.NumInputs != 6 ||
        !Op.Inputs[3].isConst() || Op.Inputs[3].Size != 1 ||
        !Op.Inputs[5].isConst() || Op.Inputs[5].Size != 1 ||
        Op.MemoryOrdering != NdMemoryOrdering::None)
      return false;
    const auto Control = Op.Inputs[5].Offset;
    if (Id == Intrinsic::X86FourFMA) {
      if (Control > 7)
        return false;
      C = {static_cast<X86FourSourceKind>(((Control & 1) ? 2 : 0) |
                                          ((Control & 2) ? 1 : 0)),
           bool(Control & 4)};
    } else {
      if (Control > 1)
        return false;
      C = {Id == Intrinsic::X86VP4DPWSSD ? X86FourSourceKind::DPWSSD
                                         : X86FourSourceKind::DPWSSDS,
           bool(Control)};
    }
    if (Op.Output.Size != C.bytes() || Op.Inputs[2].Size != C.bytes() ||
        Op.Inputs[4].Size != C.maskBytes() || !Op.Inputs[1].Size ||
        Op.Inputs[1].Size > 8 || Op.Inputs[3].Offset > 28 ||
        (Op.Inputs[3].Offset & 3) ||
        (Img.Arch == Arch::X86 && Op.Inputs[3].Offset > 4))
      return false;
    GroupBase = Op.Inputs[3].Offset;
  }
  const auto Known = [&](const NdVar &V) {
    return knownOperandBytes(V, 0, V.Size);
  };
  if (!Known(Op.Inputs[4]))
    return false;
  const auto Use = x86FourSourceOperandUse(C, readOperand(Op.Inputs[4]));
  const auto Mask = readOperand(Op.Inputs[4]) & Use.LaneMask;
  // Native execution owns CS/ES/SS descriptor state. A zero mask suppresses
  // that observation; active accesses cannot use the ordinary DS image.
  if (Use.Address && C.SegmentPrefix)
    return false;
  if ((Use.Accumulator && !Known(Op.Inputs[2])) ||
      (Use.Address && (!Known(Op.Inputs[1]) || (!C.integer() && !MXCSRKnown))))
    return false;
  const auto Old = Use.Accumulator ? readOperandBytes(Op.Inputs[2])
                                   : std::vector<uint8_t>(C.bytes(), 0);
  std::array<std::vector<uint8_t>, X86FourSourceCount> Sources;
  if (Use.Address) {
    if (Explicit) {
      if (!Known(Op.Inputs[3]))
        return false;
      const auto Packed = readOperandBytes(Op.Inputs[3]);
      for (unsigned I = 0; I < X86FourSourceCount; ++I)
        Sources[I].assign(Packed.begin() + I * C.sourceBytes(),
                          Packed.begin() + (I + 1) * C.sourceBytes());
    } else {
      for (unsigned I = 0; I < X86FourSourceCount; ++I) {
        const auto R = static_cast<x86_reg>(
            (C.scalar() ? X86_REG_XMM0 : X86_REG_ZMM0) + GroupBase + I);
        const auto V = NdVar::reg(mapCapstoneReg(R).Offset, C.sourceBytes());
        if (!Known(V))
          return false;
        Sources[I] = readOperandBytes(V);
      }
    }
  }
  // Tuple1_4X is all-or-nothing. Any active lane reads the whole m128 before
  // publishing numerical status or a destination result.
  std::array<uint32_t, X86FourSourceCount> MemoryWords{};
  if (Use.Address) {
    const auto Address = resolveMemoryAddress(Op, readOperand(Op.Inputs[1]));
    if (!Address)
      return false;
    const auto Bytes = loadMemoryBytes(*Address, sizeof(MemoryWords));
    if (!Bytes)
      return false;
    for (unsigned I = 0; I < X86FourSourceCount; ++I)
      std::memcpy(&MemoryWords[I], Bytes->data() + I * sizeof(uint32_t),
                  sizeof(uint32_t));
    if (CollectLoads &&
        static_cast<int>(LoadLog.size()) < limits::kMaxLoadRecords)
      LoadLog.push_back({*Address, sizeof(MemoryWords)});
  }
  const bool ZeroMask = C.Zero;
  if (!C.integer()) {
    const bool Scalar = C.scalar();
    const bool Negative = C.Kind == X86FourSourceKind::FNMAddPS ||
                          C.Kind == X86FourSourceKind::FNMAddSS;
    const auto ActiveMask = Mask;
    std::vector<uint8_t> Result = Old;
    const unsigned Lanes = Scalar ? 1 : 16;
    std::vector<uint32_t> Accumulators(Lanes);
    for (unsigned Lane = 0; Lane < Lanes; ++Lane) {
      const unsigned Offset = Lane * 4;
      if (((ActiveMask >> Lane) & 1) == 0) {
        if (ZeroMask)
          std::fill_n(Result.begin() + Offset, 4, 0);
        continue;
      }
      std::memcpy(&Accumulators[Lane], Old.data() + Offset, 4);
    }
    auto Rounding = [&]() {
      switch ((MXCSR >> 13) & 3U) {
      case 1:
        return llvm::APFloat::rmTowardNegative;
      case 2:
        return llvm::APFloat::rmTowardPositive;
      case 3:
        return llvm::APFloat::rmTowardZero;
      default:
        return llvm::APFloat::rmNearestTiesToEven;
      }
    }();
    auto StatusFlags = [](llvm::APFloat::opStatus Status) {
      const unsigned Raw = static_cast<unsigned>(Status);
      uint32_t Flags = 0;
      if (Raw & llvm::APFloat::opInvalidOp)
        Flags |= 1U << 0;
      if (Raw & llvm::APFloat::opDivByZero)
        Flags |= 1U << 2;
      if (Raw & llvm::APFloat::opOverflow)
        Flags |= 1U << 3;
      if (Raw & llvm::APFloat::opUnderflow)
        Flags |= 1U << 4;
      if (Raw & llvm::APFloat::opInexact)
        Flags |= 1U << 5;
      return Flags;
    };
    auto IsNaN = [](uint32_t V) {
      return (V & 0x7f800000U) == 0x7f800000U && (V & 0x007fffffU) != 0;
    };
    auto IsSignalingNaN = [&](uint32_t V) {
      return IsNaN(V) && (V & 0x00400000U) == 0;
    };
    auto IsInf = [](uint32_t V) { return (V & 0x7fffffffU) == 0x7f800000U; };
    auto IsZero = [](uint32_t V) { return (V & 0x7fffffffU) == 0; };
    auto IsDenormal = [](uint32_t V) {
      return (V & 0x7f800000U) == 0 && (V & 0x007fffffU) != 0;
    };
    auto ApplyDaz = [&](uint32_t Bits) {
      return (MXCSR & (1U << 6)) != 0 && IsDenormal(Bits) ? Bits & 0x80000000U
                                                          : Bits;
    };
    for (unsigned I = 0; I < 4; ++I) {
      uint32_t PreRaised = 0;
      std::vector<uint32_t> ArithmeticSources(Lanes);
      std::vector<uint32_t> Multipliers(Lanes);
      std::vector<uint32_t> Addends(Lanes);
      std::vector<uint8_t> NeedsPostComputation(Lanes, 0);
      for (unsigned Lane = 0; Lane < Lanes; ++Lane) {
        if (((ActiveMask >> Lane) & 1) == 0)
          continue;
        const unsigned Offset = Lane * 4;
        uint32_t OriginalSource = 0;
        std::memcpy(&OriginalSource, Sources[I].data() + Offset, 4);
        const uint32_t OriginalMultiplier = MemoryWords[I];
        const uint32_t OriginalAddend = Accumulators[Lane];

        // Intel's pre-computation priority is NaN/other-invalid, then
        // denormal.  Select a source NaN from its original architectural bits:
        // V4FNMADD's arithmetic negation must not change its sign or payload.
        if (IsNaN(OriginalSource) || IsNaN(OriginalMultiplier) ||
            IsNaN(OriginalAddend)) {
          if (IsSignalingNaN(OriginalSource) ||
              IsSignalingNaN(OriginalMultiplier) ||
              IsSignalingNaN(OriginalAddend))
            PreRaised |= 1U << 0;
          const uint32_t Selected = IsNaN(OriginalSource) ? OriginalSource
                                    : IsNaN(OriginalMultiplier)
                                        ? OriginalMultiplier
                                        : OriginalAddend;
          Accumulators[Lane] = Selected | 0x00400000U;
          continue;
        }

        uint32_t SourceBits = ApplyDaz(OriginalSource);
        const uint32_t MultiplierBits = ApplyDaz(OriginalMultiplier);
        const uint32_t AddendBits = ApplyDaz(OriginalAddend);
        if (Negative)
          SourceBits ^= 0x80000000U;
        if ((IsZero(SourceBits) && IsInf(MultiplierBits)) ||
            (IsInf(SourceBits) && IsZero(MultiplierBits))) {
          Accumulators[Lane] = 0xffc00000U;
          PreRaised |= 1U << 0;
          continue;
        }
        const bool InfiniteProduct = IsInf(SourceBits) || IsInf(MultiplierBits);
        const uint32_t ProductInf =
            0x7f800000U | ((SourceBits ^ MultiplierBits) & 0x80000000U);
        if (InfiniteProduct && IsInf(AddendBits) &&
            ((ProductInf ^ AddendBits) & 0x80000000U)) {
          Accumulators[Lane] = 0xffc00000U;
          PreRaised |= 1U << 0;
          continue;
        }

        if ((MXCSR & (1U << 6)) == 0 &&
            (IsDenormal(OriginalSource) || IsDenormal(OriginalMultiplier) ||
             IsDenormal(OriginalAddend)))
          PreRaised |= 1U << 1;
        if (InfiniteProduct) {
          Accumulators[Lane] = ProductInf;
          continue;
        }
        if (IsInf(AddendBits)) {
          Accumulators[Lane] = AddendBits;
          continue;
        }

        ArithmeticSources[Lane] = SourceBits;
        Multipliers[Lane] = MultiplierBits;
        Addends[Lane] = AddendBits;
        NeedsPostComputation[Lane] = 1;
      }

      // Packed lanes report the union of their pre-computation exceptions.
      // An unmasked pre fault suppresses post-computation for every lane in
      // this iteration, rather than allowing a different lane to add O/U/P.
      MXCSR |= PreRaised;
      if ((PreRaised & ~(MXCSR >> 7) & 0x03U) != 0)
        return false;

      uint32_t PostRaised = 0;
      for (unsigned Lane = 0; Lane < Lanes; ++Lane) {
        if (!NeedsPostComputation[Lane])
          continue;
        uint32_t Raised = 0;
        llvm::APFloat Product(llvm::APFloat::IEEEsingle(),
                              llvm::APInt(32, ArithmeticSources[Lane]));
        const llvm::APFloat Multiplier(llvm::APFloat::IEEEsingle(),
                                       llvm::APInt(32, Multipliers[Lane]));
        const llvm::APFloat Addend(llvm::APFloat::IEEEsingle(),
                                   llvm::APInt(32, Addends[Lane]));
        Raised |=
            StatusFlags(Product.fusedMultiplyAdd(Multiplier, Addend, Rounding));
        Accumulators[Lane] =
            static_cast<uint32_t>(Product.bitcastToAPInt().getZExtValue());
        const bool Tiny = IsDenormal(Accumulators[Lane]);
        const bool UnderflowMasked = (MXCSR & (1U << 11)) != 0;
        if (Tiny && !UnderflowMasked) {
          // Exact tiny results still fault when underflow is unmasked.  FTZ is
          // ignored on this path and must not synthesize precision.
          Raised |= 1U << 4;
        } else if (Tiny && (MXCSR & (1U << 15)) != 0) {
          // With underflow masked, FTZ flushes even an exact tiny result and
          // reports both underflow and precision.
          Accumulators[Lane] &= 0x80000000U;
          Raised |= (1U << 4) | (1U << 5);
        }
        PostRaised |= Raised;
      }
      MXCSR |= PostRaised;
      if ((PostRaised & ~(MXCSR >> 7) & 0x3fU) != 0)
        return false;
    }
    for (unsigned Lane = 0; Lane < Lanes; ++Lane) {
      if (((ActiveMask >> Lane) & 1) == 0)
        continue;
      const unsigned Offset = Lane * 4;
      const uint32_t Bits = Accumulators[Lane];
      Result[Offset] = static_cast<uint8_t>(Bits);
      Result[Offset + 1] = static_cast<uint8_t>(Bits >> 8);
      Result[Offset + 2] = static_cast<uint8_t>(Bits >> 16);
      Result[Offset + 3] = static_cast<uint8_t>(Bits >> 24);
    }
    writeOutputBytes(Op.Output, Result);
    return true;
  }
  std::vector<uint8_t> Result = Old;
  auto readDword = [](const std::vector<uint8_t> &Bytes, size_t Offset) {
    uint32_t Value = 0;
    std::memcpy(&Value, Bytes.data() + Offset, sizeof(Value));
    return Value;
  };
  auto writeDword = [](std::vector<uint8_t> &Bytes, size_t Offset,
                       uint32_t Value) {
    std::memcpy(Bytes.data() + Offset, &Value, sizeof(Value));
  };
  auto readWord = [](const std::vector<uint8_t> &Bytes, size_t Offset) {
    uint16_t Value = 0;
    std::memcpy(&Value, Bytes.data() + Offset, sizeof(Value));
    return static_cast<int32_t>(static_cast<int16_t>(Value));
  };
  const bool Saturating = C.Kind == X86FourSourceKind::DPWSSDS;
  for (unsigned Lane = 0; Lane < 16; ++Lane) {
    if (((Mask >> Lane) & 1) == 0) {
      if (ZeroMask)
        writeDword(Result, Lane * 4, 0);
      continue;
    }
    uint32_t Acc = readDword(Old, Lane * 4);
    for (unsigned I = 0; I < 4; ++I) {
      const int64_t Product =
          static_cast<int64_t>(readWord(Sources[I], Lane * 4)) *
              static_cast<int64_t>(
                  static_cast<int16_t>(MemoryWords[I] & UINT32_C(0xffff))) +
          static_cast<int64_t>(readWord(Sources[I], Lane * 4 + 2)) *
              static_cast<int64_t>(static_cast<int16_t>(MemoryWords[I] >> 16));
      const int64_t Sum =
          static_cast<int64_t>(static_cast<int32_t>(Acc)) + Product;
      if (Saturating) {
        const int64_t Clamped = std::max<int64_t>(
            -2147483648LL, std::min<int64_t>(2147483647LL, Sum));
        Acc = static_cast<uint32_t>(static_cast<int32_t>(Clamped));
      } else {
        // VP4DPWSSD wraps after every one of the four sequential updates.
        Acc = static_cast<uint32_t>(Sum);
      }
    }
    writeDword(Result, Lane * 4, Acc);
  }
  writeOutputBytes(Op.Output, Result);
  return true;
}
} // namespace neverd
