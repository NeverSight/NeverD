//===- PipelineHighIRDetail.h - HighIR completion boundary ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_PIPELINE_PIPELINEHIGHIRDETAIL_H
#define NEVERD_LIB_PIPELINE_PIPELINEHIGHIRDETAIL_H

#include "neverd/pipeline/Pipeline.h"

#include "llvm/ADT/StringExtras.h"

#include <cstddef>
#include <exception>
#include <string>
#include <utility>

namespace neverd::pipeline_detail {

/// Contain HighIR-stage failures before any caller can consume the result.
/// Build must complete all of its workers before returning or throwing. A
/// successful stage preserves every source function's identity; an empty body
/// is valid and does not indicate whether conversion completed.
template <typename BuildFn>
bool runHighIRStage(PipelineResult &Result, BuildFn &&Build) {
  Result.Success = false;
  Result.HighFuncs.clear();
  Result.Error.clear();
  auto Fail = [&](std::string Error) {
    Result.Success = false;
    Result.HighFuncs.clear();
    Result.Error = std::move(Error);
    return false;
  };

  try {
    std::forward<BuildFn>(Build)();
    if (Result.HighFuncs.size() != Result.MedFuncs.size())
      return Fail("HighIR conversion returned " +
                  std::to_string(Result.HighFuncs.size()) + " functions for " +
                  std::to_string(Result.MedFuncs.size()) + " source functions");
    for (size_t I = 0; I < Result.MedFuncs.size(); ++I) {
      const MedFunc &Source = Result.MedFuncs[I];
      const HighFunc &Output = Result.HighFuncs[I];
      if (Source.Name.empty() || Output.Name != Source.Name ||
          Output.Entry != Source.Entry)
        return Fail("HighIR conversion lost the identity of function '" +
                    Source.Name + "' at 0x" + llvm::utohexstr(Source.Entry));
    }
    return true;
  } catch (const std::exception &Error) {
    return Fail(std::string("HighIR conversion failed: ") + Error.what());
  } catch (...) {
    return Fail("HighIR conversion failed: unknown exception");
  }
}

} // namespace neverd::pipeline_detail

#endif // NEVERD_LIB_PIPELINE_PIPELINEHIGHIRDETAIL_H
