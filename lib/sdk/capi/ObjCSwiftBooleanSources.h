#ifndef NEVERD_SDK_CAPI_OBJCSWIFTBOOLEANSOURCES_H
#define NEVERD_SDK_CAPI_OBJCSWIFTBOOLEANSOURCES_H

#include "../../loader/Swift/SwiftBooleanProjection.h"
#include "../../loader/Swift/SwiftBooleanSourceBinding.h"
#include "ObjCSourceProjection.h"

#include "neverd/pipeline/NativeSourceHints.h"

namespace neverd::sdk {
inline std::map<va_t, SourceFunctionTypeHint>
nativeBooleanPublicationCallees(const PipelineResult &Result,
                                const MedFunc &Caller) {
  auto NativeCallees = boundNativeBooleanCallees(Caller);
  std::map<va_t, const HighFunc *> HighCallees;
  std::map<va_t, const PipelineFunctionAudit *> CalleeAudits;
  for (const auto &Candidate : Result.HighFuncs) {
    const auto [It, Inserted] =
        HighCallees.emplace(Candidate.Entry, &Candidate);
    if (!Inserted)
      It->second = nullptr;
  }
  for (const auto &Candidate : Result.FunctionAudits) {
    const auto [It, Inserted] =
        CalleeAudits.emplace(Candidate.Entry, &Candidate);
    if (!Inserted)
      It->second = nullptr;
  }
  for (auto It = NativeCallees.begin(); It != NativeCallees.end();) {
    const auto High = HighCallees.find(It->first);
    const auto Audit = CalleeAudits.find(It->first);
    const bool Complete =
        High != HighCallees.end() && High->second &&
        High->second->SourceTypeHint &&
        equalSourceABIs(*High->second->SourceTypeHint, It->second) &&
        Audit != CalleeAudits.end() && Audit->second &&
        Audit->second->Disposition == PipelineFunctionDisposition::Accepted &&
        Audit->second->HasLowIR && Audit->second->HasMedIR &&
        Audit->second->MedIRVerified && Audit->second->DecodedInstructions &&
        Audit->second->DecodedInstructions ==
            Audit->second->LiftedInstructions &&
        Audit->second->DecodeFailures.empty() &&
        Audit->second->UnsupportedInstructions.empty() &&
        Audit->second->TruncatedPaths.empty();
    if (Complete)
      ++It;
    else
      It = NativeCallees.erase(It);
  }
  return NativeCallees;
}

inline bool objCSwiftBooleanSourceCallBound(const HighExpr &Expression,
                                            const BinaryImage &Image,
                                            const PipelineResult &Result,
                                            const HighFunc &Function) {
  if (Expression.Kind != ExprKind::Call || !Expression.SourceCallHint ||
      !isSwiftBooleanSourceBinding(*Expression.SourceCallHint) ||
      Expression.IsIndirectCall || Expression.IntrinsicId != Intrinsic::None ||
      Expression.MemoryOrdering != NdMemoryOrdering::None ||
      Expression.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
      Expression.Operands.size() !=
          Expression.SourceCallHint->Signature.Parameters.size() ||
      !Result.Success || Result.SourceImage != &Image ||
      !Function.SourceTypeHint || Function.DoesNotReturn ||
      Function.StructuredExceptionRegions ||
      Function.UnstructuredExceptionRegions ||
      (Function.ExceptionMetadata &&
       !objc_projection_detail::isPlainUnwind(*Function.ExceptionMetadata)))
    return false;
  const auto &Binding = *Expression.SourceCallHint;
  if (Binding.BooleanResult->FunctionEntry != Function.Entry ||
      Expression.CallAddr != Binding.BooleanResult->Site.StaticTarget ||
      !equalSourceTypes(Expression.Type, Binding.Signature.ReturnType))
    return false;
  for (unsigned I = 0; I != Expression.Operands.size(); ++I) {
    const auto &Argument = Expression.Operands[I];
    if (!Argument || !Argument->Type ||
        Argument->Type->Size != Binding.Signature.Parameters[I].Type->Size ||
        (Argument->Type->Kind != NdTypeKind::Int &&
         Argument->Type->Kind != NdTypeKind::Ptr))
      return false;
  }
  const LowFunc *Low = nullptr;
  const MedFunc *Med = nullptr;
  const PipelineFunctionAudit *Audit = nullptr;
  for (const auto &Candidate : Result.LowFuncs)
    if (Candidate.Entry == Function.Entry) {
      if (Low)
        return false;
      Low = &Candidate;
    }
  for (const auto &Candidate : Result.FunctionAudits)
    if (Candidate.Entry == Function.Entry) {
      if (Audit)
        return false;
      Audit = &Candidate;
    }
  for (const auto &Candidate : Result.MedFuncs)
    if (Candidate.Entry == Function.Entry) {
      if (Med)
        return false;
      Med = &Candidate;
    }
  if (!Low || !Audit ||
      Audit->Disposition != PipelineFunctionDisposition::Accepted ||
      !Audit->HasLowIR || !Audit->HasMedIR || !Audit->MedIRVerified ||
      !Audit->DecodeFailures.empty() ||
      !Audit->UnsupportedInstructions.empty() || !Audit->TruncatedPaths.empty())
    return false;
  std::set<va_t> Instructions;
  for (const auto &Block : Low->Blocks)
    for (const auto &Op : Block.Ops)
      Instructions.insert(Op.Addr);
  if (Instructions.size() != Audit->DecodedInstructions ||
      Instructions.size() != Audit->LiftedInstructions)
    return false;
  if (Function.SourceTypeHint->Origin ==
      SourceFunctionTypeHint::OriginKind::NativeAnalysis) {
    if (!Med)
      return false;
    auto UnboundMed = *Med;
    auto UnboundHigh = Function;
    UnboundMed.SourceTypeHint.reset();
    UnboundMed.SourceParametersBound = false;
    UnboundHigh.SourceTypeHint.reset();
    const bool PairReturn =
        Function.SourceTypeHint->ReturnComponents.size() == 2;
    if (PairReturn) {
      // The bound record supplies only a scalar type seed. Native inference
      // must still prove both complete physical return carriers again.
      const auto &Type = Function.SourceTypeHint->ReturnType;
      if (!Type || Type->Kind != NdTypeKind::Struct || Type->Size != 16 ||
          Type->Fields.size() != 2 ||
          Type->FieldOffsets != std::vector<uint16_t>{0, 8} ||
          !Type->Fields[0] || Type->Fields[0]->Kind != NdTypeKind::Int ||
          Type->Fields[0]->Size != 8 || !Type->Fields[1] ||
          Type->Fields[1]->Kind != NdTypeKind::Int ||
          Type->Fields[1]->Size != 8)
        return false;
      UnboundMed.ReturnType = Type->Fields[0];
      UnboundHigh.ReturnType = Type->Fields[0];
    }
    std::string Diagnostic;
    const auto Inferred = inferNativeSourceTypeHint(
        Image, UnboundMed, UnboundHigh, *Audit, Diagnostic, Low, PairReturn);
    if (!Inferred || !equalSourceABIs(*Inferred, *Function.SourceTypeHint))
      return false;
  }
  // The LowIR proof may cross a source-bound native call while the selected
  // Boolean's upper result bits remain different. Reuse the same MedIR call
  // ABI inventory as native inference, but retain only callees whose current
  // HighIR declaration and complete audit independently agree. The final
  // source graph still validates each callee body and dependency closure.
  const auto NativeCallees = Med ? nativeBooleanPublicationCallees(Result, *Med)
                                 : std::map<va_t, SourceFunctionTypeHint>{};
  const auto Current = qualifySwiftBooleanProjections(
      Image, *Low, *Function.SourceTypeHint, &NativeCallees);
  std::map<SourceCallOccurrenceKey, std::pair<va_t, std::string>> Sites;
  for (const auto &Projection : Current)
    Sites.emplace(Projection.Normalization.Site,
                  std::pair{Projection.Runtime.ImportSlot,
                            Projection.Runtime.ImportName});
  const auto Selected = Sites.find(Binding.BooleanResult->Site);
  if (Selected == Sites.end() ||
      Selected->second !=
          std::pair{Binding.TargetAddress, "_" + Binding.TargetName})
    return false;
  // Count evaluations, not unique expression pointers: sharing one node in
  // two statements cannot reuse this one machine occurrence's evidence.
  size_t Budget = 100000, Matches = 0;
  bool Foreign = false;
  std::set<SourceCallOccurrenceKey> Evaluated;
  std::vector<const HighStmt *> Statements;
  auto Append = [&](const std::vector<HighStmt> &Body) {
    if (Body.size() > Budget - std::min(Budget, Statements.size())) {
      Budget = 0;
      return;
    }
    for (const auto &S : Body)
      Statements.push_back(&S);
  };
  Append(Function.Body);
  while (!Statements.empty() && Budget) {
    --Budget;
    const auto &Statement = *Statements.back();
    Statements.pop_back();
    forEachExpr(Statement, [&](const ExprPtr &Root) {
      std::vector<ExprPtr> Pending{Root};
      while (!Pending.empty() && Budget) {
        --Budget;
        auto E = Pending.back();
        Pending.pop_back();
        if (!E)
          continue;
        if (E->SourceCallHint && E->SourceCallHint->BooleanResult) {
          Matches += E.get() == &Expression;
          const auto &Other = *E->SourceCallHint;
          const auto Site = Sites.find(Other.BooleanResult->Site);
          Foreign |= !isSwiftBooleanSourceBinding(Other) ||
                     E->Kind != ExprKind::Call || E->IsIndirectCall ||
                     E->CallAddr != Other.BooleanResult->Site.StaticTarget ||
                     E->IntrinsicId != Intrinsic::None ||
                     E->MemoryOrdering != NdMemoryOrdering::None ||
                     E->MemoryAddressSpace != NdMemoryAddressSpace::Default ||
                     E->Operands.size() != Other.Signature.Parameters.size() ||
                     !equalSourceTypes(E->Type, Other.Signature.ReturnType) ||
                     Other.BooleanResult->FunctionEntry != Function.Entry ||
                     Site == Sites.end() ||
                     (Site != Sites.end() &&
                      Site->second != std::pair{Other.TargetAddress,
                                                "_" + Other.TargetName}) ||
                     !Evaluated.insert(Other.BooleanResult->Site).second;
          if (!Foreign)
            for (size_t I = 0; I < E->Operands.size(); ++I) {
              const auto &A = E->Operands[I];
              Foreign |=
                  !A || !A->Type ||
                  A->Type->Size != Other.Signature.Parameters[I].Type->Size ||
                  (A->Type->Kind != NdTypeKind::Int &&
                   A->Type->Kind != NdTypeKind::Ptr);
            }
        }
        if (E->Operands.size() > Budget - std::min(Budget, Pending.size())) {
          Budget = 0;
          break;
        }
        Pending.insert(Pending.end(), E->Operands.begin(), E->Operands.end());
      }
    });
    Append(Statement.Body);
    Append(Statement.ElseBody);
    for (const auto &Case : Statement.Cases)
      Append(Case.Body);
    Append(Statement.DefaultBody);
    for (const auto &Body : Statement.EHClauseBodies)
      Append(Body);
  }
  return Budget && Matches == 1 && !Foreign;
}
} // namespace neverd::sdk
#endif
