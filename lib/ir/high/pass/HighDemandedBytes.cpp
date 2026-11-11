//===- HighDemandedBytes.cpp - Demanded integer bits ----------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// A value assembled from known bytes and the bytes of a register the function
// never set, such as a lane written into an undefined NEON register or the
// upper half an SSE conversion keeps, prints its unknown bytes as a trap even
// where no store, call, return, branch or trap ever depends on them.
//
// This pass finds which bits of each value an observable use depends on,
// walking back from those uses through truncations, extensions, byte merges,
// constant masks, constant shifts and carries, and through each variable to
// its definitions. An unknown leaf with no observed bit becomes zero; other
// pure definitions reuse integer bit projection. Observed unknown bits, calls,
// loads and possible traps retain their evaluation.
//
//===----------------------------------------------------------------------===//

#include "HighDCEDetail.h"

#include "neverd/ir/Limits.h"
#include "neverd/ir/high/HighIR.h"
#include "neverd/ir/high/HighIntegerProjection.h"
#include "neverd/ir/high/HighSourceFlow.h"
#include "neverd/ir/intrinsics/X86/X86VectorLaneReads.h"

#include "llvm/ADT/APInt.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/bit.h"

#include <algorithm>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace neverd {
namespace {

/// Keep the previous 64-byte extent, but retain each bit within that extent.
/// Coalescing a flag read to a byte would keep PF/AF/SF alive with CF and ZF.
using BitMask = llvm::APInt;
constexpr unsigned kDemandBits = 64 * 8;

BitMask allBytes(uint16_t Bytes) {
  return BitMask::getLowBitsSet(kDemandBits, !Bytes || Bytes >= 64 ? kDemandBits
                                                                   : Bytes * 8);
}

uint16_t exprBytes(const HighExpr &E) {
  if (E.Type && E.Type->Size)
    return E.Type->Size;
  return E.Kind == ExprKind::Var || E.Kind == ExprKind::Phi ? E.Var.Size : 0;
}

/// The bits at or below the highest demanded one: a sum, difference,
/// product or negation carries only upward.
BitMask carryFill(const BitMask &Demand) {
  return BitMask::getLowBitsSet(kDemandBits, Demand.getActiveBits());
}

/// The nonzero bits of \p Value, in a \p Bytes wide operand.
BitMask nonzeroBytes(uint64_t Value, uint16_t Bytes) {
  return BitMask(kDemandBits, Value) & allBytes(Bytes);
}

/// The operand bits a left shift by \p Bits reads for the result bits in
/// \p Demand: result bit b is operand bit b - Bits.
BitMask leftShiftSource(const BitMask &Demand, uint64_t Bits, uint16_t Bytes) {
  return Bits >= kDemandBits ? BitMask(kDemandBits, 0)
                             : Demand.lshr(Bits) & allBytes(Bytes);
}

/// The operand bits a right shift by \p Bits reads for the result bits in
/// \p Demand: result bit b is operand bit b + Bits, or the sign for an
/// arithmetic shift past the top.
BitMask rightShiftSource(const BitMask &Demand, uint64_t Bits, uint16_t Bytes,
                         bool Arithmetic) {
  BitMask Source(kDemandBits, 0);
  const uint64_t Top = uint64_t(Bytes) * 8;
  if (Bits < kDemandBits)
    Source = Demand.shl(Bits) & allBytes(Bytes);
  if (Arithmetic && Top && Top <= kDemandBits &&
      (Bits >= Top ? !Demand.isZero() : Demand.getActiveBits() > Top - Bits))
    Source.setBit(Top - 1);
  return Source & allBytes(Bytes);
}

class DemandedBits {
public:
  DemandedBits(HighFunc &Func, Arch Architecture)
      : Func(Func), Architecture(Architecture) {}

  void run() {
    collectDefinitions();
    seed();
    while (!Pending.empty() && Budget) {
      const HighExpr *E = Pending.back();
      Pending.pop_back();
      propagate(*E);
    }
    if (!Budget)
      return;
    replaceUndemandedLeaves();
    projectDefinitions();
  }

private:
  HighFunc &Func;
  Arch Architecture;
  size_t Budget = ::neverd::limits::ir::kHighDemandedBytesVisitBudget;
  std::unordered_map<const HighExpr *, BitMask> NodeDemand;
  std::map<HighSourceLocalIdentity, BitMask> VarDemand;
  std::map<HighSourceLocalIdentity, std::vector<const HighExpr *>> Definitions;
  std::set<HighSourceLocalIdentity> Assigned;
  /// Variables a join copy assigns: their definitions need not cover every
  /// path that reaches a use.
  std::set<HighSourceLocalIdentity> JoinCopied;
  std::vector<const HighExpr *> Pending;

  /// A register, flag or temporary, which only its uses read.  A stack slot
  /// or parameter may also be read through memory, which no Var names.
  static bool readOnlyByUses(const MedVar &V) {
    return V.Kind == MedVar::Reg || V.Kind == MedVar::Flag ||
           V.Kind == MedVar::Temp;
  }

  /// Each variable's assigned values, which its uses' demand reaches.
  void collectDefinitions() {
    walkStmts(Func.Body, [&](HighStmt &S) {
      if (S.Kind != StmtKind::Assign || !S.Dst || S.Dst->Kind != ExprKind::Var)
        return;
      Assigned.insert(highSourceLocalIdentity(S.Dst->Var));
      if (S.IsPhiCopy)
        JoinCopied.insert(highSourceLocalIdentity(S.Dst->Var));
      if (S.Val && readOnlyByUses(S.Dst->Var))
        Definitions[highSourceLocalIdentity(S.Dst->Var)].push_back(S.Val.get());
    });
  }

  /// The bytes of \p E that may be nonzero, where E is a constant: a number,
  /// a concatenation or zero extension of constants, or a variable assigned
  /// once to one (a vector mask built from two halves, `vand q, q9` after
  /// `vmov.i32 q9, #0xff`).
  std::optional<BitMask> constantNonzeroBits(const HighExpr &E,
                                             unsigned Depth) const {
    if (Depth > ::neverd::limits::ir::kHighDemandedBytesMaxConstantMaskDepth)
      return std::nullopt;
    if (E.Kind == ExprKind::Const)
      return nonzeroBytes(E.ConstVal, exprBytes(E));
    if (E.Kind == ExprKind::BinOp && E.Op == NdOp::CONCAT &&
        E.Operands.size() == 2 && E.Operands[0] && E.Operands[1]) {
      const uint16_t LowBytes = exprBytes(*E.Operands[1]);
      if (!LowBytes || LowBytes >= 64)
        return std::nullopt;
      const auto High = constantNonzeroBits(*E.Operands[0], Depth + 1);
      const auto Low = constantNonzeroBits(*E.Operands[1], Depth + 1);
      if (!High || !Low)
        return std::nullopt;
      return (*Low & allBytes(LowBytes)) | (*High << (LowBytes * 8));
    }
    if (E.Kind == ExprKind::UnaryOp && E.Op == NdOp::INT_ZEXT &&
        E.Operands.size() == 1 && E.Operands[0])
      return constantNonzeroBits(*E.Operands[0], Depth + 1);
    // One assignment defines the variable everywhere: an incoming value has
    // none, and a join whose entry edge brings one is a join copy.
    if ((E.Kind == ExprKind::Var || E.Kind == ExprKind::Phi) &&
        !JoinCopied.count(highSourceLocalIdentity(E.Var))) {
      const auto It = Definitions.find(highSourceLocalIdentity(E.Var));
      if (It != Definitions.end() && It->second.size() == 1 && It->second[0])
        return constantNonzeroBits(*It->second[0], Depth + 1);
    }
    return std::nullopt;
  }

  void demand(const HighExpr *E, BitMask Mask) {
    if (!E || Mask.isZero() || !Budget)
      return;
    --Budget;
    auto &Current = NodeDemand.try_emplace(E, kDemandBits, 0).first->second;
    if ((Current | Mask) == Current)
      return;
    Current |= Mask;
    Pending.push_back(E);
  }

  void demandAll(const ExprPtr &E) {
    if (E)
      demand(E.get(), allBytes(exprBytes(*E)));
  }

  /// A node whose evaluation is observable whether or not its value is: a
  /// call, a memory access, an intrinsic, or an operation that can trap.
  static bool observableEvaluation(const HighExpr &E) {
    if (E.MemoryOrdering != NdMemoryOrdering::None ||
        E.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
        E.IntrinsicId != Intrinsic::None || !E.IntrinsicOutputs.empty() ||
        E.IndirectTarget)
      return true;
    switch (E.Kind) {
    case ExprKind::Call:
    case ExprKind::Load:
    case ExprKind::Store:
      return true;
    case ExprKind::BinOp:
      return E.Op == NdOp::INT_DIV || E.Op == NdOp::INT_SDIV ||
             E.Op == NdOp::INT_REM || E.Op == NdOp::INT_SREM;
    default:
      return false;
    }
  }

  /// The bytes of a returned value C returns: the integer return type's, as
  /// a bound source declaration gives it or the lift inferred it.  None where
  /// a debug declaration, which only C's writer looks up, may replace it.
  std::optional<uint16_t> returnedBytes() const {
    TypeRef Type = Func.ReturnType;
    if (Func.SourceTypeHint && Func.SourceTypeHint->ReturnType)
      Type = Func.SourceTypeHint->ReturnType;
    else if (!Func.DebugName.empty())
      return std::nullopt;
    if (!Type || Type->Kind != NdTypeKind::Int || !Type->Size)
      return std::nullopt;
    return Type->Size;
  }

  /// Every operand of every observable evaluation, and every root a
  /// statement reads for anything but a variable's value, is demanded whole.
  void seed() {
    // Observable operand demand depends only on the node, not on the root
    // that reaches it. The graph is unchanged throughout this seed phase.
    HighExprSet Seen;
    walkStmts(Func.Body, [&](HighStmt &S) {
      const bool DefinesVar =
          S.Kind == StmtKind::Assign && S.Dst && S.Dst->Kind == ExprKind::Var;
      forEachExpr(S, [&](const ExprPtr &Root) {
        seedObservableEvaluations(Root, Seen);
        if (DefinesVar && &Root == &S.Dst)
          return;
        // Assignment can itself extend or truncate a value. Keep that whole
        // value unless its bit positions match the destination's exactly.
        if (DefinesVar && &Root == &S.Val && readOnlyByUses(S.Dst->Var) &&
            Root && exprBytes(*S.Dst) == exprBytes(*Root))
          return;
        // A function returns its value as its return type: `return v;` of
        // an int32_t v in an int8_t function reads v's low byte.
        if (S.Kind == StmtKind::Return && &Root == &S.RetVal && Root)
          if (const std::optional<uint16_t> Bytes = returnedBytes();
              Bytes && *Bytes < exprBytes(*Root)) {
            demand(Root.get(), allBytes(*Bytes));
            return;
          }
        if (S.Kind == StmtKind::Assign && &Root == &S.Dst) {
          // An assignment through memory reads its address.
          Root->forEachChildExpr([&](const ExprPtr &C) { demandAll(C); });
          return;
        }
        demandAll(Root);
      });
    });
  }

  /// Demand what observable \p E reads of each operand: every byte, except
  /// where an intrinsic reads only some bytes of its vector operands.
  void demandOperandsOf(const HighExpr &E) {
    E.forEachChildExpr([&](const ExprPtr &C) {
      if (C && E.Kind == ExprKind::Call && E.IntrinsicId != Intrinsic::None)
        if (const auto Read =
                x86VectorOperandReadBytes(E.IntrinsicId, exprBytes(*C))) {
          BitMask Bits(kDemandBits, 0);
          for (unsigned I = 0; I != 64; ++I)
            if ((*Read >> I) & 1)
              Bits.setBits(I * 8, (I + 1) * 8);
          demand(C.get(), std::move(Bits));
          return;
        }
      demandAll(C);
    });
  }

  void seedObservableEvaluations(const ExprPtr &Root, HighExprSet &Seen) {
    llvm::SmallVector<const HighExpr *, 16> Stack;
    const auto Enqueue = [&](const ExprPtr &E) {
      if (!E || (E->Operands.empty() && !E->IndirectTarget))
        return; // No operand demand to seed, even for an observable leaf.
      // Edges and roots retain shared ownership during this unchanged walk.
      // A sole owner needs no visited entry: repeated paths or a reachable
      // cycle must first pass a node with more than one owning reference.
      if (E.use_count() == 1 || Seen.insert(E.get()).second)
        Stack.push_back(E.get());
    };
    Enqueue(Root);
    while (!Stack.empty() && Budget) {
      const HighExpr *E = Stack.back();
      Stack.pop_back();
      --Budget;
      if (observableEvaluation(*E))
        demandOperandsOf(*E);
      E->forEachChildExpr(Enqueue);
    }
  }

  void propagate(const HighExpr &E) {
    const auto &D = NodeDemand.at(&E);
    if (observableEvaluation(E)) {
      demandOperandsOf(E);
      return;
    }
    if (E.Kind != ExprKind::Var && E.Kind != ExprKind::Phi &&
        (exprBytes(E) > 64 || std::any_of(E.Operands.begin(), E.Operands.end(),
                                          [](const ExprPtr &P) {
                                            return P && exprBytes(*P) > 64;
                                          }))) {
      E.forEachChildExpr([&](const ExprPtr &C) { demandAll(C); });
      return;
    }
    switch (E.Kind) {
    case ExprKind::Phi:
      // A join of incoming values, read as its variable too.
      E.forEachChildExpr([&](const ExprPtr &C) { demand(C.get(), D); });
      [[fallthrough]];
    case ExprKind::Var: {
      auto &Current =
          VarDemand.try_emplace(highSourceLocalIdentity(E.Var), kDemandBits, 0)
              .first->second;
      if ((Current | D) == Current)
        return;
      Current |= D;
      const auto It = Definitions.find(highSourceLocalIdentity(E.Var));
      if (It != Definitions.end())
        for (const HighExpr *Value : It->second)
          demand(Value, Current);
      return;
    }
    case ExprKind::Const:
    case ExprKind::Undef:
    case ExprKind::IncomingStack:
    case ExprKind::EntryRegister:
      return;
    case ExprKind::UnaryOp:
      propagateUnary(E, D);
      return;
    case ExprKind::BinOp:
      propagateBinary(E, D);
      return;
    case ExprKind::Cast:
    case ExprKind::BitCast:
      propagateCast(E, D);
      return;
    default:
      E.forEachChildExpr([&](const ExprPtr &C) { demandAll(C); });
      return;
    }
  }

  static bool integer(const ExprPtr &E) {
    return E && E->Type && E->Type->Kind == NdTypeKind::Int;
  }

  void propagateUnary(const HighExpr &E, BitMask D) {
    const ExprPtr Only = E.Operands.size() == 1 ? E.Operands[0] : nullptr;
    if (!integer(Only) || !E.Type || E.Type->Kind != NdTypeKind::Int) {
      E.forEachChildExpr([&](const ExprPtr &C) { demandAll(C); });
      return;
    }
    const uint16_t Bytes = exprBytes(*Only);
    switch (E.Op) {
    case NdOp::INT_ZEXT:
      demand(Only.get(), D & allBytes(Bytes));
      return;
    case NdOp::INT_SEXT: {
      auto Source = D & allBytes(Bytes);
      if (Bytes && Bytes <= 64 && !(D & ~allBytes(Bytes)).isZero())
        Source.setBit(Bytes * 8 - 1);
      demand(Only.get(), std::move(Source));
    }
      return;
    case NdOp::INT_NOT:
      demand(Only.get(), D & allBytes(Bytes));
      return;
    case NdOp::INT_NEGATE:
      demand(Only.get(), carryFill(D) & allBytes(Bytes));
      return;
    default:
      demandAll(Only);
      return;
    }
  }

  void propagateCast(const HighExpr &E, BitMask D) {
    const ExprPtr Only = E.Operands.size() == 1 ? E.Operands[0] : nullptr;
    if (!integer(Only) || !E.Type || E.Type->Kind != NdTypeKind::Int) {
      E.forEachChildExpr([&](const ExprPtr &C) { demandAll(C); });
      return;
    }
    const uint16_t From = exprBytes(*Only);
    const uint16_t To = exprBytes(E);
    if (!From || !To) {
      demandAll(Only);
      return;
    }
    // A truncation keeps the low bytes; an extension adds bytes made of the
    // operand's sign (or of zero), which its top byte decides.
    BitMask Source = D & allBytes(std::min(From, To));
    if (To > From && From <= 64 && !(D & ~allBytes(From)).isZero())
      Source.setBit(From * 8 - 1);
    demand(Only.get(), Source);
  }

  void propagateBinary(const HighExpr &E, BitMask D) {
    if (E.Op == NdOp::SELECT && E.Operands.size() == 3 &&
        integer(E.Operands[1]) && integer(E.Operands[2]) && E.Type &&
        E.Type->Kind == NdTypeKind::Int &&
        exprBytes(*E.Operands[1]) == exprBytes(E) &&
        exprBytes(*E.Operands[2]) == exprBytes(E)) {
      demandAll(E.Operands[0]);
      demand(E.Operands[1].get(), D);
      demand(E.Operands[2].get(), D);
      return;
    }
    if (E.Operands.size() != 2 || !integer(E.Operands[0]) || !E.Operands[1] ||
        !E.Type || E.Type->Kind != NdTypeKind::Int) {
      E.forEachChildExpr([&](const ExprPtr &C) { demandAll(C); });
      return;
    }
    const ExprPtr &L = E.Operands[0];
    const ExprPtr &R = E.Operands[1];
    const uint16_t Bytes = exprBytes(*L);
    const auto Constant = [](const ExprPtr &X) {
      return X && X->Kind == ExprKind::Const;
    };
    switch (E.Op) {
    case NdOp::SUBBYTES:
      if (Constant(R) && R->ConstVal < 64) {
        demand(L.get(), (D << (R->ConstVal * 8)) & allBytes(Bytes));
        return;
      }
      break;
    case NdOp::CONCAT: {
      // The high part above the low part.
      const uint16_t Low = exprBytes(*R);
      if (!Low || !integer(R))
        break;
      demand(R.get(), D & allBytes(Low));
      demand(L.get(), Low < 64 ? D.lshr(Low * 8) & allBytes(Bytes)
                               : BitMask(kDemandBits, 0));
      return;
    }
    case NdOp::INT_AND:
      // A byte a constant mask clears is no byte of the other operand.
      if (const auto Mask = constantNonzeroBits(*R, 0)) {
        demand(L.get(), D & *Mask);
        if (!Constant(R))
          demand(R.get(), D & allBytes(exprBytes(*R)));
        return;
      }
      if (integer(R))
        if (const auto Mask = constantNonzeroBits(*L, 0)) {
          demand(R.get(), D & *Mask);
          if (!Constant(L))
            demand(L.get(), D & allBytes(Bytes));
          return;
        }
      demand(L.get(), D & allBytes(Bytes));
      demand(R.get(), D & allBytes(exprBytes(*R)));
      return;
    case NdOp::INT_OR:
    case NdOp::INT_XOR:
      demand(L.get(), D & allBytes(Bytes));
      demand(R.get(), D & allBytes(exprBytes(*R)));
      return;
    case NdOp::INT_MULT:
      // An exact same-width power-of-two product is a left shift. General
      // carry demand would keep every lower predicate bit alive even when
      // the consumer reads only higher fields of a packed word.
      for (unsigned I = 0; I != 2; ++I) {
        const auto &Factor = E.Operands[I];
        const auto &Value = E.Operands[1 - I];
        if (!exprBytes(E) || !Constant(Factor) || !Factor->Operands.empty() ||
            !integer(Factor) || !integer(Value) ||
            exprBytes(*Factor) != exprBytes(E) ||
            exprBytes(*Value) != exprBytes(E))
          continue;
        const BitMask Scale = nonzeroBytes(Factor->ConstVal, exprBytes(E));
        if (!Scale.isPowerOf2())
          continue;
        demand(Value.get(),
               leftShiftSource(D, Scale.countr_zero(), exprBytes(*Value)));
        return;
      }
      [[fallthrough]];
    case NdOp::INT_ADD:
    case NdOp::INT_SUB:
      demand(L.get(), carryFill(D) & allBytes(Bytes));
      demand(R.get(), carryFill(D) & allBytes(exprBytes(*R)));
      return;
    case NdOp::INT_LEFT:
    case NdOp::INT_RIGHT:
    case NdOp::INT_ASHR: {
      // A shift by amounts its count can take reads the bytes it moves into
      // a demanded one under any of them.
      llvm::SmallVector<uint64_t, 2> Amounts;
      if (!shiftAmounts(*R, Amounts, 0))
        break;
      BitMask Source(kDemandBits, 0);
      for (const uint64_t Amount : Amounts)
        Source |=
            E.Op == NdOp::INT_LEFT
                ? leftShiftSource(D, Amount, Bytes)
                : rightShiftSource(D, Amount, Bytes, E.Op == NdOp::INT_ASHR);
      demand(L.get(), Source);
      if (!Constant(R))
        demandAll(R);
      return;
    }
    default:
      break;
    }
    demandAll(L);
    demandAll(R);
  }

  /// The amounts a shift count can take: a constant, or a register or
  /// temporary every assignment of which gives one.
  bool shiftAmounts(const HighExpr &Count, llvm::SmallVectorImpl<uint64_t> &Out,
                    unsigned Depth) const {
    if (Count.Kind == ExprKind::Const) {
      Out.push_back(Count.ConstVal);
      return true;
    }
    if (Depth > ::neverd::limits::ir::kHighDemandedBytesShiftAmountsDepthLimit)
      return false;
    // A conversion of one keeps its value within the converted width.
    if ((Count.Kind == ExprKind::Cast ||
         (Count.Kind == ExprKind::UnaryOp &&
          (Count.Op == NdOp::INT_ZEXT || Count.Op == NdOp::INT_SEXT))) &&
        Count.Operands.size() == 1 && Count.Operands[0]) {
      const size_t First = Out.size();
      if (!shiftAmounts(*Count.Operands[0], Out, Depth + 1))
        return false;
      if (const uint16_t Bytes = exprBytes(Count); Bytes && Bytes < 8)
        for (size_t I = First; I < Out.size(); ++I)
          Out[I] &= (uint64_t{1} << (8 * Bytes)) - 1;
      return true;
    }
    if (Count.Kind != ExprKind::Var || !readOnlyByUses(Count.Var))
      return false;
    const auto It = Definitions.find(highSourceLocalIdentity(Count.Var));
    if (It == Definitions.end() || It->second.empty())
      return false;
    for (const HighExpr *Value : It->second)
      if (!Value || !shiftAmounts(*Value, Out, Depth + 1))
        return false;
    return true;
  }

  /// An entry value of a register or flag the function neither receives as
  /// a parameter nor ever assigns, which the C prints as unknown.
  bool unknownLeaf(const HighExpr &E) const {
    if (E.Kind == ExprKind::Undef)
      return true;
    if (E.Kind != ExprKind::Var ||
        (E.Var.Kind != MedVar::Reg && E.Var.Kind != MedVar::Flag) ||
        E.Var.SSAVer != 0 || E.Var.Id < 0 ||
        Assigned.count(highSourceLocalIdentity(E.Var)) ||
        isSyntheticEntryStackPointer(E.Var, Func, Architecture))
      return false;
    for (const HighParam &Param : Func.Params)
      if (Param.RegOff != kNoParamReg && E.Var.Kind == MedVar::Reg &&
          Param.RegOff == E.Var.RegOff)
        return false;
    return true;
  }

  void replaceUndemandedLeaves() {
    std::unordered_set<const HighExpr *> Seen;
    const auto Zero = [](const ExprPtr &Leaf) {
      ExprPtr Value = HighExpr::makeConst(0, exprBytes(*Leaf));
      if (Leaf->Type)
        Value->Type = Leaf->Type;
      return Value;
    };
    const auto Replace = [&](ExprPtr &Slot) {
      if (!Slot || !unknownLeaf(*Slot) || !exprBytes(*Slot))
        return;
      const auto It = NodeDemand.find(Slot.get());
      if (It != NodeDemand.end() && !It->second.isZero())
        return;
      Slot = Zero(Slot);
    };
    std::function<void(const ExprPtr &)> Visit = [&](const ExprPtr &E) {
      if (!E || !Seen.insert(E.get()).second)
        return;
      for (ExprPtr &Operand : E->Operands) {
        Replace(Operand);
        Visit(Operand);
      }
      if (E->IndirectTarget) {
        Replace(E->IndirectTarget);
        Visit(E->IndirectTarget);
      }
    };
    walkStmts(Func.Body, [&](HighStmt &S) {
      // A variable no use reads a byte of, defined as an unknown value
      // alone, which the C would otherwise omit and trap at each use.
      if (S.Kind == StmtKind::Assign && S.Dst && S.Dst->Kind == ExprKind::Var &&
          readOnlyByUses(S.Dst->Var) && S.Val && unknownLeaf(*S.Val) &&
          exprBytes(*S.Val)) {
        const auto It = VarDemand.find(highSourceLocalIdentity(S.Dst->Var));
        if (It == VarDemand.end() || It->second.isZero())
          S.Val = Zero(S.Val);
      }
      forEachExpr(S, [&](const ExprPtr &Root) { Visit(Root); });
    });
  }

  void projectDefinitions() {
    walkStmts(Func.Body, [&](HighStmt &S) {
      if (S.Kind != StmtKind::Assign || !S.Dst || !S.Val ||
          S.Dst->Kind != ExprKind::Var || !readOnlyByUses(S.Dst->Var) ||
          !integer(S.Val) || !exprBytes(*S.Val) || exprBytes(*S.Val) > 8 ||
          exprBytes(*S.Dst) != exprBytes(*S.Val))
        return;
      const auto At = VarDemand.find(highSourceLocalIdentity(S.Dst->Var));
      if (At == VarDemand.end())
        return; // Ordinary DCE owns definitions without any readers.
      const auto Mask = At->second & allBytes(exprBytes(*S.Val));
      if (Mask == allBytes(exprBytes(*S.Val)))
        return;
      S.Val = projectHighIntegerBits(S.Val, Mask.getZExtValue());
    });
  }
};

} // namespace

void zeroUndemandedUnknownBytes(HighFunc &Func, Arch Architecture) {
  if (Architecture == Arch::Unknown)
    return;
  DemandedBits(Func, Architecture).run();
}

} // namespace neverd
