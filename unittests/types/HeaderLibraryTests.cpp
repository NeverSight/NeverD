//===- HeaderLibraryTests.cpp - Type libraries of system headers ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/libc/LibCNames.h"
#include "neverd/types/HeaderLibrary.h"
#include "neverd/types/TypeParse.h"
#include "neverd/types/TypeRender.h"

using namespace neverd::types;

namespace {

std::optional<uint64_t> sizeOf(const TypeLibrary &Library,
                               llvm::StringRef Name) {
  const LocalType *Type = Library.find(Name);
  return Type ? Library.sizeOf(*makeNamed(Name.str(), Type->Kind))
              : std::nullopt;
}

/// The bytes the Win64 convention passes a parameter of \p Type in: a
/// scalar's own, a record's of 1, 2, 4 or 8 bytes, and a pointer's for a
/// larger record, which goes by its address, or an array, which decays.
std::optional<uint64_t> win64Bytes(const TypeLibrary &Library,
                                   const TypeExpr &Type) {
  const TypeExpr *Named = &Type;
  for (unsigned Depth = 0; Depth < 32 && Named->Kind == TypeExprKind::Named;
       ++Depth) {
    const LocalType *Typedef = Library.find(Named->Name);
    if (!Typedef || Typedef->Kind != TypeKind::Typedef || !Typedef->Target)
      break;
    Named = Typedef->Target.get();
  }
  if (Named->Kind == TypeExprKind::Array)
    return 8;
  const std::optional<uint64_t> Size = Library.sizeOf(*Named);
  const bool Record = Named->Kind == TypeExprKind::Record ||
                      (Named->Kind == TypeExprKind::Named &&
                       (Named->NamedKind == TypeKind::Struct ||
                        Named->NamedKind == TypeKind::Union));
  if (Record && Size && *Size != 1 && *Size != 2 && *Size != 4 && *Size != 8)
    return 8;
  return Size;
}

} // namespace

TEST(HeaderLibrary, TheKernelsReadsAsADriverDeclaresIt) {
  std::string Error;
  const TypeLibrary *Kernel =
      headerLibrary(HeaderLibrary::WindowsKernelX64, &Error);
  ASSERT_NE(Kernel, nullptr) << Error;
  EXPECT_EQ(Kernel->dataModel(), DataModel::LLP64);
  // Read once: the same library every time.
  EXPECT_EQ(headerLibrary(HeaderLibrary::WindowsKernelX64), Kernel);
  EXPECT_GT(Kernel->size(), 3000u);
  EXPECT_GT(Kernel->prototypes().size(), 1500u);
  for (const LocalType *Type : Kernel->types())
    EXPECT_EQ(Type->Origin, TypeOrigin::Header) << Type->Name;

  // The sizes x64 Windows gives them.
  EXPECT_EQ(sizeOf(*Kernel, "UNICODE_STRING"), 16u);
  EXPECT_EQ(sizeOf(*Kernel, "OBJECT_ATTRIBUTES"), 48u);
  EXPECT_EQ(sizeOf(*Kernel, "IO_STATUS_BLOCK"), 16u);
  EXPECT_EQ(sizeOf(*Kernel, "LIST_ENTRY"), 16u);
  EXPECT_EQ(sizeOf(*Kernel, "NTSTATUS"), 4u);
  EXPECT_EQ(sizeOf(*Kernel, "HANDLE"), 8u);
  EXPECT_EQ(Kernel->alignOf(*makeNamed("SLIST_ENTRY", TypeKind::Typedef)), 16u);

  // Prototypes as the WDK declares them, parameters named.
  const FunctionDef *Create = Kernel->findPrototype("NtCreateFile");
  ASSERT_NE(Create, nullptr);
  ASSERT_EQ(Create->Params.size(), 11u);
  EXPECT_EQ(Create->Params[0].Name, "FileHandle");
  EXPECT_EQ(Create->Params[10].Name, "EaLength");
  EXPECT_EQ(Create->Return->Name, "NTSTATUS");
  ASSERT_NE(Kernel->findPrototype("KeBugCheckEx"), nullptr);
  EXPECT_TRUE(Kernel->findPrototype("KeBugCheckEx")->NoReturn);
  EXPECT_FALSE(Kernel->findPrototype("IoCreateDevice")->NoReturn);
}

TEST(HeaderLibrary, EveryLibraryReadsLaidOutForItsDataModel) {
  // A pointer, and a SIZE_T, take what each data model gives them: a 32-bit
  // library's handles and records are a 32-bit program's.
  const HeaderLibrary Libraries[] = {
#define NEVERD_HEADER_LIBRARY(Id, Name, File, Model, ABI, Spelling)            \
  HeaderLibrary::Id,
#include "neverd/types/HeaderLibraries.def"
  };
  for (const HeaderLibrary Library : Libraries) {
    SCOPED_TRACE(headerLibraryName(Library).str());
    std::string Error;
    const TypeLibrary *Types = headerLibrary(Library, &Error);
    ASSERT_NE(Types, nullptr) << Error;
    EXPECT_EQ(Types->dataModel(), headerLibraryModel(Library));
    EXPECT_EQ(Types->recordABI(), headerLibraryRecordABI(Library));
    EXPECT_GT(Types->size(), 500u);
    EXPECT_GT(Types->prototypes().size(), 1500u);
    const bool Wide = headerLibraryModel(Library) != DataModel::ILP32;
    if (headerLibrarySpelling(Library) == BuiltinSpelling::Windows) {
      EXPECT_EQ(sizeOf(*Types, "HANDLE"), Wide ? 8u : 4u);
      EXPECT_EQ(sizeOf(*Types, "SIZE_T"), Wide ? 8u : 4u);
      EXPECT_EQ(sizeOf(*Types, "_UNICODE_STRING"), Wide ? 16u : 8u);
    } else {
      EXPECT_EQ(sizeOf(*Types, "size_t"), Wide ? 8u : 4u);
      EXPECT_EQ(sizeOf(*Types, "sockaddr_in"), 16u);
    }
  }
}

TEST(HeaderLibrary, TheNoReturnTablesHoldEveryNoReturnRoutine) {
  // Function discovery and the call analysis read the tables, before any
  // header: a routine the headers declare noreturn ends the code that calls
  // it with or without them.
  const HeaderLibrary Libraries[] = {
#define NEVERD_HEADER_LIBRARY(Id, Name, File, Model, ABI, Spelling)            \
  HeaderLibrary::Id,
#include "neverd/types/HeaderLibraries.def"
  };
  for (const HeaderLibrary Library : Libraries) {
    const TypeLibrary *Types = headerLibrary(Library);
    ASSERT_NE(Types, nullptr);
    for (const auto &Prototype : Types->prototypes())
      if (Prototype.getValue()->NoReturn)
        EXPECT_TRUE(neverd::libc::isNoReturnFunction(Prototype.getKey()))
            << headerLibraryName(Library).str() << " "
            << Prototype.getKey().str();
  }
}

TEST(HeaderLibrary, TheUserLibrariesDeclareNtdllsNativeAPI) {
  // phnt's native API: the routines ntdll exports, and the records a
  // program reaches from its TEB, laid out as each architecture's ntdll
  // reads them (ARM64's as x64's).
  struct Offset {
    const char *Record;
    const char *Member;
    uint64_t Wide, Narrow;
  };
  const Offset Offsets[] = {
      {"TEB", "ProcessEnvironmentBlock", 0x60, 0x30},
      {"TEB", "LastErrorValue", 0x68, 0x34},
      {"PEB", "Ldr", 0x18, 0x0c},
      {"PEB", "ProcessParameters", 0x20, 0x10},
      {"PEB_LDR_DATA", "InMemoryOrderModuleList", 0x20, 0x14},
      {"LDR_DATA_TABLE_ENTRY", "DllBase", 0x30, 0x18},
      {"RTL_USER_PROCESS_PARAMETERS", "ImagePathName", 0x60, 0x38},
  };
  for (const HeaderLibrary Library :
       {HeaderLibrary::WindowsUserX64, HeaderLibrary::WindowsUserX86,
        HeaderLibrary::WindowsUserARM64}) {
    SCOPED_TRACE(headerLibraryName(Library).str());
    std::string Error;
    const TypeLibrary *Types = headerLibrary(Library, &Error);
    ASSERT_NE(Types, nullptr) << Error;
    const FunctionDef *Load = Types->findPrototype("LdrLoadDll");
    ASSERT_NE(Load, nullptr);
    ASSERT_EQ(Load->Params.size(), 4u);
    EXPECT_EQ(Load->Params[3].Name, "DllHandle");
    const FunctionDef *Query = Types->findPrototype("NtQuerySystemInformation");
    ASSERT_NE(Query, nullptr);
    EXPECT_EQ(Query->Params.size(), 4u);
    EXPECT_EQ(Query->Return->Name, "NTSTATUS");
    // phnt's <ntzwapi.h> declares the Zw routines <winternl.h> does not.
    const FunctionDef *Close = Types->findPrototype("ZwClose");
    ASSERT_NE(Close, nullptr);
    EXPECT_EQ(Close->Params.size(), 1u);

    const bool Wide = headerLibraryModel(Library) != DataModel::ILP32;
    std::string Assertions;
    for (const Offset &O : Offsets)
      Assertions += std::string("_Static_assert(__builtin_offsetof(") +
                    O.Record + ", " + O.Member +
                    ") == " + std::to_string(Wide ? O.Wide : O.Narrow) +
                    ", \"" + O.Record + "." + O.Member + "\");\n";
    // A copy, which the assertions read without changing the library.
    TypeLibrary Layout = *Types;
    const ParseResult Laid = parseCTypes(Assertions, Layout);
    EXPECT_TRUE(Laid.Problems.empty()) << Laid.Problems.front().Message;
  }
}

TEST(HeaderLibrary, TheLinuxLibrariesLayRecordsOutAsGlibcDoes) {
  // glibc's records as each architecture's compilers lay them out, as clang
  // measures them against the same headers: i386's System V ABI aligns
  // FILE's 64-bit _offset to four and AAPCS32 to eight, and a 32-bit
  // program's time_t is 32 bits.
  struct Expected {
    HeaderLibrary Library;
    uint64_t Stat, StatSize, Timespec, Mutex, File, FileOffset, Sigaction,
        Dirent;
  };
  const Expected Libraries[] = {
      {HeaderLibrary::LinuxX64, 144, 48, 16, 40, 216, 144, 152, 280},
      {HeaderLibrary::LinuxX86, 88, 44, 8, 24, 148, 76, 140, 268},
      {HeaderLibrary::LinuxARM64, 128, 48, 16, 48, 216, 144, 152, 280},
      {HeaderLibrary::LinuxARM, 88, 44, 8, 24, 152, 80, 140, 268},
  };
  for (const Expected &E : Libraries) {
    SCOPED_TRACE(headerLibraryName(E.Library).str());
    std::string Error;
    const TypeLibrary *Types = headerLibrary(E.Library, &Error);
    ASSERT_NE(Types, nullptr) << Error;
    const auto Check = [](const char *What, uint64_t Value) {
      return "_Static_assert(" + std::string(What) +
             " == " + std::to_string(Value) + ", \"" + What + "\");\n";
    };
    const std::string Assertions =
        Check("sizeof(struct stat)", E.Stat) +
        Check("__builtin_offsetof(struct stat, st_size)", E.StatSize) +
        Check("sizeof(struct timespec)", E.Timespec) +
        Check("sizeof(pthread_mutex_t)", E.Mutex) +
        Check("sizeof(FILE)", E.File) +
        Check("__builtin_offsetof(FILE, _offset)", E.FileOffset) +
        Check("sizeof(struct sigaction)", E.Sigaction) +
        Check("sizeof(struct dirent)", E.Dirent);
    // A copy, which the assertions read without changing the library.
    TypeLibrary Layout = *Types;
    const ParseResult Laid = parseCTypes(Assertions, Layout);
    EXPECT_TRUE(Laid.Problems.empty()) << Laid.Problems.front().Message;

    const FunctionDef *Read = Types->findPrototype("read");
    ASSERT_NE(Read, nullptr);
    ASSERT_EQ(Read->Params.size(), 3u);
    EXPECT_EQ(Read->Params[2].Name, "__nbytes");
    // glibc 2.38 names strtol __isoc23_strtol for _GNU_SOURCE: a program
    // imports that name.
    for (const char *Name : {"strtol", "__isoc23_strtol"}) {
      const FunctionDef *Number = Types->findPrototype(Name);
      ASSERT_NE(Number, nullptr) << Name;
      EXPECT_EQ(Number->Params.size(), 3u) << Name;
    }
    ASSERT_NE(Types->findPrototype("exit"), nullptr);
    EXPECT_TRUE(Types->findPrototype("exit")->NoReturn);
  }
}

TEST(HeaderLibrary, JoinsWhereTheProgramHasNoTypeOfTheName) {
  TypeLibrary Headers;
  ParseOptions Options;
  Options.Origin = TypeOrigin::Header;
  ASSERT_TRUE(parseCTypes(R"(
    typedef struct _OBJ { ULONG A; } OBJ, *POBJ;
    typedef struct _HOLDER { struct _OBJ Obj; ULONG Tail; } HOLDER;
    typedef struct _USER { struct _HOLDER Holder; } USER;
    typedef struct _POINTS { POBJ First; } POINTS;
    VOID Use(POBJ Object);
  )",
                          Headers, Options)
                  .ok());
  // The program's _OBJ, of another size, and a prototype of its own.
  TypeLibrary Program;
  ASSERT_TRUE(parseCTypes("struct _OBJ { ULONGLONG A, B; };\n"
                          "ULONG Use(void *Object);",
                          Program)
                  .ok());
  const HeaderMerge Merge = mergeHeaderTypes(Program, Headers);
  // _HOLDER was laid out with the headers' _OBJ, and _USER holds it: both
  // go, with the typedefs that name them, though a record stays declared
  // for what points to it.  OBJ names the program's _OBJ, and _POINTS only
  // points to it.
  EXPECT_EQ(Merge.Conflicts,
            (std::vector<std::string>{"_HOLDER", "HOLDER", "_USER", "USER"}));
  const LocalType *Holder = Program.find("_HOLDER");
  ASSERT_NE(Holder, nullptr);
  ASSERT_TRUE(Holder->Record);
  EXPECT_FALSE(Holder->Record->Defined);
  EXPECT_EQ(Program.find("HOLDER"), nullptr);
  EXPECT_EQ(sizeOf(Program, "OBJ"), 16u);
  EXPECT_NE(Program.find("_POINTS"), nullptr);
  EXPECT_EQ(Program.find("_POINTS")->Origin, TypeOrigin::Header);
  EXPECT_EQ(Program.find("_OBJ")->Origin, TypeOrigin::User);
  // The program's own prototype stays.
  EXPECT_EQ(Program.findPrototype("Use")->Return->TheBuiltin, Builtin::ULong);
  EXPECT_EQ(Merge.Prototypes, 0u);
}

TEST(HeaderLibrary, DeclaresTheKernelRoutinesTheCallAnalysisFixes) {
  // WindowsKernelRoutines.inc fixes the bytes each parameter of a routine
  // takes, which the call analysis reads before any prototype; the kernel's
  // headers declare the same routines and must agree.
  const TypeLibrary *Kernel = headerLibrary(HeaderLibrary::WindowsKernelX64);
  ASSERT_NE(Kernel, nullptr);
  struct Routine {
    llvm::StringRef Name;
    std::vector<uint64_t> Bytes;
  };
  const Routine Routines[] = {
#define ND_WINDOWS_DISPATCH_THUNK(Name)
#define ND_WINDOWS_PROTO(Name, ...) {Name, {__VA_ARGS__}},
#include "neverd/libc/WindowsKernelRoutines.inc"
#undef ND_WINDOWS_PROTO
#undef ND_WINDOWS_DISPATCH_THUNK
  };
  size_t Declared = 0;
  for (const Routine &R : Routines) {
    const FunctionDef *Function = Kernel->findPrototype(R.Name);
    if (!Function)
      continue;
    ++Declared;
    ASSERT_EQ(Function->Params.size(), R.Bytes.size()) << R.Name.str();
    for (size_t I = 0; I < R.Bytes.size(); ++I)
      EXPECT_EQ(win64Bytes(*Kernel, *Function->Params[I].Type), R.Bytes[I])
          << R.Name.str() << " parameter " << I;
  }
  EXPECT_GT(Declared, 0u);
}
