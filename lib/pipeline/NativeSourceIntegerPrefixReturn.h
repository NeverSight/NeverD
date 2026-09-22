#ifndef NEVERD_PIPELINE_NATIVE_SOURCE_INTEGER_PREFIX_RETURN_H
#define NEVERD_PIPELINE_NATIVE_SOURCE_INTEGER_PREFIX_RETURN_H

#include "neverd/ir/SourceABI.h"
#include "neverd/ir/high/HighSourceFlow.h"

#include <algorithm>
#include <map>
#include <set>
#include <vector>

namespace neverd::detail {

// A source-only candidate: a later bound call can invalidate the upper word
// of an earlier inferred integer result. Retain a defined low word only when
// every return expression supports that projection and at least one explicitly
// carries unknown upper padding. Re-lifting and ordinary publication checks
// must still validate the body and every caller; no upper bytes are invented.
inline bool hasNativeSourceIntegerPrefixReturn(const HighFunc &Function) {
  const auto &Hint = *Function.SourceTypeHint;
  const auto &Location = Hint.ReturnLocation;
  if (Function.DoesNotReturn || Hint.ReturnType->Kind != NdTypeKind::Int ||
      Hint.ReturnType->Size != 8 || !Hint.ReturnComponents.empty() ||
      Location.Kind != SourceABICarrierKind::IntegerRegister ||
      Location.ValueBytes != 8 || Location.ExtendTo32Bits)
    return false;
  size_t Budget = 100000;
  std::map<HighSourceLocalIdentity, std::vector<ExprPtr>> Definitions;
  std::vector<ExprPtr> Returns;
  std::vector<const HighStmt *> Pending;
  for (const auto &Statement : Function.Body)
    Pending.push_back(&Statement);
  for (size_t I = 0; I < Pending.size(); ++I) {
    if (!Budget)
      return false;
    --Budget;
    const auto &S = *Pending[I];
    if (S.Kind == StmtKind::Assign && S.Dst && S.Dst->Kind == ExprKind::Var)
      Definitions[highSourceLocalIdentity(S.Dst->Var)].push_back(S.Val);
    if (S.Kind == StmtKind::Return)
      Returns.push_back(S.RetVal);
    auto Add = [&](const std::vector<HighStmt> &Body) {
      for (const auto &Child : Body)
        Pending.push_back(&Child);
    };
    if (!S.EHClauseBodies.empty())
      return false;
    Add(S.Body);
    Add(S.ElseBody);
    Add(S.DefaultBody);
    for (const auto &Case : S.Cases)
      Add(Case.Body);
    if (Pending.size() > 100000)
      return false;
  }

  bool Partial = false;
  std::set<HighSourceLocalIdentity> Active;
  auto Plain = [](const HighExpr &E) {
    return E.Type && E.Type->Kind == NdTypeKind::Int && E.Type->Size &&
           E.Type->Size <= 8 && E.IntrinsicId == Intrinsic::None &&
           E.IntrinsicOutputs.empty() &&
           E.MemoryOrdering == NdMemoryOrdering::None &&
           E.MemoryAddressSpace == NdMemoryAddressSpace::Default;
  };
  // Only explicit unknown padding, optionally sliced, supplies the reason to
  // shrink a return. An effectful upper expression cannot be discarded here.
  auto Unknown = [&](const ExprPtr &E) {
    if (!E || !Plain(*E) || E->Type->Size != 4)
      return false;
    if (E->Kind == ExprKind::Undef)
      return E->Operands.empty();
    return E->Kind == ExprKind::BinOp && E->Op == NdOp::SUBBYTES &&
           E->Operands.size() == 2 && E->Operands[0] && E->Operands[1] &&
           Plain(*E->Operands[0]) && Plain(*E->Operands[1]) &&
           E->Operands[0]->Kind == ExprKind::Undef &&
           E->Operands[0]->Operands.empty() &&
           E->Operands[1]->Kind == ExprKind::Const &&
           E->Operands[1]->Operands.empty() &&
           E->Operands[1]->ConstVal <= E->Operands[0]->Type->Size &&
           4 <= E->Operands[0]->Type->Size - E->Operands[1]->ConstVal;
  };
  const auto Defined = [&](auto &&Self, const ExprPtr &E, unsigned Bytes,
                           unsigned Depth) -> bool {
    if (!Budget || Depth > 128 || !E || !Plain(*E) || E->Type->Size < Bytes)
      return false;
    --Budget;
    if (E->Kind == ExprKind::Const)
      return E->Operands.empty();
    if (E->Kind == ExprKind::Var && E->Operands.empty()) {
      if (E->Var.Size != E->Type->Size)
        return false;
      if (E->Var.Kind == MedVar::Param)
        return E->Var.Id >= 0 && size_t(E->Var.Id) < Hint.Parameters.size() &&
               equalSourceTypes(E->Type, Hint.Parameters[E->Var.Id].Type);
      const auto Key = highSourceLocalIdentity(E->Var);
      const auto Found = Definitions.find(Key);
      if (Found == Definitions.end() || !Active.insert(Key).second)
        return false;
      bool Valid = !Found->second.empty();
      for (const auto &Value : Found->second)
        Valid &= Self(Self, Value, Bytes, Depth + 1);
      Active.erase(Key);
      return Valid;
    }
    if ((E->Kind == ExprKind::Cast ||
         (E->Kind == ExprKind::UnaryOp &&
          (E->Op == NdOp::INT_ZEXT || E->Op == NdOp::INT_SEXT))) &&
        E->Operands.size() == 1 && E->Operands[0] && Plain(*E->Operands[0])) {
      if (E->Kind == ExprKind::Cast && !equalSourceTypes(E->CastTo, E->Type))
        return false;
      if (E->Kind == ExprKind::UnaryOp &&
          E->Type->Size < E->Operands[0]->Type->Size)
        return false;
      return Self(Self, E->Operands[0],
                  std::min<unsigned>(Bytes, E->Operands[0]->Type->Size),
                  Depth + 1);
    }
    if (E->Kind == ExprKind::BinOp && E->Op == NdOp::CONCAT &&
        E->Type->Size == 8 && Bytes == 4 && E->Operands.size() == 2 &&
        E->Operands[1] && Plain(*E->Operands[1]) &&
        E->Operands[1]->Type->Size == 4 && Unknown(E->Operands[0])) {
      Partial = true;
      return Self(Self, E->Operands[1], 4, Depth + 1);
    }
    if (E->Kind == ExprKind::Call && E->SourceCallHint) {
      const auto &Signature = E->SourceCallHint->Signature;
      std::string Error;
      return !E->SourceCallHint->DoesNotReturn &&
             validateSourceABI(Signature, Error) &&
             equalSourceTypes(E->Type, Signature.ReturnType) &&
             Signature.ReturnComponents.empty() &&
             Signature.ReturnLocation.Kind ==
                 SourceABICarrierKind::IntegerRegister &&
             Signature.ReturnLocation.ValueBytes >= Bytes &&
             E->Operands.size() == sourceABIParameters(Signature).size();
    }
    return false;
  };
  if (Returns.empty())
    return false;
  for (const auto &Return : Returns)
    if (!Return || !Return->Type || Return->Type->Size != 8 ||
        !Defined(Defined, Return, 4, 0))
      return false;
  if (!Partial)
    return false;
  const auto Flow = analyzeHighSourceFlow(Function, true);
  return Flow.Complete && Flow.Items.empty();
}
} // namespace neverd::detail
#endif
