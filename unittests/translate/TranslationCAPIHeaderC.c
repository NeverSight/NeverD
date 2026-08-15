//===- TranslationCAPIHeaderC.c - Pure C translation ABI test -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/sdk/NeverDCAPI.h"

#include <stddef.h>

_Static_assert(sizeof(neverd_translate_object_format_t) == sizeof(uint32_t),
               "object format ABI must be fixed-width");
_Static_assert(sizeof(neverd_translate_error_code_t) == sizeof(uint32_t),
               "translation error ABI must be fixed-width");
_Static_assert(sizeof(neverd_translate_semantic_stop_t) == sizeof(uint32_t),
               "semantic stop ABI must be fixed-width");
_Static_assert(sizeof(neverd_translate_proof_status_t) == sizeof(uint32_t),
               "proof status ABI must be fixed-width");
_Static_assert(NEVERD_TRANSLATE_OBJECT_FORMAT_ELF == 1,
               "object format ABI drift");
_Static_assert(NEVERD_TRANSLATE_OBJECT_FORMAT_MACHO == 2,
               "object format ABI drift");
_Static_assert(NEVERD_TRANSLATE_ERROR_INVALID_REQUEST == 2,
               "request error ABI drift");
_Static_assert(NEVERD_TRANSLATE_ERROR_ARTIFACT_VERIFICATION_FAILED == 12,
               "request error ABI drift");
_Static_assert(NEVERD_TRANSLATE_ERROR_ALLOCATION_FAILURE == 13,
               "C boundary error ABI drift");
_Static_assert(NEVERD_TRANSLATE_ERROR_INTERNAL_FAILURE == 14,
               "C boundary error ABI drift");
_Static_assert(offsetof(neverd_translate_object_request_v1, struct_size) == 0,
               "request struct_size must remain first");
_Static_assert(offsetof(neverd_translate_object_request_v1, reserved) +
                       sizeof(uint32_t) ==
                   sizeof(neverd_translate_object_request_v1),
               "request fields must consume trailing padding");
_Static_assert(offsetof(neverd_translate_object_result_v1, struct_size) == 0,
               "result struct_size must remain first");
_Static_assert(offsetof(neverd_translate_object_result_v1,
                        object_pipeline_schema_version) +
                       sizeof(uint32_t) ==
                   sizeof(neverd_translate_object_result_v1),
               "result fields must remain append-only");

#if SIZE_MAX == UINT64_MAX
_Static_assert(sizeof(neverd_translate_object_request_v1) == 48,
               "64-bit request layout drift");
_Static_assert(offsetof(neverd_translate_object_request_v1, guest_bytes) == 8,
               "64-bit request layout drift");
_Static_assert(offsetof(neverd_translate_object_request_v1, entry_pc) == 24,
               "64-bit request layout drift");
_Static_assert(offsetof(neverd_translate_object_request_v1, object_format) == 40,
               "64-bit request layout drift");
_Static_assert(offsetof(neverd_translate_object_request_v1, reserved) == 44,
               "64-bit request layout drift");

_Static_assert(sizeof(neverd_translate_object_result_v1) == 240,
               "64-bit result layout drift");
_Static_assert(offsetof(neverd_translate_object_result_v1, ok) == 8,
               "64-bit result layout drift");
_Static_assert(offsetof(neverd_translate_object_result_v1, error_message) == 16,
               "64-bit result layout drift");
_Static_assert(offsetof(neverd_translate_object_result_v1, object_bytes) == 24,
               "64-bit result layout drift");
_Static_assert(offsetof(neverd_translate_object_result_v1, object_size) == 32,
               "64-bit result layout drift");
_Static_assert(offsetof(neverd_translate_object_result_v1, guest_entry_pc) == 48,
               "64-bit result layout drift");
_Static_assert(offsetof(neverd_translate_object_result_v1, block_ir_symbol) == 80,
               "64-bit result layout drift");
_Static_assert(offsetof(neverd_translate_object_result_v1,
                        translation_cache_identity) == 144,
               "64-bit result layout drift");
_Static_assert(offsetof(neverd_translate_object_result_v1, semantic_changed) ==
                   152,
               "64-bit result layout drift");
_Static_assert(offsetof(neverd_translate_object_result_v1, semantic_rewrites) ==
                   160,
               "64-bit result layout drift");
_Static_assert(offsetof(neverd_translate_object_result_v1,
                        semantic_function_pass_invocations) == 208,
               "64-bit result layout drift");
_Static_assert(offsetof(neverd_translate_object_result_v1,
                        llvm_optimization_pipeline_ran) == 228,
               "64-bit result layout drift");
_Static_assert(offsetof(neverd_translate_object_result_v1,
                        object_pipeline_schema_version) == 236,
               "64-bit result layout drift");
#endif

// Referencing each entry point through its exact C type catches accidental
// C++ linkage or C-incompatible declarations without running a test binary.
void neverd_translation_c_api_header_compile_test(void) {
  const char *(*ErrorName)(neverd_translate_error_code_t) =
      neverd_translate_error_code_name;
  int (*Translate)(const neverd_translate_object_request_v1 *,
                   neverd_translate_object_result_v1 *) =
      neverd_translate_x86_64_block_to_aarch64_object_v1;
  void (*Dispose)(neverd_translate_object_result_v1 *) =
      neverd_translate_object_result_dispose;

  (void)ErrorName;
  (void)Translate;
  (void)Dispose;
}
