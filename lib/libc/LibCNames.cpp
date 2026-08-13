//===- LibCNames.cpp - libc/POSIX symbol name registry ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// C standard library function name recognition.
///
//===----------------------------------------------------------------------===//

#include "neverd/libc/LibCNames.h"

#include "neverd/Common.h"
#include "neverd/libc/LibCAssert.h"
#include "neverd/libc/LibCComplex.h"
#include "neverd/libc/LibCCtype.h"
#include "neverd/libc/LibCDirent.h"
#include "neverd/libc/LibCDlfcn.h"
#include "neverd/libc/LibCExceptionRuntime.h"
#include "neverd/libc/LibCFcntl.h"
#include "neverd/libc/LibCFenv.h"
#include "neverd/libc/LibCInttypes.h"
#include "neverd/libc/LibCLink.h"
#include "neverd/libc/LibCLocale.h"
#include "neverd/libc/LibCMath.h"
#include "neverd/libc/LibCNlTypes.h"
#include "neverd/libc/LibCPoll.h"
#include "neverd/libc/LibCPthread.h"
#include "neverd/libc/LibCRegex.h"
#include "neverd/libc/LibCSched.h"
#include "neverd/libc/LibCSearch.h"
#include "neverd/libc/LibCSetjmp.h"
#include "neverd/libc/LibCSignal.h"
#include "neverd/libc/LibCSpawn.h"
#include "neverd/libc/LibCStdbit.h"
#include "neverd/libc/LibCStdfix.h"
#include "neverd/libc/LibCStdio.h"
#include "neverd/libc/LibCStdlib.h"
#include "neverd/libc/LibCString.h"
#include "neverd/libc/LibCStrings.h"
#include "neverd/libc/LibCTermios.h"
#include "neverd/libc/LibCThreads.h"
#include "neverd/libc/LibCTime.h"
#include "neverd/libc/LibCUcontext.h"
#include "neverd/libc/LibCUnistd.h"
#include "neverd/libc/LibCWchar.h"
#include "neverd/libc/LibCWctype.h"
#include "neverd/libc/arpa/LibCInet.h"
#include "neverd/libc/sys/LibCAuxv.h"
#include "neverd/libc/sys/LibCEpoll.h"
#include "neverd/libc/sys/LibCIoctl.h"
#include "neverd/libc/sys/LibCIpc.h"
#include "neverd/libc/sys/LibCMman.h"
#include "neverd/libc/sys/LibCPrctl.h"
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

#include "llvm/ADT/StringSwitch.h"

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
    registerFunctions(All, ToHeader, kLinkFunctions, kLinkHeader);
    registerFunctions(All, ToHeader, kNlTypesFunctions, kNlTypesHeader);
    registerFunctions(All, ToHeader, kPollFunctions, kPollHeader);
    registerFunctions(All, ToHeader, kPthreadFunctions, kPthreadHeader);
    registerFunctions(All, ToHeader, kRegexFunctions, kRegexHeader);
    registerFunctions(All, ToHeader, kSchedFunctions, kSchedHeader);
    registerFunctions(All, ToHeader, kSearchFunctions, kSearchHeader);
    registerFunctions(All, ToHeader, kSpawnFunctions, kSpawnHeader);
    registerFunctions(All, ToHeader, kStringsFunctions, kStringsHeader);
    registerFunctions(All, ToHeader, kTermiosFunctions, kTermiosHeader);
    registerFunctions(All, ToHeader, kUcontextFunctions, kUcontextHeader);
    registerFunctions(All, ToHeader, kUnistdFunctions, kUnistdHeader);
    registerFunctions(All, ToHeader, kArpaInetFunctions, kArpaInetHeader);
    registerFunctions(All, ToHeader, kSysAuxvFunctions, kSysAuxvHeader);
    registerFunctions(All, ToHeader, kSysEpollFunctions, kSysEpollHeader);
    registerFunctions(All, ToHeader, kSysIoctlFunctions, kSysIoctlHeader);
    registerFunctions(All, ToHeader, kSysIpcFunctions, kSysIpcHeader);
    registerFunctions(All, ToHeader, kSysMmanFunctions, kSysMmanHeader);
    registerFunctions(All, ToHeader, kSysPrctlFunctions, kSysPrctlHeader);
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

  ArityRegistry() {
    registerArity(Map, kStdioArity);
    registerArity(Map, kStringArity);
    registerArity(Map, kStringsArity);
    registerArity(Map, kCtypeArity);
    registerArity(Map, kStdlibArity);
    registerArity(Map, kUnistdArity);
    registerArity(Map, kMathArity);
    registerArity(Map, kComplexArity);
    registerArity(Map, kTimeArity);
    registerArity(Map, kExceptionRuntimeArity);
  }
};

const ArityRegistry &getArityRegistry() {
  static const ArityRegistry Instance;
  return Instance;
}

// --- Variadic / va_list signature tables ---
//
// Most of the printf/scanf family's fixed-parameter count follows from the name
// suffix and is computed by pattern below; the handful of irregular cases that
// cannot be patterned live in the declarative tables here, kept table-driven in
// the same style as the kXxxArity tables above rather than scattered as inline
// string comparisons inside varArgFixedCount/isVaListConsumer.

unsigned fortifiedChkFixedCount(std::string_view Core) {
  return llvm::StringSwitch<unsigned>(Core)
#define LIBC_FORTIFIED_CHK_FIXED(Name, Count) .Case(Name, Count)
#include "neverd/libc/LibCFortifiedChkFixed.inc"
#undef LIBC_FORTIFIED_CHK_FIXED
      .Default(0);
}

unsigned irregularVarArgFixedCount(std::string_view Name) {
  return llvm::StringSwitch<unsigned>(Name)
#define LIBC_IRREGULAR_VARARG(Name, Count) .Case(Name, Count)
#include "neverd/libc/LibCIrregularVarArg.inc"
#undef LIBC_IRREGULAR_VARARG
      .Default(0);
}

} // anonymous namespace

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

std::optional<LibCArity> libcArity(std::string_view Name) {
  const auto &Map = getArityRegistry().Map;
  auto It = Map.find(Name);
  if (It == Map.end())
    return std::nullopt;
  return It->second;
}

unsigned varArgFixedCount(std::string_view Name) {
  // Modern Darwin linkers specialize Objective-C dispatch through local
  // `_objc_msgSend$<selector>` stubs.  The stub materializes `_cmd` in x1,
  // while the caller supplies the receiver in x0 and one argument per colon
  // starting at x2.  Model the colon-counted prefix as fixed and leave an
  // ellipsis after it: ordinary methods simply pass no tail, while methods
  // such as stringWithFormat: preserve their true stack-passed varargs.
  constexpr std::string_view ObjCMsgSendPrefix = "objc_msgSend$";
  if (Name.starts_with(ObjCMsgSendPrefix)) {
    std::string_view Selector = Name.substr(ObjCMsgSendPrefix.size());
    if (Selector.empty())
      return 0;
    unsigned Fixed = 2; // receiver and linker-supplied _cmd
    for (char C : Selector)
      if (C == ':')
        ++Fixed;
    return Fixed;
  }

  // Fortified _FORTIFY_SOURCE variants: __<core>_chk.  clang emits these for
  // the buffer-bounded printf family when the destination size is known
  // (default on macOS / glibc).  The caller has stripped at most one leading
  // '_' (a Mach-O symbol prefix), so the name arrives as "__snprintf_chk"
  // (Mach-O) or
  // "_snprintf_chk" (ELF over-strip); normalize by dropping every leading
  // underscore, then map the core name through LibCFortifiedChkFixed.inc.
  if (Name.ends_with("_chk")) {
    std::string_view Core = Name.substr(0, Name.size() - 4);
    while (!Core.empty() && Core.front() == '_')
      Core.remove_prefix(1);
    // v*_chk take a va_list, not "...", so they are not variadic.
    if (!Core.empty() && Core.front() == 'v')
      return 0;
    return fortifiedChkFixedCount(Core);
  }

  // The *printf / *scanf families: the fixed-parameter count follows from the
  // name suffix (covers printf, fprintf, snprintf, the wide w* forms, and
  // platform variants like _snprintf / swprintf).
  if (Name.ends_with("printf") || Name.ends_with("scanf")) {
    // v*printf / v*scanf take a va_list, NOT "..." — they are not variadic.
    if (Name.starts_with('v') && (Name.substr(1).ends_with("printf") ||
                                  Name.substr(1).ends_with("scanf")))
      return 0;
    // "printf" / "scanf" (and the wide w* forms) alone → 1 fixed (format
    // string).
    if (unsigned Count = llvm::StringSwitch<unsigned>(Name)
#define LIBC_VARARG_FORMAT_NAME(Name, FixedCount) .Case(Name, FixedCount)
#include "neverd/libc/LibCVarArgFormatNames.inc"
#undef LIBC_VARARG_FORMAT_NAME
            .Default(0))
      return Count;
    // "snprintf", "snwprintf", "swprintf" → 3 fixed (buf + size + fmt).  C99
    // swprintf has the same signature shape as snprintf.
    if (Name.substr(0, 2) == "sn" || Name == "swprintf")
      return 3;
    // "fprintf", "fscanf", "sprintf", "sscanf", "dprintf", "asprintf", … → 2.
    return 2;
  }

  // Irregular variadic functions with no printf/scanf suffix.
  return irregularVarArgFixedCount(Name);
}

bool isVaListConsumer(std::string_view Name) {
  // Fortified __v*_chk forms (e.g. __vsnprintf_chk) take a va_list just like
  // their plain v* base; strip the suffix and fall through to the v* check.
  if (Name.ends_with("_chk"))
    Name = Name.substr(0, Name.size() - 4);
  // The v-prefixed printf/scanf family: the trailing argument is a va_list, so
  // any function that routes its own varargs into one of these is variadic.
  if (Name.starts_with('v') &&
      (Name.ends_with("printf") || Name.ends_with("scanf")))
    return true;
  // Irregular va_list consumers (no printf/scanf suffix).
  return llvm::StringSwitch<bool>(Name)
#define LIBC_VA_LIST_CONSUMER(Name) .Case(Name, true)
#include "neverd/libc/LibCVaListConsumers.inc"
#undef LIBC_VA_LIST_CONSUMER
      .Default(false);
}

bool isNoReturnFunction(std::string_view Name) {
  // Every entry is unconditionally __attribute__((noreturn)) in its standard
  // header.  Names whose canonical form keeps a leading underscore appear here
  // already underscore-stripped (_exit -> "exit", _Exit -> "Exit").  Functions
  // that only *sometimes* return (warn/warnx, GNU error/error_at_line) are
  // deliberately excluded so the CFG builder never drops genuinely reachable
  // fall-through code.
  return llvm::StringSwitch<bool>(stripLeadingUnderscores(Name))
#define LIBC_NO_RETURN_SYMBOL(Name) .Case(Name, true)
#include "neverd/libc/LibCNoReturn.inc"
#undef LIBC_NO_RETURN_SYMBOL
      .Default(false);
}

bool isReturnsTwiceFunction(std::string_view Name) {
  // setjmp / _setjmp / sigsetjmp / __sigsetjmp all normalize to one of these.
  return llvm::StringSwitch<bool>(stripLeadingUnderscores(Name))
#define LIBC_RETURNS_TWICE_SYMBOL(Name) .Case(Name, true)
#include "neverd/libc/LibCReturnsTwice.inc"
#undef LIBC_RETURNS_TWICE_SYMBOL
      .Default(false);
}

bool isMemCopyName(std::string_view Name) {
  return llvm::StringSwitch<bool>(stripLeadingUnderscores(Name))
#define LIBC_MEM_COPY_SYMBOL(Name) .Case(Name, true)
#include "neverd/libc/LibCMemCopyNames.inc"
#undef LIBC_MEM_COPY_SYMBOL
      .Default(false);
}

bool isMemSetName(std::string_view Name) {
  return llvm::StringSwitch<bool>(stripLeadingUnderscores(Name))
#define LIBC_MEM_SET_SYMBOL(Name) .Case(Name, true)
#include "neverd/libc/LibCMemSetNames.inc"
#undef LIBC_MEM_SET_SYMBOL
      .Default(false);
}

} // namespace neverd::libc
