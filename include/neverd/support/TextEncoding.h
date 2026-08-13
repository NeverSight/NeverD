//===- TextEncoding.h - external text normalization ---------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SUPPORT_TEXTENCODING_H
#define NEVERD_SUPPORT_TEXTENCODING_H

#include "llvm/ADT/StringRef.h"

#include <string>

namespace neverd {

/// Preserve legal UTF-8 byte-for-byte and render each malformed byte as an
/// uppercase ASCII `\xHH` escape. The result is always valid UTF-8.
std::string escapeInvalidUTF8(llvm::StringRef Text);

} // namespace neverd

#endif // NEVERD_SUPPORT_TEXTENCODING_H
