//===- WindowsRegistrationCxxFrameTests.cpp - PE32 C++ frame ABI --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/support/LLVMTargetInitialization.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/MC/BinaryRewrite.h"
#ifdef LLVM_NEVERD_X86_CXX_CATCH_SUBFIELDS
#include "llvm/IR/WinEHFrame.h"
#endif
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetMachine.h"

#include <cstdlib>
#include <functional>
#include <memory>
#include <mutex>

#ifdef LLVM_NEVERD_X86_CXX_CATCH_SUBFIELDS
namespace {

// These functions link against the genuine MSVC fixture's RTTI, throwing helper
// and destructor. They establish the generated frame ABI, not source lowering.
std::unique_ptr<llvm::Module> makeCxxFrameProfiles(llvm::LLVMContext &Context) {
  auto Owner =
      std::make_unique<llvm::Module>("registration-cxx-frame", Context);
  llvm::Module &Module = *Owner;
  Module.setTargetTriple(llvm::Triple("i686-pc-windows-msvc"));
  Module.setDataLayout("e-m:x-p:32:32-i64:64-n8:16:32-S32");
  llvm::IRBuilder<> B(Context);
  auto *I32 = B.getInt32Ty();
  auto *Ptr = B.getPtrTy();
  auto Personality = Module.getOrInsertFunction(
      "__CxxFrameHandler3", llvm::FunctionType::get(I32, {}, true));
  auto Throw = Module.getOrInsertFunction(
      "registration_cxx_throw", llvm::FunctionType::get(B.getVoidTy(), false));
  auto Destructor = Module.getOrInsertFunction(
      "??1RegistrationGuard@@QAE@XZ",
      llvm::FunctionType::get(B.getVoidTy(), {Ptr}, false));
  llvm::cast<llvm::Function>(Destructor.getCallee())
      ->setCallingConv(llvm::CallingConv::X86_ThisCall);
  auto *Type = new llvm::GlobalVariable(Module, B.getInt8Ty(), true,
                                        llvm::GlobalValue::ExternalLinkage,
                                        nullptr, "??_R0H@8");
  auto *Trace = new llvm::GlobalVariable(Module, I32, false,
                                         llvm::GlobalValue::ExternalLinkage,
                                         nullptr, "registration_cxx_trace");
  auto *Caught = new llvm::GlobalVariable(Module, I32, false,
                                          llvm::GlobalValue::ExternalLinkage,
                                          nullptr, "registration_cxx_caught");
  for (const bool Aligned : {false, true})
    for (const bool Reference : {false, true}) {
      const std::string Name = std::string("registration_cxx_generated_") +
                               (Aligned ? "aligned_" : "") +
                               (Reference ? "reference" : "value");
      auto *Parent = llvm::Function::Create(llvm::FunctionType::get(I32, false),
                                            llvm::GlobalValue::ExternalLinkage,
                                            Name, Module);
      Parent->setDLLStorageClass(llvm::GlobalValue::DLLExportStorageClass);
      Parent->addFnAttr(llvm::RewriteWinX86CxxFrameAttribute);
      Parent->addFnAttr("frame-pointer", "all");
      Parent->setPersonalityFn(
          llvm::cast<llvm::Constant>(Personality.getCallee()));
      auto Block = [&](const char *Name) {
        return llvm::BasicBlock::Create(Context, Name, Parent);
      };
      auto *Entry = Block("entry");
      auto *Normal = Block("normal");
      auto *InnerCleanup = Block("inner.cleanup");
      auto *OuterCleanup = Block("outer.cleanup");
      auto *Dispatch = Block("dispatch");
      auto *Handler = Block("handler");
      auto *Handled = Block("handled");
      B.SetInsertPoint(Entry);
      auto *Frame = B.CreateAlloca(llvm::ArrayType::get(B.getInt8Ty(), 64),
                                   nullptr, "logical.frame");
      Frame->setAlignment(llvm::Align(Aligned ? 64 : 4));
      auto Field = [&](unsigned Offset) {
        return B.CreateInBoundsGEP(B.getInt8Ty(), Frame, B.getInt32(Offset));
      };
      auto *Outer = Field(4);
      auto *Inner = Field(20);
      auto *Object = Field(44);
      auto *Result = Field(52);
      B.CreateCall(llvm::Intrinsic::getOrInsertDeclaration(
                       &Module, llvm::Intrinsic::localescape),
                   {Frame});
      B.CreateStore(B.getInt32(1), Outer);
      B.CreateStore(B.getInt32(2), Inner);
      B.CreateInvoke(Throw, Normal, InnerCleanup);
      B.SetInsertPoint(Normal);
      B.CreateUnreachable(); // The fixture helper always throws int(7).
      auto Cleanup = [&](llvm::BasicBlock *At, llvm::Value *Object,
                         llvm::BasicBlock *Next) {
        B.SetInsertPoint(At);
        auto *Pad = B.CreateCleanupPad(llvm::ConstantTokenNone::get(Context));
        auto *Call = B.CreateCall(Destructor, {Object},
                                  {llvm::OperandBundleDef("funclet", Pad)});
        Call->setCallingConv(llvm::CallingConv::X86_ThisCall);
        Call->setDoesNotThrow();
        B.CreateCleanupRet(Pad, Next);
      };
      Cleanup(InnerCleanup, Inner, OuterCleanup);
      Cleanup(OuterCleanup, Outer, Dispatch);
      B.SetInsertPoint(Dispatch);
      auto *Switch = B.CreateCatchSwitch(llvm::ConstantTokenNone::get(Context),
                                         nullptr, 1);
      Switch->addHandler(Handler);
      B.SetInsertPoint(Handler);
      auto *Pad = B.CreateCatchPad(
          Switch, {Type, B.getInt32(Reference ? 8 : 0), Frame});
      Pad->setMetadata(
          llvm::RewriteWinX86CxxCatchObjectAttachment,
          llvm::MDNode::get(Context,
                            {llvm::ConstantAsMetadata::get(B.getInt32(1)),
                             llvm::ConstantAsMetadata::get(B.getInt32(44)),
                             llvm::ConstantAsMetadata::get(B.getInt32(4))}));
      llvm::Value *ValueAddress =
          Reference ? B.CreateLoad(Ptr, Object) : Object;
      auto *Value = B.CreateLoad(I32, ValueAddress);
      llvm::Value *CaughtValue = Value;
      if (Reference) {
        CaughtValue = B.CreateAdd(Value, B.getInt32(11));
        B.CreateStore(CaughtValue, ValueAddress);
      }
      auto *Previous = B.CreateLoad(I32, Trace);
      Previous->setVolatile(true);
      B.CreateStore(
           B.CreateAdd(B.CreateMul(Previous, B.getInt32(10)), B.getInt32(3)),
           Trace)
          ->setVolatile(true);
      B.CreateStore(CaughtValue, Caught)->setVolatile(true);
      B.CreateStore(Value, Result);
      B.CreateCatchRet(Pad, Handled);
      B.SetInsertPoint(Handled);
      B.CreateRet(B.CreateLoad(I32, Result));
    }
  return Owner;
}

TEST(WindowsRegistrationCxxFrame, EmitsGenuineRuntimeCatchFrameProfiles) {
  llvm::LLVMContext Context;
  auto Owner = makeCxxFrameProfiles(Context);
  llvm::Module &Module = *Owner;
  ASSERT_FALSE(llvm::verifyModule(Module, &llvm::errs()));
  static std::once_flag Once;
  std::call_once(Once, [] { neverd::initializeLLVMCodegenTargets(); });
  std::string Error;
  const auto *Target =
      llvm::TargetRegistry::lookupTarget(Module.getTargetTriple(), Error);
  ASSERT_NE(Target, nullptr) << Error;
  llvm::TargetOptions Options;
  std::unique_ptr<llvm::TargetMachine> Machine(Target->createTargetMachine(
      Module.getTargetTriple(), "i686", "", Options, llvm::Reloc::Static));
  ASSERT_NE(Machine, nullptr);
  Module.setDataLayout(Machine->createDataLayout());
  llvm::SmallVector<char, 0> Bytes;
  llvm::raw_svector_ostream Output(Bytes);
  llvm::legacy::PassManager Passes;
  ASSERT_FALSE(Machine->addPassesToEmitFile(Passes, Output, nullptr,
                                            llvm::CodeGenFileType::ObjectFile));
  Passes.run(Module);
  ASSERT_FALSE(Bytes.empty());
  if (const char *Path =
          std::getenv("NEVERD_REGISTRATION_CXX_RUNTIME_OBJECT")) {
    std::error_code Code;
    llvm::raw_fd_ostream File(Path, Code);
    ASSERT_FALSE(Code) << Code.message();
    File.write(Bytes.data(), Bytes.size());
    File.close();
    ASSERT_FALSE(File.has_error());
  }
}

TEST(WindowsRegistrationCxxFrame, RejectsMutatedObjectsBeforeCodeGen) {
  auto Extent = [](llvm::CatchPadInst &Pad, uint32_t Offset, uint32_t Size) {
    llvm::IRBuilder<> B(Pad.getContext());
    Pad.setMetadata(
        llvm::RewriteWinX86CxxCatchObjectAttachment,
        llvm::MDNode::get(Pad.getContext(),
                          {llvm::ConstantAsMetadata::get(B.getInt32(1)),
                           llvm::ConstantAsMetadata::get(B.getInt32(Offset)),
                           llvm::ConstantAsMetadata::get(B.getInt32(Size))}));
  };
  struct Mutation {
    const char *Name;
    const char *Diagnostic;
    std::function<void(llvm::CatchPadInst &)> Apply;
  };
  const Mutation Mutations[] = {
      {"missing extent", "no checked extent",
       [](auto &Pad) {
         Pad.setMetadata(llvm::RewriteWinX86CxxCatchObjectAttachment, nullptr);
       }},
      {"one-past object", "exceeds its static frame",
       [&](auto &Pad) { Extent(Pad, 64, 4); }},
      {"partly outside", "exceeds its static frame",
       [&](auto &Pad) { Extent(Pad, 61, 4); }},
      {"wrapped offset", "exceeds its static frame",
       [&](auto &Pad) { Extent(Pad, UINT32_MAX - 3, 4); }},
      {"empty object", "no checked extent",
       [&](auto &Pad) { Extent(Pad, 44, 0); }},
      {"missing frame marker", "requires the PE32 MSVC C++ ABI",
       [](auto &Pad) {
         Pad.getFunction()->removeFnAttr(llvm::RewriteWinX86CxxFrameAttribute);
       }},
      {"changed marker value", "requires the PE32 MSVC C++ ABI",
       [](auto &Pad) {
         Pad.getFunction()->addFnAttr(llvm::RewriteWinX86CxxFrameAttribute,
                                      "unrecognized");
       }},
      {"address recipe", "exceeds its static frame",
       [](auto &Pad) {
         llvm::IRBuilder<> B(
             Pad.getFunction()->getEntryBlock().getTerminator());
         Pad.setArgOperand(2, B.CreateInBoundsGEP(B.getInt8Ty(),
                                                  Pad.getArgOperand(2),
                                                  B.getInt32(44)));
       }},
      {"null object with extent", "null catch object has an extent",
       [](auto &Pad) {
         Pad.setArgOperand(2,
                           llvm::ConstantPointerNull::get(
                               llvm::PointerType::getUnqual(Pad.getContext())));
       }},
      {"wrong personality", "requires the PE32 MSVC C++ ABI",
       [](auto &Pad) {
         auto &M = *Pad.getModule();
         auto Personality = M.getOrInsertFunction(
             "_except_handler3",
             llvm::FunctionType::get(llvm::Type::getInt32Ty(M.getContext()), {},
                                     true));
         Pad.getFunction()->setPersonalityFn(
             llvm::cast<llvm::Constant>(Personality.getCallee()));
       }},
  };
  for (const Mutation &Change : Mutations) {
    SCOPED_TRACE(Change.Name);
    llvm::LLVMContext Context;
    auto Module = makeCxxFrameProfiles(Context);
    ASSERT_FALSE(llvm::verifyModule(*Module));
    auto *Function = Module->getFunction("registration_cxx_generated_value");
    llvm::CatchPadInst *Pad = nullptr;
    for (auto &Block : *Function)
      if (auto *Candidate =
              llvm::dyn_cast<llvm::CatchPadInst>(Block.getFirstNonPHIIt()))
        Pad = Candidate;
    ASSERT_NE(Pad, nullptr);
    Change.Apply(*Pad);
    std::string Errors;
    llvm::raw_string_ostream Output(Errors);
    EXPECT_TRUE(llvm::verifyModule(*Module, &Output));
    EXPECT_NE(Errors.find(Change.Diagnostic), std::string::npos) << Errors;
  }
}

} // namespace
#endif
