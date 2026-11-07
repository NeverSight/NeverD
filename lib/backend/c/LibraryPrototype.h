//===- LibraryPrototype.h - C declarations of library routines -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_BACKEND_C_LIBRARYPROTOTYPE_H
#define NEVERD_BACKEND_C_LIBRARYPROTOTYPE_H
#include "neverd/Common.h"
#include "neverd/libc/LibCNames.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringRef.h"

#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace neverd::c_library {
/// Whether \p Declaration, a C library's own declaration of the routine
/// \p Name, names only what every unit that includes headers has: C's own
/// types, <stdint.h>'s, and record and enumeration tags.  Such a unit then
/// declares the routine as its header does, `const` and all, and agrees with
/// the header wherever it includes it too.
inline bool selfContainedDeclaration(
    llvm::StringRef Declaration, llvm::StringRef Name,
    const std::set<std::string, std::less<>> &DeclaredTypes = {}) {
  if (Declaration.empty())
    return false;
  static const std::set<std::string, std::less<>> Known = {
      "void",      "char",       "short",
      "int",       "long",       "signed",
      "unsigned",  "float",      "double",
      "_Bool",     "const",      "volatile",
      "restrict",  "__restrict", "__restrict__",
      "_Noreturn", "struct",     "union",
      "enum",      "int8_t",     "int16_t",
      "int32_t",   "int64_t",    "uint8_t",
      "uint16_t",  "uint32_t",   "uint64_t",
      "intptr_t",  "uintptr_t",  "intmax_t",
      "uintmax_t", "__typeof__", "__builtin_choose_expr",
      "sizeof",    "__cdecl",    "__stdcall"};
  llvm::StringRef Previous;
  for (size_t I = 0; I < Declaration.size();) {
    if (!llvm::isAlpha(Declaration[I]) && Declaration[I] != '_') {
      ++I;
      continue;
    }
    size_t End = I;
    while (End < Declaration.size() &&
           (llvm::isAlnum(Declaration[End]) || Declaration[End] == '_'))
      ++End;
    const llvm::StringRef Word = Declaration.slice(I, End);
    size_t Next = End;
    while (Next < Declaration.size() && Declaration[Next] == ' ')
      ++Next;
    // The routine's name, a tag, and a parameter's name, which a comma,
    // bracket or closing parenthesis follows, name no type.
    const bool Parameter = Next < Declaration.size() &&
                           llvm::StringRef(",)[").contains(Declaration[Next]);
    if (Word != Name && !Parameter && !Known.count(Word) &&
        !DeclaredTypes.count(Word) && Previous != "struct" &&
        Previous != "union" && Previous != "enum")
      return false;
    Previous = Word;
    I = End;
  }
  return true;
}

/// The bytes of C's long in a program for \p TheArch and \p Format: eight
/// on an LP64 target, four on 64-bit Windows (LLP64) and on 32-bit ones.
inline unsigned longBytes(Arch TheArch, BinaryFormat Format) {
  const bool Wide = TheArch == Arch::X64 || TheArch == Arch::AArch64;
  return Wide && Format != BinaryFormat::COFF ? 8 : 4;
}

/// \p Type with each integer keyword spelled in its exact width, as every
/// other type NeverD shows is: `long` is int64_t where long is \p LongBytes
/// of eight and int32_t where of four, `int` int32_t and `unsigned long long`
/// uint64_t, whatever system reads it.  `char` and `long double` stay.
inline std::string exactWidths(std::string_view Type, unsigned LongBytes) {
  const auto isWord = [](char C) { return llvm::isAlnum(C) || C == '_'; };
  const auto isInteger = [](std::string_view Word) {
    return Word == "signed" || Word == "unsigned" || Word == "short" ||
           Word == "int" || Word == "long" || Word == "char";
  };
  std::string Out;
  size_t I = 0;
  while (I < Type.size()) {
    if (!isWord(Type[I])) {
      Out += Type[I++];
      continue;
    }
    size_t End = I;
    while (End < Type.size() && isWord(Type[End]))
      ++End;
    if (!isInteger(Type.substr(I, End - I))) {
      Out += Type.substr(I, End - I);
      I = End;
      continue;
    }
    // The run of integer keywords, spaces between them.
    std::vector<std::string_view> Words;
    size_t RunEnd = I;
    size_t Next = I;
    while (Next < Type.size()) {
      size_t WordEnd = Next;
      while (WordEnd < Type.size() && isWord(Type[WordEnd]))
        ++WordEnd;
      const std::string_view Word = Type.substr(Next, WordEnd - Next);
      if (Word == "double") {
        // long double stays as C spells it.
        Words.clear();
        break;
      }
      if (!isInteger(Word))
        break;
      Words.push_back(Word);
      RunEnd = WordEnd;
      Next = WordEnd;
      while (Next < Type.size() && Type[Next] == ' ')
        ++Next;
    }
    if (Words.empty()) {
      Out += Type.substr(I, End - I);
      I = End;
      continue;
    }
    unsigned Longs = 0;
    bool Unsigned = false, Signed = false, Short = false, Char = false;
    for (const std::string_view Word : Words) {
      Longs += Word == "long";
      Unsigned |= Word == "unsigned";
      Signed |= Word == "signed";
      Short |= Word == "short";
      Char |= Word == "char";
    }
    unsigned Bits = 32;
    if (Char)
      Bits = 8;
    else if (Short)
      Bits = 16;
    else if (Longs >= 2)
      Bits = 64;
    else if (Longs == 1)
      Bits = LongBytes * 8;
    if (Char && !Unsigned && !Signed)
      Out += "char";
    else
      Out += (Unsigned ? "uint" : "int") + std::to_string(Bits) + "_t";
    I = RunEnd;
  }
  return Out;
}

/// \p Type, a parameter or result type of a C library prototype, as C spells
/// it for \p TheArch and \p Format: in exact widths (exactWidths), and the
/// Windows API convention a function pointer carries is stdcall on 32-bit
/// x86 and the only convention elsewhere.
inline std::string typeSpelling(std::string_view Type, Arch TheArch,
                                BinaryFormat Format) {
  std::string Text = exactWidths(Type, longBytes(TheArch, Format));
  const size_t At = Text.find(libc::kWinapiMarker);
  if (At == std::string::npos)
    return Text;
  return Text.replace(At, libc::kWinapiMarker.size(),
                      TheArch == Arch::X86 ? "__attribute__((stdcall)) " : "");
}

/// A function's \p Declarator, `name(parameters)`, returning \p Return: one
/// that returns a function pointer declares itself inside the pointer's own
/// declarator, `int (*_onexit(int (*)(void)))(void)`.
inline std::string declarator(std::string_view Return,
                              std::string_view Declarator) {
  if (const size_t Star = Return.find("*)"); Star != std::string_view::npos)
    return std::string(Return.substr(0, Star + 1)) + std::string(Declarator) +
           std::string(Return.substr(Star + 1));
  std::string Text(Return);
  if (Text.empty() || Text.back() != '*')
    Text += ' ';
  return Text + std::string(Declarator);
}

/// The C declaration of the routine \p Prototype types, named
/// \p Identifier, without `extern` or the closing `;`: `void Sleep(uint32_t)`,
/// stdcall where the Windows API convention is on 32-bit x86, its types in
/// exact widths for \p TheArch and \p Format.
inline std::string declaration(const libc::LibCPrototype &Prototype,
                               std::string_view Identifier, Arch TheArch,
                               BinaryFormat Format) {
  std::string Function(Identifier);
  Function += '(';
  for (unsigned I = 0; I < Prototype.ParamCount; ++I) {
    if (I)
      Function += ", ";
    Function += typeSpelling(Prototype.Params[I], TheArch, Format);
  }
  // ISO C before C23 spells no prototype of `...` alone.
  if (Prototype.Variadic && Prototype.ParamCount)
    Function += ", ...";
  else if (!Prototype.Variadic && !Prototype.ParamCount)
    Function += "void";
  Function += ')';
  std::string Text;
  if (Prototype.Winapi && TheArch == Arch::X86)
    Text += "__attribute__((stdcall)) ";
  return Text +
         declarator(typeSpelling(Prototype.Return, TheArch, Format), Function);
}
} // namespace neverd::c_library
#endif
