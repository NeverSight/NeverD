//===- X87CapabilitiesPublicTests.cpp - x87 public report parity --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Exercise the real unloaded-session C API and CLI against the same report.
//===----------------------------------------------------------------------===//

#include "../../TestProcess.h"
#include "gtest/gtest.h"

#include "neverd/sdk/NeverDCAPIX87.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBuffer.h"

#include <filesystem>
#include <string>

namespace {
std::string take(const char *Text) {
  if (!Text)
    return {};
  std::string Copy(Text);
  neverd_free_string(Text);
  return Copy;
}

TEST(X87CapabilitiesPublic, UnloadedSessionHasNoExecutionPrerequisite) {
  EXPECT_EQ(neverd_x87_capabilities_json(nullptr), nullptr);
  auto Session = neverd_session_create();
  ASSERT_NE(Session, nullptr);
  auto Destroy = llvm::scope_exit([&] { neverd_session_destroy(Session); });
  const auto Text = take(neverd_x87_capabilities_json(Session));
  ASSERT_FALSE(Text.empty()) << take(neverd_last_error(Session));
  EXPECT_EQ(neverd_session_is_loaded(Session), 0);
  EXPECT_TRUE(take(neverd_last_error(Session)).empty());
  auto Report = llvm::json::parse(Text);
  ASSERT_TRUE(bool(Report)) << llvm::toString(Report.takeError());
  ASSERT_NE(Report->getAsObject(), nullptr);
  EXPECT_EQ(Report->getAsObject()->getString("scope"),
            "static-inventory; no host probe, execution or proof");
  // Repeat and release separately: no borrowed storage or hidden probe state.
  EXPECT_EQ(take(neverd_x87_capabilities_json(Session)), Text);
}

TEST(X87CapabilitiesPublic, CLIAndCUseIdenticalInventory) {
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(
      llvm::sys::fs::createUniqueDirectory("neverd-x87-query", Directory));
  const std::filesystem::path Path(Directory.str().str());
  auto Remove = llvm::scope_exit([&] { std::filesystem::remove_all(Path); });
  const auto Output = (Path / "capabilities.json").string();
  const auto Command =
      neverd::test::shellQuote(NEVERD_X87_CLI) + " x87-capabilities" +
      neverd::test::redirectStdout(Output) + neverd::test::silenceStderr();
  ASSERT_EQ(
      neverd::test::systemExitCode(neverd::test::runShellCommand(Command)), 0);
  auto Buffer = llvm::MemoryBuffer::getFile(Output);
  ASSERT_TRUE(bool(Buffer));
  auto CLI = llvm::json::parse((*Buffer)->getBuffer());
  ASSERT_TRUE(bool(CLI)) << llvm::toString(CLI.takeError());
  auto Session = neverd_session_create();
  ASSERT_NE(Session, nullptr);
  auto Destroy = llvm::scope_exit([&] { neverd_session_destroy(Session); });
  auto API = llvm::json::parse(take(neverd_x87_capabilities_json(Session)));
  ASSERT_TRUE(bool(API)) << llvm::toString(API.takeError());
  EXPECT_EQ(*CLI, *API);
}
} // namespace
