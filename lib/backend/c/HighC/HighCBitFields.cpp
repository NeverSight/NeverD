//===- HighCBitFields.cpp - Captured bitfield values --------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Track integer bits within a straight-line region. A field assignment is
/// emitted only when every other stored bit is the current memory bit.
//===----------------------------------------------------------------------===//

#include "HighCWriter.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/MathExtras.h"

namespace neverd {
namespace {
using Identity = HighSourceLocalIdentity;
struct Bit {
  int Constant = -1;
  const HighExpr *Value = nullptr;
  unsigned Index = 0;
  const HighExpr *Read = nullptr;
  unsigned ReadIndex = 0;
};
using Bits = llvm::SmallVector<Bit, 64>;

unsigned width(const HighExpr *E) {
  return E && E->Type &&
                 (E->Type->Kind == NdTypeKind::Int ||
                  E->Type->Kind == NdTypeKind::Ptr) &&
                 E->Type->Size <= 8
             ? E->Type->Size * 8
             : 0;
}

// These operations only select integer bits. In particular, a discarded
// operand must not contain a call, ordered access, or potentially trapping op.
bool bitExpression(const HighExpr *E) {
  if (!width(E) || E->MemoryOrdering != NdMemoryOrdering::None ||
      E->MemoryAddressSpace != NdMemoryAddressSpace::Default ||
      E->IntrinsicId != Intrinsic::None || !E->IntrinsicOutputs.empty())
    return false;
  if (E->Kind == ExprKind::Var)
    return E->Var.Kind != MedVar::Stack;
  if (E->Kind == ExprKind::Const)
    return true;
  if (E->Kind == ExprKind::Load)
    return E->Operands.size() == 1 && bitExpression(E->Operands[0].get());
  if (E->Kind != ExprKind::Cast && E->Kind != ExprKind::BitCast &&
      !((E->Kind == ExprKind::BinOp || E->Kind == ExprKind::UnaryOp) &&
        (E->Op == NdOp::INT_AND || E->Op == NdOp::INT_OR ||
         E->Op == NdOp::INT_LEFT || E->Op == NdOp::INT_RIGHT ||
         E->Op == NdOp::INT_ASHR || E->Op == NdOp::SUBBYTES ||
         E->Op == NdOp::INT_ZEXT || E->Op == NdOp::INT_SEXT ||
         E->Op == NdOp::INT_ADD || E->Op == NdOp::INT_SUB)))
    return false;
  return llvm::all_of(E->Operands, [](const ExprPtr &Child) {
    return bitExpression(Child.get());
  });
}

struct BitValues {
  std::map<Identity, Bits> Locals;
  std::map<const HighExpr *, Bits> Memo;

  Bits get(const HighExpr *E) {
    if (auto It = Memo.find(E); It != Memo.end())
      return It->second;
    const unsigned N = width(E);
    Bits Result(N);
    for (unsigned I = 0; I < N; ++I)
      Result[I] = {-1, E, I};
    if (!N)
      return Result;
    if (E->Kind == ExprKind::Const) {
      for (unsigned I = 0; I < N; ++I)
        Result[I] = {int((E->ConstVal >> I) & 1)};
    } else if (E->Kind == ExprKind::Var) {
      if (auto It = Locals.find(highSourceLocalIdentity(E->Var));
          It != Locals.end() && It->second.size() == N)
        Result = It->second;
    } else if (E->Kind == ExprKind::Load && E->Operands.size() == 1 &&
               E->MemoryOrdering == NdMemoryOrdering::None &&
               E->MemoryAddressSpace == NdMemoryAddressSpace::Default) {
      for (unsigned I = 0; I < N; ++I) {
        Result[I].Read = E;
        Result[I].ReadIndex = I;
      }
    } else if (!E->Operands.empty()) {
      const auto A = get(E->Operands[0].get());
      const unsigned In = A.size();
      const bool Convert = E->Kind == ExprKind::Cast ||
                           E->Kind == ExprKind::BitCast ||
                           E->Op == NdOp::INT_ZEXT || E->Op == NdOp::INT_SEXT;
      if (In && Convert) {
        const bool Signed =
            E->Op == NdOp::INT_SEXT ||
            (E->Kind == ExprKind::Cast && E->Operands[0]->Type->IsSigned);
        for (unsigned I = 0; I < N; ++I)
          Result[I] = I < In ? A[I] : Signed ? A.back() : Bit{0};
      } else if (In && E->Kind == ExprKind::BinOp && E->Operands.size() == 2) {
        const auto B = get(E->Operands[1].get());
        if ((E->Op == NdOp::INT_AND || E->Op == NdOp::INT_OR) && In >= N &&
            B.size() >= N) {
          const int Absorb = E->Op == NdOp::INT_AND ? 0 : 1;
          for (unsigned I = 0; I < N; ++I) {
            if (A[I].Constant == Absorb || B[I].Constant == Absorb)
              Result[I] = {Absorb};
            else if (A[I].Constant == 1 - Absorb)
              Result[I] = B[I];
            else if (B[I].Constant == 1 - Absorb)
              Result[I] = A[I];
          }
        } else if (E->Operands[1]->Kind == ExprKind::Const) {
          uint64_t Shift = E->Operands[1]->ConstVal;
          if (E->Op == NdOp::SUBBYTES) {
            if (Shift >= 8)
              return Result;
            Shift *= 8;
          }
          if (Shift < In &&
              (E->Op == NdOp::INT_LEFT || E->Op == NdOp::INT_RIGHT ||
               E->Op == NdOp::INT_ASHR || E->Op == NdOp::SUBBYTES)) {
            for (unsigned I = 0; I < N; ++I) {
              const int Source = E->Op == NdOp::INT_LEFT ? int(I) - int(Shift)
                                                         : int(I) + int(Shift);
              Result[I] = Source >= 0 && unsigned(Source) < In ? A[Source]
                          : E->Op == NdOp::INT_ASHR            ? A.back()
                                                               : Bit{0};
            }
          }
        }
      }
    }
    Memo.emplace(E, Result);
    return Result;
  }
};

bool mentions(const HighExpr *E, Identity Key) {
  if (!E)
    return false;
  if (E->Kind == ExprKind::Var && highSourceLocalIdentity(E->Var) == Key)
    return true;
  bool Found = false;
  E->forEachChildExpr(
      [&](const ExprPtr &Child) { Found |= mentions(Child.get(), Key); });
  return Found;
}
} // namespace

std::optional<std::string> HighCWriter::bitFieldRead(const HighExpr &Value) {
  if (!bitExpression(&Value))
    return std::nullopt;
  BitValues Values;
  const Bits Result = Values.get(&Value);
  unsigned Count = 0;
  while (Count < Result.size() && Result[Count].Read)
    ++Count;
  if (!Count || llvm::any_of(llvm::drop_begin(Result, Count),
                             [](const Bit &B) { return B.Constant != 0; }))
    return std::nullopt;
  const auto *Read = Result[0].Read;
  for (unsigned I = 0; I < Count; ++I)
    if (Result[I].Read != Read ||
        Result[I].ReadIndex != Result[0].ReadIndex + I)
      return std::nullopt;
  const auto Field = bitFieldAccess(*Read->Operands[0], Read->Type->Size,
                                    Result[0].ReadIndex, Count);
  if (!Field)
    return std::nullopt;
  return typedText(Value, "(uint64_t)(" + *Field + ")", 8, false);
}

void HighCWriter::collectBitFieldStatements(const HighFunc &Function) {
  BitFieldStatements.clear();
  // Availability transport owns both payload and flag assignments. Replacing
  // only a payload here would leave its initialized flag stale.
  if (!DeferredUnknownFlags.empty())
    return;
  std::map<const NdType *, bool> RecordBits;
  const auto HasBits = [&](auto &&Self, const TypeRef &Type) -> bool {
    if (!Type)
      return false;
    if (auto It = RecordBits.find(Type.get()); It != RecordBits.end())
      return It->second;
    RecordBits.emplace(Type.get(), false);
    bool Found = !Type->FieldDisplayBits.empty();
    if (Type->Kind == NdTypeKind::Struct)
      for (const auto &Member : Type->FieldDisplayTypes)
        if (Member && Member->Kind == NdTypeKind::Struct)
          Found |= Self(Self, Member);
    RecordBits[Type.get()] = Found;
    return Found;
  };
  std::set<const HighStmt *> Candidates;
  walkStmts(Function.Body, [&](const HighStmt &S) {
    const HighExpr *Address = S.Kind == StmtKind::Store ? S.StoreAddr.get()
                              : S.Kind == StmtKind::Assign && S.Dst &&
                                      S.Dst->Kind == ExprKind::Load &&
                                      S.Dst->Operands.size() == 1
                                  ? S.Dst->Operands[0].get()
                                  : nullptr;
    if (Address)
      if (auto Base = typedPointerOffset(*Address))
        if (HasBits(HasBits, declaredRecordPointee(*Base->first)))
          Candidates.insert(&S);
  });
  if (Candidates.empty())
    return;
  struct Plan {
    std::string Member;
    Bits Value;
  };
  std::map<const HighStmt *, Plan> Plans;
  std::map<const HighStmt *, Bits> Captures;
  const auto Scan = [&](auto &&Self,
                        const std::vector<HighStmt> &Body) -> void {
    BitValues Values;
    const bool Analyze = llvm::any_of(
        Body, [&](const HighStmt &S) { return Candidates.count(&S); });
    for (const HighStmt &S : Body) {
      const auto Children = [&] {
        if (!S.Body.empty())
          Self(Self, S.Body);
        if (!S.ElseBody.empty())
          Self(Self, S.ElseBody);
        for (const auto &Case : S.Cases)
          Self(Self, Case.Body);
        if (!S.DefaultBody.empty())
          Self(Self, S.DefaultBody);
        for (const auto &Clause : S.EHClauseBodies)
          Self(Self, Clause);
      };
      if (!Analyze) {
        Children();
        continue;
      }
      Values.Memo.clear();
      const HighExpr *Address = nullptr, *Stored = nullptr;
      bool Ordinary = S.MemoryOrdering == NdMemoryOrdering::None &&
                      S.MemoryAddressSpace == NdMemoryAddressSpace::Default;
      if (S.Kind == StmtKind::Store) {
        Address = S.StoreAddr.get();
        Stored = S.StoreVal.get();
      } else if (S.Kind == StmtKind::Assign && S.Dst &&
                 S.Dst->Kind == ExprKind::Load && S.Dst->Operands.size() == 1) {
        Address = S.Dst->Operands[0].get();
        Stored = S.Val.get();
        Ordinary &=
            S.Dst->MemoryOrdering == NdMemoryOrdering::None &&
            S.Dst->MemoryAddressSpace == NdMemoryAddressSpace::Default &&
            width(S.Dst.get()) == width(Stored);
      }
      if (Address && Stored && width(Stored) && Ordinary &&
          bitExpression(Stored) && bitExpression(Address)) {
        const Bits Result = Values.get(Stored);
        uint64_t Changed = 0;
        for (unsigned I = 0; I < Result.size(); ++I) {
          const auto &B = Result[I];
          if (!B.Read || B.ReadIndex != I ||
              !B.Read->Operands[0]->structuralEq(*Address))
            Changed |= uint64_t(1) << I;
        }
        if (!Changed && Candidates.count(&S)) {
          Plans.emplace(&S, Plan{});
        } else if (Candidates.count(&S) && llvm::isShiftedMask_64(Changed)) {
          const unsigned First = llvm::countr_zero(Changed);
          const unsigned Count = llvm::popcount(Changed);
          if (auto Field =
                  bitFieldAccess(*Address, width(Stored) / 8, First, Count)) {
            const Bit &Start = Result[First];
            const bool Constants =
                llvm::all_of(llvm::ArrayRef(Result).slice(First, Count),
                             [](const Bit &B) { return B.Constant >= 0; });
            bool Slice = Start.Value && Start.Value->Kind == ExprKind::Var;
            for (unsigned I = 0; I < Count; ++I)
              Slice &= Result[First + I].Value == Start.Value &&
                       Result[First + I].Index == Start.Index + I;
            if (Constants || Slice)
              Plans.emplace(&S,
                            Plan{*Field, Bits(Result.begin() + First,
                                              Result.begin() + First + Count)});
          }
        }
        // The write invalidates any saved read it could overlap, including
        // memory loads used to calculate the saved address.
        std::map<const HighExpr *, bool> UnchangedReads;
        for (auto &[Key, Bits] : Values.Locals)
          for (Bit &B : Bits) {
            const auto Unchanged = [&](auto &&Check,
                                       const HighExpr *E) -> bool {
              if (E->Kind == ExprKind::Load &&
                  !disjointMemory(*E->Operands[0], *Address, E->Type->Size,
                                  width(Stored) / 8))
                return false;
              return llvm::all_of(E->Operands, [&](const ExprPtr &Child) {
                return Child && Check(Check, Child.get());
              });
            };
            if (B.Read) {
              const auto [It, Fresh] = UnchangedReads.try_emplace(B.Read);
              if (Fresh)
                It->second = Unchanged(Unchanged, B.Read);
              if (!It->second)
                B.Read = nullptr;
            }
          }
        std::erase_if(Values.Locals, [](const auto &Entry) {
          return llvm::none_of(Entry.second,
                               [](const Bit &B) { return B.Read; });
        });
      } else if (S.Kind == StmtKind::Assign && S.Dst &&
                 S.Dst->Kind == ExprKind::Var && bitExpression(S.Val.get()) &&
                 Ordinary) {
        Bits Result = Values.get(S.Val.get());
        const auto Key = highSourceLocalIdentity(S.Dst->Var);
        // A dependency assignment changes both the current address and a
        // captured source expression. Never substitute through that write.
        for (auto It = Values.Locals.begin(); It != Values.Locals.end();) {
          if (It->first == Key || llvm::any_of(It->second, [&](const Bit &B) {
                return mentions(B.Value, Key) ||
                       (B.Read && mentions(B.Read, Key));
              }))
            It = Values.Locals.erase(It);
          else
            ++It;
        }
        if (width(S.Dst.get()) == Result.size() &&
            llvm::any_of(Result,
                         [](const Bit &B) { return B.Read != nullptr; })) {
          Captures.emplace(&S, Result);
          for (unsigned I = 0; I < Result.size(); ++I) {
            Result[I].Value = S.Dst.get();
            Result[I].Index = I;
            if (Result[I].Read && mentions(Result[I].Read, Key))
              Result[I].Read = nullptr;
          }
          Values.Locals[Key] = std::move(Result);
        }
      } else {
        Values.Locals.clear();
      }
      Children();
    }
  };
  Scan(Scan, Function.Body);
  if (Plans.empty())
    return;

  // Recount only this presentation's reads. Remove newly unused pure
  // intermediates by a worklist, then narrow a saved container only when all
  // remaining uses observe exactly one declared field.
  std::map<Identity, unsigned> Uses;
  std::map<Identity, std::vector<const HighStmt *>> Definitions;
  const auto Reads = [&](const HighStmt &S, auto Use) {
    HighExprSet Seen;
    const auto Visit = [&](auto &&Visit, const HighExpr *E) -> void {
      if (!E || !Seen.insert(E).second)
        return;
      if (E->Kind == ExprKind::Var)
        Use(E->Var);
      E->forEachChildExpr(
          [&](const ExprPtr &Child) { Visit(Visit, Child.get()); });
    };
    forEachRhsExpr(S, [&](const ExprPtr &E) { Visit(Visit, E.get()); });
    if (S.Dst && S.Dst->Kind != ExprKind::Var)
      Visit(Visit, S.Dst.get());
  };
  walkStmts(Function.Body, [&](const HighStmt &S) {
    if (S.Kind == StmtKind::Assign && S.Dst && S.Dst->Kind == ExprKind::Var)
      Definitions[highSourceLocalIdentity(S.Dst->Var)].push_back(&S);
    if (const auto It = Plans.find(&S); It != Plans.end()) {
      for (const Bit &B : It->second.Value)
        if (B.Constant < 0 && B.Value && B.Value->Kind == ExprKind::Var)
          ++Uses[highSourceLocalIdentity(B.Value->Var)];
      // The destination's receiver is still evaluated.
      const auto Address =
          S.Kind == StmtKind::Store ? S.StoreAddr : S.Dst->Operands[0];
      HighStmt ReadAddress;
      ReadAddress.Kind = StmtKind::Return;
      ReadAddress.RetVal = Address;
      Reads(ReadAddress,
            [&](const MedVar &V) { ++Uses[highSourceLocalIdentity(V)]; });
    } else
      Reads(S, [&](const MedVar &V) { ++Uses[highSourceLocalIdentity(V)]; });
  });
  std::vector<Identity> Pending;
  for (const auto &[Key, Defs] : Definitions)
    if (!Uses[Key])
      Pending.push_back(Key);
  std::set<const HighStmt *> Removed;
  while (!Pending.empty()) {
    const auto Key = Pending.back();
    Pending.pop_back();
    for (const auto *S : Definitions[Key]) {
      if (!Captures.count(S) || !Removed.insert(S).second)
        continue;
      Reads(*S, [&](const MedVar &V) {
        const auto Input = highSourceLocalIdentity(V);
        if (Uses[Input] && !--Uses[Input])
          Pending.push_back(Input);
      });
      BitFieldStatements[S] = {};
    }
  }
  std::map<Identity, uint64_t> Demands;
  std::set<Identity> OtherUses;
  walkStmts(Function.Body, [&](const HighStmt &S) {
    if (Removed.count(&S))
      return;
    if (const auto It = Plans.find(&S); It != Plans.end()) {
      for (const Bit &B : It->second.Value)
        if (B.Constant < 0 && B.Value && B.Value->Kind == ExprKind::Var)
          Demands[highSourceLocalIdentity(B.Value->Var)] |= uint64_t(1)
                                                            << B.Index;
      // A source local also used in the address cannot be narrowed.
      const auto Address =
          S.Kind == StmtKind::Store ? S.StoreAddr : S.Dst->Operands[0];
      HighStmt ReadAddress;
      ReadAddress.Kind = StmtKind::Return;
      ReadAddress.RetVal = Address;
      Reads(ReadAddress, [&](const MedVar &V) {
        OtherUses.insert(highSourceLocalIdentity(V));
      });
    } else
      Reads(S, [&](const MedVar &V) {
        OtherUses.insert(highSourceLocalIdentity(V));
      });
  });
  std::map<Identity, unsigned> Projected;
  for (const auto &[Key, Mask] : Demands) {
    const auto &Defs = Definitions[Key];
    if (OtherUses.count(Key) || Defs.size() != 1 ||
        !llvm::isShiftedMask_64(Mask))
      continue;
    const auto Found = Captures.find(Defs.front());
    if (Found == Captures.end())
      continue;
    const auto Name = varName(Defs.front()->Dst->Var);
    if (ValueForward.count(Name) || FieldForward.count(Name) ||
        CopyForward.count(Name) || Analysis.DeadStmts.count(Defs.front()) ||
        stmtHiddenFromC(*Defs.front()))
      continue;
    const unsigned First = llvm::countr_zero(Mask),
                   Count = llvm::popcount(Mask);
    const Bits &Value = Found->second;
    if (First + Count > Value.size() || !Value[First].Read)
      continue;
    const auto *Read = Value[First].Read;
    bool Same = true;
    for (unsigned I = 0; I < Count; ++I)
      Same &= Value[First + I].Read == Read &&
              Value[First + I].ReadIndex == Value[First].ReadIndex + I;
    if (!Same)
      continue;
    if (auto Field = bitFieldAccess(*Read->Operands[0], Read->Type->Size,
                                    Value[First].ReadIndex, Count)) {
      BitFieldStatements[Defs.front()] =
          exprStr(*Defs.front()->Dst) + " = " + *Field;
      Projected[Key] = First;
    }
  }
  for (const auto &[S, P] : Plans) {
    if (P.Value.empty()) {
      BitFieldStatements[S] = {};
      continue;
    }
    std::string Value;
    if (llvm::all_of(P.Value, [](const Bit &B) { return B.Constant >= 0; })) {
      uint64_t Constant = 0;
      for (unsigned I = 0; I < P.Value.size(); ++I)
        Constant |= uint64_t(P.Value[I].Constant) << I;
      Value = std::to_string(Constant);
    } else {
      const auto *Source = P.Value[0].Value;
      const auto Projection =
          Projected.find(highSourceLocalIdentity(Source->Var));
      if (Projection != Projected.end() &&
          Projection->second == P.Value[0].Index)
        Value = exprStr(*Source);
      else {
        const uint64_t Mask = llvm::maskTrailingOnes<uint64_t>(P.Value.size());
        Value = "(((uint64_t)(" + exprStr(*Source) + ") >> " +
                std::to_string(P.Value[0].Index) + ") & " +
                std::to_string(Mask) + ")";
      }
    }
    BitFieldStatements[S] = P.Member + " = " + Value;
  }
}
} // namespace neverd
