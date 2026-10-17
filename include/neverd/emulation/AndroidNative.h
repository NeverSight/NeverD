//===- AndroidNative.h - Explicit Android native call inputs ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_EMULATION_ANDROIDNATIVE_H
#define NEVERD_EMULATION_ANDROIDNATIVE_H

#include "neverd/emulation/AndroidResolverOptions.h"
#include "neverd/emulation/ProcessCall.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace neverd::emulation {
struct NativeMemoryRegion {
  uint64_t Address = 0, Size = 0;
  /// Initial bytes followed by zero padding. Address and Size are page aligned.
  std::vector<uint8_t> Bytes;
  bool Executable = false;
  /// Copy this explicit regular file into the region before execution, then
  /// zero pad. Mutually exclusive with Bytes; guest writes never modify it.
  /// Relative paths use the caller's working directory. The file must fit Size.
  std::optional<std::filesystem::path> File;
};
struct NativeMemoryRead {
  uint64_t Address = 0, Size = 0;
  /// Require a readable mapping before guest execution. False permits memory
  /// allocated during the call; the complete range must be readable at the
  /// final resumable stop. Neither setting creates or preserves mappings.
  bool RequireMappedAtEntry = true;
};
struct NativeMemorySnapshot {
  uint64_t Address;
  std::vector<uint8_t> Bytes;
};

struct AndroidAuxiliaryValue {
  uint64_t Type = 0, Value = 0;
};

/// A complete fixed zone with no transitions or leap-second corrections.
/// OffsetSeconds is east of UTC; Name is an abbreviation, not a tzdb lookup.
struct AndroidFixedTimeZone {
  int32_t OffsetSeconds = 0;
  std::string Name;
};

/// Read-only observations immediately before selected guest instructions.
/// PCs are absolute guest addresses, not image-relative offsets. At most 64
/// distinct aligned PCs; Limit bounds the retained records across all threads.
struct NativeExecutionObservationOptions {
  std::vector<uint64_t> PCs;
  uint64_t Limit = 0;
  /// The same explicit ranges are copied at each point. Deferred mappings
  /// must be readable when observed; no mapping or provider is synthesized.
  std::vector<NativeMemoryRead> ReadMemory;
};
struct NativeExecutionObservation {
  uint64_t PC = 0, ThreadID = 0, Instructions = 0, SP = 0, NZCV = 0;
  std::array<uint64_t, 31> Registers{};
  std::vector<NativeMemorySnapshot> Memory;
};

/// One visible logd v4 entry. Payload is opaque; no host log is consulted.
struct NativeLogRecord {
  uint32_t ID = 0, PID = 0, TID = 0, Seconds = 0, Nanoseconds = 0, UID = 0;
  std::vector<uint8_t> Payload;
};

/// Android 9 / API 28, little-endian AArch64. Function
/// arguments are AAPCS64 scalar register/stack values, not process argv.
/// Explicit inputs never inherit host properties, files, or environment.
struct AndroidNativeOptions {
  uint64_t LoadBias = 0x40000000;
  /// Select exactly one exported symbol or link-time virtual address.
  std::string EntrySymbol;
  std::optional<uint64_t> EntryAddress;
  std::vector<uint64_t> Arguments;
  /// Complete immutable property snapshot. Values are at most 91 bytes.
  /// Find/read/enumeration share stable guest records with generation zero;
  /// enumeration uses name order, not an inferred device-area traversal order.
  std::map<std::string, std::string> Properties;
  /// Complete immutable getauxval input, excluding the implicit AT_NULL
  /// terminator. Absent means unknown; empty means no entries. At most 128
  /// unique nonzero types. Values are guest bits, never host pointers or CPU
  /// capability queries, and do not authorize otherwise unsupported execution.
  std::optional<std::vector<AndroidAuxiliaryValue>> AuxiliaryVector;
  /// localtime requires this explicit fixed zone. gmtime uses GMT without
  /// consulting it. Neither queries host time, TZ, properties or tzdata.
  /// The admitted calendar range is years 1..9999; other years stop clearly.
  std::optional<AndroidFixedTimeZone> FixedTimeZone;
  /// Exact offline getaddrinfo observations. Missing queries remain unknown.
  std::optional<std::vector<AndroidResolverQuery>> ResolverQueries;
  /// Complete, timestamp-ordered visible snapshot for the bounded liblog
  /// reader. Absent is unknown; an empty vector explicitly supplies no logs.
  /// IDs 0..5, at most 4096 entries and 1 MiB including v4 headers. This does
  /// not infer permissions, logd sockets, live producers or persistent logs.
  std::optional<std::vector<NativeLogRecord>> LogRecords;
  std::vector<NativeMemoryRegion> Memory;
  std::vector<NativeMemoryRead> ReadMemory;
  /// Constructors normally execute before the call, sharing its budget.
  /// False explicitly requests analysis of an uninitialized library.
  bool Initialize = true;
  /// Retain this many admitted instruction PCs. Zero disables tracing.
  uint64_t TraceLimit = 0;
  std::optional<NativeExecutionObservationOptions> ObserveExecution;
  /// Concrete analysis choice for bytes returned by malloc and the new tail
  /// of realloc. Zero preserves the default. This is not a claim about an
  /// Android allocator or proof that reading uninitialized bytes is valid.
  /// calloc and model-owned allocations remain zeroed; padding is excluded.
  uint8_t MallocFillByte = 0;
  /// One preserves the single-thread contract. Values 2..256 opt into bounded
  /// cooperative guest threads. Counts all created identities, including the
  /// entry thread; retired identities are never reused within a workload.
  uint64_t ThreadLimit = 1;
  /// Continue scheduling children after the selected entry returns. Otherwise
  /// stop at that return and report the remaining thread states. Requires
  /// ThreadLimit > 1; never replenishes execution limits.
  bool DrainThreads = false;
  /// Opt into a bounded JNI 1.6 native-interface model. Threads initially
  /// remain detached. Intrinsic classes and explicitly supplied DEX bodies
  /// share this workload VM; framework services are not inferred.
  bool JNI = false;
  /// Complete explicit DEX 035/037/038/039 images (Android API 28). No host
  /// classpath is consulted. Requires JNI; bounded integral static bodies only.
  std::vector<std::vector<uint8_t>> DexImages;
  /// Exact library names and available function names for the local dlfcn
  /// model. No host files are loaded. Calling an unmodeled function still
  /// stops.
  std::map<std::string, std::vector<std::string>> Libraries;
  /// Complete ordered RTLD_DEFAULT search scope for this workload. Entries
  /// name resident providers in Libraries; dlopen/close do not change this
  /// scope or unload them. Absent means unknown (unsupported); empty means
  /// explicitly no providers. No namespace or dependency order is inferred.
  std::optional<std::vector<std::string>> DefaultScope;
};

/// Model-owned identity and placement, not a public pthread_internal_t layout.
struct NativeThreadSnapshot {
  uint64_t ID, Handle, TLS, StackBase, StackSize, GuardSize;
  bool Finished = false, Detached = false, Retired = false, Waiting = false;
  std::optional<uint64_t> ReturnValue;
};
struct NativeTraceThread {
  uint64_t Index, ID;
};
} // namespace neverd::emulation
#endif
