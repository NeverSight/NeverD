//===- SBFSemantics.cpp - Normalized Solana SBF semantics -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/sbf/runtime/SBFSemantics.h"

#include "llvm/Support/ErrorHandling.h"

#include <bit>
#include <type_traits>

namespace neverd::sbf {
namespace {

template <typename UInt>
std::optional<uint64_t> evaluateScalarALUImpl(Operation Op, UInt Left,
                                              UInt Right, bool SwapOperands) {
  constexpr unsigned Width = sizeof(UInt) * kBitsPerByte;
  switch (Op) {
  case Operation::Add:
    return static_cast<UInt>(Left + Right);
  case Operation::Sub:
    return static_cast<UInt>(SwapOperands ? Right - Left : Left - Right);
  case Operation::Mul:
    return static_cast<UInt>(Left * Right);
  case Operation::Or:
    return Left | Right;
  case Operation::And:
    return Left & Right;
  case Operation::Xor:
    return Left ^ Right;
  case Operation::LSh:
    return static_cast<UInt>(Left << (Right & (Width - 1)));
  case Operation::RSh:
    return Left >> (Right & (Width - 1));
  case Operation::ARSh:
    return static_cast<UInt>(std::bit_cast<std::make_signed_t<UInt>>(Left) >>
                             (Right & (Width - 1)));
  case Operation::Neg:
    return static_cast<UInt>(UInt{0} - Left);
  default:
    return std::nullopt;
  }
}

template <typename UInt>
bool evaluateBranchConditionImpl(Operation Op, UInt Left, UInt Right) {
  const auto Signed = [](UInt Value) {
    return std::bit_cast<std::make_signed_t<UInt>>(Value);
  };
  switch (Op) {
  case Operation::Eq:
    return Left == Right;
  case Operation::Ne:
    return Left != Right;
  case Operation::UGt:
    return Left > Right;
  case Operation::UGe:
    return Left >= Right;
  case Operation::ULt:
    return Left < Right;
  case Operation::ULe:
    return Left <= Right;
  case Operation::SGt:
    return Signed(Left) > Signed(Right);
  case Operation::SGe:
    return Signed(Left) >= Signed(Right);
  case Operation::SLt:
    return Signed(Left) < Signed(Right);
  case Operation::SLe:
    return Signed(Left) <= Signed(Right);
  case Operation::Set:
    return (Left & Right) != 0;
  default:
    llvm_unreachable("expected a normalized SBF conditional operation");
  }
}

} // namespace

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

std::optional<uint64_t> evaluateScalarALU(Operation Op, unsigned Width,
                                          uint64_t Left, uint64_t Right,
                                          bool SwapOperands) {
  switch (Width) {
  case kWordBitWidth:
    return evaluateScalarALUImpl(Op, static_cast<uint32_t>(Left),
                                 static_cast<uint32_t>(Right), SwapOperands);
  case kDoubleWordBitWidth:
    return evaluateScalarALUImpl(Op, Left, Right, SwapOperands);
  default:
    return std::nullopt;
  }
}

bool evaluateBranchCondition(Operation Op, unsigned Width, uint64_t Left,
                             uint64_t Right) {
  switch (Width) {
  case kWordBitWidth:
    return evaluateBranchConditionImpl(Op, static_cast<uint32_t>(Left),
                                       static_cast<uint32_t>(Right));
  case kDoubleWordBitWidth:
    return evaluateBranchConditionImpl(Op, Left, Right);
  default:
    llvm_unreachable("expected a normalized SBF comparison width");
  }
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
