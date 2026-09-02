//===- EntryInputPolicy.h - Validate application entry inputs -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Defines the conservative ABI policy used to identify attacker-controlled
/// parameters of language-level application entry points.  A familiar symbol
/// name alone is insufficient: the image architecture, pointer width, calling
/// convention, arity, and every parameter width must match a supported entry
/// signature.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SAFETY_ENTRYINPUTPOLICY_H
#define NEVERD_SAFETY_ENTRYINPUTPOLICY_H

#include "llvm/ADT/StringRef.h"

#include <cstddef>
#include <optional>

namespace neverd {

struct MedFunc;

namespace safety {

struct AnalysisInput;

/// Return the attacker-controlled role of parameter \p ParameterIndex when
/// \p Function is a supported, ABI-valid application entry.  The returned
/// StringRef refers to a static string literal.
std::optional<llvm::StringRef>
applicationEntryParameterSource(const AnalysisInput &Input,
                                const MedFunc &Function, size_t ParameterIndex);

/// Whether a source label returned by applicationEntryParameterSource names a
/// process-entry input rather than an in-function source call.
bool isApplicationEntryParameterSource(llvm::StringRef Source);

} // namespace safety
} // namespace neverd

#endif // NEVERD_SAFETY_ENTRYINPUTPOLICY_H
