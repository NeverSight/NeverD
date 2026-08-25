//===- WindowsEHSemanticDigest.h - Source WinEH identity ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Versioned, canonical semantic identities for reconstructable Windows
/// exception-language records.  These tokens bind an immutable loader graph
/// to the exact LLVM WinEH row that is later emitted for it.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_LLVM_WINDOWSEHSEMANTICDIGEST_H
#define NEVERD_BACKEND_LLVM_WINDOWSEHSEMANTICDIGEST_H

#include "neverd/Common.h"

#include "llvm/MC/BinaryRewrite.h"

#include <cstdint>
#include <optional>

namespace neverd {

struct ExceptionFunction;

namespace windows_eh_semantics {

/// Canonical byte-stream schema used before SHA-256 domain separation.
inline constexpr uint32_t SemanticDigestSchemaVersion = 1;

/// Return the source-issued identity for one native SEH scope.  The digest
/// covers the complete ordered scope graph; Region identifies \p ScopeIndex
/// inside that graph and Clause is zero.
std::optional<llvm::mc_rewrite::RewriteWinEHSemanticToken>
getSEHScopeSemanticToken(const ExceptionFunction &EH, Arch TargetArch,
                         uint32_t ScopeIndex);

/// Return the source-issued identity for one FH3 or FH4 catch clause.  The
/// digest covers the complete ordered FuncInfo graph, including its native
/// encoding; Region and Clause identify the selected try block and catch.
std::optional<llvm::mc_rewrite::RewriteWinEHSemanticToken>
getCxxCatchSemanticToken(const ExceptionFunction &EH, Arch TargetArch,
                         uint32_t TryBlockIndex, uint32_t CatchIndex);

} // namespace windows_eh_semantics
} // namespace neverd

#endif // NEVERD_BACKEND_LLVM_WINDOWSEHSEMANTICDIGEST_H
