//===- MedLLVMX86AmxState.cpp - Native AMX bank operations -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Hardware owns complete tile state and publishes restart progress directly.
/// Only an architectural ZMM row result enters ordinary LLVM value SSA.
///
//===----------------------------------------------------------------------===//

#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/backend/llvm/X86/X86AmxAsm.h"
#include "neverd/backend/llvm/X86/X86AmxNative.h"
#include "neverd/ir/med/IntrinsicShapes.h"

#include "llvm/IR/Function.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/Support/ErrorHandling.h"

#include <vector>
namespace neverd {
llvm::Value *MedLLVMEmitter::emitX86AmxState(const MedOp &Op,
                                             llvm::IRBuilder<> &B) {
  auto S = x86AmxMedShape(Op, TargetArch);
  if (TargetArch != Arch::X64 || !x86AmxShapeIsValid(S))
    llvm::report_fatal_error("invalid AMX architectural bank contract");
  const auto D = *S.Descriptor;
  const auto K = x86AmxOperation(D);
  // Require the complete explicit-control API before selecting this route.
  // Older pinned SDKs retain the exact owned assembly implementation below.
  const auto ID =
      llvm::Intrinsic::lookupIntrinsicID("llvm.x86." + x86AmxNativeSuffix(D));
  const bool NativeAddress =
      x86AmxNeedsStackLeaf(D)
          ? x86AmxNativeStackMemoryIsAvailable()
          : !(D & X86AmxStackReference) || x86NativeStackMemoryIsAvailable();
  if (NativeAddress &&
      llvm::Intrinsic::lookupIntrinsicID("llvm.x86.ldtilecfg.address") && ID) {
    auto *Function = B.GetInsertBlock()->getParent();
    std::string Features =
        Function->getFnAttribute("target-features").getValueAsString().str();
    llvm::SmallVector<llvm::StringRef, 8> Required;
    const auto FeatureNames = x86AmxNativeFeatures(D);
    llvm::StringRef(FeatureNames).split(Required, ',');
    for (const auto Feature : Required) {
      if (!Features.empty())
        Features += ',';
      Features += "+" + Feature.str();
    }
    Function->addFnAttr("target-features", Features);
    std::vector<llvm::Value *> NativeArgs;
    if (x86AmxIsTileMemory(K) || x86AmxIsRow(K) || x86AmxIsCompute(K) ||
        K == X86AmxOp::Zero)
      NativeArgs.push_back(B.getInt8(x86AmxTile(D)));
    if (x86AmxIsMemory(K)) {
      auto *Base =
          resolveAtomicMemoryPtr(Op.Inputs[1], x86AmxIsTileMemory(K) ? 1 : 64,
                                 B.getInt8Ty(), B, S.MemoryAddressSpace);
      NativeArgs.push_back(B.CreatePtrToInt(Base, B.getInt64Ty()));
      if (x86AmxIsTileMemory(K))
        NativeArgs.push_back(B.CreateZExtOrTrunc(
            getRawSegmentOffset(Op.Inputs[3], B), B.getInt64Ty()));
      NativeArgs.push_back(B.getInt32(D & X86AmxAddress32 ? 32 : 64));
      NativeArgs.push_back(
          B.getInt32(x86AmxNativePrefix(D, S.MemoryAddressSpace)));
      NativeArgs.push_back(B.getInt32(bool(D & X86AmxEvexMemory)));
    } else if (x86AmxIsRow(K)) {
      NativeArgs.push_back(B.CreateZExtOrTrunc(
          getRawSegmentOffset(Op.Inputs[3], B), B.getInt32Ty()));
      if (!(D & X86AmxImmediateRow))
        NativeArgs.push_back(B.getInt32(bool(D & X86AmxExtendedRow)));
    } else if (x86AmxIsCompute(K)) {
      NativeArgs.push_back(B.getInt8(x86AmxTile(D, 1)));
      NativeArgs.push_back(B.getInt8(x86AmxTile(D, 2)));
    }
    auto *Call = B.CreateCall(
        llvm::Intrinsic::getOrInsertDeclaration(Function->getParent(), ID),
        NativeArgs);
    return x86AmxIsRow(K) ? B.CreateBitCast(Call, sizeToType(X86AmxRowBytes))
                          : nullptr;
  }
  const auto P = x86AmxAsm(D, S.MemoryAddressSpace);
  if (!P)
    llvm::report_fatal_error("invalid AMX native assembly contract");
  std::vector<llvm::Value *> Args;
  llvm::AllocaInst *Row = nullptr;
  if (P->Memory) {
    auto *Base =
        resolveAtomicMemoryPtr(Op.Inputs[1], x86AmxIsTileMemory(K) ? 1 : 64,
                               B.getInt8Ty(), B, S.MemoryAddressSpace);
    Args.push_back(Base);
    if (x86AmxIsTileMemory(K))
      Args.push_back(getRawSegmentOffset(Op.Inputs[3], B));
  } else if (P->Row) {
    auto &EntryBlock = B.GetInsertBlock()->getParent()->getEntryBlock();
    llvm::IRBuilder<> Entry(&EntryBlock, EntryBlock.begin());
    Row = Entry.CreateAlloca(sizeToType(X86AmxRowBytes), nullptr, "amx_row");
    Args.push_back(Row);
    Args.push_back(getRawSegmentOffset(Op.Inputs[3], B));
  }
  std::vector<llvm::Type *> Types;
  for (unsigned I = 0; I < Args.size(); ++I) {
    auto *T = sizeToType(P->InputBytes[I]);
    if (P->Row && I == 0)
      T = Args[I]->getType();
    if (Args[I]->getType() != T)
      Args[I] = Args[I]->getType()->isPointerTy()
                    ? B.CreatePtrToInt(Args[I], T)
                    : B.CreateZExtOrTrunc(Args[I], T);
    Types.push_back(T);
  }
  auto *Asm =
      llvm::InlineAsm::get(llvm::FunctionType::get(B.getVoidTy(), Types, false),
                           P->Text, P->Constraints, true);
  if (P->StackLeaf) {
    auto *Module = B.GetInsertBlock()->getModule();
    auto *Leaf = llvm::Function::Create(Asm->getFunctionType(),
                                        llvm::GlobalValue::InternalLinkage,
                                        "neverd_amx_stack", Module);
    Leaf->setCallingConv(llvm::CallingConv::PreserveAll);
    Leaf->addFnAttr(X86AmxStackLeafAttribute);
    Leaf->addFnAttr("frame-pointer", "none");
    Leaf->addFnAttr(llvm::Attribute::NoInline);
    Leaf->addFnAttr(llvm::Attribute::NoUnwind);
    Leaf->setUWTableKind(llvm::UWTableKind::Async);
    std::string Features;
    llvm::SmallVector<llvm::StringRef, 8> Names;
    const auto Required = x86AmxNativeFeatures(D);
    llvm::StringRef(Required).split(Names, ',');
    for (auto Name : Names) {
      if (!Features.empty())
        Features += ',';
      Features += "+" + Name.str();
    }
    Leaf->addFnAttr("target-features", Features);
    llvm::IRBuilder<> Body(
        llvm::BasicBlock::Create(B.getContext(), "entry", Leaf));
    Body.CreateCall(Asm, {Leaf->getArg(0), Leaf->getArg(1)});
    Body.CreateRetVoid();
    B.CreateCall(Leaf, Args)->setCallingConv(Leaf->getCallingConv());
  } else {
    B.CreateCall(Asm, Args);
  }
  return Row ? B.CreateLoad(sizeToType(X86AmxRowBytes), Row) : nullptr;
}
} // namespace neverd
