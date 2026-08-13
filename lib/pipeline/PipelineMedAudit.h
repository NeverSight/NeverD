//===- PipelineMedAudit.h - MedIR pipeline audit helpers --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Private MedIR verification and per-function audit bookkeeping.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_PIPELINE_PIPELINEMEDAUDIT_H
#define NEVERD_LIB_PIPELINE_PIPELINEMEDAUDIT_H

#include "neverd/ir/med/LowToMed.h"
#include "neverd/pipeline/Pipeline.h"

#include <cstddef>
#include <map>

namespace neverd {
namespace {

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

#endif // NEVERD_LIB_PIPELINE_PIPELINEMEDAUDIT_H
