//===- EVMEmitterTests.cpp - Cross-backend EVM emitter tests ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// What every generated backend has to agree on, plus the interpreter oracle
/// the per-backend differential tests are checked against.
///
//===----------------------------------------------------------------------===//

#include "EVMEmitterTestsDetail.h"
#include "gtest/gtest.h"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Verifier.h"

namespace neverd::evm {
namespace {

using test::amsterdamDifferentialProgram;
using test::anvilPort;
using test::bytecodeHex;
using test::differentialALUProgram;
using test::differentialHarness;
using test::differentialMemoryProgram;
using test::kAnvilPrivateKey;
using test::kAnvilTestAddress;
using test::wordHex;
using test::writeTemporarySource;

TEST(EVMEmitters, ProduceValidEmptyPrograms) {
  EVMProgram Program;

  llvm::LLVMContext Context;
  auto Module = emitLLVM(Program, Context);
  ASSERT_TRUE(static_cast<bool>(Module)) << llvm::toString(Module.takeError());
  EXPECT_FALSE(llvm::verifyModule(**Module, &llvm::errs()));

  auto CSource = emitC(Program);
  ASSERT_TRUE(static_cast<bool>(CSource))
      << llvm::toString(CSource.takeError());
  auto SoliditySource = emitSolidity(Program);
  ASSERT_TRUE(static_cast<bool>(SoliditySource))
      << llvm::toString(SoliditySource.takeError());

  const char *Clang =
      std::filesystem::exists("/opt/homebrew/opt/llvm/bin/clang")
          ? "/opt/homebrew/opt/llvm/bin/clang"
          : "clang";
  if (std::system(
          (std::string("command -v ") + Clang + " >/dev/null 2>&1").c_str()) !=
          0 ||
      std::system("command -v solc >/dev/null 2>&1") != 0)
    GTEST_SKIP() << "clang or solc is unavailable";

  const auto CPath = writeTemporarySource("-empty.c", *CSource);
  const auto SolidityPath = writeTemporarySource("-empty.sol", *SoliditySource);
  const std::string CompileC = std::string(Clang) +
                               " -std=c2x -Werror -fsyntax-only -ffreestanding "
                               "-target x86_64-unknown-linux-gnu '" +
                               CPath.string() + "'";
  const std::string CompileSolidity =
      "solc --bin '" + SolidityPath.string() + "' >/dev/null 2>&1";
  EXPECT_EQ(std::system(CompileC.c_str()), 0);
  EXPECT_EQ(std::system(CompileSolidity.c_str()), 0);

  std::error_code EC;
  std::filesystem::remove(CPath, EC);
  std::filesystem::remove(SolidityPath, EC);
}

TEST(EVMEmitters, AmsterdamOpcodesExecuteAcrossGeneratedBackends) {
  AnalyzeOptions Amsterdam;
  Amsterdam.Fork = Hardfork::Amsterdam;
  const std::vector<uint8_t> Code = amsterdamDifferentialProgram();
  auto Program = analyze(Code, Amsterdam);
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());

  constexpr uint64_t SlotNumber = 0x123456789abcdef0ULL;
  ExecutionEnvironment Environment;
  Environment.SlotNumber = SlotNumber;
  auto Oracle = execute(Program->Low, std::move(Environment));
  ASSERT_TRUE(static_cast<bool>(Oracle)) << llvm::toString(Oracle.takeError());
  ASSERT_EQ(Oracle->Status, ExecutionStatus::Stopped);
  const auto Stored = Oracle->Storage.find(llvm::APInt(kWordBits, 0));
  ASSERT_NE(Stored, Oracle->Storage.end());

  llvm::LLVMContext Context;
  auto Module = emitLLVM(*Program, Context);
  ASSERT_TRUE(static_cast<bool>(Module)) << llvm::toString(Module.takeError());
  EXPECT_FALSE(llvm::verifyModule(**Module, &llvm::errs()));
  const std::string LLVMText = emitLLVMText(**Module);
  EXPECT_NE(LLVMText.find("@evm_stack_peek(ptr %stack, ptr %sp, i32 17)"),
            std::string::npos);
  EXPECT_NE(LLVMText.find("@evm_stack_swap"), std::string::npos);

  auto CSource = emitC(*Program);
  ASSERT_TRUE(static_cast<bool>(CSource))
      << llvm::toString(CSource.takeError());
  EXPECT_NE(CSource->find("evm_peek(evm_stack, evm_sp, 17u)"),
            std::string::npos);
  EXPECT_NE(CSource->find("evm_swap(evm_stack, evm_sp, 18u)"),
            std::string::npos);
  const std::string SlotHostCall =
      "neverd_evm_host_op(environment, 0x" +
      llvm::utohexstr(opcodeByte(Opcode::SLOTNUM)) + "u";
  EXPECT_NE(CSource->find(SlotHostCall), std::string::npos);

  auto SoliditySource = emitSolidity(*Program);
  ASSERT_TRUE(static_cast<bool>(SoliditySource))
      << llvm::toString(SoliditySource.takeError());
  EXPECT_NE(SoliditySource->find("_evmSwap(evmStack, evmSP, 18, pc)"),
            std::string::npos);
  EXPECT_NE(SoliditySource->find("_evmSwap(evmStack, evmSP, 3, pc)"),
            std::string::npos);

  const char *Clang =
      std::filesystem::exists("/opt/homebrew/opt/llvm/bin/clang")
          ? "/opt/homebrew/opt/llvm/bin/clang"
          : "clang";
  if (std::system(
          (std::string("command -v ") + Clang + " >/dev/null 2>&1").c_str()) !=
      0)
    GTEST_SKIP() << "clang is unavailable";

  const llvm::APInt SlotWord(kWordBits, SlotNumber);
  const auto LLVMPath = writeTemporarySource("-amsterdam.ll", LLVMText);
  const auto LLVMHarnessPath = writeTemporarySource(
      "-amsterdam-harness.ll",
      differentialHarness(Stored->second, Oracle->Trace.size(), false,
                          SlotWord));
  const auto LLVMExecutablePath = writeTemporarySource("-amsterdam-runner", "");
  const std::string CompileLLVM =
      std::string(Clang) + " '" + LLVMPath.string() + "' '" +
      LLVMHarnessPath.string() + "' -o '" + LLVMExecutablePath.string() + "'";
  ASSERT_EQ(std::system(CompileLLVM.c_str()), 0);
  EXPECT_EQ(std::system(("'" + LLVMExecutablePath.string() + "'").c_str()), 0);

  const auto CPath = writeTemporarySource("-amsterdam.c", *CSource);
  const auto CModulePath = writeTemporarySource("-amsterdam-from-c.ll", "");
  const auto CHarnessPath = writeTemporarySource(
      "-amsterdam-c-harness.ll",
      differentialHarness(Stored->second, Oracle->Trace.size(), true,
                          SlotWord));
  const auto CExecutablePath = writeTemporarySource("-amsterdam-c-runner", "");
  const std::string LowerC = std::string(Clang) +
                             " -std=c2x -Werror -ffreestanding -target "
                             "x86_64-unknown-linux-gnu -S -emit-llvm '" +
                             CPath.string() + "' -o '" + CModulePath.string() +
                             "'";
  ASSERT_EQ(std::system(LowerC.c_str()), 0);
  const std::string LinkC = std::string(Clang) + " '" + CModulePath.string() +
                            "' '" + CHarnessPath.string() + "' -o '" +
                            CExecutablePath.string() + "'";
  ASSERT_EQ(std::system(LinkC.c_str()), 0);
  EXPECT_EQ(std::system(("'" + CExecutablePath.string() + "'").c_str()), 0);

  std::optional<std::filesystem::path> SolidityPath;
  if (std::system("command -v solc >/dev/null 2>&1") == 0) {
    SolidityPath = writeTemporarySource("-amsterdam.sol", *SoliditySource);
    const std::string CompileSolidity =
        "solc --bin '" + SolidityPath->string() + "' >/dev/null 2>&1";
    EXPECT_EQ(std::system(CompileSolidity.c_str()), 0);
  }

  std::error_code EC;
  for (const auto &Path : {LLVMPath, LLVMHarnessPath, LLVMExecutablePath, CPath,
                           CModulePath, CHarnessPath, CExecutablePath})
    std::filesystem::remove(Path, EC);
  if (SolidityPath)
    std::filesystem::remove(*SolidityPath, EC);
}

TEST(EVMInterpreter, MatchesCanonicalLegacyALUAndMemoryOnAnvil) {
#if defined(_WIN32)
  GTEST_SKIP() << "Anvil differential test requires a POSIX shell";
#else
  if (std::system("command -v anvil >/dev/null 2>&1 && "
                  "command -v cast >/dev/null 2>&1 && "
                  "command -v jq >/dev/null 2>&1") != 0)
    GTEST_SKIP() << "anvil, cast, or jq is unavailable";

  // The installed external EVM may predate Fusaka, so this independent oracle
  // covers the complete pre-Fusaka scalar ALU and leaves CLZ to the dedicated
  // Fusaka vectors plus the three generated-backend differential tests.
  const std::vector<uint8_t> Code = differentialALUProgram(false);
  AnalyzeOptions Options;
  Options.Fork = Hardfork::Cancun;
  auto Program = analyze(Code, Options);
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  auto Oracle = execute(Program->Low);
  ASSERT_TRUE(static_cast<bool>(Oracle)) << llvm::toString(Oracle.takeError());
  const auto Stored = Oracle->Storage.find(llvm::APInt(kWordBits, 0));
  ASSERT_NE(Stored, Oracle->Storage.end());

  const std::vector<uint8_t> MemoryCode = differentialMemoryProgram();
  auto MemoryProgram = analyze(MemoryCode, Options);
  ASSERT_TRUE(static_cast<bool>(MemoryProgram))
      << llvm::toString(MemoryProgram.takeError());
  const std::vector<uint8_t> Calldata = {0x00, 0x01, 0x7f, 0x80, 0xff, 0x42,
                                         0x10, 0x20, 0x30, 0x40, 0x50};
  ExecutionEnvironment Environment;
  Environment.Calldata = Calldata;
  auto MemoryOracle = execute(MemoryProgram->Low, std::move(Environment));
  ASSERT_TRUE(static_cast<bool>(MemoryOracle))
      << llvm::toString(MemoryOracle.takeError());
  ASSERT_EQ(MemoryOracle->Status, ExecutionStatus::Returned);
  ASSERT_EQ(MemoryOracle->ReturnData.size(), kWordBytes);

  const auto MarkerPath = writeTemporarySource("-raw-anvil", "");
  const unsigned Port = anvilPort(MarkerPath);
  const std::string URL = "http://127.0.0.1:" + std::to_string(Port);
  const std::string Runtime = bytecodeHex(Code);
  const std::string Expected = wordHex(Stored->second);
  const std::string MemoryRuntime = bytecodeHex(MemoryCode);
  const std::string CalldataHex = bytecodeHex(Calldata);
  const std::string ExpectedReturn = bytecodeHex(MemoryOracle->ReturnData);
  const std::string Command =
      "set -eu; anvil --silent --port " + std::to_string(Port) +
      " >/dev/null 2>&1 & anvil_pid=$!; "
      "trap 'kill $anvil_pid >/dev/null 2>&1 || true' EXIT; "
      "ready=0; for attempt in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 "
      "17 18 19 20; do if cast block-number --rpc-url '" +
      URL +
      "' >/dev/null 2>&1; then ready=1; break; fi; sleep 0.1; done; "
      "test $ready -eq 1; "
      "cast rpc anvil_setCode '" +
      kAnvilTestAddress.str() + "' '" + Runtime + "' --rpc-url '" + URL +
      "' >/dev/null; "
      "receipt=$(cast send '" +
      kAnvilTestAddress.str() + "' 0x --rpc-url '" + URL + "' --private-key '" +
      kAnvilPrivateKey.str() +
      "' --json); "
      "status=$(printf '%s' \"$receipt\" | jq -r .status); "
      "if test \"$status\" != 0x1; then printf 'transaction status: %s\\n' "
      "\"$status\" >&2; exit 1; fi; "
      "value=$(cast storage '" +
      kAnvilTestAddress.str() + "' 0 --rpc-url '" + URL +
      "'); "
      "if test \"$value\" != '" +
      Expected + "'; then printf 'storage: expected %s, got %s\\n' '" +
      Expected +
      "' \"$value\" >&2; exit 1; fi; "
      "cast rpc anvil_setCode '" +
      kAnvilTestAddress.str() + "' '" + MemoryRuntime + "' --rpc-url '" + URL +
      "' >/dev/null; "
      "actual=$(cast call '" +
      kAnvilTestAddress.str() + "' '" + CalldataHex + "' --rpc-url '" + URL +
      "'); "
      "if test \"$actual\" != '" +
      ExpectedReturn + "'; then printf 'return: expected %s, got %s\\n' '" +
      ExpectedReturn + "' \"$actual\" >&2; exit 1; fi;";
  EXPECT_EQ(std::system(Command.c_str()), 0);

  std::error_code EC;
  std::filesystem::remove(MarkerPath, EC);
#endif
}
} // namespace
} // namespace neverd::evm
