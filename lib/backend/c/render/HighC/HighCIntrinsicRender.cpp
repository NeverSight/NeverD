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

#include "neverd/backend/c/render/CTypeFormat.h"
#include "neverd/ir/intrinsics/AArch64/AArch64AddressMemory.h"
#include "neverd/ir/intrinsics/AArch64/AArch64ExceptionReturn.h"
#include "neverd/ir/intrinsics/AArch64/AArch64PAuthLoad.h"
#include "neverd/ir/intrinsics/AArch64/AArch64SVEVector.h"
#include "neverd/ir/intrinsics/AArch64/AArch64WritebackMemory.h"
#include "neverd/ir/intrinsics/X86/X86Unsupported.h"

#include "llvm/Support/ErrorHandling.h"

#include <stdexcept>
#include <string>

namespace neverd {

namespace {
/// An alignment precondition (RequireAligned): the access faults, here a
/// trap, when its address is not a multiple of the alignment and, given a
/// third operand, that predicate holds.  An address relative to FS or GS
/// needs the segment's base, which C does not have.
std::string renderAlignmentPrecondition(
    const HighExpr &Call, std::function<std::string(const HighExpr &)> ExprFn) {
  const size_t Count = Call.Operands.size();
  const HighExpr *Address = Count > 0 ? Call.Operands[0].get() : nullptr;
  const HighExpr *Alignment = Count > 1 ? Call.Operands[1].get() : nullptr;
  const HighExpr *Active = Count > 2 ? Call.Operands[2].get() : nullptr;
  if ((Count != 2 && Count != 3) || !Address || !Alignment ||
      Alignment->Kind != ExprKind::Const || (Count == 3 && !Active))
    llvm::report_fatal_error("invalid alignment precondition intrinsic");
  if (Call.MemoryAddressSpace != NdMemoryAddressSpace::Default)
    llvm::report_fatal_error("segmented alignment precondition requires an "
                             "architectural FS/GS base");
  const uint64_t Bytes = Alignment->ConstVal;
  if (Bytes == 0 || (Bytes & (Bytes - 1)) != 0)
    llvm::report_fatal_error("alignment precondition is not a power of two");
  // An 8-byte integer masks alike signed or not; a pointer, or anything
  // narrower, converts first.  An operator binding looser than `&` keeps its
  // parentheses.
  std::string Value = ExprFn(*Address);
  // So does an address the text spells as a pointer: a string literal, or
  // an object's `&`.
  if (!Address->Type || Address->Type->Kind != NdTypeKind::Int ||
      Address->Type->Size != 8 || isStringLiteralText(Value) ||
      llvm::StringRef(Value).starts_with("&"))
    Value = "(uint64_t)(" + Value + ")";
  else if (Address->Kind != ExprKind::Var && Address->Kind != ExprKind::Const &&
           Address->Kind != ExprKind::Cast)
    Value = "(" + Value + ")";
  std::string Condition =
      "(" + Value + " & " + std::to_string(Bytes - 1) + ") != 0";
  if (Active)
    Condition = "(" + ExprFn(*Active) + ") && " + Condition;
  return "if (" + Condition + ")\n    __builtin_trap();\n";
}

} // namespace

std::string renderMemoryIntrinsicStatement(
    Arch TheArch, const HighExpr &Call, const HighExpr *PrimaryDst,
    std::function<std::string(const HighExpr &)> ExprFn,
    std::function<std::string(const MedVar &)> VarFn, IsAliveFn IsAlive,
    SameWidthUnsignedFn SameWidthUnsigned, bool MsvcIntrinsics,
    DisjointMemoryFn Disjoint) {
  if (Call.Kind == ExprKind::Call &&
      Call.IntrinsicId == Intrinsic::RequireAligned)
    return renderAlignmentPrecondition(Call, std::move(ExprFn));
  return renderX86SegmentedIntrinsicStatement(
      TheArch, Call, PrimaryDst, std::move(ExprFn), std::move(VarFn),
      std::move(IsAlive), std::move(SameWidthUnsigned), MsvcIntrinsics,
      std::move(Disjoint));
}

std::string MultiOutputRender::operator()(
    Arch TheArch, Intrinsic IID, const std::vector<MedVar> &Outputs,
    const std::vector<ExprPtr> &Operands,
    std::function<std::string(const HighExpr &)> ExprFn,
    std::function<std::string(const MedVar &)> VarFn, IsAliveFn IsAlive) const {
  if (IID == Intrinsic::Eret)
    throw std::invalid_argument(kAArch64ExceptionReturnMalformed);
  if (isAArch64WritebackMemoryIntrinsic(IID) && TheArch != Arch::AArch64)
    throw std::invalid_argument(kWritebackMalformed);
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
  if (Id == Intrinsic::Eret &&
      (TheArch != Arch::AArch64 ||
       !aarch64ExceptionReturnShapeIsValid(Id, Ops.size() + 1, ResultBytes)))
    throw std::invalid_argument(kAArch64ExceptionReturnMalformed);
  if ((Id == Intrinsic::A64_Wrffr || Id == Intrinsic::A64_Rdffr ||
       Id == Intrinsic::A64_Setffr) &&
      TheArch != Arch::AArch64)
    throw std::invalid_argument("FFR transfers require AArch64");
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
