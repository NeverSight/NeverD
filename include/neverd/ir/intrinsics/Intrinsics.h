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

#include <optional>
#include <string>

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
  RdmsrVexImmediate,
  WrmsrnsVexImmediate,
  Count,
};

constexpr bool x86MsrAccessIsWrite(X86MsrAccessKind Kind) {
  switch (Kind) {
  case X86MsrAccessKind::WrmsrnsImmediate:
  case X86MsrAccessKind::WrmsrnsVexImmediate:
  case X86MsrAccessKind::UwrmsrLegacyRegister:
  case X86MsrAccessKind::UwrmsrVexImmediate:
  case X86MsrAccessKind::UwrmsrEvexRegister:
  case X86MsrAccessKind::UwrmsrEvexImmediate:
    return true;
  case X86MsrAccessKind::RdmsrImmediate:
  case X86MsrAccessKind::RdmsrVexImmediate:
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
  case X86MsrAccessKind::RdmsrVexImmediate:
  case X86MsrAccessKind::WrmsrnsImmediate:
  case X86MsrAccessKind::WrmsrnsVexImmediate:
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

constexpr uint8_t makeX86FPRoundTransformControl(X86FPRoundTransformKind Kind,
                                                 bool IsF64, bool Scalar,
                                                 bool SuppressExceptions) {
  return static_cast<uint8_t>(Kind) | (IsF64 ? UINT8_C(1) << 1 : 0) |
         (Scalar ? UINT8_C(1) << 2 : 0) |
         (SuppressExceptions ? UINT8_C(1) << 3 : 0);
}

constexpr bool isValidX86FPRoundTransformControl(uint8_t Control) {
  constexpr uint8_t KnownBits = UINT8_C(0x0f);
  const auto Kind = static_cast<X86FPRoundTransformKind>(Control & 1U);
  return (Control & ~KnownBits) == 0 && Kind <= X86FPRoundTransformKind::Reduce;
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

  /// A retained raw-analysis environment precondition, never a runtime call.
  /// Its exact LowIR contract is owned by LowRawEnvironment.h.
  RawEnvironmentRequirementV1 = 2,

#include "neverd/ir/intrinsics/AArch64/intrinsics_aarch64.inc"
#include "neverd/ir/intrinsics/ARM/intrinsics_arm.inc"
#include "neverd/ir/intrinsics/X86/intrinsics_x86.inc"
#include "neverd/ir/intrinsics/X86/intrinsics_x86_fp_state.inc"

  // Append after the existing x86 FP family to preserve every published ID.
  // LoadConstant retains its ROM selector and explicit incoming control word.
  X87ReadControl,
  X87WriteControl,
  X87LoadConstant,
  // Append-only: scalar comparison flags below the outgoing MXCSR.
  X86FPComiState,
  X86FPComiMemoryState,
  // Append-only: raw scalar/packed masks below the outgoing MXCSR.
  X86FPMaskCompareState,
  X86FPMaskCompareMemoryState,

  // Explicit, function-owned x87 context. Keep TOP, tags, pending exceptions
  // and all eight raw values together rather than projecting host long double.
  X87ContextBegin,
  X87ContextRefresh,
  X87ContextExport,
  X87ContextStep,
  X87ContextMMXRead,
  X87ContextMMXWrite,
  X87ContextMMXEnter,

  /// Compiler-private register exchange for a checked, shared-frame x86 SEH
  /// finally invocation. These are introduced by MedIR, never by decoding.
  X86SEHRegisterRead,
  X86SEHRegisterWrite,

// Append after every existing family: serialized intrinsic IDs are stable.
#define NEVERD_X86_VIRTUALIZATION(Id, ...) Id,
#include "neverd/ir/intrinsics/X86/X86Virtualization.def"
#undef NEVERD_X86_VIRTUALIZATION

#define NEVERD_X86_SYSTEM_STATE(Id, ...) Id,
#include "neverd/ir/intrinsics/X86/X86SystemState.def"
#undef NEVERD_X86_SYSTEM_STATE

  // Append-only: bounded SGX supervisor/VMM completion.
  X86Encls,
  X86Enclv,
  // Append-only: i386 source-segment/implicit-ES block transaction.
  X86SegmentedBlockMemory,
  X86AMXState,

// Append after existing families to preserve serialized intrinsic IDs.
#define NEVERD_A64_CPA(Id, ...) Id,
#include "neverd/ir/intrinsics/AArch64/AArch64CPA.def"

#define NEVERD_A64_PAUTH_LR_OPERATION(Id, ...) Id,
#include "neverd/ir/intrinsics/AArch64/AArch64PAuthLR.def"

  // Append-only: typed x87 rotation and free/pop. FFREEP carries the same
  // physical-slot and decoded-relative-index operands as FFREE.
  X87Fdecstp,
  X87Ffreep,
  // Append-only: raw packed-BCD/binary80 conversion under an explicit CW.
  X87LoadPackedBCD,
  X87StorePackedBCD,
  // Append-only: one native operation returns ST(0), then ST(1), under
  // explicit finite/masked guards before the typed stack commits its push.
  X87FptanPair,
  X87FsincosPair,
  X87FxtractPair,

  // CFG consumes the pending prefix before publishing a native operation.
  A64_PACMPending,
#define NEVERD_A64_PAUTH_M_OPERATION(Id, ...) Id,
#include "neverd/ir/intrinsics/AArch64/AArch64PAuthM.def"

// Compiler-private portable runtime calls. Append to preserve serialized IDs.
#define NEVERD_X87_PORTABLE_HELPER(Role, ...) PortableX87##Role,
#include "neverd/ir/X86/X87PortableABI.def"
#undef NEVERD_X87_PORTABLE_HELPER

  // Append-only: one descriptor query owns the value, ZF and selector access.
  X86LoadSegmentLimit,
  X86LoadSegmentLimitMemory,

  // Append-only: legacy descriptor-table operand-size attribute.
  X86Lgdt16,
  X86Lidt16,
  X86Sgdt16,
  X86Sidt16,

// Append-only: combined authentication, memory access and base writeback.
#define NEVERD_A64_PAUTH_LOAD_OPERATION(Id, ...) Id,
#include "neverd/ir/intrinsics/AArch64/AArch64PAuthLoad.def"

// Append-only: scalable checked pointer operations.
#define NEVERD_A64_SVE_CPA_OPERATION(Id, ...) Id,
#include "neverd/ir/intrinsics/AArch64/AArch64SVECPA.def"

// Append-only: predicated copies of validated MOVPRFX/CPA pairs.
#define NEVERD_A64_SVE_PREFIX_OPERATION(Id, ...) Id,
#include "neverd/ir/intrinsics/AArch64/AArch64SVEPrefix.def"

// Append-only: natural-register REP completion.
#define NEVERD_X86_REP_STATE(Id, ...) Id,
#include "neverd/ir/intrinsics/X86/X86RepState.def"
#undef NEVERD_X86_REP_STATE

  // Append-only: CFG proves a bounded leaf before publishing native state.
  X86EncluPending,
  X86GetsecPending,
  X86EncluBounded,
  X86GetsecBounded,

// Append-only: original memory instructions retain CPA2 address semantics.
#define NEVERD_A64_ADDRESS_MEMORY_OPERATION(Id, ...) Id,
#define NEVERD_A64_ADDRESS_VECTOR_MEMORY_OPERATION(...)
#define NEVERD_A64_ADDRESS_IMMEDIATE_MEMORY_OPERATION(...)
#include "neverd/ir/intrinsics/AArch64/AArch64AddressMemoryOperations.def"

  // Append-only: all four vector sources are explicit SSA values in one
  // packed operand. Legacy physical-block IDs retain their serialized values.
  X86FourSourceMemory,

// Append-only: keep every previously published scalar and x86 identity.
#define NEVERD_A64_ADDRESS_VECTOR_MEMORY_OPERATION(Id, ...) Id,
#include "neverd/ir/intrinsics/AArch64/AArch64AddressMemoryOperations.def"

  // Append-only: preserve legacy RAO/VEX encoding independently of APX.
  X86LegacyRaoAdd,
  X86LegacyRaoAnd,
  X86LegacyRaoOr,
  X86LegacyRaoXor,
  X86LegacyCmpccXadd,

#define NEVERD_A64_ADDRESS_IMMEDIATE_MEMORY_OPERATION(Id, ...) Id,
#include "neverd/ir/intrinsics/AArch64/AArch64AddressMemoryOperations.def"

  _Count = 16000
};

constexpr bool isPortableX87Intrinsic(Intrinsic Id) {
  switch (Id) {
#define NEVERD_X87_PORTABLE_HELPER(Role, ...) case Intrinsic::PortableX87##Role:
#include "neverd/ir/X86/X87PortableABI.def"
#undef NEVERD_X87_PORTABLE_HELPER
    return true;
  default:
    return false;
  }
}

enum class X86SystemStateKind : uint8_t {
  ReadPid,
  ReadBase,
  WriteBase,
  Void,
  Monitor,
  Wait,
  AccessRights,
  AdjustRpl,
  ShadowWrite,
  Enqueue,
  DirectStore,
  DirectStore64B,
  WriteMsr,
  MsrList,
  Void64,
  TestUserInterrupt,
  SendUserInterrupt,
  LoadKernelGs,
  HistoryReset,
  InvalidateBroadcast,
  SegmentedBlock,
  CacheZero,
  TransactionAbort
};

struct X86SystemStateContract {
  const char *Mnemonic = nullptr;
  X86SystemStateKind Kind = X86SystemStateKind::Void;
  int8_t MemoryArgument = -1;
};

constexpr X86SystemStateContract x86SystemStateContract(Intrinsic Id) {
  switch (Id) {
#define NEVERD_X86_SYSTEM_STATE(Id, Mnemonic, Kind, MemoryArgument)            \
  case Intrinsic::Id:                                                          \
    return {Mnemonic, X86SystemStateKind::Kind, MemoryArgument};
#define NEVERD_X86_EXISTING_SYSTEM_STATE NEVERD_X86_SYSTEM_STATE
#define NEVERD_X86_APPENDED_SYSTEM_STATE NEVERD_X86_SYSTEM_STATE
#include "neverd/ir/intrinsics/X86/X86SystemState.def"
#undef NEVERD_X86_APPENDED_SYSTEM_STATE
#undef NEVERD_X86_EXISTING_SYSTEM_STATE
#undef NEVERD_X86_SYSTEM_STATE
  default:
    return {};
  }
}

constexpr bool isX86SystemStateIntrinsic(Intrinsic Id) {
  return x86SystemStateContract(Id).Mnemonic != nullptr;
}

/// Enqueue, block direct stores and MSR lists take a second address. Only the
/// first address may have a segment override; MSR lists use linear addresses.
constexpr bool isX86SystemStateAddressArgument(Intrinsic Id, unsigned Index) {
  const auto C = x86SystemStateContract(Id);
  return (C.MemoryArgument >= 0 && Index == unsigned(C.MemoryArgument)) ||
         ((C.Kind == X86SystemStateKind::Enqueue ||
           C.Kind == X86SystemStateKind::DirectStore64B ||
           C.Kind == X86SystemStateKind::MsrList) &&
          Index == 1);
}

enum class X86VirtualizationResult : uint8_t { None, Flags, Read };

struct X86VirtualizationContract {
  const char *Mnemonic = nullptr;
  X86VirtualizationResult Result = X86VirtualizationResult::None;
  uint8_t Arguments = 0;
  int8_t MemoryArgument = -1;
  uint16_t MemoryBytes = 0;
  bool PhysicalAddress = false;
};

constexpr X86VirtualizationContract x86VirtualizationContract(Intrinsic Id) {
  using R = X86VirtualizationResult;
  switch (Id) {
#define NEVERD_X86_VIRTUALIZATION(Id, Mnemonic, Result, Arguments,             \
                                  MemoryArgument, MemoryBytes,                 \
                                  PhysicalAddress)                             \
  case Intrinsic::Id:                                                          \
    return {Mnemonic,       R::Result,   Arguments,                            \
            MemoryArgument, MemoryBytes, PhysicalAddress};
#include "neverd/ir/intrinsics/X86/X86Virtualization.def"
#undef NEVERD_X86_VIRTUALIZATION
  default:
    return {};
  }
}

constexpr bool isX86VirtualizationIntrinsic(Intrinsic Id) {
  return x86VirtualizationContract(Id).Mnemonic != nullptr;
}

/// True for the complete architectural AMX state surface represented in
/// LowIR.  Keep this classification centralized: AMX operations mix ordinary
/// value results with restartable memory effects, so treating only the four
/// memory forms as a family is not sufficient at backend boundaries.
constexpr bool isAMXIntrinsic(Intrinsic Id) {
  switch (Id) {
  case Intrinsic::X86AMXState:
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

/// Map encoding identities to the shared arithmetic/transaction contract.
constexpr Intrinsic x86AtomicSemanticId(Intrinsic Id) {
  switch (Id) {
  case Intrinsic::X86LegacyRaoAdd:
    return Intrinsic::ApxRaoAdd;
  case Intrinsic::X86LegacyRaoAnd:
    return Intrinsic::ApxRaoAnd;
  case Intrinsic::X86LegacyRaoOr:
    return Intrinsic::ApxRaoOr;
  case Intrinsic::X86LegacyRaoXor:
    return Intrinsic::ApxRaoXor;
  case Intrinsic::X86LegacyCmpccXadd:
    return Intrinsic::ApxCmpccXadd;
  default:
    return Id;
  }
}
constexpr bool isX86LegacyAtomicIntrinsic(Intrinsic Id) {
  return x86AtomicSemanticId(Id) != Id;
}
constexpr bool isX86CmpccXaddIntrinsic(Intrinsic Id) {
  return x86AtomicSemanticId(Id) == Intrinsic::ApxCmpccXadd;
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

constexpr bool isX86AtomicIntrinsic(Intrinsic Id) {
  return isApxAtomicIntrinsic(x86AtomicSemanticId(Id));
}

/// Representation-neutral APX atomic operand facts.  LowIR and MedIR use
/// different value kinds, so each caller classifies its own variables and
/// passes only the semantic facts which the shared contract owns.  Unknown is
/// accepted only where the containing IR has no target field; every known
/// target must be x86-64, except the 32-bit legacy RAO form.
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
  const bool I386 = isX86LegacyAtomicIntrinsic(Id) &&
                    !isX86CmpccXaddIntrinsic(Id) && Shape.SourceSize == 4;
  if (!isX86AtomicIntrinsic(Id) ||
      (Shape.TargetArch != Arch::Unknown && Shape.TargetArch != Arch::X64 &&
       !(Shape.TargetArch == Arch::X86 && I386)) ||
      !Shape.IntrinsicIdIsConst || Shape.IntrinsicIdSize != 2 ||
      !Shape.AddressIsScalar || Shape.AddressSize != 8)
    return false;

  if (!isX86CmpccXaddIntrinsic(Id))
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
/// Native execution retains the encoding family; the CPU owns
/// CPUID/XCR0/CPL, USER_MSR_CTL bitmap,
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
  bool SelectorIsConst = false;
  uint16_t SelectorSize = 0;
  bool ValueIsScalar = false;
  uint16_t ValueSize = 0;
};

constexpr bool
intrinsicX86MsrAccessShapeIsValid(Intrinsic Id,
                                  const X86MsrAccessIntrinsicShape &Shape) {
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
  if ((x86MsrAccessHasImmediateSelector(Kind) && !Shape.SelectorIsConst) ||
      Shape.SelectorSize !=
          (x86MsrAccessHasImmediateSelector(Kind) ? uint16_t{4} : uint16_t{8}))
    return false;
  if (x86MsrAccessIsWrite(Kind))
    return Shape.NumInputs == 4 && Shape.OutputSize == 0 &&
           Shape.ValueIsScalar && Shape.ValueSize == 8;
  return Shape.NumInputs == 3 && Shape.OutputIsWritableScalar &&
         Shape.OutputSize == 8 && !Shape.ValueIsScalar && Shape.ValueSize == 0;
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
constexpr bool
intrinsicX86VP4DPShapeIsValid(Intrinsic Id, uint8_t NumInputs,
                              uint16_t OutputSize, uint16_t AddressSize,
                              uint16_t DestinationSize, uint16_t GroupSize,
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
  if (isX86VirtualizationIntrinsic(Id))
    return x86VirtualizationContract(Id).MemoryArgument >= 0;
  if (isX86SystemStateIntrinsic(Id))
    return x86SystemStateContract(Id).MemoryArgument >= 0;
  switch (Id) {
  case Intrinsic::EVEXCompressStore:
  case Intrinsic::EVEXExpandLoad:
  case Intrinsic::X86FourSourceMemory:
  case Intrinsic::X86AMXState:
    return true;
  case Intrinsic::X86FPRoundMemoryState:
  case Intrinsic::X86FPMaskCompareMemoryState:
  case Intrinsic::X86FPComiMemoryState:
  case Intrinsic::X86FPScalarConvertMemoryState:
  case Intrinsic::X86F16CReadState:
  case Intrinsic::X86F16CStoreState:
  case Intrinsic::X86FPPackedConvertMemoryState:
  case Intrinsic::X86FPApprox12MemoryState:
  case Intrinsic::Movsb:
  case Intrinsic::X86FPArithMemoryState:
  case Intrinsic::X86FPFmaMemoryState:
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
  case Intrinsic::X87ContextStep:
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
  case Intrinsic::X86Lgdt16:
  case Intrinsic::X86Lidt16:
  case Intrinsic::X86Sgdt16:
  case Intrinsic::X86Sidt16:
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
  case Intrinsic::Verr:
  case Intrinsic::Verw:
  case Intrinsic::AMXLoadConfig:
  case Intrinsic::AMXStoreConfig:
  case Intrinsic::AMXTileLoad:
  case Intrinsic::AMXTileStore:
  case Intrinsic::X86FourFMA:
  case Intrinsic::X86VP4DPWSSD:
  case Intrinsic::X86VP4DPWSSDS:
  case Intrinsic::X86LegacyRaoAdd:
  case Intrinsic::ApxRaoAdd:
  case Intrinsic::X86LegacyRaoAnd:
  case Intrinsic::ApxRaoAnd:
  case Intrinsic::X86LegacyRaoOr:
  case Intrinsic::ApxRaoOr:
  case Intrinsic::X86LegacyRaoXor:
  case Intrinsic::ApxRaoXor:
  case Intrinsic::X86LegacyCmpccXadd:
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
  case Intrinsic::EVEXCompressStore:
  case Intrinsic::EVEXExpandLoad:
    // Control, architecture and scalar kinds are validated by X86CompactShape.
    return NumInputs == 5 && AddressSize == 8 && MaskSize && MaskSize <= 8 &&
           (DataSize == 16 || DataSize == 32 || DataSize == 64) &&
           OutputSize == (Id == Intrinsic::EVEXExpandLoad ? DataSize : 0);

  case Intrinsic::X86FourSourceMemory:
    // [id,address,accumulator,packed four sources,mask,control].
    // The shared X86FourSourceShape owns the remaining control/type checks.
    return NumInputs == 6 && AddressSize == 8 &&
           (OutputSize == 16 || OutputSize == 64) && MaskSize == OutputSize &&
           DataSize == (OutputSize == 16 ? 16 : 4 * OutputSize);

  case Intrinsic::X86AMXState:
    // [id, base, versioned descriptor, optional stride]. The shared AMX
    // shape additionally validates the descriptor and owns non-memory forms.
    return (NumInputs == 3 || NumInputs == 4) && OutputSize == 0 &&
           AddressSize == 8 && MaskSize == 4 &&
           (DataSize == 0 || DataSize == 8);

  case Intrinsic::X87ContextStep:
    // [id,context,selector,address,flags,source-PC]; the full selector and
    // architecture contract belongs to X87Context.h.
    return NumInputs == 6 && AddressSize == 8 && MaskSize == 4 &&
           DataSize == 8 &&
           (OutputSize == 0 || OutputSize == 2 || OutputSize == 8);
  case Intrinsic::X86FPFmaMemoryState:
    return NumInputs == 6 && AddressSize == 8 && MaskSize == 2 &&
           DataSize + 4 == OutputSize &&
           (DataSize == 4 || DataSize == 8 || DataSize == 16 || DataSize == 32);
  case Intrinsic::X86FPArithMemoryState:
    // [id,address,control,left,MXCSR]; detailed validation is shared with IR.
    return NumInputs == 5 && AddressSize == 8 && MaskSize == 1 &&
           DataSize + 4 == OutputSize &&
           (DataSize == 4 || DataSize == 8 || DataSize == 16 || DataSize == 32);
  case Intrinsic::X86FPScalarConvertMemoryState:
    // [id,address,control,MXCSR]; widths are owned by X86FPState.h.
    return NumInputs == 4 && AddressSize == 8 && MaskSize == 1 &&
           DataSize == 4 && (OutputSize == 8 || OutputSize == 12);
  case Intrinsic::X86FPMaskCompareMemoryState:
    return NumInputs == 6 && AddressSize == 8 && MaskSize == 1 &&
           DataSize == 1 &&
           (OutputSize == 8 || OutputSize == 12 || OutputSize == 20 ||
            OutputSize == 36);
  case Intrinsic::X86FPComiMemoryState:
    return NumInputs == 5 && AddressSize == 8 && MaskSize == 1 &&
           (DataSize == 4 || DataSize == 8) && OutputSize == 8;
  case Intrinsic::X86F16CReadState:
    return NumInputs == 5 && AddressSize == 8 && MaskSize == 1 &&
           DataSize == 1 && (OutputSize == 20 || OutputSize == 36);
  case Intrinsic::X86F16CStoreState:
    return NumInputs == 6 && AddressSize == 8 &&
           (MaskSize == 16 || MaskSize == 32) && DataSize == 1 &&
           OutputSize == 4;
  case Intrinsic::X86FPPackedConvertMemoryState:
    // [id,address,control,MXCSR]; complete output includes register padding.
    return NumInputs == 4 && AddressSize == 8 && MaskSize == 1 &&
           DataSize == 4 && (OutputSize == 20 || OutputSize == 36);
  case Intrinsic::X86FPApprox12MemoryState:
    // [id, address, control], without a CSR operand or effect.
    return NumInputs == 3 && AddressSize == 8 && MaskSize == 1 &&
           DataSize == 0 &&
           (OutputSize == 4 || OutputSize == 16 || OutputSize == 32);
  case Intrinsic::X86FPRoundMemoryState:
    // [id, address, control, immediate, MXCSR]. Full numerical/state
    // validation belongs to the shared X86FPState representation adapters.
    return NumInputs == 5 && AddressSize == 8 && MaskSize == 1 &&
           DataSize == 1 &&
           (OutputSize == 8 || OutputSize == 12 || OutputSize == 20 ||
            OutputSize == 36);
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
  case Intrinsic::X86Lgdt16:
  case Intrinsic::X86Lidt16:
  case Intrinsic::X86Sgdt16:
  case Intrinsic::X86Sidt16:
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
  case Intrinsic::Verr:
  case Intrinsic::Verw:
    // [id, effective-address] -> ZF, or no output once ZF is dead.
    return NumInputs == 2 && (OutputSize == 1 || OutputSize == 0) &&
           AddressSize == 8;
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
  case Intrinsic::X86LegacyRaoAdd:
  case Intrinsic::ApxRaoAdd:
  case Intrinsic::X86LegacyRaoAnd:
  case Intrinsic::ApxRaoAnd:
  case Intrinsic::X86LegacyRaoOr:
  case Intrinsic::ApxRaoOr:
  case Intrinsic::X86LegacyRaoXor:
  case Intrinsic::ApxRaoXor:
    return NumInputs == 3 && OutputSize == 0 && AddressSize == 8 &&
           (MaskSize == 4 || MaskSize == 8) && DataSize == 0;
  case Intrinsic::X86LegacyCmpccXadd:
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

/// An x86 instruction that stores a descriptor-table selector or the machine
/// status word: the value it writes is what it runs for, so a function that
/// returns it returns a value.
constexpr bool isX86SystemRegisterStore(Intrinsic Id) {
  return Id == Intrinsic::Sldt || Id == Intrinsic::Str || Id == Intrinsic::Smsw;
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

/// Recognize the reserved encoding, including malformed wide aliases which
/// must not become ordinary intrinsics after a consumer narrows the ID.
constexpr bool isRawEnvironmentRequirementId(uint64_t Id) {
  return (Id & UINT16_MAX) ==
         static_cast<uint64_t>(Intrinsic::RawEnvironmentRequirementV1);
}

const char *intrinsicName(Intrinsic Id);
/// True only for intrinsic spellings that unconditionally terminate in C.
/// Debugger breakpoints such as Int3 and ArmBkpt are excluded.
bool isUnconditionalTrapIntrinsic(
    Intrinsic Id, std::optional<uint64_t> Operand = std::nullopt);
const char *intrinsicCName(Intrinsic Id,
                           std::optional<uint64_t> Operand = std::nullopt);
const char *intrinsicAsmMnemonic(Intrinsic Id);
const char *llvmIntrinsicToCName(const char *LLVMName);
Intrinsic intrinsicFromName(const char *Name);
bool isSideeffectIntrinsic(Intrinsic Id);
uint8_t intrinsicOutputCount(Intrinsic Id);

/// True when \p Id writes no register and no memory, so the arguments a call
/// sets up before it still reach the call: the x87 wait for pending
/// exceptions that FILD, FABS and FCHS also check, and the owned x87
/// context's transport around a call, which moves the function's private
/// context between its buffer and the FPU.
constexpr bool preservesCallSetup(Intrinsic Id) {
  return Id == Intrinsic::X87Wait || Id == Intrinsic::X87ContextExport ||
         Id == Intrinsic::X87ContextRefresh;
}

/// The control or debug register (`%cr8`, `%dr7`) that a ReadCr*/WriteCr*
/// intrinsic names, or that ReadDr/WriteDr names by its constant first
/// operand \p DebugIndex (DR0..7); nullopt for any other intrinsic or index.
std::optional<std::string> x86SystemRegisterName(Intrinsic Id,
                                                 uint64_t DebugIndex);

} // namespace neverd

#endif // NEVERD_IR_INTRINSICS_INTRINSICS_H
