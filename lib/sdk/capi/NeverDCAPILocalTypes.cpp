//===- NeverDCAPILocalTypes.cpp - C API: local types ----------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The program's named types, read from its PDB the first time a frontend
/// asks, as a list, as one type laid out, and as source in a language.
///
//===----------------------------------------------------------------------===//

#include "neverd/sdk/NeverDCAPILocalTypes.h"

#include "JSONText.h"
#include "SessionImpl.h"

#include "neverd/debug/DWARFLoader.h"
#include "neverd/debug/PDBLoader.h"
#include "neverd/loader/ELF/ELFOperatingSystem.h"
#include "neverd/loader/MachO/DarwinOperatingSystem.h"
#include "neverd/support/FilePath.h"
#include "neverd/support/ProjectFiles.h"
#include "neverd/support/ProjectWriteLock.h"
#include "neverd/types/DWARFTypeImport.h"
#include "neverd/types/TypeImport.h"
#include "neverd/types/TypeJSON.h"
#include "neverd/types/TypeParse.h"
#include "neverd/types/TypeRender.h"

#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/BinaryFormat/COFF.h"
#include "llvm/DebugInfo/DWARF/DWARFContext.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBuffer.h"

#ifdef IMAGE_SUBSYSTEM_NATIVE
#undef IMAGE_SUBSYSTEM_NATIVE
#endif
#ifdef IMAGE_SUBSYSTEM_WINDOWS_GUI
#undef IMAGE_SUBSYSTEM_WINDOWS_GUI
#endif
#ifdef IMAGE_SUBSYSTEM_WINDOWS_CUI
#undef IMAGE_SUBSYSTEM_WINDOWS_CUI
#endif
#ifdef IMAGE_FILE_EXECUTABLE_IMAGE
#undef IMAGE_FILE_EXECUTABLE_IMAGE
#endif
#ifdef IMAGE_FILE_DLL
#undef IMAGE_FILE_DLL
#endif

using namespace neverd;
using namespace neverd::sdk;

namespace {

/// The data model an image without types of its own lays types out in.
types::DataModel dataModelOf(const BinaryImage &Img) {
  if (Img.Bits == Bitness::Bits32)
    return types::DataModel::ILP32;
  return Img.abiFormat() == BinaryFormat::COFF ? types::DataModel::LLP64
                                               : types::DataModel::LP64;
}

/// The DWARF a Windows program's own sections hold, which MinGW's GCC and
/// Clang write, as an object to read it from; none where it holds none, or
/// where the load kept no copy of the file.
llvm::Expected<std::unique_ptr<llvm::object::ObjectFile>>
windowsDWARF(const BinaryImage &Img) {
  if (Img.abiFormat() != BinaryFormat::COFF || Img.Raw.empty() ||
      !Img.hasSection(".debug_info"))
    return nullptr;
  return llvm::object::ObjectFile::createObjectFile(llvm::MemoryBufferRef(
      llvm::StringRef(reinterpret_cast<const char *>(Img.Raw.data()),
                      Img.Raw.size()),
      "image"));
}

/// How \p Img's compilers laid its records out: as Microsoft's for a
/// Windows program, and for any other as its architecture's System V ABI
/// says, which for a 32-bit one its data model does not; Apple's 64-bit ARM
/// and Android's i386 keep a long double that is a double.  A Windows
/// program's own DWARF, \p ImageDWARF, was MinGW's, which keeps the x87's
/// long double on x64 and x86, unless its long double is a double, as
/// clang-cl's is; one that names no long double is taken as MinGW's.
types::RecordABI recordABIOf(const BinaryImage &Img,
                             llvm::DWARFContext *ImageDWARF = nullptr) {
  if (Img.abiFormat() == BinaryFormat::COFF) {
    if (!ImageDWARF || types::dwarfLongDoubleBytes(*ImageDWARF) == 8u)
      return types::RecordABI::Microsoft;
    switch (Img.Arch) {
    case Arch::X64:
      return types::RecordABI::MinGW64;
    case Arch::X86:
      return types::RecordABI::MinGW32;
    default:
      return types::RecordABI::Microsoft;
    }
  }
  if (Img.Bits != Bitness::Bits32)
    return Img.abiFormat() == BinaryFormat::MachO && Img.Arch == Arch::AArch64
               ? types::RecordABI::AppleARM64
               : types::RecordABI::Itanium;
  switch (Img.Arch) {
  case Arch::X86:
    return linuxLibC(Img) == LinuxLibC::Bionic ? types::RecordABI::AndroidI386
                                               : types::RecordABI::I386SystemV;
  case Arch::ARM:
    return types::RecordABI::AAPCS32;
  default:
    return types::RecordABI::Itanium;
  }
}

/// An empty library for \p Img's types.
std::unique_ptr<types::TypeLibrary>
libraryFor(const BinaryImage &Img, llvm::DWARFContext *ImageDWARF = nullptr) {
  auto Library = std::make_unique<types::TypeLibrary>(dataModelOf(Img));
  Library->setRecordABI(recordABIOf(Img, ImageDWARF));
  return Library;
}

/// The PE subsystem of the session's image, read from its file's optional
/// header; none for an image that is no PE file on disk.
std::optional<uint16_t> peSubsystem(const Session &S) {
  if (S.Img.Format != BinaryFormat::COFF || S.FilePath.empty())
    return std::nullopt;
  auto Head = llvm::MemoryBuffer::getFileSlice(pathToUTF8(S.FilePath), 4096, 0);
  if (!Head)
    return std::nullopt;
  const llvm::StringRef Bytes = (*Head)->getBuffer();
  const auto read = [&](size_t At, size_t Size) -> std::optional<uint32_t> {
    if (At + Size > Bytes.size())
      return std::nullopt;
    uint32_t Value = 0;
    for (size_t I = 0; I < Size; ++I)
      Value |= uint32_t(uint8_t(Bytes[At + I])) << (8 * I);
    return Value;
  };
  // MZ, then at e_lfanew PE\0\0, the file header and the optional header,
  // whose Subsystem both PE32 and PE32+ keep 68 bytes in.
  const std::optional<uint32_t> Lfanew = read(0x3C, 4);
  if (!Bytes.starts_with("MZ") || !Lfanew || read(*Lfanew, 4) != 0x4550)
    return std::nullopt;
  const std::optional<uint32_t> Subsystem = read(*Lfanew + 4 + 20 + 68, 2);
  return Subsystem ? std::optional<uint16_t>(uint16_t(*Subsystem))
                   : std::nullopt;
}

/// The kinds of image the header libraries describe
/// (HeaderLibraryImages.def).
enum class HeaderImageKind : uint8_t {
#define NEVERD_HEADER_IMAGE_KIND(Kind) Kind,
#include "neverd/types/HeaderLibraryImages.def"
};

/// The kind of the session's image, as its headers tell it; none for an
/// image no header library describes.
std::optional<HeaderImageKind> headerImageKind(const Session &S) {
  if (const std::optional<uint16_t> Subsystem = peSubsystem(S))
    switch (*Subsystem) {
#define NEVERD_HEADER_PE_SUBSYSTEM(Subsystem, Kind)                            \
  case Subsystem:                                                              \
    return HeaderImageKind::Kind;
#include "neverd/types/HeaderLibraryImages.def"
    }
  if (const std::optional<LinuxLibC> LibC = linuxLibC(S.Img)) {
#define NEVERD_HEADER_LINUX_LIBC(Library, Kind)                                \
  if (*LibC == LinuxLibC::Library)                                             \
    return HeaderImageKind::Kind;
#include "neverd/types/HeaderLibraryImages.def"
  }
  if (const std::optional<DarwinPlatform> Platform = darwinPlatform(S.Img)) {
#define NEVERD_HEADER_DARWIN_PLATFORM(Name, Kind)                              \
  if (*Platform == DarwinPlatform::Name)                                       \
    return HeaderImageKind::Kind;
#include "neverd/types/HeaderLibraryImages.def"
  }
  if (const std::optional<ELFSystem> System = elfSystem(S.Img)) {
#define NEVERD_HEADER_ELF_SYSTEM(Name, Kind)                                   \
  if (*System == ELFSystem::Name)                                              \
    return HeaderImageKind::Kind;
#include "neverd/types/HeaderLibraryImages.def"
  }
  return std::nullopt;
}

/// The header libraries the session's image joins by its kind and
/// architecture (HeaderLibraryImages.def), in the order the rows list them.
std::vector<types::HeaderLibrary> imageLibraries(const Session &S) {
  std::vector<types::HeaderLibrary> Libraries;
  const std::optional<HeaderImageKind> Kind = headerImageKind(S);
  if (!Kind)
    return Libraries;
#define NEVERD_HEADER_LIBRARY_IMAGE(ImageKind, Machine, Library)               \
  if (*Kind == HeaderImageKind::ImageKind && S.Img.Arch == Arch::Machine &&    \
      !llvm::is_contained(Libraries, types::HeaderLibrary::Library))           \
    Libraries.push_back(types::HeaderLibrary::Library);
#include "neverd/types/HeaderLibraryImages.def"
  return Libraries;
}

/// Whether \p Library's records lay out as the program's: its data model,
/// and the record ABI a program of the session's joins libraries of.
bool compatibleLibrary(const Session &S, types::HeaderLibrary Library) {
  return types::headerLibraryModel(Library) == S.LocalTypes->dataModel() &&
         types::headerLibraryRecordABI(Library) ==
             types::libraryRecordABIOf(S.LocalTypes->recordABI());
}

/// Joins the header libraries that describe the session's image
/// (HeaderLibraryImages.def) to its types, after the program's own, in the
/// order the rows list them, but those the user left out, then those the
/// user added (libraries.json), each where its records lay out as the
/// program's.  One the user added that lays out otherwise, as a
/// libraries.json edited by hand may name, joins nothing and says why.
void joinHeaders(Session &S) {
  S.LocalTypeEntryDeclaration = S.platformEntryDeclaration();
  const auto join = [&](types::HeaderLibrary Library, bool Added) {
    if (llvm::any_of(S.LocalTypeHeaders,
                     [&](const Session::JoinedHeaders &Joined) {
                       return Joined.Library == Library;
                     }))
      return;
    Session::JoinedHeaders Joined{Library, {}, {}};
    if (!compatibleLibrary(S, Library)) {
      if (!Added)
        return;
      Joined.Error = "its records lay out otherwise than the program's";
    } else if (const types::TypeLibrary *Headers =
                   types::headerLibrary(Library, &Joined.Error)) {
      Joined.Merge = types::mergeHeaderTypes(*S.LocalTypes, *Headers, Library);
    }
    S.LocalTypeHeaders.push_back(std::move(Joined));
  };
  for (types::HeaderLibrary Library : imageLibraries(S))
    if (!llvm::is_contained(S.TypeLibraryChoice.Removed, Library))
      join(Library, false);
  for (types::HeaderLibrary Library : S.TypeLibraryChoice.Added)
    join(Library, true);
  // The entry's type, where the image's subsystem says which it is and a
  // library joined to declare it.
  if (S.LocalTypeHeaders.empty())
    return;
  if (const std::optional<uint16_t> Subsystem = peSubsystem(S))
    switch (*Subsystem) {
#define NEVERD_HEADER_PE_ENTRY_TYPE(Subsystem, Type)                           \
  case Subsystem:                                                              \
    S.LocalTypeEntryType = Type;                                               \
    break;
#include "neverd/types/HeaderLibraryImages.def"
    }
}

/// Applies \p Declaration to \p Library: the types its source declares, or
/// the removal of those it names.
types::ParseResult apply(const Session::TypeDeclaration &Declaration,
                         types::TypeLibrary &Library) {
  types::ParseResult Result;
  if (!Declaration.Forget.empty()) {
    for (const std::string &Name : Declaration.Forget)
      if (!Library.remove(Name))
        Result.Problems.push_back({0, 0, "no type is named " + Name});
    return Result;
  }
  types::ParseOptions Options;
  Options.Lang = Declaration.Lang;
  Options.Origin = types::TypeOrigin::User;
  return types::parseTypes(Declaration.Source, Library, Options);
}

/// The user's declarations, read again over freshly read types.  One the
/// program's types no longer admit, as those of another PDB may not, is
/// left out, and what it met is kept to report.
void redeclare(Session &S) {
  S.DeclarationProblems.clear();
  for (size_t I = 0; I < S.TypeDeclarations.size(); ++I)
    for (types::SourceProblem &Problem :
         apply(S.TypeDeclarations[I], *S.LocalTypes).Problems)
      S.DeclarationProblems.emplace_back(I, std::move(Problem));
}

std::filesystem::path declarationsPath(const Session &S) {
  return project_files::path(S.projectInputPath(), project_files::File::Types);
}

std::filesystem::path librariesPath(const Session &S) {
  return project_files::path(S.projectInputPath(),
                             project_files::File::TypeLibraries);
}

/// The libraries \p Value's \p Key names, or none with \p Error.
std::optional<std::vector<types::HeaderLibrary>>
readLibraries(const llvm::json::Object &Value, llvm::StringRef Key,
              std::string &Error) {
  std::vector<types::HeaderLibrary> Libraries;
  const llvm::json::Value *Names = Value.get(Key);
  if (!Names)
    return Libraries;
  const llvm::json::Array *List = Names->getAsArray();
  if (!List) {
    Error = ("\"" + Key + "\" must be an array of library names").str();
    return std::nullopt;
  }
  for (const llvm::json::Value &Entry : *List) {
    const std::optional<llvm::StringRef> Name = Entry.getAsString();
    const std::optional<types::HeaderLibrary> Library =
        Name ? types::headerLibraryNamed(*Name) : std::nullopt;
    if (!Library) {
      Error = Name
                  ? "no type library is named " + jsonSafeText(*Name)
                  : ("\"" + Key + "\" must be an array of library names").str();
      return std::nullopt;
    }
    if (!llvm::is_contained(Libraries, *Library))
      Libraries.push_back(*Library);
  }
  return Libraries;
}

/// The choice \p Value says, {"added":[...],"removed":[...]}, or none with
/// \p Error.
std::optional<Session::LibraryChoice>
readLibraryChoice(const llvm::json::Value &Value, std::string &Error) {
  const llvm::json::Object *Object = Value.getAsObject();
  if (!Object) {
    Error = "the type libraries must be an object of \"added\" and "
            "\"removed\"";
    return std::nullopt;
  }
  Session::LibraryChoice Choice;
  std::optional<std::vector<types::HeaderLibrary>> Added =
      readLibraries(*Object, "added", Error);
  if (!Added)
    return std::nullopt;
  std::optional<std::vector<types::HeaderLibrary>> Removed =
      readLibraries(*Object, "removed", Error);
  if (!Removed)
    return std::nullopt;
  Choice.Added = std::move(*Added);
  Choice.Removed = std::move(*Removed);
  return Choice;
}

llvm::json::Object libraryChoiceJson(const Session::LibraryChoice &Choice) {
  llvm::json::Array Added, Removed;
  for (types::HeaderLibrary Library : Choice.Added)
    Added.push_back(types::headerLibraryName(Library));
  for (types::HeaderLibrary Library : Choice.Removed)
    Removed.push_back(types::headerLibraryName(Library));
  return llvm::json::Object{{"added", std::move(Added)},
                            {"removed", std::move(Removed)}};
}

llvm::json::Array declarationRows(const Session &S) {
  llvm::json::Array Rows;
  for (const Session::TypeDeclaration &Declaration : S.TypeDeclarations) {
    if (!Declaration.Forget.empty()) {
      llvm::json::Array Names;
      for (const std::string &Name : Declaration.Forget)
        Names.push_back(jsonSafeText(Name));
      Rows.push_back(llvm::json::Object{{"forget", std::move(Names)}});
      continue;
    }
    Rows.push_back(
        llvm::json::Object{{"language", types::languageId(Declaration.Lang)},
                           {"source", jsonSafeText(Declaration.Source)}});
  }
  return Rows;
}

/// The declarations \p Rows holds, or none with \p Error.
std::optional<std::vector<Session::TypeDeclaration>>
parseDeclarations(const llvm::json::Value &Rows, std::string &Error) {
  const llvm::json::Array *List = Rows.getAsArray();
  if (!List) {
    Error = "the type declarations are no list";
    return std::nullopt;
  }
  std::vector<Session::TypeDeclaration> Declarations;
  for (const llvm::json::Value &Row : *List) {
    const llvm::json::Object *Object = Row.getAsObject();
    Session::TypeDeclaration Declaration;
    if (Object && Object->get("forget")) {
      const llvm::json::Array *Names = Object->getArray("forget");
      if (!Names || Names->empty()) {
        Error = "a removal names no types";
        return std::nullopt;
      }
      for (const llvm::json::Value &Name : *Names) {
        std::optional<llvm::StringRef> Text = Name.getAsString();
        if (!Text || Text->empty()) {
          Error = "a removal names a type by no text";
          return std::nullopt;
        }
        Declaration.Forget.push_back(Text->str());
      }
      Declarations.push_back(std::move(Declaration));
      continue;
    }
    std::optional<llvm::StringRef> Language =
        Object ? Object->getString("language") : std::nullopt;
    std::optional<types::Language> Lang =
        Language ? types::languageNamed(*Language) : std::nullopt;
    std::optional<llvm::StringRef> Source =
        Object ? Object->getString("source") : std::nullopt;
    if (!Lang || !Source) {
      Error = "a type declaration needs a language (c, cpp, rust or go) and "
              "its source";
      return std::nullopt;
    }
    Declaration.Lang = *Lang;
    Declaration.Source = Source->str();
    Declarations.push_back(std::move(Declaration));
  }
  return Declarations;
}

/// Makes the types \p Context's units define, read into \p Library, the
/// session's local types.
llvm::Error adoptDWARFTypes(Session &S, llvm::DWARFContext &Context,
                            std::unique_ptr<types::TypeLibrary> Library) {
  auto Read = types::importDWARFTypes(Context, *Library);
  if (!Read)
    return Read.takeError();
  S.LocalTypes = std::move(Library);
  S.LocalTypeStats.Structs = Read->Structs;
  S.LocalTypeStats.Unions = Read->Unions;
  S.LocalTypeStats.Enums = Read->Enums;
  S.LocalTypeStats.DeclaredOnly = Read->DeclaredOnly;
  S.LocalTypeSkipped = std::move(Read->Skipped);
  S.LocalTypeConflicts = std::move(Read->Conflicts);
  S.LocalTypesSource = "dwarf";
  return llvm::Error::success();
}

/// The session's local types, read from its PDB the first time; null with
/// the session's error when they cannot be read.
types::TypeLibrary *localTypes(Session &S) {
  if (S.LocalTypes)
    return S.LocalTypes.get();
  if (S.DbgKind == DebugInfoKind::DWARF && S.Dbg) {
    if (llvm::Error Err =
            static_cast<const DWARFDebugContext &>(*S.Dbg).withDWARFContext(
                [&](llvm::DWARFContext &Context) {
                  return adoptDWARFTypes(S, Context, libraryFor(S.Img));
                })) {
      S.setError("cannot read the DWARF's types: " +
                 llvm::toString(std::move(Err)));
      return nullptr;
    }
    joinHeaders(S);
    redeclare(S);
    return S.LocalTypes.get();
  }
  if (S.DbgKind != DebugInfoKind::PDB || !S.Dbg) {
    // A Windows program's own DWARF, where no PDB describes it: its types,
    // though the session reads nothing else of it, under the record ABI its
    // long double tells.
    auto Object = windowsDWARF(S.Img);
    if (!Object) {
      S.setError("cannot read the image's DWARF: " +
                 llvm::toString(Object.takeError()));
      return nullptr;
    }
    if (*Object) {
      const std::unique_ptr<llvm::DWARFContext> Context =
          llvm::DWARFContext::create(**Object);
      if (llvm::Error Err =
              adoptDWARFTypes(S, *Context, libraryFor(S.Img, Context.get()))) {
        S.setError("cannot read the DWARF's types: " +
                   llvm::toString(std::move(Err)));
        return nullptr;
      }
    } else {
      S.LocalTypes = libraryFor(S.Img);
      S.LocalTypesSource = "none";
    }
    joinHeaders(S);
    redeclare(S);
    return S.LocalTypes.get();
  }
  types::PDBTypeImportStats Stats;
  std::unique_ptr<types::TypeLibrary> Library;
  // The PDB the session holds open; a PDB 2.00 file it does not keep is
  // read again from its path.
  llvm::Error Err = static_cast<const PDBDebugContext &>(*S.Dbg).withPDBFile(
      [&](llvm::pdb::PDBFile &File) -> llvm::Error {
        auto Read = types::importPDBTypes(File, &Stats);
        if (!Read)
          return Read.takeError();
        Library = std::move(*Read);
        return llvm::Error::success();
      });
  if (Err && !Library) {
    llvm::consumeError(std::move(Err));
    auto Read = types::importPDBTypes(S.DbgPath.string(), &Stats);
    if (!Read) {
      S.setError("cannot read the PDB's types: " +
                 llvm::toString(Read.takeError()));
      return nullptr;
    }
    Library = std::move(*Read);
  } else if (Err) {
    S.setError("cannot read the PDB's types: " +
               llvm::toString(std::move(Err)));
    return nullptr;
  }
  S.LocalTypes = std::move(Library);
  S.LocalTypeStats = std::move(Stats);
  S.LocalTypesSource = "pdb";
  joinHeaders(S);
  redeclare(S);
  return S.LocalTypes.get();
}

/// The options object, or null with the session's error.
std::optional<llvm::json::Object> options(Session &S, const char *Json) {
  if (!Json || !*Json)
    return llvm::json::Object();
  auto Parsed = llvm::json::parse(Json);
  if (!Parsed) {
    S.setError("malformed options: " + llvm::toString(Parsed.takeError()));
    return std::nullopt;
  }
  if (llvm::json::Object *Object = Parsed->getAsObject())
    return std::move(*Object);
  S.setError("options must be a JSON object");
  return std::nullopt;
}

/// Reads "spelling" into \p Spelling, which stays unset without it; false
/// with \p Error when it names no spelling.
bool readSpelling(const llvm::json::Object &Options,
                  std::optional<types::BuiltinSpelling> &Spelling,
                  std::string &Error) {
  const llvm::json::Value *Value = Options.get("spelling");
  if (!Value)
    return true;
  std::optional<llvm::StringRef> Name = Value->getAsString();
  std::optional<types::BuiltinSpelling> Found =
      Name ? types::spellingNamed(*Name) : std::nullopt;
  if (!Found) {
    Error = "\"spelling\" must be " + types::spellingsText();
    return false;
  }
  Spelling = *Found;
  return true;
}

/// Reads boolean \p Key into \p Out; false with the session's error when it
/// is no boolean.
bool readFlag(const llvm::json::Object &Options, llvm::StringRef Key, bool &Out,
              std::string &Error) {
  const llvm::json::Value *Value = Options.get(Key);
  if (!Value)
    return true;
  if (std::optional<bool> Flag = Value->getAsBoolean()) {
    Out = *Flag;
    return true;
  }
  Error = ("\"" + Key + "\" must be true or false").str();
  return false;
}

/// The language, spelling and flags of a source rendering; false with
/// \p Error when one is malformed.
bool readRender(const llvm::json::Object &Options, types::RenderOptions &Render,
                bool &Dependencies, std::string &Error) {
  if (const llvm::json::Value *Value = Options.get("language")) {
    std::optional<llvm::StringRef> Id = Value->getAsString();
    std::optional<types::Language> Lang =
        Id ? types::languageNamed(*Id) : std::nullopt;
    if (!Lang) {
      Error = "\"language\" must be " + types::languagesText();
      return false;
    }
    Render.Lang = *Lang;
  }
  if (!readSpelling(Options, Render.Spelling, Error))
    return false;
  // Each language names builtins its own ways; none asked takes its default.
  if (Render.Spelling &&
      !types::spellsBuiltins(Render.Lang, *Render.Spelling)) {
    Error = (types::languageId(Render.Lang) + " names builtins as " +
             types::spellingsText(Render.Lang) + ", not " +
             types::spellingId(*Render.Spelling))
                .str();
    return false;
  }
  if (!readFlag(Options, "offsets", Render.Offsets, Error) ||
      !readFlag(Options, "hexOffsets", Render.HexOffsets, Error) ||
      !readFlag(Options, "asserts", Render.Asserts, Error) ||
      !readFlag(Options, "prototypes", Render.Prototypes, Error) ||
      !readFlag(Options, "headerPragmas", Render.HeaderPragmas, Error) ||
      !readFlag(Options, "dependencies", Dependencies, Error))
    return false;
  // The library of a system's headers whose types its headers give, or
  // false for none.
  if (const llvm::json::Value *Value = Options.get("systemLibrary")) {
    const std::optional<llvm::StringRef> Name = Value->getAsString();
    const std::optional<types::HeaderLibrary> Library =
        Name ? types::headerLibraryNamed(*Name) : std::nullopt;
    if (Value->getAsBoolean() == false) {
      Render.SystemLibrary.reset();
    } else if (!Library || !types::headerLibrarySystem(*Library)) {
      Error = "\"systemLibrary\" must be false or name a library of a "
              "system's headers";
      return false;
    } else {
      Render.SystemLibrary = *Library;
    }
  }
  // Prototypes asked of a language that renders none fail, not vanish.
  if (Render.Prototypes && !types::rendersPrototypes(Render.Lang)) {
    Error = ("\"prototypes\" render in c and cpp, not " +
             types::languageId(Render.Lang))
                .str();
    return false;
  }
  return true;
}

/// The types \p Options' "types" names in \p Library, in order; false with
/// \p Error when one is missing.
bool readRoots(const llvm::json::Object &Options,
               const types::TypeLibrary &Library,
               std::vector<const types::LocalType *> &Roots,
               std::string &Error) {
  const llvm::json::Value *Value = Options.get("types");
  if (!Value)
    return true;
  const llvm::json::Array *Names = Value->getAsArray();
  if (!Names) {
    Error = "\"types\" must be an array of names";
    return false;
  }
  for (const llvm::json::Value &Entry : *Names) {
    std::optional<llvm::StringRef> Name = Entry.getAsString();
    const types::LocalType *Type = Name ? Library.find(*Name) : nullptr;
    if (!Type) {
      Error = Name ? "no local type named " + jsonSafeText(*Name)
                   : std::string("\"types\" must be an array of names");
      return false;
    }
    Roots.push_back(Type);
  }
  return true;
}

/// \p Roots, or every type, rendered as \p Render says.
std::string renderSource(const types::TypeLibrary &Library,
                         llvm::ArrayRef<const types::LocalType *> Roots,
                         const types::RenderOptions &Render,
                         bool Dependencies) {
  if (!Roots.empty() && !Dependencies) {
    std::string Text;
    for (const types::LocalType *Type : Roots) {
      if (!Text.empty())
        Text += '\n';
      Text += types::renderType(Library, *Type, Render);
    }
    return Text;
  }
  return types::renderTypes(Library, Roots, Render);
}

} // namespace

bool Session::typesChanged(const types::TypeLibrary &Library,
                           llvm::ArrayRef<std::string> Changed,
                           bool Prototypes) {
  // Prototypes reach the analysis with the types they name: a change to
  // either takes the analysis that read them.
  const llvm::StringSet<> Prototyped = prototypeTypeNames(Library);
  Prototypes |= llvm::any_of(Changed, [&](const std::string &Name) {
    return Prototyped.contains(Name);
  });
  if (Prototypes) {
    clearPipeline();
    return true;
  }
  // The context reads a copy of the library: the next one reads it as it
  // is now, and gives the members it gave anew.  The records whose members
  // the decompiler printed, or a unit held, take only the text that read
  // them.
  dropLocalTypesDebug();
  CallVariant.reset();
  for (auto It = FunctionLlvmModules.begin(); It != FunctionLlvmModules.end();)
    if ((!It->Options.CallTypes.empty() || !It->Options.StackFrame.empty()))
      It = FunctionLlvmModules.erase(It);
    else
      ++It;
  const bool SourceAssertions =
      llvm::any_of(FunctionSources, [](const auto &Source) {
        return !Source.Options.VariableTypes.empty() ||
               !Source.Options.CallTypes.empty() ||
               !Source.Options.StackFrame.empty();
      });
  const bool Shows =
      SourceAssertions || llvm::any_of(Changed, [&](const std::string &Name) {
        return ShownTypes.contains(Name);
      });
  if (Shows) {
    FunctionSources.clear();
    ShownTypes.clear();
  }
  return Shows;
}

std::string Session::platformEntryDeclaration() const {
  if (Img.Format == BinaryFormat::COFF && Img.Entry &&
      (Img.DynInfo.PECharacteristics & llvm::COFF::IMAGE_FILE_DLL) &&
      (Img.DynInfo.PESubsystem == llvm::COFF::IMAGE_SUBSYSTEM_WINDOWS_GUI ||
       Img.DynInfo.PESubsystem == llvm::COFF::IMAGE_SUBSYSTEM_WINDOWS_CUI))
    return "BOOL __stdcall DllEntryPoint(HINSTANCE hinstDLL, DWORD fdwReason, "
           "LPVOID lpReserved)";
  return {};
}

types::TypeLibrary *Session::readLocalTypes() { return localTypes(*this); }

bool Session::checkFunctionDeclaration(const std::string &Text,
                                       std::string &Error) {
  const types::TypeLibrary *Library = localTypes(*this);
  if (!Library) {
    Error = LastError;
    return false;
  }
  const types::FunctionDeclaration Read =
      types::parseFunctionDeclaration(Text, *Library);
  if (!Read.ok()) {
    const types::SourceProblem &Problem = Read.Problems.front();
    Error = "the declaration does not read: " + std::to_string(Problem.Line) +
            ":" + std::to_string(Problem.Column) + ": " + Problem.Message;
    return false;
  }
  if (Read.Function->Variadic) {
    Error = "a variadic declaration is not supported: its calls pass more "
            "than it declares";
    return false;
  }
  // The decompiler names a parameter as declared only while no other shares
  // its name.
  llvm::StringSet<> Names;
  for (const types::Parameter &Param : Read.Function->Params)
    if (!Param.Name.empty() && !Names.insert(Param.Name).second) {
      Error = "two parameters are named " + Param.Name;
      return false;
    }
  // The conventions whose parameter placement NeverD knows for the image:
  // its own, and on x64 the keywords its compilers ignore.
  const types::CallingConvention Convention = Read.Function->Convention;
  const bool X64 = Img.Arch == Arch::X64;
  const bool Windows = Img.abiFormat() == BinaryFormat::COFF;
  bool Placed = Convention == types::CallingConvention::Unspecified ||
                Convention == types::CallingConvention::Cdecl;
  if (Img.Arch == Arch::X86)
    Placed |= Convention == types::CallingConvention::Stdcall;
  if (X64)
    Placed |= Convention == types::CallingConvention::Stdcall ||
              Convention == types::CallingConvention::Fastcall ||
              Convention == types::CallingConvention::Thiscall ||
              (Convention == types::CallingConvention::Win64 && Windows) ||
              (Convention == types::CallingConvention::SysV && !Windows);
  if (!Placed) {
    Error = "NeverD does not place this image's parameters by the "
            "declaration's calling convention";
    return false;
  }
  if (!signatureOf(*Library, *Read.Function, Read.Name)) {
    Error = "the declaration has a type the decompiler cannot describe, such "
            "as a function or an array passed by value";
    return false;
  }
  return true;
}

DebugContext *Session::decompileDebug() {
  if (LocalTypesDbg)
    return LocalTypesDbg.get();
  if (LocalTypesChecked)
    return Dbg.get();
  LocalTypesChecked = true;
  std::map<va_t, std::string> Declarations = functionDeclarations();
  // Reading the types may fail, as a PDB's may; the decompilation goes on
  // with the program's debug information alone, its error untouched, and
  // the user's declarations read against the types every image has.
  const std::string Error = LastError;
  const types::TypeLibrary *Library = localTypes(*this);
  LastError = Error;
  if (!Library && Declarations.empty())
    return Dbg.get();
  // An image without debug information takes the declarations the user
  // gives its functions and the prototypes of the routines its names are
  // those of, as the Windows kernel's headers declare its exports.
  if (!Dbg && Declarations.empty() &&
      (!Library || Library->prototypes().empty()))
    return nullptr;
  std::vector<types::HeaderLibrary> Headers;
  for (const JoinedHeaders &Joined : LocalTypeHeaders)
    if (Joined.Error.empty())
      Headers.push_back(Joined.Library);
  LocalTypesDbg = std::make_unique<LocalTypesDebugContext>(
      Dbg ? *Dbg : static_cast<const DebugContext &>(NoDebugInfo),
      Library ? std::make_shared<types::TypeLibrary>(*Library)
              : std::shared_ptr<types::TypeLibrary>(libraryFor(Img)),
      std::move(Declarations), &Img, std::move(Headers), SigDB.buildNameMap(),
      LocalTypeEntryType, programMain(), LocalTypeEntryDeclaration);
  return LocalTypesDbg.get();
}

extern "C" {

const char *neverd_local_types_json(neverd_session_t Sess,
                                    const char *OptionsJson) {
  auto *S = toSession(Sess);
  if (!S)
    return nullptr;
  S->clearError();
  if (!S->Loaded) {
    S->setError("no binary loaded");
    return nullptr;
  }
  std::optional<llvm::json::Object> Options = options(*S, OptionsJson);
  if (!Options)
    return nullptr;
  std::string Filter;
  if (const llvm::json::Value *Value = Options->get("filter")) {
    std::optional<llvm::StringRef> Text = Value->getAsString();
    if (!Text) {
      S->setError("\"filter\" must be text");
      return nullptr;
    }
    Filter = Text->str();
  }
  std::string Origin;
  if (const llvm::json::Value *Value = Options->get("origin")) {
    std::optional<llvm::StringRef> Text = Value->getAsString();
    constexpr types::TypeOrigin Origins[] = {
        types::TypeOrigin::PDB, types::TypeOrigin::DWARF,
        types::TypeOrigin::Header, types::TypeOrigin::User};
    if (!Text || llvm::none_of(Origins, [&](types::TypeOrigin Each) {
          return *Text == types::originName(Each);
        })) {
      S->setError("\"origin\" must be pdb, dwarf, header or user");
      return nullptr;
    }
    Origin = Text->str();
  }
  std::optional<types::Language> Lang;
  if (const llvm::json::Value *Value = Options->get("language")) {
    std::optional<llvm::StringRef> Id = Value->getAsString();
    Lang = Id ? types::languageNamed(*Id) : std::nullopt;
    if (!Lang) {
      S->setError("\"language\" must be " + types::languagesText());
      return nullptr;
    }
  }
  const types::TypeLibrary *Library = localTypes(*S);
  if (!Library)
    return nullptr;

  llvm::json::Array Types;
  llvm::StringMap<int64_t> Origins;
  for (llvm::json::Value &Entry : types::typeListJSON(*Library)) {
    const llvm::json::Object *Object = Entry.getAsObject();
    if (!Object)
      continue;
    const llvm::StringRef From = Object->getString("origin").value_or("");
    ++Origins[From];
    if (!Origin.empty() && From != Origin)
      continue;
    if (!Filter.empty())
      if (std::optional<llvm::StringRef> Name = Object->getString("name"))
        if (!Name->contains_insensitive(Filter))
          continue;
    Types.push_back(std::move(Entry));
  }
  llvm::json::Object ByOrigin;
  for (const auto &Entry : Origins)
    ByOrigin[Entry.first()] = Entry.second;
  llvm::json::Array Mismatches;
  for (const std::string &Name : S->LocalTypeStats.NestingMismatches)
    Mismatches.push_back(jsonSafeText(Name));
  llvm::json::Array Skipped, Conflicts;
  for (const std::string &Entry : S->LocalTypeSkipped)
    Skipped.push_back(jsonSafeText(Entry));
  for (const std::string &Name : S->LocalTypeConflicts)
    Conflicts.push_back(jsonSafeText(Name));
  llvm::json::Object Result{
      {"source", S->LocalTypesSource},
      {"dataModel", types::dataModelName(Library->dataModel())},
      {"recordABI", types::recordABIName(Library->recordABI())},
      {"stats",
       llvm::json::Object{{"structs", S->LocalTypeStats.Structs},
                          {"unions", S->LocalTypeStats.Unions},
                          {"enums", S->LocalTypeStats.Enums},
                          {"declaredOnly", S->LocalTypeStats.DeclaredOnly},
                          {"unsupportedFunctionTypes",
                           S->LocalTypeStats.UnsupportedFunctionTypes},
                          {"nestingMismatches", std::move(Mismatches)},
                          {"skipped", std::move(Skipped)},
                          {"conflicts", std::move(Conflicts)},
                          {"origins", std::move(ByOrigin)}}},
      {"types", std::move(Types)}};
  if (!S->LocalTypeHeaders.empty()) {
    llvm::json::Array Headers;
    for (const Session::JoinedHeaders &Joined : S->LocalTypeHeaders) {
      llvm::json::Object Entry{
          {"name", types::headerLibraryName(Joined.Library)}};
      if (!Joined.Error.empty()) {
        Entry["error"] = jsonSafeText(Joined.Error);
      } else {
        llvm::json::Array Conflicts;
        for (const std::string &Name : Joined.Merge.Conflicts)
          Conflicts.push_back(jsonSafeText(Name));
        Entry["types"] = static_cast<int64_t>(Joined.Merge.Types);
        Entry["prototypes"] = static_cast<int64_t>(Joined.Merge.Prototypes);
        Entry["conflicts"] = std::move(Conflicts);
      }
      Headers.push_back(std::move(Entry));
    }
    Result["headers"] = std::move(Headers);
  }
  if (!S->DeclarationProblems.empty()) {
    llvm::json::Array Problems;
    for (const auto &[Index, Problem] : S->DeclarationProblems)
      Problems.push_back(
          llvm::json::Object{{"declaration", static_cast<int64_t>(Index)},
                             {"line", Problem.Line},
                             {"column", Problem.Column},
                             {"message", jsonSafeText(Problem.Message)}});
    Result["declarationProblems"] = std::move(Problems);
  }
  if (Lang) {
    llvm::json::Array Renamed;
    for (const auto &[Name, Spelled] : types::renamedNames(*Library, *Lang))
      Renamed.push_back(
          llvm::json::Array{jsonSafeText(Name), jsonSafeText(Spelled)});
    Result["renamed"] = std::move(Renamed);
  }
  return dupStr(jsonToString(llvm::json::Value(std::move(Result))));
}

const char *neverd_local_type_json(neverd_session_t Sess, const char *Name,
                                   const char *OptionsJson) {
  auto *S = toSession(Sess);
  if (!S)
    return nullptr;
  S->clearError();
  if (!S->Loaded) {
    S->setError("no binary loaded");
    return nullptr;
  }
  if (!Name) {
    S->setError("no type named");
    return nullptr;
  }
  std::optional<llvm::json::Object> Options = options(*S, OptionsJson);
  if (!Options)
    return nullptr;
  // A member's type as C spells it.
  std::optional<types::BuiltinSpelling> Spelling;
  std::string Error;
  if (!readSpelling(*Options, Spelling, Error)) {
    S->setError(Error);
    return nullptr;
  }
  const types::TypeLibrary *Library = localTypes(*S);
  if (!Library)
    return nullptr;
  const types::LocalType *Type = Library->find(Name);
  if (!Type) {
    S->setError(std::string("no local type named ") + jsonSafeText(Name));
    return nullptr;
  }
  return dupStr(jsonToString(llvm::json::Value(types::typeJSON(
      *Library, *Type,
      Spelling.value_or(types::defaultSpelling(types::Language::C))))));
}

const char *neverd_local_types_source(neverd_session_t Sess,
                                      const char *OptionsJson) {
  auto *S = toSession(Sess);
  if (!S)
    return nullptr;
  S->clearError();
  if (!S->Loaded) {
    S->setError("no binary loaded");
    return nullptr;
  }
  std::optional<llvm::json::Object> Options = options(*S, OptionsJson);
  if (!Options)
    return nullptr;
  types::RenderOptions Render;
  bool Dependencies = true;
  std::string Error;
  const types::TypeLibrary *Library = localTypes(*S);
  if (!Library)
    return nullptr;
  // The system's types come from its headers, unless asked otherwise: the
  // first library of a system's headers the program joins.
  for (const Session::JoinedHeaders &Joined : S->LocalTypeHeaders)
    if (Joined.Error.empty() && types::headerLibrarySystem(Joined.Library)) {
      Render.SystemLibrary = Joined.Library;
      break;
    }
  if (!readRender(*Options, Render, Dependencies, Error)) {
    S->setError(Error);
    return nullptr;
  }
  std::vector<const types::LocalType *> Roots;
  if (!readRoots(*Options, *Library, Roots, Error)) {
    S->setError(Error);
    return nullptr;
  }
  return dupStr(renderSource(*Library, Roots, Render, Dependencies));
}

const char *neverd_local_types_declare(neverd_session_t Sess,
                                       const char *Source,
                                       const char *OptionsJson) {
  auto *S = toSession(Sess);
  if (!S)
    return nullptr;
  S->clearError();
  if (!S->Loaded) {
    S->setError("no binary loaded");
    return nullptr;
  }
  std::optional<llvm::json::Object> Options = options(*S, OptionsJson);
  if (!Options)
    return nullptr;
  Session::TypeDeclaration Declaration;
  if (const llvm::json::Value *Value = Options->get("language")) {
    std::optional<llvm::StringRef> Id = Value->getAsString();
    std::optional<types::Language> Lang =
        Id ? types::languageNamed(*Id) : std::nullopt;
    if (!Lang) {
      S->setError("\"language\" must be c, cpp, rust or go");
      return nullptr;
    }
    Declaration.Lang = *Lang;
  }
  types::TypeLibrary *Library = localTypes(*S);
  if (!Library)
    return nullptr;
  Declaration.Source = Source ? Source : "";
  const types::ParseResult Result = apply(Declaration, *Library);
  bool Decompilation = false;
  if (Result.ok()) {
    S->TypeDeclarations.push_back(std::move(Declaration));
    Decompilation =
        S->typesChanged(*Library, Result.Types, !Result.Prototypes.empty());
  }
  llvm::json::Array Types, Prototypes, Problems;
  for (const std::string &Name : Result.Types)
    Types.push_back(jsonSafeText(Name));
  for (const std::string &Name : Result.Prototypes)
    Prototypes.push_back(jsonSafeText(Name));
  for (const types::SourceProblem &Problem : Result.Problems)
    Problems.push_back(
        llvm::json::Object{{"line", Problem.Line},
                           {"column", Problem.Column},
                           {"message", jsonSafeText(Problem.Message)}});
  return dupStr(jsonToString(llvm::json::Value(
      llvm::json::Object{{"types", std::move(Types)},
                         {"prototypes", std::move(Prototypes)},
                         {"problems", std::move(Problems)},
                         {"decompilation", Decompilation}})));
}

const char *neverd_local_types_forget(neverd_session_t Sess,
                                      const char *NamesJson) {
  auto *S = toSession(Sess);
  if (!S)
    return nullptr;
  S->clearError();
  if (!S->Loaded) {
    S->setError("no binary loaded");
    return nullptr;
  }
  llvm::Expected<llvm::json::Value> Parsed =
      llvm::json::parse(NamesJson ? NamesJson : "");
  if (!Parsed) {
    S->setError("the names are no JSON: " + llvm::toString(Parsed.takeError()));
    return nullptr;
  }
  std::string Error;
  std::optional<std::vector<Session::TypeDeclaration>> Removal =
      parseDeclarations(
          llvm::json::Array{llvm::json::Object{{"forget", std::move(*Parsed)}}},
          Error);
  if (!Removal) {
    S->setError(Error == "a removal names no types"
                    ? "name the types to remove in a list"
                    : Error);
    return nullptr;
  }
  types::TypeLibrary *Library = localTypes(*S);
  if (!Library)
    return nullptr;
  Session::TypeDeclaration &Declaration = Removal->front();
  for (const std::string &Name : Declaration.Forget)
    if (!Library->find(Name)) {
      S->setError("no type is named " + Name);
      return nullptr;
    }
  llvm::StringSet<> Removed;
  for (const std::string &Name : Declaration.Forget)
    Removed.insert(Name);
  apply(Declaration, *Library);
  // The types that still name one removed.
  llvm::json::Array UsedBy;
  for (const types::LocalType *Type : Library->types())
    for (const std::string &Named : types::mentionedTypeNames(*Type))
      if (Removed.contains(Named)) {
        UsedBy.push_back(jsonSafeText(Type->Name));
        break;
      }
  llvm::json::Array Names;
  for (const std::string &Name : Declaration.Forget)
    Names.push_back(jsonSafeText(Name));
  // A type the prototypes name, which they still do, read as the decompiler
  // read it.
  const bool Decompilation = S->typesChanged(*Library, Declaration.Forget,
                                             /*Prototypes=*/false);
  S->TypeDeclarations.push_back(std::move(Declaration));
  return dupStr(jsonToString(
      llvm::json::Value(llvm::json::Object{{"removed", std::move(Names)},
                                           {"usedBy", std::move(UsedBy)},
                                           {"decompilation", Decompilation}})));
}

const char *neverd_types_languages_json(void) {
  llvm::json::Array Languages;
  const auto add = [&](types::Language Lang, llvm::StringRef Display) {
    llvm::json::Array Spellings;
    for (types::BuiltinSpelling Spelling : types::spellingsOf(Lang))
      Spellings.push_back(types::spellingId(Spelling));
    Languages.push_back(llvm::json::Object{
        {"id", types::languageId(Lang)},
        {"display", Display},
        {"spelling", types::spellingId(types::defaultSpelling(Lang))},
        {"spellings", std::move(Spellings)},
        {"reads", types::readsDeclarations(Lang)},
        {"prototypes", types::rendersPrototypes(Lang)}});
  };
#define NEVERD_TYPE_LANGUAGE(Name, Id, Display, Spelling)                      \
  add(types::Language::Name, Display);
#include "neverd/types/TypeLanguages.def"
  return dupStr(jsonToString(llvm::json::Value(
      llvm::json::Object{{"languages", std::move(Languages)}})));
}

const char *neverd_types_convert(const char *RequestJson) {
  const auto answer = [](llvm::json::Object Reply) {
    return dupStr(jsonToString(llvm::json::Value(std::move(Reply))));
  };
  const auto refuse = [&](const std::string &Error) {
    return answer(llvm::json::Object{{"error", jsonSafeText(Error)}});
  };
  llvm::Expected<llvm::json::Value> Parsed =
      llvm::json::parse(RequestJson ? RequestJson : "");
  if (!Parsed)
    return refuse("malformed request: " + llvm::toString(Parsed.takeError()));
  const llvm::json::Object *Request = Parsed->getAsObject();
  if (!Request)
    return refuse("the request must be a JSON object");
  std::optional<llvm::StringRef> ModelName = Request->getString("dataModel");
  std::optional<types::DataModel> Model =
      ModelName ? types::dataModelNamed(*ModelName) : std::nullopt;
  if (!Model)
    return refuse("\"dataModel\" must be LLP64, LP64 or ILP32");
  std::optional<types::RecordABI> ABI;
  if (const llvm::json::Value *Named = Request->get("recordABI")) {
    std::optional<llvm::StringRef> Name = Named->getAsString();
    ABI = Name ? types::recordABINamed(*Name) : std::nullopt;
    if (!ABI)
      return refuse("\"recordABI\" must be microsoft, mingw-x64, mingw-x86, "
                    "itanium, apple-arm64, i386-sysv, android-i386 or "
                    "aapcs32");
  }
  bool KeepGoing = false;
  std::string Error;
  if (!readFlag(*Request, "keepGoing", KeepGoing, Error))
    return refuse(Error);

  types::TypeLibrary Library(*Model);
  if (ABI)
    Library.setRecordABI(*ABI);
  llvm::json::Array Types, Prototypes, Problems, Imports;
  const llvm::json::Array *Declarations = Request->getArray("declarations");
  if (!Declarations)
    return refuse("\"declarations\" must be an array");
  for (size_t I = 0; I < Declarations->size(); ++I) {
    const llvm::json::Object *Declaration = (*Declarations)[I].getAsObject();
    std::optional<llvm::StringRef> Language =
        Declaration ? Declaration->getString("language") : std::nullopt;
    std::optional<types::Language> Lang =
        Language ? types::languageNamed(*Language) : std::nullopt;
    std::optional<llvm::StringRef> Source =
        Declaration ? Declaration->getString("source") : std::nullopt;
    if (!Lang || !Source)
      return refuse("a declaration needs a language (c, cpp, rust or go) and "
                    "its source");
    std::optional<llvm::StringRef> Prefix;
    if (const auto *Value = Declaration->get("importPrefix")) {
      Prefix = Value->getAsString();
      if (!Prefix || Prefix->empty())
        return refuse("\"importPrefix\" must be a nonempty C identifier");
    }
    std::vector<llvm::StringRef> Selected;
    std::optional<llvm::ArrayRef<llvm::StringRef>> Selection;
    if (const auto *Value = Declaration->get("importPrototypes")) {
      const auto *Names = Value->getAsArray();
      if (!Prefix || !Names)
        return refuse(
            "\"importPrototypes\" requires importPrefix and an array of names");
      for (const auto &Name : *Names) {
        const auto Text = Name.getAsString();
        if (!Text || Text->empty())
          return refuse("\"importPrototypes\" names must be nonempty strings");
        Selected.push_back(*Text);
      }
      Selection = Selected;
    }
    types::ParseOptions Parse;
    Parse.Lang = *Lang;
    Parse.KeepGoing = KeepGoing;
    types::TypeLibrary Provider(*Model);
    Provider.setRecordABI(Library.recordABI());
    types::ParseResult Result =
        types::parseTypes(*Source, Prefix ? Provider : Library, Parse);
    if (Prefix) {
      // Partial provider declarations cannot establish a complete dependency
      // closure. Even keepGoing must not import a partly parsed provider.
      Result.Types.clear();
      Result.Prototypes.clear();
      if (Result.Problems.empty()) {
        auto Imported =
            types::importPrototypes(Library, Provider, *Prefix, Selection);
        if (!Imported)
          return refuse("declaration " + std::to_string(I) + ": " +
                        llvm::toString(Imported.takeError()));
        llvm::json::Array Renamed, Dependencies, Added;
        for (const auto &Name : Imported->Dependencies)
          Dependencies.push_back(jsonSafeText(Name));
        for (const auto &Name : Imported->Prototypes)
          Added.push_back(jsonSafeText(Name));
        for (const auto &[Original, Name] : Imported->Renamed)
          Renamed.push_back(
              llvm::json::Object{{"original", jsonSafeText(Original)},
                                 {"name", jsonSafeText(Name)}});
        Imports.push_back(
            llvm::json::Object{{"declaration", static_cast<int64_t>(I)},
                               {"prefix", jsonSafeText(*Prefix)},
                               {"renamed", std::move(Renamed)},
                               {"dependencies", std::move(Dependencies)},
                               {"prototypes", std::move(Added)}});
        Result.Types = std::move(Imported->Types);
        Result.Prototypes = std::move(Imported->Prototypes);
      }
    }
    for (const std::string &Name : Result.Types)
      Types.push_back(jsonSafeText(Name));
    for (const std::string &Name : Result.Prototypes)
      Prototypes.push_back(jsonSafeText(Name));
    for (const types::SourceProblem &Problem : Result.Problems)
      Problems.push_back(
          llvm::json::Object{{"declaration", static_cast<int64_t>(I)},
                             {"line", Problem.Line},
                             {"column", Problem.Column},
                             {"message", jsonSafeText(Problem.Message)}});
  }
  llvm::json::Object Reply{{"types", std::move(Types)},
                           {"prototypes", std::move(Prototypes)}};
  if (!Imports.empty())
    Reply["imports"] = std::move(Imports);
  // Nothing renders from declarations that do not read, unless asked to
  // keep going past them.
  if (const llvm::json::Object *Render = Request->getObject("render");
      Render && (Problems.empty() || KeepGoing)) {
    types::RenderOptions Options;
    bool Dependencies = true;
    std::vector<const types::LocalType *> Roots;
    if (!readRender(*Render, Options, Dependencies, Error) ||
        !readRoots(*Render, Library, Roots, Error))
      return refuse(Error);
    Reply["source"] = renderSource(Library, Roots, Options, Dependencies);
  }
  Reply["problems"] = std::move(Problems);
  return answer(std::move(Reply));
}

const char *neverd_type_declarations_json(neverd_session_t Sess) {
  auto *S = toSession(Sess);
  if (!S)
    return nullptr;
  S->clearError();
  return dupStr(jsonToString(llvm::json::Value(declarationRows(*S))));
}

const char *neverd_type_libraries_json(neverd_session_t Sess) {
  auto *S = toSession(Sess);
  if (!S)
    return nullptr;
  S->clearError();
  // Without a program, each library alone; with one, what joins it.
  const types::TypeLibrary *Program = S->Loaded ? localTypes(*S) : nullptr;
  if (S->Loaded && !Program)
    return nullptr;
  const std::vector<types::HeaderLibrary> Image =
      Program ? imageLibraries(*S) : std::vector<types::HeaderLibrary>();
  llvm::json::Array Libraries;
  std::vector<types::HeaderLibrary> Listed;
  const auto row = [&](types::HeaderLibrary Library,
                       const Session::JoinedHeaders *Joined) {
    llvm::json::Object Row{
        {"name", types::headerLibraryName(Library)},
        {"dataModel", types::dataModelName(types::headerLibraryModel(Library))},
        {"recordABI",
         types::recordABIName(types::headerLibraryRecordABI(Library))},
        {"image", llvm::is_contained(Image, Library)},
        {"joined", Joined != nullptr}};
    if (Program)
      Row["compatible"] = compatibleLibrary(*S, Library);
    if (Joined && !Joined->Error.empty()) {
      Row["error"] = jsonSafeText(Joined->Error);
    } else if (Joined) {
      llvm::json::Array Conflicts;
      for (const std::string &Name : Joined->Merge.Conflicts)
        Conflicts.push_back(jsonSafeText(Name));
      Row["types"] = static_cast<int64_t>(Joined->Merge.Types);
      Row["prototypes"] = static_cast<int64_t>(Joined->Merge.Prototypes);
      Row["conflicts"] = std::move(Conflicts);
    }
    Libraries.push_back(std::move(Row));
    Listed.push_back(Library);
  };
  // The libraries joined first, in the order they join, then the others in
  // the table's.
  if (Program)
    for (const Session::JoinedHeaders &Joined : S->LocalTypeHeaders)
      row(Joined.Library, &Joined);
#define NEVERD_HEADER_LIBRARY(Id, Name, File, Model, ABI, Spelling)            \
  if (!llvm::is_contained(Listed, types::HeaderLibrary::Id))                   \
    row(types::HeaderLibrary::Id, nullptr);
#include "neverd/types/HeaderLibraries.def"
  llvm::json::Object Result = libraryChoiceJson(S->TypeLibraryChoice);
  Result["libraries"] = std::move(Libraries);
  return dupStr(jsonToString(llvm::json::Value(std::move(Result))));
}

const char *neverd_type_libraries_set(neverd_session_t Sess, const char *Json) {
  auto *S = toSession(Sess);
  if (!S)
    return nullptr;
  S->clearError();
  if (!S->Loaded) {
    S->setError("no binary loaded");
    return nullptr;
  }
  auto Parsed = llvm::json::parse(Json ? Json : "");
  if (!Parsed) {
    llvm::consumeError(Parsed.takeError());
    S->setError("the type libraries are not valid JSON");
    return nullptr;
  }
  std::string Error;
  std::optional<Session::LibraryChoice> Choice =
      readLibraryChoice(*Parsed, Error);
  if (!Choice) {
    S->setError(Error);
    return nullptr;
  }
  if (!localTypes(*S))
    return nullptr;
  // An added library is none of the image's and lays out as the program's;
  // a removed one is the image's.
  const std::vector<types::HeaderLibrary> Image = imageLibraries(*S);
  for (types::HeaderLibrary Library : Choice->Added) {
    const std::string Name = types::headerLibraryName(Library).str();
    if (llvm::is_contained(Image, Library)) {
      S->setError(Name + " is one the program joins already");
      return nullptr;
    }
    if (!compatibleLibrary(*S, Library)) {
      S->setError(Name + " lays its records out otherwise than the program");
      return nullptr;
    }
  }
  for (types::HeaderLibrary Library : Choice->Removed)
    if (!llvm::is_contained(Image, Library)) {
      S->setError(types::headerLibraryName(Library).str() +
                  " is none the program joins of its own");
      return nullptr;
    }
  S->TypeLibraryChoice = std::move(*Choice);
  // A decompilation that read the prototypes read the libraries these
  // replace.
  const bool ReadPrototypes = S->LocalTypesChecked;
  S->dropLocalTypes();
  if (ReadPrototypes)
    S->clearPipeline();
  return neverd_type_libraries_json(Sess);
}

int neverd_type_libraries_save(neverd_session_t Sess) {
  auto *S = toSession(Sess);
  if (!S)
    return -1;
  S->clearError();
  if (!S->requireFileBacked() || !S->Loaded)
    return -1;
  ProjectWriteLock Lock(S->FilePath, S->projectNamespaceSuffix());
  if (!Lock) {
    S->setError("project writer unavailable: " + Lock.error());
    return -1;
  }
  return saveSidecar(S, librariesPath(*S),
                     llvm::json::Value(libraryChoiceJson(S->TypeLibraryChoice)),
                     "type libraries")
             ? 0
             : -1;
}

int neverd_type_libraries_load(neverd_session_t Sess) {
  auto *S = toSession(Sess);
  if (!S)
    return -1;
  S->clearError();
  if (!S->requireFileBacked() || !S->Loaded)
    return -1;
  const std::filesystem::path Path = librariesPath(*S);
  std::error_code EC;
  const bool Exists = std::filesystem::exists(Path, EC);
  if (EC) {
    S->setError("cannot inspect the type libraries: " + EC.message());
    return -1;
  }
  Session::LibraryChoice Choice;
  if (Exists) {
    auto Buffer = llvm::MemoryBuffer::getFile(pathToUTF8(Path));
    if (!Buffer) {
      S->setError("cannot read the type libraries: " +
                  Buffer.getError().message());
      return -1;
    }
    llvm::Expected<llvm::json::Value> Parsed =
        llvm::json::parse((*Buffer)->getBuffer());
    if (!Parsed) {
      llvm::consumeError(Parsed.takeError());
      S->setError("the type libraries file is not valid JSON");
      return -1;
    }
    std::string Error;
    std::optional<Session::LibraryChoice> Read =
        readLibraryChoice(*Parsed, Error);
    if (!Read) {
      S->setError("the type libraries file: " + Error);
      return -1;
    }
    Choice = std::move(*Read);
  }
  S->TypeLibraryChoice = std::move(Choice);
  const bool ReadPrototypes = S->LocalTypesChecked;
  S->dropLocalTypes();
  if (ReadPrototypes)
    S->clearPipeline();
  return 0;
}

int neverd_type_declarations_save(neverd_session_t Sess) {
  auto *S = toSession(Sess);
  if (!S)
    return -1;
  S->clearError();
  if (!S->requireFileBacked() || !S->Loaded)
    return -1;
  ProjectWriteLock Lock(S->FilePath, S->projectNamespaceSuffix());
  if (!Lock) {
    S->setError("project writer unavailable: " + Lock.error());
    return -1;
  }
  return saveSidecar(S, declarationsPath(*S), declarationRows(*S),
                     "type declarations")
             ? 0
             : -1;
}

int neverd_type_declarations_load(neverd_session_t Sess) {
  auto *S = toSession(Sess);
  if (!S)
    return -1;
  S->clearError();
  if (!S->requireFileBacked() || !S->Loaded)
    return -1;
  const std::filesystem::path Path = declarationsPath(*S);
  std::error_code EC;
  const bool Exists = std::filesystem::exists(Path, EC);
  if (EC) {
    S->setError("cannot inspect type declarations: " + EC.message());
    return -1;
  }
  std::vector<Session::TypeDeclaration> Declarations;
  if (Exists) {
    auto Buffer = llvm::MemoryBuffer::getFile(pathToUTF8(Path));
    if (!Buffer) {
      S->setError("cannot read type declarations: " +
                  Buffer.getError().message());
      return -1;
    }
    llvm::Expected<llvm::json::Value> Parsed =
        llvm::json::parse((*Buffer)->getBuffer());
    if (!Parsed) {
      llvm::consumeError(Parsed.takeError());
      S->setError("type declarations sidecar is not valid JSON");
      return -1;
    }
    std::string Error;
    std::optional<std::vector<Session::TypeDeclaration>> Read =
        parseDeclarations(*Parsed, Error);
    if (!Read) {
      S->setError("type declarations sidecar: " + Error);
      return -1;
    }
    Declarations = std::move(*Read);
  }
  S->TypeDeclarations = std::move(Declarations);
  // A decompilation that read the prototypes read the declarations these
  // replace.
  const bool ReadPrototypes = S->LocalTypesChecked;
  S->dropLocalTypes();
  if (ReadPrototypes)
    S->clearPipeline();
  return 0;
}

} // extern "C"
