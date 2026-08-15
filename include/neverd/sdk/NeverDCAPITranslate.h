//===- NeverDCAPITranslate.h - Cross-architecture object C API -*- C -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Versioned C bindings for translating the published fail-closed x86-64 v1
/// scalar-register subset into an audited AArch64 relocatable object.  The
/// subset accepts only canonical encodings without legacy prefixes: REX.W
/// full-width GPR MOV, ADD/SUB, and register/immediate AND/OR/XOR forms over
/// supported LowIR shapes; full-width register-only CMP 39/3B and
/// register/immediate CMP 81/7, 83/7, and 3D; and full-width register-only TEST
/// 85 and register/immediate TEST F7/0 and A9. Logical and TEST forms compute
/// architecture-defined flags while preserving AF in the NeverD state model.
/// Canonical C3 RET or C2 iw RET imm16 terminates a return block, and
/// direct-relative EB cb or E9 cd JMP terminates a
/// direct-branch block.  The published lowering schema is 9.  Canonical,
/// legacy-prefix-free traditional Jcc comprises JO/JNO 70/71 or 0F 80/81,
/// JB/JAE 72/73 or 0F 82/83, JE/JNE 74/75 or 0F 84/85, JBE/JA 76/77 or 0F
/// 86/87, JS/JNS 78/79 or 0F 88/89, JP/JNP 7A/7B or 0F 8A/8B, JL/JGE 7C/7D or
/// 0F 8C/8D, and JLE/JG 7E/7F or 0F 8E/8F, using cb short or cd near
/// displacements respectively. JRCXZ/JECXZ/JCXZ and LOOP/LOOPE/LOOPNE remain
/// unpublished and fail closed. Reserved F7 /1, guest-memory operands, partial
/// registers, legacy prefixes, semantically redundant REX extension bits, and
/// any instruction or encoding outside that exact subset fail closed.
///
/// This boundary compiles an object only.  It does not link, load, publish,
/// dispatch, execute, or debug the returned bytes.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SDK_CAPI_TRANSLATE_H
#define NEVERD_SDK_CAPI_TRANSLATE_H

#include "neverd/sdk/NeverDCAPITypes.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef _WIN32
#ifdef NEVERD_EXPORTS
#define NEVERD_API __declspec(dllexport)
#else
#define NEVERD_API __declspec(dllimport)
#endif
#else
#define NEVERD_API __attribute__((visibility("default")))
#endif

/// Object container selected for the fixed AArch64 AOT target.  Values are
/// append-only.  The public storage type is fixed-width; the enum tag exists
/// only to give the constants a distinct source-level namespace.
enum neverd_translate_object_format {
  NEVERD_TRANSLATE_OBJECT_FORMAT_INVALID = 0,
  NEVERD_TRANSLATE_OBJECT_FORMAT_ELF = 1,
  NEVERD_TRANSLATE_OBJECT_FORMAT_MACHO = 2
};
typedef uint32_t neverd_translate_object_format_t;

/// Stable failures reported by the C boundary.  Engine request failures retain
/// their typed category; C argument, allocation, and unexpected implementation
/// failures have distinct categories.  Values are append-only and cross the C
/// boundary through a fixed-width integer type.
enum neverd_translate_error_code {
  NEVERD_TRANSLATE_ERROR_NONE = 0,
  NEVERD_TRANSLATE_ERROR_INVALID_ARGUMENT = 1,
  NEVERD_TRANSLATE_ERROR_INVALID_REQUEST = 2,
  NEVERD_TRANSLATE_ERROR_GUEST_STATE_REJECTED = 3,
  NEVERD_TRANSLATE_ERROR_RUNTIME_CREATION_FAILED = 4,
  NEVERD_TRANSLATE_ERROR_TARGET_MACHINE_CREATION_FAILED = 5,
  NEVERD_TRANSLATE_ERROR_BLOCK_BUILDER_CREATION_FAILED = 6,
  NEVERD_TRANSLATE_ERROR_BLOCK_CONSTRUCTION_FAILED = 7,
  NEVERD_TRANSLATE_ERROR_INSTRUCTION_BUDGET_EXCEEDED = 8,
  NEVERD_TRANSLATE_ERROR_BLOCK_LOWERING_FAILED = 9,
  NEVERD_TRANSLATE_ERROR_OBJECT_COMPILATION_FAILED = 10,
  NEVERD_TRANSLATE_ERROR_GENERATED_CODE_BUDGET_EXCEEDED = 11,
  NEVERD_TRANSLATE_ERROR_ARTIFACT_VERIFICATION_FAILED = 12,
  NEVERD_TRANSLATE_ERROR_ALLOCATION_FAILURE = 13,
  NEVERD_TRANSLATE_ERROR_INTERNAL_FAILURE = 14
};
typedef uint32_t neverd_translate_error_code_t;

/// Why proof-gated semantic simplification stopped.  Values are append-only and
/// use fixed-width public storage.
enum neverd_translate_semantic_stop {
  NEVERD_TRANSLATE_SEMANTIC_NOT_RUN = 0,
  NEVERD_TRANSLATE_SEMANTIC_STABLE = 1,
  NEVERD_TRANSLATE_SEMANTIC_CYCLE_DETECTED = 2,
  NEVERD_TRANSLATE_SEMANTIC_ROUND_BUDGET_EXHAUSTED = 3
};
typedef uint32_t neverd_translate_semantic_stop_t;

/// Most conservative synthesis-proof disposition observed while compiling.
/// Exact derivational rewrites need no solver query and may remain NOT_RUN.
/// Values are append-only and use fixed-width public storage.
enum neverd_translate_proof_status {
  NEVERD_TRANSLATE_PROOF_NOT_RUN = 0,
  NEVERD_TRANSLATE_PROOF_EQUIVALENT = 1,
  NEVERD_TRANSLATE_PROOF_DIFFERENT = 2,
  NEVERD_TRANSLATE_PROOF_UNKNOWN = 3,
  NEVERD_TRANSLATE_PROOF_INVALID = 4
};
typedef uint32_t neverd_translate_proof_status_t;

/// Borrowed synchronous input for one block in the published fail-closed
/// x86-64 v1 scalar-register subset.  Only canonical, legacy-prefix-free REX.W
/// full-width GPR MOV, ADD/SUB, and register/immediate AND/OR/XOR forms;
/// full-width register-only CMP 39/3B and register/immediate CMP 81/7, 83/7,
/// and 3D; and full-width register-only TEST 85 and register/immediate TEST
/// F7/0 and A9 are accepted.
/// Logical and TEST forms compute architecture-defined flags while preserving
/// AF in the NeverD state model. Canonical C3 RET or C2 iw RET imm16 terminates
/// a return block, and
/// direct-relative EB cb or E9 cd JMP terminates a direct-branch block.
/// The published lowering schema is 9. Canonical, legacy-prefix-free
/// traditional Jcc comprises JO/JNO 70/71 or 0F 80/81, JB/JAE 72/73 or 0F
/// 82/83, JE/JNE 74/75 or 0F 84/85, JBE/JA 76/77 or 0F 86/87, JS/JNS 78/79 or
/// 0F 88/89, JP/JNP 7A/7B or 0F 8A/8B, JL/JGE 7C/7D or 0F 8C/8D, and JLE/JG
/// 7E/7F or 0F 8E/8F, using cb short or cd near displacements respectively.
/// JRCXZ/JECXZ/JCXZ and LOOP/LOOPE/LOOPNE remain unpublished and fail closed.
/// Reserved F7 /1, ordinary guest-memory operations, partial-register forms,
/// legacy prefixes, semantically redundant REX extension bits, any instruction
/// or encoding outside that exact subset, all other control flow, and
/// unimplemented LowIR fail before object emission.
///
/// Zero the struct, set `struct_size = sizeof(request)`, and fill every other
/// field.  `guest_bytes` remains owned by the caller and only has to stay alive
/// and unchanged until the function returns.  Every byte must belong to exactly
/// one accepted block, including its terminating RET, direct JMP, or published
/// Jcc branch; trailing bytes are rejected.  The half-open address
/// range
/// `[entry_pc, entry_pc + guest_bytes_size)` must not wrap.
///
/// Version 1 fixes the rest of the request to x86-64 guest, AArch64 AOT host,
/// fail-closed unsupported instructions, LLVM O2, proof-gated semantic plus
/// LLVM optimization, and an unlimited caller semantic resource policy.
typedef struct neverd_translate_object_request_v1 {
  size_t struct_size;
  const unsigned char *guest_bytes;
  size_t guest_bytes_size;
  uint64_t entry_pc;
  uint64_t executable_generation;
  neverd_translate_object_format_t object_format;
  /// Must be zero.  This consumes the v1 layout's trailing alignment bytes so
  /// a future appended field cannot be mistaken for padding present in an old
  /// caller's allocation.
  uint32_t reserved;
} neverd_translate_object_request_v1;

/// Owned result of one translation request.
///
/// Zero the struct and set `struct_size = sizeof(result)` before calling.  All
/// non-null pointer fields, including `object_bytes`, are owned by this result;
/// release them only through neverd_translate_object_result_dispose().
/// `struct_size` makes this layout append-only: a newer library writes only
/// fields that fit in the caller's result allocation.  The mandatory v1 prefix
/// ends at `object_size`; shorter allocations are rejected because they cannot
/// carry a successful translation artifact.  Before reusing a result for a
/// later call, dispose it first; translation does not implicitly release
/// pointers owned by an earlier call.  Disposal preserves `struct_size`, so a
/// disposed result can be passed again without reinitializing that field.
typedef struct neverd_translate_object_result_v1 {
  size_t struct_size;
  int ok;
  neverd_translate_error_code_t error_code;
  const char *error_message;

  const unsigned char *object_bytes;
  size_t object_size;
  neverd_translate_object_format_t object_format;

  uint64_t guest_entry_pc;
  uint64_t guest_instruction_count;
  uint64_t guest_byte_count;
  uint64_t executable_generation;

  const char *block_ir_symbol;
  const char *block_object_symbol;
  const char *host_triple;
  const char *host_cpu;
  const char *host_target_identity;
  const char *runtime_registry_identity;
  const char *request_cache_key;
  const char *artifact_cache_key;
  const char *translation_cache_identity;

  int semantic_changed;
  uint64_t semantic_rewrites;
  uint64_t semantic_search_work;
  uint64_t semantic_proof_queries;
  uint64_t semantic_proof_conflicts;
  uint64_t semantic_proof_propagations;
  uint64_t semantic_proof_watch_visits;
  uint64_t semantic_function_pass_invocations;
  unsigned semantic_max_rounds;
  neverd_translate_semantic_stop_t semantic_stop;
  neverd_translate_proof_status_t semantic_proof;
  int llvm_optimization_pipeline_ran;

  uint32_t object_cache_identity_version;
  uint32_t object_pipeline_schema_version;
} neverd_translate_object_result_v1;

/// Stable, non-owned spelling for an error category.  An out-of-range value
/// returns "invalid".
NEVERD_API const char *
neverd_translate_error_code_name(neverd_translate_error_code_t Code);

/// Translate one block in the published x86-64 v1 scalar-register subset into
/// an audited AArch64 relocatable object.  Only canonical, legacy-prefix-free
/// REX.W full-width GPR MOV, ADD/SUB, and register/immediate AND/OR/XOR forms;
/// full-width register-only CMP 39/3B and register/immediate CMP 81/7, 83/7,
/// and 3D; and full-width register-only TEST 85 and register/immediate TEST
/// F7/0 and A9 are accepted.
/// Logical and TEST forms compute architecture-defined flags while preserving
/// AF in the NeverD state model. Canonical C3 RET or C2 iw RET imm16 terminates
/// a return
/// block, and direct-relative EB cb or E9 cd JMP terminates a direct-branch
/// block. The published lowering schema is 9. Canonical, legacy-prefix-free
/// traditional Jcc comprises JO/JNO 70/71 or 0F 80/81, JB/JAE 72/73 or 0F
/// 82/83, JE/JNE 74/75 or 0F 84/85, JBE/JA 76/77 or 0F 86/87, JS/JNS 78/79 or
/// 0F 88/89, JP/JNP 7A/7B or 0F 8A/8B, JL/JGE 7C/7D or 0F 8C/8D, and JLE/JG
/// 7E/7F or 0F 8E/8F, using cb short or cd near displacements respectively.
/// JRCXZ/JECXZ/JCXZ and LOOP/LOOPE/LOOPNE remain unpublished and fail closed.
/// Reserved F7 /1, guest-memory operands, partial registers, legacy prefixes,
/// semantically redundant REX extension bits, unsupported encodings,
/// instructions outside that exact subset, and unsupported LowIR shapes fail
/// closed.
/// `Request->guest_bytes` must contain exactly that one block.  Bytes after its
/// terminator are rejected as an invalid argument.
///
/// Returns zero whenever `Result` is usable; inspect `Result->ok` and
/// `Result->error_code` for the translation outcome.  Returns non-zero only
/// when Result is null or does not cover the mandatory prefix through
/// `object_size`.  No C++ exception crosses this boundary.
NEVERD_API int neverd_translate_x86_64_block_to_aarch64_object_v1(
    const neverd_translate_object_request_v1 *Request,
    neverd_translate_object_result_v1 *Result);

/// Release every owned pointer reached by `Result->struct_size`.  Safe on a
/// zeroed result and safe to call repeatedly.
NEVERD_API void neverd_translate_object_result_dispose(
    neverd_translate_object_result_v1 *Result);

#ifdef __cplusplus
}
#endif

#endif // NEVERD_SDK_CAPI_TRANSLATE_H
