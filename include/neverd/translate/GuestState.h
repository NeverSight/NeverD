//===- GuestState.h - Architecture-neutral guest machine state -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Defines the logical machine-visible state shared by translation control,
/// replay, debugging, and exception delivery.  Neither this vector/APInt model
/// nor its in-memory C++ layout is a generated-code ABI.  A JIT backend must
/// lower it into a private, size-and-version-checked fixed-layout runtime
/// state. serializeGuestState() emits a separate canonical wire representation
/// with fixed-width little-endian fields.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_TRANSLATE_GUESTSTATE_H
#define NEVERD_TRANSLATE_GUESTSTATE_H

#include "llvm/ADT/APInt.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace neverd::translate {

/// Stable architecture identities used by translation state and its wire
/// format.  Append new values; never renumber existing values.
enum class GuestArchitecture : uint8_t {
  X86_64 = 1,
  AArch64 = 2,
  ARM32 = 3,
  X86_32 = 4,
};

static_assert(static_cast<uint8_t>(GuestArchitecture::X86_64) == 1 &&
              static_cast<uint8_t>(GuestArchitecture::AArch64) == 2 &&
              static_cast<uint8_t>(GuestArchitecture::ARM32) == 3 &&
              static_cast<uint8_t>(GuestArchitecture::X86_32) == 4);

enum class GuestEndianness : uint8_t {
  Little = 1,
  Big = 2,
};

enum class GuestExecutionMode : uint8_t {
  Default = 0,
  ARM = 1,
  Thumb = 2,
};

using RegisterID = uint32_t;

/// IDs at or above this boundary are caller-defined extension registers.
/// Extension records carry a stable name and exact bit width in the state so a
/// newer producer never aliases an older architecture register.
inline constexpr RegisterID kFirstExtensionRegisterID = 0x80000000u;

enum class RegisterKind : uint8_t {
  General = 1,
  ProgramCounter = 2,
  StackPointer = 3,
  Link = 4,
  Flags = 5,
  FloatingPointControl = 6,
  Vector = 7,
  System = 8,
};

struct RegisterDescription {
  RegisterID ID;
  llvm::StringLiteral Name;
  uint32_t BitWidth;
  RegisterKind Kind;
};

struct ArchitectureDescription {
  GuestArchitecture Architecture;
  llvm::StringLiteral Name;
  GuestEndianness ByteOrder;
  uint16_t AddressWidth;
  GuestExecutionMode DefaultMode;
  RegisterID ProgramCounter;
  RegisterID StackPointer;
  llvm::ArrayRef<GuestExecutionMode> ExecutionModes;
  llvm::ArrayRef<RegisterDescription> Registers;
};

const ArchitectureDescription *
getArchitectureDescription(GuestArchitecture Architecture);

const RegisterDescription *findRegister(const ArchitectureDescription &Arch,
                                        RegisterID ID);
const RegisterDescription *findRegister(const ArchitectureDescription &Arch,
                                        llvm::StringRef Name);

struct GuestRegisterValue {
  RegisterID ID = 0;
  /// Empty for architecture-defined registers and required for extensions.
  std::string ExtensionName;
  llvm::APInt Value = llvm::APInt(1, 0);
};

enum class MemoryPermission : uint8_t {
  None = 0,
  Read = 1u << 0,
  Write = 1u << 1,
  Execute = 1u << 2,
};

constexpr MemoryPermission operator|(MemoryPermission Left,
                                     MemoryPermission Right) {
  return static_cast<MemoryPermission>(static_cast<uint8_t>(Left) |
                                       static_cast<uint8_t>(Right));
}

constexpr MemoryPermission operator&(MemoryPermission Left,
                                     MemoryPermission Right) {
  return static_cast<MemoryPermission>(static_cast<uint8_t>(Left) &
                                       static_cast<uint8_t>(Right));
}

constexpr bool hasPermission(MemoryPermission Set, MemoryPermission Value) {
  return (Set & Value) != MemoryPermission::None;
}

/// A byte-exact guest memory region.  Bytes retain guest memory order; no host
/// integer loads or native struct layout participate in the wire contract.
struct GuestMemoryRegion {
  uint64_t Address = 0;
  MemoryPermission Permissions = MemoryPermission::None;
  /// Incremented by the runtime when executable contents change.
  uint64_t Generation = 0;
  std::vector<uint8_t> Bytes;
};

enum class GuestExceptionKind : uint8_t {
  HardwareFault = 1,
  Software = 2,
  Signal = 3,
};

struct GuestExceptionState {
  GuestExceptionKind Kind = GuestExceptionKind::HardwareFault;
  uint64_t Code = 0;
  uint64_t FaultAddress = 0;
  uint64_t InstructionAddress = 0;
  std::vector<uint8_t> Payload;
};

struct GuestState {
  GuestArchitecture Architecture = GuestArchitecture::X86_64;
  GuestEndianness ByteOrder = GuestEndianness::Little;
  /// Authoritative decode mode.  For ARM32 the CPSR.T bit must mirror this
  /// value, and the saved PC is the aligned instruction address rather than an
  /// interworking-tagged code pointer.
  GuestExecutionMode ExecutionMode = GuestExecutionMode::Default;
  uint16_t AddressWidth = 64;
  uint64_t ThreadID = 0;
  /// Lower-case guest ISA feature identities.  Persisting a feature does not
  /// claim executable-engine support; capability validation is independent.
  std::vector<std::string> Features;
  std::vector<GuestRegisterValue> Registers;
  std::vector<GuestMemoryRegion> Memory;
  std::optional<GuestExceptionState> Exception;
};

/// Build the zero-initialized baseline register subset described for this
/// v1 schema.  That baseline is immutable so every v1 encoding remains
/// decodable; additional modeled state must use a lower-case named extension
/// register or a new schema version with an explicit upgrader.  Memory remains
/// empty until added explicitly.
llvm::Expected<GuestState>
createZeroedGuestState(GuestArchitecture Architecture, uint64_t ThreadID = 0);

const GuestRegisterValue *findRegisterValue(const GuestState &State,
                                            RegisterID ID);
GuestRegisterValue *findRegisterValue(GuestState &State, RegisterID ID);

/// Insert or replace a register while enforcing its architecture-defined width.
/// Extension IDs require a non-empty canonical lower-case ASCII name.
llvm::Error setRegisterValue(GuestState &State, RegisterID ID,
                             const llvm::APInt &Value,
                             llvm::StringRef ExtensionName = {});

/// Validate the modeled logical state before persistence or conversion into a
/// backend-private runtime ABI.
llvm::Error validateGuestState(const GuestState &State);

/// Version of the canonical GuestState wire format.  Version 1 is:
///
///   NVDSTATE | u32 version | u32 header-size | u8 architecture |
///   u8 byte-order | u8 execution-mode | u8 has-exception |
///   u16 address-width | zero[2] | u64 thread-id | u64 feature-count |
///   u64 register-count | u64 memory-count | zero[8]
///
/// Feature records contain a u64 byte length and canonical lower-case ASCII
/// bytes.  Register records
/// contain u32 ID/width, u64 name/value lengths, then name and
/// least-significant-byte-first value bytes.  Memory records contain u64
/// address/generation/size, u8 permissions, zero[7], then raw bytes.  An
/// exception contains u8 kind, zero[7], three u64 code/address fields, a u64
/// payload size, then raw payload.  Every integer field is little-endian.
/// Features, registers, and memory regions are sorted by name, ID, and address,
/// respectively, so one logical state has one encoding.  Unknown versions,
/// header sizes, modes, non-zero reserved bytes, padding bits,
/// duplicate/overlapping records, and trailing data are rejected.
inline constexpr uint32_t kGuestStateWireVersion = 1;

/// Optional caller-owned resource policy for untrusted wire input.  Every zero
/// field is unbounded: the library never substitutes a hidden finite limit.
/// Structural validation remains mandatory regardless of these budgets.
struct GuestStateWireLimits {
  uint64_t MaxWireBytes = 0;
  uint64_t MaxFeatures = 0;
  uint64_t MaxRegisters = 0;
  uint64_t MaxMemoryRegions = 0;
  uint64_t MaxRegisterBits = 0;
  uint64_t MaxGuestMemoryBytes = 0;
  uint64_t MaxExceptionPayloadBytes = 0;
};

llvm::Expected<std::vector<uint8_t>>
serializeGuestState(const GuestState &State);
llvm::Expected<GuestState>
deserializeGuestState(llvm::ArrayRef<uint8_t> Bytes,
                      const GuestStateWireLimits &Limits = {});

} // namespace neverd::translate

#endif // NEVERD_TRANSLATE_GUESTSTATE_H
