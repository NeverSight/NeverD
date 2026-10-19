//===- KernelBugCheckTerminationTests.cpp - Terminal guest crash requests -===//
// NeverD Decompiler
///
/// \file
/// Terminal guest crash requests.
///

#include "backends/unicorn/UnicornBackend.h"
#include "gtest/gtest.h"
#include "os/windows/driver/DriverImage.h"
#include "os/windows/kernel/KernelException.h"
#include "os/windows/kernel/KernelModel.h"

#include "llvm/Support/JSON.h"

namespace neverd::emulation {
namespace {
TEST(KernelBugCheckTermination, CrashMetadataCannotBecomeScenarioSuccess) {
  DriverResult Result;
  Result.Stop = DriverStopReason::Returned;
  Result.NTStatus = 0;
  auto Before = llvm::cantFail(llvm::json::parse(driverResultJSON(Result)));
  EXPECT_EQ(Before.getAsObject()->getBoolean("scenario_success"), true);
  EXPECT_TRUE(Before.getAsObject()->get("bugcheck")->getAsNull());
  Result.BugCheck = DriverBugCheck{0xdead0042u, std::nullopt};
  auto After = llvm::cantFail(llvm::json::parse(driverResultJSON(Result)));
  EXPECT_EQ(After.getAsObject()->getBoolean("scenario_success"), false);
  EXPECT_NE(After.getAsObject()->getObject("bugcheck"), nullptr);
}

TEST(KernelBugCheckTermination,
     TypedCrashPreservesCodeWidthAndKnownParameters) {
  auto Memory = llvm::cantFail(UnicornBackend::create(4 * 1024 * 1024));
  DriverResult Result;
  KernelModel Model(*Memory, Result);
  DriverImage I;
  I.Base = 0x180000000;
  I.Entry = I.Base + 4096;
  I.Size = 0x3000;
  llvm::cantFail(Model.initialize(I, {}));
  Model.enterForeground();
  Model.enterExecution(profile::StackBase);
  EXPECT_EQ(Model.argumentCount("KeBugCheck"), 1u);
  EXPECT_EQ(Model.argumentCount("KeBugCheckEx"), 5u);
  for (uint8_t Level : {0, 1, 2, 15}) {
    llvm::cantFail(Model.call("KfRaiseIrql", {Level}));
    for (bool Extended : {false, true}) {
      const std::vector<uint64_t> Arguments =
          Extended ? std::vector<uint64_t>{UINT64_C(0xabcdef00dead0042), 1,
                                           UINT64_MAX, 3,
                                           UINT64_C(0x8000000000000001)}
                   : std::vector<uint64_t>{UINT64_C(0xabcdef00dead0042)};
      auto R = Model.call(Extended ? "KeBugCheckEx" : "KeBugCheck", Arguments);
      ASSERT_FALSE(bool(R));
      std::optional<DriverBugCheck> Request;
      auto E =
          llvm::handleErrors(R.takeError(), [&](const KernelGuestBugCheck &B) {
            Request = B.request();
          });
      ASSERT_FALSE(bool(E)) << llvm::toString(std::move(E));
      ASSERT_TRUE(Request);
      EXPECT_EQ(Request->Code, 0xdead0042u);
      EXPECT_EQ(Request->Parameters.has_value(), Extended);
      if (Extended)
        EXPECT_EQ(*Request->Parameters,
                  (std::array<uint64_t, 4>{1, UINT64_MAX, 3,
                                           UINT64_C(0x8000000000000001)}));
      EXPECT_EQ(Model.currentIRQL(), Level);
    }
    llvm::cantFail(Model.call("KeLowerIrql", {0}));
  }
}

TEST(KernelBugCheckTermination,
     RealCrashStopsEntryUnloadAndSystemThreadAcrossContracts) {
  for (auto Contract :
       {ExecutionContract::Legacy, ExecutionContract::CheckedX64})
    for (bool Preemptive : {false, true})
      for (const char *Name :
           {"driver_bugcheck_terminal", "driver_bugcheck_simple",
            "driver_bugcheck_unload", "driver_bugcheck_thread",
            "driver_bugcheck_seh"}) {
        SCOPED_TRACE(std::to_string(unsigned(Contract)) + ":" +
                     std::to_string(Preemptive) + ":" + Name);
        DriverOptions O;
        O.Version = DriverOptions::WindowsVersion{10, 0, 19041};
        O.Contract = Contract;
        O.Unload = true;
        if (Preemptive)
          O.Scheduling = DriverScheduling{1, 1};
        auto R = emulateDriver(std::filesystem::path(NEVERD_DRIVER_FIXTURES) /
                                   (std::string(Name) + ".sys"),
                               O);
        ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
        EXPECT_EQ(R->Stop, DriverStopReason::BugCheck) << R->Diagnostic;
        EXPECT_FALSE(R->UnloadCompleted);
        ASSERT_TRUE(R->BugCheck);
        ASSERT_EQ(R->BugCheckCallbacks.size(), 1u);
        EXPECT_FALSE(R->BugCheckCallbacks[0].ComponentStorageReleased);
        EXPECT_EQ(R->BugCheck->Code, 0xdead0042u);
        const bool Simple = std::string_view(Name).ends_with("simple");
        const bool Entry = !std::string_view(Name).ends_with("unload") &&
                           !std::string_view(Name).ends_with("thread");
        EXPECT_EQ(R->BugCheck->Parameters.has_value(), !Simple);
        // A preemptive system thread may crash before or after DriverEntry
        // publishes its return value. A completed entry must retain success.
        const bool PreemptiveThread =
            Preemptive && std::string_view(Name).ends_with("thread");
        if (Entry)
          EXPECT_FALSE(R->NTStatus);
        else if (!PreemptiveThread || R->NTStatus)
          EXPECT_EQ(R->NTStatus, 0u);
        ASSERT_FALSE(R->Calls.empty());
        EXPECT_EQ(R->Calls.back().Name, Simple ? "KeBugCheck" : "KeBugCheckEx");
        EXPECT_FALSE(R->Calls.back().Result);
        EXPECT_EQ(R->Phase == "unload",
                  std::string_view(Name).ends_with("unload"));
        auto JSON = llvm::cantFail(llvm::json::parse(driverResultJSON(*R)));
        auto *Object = JSON.getAsObject();
        ASSERT_NE(Object, nullptr);
        EXPECT_EQ(Object->getString("stop_reason"), "bugcheck");
        EXPECT_EQ(Object->getBoolean("scenario_success"), false);
        auto *Bug = Object->getObject("bugcheck");
        ASSERT_NE(Bug, nullptr);
        EXPECT_EQ(Bug->getInteger("code"), int64_t(0xdead0042u));
        EXPECT_EQ(Bug->getBoolean("dump_callbacks_executed"), false);
        if (Simple)
          EXPECT_TRUE(Bug->get("parameters")->getAsNull());
        else {
          auto *Parameters = Bug->getArray("parameters");
          ASSERT_NE(Parameters, nullptr);
          ASSERT_EQ(Parameters->size(), 4u);
          EXPECT_EQ((*Parameters)[0].getAsString(), "0x1122334455667788");
          EXPECT_EQ((*Parameters)[1].getAsString(), "0xFFFFFFFFFFFFFFFF");
          EXPECT_EQ((*Parameters)[2].getAsString(), "0x0");
          EXPECT_EQ((*Parameters)[3].getAsString(), "0x8000000000000001");
        }
      }
}
} // namespace
} // namespace neverd::emulation
