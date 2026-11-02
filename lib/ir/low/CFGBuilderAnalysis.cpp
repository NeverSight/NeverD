//===- CFGBuilderAnalysis.cpp - Bounded PE32 display recovery -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Optional stack equations and table candidates for analysis views. These
/// guesses never enter a default builder or an integer-frame proof.
///
//===----------------------------------------------------------------------===//

#include "neverd/Limits.h"
#include "neverd/decode/Decoder.h"
#include "neverd/ir/Limits.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/lift/X86/X86Regs.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/COFF/COFFStackProbe.h"
#include "neverd/support/BinaryEncoding.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <optional>
#include <set>
#include <vector>

namespace neverd {

bool CFGBuilder::usesAnalysisRecovery() const {
  return AnalysisRecoveryEnabled && !SkipX87StackFixup && CurrentImg &&
         CurrentImg->Arch == Arch::X86 && CurrentImg->isCOFF() &&
         CurrentImg->getPointerSize() == sizeof(uint32_t);
}

std::optional<uint32_t> CFGBuilder::analysisCallStackPop(va_t Target) const {
  if (!usesAnalysisRecovery())
    return std::nullopt;
  if (const auto It = AnalysisCallPops.find(Target);
      It != AnalysisCallPops.end()) {
    AnalysisRecoveryUsed |= It->second.has_value();
    return It->second;
  }
  if (AnalysisStackWork >= limits::ir::kAnalysisStackInstructionsPerFunction)
    return std::nullopt;

  // Under the ordinary returning-call assumption, all observed near returns
  // must agree on ESP_after = ESP_before + ret_pop. Calls are not expanded:
  // this is a display equation, not a proof of their effects or return paths.
  // Conflicting exits, indirect tails, undecodable bytes and budget exhaustion
  // keep the equation unknown instead of picking a convenient return.
  auto Scan = [&]() -> std::optional<uint32_t> {
    Decoder Decode;
    if (!Decode.init(*CurrentImg))
      return std::nullopt;
    std::vector<va_t> Pending{Target};
    std::set<va_t> Seen;
    std::optional<uint32_t> Pop;
    while (!Pending.empty()) {
      const va_t PC = Pending.back();
      Pending.pop_back();
      if (!Seen.insert(PC).second)
        continue;
      if (++AnalysisStackWork >
              limits::ir::kAnalysisStackInstructionsPerFunction ||
          Seen.size() > limits::ir::kAnalysisStackInstructionsPerCallee)
        return std::nullopt;
      const auto *Segment = CurrentImg->getSegmentFor(PC);
      if (!Segment || !Segment->isExecutable() || PC < Segment->VA ||
          PC - Segment->VA >= Segment->Data.size())
        return std::nullopt;
      // Fifteen is the architectural maximum x86 instruction length.
      const size_t Bytes =
          std::min<size_t>(15, Segment->Data.size() - (PC - Segment->VA));
      const auto *Code = CurrentImg->readVA(PC, Bytes);
      DecodedInsn Insn{};
      if (!Code || !Decode.decodeOne(Code, Bytes, PC, Insn) || !Insn.Raw ||
          !Insn.Raw->detail || Insn.Size > InvalidVA - PC)
        return std::nullopt;
      if (Insn.Id == X86_INS_RET) {
        const auto Immediate = Decode.returnImmediate(Insn).value_or(0);
        if (Immediate > UINT16_MAX || (Pop && *Pop != Immediate))
          return std::nullopt;
        Pop = static_cast<uint32_t>(Immediate);
        continue;
      }
      const auto &X86 = Insn.Raw->detail->x86;
      if (Insn.Id == X86_INS_XCHG)
        for (unsigned I = 0; I < X86.op_count; ++I)
          if (X86.operands[I].type == X86_OP_REG &&
              (X86.operands[I].reg == X86_REG_ESP ||
               X86.operands[I].reg == X86_REG_SP))
            return std::nullopt;
      if (cs_insn_group(Decode.getHandle(), Insn.Raw, CS_GRP_JUMP)) {
        if (X86.op_count != 1 || X86.operands[0].type != X86_OP_IMM)
          return std::nullopt;
        Pending.push_back(static_cast<uint32_t>(X86.operands[0].imm));
        if (Insn.Id == X86_INS_JMP)
          continue;
      } else if (Decode.isFunctionTerminator(Insn) &&
                 !cs_insn_group(Decode.getHandle(), Insn.Raw, CS_GRP_CALL)) {
        return std::nullopt;
      }
      Pending.push_back(PC + Insn.Size);
    }
    return Pop;
  };
  const auto Pop = Scan();
  AnalysisCallPops.emplace(Target, Pop);
  AnalysisRecoveryUsed |= Pop.has_value();
  return Pop;
}

std::optional<int64_t>
CFGBuilder::analysisCallStackDelta(const LowOp &Call) const {
  if (!usesAnalysisRecovery() || Call.Opcode != NdOp::CALL ||
      Call.NumInputs != 1 || !Call.Inputs[0].isConst())
    return std::nullopt;
  if (const auto Probe = coff_loader::getCheckedX86AllocatingStackProbeCall(
          *CurrentImg, Call.Addr);
      Probe && Probe->Helper.Begin == Call.Inputs[0].Offset) {
    // Only the ordinary successful, nonwrapping path supplies this display
    // equation. Keep the call and every probe access in the original body.
    AnalysisRecoveryUsed = true;
    return -int64_t{Probe->AllocationBytes};
  }
  const auto Pop = analysisCallStackPop(Call.Inputs[0].Offset);
  return Pop ? std::optional<int64_t>{*Pop} : std::nullopt;
}

void CFGBuilder::recoverAnalysisTables(const BinaryImage &Img, Decoder &Dec,
                                       LowFunc &Func) {
  if (!usesAnalysisRecovery())
    return;
  std::set<va_t> Examined;
  size_t TargetsRemaining = limits::ir::kAnalysisTableTargetsPerFunction;
  std::optional<std::set<va_t>> ReferencedBases;
  const va_t NextEntry = nextKnownFunctionEntry(CurrentFuncEntry);
  const auto CanonicalJump = [](const uint8_t *Code) {
    return Code && Code[0] == 0xff && Code[1] == 0x24 &&
           (Code[2] & 0xc7) == 0x85 && ((Code[2] >> 3) & 7) != 4;
  };
  for (unsigned Round = 0; Round < limits::ir::kAnalysisTableRounds; ++Round) {
    std::vector<va_t> Candidates;
    for (const auto &[PC, Record] : Insns)
      if (PublishedReachableInsns.count(PC) && Record.IsBranch &&
          Record.IsIndirect && !Record.IsCall && !Record.IsRet &&
          !Record.IsCond && Record.JumpTableTargets.empty() &&
          Examined.insert(PC).second)
        Candidates.push_back(PC);
    bool Changed = false;
    for (va_t PC : Candidates) {
      auto &Record = Insns.at(PC);
      // Canonical i386 JMP [index*4 + disp32]. Restrict the optional strategy
      // to an exact instruction shape; other layouts keep ordinary recovery.
      const auto *Code = Img.readVA(PC, 7);
      if (Record.Size != 7 || !CanonicalJump(Code))
        continue;
      const va_t Base = readLE<uint32_t>(Code + 3);
      auto Slot = Img.CodePtrRelocSlots.lower_bound(Base);
      if (Slot == Img.CodePtrRelocSlots.end() || *Slot != Base)
        continue;
      const auto *Storage = Img.getSegmentFor(Base);
      if (!Storage || Storage->isWritable())
        continue;
      if (!ReferencedBases) {
        ReferencedBases.emplace();
        const auto *Text = Img.getSegmentFor(PC);
        if (!Text || Text->Data.size() > InvalidVA - Text->VA)
          return;
        const va_t End = std::min(NextEntry, Text->VA + Text->Data.size());
        // Adjacent tables can share one relocation run. A separately
        // relocated canonical dispatch names a candidate boundary even before
        // its case becomes reachable. Inspect only this function's pointer
        // fields, charging input targets to the same total as output targets.
        const auto Reference = [&](va_t Source,
                                   const RelocatedAddressField *Field) {
          if (!TargetsRemaining)
            return false;
          --TargetsRemaining;
          if (Source - CurrentFuncEntry < 3 || End - Source < sizeof(uint32_t))
            return true;
          const auto *Branch = Img.readVA(Source - 3, 7);
          if (CanonicalJump(Branch)) {
            const va_t Target = readLE<uint32_t>(Branch + 3);
            if (Field &&
                (Field->Width != sizeof(uint32_t) ||
                 Field->EncodedValue != Target || Field->TargetVA != Target ||
                 Field->PCRelativeFromInstructionEnd ||
                 Field->Kind != RelocatedAddressFieldKind::Generic))
              return true;
            if (Img.CodePtrRelocSlots.count(Target))
              ReferencedBases->insert(Target);
          }
          return true;
        };
        for (auto Ref = Img.CodePtrRelocSlots.lower_bound(CurrentFuncEntry);
             Ref != Img.CodePtrRelocSlots.end() && *Ref < End; ++Ref)
          if (!Reference(*Ref, nullptr))
            return;
        // Loader-identified instruction fields are not data pointer slots.
        // Preserve that distinction while finding the same physical base.
        for (const auto *Fields :
             {&Img.CodeAddressRelocOperands, &Img.DataAddressRelocOperands})
          for (auto Ref = Fields->lower_bound(CurrentFuncEntry);
               Ref != Fields->end() && Ref->first < End; ++Ref)
            if (!Reference(Ref->first, &Ref->second))
              return;
      }
      const auto Neighbor = ReferencedBases->upper_bound(Base);
      const va_t TableEnd =
          Neighbor == ReferencedBases->end() ? InvalidVA : *Neighbor;
      if (TableEnd != InvalidVA && (TableEnd - Base) % sizeof(uint32_t))
        continue;
      JumpTable Table;
      Table.AnalysisOnly = true;
      Table.InsnAddr = PC;
      Table.BaseAddr = Base;
      Table.HasBaseAddr = true;
      Table.EntrySize = Table.EntryStride = sizeof(uint32_t);
      Table.IndexRegOff = x86reg::generalReg((Code[2] >> 3) & 7);
      Table.TableLoadAddr = PC;
      bool Valid = true;
      va_t Address = Base;
      for (; Slot != Img.CodePtrRelocSlots.end() && *Slot == Address &&
             Address < TableEnd;
           ++Slot, Address += sizeof(uint32_t)) {
        if (Table.Targets.size() >= limits::ir::kAnalysisTableSlots ||
            Table.Targets.size() >= TargetsRemaining ||
            Address > UINT32_MAX - sizeof(uint32_t) ||
            Img.getSegmentFor(Address) != Storage) {
          Valid = false;
          break;
        }
        const auto *Bytes = Img.readVA(Address, sizeof(uint32_t));
        const va_t Target = Bytes ? readLE<uint32_t>(Bytes) : InvalidVA;
        const auto *Text = Img.getSegmentFor(Target);
        auto Existing = Insns.upper_bound(Target);
        const bool Overlap = Existing != Insns.begin() &&
                             std::prev(Existing)->first != Target &&
                             Target - std::prev(Existing)->first <
                                 std::prev(Existing)->second.Size;
        if (!Bytes || !Text || !Text->isExecutable() || Text->isWritable() ||
            Target < CurrentFuncEntry ||
            (NextEntry != InvalidVA && Target >= NextEntry) || Overlap ||
            (Target != CurrentFuncEntry && isKnownFunctionEntry(Target))) {
          Valid = false;
          break;
        }
        Table.SlotIndices.push_back(Table.Targets.size());
        Table.CaseLabels.push_back(Table.Targets.size());
        Table.Targets.push_back(Target);
      }
      if (!Valid || Table.Targets.size() < limits::kMinJumpTableEntries)
        continue;
      // No selector proof, relocation suppression, or authenticated LOAD is
      // fabricated. The readable table prefix is explicitly only a guess.
      Table.HasDispatchSlotMap = true;
      Table.StorageRanges.push_back(
          {Base, sizeof(uint32_t), sizeof(uint32_t), Table.Targets.size()});
      Record.JumpTableTargets = Table.Targets;
      TargetsRemaining -= Table.Targets.size();
      AnalysisTables.emplace(PC, Table);
      AnalysisRecoveryUsed = true;
      Changed = true;
      for (va_t Target : Table.Targets) {
        BlockStarts.insert(Target);
        if (!ExploredAddrs.count(Target))
          explore(Img, Dec, Target);
      }
    }
    if (!Changed)
      break;
    splitBlocks();
    rebuildBlocks(Func);
  }
}

} // namespace neverd
