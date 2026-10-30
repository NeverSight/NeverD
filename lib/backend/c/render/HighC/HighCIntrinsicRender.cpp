//===- HighCIntrinsicRender.cpp - High IR intrinsic rendering -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Arch-dispatching entry points for HighIR intrinsic rendering.
/// Architecture-specific implementations live in
/// HighCIntrinsicRenderX86.cpp and HighCIntrinsicRenderARM.cpp.
///
//===----------------------------------------------------------------------===//

#include "neverd/backend/c/render/HighC/HighCIntrinsicRender.h"

#include "neverd/ir/intrinsics/AArch64/AArch64AddressMemory.h"
#include "neverd/ir/intrinsics/AArch64/AArch64ExceptionReturn.h"
#include "neverd/ir/intrinsics/AArch64/AArch64PAuthLoad.h"
#include "neverd/ir/intrinsics/AArch64/AArch64SVEVector.h"
#include "neverd/ir/intrinsics/AArch64/AArch64WritebackMemory.h"
#include "neverd/ir/intrinsics/X86/X86Unsupported.h"

#include <stdexcept>
#include <string>

namespace neverd {

std::string MultiOutputRender::operator()(
    Arch TheArch, Intrinsic IID, const std::vector<MedVar> &Outputs,
    const std::vector<ExprPtr> &Operands,
    std::function<std::string(const HighExpr &)> ExprFn,
    std::function<std::string(const MedVar &)> VarFn, IsAliveFn IsAlive) const {
  if (isAArch64WritebackMemoryIntrinsic(IID) && TheArch != Arch::AArch64)
    throw std::invalid_argument(kWritebackMalformed);
  if (isAArch64ExceptionReturnIntrinsic(IID))
    throw std::invalid_argument(kAArch64ExceptionReturnUnsupported);
  if (const char *Reason = x86NonlocalControlTransferReason(IID))
    throw std::invalid_argument(Reason);
  if (IID == Intrinsic::RawEnvironmentRequirementV1 ||
      IID == Intrinsic::ArchitecturalValueV1)
    throw std::invalid_argument(
        "raw environment requirement has no C runtime contract");
  if (TheArch == Arch::ARM || TheArch == Arch::AArch64)
    return renderARMMultiOutput(IID, Outputs, Operands, ExprFn, VarFn, IsAlive);
  return renderX86MultiOutput(IID, Outputs, Operands, ExprFn, VarFn, IsAlive,
                              GnuToolchain);
}

std::string renderIntrinsicCall(Intrinsic Id, Arch TheArch,
                                const std::vector<std::string> &Ops,
                                uint16_t ResultBytes, bool &HasCIntrinsics,
                                bool GnuToolchain,
                                llvm::ArrayRef<uint16_t> OperandBytes) {
  if (isAArch64ExceptionReturnIntrinsic(Id))
    throw std::invalid_argument(kAArch64ExceptionReturnUnsupported);
  if (const char *Reason = x86NonlocalControlTransferReason(Id))
    throw std::invalid_argument(Reason);
  if (Id == Intrinsic::RawEnvironmentRequirementV1 ||
      Id == Intrinsic::ArchitecturalValueV1)
    throw std::invalid_argument(
        "raw environment requirement has no C runtime contract");
  if (aarch64_sve::vectorOperation(Id) && TheArch != Arch::AArch64)
    throw std::invalid_argument(aarch64_sve::kMalformedVector);
  if (isAArch64AddressMemoryIntrinsic(Id) && TheArch != Arch::AArch64)
    throw std::invalid_argument(kAddressMemoryMalformed);
  if (isAArch64WritebackMemoryIntrinsic(Id) && TheArch != Arch::AArch64)
    throw std::invalid_argument(kWritebackMalformed);
  if (isAArch64PAuthLoadIntrinsic(Id) && TheArch != Arch::AArch64)
    throw std::invalid_argument(kPAuthLoadMalformed);
  std::string Result;
  if (TheArch == Arch::ARM || TheArch == Arch::AArch64) {
    if (TheArch == Arch::AArch64 && GnuToolchain)
      if (std::string Half = renderA64FixedHalfConversion(Id, Ops);
          !Half.empty()) {
        HasCIntrinsics = true;
        return Half;
      }
    Result = renderARMIntrinsicCall(Id, Ops, ResultBytes, HasCIntrinsics,
                                    OperandBytes);
    if (Result.empty() && aarch64_sve::vectorOperation(Id))
      throw std::invalid_argument(aarch64_sve::kMalformedVector);
    if (Result.empty())
      Result = renderNeonVectorIntrinsic(Id, Ops, OperandBytes, ResultBytes,
                                         HasCIntrinsics);
    if (!Result.empty())
      return Result;
  } else {
    Result = renderX86IntrinsicCall(Id, Ops, HasCIntrinsics, GnuToolchain);
    if (!Result.empty())
      return Result;
  }

  const char *CName = intrinsicCName(Id);
  if (TheArch == Arch::X86 || TheArch == Arch::X64)
    if (const char *Sized = x86SizedIntrinsicCName(Id, ResultBytes))
      CName = Sized;
  if (CName) {
    std::string CallStr = std::string(CName) + "(";
    for (size_t I = 0; I < Ops.size(); ++I) {
      if (I > 0)
        CallStr += ", ";
      CallStr += Ops[I];
    }
    CallStr += ")";
    return CallStr;
  }

  const char *AsmMn = intrinsicAsmMnemonic(Id);
  if (AsmMn) {
    if (TheArch == Arch::X64 || TheArch == Arch::X86) {
      if (GnuToolchain)
        if (std::string Operand =
                renderX86SystemRegisterOperand(Id, Ops, ResultBytes);
            !Operand.empty())
          return Operand;
      return renderX86AsmStatement(AsmMn, Ops);
    }
    return renderARMAsmStatement(AsmMn, Ops);
  }

  return {};
}

} // namespace neverd
