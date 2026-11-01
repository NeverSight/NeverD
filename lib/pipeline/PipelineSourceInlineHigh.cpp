//===- PipelineSourceInlineHigh.cpp - Source body helpers ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Keep structured callers while materializing validated source-only helpers.
//===----------------------------------------------------------------------===//

#include "PipelineSourceInlineHigh.h"

#include "neverd/ir/TargetRegInfo.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/pipeline/Limits.h"
#include "neverd/pipeline/Pipeline.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringSet.h"

#include <algorithm>
#include <functional>
#include <utility>

namespace neverd {
namespace {

bool simpleOperand(const ExprPtr &Expression, bool RequireUnknown) {
  const HighExpr *Current = Expression.get();
  for (unsigned Depth = 0;
       Current && Depth != limits::pipeline::kSourceInlineOperandDepth;
       ++Depth) {
    if (Current->Kind == ExprKind::Undef)
      return true;
    if (!RequireUnknown && (Current->Kind == ExprKind::Const ||
                            (Current->Kind == ExprKind::Var &&
                             Current->Var.Kind != MedVar::Stack)))
      return true;
    if ((Current->Kind != ExprKind::Cast &&
         Current->Kind != ExprKind::BitCast) ||
        Current->Operands.size() != 1 || !Current->CastTo ||
        (Current->CastTo->Kind != NdTypeKind::Int &&
         Current->CastTo->Kind != NdTypeKind::Ptr))
      return false;
    Current = Current->Operands.front().get();
  }
  return false;
}

/// One copy of each DAG node, with a shared budget and explicit observation
/// remapping. The input functions and their shared expressions stay untouched.
class HighCopies {
public:
  size_t Remaining = limits::pipeline::kSourceInlineExpansionOperations;
  bool Failed = false;
  llvm::DenseMap<const HighExpr *, ExprPtr> Copies;
  using Transform = std::function<void(HighExpr &)>;

  ExprPtr expression(const ExprPtr &Original, const Transform &Change) {
    if (!Original || Failed)
      return Original;
    if (auto At = Copies.find(Original.get()); At != Copies.end())
      return At->second;
    if (!Remaining) {
      Failed = true;
      return Original;
    }
    --Remaining;
    auto Copy = std::make_shared<HighExpr>(*Original);
    Copies.try_emplace(Original.get(), Copy);
    for (auto &Operand : Copy->Operands)
      Operand = expression(Operand, Change);
    Copy->IndirectTarget = expression(Copy->IndirectTarget, Change);
    Change(*Copy);
    return Copy;
  }

  void function(HighFunc &Function, const Transform &Change) {
    walkStmts(Function.Body, [&](HighStmt &Statement) {
      if (!Remaining) {
        Failed = true;
        return;
      }
      --Remaining;
      forEachExpr(Statement, [&](ExprPtr &E) { E = expression(E, Change); });
      for (auto &Clause : Statement.EHClauses)
        Clause.FilterValue = expression(Clause.FilterValue, Change);
    });
  }
};

} // namespace

bool hasUnavailableSourceStackCall(const std::vector<HighFunc> &Functions,
                                   const PipelineResult *Original) {
  SourceInlineRequests Requests;
  std::set<va_t> Roots;
  const auto Layout =
      getTargetRegInfo(Arch::X64).integerArgumentLayout(BinaryFormat::COFF);
  size_t Remaining = limits::pipeline::kSourceInlineExpansionOperations;
  llvm::SmallPtrSet<const HighExpr *, 32> Seen;
  const auto Visit = [&](auto &&Self, const ExprPtr &E) -> void {
    if (!E || !Remaining || !Seen.insert(E.get()).second)
      return;
    --Remaining;
    if (E->Kind == ExprKind::Call && !E->IsIndirectCall && E->CallAddr &&
        !E->SourceCallHint)
      for (unsigned P = Layout.Registers.size(); P < E->Operands.size(); ++P)
        if (simpleOperand(E->Operands[P], true))
          Requests[E->CallAddr].insert(P);
    E->forEachChildExpr([&](const ExprPtr &Child) { Self(Self, Child); });
  };
  for (const auto &Function : Functions) {
    Roots.insert(Function.Entry);
    walkStmts(Function.Body, [&](const HighStmt &Statement) {
      forEachExpr(Statement, [&](const ExprPtr &E) { Visit(Visit, E); });
    });
  }
  return !Requests.empty() &&
         (!Original ||
          !sourceInlineSupports(*Original, Requests, Roots).empty());
}

std::shared_ptr<SourceInlineHighPreparation>
prepareSpecializedHighSourceCalls(PipelineResult &Recovered,
                                  std::map<va_t, SourceInlineBody> Bodies) {
  if (Bodies.empty())
    return {};
  auto Prepared = std::make_shared<SourceInlineHighPreparation>();
  Prepared->Bodies = std::move(Bodies);
  auto &Kept = *(Prepared->Recovered = std::make_shared<PipelineResult>());
  Kept.SourceImage = Recovered.SourceImage;
  Kept.Success = Recovered.Success;
  Kept.HighIRDeferred = true;
  Kept.LowFuncs = std::move(Recovered.LowFuncs);
  for (auto &Function : Recovered.MedFuncs)
    if (Prepared->Bodies.contains(Function.Entry))
      Kept.MedFuncs.push_back(std::move(Function));
  Kept.CallPrototypeArguments = std::move(Recovered.CallPrototypeArguments);
  Kept.LibraryRecognitions = std::move(Recovered.LibraryRecognitions);
  return Prepared;
}

std::optional<SourceInlineHighView> projectSpecializedHighSourceCalls(
    const BinaryImage &Image, SourceInlineHighPreparation &Prepared,
    const PipelineOptions &Options, DebugContext *Debug,
    const SourceInlineHighView &Original) {
  return specializeHighSourceBodies(Image, *Prepared.Recovered, Options, Debug,
                                    Prepared.Bodies, Original);
}

std::optional<SourceInlineHighView>
specializeHighSourceBodies(const BinaryImage &Image, PipelineResult &Recovered,
                           const PipelineOptions &Options, DebugContext *Debug,
                           const std::map<va_t, SourceInlineBody> &Bodies,
                           const SourceInlineHighView &Original) {
  if (Bodies.empty())
    return std::nullopt;
  // The original structured callers are reused below. Their joint MedIR
  // already supplied the ABI proof; only the admitted helper bodies need
  // another HighIR conversion. Keep the shared LowIR/name/signature facts.
  std::erase_if(Recovered.MedFuncs, [&](const MedFunc &Function) {
    return !Bodies.contains(Function.Entry);
  });
  const bool Ready = Pipeline().resumeHighIR(Image, Options, Recovered, Debug);
  // Only the projected helper DAGs are used on later source queries. Release
  // the temporary machine/MedIR preparation instead of retaining another copy
  // of the caller and its signature analysis with each cached module.
  std::vector<LowFunc>().swap(Recovered.LowFuncs);
  std::vector<MedFunc>().swap(Recovered.MedFuncs);
  std::vector<sigs::LibraryRecognition>().swap(Recovered.LibraryRecognitions);
  Recovered.CallPrototypeArguments.clear();
  if (!Ready)
    return std::nullopt;

  struct Helper {
    HighFunc Function;
    SourceInlineBody Body;
  };
  std::map<va_t, Helper> Helpers;
  // Names belong to this emitted unit, not to the image's native identities.
  llvm::StringSet<> Names;
  for (const auto &Symbol : Image.Symbols)
    if (!Symbol.Name.empty())
      Names.insert(Symbol.Name);
  for (const auto &Function : Original.Functions)
    Names.insert(Function.Name);
  for (const auto &Function : Recovered.HighFuncs) {
    const auto At = Bodies.find(Function.Entry);
    if (At == Bodies.end() || Function.Params.size() != At->second.Parameters ||
        Function.IsVariadic || Function.SourceTypeHint ||
        Function.StructuredExceptionRegions ||
        Function.UnstructuredExceptionRegions)
      continue;
    HighFunc Copy = Function;
    std::vector<int> Positions(Copy.Params.size(), -1);
    std::vector<HighParam> Parameters;
    for (unsigned P = 0; P < Copy.Params.size(); ++P)
      if (!At->second.Unused.count(P)) {
        Positions[P] = Parameters.size();
        Parameters.push_back(Copy.Params[P]);
      }
    HighCopies Cloner;
    Cloner.function(Copy, [&](HighExpr &E) {
      const auto Remap = [&](MedVar &V) {
        if (V.Kind != MedVar::Param)
          return;
        if (V.Id < 0 || unsigned(V.Id) >= Positions.size() ||
            Positions[V.Id] < 0) {
          Cloner.Failed = true;
          return;
        }
        V.Id = Positions[V.Id];
      };
      Remap(E.Var);
      for (auto &Output : E.IntrinsicOutputs)
        Remap(Output);
    });
    if (Cloner.Failed)
      continue;
    Copy.Params = std::move(Parameters);
    Copy.Name = "neverd_source_body_" + llvm::utohexstr(Function.Entry);
    while (!Names.insert(Copy.Name).second)
      Copy.Name += '_';
    Copy.Entry = 0;
    Copy.OriginalSize = 0;
    Copy.DebugName.clear();
    Copy.SourceFile.clear();
    Copy.SourceLine = 0;
    Copy.ExceptionMetadata.reset();
    Helpers.emplace(Function.Entry, Helper{std::move(Copy), At->second});
  }
  if (Helpers.empty())
    return std::nullopt;

  SourceInlineHighView Result = Original;
  HighCopies Cloner;
  std::set<va_t> Used;
  for (auto &Function : Result.Functions)
    Cloner.function(Function, [&](HighExpr &E) {
      if (E.Kind != ExprKind::Call || E.IsIndirectCall || E.SourceCallHint)
        return;
      const auto At = Helpers.find(E.CallAddr);
      if (At == Helpers.end() ||
          E.Operands.size() != At->second.Body.Parameters)
        return;
      for (unsigned P : At->second.Body.Unused)
        if (!simpleOperand(E.Operands[P], false))
          return;
      std::vector<ExprPtr> Arguments;
      for (unsigned P = 0; P < E.Operands.size(); ++P)
        if (!At->second.Body.Unused.count(P))
          Arguments.push_back(E.Operands[P]);
      Used.insert(E.CallAddr);
      E.CallTarget = At->second.Function.Name;
      E.CallAddr = 0;
      E.Operands = std::move(Arguments);
    });
  if (Used.empty() || Cloner.Failed)
    return std::nullopt;
  for (auto &Observation : Result.Sources)
    if (const auto Old = Observation.Expression.lock())
      if (auto At = Cloner.Copies.find(Old.get()); At != Cloner.Copies.end())
        Observation.Expression = At->second;
  for (va_t Entry : Used) {
    auto &Helper = Helpers.at(Entry).Function;
    Result.BodyOrigins.emplace(Helper.Name, Entry);
    Result.Functions.push_back(std::move(Helper));
  }
  return Result;
}
} // namespace neverd
