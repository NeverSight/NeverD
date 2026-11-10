//===- VariadicImportStub.h - Stubs of variadic imports ---------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// A stub that jumps to a variadic import (MinGW's `fprintf: jmp
/// [__imp_fprintf]`, a PLT entry of __fprintf_chk) passes every argument on,
/// the variadic ones too.  A C function receives `...` but cannot pass it
/// on, so both C backends print such a stub as a function that hands those
/// arguments to the form of the import that takes them as one value
/// (vfprintf for fprintf), and note an import with no such form.  Its
/// callers call the import itself.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_C_VARIADICIMPORTSTUB_H
#define NEVERD_BACKEND_C_VARIADICIMPORTSTUB_H

#include "CVaList.h"
#include "LibraryPrototype.h"

#include "neverd/libc/LibCNames.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/SymbolDecoration.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/raw_ostream.h"

#include <set>
#include <string>
#include <string_view>

namespace neverd::c_stub {

/// The C name of the variadic import the function at \p Entry of \p Image is
/// a stub of, or empty.
inline std::string variadicImportOfStub(const BinaryImage &Image, va_t Entry) {
  const Import *Imp = Entry ? Image.findImportAt(Entry) : nullptr;
  if (!Imp || Imp->IATAddr == Entry || Imp->Name.empty())
    return {};
  // A PE import entry is the C name; other formats name the symbol.
  const std::string Name =
      importNamesAreCNames(Image.Format)
          ? Imp->Name
          : cNameOfSymbol(Imp->Name, Image.Format, Image.Arch).str();
  if (const libc::LibCPrototype *Prototype =
          libc::libcPrototype(Name, Image.Format))
    return Prototype->Variadic ? Name : std::string();
  // A routine some C libraries alone declare variadic, as Linux's ptrace,
  // is variadic in their programs only.
  if (const libc::LibCVariadicForward *Forward =
          libc::libcVariadicForward(Name);
      Forward && Forward->Format != BinaryFormat::Unknown)
    return libc::variadicIn(*Forward, libc::libcVariantOf(Image)) &&
                   libc::isKnownFunction(Name)
               ? Name
               : std::string();
  return libc::isKnownFunction(Name) &&
                 libc::varArgFixedCount(Name, libc::libcVariantOf(Image)) > 0
             ? Name
             : std::string();
}

/// The name the definition of stub \p Stub at \p Entry of \p Import takes:
/// its own, but never the import's, which the C runtime defines.
inline std::string variadicStubName(llvm::StringRef Stub,
                                    llvm::StringRef Import, va_t Entry) {
  if (Stub != Import)
    return Stub.str();
  return (Import + "_" + llvm::utohexstr(Entry)).str();
}

/// The headers a stub that passes its arguments on through \p Forward needs.
inline void addVariadicStubHeaders(std::set<std::string> &Headers,
                                   const libc::LibCVariadicForward &Forward) {
  Headers.insert("stdarg.h");
  if (!Forward.Header.empty())
    Headers.insert(std::string(Forward.Header));
  // The array of pointers counts them in a size_t.
  if (Forward.TheKind == libc::LibCVariadicForward::Kind::Sentinel)
    Headers.insert("stddef.h");
}

/// `Type Name` in C.
inline std::string stubDeclarator(std::string_view Type, llvm::StringRef Name) {
  std::string Text(Type);
  if (!Text.empty() && Text.back() != '*')
    Text += ' ';
  return Text + Name.str();
}

/// The name a stub gives its fixed parameter \p Index.
inline std::string stubParameterName(const libc::LibCVariadicForward &Forward,
                                     size_t Index) {
  if (Forward.TheKind == libc::LibCVariadicForward::Kind::VaList &&
      Index + 1 == Forward.FixedCount)
    return "format";
  return "arg" + std::to_string(Index);
}

/// Writes \p Stub, a stub of the variadic import \p Import that jumps through
/// \p Slot where the format names the slot, as a function that passes every
/// argument it receives on through \p Forward, or, with no form C can pass
/// them to, as the note that the stub is the import.  Its fixed parameters
/// are of the types the headers of C library \p Variant give them.  A
/// va_list form no header declares, which the unit declares from a runtime
/// table as its calls of it are (`__vfprintf_chk`), is declared so once.
inline void writeVariadicImportStub(llvm::raw_ostream &OS, llvm::StringRef Stub,
                                    llvm::StringRef Import,
                                    llvm::StringRef Slot,
                                    const libc::LibCVariadicForward *Forward,
                                    libc::LibCVariant Variant, Arch TheArch,
                                    BinaryFormat Format) {
  using Kind = libc::LibCVariadicForward::Kind;
  OS << "/* " << Stub << " jumps to the import " << Import;
  if (!Slot.empty())
    OS << " through " << Slot;
  OS << ",\n   which receives every argument passed here, the variadic ones "
        "too";
  if (!Forward) {
    OS << ".\n   C cannot pass those on: the stub is the import, and a call "
          "to it\n   is a call to "
       << Import << ". */\n";
    return;
  }
  const libc::LibCVariadicForward &F = *Forward;
  switch (F.TheKind) {
  case Kind::VaList:
    OS << ":\n   " << F.Target << " takes those as one va_list. */\n";
    break;
  case Kind::Bounded:
    OS << ":\n   " << Import << " reads at most " << unsigned(F.Count)
       << " of them, passed on here one by one. */\n";
    break;
  case Kind::Sentinel:
    OS << ":\n   " << F.Target
       << " takes the pointers up to the null one as an array. */\n";
    break;
  }
  // What the stub calls, declared as it is called: as the unit declares a
  // routine of a runtime table's, which takes the list as a machine word.
  const libc::LibCPrototype *Tabled =
      F.TheKind == Kind::VaList && !libc::headerFor(F.Target)
          ? libc::libcPrototype(F.Target, Format)
          : nullptr;
  if (Tabled) {
    OS << "extern "
       << c_library::declaration(*Tabled, F.Target, TheArch, Format) << ";\n";
  } else {
    OS << "extern " << F.Return << " " << F.Target << "(";
    for (size_t I = 0; I < F.FixedCount; ++I) {
      if (F.TheKind == Kind::Sentinel && I > 0)
        break;
      OS << (I ? ", " : "") << libc::forwardFixedType(F, I, Variant);
    }
    switch (F.TheKind) {
    case Kind::VaList:
      OS << ", va_list";
      break;
    case Kind::Bounded:
      OS << ", ...";
      break;
    case Kind::Sentinel:
      OS << ", char *const *";
      if (F.Environment)
        OS << ", char *const *";
      break;
    }
    OS << ");\n";
  }

  std::string Names;
  OS << F.Return << " " << Stub << "(";
  for (size_t I = 0; I < F.FixedCount; ++I) {
    const std::string Name = stubParameterName(F, I);
    OS << (I ? ", " : "")
       << stubDeclarator(libc::forwardFixedType(F, I, Variant), Name);
    Names += (I ? ", " : "") + Name;
  }
  const std::string Last = stubParameterName(F, F.FixedCount - 1);
  OS << ", ...) {\n";
  OS << "    va_list arguments;\n";
  const bool Returns = F.Return != "void";
  switch (F.TheKind) {
  case Kind::VaList:
    OS << "    va_start(arguments, " << Last << ");\n    ";
    if (Returns)
      OS << stubDeclarator(F.Return, "result") << " = ";
    OS << F.Target << "(" << Names << ", "
       << (Tabled ? c_detail::vaListWord(TheArch, Format, "arguments")
                  : "arguments")
       << ");\n";
    OS << "    va_end(arguments);\n";
    if (Returns)
      OS << "    return result;\n";
    break;
  case Kind::Bounded: {
    OS << "    va_start(arguments, " << Last << ");\n";
    for (unsigned I = 0; I < F.Count; ++I) {
      const std::string Name = "argument" + std::to_string(F.FixedCount + I);
      OS << "    " << stubDeclarator(F.Type, Name) << " = va_arg(arguments, "
         << F.Type << ");\n";
      Names += ", " + Name;
    }
    OS << "    va_end(arguments);\n    " << (Returns ? "return " : "")
       << F.Target << "(" << Names << ");\n";
    break;
  }
  case Kind::Sentinel: {
    const std::string First = stubParameterName(F, 0);
    OS << "    size_t count = 1;\n"
       << "    va_start(arguments, " << Last << ");\n"
       << "    while (va_arg(arguments, const char *))\n"
       << "        ++count;\n"
       << "    va_end(arguments);\n"
       << "    const char *vector[count + 1];\n"
       << "    vector[0] = " << Last << ";\n"
       << "    va_start(arguments, " << Last << ");\n"
       << "    for (size_t i = 1; i <= count; ++i)\n"
       << "        vector[i] = va_arg(arguments, const char *);\n";
    if (F.Environment)
      OS << "    char *const *environment = va_arg(arguments, char *const "
            "*);\n";
    OS << "    va_end(arguments);\n    " << (Returns ? "return " : "")
       << F.Target << "(" << First << ", (char *const *)vector"
       << (F.Environment ? ", environment" : "") << ");\n";
    break;
  }
  }
  OS << "}\n";
}

} // namespace neverd::c_stub

#endif // NEVERD_BACKEND_C_VARIADICIMPORTSTUB_H
