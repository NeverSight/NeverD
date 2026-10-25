//===- PipelineIncomingStack.h - Incoming stack source inputs -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_PIPELINE_PIPELINEINCOMINGSTACK_H
#define NEVERD_PIPELINE_PIPELINEINCOMINGSTACK_H

#include "neverd/ir/SourceTypeHint.h"

#include <optional>

namespace neverd {
struct BinaryImage;
struct LowFunc;
struct MedFunc;

/// A bounded, complete leaf which only reads full incoming stack words and
/// defines its integer result. The current LowIR must change no other physical
/// register. This describes a source projection, never a machine rewriting ABI.
/// Callers must bind the same physical stack locations before SSA conversion.
std::optional<SourceFunctionTypeHint>
inferIncomingStackLeaf(const BinaryImage &Image, const LowFunc &Low,
                       const MedFunc &Med);
} // namespace neverd

#endif
