#ifndef NEVERD_SDK_CAPI_OBJCIMMUTABLENATIVESOURCES_H
#define NEVERD_SDK_CAPI_OBJCIMMUTABLENATIVESOURCES_H

#include "ObjCNativeSourceCallCallees.h"

#include "neverd/loader/MachO/ImmutableNativeCalls.h"

namespace neverd::sdk {
namespace immutable_native_source_detail {
struct CurrentFunction {
  const LowFunc *Low = nullptr;
  const MedFunc *Med = nullptr;
  const HighFunc *High = nullptr;
  const PipelineFunctionAudit *Audit = nullptr;
};

inline std::optional<CurrentFunction>
currentFunction(const PipelineResult &Result, va_t Entry) {
  CurrentFunction Found;
  for (const auto &Candidate : Result.LowFuncs)
    if (Candidate.Entry == Entry) {
      if (Found.Low)
        return std::nullopt;
      Found.Low = &Candidate;
    }
  for (const auto &Candidate : Result.MedFuncs)
    if (Candidate.Entry == Entry) {
      if (Found.Med)
        return std::nullopt;
      Found.Med = &Candidate;
    }
  for (const auto &Candidate : Result.HighFuncs)
    if (Candidate.Entry == Entry) {
      if (Found.High)
        return std::nullopt;
      Found.High = &Candidate;
    }
  for (const auto &Candidate : Result.FunctionAudits)
    if (Candidate.Entry == Entry) {
      if (Found.Audit)
        return std::nullopt;
      Found.Audit = &Candidate;
    }
  if (!Found.Low || !Found.Med || !Found.High || !Found.Audit ||
      !Found.Low->hasCompleteLiftCoverage() || !Found.Med->SourceTypeHint ||
      !Found.High->SourceTypeHint || !Found.Med->SourceParametersBound ||
      Found.Med->DoesNotReturn || Found.High->DoesNotReturn ||
      Found.High->StructuredExceptionRegions ||
      Found.High->UnstructuredExceptionRegions ||
      !equalSourceABIs(*Found.Med->SourceTypeHint,
                       *Found.High->SourceTypeHint) ||
      Found.Audit->Disposition != PipelineFunctionDisposition::Accepted ||
      !Found.Audit->HasLowIR || !Found.Audit->HasMedIR ||
      !Found.Audit->MedIRVerified || !Found.Audit->DecodedInstructions ||
      Found.Audit->DecodedInstructions != Found.Low->DecodedInstructionCount ||
      Found.Audit->LiftedInstructions != Found.Low->LiftedInstructionCount ||
      Found.Audit->DecodedInstructions != Found.Audit->LiftedInstructions ||
      !Found.Audit->DecodeFailures.empty() ||
      !Found.Audit->UnsupportedInstructions.empty() ||
      !Found.Audit->TruncatedPaths.empty())
    return std::nullopt;
  return Found;
}
} // namespace immutable_native_source_detail

inline bool objCImmutableNativeSourceCallBound(const HighExpr &Expression,
                                               const BinaryImage &Image,
                                               const PipelineResult &Result,
                                               const HighFunc &Function) {
  if (!isImmutableNativeSourceCall(Expression, Function.Entry, Image.Arch) ||
      !Result.Success || Result.SourceImage != &Image ||
      !Function.SourceTypeHint || Function.StructuredExceptionRegions ||
      Function.UnstructuredExceptionRegions || Function.DoesNotReturn)
    return false;
  const auto Caller =
      immutable_native_source_detail::currentFunction(Result, Function.Entry);
  if (!Caller ||
      !equalSourceABIs(*Caller->High->SourceTypeHint, *Function.SourceTypeHint))
    return false;
  auto Callees = nativePublicationCallees(Result, *Caller->Med);
  // Neither a dependency inventory nor a persisted Med binding supplies a
  // callee ABI. Require current complete Low/Med/High and audit agreement for
  // every indirect callee that can authorize this caller's source projection.
  for (auto It = Callees.begin(); It != Callees.end();) {
    const auto Callee =
        immutable_native_source_detail::currentFunction(Result, It->first);
    if (!Callee || !equalSourceABIs(*Callee->High->SourceTypeHint, It->second))
      It = Callees.erase(It);
    else
      ++It;
  }
  const auto Current =
      buildImmutableNativeCallHints(Image, *Caller->Low, Callees);
  const auto MatchesCurrent = [&](const SourceCallTypeHint &Binding) {
    if (!isImmutableNativeCallHint(Binding, Function.Entry, Image.Arch))
      return false;
    const auto Found =
        Current.find(Binding.ImmutableNativeCall->Site.Instruction);
    return Found != Current.end() &&
           Found->second.ImmutableNativeCall == Binding.ImmutableNativeCall &&
           equalSourceABIs(Found->second.Signature, Binding.Signature);
  };
  if (!MatchesCurrent(*Expression.SourceCallHint))
    return false;
  size_t Budget = 100000, Matches = 0;
  std::set<SourceCallOccurrenceKey> MedSites, Seen;
  for (const auto &Block : Caller->Med->Blocks)
    for (const auto &Op : Block.Ops) {
      if (!Budget--)
        return false;
      if (!Op.SourceCallHint || !Op.SourceCallHint->ImmutableNativeCall)
        continue;
      const auto &Binding = *Op.SourceCallHint;
      const auto &Site = Binding.ImmutableNativeCall->Site;
      if (!MatchesCurrent(Binding) || Op.Opcode != NdOp::INDIR_CALL ||
          Op.NumInputs != sourceABIParameters(Binding.Signature).size() + 1 ||
          Op.Inputs[0].isConst() || Op.Inputs[0].Size != 8 ||
          Op.DoesNotReturn || Op.PreservesCallerSaved ||
          Site.Instruction != Op.Addr || Site.Sequence != Op.OriginSeq ||
          !MedSites.insert(Site).second)
        return false;
    }
  // Count evaluations, including shared expression nodes. Every original
  // bound occurrence must survive exactly once, with the same current proof.
  bool Foreign = false;
  std::vector<const HighStmt *> Statements;
  auto Append = [&](const std::vector<HighStmt> &Body) {
    if (Body.size() > Budget - std::min(Budget, Statements.size())) {
      Budget = 0;
      return;
    }
    for (const auto &Statement : Body)
      Statements.push_back(&Statement);
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
        auto Call = Pending.back();
        Pending.pop_back();
        if (!Call)
          continue;
        if (Call->SourceCallHint && Call->SourceCallHint->ImmutableNativeCall) {
          Matches += Call.get() == &Expression;
          const auto &Binding = *Call->SourceCallHint;
          Foreign |=
              !isImmutableNativeSourceCall(*Call, Function.Entry, Image.Arch) ||
              !MatchesCurrent(Binding) ||
              !Seen.insert(Binding.ImmutableNativeCall->Site).second;
        }
        Call->forEachChildExpr([&](const ExprPtr &Child) {
          if (Pending.size() >= Budget)
            Budget = 0;
          else
            Pending.push_back(Child);
        });
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
  return Budget && !Foreign && Matches == 1 && Seen == MedSites;
}
} // namespace neverd::sdk
#endif
