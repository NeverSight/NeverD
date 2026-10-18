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

#include <string>
#include <vector>

namespace neverd::types {

enum class HeaderLibrary : uint8_t {
#define NEVERD_HEADER_LIBRARY(Id, Name, File, Model, ABI, Spelling) Id,
#include "neverd/types/HeaderLibraries.def"
};

/// The library's name, as JSON gives it: windows-kernel-x64.
llvm::StringRef headerLibraryName(HeaderLibrary Library);

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
  /// The header types left out: each holds by value a type whose size the
  /// program gives otherwise, so its layout would not hold, or holds one
  /// left out.  A record left out stays declared, as the types that point
  /// to it name it.
  std::vector<std::string> Conflicts;
};

/// Adds the types and prototypes of \p Headers that \p Library has none of
/// the name of, as HeaderLibrary.h describes.
HeaderMerge mergeHeaderTypes(TypeLibrary &Library, const TypeLibrary &Headers);

} // namespace neverd::types

#endif // NEVERD_TYPES_HEADERLIBRARY_H
