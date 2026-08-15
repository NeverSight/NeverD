//===- RuntimeGuestState.h - Fixed translated guest state ABI -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Defines the fixed-layout machine state consumed by translated blocks.  This
/// backend-private ABI is explicitly converted to and from logical GuestState;
/// it is neither GuestState's C++ layout nor its persistent wire format.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_TRANSLATE_RUNTIMEGUESTSTATE_H
#define NEVERD_TRANSLATE_RUNTIMEGUESTSTATE_H

#include "neverd/translate/GuestState.h"
#include "neverd/translate/TranslationIRVerifier.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/Error.h"

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace neverd::translate {

/// Little-endian bytes spell "NV64" when this value is serialized.
inline constexpr uint32_t kRuntimeGuestStateX86_64MagicV1 = 0x3436564e;
inline constexpr uint16_t kRuntimeGuestStateX86_64VersionV1 = 1;
inline constexpr uint16_t kRuntimeGuestStateX86_64SizeV1 = 160;

/// Stable indices into RuntimeGuestStateX86_64V1::GPR.  They intentionally
/// match the architecture-defined x86-64 register IDs in GuestState wire v1.
enum class RuntimeX86_64GPRV1 : uint8_t {
  RAX = 0,
  RCX = 1,
  RDX = 2,
  RBX = 3,
  RSP = 4,
  RBP = 5,
  RSI = 6,
  RDI = 7,
  R8 = 8,
  R9 = 9,
  R10 = 10,
  R11 = 11,
  R12 = 12,
  R13 = 13,
  R14 = 14,
  R15 = 15,
};

inline constexpr size_t kRuntimeX86_64GPRCountV1 = 16;

/// Frequently produced status/control flags are scalarized for host IR.  Every
/// other RFLAGS bit remains bit-exact in RFlagsBase.  The base must have these
/// bits cleared, so there is exactly one representation of a packed value.
inline constexpr uint64_t kRuntimeX86_64SplitRFlagsMaskV1 =
    (uint64_t{1} << 0) | (uint64_t{1} << 2) | (uint64_t{1} << 4) |
    (uint64_t{1} << 6) | (uint64_t{1} << 7) | (uint64_t{1} << 10) |
    (uint64_t{1} << 11);

/// Backend-private x86-64 integer-state ABI version 1.
///
/// Generated code may directly access only fields returned by
/// runtimeGuestStateX86_64MemorySlotsV1().  Header and reserved bytes are owned
/// by the dispatcher.  RFlagsBase is read-only and flag bytes are canonical
/// booleans, preventing scalar lowering from modifying unmodeled
/// control/system flags.
struct alignas(16) RuntimeGuestStateX86_64V1 {
  uint32_t Magic = kRuntimeGuestStateX86_64MagicV1;
  uint16_t Version = kRuntimeGuestStateX86_64VersionV1;
  uint16_t Size = kRuntimeGuestStateX86_64SizeV1;
  uint64_t GPR[kRuntimeX86_64GPRCountV1] = {};
  uint64_t RIP = 0;
  uint64_t RFlagsBase = 0;
  uint8_t CF = 0;
  uint8_t PF = 0;
  uint8_t AF = 0;
  uint8_t ZF = 0;
  uint8_t SF = 0;
  uint8_t DF = 0;
  uint8_t OF = 0;
  uint8_t Reserved0 = 0;
};

static_assert(sizeof(RuntimeGuestStateX86_64V1) ==
              kRuntimeGuestStateX86_64SizeV1);
static_assert(alignof(RuntimeGuestStateX86_64V1) == 16);
static_assert(std::is_standard_layout_v<RuntimeGuestStateX86_64V1>);
static_assert(std::is_trivially_copyable_v<RuntimeGuestStateX86_64V1>);
static_assert(offsetof(RuntimeGuestStateX86_64V1, GPR) == 8);
static_assert(offsetof(RuntimeGuestStateX86_64V1, RIP) == 136);
static_assert(offsetof(RuntimeGuestStateX86_64V1, RFlagsBase) == 144);
static_assert(offsetof(RuntimeGuestStateX86_64V1, CF) == 152);
static_assert(offsetof(RuntimeGuestStateX86_64V1, OF) == 158);
static_assert(offsetof(RuntimeGuestStateX86_64V1, Reserved0) == 159);

/// Exact scalar fields directly accessible by translated IR.  Metadata and
/// reserved bytes are intentionally absent; the opaque flag base is read-only.
llvm::ArrayRef<TranslationIRMemorySlot> runtimeGuestStateX86_64MemorySlotsV1();

/// Validate ABI identity, canonical split flags, and reserved bytes before a
/// translated block or dispatcher consumes this state.
llvm::Error
validateRuntimeGuestStateX86_64V1(const RuntimeGuestStateX86_64V1 &State);

/// Convert the logical x86-64 baseline integer state into its backend ABI.
/// Non-integer logical state remains owned by GuestState.
llvm::Expected<RuntimeGuestStateX86_64V1>
createRuntimeGuestStateX86_64V1(const GuestState &State);

/// Transactionally update the x86-64 GPR, RIP, and RFLAGS records in State.
/// Thread, memory, exception, feature, vector, and control records are
/// preserved.  State is unchanged if either side fails validation.
llvm::Error
applyRuntimeGuestStateX86_64V1(const RuntimeGuestStateX86_64V1 &RuntimeState,
                               GuestState &State);

/// Control-flow reason returned by one translated block.  This domain is
/// independent from runtime-service failures such as memory faults,
/// cancellation, and exhausted budgets.  Values start at 0x100 so the
/// canonical i32 block return remains unambiguous after its C++ type is erased;
/// the lower byte is reserved for runtime-service status values in ABI v1.
inline constexpr uint32_t kBlockExitKindBaseV1 = 0x100;
enum class BlockExitKindV1 : uint32_t {
  Continue = kBlockExitKindBaseV1,
  /// A manifest-authorized direct successor selected by translated code.  The
  /// source terminator may be unconditional or conditional.
  DirectBranch = kBlockExitKindBaseV1 + 1,
  IndirectBranch = kBlockExitKindBaseV1 + 2,
  Call = kBlockExitKindBaseV1 + 3,
  Return = kBlockExitKindBaseV1 + 4,
  Unsupported = kBlockExitKindBaseV1 + 5,
  Syscall = kBlockExitKindBaseV1 + 6,
  Trap = kBlockExitKindBaseV1 + 7,
};

static_assert(static_cast<uint32_t>(BlockExitKindV1::Continue) == 0x100 &&
              static_cast<uint32_t>(BlockExitKindV1::DirectBranch) == 0x101 &&
              static_cast<uint32_t>(BlockExitKindV1::IndirectBranch) == 0x102 &&
              static_cast<uint32_t>(BlockExitKindV1::Call) == 0x103 &&
              static_cast<uint32_t>(BlockExitKindV1::Return) == 0x104 &&
              static_cast<uint32_t>(BlockExitKindV1::Unsupported) == 0x105 &&
              static_cast<uint32_t>(BlockExitKindV1::Syscall) == 0x106 &&
              static_cast<uint32_t>(BlockExitKindV1::Trap) == 0x107);

/// Little-endian bytes spell "NVBX" when this value is serialized.
inline constexpr uint32_t kBlockExitMagicV1 = 0x5842564e;
inline constexpr uint16_t kBlockExitVersionV1 = 1;
inline constexpr uint16_t kBlockExitSizeV1 = 48;

/// Fixed dispatcher-facing record for one translated-block exit.
///
/// PC identifies the instruction producing the boundary.  NextPC is the
/// sequential continuation (fallthrough, call return, unsupported fallback,
/// syscall resume, or trap resume), and TargetPC is a resolved
/// branch/call/return destination.  Detail is zero for ordinary control flow;
/// Unsupported, Syscall, and Trap assign it a backend-stable operation,
/// syscall, or trap code respectively.
struct alignas(8) BlockExitV1 {
  uint32_t Magic = kBlockExitMagicV1;
  uint16_t Version = kBlockExitVersionV1;
  uint16_t Size = kBlockExitSizeV1;
  BlockExitKindV1 Kind = BlockExitKindV1::Continue;
  uint32_t Reserved0 = 0;
  uint64_t PC = 0;
  uint64_t NextPC = 0;
  uint64_t TargetPC = 0;
  uint64_t Detail = 0;
};

static_assert(sizeof(BlockExitV1) == kBlockExitSizeV1);
static_assert(alignof(BlockExitV1) == 8);
static_assert(std::is_standard_layout_v<BlockExitV1>);
static_assert(std::is_trivially_copyable_v<BlockExitV1>);
static_assert(offsetof(BlockExitV1, Kind) == 8);
static_assert(offsetof(BlockExitV1, PC) == 16);
static_assert(offsetof(BlockExitV1, NextPC) == 24);
static_assert(offsetof(BlockExitV1, TargetPC) == 32);
static_assert(offsetof(BlockExitV1, Detail) == 40);

/// Reject unknown ABI identities, reasons, reserved values, and payload fields
/// that contradict the selected control-flow kind.
llvm::Error validateBlockExitV1(const BlockExitV1 &Exit);

} // namespace neverd::translate

#endif // NEVERD_TRANSLATE_RUNTIMEGUESTSTATE_H
