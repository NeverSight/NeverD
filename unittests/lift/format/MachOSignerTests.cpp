//===- MachOSignerTests.cpp - Controlled signer subprocess tests --------===//

#include "gtest/gtest.h"

#include "neverd/backend/codegen/MachO/MachOSigner.h"

#include "llvm/Support/Error.h"

#include <string>
#include <utility>
#include <vector>

namespace {

using namespace neverd::macho_signing;

TEST(MachOSigner, VerifyUsesFixedProgramBoundedWaitAndNoInheritedStreams) {
  ProcessRequest Captured;
  const Operations Ops{[&](const ProcessRequest &Request) {
    Captured = Request;
    return ProcessResult{};
  }};

  llvm::Error Error = verifyStrict("/tmp/candidate", Ops);

  EXPECT_FALSE(static_cast<bool>(Error));
  EXPECT_EQ(Captured.Program, "/usr/bin/codesign");
  EXPECT_EQ(Captured.Arguments,
            (std::vector<std::string>{"/usr/bin/codesign", "--verify",
                                      "--strict", "/tmp/candidate"}));
  EXPECT_EQ(Captured.TimeoutSeconds, 30u);
  EXPECT_TRUE(Captured.DiscardStandardStreams);
  EXPECT_FALSE(Captured.InheritEnvironment);
  EXPECT_TRUE(Captured.Environment.empty());
}

TEST(MachOSigner, AdHocSigningUsesExactNoninteractiveIdentityArguments) {
  ProcessRequest Captured;
  const Operations Ops{[&](const ProcessRequest &Request) {
    Captured = Request;
    return ProcessResult{};
  }};

  llvm::Error Error = adHocResign("/tmp/backend", "com.neverd.fixture", Ops);

  EXPECT_FALSE(static_cast<bool>(Error));
  EXPECT_EQ(Captured.Program, "/usr/bin/codesign");
  EXPECT_EQ(Captured.Arguments,
            (std::vector<std::string>{"/usr/bin/codesign", "--force", "--sign",
                                      "-", "--identifier", "com.neverd.fixture",
                                      "--timestamp=none", "/tmp/backend"}));
  EXPECT_EQ(Captured.TimeoutSeconds, 30u);
  EXPECT_TRUE(Captured.DiscardStandardStreams);
  EXPECT_FALSE(Captured.InheritEnvironment);
  EXPECT_TRUE(Captured.Environment.empty());
}

TEST(MachOSigner, EverySubprocessFailureIsBoundedAndActionable) {
  struct Case {
    ProcessResult Result;
    const char *Message;
  };
  const Case Cases[] = {
      {{-1, true, "permission denied"}, "failed to launch"},
      {{-2, false, "Child timed out"}, "timed out"},
      {{-2, false, "Killed: 9"}, "terminated by a signal"},
      {{7, false, {}}, "status 7"},
  };
  for (const Case &Entry : Cases) {
    SCOPED_TRACE(Entry.Message);
    const Operations Ops{[&](const ProcessRequest &) { return Entry.Result; }};

    llvm::Error Error = adHocResign("/tmp/backend", "com.neverd.fixture", Ops);

    ASSERT_TRUE(static_cast<bool>(Error));
    const std::string Detail = llvm::toString(std::move(Error));
    EXPECT_NE(Detail.find(Entry.Message), std::string::npos) << Detail;
    EXPECT_LE(Detail.size(), 512u);
  }
}

TEST(MachOSigner, EmptyPathOrIdentifierNeverLaunchesCodesign) {
  unsigned Calls = 0;
  const Operations Ops{[&](const ProcessRequest &) {
    ++Calls;
    return ProcessResult{};
  }};

  llvm::Error EmptyPath = verifyStrict("", Ops);
  llvm::Error EmptyIdentifier = adHocResign("/tmp/backend", "", Ops);

  EXPECT_TRUE(static_cast<bool>(EmptyPath));
  EXPECT_TRUE(static_cast<bool>(EmptyIdentifier));
  llvm::consumeError(std::move(EmptyPath));
  llvm::consumeError(std::move(EmptyIdentifier));
  EXPECT_EQ(Calls, 0u);
}

} // namespace
