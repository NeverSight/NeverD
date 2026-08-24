//===- JumpTableResolverTwoLevel.cpp - Two-level index tables -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Recognizer for the two-level index-byte jump table — the classic MSVC
/// sparse-switch lowering `jmptab[idxtab[switchvar]]`, where a narrow index
/// table maps the switch variable onto a slot of the real address table.  It
/// composes the per-case targets explicitly, since dispatching on the
/// intermediate table index instead of the switch variable would collapse the
/// case set.  Single-level composite shapes live in
/// JumpTableResolverShapes.cpp.
///
/// Part of the CFGBuilder jump-table resolver; see JumpTableResolver.cpp for
/// top-level strategy dispatch and JumpTableResolverDetail.h for shared
/// backward-slicing helpers.
///
//===----------------------------------------------------------------------===//

#include "JumpTableResolverDetail.h"

#include "neverd/Limits.h"
#include "neverd/ir/low/CFGBuilder.h"

#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <set>
#include <utility>
#include <vector>

#define DEBUG_TYPE "neverd-cfg-builder"

namespace neverd {

//===----------------------------------------------------------------------===//
// tryTwoLevelIndexTable — index-byte (MSVC-style) two-level table
//===----------------------------------------------------------------------===//

/// Count the run of consecutive relocation slots in \p Slots starting at
/// \p TableAddr, stepping by \p EntrySize.  Mirrors the code/rel-code run
/// counters in JumpTableResolver.cpp for a locally-supplied slot set.
static uint32_t relocRunIn(const std::set<uint64_t> &Slots, va_t TableAddr,
                           uint16_t EntrySize) {
  if (EntrySize == 0 || Slots.empty())
    return 0;
  uint32_t Run = 0;
  for (va_t VA = TableAddr; Run < limits::kMaxJumpTableEntries;) {
    if (!Slots.count(VA))
      break;
    ++Run;
    if (EntrySize > InvalidVA - VA)
      break;
    VA += EntrySize;
  }
  return Run;
}

/// Decompose the address of an *index-table* load (`idxtab + switchvar[*s1]`)
/// into its constant table base and the index register.  Unlike
/// analyzeTableLoadAddr this tolerates an unscaled index (a byte index table
/// has scale 1, so there is no INT_MULT/INT_LEFT to key on) and folds one
/// operand to a constant read-only VA to identify the table base.  Returns
/// true and sets \p TableAddr (folded base), \p IndexReg (traced to a plain
/// register), and \p Scale (1 or the entry width) on success.
bool CFGBuilder::decomposeIndexTableLoadAddr(
    const BinaryImage &Img, const InsnRecord &Rec,
    const std::vector<LowOp> &Ops, int LoadIdx, uint16_t EntryWidth,
    va_t &TableAddr, uint64_t &IndexReg, uint32_t &Scale, NdVar *IndexValue,
    va_t *IndexUseAddr, int *IndexUseSeq) const {
  if (LoadIdx <= 0 || LoadIdx >= static_cast<int>(Ops.size()))
    return false;
  const LowOp &L = Ops[LoadIdx];
  const NdVar &AddrV = (L.NumInputs >= 2) ? L.Inputs[1] : L.Inputs[0];
  int AddIdx = reachingDefIdx(Ops, LoadIdx - 1, AddrV);
  for (int G = 0;
       AddIdx >= 0 && Ops[AddIdx].Opcode == NdOp::COPY &&
       Ops[AddIdx].NumInputs >= 1 &&
       (Ops[AddIdx].Inputs[0].isReg() || Ops[AddIdx].Inputs[0].isTemp()) &&
       G < limits::kMaxQuasiCopyDepth;
       ++G)
    AddIdx = reachingDefIdx(Ops, AddIdx - 1, Ops[AddIdx].Inputs[0]);
  if (AddIdx < 0 || Ops[AddIdx].Opcode != NdOp::INT_ADD ||
      Ops[AddIdx].NumInputs < 2)
    return false;
  va_t LoadAddr = L.Addr;

  // One operand is the (constant / foldable) table base; the other is the
  // switch-variable index, optionally scaled by the entry width.
  for (int BaseW = 0; BaseW < 2; ++BaseW) {
    const NdVar &BaseV = Ops[AddIdx].Inputs[BaseW];
    const NdVar &IdxV = Ops[AddIdx].Inputs[1 - BaseW];

    va_t Base = 0;
    if (BaseV.isConst()) {
      Base = BaseV.Offset;
    } else if (BaseV.isReg() || BaseV.isTemp()) {
      uint64_t BaseReg = traceToRegister(Ops, AddIdx - 1, BaseV);
      if (BaseReg == InvalidVA)
        continue;
      auto Folded = foldRegConstant(Img, Rec, BaseReg, LoadAddr);
      if (!Folded)
        continue;
      Base = *Folded;
    } else {
      continue;
    }
    if (!Img.getSegmentFor(Base))
      continue;

    // The index may be scaled (halfword index table: `idx*2`) or plain (byte
    // index table: scale 1).  Require the scale to equal the entry width.
    uint32_t S = 1;
    NdVar CandidateValue;
    va_t CandidateUseAddr = InvalidVA;
    int CandidateUseSeq = -1;
    uint64_t IdxReg = scaledIndexReg(Ops, AddIdx - 1, IdxV, &CandidateValue,
                                     &CandidateUseAddr, &CandidateUseSeq);
    if (IdxReg != InvalidVA) {
      // Recover the concrete scale so it can be validated against EntryWidth.
      int SD = reachingDefIdx(Ops, AddIdx - 1, IdxV);
      for (int G = 0;
           SD >= 0 &&
           (Ops[SD].Opcode == NdOp::COPY || Ops[SD].Opcode == NdOp::INT_ZEXT ||
            Ops[SD].Opcode == NdOp::INT_SEXT) &&
           Ops[SD].NumInputs >= 1 && G < limits::kMaxQuasiCopyDepth;
           ++G)
        SD = reachingDefIdx(Ops, SD - 1, Ops[SD].Inputs[0]);
      if (SD < 0)
        continue;
      if (Ops[SD].Opcode == NdOp::INT_MULT && Ops[SD].NumInputs >= 2 &&
          Ops[SD].Inputs[1].isConst())
        S = static_cast<uint32_t>(Ops[SD].Inputs[1].Offset);
      else if (Ops[SD].Opcode == NdOp::INT_LEFT && Ops[SD].NumInputs >= 2 &&
               Ops[SD].Inputs[1].isConst() && Ops[SD].Inputs[1].Offset < 6)
        S = 1u << Ops[SD].Inputs[1].Offset;
      else
        continue;
    } else {
      IdxReg = traceToRegister(Ops, AddIdx - 1, IdxV);
      if (IdxReg == InvalidVA)
        continue;
      CandidateValue = IdxV;
      CandidateUseAddr = Ops[AddIdx].Addr;
      CandidateUseSeq = Ops[AddIdx].Seq;
    }
    if (S != EntryWidth)
      continue;

    TableAddr = Base;
    IndexReg = IdxReg;
    Scale = S;
    if (IndexValue)
      *IndexValue = CandidateValue;
    if (IndexUseAddr)
      *IndexUseAddr = CandidateUseAddr;
    if (IndexUseSeq)
      *IndexUseSeq = CandidateUseSeq;
    return true;
  }
  return false;
}

bool CFGBuilder::tryTwoLevelIndexTable(const BinaryImage &Img,
                                       const InsnRecord &Rec,
                                       JumpTableInfo &Info,
                                       size_t *CandidateEvidenceBudget) {
  if (!CurrentImg)
    return false;
  bool HasIndBranch = false;
  for (auto &Op : Rec.Ops)
    if (Op.Opcode == NdOp::INDIR_BR && Op.NumInputs >= 1) {
      HasIndBranch = true;
      break;
    }
  if (!HasIndBranch)
    return false;

  // Flatten the dispatch block plus its single-predecessor path so both chained
  // loads (the index-table load in a predecessor goto-site block and the
  // address-table load at the branch) are visible to one backward scan.
  va_t BlkStart = CurrentFuncEntry;
  auto BIt = BlockStarts.upper_bound(Rec.Addr);
  if (BIt != BlockStarts.begin()) {
    --BIt;
    BlkStart = *BIt;
  }
  std::vector<LowOp> Ops = collectPathOps(BlkStart, Rec.Addr);
  if (Ops.empty())
    return false;

  // 1) Locate the address-table (jmptab) load: the last pointer-width scaled
  //    load feeding the branch, `jmptab + entryIdx*W2`.
  uint64_t JmpBaseReg = InvalidVA, EntryIdxReg = InvalidVA;
  uint16_t W2 = 0;
  int JmpLoadIdx = -1;
  {
    uint64_t Disp = 0;
    bool Scaled = false;
    for (int I = static_cast<int>(Ops.size()) - 1; I >= 0; --I) {
      const LowOp &L = Ops[I];
      if (L.Opcode != NdOp::LOAD || L.NumInputs < 1)
        continue;
      uint16_t W = L.Output.Size;
      if (W != 4 && W != 8)
        continue;
      const NdVar &AddrV = (L.NumInputs >= 2) ? L.Inputs[1] : L.Inputs[0];
      if (!AddrV.isReg() && !AddrV.isTemp())
        continue;
      if (analyzeTableLoadAddr(Ops, I - 1, AddrV, JmpBaseReg, EntryIdxReg,
                               Scaled, Disp) &&
          Scaled) {
        W2 = W;
        JmpLoadIdx = I;
        break;
      }
    }
  }
  if (JmpLoadIdx < 0 || EntryIdxReg == InvalidVA || W2 == 0)
    return false;

  // 2) The jmptab index must itself be the *value loaded* by a compact
  //    byte/halfword index-table load — trace it (through value-preserving
  //    reshapes only) to a LOAD of width 1 or 2.  Anything else (arithmetic on
  //    the index, a plain register) is not a two-level table.
  int IdxLoadIdx = -1;
  uint16_t W1 = 0;
  {
    NdVar V = NdVar::reg(EntryIdxReg, 8);
    int From = JmpLoadIdx - 1;
    for (int Hop = 0; Hop < limits::kMaxQuasiCopyDepth; ++Hop) {
      int D = reachingDefIdx(Ops, From, V);
      if (D < 0)
        break;
      const LowOp &O = Ops[D];
      if (O.Opcode == NdOp::LOAD) {
        W1 = O.Output.Size;
        IdxLoadIdx = D;
        break;
      }
      if ((O.Opcode == NdOp::COPY || O.Opcode == NdOp::INT_ZEXT ||
           O.Opcode == NdOp::INT_SEXT) &&
          O.NumInputs >= 1 && (O.Inputs[0].isReg() || O.Inputs[0].isTemp())) {
        V = O.Inputs[0];
        From = D - 1;
        continue;
      }
      if (O.Opcode == NdOp::SUBBYTES && O.NumInputs >= 2 &&
          O.Inputs[1].isConst() && O.Inputs[1].Offset == 0 &&
          (O.Inputs[0].isReg() || O.Inputs[0].isTemp())) {
        V = O.Inputs[0];
        From = D - 1;
        continue;
      }
      break;
    }
  }
  // A byte/halfword index table is the hallmark of the compaction; a wider
  // "index" is indistinguishable from an ordinary single-level table entry.
  if (IdxLoadIdx < 0 || (W1 != 1 && W1 != 2))
    return false;

  // 3) Decompose the index-table load address into idxtab base + switch var.
  va_t IdxTab = 0;
  uint64_t SwitchIdxReg = InvalidVA;
  NdVar SwitchIndexValue;
  va_t SwitchIndexUseAddr = InvalidVA;
  int SwitchIndexUseSeq = -1;
  uint32_t IdxScale = 1;
  if (!decomposeIndexTableLoadAddr(Img, Rec, Ops, IdxLoadIdx, W1, IdxTab,
                                   SwitchIdxReg, IdxScale, &SwitchIndexValue,
                                   &SwitchIndexUseAddr, &SwitchIndexUseSeq))
    return false;

  // 4) Fold the address-table base and confirm it is distinct from idxtab.
  va_t JmpTab = 0;
  {
    va_t FoldAt = Ops[JmpLoadIdx].Addr;
    auto Folded = foldRegConstant(Img, Rec, JmpBaseReg, FoldAt);
    if (!Folded || !Img.getSegmentFor(*Folded))
      return false;
    JmpTab = *Folded;
  }
  if (JmpTab == IdxTab)
    return false;
  const auto *JmpSeg = Img.getSegmentFor(JmpTab);
  const auto *IdxSeg = Img.getSegmentFor(IdxTab);
  if (!JmpSeg || JmpSeg->Data.empty() || !IdxSeg || IdxSeg->Data.empty())
    return false;
  const std::optional<va_t> JmpOwnerEnd = Img.mappedObjectOwnerEnd(JmpTab);
  const std::optional<va_t> IdxOwnerEnd = Img.mappedObjectOwnerEnd(IdxTab);
  if (!JmpOwnerEnd || !IdxOwnerEnd || *JmpOwnerEnd <= JmpTab ||
      *IdxOwnerEnd <= IdxTab)
    return false;
  // The index table lives in read-only data; a writable/executable "idxtab"
  // would not be a compiler-emitted constant index table.
  if (IdxSeg->isWritable() || Img.isCodeAddress(IdxTab))
    return false;

  // 5) The address table's signature: a run of loader-applied code-pointer
  //    relocations (absolute) or PC-relative-to-code relocations (relative).
  //    The run length M is the exact address-table entry count, and every
  //    idxtab byte must be < M — the constraint that distinguishes a genuine
  //    two-level table from an unrelated pair of chained loads.
  bool Relative = false;
  uint32_t M = relocRunIn(Img.CodePtrRelocSlots, JmpTab, W2);
  if (M < limits::kMinJumpTableEntries) {
    uint32_t RM = relocRunIn(Img.RelCodeRelocSlots, JmpTab, W2);
    if (RM >= limits::kMinJumpTableEntries) {
      M = RM;
      Relative = true;
    }
  }
  if (M < limits::kMinJumpTableEntries)
    return false;
  const uint64_t OwnerEntries = (*JmpOwnerEnd - JmpTab) / W2;
  M = static_cast<uint32_t>(std::min<uint64_t>(
      {M, std::numeric_limits<uint32_t>::max(), OwnerEntries}));
  if (M < limits::kMinJumpTableEntries)
    return false;

  // Flatten the whole function prefix so the outer range guard and any
  // comparison of the loaded index value are both visible.
  std::vector<LowOp> Pre;
  for (auto It = Insns.lower_bound(CurrentFuncEntry);
       It != Insns.end() && It->first <= Rec.Addr; ++It)
    for (auto &Op : It->second.Ops)
      Pre.push_back(Op);

  // Locate the index-table load in the function-prefix ops (by address and
  // output nd-var) so the discriminator below can reason about its result.
  int IdxLoadInPre = -1;
  {
    va_t L1Addr = Ops[IdxLoadIdx].Addr;
    const NdVar &L1Out = Ops[IdxLoadIdx].Output;
    for (int I = 0; I < static_cast<int>(Pre.size()); ++I)
      if (Pre[I].Opcode == NdOp::LOAD && Pre[I].Addr == L1Addr &&
          Pre[I].Output.Space == L1Out.Space &&
          Pre[I].Output.Offset == L1Out.Offset &&
          Pre[I].Output.Size == L1Out.Size &&
          Pre[I].Output.isTemp() == L1Out.isTemp())
        IdxLoadInPre = I; // last match at that address wins
  }

  // Discriminator — distinguish a genuine two-level index table from an
  // ordinary `switch(user_array[i])`, which lowers to the *identical* shape
  // (load a value, then index the compiler's jump table by it).  In the latter
  // the loaded value IS the switch variable and is range-guarded as the switch
  // condition (`cmp k, hi; ja default`); dispatching on the outer array index
  // would be wrong.  A compiler-generated index table's value, by contrast, is
  // an opaque index used *only* to address the address table and is never
  // compared.  So bail when the idxtab-loaded value reaches a constant
  // comparison: that marks it as the real switch variable (single-level).
  if (IdxLoadInPre >= 0) {
    auto tracesToIdxLoad = [&](NdVar V, int From) -> bool {
      for (int Hop = 0; Hop < limits::kMaxQuasiCopyDepth; ++Hop) {
        if (!V.isReg() && !V.isTemp())
          return false;
        int D = reachingDefIdx(Pre, From, V);
        if (D < 0)
          return false;
        if (D == IdxLoadInPre)
          return true;
        const LowOp &O = Pre[D];
        if ((O.Opcode == NdOp::COPY || O.Opcode == NdOp::INT_ZEXT ||
             O.Opcode == NdOp::INT_SEXT) &&
            O.NumInputs >= 1 && (O.Inputs[0].isReg() || O.Inputs[0].isTemp())) {
          V = O.Inputs[0];
          From = D - 1;
          continue;
        }
        if (O.Opcode == NdOp::SUBBYTES && O.NumInputs >= 2 &&
            O.Inputs[1].isConst() && O.Inputs[1].Offset == 0 &&
            (O.Inputs[0].isReg() || O.Inputs[0].isTemp())) {
          V = O.Inputs[0];
          From = D - 1;
          continue;
        }
        return false;
      }
      return false;
    };
    for (int I = 0; I < static_cast<int>(Pre.size()); ++I) {
      const LowOp &Op = Pre[I];
      bool IsCompare =
          Op.Opcode == NdOp::INT_LESS || Op.Opcode == NdOp::INT_SLESS ||
          Op.Opcode == NdOp::INT_LESSEQUAL ||
          Op.Opcode == NdOp::INT_SLESSEQUAL || Op.Opcode == NdOp::INT_EQUAL ||
          Op.Opcode == NdOp::INT_NOTEQUAL || Op.Opcode == NdOp::INT_SUB;
      if (!IsCompare || Op.NumInputs < 2)
        continue;
      int CW = Op.Inputs[1].isConst() ? 1 : (Op.Inputs[0].isConst() ? 0 : -1);
      if (CW < 0)
        continue;
      if (tracesToIdxLoad(Op.Inputs[1 - CW], I - 1))
        return false; // loaded value is the switch variable — single-level
    }
  }

  // The two chained physical tables, narrow outer LOAD and relocation-backed
  // inner address table are a distinguishing composite shape.  From here a
  // failed outer-domain or occurrence certificate must not fall through to a
  // generic resolver that would publish only jmptab on the intermediate index.
  Info.CompositeShapeClaimed = true;

  // 6) Bound the number of switch cases (idxtab length).  Prefer an explicit
  //    range guard on the switch variable; otherwise self-bound by the idxtab
  //    entries themselves (each must index a valid jmptab slot).
  //
  // Anchor the switch-variable trace at the index-table load: the register that
  // addresses idxtab (e.g. `rax`) is routinely *reused* after the load to hold
  // the loaded index byte's zero-extension, so tracing from the end of the op
  // list would follow that later reuse to the byte value instead of the real
  // switch variable.  Its reaching definition at the load is the true switch
  // variable (the guarded `x` copied into the address register).
  uint64_t SwitchSrc = traceRegSource(Ops, IdxLoadIdx - 1, SwitchIdxReg);
  Info.IndexReg = SwitchSrc;
  Info.IndexValueAtUse = SwitchIndexValue;
  Info.IndexUseAddr = SwitchIndexUseAddr;
  Info.IndexUseSeq = SwitchIndexUseSeq;
  Info.TableLoadAddr = Ops[IdxLoadIdx].Addr;
  Info.TableLoadSeq = Ops[IdxLoadIdx].Seq;
  Info.TargetLoads = {{Ops[JmpLoadIdx].Output, Ops[JmpLoadIdx].Addr,
                       Ops[JmpLoadIdx].Seq, /*DefinedAtPoint=*/true}};
  // The outer domain must be proved at the exact index-table LOAD use.  A
  // lexical compare on an older lifetime, or scanning until a byte happens to
  // index past jmptab, does not constrain the runtime switch value.  Reuse the
  // same CFG/lane/polarity proof as ordinary tables and require an exact bound
  // before publishing the composed target vector.
  if (!CandidateEvidenceBudget ||
      !inferBoundsFromPreciseGuards(Rec, Info, CandidateEvidenceBudget) ||
      Info.MaxEntries < limits::kMinJumpTableEntries ||
      Info.MaxEntries > limits::kMaxJumpTableEntries)
    return false;
  const uint32_t GuardBound = Info.MaxEntries;

  uint32_t IdxCap = static_cast<uint32_t>(std::min<uint64_t>(
      (*IdxOwnerEnd - IdxTab) / W1, std::numeric_limits<uint32_t>::max()));
  if (GuardBound > IdxCap)
    return false;
  const uint32_t Scan = GuardBound;

  // 7) Compose one target per switch value: idxtab[v] indexes jmptab.
  std::vector<va_t> Targets;
  std::set<uint32_t> InnerSlots;
  Targets.reserve(std::min<uint32_t>(Scan, 64));
  for (uint32_t V = 0; V < Scan; ++V) {
    const uint8_t *IP = Img.readVA(IdxTab + static_cast<uint64_t>(V) * W1, W1);
    if (!IP)
      return false;
    uint32_t Iidx = 0;
    std::memcpy(&Iidx, IP, W1);
    if (Iidx >= M)
      return false;
    InnerSlots.insert(Iidx);
    const uint8_t *EP =
        Img.readVA(JmpTab + static_cast<uint64_t>(Iidx) * W2, W2);
    if (!EP)
      return false;
    auto TargetOpt = decodeTableEntry(
        EP, W2, Relative, Relative, JmpTab, /*HasTargetBase=*/false,
        /*TargetBase=*/0, /*Scale=*/1, Img.getPointerSize());
    if (!TargetOpt)
      return false;
    va_t Target = *TargetOpt;
    if (!Relative) {
      std::optional<va_t> Canonical =
          canonicalizeAbsoluteTableCodeTarget(Img, Target);
      if (!Canonical)
        return false;
      Target = *Canonical;
    }
    if (!isValidTarget(Img, Target, CurrentFuncEntry))
      return false;
    Targets.push_back(Target);
  }

  if (Targets.size() != Scan)
    return false;

  Info.setBaseAddr(JmpTab);
  Info.EntrySize = W2;
  Info.IsRelative = Relative;
  Info.IsSigned = Relative;
  Info.IndexReg = SwitchSrc;
  Info.TwoLevelIndex = true;
  Info.IndexDomainAuthenticated = true;
  Info.ExplicitTargets = std::move(Targets);
  Info.StorageRanges = {
      JumpTableStorageRange{IdxTab, W1, W1, Info.ExplicitTargets.size()}};
  // M is authenticated capacity for the inner address table, not proof that
  // every slot in [0, M) belongs to this dispatch.  The outer index table may
  // select a sparse subset while the gaps hold another table or independent
  // function pointers, so publish ownership only for the actually selected
  // inner slots.
  for (uint32_t InnerSlot : InnerSlots) {
    if (InnerSlot != 0 && W2 > (InvalidVA - JmpTab) / InnerSlot)
      return false;
    Info.StorageRanges.push_back(
        JumpTableStorageRange{JmpTab + uint64_t(InnerSlot) * W2, W2, W2, 1});
  }
  JumpTableLoadRole OuterRole;
  OuterRole.Load = {Ops[IdxLoadIdx].Output, Ops[IdxLoadIdx].Addr,
                    Ops[IdxLoadIdx].Seq, /*DefinedAtPoint=*/true};
  OuterRole.LoadWidth = W1;
  OuterRole.AllowedBases = {IdxTab};
  OuterRole.Indices = {{SwitchIndexValue, SwitchIndexUseAddr, SwitchIndexUseSeq,
                        /*DefinedAtPoint=*/false}};
  OuterRole.AddressScale = IdxScale;

  JumpTableLoadRole InnerRole;
  InnerRole.Load = {Ops[JmpLoadIdx].Output, Ops[JmpLoadIdx].Addr,
                    Ops[JmpLoadIdx].Seq, /*DefinedAtPoint=*/true};
  InnerRole.LoadWidth = W2;
  InnerRole.AllowedBases = {JmpTab};
  InnerRole.Indices = {{Ops[IdxLoadIdx].Output, Ops[IdxLoadIdx].Addr,
                        Ops[IdxLoadIdx].Seq, /*DefinedAtPoint=*/true}};
  InnerRole.AddressScale = W2;
  InnerRole.AllowZeroExtension = true;
  InnerRole.AllowSignExtension = false;
  Info.LoadRoles = {std::move(OuterRole), std::move(InnerRole)};
  LLVM_DEBUG(llvm::dbgs() << "  two-level: idxtab 0x" << llvm::utohexstr(IdxTab)
                          << " (W1=" << W1 << ") -> jmptab 0x"
                          << llvm::utohexstr(JmpTab) << " (W2=" << W2
                          << ", M=" << M << "), " << Info.ExplicitTargets.size()
                          << " cases\n");
  return true;
}

} // namespace neverd
