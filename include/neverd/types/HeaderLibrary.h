//===- HeaderLibrary.h - Type libraries of system headers -------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The type libraries NeverD ships (HeaderLibraries.def): the types and
/// prototypes a system's headers declare, which a program's own debug
/// information may lack, as the Windows kernel's PDB lacks every typedef and
/// prototype and a program's lacks those of the routines it imports.  The
/// libraries an image's kind and architecture name
/// (HeaderLibraryImages.def) join its types where the program has none of
/// the name: the program's own debug information describes its build.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_TYPES_HEADERLIBRARY_H
#define NEVERD_TYPES_HEADERLIBRARY_H

#include "neverd/types/LocalTypes.h"

#include "llvm/ADT/StringRef.h"

#include <optional>
#include <string>
#include <vector>

namespace neverd::types {

enum class HeaderLibrary : uint8_t {
#define NEVERD_HEADER_LIBRARY(Id, Name, File, Model, ABI, Spelling) Id,
#include "neverd/types/HeaderLibraries.def"
};

/// The library's name, as JSON gives it: windows-kernel-x64.
llvm::StringRef headerLibraryName(HeaderLibrary Library);
/// The library of the name headerLibraryName gives, if one has it.
std::optional<HeaderLibrary> headerLibraryNamed(llvm::StringRef Name);

/// How C's preprocessor tells that a compiler's headers are those of the
/// system a library was read from (HeaderLibrarySystems.def): each a
/// condition, Probe a header, Feature a macro.
struct HeaderSystem {
  llvm::StringRef Name;
  llvm::StringRef Platform;
  llvm::StringRef Arch;
  llvm::StringRef Test;
  llvm::StringRef Probe;
  llvm::StringRef Feature;
  llvm::StringRef Configured;
};
/// The system whose own headers declare \p Library's types; none for a
/// library no system's headers give.
std::optional<HeaderSystem> headerLibrarySystem(HeaderLibrary Library);

/// The data model the library is laid out for.
DataModel headerLibraryModel(HeaderLibrary Library);

/// The record ABI the library's records are laid out by.
RecordABI headerLibraryRecordABI(HeaderLibrary Library);

/// The library's declarations as C, as NeverD embeds them.
llvm::StringRef headerLibrarySource(HeaderLibrary Library);

enum class BuiltinSpelling : uint8_t;
/// The spelling the library's headers name builtins in, which shows one of
/// its declarations as they read it: Windows' ULONG.
BuiltinSpelling headerLibrarySpelling(HeaderLibrary Library);

/// The library read from its declarations the first time it is asked for,
/// every type's origin Header; null with \p Error when they do not read.
/// Safe from any thread.
const TypeLibrary *headerLibrary(HeaderLibrary Library,
                                 std::string *Error = nullptr);

/// What joining a header library to a program's types did.
struct HeaderMerge {
  size_t Types = 0;
  size_t Prototypes = 0;
  /// The header types left out: an alias conflicts with a primary function,
  /// a required alias was removed, or a by-value dependency changed layout.
  /// A record left out stays declared, as the types that point to it name it.
  std::vector<std::string> Conflicts;
};

/// Adds the types and prototypes of \p Headers that \p Library has none of
/// the name of, as HeaderLibrary.h describes. Typedefs and function names
/// share the ordinary C namespace; plain tags remain separate. Prototypes
/// whose by-value layouts change or whose named dependencies were removed
/// are omitted.
/// A known Windows kernel provider can complete its opaque executive pointer
/// aliases from matching program PDB records. Existing program aliases win.
HeaderMerge
mergeHeaderTypes(TypeLibrary &Library, const TypeLibrary &Headers,
                 std::optional<HeaderLibrary> Provider = std::nullopt);

} // namespace neverd::types

#endif // NEVERD_TYPES_HEADERLIBRARY_H
