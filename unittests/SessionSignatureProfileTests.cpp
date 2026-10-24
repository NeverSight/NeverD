//===- SessionSignatureProfileTests.cpp - Target selection through the SDK ===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Real header loading, automatic pattern selection and public JSON reporting.
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"
#include "web/NativeFixture.h"

#include "neverd/sdk/NeverDCAPI.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/JSON.h"

#include <filesystem>
#include <fstream>
#include <string>

namespace {

class SessionSignatureProfile : public testing::Test {
protected:
  std::filesystem::path Directory;
  neverd_session_t Session = nullptr;

  void SetUp() override {
    llvm::SmallString<128> Path;
    ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory("neverd-profile", Path));
    Directory = Path.str().str();
    Session = neverd_session_create();
    ASSERT_NE(Session, nullptr);
  }

  void TearDown() override {
    neverd_session_destroy(Session);
    std::error_code Error;
    if (!Directory.empty())
      std::filesystem::remove_all(Directory, Error);
  }

  std::string write(const char *Name, const std::string &Data) {
    const auto Path = Directory / Name;
    std::filesystem::create_directories(Path.parent_path());
    std::ofstream Output(Path, std::ios::binary);
    Output.write(Data.data(), static_cast<std::streamsize>(Data.size()));
    EXPECT_TRUE(Output.good());
    return Path.string();
  }

  llvm::Expected<llvm::json::Value> headers() {
    const char *Text = neverd_headers_json(Session);
    auto Value = llvm::json::parse(Text ? Text : "{}");
    neverd_free_string(Text);
    return Value;
  }
};

TEST_F(SessionSignatureProfile, DriverSelectionAndManualResetAreVisible) {
  auto Bytes = neverd::web::test::nativePE();
  neverd::web::test::nativePut(Bytes, 0x98 + 68, 1, 2); // NATIVE
  neverd::web::test::nativePut(Bytes, 0x96, 0x1022, 2); // system image
  const auto Input = write("driver-renamed.txt", Bytes);
  ASSERT_EQ(neverd_session_load(Session, Input.c_str()), 1);
  auto Value = headers();
  ASSERT_TRUE(bool(Value)) << llvm::toString(Value.takeError());
  auto *Profile = Value->getAsObject()->getObject("target_profile");
  ASSERT_NE(Profile, nullptr);
  EXPECT_EQ(Profile->getString("system"), "windows");
  EXPECT_EQ(Profile->getString("domain"), "kernel");
  EXPECT_EQ(Value->getAsObject()->getObject("signature_selection"), nullptr);

  write("tree/pe/x86/64/supplemental-bds.pat", "invalid excluded UI pack\n");
  const auto Manual = write("tree/pe/x86/64/custom.pat",
                            "B82A000000C3 00 0000 0006 :0000 answer\n");
  const auto Tree = (Directory / "tree").string();
  ASSERT_EQ(neverd_auto_apply_signatures(Session, Tree.c_str()), 1);
  Value = headers();
  ASSERT_TRUE(bool(Value)) << llvm::toString(Value.takeError());
  auto *Selection = Value->getAsObject()->getObject("signature_selection");
  ASSERT_NE(Selection, nullptr);
  EXPECT_EQ(Selection->getInteger("available_files"), 2);
  EXPECT_EQ(Selection->getInteger("selected_files"), 1);
  EXPECT_EQ(Selection->getInteger("skipped_files"), 1);

  ASSERT_EQ(neverd_apply_signature_file(Session, Manual.c_str()), 1);
  Value = headers();
  ASSERT_TRUE(bool(Value)) << llvm::toString(Value.takeError());
  EXPECT_EQ(Value->getAsObject()->getObject("signature_selection"), nullptr);
}

} // namespace
