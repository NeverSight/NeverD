//===- PipelineIncomingStack.h - Incoming stack source inputs -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_PIPELINE_PIPELINEINCOMINGSTACK_H
#define NEVERD_PIPELINE_PIPELINEINCOMINGSTACK_H

#include "neverd/ir/SourceTypeHint.h"

#include "llvm/ADT/ArrayRef.h"

#include <map>
#include <optional>
#include <set>

namespace neverd {
struct BinaryImage;
class Decoder;
struct LowFunc;
struct MedFunc;
struct PipelineResult;

bool supportsIncomingStackSources(const BinaryImage &Image);

/// A bounded, complete leaf which only reads full incoming stack words and
/// defines its integer result. The current LowIR must change no other physical
/// register. This describes a source projection, never a machine rewriting ABI.
/// Callers must bind the same physical stack locations before SSA conversion.
std::optional<SourceFunctionTypeHint>
inferIncomingStackLeaf(const BinaryImage &Image, const LowFunc &Low,
                       const MedFunc &Med);

/// Cheap bounded rejection before the ordinary omitted-callee CFG reader.
/// This does not authorize a signature or supply a substitute LowFunc.
bool mayReadIncomingStackLeaf(const BinaryImage &Image, va_t Entry,
                              Decoder &Decoder);

/// Reconstruct the fixed integer projection of authenticated stack locations.
std::optional<SourceFunctionTypeHint>
incomingStackSourceHint(Arch Architecture, llvm::ArrayRef<int64_t> Offsets);

/// Use the existing image/summary-sensitive omitted-callee memo. Only bounded
/// leaf candidates are lifted; machine IR and its signatures stay unchanged.
std::map<va_t, SourceFunctionTypeHint>
readIncomingStackCallees(const BinaryImage &Image, PipelineResult &Result,
                         const std::set<va_t> &Targets);
} // namespace neverd

#endif
