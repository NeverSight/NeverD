//===- MedFrameBorrowSpills.h - Private pointer transport ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Index exact private-word spills in a straight-line source callee. This
/// supplies transport edges, not a native call or nonescape certificate.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_MED_PASS_MEDFRAMEBORROWSPILLS_H
#define NEVERD_IR_MED_PASS_MEDFRAMEBORROWSPILLS_H

#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/low/SourceFrameAnalysis.h"
#include "neverd/ir/med/MedStackAlignment.h"

#include "llvm/Support/MathExtras.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <vector>

namespace neverd::detail {

class FrameBorrowSpills {
public:
  FrameBorrowSpills(const MedFunc &Func, BinaryFormat Format, size_t &Budget) {
    if (Func.Blocks.size() != 1 || !Func.Blocks.front().Phis.empty() ||
        Func.FrameSize <= 0)
      return;
    const auto &TRI = getTargetRegInfo(Arch::X64);
    const auto &Ops = Func.Blocks.front().Ops;
    if (Ops.empty() || Ops.back().Opcode != NdOp::RETURN)
      return;
    std::vector<MedVar> Values;
    Values.reserve(Ops.size());
    for (const MedOp &Op : Ops) {
      if (!Budget)
        return;
      --Budget;
      if (Op.Dead)
        continue;
      // Calls, intrinsics and ordered/segmented memory need their own
      // lifetime and alias proof. An opcode's unused result is insufficient.
      if (Op.MemoryOrdering != NdMemoryOrdering::None ||
          Op.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
          (Op.Opcode == NdOp::RETURN && &Op != &Ops.back()) ||
          (!isSourceFrameScalarTransfer(Op.Opcode) && Op.Opcode != NdOp::LOAD &&
           Op.Opcode != NdOp::STORE && Op.Opcode != NdOp::RETURN))
        return;
      if (Op.Opcode == NdOp::LOAD || Op.Opcode == NdOp::STORE) {
        if (Op.NumInputs != (Op.Opcode == NdOp::LOAD ? 1 : 2))
          return;
        Values.push_back(Op.Inputs[0]);
      }
      if (Op.Output.Kind == MedVar::Reg &&
          Op.Output.RegOff == TRI.StackPointer) {
        if (Op.Output.Size != TRI.PointerSize)
          return;
        Values.push_back(Op.Output);
      }
    }
    const auto Offsets =
        entryStackOffsets(Func, Values, Arch::X64, Format, Func.EntryKind);
    int64_t Stack = 0;
    bool Releasing = false;
    size_t Next = 0;
    for (size_t I = 0; I < Ops.size(); ++I) {
      if (!Budget)
        return;
      --Budget;
      const auto &Op = Ops[I];
      if (Op.Dead)
        continue;
      if (Op.Opcode == NdOp::LOAD || Op.Opcode == NdOp::STORE) {
        const auto Address = Offsets[Next++];
        const auto Bytes =
            Op.Opcode == NdOp::LOAD ? Op.Output.Size : Op.Inputs[1].Size;
        int64_t End;
        if (!Bytes ||
            (Address && llvm::AddOverflow(*Address, int64_t(Bytes), End)))
          return;
        if (Address) {
          // Index every overlapped word, including partial and wide accesses.
          // Querying a spill never rescans the function or all other spills.
          const int64_t Word = TRI.PointerSize;
          const int64_t First = *Address - ((*Address % Word + Word) % Word);
          const auto Count =
              static_cast<uint64_t>((End - 1 - First) / Word + 1);
          if (Count > Budget)
            return;
          Budget -= Count;
          for (uint64_t W = 0; W < Count; ++W) {
            const int64_t At = First + static_cast<int64_t>(W) * Word;
            auto &Slot = Slots[At];
            if (*Address != At || Bytes != Word || At < Stack ||
                At < -Func.FrameSize || At > -Word)
              Slot.Exact = false;
            if (Op.Opcode == NdOp::STORE) {
              // An omitted source store is still a possible native pointer
              // transport, but cannot kill an earlier reaching source value.
              if (Op.PrologueSave && Slot.LastStore)
                Slot.Exact = false;
              Stores.try_emplace(&Op, Spill{At, {}});
              Slot.LastStore = &Op;
            } else if (Slot.LastStore) {
              Stores.at(Slot.LastStore).Loads.push_back(Op.Output);
            } else
              Slot.Exact = false;
          }
        }
      }
      if (Op.Output.Kind == MedVar::Reg &&
          Op.Output.RegOff == TRI.StackPointer) {
        const auto Offset = Offsets[Next++];
        // A released slot cannot reacquire its former pointer identity.
        // Monotone allocation followed by release also makes each access's
        // current-SP check sufficient for the whole spill lifetime.
        if (!Offset || *Offset > 0 || (Releasing && *Offset < Stack))
          return;
        Releasing |= *Offset > Stack;
        Stack = *Offset;
      }
    }
    Valid = Stack == 0;
  }

  const std::vector<MedVar> *loads(const MedOp &Store) const {
    const auto Found = Stores.find(&Store);
    if (!Valid || Found == Stores.end())
      return nullptr;
    if (!Slots.at(Found->second.Offset).Exact)
      return nullptr;
    return &Found->second.Loads;
  }

private:
  struct Slot {
    bool Exact = true;
    const MedOp *LastStore = nullptr;
  };
  struct Spill {
    int64_t Offset;
    std::vector<MedVar> Loads;
  };
  std::map<int64_t, Slot> Slots;
  std::map<const MedOp *, Spill> Stores;
  bool Valid = false;
};

} // namespace neverd::detail

#endif // NEVERD_IR_MED_PASS_MEDFRAMEBORROWSPILLS_H
