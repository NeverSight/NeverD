//===- LLVMScalarFunctionModel.h - Pure scalar LLVM model -------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_ANALYSIS_LLVMSCALARFUNCTIONMODEL_H
#define NEVERD_ANALYSIS_LLVMSCALARFUNCTIONMODEL_H

#include "neverd/analysis/LLVMInterpreterModel.h"

#include <functional>

namespace neverd::analysis {

enum class LLVMScalarInputDomain {
  /// Preserve the strict public LLVM contract: every argument is noundef.
  RequireNoUndef,
  /// Explicit caller precondition: consider only defined integer bitvectors.
  /// This neither annotates the IR nor proves the native calling convention.
  DefinedBitvectors,
};

struct LLVMScalarModelOptions {
  LLVMScalarInputDomain InputDomain = LLVMScalarInputDomain::RequireNoUndef;
  /// Cooperative stop during bounded construction and proof work. The callback
  /// must not modify the input IR. Empty retains the original behavior.
  std::function<bool()> StopRequested;
};

struct LLVMScalarArgument {
  NdVar Storage;
  unsigned Bits = 0;
};

/// A pure scalar interface to the same LLVM model used for state wrappers.
/// Arguments occupy distinct private register ranges; these offsets are model
/// identities, not addresses or an inferred native ABI. A one-bit argument
/// must be zero-extended into its byte of storage. RETURN carries the result.
struct LLVMScalarFunctionModel {
  LLVMScalarInputDomain InputDomain = LLVMScalarInputDomain::RequireNoUndef;
  InterpreterMachineStateModel Graph;
  std::vector<LLVMScalarArgument> Arguments;
  unsigned ResultBits = 0;
};

/// Model a verified function with noundef integer arguments and an integer
/// result. Supports i1/i8/i16/i32/i64 subject to the existing scalar model's
/// instruction and attribute contract. Memory, pointer operations, ordinary
/// calls, exceptions, undef/poison and unmodeled instructions are rejected.
/// A canonical, bundle-free llvm.assume records a definedness obligation;
/// it never restricts the symbolic input domain or supplies a proved fact.
/// The caller must initialize LLVMInterpreterDefinednessOffset to zero and
/// prove that it remains zero on every admitted execution, as well as prove
/// termination and the complete return value. Model construction alone is
/// not an equivalence or ABI certificate. The source function is unchanged.
/// DefinedBitvectors explicitly restricts the query to defined argument values
/// when noundef is absent; all other admission and definedness checks remain.
llvm::Expected<LLVMScalarFunctionModel>
modelLLVMScalarFunction(const llvm::Function &Function,
                        const LLVMInterpreterModelLimits &Limits = {},
                        const LLVMScalarModelOptions &Options = {});

} // namespace neverd::analysis

#endif
