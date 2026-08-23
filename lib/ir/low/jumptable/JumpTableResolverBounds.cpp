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
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cstdint>
#include <functional>
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
/// Img.CodePtrRelocSlots; the run length is the exact entry count of a
/// computed-goto / threaded-dispatch jump table.
uint32_t countCodePtrRelocRun(const BinaryImage &Img, va_t TableAddr,
                              uint64_t EntryStride) {
  if (EntryStride == 0 || Img.CodePtrRelocSlots.empty())
    return 0;
  uint32_t Run = 0;
  va_t VA = TableAddr;
  while (Run < limits::kMaxJumpTableEntries) {
    if (!Img.CodePtrRelocSlots.count(VA))
      break;
    ++Run;
    if (EntryStride > InvalidVA - VA)
      break;
    VA += EntryStride;
  }
  return Run;
}

uint32_t boundCodePtrRunByNextAnchor(const BinaryImage &Img, va_t BaseAddr,
                                     uint64_t EntryStride, uint32_t Run,
                                     const std::set<va_t> &DecodedAnchors) {
  if (EntryStride == 0 || Run == 0)
    return Run;

  std::set<va_t> Anchors = Img.RelCodeTableAnchors;
  Anchors.insert(DecodedAnchors.begin(), DecodedAnchors.end());
  for (const auto &[FieldVA, Field] : Img.DataAddressRelocOperands)
    if (!Field.PCRelativeFromInstructionEnd &&
        Img.hasExecutableCodeOwnerAt(FieldVA) && Field.TargetVA != InvalidVA)
      Anchors.insert(Field.TargetVA);

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
/// at the table base — the entries of a PIC `switch` jump table.  The run
/// length is the exact entry count, which bounds a `switch(x % N)` table whose
/// modulus constrains the index with no `cmp` range guard.
uint32_t countRelCodeRelocRun(const BinaryImage &Img, va_t TableAddr,
                              uint64_t EntryStride) {
  if (EntryStride == 0 || Img.RelCodeRelocSlots.empty())
    return 0;
  uint32_t Run = 0;
  va_t VA = TableAddr;
  while (Run < limits::kMaxJumpTableEntries) {
    if (!Img.RelCodeRelocSlots.count(VA))
      break;
    ++Run;
    if (EntryStride > InvalidVA - VA)
      break;
    VA += EntryStride;
  }
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

/// Fold `V` (or its COPY/ZEXT/SEXT chain) to a constant value, if any.  Used to
/// read a modulus that was materialised in a register (`mov wN,#11; msub`) as
/// the back-multiply constant, not just an immediate operand.
static std::optional<int64_t> constValueOf(const std::vector<LowOp> &Ops,
                                           int FromIdx, NdVar V,
                                           int Depth = 0) {
  if (V.isConst()) {
    // Constants are only proposal syntax here.  Do not reinterpret an
    // unsigned high-bit bit-pattern through an implementation-defined host
    // uint64_t->int64_t conversion; an unrepresentable coefficient simply
    // contributes no modulus proposal.
    if (V.Offset > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
      return std::nullopt;
    return static_cast<int64_t>(V.Offset);
  }
  if ((!V.isReg() && !V.isTemp()) || Depth > limits::kMaxQuasiCopyDepth)
    return std::nullopt;
  int D = reachingDefIdx(Ops, FromIdx, V);
  if (D < 0)
    return std::nullopt;
  const LowOp &Op = Ops[D];
  if ((Op.Opcode == NdOp::COPY || Op.Opcode == NdOp::INT_ZEXT ||
       Op.Opcode == NdOp::INT_SEXT) &&
      Op.NumInputs >= 1)
    return constValueOf(Ops, D - 1, Op.Inputs[0], Depth + 1);
  return std::nullopt;
}

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
static bool evalLinearMultiple(const std::vector<LowOp> &Ops, int FromIdx,
                               NdVar V, int Depth, int64_t &Coef) {
  if (Depth > limits::kMaxModuloDecompDepth)
    return false;
  if (!V.isReg() && !V.isTemp())
    return false;
  int D = reachingDefIdx(Ops, FromIdx, V);
  if (D < 0) {
    Coef = 1; // No definition in the slice: the base itself.
    return true;
  }
  const LowOp &Op = Ops[D];
  auto isVar = [](const NdVar &X) { return X.isReg() || X.isTemp(); };
  auto checkedCoefficient = [](llvm::APInt Value, int64_t &Out) -> bool {
    if (!Value.isSignedIntN(64))
      return false;
    Out = Value.trunc(64).getSExtValue();
    return true;
  };
  auto wideSigned = [](int64_t Value) {
    return llvm::APInt(64, static_cast<uint64_t>(Value),
                       /*isSigned=*/false, /*implicitTrunc=*/true)
        .sext(128);
  };
  switch (Op.Opcode) {
  case NdOp::COPY:
  case NdOp::INT_ZEXT:
  case NdOp::INT_SEXT:
    if (Op.NumInputs >= 1 && isVar(Op.Inputs[0]))
      return evalLinearMultiple(Ops, D - 1, Op.Inputs[0], Depth + 1, Coef);
    Coef = 1; // COPY of a constant: a materialised base.
    return true;
  case NdOp::SUBBYTES:
    if (Op.NumInputs >= 2 && Op.Inputs[1].isConst() &&
        Op.Inputs[1].Offset == 0 && isVar(Op.Inputs[0]))
      return evalLinearMultiple(Ops, D - 1, Op.Inputs[0], Depth + 1, Coef);
    return false;
  case NdOp::INT_LEFT:
    if (Op.NumInputs >= 2 && Op.Inputs[1].isConst() &&
        Op.Inputs[1].Offset < 32 && isVar(Op.Inputs[0])) {
      int64_t C;
      if (!evalLinearMultiple(Ops, D - 1, Op.Inputs[0], Depth + 1, C))
        return false;
      return checkedCoefficient(wideSigned(C).shl(Op.Inputs[1].Offset), Coef);
    }
    return false;
  case NdOp::INT_MULT: {
    // base * const, where the const may be an immediate or a register/temp that
    // resolves to one (e.g. `msub` with the modulus in a register).
    for (int CK = 0; CK < Op.NumInputs && CK < 2; ++CK) {
      int BK = 1 - CK;
      if (BK >= Op.NumInputs || !isVar(Op.Inputs[BK]))
        continue;
      std::optional<int64_t> C = constValueOf(Ops, D - 1, Op.Inputs[CK]);
      if (!C)
        continue;
      int64_t Bc;
      if (!evalLinearMultiple(Ops, D - 1, Op.Inputs[BK], Depth + 1, Bc))
        return false;
      return checkedCoefficient(wideSigned(Bc) * wideSigned(*C), Coef);
    }
    Coef = 1; // q*recip (the magic quotient itself): base.
    return true;
  }
  case NdOp::INT_ADD:
  case NdOp::INT_SUB:
    if (Op.NumInputs >= 2 && isVar(Op.Inputs[0]) && isVar(Op.Inputs[1])) {
      int64_t A, B;
      if (!evalLinearMultiple(Ops, D - 1, Op.Inputs[0], Depth + 1, A) ||
          !evalLinearMultiple(Ops, D - 1, Op.Inputs[1], Depth + 1, B))
        return false;
      const llvm::APInt Result = Op.Opcode == NdOp::INT_ADD
                                     ? wideSigned(A) + wideSigned(B)
                                     : wideSigned(A) - wideSigned(B);
      return checkedCoefficient(Result, Coef);
    }
    return false;
  default:
    Coef = 1; // Quotient produced by a non-multiplier op (shift/divide): base.
    return true;
  }
}

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
    std::vector<uint32_t> *FeasibleCoordinates) const {
  if (IncompleteIndexDomain)
    *IncompleteIndexDomain = false;
  if (UsedNonContiguous)
    *UsedNonContiguous = false;
  if (FeasibleCoordinates)
    FeasibleCoordinates->clear();
  std::vector<JumpTableValueOccurrence> IndexOccurrences =
      Info.IndexValueAlternatives;
  if (IndexOccurrences.empty() && Info.IndexValueAtUse.Size != 0 &&
      Info.IndexUseAddr != InvalidVA && Info.IndexUseSeq >= 0)
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

  std::vector<LowOp> Ops;
  for (auto It = Insns.lower_bound(CurrentFuncEntry);
       It != Insns.end() && It->first <= Rec.Addr; ++It)
    for (auto &Op : It->second.Ops)
      Ops.push_back(Op);

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
  size_t WorkBudget = limits::kMaxJumpTableEvidenceWork;
  auto FailIncomplete = [&]() -> uint32_t {
    if (IncompleteIndexDomain)
      *IncompleteIndexDomain = true;
    return 0;
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
    if (WorkBudget == 0)
      return FailIncomplete();
    --WorkBudget;
    const LowOp &Op = Ops[I];
    if (Op.Opcode != NdOp::INT_AND || Op.NumInputs < 2 || Op.Output.Size == 0)
      continue;
    RelevantMaskRoots.push_back(
        {Op.Output, Op.Addr, Op.Seq, /*DefinedAtPoint=*/true});
    const auto InsnIt = Insns.find(Op.Addr);
    if (InsnIt == Insns.end())
      continue;
    for (int ConstantSide = 0; ConstantSide < 2; ++ConstantSide) {
      if (WorkBudget == 0)
        return FailIncomplete();
      --WorkBudget;
      const NdVar &MaskOperand = Op.Inputs[ConstantSide];
      const NdVar &DynamicOperand = Op.Inputs[1 - ConstantSide];
      const std::optional<int64_t> Proposed =
          constValueOf(Ops, I - 1, MaskOperand);
      const std::optional<uint64_t> ConstantWidthMask =
          integerWidthMask(MaskOperand.Size);
      if (!Proposed || !ConstantWidthMask)
        continue;
      // INT_AND coerces both inputs to the output width.  The encoded constant
      // is zero-extended/truncated to that width, and a narrower dynamic input
      // cannot make the newly introduced high bits nonzero.  Deriving the
      // domain from the constant operand's own width would over-count an i8
      // output masked by i16(0x1ff), while ignoring the dynamic width would
      // over-count zext(i8) & i16(0x1ff).
      const std::optional<uint64_t> EffectiveMask = effectiveIntegerAndMask(
          static_cast<uint64_t>(*Proposed), MaskOperand.Size,
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
      if (DomainRepresentable)
        for (uint32_t Value = 0; Value <= static_cast<uint32_t>(Mask); ++Value)
          if ((Value & ~static_cast<uint32_t>(Mask)) == 0)
            Coordinates.push_back(Value);

      JumpTableValueQuery ConstantProof;
      ConstantProof.Candidate = MaskOperand;
      ConstantProof.UseAddr = Op.Addr;
      ConstantProof.UseSeq = Op.Seq;
      ConstantProof.Alternatives.push_back(
          {NdVar::scalar(static_cast<uint64_t>(*Proposed) & *ConstantWidthMask,
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
  ConstantQueries.reserve(Candidates.size());
  for (const MaskCandidate &Candidate : Candidates)
    ConstantQueries.push_back(Candidate.ConstantProof);
  std::vector<bool> ConstantsMatch;
  if (!ConstantQueries.empty()) {
    bool ConstantAnalysisComplete = false;
    ConstantsMatch =
        tableValuesMatchAtUses(ConstantQueries, &ConstantAnalysisComplete);
    if (!ConstantAnalysisComplete || ConstantsMatch.size() != Candidates.size())
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
        ByBound[Candidates[I].Bound].push_back(Candidates[I].Output);
        mergeCoordinates(Candidates[I].Bound, Candidates[I].Coordinates);
        BoundUsesNonContiguous[Candidates[I].Bound] |=
            Candidates[I].NonContiguous;
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
    };
    std::vector<AndIntersectionCandidate> AndCandidates;
    for (size_t CandidateIndex = 0; CandidateIndex < Candidates.size();
         ++CandidateIndex) {
      const MaskCandidate &Candidate = Candidates[CandidateIndex];
      if (!ConstantsMatch[CandidateIndex] || !Candidate.CompleteDomain)
        continue;
      for (const auto &[InputBound, Producers] : ByBound) {
        if (WorkBudget == 0)
          return FailIncomplete();
        --WorkBudget;
        std::set<uint32_t> OutputSet;
        for (uint32_t Value : BoundCoordinates[InputBound])
          OutputSet.insert(Value & Candidate.Mask);
        if (OutputSet.empty())
          continue;
        std::vector<uint32_t> OutputCoordinates(OutputSet.begin(),
                                                OutputSet.end());
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
        AndCandidates.push_back(
            {Candidate.Output, OutputBound, std::move(OutputCoordinates),
             Candidate.NonContiguous || OutputSet.size() != OutputBound ||
                 BoundUsesNonContiguous[InputBound],
             std::move(InputProof)});
      }
    }
    if (!AndCandidates.empty()) {
      std::vector<JumpTableValueQuery> AndQueries;
      AndQueries.reserve(AndCandidates.size());
      for (const AndIntersectionCandidate &Candidate : AndCandidates) {
        if (WorkBudget == 0)
          return FailIncomplete();
        --WorkBudget;
        AndQueries.push_back(Candidate.InputProof);
      }
      bool AndAnalysisComplete = false;
      const std::vector<bool> AndMatches =
          tableValuesMatchAtUses(AndQueries, &AndAnalysisComplete);
      if (!AndAnalysisComplete || AndMatches.size() != AndQueries.size())
        return FailIncomplete();
      for (size_t I = 0; I < AndCandidates.size(); ++I) {
        if (!AndMatches[I])
          continue;
        const AndIntersectionCandidate &Candidate = AndCandidates[I];
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
        }
        BoundUsesNonContiguous[Candidate.Bound] |= Candidate.UsesNonContiguous;
        Added |= NewOutput || DomainChanged;
      }
    }

    std::vector<OffsetCandidate> OffsetCandidates;
    for (int I = 0; I < static_cast<int>(Ops.size()); ++I) {
      if (WorkBudget == 0)
        return FailIncomplete();
      --WorkBudget;
      const LowOp &Op = Ops[I];
      if ((Op.Opcode != NdOp::INT_ADD && Op.Opcode != NdOp::INT_SUB) ||
          Op.NumInputs < 2 || Op.Output.Size == 0)
        continue;
      const auto InsnIt = Insns.find(Op.Addr);
      if (InsnIt == Insns.end() || InsnIt->second.IsInstructionGuard ||
          conditionalMergeOutput(Op, InsnIt->second))
        continue;

      int ConstantSide = -1;
      if (Op.Opcode == NdOp::INT_ADD) {
        if (constValueOf(Ops, I - 1, Op.Inputs[1]))
          ConstantSide = 1;
        else if (constValueOf(Ops, I - 1, Op.Inputs[0]))
          ConstantSide = 0;
      } else if (constValueOf(Ops, I - 1, Op.Inputs[1])) {
        // Only `dynamic - constant` preserves one contiguous translated
        // domain.  A constant-minus-dynamic expression remains visible to the
        // may-depend proof and therefore fails closed below.
        ConstantSide = 1;
      }
      if (ConstantSide < 0)
        continue;
      const int DynamicSide = 1 - ConstantSide;
      const NdVar &ConstantOperand = Op.Inputs[ConstantSide];
      const std::optional<int64_t> Proposed =
          constValueOf(Ops, I - 1, ConstantOperand);
      const std::optional<uint64_t> ConstantMask =
          integerWidthMask(ConstantOperand.Size);
      if (!Proposed || !ConstantMask)
        continue;
      std::optional<int64_t> Delta =
          coercedSignedConstant(static_cast<uint64_t>(*Proposed),
                                ConstantOperand.Size, Op.Output.Size);
      if (!Delta)
        continue;
      if (Op.Opcode == NdOp::INT_SUB) {
        if (*Delta == std::numeric_limits<int64_t>::min())
          continue;
        *Delta = -*Delta;
      }

      for (const auto &[InputBound, Producers] : ByBound) {
        if (WorkBudget == 0)
          return FailIncomplete();
        --WorkBudget;
        if (*Delta < 0)
          continue;
        const std::optional<uint64_t> OutputMask =
            integerWidthMask(Op.Output.Size);
        if (!OutputMask)
          continue;
        std::vector<uint32_t> OutputCoordinates;
        bool DomainComplete = true;
        for (uint32_t Value : BoundCoordinates[InputBound]) {
          if (static_cast<uint64_t>(*Delta) > *OutputMask - Value) {
            DomainComplete = false;
            break;
          }
          const uint64_t Result = Value + static_cast<uint64_t>(*Delta);
          if (Result >= limits::kMaxJumpTableEntries) {
            DomainComplete = false;
            break;
          }
          OutputCoordinates.push_back(static_cast<uint32_t>(Result));
        }
        if (!DomainComplete ||
            OutputCoordinates.size() < limits::kMinJumpTableEntries)
          continue;
        std::sort(OutputCoordinates.begin(), OutputCoordinates.end());
        OutputCoordinates.erase(
            std::unique(OutputCoordinates.begin(), OutputCoordinates.end()),
            OutputCoordinates.end());
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
            {NdVar::scalar(static_cast<uint64_t>(*Proposed) & *ConstantMask,
                           ConstantOperand.Size),
             InvalidVA, -1, false});
        JumpTableValueQuery InputProof;
        InputProof.Candidate = Op.Inputs[DynamicSide];
        InputProof.UseAddr = Op.Addr;
        InputProof.UseSeq = Op.Seq;
        InputProof.Alternatives = Producers;
        InputProof.AllowZeroExtension = true;
        OffsetCandidates.push_back(
            {{Op.Output, Op.Addr, Op.Seq, /*DefinedAtPoint=*/true},
             OutputBound,
             std::move(OutputCoordinates),
             BoundUsesNonContiguous[InputBound],
             std::move(ConstantProof),
             std::move(InputProof)});
      }
    }

    if (!OffsetCandidates.empty()) {
      std::vector<JumpTableValueQuery> OffsetQueries;
      OffsetQueries.reserve(OffsetCandidates.size() * 2);
      for (const OffsetCandidate &Candidate : OffsetCandidates) {
        if (WorkBudget < 2)
          return FailIncomplete();
        WorkBudget -= 2;
        OffsetQueries.push_back(Candidate.ConstantProof);
        OffsetQueries.push_back(Candidate.InputProof);
      }
      bool OffsetAnalysisComplete = false;
      const std::vector<bool> OffsetMatches =
          tableValuesMatchAtUses(OffsetQueries, &OffsetAnalysisComplete);
      if (!OffsetAnalysisComplete ||
          OffsetMatches.size() != OffsetQueries.size())
        return FailIncomplete();
      for (size_t I = 0; I < OffsetCandidates.size(); ++I)
        if (OffsetMatches[I * 2] && OffsetMatches[I * 2 + 1]) {
          const OffsetCandidate &Candidate = OffsetCandidates[I];
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
    std::vector<uint32_t> Coordinates;
  };
  std::vector<BoundBatch> Batches;
  for (const auto &[Bound, Producers] : ByBound) {
    if (WorkBudget < IndexOccurrences.size())
      return FailIncomplete();
    WorkBudget -= IndexOccurrences.size();
    const size_t Begin = IndexQueries.size();
    for (const JumpTableValueOccurrence &Index : IndexOccurrences)
      IndexQueries.push_back({Index.Value, Index.Addr, Index.Seq, Producers,
                              /*AllowZeroExtension=*/true,
                              /*AllowSignExtension=*/false});
    Batches.push_back(
        {Bound,
         Begin,
         IndexQueries.size(),
         BoundUsesNonContiguous[Bound],
         {BoundCoordinates[Bound].begin(), BoundCoordinates[Bound].end()}});
  }
  bool IndexAnalysisComplete = false;
  const std::vector<bool> Matches =
      tableValuesMatchAtUses(IndexQueries, &IndexAnalysisComplete);
  if (!IndexQueries.empty() && !IndexAnalysisComplete)
    return FailIncomplete();
  if (Matches.size() != IndexQueries.size())
    return FailIncomplete();
  for (const BoundBatch &Batch : Batches)
    if (std::all_of(Matches.begin() + Batch.Begin, Matches.begin() + Batch.End,
                    [](bool Match) { return Match; })) {
      if (UsedNonContiguous)
        *UsedNonContiguous = Batch.UsesNonContiguous;
      if (FeasibleCoordinates)
        *FeasibleCoordinates = Batch.Coordinates;
      return Batch.Bound;
    }

  // A shared dispatch can have an initial constant selector and masked loop
  // backedges.  The merged value is then not equal to the mask producer on
  // every predecessor even though its complete domain is still [0, Bound).
  // Admit that shape only for an exact dense zero-based coordinate set, and
  // prove the actual selector at every occurrence with the CFG/lane-aware
  // bit-vector solver.  Sparse masks and translated domains must keep their
  // stronger occurrence relation; physical table capacity is never a domain
  // certificate.
  struct DenseBoundBatch {
    const BoundBatch *Batch = nullptr;
    size_t Begin = 0;
    size_t End = 0;
  };
  std::vector<JumpTableValueQuery> DenseBoundQueries;
  std::vector<DenseBoundBatch> DenseBoundBatches;
  for (const BoundBatch &Batch : Batches) {
    // This fallback may fill a missing domain proof, tighten an authenticated
    // one, or replay an equal certificate during final validation.  It must
    // never widen a complete guard/modulo domain: incidental low-level masks
    // in a DIV implementation can supply a loose bound (for example 32 for an
    // exact x%5 selector), which is a true less-than fact but not the table's
    // runtime coordinate domain.
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
    if (WorkBudget < IndexOccurrences.size())
      return FailIncomplete();
    WorkBudget -= IndexOccurrences.size();
    const size_t Begin = DenseBoundQueries.size();
    for (const JumpTableValueOccurrence &Index : IndexOccurrences) {
      JumpTableValueQuery Query;
      Query.Candidate = Index.Value;
      Query.UseAddr = Index.Addr;
      Query.UseSeq = Index.Seq;
      Query.Relation = JumpTableValueRelation::UnsignedLessThan;
      Query.UnsignedUpperBound = Batch.Bound;
      DenseBoundQueries.push_back(std::move(Query));
    }
    DenseBoundBatches.push_back({&Batch, Begin, DenseBoundQueries.size()});
  }
  if (!DenseBoundQueries.empty()) {
    bool DenseBoundAnalysisComplete = false;
    const std::vector<bool> DenseBoundMatches =
        tableValuesMatchAtUses(DenseBoundQueries, &DenseBoundAnalysisComplete);
    if (!DenseBoundAnalysisComplete ||
        DenseBoundMatches.size() != DenseBoundQueries.size())
      return FailIncomplete();
    for (const DenseBoundBatch &Proof : DenseBoundBatches)
      if (std::all_of(DenseBoundMatches.begin() + Proof.Begin,
                      DenseBoundMatches.begin() + Proof.End,
                      [](bool Match) { return Match; })) {
        if (UsedNonContiguous)
          *UsedNonContiguous = false;
        if (FeasibleCoordinates)
          *FeasibleCoordinates = Proof.Batch->Coordinates;
        return Proof.Batch->Bound;
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
    if (WorkBudget < IndexOccurrences.size())
      return FailIncomplete();
    WorkBudget -= IndexOccurrences.size();
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
        tableValuesMatchAtUses(DependencyQueries, &DependencyAnalysisComplete);
    if (!DependencyAnalysisComplete ||
        Depends.size() != DependencyQueries.size() ||
        std::any_of(Depends.begin(), Depends.end(),
                    [](bool Match) { return Match; }))
      return FailIncomplete();
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
                                       JumpTableInfo &Info) {
  if (!Info.HasBaseAddr || Info.EntrySize == 0)
    return false;
  // The exact occurrence of a pre-scaled selector is a byte coordinate.  An
  // unsigned upper bound alone does not prove slot alignment; those tables are
  // authorized only by the exact mask-coordinate path, which checks both the
  // physical byte limit and coord % EntryStride == 0.
  if (Info.PreScaledIndex)
    return false;

  std::vector<JumpTableValueOccurrence> IndexOccurrences =
      Info.IndexValueAlternatives;
  if (IndexOccurrences.empty() && Info.IndexValueAtUse.Size != 0 &&
      Info.IndexUseAddr != InvalidVA && Info.IndexUseSeq >= 0)
    IndexOccurrences.push_back({Info.IndexValueAtUse, Info.IndexUseAddr,
                                Info.IndexUseSeq, Info.IndexValueDefinedAtUse});
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

  // The syntax trace below proposes modulus candidates only.  It has no
  // authority to publish a range: after discovering a candidate N, one shared
  // CFG/lane-aware resolver session proves the *actual* index occurrence is
  // unsigned-below N for every possible bit pattern.  Consequently a lexical
  // sibling multiply, an unrelated quotient root, or x-y*N can at worst add a
  // failed proposal; none can bound the dispatch.
  std::vector<LowOp> Ops;
  for (auto It = Insns.lower_bound(CurrentFuncEntry); It != Insns.end(); ++It)
    for (auto &Op : It->second.Ops)
      Ops.push_back(Op);

  auto sameVar = [](const NdVar &A, const NdVar &B) {
    return A.Space == B.Space && A.Offset == B.Offset && A.Size == B.Size;
  };
  auto overlaps = [](const NdVar &A, const NdVar &B) {
    if (A.Space != B.Space || A.Size == 0 || B.Size == 0)
      return false;
    if (A.Offset > InvalidVA - A.Size || B.Offset > InvalidVA - B.Size)
      return true;
    return A.Offset < B.Offset + B.Size && B.Offset < A.Offset + A.Size;
  };
  auto proofBlockStart = [&](va_t UseAddr) {
    va_t Start = CurrentFuncEntry;
    if (auto It = BlockStarts.upper_bound(UseAddr); It != BlockStarts.begin()) {
      --It;
      Start = std::max(Start, *It);
    }
    if (ActiveJumpTableProofRoots) {
      const auto &Roots = *ActiveJumpTableProofRoots;
      if (auto It = Roots.upper_bound(UseAddr); It != Roots.begin()) {
        --It;
        Start = std::max(Start, *It);
      }
    }
    return Start;
  };
  auto localDef = [&](int From, const NdVar &Value,
                      va_t BlockStart) -> std::optional<int> {
    for (int I = From; I >= 0; --I) {
      const LowOp &Op = Ops[I];
      if (Op.Addr < BlockStart)
        break;
      if ((Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL) &&
          Value.isReg())
        return std::nullopt;
      if (!overlaps(Op.Output, Value))
        continue;
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
  std::function<std::optional<uint64_t>(int, NdVar, va_t, unsigned)>
      localConstant = [&](int From, NdVar Value, va_t BlockStart,
                          unsigned Depth) -> std::optional<uint64_t> {
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
    const std::optional<int> D = localDef(From, Value, BlockStart);
    if (!D)
      return std::nullopt;
    const LowOp &Op = Ops[*D];
    if (Op.NumInputs < 1 ||
        (Op.Opcode != NdOp::COPY && Op.Opcode != NdOp::INT_ZEXT))
      return std::nullopt;
    const std::optional<uint64_t> Input =
        localConstant(*D - 1, Op.Inputs[0], BlockStart, Depth + 1);
    return Input ? std::optional<uint64_t>(*Input & *Mask) : std::nullopt;
  };
  auto provesDirectUnsignedRemainder =
      [&](const JumpTableValueOccurrence &Index, uint32_t Bound) {
        int UsePoint = -1;
        for (int I = 0; I < static_cast<int>(Ops.size()); ++I) {
          const LowOp &Use = Ops[I];
          if (Use.Addr != Index.Addr || Use.Seq != Index.Seq)
            continue;
          if (!std::any_of(Use.Inputs, Use.Inputs + Use.NumInputs,
                           [&](const NdVar &Input) {
                             return sameVar(Input, Index.Value);
                           }))
            continue;
          if (UsePoint >= 0)
            return false;
          UsePoint = I;
        }
        if (UsePoint < 0)
          return false;
        const va_t BlockStart = proofBlockStart(Index.Addr);
        NdVar Value = Index.Value;
        int From = UsePoint - 1;
        for (unsigned Depth = 0; Depth <= limits::kMaxQuasiCopyDepth; ++Depth) {
          const std::optional<int> D = localDef(From, Value, BlockStart);
          if (!D)
            return false;
          const LowOp &Op = Ops[*D];
          if (Op.Opcode == NdOp::INT_REM && Op.NumInputs >= 2 &&
              Op.Output.Size != 0 && Op.Output.Size <= sizeof(uint64_t)) {
            const std::optional<uint64_t> Divisor =
                localConstant(*D - 1, Op.Inputs[1], BlockStart, 0);
            return Divisor && *Divisor == Bound && *Divisor != 0;
          }
          if (Op.NumInputs < 1 ||
              (Op.Opcode != NdOp::COPY && Op.Opcode != NdOp::INT_ZEXT) ||
              (!Op.Inputs[0].isReg() && !Op.Inputs[0].isTemp()) ||
              (Op.Opcode == NdOp::COPY &&
               Op.Inputs[0].Size != Op.Output.Size) ||
              (Op.Opcode == NdOp::INT_ZEXT &&
               Op.Inputs[0].Size >= Op.Output.Size))
            return false;
          Value = Op.Inputs[0];
          From = *D - 1;
        }
        return false;
      };
  // Clang 20 -O0 may spill the remainder to a frame slot before widening it
  // for the table address.  The legacy lexical proposal scan below correctly
  // stops at that LOAD, so use the exact physical relocation run only to
  // propose one bounded divisor.  Capacity has no authority by itself: an
  // exact `x - q*N` producer must first pass the structural recipe theorem,
  // and every real selector occurrence must then equal one of those producers
  // through the full CFG/frame resolver.
  if (Info.PhysicalCapacity >= limits::kMinJumpTableEntries &&
      Info.PhysicalCapacity <= limits::kMaxJumpTableEntries) {
    JumpTableInfo Probe = Info;
    Probe.MaxEntries = 0;
    Probe.RuntimeCaseLabels.clear();
    Probe.RuntimeSlotIndices.clear();
    const uint32_t Bound = Info.PhysicalCapacity;
    const size_t ReadableEntries = readTableEntries(Img, Probe).size();
    if (ReadableEntries == Bound) {
      size_t ExactModuloWork = limits::kMaxJumpTableEvidenceWork;
      bool ExactModuloBudgetExhausted = false;
      auto consumeExactModuloWork = [&]() {
        if (ExactModuloWork == 0) {
          ExactModuloBudgetExhausted = true;
          return false;
        }
        --ExactModuloWork;
        return true;
      };
      auto boundedReachingDefIdx = [&](int From, const NdVar &Value) {
        for (int I = From; I >= 0; --I) {
          if (!consumeExactModuloWork())
            return -1;
          const NdVar &Output = Ops[I].Output;
          if (Output.Space == Value.Space && Output.Offset == Value.Offset)
            return I;
        }
        return -1;
      };
      auto rhsHasDirectScalarMultiplier = [&](int From, NdVar Value) -> bool {
        for (unsigned Depth = 0; Depth <= limits::kMaxQuasiCopyDepth; ++Depth) {
          const int Definition = boundedReachingDefIdx(From, Value);
          if (Definition < 0)
            return false;
          const LowOp &Op = Ops[Definition];
          // x86 IMUL writes its native low lane, then the lifter explicitly
          // widens that lane into the architectural container.  The following
          // SUB still consumes the low lane; cross only this preserving write.
          const bool PreservesQueriedLowLane =
              Op.Opcode == NdOp::INT_ZEXT && Op.NumInputs >= 1 &&
              sameVar(Op.Inputs[0], Value) && Op.Output.Space == Value.Space &&
              Op.Output.Offset == Value.Offset && Op.Output.Size > Value.Size;
          if (PreservesQueriedLowLane) {
            From = Definition - 1;
            continue;
          }
          if (Op.Opcode != NdOp::INT_MULT || Op.NumInputs < 2)
            return false;
          for (int ConstantInput = 0; ConstantInput < 2; ++ConstantInput) {
            const int DynamicInput = 1 - ConstantInput;
            const NdVar &Constant = Op.Inputs[ConstantInput];
            if (Constant.isConst() && Constant.Offset == Bound &&
                Constant.Provenance == ConstantAddressProvenance::Scalar &&
                (Op.Inputs[DynamicInput].isReg() ||
                 Op.Inputs[DynamicInput].isTemp()))
              return true;
          }
          return false;
        }
        return false;
      };

      constexpr size_t kMaxExactModuloProducers = 32;
      std::vector<JumpTableValueOccurrence> ProducerCandidates;
      bool TooManyProducers = false;
      for (int I = 0; I < static_cast<int>(Ops.size()); ++I) {
        if (!consumeExactModuloWork())
          break;
        const LowOp &Op = Ops[I];
        if (Op.Opcode != NdOp::INT_SUB || Op.NumInputs < 2 ||
            Op.Output.Size == 0 || (!Op.Output.isReg() && !Op.Output.isTemp()))
          continue;
        const auto RecIt = Insns.find(Op.Addr);
        if (RecIt == Insns.end() || RecIt->second.IsInstructionGuard ||
            !rhsHasDirectScalarMultiplier(I - 1, Op.Inputs[1]))
          continue;
        const JumpTableValueOccurrence Producer{Op.Output, Op.Addr, Op.Seq,
                                                /*DefinedAtPoint=*/true};
        if (std::find(ProducerCandidates.begin(), ProducerCandidates.end(),
                      Producer) != ProducerCandidates.end())
          continue;
        if (ProducerCandidates.size() == kMaxExactModuloProducers) {
          TooManyProducers = true;
          break;
        }
        ProducerCandidates.push_back(Producer);
      }

      if (!ExactModuloBudgetExhausted && !TooManyProducers &&
          !ProducerCandidates.empty()) {
        std::vector<JumpTableValueQuery> StructuralQueries;
        StructuralQueries.reserve(ProducerCandidates.size());
        for (const JumpTableValueOccurrence &Producer : ProducerCandidates) {
          JumpTableValueQuery Query;
          Query.Candidate = Producer.Value;
          Query.UseAddr = Producer.Addr;
          Query.UseSeq = Producer.Seq;
          Query.Relation = JumpTableValueRelation::ExactUnsignedModuloRecipe;
          Query.UnsignedUpperBound = Bound;
          StructuralQueries.push_back(std::move(Query));
        }

        bool StructuralComplete = false;
        const std::vector<bool> StructuralResults =
            tableValuesMatchAtUses(StructuralQueries, &StructuralComplete);
        std::vector<JumpTableValueOccurrence> ProvenProducers;
        if (StructuralComplete &&
            StructuralResults.size() == ProducerCandidates.size())
          for (size_t I = 0; I < StructuralResults.size(); ++I)
            if (StructuralResults[I])
              ProvenProducers.push_back(ProducerCandidates[I]);

        if (!ProvenProducers.empty()) {
          std::vector<JumpTableValueQuery> ReplayQueries;
          ReplayQueries.reserve(IndexOccurrences.size());
          for (const JumpTableValueOccurrence &Index : IndexOccurrences) {
            JumpTableValueQuery Query;
            Query.Candidate = Index.Value;
            Query.UseAddr = Index.Addr;
            Query.UseSeq = Index.Seq;
            Query.Alternatives = ProvenProducers;
            Query.AllowZeroExtension = true;
            Query.RequireExactAlternativeDefinitions = true;
            ReplayQueries.push_back(std::move(Query));
          }
          bool ReplayComplete = false;
          const std::vector<bool> ReplayResults =
              tableValuesMatchAtUses(ReplayQueries, &ReplayComplete);
          if (ReplayComplete && ReplayResults.size() == ReplayQueries.size() &&
              std::all_of(ReplayResults.begin(), ReplayResults.end(),
                          [](bool Match) { return Match; })) {
            Info.MaxEntries = Bound;
            Info.IndexDomainAuthenticated = true;
            Info.AuthenticatedModuloBound = Bound;
            Info.NormBase = 0;
            Info.NormShift = 0;
            Info.Stride = 1;
            return true;
          }
        }
      }
    }
  }

  std::set<uint32_t> CandidateBounds;
  for (const JumpTableValueOccurrence &Index : IndexOccurrences) {
    std::vector<int> UsePoints;
    for (int I = 0; I < static_cast<int>(Ops.size()); ++I) {
      const LowOp &Use = Ops[I];
      if (Use.Addr != Index.Addr || Use.Seq != Index.Seq)
        continue;
      if (std::any_of(
              Use.Inputs, Use.Inputs + Use.NumInputs,
              [&](const NdVar &Input) { return sameVar(Input, Index.Value); }))
        UsePoints.push_back(I);
    }
    if (UsePoints.size() != 1)
      continue;

    NdVar V = Index.Value;
    int From = UsePoints.front() - 1;
    for (int Step = 0; Step < limits::kMaxQuasiCopyDepth; ++Step) {
      const int D = reachingDefIdx(Ops, From, V);
      if (D < 0)
        break;
      const LowOp &Op = Ops[D];
      if ((Op.Opcode == NdOp::COPY || Op.Opcode == NdOp::INT_ZEXT ||
           Op.Opcode == NdOp::INT_SEXT) &&
          Op.NumInputs >= 1 &&
          (Op.Inputs[0].isReg() || Op.Inputs[0].isTemp())) {
        V = Op.Inputs[0];
        From = D - 1;
        continue;
      }
      if (Op.Opcode == NdOp::SUBBYTES && Op.NumInputs >= 2 &&
          Op.Inputs[1].isConst() && Op.Inputs[1].Offset == 0 &&
          (Op.Inputs[0].isReg() || Op.Inputs[0].isTemp())) {
        V = Op.Inputs[0];
        From = D - 1;
        continue;
      }
      if (Op.Opcode == NdOp::INT_REM && Op.NumInputs >= 2 &&
          Op.Output.Size != 0 && Op.Output.Size <= sizeof(uint64_t)) {
        const auto RecIt = Insns.find(Op.Addr);
        if (RecIt == Insns.end() || RecIt->second.IsInstructionGuard)
          break;
        const std::optional<int64_t> Divisor =
            constValueOf(Ops, D - 1, Op.Inputs[1]);
        if (Divisor && *Divisor >= limits::kMinJumpTableEntries &&
            *Divisor <= limits::kMaxJumpTableEntries)
          CandidateBounds.insert(static_cast<uint32_t>(*Divisor));
        break;
      }
      if ((Op.Opcode != NdOp::INT_SUB && Op.Opcode != NdOp::INT_ADD) ||
          Op.NumInputs < 2)
        break;

      for (int Which : {1, 0}) {
        const NdVar &Candidate = Op.Inputs[Which];
        if (!Candidate.isReg() && !Candidate.isTemp())
          continue;
        int64_t Coefficient = 0;
        if (!evalLinearMultiple(Ops, D - 1, Candidate, 0, Coefficient) ||
            Coefficient == std::numeric_limits<int64_t>::min())
          continue;
        const uint64_t Magnitude = Coefficient < 0
                                       ? uint64_t(-(Coefficient + 1)) + 1
                                       : uint64_t(Coefficient);
        if (Magnitude < limits::kMinJumpTableEntries ||
            Magnitude > limits::kMaxJumpTableEntries)
          continue;
        CandidateBounds.insert(static_cast<uint32_t>(Magnitude));
      }
      break;
    }
  }
  if (CandidateBounds.empty())
    return false;
  JumpTableInfo Probe = Info;
  Probe.MaxEntries = 0;
  Probe.RuntimeSlotIndices.clear();
  const size_t ReadableEntries = readTableEntries(Img, Probe).size();
  for (auto It = CandidateBounds.begin(); It != CandidateBounds.end();) {
    const uint32_t Bound = *It;
    if ((Info.PhysicalCapacity != 0 && Bound > Info.PhysicalCapacity) ||
        (Info.PhysicalCapacity == 0 && ReadableEntries < Bound))
      It = CandidateBounds.erase(It);
    else
      ++It;
  }
  if (CandidateBounds.empty() ||
      CandidateBounds.size() >
          limits::kMaxJumpTableEvidenceWork / IndexOccurrences.size())
    return false;

  for (uint32_t Bound : CandidateBounds) {
    if (!std::all_of(IndexOccurrences.begin(), IndexOccurrences.end(),
                     [&](const JumpTableValueOccurrence &Index) {
                       return provesDirectUnsignedRemainder(Index, Bound);
                     }))
      continue;
    Info.MaxEntries = Bound;
    Info.IndexDomainAuthenticated = true;
    Info.AuthenticatedModuloBound = Bound;
    Info.NormBase = 0;
    Info.NormShift = 0;
    Info.Stride = 1;
    return true;
  }

  std::vector<JumpTableValueQuery> Queries;
  Queries.reserve(CandidateBounds.size() * IndexOccurrences.size());
  for (uint32_t Bound : CandidateBounds)
    for (const JumpTableValueOccurrence &Index : IndexOccurrences) {
      JumpTableValueQuery Query;
      Query.Candidate = Index.Value;
      Query.UseAddr = Index.Addr;
      Query.UseSeq = Index.Seq;
      Query.Relation = JumpTableValueRelation::UnsignedLessThan;
      Query.UnsignedUpperBound = Bound;
      Queries.push_back(std::move(Query));
    }

  bool AnalysisComplete = false;
  const std::vector<bool> Results =
      tableValuesMatchAtUses(Queries, &AnalysisComplete);
  if (!AnalysisComplete || Results.size() != Queries.size())
    return false;
  size_t ResultIndex = 0;
  for (uint32_t Bound : CandidateBounds) {
    bool Proven = true;
    for (size_t I = 0; I < IndexOccurrences.size(); ++I)
      Proven &= Results[ResultIndex++];
    if (!Proven)
      continue;
    Info.MaxEntries = Bound;
    Info.IndexDomainAuthenticated = true;
    Info.AuthenticatedModuloBound = Bound;
    Info.NormBase = 0;
    Info.NormShift = 0;
    Info.Stride = 1;
    return true;
  }
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
