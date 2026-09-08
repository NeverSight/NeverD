#include "neverd/pipeline/NativeSourceHints.h"

#include "neverd/ir/SourceABI.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/pipeline/Pipeline.h"

#include <algorithm>
#include <set>

namespace neverd {
namespace {
bool integerCarrier(const TypeRef &Type) {
  return Type &&
         ((Type->Kind == NdTypeKind::Int &&
           (Type->Size == 1 || Type->Size == 2 || Type->Size == 4 ||
            Type->Size == 8)) ||
          (Type->Kind == NdTypeKind::Ptr && Type->Size == 8 && Type->Pointee));
}

bool sameScalar(const TypeRef &A, const TypeRef &B) {
  return integerCarrier(A) && integerCarrier(B) && A->Kind == B->Kind &&
         A->Size == B->Size && A->IsSigned == B->IsSigned;
}
} // namespace

std::optional<SourceFunctionTypeHint> inferNativeSourceTypeHint(
    const BinaryImage &Image, const MedFunc &Med, const HighFunc &High,
    const PipelineFunctionAudit &Audit, std::string &Diagnostic) {
  Diagnostic.clear();
  auto Reject =
      [&](const char *Reason) -> std::optional<SourceFunctionTypeHint> {
    Diagnostic = Reason;
    return std::nullopt;
  };
  if (Image.Format != BinaryFormat::MachO || Image.Bits != Bitness::Bits64 ||
      Image.IsRelocatable ||
      (Image.Arch != Arch::AArch64 && Image.Arch != Arch::X64) ||
      !Image.isCodeAddress(Med.Entry) || Med.Entry != High.Entry ||
      Med.Entry != Audit.Entry)
    return Reject(
        "native source inference requires one linked Darwin function");
  if (Audit.Disposition != PipelineFunctionDisposition::Accepted ||
      !Audit.HasLowIR || !Audit.HasMedIR || !Audit.MedIRVerified ||
      !Audit.DecodedInstructions ||
      Audit.DecodedInstructions != Audit.LiftedInstructions ||
      !Audit.DecodeFailures.empty() || !Audit.UnsupportedInstructions.empty() ||
      !Audit.TruncatedPaths.empty())
    return Reject("native source inference requires complete verified lifting");
  if (Med.SourceTypeHint || High.SourceTypeHint || Med.SourceParametersBound)
    return Reject("native function already has a source declaration");
  if (Med.IsVariadic || !Med.MultiReturn.empty() || Med.FPReturnViaX87 ||
      Med.DoesNotReturn || High.DoesNotReturn)
    return Reject("native function has a non-scalar or non-returning ABI");
  if (Med.Blocks.empty() || High.Body.empty() ||
      Med.Params.size() != Med.TypedParams.size() || Med.Params.size() > 64 ||
      Med.Params.size() != High.Params.size() ||
      !sameScalar(Med.ReturnType, High.ReturnType))
    return Reject("native scalar parameter or return types are incomplete");

  const auto &TRI = getTargetRegInfo(Image.Arch);
  SourceFunctionTypeHint Hint;
  Hint.Origin = SourceFunctionTypeHint::OriginKind::NativeAnalysis;
  Hint.Architecture = Image.Arch;
  Hint.HasExplicitABI = true;
  Hint.ReturnType = Med.ReturnType;
  Hint.ReturnLocation.Kind = SourceABICarrierKind::IntegerRegister;
  Hint.ReturnLocation.RegisterOffset = TRI.IntReturnReg;
  Hint.ReturnLocation.ValueBytes = Hint.ReturnType->Size;
  std::set<uint64_t> ParameterRegisters;
  std::set<int> StackSlots;
  for (size_t Index = 0; Index < Med.Params.size(); ++Index) {
    const auto &Parameter = Med.Params[Index];
    // Generic ABI recovery fills unused register gaps with Id=-1. Such a
    // placeholder proves neither a type nor an argument; retain only observed
    // inputs, with their original physical register positions unchanged.
    if (Parameter.Id < 0)
      continue;
    const auto &Type = Med.TypedParams[Index].Type;
    if (Parameter.Kind != MedVar::Param || !integerCarrier(Type) ||
        Type->Size != Parameter.Size)
      return Reject("native parameter lacks a scalar machine carrier");
    SourceParameterTypeHint Source;
    Source.Name = "native_arg" + std::to_string(Hint.Parameters.size());
    Source.Type = Type;
    Source.Location.ValueBytes = Type->Size;
    if (Parameter.RegOff == kNoParamReg) {
      // This is the generic stack recovery contract, not an inferred C
      // parameter index. A narrow SUBBYTES use can represent several packed
      // arm64 values in one slot, so it requires a separate range analysis.
      const int RegisterCount = static_cast<int>(TRI.IntParamRegs.size());
      if (Parameter.Size != 8 || Parameter.Id < RegisterCount ||
          Parameter.Id >= RegisterCount + 512)
        return Reject("native stack parameter has no complete slot evidence");
      Source.Location.Kind = SourceABICarrierKind::Stack;
      Source.Location.EntryStackOffset =
          (Image.Arch == Arch::X64 ? 8 : 0) +
          int64_t(Parameter.Id - RegisterCount) * 8;
      if (!StackSlots.insert(Parameter.Id).second)
        return Reject("native stack parameters overlap");
    } else {
      if (std::find(TRI.IntParamRegs.begin(), TRI.IntParamRegs.end(),
                    Parameter.RegOff) == TRI.IntParamRegs.end() ||
          !ParameterRegisters.insert(Parameter.RegOff).second)
        return Reject("native parameter register is ambiguous or non-integer");
      Source.Location.Kind = SourceABICarrierKind::IntegerRegister;
      Source.Location.RegisterOffset = Parameter.RegOff;
    }
    Hint.Parameters.push_back(std::move(Source));
  }
  if (!Med.MutableStackParamHomes.empty()) {
    // Mutable homes are stored by parameter index, whereas ordinary stack
    // Params use ABI slot IDs. Do not combine these coordinate systems.
    return Reject("native mutable stack parameters require range recovery");
  }

  bool HasReturn = false;
  for (const auto &Block : Med.Blocks) {
    if (!Block.ExceptionalSuccs.empty() || !Block.ExceptionalPreds.empty())
      return Reject("native exception-dependent parameters are unsupported");
    const MedOp *ReturnDefinition = nullptr;
    for (const auto &Op : Block.Ops) {
      if (Op.Opcode == NdOp::INTRINSIC)
        return Reject("native intrinsic requires explicit scalar ABI evidence");
      if (Op.Opcode == NdOp::CALL || Op.Opcode == NdOp::INDIR_CALL) {
        std::string Error;
        if (!Op.SourceCallHint ||
            !validateSourceABI(Op.SourceCallHint->Signature, Error) ||
            Op.NumInputs != Op.SourceCallHint->Signature.Parameters.size() + 1)
          return Reject(
              "native function calls a target without a source binding");
        ReturnDefinition = nullptr;
      }
      for (uint8_t I = 0; I < Op.NumInputs; ++I) {
        const auto &Input = Op.Inputs[I];
        if (Input.Kind != MedVar::Param)
          continue;
        if (Input.RegOff != kNoParamReg) {
          if (!ParameterRegisters.count(Input.RegOff))
            return Reject("native body uses an unbound input register");
          continue;
        }
        if (!StackSlots.count(Input.Id) || Input.Size != 8 ||
            (Op.Opcode == NdOp::SUBBYTES &&
             (I != 0 || Op.NumInputs != 2 || !Op.Inputs[1].isConst() ||
              Op.Inputs[1].ConstVal != 0 || Op.Output.Size != 8)))
          return Reject("native stack slot is partial or has an unknown range");
      }
      const bool SelfCopy = Op.Opcode == NdOp::COPY && Op.NumInputs == 1 &&
                            Op.Output == Op.Inputs[0];
      if (!SelfCopy && Op.Opcode != NdOp::SUBBYTES &&
          Op.Output.Kind == MedVar::Reg &&
          Op.Output.RegOff == TRI.IntReturnReg && Op.Output.Size)
        ReturnDefinition = &Op;
      if (Op.Opcode == NdOp::RETURN) {
        HasReturn = true;
        if (!ReturnDefinition ||
            ReturnDefinition->Output.Size < Hint.ReturnType->Size)
          return Reject(
              "native result has no in-block computed register value");
        // A stack-pop into RAX is cleanup, not evidence of a source result.
        if (Image.Arch == Arch::X64 && ReturnDefinition->Opcode == NdOp::LOAD &&
            ReturnDefinition->NumInputs == 1 &&
            ReturnDefinition->Inputs[0].Kind == MedVar::Reg &&
            ReturnDefinition->Inputs[0].RegOff == TRI.StackPointer)
          return Reject("native result is an epilogue register restore");
      }
    }
    for (const auto &Phi : Block.Phis)
      for (const auto &[Predecessor, Input] : Phi.Args)
        if (Input.Kind == MedVar::Param && Input.RegOff == kNoParamReg)
          return Reject("native stack parameter PHI requires range recovery");
  }
  if (!HasReturn)
    return Reject("native function has no machine return");
  if (!validateSourceABI(Hint, Diagnostic))
    return std::nullopt;
  return Hint;
}
} // namespace neverd
