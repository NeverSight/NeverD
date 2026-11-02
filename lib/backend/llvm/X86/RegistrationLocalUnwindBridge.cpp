//===- RegistrationLocalUnwindBridge.cpp - CRT parent frame projection ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Preserve the CRT's collided-unwind guard while using LLVM's virtual EBP.

#include "neverd/backend/RewriteSourceIdentity.h"
#include "neverd/backend/llvm/X86/X86RegistrationLocalUnwind.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Module.h"
#include "llvm/TargetParser/Triple.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"

namespace neverd::x86_registration {

std::string localUnwindBridgeName(va_t Target, va_t Owner) {
  std::string Name = "__nd_seh_local_unwind_" + llvm::utohexstr(Target);
  if (Owner)
    Name += "_" + llvm::utohexstr(Owner);
  return Name;
}

llvm::FunctionType *
localUnwindRuntimeType(llvm::LLVMContext &Context,
                       RegistrationLocalUnwindContract::Kind Protocol) {
  auto *Pointer = llvm::PointerType::getUnqual(Context);
  auto *Integer = llvm::Type::getInt32Ty(Context);
  auto *Void = llvm::Type::getVoidTy(Context);
  if (Protocol == RegistrationLocalUnwindContract::Kind::EH4)
    return llvm::FunctionType::get(Void, {Pointer, Pointer, Integer}, false);
  return llvm::FunctionType::get(Void, {Pointer, Integer}, false);
}

llvm::Expected<llvm::Function *>
createLocalUnwindBridge(llvm::Function &Runtime,
                        RegistrationLocalUnwindContract::Kind Protocol,
                        va_t Owner) {
  using Kind = RegistrationLocalUnwindContract::Kind;
  auto &Module = *Runtime.getParent();
  auto &Context = Module.getContext();
  const auto &Target = Module.getTargetTriple();
  auto Identity = rewrite_source::getOriginalVA(Runtime);
  if (!Identity)
    return Identity.takeError();
  if ((Protocol != Kind::EH3 && Protocol != Kind::EH4) || Owner > UINT32_MAX ||
      Target.getArch() != llvm::Triple::x86 || !Target.isOSWindows() ||
      !*Identity || !**Identity || **Identity > UINT32_MAX ||
      !Runtime.isDeclaration() || !Runtime.hasExternalLinkage() ||
      Runtime.getCallingConv() != llvm::CallingConv::C ||
      Runtime.getFunctionType() != localUnwindRuntimeType(Context, Protocol) ||
      !Runtime.getAttributes().isEmpty())
    return llvm::createStringError(
        "local unwind runtime has no checked PE32 ABI");
  const std::string Name = localUnwindBridgeName(**Identity, Owner);
  if (Module.getNamedValue(Name))
    return llvm::createStringError(
        "local unwind bridge name is already occupied");
  const bool EH4 = Protocol == Kind::EH4;
  auto *Pointer = llvm::PointerType::getUnqual(Context);
  llvm::SmallVector<llvm::Type *, 2> Parameters{Pointer};
  if (EH4)
    Parameters.push_back(Pointer);
  auto *Bridge = llvm::Function::Create(
      llvm::FunctionType::get(llvm::Type::getVoidTy(Context), Parameters,
                              false),
      llvm::GlobalValue::InternalLinkage, Name, Module);
  Bridge->addFnAttr(llvm::Attribute::NoInline);
  Bridge->addFnAttr(llvm::Attribute::OptimizeNone);
  llvm::IRBuilder<> Builder(llvm::BasicBlock::Create(Context, "entry", Bridge));
  // The CRT calls cleanup entries with its incoming EBP. LLVM's SEH entry
  // recovery expects registration + 16. Keep the caller's physical EBP intact
  // and let the CRT own state retirement, abnormal termination and its guard.
  const llvm::StringRef Assembly =
      EH4 ? "pushl %ebp\n\tpushl $$-2\n\tpushl $0\n\tpushl $2\n\t"
            "movl $0, %ebp\n\taddl $$16, %ebp\n\tcalll *$1\n\t"
            "addl $$12, %esp\n\tpopl %ebp"
          : "pushl %ebp\n\tpushl $$-1\n\tpushl $0\n\t"
            "movl $0, %ebp\n\taddl $$16, %ebp\n\tcalll *$1\n\t"
            "addl $$8, %esp\n\tpopl %ebp";
  const llvm::StringRef Constraints =
      EH4 ? "r,{eax},{ecx},~{eax},~{ecx},~{edx},~{memory},~{dirflag},~{fpsr},~{"
            "flags}"
          : "r,{eax},~{eax},~{ecx},~{edx},~{memory},~{dirflag},~{fpsr},~{"
            "flags}";
  llvm::SmallVector<llvm::Value *, 3> Arguments{Bridge->getArg(0), &Runtime};
  llvm::SmallVector<llvm::Type *, 3> Types{Pointer, Pointer};
  if (EH4) {
    Arguments.push_back(Bridge->getArg(1));
    Types.push_back(Pointer);
  }
  auto *Asm = llvm::InlineAsm::get(
      llvm::FunctionType::get(Builder.getVoidTy(), Types, false), Assembly,
      Constraints, true, false, llvm::InlineAsm::AD_ATT, true);
  // Cleanup callbacks may throw. The unwind flag prevents function-attribute
  // inference from treating the assembly call as nounwind after optimization.
  auto *Call = Builder.CreateCall(Asm, Arguments);
  Call->setTailCallKind(llvm::CallInst::TCK_NoTail);
  Call->addParamAttr(1, llvm::Attribute::NonNull);
  Builder.CreateRetVoid();
  // Keep the private callable ABI stable through interprocedural rewriting.
  llvm::appendToCompilerUsed(Module, {Bridge});
  return Bridge;
}

} // namespace neverd::x86_registration
