//===- NativeTranslationSessionInternal.h - Session internals -*- C++ -*-===//

#ifndef NEVERD_LIB_TRANSLATE_NATIVETRANSLATIONSESSIONINTERNAL_H
#define NEVERD_LIB_TRANSLATE_NATIVETRANSLATIONSESSIONINTERNAL_H

#include "neverd/translate/RuntimeGuestState.h"
#include "neverd/translate/TranslationObjectRequest.h"

#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/Support/Error.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace neverd::translate {

class GuestMemoryRuntime;

namespace detail {

/// Shared state for the run/cancellation linearization boundary.  A successful
/// run commits through finalizeNativeTranslationRunV1(); failures abandon the
/// run through abandonNativeTranslationRunV1().
struct NativeTranslationRunControlV1 {
  std::mutex Mutex;
  bool Running = false;
  bool CancellationPending = false;
  std::shared_ptr<GuestMemoryRuntime> ActiveRuntime;
};

/// End a failed run without publishing any candidate state.
void abandonNativeTranslationRunV1(NativeTranslationRunControlV1 &Control);

/// Linearize a successful result against
/// requestNativeTranslationCancellationV1. Commit executes while Control.Mutex
/// is held.  A cancellation accepted before this function acquires the mutex is
/// reported to Commit; once Running becomes false, later cancellation requests
/// are ignored.
llvm::Error finalizeNativeTranslationRunV1(
    NativeTranslationRunControlV1 &Control,
    llvm::function_ref<llvm::Error(bool CancellationWins)> Commit);

/// Request cancellation if and only if a run is active.  Setting
/// CancellationPending while holding Control.Mutex is the request's
/// linearization point.
void requestNativeTranslationCancellationV1(
    NativeTranslationRunControlV1 &Control);

/// Information retained from one failed object-translation request.  SoleCode
/// is populated only when the complete llvm::Error contains exactly one typed
/// request error and no unhandled error payloads.
struct NativeTranslationObjectFailureV1 {
  std::optional<TranslationObjectRequestErrorCode> SoleCode;
  std::optional<X86TranslationBlockBuilderErrorCode> BuilderCode;
  std::optional<uint64_t> BuilderGuestPC;
  std::optional<GuestMemoryFault> BuilderMemoryFaultDetails;
  std::optional<TranslationBlockLoweringErrorCode> LoweringCode;
  std::optional<uint64_t> LoweringGuestPC;
  std::optional<uint64_t> CompilerBudgetObserved;
  std::optional<uint64_t> CompilerBudgetLimit;
  std::optional<uint64_t> GuestInstructionCount;
  std::string Detail;
};

NativeTranslationObjectFailureV1
classifyNativeTranslationObjectFailureV1(llvm::Error Error);

/// Guest-visible capability gap retained from a single typed object failure.
/// Code identifies the request stage and Subcode identifies the nested stage
/// error.  Builder-origin failures retain their existing Code encoding with a
/// zero Subcode for schema-v1 compatibility.
struct NativeTranslationUnsupportedInstructionV1 {
  uint64_t GuestPC = 0;
  uint64_t Code = 0;
  uint64_t Subcode = 0;
};

/// Publish only errors that prove a guest instruction is outside the current
/// translation capability.  Malformed descriptors, lowerer drift, IR
/// verification, and other infrastructure failures remain unclassified.
std::optional<NativeTranslationUnsupportedInstructionV1>
classifyNativeTranslationUnsupportedInstructionV1(
    const NativeTranslationObjectFailureV1 &Failure);

/// Validate a translated block's erased status against its trusted manifest
/// and the state committed by generated code.
llvm::Expected<BlockExitV1>
translateNativeBlockExitV1(uint32_t Status,
                           const TranslationBlockDescriptorV1 &Descriptor,
                           const RuntimeGuestStateX86_64V1 &RuntimeState);

/// Form a validated cancellation result whose run identity remains StartPC
/// while its resumable exit identifies the last committed CurrentPC.
llvm::Expected<TranslationResult> makeNativeTranslationCancelledResultV1(
    const TranslationOptions &Options, uint64_t StartPC, uint64_t CurrentPC,
    GuestMemoryRuntime &Memory, uint64_t GuestInstructions,
    uint64_t BlocksTranslated, uint64_t GeneratedCodeBytes);

} // namespace detail
} // namespace neverd::translate

#endif // NEVERD_LIB_TRANSLATE_NATIVETRANSLATIONSESSIONINTERNAL_H
