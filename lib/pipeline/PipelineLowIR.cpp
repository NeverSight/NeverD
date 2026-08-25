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

#include "PipelineLowIRDetail.h"
#include "PipelineTrimStorage.h"

#include "neverd/Limits.h"
#include "neverd/debug/DebugContext.h"
#include "neverd/decode/Decoder.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/PointerRelocation.h"
#include "neverd/pipeline/Pipeline.h"
#include "neverd/support/BinaryEncoding.h"
#include "neverd/support/Parallel.h"

#include "llvm/ADT/APInt.h"
#include "llvm/Support/MathExtras.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <tuple>
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
  auto FSym = Dbg.resolveFunction(Func.Entry);
  if (!FSym)
    return;
  Func.DebugName = FSym->Name;
  Func.SourceFile = FSym->DeclLoc.File;
  Func.SourceLine = FSym->DeclLoc.Line;
  if (FSym->Size > 0)
    Func.OriginalSize = FSym->Size;
}

struct ModuleJumpTableOwner {
  size_t FuncIndex = 0;
  va_t BranchAddr = InvalidVA;
  va_t StorageIdentityVA = InvalidVA;
  std::vector<JumpTableStorageRange> StorageRanges;
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

bool storageEnvelopeContainsOrOnePast(const BinaryImage &Img,
                                      const ModuleJumpTableOwner &Owner,
                                      const ModuleAddressAnchor &Anchor) {
  return std::any_of(Owner.StorageRanges.begin(), Owner.StorageRanges.end(),
                     [&](const JumpTableStorageRange &Range) {
                       if (!addressOwnerMatches(Img, Owner, Range, Anchor))
                         return false;
                       const std::optional<va_t> End = Range.storageEnd();
                       return End && Anchor.Address >= Range.BaseAddr &&
                              Anchor.Address <= *End;
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
           std::any_of(Owner.StorageRanges.begin(), Owner.StorageRanges.end(),
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
  for (const JumpTableStorageRange &Range : Owner.StorageRanges) {
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
  for (const JumpTableStorageRange &Range : Owner.StorageRanges) {
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
           ExactValuesWidened == Other.ExactValuesWidened;
  }
};
using ModuleAddressState = std::map<LowValueKey, ModuleAddressFacts>;

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
};

enum class ModuleAddressEdgeTraversal : uint8_t {
  OrdinaryAndExceptional,
  OrdinaryOnly,
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
  if (Facts.FrameOffsets.empty())
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
    LocalUnwindContinuationEvidence *LocalUnwind = nullptr) {
  const TargetRegInfo &TRI = getTargetRegInfo(Img.Arch);
  const std::vector<TargetRegisterRange> PreservedRanges =
      TRI.callPreservedRanges(Img.Format);
  constexpr size_t kMaxModuleAddressScanOps =
      size_t{limits::kMaxJumpTableEvidenceWork} *
      size_t{limits::kMaxMultiStageRetries};
  size_t ScanOps = 0;
  auto failCollect = [](const char *, size_t = 0, va_t = InvalidVA) {
    return false;
  };
  size_t FixpointWorkRemaining = std::min(
      TestBudget.value_or(kMaxModuleAddressScanOps), kMaxModuleAddressScanOps);
  auto consumeFixpointWork = [&]() {
    if (FixpointWorkRemaining == 0)
      return false;
    --FixpointWorkRemaining;
    return true;
  };
  for (const LowFunc &Func : Funcs)
    for (const LowBlock &Block : Func.Blocks) {
      if (Block.Ops.size() > kMaxModuleAddressScanOps - ScanOps)
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
  }

  std::map<va_t, std::vector<const ModuleJumpTableOwner *>> OwnersByContainer;
  for (const ModuleJumpTableOwner &Owner : Owners) {
    if (Owner.StorageIdentityVA == InvalidVA)
      return failCollect("invalid-owner-identity");
    std::set<va_t> OwnerContainers;
    for (const JumpTableStorageRange &Range : Owner.StorageRanges) {
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

    bool MatchedTable = false;
    if (auto It = OwnersByContainer.find(Exact.OwnerVA);
        It != OwnersByContainer.end()) {
      for (const ModuleJumpTableOwner *Owner : It->second) {
        if (Owner->StorageIdentityVA == InvalidVA)
          continue;
        const bool Contains = std::any_of(
            Owner->StorageRanges.begin(), Owner->StorageRanges.end(),
            [&](const JumpTableStorageRange &Range) {
              if (storageRangeOwnerVA(Img, Range) != Exact.OwnerVA)
                return false;
              const std::optional<va_t> End = Range.storageEnd();
              return End && Exact.Address >= Range.BaseAddr &&
                     Exact.Address <= *End;
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

  // Keep finite exact alternatives for slot-precise LOAD/STORE decisions.
  // Larger joins widen to the durable owner-summary lattice above.
  constexpr size_t kMaxExactAddressAlternatives = 16;
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
          Into.ExactValues.size() > kMaxExactAddressAlternatives) {
        Into.ExactValues.clear();
        Into.ExactValuesWidened = true;
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
        const auto [WideOffset, WideSize] = TRI.findWideReg(Offset, Size);
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
            for (const auto &[Key, ExistingFacts] : State) {
              const auto &[Space, Offset, Size] = Key;
              if (Space != VnodeSpace::REG)
                continue;
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
          for (auto &[Key, Stored] : State)
            if (std::get<0>(Key) == VnodeSpace::STACK)
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
          for (const auto &[Key, Stored] : State) {
            const auto &[Space, RawOffset, StoredSize] = Key;
            if (Space == VnodeSpace::STACK && Stored.FrameCellEscaped &&
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
          for (const auto &[Key, Stored] : State) {
            const auto &[Space, RawOffset, StoredSize] = Key;
            if (Space != VnodeSpace::STACK ||
                !frameIntervalsOverlap(LoadOffset, AccessSize,
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
          if (Img.Arch == Arch::X64 && Img.Format == BinaryFormat::COFF) {
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
        if (Op.Opcode == NdOp::LOAD && Memory.Complete) {
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
                   Op.Opcode == NdOp::INTRINSIC || Op.Opcode == NdOp::RETURN) {
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
              for (const auto &[Key, Facts] : State) {
                const auto &[Space, Offset, Size] = Key;
                (void)Size;
                if (Space != VnodeSpace::REG)
                  continue;
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
                    else
                      LocalUnwind->TargetsByFunction[FuncIndex].insert(Target);
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
            for (const auto &[Key, Facts] : State) {
              if (std::get<0>(Key) != VnodeSpace::REG ||
                  TRI.regToArgIdx(std::get<1>(Key),
                                  Img.Format == BinaryFormat::COFF) < 0)
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
              if (Img.Arch == Arch::X64 && Img.Format == BinaryFormat::COFF) {
                int64_t AfterShadow = 0;
                if (llvm::AddOverflow(Cursor, int64_t{32}, AfterShadow))
                  return false;
                Cursor = AfterShadow;
              }
              constexpr size_t kMaxOutgoingStackCells = 64;
              for (size_t CellIndex = 0; CellIndex < kMaxOutgoingStackCells;
                   ++CellIndex) {
                const ModuleAddressFacts *Cell = nullptr;
                uint16_t CellSize = 0;
                for (const auto &[Key, Facts] : State) {
                  const auto &[Space, RawOffset, StoredSize] = Key;
                  if (Space != VnodeSpace::STACK || StoredSize == 0 ||
                      !Facts.FrameCellPresent ||
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
              for (const auto &[Key, Facts] : State)
                if (std::get<0>(Key) == VnodeSpace::STACK &&
                    std::get<2>(Key) != 0 && Facts.FrameCellPresent &&
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
          case NdOp::INTRINSIC:
          case NdOp::RETURN:
            // Reading an address from memory does not preserve the address of
            // the memory field in the result; observable effects were recorded
            // above and do not forward operand provenance into their outputs.
            break;
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
                for (const auto &[Key, ExistingFacts] : State) {
                  const auto &[Space, Offset, Size] = Key;
                  if (Space != VnodeSpace::REG)
                    continue;
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
      ModuleAddressState Output;
      if (!transfer(Func.Blocks[BlockIndex], Input, nullptr, Output))
        return failCollect("transfer-fixpoint", FuncIndex,
                           Func.Blocks[BlockIndex].StartAddr);
      const bool FirstVisit = !Processed[BlockIndex];
      Processed[BlockIndex] = true;
      if (!FirstVisit && Input == InStates[BlockIndex] &&
          Output == OutStates[BlockIndex])
        continue;
      InStates[BlockIndex] = std::move(Input);
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
      if (!transfer(Func.Blocks[I], InStates[I], &Uses, Ignored))
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

struct EHContinuationRootDiscovery {
  std::map<va_t, std::set<va_t>> RootsByOwner;
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
EHContinuationRootDiscovery collectWindowsEHContinuationRoots(
    const BinaryImage &Img, const std::vector<LowFunc> &Funcs,
    const std::set<va_t> &FunctionEntries, std::optional<size_t> TestBudget) {
  EHContinuationRootDiscovery Result;
  Result.CxxContinuationExitsByFunction.assign(Funcs.size(), {});
  Result.CxxContinuationExitAnalysisCompleteByFunction.assign(Funcs.size(),
                                                              false);
  if (Img.Format != BinaryFormat::COFF)
    return Result;

  for (const ExceptionFunction &Declaring : Img.ExceptionMetadata.Functions) {
    if (Declaring.Kind != RuntimeFunctionKind::Primary ||
        Declaring.ParseStatus != ExceptionParseStatus::Complete ||
        !Declaring.Cxx ||
        Declaring.Cxx->NativeEncoding != CxxExceptionInfo::Encoding::FH4 ||
        Declaring.Cxx->NativeFuncInfoVA == 0)
      continue;
    for (const CxxTryBlock &Try : Declaring.Cxx->TryBlocks)
      for (const CxxCatchHandler &Handler : Try.Handlers)
        for (va_t Target : Handler.ContinuationVAs) {
          Target = normalizeCodeAddress(Target, Img.Arch, Img.Mode);
          if (Declaring.CodeRange.contains(Target))
            continue;
          const ExceptionFunction *Owner = nullptr;
          for (const ExceptionFunction &Candidate :
               Img.ExceptionMetadata.Functions) {
            if (Candidate.Kind != RuntimeFunctionKind::Primary ||
                Candidate.ParseStatus != ExceptionParseStatus::Complete ||
                !Candidate.Cxx ||
                Candidate.Cxx->NativeEncoding !=
                    CxxExceptionInfo::Encoding::FH4 ||
                Candidate.Cxx->NativeFuncInfoVA !=
                    Declaring.Cxx->NativeFuncInfoVA ||
                Target <= Candidate.CodeRange.Begin ||
                Target >= Candidate.CodeRange.End)
              continue;
            if (Owner) {
              Owner = nullptr;
              break;
            }
            Owner = &Candidate;
          }
          if (Owner && FunctionEntries.count(Owner->CodeRange.Begin) != 0 &&
              FunctionEntries.count(Target) == 0 &&
              Img.hasExecutableCodeOwnerAt(Target))
            Result.RootsByOwner[Owner->CodeRange.Begin].insert(Target);
        }
  }

  // FH3 separated funclets return continuation addresses only in the x64
  // table ABI.  Other Windows targets either use registration records or
  // carry explicit continuation metadata handled above.
  if (Img.Arch != Arch::X64)
    return Result;

  auto exactFH3Record = [&](va_t Entry) -> const ExceptionFunction * {
    const ExceptionFunction *Match = nullptr;
    for (const ExceptionFunction &F : Img.ExceptionMetadata.Functions) {
      if (F.Kind != RuntimeFunctionKind::Primary ||
          F.CodeRange.Begin != Entry ||
          F.ParseStatus != ExceptionParseStatus::Complete || !F.Cxx ||
          F.Cxx->NativeEncoding != CxxExceptionInfo::Encoding::FH3)
        continue;
      if (Match)
        return nullptr;
      Match = &F;
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
    for (va_t Target : Func.CodeRefTargets)
      CandidateTargets.insert(normalizeCodeAddress(Target, Img.Arch, Img.Mode));
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
  // An exceptional edge enters with personality-defined register/frame state;
  // it is not a value-preserving predecessor of an ordinary machine RETURN.
  // Catch funclets are independently rooted at their real entry, so following
  // only ordinary edges proves the returned continuation without letting an
  // unwind self-cycle grow a fictitious stack epoch until the budget expires.
  if (!collectLowAddressUses(Img, Funcs, NoTableOwners, IgnoredUses, Budget,
                             TestBudget, &CandidateTargets, &Returned,
                             ModuleAddressEdgeTraversal::OrdinaryOnly,
                             &LocalUnwind)) {
    Result.AnalysisComplete = false;
    return Result;
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
    for (const ExceptionFunction &Candidate : Img.ExceptionMetadata.Functions) {
      if (Candidate.ParseStatus != ExceptionParseStatus::Complete ||
          Candidate.CodeRange.Begin != Source.Entry)
        continue;
      if (SourceRuntimeFunction) {
        SourceRuntimeFunction = nullptr;
        SourceRuntimeFunctionAmbiguous = true;
        break;
      }
      SourceRuntimeFunction = &Candidate;
    }
    if (SourceRuntimeFunctionAmbiguous || !SourceRuntimeFunction)
      continue;

    std::set<va_t> PublishedBySource;
    for (va_t Target : Source.CodeRefTargets)
      PublishedBySource.insert(
          normalizeCodeAddress(Target, Img.Arch, Img.Mode));
    for (va_t Target : LocalUnwind.TargetsByFunction[FuncIndex]) {
      Target = normalizeCodeAddress(Target, Img.Arch, Img.Mode);
      if (PublishedBySource.count(Target) == 0 ||
          FunctionEntries.count(Target) != 0 ||
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
      if (!OwnerAmbiguous && Owner &&
          FunctionEntries.count(Owner->CodeRange.Begin) != 0 &&
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
      if (PublishedBySource.count(Target) == 0 ||
          FunctionEntries.count(Target) != 0 ||
          !Img.hasExecutableCodeOwnerAt(Target))
        continue;
      const ExceptionFunction *Owner = uniqueFH3Owner(Target);
      if (!Owner || FunctionEntries.count(Owner->CodeRange.Begin) == 0)
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
      if (DeclaresSource)
        Result.RootsByOwner[Owner->CodeRange.Begin].insert(Target);
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
  auto scanPair = [&]() {
    if (PairScanRemaining == 0)
      return false;
    --PairScanRemaining;
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

  std::map<va_t, std::set<va_t>> TableObjectAliases;
  size_t TableAliasWorkRemaining = limits::kMaxJumpTableEvidenceWork;
  for (const ModuleJumpTableOwner &Owner : Owners)
    TableObjectAliases[Owner.StorageIdentityVA].insert(Owner.StorageIdentityVA);
  for (size_t Left = 0; Left < Owners.size(); ++Left)
    for (size_t Right = Left + 1; Right < Owners.size(); ++Right) {
      if (!scanPair())
        return abandonAnalysis();
      if (!pipeline_detail::tableObjectSummaryMayAlias(
              Img, Owners[Left].StorageIdentityVA, Owners[Left].StorageRanges,
              Owners[Right].StorageIdentityVA, Owners[Right].StorageRanges,
              &TableAliasWorkRemaining))
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
        if (!std::any_of(JT.StorageRanges.begin(), JT.StorageRanges.end(),
                         [&](const JumpTableStorageRange &Range) {
                           return Range.ownsStorageAddress(Slot);
                         }))
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
  if (!collectLowAddressUses(Img, Funcs, Owners, Uses, Budget, TestBudget))
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
        for (const JumpTableStorageRange &Range : Candidate.StorageRanges) {
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
    return std::any_of(Owner.StorageRanges.begin(), Owner.StorageRanges.end(),
                       [&](const JumpTableStorageRange &Range) {
                         return storageRangeOwnerVA(Img, Range) ==
                                TargetOwnerVA;
                       });
  };
  auto protectRelocationUse = [&](va_t FieldVA,
                                  const RelocatedAddressField &Field) {
    struct ExactSource {
      size_t FuncIndex = 0;
      const RelocatedInstructionAddressOccurrence *Occurrence = nullptr;
    };
    std::vector<ExactSource> ExactSources;
    for (size_t FuncIndex = 0; FuncIndex < Funcs.size(); ++FuncIndex)
      for (const RelocatedInstructionAddressOccurrence &Occurrence :
           Funcs[FuncIndex].RelocatedInstructionAddressOccurrences)
        if (Occurrence.FieldVA == FieldVA)
          ExactSources.push_back({FuncIndex, &Occurrence});

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
        TargetsOwner = storageEnvelopeContainsOrOnePast(Img, Owner, Target);
      } else {
        for (const ExactSource &Source : ExactSources) {
          const RelocatedInstructionAddressOccurrence &Occurrence =
              *Source.Occurrence;
          const ModuleAddressAnchor Target{
              Occurrence.TargetVA, Occurrence.TargetOwnerVA,
              Occurrence.TargetOwnerVA == InvalidVA
                  ? ModuleAddressOwnerKind::Unknown
                  : ModuleAddressOwnerKind::Container};
          if (storageEnvelopeContainsOrOnePast(Img, Owner, Target)) {
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
        const size_t FuncIndex = Source.FuncIndex;
        const va_t InsnAddr = Source.Occurrence->InstructionAddr;
        if (FuncIndex == Owner.FuncIndex)
          continue; // candidate-local audit already handled it
        if (isUniquelyAuthenticatedTableLoad(Funcs[FuncIndex], InsnAddr, Owner))
          continue;
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
    if (!Budget.consume())
      return abandonAnalysis();
    const uint8_t *P = Img.readVA(PointerSlot, Img.getPointerSize());
    if (!P)
      return abandonAnalysis();
    const va_t Target = static_cast<va_t>(readPtr(P, Img.is64Bit()));
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
                                      : ModuleAddressOwnerKind::Container})) {
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
          TouchesObject |=
              storageEnvelopeContainsOrOnePast(Img, Owner, Address);
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
      if (Use.UseKind == ModuleAddressUse::Kind::WriteThrough) {
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
  return {std::move(Result.UnsafeBranches), Result.AnalysisComplete};
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
    const std::set<va_t> &FunctionEntries, std::optional<size_t> TestBudget) {
  EHContinuationRootDiscovery Result = collectWindowsEHContinuationRoots(
      Img, Funcs, FunctionEntries, TestBudget);
  return {std::move(Result.RootsByOwner), Result.AnalysisComplete};
}

} // namespace pipeline_detail

//===----------------------------------------------------------------------===//
// buildLowIR — Phase 1
//===----------------------------------------------------------------------===//

void Pipeline::buildLowIR(
    const BinaryImage &Img,
    const std::vector<std::pair<va_t, std::string>> &Candidates,
    const PipelineOptions &Opts, DebugContext *Dbg, PipelineResult &Result) {
  const size_t Total = Candidates.size();
  std::vector<LowFunc> AllLow(Total);

  // The set of all detected function entries lets each CFG builder recognise an
  // unconditional `jmp` to *another* function as a tail call (call + ret)
  // rather than following it and fusing the callee into this function's CFG.
  std::set<va_t> FuncEntries;
  for (const auto &C : Candidates)
    FuncEntries.insert(C.first);

  // Decode cost tracks a function's instruction count, which is unknown before
  // the recursive-descent build runs.  Candidates are address-sorted, so the
  // byte gap to the next entry is a cheap upper-bound proxy for a function's
  // size; scheduling the largest gaps first keeps one giant function from
  // being claimed last and serializing the tail.  The gap is clamped so an
  // outsized cross-segment gap (data between the last function and the segment
  // end) does not distort the ordering — it is only a scheduling hint and never
  // affects the decoded result.
  constexpr uint64_t kMaxDecodeWeight = 1ull << 20;
  std::vector<uint64_t> Weight(Total, kMaxDecodeWeight);
  for (size_t I = 0; I + 1 < Total; ++I) {
    uint64_t Gap = Candidates[I + 1].first - Candidates[I].first;
    Weight[I] = std::min(Gap, kMaxDecodeWeight);
  }

  parallelForEachWeighted(Weight, [&](auto Claim, size_t N) {
    Decoder LocalDec;
    if (!LocalDec.init(Img.Arch, Img.Mode))
      return;
    CFGBuilder LocalCFG;
    LocalCFG.setKnownFuncEntries(&FuncEntries);
    for (size_t I; (I = Claim()) < N;) {
      AllLow[I] = LocalCFG.build(Img, LocalDec, Candidates[I].first,
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
  std::vector<std::vector<LowCxxContinuationExitEvidence>>
      StableCxxContinuationExitsByFunction;
  std::vector<bool> StableCxxContinuationExitAnalysisCompleteByFunction;
  bool HasStableCxxContinuationExitSnapshot = false;
  auto clearStableCxxContinuationExitSnapshot = [&]() {
    StableCxxContinuationExitsByFunction.clear();
    StableCxxContinuationExitAnalysisCompleteByFunction.clear();
    HasStableCxxContinuationExitSnapshot = false;
  };
  auto rebuildFunctions = [&](const std::vector<bool> &Rebuild) {
    parallelForEachWeighted(Weight, [&](auto Claim, size_t N) {
      Decoder LocalDec;
      if (!LocalDec.init(Img.Arch, Img.Mode))
        return;
      CFGBuilder LocalCFG;
      LocalCFG.setKnownFuncEntries(&FuncEntries);
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
    ModuleJumpTableArbitration Discovered = collectModuleJumpTableArbitration(
        Img, AllLow, Opts.JumpTableEvidenceBudgetForTesting);
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
              Opts.EHContinuationEvidenceBudgetForTesting);
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
    ModuleJumpTableArbitration Discovered = collectModuleJumpTableArbitration(
        Img, AllLow, Opts.JumpTableEvidenceBudgetForTesting);
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
    Function.CxxContinuationExitAnalysisComplete = false;
  }
  if (ContinuationAndArbitrationStable &&
      HasStableCxxContinuationExitSnapshot &&
      StableCxxContinuationExitsByFunction.size() == Total &&
      StableCxxContinuationExitAnalysisCompleteByFunction.size() == Total) {
    for (size_t I = 0; I < Total; ++I) {
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
      AuditIt->TruncatedPaths = Low.TruncatedPathAddresses;
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

    Result.LowFuncs.push_back(std::move(Low));
    if (AuditIt != Result.FunctionAudits.end())
      AuditIt->HasLowIR = true;
    ++FuncCount;
  }
}

} // namespace neverd
