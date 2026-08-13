//===- ELFMapParser.cpp - ELF linker map parser ---------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Parses ELF-style linker MAP files (ld.lld --Map=out.map).
///
/// Format:
///     VMA              LMA     Size Align Out     In      Symbol
///     <hex>            <hex>   <hex> <n>  .text
///     <hex>            <hex>   <hex> <n>          input.o:(.text)
///     <hex>            <hex>   <hex> <n>                  symbol_name
///
//===----------------------------------------------------------------------===//

#include "MapParsers.h"

#include "neverd/object/SectionNames.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"

namespace neverd {

void parseELFMap(llvm::StringRef Content,
                 std::map<va_t, FunctionSym> &Functions) {
  bool InTable = false;
  std::string CurrentSection;

  llvm::StringRef Remaining = Content;
  while (!Remaining.empty()) {
    auto [Line, Rest] = Remaining.split('\n');
    Remaining = Rest;

    if (isELFMapHeader(Line)) {
      InTable = true;
      continue;
    }

    if (!InTable)
      continue;

    llvm::StringRef Trimmed = Line.rtrim();
    if (Trimmed.empty())
      continue;

    size_t FirstNonSpace = Trimmed.find_first_not_of(' ');
    if (FirstNonSpace == llvm::StringRef::npos)
      continue;

    llvm::SmallVector<llvm::StringRef, 8> Tokens;
    Trimmed.split(Tokens, ' ', -1, false);
    if (Tokens.empty())
      continue;

    if (Tokens.size() >= 4) {
      unsigned long long VMA;
      if (Tokens[0].getAsInteger(16, VMA))
        continue;

      llvm::StringRef LastToken = Tokens.back();

      if (LastToken.starts_with(".")) {
        CurrentSection = LastToken.str();
        continue;
      }

      if (LastToken.contains(":(.") || LastToken.contains(":("))
        continue;

      bool IsCodeSection =
          section_names::isELFExecutableMapSection(CurrentSection);

      if (IsCodeSection) {
        std::string Name = LastToken.str();
        if (Name.empty() || Name[0] == '(' || Name[0] == '.')
          continue;

        unsigned long long Size;
        if (!Tokens[2].getAsInteger(16, Size)) {
          va_t Addr = static_cast<va_t>(VMA);
          if (Addr != 0 && Functions.find(Addr) == Functions.end()) {
            FunctionSym FS;
            FS.Name = std::move(Name);
            FS.Addr = Addr;
            FS.Size = Size;
            Functions[Addr] = std::move(FS);
          }
        }
      }
    }
  }
}

} // namespace neverd
