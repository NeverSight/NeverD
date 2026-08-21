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

#include <limits>
#include <optional>
#include <string>
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

struct JumpTable {
  va_t InsnAddr = 0;
  va_t BaseAddr = 0;
  /// Distinguishes an absent base from a relocation-proven table mapped at VA
  /// zero in an ELF relocatable object.
  bool HasBaseAddr = false;
  uint16_t EntrySize = 0;
  int IndexRegOff = -1;
  bool IsRelative = false;
  bool IsSigned = false;

  /// Non-zero for the AArch64 compact byte/halfword table form, where targets
  /// are `TargetBase + entry*scale` and the switch dispatches on a table index
  /// distinct from the loaded entry — switch recovery must use IndexRegOff
  /// rather than the blind backward scan.
  va_t TargetBase = 0;

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

  /// Recovered original case label values (one per target).
  /// Empty when recovery is not possible (e.g., relocatable objects).
  std::vector<int64_t> CaseLabels;

  /// True when \p Addr lies in the table-storage interval proven by this
  /// recovery record.  This remains distinct from executable layout: compilers
  /// may place relative switch entries inline in an instruction section.
  bool ownsStorageAddress(va_t Addr) const {
    if (!HasBaseAddr || Addr < BaseAddr)
      return false;
    if (Addr == BaseAddr)
      return true;
    if (EntrySize == 0 || Targets.empty())
      return false;
    const uint64_t Count = static_cast<uint64_t>(Targets.size());
    return Count <= (InvalidVA - BaseAddr) / EntrySize &&
           Addr < BaseAddr + Count * EntrySize;
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
