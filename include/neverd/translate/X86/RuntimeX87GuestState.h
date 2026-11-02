//===- RuntimeX87GuestState.h - Explicit translated x87 state -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Adds a separately selected x87 runtime state without changing the integer
/// state ABI or GuestState's immutable baseline wire schema.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_TRANSLATE_X86_RUNTIMEX87GUESTSTATE_H
#define NEVERD_TRANSLATE_X86_RUNTIMEX87GUESTSTATE_H

#include "neverd/translate/RuntimeGuestState.h"
#include "neverd/translate/X86/X87PortableRuntime.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace neverd::translate {

inline constexpr llvm::StringLiteral kPortableX87GuestExtensionV1 =
    "neverd.x87.portable-v1";
inline constexpr uint32_t kPortableX87GuestExtensionBitsV1 =
    kPortableX87StateSizeV1 * 8;
inline constexpr uint16_t kRuntimeX87LastStepSizeV1 = 48;
inline constexpr uint16_t kRuntimeGuestStateX86_64X87SizeV1 =
    kRuntimeGuestStateX86_64SizeV1 + kPortableX87StateSizeV1 +
    kRuntimeX87LastStepSizeV1;

/// Nonarchitectural diagnostics, reset by trusted dispatch before each block.
/// Completion is independent from cancellation: snapshotControlBlock() may
/// replace a completed store's runtime exit with Cancelled. Generated code
/// cannot directly read or write any field in this record.
struct alignas(8) RuntimeX87LastStepV1 {
  uint32_t Selector = 0;
  PortableX87StepStatusV1 Status = PortableX87StepStatusV1::InvalidState;
  uint64_t PC = 0;
  uint64_t NextPC = 0;
  uint64_t DataAddress = 0;
  uint64_t ScalarResult = 0;
  uint8_t Attempted = 0;
  uint8_t InstructionCompleted = 0;
  uint8_t HasScalarResult = 0;
  uint8_t Reserved[5] = {};
};

/// The integer prefix is byte-for-byte v1. The profile selected by the
/// artifact/registry/invocation binds the larger extent; a pointer to this
/// object must never qualify an integer-only artifact implicitly.
struct alignas(16) RuntimeGuestStateX86_64X87V1 {
  RuntimeGuestStateX86_64V1 Integer;
  PortableX87StateV1 X87;
  RuntimeX87LastStepV1 LastStep;
};

static_assert(sizeof(RuntimeX87LastStepV1) == kRuntimeX87LastStepSizeV1);
static_assert(offsetof(RuntimeX87LastStepV1, PC) == 8);
static_assert(offsetof(RuntimeX87LastStepV1, Attempted) == 40);
static_assert(sizeof(RuntimeGuestStateX86_64X87V1) ==
              kRuntimeGuestStateX86_64X87SizeV1);
static_assert(offsetof(RuntimeGuestStateX86_64X87V1, Integer) == 0);
static_assert(offsetof(RuntimeGuestStateX86_64X87V1, X87) ==
              kRuntimeGuestStateX86_64SizeV1);
static_assert(offsetof(RuntimeGuestStateX86_64X87V1, LastStep) ==
              kRuntimeGuestStateX86_64SizeV1 + kPortableX87StateSizeV1);
static_assert(std::is_standard_layout_v<RuntimeGuestStateX86_64X87V1>);
static_assert(std::is_trivially_copyable_v<RuntimeGuestStateX86_64X87V1>);

llvm::Error validateRuntimeX87LastStepV1(const RuntimeX87LastStepV1 &Step);
llvm::Error
validateRuntimeGuestStateX86_64X87V1(const RuntimeGuestStateX86_64X87V1 &State);

/// An explicit trusted-host import. ID must be caller-selected extension
/// storage, not an existing unrelated register. The exact 528-byte little-
/// endian encoding includes the portable header, provenance and raw image;
/// it never serializes C++ padding or LastStep. Importing bytes is not proof
/// that their environment/payload provenance is true.
llvm::Error setPortableX87GuestStateV1(GuestState &State, RegisterID ID,
                                       const PortableX87StateV1 &X87);

/// Require the named extension, its exact width and its versioned identity.
/// A baseline GuestState never acquires FNINIT state or known payloads here.
llvm::Expected<RuntimeGuestStateX86_64X87V1>
createRuntimeGuestStateX86_64X87V1(const GuestState &State);

/// Transactionally update integer records and the existing named extension.
/// Every unrelated extension, memory region and exception remains intact.
llvm::Error applyRuntimeGuestStateX86_64X87V1(
    const RuntimeGuestStateX86_64X87V1 &RuntimeState, GuestState &State);

} // namespace neverd::translate

#endif // NEVERD_TRANSLATE_X86_RUNTIMEX87GUESTSTATE_H
