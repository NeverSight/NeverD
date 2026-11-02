//===- X87PortableEmissionTests.cpp - Exact portable generated interfaces
//--===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "X87PortableTestImage.h"
#include "gtest/gtest.h"

#include "neverd/backend/RewriteSourceIdentity.h"
#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/backend/c/LLVMC/LLVMCEmitter.h"
#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/ir/X86/X87PortableABI.h"
#include "neverd/ir/med/X86/PortableX87Function.h"

#include "llvm/IR/Instructions.h"
#include "llvm/IR/Verifier.h"
#include "llvm/TargetParser/Triple.h"

using namespace neverd;
using namespace neverd::test;

namespace {

TEST(X87PortableEmission, RealControlFlowKeepsExactCallsAndNoSourceProof) {
  for (bool NoOpt : {false, true})
    for (const auto &Bytes : {portableX87LeafBytes(), portableX87BranchBytes(),
                              portableX87LoopBytes()})
      for (const char *Triple :
           {"x86_64-unknown-linux-gnu", "aarch64-unknown-linux-gnu",
            "arm64-apple-macosx15.0.0", "x86_64-pc-windows-msvc"}) {
        SCOPED_TRACE(NoOpt);
        SCOPED_TRACE(Bytes.size());
        SCOPED_TRACE(Triple);
        auto Source = liftPortableX87Fixture(Bytes, NoOpt);
        ASSERT_TRUE(static_cast<bool>(Source))
            << llvm::toString(Source.takeError());
        auto Prepared =
            preparePortableX87Function(*Source, Arch::X64, BinaryFormat::ELF);
        ASSERT_TRUE(static_cast<bool>(Prepared))
            << llvm::toString(Prepared.takeError());
        llvm::LLVMContext Context;
        MedLLVMEmitter Emitter;
        auto Module = Emitter.emitPortableX87(*Prepared, Context, Triple);
        ASSERT_TRUE(static_cast<bool>(Module))
            << llvm::toString(Module.takeError());
        ASSERT_FALSE(llvm::verifyModule(**Module, &llvm::errs()));
        EXPECT_EQ((*Module)->getDataLayout().getPointerSizeInBits(), 64u);
        EXPECT_EQ((*Module)->getTargetTriple().str(),
                  llvm::Triple::normalize(Triple));
        const auto *Function = (*Module)->getFunction(Prepared->name());
        ASSERT_NE(Function, nullptr);
        EXPECT_TRUE(Function->getReturnType()->isIntegerTy(32));
        EXPECT_EQ(Function->getCallingConv(), llvm::CallingConv::C);
        EXPECT_EQ(Function->arg_size(),
                  Prepared->sourceParameterBytes().size() + 2);
        for (unsigned I = 0; I != Prepared->sourceParameterBytes().size(); ++I)
          EXPECT_TRUE(Function->getArg(I)->getType()->isIntegerTy(
              Prepared->sourceParameterBytes()[I] * 8));
        EXPECT_TRUE(Function->getArg(Prepared->executionParameterIndex())
                        ->getType()
                        ->isPointerTy());
        EXPECT_TRUE(Function->getArg(Prepared->sourceReturnParameterIndex())
                        ->getType()
                        ->isPointerTy());
        unsigned Steps = 0, Results = 0;
        for (const auto &F : **Module) {
          EXPECT_EQ(F.getMetadata(rewrite_source::FunctionAttachment), nullptr);
          for (const auto &Block : F)
            for (const auto &Instruction : Block) {
              EXPECT_FALSE(Instruction.getType()->isFloatingPointTy());
              if (const auto *Call =
                      llvm::dyn_cast<llvm::CallBase>(&Instruction)) {
                ASSERT_FALSE(Call->isInlineAsm());
                ASSERT_NE(Call->getCalledFunction(), nullptr);
                const auto Name = Call->getCalledFunction()->getName();
                if (Name == "nvd_x87_v1_step") {
                  ++Steps;
                  EXPECT_EQ(Call->arg_size(), 4u);
                  EXPECT_TRUE(Call->getType()->isIntegerTy(32));
                  EXPECT_TRUE(
                      llvm::isa<llvm::ConstantInt>(Call->getArgOperand(1)));
                  EXPECT_TRUE(
                      llvm::isa<llvm::ConstantInt>(Call->getArgOperand(2)));
                  EXPECT_TRUE(
                      Call->getArgOperand(3)->getType()->isIntegerTy(64));
                } else {
                  ASSERT_EQ(Name, "nvd_x87_v1_result");
                  ++Results;
                  EXPECT_EQ(Call->arg_size(), 1u);
                  EXPECT_TRUE(Call->getType()->isIntegerTy(64));
                }
                EXPECT_EQ(
                    Call->getArgOperand(0),
                    Function->getArg(Prepared->executionParameterIndex()));
                EXPECT_FALSE(Call->onlyReadsMemory());
                EXPECT_FALSE(Call->doesNotAccessMemory());
              }
            }
        }
        EXPECT_EQ(Steps,
                  Bytes.size() == portableX87LoopBytes().size() ? 6u : 5u);
        EXPECT_EQ(Results, 1u);
        HighCEmitter High;
        auto Text = High.emitPortableX87(*Prepared, Triple);
        ASSERT_TRUE(static_cast<bool>(Text))
            << llvm::toString(Text.takeError());
        EXPECT_EQ(Text->find("neverd.entry:"), std::string::npos);
        EXPECT_EQ(Text->find("immintrin.h"), std::string::npos);
        EXPECT_EQ(Text->find("x86intrin.h"), std::string::npos);
        EXPECT_NE(Text->find(Prepared->name().str()), std::string::npos);
        for (const auto &Signature : x87PortableHelperSignaturesV1())
          EXPECT_NE(Text->find(Signature.CDeclaration.str()),
                    std::string::npos);
        CEmitterOptions Options;
        Options.PreserveLLVMFunctionTypes = true;
        Options.EmitComments = false;
        std::string LLVMText;
        llvm::raw_string_ostream Out(LLVMText);
        ASSERT_TRUE(LLVMCEmitter().emit(**Module, Out, Options));
        EXPECT_NE(LLVMText.find("nvd_x87_v1_step"), std::string::npos);
      }
}

TEST(X87PortableEmission, RejectsUnsupportedOutputRepresentations) {
  auto Source = liftPortableX87Fixture(portableX87LeafBytes());
  ASSERT_TRUE(static_cast<bool>(Source)) << llvm::toString(Source.takeError());
  auto Prepared =
      preparePortableX87Function(*Source, Arch::X64, BinaryFormat::ELF);
  ASSERT_TRUE(static_cast<bool>(Prepared))
      << llvm::toString(Prepared.takeError());
  for (const char *Triple :
       {"i386-unknown-linux-gnu", "armv7-unknown-linux-gnueabihf",
        "aarch64_be-unknown-linux-gnu", "x86_64-unknown-linux-gnux32"}) {
    SCOPED_TRACE(Triple);
    llvm::LLVMContext Context;
    auto Module = MedLLVMEmitter().emitPortableX87(*Prepared, Context, Triple);
    ASSERT_FALSE(static_cast<bool>(Module));
    llvm::consumeError(Module.takeError());
    auto Text = HighCEmitter().emitPortableX87(*Prepared, Triple);
    ASSERT_FALSE(static_cast<bool>(Text));
    llvm::consumeError(Text.takeError());
  }
}

} // namespace
