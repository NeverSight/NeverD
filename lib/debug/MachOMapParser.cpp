//===- MachOMapParser.cpp - MachO linker map parser --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Parses MachO-style linker MAP files (ld64.lld -map out.map).
///
/// Format:
///   # Path: <binary>
///   # Arch: <arch>
///   # Object files:
///   [  0] linker synthesized
///   # Sections:
///   # Address    Size       Segment  Section
///   0x1000005C0  0x0000004C __TEXT   __text
///   # Symbols:
///   # Address    Size       File  Name
///   0x1000005C0  0x00000001 [  1] _main
///
//===----------------------------------------------------------------------===//

#include "MapParsers.h"

#include "neverd/object/SectionNames.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"

namespace neverd {

void parseMachOMap(llvm::StringRef Content,
                   std::map<va_t, FunctionSym> &Functions) {
  enum class Section { None, ObjectFiles, Sections, Symbols, Dead };
  Section CurSection = Section::None;

  struct MachOSectionRange {
    va_t Addr;
    uint64_t Size;
    std::string Segment;
    std::string Name;
  };
  std::vector<MachOSectionRange> CodeSections;

  llvm::StringRef Remaining = Content;
  while (!Remaining.empty()) {
    auto [Line, Rest] = Remaining.split('\n');
    Remaining = Rest;

    llvm::StringRef Trimmed = Line.trim();
    if (Trimmed.empty())
      continue;

    if (Trimmed == "# Object files:") {
      CurSection = Section::ObjectFiles;
      continue;
    }
    if (Trimmed.starts_with("# Sections:")) {
      CurSection = Section::Sections;
      continue;
    }
    if (Trimmed.starts_with("# Symbols:")) {
      CurSection = Section::Symbols;
      continue;
    }
    if (Trimmed.starts_with("# Dead Stripped Symbols:")) {
      CurSection = Section::Dead;
      continue;
    }

    if (Trimmed.starts_with("#"))
      continue;

    if (CurSection == Section::Sections) {
      llvm::SmallVector<llvm::StringRef, 8> Tokens;
      Trimmed.split(Tokens, '\t', -1, false);
      if (Tokens.size() < 4)
        Trimmed.split(Tokens, ' ', -1, false);
      if (Tokens.size() < 4)
        continue;

      llvm::StringRef AddrStr = Tokens[0].trim();
      llvm::StringRef SizeStr = Tokens[1].trim();
      llvm::StringRef Segment = Tokens[2].trim();
      llvm::StringRef SecName = Tokens[3].trim();

      unsigned long long Addr, Size;
      if (AddrStr.getAsInteger(0, Addr) || SizeStr.getAsInteger(0, Size))
        continue;

      bool IsCode = section_names::isMachOExecutableMapSection(Segment, SecName);
      if (IsCode) {
        MachOSectionRange SR;
        SR.Addr = static_cast<va_t>(Addr);
        SR.Size = Size;
        SR.Segment = Segment.str();
        SR.Name = SecName.str();
        CodeSections.push_back(std::move(SR));
      }
    }

    if (CurSection == Section::Symbols) {
      llvm::SmallVector<llvm::StringRef, 4> Tokens;
      Trimmed.split(Tokens, '\t', -1, false);
      if (Tokens.size() < 3)
        continue;

      llvm::StringRef AddrStr = Tokens[0].trim();
      llvm::StringRef SizeStr = Tokens[1].trim();

      unsigned long long Addr, Size;
      if (AddrStr.getAsInteger(0, Addr) || SizeStr.getAsInteger(0, Size))
        continue;

      llvm::StringRef Name;
      if (Tokens.size() >= 4) {
        Name = Tokens.back().trim();
      } else {
        llvm::StringRef FileAndName = Tokens[2].trim();
        size_t BracketEnd = FileAndName.find(']');
        if (BracketEnd != llvm::StringRef::npos)
          Name = FileAndName.substr(BracketEnd + 1).trim();
        else
          Name = FileAndName;
      }

      va_t VA = static_cast<va_t>(Addr);
      if (VA == 0)
        continue;

      bool InCode = false;
      for (const auto &SR : CodeSections) {
        if (VA >= SR.Addr && VA - SR.Addr < SR.Size) {
          InCode = true;
          break;
        }
      }

      if (!InCode)
        continue;

      std::string SymName = Name.str();
      if (SymName.empty())
        continue;
      if (Name.starts_with("literal string:") ||
          Name.starts_with("helper helper") ||
          Name.starts_with("non-lazy-pointer") ||
          Name.starts_with("compact unwind"))
        continue;

      if (Functions.find(VA) == Functions.end()) {
        FunctionSym FS;
        FS.Name = std::move(SymName);
        FS.Addr = VA;
        FS.Size = Size;
        Functions[VA] = std::move(FS);
      }
    }
  }
}

} // namespace neverd
