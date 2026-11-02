//===- X86Hle.cpp - Native HLE source contracts --------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Keep instruction memory and publication opaque to the C optimizer.
//===----------------------------------------------------------------------===//

#include "X86Hle.h"

#include "neverd/ir/high/HighIR.h"
#include "neverd/ir/intrinsics/X86/X86Hle.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Support/ModRef.h"

#include <stdexcept>

namespace neverd {

std::string renderX86HleExpression(Arch TargetArch, const HighExpr &E,
                                   const std::vector<std::string> &Arguments) {
  X86HleShape S;
  S.TargetArch = TargetArch;
  S.ValidId = E.Kind == ExprKind::Call &&
              E.IntrinsicId == Intrinsic::X86HleState &&
              E.IntrinsicOutputs.empty();
  S.OutputSize = E.Type ? E.Type->Size : 0;
  S.Arguments = E.Operands.size();
  S.MemoryOrdering = E.MemoryOrdering;
  S.MemoryAddressSpace = E.MemoryAddressSpace;
  for (unsigned I = 0; I < S.Arguments && I < S.InputSizes.size(); ++I) {
    const auto &V = E.Operands[I];
    S.ScalarInputs[I] =
        V && V->Type &&
        (V->Type->Kind == NdTypeKind::Int || V->Type->Kind == NdTypeKind::Ptr);
    S.InputSizes[I] = V && V->Type ? V->Type->Size : 0;
    if (I == 4 && V && V->Kind == ExprKind::Const)
      S.Control = V->ConstVal;
  }
  if (!x86HleShapeIsValid(S) || Arguments.size() != 5)
    throw std::invalid_argument("invalid x86 HLE state contract");
  const auto P = *x86HlePlan(*S.Control);
  std::string Text =
      "({ void *__neverd_hle_memory = (void *)(" + Arguments[0] +
      "); unsigned char *__neverd_hle_original = (unsigned char *)(" +
      Arguments[1] + "); void *__neverd_hle_flags = (void *)(" + Arguments[2] +
      "); uint64_t __neverd_hle_source = (uint64_t)(" + Arguments[3] +
      "); uint64_t __neverd_hle_bank[4]; __builtin_memcpy(__neverd_hle_bank, "
      "__neverd_hle_original, " +
      std::to_string(X86HleNativeGprBytes) + "); ";
  if (P.ExplicitSource)
    Text += "__neverd_hle_bank[1] = __neverd_hle_source; ";
  Text += "__builtin_ia32_hle_context(__neverd_hle_memory, __neverd_hle_bank, "
          "__neverd_hle_flags, " +
          std::to_string(P.NativeControl) + ", " + std::to_string(P.Bytes) +
          "); ";
  for (const auto &Copy : P.Outputs)
    if (Copy.Bytes)
      Text += "__builtin_memcpy(__neverd_hle_original + " +
              std::to_string(Copy.OriginalOffset) +
              ", (unsigned char *)__neverd_hle_bank + " +
              std::to_string(Copy.NativeOffset) + ", " +
              std::to_string(Copy.Bytes) + "); ";
  return Text + "})";
}

std::string renderX86HleNativeCall(Arch TargetArch, const llvm::CallBase &Call,
                                   bool ResultLive,
                                   const std::vector<std::string> &Arguments) {
  const auto *F = Call.getCalledFunction();
  bool Valid = TargetArch == Arch::X64 && F && F->isDeclaration() &&
               F->getName() == "llvm.x86.hle.context" && !F->isVarArg() &&
               F->getCallingConv() == llvm::CallingConv::C &&
               Call.getCallingConv() == llvm::CallingConv::C &&
               !Call.getNumOperandBundles() && Call.getType()->isVoidTy() &&
               !ResultLive && Call.arg_size() == 5 && Arguments.size() == 5 &&
               Call.getMemoryEffects() == llvm::MemoryEffects::unknown() &&
               !Call.doesNotReturn() &&
               !Call.hasFnAttr(llvm::Attribute::NoSync) &&
               !Call.hasFnAttr(llvm::Attribute::Speculatable) &&
               Call.hasFnAttr(llvm::Attribute::Convergent) &&
               Call.hasFnAttr(llvm::Attribute::NoDuplicate) &&
               Call.hasFnAttr(llvm::Attribute::NoUnwind);
  const llvm::ConstantInt *Control = nullptr, *Bytes = nullptr;
  if (Valid) {
    for (unsigned I = 0; I != 3; ++I) {
      const auto *T = Call.getArgOperand(I)->getType();
      Valid &= T->isPointerTy() && T->getPointerAddressSpace() == 0;
    }
    Control = llvm::dyn_cast<llvm::ConstantInt>(Call.getArgOperand(3));
    Bytes = llvm::dyn_cast<llvm::ConstantInt>(Call.getArgOperand(4));
    Valid &= Control && Bytes && Control->getType()->isIntegerTy(32) &&
             Bytes->getType()->isIntegerTy(32) &&
             x86HleNativeControlIsValid(Control->getZExtValue(),
                                        Bytes->getZExtValue());
  }
  if (!Valid)
    throw std::invalid_argument(
        "invalid native x86 HLE memory/publication contract");
  return "__builtin_ia32_hle_context((void *)(" + Arguments[0] +
         "), (void *)(" + Arguments[1] + "), (void *)(" + Arguments[2] + "), " +
         std::to_string(Control->getZExtValue()) + ", " +
         std::to_string(Bytes->getZExtValue()) + ");\n";
}

} // namespace neverd
