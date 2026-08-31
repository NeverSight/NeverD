//===- Intrinsics.h - Intrinsic function definitions --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Defines the Intrinsic enumeration for INTRINSIC operations and
/// provides name/metadata query functions for each intrinsic.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_INTRINSICS_INTRINSICS_H
#define NEVERD_IR_INTRINSICS_INTRINSICS_H

#include "neverd/Common.h"
#include "neverd/ir/NdOps.h"

namespace neverd {

enum class AMXTileComputeKind : uint8_t {
  Int8SignedSigned,
  Int8SignedUnsigned,
  Int8UnsignedSigned,
  Int8UnsignedUnsigned,
  BF16,
  FP16,
  ComplexFP16Imaginary,
  ComplexFP16Real,
  BF8BF8,
  BF8HF8,
  HF8BF8,
  HF8HF8,
  TF32,
};

enum class AMXTileRowKind : uint8_t {
  Move,
  Int32ToFP32,
  FP32ToBF16High,
  FP32ToBF16Low,
  FP32ToFP16High,
  FP32ToFP16Low,
};

/// Architectural address-translation invalidation effects represented by the
/// opaque x86 invalidation intrinsic.  The concrete execution environment
/// owns privilege, feature, descriptor validation, fault ordering, and the
/// actual translation-cache effect.
enum class X86InvalidateKind : uint8_t {
  Invpcid = 0,
};

/// Exact encoding/feature topology for explicit-operand x86 MSR access.  The
/// distinction is architectural: legacy/VEX USER_MSR does not require APX_F,
/// while each EVEX form does, and MSR_IMM additionally requires CPL0.
enum class X86MsrAccessKind : uint8_t {
  RdmsrImmediate = 0,
  WrmsrnsImmediate,
  UrdmsrLegacyRegister,
  UwrmsrLegacyRegister,
  UrdmsrVexImmediate,
  UwrmsrVexImmediate,
  UrdmsrEvexRegister,
  UwrmsrEvexRegister,
  UrdmsrEvexImmediate,
  UwrmsrEvexImmediate,
  Count,
};

constexpr bool x86MsrAccessIsWrite(X86MsrAccessKind Kind) {
  switch (Kind) {
  case X86MsrAccessKind::WrmsrnsImmediate:
  case X86MsrAccessKind::UwrmsrLegacyRegister:
  case X86MsrAccessKind::UwrmsrVexImmediate:
  case X86MsrAccessKind::UwrmsrEvexRegister:
  case X86MsrAccessKind::UwrmsrEvexImmediate:
    return true;
  case X86MsrAccessKind::RdmsrImmediate:
  case X86MsrAccessKind::UrdmsrLegacyRegister:
  case X86MsrAccessKind::UrdmsrVexImmediate:
  case X86MsrAccessKind::UrdmsrEvexRegister:
  case X86MsrAccessKind::UrdmsrEvexImmediate:
  case X86MsrAccessKind::Count:
    return false;
  }
  return false;
}

constexpr bool x86MsrAccessHasImmediateSelector(X86MsrAccessKind Kind) {
  switch (Kind) {
  case X86MsrAccessKind::RdmsrImmediate:
  case X86MsrAccessKind::WrmsrnsImmediate:
  case X86MsrAccessKind::UrdmsrVexImmediate:
  case X86MsrAccessKind::UwrmsrVexImmediate:
  case X86MsrAccessKind::UrdmsrEvexImmediate:
  case X86MsrAccessKind::UwrmsrEvexImmediate:
    return true;
  case X86MsrAccessKind::UrdmsrLegacyRegister:
  case X86MsrAccessKind::UwrmsrLegacyRegister:
  case X86MsrAccessKind::UrdmsrEvexRegister:
  case X86MsrAccessKind::UwrmsrEvexRegister:
  case X86MsrAccessKind::Count:
    return false;
  }
  return false;
}

/// Signedness carried by the x86 DIV/IDIV architectural precondition.
enum class X86DivKind : uint8_t {
  Unsigned = 0,
  Signed = 1,
};

enum class F16CConvertKind : uint8_t {
  HalfToSingle,
  SingleToHalf,
  HalfToSingleSuppressExceptions,
  SingleToHalfSuppressExceptions,
};

enum class X86ApproxFloatKind : uint8_t {
  Rcp14F32,
  Rcp14F64,
  Rsqrt14F32,
  Rsqrt14F64,
  Rcp28F32,
  Rcp28F64,
  Rsqrt28F32,
  Rsqrt28F64,
  Exp2F32,
  Exp2F64,
};

/// Arithmetic operation carried by the exact x86 SIMD floating-point
/// intrinsic.  The intrinsic owns MXCSR rounding, exception, DAZ, FTZ, and
/// inactive-lane behavior that target-independent FLOAT_* nodes cannot
/// represent.
enum class X86FPArithKind : uint8_t {
  Add,
  Subtract,
  Multiply,
  Divide,
  FusedMultiplyAdd,
  SquareRoot,
  Minimum,
  Maximum,
};

enum class X86FPRounding : uint8_t {
  NearestTiesToEven,
  TowardNegative,
  TowardPositive,
  TowardZero,
  MXCSR,
};

enum class X86FPConvertKind : uint8_t {
  SignedIntegerToFloat,
  UnsignedIntegerToFloat,
  FloatToSignedInteger,
  FloatToUnsignedInteger,
  FloatToFloat,
};

/// Immediate-controlled transformations used by VRNDSCALE* and VREDUCE*.
/// These remain target-specific because their rounding source, precision
/// suppression, SAE, DAZ, and active-lane exception behavior are all x86
/// architectural state rather than ordinary target-independent arithmetic.
enum class X86FPRoundTransformKind : uint8_t {
  RoundScale,
  Reduce,
};

enum class X86FPExtractKind : uint8_t {
  Exponent,
  Mantissa,
};

constexpr uint8_t makeX86FPExtractControl(X86FPExtractKind Kind, bool IsF64,
                                          bool Scalar,
                                          bool SuppressExceptions) {
  return static_cast<uint8_t>(Kind) | (IsF64 ? UINT8_C(1) << 1 : 0) |
         (Scalar ? UINT8_C(1) << 2 : 0) |
         (SuppressExceptions ? UINT8_C(1) << 3 : 0);
}

constexpr bool isValidX86FPExtractControl(uint8_t Control) {
  constexpr uint8_t KnownBits = UINT8_C(0x0f);
  const auto Kind = static_cast<X86FPExtractKind>(Control & 1U);
  return (Control & ~KnownBits) == 0 && Kind <= X86FPExtractKind::Mantissa;
}

constexpr uint8_t makeX86FPRangeControl(bool IsF64, bool Scalar,
                                        bool SuppressExceptions) {
  return (IsF64 ? UINT8_C(1) : 0) | (Scalar ? UINT8_C(1) << 1 : 0) |
         (SuppressExceptions ? UINT8_C(1) << 2 : 0);
}

constexpr bool isValidX86FPRangeControl(uint8_t Control) {
  return (Control & ~UINT8_C(0x07)) == 0;
}

/// Control for the exact EVEX floating-point comparison intrinsic.  Bit zero
/// selects F64 lanes, bit one selects scalar topology, and bit two carries SAE.
constexpr uint8_t makeX86FPCompareControl(bool IsF64, bool Scalar,
                                          bool SuppressExceptions) {
  return (IsF64 ? UINT8_C(1) : 0) | (Scalar ? UINT8_C(1) << 1 : 0) |
         (SuppressExceptions ? UINT8_C(1) << 2 : 0);
}

constexpr bool isValidX86FPCompareControl(uint8_t Control) {
  return (Control & ~UINT8_C(0x07)) == 0;
}

constexpr uint8_t makeX86FPFixupControl(bool IsF64, bool Scalar,
                                        bool SuppressExceptions) {
  return (IsF64 ? UINT8_C(1) : 0) | (Scalar ? UINT8_C(1) << 1 : 0) |
         (SuppressExceptions ? UINT8_C(1) << 2 : 0);
}

constexpr bool isValidX86FPFixupControl(uint8_t Control) {
  return (Control & ~UINT8_C(0x07)) == 0;
}

constexpr uint8_t makeX86FPScaleControl(bool IsF64, bool Scalar,
                                        bool SuppressExceptions,
                                        X86FPRounding Rounding) {
  return (IsF64 ? UINT8_C(1) : 0) | (Scalar ? UINT8_C(1) << 1 : 0) |
         (SuppressExceptions ? UINT8_C(1) << 2 : 0) |
         (static_cast<uint8_t>(Rounding) << 3);
}

constexpr bool isValidX86FPScaleControl(uint8_t Control) {
  constexpr uint8_t KnownBits = UINT8_C(0x3f);
  const auto Rounding = static_cast<X86FPRounding>((Control >> 3) & 7U);
  const bool SuppressExceptions = (Control & (UINT8_C(1) << 2)) != 0;
  return (Control & ~KnownBits) == 0 && Rounding <= X86FPRounding::MXCSR &&
         (Rounding == X86FPRounding::MXCSR || SuppressExceptions);
}

constexpr uint8_t makeX86FPRoundTransformControl(
    X86FPRoundTransformKind Kind, bool IsF64, bool Scalar,
    bool SuppressExceptions) {
  return static_cast<uint8_t>(Kind) | (IsF64 ? UINT8_C(1) << 1 : 0) |
         (Scalar ? UINT8_C(1) << 2 : 0) |
         (SuppressExceptions ? UINT8_C(1) << 3 : 0);
}

constexpr bool isValidX86FPRoundTransformControl(uint8_t Control) {
  constexpr uint8_t KnownBits = UINT8_C(0x0f);
  const auto Kind = static_cast<X86FPRoundTransformKind>(Control & 1U);
  return (Control & ~KnownBits) == 0 &&
         Kind <= X86FPRoundTransformKind::Reduce;
}

/// Pack the complete per-instruction conversion contract into a stable LowIR
/// control word.  Source64 and Destination64 select 64-bit lanes; LaneCount
/// names only architecturally meaningful lanes when the destination register
/// contains zero padding.
constexpr uint16_t makeX86FPConvertControl(X86FPConvertKind Kind, bool Source64,
                                           bool Destination64, bool Truncate,
                                           bool SuppressExceptions,
                                           X86FPRounding Rounding,
                                           unsigned LaneCount) {
  return static_cast<uint16_t>(Kind) | (Source64 ? UINT16_C(1) << 3 : 0) |
         (Destination64 ? UINT16_C(1) << 4 : 0) |
         (Truncate ? UINT16_C(1) << 5 : 0) |
         (SuppressExceptions ? UINT16_C(1) << 6 : 0) |
         (static_cast<uint16_t>(Rounding) << 7) |
         (static_cast<uint16_t>(LaneCount - 1) << 10);
}

constexpr bool isValidX86FPConvertControl(uint16_t Control) {
  constexpr uint16_t KnownBits = UINT16_C(0x3fff);
  const auto Kind = static_cast<X86FPConvertKind>(Control & 7U);
  const bool Truncate = (Control & (UINT16_C(1) << 5)) != 0;
  const bool SuppressExceptions = (Control & (UINT16_C(1) << 6)) != 0;
  const auto Rounding = static_cast<X86FPRounding>((Control >> 7) & 7U);
  const unsigned LaneCount = ((Control >> 10) & 15U) + 1;
  const bool FloatToInteger = Kind == X86FPConvertKind::FloatToSignedInteger ||
                              Kind == X86FPConvertKind::FloatToUnsignedInteger;
  return (Control & ~KnownBits) == 0 &&
         Kind <= X86FPConvertKind::FloatToFloat &&
         Rounding <= X86FPRounding::MXCSR && LaneCount <= 16 &&
         (!Truncate ||
          (FloatToInteger && Rounding == X86FPRounding::TowardZero)) &&
         (Rounding == X86FPRounding::MXCSR || SuppressExceptions || Truncate);
}

constexpr uint16_t makeX86FPArithControl(X86FPArithKind Kind, bool IsF64,
                                         bool Scalar, bool SuppressExceptions,
                                         X86FPRounding Rounding,
                                         bool NegateProduct = false,
                                         bool SubtractAddend = false,
                                         bool AlternatingAddend = false,
                                         bool SubtractEven = false) {
  return static_cast<uint16_t>(Kind) | (IsF64 ? UINT16_C(1) << 3 : 0) |
         (Scalar ? UINT16_C(1) << 4 : 0) |
         (SuppressExceptions ? UINT16_C(1) << 5 : 0) |
         (NegateProduct ? UINT16_C(1) << 6 : 0) |
         (SubtractAddend ? UINT16_C(1) << 7 : 0) |
         (static_cast<uint16_t>(Rounding) << 8) |
         (AlternatingAddend ? UINT16_C(1) << 11 : 0) |
         (SubtractEven ? UINT16_C(1) << 12 : 0);
}

constexpr bool isValidX86FPArithControl(uint16_t Control) {
  constexpr uint16_t KnownBits = UINT16_C(0x1fff);
  const auto Kind = static_cast<X86FPArithKind>(Control & 7U);
  const auto Rounding = static_cast<X86FPRounding>((Control >> 8) & 7U);
  const bool SuppressExceptions = (Control & (UINT16_C(1) << 5)) != 0;
  const bool FmaOnlyFlags =
      (Control & ((UINT16_C(1) << 6) | (UINT16_C(1) << 7) |
                  (UINT16_C(1) << 11) | (UINT16_C(1) << 12))) != 0;
  const bool Alternating = (Control & (UINT16_C(1) << 11)) != 0;
  const bool SubtractEven = (Control & (UINT16_C(1) << 12)) != 0;
  return (Control & ~KnownBits) == 0 && Kind <= X86FPArithKind::Maximum &&
         Rounding <= X86FPRounding::MXCSR &&
         (Rounding == X86FPRounding::MXCSR || SuppressExceptions) &&
         (!FmaOnlyFlags || Kind == X86FPArithKind::FusedMultiplyAdd) &&
         (!SubtractEven || Alternating) &&
         (!Alternating || (Control & (UINT16_C(1) << 7)) == 0);
}

/// Intrinsic IDs for INTRINSIC operations.
///
/// Each architecture gets a 5000-slot range so entries never collide:
///   Common   :     0 -   999
///   x86/x64  :  1000 -  5999
///   AArch64  :  6000 - 10999
///   ARM32    : 11000 - 15999
enum class Intrinsic : uint16_t {
  None = 0,

  Syscall,

#include "neverd/ir/intrinsics/intrinsics_aarch64.inc"
#include "neverd/ir/intrinsics/intrinsics_arm.inc"
#include "neverd/ir/intrinsics/intrinsics_x86.inc"

  _Count = 16000
};

/// True for the complete architectural AMX state surface represented in
/// LowIR.  Keep this classification centralized: AMX operations mix ordinary
/// value results with restartable memory effects, so treating only the four
/// memory forms as a family is not sufficient at backend boundaries.
constexpr bool isAMXIntrinsic(Intrinsic Id) {
  switch (Id) {
  case Intrinsic::AMXLoadConfig:
  case Intrinsic::AMXStoreConfig:
  case Intrinsic::AMXTileLoad:
  case Intrinsic::AMXTileStore:
  case Intrinsic::AMXTileZero:
  case Intrinsic::AMXClearStartRow:
  case Intrinsic::AMXTileCompute:
  case Intrinsic::AMXTileRow:
    return true;
  default:
    return false;
  }
}

constexpr bool isApxAtomicIntrinsic(Intrinsic Id) {
  switch (Id) {
  case Intrinsic::ApxRaoAdd:
  case Intrinsic::ApxRaoAnd:
  case Intrinsic::ApxRaoOr:
  case Intrinsic::ApxRaoXor:
  case Intrinsic::ApxCmpccXadd:
    return true;
  default:
    return false;
  }
}

/// Representation-neutral APX atomic operand facts.  LowIR and MedIR use
/// different value kinds, so each caller classifies its own variables and
/// passes only the semantic facts which the shared contract owns.  Unknown is
/// accepted only where the containing IR has no target field; every known
/// target must be x86-64.
struct ApxAtomicIntrinsicShape {
  Arch TargetArch = Arch::Unknown;
  NdMemoryOrdering MemoryOrdering = NdMemoryOrdering::None;
  uint8_t NumInputs = 0;
  bool IntrinsicIdIsConst = false;
  uint16_t IntrinsicIdSize = 0;
  bool OutputIsWritableScalar = false;
  uint16_t OutputSize = 0;
  bool AddressIsScalar = false;
  uint16_t AddressSize = 0;
  bool SourceIsScalar = false;
  uint16_t SourceSize = 0;
  bool CompareIsScalar = false;
  uint16_t CompareSize = 0;
  bool ConditionIsConst = false;
  uint64_t Condition = 0;
  uint16_t ConditionSize = 0;
};

constexpr bool
intrinsicApxAtomicShapeIsValid(Intrinsic Id,
                               const ApxAtomicIntrinsicShape &Shape) {
  if (!isApxAtomicIntrinsic(Id) ||
      (Shape.TargetArch != Arch::Unknown && Shape.TargetArch != Arch::X64) ||
      !Shape.IntrinsicIdIsConst || Shape.IntrinsicIdSize != 2 ||
      !Shape.AddressIsScalar || Shape.AddressSize != 8)
    return false;

  if (Id != Intrinsic::ApxCmpccXadd)
    return Shape.NumInputs == 3 && Shape.OutputSize == 0 &&
           Shape.SourceIsScalar &&
           (Shape.SourceSize == 4 || Shape.SourceSize == 8) &&
           Shape.MemoryOrdering == NdMemoryOrdering::Relaxed;

  return Shape.NumInputs == 5 && Shape.OutputIsWritableScalar &&
         (Shape.OutputSize == 4 || Shape.OutputSize == 8) &&
         Shape.SourceIsScalar && Shape.SourceSize == Shape.OutputSize &&
         Shape.CompareIsScalar && Shape.CompareSize == Shape.OutputSize &&
         Shape.ConditionIsConst && Shape.ConditionSize == 1 &&
         Shape.Condition < 16 &&
         Shape.MemoryOrdering == NdMemoryOrdering::SequentiallyConsistent;
}

/// Representation-neutral contract for architectural x86 translation-cache
/// invalidation.  The descriptor remains an effective address: materializing
/// an ordinary LowIR LOAD here would move its page fault ahead of the
/// instruction's privilege, feature, type, and descriptor checks.
struct X86InvalidateIntrinsicShape {
  Arch TargetArch = Arch::Unknown;
  NdMemoryOrdering MemoryOrdering = NdMemoryOrdering::None;
  NdMemoryAddressSpace MemoryAddressSpace = NdMemoryAddressSpace::Default;
  uint8_t NumInputs = 0;
  bool IntrinsicIdIsConst = false;
  uint16_t IntrinsicIdSize = 0;
  uint16_t OutputSize = 0;
  bool DescriptorAddressIsScalar = false;
  uint16_t DescriptorAddressSize = 0;
  bool KindIsConst = false;
  uint64_t Kind = 0;
  uint16_t KindSize = 0;
  bool TypeIsScalar = false;
  uint16_t TypeSize = 0;
};

constexpr bool
intrinsicX86InvalidateShapeIsValid(Intrinsic Id,
                                   const X86InvalidateIntrinsicShape &Shape) {
  if (Id != Intrinsic::X86Invalidate ||
      (Shape.TargetArch != Arch::Unknown && Shape.TargetArch != Arch::X86 &&
       Shape.TargetArch != Arch::X64) ||
      !isKnownMemoryAddressSpace(Shape.MemoryAddressSpace) ||
      Shape.MemoryOrdering != NdMemoryOrdering::None || Shape.NumInputs != 4 ||
      !Shape.IntrinsicIdIsConst || Shape.IntrinsicIdSize != 2 ||
      Shape.OutputSize != 0 || !Shape.DescriptorAddressIsScalar ||
      Shape.DescriptorAddressSize != 8 || !Shape.KindIsConst ||
      Shape.KindSize != 1 ||
      Shape.Kind != static_cast<uint64_t>(X86InvalidateKind::Invpcid) ||
      !Shape.TypeIsScalar)
    return false;

  if (Shape.TargetArch == Arch::X86)
    return Shape.TypeSize == 4;
  if (Shape.TargetArch == Arch::X64)
    return Shape.TypeSize == 8;
  return Shape.TypeSize == 4 || Shape.TypeSize == 8;
}

/// Representation-neutral contract for explicit-operand MSR instructions.
/// No ordinary host operation may implement this intrinsic: an authenticated
/// architectural environment owns CPUID/XCR0/CPL, USER_MSR_CTL bitmap,
/// virtualization interception, MSR-specific validation, and exception order.
struct X86MsrAccessIntrinsicShape {
  Arch TargetArch = Arch::Unknown;
  NdMemoryOrdering MemoryOrdering = NdMemoryOrdering::None;
  NdMemoryAddressSpace MemoryAddressSpace = NdMemoryAddressSpace::Default;
  uint8_t NumInputs = 0;
  bool IntrinsicIdIsConst = false;
  uint16_t IntrinsicIdSize = 0;
  bool OutputIsWritableScalar = false;
  uint16_t OutputSize = 0;
  bool KindIsConst = false;
  uint64_t Kind = 0;
  uint16_t KindSize = 0;
  bool SelectorIsScalar = false;
  uint16_t SelectorSize = 0;
  bool ValueIsScalar = false;
  uint16_t ValueSize = 0;
};

constexpr bool intrinsicX86MsrAccessShapeIsValid(
    Intrinsic Id, const X86MsrAccessIntrinsicShape &Shape) {
  if (Id != Intrinsic::X86MsrAccess ||
      (Shape.TargetArch != Arch::Unknown && Shape.TargetArch != Arch::X64) ||
      Shape.MemoryOrdering != NdMemoryOrdering::None ||
      Shape.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
      !Shape.IntrinsicIdIsConst || Shape.IntrinsicIdSize != 2 ||
      !Shape.KindIsConst || Shape.KindSize != 1 ||
      Shape.Kind >= static_cast<uint64_t>(X86MsrAccessKind::Count) ||
      !Shape.SelectorIsScalar)
    return false;

  const auto Kind = static_cast<X86MsrAccessKind>(Shape.Kind);
  if (Shape.SelectorSize !=
      (x86MsrAccessHasImmediateSelector(Kind) ? uint16_t{4} : uint16_t{8}))
    return false;
  if (x86MsrAccessIsWrite(Kind))
    return Shape.NumInputs == 4 && Shape.OutputSize == 0 &&
           Shape.ValueIsScalar && Shape.ValueSize == 8;
  return Shape.NumInputs == 3 && Shape.OutputIsWritableScalar &&
         Shape.OutputSize == 8 && !Shape.ValueIsScalar &&
         Shape.ValueSize == 0;
}

/// Representation-neutral contract for the x86 DIV/IDIV architectural
/// precondition.  Input 1 is the double-width dividend, input 2 is the
/// half-width divisor, and input 3 is a constant X86DivKind.  The check runs
/// before target-independent integer division so divide-by-zero and a
/// non-representable half-width quotient cannot become LLVM poison or a
/// silently truncated architectural result.
struct X86DivPreconditionIntrinsicShape {
  Arch TargetArch = Arch::Unknown;
  NdMemoryOrdering MemoryOrdering = NdMemoryOrdering::None;
  NdMemoryAddressSpace MemoryAddressSpace = NdMemoryAddressSpace::Default;
  uint8_t NumInputs = 0;
  bool IntrinsicIdIsConst = false;
  uint16_t IntrinsicIdSize = 0;
  uint16_t OutputSize = 0;
  bool DividendIsScalar = false;
  uint16_t DividendSize = 0;
  bool DivisorIsScalar = false;
  uint16_t DivisorSize = 0;
  bool KindIsConst = false;
  uint64_t Kind = 0;
  uint16_t KindSize = 0;
};

constexpr bool intrinsicX86DivPreconditionShapeIsValid(
    Intrinsic Id, const X86DivPreconditionIntrinsicShape &Shape) {
  if (Id != Intrinsic::X86RequireDivPrecondition ||
      (Shape.TargetArch != Arch::Unknown && Shape.TargetArch != Arch::X86 &&
       Shape.TargetArch != Arch::X64) ||
      Shape.MemoryOrdering != NdMemoryOrdering::None ||
      Shape.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
      Shape.NumInputs != 4 || !Shape.IntrinsicIdIsConst ||
      Shape.IntrinsicIdSize != 2 || Shape.OutputSize != 0 ||
      !Shape.DividendIsScalar || !Shape.DivisorIsScalar ||
      (Shape.DividendSize != 2 && Shape.DividendSize != 4 &&
       Shape.DividendSize != 8 && Shape.DividendSize != 16) ||
      Shape.DivisorSize * 2 != Shape.DividendSize || !Shape.KindIsConst ||
      Shape.KindSize != 1 ||
      Shape.Kind > static_cast<uint64_t>(X86DivKind::Signed))
    return false;
  if (Shape.TargetArch == Arch::X86)
    return Shape.DividendSize <= 8;
  return true;
}

constexpr bool isPdepPextIntrinsic(Intrinsic Id) {
  return Id == Intrinsic::Pdep || Id == Intrinsic::Pext;
}

constexpr bool isX86VP4DPIntrinsic(Intrinsic Id) {
  return Id == Intrinsic::X86VP4DPWSSD || Id == Intrinsic::X86VP4DPWSSDS;
}

/// The exact VP4DP LowIR contract is [id, effective address, old ZMM
/// destination, source-block base, compact k mask, zeroing bit].  Source-block
/// base and zeroing bit are constants because both are decoded from EVEX; the
/// mask remains a 16-bit K-register view.  The memory address space is carried
/// separately on the operation so FS/GS remains architectural.
constexpr bool intrinsicX86VP4DPShapeIsValid(
    Intrinsic Id, uint8_t NumInputs, uint16_t OutputSize,
    uint16_t AddressSize, uint16_t DestinationSize, uint16_t GroupSize,
    uint16_t MaskSize, uint16_t ControlSize) {
  return isX86VP4DPIntrinsic(Id) && NumInputs == 6 && OutputSize == 64 &&
         AddressSize == 8 && DestinationSize == 64 && GroupSize == 1 &&
         MaskSize == 2 && ControlSize == 1;
}

/// PDEP/PEXT are scalar BMI2 operations even though their backend lowering
/// lives beside SIMD intrinsics.  Keep their exact width and side-effect-free
/// contract shared so a malformed width cannot be coerced to i32/i64 by one
/// consumer while another rejects it.
struct PdepPextIntrinsicShape {
  NdMemoryOrdering MemoryOrdering = NdMemoryOrdering::None;
  NdMemoryAddressSpace MemoryAddressSpace = NdMemoryAddressSpace::Default;
  uint8_t NumInputs = 0;
  bool IntrinsicIdIsConst = false;
  uint16_t IntrinsicIdSize = 0;
  bool OutputIsWritableScalar = false;
  uint16_t OutputSize = 0;
  bool SourceIsScalar = false;
  uint16_t SourceSize = 0;
  bool MaskIsScalar = false;
  uint16_t MaskSize = 0;
};

constexpr bool
intrinsicPdepPextShapeIsValid(Intrinsic Id,
                              const PdepPextIntrinsicShape &Shape) {
  return isPdepPextIntrinsic(Id) && Shape.NumInputs == 3 &&
         Shape.IntrinsicIdIsConst && Shape.IntrinsicIdSize == 2 &&
         Shape.OutputIsWritableScalar &&
         (Shape.OutputSize == 4 || Shape.OutputSize == 8) &&
         Shape.SourceIsScalar && Shape.SourceSize == Shape.OutputSize &&
         Shape.MaskIsScalar && Shape.MaskSize == Shape.OutputSize &&
         Shape.MemoryOrdering == NdMemoryOrdering::None &&
         Shape.MemoryAddressSpace == NdMemoryAddressSpace::Default;
}

/// Intrinsics in this list own a memory operand whose address space is carried
/// by MedOp::MemoryAddressSpace.  All other intrinsics must reject a nondefault
/// value so a new or unsupported opcode cannot accidentally drop FS/GS
/// semantics in a backend fallback.
constexpr bool intrinsicSupportsMemoryAddressSpace(Intrinsic Id) {
  switch (Id) {
  case Intrinsic::Movsb:
  case Intrinsic::Movsw:
  case Intrinsic::Movsd:
  case Intrinsic::Movsq:
  case Intrinsic::Lodsb:
  case Intrinsic::Lodsw:
  case Intrinsic::Lodsd:
  case Intrinsic::Lodsq:
  case Intrinsic::Cmpsb:
  case Intrinsic::Cmpsw:
  case Intrinsic::Cmpsd_str:
  case Intrinsic::Cmpsq:
  case Intrinsic::Outsb:
  case Intrinsic::Outsw:
  case Intrinsic::Outsd:
  case Intrinsic::MaskedLoadB:
  case Intrinsic::MaskedLoadW:
  case Intrinsic::MaskedLoadD:
  case Intrinsic::MaskedLoadQ:
  case Intrinsic::MaskedStoreW:
  case Intrinsic::MaskedStoreD:
  case Intrinsic::MaskedStoreQ:
  case Intrinsic::MaskedStoreB:
  case Intrinsic::Clflush:
  case Intrinsic::Clflushopt:
  case Intrinsic::Clwb:
  case Intrinsic::Prefetch:
  case Intrinsic::PrefetchT0:
  case Intrinsic::PrefetchT1:
  case Intrinsic::PrefetchT2:
  case Intrinsic::PrefetchNta:
  case Intrinsic::PrefetchW:
  case Intrinsic::PrefetchWT1:
  case Intrinsic::Ldmxcsr:
  case Intrinsic::Stmxcsr:
  case Intrinsic::Fxsave:
  case Intrinsic::Fxrstor:
  case Intrinsic::Fxsave64Mem:
  case Intrinsic::Fxrstor64Mem:
  case Intrinsic::Xsave:
  case Intrinsic::Xsavec:
  case Intrinsic::Xsaves:
  case Intrinsic::Xsaveopt:
  case Intrinsic::Xrstor:
  case Intrinsic::Xrstors:
  case Intrinsic::Xsave64:
  case Intrinsic::Xsavec64:
  case Intrinsic::Xsaves64:
  case Intrinsic::Xsaveopt64:
  case Intrinsic::Xrstor64:
  case Intrinsic::Xrstors64:
  case Intrinsic::X87Fldenv:
  case Intrinsic::X87Fnstenv:
  case Intrinsic::X87Frstor:
  case Intrinsic::X87Fnsave:
  case Intrinsic::Lgdt:
  case Intrinsic::Lidt:
  case Intrinsic::Sgdt:
  case Intrinsic::Sidt:
  case Intrinsic::Invlpg:
  case Intrinsic::Lldt:
  case Intrinsic::Ltr:
  case Intrinsic::Lmsw:
  case Intrinsic::Sldt:
  case Intrinsic::Str:
  case Intrinsic::Smsw:
  case Intrinsic::AMXLoadConfig:
  case Intrinsic::AMXStoreConfig:
  case Intrinsic::AMXTileLoad:
  case Intrinsic::AMXTileStore:
  case Intrinsic::X86FourFMA:
  case Intrinsic::X86VP4DPWSSD:
  case Intrinsic::X86VP4DPWSSDS:
  case Intrinsic::ApxRaoAdd:
  case Intrinsic::ApxRaoAnd:
  case Intrinsic::ApxRaoOr:
  case Intrinsic::ApxRaoXor:
  case Intrinsic::ApxCmpccXadd:
  case Intrinsic::CetWrss:
  case Intrinsic::CetWruss:
  case Intrinsic::Enqcmd:
  case Intrinsic::Enqcmds:
  case Intrinsic::X86Invalidate:
  case Intrinsic::RequireAligned:
    return true;
  default:
    return false;
  }
}

constexpr bool isX86StringIntrinsic(Intrinsic Id) {
  switch (Id) {
  case Intrinsic::Movsb:
  case Intrinsic::Movsw:
  case Intrinsic::Movsd:
  case Intrinsic::Movsq:
  case Intrinsic::Stosb:
  case Intrinsic::Stosw:
  case Intrinsic::Stosd:
  case Intrinsic::Stosq:
  case Intrinsic::Lodsb:
  case Intrinsic::Lodsw:
  case Intrinsic::Lodsd:
  case Intrinsic::Lodsq:
  case Intrinsic::Cmpsb:
  case Intrinsic::Cmpsw:
  case Intrinsic::Cmpsd_str:
  case Intrinsic::Cmpsq:
  case Intrinsic::Scasb:
  case Intrinsic::Scasw:
  case Intrinsic::Scasd:
  case Intrinsic::Scasq:
  case Intrinsic::Outsb:
  case Intrinsic::Outsw:
  case Intrinsic::Outsd:
  case Intrinsic::Insb:
  case Intrinsic::Insw:
  case Intrinsic::Insd:
    return true;
  default:
    return false;
  }
}

/// X86FPClass inputs are: intrinsic ID, control, source, active mask, and
/// imm8.  Control bit 0 selects F64 lanes and bit 1 selects scalar topology;
/// callers separately prove that control is a constant containing no other
/// bits.
constexpr bool
intrinsicX86FPClassShapeIsValid(uint8_t NumInputs, uint16_t OutputSize,
                                uint8_t Control, uint16_t ControlSize,
                                uint16_t SourceSize, uint16_t MaskSize,
                                uint16_t ImmediateSize) {
  if (NumInputs != 5 || ControlSize != 1 || ImmediateSize != 1 ||
      (SourceSize != 16 && SourceSize != 32 && SourceSize != 64) ||
      MaskSize < 1 || MaskSize > 8)
    return false;

  const bool F64 = (Control & 1) != 0;
  const bool Scalar = (Control & 2) != 0;
  if (Scalar && SourceSize != 16)
    return false;

  const uint16_t ElementSize = F64 ? 8 : 4;
  const uint16_t LaneCount = Scalar ? 1 : SourceSize / ElementSize;
  const uint16_t ExpectedOutputSize = LaneCount > 8 ? 2 : 1;
  return OutputSize == ExpectedOutputSize && LaneCount <= MaskSize * 8;
}

/// NumInputs includes the constant intrinsic ID at index 0.  Every string
/// family carries its complete architectural register state, even where the
/// memory segment is fixed to ES/default rather than selectable with FS/GS.
constexpr bool intrinsicStringShapeIsValid(Intrinsic Id, uint8_t NumInputs,
                                           uint16_t OutputSize,
                                           uint16_t AddressSize) {
  const bool ValidAddressSize =
      AddressSize == 2 || AddressSize == 4 || AddressSize == 8;
  switch (Id) {
  case Intrinsic::Movsb:
  case Intrinsic::Movsw:
  case Intrinsic::Movsd:
  case Intrinsic::Movsq:
  case Intrinsic::Stosb:
  case Intrinsic::Stosw:
  case Intrinsic::Stosd:
  case Intrinsic::Stosq:
  case Intrinsic::Lodsb:
  case Intrinsic::Lodsw:
  case Intrinsic::Lodsd:
  case Intrinsic::Lodsq:
    return NumInputs >= 5 && OutputSize != 0 && ValidAddressSize;
  case Intrinsic::Cmpsb:
  case Intrinsic::Cmpsw:
  case Intrinsic::Cmpsd_str:
  case Intrinsic::Cmpsq:
  case Intrinsic::Scasb:
  case Intrinsic::Scasw:
  case Intrinsic::Scasd:
  case Intrinsic::Scasq:
    return NumInputs >= 6 && OutputSize != 0 && ValidAddressSize;
  case Intrinsic::Outsb:
  case Intrinsic::Outsw:
  case Intrinsic::Outsd:
  case Intrinsic::Insb:
  case Intrinsic::Insw:
  case Intrinsic::Insd:
    return NumInputs >= 5 && OutputSize == 0 && ValidAddressSize;
  default:
    return false;
  }
}

/// Validate the operand/output shape required by the address-space-aware
/// implementation.  Keeping this next to the whitelist makes it impossible
/// for malformed segmented memory IR to reach a generic backend fallback.
constexpr bool intrinsicMemoryAddressSpaceShapeIsValid(
    Intrinsic Id, uint8_t NumInputs, uint16_t OutputSize, uint16_t AddressSize,
    uint16_t MaskSize, uint16_t DataSize) {
  switch (Id) {
  case Intrinsic::Movsb:
  case Intrinsic::Movsw:
  case Intrinsic::Movsd:
  case Intrinsic::Movsq:
  case Intrinsic::Lodsb:
  case Intrinsic::Lodsw:
  case Intrinsic::Lodsd:
  case Intrinsic::Lodsq:
  case Intrinsic::Cmpsb:
  case Intrinsic::Cmpsw:
  case Intrinsic::Cmpsd_str:
  case Intrinsic::Cmpsq:
  case Intrinsic::Outsb:
  case Intrinsic::Outsw:
  case Intrinsic::Outsd:
    return intrinsicStringShapeIsValid(Id, NumInputs, OutputSize, AddressSize);
  case Intrinsic::MaskedLoadB:
  case Intrinsic::MaskedLoadW:
  case Intrinsic::MaskedLoadD:
  case Intrinsic::MaskedLoadQ:
    return NumInputs >= 3 && AddressSize == 8 &&
           (OutputSize == 16 || OutputSize == 32 || OutputSize == 64) &&
           MaskSize == OutputSize;
  case Intrinsic::MaskedStoreW:
  case Intrinsic::MaskedStoreD:
  case Intrinsic::MaskedStoreQ:
    return NumInputs >= 4 && OutputSize == 0 && AddressSize == 8 &&
           (MaskSize == 16 || MaskSize == 32 || MaskSize == 64) &&
           DataSize == MaskSize;
  case Intrinsic::MaskedStoreB:
    return NumInputs >= 4 && OutputSize == 0 && AddressSize == 8 &&
           (MaskSize == 8 || MaskSize == 16 || MaskSize == 32 ||
            MaskSize == 64) &&
           DataSize == MaskSize;
  case Intrinsic::Clflush:
  case Intrinsic::Clflushopt:
  case Intrinsic::Clwb:
  case Intrinsic::Prefetch:
  case Intrinsic::PrefetchT0:
  case Intrinsic::PrefetchT1:
  case Intrinsic::PrefetchT2:
  case Intrinsic::PrefetchNta:
  case Intrinsic::PrefetchW:
  case Intrinsic::PrefetchWT1:
  case Intrinsic::Ldmxcsr:
  case Intrinsic::Stmxcsr:
  case Intrinsic::Fxsave:
  case Intrinsic::Fxrstor:
  case Intrinsic::Fxsave64Mem:
  case Intrinsic::Fxrstor64Mem:
  case Intrinsic::X87Fldenv:
  case Intrinsic::X87Fnstenv:
  case Intrinsic::X87Frstor:
  case Intrinsic::X87Fnsave:
  case Intrinsic::Lgdt:
  case Intrinsic::Lidt:
  case Intrinsic::Sgdt:
  case Intrinsic::Sidt:
  case Intrinsic::Invlpg:
  case Intrinsic::Lldt:
  case Intrinsic::Ltr:
  case Intrinsic::Lmsw:
  case Intrinsic::Sldt:
  case Intrinsic::Str:
  case Intrinsic::Smsw:
    return NumInputs >= 2 && OutputSize == 0 && AddressSize == 8;
  case Intrinsic::CetWrss:
  case Intrinsic::CetWruss:
    // [id, effective-address, scalar-source].  The source's NdVar width is
    // the architectural WRSSD/WRSSQ or WRUSSD/WRUSSQ transfer width.
    return NumInputs == 3 && OutputSize == 0 && AddressSize == 8 &&
           (MaskSize == 4 || MaskSize == 8) && DataSize == 0;
  case Intrinsic::Enqcmd:
  case Intrinsic::Enqcmds:
    // [id, command-source-address, portal-address] -> ZF.  The intrinsic owns
    // the ordered 64-byte source read because PASID/CPL checks precede it.
    // Input 1 owns MemoryAddressSpace, matching every other memory intrinsic.
    return NumInputs == 3 && OutputSize == 1 && AddressSize == 8 &&
           MaskSize == 8 && DataSize == 0;
  case Intrinsic::X86Invalidate:
    // [id, descriptor-effective-address, invalidation-kind, type].  The kind
    // value itself is constrained by the LowIR/MedIR representation adapters.
    return NumInputs == 4 && OutputSize == 0 && AddressSize == 8 &&
           MaskSize == 1 && (DataSize == 4 || DataSize == 8);
  case Intrinsic::RequireAligned:
    // [id, effective-address, required-alignment, optional-guard].  A false
    // guard suppresses both address-space resolution and the alignment fault,
    // matching an all-zero EVEX memory writemask.
    return (NumInputs == 3 || NumInputs == 4) && OutputSize == 0 &&
           AddressSize == 8 && MaskSize == 8 &&
           (NumInputs == 3 ? DataSize == 0 : DataSize == 1);
  case Intrinsic::AMXLoadConfig:
    return NumInputs == 3 && OutputSize == 64 && AddressSize == 8 &&
           MaskSize == 1;
  case Intrinsic::AMXStoreConfig:
    return NumInputs == 4 && OutputSize == 0 && AddressSize == 8 &&
           MaskSize == 64 && DataSize == 1;
  case Intrinsic::AMXTileLoad:
    return NumInputs == 6 && OutputSize == 1024 && AddressSize == 8 &&
           MaskSize == 8 && DataSize == 64;
  case Intrinsic::AMXTileStore:
    return NumInputs == 6 && OutputSize == 64 && AddressSize == 8 &&
           MaskSize == 8 && DataSize == 64;
  case Intrinsic::X86FourFMA:
    return NumInputs == 6 && (OutputSize == 16 || OutputSize == 64) &&
           AddressSize == 8 && MaskSize == OutputSize && DataSize == 1;
  case Intrinsic::X86VP4DPWSSD:
  case Intrinsic::X86VP4DPWSSDS:
    return NumInputs == 6 && OutputSize == 64 && AddressSize == 8 &&
           MaskSize == 64 && DataSize == 1;
  case Intrinsic::ApxRaoAdd:
  case Intrinsic::ApxRaoAnd:
  case Intrinsic::ApxRaoOr:
  case Intrinsic::ApxRaoXor:
    return NumInputs == 3 && OutputSize == 0 && AddressSize == 8 &&
           (MaskSize == 4 || MaskSize == 8) && DataSize == 0;
  case Intrinsic::ApxCmpccXadd:
    return NumInputs == 5 && (OutputSize == 4 || OutputSize == 8) &&
           AddressSize == 8 && MaskSize == OutputSize && DataSize == OutputSize;
  case Intrinsic::Xsave:
  case Intrinsic::Xsavec:
  case Intrinsic::Xsaves:
  case Intrinsic::Xsaveopt:
  case Intrinsic::Xrstor:
  case Intrinsic::Xrstors:
  case Intrinsic::Xsave64:
  case Intrinsic::Xsavec64:
  case Intrinsic::Xsaves64:
  case Intrinsic::Xsaveopt64:
  case Intrinsic::Xrstor64:
  case Intrinsic::Xrstors64:
    return NumInputs >= 4 && OutputSize == 0 && AddressSize == 8;
  default:
    return false;
  }
}

/// Some x86 system instructions share an intrinsic ID between r/m16 memory
/// and register encodings.  A default address space can therefore denote a
/// genuine register form; an explicit FS/GS address space never can.
constexpr bool intrinsicDefaultRegisterShapeIsValid(Intrinsic Id,
                                                    uint8_t NumInputs,
                                                    uint16_t OutputSize,
                                                    uint16_t OperandSize) {
  switch (Id) {
  case Intrinsic::Lldt:
  case Intrinsic::Ltr:
  case Intrinsic::Lmsw:
    return NumInputs >= 2 && OutputSize == 0 && OperandSize == 2;
  case Intrinsic::Sldt:
  case Intrinsic::Str:
  case Intrinsic::Smsw:
    return NumInputs == 1 &&
           (OutputSize == 2 || OutputSize == 4 || OutputSize == 8);
  default:
    return false;
  }
}

const char *intrinsicName(Intrinsic Id);
const char *intrinsicCName(Intrinsic Id);
const char *intrinsicAsmMnemonic(Intrinsic Id);
const char *llvmIntrinsicToCName(const char *LLVMName);
Intrinsic intrinsicFromName(const char *Name);
bool isSideeffectIntrinsic(Intrinsic Id);
uint8_t intrinsicOutputCount(Intrinsic Id);

} // namespace neverd

#endif // NEVERD_IR_INTRINSICS_INTRINSICS_H
