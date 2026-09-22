//===- DriverWDMStackTests.cpp - Genuine WDM dispatch and completion -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Run an optional original driver built with genuine WDK inline IRP helpers,
/// checking nested dispatch, completion control and retained packet lifetime.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/emulation/DriverSession.h"

#include <algorithm>

namespace neverd::emulation {
namespace {
#ifdef NEVERD_WDM_STACK_FIXTURE

std::vector<const char *> stackImages() {
  std::vector<const char *> Images{NEVERD_WDM_STACK_FIXTURE};
#ifdef NEVERD_WDM_STACK_CFG_FIXTURE
  Images.push_back(NEVERD_WDM_STACK_CFG_FIXTURE);
#endif
  return Images;
}

DriverOptions stackOptions(char Mode) {
  DriverOptions Options;
  Options.ServiceName = std::string("NeverDWdmStack") + Mode;
  Options.LoadAddress = 0x190000000;
  Options.Unload = true;
  for (auto Kind : {DriverRequestKind::Create, DriverRequestKind::DeviceControl,
                    DriverRequestKind::Cleanup, DriverRequestKind::Close}) {
    DriverRequest Request;
    Request.Kind = Kind;
    if (Kind == DriverRequestKind::Create)
      Request.Device = "\\DosDevices\\NeverDWdmStack";
    if (Kind == DriverRequestKind::DeviceControl) {
      Request.ControlCode = Mode == 'D' ? 0x222002 : 0x222000;
      Request.Input = {0, 1, 2, 3};
      Request.OutputSize = 4;
    }
    Options.Requests.push_back(std::move(Request));
  }
  return Options;
}

size_t stackAPICount(const DriverResult &Result, llvm::StringRef Name) {
  return std::count_if(Result.Calls.begin(), Result.Calls.end(),
                       [Name](const auto &Call) { return Call.Name == Name; });
}

std::vector<std::string> stackMessages(const DriverResult &Result) {
  std::vector<std::string> Messages;
  for (const auto &Message : Result.Messages)
    if (llvm::StringRef(Message).starts_with("WDM stack:"))
      Messages.push_back(Message);
  return Messages;
}

std::vector<std::string> expectedStackMessages(char Mode) {
  const std::string Prefix = std::string("WDM stack: ") + Mode;
  std::vector<std::string> Messages{
      std::string("WDM stack: ready ") + Mode + "\n",
      "WDM stack: lower major=0\n", Prefix + " upper dispatch\n",
      Prefix + " lower dispatch\n"};
  const bool Held = Mode == 'W' || Mode == 'D';
  const bool Deferred = Held || Mode == 'P' || Mode == 'I';
  if (Deferred) {
    Messages.push_back("WDM stack: lower returned 0x00000103\n");
    Messages.push_back(Mode == 'I' ? "WDM stack: lower DPC\n"
                                   : "WDM stack: lower worker\n");
  }
  if (Mode != 'S')
    Messages.push_back(Prefix + " completion irql=" +
                       (Mode == 'I' ? "2" : "0") + " pending=" +
                       (Deferred ? "1\n" : "0\n"));
  if (Mode == 'N')
    Messages.push_back("WDM stack: nested completion returned\n");
  if (Deferred) {
    Messages.push_back(Mode == 'I' ? "WDM stack: lower DPC completed\n"
                                   : "WDM stack: lower worker completed\n");
    if (Held)
      Messages.push_back("WDM stack: upper resumed with live buffer\n");
  } else {
    Messages.push_back("WDM stack: lower synchronous completion returned\n");
    if (Mode != 'S')
      Messages.push_back(std::string("WDM stack: lower returned 0x") +
                         (Mode == 'E'   ? "c0000022\n"
                          : Mode == 'R' ? "80000005\n"
                                        : "00000000\n"));
  }
  Messages.insert(Messages.end(), {"WDM stack: lower major=18\n",
                                    "WDM stack: lower major=2\n",
                                    "WDM stack: unloaded\n"});
  return Messages;
}

void exerciseStack(char Mode) {
  for (const auto *Image : stackImages()) {
    SCOPED_TRACE(Image);
    SCOPED_TRACE(Mode);
    auto Result = emulateDriver(Image, stackOptions(Mode));
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    ASSERT_EQ(Result->Stop, DriverStopReason::Returned) << Result->Diagnostic;
    EXPECT_EQ(Result->NTStatus, 0u);
    EXPECT_TRUE(Result->UnloadCompleted);
    EXPECT_TRUE(Result->Devices.empty());
    ASSERT_EQ(Result->Requests.size(), 4u);
    for (size_t I = 0; I != Result->Requests.size(); ++I) {
      const auto &Request = Result->Requests[I];
      EXPECT_TRUE(Request.Completed);
      EXPECT_EQ(Request.Device, "\\Device\\NeverDWdmStack");
      const uint32_t Status = I == 1 && Mode == 'R' ? 0x80000005u : 0u;
      const bool Pending = I == 1 && (Mode == 'P' || Mode == 'I');
      EXPECT_EQ(Request.IOStatus, Status);
      EXPECT_EQ(Request.DispatchStatus, Pending ? 0x103u : Status);
      EXPECT_EQ(Request.Information, I == 1 ? 4u : 0u);
      if (I != 1)
        EXPECT_TRUE(Request.Output.empty());
    }
    EXPECT_EQ(Result->Requests[1].Output,
              (std::vector<uint8_t>{'S', uint8_t(Mode), 'D', 'M'}));
    EXPECT_EQ(stackMessages(*Result), expectedStackMessages(Mode));
    EXPECT_EQ(stackAPICount(*Result, "IofCallDriver"), 4u);
    EXPECT_EQ(stackAPICount(*Result, "IoAttachDeviceToDeviceStack"), 1u);
    EXPECT_EQ(stackAPICount(*Result, "IoDetachDevice"), 1u);
    EXPECT_EQ(stackAPICount(*Result, "IofCompleteRequest"),
              Mode == 'W' || Mode == 'D' || Mode == 'N' ? 5u : 4u);
    EXPECT_EQ(stackAPICount(*Result, "KeWaitForSingleObject"),
              Mode == 'W' || Mode == 'D' ? 1u : 0u);
    bool SawLowerReturn = false;
    for (const auto &Call : Result->Calls)
      if (Call.Name == "IofCallDriver" && Call.Arguments.size() == 2 &&
          Call.Arguments[1] == Result->Requests[1].IRP) {
        const uint32_t LowerStatus =
            Mode == 'P' || Mode == 'I' || Mode == 'W' || Mode == 'D'
                ? 0x103u
                : Mode == 'E' ? 0xc0000022u
                : Mode == 'R' ? 0x80000005u
                              : 0u;
        EXPECT_EQ(Call.Result, LowerStatus);
        SawLowerReturn = true;
      }
    EXPECT_TRUE(SawLowerReturn);
  }
}

TEST(DriverWDMStack, SkipUsesTheSameSlotAndNamedLowerFileIdentity) {
  exerciseStack('S');
}
TEST(DriverWDMStack, CopyCallsCompletionBeforeSynchronousDispatchReturns) {
  exerciseStack('C');
}
TEST(DriverWDMStack, CompletionControlAndLowerDispatchStatusDoNotReplaceIoStatus) {
  exerciseStack('E');
}
TEST(DriverWDMStack, WarningStatusSelectsInvokeOnErrorAndRetainsOutput) {
  exerciseStack('R');
}
TEST(DriverWDMStack, PendingWorkerPropagatesTheUpperStackPendingBit) {
  exerciseStack('P');
}
TEST(DriverWDMStack, MoreProcessingRequiredRetainsPacketUntilWaitingDispatchResumes) {
  exerciseStack('W');
}
TEST(DriverWDMStack, DirectMdlSurvivesStoppedCompletionAndUpperWait) {
  exerciseStack('D');
}
TEST(DriverWDMStack, NestedCompletionThenMoreProcessingRequiredRetiresExactlyOnce) {
  exerciseStack('N');
}
TEST(DriverWDMStack, TimerDpcCompletionInheritsDispatchLevel) {
  exerciseStack('I');
}
TEST(DriverWDMStack, CopyForwardsBufferedReadWriteAndFileLifecycle) {
  auto Options = stackOptions('S');
  Options.Requests.erase(Options.Requests.begin() + 1);
  DriverRequest Read;
  Read.Kind = DriverRequestKind::Read;
  Read.OutputSize = 4;
  Read.ByteOffset = 18;
  DriverRequest Write;
  Write.Kind = DriverRequestKind::Write;
  Write.Input = {0x11, 0x22, 0x33};
  Write.ByteOffset = 64;
  Options.Requests.insert(Options.Requests.begin() + 1, {Read, Write});
  for (const auto *Image : stackImages()) {
    SCOPED_TRACE(Image);
    auto Result = emulateDriver(Image, Options);
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    ASSERT_EQ(Result->Stop, DriverStopReason::Returned) << Result->Diagnostic;
    EXPECT_EQ(Result->NTStatus, 0u);
    EXPECT_TRUE(Result->UnloadCompleted);
    EXPECT_TRUE(Result->Devices.empty());
    ASSERT_EQ(Result->Requests.size(), 5u);
    for (size_t I = 0; I != Result->Requests.size(); ++I) {
      const auto &Request = Result->Requests[I];
      EXPECT_TRUE(Request.Completed);
      EXPECT_EQ(Request.Device, "\\Device\\NeverDWdmStack");
      EXPECT_EQ(Request.IOStatus, 0u);
      EXPECT_EQ(Request.DispatchStatus, 0u);
      EXPECT_EQ(Request.Information, I == 1 ? 4u : I == 2 ? 3u : 0u);
      if (I != 1)
        EXPECT_TRUE(Request.Output.empty());
    }
    EXPECT_EQ(Result->Requests[1].Output,
              (std::vector<uint8_t>{'S', 'T', 'A', 'K'}));
    EXPECT_EQ(stackMessages(*Result),
              (std::vector<std::string>{
                  "WDM stack: ready S\n", "WDM stack: lower major=0\n",
                  "WDM stack: lower major=3\n",
                  "WDM stack: lower read bytes=4 offset=18\n",
                  "WDM stack: lower major=4\n",
                  "WDM stack: lower write bytes=3 offset=64 sum=102\n",
                  "WDM stack: lower major=18\n", "WDM stack: lower major=2\n",
                  "WDM stack: unloaded\n"}));
    EXPECT_EQ(stackAPICount(*Result, "IofCallDriver"), 5u);
    EXPECT_EQ(stackAPICount(*Result, "IofCompleteRequest"), 5u);
    EXPECT_EQ(stackAPICount(*Result, "IoAttachDeviceToDeviceStack"), 1u);
    EXPECT_EQ(stackAPICount(*Result, "IoDetachDevice"), 1u);
  }
}
TEST(DriverWDMStack, MalformedCursorAndNullCompletionFlagsFailExplicitly) {
  for (char Mode : {'B', 'F'})
    for (const auto *Image : stackImages()) {
      SCOPED_TRACE(Image);
      SCOPED_TRACE(Mode);
      auto Result = emulateDriver(Image, stackOptions(Mode));
      ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
      EXPECT_EQ(Result->Stop, DriverStopReason::ModelError);
      EXPECT_NE(Result->Diagnostic.find(Mode == 'B' ? "cursor" : "completion"),
                std::string::npos)
          << Result->Diagnostic;
      ASSERT_EQ(Result->Requests.size(), 2u);
      EXPECT_TRUE(Result->Requests[0].Completed);
      EXPECT_FALSE(Result->Requests[1].Completed);
      EXPECT_FALSE(Result->UnloadCompleted);
    }
}

#else
TEST(DriverWDMStack, OptionalGenuineFixtureIsConfigured) {
  GTEST_SKIP() << "NEVERD_WDM_STACK_FIXTURE requires a genuine WDK-linked fixture";
}
#endif
} // namespace
} // namespace neverd::emulation
