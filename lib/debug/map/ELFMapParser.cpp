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

#include "llvm/ADT/StringRef.h"

namespace neverd {

void parseELFMap(llvm::StringRef Content,
                 std::map<va_t, FunctionSym> &Functions) {
  bool InTable = false;
  bool HasStructuredColumns = false;
  std::string CurrentSection;

  llvm::StringRef Remaining = Content;
  while (!Remaining.empty()) {
    auto [Line, Rest] = Remaining.split('\n');
    Remaining = Rest;

    if (isELFMapHeader(Line)) {
      InTable = true;
      HasStructuredColumns = Line.contains("Out     In      Symbol");
      continue;
    }

    if (!InTable)
      continue;

    llvm::StringRef Trimmed = Line.rtrim();
    if (Trimmed.empty())
      continue;

    // Keep the payload intact: demangled names contain spaces. The gap
    // after Align distinguishes ld.lld's output, input and symbol rows.
    llvm::StringRef Fields[4];
    llvm::StringRef Payload = Trimmed.ltrim(' ');
    size_t Gap = 0;
    for (llvm::StringRef &Field : Fields) {
      auto Parts = Payload.split(' ');
      Field = Parts.first;
      Payload = Parts.second.ltrim(' ');
      Gap = Parts.second.size() - Payload.size() + 1;
    }
    if (Payload.empty())
      continue;

    unsigned long long VMA, LMA, Size, Align;
    if (Fields[0].getAsInteger(16, VMA) || Fields[1].getAsInteger(16, LMA) ||
        Fields[2].getAsInteger(16, Size) || Fields[3].getAsInteger(10, Align))
      continue;

    // writeHeader adds one space; input chunks, byte commands and inner
    // assignments add eight, while symbols add sixteen. These relative
    // gaps work for both address widths and sizes wider than eight digits.
    if (Gap >= 9 && Gap < 17)
      continue;

    if (HasStructuredColumns && Gap < 9) {
      // In the producer's layout these are output sections or top-level
      // assignments. Section names need not begin with a dot.
      if (!Payload.contains('='))
        CurrentSection = Payload.str();
      continue;
    }

    if (Gap < 17) {
      // Also accept the compact maps historically supported here. Only
      // these unindented rows need payload-based classification; a real
      // symbol may legitimately contain parentheses, :( or operator=.
      if (Payload.contains(":(") || Payload.contains('='))
        continue;
      if (Payload.starts_with(".")) {
        if (!Payload.contains(' '))
          CurrentSection = Payload.str();
        continue;
      }
      if (Payload.starts_with("("))
        continue;
    }

    if (!section_names::isELFExecutableMapSection(CurrentSection))
      continue;

    va_t Addr = static_cast<va_t>(VMA);
    if (Addr != 0 && Functions.find(Addr) == Functions.end()) {
      FunctionSym FS;
      FS.Name = Payload.str();
      FS.Addr = Addr;
      FS.Size = Size;
      Functions[Addr] = std::move(FS);
    }
  }
}

} // namespace neverd
