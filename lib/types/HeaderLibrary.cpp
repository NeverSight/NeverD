//===- HeaderLibrary.cpp - Type libraries of system headers ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/types/HeaderLibrary.h"

#include "neverd/types/TypeLayout.h"
#include "neverd/types/TypeParse.h"
#include "neverd/types/TypeRender.h"

#include "llvm/ADT/StringSet.h"

#include <array>
#include <cstddef>
#include <mutex>
#include <optional>

namespace neverd::types {
/// lib/types/headers/<File>.h, as the build embeds it, each in a unit of its
/// own (HeaderLibrarySource.cpp.in).
namespace header_sources {
#define NEVERD_HEADER_LIBRARY(Id, Name, File, Model, ABI, Spelling)            \
  extern const unsigned char File[];                                           \
  extern const std::size_t File##Size;
#include "neverd/types/HeaderLibraries.def"
} // namespace header_sources

namespace {

constexpr size_t LibraryCount = 0
#define NEVERD_HEADER_LIBRARY(Id, Name, File, Model, ABI, Spelling) +1
#include "neverd/types/HeaderLibraries.def"
    ;

/// A library read once, with what reading it met.
struct Read {
  std::once_flag Once;
  std::optional<TypeLibrary> Types;
  std::string Error;
};

std::array<Read, LibraryCount> &reads() {
  static std::array<Read, LibraryCount> All;
  return All;
}

/// The size \p Library gives the type named \p Name, as a member holds it.
std::optional<uint64_t> sizeOfNamed(const TypeLibrary &Library,
                                    llvm::StringRef Name) {
  const LocalType *Type = Library.find(Name);
  return Type ? Library.sizeOf(*makeNamed(Name.str(), Type->Kind))
              : std::nullopt;
}

} // namespace

DataModel headerLibraryModel(HeaderLibrary Library) {
  switch (Library) {
#define NEVERD_HEADER_LIBRARY(Id, Name, File, Model, ABI, Spelling)            \
  case HeaderLibrary::Id:                                                      \
    return DataModel::Model;
#include "neverd/types/HeaderLibraries.def"
  }
  return DataModel::LLP64;
}

RecordABI headerLibraryRecordABI(HeaderLibrary Library) {
  switch (Library) {
#define NEVERD_HEADER_LIBRARY(Id, Name, File, Model, ABI, Spelling)            \
  case HeaderLibrary::Id:                                                      \
    return RecordABI::ABI;
#include "neverd/types/HeaderLibraries.def"
  }
  return RecordABI::Microsoft;
}

llvm::StringRef headerLibraryName(HeaderLibrary Library) {
  switch (Library) {
#define NEVERD_HEADER_LIBRARY(Id, Name, File, Model, ABI, Spelling)            \
  case HeaderLibrary::Id:                                                      \
    return Name;
#include "neverd/types/HeaderLibraries.def"
  }
  return "";
}

llvm::StringRef headerLibrarySource(HeaderLibrary Library) {
  switch (Library) {
#define NEVERD_HEADER_LIBRARY(Id, Name, File, Model, ABI, Spelling)            \
  case HeaderLibrary::Id:                                                      \
    return llvm::StringRef(                                                    \
        reinterpret_cast<const char *>(header_sources::File),                  \
        header_sources::File##Size);
#include "neverd/types/HeaderLibraries.def"
  }
  return "";
}

BuiltinSpelling headerLibrarySpelling(HeaderLibrary Library) {
  switch (Library) {
#define NEVERD_HEADER_LIBRARY(Id, Name, File, Model, ABI, Spelling)            \
  case HeaderLibrary::Id:                                                      \
    return BuiltinSpelling::Spelling;
#include "neverd/types/HeaderLibraries.def"
  }
  return BuiltinSpelling::Portable;
}

const TypeLibrary *headerLibrary(HeaderLibrary Library, std::string *Error) {
  Read &Entry = reads()[size_t(Library)];
  std::call_once(Entry.Once, [&] {
    TypeLibrary Types(headerLibraryModel(Library));
    Types.setRecordABI(headerLibraryRecordABI(Library));
    ParseOptions Options;
    Options.Origin = TypeOrigin::Header;
    const ParseResult Result =
        parseCTypes(headerLibrarySource(Library), Types, Options);
    if (!Result.ok()) {
      const SourceProblem &First = Result.Problems.front();
      Entry.Error = headerLibraryName(Library).str() + ":" +
                    std::to_string(First.Line) + ":" +
                    std::to_string(First.Column) + ": " + First.Message;
      return;
    }
    Entry.Types = std::move(Types);
  });
  if (!Entry.Types && Error)
    *Error = Entry.Error;
  return Entry.Types ? &*Entry.Types : nullptr;
}

HeaderMerge mergeHeaderTypes(TypeLibrary &Library, const TypeLibrary &Headers) {
  HeaderMerge Merge;
  std::vector<std::string> Added;
  for (const LocalType *Type : Headers.types()) {
    if (Library.find(Type->Name))
      continue;
    LocalType Copy = *Type;
    Copy.Origin = TypeOrigin::Header;
    Library.define(std::move(Copy));
    Added.push_back(Type->Name);
  }
  // A header record was laid out with the headers' types it holds by value;
  // one that holds a type the program sizes otherwise goes, as does any type
  // that names one gone.  A typedef has no layout of its own: it names the
  // program's type, as C binds it.
  llvm::StringSet<> Gone;
  for (bool Changed = true; Changed;) {
    Changed = false;
    for (const std::string &Name : Added) {
      if (Gone.contains(Name))
        continue;
      const LocalType *Type = Library.find(Name);
      const bool LaidOut = Type->Record && Type->Record->Defined;
      for (const std::string &Held : valueDependencyNames(*Type))
        if (Gone.contains(Held) ||
            (LaidOut &&
             sizeOfNamed(Library, Held) != sizeOfNamed(Headers, Held))) {
          Gone.insert(Name);
          Merge.Conflicts.push_back(Name);
          Changed = true;
          break;
        }
    }
  }
  // A record that goes stays declared: the types that point to it, which
  // need no layout of it, still name it.
  for (const std::string &Name : Merge.Conflicts) {
    const LocalType *Type = Library.find(Name);
    if (Type && Type->Record &&
        (Type->Kind == TypeKind::Struct || Type->Kind == TypeKind::Union)) {
      LocalType Declared = *Type;
      auto Record = std::make_shared<RecordDef>();
      Record->Union = Type->Record->Union;
      Record->Defined = false;
      Declared.Record = std::move(Record);
      Library.define(std::move(Declared));
      continue;
    }
    Library.remove(Name);
  }
  Merge.Types = Added.size() - Merge.Conflicts.size();
  for (const auto &Entry : Headers.prototypes())
    if (!Library.findPrototype(Entry.first())) {
      Library.definePrototype(Entry.first().str(), Entry.second);
      ++Merge.Prototypes;
    }
  return Merge;
}

} // namespace neverd::types
