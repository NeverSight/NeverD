//===- TextEncoding.h - External text normalization -------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Lossless repair of malformed external UTF-8 bytes.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SUPPORT_TEXTENCODING_H
#define NEVERD_SUPPORT_TEXTENCODING_H

#include "llvm/ADT/StringRef.h"

#include <string>

namespace neverd {

/// Preserve legal UTF-8 byte-for-byte and render each malformed byte as an
/// uppercase ASCII `\xHH` escape. The result is always valid UTF-8.
std::string escapeInvalidUTF8(llvm::StringRef Text);

/// Apply the same repair to an owned string, preserving its storage when all
/// bytes are already legal UTF-8.
void escapeInvalidUTF8InPlace(std::string &Text);

} // namespace neverd

#endif // NEVERD_SUPPORT_TEXTENCODING_H
