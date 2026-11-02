//===- RuntimeX87Helpers.h - Sealed portable translation step -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Defines the terminal x87 helper for an explicitly selected translation
/// profile. It consumes checked guest memory, never generated-code pointers.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_TRANSLATE_X86_RUNTIMEX87HELPERS_H
#define NEVERD_TRANSLATE_X86_RUNTIMEX87HELPERS_H

#include "neverd/translate/RuntimeHelpers.h"
#include "neverd/translate/X86/RuntimeX87GuestState.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"

#include <cstdint>
#include <optional>

namespace neverd::translate {

inline constexpr uint16_t kRuntimeX87HelperProtocolVersionV1 = 1;
/// TrapExit::Code domain for qualified runtime refusals; Subcode is the stable
/// PortableX87StepStatusV1 value. Selector is retained in the diagnostic.
inline constexpr uint64_t kRuntimeX87RefusalTrapCodeV1 =
    (uint64_t{kPortableX87StateMagicV1} << 16) |
    kRuntimeX87HelperProtocolVersionV1;
inline constexpr llvm::StringLiteral kRuntimeX87StepNameV1 =
    "nvd_rt_x87_v1_step";

/// Trusted, canonical last-instruction identity retained by the linked block.
/// A helper-free integer block in the x87 profile has no such contract.
struct RuntimeX87StepContractV1 {
  uint32_t Selector = 0;
  uint64_t PC = 0;
  uint64_t NextPC = 0;
};

/// Instruction-form admission uses the shared portable form inventory.
/// Flags-consuming forms remain outside this initial translation profile.
bool isRuntimeX87InstructionV1(uint32_t Selector);

/// Reconcile the stable portable outcome with the exact-span control record,
/// trusted source instruction and completion PC. Cancellation may replace a
/// runtime exit, but cannot erase completion or authorize a stale scalar.
llvm::Error validateRuntimeX87AfterInvocationV1(
    const RuntimeGuestStateX86_64X87V1 &State, const RuntimeCallFrameV1 &Frame,
    uint32_t Status,
    const std::optional<RuntimeX87StepContractV1> &ExpectedStep);

} // namespace neverd::translate

extern "C" uint32_t nvd_rt_x87_v1_step(void *Runtime, void *State,
                                       uint32_t Selector, uint64_t GuestIP,
                                       uint64_t NextPC,
                                       uint64_t GuestDP) noexcept;

#endif // NEVERD_TRANSLATE_X86_RUNTIMEX87HELPERS_H
