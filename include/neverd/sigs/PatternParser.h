//===- PatternParser.h - FLIRT .pat text format parser --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Parser for FLIRT .pat (pattern) text files. Each line describes one
/// function signature with leading hex bytes, mask, CRC, length, and
/// one or more public function names with offsets.
///
/// Format:
///   <hex_pattern> <crc_len> <crc16> <total_len> [:offset name]... [tail]
///
/// Example:
///   558BEC83EC..5356578B7D08 0A 1234 0042 :0000 _main :0010 _helper
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SIGS_PATTERNPARSER_H
#define NEVERD_SIGS_PATTERNPARSER_H

#include "neverd/sigs/Signature.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"

#include <filesystem>
#include <vector>

namespace neverd {
namespace sigs {

class PatternParser {
public:
  /// Parse a single .pat line into a PatternModule.
  static llvm::Expected<PatternModule> parseLine(llvm::StringRef Line);

  /// Parse pattern text as one transaction. Comments, blank lines, and
  /// separators are ignored; every other line must be a valid module.
  static llvm::Expected<std::vector<PatternModule>>
  parseText(llvm::StringRef Text);

  /// Parse a .pat file, returning all valid modules.
  static llvm::Expected<std::vector<PatternModule>>
  parseFile(const std::filesystem::path &Path);

private:
  static bool parseHexByte(llvm::StringRef Hex, uint8_t &Out);
  static llvm::Expected<std::vector<PatternByte>>
  parseHexPattern(llvm::StringRef Pat);
};

} // namespace sigs
} // namespace neverd

#endif // NEVERD_SIGS_PATTERNPARSER_H
