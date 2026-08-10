//===- EVMIntegrationTests.cpp - public API EVM tests -------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "../TestProcess.h"
#include "gtest/gtest.h"

#include "neverd/sdk/NeverDCAPI.h"

#include "llvm/ADT/StringRef.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace {

unsigned long long currentProcessId() {
#ifdef _WIN32
  return static_cast<unsigned long long>(::_getpid());
#else
  return static_cast<unsigned long long>(::getpid());
#endif
}

std::string takeString(const char *Text) {
  if (!Text)
    return {};
  std::string Copy(Text);
  neverd_free_string(Text);
  return Copy;
}

std::string readFile(const std::filesystem::path &Path) {
  std::ifstream Input(Path);
  std::ostringstream Contents;
  Contents << Input.rdbuf();
  return Contents.str();
}

class EVMIntegrationTest : public ::testing::Test {
protected:
  void SetUp() override {
    Path = std::filesystem::temp_directory_path() /
           ("neverd-api-" + std::to_string(currentProcessId()) + ".evm");
    StdoutPath = Path.string() + ".stdout";
    StderrPath = Path.string() + ".stderr";
    Session = neverd_session_create();
  }
  void TearDown() override {
    neverd_session_destroy(Session);
    std::error_code EC;
    std::filesystem::remove(Path, EC);
    std::filesystem::remove(StdoutPath, EC);
    std::filesystem::remove(StderrPath, EC);
  }
  void write(llvm::StringRef Text) const {
    std::ofstream Output(Path, std::ios::binary);
    Output.write(Text.data(), static_cast<std::streamsize>(Text.size()));
  }

  struct CommandResult {
    int ExitCode = -1;
    std::string Stdout;
    std::string Stderr;
  };

  CommandResult runNeverD(llvm::StringRef Arguments) const {
    const std::string Command =
        neverd::test::shellQuote(NEVERD_BINARY) + " " + Arguments.str() + " " +
        neverd::test::shellQuote(Path.string()) +
        neverd::test::redirectOutput(StdoutPath.string(), StderrPath.string());
    const int Status = neverd::test::runShellCommand(Command);
    return {neverd::test::systemExitCode(Status), readFile(StdoutPath),
            readFile(StderrPath)};
  }

  std::filesystem::path Path;
  std::filesystem::path StdoutPath;
  std::filesystem::path StderrPath;
  neverd_session_t Session = nullptr;
};

TEST_F(EVMIntegrationTest, ExposesAllStagesAndBothSourceLanguages) {
  write("6001600055");
  ASSERT_EQ(neverd_session_load(Session, Path.string().c_str()), 1)
      << takeString(neverd_last_error(Session));
  EXPECT_EQ(takeString(neverd_session_arch_name(Session)), "evm");
  EXPECT_EQ(takeString(neverd_session_format_name(Session)), "EVM");
  EXPECT_EQ(neverd_session_bitness(Session), 256);
  EXPECT_EQ(neverd_func_count(Session), 1);
  EXPECT_EQ(takeString(neverd_func_name(Session, 0)), "evm_entry");

  ASSERT_EQ(neverd_session_analyze(Session), 1)
      << takeString(neverd_last_error(Session));
  EXPECT_NE(takeString(neverd_ir_low(Session, 0)).find("PUSH1"),
            std::string::npos);
  EXPECT_NE(takeString(neverd_ir_med(Session, 0)).find("storage.write"),
            std::string::npos);
  EXPECT_NE(takeString(neverd_ir_high(Session, 0)).find("storage write"),
            std::string::npos);
  EXPECT_NE(takeString(neverd_ir_llvm(Session, 0)).find("@evm_execute"),
            std::string::npos);
  EXPECT_NE(takeString(neverd_disasm_json(Session, 0, 10)).find("PUSH1"),
            std::string::npos);
  EXPECT_NE(takeString(neverd_cfg_json(Session, 0)).find("evm_entry"),
            std::string::npos);

  const std::string C = takeString(neverd_decompile_all_ex(
      Session, Path.string().c_str(), NEVERD_OUTPUT_C, 0, 0));
  EXPECT_NE(C.find("typedef unsigned _BitInt(NEVERD_EVM_WORD_BITS) evm_word;"),
            std::string::npos);
  const std::string Solidity = takeString(neverd_decompile_all_ex(
      Session, Path.string().c_str(), NEVERD_OUTPUT_SOLIDITY, 0, 0));
  EXPECT_NE(Solidity.find("abstract contract NeverDRecovered"),
            std::string::npos);

  EXPECT_EQ(neverd_decompile_all(Session, Path.string().c_str(),
                                 /*UseLlvmRoute=*/1, /*NoOpt=*/0,
                                 /*MaxFunctions=*/0),
            nullptr);
  EXPECT_NE(takeString(neverd_last_error(Session))
                .find("LLVM-to-C route is not supported for EVM"),
            std::string::npos);

  EXPECT_EQ(neverd_lift_to_obj(Session, Path.string().c_str(),
                               /*NoOpt=*/0, /*MaxFunctions=*/0),
            1);
  EXPECT_NE(takeString(neverd_last_error(Session))
                .find("object-code roundtrip is not supported"),
            std::string::npos);
}

TEST_F(EVMIntegrationTest, HardforkAndStrictnessAreConfigurable) {
  write("0x5f00"); // PUSH0; STOP
  ASSERT_EQ(neverd_session_load(Session, Path.string().c_str()), 1);
  ASSERT_EQ(neverd_evm_set_hardfork(Session, "london"), 1);
  EXPECT_EQ(neverd_session_analyze(Session), 0);
  EXPECT_NE(takeString(neverd_last_error(Session)).find("inactive opcode"),
            std::string::npos);

  neverd_evm_set_strict(Session, 0);
  EXPECT_EQ(neverd_session_analyze(Session), 1)
      << takeString(neverd_last_error(Session));
  EXPECT_EQ(neverd_evm_set_hardfork(Session, "not-a-fork"), 0);
  EXPECT_NE(takeString(neverd_last_error(Session)).find("unknown EVM hardfork"),
            std::string::npos);
}

TEST_F(EVMIntegrationTest,
       CLISelectsAmsterdamForConditionalImmediateBoundaries) {
  write("e75b00"); // Invalid SWAPN candidate; JUMPDEST; STOP.

  const CommandResult Default = runNeverD("disasm --func=evm_entry");
  EXPECT_NE(Default.ExitCode, 0);

  const CommandResult Amsterdam =
      runNeverD("disasm --evm-hardfork=amsterdam --func=evm_entry");
  ASSERT_EQ(Amsterdam.ExitCode, 0) << Amsterdam.Stderr;
  EXPECT_NE(Amsterdam.Stdout.find("SWAPN 0x5B"), std::string::npos);
  EXPECT_NE(Amsterdam.Stdout.find("immediate=invalid"), std::string::npos);
  EXPECT_NE(Amsterdam.Stdout.find("JUMPDEST"), std::string::npos);
  EXPECT_NE(Amsterdam.Stdout.find("STOP"), std::string::npos);

  const CommandResult JSON =
      runNeverD("disasm --json --evm-hardfork=amsterdam --func=evm_entry");
  ASSERT_EQ(JSON.ExitCode, 0) << JSON.Stderr;
  EXPECT_NE(JSON.Stdout.find("\"decode_status\":\"active\""),
            std::string::npos);
  EXPECT_NE(JSON.Stdout.find("\"immediate_status\":\"invalid\""),
            std::string::npos);

  const CommandResult CFG =
      runNeverD("cfg --evm-hardfork=amsterdam --func=evm_entry");
  ASSERT_EQ(CFG.ExitCode, 0) << CFG.Stderr;
  EXPECT_NE(CFG.Stdout.find("bb1 [label=\"PC 0x1\"]"), std::string::npos);
}

} // namespace
