//===- TextEncoding.cpp - external text normalization -------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/Support/TextEncoding.h"

#include "llvm/Support/ConvertUTF.h"

namespace neverd {

std::string escapeInvalidUTF8(llvm::StringRef Text) {
  static constexpr char Hex[] = "0123456789ABCDEF";
  const auto *Cursor = reinterpret_cast<const llvm::UTF8 *>(Text.bytes_begin());
  const auto *End = reinterpret_cast<const llvm::UTF8 *>(Text.bytes_end());

  std::string Result;
  Result.reserve(Text.size());
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

} // namespace neverd
