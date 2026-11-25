//===- MedSavedArgumentRegisters.cpp - ARM registers only saved -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The AAPCS argument registers a function only saves.  A save alone supplies
/// no evidence of an argument: MSVC ARM pushes r3 beside its callee-saved
/// registers to keep the stack 8-byte aligned and pops it back unread
/// (`push {r3, r4, r11, lr}` ... `pop {r3, r4, r11, pc}`), and /Od code homes
/// argument registers it never reads back.  Such a register's incoming value
/// reaches nothing the function does.  A register a call reads is a
/// forwarded argument, which call recovery adds where the callee takes it.
///
//===----------------------------------------------------------------------===//

#include "../../pass/MedEntryStackOffsets.h"

#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/med/MedCallingConvDetail.h"
#include "neverd/ir/med/MedIR.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"

#include <cstdint>
#include <set>
#include <utility>
#include <vector>

namespace neverd::med_calling_conv_detail {

namespace {
/// Whether \p Op only moves the bytes of its inputs.
bool carries(const MedOp &Op) {
  switch (Op.Opcode) {
  case NdOp::COPY:
  case NdOp::SUBBYTES:
  case NdOp::INT_ZEXT:
  case NdOp::INT_SEXT:
  case NdOp::CONCAT:
    return true;
  default:
    return false;
  }
}

bool isCall(const MedOp &Op) {
  return Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL;
}
} // namespace

std::set<uint64_t> onlySavedArgumentRegisters(const MedFunc &Func,
                                              Arch TargetArch,
                                              BinaryFormat Format,
                                              llvm::ArrayRef<uint64_t> Regs) {
  std::set<uint64_t> Result;
  if (Func.Blocks.empty() || Regs.empty())
    return Result;
  const TargetRegInfo &TRI = getTargetRegInfo(TargetArch);
  MedEntryStackOffsets Offsets(Func, TargetArch, Format);
  const auto FrameOffset = [&](const MedVar &V) -> std::optional<int64_t> {
    if (V.isConst())
      return std::nullopt;
    return Offsets.query(V);
  };
  const auto InArgumentRegister = [&](const MedVar &V) {
    return V.Kind == MedVar::Reg && llvm::is_contained(Regs, V.RegOff);
  };

  // Where each value is read.
  struct Read {
    const MedOp *Op = nullptr;
    unsigned Input = 0;
    const PhiNode *Phi = nullptr;
  };
  llvm::DenseMap<ValueKey, llvm::SmallVector<Read, 2>> Reads;
  for (const MedBlock &Block : Func.Blocks) {
    for (const PhiNode &Phi : Block.Phis)
      for (const auto &[Pred, Arg] : Phi.Args)
        if (!Arg.isConst())
          Reads[valueKey(Arg)].push_back({nullptr, 0, &Phi});
    for (const MedOp &Op : Block.Ops)
      for (unsigned I = 0; I < Op.NumInputs; ++I)
        if (!Op.Inputs[I].isConst())
          Reads[valueKey(Op.Inputs[I])].push_back({&Op, I, nullptr});
  }

  // Each register whose incoming value every read moves on or stores in
  // the frame, with the slots it goes to.
  std::vector<std::pair<uint64_t, std::vector<std::pair<int64_t, int64_t>>>>
      Stored;
  for (const uint64_t Register : Regs) {
    llvm::SmallVector<MedVar, 4> Work;
    for (const MedOp &Op : Func.Blocks.front().Ops) {
      if (Op.Opcode != NdOp::COPY)
        break;
      if (!isEntryLiveInCopy(Op, Register))
        continue;
      Work.push_back(Op.Output);
      if (isRenamedEntryLiveInCopy(Op, Register))
        Work.push_back(Op.Inputs[0]);
    }
    std::vector<std::pair<int64_t, int64_t>> Slots;
    llvm::DenseSet<ValueKey> Seen;
    bool OnlyStored = !Work.empty();
    while (OnlyStored && !Work.empty()) {
      const MedVar Value = Work.pop_back_val();
      if (!Seen.insert(valueKey(Value)).second)
        continue;
      const auto Found = Reads.find(valueKey(Value));
      if (Found == Reads.end())
        continue;
      for (const Read &R : Found->second) {
        if (R.Phi) {
          Work.push_back(R.Phi->Output);
          continue;
        }
        const MedOp &Op = *R.Op;
        if (carries(Op)) {
          Work.push_back(Op.Output);
          continue;
        }
        if (Op.Opcode == NdOp::STORE && R.Input == 1)
          if (const auto Offset = FrameOffset(Op.Inputs[0])) {
            Slots.emplace_back(*Offset, *Offset + Op.Inputs[1].Size);
            continue;
          }
        OnlyStored = false;
        break;
      }
    }
    if (OnlyStored && !Slots.empty())
      Stored.emplace_back(Register, std::move(Slots));
  }
  if (Stored.empty())
    return Result;

  // A frame address may address the function's own loads and stores and
  // make other frame addresses, and nothing else: one stored, passed in an
  // argument register or turned into anything else lets other code read the
  // frame, as a va_list does.
  for (const MedBlock &Block : Func.Blocks) {
    for (const PhiNode &Phi : Block.Phis)
      for (const auto &[Pred, Arg] : Phi.Args)
        if (FrameOffset(Arg) &&
            (!FrameOffset(Phi.Output) || InArgumentRegister(Phi.Output)))
          return Result;
    for (const MedOp &Op : Block.Ops)
      for (unsigned I = 0; I < Op.NumInputs; ++I) {
        if (!FrameOffset(Op.Inputs[I]))
          continue;
        if ((Op.Opcode == NdOp::LOAD || Op.Opcode == NdOp::STORE) && I == 0)
          continue;
        if ((Op.Opcode == NdOp::COPY || Op.Opcode == NdOp::INT_ADD ||
             Op.Opcode == NdOp::INT_SUB) &&
            FrameOffset(Op.Output) && !InArgumentRegister(Op.Output))
          continue;
        return Result;
      }
  }

  // Whether something takes a byte of \p Value other than the moves that
  // carry it on, as the copy of a `pop`'s load into its register: a register
  // a return hands back is taken, which no return lists yet.
  const auto Taken = [&](const MedVar &Value) {
    llvm::SmallVector<MedVar, 4> Work{Value};
    llvm::DenseSet<ValueKey> Seen;
    while (!Work.empty()) {
      const MedVar V = Work.pop_back_val();
      if (!Seen.insert(valueKey(V)).second)
        continue;
      if (V.Kind == MedVar::Reg && TRI.isReturnReg(V.RegOff))
        return true;
      const auto Found = Reads.find(valueKey(V));
      if (Found == Reads.end())
        continue;
      for (const Read &R : Found->second) {
        if (R.Phi) {
          Work.push_back(R.Phi->Output);
          continue;
        }
        if (!carries(*R.Op))
          return true;
        Work.push_back(R.Op->Output);
      }
    }
    return false;
  };

  // The frame's loads.  One reads nothing where nothing takes its value and
  // its block returns with no call after it, which could take the register
  // it went to as an argument.
  struct FrameLoad {
    int64_t Begin = 0;
    int64_t End = 0;
    bool Unread = false;
  };
  std::vector<FrameLoad> Loads;
  for (const MedBlock &Block : Func.Blocks)
    for (size_t I = 0; I < Block.Ops.size(); ++I) {
      const MedOp &Op = Block.Ops[I];
      if (Op.Opcode != NdOp::LOAD || Op.NumInputs < 1)
        continue;
      const auto Offset = FrameOffset(Op.Inputs[0]);
      if (!Offset)
        continue;
      const bool Unread =
          !Taken(Op.Output) && Block.Ops.back().Opcode == NdOp::RETURN &&
          std::none_of(Block.Ops.begin() + static_cast<std::ptrdiff_t>(I) + 1,
                       Block.Ops.end(), isCall);
      Loads.push_back({*Offset, *Offset + Op.Output.Size, Unread});
    }

  // Whether a return may hand back \p Register as the function was entered
  // with it: some way from the entry to a return writes no byte of it, nor
  // calls, as a destructor that returns `this` leaves r0.  That is the
  // return's value until the return contracts settle it, which no return
  // lists yet.
  llvm::DenseMap<int, size_t> IndexOf;
  for (size_t B = 0; B < Func.Blocks.size(); ++B)
    IndexOf[Func.Blocks[B].Id] = B;
  const auto MayReturnIncoming = [&](uint64_t Register) {
    const auto Writes = [&](const MedBlock &Block) {
      return llvm::any_of(Block.Ops, [&](const MedOp &Op) {
        if (isCall(Op))
          return true;
        const MedVar &Out = Op.Output;
        return Out.Kind == MedVar::Reg && Out.Size &&
               Out.RegOff < Register + TRI.FullRegWidth &&
               Register < Out.RegOff + Out.Size &&
               !isEntryLiveInCopy(Op, Out.RegOff);
      });
    };
    std::vector<char> Untouched(Func.Blocks.size(), 0);
    llvm::SmallVector<size_t, 8> Work{0};
    Untouched[0] = 1;
    while (!Work.empty()) {
      const MedBlock &Block = Func.Blocks[Work.pop_back_val()];
      if (Writes(Block))
        continue;
      if (!Block.Ops.empty() && Block.Ops.back().Opcode == NdOp::RETURN)
        return true;
      for (const int Succ : Block.Succs)
        if (const auto Found = IndexOf.find(Succ);
            Found != IndexOf.end() && !Untouched[Found->second]) {
          Untouched[Found->second] = 1;
          Work.push_back(Found->second);
        }
    }
    return false;
  };

  // A register is only saved where no load that reads anything takes a byte
  // of its slots.
  for (const auto &[Register, Slots] : Stored)
    if ((Register != TRI.IntReturnReg || !MayReturnIncoming(Register)) &&
        llvm::all_of(Loads, [&](const FrameLoad &Load) {
          return Load.Unread ||
                 llvm::none_of(
                     Slots, [&](const std::pair<int64_t, int64_t> &S) {
                       return Load.Begin < S.second && S.first < Load.End;
                     });
        }))
      Result.insert(Register);
  return Result;
}

} // namespace neverd::med_calling_conv_detail
