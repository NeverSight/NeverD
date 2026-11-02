//===- ScalarValidation.h - Lifted return-value validation ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Conditional equivalence of bounded pure integer models, with exact native
/// entry and register-parameter bindings. This is not a native-state proof.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_DIFF_SCALARVALIDATION_H
#define NEVERD_DIFF_SCALARVALIDATION_H

#include "neverd/Common.h"
#include "neverd/analysis/LLVMScalarEquivalence.h"
#include "neverd/diff/Control.h"
#include "neverd/diff/Limits.h"
#include "neverd/diff/Program.h"

namespace neverd {
struct ExceptionFunction;
} // namespace neverd

namespace neverd::diff {

inline constexpr llvm::StringLiteral
    ScalarValidationProfile("lifted-scalar-returns-v1");

struct ScalarValidationOptions {
  uint64_t MaxNativeInstructions =
      limits::diff::kDefaultValidationNativeInstructions;
  uint64_t MaxWork = analysis::LLVMScalarEquivalenceLimits{}.MaxWork;
  std::optional<std::string> validate() const;
};

struct ScalarValidationArgument {
  uint64_t RegisterOffset = 0;
  unsigned Bits = 0;
};

/// Borrowed fresh lift and its normalized source unwind record. The SDK retains
/// the exact MedIR record through validation. Absent records do not authorize
/// exception metadata; present records must be complete, unwind-only, exactly
/// bound to this native body and equal to the emitter's canonical projection.
struct ScalarValidationLift {
  const llvm::Function &Function;
  const ExceptionFunction *Exceptions = nullptr;
  BinaryFormat ABI = BinaryFormat::Unknown;
};

struct ScalarValidationResult {
  ComparisonState State = ComparisonState::Complete;
  analysis::LLVMScalarEquivalenceResult Proof;
  std::vector<ScalarValidationArgument> Arguments;
  unsigned ResultBits = 0;
};

llvm::StringRef
scalarValidationStatusName(analysis::LLVMScalarEquivalenceStatus Status);

/// Native profile refusal, shared with the SDK before lifting. Callers first
/// enforce MaxNativeInstructions; this scan follows that bounded native body.
std::optional<std::string> scalarValidationNativeGap(const Program &Program,
                                                     uint32_t Index);

/// Both functions must come from fresh, audited machine-ABI lifting of the
/// supplied snapshots. The SDK owns that audit and input rebinding. This layer
/// checks original entries, pure native scope and exact GPR input positions.
/// It copies each body, removing only validated NeverD provenance packaging and
/// the pointer-null policy (the scalar model refuses all pointer/memory
/// effects). A canonical, source-bound unwind-only attachment may be projected
/// away; language handlers and unknown metadata/attributes remain refusals.
/// Defined integer input values are an explicit precondition; the source IR is
/// never annotated or modified. Proved means equal integer returns and defined
/// termination in these models. It does not prove lifting, complete native
/// state, memory, calls or security.
ScalarValidationResult
validateScalarFunctions(const Program &Primary, uint32_t PrimaryIndex,
                        const ScalarValidationLift &Left,
                        const Program &Secondary, uint32_t SecondaryIndex,
                        const ScalarValidationLift &Right,
                        const ScalarValidationOptions &Options = {},
                        const ComparisonControl &Control = {});

} // namespace neverd::diff

#endif // NEVERD_DIFF_SCALARVALIDATION_H
