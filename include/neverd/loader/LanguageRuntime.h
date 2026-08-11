//===- LanguageRuntime.h - Source language and personality identity -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Identifies which language runtime produced an image and which personality
/// routine an address names.
///
/// Two different languages can share one table schema — Rust and C++ both use
/// the Itanium LSDA, and Delphi reuses the Windows registration chain — so a
/// schema alone does not determine how a landing pad behaves.  Recovering the
/// runtime identity separately is what lets NeverD describe a Rust cleanup
/// pad as `Drop` glue rather than as a C++ destructor block, and what keeps a
/// Go frame from being reported as having no exception information at all.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LOADER_LANGUAGERUNTIME_H
#define NEVERD_LOADER_LANGUAGERUNTIME_H

#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/ExceptionInfo.h"

#include "llvm/ADT/StringRef.h"

#include <string>
#include <vector>

namespace neverd {

/// \ref SourceLanguageRuntime and \ref LanguageRuntimeInfo live in
/// `ExceptionCommon.h` so that \ref ExceptionInfo can store the identity its
/// tables were classified with.  This header owns the classification itself.

/// Classify a personality routine from its symbol name.  Recognizes the
/// Windows, Itanium, Rust, and Delphi spellings, including the leading
/// underscore Darwin adds and the `__imp_` prefix a PE import thunk carries.
ExceptionPersonality classifyPersonalityName(llvm::StringRef Name);

/// The runtime a personality belongs to, for the personalities whose identity
/// determines it.  Returns Unknown for personalities shared across languages.
SourceLanguageRuntime getPersonalityRuntime(ExceptionPersonality P);

/// Name the routine at \p Address using symbols, imports, import pointer
/// slots, and relocations.  \p SlotVA is the address the personality pointer
/// was loaded through when the encoding was indirect, and is consulted first
/// because a dynamically bound slot holds no usable value in the file image.
std::string resolveRoutineName(const BinaryImage &Img, va_t Address,
                               va_t SlotVA = 0);

/// Determine which runtime produced \p Img.
LanguageRuntimeInfo detectLanguageRuntime(const BinaryImage &Img);

/// True when \p Name is a Rust symbol in either the legacy (`_ZN..17h<hash>E`)
/// or the v0 (`_R...`) mangling.
bool isRustMangledName(llvm::StringRef Name);

/// Demangle a Rust symbol.  Returns an empty string when \p Name is not a
/// Rust symbol or could not be demangled, so a caller can fall back to the
/// raw name rather than displaying a partially decoded one.
std::string demangleRustName(llvm::StringRef Name);

} // namespace neverd

#endif // NEVERD_LOADER_LANGUAGERUNTIME_H
