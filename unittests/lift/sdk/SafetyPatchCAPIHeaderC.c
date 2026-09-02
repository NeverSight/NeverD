//===- SafetyPatchCAPIHeaderC.c - Pure C sanitizer ABI contract ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/sdk/NeverDCAPI.h"

#include <stddef.h>
#include <stdint.h>

_Static_assert(offsetof(neverd_sanitize_options_v1, struct_size) == 0,
               "sanitize options struct_size must remain first");
_Static_assert(offsetof(neverd_sanitize_result_v1, struct_size) == 0,
               "sanitize result struct_size must remain first");
_Static_assert(sizeof(neverd_sanitize_status_t) == sizeof(uint32_t),
               "sanitize status carrier must stay uint32_t");
_Static_assert(sizeof(neverd_sanitize_strategy_t) == sizeof(uint32_t),
               "sanitize strategy carrier must stay uint32_t");
_Static_assert(sizeof(neverd_sanitize_publication_outcome_t) ==
                   sizeof(uint32_t),
               "sanitize publication outcome carrier must stay uint32_t");
_Static_assert(sizeof(neverd_sanitize_publication_namespace_t) ==
                   sizeof(uint32_t),
               "sanitize publication namespace carrier must stay uint32_t");
_Static_assert(sizeof(neverd_sanitize_publication_guarantees_t) ==
                   sizeof(uint32_t),
               "sanitize publication guarantees carrier must stay uint32_t");
_Static_assert(sizeof(neverd_sanitize_publication_operand_binding_t) ==
                   sizeof(uint32_t),
               "sanitize publication operand carrier must stay uint32_t");
_Static_assert(sizeof(((neverd_sanitize_result_v1 *)0)->status) ==
                   sizeof(uint32_t),
               "sanitize result status must ignore -fshort-enums");
_Static_assert(offsetof(neverd_sanitize_options_v1, strategy) == sizeof(size_t),
               "sanitize options strategy offset drift");
_Static_assert(offsetof(neverd_sanitize_options_v1, max_paths) ==
                   offsetof(neverd_sanitize_options_v1, strategy) +
                       sizeof(uint32_t),
               "sanitize options prefix drift");
_Static_assert(offsetof(neverd_sanitize_result_v1, status) ==
                   offsetof(neverd_sanitize_result_v1, ok) + sizeof(int),
               "sanitize result status offset drift");
_Static_assert(offsetof(neverd_sanitize_result_v1, plan_version) ==
                   offsetof(neverd_sanitize_result_v1, status) +
                       sizeof(uint32_t),
               "sanitize result fields must remain append-only");
_Static_assert(NEVERD_SANITIZE_STATUS_OK == 0,
               "zero-initialized result status must mean ok");
_Static_assert(NEVERD_SANITIZE_STATUS_PUBLISH_FAILED == 16,
               "sanitize status ABI drift");
_Static_assert(NEVERD_SANITIZE_STATUS_SIGNATURE_UNSUPPORTED == 17,
               "sanitize status ABI append drift");
_Static_assert(NEVERD_SANITIZE_STATUS_SIGNING_FAILED == 18,
               "sanitize status ABI append drift");
_Static_assert(NEVERD_SANITIZE_STATUS_PUBLISH_INDETERMINATE == 19,
               "sanitize indeterminate status ABI drift");
_Static_assert(NEVERD_SANITIZE_STATUS_PUBLISHED_INCOMPLETE == 20,
               "sanitize incomplete receipt status ABI drift");
_Static_assert(offsetof(neverd_sanitize_result_v1, publication_outcome) == 80,
               "sanitize publication outcome offset drift");
_Static_assert(offsetof(neverd_sanitize_result_v1,
                        publication_receipt_version) == 84,
               "sanitize publication receipt version offset drift");
_Static_assert(offsetof(neverd_sanitize_result_v1,
                        publication_receipt_complete) == 88,
               "sanitize publication receipt complete offset drift");
_Static_assert(offsetof(neverd_sanitize_result_v1,
                        publication_namespace_disposition) == 92,
               "sanitize publication namespace offset drift");
_Static_assert(offsetof(neverd_sanitize_result_v1,
                        publication_guarantee_flags) == 96,
               "sanitize publication guarantee offset drift");
_Static_assert(offsetof(neverd_sanitize_result_v1,
                        publication_operand_binding) == 100,
               "sanitize publication binding offset drift");
_Static_assert(sizeof(neverd_sanitize_result_v1) == 104,
               "sanitize result v1 size drift");

void neverd_safety_patch_c_api_header_compile_test(void) {
  int (*Sanitize)(neverd_session_t, const char *,
                  const neverd_sanitize_options_v1 *,
                  neverd_sanitize_result_v1 *) = neverd_session_sanitize;
  const char *(*StatusName)(neverd_sanitize_status_t) =
      neverd_sanitize_status_name;
  uint32_t (*PublicationABIVersion)(void) =
      neverd_sanitize_publication_abi_version;
  (void)Sanitize;
  (void)StatusName;
  (void)PublicationABIVersion;
}
