//===- EVMMedIRVerifier.h - LowIR/MedIR boundary verifier -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Declares the read-only structural verifier used before HighIR indexes any
/// externally supplied LowIR/MedIR table.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_EVM_ANALYSIS_EVMMEDIRVERIFIER_H
#define NEVERD_LIB_EVM_ANALYSIS_EVMMEDIRVERIFIER_H

#include "neverd/evm/analysis/EVMAnalyzer.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"

#include <cstdint>
#include <optional>

namespace neverd::evm::detail {

enum class MedIRValidationFailureKind : uint8_t {
#define EVM_MED_IR_VALIDATION_FAILURE(ID, MESSAGE) ID,
#include "neverd/evm/analysis/EVMMedIRValidation.def"
};

struct MedIRValidationFailure {
  uint64_t PC = kEntryPC;
  MedIRValidationFailureKind Kind =
      MedIRValidationFailureKind::MedTableCardinality;
};

[[nodiscard]] llvm::StringRef
medIRValidationFailureMessage(MedIRValidationFailureKind Kind);

/// Verifies the canonical LowIR fields consumed by the interpreter without
/// consulting CFG or abstract-analysis tables. Malformed public input is
/// returned as an API error before execution indexes or dereferences it.
[[nodiscard]] llvm::Error verifyLowIRForExecution(const EVMLowIR &Low);

/// Applies the configured LowIR cardinality and aggregate bounds, then
/// verifies the complete public LowIR structure consumed by MedIR lowering.
/// The check performs no allocation proportional to caller-supplied records.
[[nodiscard]] llvm::Error
verifyLowIRForMedLowering(const EVMLowIR &Low, const AnalyzeOptions &Options);

/// Performs the resource/structure preflight above, reconstructs canonical
/// LowIR from the embedded bytecode, and compares every public semantic field.
/// No lowering index or caller-proportional copy is built before this passes.
[[nodiscard]] llvm::Error
verifyCanonicalLowIRForMedLowering(const EVMLowIR &Low,
                                   const AnalyzeOptions &Options);

/// Verifies every table identity and reference needed by HighIR before any
/// identifier is used as an index. The first failure is stable for identical
/// inputs and carries a source PC suitable for the public fail-closed
/// diagnostic.
[[nodiscard]] std::optional<MedIRValidationFailure>
verifyMedIRStructure(const EVMLowIR &Low, const EVMMedIR &Med);

/// Applies all LowIR/MedIR aggregate bounds owned by the HighIR boundary.
/// Internally produced canonical IR still passes this accounting even when it
/// can safely skip external-input structure and replay validation.
[[nodiscard]] std::optional<MedIRValidationFailure>
verifyIRResourceBoundsForHighAnalysis(const EVMLowIR &Low, const EVMMedIR &Med,
                                      const AnalyzeOptions &Options);

/// Verifies configured bounds, structurally validates both IR levels, then
/// reconstructs LowIR and MedIR from bytecode and compares every public field.
/// This is the boundary required before HighIR consumes caller-supplied IR.
[[nodiscard]] std::optional<MedIRValidationFailure>
verifyMedIRForHighAnalysis(const EVMLowIR &Low, const EVMMedIR &Med,
                           const AnalyzeOptions &Options);

} // namespace neverd::evm::detail

#endif // NEVERD_LIB_EVM_ANALYSIS_EVMMEDIRVERIFIER_H
