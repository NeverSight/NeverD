//===- COFFLLDMapParser.cpp - COFF /lldmap parser ----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Parses COFF /lldmap format (lld-link /lldmap:out.map).
///
/// Format:
///   Address  Size     Align Out     In      Symbol
///   00001000 00000015     4 .text
///   00001000 0000000e     4         test.o:(.text)
///   0020100e 00000000     0                 symbol_name
///
//===----------------------------------------------------------------------===//

#include "MapParsers.h"

#include "neverd/object/SectionNames.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"

namespace neverd {

void parseCOFFLLDMap(llvm::StringRef Content,
                     std::map<va_t, FunctionSym> &Functions) {
  bool InTable = false;
  std::string CurrentOutSection;

  llvm::StringRef Remaining = Content;
  while (!Remaining.empty()) {
    auto [Line, Rest] = Remaining.split('\n');
    Remaining = Rest;

    llvm::StringRef Trimmed = Line.trim();
    if (isLLDMapHeader(Trimmed)) {
      InTable = true;
      continue;
    }

    if (!InTable)
      continue;

    Trimmed = Line.rtrim();
    if (Trimmed.empty())
      continue;

    llvm::SmallVector<llvm::StringRef, 8> Tokens;
    Trimmed.split(Tokens, ' ', -1, false);
    if (Tokens.size() < 3)
      continue;

    unsigned long long Addr;
    if (Tokens[0].getAsInteger(16, Addr))
      continue;

    unsigned long long Size;
    if (Tokens[1].getAsInteger(16, Size))
      continue;

    llvm::StringRef LastToken = Tokens.back();

    if (LastToken.starts_with(".")) {
      CurrentOutSection = LastToken.str();
      continue;
    }

    if (LastToken.contains(":(.") || LastToken.contains(":("))
      continue;

    bool IsCodeSection =
        section_names::isTextSectionName(CurrentOutSection);

    if (!IsCodeSection)
      continue;

    size_t IndentLevel = Line.find_first_not_of(' ');
    bool IsSymbol = IndentLevel == llvm::StringRef::npos ||
                    (Tokens.size() >= 4 && !LastToken.contains(":"));

    if (!IsSymbol)
      continue;

    std::string Name = LastToken.str();
    if (Name.empty() || Name[0] == '(' || Name[0] == '.')
      continue;

    va_t VA = static_cast<va_t>(Addr);
    if (VA != 0 && Functions.find(VA) == Functions.end()) {
      FunctionSym FS;
      FS.Name = std::move(Name);
      FS.Addr = VA;
      FS.Size = Size;
      Functions[VA] = std::move(FS);
    }
  }
}

} // namespace neverd
