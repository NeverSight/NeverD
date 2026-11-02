//===- MedLLVMPortableX87.cpp - Explicit portable generated execution -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Emit a private prepared leaf with an exact host C interface and helper ABI.
//===----------------------------------------------------------------------===//

#include "neverd/backend/RewriteSourceIdentity.h"
#include "neverd/backend/X86/PortableX87EmissionTarget.h"
#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/ir/X86/X87PortableABI.h"
#include "neverd/ir/med/IntrinsicShapes.h"
#include "neverd/ir/med/X86/PortableX87Function.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/Errc.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/TargetParser/Triple.h"

#include <exception>
#include <stdexcept>
#include <vector>

namespace neverd {
namespace {
llvm::Type *abiType(X87PortableABIValueV1 Value, llvm::LLVMContext &Context) {
  switch (Value) {
  case X87PortableABIValueV1::Context:
    return llvm::PointerType::get(Context, 0);
  case X87PortableABIValueV1::I32:
    return llvm::Type::getInt32Ty(Context);
  case X87PortableABIValueV1::I64:
    return llvm::Type::getInt64Ty(Context);
  }
  llvm_unreachable("unhandled portable x87 C ABI value");
}
} // namespace

llvm::Expected<std::unique_ptr<llvm::Module>>
MedLLVMEmitter::emitPortableX87(const PreparedPortableX87Function &Prepared,
                                llvm::LLVMContext &Context,
                                llvm::StringRef OutputTriple) {
  if (auto Error = Prepared.validate())
    return std::move(Error);
  auto Target = portableX87EmissionTarget(OutputTriple);
  if (!Target)
    return Target.takeError();
  // A caller's source/debug/image settings must not enter this alternate ABI.
  // The copied body remains private and unchanged for this complete emission.
  MedLLVMEmitter Emitter;
  std::vector<MedFunc> Functions{Prepared.Function};
  Emitter.PortableBody = &Functions.front();
  Emitter.PortableInterface = &Prepared;
  Emitter.PortableOutputTriple = Target->Triple.str();
  try {
    auto Module = Emitter.emit(Functions, Context, Prepared.name().str(),
                               Arch::X64, {}, nullptr, Target->Format);
    if (!Module || llvm::verifyModule(*Module, &llvm::errs()))
      return llvm::createStringError(llvm::errc::invalid_argument,
                                     "portable x87 emitted invalid LLVM IR");
    for (auto &Function : *Module)
      Function.setMetadata(rewrite_source::FunctionAttachment, nullptr);
    return Module;
  } catch (const std::exception &Error) {
    return llvm::createStringError(llvm::errc::invalid_argument, Error.what());
  }
}

llvm::Function *
MedLLVMEmitter::declarePortableX87Function(const MedFunc &Function) {
  if (&Function != PortableBody || !PortableInterface ||
      Function.Params.size() !=
          PortableInterface->sourceParameterBytes().size() + 2)
    throw std::invalid_argument(
        "portable x87 function has no prepared interface");
  llvm::SmallVector<llvm::Type *> Parameters;
  for (uint16_t Bytes : PortableInterface->sourceParameterBytes())
    Parameters.push_back(llvm::IntegerType::get(*Ctx, Bytes * 8));
  Parameters.push_back(llvm::PointerType::get(*Ctx, 0));
  Parameters.push_back(llvm::PointerType::get(*Ctx, 0));
  auto *Type =
      llvm::FunctionType::get(llvm::Type::getInt32Ty(*Ctx), Parameters, false);
  auto *Result = llvm::Function::Create(
      Type, llvm::GlobalValue::ExternalLinkage, Function.Name, Mod);
  Result->setCallingConv(llvm::CallingConv::C);
  return Result;
}

llvm::Value *
MedLLVMEmitter::emitPortableX87Intrinsic(const MedOp &Op,
                                         llvm::IRBuilder<> &Builder) {
  if (!PortableInterface || CurMedFunc != PortableBody ||
      !portableX87MedShapeIsValid(Op))
    throw std::invalid_argument(
        "portable x87 helper has no prepared exact call contract");
  const auto Id = static_cast<Intrinsic>(Op.Inputs[0].ConstVal);
  const auto &Signature = x87PortableHelperSignatureV1(
      Id == Intrinsic::PortableX87Step ? X87PortableHelperV1::Step
                                       : X87PortableHelperV1::Result);
  llvm::SmallVector<llvm::Type *> ParameterTypes;
  for (auto Parameter : Signature.Parameters)
    ParameterTypes.push_back(abiType(Parameter, *Ctx));
  auto *Type = llvm::FunctionType::get(abiType(Signature.Result, *Ctx),
                                       ParameterTypes, false);
  auto *Helper = Mod->getFunction(Signature.Name);
  if (Helper && (Helper->getFunctionType() != Type ||
                 Helper->getCallingConv() != llvm::CallingConv::C ||
                 !Helper->isDeclaration()))
    throw std::invalid_argument(
        "portable x87 runtime symbol has a conflicting declaration");
  if (!Helper) {
    Helper = llvm::Function::Create(Type, llvm::GlobalValue::ExternalLinkage,
                                    Signature.Name, Mod);
    Helper->setCallingConv(llvm::CallingConv::C);
  }
  llvm::SmallVector<llvm::Value *> Arguments;
  // Only the canonical host execution argument is a pointer. No guest value
  // passes through source CALL pointer recovery or argument-bank inference.
  Arguments.push_back(Builder.GetInsertBlock()->getParent()->getArg(
      PortableInterface->executionParameterIndex()));
  for (unsigned I = 1; I != Signature.Parameters.size(); ++I) {
    const auto &Input = Op.Inputs[I + 1];
    llvm::Value *Value =
        Input.isConst()
            ? llvm::ConstantInt::get(ParameterTypes[I], Input.ConstVal)
            : getVar(Input, Builder);
    if (!Value || Value->getType() != ParameterTypes[I])
      throw std::invalid_argument(
          "portable x87 guest value changed its exact integer type");
    Arguments.push_back(Value);
  }
  auto *Call = Builder.CreateCall(Helper, Arguments);
  Call->setCallingConv(llvm::CallingConv::C);
  // Both calls have ordinary unknown memory effects. Result consumes state;
  // neither call is pure, speculatable, readonly, or eligible for CSE.
  return Call;
}

} // namespace neverd
