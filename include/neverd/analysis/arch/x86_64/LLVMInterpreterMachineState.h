//===- LLVMInterpreterMachineState.h - Scalar source model -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_ANALYSIS_LLVMINTERPRETERMACHINESTATE_H
#define NEVERD_ANALYSIS_LLVMINTERPRETERMACHINESTATE_H

#include "neverd/analysis/LLVMInterpreterModel.h"
#include "neverd/analysis/arch/x86_64/InterpreterMachineState.h"

namespace llvm {
class Function;
}

namespace neverd::analysis {

/// Return the full-state observation contract required by the LLVM model:
/// all 17 state words, plus a zero, preserved definedness byte. RETURN observes
/// the actual source status independently. Callers add the entry domain,
/// accessible guest frame and preservation obligations; they must retain these
/// observations and the zero-definedness obligation. This runs no proof.
LowIRIndependenceContract llvmInterpreterMachineStateContract();

/// Model a verified scalar LLVM function with one state-pointer argument and
/// an i64 status result. The raw state-object byte layout matches
/// modelInterpreterMachineStateX64. Guest memory remains ordinary memory.
/// State storage must be accessible, eight-byte aligned and disjoint from all
/// guest accesses for the entire call, on a little-endian integral 64-bit
/// pointer/index layout. The input function is not modified.
/// Entry bytes read by the function must be initialized. Guest accesses must
/// designate live byte storage in the caller's declared frame; flat LowIR
/// memory does not establish LLVM object lifetime or pointer provenance.
///
/// Supported integer widths are i1/i8/i16/i32/i64, with explicit PHI edges,
/// branches, switches, fixed in-object pointer projections and a bounded
/// scalar subset. Unsupported instructions, attributes, metadata, pointer
/// escapes, memory modes or exhausted limits return an error and no model.
/// Guarded arithmetic emits sticky definedness obligations. These require
/// every executed admitted operation to be non-poison, a conservative
/// restriction even when a later select or dead use could mask poison.
/// Guest load/store alignment also emits a per-access definedness obligation;
/// it never restricts the entry domain or grants additional accessible bytes.
/// State-object accesses retain their separate eight-byte alignment contract.
/// Variable shifts preserve the full unsigned count and guard it against the
/// source width; no-wrap and exact flags add their own obligations.
/// An initializes parameter contract is admitted only for in-object byte
/// ranges that ordinary state stores initialize on every path before reads
/// and normal returns. Bounded must-dataflow shares pointer projections and
/// MaxWork; it may conservatively reject path-correlated valid programs.
/// Division, freeze, undef/poison literals, arbitrary calls,
/// vector/floating operations and exceptions are currently refused.
///
/// Use llvmInterpreterMachineStateContract and a fresh complete finite or
/// inductive refinement check. Model generation and its deterministic LowIR
/// records are not an LLVM/C/native certificate, do not justify native
/// architecture-undefined choices, and do not prove the compiler. Callers
/// must separately bind the exact source, LLVM module and compiler inputs.
llvm::Expected<InterpreterMachineStateModel>
modelLLVMInterpreterMachineStateX64(
    const llvm::Function &Function,
    const LLVMInterpreterModelLimits &Limits = {});

} // namespace neverd::analysis

#endif
