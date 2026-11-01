//===- PipelineSourceInline.h - Source call-body specialization -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Replace an unavailable source call operand by analyzing the actual body,
/// without changing the native callee's signature or incoming stack storage.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_PIPELINE_SOURCEINLINE_H
#define NEVERD_PIPELINE_SOURCEINLINE_H

#include "neverd/ir/high/HighIR.h"
#include "neverd/ir/high/HighSourceMap.h"

#include "llvm/IR/Module.h"

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>
namespace neverd {
struct BinaryImage;
class DebugContext;
class LLVMSourceMap;
struct PipelineResult;
struct PipelineOptions;
struct SourceViewOptions;

struct SourceInlineHighView {
  std::vector<HighFunc> Functions;
  HighSourceMap Sources;
  std::map<std::string, va_t> BodyOrigins;
};

struct SourceInlineResult {
  std::unique_ptr<llvm::Module> Module;
  std::shared_ptr<LLVMSourceMap> Sources;
  std::optional<SourceInlineHighView> High;
};

/// Cheap HighIR gate before asking for an LLVM source proof.
bool hasUnavailableSourceStackCall(const std::vector<HighFunc> &Functions);

/// A request-local original-image C view only. The initial module is never
/// changed. Complete source bodies are recovered together, verified, and
/// inlined only where an unavailable argument has no use in that body.
/// Failure or an exhausted bound leaves the original view authoritative.
SourceInlineResult specializeUnavailableSourceCalls(
    const BinaryImage &Image, const PipelineResult &Original,
    const llvm::Module &Initial, DebugContext *Debug,
    const PipelineOptions &AnalysisOptions, const SourceViewOptions &View,
    const SourceInlineHighView *High = nullptr);
} // namespace neverd

#endif // NEVERD_PIPELINE_SOURCEINLINE_H
