//===- WindowsUnicode.h - Counted UTF-16 names and explicit upcase maps
//----===//
// NeverD Decompiler
///
/// \file
/// Counted UTF-16 names and explicit upcase maps.
///

#ifndef NEVERD_EMULATION_WINDOWSUNICODE_H
#define NEVERD_EMULATION_WINDOWSUNICODE_H
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"

#include <cstdint>
#include <string>
#include <vector>
namespace neverd::emulation::windows_unicode {
llvm::Expected<std::vector<uint16_t>> utf16(llvm::StringRef UTF8);
llvm::Expected<std::string> utf8(llvm::ArrayRef<uint16_t> UTF16);
llvm::Expected<std::vector<uint8_t>> bytes(llvm::StringRef UTF8,
                                           bool Terminate = false);
llvm::Error validateUpcase(llvm::ArrayRef<uint16_t> Table);
// The absent table provides ASCII conversion only; non-ASCII comparisons fail.
llvm::Expected<std::string> key(llvm::StringRef UTF8,
                                llvm::ArrayRef<uint16_t> Table);
} // namespace neverd::emulation::windows_unicode
#endif
