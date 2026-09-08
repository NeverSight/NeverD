#ifndef NEVERD_SDK_CAPI_OBJCNATIVEDEPENDENCIES_H
#define NEVERD_SDK_CAPI_OBJCNATIVEDEPENDENCIES_H

#include "neverd/pipeline/NativeSourceHints.h"
#include "neverd/pipeline/Pipeline.h"

#include <map>
#include <set>
#include <stdexcept>

namespace neverd::sdk {

/// Select local direct-call dependencies from the actual LowIR graph. Symbols
/// never create a callee or an ABI declaration. All evidence comes from this
/// one result; accepted candidates must be re-lifted with their explicit ABI.
inline size_t inferObjCNativeDependencies(
    const BinaryImage &Image, const PipelineResult &Result,
    PipelineOptions &Options, std::map<va_t, std::string> &Diagnostics) {
  if (Result.SourceImage != &Image)
    throw std::invalid_argument(
        "native source dependency evidence belongs to another image");
  std::map<va_t, const LowFunc *> Low;
  std::map<va_t, const MedFunc *> Med;
  std::map<va_t, const HighFunc *> High;
  std::map<va_t, const PipelineFunctionAudit *> Audits;
  for (const auto &Function : Result.LowFuncs)
    Low.emplace(Function.Entry, &Function);
  for (const auto &Function : Result.MedFuncs)
    Med.emplace(Function.Entry, &Function);
  for (const auto &Function : Result.HighFuncs)
    High.emplace(Function.Entry, &Function);
  for (const auto &Audit : Result.FunctionAudits)
    Audits.emplace(Audit.Entry, &Audit);
  std::vector<va_t> Pending;
  for (const auto &Method : Image.ObjCMethods)
    if (Method.TypeHint && Method.Status == "supported")
      Pending.push_back(Method.Implementation);
  std::set<va_t> Seen;
  std::set<va_t> Targets;
  while (!Pending.empty()) {
    const auto Entry = Pending.back();
    Pending.pop_back();
    if (!Seen.insert(Entry).second)
      continue;
    const auto Found = Low.find(Entry);
    if (Found == Low.end())
      continue;
    for (const auto &Block : Found->second->Blocks)
      for (const auto &Operation : Block.Ops)
        if (Operation.Opcode == NdOp::CALL && Operation.NumInputs &&
            Operation.Inputs[0].isConst() &&
            Image.isCodeAddress(Operation.Inputs[0].Offset)) {
          const va_t Target = Operation.Inputs[0].Offset;
          Targets.insert(Target);
          Pending.push_back(Target);
        }
  }
  size_t Added = 0;
  for (va_t Target : Targets) {
    if (Options.SourceTypeHints.count(Target))
      continue;
    const auto M = Med.find(Target);
    const auto H = High.find(Target);
    const auto A = Audits.find(Target);
    if (M == Med.end() || H == High.end() || A == Audits.end()) {
      Diagnostics[Target] = "native callee has no complete pipeline evidence";
      continue;
    }
    if (M->second->SourceTypeHint || H->second->SourceTypeHint)
      continue;
    auto Hint = inferNativeSourceTypeHint(Image, *M->second, *H->second,
                                          *A->second, Diagnostics[Target]);
    if (Hint) {
      Options.SourceTypeHints.emplace(Target, std::move(*Hint));
      ++Added;
    }
  }
  return Added;
}
} // namespace neverd::sdk
#endif
