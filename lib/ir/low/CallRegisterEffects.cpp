//===- CallRegisterEffects.cpp - Callee GPR write summaries -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/ir/low/CallRegisterEffects.h"

#include "neverd/loader/BinaryImage.h"

#include <algorithm>

namespace neverd {

namespace {
constexpr uint64_t kX64GPRBytes = 16 * 8;
constexpr unsigned kX64StackPointerFamily = 4; // RSP

GPRFamilyMask familyBit(Arch A, uint64_t RegOff) {
  auto Family = gprFamilyOf(A, RegOff);
  if (!Family || *Family == kX64StackPointerFamily)
    return 0;
  return GPRFamilyMask(1) << *Family;
}

/// Record a read of \p Size bytes at \p RegOff.
void addRead(Arch A, GPRReadWidths &Reads, uint64_t RegOff, uint64_t Size) {
  auto Family = gprFamilyOf(A, RegOff);
  if (!Family || *Family == kX64StackPointerFamily)
    return;
  const uint64_t End = std::min<uint64_t>(RegOff % 8 + Size, 8);
  Reads[*Family] = std::max(Reads[*Family], static_cast<uint8_t>(End));
}

void joinReads(GPRReadWidths &Into, const GPRReadWidths &From) {
  for (size_t I = 0; I < Into.size(); ++I)
    Into[I] = std::max(Into[I], From[I]);
}
} // namespace

std::optional<unsigned> gprFamilyOf(Arch A, uint64_t RegOff) {
  if (A != Arch::X64 || RegOff >= kX64GPRBytes)
    return std::nullopt;
  return static_cast<unsigned>(RegOff / 8);
}

LocalRegisterEffect localRegisterEffect(const BinaryImage &Img,
                                        const LowFunc &F) {
  LocalRegisterEffect Effect;
  if (!F.hasCompleteLiftCoverage() ||
      !F.UnsafeIndirectBranchAddresses.empty() || F.Blocks.empty()) {
    Effect.Unknown = true;
    Effect.Incomplete = true;
    return Effect;
  }
  std::map<int, size_t> IndexOfId;
  std::set<va_t> BlockStarts;
  for (size_t I = 0; I < F.Blocks.size(); ++I) {
    IndexOfId[F.Blocks[I].Id] = I;
    BlockStarts.insert(F.Blocks[I].StartAddr);
  }
  Effect.Blocks.resize(F.Blocks.size());
  for (size_t BI = 0; BI < F.Blocks.size(); ++BI) {
    const LowBlock &Block = F.Blocks[BI];
    RegisterBlock &Out = Effect.Blocks[BI];
    for (int Succ : Block.Succs)
      if (auto It = IndexOfId.find(Succ); It != IndexOfId.end())
        Out.Succs.push_back(It->second);
    auto ControlOf = [&](size_t OpIndex) {
      for (const LowInstructionBoundary &Boundary : Block.InstructionBoundaries)
        if (OpIndex >= Boundary.FirstOp &&
            OpIndex - Boundary.FirstOp < Boundary.OpCount)
          return std::pair{Boundary.Control, Boundary.ControlFlags};
      return std::pair{LowInstructionControl::None,
                       LowInstructionControlFlag::None};
    };
    for (size_t I = 0; I < Block.Ops.size(); ++I) {
      const LowOp &Op = Block.Ops[I];
      RegisterStep Step;
      for (uint8_t In = 0; In < Op.NumInputs; ++In)
        if (Op.Inputs[In].isReg())
          addRead(Img.Arch, Step.Reads, Op.Inputs[In].Offset,
                  Op.Inputs[In].Size);
      if (Op.Output.isReg()) {
        const GPRFamilyMask Bit = familyBit(Img.Arch, Op.Output.Offset);
        Effect.Writes |= Bit;
        // A 32- or 64-bit write defines the whole register; a byte or word
        // write keeps the rest of the old value, which stays live exactly as
        // wide as a later read needs it.
        if (Op.Output.Size >= 4)
          Step.Kills |= Bit;
      }
      const auto [Control, Flags] = ControlOf(I);
      const bool TailCall = Control == LowInstructionControl::TailCall;
      switch (Op.Opcode) {
      case NdOp::INDIR_CALL:
        Effect.Unknown = true;
        Step.UnknownCall = true;
        break;
      case NdOp::INDIR_BR:
        // A resolved jump table has successors; an indirect tail jump leaves
        // for code this summary cannot see.
        if (Block.Succs.empty()) {
          Effect.Unknown = true;
          Step.UnknownTailCall = true;
        }
        break;
      case NdOp::BRANCH:
      case NdOp::COND_BR:
        // A direct branch to another function's entry leaves this body the
        // way a tail call does; the CFG keeps no block for it.
        if (Op.NumInputs > 0 && Op.Inputs[0].isConst() &&
            !BlockStarts.count(Op.Inputs[0].Offset)) {
          Effect.Callees.insert(Op.Inputs[0].Offset);
          Step.Callee = Op.Inputs[0].Offset;
        }
        break;
      case NdOp::CALL: {
        // A call that never returns cannot change a register its caller
        // reads, but it still receives this function's registers.
        const bool NoReturn = hasLowInstructionControlFlag(
            Flags, LowInstructionControlFlag::NoReturn);
        Step.Exits = NoReturn;
        if (Op.NumInputs == 0 || !Op.Inputs[0].isConst() ||
            Img.findImportAt(Op.Inputs[0].Offset)) {
          if (!NoReturn)
            Effect.Unknown = true;
          (TailCall ? Step.UnknownTailCall : Step.UnknownCall) = true;
        } else {
          Step.Callee = Op.Inputs[0].Offset;
          if (!NoReturn)
            Effect.Callees.insert(Op.Inputs[0].Offset);
        }
        break;
      }
      default:
        break;
      }
      Out.Steps.push_back(Step);
    }
  }
  return Effect;
}

namespace {
/// Bytes of each family live on entry to \p F, given the current callee
/// summaries.
GPRReadWidths entryLiveWidths(const LocalRegisterEffect &F,
                              const std::map<va_t, GPRFamilyMask> &MayWrite,
                              const std::map<va_t, GPRReadWidths> &EntryReads,
                              GPRFamilyMask VolatileFamilies,
                              GPRFamilyMask ArgumentFamilies,
                              const std::set<va_t> &DispatchThunks) {
  auto Clear = [](GPRReadWidths &Live, GPRFamilyMask Mask) {
    for (size_t I = 0; I < Live.size(); ++I)
      if ((Mask >> I) & 1)
        Live[I] = 0;
  };
  auto Transfer = [&](const RegisterBlock &Block, GPRReadWidths Live) {
    for (auto It = Block.Steps.rbegin(); It != Block.Steps.rend(); ++It) {
      const RegisterStep &Step = *It;
      if (Step.Exits)
        Live.fill(0);
      if (Step.UnknownTailCall) {
        Live.fill(0);
        for (size_t I = 0; I < Live.size(); ++I)
          if ((ArgumentFamilies >> I) & 1)
            Live[I] = 8;
      } else if (Step.UnknownCall || (Step.Callee != InvalidVA &&
                                      DispatchThunks.count(Step.Callee))) {
        Clear(Live, VolatileFamilies);
      } else if (Step.Callee != InvalidVA) {
        auto W = MayWrite.find(Step.Callee);
        Clear(Live, W != MayWrite.end() ? W->second : VolatileFamilies);
        if (auto R = EntryReads.find(Step.Callee); R != EntryReads.end())
          joinReads(Live, R->second);
      }
      Clear(Live, Step.Kills);
      joinReads(Live, Step.Reads);
    }
    return Live;
  };
  std::vector<GPRReadWidths> LiveIn(F.Blocks.size(), GPRReadWidths{});
  for (bool Changed = true; Changed;) {
    Changed = false;
    for (size_t B = F.Blocks.size(); B-- > 0;) {
      GPRReadWidths LiveOut{};
      for (size_t S : F.Blocks[B].Succs)
        if (S < LiveIn.size())
          joinReads(LiveOut, LiveIn[S]);
      const GPRReadWidths In = Transfer(F.Blocks[B], LiveOut);
      if (In != LiveIn[B]) {
        LiveIn[B] = In;
        Changed = true;
      }
    }
  }
  return LiveIn.empty() ? GPRReadWidths{} : LiveIn[0];
}
} // namespace

CallRegisterSummaries
solveCallRegisterEffects(const std::map<va_t, LocalRegisterEffect> &Funcs,
                         GPRFamilyMask VolatileFamilies,
                         GPRFamilyMask ArgumentFamilies,
                         const std::set<va_t> &DispatchThunks,
                         const std::map<va_t, GPRReadWidths> &FixedEntryReads) {
  CallRegisterSummaries Result;
  // Unknown is absorbing: a function is unknown if it or any callee is.
  std::set<va_t> Unknown;
  for (const auto &[Entry, Effect] : Funcs)
    if (Effect.Unknown)
      Unknown.insert(Entry);
  for (bool Changed = true; Changed;) {
    Changed = false;
    for (const auto &[Entry, Effect] : Funcs) {
      if (Unknown.count(Entry))
        continue;
      for (va_t Callee : Effect.Callees)
        if (!Funcs.count(Callee) || Unknown.count(Callee)) {
          Unknown.insert(Entry);
          Changed = true;
          break;
        }
    }
  }

  // Least fixed point of Writes(F) = Local(F) | Writes(callees); recursion
  // converges because masks only grow.
  std::map<va_t, GPRFamilyMask> &Writes = Result.MayWrite;
  for (const auto &[Entry, Effect] : Funcs)
    if (!Unknown.count(Entry))
      Writes[Entry] = Effect.Writes;
  for (bool Changed = true; Changed;) {
    Changed = false;
    for (auto &[Entry, Mask] : Writes) {
      GPRFamilyMask Next = Mask;
      for (va_t Callee : Funcs.at(Entry).Callees)
        Next |= Writes.at(Callee);
      if (Next != Mask) {
        Mask = Next;
        Changed = true;
      }
    }
  }

  // Entry reads, also a least fixed point: a callee's reads only grow a
  // caller's.  A body we do not fully know has none.
  std::map<va_t, GPRReadWidths> &Reads = Result.EntryReads;
  for (const auto &[Entry, Effect] : Funcs)
    if (!Effect.Incomplete)
      Reads[Entry] = GPRReadWidths{};
  for (const auto &[Entry, Widths] : FixedEntryReads)
    Reads[Entry] = Widths;
  for (bool Changed = true; Changed;) {
    Changed = false;
    for (auto &[Entry, Widths] : Reads) {
      if (FixedEntryReads.count(Entry) || !Funcs.count(Entry))
        continue;
      GPRReadWidths Next = Widths;
      joinReads(Next, entryLiveWidths(Funcs.at(Entry), Writes, Reads,
                                      VolatileFamilies, ArgumentFamilies,
                                      DispatchThunks));
      if (Next != Widths) {
        Widths = Next;
        Changed = true;
      }
    }
  }
  return Result;
}

} // namespace neverd
