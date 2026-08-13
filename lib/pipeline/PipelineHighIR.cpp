//===- PipelineHighIR.cpp - HighIR pipeline stage ------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// MedIR-to-HighIR conversion for the decompilation pipeline.
///
//===----------------------------------------------------------------------===//

#include "neverd/ir/high/MedToHigh.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/pipeline/Pipeline.h"
#include "neverd/support/Diagnostic.h"
#include "neverd/support/Parallel.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace neverd {

//===----------------------------------------------------------------------===//
// buildHighIR — Phase 3
//===----------------------------------------------------------------------===//

void Pipeline::buildHighIR(const BinaryImage &Img,
                           const PipelineOptions & /*Opts*/,
                           PipelineResult &Result) {
  auto AllFuncNames = buildFuncNameMap(Img, Result);

  detectThunkStubs(Result.LowFuncs, AllFuncNames);

  const size_t Total = Result.MedFuncs.size();
  Result.HighFuncs.resize(Total);

  // Weight each function by its MedIR op count so the heaviest structurings
  // start first and the tail stays balanced (see parallelForEachWeighted).
  std::vector<uint64_t> Weight(Total, 1);
  for (size_t I = 0; I < Total; ++I) {
    uint64_t W = 1;
    for (const auto &B : Result.MedFuncs[I].Blocks)
      W += B.Ops.size() + B.Phis.size();
    Weight[I] = W;
  }

  parallelForEachWeighted(Weight, [&](auto Claim, size_t N) {
    MedToHighConverter Local;
    Local.setFuncNames(&AllFuncNames);
    for (size_t FI; (FI = Claim()) < N;) {
      if (FI < Result.LowFuncs.size())
        Local.setJumpTables(Result.LowFuncs[FI].JumpTables);
      else
        Local.setJumpTables({});
      try {
        Result.HighFuncs[FI] = Local.convert(Result.MedFuncs[FI], Img.Arch);
        auto &HF = Result.HighFuncs[FI];
        auto &MF = Result.MedFuncs[FI];
        HF.OriginalSize = MF.OriginalSize;
        HF.DebugName = MF.DebugName;
        HF.SourceFile = MF.SourceFile;
        HF.SourceLine = MF.SourceLine;
      } catch (...) {
        syncWarning() << "pipeline: med->high threw on "
                      << Result.MedFuncs[FI].Name << "\n";
        Result.HighFuncs[FI] = HighFunc{};
      }
    }
  });
}

} // namespace neverd
