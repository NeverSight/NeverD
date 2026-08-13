//===- PipelineMedIR.cpp - MedIR pipeline stage --------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// LowIR-to-MedIR conversion for the decompilation pipeline.
///
//===----------------------------------------------------------------------===//

#include "PipelineStageDetail.h"

#include "neverd/Common.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/pipeline/Pipeline.h"
#include "neverd/support/Diagnostic.h"
#include "neverd/support/Parallel.h"

#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <vector>

#define DEBUG_TYPE "neverd-pipeline"

namespace neverd {

//===----------------------------------------------------------------------===//
// buildMedIR — Phase 2
//===----------------------------------------------------------------------===//

void Pipeline::buildMedIR(const BinaryImage &Img,
                          const PipelineOptions & /*Opts*/,
                          PipelineResult &Result) {
  auto Phase2Start = std::chrono::steady_clock::now();

  const size_t Total = Result.LowFuncs.size();
  Result.MedFuncs.resize(Total);

  // Per-callee callee-cleanup pop (x86 `ret imm`, the i386 SysV sret hidden-
  // pointer pop) so each caller's CALL to such a callee gets a post-call stack-
  // pointer correction during low->med translation.  Built once (read-only) and
  // shared across the parallel converters.
  std::map<va_t, int> CalleePop;
  for (const auto &LF : Result.LowFuncs)
    if (LF.CalleePopBytes > 0)
      CalleePop[LF.Entry] = LF.CalleePopBytes;

  // GOT/pointer-slot VAs holding a stack-probe import (____chkstk_darwin).
  // Apple clang guards a large frame with a GOT-indirect probe call in the
  // prologue; the low->med converter clears that call's spurious x0 output
  // before SSA so it does not kill the live-in argument registers (see
  // setStackProbeSlots).  Built once (read-only) and shared across the parallel
  // converters.  Empty on non-Mach-O (ImportPtrSlots is only populated there)
  // -> no-op.
  std::set<va_t> StackProbeSlots;
  for (const auto &[SlotVA, SymName] : Img.ImportPtrSlots) {
    if (isDarwinStackProbeName(SymName))
      StackProbeSlots.insert(SlotVA);
  }

  // Weight each function by its LowIR op count so the heaviest translations
  // start first and the tail stays balanced (see parallelForEachWeighted).
  std::vector<uint64_t> Weight(Total, 1);
  for (size_t I = 0; I < Total; ++I) {
    uint64_t W = 1;
    for (const auto &B : Result.LowFuncs[I].Blocks)
      W += B.Ops.size();
    Weight[I] = W;
  }

  parallelForEachWeighted(Weight, [&](auto Claim, size_t N) {
    LowToMedConverter Local;
    Local.setCalleePopMap(&CalleePop);
    Local.setStackProbeSlots(&StackProbeSlots);
    for (size_t I; (I = Claim()) < N;) {
      try {
        Result.MedFuncs[I] =
            Local.convert(Result.LowFuncs[I], Img.Arch, Img.Format);
        auto &MF = Result.MedFuncs[I];
        auto &LF = Result.LowFuncs[I];
        trimFuncStorage(MF);
        MF.OriginalSize = LF.OriginalSize;
        MF.DebugName = LF.DebugName;
        MF.SourceFile = LF.SourceFile;
        MF.SourceLine = LF.SourceLine;
      } catch (...) {
        syncWarning() << "pipeline: low->med threw on "
                      << Result.LowFuncs[I].Name << "\n";
        Result.MedFuncs[I] = MedFunc{};
      }
    }
  });

  recordMedIRVerification(Result, "pipeline-med-final");

  [[maybe_unused]] auto Elapsed =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - Phase2Start)
          .count();
  LLVM_DEBUG(llvm::dbgs() << "pipeline: LowIR -> MedIR took " << Elapsed
                          << "ms (" << Result.MedFuncs.size()
                          << " functions)\n");
}

} // namespace neverd
