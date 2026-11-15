//===- semantic_plugin.c - Semantic optimization plugin example -*- C -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/sdk/NeverDPlugin.h"

#include <stdio.h>

static int runSynthesis(void) {
  static const char Expression[] = "(x >> 4) + ((x >> 2) >> 2)";
  neverd_synthesize_options Options = {0};
  neverd_synthesize_result Result = {0};
  Options.struct_size = sizeof(Options);
  Options.width = 32;
  Options.max_cost = 5;
  Options.max_work = 1u << 18;
  Options.verify_samples = 32;
  Result.struct_size = sizeof(Result);

  if (neverd_synthesize_expr(Expression, &Options, &Result) != 0) {
    fputs("[SemanticPlugin] synthesis request was rejected\n", stderr);
    return 1;
  }
  if (!Result.ok) {
    fprintf(stderr, "[SemanticPlugin] synthesis failed: %s\n",
            Result.error ? Result.error : "unknown error");
    neverd_synthesize_result_dispose(&Result);
    return 1;
  }
  if (Result.changed && Result.proof_status != NEVERD_PROOF_EQUIVALENT) {
    fputs("[SemanticPlugin] unproved rewrite was exposed\n", stderr);
    neverd_synthesize_result_dispose(&Result);
    return 1;
  }

  printf("[SemanticPlugin] %s -> %s (%s, proof=%s)\n",
         Result.input ? Result.input : "", Result.output ? Result.output : "",
         neverd_synthesis_outcome_name(Result.outcome),
         neverd_proof_status_name(Result.proof_status));
  neverd_synthesize_result_dispose(&Result);
  return 0;
}

static int runLLVMOptimization(void) {
  static const char IR[] = "define i32 @semantic_demo(i32 %x) {\n"
                           "entry:\n"
                           "  %a = lshr i32 %x, 4\n"
                           "  %b0 = lshr i32 %x, 2\n"
                           "  %b = lshr i32 %b0, 2\n"
                           "  %sum = add i32 %a, %b\n"
                           "  ret i32 %sum\n"
                           "}\n";
  neverd_optimize_llvm_ir_options Options = {0};
  neverd_optimize_llvm_ir_result Result = {0};
  Options.struct_size = sizeof(Options);
  Options.mode = NEVERD_OPTIMIZATION_MODE_DEEP;
  Options.llvm_level = NEVERD_LLVM_OPTIMIZATION_O2;
  Options.enable_synthesis = 1;
  Options.synthesis_max_cost = 5;
  Options.synthesis_max_work = 1u << 18;
  Result.struct_size = sizeof(Result);

  if (neverd_optimize_llvm_ir(IR, &Options, &Result) != 0) {
    fputs("[SemanticPlugin] LLVM optimization request was rejected\n", stderr);
    return 1;
  }
  if (!Result.ok || !Result.output_ir) {
    fprintf(stderr, "[SemanticPlugin] LLVM optimization failed: %s\n",
            Result.error ? Result.error : "unknown error");
    neverd_optimize_llvm_ir_result_dispose(&Result);
    return 1;
  }

  printf("[SemanticPlugin] LLVM stop=%s, rewrites=%llu, proofs=%llu\n",
         neverd_optimization_stop_name(Result.stop),
         (unsigned long long)Result.semantic_rewrites,
         (unsigned long long)Result.proof_queries);
  neverd_optimize_llvm_ir_result_dispose(&Result);
  return 0;
}

static int semanticRun(neverd_session_t Session, int Arg) {
  (void)Session;
  (void)Arg;
  return runSynthesis() != 0 || runLLVMOptimization() != 0;
}

NEVERD_PLUGIN_EXPORT neverd_plugin_t neverd_plugin = {
    .Name = "Semantic Optimizer",
    .Version = "1.0.0",
    .Author = "NeverD contributors",
    .Description = "Proof-gated synthesis and transactional LLVM optimization",
    .Type = NEVERD_PLUGIN_PROCESSOR,
    .Init = NULL,
    .Term = NULL,
    .Run = semanticRun,
    .Event = NULL,
};
