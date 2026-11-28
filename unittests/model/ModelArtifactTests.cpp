//===- ModelArtifactTests.cpp - Model input identity and options ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Recheck model file identity before publication.
//===----------------------------------------------------------------------===//

#include "neverd/model/Artifact.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/JSON.h"

#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>

TEST(ModelArtifact, MutationDuringInventoryInvalidatesReport) {
  llvm::SmallString<128> Temp;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory("neverd-model-test", Temp));
  auto Root = std::filesystem::path(std::u8string(Temp.begin(), Temp.end()));
  llvm::scope_exit Cleanup([&] {
    std::error_code EC;
    std::filesystem::remove_all(Root, EC);
  });
  auto Path = Root / "chat_template.jinja";
  {
    std::ofstream F(Path, std::ios::binary);
    for (int I = 0; I < 80; ++I)
      F << "{{ message.content }}";
  }
  neverd::model::Options O;
  unsigned Calls = 0;
  O.Cancel = [&] {
    // Inside template tokenization, after the source has been opened.
    if (++Calls == 40)
      std::ofstream(Path, std::ios::binary | std::ios::app) << ' ';
    return false;
  };
  auto Result = llvm::json::parse(neverd::model::scan(Path, O));
  ASSERT_TRUE(bool(Result));
  auto *R = Result->getAsObject();
  ASSERT_NE(R, nullptr);
  EXPECT_GT(Calls, 40u);
  EXPECT_EQ(R->getString("status"), "failed");
  EXPECT_EQ(R->getString("error"), "file changed during inspection");
}

TEST(ModelArtifact, RejectsInvalidOptionsBeforeOpeningInput) {
  auto Result = llvm::json::parse(
      neverd::model::scanJSON("unopened", R"({"scale_bytes":-1})", {}));
  ASSERT_TRUE(bool(Result));
  auto *R = Result->getAsObject();
  ASSERT_NE(R, nullptr);
  EXPECT_EQ(R->getInteger("exit_code"), 1);
  EXPECT_EQ(R->getString("error"), "expected an unsigned integer");
}
