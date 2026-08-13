//===- EVMCEmitterTests.cpp - EVM C backend tests -----------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "EVMEmitterTestsDetail.h"
#include "gtest/gtest.h"

namespace neverd::evm {
namespace {

using test::differentialALUProgram;
using test::differentialHarness;
using test::writeTemporarySource;

TEST(EVMCEmitter, ProducesStandaloneCompilableC23) {
  const std::vector<uint8_t> Code = {0x60, 0x01, 0x60, 0x02, 0x60, 0x03, 0x09,
                                     0x34, 0x01, 0x60, 0x00, 0x55, 0x00};
  auto Program = analyze(Code);
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  auto Source = emitC(*Program);
  ASSERT_TRUE(static_cast<bool>(Source)) << llvm::toString(Source.takeError());
  EXPECT_NE(Source->find("#define NEVERD_EVM_WORD_BITS 256u"),
            std::string::npos);
  EXPECT_NE(Source->find("#define NEVERD_EVM_WIDE_WORD_BITS 512u"),
            std::string::npos);
  EXPECT_NE(Source->find("evm_stack[NEVERD_EVM_STACK_LIMIT]"),
            std::string::npos);
  EXPECT_NE(Source->find("neverd_evm_host_op"), std::string::npos);
  EXPECT_NE(Source->find("pc_0:"), std::string::npos);
  EXPECT_NE(Source->find("evm_word negative = v >> NEVERD_EVM_WORD_MSB"),
            std::string::npos);
  EXPECT_EQ(Source->find("(evm_sword)v >>"), std::string::npos);

  const char *Clang =
      std::filesystem::exists("/opt/homebrew/opt/llvm/bin/clang")
          ? "/opt/homebrew/opt/llvm/bin/clang"
          : "clang";
  if (std::system(
          (std::string("command -v ") + Clang + " >/dev/null 2>&1").c_str()) !=
      0)
    GTEST_SKIP() << "clang is unavailable";
  const auto Path = writeTemporarySource(".c", *Source);
  const std::string Command = std::string(Clang) +
                              " -std=c2x -Werror -fsyntax-only -ffreestanding "
                              "-target x86_64-unknown-linux-gnu '" +
                              Path.string() + "'";
  EXPECT_EQ(std::system(Command.c_str()), 0);
  std::error_code EC;
  std::filesystem::remove(Path, EC);
}

TEST(EVMCEmitter, ExecutesDifferentiallyAgainstInterpreter) {
  const std::vector<uint8_t> Code = differentialALUProgram();
  auto Program = analyze(Code);
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  auto Oracle = execute(Program->Low);
  ASSERT_TRUE(static_cast<bool>(Oracle)) << llvm::toString(Oracle.takeError());
  const auto Stored = Oracle->Storage.find(llvm::APInt(kWordBits, 0));
  ASSERT_NE(Stored, Oracle->Storage.end());

  auto Source = emitC(*Program);
  ASSERT_TRUE(static_cast<bool>(Source)) << llvm::toString(Source.takeError());
  const auto SourcePath = writeTemporarySource(".c", *Source);
  const auto ModulePath = writeTemporarySource("-from-c.ll", "");
  const auto HarnessPath = writeTemporarySource(
      "-c-harness.ll",
      differentialHarness(Stored->second, Oracle->Trace.size(), true));
  const auto ExecutablePath = writeTemporarySource("-c-runner", "");

  const char *Clang =
      std::filesystem::exists("/opt/homebrew/opt/llvm/bin/clang")
          ? "/opt/homebrew/opt/llvm/bin/clang"
          : "clang";
  if (std::system(
          (std::string("command -v ") + Clang + " >/dev/null 2>&1").c_str()) !=
      0)
    GTEST_SKIP() << "clang is unavailable";

  // Darwin currently caps frontend _BitInt at 128 bits. Parse the source
  // using a freestanding Linux target, then retarget the produced LLVM IR to
  // the native host for execution.
  const std::string Lower = std::string(Clang) +
                            " -std=c2x -Werror -ffreestanding -target "
                            "x86_64-unknown-linux-gnu -S -emit-llvm '" +
                            SourcePath.string() + "' -o '" +
                            ModulePath.string() + "'";
  ASSERT_EQ(std::system(Lower.c_str()), 0);
  const std::string Link = std::string(Clang) + " '" + ModulePath.string() +
                           "' '" + HarnessPath.string() + "' -o '" +
                           ExecutablePath.string() + "'";
  ASSERT_EQ(std::system(Link.c_str()), 0);
  EXPECT_EQ(std::system(("'" + ExecutablePath.string() + "'").c_str()), 0);

  std::error_code EC;
  std::filesystem::remove(SourcePath, EC);
  std::filesystem::remove(ModulePath, EC);
  std::filesystem::remove(HarnessPath, EC);
  std::filesystem::remove(ExecutablePath, EC);
}
} // namespace
} // namespace neverd::evm
