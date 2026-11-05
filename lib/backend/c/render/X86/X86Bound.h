//===- X86Bound.h - Instruction-owned bounds checks in C -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Instruction-owned bounds checks in C.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_BACKEND_C_RENDER_X86_X86BOUND_H
#define NEVERD_LIB_BACKEND_C_RENDER_X86_X86BOUND_H

#include "neverd/backend/llvm/X86/X86BoundNative.h"
#include "neverd/ir/high/HighIR.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Intrinsics.h"

#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace neverd {

inline constexpr const char X86BoundCompilerDispatch[] =
    "#ifndef NEVERD_X86_BOUND\n"
    "#if defined(__has_builtin)\n"
    "#if __has_builtin(__builtin_ia32_bound16_address32) && "
    "__has_builtin(__builtin_ia32_bound32_address32)\n"
    "#define NEVERD_X86_BOUND(native, ...) native\n"
    "#endif\n#endif\n"
    "#ifndef NEVERD_X86_BOUND\n"
    "#if defined(__GNUC__) || defined(__clang__)\n"
    "#define NEVERD_X86_BOUND(native, ...) __VA_ARGS__\n"
    "#else\n#error BOUND requires native x86 intrinsics or GNU assembly\n"
    "#endif\n#endif\n#endif\n\n";

inline std::string renderX86Bound(const X86BoundNative &P,
                                  const std::string &Index,
                                  const std::string &Offset) {
  const std::string I =
      "(int" + std::to_string(P.Bytes * 8) + "_t)(" + Index + ")";
  const std::string A = "(uint32_t)(uintptr_t)(" + Offset + ")";
  const std::string Native = P.builtin() + "(" + I + ", " + A + ", " +
                             std::to_string(P.AddressBits) + ", " +
                             std::to_string(P.Prefix) + ")";
  const auto *Constraint = x86MemoryAddressAsm(P.AddressBits).GNUConstraint;
  return "NEVERD_X86_BOUND(" + Native + ", __asm__ volatile(\"" +
         P.assembly(true) + "\" : : \"r\"(" + I + "), \"" + Constraint + "\"(" +
         A + ") : \"memory\"))";
}

inline X86BoundNative x86BoundHighPlan(Arch Target, const HighExpr &E) {
  if (Target != Arch::X86 || E.Kind != ExprKind::Call ||
      E.IntrinsicId != Intrinsic::Bound || E.Operands.size() != 4 ||
      !E.IntrinsicOutputs.empty() || E.IsIndirectCall || E.IndirectTarget ||
      E.SourceCallHint || E.DoesNotReturn)
    throw std::invalid_argument(X86BoundMalformed);
  std::array<unsigned, 4> Sizes{};
  for (unsigned I = 0; I != 4; ++I) {
    const auto &V = E.Operands[I];
    if (!V || !V->Type ||
        (V->Type->Kind != NdTypeKind::Int &&
         !(I == 1 && V->Type->Kind == NdTypeKind::Ptr)))
      throw std::invalid_argument(X86BoundMalformed);
    Sizes[I] = V->Type->Size;
  }
  const auto Constant = [&](unsigned I) -> std::optional<uint64_t> {
    const auto &V = *E.Operands[I];
    return V.Kind == ExprKind::Const && !isAddressProvenance(V.ConstProvenance)
               ? std::optional<uint64_t>(V.ConstVal)
               : std::nullopt;
  };
  if (!x86BoundShapeIsValid(
          Target, E.Operands.size(), E.Type ? E.Type->Size : 0, Sizes,
          Constant(2), Constant(3), E.MemoryAddressSpace, E.MemoryOrdering))
    throw std::invalid_argument(X86BoundMalformed);
  return {Sizes[0], unsigned(*Constant(2)), unsigned(*Constant(3))};
}

inline bool isX86BoundNativeName(llvm::StringRef Name) {
  return Name == "llvm.x86.bound16" || Name == "llvm.x86.bound32" ||
         Name == "llvm.x86.bound16.address32" ||
         Name == "llvm.x86.bound32.address32";
}

/// Match only the exact native contract or this backend's assembly fallback.
inline std::optional<X86BoundNative> x86BoundCallPlan(Arch Target,
                                                      const llvm::CallBase &C) {
  std::optional<X86BoundNative> P;
  if (const auto *F = C.getCalledFunction();
      F && isX86BoundNativeName(F->getName())) {
    const auto ID = llvm::Intrinsic::lookupIntrinsicID(F->getName());
    if (!ID || F->getIntrinsicID() != ID || !F->isDeclaration() ||
        F->hasFnAttribute(llvm::Attribute::Speculatable))
      throw std::invalid_argument(X86BoundMalformed);
    const auto Name = F->getName();
    const bool Explicit = Name.ends_with(".address32");
    if (C.arg_size() != (Explicit ? 4u : 2u))
      throw std::invalid_argument(X86BoundMalformed);
    unsigned Bits = 32, Prefix = 0;
    if (Explicit) {
      const auto *B = llvm::dyn_cast<llvm::ConstantInt>(C.getArgOperand(2));
      const auto *S = llvm::dyn_cast<llvm::ConstantInt>(C.getArgOperand(3));
      if (!B || !S || !B->getType()->isIntegerTy(32) ||
          !S->getType()->isIntegerTy(32) ||
          !x86BoundAddressIsValid(B->getZExtValue(), S->getZExtValue()))
        throw std::invalid_argument(X86BoundMalformed);
      Bits = B->getZExtValue();
      Prefix = S->getZExtValue();
    }
    const auto *A = C.getArgOperand(1)->getType();
    if (Explicit ? !A->isIntegerTy(32)
                 : !A->isPointerTy() || A->getPointerAddressSpace())
      throw std::invalid_argument(X86BoundMalformed);
    P = X86BoundNative{Name.starts_with("llvm.x86.bound16") ? 2u : 4u, Bits,
                       Prefix};
  } else if (const auto *A =
                 llvm::dyn_cast<llvm::InlineAsm>(C.getCalledOperand())) {
    for (unsigned Bytes : {2u, 4u})
      for (unsigned Bits : {16u, 32u})
        for (unsigned Prefix : {0u, 0x26u, 0x2eu, 0x36u, 0x3eu, 0x64u, 0x65u}) {
          const X86BoundNative Candidate{Bytes, Bits, Prefix};
          if (A->getAsmString() == Candidate.assembly() &&
              A->getConstraintString() == Candidate.constraints()) {
            if (!A->hasSideEffects() ||
                A->getDialect() != llvm::InlineAsm::AD_ATT ||
                C.arg_size() != 2 ||
                !C.getArgOperand(1)->getType()->isIntegerTy(32))
              throw std::invalid_argument(X86BoundMalformed);
            P = Candidate;
          }
        }
  }
  if (P &&
      (Target != Arch::X86 || C.getNumOperandBundles() ||
       C.getFunctionType()->isVarArg() || C.doesNotReturn() ||
       C.getCallingConv() != llvm::CallingConv::C ||
       C.getMemoryEffects() != llvm::MemoryEffects::unknown() ||
       C.hasFnAttr(llvm::Attribute::Speculatable) || !C.getType()->isVoidTy() ||
       !C.getArgOperand(0)->getType()->isIntegerTy(P->Bytes * 8)))
    throw std::invalid_argument(X86BoundMalformed);
  return P;
}

} // namespace neverd

#endif // NEVERD_LIB_BACKEND_C_RENDER_X86_X86BOUND_H
