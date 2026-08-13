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
#include "neverd/libc/LibCUtime.h"
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
    registerFunctions(All, ToHeader, kUtimeFunctions, kUtimeHeader);
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

} // namespace neverd::libc
