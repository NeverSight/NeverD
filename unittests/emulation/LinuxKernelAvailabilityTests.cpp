//===- LinuxKernelAvailabilityTests.cpp - Explicit absence inputs ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "HvfTestPolicy.h"
#include "gtest/gtest.h"
#include "os/linux/kernel/LinuxKernelAvailability.h"

#include "neverd/emulation/ExecutionConfiguration.h"
#include "neverd/emulation/ProcessReport.h"
#include "neverd/emulation/ProcessReportFields.h"
namespace neverd::emulation {
namespace {
using namespace linux_model;
TEST(LinuxKernelAvailability,
     ResidencyObservationsAreExplicitStrictAndBounded) {
  auto O = processOptionsFromJSON(
      R"({"linux_kernel":{"residency_observations":[{"address":"4294967296","pages":[0,1,0]}]}})");
  ASSERT_TRUE(bool(O)) << llvm::toString(O.takeError());
  ASSERT_TRUE(O->LinuxKernel->ResidencyObservations);
  EXPECT_EQ(O->LinuxKernel->ResidencyObservations->at(0).Address, 0x100000000u);
  EXPECT_EQ(O->LinuxKernel->ResidencyObservations->at(0).Pages,
            (std::vector<uint8_t>{0, 1, 0}));
  for (const char *Value :
       {"null", "true", "{}", "0", "[null]", "[{}]",
        R"([{"address":4097,"pages":[1]}])", R"([{"address":4096,"pages":[]}])",
        R"([{"address":4096,"pages":[true]}])",
        R"([{"address":4096,"pages":[2]}])",
        R"([{"address":4096,"pages":[-1]}])",
        R"([{"address":4096,"pages":["1"]}])",
        R"([{"address":4096,"pages":[0],"extra":0}])",
        R"([{"address":"0xfffffffffffff000","pages":[0]}])",
        R"([{"address":"18446744073709547520","pages":[0]}])",
        R"([{"address":9007199254740992,"pages":[0]}])"}) {
    auto Bad = processOptionsFromJSON(
        std::string("{\"linux_kernel\":{\"residency_observations\":") + Value +
        "}}");
    ASSERT_FALSE(bool(Bad)) << Value;
    llvm::consumeError(Bad.takeError());
  }
  auto Empty = processOptionsFromJSON(
      R"({"linux_kernel":{"residency_observations":[]}})");
  ASSERT_TRUE(bool(Empty)) << llvm::toString(Empty.takeError());
  EXPECT_TRUE(Empty->LinuxKernel->ResidencyObservations->empty());
  auto Wide = processOptionsFromJSON(
      R"({"linux_kernel":{"residency_observations":[{"address":"18014398509481984","pages":[1]}]}})");
  ASSERT_TRUE(bool(Wide)) << llvm::toString(Wide.takeError());
  EXPECT_EQ(Wide->LinuxKernel->ResidencyObservations->at(0).Address, uint64_t(1)
                                                                         << 54);
  auto &V = *O->LinuxKernel->ResidencyObservations;
  for (auto Observation : std::vector<LinuxResidencyObservation>{
           {0, {}},
           {1, {0}},
           {0, {2}},
           {UINT64_MAX - 4095, {0}},
           {0, std::vector<uint8_t>(65537)}}) {
    V = {Observation};
    auto E = validateKernelOptions(*O->LinuxKernel);
    EXPECT_TRUE(bool(E));
    llvm::consumeError(std::move(E));
  }
  V.assign(1025, {0, {0}});
  auto E = validateKernelOptions(*O->LinuxKernel);
  EXPECT_TRUE(bool(E));
  llvm::consumeError(std::move(E));
  V.assign(16, {0, std::vector<uint8_t>(65536, 1)});
  EXPECT_FALSE(bool(validateKernelOptions(*O->LinuxKernel)));
  V.push_back({0, {0}});
  E = validateKernelOptions(*O->LinuxKernel);
  EXPECT_TRUE(bool(E));
  llvm::consumeError(std::move(E));
}

TEST(LinuxKernelAvailability, AbsenceAppliesOnlyToDeclaredInterfaces) {
  std::optional<LinuxKernelOptions> Options;
  EXPECT_FALSE(unavailableKernelService(ServiceKind::PidFDOpen, Options));
  Options.emplace();
  EXPECT_FALSE(unavailableKernelService(ServiceKind::PidFDOpen, Options));
  Options->UnavailableSyscalls.insert(LinuxUnavailableSyscall::PidFDOpen);
  EXPECT_TRUE(unavailableKernelService(ServiceKind::PidFDOpen, Options));
  EXPECT_FALSE(unavailableKernelService(ServiceKind::GetPID, Options));
  EXPECT_FALSE(unavailableKernelService(ServiceKind::SetPriority, Options));
  EXPECT_FALSE(bool(validateKernelOptions(*Options)));
  Options->UnavailableSyscalls.insert(static_cast<LinuxUnavailableSyscall>(99));
  auto E = validateKernelOptions(*Options);
  ASSERT_TRUE(bool(E));
  EXPECT_EQ(llvm::toString(std::move(E)), KernelOptions);
}
TEST(LinuxKernelAvailability, JSONAndTypedInputsFailBeforeLoading) {
  auto O = processOptionsFromJSON(
      R"({"linux_kernel":{"unavailable_syscalls":["pidfd_open"]}})");
  ASSERT_TRUE(bool(O)) << llvm::toString(O.takeError());
  ASSERT_TRUE(O->LinuxKernel);
  EXPECT_TRUE(O->LinuxKernel->UnavailableSyscalls.contains(
      LinuxUnavailableSyscall::PidFDOpen));
  for (auto Profile :
       {ProcessProfile::WindowsPE64, ProcessProfile::MacOSMachO64,
        ProcessProfile::IOSMachO64, ProcessProfile::IOSSimulatorMachO64}) {
    auto R = emulateProcess("missing.elf", Profile, *O);
    ASSERT_FALSE(bool(R));
    EXPECT_EQ(llvm::toString(R.takeError()),
              process_report::LinuxKernelProfile);
  }
  O->LinuxKernel->UnavailableSyscalls.insert(
      static_cast<LinuxUnavailableSyscall>(99));
  auto Bad = emulateProcess("missing.elf", ProcessProfile::LinuxELF64, *O);
  ASSERT_FALSE(bool(Bad));
  EXPECT_EQ(llvm::toString(Bad.takeError()), KernelOptions);
  for (const char *Value :
       {"null", "[]", "{}", R"({"unavailable_syscalls":null})",
        R"({"unavailable_syscalls":true})", R"({"unavailable_syscalls":[434]})",
        R"({"unavailable_syscalls":["openat"]})",
        R"({"unavailable_syscalls":["unknown"]})",
        R"({"unavailable_syscalls":["pidfd_open","pidfd_open"]})",
        R"({"unavailable_syscalls":["pidfd_open"],"extra":0})"}) {
    auto R =
        processOptionsFromJSON(std::string("{\"linux_kernel\":") + Value + "}");
    ASSERT_FALSE(bool(R)) << Value;
    llvm::consumeError(R.takeError());
  }
  auto Empty =
      processOptionsFromJSON(R"({"linux_kernel":{"unavailable_syscalls":[]}})");
  ASSERT_TRUE(bool(Empty)) << llvm::toString(Empty.takeError());
  EXPECT_TRUE(Empty->LinuxKernel->UnavailableSyscalls.empty());
}
TEST(LinuxKernelAvailability,
     XAttrQueryImportContractsRequireExactTypedOrJSONInput) {
  struct Case {
    const char *Label;
    LinuxXAttrQueryImport Import;
  };
  constexpr Case Cases[] = {
      {"target-before-name", LinuxXAttrQueryImport::TargetBeforeName},
      {"name-before-target", LinuxXAttrQueryImport::NameBeforeTarget}};
  for (const auto &C : Cases) {
    auto O = processOptionsFromJSON(
        std::string("{\"linux_kernel\":{\"xattr_query_import\":\"") + C.Label +
        "\"}}");
    ASSERT_TRUE(bool(O)) << llvm::toString(O.takeError());
    ASSERT_TRUE(O->LinuxKernel);
    EXPECT_EQ(O->LinuxKernel->XAttrQueryImport, C.Import);
    EXPECT_EQ(xattrQueryImport(*O->LinuxKernel), C.Import);
    EXPECT_FALSE(O->LinuxKernel->GKI);
    EXPECT_FALSE(bool(validateKernelOptions(*O->LinuxKernel)));
  }
  for (const char *Value :
       {"null", "true", "false", "0", "[]", "{}", "\"\"", "\"unknown\"",
        "\"target-first\"", "\"TARGET-BEFORE-NAME\""}) {
    auto O = processOptionsFromJSON(
        std::string("{\"linux_kernel\":{\"xattr_query_import\":") + Value +
        "}}");
    ASSERT_FALSE(bool(O)) << Value;
    llvm::consumeError(O.takeError());
  }
  auto Extra = processOptionsFromJSON(
      R"({"linux_kernel":{"xattr_query_import":"target-before-name","extra":0}})");
  ASSERT_FALSE(bool(Extra));
  llvm::consumeError(Extra.takeError());
  ProcessOptions O;
  auto &K = O.LinuxKernel.emplace();
  EXPECT_FALSE(xattrQueryImport(K));
  K.XAttrQueryImport = static_cast<LinuxXAttrQueryImport>(99);
  EXPECT_FALSE(xattrQueryImport(K));
  auto E = validateKernelOptions(K);
  ASSERT_TRUE(bool(E));
  EXPECT_EQ(llvm::toString(std::move(E)), KernelOptions);
  auto Bad = emulateProcess("missing.elf", ProcessProfile::LinuxELF64, O);
  ASSERT_FALSE(bool(Bad));
  EXPECT_EQ(llvm::toString(Bad.takeError()), KernelOptions);
}

TEST(LinuxKernelAvailability,
     AnonymousVMANamesRequireExplicitTypedOrJSONInput) {
  auto O = processOptionsFromJSON(
      R"({"linux_kernel":{"anonymous_vma_names":"android-user-pointer"}})");
  ASSERT_TRUE(bool(O)) << llvm::toString(O.takeError());
  ASSERT_TRUE(O->LinuxKernel);
  EXPECT_EQ(O->LinuxKernel->AnonymousVMANames,
            LinuxAnonymousVMANames::AndroidUserPointer);
  EXPECT_FALSE(O->LinuxKernel->GKI);
  EXPECT_FALSE(bool(validateKernelOptions(*O->LinuxKernel)));
  for (const char *Value : {"null", "true", "false", "0", "[]", "{}", "\"\"",
                            "\"linux\"", "\"ANDROID-USER-POINTER\""}) {
    auto Bad = processOptionsFromJSON(
        std::string("{\"linux_kernel\":{\"anonymous_vma_names\":") + Value +
        "}}");
    ASSERT_FALSE(bool(Bad)) << Value;
    llvm::consumeError(Bad.takeError());
  }
  O->LinuxKernel->AnonymousVMANames = static_cast<LinuxAnonymousVMANames>(99);
  auto Bad = emulateProcess("missing.elf", ProcessProfile::LinuxELF64, *O);
  ASSERT_FALSE(bool(Bad));
  EXPECT_EQ(llvm::toString(Bad.takeError()), KernelOptions);
}

TEST(LinuxKernelAvailability,
     XAttrQueryImportPreservesGKIAndRejectsTwoAuthorities) {
  struct Case {
    AndroidGKIKernel Kernel;
    const char *Label;
    bool NameFirst;
  };
  constexpr Case Cases[] = {
#define NEVERD_GKI_RELEASE_CASE(Name, Label, ThreadFlag, SingleBuffer, Error,  \
                                NameFirst)                                     \
  {AndroidGKIKernel::Name, Label, NameFirst},
#include "GKIReleaseCases.def"
#undef NEVERD_GKI_RELEASE_CASE
  };
  for (const auto &C : Cases) {
    SCOPED_TRACE(C.Label);
    LinuxKernelOptions K;
    K.GKI = C.Kernel;
    const auto Expected = C.NameFirst ? LinuxXAttrQueryImport::NameBeforeTarget
                                      : LinuxXAttrQueryImport::TargetBeforeName;
    EXPECT_EQ(xattrQueryImport(K), Expected);
    EXPECT_FALSE(bool(validateKernelOptions(K)));
    for (const auto &Import : {LinuxXAttrQueryImport::TargetBeforeName,
                               LinuxXAttrQueryImport::NameBeforeTarget}) {
      K.XAttrQueryImport = Import;
      EXPECT_FALSE(xattrQueryImport(K));
      auto E = validateKernelOptions(K);
      ASSERT_TRUE(bool(E));
      EXPECT_EQ(llvm::toString(std::move(E)), KernelOptions);
    }
    for (const char *Import : {"target-before-name", "name-before-target"}) {
      auto O = processOptionsFromJSON(
          std::string("{\"linux_kernel\":{\"gki\":\"") + C.Label +
          "\",\"xattr_query_import\":\"" + Import + "\"}}");
      ASSERT_FALSE(bool(O));
      llvm::consumeError(O.takeError());
    }
  }
  LinuxKernelOptions K;
  K.GKI = static_cast<AndroidGKIKernel>(99);
  EXPECT_FALSE(xattrQueryImport(K));
}

TEST(LinuxKernelAvailability,
     XAttrQueryImportDoesNotSelectOtherKernelContracts) {
  for (const char *Import : {"target-before-name", "name-before-target"}) {
    auto O = processOptionsFromJSON(
        std::string("{\"linux_kernel\":{\"xattr_query_import\":\"") + Import +
        R"(","unavailable_syscalls":["pidfd_open"]}})");
    ASSERT_TRUE(bool(O)) << llvm::toString(O.takeError());
    auto &K = *O->LinuxKernel;
    EXPECT_FALSE(K.GKI);
    EXPECT_FALSE(K.Tasks);
    EXPECT_TRUE(
        unavailableKernelService(ServiceKind::PidFDOpen, O->LinuxKernel));
    EXPECT_FALSE(
        unavailableKernelService(ServiceKind::GetXAttr, O->LinuxKernel));
    EXPECT_FALSE(bool(validateKernelOptions(K)));
    K.XAttrQueryImport.reset();
    EXPECT_FALSE(xattrQueryImport(K));
    EXPECT_FALSE(bool(validateKernelOptions(K)));
  }
}

struct Profile {
  const char *Name;
  ExecutionBackendKind Backend;
  GuestArchitecture ISA;
};
constexpr Profile Profiles[] = {
#define NEVERD_USER_PROFILE(Name, Backend, ISA, Contract)                      \
  {#Name, ExecutionBackendKind::Backend, GuestArchitecture::ISA},
#include "UserExecutionCases.def"
#undef NEVERD_USER_PROFILE
};
void PrintTo(const Profile &P, std::ostream *OS) { *OS << P.Name; }
class LinuxKernelProcess : public testing::TestWithParam<Profile> {
protected:
  ProcessOptions Options;
  void SetUp() override {
#ifndef NEVERD_PROCESS_FIXTURE_DIR
    GTEST_SKIP() << "Clang and ld.lld process fixtures unavailable";
#else
    Options.Backend = GetParam().Backend;
    ExecutionConfiguration C;
    C.Backend = Options.Backend;
    C.Architecture = GetParam().ISA;
    C.Contract = GetParam().ISA == GuestArchitecture::X64
                     ? ExecutionContract::CheckedUserX64
                     : ExecutionContract::CheckedUserAArch64;
    auto Probe = probeExecutionBackend(C);
    ASSERT_TRUE(bool(Probe)) << llvm::toString(Probe.takeError());
    if (Probe->Availability != BackendAvailability::Available) {
      if (requireHvf(GetParam().Backend, GetParam().ISA))
        FAIL() << Probe->Reason;
      GTEST_SKIP() << Probe->Reason;
    }
    Options.LinuxKernel.emplace().UnavailableSyscalls.insert(
        LinuxUnavailableSyscall::PidFDOpen);
#endif
  }
  ProcessResult run(char Mode, const char *Opt) {
    Options.Arguments = {"kernel", std::string(1, Mode)};
#ifdef NEVERD_PROCESS_FIXTURE_DIR
    auto Path =
        std::filesystem::path(NEVERD_PROCESS_FIXTURE_DIR) /
        (std::string(GetParam().ISA == GuestArchitecture::X64 ? "X64"
                                                              : "AArch64") +
         "-kernel-" + Opt + ".elf");
#else
    std::filesystem::path Path;
#endif
    return llvm::cantFail(
        emulateProcess(Path, ProcessProfile::LinuxELF64, Options));
  }
};
TEST_P(LinuxKernelProcess, AbsentCallReturnsENOSYSBeforeArgumentValidation) {
  for (const char *Opt : {"O0", "O2"}) {
    auto R = run('u', Opt);
    ASSERT_EQ(R.Stop, ProcessStopReason::Exited) << R.Diagnostic;
    EXPECT_EQ(R.ExitStatus, 0u);
    unsigned Calls = 0;
    for (const auto &Event : R.Services) {
      if (Event.Number == 434) {
        EXPECT_EQ(Event.Result, uint64_t(0) - 38);
        ++Calls;
      }
    }
    EXPECT_EQ(Calls, 2u);
  }
}
TEST_P(LinuxKernelProcess, UnspecifiedAndOtherInterfacesKeepTheirBoundaries) {
  for (char Mode : {'a', 'e', 'x'}) {
    if (Mode == 'a')
      Options.LinuxKernel.reset();
    else {
      Options.LinuxKernel.emplace();
      if (Mode == 'x')
        Options.LinuxKernel->UnavailableSyscalls.insert(
            LinuxUnavailableSyscall::PidFDOpen);
    }
    auto R = run(Mode == 'x' ? 'x' : 'a', "O2");
    EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService);
    ASSERT_FALSE(R.Services.empty());
    EXPECT_FALSE(R.Services.back().Result);
  }
}
INSTANTIATE_TEST_SUITE_P(Backends, LinuxKernelProcess,
                         testing::ValuesIn(Profiles));
} // namespace
} // namespace neverd::emulation
