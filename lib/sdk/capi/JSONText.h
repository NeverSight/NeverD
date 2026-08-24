//===- JSONText.h - Safe dynamic text at the C JSON boundary ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Canonicalizes untrusted loader/debug text immediately before public JSON
/// presentation. Raw symbol bytes remain untouched for relocation hashing and
/// runtime dispatch; only the owned presentation spelling is repaired.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SDK_CAPI_JSONTEXT_H
#define NEVERD_SDK_CAPI_JSONTEXT_H

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/JSON.h"

#include <set>
#include <string>

namespace neverd::sdk {

[[nodiscard]] inline std::string jsonSafeText(llvm::StringRef Text) {
  return llvm::json::isUTF8(Text) ? Text.str() : llvm::json::fixUTF8(Text);
}

/// Own and deduplicate repaired spellings whose StringRefs must remain stable
/// through a transactional JSON serialization.
class JSONTextPool {
public:
  [[nodiscard]] llvm::StringRef intern(llvm::StringRef Text) {
    const auto [It, Inserted] = Texts.insert(jsonSafeText(Text));
    (void)Inserted;
    return *It;
  }

private:
  std::set<std::string, std::less<>> Texts;
};

} // namespace neverd::sdk

#endif // NEVERD_SDK_CAPI_JSONTEXT_H
