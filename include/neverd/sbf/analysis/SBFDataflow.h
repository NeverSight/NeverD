//===- SBFDataflow.h - Solana SBF register and scratch lattice --*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Defines the abstract machine state shared by the block-level fixed point and
/// by every consumer that needs the value reaching one specific instruction.
/// Both go through the same transfer function so they cannot disagree about
/// what a register or a scratch byte holds.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SBF_ANALYSIS_SBFDATAFLOW_H
#define NEVERD_SBF_ANALYSIS_SBFDATAFLOW_H

#include "neverd/sbf/SBFIR.h"
#include "neverd/sbf/analysis/SBFAnalysisLimits.h"
#include "neverd/sbf/image/SBFProgramImage.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/STLFunctionalExtras.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <vector>

namespace neverd::sbf {

/// Abstract value of every SBF register at one program point.
using RegisterState = std::array<RegisterValue, kRegisterCount>;

/// The address a base register and a displacement designate, when the base is
/// known well enough to name one.
std::optional<va_t> effectiveAddress(const RegisterValue &Base,
                                     int64_t Displacement);

/// Whether \p Address falls in memory only this program can write: its call
/// frames and its heap. The read-only image is not scratch because nothing
/// writes it, and the serialized input is not scratch because the runtime and
/// every invoked program share it.
bool isScratchAddress(va_t Address);

/// Whether the full half-open range `[Address, Address + Size)` stays inside
/// one scratch VM region. This is the checked authority for facts created by
/// multi-byte stores and memory-transfer syscalls; a range crossing from one
/// VM region to the next faults at runtime and proves no bytes.
bool isScratchRange(va_t Address, uint64_t Size);

/// Scratch bytes whose value is proven at one program point.
///
/// This exists because a program assembles structured arguments in memory and
/// then hands the runtime a pointer to them: the seeds of a program-derived
/// address, the serialized instruction of a cross-program invocation, and that
/// instruction's payload are each written as a run of stores or copied into
/// place, and none of them is ever in a register at the call.
///
/// The model holds bytes rather than words, so a payload assembled one field
/// at a time reads back as the buffer the runtime will see.
class MemoryModel {
public:
  /// Record that \p Bytes are the contents of the scratch memory at
  /// \p Address. Nothing is recorded past \c kMaxModeledScratchBytes.
  void write(va_t Address, llvm::ArrayRef<uint8_t> Bytes);

  /// Forget \p Size bytes at \p Address.
  void invalidate(va_t Address, uint64_t Size);

  /// Forget \p Address and everything above it, which is what a write of
  /// unproven length starting there can reach.
  void invalidateFrom(va_t Address);

  void clear();
  bool empty() const { return Runs.empty(); }
  uint64_t trackedBytes() const { return TrackedBytes; }

  /// Conservative bytes charged for retaining this logical snapshot. The
  /// estimate includes byte buffers, ordered-map nodes, and owning objects;
  /// shared snapshots are deliberately charged once per retaining slot.
  uint64_t retainedByteEstimate() const;

  /// The proven bytes at \p Address, empty unless every one of them is proven.
  llvm::ArrayRef<uint8_t> read(va_t Address, uint64_t Size) const;

  /// The proven machine word at \p Address.
  std::optional<uint64_t> readWord(va_t Address) const;

  /// Keep only the bytes on which this model and \p Other agree, which is what
  /// a program point reachable by two paths can claim.
  void meet(const MemoryModel &Other);

  bool operator==(const MemoryModel &Other) const;
  bool operator!=(const MemoryModel &Other) const { return !(*this == Other); }

private:
  /// Disjoint, non-adjacent runs of proven bytes, ordered by address. Runs
  /// rather than single bytes so a reader can hand out a contiguous view.
  std::map<va_t, std::vector<uint8_t>> Runs;
  uint64_t TrackedBytes = 0;
};

/// What the register fixed point does not carry: the scratch bytes a program
/// has written.
struct ScratchState {
  MemoryModel Memory;

  void meet(const ScratchState &Other);
  bool operator==(const ScratchState &Other) const;
  bool operator!=(const ScratchState &Other) const { return !(*this == Other); }
};

/// Registers together with the scratch memory they address.
struct MachineState {
  RegisterState Registers{};
  ScratchState Scratch;

  const RegisterValue &operator[](size_t Register) const {
    return Registers[Register];
  }
};

/// Slot-indexed view of the MedIR instruction stream. Blocks address their
/// instructions by slot, but the stream is stored flat and skips LDDW
/// continuations, so consumers need this mapping to walk a block.
class MedInstructionIndex {
public:
  explicit MedInstructionIndex(const MedIR &IR);

  const MedInstruction *find(size_t Slot) const;

private:
  std::vector<const MedInstruction *> BySlot;
};

/// Apply one instruction's effect to \p State.
///
/// This is the only transfer function of the SBF value lattice. The block fixed
/// point in the analyzer and any per-instruction replay both call it, so a
/// recovered value can never contradict the analyzer's block summary.
void applyRegisterTransfer(const MedInstruction &Instruction,
                           RegisterState &State);

/// Apply one instruction's effect to registers and to scratch memory.
///
/// This is \c applyRegisterTransfer plus the memory effects the register
/// lattice alone cannot express, so the register half of the result is the
/// same value the analyzer's fixed point computes. \p Image supplies the bytes
/// a copy can be following, which is how a payload copied out of read-only
/// data becomes readable at the invocation that sends it.
void applyTransfer(const MedInstruction &Instruction, MachineState &State,
                   const ProgramImage &Image);

/// Whether Solana recovery reads scratch bytes at this runtime call. CPI and
/// PDA decoding dereference program-built descriptors; other recovery
/// visitors consume registers or the immutable ProgramImage only.
bool consumesScratchFacts(const MedInstruction &Instruction);

enum class ScratchFlowPrecision : uint8_t { Exact, WidenedToUnknown };

/// Entry scratch state of every block.
///
/// A program does not have to finish assembling an argument in the block that
/// passes it, so this is a forward must-analysis over the CFG: a byte survives
/// into a block only when every path that reaches it wrote the same value.
/// Call edges are deliberately not followed, because a callee runs in its own
/// frame and inherits nothing from its caller's.
class ScratchFlow {
public:
  struct Statistics {
    ScratchFlowPrecision Precision = ScratchFlowPrecision::Exact;
    uint64_t RetainedByteEstimate = 0;
    uint64_t PeakRetainedByteEstimate = 0;
    size_t IndexedBlockCount = 0;
    size_t IndexedEdgeCount = 0;
    size_t SuccessorIndexEntryCount = 0;
  };

  ScratchFlow(const SBFProgram &Program, const MedInstructionIndex &Index);

  const ScratchState &entryState(size_t BlockID) const;
  const Statistics &statistics() const { return Stats; }

private:
  /// Sparse, shared immutable states. Empty/basic blocks reuse the same state
  /// object, so the storage scales with distinct scratch mutations rather than
  /// with the program's block count.
  std::vector<std::shared_ptr<const ScratchState>> Entry;
  std::shared_ptr<const ScratchState> Empty =
      std::make_shared<const ScratchState>();
  ScratchState Unreached;
  Statistics Stats;
};

/// Replay \p Block from \p Entry and the block's recorded register inputs,
/// reporting the state that reaches each instruction before it executes.
///
/// Once the analyzer's fixed point has converged, the register state left after
/// the last instruction equals `Block.Outputs`.
void replayBlock(
    const MedInstructionIndex &Index, const MedBlock &Block,
    const ScratchState &Entry, const ProgramImage &Image,
    llvm::function_ref<void(const MedInstruction &, const MachineState &)>
        Visit);

} // namespace neverd::sbf

#endif // NEVERD_SBF_ANALYSIS_SBFDATAFLOW_H
