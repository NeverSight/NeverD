//===- MedLLVMIntrinsic.cpp - INTRINSIC dispatch & inline asm ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// INTRINSIC opcode dispatch and inline-asm helper.  Architecture-specific
/// side-effect emitters live in X86/MedLLVMX86Sideeffect.cpp,
/// AArch64/MedLLVMAArch64Sideeffect.cpp, and ARM/MedLLVMARMSideeffect.cpp.
/// Value-producing emitters in X86/MedLLVMX86ValueEmitter.cpp,
/// AArch64/MedLLVMAArch64ValueEmitter.cpp, and ARM/MedLLVMARMValueEmitter.cpp.
/// x86 SIMD intrinsics live in X86/MedLLVMX86SimdEmitter.cpp.
///
//===----------------------------------------------------------------------===//

#include "neverd/backend/llvm/MedLLVMEmitter.h"

#define DEBUG_TYPE "neverd-med-llvm-intrinsic"
#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/support/Diagnostic.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

namespace neverd {

std::optional<uint8_t>
MedLLVMEmitter::atomicIntrinsicAddressInput(const MedOp &Op) const {
  if (TargetArch != Arch::AArch64 || Op.NumInputs == 0 ||
      !Op.Inputs[0].isConst())
    return std::nullopt;

  using I = Intrinsic;
  const auto IC = static_cast<I>(Op.Inputs[0].ConstVal);
  switch (IC) {
  case I::A64_AtomicAnd:
  case I::A64_AtomicOr:
  case I::A64_AtomicXor:
  case I::A64_AtomicSmax:
  case I::A64_AtomicSmin:
  case I::A64_AtomicUmax:
  case I::A64_AtomicUmin:
    return Op.NumInputs >= 3 ? std::optional<uint8_t>(2) : std::nullopt;
  case I::A64_Ldxr:
  case I::A64_Ldaxr:
  case I::A64_Ldxp:
  case I::A64_Ldaxp:
    return Op.NumInputs >= 2 ? std::optional<uint8_t>(1) : std::nullopt;
  case I::A64_Stxr:
  case I::A64_Stlxr:
  case I::A64_Stxp:
  case I::A64_Stlxp:
    return Op.NumInputs >= 3 ? std::optional<uint8_t>(2) : std::nullopt;
  case I::A64_Rcwcasp:
  case I::A64_Rcwcaspa:
  case I::A64_Rcwcaspal:
  case I::A64_Rcwcaspl:
  case I::A64_Rcwscasp:
  case I::A64_Rcwscaspa:
  case I::A64_Rcwscaspal:
  case I::A64_Rcwscaspl:
    return Op.NumInputs >= 4 ? std::optional<uint8_t>(3) : std::nullopt;
  case I::A64_Ldclrp:
  case I::A64_Ldclrpa:
  case I::A64_Ldclrpal:
  case I::A64_Ldclrpl:
  case I::A64_Ldsetp:
  case I::A64_Ldsetpa:
  case I::A64_Ldsetpal:
  case I::A64_Ldsetpl:
    return Op.NumInputs >= 3 ? std::optional<uint8_t>(2) : std::nullopt;
  default:
    return std::nullopt;
  }
}

//===----------------------------------------------------------------------===//
// INTRINSIC dispatch
//===----------------------------------------------------------------------===//

llvm::Value *MedLLVMEmitter::emitIntrinsic(const MedOp &Op,
                                           llvm::IRBuilder<> &Builder) {
  uint16_t IntrCode = 0;
  if (Op.NumInputs > 0 && Op.Inputs[0].isConst())
    IntrCode = static_cast<uint16_t>(Op.Inputs[0].ConstVal);

  using I = Intrinsic;
  auto IC = static_cast<I>(IntrCode);

  if (emitSideeffectIntrinsic(Op, IC, Builder))
    return nullptr;

  if (Op.Output.Size == 0)
    return nullptr;

  auto *OutTy = sizeToType(Op.Output.Size);

  llvm::Value *R = nullptr;

  switch (TargetArch) {
  case Arch::X86:
  case Arch::X64:
    if ((R = emitAesIntrinsic(Op, IC, Builder)))
      return R;
    if ((R = emitShaIntrinsic(Op, IC, Builder)))
      return R;
    if ((R = emitGfniIntrinsic(Op, IC, Builder)))
      return R;
    if ((R = emitShuffleIntrinsic(Op, IC, Builder)))
      return R;
    if ((R = emitPackedShift(Op, IC, Builder)))
      return R;
    if ((R = emitMiscSimd(Op, IC, Builder)))
      return R;
    if ((R = emitX86IntrinsicValue(Op, IC, Builder)))
      return R;
    break;
  case Arch::AArch64:
    if ((R = emitAArch64IntrinsicValue(Op, IC, Builder)))
      return R;
    break;
  case Arch::ARM:
    if ((R = emitARMIntrinsicValue(Op, IC, Builder)))
      return R;
    break;
  default:
    break;
  }

  if (IC == I::Hlt) {
    auto *Fn = llvm::Intrinsic::getOrInsertDeclaration(
        Mod, llvm::Intrinsic::debugtrap);
    Builder.CreateCall(Fn, {});
    return (Op.Output.Size > 0) ? llvm::ConstantInt::get(OutTy, 0) : nullptr;
  }

  {
    // The bare-mnemonic inline-asm fallback is for VOID side-effect intrinsics
    // only (dsb/isb/wfe/...).  A value-producing intrinsic (Output.Size > 0)
    // reaching here has no real handler: emitting its mnemonic with bare `r`
    // operands yields malformed asm (e.g. a 1-operand `sel`/`ssat`) that aborts
    // codegen and crashes the whole process.  Fall through to the warning +
    // safe 0 so it surfaces as a test mismatch instead (see #270d ArmSsat, #280
    // ArmSel).
    const char *AsmStr = intrinsicAsmMnemonic(IC);
    if (AsmStr && Op.Output.Size == 0) {
      emitVoidInlineAsm(AsmStr, Op, Builder);
      return nullptr;
    }
  }

  syncWarning() << "INTRINSIC unhandled intrinsic: code=" << IntrCode << " ("
                << intrinsicName(IC) << ") out_sz=" << Op.Output.Size
                << " n_in=" << static_cast<unsigned>(Op.NumInputs) << "\n";
  if (Op.Output.Size > 0)
    ++UnhandledValueIntrinsicCount;
  return (Op.Output.Size > 0) ? llvm::ConstantInt::get(OutTy, 0) : nullptr;
}

//===----------------------------------------------------------------------===//
// Inline asm helper
//===----------------------------------------------------------------------===//

void MedLLVMEmitter::emitVoidInlineAsm(const char *Mnemonic, const MedOp &Op,
                                       llvm::IRBuilder<> &Builder) {
  auto *VoidTy = llvm::Type::getVoidTy(*Ctx);
  std::vector<llvm::Type *> ParamTys;
  std::vector<llvm::Value *> ParamVals;
  for (uint16_t I = 1; I < Op.NumInputs; ++I) {
    auto *V = getVar(Op.Inputs[I], Builder);
    ParamTys.push_back(V->getType());
    ParamVals.push_back(V);
  }
  std::string Cstr;
  for (size_t I = 0; I < ParamVals.size(); ++I)
    Cstr += (Cstr.empty() ? "r" : ",r");
  Cstr += (Cstr.empty() ? "~{memory}" : ",~{memory}");
  auto *AsmFnTy = llvm::FunctionType::get(VoidTy, ParamTys, false);
  auto *IA = llvm::InlineAsm::get(AsmFnTy, Mnemonic, Cstr, true);
  Builder.CreateCall(IA, ParamVals);
}

//===----------------------------------------------------------------------===//
// Side-effect intrinsic dispatcher
//===----------------------------------------------------------------------===//

bool MedLLVMEmitter::emitSideeffectIntrinsic(const MedOp &Op, Intrinsic IC,
                                             llvm::IRBuilder<> &Builder) {
  bool Matched = false;
  switch (TargetArch) {
  case Arch::X86:
  case Arch::X64:
    Matched = emitX86Sideeffect(Op, IC, Builder);
    break;
  case Arch::AArch64:
    Matched = emitAArch64Sideeffect(Op, IC, Builder);
    break;
  case Arch::ARM:
    Matched = emitARMSideeffect(Op, IC, Builder);
    break;
  default:
    break;
  }
  if (Matched && Op.Output.Size > 0) {
    auto *OutTy = sizeToType(Op.Output.Size);
    setVar(Op.Output, llvm::ConstantInt::get(OutTy, 0), Builder);
  }
  return Matched;
}

} // namespace neverd
