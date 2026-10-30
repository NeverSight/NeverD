//===- MedMutableToHigh.cpp - Explicit mutable HighIR lowering ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "MedMutableToHigh.h"

#include "neverd/ir/SourceABI.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/ir/med/MedMutableSource.h"

#include <algorithm>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace neverd::detail {
namespace {

ExprPtr storage(const MedVar &Value) {
  if (Value.isConst())
    // This path has no image relocation: retain the literal's complete bits,
    // as the mutable LLVM emitter does, without inferring a symbol or frame.
    return HighExpr::makeConst(Value.ConstVal, Value.Size,
                               ConstantAddressProvenance::Scalar);
  MedVar Local;
  Local.Kind = MedVar::Temp;
  Local.Id = Value.Id;
  Local.Size = Value.Size;
  Local.TheArch = Value.TheArch;
  return HighExpr::makeVar(Local, NdType::makeInt(Value.Size, false));
}

ExprPtr integerCast(ExprPtr Value, unsigned Size) {
  auto Cast = std::make_shared<HighExpr>();
  Cast->Kind = ExprKind::Cast;
  Cast->Type = Cast->CastTo = NdType::makeInt(Size, false);
  Cast->Operands.push_back(std::move(Value));
  return Cast;
}

ExprPtr scalarExpression(const MedOp &Op) {
  if (Op.Opcode == NdOp::COPY)
    return storage(Op.Inputs[0]);
  if (Op.Opcode == NdOp::LOAD)
    return HighExpr::makeLoad(storage(Op.Inputs[0]),
                              NdType::makeInt(Op.Output.Size, false));
  ExprPtr Value;
  if (Op.NumInputs == 1)
    Value = HighExpr::makeUnary(Op.Opcode, storage(Op.Inputs[0]));
  else {
    Value = HighExpr::makeBinop(Op.Opcode, storage(Op.Inputs[0]),
                                storage(Op.Inputs[1]));
    if (Op.Opcode == NdOp::SELECT)
      Value->Operands.push_back(storage(Op.Inputs[2]));
  }
  // Ordinary arithmetic keeps its input width before the assignment writes
  // the output width. These operations explicitly select a new result width.
  if (Op.Opcode == NdOp::INT_ZEXT || Op.Opcode == NdOp::INT_SEXT ||
      Op.Opcode == NdOp::CONCAT || Op.Opcode == NdOp::SUBBYTES ||
      Op.Opcode == NdOp::SELECT)
    Value->Type = NdType::makeInt(Op.Output.Size, false);
  if (Op.Opcode == NdOp::INT_AND)
    Value->Type =
        NdType::makeInt(std::max(Op.Inputs[0].Size, Op.Inputs[1].Size), false);
  return Value;
}

void parameters(HighFunc &High, const MedFunc &Med, Arch Architecture) {
  if (Med.SourceTypeHint) {
    const auto &Hint = *Med.SourceTypeHint;
    const auto &TRI = getTargetRegInfo(Architecture);
    std::string Error;
    if (!validateSourceABI(Hint, Error) || !Hint.HasExplicitABI ||
        Hint.Architecture != Architecture ||
        Hint.Convention != SourceFunctionTypeHint::ConventionKind::C ||
        !equalSourceTypes(Hint.ReturnType, Med.ReturnType) ||
        !Hint.ReturnComponents.empty() ||
        Hint.Parameters.size() != Med.Params.size() ||
        (Med.ReturnType->Kind != NdTypeKind::Void &&
         (Hint.ReturnLocation.Kind != SourceABICarrierKind::IntegerRegister ||
          Hint.ReturnLocation.RegisterOffset != TRI.IntReturnReg ||
          Hint.ReturnLocation.ValueBytes != Med.ReturnType->Size)))
      throw std::runtime_error("mutable HighIR requires a scalar source ABI");
    for (size_t I = 0; I < Hint.Parameters.size(); ++I) {
      const auto &P = Hint.Parameters[I];
      const auto &Carrier = Med.Params[I];
      if (P.TheRole != SourceParameterTypeHint::Role::Ordinary ||
          P.IndirectByValue || !P.Components.empty() || !P.Type ||
          (P.Type->Kind != NdTypeKind::Int &&
           P.Type->Kind != NdTypeKind::Ptr) ||
          P.Type->Size != Carrier.Size ||
          P.Location.Kind != SourceABICarrierKind::IntegerRegister ||
          P.Location.RegisterOffset != Carrier.RegOff ||
          P.Location.ValueBytes != Carrier.Size)
        throw std::runtime_error(
            "mutable HighIR source parameter disagrees with its carrier");
      High.Params.push_back({P.Name, P.Type, Carrier.RegOff, Carrier.Id});
    }
    High.SourceTypeHint = Hint;
    return;
  }
  for (size_t I = 0; I < Med.Params.size(); ++I) {
    const auto &P = Med.Params[I];
    TypeRef Type = I < Med.TypedParams.size() ? Med.TypedParams[I].Type
                                              : NdType::makeInt(P.Size, false);
    High.Params.push_back({"arg" + std::to_string(I), Type, P.RegOff, P.Id});
  }
}

} // namespace

HighFunc
lowerMedMutableToHigh(const MedFunc &Med, Arch Architecture,
                      const std::function<void(const MedOp &, const ExprPtr &)>
                          &ObserveExpression,
                      const std::function<void(const MedOp &, const HighStmt &)>
                          &ObserveStatement) {
  std::string Error;
  auto Plan = analyzeMedMutableSource(Med, Architecture, &Error);
  if (!Plan)
    throw std::runtime_error(Error);
  HighFunc High;
  High.Entry = Med.Entry;
  High.Name = Med.Name;
  High.ReturnType = Med.ReturnType;
  High.ReturnsNoValue = Med.ReturnsNoValue;
  High.EntryKind = Med.EntryKind;
  parameters(High, Med, Architecture);

  std::map<uint64_t, size_t> ParameterByRegister;
  for (size_t I = 0; I < Med.Params.size(); ++I)
    ParameterByRegister.emplace(Med.Params[I].RegOff, I);
  for (const auto &V : Plan->Variables) {
    if (!Plan->EntryValues.count(V.Id) && V.Kind != MedVar::Param)
      continue;
    const auto Found = ParameterByRegister.find(V.RegOff);
    if (Found == ParameterByRegister.end())
      throw std::runtime_error(
          "mutable HighIR source reads an unbound entry register");
    const size_t I = Found->second;
    MedVar Parameter = Med.Params[I];
    Parameter.Kind = MedVar::Param;
    Parameter.Id = static_cast<int>(I);
    Parameter.RenameTag = -1;
    HighStmt Seed;
    Seed.Kind = StmtKind::Assign;
    Seed.Dst = storage(V);
    Seed.Val =
        integerCast(HighExpr::makeVar(Parameter, High.Params[I].Type), V.Size);
    Seed.KeepsName = true;
    // Seeds precede the entry label, so a backedge cannot restore arguments.
    High.Body.push_back(std::move(Seed));
  }

  for (const auto &B : Med.Blocks) {
    HighStmt Mark;
    Mark.Kind = StmtKind::Nop;
    Mark.Addr = B.StartAddr;
    High.Body.push_back(std::move(Mark));
    std::optional<MedVar> PreviousCopy;
    for (const auto &Op : B.Ops) {
      if (Op.Dead || Op.Opcode == NdOp::NOP)
        continue;
      HighStmt S;
      // Only block entries define labels. An original instruction address
      // can occur in several specialized blocks and is not a CFG identity.
      switch (Op.Opcode) {
      case NdOp::BRANCH:
        S.Kind = StmtKind::Goto;
        S.GotoTarget = Op.Inputs[0].ConstVal;
        break;
      case NdOp::COND_BR: {
        S.Kind = StmtKind::If;
        S.Cond = storage(Op.Inputs[1]);
        HighStmt Taken;
        Taken.Kind = StmtKind::Goto;
        Taken.GotoTarget = Op.Inputs[0].ConstVal;
        S.Body.push_back(std::move(Taken));
        break;
      }
      case NdOp::RETURN:
        S.Kind = StmtKind::Return;
        if (Plan->ReturnValue)
          S.RetVal = storage(*Plan->ReturnValue);
        break;
      case NdOp::STORE:
        S.Kind = StmtKind::Store;
        S.StoreAddr = storage(Op.Inputs[0]);
        S.StoreVal = storage(Op.Inputs[1]);
        break;
      case NdOp::INTRINSIC: {
        // The shared mutable plan admits only explicit alignment checks.
        const auto ID = static_cast<Intrinsic>(Op.Inputs[0].ConstVal);
        std::vector<ExprPtr> Arguments;
        for (unsigned I = 1; I < Op.NumInputs; ++I)
          Arguments.push_back(storage(Op.Inputs[I]));
        S.Kind = StmtKind::Call;
        S.CallExpr =
            HighExpr::makeCall(intrinsicName(ID), 0, std::move(Arguments));
        S.CallExpr->IntrinsicId = ID;
        S.CallExpr->Type = NdType::makeVoid();
        S.CallExpr->MemoryOrdering = Op.MemoryOrdering;
        S.CallExpr->MemoryAddressSpace = Op.MemoryAddressSpace;
        break;
      }
      default:
        S.Kind = StmtKind::Assign;
        S.Dst = storage(Op.Output);
        S.Val = scalarExpression(Op);
        S.KeepsName = true;
        break;
      }
      // Adjacent plain copies to one local have no intervening observer. A
      // new copy that does not read that local supersedes the earlier one.
      // Loads, arithmetic and control remain explicit, including dead reads.
      if (Op.Opcode == NdOp::COPY && PreviousCopy &&
          *PreviousCopy == Op.Output && Op.Inputs[0] != Op.Output)
        High.Body.pop_back();
      PreviousCopy =
          Op.Opcode == NdOp::COPY ? std::optional(Op.Output) : std::nullopt;
      if (Op.Addr != InvalidVA && Op.OriginSeq >= 0) {
        const ExprPtr &Expression = S.Val    ? S.Val
                                    : S.Cond ? S.Cond
                                             : S.CallExpr;
        if (ObserveExpression && Expression)
          ObserveExpression(Op, Expression);
        if (ObserveStatement)
          ObserveStatement(Op, S);
      }
      High.Body.push_back(std::move(S));
      if (Op.Opcode == NdOp::COND_BR) {
        HighStmt Other;
        Other.Kind = StmtKind::Goto;
        for (int ID : B.Succs)
          if (Med.Blocks[ID].StartAddr != Op.Inputs[0].ConstVal)
            Other.GotoTarget = Med.Blocks[ID].StartAddr;
        High.Body.push_back(std::move(Other));
      }
    }
    const auto Last = B.Ops.back().Opcode;
    if (Last != NdOp::BRANCH && Last != NdOp::COND_BR && Last != NdOp::RETURN) {
      // The shared plan validated exactly one successor for this boundary.
      // Emit that edge even when the destination is not the next block.
      HighStmt Next;
      Next.Kind = StmtKind::Goto;
      Next.GotoTarget = Med.Blocks[B.Succs.front()].StartAddr;
      High.Body.push_back(std::move(Next));
    }
  }
  return High;
}

} // namespace neverd::detail
