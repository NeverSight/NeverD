//===- LLVMX87NativeState.h - Native x87 state contracts ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Own the fixed interfaces for native x87 control, status and reset calls.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_LLVM_X86_LLVMX87NATIVESTATE_H
#define NEVERD_BACKEND_LLVM_X86_LLVMX87NATIVESTATE_H

#include "neverd/ArchSupport.h"
#include "neverd/backend/llvm/X86/LLVMX86X87StateAsm.h"
#include "neverd/ir/intrinsics/Intrinsics.h"

#include "llvm/IR/CallingConv.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"

#include <stdexcept>

namespace neverd {

enum class X87NativeStateKind {
  ReadControl,
  WriteControl,
  ReadStatus,
  StoreStatus,
  ClearExceptions,
  Initialize,
  Wait,
  SaveEnvironment16,
  SaveEnvironment32,
  LoadEnvironment16,
  LoadEnvironment32,
  SaveRegisters16,
  SaveRegisters32,
  RestoreRegisters16,
  RestoreRegisters32
};

struct X87NativeStateSpec {
  X87NativeStateKind Kind;
  const char *LLVMName;
  const char *Builtin;
  const char *Mnemonic;
  bool ReturnsStatus;
  bool HasPointer;
  unsigned ImageBytes = 0;
  bool ReadsImage = false;
  bool ReplacesBank = false;
};

inline constexpr X87NativeStateSpec X87NativeStateSpecs[] = {
    {X87NativeStateKind::ReadControl, "llvm.x86.fnstcw",
     "__builtin_ia32_fnstcw", "fnstcw", false, true},
    {X87NativeStateKind::WriteControl, "llvm.x86.fldcw", "__builtin_ia32_fldcw",
     "fldcw", false, true},
    {X87NativeStateKind::ReadStatus, "llvm.x86.fnstsw", "__builtin_ia32_fnstsw",
     "fnstsw", true, false},
    {X87NativeStateKind::StoreStatus, "llvm.x86.fnstsw.mem",
     "__builtin_ia32_fnstsw_mem", "fnstsw", false, true},
    {X87NativeStateKind::ClearExceptions, "llvm.x86.fnclex",
     "__builtin_ia32_fnclex", "fnclex", false, false},
    {X87NativeStateKind::Initialize, "llvm.x86.fninit", "__builtin_ia32_fninit",
     "fninit", false, false},
    {X87NativeStateKind::Wait, "llvm.x86.fwait", "__builtin_ia32_fwait",
     "fwait", false, false},
    {X87NativeStateKind::SaveEnvironment16, "llvm.x86.fnstenv16",
     "__builtin_ia32_fnstenv16", "data16 fnstenv", false, true, 14},
    {X87NativeStateKind::SaveEnvironment32, "llvm.x86.fnstenv32",
     "__builtin_ia32_fnstenv32", "fnstenv", false, true, 28},
    {X87NativeStateKind::LoadEnvironment16, "llvm.x86.fldenv16",
     "__builtin_ia32_fldenv16", "data16 fldenv", false, true, 14, true, true},
    {X87NativeStateKind::LoadEnvironment32, "llvm.x86.fldenv32",
     "__builtin_ia32_fldenv32", "fldenv", false, true, 28, true, true},
    {X87NativeStateKind::SaveRegisters16, "llvm.x86.fnsave16",
     "__builtin_ia32_fnsave16", "data16 fnsave", false, true, 94, false, true},
    {X87NativeStateKind::SaveRegisters32, "llvm.x86.fnsave32",
     "__builtin_ia32_fnsave32", "fnsave", false, true, 108, false, true},
    {X87NativeStateKind::RestoreRegisters16, "llvm.x86.frstor16",
     "__builtin_ia32_frstor16", "data16 frstor", false, true, 94, true, true},
    {X87NativeStateKind::RestoreRegisters32, "llvm.x86.frstor32",
     "__builtin_ia32_frstor32", "frstor", false, true, 108, true, true}};

inline const X87NativeStateSpec &x87NativeStateSpec(X87NativeStateKind Kind) {
  for (const auto &Spec : X87NativeStateSpecs)
    if (Spec.Kind == Kind)
      return Spec;
  throw std::invalid_argument("invalid native x87 state operation");
}

/// Only these typed operations act on ambient state. X87ReadStatus instead
/// reads the sampled SSA status of an owned operation and must not use FNSTSW.
inline const X87NativeStateSpec *x87NativeStateOfIntrinsic(Intrinsic Id) {
  switch (Id) {
  case Intrinsic::X87Fninit:
    return &x87NativeStateSpec(X87NativeStateKind::Initialize);
  case Intrinsic::X87Fnclex:
    return &x87NativeStateSpec(X87NativeStateKind::ClearExceptions);
  case Intrinsic::X87Wait:
    return &x87NativeStateSpec(X87NativeStateKind::Wait);
  case Intrinsic::X87Fnstenv16:
    return &x87NativeStateSpec(X87NativeStateKind::SaveEnvironment16);
  case Intrinsic::X87Fnstenv:
    return &x87NativeStateSpec(X87NativeStateKind::SaveEnvironment32);
  case Intrinsic::X87Fldenv16:
    return &x87NativeStateSpec(X87NativeStateKind::LoadEnvironment16);
  case Intrinsic::X87Fldenv:
    return &x87NativeStateSpec(X87NativeStateKind::LoadEnvironment32);
  case Intrinsic::X87Fnsave16:
    return &x87NativeStateSpec(X87NativeStateKind::SaveRegisters16);
  case Intrinsic::X87Fnsave:
    return &x87NativeStateSpec(X87NativeStateKind::SaveRegisters32);
  case Intrinsic::X87Frstor16:
    return &x87NativeStateSpec(X87NativeStateKind::RestoreRegisters16);
  case Intrinsic::X87Frstor:
    return &x87NativeStateSpec(X87NativeStateKind::RestoreRegisters32);
  default:
    return nullptr;
  }
}

inline X87StateEffect x87NativeStateEffect(const X87NativeStateSpec &Spec) {
  return Spec.ReplacesBank || Spec.Kind == X87NativeStateKind::Initialize
             ? X87StateEffect::Reset
         : Spec.ImageBytes || Spec.Kind == X87NativeStateKind::WriteControl
             ? X87StateEffect::Control
             : X87StateEffect::Status;
}

inline const X87NativeStateSpec *
classifyX87NativeState(const llvm::CallBase &Call, Arch TheArch) {
  // getCalledFunction() hides a named function when the call-site prototype
  // differs. Such a call must still reach this contract's rejection.
  const auto *Callee = llvm::dyn_cast<llvm::Function>(
      Call.getCalledOperand()->stripPointerCastsAndAliases());
  if (!Callee)
    return nullptr;
  const X87NativeStateSpec *Spec = nullptr;
  for (const auto &Candidate : X87NativeStateSpecs)
    if (Callee->getName() == Candidate.LLVMName)
      Spec = &Candidate;
  if (!Spec)
    return nullptr;
  if ((TheArch != Arch::X86 && TheArch != Arch::X64) ||
      !llvm::isa<llvm::CallInst>(Call) || !Callee->isDeclaration() ||
      Call.getCalledOperand() != Callee ||
      Call.getFunctionType() != Callee->getFunctionType() ||
      Call.getCallingConv() != llvm::CallingConv::C ||
      Callee->getCallingConv() != llvm::CallingConv::C ||
      (Spec->ReturnsStatus ? !Call.getType()->isIntegerTy(16)
                           : !Call.getType()->isVoidTy()) ||
      Call.getFunctionType()->isVarArg() ||
      Call.arg_size() != unsigned(Spec->HasPointer) ||
      (Spec->HasPointer &&
       (!Call.getArgOperand(0)->getType()->isPointerTy() ||
        Call.getArgOperand(0)->getType()->getPointerAddressSpace() != 0)))
    throw std::invalid_argument("invalid native x87 state call");
  return Spec;
}

} // namespace neverd

#endif // NEVERD_BACKEND_LLVM_X86_LLVMX87NATIVESTATE_H
