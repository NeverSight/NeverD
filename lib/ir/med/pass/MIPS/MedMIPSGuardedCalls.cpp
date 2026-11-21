//===- MedMIPSGuardedCalls.cpp - Equality-guarded native arguments --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "MedMIPSGuardedCalls.h"

#include "../../../../safety/StackSlotFlow.h"
#include "../../abi/MedABIPassDetail.h"

#include "neverd/lift/MIPS/MIPSRegs.h"

#include "llvm/ADT/SetVector.h"

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
  const MedVar *Result = nullptr;
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

using OriginPairs =
    llvm::SmallDenseMap<std::pair<const MedVar *, const MedVar *>,
                        const MedVar *, 4>;
/// Keep an identity or a safety property only when all incoming paths agree.
/// Different checked words may each be safe at the same argument position.
RegisterWord intersect(RegisterWord A, RegisterWord B, const MedBlock &Join,
                       MedDefSiteIndex &Definitions, OriginPairs &Pairs) {
  if (!A.Name || A.Name != B.Name)
    return {};
  Word V;
  if (A.Value.Result == B.Value.Result)
    V.Result = A.Value.Result;
  else if (A.Value.Result && B.Value.Result) {
    // Two PHIs carrying the same pair of incoming words select equal bits.
    // Keep a common representative while this is true on every predecessor.
    const auto D = Definitions.find(*A.Name);
    if (!D.Ambiguous && D.Block == &Join && D.Phi && A.Name == &D.Phi->Output)
      V.Result = Pairs.try_emplace({A.Value.Result, B.Value.Result}, A.Name)
                     .first->second;
  }
  if (A.Value.Proof == B.Value.Proof)
    V.Proof = A.Value.Proof;
  else if (A.Value.Proof != Evidence::None && B.Value.Proof != Evidence::None)
    V.Proof = Evidence::Equal;
  return bind(*A.Name, V);
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

GuardedCalls
guardedResultArguments(const MedFunc &F, const NativeCallSites &Native,
                       const TargetRegInfo &TRI, BinaryFormat Format,
                       const safety::detail::ExactPhiGraph &Graph,
                       MedDefSiteIndex &Definitions, size_t &Remaining,
                       llvm::function_ref<bool(const MedVar &)> IsNonFrame) {
  if (F.Blocks.empty() || !F.Blocks.front().isFunctionEntry(F.Entry))
    return GuardedCalls{};
  GuardedCalls Proven;
  const auto Arguments = TRI.integerArgumentLayout(Format);
  struct BlockInput {
    Registers Words{};
    llvm::SmallVector<std::pair<int, Registers>, 0> Edges;
    bool Visited = false;
  };
  llvm::DenseMap<const MedBlock *, BlockInput> Incoming;
  llvm::SmallSetVector<const MedBlock *, 8> Work;
  Incoming.try_emplace(&F.Blocks.front());
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
    auto Regs = Input.Words;
    llvm::SmallDenseMap<std::pair<int, int>, Word, 8> Temps;
    auto read = [&](const MedVar &V) -> Word {
      if (V.Size != TRI.FullRegWidth)
        return {};
      if (V.isConst())
        return {nullptr, Evidence::Independent};
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
      if (V.Kind == MedVar::Temp) {
        if (V.Size == TRI.FullRegWidth && Value != Word{})
          Temps[{V.Id, V.SSAVer}] = Value;
        else if (V.Size)
          Temps.erase({V.Id, V.SSAVer});
      } else {
        writeRegister(Regs, V, Value);
      }
    };
    const auto Edge = equalityEdge(*B, Graph, Definitions);
    const MedVar *Checked = nullptr;
    llvm::ArrayRef<size_t> Sites;
    if (const auto It = Native.find(B); It != Native.end())
      Sites = It->second;
    bool AtEntry = B->Preds.empty() && B->isFunctionEntry(F.Entry);
    for (const MedOp &Op : B->Ops) {
      if (&Op == Edge.Compare) {
        const auto L = read(Op.Inputs[0]), R = read(Op.Inputs[1]);
        if (L.Result && R.Proof == Evidence::Independent)
          Checked = L.Result;
        else if (R.Result && L.Proof == Evidence::Independent)
          Checked = R.Result;
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
            Proven[&Op] = std::move(Fact);
          else
            Proven.erase(&Op);
        }
        // Raw SSA precedes ABI clobber modeling. Only preserved registers
        // keep their identities; rechecking one after the call is required.
        for (unsigned I = 0; I < Regs.size(); ++I) {
          auto &R = Regs[I];
          if (!R.Name)
            continue;
          if (!TRI.isCallPreserved(mipsreg::r(I), TRI.FullRegWidth) ||
              R.Value.Result == &Op.Output)
            R = {};
          else if (R.Value.Proof == Evidence::Equal) {
            R.Value.Proof = Evidence::None;
            R = bind(*R.Name, R.Value);
          }
        }
        Checked = nullptr;
        Temps.clear();
        AtEntry = false;
        if (Untyped && fullGPR(Op.Output) &&
            Op.Output.RegOff == TRI.IntReturnReg)
          Value.Result = &Op.Output;
      } else {
        if (const auto *Source = copiedWord(Op))
          Value = read(*Source);
        if (Value == Word{} && AtEntry && fullGPR(Op.Output) &&
            IsNonFrame(Op.Output))
          Value.Proof = Evidence::Independent;
      }
      write(Op.Output, Value);
      for (const auto &Output : Op.IntrinsicOutputs)
        write(Output, {});
    }
    for (int Id : B->Succs) {
      const auto *Succ = Graph.blockById(Id);
      if (!Succ || Remaining < Regs.size())
        return GuardedCalls{};
      Remaining -= Regs.size();
      auto Out = Regs;
      for (auto &R : Out) {
        if (!R.Value.Result)
          continue;
        if (R.Value.Result == Checked && Succ == Edge.Successor)
          R.Value.Proof = Evidence::Equal;
        if (!Succ->Phis.empty()) {
          // Re-entering a PHI selects a new word. Old aliases may retain a
          // safety proof, but cannot identify this execution's PHI result.
          const auto D = Definitions.find(*R.Value.Result);
          if (D.Phi && D.Block == Succ) {
            R.Value.Result = nullptr;
            R = bind(*R.Name, R.Value);
          }
        }
      }
      for (const auto &Phi : Succ->Phis) {
        if (Phi.Args.size() + 1 > Remaining)
          return GuardedCalls{};
        Remaining -= Phi.Args.size() + 1;
        enterPhi(Out, Phi, B->Id);
      }
      // The external entry contributes unknown words on every invocation.
      if (Succ == &F.Blocks.front())
        continue;
      auto [It, Inserted] = Incoming.try_emplace(Succ);
      auto &Next = It->second;
      if (Succ->Preds.size() > 1) {
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
        if (Edges.size() > Remaining / Out.size())
          return GuardedCalls{};
        Remaining -= Edges.size() * Out.size();
        Out = Edges.front().second;
        for (const auto &E : llvm::drop_begin(Edges)) {
          OriginPairs Pairs;
          for (unsigned I = 0; I < Out.size(); ++I)
            Out[I] = intersect(Out[I], E.second[I], *Succ, Definitions, Pairs);
        }
      }
      if (Inserted || Next.Words != Out) {
        Next.Words = Out;
        Work.insert(Succ);
      }
    }
  }
  // A disconnected predecessor cannot contribute evidence to a merge.
  if (Incoming.size() != F.Blocks.size())
    return GuardedCalls{};
  return Proven;
}

} // namespace neverd::mips
