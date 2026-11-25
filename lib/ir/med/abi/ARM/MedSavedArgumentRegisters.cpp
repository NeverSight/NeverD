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
/// (`push {r3, r4, r11, lr}` ... `pop {r3, r4, r11, pc}`), reserves words for
/// its locals with `push {r2, r3, ...}` and writes them before it reads them,
/// and its /Od code homes argument registers it never reads back.  Such a
/// register's incoming value reaches nothing the function does, nor do the
/// stores that save it, which go.  A register a call reads is a forwarded
/// argument, which call recovery adds where the callee takes it.
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

#include <algorithm>
#include <cstdint>
#include <optional>
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

namespace {
/// The argument registers \p Func only saves, with the stores that save
/// them in \p Saves.
std::set<uint64_t> onlySaved(const MedFunc &Func, Arch TargetArch,
                             BinaryFormat Format, llvm::ArrayRef<uint64_t> Regs,
                             std::vector<const MedOp *> &SavesOut) {
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
  // the frame, with the slots it goes to and the stores that save it.
  struct Saved {
    uint64_t Register = 0;
    std::vector<std::pair<int64_t, int64_t>> Slots;
    std::vector<const MedOp *> Saves;
  };
  std::vector<Saved> Stored;
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
    std::vector<const MedOp *> Saves;
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
            Saves.push_back(&Op);
            continue;
          }
        OnlyStored = false;
        break;
      }
    }
    if (OnlyStored && !Slots.empty())
      Stored.push_back({Register, std::move(Slots), std::move(Saves)});
  }
  if (Stored.empty())
    return Result;

  // A frame address may address the function's own loads and stores and
  // make other frame addresses, and nothing else: one stored, passed in an
  // argument register or turned into anything else lets other code read the
  // frame, as a va_list does.  A join nothing reads takes it nowhere, as the
  // frame pointer's at an epilogue that pops the register.
  for (const MedBlock &Block : Func.Blocks) {
    for (const PhiNode &Phi : Block.Phis)
      for (const auto &[Pred, Arg] : Phi.Args)
        if (FrameOffset(Arg) &&
            (!FrameOffset(Phi.Output) || InArgumentRegister(Phi.Output)) &&
            Reads.count(valueKey(Phi.Output)))
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
  llvm::DenseMap<const MedOp *, size_t> LoadIndex;
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
      LoadIndex[&Op] = Loads.size();
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

  // Whether every load that takes a byte of [Begin, End) reads bytes other
  // stores overwrote on every way to it from \p Save in the entry block, as
  // a slot MSVC reserves with `push {r2, r3, ...}` is a local the function
  // writes before it reads.  A handler the unwinder enters knows what each
  // block it guards had written when it began, as the exception may strike
  // before any of its stores; a block no edge reaches knows of no write.
  const auto OverwrittenBeforeRead = [&](const MedOp *Save, int64_t Begin,
                                         int64_t End) {
    const int64_t Width = End - Begin;
    if (Width <= 0 || Width > 32)
      return false;
    const uint32_t All =
        Width == 32 ? ~uint32_t{0} : (uint32_t{1} << Width) - 1;
    const auto Bytes = [&](int64_t Lo, int64_t Hi) -> uint32_t {
      Lo = std::max(Lo, Begin);
      Hi = std::min(Hi, End);
      if (Lo >= Hi)
        return 0;
      const int64_t N = Hi - Lo;
      return (N == 32 ? ~uint32_t{0} : (uint32_t{1} << N) - 1) << (Lo - Begin);
    };
    const MedBlock &Entry = Func.Blocks.front();
    const auto SaveAt =
        std::find_if(Entry.Ops.begin(), Entry.Ops.end(),
                     [&](const MedOp &Op) { return &Op == Save; });
    if (SaveAt == Entry.Ops.end())
      return false;
    const size_t First = static_cast<size_t>(SaveAt - Entry.Ops.begin()) + 1;
    // What each block's ops write of the bytes, and whether a taken load
    // reads one before it is written, from what the block enters with.
    const auto Walk = [&](size_t B, uint32_t Mask,
                          bool Check) -> std::optional<uint32_t> {
      const MedBlock &Block = Func.Blocks[B];
      for (size_t I = B == 0 ? First : 0; I < Block.Ops.size(); ++I) {
        const MedOp &Op = Block.Ops[I];
        if (Op.Opcode == NdOp::STORE && Op.NumInputs >= 2)
          if (const auto Offset = FrameOffset(Op.Inputs[0]))
            Mask |= Bytes(*Offset, *Offset + Op.Inputs[1].Size);
        if (Check && Op.Opcode == NdOp::LOAD)
          if (const auto Found = LoadIndex.find(&Op);
              Found != LoadIndex.end()) {
            const FrameLoad &Load = Loads[Found->second];
            const uint32_t Read = Bytes(Load.Begin, Load.End);
            if (!Load.Unread && (Mask & Read) != Read)
              return std::nullopt;
          }
      }
      return Mask;
    };
    std::vector<uint32_t> In(Func.Blocks.size(), All);
    std::vector<char> Reached(Func.Blocks.size(), 0);
    llvm::SmallVector<size_t, 8> Work;
    Reached[0] = 1;
    In[0] = 0;
    Work.push_back(0);
    for (size_t B = 1; B < Func.Blocks.size(); ++B)
      if (Func.Blocks[B].Preds.empty() &&
          Func.Blocks[B].ExceptionalPreds.empty()) {
        Reached[B] = 1;
        In[B] = 0;
        Work.push_back(B);
      }
    const auto Meet = [&](int Id, uint32_t Mask) {
      const auto Found = IndexOf.find(Id);
      if (Found == IndexOf.end())
        return;
      const size_t S = Found->second;
      const uint32_t Next = Reached[S] ? (In[S] & Mask) : Mask;
      if (Reached[S] && Next == In[S])
        return;
      Reached[S] = 1;
      In[S] = Next;
      Work.push_back(S);
    };
    while (!Work.empty()) {
      const size_t B = Work.pop_back_val();
      const uint32_t Out = *Walk(B, In[B], /*Check=*/false);
      for (const int Succ : Func.Blocks[B].Succs)
        Meet(Succ, Out);
      for (const ExceptionalEdge &Edge : Func.Blocks[B].ExceptionalSuccs)
        Meet(Edge.BlockId, In[B]);
    }
    for (size_t B = 0; B < Func.Blocks.size(); ++B)
      if (Reached[B] && !Walk(B, In[B], /*Check=*/true))
        return false;
    return true;
  };

  // A register is only saved where no load that reads anything takes a byte
  // of its slots, or, saved once in the entry block, none takes a byte it
  // has not seen overwritten.
  for (const Saved &S : Stored) {
    if (S.Register == TRI.IntReturnReg && MayReturnIncoming(S.Register))
      continue;
    const bool Unread = llvm::all_of(Loads, [&](const FrameLoad &Load) {
      return Load.Unread ||
             llvm::none_of(
                 S.Slots, [&](const std::pair<int64_t, int64_t> &Slot) {
                   return Load.Begin < Slot.second && Slot.first < Load.End;
                 });
    });
    if (Unread || (S.Saves.size() == 1 &&
                   OverwrittenBeforeRead(S.Saves.front(), S.Slots.front().first,
                                         S.Slots.front().second))) {
      Result.insert(S.Register);
      SavesOut.insert(SavesOut.end(), S.Saves.begin(), S.Saves.end());
    }
  }
  return Result;
}
} // namespace

std::set<uint64_t>
dropOnlySavedArgumentRegisters(MedFunc &Func, Arch TargetArch,
                               BinaryFormat Format,
                               llvm::ArrayRef<uint64_t> Regs) {
  std::vector<const MedOp *> Saves;
  std::set<uint64_t> Registers =
      onlySaved(Func, TargetArch, Format, Regs, Saves);
  if (Saves.empty())
    return Registers;
  // What the saves store is no value the function or anything after it
  // sees: neither C route writes it.
  const llvm::DenseSet<const MedOp *> Dropped(Saves.begin(), Saves.end());
  for (MedBlock &Block : Func.Blocks)
    llvm::erase_if(Block.Ops,
                   [&](const MedOp &Op) { return Dropped.contains(&Op); });
  // Nor is the value the register came in with, once nothing reads it.
  llvm::DenseSet<ValueKey> Read;
  for (const MedBlock &Block : Func.Blocks) {
    for (const PhiNode &Phi : Block.Phis)
      for (const auto &[Pred, Arg] : Phi.Args)
        if (!Arg.isConst())
          Read.insert(valueKey(Arg));
    for (const MedOp &Op : Block.Ops)
      for (unsigned I = 0; I < Op.NumInputs; ++I)
        if (!Op.Inputs[I].isConst() &&
            !(Op.Opcode == NdOp::COPY && Op.Inputs[I].Kind == Op.Output.Kind &&
              Op.Inputs[I].Id == Op.Output.Id))
          Read.insert(valueKey(Op.Inputs[I]));
  }
  auto &Entry = Func.Blocks.front().Ops;
  llvm::erase_if(Entry, [&](const MedOp &Op) {
    return Op.Opcode == NdOp::COPY &&
           llvm::any_of(Registers,
                        [&](uint64_t R) { return isEntryLiveInCopy(Op, R); }) &&
           !Read.contains(valueKey(Op.Output)) &&
           !Read.contains(valueKey(Op.Inputs[0]));
  });
  return Registers;
}

} // namespace neverd::med_calling_conv_detail
