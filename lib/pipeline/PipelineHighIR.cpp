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
#include "neverd/support/Parallel.h"

#include "llvm/ADT/StringExtras.h"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <stdexcept>
#include <string>
#include <utility>
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
  // A worker failure invalidates the whole stage. Keep output private until
  // every worker has joined so callers never receive a partially built batch.
  std::vector<HighFunc> Pending(Total);

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
    Local.setBinaryImage(&Img);
    Local.setFuncNames(&AllFuncNames);
    for (size_t FI; (FI = Claim()) < N;) {
      const MedFunc &MF = Result.MedFuncs[FI];
      try {
        if (FI < Result.LowFuncs.size())
          Local.setJumpTables(Result.LowFuncs[FI].JumpTables);
        else
          Local.setJumpTables({});
        Pending[FI] = Local.convert(MF, Img.Arch);
        auto &HF = Pending[FI];
        HF.OriginalSize = MF.OriginalSize;
        HF.DebugName = MF.DebugName;
        HF.SourceFile = MF.SourceFile;
        HF.SourceLine = MF.SourceLine;
      } catch (const std::exception &Error) {
        throw std::runtime_error("function '" + MF.Name + "' at 0x" +
                                 llvm::utohexstr(MF.Entry) + ": " +
                                 Error.what());
      } catch (...) {
        throw std::runtime_error("function '" + MF.Name + "' at 0x" +
                                 llvm::utohexstr(MF.Entry) +
                                 ": unknown exception");
      }
    }
  });
  Result.HighFuncs = std::move(Pending);
}

} // namespace neverd
