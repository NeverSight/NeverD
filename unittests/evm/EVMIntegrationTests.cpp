//===- EVMIntegrationTests.cpp - public API EVM tests -------------------===//

#include "gtest/gtest.h"

#include "neverd/sdk/NeverDCAPI.h"

#include "llvm/ADT/StringRef.h"

#include <filesystem>
#include <fstream>
#include <string>

namespace {

std::string takeString(const char *Text) {
  if (!Text)
    return {};
  std::string Copy(Text);
  neverd_free_string(Text);
  return Copy;
}

class EVMIntegrationTest : public ::testing::Test {
protected:
  void SetUp() override {
    Path = std::filesystem::temp_directory_path() /
           ("neverd-api-" + std::to_string(++Sequence) + ".evm");
    Session = neverd_session_create();
  }
  void TearDown() override {
    neverd_session_destroy(Session);
    std::error_code EC;
    std::filesystem::remove(Path, EC);
  }
  void write(llvm::StringRef Text) const {
    std::ofstream Output(Path, std::ios::binary);
    Output.write(Text.data(), static_cast<std::streamsize>(Text.size()));
  }

  inline static unsigned Sequence = 0;
  std::filesystem::path Path;
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
  EXPECT_NE(
      C.find("typedef unsigned _BitInt(NEVERD_EVM_WORD_BITS) evm_word;"),
      std::string::npos);
  const std::string Solidity = takeString(neverd_decompile_all_ex(
      Session, Path.string().c_str(), NEVERD_OUTPUT_SOLIDITY, 0, 0));
  EXPECT_NE(Solidity.find("abstract contract NeverDRecovered"),
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

} // namespace
