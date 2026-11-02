//===- CodegenDiagnosticTests.cpp - Failed object emission --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// LLVM diagnostics must prevent publication of incomplete machine code.
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/backend/codegen/CodeGen.h"

#include "llvm/IR/DiagnosticInfo.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Module.h"

using namespace neverd;

TEST(CodegenDiagnostics, AllocationErrorsCannotPublishAnIncompleteObject) {
  for (auto Format :
       {BinaryFormat::ELF, BinaryFormat::COFF, BinaryFormat::MachO})
    for (bool Valid : {false, true}) {
      llvm::LLVMContext Context;
      unsigned Errors = 0;
      Context.setDiagnosticHandlerCallBack(
          [](const llvm::DiagnosticInfo *Info, void *State) {
            if (Info->getSeverity() == llvm::DS_Error)
              ++*static_cast<unsigned *>(State);
          },
          &Errors);
      const auto *Handler = Context.getDiagHandlerPtr();
      llvm::Module M("allocation-diagnostic", Context);
      llvm::IRBuilder<> B(Context);
      auto *Type =
          llvm::FunctionType::get(B.getVoidTy(), {B.getInt64Ty()}, false);
      auto *F = llvm::Function::Create(Type, llvm::GlobalValue::ExternalLinkage,
                                       "probe", M);
      B.SetInsertPoint(llvm::BasicBlock::Create(Context, "entry", F));
      // GNU C's "b" must be translated to LLVM's fixed-register "{bx}".
      // The former yields an allocation diagnostic, yet LLVM can still
      // serialize a parseable object with the instruction omitted.
      auto *Asm =
          llvm::InlineAsm::get(Type, "# allocation diagnostic fixture",
                               Valid ? "{bx},~{memory}" : "b,~{memory}", true);
      B.CreateCall(Asm, {F->getArg(0)});
      B.CreateRetVoid();
      const auto Result = Codegen().compile(M, Arch::X64, Format);
      EXPECT_EQ(Result.Success, Valid);
      EXPECT_EQ(Errors == 0, Valid);
      EXPECT_EQ(Result.ObjectData.empty(), !Valid);
      EXPECT_EQ(Context.getDiagHandlerPtr(), Handler);
      EXPECT_EQ(Context.getDiagnosticContext(), &Errors);
      if (!Valid) {
        EXPECT_TRUE(Result.Functions.empty());
        EXPECT_TRUE(Result.Relocations.empty());
      }
    }
}
