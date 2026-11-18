//===- LLVMTargetInitializationTests.cpp - Linked backend registry --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// An SDK with extra backends must still link and register NeverD's backends.
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/support/LLVMTargetInitialization.h"

#include "llvm/MC/TargetRegistry.h"

#include <set>
#include <string>

TEST(LLVMTargetInitialization, RegistersExactlyTheLinkedCodegenBackends) {
  neverd::initializeLLVMCodegenTargets();
  neverd::initializeLLVMCodegenTargets();
  const std::set<std::string> Expected = {
#define NEVERD_LLVM_CODEGEN_TARGET(Name) #Name,
#include "neverd/support/LLVMCodegenTargets.def"
  };
  std::set<std::string> Actual;
  for (const llvm::Target &Target : llvm::TargetRegistry::targets()) {
    SCOPED_TRACE(Target.getName());
    Actual.emplace(Target.getBackendName());
    EXPECT_TRUE(Target.hasTargetMachine());
    EXPECT_TRUE(Target.hasMCAsmBackend());
    EXPECT_TRUE(Target.hasMCAsmParser());
  }
  EXPECT_EQ(Actual, Expected);
}
