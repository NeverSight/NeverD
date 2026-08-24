//===- JumpTableResolverBounds.cpp - Entry-count bounds -------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Entry-count bounds for jump tables that carry no comparison range guard:
/// relocation-run counting (absolute code-pointer and PC-relative-to-code
/// runs, capped at the next table anchor), index-mask bounds, modulo bounds
/// recovered from a magic-division remainder, and normalization pull-back of a
/// raw bound.  Comparison-guard bounds live in JumpTableResolverGuards.cpp.
///
/// Part of the CFGBuilder jump-table resolver; see JumpTableResolver.cpp for
/// top-level strategy dispatch and JumpTableResolverDetail.h for shared
/// backward-slicing helpers.
///
//===----------------------------------------------------------------------===//

#include "JumpTableResolverDetail.h"

#include "neverd/Limits.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/low/CFGBuilder.h"

#include "llvm/ADT/APInt.h"
#include "llvm/Support/MathExtras.h"
#include <algorithm>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <tuple>
#include <vector>

namespace neverd {

static std::optional<uint64_t> integerWidthMask(uint16_t Size) {
  if (Size == 0 || Size > sizeof(uint64_t))
    return std::nullopt;
  if (Size == sizeof(uint64_t))
    return ~uint64_t{0};
  return (uint64_t{1} << (Size * 8)) - 1;
}

static constexpr bool moduloDomainFitsSignedWidth(uint32_t Bound,
                                                  uint16_t Size) {
  if (Bound == 0 || Size == 0 || Size > sizeof(uint64_t))
    return false;
  const unsigned Bits = Size * 8;
  return uint64_t(Bound - 1) < (uint64_t{1} << (Bits - 1));
}

// A 140-way domain is non-negative in a 16-bit signed view, but values
// 128..139 become negative when a one-byte producer is sign-extended.  Keep
// this width-sensitive boundary compile-time visible: conditional signed
// remainder closure must check producer widths, not only the consumer width.
static_assert(!moduloDomainFitsSignedWidth(140, 1));
static_assert(moduloDomainFitsSignedWidth(140, 2));

class ModuloEvidenceBudget {
public:
  constexpr explicit ModuloEvidenceBudget(size_t Limit,
                                          size_t *LocalAggregate = nullptr,
                                          size_t *OuterAggregate = nullptr)
      : Remaining(Limit), LocalAggregate(LocalAggregate),
        OuterAggregate(OuterAggregate) {}

  constexpr bool consume(size_t Amount = 1) {
    if (Amount > Remaining ||
        (LocalAggregate && Amount > *LocalAggregate) ||
        (OuterAggregate && Amount > *OuterAggregate)) {
      Exhausted = true;
      // A failed shared charge is candidate-wide evidence exhaustion.  Zero
      // the shared balance so the resolver's central transactional guard can
      // distinguish it from an ordinary local modulo-prefix truncation.
      if (LocalAggregate && Amount > *LocalAggregate)
        *LocalAggregate = 0;
      if (OuterAggregate && Amount > *OuterAggregate)
        *OuterAggregate = 0;
      return false;
    }
    Remaining -= Amount;
    if (LocalAggregate)
      *LocalAggregate -= Amount;
    if (OuterAggregate)
      *OuterAggregate -= Amount;
    return true;
  }

  constexpr size_t remaining() const {
    size_t Available = Remaining;
    if (LocalAggregate)
      Available = std::min(Available, *LocalAggregate);
    if (OuterAggregate)
      Available = std::min(Available, *OuterAggregate);
    return Available;
  }
  constexpr bool canConsume(size_t Amount = 1) {
    if (Amount <= Remaining &&
        (!LocalAggregate || Amount <= *LocalAggregate) &&
        (!OuterAggregate || Amount <= *OuterAggregate))
      return true;
    Exhausted = true;
    if (LocalAggregate && Amount > *LocalAggregate)
      *LocalAggregate = 0;
    if (OuterAggregate && Amount > *OuterAggregate)
      *OuterAggregate = 0;
    return false;
  }
  constexpr bool exhausted() const { return Exhausted; }

private:
  size_t Remaining;
  size_t *LocalAggregate = nullptr;
  size_t *OuterAggregate = nullptr;
  bool Exhausted = false;
};

static_assert([] {
  ModuloEvidenceBudget Budget(2);
  return Budget.consume() && Budget.consume() && !Budget.consume() &&
         Budget.remaining() == 0 && Budget.exhausted();
}());

static size_t orderedEvidenceLookupWork(size_t Count) {
  size_t Work = 1;
  for (size_t N = Count; N > 1; N = (N + 1) / 2)
    ++Work;
  return Work;
}

// Keep attacker-controlled work in independent accounts so an intentionally
// expensive proposal scan cannot consume the memory needed for final all-path
// replay.  Phase caps overlap, but every charge also debits one shared public
// 4096-unit invocation ceiling; this lets a large real proposal borrow unused
// direct/replay capacity without increasing total work.  The
// exact structural relation in JumpTableResolverSlice also has a per-batch
// symbolization ceiling, while its CFG snapshot, value reconstruction and
// every expression visit debit the same candidate-wide aggregate account as
// target/address roles and mask-domain recovery.
static constexpr size_t kModuloProposalWork = 2560;
static constexpr size_t kModuloStructuralRetentionWork = 1536;
static constexpr size_t kModuloDirectWork = 512;
static constexpr size_t kModuloReplayWork = 1024;
static_assert(kModuloProposalWork <= limits::kMaxJumpTableEvidenceWork);
static_assert(kModuloStructuralRetentionWork <=
              limits::kMaxJumpTableEvidenceWork);
static_assert(kModuloDirectWork <= limits::kMaxJumpTableEvidenceWork);
static_assert(kModuloReplayWork <= limits::kMaxJumpTableEvidenceWork);

// Proposal discovery is an under-approximation: retain at most this
// instruction-prefix worth of ops, prepaid before each append.  Candidate
// scanning charges the same ops a second time; the remaining quarter covers
// compact instruction-span records and bounded reaching-definition/evaluator
// visits.  Structural authentication retains its own independent account, and
// final all-path replay keeps the full 1024-unit reserve below.
static constexpr size_t kMaxRetainedModuloProposalOps = 512;
static_assert(kMaxRetainedModuloProposalOps * 2 <= kModuloProposalWork);

static constexpr std::optional<size_t>
moduloDomainEvidenceWork(uint32_t Bound, size_t ProducerCount,
                         size_t OccurrenceCount,
                         bool RequireProducerReachability) {
  if (Bound == 0 || OccurrenceCount == 0)
    return std::nullopt;
  const size_t Max = std::numeric_limits<size_t>::max();
  if (size_t(Bound) > (Max - ProducerCount) / 2)
    return std::nullopt;
  const size_t DomainAlternatives = ProducerCount + size_t(Bound) * 2;
  // Prepay both the public query representation and the resolver's mirrored
  // allowed-value/result storage.  This prevents a small query-count budget
  // from authorizing an unbounded alternatives allocation.
  if (DomainAlternatives > (Max - 3) / 2)
    return std::nullopt;
  // Each public query retains its query record plus result and per-query
  // completion bits.  Alternatives are mirrored once inside the resolver.
  size_t PerOccurrence = size_t{3} + DomainAlternatives * 2;
  if (RequireProducerReachability) {
    if (ProducerCount > (Max - 3) / 2)
      return std::nullopt;
    const size_t DependencyWork = size_t{3} + ProducerCount * 2;
    if (PerOccurrence > Max - DependencyWork)
      return std::nullopt;
    PerOccurrence += DependencyWork;
  }
  if (OccurrenceCount > Max / PerOccurrence)
    return std::nullopt;
  return OccurrenceCount * PerOccurrence;
}

constexpr std::optional<size_t> Modulo140ReplayWork =
    moduloDomainEvidenceWork(140, 1, 1, true);
static_assert(Modulo140ReplayWork && *Modulo140ReplayWork == 570);
static_assert(Modulo140ReplayWork &&
              *Modulo140ReplayWork + 140 + 2 <= kModuloReplayWork);
constexpr std::optional<size_t> Modulo2048BootstrapWork =
    moduloDomainEvidenceWork(2048, 1, 1, false);
static_assert(Modulo2048BootstrapWork &&
              *Modulo2048BootstrapWork >
                  limits::kMaxJumpTableEvidenceWork);

std::optional<uint64_t> effectiveIntegerAndMask(uint64_t EncodedMask,
                                                uint16_t MaskSize,
                                                uint16_t DynamicSize,
                                                uint16_t OutputSize) {
  const std::optional<uint64_t> ConstantWidthMask = integerWidthMask(MaskSize);
  const std::optional<uint64_t> OutputWidthMask = integerWidthMask(OutputSize);
  const std::optional<uint64_t> DynamicWidthMask =
      integerWidthMask(DynamicSize);
  if (!ConstantWidthMask || !OutputWidthMask || !DynamicWidthMask)
    return std::nullopt;
  uint64_t Mask = (EncodedMask & *ConstantWidthMask) & *OutputWidthMask;
  if (DynamicSize < OutputSize)
    Mask &= *DynamicWidthMask;
  return Mask;
}

/// Count the run of consecutive absolute code-pointer relocation slots starting
/// at TableAddr, stepping by EntrySize.  The loader records such slots in
/// Img.CodePtrRelocSlots.  ScanComplete distinguishes a run ending at the
/// global limit from one that continues past it.
uint32_t countCodePtrRelocRun(const BinaryImage &Img, va_t TableAddr,
                              uint64_t EntryStride, bool *ScanComplete) {
  if (ScanComplete)
    *ScanComplete = true;
  if (EntryStride == 0 || Img.CodePtrRelocSlots.empty())
    return 0;
  uint32_t Run = 0;
  va_t VA = TableAddr;
  bool HasNextVA = true;
  while (Run < limits::kMaxJumpTableEntries) {
    if (!Img.CodePtrRelocSlots.count(VA))
      break;
    ++Run;
    if (EntryStride > InvalidVA - VA) {
      HasNextVA = false;
      break;
    }
    VA += EntryStride;
  }
  if (ScanComplete && Run == limits::kMaxJumpTableEntries && HasNextVA &&
      Img.CodePtrRelocSlots.count(VA))
    *ScanComplete = false;
  return Run;
}

static bool authenticatedSourceAnchorSpanMatches(
    const AuthenticatedSourceAnchorExemption &Exemption, va_t BaseAddr,
    uint64_t EntryStride, uint32_t Run) {
  if (EntryStride == 0 || Run == 0 || Exemption.CandidateBaseVA != BaseAddr ||
      Exemption.TargetVA == InvalidVA || Exemption.TargetOwnerVA == InvalidVA ||
      Exemption.EffectiveSourceVA == InvalidVA ||
      Exemption.SourceByteCount == 0 ||
      // An occurrence naming B that is adjusted to read A is not B's storage
      // consumer.  Suppressing B in that case would merge adjacent A/B runs.
      Exemption.TargetVA != Exemption.EffectiveSourceVA ||
      Exemption.EffectiveSourceVA < BaseAddr ||
      uint64_t(Run) > (InvalidVA - BaseAddr) / EntryStride ||
      Exemption.SourceByteCount > InvalidVA - Exemption.EffectiveSourceVA)
    return false;

  const va_t RunEnd = BaseAddr + uint64_t(Run) * EntryStride;
  const va_t SourceEnd =
      Exemption.EffectiveSourceVA + Exemption.SourceByteCount;
  return Exemption.EffectiveSourceVA < RunEnd && SourceEnd <= RunEnd;
}

bool authenticatedSourceAnchorExemptionMatches(
    const AuthenticatedSourceAnchorExemption &Exemption, va_t BaseAddr,
    uint64_t EntryStride, uint32_t Run, va_t FieldVA,
    const RelocatedAddressField &Field) {
  return Exemption.FieldVA == FieldVA && FieldVA != InvalidVA &&
         !Field.PCRelativeFromInstructionEnd &&
         Field.TargetVA == Exemption.TargetVA &&
         Field.TargetOwnerVA == Exemption.TargetOwnerVA &&
         authenticatedSourceAnchorSpanMatches(Exemption, BaseAddr, EntryStride,
                                              Run);
}

bool authenticatedSourceAnchorExemptionMatches(
    const AuthenticatedSourceAnchorExemption &Exemption, va_t BaseAddr,
    uint64_t EntryStride, uint32_t Run,
    const RelocatedInstructionAddressOccurrence &Occurrence) {
  if (Exemption.FieldVA != InvalidVA ||
      Occurrence.Authority != RelocatedInstructionAddressProofKind::
                                  AArch64RelocationFreeDataDereference ||
      Occurrence.FieldVA != InvalidVA ||
      Occurrence.TargetVA != Exemption.TargetVA ||
      Occurrence.TargetOwnerVA != Exemption.TargetOwnerVA ||
      Occurrence.Width == 0 ||
      Occurrence.Provenance != ConstantAddressProvenance::DataAddress ||
      !Occurrence.DefinesOutput || Occurrence.OutputMayDepend ||
      Occurrence.InstructionAddr == InvalidVA || Occurrence.OpSeq < 0 ||
      Occurrence.OutputOpcode == NdOp::NOP ||
      (!Occurrence.OutputWitness.isReg() &&
       !Occurrence.OutputWitness.isTemp()) ||
      Occurrence.OutputWitness.Size != Occurrence.Width ||
      Occurrence.SeedInstructionAddr == InvalidVA || Occurrence.SeedOpSeq < 0 ||
      Occurrence.SeedOpcode != NdOp::COPY ||
      !Occurrence.SeedInputWitness.isConst() ||
      Occurrence.SeedInputWitness.Provenance !=
          ConstantAddressProvenance::AddressFragment ||
      (!Occurrence.SeedOutputWitness.isReg() &&
       !Occurrence.SeedOutputWitness.isTemp()) ||
      Occurrence.ArithmeticProof.empty() ||
      Occurrence.DereferenceInstructionAddr == InvalidVA ||
      Occurrence.DereferenceOpSeq < 0 ||
      (Occurrence.DereferenceOpcode != NdOp::LOAD &&
       Occurrence.DereferenceOpcode != NdOp::STORE) ||
      Occurrence.DereferenceAccessSize == 0)
    return false;
  const RelocatedInstructionAddressArithmeticStep &Final =
      Occurrence.ArithmeticProof.back();
  if (Final.InstructionAddr != Occurrence.InstructionAddr ||
      Final.OpSeq != Occurrence.OpSeq ||
      Final.Opcode != Occurrence.OutputOpcode ||
      Final.OutputWitness != Occurrence.OutputWitness ||
      Occurrence.DereferenceAddressWitness.Size != Occurrence.Width)
    return false;
  return authenticatedSourceAnchorSpanMatches(Exemption, BaseAddr, EntryStride,
                                              Run);
}

uint32_t boundCodePtrRunByNextAnchor(
    const BinaryImage &Img, va_t BaseAddr, uint64_t EntryStride, uint32_t Run,
    const std::set<va_t> &DecodedAnchors,
    const std::map<va_t, AuthenticatedSourceAnchorExemption>
        &AuthenticatedSources) {
  if (EntryStride == 0 || Run == 0)
    return Run;

  std::set<va_t> Anchors = Img.RelCodeTableAnchors;
  Anchors.insert(DecodedAnchors.begin(), DecodedAnchors.end());
  for (const auto &[FieldVA, Field] : Img.DataAddressRelocOperands) {
    const auto Authenticated = AuthenticatedSources.find(FieldVA);
    const bool IsAuthenticated =
        Authenticated != AuthenticatedSources.end() &&
        authenticatedSourceAnchorExemptionMatches(
            Authenticated->second, BaseAddr, EntryStride, Run, FieldVA, Field);
    if (!IsAuthenticated && !Field.PCRelativeFromInstructionEnd &&
        Img.hasExecutableCodeOwnerAt(FieldVA) && Field.TargetVA != InvalidVA)
      Anchors.insert(Field.TargetVA);
  }

  for (auto It = Anchors.upper_bound(BaseAddr); It != Anchors.end(); ++It) {
    const va_t NextAnchor = *It;
    if (!Img.CodePtrRelocSlots.count(NextAnchor))
      continue;
    const uint64_t Delta = NextAnchor - BaseAddr;
    if (Delta % EntryStride != 0)
      return 0;
    const uint64_t Slots = Delta / EntryStride;
    if (Slots > std::numeric_limits<uint32_t>::max())
      return 0;
    return std::min(Run, static_cast<uint32_t>(Slots));
  }
  return Run;
}

bool codePtrRelocRunHasExactBoundary(const BinaryImage &Img, va_t BaseAddr,
                                     uint64_t EntryStride, uint32_t Run,
                                     const std::set<va_t> &DecodedAnchors) {
  if (EntryStride == 0 || Run == 0 ||
      uint64_t(Run) > (InvalidVA - BaseAddr) / EntryStride)
    return false;
  const va_t End = BaseAddr + uint64_t(Run) * EntryStride;
  if (const std::optional<va_t> OwnerEnd = Img.mappedObjectOwnerEnd(BaseAddr);
      OwnerEnd && *OwnerEnd == End)
    return true;
  if (Img.RelCodeTableAnchors.count(End))
    return true;
  if (DecodedAnchors.count(End))
    return true;
  for (const auto &[FieldVA, Field] : Img.DataAddressRelocOperands)
    if (!Field.PCRelativeFromInstructionEnd &&
        Img.hasExecutableCodeOwnerAt(FieldVA) && Field.TargetVA == End)
      return true;
  return false;
}

/// Count the run of consecutive PC-relative-to-code relocation slots starting
/// at the table base — the entries of a PIC `switch` jump table.
/// ScanComplete distinguishes a run ending at the global limit from one that
/// continues past it.
uint32_t countRelCodeRelocRun(const BinaryImage &Img, va_t TableAddr,
                              uint64_t EntryStride, bool *ScanComplete) {
  if (ScanComplete)
    *ScanComplete = true;
  if (EntryStride == 0 || Img.RelCodeRelocSlots.empty())
    return 0;
  uint32_t Run = 0;
  va_t VA = TableAddr;
  bool HasNextVA = true;
  while (Run < limits::kMaxJumpTableEntries) {
    if (!Img.RelCodeRelocSlots.count(VA))
      break;
    ++Run;
    if (EntryStride > InvalidVA - VA) {
      HasNextVA = false;
      break;
    }
    VA += EntryStride;
  }
  if (ScanComplete && Run == limits::kMaxJumpTableEntries && HasNextVA &&
      Img.RelCodeRelocSlots.count(VA))
    *ScanComplete = false;
  return Run;
}

/// Truncate a RelCodeReloc entry run so it stops at the next PIC jump-table
/// base anchor.  Two unguarded PIC tables laid out back-to-back in rodata share
/// one continuous RelCodeReloc entry run, so a raw run count from the first
/// table's base over-reads past its end into the second — recovering bogus
/// successor edges (each an entry of the second table decoded relative to the
/// first base) that misroute the first dispatch (§15.2 adjacent-unguarded-pic-
/// table).  The next table's own base anchor — a rodata VA a `lea`/`adrp+add`/
/// GOTOFF materializes AND itself a RelCodeReloc entry position (so a plain
/// string/constant `lea` never truncates a real table) — is this table's exact
/// end.  Returns the run capped to the distance to that anchor.
uint32_t boundRelRunByNextAnchor(const BinaryImage &Img, va_t BaseAddr,
                                 uint64_t EntryStride, uint32_t Run,
                                 const std::set<va_t> &DecodedAnchors) {
  if (EntryStride == 0 || Run == 0)
    return Run;
  va_t NextAnchor = 0;
  std::set<va_t> Anchors = Img.RelCodeTableAnchors;
  Anchors.insert(DecodedAnchors.begin(), DecodedAnchors.end());
  for (auto It = Anchors.upper_bound(BaseAddr); It != Anchors.end(); ++It)
    if (Img.RelCodeRelocSlots.count(*It)) {
      NextAnchor = *It;
      break;
    }
  if (NextAnchor <= BaseAddr)
    return Run;
  const uint64_t Delta = NextAnchor - BaseAddr;
  if (Delta % EntryStride != 0)
    return 0;
  const uint64_t Slots = Delta / EntryStride;
  if (Slots > std::numeric_limits<uint32_t>::max())
    return 0;
  uint32_t Cap = static_cast<uint32_t>(Slots);
  return std::min(Run, Cap);
}

//===----------------------------------------------------------------------===//
// evalLinearMultiple — read the integer multiplier of a single base value
//===----------------------------------------------------------------------===//

/// Decompose `V` into `base * Coef`, where Coef is built from the shift /
/// small-constant-multiply / add / subtract terms of one conceptual base.
/// Recovers the modulus N out of the `quotient * N` back-multiply clang emits
/// for `x % N` (rendered as shift/add/sub trees, e.g. q*7=(q<<3)-q,
/// q*9=(q<<3)+q, q*10=(q<<3)+(q<<1), or a direct `q*N` where N may live in a
/// register).  Any op that is not a multiplier-tree node (the magic
/// `(x*recip)>>s` quotient, a load, a param) terminates a branch as the base
/// with coefficient 1; the caller gates on a multiply being present and on the
/// recovered N matching the table's real entry count so this leniency cannot
/// misread an ordinary table.
class BoundedLinearMultipleEvaluator {
public:
  BoundedLinearMultipleEvaluator(const std::vector<LowOp> &Ops,
                                 ModuloEvidenceBudget &Budget)
      : Ops(Ops), Budget(Budget) {}

  bool evaluate(int FromIdx, NdVar Value, int64_t &Coefficient) {
    return evaluateImpl(FromIdx, Value, 0, Coefficient);
  }

  std::optional<int64_t> constant(int FromIdx, NdVar Value) {
    return constantImpl(FromIdx, Value, 0);
  }

  bool exhausted() const { return Exhausted; }

private:
  using Key = std::tuple<int, uint8_t, uint64_t, uint16_t>;
  enum class MemoState : uint8_t { Active, Failed, Done };
  struct MemoEntry {
    MemoState State = MemoState::Active;
    int64_t Value = 0;
  };

  static Key key(int FromIdx, const NdVar &Value) {
    return {FromIdx, static_cast<uint8_t>(Value.Space), Value.Offset,
            Value.Size};
  }

  bool consume(size_t Amount = 1) {
    if (Budget.consume(Amount))
      return true;
    Exhausted = true;
    return false;
  }

  std::optional<int> reachingDef(int FromIdx, const NdVar &Value) {
    for (int I = FromIdx; I >= 0; --I) {
      if (!consume())
        return std::nullopt;
      const NdVar &Output = Ops[I].Output;
      if (Output.Space == Value.Space && Output.Offset == Value.Offset)
        return I;
    }
    return std::nullopt;
  }

  static bool checkedCoefficient(llvm::APInt Value, int64_t &Out) {
    if (!Value.isSignedIntN(64))
      return false;
    Out = Value.trunc(64).getSExtValue();
    return true;
  }

  static llvm::APInt wideSigned(int64_t Value) {
    return llvm::APInt(64, static_cast<uint64_t>(Value),
                       /*isSigned=*/false, /*implicitTrunc=*/true)
        .sext(128);
  }

  std::optional<int64_t> constantImpl(int FromIdx, NdVar Value, int Depth) {
    if (!consume() || Depth > limits::kMaxQuasiCopyDepth)
      return std::nullopt;
    if (Value.isConst()) {
      if (Value.Offset >
          static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
        return std::nullopt;
      return static_cast<int64_t>(Value.Offset);
    }
    if (!Value.isReg() && !Value.isTemp())
      return std::nullopt;

    const Key K = key(FromIdx, Value);
    auto [It, Inserted] = ConstantMemo.try_emplace(K);
    if (!Inserted) {
      if (It->second.State == MemoState::Done)
        return It->second.Value;
      return std::nullopt;
    }
    const std::optional<int> Definition = reachingDef(FromIdx, Value);
    if (!Definition) {
      It->second.State = MemoState::Failed;
      return std::nullopt;
    }
    const LowOp &Op = Ops[*Definition];
    // x86 materializes even an immediate shift count as `imm & (width-1)`.
    // Fold only a fully proved constant AND so dynamic masked shifts remain
    // outside the exact multiplier recipe.
    if (Op.Opcode == NdOp::INT_AND) {
      if (Op.NumInputs < 2 || Op.Output.Size == 0 ||
          Op.Output.Size > sizeof(uint64_t)) {
        It->second.State = MemoState::Failed;
        return std::nullopt;
      }
      const std::optional<int64_t> Left =
          constantImpl(*Definition - 1, Op.Inputs[0], Depth + 1);
      const std::optional<int64_t> Right =
          constantImpl(*Definition - 1, Op.Inputs[1], Depth + 1);
      const std::optional<uint64_t> Mask = integerWidthMask(Op.Output.Size);
      if (!Left || !Right || !Mask) {
        It->second.State = MemoState::Failed;
        return std::nullopt;
      }
      const uint64_t Result =
          (static_cast<uint64_t>(*Left) & static_cast<uint64_t>(*Right)) &
          *Mask;
      if (Result >
          static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
        It->second.State = MemoState::Failed;
        return std::nullopt;
      }
      It->second.State = MemoState::Done;
      It->second.Value = static_cast<int64_t>(Result);
      return It->second.Value;
    }
    if ((Op.Opcode != NdOp::COPY && Op.Opcode != NdOp::INT_ZEXT &&
         Op.Opcode != NdOp::INT_SEXT) ||
        Op.NumInputs < 1) {
      It->second.State = MemoState::Failed;
      return std::nullopt;
    }
    const std::optional<int64_t> Result =
        constantImpl(*Definition - 1, Op.Inputs[0], Depth + 1);
    if (!Result) {
      It->second.State = MemoState::Failed;
      return std::nullopt;
    }
    It->second.State = MemoState::Done;
    It->second.Value = *Result;
    return Result;
  }

  bool evaluateImpl(int FromIdx, NdVar Value, int Depth, int64_t &Coefficient) {
    if (!consume() || Depth > limits::kMaxModuloDecompDepth ||
        (!Value.isReg() && !Value.isTemp()))
      return false;

    const Key K = key(FromIdx, Value);
    auto [It, Inserted] = LinearMemo.try_emplace(K);
    if (!Inserted) {
      if (It->second.State != MemoState::Done)
        return false;
      Coefficient = It->second.Value;
      return true;
    }
    auto fail = [&]() {
      It->second.State = MemoState::Failed;
      return false;
    };
    auto finish = [&](int64_t Result) {
      It->second.State = MemoState::Done;
      It->second.Value = Result;
      Coefficient = Result;
      return true;
    };

    const std::optional<int> Definition = reachingDef(FromIdx, Value);
    if (!Definition) {
      if (Exhausted)
        return fail();
      return finish(1); // No definition in the slice: the base itself.
    }
    const int D = *Definition;
    const LowOp &Op = Ops[D];
    auto isVar = [](const NdVar &X) { return X.isReg() || X.isTemp(); };
    switch (Op.Opcode) {
    case NdOp::COPY:
    case NdOp::INT_ZEXT:
    case NdOp::INT_SEXT:
      if (Op.NumInputs >= 1 && isVar(Op.Inputs[0])) {
        int64_t Inner = 0;
        return evaluateImpl(D - 1, Op.Inputs[0], Depth + 1, Inner)
                   ? finish(Inner)
                   : fail();
      }
      return finish(1); // COPY of a constant: a materialised base.
    case NdOp::SUBBYTES:
      if (Op.NumInputs >= 2 && Op.Inputs[1].isConst() &&
          Op.Inputs[1].Offset == 0 && isVar(Op.Inputs[0])) {
        int64_t Inner = 0;
        return evaluateImpl(D - 1, Op.Inputs[0], Depth + 1, Inner)
                   ? finish(Inner)
                   : fail();
      }
      return fail();
    case NdOp::INT_LEFT:
      if (Op.NumInputs >= 2 && isVar(Op.Inputs[0])) {
        const std::optional<int64_t> Shift =
            constantImpl(D - 1, Op.Inputs[1], 0);
        if (!Shift || *Shift < 0 || *Shift >= 32)
          return fail();
        int64_t Inner = 0;
        int64_t Result = 0;
        if (!evaluateImpl(D - 1, Op.Inputs[0], Depth + 1, Inner) ||
            !checkedCoefficient(wideSigned(Inner).shl(*Shift), Result))
          return fail();
        return finish(Result);
      }
      return fail();
    case NdOp::INT_MULT: {
      for (int ConstantSide = 0;
           ConstantSide < Op.NumInputs && ConstantSide < 2; ++ConstantSide) {
        const int BaseSide = 1 - ConstantSide;
        if (BaseSide >= Op.NumInputs || !isVar(Op.Inputs[BaseSide]))
          continue;
        const std::optional<int64_t> Constant =
            constantImpl(D - 1, Op.Inputs[ConstantSide], 0);
        if (!Constant) {
          if (Exhausted)
            return fail();
          continue;
        }
        int64_t Base = 0;
        int64_t Result = 0;
        if (!evaluateImpl(D - 1, Op.Inputs[BaseSide], Depth + 1, Base) ||
            !checkedCoefficient(wideSigned(Base) * wideSigned(*Constant),
                                Result))
          return fail();
        return finish(Result);
      }
      return finish(1); // q*recip: the quotient-producing base.
    }
    case NdOp::INT_ADD:
    case NdOp::INT_SUB:
      if (Op.NumInputs >= 2 && isVar(Op.Inputs[0]) &&
          isVar(Op.Inputs[1])) {
        int64_t Left = 0;
        int64_t Right = 0;
        int64_t Result = 0;
        if (!evaluateImpl(D - 1, Op.Inputs[0], Depth + 1, Left) ||
            !evaluateImpl(D - 1, Op.Inputs[1], Depth + 1, Right))
          return fail();
        const llvm::APInt Combined =
            Op.Opcode == NdOp::INT_ADD
                ? wideSigned(Left) + wideSigned(Right)
                : wideSigned(Left) - wideSigned(Right);
        if (!checkedCoefficient(Combined, Result))
          return fail();
        return finish(Result);
      }
      return fail();
    default:
      return finish(1); // A non-multiplier op is the conceptual base.
    }
  }

  const std::vector<LowOp> &Ops;
  ModuloEvidenceBudget &Budget;
  bool Exhausted = false;
  std::map<Key, MemoEntry> LinearMemo;
  std::map<Key, MemoEntry> ConstantMemo;
};

//===----------------------------------------------------------------------===//
// inferBoundsFromModulo — bound a `switch(x % N)` table from its remainder
//===----------------------------------------------------------------------===//

/// A power-of-two modulo switch (`switch(x % 2^k)` / `switch(x & (2^k-1))`)
/// lowers the index to `and $(2^k-1)` with no `cmp` range guard.  When such a
/// table sits adjacent to another in rodata the two form one continuous
/// relocation run, so the run-length count over-reads the first table into the
/// second — fabricating bogus successor edges (and, with x87 residents, an
/// unbalanced stack the TOP propagation cannot reconcile).  The mask is a hard
/// upper bound on the index: it confines it to [0, M].  A following `-c` (clang
/// emits `dec` when a peeled iteration proved the low cases dead) lowers the
/// top index to M-c, so the table holds at most (M + Offset) + 1 entries.
/// Returns that bound, or 0 when the index does not reduce to a clean low-bit
/// mask.
uint32_t CFGBuilder::inferBoundsFromMask(
    const InsnRecord &Rec, const JumpTableInfo &Info, bool AllowNonContiguous,
    bool *IncompleteIndexDomain, bool *UsedNonContiguous,
    std::vector<uint32_t> *FeasibleCoordinates,
    std::vector<JumpTableMaskKnownOneWitness> *KnownOneWitnesses,
    bool RequireProducerReachability,
    const std::vector<va_t> *CandidateTargetsOverride,
    const std::set<va_t> *ReachableInstructions, bool AllowFixedPointBootstrap,
    bool AllowRawDenseShortcut, size_t *AggregateEvidenceBudget,
    bool *SemanticIndexDomainAmbiguous) const {
  if (IncompleteIndexDomain)
    *IncompleteIndexDomain = false;
  if (SemanticIndexDomainAmbiguous)
    *SemanticIndexDomainAmbiguous = false;
  if (UsedNonContiguous)
    *UsedNonContiguous = false;
  if (FeasibleCoordinates)
    FeasibleCoordinates->clear();
  if (KnownOneWitnesses)
    KnownOneWitnesses->clear();
  size_t LocalWorkBudget = limits::kMaxJumpTableMaskCoreEvidenceWork;
  size_t OwnedEvidenceBudget = std::min<size_t>(
      limits::kMaxJumpTableMaskFixedPointEvidenceWork,
      MaskFixedPointEvidenceBudgetForTesting.value_or(
          limits::kMaxJumpTableMaskFixedPointEvidenceWork));
  size_t *EvidenceBudget =
      AggregateEvidenceBudget ? AggregateEvidenceBudget : &OwnedEvidenceBudget;
  auto consumeBudget = [&](size_t &Budget, size_t Amount) {
    if (Amount > Budget) {
      Budget = 0;
      if (IncompleteIndexDomain)
        *IncompleteIndexDomain = true;
      return false;
    }
    Budget -= Amount;
    return true;
  };
  auto consumeBudgetProducts =
      [&](std::initializer_list<std::pair<size_t, size_t>> Products) {
        const size_t Max = std::numeric_limits<size_t>::max();
        size_t Total = 0;
        for (const auto &[Count, Cost] : Products) {
          if (Count != 0 && Cost > Max / Count)
            return consumeBudget(*EvidenceBudget, Max);
          const size_t Product = Count * Cost;
          if (Product > Max - Total)
            return consumeBudget(*EvidenceBudget, Max);
          Total += Product;
        }
        return consumeBudget(*EvidenceBudget, Total);
      };
  auto consumeWork = [&](size_t Amount) {
    // The ordinary proposal search keeps its historic per-core ceiling, but
    // every retained item/query also consumes the invocation-wide account.
    // A recursive core therefore cannot refresh either the graph allowance or
    // a second unmetered fallback after the candidate-local fixed point.
    if (Amount > LocalWorkBudget) {
      LocalWorkBudget = 0;
      if (IncompleteIndexDomain)
        *IncompleteIndexDomain = true;
      return false;
    }
    if (!consumeBudget(*EvidenceBudget, Amount)) {
      LocalWorkBudget = 0;
      return false;
    }
    LocalWorkBudget -= Amount;
    return true;
  };
  auto consumeWorkProducts =
      [&](std::initializer_list<std::pair<size_t, size_t>> Products) {
        // Query construction frequently retains a cross-product (for example,
        // one alternatives vector per index occurrence).  Compute that charge
        // before reserving or copying any attacker-shaped vectors.  Overflow is
        // evidence exhaustion, not permission to continue with a wrapped cost.
        const size_t Max = std::numeric_limits<size_t>::max();
        size_t Total = 0;
        for (const auto &[Count, Cost] : Products) {
          if (Count != 0 && Cost > Max / Count)
            return consumeWork(Max);
          const size_t Product = Count * Cost;
          if (Product > Max - Total)
            return consumeWork(Max);
          Total += Product;
        }
        return consumeWork(Total);
      };
  auto failGraphIncomplete = [&]() -> uint32_t {
    if (IncompleteIndexDomain)
      *IncompleteIndexDomain = true;
    return 0;
  };
  const bool HasFallbackIndexOccurrence =
      Info.IndexValueAlternatives.empty() && Info.IndexValueAtUse.Size != 0 &&
      Info.IndexUseAddr != InvalidVA && Info.IndexUseSeq >= 0;
  const size_t IndexOccurrenceCount =
      Info.IndexValueAlternatives.empty()
          ? (HasFallbackIndexOccurrence ? size_t{1} : size_t{0})
          : Info.IndexValueAlternatives.size();
  // Retain and validate the exact occurrence batch under both the per-core and
  // invocation-wide accounts.  The synthesized fallback is part of the same
  // prospective batch and cannot bypass either ceiling.
  if (!consumeWorkProducts(
          {{IndexOccurrenceCount, 1}, {IndexOccurrenceCount, 1}}))
    return 0;
  std::vector<JumpTableValueOccurrence> IndexOccurrences =
      Info.IndexValueAlternatives;
  if (HasFallbackIndexOccurrence)
    IndexOccurrences.push_back({Info.IndexValueAtUse, Info.IndexUseAddr,
                                Info.IndexUseSeq, Info.IndexValueDefinedAtUse});
  if (IndexOccurrences.empty())
    return 0;
  // Index occurrences describe values consumed by the table-address
  // expression.  A producer occurrence would require resolving immediately
  // after the defining op, while JumpTableValueQuery candidates are resolved
  // immediately before their use.  Current table strategies record index
  // inputs, so reject any incomplete producer-shaped metadata rather than
  // silently changing that contract here.
  if (std::any_of(IndexOccurrences.begin(), IndexOccurrences.end(),
                  [](const JumpTableValueOccurrence &Occurrence) {
                    return Occurrence.DefinedAtPoint ||
                           Occurrence.Value.Size == 0 ||
                           Occurrence.Addr == InvalidVA || Occurrence.Seq < 0;
                  }))
    return 0;

  auto matchForTargets = [&](const std::vector<JumpTableValueQuery> &Queries,
                             const std::vector<va_t> *Targets,
                             size_t *SharedBudget,
                             bool *AnalysisComplete = nullptr,
                             std::vector<bool> *QueryAnalysisComplete =
                                 nullptr) {
    return tableValuesMatchAtUses(Queries, AnalysisComplete,
                                  QueryAnalysisComplete, Rec.Addr, Targets,
                                  SharedBudget);
  };
  auto matchAtUses = [&](const std::vector<JumpTableValueQuery> &Queries,
                         bool *AnalysisComplete = nullptr,
                         std::vector<bool> *QueryAnalysisComplete = nullptr) {
    return matchForTargets(Queries, CandidateTargetsOverride, EvidenceBudget,
                           AnalysisComplete,
                           QueryAnalysisComplete);
  };

  // A shared computed-goto dispatch may start from one literal selector and
  // acquire additional masked selectors only after that literal edge opens a
  // case block.  Proving against the final provisional edge set in one pass is
  // circular: an otherwise unreachable case could donate the mask that makes
  // its own edge appear valid.  Compute the least candidate-local fixed point
  // instead.  Each iteration resolves values in a graph containing only the
  // coordinates authorized by earlier iterations, then rebuilds reachability
  // before admitting another domain.  The bounded physical run supplies the
  // coordinate-to-target map only; it never supplies selector authority.
  bool FixedPointGateIncomplete = false;
  if (AllowFixedPointBootstrap && !CandidateTargetsOverride &&
      !ReachableInstructions && CurrentImg && Info.PhysicalCapacity >= 2 &&
      Info.PhysicalCapacity <= 64 && [&] {
        const uint64_t PhysicalStride =
            Info.EntryStride != 0 ? Info.EntryStride : Info.EntrySize;
        std::optional<uint64_t> AddressScale;
        for (const JumpTableLoadRole &Role : Info.LoadRoles) {
          if (!consumeWorkProducts(
                  {{1, 1}, {Role.AllowedBases.size(), 1}})) {
            FixedPointGateIncomplete = true;
            return false;
          }
          if (Role.LoadWidth != Info.EntrySize || Role.AddressScale == 0 ||
              std::find(Role.AllowedBases.begin(), Role.AllowedBases.end(),
                        Info.BaseAddr) == Role.AllowedBases.end())
            continue;
          if (AddressScale && *AddressScale != Role.AddressScale)
            return false;
          AddressScale = Role.AddressScale;
        }
        // The fixed-point target vector is indexed directly by selector
        // coordinate.  Folded/pre-scaled layouts require the caller's separate
        // byte-coordinate mapping and stay on the ordinary proof path.
        return AddressScale && PhysicalStride != 0 &&
               *AddressScale == PhysicalStride;
      }()) {
    // One top-level candidate owns one aggregate allowance.  All seed
    // queries, graph snapshots, iterations, recursive core proofs, and precise
    // replay below share this same balance.  A resolver stage may examine
    // several unrelated candidate branches; letting an earlier candidate
    // consume a stage-global balance would make later proofs depend on map
    // iteration order rather than their own evidence.
    JumpTableInfo Physical = Info;
    Physical.RuntimeSlotIndices.clear();
    Physical.RuntimeCaseLabels.clear();
    Physical.AuthenticatedMaskCoordinates.clear();
    Physical.AuthenticatedMaskKnownOneWitnesses.clear();
    // PhysicalCapacity is a readable/storage ceiling, not necessarily the
    // exact table length: an adjacent object or the first non-code entry may
    // terminate the candidate earlier.  Find the largest completely decodable
    // prefix without ever treating that prefix as selector authority.  Exact
    // bounded reads are monotone, and the logarithmic search prepays every
    // decoded target/slot/result against the candidate's aggregate balance.
    std::vector<va_t> PhysicalTargets;
    std::vector<uint32_t> PhysicalSlots;
    uint32_t Lower = limits::kMinJumpTableEntries;
    uint32_t Upper = Info.PhysicalCapacity;
    while (Lower <= Upper) {
      const uint32_t Count = Lower + (Upper - Lower) / 2;
      if (!consumeBudget(*EvidenceBudget, size_t(Count) * 3))
        return 0;
      Physical.MaxEntries = Count;
      std::vector<uint32_t> Slots;
      std::vector<va_t> Targets =
          readTableEntries(*CurrentImg, Physical, &Slots);
      if (Targets.size() == Count) {
        PhysicalTargets = std::move(Targets);
        PhysicalSlots = std::move(Slots);
        Lower = Count + 1;
      } else {
        if (Count == 0)
          break;
        Upper = Count - 1;
      }
    }
    if (PhysicalTargets.size() >= limits::kMinJumpTableEntries) {
      const uint32_t CandidateCapacity =
          static_cast<uint32_t>(PhysicalTargets.size());
      JumpTableInfo FixedPointInfo = Info;
      FixedPointInfo.PhysicalCapacity = CandidateCapacity;
      if (PhysicalSlots.empty()) {
        PhysicalSlots.resize(PhysicalTargets.size());
        for (uint32_t I = 0; I < PhysicalSlots.size(); ++I)
          PhysicalSlots[I] = I;
      }
      const bool ExactDenseSlots =
          PhysicalSlots.size() == PhysicalTargets.size() &&
          std::all_of(
              PhysicalSlots.begin(), PhysicalSlots.end(),
              [&](uint32_t Slot) { return Slot < CandidateCapacity; }) &&
          std::set<uint32_t>(PhysicalSlots.begin(), PhysicalSlots.end())
                  .size() == PhysicalSlots.size();
      if (ExactDenseSlots) {
        auto targetsFor = [&](const std::set<uint32_t> &Coordinates)
            -> std::optional<std::vector<va_t>> {
          // Every round scans the physical coordinate map and retains at most
          // one target per authorized coordinate.  Both costs precede growth.
          if (!consumeBudgetProducts({{PhysicalTargets.size(), 1},
                                      {Coordinates.size(), 1}}))
            return std::nullopt;
          std::vector<va_t> Targets;
          Targets.reserve(Coordinates.size());
          for (size_t I = 0; I < PhysicalTargets.size(); ++I)
            if (Coordinates.count(PhysicalSlots[I]))
              Targets.push_back(PhysicalTargets[I]);
          return Targets;
        };
        const std::vector<va_t> NoTargets;
        const std::set<va_t> &Roots = ActiveJumpTableProofRoots
                                          ? *ActiveJumpTableProofRoots
                                          : PersistentCFGRoots;
        std::set<uint32_t> Authorized;
        std::vector<JumpTableValueQuery> SeedQueries;
        const size_t Capacity = CandidateCapacity;
        if (IndexOccurrences.size() >
            std::numeric_limits<size_t>::max() / Capacity) {
          if (IncompleteIndexDomain)
            *IncompleteIndexDomain = true;
          return 0;
        }
        const size_t SeedQueryCount = Capacity * IndexOccurrences.size();
        constexpr size_t SeedWorkPerQuery = 7;
        if (SeedQueryCount > *EvidenceBudget / SeedWorkPerQuery) {
          *EvidenceBudget = 0;
          if (IncompleteIndexDomain)
            *IncompleteIndexDomain = true;
          return 0;
        }
        if (!consumeBudget(*EvidenceBudget,
                           SeedQueryCount * SeedWorkPerQuery))
          return 0;
        SeedQueries.reserve(SeedQueryCount);
        for (uint32_t Coordinate = 0; Coordinate < CandidateCapacity;
             ++Coordinate)
          for (const JumpTableValueOccurrence &Index : IndexOccurrences) {
            JumpTableValueQuery Query;
            Query.Candidate = Index.Value;
            Query.UseAddr = Index.Addr;
            Query.UseSeq = Index.Seq;
            Query.Alternatives = {{NdVar::cst(Coordinate, Index.Value.Size),
                                   InvalidVA, -1, false},
                                  {NdVar::scalar(Coordinate, Index.Value.Size),
                                   InvalidVA, -1, false}};
            Query.AllowZeroExtension = true;
            SeedQueries.push_back(std::move(Query));
          }
        std::vector<bool> SeedComplete;
        const std::vector<bool> SeedMatches = matchForTargets(
            SeedQueries, &NoTargets, EvidenceBudget, nullptr, &SeedComplete);
        bool SeedAnalysisIncomplete =
            SeedMatches.size() != SeedQueries.size() ||
            SeedComplete.size() != SeedQueries.size();
        for (uint32_t Coordinate = 0;
             !SeedAnalysisIncomplete && Coordinate < CandidateCapacity;
             ++Coordinate) {
          const size_t Begin = size_t(Coordinate) * IndexOccurrences.size();
          const size_t End = Begin + IndexOccurrences.size();
          if (!std::all_of(SeedComplete.begin() + Begin,
                           SeedComplete.begin() + End,
                           [](bool Complete) { return Complete; })) {
            SeedAnalysisIncomplete = true;
            break;
          }
          if (std::all_of(SeedMatches.begin() + Begin,
                          SeedMatches.begin() + End,
                          [](bool Match) { return Match; })) {
            Authorized.insert(Coordinate);
            break;
          }
        }
        // A selector may already be bounded before the first indirect edge
        // (for example `arg & 7`).  Run the ordinary proof once in the empty-
        // edge graph so that dynamic, but independently authenticated, entry
        // domains can seed the same monotone iteration.
        if (Authorized.empty()) {
          bool EntryReachabilityComplete = false;
          const std::set<va_t> EntryReachable = candidateReachableInstructions(
              Rec, NoTargets, Roots, Info.StorageRanges, EvidenceBudget,
              &EntryReachabilityComplete);
          if (!EntryReachabilityComplete)
            return failGraphIncomplete();
          bool EntryIncomplete = false;
          bool EntrySemanticAmbiguous = false;
          bool EntryNonContiguous = false;
          std::vector<uint32_t> EntryCoordinates;
          std::vector<JumpTableMaskKnownOneWitness> EntryKnownOneWitnesses;
          const uint32_t EntryBound = inferBoundsFromMask(
              Rec, FixedPointInfo, AllowNonContiguous, &EntryIncomplete,
              &EntryNonContiguous, &EntryCoordinates,
              &EntryKnownOneWitnesses,
              /*RequireProducerReachability=*/true, &NoTargets, &EntryReachable,
              /*AllowFixedPointBootstrap=*/false,
              /*AllowRawDenseShortcut=*/true, EvidenceBudget,
              &EntrySemanticAmbiguous);
          if (EntrySemanticAmbiguous) {
            if (SemanticIndexDomainAmbiguous)
              *SemanticIndexDomainAmbiguous = true;
            return 0;
          }
          if (EntryBound != 0 && !EntryIncomplete &&
              !EntryCoordinates.empty() &&
              std::all_of(EntryCoordinates.begin(), EntryCoordinates.end(),
                          [&](uint32_t Coordinate) {
                            return Coordinate < CandidateCapacity;
                          })) {
            // Empty-edge reachability proves an independent seed, not closure:
            // an admitted case can flow back to this dispatch after widening
            // the selector.  Replay the complete entry domain in the same
            // candidate-local least fixed point used for literal seeds.
            Authorized.insert(EntryCoordinates.begin(),
                              EntryCoordinates.end());
            if (KnownOneWitnesses)
              *KnownOneWitnesses = std::move(EntryKnownOneWitnesses);
          }
        }
        if (!Authorized.empty()) {
          for (uint32_t Iteration = 0; Iteration <= CandidateCapacity;
               ++Iteration) {
            std::optional<std::vector<va_t>> AuthorizedTargetsOr =
                targetsFor(Authorized);
            if (!AuthorizedTargetsOr)
              return 0;
            const std::vector<va_t> &AuthorizedTargets = *AuthorizedTargetsOr;
            bool ReachabilityComplete = false;
            const std::set<va_t> Reachable = candidateReachableInstructions(
                Rec, AuthorizedTargets, Roots, Info.StorageRanges,
                EvidenceBudget, &ReachabilityComplete);
            if (!ReachabilityComplete)
              return failGraphIncomplete();
            if (!Reachable.count(CurrentFuncEntry))
              return 0;

            bool CoreIncomplete = false;
            bool CoreSemanticAmbiguous = false;
            bool CoreNonContiguous = false;
            std::vector<uint32_t> CoreCoordinates;
            std::vector<JumpTableMaskKnownOneWitness> CoreKnownOneWitnesses;
            const uint32_t CoreBound = inferBoundsFromMask(
                Rec, FixedPointInfo, AllowNonContiguous, &CoreIncomplete,
                &CoreNonContiguous, &CoreCoordinates,
                &CoreKnownOneWitnesses,
                /*RequireProducerReachability=*/!AuthorizedTargets.empty(),
                &AuthorizedTargets, &Reachable,
                /*AllowFixedPointBootstrap=*/false,
                /*AllowRawDenseShortcut=*/true, EvidenceBudget,
                &CoreSemanticAmbiguous);
            if (CoreSemanticAmbiguous) {
              if (SemanticIndexDomainAmbiguous)
                *SemanticIndexDomainAmbiguous = true;
              return 0;
            }
            if (CoreIncomplete) {
              if (IncompleteIndexDomain)
                *IncompleteIndexDomain = true;
              return 0;
            }

            // The next-set copy, candidate insertions, and closure scan are
            // transactional evidence work.  Prepay them before mutating Next.
            if (!consumeBudgetProducts({{Authorized.size(), 1},
                                        {CoreCoordinates.size(), 2}}))
              return 0;
            std::set<uint32_t> Next = Authorized;
            if (CoreBound != 0 && !CoreCoordinates.empty() &&
                std::all_of(CoreCoordinates.begin(), CoreCoordinates.end(),
                            [&](uint32_t Coordinate) {
                              return Coordinate < CandidateCapacity;
                            }))
              Next.insert(CoreCoordinates.begin(), CoreCoordinates.end());

            const bool CoreClosed =
                CoreBound != 0 && !CoreCoordinates.empty() &&
                std::all_of(CoreCoordinates.begin(), CoreCoordinates.end(),
                            [&](uint32_t Coordinate) {
                              return Authorized.count(Coordinate);
                            });
            if (Next == Authorized) {
              // The seed edge is exact but its destination may not have been
              // decoded when this immutable resolver graph was built.  Queue
              // only the already-authorized targets for recursive descent;
              // do not publish a partial table or admit any additional
              // coordinate until a later graph proves the closed domain.
              if (!consumeBudgetProducts(
                      {{AuthorizedTargets.size(),
                        orderedEvidenceLookupWork(ExploredAddrs.size())},
                       {AuthorizedTargets.size(),
                        orderedEvidenceLookupWork(BlockStarts.size())},
                       {AuthorizedTargets.size(),
                        orderedEvidenceLookupWork(Insns.size())}}))
                return 0;
              const bool UndecodedAttempt = std::any_of(
                  AuthorizedTargets.begin(), AuthorizedTargets.end(),
                  [&](va_t Target) {
                    return ExploredAddrs.count(Target) &&
                           BlockStarts.count(Target) && !Insns.count(Target);
                  });
              if (UndecodedAttempt) {
                if (IncompleteIndexDomain)
                  *IncompleteIndexDomain = true;
                return 0;
              }
              const bool NeedsGraphGrowth = std::any_of(
                  AuthorizedTargets.begin(), AuthorizedTargets.end(),
                  [&](va_t Target) {
                    return !ExploredAddrs.count(Target) ||
                           !BlockStarts.count(Target);
                  });
              if (!AuthorizedTargets.empty() && NeedsGraphGrowth) {
                // This record outlives the candidate-local resolver.  Prepay
                // the ordered map insertion, its node construction and later
                // destruction, plus both the target copy and the vector's
                // future deep clear.  A later stage clears this map before it
                // initializes a fresh evidence account, so none of that
                // cleanup may be deferred to the next budget.
                if (!consumeBudgetProducts(
                        {{1, orderedEvidenceLookupWork(
                                 CandidateFixedPointExplorationTargets.size())}}))
                  return 0;
                auto Existing =
                    CandidateFixedPointExplorationTargets.find(Rec.Addr);
                const size_t ExistingTargets =
                    Existing == CandidateFixedPointExplorationTargets.end()
                        ? 0
                        : Existing->second.size();
                if (AuthorizedTargets.size() >
                        std::numeric_limits<size_t>::max() - ExistingTargets ||
                    !consumeBudgetProducts(
                        {{1, 2},
                         {ExistingTargets + AuthorizedTargets.size(), 2}}))
                  return 0;
                if (Existing == CandidateFixedPointExplorationTargets.end())
                  CandidateFixedPointExplorationTargets.emplace(
                      Rec.Addr, AuthorizedTargets);
                else
                  Existing->second.insert(Existing->second.end(),
                                          AuthorizedTargets.begin(),
                                          AuthorizedTargets.end());
              }
              if (!NeedsGraphGrowth &&
                  Authorized.size() >= limits::kMinJumpTableEntries &&
                  CoreClosed) {
                const uint32_t Bound = *Authorized.rbegin() + 1;
                if (FeasibleCoordinates &&
                    !consumeBudget(*EvidenceBudget, Authorized.size()))
                  return 0;
                if (UsedNonContiguous)
                  *UsedNonContiguous = Authorized.size() != Bound;
                if (FeasibleCoordinates)
                  FeasibleCoordinates->assign(Authorized.begin(),
                                              Authorized.end());
                if (KnownOneWitnesses && !CoreKnownOneWitnesses.empty())
                  *KnownOneWitnesses = std::move(CoreKnownOneWitnesses);
                return Bound;
              }
              // This is a normal graph-growth request, not incomplete proof
              // evidence.  Marking the candidate incomplete here makes the
              // enclosing proposal stage roll back the just-recorded
              // exploration target, so a literal seed can never open the
              // case that supplies the next selector domain.  The immutable
              // current graph still publishes no table; the outer stage
              // decodes only these already-authorized targets and retries.
              // If every authorized destination is already a block start,
              // another retry cannot change this graph; record that as an
              // incomplete domain so the unresolved table-shaped branch is
              // preserved fail-closed instead of becoming a tail call.
              if (!NeedsGraphGrowth && IncompleteIndexDomain)
                *IncompleteIndexDomain = true;
              return 0;
            }
            Authorized = std::move(Next);
          }
          if (IncompleteIndexDomain)
            *IncompleteIndexDomain = true;
          return 0;
        }
        // A revalidation graph carrying provisional edges must not fall back
        // to proving itself when neither constants nor the empty-edge graph
        // supplied an initial domain.
        if (!Rec.JumpTableTargets.empty()) {
          if (IncompleteIndexDomain)
            *IncompleteIndexDomain = true;
          return 0;
        }
      }
    }
  }
  if (FixedPointGateIncomplete)
    return 0;

  // Layout-specific fixed-point mapping is unnecessary when the runtime
  // domain is already provable without any candidate edge.  Revalidate every
  // provisional graph once with an empty target vector before the strict
  // fallback below.  This preserves independently bounded pre-scaled,
  // compact, large, and otherwise non-1:1 layouts while preventing their
  // current successors from making a case-local mask reachable.
  if (AllowFixedPointBootstrap && !CandidateTargetsOverride &&
      !ReachableInstructions && !Rec.JumpTableTargets.empty()) {
    const std::vector<va_t> NoTargets;
    const std::set<va_t> &Roots = ActiveJumpTableProofRoots
                                      ? *ActiveJumpTableProofRoots
                                      : PersistentCFGRoots;
    bool EntryReachabilityComplete = false;
    const std::set<va_t> EntryReachable = candidateReachableInstructions(
        Rec, NoTargets, Roots, Info.StorageRanges, EvidenceBudget,
        &EntryReachabilityComplete);
    if (!EntryReachabilityComplete)
      return failGraphIncomplete();
    if (EntryReachable.count(CurrentFuncEntry)) {
      bool EntryIncomplete = false;
      bool EntrySemanticAmbiguous = false;
      bool EntryNonContiguous = false;
      std::vector<uint32_t> EntryCoordinates;
      std::vector<JumpTableMaskKnownOneWitness> EntryKnownOneWitnesses;
      const uint32_t EntryBound = inferBoundsFromMask(
          Rec, Info, AllowNonContiguous, &EntryIncomplete,
          &EntryNonContiguous, &EntryCoordinates,
          &EntryKnownOneWitnesses,
          /*RequireProducerReachability=*/true, &NoTargets, &EntryReachable,
          /*AllowFixedPointBootstrap=*/false,
          /*AllowRawDenseShortcut=*/true, EvidenceBudget,
          &EntrySemanticAmbiguous);
      if (EntrySemanticAmbiguous) {
        if (SemanticIndexDomainAmbiguous)
          *SemanticIndexDomainAmbiguous = true;
        return 0;
      }
      if (EntryBound != 0 && !EntryIncomplete && !EntryCoordinates.empty()) {
        // Layouts that cannot use the coordinate-to-target fixed point above
        // may retain an empty-edge certificate only when every provisional
        // destination is one-shot: no ordinary path from a candidate target
        // can return to this exact dispatch and mutate the selector.  Charge
        // the second graph snapshot to the same aggregate allowance.
        if (!consumeBudgetProducts(
                {{Rec.JumpTableTargets.size(), 1},
                 {Rec.JumpTableTargets.size(), 1}}))
          return 0;
        const std::set<va_t> CandidateRoots(Rec.JumpTableTargets.begin(),
                                            Rec.JumpTableTargets.end());
        bool CandidateReachabilityComplete = false;
        const std::set<va_t> FromCandidateTargets =
            candidateReachableInstructions(Rec, NoTargets, CandidateRoots,
                                           Info.StorageRanges, EvidenceBudget,
                                           &CandidateReachabilityComplete);
        if (!CandidateReachabilityComplete)
          return failGraphIncomplete();
        const bool AllCandidateRootsDecoded =
            std::all_of(CandidateRoots.begin(), CandidateRoots.end(),
                        [&](va_t Target) {
                          return FromCandidateTargets.count(Target);
                        });
        if (AllCandidateRootsDecoded &&
            !FromCandidateTargets.count(Rec.Addr)) {
          if (UsedNonContiguous)
            *UsedNonContiguous = EntryNonContiguous;
          if (FeasibleCoordinates)
            *FeasibleCoordinates = std::move(EntryCoordinates);
          if (KnownOneWitnesses)
            *KnownOneWitnesses = std::move(EntryKnownOneWitnesses);
          return EntryBound;
        }
      }
    }
  }

  // A graph that already carries provisional targets must be revalidated only
  // through the candidate-local least fixed point above.  If its physical
  // layout cannot enter that proof, retaining the current edges in an ordinary
  // resolver query would let the proposal certify its own producers.
  if (AllowFixedPointBootstrap && !CandidateTargetsOverride &&
      !ReachableInstructions && !Rec.JumpTableTargets.empty()) {
    if (IncompleteIndexDomain)
      *IncompleteIndexDomain = true;
    return 0;
  }

  std::vector<LowOp> Ops;
  for (auto It = Insns.lower_bound(CurrentFuncEntry);
       It != Insns.end() && It->first <= Rec.Addr; ++It)
    if (!ReachableInstructions || ReachableInstructions->count(It->first)) {
      // Retained LowIR is attacker-sized state.  Prepay it before growing the
      // proposal vector; the later semantic scan is charged independently.
      if (!consumeWork(It->second.Ops.size()))
        return failGraphIncomplete();
      Ops.insert(Ops.end(), It->second.Ops.begin(), It->second.Ops.end());
    }

  struct MaskCandidate {
    JumpTableValueOccurrence Output;
    NdVar DynamicInput;
    uint32_t Bound = 0;
    uint32_t Mask = 0;
    std::vector<uint32_t> Coordinates;
    bool CompleteDomain = false;
    bool NonContiguous = false;
    JumpTableValueQuery ConstantProof;
  };
  std::vector<MaskCandidate> Candidates;
  std::vector<JumpTableValueOccurrence> RelevantMaskRoots;
  auto FailIncomplete = [&]() -> uint32_t {
    if (IncompleteIndexDomain)
      *IncompleteIndexDomain = true;
    return 0;
  };

  enum class ConstantLookupKind : uint8_t { NoProof, Value, Exhausted };
  struct ConstantLookupResult {
    ConstantLookupKind Kind = ConstantLookupKind::NoProof;
    int64_t Value = 0;
  };
  std::function<ConstantLookupResult(int, NdVar, int)> constValueOf =
      [&](int FromIdx, NdVar V, int Depth) -> ConstantLookupResult {
    if (V.isConst()) {
      // Constants are proposal syntax only.  Reject an unsigned high-bit
      // pattern that cannot be represented as the signed transform addend.
      if (V.Offset >
          static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
        return {};
      return {ConstantLookupKind::Value, static_cast<int64_t>(V.Offset)};
    }
    if ((!V.isReg() && !V.isTemp()) ||
        Depth > limits::kMaxQuasiCopyDepth)
      return {};
    int Definition = -1;
    for (int I = FromIdx; I >= 0; --I) {
      if (!consumeWork(1))
        return {ConstantLookupKind::Exhausted, 0};
      const NdVar &Output = Ops[I].Output;
      if (Output.Space == V.Space && Output.Offset == V.Offset) {
        Definition = I;
        break;
      }
    }
    if (Definition < 0)
      return {};
    const LowOp &Op = Ops[Definition];
    if ((Op.Opcode == NdOp::COPY || Op.Opcode == NdOp::INT_ZEXT ||
         Op.Opcode == NdOp::INT_SEXT) &&
        Op.NumInputs >= 1)
      return constValueOf(Definition - 1, Op.Inputs[0], Depth + 1);
    return {};
  };

  auto conditionalMergeOutput =
      [](const LowOp &Producer,
         const InsnRecord &Insn) -> std::optional<JumpTableValueOccurrence> {
    for (const LowOp &Merge : Insn.Ops) {
      if (Merge.Opcode != NdOp::SELECT || Merge.NumInputs < 3 ||
          Merge.Output.Size == 0)
        continue;
      for (int Arm = 1; Arm < std::min<int>(Merge.NumInputs, 3); ++Arm)
        if (Merge.Inputs[Arm].Space == Producer.Output.Space &&
            Merge.Inputs[Arm].Offset == Producer.Output.Offset &&
            Merge.Inputs[Arm].Size == Producer.Output.Size)
          return JumpTableValueOccurrence{Merge.Output, Merge.Addr, Merge.Seq,
                                          /*DefinedAtPoint=*/true};
    }
    return std::nullopt;
  };

  // A lexical COPY trace is used only to propose the encoded mask value.  It
  // is not evidence: the batch query below re-resolves the operand at the AND
  // use through the complete CFG, exact lane, canonical frame epoch, and call/
  // atomic/unknown-alias barriers.  A sibling or stale lifetime therefore
  // merely proposes a value whose occurrence proof fails.
  for (int I = 0; I < static_cast<int>(Ops.size()); ++I) {
    if (!consumeWork(1))
      return FailIncomplete();
    const LowOp &Op = Ops[I];
    if (Op.Opcode != NdOp::INT_AND || Op.NumInputs < 2 || Op.Output.Size == 0)
      continue;
    RelevantMaskRoots.push_back(
        {Op.Output, Op.Addr, Op.Seq, /*DefinedAtPoint=*/true});
    const auto InsnIt = Insns.find(Op.Addr);
    if (InsnIt == Insns.end())
      continue;
    for (int ConstantSide = 0; ConstantSide < 2; ++ConstantSide) {
      if (!consumeWork(1))
        return FailIncomplete();
      const NdVar &MaskOperand = Op.Inputs[ConstantSide];
      const NdVar &DynamicOperand = Op.Inputs[1 - ConstantSide];
      const ConstantLookupResult Proposed =
          constValueOf(I - 1, MaskOperand, 0);
      if (Proposed.Kind == ConstantLookupKind::Exhausted)
        return FailIncomplete();
      const std::optional<uint64_t> ConstantWidthMask =
          integerWidthMask(MaskOperand.Size);
      if (Proposed.Kind != ConstantLookupKind::Value || !ConstantWidthMask)
        continue;
      // INT_AND coerces both inputs to the output width.  The encoded constant
      // is zero-extended/truncated to that width, and a narrower dynamic input
      // cannot make the newly introduced high bits nonzero.  Deriving the
      // domain from the constant operand's own width would over-count an i8
      // output masked by i16(0x1ff), while ignoring the dynamic width would
      // over-count zext(i8) & i16(0x1ff).
      const std::optional<uint64_t> EffectiveMask = effectiveIntegerAndMask(
          static_cast<uint64_t>(Proposed.Value), MaskOperand.Size,
          DynamicOperand.Size, Op.Output.Size);
      if (!EffectiveMask)
        continue;
      uint64_t Mask = *EffectiveMask;
      if (Mask == 0)
        continue;
      const bool NonContiguous = (Mask & (Mask + 1)) != 0;
      if (NonContiguous && !AllowNonContiguous)
        continue;
      // For an arbitrary bit mask M, every result lies in the exact physical
      // coordinate interval [0,M].  Filling M's zero bits to the next low-bit
      // mask over-counts a trailing slot (`x & 0x1e` has max 30, not 31) and
      // previously relied on min(relocation-run, mask-bound) to hide it.
      // Preserve M itself; gaps inside [0,M] are handled as dense filler slots.
      const bool DomainRepresentable =
          Mask != std::numeric_limits<uint64_t>::max() &&
          Mask + 1 >= limits::kMinJumpTableEntries &&
          Mask + 1 <= limits::kMaxJumpTableEntries;
      std::vector<uint32_t> Coordinates;
      if (DomainRepresentable) {
        // The dense coordinate walk is retained evidence, not a free
        // by-product of one mask proposal.  Charge before any allocation.
        if (!consumeWork(static_cast<size_t>(Mask) + 1))
          return FailIncomplete();
        for (uint32_t Value = 0; Value <= static_cast<uint32_t>(Mask); ++Value)
          if ((Value & ~static_cast<uint32_t>(Mask)) == 0)
            Coordinates.push_back(Value);
      }

      JumpTableValueQuery ConstantProof;
      ConstantProof.Candidate = MaskOperand;
      ConstantProof.UseAddr = Op.Addr;
      ConstantProof.UseSeq = Op.Seq;
      ConstantProof.Alternatives.push_back(
          {NdVar::scalar(static_cast<uint64_t>(Proposed.Value) &
                             *ConstantWidthMask,
                         MaskOperand.Size),
           InvalidVA, -1, false});
      JumpTableValueOccurrence BoundedOutput{Op.Output, Op.Addr, Op.Seq,
                                             /*DefinedAtPoint=*/true};
      // Register-predicated ARM operations are represented as an explicit
      // SELECT inside one InsnRecord and do not necessarily set
      // IsInstructionGuard.  Either form leaves an unmasked feasible arm and
      // is therefore an incomplete domain until a separate path proof shows
      // the predicate is always true at the table load.
      const std::optional<JumpTableValueOccurrence> ConditionalOutput =
          conditionalMergeOutput(Op, InsnIt->second);
      const bool IncompleteProducer = InsnIt->second.IsInstructionGuard ||
                                      ConditionalOutput.has_value() ||
                                      !DomainRepresentable;
      Candidates.push_back(
          {BoundedOutput, DynamicOperand,
           DomainRepresentable ? static_cast<uint32_t>(Mask + 1) : 0u,
           static_cast<uint32_t>(Mask), std::move(Coordinates),
           !IncompleteProducer, NonContiguous, std::move(ConstantProof)});
    }
  }
  if (Candidates.empty() && RelevantMaskRoots.empty())
    return 0;

  std::vector<JumpTableValueQuery> ConstantQueries;
  // Each retained constant-proof query owns one scalar alternative.
  if (!consumeWorkProducts({{Candidates.size(), 2}}))
    return FailIncomplete();
  ConstantQueries.reserve(Candidates.size());
  for (const MaskCandidate &Candidate : Candidates)
    ConstantQueries.push_back(Candidate.ConstantProof);
  std::vector<bool> ConstantsMatch;
  if (!ConstantQueries.empty()) {
    bool ConstantAnalysisComplete = false;
    ConstantsMatch = matchAtUses(ConstantQueries, &ConstantAnalysisComplete);
    if (!ConstantAnalysisComplete || ConstantsMatch.size() != Candidates.size())
      return FailIncomplete();
  }

  // A negative translation after a mask is safe only when every feasible mask
  // coordinate is at least the translation magnitude.  Clang commonly proves
  // that lower bound by setting a bit before the mask (`(x | 1) & 7; --x`).
  // Recover those known-one bits occurrence-locally: lexical ORs merely propose
  // outputs/constants, while the two batched queries below prove both the OR
  // constant at its own use and that every reaching arm of the AND's dynamic
  // input is one of the exact OR outputs carrying the same required bits.
  struct KnownOneProducer {
    JumpTableValueOccurrence Output;
    NdVar ConstantOperand;
    uint64_t KnownOneBits = 0;
    JumpTableValueQuery ConstantProof;
  };
  std::vector<KnownOneProducer> KnownOneProducers;
  auto ensureAppendCapacity = [&](auto &Values) {
    if (Values.size() < Values.capacity())
      return true;
    const size_t Max = std::numeric_limits<size_t>::max();
    const size_t NewCapacity =
        Values.capacity() == 0
            ? size_t{1}
            : (Values.capacity() > Max / 2 ? Max : Values.capacity() * 2);
    // reserve() may allocate the new slots and move every retained element.
    // Charge the complete new capacity before either action, so an exhausted
    // proof never grows an attacker-shaped container first.
    if (NewCapacity == Max || !consumeWork(NewCapacity))
      return false;
    Values.reserve(NewCapacity);
    return true;
  };
  for (int I = 0; I < static_cast<int>(Ops.size()); ++I) {
    if (!consumeWork(1))
      return FailIncomplete();
    const LowOp &Op = Ops[I];
    if (Op.Opcode != NdOp::INT_OR || Op.NumInputs < 2 || Op.Output.Size == 0)
      continue;
    const std::optional<uint64_t> OutputMask = integerWidthMask(Op.Output.Size);
    if (!OutputMask)
      continue;
    for (int ConstantSide = 0; ConstantSide < 2; ++ConstantSide) {
      const NdVar &ConstantOperand = Op.Inputs[ConstantSide];
      const ConstantLookupResult Proposed =
          constValueOf(I - 1, ConstantOperand, 0);
      if (Proposed.Kind == ConstantLookupKind::Exhausted)
        return FailIncomplete();
      if (Proposed.Kind != ConstantLookupKind::Value)
        continue;
      const uint64_t KnownOneBits =
          static_cast<uint64_t>(Proposed.Value) & *OutputMask;
      if (KnownOneBits == 0)
        continue;
      if (!consumeWorkProducts({{1, 4}}))
        return FailIncomplete();
      JumpTableValueQuery ConstantProof;
      ConstantProof.Candidate = ConstantOperand;
      ConstantProof.UseAddr = Op.Addr;
      ConstantProof.UseSeq = Op.Seq;
      ConstantProof.Alternatives.push_back(
          {NdVar::scalar(KnownOneBits, ConstantOperand.Size), InvalidVA, -1,
           false});
      if (!ensureAppendCapacity(KnownOneProducers))
        return FailIncomplete();
      KnownOneProducers.push_back(
          {{Op.Output, Op.Addr, Op.Seq, /*DefinedAtPoint=*/true},
           ConstantOperand, KnownOneBits, std::move(ConstantProof)});
      break;
    }
  }

  std::vector<bool> KnownOneConstantsMatch;
  if (!KnownOneProducers.empty()) {
    if (!consumeWorkProducts({{KnownOneProducers.size(), 2}}))
      return FailIncomplete();
    std::vector<JumpTableValueQuery> Queries;
    Queries.reserve(KnownOneProducers.size());
    for (KnownOneProducer &Producer : KnownOneProducers)
      Queries.push_back(std::move(Producer.ConstantProof));
    bool AnalysisComplete = false;
    KnownOneConstantsMatch = matchAtUses(Queries, &AnalysisComplete);
    if (!AnalysisComplete ||
        KnownOneConstantsMatch.size() != KnownOneProducers.size())
      return FailIncomplete();
  }

  struct KnownOneGroup {
    size_t CandidateIndex = 0;
    uint32_t RequiredBits = 0;
    std::vector<JumpTableValueOccurrence> Producers;
    std::vector<size_t> ProducerIndices;
  };
  std::vector<KnownOneGroup> KnownOneGroups;
  for (size_t CandidateIndex = 0; CandidateIndex < Candidates.size();
       ++CandidateIndex) {
    MaskCandidate &Candidate = Candidates[CandidateIndex];
    if (!Candidate.CompleteDomain)
      continue;
    for (size_t ProducerIndex = 0;
         ProducerIndex < KnownOneProducers.size(); ++ProducerIndex) {
      if (!consumeWork(1))
        return FailIncomplete();
      const KnownOneProducer &Producer = KnownOneProducers[ProducerIndex];
      if (!KnownOneConstantsMatch[ProducerIndex] ||
          Producer.Output.Value.Size != Candidate.DynamicInput.Size)
        continue;
      const uint32_t RequiredBits =
          static_cast<uint32_t>(Producer.KnownOneBits) & Candidate.Mask;
      if (RequiredBits == 0)
        continue;
      if (!consumeWork(KnownOneGroups.size()))
        return FailIncomplete();
      auto Group = std::find_if(
          KnownOneGroups.begin(), KnownOneGroups.end(),
          [&](const KnownOneGroup &Known) {
            return Known.CandidateIndex == CandidateIndex &&
                   Known.RequiredBits == RequiredBits;
          });
      if (Group == KnownOneGroups.end()) {
        if (!consumeWorkProducts({{1, 3}}))
          return FailIncomplete();
        if (!ensureAppendCapacity(KnownOneGroups))
          return FailIncomplete();
        KnownOneGroup NewGroup;
        NewGroup.CandidateIndex = CandidateIndex;
        NewGroup.RequiredBits = RequiredBits;
        if (!ensureAppendCapacity(NewGroup.Producers) ||
            !ensureAppendCapacity(NewGroup.ProducerIndices))
          return FailIncomplete();
        NewGroup.Producers.push_back(Producer.Output);
        NewGroup.ProducerIndices.push_back(ProducerIndex);
        KnownOneGroups.push_back(std::move(NewGroup));
      } else {
        if (!consumeWorkProducts({{1, 2}}))
          return FailIncomplete();
        if (!ensureAppendCapacity(Group->Producers) ||
            !ensureAppendCapacity(Group->ProducerIndices))
          return FailIncomplete();
        Group->Producers.push_back(Producer.Output);
        Group->ProducerIndices.push_back(ProducerIndex);
      }
    }
  }

  if (!consumeWork(Candidates.size()))
    return FailIncomplete();
  std::vector<std::vector<size_t>> MatchedKnownOneProducers(Candidates.size());
  if (!KnownOneGroups.empty()) {
    if (!consumeWorkProducts(
            {{KnownOneGroups.size(), 1}, {Candidates.size(), 1}}))
      return FailIncomplete();
    std::vector<JumpTableValueQuery> Queries;
    Queries.reserve(KnownOneGroups.size());
    for (KnownOneGroup &Group : KnownOneGroups) {
      JumpTableValueQuery Query;
      const MaskCandidate &Candidate = Candidates[Group.CandidateIndex];
      Query.Candidate = Candidate.DynamicInput;
      Query.UseAddr = Candidate.Output.Addr;
      Query.UseSeq = Candidate.Output.Seq;
      Query.Alternatives = std::move(Group.Producers);
      Queries.push_back(std::move(Query));
    }
    bool AnalysisComplete = false;
    const std::vector<bool> Matches = matchAtUses(Queries, &AnalysisComplete);
    if (!AnalysisComplete || Matches.size() != KnownOneGroups.size())
      return FailIncomplete();
    if (!consumeWork(Matches.size()))
      return FailIncomplete();
    size_t SingletonCount = 0;
    for (size_t I = 0; I < Matches.size(); ++I) {
      if (!Matches[I])
        continue;
      if (Queries[I].Alternatives.size() >
          std::numeric_limits<size_t>::max() - SingletonCount)
        return FailIncomplete();
      SingletonCount += Queries[I].Alternatives.size();
    }
    // A union proof says every arm is one member of the group, but attaching
    // every member makes the final certificate depend on unrelated/provisional
    // siblings.  Admit known-one refinement only when one exact OR occurrence
    // alone covers every reaching arm.  This is deliberately conservative for
    // true multi-producer PHIs and keeps the certificate proof-minimal.
    if (!consumeWorkProducts({{SingletonCount, 3}, {Matches.size(), 1}}))
      return FailIncomplete();
    struct SingletonKnownOneProof {
      size_t GroupIndex = 0;
      size_t ProducerSlot = 0;
    };
    std::vector<SingletonKnownOneProof> SingletonProofs;
    std::vector<JumpTableValueQuery> SingletonQueries;
    SingletonProofs.reserve(SingletonCount);
    SingletonQueries.reserve(SingletonCount);
    for (size_t I = 0; I < Matches.size(); ++I) {
      if (!Matches[I])
        continue;
      const MaskCandidate &Candidate =
          Candidates[KnownOneGroups[I].CandidateIndex];
      for (size_t ProducerSlot = 0;
           ProducerSlot < Queries[I].Alternatives.size(); ++ProducerSlot) {
        JumpTableValueQuery Query;
        Query.Candidate = Candidate.DynamicInput;
        Query.UseAddr = Candidate.Output.Addr;
        Query.UseSeq = Candidate.Output.Seq;
        Query.Alternatives.push_back(Queries[I].Alternatives[ProducerSlot]);
        SingletonProofs.push_back({I, ProducerSlot});
        SingletonQueries.push_back(std::move(Query));
      }
    }
    bool SingletonAnalysisComplete = SingletonQueries.empty();
    const std::vector<bool> SingletonMatches =
        SingletonQueries.empty()
            ? std::vector<bool>{}
            : matchAtUses(SingletonQueries, &SingletonAnalysisComplete);
    if (!SingletonAnalysisComplete ||
        SingletonMatches.size() != SingletonProofs.size())
      return FailIncomplete();
    if (!consumeWork(SingletonMatches.size()))
      return FailIncomplete();
    std::vector<uint32_t> RequiredBitsByCandidate(Candidates.size(), 0);
    std::vector<bool> SelectedGroup(KnownOneGroups.size(), false);
    for (size_t I = 0; I < SingletonMatches.size(); ++I) {
      if (!SingletonMatches[I])
        continue;
      const SingletonKnownOneProof &Proof = SingletonProofs[I];
      if (SelectedGroup[Proof.GroupIndex])
        continue;
      SelectedGroup[Proof.GroupIndex] = true;
      const KnownOneGroup &Group = KnownOneGroups[Proof.GroupIndex];
      const size_t CandidateIndex = Group.CandidateIndex;
      RequiredBitsByCandidate[CandidateIndex] |= Group.RequiredBits;
      auto &Matched = MatchedKnownOneProducers[CandidateIndex];
      if (!ensureAppendCapacity(Matched) || !consumeWork(1))
        return FailIncomplete();
      Matched.push_back(Group.ProducerIndices[Proof.ProducerSlot]);
    }
    for (size_t I = 0; I < Candidates.size(); ++I) {
      MaskCandidate &Candidate = Candidates[I];
      const uint32_t RequiredBits = RequiredBitsByCandidate[I];
      if (RequiredBits == 0)
        continue;
      if (!consumeWorkProducts({{Candidate.Coordinates.size(), 2}}))
        return FailIncomplete();
      std::vector<uint32_t> RefinedCoordinates;
      RefinedCoordinates.reserve(Candidate.Coordinates.size());
      for (uint32_t Coordinate : Candidate.Coordinates)
        if ((Coordinate & RequiredBits) == RequiredBits)
          RefinedCoordinates.push_back(Coordinate);
      if (RefinedCoordinates.size() < limits::kMinJumpTableEntries) {
        Candidate.CompleteDomain = false;
        Candidate.Bound = 0;
        Candidate.Coordinates.clear();
        continue;
      }
      Candidate.Coordinates = std::move(RefinedCoordinates);
      Candidate.Bound = Candidate.Coordinates.back() + 1;
      Candidate.NonContiguous |=
          Candidate.Coordinates.size() != Candidate.Bound;
    }
  }

  // Keep lower-bound witnesses attached to the exact producer occurrence whose
  // domain they refined.  A dead sibling OR must not contaminate an unrelated
  // selected domain merely because both happened to match during the batched
  // scan.  Translation/intersection closure below propagates this lineage only
  // along exact proven producer edges, and the caller receives the lineage of
  // the domain that actually wins.
  struct MaskWitnessLineage {
    JumpTableValueOccurrence Output;
    std::vector<JumpTableMaskKnownOneWitness> Witnesses;
  };
  std::vector<MaskWitnessLineage> MaskWitnessLineages;
  bool WitnessLineageComplete = true;
  auto sameOccurrence = [](const JumpTableValueOccurrence &A,
                           const JumpTableValueOccurrence &B) {
    return A == B;
  };
  auto findWitnessLineage =
      [&](const JumpTableValueOccurrence &Output) -> MaskWitnessLineage * {
    if (!consumeWork(MaskWitnessLineages.size())) {
      WitnessLineageComplete = false;
      return nullptr;
    }
    auto It =
        std::find_if(MaskWitnessLineages.begin(), MaskWitnessLineages.end(),
                     [&](const MaskWitnessLineage &Lineage) {
                       return sameOccurrence(Lineage.Output, Output);
                     });
    return It == MaskWitnessLineages.end() ? nullptr : &*It;
  };
  auto appendUniqueWitness = [&](auto &Values,
                                 const JumpTableMaskKnownOneWitness &Witness) {
    if (!consumeWork(Values.size()))
      return false;
    if (std::find(Values.begin(), Values.end(), Witness) != Values.end())
      return true;
    if (!ensureAppendCapacity(Values) || !consumeWork(1))
      return false;
    Values.push_back(Witness);
    return true;
  };
  auto appendLineage = [&](const JumpTableValueOccurrence &Output,
                           const auto &Witnesses) {
    MaskWitnessLineage *Lineage = findWitnessLineage(Output);
    if (!WitnessLineageComplete)
      return false;
    if (!Lineage) {
      if (!ensureAppendCapacity(MaskWitnessLineages) || !consumeWork(1))
        return false;
      MaskWitnessLineages.push_back({Output, {}});
      Lineage = &MaskWitnessLineages.back();
    }
    for (const JumpTableMaskKnownOneWitness &Witness : Witnesses)
      if (!appendUniqueWitness(Lineage->Witnesses, Witness))
        return false;
    return true;
  };
  auto collectLineages = [&](const auto &Producers,
                             std::vector<JumpTableMaskKnownOneWitness> &Out) {
    for (const JumpTableValueOccurrence &Producer : Producers) {
      MaskWitnessLineage *Lineage = findWitnessLineage(Producer);
      if (!WitnessLineageComplete)
        return false;
      if (!Lineage)
        continue;
      for (const JumpTableMaskKnownOneWitness &Witness : Lineage->Witnesses)
        if (!appendUniqueWitness(Out, Witness))
          return false;
    }
    return true;
  };
  auto collectOneLineage = [&](const JumpTableValueOccurrence &Producer,
                               std::vector<JumpTableMaskKnownOneWitness> &Out) {
    MaskWitnessLineage *Lineage = findWitnessLineage(Producer);
    if (!WitnessLineageComplete)
      return false;
    if (!Lineage)
      return true;
    for (const JumpTableMaskKnownOneWitness &Witness : Lineage->Witnesses)
      if (!appendUniqueWitness(Out, Witness))
        return false;
    return true;
  };
  auto publishLineages = [&](const auto &Producers) {
    if (!KnownOneWitnesses)
      return true;
    std::vector<JumpTableMaskKnownOneWitness> Selected;
    if (!collectLineages(Producers, Selected))
      return false;
    *KnownOneWitnesses = std::move(Selected);
    return true;
  };
  for (size_t CandidateIndex = 0; CandidateIndex < Candidates.size();
       ++CandidateIndex) {
    const MaskCandidate &Candidate = Candidates[CandidateIndex];
    if (!Candidate.CompleteDomain ||
        MatchedKnownOneProducers[CandidateIndex].empty())
      continue;
    std::vector<JumpTableMaskKnownOneWitness> Witnesses;
    for (size_t ProducerIndex : MatchedKnownOneProducers[CandidateIndex]) {
      const KnownOneProducer &Producer = KnownOneProducers[ProducerIndex];
      if (!appendUniqueWitness(
              Witnesses,
              {Producer.Output, Candidate.Output, Producer.ConstantOperand,
               Producer.KnownOneBits}))
        return FailIncomplete();
    }
    if (!appendLineage(Candidate.Output, Witnesses))
      return FailIncomplete();
  }

  // Group authenticated mask producers by the physical bound they prove.
  // Every feasible index occurrence must resolve to a producer in the same
  // group; predecessor arms that prove different domains do not silently take
  // min/max here because that would require separate path-domain reasoning.
  std::map<uint32_t, std::vector<JumpTableValueOccurrence>> ByBound;
  std::map<uint32_t, bool> BoundUsesNonContiguous;
  std::map<uint32_t, std::set<uint32_t>> BoundCoordinates;
  auto mergeCoordinates = [&](uint32_t Bound,
                              const std::vector<uint32_t> &Coordinates) {
    auto &Known = BoundCoordinates[Bound];
    const size_t OldSize = Known.size();
    Known.insert(Coordinates.begin(), Coordinates.end());
    return Known.size() != OldSize;
  };
  for (size_t I = 0; I < Candidates.size(); ++I)
    if (ConstantsMatch[I]) {
      if (Candidates[I].CompleteDomain) {
        // Grouping retains one producer and inserts every coordinate into the
        // bound set while touching three per-bound maps.  Prepay before the
        // first map/vector mutation so exhaustion leaves no partial group.
        if (!consumeWorkProducts(
                {{Candidates[I].Coordinates.size(), 2}, {1, 3}}))
          return FailIncomplete();
        ByBound[Candidates[I].Bound].push_back(Candidates[I].Output);
        mergeCoordinates(Candidates[I].Bound, Candidates[I].Coordinates);
        BoundUsesNonContiguous[Candidates[I].Bound] |=
            Candidates[I].NonContiguous;
      }
    }

  // Most selectors consume one root mask directly (possibly through a
  // width-preserving copy/extension).  Prove that exact occurrence before
  // exploring translated-domain proposals.  Besides being the strongest
  // certificate, this keeps unrelated arithmetic in large case bodies from
  // exhausting the bounded proposal search.  This shortcut is valid only when
  // the mask's dynamic input does not itself depend on another mask occurrence:
  // nested masks must first pass through the domain-intersection closure below.
  bool DirectIndexAnalysisIncomplete = false;
  auto directIndexBound = [&]() -> std::optional<uint32_t> {
    if (IndexOccurrences.empty())
      return std::nullopt;
    for (const auto &[Bound, Producers] : ByBound) {
      const std::set<uint32_t> &Coordinates = BoundCoordinates.at(Bound);
      // Prepay the producer scan, retained query records, and the full
      // occurrence x producer alternatives product before Queries allocates.
      if (!consumeWorkProducts({{Producers.size(), 1},
                                {IndexOccurrences.size(), 1},
                                {IndexOccurrences.size(), Producers.size()}})) {
        DirectIndexAnalysisIncomplete = true;
        return std::nullopt;
      }
      const uint32_t MaxCoordinate = Coordinates.empty()
                                         ? std::numeric_limits<uint32_t>::max()
                                         : *Coordinates.rbegin();
      const bool AllowsSignExtension =
          !Producers.empty() &&
          std::all_of(Producers.begin(), Producers.end(),
                      [&](const JumpTableValueOccurrence &Producer) {
                        if (Producer.Value.Size == 0 ||
                            Producer.Value.Size > sizeof(uint64_t))
                          return false;
                        const unsigned Bits = Producer.Value.Size * 8;
                        const uint64_t SignBit = uint64_t{1} << (Bits - 1);
                        return MaxCoordinate < SignBit;
                      });
      std::vector<JumpTableValueQuery> Queries;
      Queries.reserve(IndexOccurrences.size());
      for (const JumpTableValueOccurrence &Index : IndexOccurrences)
        Queries.push_back({Index.Value, Index.Addr, Index.Seq, Producers,
                           /*AllowZeroExtension=*/true, AllowsSignExtension});
      bool AnalysisComplete = false;
      const std::vector<bool> Matches = matchAtUses(Queries, &AnalysisComplete);
      if (!AnalysisComplete || Matches.size() != Queries.size()) {
        DirectIndexAnalysisIncomplete = true;
        return std::nullopt;
      }
      if (std::all_of(Matches.begin(), Matches.end(),
                      [](bool Match) { return Match; })) {
        std::vector<JumpTableValueQuery> UpstreamMaskQueries;
        for (size_t CandidateIndex = 0; CandidateIndex < Candidates.size();
             ++CandidateIndex) {
          if (!consumeWork(1)) {
            DirectIndexAnalysisIncomplete = true;
            return std::nullopt;
          }
          const MaskCandidate &Candidate = Candidates[CandidateIndex];
          if (!ConstantsMatch[CandidateIndex] || !Candidate.CompleteDomain ||
              Candidate.Bound != Bound)
            continue;
          // This query owns a copy of every relevant root.  Charge it before
          // constructing the alternatives vector, not after the batch exists.
          if (!consumeWorkProducts(
                  {{1, 1}, {RelevantMaskRoots.size(), 1}})) {
            DirectIndexAnalysisIncomplete = true;
            return std::nullopt;
          }
          JumpTableValueQuery Query;
          Query.Candidate = Candidate.DynamicInput;
          Query.UseAddr = Candidate.Output.Addr;
          Query.UseSeq = Candidate.Output.Seq;
          Query.Alternatives = RelevantMaskRoots;
          Query.AllowZeroExtension = true;
          Query.Relation = JumpTableValueRelation::MayDepend;
          UpstreamMaskQueries.push_back(std::move(Query));
        }
        bool UpstreamMaskAnalysisComplete = false;
        const std::vector<bool> DependsOnUpstreamMask =
            matchAtUses(UpstreamMaskQueries, &UpstreamMaskAnalysisComplete);
        if (!UpstreamMaskAnalysisComplete ||
            DependsOnUpstreamMask.size() != UpstreamMaskQueries.size()) {
          DirectIndexAnalysisIncomplete = true;
          return std::nullopt;
        }
        if (std::any_of(DependsOnUpstreamMask.begin(),
                        DependsOnUpstreamMask.end(),
                        [](bool Depends) { return Depends; }))
          continue;
        if (FeasibleCoordinates && !consumeWork(Coordinates.size())) {
          DirectIndexAnalysisIncomplete = true;
          return std::nullopt;
        }
        if (UsedNonContiguous)
          *UsedNonContiguous = BoundUsesNonContiguous[Bound];
        if (FeasibleCoordinates)
          FeasibleCoordinates->assign(Coordinates.begin(), Coordinates.end());
        if (!publishLineages(Producers)) {
          DirectIndexAnalysisIncomplete = true;
          return std::nullopt;
        }
        return Bound;
      }
    }
    return std::nullopt;
  };
  if (const std::optional<uint32_t> DirectBound = directIndexBound())
    return *DirectBound;
  if (DirectIndexAnalysisIncomplete) {
    return FailIncomplete();
  }

  // A cyclic computed-goto merges an entry constant with several case-local
  // masks of the same dense domain.  Once provisional successors expose all
  // case bodies, multiplying every unrelated lexical AND by every known bound
  // can exhaust the evidence budget before the existing dense-domain replay.
  // Inside the candidate-local fixed point, probe the capacity-shaped dense
  // group before the general cross-product closure.  Every selector occurrence
  // must still be one exact producer or concrete coordinate and final replay
  // must observe multiple reaching producers.  The target override guarantees
  // that every producer considered here was made reachable by an earlier
  // authorized coordinate; provisional edges therefore cannot prove
  // themselves.  A reaching capacity-shaped producer whose dynamic input
  // depends on another mask stays in the ordinary intersection/translation
  // closure below instead of widening that stronger domain to the outer mask.
  if (AllowRawDenseShortcut && CandidateTargetsOverride &&
      ReachableInstructions) {
    struct RawDenseMaskBatch {
      uint32_t Bound = 0;
      size_t ExactBegin = 0;
      size_t ExactEnd = 0;
      bool AllowsSignExtension = false;
      std::vector<uint32_t> Coordinates;
      std::vector<JumpTableValueOccurrence> Producers;
    };
    std::vector<RawDenseMaskBatch> RawDenseBatches;
    std::vector<JumpTableValueQuery> RawDenseExactQueries;
    for (const auto &[Bound, Producers] : ByBound) {
      // Capacity only schedules the one dense proposal whose successful domain
      // proof could close this table; it is never accepted as domain evidence.
      if (Info.PhysicalCapacity == 0 || Bound != Info.PhysicalCapacity)
        continue;
      const std::set<uint32_t> &CoordinateSet = BoundCoordinates.at(Bound);
      // Scan and retain both dense-domain components, plus the batch record,
      // before the density/width checks or batch vectors allocate.
      if (!consumeWorkProducts({{CoordinateSet.size(), 2},
                                {Producers.size(), 2}, {1, 1}}))
        return FailIncomplete();
      bool IsDenseZeroBased = !BoundUsesNonContiguous[Bound] && Bound != 0 &&
                              CoordinateSet.size() == Bound;
      uint32_t Expected = 0;
      for (uint32_t Coordinate : CoordinateSet)
        IsDenseZeroBased &= Coordinate == Expected++;
      if (!IsDenseZeroBased || Producers.empty())
        continue;

      const size_t Max = std::numeric_limits<size_t>::max();
      if (CoordinateSet.size() > (Max - Producers.size()) / 2)
        return FailIncomplete();
      const size_t AlternativeCount =
          Producers.size() + CoordinateSet.size() * 2;
      if (AlternativeCount > (Max - 3) / 2)
        return FailIncomplete();
      const size_t WorkPerQuery = size_t{3} + AlternativeCount * 2;
      if (IndexOccurrences.size() > Max / WorkPerQuery)
        return FailIncomplete();
      const size_t BatchWork = IndexOccurrences.size() * WorkPerQuery;
      // Prepay the retained query/alternative batch before any vectors grow.
      if (!consumeWork(BatchWork))
        return FailIncomplete();

      const uint32_t MaxCoordinate = *CoordinateSet.rbegin();
      const bool AllowsSignExtension =
          std::all_of(Producers.begin(), Producers.end(),
                      [&](const JumpTableValueOccurrence &Producer) {
                        if (Producer.Value.Size == 0 ||
                            Producer.Value.Size > sizeof(uint64_t))
                          return false;
                        const unsigned Bits = Producer.Value.Size * 8;
                        return MaxCoordinate < (uint64_t{1} << (Bits - 1));
                      });
      RawDenseMaskBatch Batch;
      Batch.Bound = Bound;
      Batch.AllowsSignExtension = AllowsSignExtension;
      Batch.Coordinates.assign(CoordinateSet.begin(), CoordinateSet.end());
      Batch.Producers = Producers;
      Batch.ExactBegin = RawDenseExactQueries.size();
      for (const JumpTableValueOccurrence &Index : IndexOccurrences) {
        JumpTableValueQuery Exact;
        Exact.Candidate = Index.Value;
        Exact.UseAddr = Index.Addr;
        Exact.UseSeq = Index.Seq;
        Exact.Alternatives = Producers;
        for (uint32_t Coordinate : Batch.Coordinates) {
          Exact.Alternatives.push_back(
              {NdVar::cst(Coordinate, Index.Value.Size), InvalidVA, -1, false});
          Exact.Alternatives.push_back(
              {NdVar::scalar(Coordinate, Index.Value.Size), InvalidVA, -1,
               false});
        }
        Exact.AllowZeroExtension = true;
        Exact.AllowSignExtension = AllowsSignExtension;
        RawDenseExactQueries.push_back(std::move(Exact));
      }
      Batch.ExactEnd = RawDenseExactQueries.size();
      RawDenseBatches.push_back(std::move(Batch));
    }

    if (!RawDenseExactQueries.empty()) {
      bool ExactAnalysisComplete = false;
      const std::vector<bool> ExactMatches =
          matchAtUses(RawDenseExactQueries, &ExactAnalysisComplete);
      if (!ExactAnalysisComplete ||
          ExactMatches.size() != RawDenseExactQueries.size()) {
        return FailIncomplete();
      }

      for (const RawDenseMaskBatch &Batch : RawDenseBatches) {
        if (!std::all_of(ExactMatches.begin() + Batch.ExactBegin,
                         ExactMatches.begin() + Batch.ExactEnd,
                         [](bool Match) { return Match; }))
          continue;

        if (IndexOccurrences.size() >
            std::numeric_limits<size_t>::max() / Batch.Producers.size())
          return FailIncomplete();
        const size_t ReachQueryCount =
            IndexOccurrences.size() * Batch.Producers.size();
        if (ReachQueryCount > std::numeric_limits<size_t>::max() / 5 ||
            !consumeWork(ReachQueryCount * 5))
          return FailIncomplete();
        std::vector<JumpTableValueQuery> ReachQueries;
        ReachQueries.reserve(ReachQueryCount);
        for (const JumpTableValueOccurrence &Index : IndexOccurrences)
          for (const JumpTableValueOccurrence &Producer : Batch.Producers) {
            JumpTableValueQuery Reach;
            Reach.Candidate = Index.Value;
            Reach.UseAddr = Index.Addr;
            Reach.UseSeq = Index.Seq;
            Reach.Alternatives = {Producer};
            Reach.AllowZeroExtension = true;
            Reach.AllowSignExtension = Batch.AllowsSignExtension;
            Reach.Relation = JumpTableValueRelation::MayDepend;
            ReachQueries.push_back(std::move(Reach));
          }
        bool ReachAnalysisComplete = false;
        const std::vector<bool> ReachMatches =
            matchAtUses(ReachQueries, &ReachAnalysisComplete);
        if (!ReachAnalysisComplete ||
            ReachMatches.size() != ReachQueries.size())
          return FailIncomplete();
        // ReachQueryCount prepays the query/result batch; the per-producer
        // reachability accumulator is a separate retained container.
        if (!consumeWork(Batch.Producers.size()))
          return FailIncomplete();
        std::vector<bool> ProducerReaches(Batch.Producers.size(), false);
        size_t ReachResult = 0;
        for (size_t Index = 0; Index < IndexOccurrences.size(); ++Index)
          for (size_t Producer = 0; Producer < Batch.Producers.size();
               ++Producer, ++ReachResult)
            ProducerReaches[Producer] =
                ProducerReaches[Producer] || ReachMatches[ReachResult];
        const bool SawProducer =
            std::any_of(ProducerReaches.begin(), ProducerReaches.end(),
                        [](bool Reaches) { return Reaches; });
        // Exact-domain replay alone could match only the literal seed.  At
        // least one authenticated producer must actually reach the selector
        // in this candidate-local graph before it may widen the fixed point.
        // One producer is sufficient: the already-authorized coordinate that
        // exposed it is the non-circular predecessor (the common
        // literal-zero + one masked loop-back lowering).
        if (!SawProducer)
          continue;
        const size_t ReachingProducerCount =
            std::count(ProducerReaches.begin(), ProducerReaches.end(), true);

        if (!consumeWorkProducts(
                {{Batch.Producers.size(), 1},
                 {ReachingProducerCount, Candidates.size()}}))
          return FailIncomplete();
        std::vector<size_t> UpstreamCandidateIndices;
        for (size_t ProducerIndex = 0; ProducerIndex < Batch.Producers.size();
             ++ProducerIndex) {
          if (!ProducerReaches[ProducerIndex])
            continue;
          const JumpTableValueOccurrence &Producer =
              Batch.Producers[ProducerIndex];
          bool FoundCandidate = false;
          for (size_t CandidateIndex = 0; CandidateIndex < Candidates.size();
               ++CandidateIndex) {
            const MaskCandidate &Candidate = Candidates[CandidateIndex];
            if (!ConstantsMatch[CandidateIndex] || !Candidate.CompleteDomain ||
                Candidate.Bound != Batch.Bound ||
                Candidate.Output.Value.Space != Producer.Value.Space ||
                Candidate.Output.Value.Offset != Producer.Value.Offset ||
                Candidate.Output.Value.Size != Producer.Value.Size ||
                Candidate.Output.Addr != Producer.Addr ||
                Candidate.Output.Seq != Producer.Seq)
              continue;
            if (!consumeWork(1))
              return FailIncomplete();
            UpstreamCandidateIndices.push_back(CandidateIndex);
            FoundCandidate = true;
          }
          if (!FoundCandidate)
            return FailIncomplete();
        }
        const size_t UpstreamQueryCount = UpstreamCandidateIndices.size();
        if (!consumeWorkProducts(
                {{UpstreamQueryCount, 1},
                 {UpstreamQueryCount, RelevantMaskRoots.size()}}))
          return FailIncomplete();
        std::vector<JumpTableValueQuery> UpstreamQueries;
        UpstreamQueries.reserve(UpstreamQueryCount);
        for (size_t CandidateIndex : UpstreamCandidateIndices) {
          const MaskCandidate &Candidate = Candidates[CandidateIndex];
          JumpTableValueQuery Upstream;
          Upstream.Candidate = Candidate.DynamicInput;
          Upstream.UseAddr = Candidate.Output.Addr;
          Upstream.UseSeq = Candidate.Output.Seq;
          Upstream.Alternatives = RelevantMaskRoots;
          Upstream.AllowZeroExtension = true;
          Upstream.Relation = JumpTableValueRelation::MayDepend;
          UpstreamQueries.push_back(std::move(Upstream));
        }
        if (!UpstreamQueries.empty()) {
          bool UpstreamAnalysisComplete = false;
          const std::vector<bool> DependsOnUpstreamMask =
              matchAtUses(UpstreamQueries, &UpstreamAnalysisComplete);
          if (!UpstreamAnalysisComplete ||
              DependsOnUpstreamMask.size() != UpstreamQueries.size())
            return FailIncomplete();
          if (std::any_of(DependsOnUpstreamMask.begin(),
                          DependsOnUpstreamMask.end(),
                          [](bool Depends) { return Depends; })) {
            bool PreciseIncomplete = false;
            bool PreciseSemanticAmbiguous = false;
            bool PreciseNonContiguous = false;
            std::vector<uint32_t> PreciseCoordinates;
            std::vector<JumpTableMaskKnownOneWitness>
                PreciseKnownOneWitnesses;
            const uint32_t PreciseBound = inferBoundsFromMask(
                Rec, Info, AllowNonContiguous, &PreciseIncomplete,
                &PreciseNonContiguous, &PreciseCoordinates,
                &PreciseKnownOneWitnesses,
                RequireProducerReachability, CandidateTargetsOverride,
                ReachableInstructions,
                /*AllowFixedPointBootstrap=*/false,
                /*AllowRawDenseShortcut=*/false, EvidenceBudget,
                &PreciseSemanticAmbiguous);
            if (PreciseSemanticAmbiguous) {
              if (SemanticIndexDomainAmbiguous)
                *SemanticIndexDomainAmbiguous = true;
              return 0;
            }
            if (PreciseIncomplete)
              return FailIncomplete();
            // A raw capacity-shaped mask is only a scheduling shortcut.  Once
            // its dynamic input is known to depend on another mask occurrence,
            // the precise intersection/translation closure is the sole domain
            // authority.  Failure to prove that stronger domain must reject
            // this raw batch rather than widen it back to Batch.Bound.
            if (PreciseBound == 0 || PreciseCoordinates.empty())
              continue;
            if (UsedNonContiguous)
              *UsedNonContiguous = PreciseNonContiguous;
            if (FeasibleCoordinates)
              *FeasibleCoordinates = std::move(PreciseCoordinates);
            if (KnownOneWitnesses)
              *KnownOneWitnesses = std::move(PreciseKnownOneWitnesses);
            return PreciseBound;
          }
        }

        if (FeasibleCoordinates &&
            !consumeWork(Batch.Coordinates.size()))
          return FailIncomplete();
        if (UsedNonContiguous)
          *UsedNonContiguous = false;
        if (FeasibleCoordinates)
          *FeasibleCoordinates = Batch.Coordinates;
        if (!publishLineages(Batch.Producers))
          return FailIncomplete();
        return Batch.Bound;
      }
    }
  }

  // Preserve the established `(x & M) +/- C` lowering, but make both edges of
  // the transform occurrence-local.  The lexical trace merely proposes C;
  // one batch proves the constant at the arithmetic use and proves the other
  // operand comes from an authenticated mask producer.  Constants follow the
  // emitter's Coerce contract: their source-width bit pattern is zero-extended
  // or truncated to the arithmetic output width before two's-complement
  // interpretation (so i8(0xf0) in a 32-bit ADD is +240, not -16).
  struct OffsetCandidate {
    JumpTableValueOccurrence Output;
    uint32_t Bound = 0;
    std::vector<uint32_t> Coordinates;
    bool UsesNonContiguous = false;
    JumpTableValueQuery ConstantProof;
    JumpTableValueQuery InputProof;
    std::vector<JumpTableMaskKnownOneWitness> KnownOneWitnesses;
  };
  auto coercedSignedConstant =
      [&](uint64_t Raw, uint16_t SourceSize,
          uint16_t OutputSize) -> std::optional<int64_t> {
    const std::optional<uint64_t> SourceMask = integerWidthMask(SourceSize);
    const std::optional<uint64_t> OutputMask = integerWidthMask(OutputSize);
    if (!SourceMask || !OutputMask)
      return std::nullopt;
    uint64_t Bits = (Raw & *SourceMask) & *OutputMask;
    if (OutputSize < sizeof(uint64_t)) {
      const uint64_t Sign = uint64_t{1} << (OutputSize * 8 - 1);
      if (Bits & Sign)
        Bits |= ~*OutputMask;
    }
    return static_cast<int64_t>(Bits);
  };

  // Compute the transitive closure of safe constant translations.  This is a
  // finite occurrence graph: every successful step adds one exact LowOp output
  // to one bound group, and the shared work budget caps hostile cross-products.
  // Anything derived from an authenticated mask but not admitted by this
  // closure is handled by the may-depend check below, so an unknown operand,
  // arithmetic overflow, or a second/third transform can never disappear into
  // the relocation-run fallback as if no mask had existed.
  size_t SafeProducerCount = 0;
  for (const auto &[Bound, Producers] : ByBound) {
    (void)Bound;
    if (Producers.size() >
        std::numeric_limits<size_t>::max() - SafeProducerCount)
      return FailIncomplete();
    SafeProducerCount += Producers.size();
  }
  // One pass counts the producer batch and one pass retains the set nodes.
  if (!consumeWorkProducts({{SafeProducerCount, 2}}))
    return FailIncomplete();
  std::set<std::tuple<uint32_t, uint8_t, uint64_t, uint16_t, va_t, int>>
      KnownSafeOutputs;
  for (const auto &[Bound, Producers] : ByBound)
    for (const JumpTableValueOccurrence &Producer : Producers)
      KnownSafeOutputs.emplace(Bound,
                               static_cast<uint8_t>(Producer.Value.Space),
                               Producer.Value.Offset, Producer.Value.Size,
                               Producer.Addr, Producer.Seq);

  for (;;) {
    bool Added = false;

    // AND intersects bit domains.  An outer mask that consumes an already
    // authenticated inner-mask result is bounded by the tighter of the two.
    // Prove the dynamic input at this exact AND occurrence before carrying the
    // intersection to its output.  Otherwise `x & 1; x & 7` would match the
    // outer producer and publish eight slots instead of the complete two-slot
    // domain.
    struct AndIntersectionCandidate {
      JumpTableValueOccurrence Output;
      uint32_t Bound = 0;
      std::vector<uint32_t> Coordinates;
      bool UsesNonContiguous = false;
      JumpTableValueQuery InputProof;
      std::vector<JumpTableMaskKnownOneWitness> KnownOneWitnesses;
    };
    std::vector<AndIntersectionCandidate> AndCandidates;
    for (size_t CandidateIndex = 0; CandidateIndex < Candidates.size();
         ++CandidateIndex) {
      const MaskCandidate &Candidate = Candidates[CandidateIndex];
      if (!ConstantsMatch[CandidateIndex] || !Candidate.CompleteDomain)
        continue;
      for (const auto &[InputBound, Producers] : ByBound) {
        if (!consumeWork(1))
          return FailIncomplete();
        const std::set<uint32_t> &InputCoordinates =
            BoundCoordinates.at(InputBound);
        // The transformed coordinate walk can retain at most one element per
        // input coordinate.  AND is not monotone, so sorting that vector adds
        // ceil(log2 N) comparison levels plus the final unique scan.  Pay the
        // full bounded sort/walk/retention cost and producer copy first.
        const size_t SortLevels =
            InputCoordinates.size() > 1
                ? llvm::Log2_64_Ceil(
                      static_cast<uint64_t>(InputCoordinates.size()))
                : 0;
        if (!consumeWorkProducts({{InputCoordinates.size(), SortLevels + 3},
                                  {Producers.size(), 1}, {1, 1}}))
          return FailIncomplete();
        std::vector<uint32_t> OutputCoordinates;
        OutputCoordinates.reserve(InputCoordinates.size());
        for (uint32_t Value : InputCoordinates)
          OutputCoordinates.push_back(Value & Candidate.Mask);
        std::sort(OutputCoordinates.begin(), OutputCoordinates.end());
        OutputCoordinates.erase(
            std::unique(OutputCoordinates.begin(), OutputCoordinates.end()),
            OutputCoordinates.end());
        if (OutputCoordinates.empty())
          continue;
        const uint32_t OutputBound = OutputCoordinates.back() + 1;
        if (KnownSafeOutputs.count(
                {OutputBound,
                 static_cast<uint8_t>(Candidate.Output.Value.Space),
                 Candidate.Output.Value.Offset, Candidate.Output.Value.Size,
                 Candidate.Output.Addr, Candidate.Output.Seq}))
          continue;
        JumpTableValueQuery InputProof;
        InputProof.Candidate = Candidate.DynamicInput;
        InputProof.UseAddr = Candidate.Output.Addr;
        InputProof.UseSeq = Candidate.Output.Seq;
        InputProof.Alternatives = Producers;
        InputProof.AllowZeroExtension = true;
        const bool UsesNonContiguous =
            Candidate.NonContiguous ||
            OutputCoordinates.size() != OutputBound ||
            BoundUsesNonContiguous[InputBound];
        std::vector<JumpTableMaskKnownOneWitness> KnownOneLineage;
        if (!collectLineages(Producers, KnownOneLineage) ||
            !collectOneLineage(Candidate.Output, KnownOneLineage))
          return FailIncomplete();
        AndCandidates.push_back(
            {Candidate.Output, OutputBound, std::move(OutputCoordinates),
             UsesNonContiguous, std::move(InputProof),
             std::move(KnownOneLineage)});
      }
    }
    if (!AndCandidates.empty()) {
      std::vector<JumpTableValueQuery> AndQueries;
      if (!consumeWork(AndCandidates.size()))
        return FailIncomplete();
      AndQueries.reserve(AndCandidates.size());
      for (AndIntersectionCandidate &Candidate : AndCandidates)
        AndQueries.push_back(std::move(Candidate.InputProof));
      bool AndAnalysisComplete = false;
      const std::vector<bool> AndMatches =
          matchAtUses(AndQueries, &AndAnalysisComplete);
      if (!AndAnalysisComplete || AndMatches.size() != AndQueries.size())
        return FailIncomplete();
      for (size_t I = 0; I < AndCandidates.size(); ++I) {
        if (!AndMatches[I])
          continue;
        const AndIntersectionCandidate &Candidate = AndCandidates[I];
        if (!consumeWorkProducts(
                {{Candidate.Coordinates.size(), 2}, {1, 3}}))
          return FailIncomplete();
        const bool NewOutput =
            KnownSafeOutputs
                .emplace(Candidate.Bound,
                         static_cast<uint8_t>(Candidate.Output.Value.Space),
                         Candidate.Output.Value.Offset,
                         Candidate.Output.Value.Size, Candidate.Output.Addr,
                         Candidate.Output.Seq)
                .second;
        const bool DomainChanged =
            mergeCoordinates(Candidate.Bound, Candidate.Coordinates);
        if (NewOutput) {
          ByBound[Candidate.Bound].push_back(Candidate.Output);
          if (!appendLineage(Candidate.Output,
                             Candidate.KnownOneWitnesses))
            return FailIncomplete();
        }
        BoundUsesNonContiguous[Candidate.Bound] |= Candidate.UsesNonContiguous;
        Added |= NewOutput || DomainChanged;
      }
    }

    std::vector<OffsetCandidate> OffsetCandidates;
    for (int I = 0; I < static_cast<int>(Ops.size()); ++I) {
      if (!consumeWork(1))
        return FailIncomplete();
      const LowOp &Op = Ops[I];
      if ((Op.Opcode != NdOp::INT_ADD && Op.Opcode != NdOp::INT_SUB) ||
          Op.NumInputs < 2 || Op.Output.Size == 0)
        continue;
      const auto InsnIt = Insns.find(Op.Addr);
      if (InsnIt == Insns.end() || InsnIt->second.IsInstructionGuard ||
          conditionalMergeOutput(Op, InsnIt->second))
        continue;

      int ConstantSide = -1;
      ConstantLookupResult Proposed;
      if (Op.Opcode == NdOp::INT_ADD) {
        Proposed = constValueOf(I - 1, Op.Inputs[1], 0);
        if (Proposed.Kind == ConstantLookupKind::Exhausted)
          return FailIncomplete();
        if (Proposed.Kind == ConstantLookupKind::Value) {
          ConstantSide = 1;
        } else {
          Proposed = constValueOf(I - 1, Op.Inputs[0], 0);
          if (Proposed.Kind == ConstantLookupKind::Exhausted)
            return FailIncomplete();
          if (Proposed.Kind == ConstantLookupKind::Value)
            ConstantSide = 0;
        }
      } else {
        // Only `dynamic - constant` preserves one contiguous translated
        // domain.  A constant-minus-dynamic expression remains visible to the
        // may-depend proof and therefore fails closed below.
        Proposed = constValueOf(I - 1, Op.Inputs[1], 0);
        if (Proposed.Kind == ConstantLookupKind::Exhausted)
          return FailIncomplete();
        if (Proposed.Kind == ConstantLookupKind::Value)
          ConstantSide = 1;
      }
      if (ConstantSide < 0)
        continue;
      const int DynamicSide = 1 - ConstantSide;
      const NdVar &ConstantOperand = Op.Inputs[ConstantSide];
      const std::optional<uint64_t> ConstantMask =
          integerWidthMask(ConstantOperand.Size);
      if (!ConstantMask)
        continue;
      std::optional<int64_t> Delta =
          coercedSignedConstant(static_cast<uint64_t>(Proposed.Value),
                                ConstantOperand.Size, Op.Output.Size);
      if (!Delta)
        continue;
      if (Op.Opcode == NdOp::INT_SUB) {
        if (*Delta == std::numeric_limits<int64_t>::min())
          continue;
        *Delta = -*Delta;
      }

      for (const auto &[InputBound, Producers] : ByBound) {
        if (!consumeWork(1))
          return FailIncomplete();
        const std::optional<uint64_t> OutputMask =
            integerWidthMask(Op.Output.Size);
        if (!OutputMask)
          continue;
        const std::set<uint32_t> &InputCoordinates =
            BoundCoordinates.at(InputBound);
        // Prepay the coordinate walk and its worst-case retained output, the
        // copied producer alternatives, the scalar proof alternative, and the
        // retained candidate before allocating any of them.
        if (!consumeWorkProducts({{InputCoordinates.size(), 2},
                                  {Producers.size(), 1}, {1, 2}}))
          return FailIncomplete();
        std::vector<uint32_t> OutputCoordinates;
        OutputCoordinates.reserve(InputCoordinates.size());
        bool DomainComplete = true;
        for (uint32_t Value : InputCoordinates) {
          uint64_t Result = 0;
          if (*Delta < 0) {
            const uint64_t Magnitude =
                static_cast<uint64_t>(-(*Delta + 1)) + 1;
            if (Value < Magnitude) {
              DomainComplete = false;
              break;
            }
            Result = Value - Magnitude;
          } else {
            const uint64_t Magnitude = static_cast<uint64_t>(*Delta);
            if (Magnitude > *OutputMask - Value) {
              DomainComplete = false;
              break;
            }
            Result = Value + Magnitude;
          }
          if (Result >= limits::kMaxJumpTableEntries) {
            DomainComplete = false;
            break;
          }
          OutputCoordinates.push_back(static_cast<uint32_t>(Result));
        }
        if (!DomainComplete ||
            OutputCoordinates.size() < limits::kMinJumpTableEntries)
          continue;
        // InputCoordinates is an ordered unique set and adding one fixed,
        // non-negative delta is strictly monotone after the overflow checks.
        const uint32_t OutputBound = OutputCoordinates.back() + 1;
        if (KnownSafeOutputs.count(
                {OutputBound, static_cast<uint8_t>(Op.Output.Space),
                 Op.Output.Offset, Op.Output.Size, Op.Addr, Op.Seq}))
          continue;

        JumpTableValueQuery ConstantProof;
        ConstantProof.Candidate = ConstantOperand;
        ConstantProof.UseAddr = Op.Addr;
        ConstantProof.UseSeq = Op.Seq;
        ConstantProof.Alternatives.push_back(
            {NdVar::scalar(static_cast<uint64_t>(Proposed.Value) &
                               *ConstantMask,
                           ConstantOperand.Size),
             InvalidVA, -1, false});
        JumpTableValueQuery InputProof;
        InputProof.Candidate = Op.Inputs[DynamicSide];
        InputProof.UseAddr = Op.Addr;
        InputProof.UseSeq = Op.Seq;
        InputProof.Alternatives = Producers;
        InputProof.AllowZeroExtension = true;
        std::vector<JumpTableMaskKnownOneWitness> KnownOneLineage;
        if (!collectLineages(Producers, KnownOneLineage))
          return FailIncomplete();
        OffsetCandidates.push_back(
            {{Op.Output, Op.Addr, Op.Seq, /*DefinedAtPoint=*/true},
             OutputBound,
             std::move(OutputCoordinates),
             BoundUsesNonContiguous[InputBound],
             std::move(ConstantProof),
             std::move(InputProof),
             std::move(KnownOneLineage)});
      }
    }

    if (!OffsetCandidates.empty()) {
      std::vector<JumpTableValueQuery> OffsetQueries;
      if (!consumeWorkProducts({{OffsetCandidates.size(), 2}}))
        return FailIncomplete();
      if (OffsetCandidates.size() >
          std::numeric_limits<size_t>::max() / 2)
        return FailIncomplete();
      OffsetQueries.reserve(OffsetCandidates.size() * 2);
      for (OffsetCandidate &Candidate : OffsetCandidates) {
        OffsetQueries.push_back(std::move(Candidate.ConstantProof));
        OffsetQueries.push_back(std::move(Candidate.InputProof));
      }
      bool OffsetAnalysisComplete = false;
      const std::vector<bool> OffsetMatches =
          matchAtUses(OffsetQueries, &OffsetAnalysisComplete);
      if (!OffsetAnalysisComplete ||
          OffsetMatches.size() != OffsetQueries.size())
        return FailIncomplete();
      for (size_t I = 0; I < OffsetCandidates.size(); ++I)
        if (OffsetMatches[I * 2] && OffsetMatches[I * 2 + 1]) {
          const OffsetCandidate &Candidate = OffsetCandidates[I];
          if (!consumeWorkProducts(
                  {{Candidate.Coordinates.size(), 2}, {1, 3}}))
            return FailIncomplete();
          const bool NewOutput =
              KnownSafeOutputs
                  .emplace(Candidate.Bound,
                           static_cast<uint8_t>(Candidate.Output.Value.Space),
                           Candidate.Output.Value.Offset,
                           Candidate.Output.Value.Size, Candidate.Output.Addr,
                           Candidate.Output.Seq)
                  .second;
          const bool DomainChanged =
              mergeCoordinates(Candidate.Bound, Candidate.Coordinates);
          if (NewOutput) {
            ByBound[Candidate.Bound].push_back(Candidate.Output);
            if (!appendLineage(Candidate.Output,
                               Candidate.KnownOneWitnesses))
              return FailIncomplete();
          }
          BoundUsesNonContiguous[Candidate.Bound] |=
              Candidate.UsesNonContiguous;
          Added |= NewOutput || DomainChanged;
        }
    }
    if (!Added)
      break;
  }

  std::vector<JumpTableValueQuery> IndexQueries;
  struct BoundBatch {
    uint32_t Bound = 0;
    size_t Begin = 0;
    size_t End = 0;
    bool UsesNonContiguous = false;
    bool AllowsSignExtension = false;
    std::vector<uint32_t> Coordinates;
  };
  std::vector<BoundBatch> Batches;
  for (const auto &[Bound, Producers] : ByBound) {
    const std::set<uint32_t> &Coordinates = BoundCoordinates.at(Bound);
    // Retain one query and one full producer-alternatives copy per index, plus
    // the batch's coordinate copy.  The producer width scan is independent
    // loop work.  All of it is charged before IndexQueries or Batches grows.
    if (!consumeWorkProducts({{Producers.size(), 1},
                              {IndexOccurrences.size(), 1},
                              {IndexOccurrences.size(), Producers.size()},
                              {1, 1}, {Coordinates.size(), 1}}))
      return FailIncomplete();
    const size_t Begin = IndexQueries.size();
    const uint32_t MaxCoordinate = Coordinates.empty()
                                       ? std::numeric_limits<uint32_t>::max()
                                       : *Coordinates.rbegin();
    const bool AllowsSignExtension =
        !Producers.empty() &&
        std::all_of(Producers.begin(), Producers.end(),
                    [&](const JumpTableValueOccurrence &Producer) {
                      if (Producer.Value.Size == 0 ||
                          Producer.Value.Size > sizeof(uint64_t))
                        return false;
                      const unsigned Bits = Producer.Value.Size * 8;
                      const uint64_t SignBit = uint64_t{1} << (Bits - 1);
                      return MaxCoordinate < SignBit;
                    });
    for (const JumpTableValueOccurrence &Index : IndexOccurrences)
      IndexQueries.push_back({Index.Value, Index.Addr, Index.Seq, Producers,
                              /*AllowZeroExtension=*/true,
                              AllowsSignExtension});
    Batches.push_back(
        {Bound,
         Begin,
         IndexQueries.size(),
         BoundUsesNonContiguous[Bound],
         AllowsSignExtension,
         {Coordinates.begin(), Coordinates.end()}});
  }
  bool IndexAnalysisComplete = false;
  const std::vector<bool> Matches =
      matchAtUses(IndexQueries, &IndexAnalysisComplete);
  if (!IndexQueries.empty() && !IndexAnalysisComplete)
    return FailIncomplete();
  if (Matches.size() != IndexQueries.size())
    return FailIncomplete();
  for (const BoundBatch &Batch : Batches)
    if (std::all_of(Matches.begin() + Batch.Begin, Matches.begin() + Batch.End,
                    [](bool Match) { return Match; })) {
      if (FeasibleCoordinates && !consumeWork(Batch.Coordinates.size()))
        return FailIncomplete();
      if (UsedNonContiguous)
        *UsedNonContiguous = Batch.UsesNonContiguous;
      if (FeasibleCoordinates)
        *FeasibleCoordinates = Batch.Coordinates;
      const auto Producers = ByBound.find(Batch.Bound);
      if (Producers == ByBound.end() ||
          !publishLineages(Producers->second))
        return FailIncomplete();
      return Batch.Bound;
    }

  // A shared dispatch can have an initial constant selector and masked loop
  // backedges.  The merged value is then not equal to the mask producer on
  // every predecessor even though its complete domain is still [0, Bound).
  // Admit that shape only for an exact dense zero-based coordinate set.  Every
  // actual selector occurrence must equal either a producer in this exact
  // authenticated mask group or one of the group's constant coordinates.
  // This admits a constant entry arm during provisional target discovery but
  // does not reduce the proof to a numeric range: an unrelated mask elsewhere
  // in the function (for example a case-body variable shift) cannot donate its
  // bound merely because the real selector happens to fit below it.  Final
  // replay additionally requires at least one producer-dependent reaching
  // value, so a constants-only dispatch cannot borrow an unreachable lexical
  // mask.  Sparse masks and translated domains keep their stronger occurrence
  // relation; physical table capacity is never a domain certificate.
  struct DenseBoundBatch {
    const BoundBatch *Batch = nullptr;
    size_t Begin = 0;
    size_t End = 0;
  };
  std::vector<JumpTableValueQuery> DenseBoundQueries;
  std::vector<DenseBoundBatch> DenseBoundBatches;
  for (const BoundBatch &Batch : Batches) {
    // Dense fallback may fill a missing proof, tighten an authenticated one,
    // or replay the same certificate.  It must never widen a complete guard or
    // modulo domain: incidental masks in division/flag arithmetic can prove a
    // looser numeric upper bound without defining the selector coordinates.
    const bool HasIndependentDomain =
        Info.AuthenticatedGuardBound != 0 || Info.AuthenticatedModuloBound != 0;
    if (HasIndependentDomain && Info.MaxEntries != 0 &&
        Batch.Bound > Info.MaxEntries)
      continue;
    bool IsDenseZeroBased = !Batch.UsesNonContiguous && Batch.Bound != 0 &&
                            Batch.Coordinates.size() == Batch.Bound;
    for (size_t I = 0; IsDenseZeroBased && I < Batch.Coordinates.size(); ++I)
      IsDenseZeroBased = Batch.Coordinates[I] == I;
    if (!IsDenseZeroBased)
      continue;
    const size_t ProducerCount = ByBound[Batch.Bound].size();
    if (Batch.Coordinates.size() >
        (std::numeric_limits<size_t>::max() - ProducerCount - 1) / 2)
      return FailIncomplete();
    size_t WorkPerOccurrence =
        size_t{1} + ProducerCount + Batch.Coordinates.size() * 2;
    if (RequireProducerReachability) {
      if (WorkPerOccurrence >
          std::numeric_limits<size_t>::max() - ProducerCount - 1)
        return FailIncomplete();
      WorkPerOccurrence += ProducerCount + 1;
    }
    if (WorkPerOccurrence == 0 ||
        IndexOccurrences.size() >
            std::numeric_limits<size_t>::max() / WorkPerOccurrence ||
        !consumeWork(IndexOccurrences.size() * WorkPerOccurrence))
      return FailIncomplete();
    const size_t Begin = DenseBoundQueries.size();
    for (const JumpTableValueOccurrence &Index : IndexOccurrences) {
      JumpTableValueQuery ExactDomainQuery;
      ExactDomainQuery.Candidate = Index.Value;
      ExactDomainQuery.UseAddr = Index.Addr;
      ExactDomainQuery.UseSeq = Index.Seq;
      ExactDomainQuery.Alternatives = ByBound[Batch.Bound];
      for (uint32_t Coordinate : Batch.Coordinates) {
        // LowIR emitted before occurrence-role classification can still carry
        // Unknown provenance for an integer immediate, while propagated
        // arithmetic carries Scalar.  They are the same selector bit-pattern,
        // but neither is interchangeable with an address-provenance constant.
        ExactDomainQuery.Alternatives.push_back(
            {NdVar::cst(Coordinate, Index.Value.Size), InvalidVA, -1,
             /*DefinedAtPoint=*/false});
        ExactDomainQuery.Alternatives.push_back(
            {NdVar::scalar(Coordinate, Index.Value.Size), InvalidVA, -1,
             /*DefinedAtPoint=*/false});
      }
      ExactDomainQuery.AllowZeroExtension = true;
      ExactDomainQuery.AllowSignExtension = Batch.AllowsSignExtension;
      DenseBoundQueries.push_back(std::move(ExactDomainQuery));

      if (RequireProducerReachability) {
        JumpTableValueQuery DependencyQuery;
        DependencyQuery.Candidate = Index.Value;
        DependencyQuery.UseAddr = Index.Addr;
        DependencyQuery.UseSeq = Index.Seq;
        DependencyQuery.Alternatives = ByBound[Batch.Bound];
        DependencyQuery.AllowZeroExtension = true;
        DependencyQuery.AllowSignExtension = Batch.AllowsSignExtension;
        DependencyQuery.Relation = JumpTableValueRelation::MayDepend;
        DenseBoundQueries.push_back(std::move(DependencyQuery));
      }
    }
    DenseBoundBatches.push_back({&Batch, Begin, DenseBoundQueries.size()});
  }
  if (!DenseBoundQueries.empty()) {
    bool DenseBoundAnalysisComplete = false;
    const std::vector<bool> DenseBoundMatches =
        matchAtUses(DenseBoundQueries, &DenseBoundAnalysisComplete);
    if (!DenseBoundAnalysisComplete ||
        DenseBoundMatches.size() != DenseBoundQueries.size())
      return FailIncomplete();
    for (const DenseBoundBatch &Proof : DenseBoundBatches) {
      const size_t QueryStride = RequireProducerReachability ? 2 : 1;
      bool Proven = true;
      bool SawProducer = false;
      for (size_t I = Proof.Begin; Proven && I < Proof.End; I += QueryStride) {
        Proven = DenseBoundMatches[I];
        if (RequireProducerReachability)
          SawProducer |= DenseBoundMatches[I + 1];
      }
      Proven &= !RequireProducerReachability || SawProducer;
      if (Proven) {
        if (FeasibleCoordinates &&
            !consumeWork(Proof.Batch->Coordinates.size()))
          return FailIncomplete();
        if (UsedNonContiguous)
          *UsedNonContiguous = false;
        if (FeasibleCoordinates)
          *FeasibleCoordinates = Proof.Batch->Coordinates;
        const auto Producers = ByBound.find(Proof.Batch->Bound);
        if (Producers == ByBound.end() ||
            !publishLineages(Producers->second))
          return FailIncomplete();
        return Proof.Batch->Bound;
      }
    }
  }

  // No complete producer expression matched the actual table index.  Before
  // allowing relocation/object bounds to take over, ask the CFG resolver
  // whether the real index may nevertheless depend on any authenticated mask
  // occurrence.  This catches every unmodelled transform uniformly: variable
  // operands, predicated SELECTs, width wrap, INT64_MIN negation, and arbitrary
  // multi-op chains.  A proven dependency without a complete transform-domain
  // certificate is an incomplete index domain, never "no mask evidence".
  if (!RelevantMaskRoots.empty()) {
    // Every dependency query owns the complete root set.  Pay the retained
    // occurrence x root product before reserve/construction.
    if (!consumeWorkProducts({{IndexOccurrences.size(), 1},
                              {IndexOccurrences.size(),
                               RelevantMaskRoots.size()}}))
      return FailIncomplete();
    std::vector<JumpTableValueQuery> DependencyQueries;
    DependencyQueries.reserve(IndexOccurrences.size());
    for (const JumpTableValueOccurrence &Index : IndexOccurrences) {
      JumpTableValueQuery Query{Index.Value,
                                Index.Addr,
                                Index.Seq,
                                RelevantMaskRoots,
                                /*AllowZeroExtension=*/true,
                                /*AllowSignExtension=*/false};
      Query.Relation = JumpTableValueRelation::MayDepend;
      DependencyQueries.push_back(std::move(Query));
    }
    bool DependencyAnalysisComplete = false;
    const std::vector<bool> Depends =
        matchAtUses(DependencyQueries, &DependencyAnalysisComplete);
    if (!DependencyAnalysisComplete ||
        Depends.size() != DependencyQueries.size())
      return FailIncomplete();
    if (std::any_of(Depends.begin(), Depends.end(),
                    [](bool Match) { return Match; })) {
      // The graph query completed and proved that the selector depends on an
      // authenticated mask, but no exact transform-domain theorem matched.
      // This is a semantic rejection, not resource/graph incompleteness: a
      // separately authenticated all-callable physical object may still be a
      // valid tail callback, while local/unknown targets remain opaque.
      if (SemanticIndexDomainAmbiguous)
        *SemanticIndexDomainAmbiguous = true;
      return 0;
    }
  }
  return 0;
}

/// A modulo switch (`switch(x % N)`, N not a power of two) carries no `cmp`
/// range guard — the remainder is already in [0, N) — so the entry count must
/// come from the modulus N itself.  clang computes `x % N` as
/// `idx = x - (x / N) * N` with a magic-reciprocal division for the quotient
/// and a shift/add/sub tree for the `* N` back-multiply.  This recovers N by
/// decomposing that back-multiply, which bounds tables that carry no entry
/// relocations (AArch64 byte/halfword compact tables, ARM32 inline `.text` word
/// tables) and so cannot use the relocation-run count (#403).  Returns true and
/// sets Info.MaxEntries when a magic-division remainder yields a sane modulus.
bool CFGBuilder::inferBoundsFromModulo(const BinaryImage &Img,
                                       const InsnRecord &Rec,
                                       JumpTableInfo &Info,
                                       size_t *AggregateEvidenceBudget,
                                       bool *EvidenceIncomplete,
                                       bool RequireProducerReachability,
                                       const std::vector<va_t>
                                           *CandidateTargetsOverride,
                                       const std::set<va_t>
                                           *ReachableInstructions,
                                       bool AllowFixedPointBootstrap,
                                       std::vector<JumpTableValueOccurrence>
                                           *AuthenticatedProducers,
                                       uint32_t RequiredProducerBound,
                                       const std::vector<
                                           JumpTableValueOccurrence>
                                           *RequiredProducers,
                                       bool RestrictProducerDiscovery) {
  if (EvidenceIncomplete)
    *EvidenceIncomplete = false;
  if (AuthenticatedProducers)
    AuthenticatedProducers->clear();
  if (!Info.HasBaseAddr || Info.EntrySize == 0)
    return false;
  // The exact occurrence of a pre-scaled selector is a byte coordinate.  An
  // unsigned upper bound alone does not prove slot alignment; those tables are
  // authorized only by the exact mask-coordinate path, which checks both the
  // physical byte limit and coord % EntryStride == 0.
  if (Info.PreScaledIndex)
    return false;

  // A local computed-goto can be circular even though every individual fact is
  // exact: the entry selector is a literal coordinate, its destination computes
  // `x % N`, and only that newly exposed remainder authorizes the other table
  // coordinates.  Neither the physical relocation run nor a graph containing
  // every physical target may break that cycle.  The former is capacity rather
  // than a runtime domain, while the latter lets an unreachable case donate the
  // producer that makes its own edge reachable.  Compute the least
  // candidate-local fixed point instead.  Every round resolves values in a
  // graph containing only coordinates authorized by an earlier round; targets
  // are published only after the complete domain is stable under replay.
  size_t OwnedAggregateEvidenceBudget =
      limits::kMaxJumpTableMaskFixedPointEvidenceWork;
  if (!AggregateEvidenceBudget)
    AggregateEvidenceBudget = &OwnedAggregateEvidenceBudget;
  auto consumeFixedPointEvidence = [&](size_t Amount = 1) {
    if (Amount > *AggregateEvidenceBudget) {
      *AggregateEvidenceBudget = 0;
      if (EvidenceIncomplete)
        *EvidenceIncomplete = true;
      return false;
    }
    *AggregateEvidenceBudget -= Amount;
    return true;
  };
  auto consumeFixedPointProducts =
      [&](std::initializer_list<std::pair<size_t, size_t>> Products) {
        const size_t Max = std::numeric_limits<size_t>::max();
        size_t Total = 0;
        for (const auto &[Count, Cost] : Products) {
          if (Count != 0 && Cost > Max / Count)
            return consumeFixedPointEvidence(Max);
          const size_t Product = Count * Cost;
          if (Product > Max - Total)
            return consumeFixedPointEvidence(Max);
          Total += Product;
        }
        return consumeFixedPointEvidence(Total);
      };

  const bool TopLevelFixedPointAttempt =
      AllowFixedPointBootstrap && !CandidateTargetsOverride &&
      !ReachableInstructions;
  if (TopLevelFixedPointAttempt && CurrentImg &&
      Info.PhysicalCapacity >= limits::kMinJumpTableEntries &&
      Info.PhysicalCapacity <= 64) {
    const uint64_t PhysicalStride =
        Info.EntryStride != 0 ? Info.EntryStride : Info.EntrySize;
    std::optional<uint64_t> AddressScale;
    bool FixedPointEligible = PhysicalStride != 0;
    for (const JumpTableLoadRole &Role : Info.LoadRoles) {
      if (!consumeFixedPointProducts({{1, 1}, {Role.AllowedBases.size(), 1}}))
        return false;
      if (Role.LoadWidth != Info.EntrySize || Role.AddressScale == 0 ||
          std::find(Role.AllowedBases.begin(), Role.AllowedBases.end(),
                    Info.BaseAddr) == Role.AllowedBases.end())
        continue;
      if (AddressScale && *AddressScale != Role.AddressScale) {
        FixedPointEligible = false;
        break;
      }
      AddressScale = Role.AddressScale;
    }
    FixedPointEligible &= AddressScale && *AddressScale == PhysicalStride;
    if (FixedPointEligible) {
      JumpTableInfo Physical = Info;
      Physical.RuntimeSlotIndices.clear();
      Physical.RuntimeCaseLabels.clear();
      Physical.AuthenticatedMaskCoordinates.clear();
      Physical.AuthenticatedMaskKnownOneWitnesses.clear();
      std::vector<va_t> PhysicalTargets;
      std::vector<uint32_t> PhysicalSlots;
      uint32_t Lower = limits::kMinJumpTableEntries;
      uint32_t Upper = Info.PhysicalCapacity;
      while (Lower <= Upper) {
        const uint32_t Count = Lower + (Upper - Lower) / 2;
        if (!consumeFixedPointEvidence(size_t(Count) * 3))
          return false;
        Physical.MaxEntries = Count;
        std::vector<uint32_t> Slots;
        std::vector<va_t> Targets =
            readTableEntries(*CurrentImg, Physical, &Slots);
        if (Targets.size() == Count) {
          PhysicalTargets = std::move(Targets);
          PhysicalSlots = std::move(Slots);
          Lower = Count + 1;
        } else {
          if (Count == 0)
            break;
          Upper = Count - 1;
        }
      }

      if (PhysicalTargets.size() >= limits::kMinJumpTableEntries) {
        const uint32_t CandidateCapacity =
            static_cast<uint32_t>(PhysicalTargets.size());
        if (PhysicalSlots.empty()) {
          PhysicalSlots.resize(PhysicalTargets.size());
          for (uint32_t I = 0; I < PhysicalSlots.size(); ++I)
            PhysicalSlots[I] = I;
        }
        const bool ExactDenseSlots =
            PhysicalSlots.size() == PhysicalTargets.size() &&
            std::all_of(PhysicalSlots.begin(), PhysicalSlots.end(),
                        [&](uint32_t Slot) {
                          return Slot < CandidateCapacity;
                        }) &&
            std::set<uint32_t>(PhysicalSlots.begin(), PhysicalSlots.end())
                    .size() == PhysicalSlots.size();
        if (ExactDenseSlots) {
          std::vector<JumpTableValueOccurrence> SeedOccurrences =
              Info.IndexValueAlternatives;
          if (SeedOccurrences.empty() && Info.IndexValueAtUse.Size != 0 &&
              Info.IndexUseAddr != InvalidVA && Info.IndexUseSeq >= 0)
            SeedOccurrences.push_back(
                {Info.IndexValueAtUse, Info.IndexUseAddr, Info.IndexUseSeq,
                 Info.IndexValueDefinedAtUse});
          const bool ValidSeedOccurrences =
              !SeedOccurrences.empty() &&
              std::none_of(SeedOccurrences.begin(), SeedOccurrences.end(),
                           [](const JumpTableValueOccurrence &Occurrence) {
                             return Occurrence.DefinedAtPoint ||
                                    Occurrence.Value.Size == 0 ||
                                    Occurrence.Addr == InvalidVA ||
                                    Occurrence.Seq < 0;
                           });
          if (ValidSeedOccurrences) {
            auto targetsFor = [&](const std::set<uint32_t> &Coordinates)
                -> std::optional<std::vector<va_t>> {
              if (!consumeFixedPointProducts({{PhysicalTargets.size(), 1},
                                              {Coordinates.size(), 1}}))
                return std::nullopt;
              std::vector<va_t> Targets;
              Targets.reserve(Coordinates.size());
              for (size_t I = 0; I < PhysicalTargets.size(); ++I)
                if (Coordinates.count(PhysicalSlots[I]))
                  Targets.push_back(PhysicalTargets[I]);
              return Targets;
            };
            auto queueGraphGrowth =
                [&](const std::vector<va_t> &Targets)
                -> std::optional<bool> {
              if (!consumeFixedPointProducts(
                      {{Targets.size(),
                        orderedEvidenceLookupWork(ExploredAddrs.size())},
                       {Targets.size(),
                        orderedEvidenceLookupWork(BlockStarts.size())},
                       {Targets.size(),
                        orderedEvidenceLookupWork(Insns.size())},
                       {Targets.size(), 1}}))
                return std::nullopt;
              const bool UndecodedAttempt =
                  std::any_of(Targets.begin(), Targets.end(), [&](va_t Target) {
                    return ExploredAddrs.count(Target) &&
                           BlockStarts.count(Target) && !Insns.count(Target);
                  });
              if (UndecodedAttempt) {
                if (EvidenceIncomplete)
                  *EvidenceIncomplete = true;
                return std::nullopt;
              }
              const bool NeedsGraphGrowth =
                  std::any_of(Targets.begin(), Targets.end(), [&](va_t Target) {
                    return !ExploredAddrs.count(Target) ||
                           !BlockStarts.count(Target);
                  });
              if (!NeedsGraphGrowth)
                return false;
              if (!consumeFixedPointProducts(
                      {{1, orderedEvidenceLookupWork(
                               CandidateFixedPointExplorationTargets.size())}}))
                return std::nullopt;
              auto Existing =
                  CandidateFixedPointExplorationTargets.find(Rec.Addr);
              const size_t ExistingTargets =
                  Existing == CandidateFixedPointExplorationTargets.end()
                      ? 0
                      : Existing->second.size();
              if (Targets.size() >
                      std::numeric_limits<size_t>::max() - ExistingTargets ||
                  !consumeFixedPointProducts(
                      {{1, 2}, {ExistingTargets + Targets.size(), 2}}))
                return std::nullopt;
              if (Existing == CandidateFixedPointExplorationTargets.end())
                CandidateFixedPointExplorationTargets.emplace(Rec.Addr,
                                                               Targets);
              else
                Existing->second.insert(Existing->second.end(),
                                        Targets.begin(), Targets.end());
              return true;
            };

            const std::vector<va_t> NoTargets;
            const std::set<va_t> &Roots =
                ActiveJumpTableProofRoots ? *ActiveJumpTableProofRoots
                                          : PersistentCFGRoots;
            std::set<uint32_t> Authorized;
            const size_t Capacity = CandidateCapacity;
            if (SeedOccurrences.size() >
                std::numeric_limits<size_t>::max() / Capacity)
              return false;
            const size_t SeedQueryCount =
                Capacity * SeedOccurrences.size();
            constexpr size_t SeedWorkPerQuery = 7;
            if (SeedQueryCount >
                    std::numeric_limits<size_t>::max() / SeedWorkPerQuery ||
                !consumeFixedPointEvidence(SeedQueryCount * SeedWorkPerQuery))
              return false;
            std::vector<JumpTableValueQuery> SeedQueries;
            SeedQueries.reserve(SeedQueryCount);
            for (uint32_t Coordinate = 0; Coordinate < CandidateCapacity;
                 ++Coordinate)
              for (const JumpTableValueOccurrence &Index : SeedOccurrences) {
                JumpTableValueQuery Query;
                Query.Candidate = Index.Value;
                Query.UseAddr = Index.Addr;
                Query.UseSeq = Index.Seq;
                Query.Alternatives = {
                    {NdVar::cst(Coordinate, Index.Value.Size), InvalidVA, -1,
                     false},
                    {NdVar::scalar(Coordinate, Index.Value.Size), InvalidVA, -1,
                     false}};
                Query.AllowZeroExtension = true;
                SeedQueries.push_back(std::move(Query));
              }
            std::vector<bool> SeedComplete;
            const std::vector<bool> SeedMatches = tableValuesMatchAtUses(
                SeedQueries, nullptr, &SeedComplete, Rec.Addr, &NoTargets,
                AggregateEvidenceBudget);
            if (SeedMatches.size() != SeedQueries.size() ||
                SeedComplete.size() != SeedQueries.size() ||
                std::any_of(SeedComplete.begin(), SeedComplete.end(),
                            [](bool Complete) { return !Complete; })) {
              if (EvidenceIncomplete)
                *EvidenceIncomplete = true;
              return false;
            }
            for (uint32_t Coordinate = 0; Coordinate < CandidateCapacity;
                 ++Coordinate) {
              const size_t Begin = size_t(Coordinate) * SeedOccurrences.size();
              const size_t End = Begin + SeedOccurrences.size();
              if (std::all_of(SeedMatches.begin() + Begin,
                              SeedMatches.begin() + End,
                              [](bool Match) { return Match; })) {
                Authorized.insert(Coordinate);
                break;
              }
            }

            bool EntryReachabilityComplete = false;
            const std::set<va_t> EntryReachable =
                candidateReachableInstructions(
                    Rec, NoTargets, Roots, Info.StorageRanges,
                    AggregateEvidenceBudget, &EntryReachabilityComplete);
            if (!EntryReachabilityComplete) {
              if (EvidenceIncomplete)
                *EvidenceIncomplete = true;
              return false;
            }
            JumpTableInfo EntryInfo = Info;
            EntryInfo.PhysicalCapacity = CandidateCapacity;
            EntryInfo.MaxEntries = 0;
            EntryInfo.IndexDomainAuthenticated = false;
            EntryInfo.AuthenticatedModuloBound = 0;
            bool EntryIncomplete = false;
            std::vector<JumpTableValueOccurrence> RetainedProducers;
            const bool EntryModulo = inferBoundsFromModulo(
                Img, Rec, EntryInfo, AggregateEvidenceBudget, &EntryIncomplete,
                /*RequireProducerReachability=*/true, &NoTargets,
                &EntryReachable, /*AllowFixedPointBootstrap=*/false,
                &RetainedProducers);
            if (EntryIncomplete) {
              if (EvidenceIncomplete)
                *EvidenceIncomplete = true;
              return false;
            }
            if (EntryModulo && EntryInfo.MaxEntries <= CandidateCapacity)
              for (uint32_t Coordinate = 0;
                   Coordinate < EntryInfo.MaxEntries; ++Coordinate)
                Authorized.insert(Coordinate);

            // Some exact modulo theorems are replayable only by rediscovering
            // their full expression in each immutable graph (for example an
            // add-after-scaled-difference producer).  Require a retained
            // occurrence only when the theorem exported one; otherwise let
            // the next graph rerun bounded discovery instead of manufacturing
            // a nonzero required bound with a null witness set.
            uint32_t RetainedProducerBound =
                EntryModulo && !RetainedProducers.empty()
                    ? EntryInfo.MaxEntries
                    : 0;
            if (!EntryModulo)
              RetainedProducers.clear();

            if (Authorized.empty())
              return false;
            for (uint32_t Iteration = 0; Iteration <= CandidateCapacity;
                 ++Iteration) {
              std::optional<std::vector<va_t>> AuthorizedTargetsOr =
                  targetsFor(Authorized);
              if (!AuthorizedTargetsOr)
                return false;
              const std::vector<va_t> &AuthorizedTargets =
                  *AuthorizedTargetsOr;
              bool ReachabilityComplete = false;
              const std::set<va_t> Reachable = candidateReachableInstructions(
                  Rec, AuthorizedTargets, Roots, Info.StorageRanges,
                  AggregateEvidenceBudget, &ReachabilityComplete);
              if (!ReachabilityComplete) {
                if (EvidenceIncomplete)
                  *EvidenceIncomplete = true;
                return false;
              }
              if (!Reachable.count(CurrentFuncEntry))
                return false;

              JumpTableInfo CoreInfo = Info;
              CoreInfo.PhysicalCapacity = CandidateCapacity;
              CoreInfo.MaxEntries = 0;
              CoreInfo.IndexDomainAuthenticated = false;
              CoreInfo.AuthenticatedModuloBound = 0;
              bool CoreIncomplete = false;
              std::vector<JumpTableValueOccurrence> CoreProducers;
              const bool CoreModulo = inferBoundsFromModulo(
                  Img, Rec, CoreInfo, AggregateEvidenceBudget, &CoreIncomplete,
                  /*RequireProducerReachability=*/true, &AuthorizedTargets,
                  &Reachable, /*AllowFixedPointBootstrap=*/false,
                  &CoreProducers, RetainedProducerBound,
                  RetainedProducers.empty() ? nullptr : &RetainedProducers,
                  /*RestrictProducerDiscovery=*/!RetainedProducers.empty());
              if (CoreIncomplete) {
                if (EvidenceIncomplete)
                  *EvidenceIncomplete = true;
                return false;
              }

              if (!consumeFixedPointEvidence(Authorized.size()))
                return false;
              std::set<uint32_t> Next = Authorized;
              if (CoreModulo && CoreInfo.MaxEntries != 0 &&
                  CoreInfo.MaxEntries <= CandidateCapacity) {
                RetainedProducerBound = CoreProducers.empty()
                                            ? 0
                                            : CoreInfo.MaxEntries;
                RetainedProducers = std::move(CoreProducers);
                for (uint32_t Coordinate = 0;
                     Coordinate < CoreInfo.MaxEntries; ++Coordinate) {
                  if (!consumeFixedPointEvidence())
                    return false;
                  Next.insert(Coordinate);
                }
              }

              const bool CoreClosed =
                  CoreModulo && CoreInfo.MaxEntries == Authorized.size() &&
                  std::all_of(Authorized.begin(), Authorized.end(),
                              [&](uint32_t Coordinate) {
                                return Coordinate < CoreInfo.MaxEntries;
                              });
              if (Next == Authorized) {
                const std::optional<bool> Growth =
                    queueGraphGrowth(AuthorizedTargets);
                if (!Growth)
                  return false;
                if (*Growth) {
                  // CoreInfo was proved against the current immutable graph.
                  // A closed numeric domain is not a closed CFG until every
                  // authorized case body has been decoded into this snapshot;
                  // replay after the bounded rebuild before publishing.
                  return false;
                }
                if (CoreClosed &&
                    Authorized.size() >= limits::kMinJumpTableEntries) {
                  Info = std::move(CoreInfo);
                  return true;
                }
                // Every authorized target is already present in this immutable
                // graph and the exact proof reached a stable semantic reject.
                // This candidate is locally invalid, not globally incomplete;
                // poisoning the enclosing stage would roll back unrelated
                // candidates and retry the same closed graph.
                return false;
              }

              std::optional<std::vector<va_t>> NextTargets = targetsFor(Next);
              if (!NextTargets)
                return false;
              const std::optional<bool> Growth =
                  queueGraphGrowth(*NextTargets);
              if (!Growth)
                return false;
              if (*Growth)
                return false;
              Authorized = std::move(Next);
            }
            if (EvidenceIncomplete)
              *EvidenceIncomplete = true;
            return false;
          }
        }
      }
    }
  }

  // Layouts outside the bounded coordinate-to-target gate above cannot run
  // the incremental LFP, but a previously published table still must not feed
  // its own case-local producer back into revalidation.  First authenticate
  // exact producer occurrences with the candidate edge removed.  Then replay
  // only those occurrences in the complete old-target graph: each recipe is
  // re-resolved at the same point, and the selector MustEqual/MayDepend closure
  // below sees every late case.  This admits an entry `% N` followed by common
  // case backedges (including large O0 tables), while a case-local bootstrap
  // has no empty-graph producer and a late out-of-domain write fails replay.
  if (TopLevelFixedPointAttempt && !Rec.JumpTableTargets.empty()) {
    const std::vector<va_t> NoTargets;
    const std::set<va_t> &Roots = ActiveJumpTableProofRoots
                                      ? *ActiveJumpTableProofRoots
                                      : PersistentCFGRoots;
    bool EntryReachabilityComplete = false;
    const std::set<va_t> EntryReachable = candidateReachableInstructions(
        Rec, NoTargets, Roots, Info.StorageRanges, AggregateEvidenceBudget,
        &EntryReachabilityComplete);
    if (!EntryReachabilityComplete) {
      if (EvidenceIncomplete)
        *EvidenceIncomplete = true;
      return false;
    }
    JumpTableInfo EntryInfo = Info;
    EntryInfo.MaxEntries = 0;
    EntryInfo.IndexDomainAuthenticated = false;
    EntryInfo.AuthenticatedModuloBound = 0;
    bool EntryIncomplete = false;
    std::vector<JumpTableValueOccurrence> EntryProducers;
    const bool EntryModulo = inferBoundsFromModulo(
        Img, Rec, EntryInfo, AggregateEvidenceBudget, &EntryIncomplete,
        /*RequireProducerReachability=*/true, &NoTargets, &EntryReachable,
        /*AllowFixedPointBootstrap=*/false, &EntryProducers);
    if (EntryIncomplete) {
      if (EvidenceIncomplete)
        *EvidenceIncomplete = true;
      return false;
    }
    if (!EntryModulo || EntryInfo.MaxEntries == 0)
      return false;

    bool FullReachabilityComplete = false;
    const std::set<va_t> FullReachable = candidateReachableInstructions(
        Rec, Rec.JumpTableTargets, Roots, Info.StorageRanges,
        AggregateEvidenceBudget, &FullReachabilityComplete);
    if (!FullReachabilityComplete) {
      if (EvidenceIncomplete)
        *EvidenceIncomplete = true;
      return false;
    }
    if (!consumeFixedPointProducts(
            {{Rec.JumpTableTargets.size(),
              orderedEvidenceLookupWork(FullReachable.size())},
             {Rec.JumpTableTargets.size(),
              orderedEvidenceLookupWork(Insns.size())},
             {Rec.JumpTableTargets.size(),
              orderedEvidenceLookupWork(BlockStarts.size())}}))
      return false;
    const bool FullTargetsDecoded =
        std::all_of(Rec.JumpTableTargets.begin(),
                    Rec.JumpTableTargets.end(), [&](va_t Target) {
                      return FullReachable.count(Target) &&
                             Insns.count(Target) && BlockStarts.count(Target);
                    });
    if (!FullTargetsDecoded) {
      if (EvidenceIncomplete)
        *EvidenceIncomplete = true;
      return false;
    }
    JumpTableInfo FullInfo = Info;
    FullInfo.MaxEntries = 0;
    FullInfo.IndexDomainAuthenticated = false;
    FullInfo.AuthenticatedModuloBound = 0;
    bool FullIncomplete = false;
    const bool HasReplayableEntryProducer = !EntryProducers.empty();
    const bool FullModulo = inferBoundsFromModulo(
        Img, Rec, FullInfo, AggregateEvidenceBudget, &FullIncomplete,
        /*RequireProducerReachability=*/true, &Rec.JumpTableTargets,
        &FullReachable, /*AllowFixedPointBootstrap=*/false,
        /*AuthenticatedProducers=*/nullptr,
        HasReplayableEntryProducer ? EntryInfo.MaxEntries : 0,
        HasReplayableEntryProducer ? &EntryProducers : nullptr,
        /*RestrictProducerDiscovery=*/HasReplayableEntryProducer);
    if (FullIncomplete) {
      if (EvidenceIncomplete)
        *EvidenceIncomplete = true;
      return false;
    }
    if (!FullModulo || FullInfo.MaxEntries != EntryInfo.MaxEntries)
      return false;
    Info = std::move(FullInfo);
    return true;
  }

  if (!CandidateTargetsOverride && !Rec.JumpTableTargets.empty())
    CandidateTargetsOverride = &Rec.JumpTableTargets;
  RequireProducerReachability |=
      CandidateTargetsOverride && !CandidateTargetsOverride->empty();
  std::optional<std::set<va_t>> OwnedReachableInstructions;
  if (!ReachableInstructions && CandidateTargetsOverride) {
    const std::set<va_t> &Roots = ActiveJumpTableProofRoots
                                      ? *ActiveJumpTableProofRoots
                                      : PersistentCFGRoots;
    bool ReachabilityComplete = false;
    OwnedReachableInstructions = candidateReachableInstructions(
        Rec, *CandidateTargetsOverride, Roots, Info.StorageRanges,
        AggregateEvidenceBudget, &ReachabilityComplete);
    if (!ReachabilityComplete) {
      if (EvidenceIncomplete)
        *EvidenceIncomplete = true;
      return false;
    }
    ReachableInstructions = &*OwnedReachableInstructions;
  }

  if ((RequiredProducerBound == 0) != (RequiredProducers == nullptr) ||
      (RequiredProducers &&
       (RequiredProducerBound < limits::kMinJumpTableEntries ||
        RequiredProducerBound > limits::kMaxJumpTableEntries ||
        RequiredProducers->empty() ||
        RequiredProducers->size() > kMaxRetainedModuloProposalOps ||
        (Info.PhysicalCapacity != 0 &&
         RequiredProducerBound > Info.PhysicalCapacity) ||
        std::any_of(RequiredProducers->begin(), RequiredProducers->end(),
                    [](const JumpTableValueOccurrence &Occurrence) {
                      return !Occurrence.DefinedAtPoint ||
                             Occurrence.Value.Size == 0 ||
                             (!Occurrence.Value.isReg() &&
                              !Occurrence.Value.isTemp()) ||
                             Occurrence.Addr == InvalidVA ||
                             Occurrence.Seq < 0;
                    }))))
    return false;
  if (RestrictProducerDiscovery && !RequiredProducers)
    return false;

  // Retained occurrences are monotone proposals, never cached proof.  Replay
  // every one against this immutable target graph before proposal discovery
  // can spend its bounded lexical accounts.  A late edge into the middle of a
  // reciprocal recipe therefore changes the current expression and rejects
  // the occurrence; a direct URem is likewise checked at the same output.
  std::vector<JumpTableValueOccurrence> RevalidatedRequiredProducers;
  if (RequiredProducers) {
    for (size_t I = 0; I < RequiredProducers->size(); ++I) {
      if (!consumeFixedPointEvidence(I))
        return false;
      if (std::find(RequiredProducers->begin(),
                    RequiredProducers->begin() + I,
                    (*RequiredProducers)[I]) !=
          RequiredProducers->begin() + I)
        return false;
    }
    if (!consumeFixedPointProducts({{RequiredProducers->size(), 5}}))
      return false;
    std::vector<JumpTableValueQuery> RequiredQueries;
    RequiredQueries.reserve(RequiredProducers->size());
    for (const JumpTableValueOccurrence &Producer : *RequiredProducers) {
      JumpTableValueQuery Query;
      Query.Candidate = Producer.Value;
      Query.UseAddr = Producer.Addr;
      Query.UseSeq = Producer.Seq;
      Query.Relation = JumpTableValueRelation::ExactUnsignedModuloRecipe;
      Query.UnsignedUpperBound = RequiredProducerBound;
      RequiredQueries.push_back(std::move(Query));
    }
    std::vector<bool> RequiredComplete;
    const std::vector<bool> RequiredResults = tableValuesMatchAtUses(
        RequiredQueries, nullptr, &RequiredComplete,
        CandidateTargetsOverride ? Rec.Addr : InvalidVA,
        CandidateTargetsOverride, AggregateEvidenceBudget);
    if (RequiredResults.size() != RequiredQueries.size() ||
        RequiredComplete.size() != RequiredQueries.size() ||
        std::any_of(RequiredComplete.begin(), RequiredComplete.end(),
                    [](bool Complete) { return !Complete; })) {
      if (EvidenceIncomplete)
        *EvidenceIncomplete = true;
      return false;
    }
    if (std::any_of(RequiredResults.begin(), RequiredResults.end(),
                    [](bool Match) { return !Match; }))
      return false;
    RevalidatedRequiredProducers = *RequiredProducers;
  }

  // Each phase retains its historic local cap, but all four debit the same
  // candidate-wide account.  Replaying modulo evidence across candidates or
  // final-root stages therefore cannot refresh four unmetered 4096-unit pools.
  size_t LocalAggregateEvidenceBudget = limits::kMaxJumpTableEvidenceWork;
  ModuloEvidenceBudget ProposalBudget(
      kModuloProposalWork, &LocalAggregateEvidenceBudget,
      AggregateEvidenceBudget);
  ModuloEvidenceBudget StructuralBudget(kModuloStructuralRetentionWork,
                                        &LocalAggregateEvidenceBudget,
                                        AggregateEvidenceBudget);
  ModuloEvidenceBudget DirectBudget(kModuloDirectWork,
                                    &LocalAggregateEvidenceBudget,
                                    AggregateEvidenceBudget);
  ModuloEvidenceBudget ReplayBudget(kModuloReplayWork,
                                    &LocalAggregateEvidenceBudget,
                                    AggregateEvidenceBudget);
  bool Succeeded = false;
  bool ExplicitlyIncomplete = false;
  struct ModuloIncompleteOnExit {
    bool *Output = nullptr;
    const bool &Succeeded;
    const bool &ExplicitlyIncomplete;
    const ModuloEvidenceBudget &Proposal;
    const ModuloEvidenceBudget &Structural;
    const ModuloEvidenceBudget &Direct;
    const ModuloEvidenceBudget &Replay;

    ~ModuloIncompleteOnExit() {
      if (Output && !Succeeded &&
          (ExplicitlyIncomplete || Direct.exhausted() || Replay.exhausted()))
        *Output = true;
    }
  } IncompleteOnExit{EvidenceIncomplete,
                     Succeeded,
                     ExplicitlyIncomplete,
                     ProposalBudget,
                     StructuralBudget,
                     DirectBudget,
                     ReplayBudget};
  auto consumeProducts =
      [](ModuloEvidenceBudget &Budget,
         std::initializer_list<std::pair<size_t, size_t>> Products) {
        const size_t Max = std::numeric_limits<size_t>::max();
        size_t Total = 0;
        for (const auto &[Count, Cost] : Products) {
          if (Count != 0 && Cost > Max / Count)
            return Budget.consume(Max);
          const size_t Product = Count * Cost;
          if (Product > Max - Total)
            return Budget.consume(Max);
          Total += Product;
        }
        return Budget.consume(Total);
      };
  if (!consumeProducts(ProposalBudget,
                       {{Info.IndexValueAlternatives.size(), 2}}))
    return false;
  std::vector<JumpTableValueOccurrence> IndexOccurrences =
      Info.IndexValueAlternatives;
  if (IndexOccurrences.empty() && Info.IndexValueAtUse.Size != 0 &&
      Info.IndexUseAddr != InvalidVA && Info.IndexUseSeq >= 0) {
    if (!ProposalBudget.consume(2))
      return false;
    IndexOccurrences.push_back({Info.IndexValueAtUse, Info.IndexUseAddr,
                                Info.IndexUseSeq, Info.IndexValueDefinedAtUse});
  }
  if (IndexOccurrences.empty() ||
      std::any_of(IndexOccurrences.begin(), IndexOccurrences.end(),
                  [](const JumpTableValueOccurrence &Occurrence) {
                    return Occurrence.DefinedAtPoint ||
                           Occurrence.Value.Size == 0 ||
                           (!Occurrence.Value.isReg() &&
                            !Occurrence.Value.isTemp()) ||
                           Occurrence.Addr == InvalidVA || Occurrence.Seq < 0;
                  }))
    return false;
  if (IndexOccurrences.size() > std::numeric_limits<size_t>::max() / 2)
    return false;
  if (!ProposalBudget.canConsume(IndexOccurrences.size() * 2))
    return false;
  // Prepay one desired-point entry and one unique-or-ambiguous location entry
  // per selector occurrence.  Populating these while the bounded instruction
  // prefix is copied avoids rescanning up to 512 LowOps for every bound.
  if (!consumeProducts(
          ProposalBudget,
          {{IndexOccurrences.size(),
            orderedEvidenceLookupWork(IndexOccurrences.size()) + 2}}))
    return false;
  std::set<detail::JumpTableProofPoint> IndexOccurrencePoints;
  for (const JumpTableValueOccurrence &Occurrence : IndexOccurrences)
    IndexOccurrencePoints.emplace(Occurrence.Addr, Occurrence.Seq);
  std::map<detail::JumpTableProofPoint, detail::JumpTableProofLocation>
      UniqueIndexPoints;
  std::set<detail::JumpTableProofPoint> AmbiguousIndexPoints;

  auto sameVar = [](const NdVar &A, const NdVar &B) {
    return A.Space == B.Space && A.Offset == B.Offset && A.Size == B.Size;
  };

  // The syntax trace below proposes modulus candidates only.  It has no
  // authority to publish a range: every producer is authenticated separately,
  // then one shared CFG/lane-aware resolver session confines every feasible
  // actual selector arm to an exact producer or literal coordinate.  A lexical
  // sibling multiply, unrelated quotient root, or x-y*N can therefore add only
  // a failed proposal; none can bound the dispatch.
  const va_t ProofEnd =
      CurrentFuncRange && CurrentFuncRange->first == CurrentFuncEntry
          ? CurrentFuncRange->second
          : InvalidVA;
  auto inProofEnvelope = [&](va_t Addr) {
    return Addr >= CurrentFuncEntry &&
           (ProofEnd == InvalidVA || Addr < ProofEnd);
  };
  // Candidate discovery is intentionally an under-approximation.  Retain an
  // instruction-atomic prefix only while both the fixed op ceiling and the
  // proposal account can prepay every appended LowOp.  A producer outside the
  // prefix is simply absent from Alternatives; the final MustEqual/MayDepend
  // replay still examines the complete CFG, so any omitted producer that can
  // reach the selector makes the proof fail rather than donating a domain.
  std::vector<LowOp> Ops;
  struct RetainedInstructionSpan {
    size_t End = 0;
    bool IsInstructionGuard = false;
  };
  std::vector<RetainedInstructionSpan> RetainedInstructionSpans;
  auto retainInstruction = [&](const InsnRecord &Instruction) {
    // Charge the instruction record even when it carries no LowOps.  Otherwise
    // an attacker can place an unbounded run of zero-op decoded records in
    // front of the bounded retained prefix without debiting the shared stage.
    if (!ProposalBudget.consume()) {
      return false;
    }
    const size_t InstructionOps = Instruction.Ops.size();
    if (InstructionOps >
        kMaxRetainedModuloProposalOps -
            std::min(Ops.size(), kMaxRetainedModuloProposalOps)) {
      return false;
    }
    // Retain one compact instruction span beside the flattened LowOps.  This
    // makes the later sequential proposal scan guard-aware without performing
    // one ordered Insns lookup per LowOp.  The two units per retained object
    // prepay construction/copy and future destruction; span transitions are
    // charged when the scan crosses them.
    if (!consumeProducts(
            ProposalBudget,
            {{InstructionOps, 2}, {InstructionOps != 0 ? size_t{1} : 0, 2}})) {
      return false;
    }
    const size_t Begin = Ops.size();
    Ops.insert(Ops.end(), Instruction.Ops.begin(), Instruction.Ops.end());
    if (InstructionOps != 0)
      RetainedInstructionSpans.push_back(
          {Ops.size(), Instruction.IsInstructionGuard});
    for (size_t J = 0; J < InstructionOps; ++J) {
      const LowOp &Op = Ops[Begin + J];
      const detail::JumpTableProofPoint Point{Op.Addr, Op.Seq};
      if (!ProposalBudget.consume(
              orderedEvidenceLookupWork(IndexOccurrencePoints.size())) ||
          !IndexOccurrencePoints.count(Point))
        continue;
      if (!consumeProducts(
              ProposalBudget,
              {{3, orderedEvidenceLookupWork(Ops.size())}, {1, 2}}))
        break;
      detail::recordUniqueJumpTableProofPoint(
          UniqueIndexPoints, AmbiguousIndexPoints, Point,
          {/*Block=*/-1, static_cast<int>(Begin + J)});
    }
    return !ProposalBudget.exhausted();
  };
  if (!RestrictProducerDiscovery && ReachableInstructions) {
    if (!ProposalBudget.consume(
            orderedEvidenceLookupWork(ReachableInstructions->size())))
      return false;
    for (va_t Addr : *ReachableInstructions) {
      if (!inProofEnvelope(Addr))
        continue;
      if (!ProposalBudget.consume(orderedEvidenceLookupWork(Insns.size()))) {
        break;
      }
      const auto It = Insns.find(Addr);
      if (It != Insns.end() && !retainInstruction(It->second))
        break;
    }
  } else if (!RestrictProducerDiscovery) {
    if (!ProposalBudget.consume(orderedEvidenceLookupWork(Insns.size())))
      return false;
    for (auto It = Insns.lower_bound(CurrentFuncEntry);
         It != Insns.end() && inProofEnvelope(It->first); ++It)
      if (!retainInstruction(It->second))
        break;
  }
  if (Ops.empty() && !RestrictProducerDiscovery)
    return false;

  // A candidate-local replay with provisional targets must reach at least one
  // exact remainder producer.  Literal coordinates seed exploration, but can
  // never stand in for the runtime producer that closes the modulo domain.
  BoundedLinearMultipleEvaluator LinearEvaluator(Ops, ProposalBudget);
  auto overlaps = [](const NdVar &A, const NdVar &B) {
    if (A.Space != B.Space || A.Size == 0 || B.Size == 0)
      return false;
    if (A.Offset > InvalidVA - A.Size || B.Offset > InvalidVA - B.Size)
      return true;
    return A.Offset < B.Offset + B.Size && B.Offset < A.Offset + A.Size;
  };
  auto proofBlockStart =
      [&](va_t UseAddr, ModuloEvidenceBudget &Budget) -> std::optional<va_t> {
    if (!Budget.consume(orderedEvidenceLookupWork(BlockStarts.size())))
      return std::nullopt;
    va_t Start = CurrentFuncEntry;
    if (auto It = BlockStarts.upper_bound(UseAddr); It != BlockStarts.begin()) {
      --It;
      Start = std::max(Start, *It);
    }
    if (ActiveJumpTableProofRoots) {
      const auto &Roots = *ActiveJumpTableProofRoots;
      if (!Budget.consume(orderedEvidenceLookupWork(Roots.size())))
        return std::nullopt;
      if (auto It = Roots.upper_bound(UseAddr); It != Roots.begin()) {
        --It;
        Start = std::max(Start, *It);
      }
    }
    return Start;
  };
  auto localDef = [&](int From, const NdVar &Value, va_t BlockStart,
                      ModuloEvidenceBudget &Budget) -> std::optional<int> {
    for (int I = From; I >= 0; --I) {
      if (!Budget.consume())
        return std::nullopt;
      const LowOp &Op = Ops[I];
      if (Op.Addr < BlockStart)
        break;
      if ((Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL) &&
          Value.isReg())
        return std::nullopt;
      if (!overlaps(Op.Output, Value))
        continue;
      if (!Budget.consume(orderedEvidenceLookupWork(Insns.size())))
        return std::nullopt;
      const auto RecIt = Insns.find(Op.Addr);
      const bool PreservesQueriedLowLane =
          Op.Opcode == NdOp::INT_ZEXT && Op.NumInputs >= 1 &&
          sameVar(Op.Inputs[0], Value) && Op.Output.Space == Value.Space &&
          Op.Output.Offset == Value.Offset && Op.Output.Size > Value.Size;
      if (RecIt == Insns.end() || RecIt->second.IsInstructionGuard ||
          (!sameVar(Op.Output, Value) && !PreservesQueriedLowLane))
        return std::nullopt;
      return I;
    }
    return std::nullopt;
  };
  std::function<std::optional<uint64_t>(int, NdVar, va_t, unsigned,
                                        ModuloEvidenceBudget &)>
      localConstant =
          [&](int From, NdVar Value, va_t BlockStart, unsigned Depth,
              ModuloEvidenceBudget &Budget) -> std::optional<uint64_t> {
    if (!Budget.consume())
      return std::nullopt;
    if (Depth > limits::kMaxQuasiCopyDepth || Value.Size == 0 ||
        Value.Size > sizeof(uint64_t))
      return std::nullopt;
    const std::optional<uint64_t> Mask = integerWidthMask(Value.Size);
    if (!Mask)
      return std::nullopt;
    if (Value.isConst())
      return Value.Offset & *Mask;
    if (!Value.isReg() && !Value.isTemp())
      return std::nullopt;
    const std::optional<int> D = localDef(From, Value, BlockStart, Budget);
    if (!D)
      return std::nullopt;
    const LowOp &Op = Ops[*D];
    if (Op.NumInputs < 1 ||
        (Op.Opcode != NdOp::COPY && Op.Opcode != NdOp::INT_ZEXT))
      return std::nullopt;
    const std::optional<uint64_t> Input =
        localConstant(*D - 1, Op.Inputs[0], BlockStart, Depth + 1, Budget);
    return Input ? std::optional<uint64_t>(*Input & *Mask) : std::nullopt;
  };
  auto provesDirectUnsignedRemainder = [&](size_t IndexOccurrence,
                                           uint32_t Bound) {
    if (IndexOccurrence >= IndexOccurrences.size())
      return false;
    const JumpTableValueOccurrence &Index = IndexOccurrences[IndexOccurrence];
    if (!DirectBudget.consume(
            orderedEvidenceLookupWork(UniqueIndexPoints.size())))
      return false;
    const auto Use = UniqueIndexPoints.find({Index.Addr, Index.Seq});
    if (Use == UniqueIndexPoints.end() || Use->second.second < 0 ||
        Use->second.second >= static_cast<int>(Ops.size()))
      return false;
    const int UsePoint = Use->second.second;
    const LowOp &UseOp = Ops[UsePoint];
    if (!std::any_of(
            UseOp.Inputs, UseOp.Inputs + UseOp.NumInputs,
            [&](const NdVar &Input) { return sameVar(Input, Index.Value); }))
      return false;
    const std::optional<va_t> BlockStart =
        proofBlockStart(Index.Addr, DirectBudget);
    if (!BlockStart)
      return false;
    NdVar Value = Index.Value;
    int From = UsePoint - 1;
    for (unsigned Depth = 0; Depth <= limits::kMaxQuasiCopyDepth; ++Depth) {
      const std::optional<int> D =
          localDef(From, Value, *BlockStart, DirectBudget);
      if (!D)
        return false;
      const LowOp &Op = Ops[*D];
      if (Op.Opcode == NdOp::INT_REM && Op.NumInputs >= 2 &&
          Op.Output.Size != 0 && Op.Output.Size <= sizeof(uint64_t)) {
        const std::optional<uint64_t> Divisor =
            localConstant(*D - 1, Op.Inputs[1], *BlockStart, 0, DirectBudget);
        return Divisor && *Divisor == Bound && *Divisor != 0;
      }
      if (Op.NumInputs < 1 ||
          (Op.Opcode != NdOp::COPY && Op.Opcode != NdOp::INT_ZEXT) ||
          (!Op.Inputs[0].isReg() && !Op.Inputs[0].isTemp()) ||
          (Op.Opcode == NdOp::COPY && Op.Inputs[0].Size != Op.Output.Size) ||
          (Op.Opcode == NdOp::INT_ZEXT && Op.Inputs[0].Size >= Op.Output.Size))
        return false;
      Value = Op.Inputs[0];
      From = *D - 1;
    }
    return false;
  };
  struct ModuloProducerCandidate {
    JumpTableValueOccurrence Occurrence;
    int DefinitionIndex = -1;
  };
  std::map<uint32_t, std::vector<ModuloProducerCandidate>> ProducerCandidates;
  auto recordCandidateProducer = [&](uint64_t Bound, const LowOp &Producer,
                                     int DefinitionIndex,
                                     ModuloEvidenceBudget &Budget) {
    if (Bound < limits::kMinJumpTableEntries ||
        Bound > limits::kMaxJumpTableEntries || DefinitionIndex < -1 ||
        DefinitionIndex >= static_cast<int>(Ops.size()) ||
        Producer.Output.Size == 0 ||
        (!Producer.Output.isReg() && !Producer.Output.isTemp()))
      return;
    // Capacity cannot authenticate a runtime domain, but it can reject an
    // impossible dense modulo proposal: x % N reaches every coordinate in
    // [0,N), so a table with fewer than N authenticated physical slots cannot
    // implement it.  Filter that proposal before exact-recipe symbolization;
    // otherwise an unrelated large modulus or shared arithmetic DAG can spend
    // the complete structural account ahead of the feasible producer.
    if (Info.PhysicalCapacity != 0 && Bound > Info.PhysicalCapacity)
      return;
    const uint32_t NarrowBound = static_cast<uint32_t>(Bound);
    if (!Budget.consume(
            orderedEvidenceLookupWork(ProducerCandidates.size())))
      return;
    auto KnownIt = ProducerCandidates.find(NarrowBound);
    if (KnownIt == ProducerCandidates.end()) {
      // The ordered lookup is paid above; retain the new map node before
      // try_emplace so allocation cannot precede its evidence charge.
      if (!Budget.consume())
        return;
      KnownIt = ProducerCandidates.try_emplace(NarrowBound).first;
    }
    auto &Known = KnownIt->second;
    JumpTableValueOccurrence Occurrence{Producer.Output, Producer.Addr,
                                        Producer.Seq,
                                        /*DefinedAtPoint=*/true};
    if (!Budget.consume(Known.size()))
      return;
    if (std::any_of(Known.begin(), Known.end(),
                    [&](const ModuloProducerCandidate &Candidate) {
                      return Candidate.Occurrence == Occurrence;
                    }))
      return;
    // One logical unit prepays the retained candidate and its map/vector
    // bookkeeping.  Discovery fails closed rather than accumulating an
    // attacker-sized proposal list.
    if (!Budget.consume())
      return;
    Known.push_back({Occurrence, DefinitionIndex});
  };

  // A relocation run is physical capacity, never a runtime-domain proof.  It
  // is nevertheless a useful bounded proposal when the exact selector use has
  // a nearest same-lane definition in its CFG block.  In particular LLVM's
  // `% 7` recipe finishes in the instruction immediately before the indexed
  // load, whereas the load instruction itself defines only the scaled address
  // temporary.  The full CFG ExactUnsignedModuloRecipe relation below must
  // authenticate that exact producer, and final MustEqual/MayDepend replay
  // must still connect every feasible selector arm to it; capacity alone can
  // therefore add only a failed proposal, never authorize a domain.
  if (Info.PhysicalCapacity >= limits::kMinJumpTableEntries &&
      Info.PhysicalCapacity <= limits::kMaxJumpTableEntries) {
    for (const JumpTableValueOccurrence &Index : IndexOccurrences) {
      if (!ProposalBudget.consume(
              orderedEvidenceLookupWork(UniqueIndexPoints.size())))
        break;
      const auto Use = UniqueIndexPoints.find({Index.Addr, Index.Seq});
      if (Use == UniqueIndexPoints.end() || Use->second.second < 0 ||
          Use->second.second >= static_cast<int>(Ops.size()))
        continue;
      const int UsePoint = Use->second.second;
      const LowOp &UseOp = Ops[UsePoint];
      if (!std::any_of(
              UseOp.Inputs, UseOp.Inputs + UseOp.NumInputs,
              [&](const NdVar &Input) { return sameVar(Input, Index.Value); }))
        continue;
      const std::optional<va_t> BlockStart =
          proofBlockStart(Index.Addr, ProposalBudget);
      if (!BlockStart)
        break;
      const std::optional<int> Definition =
          localDef(UsePoint - 1, Index.Value, *BlockStart, ProposalBudget);
      if (Definition)
        recordCandidateProducer(Info.PhysicalCapacity, Ops[*Definition],
                                *Definition, ProposalBudget);
      if (ProposalBudget.exhausted())
        break;
    }

    // The fixed lexical prefix above is a sound under-approximation, but a
    // computed-goto can place one exact remainder producer in each newly
    // authorized case.  Once a candidate-local graph is available, scan only
    // its reachable instructions and retain bounded INT_SUB or direct INT_REM
    // output occurrences as additional capacity proposals.  We keep no full
    // LowOp copy: the shared point-sensitive resolver authenticates every
    // retained occurrence as an exact reciprocal recipe or URem(X, N), and the
    // final MustEqual/MayDepend batch still rejects any omitted feasible
    // producer.
    // Thus physical capacity chooses N to test; it never proves x < N.
    if (ReachableInstructions && !RestrictProducerDiscovery) {
      if (StructuralBudget.consume(
              orderedEvidenceLookupWork(ReachableInstructions->size()))) {
        for (va_t Addr : *ReachableInstructions) {
          if (!inProofEnvelope(Addr))
            continue;
          if (!StructuralBudget.consume(
                  orderedEvidenceLookupWork(Insns.size()) + 1)) {
            break;
          }
          const auto It = Insns.find(Addr);
          if (It == Insns.end() || It->second.IsInstructionGuard)
            continue;
          if (!StructuralBudget.consume(It->second.Ops.size())) {
            break;
          }
          for (const LowOp &Op : It->second.Ops) {
            if ((Op.Opcode != NdOp::INT_SUB &&
                 Op.Opcode != NdOp::INT_REM) ||
                Op.NumInputs < 2)
              continue;
            recordCandidateProducer(Info.PhysicalCapacity, Op,
                                    /*DefinitionIndex=*/-1,
                                    StructuralBudget);
            if (StructuralBudget.exhausted()) {
              break;
            }
          }
          if (StructuralBudget.exhausted())
            break;
        }
      }
    }
  }

  auto hasAddAdjustedNumerator = [&](int From, NdVar Value, va_t BlockStart) {
    for (unsigned Depth = 0; Depth <= limits::kMaxQuasiCopyDepth; ++Depth) {
      const std::optional<int> Definition =
          localDef(From, Value, BlockStart, ProposalBudget);
      if (!Definition)
        return false;
      const LowOp &Def = Ops[*Definition];
      if (Def.Opcode == NdOp::INT_ADD && Def.NumInputs >= 2)
        return (Def.Inputs[0].isReg() || Def.Inputs[0].isTemp()) &&
               (Def.Inputs[1].isReg() || Def.Inputs[1].isTemp());
      if (Def.NumInputs < 1 ||
          (Def.Opcode != NdOp::COPY && Def.Opcode != NdOp::INT_ZEXT) ||
          (!Def.Inputs[0].isReg() && !Def.Inputs[0].isTemp()))
        return false;
      Value = Def.Inputs[0];
      From = *Definition - 1;
    }
    return false;
  };

  // Candidate discovery scans only the prepaid prefix.  A shared local
  // computed-goto commonly stores each case's remainder to a frame cell, so
  // several producer occurrences may be retained even though none is a
  // lexical definition of the final table-index use.  These are proposals
  // only: the batched proof below still requires every feasible selector arm
  // to equal an exact authenticated producer (or literal coordinate).
  size_t RetainedSpanIndex = 0;
  for (int I = 0; I < static_cast<int>(Ops.size()); ++I) {
    if (!ProposalBudget.canConsume() || !ProposalBudget.consume())
      break;
    while (RetainedSpanIndex < RetainedInstructionSpans.size() &&
           size_t(I) >= RetainedInstructionSpans[RetainedSpanIndex].End) {
      if (!ProposalBudget.consume())
        break;
      ++RetainedSpanIndex;
    }
    if (ProposalBudget.exhausted() ||
        RetainedSpanIndex >= RetainedInstructionSpans.size())
      break;
    const LowOp &Op = Ops[I];
    if (RetainedInstructionSpans[RetainedSpanIndex].IsInstructionGuard)
      continue;
    if (Op.Opcode == NdOp::INT_REM && Op.NumInputs >= 2) {
      const std::optional<int64_t> Divisor =
          LinearEvaluator.constant(I - 1, Op.Inputs[1]);
      if (LinearEvaluator.exhausted())
        break;
      if (Divisor && *Divisor > 0)
        recordCandidateProducer(static_cast<uint64_t>(*Divisor), Op, I,
                                ProposalBudget);
      if (ProposalBudget.exhausted())
        break;
      continue;
    }
    if (Op.Opcode != NdOp::INT_SUB || Op.NumInputs < 2)
      continue;
    // In an exact remainder identity the quotient product is the right-hand
    // side of `x - q*N`.  Reversed subtraction and ADD are proposals for no
    // valid theorem, so do not spend proof work decomposing both operands of
    // every unrelated arithmetic instruction in the function.
    const NdVar &Candidate = Op.Inputs[1];
    if (!Candidate.isReg() && !Candidate.isTemp())
      continue;
    int64_t Coefficient = 0;
    if (!LinearEvaluator.evaluate(I - 1, Candidate, Coefficient)) {
      if (LinearEvaluator.exhausted())
        break;
      continue;
    }
    if (Coefficient == std::numeric_limits<int64_t>::min())
      continue;
    const uint64_t Magnitude = Coefficient < 0
                                   ? uint64_t(-(Coefficient + 1)) + 1
                                   : uint64_t(Coefficient);
    // LLVM may spell q*(2^k-1) as `(x+q) - (q<<k)`.  The right-hand side then
    // proposes 2^k even though the divisor is 2^k-1.  Admit the latter only
    // behind the bounded syntactic `add` filter; it remains a proposal until
    // ExactUnsignedModuloRecipe proves that the add and shift share the same
    // quotient/dividend expression.
    if (llvm::isPowerOf2_64(Magnitude) && Magnitude > 2) {
      const std::optional<va_t> BlockStart =
          proofBlockStart(Op.Addr, ProposalBudget);
      if (!BlockStart)
        break;
      if (hasAddAdjustedNumerator(I - 1, Op.Inputs[0], *BlockStart))
        recordCandidateProducer(Magnitude - 1, Op, I, ProposalBudget);
    }
    recordCandidateProducer(Magnitude, Op, I, ProposalBudget);
    if (ProposalBudget.exhausted())
      break;
  }
  auto exactLocalDef = [&](int From, const NdVar &Value,
                           va_t BlockStart) -> std::optional<int> {
    for (unsigned Depth = 0; Depth <= limits::kMaxQuasiCopyDepth; ++Depth) {
      const std::optional<int> D =
          localDef(From, Value, BlockStart, StructuralBudget);
      if (!D)
        return std::nullopt;
      const LowOp &Op = Ops[*D];
      if (sameVar(Op.Output, Value))
        return D;
      // AArch64 emits a full X-register zero-extension after every W-register
      // definition.  It writes the enclosing lane but preserves the exact W
      // value being traced, so keep looking for that lane's real definition.
      if (Op.Opcode != NdOp::INT_ZEXT || Op.NumInputs < 1 ||
          !sameVar(Op.Inputs[0], Value) || Op.Output.Space != Value.Space ||
          Op.Output.Offset != Value.Offset || Op.Output.Size <= Value.Size)
        return std::nullopt;
      From = *D - 1;
    }
    return std::nullopt;
  };
  struct ExactDivisionRemainderShape {
    bool IsUnsigned = false;
    bool ExactRelationReplayable = false;
    JumpTableValueOccurrence NumeratorUse;
  };
  auto exactDivisionRemainderShape =
      [&](int DefinitionIndex,
          uint32_t Bound) -> std::optional<ExactDivisionRemainderShape> {
    if (DefinitionIndex < 0 ||
        DefinitionIndex >= static_cast<int>(Ops.size()) || Bound == 0)
      return std::nullopt;
    const LowOp &Remainder = Ops[DefinitionIndex];
    if (!StructuralBudget.consume(orderedEvidenceLookupWork(Insns.size())))
      return std::nullopt;
    const auto RemainderRec = Insns.find(Remainder.Addr);
    if (RemainderRec == Insns.end() ||
        RemainderRec->second.IsInstructionGuard || Remainder.Output.Size == 0 ||
        Remainder.Output.Size > sizeof(uint64_t))
      return std::nullopt;
    const std::optional<va_t> BlockStart =
        proofBlockStart(Remainder.Addr, StructuralBudget);
    if (!BlockStart)
      return std::nullopt;

    // INT_REM is NeverD's unsigned remainder opcode.  Authenticate the
    // divisor at this exact occurrence; a same-number definition in a
    // sibling block is not a certificate for this producer.
    if (Remainder.Opcode == NdOp::INT_REM && Remainder.NumInputs >= 2) {
      const std::optional<uint64_t> Divisor =
          localConstant(DefinitionIndex - 1, Remainder.Inputs[1], *BlockStart,
                        0, StructuralBudget);
      if (Divisor && *Divisor == Bound)
        return ExactDivisionRemainderShape{/*IsUnsigned=*/true,
                                           /*ExactRelationReplayable=*/true,
                                           {}};
      return std::nullopt;
    }

    // Recognize only the exact division/remainder identity
    //
    //   remainder = x - (x / N) * N
    //
    // in one CFG block.  The unsigned form is a range certificate.  The
    // signed form is retained only as a conditional normalization shape;
    // it receives no authority unless the separate CFG proof below shows
    // its numerator is already in [0,N).  Merely finding coefficient N is
    // insufficient: x-N*y, reversed subtraction, or two independent
    // quotient roots do not establish the domain [0,N).
    if (Remainder.Opcode != NdOp::INT_SUB || Remainder.NumInputs < 2)
      return std::nullopt;
    const NdVar &NumeratorAtSubtract = Remainder.Inputs[0];
    const NdVar &ProductValue = Remainder.Inputs[1];
    if ((!NumeratorAtSubtract.isReg() && !NumeratorAtSubtract.isTemp()) ||
        (!ProductValue.isReg() && !ProductValue.isTemp()) ||
        Remainder.Output.Size != NumeratorAtSubtract.Size ||
        Remainder.Output.Size != ProductValue.Size)
      return std::nullopt;

    const std::optional<int> ProductDef =
        exactLocalDef(DefinitionIndex - 1, ProductValue, *BlockStart);
    if (!ProductDef)
      return std::nullopt;
    const LowOp &Product = Ops[*ProductDef];
    if (Product.Opcode != NdOp::INT_MULT || Product.NumInputs < 2 ||
        Product.Output.Size != Remainder.Output.Size)
      return std::nullopt;

    int QuotientSide = -1;
    for (int ConstantSide = 0; ConstantSide < 2; ++ConstantSide) {
      const std::optional<uint64_t> Multiplier =
          localConstant(*ProductDef - 1, Product.Inputs[ConstantSide],
                        *BlockStart, 0, StructuralBudget);
      const int OtherSide = 1 - ConstantSide;
      if (!Multiplier || *Multiplier != Bound ||
          (!Product.Inputs[OtherSide].isReg() &&
           !Product.Inputs[OtherSide].isTemp()))
        continue;
      if (QuotientSide >= 0)
        return std::nullopt;
      QuotientSide = OtherSide;
    }
    if (QuotientSide < 0)
      return std::nullopt;
    const NdVar &QuotientValue = Product.Inputs[QuotientSide];
    if (QuotientValue.Size != Remainder.Output.Size)
      return std::nullopt;

    const std::optional<int> QuotientDef =
        exactLocalDef(*ProductDef - 1, QuotientValue, *BlockStart);
    if (!QuotientDef)
      return std::nullopt;
    const LowOp &Division = Ops[*QuotientDef];
    if ((Division.Opcode != NdOp::INT_DIV &&
         Division.Opcode != NdOp::INT_SDIV) ||
        Division.NumInputs < 2 ||
        Division.Output.Size != Remainder.Output.Size ||
        !sameVar(Division.Inputs[0], NumeratorAtSubtract) ||
        overlaps(Division.Output, NumeratorAtSubtract))
      return std::nullopt;
    const std::optional<uint64_t> Divisor = localConstant(
        *QuotientDef - 1, Division.Inputs[1], *BlockStart, 0, StructuralBudget);
    if (!Divisor || *Divisor != Bound)
      return std::nullopt;

    // Physical-register equality is meaningful only while the original
    // numerator is live.  Any intervening whole/partial write, guard, or
    // call breaks the exact x-at-both-occurrences identity.
    for (int I = *QuotientDef + 1; I < DefinitionIndex; ++I) {
      if (!StructuralBudget.consume())
        return std::nullopt;
      const LowOp &Between = Ops[I];
      if (((Between.Opcode == NdOp::CALL ||
            Between.Opcode == NdOp::INDIR_CALL) &&
           NumeratorAtSubtract.isReg()) ||
          overlaps(Between.Output, NumeratorAtSubtract))
        return std::nullopt;
    }
    const bool IsUnsigned = Division.Opcode == NdOp::INT_DIV;
    return ExactDivisionRemainderShape{IsUnsigned,
                                       /*ExactRelationReplayable=*/IsUnsigned,
                                       {Division.Inputs[0], Division.Addr,
                                        Division.Seq,
                                        /*DefinedAtPoint=*/false}};
  };

  // Authenticate proposals only through exact modulo structure.  Do not fall
  // back to one independent ULT solver per syntactic candidate: besides being
  // quadratic in the function size, that would let a generic range fact stand
  // in for the required occurrence-level remainder identity.
  struct ConditionalSignedProducer {
    uint32_t Bound = 0;
    JumpTableValueOccurrence Producer;
    JumpTableValueOccurrence NumeratorUse;
  };
  struct StructuralModuloProducer {
    uint32_t Bound = 0;
    JumpTableValueOccurrence Producer;
  };
  std::vector<ConditionalSignedProducer> ConditionalSignedProducers;
  std::vector<StructuralModuloProducer> StructuralModuloProducers;
  std::vector<JumpTableValueQuery> StructuralModuloQueries;
  std::map<uint32_t, std::vector<JumpTableValueOccurrence>> ProvenProducers;
  std::map<uint32_t, std::vector<JumpTableValueOccurrence>>
      ReplayableExactProducers;
  // The exact current-graph replay above prepaid and revalidated these
  // occurrences before attacker-shaped proposal discovery could exhaust its
  // local account.  They may now seed the proven set for this immutable graph;
  // optional discovery below can only add independently revalidated peers.
  if (RequiredProducers) {
    ProvenProducers.emplace(RequiredProducerBound,
                            RevalidatedRequiredProducers);
    ReplayableExactProducers.emplace(RequiredProducerBound,
                                     RevalidatedRequiredProducers);
  }
  if (RestrictProducerDiscovery)
    ProducerCandidates.clear();
  auto appendUniqueProducer =
      [&](std::map<uint32_t, std::vector<JumpTableValueOccurrence>> &ByBound,
          uint32_t Bound, const JumpTableValueOccurrence &Producer) {
    if (!StructuralBudget.consume(
            orderedEvidenceLookupWork(ByBound.size())))
      return false;
    auto It = ByBound.find(Bound);
    if (It == ByBound.end()) {
      if (!StructuralBudget.consume())
        return false;
      It = ByBound.try_emplace(Bound).first;
    }
    std::vector<JumpTableValueOccurrence> &Known = It->second;
    if (!StructuralBudget.consume(Known.size()))
      return false;
    if (std::find(Known.begin(), Known.end(), Producer) != Known.end())
      return true;
    if (!StructuralBudget.consume(2))
      return false;
    Known.push_back(Producer);
    return true;
  };
  auto appendProvenProducer = [&](uint32_t Bound,
                                  const JumpTableValueOccurrence &Producer) {
    return appendUniqueProducer(ProvenProducers, Bound, Producer);
  };
  auto appendReplayableExactProducer =
      [&](uint32_t Bound, const JumpTableValueOccurrence &Producer) {
        return appendUniqueProducer(ReplayableExactProducers, Bound, Producer);
      };
  // This account bounds an under-approximate authenticated-producer prefix.
  // If it fills, omitted producers are absent from the allowed set; final
  // full-CFG MustEqual/MayDepend replay then rejects any selector path that
  // needs one, so prefix exhaustion cannot manufacture a domain.
  bool StructuralRetentionFull = false;
  auto retainExactModuloQuery =
      [&](uint32_t Bound, const JumpTableValueOccurrence &Producer) {
        // Prepay metadata, public query, result slot, and eventual
        // authenticated alternative.  The relation carries no alternatives
        // and shares one structural-symbolization budget for the whole batch.
        if (!StructuralBudget.canConsume(4) ||
            !StructuralBudget.consume(4))
          return false;
        JumpTableValueQuery Query;
        Query.Candidate = Producer.Value;
        Query.UseAddr = Producer.Addr;
        Query.UseSeq = Producer.Seq;
        Query.Relation = JumpTableValueRelation::ExactUnsignedModuloRecipe;
        Query.UnsignedUpperBound = Bound;
        StructuralModuloProducers.push_back({Bound, Producer});
        StructuralModuloQueries.push_back(std::move(Query));
        return true;
      };
  for (auto CandidateIt = ProducerCandidates.begin();
       CandidateIt != ProducerCandidates.end(); ++CandidateIt) {
    const auto &[Bound, Producers] = *CandidateIt;
    if (!StructuralBudget.consume()) {
      StructuralRetentionFull = true;
      break;
    }
    for (const ModuloProducerCandidate &Candidate : Producers) {
      if (!StructuralBudget.consume()) {
        StructuralRetentionFull = true;
        break;
      }
      const JumpTableValueOccurrence &Producer = Candidate.Occurrence;
      const std::optional<ExactDivisionRemainderShape> Shape =
          exactDivisionRemainderShape(Candidate.DefinitionIndex, Bound);
      if (Shape) {
        if (!StructuralBudget.canConsume() || !StructuralBudget.consume()) {
          StructuralRetentionFull = true;
          break;
        }
        if (Shape->IsUnsigned) {
          if (Shape->ExactRelationReplayable) {
            if (!retainExactModuloQuery(Bound, Producer)) {
              StructuralRetentionFull = true;
              break;
            }
          } else if (!appendProvenProducer(Bound, Producer)) {
            StructuralRetentionFull = true;
            break;
          }
        } else {
          if (!StructuralBudget.consume()) {
            StructuralRetentionFull = true;
            break;
          }
          ConditionalSignedProducers.push_back(
              {Bound, Producer, Shape->NumeratorUse});
        }
      } else {
        // clang normally lowers constant unsigned division to a reciprocal
        // multiply/shift recipe rather than INT_DIV; an O0 target can retain
        // URem directly.  Ask the shared point-sensitive resolver to
        // authenticate either exact structure at this output occurrence.
        if (!retainExactModuloQuery(Bound, Producer)) {
          StructuralRetentionFull = true;
          break;
        }
      }
      if (StructuralBudget.exhausted()) {
        StructuralRetentionFull = true;
        break;
      }
    }
    if (StructuralRetentionFull)
      break;
  }
  bool StructuralAnalysisComplete = StructuralModuloQueries.empty();
  if (!StructuralModuloQueries.empty()) {
    StructuralAnalysisComplete = false;
    const std::vector<bool> StructuralResults = tableValuesMatchAtUses(
        StructuralModuloQueries, &StructuralAnalysisComplete, nullptr,
        CandidateTargetsOverride ? Rec.Addr : InvalidVA,
        CandidateTargetsOverride, AggregateEvidenceBudget);
    // This is the resolver's shared expression-symbolization budget for the
    // already retained exact queries, distinct from StructuralRetentionFull's
    // sound producer-prefix under-approximation above.  If expression analysis
    // of that retained batch is incomplete, reject the whole batch so success
    // cannot depend on attacker-controlled candidate order.
    if (!StructuralAnalysisComplete ||
        StructuralResults.size() != StructuralModuloProducers.size()) {
      ExplicitlyIncomplete = true;
      return false;
    }
    for (size_t I = 0; I < StructuralModuloProducers.size(); ++I) {
      if (!StructuralResults[I])
        continue;
      if (!appendProvenProducer(StructuralModuloProducers[I].Bound,
                                StructuralModuloProducers[I].Producer) ||
          !appendReplayableExactProducer(
              StructuralModuloProducers[I].Bound,
              StructuralModuloProducers[I].Producer)) {
        StructuralRetentionFull = true;
        break;
      }
    }
  }

  // A signed `x % N` is not a range certificate: negative x produces a
  // negative remainder.  It may nevertheless sit at a shared dispatch whose
  // frame PHI is initialized by a literal coordinate and updated exclusively
  // by the unsigned producers above.  Admit that signed normalization only
  // after proving its exact numerator occurrence equals an already-bounded
  // producer or a coordinate in [0,N).  This is an inductive closure over the
  // real CFG, not permission derived from signed division itself.
  struct ConditionalSignedProof {
    ConditionalSignedProducer Producer;
  };
  std::vector<ConditionalSignedProof> ConditionalSignedProofs;
  std::vector<JumpTableValueQuery> ConditionalSignedQueries;
  for (const ConditionalSignedProducer &Conditional :
       ConditionalSignedProducers) {
    if (!StructuralBudget.consume(
            orderedEvidenceLookupWork(ProvenProducers.size()) + 1)) {
      StructuralRetentionFull = true;
      break;
    }
    const auto Known = ProvenProducers.find(Conditional.Bound);
    const size_t ProducerCount =
        Known == ProvenProducers.end() ? 0 : Known->second.size();
    const std::optional<size_t> ThisWork = moduloDomainEvidenceWork(
        Conditional.Bound, ProducerCount, /*OccurrenceCount=*/1,
        /*RequireProducerReachability=*/false);
    if (!ThisWork || *ThisWork == std::numeric_limits<size_t>::max() ||
        !StructuralBudget.canConsume(*ThisWork + 1))
      continue;
    if (!StructuralBudget.consume(*ThisWork + 1))
      continue;

    JumpTableValueQuery Query;
    Query.Candidate = Conditional.NumeratorUse.Value;
    Query.UseAddr = Conditional.NumeratorUse.Addr;
    Query.UseSeq = Conditional.NumeratorUse.Seq;
    Query.Alternatives.reserve(ProducerCount + size_t(Conditional.Bound) * 2);
    if (Known != ProvenProducers.end())
      Query.Alternatives = Known->second;
    for (uint32_t Coordinate = 0; Coordinate < Conditional.Bound;
         ++Coordinate) {
      Query.Alternatives.push_back(
          {NdVar::cst(Coordinate, Conditional.NumeratorUse.Value.Size),
           InvalidVA, -1, /*DefinedAtPoint=*/false});
      Query.Alternatives.push_back(
          {NdVar::scalar(Coordinate, Conditional.NumeratorUse.Value.Size),
           InvalidVA, -1, /*DefinedAtPoint=*/false});
    }
    Query.AllowZeroExtension = true;
    Query.AllowSignExtension =
        moduloDomainFitsSignedWidth(Conditional.Bound,
                                    Conditional.NumeratorUse.Value.Size) &&
        (Known == ProvenProducers.end() ||
         std::all_of(Known->second.begin(), Known->second.end(),
                     [&](const JumpTableValueOccurrence &Producer) {
                       return moduloDomainFitsSignedWidth(Conditional.Bound,
                                                          Producer.Value.Size);
                     }));
    ConditionalSignedProofs.push_back({Conditional});
    ConditionalSignedQueries.push_back(std::move(Query));
  }
  bool ConditionalSignedAnalysisComplete = false;
  const std::vector<bool> ConditionalSignedResults = tableValuesMatchAtUses(
      ConditionalSignedQueries, &ConditionalSignedAnalysisComplete, nullptr,
      CandidateTargetsOverride ? Rec.Addr : InvalidVA,
      CandidateTargetsOverride, AggregateEvidenceBudget);
  if (!ConditionalSignedQueries.empty() &&
      (!ConditionalSignedAnalysisComplete ||
       ConditionalSignedResults.size() != ConditionalSignedProofs.size()))
    ExplicitlyIncomplete = true;
  if (ConditionalSignedAnalysisComplete &&
      ConditionalSignedResults.size() == ConditionalSignedProofs.size())
    for (size_t I = 0; I < ConditionalSignedProofs.size(); ++I)
      if (ConditionalSignedResults[I]) {
        if (!appendProvenProducer(ConditionalSignedProofs[I].Producer.Bound,
                                  ConditionalSignedProofs[I].Producer.Producer))
          continue;
      }
  std::map<uint32_t, std::vector<JumpTableValueOccurrence>> CandidateProducers =
      std::move(ProvenProducers);
  auto exportAuthenticatedProducers = [&](uint32_t Bound) {
    if (!AuthenticatedProducers)
      return true;
    if (!ReplayBudget.consume(
            orderedEvidenceLookupWork(ReplayableExactProducers.size())))
      return false;
    const auto It = ReplayableExactProducers.find(Bound);
    if (It == ReplayableExactProducers.end())
      return true;
    if (!consumeProducts(ReplayBudget, {{It->second.size(), 2}}))
      return false;
    *AuthenticatedProducers = It->second;
    return true;
  };

  struct PreparedModuloBound {
    uint32_t Bound = 0;
  };
  std::vector<PreparedModuloBound> PreparedBounds;
  for (const auto &[Bound, Producers] : CandidateProducers) {
    if (!ReplayBudget.consume())
      break;
    if (Producers.empty() ||
        (Info.PhysicalCapacity != 0 && Bound > Info.PhysicalCapacity))
      continue;

    // Process bounds in ascending order.  First prepay only the bounded table
    // read plus one scalar candidate record; an exact local INT_REM fast path
    // does not allocate the much larger replay-alternative vectors.
    constexpr size_t PreparedRecord = 1;
    if (size_t(Bound) > std::numeric_limits<size_t>::max() - PreparedRecord ||
        !ReplayBudget.canConsume(size_t(Bound) + PreparedRecord) ||
        !ReplayBudget.consume(size_t(Bound) + PreparedRecord))
      continue;

    // Avoid copying attacker-sized occurrence/role vectors merely to ask the
    // table reader whether this one scalar bound is physically decodable.
    JumpTableInfo Probe;
    Probe.setBaseAddr(Info.BaseAddr);
    Probe.EntrySize = Info.EntrySize;
    Probe.EntryStride = Info.EntryStride;
    Probe.MaxEntries = Bound;
    Probe.IsRelative = Info.IsRelative;
    Probe.IsSigned = Info.IsSigned;
    Probe.EntryScale = Info.EntryScale;
    if (Info.HasTargetBase)
      Probe.setTargetBase(Info.TargetBase);
    if (readTableEntries(Img, Probe).size() != Bound)
      continue;

    bool DirectProof = true;
    for (size_t I = 0; I < IndexOccurrences.size(); ++I)
      if (!provesDirectUnsignedRemainder(I, Bound)) {
        DirectProof = false;
        break;
      }
    if (!DirectProof) {
      // Direct tracing is only a fast path.  Its independent account may run
      // out on a shared selector without consuming the reserved query work.
      const std::optional<size_t> QueryWork = moduloDomainEvidenceWork(
          Bound, Producers.size(), IndexOccurrences.size(),
          RequireProducerReachability);
      // One additional unit prepays the DomainBatch record.  QueryWork
      // already covers public queries, mirrored alternatives, and results.
      if (!QueryWork || *QueryWork == std::numeric_limits<size_t>::max() ||
          !ReplayBudget.canConsume(*QueryWork + 1) ||
          !ReplayBudget.consume(*QueryWork + 1))
        continue;
      PreparedBounds.push_back({Bound});
      continue;
    }
    Info.MaxEntries = Bound;
    Info.IndexDomainAuthenticated = true;
    Info.AuthenticatedModuloBound = Bound;
    Info.NormBase = 0;
    Info.NormShift = 0;
    Info.Stride = 1;
    if (!exportAuthenticatedProducers(Bound))
      return false;
    Succeeded = true;
    return true;
  }
  if (PreparedBounds.empty())
    return false;

  struct DomainBatch {
    uint32_t Bound = 0;
    size_t Begin = 0;
    size_t End = 0;
  };
  std::vector<JumpTableValueQuery> Queries;
  std::vector<DomainBatch> DomainBatches;
  for (const PreparedModuloBound &Prepared : PreparedBounds) {
    const uint32_t Bound = Prepared.Bound;
    if (!ReplayBudget.consume(
            orderedEvidenceLookupWork(CandidateProducers.size()) + 1))
      break;
    const auto Producers = CandidateProducers.find(Bound);
    if (Producers == CandidateProducers.end() || Producers->second.empty())
      continue;
    const size_t ProducerCount = Producers->second.size();
    const size_t Begin = Queries.size();
    for (const JumpTableValueOccurrence &Index : IndexOccurrences) {
      JumpTableValueQuery DomainQuery;
      DomainQuery.Candidate = Index.Value;
      DomainQuery.UseAddr = Index.Addr;
      DomainQuery.UseSeq = Index.Seq;
      DomainQuery.Alternatives.reserve(ProducerCount + size_t(Bound) * 2);
      DomainQuery.Alternatives = Producers->second;
      for (uint32_t Coordinate = 0; Coordinate < Bound; ++Coordinate) {
        DomainQuery.Alternatives.push_back(
            {NdVar::cst(Coordinate, Index.Value.Size), InvalidVA, -1,
             /*DefinedAtPoint=*/false});
        DomainQuery.Alternatives.push_back(
            {NdVar::scalar(Coordinate, Index.Value.Size), InvalidVA, -1,
             /*DefinedAtPoint=*/false});
      }
      DomainQuery.AllowZeroExtension = true;
      DomainQuery.AllowSignExtension =
          moduloDomainFitsSignedWidth(Bound, Index.Value.Size) &&
          std::all_of(Producers->second.begin(), Producers->second.end(),
                      [&](const JumpTableValueOccurrence &Producer) {
                        return moduloDomainFitsSignedWidth(Bound,
                                                           Producer.Value.Size);
                      });
      Queries.push_back(std::move(DomainQuery));

      if (RequireProducerReachability) {
        JumpTableValueQuery DependencyQuery;
        DependencyQuery.Candidate = Index.Value;
        DependencyQuery.UseAddr = Index.Addr;
        DependencyQuery.UseSeq = Index.Seq;
        DependencyQuery.Alternatives.reserve(ProducerCount);
        DependencyQuery.Alternatives = Producers->second;
        DependencyQuery.AllowZeroExtension = true;
        DependencyQuery.AllowSignExtension =
            moduloDomainFitsSignedWidth(Bound, Index.Value.Size) &&
            std::all_of(Producers->second.begin(), Producers->second.end(),
                        [&](const JumpTableValueOccurrence &Producer) {
                          return moduloDomainFitsSignedWidth(
                              Bound, Producer.Value.Size);
                        });
        DependencyQuery.Relation = JumpTableValueRelation::MayDepend;
        Queries.push_back(std::move(DependencyQuery));
      }
    }
    DomainBatches.push_back({Bound, Begin, Queries.size()});
  }
  if (Queries.empty())
    return false;

  std::vector<bool> QueryAnalysisComplete;
  const std::vector<bool> Results = tableValuesMatchAtUses(
      Queries, nullptr, &QueryAnalysisComplete,
      CandidateTargetsOverride ? Rec.Addr : InvalidVA,
      CandidateTargetsOverride, AggregateEvidenceBudget);
  if (Results.size() != Queries.size() ||
      QueryAnalysisComplete.size() != Queries.size()) {
    ExplicitlyIncomplete = true;
    return false;
  }
  if (!ReplayBudget.consume(QueryAnalysisComplete.size()))
    return false;
  const bool SawIncompleteQuery =
      std::any_of(QueryAnalysisComplete.begin(), QueryAnalysisComplete.end(),
                  [](bool Complete) { return !Complete; });
  for (const DomainBatch &Batch : DomainBatches) {
    const size_t QueryStride = RequireProducerReachability ? 2 : 1;
    bool Proven = true;
    bool SawProducer = false;
    for (size_t I = Batch.Begin; Proven && I < Batch.End; I += QueryStride) {
      Proven = QueryAnalysisComplete[I] && Results[I];
      if (RequireProducerReachability) {
        Proven &= QueryAnalysisComplete[I + 1];
        SawProducer |= Results[I + 1];
      }
    }
    if (!Proven || (RequireProducerReachability && !SawProducer))
      continue;
    Info.MaxEntries = Batch.Bound;
    Info.IndexDomainAuthenticated = true;
    Info.AuthenticatedModuloBound = Batch.Bound;
    Info.NormBase = 0;
    Info.NormShift = 0;
    Info.Stride = 1;
    if (!exportAuthenticatedProducers(Batch.Bound))
      return false;
    Succeeded = true;
    return true;
  }
  ExplicitlyIncomplete |= SawIncompleteQuery;
  return false;
}

//===----------------------------------------------------------------------===//
// pullBackBound — adjust a guard bound through normalization operations
//===----------------------------------------------------------------------===//

uint32_t CFGBuilder::pullBackBound(uint32_t RawBound,
                                   const JumpTableInfo &Info) const {
  uint32_t Adjusted = RawBound;

  if (Info.NormBase > 0 && Adjusted > static_cast<uint32_t>(Info.NormBase))
    Adjusted -= static_cast<uint32_t>(Info.NormBase);

  if (Info.NormShift > 0)
    Adjusted >>= Info.NormShift;

  if (Adjusted == 0)
    return RawBound;
  return Adjusted;
}

} // namespace neverd
