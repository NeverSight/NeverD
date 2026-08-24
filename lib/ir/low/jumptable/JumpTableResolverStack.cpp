//===- JumpTableResolverStack.cpp - Stack table materialization ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Stack-spill forwarding and recovery of jump tables materialized into local
/// frame storage from a constant initializer run.
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
#include "neverd/libc/LibCNames.h"
#include "neverd/loader/PointerRelocation.h"

#include "llvm/ADT/ScopeExit.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <tuple>
#include <vector>

namespace neverd {

namespace {

std::optional<int64_t> stackSignedDelta(const NdVar &Value,
                                        uint16_t ArithmeticSize) {
  if (!Value.isConst() || Value.Size == 0 || Value.Size > sizeof(uint64_t) ||
      ArithmeticSize == 0 || ArithmeticSize > sizeof(uint64_t) ||
      Value.Provenance != ConstantAddressProvenance::Scalar)
    return std::nullopt;
  const unsigned SourceBits = static_cast<unsigned>(Value.Size) * 8;
  const unsigned ArithmeticBits = static_cast<unsigned>(ArithmeticSize) * 8;
  const uint64_t SourceMask = SourceBits == 64
                                  ? std::numeric_limits<uint64_t>::max()
                                  : (uint64_t{1} << SourceBits) - 1;
  const uint64_t ArithmeticMask = ArithmeticBits == 64
                                      ? std::numeric_limits<uint64_t>::max()
                                      : (uint64_t{1} << ArithmeticBits) - 1;
  const uint64_t Raw = (Value.Offset & SourceMask) & ArithmeticMask;
  const uint64_t Sign = uint64_t{1} << (ArithmeticBits - 1);
  if ((Raw & Sign) == 0)
    return static_cast<int64_t>(Raw);
  return -1 - static_cast<int64_t>((~Raw) & ArithmeticMask);
}

} // namespace

std::optional<int64_t> stackCheckedOffset(int64_t Base, int64_t Delta,
                                          bool Subtract) {
  constexpr int64_t Min = std::numeric_limits<int64_t>::min();
  constexpr int64_t Max = std::numeric_limits<int64_t>::max();
  if (!Subtract) {
    if ((Delta > 0 && Base > Max - Delta) || (Delta < 0 && Base < Min - Delta))
      return std::nullopt;
    return Base + Delta;
  }
  if ((Delta > 0 && Base < Min + Delta) || (Delta < 0 && Base > Max + Delta))
    return std::nullopt;
  return Base - Delta;
}

std::optional<va_t> checkedVAOffset(va_t Base, int64_t Delta) {
  if (Delta >= 0) {
    const uint64_t Amount = static_cast<uint64_t>(Delta);
    if (Amount > InvalidVA - Base)
      return std::nullopt;
    return Base + Amount;
  }
  const uint64_t Amount = uint64_t{0} - static_cast<uint64_t>(Delta);
  if (Amount > Base)
    return std::nullopt;
  return Base - Amount;
}

std::optional<va_t>
exactImmutableDataSpanOwner(const BinaryImage &Img, va_t Start, uint64_t Size,
                            va_t ExpectedOwner) {
  if (Size == 0 || Size - 1 > InvalidVA - Start)
    return std::nullopt;
  const va_t Last = Start + Size - 1;
  if (!Img.hasObjectDataProvenance(Start) ||
      !Img.hasObjectDataProvenance(Last) ||
      Img.hasExecutableCodeOwnerAt(Start) ||
      Img.hasExecutableCodeOwnerAt(Last) ||
      isRuntimeWritableAddress(Img, Start) ||
      isRuntimeWritableAddress(Img, Last))
    return std::nullopt;

  const Section *SectionOwner = nullptr;
  for (const Section &Section : Img.Sections) {
    if (!Section.isReadable() || Section.Size == 0 ||
        Section.Size > InvalidVA - Section.VA)
      continue;
    const va_t End = Section.VA + Section.Size;
    if (Start >= End || Section.VA > Last)
      continue;
    if (SectionOwner || Start < Section.VA || Last >= End ||
        Section.isExecutable())
      return std::nullopt;
    SectionOwner = &Section;
  }
  if (SectionOwner) {
    if (ExpectedOwner != InvalidVA && SectionOwner->VA != ExpectedOwner)
      return std::nullopt;
    return SectionOwner->VA;
  }

  const Segment *StartSegment = Img.getSegmentFor(Start);
  const Segment *LastSegment = Img.getSegmentFor(Last);
  if (!StartSegment || StartSegment != LastSegment ||
      Img.segmentHasReadableSectionMetadata(*StartSegment) ||
      !StartSegment->isReadable() || StartSegment->isExecutable() ||
      StartSegment->isWritable() || Start < StartSegment->VA ||
      Last - StartSegment->VA >= StartSegment->Data.size() ||
      (ExpectedOwner != InvalidVA && StartSegment->VA != ExpectedOwner))
    return std::nullopt;

  const Segment *SegmentOwner = nullptr;
  for (const Segment &Segment : Img.Segments) {
    if (!Segment.isReadable() || Segment.Size == 0 ||
        Segment.Size > InvalidVA - Segment.VA)
      continue;
    const va_t End = Segment.VA + Segment.Size;
    if (Start >= End || Segment.VA > Last)
      continue;
    if (SegmentOwner || Start < Segment.VA || Last >= End ||
        Segment.isExecutable() || Segment.isWritable())
      return std::nullopt;
    SegmentOwner = &Segment;
  }
  if (SegmentOwner != StartSegment)
    return std::nullopt;
  return StartSegment->VA;
}

//===----------------------------------------------------------------------===//
// forwardIndexThroughStackSpill — reconnect a guarded spilled index
//===----------------------------------------------------------------------===//

uint64_t
CFGBuilder::forwardIndexThroughStackSpill(const std::vector<LowOp> &BlockOps,
                                          int LoadIdx, uint64_t IndexReg,
                                          va_t BlkStart) const {
  if (IndexReg == InvalidVA || LoadIdx <= 0 || !CurrentImg)
    return IndexReg;
  const TargetRegInfo &TRI = getTargetRegInfo(CurrentImg->Arch);

  // Trace the index register's reaching def (before the table load) to an
  // underlying LOAD, through a couple of COPY/extend hops — the -O0 reload of a
  // spilled switch variable (`ldr rIdx,[sp,#k]`).
  NdVar Cur = NdVar::reg(IndexReg, 4);
  int From = LoadIdx - 1;
  int LdIdx = -1;
  for (int Hop = 0; Hop < limits::kMaxQuasiCopyDepth; ++Hop) {
    int D = reachingDefIdx(BlockOps, From, Cur);
    if (D < 0)
      return IndexReg;
    const LowOp &O = BlockOps[D];
    if (O.Opcode == NdOp::LOAD) {
      LdIdx = D;
      break;
    }
    if ((O.Opcode == NdOp::COPY || O.Opcode == NdOp::INT_ZEXT ||
         O.Opcode == NdOp::INT_SEXT) &&
        O.NumInputs >= 1 && (O.Inputs[0].isReg() || O.Inputs[0].isTemp())) {
      Cur = O.Inputs[0];
      From = D - 1;
      continue;
    }
    return IndexReg;
  }
  if (LdIdx < 0)
    return IndexReg;

  // The reload address must be a plain SP/FP slot; capture its (base, offset).
  const LowOp &Ld = BlockOps[LdIdx];
  NdVar AddrV = (Ld.NumInputs >= 2) ? Ld.Inputs[1] : Ld.Inputs[0];
  uint64_t SlotBase = InvalidVA;
  int64_t SlotOff = 0;
  if (!frameSlotKey(BlockOps, LdIdx - 1, AddrV, TRI, SlotBase, SlotOff))
    return IndexReg;

  // The value stored to the slot is often a *copy* of the guarded switch
  // variable (`and eax,7; mov ecx,eax; mov [slot],ecx`), and that intermediate
  // register (ecx) is frequently reused as the table base at the dispatch
  // (`lea rcx,[rip]; mov (rcx,idx,4)`).  Forwarding the index to that clobbered
  // copy loses both the range guard and the index mask (which live on the
  // original eax), so the bound defaults to the raw relocation run and a table
  // adjacent in rodata is over-read.  Trace the stored value to its ultimate
  // copy source instead: guardConstrainsIndex traces a guard operand back to
  // its own source, so a guard on any register in the copy chain still matches
  // the earliest source, and that source is not the reused base.
  auto ultimateSource = [&](uint64_t Reg, va_t StoreAddr) -> uint64_t {
    std::vector<LowOp> Pre;
    for (auto PIt = Insns.lower_bound(CurrentFuncEntry);
         PIt != Insns.end() && PIt->first <= StoreAddr; ++PIt)
      for (auto &PO : PIt->second.Ops)
        Pre.push_back(PO);
    uint64_t Ult = traceRegSource(Pre, static_cast<int>(Pre.size()) - 1, Reg);
    return (Ult != InvalidVA) ? Ult : Reg;
  };

  // A store to the same slot earlier in the dispatch block takes precedence
  // over any predecessor store (it is the value the reload actually observes).
  for (int I = LdIdx - 1; I >= 0; --I) {
    const LowOp &Op = BlockOps[I];
    if (Op.Opcode != NdOp::STORE || Op.NumInputs < 2)
      continue;
    uint64_t B = InvalidVA;
    int64_t Off = 0;
    if (!frameSlotKey(BlockOps, I - 1, Op.Inputs[0], TRI, B, Off))
      continue;
    if (B != SlotBase || Off != SlotOff)
      continue;
    uint64_t Src = traceToRegister(BlockOps, I - 1, Op.Inputs[1]);
    if (Src == InvalidVA)
      return IndexReg;
    return ultimateSource(Src, BlockOps[I].Addr);
  }

  // Otherwise the spill is in a predecessor — the nearest STORE to the same
  // slot before the dispatch block is the guarded switch variable.
  for (auto It = Insns.lower_bound(BlkStart); It != Insns.begin();) {
    --It;
    const InsnRecord &IR = It->second;
    for (int I = static_cast<int>(IR.Ops.size()) - 1; I >= 0; --I) {
      const LowOp &Op = IR.Ops[I];
      if (Op.Opcode != NdOp::STORE || Op.NumInputs < 2)
        continue;
      uint64_t B = InvalidVA;
      int64_t Off = 0;
      if (!frameSlotKey(IR.Ops, I - 1, Op.Inputs[0], TRI, B, Off))
        continue;
      if (B != SlotBase || Off != SlotOff)
        continue;
      uint64_t Src = traceToRegister(IR.Ops, I - 1, Op.Inputs[1]);
      if (Src == InvalidVA)
        return IndexReg;
      return ultimateSource(Src, It->first);
    }
  }
  return IndexReg;
}

//===----------------------------------------------------------------------===//
// resolveStackMaterializedTableSource — recover a local table initializer
//===----------------------------------------------------------------------===//

va_t CFGBuilder::resolveStackMaterializedTableSource(
    const BinaryImage &Img, const InsnRecord &Rec,
    const std::vector<LowOp> &Ops, int LoadIdx, uint64_t BaseReg,
    uint16_t LoadWidth, int64_t TableDisp, bool *MutatedOut,
    std::vector<JumpTableFrameInitializerChunk> *InitializersOut,
    std::vector<JumpTableValueOccurrence> *StorageConsumersOut) const {
  if (MutatedOut)
    *MutatedOut = false;
  if (InitializersOut)
    InitializersOut->clear();
  if (StorageConsumersOut)
    StorageConsumersOut->clear();
  if (BaseReg == InvalidVA || LoadIdx < 0 || LoadWidth == 0 || !CurrentImg)
    return InvalidVA;
  if (StackTableEvidenceIncompleteBranches.count(Rec.Addr))
    return InvalidVA;

  bool EvidenceComplete = true;
  auto chargeEvidence = [&](size_t Amount = 1) {
    if (!EvidenceComplete)
      return false;
    if (consumeStackTableEvidence(Amount))
      return true;
    EvidenceComplete = false;
    return false;
  };
  llvm::scope_exit MarkIncomplete([&] {
    if (!EvidenceComplete)
      StackTableEvidenceIncompleteBranches.insert(Rec.Addr);
  });

  const TargetRegInfo &TRI = getTargetRegInfo(CurrentImg->Arch);
  const llvm::ArrayRef<uint64_t> IntParamRegs =
      TRI.integerParamRegs(CurrentImg->Format);
  auto isGuestAddressZExt = [&](const LowOp &Op) {
    return Op.Opcode == NdOp::INT_ZEXT && Op.NumInputs >= 1 &&
           Op.Inputs[0].Size == CurrentImg->getPointerSize() &&
           Op.Output.Size > Op.Inputs[0].Size &&
           (Op.Inputs[0].isReg() || Op.Inputs[0].isTemp());
  };

  // Index exact linear definitions once per immutable trace.  Repeated
  // backwards queries used to rescan the same prefix for every frame chunk and
  // could consume the complete evidence allowance quadratically on ordinary
  // -O0 functions before final revalidation ran.  Building this index charges
  // every retained definition scan once; each lookup is charged separately,
  // so adversarial query counts remain bounded without charging duplicate
  // lexical work as new evidence.
  using LinearDefKey = std::pair<VnodeSpace, uint64_t>;
  using LinearDefIndex = std::map<LinearDefKey, std::vector<int>>;
  std::map<const std::vector<LowOp> *, LinearDefIndex> LinearDefs;
  auto budgetedReachingDefIdx = [&](const std::vector<LowOp> &TraceOps,
                                    int FromIdx, const NdVar &Value) {
    auto [IndexIt, Inserted] = LinearDefs.try_emplace(&TraceOps);
    if (Inserted) {
      LinearDefIndex &Index = IndexIt->second;
      for (int I = 0; I < static_cast<int>(TraceOps.size()); ++I) {
        if (!chargeEvidence()) {
          LinearDefs.erase(IndexIt);
          return -1;
        }
        const NdVar &Output = TraceOps[I].Output;
        if (Output.isReg() || Output.isTemp())
          Index[{Output.Space, Output.Offset}].push_back(I);
      }
    }
    if (!chargeEvidence())
      return -1;
    const auto DefsIt = IndexIt->second.find({Value.Space, Value.Offset});
    if (DefsIt == IndexIt->second.end())
      return -1;
    const int Cutoff =
        std::min(FromIdx, static_cast<int>(TraceOps.size()) - 1);
    auto Def = std::upper_bound(DefsIt->second.begin(), DefsIt->second.end(),
                                Cutoff);
    if (Def == DefsIt->second.begin())
      return -1;
    --Def;
    return *Def;
  };
  auto budgetedTraceIndexToRegister = [&](const std::vector<LowOp> &TraceOps,
                                          int FromIdx, NdVar Value) {
    for (int Depth = 0; Depth < limits::kMaxQuasiCopyDepth; ++Depth) {
      if (!chargeEvidence())
        return InvalidVA;
      if (Value.isReg())
        return Value.Offset;
      if (!Value.isTemp())
        return InvalidVA;
      const int Def = budgetedReachingDefIdx(TraceOps, FromIdx, Value);
      if (Def < 0)
        return InvalidVA;
      const LowOp &Op = TraceOps[Def];
      if ((Op.Opcode == NdOp::COPY || Op.Opcode == NdOp::INT_ZEXT ||
           Op.Opcode == NdOp::INT_SEXT) &&
          Op.NumInputs >= 1) {
        Value = Op.Inputs[0];
      } else if (Op.Opcode == NdOp::SUBBYTES && Op.NumInputs >= 2 &&
                 Op.Inputs[1].isConst() && Op.Inputs[1].Offset == 0) {
        Value = Op.Inputs[0];
      } else {
        return InvalidVA;
      }
      FromIdx = Def - 1;
    }
    return InvalidVA;
  };
  auto budgetedScaledIndexReg = [&](const std::vector<LowOp> &TraceOps,
                                    int FromIdx, NdVar Value) {
    for (int Depth = 0; Depth < limits::kMaxQuasiCopyDepth; ++Depth) {
      if (!chargeEvidence())
        return InvalidVA;
      if (!Value.isTemp() && !Value.isReg())
        return InvalidVA;
      const int Def = budgetedReachingDefIdx(TraceOps, FromIdx, Value);
      if (Def < 0)
        return InvalidVA;
      const LowOp &Op = TraceOps[Def];
      const bool Scaled = (Op.Opcode == NdOp::INT_MULT && Op.NumInputs >= 2 &&
                           Op.Inputs[1].isConst() && Op.Inputs[1].Offset > 1) ||
                          (Op.Opcode == NdOp::INT_LEFT && Op.NumInputs >= 2 &&
                           Op.Inputs[1].isConst() && Op.Inputs[1].Offset >= 1);
      if (Scaled)
        return budgetedTraceIndexToRegister(TraceOps, Def - 1, Op.Inputs[0]);
      if ((Op.Opcode != NdOp::COPY && Op.Opcode != NdOp::INT_ZEXT &&
           Op.Opcode != NdOp::INT_SEXT) ||
          Op.NumInputs < 1)
        return InvalidVA;
      Value = Op.Inputs[0];
      FromIdx = Def - 1;
    }
    return InvalidVA;
  };
  auto budgetedCodePtrRelocRun = [&](va_t TableAddr) {
    if (LoadWidth == 0 || Img.CodePtrRelocSlots.empty())
      return uint32_t{0};
    uint32_t Run = 0;
    va_t Address = TableAddr;
    while (Run < limits::kMaxJumpTableEntries) {
      if (!chargeEvidence())
        return uint32_t{0};
      if (!Img.CodePtrRelocSlots.count(Address))
        break;
      ++Run;
      if (LoadWidth > InvalidVA - Address)
        break;
      Address += LoadWidth;
    }
    return Run;
  };

  // Flatten the function prefix up to the dispatch first: the init store and
  // its source LOAD live in the entry block (not the single-predecessor
  // dispatch path), and at -O2 the table-base register itself (`x10 = sp`) is
  // defined in the entry block while the LOAD/branch sit in the dispatch block,
  // so the base must be traceable over the whole prefix too.
  std::vector<LowOp> FuncOps;
  LinearDefIndex FuncLinearDefs;
  int LoadPosInFunc = -1;
  const LowOp *DispatchLoad =
      LoadIdx >= 0 && LoadIdx < static_cast<int>(Ops.size()) ? &Ops[LoadIdx]
                                                              : nullptr;
  for (auto It = Insns.lower_bound(CurrentFuncEntry);
       It != Insns.end() && It->first <= Rec.Addr; ++It) {
    if (!chargeEvidence())
      return InvalidVA;
    for (auto &Op : It->second.Ops) {
      if (!chargeEvidence())
        return InvalidVA;
      const int OpIndex = static_cast<int>(FuncOps.size());
      FuncOps.push_back(Op);
      if (Op.Output.isReg() || Op.Output.isTemp())
        FuncLinearDefs[{Op.Output.Space, Op.Output.Offset}].push_back(OpIndex);
      if (DispatchLoad && Op.Opcode == NdOp::LOAD &&
          Op.Addr == DispatchLoad->Addr && Op.Seq == DispatchLoad->Seq &&
          Op.Output == DispatchLoad->Output) {
        if (LoadPosInFunc >= 0)
          return InvalidVA;
        LoadPosInFunc = OpIndex;
      }
    }
  }
  if (LoadPosInFunc < 0)
    return InvalidVA;
  LinearDefs.emplace(&FuncOps, std::move(FuncLinearDefs));

  // A GOTOFF displacement is only an address when the loader recorded the
  // exact relocated operand inside the instruction that produced it.  This is
  // deliberately occurrence-based: a scalar with the same numeric value is
  // not a provisional table source.  The final value query additionally proves
  // that the matching GOTPC model-zero producer reaches this use.
  auto exactI386DataAddressAt = [&](const LowOp *Use, int ConstantSide,
                                    va_t InsnAddr,
                                    va_t Candidate) -> std::optional<va_t> {
    if (Img.Arch != Arch::X86 || !Img.isELF() || Img.getPointerSize() != 4)
      return std::nullopt;
    auto InsnIt = Insns.find(InsnAddr);
    if (InsnIt == Insns.end() || InsnIt->second.Size == 0 ||
        InsnIt->second.Size > InvalidVA - InsnAddr)
      return std::nullopt;
    const va_t End = InsnAddr + InsnIt->second.Size;
    const RelocatedAddressField *OnlyField = nullptr;
    va_t OnlyFieldVA = InvalidVA;
    unsigned FieldCount = 0;
    auto Field = Img.DataAddressRelocOperands.lower_bound(InsnAddr);
    for (; Field != Img.DataAddressRelocOperands.end() && Field->first < End;
         ++Field) {
      if (!chargeEvidence())
        return std::nullopt;
      ++FieldCount;
      OnlyField = &Field->second;
      OnlyFieldVA = Field->first;
    }
    if (FieldCount != 1 || !OnlyField ||
        OnlyField->Kind != RelocatedAddressFieldKind::I386ELFGOTOFF ||
        OnlyField->Width != Img.getPointerSize() ||
        OnlyField->TargetVA != Candidate ||
        OnlyField->TargetOwnerVA == InvalidVA ||
        !Img.relocatedI386GOTOFFTargetBelongsToOwner(OnlyField->TargetVA,
                                                     OnlyField->TargetOwnerVA))
      return std::nullopt;

    const RelocatedInstructionAddressOccurrence *Exact = nullptr;
    for (const RelocatedInstructionAddressOccurrence &Occurrence :
         RelocatedInstructionAddressOccurrences) {
      if (!chargeEvidence())
        return std::nullopt;
      if (Occurrence.FieldVA != OnlyFieldVA ||
          Occurrence.InstructionAddr != InsnAddr ||
          Occurrence.Width != OnlyField->Width ||
          Occurrence.TargetVA != OnlyField->TargetVA ||
          Occurrence.TargetOwnerVA != OnlyField->TargetOwnerVA ||
          Occurrence.Provenance != ConstantAddressProvenance::DataAddress ||
          Occurrence.PCRelativeFromInstructionEnd || Occurrence.DefinesOutput ||
          Occurrence.OutputMayDepend || Occurrence.InputIndex < 0)
        continue;
      if (Use && (Occurrence.OpSeq != Use->Seq ||
                  Occurrence.InstructionAddr != Use->Addr ||
                  Occurrence.InputIndex != ConstantSide))
        continue;
      if (Exact)
        return std::nullopt;
      Exact = &Occurrence;
    }
    if (!Exact)
      return std::nullopt;
    const LowOp *ExactOp = nullptr;
    for (const LowOp &Op : InsnIt->second.Ops) {
      if (!chargeEvidence())
        return std::nullopt;
      if (Op.Addr == Exact->InstructionAddr && Op.Seq == Exact->OpSeq &&
          Exact->InputIndex < Op.NumInputs &&
          Op.Inputs[Exact->InputIndex].isConst() &&
          Op.Inputs[Exact->InputIndex].Offset == OnlyField->TargetVA &&
          Op.Inputs[Exact->InputIndex].Provenance ==
              ConstantAddressProvenance::DataAddress) {
        if (ExactOp)
          return std::nullopt;
        ExactOp = &Op;
      }
    }
    return ExactOp ? std::optional<va_t>(OnlyField->TargetVA) : std::nullopt;
  };

  // Resolve a value (a register holding a computed address, or a load/store
  // address temp) to a frame slot (frame register + signed byte offset) by
  // tracing COPY / INT_ADD(const) / INT_SUB(const).  Unlike the shared
  // frameSlotKey this *follows stack-pointer redefinitions*: a pre-indexed
  // store/alloc (`stp q0,q1,[sp,#-0x30]!`, common at -O2) mutates SP mid
  // function, so an init store written relative to the entry SP and the table
  // base taken from the post-update SP must be canonicalised to the same entry
  // frame register or their offsets would not line up.  The frame pointer is
  // stable (set once), so only SP is followed, and only through plain frame
  // arithmetic — a non-followable SP def (`and sp,sp,#-16` realignment,
  // `sub sp,sp,xN` VLA) falls back to treating that SP as the canonical base,
  // matching the non-mutating model (no behaviour change for those shapes).
  auto canonFrameSlot = [&](const std::vector<LowOp> &O, NdVar Start,
                            int StartFrom, uint64_t &OutReg, int64_t &OutOff,
                            bool FollowSubpiece = false) -> bool {
    int64_t Off = 0;
    NdVar Cur = Start;
    int From = StartFrom;
    bool HaveFallback = false;
    uint64_t FbReg = 0;
    int64_t FbOff = 0;
    for (int Hop = 0; Hop < limits::kMaxQuasiCopyDepth; ++Hop) {
      if (!chargeEvidence())
        return false;
      if (Cur.isReg() && TRI.isFrameReg(Cur.Offset)) {
        if (!TRI.isStackPointer(
                Cur.Offset)) { // frame pointer: stable, canonical
          OutReg = Cur.Offset;
          OutOff = Off;
          return true;
        }
        HaveFallback = true; // SP: record, then try to follow a mutation
        FbReg = Cur.Offset;
        FbOff = Off;
      }
      int D = (Cur.isReg() || Cur.isTemp())
                  ? budgetedReachingDefIdx(O, From, Cur)
                  : -1;
      if (D < 0) {
        if (HaveFallback) {
          OutReg = FbReg;
          OutOff = FbOff;
          return true;
        }
        return false;
      }
      const LowOp &Op = O[D];
      if (Op.Opcode == NdOp::COPY && Op.NumInputs >= 1 &&
          (Op.Inputs[0].isReg() || Op.Inputs[0].isTemp())) {
        Cur = Op.Inputs[0];
        From = D - 1;
        continue;
      }
      // Memory operands in a 32-bit guest are zero-extended to the host VA
      // container after guest-width effective-address arithmetic.  This is an
      // address view, not a new frame epoch: follow only an exact pointer-width
      // input widened to the current address value.  Requiring the guest
      // pointer width prevents an unrelated narrow integer extension from
      // being mistaken for a stack address.
      if (isGuestAddressZExt(Op)) {
        Cur = Op.Inputs[0];
        From = D - 1;
        continue;
      }
      // Low-half extraction of a wider temp: on 32-bit targets the lifter
      // models a `lea`/address computation in a 64-bit temp and narrows it to
      // the 32-bit register with `SUBBYTES(x, 0)`.  The low half holds the
      // (32-bit) address, so follow it like a rename to keep tracing the frame
      // arithmetic behind it (a frame-slot value passed as a memcpy argument
      // reaches canonFrameSlot through this narrow).  Opt-in only: the
      // table-base / init-store-address traces never cross a SUBBYTES in
      // practice, and unconditionally following it there mis-resolves some
      // 32-bit stack jump tables, so this is enabled solely for the memcpy
      // destination-argument recovery that needs it.
      if (FollowSubpiece && Op.Opcode == NdOp::SUBBYTES && Op.NumInputs >= 2 &&
          Op.Inputs[1].isConst() && Op.Inputs[1].Offset == 0 &&
          (Op.Inputs[0].isReg() || Op.Inputs[0].isTemp())) {
        Cur = Op.Inputs[0];
        From = D - 1;
        continue;
      }
      if (Op.Opcode == NdOp::INT_ADD && Op.NumInputs >= 2) {
        int CW = Op.Inputs[1].isConst() ? 1 : (Op.Inputs[0].isConst() ? 0 : -1);
        if (CW >= 0 &&
            budgetedScaledIndexReg(O, D - 1, Op.Inputs[1 - CW]) == InvalidVA) {
          const std::optional<int64_t> Delta =
              stackSignedDelta(Op.Inputs[CW], Op.Output.Size);
          if (!Delta)
            return false;
          const std::optional<int64_t> Next = stackCheckedOffset(Off, *Delta);
          if (!Next)
            return false;
          Off = *Next;
          Cur = Op.Inputs[1 - CW];
          From = D - 1;
          continue;
        }
        // base + scaled-index: ARM/Thumb -O0 folds the scaled table index into
        // the base register itself (`add r0,sp,#k; add r0,r0,idx,lsl#2;
        // ldr [r0]`), so the frame-slot base is reused and the table-base trace
        // reaches this add before the pure base.  The index is not part of the
        // frame slot, so follow the non-index (base) operand and drop it.
        int SI = (budgetedScaledIndexReg(O, D - 1, Op.Inputs[1]) != InvalidVA)
                     ? 1
                 : (budgetedScaledIndexReg(O, D - 1, Op.Inputs[0]) != InvalidVA)
                     ? 0
                     : -1;
        if (SI >= 0 &&
            (Op.Inputs[1 - SI].isReg() || Op.Inputs[1 - SI].isTemp())) {
          Cur = Op.Inputs[1 - SI];
          From = D - 1;
          continue;
        }
        if (HaveFallback) {
          OutReg = FbReg;
          OutOff = FbOff;
          return true;
        }
        return false;
      }
      if (Op.Opcode == NdOp::INT_SUB && Op.NumInputs >= 2 &&
          Op.Inputs[1].isConst()) {
        const std::optional<int64_t> Delta =
            stackSignedDelta(Op.Inputs[1], Op.Output.Size);
        if (!Delta)
          return false;
        const std::optional<int64_t> Next =
            stackCheckedOffset(Off, *Delta, /*Subtract=*/true);
        if (!Next)
          return false;
        Off = *Next;
        Cur = Op.Inputs[0];
        From = D - 1;
        continue;
      }
      // Unfollowable SP def: fall back to the recorded SP as the canonical
      // base.
      if (HaveFallback) {
        OutReg = FbReg;
        OutOff = FbOff;
        return true;
      }
      return false;
    }
    if (HaveFallback) {
      OutReg = FbReg;
      OutOff = FbOff;
      return true;
    }
    return false;
  };

  auto budgetedFrameSlotKey = [&](const std::vector<LowOp> &TraceOps,
                                  int FromIdx, NdVar Address, uint64_t &OutReg,
                                  int64_t &OutOff) {
    OutOff = 0;
    for (int Depth = 0; Depth < limits::kMaxQuasiCopyDepth; ++Depth) {
      if (!chargeEvidence())
        return false;
      if (Address.isReg()) {
        if (!TRI.isFrameReg(Address.Offset))
          return false;
        OutReg = Address.Offset;
        return true;
      }
      if (!Address.isTemp())
        return false;
      const int Def = budgetedReachingDefIdx(TraceOps, FromIdx, Address);
      if (Def < 0)
        return false;
      const LowOp &Op = TraceOps[Def];
      if (Op.Opcode == NdOp::COPY && Op.NumInputs >= 1) {
        Address = Op.Inputs[0];
        FromIdx = Def - 1;
        continue;
      }
      if (Op.Opcode == NdOp::INT_ADD && Op.NumInputs >= 2) {
        const int Constant =
            Op.Inputs[1].isConst() ? 1 : (Op.Inputs[0].isConst() ? 0 : -1);
        if (Constant < 0 ||
            budgetedScaledIndexReg(TraceOps, Def - 1,
                                   Op.Inputs[1 - Constant]) != InvalidVA)
          return false;
        const auto Delta =
            stackSignedDelta(Op.Inputs[Constant], Op.Output.Size);
        if (!Delta)
          return false;
        const auto Next = stackCheckedOffset(OutOff, *Delta);
        if (!Next)
          return false;
        OutOff = *Next;
        Address = Op.Inputs[1 - Constant];
        FromIdx = Def - 1;
        continue;
      }
      if (Op.Opcode == NdOp::INT_SUB && Op.NumInputs >= 2 &&
          Op.Inputs[1].isConst()) {
        const auto Delta = stackSignedDelta(Op.Inputs[1], Op.Output.Size);
        if (!Delta)
          return false;
        const auto Next = stackCheckedOffset(OutOff, *Delta, /*Subtract=*/true);
        if (!Next)
          return false;
        OutOff = *Next;
        Address = Op.Inputs[0];
        FromIdx = Def - 1;
        continue;
      }
      return false;
    }
    return false;
  };

  auto budgetedFoldRegConstant = [&](uint64_t Reg, va_t Cutoff) {
    // foldRegConstant can retry several prefixes and recursively scan reaching
    // definitions.  Charge each traversal through the shared resolver-stage
    // balance instead of estimating one prefix up front.
    return foldRegConstant(Img, Rec, Reg, Cutoff, [&](size_t Amount) {
      return chargeEvidence(Amount);
    });
  };

  // 1) The table-base register must resolve to a frame slot.  Trace it over the
  //    function prefix (FuncOps) anchored at the table LOAD's position: the
  //    base register is a reused scratch register, so its reaching def must be
  //    the one live *at the load* (not the latest in the function), and FuncOps
  //    — unlike the passed dispatch/path ops — includes the entry-block
  //    prologue `sub sp,sp,#N`, so the stack-pointer adjustment is followed
  //    consistently with the init-store scan (step 3, which always uses
  //    FuncOps).  Locate the load in FuncOps by its instruction address +
  //    output nd-var.
  int BaseFrom = LoadPosInFunc - 1;
  uint64_t FrameReg = InvalidVA;
  int64_t FrameOff = 0;
  if (!canonFrameSlot(FuncOps,
                      NdVar::reg(BaseReg, CurrentImg->getPointerSize()),
                      BaseFrom, FrameReg, FrameOff)) {
    return InvalidVA;
  }
  // x86-64/i386 -O0 ride the table's frame offset in the load displacement
  // (`mov (%rbp,%idx,8),-0x30`), not in the base register (AArch64
  // `add xB,sp,#k`); fold it into the resolved frame slot so the init-store
  // scan below matches the slot the initializer run was copied into.
  const std::optional<int64_t> TableOffset =
      stackCheckedOffset(FrameOff, TableDisp);
  if (!TableOffset)
    return InvalidVA;
  FrameOff = *TableOffset;

  // Trace a stored value back to the constant (read-only) VA it was ultimately
  // loaded from, following value-preserving COPY chains to the defining LOAD.
  // Two materialisation shapes reach the same __const initializer run:
  //   * Direct: `ldr q0,[__const]; str q0,[slot]` — the LOAD address folds to a
  //     constant data VA (foldRegConstant).
  //   * Staging buffer (clang -O0 for >=5-entry local tables): the initializer
  //     run is first copied to a scratch frame area, the real table is
  //     `memset`-cleared, then the scratch is reloaded and re-stored
  //     (`ldr x,[sp+k]; ... stur x,[fp-j]`).  Here the LOAD reads a *frame
  //     slot*, so recurse through the store that filled that scratch slot to
  //     reach the
  //     __const LOAD.
  // Returns the const VA corresponding to the *start* of `Val` (callers add any
  // intra-store entry offset).  The code-pointer reloc-run gate at the call
  // sites keeps this from misreading a non-table register-indirect branch.
  struct ExactStaticAddressMetadata {
    ConstantAddressProvenance Provenance = ConstantAddressProvenance::Unknown;
    va_t OwnerVA = InvalidVA;
    JumpTableValueOccurrence Producer;
    va_t FieldVA = InvalidVA;
    va_t TargetVA = InvalidVA;
  };
  auto hasUsableAddressAuthority =
      [&](const RelocatedInstructionAddressOccurrence &Occurrence) {
    if (Occurrence.Authority ==
        RelocatedInstructionAddressProofKind::LoaderField)
      return Occurrence.FieldVA != InvalidVA;
    if (Occurrence.Authority != RelocatedInstructionAddressProofKind::
                                    AArch64RelocationFreeDataDereference)
      return false;
    return Img.Arch == Arch::AArch64 && !Img.IsRelocatable &&
           Occurrence.FieldVA == InvalidVA && Occurrence.DefinesOutput &&
           !Occurrence.OutputMayDepend &&
           Occurrence.Provenance == ConstantAddressProvenance::DataAddress &&
           Occurrence.Width == Img.getPointerSize() &&
           Occurrence.SeedInstructionAddr != InvalidVA &&
           Occurrence.SeedOpSeq >= 0 &&
           Occurrence.SeedOpcode == NdOp::COPY &&
           Occurrence.SeedInputWitness.isConst() &&
           Occurrence.SeedInputWitness.Provenance ==
               ConstantAddressProvenance::AddressFragment &&
           Occurrence.SeedInputWitness.Size == Img.getPointerSize() &&
           Occurrence.SeedOutputWitness.isReg() &&
           Occurrence.SeedOutputWitness.Size == Img.getPointerSize() &&
           !Occurrence.ArithmeticProof.empty() &&
           Occurrence.ArithmeticProof.back().InstructionAddr ==
               Occurrence.InstructionAddr &&
           Occurrence.ArithmeticProof.back().OpSeq == Occurrence.OpSeq &&
           Occurrence.ArithmeticProof.back().Opcode ==
               Occurrence.OutputOpcode &&
           Occurrence.ArithmeticProof.back().OutputWitness ==
               Occurrence.OutputWitness &&
           Occurrence.DereferenceInstructionAddr != InvalidVA &&
           Occurrence.DereferenceOpSeq >= 0 &&
           (Occurrence.DereferenceOpcode == NdOp::LOAD ||
            Occurrence.DereferenceOpcode == NdOp::STORE) &&
           Occurrence.DereferenceAccessSize != 0;
  };
  auto exactStaticAddressMetadata =
      [&](const LowOp &Load, va_t StaticAddress,
          uint64_t AccessSize) -> std::optional<ExactStaticAddressMetadata> {
    const RelocatedInstructionAddressOccurrence *Exact = nullptr;
    for (const RelocatedInstructionAddressOccurrence &Occurrence :
         RelocatedInstructionAddressOccurrences) {
      if (!chargeEvidence())
        return std::nullopt;
      if (Occurrence.InstructionAddr != Load.Addr ||
          !hasUsableAddressAuthority(Occurrence) ||
          Occurrence.Provenance != ConstantAddressProvenance::DataAddress ||
          Occurrence.OutputMayDepend || Occurrence.TargetOwnerVA == InvalidVA ||
          !exactImmutableDataSpanOwner(Img, StaticAddress, AccessSize,
                                       Occurrence.TargetOwnerVA))
        continue;
      const auto Insn = Insns.find(Occurrence.InstructionAddr);
      if (Insn == Insns.end())
        continue;
      const LowOp *Producer = nullptr;
      for (const LowOp &Op : Insn->second.Ops) {
        if (!chargeEvidence())
          return std::nullopt;
        if (Op.Addr == Occurrence.InstructionAddr &&
            Op.Seq == Occurrence.OpSeq) {
          if (Producer) {
            Producer = nullptr;
            break;
          }
          Producer = &Op;
        }
      }
      if (!Producer)
        continue;
      bool MatchesOccurrence = false;
      if (Occurrence.DefinesOutput) {
        MatchesOccurrence = Producer->Opcode == Occurrence.OutputOpcode &&
                            Producer->Output == Occurrence.OutputWitness;
      } else if (Occurrence.InputIndex >= 0 &&
                 Occurrence.InputIndex < Producer->NumInputs) {
        const NdVar &Input = Producer->Inputs[Occurrence.InputIndex];
        MatchesOccurrence = Input.isConst() &&
                            Input.Offset == Occurrence.TargetVA &&
                            Input.Provenance == Occurrence.Provenance &&
                            Input.AddressOwnerVA == Occurrence.TargetOwnerVA;
      }
      if (!MatchesOccurrence)
        continue;
      if (Exact)
        return std::nullopt;
      Exact = &Occurrence;
    }
    if (!Exact)
      return std::nullopt;
    const auto Insn = Insns.find(Exact->InstructionAddr);
    if (Insn == Insns.end())
      return std::nullopt;
    for (const LowOp &Op : Insn->second.Ops) {
      if (!chargeEvidence())
        return std::nullopt;
      if (Op.Addr != Exact->InstructionAddr || Op.Seq != Exact->OpSeq)
        continue;
      JumpTableValueOccurrence Producer;
      if (Exact->DefinesOutput) {
        Producer = {Op.Output, Op.Addr, Op.Seq, /*DefinedAtPoint=*/true};
      } else if (Exact->InputIndex >= 0 && Exact->InputIndex < Op.NumInputs) {
        Producer = {Op.Inputs[Exact->InputIndex], Op.Addr, Op.Seq,
                    /*DefinedAtPoint=*/false};
      } else {
        return std::nullopt;
      }
      return ExactStaticAddressMetadata{Exact->Provenance, Exact->TargetOwnerVA,
                                        Producer, Exact->FieldVA,
                                        Exact->TargetVA};
    }
    return std::nullopt;
  };

  auto exactStaticAddressMetadataAtUse =
      [&](NdVar Value, int From,
          va_t StaticAddress,
          uint64_t AccessSize) -> std::optional<ExactStaticAddressMetadata> {
    const RelocatedInstructionAddressOccurrence *Exact = nullptr;
    JumpTableValueOccurrence ExactProducer;
    for (int Hop = 0; Hop < limits::kMaxQuasiCopyDepth; ++Hop) {
      if (!chargeEvidence())
        return std::nullopt;
      if (!Value.isReg() && !Value.isTemp())
        break;
      const int D = budgetedReachingDefIdx(FuncOps, From, Value);
      if (D < 0)
        break;
      const LowOp &Op = FuncOps[D];
      for (const RelocatedInstructionAddressOccurrence &Occurrence :
           RelocatedInstructionAddressOccurrences) {
        if (!chargeEvidence())
          return std::nullopt;
        if (Occurrence.InstructionAddr != Op.Addr ||
            Occurrence.OpSeq != Op.Seq ||
            !hasUsableAddressAuthority(Occurrence) ||
            Occurrence.Provenance != ConstantAddressProvenance::DataAddress ||
            Occurrence.OutputMayDepend ||
            Occurrence.TargetOwnerVA == InvalidVA ||
            !Img.relocatedTargetBelongsToOwner(Occurrence.TargetVA,
                                               Occurrence.TargetOwnerVA) ||
            !exactImmutableDataSpanOwner(Img, StaticAddress, AccessSize,
                                         Occurrence.TargetOwnerVA))
          continue;
        JumpTableValueOccurrence Producer;
        if (Occurrence.DefinesOutput) {
          if (Op.Opcode != Occurrence.OutputOpcode ||
              Op.Output != Occurrence.OutputWitness)
            continue;
          Producer = {Op.Output, Op.Addr, Op.Seq, /*DefinedAtPoint=*/true};
        } else {
          if (Occurrence.InputIndex < 0 ||
              Occurrence.InputIndex >= Op.NumInputs)
            continue;
          const NdVar &Input = Op.Inputs[Occurrence.InputIndex];
          if (!Input.isConst() || Input.Offset != Occurrence.TargetVA ||
              Input.Provenance != Occurrence.Provenance ||
              Input.AddressOwnerVA != Occurrence.TargetOwnerVA)
            continue;
          Producer = {Input, Op.Addr, Op.Seq, /*DefinedAtPoint=*/false};
        }
        if (Exact)
          return std::nullopt;
        Exact = &Occurrence;
        ExactProducer = Producer;
      }
      if (Exact)
        break;
      if ((Op.Opcode == NdOp::COPY || isGuestAddressZExt(Op) ||
           (Op.Opcode == NdOp::SUBBYTES && Op.NumInputs >= 2 &&
            Op.Inputs[1].isConst() && Op.Inputs[1].Offset == 0)) &&
          Op.NumInputs >= 1 &&
          (Op.Inputs[0].isReg() || Op.Inputs[0].isTemp())) {
        Value = Op.Inputs[0];
        From = D - 1;
        continue;
      }
      if ((Op.Opcode == NdOp::INT_ADD || Op.Opcode == NdOp::INT_SUB) &&
          Op.NumInputs >= 2) {
        int DynamicSide = -1;
        if ((Op.Inputs[0].isReg() || Op.Inputs[0].isTemp()) &&
            Op.Inputs[1].isConst() &&
            Op.Inputs[1].Provenance == ConstantAddressProvenance::Scalar)
          DynamicSide = 0;
        if (Op.Opcode == NdOp::INT_ADD &&
            (Op.Inputs[1].isReg() || Op.Inputs[1].isTemp()) &&
            Op.Inputs[0].isConst() &&
            Op.Inputs[0].Provenance == ConstantAddressProvenance::Scalar) {
          if (DynamicSide >= 0)
            break;
          DynamicSide = 1;
        }
        if (DynamicSide >= 0) {
          Value = Op.Inputs[DynamicSide];
          From = D - 1;
          continue;
        }
      }
      break;
    }
    if (!Exact)
      return std::nullopt;
    return ExactStaticAddressMetadata{Exact->Provenance, Exact->TargetOwnerVA,
                                      ExactProducer, Exact->FieldVA,
                                      Exact->TargetVA};
  };

  std::function<std::optional<va_t>(
      NdVar, int, int, std::vector<JumpTableValueOccurrence> *,
      std::vector<JumpTableFrameInitializerChunk::StaticSourcePiece> *)>
      traceValToConstSrc =
          [&](NdVar Val, int VFrom, int Depth,
              std::vector<JumpTableValueOccurrence> *Consumers,
              std::vector<JumpTableFrameInitializerChunk::StaticSourcePiece>
                  *SourceValues) -> std::optional<va_t> {
    if (!chargeEvidence())
      return std::nullopt;
    if (Depth > 4)
      return std::nullopt;
    int LdD = -1;
    {
      NdVar V = Val;
      int From = VFrom;
      for (int Hop = 0; Hop < limits::kMaxQuasiCopyDepth; ++Hop) {
        if (!chargeEvidence())
          return std::nullopt;
        if (!V.isReg() && !V.isTemp())
          break;
        int D = budgetedReachingDefIdx(FuncOps, From, V);
        if (D < 0)
          break;
        const LowOp &O = FuncOps[D];
        if (O.Opcode == NdOp::LOAD) {
          LdD = D;
          break;
        }
        // NEON vector-lane materialisation (clang -O2 for >=5-entry local
        // tables): the table run is assembled into a 128-bit register lane by
        // lane (`fmov d0,x; mov.d v0[1],x; stp q0,q1,[slot]`) rather than
        // copied with a clean SIMD load/store.  CONCAT(hi, lo) is the lane
        // assembly; recurse into both halves and require their const sources to
        // be contiguous (hi == lo + lo.size) — i.e. the vector faithfully
        // mirrors a const sub-run — then return the run start (lo).  A
        // shuffled/permuted assembly is not contiguous and is rejected (the
        // caller traps).
        if (O.Opcode == NdOp::CONCAT && O.NumInputs >= 2) {
          std::vector<JumpTableValueOccurrence> LoConsumers;
          std::vector<JumpTableValueOccurrence> HiConsumers;
          std::vector<JumpTableFrameInitializerChunk::StaticSourcePiece>
              LoSources;
          std::vector<JumpTableFrameInitializerChunk::StaticSourcePiece>
              HiSources;
          auto Lo = traceValToConstSrc(O.Inputs[1], D - 1, Depth + 1,
                                       &LoConsumers, &LoSources);
          auto Hi = traceValToConstSrc(O.Inputs[0], D - 1, Depth + 1,
                                       &HiConsumers, &HiSources);
          const std::optional<va_t> ExpectedHi =
              Lo ? checkedVAOffset(*Lo, static_cast<int64_t>(O.Inputs[1].Size))
                 : std::nullopt;
          if (Lo && Hi && ExpectedHi && *Hi == *ExpectedHi) {
            if (Consumers) {
              if (!chargeEvidence(LoConsumers.size()) ||
                  !chargeEvidence(HiConsumers.size()))
                return std::nullopt;
              Consumers->insert(Consumers->end(), LoConsumers.begin(),
                                LoConsumers.end());
              Consumers->insert(Consumers->end(), HiConsumers.begin(),
                                HiConsumers.end());
            }
            if (SourceValues) {
              if (!chargeEvidence(LoSources.size()) ||
                  !chargeEvidence(HiSources.size()))
                return std::nullopt;
              SourceValues->insert(SourceValues->end(), LoSources.begin(),
                                   LoSources.end());
              SourceValues->insert(SourceValues->end(), HiSources.begin(),
                                   HiSources.end());
            }
            return Lo;
          }
          return std::nullopt;
        }
        if (O.Opcode == NdOp::COPY && O.NumInputs >= 1 &&
            (O.Inputs[0].isReg() || O.Inputs[0].isTemp())) {
          V = O.Inputs[0];
          From = D - 1;
          continue;
        }
        // A lane scalar is widened to the q-register (INT_ZEXT d->q) and its
        // low element re-extracted (SUBBYTES v,0) during the vector assembly
        // above; both preserve the low bytes' const source, so follow them to
        // the defining LOAD.
        if ((O.Opcode == NdOp::INT_ZEXT ||
             (O.Opcode == NdOp::SUBBYTES && O.NumInputs >= 2 &&
              O.Inputs[1].isConst() && O.Inputs[1].Offset == 0)) &&
            O.NumInputs >= 1 && (O.Inputs[0].isReg() || O.Inputs[0].isTemp())) {
          V = O.Inputs[0];
          From = D - 1;
          continue;
        }
        break;
      }
    }
    if (LdD < 0)
      return std::nullopt;
    const LowOp &Ld = FuncOps[LdD];
    NdVar AddrV = (Ld.NumInputs >= 2) ? Ld.Inputs[1] : Ld.Inputs[0];

    // Staging buffer: the value came from a frame slot an earlier store filled.
    // Resolve that slot and recurse through the latest covering store's value.
    uint64_t SlotReg = InvalidVA;
    int64_t SlotOff = 0;
    if (budgetedFrameSlotKey(FuncOps, LdD - 1, AddrV, SlotReg, SlotOff)) {
      int64_t LdW = static_cast<int64_t>(Ld.Output.Size);
      for (int I = LdD - 1; I >= 0; --I) {
        if (!chargeEvidence())
          return std::nullopt;
        const LowOp &St = FuncOps[I];
        if (St.Opcode != NdOp::STORE || St.NumInputs < 2)
          continue;
        uint64_t SB = InvalidVA;
        int64_t SOff = 0;
        if (!budgetedFrameSlotKey(FuncOps, I - 1, St.Inputs[0], SB, SOff))
          continue;
        if (SB != SlotReg)
          continue;
        int64_t SS = static_cast<int64_t>(St.Inputs[1].Size);
        const std::optional<int64_t> LoadEnd = stackCheckedOffset(SlotOff, LdW);
        const std::optional<int64_t> StoreEnd = stackCheckedOffset(SOff, SS);
        if (SS <= 0 || !LoadEnd || !StoreEnd || SOff > SlotOff ||
            *LoadEnd > *StoreEnd)
          continue; // store does not cover the loaded slice
        std::vector<JumpTableValueOccurrence> NestedConsumers;
        std::vector<JumpTableFrameInitializerChunk::StaticSourcePiece>
            NestedSources;
        auto Src = traceValToConstSrc(St.Inputs[1], I - 1, Depth + 1,
                                      &NestedConsumers, &NestedSources);
        if (!Src)
          return std::nullopt;
        if (Consumers) {
          if (!chargeEvidence(NestedConsumers.size()))
            return std::nullopt;
          Consumers->insert(Consumers->end(), NestedConsumers.begin(),
                            NestedConsumers.end());
        }
        if (SourceValues) {
          if (!chargeEvidence(NestedSources.size()))
            return std::nullopt;
          SourceValues->insert(SourceValues->end(), NestedSources.begin(),
                               NestedSources.end());
        }
        const std::optional<int64_t> SliceDelta =
            stackCheckedOffset(SlotOff, SOff, /*Subtract=*/true);
        if (!SliceDelta)
          return std::nullopt;
        return checkedVAOffset(*Src, *SliceDelta);
      }
      return std::nullopt;
    }

    // Constant data source: decompose the LOAD address into base reg + const
    // displacement, then fold the base register at the load to a const data VA.
    uint64_t AddrReg = InvalidVA;
    int64_t AddrDisp = 0;
    bool ConstBase = false;
    uint64_t ConstBaseVA = 0;
    std::optional<va_t> ExactRelocatedBase;
    {
      NdVar A = AddrV;
      int AFrom = LdD - 1;
      for (int Hop = 0; Hop < limits::kMaxQuasiCopyDepth; ++Hop) {
        if (!chargeEvidence())
          return std::nullopt;
        if (A.isReg()) {
          AddrReg = A.Offset;
          break;
        }
        // x86-64/i386 RIP/PC-relative table-entry load: the lifter folds the
        // PC-relative address to a constant base (`COPY tmp,<RIP>; INT_ADD
        // tmp,disp`), so the chain terminates at a constant rather than a base
        // register.  The absolute __const VA is that constant plus the
        // accumulated displacement (AArch64 keeps an adrp/add register base, so
        // this branch is x86-only).
        if (A.isConst()) {
          ConstBase = true;
          ConstBaseVA = A.Offset;
          break;
        }
        if (!A.isTemp())
          break;
        int D = budgetedReachingDefIdx(FuncOps, AFrom, A);
        if (D < 0)
          break;
        const LowOp &O = FuncOps[D];
        if (O.Opcode == NdOp::COPY && O.NumInputs >= 1) {
          A = O.Inputs[0];
          AFrom = D - 1;
          continue;
        }
        if (isGuestAddressZExt(O)) {
          A = O.Inputs[0];
          AFrom = D - 1;
          continue;
        }
        if (O.Opcode == NdOp::INT_ADD && O.NumInputs >= 2) {
          int CW = O.Inputs[1].isConst() ? 1 : (O.Inputs[0].isConst() ? 0 : -1);
          if (CW < 0)
            break;
          const NdVar &Constant = O.Inputs[CW];
          std::optional<va_t> Exact = exactI386DataAddressAt(
              &O, CW, O.Addr, static_cast<uint32_t>(Constant.Offset));
          if (Exact) {
            if (ExactRelocatedBase)
              return std::nullopt;
            ExactRelocatedBase = *Exact;
          } else {
            const std::optional<int64_t> Delta =
                stackSignedDelta(Constant, O.Output.Size);
            if (!Delta)
              return std::nullopt;
            const std::optional<int64_t> Next =
                stackCheckedOffset(AddrDisp, *Delta);
            if (!Next)
              return std::nullopt;
            AddrDisp = *Next;
          }
          A = O.Inputs[1 - CW];
          AFrom = D - 1;
          continue;
        }
        break;
      }
    }
    if (ConstBase) {
      const std::optional<va_t> StaticAddress =
          checkedVAOffset(ConstBaseVA, AddrDisp);
      if (!StaticAddress)
        return std::nullopt;
      const auto StaticMetadata =
          exactStaticAddressMetadataAtUse(AddrV, LdD - 1, *StaticAddress,
                                          Ld.Output.Size);
      if (!StaticMetadata)
        return std::nullopt;
      if (Consumers) {
        if (!chargeEvidence())
          return std::nullopt;
        Consumers->push_back(
            {AddrV, Ld.Addr, Ld.Seq, /*DefinedAtPoint=*/false});
      }
      if (SourceValues) {
        if (!chargeEvidence())
          return std::nullopt;
        SourceValues->push_back(
            {{Ld.Output, Ld.Addr, Ld.Seq, /*DefinedAtPoint=*/true},
             {AddrV, Ld.Addr, Ld.Seq, /*DefinedAtPoint=*/false},
             StaticMetadata->Producer,
             StaticMetadata->FieldVA,
             StaticMetadata->TargetVA,
             *StaticAddress,
             Ld.Output.Size,
             StaticMetadata->Provenance,
             StaticMetadata->OwnerVA});
      }
      return *StaticAddress;
    }
    if (AddrReg == InvalidVA)
      return std::nullopt;
    if (ExactRelocatedBase) {
      const std::optional<va_t> StaticAddress =
          checkedVAOffset(*ExactRelocatedBase, AddrDisp);
      if (!StaticAddress)
        return std::nullopt;
      const auto StaticMetadata =
          exactStaticAddressMetadataAtUse(AddrV, LdD - 1, *StaticAddress,
                                          Ld.Output.Size);
      if (!StaticMetadata)
        return std::nullopt;
      if (Consumers) {
        if (!chargeEvidence())
          return std::nullopt;
        Consumers->push_back(
            {AddrV, Ld.Addr, Ld.Seq, /*DefinedAtPoint=*/false});
      }
      if (SourceValues) {
        if (!chargeEvidence())
          return std::nullopt;
        SourceValues->push_back(
            {{Ld.Output, Ld.Addr, Ld.Seq, /*DefinedAtPoint=*/true},
             {AddrV, Ld.Addr, Ld.Seq, /*DefinedAtPoint=*/false},
             StaticMetadata->Producer,
             StaticMetadata->FieldVA,
             StaticMetadata->TargetVA,
             *StaticAddress,
             Ld.Output.Size,
             StaticMetadata->Provenance,
             StaticMetadata->OwnerVA});
      }
      return *StaticAddress;
    }
    std::optional<uint64_t> Folded = budgetedFoldRegConstant(AddrReg, Ld.Addr);
    if (Folded && Img.getSegmentFor(*Folded)) {
      const std::optional<va_t> StaticAddress =
          checkedVAOffset(*Folded, AddrDisp);
      if (!StaticAddress)
        return std::nullopt;
      const auto StaticMetadata =
          exactStaticAddressMetadataAtUse(AddrV, LdD - 1, *StaticAddress,
                                          Ld.Output.Size);
      if (!StaticMetadata)
        return std::nullopt;
      if (Consumers) {
        if (!chargeEvidence())
          return std::nullopt;
        Consumers->push_back(
            {AddrV, Ld.Addr, Ld.Seq, /*DefinedAtPoint=*/false});
      }
      if (SourceValues) {
        if (!chargeEvidence())
          return std::nullopt;
        SourceValues->push_back(
            {{Ld.Output, Ld.Addr, Ld.Seq, /*DefinedAtPoint=*/true},
             {AddrV, Ld.Addr, Ld.Seq, /*DefinedAtPoint=*/false},
             StaticMetadata->Producer,
             StaticMetadata->FieldVA,
             StaticMetadata->TargetVA,
             *StaticAddress,
             Ld.Output.Size,
             StaticMetadata->Provenance,
             StaticMetadata->OwnerVA});
      }
      return *StaticAddress;
    }
    // A fold failure is not evidence for the i386 GOT model.  Admit the
    // provisional source only when this exact LOAD instruction contains the
    // matching loader-authenticated GOTOFF address operand.
    const va_t I386Candidate = static_cast<uint32_t>(AddrDisp);
    if (std::optional<va_t> Exact =
            exactI386DataAddressAt(nullptr, -1, Ld.Addr, I386Candidate)) {
      const auto StaticMetadata =
          exactStaticAddressMetadataAtUse(AddrV, LdD - 1, *Exact,
                                          Ld.Output.Size);
      if (!StaticMetadata)
        return std::nullopt;
      if (Consumers) {
        if (!chargeEvidence())
          return std::nullopt;
        Consumers->push_back(
            {AddrV, Ld.Addr, Ld.Seq, /*DefinedAtPoint=*/false});
      }
      if (SourceValues) {
        if (!chargeEvidence())
          return std::nullopt;
        SourceValues->push_back(
            {{Ld.Output, Ld.Addr, Ld.Seq, /*DefinedAtPoint=*/true},
             {AddrV, Ld.Addr, Ld.Seq, /*DefinedAtPoint=*/false},
             StaticMetadata->Producer,
             StaticMetadata->FieldVA,
             StaticMetadata->TargetVA,
             *Exact,
             Ld.Output.Size,
             StaticMetadata->Provenance,
             StaticMetadata->OwnerVA});
      }
      return *Exact;
    }
    return std::nullopt;
  };

  // 3) Find the STORE that initialised the table's frame slot from a constant
  //    data source.  clang copies the read-only initializer run (one scalar or
  //    SIMD store per 1-2 entries), so a store whose byte span covers the table
  //    base offset and whose value traces (via traceValToConstSrc, including
  //    through a staging buffer) to a __const LOAD pins the source.  Prefer the
  //    latest such store before dispatch.
  va_t BestSource = InvalidVA;
  bool BestMutated = false;
  std::vector<JumpTableFrameInitializerChunk> BestInitializers;
  std::vector<JumpTableValueOccurrence> BestStorageConsumers;
  for (int I = 0; I < LoadPosInFunc; ++I) {
    if (!chargeEvidence())
      return InvalidVA;
    const LowOp &Op = FuncOps[I];
    if (Op.Opcode != NdOp::STORE || Op.NumInputs < 2)
      continue;
    uint64_t B = InvalidVA;
    int64_t Off = 0;
    const bool CanonStore =
        canonFrameSlot(FuncOps, Op.Inputs[0], I - 1, B, Off);
    if (!CanonStore)
      continue;
    if (B != FrameReg)
      continue;
    int64_t StoreSize = static_cast<int64_t>(Op.Inputs[1].Size);
    const std::optional<int64_t> StoreEnd = stackCheckedOffset(Off, StoreSize);
    if (StoreSize <= 0 || !StoreEnd || Off > FrameOff || FrameOff >= *StoreEnd)
      continue;
    const std::optional<int64_t> EntryDeltaOpt =
        stackCheckedOffset(FrameOff, Off, /*Subtract=*/true);
    if (!EntryDeltaOpt || *EntryDeltaOpt < 0)
      continue;
    const int64_t EntryDelta = *EntryDeltaOpt;

    // Trace the stored value (directly or through a staging buffer) to the
    // __const VA it was loaded from; the table base sits EntryDelta into it.
    std::vector<JumpTableValueOccurrence> CandidateConsumers;
    std::vector<JumpTableFrameInitializerChunk::StaticSourcePiece>
        CandidateSourceValues;
    auto SrcOpt = traceValToConstSrc(
        Op.Inputs[1], I - 1, 0, &CandidateConsumers, &CandidateSourceValues);
    if (!SrcOpt)
      continue;
    const std::optional<va_t> SourceOpt = checkedVAOffset(*SrcOpt, EntryDelta);
    if (!SourceOpt)
      continue;
    const va_t Source = *SourceOpt;

    // The source must be a constant (read-only) data region carrying a run of
    // absolute code-pointer relocations — the verifiable signature of a label
    // table (the initializer run lives in __DATA_CONST / .data.rel.ro, which
    // the loader may flag writable, so the reloc run, not the segment
    // permission, is the gate).  This is what distinguishes a
    // stack-materialised computed-goto table from any other stack array, so a
    // non-table register-indirect branch is never misread.
    const auto *Seg = Img.getSegmentFor(Source);
    if (!Seg || Seg->Data.empty())
      continue;
    const uint32_t RelocRun = budgetedCodePtrRelocRun(Source);
    if (RelocRun < limits::kMinJumpTableEntries)
      continue;
    if (!chargeEvidence(1) || !chargeEvidence(CandidateSourceValues.size()) ||
        !chargeEvidence(CandidateConsumers.size()))
      return InvalidVA;
    BestSource = Source; // keep scanning; latest valid init wins
    JumpTableFrameInitializerChunk Initializer;
    Initializer.Writer = {Op.Inputs[0], Op.Addr, Op.Seq,
                          /*DefinedAtPoint=*/false};
    Initializer.Destination = {
        {Op.Inputs[0], Op.Addr, Op.Seq, /*DefinedAtPoint=*/false}, EntryDelta};
    Initializer.StoredValue = {Op.Inputs[1], Op.Addr, Op.Seq,
                               /*DefinedAtPoint=*/false};
    Initializer.ByteCount = static_cast<uint64_t>(StoreSize);
    Initializer.StaticSourceAddress = *SrcOpt;
    Initializer.StaticSources = std::move(CandidateSourceValues);
    BestInitializers = {std::move(Initializer)};
    BestStorageConsumers = std::move(CandidateConsumers);
  }

  // Larger local tables (clang -O0, ~>=8 entries) are not copied store-by-store
  // but with a single `memcpy(table_slot, __const_run, size)` call, so the
  // store scan above finds nothing.  Recognise that init: a memcpy/memmove
  // whose destination (first integer argument) is the table's frame slot and
  // whose source (second argument) folds to a __const VA carrying a
  // code-pointer reloc run.  The reloc-run gate is the table signature, so a
  // non-table memcpy into a stack buffer is never misread.
  if (BestSource == InvalidVA) {
    for (int I = 0; I < LoadPosInFunc; ++I) {
      if (!chargeEvidence())
        return InvalidVA;
      const LowOp &Op = FuncOps[I];
      if (Op.Opcode != NdOp::CALL || Op.NumInputs < 1 ||
          !Op.Inputs[0].isConst())
        continue;
      va_t Target = static_cast<va_t>(Op.Inputs[0].Offset);
      bool IsCopy = false;
      if (const Import *Imp = Img.findImportAt(Target))
        IsCopy = libc::isMemCopyName(Imp->Name);
      if (!IsCopy)
        if (const Symbol *Sym = Img.findSymbolAt(Target))
          IsCopy = libc::isMemCopyName(Sym->Name);
      // Relocatable objects (the lift roundtrip input) leave the call's
      // constant target at the placeholder 0 — the real callee is named by a
      // relocation at the call instruction (x86 `call rel32` reloc at +1,
      // AArch64/ARM `bl` reloc at +0).  When the resolved constant misses,
      // consult the call-site relocation's symbol name so a memcpy/memmove
      // table-init copy is still recognised.  The code-pointer reloc-run gate
      // below remains the table signature, so a non-table memcpy is never
      // misread as a jump table.
      if (!IsCopy)
        for (const RelocationEntry &R : Img.Relocations) {
          if (!chargeEvidence())
            return InvalidVA;
          if ((R.Address == Op.Addr || R.Address == Op.Addr + 1) &&
              !R.SymbolName.empty() && libc::isMemCopyName(R.SymbolName)) {
            IsCopy = true;
            break;
          }
        }
      if (!IsCopy)
        continue;
      // arg0 (dst) must resolve to the table's frame slot, covering the base.
      // The memcpy destination is a register holding a computed frame address
      // (`add x0,sp,#k`); canonFrameSlot traces it the same way as the table
      // base so both share one stack-pointer-aware canonicalisation.
      uint64_t DB = InvalidVA;
      int64_t DOff = 0;
      // arg0 (dst, the table frame slot) and arg1 (src, the const initializer
      // VA). Register-based ABIs (x86-64 SysV/Win64, AArch64, ARM) pass them in
      // the image ABI's integer parameter registers; i386 cdecl passes them on
      // the outgoing stack
      // ([sp+0]=dst, [sp+ptr]=src), recovered from the stores that fill those
      // slots just before the call.
      // Resolve a value that holds a constant data address (the memcpy source =
      // address-of the initializer run) to its absolute VA: strip COPY /
      // low-half SUBBYTES renames, sum checked scalar addends, then fold the
      // base register.  i386 GOTOFF is admitted only through the exact loader
      // occurrence for the producing instruction, never from a numeric
      // displacement alone.
      auto addrToConstVA = [&](NdVar A, int AFrom) -> std::optional<uint64_t> {
        int64_t Disp = 0;
        uint64_t Reg = InvalidVA;
        bool ConstBase = false;
        uint64_t ConstVA = 0;
        std::optional<va_t> ExactRelocatedBase;
        for (int Hop = 0; Hop < limits::kMaxQuasiCopyDepth; ++Hop) {
          if (!chargeEvidence())
            return std::nullopt;
          if (A.isConst()) {
            ConstBase = true;
            ConstVA = A.Offset;
            break;
          }
          if (!A.isReg() && !A.isTemp())
            break;
          int D = budgetedReachingDefIdx(FuncOps, AFrom, A);
          if (D >= 0) {
            const LowOp &O = FuncOps[D];
            // COPY / low-half SUBBYTES are pure renames — follow for both regs
            // and temps (the i386 `lea` result reaches the stored arg through a
            // 64-bit temp narrowed to the 32-bit register).
            if ((O.Opcode == NdOp::COPY || isGuestAddressZExt(O) ||
                 (O.Opcode == NdOp::SUBBYTES && O.NumInputs >= 2 &&
                  O.Inputs[1].isConst() && O.Inputs[1].Offset == 0)) &&
                O.NumInputs >= 1 &&
                (O.Inputs[0].isReg() || O.Inputs[0].isTemp() ||
                 O.Inputs[0].isConst())) {
              A = O.Inputs[0];
              AFrom = D - 1;
              continue;
            }
            // Sum a constant addend only while the running base is a TEMP: a
            // register base is the terminal (e.g. the i386 GOT base register,
            // whose get_pc_thunk arithmetic must not be folded into the GOTOFF
            // displacement).
            if (A.isTemp() && O.Opcode == NdOp::INT_ADD && O.NumInputs >= 2) {
              int CW =
                  O.Inputs[1].isConst() ? 1 : (O.Inputs[0].isConst() ? 0 : -1);
              if (CW >= 0 &&
                  (O.Inputs[1 - CW].isReg() || O.Inputs[1 - CW].isTemp())) {
                const NdVar &Constant = O.Inputs[CW];
                std::optional<va_t> Exact = exactI386DataAddressAt(
                    &O, CW, O.Addr, static_cast<uint32_t>(Constant.Offset));
                if (Exact) {
                  if (ExactRelocatedBase)
                    return std::nullopt;
                  ExactRelocatedBase = *Exact;
                } else {
                  const std::optional<int64_t> Delta =
                      stackSignedDelta(Constant, O.Output.Size);
                  if (!Delta)
                    return std::nullopt;
                  const std::optional<int64_t> Next =
                      stackCheckedOffset(Disp, *Delta);
                  if (!Next)
                    return std::nullopt;
                  Disp = *Next;
                }
                A = O.Inputs[1 - CW];
                AFrom = D - 1;
                continue;
              }
            }
          }
          if (A.isReg())
            Reg = A.Offset;
          break;
        }
        if (ConstBase)
          return checkedVAOffset(ConstVA, Disp);
        if (Reg != InvalidVA) {
          if (auto F = budgetedFoldRegConstant(Reg, Op.Addr);
              F && Img.getSegmentFor(*F))
            return checkedVAOffset(*F, Disp);
          if (ExactRelocatedBase)
            return checkedVAOffset(*ExactRelocatedBase, Disp);
        }
        return std::nullopt;
      };

      auto scalarConstantAt =
          [&](NdVar Value, int From,
              JumpTableValueOccurrence *Producer) -> std::optional<uint64_t> {
        for (int Hop = 0; Hop < limits::kMaxQuasiCopyDepth; ++Hop) {
          if (!chargeEvidence())
            return std::nullopt;
          if (Value.isConst()) {
            if ((Value.Provenance != ConstantAddressProvenance::Unknown &&
                 Value.Provenance != ConstantAddressProvenance::Scalar) ||
                Value.Size == 0 || Value.Size > sizeof(uint64_t))
              return std::nullopt;
            const int ConsumerIndex = From + 1;
            if (Producer && ConsumerIndex >= 0 &&
                ConsumerIndex < static_cast<int>(FuncOps.size())) {
              const LowOp &Consumer = FuncOps[ConsumerIndex];
              int ExactInput = -1;
              for (int Input = 0; Input < Consumer.NumInputs; ++Input) {
                if (!chargeEvidence())
                  return std::nullopt;
                if (Consumer.Inputs[Input] == Value) {
                  if (ExactInput >= 0)
                    return std::nullopt;
                  ExactInput = Input;
                }
              }
              if (ExactInput < 0)
                return std::nullopt;
              if (!chargeEvidence(
                      RelocatedInstructionAddressOccurrences.size()))
                return std::nullopt;
              const bool Relocated = std::any_of(
                  RelocatedInstructionAddressOccurrences.begin(),
                  RelocatedInstructionAddressOccurrences.end(),
                  [&](const RelocatedInstructionAddressOccurrence &Occurrence) {
                    return Occurrence.InstructionAddr == Consumer.Addr &&
                           Occurrence.OpSeq == Consumer.Seq &&
                           (Occurrence.DefinesOutput ||
                            Occurrence.InputIndex == ExactInput);
                  });
              if (Relocated)
                return std::nullopt;
              *Producer = {Value, Consumer.Addr, Consumer.Seq,
                           /*DefinedAtPoint=*/false};
            }
            const unsigned Bits = static_cast<unsigned>(Value.Size) * 8;
            const uint64_t Mask = Bits == 64
                                      ? std::numeric_limits<uint64_t>::max()
                                      : (uint64_t{1} << Bits) - 1;
            return Value.Offset & Mask;
          }
          if (!Value.isReg() && !Value.isTemp())
            return std::nullopt;
          int D = budgetedReachingDefIdx(FuncOps, From, Value);
          if (D < 0)
            return std::nullopt;
          const LowOp &Def = FuncOps[D];
          if ((Def.Opcode == NdOp::COPY || Def.Opcode == NdOp::INT_ZEXT) &&
              Def.NumInputs >= 1) {
            if (Def.Opcode == NdOp::COPY && Def.Inputs[0].isConst()) {
              // Decoder immediates predate scalar provenance tagging on some
              // targets.  Admit the exact COPY literal only when no loader
              // address occurrence claims that operand; a raw relocated value
              // must never become a memcpy length certificate.
              if (!chargeEvidence(
                      RelocatedInstructionAddressOccurrences.size()))
                return std::nullopt;
              const bool Relocated = std::any_of(
                  RelocatedInstructionAddressOccurrences.begin(),
                  RelocatedInstructionAddressOccurrences.end(),
                  [&](const RelocatedInstructionAddressOccurrence &Occurrence) {
                    return Occurrence.InstructionAddr == Def.Addr &&
                           Occurrence.OpSeq == Def.Seq &&
                           (Occurrence.DefinesOutput ||
                            Occurrence.InputIndex == 0);
                  });
              if (Relocated ||
                  (Def.Inputs[0].Provenance !=
                       ConstantAddressProvenance::Unknown &&
                   Def.Inputs[0].Provenance !=
                       ConstantAddressProvenance::Scalar) ||
                  Def.Inputs[0].Size == 0 ||
                  Def.Inputs[0].Size > sizeof(uint64_t))
                return std::nullopt;
              if (Producer)
                *Producer = {Def.Output, Def.Addr, Def.Seq,
                             /*DefinedAtPoint=*/true};
              const unsigned Bits =
                  static_cast<unsigned>(Def.Inputs[0].Size) * 8;
              const uint64_t Mask = Bits == 64
                                        ? std::numeric_limits<uint64_t>::max()
                                        : (uint64_t{1} << Bits) - 1;
              return Def.Inputs[0].Offset & Mask;
            }
            Value = Def.Inputs[0];
            From = D - 1;
            continue;
          }
          if (Def.Opcode == NdOp::SUBBYTES && Def.NumInputs >= 2 &&
              Def.Inputs[1].isConst() && Def.Inputs[1].Offset == 0) {
            Value = Def.Inputs[0];
            From = D - 1;
            continue;
          }
          return std::nullopt;
        }
        return std::nullopt;
      };

      std::optional<uint64_t> Folded;
      std::optional<ExactStaticAddressMetadata> SourceMetadata;
      std::optional<uint64_t> CopyLength;
      JumpTableValueOccurrence LengthProducer;
      bool ArgsOk = false;
      JumpTableValueOccurrence DestinationUse;
      JumpTableValueOccurrence SourceUse;
      JumpTableValueOccurrence LengthUse;
      // Register-passed args (x86-64 SysV/Win64, AArch64, ARM): dst in
      // IntParamRegs[0], src in IntParamRegs[1]. Commit only when the source
      // also folds — on i386 the dst register may coincidentally still hold the
      // table address while the args truly live on the stack, so a register dst
      // with no register source must fall through to the stack recovery below.
      if (IntParamRegs.size() >= 3 &&
          canonFrameSlot(FuncOps,
                         NdVar::reg(IntParamRegs[0], Img.getPointerSize()),
                         I - 1, DB, DOff, /*FollowSubpiece=*/true)) {
        NdVar RegisterSource =
            NdVar::reg(IntParamRegs[1], Img.getPointerSize());
        NdVar RegisterLength =
            NdVar::reg(IntParamRegs[2], Img.getPointerSize());
        if (auto F = addrToConstVA(RegisterSource, I - 1);
            F && Img.getSegmentFor(*F)) {
          Folded = *F;
          CopyLength =
              scalarConstantAt(RegisterLength, I - 1, &LengthProducer);
          if (CopyLength)
            SourceMetadata = exactStaticAddressMetadataAtUse(
                RegisterSource, I - 1, *F, *CopyLength);
        }
        if (Folded && CopyLength && SourceMetadata) {
          ArgsOk = true;
          DestinationUse = {NdVar::reg(IntParamRegs[0], Img.getPointerSize()),
                            Op.Addr, Op.Seq, /*DefinedAtPoint=*/false};
          SourceUse = {RegisterSource, Op.Addr, Op.Seq,
                       /*DefinedAtPoint=*/false};
          LengthUse = {RegisterLength, Op.Addr, Op.Seq,
                       /*DefinedAtPoint=*/false};
        }
      }
      // Stack-passed args (i386 cdecl): dst=[sp+0], src=[sp+ptr], recovered
      // from the stores that fill the outgoing argument slots just before the
      // call.
      if (!ArgsOk) {
        NdVar DstVal, SrcVal, LenVal;
        int DstFrom = -1, SrcFrom = -1, LenFrom = -1;
        JumpTableValueOccurrence StackDestinationUse;
        JumpTableValueOccurrence StackSourceUse;
        JumpTableValueOccurrence StackLengthUse;
        bool HaveDst = false, HaveSrc = false, HaveLen = false;
        int64_t PtrSz = static_cast<int64_t>(Img.getPointerSize());
        auto outgoingSlotKey = [&](NdVar Address, int From, uint64_t &Base,
                                   int64_t &Offset) {
          for (int Depth = 0; Depth < limits::kMaxQuasiCopyDepth; ++Depth) {
            if (!chargeEvidence())
              return false;
            if (!Address.isReg() && !Address.isTemp())
              break;
            const int D = budgetedReachingDefIdx(FuncOps, From, Address);
            if (D < 0)
              break;
            const LowOp &Def = FuncOps[D];
            if ((Def.Opcode == NdOp::COPY || isGuestAddressZExt(Def)) &&
                Def.NumInputs >= 1) {
              Address = Def.Inputs[0];
              From = D - 1;
              continue;
            }
            break;
          }
          return budgetedFrameSlotKey(FuncOps, From, Address, Base, Offset);
        };
        for (int J = I - 1; J >= 0 && !(HaveDst && HaveSrc && HaveLen); --J) {
          if (!chargeEvidence())
            return InvalidVA;
          const LowOp &St = FuncOps[J];
          // Outgoing cells belong to this exact call epoch.  Never borrow
          // arguments prepared for an earlier call or cross a stack-pointer
          // redefinition (allocation, pivot, cleanup) while searching.
          if (St.Opcode == NdOp::CALL || St.Opcode == NdOp::INDIR_CALL ||
              St.Opcode == NdOp::RETURN ||
              (St.Output.isReg() && TRI.isStackPointer(St.Output.Offset))) {
            break;
          }
          if (St.Opcode != NdOp::STORE || St.NumInputs < 2)
            continue;
          uint64_t SB = InvalidVA;
          int64_t SOff = 0;
          if (!outgoingSlotKey(St.Inputs[0], J - 1, SB, SOff))
            continue;
          if (!TRI.isStackPointer(SB))
            continue;
          if (SOff == 0 && !HaveDst) {
            DstVal = St.Inputs[1];
            DstFrom = J - 1;
            StackDestinationUse = {St.Inputs[1], St.Addr, St.Seq,
                                   /*DefinedAtPoint=*/false};
            HaveDst = true;
          } else if (SOff == PtrSz && !HaveSrc) {
            SrcVal = St.Inputs[1];
            SrcFrom = J - 1;
            StackSourceUse = {St.Inputs[1], St.Addr, St.Seq,
                              /*DefinedAtPoint=*/false};
            HaveSrc = true;
          } else if (PtrSz <= std::numeric_limits<int64_t>::max() / 2 &&
                     SOff == PtrSz * 2 && !HaveLen) {
            LenVal = St.Inputs[1];
            LenFrom = J - 1;
            StackLengthUse = {St.Inputs[1], St.Addr, St.Seq,
                              /*DefinedAtPoint=*/false};
            HaveLen = true;
          }
        }
        if (HaveDst && HaveSrc && HaveLen &&
            canonFrameSlot(FuncOps, DstVal, DstFrom, DB, DOff,
                           /*FollowSubpiece=*/true)) {
          Folded = addrToConstVA(SrcVal, SrcFrom);
          CopyLength = scalarConstantAt(LenVal, LenFrom, &LengthProducer);
          if (Folded && CopyLength)
            SourceMetadata = exactStaticAddressMetadataAtUse(
                SrcVal, SrcFrom, *Folded, *CopyLength);
          ArgsOk = Folded.has_value() && CopyLength.has_value() &&
                   SourceMetadata.has_value();
          if (ArgsOk) {
            DestinationUse = StackDestinationUse;
            SourceUse = StackSourceUse;
            LengthUse = StackLengthUse;
          }
        }
      }
      if (!ArgsOk)
        continue;
      if (DB != FrameReg)
        continue;
      const std::optional<int64_t> EntryDeltaOpt =
          stackCheckedOffset(FrameOff, DOff, /*Subtract=*/true);
      if (!EntryDeltaOpt || *EntryDeltaOpt < 0)
        continue;
      const int64_t EntryDelta = *EntryDeltaOpt;
      if (!Folded || *Folded == 0 || !CopyLength)
        continue;
      if (static_cast<uint64_t>(EntryDelta) > InvalidVA - *Folded)
        continue;
      va_t Source = *Folded + static_cast<uint64_t>(EntryDelta);
      const auto *Seg = Img.getSegmentFor(Source);
      if (!Seg || Seg->Data.empty())
        continue;
      const uint32_t EntryCount = budgetedCodePtrRelocRun(Source);
      if (EntryCount < limits::kMinJumpTableEntries ||
          LoadWidth > std::numeric_limits<uint64_t>::max() / EntryCount)
        continue;
      const uint64_t TableBytes = uint64_t{LoadWidth} * EntryCount;
      if (static_cast<uint64_t>(EntryDelta) >
              std::numeric_limits<uint64_t>::max() - TableBytes ||
          *CopyLength < static_cast<uint64_t>(EntryDelta) + TableBytes)
        continue;
      if (!chargeEvidence())
        return InvalidVA;
      BestSource = Source; // latest valid copy wins
      JumpTableFrameInitializerChunk Initializer;
      Initializer.Writer = {Op.Inputs[0], Op.Addr, Op.Seq,
                            /*DefinedAtPoint=*/false};
      Initializer.Destination = {DestinationUse, EntryDelta};
      Initializer.SourceAddress = SourceUse;
      Initializer.Length = LengthUse;
      Initializer.LengthProducer = LengthProducer;
      Initializer.ByteCount = *CopyLength;
      Initializer.StaticSourceAddress = *Folded;
      Initializer.StaticSourceProvenance = SourceMetadata->Provenance;
      Initializer.StaticSourceOwnerVA = SourceMetadata->OwnerVA;
      Initializer.StaticSourceProducer = SourceMetadata->Producer;
      Initializer.StaticSourceFieldVA = SourceMetadata->FieldVA;
      Initializer.StaticSourceProducerTargetVA = SourceMetadata->TargetVA;
      Initializer.IsMemcpy = true;
      BestInitializers = {std::move(Initializer)};
    }
  }

  // Soundness gate: an index-dispatch switch over the recovered static targets
  // is only correct if the stack table still holds the *positional* constant
  // entries at run time.  When the program overwrites an entry slot after the
  // initializer with a value that is not the constant entry for that slot — a
  // runtime permutation (`void *t=tab[0]; tab[0]=tab[3]; tab[3]=t;`) — the
  // static map no longer describes the runtime index->target mapping.  Flag it
  // so the emitter traps loudly instead of silently selecting the wrong case
  // (sound resolution would need runtime value dispatch — a documented gap).
  if (BestSource != InvalidVA && MutatedOut) {
    uint32_t N = budgetedCodePtrRelocRun(BestSource);
    if (N == 0 ||
        LoadWidth >
            static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) / N)
      return InvalidVA;
    int64_t RegionLo = FrameOff;
    const int64_t RegionBytes = static_cast<int64_t>(N) * LoadWidth;
    if ((RegionBytes > 0 &&
         RegionLo > std::numeric_limits<int64_t>::max() - RegionBytes) ||
        (RegionBytes < 0 &&
         RegionLo < std::numeric_limits<int64_t>::min() - RegionBytes))
      return InvalidVA;
    int64_t RegionHi = RegionLo + RegionBytes;

    // A store whose address is `table_base + variable_index` (the same shape as
    // the dispatch load, e.g. `tab[k] = ...` with a non-constant k) can write a
    // non-positional value into any entry, which canonFrameSlot rejects (the
    // scaled index has no constant offset).  Such a write cannot be proven
    // faithful, so it must flag the table as mutated rather than be skipped —
    // otherwise a runtime-permuted table is dispatched on its stale static map.
    auto storeHitsTableVarIndex = [&](NdVar AddrV, int FromIdx) -> bool {
      NdVar A = AddrV;
      int From = FromIdx;
      for (int Hop = 0; Hop < limits::kMaxQuasiCopyDepth; ++Hop) {
        if (!chargeEvidence())
          return false;
        if (!A.isReg() && !A.isTemp())
          return false;
        int D = budgetedReachingDefIdx(FuncOps, From, A);
        if (D < 0)
          return false;
        const LowOp &O = FuncOps[D];
        if ((O.Opcode == NdOp::COPY || isGuestAddressZExt(O)) &&
            O.NumInputs >= 1 && (O.Inputs[0].isReg() || O.Inputs[0].isTemp())) {
          A = O.Inputs[0];
          From = D - 1;
          continue;
        }
        if (O.Opcode == NdOp::INT_ADD && O.NumInputs >= 2) {
          for (int S = 0; S < 2; ++S) {
            if (!chargeEvidence())
              return false;
            if (budgetedScaledIndexReg(FuncOps, D - 1, O.Inputs[S]) ==
                InvalidVA)
              continue;
            uint64_t BB = InvalidVA;
            int64_t BOff = 0;
            if (canonFrameSlot(FuncOps, O.Inputs[1 - S], D - 1, BB, BOff) &&
                BB == FrameReg && BOff >= RegionLo && BOff < RegionHi)
              return true; // variable-index write into the table region
          }
        }
        return false;
      }
      return false;
    };

    for (int I = 0; I < static_cast<int>(FuncOps.size()); ++I) {
      if (!chargeEvidence())
        return InvalidVA;
      const LowOp &Op = FuncOps[I];
      if (Op.Opcode != NdOp::STORE || Op.NumInputs < 2)
        continue;
      uint64_t B = InvalidVA;
      int64_t Off = 0;
      if (!canonFrameSlot(FuncOps, Op.Inputs[0], I - 1, B, Off)) {
        if (storeHitsTableVarIndex(Op.Inputs[0], I - 1)) {
          BestMutated = true;
          break;
        }
        continue;
      }
      if (B != FrameReg)
        continue;
      int64_t SS = static_cast<int64_t>(Op.Inputs[1].Size);
      const std::optional<int64_t> StoreEnd = stackCheckedOffset(Off, SS);
      if (!StoreEnd)
        return InvalidVA;
      if (SS <= 0 || *StoreEnd <= RegionLo || Off >= RegionHi)
        continue; // store does not touch the table's frame region
      // A faithful initializer store copies constant entry e to slot e: its
      // base offset is entry-aligned and its value is loaded from BestSource +
      // (Off - base).  Anything else — a permuting overwrite or a non-constant
      // value — breaks the positional map, so the static targets are unsound.
      bool Faithful = false;
      std::vector<JumpTableValueOccurrence> FaithfulConsumers;
      std::vector<JumpTableFrameInitializerChunk::StaticSourcePiece>
          FaithfulSourceValues;
      std::optional<int64_t> EntryDelta;
      std::optional<va_t> ExpectedSource;
      if (Off >= RegionLo) {
        EntryDelta = stackCheckedOffset(Off, RegionLo, /*Subtract=*/true);
        if (!EntryDelta)
          return InvalidVA;
        ExpectedSource = checkedVAOffset(BestSource, *EntryDelta);
        if (!ExpectedSource)
          return InvalidVA;
      }
      if (EntryDelta && *EntryDelta % LoadWidth == 0) {
        if (auto Src =
                traceValToConstSrc(Op.Inputs[1], I - 1, 0, &FaithfulConsumers,
                                   &FaithfulSourceValues))
          Faithful = *Src == *ExpectedSource;
      }
      if (!Faithful) {
        BestMutated = true;
        break;
      }
      if (!chargeEvidence(FaithfulConsumers.size()) ||
          !chargeEvidence(FaithfulSourceValues.size()) || !chargeEvidence())
        return InvalidVA;
      BestStorageConsumers.insert(BestStorageConsumers.end(),
                                  FaithfulConsumers.begin(),
                                  FaithfulConsumers.end());
      JumpTableFrameInitializerChunk Initializer;
      Initializer.Writer = {Op.Inputs[0], Op.Addr, Op.Seq,
                            /*DefinedAtPoint=*/false};
      const std::optional<int64_t> DestinationAddend =
          stackCheckedOffset(RegionLo, Off, /*Subtract=*/true);
      if (!DestinationAddend || !ExpectedSource)
        return InvalidVA;
      Initializer.Destination = {
          {Op.Inputs[0], Op.Addr, Op.Seq, /*DefinedAtPoint=*/false},
          *DestinationAddend};
      Initializer.StoredValue = {Op.Inputs[1], Op.Addr, Op.Seq,
                                 /*DefinedAtPoint=*/false};
      Initializer.ByteCount = static_cast<uint64_t>(SS);
      Initializer.StaticSourceAddress = *ExpectedSource;
      Initializer.StaticSources = std::move(FaithfulSourceValues);
      BestInitializers.push_back(std::move(Initializer));
    }
  }

  if (!EvidenceComplete)
    return InvalidVA;

  // Sorting/uniquing is part of evidence work too.  Charging the retained
  // population bounds repeated candidate/stage normalization without making
  // comparator order observable in the budget.
  if (!chargeEvidence(BestInitializers.size()) ||
      !chargeEvidence(BestStorageConsumers.size()))
    return InvalidVA;

  std::sort(
      BestInitializers.begin(), BestInitializers.end(),
      [](const JumpTableFrameInitializerChunk &A,
         const JumpTableFrameInitializerChunk &B) {
        return std::tie(A.Writer.Addr, A.Writer.Seq,
                        A.Destination.Use.Value.Space,
                        A.Destination.Use.Value.Offset,
                        A.Destination.Use.Value.Size, A.Destination.ByteAddend,
                        A.ByteCount, A.StaticSourceAddress, A.IsMemcpy) <
               std::tie(B.Writer.Addr, B.Writer.Seq,
                        B.Destination.Use.Value.Space,
                        B.Destination.Use.Value.Offset,
                        B.Destination.Use.Value.Size, B.Destination.ByteAddend,
                        B.ByteCount, B.StaticSourceAddress, B.IsMemcpy);
      });
  BestInitializers.erase(
      std::unique(BestInitializers.begin(), BestInitializers.end()),
      BestInitializers.end());

  std::sort(
      BestStorageConsumers.begin(), BestStorageConsumers.end(),
      [](const JumpTableValueOccurrence &A, const JumpTableValueOccurrence &B) {
        return std::tie(A.Addr, A.Seq, A.Value.Space, A.Value.Offset,
                        A.Value.Size) < std::tie(B.Addr, B.Seq, B.Value.Space,
                                                 B.Value.Offset, B.Value.Size);
      });
  BestStorageConsumers.erase(
      std::unique(BestStorageConsumers.begin(), BestStorageConsumers.end()),
      BestStorageConsumers.end());
  if (BestSource != InvalidVA &&
      ((StorageConsumersOut && !chargeEvidence(BestStorageConsumers.size())) ||
       (InitializersOut && !chargeEvidence(BestInitializers.size()))))
    return InvalidVA;
  if (!EvidenceComplete)
    return InvalidVA;
  // Commit caller-visible evidence only after every scan, normalization and
  // copy charge has succeeded.  Exhaustion therefore leaves all outputs in
  // their entry state even when it occurs at the last retained item.
  if (StorageConsumersOut && BestSource != InvalidVA)
    *StorageConsumersOut = BestStorageConsumers;
  if (InitializersOut && BestSource != InvalidVA)
    *InitializersOut = BestInitializers;
  if (MutatedOut && BestSource != InvalidVA)
    *MutatedOut = BestMutated;

  return BestSource;
}

} // namespace neverd
