//===- NeverDCAPITranslate.cpp - Cross-architecture object C API --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/sdk/NeverDCAPITranslate.h"

#include "neverd/translate/GuestState.h"
#include "neverd/translate/TranslationObjectRequest.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/raw_ostream.h"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <limits>
#include <new>
#include <string>
#include <utility>

using namespace neverd::translate;

namespace {

#define FIELD_END(Type, Field)                                                 \
  (offsetof(Type, Field) + sizeof(static_cast<Type *>(nullptr)->Field))

bool reaches(size_t Size, size_t End) { return Size >= End; }

static_assert(static_cast<uint8_t>(
                  TranslationObjectRequestErrorCode::InvalidRequest) == 0);
static_assert(static_cast<uint8_t>(
                  TranslationObjectRequestErrorCode::GuestStateRejected) == 1);
static_assert(static_cast<uint8_t>(
                  TranslationObjectRequestErrorCode::RuntimeCreationFailed) ==
              2);
static_assert(
    static_cast<uint8_t>(
        TranslationObjectRequestErrorCode::TargetMachineCreationFailed) == 3);
static_assert(
    static_cast<uint8_t>(
        TranslationObjectRequestErrorCode::BlockBuilderCreationFailed) == 4);
static_assert(static_cast<uint8_t>(
                  TranslationObjectRequestErrorCode::BlockConstructionFailed) ==
              5);
static_assert(
    static_cast<uint8_t>(
        TranslationObjectRequestErrorCode::InstructionBudgetExceeded) == 6);
static_assert(static_cast<uint8_t>(
                  TranslationObjectRequestErrorCode::BlockLoweringFailed) == 7);
static_assert(static_cast<uint8_t>(
                  TranslationObjectRequestErrorCode::ObjectCompilationFailed) ==
              8);
static_assert(
    static_cast<uint8_t>(
        TranslationObjectRequestErrorCode::GeneratedCodeBudgetExceeded) == 9);
static_assert(
    static_cast<uint8_t>(
        TranslationObjectRequestErrorCode::ArtifactVerificationFailed) == 10);

static_assert(static_cast<uint8_t>(TranslationSemanticStopV1::NotRun) ==
              NEVERD_TRANSLATE_SEMANTIC_NOT_RUN);
static_assert(static_cast<uint8_t>(TranslationSemanticStopV1::Stable) ==
              NEVERD_TRANSLATE_SEMANTIC_STABLE);
static_assert(static_cast<uint8_t>(TranslationSemanticStopV1::CycleDetected) ==
              NEVERD_TRANSLATE_SEMANTIC_CYCLE_DETECTED);
static_assert(
    static_cast<uint8_t>(TranslationSemanticStopV1::RoundBudgetExhausted) ==
    NEVERD_TRANSLATE_SEMANTIC_ROUND_BUDGET_EXHAUSTED);
static_assert(static_cast<uint8_t>(neverd::solver::ProofStatus::NotRun) ==
              NEVERD_TRANSLATE_PROOF_NOT_RUN);
static_assert(static_cast<uint8_t>(neverd::solver::ProofStatus::Equivalent) ==
              NEVERD_TRANSLATE_PROOF_EQUIVALENT);
static_assert(static_cast<uint8_t>(neverd::solver::ProofStatus::Different) ==
              NEVERD_TRANSLATE_PROOF_DIFFERENT);
static_assert(static_cast<uint8_t>(neverd::solver::ProofStatus::Unknown) ==
              NEVERD_TRANSLATE_PROOF_UNKNOWN);
static_assert(static_cast<uint8_t>(neverd::solver::ProofStatus::Invalid) ==
              NEVERD_TRANSLATE_PROOF_INVALID);

void clearResult(neverd_translate_object_result_v1 *Result) {
  const size_t Size = Result->struct_size;
#define CLEAR(Field)                                                           \
  do {                                                                         \
    if (reaches(Size, FIELD_END(neverd_translate_object_result_v1, Field)))    \
      Result->Field = {};                                                      \
  } while (false)

  CLEAR(ok);
  CLEAR(error_code);
  CLEAR(error_message);
  CLEAR(object_bytes);
  CLEAR(object_size);
  CLEAR(object_format);
  CLEAR(guest_entry_pc);
  CLEAR(guest_instruction_count);
  CLEAR(guest_byte_count);
  CLEAR(executable_generation);
  CLEAR(block_ir_symbol);
  CLEAR(block_object_symbol);
  CLEAR(host_triple);
  CLEAR(host_cpu);
  CLEAR(host_target_identity);
  CLEAR(runtime_registry_identity);
  CLEAR(request_cache_key);
  CLEAR(artifact_cache_key);
  CLEAR(translation_cache_identity);
  CLEAR(semantic_changed);
  CLEAR(semantic_rewrites);
  CLEAR(semantic_search_work);
  CLEAR(semantic_proof_queries);
  CLEAR(semantic_proof_conflicts);
  CLEAR(semantic_proof_propagations);
  CLEAR(semantic_proof_watch_visits);
  CLEAR(semantic_function_pass_invocations);
  CLEAR(semantic_max_rounds);
  CLEAR(semantic_stop);
  CLEAR(semantic_proof);
  CLEAR(llvm_optimization_pipeline_ran);
  CLEAR(object_cache_identity_version);
  CLEAR(object_pipeline_schema_version);
#undef CLEAR
}

char *copyString(llvm::StringRef Text) noexcept {
  if (Text.size() == std::numeric_limits<size_t>::max())
    return nullptr;
  char *Copy = static_cast<char *>(std::malloc(Text.size() + 1));
  if (!Copy)
    return nullptr;
  if (!Text.empty())
    std::memcpy(Copy, Text.data(), Text.size());
  Copy[Text.size()] = '\0';
  return Copy;
}

unsigned char *copyBytes(llvm::ArrayRef<uint8_t> Bytes) noexcept {
  if (Bytes.empty())
    return nullptr;
  auto *Copy = static_cast<unsigned char *>(std::malloc(Bytes.size()));
  if (!Copy)
    return nullptr;
  std::memcpy(Copy, Bytes.data(), Bytes.size());
  return Copy;
}

void writeFailure(neverd_translate_object_result_v1 *Result,
                  neverd_translate_error_code_t Code,
                  llvm::StringRef Detail = {}) noexcept {
  if (reaches(Result->struct_size,
              FIELD_END(neverd_translate_object_result_v1, ok)))
    Result->ok = 0;
  if (reaches(Result->struct_size,
              FIELD_END(neverd_translate_object_result_v1, error_code)))
    Result->error_code = Code;
  if (Code == NEVERD_TRANSLATE_ERROR_ALLOCATION_FAILURE || Detail.empty() ||
      !reaches(Result->struct_size,
               FIELD_END(neverd_translate_object_result_v1, error_message)))
    return;

  Result->error_message = copyString(Detail);
  if (!Result->error_message &&
      reaches(Result->struct_size,
              FIELD_END(neverd_translate_object_result_v1, error_code)))
    Result->error_code = NEVERD_TRANSLATE_ERROR_ALLOCATION_FAILURE;
}

neverd_translate_error_code_t
mapRequestError(TranslationObjectRequestErrorCode Code) {
  switch (Code) {
  case TranslationObjectRequestErrorCode::InvalidRequest:
    return NEVERD_TRANSLATE_ERROR_INVALID_REQUEST;
  case TranslationObjectRequestErrorCode::GuestStateRejected:
    return NEVERD_TRANSLATE_ERROR_GUEST_STATE_REJECTED;
  case TranslationObjectRequestErrorCode::RuntimeCreationFailed:
    return NEVERD_TRANSLATE_ERROR_RUNTIME_CREATION_FAILED;
  case TranslationObjectRequestErrorCode::TargetMachineCreationFailed:
    return NEVERD_TRANSLATE_ERROR_TARGET_MACHINE_CREATION_FAILED;
  case TranslationObjectRequestErrorCode::BlockBuilderCreationFailed:
    return NEVERD_TRANSLATE_ERROR_BLOCK_BUILDER_CREATION_FAILED;
  case TranslationObjectRequestErrorCode::BlockConstructionFailed:
    return NEVERD_TRANSLATE_ERROR_BLOCK_CONSTRUCTION_FAILED;
  case TranslationObjectRequestErrorCode::InstructionBudgetExceeded:
    return NEVERD_TRANSLATE_ERROR_INSTRUCTION_BUDGET_EXCEEDED;
  case TranslationObjectRequestErrorCode::BlockLoweringFailed:
    return NEVERD_TRANSLATE_ERROR_BLOCK_LOWERING_FAILED;
  case TranslationObjectRequestErrorCode::ObjectCompilationFailed:
    return NEVERD_TRANSLATE_ERROR_OBJECT_COMPILATION_FAILED;
  case TranslationObjectRequestErrorCode::GeneratedCodeBudgetExceeded:
    return NEVERD_TRANSLATE_ERROR_GENERATED_CODE_BUDGET_EXCEEDED;
  case TranslationObjectRequestErrorCode::ArtifactVerificationFailed:
    return NEVERD_TRANSLATE_ERROR_ARTIFACT_VERIFICATION_FAILED;
  }
  return NEVERD_TRANSLATE_ERROR_INTERNAL_FAILURE;
}

struct MappedRequestError {
  neverd_translate_error_code_t Code = NEVERD_TRANSLATE_ERROR_INTERNAL_FAILURE;
  std::string Detail;
};

MappedRequestError mapRequestError(llvm::Error Error) {
  MappedRequestError Mapped;
  size_t RequestErrorCount = 0;
  llvm::Error Unhandled = llvm::handleErrors(
      std::move(Error), [&](const TranslationObjectRequestError &Typed) {
        ++RequestErrorCount;
        if (RequestErrorCount == 1)
          Mapped.Code = mapRequestError(Typed.code());
        llvm::raw_string_ostream Stream(Mapped.Detail);
        if (RequestErrorCount != 1)
          Stream << "; ";
        Typed.log(Stream);
      });
  if (Unhandled) {
    Mapped.Code = NEVERD_TRANSLATE_ERROR_INTERNAL_FAILURE;
    Mapped.Detail = llvm::toString(std::move(Unhandled));
  } else if (RequestErrorCount == 0) {
    Mapped.Code = NEVERD_TRANSLATE_ERROR_INTERNAL_FAILURE;
    Mapped.Detail = "translation failed without a typed request error";
  } else if (RequestErrorCount != 1) {
    Mapped.Code = NEVERD_TRANSLATE_ERROR_INTERNAL_FAILURE;
    Mapped.Detail = "translation failed with multiple typed request errors: " +
                    Mapped.Detail;
  }
  return Mapped;
}

TranslationOptions fixedOptions(neverd_translate_object_format_t ObjectFormat) {
  TranslationOptions Options;
  Options.Guest = GuestArchitecture::X86_64;
  Options.Mode = TranslationMode::AOT;
  Options.Target.Kind = HostTargetKind::Explicit;
  Options.Target.Architecture = GuestArchitecture::AArch64;
  Options.Target.Triple = ObjectFormat == NEVERD_TRANSLATE_OBJECT_FORMAT_ELF
                              ? "aarch64-unknown-linux-gnu"
                              : "aarch64-apple-macosx";
  Options.UnsupportedInstructions = UnsupportedInstructionPolicy::Fail;
  Options.Optimization = TranslationOptimizationPolicy::ProvenSemanticAndLLVM;
  Options.LLVMLevel = LLVMOptimizationLevel::O2;
  Options.BlockCache = BlockCachePolicy::Disabled;
  Options.CodeInvalidation = CodeInvalidationPolicy::RejectExecutableWrites;
  Options.DeterministicReplay = DeterministicReplayPolicy::Disabled;
  Options.VerifyGeneratedIR = true;
  Options.PreserveExceptionState = true;
  Options.RequiredCapabilities = TranslationCapability::ScalarInteger;
  Options.InstructionBudget = 0;
  Options.BlockBudget = 0;
  Options.GeneratedCodeByteBudget = 0;
  return Options;
}

struct OwnedResult {
  neverd_translate_object_result_v1 Value{};

  ~OwnedResult() { release(); }

  OwnedResult(const OwnedResult &) = delete;
  OwnedResult &operator=(const OwnedResult &) = delete;
  OwnedResult() = default;

  bool own(const unsigned char *&Destination,
           llvm::ArrayRef<uint8_t> Bytes) noexcept {
    Destination = copyBytes(Bytes);
    return Destination != nullptr;
  }

  bool own(const char *&Destination, llvm::StringRef Text) noexcept {
    Destination = copyString(Text);
    return Destination != nullptr;
  }

  void release() noexcept {
    std::free(const_cast<char *>(Value.error_message));
    std::free(const_cast<unsigned char *>(Value.object_bytes));
    std::free(const_cast<char *>(Value.block_ir_symbol));
    std::free(const_cast<char *>(Value.block_object_symbol));
    std::free(const_cast<char *>(Value.host_triple));
    std::free(const_cast<char *>(Value.host_cpu));
    std::free(const_cast<char *>(Value.host_target_identity));
    std::free(const_cast<char *>(Value.runtime_registry_identity));
    std::free(const_cast<char *>(Value.request_cache_key));
    std::free(const_cast<char *>(Value.artifact_cache_key));
    std::free(const_cast<char *>(Value.translation_cache_identity));
    Value.error_message = nullptr;
    Value.object_bytes = nullptr;
    Value.block_ir_symbol = nullptr;
    Value.block_object_symbol = nullptr;
    Value.host_triple = nullptr;
    Value.host_cpu = nullptr;
    Value.host_target_identity = nullptr;
    Value.runtime_registry_identity = nullptr;
    Value.request_cache_key = nullptr;
    Value.artifact_cache_key = nullptr;
    Value.translation_cache_identity = nullptr;
  }
};

void commitResult(OwnedResult &From,
                  neverd_translate_object_result_v1 *To) noexcept {
  const size_t Size = To->struct_size;
#define COPY(Field)                                                            \
  do {                                                                         \
    if (reaches(Size, FIELD_END(neverd_translate_object_result_v1, Field)))    \
      To->Field = From.Value.Field;                                            \
  } while (false)
#define MOVE_POINTER(Field)                                                    \
  do {                                                                         \
    if (reaches(Size, FIELD_END(neverd_translate_object_result_v1, Field))) {  \
      To->Field = From.Value.Field;                                            \
      From.Value.Field = nullptr;                                              \
    }                                                                          \
  } while (false)

  COPY(ok);
  COPY(error_code);
  MOVE_POINTER(error_message);
  MOVE_POINTER(object_bytes);
  COPY(object_size);
  COPY(object_format);
  COPY(guest_entry_pc);
  COPY(guest_instruction_count);
  COPY(guest_byte_count);
  COPY(executable_generation);
  MOVE_POINTER(block_ir_symbol);
  MOVE_POINTER(block_object_symbol);
  MOVE_POINTER(host_triple);
  MOVE_POINTER(host_cpu);
  MOVE_POINTER(host_target_identity);
  MOVE_POINTER(runtime_registry_identity);
  MOVE_POINTER(request_cache_key);
  MOVE_POINTER(artifact_cache_key);
  MOVE_POINTER(translation_cache_identity);
  COPY(semantic_changed);
  COPY(semantic_rewrites);
  COPY(semantic_search_work);
  COPY(semantic_proof_queries);
  COPY(semantic_proof_conflicts);
  COPY(semantic_proof_propagations);
  COPY(semantic_proof_watch_visits);
  COPY(semantic_function_pass_invocations);
  COPY(semantic_max_rounds);
  COPY(semantic_stop);
  COPY(semantic_proof);
  COPY(llvm_optimization_pipeline_ran);
  COPY(object_cache_identity_version);
  COPY(object_pipeline_schema_version);
#undef MOVE_POINTER
#undef COPY
}

bool populateResult(const TranslationObjectResultV1 &Translation,
                    const neverd_translate_object_request_v1 &Request,
                    OwnedResult &Output) {
  const TranslationBlockDescriptorV1 &Descriptor = Translation.descriptor();
  const TranslationObjectArtifactV1 &Artifact = Translation.artifact();
  if (Artifact.bytes().empty() || Artifact.blockSymbols().size() != 1)
    return false;

  Output.Value.ok = 1;
  Output.Value.error_code = NEVERD_TRANSLATE_ERROR_NONE;
  Output.Value.object_size = Artifact.bytes().size();
  Output.Value.object_format = Request.object_format;
  Output.Value.guest_entry_pc = Descriptor.Header.EntryPC;
  Output.Value.guest_instruction_count =
      Descriptor.Header.GuestInstructionCount;
  Output.Value.guest_byte_count = Descriptor.Header.GuestByteCount;
  Output.Value.executable_generation = Request.executable_generation;

  const TranslationSemanticReportV1 &Report = Artifact.semanticReport();
  Output.Value.semantic_changed = Report.Changed ? 1 : 0;
  Output.Value.semantic_rewrites = Report.Rewrites;
  Output.Value.semantic_search_work = Report.SearchWork;
  Output.Value.semantic_proof_queries = Report.ProofWork.Queries;
  Output.Value.semantic_proof_conflicts = Report.ProofWork.Conflicts;
  Output.Value.semantic_proof_propagations = Report.ProofWork.Propagations;
  Output.Value.semantic_proof_watch_visits = Report.ProofWork.WatchVisits;
  Output.Value.semantic_function_pass_invocations =
      Report.FunctionPassInvocations;
  Output.Value.semantic_max_rounds = Report.MaxRounds;
  Output.Value.semantic_stop =
      static_cast<neverd_translate_semantic_stop_t>(Report.Stop);
  Output.Value.semantic_proof =
      static_cast<neverd_translate_proof_status_t>(Report.Proof);
  Output.Value.llvm_optimization_pipeline_ran =
      Artifact.llvmOptimizationPipelineRan() ? 1 : 0;
  Output.Value.object_cache_identity_version =
      TranslationObjectArtifactV1::CacheIdentityVersion;
  Output.Value.object_pipeline_schema_version =
      TranslationObjectArtifactV1::PipelineSchemaVersion;

  const TranslationObjectSymbolV1 &Block = Artifact.blockSymbols().front();
  return Output.own(Output.Value.object_bytes, Artifact.bytes()) &&
         Output.own(Output.Value.block_ir_symbol, Block.IRName) &&
         Output.own(Output.Value.block_object_symbol, Block.ObjectName) &&
         Output.own(Output.Value.host_triple, Artifact.hostTarget().triple()) &&
         Output.own(Output.Value.host_cpu, Artifact.hostTarget().cpu()) &&
         Output.own(Output.Value.host_target_identity,
                    Artifact.hostTarget().cacheKey()) &&
         Output.own(Output.Value.runtime_registry_identity,
                    Artifact.runtimeRegistryIdentity()) &&
         Output.own(Output.Value.request_cache_key,
                    Artifact.requestCacheKey()) &&
         Output.own(Output.Value.artifact_cache_key,
                    Artifact.artifactCacheKey()) &&
         Output.own(Output.Value.translation_cache_identity,
                    Translation.cacheIdentity());
}

bool validRequest(const neverd_translate_object_request_v1 *Request,
                  llvm::StringRef &Reason) {
  if (!Request) {
    Reason = "request is null";
    return false;
  }
  if (!reaches(Request->struct_size,
               FIELD_END(neverd_translate_object_request_v1, reserved))) {
    Reason = "request struct is too short";
    return false;
  }
  if (!Request->guest_bytes || Request->guest_bytes_size == 0) {
    Reason = "guest byte range is empty";
    return false;
  }
  if (Request->guest_bytes_size > std::numeric_limits<uint64_t>::max() ||
      Request->entry_pc >
          std::numeric_limits<uint64_t>::max() - Request->guest_bytes_size) {
    Reason = "guest byte address range wraps";
    return false;
  }
  if (Request->object_format != NEVERD_TRANSLATE_OBJECT_FORMAT_ELF &&
      Request->object_format != NEVERD_TRANSLATE_OBJECT_FORMAT_MACHO) {
    Reason = "unsupported AArch64 object format";
    return false;
  }
  if (Request->reserved != 0) {
    Reason = "request reserved field is non-zero";
    return false;
  }
  return true;
}

int translate(const neverd_translate_object_request_v1 *Request,
              neverd_translate_object_result_v1 *Result) {
  llvm::StringRef InvalidReason;
  if (!validRequest(Request, InvalidReason)) {
    writeFailure(Result, NEVERD_TRANSLATE_ERROR_INVALID_ARGUMENT,
                 InvalidReason);
    return 0;
  }

  llvm::Expected<GuestState> StateOrErr =
      createZeroedGuestState(GuestArchitecture::X86_64);
  if (!StateOrErr) {
    const std::string Detail = llvm::toString(StateOrErr.takeError());
    writeFailure(Result, NEVERD_TRANSLATE_ERROR_INTERNAL_FAILURE, Detail);
    return 0;
  }

  GuestMemoryRegion Region;
  Region.Address = Request->entry_pc;
  Region.Permissions = MemoryPermission::Read | MemoryPermission::Execute;
  Region.Generation = Request->executable_generation;
  Region.Bytes.assign(Request->guest_bytes,
                      Request->guest_bytes + Request->guest_bytes_size);
  StateOrErr->Memory.push_back(std::move(Region));

  TranslationOptions Options = fixedOptions(Request->object_format);
  const TranslationObjectRequestV1 EngineRequest(
      *StateOrErr, Request->entry_pc, std::move(Options),
      TranslationSemanticPolicyV1::unlimited());
  llvm::Expected<TranslationObjectResultV1> TranslationOrErr =
      compileTranslationObjectRequestV1(EngineRequest);
  if (!TranslationOrErr) {
    MappedRequestError Error = mapRequestError(TranslationOrErr.takeError());
    writeFailure(Result, Error.Code, Error.Detail);
    return 0;
  }

  const uint64_t ConsumedBytes =
      TranslationOrErr->descriptor().Header.GuestByteCount;
  if (ConsumedBytes != Request->guest_bytes_size) {
    const std::string Detail =
        "guest byte range must contain exactly one block: translated " +
        std::to_string(ConsumedBytes) + " of " +
        std::to_string(Request->guest_bytes_size) +
        " bytes; trailing bytes are not accepted";
    writeFailure(Result, NEVERD_TRANSLATE_ERROR_INVALID_ARGUMENT, Detail);
    return 0;
  }

  OwnedResult Output;
  if (!populateResult(*TranslationOrErr, *Request, Output)) {
    const neverd_translate_error_code_t Code =
        TranslationOrErr->artifact().bytes().empty() ||
                TranslationOrErr->artifact().blockSymbols().size() != 1
            ? NEVERD_TRANSLATE_ERROR_INTERNAL_FAILURE
            : NEVERD_TRANSLATE_ERROR_ALLOCATION_FAILURE;
    writeFailure(Result, Code,
                 Code == NEVERD_TRANSLATE_ERROR_INTERNAL_FAILURE
                     ? "translation produced an invalid public result shape"
                     : llvm::StringRef());
    return 0;
  }

  commitResult(Output, Result);
  return 0;
}

} // namespace

extern "C" {

const char *
neverd_translate_error_code_name(neverd_translate_error_code_t Code) {
  switch (Code) {
  case NEVERD_TRANSLATE_ERROR_NONE:
    return "none";
  case NEVERD_TRANSLATE_ERROR_INVALID_ARGUMENT:
    return "invalid-argument";
  case NEVERD_TRANSLATE_ERROR_INVALID_REQUEST:
    return "invalid-request";
  case NEVERD_TRANSLATE_ERROR_GUEST_STATE_REJECTED:
    return "guest-state-rejected";
  case NEVERD_TRANSLATE_ERROR_RUNTIME_CREATION_FAILED:
    return "runtime-creation-failed";
  case NEVERD_TRANSLATE_ERROR_TARGET_MACHINE_CREATION_FAILED:
    return "target-machine-creation-failed";
  case NEVERD_TRANSLATE_ERROR_BLOCK_BUILDER_CREATION_FAILED:
    return "block-builder-creation-failed";
  case NEVERD_TRANSLATE_ERROR_BLOCK_CONSTRUCTION_FAILED:
    return "block-construction-failed";
  case NEVERD_TRANSLATE_ERROR_INSTRUCTION_BUDGET_EXCEEDED:
    return "instruction-budget-exceeded";
  case NEVERD_TRANSLATE_ERROR_BLOCK_LOWERING_FAILED:
    return "block-lowering-failed";
  case NEVERD_TRANSLATE_ERROR_OBJECT_COMPILATION_FAILED:
    return "object-compilation-failed";
  case NEVERD_TRANSLATE_ERROR_GENERATED_CODE_BUDGET_EXCEEDED:
    return "generated-code-budget-exceeded";
  case NEVERD_TRANSLATE_ERROR_ARTIFACT_VERIFICATION_FAILED:
    return "artifact-verification-failed";
  case NEVERD_TRANSLATE_ERROR_ALLOCATION_FAILURE:
    return "allocation-failure";
  case NEVERD_TRANSLATE_ERROR_INTERNAL_FAILURE:
    return "internal-failure";
  }
  return "invalid";
}

int neverd_translate_x86_64_block_to_aarch64_object_v1(
    const neverd_translate_object_request_v1 *Request,
    neverd_translate_object_result_v1 *Result) {
  if (!Result ||
      !reaches(Result->struct_size,
               FIELD_END(neverd_translate_object_result_v1, object_size)))
    return 1;

  clearResult(Result);
  try {
    return translate(Request, Result);
  } catch (const std::bad_alloc &) {
    clearResult(Result);
    writeFailure(Result, NEVERD_TRANSLATE_ERROR_ALLOCATION_FAILURE);
    return 0;
  } catch (const std::exception &Error) {
    clearResult(Result);
    writeFailure(Result, NEVERD_TRANSLATE_ERROR_INTERNAL_FAILURE, Error.what());
    return 0;
  } catch (...) {
    clearResult(Result);
    writeFailure(Result, NEVERD_TRANSLATE_ERROR_INTERNAL_FAILURE,
                 "unexpected translation failure");
    return 0;
  }
}

void neverd_translate_object_result_dispose(
    neverd_translate_object_result_v1 *Result) {
  if (!Result)
    return;
  const size_t Size = Result->struct_size;
#define RELEASE(Field)                                                         \
  do {                                                                         \
    if (reaches(Size, FIELD_END(neverd_translate_object_result_v1, Field))) {  \
      std::free(const_cast<void *>(static_cast<const void *>(Result->Field))); \
      Result->Field = nullptr;                                                 \
    }                                                                          \
  } while (false)

  RELEASE(error_message);
  RELEASE(object_bytes);
  RELEASE(block_ir_symbol);
  RELEASE(block_object_symbol);
  RELEASE(host_triple);
  RELEASE(host_cpu);
  RELEASE(host_target_identity);
  RELEASE(runtime_registry_identity);
  RELEASE(request_cache_key);
  RELEASE(artifact_cache_key);
  RELEASE(translation_cache_identity);
#undef RELEASE
  clearResult(Result);
}

} // extern "C"

#undef FIELD_END
