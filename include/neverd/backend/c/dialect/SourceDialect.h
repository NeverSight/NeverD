//===- SourceDialect.h - Pseudocode in its source language ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The C the HighC emitter writes, spelled in the language a function was
/// written in: C++, Rust or Go. The C stays the semantic
/// authority.  A dialect spells exactly what the C says, with C's implicit
/// conversions written out and the names of the source language
/// (`core::fmt::write`, `internal/cpu.Initialize`), and shows a declaration
/// as C, with the reason, where it cannot.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_C_DIALECT_SOURCEDIALECT_H
#define NEVERD_BACKEND_C_DIALECT_SOURCEDIALECT_H

#include "neverd/Common.h"
#include "neverd/loader/ExceptionCommon.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"

#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace neverd {

struct CSourceName;
namespace types {
class TypeLibrary;
}

enum class SourceDialect : uint8_t {
#define NEVERD_SOURCE_DIALECT(Id, Key, Display) Id,
#include "neverd/backend/c/dialect/SourceDialects.def"
};

/// `c`, `cpp`, `rust` or `go`.
llvm::StringRef sourceDialectKey(SourceDialect Dialect);
/// `C`, `C++`, `Rust` or `Go`.
llvm::StringRef sourceDialectDisplayName(SourceDialect Dialect);
std::optional<SourceDialect> sourceDialectFromKey(llvm::StringRef Key);

/// The dialect a runtime's code reads in other than C: C++, Rust or Go.
std::optional<SourceDialect> dialectOfRuntime(SourceLanguageRuntime Runtime);

/// C, plus each source language established by the image's runtime evidence.
std::vector<SourceDialect>
offeredSourceDialects(const LanguageRuntimeInfo &Language);

/// The dialect a function of an image reads best in, among those the image
/// is offered in. C-linkage names in a C++ image keep the image's dialect,
/// as do functions the image does not name (\p Named false).
/// \p HandlerRuntime is the runtime the function's own exception personality
/// belongs to: a function whose `try` a C++ runtime dispatches is C++ code
/// whatever its name, and reads in C++ even where the image's other
/// evidence did not establish that language.
SourceDialect sourceDialectOfFunction(
    llvm::StringRef Symbol, bool Named, const LanguageRuntimeInfo &Language,
    SourceLanguageRuntime HandlerRuntime = SourceLanguageRuntime::Unknown);

/// The program's types a unit's C defines (CSourceMap::LocalTypes), as the
/// dialect's language defines them: the text that stands for those bytes of
/// the C, and how code reaches each member of each record the text defines,
/// by the record's and the member's C names.
struct SourceDialectLocalTypes {
  size_t CBegin = 0;
  size_t CEnd = 0;
  std::string Text;
  struct Hop {
    std::string Name;
    /// A method that returns a pointer to the member.
    bool Accessor = false;
  };
  /// The hops to a member, its own last.  A bit-field's lead to the record
  /// whose methods read and write it, Getter and Setter.
  struct Path {
    std::vector<Hop> Hops;
    std::string Getter, Setter;

    bool bitField() const { return !Getter.empty() && !Setter.empty(); }
  };
  std::map<std::string, std::map<std::string, Path>> Members;
  /// The records the text defines without members, as it only points to
  /// them.
  std::set<std::string> Opaque;
};

struct SourceDialectOptions {
  SourceDialect Dialect = SourceDialect::C;
  Arch TheArch = Arch::X64;
  BinaryFormat Format = BinaryFormat::ELF;
  /// The names the C text spells, from CSourceMap::Names.
  llvm::ArrayRef<CSourceName> Names;
  /// Target header types joined to the program's local types. The reader
  /// obtains declarations of referenced header types from this library;
  /// definitions in the emitted C remain authoritative.
  const types::TypeLibrary *Types = nullptr;
  /// Program types the emitter proved are supplied by matching headers.
  llvm::ArrayRef<std::string> HeaderTypes;
  /// Rust and Go: the program's types, as the language defines them.
  std::optional<SourceDialectLocalTypes> LocalTypes;
};

/// A statement or declaration of the dialect text and the C bytes it spells.
struct SourceDialectPiece {
  size_t CBegin = 0;
  size_t CEnd = 0;
  size_t Begin = 0;
  size_t End = 0;
};

/// A source language's name in the dialect text, which splitting the text
/// into identifiers would not find whole: `core::fmt::write`.
struct SourceDialectName {
  size_t Begin = 0;
  size_t End = 0;
  /// The C identifier the C view spells for it.
  std::string Identifier;
  std::string Symbol;
  std::optional<va_t> Address;
};

struct SourceDialectText {
  std::string Text;
  /// In text order.
  std::vector<SourceDialectPiece> Pieces;
  /// Why each declaration shown as C could not be spelled, in text order.
  std::vector<std::string> Unread;
  /// In text order.
  std::vector<SourceDialectName> Names;

  /// The text that spells the C bytes [CBegin, CEnd): the pieces within
  /// them, or the smallest piece around them.
  std::optional<std::pair<size_t, size_t>> map(size_t CBegin,
                                               size_t CEnd) const;
  /// Map one complete recorded piece, ignoring surrounding C whitespace.
  /// Navigation must not expand a partial piece to an enclosing declaration
  /// or combine pieces across unrecorded text. Ambiguous renderings are
  /// refused.
  std::optional<std::pair<size_t, size_t>>
  mapExact(llvm::StringRef C, size_t CBegin, size_t CEnd) const;
  /// Where the text spelling the C from \p CBegin on starts: the first
  /// piece that starts there or after.
  std::optional<size_t> mapOffset(size_t CBegin) const;
};

/// \p C, a complete HighC emission, spelled in \p Options.Dialect.
SourceDialectText spellInDialect(llvm::StringRef C,
                                 const SourceDialectOptions &Options);

} // namespace neverd

#endif // NEVERD_BACKEND_C_DIALECT_SOURCEDIALECT_H
