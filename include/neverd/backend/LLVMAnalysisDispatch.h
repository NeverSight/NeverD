//===- LLVMAnalysisDispatch.h - Display-only indirect exits ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Preserve an uninterpreted transfer and its original site in private source
/// LLVM. These markers have no runtime implementation or native proof value.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_LLVMANALYSISDISPATCH_H
#define NEVERD_BACKEND_LLVMANALYSISDISPATCH_H

#include "llvm/IR/Constants.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace neverd::llvm_analysis_dispatch {

inline constexpr llvm::StringLiteral FunctionAttribute("neverd.analysis-only");
inline constexpr llvm::StringLiteral ExitAttribute("neverd.analysis.dispatch");
inline constexpr llvm::StringLiteral
    SiteBundle("neverd.analysis.dispatch.site");

inline llvm::CallInst *create(llvm::IRBuilder<> &Builder, llvm::Value *Target,
                              uint64_t Address) {
  auto &Module = *Builder.GetInsertBlock()->getModule();
  auto *Type =
      llvm::FunctionType::get(Builder.getVoidTy(), {Target->getType()}, false);
  std::string Name = "__neverd_analysis_indirect_jump";
  llvm::Function *Function = nullptr;
  while (auto *Existing = Module.getNamedValue(Name)) {
    Function = llvm::dyn_cast<llvm::Function>(Existing);
    if (Function && Function->isDeclaration() &&
        Function->hasFnAttribute(ExitAttribute) &&
        Function->getFunctionType() == Type)
      break;
    Function = nullptr;
    Name += '_';
  }
  if (!Function) {
    Function = llvm::Function::Create(Type, llvm::GlobalValue::ExternalLinkage,
                                      Name, Module);
    Function->addFnAttr(ExitAttribute);
    Function->addFnAttr(llvm::Attribute::NoReturn);
    Function->addFnAttr(llvm::Attribute::NoUnwind);
  }
  // A bundle keeps distinct sites distinct through LLVM optimization; ordinary
  // instruction metadata alone may be dropped when identical calls merge.
  llvm::OperandBundleDef Site(
      SiteBundle.str(), std::vector<llvm::Value *>{Builder.getInt64(Address)});
  return Builder.CreateCall(Function, {Target}, {Site});
}

inline std::optional<uint64_t> address(const llvm::CallBase &Call) {
  const auto *Function = Call.getCalledFunction();
  if (!Function || !Function->hasFnAttribute(ExitAttribute))
    return std::nullopt;
  if (Call.getNumOperandBundles() != 1)
    throw std::invalid_argument("malformed analysis indirect-jump site");
  const auto Site = Call.getOperandBundle(SiteBundle);
  const auto *Address = Site && Site->Inputs.size() == 1
                            ? llvm::dyn_cast<llvm::ConstantInt>(Site->Inputs[0])
                            : nullptr;
  if (!Function->isDeclaration() || !Call.getType()->isVoidTy() ||
      Call.arg_size() != 1 ||
      !Call.getArgOperand(0)->getType()->isIntegerTy(32) ||
      Call.getFunctionType() != Function->getFunctionType() ||
      !Call.doesNotReturn() || Function->isVarArg() || !Address ||
      !Address->getType()->isIntegerTy(64) ||
      !Call.getFunction()->hasFnAttribute(FunctionAttribute))
    throw std::invalid_argument("malformed analysis indirect-jump marker");
  return Address->getZExtValue();
}

} // namespace neverd::llvm_analysis_dispatch

#endif // NEVERD_BACKEND_LLVMANALYSISDISPATCH_H
