//===- PipelineStageDetail.h - Private pipeline stage helpers ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Translation-unit-local helpers shared by pipeline stage implementations.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_PIPELINE_PIPELINESTAGEDETAIL_H
#define NEVERD_LIB_PIPELINE_PIPELINESTAGEDETAIL_H

#include "neverd/ir/med/LowToMed.h"
#include "neverd/pipeline/Pipeline.h"

#include <cstddef>
#include <map>

namespace neverd {
namespace {

// Both IRs are built by repeated push_back and then kept alive for the rest of
// the run, so every block carries whatever slack its last geometric growth left
// behind -- across tens of thousands of blocks that slack is a sizeable
// fraction of the pipeline's resident set.  Trimming each function once, on the
// worker that just finished building it, costs one copy of data already in
// cache and is invisible next to decode/SSA.
template <typename Func> inline void trimFuncStorage(Func &F) {
  for (auto &B : F.Blocks) {
    B.Ops.shrink_to_fit();
    B.Succs.shrink_to_fit();
    B.Preds.shrink_to_fit();
    if constexpr (requires { B.Phis; })
      B.Phis.shrink_to_fit();
  }
  F.Blocks.shrink_to_fit();
}

inline bool recordMedIRVerification(PipelineResult &Result,
                                    const char *PassName) {
  std::map<va_t, PipelineFunctionAudit *> AuditByEntry;
  for (auto &Audit : Result.FunctionAudits)
    AuditByEntry[Audit.Entry] = &Audit;

  Result.MedIRVerifierFailures = 0;
  for (size_t I = 0; I < Result.LowFuncs.size(); ++I) {
    const LowFunc &LF = Result.LowFuncs[I];
    const bool HasMed = I < Result.MedFuncs.size() &&
                        Result.MedFuncs[I].Entry == LF.Entry &&
                        !Result.MedFuncs[I].Blocks.empty();
    const bool Verified = HasMed && verifyMedFunc(Result.MedFuncs[I], PassName);
    if (!Verified)
      ++Result.MedIRVerifierFailures;

    auto It = AuditByEntry.find(LF.Entry);
    if (It == AuditByEntry.end())
      continue;
    PipelineFunctionAudit &Audit = *It->second;
    Audit.HasMedIR = HasMed;
    Audit.MedIRVerified = Verified;
    if (!Verified)
      Audit.Disposition = PipelineFunctionDisposition::MedIRFailed;
  }
  return Result.MedIRVerifierFailures == 0;
}

} // namespace
} // namespace neverd

#endif // NEVERD_LIB_PIPELINE_PIPELINESTAGEDETAIL_H
