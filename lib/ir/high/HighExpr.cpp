//===- HighExpr.cpp - HighExpr factories and free helpers -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// HighExpr static factory methods and free helper functions (intrinsicName
/// etc.).  Display/print logic lives in HighIRPrint.cpp.
///
//===----------------------------------------------------------------------===//

#include "neverd/ir/SourceABI.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/high/MedToHigh.h"
#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/ir/intrinsics/X86Interrupts.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"

#include <functional>
#include <map>
#include <set>

namespace neverd {

bool isSyntheticEntryStackPointer(const MedVar &Value, const HighFunc &Function,
                                  Arch Architecture) {
  return (Function.FrameSize > 0 || Function.FrameHeadroom > 0) &&
         Value.Kind == MedVar::Reg && Value.RenameTag < 0 &&
         Value.SSAVer == 0 &&
         Value.RegOff == getTargetRegInfo(Architecture).StackPointer;
}

//===----------------------------------------------------------------------===//
// intrinsic helpers
//===----------------------------------------------------------------------===//

Intrinsic intrinsicId(const MedOp &Op) {
  if (Op.NumInputs < 1)
    return Intrinsic::None;
  auto &In = Op.Inputs[0];
  if (!In.isConst())
    return Intrinsic::None;
  return static_cast<Intrinsic>(In.ConstVal);
}

std::string intrinsicName(const MedOp &Op) {
  auto Id = intrinsicId(Op);
  if (Id == Intrinsic::None)
    return {};
  return intrinsicName(Id);
}

//===----------------------------------------------------------------------===//
// HighExpr factories
//===----------------------------------------------------------------------===//

ExprPtr HighExpr::makeVar(MedVar V, TypeRef Ty) {
  auto E = std::make_shared<HighExpr>();
  E->Kind = ExprKind::Var;
  E->Var = V;
  E->Type = Ty ? Ty : NdType::makeInt(V.Size);
  return E;
}

ExprPtr HighExpr::makeConst(uint64_t Val, uint16_t Size,
                            ConstantAddressProvenance Provenance,
                            uint64_t AddressOwner) {
  auto E = std::make_shared<HighExpr>();
  E->Kind = ExprKind::Const;
  E->ConstVal = Val;
  E->ConstProvenance = Provenance;
  E->AddressOwnerVA = AddressOwner;
  E->Type = NdType::makeInt(Size, false);
  return E;
}

ExprPtr HighExpr::makeUndef(uint16_t Size) {
  auto E = std::make_shared<HighExpr>();
  E->Kind = ExprKind::Undef;
  E->Type = NdType::makeInt(Size, false);
  return E;
}

ExprPtr HighExpr::makeBitCast(ExprPtr Value, TypeRef Type) {
  if (!Value || !Value->Type || !Type || !Type->Size ||
      Value->Type->Size != Type->Size)
    return makeUndef(Type ? Type->Size : 0);
  // Inverse bit reinterpretations cancel without any numeric conversion.
  if (Value->Kind == ExprKind::BitCast && Value->Operands.size() == 1 &&
      Value->Operands[0] && Value->Operands[0]->Type &&
      Value->Operands[0]->Type->str() == Type->str())
    return Value->Operands[0];
  auto E = std::make_shared<HighExpr>();
  E->Kind = ExprKind::BitCast;
  E->Type = std::move(Type);
  E->Operands.push_back(std::move(Value));
  return E;
}

ExprPtr HighExpr::makeRecord(TypeRef Type, std::vector<ExprPtr> Leaves) {
  const auto Members = sourceAggregateMembers(Type);
  if (Members.empty() || Members.size() != Leaves.size())
    return makeUndef(Type ? Type->Size : 0);
  for (size_t I = 0; I < Members.size(); ++I)
    if (!Leaves[I] || !equalSourceTypes(Members[I].Type, Leaves[I]->Type))
      return makeUndef(Type->Size);
  size_t Index = 0;
  std::function<ExprPtr(const TypeRef &)> Build = [&](const TypeRef &T) {
    if (T->Kind != NdTypeKind::Struct)
      return Leaves[Index++];
    auto E = std::make_shared<HighExpr>();
    E->Kind = ExprKind::Record;
    E->Type = T;
    for (const auto &Field : T->Fields)
      E->Operands.push_back(Build(Field));
    return E;
  };
  return Build(Type);
}

ExprPtr HighExpr::makeRecordField(ExprPtr Record, uint16_t Offset,
                                  uint16_t Bytes) {
  if (!Record || sourceAggregateMembers(Record->Type).empty())
    return makeUndef(Bytes);
  while (Record->Type->Kind == NdTypeKind::Struct) {
    const auto &T = Record->Type;
    size_t Index = 0;
    for (; Index < T->Fields.size(); ++Index)
      if (Offset >= T->FieldOffsets[Index] &&
          Offset - T->FieldOffsets[Index] + Bytes <= T->Fields[Index]->Size)
        break;
    if (Index == T->Fields.size())
      return makeUndef(Bytes);
    Offset -= T->FieldOffsets[Index];
    if (Record->Kind == ExprKind::Record &&
        Record->Operands.size() == T->Fields.size()) {
      if (!Record->Operands[Index] ||
          !equalSourceTypes(Record->Operands[Index]->Type, T->Fields[Index]))
        return makeUndef(Bytes);
      Record = Record->Operands[Index];
    } else {
      auto E = std::make_shared<HighExpr>();
      E->Kind = ExprKind::Field;
      E->Type = T->Fields[Index];
      E->ConstVal = Index;
      E->Operands.push_back(std::move(Record));
      Record = std::move(E);
    }
  }
  return !Offset && Record->Type->Size == Bytes ? Record : makeUndef(Bytes);
}

ExprPtr HighExpr::makeBinop(NdOp Op, ExprPtr LHS, ExprPtr RHS) {
  auto E = std::make_shared<HighExpr>();
  E->Kind = ExprKind::BinOp;
  E->Op = Op;
  E->Operands.push_back(LHS);
  E->Operands.push_back(RHS);
  switch (Op) {
  case NdOp::INT_EQUAL:
  case NdOp::INT_NOTEQUAL:
  case NdOp::INT_LESS:
  case NdOp::INT_SLESS:
  case NdOp::INT_LESSEQUAL:
  case NdOp::INT_SLESSEQUAL:
  case NdOp::INT_CARRY:
  case NdOp::INT_SOVF:
  case NdOp::INT_SBOR:
  case NdOp::BOOL_AND:
  case NdOp::BOOL_OR:
  case NdOp::BOOL_XOR:
  case NdOp::FLOAT_EQUAL:
  case NdOp::FLOAT_NOTEQUAL:
  case NdOp::FLOAT_LESS:
  case NdOp::FLOAT_LESSEQUAL:
    E->Type = NdType::makeInt(1, false);
    break;
  default:
    break;
  }
  if (!E->Type && LHS->Type)
    E->Type = LHS->Type;
  return E;
}

ExprPtr HighExpr::makeUnary(NdOp Op, ExprPtr Operand) {
  auto E = std::make_shared<HighExpr>();
  E->Kind = ExprKind::UnaryOp;
  E->Op = Op;
  E->Operands.push_back(Operand);
  if (Op == NdOp::BOOL_NOT || Op == NdOp::FLOAT_ISNAN)
    E->Type = NdType::makeInt(1, false);
  else if (Operand->Type)
    E->Type = Operand->Type;
  return E;
}

ExprPtr HighExpr::makeLoad(ExprPtr Addr, TypeRef Ty,
                           NdMemoryOrdering MemoryOrdering,
                           NdMemoryAddressSpace MemoryAddressSpace) {
  auto E = std::make_shared<HighExpr>();
  E->Kind = ExprKind::Load;
  E->MemoryOrdering = MemoryOrdering;
  E->MemoryAddressSpace = MemoryAddressSpace;
  E->Operands.push_back(Addr);
  E->Type = Ty;
  return E;
}

bool isNonReturningSourceCall(const ExprPtr &Expression) {
  // A proven runtime binding emits a direct source call even if the machine
  // reached it through an import slot. Native indirectness is not the emitted
  // dispatch contract; unbound indirect calls have no such source effect.
  return Expression && Expression->Kind == ExprKind::Call &&
         Expression->SourceCallHint &&
         Expression->SourceCallHint->DoesNotReturn &&
         Expression->IntrinsicId == Intrinsic::None &&
         Expression->IntrinsicOutputs.empty() &&
         Expression->MemoryOrdering == NdMemoryOrdering::None &&
         Expression->MemoryAddressSpace == NdMemoryAddressSpace::Default;
}

/// An `int N` whose vector never returns (X86Interrupts.def), as MedIR's
/// isArchitecturalNoReturn also decides.
static bool isX86NoReturnInterruptCall(const HighExpr &Expression) {
  if (Expression.IntrinsicId != Intrinsic::IntN ||
      Expression.Operands.empty() || !Expression.Operands[0])
    return false;
  const HighExpr *Vector = Expression.Operands[0].get();
  while (Vector && Vector->Operands.size() == 1 &&
         (Vector->Kind == ExprKind::Cast ||
          (Vector->Kind == ExprKind::UnaryOp &&
           (Vector->Op == NdOp::INT_ZEXT || Vector->Op == NdOp::INT_SEXT))))
    Vector = Vector->Operands[0].get();
  return Vector && Vector->Kind == ExprKind::Const &&
         isX86NoReturnInterrupt(Vector->ConstVal);
}

bool isTerminatingHighCall(const ExprPtr &Expression) {
  if (isNonReturningSourceCall(Expression))
    return true;
  if (Expression && Expression->Kind == ExprKind::Call &&
      !Expression->IsIndirectCall && !Expression->SourceCallHint &&
      isX86NoReturnInterruptCall(*Expression))
    return true;
  return Expression && Expression->Kind == ExprKind::Call &&
         !Expression->IsIndirectCall && !Expression->SourceCallHint &&
         Expression->Operands.empty() && Expression->IntrinsicOutputs.empty() &&
         Expression->MemoryAddressSpace == NdMemoryAddressSpace::Default &&
         isUnconditionalTrapIntrinsic(Expression->IntrinsicId);
}

ExprPtr withMergedCallSites(const ExprPtr &Kept, const ExprPtr &Dropped) {
  auto Sites = [](const ExprPtr &Root) {
    std::set<va_t> Found;
    HighExprSet Seen;
    std::vector<const HighExpr *> Work{Root.get()};
    while (!Work.empty()) {
      const HighExpr *E = Work.back();
      Work.pop_back();
      if (!E || !Seen.insert(E).second)
        continue;
      if (E->Kind == ExprKind::Call)
        Found.insert(E->CallSiteAddr);
      E->forEachChildExpr([&](const ExprPtr &C) { Work.push_back(C.get()); });
    }
    return Found;
  };
  if (Sites(Kept) == Sites(Dropped))
    return Kept;
  // Copy the calls and what holds them; a node shared within the value stays
  // shared.
  std::map<const HighExpr *, ExprPtr> Copies;
  std::function<ExprPtr(const ExprPtr &)> Forget = [&](const ExprPtr &E) {
    if (!E)
      return E;
    if (const auto It = Copies.find(E.get()); It != Copies.end())
      return It->second;
    Copies.emplace(E.get(), E);
    bool Changed = E->Kind == ExprKind::Call && E->CallSiteAddr != 0;
    std::vector<ExprPtr> Operands;
    Operands.reserve(E->Operands.size());
    for (const ExprPtr &Operand : E->Operands) {
      Operands.push_back(Forget(Operand));
      Changed |= Operands.back() != Operand;
    }
    ExprPtr Target = Forget(E->IndirectTarget);
    Changed |= Target != E->IndirectTarget;
    if (!Changed)
      return E;
    auto Copy = std::make_shared<HighExpr>(*E);
    Copy->Operands = std::move(Operands);
    Copy->IndirectTarget = std::move(Target);
    if (Copy->Kind == ExprKind::Call)
      Copy->CallSiteAddr = 0;
    return Copies[E.get()] = Copy;
  };
  return Forget(Kept);
}

static bool hasMemoryBoundary(const HighExpr &Expr) {
  // A target address space is not itself an atomic ordering, but cleanup
  // must also preserve FS/GS boundaries instead of treating their numeric
  // offsets as ordinary address-space-zero memory.
  return Expr.MemoryOrdering != NdMemoryOrdering::None ||
         Expr.MemoryAddressSpace != NdMemoryAddressSpace::Default;
}

bool HighExpr::hasOrderedMemoryAccess() const {
  HighExprSet Seen;
  std::vector<const HighExpr *> Work{this};
  while (!Work.empty()) {
    const HighExpr *Expr = Work.back();
    Work.pop_back();
    if (!Expr || !Seen.insert(Expr).second)
      continue;
    if (hasMemoryBoundary(*Expr))
      return true;
    Expr->forEachChildExpr(
        [&](const ExprPtr &Operand) { Work.push_back(Operand.get()); });
  }
  return false;
}

HighExprSet findOrderedMemoryAncestors(const std::vector<ExprPtr> &Roots) {
  llvm::DenseMap<const HighExpr *, llvm::SmallVector<const HighExpr *, 2>>
      Parents;
  std::vector<const HighExpr *> Nodes, Ordered;
  for (const auto &Root : Roots)
    if (Root && Parents.try_emplace(Root.get()).second)
      Nodes.push_back(Root.get());
  // Record every edge before propagating effects. A recursive memo can mark
  // a shared cycle pure before discovering the ordered access it reaches.
  for (size_t I = 0; I < Nodes.size(); ++I) {
    const HighExpr *Expr = Nodes[I];
    if (hasMemoryBoundary(*Expr))
      Ordered.push_back(Expr);
    Expr->forEachChildExpr([&](const ExprPtr &Child) {
      auto [It, Added] = Parents.try_emplace(Child.get());
      It->second.push_back(Expr);
      if (Added)
        Nodes.push_back(Child.get());
    });
  }
  HighExprSet Result(Ordered.begin(), Ordered.end());
  for (size_t I = 0; I < Ordered.size(); ++I)
    for (const HighExpr *Parent : Parents.find(Ordered[I])->second)
      if (Result.insert(Parent).second)
        Ordered.push_back(Parent);
  return Result;
}

ExprPtr HighExpr::makeCall(const std::string &Target, va_t Addr,
                           std::vector<ExprPtr> Args) {
  auto E = std::make_shared<HighExpr>();
  E->Kind = ExprKind::Call;
  E->CallTarget = Target;
  E->CallAddr = Addr;
  E->Operands = std::move(Args);
  return E;
}

} // namespace neverd
