//===- GNUldMapParser.cpp - GNU ld linker map parser ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Parses GNU ld MAP files (ld --Map=out.map).
///
/// Format:
///   Linker script and memory map
///   .text           0x0000000000001060      0x115
///    .text          0x0000000000001060       0x26 /lib/Scrt1.o
///                   0x0000000000001060                _start
///    .text          0x0000000000001090       0x15 /tmp/ccXXXXXX.o
///                   0x0000000000001090                main
///
//===----------------------------------------------------------------------===//

#include "MapParsers.h"

#include "neverd/Object/SectionNames.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"

namespace neverd {

void parseGNUldMap(llvm::StringRef Content,
                   std::map<va_t, FunctionSym> &Functions) {
  bool InTable = false;
  std::string CurrentOutSection;

  llvm::StringRef Remaining = Content;
  while (!Remaining.empty()) {
    auto [Line, Rest] = Remaining.split('\n');
    Remaining = Rest;

    if (isGNUldMapHeader(Line)) {
      InTable = true;
      continue;
    }
    if (!InTable)
      continue;

    llvm::StringRef Trimmed = Line.rtrim();
    if (Trimmed.empty())
      continue;

    if (Trimmed.starts_with("LOAD ") || Trimmed.starts_with("OUTPUT(") ||
        Trimmed.starts_with("END GROUP") || Trimmed.starts_with("START GROUP"))
      continue;

    size_t FirstNonSpace = Trimmed.find_first_not_of(' ');
    if (FirstNonSpace == llvm::StringRef::npos)
      continue;

    llvm::SmallVector<llvm::StringRef, 8> Tokens;
    Trimmed.split(Tokens, ' ', -1, false);
    if (Tokens.empty())
      continue;

    if (FirstNonSpace == 0 && Tokens[0].starts_with(".")) {
      CurrentOutSection = Tokens[0].str();
      continue;
    }

    bool IsCodeSection = llvm::StringRef(CurrentOutSection)
                             .starts_with(section_names::elf::Text) ||
                         CurrentOutSection == section_names::elf::Init ||
                         CurrentOutSection == section_names::elf::Fini ||
                         CurrentOutSection == section_names::elf::Plt;

    if (!IsCodeSection)
      continue;

    if (Tokens.size() == 2) {
      unsigned long long Addr;
      if (Tokens[0].getAsInteger(0, Addr))
        continue;

      llvm::StringRef Name = Tokens[1];
      if (Name.starts_with(".") || Name.starts_with("PROVIDE(") ||
          Name.starts_with("*fill*") || Name.contains("="))
        continue;

      va_t VA = static_cast<va_t>(Addr);
      if (VA != 0 && Functions.find(VA) == Functions.end()) {
        FunctionSym FS;
        FS.Name = Name.str();
        FS.Addr = VA;
        Functions[VA] = std::move(FS);
      }
    }
  }
}

} // namespace neverd
