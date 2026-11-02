//===- FunctionAuditReason.h - Source query failure diagnostics -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SDK_CAPI_FUNCTIONAUDITREASON_H
#define NEVERD_SDK_CAPI_FUNCTIONAUDITREASON_H

#include "neverd/pipeline/Pipeline.h"
#include "neverd/sdk/Limits.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"

#include <string>
#include <vector>

namespace neverd::sdk {

/// The audit's disposition and the evidence for it, shared by source routes.
inline std::string functionAuditReason(const PipelineFunctionAudit &Audit) {
  std::string Reason = pipelineFunctionDispositionName(Audit.Disposition);
  auto Name = [&](const char *What, const std::vector<va_t> &Addrs) {
    if (Addrs.empty())
      return;
    Reason += std::string("; ") + What + " at";
    for (size_t I = 0;
         I < Addrs.size() && I < kFunctionAuditAddressPreviewCount; ++I)
      Reason += " 0x" + llvm::utohexstr(Addrs[I]);
    if (Addrs.size() > ::neverd::sdk::kFunctionAuditAddressPreviewCount)
      Reason += " ...";
  };
  std::vector<va_t> Unsupported;
  for (va_t Addr : Audit.UnsupportedInstructions)
    if (!llvm::is_contained(Audit.UnprovenReturns, Addr))
      Unsupported.push_back(Addr);
  Name("unsupported instruction", Unsupported);
  Name("return or tail transfer with unproved return-address ownership",
       Audit.UnprovenReturns);
  Name("decode failure", Audit.DecodeFailures);
  if (!Audit.Diagnostic.empty())
    Reason += "; " + Audit.Diagnostic;
  return Reason;
}

inline std::string missingPipelineFunctionReason(const PipelineResult &Result,
                                                 va_t Entry,
                                                 std::string Message) {
  for (const PipelineFunctionAudit &Audit : Result.FunctionAudits)
    if (Audit.Entry == Entry &&
        Audit.Disposition != PipelineFunctionDisposition::Accepted)
      return Message + " (" + functionAuditReason(Audit) + ")";
  return Message;
}

} // namespace neverd::sdk

#endif
