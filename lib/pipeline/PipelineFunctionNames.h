//===- PipelineFunctionNames.h - Debug names before source ABI recovery -===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Name selected functions and their direct callees before either source
/// route recovers call arguments. A callee omitted from this run still has
/// its runtime identity; recovering its name only while printing C is late.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_PIPELINE_FUNCTIONNAMES_H
#define NEVERD_PIPELINE_FUNCTIONNAMES_H

#include "neverd/Common.h"
#include "neverd/debug/DebugContext.h"
#include "neverd/pipeline/Pipeline.h"

#include <map>
#include <set>
#include <string>
#include <utility>

namespace neverd::pipeline_detail {

inline std::map<va_t, std::string>
debugFunctionNames(const PipelineResult &Result,
                   const std::map<va_t, std::string> &Known,
                   const DebugContext *Debug) {
  std::map<va_t, std::string> Names;
  if (!Debug || !Debug->hasInfo())
    return Names;
  std::set<va_t> Entries;
  for (const MedFunc &Function : Result.MedFuncs) {
    Entries.insert(Function.Entry);
    for (const MedBlock &Block : Function.Blocks)
      for (const MedOp &Op : Block.Ops)
        if (Op.Opcode == NdOp::CALL && Op.NumInputs && Op.Inputs[0].isConst() &&
            Op.Inputs[0].ConstVal && Op.Inputs[0].ConstVal != InvalidVA)
          Entries.insert(Op.Inputs[0].ConstVal);
  }
  for (va_t Entry : Entries) {
    const auto Existing = Known.find(Entry);
    if (Existing != Known.end() && !Existing->second.empty() &&
        !isSynthesizedFuncName(Existing->second))
      continue;
    // Use the name-only query: a PDB public must not force local/type streams
    // to load just to identify a runtime routine's existing ABI contract.
    if (auto Name = Debug->functionName(Entry); Name && !Name->empty())
      Names.emplace(Entry, std::move(*Name));
  }
  return Names;
}

} // namespace neverd::pipeline_detail

#endif // NEVERD_PIPELINE_FUNCTIONNAMES_H
