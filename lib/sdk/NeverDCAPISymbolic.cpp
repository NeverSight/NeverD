//===- NeverDCAPISymbolic.cpp - Symbolic exploration C API ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Exposes bounded, architecture-aware LowIR path exploration as JSON.
///
//===----------------------------------------------------------------------===//

#include "SessionImpl.h"

#include "neverd/ir/TargetRegInfo.h"
#include "neverd/sdk/NeverDCAPI.h"
#include "neverd/symbolic/SymExplore.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/JSON.h"

#include <cstddef>

using namespace neverd;
using namespace neverd::sdk;

namespace {

#define FIELD_END(Type, Field)                                                 \
  (offsetof(Type, Field) + sizeof(static_cast<Type *>(nullptr)->Field))

bool reaches(size_t Size, size_t End) { return Size >= End; }

symbolic::ExploreOptions
readOptions(const neverd_symbolic_explore_options *Input) {
  symbolic::ExploreOptions Options;
  if (!Input)
    return Options;

  const size_t Size = Input->struct_size;
  if (reaches(Size, FIELD_END(neverd_symbolic_explore_options, max_paths)) &&
      Input->max_paths)
    Options.MaxPaths = Input->max_paths;
  if (reaches(Size, FIELD_END(neverd_symbolic_explore_options, max_steps)) &&
      Input->max_steps)
    Options.MaxSteps = Input->max_steps;
  if (reaches(Size,
              FIELD_END(neverd_symbolic_explore_options, max_block_visits)) &&
      Input->max_block_visits)
    Options.MaxBlockVisits = Input->max_block_visits;
  return Options;
}

bool includeExpressions(const neverd_symbolic_explore_options *Input) {
  return Input &&
         reaches(Input->struct_size, FIELD_END(neverd_symbolic_explore_options,
                                               include_expressions)) &&
         Input->include_expressions != 0;
}

llvm::SmallVector<symbolic::SymRegisterRange, 16>
callPreservedRanges(const BinaryImage &Image) {
  const TargetRegInfo &TRI = getTargetRegInfo(Image.Arch);
  llvm::SmallVector<symbolic::SymRegisterRange, 16> Ranges;
  for (const TargetRegisterRange &Range :
       TRI.callPreservedRanges(Image.Format))
    Ranges.push_back({Range.Offset, Range.Bytes});
  return Ranges;
}

llvm::json::Object errorReport(llvm::StringRef Message) {
  return llvm::json::Object{{"ok", false}, {"error", Message}};
}

#undef FIELD_END

} // namespace

extern "C" {

const char *
neverd_symbolic_explore_json(neverd_session_t Sess, neverd_va_t FuncEntry,
                             const neverd_symbolic_explore_options *Options) {
  auto *S = toSession(Sess);
  S->clearError();
  if (!S->ensurePipeline())
    return dupStr(jsonToString(errorReport(S->LastError)));

  if (S->PipeResult.EVM || S->PipeResult.SBF) {
    S->setError("symbolic path exploration currently requires native LowIR");
    return dupStr(jsonToString(errorReport(S->LastError)));
  }

  const LowFunc *Function = S->findLowFunc(FuncEntry);
  if (!Function) {
    S->setError("function not found in LowIR");
    return dupStr(jsonToString(errorReport(S->LastError)));
  }

  symbolic::ExploreOptions EngineOptions = readOptions(Options);
  llvm::SmallVector<symbolic::SymRegisterRange, 16> Preserved =
      callPreservedRanges(S->Img);
  EngineOptions.CallPreservedRegisters.assign(Preserved.begin(),
                                              Preserved.end());

  symbolic::SymContext Context;
  symbolic::SymExploration Exploration =
      symbolic::explorePathsDetailed(Context, *Function, EngineOptions);

  llvm::json::Array Paths;
  for (const symbolic::SymPath &Path : Exploration.Paths) {
    llvm::json::Array Blocks;
    for (int Block : Path.Blocks)
      Blocks.push_back(Block);

    llvm::json::Object Item{
        {"outcome", symbolic::pathOutcomeName(Path.Outcome)},
        {"block", Path.BlockId},
        {"blocks", std::move(Blocks)},
        {"constraints", static_cast<int64_t>(Path.Constraints.size())},
        {"unmodelledOps", static_cast<int64_t>(Path.UnmodelledOps)}};
    if (includeExpressions(Options)) {
      Item["predicate"] = Context.toString(Path.predicate(Context));
      if (Path.Target.isValid())
        Item["target"] = Context.toString(Path.Target);
    }
    Paths.push_back(std::move(Item));
  }

  llvm::json::Object Root{
      {"ok", true},
      {"function", Function->Name},
      {"entry", vaHex(Function->Entry)},
      {"liftComplete", Function->hasCompleteInstructionLift()},
      {"complete", Exploration.Complete},
      {"exact", Function->hasCompleteInstructionLift() &&
                    Exploration.Complete && Exploration.UnmodelledOps == 0},
      {"reachablePaths", static_cast<int64_t>(Exploration.ReachablePaths)},
      {"reportedPaths", static_cast<int64_t>(Exploration.Paths.size())},
      {"executedSteps", static_cast<int64_t>(Exploration.ExecutedSteps)},
      {"unmodelledOps", static_cast<int64_t>(Exploration.UnmodelledOps)},
      {"paths", std::move(Paths)}};
  return dupStr(jsonToString(llvm::json::Value(std::move(Root))));
}

} // extern "C"
