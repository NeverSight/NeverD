//===- PipelineIncomingStack.h - Incoming stack source inputs -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Bounded source-only discovery of complete incoming stack values.

#ifndef NEVERD_PIPELINE_PIPELINEINCOMINGSTACK_H
#define NEVERD_PIPELINE_PIPELINEINCOMINGSTACK_H

#include "neverd/ir/SourceTypeHint.h"

#include "llvm/ADT/ArrayRef.h"

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <utility>

namespace neverd {
struct BinaryImage;
class Decoder;
struct LowFunc;
struct MedFunc;
struct PipelineResult;

bool supportsIncomingStackSources(const BinaryImage &Image);

/// A bounded, complete leaf which reads aligned incoming scalar carriers and
/// defines its integer result. Private saves may preserve frame/link registers;
/// the shared byte analysis must prove their restoration and the entry stack.
/// This describes a source projection, never a machine rewriting ABI. Callers
/// must bind the same physical stack locations before SSA conversion.
std::optional<SourceFunctionTypeHint>
inferIncomingStackLeaf(const BinaryImage &Image, const LowFunc &Low,
                       const MedFunc &Med);

/// Cheap bounded rejection before the ordinary omitted-callee CFG reader.
/// This does not authorize a signature or supply a substitute LowFunc.
bool mayReadIncomingStackLeaf(const BinaryImage &Image, va_t Entry,
                              Decoder &Decoder);

/// Reconstruct the fixed integer projection of authenticated stack locations.
std::optional<SourceFunctionTypeHint>
incomingStackSourceHint(Arch Architecture,
                        llvm::ArrayRef<std::pair<int64_t, uint16_t>> Inputs);

/// Use the existing image/summary-sensitive omitted-callee memo. Only bounded
/// leaf candidates are lifted; machine IR and its signatures stay unchanged.
std::map<va_t, SourceFunctionTypeHint>
readIncomingStackCallees(const BinaryImage &Image, PipelineResult &Result,
                         const std::set<va_t> &Targets);
} // namespace neverd

#endif // NEVERD_PIPELINE_PIPELINEINCOMINGSTACK_H
