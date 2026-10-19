//===- WindowsUnicode.cpp - Counted UTF-16 names and explicit upcase maps
//--===//
// NeverD Decompiler
///
/// \file
/// Counted UTF-16 names and explicit upcase maps.
///

#include "WindowsUnicode.h"

#include "llvm/Support/ConvertUTF.h"
#include "llvm/Support/Endian.h"
namespace neverd::emulation::windows_unicode {
namespace {
llvm::Error invalid(const llvm::Twine &Text) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 "Windows Unicode: " + Text);
}
bool surrogate(uint16_t U) { return U >= 0xd800 && U <= 0xdfff; }
} // namespace
llvm::Expected<std::vector<uint16_t>> utf16(llvm::StringRef Text) {
  std::vector<uint16_t> Units(Text.size());
  if (Text.empty())
    return Units;
  const auto *Begin = reinterpret_cast<const llvm::UTF8 *>(Text.data());
  auto *Out = reinterpret_cast<llvm::UTF16 *>(Units.data());
  const auto *End = Begin + Text.size();
  auto Result = llvm::ConvertUTF8toUTF16(
      &Begin, End, &Out,
      reinterpret_cast<llvm::UTF16 *>(Units.data() + Units.size()),
      llvm::strictConversion);
  if (Result != llvm::conversionOK || Begin != End)
    return invalid("invalid UTF-8 name");
  Units.resize(Out - reinterpret_cast<llvm::UTF16 *>(Units.data()));
  return Units;
}
llvm::Expected<std::string> utf8(llvm::ArrayRef<uint16_t> Units) {
  std::string Text(Units.size() * 3, '\0');
  if (Units.empty())
    return Text;
  const auto *Begin = reinterpret_cast<const llvm::UTF16 *>(Units.data());
  const auto *End = Begin + Units.size();
  auto *Out = reinterpret_cast<llvm::UTF8 *>(Text.data());
  auto Result = llvm::ConvertUTF16toUTF8(
      &Begin, End, &Out,
      reinterpret_cast<llvm::UTF8 *>(Text.data() + Text.size()),
      llvm::strictConversion);
  if (Result != llvm::conversionOK || Begin != End)
    return invalid("invalid UTF-16 name");
  Text.resize(Out - reinterpret_cast<llvm::UTF8 *>(Text.data()));
  return Text;
}
llvm::Expected<std::vector<uint8_t>> bytes(llvm::StringRef Text,
                                           bool Terminate) {
  auto Units = utf16(Text);
  if (!Units)
    return Units.takeError();
  std::vector<uint8_t> B((Units->size() + size_t(Terminate)) * 2);
  for (size_t I = 0; I < Units->size(); ++I)
    llvm::support::endian::write16le(B.data() + I * 2, (*Units)[I]);
  return B;
}
llvm::Error validateUpcase(llvm::ArrayRef<uint16_t> Table) {
  if (Table.empty())
    return llvm::Error::success();
  if (Table.size() != 65536)
    return invalid("upcase_table requires exactly 65536 UTF-16 entries");
  for (size_t U = 0; U < Table.size(); ++U) {
    const uint16_t Upper = Table[U];
    if (Table[Upper] != Upper || (surrogate(U) ? Upper != U : surrogate(Upper)))
      return invalid("upcase_table must be idempotent and preserve UTF-16 "
                     "surrogate units");
    if (U < 128 && Upper != (U >= 'a' && U <= 'z' ? U - 32 : U))
      return invalid(
          "upcase_table must preserve ASCII syntax and uppercase a-z");
    if (U >= 128 && Upper < 128 && !(Upper >= 'A' && Upper <= 'Z'))
      return invalid("upcase_table cannot turn Unicode into ASCII path syntax");
  }
  return llvm::Error::success();
}
llvm::Expected<std::string> key(llvm::StringRef Text,
                                llvm::ArrayRef<uint16_t> Table) {
  auto Units = utf16(Text);
  if (!Units)
    return Units.takeError();
  if (!Table.empty() && Table.size() != 65536)
    return invalid("invalid upcase table size");
  for (auto &U : *Units) {
    if (!Table.empty())
      U = Table[U];
    else if (U >= 128)
      return invalid("non-ASCII filename comparison requires an explicit "
                     "volume upcase_table");
    else if (U >= 'a' && U <= 'z')
      U -= 32;
  }
  return utf8(*Units);
}
} // namespace neverd::emulation::windows_unicode
