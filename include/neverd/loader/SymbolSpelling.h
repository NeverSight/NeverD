//===- SymbolSpelling.h - How a symbol name reads ---------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The two spellings NeverD gives a mangled symbol name.  The readable one is
/// what a reader sees in a comment, a listing or the function list:
/// `QDomNode::nodeType() const`, `core::fmt::write`, `Demo.Box.count.getter`.
/// The stem is the C identifier decompiled C calls the symbol by, for the
/// languages C++ rules do not cover: `core_fmt_write`, `Demo_Box_count_getter`
/// and, as the GNU Objective-C runtime spells methods, `_i_NSString__length`.
///
/// The scheme that mangled a name (SymbolSchemes.def) decides both.  A name
/// no scheme claims -- C, assembly, Go's `fmt.(*pp).doPrintf`, GCC's
/// `foo.constprop.0` -- already reads as the image spells it.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LOADER_SYMBOLSPELLING_H
#define NEVERD_LOADER_SYMBOLSPELLING_H

#include "llvm/ADT/StringRef.h"

#include <cstdint>
#include <string>

namespace neverd {

/// The mangling a symbol name follows.
enum class SymbolScheme : uint8_t {
  None,
#define NEVERD_SYMBOL_SCHEME(Id, Language) Id,
#include "neverd/loader/SymbolSchemes.def"
};

/// The scheme that spells \p Name: the symbol as the image spells it, or its
/// C name without the underscore Mach-O adds.  None for a name no scheme
/// claims.  Only the spelling is read: a name may claim a scheme and still
/// not demangle.
SymbolScheme symbolScheme(llvm::StringRef Name);

/// The source language \p Scheme spells names of (`c++`, `rust`, `swift`,
/// `d`, `objective-c`), or empty for None.
llvm::StringRef symbolSchemeLanguage(SymbolScheme Scheme);

/// \p Name as its source language spells it: `QDomNode::nodeType() const`,
/// `int __cdecl QDomNode::nodeType(void) const`, `core::fmt::write` (a legacy
/// Rust name's hash dropped), `Demo.Box.count.getter`.  Empty when \p Name is
/// not mangled or does not demangle completely, so that it reads as the image
/// spells it rather than as a partial reading.
std::string readableSymbolName(llvm::StringRef Name);

/// The name a list, an identity or a summary shows for \p Name: its readable
/// spelling when it has one, else \p Name as it is.
std::string displaySymbolName(llvm::StringRef Name);

/// A C++ function or object's qualified name, without its signature. Empty
/// for malformed names, other languages and implementation-only symbols.
std::string cxxSourceName(llvm::StringRef Name);

/// Whether a complete Itanium function name encodes a constructor or
/// destructor. This identifies the function; its target ABI decides whether
/// it returns a machine value.
bool isItaniumConstructorOrDestructor(llvm::StringRef Name);

/// Simplify a type supplied by debug information using the same exact
/// standard-library aliases as demangled symbols; custom template arguments
/// remain visible.
std::string readableCxxTypeName(llvm::StringRef Name);

/// The stem of the C identifier that names \p Name, for Rust, Swift, D and
/// Objective-C methods: their path's words joined with underscores
/// (`core_fmt_write`; `String_Write_write_fmt` for a trait's method), and an
/// Objective-C method as the GNU runtime names it (`_i_NSString__length`,
/// `_c_NSObject__alloc`).  Empty for other names, including C++, whose C
/// rules belong to the C backend.  A stem may hold bytes C does not allow in
/// an identifier, such as a Unicode Rust identifier; the backend escapes them.
std::string symbolIdentifierStem(llvm::StringRef Name);

} // namespace neverd

#endif // NEVERD_LOADER_SYMBOLSPELLING_H
