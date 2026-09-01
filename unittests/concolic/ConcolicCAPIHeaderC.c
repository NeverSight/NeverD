//===- ConcolicCAPIHeaderC.c - Pure C concolic ABI test -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/sdk/NeverDCAPI.h"

#include <stddef.h>
#include <stdint.h>

_Static_assert(offsetof(neverd_lowir_concolic_register_seed_v1, offset) == 0,
               "concolic seed offset must remain first");
_Static_assert(offsetof(neverd_lowir_concolic_register_seed_v1, reserved) +
                       sizeof(uint32_t) ==
                   sizeof(neverd_lowir_concolic_register_seed_v1),
               "concolic seed fields must consume trailing padding");
_Static_assert(offsetof(neverd_lowir_concolic_options_v1, struct_size) == 0,
               "concolic options struct_size must remain first");
_Static_assert(offsetof(neverd_lowir_concolic_options_v1, solver_max_gates) +
                       sizeof(uint64_t) ==
                   sizeof(neverd_lowir_concolic_options_v1),
               "concolic options fields must remain append-only");

#if SIZE_MAX == UINT64_MAX
_Static_assert(sizeof(neverd_lowir_concolic_register_seed_v1) == 24,
               "64-bit concolic seed layout drift");
_Static_assert(offsetof(neverd_lowir_concolic_register_seed_v1, value) == 8,
               "64-bit concolic seed layout drift");
_Static_assert(offsetof(neverd_lowir_concolic_register_seed_v1, bytes) == 16,
               "64-bit concolic seed layout drift");
_Static_assert(offsetof(neverd_lowir_concolic_register_seed_v1, reserved) == 20,
               "64-bit concolic seed layout drift");

_Static_assert(sizeof(neverd_lowir_concolic_options_v1) == 80,
               "64-bit concolic options layout drift");
_Static_assert(offsetof(neverd_lowir_concolic_options_v1, register_seeds) == 8,
               "64-bit concolic options layout drift");
_Static_assert(offsetof(neverd_lowir_concolic_options_v1,
                        register_seed_count) == 16,
               "64-bit concolic options layout drift");
_Static_assert(offsetof(neverd_lowir_concolic_options_v1, max_steps) == 24,
               "64-bit concolic options layout drift");
_Static_assert(offsetof(neverd_lowir_concolic_options_v1,
                        max_loop_iterations) == 32,
               "64-bit concolic options layout drift");
_Static_assert(offsetof(neverd_lowir_concolic_options_v1, max_flip_attempts) ==
                   36,
               "64-bit concolic options layout drift");
_Static_assert(offsetof(neverd_lowir_concolic_options_v1, max_candidates) == 40,
               "64-bit concolic options layout drift");
_Static_assert(offsetof(neverd_lowir_concolic_options_v1, reserved) == 44,
               "64-bit concolic options layout drift");
_Static_assert(offsetof(neverd_lowir_concolic_options_v1,
                        solver_max_conflicts) == 48,
               "64-bit concolic options layout drift");
_Static_assert(offsetof(neverd_lowir_concolic_options_v1,
                        solver_max_propagations) == 56,
               "64-bit concolic options layout drift");
_Static_assert(offsetof(neverd_lowir_concolic_options_v1,
                        solver_max_watch_visits) == 64,
               "64-bit concolic options layout drift");
_Static_assert(offsetof(neverd_lowir_concolic_options_v1, solver_max_gates) ==
                   72,
               "64-bit concolic options layout drift");
#endif

// Referencing the entry point through its exact type catches C++ linkage or
// ownership-signature drift without executing a binary.
void neverd_concolic_c_api_header_compile_test(void) {
  const char *(*Run)(neverd_session_t, neverd_va_t,
                     const neverd_lowir_concolic_options_v1 *) =
      neverd_lowir_concolic_json_v1;
  (void)Run;
}
