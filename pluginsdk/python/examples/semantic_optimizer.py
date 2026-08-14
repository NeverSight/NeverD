from __future__ import annotations

from neverd_plugin import (
    LLVMOptimizationLevel,
    OptimizationMode,
    Plugin,
    PluginType,
    ProofStatus,
    Session,
    optimize_llvm_ir,
    synthesize_expression,
)


_DEMO_IR = """
define i32 @semantic_demo(i32 %x) {
entry:
  %a = lshr i32 %x, 4
  %b0 = lshr i32 %x, 2
  %b = lshr i32 %b0, 2
  %sum = add i32 %a, %b
  ret i32 %sum
}
"""


@Plugin(
    name="Python Semantic Optimizer",
    version="1.0.0",
    author="NeverD contributors",
    description="Runs proof-gated synthesis and transactional LLVM optimization",
    type=PluginType.PROCESSOR,
)
class SemanticOptimizer:
    def on_run(self, session: Session, arg: int) -> int:
        del session, arg
        rewrite = synthesize_expression(
            "(x >> 4) + ((x >> 2) >> 2)",
            max_cost=5,
            max_work=1 << 18,
            verify_samples=32,
        )
        if rewrite.changed and rewrite.proof_status is not ProofStatus.EQUIVALENT:
            raise RuntimeError("NeverD exposed a rewrite without a proof")

        module = optimize_llvm_ir(
            _DEMO_IR,
            mode=OptimizationMode.DEEP,
            llvm_level=LLVMOptimizationLevel.O2,
            enable_synthesis=True,
            synthesis_max_cost=5,
            synthesis_max_work=1 << 18,
        )
        print(
            f"expression={rewrite.output}; proof={rewrite.proof_status.name}; "
            f"llvm_stop={module.stop.name}; rewrites={module.semantic_rewrites}"
        )
        return 0
