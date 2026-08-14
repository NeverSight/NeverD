//===- SemanticCAPIHeaderC.c - Pure C semantic ABI compile test ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/sdk/NeverDCAPI.h"

#include <stddef.h>
#include <stdint.h>

_Static_assert(NEVERD_PROOF_NOT_RUN == 0, "proof ABI drift");
_Static_assert(NEVERD_PROOF_UNKNOWN == 3, "proof ABI drift");
_Static_assert(NEVERD_PROOF_INVALID == 4, "proof ABI drift");
_Static_assert(NEVERD_SYNTHESIS_REWRITTEN == 6, "synthesis ABI drift");
_Static_assert(NEVERD_OPTIMIZATION_INPUT_INVALID == 4,
               "optimization ABI drift");
_Static_assert(offsetof(neverd_simplify_options, exhaustive) ==
                   offsetof(neverd_simplify_options, allow_growth) +
                       sizeof(int),
               "simplify options must remain append-only");

// Referencing every entry point through its exact C type catches accidental
// C++-only declarations without adding another executable or runtime test.
void neverd_semantic_c_api_header_compile_test(void) {
  int (*Simplify)(const char *, const neverd_simplify_options *,
                  neverd_simplify_result *) = neverd_simplify_expr;
  void (*DisposeSimplification)(neverd_simplify_result *) =
      neverd_simplify_result_dispose;
  const char *(*SimplificationJSON)(const char *, unsigned, int) =
      neverd_simplify_expr_json;

  int (*Synthesize)(const char *, const neverd_synthesize_options *,
                    neverd_synthesize_result *) = neverd_synthesize_expr;
  void (*DisposeSynthesis)(neverd_synthesize_result *) =
      neverd_synthesize_result_dispose;
  const char *(*SynthesisJSON)(const char *,
                               const neverd_synthesize_options *) =
      neverd_synthesize_expr_json_v1;

  int (*Optimize)(const char *, const neverd_optimize_llvm_ir_options *,
                  neverd_optimize_llvm_ir_result *) = neverd_optimize_llvm_ir;
  void (*DisposeOptimization)(neverd_optimize_llvm_ir_result *) =
      neverd_optimize_llvm_ir_result_dispose;
  const char *(*OptimizationJSON)(const char *,
                                  const neverd_optimize_llvm_ir_options *) =
      neverd_optimize_llvm_ir_json_v1;

  (void)Simplify;
  (void)DisposeSimplification;
  (void)SimplificationJSON;
  (void)Synthesize;
  (void)DisposeSynthesis;
  (void)SynthesisJSON;
  (void)Optimize;
  (void)DisposeOptimization;
  (void)OptimizationJSON;
}
