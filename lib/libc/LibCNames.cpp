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

#include "neverd/libc/LibCAssert.h"
#include "neverd/libc/LibCComplex.h"
#include "neverd/libc/LibCCtype.h"
#include "neverd/libc/LibCDirent.h"
#include "neverd/libc/LibCDlfcn.h"
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

// A (name, fixed-parameter-count) row of a variadic-function table: the number
// of parameters before the trailing "..." for the named function.
struct VarArgEntry {
  std::string_view Name;
  unsigned FixedCount;
};

// Fortified _FORTIFY_SOURCE forms `__<core>_chk` prepend guard arguments (a
// flag, and for the buffer-writing forms the destination/object size) before
// the format string, so they carry more fixed parameters than the plain
// `<core>` they wrap.  Keyed by the core name with leading underscores and the
// `_chk` suffix already stripped.
inline constexpr auto kFortifiedChkFixed = std::to_array<VarArgEntry>({
    {"printf", 2},   // (flag, fmt, ...)
    {"fprintf", 3},  // (stream, flag, fmt, ...)
    {"dprintf", 3},  // (fd, flag, fmt, ...)
    {"asprintf", 3}, // (&buf, flag, fmt, ...)
    {"sprintf", 4},  // (buf, flag, slen, fmt, ...)
    {"snprintf", 5}, // (buf, maxlen, flag, slen, fmt, ...)
});

// Irregular variadic functions whose fixed-parameter count is not derivable
// from a printf/scanf suffix (the syslog/err/warn families, and the POSIX
// open/fcntl/ioctl/exec*/mq_open/sem_open forms).  Each takes one fixed
// parameter before "...".
inline constexpr auto kIrregularVarArg = std::to_array<VarArgEntry>({
    {"syslog", 1},
    {"err", 1},
    {"errx", 1},
    {"warn", 1},
    {"warnx", 1},
    {"open", 1},
    {"openat", 1},
    {"fcntl", 1},
    {"ioctl", 1},
    {"execl", 1},
    {"execlp", 1},
    {"execle", 1},
    {"mq_open", 1},
    {"sem_open", 1},
});

// Irregular `va_list`-consuming functions with no printf/scanf suffix (the
// v-prefixed syslog/err/warn forms).
inline constexpr auto kIrregularVaListConsumers =
    std::to_array<std::string_view>({
        "vsyslog",
        "verr",
        "verrx",
        "vwarn",
        "vwarnx",
    });

// Looks up Name in a (name, fixed-count) table; nullopt if absent.
template <size_t N>
std::optional<unsigned>
lookupFixedCount(const std::array<VarArgEntry, N> &Table,
                 std::string_view Name) {
  for (const VarArgEntry &E : Table)
    if (E.Name == Name)
      return E.FixedCount;
  return std::nullopt;
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
  // Fortified _FORTIFY_SOURCE variants: __<core>_chk.  clang emits these for
  // the buffer-bounded printf family when the destination size is known
  // (default on macOS / glibc).  The caller has stripped at most one leading
  // '_' (a Mach-O symbol prefix), so the name arrives as "__snprintf_chk"
  // (Mach-O) or
  // "_snprintf_chk" (ELF over-strip); normalize by dropping every leading
  // underscore, then map the core name through kFortifiedChkFixed.
  if (Name.ends_with("_chk")) {
    std::string_view Core = Name.substr(0, Name.size() - 4);
    while (!Core.empty() && Core.front() == '_')
      Core.remove_prefix(1);
    // v*_chk take a va_list, not "...", so they are not variadic.
    if (!Core.empty() && Core.front() == 'v')
      return 0;
    return lookupFixedCount(kFortifiedChkFixed, Core).value_or(0);
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
    if (Name == "printf" || Name == "scanf" || Name == "wprintf" ||
        Name == "wscanf")
      return 1;
    // "snprintf", "snwprintf", "swprintf" → 3 fixed (buf + size + fmt).  C99
    // swprintf has the same signature shape as snprintf.
    if (Name.substr(0, 2) == "sn" || Name == "swprintf")
      return 3;
    // "fprintf", "fscanf", "sprintf", "sscanf", "dprintf", "asprintf", … → 2.
    return 2;
  }

  // Irregular variadic functions with no printf/scanf suffix.
  return lookupFixedCount(kIrregularVarArg, Name).value_or(0);
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
  for (std::string_view S : kIrregularVaListConsumers)
    if (Name == S)
      return true;
  return false;
}

bool isNoReturnFunction(std::string_view Name) {
  while (!Name.empty() && Name.front() == '_')
    Name.remove_prefix(1);
  // Every entry is unconditionally __attribute__((noreturn)) in its standard
  // header.  Names whose canonical form keeps a leading underscore appear here
  // already underscore-stripped (_exit -> "exit", _Exit -> "Exit").  Functions
  // that only *sometimes* return (warn/warnx, GNU error/error_at_line) are
  // deliberately excluded so the CFG builder never drops genuinely reachable
  // fall-through code.
  static constexpr std::string_view kNoReturn[] = {
      "abort",          "exit",       "Exit",
      "quick_exit",     "longjmp",    "siglongjmp",
      "pthread_exit",   "thrd_exit",  "err",
      "errx",           "verr",       "verrx",
      "assert_fail",    "assert_rtn", "assert_perror_fail",
      "stack_chk_fail", "cxa_throw",  "cxa_rethrow",
      "Unwind_Resume",
  };
  for (std::string_view S : kNoReturn)
    if (Name == S)
      return true;
  return false;
}

bool isReturnsTwiceFunction(std::string_view Name) {
  while (!Name.empty() && Name.front() == '_')
    Name.remove_prefix(1);
  // setjmp / _setjmp / sigsetjmp / __sigsetjmp all normalize to one of these.
  return Name == "setjmp" || Name == "sigsetjmp";
}

} // namespace neverd::libc
