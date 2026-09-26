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
  if (LHS->Type)
    E->Type = LHS->Type;
  return E;
}

ExprPtr HighExpr::makeUnary(NdOp Op, ExprPtr Operand) {
  auto E = std::make_shared<HighExpr>();
  E->Kind = ExprKind::UnaryOp;
  E->Op = Op;
  E->Operands.push_back(Operand);
  if (Operand->Type)
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

bool isTerminatingHighCall(const ExprPtr &Expression) {
  if (isNonReturningSourceCall(Expression))
    return true;
  return Expression && Expression->Kind == ExprKind::Call &&
         !Expression->IsIndirectCall && !Expression->SourceCallHint &&
         Expression->Operands.empty() && Expression->IntrinsicOutputs.empty() &&
         Expression->MemoryAddressSpace == NdMemoryAddressSpace::Default &&
         isUnconditionalTrapIntrinsic(Expression->IntrinsicId);
}

bool HighExpr::hasOrderedMemoryAccess() const {
  std::unordered_set<const HighExpr *> Seen;
  std::vector<const HighExpr *> Work{this};
  while (!Work.empty()) {
    const HighExpr *Expr = Work.back();
    Work.pop_back();
    if (!Expr || !Seen.insert(Expr).second)
      continue;
    // A target address space is not itself an atomic ordering, but the HighIR
    // cleanup passes use this query as their general "must preserve the memory
    // boundary" gate.  Treat FS/GS accesses conservatively here so an
    // optimization keyed only by the numeric offset cannot merge or discard
    // them as ordinary address-space-zero memory.
    if (Expr->MemoryOrdering != NdMemoryOrdering::None ||
        Expr->MemoryAddressSpace != NdMemoryAddressSpace::Default)
      return true;
    Expr->forEachChildExpr(
        [&](const ExprPtr &Operand) { Work.push_back(Operand.get()); });
  }
  return false;
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
