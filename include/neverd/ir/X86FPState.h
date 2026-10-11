//===- X86FPState.h - Explicit x86 numerical and FP state results -*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_X86FPSTATE_H
#define NEVERD_IR_X86FPSTATE_H

#include "neverd/ir/intrinsics/Intrinsics.h"

#include <string>

namespace neverd {

constexpr bool isX86FPFmaStateIntrinsic(Intrinsic Id) {
  return Id == Intrinsic::X86FPFmaState || Id == Intrinsic::X86FPFmaMemoryState;
}

constexpr bool isX86FPArithStateIntrinsic(Intrinsic Id) {
  return Id == Intrinsic::X86FPArithState ||
         Id == Intrinsic::X86FPArithMemoryState || isX86FPFmaStateIntrinsic(Id);
}

constexpr bool x86FPArithStateIsScalar(unsigned Control) {
  return (Control & 16) != 0;
}
constexpr unsigned x86FPArithStateElementBytes(unsigned Control) {
  return (Control & 8) ? 8 : 4;
}
constexpr bool x86FPArithStateIsUnary(unsigned Control) {
  return (Control & 7) == unsigned(X86FPArithKind::SquareRoot);
}
constexpr bool x86FPArithStateIsHorizontal(unsigned Control) {
  return (Control & 7) != unsigned(X86FPArithKind::FusedMultiplyAdd) &&
         (Control & 64) != 0;
}
constexpr bool x86FPArithStateIsAlternating(unsigned Control) {
  return (Control & 7) != unsigned(X86FPArithKind::FusedMultiplyAdd) &&
         (Control & 128) != 0;
}

/// FMA3 controls retain order132/213/231 in bits9:8, product/addend signs
/// in bits6/7 and alternating addends in bits10/11. VEX has no SAE/ER.
constexpr unsigned makeX86FPFmaStateControl(unsigned Order, bool Double,
                                            bool Scalar, bool Memory,
                                            bool Negate, bool Subtract,
                                            bool Alternating = false,
                                            bool SubtractEven = false) {
  return unsigned(X86FPArithKind::FusedMultiplyAdd) | (Double ? 8 : 0) |
         (Scalar ? 16 : 0) | (Memory && !Scalar ? 32 : 0) | (Negate ? 64 : 0) |
         (Subtract ? 128 : 0) | (Order << 8) | (Alternating ? 1024 : 0) |
         (SubtractEven ? 2048 : 0);
}
constexpr bool x86FPFmaStateControlIsValid(uint64_t Control, bool Memory) {
  const bool Alternating = (Control & 1024) != 0;
  return (Control & ~UINT64_C(4095)) == 0 &&
         (Control & 7) == unsigned(X86FPArithKind::FusedMultiplyAdd) &&
         ((Control >> 8) & 3) < 3 &&
         ((Control & 32) != 0) == (Memory && !(Control & 16)) &&
         (!(Control & 2048) || Alternating) &&
         (!Alternating || !(Control & (16 | 64 | 128)));
}
constexpr unsigned x86FPArithStateControl(unsigned Layout) {
  return (Layout >> 8) & 0xffff;
}

/// Bits6/7 select horizontal pairs or alternating subtraction/addition.
/// Both complete as one packed instruction, including exception priority.
constexpr bool x86FPArithStateControlIsValid(uint64_t Control, bool Memory) {
  const unsigned Kind = Control & 7;
  const bool Horizontal = x86FPArithStateIsHorizontal(Control);
  const bool Alternating = x86FPArithStateIsAlternating(Control);
  const bool Scalar = x86FPArithStateIsScalar(Control);
  return (Control & ~(Memory ? UINT64_C(255) : UINT64_C(223))) == 0 &&
         Kind != unsigned(X86FPArithKind::FusedMultiplyAdd) &&
         (!Scalar || !(Control & 32)) &&
         (!(Horizontal || Alternating) || !Scalar) &&
         !(Horizontal && Alternating) &&
         (!Horizontal || Kind == unsigned(X86FPArithKind::Add) ||
          Kind == unsigned(X86FPArithKind::Subtract)) &&
         (!Alternating || Kind == unsigned(X86FPArithKind::Add));
}

constexpr bool isX86FPApprox12Intrinsic(Intrinsic Id) {
  return Id == Intrinsic::X86FPApprox12State ||
         Id == Intrinsic::X86FPApprox12MemoryState;
}

constexpr bool isX86FPStateMemoryIntrinsic(Intrinsic Id) {
  return Id == Intrinsic::X86FPRoundMemoryState ||
         Id == Intrinsic::X86FPApprox12MemoryState ||
         Id == Intrinsic::X86FPArithMemoryState ||
         Id == Intrinsic::X86FPFmaMemoryState;
}

/// APPROX12 has no CSR operand: even unknown MXCSR remains unchanged.
/// Bit 0 selects RSQRT, bit 1 scalar, bit 2 VEX packed memory (unaligned).
constexpr bool x86FPApprox12IsScalar(unsigned Control) {
  return (Control & 2) != 0;
}

constexpr const char *x86FPApprox12Mnemonic(unsigned Layout) {
  const unsigned Bytes = Layout & 0xff;
  const unsigned Control = (Layout >> 8) & 0xff;
  return x86FPApprox12IsScalar(Control) ? ((Control & 1) ? "rsqrtss" : "rcpss")
         : Bytes == 32 || (Control & 4)
             ? ((Control & 1) ? "vrsqrtps" : "vrcpps")
             : ((Control & 1) ? "rsqrtps" : "rcpps");
}

/// Scalar SSE operations consume raw operands and an explicit MXCSR value.
/// Their single result packs the numerical bits below the outgoing four-byte
/// MXCSR. SUBBYTES transports both definitions through SSA without implicit
/// auxiliary-output discovery. An unmasked exception prevents completion.
constexpr bool isX86ScalarFPStateIntrinsic(Intrinsic Id) {
  return Id == Intrinsic::X86FPAddState || Id == Intrinsic::X86FPSubState ||
         Id == Intrinsic::X86FPMulState || Id == Intrinsic::X86FPDivState;
}

constexpr bool isX86FPStateIntrinsic(Intrinsic Id) {
  return isX86ScalarFPStateIntrinsic(Id) || isX86FPApprox12Intrinsic(Id) ||
         isX86FPArithStateIntrinsic(Id) || Id == Intrinsic::X86FPRoundState ||
         Id == Intrinsic::X86FPRoundMemoryState ||
         Id == Intrinsic::X86FPCvtToIntState ||
         Id == Intrinsic::X86FPTruncToIntState ||
         Id == Intrinsic::X86ReadMXCSR || Id == Intrinsic::X86WriteMXCSR;
}

constexpr bool isX86FPConversionStateIntrinsic(Intrinsic Id) {
  return Id == Intrinsic::X86FPCvtToIntState ||
         Id == Intrinsic::X86FPTruncToIntState;
}

constexpr bool isX86FPNumericalStateIntrinsic(Intrinsic Id) {
  return isX86FPStateIntrinsic(Id) && Id != Intrinsic::X86ReadMXCSR &&
         Id != Intrinsic::X86WriteMXCSR;
}

constexpr bool isX86FPRoundStateIntrinsic(Intrinsic Id) {
  return Id == Intrinsic::X86FPRoundState ||
         Id == Intrinsic::X86FPRoundMemoryState;
}

/// A state effect does not imply a void result. Reads and completed
/// numerical/state aggregates produce values; only the explicit commit is void.
constexpr bool x86FPStateReturnsValue(Intrinsic Id) {
  return isX86FPStateIntrinsic(Id) && Id != Intrinsic::X86WriteMXCSR;
}

constexpr const char *x86FPStateConversionMnemonic(Intrinsic Id,
                                                   unsigned SourceBytes) {
  if (Id == Intrinsic::X86FPCvtToIntState)
    return SourceBytes == 4 ? "cvtss2si" : "cvtsd2si";
  if (Id == Intrinsic::X86FPTruncToIntState)
    return SourceBytes == 4 ? "cvttss2si" : "cvttsd2si";
  return nullptr;
}

constexpr const char *x86ScalarFPStateMnemonic(Intrinsic Id) {
  switch (Id) {
  case Intrinsic::X86FPAddState:
    return "add";
  case Intrinsic::X86FPSubState:
    return "sub";
  case Intrinsic::X86FPMulState:
    return "mul";
  case Intrinsic::X86FPDivState:
    return "div";
  default:
    return nullptr;
  }
}

constexpr X86FPArithKind x86ScalarFPStateKind(Intrinsic Id) {
  switch (Id) {
  case Intrinsic::X86FPSubState:
    return X86FPArithKind::Subtract;
  case Intrinsic::X86FPMulState:
    return X86FPArithKind::Multiply;
  case Intrinsic::X86FPDivState:
    return X86FPArithKind::Divide;
  default:
    return X86FPArithKind::Add;
  }
}

struct X86FPStateShape {
  Arch TargetArch = Arch::Unknown;
  NdMemoryOrdering MemoryOrdering = NdMemoryOrdering::None;
  NdMemoryAddressSpace MemoryAddressSpace = NdMemoryAddressSpace::Default;
  unsigned NumInputs = 0;
  bool IdIsConst = false;
  unsigned IdSize = 0;
  uint64_t IdValue = 0;
  bool OutputIsWritable = false;
  unsigned OutputSize = 0;
  bool OperandsAreScalar = false;
  unsigned LeftSize = 0;
  unsigned RightSize = 0;
  unsigned ThirdSize = 0;
  unsigned FourthSize = 0;
  bool ArithmeticLeftIsZero = false;
  unsigned StateSize = 0;
  bool HasAuxiliaryOutputs = false;
  bool DestinationIsConst = false;
  unsigned DestinationSelectorSize = 0;
  uint64_t DestinationBytes = 0;
  bool ArchitectureMatchesOperands = true;
  bool ControlIsConst = false;
  unsigned ControlSize = 0;
  uint64_t Control = 0;
  bool ImmediateIsConst = false;
  unsigned ImmediateSize = 0;
  uint64_t Immediate = 0;
  bool AddressIsScalar = false;
};

/// ROUND shares the numerical round-transform controls for element width and
/// scalar selection, but has neither scale/reduce nor SAE. Immediate bit 3
/// suppresses only precision; bits 7:4 are ignored by this instruction family.
constexpr bool x86FPRoundStateControlIsValid(uint64_t Control) {
  return (Control & ~UINT64_C(6)) == 0;
}
constexpr bool x86FPRoundStateIsScalar(unsigned Control) {
  return (Control & 4) != 0;
}
constexpr unsigned x86FPRoundStateElementBytes(unsigned Control) {
  return (Control & 2) ? 8 : 4;
}

constexpr bool x86FPStateShapeIsValid(Intrinsic Id,
                                      const X86FPStateShape &Shape) {
  if (!isX86FPStateIntrinsic(Id) || !Shape.ArchitectureMatchesOperands ||
      (Shape.TargetArch != Arch::Unknown && Shape.TargetArch != Arch::X86 &&
       Shape.TargetArch != Arch::X64) ||
      Shape.MemoryOrdering != NdMemoryOrdering::None ||
      (isX86FPStateMemoryIntrinsic(Id)
           ? !isKnownMemoryAddressSpace(Shape.MemoryAddressSpace)
           : Shape.MemoryAddressSpace != NdMemoryAddressSpace::Default) ||
      !Shape.IdIsConst || Shape.IdSize != 2 || Shape.HasAuxiliaryOutputs)
    return false;
  if (Id == Intrinsic::X86ReadMXCSR)
    return Shape.NumInputs == 1 && Shape.OutputIsWritable &&
           Shape.OutputSize == 4;
  if (Id == Intrinsic::X86WriteMXCSR)
    return Shape.NumInputs == 2 && Shape.OutputSize == 0 &&
           Shape.OperandsAreScalar && Shape.LeftSize == 4;
  if (isX86FPApprox12Intrinsic(Id)) {
    const bool Memory = Id == Intrinsic::X86FPApprox12MemoryState;
    const bool Scalar = x86FPApprox12IsScalar(Shape.Control);
    const bool VexPacked = (Shape.Control & 4) != 0;
    return Shape.IdValue == static_cast<unsigned>(Id) && Shape.NumInputs == 3 &&
           Shape.OutputIsWritable && Shape.OperandsAreScalar &&
           Shape.ControlIsConst && Shape.ControlSize == 1 &&
           (Shape.Control & ~(Memory ? UINT64_C(7) : UINT64_C(3))) == 0 &&
           (!Scalar || !VexPacked) &&
           (Scalar ? Shape.OutputSize == 4
                   : Shape.OutputSize == 16 ||
                         ((!Memory || VexPacked) && Shape.OutputSize == 32)) &&
           (Memory ? Shape.AddressIsScalar && Shape.LeftSize == 8
                   : Shape.RightSize == Shape.OutputSize);
  }
  if (isX86FPArithStateIntrinsic(Id)) {
    const bool Fma = isX86FPFmaStateIntrinsic(Id);
    const bool Memory = isX86FPStateMemoryIntrinsic(Id);
    const bool Scalar = x86FPArithStateIsScalar(Shape.Control);
    const unsigned Bytes = Shape.OutputSize >= 4 ? Shape.OutputSize - 4 : 0;
    const bool VexPackedMemory = (Shape.Control & 32) != 0;
    return Shape.IdValue == unsigned(Id) &&
           Shape.NumInputs == (Fma ? 6U : 5U) && Shape.OutputIsWritable &&
           Shape.OperandsAreScalar && Shape.ControlIsConst &&
           Shape.ControlSize == (Fma ? 2U : 1U) &&
           (Fma ? x86FPFmaStateControlIsValid(Shape.Control, Memory)
                : x86FPArithStateControlIsValid(Shape.Control, Memory)) &&
           (!Fma || Shape.FourthSize == Bytes) &&
           (!Scalar || !VexPackedMemory) && Shape.StateSize == 4 &&
           (Scalar ? Bytes == x86FPArithStateElementBytes(Shape.Control)
                   : Bytes == 16 ||
                         ((!Memory || VexPackedMemory) && Bytes == 32)) &&
           (Memory ? Shape.AddressIsScalar && Shape.LeftSize == 8 &&
                         Shape.ThirdSize == Bytes
                   : Shape.RightSize == Bytes && Shape.ThirdSize == Bytes) &&
           (!x86FPArithStateIsUnary(Shape.Control) ||
            Shape.ArithmeticLeftIsZero);
  }
  if (Id == Intrinsic::X86FPRoundState) {
    const bool Scalar = x86FPRoundStateIsScalar(Shape.Control);
    return Shape.NumInputs == 5 && Shape.OutputIsWritable &&
           Shape.OperandsAreScalar && Shape.ControlIsConst &&
           Shape.ControlSize == 1 &&
           x86FPRoundStateControlIsValid(Shape.Control) &&
           Shape.ImmediateIsConst && Shape.ImmediateSize == 1 &&
           Shape.Immediate <= 0xff && Shape.StateSize == 4 &&
           (Scalar
                ? Shape.RightSize == x86FPRoundStateElementBytes(Shape.Control)
                : Shape.RightSize == 16 || Shape.RightSize == 32) &&
           Shape.OutputSize == Shape.RightSize + 4;
  }
  if (Id == Intrinsic::X86FPRoundMemoryState) {
    const unsigned Bytes = Shape.OutputSize >= 4 ? Shape.OutputSize - 4 : 0;
    const bool Scalar = x86FPRoundStateIsScalar(Shape.Control);
    const bool VexPacked = (Shape.Control & 8) != 0;
    return Shape.NumInputs == 5 && Shape.OutputIsWritable &&
           Shape.OperandsAreScalar && Shape.AddressIsScalar &&
           Shape.LeftSize == 8 && Shape.ControlIsConst &&
           Shape.ControlSize == 1 && (Shape.Control & ~UINT64_C(14)) == 0 &&
           Shape.ImmediateIsConst && Shape.ImmediateSize == 1 &&
           Shape.Immediate <= 0xff && Shape.StateSize == 4 &&
           (Scalar ? !VexPacked &&
                         Bytes == x86FPRoundStateElementBytes(Shape.Control)
                   : Bytes == 16 || (VexPacked && Bytes == 32));
  }
  if (isX86FPConversionStateIntrinsic(Id))
    return Shape.NumInputs == 4 && Shape.OutputIsWritable &&
           Shape.OperandsAreScalar &&
           (Shape.LeftSize == 4 || Shape.LeftSize == 8) &&
           Shape.StateSize == 4 && Shape.DestinationIsConst &&
           Shape.DestinationSelectorSize == 4 &&
           (Shape.DestinationBytes == 4 || Shape.DestinationBytes == 8) &&
           !(Shape.TargetArch == Arch::X86 && Shape.DestinationBytes == 8) &&
           Shape.OutputSize == Shape.DestinationBytes + 4;
  return Shape.NumInputs == 4 && Shape.OutputIsWritable &&
         Shape.OperandsAreScalar &&
         (Shape.LeftSize == 4 || Shape.LeftSize == 8) &&
         Shape.RightSize == Shape.LeftSize && Shape.StateSize == 4 &&
         Shape.OutputSize == Shape.LeftSize + 4;
}

/// Helper/classifier keys preserve independent source and integer widths.
/// Arithmetic retains its existing source-size key. Conversion uses the low
/// byte for the source width and the next byte for the destination width.
constexpr unsigned x86FPConversionLayout(unsigned SourceBytes,
                                         unsigned DestinationBytes) {
  return SourceBytes | (DestinationBytes << 8);
}
constexpr unsigned x86FPStateSourceBytes(unsigned Layout) {
  return Layout & 0xff;
}
constexpr unsigned x86FPRoundStateLayout(
    unsigned Bytes, unsigned Control, unsigned Immediate,
    NdMemoryAddressSpace Space = NdMemoryAddressSpace::Default) {
  return Bytes | (Control << 8) | ((Immediate & 15) << 16) |
         (static_cast<unsigned>(Space) << 24);
}
constexpr unsigned x86FPRoundStateControl(unsigned Layout) {
  return (Layout >> 8) & 0xff;
}
constexpr const char *x86FPArithStateOperation(unsigned Control) {
  if (x86FPArithStateIsHorizontal(Control))
    return (Control & 7) == unsigned(X86FPArithKind::Add) ? "hadd" : "hsub";
  if (x86FPArithStateIsAlternating(Control))
    return "addsub";
  switch (static_cast<X86FPArithKind>(Control & 7)) {
  case X86FPArithKind::Add:
    return "add";
  case X86FPArithKind::Subtract:
    return "sub";
  case X86FPArithKind::Multiply:
    return "mul";
  case X86FPArithKind::Divide:
    return "div";
  case X86FPArithKind::SquareRoot:
    return "sqrt";
  case X86FPArithKind::Minimum:
    return "min";
  case X86FPArithKind::Maximum:
    return "max";
  default:
    return nullptr;
  }
}
inline std::string x86FPArithStateMnemonic(unsigned Layout) {
  const unsigned Control = x86FPArithStateControl(Layout);
  if ((Control & 7) == unsigned(X86FPArithKind::FusedMultiplyAdd)) {
    const char *Operation =
        (Control & 1024) ? ((Control & 2048) ? "fmaddsub" : "fmsubadd")
        : (Control & 64) ? ((Control & 128) ? "fnmsub" : "fnmadd")
                         : ((Control & 128) ? "fmsub" : "fmadd");
    const char *Order = ((Control >> 8) & 3) == 0   ? "132"
                        : ((Control >> 8) & 3) == 1 ? "213"
                                                    : "231";
    return std::string("v") + Operation + Order +
           ((Control & 16) ? ((Control & 8) ? "sd" : "ss")
                           : ((Control & 8) ? "pd" : "ps"));
  }
  const bool Vex = x86FPStateSourceBytes(Layout) == 32 || (Control & 32);
  return std::string(Vex ? "v" : "") + x86FPArithStateOperation(Control) +
         (x86FPArithStateIsScalar(Control) ? ((Control & 8) ? "sd" : "ss")
                                           : ((Control & 8) ? "pd" : "ps"));
}
constexpr unsigned x86FPRoundStateImmediate(unsigned Layout) {
  return (Layout >> 16) & 15;
}
constexpr NdMemoryAddressSpace x86FPRoundStateAddressSpace(unsigned Layout) {
  return static_cast<NdMemoryAddressSpace>(Layout >> 24);
}
constexpr const char *x86FPRoundStateMnemonic(unsigned Layout) {
  const unsigned Control = x86FPRoundStateControl(Layout);
  if (x86FPRoundStateIsScalar(Control))
    return x86FPRoundStateElementBytes(Control) == 8 ? "roundsd" : "roundss";
  if (x86FPStateSourceBytes(Layout) == 32 || (Control & 8))
    return x86FPRoundStateElementBytes(Control) == 8 ? "vroundpd" : "vroundps";
  return x86FPRoundStateElementBytes(Control) == 8 ? "roundpd" : "roundps";
}
constexpr unsigned x86FPStateDestinationBytes(Intrinsic Id, unsigned Layout) {
  return isX86FPConversionStateIntrinsic(Id) ? Layout >> 8
                                             : x86FPStateSourceBytes(Layout);
}
constexpr unsigned x86FPStateHelperLayout(Intrinsic Id,
                                          const X86FPStateShape &Shape) {
  return isX86FPArithStateIntrinsic(Id)
             ? x86FPRoundStateLayout(Shape.OutputSize - 4, Shape.Control, 0,
                                     Shape.MemoryAddressSpace)
         : isX86FPApprox12Intrinsic(Id)
             ? x86FPRoundStateLayout(Shape.OutputSize, Shape.Control, 0,
                                     Shape.MemoryAddressSpace)
         : isX86FPRoundStateIntrinsic(Id)
             ? x86FPRoundStateLayout(
                   Id == Intrinsic::X86FPRoundMemoryState ? Shape.OutputSize - 4
                                                          : Shape.RightSize,
                   Shape.Control, Shape.Immediate, Shape.MemoryAddressSpace)
         : isX86FPConversionStateIntrinsic(Id)
             ? x86FPConversionLayout(
                   Shape.LeftSize,
                   static_cast<unsigned>(Shape.DestinationBytes))
         : isX86ScalarFPStateIntrinsic(Id) ? Shape.LeftSize
                                           : 0;
}

/// Only this exact slice of a completed aggregate denotes a scalar FP value.
/// The whole aggregate and its status slice remain integer bit carriers.
constexpr unsigned x86FPStateNumericalSliceSize(Intrinsic Id,
                                                const X86FPStateShape &Shape,
                                                uint64_t Offset,
                                                unsigned Bytes) {
  const unsigned ScalarBytes =
      isX86FPArithStateIntrinsic(Id) ? Shape.OutputSize - 4
      : isX86FPApprox12Intrinsic(Id) ? Shape.OutputSize
      : Id == Intrinsic::X86FPRoundMemoryState
          ? (Shape.OutputSize >= 4 ? Shape.OutputSize - 4 : 0)
      : Id == Intrinsic::X86FPRoundState ? Shape.RightSize
                                         : Shape.LeftSize;
  const bool Scalar =
      isX86ScalarFPStateIntrinsic(Id) ||
      (isX86FPArithStateIntrinsic(Id) &&
       x86FPArithStateIsScalar(Shape.Control)) ||
      (isX86FPApprox12Intrinsic(Id) && x86FPApprox12IsScalar(Shape.Control)) ||
      (isX86FPRoundStateIntrinsic(Id) &&
       x86FPRoundStateIsScalar(Shape.Control));
  return Scalar && x86FPStateShapeIsValid(Id, Shape) && Offset == 0 &&
                 Bytes == ScalarBytes
             ? Bytes
             : 0;
}

} // namespace neverd

#endif // NEVERD_IR_X86FPSTATE_H
