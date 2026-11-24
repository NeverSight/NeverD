//===- PipelineShiftedReturnPaths.cpp - Paired shift paths ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Follow an integer return and its shift count through the same ordinary CFG
/// edges. A repeated header is closed only for the same incoming bindings.
//===----------------------------------------------------------------------===//

#include "PipelineShiftedReturnPaths.h"

#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/ir/med/IntrinsicShapes.h"
#include "neverd/ir/med/MedConstantPropagation.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/Support/MathExtras.h"

#include <algorithm>
#include <optional>
#include <set>

namespace neverd::return_contract {

static bool isScalarShift(const MedOp &Op) {
  return (Op.Opcode == NdOp::INT_LEFT || Op.Opcode == NdOp::INT_RIGHT ||
          Op.Opcode == NdOp::INT_ASHR) &&
         scalarShiftShape(Op);
}

bool isVariableShift(const MedOp &Op) {
  return isScalarShift(Op) && !Op.Inputs[1].isConst();
}

uint64_t shiftedBits(NdOp Opcode, unsigned Width, uint64_t V, uint64_t Count) {
  if (Opcode == NdOp::INT_ASHR)
    return llvm::SignExtend64(V, Width) >> std::min<uint64_t>(Count, Width - 1);
  if (Count >= Width)
    return 0;
  return Opcode == NdOp::INT_LEFT ? V << Count : V >> Count;
}

namespace {

using Number = std::optional<uint64_t>;
using Binding = std::pair<const MedVar *, Number>;
using Frame = llvm::SmallVector<Binding, 6>;
using Locals = MedValueMap<Binding>;
using Views = llvm::SmallDenseMap<std::pair<uint64_t, unsigned>, unsigned, 8>;
using Outcomes = llvm::SmallVector<uint64_t, 4>;

bool sameView(const MedVar &A, const MedVar &B) {
  return A == B && A.Size == B.Size &&
         (A.Kind != MedVar::Reg || A.RegOff == B.RegOff);
}

bool scalarEquality(const MedOp &Op) {
  return (Op.Opcode == NdOp::INT_EQUAL || Op.Opcode == NdOp::INT_NOTEQUAL) &&
         Op.NumInputs == 2 && Op.Output.Size == 1 && Op.Inputs[0].Size &&
         Op.Inputs[0].Size <= 8 && Op.Inputs[0].Size == Op.Inputs[1].Size;
}

struct ShiftPaths {
  const MedFunc &Func;
  const MedBlock &Exit;
  const MedVar &Result;
  const TargetRegInfo &TRI;
  BinaryFormat Format;
  unsigned Bytes;
  unsigned &Work;
  Views Registers{};
  llvm::SmallPtrSet<const PhiNode *, 8> NeededPhis;
  llvm::SmallPtrSet<const MedBlock *, 8> CheckedJoins;

  bool spend(size_t Amount) {
    if (Amount > Work) {
      Work = 0;
      return false;
    }
    Work -= Amount;
    return true;
  }

  // Only register views feeding the returned expression live across an edge.
  // In particular a changing loop induction variable need not enter the key.
  bool prepare(const MedValueMap<const MedOp *> &Defs,
               const MedValueMap<PhiSite> &Phis) {
    if (!spend(Exit.Ops.size()))
      return false;
    llvm::SmallPtrSet<const MedOp *, 4> ExitShifts;
    for (const MedOp &Op : Exit.Ops)
      if (isVariableShift(Op))
        ExitShifts.insert(&Op);
    bool HasShift = false;
    bool HasEarlierJoin = false;
    llvm::SmallVector<const MedVar *, 12> Pending{&Result};
    MedValueMap<const MedVar *> Seen;
    Registers.try_emplace({TRI.IntReturnReg, TRI.FullRegWidth}, 0);
    while (!Pending.empty()) {
      const auto &V = *Pending.pop_back_val();
      if (!spend(1))
        return false;
      if (V.isConst())
        continue;
      auto [It, Added] = Seen.try_emplace(medValueKey(V), &V);
      if (!Added) {
        if (!sameView(*It->second, V))
          return false;
        continue;
      }
      if (V.Kind == MedVar::Reg)
        Registers.try_emplace({V.RegOff, V.Size}, Registers.size());
      if (const auto P = Phis.find(medValueKey(V)); P != Phis.end()) {
        const auto &[Phi, Block] = P->second;
        if (!sameView(Phi->Output, V) || !spend(Phi->Args.size()))
          return false;
        HasEarlierJoin |= Block != &Exit && Block->Preds.size() > 1;
        NeededPhis.insert(Phi);
        for (const auto &Arg : Phi->Args)
          Pending.push_back(&Arg.second);
      } else if (const MedOp *Op = Defs.lookup(medValueKey(V))) {
        if (!sameView(Op->Output, V))
          return false;
        HasShift |= ExitShifts.contains(Op);
        for (unsigned I = 0; I < Op->NumInputs; ++I)
          Pending.push_back(&Op->Inputs[I]);
      }
    }
    return HasShift && HasEarlierJoin;
  }

  auto view(const MedVar &V) const {
    return V.Kind == MedVar::Reg ? Registers.find({V.RegOff, V.Size})
                                 : Registers.end();
  }

  Number read(const MedVar &V, const Frame &State, const Locals &Local) const {
    if (!V.Size || V.Size > 8)
      return {};
    if (V.isConst())
      return isScalarLiteral(V) && llvm::isUIntN(V.Size * 8, V.ConstVal)
                 ? Number(V.ConstVal)
                 : Number();
    const Binding *Found = nullptr;
    if (const auto At = Local.find(medValueKey(V)); At != Local.end())
      Found = &At->second;
    else if (const auto At = view(V); At != Registers.end())
      Found = &State[At->second];
    return Found && Found->first && sameView(*Found->first, V) ? Found->second
                                                               : Number();
  }

  Number evaluate(const MedOp &Op, const Frame &State,
                  const Locals &Local) const {
    const unsigned Size = Op.Output.Size;
    if (!Size || Size > 8 || !Op.NumInputs || Op.NumInputs > 2)
      return {};
    const auto &A = Op.Inputs[0];
    const auto L = read(A, State, Local);
    if (!L)
      return {};
    if (Op.NumInputs == 1) {
      if (Op.Opcode == NdOp::COPY && A.Size == Size)
        return L;
      if (A.Size <= Size && Op.Opcode == NdOp::INT_ZEXT)
        return L;
      if (A.Size <= Size && Op.Opcode == NdOp::INT_SEXT)
        return uint64_t(llvm::SignExtend64(*L, A.Size * 8));
      return {};
    }
    const auto &B = Op.Inputs[1];
    const auto R = read(B, State, Local);
    if (!R)
      return {};
    if (Op.Opcode == NdOp::SUBBYTES && isScalarLiteral(B) && A.Size >= Size &&
        *R <= unsigned(A.Size - Size))
      return *L >> (8 * *R);
    if (Op.Opcode == NdOp::CONCAT && A.Size + B.Size == Size)
      return (*L << (8 * B.Size)) | *R;
    if (isScalarShift(Op))
      return shiftedBits(Op.Opcode, 8 * Size, *L, *R);
    if (Op.Opcode == NdOp::INT_AND && isScalarLiteral(B) && A.Size <= Size &&
        B.Size == Size && llvm::isUIntN(8 * A.Size, *R))
      return *L & *R;
    if (scalarEquality(Op))
      return (*L == *R) == (Op.Opcode == NdOp::INT_EQUAL);
    if (A.Size != B.Size || A.Size != Size)
      return {};
    switch (Op.Opcode) {
    case NdOp::INT_AND:
      return *L & *R;
    case NdOp::INT_OR:
      return *L | *R;
    case NdOp::INT_XOR:
      return *L ^ *R;
    case NdOp::INT_ADD:
      return *L + *R;
    case NdOp::INT_SUB:
      return *L - *R;
    default:
      return {};
    }
  }

  bool apply(const MedOp &Op, Frame &State, Locals &Local, bool Seed) const {
    if (Op.Opcode == NdOp::NOP)
      return true;
    if (Op.Opcode == NdOp::INTRINSIC && hasValidMedIntrinsicId(Op) &&
        Op.Inputs[0].ConstVal == uint64_t(Intrinsic::MIPSRequireWord) &&
        mipsScalarMedShapeIsValid(Op, TRI.TheArch)) {
      const auto Word = read(Op.Inputs[1], State, Local);
      return Word && llvm::isInt<32>(static_cast<int64_t>(*Word));
    }
    const MedVar &V = Op.Output;
    // A loop test can define both the architectural zero flag and the SSA
    // predicate. The flag reads its complete operands but changes no carrier.
    const bool EqualityFlag = V.Kind == MedVar::Flag && scalarEquality(Op) &&
                              Op.MemoryOrdering == NdMemoryOrdering::None &&
                              Op.IntrinsicOutputs.empty();
    if (!EqualityFlag && medReadsInputsWhole(Op))
      return false;
    if (V.Kind == MedVar::Reg) {
      const auto [Reg, Width] = TRI.findWideReg(V.RegOff, V.Size);
      if (TRI.isReturnControlReg(Reg) || TRI.isPlatformRegister(Reg, Format) ||
          TRI.callPreservedPrefixSize(Reg, Width, Format))
        return false;
      if (Seed && view(V) == Registers.end() &&
          !llvm::any_of(Registers, [&](const auto &P) {
            return TRI.findWideReg(P.first.first, P.first.second).first == Reg;
          }))
        return false;
    }
    Number N = evaluate(Op, State, Local);
    if (N)
      *N &= llvm::maskTrailingOnes<uint64_t>(8 * V.Size);
    const Binding Written{&V, N};
    Local[medValueKey(V)] = Written;
    if (const auto At = view(V); At != Registers.end())
      State[At->second] = Written;
    return true;
  }

  // Read every incoming operand in the predecessor frame. A PHI earlier in
  // the block must never change what a later PHI reads on a backedge.
  bool enter(const MedBlock &Next, int Pred, const Frame &Before,
             const Locals &Local, Frame &After, bool &CrossedResultJoin) {
    if (!Next.ExceptionalPreds.empty() || !Next.ExceptionalSuccs.empty() ||
        !spend(Next.Phis.size() + Next.Preds.size() + 1) ||
        !llvm::is_contained(Next.Preds, Pred))
      return false;
    if (!Next.Phis.empty() && !CheckedJoins.contains(&Next)) {
      const std::set<int> Preds(Next.Preds.begin(), Next.Preds.end());
      llvm::SmallBitVector Defined(Registers.size());
      for (const PhiNode &Phi : Next.Phis) {
        const auto At = view(Phi.Output);
        if (At == Registers.end())
          continue;
        if (!spend(Phi.Args.size() + Preds.size()) ||
            Defined.test(At->second) ||
            !hasCompleteOrdinaryPhiInputs(Next, Phi, Preds))
          return false;
        Defined.set(At->second);
      }
      CheckedJoins.insert(&Next);
    }
    const unsigned Carrier =
        Registers.lookup({TRI.IntReturnReg, TRI.FullRegWidth});
    bool MergesCarrier = false;
    for (const PhiNode &Phi : Next.Phis) {
      const auto At = view(Phi.Output);
      if (At == Registers.end())
        continue;
      if (!spend(Phi.Args.size()))
        return false;
      const auto Arg = llvm::find_if(
          Phi.Args, [&](const auto &A) { return A.first == Pred; });
      const auto N = read(Arg->second, Before, Local);
      const auto &[Previous, Written] = Before[At->second];
      if (Previous && Local.count(medValueKey(*Previous)) &&
          (!N || Written != N))
        return false;
      After[At->second] = {&Phi.Output, N};
      MergesCarrier |= At->second == Carrier && Next.Preds.size() > 1;
    }
    const auto &Whole = After[Carrier];
    if ((MergesCarrier || &Next == &Exit) && !Whole.second)
      return false;
    CrossedResultJoin |= MergesCarrier && &Next != &Exit;
    for (const PhiNode &Phi : Next.Phis) {
      if (!NeededPhis.contains(&Phi) || Phi.Output.Kind != MedVar::Reg ||
          Phi.Output.Size >= TRI.FullRegWidth)
        continue;
      int Offset = TRI.subRegByteOffset(Phi.Output.RegOff, Phi.Output.Size,
                                        TRI.IntReturnReg, TRI.FullRegWidth);
      if (Offset < 0)
        continue;
      const auto &N = After[view(Phi.Output)->second].second;
      if (!Whole.second || !N ||
          *N != ((*Whole.second >> (8 * Offset)) &
                 llvm::maskTrailingOnes<uint64_t>(8 * Phi.Output.Size)))
        return false;
    }
    return true;
  }

  bool outcomes(int Seed, Outcomes &Values, bool &CrossedResultJoin) {
    using Point = std::pair<int, Frame>;
    llvm::SmallVector<Point, 8> Pending;
    Pending.emplace_back(Seed, Frame(Registers.size()));
    std::set<Point> Seen;
    while (!Pending.empty()) {
      auto [Id, State] = Pending.pop_back_val();
      const MedBlock &B = Func.Blocks[Id];
      if (!spend(B.Ops.size() + 1) || !B.ExceptionalPreds.empty() ||
          !B.ExceptionalSuccs.empty())
        return false;
      const bool Initial = Id == Seed;
      Locals Local;
      const MedOp *Term = nullptr;
      for (const MedOp &Op : B.Ops) {
        if (&Op == &B.Ops.back() &&
            (Op.Opcode == NdOp::COND_BR || Op.Opcode == NdOp::BRANCH ||
             Op.Opcode == NdOp::RETURN)) {
          Term = &Op;
          break;
        }
        if (!apply(Op, State, Local, Initial && B.Succs.size() == 1))
          return false;
      }
      if (&B == &Exit) {
        const auto V = read(Result, State, Local);
        if (!Term || Term->Opcode != NdOp::RETURN || !V)
          return false;
        Values.push_back(*V & llvm::maskTrailingOnes<uint64_t>(8 * Bytes));
        continue;
      }
      const bool Conditional = Term && Term->Opcode == NdOp::COND_BR;
      if (B.Succs.size() != (Conditional ? 2u : 1u) ||
          (Term && Term->Opcode == NdOp::RETURN))
        return false;
      Number Taken;
      if (Conditional) {
        if (Term->NumInputs != 2 || !Term->Inputs[0].isConst() ||
            Term->Inputs[1].Kind == MedVar::Unspecified ||
            B.Succs[0] == B.Succs[1])
          return false;
        Taken = read(Term->Inputs[1], State, Local);
      }
      unsigned Targets = 0;
      for (int Succ : B.Succs) {
        if (static_cast<unsigned>(Succ) >= Func.Blocks.size() || Succ == Seed)
          return false;
        const MedBlock &Next = Func.Blocks[Succ];
        if (Conditional) {
          const bool Target = Next.StartAddr == Term->Inputs[0].ConstVal;
          Targets += Target;
          if (Taken && bool(*Taken) != Target)
            continue;
        } else if (Term &&
                   (Term->NumInputs != 1 || !Term->Inputs[0].isConst() ||
                    Term->Inputs[0].ConstVal != Next.StartAddr))
          return false;
        if (!spend(State.size()))
          return false;
        Frame Incoming = State;
        if (!enter(Next, Id, State, Local, Incoming, CrossedResultJoin))
          return false;
        if (Seen.emplace(Succ, Incoming).second)
          Pending.emplace_back(Succ, std::move(Incoming));
      }
      if (Conditional && Targets != 1)
        return false;
    }
    llvm::sort(Values);
    return !Values.empty();
  }
};
} // namespace

bool selectsShiftedReturnAcrossJoins(
    const MedFunc &Func, const MedBlock &Exit, const MedVar &Result,
    const TargetRegInfo &TRI, BinaryFormat Format, unsigned Bytes,
    const MedValueMap<const MedOp *> &Defs, const MedValueMap<PhiSite> &Phis,
    const llvm::SmallBitVector &Reachable, unsigned &Work) {
  ShiftPaths Paths{Func, Exit, Result, TRI, Format, Bytes, Work};
  if (!Paths.prepare(Defs, Phis))
    return false;
  for (int Id : Reachable.set_bits()) {
    if (!Paths.spend(1))
      return false;
    const MedBlock &Choice = Func.Blocks[Id];
    if (Choice.Succs.size() != 2 || Choice.Succs[0] == Choice.Succs[1] ||
        !Choice.ExceptionalPreds.empty() || !Choice.ExceptionalSuccs.empty() ||
        Choice.Ops.empty() || Choice.Ops.back().Opcode != NdOp::COND_BR ||
        Choice.Ops.back().NumInputs != 2 ||
        !Choice.Ops.back().Inputs[0].isConst() ||
        Choice.Ops.back().Inputs[1].isConst() ||
        Choice.Ops.back().Inputs[1].Kind == MedVar::Unspecified)
      continue;
    const int A = Choice.Succs[0], B = Choice.Succs[1];
    if (static_cast<unsigned>(A) >= Func.Blocks.size() ||
        static_cast<unsigned>(B) >= Func.Blocks.size())
      continue;
    const auto &L = Func.Blocks[A], &R = Func.Blocks[B];
    const auto Target = Choice.Ops.back().Inputs[0].ConstVal;
    if ((L.StartAddr == Target) == (R.StartAddr == Target))
      continue;
    if (L.Preds.size() != 1 || R.Preds.size() != 1 || L.Preds.front() != Id ||
        R.Preds.front() != Id || L.Succs.empty() || R.Succs.empty())
      continue;
    // Only one side of an outer choice may pass through the earlier join.
    // The single-join case has its own proof of the allowed count domain.
    bool CrossedResultJoin = false;
    Outcomes Left, Right;
    if (Paths.outcomes(A, Left, CrossedResultJoin) &&
        Paths.outcomes(B, Right, CrossedResultJoin) && CrossedResultJoin &&
        llvm::none_of(
            Left, [&](uint64_t V) { return llvm::binary_search(Right, V); }))
      return true;
  }
  return false;
}

} // namespace neverd::return_contract
