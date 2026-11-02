//===- LibCNames.cpp - libc/POSIX registry and lookup -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// libc/POSIX symbol registration and signature lookup.
///
//===----------------------------------------------------------------------===//

#include "neverd/libc/LibCNames.h"

#include "LibCVariantRows.h"

#include "neverd/libc/LibCAssert.h"
#include "neverd/libc/LibCComplex.h"
#include "neverd/libc/LibCCtype.h"
#include "neverd/libc/LibCDirent.h"
#include "neverd/libc/LibCDlfcn.h"
#include "neverd/libc/LibCExceptionRuntime.h"
#include "neverd/libc/LibCFcntl.h"
#include "neverd/libc/LibCFenv.h"
#include "neverd/libc/LibCFnmatch.h"
#include "neverd/libc/LibCFortify.h"
#include "neverd/libc/LibCGetopt.h"
#include "neverd/libc/LibCGrp.h"
#include "neverd/libc/LibCInttypes.h"
#include "neverd/libc/LibCLanginfo.h"
#include "neverd/libc/LibCLibintl.h"
#include "neverd/libc/LibCLink.h"
#include "neverd/libc/LibCLocale.h"
#include "neverd/libc/LibCMath.h"
#include "neverd/libc/LibCNlTypes.h"
#include "neverd/libc/LibCObjC.h"
#include "neverd/libc/LibCPoll.h"
#include "neverd/libc/LibCPthread.h"
#include "neverd/libc/LibCPwd.h"
#include "neverd/libc/LibCRegex.h"
#include "neverd/libc/LibCResult.h"
#include "neverd/libc/LibCSched.h"
#include "neverd/libc/LibCSearch.h"
#include "neverd/libc/LibCSemaphore.h"
#include "neverd/libc/LibCSetjmp.h"
#include "neverd/libc/LibCShadow.h"
#include "neverd/libc/LibCSignal.h"
#include "neverd/libc/LibCSpawn.h"
#include "neverd/libc/LibCStartup.h"
#include "neverd/libc/LibCStdbit.h"
#include "neverd/libc/LibCStdfix.h"
#include "neverd/libc/LibCStdio.h"
#include "neverd/libc/LibCStdioExt.h"
#include "neverd/libc/LibCStdlib.h"
#include "neverd/libc/LibCString.h"
#include "neverd/libc/LibCStrings.h"
#include "neverd/libc/LibCTermios.h"
#include "neverd/libc/LibCThreads.h"
#include "neverd/libc/LibCTime.h"
#include "neverd/libc/LibCUchar.h"
#include "neverd/libc/LibCUcontext.h"
#include "neverd/libc/LibCUnistd.h"
#include "neverd/libc/LibCUtime.h"
#include "neverd/libc/LibCWchar.h"
#include "neverd/libc/LibCWctype.h"
#include "neverd/libc/WindowsCRT.h"
#include "neverd/libc/arpa/LibCInet.h"
#include "neverd/libc/sys/LibCAuxv.h"
#include "neverd/libc/sys/LibCEpoll.h"
#include "neverd/libc/sys/LibCIoctl.h"
#include "neverd/libc/sys/LibCIpc.h"
#include "neverd/libc/sys/LibCMman.h"
#include "neverd/libc/sys/LibCPrctl.h"
#include "neverd/libc/sys/LibCPtrace.h"
#include "neverd/libc/sys/LibCRandom.h"
#include "neverd/libc/sys/LibCResource.h"
#include "neverd/libc/sys/LibCSelect.h"
#include "neverd/libc/sys/LibCSem.h"
#include "neverd/libc/sys/LibCSendfile.h"
#include "neverd/libc/sys/LibCSocket.h"
#include "neverd/libc/sys/LibCStat.h"
#include "neverd/libc/sys/LibCStatvfs.h"
#include "neverd/libc/sys/LibCTime.h"
#include "neverd/libc/sys/LibCUio.h"
#include "neverd/libc/sys/LibCUtsname.h"
#include "neverd/libc/sys/LibCWait.h"

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace neverd::libc {
namespace {

template <typename T, size_t N>
void registerFunctions(std::unordered_set<std::string> &All,
                       std::unordered_map<std::string, std::string> &ToHeader,
                       const std::array<T, N> &Names, std::string_view Header) {
  const std::string Hdr(Header);
  for (const T &Name : Names) {
    std::string Key{std::string_view{Name}};
    All.insert(Key);
    ToHeader.emplace(std::move(Key), Hdr);
  }
}

struct Registry {
  std::unordered_set<std::string> All;
  std::unordered_map<std::string, std::string> ToHeader;

  Registry() {
    registerFunctions(All, ToHeader, kAssertFunctions, kAssertHeader);
    registerFunctions(All, ToHeader, kComplexFunctions, kComplexHeader);
    registerFunctions(All, ToHeader, kCtypeFunctions, kCtypeHeader);
    registerFunctions(All, ToHeader, kFenvFunctions, kFenvHeader);
    registerFunctions(All, ToHeader, kInttypesFunctions, kInttypesHeader);
    registerFunctions(All, ToHeader, kLocaleFunctions, kLocaleHeader);
    registerFunctions(All, ToHeader, kMathFunctions, kMathHeader);
    registerFunctions(All, ToHeader, kSetjmpFunctions, kSetjmpHeader);
    registerFunctions(All, ToHeader, kSignalFunctions, kSignalHeader);
    registerFunctions(All, ToHeader, kStdbitFunctions, kStdbitHeader);
    registerFunctions(All, ToHeader, kStdfixFunctions, kStdfixHeader);
    registerFunctions(All, ToHeader, kStdioFunctions, kStdioHeader);
    registerFunctions(All, ToHeader, kStdlibFunctions, kStdlibHeader);
    registerFunctions(All, ToHeader, kStringFunctions, kStringHeader);
    registerFunctions(All, ToHeader, kThreadsFunctions, kThreadsHeader);
    registerFunctions(All, ToHeader, kTimeFunctions, kTimeHeader);
    registerFunctions(All, ToHeader, kWcharFunctions, kWcharHeader);
    registerFunctions(All, ToHeader, kWctypeFunctions, kWctypeHeader);
    registerFunctions(All, ToHeader, kDirentFunctions, kDirentHeader);
    registerFunctions(All, ToHeader, kDlfcnFunctions, kDlfcnHeader);
    registerFunctions(All, ToHeader, kFcntlFunctions, kFcntlHeader);
    registerFunctions(All, ToHeader, kLibintlFunctions, kLibintlHeader);
    registerFunctions(All, ToHeader, kFnmatchFunctions, kFnmatchHeader);
    registerFunctions(All, ToHeader, kGetoptFunctions, kGetoptHeader);
    registerFunctions(All, ToHeader, kLanginfoFunctions, kLanginfoHeader);
    registerFunctions(All, ToHeader, kStdioExtFunctions, kStdioExtHeader);
    registerFunctions(All, ToHeader, kUcharFunctions, kUcharHeader);
    registerFunctions(All, ToHeader, kLinkFunctions, kLinkHeader);
    registerFunctions(All, ToHeader, kNlTypesFunctions, kNlTypesHeader);
    registerFunctions(All, ToHeader, kObjCFunctions, kObjCHeader);
    registerFunctions(All, ToHeader, kPollFunctions, kPollHeader);
    registerFunctions(All, ToHeader, kPthreadFunctions, kPthreadHeader);
    registerFunctions(All, ToHeader, kPwdFunctions, kPwdHeader);
    registerFunctions(All, ToHeader, kGrpFunctions, kGrpHeader);
    registerFunctions(All, ToHeader, kShadowFunctions, kShadowHeader);
    registerFunctions(All, ToHeader, kRegexFunctions, kRegexHeader);
    registerFunctions(All, ToHeader, kSchedFunctions, kSchedHeader);
    registerFunctions(All, ToHeader, kSearchFunctions, kSearchHeader);
    registerFunctions(All, ToHeader, kSemaphoreFunctions, kSemaphoreHeader);
    registerFunctions(All, ToHeader, kSpawnFunctions, kSpawnHeader);
    registerFunctions(All, ToHeader, kStringsFunctions, kStringsHeader);
    registerFunctions(All, ToHeader, kTermiosFunctions, kTermiosHeader);
    registerFunctions(All, ToHeader, kUcontextFunctions, kUcontextHeader);
    registerFunctions(All, ToHeader, kUnistdFunctions, kUnistdHeader);
    registerFunctions(All, ToHeader, kUtimeFunctions, kUtimeHeader);
    registerFunctions(All, ToHeader, kArpaInetFunctions, kArpaInetHeader);
    registerFunctions(All, ToHeader, kSysAuxvFunctions, kSysAuxvHeader);
    registerFunctions(All, ToHeader, kSysEpollFunctions, kSysEpollHeader);
    registerFunctions(All, ToHeader, kSysIoctlFunctions, kSysIoctlHeader);
    registerFunctions(All, ToHeader, kSysIpcFunctions, kSysIpcHeader);
    registerFunctions(All, ToHeader, kSysMmanFunctions, kSysMmanHeader);
    registerFunctions(All, ToHeader, kSysPrctlFunctions, kSysPrctlHeader);
    registerFunctions(All, ToHeader, kSysPtraceFunctions, kSysPtraceHeader);
    registerFunctions(All, ToHeader, kSysRandomFunctions, kSysRandomHeader);
    registerFunctions(All, ToHeader, kSysResourceFunctions, kSysResourceHeader);
    registerFunctions(All, ToHeader, kSysSelectFunctions, kSysSelectHeader);
    registerFunctions(All, ToHeader, kSysSemFunctions, kSysSemHeader);
    registerFunctions(All, ToHeader, kSysSendfileFunctions, kSysSendfileHeader);
    registerFunctions(All, ToHeader, kSysSocketFunctions, kSysSocketHeader);
    registerFunctions(All, ToHeader, kSysStatFunctions, kSysStatHeader);
    registerFunctions(All, ToHeader, kSysStatvfsFunctions, kSysStatvfsHeader);
    registerFunctions(All, ToHeader, kSysTimeFunctions, kSysTimeHeader);
    registerFunctions(All, ToHeader, kSysUioFunctions, kSysUioHeader);
    registerFunctions(All, ToHeader, kSysUtsnameFunctions, kSysUtsnameHeader);
    registerFunctions(All, ToHeader, kSysWaitFunctions, kSysWaitHeader);
    // A large-file routine is its routine's header's (LibCLargeFile.def).
#define NEVERD_LIBC_LARGE_FILE(NAME, ROUTINE)                                  \
  if (const auto Base = ToHeader.find(ROUTINE); Base != ToHeader.end()) {      \
    std::string Header = Base->second;                                         \
    All.insert(NAME);                                                          \
    ToHeader.emplace(NAME, std::move(Header));                                 \
  }
#include "neverd/libc/LibCLargeFile.def"
  }
};

const Registry &getRegistry() {
  static const Registry Instance;
  return Instance;
}

template <size_t N>
void registerArity(std::unordered_map<std::string_view, LibCArity> &Map,
                   const std::array<LibCArityEntry, N> &Entries) {
  for (const LibCArityEntry &E : Entries)
    Map.emplace(E.Name, E.Arity);
}

/// The arity of each non-variadic routine of \p Prototypes, keyed as the
/// arity tables key a name: without its leading underscores.  Every parameter
/// fills an integer or pointer argument.
template <size_t N>
void registerPrototypeArity(
    std::unordered_map<std::string_view, LibCArity> &Map,
    const std::array<LibCPrototype, N> &Prototypes) {
  for (const LibCPrototype &Prototype : Prototypes)
    if (!Prototype.Variadic)
      Map.emplace(std::string_view(stripLeadingUnderscores(Prototype.Name)),
                  LibCArity{static_cast<int>(Prototype.ParamCount), 0});
}

// Curated arity table for common NON-variadic libc functions whose signature is
// fixed and well known.  Used to bound argument recovery for an external call
// (the heuristic register/FP/stack scans otherwise over-collect: a dead
// incoming parameter still in its register, a SIMD scratch register, or a spill
// near the call -- which then makes the emitter declare the callee variadic and
// place the bogus overflow on the stack, corrupting the frame).  The per-header
// kXxxArity tables in libc_*.h list only entries we are confident about;
// anything absent is left to the heuristic scans unchanged.
struct ArityRegistry {
  std::unordered_map<std::string_view, LibCArity> Map;
  std::unordered_set<std::string_view> NativeComplex;

  ArityRegistry() {
    registerArity(Map, kStdioArity);
    registerArity(Map, kStringArity);
    registerArity(Map, kStringsArity);
    registerArity(Map, kCtypeArity);
    registerArity(Map, kStdlibArity);
    registerArity(Map, kUnistdArity);
    registerArity(Map, kMathArity);
    registerArity(Map, kComplexArity);
    for (const auto &Entry : kComplexArity)
      NativeComplex.insert(Entry.Name);
    registerArity(Map, kTimeArity);
    registerArity(Map, kExceptionRuntimeArity);
    registerArity(Map, kSetjmpArity);
    registerArity(Map, kLibintlArity);
    registerArity(Map, kAssertArity);
    registerArity(Map, kDirentArity);
    registerArity(Map, kDlfcnArity);
    registerArity(Map, kFcntlArity);
    registerArity(Map, kFenvArity);
    registerArity(Map, kInttypesArity);
    registerArity(Map, kLinkArity);
    registerArity(Map, kLocaleArity);
    registerArity(Map, kNlTypesArity);
    registerArity(Map, kPollArity);
    registerArity(Map, kPthreadArity);
    registerArity(Map, kPwdArity);
    registerArity(Map, kGrpArity);
    registerArity(Map, kShadowArity);
    registerArity(Map, kRegexArity);
    registerArity(Map, kSchedArity);
    registerArity(Map, kSearchArity);
    registerArity(Map, kSemaphoreArity);
    registerArity(Map, kSignalArity);
    registerArity(Map, kSpawnArity);
    registerArity(Map, kTermiosArity);
    registerArity(Map, kThreadsArity);
    registerArity(Map, kUcontextArity);
    registerArity(Map, kUtimeArity);
    registerArity(Map, kWcharArity);
    registerArity(Map, kWctypeArity);
    registerArity(Map, kArpaInetArity);
    registerArity(Map, kSysAuxvArity);
    registerArity(Map, kSysEpollArity);
    registerArity(Map, kSysIpcArity);
    registerArity(Map, kSysMmanArity);
    registerArity(Map, kSysRandomArity);
    registerArity(Map, kSysResourceArity);
    registerArity(Map, kSysSelectArity);
    registerArity(Map, kSysSemArity);
    registerArity(Map, kSysSendfileArity);
    registerArity(Map, kSysSocketArity);
    registerArity(Map, kSysStatArity);
    registerArity(Map, kSysStatvfsArity);
    registerArity(Map, kSysTimeArity);
    registerArity(Map, kSysUioArity);
    registerArity(Map, kSysUtsnameArity);
    registerArity(Map, kSysWaitArity);
    registerArity(Map, kGetoptArity);
    registerArity(Map, kLanginfoArity);
    registerArity(Map, kFnmatchArity);
    registerArity(Map, kStdioExtArity);
    registerArity(Map, kUcharArity);
    // The Windows C runtime's names are those of other runtimes' routines
    // once their underscores go (`_lock`, `terminate`): its prototypes apply
    // by object format alone.
    registerPrototypeArity(Map, kStartupPrototypes);
    registerPrototypeArity(Map, kItaniumRuntimePrototypes);
    registerPrototypeArity(Map, kFortifyPrototypes);
  }
};

const ArityRegistry &getArityRegistry() {
  static const ArityRegistry Instance;
  return Instance;
}

template <size_t N>
void registerPrototypes(
    std::unordered_map<std::string_view, const LibCPrototype *> &Map,
    const std::array<LibCPrototype, N> &Prototypes) {
  for (const LibCPrototype &Prototype : Prototypes)
    Map.emplace(Prototype.Name, &Prototype);
}

/// The prototypes of the routines no standard header declares, by C name.
struct PrototypeRegistry {
  std::unordered_map<std::string_view, const LibCPrototype *> Map;

  PrototypeRegistry() {
    registerPrototypes(Map, kStartupPrototypes);
    registerPrototypes(Map, kItaniumRuntimePrototypes);
    registerPrototypes(Map, kFortifyPrototypes);
    registerPrototypes(Map, kWindowsCRTPrototypes);
  }
};

const PrototypeRegistry &getPrototypeRegistry() {
  static const PrototypeRegistry Instance;
  return Instance;
}

} // anonymous namespace

std::string_view largeFileRoutine(std::string_view Name) {
  static const std::unordered_map<std::string_view, std::string_view> Routines =
      {
#define NEVERD_LIBC_LARGE_FILE(NAME, ROUTINE) {NAME, ROUTINE},
#include "neverd/libc/LibCLargeFile.def"
      };
  const auto It = Routines.find(Name);
  return It == Routines.end() ? std::string_view() : It->second;
}

bool isKnownFunction(std::string_view Name) {
  return getRegistry().All.count(std::string(Name)) > 0;
}

const char *headerFor(std::string_view Name) {
  const auto &Map = getRegistry().ToHeader;
  auto It = Map.find(std::string(Name));
  if (It == Map.end())
    return nullptr;
  return It->second.c_str();
}

std::optional<LibCArity> libcArity(std::string_view Name, LibCVariant Variant) {
  Name = routineOfSymbol(Name, Variant);
  if (Variant != LibCVariant::GNU) {
    if (std::optional<LibCArity> Arity = variant_rows::arity(Name, Variant))
      return Arity;
    // A routine variadic there takes no fixed arguments alone.
    if (variant_rows::variadic(Name, Variant))
      return std::nullopt;
  }
  const auto &Registry = getArityRegistry();
  // Microsoft's complex.h uses _Dcomplex/_Fcomplex records, whose carrier
  // depends on the target ABI. The native _Complex FP-register shorthand
  // cannot describe them; their full header types own call classification.
  if (Variant == LibCVariant::Windows && Registry.NativeComplex.count(Name))
    return std::nullopt;
  const auto &Map = Registry.Map;
  auto It = Map.find(Name);
  if (It == Map.end())
    return std::nullopt;
  return It->second;
}

const LibCPrototype *libcPrototype(std::string_view Name, BinaryFormat Format) {
  const auto &Map = getPrototypeRegistry().Map;
  const auto It = Map.find(Name);
  if (It == Map.end())
    return nullptr;
  const LibCPrototype *Prototype = It->second;
  return Prototype->Format == BinaryFormat::Unknown ||
                 Prototype->Format == Format
             ? Prototype
             : nullptr;
}

std::optional<LibCResult> libcResult(std::string_view Name,
                                     LibCVariant Variant) {
  // A large-file routine returns what its routine does.
  if (const std::string_view Routine = largeFileRoutine(Name); !Routine.empty())
    return libcResult(Routine, Variant);
  Name = routineOfSymbol(Name, Variant);
  // A Windows C runtime's own prototypes are COFF's (LibCPrototype::Format).
  const BinaryFormat Format = Variant == LibCVariant::Windows
                                  ? BinaryFormat::COFF
                                  : BinaryFormat::Unknown;
  if (const LibCPrototype *Prototype = libcPrototype(Name, Format))
    return Prototype->Return == "void"        ? LibCResult::Void
           : isPointerType(Prototype->Return) ? LibCResult::Pointer
                                              : LibCResult::Value;
  if (const std::optional<LibCResult> Result =
          variant_rows::result(Name, Variant))
    return Result;
  static const std::unordered_map<std::string_view, LibCResult> Kinds = {
#define NEVERD_LIBC_RETURN_KIND(NAME, KIND) {NAME, LibCResult::KIND},
#include "neverd/libc/LibCReturnKinds.inc"
#undef NEVERD_LIBC_RETURN_KIND
  };
  const auto It = Kinds.find(Name);
  if (It == Kinds.end())
    return std::nullopt;
  return It->second;
}

std::optional<bool> libcReturnsValue(std::string_view Name,
                                     LibCVariant Variant) {
  const std::optional<LibCResult> Result = libcResult(Name, Variant);
  if (!Result)
    return std::nullopt;
  return *Result != LibCResult::Void;
}

const char *headerOfDataObject(std::string_view Name) {
  static const std::unordered_map<std::string_view, const char *> Headers = {
#define NEVERD_LIBC_DATA_OBJECT(NAME, HEADER, TYPE) {NAME, HEADER},
#include "neverd/libc/LibCDataObjects.def"
  };
  const auto It = Headers.find(Name);
  return It == Headers.end() ? nullptr : It->second;
}

const char *typeOfDataObject(std::string_view Name) {
  static const std::unordered_map<std::string_view, const char *> Types = {
#define NEVERD_LIBC_DATA_OBJECT(NAME, HEADER, TYPE) {NAME, TYPE},
#include "neverd/libc/LibCDataObjects.def"
  };
  const auto It = Types.find(Name);
  return It == Types.end() ? nullptr : It->second;
}

bool returnsDoubleWord(std::string_view Name, unsigned WordBytes) {
  static const std::unordered_map<std::string_view, unsigned> Words = {
#define NEVERD_DOUBLE_WORD_RESULT(NAME, WORD_BYTES) {NAME, WORD_BYTES},
#include "neverd/libc/CompilerRuntimeDoubleWord.def"
  };
  const auto It = Words.find(Name);
  return It != Words.end() && It->second == WordBytes;
}

std::optional<LibCArity> libcArityForSymbol(std::string_view Name,
                                            LibCVariant Variant) {
  // Mach-O prepends one underscore to the C identifier `__error`, producing
  // `___error`.  Do not normalize it to `error`: GNU error(3) is a distinct
  // variadic function, while Darwin __error(void) takes no arguments.
  if (Name == "___error")
    return LibCArity{};
  while (!Name.empty() && Name.front() == '_')
    Name.remove_prefix(1);
  return libcArity(Name, Variant);
}

} // namespace neverd::libc
