//===- PipelineSourceCallHelpers.h - Shared source bodies ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Keep one source-only definition of an admitted body across its callers.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_PIPELINE_SOURCECALLHELPERS_H
#define NEVERD_PIPELINE_SOURCECALLHELPERS_H

#include "neverd/backend/RewriteSourceIdentity.h"
#include "neverd/backend/llvm/LLVMSourceBody.h"
#include "neverd/backend/llvm/LLVMSourceMap.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/Transforms/Utils/Cloning.h"

namespace neverd::source_inline {

struct SharedBody {
  llvm::Function *Function = nullptr;
  llvm::SmallVector<unsigned, 8> Parameters;
};

/// The caller has already validated this complete body and its observations.
/// Only actually unused LLVM formals are omitted. The native declaration and
/// the caller's existing argument computations remain untouched.
inline SharedBody shareBody(llvm::Function &Original, LLVMSourceMap &Sources) {
  SharedBody Body;
  llvm::SmallVector<llvm::Type *, 8> Types;
  for (auto &Argument : Original.args())
    if (!Argument.use_empty()) {
      Body.Parameters.push_back(Argument.getArgNo());
      Types.push_back(Argument.getType());
    }
  auto *Type = llvm::FunctionType::get(Original.getReturnType(), Types, false);
  Body.Function = llvm::Function::Create(
      Type, llvm::GlobalValue::InternalLinkage, Original.getAddressSpace(),
      "neverd_source_body_" + Original.getName(), Original.getParent());
  llvm::ValueToValueMapTy Values;
  auto Argument = Body.Function->arg_begin();
  for (auto &Old : Original.args()) {
    if (Old.use_empty()) {
      // There are no operand uses. A clone map still needs a value for any
      // debug metadata mentioning the discarded formal; it is never read.
      Values[&Old] = llvm::PoisonValue::get(Old.getType());
    } else {
      Argument->setName(Old.getName());
      Values[&Old] = &*Argument++;
    }
  }
  llvm::SmallVector<llvm::ReturnInst *, 8> Returns;
  llvm::CloneFunctionInto(Body.Function, &Original, Values,
                          llvm::CloneFunctionChangeType::LocalChangesOnly,
                          Returns);
  // Cloning remaps parameter attributes. As in LLVM's dead-argument
  // elimination, discard optional allocsize facts whose indices changed.
  Body.Function->removeFnAttr(llvm::Attribute::AllocSize);
  Body.Function->setVisibility(llvm::GlobalValue::DefaultVisibility);
  Body.Function->setDLLStorageClass(llvm::GlobalValue::DefaultStorageClass);
  Body.Function->setDSOLocal(true);
  Body.Function->setMetadata(rewrite_source::FunctionAttachment, nullptr);
  Body.Function->setSubprogram(nullptr);
  Body.Function->addFnAttr(LLVMSourceBody);
  Values[&Original] = Body.Function;
  Sources.remap([&](llvm::Value *Value) -> llvm::Value * {
    const auto At = Values.find(Value);
    return At == Values.end() ? Value : static_cast<llvm::Value *>(At->second);
  });
  return Body;
}

inline void callSharedBody(llvm::CallInst &Original, const SharedBody &Body) {
  llvm::SmallVector<llvm::Value *, 8> Arguments;
  llvm::SmallVector<llvm::AttributeSet, 8> Attributes;
  for (unsigned Parameter : Body.Parameters) {
    Arguments.push_back(Original.getArgOperand(Parameter));
    Attributes.push_back(Original.getAttributes().getParamAttrs(Parameter));
  }
  llvm::IRBuilder<> Builder(&Original);
  auto *Call = Builder.CreateCall(Body.Function, Arguments);
  Call->setCallingConv(Original.getCallingConv());
  const auto FunctionAttributes =
      Original.getAttributes().getFnAttrs().removeAttribute(
          Original.getContext(), llvm::Attribute::AllocSize);
  Call->setAttributes(llvm::AttributeList::get(
      Original.getContext(), FunctionAttributes,
      Original.getAttributes().getRetAttrs(), Attributes));
  Call->copyMetadata(Original);
  Call->copyIRFlags(&Original);
  Call->takeName(&Original);
  Original.replaceAllUsesWith(Call);
  Original.eraseFromParent();
}

} // namespace neverd::source_inline

#endif // NEVERD_PIPELINE_SOURCECALLHELPERS_H
