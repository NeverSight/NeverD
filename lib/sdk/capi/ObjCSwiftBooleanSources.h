#ifndef NEVERD_SDK_CAPI_OBJCSWIFTBOOLEANSOURCES_H
#define NEVERD_SDK_CAPI_OBJCSWIFTBOOLEANSOURCES_H

#include "../../loader/Swift/SwiftBooleanProjection.h"
#include "../../loader/Swift/SwiftBooleanSourceBinding.h"
#include "ObjCSourceProjection.h"

namespace neverd::sdk {
inline bool objCSwiftBooleanSourceCallBound(const HighExpr &Expression,
                                            const BinaryImage &Image,
                                            const PipelineResult &Result,
                                            const HighFunc &Function) {
  if (Expression.Kind != ExprKind::Call || !Expression.SourceCallHint ||
      !isSwiftBooleanSourceBinding(*Expression.SourceCallHint) ||
      Expression.IsIndirectCall || Expression.IntrinsicId != Intrinsic::None ||
      Expression.MemoryOrdering != NdMemoryOrdering::None ||
      Expression.MemoryAddressSpace != NdMemoryAddressSpace::Default ||
      Expression.Operands.size() != 5 || !Result.Success ||
      Result.SourceImage != &Image || !Function.SourceTypeHint ||
      Function.DoesNotReturn || Function.StructuredExceptionRegions ||
      Function.UnstructuredExceptionRegions ||
      (Function.ExceptionMetadata &&
       !objc_projection_detail::isPlainUnwind(*Function.ExceptionMetadata)))
    return false;
  const auto &Binding = *Expression.SourceCallHint;
  if (Binding.BooleanResult->FunctionEntry != Function.Entry ||
      Expression.CallAddr != Binding.BooleanResult->Site.StaticTarget ||
      !equalSourceTypes(Expression.Type, Binding.Signature.ReturnType))
    return false;
  for (unsigned I = 0; I != 5; ++I) {
    const auto &Argument = Expression.Operands[I];
    if (!Argument || !Argument->Type ||
        Argument->Type->Size != Binding.Signature.Parameters[I].Type->Size ||
        (Argument->Type->Kind != NdTypeKind::Int &&
         Argument->Type->Kind != NdTypeKind::Ptr))
      return false;
  }
  const LowFunc *Low = nullptr;
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
  const auto Current =
      qualifySwiftBooleanProjection(Image, *Low, *Function.SourceTypeHint);
  if (!Current || Current->Runtime.ImportSlot != Binding.TargetAddress ||
      Current->Normalization.Site < Binding.BooleanResult->Site ||
      Binding.BooleanResult->Site < Current->Normalization.Site)
    return false;
  // Count evaluations, not unique expression pointers: sharing one node in
  // two statements cannot reuse this one machine occurrence's evidence.
  size_t Budget = 100000, Matches = 0;
  bool Foreign = false;
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
          Foreign |= E.get() != &Expression;
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
