//===- MIPSObjectCapabilityTests.cpp - Linked LLVM MIPS support -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Check object emission against both the linked registry and NeverD's public
/// architecture query, including SDKs that omit the MIPS backend.
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/ArchSupport.h"
#include "neverd/backend/codegen/CodeGen.h"

#include "llvm/IR/IRBuilder.h"
#include "llvm/MC/TargetRegistry.h"

using namespace neverd;

namespace {

TEST(MIPSObjectCapability, MatchesRelocatableELFEmission) {
  llvm::LLVMContext Context;
  llvm::Module Module(ProjectName, Context);
  auto *Signature =
      llvm::FunctionType::get(llvm::Type::getVoidTy(Context), false);
  auto *Leaf =
      llvm::Function::Create(Signature, llvm::GlobalValue::ExternalLinkage,
                             getArchName(Arch::MIPS32), Module);
  llvm::IRBuilder<>(llvm::BasicBlock::Create(Context, {}, Leaf))
      .CreateRetVoid();

  const auto Result =
      Codegen().compile(Module, Arch::MIPS32, BinaryFormat::ELF);
  const llvm::Triple Triple(llvmCodegenTriple(Arch::MIPS32, BinaryFormat::ELF));
  std::string Diagnostic;
  const auto *Backend = llvm::TargetRegistry::lookupTarget(Triple, Diagnostic);
  EXPECT_EQ(archCodegenSupported(Arch::MIPS32), Backend != nullptr);
  EXPECT_EQ(Result.Success, Backend != nullptr);
  EXPECT_EQ(!Result.ObjectData.empty(), Result.Success);
}

} // namespace
