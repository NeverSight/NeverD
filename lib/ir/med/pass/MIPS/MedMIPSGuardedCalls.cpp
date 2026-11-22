//===- MedMIPSGuardedCalls.cpp - Equality-guarded native arguments --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "MedMIPSGuardedCalls.h"

#include "../../../../safety/StackSlotFlow.h"
#include "../../abi/MedABIPassDetail.h"

#include "neverd/ArchSupport.h"
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
// Identity 0 is unknown. Reserve one identity for the all-zero pattern
// so a shift and an OR can reconstruct the two exact 32-bit halves.
constexpr unsigned ZeroId = 1;
constexpr int64_t WordBytes = mipsreg::SlotBytes;
constexpr int64_t HalfBytes = WordBytes / 2;

bool highMemoryHalf(int64_t Displacement, bool Little) {
  return Displacement == (Little ? HalfBytes : 0);
}

struct Half {
  unsigned Id = 0;
  // As the low half of a checked GPR, these bits qualified this high half.
  // Remember the order even when a move or a spill separates the halves.
  unsigned CheckedHigh = 0;
  bool operator==(const Half &) const = default;
};

struct Word {
  Half Low, High;
  Evidence Proof = Evidence::None;
  bool operator==(const Word &) const = default;

  bool complete() const { return Low.Id && High.Id; }
  Evidence evidence() const {
    if (Proof == Evidence::None && Low.CheckedHigh &&
        Low.CheckedHigh == High.Id)
      return Evidence::Equal;
    return Proof;
  }
  bool check(const Word &W) {
    for (auto *H : {&Low, &High})
      if (H->Id == W.Low.Id)
        H->CheckedHigh = W.High.Id;
    if (Low.Id != W.Low.Id || High.Id != W.High.Id)
      return false;
    Proof = Evidence::Equal;
    return true;
  }
  void afterCall() {
    if (Proof == Evidence::Equal)
      Proof = Evidence::None;
    Low.CheckedHigh = High.CheckedHigh = 0;
  }
};

/// Only transfer exact 32-bit patterns. Sign extension leaves the upper
/// pattern unknown; a subsequent left shift by 32 discards it.
Word transferredHalves(const MedOp &Op,
                       llvm::function_ref<Word(const MedVar &)> Read) {
  if (Op.NumInputs == 1 && Op.Inputs[0].Size == HalfBytes &&
      Op.Output.Size == WordBytes &&
      (Op.Opcode == NdOp::INT_ZEXT || Op.Opcode == NdOp::INT_SEXT))
    return {Read(Op.Inputs[0]).Low, {Op.Opcode == NdOp::INT_ZEXT ? ZeroId : 0}};
  if (Op.NumInputs != 2)
    return {};
  const auto &A = Op.Inputs[0], &B = Op.Inputs[1];
  if (Op.Opcode == NdOp::SUBBYTES && A.Size == WordBytes && B.isConst() &&
      Op.Output.Size == HalfBytes &&
      (B.ConstVal == 0 || B.ConstVal == HalfBytes)) {
    const auto W = Read(A);
    return {B.ConstVal == 0 ? W.Low : W.High, {}};
  }
  if (Op.Output.Size != WordBytes)
    return {};
  if (Op.Opcode == NdOp::CONCAT && A.Size == HalfBytes && B.Size == HalfBytes)
    return {Read(B).Low, Read(A).Low};
  if (A.Size != WordBytes || B.Size != WordBytes)
    return {};
  if (Op.Opcode == NdOp::INT_LEFT || Op.Opcode == NdOp::INT_RIGHT ||
      Op.Opcode == NdOp::INT_ASHR) {
    if (!B.isConst())
      return {};
    if (B.ConstVal == 0)
      return Read(A);
    if (B.ConstVal == 32) {
      if (Op.Opcode == NdOp::INT_LEFT)
        return {{ZeroId}, Read(A).Low};
      return {Read(A).High, {Op.Opcode == NdOp::INT_RIGHT ? ZeroId : 0}};
    }
  }
  if (Op.Opcode == NdOp::INT_AND && (A.isConst() || B.isConst())) {
    const auto Mask = A.isConst() ? A.ConstVal : B.ConstVal;
    const auto W = Read(A.isConst() ? B : A);
    const auto Apply = [](Half H, uint32_t Bits) -> Half {
      if (Bits == UINT32_MAX)
        return H;
      return {Bits == 0 ? ZeroId : 0};
    };
    return {Apply(W.Low, uint32_t(Mask)), Apply(W.High, uint32_t(Mask >> 32))};
  }
  if (Op.Opcode == NdOp::INT_OR || Op.Opcode == NdOp::INT_XOR ||
      Op.Opcode == NdOp::INT_ADD) {
    const auto L = Read(A), R = Read(B);
    // A carry out of overlapping low bits may change the entire high half.
    if (Op.Opcode == NdOp::INT_ADD && L.Low.Id != ZeroId && R.Low.Id != ZeroId)
      return {};
    const auto Combine = [](Half L, Half R) -> Half {
      if (L.Id == ZeroId)
        return R;
      return R.Id == ZeroId ? L : Half{};
    };
    return {Combine(L.Low, R.Low), Combine(L.High, R.High)};
  }
  return {};
}

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
  // A checked stack argument is safe as a whole. Keep track of the opaque
  // halves still present in case a later store changes the other bits.
  unsigned OpaqueHalves = 0;
  bool Unchecked = false;
  bool operator==(const Spill &) const = default;

  void forget() {
    Value = {};
    Unchecked = OpaqueHalves != 0;
  }

  bool store(int64_t Offset, uint16_t Bytes, Word W, bool Little,
             unsigned &NextIdentity) {
    if (!Offset && Bytes == WordBytes) {
      Value = W;
      // A join may keep a whole-word guard but lose both individual
      // identities (one arm supplied an independent value).
      const auto Proof = W.evidence();
      const auto OpaqueHalf = [&](Half H) {
        return H.Id > ZeroId || (!H.Id && Proof == Evidence::Equal);
      };
      OpaqueHalves =
          unsigned(OpaqueHalf(W.Low)) | (unsigned(OpaqueHalf(W.High)) << 1);
      Unchecked = OpaqueHalves && Proof == Evidence::None;
      return W.complete() || Proof == Evidence::Equal;
    }
    bool Tracked = false;
    // Replacing part of a checked word can expose a different pointer.
    // Retain every outstanding half until its last opaque bits are replaced.
    const auto PreviousOpaqueHalves = OpaqueHalves;
    Value.Proof = Evidence::None;
    for (int64_t At : {int64_t{0}, HalfBytes}) {
      if (!safety::detail::stackRangesOverlap(At, HalfBytes, Offset, Bytes))
        continue;
      const bool High = highMemoryHalf(At, Little);
      const unsigned Mask = High ? 2 : 1;
      auto &Part = High ? Value.High : Value.Low;
      Half H;
      if (At == Offset && Bytes == HalfBytes) {
        H = W.Low;
        Tracked = H.Id > ZeroId;
      }
      if (Offset <= At && Offset + Bytes >= At + HalfBytes)
        OpaqueHalves &= ~Mask;
      if (H.Id > ZeroId)
        OpaqueHalves |= Mask;
      // A sample after a narrow write has fresh bits. Earlier samples
      // keep their identities, including the half that was untouched.
      if (!H.Id && Part.Id && PreviousOpaqueHalves && Bytes <= HalfBytes)
        H.Id = NextIdentity++;
      Part = H;
    }
    Unchecked = OpaqueHalves && Value.evidence() != Evidence::Equal;
    return Tracked;
  }

  bool exposedAtCall(bool Private) {
    if (Private) {
      Value.afterCall();
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

/// An identity names equal bits within one execution state. At a join two
/// locations remain equal exactly when their incoming identities agree on
/// each edge. Renumbering by location order gives loops a stable fixed point
/// without identifying successive executions of the same call instruction.
void intersect(State &A, const State &B) {
  llvm::SmallDenseMap<std::pair<unsigned, unsigned>, unsigned, 8> Classes;
  const auto MeetBits = [&](unsigned L, unsigned R) -> unsigned {
    if (!L || !R)
      return 0;
    if (L == ZeroId && R == ZeroId)
      return ZeroId;
    return Classes.try_emplace({L, R}, Classes.size() + ZeroId + 1)
        .first->second;
  };
  const auto MeetHalf = [&](Half L, Half R) -> Half {
    return {MeetBits(L.Id, R.Id), MeetBits(L.CheckedHigh, R.CheckedHigh)};
  };
  const auto Meet = [&](Word L, Word R) {
    Word V{MeetHalf(L.Low, R.Low), MeetHalf(L.High, R.High)};
    if (L.evidence() == R.evidence())
      V.Proof = L.evidence();
    else if (L.evidence() != Evidence::None && R.evidence() != Evidence::None)
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
    A.Slots[I].OpaqueHalves |= B.Slots[I].OpaqueHalves;
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
  // Index aligned frame cells used for a 64- or 32-bit transfer. Even a store
  // with no later load may escape as a stack argument.
  const bool Little = archLittleEndian(TRI.TheArch);
  llvm::SmallVector<int64_t, 8> Offsets;
  for (const auto &B : F.Blocks) {
    if (B.Ops.size() > Remaining)
      return {};
    Remaining -= B.Ops.size();
    for (const auto &Op : B.Ops) {
      if (safety::detail::isStackMemoryWrite(Op.Opcode) &&
          (Op.Opcode != NdOp::STORE || Op.NumInputs != 2 || !Op.Inputs[1].Size))
        return {};
      if (Op.Opcode != NdOp::STORE &&
          (Op.Opcode != NdOp::LOAD || Op.NumInputs != 1))
        continue;
      const auto Bytes =
          Op.Opcode == NdOp::STORE ? Op.Inputs[1].Size : Op.Output.Size;
      if (Bytes == WordBytes || Bytes == HalfBytes)
        if (const auto At = Stack.Offset(Op.Inputs[0]);
            At && *At >= -limits::ir::kSourceFrameAnalysisMaxFrame &&
            *At <= -Bytes && *At % Bytes == 0)
          Offsets.push_back(*At & -WordBytes);
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
    // A normalized location has at most four distinct identities (the two
    // halves and their guard partners). Dynamic call executions start fresh.
    unsigned NextIdentity = 4 * Locations + ZeroId + 1;
    llvm::SmallDenseMap<std::pair<int, int>, Word, 8> Temps;
    auto read = [&](const MedVar &V) -> Word {
      if (V.Size != WordBytes && V.Size != HalfBytes)
        return {};
      if (V.isConst())
        return {{uint32_t(V.ConstVal) == 0 ? ZeroId : 0},
                {V.Size == WordBytes && (V.ConstVal >> 32) == 0 ? ZeroId : 0},
                V.Size == WordBytes ? Evidence::Independent : Evidence::None};
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
          S.forget();
      }
      if (V.Kind == MedVar::Temp) {
        if ((V.Size == WordBytes || V.Size == HalfBytes) && Value != Word{})
          Temps[{V.Id, V.SSAVer}] = Value;
        else if (V.Size)
          Temps.erase({V.Id, V.SSAVer});
      } else {
        writeRegister(Regs, V, Value);
      }
      return true;
    };
    const auto Edge = equalityEdge(*B, Graph, Definitions);
    Word Checked;
    llvm::ArrayRef<size_t> Sites;
    if (const auto It = Native.find(B); It != Native.end())
      Sites = It->second;
    bool AtEntry = B->Preds.empty() && B->isFunctionEntry(F.Entry);
    for (const MedOp &Op : B->Ops) {
      if (&Op == Edge.Compare && Op.Inputs[0].Size == WordBytes &&
          Op.Inputs[1].Size == WordBytes) {
        const auto L = read(Op.Inputs[0]), R = read(Op.Inputs[1]);
        if (L.complete() && R.Proof == Evidence::Independent)
          Checked = L;
        else if (R.complete() && L.Proof == Evidence::Independent)
          Checked = R;
      }
      Word Value;
      if (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL) {
        const bool Untyped = !Sites.empty() && &Op == &B->Ops[Sites.front()];
        if (Untyped) {
          Sites = Sites.drop_front();
          GuardedCall Fact;
          for (unsigned I = 0; I < Arguments.Registers.size(); ++I)
            if (Regs[Arguments.Registers[I] / mipsreg::SlotBytes]
                    .Value.evidence() == Evidence::Equal)
              Fact.Registers |= uint32_t{1} << I;
          for (unsigned I = 1; I < Op.NumInputs; ++I)
            if (read(Op.Inputs[I]).evidence() == Evidence::Equal)
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
          else {
            R.Value.afterCall();
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
        Checked = {};
        Temps.clear();
        AtEntry = false;
        if (Untyped && fullGPR(Op.Output) &&
            Op.Output.RegOff == TRI.IntReturnReg)
          Value = {{NextIdentity++}, {NextIdentity++}};
      } else {
        if (const auto *Source = copiedWord(Op)) {
          Value = read(*Source);
        } else if (Op.Opcode == NdOp::LOAD && Op.NumInputs == 1 &&
                   (Op.Output.Size == WordBytes ||
                    Op.Output.Size == HalfBytes)) {
          const auto At = Stack.Offset(Op.Inputs[0]);
          if (At && SP && *At >= *SP && *At % Op.Output.Size == 0) {
            const auto Base = *At & -WordBytes;
            const auto It = llvm::lower_bound(Offsets, Base);
            if (It != Offsets.end() && *It == Base) {
              const auto &W = Current.Slots[It - Offsets.begin()].Value;
              Value = Op.Output.Size == WordBytes
                          ? W
                          : Word{highMemoryHalf(*At - Base, Little) ? W.High
                                                                    : W.Low,
                                 {}};
            }
          }
        } else if (Op.Opcode == NdOp::STORE && Op.NumInputs == 2) {
          Proven.Spills.erase(&Op);
          const auto At = Stack.Offset(Op.Inputs[0]);
          const auto &Stored = Op.Inputs[1];
          if (At) {
            auto It = llvm::lower_bound(Offsets, *At & -WordBytes);
            for (; It != Offsets.end() && safety::detail::stackRangesOverlap(
                                              *It, WordBytes, *At, Stored.Size);
                 ++It) {
              if (!Remaining)
                return {};
              --Remaining;
              auto &S = Current.Slots[It - Offsets.begin()];
              if (!SP || *It < *SP) {
                S.forget();
                continue;
              }
              if (S.store(*At - *It, Stored.Size, read(Stored), Little,
                          NextIdentity))
                Proven.Spills.insert(&Op);
            }
          } else if (!Offsets.empty() && !IsNonFrame(Op.Inputs[0])) {
            if (Offsets.size() > Remaining)
              return {};
            Remaining -= Offsets.size();
            for (auto &S : Current.Slots)
              S.forget();
          }
        } else if (Op.Output.Size)
          Value = transferredHalves(Op, read);
        if (Value.Proof == Evidence::None && AtEntry && fullGPR(Op.Output) &&
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
      if (Checked.complete() && Succ == Edge.Successor) {
        for (auto &R : Out.Regs)
          R.Value.check(Checked);
        for (auto &S : Out.Slots)
          if (S.Value.check(Checked))
            S.Unchecked = false;
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
