//===- RecoveryArchitecture.h - Recovery policy -----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file Native recovery contract dispatch.

#ifndef NEVERD_ANALYSIS_CORE_RECOVERYARCHITECTURE_H
#define NEVERD_ANALYSIS_CORE_RECOVERYARCHITECTURE_H

#include "neverd/analysis/InterpreterSpecialization.h"
#include "neverd/symbolic/SymExpr.h"

#include "llvm/Support/Errc.h"

#include <memory>
#include <optional>

namespace neverd::symbolic {
class SymContext;
class SymState;
} // namespace neverd::symbolic

namespace neverd::analysis {
/// Runtime address-space policy belongs to the declared architecture. The
/// common executor handles the resulting full addresses and ordinary aliases.
llvm::Error validateRuntimeMemoryOptions(const SpecializationOptions &Options);
bool supportsRuntimeMemoryOperation(const LowOp &Op,
                                    const SpecializationOptions &Options);
void seedRuntimeMemoryState(symbolic::SymContext &Ctx,
                            symbolic::SymState &State,
                            const SpecializationOptions &Options);

/// A retained native store's ordinary-memory evaluation. Operand positions
/// remain identical; only the opcode and constant input 0 change. The common
/// executor must apply its full store alias/provenance checks while retaining
/// the native operation in the residual and emitted source.
std::optional<LowOp>
retainedStoreEvaluation(const LowOp &Op, const SpecializationOptions &Options);

/// Architecture-owned control values are separate from guest registers and
/// instruction-local temporaries. Node projection may forget correlations but
/// must retain the complete declared architectural domain.
struct RuntimeControlState {
  std::vector<symbolic::SymRef> Values;
};

struct RuntimeControlTransition {
  /// Exact scalar evaluation only. The original intrinsic remains in LowIR.
  /// This interface admits no memory writes: those require the core's ordinary
  /// alias, provenance and return-cell checks.
  std::vector<LowOp> Ops;
  std::optional<NdVar> Rejected;
  std::optional<std::pair<size_t, NdVar>> OutputState;
  std::string InvalidValueDiagnostic;
};

llvm::Error validateRuntimeControlOptions(const SpecializationOptions &Options);
bool supportsRuntimeControlOperation(const LowOp &Op,
                                     const SpecializationOptions &Options);
RuntimeControlState
seedRuntimeControlState(symbolic::SymContext &Ctx,
                        const SpecializationOptions &Options);
RuntimeControlState
seedRuntimeControlState(symbolic::SymContext &Ctx, symbolic::SymState &State,
                        const SpecializationOptions &Options);
/// Commit an evaluated architecture-owned control value and any dependent
/// address interpretation together. Refusal leaves the machine state intact.
/// The caller must first discharge the transition's rejection predicate.
llvm::Error commitRuntimeControlState(symbolic::SymState &State,
                                      RuntimeControlState &Controls,
                                      size_t Index, symbolic::SymRef Value,
                                      const SpecializationOptions &Options);
std::optional<RuntimeControlTransition>
lowerRuntimeControlOperation(const LowOp &Op, symbolic::SymState &State,
                             const RuntimeControlState &Controls,
                             const SpecializationOptions &Options,
                             llvm::function_ref<NdVar(uint16_t)> Temporary);

/// An architecture-authenticated read with instruction-local output carriers.
/// Each occurrence receives an independent, unconstrained sample. The original
/// read and its inputs remain observable in LowIR; these scalar operations only
/// connect the complete instruction's outputs for conservative raw analysis.
/// No memory, flag, accessibility, source or native-proof contract is added.
struct RuntimeRegisterRead {
  uint16_t SampleBytes = 0;
  std::vector<LowOp> Ops;
  std::vector<NdVar> PlaceholderOutputs;
};

std::optional<RuntimeRegisterRead>
lowerRuntimeRegisterRead(const SpecializationInstruction &Instruction,
                         const SpecializationOptions &Options, NdVar Sample);

struct RawEnvironmentPreparation {
  uint64_t Bytes = 0;
  uint64_t Records = 0;
};

/// One allowance shared by generic receipt walks and native validation work.
struct RecoveryValidationBudget {
  uint64_t Bytes;
  uint64_t Records;
  bool bytes(uint64_t N) {
    if (N > Bytes)
      return false;
    Bytes -= N;
    return true;
  }
  bool records(uint64_t N, uint64_t Width) {
    if (N > Records || N > Bytes / Width)
      return false;
    Records -= N;
    Bytes -= N * Width;
    return true;
  }
  static llvm::Error exhausted() {
    return llvm::createStringError(
        std::make_error_code(std::errc::value_too_large),
        "raw environment result validation budget exhausted");
  }
};

/// Immutable index of caller-declared physical exits. Lookup supplies only a
/// declaration; the original instruction still needs its native validation.
/// Construction and queries share the specialization operation allowance.
class RecoveryMachineExitLookup {
public:
  virtual ~RecoveryMachineExitLookup() = default;
  /// False means insufficient work. A successful miss leaves Exit empty.
  virtual bool
  lookup(va_t Address, uint64_t &Work,
         std::optional<SpecializationExternalExit> &Exit) const = 0;
};

/// Native policies live under arch/<isa>. Core owns graph/evidence integrity
/// and generic resource accounting; raw integrity never interprets native
/// fields.
class RecoveryArchitecture {
public:
  virtual ~RecoveryArchitecture() = default;
  virtual llvm::Expected<RawEnvironmentPreparation>
  validateRawDeclarations(const SpecializationOptions &Options) const = 0;
  /// Options must have passed declaration validation. The owned snapshot is
  /// reusable across precision retries, which cannot change native exits.
  virtual llvm::Expected<std::unique_ptr<RecoveryMachineExitLookup>>
  prepareMachineExitLookup(const SpecializationOptions &Options,
                           uint64_t &Work) const = 0;
  virtual std::string
  rawOptionsDigest(const SpecializationOptions &Options) const = 0;
  virtual llvm::Error
  validateRawExtent(const SpecializationRawEnvironment &Domain) const = 0;
  virtual llvm::Error
  reserveExternalExitValidation(const SpecializationResult &Result,
                                const SpecializationOptions &Options,
                                RecoveryValidationBudget &Budget) const = 0;
  virtual std::string
  externalExitDigest(const SpecializationExternalExit &Exit) const = 0;
  virtual llvm::Error
  validateExternalExits(const SpecializationResult &Result,
                        const SpecializationOptions &Options) const = 0;
};

/// Selected from the declared target, never from the build host or receipt.
/// An architecture without an implementation refuses; it cannot borrow x64.
llvm::Expected<const RecoveryArchitecture *> recoveryArchitecture(Arch Target);
} // namespace neverd::analysis
#endif
