//===- JumpTableResolver.cpp - Jump table detection and resolution --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Jump table resolution from indirect branch patterns and metadata
/// extraction into LowFunc::JumpTables.
///
/// The resolver uses a multi-strategy approach with fallback:
///
///   1. **ARM-family detectors** — the architecture-gated recognizers for
///      the ARM TBB/TBH table-branch and the AArch64 compact byte/halfword
///      table.  These are the only target-specific strategies and live in
///      JumpTableResolverARM.cpp; every strategy below is architecture-neutral.
///
///   2. **PIC-relative tables** — handle the common x64 pattern where
///      each table entry is a 32-bit signed offset from the table base:
///        target = base + (int32_t)table[index]
///
///   3. **Symbolic dispatch decomposition** — execute the dispatch with each
///      register input symbolic and recover an exact linear table shape.
///
///   4. **Backward slicing** — trace data flow from the INDIR_BR input
///      through INT_ADD, INT_MULT, LOAD, INT_ZEXT, INT_LEFT, INT_RIGHT,
///      INT_ASHR, INT_SEXT, SUBBYTES, and COPY to identify the base
///      address and entry layout.  Cross-instruction base recovery lives in
///      JumpTableResolverSource.cpp, stack-materialized sources in
///      JumpTableResolverStack.cpp, and composite layouts in
///      JumpTableResolverShapes.cpp.
///
///   5. **Guard analysis** — scan preceding instructions *and* CFG
///      predecessor blocks for comparison/mask ops (INT_LESS,
///      INT_LESSEQUAL, INT_SUB, INT_AND) that bound the switch
///      variable, giving a precise entry count.  Lives in
///      JumpTableResolverGuards.cpp.
///
///   6. **Multi-format entries** — read 1, 2, 4, or 8 byte entries,
///      both signed and unsigned, with tolerance for sparse invalid
///      entries in bounded tables.
///
///   7. **Sanity validation** — each target is checked for executable
///      segment membership, data availability at the target address,
///      reasonable distance from the function, and duplicate-run limits.
///
///   8. **Multi-stage fallback** — when the primary strategy produces
///      too few entries, retry with alternative entry sizes to recover
///      tables that use an unexpected format.  Path collection and dispatch
///      emulation fallbacks live in JumpTableResolverEmu.cpp.
///
/// This file holds the strategy dispatch itself.  The framework pieces it
/// drives are split by responsibility across sibling translation units:
/// backward slicing in JumpTableResolverSlice.cpp, table-base constant folding
/// in JumpTableResolverFold.cpp, index normalization and stride in
/// JumpTableResolverNorm.cpp, guard-free entry-count bounds in
/// JumpTableResolverBounds.cpp, entry decoding and target validation in
/// JumpTableResolverTargets.cpp, and case-label / metadata construction in
/// JumpTableResolverExtract.cpp.
///
/// See CFGBuilder.cpp for the main CFG construction logic.
///
//===----------------------------------------------------------------------===//

#include "JumpTableResolverDetail.h"

#include "neverd/Limits.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/symbolic/SymDispatch.h"

#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <set>
#include <utility>
#include <vector>

#define DEBUG_TYPE "neverd-cfg-builder"

namespace neverd {

//===----------------------------------------------------------------------===//
// ARM-family target detectors (tryARMTableBranch, tryAArch64CompactTable) live
// in JumpTableResolverARM.cpp.  They are the only architecture-gated table
// recognizers; every other strategy -- the guard/bounds analysis in
// JumpTableResolverGuards.cpp, source/stack/shape detectors in their dedicated
// JumpTableResolver*.cpp files, and the framework below -- is pattern-based and
// architecture-neutral, so there is no corresponding x86 detector to split
// out (LLVM target-dispatch pattern).
//===----------------------------------------------------------------------===//

//===----------------------------------------------------------------------===//
// resolveJumpTable — top-level multi-strategy resolution
//===----------------------------------------------------------------------===//

std::set<va_t>
CFGBuilder::jumpTableProofRoots(const JumpTableInfo &Info) const {
  std::set<va_t> Roots = PersistentCFGRoots;
  if (!CurrentImg || RelocationCFGRootSources.empty())
    return Roots;
  const std::set<va_t> DecodedTableAnchors =
      currentRelocatedInstructionTableAnchors(*CurrentImg);

  std::vector<JumpTableStorageRange> CandidateStorage = Info.StorageRanges;
  if (CandidateStorage.empty() && Info.HasBaseAddr && Info.EntrySize != 0 &&
      Info.PhysicalCapacity != 0 && Info.RelocAbsolute) {
    const uint64_t PhysicalStride =
        Info.EntryStride != 0 ? Info.EntryStride : Info.EntrySize;
    if (PhysicalStride >= Info.EntrySize &&
        codePtrRelocRunHasExactBoundary(*CurrentImg, Info.BaseAddr,
                                        PhysicalStride, Info.PhysicalCapacity,
                                        DecodedTableAnchors))
      CandidateStorage.push_back(
          JumpTableStorageRange{Info.BaseAddr, Info.EntrySize, PhysicalStride,
                                Info.PhysicalCapacity});
  }
  if (CandidateStorage.empty())
    return Roots;

  std::set<va_t> CandidateTargets(Info.ExplicitTargets.begin(),
                                  Info.ExplicitTargets.end());
  // Physical ownership and relocation suppression are deliberately separate.
  // A sparse dispatch may own one object containing compiler filler while a
  // reachable second consumer still needs one filler relocation as a CFG root
  // and LLVM pointer-mirror field.  Only the candidate-local allowlist may
  // remove such a root; StorageRanges alone never grants that permission.
  std::set<va_t> SuppressibleSlots(Info.SuppressibleRelocationSlots.begin(),
                                   Info.SuppressibleRelocationSlots.end());
  if (ProtectedJumpTableRelocationSlots)
    for (va_t Slot : *ProtectedJumpTableRelocationSlots)
      SuppressibleSlots.erase(Slot);

  auto ownsWholeRelocationSlot = [&](va_t Slot) {
    const uint32_t PointerSize = CurrentImg->getPointerSize();
    return PointerSize != 0 &&
           std::any_of(
               CandidateStorage.begin(), CandidateStorage.end(),
               [&](const JumpTableStorageRange &Range) {
                 if (Range.EntrySize < PointerSize ||
                     Range.EntryStride < Range.EntrySize ||
                     Range.PhysicalSlotCount == 0 || Slot < Range.BaseAddr)
                   return false;
                 const uint64_t Delta = Slot - Range.BaseAddr;
                 return Delta % Range.EntryStride == 0 &&
                        Delta / Range.EntryStride < Range.PhysicalSlotCount;
               });
  };

  size_t CandidateSlotBudget = limits::kMaxJumpTableEntries;
  for (va_t Slot : SuppressibleSlots) {
    if (CandidateSlotBudget-- == 0 ||
        !CurrentImg->CodePtrRelocSlots.count(Slot) ||
        !ownsWholeRelocationSlot(Slot))
      return Roots;
    const uint8_t *P = CurrentImg->readVA(Slot, CurrentImg->getPointerSize());
    if (!P)
      return Roots;
    CandidateTargets.insert(normalizeCodeAddress(
        readPtr(P, CurrentImg->is64Bit()), CurrentImg->Arch, CurrentImg->Mode));
  }
  if (CandidateTargets.empty())
    return Roots;

  for (const auto &[Target, Sources] : RelocationCFGRootSources) {
    if (DurableCFGRoots.count(Target) || !CandidateTargets.count(Target) ||
        Sources.empty())
      continue;
    if (std::all_of(Sources.begin(), Sources.end(), [&](va_t Slot) {
          return SuppressibleSlots.count(Slot) && ownsWholeRelocationSlot(Slot);
        }))
      Roots.erase(Target);
  }
  return Roots;
}

std::vector<va_t> CFGBuilder::resolveJumpTable(const BinaryImage &Img,
                                               const InsnRecord &Rec) {
  const std::set<va_t> DecodedTableAnchors =
      currentRelocatedInstructionTableAnchors(Img);
  // Preserve the immediately prior proof only for fixed-point revalidation.
  // Target blocks may be added after publication, but the certified table
  // expression and guard occur before the indirect branch and remain unchanged.
  std::optional<JumpTableInfo> PriorInfo;
  if (auto It = ResolvedTableInfo.find(Rec.Addr);
      It != ResolvedTableInfo.end())
    PriorInfo = It->second;
  // A revalidation must never leave metadata from the previously published
  // target set behind when the new proof fails or shrinks.
  ResolvedTableInfo.erase(Rec.Addr);
  RequestedCompleteJumpTableProof = false;
  ActiveJumpTableProofRoots.reset();
  const bool ModuleMutationUnsafe =
      UnsafeJumpTableBranches && UnsafeJumpTableBranches->count(Rec.Addr);
  JumpTableInfo Info;
  auto HasOccurrenceMetadata = [](const JumpTableInfo &Candidate) {
    if (!Candidate.LoadRoles.empty())
      return true;
    const bool HasIndex = !Candidate.IndexValueAlternatives.empty() ||
                          (((Candidate.IndexValueAtUse.isReg() ||
                             Candidate.IndexValueAtUse.isTemp() ||
                             Candidate.IndexValueAtUse.isConst()) &&
                            Candidate.IndexValueAtUse.Size != 0 &&
                            (Candidate.IndexValueAtUse.isConst() ||
                             (Candidate.IndexUseAddr != InvalidVA &&
                              Candidate.IndexUseSeq >= 0))));
    return HasIndex && Candidate.TableLoadAddr != InvalidVA &&
           Candidate.TableLoadSeq >= 0 && !Candidate.TargetLoads.empty();
  };
  auto RejectIncompleteCandidate = [&] {
    Info = JumpTableInfo{};
    return false;
  };
  // Strategies are tried most-specific first, falling back to the generic
  // pattern matchers.  Strategies 1 and 1b are the architecture-gated
  // ARM-family detectors (defined in JumpTableResolverARM.cpp); every strategy
  // from 1c onward is architecture-neutral pattern matching that covers x86,
  // x64, ARM32, and AArch64 alike — which is why there is no x86-specific
  // detector to dispatch to here.

  // Strategy 1: ARM32 TBB/TBH table-branch (most specific, check first).
  bool Recovered = tryARMTableBranch(Img, Rec, Info);
  if (Recovered && !HasOccurrenceMetadata(Info))
    Recovered = RejectIncompleteCandidate();

  // Strategy 1b: AArch64 compact byte/halfword table (separate entry base and
  // code anchor, scaled entries) — must precede the generic relative resolver,
  // which would otherwise latch onto the entry base as the (wrong) target base.
  if (!Recovered)
    Recovered = tryAArch64CompactTable(Img, Rec, Info);
  if (Recovered && !HasOccurrenceMetadata(Info))
    Recovered = RejectIncompleteCandidate();

  // Strategy 1c: runtime-selected table base (`base = cond ? A : B; jmp
  // *base[idx]`) — two adjacent code-pointer tables merged into one.  Must
  // precede the generic relative/cross-instruction resolvers, which would fold
  // only one arm of the select and recover half the table.
  if (!Recovered) {
    Info = JumpTableInfo{};
    Recovered = tryTwoTableSelect(Img, Rec, Info);
    if (Info.CompositeShapeClaimed &&
        (!Recovered || !HasOccurrenceMetadata(Info)))
      return {};
    if (!Recovered)
      Info = JumpTableInfo{};
  }
  if (Recovered && !HasOccurrenceMetadata(Info))
    return {};

  // Strategy 1d: two-level index-byte table (`jmptab[idxtab[switchvar]]`, the
  // classic MSVC sparse-switch lowering).  Must precede the generic
  // relative/cross-instruction resolvers, which would otherwise recover only
  // the inner address table (jmptab) and dispatch on the intermediate table
  // index instead of the real switch variable — collapsing the case set and
  // losing the true labels.  It composes the per-case targets into
  // ExplicitTargets, so it short-circuits the single-base machinery below.
  if (!Recovered) {
    Info = JumpTableInfo{};
    Recovered = tryTwoLevelIndexTable(Img, Rec, Info);
    if (Info.CompositeShapeClaimed &&
        (!Recovered || !HasOccurrenceMetadata(Info)))
      return {};
    if (!Recovered)
      Info = JumpTableInfo{};
  }
  if (Recovered && !HasOccurrenceMetadata(Info))
    return {};
  if (!Recovered && RequestedCompleteJumpTableProof &&
      !JumpTableProofContextComplete)
    return {};

  // Strategy 2: PIC-relative table (architecture-neutral; common on x64).
  if (!Recovered)
    Recovered = tryRelativeTable(Img, Rec, Info);
  if (Recovered && !HasOccurrenceMetadata(Info))
    Recovered = RejectIncompleteCandidate();

  // Strategy 2b: PIC-relative table whose base register is materialised in
  // a preceding instruction (x86 `lea table(%rip),%reg` / ARM32 ADR).  The
  // per-record slice above cannot see the base, so fold it across
  // instructions by emulating the dominating prefix.
  if (!Recovered)
    Recovered = tryCrossInstrRelativeTable(Img, Rec, Info);
  if (Recovered && !HasOccurrenceMetadata(Info))
    Recovered = RejectIncompleteCandidate();

  // Strategy 2c: constant-base absolute table whose load is decoupled from the
  // branch by an -O0 spill/reload relay (`... mov tab(,idx,W),%r; mov
  // %r,[slot];
  // ... mov [slot],%r; jmp *%r`), including a shared multi-site computed-goto
  // dispatch where several goto-site predecessors feed one common table.  The
  // cross-instruction resolver above only reaches a load in the branch's own
  // block or a single-predecessor path, so a many-predecessor shared dispatch
  // reaches none; this recovers the table from the code-pointer relocation run
  // at its constant base regardless of how many goto sites share it.
  if (!Recovered)
    Recovered = tryConstBaseAbsoluteTable(Img, Rec, Info);
  if (Recovered && !HasOccurrenceMetadata(Info))
    Recovered = RejectIncompleteCandidate();

  // Strategy 3: execute the dispatch once with each register input left as the
  // one whole-word unknown.  A successful decomposition is exact: the loaded
  // address itself states the table base, entry width and index stride.  Keep
  // this behind the specialised forms above, whose multi-table and
  // architecture-specific layouts intentionally carry more metadata than one
  // linear expression can describe.
  if (!Recovered) {
    std::set<std::pair<uint64_t, uint16_t>> Candidates;
    for (const LowOp &Op : Rec.Ops)
      for (uint8_t I = 0; I < Op.NumInputs; ++I)
        if (Op.Inputs[I].isReg() && Op.Inputs[I].Size != 0)
          Candidates.emplace(Op.Inputs[I].Offset, Op.Inputs[I].Size);

    std::optional<symbolic::DispatchShape> Shape;
    uint64_t ShapeIndex = InvalidVA;
    bool Ambiguous = false;
    for (const auto &[Reg, Bytes] : Candidates) {
      symbolic::SymContext SymCtx;
      std::optional<symbolic::DispatchShape> Candidate =
          symbolic::analyzeDispatch(SymCtx, Rec.Ops, Reg, Bytes);
      if (!Candidate || Candidate->EntryStride == 0 ||
          Candidate->EntryScale > std::numeric_limits<uint32_t>::max())
        continue;
      if (Shape) {
        Ambiguous = true;
        break;
      }
      Shape = *Candidate;
      ShapeIndex = Reg;
    }

    if (Shape && !Ambiguous) {
      Info.setBaseAddr(Shape->TableBase);
      Info.EntrySize = Shape->EntrySize;
      const bool PreScaled = Shape->EntryStride == 1 && Shape->EntrySize > 1;
      Info.EntryStride = PreScaled ? Shape->EntrySize : Shape->EntryStride;
      Info.IndexReg = ShapeIndex;
      Info.IsRelative = Shape->Kind == symbolic::DispatchKind::Relative;
      Info.IsSigned = Shape->EntryIsSigned;
      if (Info.IsRelative)
        Info.setTargetBase(Shape->RelativeBase);
      Info.EntryScale = static_cast<uint32_t>(Shape->EntryScale);
      Info.PreScaledIndex = PreScaled;
      if (PreScaled)
        Info.Stride = Shape->EntrySize;

      // Bind the symbolic shape back to concrete LowIR occurrences.  A
      // numeric linear decomposition is only a candidate; publication still
      // requires an exact LOAD output-to-branch certificate and an exact
      // base/index address role.  Constant-base direct tables are handled here
      // because analyzeTableLoadAddr deliberately requires a register base.
      for (int I = 0; I < static_cast<int>(Rec.Ops.size()); ++I) {
        const LowOp &Load = Rec.Ops[I];
        if (Load.Opcode != NdOp::LOAD || Load.NumInputs < 1 ||
            Load.Output.Size != Info.EntrySize)
          continue;
        const NdVar &LoadAddress = Load.Inputs[Load.NumInputs >= 2 ? 1 : 0];
        int AddIdx = reachingDefIdx(Rec.Ops, I - 1, LoadAddress);
        for (int Guard = 0;
             AddIdx >= 0 && Rec.Ops[AddIdx].Opcode == NdOp::COPY &&
             Rec.Ops[AddIdx].NumInputs >= 1 &&
             Guard < limits::kMaxQuasiCopyDepth;
             ++Guard)
          AddIdx =
              reachingDefIdx(Rec.Ops, AddIdx - 1, Rec.Ops[AddIdx].Inputs[0]);
        if (AddIdx < 0 || Rec.Ops[AddIdx].Opcode != NdOp::INT_ADD ||
            Rec.Ops[AddIdx].NumInputs < 2)
          continue;
        const LowOp &Add = Rec.Ops[AddIdx];
        for (int BaseSide = 0; BaseSide < 2; ++BaseSide) {
          const NdVar &Base = Add.Inputs[BaseSide];
          if (!Base.isConst() || Base.Offset != Info.BaseAddr)
            continue;
          const NdVar &Dynamic = Add.Inputs[1 - BaseSide];
          NdVar ExactIndex;
          va_t IndexUseAddr = InvalidVA;
          int IndexUseSeq = -1;
          uint64_t IndexReg = InvalidVA;
          uint64_t AddressScale = 1;
          if (PreScaled) {
            ExactIndex = Dynamic;
            IndexUseAddr = Add.Addr;
            IndexUseSeq = Add.Seq;
            IndexReg = traceToRegister(Rec.Ops, AddIdx - 1, Dynamic);
          } else {
            IndexReg = scaledIndexReg(Rec.Ops, AddIdx - 1, Dynamic, &ExactIndex,
                                      &IndexUseAddr, &IndexUseSeq);
            AddressScale = Shape->EntryStride;
          }
          if (IndexReg == InvalidVA || ExactIndex.Size == 0 ||
              IndexUseAddr == InvalidVA || IndexUseSeq < 0)
            continue;

          JumpTableValueOccurrence LoadOccurrence{
              Load.Output, Load.Addr, Load.Seq, /*DefinedAtPoint=*/true};
          JumpTableValueOccurrence IndexOccurrence{ExactIndex, IndexUseAddr,
                                                   IndexUseSeq,
                                                   /*DefinedAtPoint=*/false};
          Info.TargetLoads.push_back(LoadOccurrence);
          JumpTableLoadRole Role;
          Role.Load = LoadOccurrence;
          Role.LoadWidth = Info.EntrySize;
          Role.AllowedBases = {Info.BaseAddr};
          Role.Indices = {IndexOccurrence};
          Role.AddressScale = AddressScale;
          Info.LoadRoles.push_back(std::move(Role));
          if (Info.IndexValueAlternatives.empty()) {
            Info.IndexValueAtUse = ExactIndex;
            Info.IndexUseAddr = IndexUseAddr;
            Info.IndexUseSeq = IndexUseSeq;
            Info.TableLoadAddr = Load.Addr;
            Info.TableLoadSeq = Load.Seq;
            Info.IndexReg = IndexReg;
          }
          Info.IndexValueAlternatives.push_back(IndexOccurrence);
          break;
        }
      }
      Recovered = !Info.TargetLoads.empty() && !Info.LoadRoles.empty();
      LLVM_DEBUG(llvm::dbgs() << "  symbolic-dispatch: table=0x"
                              << llvm::utohexstr(Info.BaseAddr)
                              << " entry=" << Info.EntrySize << " index=0x"
                              << llvm::utohexstr(Info.IndexReg) << "\n");
    }
  }
  if (Recovered && !HasOccurrenceMetadata(Info))
    Recovered = RejectIncompleteCandidate();

  // Strategy 4: Backward slicing for absolute tables.
  if (!Recovered) {
    Recovered = sliceBackForTableBase(Rec, Info);
    if (Recovered && !HasOccurrenceMetadata(Info))
      Recovered = RejectIncompleteCandidate();
  }
  if (!Recovered) {
    return {};
  }

  // Bootstrap occurrence proofs with an explicit, candidate-local relocation
  // allowlist.  This list is provisional: exact runtime-domain recovery and a
  // final reachable-consumer audit below may only remove permissions before
  // publication.  Keeping the permission separate from StorageRanges prevents
  // physical object ownership from silently suppressing a live filler/gap
  // relocation in the CFG or LLVM pointer mirror.
  if (Info.StorageRanges.empty() && Info.RelocAbsolute && Info.HasBaseAddr &&
      Info.EntrySize == Img.getPointerSize() && Info.PhysicalCapacity != 0) {
    const uint64_t PhysicalStride =
        Info.EntryStride != 0 ? Info.EntryStride : Info.EntrySize;
    if (PhysicalStride < Info.EntrySize)
      return {};
    for (uint32_t I = 0; I < Info.PhysicalCapacity; ++I) {
      if (I != 0 && PhysicalStride > (InvalidVA - Info.BaseAddr) / I)
        return {};
      Info.StorageRanges.push_back(
          JumpTableStorageRange{Info.BaseAddr + uint64_t(I) * PhysicalStride,
                                Info.EntrySize, Info.EntrySize, 1});
    }
  }
  if (Info.SuppressibleRelocationSlots.empty()) {
    for (const JumpTableStorageRange &Range : Info.StorageRanges) {
      if (Range.EntrySize < Img.getPointerSize() ||
          Range.EntryStride < Range.EntrySize || Range.PhysicalSlotCount == 0)
        continue;
      for (uint32_t I = 0; I < Range.PhysicalSlotCount; ++I) {
        if (I != 0 && Range.EntryStride > (InvalidVA - Range.BaseAddr) / I)
          return {};
        const va_t Slot = Range.BaseAddr + uint64_t(I) * Range.EntryStride;
        if (Img.CodePtrRelocSlots.count(Slot))
          Info.SuppressibleRelocationSlots.push_back(Slot);
      }
    }
    std::sort(Info.SuppressibleRelocationSlots.begin(),
              Info.SuppressibleRelocationSlots.end());
    Info.SuppressibleRelocationSlots.erase(
        std::unique(Info.SuppressibleRelocationSlots.begin(),
                    Info.SuppressibleRelocationSlots.end()),
        Info.SuppressibleRelocationSlots.end());
  }
  if (ProtectedJumpTableRelocationSlots)
    Info.SuppressibleRelocationSlots.erase(
        std::remove_if(Info.SuppressibleRelocationSlots.begin(),
                       Info.SuppressibleRelocationSlots.end(),
                       [&](va_t Slot) {
                         return ProtectedJumpTableRelocationSlots->count(Slot);
                       }),
        Info.SuppressibleRelocationSlots.end());

  // An incomplete module-arbitration fallback may preserve an unresolved
  // branch only after the detector has established that it is genuinely a
  // local jump-table candidate.  A pattern match alone is insufficient: an
  // indexed callback array has the same LOAD+INDIR_BR shape but its entries
  // are other function entries and must remain an indirect tail call.  Require
  // authenticated physical storage and a complete, untruncated minimum target
  // set whose entries all belong to this function before recording the
  // independent do-not-tailcall identity.
  auto ClaimValidatedPotentialTable = [&] {
    if (!AuthoritativeCurrentFuncRange || !Info.IndexDomainAuthenticated)
      return;
    if (Info.ExplicitTargets.empty() &&
        Info.MaxEntries < limits::kMinJumpTableEntries)
      return;
    size_t PhysicalSlots = 0;
    for (const JumpTableStorageRange &Range : Info.StorageRanges) {
      if (Range.EntrySize == 0 || Range.EntryStride < Range.EntrySize ||
          Range.PhysicalSlotCount == 0 ||
          Range.PhysicalSlotCount >
              limits::kMaxJumpTableEntries - PhysicalSlots)
        return;
      PhysicalSlots += Range.PhysicalSlotCount;
    }
    if (PhysicalSlots < limits::kMinJumpTableEntries)
      return;

    std::vector<va_t> CandidateTargets = Info.ExplicitTargets;
    if (CandidateTargets.empty())
      CandidateTargets = readTableEntries(Img, Info);
    if (CandidateTargets.size() < limits::kMinJumpTableEntries)
      return;
    std::vector<va_t> Checked = CandidateTargets;
    if (!sanityCheckTargets(Img, Checked) ||
        Checked.size() != CandidateTargets.size())
      return;
    for (va_t Target : CandidateTargets) {
      if (!isValidTarget(Img, Target, CurrentFuncEntry))
        return;
      if (Target != CurrentFuncEntry) {
        const bool InAuthoritativeBody =
            AuthoritativeCurrentFuncRange &&
            Target > AuthoritativeCurrentFuncRange->first &&
            Target < AuthoritativeCurrentFuncRange->second;
        if (!InAuthoritativeBody || !isOwnedInteriorTarget(Img, Target))
          return;
      }
    }
    PotentialJumpTableBranches.insert(Rec.Addr);
  };
  ClaimValidatedPotentialTable();

  ActiveJumpTableProofRoots = jumpTableProofRoots(Info);

  // Single-level strategies share the same physical LOAD role once they have
  // produced complete occurrence metadata.  Composite strategies populate
  // their ordered roles themselves.  Do not synthesize a role from a bare
  // register number: an older detector that lacks the exact index use point is
  // intentionally rejected by the publication gate and must defer to a richer
  // strategy.
  if (Info.LoadRoles.empty() && !Info.TwoLevelIndex && !Info.TwoTableSelect &&
      Info.HasBaseAddr && !Info.TargetLoads.empty()) {
    std::vector<JumpTableValueOccurrence> Indices = Info.IndexValueAlternatives;
    if (Indices.empty() &&
        (Info.IndexValueAtUse.isReg() || Info.IndexValueAtUse.isTemp() ||
         Info.IndexValueAtUse.isConst()) &&
        Info.IndexValueAtUse.Size != 0) {
      Indices.push_back({Info.IndexValueAtUse, Info.IndexUseAddr,
                         Info.IndexUseSeq, Info.IndexValueDefinedAtUse});
    }
    const uint64_t PhysicalStride =
        Info.EntryStride != 0 ? Info.EntryStride : Info.EntrySize;
    if (!Indices.empty() && PhysicalStride != 0) {
      for (const JumpTableValueOccurrence &Load : Info.TargetLoads) {
        JumpTableLoadRole Role;
        Role.Load = Load;
        Role.LoadWidth = Info.EntrySize;
        Role.AllowedBases = {Info.BaseAddr};
        Role.Indices = Indices;
        Role.AddressScale = Info.PreScaledIndex ? 1 : PhysicalStride;
        // A logical selector is commonly narrower than the address
        // container (`w`/`edi` -> zext -> pointer-width scale).  The role
        // query still resolves this exact occurrence through the CFG and only
        // accepts an explicit zero extension; it must not require the public
        // selector witness itself to be widened and thereby lose its guard or
        // source-level case-label width.
        Role.AllowZeroExtension =
            std::any_of(Indices.begin(), Indices.end(), [&](const auto &Index) {
              return Index.Value.Size != 0 &&
                     Index.Value.Size < Img.getPointerSize();
            });
        Info.LoadRoles.push_back(std::move(Role));
      }
    }
  }
  if (!Info.TwoLevelIndex && !Info.TwoTableSelect)
    for (JumpTableLoadRole &Role : Info.LoadRoles)
      if (std::any_of(Role.Indices.begin(), Role.Indices.end(),
                      [&](const auto &Index) {
                        return Index.Value.Size != 0 &&
                               Index.Value.Size < Img.getPointerSize();
                      }))
        Role.AllowZeroExtension = true;
  // Shape detection is only a candidate generator.  Before any static,
  // explicit, relocation-bounded, or emulated result can be published, prove
  // that the actual INDIR_BR input is derived from the exact authenticated
  // table LOAD occurrence(s) on every feasible path.  An unrelated prior LOAD
  // at the same addresses, or emulator address co-occurrence, is not evidence.
  const bool TargetRole = branchTargetDependsOnTableLoad(Rec, Info);
  const bool AddressRole = tableLoadAddressesMatchRole(Info);
  if (!TargetRole || !AddressRole) {
    return {};
  }
  Info.MutatedUnsafe |= ModuleMutationUnsafe;

  // A runtime-selected dispatch over two non-adjacent code-pointer tables
  // carries its complete target set explicitly (the union of both runs), which
  // no single-base contiguous read can reconstruct.  Use it verbatim: the
  // guard / normalization / stride / emulation machinery below all assume one
  // contiguous base and would corrupt the two-run layout.  The set is already
  // exact and validated (every entry a resolved in-function code pointer), so
  // the dispatch lowers directly to the merged two-table switch.
  if (!Info.ExplicitTargets.empty()) {
    if (!Info.IndexDomainAuthenticated)
      return {};
    if (!JumpTableProofContextComplete && RequestedCompleteJumpTableProof)
      return {};
    std::vector<va_t> Targets = Info.ExplicitTargets;
    // Every entry was validated during the run read; a sanity-check truncation
    // would desync the concatenated positional labels, so require it to keep
    // the full set rather than emit a mis-aligned switch.
    if (!sanityCheckTargets(Img, Targets) ||
        Targets.size() != Info.ExplicitTargets.size() ||
        Targets.size() < limits::kMinJumpTableEntries)
      return {};
    Info.RequiresCompleteCFGProof = RequestedCompleteJumpTableProof;
    ResolvedTableInfo[Rec.Addr] = Info;
    LLVM_DEBUG(llvm::dbgs()
               << "Jump table @ 0x" << llvm::utohexstr(Rec.Addr) << ": "
               << Targets.size() << " entries ("
               << (Info.TwoLevelIndex ? "two-level index-byte"
                                      : "runtime-selected two-table")
               << ", base=0x" << llvm::utohexstr(Info.BaseAddr) << ")\n");
    return Targets;
  }

  // Detect normalization (INT_SUB base, right-shift) so we can
  // pull back guard bounds and recover case labels later.  A reloc-absolute
  // computed-goto table is indexed directly by `tab[idx]` with idx in [0,N), so
  // the case values are the raw indices 0..N-1 — there is no case-label
  // normalization to invert.  Crucially, the shift in an index expression like
  // `(acc >> k) & 3` is part of *computing* the index, not a table
  // normalization, so running the detectors here would mis-read it as NormShift
  // and emit bogus `i << k` case values that no longer match the runtime index.
  if (!Info.PreScaledIndex) {
    detectNormalization(Rec, Info);

    // Detect stride from AND masks on the switch variable.  When
    // the index has known-zero low bits the effective table size is
    // guard_bound / stride.
    detectStride(Rec, Info);
  }

  // Refine the entry count through one shared proof path.  A bound is accepted
  // only when the compared value is the exact table-index value at that use,
  // the condition has an unambiguous CFG reaching definition, and the branch
  // polarity on the unique edge to the table LOAD is known.  This replaces the
  // old address-ordered scans and same-slot shortcuts, which could disagree on
  // sibling definitions, memory clobbers, or inclusive polarity.  A composite
  // table may already carry an independently authenticated index domain; a
  // relocation run by itself is only physical storage capacity and never
  // skips this search.
  const bool ReusePriorGuardProof =
      PriorInfo && !Rec.JumpTableTargets.empty() &&
      PriorInfo->AuthenticatedGuardBound >= limits::kMinJumpTableEntries &&
      PriorInfo->HasBaseAddr == Info.HasBaseAddr &&
      PriorInfo->BaseAddr == Info.BaseAddr &&
      PriorInfo->EntrySize == Info.EntrySize &&
      PriorInfo->EntryStride == Info.EntryStride &&
      PriorInfo->IsRelative == Info.IsRelative &&
      PriorInfo->IsSigned == Info.IsSigned &&
      PriorInfo->HasTargetBase == Info.HasTargetBase &&
      PriorInfo->TargetBase == Info.TargetBase &&
      PriorInfo->EntryScale == Info.EntryScale &&
      PriorInfo->TableLoadAddr == Info.TableLoadAddr &&
      PriorInfo->TableLoadSeq == Info.TableLoadSeq &&
      PriorInfo->IndexValueAtUse == Info.IndexValueAtUse &&
      PriorInfo->IndexUseAddr == Info.IndexUseAddr &&
      PriorInfo->IndexUseSeq == Info.IndexUseSeq;
  bool GuardFound = Info.IndexDomainAuthenticated;
  if (!GuardFound && ReusePriorGuardProof) {
    Info.MaxEntries = PriorInfo->AuthenticatedGuardBound;
    Info.IndexDomainAuthenticated = true;
    Info.AuthenticatedGuardBound = PriorInfo->AuthenticatedGuardBound;
    GuardFound = true;
  }
  if (!GuardFound) {
    GuardFound = inferBoundsFromPreciseGuards(Rec, Info);
    if (GuardFound) {
      Info.IndexDomainAuthenticated = true;
      Info.AuthenticatedGuardBound = Info.MaxEntries;
    }
  }

  // Record a PIC relative relocation run as physical capacity.  It proves that
  // the occupied slots are relocatable code offsets, but it does not prove the
  // runtime selector domain.  An unguarded `switch(x % N)` is accepted only
  // after the modulo expression below independently authenticates [0, N).
  const uint64_t PhysicalEntryStride =
      Info.EntryStride != 0 ? Info.EntryStride : Info.EntrySize;
  if (!Info.RelocAbsolute && Info.IsRelative && Info.HasBaseAddr &&
      Info.EntrySize > 0) {
    uint32_t RelRun =
        countRelCodeRelocRun(Img, Info.BaseAddr, PhysicalEntryStride);
    // A second unguarded PIC table placed immediately after this one continues
    // the same RelCodeReloc run, so the raw count over-reads into it; cap the
    // run at the next table's base anchor (its exact end).
    RelRun = boundRelRunByNextAnchor(Img, Info.BaseAddr, PhysicalEntryStride,
                                     RelRun, DecodedTableAnchors);
    if (RelRun >= limits::kMinJumpTableEntries)
      Info.PhysicalCapacity = std::max(Info.PhysicalCapacity, RelRun);
  }

  // A run of absolute code-pointer relocations at the table base proves the
  // entries are absolute code pointers, overriding the backward slice's
  // width-based relative guess.  The slice marks any sub-pointer-width load
  // relative, so an i386 4-byte absolute table (`jmpl *tab(,idx,4)` with
  // R_386_32 entries) would otherwise be decoded as PC-relative offsets and
  // dropped.  This classification is independent of how the table is bounded
  // (a `switch(x & mask)` still has an `and`-derived guard), so it must run
  // regardless of the guard search — decode correctness and entry count are
  // separate concerns.
  const uint32_t RawAbsCodePtrRun =
      !Info.RelocAbsolute && !Info.TwoTableSelect && !Info.HasTargetBase &&
              Info.HasBaseAddr && Info.EntrySize > 0 &&
              PhysicalEntryStride >= Info.EntrySize
          ? countCodePtrRelocRun(Img, Info.BaseAddr, PhysicalEntryStride)
          : 0;
  const uint32_t AbsCodePtrRun =
      boundCodePtrRunByNextAnchor(Img, Info.BaseAddr, PhysicalEntryStride,
                                  RawAbsCodePtrRun, DecodedTableAnchors);
  if (AbsCodePtrRun >= limits::kMinJumpTableEntries && Info.IsRelative) {
    Info.IsRelative = false;
    Info.IsSigned = false;
  }
  if (AbsCodePtrRun >= limits::kMinJumpTableEntries)
    Info.PhysicalCapacity = std::max(Info.PhysicalCapacity, AbsCodePtrRun);

  // Absolute code-pointer relocations likewise authenticate storage capacity,
  // never a selector domain.  An unguarded `switch(x & mask)` is accepted only
  // when the exact mask occurrence proves its feasible runtime coordinates;
  // sparse masks may own fewer slots than the surrounding relocation run.
  // A `switch(x % N)` table whose entries carry no relocations (AArch64 compact
  // byte/halfword tables, ARM32 inline `.text` word tables) cannot use the
  // relocation run above and has no `cmp` range guard.  Read the modulus N out
  // of the magic-division remainder that computes the index, which bounds the
  // table exactly and keeps the single-target readonly fallback below (which
  // only fires at MaxEntries == 0) from collapsing it to one entry.
  if (!GuardFound && !Info.IndexDomainAuthenticated && Info.HasBaseAddr &&
      Info.EntrySize > 0)
    inferBoundsFromModulo(Img, Rec, Info);

  // If a normalization offset is present and the guard bound looks
  // like it was applied to the original (pre-normalization) variable,
  // adjust it down to reflect the actual table size.
  if (Info.MaxEntries > 0 && Info.NormBase > 0) {
    uint32_t Adj = pullBackBound(Info.MaxEntries, Info);
    if (Adj != Info.MaxEntries && Adj >= limits::kMinJumpTableEntries) {
      LLVM_DEBUG(llvm::dbgs()
                 << "  pullback: adjusted bound " << Info.MaxEntries << " -> "
                 << Adj << " (normBase=" << Info.NormBase << ")\n");
      Info.MaxEntries = Adj;
    }
  }

  // A power-of-two-modulo / masked index (`and $(2^k-1)`, with an optional
  // following `dec` from a peeled iteration) is hard-bounded by the mask, for
  // every table kind (PIC-relative, GOTOFF, absolute).  Two such tables placed
  // back-to-back in rodata form one continuous relocation run / pointer run, so
  // an over-long read runs past the first table into the second — fabricating
  // bogus successor edges (and, with x87 residents, an unbalanced stack the TOP
  // recovery cannot reconcile).  The mask is a hard upper bound on the index,
  // so clamp to it even when a range guard was found: a guard derived from the
  // pre-`dec` mask (`and $7; dec` => index in [-1,6], 7 entries) over-counts by
  // the offset, and min(guard, mask) is always the safe table size.
  // A two-table merge holds 2N entries while the per-table index mask bounds
  // the index to N; the runtime base select supplies the doubling, so the mask
  // must not clamp the merged count.
  bool IncompleteMaskDomain = false;
  bool UsedNonContiguousMask = false;
  std::vector<uint32_t> MaskCoordinates;
  const uint32_t MaskBound = inferBoundsFromMask(
      Rec, Info, /*AllowNonContiguous=*/true, &IncompleteMaskDomain,
      &UsedNonContiguousMask, &MaskCoordinates);
  if (!IncompleteMaskDomain && !Info.TwoTableSelect &&
      !Info.TwoLevelIndex && MaskBound > 0) {
    Info.AuthenticatedMaskCoordinates = MaskCoordinates;
    // The next-anchor cap is a runtime-domain heuristic: another reachable
    // consumer may name an interior relocation without ending the physical
    // table object.  For storage ownership, use the uncapped relocation run
    // only when its end is independently equal to the mapped object/section
    // boundary.  This keeps exact physical capacity separate from both the
    // mask domain and consumer-specific suppression permissions.
    const uint32_t ExactAbsoluteStorageSlots = [&] {
      if (Info.EntrySize == 0 || PhysicalEntryStride < Info.EntrySize ||
          RawAbsCodePtrRun == 0)
        return uint32_t{0};
      if (const std::optional<va_t> OwnerEnd =
              Img.mappedObjectOwnerEnd(Info.BaseAddr);
          OwnerEnd && *OwnerEnd >= Info.BaseAddr + Info.EntrySize) {
        const uint64_t Span = *OwnerEnd - Info.BaseAddr;
        if ((Span - Info.EntrySize) % PhysicalEntryStride == 0) {
          const uint64_t Slots =
              (Span - Info.EntrySize) / PhysicalEntryStride + 1;
          if (Slots <= RawAbsCodePtrRun &&
              Slots <= std::numeric_limits<uint32_t>::max())
            return static_cast<uint32_t>(Slots);
        }
      }
      return codePtrRelocRunHasExactBoundary(
                 Img, Info.BaseAddr, PhysicalEntryStride, RawAbsCodePtrRun,
                 DecodedTableAnchors)
                 ? RawAbsCodePtrRun
                 : uint32_t{0};
    }();
    const uint32_t AuthenticatedStorageSlots =
        std::max(Info.PhysicalCapacity, ExactAbsoluteStorageSlots);
    const uint64_t PhysicalStride =
        Info.EntryStride != 0 ? Info.EntryStride : Info.EntrySize;
    if (PhysicalStride < Info.EntrySize || Info.EntrySize == 0 ||
        MaskCoordinates.empty())
      return {};

    std::optional<uint64_t> AddressScale;
    for (const JumpTableLoadRole &Role : Info.LoadRoles) {
      if (Role.LoadWidth != Info.EntrySize || Role.AddressScale == 0 ||
          std::find(Role.AllowedBases.begin(), Role.AllowedBases.end(),
                    Info.BaseAddr) == Role.AllowedBases.end())
        continue;
      if (AddressScale && *AddressScale != Role.AddressScale)
        return {};
      AddressScale = Role.AddressScale;
    }
    if (!AddressScale)
      return {};

    // A guard and a mask constrain the same runtime selector domain.  Keep
    // their intersection before converting coordinates to physical slots;
    // MaxEntries from a relocation run is instead storage capacity and is
    // checked only after the conversion.  Pre-scaled indices carry byte
    // coordinates while a comparison commonly bounds logical entries.
    if (GuardFound && Info.MaxEntries > 0) {
      uint64_t GuardCoordinateLimit = Info.MaxEntries;
      if (Info.PreScaledIndex && Info.Stride > 1) {
        if (GuardCoordinateLimit >
            std::numeric_limits<uint64_t>::max() / Info.Stride)
          return {};
        GuardCoordinateLimit *= Info.Stride;
      }
      MaskCoordinates.erase(
          std::remove_if(MaskCoordinates.begin(), MaskCoordinates.end(),
                         [&](uint32_t Coordinate) {
                           return Coordinate >= GuardCoordinateLimit;
                         }),
          MaskCoordinates.end());
      if (MaskCoordinates.size() < limits::kMinJumpTableEntries)
        return {};
    }

    std::vector<uint32_t> PhysicalSlots;
    std::vector<JumpTableStorageRange> ExactStorage;
    PhysicalSlots.reserve(MaskCoordinates.size());
    ExactStorage.reserve(MaskCoordinates.size());
    for (uint32_t Coordinate : MaskCoordinates) {
      if (Coordinate != 0 &&
          *AddressScale > std::numeric_limits<uint64_t>::max() / Coordinate)
        return {};
      const uint64_t ByteOffset = uint64_t(Coordinate) * *AddressScale;
      if (ByteOffset % PhysicalStride != 0)
        return {};
      const uint64_t Slot = ByteOffset / PhysicalStride;
      if (Slot > std::numeric_limits<uint32_t>::max())
        return {};
      if (!PhysicalSlots.empty() && Slot <= PhysicalSlots.back())
        return {};
      if (ByteOffset > InvalidVA - Info.BaseAddr ||
          Info.EntrySize - 1 > InvalidVA - (Info.BaseAddr + ByteOffset))
        return {};
      PhysicalSlots.push_back(static_cast<uint32_t>(Slot));
      ExactStorage.push_back(JumpTableStorageRange{
          Info.BaseAddr + ByteOffset, Info.EntrySize, Info.EntrySize, 1});
    }

    const uint32_t PhysicalSpan = PhysicalSlots.back() + 1;
    // A relocation run is authenticated storage capacity, not an index-domain
    // proof.  Every feasible coordinate must map inside it; taking min would
    // silently discard live selector values.
    if (Info.PhysicalCapacity > 0 && PhysicalSpan > Info.PhysicalCapacity)
      return {};
    Info.RuntimeCaseLabels = MaskCoordinates;

    // Runtime-domain slots and physical object ownership are different facts.
    // An exact owner/anchor boundary may prove that compiler filler belongs to
    // the same object, but it does not authorize suppressing a filler
    // relocation that another reachable instruction consumes.  That separate
    // permission is computed after the final target graph is known.
    const bool OwnsCompletePhysicalObject =
        AuthenticatedStorageSlots == PhysicalSpan &&
        codePtrRelocRunHasExactBoundary(Img, Info.BaseAddr, PhysicalStride,
                                        PhysicalSpan, DecodedTableAnchors);
    Info.RuntimeSlotIndices = std::move(PhysicalSlots);
    if (OwnsCompletePhysicalObject) {
      Info.StorageRanges = {JumpTableStorageRange{
          Info.BaseAddr, Info.EntrySize, PhysicalStride, PhysicalSpan}};
    } else {
      Info.StorageRanges = std::move(ExactStorage);
    }
    Info.SuppressibleRelocationSlots.clear();
    for (uint32_t Slot : Info.RuntimeSlotIndices) {
      if (Slot != 0 && PhysicalStride > (InvalidVA - Info.BaseAddr) / Slot)
        return {};
      const va_t SlotVA = Info.BaseAddr + uint64_t(Slot) * PhysicalStride;
      if (Img.CodePtrRelocSlots.count(SlotVA))
        Info.SuppressibleRelocationSlots.push_back(SlotVA);
    }
    if (ProtectedJumpTableRelocationSlots)
      Info.SuppressibleRelocationSlots.erase(
          std::remove_if(Info.SuppressibleRelocationSlots.begin(),
                         Info.SuppressibleRelocationSlots.end(),
                         [&](va_t Slot) {
                           return ProtectedJumpTableRelocationSlots->count(
                               Slot);
                         }),
          Info.SuppressibleRelocationSlots.end());
    Info.MaxEntries = PhysicalSpan;
    Info.IndexDomainAuthenticated = true;
    LLVM_DEBUG(llvm::dbgs()
               << "  mask-domain: authenticated "
               << Info.RuntimeCaseLabels.size() << " coordinates over "
               << PhysicalSpan << " physical slots\n");
  } else if (MaskBound == 0 && Info.MaxEntries > 0 && Info.Stride > 1 &&
             !Info.RelocBounded) {
    // Legacy guard-only recovery has no exact coordinate set.  Apply its
    // historical stride adjustment only after exact mask recovery had a chance
    // to establish the real runtime coordinate; doing it earlier divides an
    // equal guard/mask bound twice.
    uint32_t Adj = Info.MaxEntries / Info.Stride;
    if (Adj >= limits::kMinJumpTableEntries)
      Info.MaxEntries = Adj;
  }
  // An unmodelled mask-dependent transform cannot be rescued by readable
  // relocation capacity.  It also must not erase an independent complete
  // proof over this exact final index occurrence: a full-domain guard or the
  // LLVM constant-division remainder theorem already proves every runtime
  // value is inside the published domain, irrespective of incidental ANDs in
  // the quotient/flag calculation.  Final-root revalidation below replays that
  // independent witness after ownership has been narrowed.
  if (IncompleteMaskDomain && Info.AuthenticatedGuardBound == 0 &&
      Info.AuthenticatedModuloBound == 0) {
    return {};
  }
  // A sampled/non-prefix guard must not be rescued by a relocation run: the
  // run proves readable table storage, not which selector values can reach
  // it.  An independently authenticated exact mask domain is sufficient
  // because it bounds the actual address coordinate regardless of the guard;
  // the legacy modulo recognizer is intentionally not a rescue here until it
  // is occurrence/CFG authenticated in the same way.
  if (Info.IncompleteGuardDomain && !Info.IndexDomainAuthenticated) {
    return {};
  }

  if (Info.MaxEntries == 0 || Info.MaxEntries > limits::kMaxJumpTableEntries)
    Info.MaxEntries = 0;
  if (!JumpTableProofContextComplete && RequestedCompleteJumpTableProof)
    return {};
  if (Info.PreScaledIndex && Info.RuntimeSlotIndices.empty())
    return {};
  // Static bytes and relocation runs establish only physical capacity.  Every
  // multi-target publication must also carry a complete proof of the exact
  // runtime selector domain.  In particular, neither the legacy unbounded
  // reader nor self-bounding emulation may turn a readable prefix into an
  // index bound.  A single immutable pointer is a separate direct-branch
  // problem and is intentionally not published as a jump table here.
  if (!Info.IndexDomainAuthenticated ||
      Info.MaxEntries < limits::kMinJumpTableEntries) {
    return {};
  }

  // Capacity constrains an authenticated domain; it never supplies one.  A
  // domain that exceeds known storage cannot be repaired by taking min(),
  // because that would silently drop feasible selector values.
  if (Info.PhysicalCapacity != 0 && Info.MaxEntries > Info.PhysicalCapacity)
    return {};

  // Mask recovery already materializes its exact (possibly sparse or
  // pre-scaled) coordinate-to-slot map.  Guard and modulo domains are dense:
  // publish only slots [0,N), not the entire relocation capacity that was used
  // to bootstrap the first occurrence proof.  This keeps adjacent table slots
  // out of final ownership and suppression, then revalidates both certificates
  // on the refined root set below.
  if (Info.RuntimeSlotIndices.empty()) {
    if (!Info.HasBaseAddr || Info.EntrySize == 0)
      return {};
    const uint64_t PhysicalStride =
        Info.EntryStride != 0 ? Info.EntryStride : Info.EntrySize;
    if (PhysicalStride < Info.EntrySize)
      return {};

    std::vector<JumpTableStorageRange> ExactStorage;
    std::vector<va_t> ExactSuppressibleSlots;
    ExactStorage.reserve(Info.MaxEntries);
    ExactSuppressibleSlots.reserve(Info.MaxEntries);
    for (uint32_t Slot = 0; Slot < Info.MaxEntries; ++Slot) {
      if (Slot != 0 && PhysicalStride > (InvalidVA - Info.BaseAddr) / Slot)
        return {};
      const va_t SlotVA = Info.BaseAddr + uint64_t(Slot) * PhysicalStride;
      if (Info.EntrySize - 1 > InvalidVA - SlotVA ||
          !Img.readVA(SlotVA, Info.EntrySize))
        return {};
      const std::optional<va_t> OwnerEnd = Img.mappedObjectOwnerEnd(SlotVA);
      if (!OwnerEnd || SlotVA >= *OwnerEnd ||
          Info.EntrySize > *OwnerEnd - SlotVA)
        return {};
      ExactStorage.push_back(
          JumpTableStorageRange{SlotVA, Info.EntrySize, Info.EntrySize, 1});
      if (Img.CodePtrRelocSlots.count(SlotVA))
        ExactSuppressibleSlots.push_back(SlotVA);
    }
    Info.StorageRanges = std::move(ExactStorage);
    Info.SuppressibleRelocationSlots = std::move(ExactSuppressibleSlots);
    if (ProtectedJumpTableRelocationSlots)
      Info.SuppressibleRelocationSlots.erase(
          std::remove_if(Info.SuppressibleRelocationSlots.begin(),
                         Info.SuppressibleRelocationSlots.end(),
                         [&](va_t Slot) {
                           return ProtectedJumpTableRelocationSlots->count(
                               Slot);
                         }),
          Info.SuppressibleRelocationSlots.end());
  }

  // The first occurrence proof may need a relocation-backed candidate root
  // set before the exact runtime domain is known.  Require both certificates
  // again after final slot ownership/suppression has been materialized.  The
  // index-domain proof itself must also be replayed: a relocation just beyond
  // the final domain may restore a predecessor that bypasses the guard while
  // leaving both LOAD-role certificates valid.
  ActiveJumpTableProofRoots = jumpTableProofRoots(Info);
  auto RevalidateIndexDomain = [&]() -> bool {
    bool Revalidated = false;
    if (Info.AuthenticatedGuardBound != 0) {
      JumpTableInfo Check = Info;
      Check.MaxEntries = 0;
      Check.IndexDomainAuthenticated = false;
      Check.IncompleteGuardDomain = false;
      const bool GuardReplay =
          ReusePriorGuardProof || inferBoundsFromPreciseGuards(Rec, Check);
      if (!ReusePriorGuardProof &&
          (!GuardReplay || Check.MaxEntries != Info.AuthenticatedGuardBound))
        return false;
      Revalidated = true;
    }
    if (Info.AuthenticatedModuloBound != 0) {
      JumpTableInfo Check = Info;
      Check.MaxEntries = 0;
      Check.IndexDomainAuthenticated = false;
      Check.AuthenticatedModuloBound = 0;
      if (!inferBoundsFromModulo(Img, Rec, Check) ||
          Check.MaxEntries != Info.AuthenticatedModuloBound)
        return false;
      Revalidated = true;
    }
    if (!Info.AuthenticatedMaskCoordinates.empty()) {
      bool Incomplete = false;
      bool NonContiguous = false;
      std::vector<uint32_t> Coordinates;
      const uint32_t Bound =
          inferBoundsFromMask(Rec, Info, /*AllowNonContiguous=*/true,
                              &Incomplete, &NonContiguous, &Coordinates);
      if (Incomplete || Bound == 0 ||
          Coordinates != Info.AuthenticatedMaskCoordinates)
        return false;
      Revalidated = true;
    }
    // Composite strategies have exact storage from the outset and return via
    // ExplicitTargets above; every ordinary table must retain at least one
    // replayable full-domain witness.
    return Revalidated;
  };
  const bool RevalidatedIndexDomain = RevalidateIndexDomain();
  if (!RevalidatedIndexDomain)
    return {};
  if (!branchTargetDependsOnTableLoad(Rec, Info))
    return {};
  if (!tableLoadAddressesMatchRole(Info))
    return {};
  ClaimValidatedPotentialTable();

  std::vector<uint32_t> KeptIdx;
  auto Targets = readTableEntries(Img, Info, &KeptIdx);

  // A proven runtime domain is complete: every feasible coordinate must map to
  // one valid target.  Truncation is only meaningful for the legacy unbounded
  // scanner; truncating a bounded domain silently changes guest control flow.
  const size_t BeforeSanity = Targets.size();
  const bool Sane = sanityCheckTargets(Img, Targets);
  if (!Sane || ((Info.MaxEntries > 0 || !Info.RuntimeSlotIndices.empty()) &&
                Targets.size() != BeforeSanity))
    Targets.clear();
  if (KeptIdx.size() > Targets.size())
    KeptIdx.resize(Targets.size()); // sanity-check truncates trailing entries

  // Emulation-based fallback: when all static strategies fail, try
  // running the ops through the NdOp emulator for each candidate index.
  if (Targets.size() < limits::kMinJumpTableEntries && Info.MaxEntries > 0 &&
      Info.RuntimeSlotIndices.empty() && CurrentImg) {
    auto EmuTargets = tryEmulatedResolution(Img, Rec, Info);
    std::vector<va_t> Checked = EmuTargets;
    if (EmuTargets.size() == Info.MaxEntries &&
        sanityCheckTargets(Img, Checked) &&
        Checked.size() == EmuTargets.size()) {
      Targets = std::move(EmuTargets);
      KeptIdx.clear(); // dense fallback: positional labels apply
      LLVM_DEBUG(llvm::dbgs() << "  emulated: recovered " << Targets.size()
                              << " entries via NdOp emulation\n");
    }
  }

  // Emulation cross-check for a bounded table that decoded *fewer* targets than
  // its known entry count.  The static reader classifies the entry layout
  // (relative/absolute, sign, scale, target-base) before decoding, and a
  // misclassification can truncate an otherwise-valid table part-way — leaving
  // a plausible-but-incomplete target list that the `< kMin` fallback above
  // (which fires only on near-total failure) never revisits.  Re-run the
  // *actual* dispatch arithmetic through the emulator, which reads the real
  // base+index+load and so cannot mis-guess the layout, and adopt its result
  // only when it is strictly more complete AND reproduces the static decode on
  // every shared index.  That prefix-agreement gate makes this monotonic: it
  // can only append cases the static read dropped, never rewrite one it already
  // decoded, so a correctly recovered table is left untouched.  Restricted to a
  // dense static result (no sparse skips) so the two index coordinates align,
  // and bounded by the same MaxEntries so it can never over-read past the
  // guard/reloc bound.
  if (CurrentImg && Info.RuntimeSlotIndices.empty() && Info.MaxEntries > 0 &&
      Targets.size() >= limits::kMinJumpTableEntries &&
      Targets.size() < Info.MaxEntries) {
    bool DenseStatic = KeptIdx.size() == Targets.size();
    for (size_t I = 0; DenseStatic && I < KeptIdx.size(); ++I)
      DenseStatic = KeptIdx[I] == I;
    if (DenseStatic) {
      auto EmuTargets = tryEmulatedResolution(Img, Rec, Info);
      bool ExtendsStatic = EmuTargets.size() == Info.MaxEntries &&
                           EmuTargets.size() > Targets.size();
      for (size_t I = 0; ExtendsStatic && I < Targets.size(); ++I)
        ExtendsStatic = EmuTargets[I] == Targets[I];
      if (ExtendsStatic) {
        LLVM_DEBUG(llvm::dbgs() << "  emu-verify: extended bounded table from "
                                << Targets.size() << " to " << EmuTargets.size()
                                << " entries via dispatch emulation\n");
        Targets = std::move(EmuTargets);
        KeptIdx.clear(); // dense positional labels apply
      }
    }
  }

  // Ground-truth cross-check for a plain relative/absolute table: rebuild the
  // targets by executing the *actual* dispatch arithmetic per index instead of
  // trusting the statically classified entry layout (relative-vs-absolute,
  // signedness).  A misclassified layout decodes a full-length but *wrong*
  // target set that still passes the sanity check (every entry lands in the
  // function), a silent miscompile the extend/fallback strategies above never
  // revisit because the count already looks complete.  The emulation reads the
  // same table bytes and applies the same transform the processor would, so
  // when it is fully grounded — every index read the recovered table slot
  // (BaseAddr + i*EntryStride) and produced a valid target — its result is
  // authoritative and supersedes a disagreeing static decode.
  //
  // Guarded to be a no-op wherever the static decode is already trustworthy, so
  // currently-recovered tables keep byte-identical targets: skipped for
  // reloc-bounded / two-table / compact (TargetBase) / pre-scaled tables (whose
  // layout is confirmed by relocations or a dedicated detector), for sparse
  // (gapped) decodes whose positional index would not line up with the emulated
  // slot, and adopted only when the emulation agrees on entry count yet differs
  // on some value.
  bool DenseStatic = KeptIdx.empty() || KeptIdx.size() == Targets.size();
  for (size_t I = 0; DenseStatic && I < KeptIdx.size(); ++I)
    DenseStatic = KeptIdx[I] == I;
  if (CurrentImg && Info.RuntimeSlotIndices.empty() && DenseStatic &&
      !Targets.empty() && Info.IndexReg != InvalidVA && Info.HasBaseAddr &&
      !Info.HasTargetBase && Info.EntryScale == 1 && !Info.PreScaledIndex &&
      !Info.TwoTableSelect && !Info.RelocAbsolute && !Info.RelocBounded &&
      (Info.EntrySize == 1 || Info.EntrySize == 2 || Info.EntrySize == 4 ||
       Info.EntrySize == 8)) {
    bool Grounded = false;
    auto EmuTargets = emulateGroundedTargets(
        Img, Rec, Info, static_cast<uint32_t>(Targets.size()), Grounded);
    if (Grounded && EmuTargets.size() == Targets.size() &&
        EmuTargets != Targets) {
      std::vector<va_t> Check = EmuTargets;
      if (sanityCheckTargets(Img, Check) && Check.size() == EmuTargets.size()) {
        LLVM_DEBUG(llvm::dbgs()
                   << "  emu-ground: corrected " << Targets.size()
                   << " statically-misclassified targets via grounded dispatch "
                      "emulation\n");
        Targets = std::move(EmuTargets);
        KeptIdx.clear(); // dense positional labels apply
      }
    }
  }

  if (Targets.size() < limits::kMinJumpTableEntries)
    return {};

  if ((!Info.RuntimeCaseLabels.empty() || !Info.RuntimeSlotIndices.empty()) &&
      (Info.RuntimeCaseLabels.size() != Targets.size() ||
       Info.RuntimeSlotIndices.size() != Targets.size()))
    return {};

  // Decide which physical code-pointer relocations this dispatch may suppress
  // only after both the final runtime domain and final target edges are known.
  // Start with every pointer relocation in the authenticated physical object,
  // build a candidate-local CFG, and monotonically remove any gap/filler slot
  // that has an independent consumer in that reachable graph.  Repeating to a
  // fixed point handles a target root that becomes reachable only after another
  // slot loses suppression.  A lexical consumer in a pruned block is not
  // evidence; an entry-reachable LEA/LOAD of the slot is.
  if (!Info.StorageRanges.empty() && Img.getPointerSize() != 0) {
    std::vector<va_t> PhysicalCodePtrSlots;
    size_t SlotBudget = limits::kMaxJumpTableEntries;
    for (const JumpTableStorageRange &Range : Info.StorageRanges) {
      if (Range.EntrySize < Img.getPointerSize() ||
          Range.EntryStride < Range.EntrySize || Range.PhysicalSlotCount == 0)
        continue;
      if (Range.PhysicalSlotCount > SlotBudget)
        return {};
      SlotBudget -= Range.PhysicalSlotCount;
      for (uint32_t I = 0; I < Range.PhysicalSlotCount; ++I) {
        if (I != 0 && Range.EntryStride > (InvalidVA - Range.BaseAddr) / I)
          return {};
        const va_t Slot = Range.BaseAddr + uint64_t(I) * Range.EntryStride;
        if (Img.CodePtrRelocSlots.count(Slot))
          PhysicalCodePtrSlots.push_back(Slot);
      }
    }
    std::sort(PhysicalCodePtrSlots.begin(), PhysicalCodePtrSlots.end());
    PhysicalCodePtrSlots.erase(
        std::unique(PhysicalCodePtrSlots.begin(), PhysicalCodePtrSlots.end()),
        PhysicalCodePtrSlots.end());

    auto storageOwns = [&](va_t Address) {
      return std::any_of(Info.StorageRanges.begin(), Info.StorageRanges.end(),
                         [&](const JumpTableStorageRange &Range) {
                           return Range.ownsStorageAddress(Address);
                         });
    };
    auto containingInsnIsReachable = [&](va_t Field,
                                         const std::set<va_t> &Reachable) {
      auto It = Insns.upper_bound(Field);
      if (It == Insns.begin())
        return false;
      --It;
      const va_t Begin = It->first;
      const uint64_t Size = It->second.Size;
      return Reachable.count(Begin) && Field >= Begin &&
             Size <= InvalidVA - Begin && Field < Begin + Size;
    };
    auto storageBase = [&](va_t Address) {
      return std::any_of(Info.StorageRanges.begin(), Info.StorageRanges.end(),
                         [&](const JumpTableStorageRange &Range) {
                           return Range.BaseAddr == Address;
                         });
    };
    auto isAuthenticatedTargetLoad = [&](const LowOp &Op) {
      if (Op.Opcode != NdOp::LOAD)
        return false;
      auto Authenticates = [&](const JumpTableInfo &Candidate) {
        return std::any_of(Candidate.LoadRoles.begin(),
                           Candidate.LoadRoles.end(),
                           [&](const JumpTableLoadRole &Role) {
                             return Role.Load.Addr == Op.Addr &&
                                    Role.Load.Seq == Op.Seq &&
                                    Role.LoadWidth == Op.Output.Size;
                           });
      };
      if (Authenticates(Info))
        return true;
      // A peeled/loop-body pair can dispatch through the same exact physical
      // table.  Once one branch has a published occurrence certificate, its
      // target LOAD is not an independent data consumer of the sibling table:
      // both loads are candidates for the same post-SSA terminal-use check.
      // Compare precise storage runs, not a numeric base, so an adjacent or
      // overlapping foreign table cannot borrow this exemption.  The module
      // Requested/Vetoed suppression arbitration remains authoritative when a
      // certified LOAD has any observable side use or a later rebuild loses
      // its jump-table plan.
      return std::any_of(ResolvedTableInfo.begin(), ResolvedTableInfo.end(),
                         [&](const auto &Entry) {
                           const JumpTableInfo &Other = Entry.second;
                           return !Info.StorageRanges.empty() &&
                                  Other.StorageRanges == Info.StorageRanges &&
                                  Authenticates(Other);
                         });
    };
    std::vector<JumpTableValueOccurrence> SlotAddressAlternatives;
    SlotAddressAlternatives.reserve(PhysicalCodePtrSlots.size());
    for (va_t Slot : PhysicalCodePtrSlots)
      SlotAddressAlternatives.push_back(
          {NdVar::address(Slot, Img.getPointerSize()), InvalidVA, -1,
           /*DefinedAtPoint=*/false});

    // Object escape is wider than code-pointer-slot identity.  A consumer can
    // expose `base + 4`, an entry's interior byte, or a dynamically formed
    // pointer rooted anywhere in the authenticated storage span and then walk
    // to another relocation slot.  Enumerate every byte of the (bounded)
    // physical ranges for this revocation-only proof; if the evidence budget
    // cannot cover the complete object, fail closed by retaining every mirror
    // relocation.  Direct LOAD classification below deliberately keeps the
    // narrower slot-start alternatives so an exact read preserves only the
    // code pointer it actually consumes.
    std::vector<JumpTableValueOccurrence> StorageAddressAlternatives;
    bool StorageAddressAlternativesComplete = true;
    size_t StorageAddressBudget = limits::kMaxJumpTableEvidenceWork;
    std::set<va_t> StorageAddresses;
    for (const JumpTableStorageRange &Range : Info.StorageRanges) {
      const std::optional<va_t> End = Range.storageEnd();
      if (!End || *End < Range.BaseAddr ||
          *End - Range.BaseAddr > StorageAddressBudget) {
        StorageAddressAlternativesComplete = false;
        break;
      }
      StorageAddressBudget -= static_cast<size_t>(*End - Range.BaseAddr);
      for (va_t Address = Range.BaseAddr; Address < *End; ++Address)
        StorageAddresses.insert(Address);
    }
    if (StorageAddressAlternativesComplete) {
      StorageAddressAlternatives.reserve(StorageAddresses.size());
      for (va_t Address : StorageAddresses)
        StorageAddressAlternatives.push_back(
            {NdVar::address(Address, Img.getPointerSize()), InvalidVA, -1,
             /*DefinedAtPoint=*/false});
    }
    auto objectAddressEscapes = [&](const std::set<va_t> &Reachable,
                                    bool &AnalysisComplete) {
      if (!StorageAddressAlternativesComplete) {
        AnalysisComplete = false;
        return false;
      }
      if (StorageAddressAlternatives.empty())
        return false;

      std::vector<JumpTableValueQuery> EscapeQueries;
      auto AddQuery = [&](const NdVar &Value, const LowOp &Use) {
        if ((!Value.isReg() && !Value.isTemp() && !Value.isConst()) ||
            Value.Size == 0)
          return;
        JumpTableValueQuery Q;
        Q.Candidate = Value;
        Q.UseAddr = Use.Addr;
        Q.UseSeq = Use.Seq;
        // This query can only revoke relocation suppression.  A generic exact
        // address alternative is therefore deliberately conservative: a
        // coincident scalar may retain a mirror field, but can never grant a
        // stale-address optimization.  Loader/role owners remain mandatory in
        // the positive table certificates above.
        Q.Alternatives = StorageAddressAlternatives;
        Q.AllowZeroExtension = true;
        Q.Relation = JumpTableValueRelation::MayDepend;
        EscapeQueries.push_back(std::move(Q));
      };
      for (va_t Addr : Reachable) {
        auto It = Insns.find(Addr);
        if (It == Insns.end())
          continue;
        for (const LowOp &Op : It->second.Ops) {
          if (Op.Opcode == NdOp::STORE && Op.NumInputs >= 2)
            AddQuery(Op.Inputs[1], Op);
          if (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL ||
              Op.Opcode == NdOp::INTRINSIC || Op.Opcode == NdOp::RETURN)
            for (uint8_t I = 0; I < Op.NumInputs; ++I)
              AddQuery(Op.Inputs[I], Op);
        }
      }
      if (EscapeQueries.empty())
        return false;
      bool Complete = true;
      const std::vector<bool> Results =
          tableValuesMatchAtUses(EscapeQueries, &Complete);
      AnalysisComplete &= Complete;
      return std::any_of(Results.begin(), Results.end(),
                         [](bool Result) { return Result; });
    };
    auto reachableIndependentLoads = [&](const std::set<va_t> &Reachable,
                                         bool &AnalysisComplete,
                                         std::set<va_t> &ExactSlots,
                                         bool &MayAliasObject) {
      struct LoadUse {
        NdVar Address;
        va_t Addr = InvalidVA;
        int Seq = -1;
      };
      std::vector<LoadUse> Loads;
      for (va_t Addr : Reachable) {
        auto It = Insns.find(Addr);
        if (It == Insns.end())
          continue;
        for (const LowOp &Op : It->second.Ops) {
          if (Op.Opcode != NdOp::LOAD || Op.NumInputs == 0 ||
              isAuthenticatedTargetLoad(Op))
            continue;
          const NdVar &Address =
              Op.NumInputs >= 2 ? Op.Inputs[1] : Op.Inputs[0];
          if ((!Address.isReg() && !Address.isTemp() && !Address.isConst()) ||
              Address.Size == 0)
            continue;
          Loads.push_back({Address, Op.Addr, Op.Seq});
        }
      }
      if (!StorageAddressAlternativesComplete) {
        AnalysisComplete = false;
        return;
      }
      if (Loads.empty() || StorageAddressAlternatives.empty())
        return;
      if (Loads.size() > limits::kMaxJumpTableEvidenceWork ||
          PhysicalCodePtrSlots.size() > limits::kMaxJumpTableEvidenceWork ||
          StorageAddressAlternatives.size() >
              limits::kMaxJumpTableEvidenceWork ||
          PhysicalCodePtrSlots.size() > limits::kMaxJumpTableEvidenceWork -
                                            StorageAddressAlternatives.size() ||
          Loads.size() > limits::kMaxJumpTableEvidenceWork /
                             (PhysicalCodePtrSlots.size() +
                              StorageAddressAlternatives.size())) {
        AnalysisComplete = false;
        return;
      }

      std::vector<JumpTableValueQuery> Queries;
      Queries.reserve(Loads.size() * (PhysicalCodePtrSlots.size() + 1));
      for (size_t LoadIndex = 0; LoadIndex < Loads.size(); ++LoadIndex) {
        const LoadUse &Load = Loads[LoadIndex];
        for (size_t SlotIndex = 0; SlotIndex < PhysicalCodePtrSlots.size();
             ++SlotIndex) {
          JumpTableValueQuery Exact;
          Exact.Candidate = Load.Address;
          Exact.UseAddr = Load.Addr;
          Exact.UseSeq = Load.Seq;
          Exact.Alternatives = {SlotAddressAlternatives[SlotIndex]};
          Exact.AllowZeroExtension = true;
          Queries.push_back(std::move(Exact));
        }
        JumpTableValueQuery Dependency;
        Dependency.Candidate = Load.Address;
        Dependency.UseAddr = Load.Addr;
        Dependency.UseSeq = Load.Seq;
        Dependency.Alternatives = StorageAddressAlternatives;
        Dependency.AllowZeroExtension = true;
        Dependency.Relation = JumpTableValueRelation::MayDepend;
        Queries.push_back(std::move(Dependency));
      }

      bool Complete = true;
      const std::vector<bool> Results =
          tableValuesMatchAtUses(Queries, &Complete);
      AnalysisComplete &= Complete;
      size_t ResultIndex = 0;
      for (size_t LoadIndex = 0; LoadIndex < Loads.size(); ++LoadIndex) {
        bool HasExactSlot = false;
        for (size_t SlotIndex = 0; SlotIndex < PhysicalCodePtrSlots.size();
             ++SlotIndex) {
          if (ResultIndex < Results.size() && Results[ResultIndex]) {
            ExactSlots.insert(PhysicalCodePtrSlots[SlotIndex]);
            HasExactSlot = true;
          }
          ++ResultIndex;
        }
        const bool DependsOnObject =
            ResultIndex < Results.size() && Results[ResultIndex];
        ++ResultIndex;
        if (DependsOnObject && !HasExactSlot)
          MayAliasObject = true;
      }
    };
    auto hasReachableIndependentConsumer = [&](va_t Slot,
                                               const std::set<va_t> &Reachable,
                                               bool WholeObjectEscapes) {
      if (WholeObjectEscapes)
        return true;
      for (const auto &[FieldVA, Field] : Img.DataAddressRelocOperands)
        if (!Field.PCRelativeFromInstructionEnd && Field.TargetVA == Slot &&
            !storageBase(Slot) && containingInsnIsReachable(FieldVA, Reachable))
          return true;
      for (const auto &[FieldVA, Field] : Img.CodeAddressRelocOperands)
        if (!Field.PCRelativeFromInstructionEnd && Field.TargetVA == Slot &&
            !storageBase(Slot) && containingInsnIsReachable(FieldVA, Reachable))
          return true;

      // Relocation-free same-section materializations still carry exact
      // address provenance in LowIR.  Restrict the scan to the final
      // candidate graph so an unreachable textual LEA cannot self-bootstrap
      for (va_t Addr : Reachable) {
        auto It = Insns.find(Addr);
        if (It == Insns.end())
          continue;
        for (const LowOp &Op : It->second.Ops) {
          if (isAuthenticatedTargetLoad(Op))
            continue;
          auto IsExactSlot = [&](const NdVar &V) {
            return V.isConst() && V.Offset == Slot && !storageBase(Slot) &&
                   isExactAddressProvenance(V.Provenance);
          };
          if (IsExactSlot(Op.Output))
            return true;
          for (uint8_t I = 0; I < Op.NumInputs; ++I)
            if (IsExactSlot(Op.Inputs[I]))
              return true;
        }
      }

      // An occurrence-backed pointer elsewhere in the image is
      // conservatively independent when its source slot is outside this
      // candidate object.  This can only retain evidence; it never grants
      // suppression in the absence of a reachable consumer proof.
      for (va_t PointerSlot : Img.DataPtrRelocSlots) {
        if (storageOwns(PointerSlot))
          continue;
        const uint8_t *P = Img.readVA(PointerSlot, Img.getPointerSize());
        if (P && readPtr(P, Img.is64Bit()) == Slot)
          return true;
      }
      // Value-keyed loader summaries do not identify a consuming
      // instruction and therefore cannot distinguish a reachable use from
      // a relocation in a pruned lexical block.  All occurrence-backed
      // uses in this function were checked above; cross-function vetoes
      // are resolved once every LowFunc is available, where reachability
      // and exact LOAD/escape occurrences can be considered together.
      return false;
    };

    std::vector<va_t> Allowlist = PhysicalCodePtrSlots;
    if (ProtectedJumpTableRelocationSlots)
      Allowlist.erase(
          std::remove_if(Allowlist.begin(), Allowlist.end(),
                         [&](va_t Slot) {
                           return ProtectedJumpTableRelocationSlots->count(
                               Slot);
                         }),
          Allowlist.end());
    bool Stable = false;
    for (size_t Iteration = 0; Iteration <= PhysicalCodePtrSlots.size();
         ++Iteration) {
      JumpTableInfo CandidateInfo = Info;
      CandidateInfo.SuppressibleRelocationSlots = Allowlist;
      const std::set<va_t> Roots = jumpTableProofRoots(CandidateInfo);
      ActiveJumpTableProofRoots = Roots;
      const std::set<va_t> Reachable = candidateReachableInstructions(
          Rec, Targets, Roots, Info.StorageRanges);
      if (!Reachable.count(CurrentFuncEntry))
        return {};

      bool ConsumerAnalysisComplete = true;
      const bool WholeObjectEscapes =
          objectAddressEscapes(Reachable, ConsumerAnalysisComplete);
      std::set<va_t> DirectReadSlots;
      bool AmbiguousObjectLoad = false;
      reachableIndependentLoads(Reachable, ConsumerAnalysisComplete,
                                DirectReadSlots, AmbiguousObjectLoad);
      if (!ConsumerAnalysisComplete)
        Allowlist.clear();

      std::vector<va_t> Refined;
      for (va_t Slot : Allowlist)
        if (!WholeObjectEscapes && !AmbiguousObjectLoad &&
            !DirectReadSlots.count(Slot) &&
            !hasReachableIndependentConsumer(Slot, Reachable,
                                             /*WholeObjectEscapes=*/false))
          Refined.push_back(Slot);
      if (Refined == Allowlist) {
        Stable = true;
        break;
      }
      Allowlist = std::move(Refined);
    }
    if (!Stable)
      return {};
    Info.SuppressibleRelocationSlots = std::move(Allowlist);

    // A newly preserved relocation target is a real proof root.  Re-run both
    // occurrence certificates with the final allowlist before publishing.
    ActiveJumpTableProofRoots = jumpTableProofRoots(Info);
    const bool FinalIndexDomain = RevalidateIndexDomain();
    const bool FinalTargetRole = branchTargetDependsOnTableLoad(Rec, Info);
    const bool FinalAddressRole = tableLoadAddressesMatchRole(Info);
    if (!FinalIndexDomain || !FinalTargetRole || !FinalAddressRole)
      return {};
  } else {
    Info.SuppressibleRelocationSlots.clear();
  }

  // Carry the kept slot indices so recoverCaseLabels assigns case values by the
  // real table index (a bounded sparse table skips don't-care slots).  Only
  // useful when the kept indices are *not* the trivial 0..N-1 (a gap exists);
  // an empty vector leaves the positional labelling unchanged.
  {
    bool HasGap = KeptIdx.size() != Targets.size();
    for (size_t I = 0; !HasGap && I < KeptIdx.size(); ++I)
      HasGap = KeptIdx[I] != I;
    Info.EntryIndices = HasGap ? std::move(KeptIdx) : std::vector<uint32_t>{};
  }
  Info.RequiresCompleteCFGProof = RequestedCompleteJumpTableProof;
  ResolvedTableInfo[Rec.Addr] = Info;

  LLVM_DEBUG({
    llvm::dbgs() << "Jump table @ 0x" << llvm::utohexstr(Rec.Addr) << ": "
                 << Targets.size() << " entries, base=0x"
                 << llvm::utohexstr(Info.BaseAddr)
                 << ", entrySize=" << Info.EntrySize
                 << (Info.IsRelative ? " (relative" : " (absolute")
                 << (Info.IsSigned ? ", signed)" : ")") << "\n";
  });
  return Targets;
}

} // namespace neverd
