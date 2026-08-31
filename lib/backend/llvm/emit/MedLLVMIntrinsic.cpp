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
#include "neverd/ir/med/IntrinsicShapes.h"
#include "neverd/support/Diagnostic.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/ErrorHandling.h"
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
  if (Op.NumInputs == 0 || !Op.Inputs[0].isConst())
    llvm::report_fatal_error("intrinsic has no constant intrinsic ID");
  const auto IntrCode = static_cast<uint16_t>(Op.Inputs[0].ConstVal);

  using I = Intrinsic;
  auto IC = static_cast<I>(IntrCode);

  if (IC == I::X86RequireDivPrecondition && TargetArch != Arch::X86 &&
      TargetArch != Arch::X64)
    llvm::report_fatal_error(
        "x86 divide precondition requires an x86 target");

  // LowIR carries AMX as exact 64-byte TILECFG and 1-KiB tile values.  A
  // faithful compiled lowering additionally needs checked strided bulk memory,
  // restart progress in TILECFG.start_row, and an exception continuation that
  // publishes partially completed architectural state.  The sealed runtime
  // ABI currently exposes only scalar memory helpers and no tile-state slots;
  // declaring an ambient helper here would bypass that ABI and turn faults or
  // cross-function tile state into guesses.  Reject the complete family before
  // any target handler can emit an observable instruction.  Once a versioned
  // runtime contract owns those facilities, this single guard is the handoff
  // point for an exact AMX value/side-effect emitter.
  if (isAMXIntrinsic(IC))
    llvm::report_fatal_error(
        "AMX LLVM lowering requires a versioned tile-state, "
        "restartable-memory, and fault-continuation runtime ABI");

  if (isPdepPextIntrinsic(IC) &&
      !intrinsicPdepPextShapeIsValid(IC, pdepPextMedShape(Op)))
    llvm::report_fatal_error(
        "PDEP/PEXT intrinsic has an invalid operand/output contract");

  // LLVM has no stable target-independent representation for the contiguous
  // packed-address fault contract used by EVEX compress/expand in the LLVM
  // version supported by this tree.  Never turn a memory effect into zero.
  if (IC == I::EVEXCompressStore || IC == I::EVEXExpandLoad)
    llvm::report_fatal_error(
        "EVEX compress/expand memory lowering is not available");

  // The supported LLVM fork has no stable intrinsic for VDBPSADBW's four
  // independent immediate selectors.  Returning the generic zero fallback
  // would silently change program semantics, so keep this exact lift closed
  // until a target-independent lowering is provided.
  if (IC == I::Vdbpsadbw)
    llvm::report_fatal_error("VDBPSADBW LLVM lowering is not available");
  if (IC == I::X86FourFMA)
    llvm::report_fatal_error("x86 four-iteration FMA lowering is not available");
  if (isX86VP4DPIntrinsic(IC))
    llvm::report_fatal_error(
        "x86 VP4DP word dot-product lowering is not available");
  if (IC == I::ApxRaoAdd || IC == I::ApxRaoAnd || IC == I::ApxRaoOr ||
      IC == I::ApxRaoXor || IC == I::ApxCmpccXadd)
    llvm::report_fatal_error("APX atomic LLVM lowering is not available");

  if (Op.MemoryAddressSpace != NdMemoryAddressSpace::Default) {
    if (TargetArch != Arch::X86 && TargetArch != Arch::X64)
      llvm::report_fatal_error(
          "FS/GS memory address spaces require an x86 target");
    if (!intrinsicSupportsMemoryAddressSpace(IC))
      llvm::report_fatal_error(
          "intrinsic does not support a memory address space");
    if (!intrinsicMemoryAddressSpaceShapeIsValid(IC, Op.NumInputs,
                                                 Op.Output.Size,
                                                 Op.NumInputs > 1
                                                     ? Op.Inputs[1].Size
                                                     : 0,
                                                 Op.NumInputs > 2
                                                     ? Op.Inputs[2].Size
                                                     : 0,
                                                 Op.NumInputs > 3
                                                     ? Op.Inputs[3].Size
                                                     : 0))
      llvm::report_fatal_error(
          "segmented-memory intrinsic has an invalid operand/output shape");
  }

  if (emitSideeffectIntrinsic(Op, IC, Builder))
    return nullptr;

  // Masked stores are value-dispatch intrinsics with a void result.  They must
  // be dispatched before the generic zero-output early return; otherwise the
  // observable store is silently dropped.  The arity check also makes the
  // handler's nullptr-success convention unambiguous.
  if ((TargetArch == Arch::X86 || TargetArch == Arch::X64) &&
      (IC == I::MaskedStoreB || IC == I::MaskedStoreW ||
       IC == I::MaskedStoreD || IC == I::MaskedStoreQ)) {
    if (Op.NumInputs < 4 || Op.Output.Size != 0)
      llvm::report_fatal_error(
          "masked-store intrinsic has an invalid operand/output shape");
    (void)emitMaskedMemOp(Op, IC, Builder);
    return nullptr;
  }

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

  if (Op.MemoryAddressSpace != NdMemoryAddressSpace::Default)
    llvm::report_fatal_error(
        "segmented-memory intrinsic was not handled by the target backend");

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
