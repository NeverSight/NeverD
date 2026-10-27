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
#include "llvm/ADT/FoldingSet.h"
#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/ADT/SmallVector.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <utility>
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

  /// Include every retained byte and its address in a contextual-summary key.
  void profile(llvm::FoldingSetNodeID &ID) const;

  bool operator==(const MemoryModel &Other) const;
  bool operator!=(const MemoryModel &Other) const { return !(*this == Other); }

private:
  /// Disjoint, non-adjacent runs of proven bytes, ordered by address. Runs
  /// rather than single bytes so a reader can hand out a contiguous view.
  std::map<va_t, std::vector<uint8_t>> Runs;
  uint64_t TrackedBytes = 0;
};

/// Inclusive unsigned interval. Scalar/capture bounds do not wrap; may-write
/// intervals can wrap (Lower > Upper) to preserve modular address arithmetic.
struct UnsignedValueRange {
  uint64_t Lower = 0;
  uint64_t Upper = UINT64_MAX;
  bool operator==(const UnsignedValueRange &) const = default;
};

/// Bounds of immutable, analysis-local captured values. Distinct entries may
/// alias: capture identity proves equality only, never allocation freshness.
using SymbolicAddressSpace = std::vector<UnsignedValueRange>;

RegisterValue unsignedRangeValue(UnsignedValueRange Range);
std::optional<UnsignedValueRange>
unsignedValueRange(const RegisterValue &Value,
                   const SymbolicAddressSpace *Addresses = nullptr);
/// Includes wrapped address intervals; unlike unsignedValueRange it does not
/// widen an affine address merely because the displacement can wrap.
std::optional<UnsignedValueRange>
addressValueRange(const RegisterValue &Value,
                  const SymbolicAddressSpace *Addresses = nullptr);
/// Normalize an exact/affine address into a typed memory key. Unknown ranges
/// have no identity and cannot name a cell.
std::optional<RegisterValue> memoryAddress(const RegisterValue &Base,
                                           int64_t Displacement = 0);

/// Full-width stored values, retaining address provenance independently of the
/// descriptor byte view. In particular, a frame-relative pointer is never a
/// literal integer merely because its analysis coordinates were serialized.
enum class MemoryCoordinate : uint8_t { Absolute, FrameRelative };

/// A complete finite disjunction used by the optional contextual analysis.
/// Empty denotes unknown, not an unreachable state. Ordinary must-analysis
/// still reads only singleton values and uses the existing equality meet.
class ProvenValueSet {
public:
  ProvenValueSet() = default;
  ProvenValueSet(const RegisterValue &Value);
  RegisterValue singleton() const;
  llvm::ArrayRef<RegisterValue> values() const { return Values; }
  bool unknown() const { return Values.empty(); }
  void join(const ProvenValueSet &Other);
  void profile(llvm::FoldingSetNodeID &ID) const;
  uint64_t allocatedBytes() const;
  bool operator==(const ProvenValueSet &) const = default;

private:
  llvm::SmallVector<RegisterValue, 1> Values;
};

/// Immutable coordinate configuration shared by every state of one analysis.
struct StoredMemoryCoordinates {
  std::optional<uint64_t> FrameTranslationLimit;
  std::shared_ptr<const SymbolicAddressSpace> Addresses;
};

class StoredRegisterMemory {
public:
  void setCoordinates(std::shared_ptr<const StoredMemoryCoordinates> Context) {
    Coordinates = std::move(Context);
  }
  void setSymbolicAddresses(std::shared_ptr<const SymbolicAddressSpace> Space);
  const std::shared_ptr<const SymbolicAddressSpace> &symbolicAddresses() const;
  std::optional<uint64_t> frameTranslationBound() const;
  void writeValues(const RegisterValue &Address, ProvenValueSet Value);
  const ProvenValueSet &readValues(const RegisterValue &Address) const;
  void invalidate(const RegisterValue &Address, uint64_t Size);
  void copyFrom(const StoredRegisterMemory &Source,
                const RegisterValue &SourceAddress,
                const RegisterValue &Destination, uint64_t Size);
  /// Rebase captured values at a call boundary, or widen them to ranges at a
  /// loop boundary. Dropping an address identity also drops its keyed cells.
  void transformSymbols(llvm::function_ref<RegisterValue(RegisterValue)> Map);
  void visitCells(
      llvm::function_ref<void(const RegisterValue &, const ProvenValueSet &)>
          Visit) const;
  /// Overlay simultaneous must-facts after the caller has applied may-writes.
  void includeCells(const StoredRegisterMemory &Other,
                    llvm::function_ref<bool(const RegisterValue &)> Include);
  /// Frame coordinates denote the same addresses translated by an unknown
  /// delta in [0, Limit]. Only an established automatic-frame invariant may
  /// supply this limit; unset coordinates retain conservative aliasing.
  void setFrameTranslationLimit(std::optional<uint64_t> Limit);
  /// The store/copy owner must first validate the complete scratch range.
  void write(va_t Address, const RegisterValue &Value,
             MemoryCoordinate Coordinate = MemoryCoordinate::Absolute);
  void writeValues(va_t Address, ProvenValueSet Value,
                   MemoryCoordinate Coordinate = MemoryCoordinate::Absolute);
  /// A write in either coordinate system may alias any value in the other:
  /// the current function's absolute call depth is not known. A validated
  /// translation bound can exclude overlap, but cannot equate a frame with
  /// one absolute address. Unbounded coordinates invalidate conservatively.
  void invalidate(va_t Address, uint64_t Size,
                  MemoryCoordinate Coordinate = MemoryCoordinate::Absolute);
  void invalidateFrom(va_t Address,
                      MemoryCoordinate Coordinate = MemoryCoordinate::Absolute);
  /// Keep only absolute values independent of a symbolic frame anchor.
  void discardFrameCoordinates();
  void clear() { Values.clear(); }
  bool empty() const { return Values.empty(); }
  uint64_t retainedByteEstimate() const;
  RegisterValue
  read(va_t Address,
       MemoryCoordinate Coordinate = MemoryCoordinate::Absolute) const;
  const ProvenValueSet &
  readValues(va_t Address,
             MemoryCoordinate Coordinate = MemoryCoordinate::Absolute) const;
  void meet(const StoredRegisterMemory &Other);
  /// Contextual union of complete alternatives at cells present on both paths.
  void joinValues(const StoredRegisterMemory &Other);
  void profile(llvm::FoldingSetNodeID &ID) const;
  size_t size() const { return Values.size(); }
  void
  copyFrom(const StoredRegisterMemory &Source, va_t SourceAddress,
           va_t Destination, uint64_t Size,
           MemoryCoordinate SourceCoordinate = MemoryCoordinate::Absolute,
           MemoryCoordinate DestinationCoordinate = MemoryCoordinate::Absolute);
  bool operator==(const StoredRegisterMemory &Other) const;

private:
  // Coordinate 0 is absolute, 1 is frame-relative, and N+2 denotes capture N.
  // The second word is a numeric coordinate or a signed capture displacement.
  std::map<std::pair<uint64_t, uint64_t>, ProvenValueSet> Values;
  std::shared_ptr<const StoredMemoryCoordinates> Coordinates;
};

/// What the register fixed point does not carry: the scratch bytes a program
/// has written.
struct ScratchState {
  MemoryModel Memory;
  StoredRegisterMemory Values;

  bool empty() const { return Memory.empty() && Values.empty(); }
  uint64_t retainedByteEstimate() const;
  void clear();
  void invalidate(va_t Address, uint64_t Size);
  void invalidateFrom(va_t Address);
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

/// Number of leading registers whose values a call can replace. A returning
/// runtime syscall replaces only r0; its five arguments are passed by value.
/// Calls that can execute guest code retain the conservative r0-r5 clobber.
constexpr unsigned callClobberedRegisterCount(CallKind Kind) {
  if (Kind == CallKind::None)
    return 0;
  return Kind == CallKind::Syscall ? kFirstArgumentRegister
                                   : kFirstCalleeSavedRegister;
}

/// Apply one instruction's effect to \p State.
///
/// This is the only transfer function of the SBF value lattice. The block fixed
/// point in the analyzer and any per-instruction replay both call it, so a
/// recovered value can never contradict the analyzer's block summary.
/// Image is the immutable, relocated VM image. Without an explicit scratch
/// context, only data-visible image loads can produce constants. A contextual
/// analysis can additionally supply proven full-width stored register values;
/// pointer provenance is retained and descriptor bytes never authorize a fold.
void applyRegisterTransfer(const MedInstruction &Instruction,
                           RegisterState &State, const ProgramImage &Image,
                           const StoredRegisterMemory *Scratch = nullptr);

enum class ScratchLoadPolicy : uint8_t { Ignore, ProvenValues };

/// Apply one instruction's effect to registers and to scratch memory.
///
/// This is \c applyRegisterTransfer plus the memory effects the register
/// lattice alone cannot express, so the register half of the result is the
/// same value the analyzer's fixed point computes with the default Ignore
/// policy. ProvenValues is for a distinct, caller-seeded context, not replay of
/// ordinary block summaries. \p Image supplies the bytes
/// a copy can be following, which is how a payload copied out of read-only
/// data becomes readable at the invocation that sends it.
void applyTransfer(const MedInstruction &Instruction, MachineState &State,
                   const ProgramImage &Image,
                   ScratchLoadPolicy Loads = ScratchLoadPolicy::Ignore);

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

  ScratchFlow(const SBFProgram &Program, const MedInstructionIndex &Index,
              ScratchLoadPolicy Loads = ScratchLoadPolicy::Ignore);

  const ScratchState &entryState(size_t BlockID) const;
  /// Joint register/scratch must-facts when ProvenValues was requested. A
  /// widened or unreached state returns nullptr; callers then use the ordinary
  /// block inputs and replay local stores rather than partial solver output.
  const RegisterState *entryRegisters(size_t BlockID) const;
  const Statistics &statistics() const { return Stats; }

private:
  /// Sparse, shared immutable states. Empty/basic blocks reuse the same state
  /// object, so the storage scales with distinct scratch mutations rather than
  /// with the program's block count.
  std::vector<std::shared_ptr<const ScratchState>> Entry;
  std::vector<RegisterState> RegisterEntries;
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
