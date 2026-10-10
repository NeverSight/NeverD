#pragma once
#include "AsarEnvelopeFixture.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/SHA256.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <string>
#include <string_view>

namespace neverd::web::test {
inline std::string asarJSON(llvm::json::Value V) {
  std::string S;
  llvm::raw_string_ostream(S) << V;
  return S;
}
inline std::string asarHash(std::string_view Bytes) {
  llvm::SHA256 H;
  H.update(llvm::StringRef(Bytes));
  return llvm::toHex(H.final(), true);
}
inline llvm::json::Object asarIntegrity(std::string_view Data,
                                        uint64_t BlockSize = 4 * 1024 * 1024) {
  llvm::json::Array Blocks;
  for (uint64_t At = 0; At <= Data.size(); At += BlockSize)
    Blocks.emplace_back(asarHash(
        Data.substr(At, std::min<uint64_t>(BlockSize, Data.size() - At))));
  return llvm::json::Object{{"algorithm", "SHA256"},
                            {"hash", asarHash(Data)},
                            {"blockSize", BlockSize},
                            {"blocks", std::move(Blocks)}};
}
inline llvm::json::Object asarFile(std::string_view Data, uint64_t Offset = 0,
                                   bool Unpacked = false) {
  llvm::json::Object O{{"size", Data.size()},
                       {"integrity", asarIntegrity(Data)}};
  if (Unpacked)
    O["unpacked"] = true;
  else
    O["offset"] = std::to_string(Offset);
  return O;
}
inline std::string asarArchive(llvm::json::Object Files,
                               std::string_view Data = {}) {
  return asarBytes(asarJSON(llvm::json::Object{{"files", std::move(Files)}}),
                   Data);
}
inline std::string
asarArchive(std::initializer_list<llvm::json::Object::KV> Files,
            std::string_view Data = {}) {
  return asarArchive(llvm::json::Object(Files), Data);
}
} // namespace neverd::web::test
