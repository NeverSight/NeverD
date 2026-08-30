//===- LowIR.h - Low-level IR definitions -------------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Defines the low-level intermediate representation: NdVar, LowOp,
/// LowBlock, LowFunc, and JumpTable structures used as the initial
/// representation after instruction lifting.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_LOW_LOWIR_H
#define NEVERD_IR_LOW_LOWIR_H

#include "neverd/ir/NdOps.h"
#include "neverd/loader/ExceptionInfo.h"

#include "llvm/Support/Errc.h"
#include "llvm/Support/Error.h"

#include <algorithm>
#include <limits>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <vector>

namespace neverd {

constexpr uint64_t TmpBase = 0x10000;
constexpr uint64_t TmpStride = 8;

constexpr uint64_t DiscardXzr32 = 0xFFF0;
constexpr uint64_t DiscardXzr64 = 0xFFF8;

enum class VnodeSpace : uint8_t {
  REG,
  TEMP,
  CONST,
  RAM,
  STACK,
};

/// What an immediate means at this exact IR occurrence.  Numeric equality is
/// not provenance: the same value can be a scalar, an incomplete page base,
/// or a complete address in different operands.
enum class ConstantAddressProvenance : uint8_t {
  Unknown,
  Scalar,
  AddressFragment,
  Address,
  DataAddress,
  CodeAddress,
};

/// Whether a constant at this operand position is consumed as a numeric value
/// rather than transported as a first-class pointer.  Keep this occurrence
/// rule shared by initial LowIR emission and later COPY propagation: SSA
/// substitution must not turn an encoded scalar immediate back into an
/// address merely because its bits collide with a low image VA.
constexpr bool isNumericConstantOperand(NdOp Opcode, unsigned Index) {
  switch (Opcode) {
  case NdOp::INT_ADD:
  case NdOp::INT_SUB:
  case NdOp::INT_AND:
  case NdOp::INT_OR:
  case NdOp::INT_XOR:
  case NdOp::INT_LEFT:
  case NdOp::INT_RIGHT:
  case NdOp::INT_ASHR:
  case NdOp::INT_MULT:
  case NdOp::INT_DIV:
  case NdOp::INT_SDIV:
  case NdOp::INT_REM:
  case NdOp::INT_SREM:
  case NdOp::INT_EQUAL:
  case NdOp::INT_NOTEQUAL:
  case NdOp::INT_LESS:
  case NdOp::INT_SLESS:
  case NdOp::INT_LESSEQUAL:
  case NdOp::INT_SLESSEQUAL:
  case NdOp::INT_ZEXT:
  case NdOp::INT_SEXT:
  case NdOp::INT_NEGATE:
  case NdOp::INT_NOT:
  case NdOp::INT_CARRY:
  case NdOp::INT_SOVF:
  case NdOp::INT_SBOR:
  case NdOp::BOOL_AND:
  case NdOp::BOOL_OR:
  case NdOp::BOOL_XOR:
  case NdOp::BOOL_NOT:
  case NdOp::FLOAT_ADD:
  case NdOp::FLOAT_SUB:
  case NdOp::FLOAT_MULT:
  case NdOp::FLOAT_DIV:
  case NdOp::FLOAT_NEG:
  case NdOp::FLOAT_ABS:
  case NdOp::FLOAT_SQRT:
  case NdOp::FLOAT_EQUAL:
  case NdOp::FLOAT_NOTEQUAL:
  case NdOp::FLOAT_LESS:
  case NdOp::FLOAT_INT2FLOAT:
  case NdOp::FLOAT_FLOAT2INT:
  case NdOp::FLOAT_TRUNC:
  case NdOp::FLOAT_CEIL:
  case NdOp::FLOAT_FLOOR:
  case NdOp::INT_NEG2:
  case NdOp::POPCOUNT:
  case NdOp::FLOAT_LESSEQUAL:
  case NdOp::FLOAT_ISNAN:
  case NdOp::FLOAT_FLOAT2FLOAT:
  case NdOp::FLOAT_ROUND:
  case NdOp::LZCOUNT:
  case NdOp::FLOAT_UINT2FLOAT:
  case NdOp::FLOAT_FLOAT2UINT:
  case NdOp::FLOAT_FMA:
  case NdOp::FLOAT_ROUNDEVEN:
  case NdOp::FLOAT_MIN:
  case NdOp::FLOAT_MAX:
  case NdOp::FLOAT_MINNUM:
  case NdOp::FLOAT_MAXNUM:
    return true;
  case NdOp::SUBBYTES:
    return Index == 1;
  default:
    return false;
  }
}

constexpr bool isExactAddressProvenance(ConstantAddressProvenance Provenance) {
  return Provenance == ConstantAddressProvenance::Address ||
         Provenance == ConstantAddressProvenance::DataAddress ||
         Provenance == ConstantAddressProvenance::CodeAddress;
}

constexpr bool isDataAddressProvenance(ConstantAddressProvenance Provenance) {
  return Provenance == ConstantAddressProvenance::DataAddress;
}

constexpr bool isCodeAddressProvenance(ConstantAddressProvenance Provenance) {
  return Provenance == ConstantAddressProvenance::CodeAddress;
}

constexpr bool isAddressProvenance(ConstantAddressProvenance Provenance) {
  return isExactAddressProvenance(Provenance) ||
         Provenance == ConstantAddressProvenance::AddressFragment;
}

struct NdVar {
  VnodeSpace Space = VnodeSpace::CONST;
  uint64_t Offset = 0;
  uint16_t Size = 0;
  ConstantAddressProvenance Provenance = ConstantAddressProvenance::Unknown;
  /// Occurrence-local owner for a loader-authenticated address constant.
  /// Kept separate from the numeric payload so one-past == next-section-start
  /// does not silently change object identity after relinking.
  uint64_t AddressOwnerVA = InvalidVA;

  bool isConst() const { return Space == VnodeSpace::CONST; }
  bool isReg() const { return Space == VnodeSpace::REG; }
  bool isTemp() const { return Space == VnodeSpace::TEMP; }
  bool isRam() const { return Space == VnodeSpace::RAM; }

  bool operator==(const NdVar &O) const {
    return Space == O.Space && Offset == O.Offset && Size == O.Size &&
           Provenance == O.Provenance && AddressOwnerVA == O.AddressOwnerVA;
  }
  bool operator!=(const NdVar &O) const { return !(*this == O); }

  static NdVar reg(uint64_t Off, uint16_t Sz) {
    return {VnodeSpace::REG, Off, Sz};
  }
  static NdVar tmp(uint64_t Off, uint16_t Sz) {
    return {VnodeSpace::TEMP, Off, Sz};
  }
  static NdVar cst(uint64_t Val, uint16_t Sz) {
    return {VnodeSpace::CONST, Val, Sz};
  }
  static NdVar scalar(uint64_t Val, uint16_t Sz) {
    return {VnodeSpace::CONST, Val, Sz, ConstantAddressProvenance::Scalar};
  }
  static NdVar addressFragment(uint64_t Val, uint16_t Sz) {
    return {VnodeSpace::CONST, Val, Sz,
            ConstantAddressProvenance::AddressFragment};
  }
  static NdVar address(uint64_t Val, uint16_t Sz) {
    return {VnodeSpace::CONST, Val, Sz, ConstantAddressProvenance::Address};
  }
  static NdVar dataAddress(uint64_t Val, uint16_t Sz,
                           uint64_t OwnerVA = InvalidVA) {
    return {VnodeSpace::CONST, Val, Sz, ConstantAddressProvenance::DataAddress,
            OwnerVA};
  }
  static NdVar codeAddress(uint64_t Val, uint16_t Sz,
                           uint64_t OwnerVA = InvalidVA) {
    return {VnodeSpace::CONST, Val, Sz, ConstantAddressProvenance::CodeAddress,
            OwnerVA};
  }
  static NdVar ram(uint64_t Addr, uint16_t Sz) {
    return {VnodeSpace::RAM, Addr, Sz};
  }
};

struct LowOp {
  NdOp Opcode = NdOp::NOP;
  NdMemoryOrdering MemoryOrdering = NdMemoryOrdering::None;
  NdMemoryAddressSpace MemoryAddressSpace = NdMemoryAddressSpace::Default;
  NdVar Output = {};
  NdVar Inputs[6] = {};
  uint8_t NumInputs = 0;
  va_t Addr = 0;
  int Seq = 0;

  void addInput(NdVar V) {
    if (NumInputs < 6)
      Inputs[NumInputs++] = V;
  }
};

/// Canonical operand roles for LowIR memory effects.  LOAD/STORE may carry an
/// explicit address-space operand in input 0; atomics do not.  Keeping this
/// decoding beside LowOp prevents individual analyses from silently treating
/// a 3-input STORE's address-space token as its runtime address.
struct LowMemoryOperandView {
  const NdVar *Address = nullptr;
  const NdVar *StoredValue = nullptr;
  const NdVar *ExpectedValue = nullptr;
  uint16_t AccessSize = 0;
  bool Complete = false;
};

inline LowMemoryOperandView lowMemoryOperands(const LowOp &Op) {
  LowMemoryOperandView View;
  switch (Op.Opcode) {
  case NdOp::LOAD:
    if (Op.NumInputs == 0 || Op.Output.Size == 0)
      return View;
    View.Address = &Op.Inputs[Op.NumInputs >= 2 ? 1 : 0];
    View.AccessSize = Op.Output.Size;
    View.Complete = true;
    return View;
  case NdOp::STORE: {
    if (Op.NumInputs < 2)
      return View;
    const bool HasAddressSpace = Op.NumInputs >= 3;
    View.Address = &Op.Inputs[HasAddressSpace ? 1 : 0];
    View.StoredValue = &Op.Inputs[HasAddressSpace ? 2 : 1];
    View.AccessSize = View.StoredValue->Size;
    View.Complete = View.AccessSize != 0;
    return View;
  }
  case NdOp::ATOMIC_XCHG:
  case NdOp::ATOMIC_ADD:
    if (Op.NumInputs < 2 || Op.Output.Size == 0)
      return View;
    View.Address = &Op.Inputs[0];
    View.StoredValue = &Op.Inputs[1];
    View.AccessSize = Op.Output.Size;
    View.Complete = View.StoredValue->Size == View.AccessSize;
    return View;
  case NdOp::ATOMIC_CMPXCHG:
    if (Op.NumInputs < 3 || Op.Output.Size == 0)
      return View;
    View.Address = &Op.Inputs[0];
    View.ExpectedValue = &Op.Inputs[1];
    View.StoredValue = &Op.Inputs[2];
    View.AccessSize = Op.Output.Size;
    View.Complete = View.ExpectedValue->Size == View.AccessSize &&
                    View.StoredValue->Size == View.AccessSize;
    return View;
  default:
    return View;
  }
}

/// Instruction-level control classification retained beside flattened LowOps.
/// Modifiers such as conditional and indirect remain flags so combinations
/// produced by predicated ISAs do not need lossy enum cases.
enum class LowInstructionControl : uint8_t {
  None = 0,
  Branch = 1,
  Call = 2,
  Return = 3,
  TailCall = 4,
  ConditionalReturn = 5,
  ConditionalCall = 6,
  Terminator = 7,
};

enum class LowInstructionControlFlag : uint16_t {
  None = 0,
  Branch = 1u << 0,
  Conditional = 1u << 1,
  Call = 1u << 2,
  Return = 1u << 3,
  Indirect = 1u << 4,
  NoReturn = 1u << 5,
  Terminator = 1u << 6,
  Resumable = 1u << 7,
  /// The flattened COND_BR is an instruction-local predicate guard, not an
  /// independently encoded guest branch.  Its selected effect remains in the
  /// same LowInstructionBoundary.
  InstructionGuard = 1u << 8,
};

/// How a control transfer determines the execution mode at its destination.
/// This is separate from InstructionMode, which records the mode used to
/// decode the source instruction.  ARM B/BL preserve it, immediate BLX names a
/// fixed opposite mode, and register BX/BLX select it from target bit zero.
enum class LowInstructionTargetMode : uint8_t {
  Preserve = 0,
  ARM = 1,
  Thumb = 2,
  FromTargetBit0 = 3,
};

/// Canonical destination of one LowIR control transfer.
struct LowControlTarget {
  va_t Address = 0;
  InstructionMode Mode = InstructionMode::Default;
};

/// Resolve a raw control target according to its instruction boundary.
///
/// ARM and Thumb destinations are checked for their architectural alignment,
/// AArch32 targets must fit its address width, and InvalidVA is never a valid
/// result.  FromTargetBit0 consumes and clears bit zero.  Address zero remains
/// valid.  An error is returned instead of guessing whenever the source mode,
/// target-mode contract, address width, or alignment is inconsistent.
inline llvm::Expected<LowControlTarget>
canonicalizeLowControlTarget(uint64_t RawTarget, InstructionMode SourceMode,
                             LowInstructionTargetMode TargetMode) {
  const auto Invalid =
      [](const char *Message) -> llvm::Expected<LowControlTarget> {
    return llvm::createStringError(llvm::errc::invalid_argument, "%s", Message);
  };

  switch (SourceMode) {
  case InstructionMode::Default:
  case InstructionMode::ARM:
  case InstructionMode::Thumb:
    break;
  default:
    return Invalid("control target has an unknown source instruction mode");
  }

  const bool IsAArch32 = SourceMode == InstructionMode::ARM ||
                         SourceMode == InstructionMode::Thumb;
  if (RawTarget == InvalidVA)
    return Invalid("control target uses the invalid address");
  if (IsAArch32 && RawTarget > std::numeric_limits<uint32_t>::max())
    return Invalid("AArch32 control target exceeds its address width");

  LowControlTarget Result{RawTarget, SourceMode};
  switch (TargetMode) {
  case LowInstructionTargetMode::Preserve:
    break;
  case LowInstructionTargetMode::ARM:
    if (!IsAArch32)
      return Invalid("fixed ARM target has a non-ARM source mode");
    Result.Mode = InstructionMode::ARM;
    break;
  case LowInstructionTargetMode::Thumb:
    if (!IsAArch32)
      return Invalid("fixed Thumb target has a non-ARM source mode");
    Result.Mode = InstructionMode::Thumb;
    break;
  case LowInstructionTargetMode::FromTargetBit0:
    if (!IsAArch32)
      return Invalid("target-bit mode exchange has a non-ARM source mode");
    Result.Mode =
        (RawTarget & 1) != 0 ? InstructionMode::Thumb : InstructionMode::ARM;
    Result.Address = RawTarget & ~uint64_t{1};
    break;
  default:
    return Invalid("control target has an unknown destination-mode contract");
  }

  const uint64_t Alignment = Result.Mode == InstructionMode::ARM     ? 4
                             : Result.Mode == InstructionMode::Thumb ? 2
                                                                     : 1;
  if ((Result.Address & (Alignment - 1)) != 0)
    return Invalid("control target is not aligned for its destination mode");
  return Result;
}

constexpr LowInstructionControlFlag operator|(LowInstructionControlFlag Left,
                                              LowInstructionControlFlag Right) {
  return static_cast<LowInstructionControlFlag>(static_cast<uint16_t>(Left) |
                                                static_cast<uint16_t>(Right));
}

constexpr LowInstructionControlFlag &
operator|=(LowInstructionControlFlag &Left, LowInstructionControlFlag Right) {
  Left = Left | Right;
  return Left;
}

constexpr bool hasLowInstructionControlFlag(LowInstructionControlFlag Set,
                                            LowInstructionControlFlag Flag) {
  return (static_cast<uint16_t>(Set) & static_cast<uint16_t>(Flag)) != 0;
}

/// Exact provenance of one decoded guest instruction inside a LowBlock.
/// FirstOp and OpCount name a canonical, block-relative half-open slice in
/// LowBlock::Ops.  OpCount may be zero: a decoded instruction remains visible
/// even when lifting produces no LowOps.
struct LowInstructionBoundary {
  va_t Address = 0;
  uint16_t Size = 0;
  uint64_t FirstOp = 0;
  uint64_t OpCount = 0;
  InstructionMode Mode = InstructionMode::Default;
  LowInstructionControl Control = LowInstructionControl::None;
  LowInstructionControlFlag ControlFlags = LowInstructionControlFlag::None;

  /// Destination-mode contract for this instruction's control transfer.  Keep
  /// compact scalar provenance ahead of Immediate to avoid per-boundary tail
  /// padding across large functions.
  LowInstructionTargetMode TargetMode = LowInstructionTargetMode::Preserve;

  /// Direct branch/call target or an encoded return-pop immediate.  Absence is
  /// distinct from an explicitly encoded zero (for example `ret 0`).
  std::optional<uint64_t> Immediate;
};

struct LowBlock {
  int Id = -1;
  va_t StartAddr = 0;
  va_t EndAddr = 0;
  std::vector<LowOp> Ops;
  std::vector<LowInstructionBoundary> InstructionBoundaries;
  std::vector<int> Succs;
  std::vector<int> Preds;
  std::vector<ExceptionalEdge> ExceptionalSuccs;
  std::vector<ExceptionalEdge> ExceptionalPreds;

  bool hasSucc(int S) const {
    for (auto X : Succs)
      if (X == S)
        return true;
    return false;
  }

  bool hasInstructionBoundaries() const {
    return !InstructionBoundaries.empty();
  }
};

/// How an exact address occurrence was authenticated.  Most occurrences bind
/// a real loader field.  Linked images can also retain an architecture-defined
/// PC-relative address calculation after its relocation records are gone.  The
/// relocation-free kinds below carry occurrence-local semantic witnesses so a
/// numerically equal scalar cannot inherit their address identity.
enum class RelocatedInstructionAddressProofKind : uint8_t {
  LoaderField,
  AArch64RelocationFreeDataDereference,
  /// A decoded x86/x64 `lea reg,[rip/eip+disp]` with no surviving loader
  /// relocation.  The CFG builder binds the architecture-defined target to
  /// the exact final register-writing LowOp occurrence and republishes it only
  /// while that source instruction remains in the final reachable CFG.
  X86PCRelativeCodeAddress,
};

/// One exact full-width arithmetic step in a relocation-free AArch64 address
/// proof.  Retaining every occurrence prevents a later LowIR rewrite from
/// preserving only the same numeric result while changing the reaching ADRP
/// definition or introducing a partial-register alias.
struct RelocatedInstructionAddressArithmeticStep {
  va_t InstructionAddr = InvalidVA;
  int OpSeq = -1;
  NdOp Opcode = NdOp::NOP;
  uint8_t BaseInputIndex = 0;
  NdVar BaseInputWitness = {};
  NdVar ScalarInputWitness = {};
  NdVar OutputWitness = {};

  bool operator==(
      const RelocatedInstructionAddressArithmeticStep &Other) const = default;
};

/// One exact address occurrence that a final published LowIR instruction
/// consumed or defined.  Loader-side PC-relative records cannot name the
/// semantic target until the containing instruction has been decoded: x86
/// displacements are relative to the instruction end, which need not
/// immediately follow the relocation field.  Keeping the decoded
/// instruction/op occurrence beside the recomputed target prevents global
/// users from treating a loader approximation as address provenance.
struct RelocatedInstructionAddressOccurrence {
  va_t FieldVA = InvalidVA;
  va_t InstructionAddr = InvalidVA;
  int OpSeq = -1;
  va_t TargetVA = InvalidVA;
  va_t TargetOwnerVA = InvalidVA;
  uint8_t Width = 0;
  ConstantAddressProvenance Provenance = ConstantAddressProvenance::Unknown;
  bool PCRelativeFromInstructionEnd = false;
  /// True when the relocation authenticates this exact operation's output
  /// (for example a paired AArch64 ADRP+ADD), rather than a constant operand
  /// encoded in the instruction.  The producing CFG proof must validate the
  /// complete base+fragment chain before setting this bit.
  bool DefinesOutput = false;
  /// The exact output can depend on the authenticated materialization on only
  /// a subset of feasible paths.  This is not a constant-value certificate:
  /// consumers may retain/mark the target object unsafe, but may not use the
  /// occurrence for exact symbolization or table-bound authorization.
  bool OutputMayDepend = false;
  /// Stable identity of the operation that was authenticated to define the
  /// complete address.  Addr/Seq alone identify an operation occurrence, but
  /// later LowIR rewrites must not retain the certificate after changing that
  /// operation's output lane or semantic role.
  NdOp OutputOpcode = NdOp::NOP;
  NdVar OutputWitness = {};
  /// Exact input position that consumed the encoded field, or -1 when the
  /// field authenticates the operation output.  Equal constants in two input
  /// positions are permanently ambiguous and retain -1.
  int InputIndex = -1;

  /// LoaderField requires a real FieldVA.  The relocation-free AArch64 kind
  /// deliberately keeps FieldVA invalid and carries the complete semantic
  /// proof below, so consumers that require a loader field cannot silently
  /// reinterpret it as one.
  RelocatedInstructionAddressProofKind Authority =
      RelocatedInstructionAddressProofKind::LoaderField;
  va_t SeedInstructionAddr = InvalidVA;
  int SeedOpSeq = -1;
  NdOp SeedOpcode = NdOp::NOP;
  NdVar SeedInputWitness = {};
  NdVar SeedOutputWitness = {};
  std::vector<RelocatedInstructionAddressArithmeticStep> ArithmeticProof;
  va_t DereferenceInstructionAddr = InvalidVA;
  int DereferenceOpSeq = -1;
  NdOp DereferenceOpcode = NdOp::NOP;
  NdVar DereferenceAddressWitness = {};
  uint16_t DereferenceAccessSize = 0;

  bool operator==(const RelocatedInstructionAddressOccurrence &Other) const =
      default;
};

/// Exact scalar or negative relocation operand consumed by one LowOp input.
/// Unlike an address occurrence, GOTPC is a scalar adjustment and an ambiguous
/// GOTOFF displacement is only a fail-closed tombstone; neither grants address
/// provenance by numeric equality.
struct RelocatedInstructionScalarOperandOccurrence {
  enum class OperandKind : uint8_t {
    I386ELFGOTPC,
    I386ELFAmbiguousGOTOFF,
  };

  va_t FieldVA = InvalidVA;
  va_t InstructionAddr = InvalidVA;
  int OpSeq = -1;
  uint8_t Width = 0;
  uint8_t InputIndex = 0;
  uint64_t EncodedValue = 0;
  OperandKind Kind = OperandKind::I386ELFGOTPC;
  NdOp Opcode = NdOp::NOP;
  NdVar OutputWitness = {};

  bool operator==(
      const RelocatedInstructionScalarOperandOccurrence &Other) const = default;
};

/// Exact ordinary POP LOAD/COPY producer observed for the i386 PIC get-PC
/// idiom `call $+5; pop reg`.  This occurrence is only a candidate seed: the
/// CFG proof must establish that the adjacent call's exact stack push is the
/// POP's sole reaching predecessor before publishing a scalar model.  A
/// role-neutral Address constant with the same numeric value is not equivalent.
struct I386GetPcOccurrence {
  va_t CallInstructionAddr = InvalidVA;
  va_t InstructionAddr = InvalidVA;
  int OpSeq = -1;
  uint32_t PCValue = 0;
  NdOp OutputOpcode = NdOp::NOP;
  NdVar OutputWitness = {};
  /// Exact value copied out of the POP's LOAD.  Copy propagation may make
  /// later PIC arithmetic refer to this SSA value directly, so consumers must
  /// bind it alongside (but never confuse it with) the architectural output.
  NdVar InputWitness = {};
  /// Permission to fold the raw architectural PC on non-ELF i386 after the
  /// final CFG proves that the adjacent CALL's exact stack push is the POP
  /// LOAD/COPY's sole reaching definition.  ELF publishes its stricter paired
  /// GOTPC scalar model instead and deliberately leaves this false.
  bool RawPCAuthenticated = false;

  bool operator==(const I386GetPcOccurrence &Other) const = default;
};

/// Relocation-authenticated scalar address-model result at one exact LowOp.
/// This is deliberately separate from RelocatedInstructionAddressOccurrence:
/// the i386 GOTPC result is the numeric/model-zero GOT base, not an address and
/// not permission to symbolize an equal constant elsewhere.
struct RelocatedInstructionScalarModelOccurrence {
  enum class ModelKind : uint8_t { I386ELFGOTBaseZero };

  va_t FieldVA = InvalidVA;
  va_t InstructionAddr = InvalidVA;
  int OpSeq = -1;
  uint8_t Width = 0;
  ModelKind Model = ModelKind::I386ELFGOTBaseZero;
  NdOp OutputOpcode = NdOp::NOP;
  NdVar OutputWitness = {};
  va_t SeedInstructionAddr = InvalidVA;
  int SeedOpSeq = -1;
  NdOp SeedOpcode = NdOp::NOP;
  NdVar SeedOutputWitness = {};

  bool operator==(
      const RelocatedInstructionScalarModelOccurrence &Other) const = default;
};

/// One physically stored run that participates in a recovered jump-table
/// shape.  Logical cases and targets are intentionally not stored here: a
/// two-level table owns both its compact index run and its address run, while
/// a runtime-selected two-table dispatch can own two disjoint runs.
struct JumpTableStorageRange {
  va_t BaseAddr = 0;
  uint16_t EntrySize = 0;
  uint64_t EntryStride = 0;
  uint64_t PhysicalSlotCount = 0;

  bool operator==(const JumpTableStorageRange &Other) const {
    return BaseAddr == Other.BaseAddr && EntrySize == Other.EntrySize &&
           EntryStride == Other.EntryStride &&
           PhysicalSlotCount == Other.PhysicalSlotCount;
  }

  bool operator<(const JumpTableStorageRange &Other) const {
    return std::tie(BaseAddr, EntrySize, EntryStride, PhysicalSlotCount) <
           std::tie(Other.BaseAddr, Other.EntrySize, Other.EntryStride,
                    Other.PhysicalSlotCount);
  }

  std::optional<uint64_t> storageSize() const {
    if (EntrySize == 0 || EntryStride < EntrySize || PhysicalSlotCount == 0)
      return std::nullopt;
    const uint64_t LastSlot = PhysicalSlotCount - 1;
    if (LastSlot > (InvalidVA - EntrySize) / EntryStride)
      return std::nullopt;
    return LastSlot * EntryStride + EntrySize;
  }

  /// Own only bytes occupied by table entries.  Padding between strided
  /// records is not automatically table storage and may contain an unrelated
  /// relocation-free code label or another record field.
  bool ownsStorageAddress(va_t Addr) const {
    auto Size = storageSize();
    if (!Size || Addr < BaseAddr || *Size > InvalidVA - BaseAddr ||
        Addr >= BaseAddr + *Size)
      return false;
    const uint64_t Offset = Addr - BaseAddr;
    const uint64_t Slot = Offset / EntryStride;
    return Slot < PhysicalSlotCount && Offset % EntryStride < EntrySize;
  }

  std::optional<va_t> storageEnd() const {
    auto Size = storageSize();
    if (!Size || *Size > InvalidVA - BaseAddr)
      return std::nullopt;
    return BaseAddr + *Size;
  }
};

/// Stable identity for one authenticated LowIR operation occurrence.  An
/// instruction address alone is insufficient because a lifted machine
/// instruction commonly expands to several LowOps, and a physical register is
/// not an SSA/value identity.  Jump-table consumers use this witness to
/// distinguish the dispatch LOAD from an independent read of the same table.
struct JumpTableOpOccurrence {
  va_t Addr = InvalidVA;
  int Seq = -1;
  uint16_t Size = 0;

  bool operator==(const JumpTableOpOccurrence &Other) const {
    return Addr == Other.Addr && Seq == Other.Seq && Size == Other.Size;
  }

  bool operator<(const JumpTableOpOccurrence &Other) const {
    return std::tie(Addr, Seq, Size) <
           std::tie(Other.Addr, Other.Seq, Other.Size);
  }
};

/// Stable public reference to the exact LowIR value occurrence used as a
/// recovered switch selector.  The resolver's NdVar is intentionally not
/// exported: a physical register name is not an SSA identity, and consumers
/// must bind this operand only after Low-to-Med rewriting has completed.
struct JumpTableSelectorUseRef {
  enum class ValueRole : uint8_t { Input, Output };

  va_t Addr = InvalidVA;
  int Seq = -1;
  NdOp ExpectedOpcode = NdOp::NOP;
  ValueRole Role = ValueRole::Input;
  uint8_t InputNo = 0;
  uint16_t ExpectedSize = 0;

  bool operator==(const JumpTableSelectorUseRef &Other) const {
    return Addr == Other.Addr && Seq == Other.Seq &&
           ExpectedOpcode == Other.ExpectedOpcode && Role == Other.Role &&
           InputNo == Other.InputNo && ExpectedSize == Other.ExpectedSize;
  }
};

/// Exact public recipe for a runtime-selected two-table switch condition.
/// ByteIndex names the already-scaled dynamic operand of the authenticated
/// table-address ADD.  Condition names the boolean value whose true/false arm
/// selected the physical table base.  Backends concatenate the two physical
/// runs in one selector coordinate as
/// `ByteIndex + (Condition ? TrueOffset : FalseOffset)`.
struct JumpTableCompositeSelectorUseRef {
  enum class Kind : uint8_t { SelectOffset };

  Kind RecipeKind = Kind::SelectOffset;
  JumpTableSelectorUseRef ByteIndex;
  JumpTableSelectorUseRef Condition;
  uint64_t TrueOffset = 0;
  uint64_t FalseOffset = 0;
  uint16_t ResultSize = 0;

  bool operator==(const JumpTableCompositeSelectorUseRef &Other) const {
    return RecipeKind == Other.RecipeKind && ByteIndex == Other.ByteIndex &&
           Condition == Other.Condition && TrueOffset == Other.TrueOffset &&
           FalseOffset == Other.FalseOffset && ResultSize == Other.ResultSize;
  }
};

struct JumpTable {
  va_t InsnAddr = 0;
  va_t BaseAddr = 0;
  /// Distinguishes an absent base from a relocation-proven table mapped at VA
  /// zero in an ELF relocatable object.
  bool HasBaseAddr = false;
  uint16_t EntrySize = 0;
  /// Physical byte distance between adjacent table slots.  This may exceed
  /// EntrySize for padded/strided layouts.
  uint64_t EntryStride = 0;
  /// Physical storage owned by this logical dispatch.  Ordinary tables have
  /// one range; non-adjacent two-table and two-level shapes have two.  Never
  /// infer these ranges from Targets.size(): logical cases and physical slots
  /// are different domains for composite and sparse layouts.
  std::vector<JumpTableStorageRange> StorageRanges;
  /// Exact relocation slots whose stored code-pointer identity is fully
  /// consumed by this recovered dispatch and may therefore be omitted from
  /// independent CFG-root discovery and the LLVM code-pointer mirror.
  /// Physical storage ownership alone is insufficient: a gap/filler slot can
  /// live inside the same table object while another reachable consumer takes
  /// its address or reads it independently.
  std::vector<va_t> SuppressibleRelocationSlots;
  /// Exact LOAD occurrences certified as the recovered dispatch's table
  /// reads.  Module-wide consumer arbitration excludes only these operations;
  /// another LOAD of the same runtime or filler slot is an independent
  /// consumer and vetoes relocation suppression.
  std::vector<JumpTableOpOccurrence> AuthenticatedTableLoads;
  /// Exact LowIR selector occurrences.  These are converted into a Med-only
  /// selector plan after SSA/copy propagation; absence or ambiguity is a hard
  /// failure for shapes that require exact selector ownership.
  std::vector<JumpTableSelectorUseRef> SelectorUseRefs;
  /// Exact composite recipe for a runtime-selected table base.  This is kept
  /// separate from SelectorUseRefs because its selector lives in the final
  /// byte-address coordinate and also depends on a certified
  /// condition/polarity.
  std::optional<JumpTableCompositeSelectorUseRef> CompositeSelectorUseRef;
  int IndexRegOff = -1;
  bool IsRelative = false;
  bool IsSigned = false;

  /// Set when table storage and target anchor differ, including AArch64 compact
  /// tables and PE tables of unsigned RVAs based at the image base.  Targets
  /// are `TargetBase + entry*scale`.  Compact tables dispatch on a table index
  /// distinct from the loaded entry; the separate presence bit is also needed
  /// because relocatable objects may place a valid target anchor at VA zero.
  va_t TargetBase = 0;
  bool HasTargetBase = false;
  /// True only for the linked x64 PE u32-RVA encoding.  HasTargetBase alone
  /// also describes compact target-relative tables on other architectures.
  bool IsPEImageRelativeRVA = false;

  /// Exact machine-instruction address of the LOAD that reads the compact
  /// table entry.  Low-to-Med copy propagation may replace the resolver's
  /// physical index register with a reload temp, so the emitter anchors at
  /// this LOAD and extracts the dynamic address term instead of scanning the
  /// whole block for a same-numbered register or the later loaded entry.
  va_t TableLoadAddr = InvalidVA;

  /// Set for a size-optimized computed goto whose index register already holds
  /// the byte offset (`table + entry*size`, scale folded into the index).  The
  /// address carries no scale multiply, so switch recovery must dispatch on
  /// IndexRegOff rather than the backward scan, which would latch onto an
  /// unrelated multiply (e.g. an LCG step) in the dispatch block.
  bool PreScaledIndex = false;

  /// Runtime-selected table base ("two-table" indirect dispatch): the dispatch
  /// loads from `(cond ? A : B)[idx]` where A and B are two adjacent
  /// code-pointer tables.  The resolver merges them into one table at BaseAddr
  /// = min(A,B) with the combined entry count, and the emitter synthesizes the
  /// switch selector (a byte offset into the merged table) as `idx_bytes + (D
  /// when the higher table is selected)`, turning the runtime base select into
  /// a single switch.
  bool TwoTableSelect = false;

  /// Byte distance between the two tables (entries(lo) * EntrySize); the
  /// emitter adds it to the index byte offset when the higher table is
  /// selected.
  uint32_t TwoTableOffset = 0;

  /// True when the higher table (BaseAddr + TwoTableOffset) is selected by the
  /// positive arm — the clean-SELECT true input, or the mask-blend operand
  /// ANDed with the base mask M (rather than ~M).  Lets the emitter pick the
  /// correct blend mask / select arm without re-folding the table addresses.
  bool TwoTableHiPositive = false;

  /// Two-level (index-byte) table dispatch: a compact byte/halfword index table
  /// maps the switch variable to an entry index that then indexes the real
  /// address table — `target = jmptab[idxtab[switchvar]]` (the classic MSVC
  /// sparse-switch lowering).  Targets are precomputed one per switch value
  /// (positions 0..N) so an ordinary index switch on the real switch variable
  /// (IndexRegOff) reproduces the dispatch; the intermediate table index is not
  /// the switch condition.  The emitter must dispatch on IndexRegOff rather
  /// than tracing the branch target back (which would find the intermediate
  /// index).
  bool TwoLevelIndex = false;

  /// Set for a stack-materialised computed-goto table (a non-`static` label
  /// array clang copies onto the stack) whose entries are *written again* after
  /// the constant initializer copy, with a value that is not the positional
  /// constant entry — i.e. the program permutes/overwrites the table at run
  /// time
  /// (`void *t=tab[0]; tab[0]=tab[3]; tab[3]=t;`).  The recovered static
  /// targets then no longer describe the runtime index->target mapping, so an
  /// index-dispatch switch would silently pick the wrong case.  The emitter
  /// refuses such a table and lowers the INDIR_BR to a loud trap instead of a
  /// silent miscompile (sound resolution would need runtime value dispatch — a
  /// separate, documented gap).  Targets are still populated so the dispatch
  /// keeps its successors (the trap path needs them).
  bool MutatedUnsafe = false;

  std::vector<va_t> Targets;

  /// Physical table slot corresponding to each Targets element.  Keeping this
  /// separate from CaseLabels is essential: source labels may be normalized,
  /// shifted, or strided, while a constant table LOAD must map its physical
  /// byte offset to exactly one kept target.  Dense tables carry {0,1,...}.
  std::vector<uint32_t> SlotIndices;

  /// True only when SlotIndices maps slots of the primary dispatch-table run
  /// to Targets positions.  Composite tables deliberately leave this false:
  /// their logical selector coordinate is not a physical slot coordinate.
  bool HasDispatchSlotMap = false;

  /// Recovered original case label values (one per target).
  /// Empty when recovery is not possible (e.g., relocatable objects).
  std::vector<int64_t> CaseLabels;

  std::optional<uint64_t>
  targetPositionForPhysicalSlot(uint64_t PhysicalSlot) const {
    if (!HasDispatchSlotMap)
      return std::nullopt;
    if (SlotIndices.size() != Targets.size())
      return std::nullopt;
    for (size_t I = 0; I < SlotIndices.size(); ++I)
      if (SlotIndices[I] == PhysicalSlot)
        return static_cast<uint64_t>(I);
    return std::nullopt;
  }

  /// True when \p Addr lies in the table-storage interval proven by this
  /// recovery record.  This remains distinct from executable layout: compilers
  /// may place relative switch entries inline in an instruction section.
  bool ownsStorageAddress(va_t Addr) const {
    return std::any_of(StorageRanges.begin(), StorageRanges.end(),
                       [&](const JumpTableStorageRange &Range) {
                         return Range.ownsStorageAddress(Addr);
                       });
  }

  bool suppressesRelocationSlot(va_t Addr) const {
    return std::binary_search(SuppressibleRelocationSlots.begin(),
                              SuppressibleRelocationSlots.end(), Addr);
  }

  /// End of the sole physical range.  Composite tables intentionally have no
  /// single end; callers that arbitrate ownership must iterate StorageRanges.
  std::optional<va_t> storageEnd() const {
    if (StorageRanges.size() != 1)
      return std::nullopt;
    return StorageRanges.front().storageEnd();
  }
};

/// Exact LowIR RETURN occurrence carrying a recovered Windows C++ continuation
/// value.  The source identity is the pair (ReturnAddr, ReturnSeq), not merely
/// an address: one guest instruction may lower to several LowOps, while later
/// IR rewrites may move the surviving operation to a different block.
struct LowCxxContinuationExitEvidence {
  va_t ReturnAddr = InvalidVA;
  int ReturnSeq = -1;
  /// Sorted, duplicate-free exact targets reaching this RETURN.  More than one
  /// target is still complete may-evidence, but cannot authorize a unique
  /// catch continuation.
  std::vector<va_t> Targets;
  bool Complete = false;

  std::optional<va_t> uniqueTarget() const {
    if (!Complete || Targets.size() != 1)
      return std::nullopt;
    return Targets.front();
  }
};

struct LowFunc {
  va_t Entry = 0;
  uint64_t OriginalSize = 0;
  std::string Name;
  std::string DebugName;
  std::string SourceFile;
  uint32_t SourceLine = 0;
  std::vector<LowBlock> Blocks;
  std::vector<JumpTable> JumpTables;
  /// Final CFG roots which independently seed this function's published
  /// blocks: the real entry/exception roots, unsuppressed relocation roots,
  /// and reachable address-taken code roots.  Module-wide evidence analysis
  /// must start here instead of treating every textual LowBlock as
  /// independently executable; the latter lets unreachable table filler
  /// bootstrap its own relocation preservation.
  std::set<va_t> ModuleAnalysisRoots;
  /// Final roots that can be entered with ordinary machine-state semantics:
  /// the real function entry, unsuppressed relocation/address-taken labels,
  /// and validated continuation points.  Exception-only handlers, cleanups,
  /// and landing pads remain in ModuleAnalysisRoots but not here.  Keeping a
  /// separate positive role set is intentional: one address may have both an
  /// ordinary code-pointer/continuation role and an exceptional-entry role.
  std::set<va_t> OrdinaryModuleAnalysisRoots;
  /// Per-RETURN continuation evidence published atomically only after the
  /// module EH/root and jump-table arbitration fixed points are both stable.
  /// The function-level target union used during root closure is intentionally
  /// not retained here: downstream reconstruction must bind exact exits.
  std::vector<LowCxxContinuationExitEvidence> CxxContinuationExits;
  /// Distinguishes a completed analysis whose exact occurrence set is empty
  /// from a function for which no stable module-level certificate was
  /// published.  False always requires CxxContinuationExits to be empty.
  bool CxxContinuationExitAnalysisComplete = false;
  /// Exact relocation occurrences consumed by final published instructions.
  /// Speculatively decoded/pruned instructions are deliberately absent, as
  /// are fields that merely fall inside an instruction's byte envelope.
  std::vector<RelocatedInstructionAddressOccurrence>
      RelocatedInstructionAddressOccurrences;
  /// Exact call/pop get-PC producers retained from final published LowIR.
  std::vector<I386GetPcOccurrence> I386GetPcOccurrences;
  /// Exact scalar address-model outputs retained from final CFG proof.
  std::vector<RelocatedInstructionScalarModelOccurrence>
      RelocatedInstructionScalarModelOccurrences;
  /// Non-call indirect branches that published at least one validated jump-
  /// table target in this function's monotone build history.  This is distinct
  /// from the final JumpTables list: a later complete proof may remove every
  /// target without changing the guest instruction's branch identity.
  std::set<va_t> EverPublishedJumpTableBranchAddresses;
  /// Indirect branches whose target storage may be mutable or whose module-
  /// wide evidence analysis failed closed.  This identity is deliberately
  /// independent of JumpTables: a later proof gate may reject all table
  /// metadata, but the original branch must still remain an INDIR_BR and trap
  /// rather than being reinterpreted as a function-pointer tail call.
  std::set<va_t> UnsafeIndirectBranchAddresses;
  std::optional<ExceptionFunction> ExceptionMetadata;

  /// Coverage accounting for recursive-descent decode and lift.  These values
  /// describe reachable instruction starts, not a linear sweep of the section.
  uint64_t DecodedInstructionCount = 0;
  uint64_t LiftedInstructionCount = 0;
  std::vector<va_t> DecodeFailureAddresses;
  std::vector<va_t> UnsupportedInstructionAddresses;
  std::vector<va_t> TruncatedPathAddresses;

  /// True when every instruction this function reaches was decoded and lifted.
  ///
  /// Deliberately says nothing about a path that leaves the mapped image: the
  /// bytes such a path names are not in this image, so nothing about the lift
  /// could have produced them.  A function that tail-jumps to an unmapped
  /// address -- an extern call in an unlinked object, a jump into a section
  /// the loader did not map, or padding a scan wandered into -- is lifted
  /// exactly as completely as one that returns.
  bool hasCompleteInstructionLift() const {
    return DecodedInstructionCount == LiftedInstructionCount &&
           DecodeFailureAddresses.empty() &&
           UnsupportedInstructionAddresses.empty();
  }

  /// True when the lift is complete *and* every path stayed inside the image,
  /// so the function is wholly described by what was recovered.  This is the
  /// question a coverage report asks; it is not grounds for discarding a
  /// function, which \ref hasCompleteInstructionLift settles.
  bool hasCompleteLiftCoverage() const {
    return hasCompleteInstructionLift() && TruncatedPathAddresses.empty();
  }

  /// Bytes this function pops off the caller's stack on return beyond the
  /// return address (x86 `ret imm`, the i386 SysV callee-cleanup convention
  /// used for the hidden struct-return (sret) pointer).  0 for an ordinary
  /// `ret`.  A caller of such a function must add this to its post-call stack
  /// pointer (the callee popped the argument), recovered by LowToMed via the
  /// per-callee map.
  int CalleePopBytes = 0;

  /// Executable targets this function takes the address of via a
  /// relocation-free PC-relative `lea` (a same-section function pointer).
  /// Merged into the image so the emitter symbolizes the matching constant to
  /// `ptrtoint @func`.
  std::vector<va_t> CodeRefTargets;

  LowBlock *blockFor(va_t Addr) {
    for (auto &B : Blocks)
      if (Addr >= B.StartAddr && Addr < B.EndAddr)
        return &B;
    return nullptr;
  }

  uint64_t computedSize() const {
    if (Blocks.empty())
      return 0;
    va_t MaxEnd = 0;
    for (const auto &B : Blocks)
      if (B.EndAddr > MaxEnd)
        MaxEnd = B.EndAddr;
    return MaxEnd > Entry ? MaxEnd - Entry : 0;
  }
};

/// Optional validation keeps manually constructed legacy LowIR valid when it
/// carries no instruction metadata.  Once any boundary is present, validation
/// is strict; Required additionally rejects wholly missing or partially
/// populated metadata.
enum class LowInstructionBoundaryRequirement : uint8_t {
  Optional,
  Required,
};

llvm::Error validateLowInstructionBoundaries(
    const LowBlock &Block, LowInstructionBoundaryRequirement Requirement =
                               LowInstructionBoundaryRequirement::Optional);

llvm::Error validateLowInstructionBoundaries(
    const LowFunc &Function, LowInstructionBoundaryRequirement Requirement =
                                 LowInstructionBoundaryRequirement::Optional);

} // namespace neverd

#endif // NEVERD_IR_LOW_LOWIR_H
