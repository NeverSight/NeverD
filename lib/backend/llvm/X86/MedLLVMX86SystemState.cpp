//===- MedLLVMX86SystemState.cpp - Bounded system instruction lowering ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Keep system effects, conditional memory and actual flag completion in one
/// native instruction. Unsupported environments still fault architecturally.
//===----------------------------------------------------------------------===//

#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/backend/llvm/X86/X86SystemStateNative.h"
#include "neverd/ir/med/IntrinsicShapes.h"

#include "llvm/IR/Function.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/TargetParser/SubtargetFeature.h"

#include <set>
#include <string>
#include <vector>

namespace neverd {

void applyX86SystemNativeTargetFeatures(llvm::Function &Function) {
  std::set<std::string> Required;
  for (const auto &Block : Function)
    for (const auto &Instruction : Block)
      if (const auto *Call = llvm::dyn_cast<llvm::CallBase>(&Instruction))
        if (const auto *Callee = Call->getCalledFunction())
          if (const char *Feature =
                  x86SystemNativeTargetFeature(Callee->getName().str()))
            Required.insert(Feature);
  if (Required.empty())
    return;
  llvm::SubtargetFeatures Features(
      Function.getFnAttribute("target-features").getValueAsString());
  for (const auto &Feature : Required)
    Features.AddFeature(Feature);
  Function.addFnAttr("target-features", Features.getString());
}

llvm::Value *MedLLVMEmitter::emitX86SystemState(const MedOp &Op, Intrinsic Id,
                                                llvm::IRBuilder<> &Builder) {
  if (Id == Intrinsic::X86MsrAccess) {
    if (TargetArch != Arch::X64 ||
        !intrinsicX86MsrAccessShapeIsValid(Id, x86MsrAccessMedShape(Op)))
      llvm::report_fatal_error("invalid x86 MSR operand/output contract");
    const auto P = x86MsrAccessAsm(
        static_cast<X86MsrAccessKind>(Op.Inputs[1].ConstVal), TargetArch);
    if (!P)
      llvm::report_fatal_error("unsupported x86 MSR assembly contract");
    std::vector<llvm::Value *> Arguments;
    std::vector<llvm::Type *> Types;
    for (size_t I = 0; I < P->InputBytes.size(); ++I) {
      auto *T = sizeToType(P->InputBytes[I]);
      auto *V = getVar(Op.Inputs[I + 2], Builder);
      Arguments.push_back(V->getType()->isPointerTy()
                              ? Builder.CreatePtrToInt(V, T)
                              : Builder.CreateZExtOrTrunc(V, T));
      Types.push_back(T);
    }
    auto *Result = P->OutputBytes.empty() ? Builder.getVoidTy()
                                          : sizeToType(P->OutputBytes[0]);
    auto *Asm = llvm::InlineAsm::get(
        llvm::FunctionType::get(Result, Types, false), P->Text, P->Constraints,
        /*hasSideEffects=*/true);
    return Builder.CreateCall(Asm, Arguments);
  }
  const auto Shape = x86SystemStateMedShape(Op, TargetArch);
  if (!x86SystemStateShapeIsValid(Id, Shape))
    llvm::report_fatal_error("invalid x86 system instruction operand contract");
  const auto C = x86SystemStateContract(Id);
  const auto P =
      x86SystemStateAsm(Id, TargetArch, x86SystemStateWidth(Id, Shape),
                        Op.MemoryAddressSpace, Shape.SegmentedBlockSelector);
  if (!P)
    llvm::report_fatal_error("unsupported x86 system assembly contract");
  std::vector<llvm::Value *> Arguments;
  std::vector<llvm::Type *> Types;
  const unsigned FirstInput =
      C.Kind == X86SystemStateKind::SegmentedBlock ? 2 : 1;
  for (size_t I = 0; I < P->InputBytes.size(); ++I) {
    auto *T = sizeToType(P->InputBytes[I]);
    llvm::Value *V;
    if (isX86SystemStateAddressArgument(Id, I)) {
      const auto Space = I == C.MemoryArgument ? Op.MemoryAddressSpace
                                               : NdMemoryAddressSpace::Default;
      const uint16_t AccessBytes =
          x86SystemStateMemoryBytes(Id, x86SystemStateWidth(Id, Shape));
      V = resolveAtomicMemoryPtr(Op.Inputs[I + 1], AccessBytes,
                                 sizeToType(AccessBytes), Builder, Space);
      if (Space == NdMemoryAddressSpace::Default)
        T = V->getType();
    } else if (C.Kind == X86SystemStateKind::SegmentedBlock ||
               isX86SystemStatePhysicalAddressArgument(Id, I))
      // A segment offset may equal an image VA without naming its replacement.
      // Reuse the FS/GS offset materializer, including non-constant SSA copies.
      V = getRawSegmentOffset(Op.Inputs[I + FirstInput], Builder);
    else
      V = getVar(Op.Inputs[I + FirstInput], Builder);
    if (V->getType() != T)
      V = V->getType()->isPointerTy() ? Builder.CreatePtrToInt(V, T)
                                      : Builder.CreateZExtOrTrunc(V, T);
    Arguments.push_back(V);
    Types.push_back(T);
  }
  const auto Native = x86SystemStateNative(
      Id, TargetArch, x86SystemStateWidth(Id, Shape), Op.MemoryAddressSpace);
  llvm::CallInst *Call;
  if (Native &&
      (Id == Intrinsic::X86Skinit
           ? llvm::Intrinsic::lookupIntrinsicID(Native->LLVMName)
           : Id == Intrinsic::Xabort ||
                 llvm::Intrinsic::lookupIntrinsicID(
                     Native->AddressBits ? "llvm.x86.lar.mem16.address"
                                         : "llvm.x86.invlpgb.addr32"))) {
    const auto IID = llvm::Intrinsic::lookupIntrinsicID(Native->LLVMName);
    if (!IID)
      llvm::report_fatal_error("native system SDK is incomplete");
    if (Native->PackMsr) {
      auto *Low = Builder.CreateZExt(Arguments[1], Builder.getInt64Ty());
      auto *High = Builder.CreateZExt(Arguments[2], Builder.getInt64Ty());
      Arguments = {Arguments[0],
                   Builder.CreateOr(Low, Builder.CreateShl(High, 32))};
    }
    if (Native->AddressBits) {
      // computeEA already wraps at the source instruction's address size.
      // Keep that offset separate from the CPU's FS/GS base addition.
      Arguments.push_back(Builder.getInt32(Native->AddressBits));
      Arguments.push_back(Builder.getInt32(Native->SegmentPrefix));
    }
    if (Native->DescriptorQuery)
      Arguments.push_back(
          llvm::ConstantInt::get(sizeToType(P->OutputBytes[0]), 0));
    Call = Builder.CreateCall(llvm::Intrinsic::getOrInsertDeclaration(Mod, IID),
                              Arguments);
  } else {
    llvm::Type *Return = Builder.getVoidTy();
    if (P->OutputBytes.size() == 1)
      Return = sizeToType(P->OutputBytes[0]);
    else if (P->OutputBytes.size() == 2)
      Return = llvm::StructType::get(
          *Ctx, {sizeToType(P->OutputBytes[0]), Builder.getInt8Ty()});
    auto *Signature = llvm::FunctionType::get(Return, Types, false);
    auto *Asm = llvm::InlineAsm::get(Signature, P->Text, P->Constraints, true);
    Call = Builder.CreateCall(Asm, Arguments);
  }
  if (C.Kind == X86SystemStateKind::SecureLaunch) {
    Call->setDoesNotReturn();
    Call->addFnAttr(llvm::Attribute::Convergent);
    Call->addFnAttr(llvm::Attribute::NoDuplicate);
    Builder.CreateUnreachable();
  }
  if (P->OutputBytes.empty())
    return nullptr;
  if (P->OutputBytes.size() == 1)
    return Call;
  auto *Value = Builder.CreateExtractValue(Call, 0);
  auto *Flag = Builder.CreateExtractValue(Call, 1);
  if (C.Kind == X86SystemStateKind::AccessRights)
    Value = Builder.CreateSelect(Builder.CreateICmpNE(Flag, Builder.getInt8(0)),
                                 Value,
                                 llvm::ConstantInt::get(Value->getType(), 0));
  auto *Wide = sizeToType(Op.Output.Size);
  return Builder.CreateOr(
      Builder.CreateZExt(Value, Wide),
      Builder.CreateShl(Builder.CreateZExt(Flag, Wide), 8 * P->OutputBytes[0]));
}

} // namespace neverd
