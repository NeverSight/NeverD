//===- Semantics.cpp - Normalized Solana SBF semantics -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/sbf/Semantics.h"

#include <bit>

namespace neverd::sbf {

SemanticTraits semanticTraits(const OpcodeInfo &Info, Version V) {
  SemanticTraits Result;
  Result.WritesDestination = Info.writesDestinationRegister();
  if (Info.ID == Opcode::LDDW)
    Result.Immediate = ImmediateExtension::Full64;
  else if (versionHasFeature(V, VersionFeature::PQR) &&
           (Info.Op == Operation::UHighMul || Info.Op == Operation::UDiv ||
            Info.Op == Operation::URem || Info.Op == Operation::HighOr))
    Result.Immediate = ImmediateExtension::Zero32;

  if (Info.Width == kWordBitWidth && Result.WritesDestination) {
    if ((Info.ID == Opcode::MOV32_REG &&
         versionHasFeature(V, VersionFeature::ExplicitSignExtension)) ||
        ((Info.Op == Operation::Add || Info.Op == Operation::Sub ||
          Info.Op == Operation::Mul) &&
         !versionHasFeature(V, VersionFeature::ExplicitSignExtension)))
      Result.Result = ResultExtension::Sign32;
    else
      Result.Result = ResultExtension::Zero32;
  }

  switch (Info.Form) {
  case OperandForm::DstSrc:
  case OperandForm::Load:
  case OperandForm::BranchReg:
  case OperandForm::StoreReg:
    Result.Source = OperandSourceKind::SourceRegister;
    break;
  case OperandForm::CallReg:
    Result.Source = OperandSourceKind::VersionedCallRegister;
    break;
  case OperandForm::None:
  case OperandForm::Dst:
  case OperandForm::Branch:
    Result.Source = OperandSourceKind::None;
    break;
  default:
    Result.Source = OperandSourceKind::Immediate;
    break;
  }

  if (Info.Op == Operation::UDiv || Info.Op == Operation::URem)
    Result.Faults = FaultPolicy::DivideByZero;
  else if (Info.Op == Operation::SDiv || Info.Op == Operation::SRem)
    Result.Faults = FaultPolicy::DivideByZero | FaultPolicy::DivideOverflow;
  else if (Info.Op == Operation::Load || Info.Op == Operation::Store)
    Result.Faults = FaultPolicy::MemoryAccess;
  else if (Info.isCall())
    Result.Faults = FaultPolicy::Call;

  if (Info.Op == Operation::Load || Info.Op == Operation::Store)
    Result.MemoryWidth = Info.Width;
  Result.SwapOperands = Info.Op == Operation::Sub &&
                        Info.Form == OperandForm::DstImm &&
                        versionHasFeature(V, VersionFeature::SwapSubImmediate);

  if (Info.Op == Operation::Jump)
    Result.Terminator = TerminatorKind::Jump;
  else if (Info.isConditionalBranch())
    Result.Terminator = TerminatorKind::ConditionalJump;
  else if (Info.isCall())
    Result.Terminator = TerminatorKind::Call;
  else if (Info.isExit())
    Result.Terminator = TerminatorKind::Return;
  return Result;
}

uint64_t normalizeImmediate(uint64_t Immediate, ImmediateExtension Extension) {
  switch (Extension) {
  case ImmediateExtension::Zero32:
    return static_cast<uint32_t>(Immediate);
  case ImmediateExtension::Full64:
    return Immediate;
  case ImmediateExtension::Sign32:
    return static_cast<uint64_t>(static_cast<int64_t>(
        std::bit_cast<int32_t>(static_cast<uint32_t>(Immediate))));
  }
  return Immediate;
}

uint64_t extendALU32Result(uint32_t Value, ResultExtension Extension) {
  if (Extension == ResultExtension::Sign32)
    return static_cast<uint64_t>(
        static_cast<int64_t>(std::bit_cast<int32_t>(Value)));
  return Value;
}

int64_t callxRegisterIndex(Version V, uint8_t Dst, uint8_t Src,
                           int32_t Immediate) {
  if (versionHasFeature(V, VersionFeature::CallXSource))
    return Src;
  if (versionHasFeature(V, VersionFeature::CallXDestination))
    return Dst;
  return Immediate;
}

} // namespace neverd::sbf
