//===- Identity.cpp - Shared archive member identities --------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Immutable byte identities and portable member name admission.
//===----------------------------------------------------------------------===//

#include "../Internal.h"

#include "neverd/support/SHA256.h"

#include "llvm/ADT/StringExtras.h"

#include <array>

namespace neverd::web {
bool validUtf8(std::string_view Bytes) {
  return llvm::json::isUTF8(llvm::StringRef(Bytes));
}

std::string sha256(std::string_view Bytes) {
  return llvm::toHex(neverd::sha256(llvm::arrayRefFromStringRef(Bytes)), true);
}

std::string identity(std::string_view Domain,
                     const std::vector<std::string_view> &Fields) {
  SHA256Stream Hash;
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

void validateMemberName(std::string_view Name) {
  if (Name.empty() || Name == "." || Name == ".." || Name.size() > 255 ||
      Name.back() == '.' || Name.back() == ' ' ||
      !llvm::json::isUTF8(llvm::StringRef(Name)))
    throw Error("unsafe_member_name");
  for (unsigned char C : Name)
    if (C < 32 || C == 127 ||
        std::string_view("/\\:*?\"<>|").find(C) != std::string_view::npos)
      throw Error("unsafe_member_name");
  auto Stem = Name.substr(0, Name.find('.'));
  std::string Upper(Stem);
  for (auto &C : Upper)
    if (C >= 'a' && C <= 'z')
      C -= 'a' - 'A';
  // Windows reserves superscript 1, 2 and 3 as COM/LPT device suffixes too.
  // This validation does not need or imply compatibility normalization.
  const auto Suffix =
      Upper.size() > 3 ? std::string_view(Upper).substr(3) : std::string_view{};
  if (Upper == "CON" || Upper == "PRN" || Upper == "AUX" || Upper == "NUL" ||
      ((Upper.starts_with("COM") || Upper.starts_with("LPT")) &&
       ((Suffix.size() == 1 && Suffix[0] >= '1' && Suffix[0] <= '9') ||
        Suffix == "¹" || Suffix == "²" || Suffix == "³")))
    throw Error("unsafe_member_name");
}

} // namespace neverd::web
