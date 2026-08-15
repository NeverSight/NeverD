//===- RuntimeHelpers.h - Sealed translation helper boundary -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Defines the host-owned call frame and exact native helper registry used by
/// translated code.  Generated IR may address only the leading
/// RuntimeControlBlockV1 bytes; everything after that prefix is trusted host
/// state and remains outside the verifier's runtime extent.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_TRANSLATE_RUNTIMEHELPERS_H
#define NEVERD_TRANSLATE_RUNTIMEHELPERS_H

#include "neverd/translate/RuntimeABI.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace llvm {
class DataLayout;
class Module;
class Triple;
} // namespace llvm

namespace neverd::translate {

class GuestMemoryRuntime;

/// Host-side identity of one published translation.  The dispatcher validates
/// this complete tuple before entering generated code.  A successful match is
/// never represented by a guest-writable boolean in the generated ABI.
struct RuntimeCodeCredentialV1 {
  uint64_t SessionID = 0;
  uint64_t BlockID = 0;
  uint64_t EntryPC = 0;
  uint64_t CacheGeneration = 0;
  uint64_t CodeEpoch = 0;

  friend bool operator==(const RuntimeCodeCredentialV1 &,
                         const RuntimeCodeCredentialV1 &) = default;
};

/// Runtime object passed as the canonical block's second pointer argument.
///
/// Control must remain the first member.  The IR verifier is invoked with
/// kRuntimeControlBlockSizeV1, so translated code cannot address Memory or the
/// credential fields.  The host owns Memory and must keep it alive for every
/// invocation using this frame.
struct alignas(8) RuntimeCallFrameV1 {
  RuntimeControlBlockV1 Control;
  GuestMemoryRuntime *Memory = nullptr;
  RuntimeCodeCredentialV1 Published;
  RuntimeCodeCredentialV1 Validated;
};

static_assert(offsetof(RuntimeCallFrameV1, Control) == 0);
static_assert(std::is_standard_layout_v<RuntimeCallFrameV1>);

/// Create a call frame only after trusted dispatch has validated the complete
/// published credential.  For ValidateBeforeDispatch, CurrentPC and
/// ExpectedGeneration must identify the exact executable owner validated last;
/// other policies require both values to remain zero.  Mismatched identities
/// or an incoherent control snapshot fail closed.
llvm::Expected<RuntimeCallFrameV1> createRuntimeCallFrameV1(
    GuestMemoryRuntime &Memory, const RuntimeCodeCredentialV1 &Published,
    const RuntimeCodeCredentialV1 &Validated, uint64_t CurrentPC = 0,
    uint64_t ExpectedGeneration = 0);

llvm::Error
validateRuntimeCodeCredentialV1(const RuntimeCodeCredentialV1 &Published,
                                const RuntimeCodeCredentialV1 &Validated);

/// Validate the control record after one translated-block invocation.
///
/// Runtime-service statuses must exactly match a complete non-success runtime
/// exit.  A block-exit status requires an empty runtime exit.  Under
/// ValidateBeforeDispatch, a trusted helper may consume the entry-generation
/// proof before the block exits; only the helper-produced zeroed proof shape is
/// accepted in that case, while all ABI identity, reserved, budget, and exit
/// invariants remain enforced.
llvm::Error
validateRuntimeControlBlockAfterInvocationV1(const RuntimeControlBlockV1 &Block,
                                             uint32_t Status);

enum class RuntimeABIHelperClassV1 : uint8_t {
  Load = 1,
  Store = 2,
};

using RuntimeABILoadHelperV1 = uint32_t (*)(
    void *Runtime, uint64_t Address, uint32_t RequiredAlignment) noexcept;
using RuntimeABIStoreHelperV1 =
    uint32_t (*)(void *Runtime, uint64_t Address, uint64_t Value,
                 uint32_t RequiredAlignment) noexcept;

/// One exact native binding.  Exactly one function pointer is non-null and
/// Class selects it.  Consumers must derive LLVM declarations and native
/// addresses from this registry rather than performing ambient symbol lookup.
struct RuntimeABIHelperBindingV1 {
  llvm::StringRef Name;
  RuntimeABIHelperClassV1 Class = RuntimeABIHelperClassV1::Load;
  RuntimeABILoadHelperV1 Load = nullptr;
  RuntimeABIStoreHelperV1 Store = nullptr;
};

llvm::ArrayRef<RuntimeABIHelperBindingV1> runtimeABIHelperBindingsV1();
const RuntimeABIHelperBindingV1 *
findRuntimeABIHelperBindingV1(llvm::StringRef Name);

/// Verify the control protocol layered on top of verifyTranslationIR(). Every
/// native load/store status must branch immediately: the non-zero edge returns
/// that exact status without further effects, while a load's ScalarResult read
/// is confined to its zero-status edge. Direct or stale result-slot reads are
/// rejected.
llvm::Error verifyRuntimeABIHelperProtocolV1(const llvm::Module &Module);

/// Verify a complete v1 runtime module through the generic host-IR boundary
/// and the helper status protocol as one indivisible operation.  StateSlots
/// may describe only the state argument; the sealed runtime slots and helper
/// declarations are supplied by this function.
llvm::Error verifyRuntimeTranslationIRV1(
    const llvm::Module &Module, const llvm::Triple &ExpectedHostTriple,
    const llvm::DataLayout &ExpectedHostDataLayout, uint64_t StateSize,
    llvm::ArrayRef<TranslationIRMemorySlot> StateSlots = {});

} // namespace neverd::translate

extern "C" {

uint32_t nvd_rt_v1_load8_le(void *Runtime, uint64_t Address,
                            uint32_t RequiredAlignment) noexcept;
uint32_t nvd_rt_v1_load16_le(void *Runtime, uint64_t Address,
                             uint32_t RequiredAlignment) noexcept;
uint32_t nvd_rt_v1_load32_le(void *Runtime, uint64_t Address,
                             uint32_t RequiredAlignment) noexcept;
uint32_t nvd_rt_v1_load64_le(void *Runtime, uint64_t Address,
                             uint32_t RequiredAlignment) noexcept;
uint32_t nvd_rt_v1_store8_le(void *Runtime, uint64_t Address, uint64_t Value,
                             uint32_t RequiredAlignment) noexcept;
uint32_t nvd_rt_v1_store16_le(void *Runtime, uint64_t Address, uint64_t Value,
                              uint32_t RequiredAlignment) noexcept;
uint32_t nvd_rt_v1_store32_le(void *Runtime, uint64_t Address, uint64_t Value,
                              uint32_t RequiredAlignment) noexcept;
uint32_t nvd_rt_v1_store64_le(void *Runtime, uint64_t Address, uint64_t Value,
                              uint32_t RequiredAlignment) noexcept;

} // extern "C"

#endif // NEVERD_TRANSLATE_RUNTIMEHELPERS_H
