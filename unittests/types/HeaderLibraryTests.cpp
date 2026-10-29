//===- HeaderLibraryTests.cpp - Type libraries of system headers ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/libc/LibCNames.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/types/HeaderLibrary.h"
#include "neverd/types/TypeParse.h"
#include "neverd/types/TypeRender.h"

using namespace neverd::types;

namespace {

bool microsoftSource(HeaderLibrary Library) {
  return headerLibrarySource(Library).starts_with(
      "// Local declarations generated from Microsoft SDK/WDK.");
}

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

TEST(HeaderLibrary, DriverProvidersHaveExactConfigurationCallContracts) {
  for (auto Id :
       {HeaderLibrary::WindowsKernelX64, HeaderLibrary::WindowsKernelX86,
        HeaderLibrary::WindowsKernelARM64}) {
    const TypeLibrary *Kernel = headerLibrary(Id);
    ASSERT_NE(Kernel, nullptr);
    const bool X86 = Id == HeaderLibrary::WindowsKernelX86;
    SCOPED_TRACE(headerLibraryName(Id).str());
    EXPECT_EQ(sizeOf(*Kernel, "NDIS_CONFIGURATION_OBJECT"), X86 ? 12u : 24u);
    EXPECT_EQ(sizeOf(*Kernel, "NDIS_CONFIGURATION_PARAMETER"), X86 ? 12u : 24u);
    EXPECT_EQ(sizeOf(*Kernel, "NDIS_STATUS"), 4u);
    EXPECT_EQ(sizeOf(*Kernel, "SOCKADDR_STORAGE"), 128u);
    EXPECT_EQ(sizeOf(*Kernel, "PCI_COMMON_CONFIG"), 256u);
    for (auto [Name, Count] : {std::pair{"NdisCloseConfiguration", 1u},
                               {"NdisOpenConfigurationEx", 2u},
                               {"NdisReadConfiguration", 5u},
                               {"FltReleaseFileNameInformation", 1u},
                               {"FltGetFileNameInformationUnsafe", 4u},
                               {"GetIfTable2Ex", 2u},
                               {"MmMapIoSpaceEx", 3u},
                               {"PsGetProcessPeb", 1u},
                               {"RtlAnsiCharToUnicodeChar", 1u},
                               {"ZwQuerySystemInformation", 4u}}) {
      const auto *Function = Kernel->findPrototype(Name);
      ASSERT_NE(Function, nullptr) << Name;
      EXPECT_EQ(Function->Params.size(), Count) << Name;
      EXPECT_FALSE(Function->Variadic) << Name;
      if (X86)
        EXPECT_EQ(Function->Convention, CallingConvention::Stdcall) << Name;
    }
    EXPECT_EQ(
        Kernel->findPrototype("NdisCloseConfiguration")->Return->TheBuiltin,
        Builtin::Void);
    EXPECT_EQ(
        Kernel->findPrototype("NdisReadConfiguration")->Return->TheBuiltin,
        Builtin::Void);
    EXPECT_EQ(Kernel->findPrototype("NdisOpenConfigurationEx")->Return->Name,
              "NDIS_STATUS");
    EXPECT_EQ(Kernel->findPrototype("GetIfTable2Ex")->Return->Name, "NTSTATUS");
    EXPECT_EQ(
        Kernel->findPrototype("RtlAnsiCharToUnicodeChar")->Return->TheBuiltin,
        Builtin::WChar);
    EXPECT_EQ(Kernel->findPrototype("NdisReadConfiguration")->Params[4].Name,
              "ParameterType");
  }
}

TEST(HeaderLibrary, Arm64KernelKeepsItsOwnContextAndRecordAlignment) {
  const auto *Kernel = headerLibrary(HeaderLibrary::WindowsKernelARM64);
  ASSERT_NE(Kernel, nullptr);
  EXPECT_EQ(Kernel->dataModel(), DataModel::LLP64);
  EXPECT_GT(Kernel->prototypes().size(), 2200u);
  EXPECT_EQ(sizeOf(*Kernel, "CONTEXT"), 912u);
  EXPECT_EQ(Kernel->alignOf(*makeNamed("CONTEXT", TypeKind::Typedef)), 16u);
  EXPECT_EQ(sizeOf(*Kernel, "PAGED_LOOKASIDE_LIST"), 128u);
  EXPECT_EQ(sizeOf(*Kernel, "NPAGED_LOOKASIDE_LIST"), 128u);
  EXPECT_EQ(sizeOf(*Kernel, "IO_STACK_LOCATION"), 72u);
  EXPECT_EQ(sizeOf(*Kernel, "CO_MEDIA_PARAMETERS"), 32u);
  EXPECT_EQ(Kernel->alignOf(*makeNamed("TOKEN_STATISTICS", TypeKind::Typedef)),
            8u);
  for (const char *Name :
       {"WHEA_REVISION", "WHEA_ERROR_RECORD_SECTION_DESCRIPTOR",
        "WHEA_PROCESSOR_GENERIC_ERROR_SECTION"})
    EXPECT_EQ(Kernel->alignOf(*makeNamed(Name, TypeKind::Typedef)), 1u) << Name;
  const auto *Context = Kernel->resolve("CONTEXT");
  ASSERT_NE(Context, nullptr);
  ASSERT_NE(Context->Record, nullptr);
  bool HasSp = false, HasPc = false;
  for (const auto &Field : Context->Record->Members) {
    if (Field.Name == "Sp") {
      HasSp = true;
      EXPECT_EQ(Field.BitOffset, 256u * 8);
    }
    if (Field.Name == "Pc") {
      HasPc = true;
      EXPECT_EQ(Field.BitOffset, 264u * 8);
    }
    EXPECT_NE(Field.Name, "Rip");
    EXPECT_NE(Field.Name, "Rsp");
  }
  EXPECT_TRUE(HasSp);
  EXPECT_TRUE(HasPc);
  for (auto [Name, OfficialSize] : {std::pair{"KUSER_SHARED_DATA", 2688u},
                                    {"NDIS_OFFLOAD", 188u},
                                    {"PHYSICAL_COUNTER_RESOURCE_LIST", 32u}}) {
    const auto *Type = Kernel->resolve(Name);
    ASSERT_NE(Type, nullptr) << Name;
    ASSERT_NE(Type->Record, nullptr) << Name;
    if (microsoftSource(HeaderLibrary::WindowsKernelARM64)) {
      EXPECT_TRUE(Type->Record->Defined) << Name;
      EXPECT_EQ(sizeOf(*Kernel, Name), OfficialSize) << Name;
    } else {
      EXPECT_FALSE(Type->Record->Defined) << Name;
      EXPECT_FALSE(Kernel->sizeOf(*makeNamed(Name, TypeKind::Typedef))) << Name;
    }
  }
}

TEST(HeaderLibrary, UserProvidersKeepTheirOwnStatusAndParameterWidths) {
  for (auto Id : {HeaderLibrary::WindowsUserX64, HeaderLibrary::WindowsUserX86,
                  HeaderLibrary::WindowsUserARM64}) {
    const auto *User = headerLibrary(Id);
    ASSERT_NE(User, nullptr);
    SCOPED_TRACE(headerLibraryName(Id).str());
    for (auto [Name, Count] : {std::pair{"WlanOpenHandle", 4u},
                               {"DwmFlush", 0u},
                               {"WinUsb_Initialize", 2u},
                               {"HidD_GetAttributes", 2u},
                               {"ImmGetContext", 1u},
                               {"FilterConnectCommunicationPort", 6u}}) {
      const auto *Function = User->findPrototype(Name);
      ASSERT_NE(Function, nullptr) << Name;
      EXPECT_EQ(Function->Params.size(), Count) << Name;
      EXPECT_FALSE(Function->Variadic) << Name;
      if (Id == HeaderLibrary::WindowsUserX86)
        EXPECT_EQ(Function->Convention, CallingConvention::Stdcall) << Name;
    }
    // NETIO's kernel status and IPHLPAPI's user status have the same width,
    // but keep their separate declarations and signedness.
    EXPECT_EQ(User->findPrototype("GetIfTable2Ex")->Return->Name, "DWORD");
  }
}

TEST(HeaderLibrary, OptionalUCRTExportsKeepFloatingVarargAndCallbackContracts) {
  for (auto Id : {HeaderLibrary::WindowsUserX64, HeaderLibrary::WindowsUserX86,
                  HeaderLibrary::WindowsUserARM64}) {
    const auto *User = headerLibrary(Id);
    ASSERT_NE(User, nullptr);
    if (!User->findPrototype("_o__hypot"))
      GTEST_SKIP() << "This build has no local Microsoft UCRT provider";
    SCOPED_TRACE(headerLibraryName(Id).str());
    const bool X86 = Id == HeaderLibrary::WindowsUserX86;
    const auto *Hypot = User->findPrototype("_o__hypot");
    ASSERT_EQ(Hypot->Params.size(), 2u);
    EXPECT_EQ(Hypot->Return->TheBuiltin, Builtin::Double);
    for (const auto &Param : Hypot->Params)
      EXPECT_EQ(Param.Type->TheBuiltin, Builtin::Double);
    const auto *Open = User->findPrototype("_wopen");
    ASSERT_NE(Open, nullptr);
    EXPECT_EQ(Open->Params.size(), 2u);
    EXPECT_TRUE(Open->Variadic);
    const auto *Begin = User->findPrototype("_o__beginthreadex");
    ASSERT_NE(Begin, nullptr);
    ASSERT_EQ(Begin->Params.size(), 6u);
    EXPECT_EQ(User->sizeOf(*Begin->Return), X86 ? 4u : 8u);
    const auto *Callback = User->resolve(Begin->Params[2].Type->Name);
    ASSERT_NE(Callback, nullptr);
    ASSERT_NE(Callback->Target, nullptr);
    ASSERT_EQ(Callback->Target->Kind, TypeExprKind::Pointer);
    ASSERT_NE(Callback->Target->Element, nullptr);
    const auto *Routine = Callback->Target->Element->Function.get();
    ASSERT_NE(Routine, nullptr);
    EXPECT_EQ(Routine->Params.size(), 1u);
    if (X86)
      EXPECT_EQ(Routine->Convention, CallingConvention::Stdcall);
    const auto *SetTerminate = User->findPrototype("_o_set_terminate");
    ASSERT_NE(SetTerminate, nullptr);
    ASSERT_EQ(SetTerminate->Params.size(), 1u);
    EXPECT_EQ(SetTerminate->Return->Name, SetTerminate->Params[0].Type->Name);
    const auto *Handler = User->resolve(SetTerminate->Return->Name);
    ASSERT_NE(Handler, nullptr);
    ASSERT_NE(Handler->Target, nullptr);
    ASSERT_EQ(Handler->Target->Kind, TypeExprKind::Pointer);
    ASSERT_NE(Handler->Target->Element, nullptr);
    const auto *Termination = Handler->Target->Element->Function.get();
    ASSERT_NE(Termination, nullptr);
    EXPECT_TRUE(Termination->Params.empty());
    EXPECT_FALSE(Termination->Variadic);
    if (X86) {
      EXPECT_EQ(Termination->Convention, CallingConvention::Cdecl);
      EXPECT_EQ(SetTerminate->Convention, CallingConvention::Cdecl);
    }
    const auto *PureCall = User->findPrototype("_o__purecall");
    ASSERT_NE(PureCall, nullptr);
    EXPECT_EQ(PureCall->Return->TheBuiltin, Builtin::Int);
    EXPECT_EQ(User->findPrototype("_mm_set1_epi32"), nullptr);
    EXPECT_EQ(User->findPrototype("_CIpow"), nullptr);
  }
}

TEST(HeaderLibrary, OptionalUCRTTerminationNeedsItsDeclarationContext) {
  for (auto Id : {HeaderLibrary::WindowsUserX64, HeaderLibrary::WindowsUserX86,
                  HeaderLibrary::WindowsUserARM64}) {
    const auto *User = headerLibrary(Id);
    ASSERT_NE(User, nullptr);
    if (!User->findPrototype("_o_terminate"))
      GTEST_SKIP() << "This build has no local Microsoft UCRT provider";
    for (const char *Name : {"terminate", "unexpected"}) {
      SCOPED_TRACE(headerLibraryName(Id).str());
      SCOPED_TRACE(Name);
      ASSERT_NE(User->findPrototype(Name), nullptr);
      EXPECT_TRUE(User->findPrototype(Name)->NoReturn);
      EXPECT_FALSE(neverd::libc::isNoReturnFunction(Name));
      neverd::BinaryImage Image;
      neverd::Import Imported;
      Imported.Name = Name;
      Imported.IATAddr = 0x1000;
      Image.Imports.push_back(Imported);
      const neverd::libc::NoReturnTargetIndex WithoutDeclaration(Image);
      EXPECT_FALSE(WithoutDeclaration.contains(Image, Imported.IATAddr));
      const neverd::libc::NoReturnTargetIndex WithDeclaration(
          Image, {}, [&](llvm::StringRef Name) {
            const auto *Prototype = User->findPrototype(Name);
            return Prototype && Prototype->NoReturn;
          });
      EXPECT_TRUE(WithDeclaration.contains(Image, Imported.IATAddr));
    }
  }
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
    EXPECT_GT(Types->size(), 300u);
    EXPECT_GT(Types->prototypes().size(), 1000u);
    const bool Wide = headerLibraryModel(Library) != DataModel::ILP32;
    if (headerLibrarySpelling(Library) == BuiltinSpelling::Windows) {
      EXPECT_EQ(sizeOf(*Types, "HANDLE"), Wide ? 8u : 4u);
      EXPECT_EQ(sizeOf(*Types, "SIZE_T"), Wide ? 8u : 4u);
      EXPECT_EQ(sizeOf(*Types, "_UNICODE_STRING"), Wide ? 16u : 8u);
    } else if (Types->find("EFI_GUID")) {
      // UEFI's own names, which hold no socket.
      EXPECT_EQ(sizeOf(*Types, "UINTN"), Wide ? 8u : 4u);
      EXPECT_EQ(sizeOf(*Types, "EFI_HANDLE"), Wide ? 8u : 4u);
      EXPECT_EQ(sizeOf(*Types, "EFI_GUID"), 16u);
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
      if (Prototype.getValue()->NoReturn) {
        // These context-specific CRT names are intentionally absent from the
        // global name table. Its declaration feeds NoReturnTargetIndex above.
        if (Prototype.getKey() == "terminate" ||
            Prototype.getKey() == "unexpected") {
          EXPECT_TRUE(microsoftSource(Library));
          if (Prototype.getKey() == "terminate")
            EXPECT_TRUE(Types->findPrototype("_o_terminate"));
          continue;
        }
        EXPECT_TRUE(neverd::libc::isNoReturnFunction(Prototype.getKey()))
            << headerLibraryName(Library).str() << " "
            << Prototype.getKey().str();
      }
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
    for (const Offset &O : Offsets) {
      // The SDK's public TEB keeps this field reserved. A supplement must
      // not replace its official definition with a private, larger record.
      if (microsoftSource(Library) &&
          llvm::StringRef(O.Member) == "LastErrorValue")
        continue;
      Assertions += std::string("_Static_assert(__builtin_offsetof(") +
                    O.Record + ", " + O.Member +
                    ") == " + std::to_string(Wide ? O.Wide : O.Narrow) +
                    ", \"" + O.Record + "." + O.Member + "\");\n";
    }
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

TEST(HeaderLibrary, TheAndroidLibrariesLayRecordsOutAsBionicDoes) {
  // bionic's records as each architecture's compilers lay them out, as clang
  // measures them against the same headers for API level 35 and the NDK's:
  // not glibc's, whose 32-bit pthread_mutex_t is 24 bytes and whose x64
  // sigaction 152.
  struct Expected {
    HeaderLibrary Library;
    uint64_t Stat, StatSize, Timespec, Mutex, Sigaction, Dirent;
  };
  const Expected Libraries[] = {
      {HeaderLibrary::AndroidX64, 144, 48, 16, 40, 32, 280},
      {HeaderLibrary::AndroidX86, 96, 44, 8, 4, 16, 276},
      {HeaderLibrary::AndroidARM64, 128, 48, 16, 40, 32, 280},
      {HeaderLibrary::AndroidARM, 104, 48, 8, 4, 16, 280},
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
        Check("sizeof(struct sigaction)", E.Sigaction) +
        Check("sizeof(struct dirent)", E.Dirent);
    // A copy, which the assertions read without changing the library.
    TypeLibrary Layout = *Types;
    const ParseResult Laid = parseCTypes(Assertions, Layout);
    EXPECT_TRUE(Laid.Problems.empty()) << Laid.Problems.front().Message;

    const FunctionDef *Init = Types->findPrototype("pthread_mutex_init");
    ASSERT_NE(Init, nullptr);
    EXPECT_EQ(Init->Params.size(), 2u);
    const FunctionDef *Read = Types->findPrototype("read");
    ASSERT_NE(Read, nullptr);
    ASSERT_EQ(Read->Params.size(), 3u);
    EXPECT_EQ(Read->Params[2].Name, "__count");
    ASSERT_NE(Types->findPrototype("__assert2"), nullptr);
    EXPECT_TRUE(Types->findPrototype("__assert2")->NoReturn);
    // bionic's strtol_l, which takes a locale, is strtol's symbol: strtol's
    // own declaration describes strtol.
    for (const char *Name : {"strtol", "strtoul", "wcstol"}) {
      const FunctionDef *Number = Types->findPrototype(Name);
      ASSERT_NE(Number, nullptr) << Name;
      EXPECT_EQ(Number->Params.size(), 3u) << Name;
    }

    // The NDK's <jni.h> and <android/log.h>: JNIEnv's function table, a
    // pointer for each of its 233 entries, and the log.
    const bool Wide = headerLibraryModel(E.Library) != DataModel::ILP32;
    const std::string JNI =
        Check("sizeof(struct JNINativeInterface)", Wide ? 1864 : 932) +
        Check("sizeof(JNIEnv)", Wide ? 8 : 4) +
        Check("sizeof(JavaVMInitArgs)", Wide ? 24 : 16) +
        Check("sizeof(jvalue)", 8);
    const ParseResult Native = parseCTypes(JNI, Layout);
    EXPECT_TRUE(Native.Problems.empty()) << Native.Problems.front().Message;
    const FunctionDef *Create = Types->findPrototype("JNI_CreateJavaVM");
    ASSERT_NE(Create, nullptr);
    EXPECT_EQ(Create->Params.size(), 3u);
    const FunctionDef *Print = Types->findPrototype("__android_log_print");
    ASSERT_NE(Print, nullptr);
    EXPECT_EQ(Print->Params.size(), 3u);
    EXPECT_TRUE(Print->Variadic);
    ASSERT_NE(Types->findPrototype("__android_log_assert"), nullptr);
    EXPECT_TRUE(Types->findPrototype("__android_log_assert")->NoReturn);
  }
}

TEST(HeaderLibrary, TheMuslLibrariesLayRecordsOutAsMuslDoes) {
  // musl's records as each architecture's compilers lay them out, as clang
  // measures them against the headers musl installs for it: a 32-bit
  // program's time_t is 64 bits, which makes its stat and timespec larger
  // than glibc's and bionic's, and its stat is __stat_time64.
  struct Expected {
    HeaderLibrary Library;
    uint64_t Stat, StatSize, Timespec, Mutex, Sigaction, Dirent;
    bool Time64;
  };
  const Expected Libraries[] = {
      {HeaderLibrary::MuslX64, 144, 48, 16, 40, 152, 280, false},
      {HeaderLibrary::MuslX86, 144, 44, 16, 24, 140, 276, true},
      {HeaderLibrary::MuslARM64, 128, 48, 16, 40, 152, 280, false},
      {HeaderLibrary::MuslARM, 152, 48, 16, 24, 140, 280, true},
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
        Check("sizeof(struct sigaction)", E.Sigaction) +
        Check("sizeof(struct dirent)", E.Dirent);
    // A copy, which the assertions read without changing the library.
    TypeLibrary Layout = *Types;
    const ParseResult Laid = parseCTypes(Assertions, Layout);
    EXPECT_TRUE(Laid.Problems.empty()) << Laid.Problems.front().Message;

    const FunctionDef *Read = Types->findPrototype("read");
    ASSERT_NE(Read, nullptr);
    EXPECT_EQ(Read->Params.size(), 3u);
    const FunctionDef *Stat = Types->findPrototype("__stat_time64");
    EXPECT_EQ(Stat != nullptr, E.Time64);
    if (Stat)
      EXPECT_EQ(Stat->Params.size(), 2u);
    ASSERT_NE(Types->findPrototype("exit"), nullptr);
    EXPECT_TRUE(Types->findPrototype("exit")->NoReturn);
  }
}

TEST(HeaderLibrary, TheDarwinLibrariesLayRecordsOutAsLibSystemDoes) {
  // libSystem's records as clang lays them out against the headers Apple's
  // projects install, for each platform; macOS and iOS share them, but for
  // the registers jmp_buf saves.  statfs's 2168 bytes are what the SDK's
  // encoding of fstatfs$INODE64 says too (DarwinSourceDeclarations.inc).
  struct Expected {
    HeaderLibrary Library;
    uint64_t JmpBuf;
    // An x64 program imports stat as stat$INODE64; ARM64 has only 64-bit
    // inodes.
    bool Inode64;
    // macOS names syslog syslog$DARWIN_EXTSN; iOS's is the plain one.
    bool ExtendedSyslog;
  };
  const Expected Libraries[] = {
      {HeaderLibrary::MacOSX64, 148, true, true},
      {HeaderLibrary::MacOSARM64, 192, false, true},
      {HeaderLibrary::IOSARM64, 192, false, false},
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
        Check("sizeof(struct stat)", 144) +
        Check("__builtin_offsetof(struct stat, st_size)", 96) +
        Check("sizeof(struct timespec)", 16) +
        Check("sizeof(pthread_mutex_t)", 64) + Check("sizeof(FILE)", 152) +
        Check("__builtin_offsetof(FILE, _offset)", 144) +
        Check("sizeof(struct sigaction)", 16) +
        Check("sizeof(struct dirent)", 1048) +
        Check("sizeof(struct statfs)", 2168) +
        Check("sizeof(jmp_buf)", E.JmpBuf);
    // A copy, which the assertions read without changing the library.
    TypeLibrary Layout = *Types;
    const ParseResult Laid = parseCTypes(Assertions, Layout);
    EXPECT_TRUE(Laid.Problems.empty()) << Laid.Problems.front().Message;

    const FunctionDef *Stat = Types->findPrototype("stat");
    ASSERT_NE(Stat, nullptr);
    EXPECT_EQ(Stat->Params.size(), 2u);
    const FunctionDef *Inode64 = Types->findPrototype("stat$INODE64");
    EXPECT_EQ(Inode64 != nullptr, E.Inode64);
    if (Inode64)
      EXPECT_EQ(Inode64->Params.size(), 2u);
    ASSERT_NE(Types->findPrototype("realpath$DARWIN_EXTSN"), nullptr);
    EXPECT_EQ(Types->findPrototype("syslog$DARWIN_EXTSN") != nullptr,
              E.ExtendedSyslog);
    ASSERT_NE(Types->findPrototype("_dyld_image_count"), nullptr);
    ASSERT_NE(Types->findPrototype("os_unfair_lock_lock"), nullptr);
    ASSERT_NE(Types->findPrototype("exit"), nullptr);
    EXPECT_TRUE(Types->findPrototype("exit")->NoReturn);
  }
}

TEST(HeaderLibrary, TheFreeBSDLibrariesLayRecordsOutAsFreeBSDDoes) {
  // FreeBSD's records as clang lays them out against FreeBSD 15.1's headers
  // for each architecture: the 64-bit inode stat i386 shortens by its 32-bit
  // time_t, a pthread_mutex_t that points to the mutex, and kqueue's kevent.
  struct Expected {
    HeaderLibrary Library;
    uint64_t Stat, StatSize, Timespec, Mutex, File, FileOffset, Sigaction,
        Kevent, JmpBuf;
  };
  const Expected Libraries[] = {
      {HeaderLibrary::FreeBSDX64, 224, 112, 16, 8, 312, 144, 32, 64, 96},
      {HeaderLibrary::FreeBSDX86, 208, 96, 8, 4, 236, 80, 24, 56, 48},
      {HeaderLibrary::FreeBSDARM64, 224, 112, 16, 8, 312, 144, 32, 64, 512},
      {HeaderLibrary::FreeBSDARM, 224, 112, 16, 4, 240, 80, 24, 64, 260},
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
        Check("sizeof(struct dirent)", 280) +
        Check("sizeof(struct kevent)", E.Kevent) +
        Check("sizeof(jmp_buf)", E.JmpBuf) +
        Check("sizeof(struct sockaddr_storage)", 128);
    // A copy, which the assertions read without changing the library.
    TypeLibrary Layout = *Types;
    const ParseResult Laid = parseCTypes(Assertions, Layout);
    EXPECT_TRUE(Laid.Problems.empty()) << Laid.Problems.front().Message;

    const auto Params = [&](const char *Name) -> size_t {
      const FunctionDef *Function = Types->findPrototype(Name);
      return Function ? Function->Params.size() : size_t(-1);
    };
    EXPECT_EQ(Params("stat"), 2u);
    EXPECT_EQ(Params("kevent"), 6u);
    EXPECT_EQ(Params("sendfile"), 7u);
    EXPECT_EQ(Params("cap_enter"), 0u);
    EXPECT_EQ(Params("sysctlbyname"), 5u);
    ASSERT_NE(Types->findPrototype("abort2"), nullptr);
    EXPECT_TRUE(Types->findPrototype("abort2")->NoReturn);
  }
}

TEST(HeaderLibrary, TheUefiLibrariesLayOutTheFirmwaresTables) {
  // The tables a UEFI module is given and the protocols it locates, as clang
  // lays them out against MdePkg for each architecture.  The library classes
  // of one kind of module are left out: each declares _ModuleEntryPoint its
  // way, and the specification's EFI_IMAGE_ENTRY_POINT types an
  // application's entry instead.
  struct Expected {
    HeaderLibrary Library;
    uint64_t SystemTable, BootServicesAt, BootServices, LocateProtocolAt,
        RuntimeServices, LoadedImage, ImageSizeAt, FileProtocol, OutputStringAt;
  };
  const Expected Libraries[] = {
      {HeaderLibrary::UefiX64, 120, 96, 376, 320, 136, 96, 72, 120, 8},
      {HeaderLibrary::UefiX86, 72, 60, 200, 172, 80, 64, 40, 64, 4},
      {HeaderLibrary::UefiARM64, 120, 96, 376, 320, 136, 96, 72, 120, 8},
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
        Check("sizeof(EFI_SYSTEM_TABLE)", E.SystemTable) +
        Check("__builtin_offsetof(EFI_SYSTEM_TABLE, BootServices)",
              E.BootServicesAt) +
        Check("sizeof(EFI_BOOT_SERVICES)", E.BootServices) +
        Check("__builtin_offsetof(EFI_BOOT_SERVICES, LocateProtocol)",
              E.LocateProtocolAt) +
        Check("sizeof(EFI_RUNTIME_SERVICES)", E.RuntimeServices) +
        Check("sizeof(EFI_LOADED_IMAGE_PROTOCOL)", E.LoadedImage) +
        Check("__builtin_offsetof(EFI_LOADED_IMAGE_PROTOCOL, ImageSize)",
              E.ImageSizeAt) +
        Check("sizeof(EFI_FILE_PROTOCOL)", E.FileProtocol) +
        Check("__builtin_offsetof(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL, "
              "OutputString)",
              E.OutputStringAt) +
        Check("sizeof(EFI_GUID)", 16) +
        Check("sizeof(EFI_GRAPHICS_OUTPUT_MODE_INFORMATION)", 36) +
        Check("sizeof(SMBIOS_TABLE_ENTRY_POINT)", 31);
    // A copy, which the assertions read without changing the library.
    TypeLibrary Layout = *Types;
    const ParseResult Laid = parseCTypes(Assertions, Layout);
    EXPECT_TRUE(Laid.Problems.empty()) << Laid.Problems.front().Message;

    const LocalType *Entry = Types->find("EFI_IMAGE_ENTRY_POINT");
    ASSERT_NE(Entry, nullptr);
    EXPECT_EQ(Entry->Kind, TypeKind::Typedef);
    EXPECT_EQ(Types->findPrototype("_ModuleEntryPoint"), nullptr);
    const FunctionDef *Copy = Types->findPrototype("CopyMem");
    ASSERT_NE(Copy, nullptr);
    EXPECT_EQ(Copy->Params.size(), 3u);
    const FunctionDef *Print = Types->findPrototype("AsciiPrint");
    ASSERT_NE(Print, nullptr);
    EXPECT_TRUE(Print->Variadic);
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

TEST(HeaderLibrary, SupplementsPreservePrimaryDeclarationsAndRejectChangedABI) {
  TypeLibrary Primary;
  TypeLibrary Supplement;
  ASSERT_TRUE(parseCTypes(R"(
    struct Base { char Bytes[8]; };
    void Existing(void *Object);
  )",
                          Primary)
                  .ok());
  ASSERT_TRUE(parseCTypes(R"(
    struct Base { unsigned long long Value; };
    struct Holder { char Prefix; struct Base Value; };
    int Existing(int Extra, int Argument);
    void Missing(void *Object);
    void ByValue(struct Base Value);
    void ByPointer(struct Base *Value);
    void BadHolder(struct Holder Value);
  )",
                          Supplement)
                  .ok());
  const auto Merge = mergeHeaderTypes(Primary, Supplement);
  ASSERT_NE(Primary.findPrototype("Existing"), nullptr);
  EXPECT_EQ(Primary.findPrototype("Existing")->Params.size(), 1u);
  EXPECT_EQ(Primary.findPrototype("Existing")->Return->TheBuiltin,
            Builtin::Void);
  EXPECT_EQ(Primary.find("Base")->Record->Members[0].Name, "Bytes");
  EXPECT_NE(Primary.findPrototype("Missing"), nullptr);
  EXPECT_NE(Primary.findPrototype("ByPointer"), nullptr);
  EXPECT_EQ(Primary.findPrototype("ByValue"), nullptr);
  EXPECT_EQ(Primary.findPrototype("BadHolder"), nullptr);
  EXPECT_EQ(Merge.Prototypes, 2u);
  EXPECT_EQ(Merge.Conflicts, (std::vector<std::string>{"Holder"}));
  EXPECT_FALSE(Primary.find("Holder")->Record->Defined);
}

TEST(HeaderLibrary, PrimaryAliasesExcludeSupplementalFunctionNames) {
  for (DataModel Model :
       {DataModel::ILP32, DataModel::LLP64, DataModel::LP64}) {
    TypeLibrary Primary(Model);
    TypeLibrary Supplement(Model);
    ASSERT_TRUE(parseCTypes(R"(
      typedef char Check[1];
      typedef int Callback(void);
      typedef struct Named { int Value; } Named;
      struct Tagged { int Value; };
      union UnionTag { int Value; };
      enum EnumTag { Value = 1 };
    )",
                            Primary)
                    .ok());
    ASSERT_TRUE(parseCTypes(R"(
      void Check(int *);
      void Callback(int *);
      void Named(int *);
      void Tagged(void);
      void UnionTag(void);
      void EnumTag(void);
      void Missing(void);
    )",
                            Supplement)
                    .ok());
    const auto Merge = mergeHeaderTypes(Primary, Supplement);
    EXPECT_EQ(Merge.Types, 0u);
    EXPECT_EQ(Merge.Prototypes, 4u);
    EXPECT_TRUE(Merge.Conflicts.empty());
    for (const char *Name : {"Check", "Callback", "Named"}) {
      EXPECT_NE(Primary.find(Name), nullptr) << Name;
      EXPECT_EQ(Primary.findPrototype(Name), nullptr) << Name;
    }
    for (const char *Name : {"Tagged", "UnionTag", "EnumTag", "Missing"})
      EXPECT_NE(Primary.findPrototype(Name), nullptr) << Name;
  }
}

TEST(HeaderLibrary, PrimaryFunctionsExcludeAliasesAndTheirRequiredNames) {
  for (DataModel Model :
       {DataModel::ILP32, DataModel::LLP64, DataModel::LP64}) {
    TypeLibrary Primary(Model);
    TypeLibrary Supplement(Model);
    ASSERT_TRUE(parseCTypes(R"(
      int Existing(void);
      void Callback(void);
      void Tagged(void);
      void Plain(void);
    )",
                            Primary)
                    .ok());
    ASSERT_TRUE(parseCTypes(R"(
      typedef int Existing;
      typedef int Callback(int);
      typedef Existing *Indirect;
      typedef void Signature(Indirect);
      struct Holder { Indirect *Link; };
      typedef struct Tagged { int Value; } Tagged;
      struct Plain { int Value; };
      void Dangling(Indirect);
      void Ref(Callback *);
      void TakeHolder(struct Holder *);
      void TakeTag(struct Tagged *);
      void TakePlain(struct Plain *);
    )",
                            Supplement)
                    .ok());
    const auto Merge = mergeHeaderTypes(Primary, Supplement);
    EXPECT_EQ(Merge.Types, 2u);
    EXPECT_EQ(Merge.Prototypes, 3u);
    EXPECT_EQ(Merge.Conflicts,
              (std::vector<std::string>{"Existing", "Callback", "Indirect",
                                        "Signature", "Holder"}));
    for (const char *Name : {"Existing", "Callback", "Indirect", "Signature"})
      EXPECT_EQ(Primary.find(Name), nullptr) << Name;
    ASSERT_NE(Primary.findPrototype("Existing"), nullptr);
    EXPECT_EQ(Primary.findPrototype("Existing")->Return->TheBuiltin,
              Builtin::Int);
    EXPECT_EQ(Primary.findPrototype("Dangling"), nullptr);
    EXPECT_EQ(Primary.findPrototype("Ref"), nullptr);
    ASSERT_NE(Primary.find("Holder"), nullptr);
    EXPECT_FALSE(Primary.find("Holder")->Record->Defined);
    ASSERT_NE(Primary.find("Tagged"), nullptr);
    EXPECT_FALSE(Primary.find("Tagged")->TypedefNamed);
    for (const char *Name : {"TakeHolder", "TakeTag", "TakePlain"})
      EXPECT_NE(Primary.findPrototype(Name), nullptr) << Name;
    RenderOptions Options;
    Options.Prototypes = true;
    Options.Spelling = BuiltinSpelling::Keywords;
    TypeLibrary Again(Model);
    ASSERT_TRUE(parseCTypes(renderTypes(Primary, {}, Options), Again).ok());
    EXPECT_EQ(Again.find("Existing"), nullptr);
    EXPECT_EQ(Again.prototypes().size(), Primary.prototypes().size());
  }
}

TEST(HeaderLibrary, ExecutivePointerAliasesUseTheProgramsPDBRecords) {
  for (const auto Id :
       {HeaderLibrary::WindowsKernelX86, HeaderLibrary::WindowsKernelX64,
        HeaderLibrary::WindowsKernelARM64}) {
    SCOPED_TRACE(headerLibraryName(Id).str());
    TypeLibrary Headers(headerLibraryModel(Id));
    ParseOptions HeaderOptions;
    HeaderOptions.Origin = TypeOrigin::Header;
    ASSERT_TRUE(parseCTypes(R"(
      typedef const struct _KPROCESS *PEPROCESS;
      typedef struct _KTHREAD *PETHREAD;
      typedef struct _KPROCESS *PKPROCESS;
      typedef struct _KTHREAD *PKTHREAD;
      void Observe(PEPROCESS Process, PETHREAD Thread);
    )",
                            Headers, HeaderOptions)
                    .ok());
    const auto Pointee = [](const TypeLibrary &Library, const char *Name) {
      const auto *Alias = Library.find(Name);
      return Alias && Alias->Target && Alias->Target->Element
                 ? Alias->Target->Element->Name
                 : std::string();
    };
    for (unsigned Variant = 0; Variant < 8; ++Variant) {
      SCOPED_TRACE(Variant);
      TypeLibrary Program(headerLibraryModel(Id));
      ParseOptions Options;
      Options.Origin = Variant == 1 ? TypeOrigin::User : TypeOrigin::PDB;
      ASSERT_TRUE(parseCTypes("struct _KPROCESS { unsigned long Flags; };\n"
                              "struct _KTHREAD { unsigned long Flags; };\n",
                              Program, Options)
                      .ok());
      const std::string Executive =
          Variant == 2 ? "struct _EPROCESS; struct _ETHREAD;"
                       : std::string("struct _EPROCESS { ") +
                             (Variant == 3 ? "unsigned long Before; " : "") +
                             "struct _KPROCESS Pcb; void *Identity; };\n"
                             "struct _ETHREAD { struct " +
                             (Variant == 4 ? "_KPROCESS" : "_KTHREAD") +
                             " Tcb; void *Identity; };";
      ASSERT_TRUE(parseCTypes(Executive, Program, Options).ok());
      if (Variant == 5)
        ASSERT_TRUE(parseCTypes("typedef void *PEPROCESS;", Program).ok());
      const std::optional<HeaderLibrary> Provider =
          Variant == 7 ? std::nullopt
                       : std::optional(
                             Variant == 6 ? HeaderLibrary::WindowsUserX64 : Id);
      const auto Merge = mergeHeaderTypes(Program, Headers, Provider);
      EXPECT_TRUE(Merge.Conflicts.empty());
      EXPECT_EQ(Pointee(Program, "PEPROCESS"), Variant == 5 ? ""
                                               : Variant == 0 || Variant == 4
                                                   ? "_EPROCESS"
                                                   : "_KPROCESS");
      EXPECT_EQ(Pointee(Program, "PETHREAD"),
                Variant == 0 || Variant == 3 || Variant == 5 ? "_ETHREAD"
                                                             : "_KTHREAD");
      EXPECT_EQ(Pointee(Program, "PKPROCESS"), "_KPROCESS");
      EXPECT_EQ(Pointee(Program, "PKTHREAD"), "_KTHREAD");
      if (Variant != 5) {
        const auto &Pointer = Program.find("PEPROCESS")->Target;
        EXPECT_EQ(Pointer->PointerBytes, pointerBytes(Program.dataModel()));
        EXPECT_TRUE(Pointer->Element->Const);
      }
      ASSERT_NE(Program.findPrototype("Observe"), nullptr);
      EXPECT_EQ(Program.findPrototype("Observe")->Params.size(), 2u);
      // The provider remains immutable for sessions with no program records.
      EXPECT_EQ(Pointee(Headers, "PEPROCESS"), "_KPROCESS");
    }
    TypeLibrary Program(headerLibraryModel(Id));
    EXPECT_TRUE(mergeHeaderTypes(Program, Headers).Conflicts.empty());
    EXPECT_EQ(Pointee(Program, "PEPROCESS"), "_KPROCESS");
  }
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

TEST(HeaderLibrary, NativeServiceContractsMatchTheirBundledDeclarations) {
  const TypeLibrary *Native = headerLibrary(HeaderLibrary::WindowsUserX64);
  ASSERT_NE(Native, nullptr);
  const char *Names[] = {
#define ND_WINDOWS_PROTO(Name, ...) Name,
#include "neverd/libc/WindowsNativeRoutines.inc"
#undef ND_WINDOWS_PROTO
      // WDK declarations also present in the bundled native-service headers.
      "ZwAllocateVirtualMemoryEx", "ZwCommitRegistryTransaction",
      "ZwCopyFileChunk",           "ZwCreateRegistryTransaction",
      "ZwFlushBuffersFileEx",      "ZwQueryDirectoryFileEx",
      "ZwQueryInformationByName",  "ZwSaveKeyEx",
  };
  for (const char *Name : Names) {
    SCOPED_TRACE(Name);
    const FunctionDef *Function = Native->findPrototype(Name);
    ASSERT_NE(Function, nullptr);
    ASSERT_FALSE(Function->Variadic);
    const auto *Contract = neverd::libc::windowsKernelPrototype(Name);
    ASSERT_NE(Contract, nullptr);
    ASSERT_EQ(Contract->ArgCount, Function->Params.size());
    for (size_t I = 0; I < Function->Params.size(); ++I)
      EXPECT_EQ(win64Bytes(*Native, *Function->Params[I].Type),
                Contract->ArgWidths[I]);
  }
}

TEST(HeaderLibrary, EveryUnambiguousDdkNativeServiceHasAnExactCallContract) {
  // Check declarations toward contracts as well as contracts toward
  // declarations: checking only existing rows cannot catch a missing API.
  for (const auto KernelId :
       {HeaderLibrary::WindowsKernelX64, HeaderLibrary::WindowsKernelARM64}) {
    SCOPED_TRACE(headerLibraryName(KernelId).str());
    const TypeLibrary *Kernel = headerLibrary(KernelId);
    const TypeLibrary *Native =
        headerLibrary(KernelId == HeaderLibrary::WindowsKernelX64
                          ? HeaderLibrary::WindowsUserX64
                          : HeaderLibrary::WindowsUserARM64);
    ASSERT_NE(Kernel, nullptr);
    ASSERT_NE(Native, nullptr);
    size_t Declared = 0, Conflicting = 0;
    for (const auto &Entry : Kernel->prototypes()) {
      if (!Entry.first().starts_with("Zw"))
        continue;
      ++Declared;
      SCOPED_TRACE(Entry.first().str());
      const FunctionDef *Function = Kernel->findPrototype(Entry.first());
      ASSERT_NE(Function, nullptr);
      ASSERT_FALSE(Function->Variadic);
      const auto *Contract =
          neverd::libc::windowsKernelPrototype(Entry.first().str());
      const FunctionDef *Other = Native->findPrototype(Entry.first());
      bool Agrees = !Other || (Other->Variadic == Function->Variadic &&
                               Other->Params.size() == Function->Params.size());
      if (Other && Agrees)
        for (size_t I = 0; I < Function->Params.size(); ++I)
          Agrees &= win64Bytes(*Kernel, *Function->Params[I].Type) ==
                    win64Bytes(*Native, *Other->Params[I].Type);
      if (!Agrees) {
        ++Conflicting;
        EXPECT_EQ(Contract, nullptr);
        continue;
      }
      EXPECT_NE(Contract, nullptr);
      if (!Contract)
        continue;
      ASSERT_EQ(Contract->ArgCount, Function->Params.size());
      for (size_t I = 0; I < Function->Params.size(); ++I)
        EXPECT_EQ(win64Bytes(*Kernel, *Function->Params[I].Type),
                  Contract->ArgWidths[I])
            << "parameter " << I;
    }
    // Review a new conflict instead of silently increasing the exception set.
    // The known FlushInstructionCache disagreement has a separate check below.
    EXPECT_GE(Declared, 141u);
    EXPECT_EQ(Conflicting, 1u);
  }
}

TEST(HeaderLibrary, ConflictingNativeWidthsDoNotAuthorizeAContract) {
  const TypeLibrary *Kernel = headerLibrary(HeaderLibrary::WindowsKernelX64);
  const TypeLibrary *Native = headerLibrary(HeaderLibrary::WindowsUserX64);
  ASSERT_NE(Kernel, nullptr);
  ASSERT_NE(Native, nullptr);
  const auto *Ddk = Kernel->findPrototype("ZwFlushInstructionCache");
  const auto *Phnt = Native->findPrototype("ZwFlushInstructionCache");
  ASSERT_NE(Ddk, nullptr);
  ASSERT_NE(Phnt, nullptr);
  ASSERT_EQ(Ddk->Params.size(), 3u);
  ASSERT_EQ(Phnt->Params.size(), 3u);
  EXPECT_EQ(win64Bytes(*Kernel, *Ddk->Params[2].Type), 4u);
  EXPECT_EQ(win64Bytes(*Native, *Phnt->Params[2].Type), 8u);
  EXPECT_EQ(neverd::libc::windowsKernelPrototype("ZwFlushInstructionCache"),
            nullptr);
}
