//===- PDBFunctionNameHint.h - Cheap exact public-name lookup -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_DEBUG_PDBFUNCTIONNAMEHINT_H
#define NEVERD_DEBUG_PDBFUNCTIONNAMEHINT_H

#include "neverd/Common.h"

#include "llvm/ADT/StringRef.h"

#include <filesystem>
#include <optional>

namespace neverd {

/// Resolve one exact PDB public function name before loading a PE image.
/// This is only an optimization hint.  Any malformed, mismatched, absent, or
/// ambiguous input returns no address so normal image/debug loading retains
/// authority over diagnostics and function identity.
std::optional<va_t>
resolvePDBPublicFunctionNameHint(const std::filesystem::path &BinaryPath,
                                 const std::filesystem::path &PDBPath,
                                 llvm::StringRef Name);

} // namespace neverd

#endif // NEVERD_DEBUG_PDBFUNCTIONNAMEHINT_H
