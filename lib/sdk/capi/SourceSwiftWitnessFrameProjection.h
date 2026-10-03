#ifndef NEVERD_SDK_CAPI_SOURCESWIFTWITNESSFRAMEPROJECTION_H
#define NEVERD_SDK_CAPI_SOURCESWIFTWITNESSFRAMEPROJECTION_H

#include "ObjCNativeCurrentFunction.h"
#include "SourceExpressionIdentity.h"

#include "neverd/ir/high/MedToHigh.h"
#include "neverd/pipeline/NativeSourceHints.h"

namespace neverd::sdk {
namespace swift_witness_frame_detail {
using Calls = std::map<SourceCallOccurrenceKey, const HighExpr *>;

// Count evaluations rather than expression pointers. In particular, two
// statements sharing the same node cannot reuse a single machine occurrence.
inline std::optional<Calls> calls(const HighFunc &Function) {
  Calls Result;
  size_t Budget = 100000;
  bool Invalid = false;
  std::vector<const HighStmt *> Statements;
  const auto Append = [&](const auto &Body) {
    if (Body.size() > Budget - std::min(Budget, Statements.size())) {
      Budget = 0;
      return;
    }
    for (const auto &S : Body)
      Statements.push_back(&S);
  };
  Append(Function.Body);
  while (Budget && !Statements.empty()) {
    --Budget;
    const auto &S = *Statements.back();
    Statements.pop_back();
    forEachExpr(S, [&](const ExprPtr &Root) {
      std::vector<ExprPtr> Pending{Root};
      while (Budget && !Pending.empty()) {
        --Budget;
        auto E = Pending.back();
        Pending.pop_back();
        if (!E)
          continue;
        if (E->SourceCallHint && E->SourceCallHint->SwiftWitnessFrame) {
          const auto &Hint = *E->SourceCallHint;
          Invalid |=
              E->Kind != ExprKind::Call || !E->IsIndirectCall || E->CallAddr ||
              E->IntrinsicId != Intrinsic::None ||
              E->MemoryOrdering != NdMemoryOrdering::None ||
              E->MemoryAddressSpace != NdMemoryAddressSpace::Default ||
              !E->IndirectTarget || !E->IndirectTarget->Type ||
              E->IndirectTarget->Type->Size != 8 ||
              !isSwiftValueWitnessSourceCallHint(Hint, Arch::AArch64) ||
              Hint.SwiftWitnessFrame->FunctionEntry != Function.Entry ||
              E->Operands.size() != Hint.Signature.Parameters.size() ||
              !E->Type || !Hint.Signature.ReturnType ||
              E->Type->Size != Hint.Signature.ReturnType->Size ||
              !(E->Type->Kind == Hint.Signature.ReturnType->Kind ||
                ((E->Type->Kind == NdTypeKind::Int ||
                  E->Type->Kind == NdTypeKind::Ptr) &&
                 (Hint.Signature.ReturnType->Kind == NdTypeKind::Int ||
                  Hint.Signature.ReturnType->Kind == NdTypeKind::Ptr))) ||
              !Result.emplace(Hint.SwiftWitnessFrame->Site, E.get()).second;
        }
        E->forEachChildExpr([&](const ExprPtr &Child) {
          if (Pending.size() >= Budget)
            Budget = 0;
          else
            Pending.push_back(Child);
        });
      }
    });
    Append(S.Body);
    Append(S.ElseBody);
    for (const auto &Case : S.Cases)
      Append(Case.Body);
    Append(S.DefaultBody);
    for (const auto &Body : S.EHClauseBodies)
      Append(Body);
  }
  return Budget && !Invalid ? std::optional<Calls>(std::move(Result))
                            : std::nullopt;
}

inline bool sameCalls(const Calls &Expected, const Calls &Actual) {
  if (Expected.size() != Actual.size())
    return false;
  size_t Budget = 100000;
  for (const auto &[Site, E] : Expected) {
    const auto Found = Actual.find(Site);
    if (Found == Actual.end())
      return false;
    const auto &A = *Found->second;
    if (E->SourceCallHint->ValueWitness != A.SourceCallHint->ValueWitness ||
        E->SourceCallHint->SwiftWitnessFrame !=
            A.SourceCallHint->SwiftWitnessFrame ||
        !equalSourceABIs(E->SourceCallHint->Signature,
                         A.SourceCallHint->Signature) ||
        E->Operands.size() != A.Operands.size() ||
        !sameSourceExpressionIdentity(*E->IndirectTarget, *A.IndirectTarget,
                                      Budget))
      return false;
    for (size_t I = 0; I < E->Operands.size(); ++I)
      if (!E->Operands[I] || !A.Operands[I] ||
          !sameSourceExpressionIdentity(*E->Operands[I], *A.Operands[I],
                                        Budget))
        return false;
  }
  return true;
}
} // namespace swift_witness_frame_detail

class SourceSwiftWitnessFrameProjectionValidator {
  const BinaryImage &Image;
  const PipelineResult &Result;
  std::map<va_t, bool> Candidates;

public:
  SourceSwiftWitnessFrameProjectionValidator(const BinaryImage &Image,
                                             const PipelineResult &Result)
      : Image(Image), Result(Result) {
    for (const auto &F : Result.MedFuncs)
      for (const auto &B : F.Blocks)
        for (const auto &Op : B.Ops)
          if (Op.SourceCallHint && (Op.SourceCallHint->ValueWitness ||
                                    Op.SourceCallHint->SwiftWitnessFrame))
            Candidates[F.Entry] |= bool(Op.SourceCallHint->SwiftWitnessFrame);
  }

  bool valid(const HighFunc &Function) const {
    using namespace swift_witness_frame_detail;
    const auto Published = calls(Function);
    if (!Published || Result.SourceImage != &Image || !Result.Success)
      return false;
    const auto Candidate = Candidates.find(Function.Entry);
    if (Candidate == Candidates.end())
      return Published->empty();
    // An ordinary register-derived witness may precede a terminal call or
    // trap. Only a frame-dependent occurrence requires a returning body here.
    // Still rebuild the LowIR bindings below: deleting a frame receipt must
    // not turn its call into an unrelated register-only witness.
    const auto Current = native_source_detail::currentFunction(
        Result, Function.Entry, /*RequireReturningBody=*/Candidate->second);
    if (!Current ||
        !validateSwiftWitnessFrameBindings(Image, Current->Low, *Current->Med))
      return false;
    Calls MedSites;
    for (const auto &B : Current->Med->Blocks)
      for (const auto &Op : B.Ops)
        if (Op.SourceCallHint && Op.SourceCallHint->SwiftWitnessFrame)
          MedSites.emplace(Op.SourceCallHint->SwiftWitnessFrame->Site, nullptr);
    if (MedSites.empty())
      return Published->empty();
    if (!Function.SourceTypeHint || Function.DoesNotReturn ||
        Function.StructuredExceptionRegions ||
        Function.UnstructuredExceptionRegions ||
        !equalSourceABIs(*Function.SourceTypeHint,
                         *Current->High->SourceTypeHint) ||
        Function.Params.size() != Current->High->Params.size())
      return false;
    for (size_t I = 0; I < Function.Params.size(); ++I)
      if (!equalSourceTypes(Function.Params[I].Type,
                            Current->High->Params[I].Type))
        return false;
    // Regenerate with the canonical Med-to-High owner. A modified HighIR
    // target/argument, including modification of both saved and published
    // bodies, must not launder the occurrence receipt. This is a source
    // comparison only; all frame/effect semantics were re-proved above.
    MedToHighConverter Converter;
    Converter.setBinaryImage(&Image);
    std::map<va_t, std::string> Names;
    for (const auto &F : Result.MedFuncs)
      Names.emplace(F.Entry, F.Name);
    Converter.setFuncNames(&Names);
    Converter.setJumpTables(Current->Low->JumpTables);
    const auto Replay = Converter.convert(*Current->Med, Image.Arch);
    const auto Expected = calls(Replay), Saved = calls(*Current->High);
    if (!Expected || !Saved || Expected->size() != MedSites.size())
      return false;
    for (const auto &[Site, E] : *Expected)
      if (!MedSites.count(Site))
        return false;
    return sameCalls(*Expected, *Saved) && sameCalls(*Expected, *Published);
  }
};
} // namespace neverd::sdk
#endif
