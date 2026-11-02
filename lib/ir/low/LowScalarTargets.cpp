//===- LowScalarTargets.cpp - Finite scalar targets ----------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Propagate finite integer sets without reading or emulating guest memory.
//===----------------------------------------------------------------------===//

#include "LowScalarTargets.h"

#include "neverd/Limits.h"
#include "neverd/ir/low/CallRegisterBody.h"
#include "neverd/ir/low/LowScalarConstant.h"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <set>
#include <utility>
#include <vector>

namespace neverd {
namespace {
using Values = std::vector<uint64_t>;
using Slot = std::pair<uint64_t, uint16_t>;
using State = std::map<Slot, Values>;

uint64_t widthMask(uint16_t Size) {
  return UINT64_MAX >> ((sizeof(uint64_t) - Size) * 8);
}

void canonicalize(Values &V) {
  std::sort(V.begin(), V.end());
  V.erase(std::unique(V.begin(), V.end()), V.end());
}

Values readRegister(const State &S, const NdVar &V) {
  if (!V.Size || V.Size > sizeof(uint64_t))
    return {};
  if (V.isConst() && V.Provenance != ConstantAddressProvenance::AddressFragment)
    return {V.Offset & widthMask(V.Size)};
  if (!V.isReg() || V.Offset > UINT64_MAX - V.Size)
    return {};
  auto I = S.upper_bound({V.Offset, UINT16_MAX});
  if (I == S.begin())
    return {};
  --I;
  const auto [Offset, Size] = I->first;
  if (Offset > V.Offset || V.Offset - Offset + V.Size > Size)
    return {};
  Values Result = I->second;
  for (auto &Bits : Result)
    Bits = (Bits >> ((V.Offset - Offset) * 8)) & widthMask(V.Size);
  canonicalize(Result);
  return Result;
}
} // namespace

namespace {
std::optional<LowScalarOperandValues>
finiteOperandValues(llvm::ArrayRef<LowBlock> Blocks,
                    llvm::ArrayRef<uint8_t> Roots,
                    const LowScalarOperandQueries &Queries, size_t &Remaining) {
  const auto Spend = [&](size_t N) {
    if (N > Remaining) {
      Remaining = 0;
      return false;
    }
    Remaining -= N;
    return true;
  };
  const size_t MaxValues = limits::kMaxScalarValueSet;
  std::map<int, size_t> Ids;
  for (size_t B = 0; B != Blocks.size(); ++B) {
    if (!Spend(1) || !Ids.emplace(Blocks[B].Id, B).second)
      return {};
  }

  std::vector<std::optional<State>> Entries(Blocks.size());
  std::vector<bool> Queued(Blocks.size());
  std::deque<size_t> Work;
  const auto Queue = [&](size_t B) {
    if (!Queued[B]) {
      Queued[B] = true;
      Work.push_back(B);
    }
  };
  for (size_t B = 0; B != Blocks.size(); ++B) {
    if (Roots[B]) {
      Entries[B] = State{};
      Queue(B);
    }
  }
  LowScalarOperandValues ValuesAt;
  while (!Work.empty()) {
    const size_t B = Work.front();
    Work.pop_front();
    Queued[B] = false;
    if (!Spend(1 + Entries[B]->size()))
      return {};
    State Registers = *Entries[B], Temps;
    va_t Instruction = InvalidVA;
    const auto Read = [&](const NdVar &V) {
      if (!V.isTemp())
        return readRegister(Registers, V);
      const auto I = Temps.find({V.Offset, V.Size});
      return I == Temps.end() ? Values{} : I->second;
    };
    for (const LowOp &Op : Blocks[B].Ops) {
      if (!Spend(1))
        return {};
      if (Instruction != Op.Addr) {
        Temps.clear();
        Instruction = Op.Addr;
      }
      if (const auto At = Queries.find(&Op); At != Queries.end())
        for (unsigned Input : At->second) {
          if (!Spend(1) || Input >= Op.NumInputs)
            return {};
          auto Value = Read(Op.Inputs[Input]);
          if (Value.empty())
            ValuesAt.erase({&Op, Input});
          else
            ValuesAt[{&Op, Input}] = std::move(Value);
        }
      if (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL ||
          Op.Opcode == NdOp::INTRINSIC || Op.Opcode == NdOp::ATOMIC_XCHG ||
          Op.Opcode == NdOp::ATOMIC_ADD || Op.Opcode == NdOp::ATOMIC_CMPXCHG) {
        Registers.clear();
        Temps.clear();
        continue;
      }
      const auto &Out = Op.Output;
      if (!Out.Size || (!Out.isReg() && !Out.isTemp()))
        continue;
      Values Result;
      if (Out.Size <= sizeof(uint64_t) && Op.NumInputs <= 2 &&
          Op.MemoryOrdering == NdMemoryOrdering::None &&
          Op.MemoryAddressSpace == NdMemoryAddressSpace::Default) {
        const auto A = Op.NumInputs ? Read(Op.Inputs[0]) : Values{};
        const auto C = Op.NumInputs == 2 ? Read(Op.Inputs[1]) : Values{};
        // AND with a small scalar mask bounds an otherwise unknown value.
        // Enumerate *all* submasks, including values whose target overlaps a
        // different instruction stream. Never filter a value by plausibility.
        if (Op.Opcode == NdOp::INT_AND && Op.NumInputs == 2 &&
            Op.Inputs[0].Size == Out.Size && Op.Inputs[1].Size == Out.Size &&
            (A.empty() || C.empty())) {
          const Values &Masks = A.empty() ? C : A;
          if (Masks.size() == 1 &&
              std::popcount(Masks.front()) <= std::countr_zero(MaxValues)) {
            const uint64_t Mask = Masks.front();
            uint64_t V = Mask;
            do {
              Result.push_back(V);
              V = (V - 1) & Mask;
            } while (V != Mask);
          }
        } else if (!A.empty() && (Op.NumInputs == 1 || !C.empty()) &&
                   A.size() * std::max(size_t{1}, C.size()) <=
                       limits::kMaxScalarValueProduct) {
          const Values Right = C.empty() ? Values{0} : C;
          for (uint64_t L : A)
            for (uint64_t R : Right) {
              if (!Spend(1))
                return {};
              // Keep the scalar evaluator as the sole width/operation owner.
              // Substitute independent constants rather than comparing operand
              // identities in the callback (both operands can be the same).
              LowOp Constant = Op;
              Constant.Inputs[0] = NdVar::cst(L, Op.Inputs[0].Size);
              if (Op.NumInputs == 2)
                Constant.Inputs[1] = NdVar::cst(R, Op.Inputs[1].Size);
              auto Value =
                  lowScalarConstantResult(Constant, [](const NdVar &V) {
                    return V.isConst() ? std::optional<uint64_t>{V.Offset}
                                       : std::nullopt;
                  });
              if (!Value) {
                Result.clear();
                goto Unresolved;
              }
              Result.push_back(*Value);
            }
        }
      }
    Unresolved:
      canonicalize(Result);
      if (Result.size() > MaxValues)
        Result.clear();
      if (!Spend(1 + (Out.isTemp() ? Temps.size() : Registers.size()) +
                 Result.size()))
        return {};
      State &Written = Out.isTemp() ? Temps : Registers;
      if (Out.Offset > UINT64_MAX - Out.Size) {
        Written.clear();
        continue;
      }
      // Temporary byte intervals overlap just like register intervals. A
      // partial write cannot leave a stale full-width scalar fact behind.
      for (auto I = Written.begin(); I != Written.end();)
        if (I->first.first < Out.Offset + Out.Size &&
            Out.Offset < I->first.first + I->first.second)
          I = Written.erase(I);
        else
          ++I;
      if (!Result.empty())
        Written[{Out.Offset, Out.Size}] = std::move(Result);
    }
    for (int Succ : Blocks[B].Succs) {
      const auto Found = Ids.find(Succ);
      if (!Spend(1 + Registers.size()) || Found == Ids.end())
        return {};
      auto &Entry = Entries[Found->second];
      if (!Entry) {
        Entry = Registers;
        Queue(Found->second);
        continue;
      }
      State Joined;
      for (const auto &[Key, V] : *Entry) {
        // Meet exact slices symmetrically. A narrow incoming fact must not
        // produce a different join merely because it was visited first.
        const auto At = Registers.find(Key);
        if (At == Registers.end())
          continue;
        Values Other = At->second;
        if (!Spend(V.size() + Other.size()))
          return {};
        Other.insert(Other.end(), V.begin(), V.end());
        canonicalize(Other);
        if (Other.size() <= MaxValues)
          Joined.emplace(Key, std::move(Other));
      }
      if (Joined != *Entry) {
        Entry = std::move(Joined);
        Queue(Found->second);
      }
    }
  }
  return ValuesAt;
}
} // namespace

std::optional<LowScalarOperandValues>
finiteScalarOperandValues(CallRegisterBodyView Function,
                          const LowScalarOperandQueries &Queries,
                          size_t &Remaining) {
  if (!Function.Complete || Function.UnsafeIndirectBranches ||
      Function.HasIndependentRoots)
    return {};
  std::vector<uint8_t> Roots(Function.Blocks.size());
  for (size_t B = 0; B != Function.Blocks.size(); ++B)
    Roots[B] = B == 0 || Function.Blocks[B].Preds.empty() ||
               !Function.Blocks[B].ExceptionalPreds.empty();
  return finiteOperandValues(Function.Blocks, Roots, Queries, Remaining);
}

std::optional<std::map<va_t, std::vector<va_t>>>
finiteScalarBranchTargets(const LowFunc &Function, size_t &Remaining) {
  std::vector<uint8_t> Roots(Function.Blocks.size());
  std::set<va_t> BranchOccurrences;
  LowScalarOperandQueries Queries;
  for (size_t B = 0; B != Function.Blocks.size(); ++B) {
    const auto &Block = Function.Blocks[B];
    if (!Remaining)
      return {};
    --Remaining;
    Roots[B] = Block.StartAddr == Function.Entry || Block.Preds.empty() ||
               !Block.ExceptionalPreds.empty() ||
               Function.ModuleAnalysisRoots.count(Block.StartAddr) ||
               Function.OrdinaryModuleAnalysisRoots.count(Block.StartAddr);
    for (const LowOp &Op : Block.Ops) {
      if (!Remaining)
        return {};
      --Remaining;
      // An x87 normalization can clone a branch into state-specific blocks.
      // Such occurrences require their own joint target owner.
      if (Op.Opcode == NdOp::INDIR_BR) {
        if (!BranchOccurrences.insert(Op.Addr).second)
          return {};
        if (Op.NumInputs == 1)
          Queries.emplace(&Op, std::vector<unsigned>{0});
      }
    }
  }
  const auto Values =
      finiteOperandValues(Function.Blocks, Roots, Queries, Remaining);
  if (!Values)
    return {};
  std::map<va_t, std::vector<va_t>> Targets;
  for (const auto &[Query, Cover] : *Values)
    Targets.emplace(Query.first->Addr, Cover);
  return Targets;
}

} // namespace neverd
