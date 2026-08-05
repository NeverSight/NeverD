//===- BinaryUtils.h - Binary patching utilities -------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Codegen-layer utilities for binary patching: object-file creation from
/// memory buffers and cross-format symbol name resolution.
///
/// Format-specific header parsing lives in neverd/Object/*.h (mirrors
/// llvm/Object/).  Low-level endian helpers live in
/// neverd/Support/BinaryEncoding.h.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_CODEGEN_BINARYUTILS_H
#define NEVERD_BACKEND_CODEGEN_BINARYUTILS_H

#include "llvm/ADT/StringRef.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/MemoryBuffer.h"

#include <string>
#include <vector>

namespace neverd {

// ===--------------------------------------------------------------------===//
// Binary-to-ObjectFile helper
// ===--------------------------------------------------------------------===//

/// Create a non-owning MemoryBuffer + LLVM ObjectFile from a binary blob.
/// Returns nullptr on failure (errors are consumed).
inline std::unique_ptr<llvm::object::ObjectFile>
createObjectFromBuffer(const std::vector<uint8_t> &Binary) {
  auto Buf = llvm::MemoryBuffer::getMemBuffer(
      llvm::StringRef(reinterpret_cast<const char *>(Binary.data()),
                      Binary.size()),
      "", false);
  auto ObjOr =
      llvm::object::ObjectFile::createObjectFile(Buf->getMemBufferRef());
  if (!ObjOr) {
    llvm::consumeError(ObjOr.takeError());
    return nullptr;
  }
  return std::move(*ObjOr);
}

// ===--------------------------------------------------------------------===//
// Cross-format symbol name helpers
// ===--------------------------------------------------------------------===//

/// Try \p Name, the leading-underscore variant (MachO convention),
/// and the `__imp_` prefix (COFF dllimport convention).
/// Returns the found key, or empty string.
template <typename MapT>
std::string resolveSymbolAlias(const std::string &Name, const MapT &Map) {
  if (Map.count(Name))
    return Name;

  static constexpr llvm::StringLiteral kImpPrefix("__imp_");

  if (!Name.empty() && Name[0] == '_') {
    std::string NoUnderscore = Name.substr(1);
    if (Map.count(NoUnderscore))
      return NoUnderscore;
  } else {
    std::string WithUnderscore = "_" + Name;
    if (Map.count(WithUnderscore))
      return WithUnderscore;
  }

  llvm::StringRef Ref(Name);
  if (Ref.starts_with(kImpPrefix)) {
    std::string Stripped = Ref.drop_front(kImpPrefix.size()).str();
    if (Map.count(Stripped))
      return Stripped;
  } else {
    std::string WithImp = (kImpPrefix + Ref).str();
    if (Map.count(WithImp))
      return WithImp;
  }

  return {};
}

} // namespace neverd

#endif // NEVERD_BACKEND_CODEGEN_BINARYUTILS_H
