//===- LoadOptions.h - How a session reads its input ------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SDK_CAPI_LOADOPTIONS_H
#define NEVERD_SDK_CAPI_LOADOPTIONS_H

#include "neverd/support/BinaryLoading.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"

#include <filesystem>
#include <string>

namespace neverd::sdk {

/// Parse neverd_session_set_load_options JSON: the loader the user chose.
llvm::Expected<LoaderChoice> parseLoadOptions(llvm::StringRef Text);

/// The load options JSON of the loader \p Choice names.
std::string loadOptionsJson(const LoaderChoice &Choice);

/// The loader the input's `load.json` keeps (its project folder's); the file's
/// own format without one.
llvm::Expected<LoaderChoice>
readLoadOptionsSidecar(const std::filesystem::path &Input);

/// Settle the processor a binary file is read as when the user named none:
/// read it from the bytes and keep in \p Choice what was read and why.
/// Fails, naming what the bytes look like, when they name no processor
/// NeverD decodes clearly enough.
llvm::Error settleBinaryFileProcessor(const std::filesystem::path &Input,
                                      LoaderChoice &Choice);

/// Settle the platform whose conventions a binary file's code follows when
/// the user named none: read it from the code, and keep in \p Choice what
/// was read and why, so the file reopens under the same conventions.
void settleBinaryFilePlatform(BinaryImage &Img, LoaderChoice &Choice);

} // namespace neverd::sdk

#endif // NEVERD_SDK_CAPI_LOADOPTIONS_H
