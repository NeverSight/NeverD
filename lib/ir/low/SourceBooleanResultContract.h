#ifndef NEVERD_IR_LOW_SOURCEBOOLEANRESULTCONTRACT_H
#define NEVERD_IR_LOW_SOURCEBOOLEANRESULTCONTRACT_H

#include "neverd/ir/SourceABI.h"
#include "neverd/ir/TargetRegInfo.h"

namespace neverd {

/// Raw selected-call evidence, separate from a byte-valued source expression.
/// The runtime supplies exactly one meaningful result bit in an eight-byte
/// carrier. Parameter locations are complete, independently authenticated ABI
/// facts. This structure is neither a SourceFunctionTypeHint nor permission to
/// normalize the unspecified result bits.
struct SourceBooleanResultContract {
  Arch Architecture = Arch::Unknown;
  SourceFunctionTypeHint::ConventionKind Convention =
      SourceFunctionTypeHint::ConventionKind::Swift;
  std::vector<SourceParameterTypeHint> Parameters;
  uint64_t ResultRegister = 0;
  uint16_t ResultCarrierBytes = 0;
  uint16_t DefinedResultBits = 0;
};

inline std::optional<std::vector<SourceABIParameter>>
sourceBooleanInputParameters(const SourceBooleanResultContract &Contract) {
  const auto &TRI = getTargetRegInfo(Arch::AArch64);
  if (Contract.Architecture != Arch::AArch64 ||
      Contract.Convention != SourceFunctionTypeHint::ConventionKind::Swift ||
      Contract.ResultRegister != TRI.IntReturnReg ||
      Contract.ResultCarrierBytes != 8 || Contract.DefinedResultBits != 1)
    return std::nullopt;
  // SourceABI owns parameter geometry. A void return here means this temporary
  // describes inputs only: it never asserts a runtime void/byte/word result.
  SourceFunctionTypeHint Inputs;
  Inputs.Architecture = Contract.Architecture;
  Inputs.Convention = Contract.Convention;
  Inputs.Parameters = Contract.Parameters;
  Inputs.ReturnType = NdType::makeVoid();
  Inputs.HasExplicitABI = true;
  std::string Error;
  if (!validateSourceABI(Inputs, Error))
    return std::nullopt;
  return sourceABIParameters(Inputs);
}

/// Ordinary calls in the difference proof require complete, independently
/// established source ABIs. Frame-borrowing and native-state privileges are
/// intentionally absent: they belong to their separate semantic owners.
struct SourceBooleanOtherCallContract {
  const SourceFunctionTypeHint *Signature = nullptr;
  bool DoesNotReturn = false;
};
} // namespace neverd
#endif
