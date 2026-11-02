//===- X87ImportCall.cpp - Native CRT x87 call adapter
//----------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/backend/llvm/X86/X86X87ImportCall.h"

#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Module.h"
#include "llvm/TargetParser/Triple.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"

namespace neverd {

llvm::Expected<llvm::Function *>
createX87ImportCallAdapter(llvm::Function &Runtime, unsigned Arguments) {
  llvm::Module &Module = *Runtime.getParent();
  llvm::LLVMContext &Context = Module.getContext();
  auto *ValueType = llvm::Type::getX86_FP80Ty(Context);
  auto *PointerType = llvm::PointerType::getUnqual(Context);
  if ((Arguments != 1 && Arguments != 2) ||
      Module.getTargetTriple().getArch() != llvm::Triple::x86 ||
      !Module.getTargetTriple().isOSWindows() || !Runtime.isDeclaration() ||
      !Runtime.hasExternalLinkage() ||
      Runtime.getCallingConv() != llvm::CallingConv::C ||
      Runtime.getFunctionType() != llvm::FunctionType::get(ValueType, false) ||
      !Runtime.getAttributes().isEmpty())
    return llvm::createStringError("CRT x87 import has no checked native ABI");

  const std::string Name = "__nd_x87_import_" + Runtime.getName().str();
  llvm::SmallVector<llvm::Type *, 2> Types(Arguments, ValueType);
  Types.push_back(PointerType);
  auto *Type = llvm::FunctionType::get(ValueType, Types, false);
  if (auto *Named = Module.getNamedValue(Name)) {
    auto *Existing = llvm::dyn_cast<llvm::Function>(Named);
    if (Existing && Existing->getFunctionType() == Type &&
        Existing->hasInternalLinkage() &&
        Existing->hasFnAttribute("neverd.x87-import-adapter"))
      return Existing;
    return llvm::createStringError("CRT x87 adapter name is already occupied");
  }
  auto *Adapter = llvm::Function::Create(
      Type, llvm::GlobalValue::InternalLinkage, Name, Module);
  Adapter->addFnAttr(llvm::Attribute::NoInline);
  Adapter->addFnAttr(llvm::Attribute::OptimizeNone);
  Adapter->addFnAttr("neverd.x87-import-adapter");
  llvm::IRBuilder<> Builder(
      llvm::BasicBlock::Create(Context, "entry", Adapter));
  llvm::SmallVector<llvm::Value *, 3> Values;
  for (llvm::Argument &Argument : Adapter->args())
    Values.push_back(&Argument);
  const std::string Assembly = "calll *$" + std::to_string(Arguments + 1);
  std::string Constraints = "={st},0,";
  if (Arguments == 2)
    Constraints += "{st(1)},";
  Constraints += "r,~{eax},~{ecx},~{edx},~{memory},~{dirflag},~{fpsr},~{flags}";
  for (unsigned K = 1; K < 8; ++K)
    Constraints += ",~{st(" + std::to_string(K) + ")}";
  for (unsigned K = 0; K < 8; ++K)
    Constraints += ",~{xmm" + std::to_string(K) + "}";
  // Call the actual CRT entry with its original stack ordering and all eighty
  // payload bits. The runtime may call error handlers or unwind; it is neither
  // a pure mathematical intrinsic nor a nounwind inline assembly operation.
  auto *AssemblyCall = llvm::InlineAsm::get(
      llvm::FunctionType::get(ValueType, Types, false), Assembly, Constraints,
      true, false, llvm::InlineAsm::AD_ATT, true);
  auto *Call = Builder.CreateCall(AssemblyCall, Values);
  Call->setTailCallKind(llvm::CallInst::TCK_NoTail);
  Builder.CreateRet(Call);
  llvm::appendToCompilerUsed(Module, {Adapter});
  return Adapter;
}

} // namespace neverd
