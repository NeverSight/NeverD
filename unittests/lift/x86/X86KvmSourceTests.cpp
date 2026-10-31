//===- X86KvmSourceTests.cpp - Explicit KVM call contracts ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Verify native ABI admission, conservative effects and SDK boundaries.
//===----------------------------------------------------------------------===//

#include "../../../lib/backend/c/render/X86/X86Virtualization.h"
#include "gtest/gtest.h"

#include "neverd/backend/codegen/CodeGen.h"
#include "neverd/lift/X86/X86HypercallEncoding.h"

#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/ModRef.h"

#include <stdexcept>
#include <string>
#include <vector>

using namespace neverd;

TEST(X86KvmSource, NativeContractRejectsLostInputsEffectsAndMode) {
  for (const auto A : {Arch::X86, Arch::X64})
    for (const char *Mnemonic : {"vmcall", "vmmcall"})
      for (unsigned Mutation = 0; Mutation != 11; ++Mutation) {
        SCOPED_TRACE(static_cast<unsigned>(A));
        SCOPED_TRACE(Mnemonic);
        SCOPED_TRACE(Mutation);
        llvm::LLVMContext Context;
        llvm::Module Module("kvm-contract", Context);
        llvm::IRBuilder<> B(Context);
        auto *Word = A == Arch::X64 ? B.getInt64Ty() : B.getInt32Ty();
        const std::string Name = std::string("llvm.x86.") + Mnemonic +
                                 (A == Arch::X64 ? ".kvm" : ".kvm32");
        auto *F = llvm::Function::Create(
            llvm::FunctionType::get(Word, std::vector<llvm::Type *>(5, Word),
                                    false),
            llvm::GlobalValue::ExternalLinkage, Name, Module);
        for (const auto Attribute :
             {llvm::Attribute::Convergent, llvm::Attribute::NoDuplicate,
              llvm::Attribute::NoUnwind})
          F->addFnAttr(Attribute);
        auto *Owner = llvm::Function::Create(
            llvm::FunctionType::get(Word, false),
            llvm::GlobalValue::ExternalLinkage, "owner", Module);
        B.SetInsertPoint(llvm::BasicBlock::Create(Context, "entry", Owner));
        auto *Call = B.CreateCall(
            F, std::vector<llvm::Value *>(5, llvm::ConstantInt::get(Word, 7)));
        B.CreateRet(Call);
        if (Mutation == 1)
          Call->setOnlyReadsMemory();
        if (Mutation == 2)
          Call->setOnlyWritesMemory();
        if (Mutation == 3)
          Call->setDoesNotReturn();
        if (Mutation == 4)
          Call->setArgOperand(4, B.getInt16(0));
        if (Mutation == 5)
          F->removeFnAttr(llvm::Attribute::Convergent);
        if (Mutation == 6)
          F->removeFnAttr(llvm::Attribute::NoDuplicate);
        if (Mutation == 7)
          F->removeFnAttr(llvm::Attribute::NoUnwind);
        if (Mutation == 8)
          Call->setCallingConv(llvm::CallingConv::Fast);
        if (Mutation == 9)
          llvm::BasicBlock::Create(Context, "definition", F);
        const auto Mode =
            Mutation == 10 ? (A == Arch::X64 ? Arch::X86 : Arch::X64) : A;
        const std::vector<std::string> Arguments = {"number", "a", "b", "c",
                                                    "d"};
        if (Mutation) {
          EXPECT_THROW(renderX86VirtualizationNativeCall(Mode, *Call, "result",
                                                         true, Arguments),
                       std::invalid_argument);
          continue;
        }
        const auto Text = renderX86VirtualizationNativeCall(
            Mode, *Call, "result", true, Arguments);
        ASSERT_TRUE(Text);
        EXPECT_EQ(*Text, std::string("result = __builtin_ia32_") + Mnemonic +
                             (A == Arch::X64 ? "_kvm" : "_kvm32") +
                             "(number, a, b, c, d);\n");
        const auto Unused = renderX86VirtualizationNativeCall(Mode, *Call, {},
                                                              false, Arguments);
        ASSERT_TRUE(Unused);
        EXPECT_EQ(Unused->find(" = "), std::string::npos);
        if (!F->getIntrinsicID()) {
          auto Result = Codegen().compile(Module, A, BinaryFormat::ELF);
          EXPECT_FALSE(Result.Success);
          EXPECT_TRUE(Result.ObjectData.empty());
        }
      }
}

TEST(X86KvmSource, OnlyCompleteCanonicalBytesBindAnAbi) {
  for (const auto Last : {uint8_t(0xc1), uint8_t(0xd9)}) {
    const std::vector<uint8_t> Canonical = {0x0f, 0x01, Last};
    EXPECT_TRUE(x86HypercallEncoding(Canonical));
    for (const auto Prefix : {0x66, 0x67, 0xf0, 0xf2, 0xf3, 0x48}) {
      auto Prefixed = Canonical;
      Prefixed.insert(Prefixed.begin(), Prefix);
      EXPECT_FALSE(x86HypercallEncoding(Prefixed));
    }
    auto Truncated = Canonical;
    Truncated.pop_back();
    EXPECT_FALSE(x86HypercallEncoding(Truncated));
    auto Extended = Canonical;
    Extended.push_back(0xc3);
    EXPECT_FALSE(x86HypercallEncoding(Extended));
  }
  EXPECT_FALSE(x86HypercallEncoding({0x0f, 0x01, 0xd8}));
}
