//===- PipelineLowIR.cpp - LowIR pipeline stage --------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// LowIR construction for the decompilation pipeline.
///
//===----------------------------------------------------------------------===//

#include "PipelineCallAbiDetail.h"
#include "PipelineLowIRDetail.h"
#include "PipelineSEHRuntimeMemory.h"
#include "PipelineTrimStorage.h"

#include "neverd/Limits.h"
#include "neverd/debug/DebugContext.h"
#include "neverd/debug/UserPrototype.h"
#include "neverd/decode/Decoder.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/ir/low/CallRegisterEffects.h"
#include "neverd/ir/low/GuardedZeroSelect.h"
#include "neverd/ir/low/ImportCallee.h"
#include "neverd/ir/low/InternalNoReturn.h"
#include "neverd/ir/low/LowIntrinsicEffects.h"
#include "neverd/ir/low/LowScalarConstant.h"
#include "neverd/ir/low/RegistrationABI.h"
#include "neverd/ir/low/SEHFrameProof.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/ir/med/MedCallConvention.h"
#include "neverd/ir/med/MedCallTarget.h"
#include "neverd/libc/LibCFixedIntegerArguments.h"
#include "neverd/libc/LibCFloatingArguments.h"
#include "neverd/libc/LibCNames.h"
#include "neverd/libc/LibCRecordArguments.h"
#include "neverd/libc/LibCVariadicArguments.h"
#include "neverd/libc/StackProbeIndex.h"
#include "neverd/lift/X86/X86Regs.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/COFF/RuntimeProviders.h"
#include "neverd/loader/DataSymbolBinding.h"
#include "neverd/loader/ExecutableCodeOwnerIndex.h"
#include "neverd/loader/FunctionNames.h"
#include "neverd/loader/PointerRelocation.h"
#include "neverd/loader/SymbolDecoration.h"
#include "neverd/pipeline/Limits.h"
#include "neverd/pipeline/Pipeline.h"
#include "neverd/support/BinaryEncoding.h"
#include "neverd/support/Parallel.h"

#include "llvm/ADT/APInt.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/iterator_range.h"
#include "llvm/Support/MathExtras.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <iterator>
#include <limits>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace neverd {

namespace pipeline_detail {
namespace {

bool storageRangesMayOverlap(const JumpTableStorageRange &Left,
                             const JumpTableStorageRange &Right,
                             size_t &WorkRemaining) {
  if (WorkRemaining == 0)
    return true;
  --WorkRemaining;
  const std::optional<va_t> LeftEnd = Left.storageEnd();
  const std::optional<va_t> RightEnd = Right.storageEnd();
  if (!LeftEnd || !RightEnd)
    return true;
  if (*LeftEnd <= Right.BaseAddr || *RightEnd <= Left.BaseAddr)
    return false;
  if (Left.PhysicalSlotCount > limits::kMaxJumpTableEntries ||
      Right.PhysicalSlotCount > limits::kMaxJumpTableEntries)
    return true;

  const JumpTableStorageRange *Probe = &Left;
  const JumpTableStorageRange *Other = &Right;
  va_t OtherEnd = *RightEnd;
  if (Right.PhysicalSlotCount < Left.PhysicalSlotCount) {
    Probe = &Right;
    Other = &Left;
    OtherEnd = *LeftEnd;
  }
  for (uint64_t I = 0; I < Probe->PhysicalSlotCount; ++I) {
    if (WorkRemaining == 0)
      return true;
    --WorkRemaining;
    const va_t ProbeStart = Probe->BaseAddr + I * Probe->EntryStride;
    const va_t ProbeEnd = ProbeStart + Probe->EntrySize;
    if (ProbeEnd <= Other->BaseAddr || ProbeStart >= OtherEnd)
      continue;
    const uint64_t LastStartBeforeProbeEnd = std::min<uint64_t>(
        (ProbeEnd - 1 - Other->BaseAddr) / Other->EntryStride,
        Other->PhysicalSlotCount - 1);
    const va_t OtherStart =
        Other->BaseAddr + LastStartBeforeProbeEnd * Other->EntryStride;
    if (OtherStart + Other->EntrySize > ProbeStart)
      return true;
  }
  return false;
}

} // namespace

bool tableObjectSummaryMayAlias(
    const BinaryImage & /*Img*/, va_t RootIdentity,
    llvm::ArrayRef<JumpTableStorageRange> RootRanges, va_t OwnerIdentity,
    llvm::ArrayRef<JumpTableStorageRange> OwnerRanges, size_t *WorkRemaining) {
  if (RootIdentity == InvalidVA)
    return false;
  if (RootIdentity == OwnerIdentity)
    return true;
  size_t LocalWorkRemaining = limits::kMaxJumpTableEvidenceWork;
  size_t &Remaining = WorkRemaining ? *WorkRemaining : LocalWorkRemaining;
  for (const JumpTableStorageRange &RootRange : RootRanges)
    for (const JumpTableStorageRange &OwnerRange : OwnerRanges)
      if (storageRangesMayOverlap(RootRange, OwnerRange, Remaining))
        return true;
  return false;
}

} // namespace pipeline_detail

namespace {

bool hasRealOps(const LowFunc &Func) {
  for (auto &Blk : Func.Blocks)
    for (auto &Op : Blk.Ops)
      if (Op.Opcode != NdOp::NOP)
        return true;
  return false;
}

void annotateDebugInfo(LowFunc &Func, DebugContext &Dbg) {
  // `functionName` is publics-only. `resolveFunction` walks S_LOCAL / TPI
  // and is the `--func` LowIR cost of a moderately sized helper; HighC
  // still calls it when it needs params or frame slots.
  if (auto Name = Dbg.functionName(Func.Entry))
    Func.DebugName = *Name;
}

struct ModuleJumpTableOwner {
  size_t FuncIndex = 0;
  va_t BranchAddr = InvalidVA;
  va_t StorageIdentityVA = InvalidVA;
  std::vector<JumpTableStorageRange> StorageRanges;
  std::vector<JumpTableStorageRange> PhysicalIdentityRanges;
  std::set<va_t> RequestedSlots;
};

enum class ModuleAddressOwnerKind : uint8_t {
  Unknown,
  Container,
  TableObject,
};

struct ModuleAddressAnchor {
  va_t Address = InvalidVA;
  va_t OwnerVA = InvalidVA;
  ModuleAddressOwnerKind OwnerKind = ModuleAddressOwnerKind::Unknown;

  bool operator==(const ModuleAddressAnchor &Other) const = default;

  bool operator<(const ModuleAddressAnchor &Other) const {
    return std::tie(Address, OwnerVA, OwnerKind) <
           std::tie(Other.Address, Other.OwnerVA, Other.OwnerKind);
  }
};

struct ModuleAddressRootScope {
  /// TableObject is a durable logical-object may-dependency learned from an
  /// exact in-envelope address.  Later concrete values may leave that numeric
  /// envelope, but widening must retain the object identity rather than use a
  /// representative address for alias decisions.  Container and Unknown are
  /// progressively wider wildcard scopes.
  va_t OwnerVA = InvalidVA;
  ModuleAddressOwnerKind OwnerKind = ModuleAddressOwnerKind::Unknown;

  bool operator==(const ModuleAddressRootScope &Other) const = default;

  bool operator<(const ModuleAddressRootScope &Other) const {
    return std::tie(OwnerVA, OwnerKind) <
           std::tie(Other.OwnerVA, Other.OwnerKind);
  }
};

va_t storageRangeOwnerVA(const BinaryImage &Img,
                         const JumpTableStorageRange &Range) {
  if (const Section *Sec = Img.getSectionFor(Range.BaseAddr))
    return Sec->VA;
  if (const Segment *Seg = Img.getSegmentFor(Range.BaseAddr))
    return Seg->VA;
  return InvalidVA;
}

bool addressOwnerMatches(const BinaryImage &Img,
                         const ModuleJumpTableOwner &Owner,
                         const JumpTableStorageRange &Range,
                         const ModuleAddressAnchor &Anchor) {
  switch (Anchor.OwnerKind) {
  case ModuleAddressOwnerKind::Unknown:
    return true;
  case ModuleAddressOwnerKind::Container:
    return Anchor.OwnerVA != InvalidVA &&
           Anchor.OwnerVA == storageRangeOwnerVA(Img, Range);
  case ModuleAddressOwnerKind::TableObject:
    return Anchor.OwnerVA != InvalidVA &&
           Anchor.OwnerVA == Owner.StorageIdentityVA;
  }
  return false;
}

bool isExactRelativeTableAnchor(
    const BinaryImage &Img,
    const std::set<va_t> *OccurrenceRelativeTableAnchors, va_t Address) {
  return Img.RelCodeTableAnchors.count(Address) != 0 ||
         (OccurrenceRelativeTableAnchors &&
          OccurrenceRelativeTableAnchors->count(Address) != 0);
}

bool storageEnvelopeContainsOrOnePast(
    const BinaryImage &Img, const ModuleJumpTableOwner &Owner,
    const ModuleAddressAnchor &Anchor,
    const std::set<va_t> *OccurrenceRelativeTableAnchors) {
  return std::any_of(
      Owner.PhysicalIdentityRanges.begin(), Owner.PhysicalIdentityRanges.end(),
      [&](const JumpTableStorageRange &Range) {
        if (!addressOwnerMatches(Img, Owner, Range, Anchor))
          return false;
        const std::optional<va_t> End = Range.storageEnd();
        if (!End || Anchor.Address < Range.BaseAddr || Anchor.Address > *End)
          return false;
        // A loader-authenticated relative-table anchor at the
        // exact end starts a new physical object.  Prefer that
        // identity over treating the address as one-past this
        // owner; ordinary unclaimed one-past pointers retain
        // their conservative object association.
        return Anchor.Address != *End ||
               !isExactRelativeTableAnchor(Img, OccurrenceRelativeTableAnchors,
                                           Anchor.Address);
      });
}

bool moduleAddressSummaryMayAliasOwner(
    const BinaryImage &Img,
    const std::map<va_t, std::set<va_t>> &TableObjectAliases,
    const ModuleJumpTableOwner &Owner, const ModuleAddressRootScope &Root) {
  switch (Root.OwnerKind) {
  case ModuleAddressOwnerKind::Unknown:
    return true;
  case ModuleAddressOwnerKind::Container:
    return Root.OwnerVA != InvalidVA &&
           std::any_of(Owner.PhysicalIdentityRanges.begin(),
                       Owner.PhysicalIdentityRanges.end(),
                       [&](const JumpTableStorageRange &Range) {
                         return storageRangeOwnerVA(Img, Range) == Root.OwnerVA;
                       });
  case ModuleAddressOwnerKind::TableObject:
    if (Root.OwnerVA == InvalidVA)
      return false;
    if (Root.OwnerVA == Owner.StorageIdentityVA)
      return true;
    const auto It = TableObjectAliases.find(Root.OwnerVA);
    return It != TableObjectAliases.end() &&
           It->second.count(Owner.StorageIdentityVA) != 0;
  }
  return false;
}

struct StorageAccessIntersection {
  bool Complete = true;
  bool Any = false;
  std::set<va_t> RequestedSlots;
};

StorageAccessIntersection storageAccessIntersection(
    const BinaryImage &Img, const ModuleJumpTableOwner &Owner,
    const ModuleAddressAnchor &Anchor, uint16_t AccessSize) {
  StorageAccessIntersection Result;
  const va_t Address = Anchor.Address;
  if (AccessSize == 0 || AccessSize > InvalidVA - Address) {
    Result.Complete = false;
    return Result;
  }
  const va_t AccessEnd = Address + AccessSize;
  size_t Work = 0;
  for (const JumpTableStorageRange &Range : Owner.PhysicalIdentityRanges) {
    if (!addressOwnerMatches(Img, Owner, Range, Anchor))
      continue;
    if (Range.EntrySize == 0 || Range.EntryStride < Range.EntrySize ||
        Range.PhysicalSlotCount == 0 ||
        Range.PhysicalSlotCount > limits::kMaxJumpTableEntries - Work) {
      Result.Complete = false;
      return Result;
    }
    Work += Range.PhysicalSlotCount;
    const std::optional<va_t> RangeEnd = Range.storageEnd();
    if (!RangeEnd) {
      Result.Complete = false;
      return Result;
    }
    if (AccessEnd <= Range.BaseAddr || Address >= *RangeEnd)
      continue;

    // Solve `[A,A+N) intersects [B+i*S,B+i*S+E)` without enumerating slots.
    // First is the lowest slot whose occupied bytes end after Address; Last is
    // the highest slot whose occupied bytes start before AccessEnd.
    uint64_t First = 0;
    if (Address >= Range.BaseAddr &&
        Address - Range.BaseAddr >= Range.EntrySize)
      First =
          (Address - Range.BaseAddr - Range.EntrySize) / Range.EntryStride + 1;
    const uint64_t Last = (AccessEnd - 1 - Range.BaseAddr) / Range.EntryStride;
    if (First > Last || First >= Range.PhysicalSlotCount)
      continue;
    const uint64_t LastInRange =
        std::min<uint64_t>(Last, Range.PhysicalSlotCount - 1);
    for (uint64_t I = First; I <= LastInRange; ++I) {
      Result.Any = true;
      const va_t Slot = Range.BaseAddr + I * Range.EntryStride;
      if (Owner.RequestedSlots.count(Slot))
        Result.RequestedSlots.insert(Slot);
    }
  }
  return Result;
}

bool storageMayBeWritable(const BinaryImage &Img,
                          const ModuleJumpTableOwner &Owner) {
  size_t Work = 0;
  for (const JumpTableStorageRange &Range : Owner.PhysicalIdentityRanges) {
    if (Range.EntrySize == 0 || Range.EntryStride < Range.EntrySize ||
        Range.PhysicalSlotCount == 0 ||
        Range.PhysicalSlotCount > limits::kMaxJumpTableEntries - Work)
      return true;
    Work += Range.PhysicalSlotCount;
    for (uint64_t I = 0; I < Range.PhysicalSlotCount; ++I) {
      if (I != 0 && Range.EntryStride > (InvalidVA - Range.BaseAddr) / I)
        return true;
      const va_t Slot = Range.BaseAddr + I * Range.EntryStride;
      if (Range.EntrySize - 1 > InvalidVA - Slot)
        return true;
      const va_t Last = Slot + Range.EntrySize - 1;
      const Section *FirstSection = Img.getSectionFor(Slot);
      const Section *LastSection = Img.getSectionFor(Last);
      if (FirstSection != LastSection) {
        const Segment *FirstSegment = Img.getSegmentFor(Slot);
        const Segment *LastSegment = Img.getSegmentFor(Last);
        if (FirstSection || LastSection || FirstSegment != LastSegment)
          return true;
      }
      if (isRuntimeWritableAddress(Img, Slot) ||
          isRuntimeWritableAddress(Img, Last))
        return true;
    }
  }
  return false;
}

std::vector<const LowOp *> instructionLoads(const LowFunc &Func,
                                            va_t InsnAddr) {
  std::vector<const LowOp *> Loads;
  for (const LowBlock &Block : Func.Blocks)
    for (const LowOp &Op : Block.Ops)
      if (Op.Addr == InsnAddr && Op.Opcode == NdOp::LOAD)
        Loads.push_back(&Op);
  return Loads;
}

bool isAuthenticatedTableLoad(const LowFunc &Func, va_t Addr, int Seq,
                              const ModuleJumpTableOwner &Owner) {
  for (const JumpTable &JT : Func.JumpTables) {
    const bool SameStorage = std::any_of(
        JT.StorageRanges.begin(), JT.StorageRanges.end(),
        [&](const JumpTableStorageRange &Range) {
          return std::any_of(Owner.StorageRanges.begin(),
                             Owner.StorageRanges.end(),
                             [&](const JumpTableStorageRange &OwnerRange) {
                               return Range == OwnerRange;
                             });
        });
    if (!SameStorage)
      continue;
    if (std::any_of(JT.AuthenticatedTableLoads.begin(),
                    JT.AuthenticatedTableLoads.end(),
                    [&](const JumpTableOpOccurrence &Load) {
                      return Load.Addr == Addr && Load.Seq == Seq;
                    }))
      return true;
  }
  return false;
}

bool isUniquelyAuthenticatedTableLoad(const LowFunc &Func, va_t Addr,
                                      const ModuleJumpTableOwner &Owner) {
  const std::vector<const LowOp *> Loads = instructionLoads(Func, Addr);
  if (Loads.size() != 1)
    return false;
  const LowOp &LoadOp = *Loads.front();
  if (!isAuthenticatedTableLoad(Func, LoadOp.Addr, LoadOp.Seq, Owner))
    return false;
  return std::any_of(
      Func.JumpTables.begin(), Func.JumpTables.end(), [&](const JumpTable &JT) {
        return std::any_of(JT.AuthenticatedTableLoads.begin(),
                           JT.AuthenticatedTableLoads.end(),
                           [&](const JumpTableOpOccurrence &Load) {
                             return Load.Addr == LoadOp.Addr &&
                                    Load.Seq == LoadOp.Seq &&
                                    Load.Size == LoadOp.Output.Size;
                           });
      });
}

struct ModuleAddressUse {
  enum class Kind : uint8_t { Load, PointerEscape, WriteThrough };
  size_t FuncIndex = 0;
  va_t Addr = InvalidVA;
  int Seq = -1;
  Kind UseKind = Kind::PointerEscape;
  uint16_t AccessSize = 0;
  std::set<ModuleAddressAnchor> ExactAddresses;
  std::set<ModuleAddressRootScope> RootScopes;
  bool Imprecise = false;
};

using LowValueKey = std::tuple<VnodeSpace, uint64_t, uint16_t>;
struct ModuleAddressFacts {
  /// Finite may-owner summaries identify a table object, containing section
  /// or segment, or an unknown module object.  Concrete addresses live only
  /// in ExactValues, so summary scopes can never enter numeric arithmetic.
  std::set<ModuleAddressRootScope> Roots;
  std::set<ModuleAddressAnchor> ExactValues;
  std::set<int64_t> FrameOffsets;
  /// Persistent may-alias state for a STACK cell.  Once an address covering
  /// the cell escapes, later strong stores cannot make the cell private again:
  /// the external holder may read or overwrite it at any subsequent call.
  bool FrameCellEscaped = false;
  bool FrameCellPresent = false;
  bool FrameCellOutgoingSeed = false;
  bool MayBeNonFrame = false;
  bool Imprecise = false;
  bool ExactValuesWidened = false;
  /// FrameOffsets grew past its finite bound (a pointer walking the frame in
  /// a loop).  The set then holds a single placeholder offset: the value is
  /// some frame address at an unknown offset, and MayBeNonFrame/Imprecise
  /// make every consumer treat it as such.
  bool FrameOffsetsWidened = false;

  bool empty() const { return Roots.empty() && ExactValues.empty(); }
  bool hasState() const {
    return !empty() || !FrameOffsets.empty() || MayBeNonFrame;
  }
  bool isPrivateFrameOnly() const {
    return FrameOffsets.size() == 1 && !MayBeNonFrame && empty() && !Imprecise;
  }
  bool operator==(const ModuleAddressFacts &Other) const {
    return Roots == Other.Roots && ExactValues == Other.ExactValues &&
           FrameOffsets == Other.FrameOffsets &&
           FrameCellEscaped == Other.FrameCellEscaped &&
           FrameCellPresent == Other.FrameCellPresent &&
           FrameCellOutgoingSeed == Other.FrameCellOutgoingSeed &&
           MayBeNonFrame == Other.MayBeNonFrame &&
           Imprecise == Other.Imprecise &&
           ExactValuesWidened == Other.ExactValuesWidened &&
           FrameOffsetsWidened == Other.FrameOffsetsWidened;
  }
};
using ModuleAddressState = std::map<LowValueKey, ModuleAddressFacts>;

/// The facts of \p State under keys in \p Space.  The map orders its keys by
/// space first, so they are one run of it: a scan of the registers need not
/// visit every frame cell and temporary, nor a scan of the frame the rest.
template <typename StateT> auto entriesIn(StateT &State, VnodeSpace Space) {
  return llvm::make_range(
      State.lower_bound(LowValueKey{Space, 0, 0}),
      State.upper_bound(LowValueKey{Space, std::numeric_limits<uint64_t>::max(),
                                    std::numeric_limits<uint16_t>::max()}));
}

struct ReturnedCodeEvidence {
  std::vector<std::set<va_t>> TargetsByFunction;
  std::vector<bool> CompleteByFunction;
  std::vector<std::vector<LowCxxContinuationExitEvidence>>
      OccurrencesByFunction;
};

struct LocalUnwindContinuationEvidence {
  std::vector<std::set<va_t>> TargetsByFunction;
  std::vector<bool> CompleteByFunction;
  std::vector<bool> SawCallByFunction;
  std::vector<std::vector<LowSEHLocalUnwindEvidence>> SameFrameCallsByFunction;
};

enum class ModuleAddressEdgeTraversal : uint8_t {
  OrdinaryAndExceptional,
  OrdinaryOnly,
  // Inventory each declared activation separately, without forwarding values
  // across a personality-defined exceptional edge. This mode never produces
  // an ordinary returned-continuation certificate.
  IndependentlyRootedOrdinaryOnly,
};

struct ModuleEvidenceBudget {
  explicit ModuleEvidenceBudget(std::optional<size_t> TestLimit)
      : Remaining(
            std::min(TestLimit.value_or(limits::kMaxJumpTableEvidenceWork),
                     size_t{limits::kMaxJumpTableEvidenceWork})) {}

  size_t Remaining = 0;

  bool consume(size_t Amount = 1) {
    if (Amount > Remaining)
      return false;
    Remaining -= Amount;
    return true;
  }
};

LowValueKey lowValueKey(const NdVar &Value) {
  return {Value.Space, Value.Offset, Value.Size};
}

uint64_t coerceUnsigned(uint64_t Value, uint16_t Size) {
  if (Size == 0 || Size >= sizeof(uint64_t))
    return Value;
  return Value & ((uint64_t{1} << (Size * 8)) - 1);
}

uint64_t signExtendUnsigned(uint64_t Value, uint16_t InputSize,
                            uint16_t OutputSize) {
  Value = coerceUnsigned(Value, InputSize);
  if (InputSize != 0 && InputSize < sizeof(uint64_t)) {
    const unsigned InputBits = InputSize * 8;
    const uint64_t SignBit = uint64_t{1} << (InputBits - 1);
    if ((Value & SignBit) != 0)
      Value |= ~((uint64_t{1} << InputBits) - 1);
  }
  return coerceUnsigned(Value, OutputSize);
}

std::optional<int64_t> signedConstantAtArithmeticWidth(uint64_t Value,
                                                       uint16_t OutputSize) {
  if (OutputSize == 0 || OutputSize > sizeof(uint64_t))
    return std::nullopt;
  const unsigned Bits = OutputSize * 8;
  llvm::APInt Pattern(Bits, coerceUnsigned(Value, OutputSize));
  return Pattern.sextOrTrunc(64).getSExtValue();
}

bool adjustFrameOffsets(ModuleAddressFacts &Facts, uint64_t RawDelta,
                        uint16_t ArithmeticSize, bool Subtract) {
  if (Facts.FrameOffsets.empty() || Facts.FrameOffsetsWidened)
    return true;
  const std::optional<int64_t> Delta =
      signedConstantAtArithmeticWidth(RawDelta, ArithmeticSize);
  if (!Delta) {
    Facts.FrameOffsets.clear();
    Facts.MayBeNonFrame = true;
    Facts.Imprecise = true;
    return false;
  }
  std::set<int64_t> Adjusted;
  for (int64_t Offset : Facts.FrameOffsets) {
    int64_t Result = 0;
    const bool Overflow = Subtract ? llvm::SubOverflow(Offset, *Delta, Result)
                                   : llvm::AddOverflow(Offset, *Delta, Result);
    if (Overflow) {
      Facts.FrameOffsets.clear();
      Facts.MayBeNonFrame = true;
      Facts.Imprecise = true;
      return false;
    }
    Adjusted.insert(Result);
  }
  Facts.FrameOffsets = std::move(Adjusted);
  return true;
}

/// Collect occurrence-backed table-address uses that do not depend on loader
/// relocation maps.  This deliberately computes a may-set: retaining an extra
/// mirror field is harmless, while missing one can make a later function read
/// a stale zero.  Exact address provenance is required at the root; scalar
/// numeric coincidence never creates a use.
bool collectLowAddressUses(
    const BinaryImage &Img, const std::vector<LowFunc> &Funcs,
    const std::vector<ModuleJumpTableOwner> &Owners,
    std::vector<ModuleAddressUse> &Uses, ModuleEvidenceBudget &Budget,
    std::optional<size_t> TestBudget,
    const std::set<va_t> *AdditionalExactRoots = nullptr,
    ReturnedCodeEvidence *ReturnedCode = nullptr,
    ModuleAddressEdgeTraversal EdgeTraversal =
        ModuleAddressEdgeTraversal::OrdinaryAndExceptional,
    LocalUnwindContinuationEvidence *LocalUnwind = nullptr,
    const std::set<va_t> *OccurrenceTableAnchors = nullptr) {
  const TargetRegInfo &TRI = getTargetRegInfo(Img.Arch);
  const std::vector<TargetRegisterRange> PreservedRanges =
      TRI.callPreservedRanges(Img.abiFormat());
  // The register containing a register, which every read and write asks of
  // each register entry it scans; it depends on the target alone.
  llvm::DenseMap<std::pair<uint64_t, uint16_t>, std::pair<uint64_t, uint16_t>>
      WideRegisters;
  auto wideRegister = [&](uint64_t Offset, uint16_t Size) {
    auto [It, Inserted] = WideRegisters.try_emplace({Offset, Size});
    if (Inserted)
      It->second = TRI.findWideReg(Offset, Size);
    return It->second;
  };
  const auto SEHRuntimeMemory = collectLowSEHRuntimeMemory(Img, Funcs);
  std::map<va_t, const LowFunc *> SEHCallees;
  if (LocalUnwind)
    for (const auto &Func : Funcs)
      if (!SEHCallees.emplace(Func.Entry, &Func).second)
        return false;
  // Input capacity is distinct from retained evidence and fixed-point work.
  // Permit an inventory at the existing single-function SSA admission scale;
  // this does not certify its address flow or enlarge the queue allowance.
  constexpr size_t kMaxModuleAddressInputOps = limits::kMaxSSAFunctionOps;
  constexpr size_t kMaxModuleAddressFixpointWork =
      size_t{limits::kMaxJumpTableEvidenceWork} *
      size_t{limits::kMaxMultiStageRetries};
  // Address-use receipts and complete-CFG frame dataflow count different
  // work. Charging every lifted operation to the small address-use budget
  // would reject a straight-line function before its first continuation and
  // would also discard unrelated module ownership evidence. Keep a separate,
  // cumulative cap for frame transfers, joins and dependency hashing.
  constexpr size_t kMaxSEHFrameWork = kMaxModuleAddressFixpointWork * 16;
  size_t SEHFrameWorkRemaining =
      std::min(TestBudget.value_or(kMaxSEHFrameWork), kMaxSEHFrameWork);
  size_t ScanOps = 0;
  auto failCollect = [](const char *, size_t = 0, va_t = InvalidVA) {
    return false;
  };
  size_t FixpointWorkRemaining =
      std::min(TestBudget.value_or(kMaxModuleAddressFixpointWork),
               kMaxModuleAddressFixpointWork);
  // A queue debit alone does not bound repeatedly visiting a long block.
  // Charge every operation in both the fixed point and final recording pass.
  constexpr size_t kMaxAddressTransferWork = kMaxModuleAddressFixpointWork * 16;
  size_t TransferWorkRemaining = kMaxAddressTransferWork;
  auto consumeFixpointWork = [&]() {
    if (FixpointWorkRemaining == 0)
      return false;
    --FixpointWorkRemaining;
    return true;
  };
  for (const LowFunc &Func : Funcs)
    for (const LowBlock &Block : Func.Blocks) {
      if (Block.Ops.size() > kMaxModuleAddressInputOps - ScanOps)
        return failCollect("scan-cap", 0, Block.StartAddr);
      ScanOps += Block.Ops.size();
    }
  if (ReturnedCode) {
    ReturnedCode->TargetsByFunction.assign(Funcs.size(), {});
    ReturnedCode->CompleteByFunction.assign(Funcs.size(), true);
    ReturnedCode->OccurrencesByFunction.assign(Funcs.size(), {});
  }
  if (LocalUnwind) {
    LocalUnwind->TargetsByFunction.assign(Funcs.size(), {});
    LocalUnwind->CompleteByFunction.assign(Funcs.size(), true);
    LocalUnwind->SawCallByFunction.assign(Funcs.size(), false);
    LocalUnwind->SameFrameCallsByFunction.assign(Funcs.size(), {});
  }

  std::map<va_t, std::vector<const ModuleJumpTableOwner *>> OwnersByContainer;
  for (const ModuleJumpTableOwner &Owner : Owners) {
    if (Owner.StorageIdentityVA == InvalidVA)
      return failCollect("invalid-owner-identity");
    std::set<va_t> OwnerContainers;
    for (const JumpTableStorageRange &Range : Owner.PhysicalIdentityRanges) {
      const va_t ContainerVA = storageRangeOwnerVA(Img, Range);
      if (ContainerVA == InvalidVA || !Range.storageEnd())
        return failCollect("invalid-owner-range");
      OwnerContainers.insert(ContainerVA);
    }
    for (va_t ContainerVA : OwnerContainers)
      OwnersByContainer[ContainerVA].push_back(&Owner);
  }
  std::set<va_t> AdditionalExactOwnerVAs;
  if (AdditionalExactRoots)
    for (va_t Address : *AdditionalExactRoots) {
      if (const Section *Sec = Img.getSectionFor(Address))
        AdditionalExactOwnerVAs.insert(Sec->VA);
      else if (const Segment *Seg = Img.getSegmentFor(Address))
        AdditionalExactOwnerVAs.insert(Seg->VA);
    }

  auto exactCouldMatchAnyOwner = [&](const ModuleAddressAnchor &Exact) {
    // Returned-code completeness is decided at the exact RETURN occurrence.
    // Keep every authenticated alternative until then: filtering an address
    // merely because its owner differs from every candidate owner would turn
    // `{candidate, other-exact}` into a falsely complete singleton.
    if (ReturnedCode && AdditionalExactRoots)
      return true;
    if (AdditionalExactRoots && AdditionalExactRoots->count(Exact.Address) != 0)
      return true;
    // PC-relative materialization commonly starts from the instruction-end
    // address and reaches the published code target through a constant ADD.
    // Retain intermediate exact values in the same authenticated container;
    // only final RETURN values that exactly match AdditionalExactRoots are
    // exported as continuation evidence.
    if (Exact.OwnerVA != InvalidVA &&
        AdditionalExactOwnerVAs.count(Exact.OwnerVA) != 0)
      return true;
    switch (Exact.OwnerKind) {
    case ModuleAddressOwnerKind::Unknown:
      return true;
    case ModuleAddressOwnerKind::Container:
      return Exact.OwnerVA != InvalidVA &&
             OwnersByContainer.count(Exact.OwnerVA) != 0;
    case ModuleAddressOwnerKind::TableObject:
      return Exact.OwnerVA != InvalidVA &&
             std::any_of(Owners.begin(), Owners.end(),
                         [&](const ModuleJumpTableOwner &Owner) {
                           return Owner.StorageIdentityVA == Exact.OwnerVA;
                         });
    }
    return false;
  };

  auto classifyExactRoot = [&](std::set<ModuleAddressRootScope> &Roots,
                               const ModuleAddressAnchor &Exact,
                               bool IncludeFallback) {
    if (Exact.OwnerKind == ModuleAddressOwnerKind::Unknown ||
        Exact.OwnerVA == InvalidVA) {
      if (IncludeFallback)
        Roots.insert({InvalidVA, ModuleAddressOwnerKind::Unknown});
      return false;
    }
    if (Exact.OwnerKind == ModuleAddressOwnerKind::TableObject) {
      Roots.insert({Exact.OwnerVA, ModuleAddressOwnerKind::TableObject});
      return true;
    }

    // A final public exact DataAddress occurrence for a relocation-backed
    // table slot supplies the same logical-object identity used by the local
    // CFG builder.  Loader-authenticated relative anchors carry that identity
    // directly.  In both cases keep every containing owner for an interior
    // anchor, but at an exact end prefer the adjacent object over the previous
    // table's one-past envelope.
    const bool IsExactTableAnchor =
        Img.RelCodeTableAnchors.count(Exact.Address) != 0 ||
        (OccurrenceTableAnchors &&
         OccurrenceTableAnchors->count(Exact.Address) != 0);
    bool MatchedTable = IsExactTableAnchor;
    if (IsExactTableAnchor)
      Roots.insert({Exact.Address, ModuleAddressOwnerKind::TableObject});
    if (auto It = OwnersByContainer.find(Exact.OwnerVA);
        It != OwnersByContainer.end()) {
      for (const ModuleJumpTableOwner *Owner : It->second) {
        if (Owner->StorageIdentityVA == InvalidVA)
          continue;
        const bool Contains =
            std::any_of(Owner->PhysicalIdentityRanges.begin(),
                        Owner->PhysicalIdentityRanges.end(),
                        [&](const JumpTableStorageRange &Range) {
                          if (storageRangeOwnerVA(Img, Range) != Exact.OwnerVA)
                            return false;
                          const std::optional<va_t> End = Range.storageEnd();
                          if (!End || Exact.Address < Range.BaseAddr ||
                              Exact.Address > *End)
                            return false;
                          if (IsExactTableAnchor && Exact.Address == *End)
                            return false;
                          return true;
                        });
        if (!Contains)
          continue;
        Roots.insert(
            {Owner->StorageIdentityVA, ModuleAddressOwnerKind::TableObject});
        MatchedTable = true;
      }
    }
    if (!MatchedTable && IncludeFallback)
      Roots.insert({Exact.OwnerVA, ModuleAddressOwnerKind::Container});
    return MatchedTable;
  };

  auto initializeExactRoots = [&](ModuleAddressFacts &Facts) {
    Facts.Roots.clear();
    for (auto It = Facts.ExactValues.begin(); It != Facts.ExactValues.end();) {
      if (!exactCouldMatchAnyOwner(*It))
        It = Facts.ExactValues.erase(It);
      else
        ++It;
    }
    for (const ModuleAddressAnchor &Exact : Facts.ExactValues)
      classifyExactRoot(Facts.Roots, Exact, true);
  };
  auto reclassifyExactRoots = [&](ModuleAddressFacts &Facts) {
    std::set<ModuleAddressRootScope> ObjectRoots;
    std::set<ModuleAddressRootScope> FallbackRoots;
    bool AllTableObjects = !Facts.ExactValues.empty();
    for (const ModuleAddressAnchor &Exact : Facts.ExactValues) {
      const bool IsTableObject = classifyExactRoot(ObjectRoots, Exact, false);
      AllTableObjects &= IsTableObject;
      if (!IsTableObject)
        classifyExactRoot(FallbackRoots, Exact, true);
    }
    if (AllTableObjects && !Facts.Imprecise && !Facts.ExactValuesWidened) {
      Facts.Roots = std::move(ObjectRoots);
      return;
    }
    Facts.Roots.insert(ObjectRoots.begin(), ObjectRoots.end());
    if (Facts.Roots.empty())
      Facts.Roots.insert(FallbackRoots.begin(), FallbackRoots.end());
  };

  for (size_t FuncIndex = 0; FuncIndex < Funcs.size(); ++FuncIndex) {
    const LowFunc &Func = Funcs[FuncIndex];
    const size_t BlockCount = Func.Blocks.size();
    std::map<int, size_t> PositionForId;
    for (size_t I = 0; I < BlockCount; ++I)
      PositionForId[Func.Blocks[I].Id] = I;
    std::map<std::pair<va_t, int>, RelocatedInstructionAddressOccurrence>
        ExactOutputMaterializations;
    auto sameOutputMaterialization =
        [](const RelocatedInstructionAddressOccurrence &Left,
           const RelocatedInstructionAddressOccurrence &Right) {
          // FieldVA identifies one provenance source, not a distinct result.
          // Multiple authenticated literals may feed the same exact output;
          // module transfer needs one value summary while LowFunc retains the
          // complete source occurrence set.  Every semantic result/role field
          // must still agree before the records can share an (Addr, Seq) key.
          return Left.TargetVA == Right.TargetVA &&
                 Left.TargetOwnerVA == Right.TargetOwnerVA &&
                 Left.Width == Right.Width &&
                 Left.Provenance == Right.Provenance &&
                 Left.OutputOpcode == Right.OutputOpcode &&
                 Left.OutputWitness == Right.OutputWitness;
        };
    for (const RelocatedInstructionAddressOccurrence &Occurrence :
         Func.RelocatedInstructionAddressOccurrences) {
      if (!Occurrence.DefinesOutput)
        continue;
      if (Occurrence.Authority ==
          RelocatedInstructionAddressProofKind::X86PCRelativeCodeAddress) {
        const va_t NormalizedTarget =
            normalizeCodeAddress(Occurrence.TargetVA, Img.Arch, Img.Mode);
        if ((Img.Arch != Arch::X86 && Img.Arch != Arch::X64) ||
            Occurrence.FieldVA != InvalidVA ||
            Occurrence.Width != Img.getPointerSize() ||
            Occurrence.Provenance != ConstantAddressProvenance::CodeAddress ||
            !Occurrence.PCRelativeFromInstructionEnd ||
            Occurrence.OutputOpcode != NdOp::COPY ||
            !Occurrence.OutputWitness.isReg() ||
            Occurrence.SeedInstructionAddr != Occurrence.InstructionAddr ||
            Occurrence.SeedOpSeq != Occurrence.OpSeq ||
            Occurrence.SeedOpcode != Occurrence.OutputOpcode ||
            !Occurrence.SeedInputWitness.isTemp() ||
            Occurrence.SeedInputWitness.Size != Occurrence.Width ||
            Occurrence.SeedOutputWitness != Occurrence.OutputWitness ||
            !Img.hasExecutableCodeOwnerAt(NormalizedTarget) ||
            !Img.relocatedTargetBelongsToOwner(NormalizedTarget,
                                               Occurrence.TargetOwnerVA) ||
            std::find(Func.CodeRefTargets.begin(), Func.CodeRefTargets.end(),
                      NormalizedTarget) == Func.CodeRefTargets.end())
          return failCollect("invalid-x86-code-address-materialization",
                             FuncIndex, Occurrence.InstructionAddr);
      }
      // A subset-path materialization does not describe the other reaching
      // values of the output.  Treating its relocation target as the complete
      // value set would lose another authenticated page (or an opaque live-in)
      // and could miss a writable-table escape.  Until the public witness can
      // carry every reaching alternative, make module arbitration incomplete:
      // its fallback preserves all mirrors and rejects writable static tables.
      if (Occurrence.OutputMayDepend)
        return failCollect("partial-output-materialization", FuncIndex,
                           Occurrence.InstructionAddr);
      if (Occurrence.InstructionAddr == InvalidVA || Occurrence.OpSeq < 0 ||
          Occurrence.TargetVA == InvalidVA ||
          Occurrence.TargetOwnerVA == InvalidVA ||
          Occurrence.OutputOpcode == NdOp::NOP ||
          (!Occurrence.OutputWitness.isReg() &&
           !Occurrence.OutputWitness.isTemp()) ||
          Occurrence.OutputWitness.Size == 0)
        return failCollect("invalid-output-materialization", FuncIndex,
                           Occurrence.InstructionAddr);
      const auto Key =
          std::make_pair(Occurrence.InstructionAddr, Occurrence.OpSeq);
      auto [It, Inserted] =
          ExactOutputMaterializations.emplace(Key, Occurrence);
      if (!Inserted && !sameOutputMaterialization(It->second, Occurrence))
        return failCollect("conflicting-output-materialization", FuncIndex,
                           Occurrence.InstructionAddr);
    }
    std::vector<ModuleAddressState> InStates(BlockCount);
    std::vector<ModuleAddressState> OutStates(BlockCount);
    std::map<std::pair<va_t, int>, LowCxxContinuationExitEvidence>
        ReturnedOccurrences;
    std::optional<LowSEHFrameProof> SEHFrameProof;

    auto mergeFacts = [&](ModuleAddressFacts &Into,
                          const ModuleAddressFacts &From) {
      Into.Roots.insert(From.Roots.begin(), From.Roots.end());
      Into.ExactValues.insert(From.ExactValues.begin(), From.ExactValues.end());
      Into.FrameOffsets.insert(From.FrameOffsets.begin(),
                               From.FrameOffsets.end());
      Into.FrameCellEscaped |= From.FrameCellEscaped;
      Into.FrameCellPresent |= From.FrameCellPresent;
      Into.FrameCellOutgoingSeed |= From.FrameCellOutgoingSeed;
      Into.MayBeNonFrame |= From.MayBeNonFrame;
      Into.Imprecise |= From.Imprecise;
      Into.ExactValuesWidened |= From.ExactValuesWidened;
      if (Into.ExactValuesWidened ||
          Into.ExactValues.size() >
              ::neverd::limits::pipeline::
                  kPipelineLowIRMaxExactAddressAlternatives) {
        Into.ExactValues.clear();
        Into.ExactValuesWidened = true;
        Into.Imprecise = true;
      }
      Into.FrameOffsetsWidened |= From.FrameOffsetsWidened;
      if (!Into.FrameOffsets.empty() &&
          (Into.FrameOffsetsWidened ||
           Into.FrameOffsets.size() >
               ::neverd::limits::pipeline::
                   kPipelineLowIRMaxFrameOffsetAlternatives)) {
        Into.FrameOffsets = {0};
        Into.FrameOffsetsWidened = true;
        Into.MayBeNonFrame = true;
        Into.Imprecise = true;
      }
      const bool Complete =
          Into.Roots.size() <= limits::kMaxJumpTableEvidenceWork &&
          Into.FrameOffsets.size() <= limits::kMaxJumpTableEvidenceWork;
      return Complete;
    };
    auto mergePredecessors = [&](const LowBlock &Block,
                                 ModuleAddressState &Merged) {
      std::vector<int> Preds = Block.Preds;
      if (EdgeTraversal == ModuleAddressEdgeTraversal::OrdinaryAndExceptional)
        for (const ExceptionalEdge &Edge : Block.ExceptionalPreds)
          Preds.push_back(Edge.BlockId);
      if (Preds.empty())
        return true;
      std::map<LowValueKey, size_t> Seen;
      for (int PredId : Preds) {
        auto Pos = PositionForId.find(PredId);
        if (Pos == PositionForId.end())
          continue;
        for (const auto &[Key, Facts] : OutStates[Pos->second]) {
          if (!mergeFacts(Merged[Key], Facts))
            return false;
          ++Seen[Key];
        }
      }
      for (auto &[Key, Facts] : Merged)
        if (Seen[Key] != Preds.size()) {
          Facts.Imprecise = true;
          Facts.MayBeNonFrame = true;
        }
      return true;
    };
    auto mergeAlternativeStates = [&](const ModuleAddressState &First,
                                      const ModuleAddressState &Second,
                                      ModuleAddressState &Merged) {
      std::map<LowValueKey, unsigned> Seen;
      auto MergeOne = [&](const ModuleAddressState &Alternative) {
        for (const auto &[Key, Facts] : Alternative) {
          if (!mergeFacts(Merged[Key], Facts))
            return false;
          ++Seen[Key];
        }
        return true;
      };
      if (!MergeOne(First) || !MergeOne(Second))
        return false;
      for (auto &[Key, Facts] : Merged)
        if (Seen[Key] != 2) {
          // The value/cell exists on only one execution of the predicated
          // instruction.  Keep its authenticated roots as may-information,
          // but never treat the missing arm as an exact definition.
          Facts.Imprecise = true;
          Facts.MayBeNonFrame = true;
        }
      return true;
    };
    auto transfer = [&](const LowBlock &Block, ModuleAddressState State,
                        std::vector<ModuleAddressUse> *RecordedUses,
                        ModuleAddressState &Result) {
      if (Block.Ops.size() > TransferWorkRemaining)
        return false;
      TransferWorkRemaining -= Block.Ops.size();
      // The architectural stack epoch is a durable fact only at the real
      // function entry.  A disconnected relocation root, or a path after an
      // opaque/pivoting SP definition, must never manufacture a fresh epoch 0
      // merely because the current state has no SP key.
      if (Block.StartAddr == Func.Entry && Img.getPointerSize() != 0) {
        ModuleAddressFacts &EntrySP =
            State[{VnodeSpace::REG, TRI.StackPointer,
                   static_cast<uint16_t>(Img.getPointerSize())}];
        EntrySP.FrameOffsets.insert(0);
      }
      struct RegisterLane {
        uint64_t WideOffset = 0;
        uint16_t WideSize = 0;
        uint16_t Begin = 0;
        uint16_t End = 0;
      };
      auto registerLane = [&](uint64_t Offset,
                              uint16_t Size) -> std::optional<RegisterLane> {
        if (Size == 0)
          return std::nullopt;
        if (TRI.isFlag(Offset, Size))
          return RegisterLane{Offset, Size, 0, Size};
        const auto [WideOffset, WideSize] = wideRegister(Offset, Size);
        int ByteOffset = 0;
        if (WideOffset != Offset || WideSize != Size) {
          ByteOffset = TRI.subRegByteOffset(Offset, Size, WideOffset, WideSize);
          if (ByteOffset < 0 ||
              static_cast<uint64_t>(ByteOffset) + Size > WideSize)
            return RegisterLane{Offset, Size, 0, Size};
        }
        return RegisterLane{WideOffset, WideSize,
                            static_cast<uint16_t>(ByteOffset),
                            static_cast<uint16_t>(ByteOffset + Size)};
      };
      auto registerLanesOverlap = [](const RegisterLane &Left,
                                     const RegisterLane &Right) {
        return Left.WideOffset == Right.WideOffset &&
               Left.WideSize == Right.WideSize && Left.Begin < Right.End &&
               Right.Begin < Left.End;
      };
      bool RegisterProjectionComplete = true;
      auto factsFor = [&](const NdVar &Value) {
        ModuleAddressFacts Facts;
        if (Value.Size == 0)
          return Facts;
        auto It = State.find(lowValueKey(Value));
        if (It != State.end() && Value.Space != VnodeSpace::REG)
          return It->second;
        if (Value.Space == VnodeSpace::REG) {
          const std::optional<RegisterLane> Requested =
              registerLane(Value.Offset, Value.Size);
          if (Requested) {
            bool Projected = false;
            if (It != State.end()) {
              Facts = It->second;
              Projected = true;
            }
            for (const auto &[Key, ExistingFacts] :
                 entriesIn(State, VnodeSpace::REG)) {
              const auto &[Space, Offset, Size] = Key;
              (void)Space;
              if (Key == lowValueKey(Value))
                continue;
              const std::optional<RegisterLane> Existing =
                  registerLane(Offset, Size);
              if (!Existing || Existing->WideOffset != Requested->WideOffset ||
                  Existing->WideSize != Requested->WideSize ||
                  Existing->Begin > Requested->Begin ||
                  Existing->End < Requested->End)
                continue;
              ModuleAddressFacts View = ExistingFacts;
              // A sub-register read is a value projection, not the original
              // pointer or frame-base identity.  Preserve the authenticated
              // object roots as may-dependencies, but do not turn coincident
              // low bits into a new exact address or frame epoch.
              if (Existing->Begin != Requested->Begin ||
                  Existing->End != Requested->End) {
                reclassifyExactRoots(View);
                View.ExactValues.clear();
                if (!View.FrameOffsets.empty()) {
                  View.FrameOffsets.clear();
                  View.MayBeNonFrame = true;
                }
                View.FrameCellEscaped = false;
                View.FrameCellPresent = false;
                View.FrameCellOutgoingSeed = false;
                View.Imprecise = true;
              }
              if (!Projected) {
                Facts = std::move(View);
                Projected = true;
              } else if (!mergeFacts(Facts, View)) {
                RegisterProjectionComplete = false;
                return Facts;
              }
            }
            if (Projected)
              return Facts;
          }
        }
        if (Value.isConst()) {
          if (isExactAddressProvenance(Value.Provenance)) {
            const va_t Address = coerceUnsigned(Value.Offset, Value.Size);
            const ModuleAddressAnchor Anchor{
                Address, Value.AddressOwnerVA,
                Value.AddressOwnerVA == InvalidVA
                    ? ModuleAddressOwnerKind::Unknown
                    : ModuleAddressOwnerKind::Container};
            Facts.ExactValues.insert(Anchor);
            initializeExactRoots(Facts);
            Facts.MayBeNonFrame = !Facts.empty();
          }
          return Facts;
        }
        return Facts;
      };
      auto addUse = [&](const LowOp &Op, ModuleAddressUse::Kind Kind,
                        const ModuleAddressFacts &Facts,
                        uint16_t AccessSize = 0) {
        if (!RecordedUses)
          return true;
        // Only occurrence-backed table-address roots participate in module
        // arbitration.  A frame-only address is handled by the frame-memory
        // escape lattice below; an arbitrary scalar/opaque pointer does not
        // become a may-alias to every table merely because it is imprecise.
        if (Facts.empty())
          return true;
        if (!Budget.consume())
          return false;
        ModuleAddressUse Use;
        Use.FuncIndex = FuncIndex;
        Use.Addr = Op.Addr;
        Use.Seq = Op.Seq;
        Use.UseKind = Kind;
        Use.AccessSize = AccessSize;
        // Exact transforms name the concrete field being read.  Roots are
        // retained for imprecise arithmetic/merges, where any address in the
        // containing physical object may be reached and the whole owner must
        // be preserved.
        if (!Facts.Imprecise && !Facts.ExactValues.empty())
          Use.ExactAddresses = Facts.ExactValues;
        else {
          Use.RootScopes = Facts.Roots;
          Use.ExactAddresses = Facts.ExactValues;
        }
        Use.Imprecise = Facts.Imprecise;
        RecordedUses->push_back(std::move(Use));
        return true;
      };
      auto coerceFacts = [&](ModuleAddressFacts Facts, uint16_t Size) {
        std::set<ModuleAddressAnchor> Coerced;
        for (ModuleAddressAnchor Anchor : Facts.ExactValues) {
          Anchor.Address = coerceUnsigned(Anchor.Address, Size);
          Coerced.insert(Anchor);
        }
        Facts.ExactValues = std::move(Coerced);
        reclassifyExactRoots(Facts);
        return Facts;
      };
      auto signExtendFacts = [&](ModuleAddressFacts Facts, uint16_t InputSize,
                                 uint16_t OutputSize) {
        std::set<ModuleAddressAnchor> ExtendedValues;
        for (ModuleAddressAnchor Anchor : Facts.ExactValues) {
          Anchor.Address =
              signExtendUnsigned(Anchor.Address, InputSize, OutputSize);
          ExtendedValues.insert(Anchor);
        }
        Facts.ExactValues = std::move(ExtendedValues);
        reclassifyExactRoots(Facts);
        return Facts;
      };
      auto frameMemoryKey = [](int64_t Offset, uint16_t Size) {
        return LowValueKey{VnodeSpace::STACK, static_cast<uint64_t>(Offset),
                           Size};
      };
      auto allFrameMemoryEscapedKey = []() {
        // Real memory accesses never have width zero.  A zero-width STACK
        // cell is therefore an unambiguous lattice sentinel saying that an
        // imprecise frame address escaped and every present/future frame cell
        // must be treated as externally aliased.
        return LowValueKey{VnodeSpace::STACK, uint64_t{0}, uint16_t{0}};
      };
      auto frameIntervalsOverlap = [](int64_t Left, uint16_t LeftSize,
                                      int64_t Right, uint16_t RightSize) {
        if (LeftSize == 0 || RightSize == 0)
          return true;
        int64_t LeftEnd = 0;
        int64_t RightEnd = 0;
        if (llvm::AddOverflow(Left, static_cast<int64_t>(LeftSize), LeftEnd) ||
            llvm::AddOverflow(Right, static_cast<int64_t>(RightSize), RightEnd))
          return true;
        return Left < RightEnd && Right < LeftEnd;
      };
      auto eraseFrameMemory = [&](const ModuleAddressFacts &Address,
                                  uint16_t AccessSize) {
        const bool Exact = Address.isPrivateFrameOnly();
        const int64_t ExactOffset =
            Exact ? *Address.FrameOffsets.begin() : int64_t{0};
        for (auto It = State.begin(); It != State.end();) {
          const auto &[Space, RawOffset, StoredSize] = It->first;
          if (Space != VnodeSpace::STACK) {
            ++It;
            continue;
          }
          const int64_t StoredOffset = static_cast<int64_t>(RawOffset);
          if (!Exact || frameIntervalsOverlap(ExactOffset, AccessSize,
                                              StoredOffset, StoredSize)) {
            if (It->second.FrameCellEscaped) {
              ModuleAddressFacts Tombstone;
              Tombstone.FrameCellEscaped = true;
              It->second = std::move(Tombstone);
              ++It;
            } else {
              It = State.erase(It);
            }
          } else {
            ++It;
          }
        }
      };
      auto markFrameMemoryEscaped = [&](const ModuleAddressFacts &Address,
                                        uint16_t AccessSize) {
        if (AccessSize == 0)
          return false;
        // A finite may-set is sufficient: every frame alternative becomes a
        // persistent escaped interval.  A simultaneous non-frame arm is
        // handled by the ordinary pointer-escape use and does not erase the
        // known frame aliases.  An empty set simply means this value carried
        // no frame address to mark.
        if (Address.FrameOffsets.empty())
          return true;
        if (Address.Imprecise || Address.MayBeNonFrame ||
            Address.FrameOffsets.size() != 1) {
          State[allFrameMemoryEscapedKey()].FrameCellEscaped = true;
          for (auto &[Key, Stored] : entriesIn(State, VnodeSpace::STACK))
            Stored.FrameCellEscaped = true;
          return true;
        }
        for (int64_t EscapedOffset : Address.FrameOffsets) {
          for (auto &[Key, Stored] : State) {
            const auto &[Space, RawOffset, StoredSize] = Key;
            if (Space == VnodeSpace::STACK &&
                frameIntervalsOverlap(EscapedOffset, AccessSize,
                                      static_cast<int64_t>(RawOffset),
                                      StoredSize))
              Stored.FrameCellEscaped = true;
          }
          State[frameMemoryKey(EscapedOffset, AccessSize)].FrameCellEscaped =
              true;
        }
        return true;
      };
      auto frameMemoryMayHaveEscaped = [&](const ModuleAddressFacts &Address,
                                           uint16_t AccessSize) {
        if (AccessSize == 0 || Address.FrameOffsets.empty() ||
            Address.MayBeNonFrame || Address.Imprecise)
          return true;
        for (int64_t Offset : Address.FrameOffsets)
          for (const auto &[Key, Stored] :
               entriesIn(State, VnodeSpace::STACK)) {
            const auto &[Space, RawOffset, StoredSize] = Key;
            (void)Space;
            if (Stored.FrameCellEscaped &&
                frameIntervalsOverlap(Offset, AccessSize,
                                      static_cast<int64_t>(RawOffset),
                                      StoredSize))
              return true;
          }
        return false;
      };
      auto loadFrameMemory = [&](const ModuleAddressFacts &Address,
                                 uint16_t AccessSize) {
        ModuleAddressFacts Loaded;
        if (AccessSize == 0 || Address.FrameOffsets.empty())
          return Loaded;
        if (Address.MayBeNonFrame) {
          Loaded.MayBeNonFrame = true;
          Loaded.Imprecise = true;
          return Loaded;
        }
        bool MissingAlternative = false;
        for (int64_t LoadOffset : Address.FrameOffsets) {
          bool Found = false;
          for (const auto &[Key, Stored] :
               entriesIn(State, VnodeSpace::STACK)) {
            const auto &[Space, RawOffset, StoredSize] = Key;
            (void)Space;
            if (!frameIntervalsOverlap(LoadOffset, AccessSize,
                                       static_cast<int64_t>(RawOffset),
                                       StoredSize))
              continue;
            Found = true;
            if (Stored.FrameCellEscaped) {
              Loaded = ModuleAddressFacts{};
              Loaded.MayBeNonFrame = true;
              Loaded.Imprecise = true;
              return Loaded;
            }
            if (!mergeFacts(Loaded, Stored)) {
              Loaded = ModuleAddressFacts{};
              Loaded.MayBeNonFrame = true;
              Loaded.Imprecise = true;
              return Loaded;
            }
            if (LoadOffset != static_cast<int64_t>(RawOffset) ||
                AccessSize != StoredSize)
              Loaded.Imprecise = true;
          }
          MissingAlternative |= !Found;
        }
        Loaded.Imprecise |=
            Address.FrameOffsets.size() != 1 || MissingAlternative;
        Loaded.MayBeNonFrame |= MissingAlternative;
        // These flags describe the STACK cell, not the value loaded from it.
        Loaded.FrameCellEscaped = false;
        Loaded.FrameCellPresent = false;
        Loaded.FrameCellOutgoingSeed = false;
        return Loaded;
      };
      auto escapeFrameAddress = [&](const LowOp &Op,
                                    const ModuleAddressFacts &Address,
                                    uint16_t AccessSize) {
        if (Address.FrameOffsets.empty())
          return true;
        const ModuleAddressFacts Contents =
            loadFrameMemory(Address, AccessSize);
        if (!addUse(Op, ModuleAddressUse::Kind::PointerEscape, Contents) ||
            !markFrameMemoryEscaped(Address, AccessSize))
          return false;
        eraseFrameMemory(Address, AccessSize);
        return true;
      };
      auto storeFrameMemory = [&](int64_t Offset, uint16_t AccessSize,
                                  ModuleAddressFacts Stored,
                                  bool EscapedCell = false) {
        Stored.FrameCellEscaped = EscapedCell;
        Stored.FrameCellPresent = true;
        Stored.FrameCellOutgoingSeed = false;
        const ModuleAddressFacts CurrentSP = factsFor(NdVar::reg(
            TRI.StackPointer, static_cast<uint16_t>(TRI.PointerSize)));
        if (CurrentSP.isPrivateFrameOnly()) {
          int64_t OutgoingBase = *CurrentSP.FrameOffsets.begin();
          if (Img.Arch == Arch::X64 && Img.abiFormat() == BinaryFormat::COFF) {
            int64_t WithShadow = 0;
            if (!llvm::AddOverflow(OutgoingBase, int64_t{32}, WithShadow))
              OutgoingBase = WithShadow;
          }
          Stored.FrameCellOutgoingSeed = Offset == OutgoingBase;
        }
        State[frameMemoryKey(Offset, AccessSize)] = std::move(Stored);
      };

      std::map<size_t, size_t> GuardEnds;
      for (const LowInstructionBoundary &Boundary :
           Block.InstructionBoundaries) {
        if (!hasLowInstructionControlFlag(
                Boundary.ControlFlags,
                LowInstructionControlFlag::InstructionGuard))
          continue;
        if (Boundary.FirstOp > Block.Ops.size() || Boundary.OpCount == 0 ||
            Boundary.OpCount > Block.Ops.size() - Boundary.FirstOp)
          return false;
        const size_t First = static_cast<size_t>(Boundary.FirstOp);
        const size_t End = First + static_cast<size_t>(Boundary.OpCount);
        if (!GuardEnds.emplace(First, End).second)
          return false;
      }
      std::optional<ModuleAddressState> GuardSkipState;
      size_t ActiveGuardEnd = 0;
      auto finishInstructionGuard = [&](size_t NextOp) {
        if (!GuardSkipState || NextOp != ActiveGuardEnd)
          return true;
        ModuleAddressState Joined;
        if (!mergeAlternativeStates(*GuardSkipState, State, Joined))
          return false;
        State = std::move(Joined);
        GuardSkipState.reset();
        ActiveGuardEnd = 0;
        return true;
      };

      for (size_t OpIndex = 0; OpIndex < Block.Ops.size(); ++OpIndex) {
        if (auto It = GuardEnds.find(OpIndex); It != GuardEnds.end()) {
          if (GuardSkipState)
            return false;
          GuardSkipState = State;
          ActiveGuardEnd = It->second;
        }
        const LowOp &Op = Block.Ops[OpIndex];
        std::optional<ModuleAddressFacts> MemoryEffectOutput;
        const LowMemoryOperandView Memory = lowMemoryOperands(Op);
        const bool IsMemoryEffect =
            Op.Opcode == NdOp::LOAD || Op.Opcode == NdOp::STORE ||
            Op.Opcode == NdOp::ATOMIC_XCHG || Op.Opcode == NdOp::ATOMIC_ADD ||
            Op.Opcode == NdOp::ATOMIC_CMPXCHG;
        if (IsMemoryEffect && !Memory.Complete)
          return false;
        if (Img.Arch == Arch::AArch64 &&
            lowAArch64AddressLoadShapeIsValid(Op)) {
          // The original checked load observes its address without publishing
          // it or clobbering ABI argument registers. Keep both operand roots
          // as a conservative read footprint; neither the effective address
          // nor the loaded value is an unchecked base-plus-index expression.
          ModuleAddressFacts Address =
              factsFor(Op.Inputs[kAddressMemoryBaseOperand + 1]);
          if (!mergeFacts(Address,
                          factsFor(Op.Inputs[kAddressMemoryIndexOperand + 1])))
            return false;
          reclassifyExactRoots(Address);
          Address.Imprecise = true;
          if (!addUse(Op, ModuleAddressUse::Kind::Load, Address,
                      Op.Output.Size))
            return false;
          if (!Address.FrameOffsets.empty()) {
            // A dynamic checked address can read any retained private spill.
            // Preserve their possible pointee roots without claiming an exact
            // address, frame-cell identity, or successful access. A later use
            // of the loaded pointer must still expose a writable table.
            ModuleAddressFacts Loaded;
            for (const auto &[Key, Stored] :
                 entriesIn(State, VnodeSpace::STACK))
              if (std::get<2>(Key) != 0 && !mergeFacts(Loaded, Stored))
                return false;
            reclassifyExactRoots(Loaded);
            Loaded.ExactValues.clear();
            Loaded.FrameCellEscaped = false;
            Loaded.FrameCellPresent = false;
            Loaded.FrameCellOutgoingSeed = false;
            Loaded.MayBeNonFrame = true;
            Loaded.Imprecise = true;
            MemoryEffectOutput = std::move(Loaded);
          }
        } else if (Op.Opcode == NdOp::LOAD && Memory.Complete) {
          if (!addUse(Op, ModuleAddressUse::Kind::Load,
                      factsFor(*Memory.Address), Memory.AccessSize))
            return false;
        } else if (Op.Opcode == NdOp::STORE && Memory.Complete) {
          // Writing through a table-derived pointer invalidates the static
          // index-to-target mapping.  Storing the pointer value merely exposes
          // it to arbitrary downstream indexing; keep these effects separate
          // so module arbitration can reject the former table while only
          // retaining mirrors for the latter.
          const ModuleAddressFacts Destination = factsFor(*Memory.Address);
          const ModuleAddressFacts StoredValue = factsFor(*Memory.StoredValue);
          const bool EscapedPrivateCell =
              Destination.isPrivateFrameOnly() &&
              frameMemoryMayHaveEscaped(Destination, Memory.AccessSize);
          const bool StoredValueEscapes =
              !Destination.isPrivateFrameOnly() || EscapedPrivateCell;
          if (StoredValueEscapes &&
              !escapeFrameAddress(Op, StoredValue, TRI.PointerSize))
            return false;
          eraseFrameMemory(Destination, Memory.AccessSize);
          if (Destination.isPrivateFrameOnly()) {
            if (EscapedPrivateCell) {
              // Even an externally aliased cell records a concrete write at
              // the current call-site stack coordinate.  Its contents remain
              // unknown, but the presence marker is needed to advance over a
              // scalar ABI slot and inspect later stack arguments.
              storeFrameMemory(*Destination.FrameOffsets.begin(),
                               Memory.AccessSize, ModuleAddressFacts{}, true);
            } else {
              storeFrameMemory(*Destination.FrameOffsets.begin(),
                               Memory.AccessSize, StoredValue);
            }
          }
          if (!addUse(Op, ModuleAddressUse::Kind::WriteThrough, Destination,
                      Memory.AccessSize) ||
              (StoredValueEscapes &&
               !addUse(Op, ModuleAddressUse::Kind::PointerEscape, StoredValue)))
            return false;
        } else if ((Op.Opcode == NdOp::ATOMIC_XCHG ||
                    Op.Opcode == NdOp::ATOMIC_ADD ||
                    Op.Opcode == NdOp::ATOMIC_CMPXCHG) &&
                   Memory.Complete) {
          const ModuleAddressFacts Destination = factsFor(*Memory.Address);
          const ModuleAddressFacts StoredValue = factsFor(*Memory.StoredValue);
          const uint16_t AccessSize = Memory.AccessSize;
          ModuleAddressFacts OldValue =
              loadFrameMemory(Destination, AccessSize);
          if (OldValue.hasState())
            MemoryEffectOutput = OldValue;
          const bool EscapedPrivateCell =
              Destination.isPrivateFrameOnly() &&
              frameMemoryMayHaveEscaped(Destination, AccessSize);
          const bool StoredValueEscapes =
              !Destination.isPrivateFrameOnly() || EscapedPrivateCell;
          if (StoredValueEscapes &&
              !escapeFrameAddress(Op, StoredValue, TRI.PointerSize))
            return false;
          eraseFrameMemory(Destination, AccessSize);

          if (Destination.isPrivateFrameOnly() && !EscapedPrivateCell) {
            const int64_t Offset = *Destination.FrameOffsets.begin();
            if (Op.Opcode == NdOp::ATOMIC_XCHG) {
              ModuleAddressFacts NewValue = StoredValue;
              storeFrameMemory(Offset, AccessSize, std::move(NewValue));
            } else if (Op.Opcode == NdOp::ATOMIC_CMPXCHG) {
              ModuleAddressFacts Possible = OldValue;
              if (!mergeFacts(Possible, StoredValue))
                return false;
              Possible.Imprecise = true;
              storeFrameMemory(Offset, AccessSize, std::move(Possible));
            }
            // ATOMIC_ADD transforms the cell value; after the kill above no
            // exact frame-memory fact may survive.
          }
          if (!addUse(Op, ModuleAddressUse::Kind::WriteThrough, Destination,
                      AccessSize))
            return false;
          if (StoredValueEscapes &&
              !addUse(Op, ModuleAddressUse::Kind::PointerEscape, StoredValue))
            return false;
        } else if (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL ||
                   (Op.Opcode == NdOp::INTRINSIC &&
                    !lowIntrinsicHasOnlyValueEffects(Op)) ||
                   Op.Opcode == NdOp::RETURN) {
          if (Op.Opcode == NdOp::RETURN) {
            ModuleAddressFacts Returned;
            std::vector<ModuleAddressFacts> EscapedFrameAddresses;
            bool HasExplicitReturnValue = false;
            auto mergeReturnValue = [&](const ModuleAddressFacts &Value) {
              ModuleAddressFacts NonFrameValue = Value;
              NonFrameValue.FrameOffsets.clear();
              NonFrameValue.FrameCellEscaped = false;
              if (!mergeFacts(Returned, NonFrameValue))
                return false;
              if (!Value.FrameOffsets.empty()) {
                const ModuleAddressFacts Contents =
                    loadFrameMemory(Value, TRI.PointerSize);
                if (!mergeFacts(Returned, Contents))
                  return false;
                EscapedFrameAddresses.push_back(Value);
              }
              return true;
            };
            for (uint8_t I = 0; I < Op.NumInputs; ++I) {
              const NdVar &Input = Op.Inputs[I];
              if (Input.isReg() && TRI.isLinkRegister(Input.Offset))
                continue;
              HasExplicitReturnValue = true;
              if (!mergeReturnValue(factsFor(Input)))
                return false;
            }
            if (!HasExplicitReturnValue) {
              // ARM/AArch64 RETURN carries the control-flow target (LR/X30),
              // not the returned value.  In that representation recover the
              // ordinary scalar ABI result explicitly.  Do not treat every
              // register that *could* participate in a multi-register struct
              // return as live: without a signature that made i386 EDX and
              // AArch64 X1 false pointer escapes whenever they merely held a
              // stale unrelated value at an ordinary scalar return.
              for (const auto &[Key, Facts] :
                   entriesIn(State, VnodeSpace::REG)) {
                const auto &[Space, Offset, Size] = Key;
                (void)Space;
                (void)Size;
                const bool IsPrimaryIntegerReturn = Offset == TRI.IntReturnReg;
                const bool IsPrimaryFPReturn =
                    TRI.FPReturnReg != 0 && Offset == TRI.FPReturnReg;
                if ((IsPrimaryIntegerReturn || IsPrimaryFPReturn) &&
                    !mergeReturnValue(Facts))
                  return false;
              }
            }
            if (RecordedUses && ReturnedCode) {
              std::set<va_t> &Targets =
                  ReturnedCode->TargetsByFunction[FuncIndex];
              LowCxxContinuationExitEvidence Occurrence;
              Occurrence.ReturnAddr = Op.Addr;
              Occurrence.ReturnSeq = Op.Seq;
              Occurrence.Complete =
                  !Returned.Imprecise && !Returned.ExactValuesWidened;
              if (Occurrence.ReturnAddr == InvalidVA ||
                  Occurrence.ReturnSeq < 0)
                return false;
              if (Occurrence.Complete && AdditionalExactRoots) {
                for (const ModuleAddressAnchor &Exact : Returned.ExactValues) {
                  if (AdditionalExactRoots->count(Exact.Address) == 0) {
                    Occurrence.Complete = false;
                    break;
                  }
                  Occurrence.Targets.push_back(Exact.Address);
                }
              }
              if (!Occurrence.Complete) {
                ReturnedCode->CompleteByFunction[FuncIndex] = false;
                Targets.clear();
                Occurrence.Targets.clear();
              } else if (ReturnedCode->CompleteByFunction[FuncIndex]) {
                Targets.insert(Occurrence.Targets.begin(),
                               Occurrence.Targets.end());
              }
              std::sort(Occurrence.Targets.begin(), Occurrence.Targets.end());
              Occurrence.Targets.erase(std::unique(Occurrence.Targets.begin(),
                                                   Occurrence.Targets.end()),
                                       Occurrence.Targets.end());
              if (!ReturnedOccurrences
                       .emplace(std::make_pair(Occurrence.ReturnAddr,
                                               Occurrence.ReturnSeq),
                                std::move(Occurrence))
                       .second)
                return false;
            }
            if (!addUse(Op, ModuleAddressUse::Kind::PointerEscape, Returned))
              return false;
            for (const ModuleAddressFacts &Address : EscapedFrameAddresses)
              if (!markFrameMemoryEscaped(Address, TRI.PointerSize))
                return false;
            for (const ModuleAddressFacts &Address : EscapedFrameAddresses)
              eraseFrameMemory(Address, TRI.PointerSize);
          }
          if (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL ||
              Op.Opcode == NdOp::INTRINSIC) {
            if (RecordedUses && LocalUnwind && Op.Opcode == NdOp::CALL &&
                Op.NumInputs != 0 && Op.Inputs[0].isConst() &&
                Img.Arch == Arch::X64 && Img.Format == BinaryFormat::COFF) {
              const va_t CalleeVA = normalizeCodeAddress(
                  static_cast<va_t>(Op.Inputs[0].Offset), Img.Arch, Img.Mode);
              const Import *Imported = Img.findImportStubAt(CalleeVA);
              if (Imported &&
                  stripLeadingUnderscores(Imported->Name) == "local_unwind") {
                LocalUnwind->SawCallByFunction[FuncIndex] = true;
                if (TRI.Win64ParamRegs.size() < 2 || TRI.PointerSize == 0) {
                  LocalUnwind->CompleteByFunction[FuncIndex] = false;
                } else {
                  const ModuleAddressFacts Continuation = factsFor(
                      NdVar::reg(TRI.Win64ParamRegs[1], TRI.PointerSize));
                  if (Continuation.Imprecise ||
                      Continuation.ExactValuesWidened ||
                      Continuation.ExactValues.size() != 1 ||
                      !AdditionalExactRoots) {
                    LocalUnwind->CompleteByFunction[FuncIndex] = false;
                  } else {
                    const va_t Target =
                        Continuation.ExactValues.begin()->Address;
                    if (AdditionalExactRoots->count(Target) == 0)
                      LocalUnwind->CompleteByFunction[FuncIndex] = false;
                    else {
                      LocalUnwind->TargetsByFunction[FuncIndex].insert(Target);
                      const bool KnownRuntime =
                          coff_loader::isX86ContinuationRuntimeProvider(
                              Imported->Module);
                      if (KnownRuntime) {
                        // The complete decoded LowIR proof owns equality of
                        // the physical SP and the frame argument. The address
                        // inventory owns only continuation identity and uses;
                        // its independently rooted frame lattice cannot veto
                        // or replace a checked runtime-activation coordinate.
                        if (!SEHFrameProof)
                          SEHFrameProof = proveLowSEHFrames(
                              Func, TRI, SEHFrameWorkRemaining, &SEHCallees,
                              &SEHRuntimeMemory);
                        const auto Proof =
                            SEHFrameProof->Calls.find({Op.Addr, Op.Seq});
                        if (Proof != SEHFrameProof->Calls.end())
                          LocalUnwind->SameFrameCallsByFunction[FuncIndex]
                              .push_back({Target, Op.Addr, Op.Seq, CalleeVA,
                                          Proof->second,
                                          SEHFrameProof->DependencyDigest,
                                          SEHFrameProof->CalleeDependencies,
                                          SEHFrameProof->UsesRuntimeMemory});
                      }
                    }
                  }
                }
              }
            }
            // LowIR calls do not enumerate ABI argument registers as explicit
            // operands.  Any authenticated address still live in the register
            // state can therefore be handed to an opaque callee; for writable
            // table storage that is mutation evidence, not merely a mirror
            // consumer.  Combining the facts into one use keeps this scan
            // bounded independently of register count.
            ModuleAddressFacts ImplicitArguments;
            std::vector<ModuleAddressFacts> EscapedFrameAddresses;
            auto mergeCallArgument = [&](const ModuleAddressFacts &Facts) {
              ModuleAddressFacts NonFrameFacts = Facts;
              NonFrameFacts.FrameOffsets.clear();
              NonFrameFacts.FrameCellEscaped = false;
              NonFrameFacts.FrameCellPresent = false;
              NonFrameFacts.FrameCellOutgoingSeed = false;
              if (!mergeFacts(ImplicitArguments, NonFrameFacts))
                return false;
              if (!Facts.FrameOffsets.empty()) {
                ModuleAddressFacts Contents =
                    loadFrameMemory(Facts, TRI.PointerSize);
                if (!mergeFacts(ImplicitArguments, Contents))
                  return false;
                EscapedFrameAddresses.push_back(Facts);
              }
              return true;
            };
            // Unlike CALL, an intrinsic carries its data operands explicitly.
            // A memory/opaque intrinsic may observe an address in a temporary
            // even when no ABI argument register contains that address.
            if (Op.Opcode == NdOp::INTRINSIC)
              for (uint8_t I = 1; I < Op.NumInputs; ++I)
                if (!mergeCallArgument(factsFor(Op.Inputs[I])))
                  return false;
            for (const auto &[Key, Facts] : entriesIn(State, VnodeSpace::REG)) {
              if (TRI.regToArgIdx(std::get<1>(Key),
                                  Img.abiFormat() == BinaryFormat::COFF) < 0)
                continue;
              if (!mergeCallArgument(Facts))
                return false;
            }

            // Stack-passed arguments form a bounded contiguous call-site
            // window.  SysV/AAPCS/i386 start at the current SP; Win64 starts
            // after the 32-byte shadow space.  Scalar cells are retained as
            // markers so a pointer in the second or later stack argument is
            // not hidden by an untracked first scalar argument.
            const ModuleAddressFacts CurrentSP = factsFor(NdVar::reg(
                TRI.StackPointer, static_cast<uint16_t>(TRI.PointerSize)));
            if (CurrentSP.isPrivateFrameOnly()) {
              int64_t Cursor = *CurrentSP.FrameOffsets.begin();
              if (Img.Arch == Arch::X64 &&
                  Img.abiFormat() == BinaryFormat::COFF) {
                int64_t AfterShadow = 0;
                if (llvm::AddOverflow(Cursor, int64_t{32}, AfterShadow))
                  return false;
                Cursor = AfterShadow;
              }
              for (size_t CellIndex = 0;
                   CellIndex < ::neverd::limits::pipeline::
                                   kPipelineLowIRMaxOutgoingStackCells;
                   ++CellIndex) {
                const ModuleAddressFacts *Cell = nullptr;
                uint16_t CellSize = 0;
                for (const auto &[Key, Facts] :
                     entriesIn(State, VnodeSpace::STACK)) {
                  const auto &[Space, RawOffset, StoredSize] = Key;
                  (void)Space;
                  if (StoredSize == 0 || !Facts.FrameCellPresent ||
                      static_cast<int64_t>(RawOffset) != Cursor)
                    continue;
                  if (Cell != nullptr)
                    return false;
                  Cell = &Facts;
                  CellSize = StoredSize;
                }
                if (!Cell)
                  break;
                if (CellIndex == 0 && !Cell->FrameCellOutgoingSeed)
                  break;
                if (!mergeCallArgument(*Cell))
                  return false;
                uint64_t SlotAdvance = CellSize;
                if (TRI.PointerSize != 0) {
                  const uint64_t Remainder = SlotAdvance % TRI.PointerSize;
                  if (Remainder != 0) {
                    const uint64_t Padding = TRI.PointerSize - Remainder;
                    if (Padding > UINT64_MAX - SlotAdvance)
                      return false;
                    SlotAdvance += Padding;
                  }
                  SlotAdvance =
                      std::max<uint64_t>(SlotAdvance, TRI.PointerSize);
                }
                if (SlotAdvance > static_cast<uint64_t>(INT64_MAX))
                  return false;
                int64_t Next = 0;
                if (llvm::AddOverflow(Cursor, static_cast<int64_t>(SlotAdvance),
                                      Next))
                  return false;
                Cursor = Next;
              }
            } else {
              // An ambiguous/pivoted call SP cannot establish a finite ABI
              // window.  Preserve soundness locally by treating every
              // address-bearing frame cell as a possible stack argument;
              // this does not poison unrelated functions or tables.
              for (const auto &[Key, Facts] :
                   entriesIn(State, VnodeSpace::STACK))
                if (std::get<2>(Key) != 0 && Facts.FrameCellPresent &&
                    !mergeCallArgument(Facts))
                  return false;
            }
            if (!addUse(Op, ModuleAddressUse::Kind::PointerEscape,
                        ImplicitArguments))
              return false;

            // Once a callee sees a private-frame address it may overwrite the
            // pointee.  Kill only the escaped cells; unrelated private spills
            // remain stable across the call.
            for (const ModuleAddressFacts &Address : EscapedFrameAddresses)
              if (!markFrameMemoryEscaped(Address, TRI.PointerSize))
                return false;
            for (const ModuleAddressFacts &Address : EscapedFrameAddresses)
              eraseFrameMemory(Address, TRI.PointerSize);

            // A call is a reaching-definition barrier for caller-saved
            // registers and instruction-local temporaries.  Retaining their
            // pre-call address facts would later authenticate a stale table
            // base/index.  Preserve only byte views fully covered by the ABI's
            // nonvolatile ranges.
            for (auto It = State.begin(); It != State.end();) {
              const auto &[Space, Offset, Size] = It->first;
              bool Preserved = false;
              if (Space == VnodeSpace::REG && Size <= InvalidVA - Offset)
                for (const TargetRegisterRange &Range : PreservedRanges)
                  if (Range.Bytes <= InvalidVA - Range.Offset &&
                      Offset >= Range.Offset &&
                      Offset + Size <= Range.Offset + Range.Bytes) {
                    Preserved = true;
                    break;
                  }
              if (Space == VnodeSpace::STACK)
                Preserved = true;
              if (Preserved)
                ++It;
              else
                It = State.erase(It);
            }
          }
        }

        if (Op.Output.Size != 0 && !Op.Output.isConst()) {
          ModuleAddressFacts Output;
          switch (Op.Opcode) {
          case NdOp::COPY:
            if (Op.NumInputs != 0) {
              Output = coerceFacts(factsFor(Op.Inputs[0]), Op.Output.Size);
              if (!Output.FrameOffsets.empty() &&
                  Op.Inputs[0].Size != Op.Output.Size)
                Output.MayBeNonFrame = true;
            }
            break;
          case NdOp::INT_ZEXT:
            if (Op.NumInputs != 0) {
              Output = coerceFacts(factsFor(Op.Inputs[0]), Op.Output.Size);
              const bool CanonicalGuestAddressWidening =
                  Op.Inputs[0].Size == TRI.PointerSize &&
                  Op.Output.Size == sizeof(va_t) &&
                  Op.Output.Space == VnodeSpace::TEMP;
              if (!Output.FrameOffsets.empty() &&
                  !CanonicalGuestAddressWidening)
                Output.MayBeNonFrame = true;
            }
            break;
          case NdOp::INT_SEXT:
            if (Op.NumInputs != 0) {
              Output = signExtendFacts(factsFor(Op.Inputs[0]),
                                       Op.Inputs[0].Size, Op.Output.Size);
              if (!Output.FrameOffsets.empty())
                Output.MayBeNonFrame = true;
            }
            break;
          case NdOp::SUBBYTES:
            if (Op.NumInputs != 0) {
              Output = factsFor(Op.Inputs[0]);
              if (Op.NumInputs >= 2 && Op.Inputs[1].isConst() &&
                  Op.Inputs[1].Offset == 0) {
                Output = coerceFacts(std::move(Output), Op.Output.Size);
              } else if (Output.hasState()) {
                // A non-zero or dynamic lane extraction is not an address-
                // preserving cast.  Keep the authenticated source anchor as a
                // may-alias root, but never manufacture an exact address from
                // the low bytes; module arbitration will preserve the complete
                // owning object.
                reclassifyExactRoots(Output);
                Output.ExactValues.clear();
                Output.MayBeNonFrame = true;
                Output.Imprecise = true;
              }
            }
            break;
          case NdOp::SELECT: {
            bool MissingArm = false;
            for (uint8_t I = 1; I < Op.NumInputs; ++I) {
              ModuleAddressFacts Arm = factsFor(Op.Inputs[I]);
              MissingArm |= !Arm.hasState();
              if (!mergeFacts(Output, Arm))
                return false;
            }
            Output.Imprecise |= MissingArm;
            Output.MayBeNonFrame |= MissingArm;
            break;
          }
          case NdOp::INT_ADD:
          case NdOp::INT_SUB:
            if (Op.NumInputs >= 2) {
              ModuleAddressFacts Left = factsFor(Op.Inputs[0]);
              ModuleAddressFacts Right = factsFor(Op.Inputs[1]);
              if (Left.hasState()) {
                if (Op.Inputs[1].isConst())
                  adjustFrameOffsets(Left, Op.Inputs[1].Offset, Op.Output.Size,
                                     Op.Opcode == NdOp::INT_SUB);
                else if (!Left.FrameOffsets.empty()) {
                  Left.FrameOffsets.clear();
                  Left.MayBeNonFrame = true;
                  Left.Imprecise = true;
                }
                if (!mergeFacts(Output, Left))
                  return false;
                Output.ExactValues.clear();
                if (Op.Inputs[1].isConst() && !Output.ExactValuesWidened) {
                  for (ModuleAddressAnchor Anchor : Left.ExactValues) {
                    Anchor.Address = coerceUnsigned(
                        Op.Opcode == NdOp::INT_ADD
                            ? Anchor.Address + Op.Inputs[1].Offset
                            : Anchor.Address - Op.Inputs[1].Offset,
                        Op.Output.Size);
                    Output.ExactValues.insert(Anchor);
                  }
                  reclassifyExactRoots(Output);
                } else {
                  Output.Imprecise = true;
                  Output.MayBeNonFrame |= !Output.FrameOffsets.empty();
                }
              }
              if (Op.Opcode == NdOp::INT_ADD && Right.hasState()) {
                if (Op.Inputs[0].isConst())
                  adjustFrameOffsets(Right, Op.Inputs[0].Offset, Op.Output.Size,
                                     false);
                else if (!Right.FrameOffsets.empty()) {
                  Right.FrameOffsets.clear();
                  Right.MayBeNonFrame = true;
                  Right.Imprecise = true;
                }
                if (!mergeFacts(Output, Right))
                  return false;
                if (Op.Inputs[0].isConst() && !Output.ExactValuesWidened) {
                  Output.ExactValues.clear();
                  for (ModuleAddressAnchor Anchor : Right.ExactValues) {
                    Anchor.Address = coerceUnsigned(
                        Anchor.Address + Op.Inputs[0].Offset, Op.Output.Size);
                    Output.ExactValues.insert(Anchor);
                  }
                  reclassifyExactRoots(Output);
                } else {
                  Output.ExactValues.clear();
                  Output.Imprecise = true;
                  Output.MayBeNonFrame |= !Output.FrameOffsets.empty();
                }
              }
            }
            break;
          case NdOp::LOAD:
            if (Memory.Complete)
              Output =
                  loadFrameMemory(factsFor(*Memory.Address), Memory.AccessSize);
            break;
          case NdOp::STORE:
          case NdOp::ATOMIC_XCHG:
          case NdOp::ATOMIC_ADD:
          case NdOp::ATOMIC_CMPXCHG:
            if (MemoryEffectOutput)
              Output = *MemoryEffectOutput;
            break;
          case NdOp::CALL:
          case NdOp::INDIR_CALL:
          case NdOp::RETURN:
            // Reading an address from memory does not preserve the address of
            // the memory field in the result; observable effects were recorded
            // above and do not forward operand provenance into their outputs.
            break;
          case NdOp::INTRINSIC:
            if (MemoryEffectOutput) {
              Output = *MemoryEffectOutput;
              break;
            }
            if (!lowIntrinsicHasOnlyValueEffects(Op))
              break;
            [[fallthrough]];
          default:
            // A pure, unmodelled value transform is not itself an externally
            // observable pointer escape.  Carry authenticated object roots
            // forward as an imprecise result so a later STORE/CALL/RETURN still
            // vetoes suppression, but do not mark frame memory externally
            // aliased merely because flag arithmetic compares or hashes SP/FP.
            for (uint8_t I = 0; I < Op.NumInputs; ++I) {
              ModuleAddressFacts Input = factsFor(Op.Inputs[I]);
              Input.FrameOffsets.clear();
              Input.FrameCellEscaped = false;
              Input.FrameCellPresent = false;
              Input.FrameCellOutgoingSeed = false;
              if (Input.empty())
                continue;
              reclassifyExactRoots(Input);
              Input.ExactValues.clear();
              Input.MayBeNonFrame = true;
              Input.Imprecise = true;
              if (!mergeFacts(Output, Input))
                return false;
            }
            break;
          }
          if (auto Materialized =
                  ExactOutputMaterializations.find({Op.Addr, Op.Seq});
              Materialized != ExactOutputMaterializations.end()) {
            const RelocatedInstructionAddressOccurrence &Occurrence =
                Materialized->second;
            if (Op.Opcode != Occurrence.OutputOpcode ||
                Op.Output != Occurrence.OutputWitness)
              return false;
            const ModuleAddressAnchor Anchor{Occurrence.TargetVA,
                                             Occurrence.TargetOwnerVA,
                                             ModuleAddressOwnerKind::Container};
            ModuleAddressFacts Certified;
            Certified.ExactValues.insert(Anchor);
            initializeExactRoots(Certified);
            Certified.MayBeNonFrame = !Certified.empty();
            if (!Occurrence.OutputMayDepend) {
              Output = std::move(Certified);
            } else {
              Certified.ExactValues.clear();
              Certified.Imprecise = true;
              if (!mergeFacts(Output, Certified))
                return false;
            }
          }
          if (!Output.hasState() && Op.Output.Space == VnodeSpace::REG &&
              (TRI.isStackPointer(Op.Output.Offset) ||
               TRI.isFramePointer(Op.Output.Offset))) {
            // An unmodelled SP/FP definition is a persistent epoch barrier.  Do
            // not erase it: erasure would let a later lookup reseed the
            // physical register as the original frame base.
            Output.MayBeNonFrame = true;
            Output.Imprecise = true;
          }
          std::optional<LowValueKey> WideOutputKey;
          ModuleAddressFacts WideOutput;
          if (Op.Output.Space == VnodeSpace::REG) {
            if (std::optional<RegisterLane> Written =
                    registerLane(Op.Output.Offset, Op.Output.Size)) {
              const bool ZeroExtends =
                  TRI.writeZeroExtends(Op.Output.Offset, Op.Output.Size) &&
                  Written->WideSize > Op.Output.Size;
              const bool PartialWrite =
                  !ZeroExtends &&
                  (Written->Begin != 0 || Written->End != Written->WideSize);
              if (ZeroExtends) {
                // A narrow architectural write first truncates to the written
                // lane and only then zero-extends into the full register.  A
                // frame offset cannot survive that value-changing round trip:
                // ESP/WSP derived from the old SP is a new, non-frame epoch.
                if (!Output.FrameOffsets.empty()) {
                  Output.FrameOffsets.clear();
                  Output.MayBeNonFrame = true;
                  Output.Imprecise = true;
                }
                Written->Begin = 0;
                Written->End = Written->WideSize;
                WideOutputKey = LowValueKey{
                    VnodeSpace::REG, Written->WideOffset, Written->WideSize};
                WideOutput = coerceFacts(Output, Written->WideSize);
              } else if (PartialWrite) {
                // A byte/word write does not define the rest of its containing
                // architectural register.  Retain every authenticated root
                // that may survive in an uncovered lane, and include the new
                // lane as another may-source.  The combined full-width value is
                // no longer an exact pointer (nor an affine SP/FP epoch), but
                // dropping those roots would let `movb %dil,%dil` or an AH
                // write erase a live table pointer before an opaque call.
                ModuleAddressFacts Combined;
                for (const auto &[Key, ExistingFacts] :
                     entriesIn(State, VnodeSpace::REG)) {
                  const auto &[Space, Offset, Size] = Key;
                  (void)Space;
                  const std::optional<RegisterLane> Existing =
                      registerLane(Offset, Size);
                  if (!Existing ||
                      Existing->WideOffset != Written->WideOffset ||
                      Existing->WideSize != Written->WideSize ||
                      (Existing->Begin >= Written->Begin &&
                       Existing->End <= Written->End))
                    continue;
                  ModuleAddressFacts Surviving = ExistingFacts;
                  reclassifyExactRoots(Surviving);
                  Surviving.ExactValues.clear();
                  if (!Surviving.FrameOffsets.empty()) {
                    Surviving.FrameOffsets.clear();
                    Surviving.MayBeNonFrame = true;
                  }
                  Surviving.FrameCellEscaped = false;
                  Surviving.FrameCellPresent = false;
                  Surviving.FrameCellOutgoingSeed = false;
                  Surviving.Imprecise = true;
                  if (!mergeFacts(Combined, Surviving))
                    return false;
                }
                ModuleAddressFacts NewLane = Output;
                reclassifyExactRoots(NewLane);
                NewLane.ExactValues.clear();
                if (!NewLane.FrameOffsets.empty()) {
                  NewLane.FrameOffsets.clear();
                  NewLane.MayBeNonFrame = true;
                }
                NewLane.FrameCellEscaped = false;
                NewLane.FrameCellPresent = false;
                NewLane.FrameCellOutgoingSeed = false;
                if (NewLane.hasState()) {
                  NewLane.Imprecise = true;
                  if (!mergeFacts(Combined, NewLane))
                    return false;
                }
                if (Combined.hasState()) {
                  Combined.Imprecise = true;
                  WideOutputKey = LowValueKey{
                      VnodeSpace::REG, Written->WideOffset, Written->WideSize};
                  WideOutput = std::move(Combined);
                }
              }
              for (auto It = State.begin(); It != State.end();) {
                if (std::get<0>(It->first) != VnodeSpace::REG) {
                  ++It;
                  continue;
                }
                const std::optional<RegisterLane> Existing = registerLane(
                    std::get<1>(It->first), std::get<2>(It->first));
                if (Existing && registerLanesOverlap(*Written, *Existing))
                  It = State.erase(It);
                else
                  ++It;
              }
            }
          } else {
            State.erase(lowValueKey(Op.Output));
          }
          if (Output.hasState())
            State[lowValueKey(Op.Output)] = std::move(Output);
          if (WideOutputKey && WideOutput.hasState())
            State[*WideOutputKey] = std::move(WideOutput);
        }
        if (!finishInstructionGuard(OpIndex + 1))
          return false;
      }
      if (GuardSkipState)
        return false;
      if (!RegisterProjectionComplete)
        return false;
      Result = std::move(State);
      return true;
    };

    std::vector<size_t> Queue;
    std::vector<bool> Queued(BlockCount, false);
    std::vector<bool> Reached(BlockCount, false);
    std::vector<bool> Processed(BlockCount, false);
    auto queueRoot = [&](va_t Root) {
      for (size_t I = 0; I < BlockCount; ++I) {
        const LowBlock &Block = Func.Blocks[I];
        if (Root < Block.StartAddr || Root >= Block.EndAddr || Queued[I])
          continue;
        if (!consumeFixpointWork())
          return false;
        Queued[I] = true;
        Reached[I] = true;
        Queue.push_back(I);
        return true;
      }
      return true;
    };
    const std::set<va_t> &AnalysisRoots =
        EdgeTraversal == ModuleAddressEdgeTraversal::OrdinaryOnly
            ? Func.OrdinaryModuleAnalysisRoots
            : Func.ModuleAnalysisRoots;
    if (!AnalysisRoots.empty()) {
      for (va_t Root : AnalysisRoots)
        if (!queueRoot(Root))
          return false;
    } else {
      // Hand-built/legacy LowIR predating explicit root metadata still has a
      // single durable function entry.  Falling back to every no-predecessor
      // block would recreate the dead-root self-bootstrap above.
      if (!queueRoot(Func.Entry))
        return false;
    }
    for (size_t Cursor = 0; Cursor < Queue.size(); ++Cursor) {
      const size_t BlockIndex = Queue[Cursor];
      Queued[BlockIndex] = false;
      if (!consumeFixpointWork())
        return false;
      ModuleAddressState Input;
      if (!mergePredecessors(Func.Blocks[BlockIndex], Input))
        return failCollect("merge-preds", FuncIndex,
                           Func.Blocks[BlockIndex].StartAddr);
      // The graph and transfer rules are frozen for this solve. Identical
      // incoming facts already have a complete output; final use recording
      // remains a separate transfer below.
      if (Processed[BlockIndex] && Input == InStates[BlockIndex])
        continue;
      ModuleAddressState Output;
      if (!transfer(Func.Blocks[BlockIndex], Input, nullptr, Output))
        return failCollect("transfer-fixpoint", FuncIndex,
                           Func.Blocks[BlockIndex].StartAddr);
      const bool FirstVisit = !Processed[BlockIndex];
      Processed[BlockIndex] = true;
      // Recording needs the latest input even when this block kills every
      // changed fact. Only a changed output can affect a successor.
      InStates[BlockIndex] = std::move(Input);
      if (!FirstVisit && Output == OutStates[BlockIndex])
        continue;
      OutStates[BlockIndex] = std::move(Output);
      auto queueSucc = [&](int SuccId) {
        auto Pos = PositionForId.find(SuccId);
        if (Pos != PositionForId.end() && !Queued[Pos->second]) {
          if (!consumeFixpointWork())
            return false;
          Queued[Pos->second] = true;
          Reached[Pos->second] = true;
          Queue.push_back(Pos->second);
        }
        return true;
      };
      for (int Succ : Func.Blocks[BlockIndex].Succs)
        if (!queueSucc(Succ))
          return false;
      if (EdgeTraversal == ModuleAddressEdgeTraversal::OrdinaryAndExceptional)
        for (const ExceptionalEdge &Edge :
             Func.Blocks[BlockIndex].ExceptionalSuccs)
          if (!queueSucc(Edge.BlockId))
            return false;
    }
    for (size_t I = 0; I < BlockCount; ++I) {
      if (!Reached[I])
        continue;
      ModuleAddressState Ignored;
      // The recording pass reads each input state for the last time.
      if (!transfer(Func.Blocks[I], std::move(InStates[I]), &Uses, Ignored))
        return failCollect("transfer-record", FuncIndex,
                           Func.Blocks[I].StartAddr);
    }
    if (ReturnedCode) {
      auto &Published = ReturnedCode->OccurrencesByFunction[FuncIndex];
      Published.reserve(ReturnedOccurrences.size());
      for (auto &[Key, Occurrence] : ReturnedOccurrences) {
        (void)Key;
        Published.push_back(std::move(Occurrence));
      }
    }
  }
  return true;
}

/// The exception records whose code range begins at \p Entry, in no
/// particular order.  The start index, sorted by start, finds them when it
/// lists every record; an image holds tens of thousands.
llvm::SmallVector<const ExceptionFunction *, 2>
exceptionRecordsStartingAt(const ExceptionInfo &Info, va_t Entry) {
  llvm::SmallVector<const ExceptionFunction *, 2> Found;
  if (Info.FunctionIndex.size() != Info.Functions.size()) {
    for (const ExceptionFunction &F : Info.Functions)
      if (F.CodeRange.Begin == Entry)
        Found.push_back(&F);
    return Found;
  }
  const auto Starts = [&](size_t Index) {
    return Info.Functions[Index].CodeRange.Begin;
  };
  for (auto It = llvm::partition_point(
           Info.FunctionIndex,
           [&](size_t Index) { return Starts(Index) < Entry; });
       It != Info.FunctionIndex.end() && Starts(*It) == Entry; ++It)
    Found.push_back(&Info.Functions[*It]);
  return Found;
}

struct EHContinuationRootDiscovery {
  std::map<va_t, std::set<va_t>> RootsByOwner;
  std::map<va_t, std::vector<LowCxxContinuationEntryEvidence>> EntriesByOwner;
  std::map<va_t, std::vector<LowSEHLocalUnwindEvidence>> LocalUnwindsByOwner;
  std::vector<std::vector<LowCxxContinuationExitEvidence>>
      CxxContinuationExitsByFunction;
  std::vector<bool> CxxContinuationExitAnalysisCompleteByFunction;
  bool AnalysisComplete = true;
};

/// Route explicit FH4 continuations and recover the continuation values
/// returned by separated FH3 catch funclets.  A plain FH3 code reference is
/// deliberately insufficient: the source must be a complete catch
/// contribution declared by the target owner's try map, and bounded LowIR
/// dataflow must prove that the referenced address reaches an ordinary return
/// value.  The result stays pipeline-local so provisional evidence can be
/// discarded and recomputed after module arbitration.
} // namespace

namespace pipeline_detail {
std::vector<std::pair<va_t, va_t>>
fh4ContinuationCandidates(const BinaryImage &Img) {
  std::vector<std::pair<va_t, va_t>> Candidates;
  if (Img.Format != BinaryFormat::COFF)
    return Candidates;
  const auto IsFH4 = [](const ExceptionFunction &F) {
    return F.Kind == RuntimeFunctionKind::Primary &&
           F.ParseStatus == ExceptionParseStatus::Complete && F.Cxx &&
           F.Cxx->NativeEncoding == CxxExceptionInfo::Encoding::FH4;
  };
  // The functions of each FH4 function information record, in order: a
  // continuation names one of them.
  std::map<va_t, std::vector<const ExceptionFunction *>> ByFuncInfo;
  for (const ExceptionFunction &F : Img.ExceptionMetadata.Functions)
    if (IsFH4(F))
      ByFuncInfo[F.Cxx->NativeFuncInfoVA].push_back(&F);
  for (const ExceptionFunction &Declaring : Img.ExceptionMetadata.Functions) {
    if (!IsFH4(Declaring) || Declaring.Cxx->NativeFuncInfoVA == 0)
      continue;
    const std::vector<const ExceptionFunction *> &Sharing =
        ByFuncInfo.at(Declaring.Cxx->NativeFuncInfoVA);
    for (const CxxTryBlock &Try : Declaring.Cxx->TryBlocks)
      for (const CxxCatchHandler &Handler : Try.Handlers)
        for (va_t Target : Handler.ContinuationVAs) {
          Target = normalizeCodeAddress(Target, Img.Arch, Img.Mode);
          if (Declaring.CodeRange.contains(Target))
            continue;
          // The one function of the record that holds the target past its
          // start.
          const ExceptionFunction *Owner = nullptr;
          for (const ExceptionFunction *Candidate : Sharing) {
            if (Target <= Candidate->CodeRange.Begin ||
                Target >= Candidate->CodeRange.End)
              continue;
            if (Owner) {
              Owner = nullptr;
              break;
            }
            Owner = Candidate;
          }
          if (Owner && Img.hasExecutableCodeOwnerAt(Target))
            Candidates.emplace_back(Owner->CodeRange.Begin, Target);
        }
  }
  return Candidates;
}
} // namespace pipeline_detail

namespace {
EHContinuationRootDiscovery collectWindowsEHContinuationRoots(
    const BinaryImage &Img, const std::vector<LowFunc> &Funcs,
    const std::set<va_t> &FunctionEntries, std::optional<size_t> TestBudget,
    bool CompleteModule = true,
    const std::vector<std::pair<va_t, va_t>> *FH4Candidates = nullptr) {
  EHContinuationRootDiscovery Result;
  Result.CxxContinuationExitsByFunction.assign(Funcs.size(), {});
  Result.CxxContinuationExitAnalysisCompleteByFunction.assign(Funcs.size(),
                                                              false);
  if (Img.Format != BinaryFormat::COFF)
    return Result;

  auto isFunctionEntry = [&](va_t Addr) {
    return Img.isFunctionEntryCandidateAt(Addr) &&
           (FunctionEntries.count(Addr) != 0 ||
            Img.hasKnownFunctionEntryAt(Addr));
  };

  // A catch continuation an FH4 handler names in another function of its
  // record is a root of that function where it starts no function.
  std::vector<std::pair<va_t, va_t>> OwnFH4Candidates;
  if (!FH4Candidates)
    OwnFH4Candidates = pipeline_detail::fh4ContinuationCandidates(Img);
  for (const auto &[Owner, Target] :
       FH4Candidates ? *FH4Candidates : OwnFH4Candidates)
    if (isFunctionEntry(Owner) && !isFunctionEntry(Target))
      Result.RootsByOwner[Owner].insert(Target);

  // FH3 separated funclets return continuation addresses only in the x64
  // table ABI.  Other Windows targets either use registration records or
  // carry explicit continuation metadata handled above.
  if (Img.Arch != Arch::X64)
    return Result;

  auto exactFH3Record = [&](va_t Entry) -> const ExceptionFunction * {
    const ExceptionFunction *Match = nullptr;
    for (const ExceptionFunction *F :
         exceptionRecordsStartingAt(Img.ExceptionMetadata, Entry)) {
      if (F->Kind != RuntimeFunctionKind::Primary ||
          F->ParseStatus != ExceptionParseStatus::Complete || !F->Cxx ||
          F->Cxx->NativeEncoding != CxxExceptionInfo::Encoding::FH3)
        continue;
      if (Match)
        return nullptr;
      Match = F;
    }
    return Match;
  };
  std::vector<const ExceptionFunction *> FH3CatchSources(Funcs.size(), nullptr);
  bool HasFH3CatchSource = false;
  for (size_t FuncIndex = 0; FuncIndex < Funcs.size(); ++FuncIndex) {
    const ExceptionFunction *Source = exactFH3Record(Funcs[FuncIndex].Entry);
    if (!Source || !Source->Cxx->IsSeparated || !Source->Cxx->IsCatchFunclet)
      continue;
    FH3CatchSources[FuncIndex] = Source;
    HasFH3CatchSource = true;
  }
  std::set<va_t> CandidateTargets;
  for (const LowFunc &Func : Funcs)
    for (va_t Target : Func.CodeRefTargets) {
      Target = normalizeCodeAddress(Target, Img.Arch, Img.Mode);
      // Ordinary calls already name function entries.  Returned-code
      // dataflow is only for mid-function continuations (local-unwind /
      // FH3 catch).  Walking every call target on a `--func` C++ body
      // is the LowIR cost of moderately sized helpers.
      if (isFunctionEntry(Target) || !Img.hasExecutableCodeOwnerAt(Target))
        continue;
      CandidateTargets.insert(Target);
    }
  // A source-level local-unwind helper also consumes returned-code analysis,
  // even when the module has no separated FH3 catch contribution.  Conversely,
  // an FH3 catch with no address candidates must still publish a completed,
  // exact-empty occurrence snapshot rather than looking unanalysed.
  if (CandidateTargets.empty() && !HasFH3CatchSource)
    return Result;

  ReturnedCodeEvidence Returned;
  LocalUnwindContinuationEvidence LocalUnwind;
  std::vector<ModuleAddressUse> IgnoredUses;
  const std::vector<ModuleJumpTableOwner> NoTableOwners;
  ModuleEvidenceBudget Budget(TestBudget);
  // Ordinary catch returns and local-unwind call uses have different entry
  // domains. Preserve the ordinary-only returned-value proof. A separate
  // address inventory visits runtime activation roots from unknown state and
  // follows only ordinary edges; frame equality still comes from LowIR's
  // decoded-frame authority, never from this inventory or the runtime name.
  if (HasFH3CatchSource) {
    std::vector<ModuleAddressUse> ReturnedUses;
    if (!collectLowAddressUses(Img, Funcs, NoTableOwners, ReturnedUses, Budget,
                               TestBudget, &CandidateTargets, &Returned,
                               ModuleAddressEdgeTraversal::OrdinaryOnly)) {
      Result.AnalysisComplete = false;
      return Result;
    }
  }
  if (!collectLowAddressUses(
          Img, Funcs, NoTableOwners, IgnoredUses, Budget, TestBudget,
          &CandidateTargets, nullptr,
          ModuleAddressEdgeTraversal::IndependentlyRootedOrdinaryOnly,
          &LocalUnwind)) {
    Result.AnalysisComplete = false;
    return Result;
  }

  // A same-function unwind target is not an independent machine entry merely
  // because LEA takes its address. Certify every observable use before giving
  // it that narrower role. Relocation-backed pointers, foreign uses, widened
  // values, and calls with a different/unknown frame retain the ordinary role.
  const detail::AbsoluteRelocationRootIndex RelocationRoots(Img);
  std::set<va_t> InspectedEntries;
  for (const LowFunc &F : Funcs) {
    CompleteModule &= !F.Blocks.empty() && F.hasCompleteLiftCoverage();
    InspectedEntries.insert(F.Entry);
  }
  // Only a module, which a selection never is, can be complete: once it is
  // not, the image's other entries say nothing more.
  for (auto Entry = FunctionEntries.begin();
       CompleteModule && Entry != FunctionEntries.end(); ++Entry)
    if (!InspectedEntries.count(*Entry) && !Img.findImportStubAt(*Entry))
      CompleteModule = false;
  for (size_t I = 0; CompleteModule && I < Funcs.size(); ++I) {
    const LowFunc &F = Funcs[I];
    if (!F.ExceptionMetadata || !F.ExceptionMetadata->SEH ||
        !LocalUnwind.CompleteByFunction[I])
      continue;
    const auto &EH = *F.ExceptionMetadata;
    std::map<va_t, std::set<va_t>> RelocationSources;
    if (!RelocationRoots.collectSources(Img, EH.CodeRange.Begin,
                                        EH.CodeRange.End, RelocationSources))
      continue;
    std::map<va_t, std::vector<LowSEHLocalUnwindEvidence>> Candidates;
    for (const auto &Call : LocalUnwind.SameFrameCallsByFunction[I])
      if (Call.Target != F.Entry && EH.ownsCode(Call.Target) &&
          !isFunctionEntry(Call.Target) &&
          !RelocationSources.count(Call.Target))
        Candidates[Call.Target].push_back(Call);
    for (const auto &[Target, Calls] : Candidates) {
      const Section *Section = Img.getSectionFor(Target);
      const Segment *Segment = Img.getSegmentFor(Target);
      const va_t Container = Section   ? Section->VA
                             : Segment ? Segment->VA
                                       : InvalidVA;
      bool Exclusive = true;
      for (const auto &Use : IgnoredUses) {
        if (!Budget.consume()) {
          Result.AnalysisComplete = false;
          Result.LocalUnwindsByOwner.clear();
          return Result;
        }
        const bool Exact = llvm::any_of(Use.ExactAddresses, [&](const auto &A) {
          return A.Address == Target;
        });
        const bool May = llvm::any_of(Use.RootScopes, [&](const auto &Root) {
          return Root.OwnerKind == ModuleAddressOwnerKind::Unknown ||
                 Root.OwnerVA == Container;
        });
        if (!Exact && !May)
          continue;
        // The generic call-use summary includes unspecified registers and
        // frame contents. This exact runtime ABI consumes only the two
        // arguments independently certified above, including an exact target.
        if (Use.FuncIndex != I ||
            Use.UseKind != ModuleAddressUse::Kind::PointerEscape ||
            !llvm::any_of(Calls, [&](const auto &Call) {
              return Use.Addr == Call.CallAddr && Use.Seq == Call.CallSeq;
            })) {
          Exclusive = false;
          break;
        }
      }
      if (Exclusive)
        Result.LocalUnwindsByOwner[F.Entry].insert(
            Result.LocalUnwindsByOwner[F.Entry].end(), Calls.begin(),
            Calls.end());
    }
  }

  for (size_t FuncIndex = 0; FuncIndex < Funcs.size(); ++FuncIndex) {
    if (FuncIndex >= LocalUnwind.SawCallByFunction.size() ||
        !LocalUnwind.SawCallByFunction[FuncIndex])
      continue;
    if (FuncIndex >= LocalUnwind.CompleteByFunction.size() ||
        !LocalUnwind.CompleteByFunction[FuncIndex]) {
      Result.AnalysisComplete = false;
      return Result;
    }
    const LowFunc &Source = Funcs[FuncIndex];
    const ExceptionFunction *SourceRuntimeFunction = nullptr;
    bool SourceRuntimeFunctionAmbiguous = false;
    for (const ExceptionFunction *Candidate :
         exceptionRecordsStartingAt(Img.ExceptionMetadata, Source.Entry)) {
      if (Candidate->ParseStatus != ExceptionParseStatus::Complete)
        continue;
      if (SourceRuntimeFunction) {
        SourceRuntimeFunction = nullptr;
        SourceRuntimeFunctionAmbiguous = true;
        break;
      }
      SourceRuntimeFunction = Candidate;
    }
    if (SourceRuntimeFunctionAmbiguous || !SourceRuntimeFunction)
      continue;

    std::set<va_t> PublishedBySource;
    for (va_t Target : Source.CodeRefTargets)
      PublishedBySource.insert(
          normalizeCodeAddress(Target, Img.Arch, Img.Mode));
    for (va_t Target : LocalUnwind.TargetsByFunction[FuncIndex]) {
      Target = normalizeCodeAddress(Target, Img.Arch, Img.Mode);
      if (PublishedBySource.count(Target) == 0 || isFunctionEntry(Target) ||
          !Img.hasExecutableCodeOwnerAt(Target))
        continue;

      const ExceptionFunction *Owner = nullptr;
      bool OwnerAmbiguous = false;
      for (const ExceptionFunction &Candidate :
           Img.ExceptionMetadata.Functions) {
        if (Candidate.Kind != RuntimeFunctionKind::Primary ||
            Candidate.ParseStatus != ExceptionParseStatus::Complete ||
            !Candidate.SEH || Target <= Candidate.CodeRange.Begin ||
            Target >= Candidate.CodeRange.End)
          continue;
        if (Owner) {
          Owner = nullptr;
          OwnerAmbiguous = true;
          break;
        }
        Owner = &Candidate;
      }
      if (!OwnerAmbiguous && Owner && isFunctionEntry(Owner->CodeRange.Begin) &&
          Owner->CodeRange.Begin != Source.Entry)
        Result.RootsByOwner[Owner->CodeRange.Begin].insert(Target);
    }
  }

  auto uniqueFH3Owner = [&](va_t Target) -> const ExceptionFunction * {
    const ExceptionFunction *Match = nullptr;
    for (const ExceptionFunction &F : Img.ExceptionMetadata.Functions) {
      if (F.Kind != RuntimeFunctionKind::Primary ||
          F.ParseStatus != ExceptionParseStatus::Complete || !F.Cxx ||
          F.Cxx->NativeEncoding != CxxExceptionInfo::Encoding::FH3 ||
          Target <= F.CodeRange.Begin || Target >= F.CodeRange.End)
        continue;
      if (Match)
        return nullptr;
      Match = &F;
    }
    return Match;
  };

  for (size_t FuncIndex = 0; FuncIndex < Funcs.size(); ++FuncIndex) {
    if (!FH3CatchSources[FuncIndex])
      continue;
    Result.CxxContinuationExitAnalysisCompleteByFunction[FuncIndex] = true;
    Result.CxxContinuationExitsByFunction[FuncIndex] =
        Returned.OccurrencesByFunction[FuncIndex];
    if (FuncIndex >= Returned.CompleteByFunction.size() ||
        !Returned.CompleteByFunction[FuncIndex])
      continue;
    const LowFunc &SourceLow = Funcs[FuncIndex];

    std::set<va_t> PublishedBySource;
    for (va_t Target : SourceLow.CodeRefTargets)
      PublishedBySource.insert(
          normalizeCodeAddress(Target, Img.Arch, Img.Mode));
    for (va_t Target : Returned.TargetsByFunction[FuncIndex]) {
      Target = normalizeCodeAddress(Target, Img.Arch, Img.Mode);
      if (PublishedBySource.count(Target) == 0 || isFunctionEntry(Target) ||
          !Img.hasExecutableCodeOwnerAt(Target))
        continue;
      const ExceptionFunction *Owner = uniqueFH3Owner(Target);
      if (!Owner || !isFunctionEntry(Owner->CodeRange.Begin))
        continue;
      const bool DeclaresSource = std::any_of(
          Owner->Cxx->TryBlocks.begin(), Owner->Cxx->TryBlocks.end(),
          [&](const CxxTryBlock &Try) {
            return std::any_of(Try.Handlers.begin(), Try.Handlers.end(),
                               [&](const CxxCatchHandler &Handler) {
                                 return normalizeCodeAddress(
                                            Handler.HandlerVA, Img.Arch,
                                            Img.Mode) == SourceLow.Entry;
                               });
          });
      if (DeclaresSource) {
        Result.RootsByOwner[Owner->CodeRange.Begin].insert(Target);
        if (Owner->Cxx->NativeFuncInfoVA &&
            Owner->Cxx->NativeFuncInfoVA ==
                FH3CatchSources[FuncIndex]->Cxx->NativeFuncInfoVA)
          for (const auto &Exit : Returned.OccurrencesByFunction[FuncIndex])
            if (Exit.Complete && llvm::is_contained(Exit.Targets, Target))
              Result.EntriesByOwner[Owner->CodeRange.Begin].push_back(
                  {Target, SourceLow.Entry, Exit.ReturnAddr, Exit.ReturnSeq});
      }
    }
  }
  return Result;
}

struct ModuleJumpTableArbitration {
  std::set<va_t> ProtectedRelocationSlots;
  std::set<va_t> UnsafeBranches;
  bool AnalysisComplete = true;
};

ModuleJumpTableArbitration
collectModuleJumpTableArbitration(const BinaryImage &Img,
                                  const std::vector<LowFunc> &Funcs,
                                  std::optional<size_t> TestBudget) {
  ModuleJumpTableArbitration Result;
  ModuleEvidenceBudget Budget(TestBudget);
  size_t PairScanRemaining = size_t{limits::kMaxJumpTableEvidenceWork} *
                             size_t{limits::kMaxMultiStageRetries};
  // Final-graph replay is proof work, distinct from retaining a source-index
  // node or screening a pair. Every replay shares this one finite balance.
  size_t SourceProofRemaining =
      std::min(TestBudget.value_or(limits::kMaxJumpTableCandidateEvidenceWork),
               size_t{limits::kMaxJumpTableCandidateEvidenceWork});
  auto scanPair = [&](size_t Amount = 1) {
    if (Amount > PairScanRemaining)
      return false;
    PairScanRemaining -= Amount;
    return true;
  };
  std::vector<ModuleJumpTableOwner> Owners;
  std::set<va_t> Requested;
  for (size_t FuncIndex = 0; FuncIndex < Funcs.size(); ++FuncIndex)
    for (const JumpTable &JT : Funcs[FuncIndex].JumpTables) {
      if (JT.StorageRanges.empty())
        continue;
      ModuleJumpTableOwner Owner;
      Owner.FuncIndex = FuncIndex;
      Owner.BranchAddr = JT.InsnAddr;
      Owner.StorageIdentityVA =
          JT.HasBaseAddr
              ? JT.BaseAddr
              : (JT.StorageRanges.empty() ? InvalidVA
                                          : JT.StorageRanges.front().BaseAddr);
      Owner.StorageRanges = JT.StorageRanges;
      Owner.PhysicalIdentityRanges =
          JT.ExactPhysicalStorageRange
              ? std::vector<
                    JumpTableStorageRange>{*JT.ExactPhysicalStorageRange}
              : JT.StorageRanges;
      Owner.RequestedSlots.insert(JT.SuppressibleRelocationSlots.begin(),
                                  JT.SuppressibleRelocationSlots.end());
      Requested.insert(Owner.RequestedSlots.begin(),
                       Owner.RequestedSlots.end());
      Owners.push_back(std::move(Owner));
    }
  if (Owners.empty())
    return Result;
  auto abandonAnalysis = [&]() {
    Result.AnalysisComplete = false;
    Result.ProtectedRelocationSlots.insert(Requested.begin(), Requested.end());
    for (const ModuleJumpTableOwner &Owner : Owners)
      if (storageMayBeWritable(Img, Owner))
        Result.UnsafeBranches.insert(Owner.BranchAddr);
    return Result;
  };
  if (!Budget.consume(Owners.size()) || !Budget.consume(Requested.size()))
    return abandonAnalysis();

  // Loader table-anchor metadata is format-dependent and may not name the
  // first relocation slot of an adjacent relative table.  A final public
  // occurrence can supply that missing identity, but only when the final
  // public inventory exactly consumes or defines a data address and the target
  // itself is a relative-code or code-pointer relocation slot.  This mirrors
  // CFGBuilder::currentRelocatedInstructionTableAnchors: CFG publication has
  // already revalidated every retained occurrence against its final
  // Boundary/Op.  Prepay every attacker-controlled field check and ordered-
  // container operation; on exhaustion the local set is discarded and
  // arbitration protects every requested slot.
  std::set<va_t> OccurrenceTableAnchors;
  std::set<va_t> OccurrenceRelativeTableAnchors;
  const size_t RelCodeRelocationLookupWork =
      detail::orderedContainerLookupWork(Img.RelCodeRelocSlots.size());
  const size_t CodePointerRelocationLookupWork =
      detail::orderedContainerLookupWork(Img.CodePtrRelocSlots.size());
  for (const LowFunc &Func : Funcs)
    for (const RelocatedInstructionAddressOccurrence &Occurrence :
         Func.RelocatedInstructionAddressOccurrences) {
      constexpr size_t PredicateFixedWork = 4;
      if (!scanPair(PredicateFixedWork))
        return abandonAnalysis();
      if (Occurrence.OutputMayDepend || Occurrence.TargetVA == InvalidVA ||
          Occurrence.Provenance != ConstantAddressProvenance::DataAddress)
        continue;
      if (!scanPair(RelCodeRelocationLookupWork))
        return abandonAnalysis();
      const bool IsRelativeAnchor =
          Img.RelCodeRelocSlots.count(Occurrence.TargetVA) != 0;
      bool IsCodePointerAnchor = false;
      if (!IsRelativeAnchor) {
        if (!scanPair(CodePointerRelocationLookupWork))
          return abandonAnalysis();
        IsCodePointerAnchor =
            Img.CodePtrRelocSlots.count(Occurrence.TargetVA) != 0;
      }
      if (!IsRelativeAnchor && !IsCodePointerAnchor)
        continue;
      const size_t AnchorInsertWork =
          detail::orderedContainerLookupWork(OccurrenceTableAnchors.size()) + 2;
      if (!Budget.consume(AnchorInsertWork))
        return abandonAnalysis();
      OccurrenceTableAnchors.insert(Occurrence.TargetVA);
      if (IsRelativeAnchor) {
        const size_t RelativeAnchorInsertWork =
            detail::orderedContainerLookupWork(
                OccurrenceRelativeTableAnchors.size()) +
            2;
        if (!Budget.consume(RelativeAnchorInsertWork))
          return abandonAnalysis();
        OccurrenceRelativeTableAnchors.insert(Occurrence.TargetVA);
      }
    }

  struct CompletePhysicalEnvelope {
    va_t Begin = InvalidVA;
    va_t End = InvalidVA;
    bool Complete = false;
  };
  std::vector<CompletePhysicalEnvelope> PhysicalEnvelopes(Owners.size());
  CompletePhysicalEnvelope AllPhysicalStorage;
  AllPhysicalStorage.Complete = true;
  for (size_t OwnerIndex = 0; OwnerIndex < Owners.size(); ++OwnerIndex) {
    const ModuleJumpTableOwner &Owner = Owners[OwnerIndex];
    if (!Budget.consume(Owner.PhysicalIdentityRanges.size()))
      return abandonAnalysis();
    CompletePhysicalEnvelope Envelope;
    Envelope.Complete = !Owner.PhysicalIdentityRanges.empty();
    for (const JumpTableStorageRange &Range : Owner.PhysicalIdentityRanges) {
      const std::optional<va_t> End = Range.storageEnd();
      if (!End) {
        Envelope.Complete = false;
        break;
      }
      if (Envelope.Begin == InvalidVA || Range.BaseAddr < Envelope.Begin)
        Envelope.Begin = Range.BaseAddr;
      if (Envelope.End == InvalidVA || *End > Envelope.End)
        Envelope.End = *End;
    }
    PhysicalEnvelopes[OwnerIndex] = Envelope;
    AllPhysicalStorage.Complete &= Envelope.Complete;
    if (Envelope.Complete) {
      if (AllPhysicalStorage.Begin == InvalidVA ||
          Envelope.Begin < AllPhysicalStorage.Begin)
        AllPhysicalStorage.Begin = Envelope.Begin;
      if (AllPhysicalStorage.End == InvalidVA ||
          Envelope.End > AllPhysicalStorage.End)
        AllPhysicalStorage.End = Envelope.End;
    }
  }
  // Retain a prefix maximum of the complete owner envelopes. The last start
  // at or before an address then proves whether any envelope covers it, even
  // when envelopes overlap or nest. Do not scan the unrelated space between
  // disjoint owners as though the whole module were one table object.
  std::map<va_t, va_t> PhysicalEndByBegin;
  if (AllPhysicalStorage.Complete) {
    for (const CompletePhysicalEnvelope &Envelope : PhysicalEnvelopes) {
      if (!scanPair(
              detail::orderedContainerLookupWork(PhysicalEndByBegin.size())) ||
          !Budget.consume())
        return abandonAnalysis();
      auto [It, Inserted] =
          PhysicalEndByBegin.emplace(Envelope.Begin, Envelope.End);
      if (!Inserted)
        It->second = std::max(It->second, Envelope.End);
    }
    va_t End = 0;
    for (auto &[Begin, PrefixEnd] : PhysicalEndByBegin) {
      if (!scanPair())
        return abandonAnalysis();
      End = std::max(End, PrefixEnd);
      PrefixEnd = End;
    }
  }
  // This is only a rejection filter for exact addresses. Each owner keeps
  // its sparse filler, internal gaps and one-past endpoint; possible matches
  // still use the owner-aware checks below. An incomplete range disables it.
  // A frozen gap proves every strictly interior address disjoint. Reuse that
  // one interval across nearby inputs, each still paying its field-scan debit.
  std::optional<std::pair<va_t, va_t>> LastPhysicalGap;
  auto outsideAllPhysicalStorage = [&](va_t Address) -> std::optional<bool> {
    if (!AllPhysicalStorage.Complete)
      return false;
    if (Address < AllPhysicalStorage.Begin || Address > AllPhysicalStorage.End)
      return true;
    if (LastPhysicalGap && Address > LastPhysicalGap->first &&
        Address < LastPhysicalGap->second)
      return true;
    if (!scanPair(
            detail::orderedContainerLookupWork(PhysicalEndByBegin.size())))
      return std::nullopt;
    const auto After = PhysicalEndByBegin.upper_bound(Address);
    if (After == PhysicalEndByBegin.begin())
      return true;
    const va_t BeforeEnd = std::prev(After)->second;
    if (Address <= BeforeEnd)
      return false;
    if (After != PhysicalEndByBegin.end())
      LastPhysicalGap = std::pair{BeforeEnd, After->first};
    return true;
  };

  std::map<va_t, std::set<va_t>> TableObjectAliases;
  size_t TableAliasWorkRemaining = limits::kMaxJumpTableEvidenceWork;
  for (const ModuleJumpTableOwner &Owner : Owners)
    TableObjectAliases[Owner.StorageIdentityVA].insert(Owner.StorageIdentityVA);
  for (size_t Left = 0; Left < Owners.size(); ++Left)
    for (size_t Right = Left + 1; Right < Owners.size(); ++Right) {
      if (!scanPair())
        return abandonAnalysis();
      const CompletePhysicalEnvelope &LeftEnvelope = PhysicalEnvelopes[Left];
      const CompletePhysicalEnvelope &RightEnvelope = PhysicalEnvelopes[Right];
      if (LeftEnvelope.Complete && RightEnvelope.Complete &&
          (LeftEnvelope.End <= RightEnvelope.Begin ||
           RightEnvelope.End <= LeftEnvelope.Begin))
        continue;
      if (!pipeline_detail::tableObjectSummaryMayAlias(
              Img, Owners[Left].StorageIdentityVA,
              Owners[Left].PhysicalIdentityRanges,
              Owners[Right].StorageIdentityVA,
              Owners[Right].PhysicalIdentityRanges, &TableAliasWorkRemaining))
        continue;
      TableObjectAliases[Owners[Left].StorageIdentityVA].insert(
          Owners[Right].StorageIdentityVA);
      TableObjectAliases[Owners[Right].StorageIdentityVA].insert(
          Owners[Left].StorageIdentityVA);
    }

  std::set<va_t> &Protected = Result.ProtectedRelocationSlots;
  // Preserve/veto dominates a suppression request when two recovered tables
  // share physical storage or one table's local consumer audit retained a
  // field that another table requested to suppress.
  for (va_t Slot : Requested)
    for (size_t FuncIndex = 0; FuncIndex < Funcs.size(); ++FuncIndex)
      for (const JumpTable &JT : Funcs[FuncIndex].JumpTables) {
        if (!scanPair())
          return abandonAnalysis();
        const auto OwnsRequestedSlot = [&](const JumpTableStorageRange &Range) {
          return Range.ownsStorageAddress(Slot);
        };
        if (!(JT.ExactPhysicalStorageRange
                  ? OwnsRequestedSlot(*JT.ExactPhysicalStorageRange)
                  : std::any_of(JT.StorageRanges.begin(),
                                JT.StorageRanges.end(), OwnsRequestedSlot)))
          continue;
        if (!Budget.consume())
          return abandonAnalysis();
        if (!JT.suppressesRelocationSlot(Slot))
          Protected.insert(Slot);
      }

  auto protectWholeOwner = [&](const ModuleJumpTableOwner &Owner) {
    Protected.insert(Owner.RequestedSlots.begin(), Owner.RequestedSlots.end());
  };
  std::vector<ModuleAddressUse> Uses;
  if (!collectLowAddressUses(Img, Funcs, Owners, Uses, Budget, TestBudget,
                             nullptr, nullptr,
                             ModuleAddressEdgeTraversal::OrdinaryAndExceptional,
                             nullptr, &OccurrenceTableAnchors))
    return abandonAnalysis();
  auto summaryRootMayCoverField = [&](const ModuleAddressRootScope &Root,
                                      va_t FieldVA, uint8_t FieldWidth) {
    switch (Root.OwnerKind) {
    case ModuleAddressOwnerKind::Unknown:
      return true;
    case ModuleAddressOwnerKind::Container: {
      va_t FieldOwner = InvalidVA;
      if (const Section *Sec = Img.getSectionFor(FieldVA))
        FieldOwner = Sec->VA;
      else if (const Segment *Seg = Img.getSegmentFor(FieldVA))
        FieldOwner = Seg->VA;
      return FieldOwner != InvalidVA && Root.OwnerVA == FieldOwner;
    }
    case ModuleAddressOwnerKind::TableObject:
      for (const ModuleJumpTableOwner &Candidate : Owners) {
        if (Candidate.StorageIdentityVA != Root.OwnerVA)
          continue;
        for (const JumpTableStorageRange &Range :
             Candidate.PhysicalIdentityRanges) {
          const std::optional<va_t> End = Range.storageEnd();
          if (!End)
            return true;
          const va_t FieldEnd = FieldVA + FieldWidth;
          if (FieldVA < *End && Range.BaseAddr < FieldEnd)
            return true;
        }
      }
      return false;
    }
    return false;
  };
  auto reachableLoadCoversField = [&](va_t FieldVA, uint8_t FieldWidth,
                                      const ModuleJumpTableOwner &Owner) {
    if (FieldWidth == 0 || FieldWidth > InvalidVA - FieldVA)
      return true;
    const va_t FieldEnd = FieldVA + FieldWidth;
    for (const ModuleAddressUse &Use : Uses) {
      if (Use.UseKind != ModuleAddressUse::Kind::Load || Use.AccessSize == 0)
        continue;
      // The exact target-table LOAD is the recovered dispatch consumer, not
      // an independent read of its relocation fields.  The ordinary use loop
      // below already applies this same occurrence+owner exclusion; applying
      // the relocation-field veto first without it made every TwoTable slot
      // protected, reintroduced all case roots, and invalidated the very CFG
      // certificate that authenticated the LOAD.  Any sibling LOAD still
      // reaches this path and preserves the affected field/object.
      if (isAuthenticatedTableLoad(Funcs[Use.FuncIndex], Use.Addr, Use.Seq,
                                   Owner))
        continue;
      for (const ModuleAddressRootScope &Root : Use.RootScopes)
        if (summaryRootMayCoverField(Root, FieldVA, FieldWidth))
          return true;
      for (const ModuleAddressAnchor &Anchor : Use.ExactAddresses) {
        if (Use.AccessSize > InvalidVA - Anchor.Address)
          return true;
        const va_t AccessEnd = Anchor.Address + Use.AccessSize;
        if (Anchor.Address < FieldEnd && FieldVA < AccessEnd)
          return true;
      }
    }
    return false;
  };
  auto relocationContainerMayOwn = [&](const ModuleJumpTableOwner &Owner,
                                       va_t TargetOwnerVA) {
    if (TargetOwnerVA == InvalidVA)
      return true;
    return std::any_of(Owner.PhysicalIdentityRanges.begin(),
                       Owner.PhysicalIdentityRanges.end(),
                       [&](const JumpTableStorageRange &Range) {
                         return storageRangeOwnerVA(Img, Range) ==
                                TargetOwnerVA;
                       });
  };
  struct ExactSource {
    size_t FuncIndex = 0;
    const RelocatedInstructionAddressOccurrence *Occurrence = nullptr;
  };
  // Index the frozen public inventory once instead of scanning every source
  // for every loader field. Preserve all sources, including conflicting and
  // imprecise ones; anchor eligibility is not source eligibility. Only actual
  // loader fields query this index below, so indexing a source needs no
  // preliminary lookup in both loader maps. Unmatched keys grant no authority
  // and still spend the same bounded retention account.
  std::map<va_t, std::vector<ExactSource>> SourcesByField;
  for (size_t FuncIndex = 0; FuncIndex < Funcs.size(); ++FuncIndex)
    for (const auto &Occurrence :
         Funcs[FuncIndex].RelocatedInstructionAddressOccurrences) {
      if (!scanPair())
        return abandonAnalysis();
      if (Occurrence.FieldVA == InvalidVA)
        continue;
      if (!scanPair(detail::orderedContainerLookupWork(SourcesByField.size())))
        return abandonAnalysis();
      auto It = SourcesByField.lower_bound(Occurrence.FieldVA);
      if (It == SourcesByField.end() || It->first != Occurrence.FieldVA) {
        if (!Budget.consume())
          return abandonAnalysis();
        It = SourcesByField.emplace_hint(It, Occurrence.FieldVA,
                                         std::vector<ExactSource>{});
      }
      if (!Budget.consume())
        return abandonAnalysis();
      It->second.push_back({FuncIndex, &Occurrence});
    }

  // An occurrence's address is a unique identity within this immutable input
  // inventory. Equal fields in the same function still receive independent
  // bindings; an imprecise duplicate cannot borrow a successful source proof.
  std::map<const RelocatedInstructionAddressOccurrence *,
           std::optional<JumpTableStorageRange>>
      FinalSourceAccesses;

  auto protectRelocationUse = [&](va_t FieldVA,
                                  const RelocatedAddressField &Field) {
    if (!scanPair())
      return false;
    // A complete absolute target outside every physical envelope cannot
    // expose an owner, regardless of which instruction contexts name it.
    // Screen it before paying for the field/source lookup. PC-relative loader
    // approximations must still reach the exact-source/container logic.
    if (!Field.PCRelativeFromInstructionEnd) {
      const auto Outside = outsideAllPhysicalStorage(Field.TargetVA);
      if (!Outside)
        return false;
      if (*Outside)
        return true;
    }
    if (!scanPair(detail::orderedContainerLookupWork(SourcesByField.size())))
      return false;
    const auto Sources = SourcesByField.find(FieldVA);
    const llvm::ArrayRef<ExactSource> ExactSources =
        Sources == SourcesByField.end()
            ? llvm::ArrayRef<ExactSource>()
            : llvm::ArrayRef<ExactSource>(Sources->second);

    // A loader-side PC-relative TargetVA is only an approximation until
    // the containing instruction end is known.  Published exact
    // occurrences below carry the recomputed target.  With no such
    // occurrence, fail closed as an unclassified data/global relocation
    // and use only its container owner; do not authorize by the numeric
    // approximation.
    const bool HasExactPCRelativeTarget =
        !Field.PCRelativeFromInstructionEnd || !ExactSources.empty();
    for (const ModuleJumpTableOwner &Owner : Owners) {
      if (!scanPair())
        return false;
      bool TargetsOwner = false;
      if (!Field.PCRelativeFromInstructionEnd) {
        const ModuleAddressAnchor Target{
            Field.TargetVA, Field.TargetOwnerVA,
            Field.TargetOwnerVA == InvalidVA
                ? ModuleAddressOwnerKind::Unknown
                : ModuleAddressOwnerKind::Container};
        TargetsOwner = storageEnvelopeContainsOrOnePast(
            Img, Owner, Target, &OccurrenceRelativeTableAnchors);
      } else {
        for (const ExactSource &Source : ExactSources) {
          if (!scanPair())
            return false;
          const RelocatedInstructionAddressOccurrence &Occurrence =
              *Source.Occurrence;
          const ModuleAddressAnchor Target{
              Occurrence.TargetVA, Occurrence.TargetOwnerVA,
              Occurrence.TargetOwnerVA == InvalidVA
                  ? ModuleAddressOwnerKind::Unknown
                  : ModuleAddressOwnerKind::Container};
          if (storageEnvelopeContainsOrOnePast(
                  Img, Owner, Target, &OccurrenceRelativeTableAnchors)) {
            TargetsOwner = true;
            break;
          }
        }
        if (!HasExactPCRelativeTarget)
          TargetsOwner = relocationContainerMayOwn(Owner, Field.TargetOwnerVA);
      }
      if (!TargetsOwner)
        continue;
      if (!Budget.consume())
        return false;
      // A reachable memory read is authoritative data use even if a
      // bogus code root also made the same bytes a published instruction.
      // Apply this veto before interpreting instruction sources.
      if (reachableLoadCoversField(FieldVA, Field.Width, Owner)) {
        protectWholeOwner(Owner);
        continue;
      }
      if (ExactSources.empty()) {
        // A data/global occurrence has no function-local proof limiting
        // how the exposed pointer will be used.  The same conservative
        // rule applies to an unexplored relocation in executable bytes:
        // without a final exact occurrence it is indistinguishable from
        // inline data.
        protectWholeOwner(Owner);
        continue;
      }
      for (const ExactSource &Source : ExactSources) {
        if (!scanPair())
          return false;
        const size_t FuncIndex = Source.FuncIndex;
        const va_t InsnAddr = Source.Occurrence->InstructionAddr;
        if (FuncIndex == Owner.FuncIndex)
          continue; // candidate-local audit already handled it
        if (isUniquelyAuthenticatedTableLoad(Funcs[FuncIndex], InsnAddr, Owner))
          continue;
        if (!scanPair(
                detail::orderedContainerLookupWork(FinalSourceAccesses.size())))
          return false;
        auto Access = FinalSourceAccesses.find(Source.Occurrence);
        if (Access == FinalSourceAccesses.end()) {
          if (!Budget.consume())
            return false;
          const auto Range = CFGBuilder::proveInstructionLocalTableSource(
              Img, Funcs[FuncIndex], *Source.Occurrence, SourceProofRemaining);
          if (SourceProofRemaining == 0)
            return false;
          Access = FinalSourceAccesses.emplace(Source.Occurrence, Range).first;
        }
        if (Access->second) {
          const auto End = Access->second->storageEnd();
          bool Disjoint =
              End.has_value() && !Owner.PhysicalIdentityRanges.empty();
          for (const JumpTableStorageRange &Old :
               Owner.PhysicalIdentityRanges) {
            if (!scanPair())
              return false;
            const auto OldEnd = Old.storageEnd();
            // Whole envelopes also preserve sparse filler/gap identities.
            Disjoint &=
                End && OldEnd &&
                (*End <= Old.BaseAddr || *OldEnd <= Access->second->BaseAddr);
          }
          if (Disjoint)
            continue;
        }
        // A loader record has no LowOp sequence identity.  Unless this
        // machine instruction contains exactly one LOAD and that exact
        // occurrence is an authenticated table role, its address shape is
        // unknown: retaining only the numerically named slot would miss a
        // dynamic offset, subpiece, or sibling LOAD.
        protectWholeOwner(Owner);
      }
    }
    return true;
  };
  for (const auto &[FieldVA, Field] : Img.DataAddressRelocOperands)
    if (!protectRelocationUse(FieldVA, Field))
      return abandonAnalysis();
  for (const auto &[FieldVA, Field] : Img.CodeAddressRelocOperands)
    if (!protectRelocationUse(FieldVA, Field))
      return abandonAnalysis();
  for (va_t PointerSlot : Img.DataPtrRelocSlots) {
    if (!scanPair())
      return abandonAnalysis();
    const uint8_t *P = Img.readVA(PointerSlot, Img.getPointerSize());
    if (!P)
      return abandonAnalysis();
    const va_t Target = static_cast<va_t>(readPtr(P, Img.is64Bit()));
    const auto Outside = outsideAllPhysicalStorage(Target);
    if (!Outside)
      return abandonAnalysis();
    if (*Outside)
      continue;
    va_t TargetOwnerVA = InvalidVA;
    if (auto It = Img.DataPtrRelocTargetOwners.find(PointerSlot);
        It != Img.DataPtrRelocTargetOwners.end())
      TargetOwnerVA = It->second;
    for (const ModuleJumpTableOwner &Owner : Owners) {
      if (!scanPair())
        return abandonAnalysis();
      if (storageEnvelopeContainsOrOnePast(
              Img, Owner,
              ModuleAddressAnchor{Target, TargetOwnerVA,
                                  TargetOwnerVA == InvalidVA
                                      ? ModuleAddressOwnerKind::Unknown
                                      : ModuleAddressOwnerKind::Container},
              &OccurrenceRelativeTableAnchors)) {
        if (!Budget.consume())
          return abandonAnalysis();
        protectWholeOwner(Owner);
        // A relocation-backed global pointer exposes the complete writable
        // table to code outside this occurrence-level LowIR analysis.  A later
        // load through that pointer deliberately loses the pointee provenance,
        // so conservatively reject the static mapping rather than waiting for
        // an unobservable STORE-through alias.
        if (storageMayBeWritable(Img, Owner))
          Result.UnsafeBranches.insert(Owner.BranchAddr);
      }
    }
  }

  for (const ModuleAddressUse &Use : Uses)
    for (const ModuleJumpTableOwner &Owner : Owners) {
      if (!scanPair())
        return abandonAnalysis();
      const bool Writable = storageMayBeWritable(Img, Owner);
      if (Use.FuncIndex == Owner.FuncIndex &&
          Use.UseKind == ModuleAddressUse::Kind::Load)
        continue;
      if (Use.FuncIndex == Owner.FuncIndex &&
          Use.UseKind == ModuleAddressUse::Kind::PointerEscape && !Writable)
        continue;
      if (Use.UseKind == ModuleAddressUse::Kind::Load &&
          isAuthenticatedTableLoad(Funcs[Use.FuncIndex], Use.Addr, Use.Seq,
                                   Owner))
        continue;
      bool TouchesObject = false;
      bool IntersectionComplete = true;
      std::set<va_t> TouchedRequestedSlots;
      for (const ModuleAddressRootScope &Root : Use.RootScopes)
        TouchesObject |= moduleAddressSummaryMayAliasOwner(
            Img, TableObjectAliases, Owner, Root);
      for (const ModuleAddressAnchor &Address : Use.ExactAddresses) {
        if (Use.Imprecise ||
            Use.UseKind == ModuleAddressUse::Kind::PointerEscape) {
          TouchesObject |= storageEnvelopeContainsOrOnePast(
              Img, Owner, Address, &OccurrenceRelativeTableAnchors);
          continue;
        }
        StorageAccessIntersection Intersection =
            storageAccessIntersection(Img, Owner, Address, Use.AccessSize);
        IntersectionComplete &= Intersection.Complete;
        TouchesObject |= Intersection.Any;
        TouchedRequestedSlots.insert(Intersection.RequestedSlots.begin(),
                                     Intersection.RequestedSlots.end());
      }
      if (!TouchesObject && IntersectionComplete)
        continue;
      if (!Budget.consume())
        return abandonAnalysis();
      if (Use.UseKind == ModuleAddressUse::Kind::WriteThrough && Writable) {
        Result.UnsafeBranches.insert(Owner.BranchAddr);
        protectWholeOwner(Owner);
        continue;
      }
      if (Use.UseKind == ModuleAddressUse::Kind::PointerEscape && Writable) {
        Result.UnsafeBranches.insert(Owner.BranchAddr);
        protectWholeOwner(Owner);
        continue;
      }
      if (Use.UseKind == ModuleAddressUse::Kind::Load && !Use.Imprecise &&
          IntersectionComplete && !TouchedRequestedSlots.empty())
        Protected.insert(TouchedRequestedSlots.begin(),
                         TouchedRequestedSlots.end());
      else
        protectWholeOwner(Owner);
    }
  return Result;
}

} // namespace

namespace pipeline_detail {

ModuleJumpTableArbitrationTestResult
arbitrateModuleJumpTablesForTesting(const BinaryImage &Img,
                                    const std::vector<LowFunc> &Funcs,
                                    std::optional<size_t> TestBudget) {
  ModuleJumpTableArbitration Result =
      collectModuleJumpTableArbitration(Img, Funcs, TestBudget);
  return {std::move(Result.UnsafeBranches), Result.AnalysisComplete,
          std::move(Result.ProtectedRelocationSlots)};
}

ReturnedCodeEvidenceTestResult collectReturnedCodeEvidenceForTesting(
    const BinaryImage &Img, const std::vector<LowFunc> &Funcs,
    const std::set<va_t> &CandidateTargets, std::optional<size_t> TestBudget) {
  ModuleEvidenceBudget Budget(TestBudget);
  ReturnedCodeEvidence Returned;
  std::vector<ModuleAddressUse> IgnoredUses;
  const std::vector<ModuleJumpTableOwner> NoTableOwners;
  const bool AnalysisComplete = collectLowAddressUses(
      Img, Funcs, NoTableOwners, IgnoredUses, Budget, TestBudget,
      &CandidateTargets, &Returned, ModuleAddressEdgeTraversal::OrdinaryOnly);
  if (!AnalysisComplete)
    return {};
  return {std::move(Returned.TargetsByFunction),
          std::move(Returned.CompleteByFunction),
          std::move(Returned.OccurrencesByFunction), AnalysisComplete};
}

WindowsEHContinuationRootTestResult collectWindowsEHContinuationRootsForTesting(
    const BinaryImage &Img, const std::vector<LowFunc> &Funcs,
    const std::set<va_t> &FunctionEntries, std::optional<size_t> TestBudget,
    bool CompleteModule) {
  EHContinuationRootDiscovery Result = collectWindowsEHContinuationRoots(
      Img, Funcs, FunctionEntries, TestBudget, CompleteModule);
  return {std::move(Result.RootsByOwner), std::move(Result.LocalUnwindsByOwner),
          Result.AnalysisComplete};
}

} // namespace pipeline_detail

//===----------------------------------------------------------------------===//
// buildLowIR — Phase 1
//===----------------------------------------------------------------------===//

namespace pipeline_detail {
/// What computeCallRegisterEffects reads of a callee's body: its register
/// effect, the fixed arguments of the formatted calls it makes, and whether
/// it forwards to the guard dispatcher.  The effect read every call's fixed
/// arguments as the run had collected them by then (Consulted, absent ones
/// too); it stands for another run only where that run gives each of those
/// calls the same.
struct CalleeFacts {
  LocalRegisterEffect Effect;
  std::vector<std::pair<va_t, GPRReadWidths>> FormattedReads;
  std::vector<std::pair<va_t, std::optional<GPRReadWidths>>> Consulted;
  bool ForwardsToGuardDispatch = false;
};

/// CalleeFacts by callee entry, for one analysis cache's facts.  A body is
/// lifted, and its effect computed, once rather than in every run whose
/// callees reach it.
class CalleeFactsCache {
public:
  std::shared_ptr<const CalleeFacts> find(va_t Entry) {
    std::lock_guard<std::mutex> Lock(Mutex);
    const auto Found = Index.find(Entry);
    if (Found == Index.end())
      return nullptr;
    Items.splice(Items.begin(), Items, Found->second);
    return Items.front().Facts;
  }
  /// Keep \p Facts for \p Entry.  Without \p Evict, a cache with no room
  /// keeps what it holds (warming fills it in entry order).
  void insert(va_t Entry, std::shared_ptr<const CalleeFacts> Facts,
              bool Evict) {
    const size_t Size = bytes(*Facts);
    std::lock_guard<std::mutex> Lock(Mutex);
    if (Size > MaxBytes)
      return;
    if (const auto Found = Index.find(Entry); Found != Index.end()) {
      Bytes -= Found->second->Bytes;
      Items.erase(Found->second);
      Index.erase(Found);
    }
    if (!Evict && Size > MaxBytes - Bytes)
      return;
    while (!Items.empty() && Size > MaxBytes - Bytes) {
      Bytes -= Items.back().Bytes;
      Index.erase(Items.back().Entry);
      Items.pop_back();
    }
    Items.push_front({Entry, std::move(Facts), Size});
    Index[Entry] = Items.begin();
    Bytes += Size;
  }

private:
  // Joined effects take a few kilobytes a function: room for a kernel's.
  static constexpr size_t MaxBytes =
      ::neverd::limits::pipeline::kPipelineLowIRMaxBytes;
  struct Item {
    va_t Entry;
    std::shared_ptr<const CalleeFacts> Facts;
    size_t Bytes;
  };
  static size_t bytes(const CalleeFacts &Facts) {
    constexpr size_t Node = 6 * sizeof(void *);
    const LocalRegisterEffect &Effect = Facts.Effect;
    size_t Bytes =
        sizeof(Item) + sizeof(CalleeFacts) + 2 * Node +
        Effect.Blocks.capacity() * sizeof(RegisterBlock) +
        Effect.ExitConstants.capacity() * sizeof(GPRExitConstants::value_type) +
        (Effect.Callees.size() + Effect.NoReturnCallees.size() +
         Effect.StackTailCallees.size()) *
            (Node + sizeof(va_t)) +
        Facts.FormattedReads.capacity() * sizeof(Facts.FormattedReads.front()) +
        Facts.Consulted.capacity() * sizeof(Facts.Consulted.front());
    if (Effect.RegisterLeaf)
      Bytes += sizeof(*Effect.RegisterLeaf) +
               Effect.RegisterLeaf->capacity() * sizeof(LowOp);
    for (const RegisterBlock &Block : Effect.Blocks)
      Bytes += Block.Steps.capacity() * sizeof(RegisterStep) +
               Block.Succs.capacity() * sizeof(size_t);
    return Bytes;
  }
  std::mutex Mutex;
  std::list<Item> Items;
  std::unordered_map<va_t, std::list<Item>::iterator> Index;
  size_t Bytes = 0;
};

/// The fixed arguments of the formatted calls in \p Blocks, as
/// computeCallRegisterEffects merges them into a run's FixedCallReads; with
/// \p Converted, the calls themselves too.
std::vector<std::pair<va_t, GPRReadWidths>>
formattedCallReads(const BinaryImage &Img, const CallRegisterBodyView &Body,
                   std::map<va_t, FormattedCall> *Converted) {
  std::map<va_t, FormattedCall> Calls;
  pipeline_detail::collectFormattedCalls(Img, Body.Blocks, Calls,
                                         Body.HasIndependentRoots);
  std::vector<std::pair<va_t, GPRReadWidths>> Reads;
  Reads.reserve(Calls.size());
  for (auto &[Address, Call] : Calls) {
    GPRReadWidths Widths{};
    for (const FormattedCallArgument &Argument : Call.Arguments)
      if (const auto Family = registerFamilyOf(Img.Arch, Argument.Register))
        Widths[Family->first] = std::max<uint8_t>(
            Widths[Family->first],
            static_cast<uint8_t>(Family->second + Argument.Bytes));
    Reads.emplace_back(Address, Widths);
    if (Converted)
      Converted->emplace(Address, std::move(Call));
  }
  return Reads;
}

void mergeFixedCallReads(std::map<va_t, GPRReadWidths> &Into,
                         llvm::ArrayRef<std::pair<va_t, GPRReadWidths>> Reads) {
  for (const auto &[Address, Widths] : Reads) {
    GPRReadWidths &Merged = Into[Address];
    for (size_t I = 0; I < Merged.size(); ++I)
      Merged[I] = std::max(Merged[I], Widths[I]);
  }
}

/// What localRegisterEffect reads of \p Reads for the calls in \p Blocks.
std::vector<std::pair<va_t, std::optional<GPRReadWidths>>>
consultedCallReads(llvm::ArrayRef<LowBlock> Blocks,
                   const std::map<va_t, GPRReadWidths> &Reads) {
  std::vector<std::pair<va_t, std::optional<GPRReadWidths>>> Consulted;
  for (const LowBlock &Block : Blocks)
    for (const LowOp &Op : Block.Ops)
      if (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL) {
        const auto Found = Reads.find(Op.Addr);
        Consulted.emplace_back(Op.Addr, Found == Reads.end()
                                            ? std::nullopt
                                            : std::optional(Found->second));
      }
  return Consulted;
}

bool consultedReadsHold(const CalleeFacts &Facts,
                        const std::map<va_t, GPRReadWidths> &Reads) {
  return llvm::all_of(Facts.Consulted, [&](const auto &Call) {
    const auto Found = Reads.find(Call.first);
    return Found == Reads.end() ? !Call.second
                                : Call.second && *Call.second == Found->second;
  });
}
} // namespace pipeline_detail

namespace {
/// True when \p F only jumps through the load configuration's Control Flow
/// Guard dispatch pointer, as ntoskrnl's KeGuardDispatchICall does with
/// `jmp [__guard_dispatch_icall_fptr]`.  Whichever dispatcher the runtime
/// installs in that pointer calls RAX with the caller's argument registers.
bool forwardsToGuardDispatch(const BinaryImage &Img,
                             llvm::ArrayRef<LowBlock> Blocks) {
  if (Blocks.size() != 1)
    return false;
  const std::vector<LowOp> &Ops = Blocks.front().Ops;
  if (Ops.size() != 2 || Ops[0].Opcode != NdOp::INDIR_CALL ||
      Ops[0].NumInputs != 1 || Ops[1].Opcode != NdOp::RETURN)
    return false;
  // A constant input to an indirect transfer names the slot it loads from.
  const NdVar &Slot = Ops[0].Inputs[0];
  return Slot.isConst() && Slot.Size == Img.getPointerSize() &&
         (Img.hasRuntimeCallablePointerSlotAt(
              Slot.Offset, RuntimeCallablePointerSlotKind::GuardCFDispatch) ||
          Img.hasRuntimeCallablePointerSlotAt(
              Slot.Offset, RuntimeCallablePointerSlotKind::GuardXFGDispatch));
}

/// The arguments of the libc imports that \p Funcs and the callees
/// \p Effects summarizes call: the registers each reads, and with \p
/// StackArguments the argument positions its stack arguments imply.
struct ImportPrototypeArguments {
  std::map<va_t, GPRReadWidths> Reads;
  std::map<va_t, int> StackArgs;
  /// The bytes of the scalar float each routine returns, where it does.
  std::map<va_t, uint16_t> FloatReturns;
  /// What the prototypes of the image's own exported routines that \p
  /// Effects summarizes declare: their callers pass at least these, which
  /// join what each body reads itself.
  std::map<va_t, GPRReadWidths> DeclaredReads;
  std::map<va_t, int> DeclaredStackArgs;
  /// Where every argument is on the stack (i386), the StackArgs a typed
  /// prototype counts slot by slot, unlike the arity tables, which count a
  /// LONGLONG as one argument.
  std::set<va_t> StackSlots;
  /// The bytes of those slots a stdcall routine pops as it returns.
  std::map<va_t, int> StackPops;
  /// The integer arguments each import's prototype counts, where the setup
  /// of each call gives it its arguments (Windows on ARM64).
  std::map<va_t, int> Counts;
  std::map<va_t, FloatingCallArguments> FloatingArguments;
  std::map<va_t, FixedIntegerCallArguments> IntegerArguments;
  std::map<va_t, std::vector<uint16_t>> VariadicArguments;
  /// The imports whose integer arguments a user declaration or header record
  /// layout specifies, overriding the parameter counts in the arity tables.
  std::set<va_t> PrototypeDeclared;
  /// The explicitly user-declared subset, for source-binding precedence.
  std::set<va_t> UserDeclared;
};

/// The arguments of the libc imports that \p Funcs and the callees
/// \p Effects summarizes call, keyed by the address each call names its
/// import by: an executable stub or a slot the loader binds, listed in the
/// import directory or not (importCalleeName).  A stub lifted as a function
/// is keyed too.  A call in a function whose own summary is incomplete still
/// passes its import's parameters.  Only a routine with a fixed prototype is
/// listed: the one the user declares (userPrototype), where it takes only
/// integer arguments, which decides over the tables (PrototypeDeclared); else
/// one of the image format's own runtime (LibCPrototype, such as the Windows C
/// runtime's `_initterm`), else one the arity tables give (LibCNames.h), by the
/// exact name where the import entry is the C name, else one the program's
/// types declare (\p Dbg's resolvePrototype), as the Windows kernel's headers
/// declare the routines a driver imports.  A routine the image exports whose
/// name binds a prototype (\p Dbg's resolveFunction) is listed too: where \p
/// Effects summarizes its body, in DeclaredReads and DeclaredStackArgs, which
/// join what the body reads, otherwise as an import is. Each integer register
/// argument is read pointer-wide.  A floating argument is listed only under
/// System V, which passes it in the next vector register at its width, and only
/// when every argument has a register. The arguments past the registers are on
/// the stack, which only a convention that summarizes stack arguments (\p
/// StackArguments) counts; elsewhere such a routine is not listed.  Without
/// \p RegisterArguments an import takes every argument on the stack.  A call
/// to one of \p DispatchThunks, where given, calls its target register.
ImportPrototypeArguments importPrototypeArguments(
    const BinaryImage &Img, const std::vector<LowFunc> &Funcs,
    const LocalRegisterEffectRefs &Effects, bool StackArguments,
    const DebugContext *Dbg, bool RegisterArguments = true,
    const std::map<va_t, PointerVariableCallee> *PointerCallees = nullptr,
    const std::vector<va_t> *ExportStarts = nullptr,
    const std::vector<va_t> *AlsoClassify = nullptr,
    const std::set<va_t> *DispatchThunks = nullptr) {
  const TargetRegInfo &TRI = getTargetRegInfo(Img.Arch);
  const llvm::ArrayRef<uint64_t> Registers =
      RegisterArguments
          ? llvm::ArrayRef<uint64_t>(
                TRI.integerArgumentLayout(Img.abiFormat()).Registers)
          : llvm::ArrayRef<uint64_t>();
  const CallArgumentConvention *Convention =
      callArgumentConvention(Img.Arch, Img.abiFormat());
  const bool VectorArguments = RegisterArguments && Img.Arch == Arch::X64 &&
                               Convention &&
                               !Convention->PositionalArgumentSlots;
  // A positional convention passes argument K in slot K's integer or vector
  // register by its class.
  const bool SlotVectors = RegisterArguments && Img.Arch == Arch::X64 &&
                           Convention && Convention->PositionalArgumentSlots &&
                           Convention->VectorArgumentsFromCalleeSummary;
  // The integer arguments of a routine, the widths of its floating ones,
  // which take vector registers, and each argument's floating width in
  // order (0 for an integer one).
  struct Arguments {
    int Integers = 0;
    /// A record may use stack slots while leaving integer registers free.
    std::optional<int> Registers;
    std::vector<uint8_t> Floats;
    std::vector<uint8_t> Order;
    /// The bytes each integer argument declares, in order, where a
    /// prototype gives them.
    std::vector<uint8_t> IntegerBytes;
    /// Integers counts the stack slots every argument fills.
    bool Slots = false;
    /// The routine pops its stack arguments (stdcall).
    bool CalleePops = false;
  };
  // The prototype the program's types declare for a routine of another
  // module.  A positional convention passes a record by value in an integer
  // register or by its address there; another convention's rules for one
  // are not read here.
  auto Declared = [&](const std::string &Name) -> std::optional<FunctionSym> {
    if (!Dbg)
      return std::nullopt;
    return Dbg->resolvePrototype(cNameOfStatedName(Name, Img.Format, Img.Arch));
  };
  auto SignatureArguments =
      [&](const FunctionSym &Function) -> std::optional<Arguments> {
    if (Function.IsVariadic)
      return std::nullopt;
    const bool Positional = Convention && Convention->PositionalArgumentSlots;
    // Where every argument is on the stack (i386), fastcall and thiscall
    // pass leading ones in registers, which these positions do not describe.
    if (!RegisterArguments && (Function.CallConv == DebugCallConv::Fastcall ||
                               Function.CallConv == DebugCallConv::Thiscall))
      return std::nullopt;
    Arguments Result;
    Result.CalleePops = !RegisterArguments &&
                        Function.CallConv == DebugCallConv::Stdcall &&
                        !Function.Params.empty();
    for (const auto &[ParamName, Type] : Function.Params) {
      (void)ParamName;
      if (!Type)
        return std::nullopt;
      // On the stack an argument takes the slots it fills, a record's by
      // value too: a LONGLONG, a double and a POINT take two on i386.
      if (!RegisterArguments) {
        if (!Type->Size ||
            (Type->Kind != NdTypeKind::Int && Type->Kind != NdTypeKind::Ptr &&
             Type->Kind != NdTypeKind::Float &&
             Type->Kind != NdTypeKind::Struct))
          return std::nullopt;
        Result.Integers += static_cast<int>((Type->Size + TRI.PointerSize - 1) /
                                            TRI.PointerSize);
        Result.Order.push_back(0);
        Result.IntegerBytes.push_back(static_cast<uint8_t>(TRI.PointerSize));
        Result.Slots = true;
        continue;
      }
      if (Type->Kind == NdTypeKind::Float &&
          (Type->Size == sizeof(float) || Type->Size == sizeof(double))) {
        Result.Floats.push_back(static_cast<uint8_t>(Type->Size));
        Result.Order.push_back(Result.Floats.back());
        continue;
      }
      // An enumeration passes as the integer it is.
      const bool Integer = Type->Kind == NdTypeKind::Int ||
                           (Type->Kind == NdTypeKind::Struct && Type->IsEnum);
      if (!Integer && Type->Kind != NdTypeKind::Ptr &&
          (Type->Kind != NdTypeKind::Struct || !Positional))
        return std::nullopt;
      // An integer wider than a register takes more than one, an ARM pair's
      // aligned, which these positions do not describe.
      if (Integer && Type->Size > TRI.PointerSize)
        return std::nullopt;
      ++Result.Integers;
      Result.Order.push_back(0);
      // A record takes its whole slot, by value or by its address.
      Result.IntegerBytes.push_back(static_cast<uint8_t>(
          Integer && Type->Size && Type->Size < TRI.PointerSize
              ? Type->Size
              : TRI.PointerSize));
    }
    return Result;
  };
  auto DeclaredArguments =
      [&](const std::string &Name) -> std::optional<Arguments> {
    const std::optional<FunctionSym> Function = Declared(Name);
    return Function ? SignatureArguments(*Function) : std::nullopt;
  };
  // The prototype the user declares for a routine of another module, where
  // it takes only integer arguments: the user may declare a routine of a C
  // library's name that a library of the program's own exports.
  auto UserArguments =
      [&](const std::string &Name) -> std::optional<Arguments> {
    const std::optional<FunctionSym> Function =
        userPrototype(Dbg, cNameOfStatedName(Name, Img.Format, Img.Arch));
    std::optional<Arguments> Result =
        Function ? SignatureArguments(*Function) : std::nullopt;
    if (Result && !Result->Floats.empty())
      return std::nullopt;
    return Result;
  };
  // Whether an export names \p Addr; ExportStarts, where given, lists the
  // address of every export.
  auto Exported = [&](va_t Addr) {
    return ExportStarts ? std::binary_search(ExportStarts->begin(),
                                             ExportStarts->end(), Addr)
                        : Img.findExportAt(Addr) != nullptr;
  };
  // A routine the image exports, whose name binds a prototype
  // (LocalTypesDebugContext), takes what that prototype declares from every
  // caller: its export binds the convention, which a compiler keeps for the
  // callers within the image too, as a Windows system call stub's callers
  // pass the parameters its dispatcher reads from the stack.  A body that
  // reads more keeps it.  Only a convention of register summaries reads it.
  auto ExportedArguments = [&](va_t Addr) -> std::optional<Arguments> {
    if (!Dbg || !RegisterArguments || !Exported(Addr))
      return std::nullopt;
    const std::optional<FunctionSym> Function = Dbg->resolveFunction(Addr);
    if (!Function || Function->PrototypeLibrary.empty())
      return std::nullopt;
    return SignatureArguments(*Function);
  };
  // A runtime's own prototype names its routine exactly, where stripping
  // leading underscores for the arity tables could reach another runtime's.
  auto RoutineArguments =
      [&](const std::string &Name) -> std::optional<Arguments> {
    if (const libc::LibCPrototype *Prototype =
            libc::libcPrototype(Name, Img.abiFormat())) {
      if (Prototype->Variadic)
        return std::nullopt;
      Arguments Result;
      for (const std::string_view Type :
           llvm::ArrayRef(Prototype->Params.data(), Prototype->ParamCount)) {
        if (!libc::isFloatingType(Type)) {
          ++Result.Integers;
          Result.Order.push_back(0);
          continue;
        }
        if (Type != "float" && Type != "double")
          return std::nullopt;
        Result.Floats.push_back(Type == "float" ? sizeof(float)
                                                : sizeof(double));
        Result.Order.push_back(Result.Floats.back());
      }
      return Result;
    }
    // A PE import entry is the C name: MSVC's `_pipe(fds, size, mode)` is
    // not POSIX `pipe(fds)`, which stripping its underscore would reach.
    const auto Arity =
        importNamesAreCNames(Img.Format)
            ? libc::libcArity(Name, libc::libcVariantOf(Img))
            : libc::libcArityForSymbol(Name, libc::libcVariantOf(Img));
    if (!Arity || Arity->IntArgs < 0) {
      if (std::optional<Arguments> Declared = DeclaredArguments(Name))
        return Declared;
      // A variadic routine reads its fixed parameters, integers all, where
      // its variadic arguments follow them in the argument registers
      // (`open(path, flags, ...)`): a caller passing one on untouched passes
      // it as its own parameter.
      if (!Convention || !Convention->VariadicImportArgumentsInRegisters ||
          !libc::isKnownFunction(Name))
        return std::nullopt;
      const unsigned Fixed =
          libc::varArgFixedCount(Name, libc::libcVariantOf(Img));
      if (!Fixed)
        return std::nullopt;
      Arguments Result;
      Result.Integers = static_cast<int>(Fixed);
      Result.Order.assign(Fixed, 0);
      return Result;
    }
    Arguments Result;
    Result.Integers = Arity->IntArgs;
    Result.Floats.assign(Arity->FpArgs,
                         Arity->FpIsFloat ? sizeof(float) : sizeof(double));
    // The tables list the integer arguments first unless FpFirst.
    const std::vector<uint8_t> Integers(Arity->IntArgs, 0);
    Result.Order = Arity->FpFirst ? Result.Floats : Integers;
    const std::vector<uint8_t> &Rest =
        Arity->FpFirst ? Integers : Result.Floats;
    Result.Order.insert(Result.Order.end(), Rest.begin(), Rest.end());
    return Result;
  };
  // Where every argument is on the stack (i386 Windows), the thunk an import
  // library links for a stdcall routine, and its import slot, spell the
  // bytes the routine pops (`_NlsValidateLocale@8`): its arguments, slot by
  // slot, where no declaration gives them.
  std::map<va_t, unsigned> DecoratedBytes;
  if (!RegisterArguments)
    for (const Symbol &Sym : Img.Symbols)
      if (const std::optional<unsigned> Bytes =
              stdcallArgumentBytes(Sym.Name, Img.abiFormat(), Img.Arch);
          Bytes && *Bytes % TRI.PointerSize == 0)
        DecoratedBytes.try_emplace(Sym.Addr, *Bytes);
  auto DecoratedArguments = [&](va_t Addr) -> std::optional<Arguments> {
    const auto It = DecoratedBytes.find(Addr);
    if (It == DecoratedBytes.end())
      return std::nullopt;
    Arguments Result;
    Result.Integers = static_cast<int>(It->second / TRI.PointerSize);
    Result.Order.assign(static_cast<size_t>(Result.Integers), 0);
    Result.IntegerBytes.assign(static_cast<size_t>(Result.Integers),
                               static_cast<uint8_t>(TRI.PointerSize));
    Result.Slots = true;
    Result.CalleePops = true;
    return Result;
  };
  // The floating type the routine returns, by the same declarations.
  auto RoutineFloatReturn = [&](const std::string &Name) -> uint16_t {
    if (const libc::LibCPrototype *Prototype =
            libc::libcPrototype(Name, Img.Format))
      return libc::floatReturnBytes(*Prototype);
    const auto Arity =
        importNamesAreCNames(Img.Format)
            ? libc::libcArity(Name, libc::libcVariantOf(Img))
            : libc::libcArityForSymbol(Name, libc::libcVariantOf(Img));
    if (Arity)
      return libc::floatReturnBytes(*Arity);
    if (const std::optional<FunctionSym> Function = Declared(Name);
        Function && Function->ReturnType &&
        Function->ReturnType->Kind == NdTypeKind::Float &&
        (Function->ReturnType->Size == sizeof(float) ||
         Function->ReturnType->Size == sizeof(double)))
      return Function->ReturnType->Size;
    return 0;
  };
  ImportPrototypeArguments Result;
  std::set<va_t> Seen;
  auto Classify = [&](va_t Addr) {
    if (!Seen.insert(Addr).second)
      return;
    std::string Name = importCalleeName(Img, Addr);
    // A function-pointer variable another module's routine starts at takes
    // that routine's arguments, keyed by the variable's address.
    if (Name.empty() && PointerCallees)
      if (const auto Callee = PointerCallees->find(Addr);
          Callee != PointerCallees->end())
        Name = Callee->second.ImportName;
    std::optional<Arguments> Routine;
    // The summary of an exported routine's own body joins its prototype.
    bool Joins = false;
    bool PrototypeOwnsArguments = false;
    bool UserDeclared = false;
    if (!Name.empty()) {
      if (const uint16_t Bytes = RoutineFloatReturn(Name))
        Result.FloatReturns[Addr] = Bytes;
      // Bind a complete fixed declaration before SSA, including stack values
      // written in predecessor blocks. Counts alone cannot retain those uses.
      if (Convention && Convention->ImportArgumentsFromPrototype)
        if (const auto Function = Declared(Name))
          if (auto Integer = libc::fixedIntegerCallArguments(
                  *Function, Img.Arch, Img.abiFormat())) {
            Result.IntegerArguments.emplace(Addr, std::move(*Integer));
            Result.PrototypeDeclared.insert(Addr);
            if (Function->PrototypeLibrary == UserPrototypeLibrary)
              Result.UserDeclared.insert(Addr);
          }
      // So are those a platform registry authenticates, each with its width
      // (the convention's PrototypeIntegerArguments).
      if (Convention && Convention->PrototypeIntegerArguments &&
          !Result.IntegerArguments.count(Addr))
        if (auto Integer = Convention->PrototypeIntegerArguments(Name))
          Result.IntegerArguments.emplace(Addr, std::move(*Integer));
      Routine = UserArguments(Name);
      UserDeclared = Routine.has_value();
      PrototypeOwnsArguments = Routine.has_value();
      // A variadic header declares a minimum prefix, never an exact arity.
      if (!Routine)
        if (const auto Function = Declared(Name))
          if (auto Prefix = libc::variadicCallArguments(*Function, Img.Arch,
                                                        Img.abiFormat())) {
            GPRReadWidths Widths{};
            for (size_t I = 0; I < Prefix->size() && I < Registers.size(); ++I)
              if (const auto Family = gprFamilyOf(Img.Arch, Registers[I]))
                Widths[*Family] = (*Prefix)[I];
            Result.Reads.emplace(Addr, Widths);
            if (StackArguments && Prefix->size() > Registers.size())
              Result.StackArgs.emplace(Addr, Prefix->size());
            Result.VariadicArguments.emplace(Addr, std::move(*Prefix));
            return;
          }
      // A complete fixed FP header signature gives the SSA call its physical
      // register operands even when the caller only forwards them untouched.
      // Do not encode ARM64 registers in the x64 GPR family summaries below.
      if (!Routine && Convention &&
          (Convention->ArgumentsFromCallSetup ||
           Convention->RegisterArgumentsFromCalleeSummary))
        if (const auto Function = Declared(Name))
          if (auto Floating = libc::floatingCallArguments(*Function, Img.Arch,
                                                          Img.abiFormat())) {
            Result.FloatingArguments.emplace(Addr, std::move(*Floating));
            Result.PrototypeDeclared.insert(Addr);
            if (Function->PrototypeLibrary == UserPrototypeLibrary)
              Result.UserDeclared.insert(Addr);
            if (Convention->ArgumentsFromCallSetup &&
                !Convention->RegparmOnlyForInternalCalls)
              return;
          }
      // Header arity is a count of source parameters. A record's pieces take
      // the target ABI's registers or stack positions before those tables.
      if (!Routine)
        if (const auto Function = Declared(Name))
          if (const auto Layout = libc::recordCallArguments(
                  *Function, Img.Arch, Img.abiFormat(),
                  libc::libcVariantOf(Img))) {
            Arguments Record;
            Record.Integers = static_cast<int>(Layout->Positions);
            Record.Registers = static_cast<int>(Layout->Registers);
            Record.Order.assign(Layout->Positions, 0);
            Record.Slots = !RegisterArguments;
            Record.CalleePops = !RegisterArguments &&
                                Function->CallConv == DebugCallConv::Stdcall;
            Routine = std::move(Record);
            PrototypeOwnsArguments = true;
          }
      if (!Routine)
        Routine = RoutineArguments(Name);
      if (!Routine)
        Routine = DecoratedArguments(Addr);
    } else {
      Routine = ExportedArguments(Addr);
      if (!Routine)
        return;
      Joins = Effects.count(Addr) != 0;
      PrototypeOwnsArguments = true;
    }
    if (Routine && !Routine->Floats.empty() && !SlotVectors &&
        (!VectorArguments ||
         Routine->Floats.size() > kX64VectorArgumentFamilies ||
         Routine->Integers > static_cast<int>(Registers.size())))
      return;
    // Positions take every argument in a positional convention, else the
    // integer ones.
    const std::optional<int> Count =
        !Routine      ? std::nullopt
        : SlotVectors ? std::optional<int>(Routine->Order.size())
                      : std::optional<int>(Routine->Integers);
    const int InRegisters =
        Routine && Routine->Registers
            ? *Routine->Registers
            : std::min(Count.value_or(0), static_cast<int>(Registers.size()));
    if (!Count || (*Count > InRegisters && !StackArguments))
      return;
    // Setup-only conventions currently publish one dense count. A signature
    // whose register padding needs a separate count cannot use that channel.
    if (Convention && Convention->ArgumentsFromCallSetup &&
        Routine->Registers &&
        InRegisters != std::min(*Count, static_cast<int>(Registers.size())))
      return;
    GPRReadWidths Widths{};
    // An import's integer arguments are read pointer-wide; an exported
    // routine's body reads what its prototype declares, which joins its own
    // reads: a BOOLEAN's caller may set only its low byte.
    size_t Integer = 0;
    for (int K = 0; K < InRegisters; ++K) {
      if (SlotVectors && Routine->Order[K]) {
        Widths[kX64VectorFamilyBase + K] = Routine->Order[K];
        continue;
      }
      const uint8_t Bytes = Joins && Integer < Routine->IntegerBytes.size()
                                ? Routine->IntegerBytes[Integer]
                                : static_cast<uint8_t>(TRI.PointerSize);
      ++Integer;
      if (const auto Family = gprFamilyOf(Img.Arch, Registers[K]))
        Widths[*Family] = Bytes;
    }
    if (!SlotVectors)
      for (size_t K = 0; K < Routine->Floats.size(); ++K)
        Widths[kX64VectorFamilyBase + K] = Routine->Floats[K];
    (Joins ? Result.DeclaredReads : Result.Reads)[Addr] = Widths;
    // Counted as positions, the registers' included, as the stack summary
    // counts them (LocalRegisterEffect::StackArgs); 0 for none on the stack.
    if (StackArguments)
      (Joins ? Result.DeclaredStackArgs : Result.StackArgs)[Addr] =
          *Count > InRegisters ? *Count : 0;
    if (RegisterArguments && !Name.empty() && Convention &&
        Convention->ArgumentsFromCallSetup && Routine->Floats.empty())
      Result.Counts[Addr] = *Count;
    if (StackArguments && !RegisterArguments && !Joins &&
        (Routine->Slots || Routine->Order.empty())) {
      Result.StackSlots.insert(Addr);
      if (Routine->CalleePops && *Count > 0)
        Result.StackPops[Addr] = *Count * static_cast<int>(TRI.PointerSize);
    }
    if (PrototypeOwnsArguments) {
      Result.PrototypeDeclared.insert(Addr);
      if (UserDeclared)
        Result.UserDeclared.insert(Addr);
    }
  };
  // Whether a call's target is a value the caller computed: an indirect call
  // through one, or a dispatcher call, which MedIR gives its target register.
  bool ComputedTargets = false;
  for (const LowFunc &F : Funcs)
    for (const LowBlock &Block : F.Blocks)
      for (size_t I = 0; I < Block.Ops.size(); ++I) {
        const LowOp &Op = Block.Ops[I];
        if ((Op.Opcode != NdOp::CALL && Op.Opcode != NdOp::INDIR_CALL) ||
            Op.NumInputs == 0)
          continue;
        if (Op.Inputs[0].isConst()) {
          Classify(Op.Inputs[0].Offset);
          ComputedTargets |=
              DispatchThunks && DispatchThunks->count(Op.Inputs[0].Offset);
          continue;
        }
        ComputedTargets |= Op.Opcode == NdOp::INDIR_CALL;
        // A register loaded from the slot names the import as the slot does
        // (LowToMed's RegisterCallSlot).
        if (Op.Opcode == NdOp::INDIR_CALL && Op.Inputs[0].isReg())
          if (const std::optional<va_t> Slot =
                  loadedCallSlot(F, Block, I, Op.Inputs[0]))
            Classify(*Slot);
      }
  // An import's declaration belongs to the image, independently of whether
  // the caller loads its IAT word directly or caches that value in its frame.
  // Late target discovery (MedCallTarget.h) must find the same pre-SSA
  // argument contract for any slot such a computed target was loaded from.
  // Without one, no call can observe the imports nothing above names.
  if (ComputedTargets) {
    for (const auto &Import : Img.Imports)
      if (Import.IATAddr)
        Classify(Import.IATAddr);
    for (const auto &[Slot, Name] : Img.ImportPtrSlots)
      Classify(Slot);
  }
  // Addresses no call above names, classified as a call to them would be.
  if (AlsoClassify)
    for (const va_t Addr : *AlsoClassify)
      Classify(Addr);
  for (const auto &[Entry, Effect] : Effects) {
    Classify(Entry);
    for (const RegisterBlock &Block : Effect->Blocks)
      for (const RegisterStep &Step : Block.Steps) {
        if (Step.ImportCallee != InvalidVA)
          Classify(Step.ImportCallee);
        // A callee lifted only for its summary calls through a variable too.
        if (Step.ArgumentsFrom != InvalidVA)
          Classify(Step.ArgumentsFrom);
      }
  }
  return Result;
}

/// Summarize which GPRs each direct callee may write (CallRegisterEffects.h).
/// Callees outside Result.LowFuncs are lifted here with the same CFG settings,
/// up to the Limits.h depth and count; beyond those they stay unsummarized.
/// An import reads the arguments its prototype declares, which \p Dbg gives
/// where the libc tables know none of its name.
/// What a call through each function-pointer variable of \p PointerCallees
/// passes the arguments of (PipelineResult::CallArgumentSources): another
/// module's routine, keyed by the variable's address, or this image's, by
/// its entry.
std::map<va_t, va_t> callArgumentSourcesOf(
    const BinaryImage &Img,
    const std::map<va_t, PointerVariableCallee> &PointerCallees) {
  std::map<va_t, va_t> Sources;
  for (const auto &[Slot, Callee] : PointerCallees)
    Sources.emplace(Slot, Callee.ImportName.empty() ? Callee.LocalEntry : Slot);
  // A register loaded from an import slot reads the same declared arguments
  // as a call through the slot itself. This proves its ABI, not its target.
  for (const auto &Import : Img.Imports)
    if (Import.IATAddr && !Import.Name.empty())
      Sources.try_emplace(Import.IATAddr, Import.IATAddr);
  return Sources;
}

/// The argument counts of every prototyped import of \p Img, as \p Kept
/// holds them where they were read from the image as it is with \p Dbg; read
/// now, and kept there, otherwise (PipelineAnalysisCache::ImportCounts).
std::shared_ptr<const ImageImportCounts> imageImportArguments(
    const BinaryImage &Img, const DebugContext *Dbg,
    const std::map<va_t, PointerVariableCallee> &PointerCallees,
    std::shared_ptr<const ImageImportCounts> *Kept) {
  std::shared_ptr<const ImageImportCounts> Everywhere =
      Kept && *Kept && (*Kept)->readFrom(Img, Dbg) ? *Kept : nullptr;
  if (!Everywhere) {
    std::vector<va_t> Addresses;
    for (const auto &[Addr, Name] : Img.getImportAddressNames())
      Addresses.push_back(Addr);
    for (const RelocationEntry &Rel : Img.Relocations)
      if (!Rel.SymbolName.empty())
        Addresses.push_back(Rel.Address);
    for (const auto &[Slot, Callee] : PointerCallees)
      Addresses.push_back(Slot);
    for (const Export &Exp : Img.Exports)
      Addresses.push_back(Exp.Addr);
    const auto *Convention = callArgumentConvention(Img.Arch, Img.abiFormat());
    ImportPrototypeArguments Prototyped = importPrototypeArguments(
        Img, {}, {}, /*StackArguments=*/true, Dbg,
        /*RegisterArguments=*/!Convention ||
            !Convention->RegparmOnlyForInternalCalls,
        &PointerCallees, /*ExportStarts=*/nullptr, &Addresses);
    Everywhere = std::make_shared<const ImageImportCounts>(ImageImportCounts{
        &Img, Dbg, Img.Imports.data(), Img.Imports.size(),
        Img.Relocations.data(), Img.Relocations.size(), Img.Exports.data(),
        Img.Exports.size(), std::move(Prototyped.Counts),
        std::move(Prototyped.StackArgs), std::move(Prototyped.StackSlots),
        std::move(Prototyped.FloatingArguments),
        std::move(Prototyped.IntegerArguments),
        std::move(Prototyped.VariadicArguments)});
    if (Kept)
      *Kept = Everywhere;
  }
  return Everywhere;
}

/// Omitted callees read the image's declarations, including imports absent
/// from the selection. Warming and demand conversion share that same input.
void setOmittedImportArguments(
    const BinaryImage &Img, const DebugContext *Dbg,
    const std::map<va_t, PointerVariableCallee> &PointerCallees,
    std::shared_ptr<const ImageImportCounts> *Kept, PipelineResult &Result) {
  const auto *Convention = callArgumentConvention(Img.Arch, Img.abiFormat());
  if (!Convention || !Convention->ImportArgumentsFromPrototype ||
      !Convention->ArgumentsFromCallSetup)
    return;
  const auto Arguments = imageImportArguments(Img, Dbg, PointerCallees, Kept);
  Result.OmittedCalleeVariadicArguments =
      std::shared_ptr<const std::map<va_t, std::vector<uint16_t>>>(
          Arguments, &Arguments->VariadicArguments);
  if (Convention->RegparmOnlyForInternalCalls) {
    Result.OmittedCalleeStackArgs = std::shared_ptr<const std::map<va_t, int>>(
        Arguments, &Arguments->StackArgs);
    Result.OmittedCalleeStackSlots = std::shared_ptr<const std::set<va_t>>(
        Arguments, &Arguments->StackSlots);
  } else {
    Result.OmittedCalleeArgumentCounts =
        std::shared_ptr<const std::map<va_t, int>>(Arguments,
                                                   &Arguments->Counts);
    Result.OmittedCalleeIntegerArguments =
        std::shared_ptr<const std::map<va_t, FixedIntegerCallArguments>>(
            Arguments, &Arguments->IntegerArguments);
    Result.OmittedCalleeFloatingArguments =
        std::shared_ptr<const std::map<va_t, FloatingCallArguments>>(
            Arguments, &Arguments->FloatingArguments);
  }
}

static LocalRegisterEffect summarizeRecoveredCallRegisters(
    const BinaryImage &Img, const CallRegisterBodyView &Body,
    const libc::NoReturnTargetIndex &NoReturnTargets,
    const std::map<va_t, GPRReadWidths> &FixedCallReads,
    const std::map<va_t, va_t> &ArgumentSources,
    const libc::StackProbeIndex &StackProbes) {
  // Late MedIR target recovery cannot repair a summary that already treated
  // this call as opaque. Demand analysis and cache warmup use the same proof.
  // Only this private summary view changes; lifting retains the original
  // machine operand, especially a captured mutable import.  A body with no
  // such call is summarized in place.
  std::optional<CallRegisterBody> Projection;
  size_t Work = limits::ir::kSourceFrameAnalysisRemaining;
  std::optional<std::map<SourceCallOccurrenceKey, va_t>> ScalarTargets;
  if (!Img.IsRelocatable)
    for (size_t B = 0; B < Body.Blocks.size(); ++B) {
      auto Targets =
          resolveLowBlockCallTargets(Body.Blocks[B], Img.Arch, &Work);
      for (size_t I = 0; I < Body.Blocks[B].Ops.size(); ++I)
        if (const auto &Call = Body.Blocks[B].Ops[I];
            !Targets.count(I) && Call.Opcode == NdOp::INDIR_CALL &&
            Call.NumInputs == 1 && !Call.Inputs[0].isConst()) {
          const auto Site = sourceCallOccurrenceKey(Call);
          if (!Site)
            continue;
          if (!ScalarTargets)
            ScalarTargets = constantRegisterCallTargets(Body);
          if (const auto Found = ScalarTargets->find(*Site);
              Found != ScalarTargets->end())
            Targets.try_emplace(I, MedCallTarget{Found->second, false});
        }
      for (const auto &[Index, Target] : Targets) {
        if (Target.ThroughSlot ? importCalleeName(Img, Target.Address).empty()
                               : !Img.hasExecutableCodeOwnerAt(Target.Address))
          continue;
        if (!Projection) {
          Projection.emplace();
          Projection->Blocks.assign(Body.Blocks.begin(), Body.Blocks.end());
          Projection->Complete = Body.Complete;
          Projection->UnsafeIndirectBranches = Body.UnsafeIndirectBranches;
          Projection->HasIndependentRoots = Body.HasIndependentRoots;
        }
        auto &Call = Projection->Blocks[B].Ops[Index];
        // This private view must also expose an import's register reads.
        // A caller forwarding RCX still owns that incoming parameter even
        // when the callable was transported through a frame slot.
        Call.Opcode = Target.ThroughSlot ? NdOp::INDIR_CALL : NdOp::CALL;
        Call.Inputs[0] = NdVar::cst(Target.Address, Call.Inputs[0].Size);
      }
    }
  return localRegisterEffect(
      Img, Projection ? CallRegisterBodyView(*Projection) : Body,
      &NoReturnTargets, &FixedCallReads, &ArgumentSources, &StackProbes);
}

void computeCallRegisterEffects(
    const BinaryImage &Img, const std::set<va_t> &FuncEntries,
    const libc::NoReturnTargetIndex &NoReturnTargets,
    const libc::StackProbeIndex &StackProbes,
    const NoReturnCalleeProver &NoReturnCallees,
    const detail::AbsoluteRelocationRootIndex &AbsoluteRelocationRoots,
    const ExecutableCodeOwnerIndex *CodeOwnerIndex,
    const std::shared_ptr<X87CallGraphCache> &X87Graphs,
    const std::map<va_t, PointerVariableCallee> &PointerCallees,
    pipeline_detail::CalleeFactsCache &KnownCallees, const DebugContext *Dbg,
    const std::map<va_t, std::string> *FunctionNames,
    const std::vector<va_t> *ExportStarts,
    std::shared_ptr<const ImageImportCounts> *KeptImportCounts,
    PipelineResult &Result) {
  // A call through a function-pointer variable passes the arguments of the
  // routine the variable starts at (PointerCallees): another module's, whose
  // prototype is kept under the variable's address, or this image's, by its
  // entry.
  Result.CallArgumentSources = callArgumentSourcesOf(Img, PointerCallees);
  Result.CallPrototypeArguments.clear();
  Result.CallUserPrototypes.clear();
  if (Img.Arch != Arch::X64) {
    // Format bindings carry explicit operands into MedIR independently of
    // the x64-only register-effect summaries below.
    for (const LowFunc &Function : Result.LowFuncs)
      pipeline_detail::collectFormattedCalls(Img, Function,
                                             Result.FormattedCalls);
    // Where an import takes every argument on the stack (i386: regparm is
    // for internal calls only), there are no register summaries, but a
    // prototyped import still reads its count of stack arguments.
    if (const CallArgumentConvention *Convention =
            callArgumentConvention(Img.Arch, Img.abiFormat());
        Convention && Convention->ImportArgumentsFromPrototype &&
        Convention->StackArgumentSummary &&
        Convention->RegparmOnlyForInternalCalls) {
      ImportPrototypeArguments Prototyped = importPrototypeArguments(
          Img, Result.LowFuncs, {}, /*StackArguments=*/true, Dbg,
          /*RegisterArguments=*/false, &PointerCallees);
      Result.CallEntryStackArgs = std::move(Prototyped.StackArgs);
      Result.CallEntryStackSlots = std::move(Prototyped.StackSlots);
      Result.CallEntryStackPops = std::move(Prototyped.StackPops);
      Result.CallEntryFloatingArguments =
          std::move(Prototyped.FloatingArguments);
      Result.CallEntryIntegerArguments = std::move(Prototyped.IntegerArguments);
      Result.CallFloatReturns = std::move(Prototyped.FloatReturns);
      Result.CallPrototypeArguments = std::move(Prototyped.PrototypeDeclared);
      Result.CallUserPrototypes = std::move(Prototyped.UserDeclared);
      Result.CallEntryVariadicArguments =
          std::move(Prototyped.VariadicArguments);
    } else if (Convention && Convention->ImportArgumentsFromPrototype &&
               Convention->ArgumentsFromCallSetup) {
      // Where the setup of each call gives it its arguments (Windows on
      // ARM64), a prototyped import's declaration still counts them.
      ImportPrototypeArguments Prototyped = importPrototypeArguments(
          Img, Result.LowFuncs, {}, /*StackArguments=*/true, Dbg,
          /*RegisterArguments=*/true, &PointerCallees);
      Result.CallEntryArgumentCounts = std::move(Prototyped.Counts);
      Result.CallEntryFloatingArguments =
          std::move(Prototyped.FloatingArguments);
      Result.CallEntryIntegerArguments = std::move(Prototyped.IntegerArguments);
      Result.CallFloatReturns = std::move(Prototyped.FloatReturns);
      Result.CallPrototypeArguments = std::move(Prototyped.PrototypeDeclared);
      Result.CallUserPrototypes = std::move(Prototyped.UserDeclared);
      Result.CallEntryVariadicArguments =
          std::move(Prototyped.VariadicArguments);
    }
    setOmittedImportArguments(Img, Dbg, PointerCallees, KeptImportCounts,
                              Result);
    return;
  }
  // Each function's effect.  A callee's is the one its kept facts hold,
  // which the run reads in place (Kept holds them); the run copies an
  // effect only to change it.
  LocalRegisterEffectRefs Effects;
  std::vector<std::shared_ptr<const void>> Kept;
  std::map<va_t, int> Depth;
  std::vector<va_t> Work;
  std::set<va_t> GuardForwarders;
  // A call a constant format describes reads exactly what it names; the
  // functions MedIR converts also pass those arguments (FormattedCall).
  std::map<va_t, GPRReadWidths> FixedCallReads;
  auto CollectFormattedCalls = [&](const CallRegisterBodyView &Body,
                                   bool Converted) {
    auto Reads = pipeline_detail::formattedCallReads(
        Img, Body, Converted ? &Result.FormattedCalls : nullptr);
    pipeline_detail::mergeFixedCallReads(FixedCallReads, Reads);
    return Reads;
  };
  auto Summarize = [&](const CallRegisterBodyView &Body) {
    return summarizeRecoveredCallRegisters(
        Img, Body, NoReturnTargets, FixedCallReads, Result.CallArgumentSources,
        StackProbes);
  };
  // A callee's body as a run takes it: retained and validated, or lifted
  // with the run's settings.
  auto BuildCallee = [&](Decoder &Dec, va_t Entry) {
    CFGBuilder ExtraCFG;
    ExtraCFG.setKnownFuncEntries(&FuncEntries);
    ExtraCFG.setNoReturnTargetIndex(&NoReturnTargets);
    ExtraCFG.setNoReturnCalleeProver(&NoReturnCallees);
    ExtraCFG.setAbsoluteRelocationRootIndex(&AbsoluteRelocationRoots);
    ExtraCFG.setExecutableCodeOwnerIndex(CodeOwnerIndex);
    ExtraCFG.setX87CallGraphCache(X87Graphs);
    // Named only when built: the lookup scans every export and symbol, and
    // most callees come from the cache.
    return ExtraCFG.build(Img, Dec, Entry, Img.getFunctionNameAt(Entry));
  };
  for (const LowFunc &LF : Result.LowFuncs) {
    CollectFormattedCalls(CallRegisterBodyView(LF), /*Converted=*/true);
    auto Effect = std::make_shared<const LocalRegisterEffect>(
        Summarize(CallRegisterBodyView(LF)));
    Effects[LF.Entry] = Effect.get();
    Kept.push_back(std::move(Effect));
    if (forwardsToGuardDispatch(Img, LF.Blocks))
      GuardForwarders.insert(LF.Entry);
    Depth[LF.Entry] = 0;
    Work.push_back(LF.Entry);
  }
  size_t ExtraLifts = 0;
  // Reserve callees one breadth-first frontier at a time in the original
  // caller/address order. Discovery, budget admission and summary publication
  // remain deterministic; only independent CFG builds run concurrently.
  for (size_t Next = 0; Next < Work.size();) {
    const size_t FrontierEnd = Work.size();
    std::vector<va_t> Pending;
    for (; Next < FrontierEnd; ++Next) {
      const va_t Caller = Work[Next];
      const int CalleeDepth = Depth[Caller] + 1;
      const LocalRegisterEffect &CallerEffect = *Effects.at(Caller);
      std::set<va_t> Callees = CallerEffect.Callees;
      Callees.insert(CallerEffect.NoReturnCallees.begin(),
                     CallerEffect.NoReturnCallees.end());
      // A call through a function-pointer variable reads the arguments of
      // the routine of this image the variable starts at.
      for (const RegisterBlock &Block : CallerEffect.Blocks)
        for (const RegisterStep &Step : Block.Steps)
          if (Step.ArgumentsFrom != InvalidVA)
            Callees.insert(Step.ArgumentsFrom);
      for (va_t Callee : Callees) {
        if (Depth.count(Callee) ||
            CalleeDepth > limits::kMaxCallEffectCalleeDepth ||
            ExtraLifts >= limits::kMaxCallEffectExtraLifts ||
            !Img.hasExecutableCodeOwnerAt(Callee))
          continue;
        ++ExtraLifts;
        Depth[Callee] = CalleeDepth;
        Pending.push_back(Callee);
      }
    }
    // Give parallel workers enough admitted bodies to overlap costly callees
    // across the old eight-body barriers. Publication remains in Pending
    // order, and the next breadth-first frontier is still discovered only
    // after this one finishes. Retain at most 32 bodies and four active CFG
    // builders; a serial run keeps its smaller eight-body working set.
    const size_t BatchSize =
        workerThreadCount() > 1
            ? ::neverd::limits::pipeline::
                  kPipelineLowIRComputeCallRegisterEffectsBatchSizeSelected
            : ::neverd::limits::pipeline::
                  kPipelineLowIRComputeCallRegisterEffectsBatchSizeDefault;
    for (size_t Begin = 0; Begin < Pending.size(); Begin += BatchSize) {
      const size_t Count = std::min(BatchSize, Pending.size() - Begin);
      std::vector<LowFunc> Bodies(Count);
      std::vector<std::shared_ptr<const CallRegisterBody>> CachedBodies(Count);
      std::vector<std::shared_ptr<const pipeline_detail::CalleeFacts>> Known(
          Count);
      std::vector<uint64_t> Weights(Count, 0);
      std::atomic<bool> Initialized{true};
      uint64_t Bytes = 0;
      for (size_t I = 0; I < Count; ++I) {
        const va_t Entry = Pending[Begin + I];
        Known[I] = KnownCallees.find(Entry);
        if (Known[I])
          continue;
        const va_t End = Img.getFunctionMetadataEnd(Entry, CodeOwnerIndex);
        if (End != InvalidVA && End > Entry)
          Weights[I] = std::min<uint64_t>(End - Entry, 1u << 20);
        Bytes += Weights[I];
      }
      parallelForEachWeighted(
          Weights,
          [&](auto Claim, size_t N) {
            // A worker opens a decoder only for a body it builds: a warmed
            // run knows most callees from their facts or kept bodies.
            std::optional<Decoder> ExtraDec;
            for (size_t I; (I = Claim()) < N;) {
              if (Known[I])
                continue;
              const va_t Entry = Pending[Begin + I];
              CachedBodies[I] = CFGBuilder::cachedCallRegisterBody(
                  X87Graphs, Img, Entry, &FuncEntries, &NoReturnTargets,
                  &NoReturnCallees, &AbsoluteRelocationRoots, CodeOwnerIndex);
              if (CachedBodies[I])
                continue;
              if (!ExtraDec && !ExtraDec.emplace().init(Img)) {
                Initialized.store(false, std::memory_order_relaxed);
                return;
              }
              Bodies[I] = BuildCallee(*ExtraDec, Entry);
            }
          },
          Bytes >= ::neverd::limits::pipeline::kPipelineLowIRMinParallelBytes
              ? 2
              : limits::kMinParallelIRWorkItems,
          /*MaxThreads=*/4);
      if (!Initialized.load(std::memory_order_relaxed))
        return;
      for (size_t I = 0; I < Count; ++I) {
        const va_t Callee = Pending[Begin + I];
        if (const auto &Facts = Known[I]) {
          // What an earlier run or warming read of the body stands while
          // this run gives its calls the same fixed arguments.
          pipeline_detail::mergeFixedCallReads(FixedCallReads,
                                               Facts->FormattedReads);
          if (pipeline_detail::consultedReadsHold(*Facts, FixedCallReads)) {
            Effects[Callee] = &Facts->Effect;
            Kept.push_back(Facts);
            if (Facts->ForwardsToGuardDispatch)
              GuardForwarders.insert(Callee);
            Work.push_back(Callee);
            continue;
          }
          CachedBodies[I] = CFGBuilder::cachedCallRegisterBody(
              X87Graphs, Img, Callee, &FuncEntries, &NoReturnTargets,
              &NoReturnCallees, &AbsoluteRelocationRoots, CodeOwnerIndex);
          if (!CachedBodies[I]) {
            Decoder Dec;
            if (!Dec.init(Img))
              return;
            Bodies[I] = BuildCallee(Dec, Callee);
          }
        }
        const CallRegisterBodyView Body =
            CachedBodies[I] ? CallRegisterBodyView(*CachedBodies[I])
                            : CallRegisterBodyView(Bodies[I]);
        auto Reads = CollectFormattedCalls(Body, /*Converted=*/false);
        auto Facts = std::make_shared<pipeline_detail::CalleeFacts>();
        Facts->Effect = Summarize(Body);
        Effects[Callee] = &Facts->Effect;
        Kept.push_back(Facts);
        const bool Forwards = forwardsToGuardDispatch(Img, Body.Blocks);
        if (Forwards)
          GuardForwarders.insert(Callee);
        Facts->FormattedReads = std::move(Reads);
        Facts->Consulted =
            pipeline_detail::consultedCallReads(Body.Blocks, FixedCallReads);
        Facts->ForwardsToGuardDispatch = Forwards;
        KnownCallees.insert(Callee, std::move(Facts), /*Evict=*/true);
        Work.push_back(Callee);
      }
    }
  }
  auto Family = [](uint64_t RegOff) {
    return GPRFamilyMask(1) << (RegOff / 8);
  };
  const bool Win64 = Img.abiFormat() == BinaryFormat::COFF;
  GPRFamilyMask Volatile = Family(x86reg::RAX) | Family(x86reg::RCX) |
                           Family(x86reg::RDX) | Family(x86reg::R8) |
                           Family(x86reg::R9) | Family(x86reg::R10) |
                           Family(x86reg::R11);
  GPRFamilyMask Arguments = Family(x86reg::RCX) | Family(x86reg::RDX) |
                            Family(x86reg::R8) | Family(x86reg::R9);
  if (!Win64) {
    Volatile |= Family(x86reg::RSI) | Family(x86reg::RDI);
    Arguments |= Family(x86reg::RSI) | Family(x86reg::RDI);
  }
  // A call clobbers the vector argument registers Win64 does not preserve
  // (XMM6 and up are callee-saved there).  Code no summary describes takes
  // no vector argument: an import that does has a prototype.
  Volatile |= vectorArgumentFamilies(Win64 ? 6 : kX64VectorArgumentFamilies);
  // Documented contracts (WindowsKernelRoutines.inc): a Control Flow Guard
  // dispatcher is an indirect call, and a WDK routine reads exactly its
  // prototype's parameters however its body forwards registers.  A forwarder
  // to the load configuration's dispatch pointer is a dispatcher by that
  // pointer's contract, whatever its name.
  std::set<va_t> DispatchThunks;
  std::map<va_t, GPRReadWidths> FixedEntryReads;
  std::map<va_t, int> FixedEntryStackArgs;
  std::map<va_t, FixedIntegerCallArguments> FixedEntryIntegerArguments;
  if (Win64) {
    DispatchThunks = GuardForwarders;
    const auto *Convention = callArgumentConvention(Img.Arch, Img.abiFormat());
    auto Classify = [&](va_t Entry, llvm::StringRef Name) {
      if (Name.empty() || Entry == InvalidVA)
        return;
      if (libc::isIndirectCallDispatchThunk(Name)) {
        DispatchThunks.insert(Entry);
        return;
      }
      const libc::WindowsKernelPrototype *Proto =
          libc::windowsKernelPrototype(Name);
      if (!Proto)
        return;
      Result.CallPrototypeArguments.insert(Entry);
      // Internal named routines use the same complete operand contract as
      // imports. Counts alone leave register-only calls open to incidental
      // frame stores during later argument recovery.
      if (Convention && Convention->PrototypeIntegerArguments)
        if (auto Integer = Convention->PrototypeIntegerArguments(Name))
          FixedEntryIntegerArguments.emplace(Entry, std::move(*Integer));
      static constexpr uint64_t Win64Args[] = {x86reg::RCX, x86reg::RDX,
                                               x86reg::R8, x86reg::R9};
      GPRReadWidths Widths{};
      for (unsigned I = 0; I < 4 && I < Proto->ArgCount; ++I)
        Widths[Win64Args[I] / 8] = Proto->ArgWidths[I];
      FixedEntryReads[Entry] = Widths;
      FixedEntryStackArgs[Entry] =
          Proto->ArgCount > std::size(Win64Args) ? Proto->ArgCount : 0;
    };
    std::set<va_t> Callees;
    for (const auto &[Entry, Effect] : Effects)
      Callees.insert(Entry);
    if (FunctionNames) {
      // What functionNamesAt gives these: the analysis cache read every
      // named address of the image once.
      for (const va_t Entry : Callees)
        if (const auto Named = FunctionNames->find(Entry);
            Named != FunctionNames->end())
          Classify(Entry, Named->second);
        else
          Classify(Entry, (kAutoFuncPrefix + llvm::utohexstr(Entry)).str());
    } else {
      for (const auto &[Entry, Name] : functionNamesAt(Img, Callees))
        Classify(Entry, Name);
    }
    for (const Import &Imp : Img.Imports)
      Classify(Imp.IATAddr, Imp.Name);
  }
  // A libc import with a fixed prototype reads its parameters however the
  // code reaches it; a documented contract above decides first.
  if (const CallArgumentConvention *Convention =
          callArgumentConvention(Img.Arch, Img.abiFormat());
      Convention && Convention->ImportArgumentsFromPrototype) {
    ImportPrototypeArguments Prototyped = importPrototypeArguments(
        Img, Result.LowFuncs, Effects, Convention->StackArgumentSummary, Dbg,
        /*RegisterArguments=*/true, &PointerCallees, ExportStarts,
        /*AlsoClassify=*/nullptr, &DispatchThunks);
    // An exported body joins its declared reads into the entry summary.
    // Its recovered body alone cannot later truncate those arguments.
    for (va_t Addr : Prototyped.PrototypeDeclared)
      if (!DispatchThunks.count(Addr))
        Result.CallPrototypeArguments.insert(Addr);
    // The user's declaration of an import decides where no documented
    // contract does.
    for (auto &[Addr, Widths] : Prototyped.Reads)
      if (!DispatchThunks.count(Addr) &&
          FixedEntryReads.try_emplace(Addr, Widths).second &&
          Prototyped.PrototypeDeclared.count(Addr)) {
        Result.CallPrototypeArguments.insert(Addr);
        if (Prototyped.UserDeclared.count(Addr))
          Result.CallUserPrototypes.insert(Addr);
      }
    for (auto &[Addr, Count] : Prototyped.StackArgs)
      if (!DispatchThunks.count(Addr))
        FixedEntryStackArgs.try_emplace(Addr, Count);
    // The run's own copy of an effect it changes.
    auto Change = [&](const LocalRegisterEffect *&Effect) {
      auto Changed = std::make_shared<LocalRegisterEffect>(*Effect);
      Effect = Changed.get();
      Kept.push_back(Changed);
      return Changed.get();
    };
    // An exported routine's body reads its declared parameters at entry, as
    // its callers pass them, whatever else it reads.
    for (const auto &[Addr, Widths] : Prototyped.DeclaredReads)
      if (auto Effect = Effects.find(Addr); Effect != Effects.end() &&
                                            !Effect->second->Blocks.empty() &&
                                            !FixedEntryReads.count(Addr)) {
        RegisterStep Declared;
        Declared.Reads = Widths;
        std::vector<RegisterStep> &Steps =
            Change(Effect->second)->Blocks.front().Steps;
        Steps.insert(Steps.begin(), Declared);
      }
    for (const auto &[Addr, Count] : Prototyped.DeclaredStackArgs)
      if (auto Effect = Effects.find(Addr);
          Effect != Effects.end() && !Effect->second->UnknownStackReads &&
          Count > Effect->second->StackArgs)
        Change(Effect->second)->StackArgs = Count;
    Result.CallFloatReturns = std::move(Prototyped.FloatReturns);
    Result.CallEntryFloatingArguments = std::move(Prototyped.FloatingArguments);
    Result.CallEntryIntegerArguments = std::move(FixedEntryIntegerArguments);
    Result.CallEntryIntegerArguments.merge(Prototyped.IntegerArguments);
    Result.CallEntryVariadicArguments = std::move(Prototyped.VariadicArguments);
  }
  CallRegisterSummaries Summaries =
      solveCallRegisterEffects(Effects, Volatile, Arguments, DispatchThunks,
                               FixedEntryReads, FixedEntryStackArgs);
  Result.CallDispatchThunks = std::move(DispatchThunks);
  Result.CallMayWriteGPRs = std::move(Summaries.MayWrite);
  Result.CallExitConstants = std::move(Summaries.ExitConstants);
  for (const auto &[Entry, Effect] : Effects)
    if (Effect->RegisterLeaf)
      Result.CallRegisterLeaves.emplace(Entry, Effect->RegisterLeaf);
  Result.CallEntryReadGPRs = std::move(Summaries.EntryReads);
  Result.CallUnreadGPRs = std::move(Summaries.Unread);
  Result.CallEntryStackArgs = std::move(Summaries.EntryStackArgs);
  Result.CallVariadicFrom = std::move(Summaries.VariadicFrom);
}
} // namespace

struct PipelineAnalysisCache::Impl {
  static std::map<std::string, uint16_t>
  declaredX87Imports(const BinaryImage &Image, const DebugContext *Debug) {
    std::map<std::string, uint16_t> Returns;
    if (Image.Arch != Arch::X86 || !Debug)
      return Returns;
    for (const auto &Import : Image.Imports)
      if (const auto Function = Debug->resolvePrototype(
              cNameOfStatedName(Import.Name, Image.Format, Image.Arch)))
        if (const auto Signature = libc::floatingCallArguments(
                *Function, Image.Arch, Image.abiFormat()))
          Returns.emplace(Import.Name, Signature->ReturnBytes);
    return Returns;
  }

  /// These indexes read the image, not a selection's extra entry boundaries.
  /// Keep them while replacing SelectionFacts for another unnamed function.
  struct ImageInputs {
    const BinaryImage *Image;
    DebugContext *Debug;
    const std::shared_ptr<const SymbolInventory> Inventory;
    const std::set<uint64_t> VerifiedEntries;
    libc::NoReturnTargetIndex Targets;
    libc::StackProbeIndex StackProbes;
    const std::map<va_t, PointerVariableCallee> PointerCallees;
    const std::map<std::string, uint16_t> X87FloatingImports;
    detail::AbsoluteRelocationRootIndex Roots;
    ExecutableCodeOwnerIndex Owners;

    ImageInputs(const BinaryImage &Img, DebugContext *Dbg,
                std::shared_ptr<const SymbolInventory> Inventory)
        : Image(&Img), Debug(Dbg), Inventory(std::move(Inventory)),
          VerifiedEntries(Img.VerifiedFunctionEntries),
          Targets(
              Img,
              Dbg ? libc::NoReturnTargetIndex::NameResolver(
                        [Dbg](va_t Addr) { return Dbg->functionName(Addr); })
                  : libc::NoReturnTargetIndex::NameResolver(),
              Dbg ? libc::NoReturnTargetIndex::DeclaredNoReturn(
                        [Dbg, &Img](llvm::StringRef Name) {
                          const std::optional<FunctionSym> Prototype =
                              Dbg->resolvePrototype(cNameOfStatedName(
                                  Name, Img.Format, Img.Arch));
                          return Prototype && Prototype->NoReturn;
                        })
                  : libc::NoReturnTargetIndex::DeclaredNoReturn()),
          StackProbes(Img), PointerCallees(collectPointerVariableCallees(Img)),
          X87FloatingImports(declaredX87Imports(Img, Dbg)), Roots(Img),
          Owners(Img) {}

    /// Import-address names (getImportAddressNames). HighIR lays a run's own
    /// function, debug and thunk names over this map and takes them off
    /// before returning. The fingerprint is the import storage and the counts
    /// admission of an import stub reads. Exception metadata is not in it, so
    /// reset the cache before changing the image.
    struct ImportNameMemo {
      const void *ImportData = nullptr;
      size_t ImportCount = 0;
      size_t StubCount = 0;
      size_t DataInCodeCount = 0;
      size_t ContinuationCount = 0;
      std::shared_ptr<std::map<va_t, std::string>> Names;

      bool matches(const BinaryImage &Img) const {
        return Names &&
               ImportData == static_cast<const void *>(Img.Imports.data()) &&
               ImportCount == Img.Imports.size() &&
               StubCount == Img.ImportStubIndices.size() &&
               DataInCodeCount == Img.DataInCode.size() &&
               ContinuationCount == Img.ContinuationCodeStarts.size();
      }
    };
    ImportNameMemo ImportNames;

    bool matches(const BinaryImage &Img, DebugContext *Dbg,
                 const SymbolInventory *CandidateInventory) const {
      return Image == &Img && Debug == Dbg &&
             Inventory.get() == CandidateInventory &&
             VerifiedEntries == Img.VerifiedFunctionEntries;
    }
  };

  static std::shared_ptr<ImageInputs>
  imageInputs(const BinaryImage &Img, DebugContext *Dbg,
              std::shared_ptr<const SymbolInventory> Inventory,
              const std::shared_ptr<Impl> &SymbolFacts,
              const std::shared_ptr<Impl> &SelectionFacts) {
    for (const auto &Kept : {SymbolFacts, SelectionFacts})
      if (Kept && Kept->Inputs->matches(Img, Dbg, Inventory.get()))
        return Kept->Inputs;
    return std::make_shared<ImageInputs>(Img, Dbg, std::move(Inventory));
  }

  const std::shared_ptr<ImageInputs> Inputs;
  /// The symbol inventory whose starts Entries are, if they are only those.
  const std::shared_ptr<const SymbolInventory> BuiltFrom;
  // Never changes, so the graph cache matches it by identity.
  const std::set<va_t> Entries;
  mutable pipeline_detail::CalleeFactsCache CalleeFacts;
  /// Whether warming found no room for another body.  Later pages lift each
  /// function once, for its callee facts.
  bool BodiesFull = false;
  InternalNoReturnIndex Callees;
  std::shared_ptr<X87CallGraphCache> Graphs;

  /// Entries are the inventory's starts when \p SymbolEntries is true.
  Impl(std::shared_ptr<ImageInputs> Inputs, std::set<va_t> Entries,
       bool RetainBodies, bool SymbolEntries)
      : Inputs(std::move(Inputs)),
        BuiltFrom(SymbolEntries ? this->Inputs->Inventory : nullptr),
        Entries(std::move(Entries)),
        Callees(*this->Inputs->Image, &this->Entries, &this->Inputs->Targets,
                &this->Inputs->Roots, &this->Inputs->Owners),
        Graphs(createX87CallGraphCache(
            &Callees, &this->Inputs->Targets, RetainBodies,
            {limits::kMaxPipelineCallGraphEntries,
             limits::kMaxPipelineCallGraphBytes,
             limits::kMaxPipelineCallBodyBytes},
            &this->Entries, this->Inputs->X87FloatingImports)) {}

  /// \p CandidateEntries is sorted and holds each entry once. BuiltFrom
  /// permits the identity shortcut only when these are the inventory's starts.
  bool matches(const BinaryImage &Img, DebugContext *Dbg,
               llvm::ArrayRef<va_t> CandidateEntries,
               const SymbolInventory *Inventory) const {
    // Object identity is only a guard: the owner must retire this state on
    // reload/mutation. These two inventories can grow during const analysis,
    // and affect CFG boundaries, ownership and proof work charges.  Entries
    // read from the same inventory are its starts; a grown symbol table reads
    // another.
    return Inputs->matches(Img, Dbg, Inventory) &&
           ((BuiltFrom.get() == Inventory) ||
            std::equal(Entries.begin(), Entries.end(), CandidateEntries.begin(),
                       CandidateEntries.end()));
  }
};

std::map<va_t, std::string> *
Pipeline::importAddressNames(const BinaryImage &Img,
                             PipelineAnalysisCache *Cache) {
  if (!Cache)
    return nullptr;
  // Facts and a selection can each hold image inputs. Prefer a memo that
  // already names this image's imports; otherwise fill the first inputs that
  // belong to it. HighIR removes whatever it adds before it returns.
  using InputsPtr = std::shared_ptr<PipelineAnalysisCache::Impl::ImageInputs>;
  InputsPtr Inputs;
  for (const std::shared_ptr<PipelineAnalysisCache::Impl> &Kept :
       {Cache->Facts, Cache->SelectionFacts}) {
    if (!Kept || !Kept->Inputs || Kept->Inputs->Image != &Img)
      continue;
    if (!Inputs)
      Inputs = Kept->Inputs;
    if (Kept->Inputs->ImportNames.matches(Img)) {
      Inputs = Kept->Inputs;
      break;
    }
  }
  if (!Inputs)
    return nullptr;
  PipelineAnalysisCache::Impl::ImageInputs::ImportNameMemo &Memo =
      Inputs->ImportNames;
  if (Memo.matches(Img))
    return Memo.Names.get();
  auto Built = std::make_shared<std::map<va_t, std::string>>(
      Img.getImportAddressNames());
  Memo.Names.reset();
  Memo.ImportData = static_cast<const void *>(Img.Imports.data());
  Memo.ImportCount = Img.Imports.size();
  Memo.StubCount = Img.ImportStubIndices.size();
  Memo.DataInCodeCount = Img.DataInCode.size();
  Memo.ContinuationCount = Img.ContinuationCodeStarts.size();
  Memo.Names = std::move(Built);
  return Memo.Names.get();
}

namespace {
/// The starts of the image's function symbols that are not boundary
/// guesses, sorted and each once: the entries every run knows.  An image
/// holds tens of thousands, which a sorted vector gathers and compares with
/// the kept facts' far sooner than a set is built and torn down per run.
std::vector<va_t> symbolFunctionStarts(const BinaryImage &Img,
                                       const std::set<va_t> &BoundaryGuesses) {
  std::vector<va_t> Starts;
  Starts.reserve(Img.Symbols.size());
  // A fragment that continues another function (Img.ContinuationCodeStarts)
  // begins none, though a symbol names it (`foo.cold`).
  for (const auto &Sym : Img.Symbols)
    if (Sym.IsFunc && !BoundaryGuesses.count(Sym.Addr) &&
        !Img.isFunctionContinuationAt(Sym.Addr))
      Starts.push_back(Sym.Addr);
  llvm::sort(Starts);
  Starts.erase(std::unique(Starts.begin(), Starts.end()), Starts.end());
  return Starts;
}

/// The address of every export of \p Img, sorted and each once.
std::vector<va_t> exportStarts(const BinaryImage &Img) {
  std::vector<va_t> Starts;
  Starts.reserve(Img.Exports.size());
  for (const Export &Exp : Img.Exports)
    Starts.push_back(Exp.Addr);
  llvm::sort(Starts);
  Starts.erase(std::unique(Starts.begin(), Starts.end()), Starts.end());
  return Starts;
}
} // namespace

PipelineAnalysisCache::SymbolInventory::SymbolInventory(const BinaryImage &Img)
    : BoundaryGuesses(Img.boundaryGuessFunctionStarts()),
      Starts(symbolFunctionStarts(Img, BoundaryGuesses)),
      ExportStarts(exportStarts(Img)), Image(&Img),
      SymbolData(Img.Symbols.data()), SymbolCount(Img.Symbols.size()),
      ExportData(Img.Exports.data()), ExportCount(Img.Exports.size()),
      RangeData(Img.KnownCodeRanges.data()),
      RangeCount(Img.KnownCodeRanges.size()),
      ContinuationCount(Img.ContinuationCodeStarts.size()) {}

bool PipelineAnalysisCache::SymbolInventory::readFrom(
    const BinaryImage &Img) const {
  return Image == &Img && SymbolData == Img.Symbols.data() &&
         SymbolCount == Img.Symbols.size() &&
         ExportData == Img.Exports.data() &&
         ExportCount == Img.Exports.size() &&
         RangeData == Img.KnownCodeRanges.data() &&
         RangeCount == Img.KnownCodeRanges.size() &&
         ContinuationCount == Img.ContinuationCodeStarts.size();
}

const std::map<va_t, std::string> &
PipelineAnalysisCache::SymbolInventory::functionNames(
    const BinaryImage &Img) const {
  if (!FunctionNames) {
    std::set<va_t> Named;
    for (const Export &Exp : Img.Exports)
      Named.insert(Exp.Addr);
    for (const Symbol &Sym : Img.Symbols)
      Named.insert(Sym.Addr);
    FunctionNames = functionNamesAt(Img, Named);
  }
  return *FunctionNames;
}

const std::vector<std::pair<va_t, va_t>> &
PipelineAnalysisCache::SymbolInventory::fh4ContinuationCandidates(
    const BinaryImage &Img) const {
  if (!FH4ContinuationCandidates)
    FH4ContinuationCandidates = pipeline_detail::fh4ContinuationCandidates(Img);
  return *FH4ContinuationCandidates;
}

const std::map<va_t, Intrinsic> &
PipelineAnalysisCache::SymbolInventory::runtimeRoutineCalls(
    const BinaryImage &Img) const {
  if (!RuntimeRoutineCalls)
    RuntimeRoutineCalls = runtimeRoutineCallsOf(Img, Img.Arch);
  return *RuntimeRoutineCalls;
}

std::shared_ptr<const PipelineAnalysisCache::SymbolInventory>
PipelineAnalysisCache::SymbolInventory::of(const BinaryImage &Img,
                                           PipelineAnalysisCache &Cache) {
  if (!Cache.Inventory || !Cache.Inventory->readFrom(Img))
    Cache.Inventory = std::make_shared<const SymbolInventory>(Img);
  return Cache.Inventory;
}

namespace {
/// Keep what a run reads of each of \p Functions as a callee
/// (computeCallRegisterEffects), with only the function's own formatted
/// calls giving fixed arguments; a run whose other bodies give one of its
/// calls more takes the body again.  The cache keeps what it has room for.
void warmCalleeFacts(
    const BinaryImage &Img, llvm::ArrayRef<va_t> Functions,
    const std::shared_ptr<X87CallGraphCache> &Graphs,
    const std::set<va_t> &Entries, const libc::NoReturnTargetIndex &Targets,
    const NoReturnCalleeProver &Callees,
    const detail::AbsoluteRelocationRootIndex &Roots,
    const ExecutableCodeOwnerIndex &Owners,
    const libc::StackProbeIndex &StackProbes,
    const std::map<va_t, PointerVariableCallee> &PointerCallees,
    pipeline_detail::CalleeFactsCache &Known) {
  std::vector<va_t> Missing;
  for (va_t Entry : Functions)
    if (Img.isFunctionEntryCandidateAt(Entry) && !Known.find(Entry))
      Missing.push_back(Entry);
  if (Missing.empty())
    return;
  std::vector<std::shared_ptr<const CallRegisterBody>> Cached(Missing.size());
  std::vector<LowFunc> Built(Missing.size());
  std::vector<char> Taken(Missing.size(), 0);
  std::vector<uint64_t> Weights(Missing.size(), 0);
  for (size_t I = 0; I < Missing.size(); ++I) {
    const va_t End = Img.getFunctionMetadataEnd(Missing[I], &Owners);
    if (End != InvalidVA && End > Missing[I])
      Weights[I] = std::min<uint64_t>(End - Missing[I], 1u << 20);
  }
  std::vector<std::exception_ptr> Failures(Missing.size());
  parallelForEachWeighted(
      Weights,
      [&](auto Claim, size_t N) {
        Decoder Dec;
        if (!Dec.init(Img))
          return;
        CFGBuilder Builder;
        Builder.setKnownFuncEntries(&Entries);
        Builder.setNoReturnTargetIndex(&Targets);
        Builder.setNoReturnCalleeProver(&Callees);
        Builder.setAbsoluteRelocationRootIndex(&Roots);
        Builder.setExecutableCodeOwnerIndex(&Owners);
        Builder.setX87CallGraphCache(Graphs);
        for (size_t I; (I = Claim()) < N;)
          try {
            Cached[I] = CFGBuilder::cachedCallRegisterBody(
                Graphs, Img, Missing[I], &Entries, &Targets, &Callees, &Roots,
                &Owners);
            if (!Cached[I])
              Built[I] = Builder.build(Img, Dec, Missing[I],
                                       Img.getFunctionNameAt(Missing[I]));
            Taken[I] = 1;
          } catch (...) {
            Failures[I] = std::current_exception();
          }
      },
      1, std::min(workerThreadCount(), 4u));
  for (const std::exception_ptr &Failure : Failures)
    if (Failure)
      std::rethrow_exception(Failure);
  // computeCallRegisterEffects reads calls through pointer variables so.
  std::map<va_t, va_t> ArgumentSources;
  for (const auto &[Slot, Callee] : PointerCallees)
    ArgumentSources.emplace(Slot, Callee.ImportName.empty() ? Callee.LocalEntry
                                                            : Slot);
  for (size_t I = 0; I < Missing.size(); ++I) {
    if (!Taken[I])
      continue;
    const CallRegisterBodyView Body = Cached[I]
                                          ? CallRegisterBodyView(*Cached[I])
                                          : CallRegisterBodyView(Built[I]);
    auto Callee = std::make_shared<pipeline_detail::CalleeFacts>();
    Callee->FormattedReads =
        pipeline_detail::formattedCallReads(Img, Body, nullptr);
    std::map<va_t, GPRReadWidths> FixedCallReads;
    pipeline_detail::mergeFixedCallReads(FixedCallReads,
                                         Callee->FormattedReads);
    Callee->Effect = summarizeRecoveredCallRegisters(
        Img, Body, Targets, FixedCallReads, ArgumentSources, StackProbes);
    Callee->Consulted =
        pipeline_detail::consultedCallReads(Body.Blocks, FixedCallReads);
    Callee->ForwardsToGuardDispatch = forwardsToGuardDispatch(Img, Body.Blocks);
    Known.insert(Missing[I], std::move(Callee), /*Evict=*/false);
  }
}
} // namespace

std::optional<AnalysisCacheWarmup>
Pipeline::warmAnalysisCache(const BinaryImage &Img, const PipelineOptions &Opts,
                            DebugContext *Dbg, llvm::ArrayRef<va_t> Functions) {
  if (!Opts.AnalysisCache)
    return std::nullopt;
  // The facts a selected-function run of a symbol start keeps (buildLowIR
  // below), with what every run reads of the image's names, runtime routines
  // and catch continuations, read here rather than in the first run.
  const auto symbolFacts = [&](PipelineAnalysisCache &Cache) {
    const auto Inventory =
        PipelineAnalysisCache::SymbolInventory::of(Img, Cache);
    const std::vector<va_t> &CandidateEntries = Inventory->Starts;
    (void)Inventory->functionNames(Img);
    (void)Inventory->runtimeRoutineCalls(Img);
    (void)Inventory->fh4ContinuationCandidates(Img);
    // Facts for another symbol inventory serve no later run.
    if (!Cache.Facts ||
        !Cache.Facts->matches(Img, Dbg, CandidateEntries, Inventory.get()))
      Cache.Facts = std::make_shared<PipelineAnalysisCache::Impl>(
          PipelineAnalysisCache::Impl::imageInputs(
              Img, Dbg, Inventory, Cache.Facts, Cache.SelectionFacts),
          std::set<va_t>(CandidateEntries.begin(), CandidateEntries.end()),
          /*RetainBodies=*/true, /*SymbolEntries=*/true);
    else
      Cache.Facts->Callees.trimMemoizedProofs(
          limits::kMaxRetainedNoReturnAnswers);
  };
  if (Img.Arch != Arch::X64) {
    // i386, AArch64 and ARM runs share those facts too (sharedAnalysisCache).
    if (Img.Arch == Arch::X86 || Img.Arch == Arch::AArch64 ||
        Img.Arch == Arch::ARM)
      symbolFacts(*Opts.AnalysisCache);
    // Where each call's setup gives its arguments, a restricted run reads
    // the signature bodies of the callees it leaves out. Those conversions
    // read the image's import counts and stack-slot identities, together with
    // the function-pointer variables' routines, independently of the current
    // selection's calls. Warming supplies those same declaration facts.
    const CallArgumentConvention *Convention =
        callArgumentConvention(Img.Arch, Img.abiFormat());
    if (!needsCallSignatureRecovery(Convention) || Img.IsRelocatable)
      return std::nullopt;
    PipelineAnalysisCache &Cache = *Opts.AnalysisCache;
    const std::map<va_t, PointerVariableCallee> PointerCallees =
        collectPointerVariableCallees(Img);
    PipelineResult Summaries;
    Summaries.CallArgumentSources = callArgumentSourcesOf(Img, PointerCallees);
    setOmittedImportArguments(Img, Dbg, PointerCallees, &Cache.ImportCounts,
                              Summaries);
    if (!Cache.CalleeSignatures)
      Cache.CalleeSignatures = std::make_shared<OmittedCalleeMemo>();
    Summaries.CalleeSignatureMemo = Cache.CalleeSignatures;
    return warmOmittedCallees(Img, Summaries, Functions);
  }
  // The entries a selected-function run of a symbol start knows (buildLowIR
  // below): its own entry adds nothing to them, so its run shares this state.
  PipelineAnalysisCache &Cache = *Opts.AnalysisCache;
  symbolFacts(Cache);
  auto &Facts = *Cache.Facts;
  Facts.Inputs->Targets.clearMemoizedNames();
  const auto &Inputs = *Facts.Inputs;
  // Bodies and graphs serve the call proofs of the functions runs lift
  // themselves; a callee is read from its facts.  Once no body fits, a page
  // only lifts each function for its callee facts, once.
  CFGBuilder::CallGraphWarmup Warmup;
  if (!Facts.BodiesFull) {
    Warmup = CFGBuilder::warmCallGraphs(
        Facts.Graphs, Img, Functions, &Facts.Entries, &Inputs.Targets,
        &Facts.Callees, &Inputs.Roots, &Inputs.Owners, workerThreadCount());
    Facts.BodiesFull = Warmup.Full;
  }
  warmCalleeFacts(Img, Functions, Facts.Graphs, Facts.Entries, Inputs.Targets,
                  Facts.Callees, Inputs.Roots, Inputs.Owners,
                  Inputs.StackProbes, Inputs.PointerCallees, Facts.CalleeFacts);
  return AnalysisCacheWarmup{Warmup.Retained, Facts.BodiesFull};
}

void Pipeline::buildLowIR(
    const BinaryImage &Img,
    const std::vector<std::pair<va_t, std::string>> &Candidates,
    const PipelineOptions &Opts, DebugContext *Dbg, PipelineResult &Result) {
  const size_t Total = Candidates.size();
  std::vector<LowFunc> AllLow(Total);
  // Detected candidates plus every confirmed function-symbol start. Selecting
  // bodies must not change their boundaries: one unwind range can cover
  // independently callable entries, including address-taken ones. CFGBuilder
  // also queries
  // RuntimeFunctionAddrs / KnownCodeRanges / ExceptionMetadata live so a
  // tail `jmp` to `_report_gsfailure` still becomes call+ret.
  // A padding-boundary guess is not a tail-call target: MSVC separates
  // hot/cold chunks of one function with the same int3 padding, and a jump
  // into such a chunk must keep its code in the jumping function.
  // The same symbol inventory owns both full-image and selected-function CFGs;
  // unselected bodies need not be lifted to preserve their entry boundaries.
  const bool Restricted =
      !Opts.OnlyFunctionEntries.empty() || Opts.MaxFunctions != 0;
  PipelineAnalysisCache LocalCache;
  PipelineAnalysisCache *Shared = sharedAnalysisCache(Img, Opts);
  PipelineAnalysisCache &Cache = Shared ? *Shared : LocalCache;
  const auto Inventory = PipelineAnalysisCache::SymbolInventory::of(Img, Cache);
  const std::set<va_t> &BoundaryGuesses = Inventory->BoundaryGuesses;
  const std::vector<va_t> &SymbolStarts = Inventory->Starts;
  std::vector<va_t> Added;
  const auto Add = [&](va_t Entry) {
    if (!Img.isFunctionEntryCandidateAt(Entry))
      return;
    if (!std::binary_search(SymbolStarts.begin(), SymbolStarts.end(), Entry))
      Added.push_back(Entry);
  };
  for (const auto &C : Candidates)
    if (!BoundaryGuesses.count(C.first))
      Add(C.first);
  if (Opts.OnlyFunctionEntries.empty()) {
    // A chained-unwind continuation is part of its parent function: a jump
    // to it must be followed, not modeled as a tail call to a missing one.
    for (const auto &[Start, End] : Img.KnownCodeRanges) {
      (void)End;
      if (Start != 0 && Img.hasKnownFunctionEntryAt(Start))
        Add(Start);
    }
  }
  const bool AddsEntries = !Added.empty();
  std::vector<va_t> MergedEntries;
  if (AddsEntries) {
    llvm::sort(Added);
    Added.erase(std::unique(Added.begin(), Added.end()), Added.end());
    MergedEntries.reserve(SymbolStarts.size() + Added.size());
    std::merge(SymbolStarts.begin(), SymbolStarts.end(), Added.begin(),
               Added.end(), std::back_inserter(MergedEntries));
  }
  const std::vector<va_t> &CandidateEntries =
      AddsEntries ? MergedEntries : SymbolStarts;
  // A selection that is not a symbol start adds its own entry and keys other
  // facts, kept beside the symbol inventory's rather than in their place.
  std::shared_ptr<PipelineAnalysisCache::Impl> &Kept =
      AddsEntries ? Cache.SelectionFacts : Cache.Facts;
  if (!Kept || !Kept->matches(Img, Dbg, CandidateEntries, Inventory.get()))
    Kept = std::make_shared<PipelineAnalysisCache::Impl>(
        PipelineAnalysisCache::Impl::imageInputs(
            Img, Dbg, Inventory, Cache.Facts, Cache.SelectionFacts),
        std::set<va_t>(CandidateEntries.begin(), CandidateEntries.end()),
        Restricted, /*SymbolEntries=*/!AddsEntries);
  else {
    // Earlier runs' no-return answers hold for the same facts, and replaying
    // a retained body asks them again; the screening data behind them does
    // not accumulate.  Lazy names are resolved again each run.
    Kept->Callees.trimMemoizedProofs(limits::kMaxRetainedNoReturnAnswers);
  }
  const auto &Facts = *Kept;
  Facts.Inputs->Targets.clearMemoizedNames();
  const auto &Inputs = *Facts.Inputs;
  const auto &FuncEntries = Facts.Entries;
  const auto &NoReturnTargets = Inputs.Targets;
  const auto &AbsoluteRelocationRoots = Inputs.Roots;
  const auto *CodeOwnerIndex = &Inputs.Owners;
  const auto &NoReturnCallees = Facts.Callees;
  const auto &X87Graphs = Facts.Graphs;
  // A small selected-function batch runs inline. Give its independent call
  // proofs the otherwise unused worker quota; parallel CFG batches and the
  // extra-callee frontier keep their existing outer parallelism.
  const unsigned X87ProofThreads = Total < limits::kMinParallelIRWorkItems
                                       ? std::min(workerThreadCount(), 4u)
                                       : 1;

  std::vector<uint64_t> Weight(
      Total, ::neverd::limits::pipeline::kPipelineLowIRMaxDecodeWeight);
  for (size_t I = 0; I + 1 < Total; ++I) {
    uint64_t Gap = Candidates[I + 1].first - Candidates[I].first;
    Weight[I] = std::min(
        Gap, ::neverd::limits::pipeline::kPipelineLowIRMaxDecodeWeight);
  }

  // Registration stack proofs repeat the same fresh query for one callee from
  // many callers. The image does not change during these builds, so their
  // recorded answers, replayed with their work, are the fresh ones.
  RegistrationStackPopMemo StackPops(Img);
  parallelForEachWeighted(Weight, [&](auto Claim, size_t N) {
    RegistrationStackPopMemoScope StackPopScope(StackPops);
    Decoder LocalDec;
    if (!LocalDec.init(Img))
      return;
    CFGBuilder LocalCFG;
    LocalCFG.setKnownFuncEntries(&FuncEntries);
    LocalCFG.setNoReturnTargetIndex(&NoReturnTargets);
    LocalCFG.setNoReturnCalleeProver(&NoReturnCallees);
    LocalCFG.setAbsoluteRelocationRootIndex(&AbsoluteRelocationRoots);
    LocalCFG.setExecutableCodeOwnerIndex(CodeOwnerIndex);
    LocalCFG.setX87CallGraphCache(X87Graphs);
    LocalCFG.setX87CallProofThreads(X87ProofThreads);
    for (size_t I; (I = Claim()) < N;) {
      // Only the initial default construction may reuse a complete body.
      // Module table arbitration and continuation closure below still rebuild
      // with their own protected roots and policies.
      auto Cached = CFGBuilder::cachedDefaultFunction(
          X87Graphs, Img, Candidates[I].first, Candidates[I].second,
          &FuncEntries, &NoReturnTargets, &NoReturnCallees,
          &AbsoluteRelocationRoots, CodeOwnerIndex);
      AllLow[I] = Cached ? std::move(*Cached)
                         : LocalCFG.build(Img, LocalDec, Candidates[I].first,
                                          Candidates[I].second);
      trimFuncStorage(AllLow[I]);
    }
  });

  std::vector<std::set<va_t>> EverPublishedJumpTableBranchHistory(Total);
  for (size_t I = 0; I < Total; ++I)
    EverPublishedJumpTableBranchHistory[I].insert(
        AllLow[I].EverPublishedJumpTableBranchAddresses.begin(),
        AllLow[I].EverPublishedJumpTableBranchAddresses.end());

  // Per-function recovery cannot see a later function that independently
  // reads or escapes one of its table's relocation slots.  Resolve that
  // module-wide veto after every provisional LowFunc exists, then rebuild only
  // affected owners with a frozen protected set.  Protection is monotone:
  // preserve/unknown evidence always dominates suppression, and rebuilding an
  // owner restores the corresponding relocation CFG roots as well as the LLVM
  // mirror field.  Iterate because a restored root can expose another
  // occurrence, but never let a later pass retract protection.
  std::set<va_t> ProtectedRelocationSlots;
  std::set<va_t> UnsafeJumpTableBranches;
  bool PreservePotentialJumpTableBranches = false;
  std::map<va_t, std::set<va_t>> ContinuationRootsByOwner;
  std::map<va_t, std::set<va_t>> BaselineOrdinaryRoots;
  for (const auto &Function : AllLow)
    BaselineOrdinaryRoots[Function.Entry] =
        Function.OrdinaryModuleAnalysisRoots;
  std::map<va_t, std::vector<LowCxxContinuationEntryEvidence>>
      StableCxxContinuationEntriesByOwner;
  std::map<va_t, std::vector<LowSEHLocalUnwindEvidence>>
      StableLocalUnwindsByOwner;
  std::vector<std::vector<LowCxxContinuationExitEvidence>>
      StableCxxContinuationExitsByFunction;
  std::vector<bool> StableCxxContinuationExitAnalysisCompleteByFunction;
  bool HasStableCxxContinuationExitSnapshot = false;
  auto clearStableCxxContinuationExitSnapshot = [&]() {
    StableCxxContinuationEntriesByOwner.clear();
    StableLocalUnwindsByOwner.clear();
    StableCxxContinuationExitsByFunction.clear();
    StableCxxContinuationExitAnalysisCompleteByFunction.clear();
    HasStableCxxContinuationExitSnapshot = false;
  };
  // Continuation discovery need not change any body. The same immutable
  // AllLow then has the same module arbitration, including budget refusals.
  // Keep one result within this run; every body replacement below retires it
  // before parallel builders can change its inputs. Image publication occurs
  // only after both fixed points finish.
  std::optional<ModuleJumpTableArbitration> CachedArbitration;
  auto arbitrationForCurrentBodies =
      [&]() -> const ModuleJumpTableArbitration & {
    if (!CachedArbitration)
      CachedArbitration = collectModuleJumpTableArbitration(
          Img, AllLow, Opts.JumpTableEvidenceBudgetForTesting);
    return *CachedArbitration;
  };
  auto rebuildFunctions = [&](const std::vector<bool> &Rebuild) {
    CachedArbitration.reset();
    parallelForEachWeighted(Weight, [&](auto Claim, size_t N) {
      RegistrationStackPopMemoScope StackPopScope(StackPops);
      Decoder LocalDec;
      if (!LocalDec.init(Img))
        return;
      CFGBuilder LocalCFG;
      LocalCFG.setKnownFuncEntries(&FuncEntries);
      LocalCFG.setNoReturnTargetIndex(&NoReturnTargets);
      LocalCFG.setNoReturnCalleeProver(&NoReturnCallees);
      LocalCFG.setAbsoluteRelocationRootIndex(&AbsoluteRelocationRoots);
      LocalCFG.setExecutableCodeOwnerIndex(CodeOwnerIndex);
      LocalCFG.setX87CallGraphCache(X87Graphs);
      LocalCFG.setX87CallProofThreads(X87ProofThreads);
      LocalCFG.setProtectedJumpTableRelocationSlots(&ProtectedRelocationSlots);
      LocalCFG.setUnsafeJumpTableBranches(&UnsafeJumpTableBranches);
      LocalCFG.setPreservePotentialJumpTableBranches(
          PreservePotentialJumpTableBranches);
      for (size_t I; (I = Claim()) < N;) {
        if (!Rebuild[I])
          continue;
        auto Continuations = ContinuationRootsByOwner.find(Candidates[I].first);
        LocalCFG.setCrossFunctionContinuationRoots(
            Continuations == ContinuationRootsByOwner.end()
                ? nullptr
                : &Continuations->second);
        LocalCFG.setPreviouslyPublishedJumpTableBranches(
            &EverPublishedJumpTableBranchHistory[I]);
        AllLow[I] = LocalCFG.build(Img, LocalDec, Candidates[I].first,
                                   Candidates[I].second);
        EverPublishedJumpTableBranchHistory[I].insert(
            AllLow[I].EverPublishedJumpTableBranchAddresses.begin(),
            AllLow[I].EverPublishedJumpTableBranchAddresses.end());
        trimFuncStorage(AllLow[I]);
      }
    });
  };

  const std::vector<bool> RebuildAll(Total, true);
  bool ArbitrationStable = false;
  bool PreserveAllRelocationSlots = false;
  const size_t MaxArbitrationPasses = limits::kMaxJumpTableEvidenceWork + 1;
  for (size_t Pass = 0; Pass < MaxArbitrationPasses; ++Pass) {
    ModuleJumpTableArbitration Discovered = arbitrationForCurrentBodies();
    if (!Discovered.AnalysisComplete) {
      ProtectedRelocationSlots.insert(
          Discovered.ProtectedRelocationSlots.begin(),
          Discovered.ProtectedRelocationSlots.end());
      UnsafeJumpTableBranches.insert(Discovered.UnsafeBranches.begin(),
                                     Discovered.UnsafeBranches.end());
      PreserveAllRelocationSlots = true;
      break;
    }
    std::vector<va_t> NewlyProtected;
    std::set_difference(Discovered.ProtectedRelocationSlots.begin(),
                        Discovered.ProtectedRelocationSlots.end(),
                        ProtectedRelocationSlots.begin(),
                        ProtectedRelocationSlots.end(),
                        std::back_inserter(NewlyProtected));
    std::vector<va_t> NewlyUnsafe;
    std::set_difference(
        Discovered.UnsafeBranches.begin(), Discovered.UnsafeBranches.end(),
        UnsafeJumpTableBranches.begin(), UnsafeJumpTableBranches.end(),
        std::back_inserter(NewlyUnsafe));
    if (NewlyProtected.empty() && NewlyUnsafe.empty()) {
      ArbitrationStable = true;
      break;
    }
    ProtectedRelocationSlots.insert(NewlyProtected.begin(),
                                    NewlyProtected.end());
    UnsafeJumpTableBranches.insert(NewlyUnsafe.begin(), NewlyUnsafe.end());

    std::vector<bool> Rebuild(Total, false);
    for (size_t I = 0; I < Total; ++I)
      for (const JumpTable &JT : AllLow[I].JumpTables)
        if (std::find(NewlyUnsafe.begin(), NewlyUnsafe.end(), JT.InsnAddr) !=
                NewlyUnsafe.end() ||
            std::any_of(
                NewlyProtected.begin(), NewlyProtected.end(),
                [&](va_t Slot) { return JT.suppressesRelocationSlot(Slot); })) {
          Rebuild[I] = true;
          break;
        }
    if (std::none_of(Rebuild.begin(), Rebuild.end(),
                     [](bool Value) { return Value; })) {
      ArbitrationStable = true;
      break;
    }

    rebuildFunctions(Rebuild);
  }
  if (!ArbitrationStable || PreserveAllRelocationSlots) {
    // A budget/iteration failure is uncertainty, not absence of consumers.
    // Disable relocation suppression module-wide and rebuild every candidate
    // once so the public CFG and LLVM mirror agree on the same fail-closed
    // decision, including tables discovered only through restored roots.
    ProtectedRelocationSlots.insert(Img.CodePtrRelocSlots.begin(),
                                    Img.CodePtrRelocSlots.end());
    PreservePotentialJumpTableBranches = true;
    bool FallbackStable = false;
    for (size_t Pass = 0; Pass < limits::kMaxMultiStageRetries; ++Pass) {
      // Restoring every relocation root may expose functions/tables that were
      // absent from the pre-fallback snapshot.  Rebuild first, then discover
      // every newly visible writable owner and repeat so its branch receives
      // the independent do-not-tailcall / trap identity as well.
      rebuildFunctions(RebuildAll);
      std::vector<va_t> NewlyUnsafe;
      for (const LowFunc &Func : AllLow)
        for (const JumpTable &JT : Func.JumpTables) {
          ModuleJumpTableOwner Owner;
          Owner.StorageRanges = JT.StorageRanges;
          Owner.PhysicalIdentityRanges =
              JT.ExactPhysicalStorageRange
                  ? std::vector<
                        JumpTableStorageRange>{*JT.ExactPhysicalStorageRange}
                  : JT.StorageRanges;
          if (storageMayBeWritable(Img, Owner) &&
              !UnsafeJumpTableBranches.count(JT.InsnAddr))
            NewlyUnsafe.push_back(JT.InsnAddr);
        }
      std::sort(NewlyUnsafe.begin(), NewlyUnsafe.end());
      NewlyUnsafe.erase(std::unique(NewlyUnsafe.begin(), NewlyUnsafe.end()),
                        NewlyUnsafe.end());
      if (NewlyUnsafe.empty()) {
        FallbackStable = true;
        break;
      }
      UnsafeJumpTableBranches.insert(NewlyUnsafe.begin(), NewlyUnsafe.end());
    }
    if (!FallbackStable) {
      // The set is monotone, so one final rebuild publishes every unsafe
      // identity discovered within the bounded loop.  Any unresolved marked
      // branch still remains an INDIR_BR and traps in LLVM/HighIR.
      rebuildFunctions(RebuildAll);
    }
  }

  auto clearContinuationRoots = [&]() {
    if (ContinuationRootsByOwner.empty())
      return;
    std::vector<bool> Rebuild(Total, false);
    for (size_t I = 0; I < Total; ++I)
      Rebuild[I] = ContinuationRootsByOwner.count(Candidates[I].first) != 0;
    ContinuationRootsByOwner.clear();
    rebuildFunctions(Rebuild);
  };
  auto closeEHContinuations = [&]() {
    clearStableCxxContinuationExitSnapshot();
    for (size_t Pass = 0; Pass < limits::kMaxMultiStageRetries; ++Pass) {
      EHContinuationRootDiscovery Discovered =
          collectWindowsEHContinuationRoots(
              Img, AllLow, FuncEntries,
              Opts.EHContinuationEvidenceBudgetForTesting,
              Opts.OnlyFunctionEntries.empty() && !Opts.MaxFunctions &&
                  Opts.FunctionEdits.empty(),
              Shared ? &Inventory->fh4ContinuationCandidates(Img) : nullptr);
      if (!Discovered.AnalysisComplete) {
        clearContinuationRoots();
        return false;
      }

      // Recompute the complete owner map on every round.  Adding a disconnected
      // root can expose an alternate return path or a conflicting address
      // materialization, so previously accepted evidence is not intrinsically
      // monotone.  Exact replacement makes the transaction retractable; an
      // oscillation is bounded below and ultimately restores the root-free
      // baseline rather than publishing a stale union.
      if (Discovered.RootsByOwner == ContinuationRootsByOwner) {
        if (Discovered.CxxContinuationExitsByFunction.size() != Total ||
            Discovered.CxxContinuationExitAnalysisCompleteByFunction.size() !=
                Total) {
          clearContinuationRoots();
          return false;
        }
        StableCxxContinuationExitsByFunction =
            std::move(Discovered.CxxContinuationExitsByFunction);
        StableCxxContinuationEntriesByOwner =
            std::move(Discovered.EntriesByOwner);
        StableLocalUnwindsByOwner = std::move(Discovered.LocalUnwindsByOwner);
        StableCxxContinuationExitAnalysisCompleteByFunction =
            std::move(Discovered.CxxContinuationExitAnalysisCompleteByFunction);
        HasStableCxxContinuationExitSnapshot = true;
        return true;
      }

      std::vector<bool> Rebuild(Total, false);
      for (size_t I = 0; I < Total; ++I) {
        const va_t Owner = Candidates[I].first;
        const auto Old = ContinuationRootsByOwner.find(Owner);
        const auto Next = Discovered.RootsByOwner.find(Owner);
        if (Old == ContinuationRootsByOwner.end() &&
            Next == Discovered.RootsByOwner.end())
          continue;
        if (Old == ContinuationRootsByOwner.end() ||
            Next == Discovered.RootsByOwner.end() ||
            Old->second != Next->second)
          Rebuild[I] = true;
      }
      ContinuationRootsByOwner = std::move(Discovered.RootsByOwner);
      rebuildFunctions(Rebuild);
    }

    // A bounded least-fixed-point that did not converge is not a partial
    // certificate.  Withdraw every inferred root and restore the owners from
    // their ordinary entry/EH metadata before later stages inspect them.
    clearContinuationRoots();
    clearStableCxxContinuationExitSnapshot();
    return false;
  };

  // Continuation blocks can expose a table consumer that was absent from the
  // entry-only baseline.  If that changes module arbitration, withdraw all EH
  // roots, rebuild the baseline under the new monotone protection set, and
  // compute the least fixed point again.  No provisional code reference is
  // published to BinaryImage during these rounds.
  bool ContinuationAndArbitrationStable = false;
  for (size_t Pass = 0; Pass < limits::kMaxMultiStageRetries; ++Pass) {
    if (!closeEHContinuations()) {
      ContinuationAndArbitrationStable = true;
      break;
    }
    ModuleJumpTableArbitration Discovered = arbitrationForCurrentBodies();
    if (!Discovered.AnalysisComplete) {
      clearStableCxxContinuationExitSnapshot();
      clearContinuationRoots();
      ProtectedRelocationSlots.insert(Img.CodePtrRelocSlots.begin(),
                                      Img.CodePtrRelocSlots.end());
      UnsafeJumpTableBranches.insert(Discovered.UnsafeBranches.begin(),
                                     Discovered.UnsafeBranches.end());
      PreservePotentialJumpTableBranches = true;
      rebuildFunctions(RebuildAll);
      ContinuationAndArbitrationStable = true;
      break;
    }

    std::vector<va_t> NewlyProtected;
    std::set_difference(Discovered.ProtectedRelocationSlots.begin(),
                        Discovered.ProtectedRelocationSlots.end(),
                        ProtectedRelocationSlots.begin(),
                        ProtectedRelocationSlots.end(),
                        std::back_inserter(NewlyProtected));
    std::vector<va_t> NewlyUnsafe;
    std::set_difference(
        Discovered.UnsafeBranches.begin(), Discovered.UnsafeBranches.end(),
        UnsafeJumpTableBranches.begin(), UnsafeJumpTableBranches.end(),
        std::back_inserter(NewlyUnsafe));
    if (NewlyProtected.empty() && NewlyUnsafe.empty()) {
      ContinuationAndArbitrationStable = true;
      break;
    }

    ProtectedRelocationSlots.insert(NewlyProtected.begin(),
                                    NewlyProtected.end());
    UnsafeJumpTableBranches.insert(NewlyUnsafe.begin(), NewlyUnsafe.end());
    clearStableCxxContinuationExitSnapshot();
    ContinuationRootsByOwner.clear();
    rebuildFunctions(RebuildAll);
  }
  if (!ContinuationAndArbitrationStable) {
    clearStableCxxContinuationExitSnapshot();
    clearContinuationRoots();
  }

  // Publish one immutable occurrence batch only after both the continuation
  // closure and module table arbitration agree on this exact AllLow snapshot.
  // Until this point every discovery result is pipeline-local and retractable;
  // any failure above leaves every function visibly unanalysed.
  for (LowFunc &Function : AllLow) {
    Function.CxxContinuationExits.clear();
    Function.CxxContinuationEntries.clear();
    Function.SEHLocalUnwindContinuations.clear();
    Function.CxxContinuationExitAnalysisComplete = false;
  }
  if (ContinuationAndArbitrationStable &&
      HasStableCxxContinuationExitSnapshot &&
      StableCxxContinuationExitsByFunction.size() == Total &&
      StableCxxContinuationExitAnalysisCompleteByFunction.size() == Total) {
    for (size_t I = 0; I < Total; ++I) {
      if (auto Found = StableLocalUnwindsByOwner.find(AllLow[I].Entry);
          Found != StableLocalUnwindsByOwner.end())
        AllLow[I].SEHLocalUnwindContinuations = std::move(Found->second);
      if (auto Found =
              StableCxxContinuationEntriesByOwner.find(AllLow[I].Entry);
          Found != StableCxxContinuationEntriesByOwner.end())
        for (const auto &Entry : Found->second)
          if (!BaselineOrdinaryRoots[AllLow[I].Entry].count(Entry.Target))
            AllLow[I].CxxContinuationEntries.push_back(Entry);
      if (!StableCxxContinuationExitAnalysisCompleteByFunction[I])
        continue;
      AllLow[I].CxxContinuationExits =
          std::move(StableCxxContinuationExitsByFunction[I]);
      AllLow[I].CxxContinuationExitAnalysisComplete = true;
    }
  }

  // Only the final reachable functions publish relocation-free PC-relative
  // code identities.  Keeping publication after both fixed points prevents a
  // provisional producer or withdrawn continuation from becoming stale
  // image-global evidence.
  for (const LowFunc &Function : AllLow)
    for (va_t Ref : Function.CodeRefTargets)
      Img.CodeRefTargets.insert(Ref);
  // CodeRefTargets affects later address propagation and x87 tag entry seeds,
  // but not the raw machine graphs retained by X87Graphs: their builders skip
  // floating-stack normalization. Keep those facts for extra-callee analysis.
  // Ordinary builders normalize stack and tags against the newly published
  // roots. Complete-function reuse requires the independent no-stack witness;
  // neither a tag-dependent normalized CFG nor a caller's effect answer is
  // shared.

  size_t FuncCount = 0;
  for (size_t I = 0; I < Total; ++I) {
    auto &Low = AllLow[I];
    auto AuditIt =
        std::find_if(Result.FunctionAudits.begin(), Result.FunctionAudits.end(),
                     [&](const PipelineFunctionAudit &Audit) {
                       return Audit.Entry == Candidates[I].first;
                     });
    if (AuditIt != Result.FunctionAudits.end()) {
      AuditIt->DecodedInstructions = Low.DecodedInstructionCount;
      AuditIt->LiftedInstructions = Low.LiftedInstructionCount;
      AuditIt->DecodeFailures = Low.DecodeFailureAddresses;
      AuditIt->UnsupportedInstructions = Low.UnsupportedInstructionAddresses;
      AuditIt->UnprovenReturns = Low.UnprovenReturnAddresses;
      AuditIt->TruncatedPaths = Low.TruncatedPathAddresses;
      if (Opts.DeferMedIR) {
        std::set<va_t> Addresses;
        for (const auto &Block : Low.Blocks)
          for (const auto &Instruction : Block.InstructionBoundaries)
            Addresses.insert(Instruction.Address);
        Addresses.insert(Low.UnsupportedInstructionAddresses.begin(),
                         Low.UnsupportedInstructionAddresses.end());
        AuditIt->NativeInstructionAddresses.assign(Addresses.begin(),
                                                   Addresses.end());
      }
    }

    if (Opts.MaxFunctions > 0 && FuncCount >= Opts.MaxFunctions) {
      if (AuditIt != Result.FunctionAudits.end())
        AuditIt->Disposition = PipelineFunctionDisposition::SkippedLimit;
      continue;
    }
    if (!hasRealOps(Low)) {
      if (AuditIt != Result.FunctionAudits.end())
        AuditIt->Disposition =
            Low.hasCompleteInstructionLift()
                ? PipelineFunctionDisposition::RejectedLowIR
                : PipelineFunctionDisposition::RejectedIncomplete;
      continue;
    }
    // Only an incomplete *lift* disqualifies a function.  A path that left the
    // mapped image is recorded in the audit but is not a defect in what was
    // recovered, and rejecting it would drop a function whose every
    // instruction lifted cleanly.
    if (!Low.hasCompleteInstructionLift()) {
      if (AuditIt != Result.FunctionAudits.end())
        AuditIt->Disposition = PipelineFunctionDisposition::RejectedIncomplete;
      continue;
    }

    if (Dbg && Dbg->hasInfo())
      annotateDebugInfo(Low, *Dbg);
    if (Low.OriginalSize == 0)
      Low.OriginalSize = Low.computedSize();
    // A `bsr` the code reaches only with a nonzero source writes its result:
    // the kept destination is no read of it.
    foldGuardedZeroSelects(Low);

    Result.LowFuncs.push_back(std::move(Low));
    if (AuditIt != Result.FunctionAudits.end())
      AuditIt->HasLowIR = true;
    ++FuncCount;
  }

  computeCallRegisterEffects(
      Img, FuncEntries, NoReturnTargets, Inputs.StackProbes, NoReturnCallees,
      AbsoluteRelocationRoots, CodeOwnerIndex, X87Graphs, Inputs.PointerCallees,
      Facts.CalleeFacts, Dbg, Shared ? &Inventory->functionNames(Img) : nullptr,
      Shared ? &Inventory->ExportStarts : nullptr,
      Opts.AnalysisCache ? &Opts.AnalysisCache->ImportCounts : nullptr, Result);
}

} // namespace neverd
