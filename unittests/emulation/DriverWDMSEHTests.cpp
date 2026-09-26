//===- DriverWDMSEHTests.cpp - Genuine WDK C exception execution ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Execute original drivers whose constant C handlers are compiled from genuine
/// WDK declarations. Unsupported handlers and CPU faults remain explicit stops.
//===----------------------------------------------------------------------===//

#include "fixtures/driver_seh_test.h"
#include "gtest/gtest.h"
#include "windows/DriverImage.h"

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
  const auto Complete =
      messageIndex(Result, std::string("WDM SEH: complete mode=") + Mode);
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
                                     : Mode == 'D'
                                         ? "ExRaiseDatatypeMisalignment"
                                         : "ExRaiseStatus";
        raisedCalls(*Result, {Name});
        const llvm::StringRef Code = Mode == 'A'   ? "c0000005"
                                     : Mode == 'D' ? "80000002"
                                                   : "c000009a";
        EXPECT_LT(messageIndex(*Result, "code=" + Code.str()),
                  Result->Messages.size());
        EXPECT_LT(messageIndex(*Result, "local=12cb5687"),
                  Result->Messages.size());
      }
}

TEST(DriverWDMSEH, GSCookiesMatchRealRuntimeForFixedAndAlignedFrames) {
  for (const auto *Image : images())
    for (uint64_t Address : {0x180000000ULL, 0x190000000ULL})
      for (char Mode : {SehGSCookie, SehGSAlignedCookie, SehGSStandaloneCookie,
                        SehGSStandaloneAlignedCookie}) {
        SCOPED_TRACE(Image);
        SCOPED_TRACE(Mode);
        auto Result = emulateDriver(Image, options(Mode, Address));
        ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
        clean(*Result, Mode);
        EXPECT_NE(Result->SecurityCookieAddress, 0u);
        EXPECT_LT(messageIndex(*Result, "GS cookie checked"),
                  Result->Messages.size());
        raisedCalls(*Result, {"ExRaiseAccessViolation"});
      }
}

TEST(DriverWDMSEH, StandaloneRuntimeIdentityPreservesCookieOnlyMetadata) {
  for (const auto *Path : images()) {
    SCOPED_TRACE(Path);
    auto Image = loadDriverImage(Path, profile::DefaultMemoryLimit);
    ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
    size_t Frames = 0;
    for (const auto &Function : Image->Exceptions.Functions) {
      if (Function.Personality != ExceptionPersonality::GSHandlerCheck)
        continue;
      ++Frames;
      EXPECT_EQ(Function.ParseStatus, ExceptionParseStatus::Complete);
      ASSERT_TRUE(Function.GSCookie);
      EXPECT_EQ(Function.GSCookie->ParseStatus, ExceptionParseStatus::Complete);
      EXPECT_FALSE(Function.SEH);
      EXPECT_FALSE(Function.Cxx);
      EXPECT_FALSE(Function.canRegenerateLanguageMetadata());
    }
    EXPECT_EQ(Frames, 2u);
  }
}

TEST(DriverWDMSEH, CorruptGSCookiesCannotReachHandlersOrUnload) {
  for (const auto *Image : images())
    for (uint64_t Address : {0x180000000ULL, 0x190000000ULL})
      for (char Mode : {SehGSCorruptCookie, SehGSAlignedCorruptCookie,
                        SehGSStandaloneCorruptCookie,
                        SehGSStandaloneAlignedCorruptCookie}) {
        SCOPED_TRACE(Image);
        SCOPED_TRACE(Mode);
        auto Result = emulateDriver(Image, options(Mode, Address));
        ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
        EXPECT_EQ(Result->Stop, DriverStopReason::ModelError)
            << Result->Diagnostic;
        EXPECT_NE(Result->Diagnostic.find("GS security cookie check failed"),
                  std::string::npos)
            << Result->Diagnostic;
        EXPECT_FALSE(Result->UnloadCompleted);
        EXPECT_EQ(messageIndex(*Result, "GS cookie checked"),
                  Result->Messages.size());
        raisedCalls(*Result, {"ExRaiseAccessViolation"});
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

TEST(DriverWDMSEH, FullNonvolatileXmmValuesSurviveRealHelperUnwind) {
  for (const auto *Image : images())
    for (uint64_t Address : {0x180000000ULL, 0x190000000ULL}) {
      SCOPED_TRACE(Image);
      auto Result = emulateDriver(Image, options(SehXmmUnwind, Address));
      ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
      clean(*Result, SehXmmUnwind);
      raisedCalls(*Result, {"ExRaiseAccessViolation"});
      EXPECT_LT(messageIndex(*Result, "full nonvolatile XMM restored"),
                Result->Messages.size());
    }
}

TEST(DriverWDMSEH, ChainedRuntimeFunctionsRestoreOnePrimaryStack) {
  for (const auto *Image : images())
    for (uint64_t Address : {0x180000000ULL, 0x190000000ULL}) {
      SCOPED_TRACE(Image);
      auto Result = emulateDriver(Image, options(SehChainedUnwind, Address));
      ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
      clean(*Result, SehChainedUnwind);
      raisedCalls(*Result, {"ExRaiseAccessViolation"});
      EXPECT_LT(messageIndex(*Result, "chained nonvolatile restored"),
                Result->Messages.size());
    }
}

TEST(DriverWDMSEH, FaultInsidePartialPrologueRestoresOnlyExecutedSaves) {
  for (const auto *Image : images())
    for (uint64_t Address : {0x180000000ULL, 0x190000000ULL}) {
      SCOPED_TRACE(Image);
      auto Options = options(SehPrologueUnwind, Address);
      for (auto Kind :
           {DriverRequestKind::Create, DriverRequestKind::DeviceControl,
            DriverRequestKind::Cleanup, DriverRequestKind::Close}) {
        DriverRequest Request;
        Request.Kind = Kind;
        Request.Device = "\\Device\\NeverDSEH";
        if (Kind == DriverRequestKind::DeviceControl) {
          Request.ControlCode = SehRecoverControlCode;
          Request.Input = {0, 0, 0, 0};
          Request.UserInputAccess = DriverUserPageAccess::NoAccess;
        }
        Options.Requests.push_back(std::move(Request));
      }
      auto Result = emulateDriver(Image, Options);
      ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
      ASSERT_EQ(Result->Stop, DriverStopReason::Returned) << Result->Diagnostic;
      ASSERT_EQ(Result->Requests.size(), 4u);
      EXPECT_EQ(Result->Requests[1].IOStatus, 0u);
      EXPECT_TRUE(Result->UnloadCompleted);
      EXPECT_FALSE(Result->Fault);
      EXPECT_LT(messageIndex(*Result, "partial prologue restored"),
                Result->Messages.size());
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
    EXPECT_EQ(messageIndex(*Result, "unexpected outer"),
              Result->Messages.size());
    EXPECT_LT(messageIndex(*Result, "stage=22"), Result->Messages.size());
  }
}

TEST(DriverWDMSEH, ExceptionRaisedInsideHandlerReachesEnclosingScope) {
  for (const auto *Image : images()) {
    auto Result = emulateDriver(Image, options('R'));
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    clean(*Result, 'R');
    raisedCalls(*Result,
                {"ExRaiseAccessViolation", "ExRaiseDatatypeMisalignment"});
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
    raisedCalls(*Result,
                {"ExRaiseAccessViolation", "ExRaiseDatatypeMisalignment"});
    EXPECT_LT(messageIndex(*Result, "helper reraising inner code=c0000005"),
              messageIndex(*Result, "helper reraising outer code=80000002"));
    EXPECT_LT(messageIndex(*Result, "stage=42"), Result->Messages.size());
  }
}

TEST(DriverWDMSEH, FiltersExecuteWithOriginalContextAndStableRecords) {
  for (const auto *Image : images())
    for (uint64_t Address : {0x180000000ULL, 0x190000000ULL})
      for (char Mode : {char(SehDynamicFilter), char(SehSearchFilters)}) {
        SCOPED_TRACE(Image);
        SCOPED_TRACE(Mode);
        auto Result = emulateDriver(Image, options(Mode, Address));
        ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
        clean(*Result, Mode);
        raisedCalls(*Result, {"ExRaiseAccessViolation"});
        EXPECT_LT(messageIndex(*Result, Mode == SehDynamicFilter
                                            ? "dynamic inner handled"
                                            : "dynamic outer handled"),
                  Result->Messages.size());
        if (Mode == SehSearchFilters)
          EXPECT_LT(messageIndex(*Result, "decision=0 stage=51"),
                    messageIndex(*Result, "decision=1 stage=52"));
      }
}

TEST(DriverWDMSEH, FinallyRunsAfterSearchInUnwindOrderAndOnNormalExit) {
  for (const auto *Image : images())
    for (uint64_t Address : {0x180000000ULL, 0x190000000ULL})
      for (char Mode : {char(SehExceptionalFinally), char(SehNormalFinally)}) {
        SCOPED_TRACE(Image);
        SCOPED_TRACE(Mode);
        auto Result = emulateDriver(Image, options(Mode, Address));
        ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
        clean(*Result, Mode);
        if (Mode == SehExceptionalFinally) {
          raisedCalls(*Result, {"ExRaiseAccessViolation"});
          EXPECT_LT(messageIndex(*Result, "filter decision=1 stage=61"),
                    messageIndex(*Result, "helper finally abnormal"));
          EXPECT_LT(messageIndex(*Result, "helper finally abnormal"),
                    messageIndex(*Result, "parent finally abnormal=1"));
          EXPECT_LT(messageIndex(*Result, "parent finally abnormal=1"),
                    messageIndex(*Result, "finally handler"));
        } else {
          raisedCalls(*Result, {});
          EXPECT_LT(messageIndex(*Result, "parent finally abnormal=0"),
                    Result->Messages.size());
        }
      }
}

TEST(DriverWDMSEH, NestedExceptionsHandledWithinCallbacksResumeParentDispatch) {
  for (const auto *Image : images())
    for (uint64_t Address : {0x180000000ULL, 0x190000000ULL})
      for (char Mode : {char(SehLocalFilter), char(SehLocalFinally)}) {
        auto Result = emulateDriver(Image, options(Mode, Address));
        ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
        clean(*Result, Mode);
        raisedCalls(*Result,
                    {"ExRaiseAccessViolation", "ExRaiseDatatypeMisalignment"});
        EXPECT_LT(messageIndex(*Result, "nested exception record linked"),
                  messageIndex(*Result, "local nested handler"));
        EXPECT_LT(messageIndex(*Result, "local nested handler"),
                  messageIndex(*Result, Mode == SehLocalFilter
                                            ? "dynamic inner handled"
                                            : "finally handler"));
      }
}

TEST(DriverWDMSEH, NestedSearchAndCollidedUnwindReachTheOwningStack) {
  for (const auto *Image : images())
    for (uint64_t Address : {0x180000000ULL, 0x190000000ULL})
      for (char Mode : {char(SehNestedFilter), char(SehNestedSearch),
                        char(SehRepeatedFilter), char(SehNestedFinally)}) {
        SCOPED_TRACE(Mode);
        auto Result = emulateDriver(Image, options(Mode, Address));
        ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
        clean(*Result, Mode);
        if (Mode == SehRepeatedFilter)
          raisedCalls(*Result,
                      {"ExRaiseAccessViolation", "ExRaiseDatatypeMisalignment",
                       "ExRaiseStatus"});
        else
          raisedCalls(*Result, {"ExRaiseAccessViolation",
                                "ExRaiseDatatypeMisalignment"});
        const auto Nested = messageIndex(*Result, "nested filter decision=");
        EXPECT_LT(Nested, Result->Messages.size());
        if (Mode == SehNestedFinally) {
          EXPECT_EQ(
              std::count_if(Result->Messages.begin(), Result->Messages.end(),
                            [](const std::string &Message) {
                              return Message.find("collided finally entered") !=
                                     std::string::npos;
                            }),
              1);
          EXPECT_LT(messageIndex(*Result, "collided finally entered"), Nested);
          EXPECT_LT(Nested, messageIndex(*Result, "parent finally abnormal=1"));
          EXPECT_LT(messageIndex(*Result, "parent finally abnormal=1"),
                    messageIndex(*Result, "finally handler"));
        } else {
          EXPECT_LT(Nested,
                    messageIndex(*Result, Mode == SehNestedSearch
                                              ? "dynamic outer handled"
                                              : "dynamic inner handled"));
        }
      }
}

TEST(DriverWDMSEH, UnsupportedFilterContinuationsFailWithoutInventingReturn) {
  for (const auto *Image : images()) {
    auto Result = emulateDriver(Image, options(SehContinueApi));
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    EXPECT_EQ(Result->Stop, DriverStopReason::ModelError) << Result->Diagnostic;
    EXPECT_FALSE(Result->NTStatus);
    EXPECT_FALSE(Result->UnloadCompleted);
    EXPECT_NE(Result->Diagnostic.find("continuing a modeled API"),
              std::string::npos);
    EXPECT_EQ(messageIndex(*Result, "dynamic inner handled"),
              Result->Messages.size());
  }
}

TEST(DriverWDMSEH, ContinueExecutionRetriesUserFaultWithValidatedContext) {
  for (const auto *Image : images())
    for (uint64_t Address : {0x180000000ULL, 0x190000000ULL})
      for (char Mode :
           {char(SehRecoverUserRead), char(SehRejectContextMutation)}) {
        SCOPED_TRACE(Image);
        SCOPED_TRACE(Mode);
        auto Options = options(Mode, Address);
        for (auto Kind :
             {DriverRequestKind::Create, DriverRequestKind::DeviceControl,
              DriverRequestKind::Cleanup, DriverRequestKind::Close}) {
          DriverRequest Request;
          Request.Kind = Kind;
          Request.Device = "\\Device\\NeverDSEH";
          if (Kind == DriverRequestKind::DeviceControl) {
            Request.ControlCode = SehRecoverControlCode;
            Request.Input = {0, 0, 0, 0};
            Request.UserInputAccess = DriverUserPageAccess::NoAccess;
            Request.OutputSize = 4;
          }
          Options.Requests.push_back(std::move(Request));
        }
        auto Result = emulateDriver(Image, Options);
        ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
        EXPECT_LT(messageIndex(*Result, "resume user read"),
                  Result->Messages.size());
        EXPECT_LT(messageIndex(*Result, "filter process mode=1 creating=4096"),
                  Result->Messages.size());
        EXPECT_LT(messageIndex(*Result, "filter user memory verified"),
                  Result->Messages.size());
        if (Mode == SehRejectContextMutation) {
          EXPECT_EQ(Result->Stop, DriverStopReason::ModelError);
          EXPECT_NE(Result->Diagnostic.find("unsupported exception context"),
                    std::string::npos)
              << Result->Diagnostic;
          EXPECT_FALSE(Result->UnloadCompleted);
          EXPECT_EQ(messageIndex(*Result, "resumed value"),
                    Result->Messages.size());
          continue;
        }
        ASSERT_EQ(Result->Stop, DriverStopReason::Returned)
            << Result->Diagnostic;
        ASSERT_EQ(Result->Requests.size(), 4u);
        EXPECT_EQ(Result->Requests[1].IOStatus, 0u);
        EXPECT_EQ(Result->Requests[1].Information, 4u);
        EXPECT_EQ(Result->Requests[1].Output,
                  (std::vector<uint8_t>{0x78, 0x56, 0x34, 0x12}));
        EXPECT_TRUE(Result->UnloadCompleted);
        EXPECT_FALSE(Result->Fault);
        EXPECT_LT(messageIndex(*Result, "resumed value=12345678"),
                  Result->Messages.size());
      }
}

TEST(DriverWDMSEH, WorkerFilterCannotInheritAbsentRequestorAuthority) {
  for (const auto *Image : images())
    for (char Mode : {char(SehWorkerUserProbe), char(SehWorkerUserLock),
                      char(SehAttachedWorker), char(SehAttachedForbidden)}) {
      auto Options = options(Mode);
      DriverRequest Create;
      Create.Kind = DriverRequestKind::Create;
      Create.Device = "\\Device\\NeverDSEH";
      Options.Requests.push_back(Create);
      DriverRequest IO;
      IO.Device = Create.Device;
      IO.ControlCode = SehRecoverControlCode;
      IO.Input = {0, 0, 0, 0};
      IO.OutputSize = 4;
      Options.Requests.push_back(IO);
      if (Mode == SehAttachedWorker)
        for (const auto Kind :
             {DriverRequestKind::Cleanup, DriverRequestKind::Close}) {
          DriverRequest Final;
          Final.Kind = Kind;
          Final.Device = Create.Device;
          Options.Requests.push_back(Final);
        }
      auto Result = emulateDriver(Image, Options);
      ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
      if (Mode == SehAttachedForbidden) {
        EXPECT_EQ(Result->Stop, DriverStopReason::ModelError);
        EXPECT_NE(Result->Diagnostic.find("process attachment permits only "
                                          "bounded user-memory operations"),
                  std::string::npos)
            << Result->Diagnostic;
        ASSERT_FALSE(Result->Calls.empty());
        EXPECT_EQ(Result->Calls.back().Name, "ExRaiseAccessViolation");
        ASSERT_EQ(Result->Requests.size(), 2u);
        EXPECT_FALSE(Result->Requests.back().Completed);
        continue;
      }
      EXPECT_LT(messageIndex(*Result, "filter process mode=0 creating=4"),
                Result->Messages.size());
      if (Mode == SehAttachedWorker) {
        ASSERT_EQ(Result->Stop, DriverStopReason::Returned)
            << Result->Diagnostic;
        ASSERT_EQ(Result->Requests.size(), 4u);
        for (const auto &Request : Result->Requests) {
          EXPECT_TRUE(Request.Completed);
          EXPECT_EQ(Request.IOStatus, 0u);
        }
        EXPECT_TRUE(Result->UnloadCompleted);
        EXPECT_EQ(Result->Requests[1].Output,
                  (std::vector<uint8_t>{0x22, 0x4a, 0x6e, 0x51}));
        EXPECT_LT(messageIndex(*Result, "attached worker filter handled"),
                  Result->Messages.size());
        continue;
      }
      EXPECT_EQ(Result->Stop, DriverStopReason::ModelError)
          << Result->Diagnostic;
      EXPECT_NE(Result->Diagnostic.find("requesting process"),
                std::string::npos);
      EXPECT_TRUE(Result->Phase.starts_with("callback:"));
      ASSERT_FALSE(Result->Calls.empty());
      EXPECT_EQ(Result->Calls.back().Name, Mode == SehWorkerUserProbe
                                               ? "ProbeForWrite"
                                               : "MmProbeAndLockPages");
      ASSERT_EQ(Result->Requests.size(), 2u);
      EXPECT_FALSE(Result->Requests.back().Completed);
      EXPECT_EQ(messageIndex(*Result, "filter user memory verified"),
                Result->Messages.size());
      EXPECT_EQ(messageIndex(*Result, "worker gained user authority"),
                Result->Messages.size());
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
    EXPECT_EQ(messageIndex(*Result, "WDM SEH: complete"),
              Result->Messages.size());
    raisedCalls(*Result, {"ExRaiseStatus"});
  }
}

TEST(DriverWDMSEH, CpuMemoryFaultDoesNotUseApiExceptionRecovery) {
  for (const auto *Image : images()) {
    auto Result = emulateDriver(Image, options('C'));
    ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
    EXPECT_EQ(Result->Stop, DriverStopReason::MemoryFault)
        << Result->Diagnostic;
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
  GTEST_SKIP()
      << "configure NEVERD_WDM_SEH_FIXTURE for genuine WDK C SEH tests";
}
#endif
} // namespace
} // namespace neverd::emulation
