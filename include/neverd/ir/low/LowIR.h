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

struct NdVar {
  VnodeSpace Space = VnodeSpace::CONST;
  uint64_t Offset = 0;
  uint16_t Size = 0;

  bool isConst() const { return Space == VnodeSpace::CONST; }
  bool isReg() const { return Space == VnodeSpace::REG; }
  bool isTemp() const { return Space == VnodeSpace::TEMP; }
  bool isRam() const { return Space == VnodeSpace::RAM; }

  bool operator==(const NdVar &O) const {
    return Space == O.Space && Offset == O.Offset && Size == O.Size;
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
  static NdVar ram(uint64_t Addr, uint16_t Sz) {
    return {VnodeSpace::RAM, Addr, Sz};
  }
};

struct LowOp {
  NdOp Opcode = NdOp::NOP;
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

struct LowBlock {
  int Id = -1;
  va_t StartAddr = 0;
  va_t EndAddr = 0;
  std::vector<LowOp> Ops;
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
};

struct JumpTable {
  va_t InsnAddr = 0;
  va_t BaseAddr = 0;
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

  bool hasCompleteLiftCoverage() const {
    return DecodedInstructionCount == LiftedInstructionCount &&
           DecodeFailureAddresses.empty() &&
           UnsupportedInstructionAddresses.empty() &&
           TruncatedPathAddresses.empty();
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

} // namespace neverd

#endif // NEVERD_IR_LOW_LOWIR_H
