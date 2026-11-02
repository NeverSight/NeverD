//===- TextEncoding.cpp - External text normalization --------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Shared valid-prefix scanning and byte-preserving UTF-8 repair.
///
//===----------------------------------------------------------------------===//

#include "neverd/support/TextEncoding.h"

#include "neverd/support/ByteScan.h"
#include "neverd/support/Limits.h"

#include "llvm/Support/ConvertUTF.h"

#include <cstddef>

namespace neverd {
namespace {

size_t validUTF8Prefix(llvm::StringRef Text) {
  if (Text.size() >= ::neverd::limits::support::
                         kTextEncodingEscapeInvalidUTF8TextSizeLimit &&
      knownValidUTF8({Text.data(), Text.size()}))
    return Text.size();
  const auto *Begin = reinterpret_cast<const llvm::UTF8 *>(Text.bytes_begin());
  const auto *Cursor = Begin;
  const auto *End = reinterpret_cast<const llvm::UTF8 *>(Text.bytes_end());
  while (Cursor != End) {
    const unsigned SequenceSize = llvm::getUTF8SequenceSize(Cursor, End);
    if (!SequenceSize)
      return Cursor - Begin;
    Cursor += SequenceSize;
  }
  return Text.size();
}

std::string escapeFromOffset(llvm::StringRef Text, size_t Prefix) {
  static constexpr char Hex[] = "0123456789ABCDEF";
  const auto *Cursor =
      reinterpret_cast<const llvm::UTF8 *>(Text.bytes_begin()) + Prefix;
  const auto *End = reinterpret_cast<const llvm::UTF8 *>(Text.bytes_end());

  std::string Result;
  Result.reserve(Text.size());
  Result.append(Text.data(), Prefix);
  while (Cursor != End) {
    unsigned SequenceSize = llvm::getUTF8SequenceSize(Cursor, End);
    if (SequenceSize != 0) {
      Result.append(reinterpret_cast<const char *>(Cursor), SequenceSize);
      Cursor += SequenceSize;
      continue;
    }

    const uint8_t Byte = *Cursor++;
    Result.push_back('\\');
    Result.push_back('x');
    Result.push_back(Hex[Byte >> 4]);
    Result.push_back(Hex[Byte & 0x0f]);
  }
  return Result;
}

} // namespace

std::string escapeInvalidUTF8(llvm::StringRef Text) {
  const size_t Prefix = validUTF8Prefix(Text);
  return Prefix == Text.size() ? Text.str() : escapeFromOffset(Text, Prefix);
}

void escapeInvalidUTF8InPlace(std::string &Text) {
  const size_t Prefix = validUTF8Prefix(Text);
  if (Prefix != Text.size())
    Text = escapeFromOffset(Text, Prefix);
}

} // namespace neverd
