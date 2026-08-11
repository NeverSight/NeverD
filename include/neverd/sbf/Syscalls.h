//===- Syscalls.h - Solana runtime syscall metadata ------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SBF_SYSCALLS_H
#define NEVERD_SBF_SYSCALLS_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"

#include <cstdint>

namespace neverd::sbf {

enum class Syscall : uint8_t {
#define SBF_SYSCALL(ID, NAME, ARGUMENT_COUNT, RETURN_KIND, CATEGORY, EFFECTS,  \
                    AVAILABILITY, SOURCE)                                      \
  ID,
#include "neverd/sbf/SBFSyscalls.def"
  Unknown,
};

enum class SyscallReturnKind : uint8_t {
  Never,
  Void,
  Status,
  Value,
  Boolean,
  Address,
};

enum class SyscallAvailability : uint8_t {
#define SBF_SYSCALL_AVAILABILITY(ID, SPELLING) ID,
#include "neverd/sbf/SBFSyscallAvailability.def"
};

enum class SyscallSource : uint8_t {
#define SBF_UPSTREAM_SOURCE(ID, NAME, REVISION) ID,
#include "neverd/sbf/SBFUpstreamSources.def"
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
  SyscallReturnKind ReturnKind;
  SyscallCategory Category;
  SyscallEffect Effects;
  SyscallAvailability Availability;
  SyscallSource Source;
};

struct SyscallSourceInfo {
  SyscallSource ID;
  llvm::StringLiteral Name;
  llvm::StringLiteral Revision;
};

llvm::StringRef syscallAvailabilityName(SyscallAvailability Availability);
llvm::ArrayRef<SyscallSourceInfo> syscallSourceInfos();
llvm::StringRef syscallSourceName(SyscallSource Source);
llvm::StringRef syscallSourceRevision(SyscallSource Source);

uint32_t hashSymbolName(llvm::StringRef Name);
llvm::ArrayRef<SyscallInfo> syscallInfos();
const SyscallInfo *getSyscallInfo(uint32_t Hash);
const SyscallInfo *getSyscallInfo(Syscall ID);
const SyscallInfo *findSyscallByName(llvm::StringRef Name);

} // namespace neverd::sbf

#endif // NEVERD_SBF_SYSCALLS_H
