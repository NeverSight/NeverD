//===- ArtifactStore.cpp - Artifact identities and admission -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Artifact identities and admission.
///
//===----------------------------------------------------------------------===//

#include "Internal.h"
#include "JsonReader.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/SHA256.h"
#include "llvm/Support/raw_ostream.h"

#include <array>

namespace neverd::web {
bool validUtf8(std::string_view Bytes) {
  return llvm::json::isUTF8(llvm::StringRef(Bytes));
}

std::string sha256(std::string_view Bytes) {
  llvm::SHA256 Hash;
  Hash.update(llvm::StringRef(Bytes));
  return llvm::toHex(Hash.final(), true);
}

std::string identity(std::string_view Domain,
                     const std::vector<std::string_view> &Fields) {
  llvm::SHA256 Hash;
  auto Field = [&](std::string_view Text) {
    std::array<uint8_t, 8> Size{};
    uint64_t Length = Text.size();
    for (unsigned I = 0; I != 8; ++I)
      Size[7 - I] = uint8_t(Length >> (I * 8));
    Hash.update(Size);
    Hash.update(llvm::StringRef(Text));
  };
  Field("neverd.web.identity.v1");
  Field(Domain);
  for (auto Text : Fields)
    Field(Text);
  return llvm::toHex(Hash.final(), true);
}

std::string json(llvm::json::Value Value) {
  std::string Result;
  llvm::raw_string_ostream(Result) << Value;
  return Result;
}

std::string failure(std::string_view Code) {
  return json(llvm::json::Object{
      {"schema_version", 1},
      {"status", "error"},
      {"error", llvm::json::Object{{"code", std::string(Code)}}}});
}

std::string limitsIdentity(const Limits &Budget) {
  const auto Bytes = std::to_string(Budget.MaxInputBytes);
  const auto Member = std::to_string(Budget.MaxMemberBytes);
  const auto Entries = std::to_string(Budget.MaxEntries);
  const auto Depth = std::to_string(Budget.MaxDepth);
  return identity("limits", {Bytes, Member, Entries, Depth});
}

Limits parseLimits(std::string_view Options) {
  Limits Budget;
  if (Options.empty())
    return Budget;
  // Import options are a flat numeric object. Apply the same structural
  // admission used by container, package and observation metadata readers.
  auto Parsed = parseBoundedJSON(Options, {4096, 1, 32, 4096});
  auto *Object = Parsed.getAsObject();
  if (!Object || Object->getInteger("schema_version") != 1)
    throw Error("unsupported_schema");
  for (const auto &Entry : *Object) {
    auto Key = Entry.first.str();
    if (Key == "schema_version")
      continue;
    auto Number = Entry.second.getAsInteger();
    if (!Number || *Number <= 0)
      throw Error("invalid_limit");
    uint64_t *Target = nullptr, Maximum = 0;
    if (Key == "max_input_bytes") {
      Target = &Budget.MaxInputBytes;
      Maximum = Limits::HardInputBytes;
    } else if (Key == "max_member_bytes") {
      Target = &Budget.MaxMemberBytes;
      Maximum = Limits::HardMemberBytes;
    } else if (Key == "max_entries") {
      Target = &Budget.MaxEntries;
      Maximum = Limits::HardEntries;
    } else if (Key == "max_depth") {
      Target = &Budget.MaxDepth;
      Maximum = Limits::HardDepth;
    } else {
      throw Error("unknown_option");
    }
    if (uint64_t(*Number) > Maximum)
      throw Error("limit_above_capability");
    *Target = uint64_t(*Number);
  }
  return Budget;
}

} // namespace neverd::web
