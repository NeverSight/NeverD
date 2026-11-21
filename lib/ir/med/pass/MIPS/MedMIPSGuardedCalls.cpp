//===- MedMIPSGuardedCalls.cpp - Equality-guarded native arguments --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "MedMIPSGuardedCalls.h"

#include "../../../../safety/StackSlotFlow.h"
#include "../../abi/MedABIPassDetail.h"

#include "neverd/ir/Limits.h"
#include "neverd/lift/MIPS/MIPSRegs.h"

#include "llvm/ADT/SetVector.h"

#include <algorithm>
#include <array>

namespace neverd::mips {

const MedVar *copiedWord(const MedOp &Op) {
  const MedVar *Input = nullptr;
  if (Op.Opcode == NdOp::COPY && Op.NumInputs == 1)
    Input = &Op.Inputs[0];
  else if ((Op.Opcode == NdOp::INT_ADD || Op.Opcode == NdOp::INT_OR ||
            Op.Opcode == NdOp::INT_XOR) &&
           Op.NumInputs == 2) {
    for (unsigned I : {0u, 1u})
      if (Op.Inputs[I].isConst() && Op.Inputs[I].ConstVal == 0) {
        Input = &Op.Inputs[1 - I];
        break;
      }
  }
  return Input && Input->Size == Op.Output.Size ? Input : nullptr;
}

namespace {
enum class Evidence { None, Independent, Equal };
struct Word {
  unsigned Identity = 0;
  Evidence Proof = Evidence::None;
  bool operator==(const Word &) const = default;
};

struct RegisterWord {
  const MedVar *Name = nullptr;
  Word Value;
  bool operator==(const RegisterWord &) const = default;
};
using Registers = std::array<RegisterWord, 32>;

bool fullGPR(const MedVar &V) {
  return V.Kind == MedVar::Reg && V.RegOff < mipsreg::r(32) &&
         V.RegOff % mipsreg::SlotBytes == 0 && V.Size == mipsreg::SlotBytes;
}

RegisterWord bind(const MedVar &Name, Word Value) {
  return Value == Word{} ? RegisterWord{} : RegisterWord{&Name, Value};
}

void writeRegister(Registers &Regs, const MedVar &V, Word Value) {
  if (V.Kind != MedVar::Reg || !V.Size || V.RegOff >= mipsreg::r(32))
    return;
  const unsigned First = V.RegOff / mipsreg::SlotBytes;
  if (fullGPR(V)) {
    Regs[First] = bind(V, Value);
    return;
  }
  for (unsigned I = First; I < Regs.size() && mipsreg::r(I) < V.RegOff + V.Size;
       ++I)
    Regs[I] = {};
}

struct Spill {
  Word Value;
  // The store can reach an open call through its outgoing stack arguments.
  bool Unchecked = false;
  bool operator==(const Spill &) const = default;

  bool store(Word W) {
    Value = W;
    Unchecked = W.Identity && W.Proof == Evidence::None;
    return W.Identity || W.Proof == Evidence::Equal;
  }

  // A call with an open or overlapping argument area can both observe and
  // change this word. A closed call outside the slot preserves its bits.
  bool exposedAtCall(bool Private) {
    if (Private) {
      if (Value.Proof == Evidence::Equal)
        Value.Proof = Evidence::None;
      return false;
    }
    const bool Exposed = Unchecked;
    *this = {};
    return Exposed;
  }
};
struct State {
  Registers Regs{};
  llvm::SmallVector<Spill, 0> Slots;
  bool operator==(const State &) const = default;
};

/// An identity names equal words within one execution state. At a join two
/// locations remain equal exactly when their incoming identities agree on
/// each edge. Renumbering by location order gives loops a stable fixed point
/// without identifying successive executions of the same call instruction.
void intersect(State &A, const State &B) {
  llvm::SmallDenseMap<std::pair<unsigned, unsigned>, unsigned, 8> Classes;
  const auto Meet = [&](Word L, Word R) {
    Word V;
    if (L.Identity && R.Identity)
      V.Identity =
          Classes.try_emplace({L.Identity, R.Identity}, Classes.size() + 1)
              .first->second;
    if (L.Proof == R.Proof)
      V.Proof = L.Proof;
    else if (L.Proof != Evidence::None && R.Proof != Evidence::None)
      V.Proof = Evidence::Equal;
    return V;
  };
  for (unsigned I = 0; I < A.Regs.size(); ++I) {
    auto &L = A.Regs[I];
    const auto &R = B.Regs[I];
    L = L.Name && L.Name == R.Name ? bind(*L.Name, Meet(L.Value, R.Value))
                                   : RegisterWord{};
  }
  for (size_t I = 0; I < A.Slots.size(); ++I) {
    A.Slots[I].Value = Meet(A.Slots[I].Value, B.Slots[I].Value);
    A.Slots[I].Unchecked |= B.Slots[I].Unchecked;
  }
}

/// Select the successor on which the comparison was equal. The instruction
/// in the delay slot may change a register after the condition was captured.
struct EqualityEdge {
  const MedOp *Compare = nullptr;
  const MedBlock *Successor = nullptr;
};
EqualityEdge equalityEdge(const MedBlock &Block,
                          const safety::detail::ExactPhiGraph &Graph,
                          MedDefSiteIndex &Definitions) {
  if (Block.Role != MedBlockRole::Machine || Block.Ops.empty() ||
      Block.Succs.size() != 2)
    return {};
  const auto &Branch = Block.Ops.back();
  if (Branch.Opcode != NdOp::COND_BR || Branch.NumInputs != 2 ||
      !Branch.Inputs[0].isConst() || Branch.Inputs[1].Size != 1)
    return {};
  const auto D = Definitions.find(Branch.Inputs[1]);
  if (D.Ambiguous || D.Block != &Block || !D.Op || D.Op->NumInputs != 2 ||
      D.Op->Output.Size != 1 || D.Op->Output != Branch.Inputs[1] ||
      (D.Op->Opcode != NdOp::INT_EQUAL && D.Op->Opcode != NdOp::INT_NOTEQUAL))
    return {};
  const auto *A = Graph.blockById(Block.Succs[0]);
  const auto *B = Graph.blockById(Block.Succs[1]);
  const auto Target = Branch.Inputs[0].ConstVal;
  if (!A || !B || A->Role != MedBlockRole::Machine ||
      B->Role != MedBlockRole::Machine ||
      (A->StartAddr == Target) == (B->StartAddr == Target))
    return {};
  return {D.Op, (A->StartAddr == Target) == (D.Op->Opcode == NdOp::INT_EQUAL)
                    ? A
                    : B};
}

/// Rename only a complete incoming physical value. A PHI is not a write
/// that can erase a partial register update or an unchecked incoming path.
void enterPhi(Registers &Out, const PhiNode &Phi, int Pred) {
  if (!fullGPR(Phi.Output)) {
    writeRegister(Out, Phi.Output, {});
    return;
  }
  auto &R = Out[Phi.Output.RegOff / mipsreg::SlotBytes];
  if (!R.Name)
    return;
  for (const auto &[Id, V] : Phi.Args)
    if (Id == Pred && safety::detail::sameRegisterOccurrence(*R.Name, V)) {
      R.Name = &Phi.Output;
      return;
    }
  R = {};
}

} // namespace

GuardedCalls guardedResultArguments(
    const MedFunc &F, const NativeCallSites &Native, const TargetRegInfo &TRI,
    BinaryFormat Format, const safety::detail::ExactPhiGraph &Graph,
    MedDefSiteIndex &Definitions, GuardedStackLayout Stack, size_t &Remaining,
    llvm::function_ref<bool(const MedVar &)> IsNonFrame) {
  if (F.Blocks.empty() || !F.Blocks.front().isFunctionEntry(F.Entry))
    return GuardedCalls{};
  GuardedCalls Proven;
  const auto Arguments = TRI.integerArgumentLayout(Format);
  // Only whole words that can actually be reloaded have a tracked slot.
  const int64_t Width = TRI.FullRegWidth;
  llvm::SmallVector<int64_t, 8> Offsets;
  for (const auto &B : F.Blocks) {
    if (B.Ops.size() > Remaining)
      return {};
    Remaining -= B.Ops.size();
    for (const auto &Op : B.Ops) {
      if (safety::detail::isStackMemoryWrite(Op.Opcode) &&
          (Op.Opcode != NdOp::STORE || Op.NumInputs != 2))
        return {};
      if (Op.Opcode == NdOp::LOAD && Op.NumInputs == 1 &&
          Op.Output.Size == Width)
        if (const auto At = Stack.Offset(Op.Inputs[0]);
            At && *At >= -limits::ir::kSourceFrameAnalysisMaxFrame &&
            *At <= -Width && *At % Width == 0)
          Offsets.push_back(*At);
    }
  }
  llvm::sort(Offsets);
  Offsets.erase(std::unique(Offsets.begin(), Offsets.end()), Offsets.end());
  State Entry;
  Entry.Slots.resize(Offsets.size());
  const size_t Locations = Entry.Regs.size() + Offsets.size();
  struct BlockInput {
    State Words;
    llvm::SmallVector<std::pair<int, State>, 0> Edges;
    bool Visited = false;
  };
  llvm::DenseMap<const MedBlock *, BlockInput> Incoming;
  llvm::SmallSetVector<const MedBlock *, 8> Work;
  Incoming[&F.Blocks.front()].Words = Entry;
  llvm::SmallPtrSet<const MedOp *, 4> Exposed;
  Work.insert(&F.Blocks.front());
  while (!Work.empty()) {
    const auto *B = Work.pop_back_val();
    const size_t Cost = 1 + B->Ops.size() + B->Preds.size() + B->Succs.size();
    if (Cost > Remaining)
      return GuardedCalls{};
    Remaining -= Cost;
    auto &Input = Incoming.at(B);
    if (!Input.Visited) {
      if (!Graph.hasCompleteNormalPredecessors(*B))
        return GuardedCalls{};
      for (const auto &Phi : B->Phis) {
        if (Phi.Args.size() + 1 > Remaining)
          return GuardedCalls{};
        Remaining -= Phi.Args.size() + 1;
        if (fullGPR(Phi.Output) &&
            !Graph.hasCompleteIncoming(B - F.Blocks.data(), Phi))
          return GuardedCalls{};
      }
      Input.Visited = true;
    }
    auto Current = Input.Words;
    auto &Regs = Current.Regs;
    auto SP = Stack.BlockEntrySP(B->Id);
    unsigned NextIdentity = Locations + 1;
    llvm::SmallDenseMap<std::pair<int, int>, Word, 8> Temps;
    auto read = [&](const MedVar &V) -> Word {
      if (V.Size != TRI.FullRegWidth)
        return {};
      if (V.isConst())
        return {0, Evidence::Independent};
      if (V.Kind == MedVar::Temp)
        return Temps.lookup({V.Id, V.SSAVer});
      if (fullGPR(V)) {
        const auto &R = Regs[V.RegOff / mipsreg::SlotBytes];
        if (R.Name && safety::detail::sameRegisterOccurrence(*R.Name, V))
          return R.Value;
      }
      return {};
    };
    auto write = [&](const MedVar &V, Word Value) {
      if (V.Kind == MedVar::Reg && V.RegOff == TRI.StackPointer) {
        SP = Stack.Offset(V);
        // An allocation cannot reacquire the identity of a released word.
        const auto End = SP ? llvm::lower_bound(Offsets, *SP) : Offsets.end();
        const size_t Count = End - Offsets.begin();
        if (Count > Remaining)
          return false;
        Remaining -= Count;
        for (auto &S : llvm::MutableArrayRef(Current.Slots).take_front(Count))
          S.Value = {};
      }
      if (V.Kind == MedVar::Temp) {
        if (V.Size == TRI.FullRegWidth && Value != Word{})
          Temps[{V.Id, V.SSAVer}] = Value;
        else if (V.Size)
          Temps.erase({V.Id, V.SSAVer});
      } else {
        writeRegister(Regs, V, Value);
      }
      return true;
    };
    const auto Edge = equalityEdge(*B, Graph, Definitions);
    unsigned Checked = 0;
    llvm::ArrayRef<size_t> Sites;
    if (const auto It = Native.find(B); It != Native.end())
      Sites = It->second;
    bool AtEntry = B->Preds.empty() && B->isFunctionEntry(F.Entry);
    for (const MedOp &Op : B->Ops) {
      if (&Op == Edge.Compare) {
        const auto L = read(Op.Inputs[0]), R = read(Op.Inputs[1]);
        if (L.Identity && R.Proof == Evidence::Independent)
          Checked = L.Identity;
        else if (R.Identity && L.Proof == Evidence::Independent)
          Checked = R.Identity;
      }
      Word Value;
      if (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL) {
        const bool Untyped = !Sites.empty() && &Op == &B->Ops[Sites.front()];
        if (Untyped) {
          Sites = Sites.drop_front();
          GuardedCall Fact;
          for (unsigned I = 0; I < Arguments.Registers.size(); ++I)
            if (Regs[Arguments.Registers[I] / mipsreg::SlotBytes].Value.Proof ==
                Evidence::Equal)
              Fact.Registers |= uint32_t{1} << I;
          for (unsigned I = 1; I < Op.NumInputs; ++I)
            if (read(Op.Inputs[I]).Proof == Evidence::Equal)
              Fact.Inputs.push_back(I);
          if (Fact.Registers || !Fact.Inputs.empty())
            Proven.Arguments[&Op] = std::move(Fact);
          else
            Proven.Arguments.erase(&Op);
        }
        // Raw SSA precedes ABI clobber modeling. Only preserved registers
        // keep their identities; rechecking one after the call is required.
        for (unsigned I = 0; I < Regs.size(); ++I) {
          auto &R = Regs[I];
          if (!R.Name)
            continue;
          if (!TRI.isCallPreserved(mipsreg::r(I), TRI.FullRegWidth))
            R = {};
          else if (R.Value.Proof == Evidence::Equal) {
            R.Value.Proof = Evidence::None;
            R = bind(*R.Name, R.Value);
          }
        }
        if (Offsets.size() > Remaining)
          return {};
        Remaining -= Offsets.size();
        const auto Private = Stack.ClosedPrivateStart(Op);
        bool Unsafe = false;
        for (size_t I = 0; I < Offsets.size(); ++I)
          Unsafe |=
              Current.Slots[I].exposedAtCall(Private && Offsets[I] >= *Private);
        if (Unsafe)
          Exposed.insert(&Op);
        else
          Exposed.erase(&Op);
        Checked = 0;
        Temps.clear();
        AtEntry = false;
        if (Untyped && fullGPR(Op.Output) &&
            Op.Output.RegOff == TRI.IntReturnReg)
          Value.Identity = NextIdentity++;
      } else {
        if (const auto *Source = copiedWord(Op)) {
          Value = read(*Source);
        } else if (Op.Opcode == NdOp::LOAD && Op.NumInputs == 1 &&
                   Op.Output.Size == Width) {
          const auto At = Stack.Offset(Op.Inputs[0]);
          if (At && SP && *At >= *SP) {
            const auto It = llvm::lower_bound(Offsets, *At);
            if (It != Offsets.end() && *It == *At)
              Value = Current.Slots[It - Offsets.begin()].Value;
          }
        } else if (Op.Opcode == NdOp::STORE && Op.NumInputs == 2) {
          Proven.Spills.erase(&Op);
          const auto At = Stack.Offset(Op.Inputs[0]);
          const auto &Stored = Op.Inputs[1];
          if (At) {
            auto It = llvm::lower_bound(Offsets, *At & -Width);
            for (; It != Offsets.end() && safety::detail::stackRangesOverlap(
                                              *It, Width, *At, Stored.Size);
                 ++It) {
              if (!Remaining)
                return {};
              --Remaining;
              auto &S = Current.Slots[It - Offsets.begin()];
              if (*At == *It && Stored.Size == Width && SP && *At >= *SP) {
                if (S.store(read(Stored)))
                  Proven.Spills.insert(&Op);
              } else if (S.Value.Identity && SP && *It >= *SP && Stored.Size &&
                         Stored.Size < Width) {
                // This changed word can itself be sampled and checked.
                // A register loaded before the write keeps its old identity.
                S.store({NextIdentity++, Evidence::None});
              } else {
                S.Value = {};
              }
            }
          } else if (!Offsets.empty() && !IsNonFrame(Op.Inputs[0])) {
            if (Offsets.size() > Remaining)
              return {};
            Remaining -= Offsets.size();
            for (auto &S : Current.Slots)
              S.Value = {};
          }
        }
        if (Value == Word{} && AtEntry && fullGPR(Op.Output) &&
            IsNonFrame(Op.Output))
          Value.Proof = Evidence::Independent;
      }
      if (!write(Op.Output, Value))
        return {};
      for (const auto &Output : Op.IntrinsicOutputs)
        if (!write(Output, {}))
          return {};
    }
    for (int Id : B->Succs) {
      const auto *Succ = Graph.blockById(Id);
      if (!Succ || Remaining < Locations)
        return {};
      Remaining -= Locations;
      auto Out = Current;
      if (Checked && Succ == Edge.Successor) {
        for (auto &R : Out.Regs)
          if (R.Value.Identity == Checked)
            R.Value.Proof = Evidence::Equal;
        for (auto &S : Out.Slots)
          if (S.Value.Identity == Checked) {
            S.Value.Proof = Evidence::Equal;
            S.Unchecked = false;
          }
      }
      for (const auto &Phi : Succ->Phis) {
        if (Phi.Args.size() + 1 > Remaining)
          return GuardedCalls{};
        Remaining -= Phi.Args.size() + 1;
        enterPhi(Out.Regs, Phi, B->Id);
      }
      // Also normalize a single edge so fresh call identities never collide
      // with incoming ones when this block is visited again.
      intersect(Out, Out);
      auto [It, Inserted] = Incoming.try_emplace(Succ);
      auto &Next = It->second;
      if (Succ->Preds.size() > 1 || Succ == &F.Blocks.front()) {
        // A predecessor may discover a new PHI identity on a later visit.
        // Replace its old contribution before intersecting the current edges.
        auto &Edges = Next.Edges;
        auto Pos = llvm::lower_bound(
            Edges, B->Id, [](const auto &E, int Id) { return E.first < Id; });
        if (Pos != Edges.end() && Pos->first == B->Id) {
          if (Pos->second == Out)
            continue;
          Pos->second = Out;
        } else {
          Edges.insert(Pos, {B->Id, Out});
        }
        if (Edges.size() > Remaining / Locations)
          return {};
        Remaining -= Edges.size() * Locations;
        Out = Edges.front().second;
        for (const auto &E : llvm::drop_begin(Edges))
          intersect(Out, E.second);
        if (Succ == &F.Blocks.front())
          intersect(Out, Entry);
      }
      if (Inserted || Next.Words != Out) {
        Next.Words = Out;
        Work.insert(Succ);
      }
    }
  }
  // A disconnected predecessor cannot contribute evidence to a merge.
  if (Incoming.size() != F.Blocks.size() || !Exposed.empty())
    return GuardedCalls{};
  return Proven;
}

} // namespace neverd::mips
