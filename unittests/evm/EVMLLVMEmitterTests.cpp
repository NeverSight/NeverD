//===- EVMLLVMEmitterTests.cpp - EVM LLVM IR backend tests --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "EVMEmitterTestsDetail.h"
#include "gtest/gtest.h"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Verifier.h"

namespace neverd::evm {
namespace {

using test::differentialALUProgram;
using test::differentialHarness;
using test::writeTemporarySource;

TEST(EVMLLVMEmitter, ProducesVerifiedI256StateMachine) {
  const std::vector<uint8_t> Code = {
      0x60, 0x01, 0x60, 0x02, 0x60, 0x03, 0x08, // ADDMOD
      0x60, 0x0b, 0x56, 0x00, 0x5b, 0x00};      // validated JUMP
  auto Program = analyze(Code);
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());

  llvm::LLVMContext Context;
  auto Module = emitLLVM(*Program, Context);
  ASSERT_TRUE(static_cast<bool>(Module)) << llvm::toString(Module.takeError());
  EXPECT_FALSE(llvm::verifyModule(**Module, &llvm::errs()));

  const std::string IR = emitLLVMText(**Module);
  EXPECT_NE(IR.find("define i32 @evm_execute(ptr"), std::string::npos);
  EXPECT_NE(IR.find("[1024 x i256]"), std::string::npos);
  EXPECT_NE(IR.find("i512"), std::string::npos);
  EXPECT_NE(IR.find("switch i256"), std::string::npos);
  EXPECT_NE(IR.find("neverd_evm_host_op"), std::string::npos);
  EXPECT_NE(IR.find("stack.overflow"), std::string::npos);
}

TEST(EVMLLVMEmitter, EmitsGuardedSignedDivisionAndEnvironmentHooks) {
  const std::vector<uint8_t> Code = {0x60, 0x07, 0x60, 0x00, 0x05, 0x60, 0x80,
                                     0x5f, 0x0b, 0x60, 0x01, 0x60, 0xff, 0x1a,
                                     0x34, 0x01, 0x60, 0x00, 0x55, 0x00};
  auto Program = analyze(Code);
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  llvm::LLVMContext Context;
  auto Module = emitLLVM(*Program, Context);
  ASSERT_TRUE(static_cast<bool>(Module)) << llvm::toString(Module.takeError());
  EXPECT_FALSE(llvm::verifyModule(**Module, &llvm::errs()));
  const std::string IR = emitLLVMText(**Module);
  EXPECT_NE(IR.find("sdiv"), std::string::npos);
  EXPECT_NE(IR.find("select i1"), std::string::npos);
  EXPECT_NE(IR.find("i8 52"), std::string::npos);  // CALLVALUE host opcode.
  EXPECT_NE(IR.find("i8 85"), std::string::npos);  // SSTORE host opcode.
  EXPECT_EQ(IR.find("i8 11,"), std::string::npos); // SIGNEXTEND is inline.
  EXPECT_EQ(IR.find("i8 26,"), std::string::npos); // BYTE is inline.
}

TEST(EVMLLVMEmitter, ExecutesDifferentiallyAgainstInterpreter) {
  const std::vector<uint8_t> Code = differentialALUProgram();
  auto Program = analyze(Code);
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  auto Oracle = execute(Program->Low);
  ASSERT_TRUE(static_cast<bool>(Oracle)) << llvm::toString(Oracle.takeError());
  ASSERT_EQ(Oracle->Status, ExecutionStatus::Stopped);
  const auto Stored = Oracle->Storage.find(llvm::APInt(kWordBits, 0));
  ASSERT_NE(Stored, Oracle->Storage.end());

  llvm::LLVMContext Context;
  auto Module = emitLLVM(*Program, Context);
  ASSERT_TRUE(static_cast<bool>(Module)) << llvm::toString(Module.takeError());
  const auto ModulePath = writeTemporarySource(".ll", emitLLVMText(**Module));

  const std::string Harness =
      differentialHarness(Stored->second, Oracle->Trace.size());
  const auto HarnessPath = writeTemporarySource("-harness.ll", Harness);
  const auto ExecutablePath = writeTemporarySource("-runner", "");

  const char *Clang =
      std::filesystem::exists("/opt/homebrew/opt/llvm/bin/clang")
          ? "/opt/homebrew/opt/llvm/bin/clang"
          : "clang";
  if (std::system(
          (std::string("command -v ") + Clang + " >/dev/null 2>&1").c_str()) !=
      0)
    GTEST_SKIP() << "clang is unavailable";
  const std::string Compile = std::string(Clang) + " '" + ModulePath.string() +
                              "' '" + HarnessPath.string() + "' -o '" +
                              ExecutablePath.string() + "'";
  ASSERT_EQ(std::system(Compile.c_str()), 0);
  EXPECT_EQ(std::system(("'" + ExecutablePath.string() + "'").c_str()), 0);

  std::error_code EC;
  std::filesystem::remove(ModulePath, EC);
  std::filesystem::remove(HarnessPath, EC);
  std::filesystem::remove(ExecutablePath, EC);
}
} // namespace
} // namespace neverd::evm
