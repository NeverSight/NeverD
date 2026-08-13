//===- EVMSolidityEmitterTests.cpp - EVM Solidity backend tests ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "EVMEmitterTestsDetail.h"
#include "gtest/gtest.h"

namespace neverd::evm {
namespace {

using test::anvilPort;
using test::differentialALUProgram;
using test::kAnvilPrivateKey;
using test::kSStoreValueArgumentIndex;
using test::writeTemporarySource;

TEST(EVMSolidityEmitter, ProducesCompilableRecoveredContractAndStateMachine) {
  const std::vector<uint8_t> Code = {
      0x60, 0x00, 0x35, 0x60, 0xe0, 0x1c, 0x80, 0x63, 0x12, 0x34,
      0x56, 0x78, 0x14, 0x60, 0x15, 0x57, 0x5b, 0x60, 0x00, 0x80,
      0xfd, 0x5b, 0x60, 0x2a, 0x60, 0x00, 0x52, 0x60, 0x20, 0x60,
      0x00, 0xf3, 0x60, 0x03, 0x54, 0x50, 0x00};
  auto Program = analyze(Code);
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  auto Source = emitSolidity(*Program);
  ASSERT_TRUE(static_cast<bool>(Source)) << llvm::toString(Source.takeError());
  EXPECT_NE(Source->find("pragma solidity"), std::string::npos);
  EXPECT_NE(Source->find("abstract contract NeverDRecovered"),
            std::string::npos);
  EXPECT_NE(Source->find("selector 0x12345678"), std::string::npos);
  EXPECT_NE(Source->find("function func_12345678"), std::string::npos);
  EXPECT_NE(Source->find("uint256[1024] memory evmStack"), std::string::npos);
  EXPECT_NE(Source->find("constant recovered_storage_slot_3"),
            std::string::npos);
  EXPECT_NE(Source->find("error EVMInvalidJump"), std::string::npos);

  if (std::system("command -v solc >/dev/null 2>&1") != 0)
    GTEST_SKIP() << "solc is unavailable";
  const auto Path = writeTemporarySource(".sol", *Source);
  const std::string Command =
      "solc --bin '" + Path.string() + "' >/dev/null 2>&1";
  EXPECT_EQ(std::system(Command.c_str()), 0);
  std::error_code EC;
  std::filesystem::remove(Path, EC);
}

// A hashed signature turns a recovered entry point into a declaration a reader
// can compare against the interface the contract claims to implement, so the
// declaration has to be spelled the way Solidity accepts it, data locations
// and all.
TEST(EVMSolidityEmitter, DeclaresHashedSignaturesWithTheirDataLocations) {
  const std::vector<uint8_t> Code = {
      0x60, 0x00, 0x35, 0x60, 0xe0, 0x1c,             // selector
      0x80, 0x63, 0xa9, 0x05, 0x9c, 0xbb, 0x14, 0x60, // transfer(address,
      0x1b, 0x57,                                     //   uint256)
      0x80, 0x63, 0x06, 0xfd, 0xde, 0x03, 0x14, 0x60, // name()
      0x21, 0x57,                                     //
      0x00,                                           // fallthrough STOP
      0x5b, 0x60, 0x04, 0x35, 0x50, 0x00,             // transfer body
      0x5b, 0x60, 0x20, 0x60, 0x00, 0xf3};            // name body

  auto Program = analyze(Code);
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  ASSERT_EQ(Program->High.Functions.size(), 2u);

  auto Source = emitSolidity(*Program);
  ASSERT_TRUE(static_cast<bool>(Source)) << llvm::toString(Source.takeError());
  EXPECT_NE(Source->find("hashed signature transfer(address,uint256) (erc-20)"),
            std::string::npos);
  EXPECT_NE(Source->find("function transfer(address arg0, uint256 arg1) "
                         "external pure virtual returns (bool);"),
            std::string::npos);
  EXPECT_NE(
      Source->find("function name() external pure virtual returns (string "
                   "memory);"),
      std::string::npos);

  if (std::system("command -v solc >/dev/null 2>&1") != 0)
    GTEST_SKIP() << "solc is unavailable";
  const auto Path = writeTemporarySource("-hashed-signatures.sol", *Source);
  const std::string Command =
      "solc --bin '" + Path.string() + "' >/dev/null 2>&1";
  EXPECT_EQ(std::system(Command.c_str()), 0);
  std::error_code EC;
  std::filesystem::remove(Path, EC);
}

TEST(EVMSolidityEmitter, EmitsRecoveredPayabilityIndependentlyOfStateAccess) {
  EVMProgram Program;
  RecoveredFunction Function;
  Function.Name = "payable_entry";
  Function.StateMutability = Mutability::Payable;
  Program.High.Functions.push_back(std::move(Function));

  auto Source = emitSolidity(Program);
  ASSERT_TRUE(static_cast<bool>(Source)) << llvm::toString(Source.takeError());
  EXPECT_NE(Source->find("function payable_entry() external payable virtual;"),
            std::string::npos);
}

TEST(EVMSolidityEmitter, StopsOnFalseTerminalJumpWithoutJumpDestinations) {
  const std::vector<uint8_t> Code = {opcodeByte(Opcode::PUSH0),
                                     opcodeByte(Opcode::PUSH0),
                                     opcodeByte(Opcode::JUMPI)};
  auto Program = analyze(Code);
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  auto Source = emitSolidity(*Program);
  ASSERT_TRUE(static_cast<bool>(Source)) << llvm::toString(Source.takeError());
  EXPECT_NE(Source->find("if (condition == 0) { return "
                         "NEVERD_EVM_STOPPED; }"),
            std::string::npos);

  if (std::system("command -v solc >/dev/null 2>&1") != 0)
    GTEST_SKIP() << "solc is unavailable";
  const auto Path = writeTemporarySource("-terminal-jump.sol", *Source);
  const std::string Command =
      "solc --bin '" + Path.string() + "' >/dev/null 2>&1";
  EXPECT_EQ(std::system(Command.c_str()), 0);
  std::error_code EC;
  std::filesystem::remove(Path, EC);
}

TEST(EVMSolidityEmitter, ExecutesDifferentiallyOnAnvil) {
#if defined(_WIN32)
  GTEST_SKIP() << "Anvil differential test requires a POSIX shell";
#else
  if (std::system("command -v solc >/dev/null 2>&1 && "
                  "command -v anvil >/dev/null 2>&1 && "
                  "command -v cast >/dev/null 2>&1 && "
                  "command -v jq >/dev/null 2>&1") != 0)
    GTEST_SKIP() << "solc, anvil, cast, or jq is unavailable";

  const std::vector<uint8_t> Code = differentialALUProgram();
  auto Program = analyze(Code);
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  auto Oracle = execute(Program->Low);
  ASSERT_TRUE(static_cast<bool>(Oracle)) << llvm::toString(Oracle.takeError());
  const auto Stored = Oracle->Storage.find(llvm::APInt(kWordBits, 0));
  ASSERT_NE(Stored, Oracle->Storage.end());

  auto Source = emitSolidity(*Program);
  ASSERT_TRUE(static_cast<bool>(Source)) << llvm::toString(Source.takeError());
  std::string Harness;
  llvm::raw_string_ostream HarnessOS(Harness);
  HarnessOS << "\ncontract NeverDDifferentialHarness is "
            << kDefaultContractName
            << " {\n"
               "    uint256 public captured;\n\n"
               "    function _evmHost(uint8 opcode, uint256["
            << static_cast<unsigned>(maxHostOpcodeArguments())
            << "] memory args_, bytes memory)\n"
               "        internal override returns (uint256)\n"
               "    {\n"
               "        if (opcode == 0x"
            << llvm::utohexstr(opcodeByte(Opcode::SSTORE))
            << ") captured = args_[" << kSStoreValueArgumentIndex
            << "];\n"
               "        return 0;\n"
               "    }\n"
               "}\n";
  HarnessOS.flush();
  *Source += Harness;

  const auto SourcePath = writeTemporarySource("-anvil.sol", *Source);
  const auto ArtifactDirectory =
      SourcePath.parent_path() / (SourcePath.stem().string() + "-artifacts");
  std::filesystem::create_directories(ArtifactDirectory);
  const auto BinaryPath = ArtifactDirectory / "NeverDDifferentialHarness.bin";
  const unsigned Port = anvilPort(SourcePath);
  const std::string URL = "http://127.0.0.1:" + std::to_string(Port);
  llvm::SmallString<80> StoredDecimal;
  Stored->second.toStringUnsigned(StoredDecimal, 10);

  std::string Command =
      "set -eu; anvil --silent --port " + std::to_string(Port) +
      " >/dev/null 2>&1 & anvil_pid=$!; "
      "trap 'kill $anvil_pid >/dev/null 2>&1 || true' EXIT; "
      "ready=0; for attempt in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 "
      "17 18 19 20; do if cast block-number --rpc-url '" +
      URL +
      "' >/dev/null 2>&1; then ready=1; break; fi; sleep 0.1; done; "
      "test $ready -eq 1; "
      "solc --bin '" +
      SourcePath.string() + "' -o '" + ArtifactDirectory.string() +
      "' --overwrite >/dev/null 2>&1; "
      "deploy_bin=$(tr -d '\\n' < '" +
      BinaryPath.string() +
      "'); "
      "address=$(cast send --rpc-url '" +
      URL + "' --private-key '" + kAnvilPrivateKey.str() +
      "' --create \"$deploy_bin\" --json | jq -r .contractAddress); "
      "test \"$address\" != null; "
      "receipt=$(cast send \"$address\" 'execute(bytes)' 0x --rpc-url '" +
      URL + "' --private-key '" + kAnvilPrivateKey.str() +
      "' --json); "
      "status=$(printf '%s' \"$receipt\" | jq -r .status); "
      "if test \"$status\" != 0x1; then printf 'transaction status: %s\\n' "
      "\"$status\" >&2; exit 1; fi; "
      "actual_logs=$(printf '%s' \"$receipt\" | jq '.logs | length'); "
      "if test \"$actual_logs\" -ne " +
      std::to_string(Oracle->Trace.size()) +
      "; then printf 'trace count: expected %s, got %s\\n' '" +
      std::to_string(Oracle->Trace.size()) +
      "' \"$actual_logs\" >&2; exit 1; fi; "
      "value=$(cast call \"$address\" 'captured()(uint256)' --rpc-url '" +
      URL + "' | awk '{print $1}'); if test \"$value\" != " +
      StoredDecimal.str().str() +
      "; then printf 'captured value: expected %s, got %s\\n' '" +
      StoredDecimal.str().str() + "' \"$value\" >&2; exit 1; fi;";
  EXPECT_EQ(std::system(Command.c_str()), 0);

  std::error_code EC;
  std::filesystem::remove(SourcePath, EC);
  std::filesystem::remove_all(ArtifactDirectory, EC);
#endif
}
} // namespace
} // namespace neverd::evm
