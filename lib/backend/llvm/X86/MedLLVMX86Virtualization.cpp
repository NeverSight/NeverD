//===- MedLLVMX86Virtualization.cpp - VMX/SVM lowering -------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Emit instruction-owned memory accesses and actual VMX completion flags.
/// Ordinary execution still requires the instruction's architectural privilege
/// and processor state; the emitter does not simulate a VMCS or assume success.
//===----------------------------------------------------------------------===//

#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/backend/llvm/X86/X86VirtualizationNative.h"
#include "neverd/ir/med/IntrinsicShapes.h"

#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/Support/ErrorHandling.h"

#include <vector>

namespace neverd {

llvm::Value *MedLLVMEmitter::emitX86Virtualization(const MedOp &Op,
                                                   Intrinsic Id,
                                                   llvm::IRBuilder<> &Builder) {
  if (!x86VirtualizationShapeIsValid(Id,
                                     x86VirtualizationMedShape(Op, TargetArch)))
    llvm::report_fatal_error(
        llvm::Twine("x86 virtualization intrinsic has an invalid "
                    "operand/output contract: id=") +
        llvm::Twine(static_cast<unsigned>(Id)) +
        " arch=" + llvm::Twine(static_cast<unsigned>(TargetArch)) +
        " output=" + llvm::Twine(Op.Output.Size) +
        " inputs=" + llvm::Twine(static_cast<unsigned>(Op.NumInputs)));
  const auto C = x86VirtualizationContract(Id);
  if (C.Result == X86VirtualizationResult::Value) {
    const auto Word = Op.Output.Size;
    auto *Type = sizeToType(Word);
    std::vector<llvm::Value *> Arguments;
    for (unsigned I = 1; I != Op.NumInputs; ++I)
      Arguments.push_back(getVar(Op.Inputs[I], Builder));
    const std::string Name =
        std::string("llvm.x86.") + C.Mnemonic + (Word == 4 ? "32" : "");
    auto Callee = Mod->getOrInsertFunction(
        Name, llvm::FunctionType::get(Type, std::vector<llvm::Type *>(5, Type),
                                      false));
    auto *Function = llvm::cast<llvm::Function>(Callee.getCallee());
    Function->setMemoryEffects(llvm::MemoryEffects::unknown());
    Function->addFnAttr(llvm::Attribute::Convergent);
    Function->addFnAttr(llvm::Attribute::NoDuplicate);
    Function->addFnAttr(llvm::Attribute::NoUnwind);
    return Builder.CreateCall(Callee, Arguments, "kvm.status");
  }
  if (Id == Intrinsic::X86SvmRunContext) {
    auto *Pointer = resolveAtomicMemoryPtr(Op.Inputs[1], C.MemoryBytes,
                                           Builder.getInt8Ty(), Builder);
    auto *Type = llvm::FunctionType::get(
        Builder.getVoidTy(), {Builder.getPtrTy(), Builder.getInt32Ty()}, false);
    auto Callee = Mod->getOrInsertFunction("llvm.x86.vmrun.context", Type);
    auto *Function = llvm::cast<llvm::Function>(Callee.getCallee());
    Function->setMemoryEffects(llvm::MemoryEffects::unknown());
    Function->addFnAttr(llvm::Attribute::Convergent);
    Function->addFnAttr(llvm::Attribute::NoDuplicate);
    Function->addFnAttr(llvm::Attribute::NoUnwind);
    if (Function->getIntrinsicID())
      Function->addParamAttr(1, llvm::Attribute::ImmArg);
    // The old SDK may transport this extension into LLVM C. Native object
    // emission explicitly refuses it until its backend recognizes the API.
    Builder.CreateCall(Callee,
                       {Pointer, Builder.getInt32(Op.Inputs[2].ConstVal)});
    return nullptr;
  }
  const uint16_t Word = TargetArch == Arch::X64 ? 8 : 4;
  const auto P =
      x86VirtualizationAsm(Id, TargetArch, Op.MemoryAddressSpace,
                           C.PhysicalAddress ? Op.Inputs[1].Size : 0);
  if (!P)
    llvm::report_fatal_error(
        "unsupported x86 virtualization assembly contract");
  std::vector<llvm::Value *> Arguments;
  std::vector<llvm::Type *> Types;
  for (uint8_t I = 0; I < C.Arguments; ++I) {
    llvm::Value *Value = nullptr;
    if (I == C.MemoryArgument) {
      const uint16_t AccessBytes = C.MemoryBytes ? C.MemoryBytes : Word;
      Value = resolveAtomicMemoryPtr(Op.Inputs[I + 1], AccessBytes,
                                     sizeToType(AccessBytes), Builder,
                                     Op.MemoryAddressSpace);
    } else
      Value = getVar(Op.Inputs[I + 1], Builder);
    auto *Type = sizeToType(P->InputBytes[I]);
    const bool Address = I == C.MemoryArgument &&
                         Op.MemoryAddressSpace == NdMemoryAddressSpace::Default;
    // The pointer escape and memory clobber invalidate previous frame values.
    if (Address)
      Type = Value->getType();
    else if (Value->getType()->isPointerTy())
      Value = Builder.CreatePtrToInt(Value, Type);
    else if (Value->getType() != Type)
      Value = Builder.CreateZExtOrTrunc(Value, Type);
    Arguments.push_back(Value);
    Types.push_back(Type);
  }
  llvm::Value *Value = nullptr;
  llvm::Value *Flags = nullptr;
  const auto Native =
      x86VirtualizationNative(Id, TargetArch, Op.MemoryAddressSpace,
                              C.PhysicalAddress ? Op.Inputs[1].Size : 0);
  // The production SDK remains pinned until the complete instruction release.
  // This marker identifies the fork's qualified VMX/SVM interfaces; older
  // SDKs retain the exact assembly contract rather than declaring fake APIs.
  if (Native && llvm::Intrinsic::lookupIntrinsicID("llvm.x86.vmread.address")) {
    const auto IID = llvm::Intrinsic::lookupIntrinsicID(Native->LLVMName);
    if (!IID)
      llvm::report_fatal_error("native virtualization SDK is incomplete");
    if (Native->Result == X86VirtualizationResult::Read)
      Arguments.push_back(llvm::ConstantInt::get(sizeToType(Word), 0));
    if (Native->AddressControls) {
      // computeEA already performs architectural address-size truncation.
      // The offset remains separate from the native FS/GS base addition.
      Arguments.push_back(Builder.getInt32(8 * Word));
      Arguments.push_back(Builder.getInt32(Native->SegmentPrefix));
    }
    auto *Call = Builder.CreateCall(
        llvm::Intrinsic::getOrInsertDeclaration(Mod, IID), Arguments);
    if (Native->Result == X86VirtualizationResult::None)
      Flags = Builder.getInt16(0);
    else {
      llvm::Value *Status = Call;
      if (Native->Result == X86VirtualizationResult::Read) {
        Value = Builder.CreateExtractValue(Call, 0);
        Status = Builder.CreateExtractValue(Call, 1);
      }
      Status = Builder.CreateZExt(Status, Builder.getInt16Ty());
      Flags =
          Builder.CreateOr(Builder.CreateAnd(Builder.CreateLShr(Status, 1), 1),
                           Builder.CreateShl(Builder.CreateAnd(Status, 1), 8));
    }
  } else {
    llvm::Type *Return = Builder.getVoidTy();
    if (C.Result == X86VirtualizationResult::Read)
      Return =
          llvm::StructType::get(*Ctx, {sizeToType(Word), Builder.getInt16Ty()});
    else if (C.Result == X86VirtualizationResult::Flags)
      Return = Builder.getInt16Ty();
    auto *Signature = llvm::FunctionType::get(Return, Types, false);
    auto *Asm = llvm::InlineAsm::get(Signature, P->Text, P->Constraints, true);
    auto *Call = Builder.CreateCall(Asm, Arguments);
    if (C.Result == X86VirtualizationResult::Read) {
      Value = Builder.CreateExtractValue(Call, 0);
      Flags = Builder.CreateExtractValue(Call, 1);
    } else if (C.Result == X86VirtualizationResult::Flags)
      Flags = Call;
  }
  if (C.Result == X86VirtualizationResult::None)
    return nullptr;
  if (C.Result == X86VirtualizationResult::Flags)
    return Flags;
  // A failed VMREAD does not define a new architectural value. Only the
  // success arm can demand the physical output; the lifter merges the old
  // destination on failure without speculatively reading unknown old bits.
  Value =
      Builder.CreateSelect(Builder.CreateICmpEQ(Flags, Builder.getInt16(0)),
                           Value, llvm::ConstantInt::get(sizeToType(Word), 0));
  auto *Wide = sizeToType(2 * Word);
  return Builder.CreateOr(
      Builder.CreateZExt(Value, Wide),
      Builder.CreateShl(Builder.CreateZExt(Flags, Wide), 8 * Word));
}

} // namespace neverd
