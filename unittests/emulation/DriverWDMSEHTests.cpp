//===- DriverWDMSEHTests.cpp - Genuine WDK C exception execution ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Execute original drivers whose constant C handlers are compiled from genuine
/// WDK declarations. Unsupported handlers and CPU faults remain explicit stops.
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/emulation/DriverSession.h"

#include <algorithm>

namespace neverd::emulation {
namespace {
#ifdef NEVERD_WDM_SEH_FIXTURE
std::vector<const char *> images() {
  std::vector<const char *> Images{NEVERD_WDM_SEH_FIXTURE};
#ifdef NEVERD_WDM_SEH_CFG_FIXTURE
  Images.push_back(NEVERD_WDM_SEH_CFG_FIXTURE);
#endif
  return Images;
}

DriverOptions options(char Mode, uint64_t Address = 0x190000000) {
  DriverOptions Options;
  Options.ServiceName = std::string("NeverDSEH") + Mode;
  Options.LoadAddress = Address;
  Options.Unload = true;
  return Options;
}

size_t messageIndex(const DriverResult &Result, llvm::StringRef Text) {
  for (size_t I = 0; I < Result.Messages.size(); ++I)
    if (Result.Messages[I].find(Text.str()) != std::string::npos)
      return I;
  return Result.Messages.size();
}

void raisedCalls(const DriverResult &Result,
                 std::initializer_list<llvm::StringRef> Names) {
  std::vector<llvm::StringRef> Actual;
  for (const auto &Call : Result.Calls)
    if (llvm::StringRef(Call.Name).starts_with("ExRaise")) {
      Actual.push_back(Call.Name);
      EXPECT_FALSE(Call.Result) << Call.Name;
    }
  EXPECT_EQ(Actual, std::vector<llvm::StringRef>(Names));
}

void clean(const DriverResult &Result, char Mode) {
  ASSERT_EQ(Result.Stop, DriverStopReason::Returned) << Result.Diagnostic;
  EXPECT_EQ(Result.NTStatus, 0u);
  EXPECT_TRUE(Result.UnloadCompleted);
  EXPECT_FALSE(Result.Fault);
  EXPECT_TRUE(Result.Devices.empty());
  EXPECT_TRUE(Result.Requests.empty());
  for (const auto &Message : Result.Messages)
    EXPECT_EQ(Message.find("WDM SEH: failure"), std::string::npos) << Message;
  const auto Complete = messageIndex(
      Result, std::string("WDM SEH: complete mode=") + Mode);
  EXPECT_LT(Complete, Result.Messages.size());
  EXPECT_LT(Complete, messageIndex(Result, "WDM SEH: unload"));
}

TEST(DriverWDMSEH, ThreeRaiseExportsExecuteConstantHandlersWithCfgAndRebasing) {
  for (const auto *Image : images())
    for (uint64_t Address : {0x180000000ULL, 0x190000000ULL})
      for (char Mode : {'S', 'A', 'D'}) {
        SCOPED_TRACE(Image);
        SCOPED_TRACE(Mode);
        auto Result = emulateDriver(Image, options(Mode, Address));
        ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
        clean(*Result, Mode);
        EXPECT_EQ(Result->ImageBase, Address);
        const llvm::StringRef Name = Mode == 'A' ? "ExRaiseAccessViolation"
            : Mode == 'D' ? "ExRaiseDatatypeMisalignment" : "ExRaiseStatus";
        raisedCalls(*Result, {Name});
        const llvm::StringRef Code = Mode == 'A' ? "c0000005"
            : Mode == 'D' ? "80000002" : "c000009a";
        EXPECT_LT(messageIndex(*Result, "code=" + Code.str()),
                  Result->Messages.size());
        EXPECT_LT(messageIndex(*Result, "local=12cb5687"),
                  Result->Messages.size());
      }
}

TEST(DriverWDMSEH, HelperUnwindRestoresNonvolatileRegistersAndParentLocals) {
  for (const auto *Image : images()) {
    auto Result = emulateDriver(Image, options('H'));
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    clean(*Result, 'H');
    raisedCalls(*Result, {"ExRaiseStatus"});
    EXPECT_LT(messageIndex(*Result, "nonvolatile restored"),
              Result->Messages.size());
    EXPECT_LT(messageIndex(*Result, "stage=11"), Result->Messages.size());
  }
}

TEST(DriverWDMSEH, InnermostConstantScopeHandlesBeforeOuterScope) {
  for (const auto *Image : images()) {
    auto Result = emulateDriver(Image, options('N'));
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    clean(*Result, 'N');
    raisedCalls(*Result, {"ExRaiseAccessViolation"});
    EXPECT_LT(messageIndex(*Result, "nested inner code=c0000005"),
              Result->Messages.size());
    EXPECT_EQ(messageIndex(*Result, "unexpected outer"), Result->Messages.size());
    EXPECT_LT(messageIndex(*Result, "stage=22"), Result->Messages.size());
  }
}

TEST(DriverWDMSEH, ExceptionRaisedInsideHandlerReachesEnclosingScope) {
  for (const auto *Image : images()) {
    auto Result = emulateDriver(Image, options('R'));
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    clean(*Result, 'R');
    raisedCalls(*Result, {"ExRaiseAccessViolation", "ExRaiseDatatypeMisalignment"});
    EXPECT_LT(messageIndex(*Result, "reraising inner code=c0000005"),
              messageIndex(*Result, "reraising outer code=80000002"));
    EXPECT_LT(messageIndex(*Result, "stage=32"), Result->Messages.size());
  }
}

TEST(DriverWDMSEH, ExceptionRaisedInsideHelperHandlerReachesCallerScope) {
  for (const auto *Image : images()) {
    auto Result = emulateDriver(Image, options('G'));
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    clean(*Result, 'G');
    raisedCalls(*Result, {"ExRaiseAccessViolation", "ExRaiseDatatypeMisalignment"});
    EXPECT_LT(messageIndex(*Result, "helper reraising inner code=c0000005"),
              messageIndex(*Result, "helper reraising outer code=80000002"));
    EXPECT_LT(messageIndex(*Result, "stage=42"), Result->Messages.size());
  }
}

TEST(DriverWDMSEH, DynamicFilterRemainsExplicitlyUnsupportedWithoutExecution) {
  for (const auto *Image : images()) {
    auto Result = emulateDriver(Image, options('F'));
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    EXPECT_EQ(Result->Stop, DriverStopReason::ModelError) << Result->Diagnostic;
    EXPECT_FALSE(Result->NTStatus);
    EXPECT_FALSE(Result->UnloadCompleted);
    EXPECT_NE(Result->Diagnostic.find("filter"), std::string::npos);
    EXPECT_EQ(messageIndex(*Result, "unsupported filter"), Result->Messages.size());
    raisedCalls(*Result, {"ExRaiseAccessViolation"});
  }
}

TEST(DriverWDMSEH, FinallyRemainsExplicitlyUnsupportedWithoutExecutingCleanup) {
  for (const auto *Image : images()) {
    auto Result = emulateDriver(Image, options('T'));
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    EXPECT_EQ(Result->Stop, DriverStopReason::ModelError) << Result->Diagnostic;
    EXPECT_FALSE(Result->NTStatus);
    EXPECT_FALSE(Result->UnloadCompleted);
    EXPECT_NE(Result->Diagnostic.find("finally"), std::string::npos);
    EXPECT_EQ(messageIndex(*Result, "unsupported finally"), Result->Messages.size());
    raisedCalls(*Result, {"ExRaiseAccessViolation"});
  }
}

TEST(DriverWDMSEH, UnhandledApiExceptionCannotReturnFromNoreturnCall) {
  for (const auto *Image : images()) {
    auto Result = emulateDriver(Image, options('U'));
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    EXPECT_EQ(Result->Stop, DriverStopReason::ModelError) << Result->Diagnostic;
    EXPECT_FALSE(Result->NTStatus);
    EXPECT_FALSE(Result->UnloadCompleted);
    EXPECT_NE(Result->Diagnostic.find("unhandled"), std::string::npos);
    EXPECT_EQ(messageIndex(*Result, "WDM SEH: complete"), Result->Messages.size());
    raisedCalls(*Result, {"ExRaiseStatus"});
  }
}

TEST(DriverWDMSEH, CpuMemoryFaultDoesNotUseApiExceptionRecovery) {
  for (const auto *Image : images()) {
    auto Result = emulateDriver(Image, options('C'));
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    EXPECT_EQ(Result->Stop, DriverStopReason::MemoryFault) << Result->Diagnostic;
    EXPECT_FALSE(Result->NTStatus);
    EXPECT_FALSE(Result->UnloadCompleted);
    ASSERT_TRUE(Result->Fault);
    EXPECT_EQ(Result->Fault->Address, 0x11100000u);
    EXPECT_EQ(Result->Fault->Access, "read");
    EXPECT_EQ(messageIndex(*Result, "unsupported CPU handler"),
              Result->Messages.size());
    raisedCalls(*Result, {});
  }
}
#else
TEST(DriverWDMSEH, GenuineFixtureIsOptional) {
  GTEST_SKIP() << "configure NEVERD_WDM_SEH_FIXTURE for genuine WDK C SEH tests";
}
#endif
} // namespace
} // namespace neverd::emulation
