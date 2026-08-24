//===- SBFSyscalls.h - Solana runtime syscall metadata ----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SBF_RUNTIME_SBFSYSCALLS_H
#define NEVERD_SBF_RUNTIME_SBFSYSCALLS_H

#include "neverd/sbf/SBFConstants.h"
#include "neverd/sbf/runtime/SBFRuntimeProfile.h"
#include "neverd/sbf/solana/SBFPubkey.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace neverd::sbf {

enum class Syscall : uint8_t {
#define SBF_SYSCALL(ID, NAME, ARGUMENT_COUNT, POINTER_ARGUMENTS, RETURN_KIND,  \
                    CATEGORY, EFFECTS, AVAILABILITY, SOURCE)                   \
  ID,
#include "neverd/sbf/runtime/SBFSyscalls.def"
  Unknown,
};

/// Position of one syscall argument, named the way a published signature
/// counts them and ordered so the enumerator's value is the zero-based ordinal.
enum class SyscallArgument : uint8_t {
#define SBF_ARGUMENT_REGISTER(ID, REGISTER) ID,
#include "neverd/sbf/SBFArgumentRegisters.def"
};

constexpr unsigned argumentOrdinal(SyscallArgument Argument) {
  return static_cast<unsigned>(Argument);
}

constexpr unsigned argumentRegister(SyscallArgument Argument) {
  return kFirstArgumentRegister + argumentOrdinal(Argument);
}

/// Which argument registers carry a VM address rather than a scalar.
///
/// SBPFv3 maps read-only data at virtual address zero, so a small scalar such
/// as a length is indistinguishable from a low data address by value alone.
/// Recovery therefore has to know the ABI to tell a pointer from a count.
///
/// This says only that a register holds an address. What the runtime does
/// through that address is a separate fact, and lives in SBFSyscallMemory.def.
enum class SyscallPointerArguments : uint8_t {
  None = 0,
  Arg1 = 1u << 0,
  Arg2 = 1u << 1,
  Arg3 = 1u << 2,
  Arg4 = 1u << 3,
  Arg5 = 1u << 4,
};

constexpr SyscallPointerArguments operator|(SyscallPointerArguments L,
                                            SyscallPointerArguments R) {
  return static_cast<SyscallPointerArguments>(static_cast<uint8_t>(L) |
                                              static_cast<uint8_t>(R));
}

/// Whether the zero-based argument \p Ordinal carries a VM address.
constexpr bool isPointerArgument(SyscallPointerArguments Set,
                                 unsigned Ordinal) {
  return Ordinal < kArgumentRegisterCount &&
         (static_cast<uint8_t>(Set) & (uint8_t{1} << Ordinal)) != 0;
}

constexpr bool isPointerArgument(SyscallPointerArguments Set,
                                 SyscallArgument Argument) {
  return isPointerArgument(Set, argumentOrdinal(Argument));
}

enum class SyscallReturnKind : uint8_t {
  Never,
  Void,
  Status,
  Value,
  Boolean,
  Address,
};

/// How settled a syscall's published signature is.
///
/// This is not the same question as whether a call to it resolves, and the two
/// stopped having the same answer once the runtime started building two
/// registries and gating entries in each of them per cluster. Ask
/// syscallRegistration for that.
enum class SyscallLifecycle : uint8_t {
#define SBF_SYSCALL_LIFECYCLE(ID, SPELLING, SUMMARY) ID,
#include "neverd/sbf/runtime/SBFSyscallLifecycle.def"
};

struct SyscallLifecycleInfo {
  SyscallLifecycle ID;
  llvm::StringLiteral Name;
  llvm::StringLiteral Summary;
};

llvm::ArrayRef<SyscallLifecycleInfo> syscallLifecycleInfos();
llvm::StringRef syscallLifecycleName(SyscallLifecycle Lifecycle);

enum class SyscallSource : uint8_t {
#define SBF_UPSTREAM_SOURCE(ID, NAME, REVISION) ID,
#include "neverd/sbf/runtime/SBFUpstreamSources.def"
  Unknown,
};

enum class SyscallCategory : uint8_t {
  Terminal,
  Logging,
  Memory,
  PDA,
  Crypto,
  CPI,
  Sysvar,
  Runtime,
  Deprecated,
};

enum class SyscallEffect : uint8_t {
  None = 0,
  ReadsMemory = 1u << 0,
  WritesMemory = 1u << 1,
  Terminal = 1u << 2,
  CPI = 1u << 3,
};

constexpr SyscallEffect operator|(SyscallEffect L, SyscallEffect R) {
  return static_cast<SyscallEffect>(static_cast<uint8_t>(L) |
                                    static_cast<uint8_t>(R));
}

constexpr bool hasEffect(SyscallEffect Set, SyscallEffect Effect) {
  return (static_cast<uint8_t>(Set) & static_cast<uint8_t>(Effect)) != 0;
}

struct SyscallInfo {
  Syscall ID;
  llvm::StringLiteral Name;
  uint32_t Hash;
  uint8_t ArgumentCount;
  SyscallPointerArguments PointerArguments;
  SyscallReturnKind ReturnKind;
  SyscallCategory Category;
  SyscallEffect Effects;
  SyscallLifecycle Lifecycle;
  SyscallSource Source;
};

//===----------------------------------------------------------------------===//
// Whether a call to one resolves
//
// Three separate things decide this, and collapsing any two of them gives an
// answer that is right for one chain and wrong for the next.
//===----------------------------------------------------------------------===//

/// The registries \p ID appears in. Nearly every syscall is in both; the ones
/// that are not are the reason a program that runs cannot always be deployed.
RuntimePurposeSet syscallPurposes(Syscall ID);

/// Which way a gate points, because one that removes a syscall reads exactly
/// like one that adds it unless the direction is recorded.
enum class SyscallGatePolarity : uint8_t {
#define SBF_SYSCALL_GATE_POLARITY(ID, NAME, SUMMARY) ID,
#include "neverd/sbf/runtime/SBFSyscallRegistration.def"
};

llvm::StringRef syscallGatePolarityName(SyscallGatePolarity Polarity);

struct SyscallGateInfo {
  Syscall ID;
  RuntimeFeature Feature;
  SyscallGatePolarity Polarity;
  /// Registries in which the gate applies. Most gates apply to both; sparse
  /// purpose-scoped rows preserve historical registry differences.
  RuntimePurposeSet Purposes;
};

llvm::ArrayRef<SyscallGateInfo> syscallGateInfos();
/// The gate governing \p ID, or null when nothing does.
const SyscallGateInfo *getSyscallGate(Syscall ID);

/// Why a program's reference to a syscall would or would not resolve.
enum class SyscallRegistration : uint8_t {
  /// The runtime registers it, so the call links.
  Registered,
  /// A gate the profile does not satisfy. Which gate is in getSyscallGate.
  GateUnmet,
  /// The registry the profile selects does not contain it at all, whatever the
  /// chain has switched on.
  EnvironmentExcluded,
};

llvm::StringRef syscallRegistrationName(SyscallRegistration Registration);

/// Whether \p ID resolves in an explicitly selected Agave registry and
/// feature snapshot.  This is the primitive authority used by both named
/// chain profiles and conformance snapshots whose feature set came directly
/// from a validator test vector.
SyscallRegistration syscallRegistration(Syscall ID, RuntimePurpose Purpose,
                                        RuntimeFeature ActiveFeatures);

/// Whether \p ID resolves under \p Profile, and if not, which of the two
/// reasons applies.
SyscallRegistration syscallRegistration(Syscall ID,
                                        const RuntimeProfile &Profile);

/// Every registered syscall hash for one Agave registry/feature snapshot,
/// sorted and deduplicated for deterministic legacy relocation lookup.
std::vector<uint32_t> registeredSyscallHashes(RuntimePurpose Purpose,
                                              RuntimeFeature ActiveFeatures);

/// Report a gate or environment row naming a syscall the syscall table does
/// not declare, or two rows claiming the same syscall.
llvm::Error validateSyscallRegistrationTable();

//===----------------------------------------------------------------------===//
// Caller-memory windows
//===----------------------------------------------------------------------===//

/// Which direction the runtime moves bytes through one address argument.
enum class SyscallMemoryAccess : uint8_t { Read, Write };

/// How a signature bounds the window an address argument opens.
enum class SyscallExtent : uint8_t {
  /// A protocol-sized object, such as an address or a digest.
  Fixed,
  /// A buffer whose byte length is another argument.
  Counted,
  /// A buffer this signature does not bound. A buffer never extends below its
  /// own start, so an opaque window still proves that nothing below the base
  /// address is touched.
  Opaque,
};

llvm::StringRef syscallMemoryAccessName(SyscallMemoryAccess Access);
llvm::StringRef syscallExtentName(SyscallExtent Extent);

/// One window of caller memory a syscall reads or writes.
struct SyscallMemoryInfo {
  Syscall ID;
  /// The argument holding the window's base address.
  SyscallArgument Argument;
  SyscallMemoryAccess Access;
  SyscallExtent Extent;
  /// Bytes for a Fixed extent, the length argument's ordinal for a Counted
  /// one, and unused otherwise. Read it through the two accessors, which
  /// answer only for the extent the value belongs to.
  uint64_t Detail;

  std::optional<uint64_t> fixedBytes() const;
  std::optional<SyscallArgument> lengthArgument() const;
};

llvm::ArrayRef<SyscallMemoryInfo> syscallMemoryInfos();

/// The windows \p ID opens, in table order.
llvm::ArrayRef<SyscallMemoryInfo> getSyscallMemory(Syscall ID);

/// True when the table proves \p ID cannot change any byte the caller wrote.
///
/// This is what lets a value proven before a call still be proven after it.
/// An unlisted syscall is not proven harmless, so it answers false.
bool preservesCallerMemory(Syscall ID);

/// Report a window on an argument the syscall does not take, on an argument
/// not declared to hold an address, a length taken from an address argument,
/// a duplicated window, or a window whose direction the syscall's effects do
/// not admit.
llvm::Error validateSyscallMemoryTable();

struct SyscallSourceInfo {
  SyscallSource ID;
  llvm::StringLiteral Name;
  llvm::StringLiteral Revision;
};

llvm::ArrayRef<SyscallSourceInfo> syscallSourceInfos();
llvm::StringRef syscallSourceName(SyscallSource Source);
llvm::StringRef syscallSourceRevision(SyscallSource Source);

uint32_t hashSymbolName(llvm::StringRef Name);
llvm::ArrayRef<SyscallInfo> syscallInfos();
const SyscallInfo *getSyscallInfo(uint32_t Hash);
const SyscallInfo *getSyscallInfo(Syscall ID);
const SyscallInfo *findSyscallByName(llvm::StringRef Name);

} // namespace neverd::sbf

#endif // NEVERD_SBF_RUNTIME_SBFSYSCALLS_H
