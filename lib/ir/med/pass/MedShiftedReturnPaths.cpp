//===- MedShiftedReturnPaths.cpp - Paired shift paths ---------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Follow an integer return and its shift count through the same ordinary CFG
/// edges. A repeated header is closed only for the same incoming bindings.
//===----------------------------------------------------------------------===//

#include "neverd/ir/med/MedShiftedReturnPaths.h"

#include "neverd/ir/Limits.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/ir/med/IntrinsicShapes.h"
#include "neverd/ir/med/MedCallingConvDetail.h"
#include "neverd/ir/med/MedConstantPropagation.h"
#include "neverd/lift/AArch64/AArch64Regs.h"
#include "neverd/lift/MIPS/MIPSRegs.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/Support/MathExtras.h"

#include <algorithm>
#include <optional>
#include <set>

namespace neverd::return_contract {

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

bool charge(unsigned &Work, size_t Amount) {
  if (Amount > Work) {
    Work = 0;
    return false;
  }
  Work -= Amount;
  return true;
}

bool preserved(const TargetRegInfo &TRI, BinaryFormat Format, uint64_t Reg,
               unsigned Width) {
  return TRI.isReturnControlReg(Reg) || TRI.isPlatformRegister(Reg, Format) ||
         TRI.callPreservedPrefixSize(Reg, Width, Format);
}

bool integerCarrier(const TargetRegInfo &TRI, uint64_t Reg) {
  if (isMIPS(TRI.TheArch))
    return Reg > mipsreg::Zero && Reg <= mipsreg::RA &&
           Reg % mipsreg::SlotBytes == 0;
  if (TRI.TheArch == Arch::AArch64)
    return Reg <= a64reg::X30 && Reg % TRI.FullRegWidth == 0;
  return TRI.isGeneralReg(Reg);
}

struct CarrierInput {
  MedVar *Incoming;
  MedVar Active;
  MedVar Output;
};

struct CarrierEdge {
  int Pred;
  llvm::SmallVector<CarrierInput, 4> Inputs;
};

struct CarrierUses {
  const CarrierEdge &Edge;
  const med_calling_conv_detail::ValueSet &Watched;
};

struct JoinedView {
  unsigned Whole;
  unsigned ByteOffset;
  bool CarriesValue;
};

bool sameView(const MedVar &A, const MedVar &B) {
  return A == B && A.Size == B.Size &&
         (A.Kind != MedVar::Reg || A.RegOff == B.RegOff);
}

bool scalarEquality(const MedOp &Op) {
  return (Op.Opcode == NdOp::INT_EQUAL || Op.Opcode == NdOp::INT_NOTEQUAL) &&
         Op.NumInputs == 2 && Op.Output.Size == 1 && Op.Inputs[0].Size &&
         Op.Inputs[0].Size <= 8 && Op.Inputs[0].Size == Op.Inputs[1].Size;
}

bool scalarTransport(const MedOp &Op) {
  if (Op.MemoryOrdering != NdMemoryOrdering::None ||
      !Op.IntrinsicOutputs.empty() || !Op.Output.Size || Op.Output.Size > 8 ||
      (Op.Output.Kind != MedVar::Reg && Op.Output.Kind != MedVar::Temp))
    return false;
  switch (Op.Opcode) {
  case NdOp::COPY:
  case NdOp::INT_ZEXT:
  case NdOp::INT_SEXT:
    return Op.NumInputs == 1;
  case NdOp::SUBBYTES:
    return Op.NumInputs == 2 && isScalarLiteral(Op.Inputs[1]);
  default:
    return false;
  }
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
  MedValueMap<unsigned> CarriedTemps{};
  llvm::SmallDenseMap<const PhiNode *, JoinedView, 8> JoinedViews{};
  llvm::SmallPtrSet<const MedBlock *, 8> CheckedJoins;

  bool spend(size_t Amount) { return charge(Work, Amount); }

  bool recordJoinedView(const PhiNode &Phi, bool CarriesValue) {
    const MedVar &V = Phi.Output;
    if (V.Kind != MedVar::Reg)
      return true;
    const auto [Reg, Width] = TRI.findWideReg(V.RegOff, V.Size);
    if (Width != TRI.FullRegWidth)
      return true;
    if (preserved(TRI, Format, Reg, Width))
      return false;
    if (!CarriesValue && V.Size == Width)
      return true;
    const int Offset = V.Size == Width
                           ? 0
                           : TRI.subRegByteOffset(V.RegOff, V.Size, Reg, Width);
    if (Offset < 0)
      return false;
    const auto Whole =
        Registers.try_emplace({Reg, Width}, Registers.size()).first->second;
    JoinedViews.try_emplace(&Phi,
                            JoinedView{Whole, unsigned(Offset), CarriesValue});
    return true;
  }

  // A forwarded SSA temporary can outlive its block after copy propagation.
  // Only these temporaries need a slot in the state carried around a loop.
  bool indexCarriedTemps(const MedValueMap<const MedVar *> &Slice) {
    const auto Carry = [&](const MedVar &V) {
      if (V.Kind == MedVar::Temp && Slice.count(medValueKey(V)))
        CarriedTemps.try_emplace(medValueKey(V),
                                 Registers.size() + CarriedTemps.size());
    };
    llvm::SmallPtrSet<const MedVar *, 16> Local;
    for (const MedBlock &B : Func.Blocks) {
      if (!spend(1 + B.Phis.size() + B.Ops.size()))
        return false;
      Local.clear();
      for (const PhiNode &Phi : B.Phis) {
        Carry(Phi.Output);
        if (!spend(Phi.Args.size()))
          return false;
        for (const auto &Arg : Phi.Args)
          Carry(Arg.second);
      }
      for (const MedOp &Op : B.Ops) {
        for (unsigned I = 0; I < Op.NumInputs; ++I) {
          const MedVar &V = Op.Inputs[I];
          if (V.Kind == MedVar::Temp) {
            const auto *Needed = Slice.lookup(medValueKey(V));
            if (Needed && !Local.contains(Needed))
              Carry(V);
          }
        }
        if (Op.Output.Kind == MedVar::Temp)
          if (const auto *Needed = Slice.lookup(medValueKey(Op.Output)))
            Local.insert(Needed);
      }
    }
    return true;
  }

  // Only register views feeding the returned expression live across an edge.
  // In particular a changing loop induction variable need not enter the key.
  bool prepare(const MedValueMap<const MedOp *> &Defs,
               const MedValueMap<PhiSite> &Phis, bool BindSelectors = false) {
    MedValueMap<const MedVar *> Slice;
    bool HasShift = false;
    bool HasValueJoin = false;
    bool HasTemps = false;
    llvm::SmallVector<const MedVar *, 12> Values{&Result}, Counts;
    // Source argument recovery must also prove which carrier can be read.
    // A bit-test may read W3 while the eventual shift takes its count from X3.
    if (BindSelectors)
      for (const MedBlock &B : Func.Blocks)
        if (!B.Ops.empty() && B.Ops.back().Opcode == NdOp::COND_BR &&
            B.Ops.back().NumInputs == 2)
          Counts.push_back(&B.Ops.back().Inputs[1]);
    // Visit data before counts so a value used in both roles is recorded once.
    while (!Values.empty() || !Counts.empty()) {
      const bool CarriesValue = !Values.empty();
      auto &Pending = CarriesValue ? Values : Counts;
      const auto &V = *Pending.pop_back_val();
      if (!spend(1))
        return false;
      if (V.isConst())
        continue;
      auto [It, Added] = Slice.try_emplace(medValueKey(V), &V);
      if (!Added) {
        if (!sameView(*It->second, V))
          return false;
        continue;
      }
      if (V.Kind == MedVar::Reg)
        Registers.try_emplace({V.RegOff, V.Size}, Registers.size());
      HasTemps |= V.Kind == MedVar::Temp;
      if (const auto P = Phis.find(medValueKey(V)); P != Phis.end()) {
        const auto &[Phi, Block] = P->second;
        if (!sameView(Phi->Output, V) || !spend(Phi->Args.size()) ||
            !recordJoinedView(*Phi, CarriesValue))
          return false;
        HasValueJoin |= CarriesValue && Block->Preds.size() > 1;
        for (const auto &Arg : Phi->Args)
          Pending.push_back(&Arg.second);
      } else if (const MedOp *Op = Defs.lookup(medValueKey(V))) {
        if (!sameView(Op->Output, V))
          return false;
        // A predecessor may shift before another join or a final byte mask.
        HasShift |= CarriesValue && isScalarShift(*Op);
        for (unsigned I = 0; I < Op->NumInputs; ++I) {
          auto &Inputs = I == 1 && isScalarShift(*Op) ? Counts : Pending;
          Inputs.push_back(&Op->Inputs[I]);
        }
      }
    }
    return HasShift && HasValueJoin && (!HasTemps || indexCarriedTemps(Slice));
  }

  auto view(const MedVar &V) const {
    return V.Kind == MedVar::Reg ? Registers.find({V.RegOff, V.Size})
                                 : Registers.end();
  }

  std::optional<unsigned> slot(const MedVar &V) const {
    if (const auto At = view(V); At != Registers.end())
      return At->second;
    if (V.Kind == MedVar::Temp) {
      const auto At = CarriedTemps.find(medValueKey(V));
      if (At != CarriedTemps.end())
        return At->second;
    }
    return {};
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
    else if (const auto At = slot(V))
      Found = &State[*At];
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
      if (Op.Opcode == NdOp::BOOL_NOT && A.Size == 1 && Size == 1)
        return !*L;
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
    // A bit test can mask a word into a byte result. A count mask can also
    // widen its input when every selected bit exists in that input.
    if (Op.Opcode == NdOp::INT_AND && isScalarLiteral(B) &&
        ((A.Size == B.Size && Size < A.Size) ||
         (A.Size <= Size && B.Size == Size && llvm::isUIntN(8 * A.Size, *R))))
      return *L & *R;
    if (scalarEquality(Op))
      return (*L == *R) == (Op.Opcode == NdOp::INT_EQUAL);
    // A narrow positive immediate, e.g. SUB X3,X3,#256, has the same bits
    // under either extension to the width of the register.
    const bool NarrowLiteral = isScalarLiteral(B) && B.Size < Size &&
                               llvm::isUIntN(8 * B.Size - 1, *R);
    if (A.Size != Size || (B.Size != Size && !NarrowLiteral))
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

  bool apply(const MedOp &Op, Frame &State, Locals &Local, bool Seed,
             bool BeforeABI) const {
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
    // Before ABI recovery a stack argument can still be a load, and dead
    // arithmetic flags may still be present. Their inputs are checked by the
    // use proof. Neither supplies a numeric value without an evaluated op.
    const bool WatchedLoadOrFlag =
        BeforeABI && Op.MemoryOrdering == NdMemoryOrdering::None &&
        Op.IntrinsicOutputs.empty() &&
        ((Op.Opcode == NdOp::LOAD && Op.NumInputs == 1) ||
         (V.Kind == MedVar::Flag &&
          (Op.Opcode == NdOp::COPY ||
           (Op.Opcode >= NdOp::INT_EQUAL && Op.Opcode <= NdOp::BOOL_NOT))));
    if (!EqualityFlag && !WatchedLoadOrFlag && medReadsInputsWhole(Op))
      return false;
    if (V.Kind == MedVar::Reg) {
      const auto [Reg, Width] = TRI.findWideReg(V.RegOff, V.Size);
      if (preserved(TRI, Format, Reg, Width))
        return false;
      // Copy propagation can leave the physical handoff beside a use of its
      // original SSA source. Both describe the same caller-saved value.
      if (Seed && view(V) == Registers.end() &&
          !llvm::any_of(Registers, [&](const auto &P) {
            return TRI.findWideReg(P.first.first, P.first.second).first == Reg;
          })) {
        if (Op.Opcode != NdOp::COPY || Op.NumInputs != 1 || !V.Size ||
            V.Size > 8 || Op.Inputs[0].Size != V.Size ||
            !integerCarrier(TRI, Reg) || !read(Op.Inputs[0], State, Local))
          return false;
      }
    }
    Number N = evaluate(Op, State, Local);
    if (N)
      *N &= llvm::maskTrailingOnes<uint64_t>(8 * V.Size);
    const Binding Written{&V, N};
    Local[medValueKey(V)] = Written;
    if (const auto At = slot(V))
      State[*At] = Written;
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
      llvm::SmallBitVector Defined(Before.size());
      for (const PhiNode &Phi : Next.Phis) {
        const auto At = slot(Phi.Output);
        if (!At)
          continue;
        if (!spend(Phi.Args.size() + Preds.size()) || Defined.test(*At) ||
            !hasCompleteOrdinaryPhiInputs(Next, Phi, Preds))
          return false;
        Defined.set(*At);
      }
      CheckedJoins.insert(&Next);
    }
    for (const PhiNode &Phi : Next.Phis) {
      const auto At = slot(Phi.Output);
      if (!At)
        continue;
      if (!spend(Phi.Args.size()))
        return false;
      const auto Arg = llvm::find_if(
          Phi.Args, [&](const auto &A) { return A.first == Pred; });
      const auto N = read(Arg->second, Before, Local);
      const auto &[Previous, Written] = Before[*At];
      // The same SSA definition agrees even when it holds an unknown loop
      // counter. A folded or renamed input still needs a concrete match.
      if (Phi.Output.Kind == MedVar::Reg && Previous &&
          !sameView(*Previous, Arg->second) &&
          Local.count(medValueKey(*Previous)) && (!N || Written != N))
        return false;
      After[*At] = {&Phi.Output, N};
    }
    for (const PhiNode &Phi : Next.Phis) {
      const auto It = JoinedViews.find(&Phi);
      if (It == JoinedViews.end())
        continue;
      const auto &[WholeIndex, Offset, CarriesValue] = It->second;
      const auto &N = After[view(Phi.Output)->second].second;
      if (!N)
        continue;
      const auto &Whole = After[WholeIndex];
      // A separately joined byte must match its architectural parent on this
      // edge, including when the byte supplies the count for that same word.
      if (Phi.Output.Size < TRI.FullRegWidth &&
          (Whole.first || TRI.isSubRegOf(Phi.Output.RegOff, Phi.Output.Size,
                                         TRI.IntReturnReg, TRI.FullRegWidth)) &&
          (!Whole.second ||
           *N != ((*Whole.second >> (8 * Offset)) &
                  llvm::maskTrailingOnes<uint64_t>(8 * Phi.Output.Size))))
        return false;
      CrossedResultJoin |= CarriesValue && Next.Preds.size() > 1;
    }
    return true;
  }

  bool outcomes(int Seed, Outcomes &Values, bool &CrossedResultJoin,
                const CarrierUses *Uses = nullptr) {
    using Point = std::pair<int, Frame>;
    llvm::SmallVector<Point, 8> Pending;
    Pending.emplace_back(Seed, Frame(Registers.size() + CarriedTemps.size()));
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
        if (Uses && !scalarTransport(Op))
          for (unsigned I = 0; I < Op.NumInputs; ++I)
            if (med_calling_conv_detail::containsValue(Uses->Watched,
                                                       Op.Inputs[I]) &&
                !read(Op.Inputs[I], State, Local))
              return false;
        if (&Op == &B.Ops.back() &&
            (Op.Opcode == NdOp::COND_BR || Op.Opcode == NdOp::BRANCH ||
             Op.Opcode == NdOp::RETURN)) {
          Term = &Op;
          break;
        }
        if (!apply(Op, State, Local, Initial && B.Succs.size() == 1,
                   Uses != nullptr))
          return false;
      }
      // The replacement must be defined on this predecessor. Treat the
      // original input as unknown for every subsequent observation.
      if (Uses && Initial)
        for (const auto &C : Uses->Edge.Inputs)
          if (read(*C.Incoming, State, Local) || !read(C.Active, State, Local))
            return false;
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
// SSA uses -1 for an absent definition and zero for the first binding.
// A version-zero name can also name a real first write on the other arm.
// Classify each PHI operand at its own predecessor, including every low-word
// and full-register view. A count written on both arms is not a spare carrier.
struct CarrierPhi {
  PhiNode *Phi;
  uint64_t Parent;
  llvm::SmallVector<MedVar *, 4> Inputs;
  llvm::SmallBitVector Written;
  llvm::SmallBitVector Unwritten;
};

bool collectCarrierEdges(const MedFunc &Func, MedBlock &Join,
                         const TargetRegInfo &TRI, BinaryFormat Format,
                         unsigned &Work,
                         llvm::SmallVectorImpl<CarrierEdge> &Edges) {
  const size_t N = Join.Preds.size();
  if (N < 2 || Join.Phis.size() < 2 || !Join.ExceptionalPreds.empty() ||
      !Join.ExceptionalSuccs.empty())
    return true;
  const auto IsEntryView = [](const MedVar &V, const MedVar &Out) {
    return V.Kind == MedVar::Reg && (V.SSAVer == -1 || V.SSAVer == 0) &&
           V.RegOff == Out.RegOff && V.Size == Out.Size;
  };
  if (!llvm::any_of(Join.Phis, [&](const PhiNode &P) {
        return P.Output.Kind == MedVar::Reg &&
               P.Output.RegOff == TRI.IntReturnReg &&
               llvm::any_of(P.Args, [&](const auto &A) {
                 return IsEntryView(A.second, P.Output);
               });
      }))
    return true;

  llvm::SmallVector<MedValueMap<const MedVar *>, 4> Written(N);
  llvm::SmallDenseMap<int, unsigned, 4> Positions;
  for (auto [I, Pred] : llvm::enumerate(Join.Preds)) {
    if (static_cast<unsigned>(Pred) >= Func.Blocks.size() ||
        !Positions.try_emplace(Pred, I).second)
      return true;
    const auto &B = Func.Blocks[Pred];
    if (!charge(Work, 1 + B.Ops.size()))
      return false;
    if (B.Succs.size() != 1 || B.Succs.front() != Join.Id)
      continue;
    for (const auto &Op : B.Ops)
      if (Op.Output.Kind == MedVar::Reg &&
          !(Op.Opcode == NdOp::COPY && Op.NumInputs == 1 &&
            sameView(Op.Output, Op.Inputs[0])))
        Written[I].try_emplace(medValueKey(Op.Output), &Op.Output);
  }
  const std::set<int> Preds(Join.Preds.begin(), Join.Preds.end());
  llvm::SmallVector<CarrierPhi, 4> Candidates;
  for (auto &P : Join.Phis) {
    if (!charge(Work, 1 + P.Args.size()))
      return false;
    const auto &V = P.Output;
    if (V.Kind != MedVar::Reg || !V.Size || V.Size > TRI.FullRegWidth)
      continue;
    const auto [Parent, Width] = TRI.findWideReg(V.RegOff, V.Size);
    if (Parent != V.RegOff || Width != TRI.FullRegWidth ||
        preserved(TRI, Format, Parent, Width) ||
        !hasCompleteOrdinaryPhiInputs(Join, P, Preds))
      continue;
    CarrierPhi C{
        &P, Parent, {}, llvm::SmallBitVector(N), llvm::SmallBitVector(N)};
    C.Inputs.resize(N);
    for (auto &[From, In] : P.Args) {
      const unsigned I = Positions.lookup(From);
      C.Inputs[I] = &In;
      const auto *Def = Written[I].lookup(medValueKey(In));
      if (In.Size == V.Size &&
          (isScalarLiteral(In) || (Def && sameView(*Def, In))))
        C.Written.set(I);
      else if (IsEntryView(In, V))
        C.Unwritten.set(I);
    }
    if (C.Written.any() && C.Unwritten.any())
      Candidates.push_back(std::move(C));
  }

  llvm::SmallVector<CarrierEdge, 4> Joined;
  for (int Pred : Join.Preds)
    Joined.push_back({Pred, {}});
  for (const CarrierPhi &Ret : Candidates) {
    if (Ret.Parent != TRI.IntReturnReg)
      continue;
    const CarrierPhi *Scratch = nullptr;
    for (const CarrierPhi &C : Candidates) {
      if (!charge(Work, N))
        return false;
      if (C.Parent == Ret.Parent ||
          C.Phi->Output.Size != Ret.Phi->Output.Size ||
          !C.Written.anyCommon(Ret.Unwritten) ||
          !C.Unwritten.anyCommon(Ret.Written))
        continue;
      if (Scratch) // Multiple possible handoffs of the same register view.
        return true;
      Scratch = &C;
    }
    if (!Scratch)
      continue;
    for (unsigned I = 0; I < N; ++I) {
      const auto &B = Func.Blocks[Join.Preds[I]];
      if (B.Succs.size() != 1 || B.Succs.front() != Join.Id)
        continue;
      if (Ret.Unwritten.test(I) && Scratch->Written.test(I))
        Joined[I].Inputs.push_back(
            {Ret.Inputs[I], *Scratch->Inputs[I], Ret.Phi->Output});
      if (Scratch->Unwritten.test(I) && Ret.Written.test(I))
        Joined[I].Inputs.push_back(
            {Scratch->Inputs[I], *Ret.Inputs[I], Scratch->Phi->Output});
    }
  }
  for (auto &E : Joined)
    if (!E.Inputs.empty())
      Edges.push_back(std::move(E));
  return true;
}
} // namespace

bool foldUnusedShiftCarriers(MedFunc &Func, const TargetRegInfo &TRI,
                             BinaryFormat Format) {
  if (!Func.MultiReturn.empty() || Func.FPReturnViaX87 ||
      Func.ExplicitX87ReturnValue || Func.ReturnsVectorRegister ||
      ((Func.ReturnType ||
        (Func.SourceTypeHint && Func.SourceTypeHint->ReturnType)) &&
       medLeavesOtherRegisters(Func)) ||
      llvm::none_of(Func.Blocks, [](const MedBlock &B) {
        return B.Preds.size() > 1 && B.Phis.size() > 1;
      }))
    return false;
  unsigned Work = limits::ir::kMedShiftedReturnPathWork;
  bool HasShift = false;
  size_t BodyCost = 0;
  for (const MedBlock &B : Func.Blocks) {
    size_t Cost = 1 + B.Ops.size() + B.Phis.size();
    for (const auto &P : B.Phis)
      Cost += P.Args.size();
    if (!charge(Work, Cost))
      return false;
    BodyCost += Cost;
    HasShift = HasShift || llvm::any_of(B.Ops, isVariableShift);
  }
  if (!HasShift)
    return false;
  llvm::SmallVector<CarrierEdge, 4> Edges;
  for (MedBlock &B : Func.Blocks)
    if (!collectCarrierEdges(Func, B, TRI, Format, Work, Edges))
      return false;
  if (Edges.empty())
    return false;

  MedValueMap<const MedOp *> Defs;
  MedValueMap<PhiSite> Phis;
  for (const MedBlock &B : Func.Blocks) {
    for (const MedOp &Op : B.Ops)
      if (Op.Output.Size && !Op.Output.isConst())
        Defs.try_emplace(medValueKey(Op.Output), &Op);
    for (const PhiNode &P : B.Phis)
      Phis.try_emplace(medValueKey(P.Output), PhiSite{&P, &B});
  }
  llvm::SmallVector<CarrierInput, 8> Changes;
  for (const MedBlock &Exit : Func.Blocks) {
    if (!Work || !Exit.Succs.empty() || Exit.Ops.empty() ||
        Exit.Ops.back().Opcode != NdOp::RETURN)
      continue;
    const auto Write =
        llvm::find_if(llvm::reverse(Exit.Ops), [&](const MedOp &Op) {
          return Op.Output.Kind == MedVar::Reg &&
                 Op.Output.RegOff == TRI.IntReturnReg &&
                 Op.Output.Size == TRI.FullRegWidth;
        });
    if (Write == Exit.Ops.rend())
      continue;
    ShiftPaths Paths{Func, Exit, Write->Output, TRI, Format, TRI.FullRegWidth,
                     Work};
    if (!Paths.prepare(Defs, Phis, true))
      continue;
    for (const auto &E : Edges) {
      if (!Paths.spend(BodyCost))
        break;
      llvm::SmallVector<MedVar, 4> Roots;
      for (const auto &C : E.Inputs)
        Roots.push_back(C.Output);
      const auto Uses = med_calling_conv_detail::computeForwardValueClosure(
          Func, Roots, [](const MedOp &Op, unsigned I) {
            return I == 0 && scalarTransport(Op);
          });
      const CarrierUses Check{E, Uses};
      bool Crossed = false;
      Outcomes Values;
      if (Paths.outcomes(E.Pred, Values, Crossed, &Check) && Crossed)
        llvm::append_range(Changes, E.Inputs);
    }
  }
  for (const auto &C : Changes)
    *C.Incoming = C.Active;
  return !Changes.empty();
}

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
    // An outer choice can have just one arm passing through the value join.
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
