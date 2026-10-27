//===- AndroidJNITests.cpp - JNI values, lifetime and thread boundaries ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Original guest callers across relocation packing and CPU transports.
//===----------------------------------------------------------------------===//
#include "gtest/gtest.h"

#include "neverd/emulation/ExecutionConfiguration.h"
#include "neverd/emulation/ProcessReport.h"
#include "neverd/emulation/ProcessSession.h"

#include "llvm/Support/Endian.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FileUtilities.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"

#include <optional>
#include <tuple>

namespace neverd::emulation {
namespace {
class AndroidJNI : public testing::TestWithParam<
                       std::tuple<const char *, ExecutionBackendKind>> {
protected:
  std::filesystem::path Path;
  ProcessOptions Options;
  void SetUp() override {
#ifndef NEVERD_ANDROID_FIXTURE_DIR
    GTEST_SKIP() << "Android fixture compiler unavailable";
#else
    Path = std::filesystem::path(NEVERD_ANDROID_FIXTURE_DIR) /
           (std::string(std::get<0>(GetParam())) + ".so");
    Options.Backend = std::get<1>(GetParam());
    ExecutionConfiguration C;
    C.Backend = Options.Backend;
    C.Architecture = GuestArchitecture::AArch64;
    C.Contract = ExecutionContract::CheckedUserAArch64;
    auto P = probeExecutionBackend(C);
    ASSERT_TRUE(bool(P)) << llvm::toString(P.takeError());
    if (P->Availability != BackendAvailability::Available)
      GTEST_SKIP() << P->Reason;
    Options.Android.emplace();
    Options.Android->Initialize = false;
    Options.Android->JNI = true;
    Options.Android->ThreadLimit = 2;
    Options.InstructionQuantum = 83;
    Options.Limits.Instructions = 200000;
    Options.Limits.Events = 5000;
    Options.Limits.TimeoutMicroseconds = 30000000;
#endif
  }
  ProcessResult run(const char *Entry) {
    Options.Android->EntrySymbol = Entry;
    return llvm::cantFail(
        emulateProcess(Path, ProcessProfile::AndroidNativeAArch64, Options));
  }
};
TEST_P(AndroidJNI, VMThreadAttachmentAndVersions) {
  auto R = run("jni_vm_lifecycle");
  EXPECT_EQ(R.Stop, ProcessStopReason::Returned) << R.Diagnostic;
  EXPECT_EQ(R.ReturnValue, 0u);
}
TEST_P(AndroidJNI, StatLoggerUsesSharedClockArraysAndRetainedLabels) {
  Options.LinuxTime.emplace().Clocks = {{7, {50, 987654321}}};
  for (unsigned advance : {0u, 31u}) {
    Options.LinuxTime->NanosecondsPerInstruction = advance;
    for (unsigned mode : {0u, 1u}) {
      Options.Android->Arguments = {mode};
      auto result = run("jni_stat_logger");
      ASSERT_EQ(result.Stop, ProcessStopReason::Returned) << result.Diagnostic;
      EXPECT_EQ(result.ReturnValue, 0u);
    }
  }
}
TEST_P(AndroidJNI, StatLoggerReadsBootClockBeforeNullLabelsException) {
  Options.Android->Arguments = {1};
  auto missing = run("jni_stat_logger");
  EXPECT_EQ(missing.Stop, ProcessStopReason::UnsupportedService);
  EXPECT_NE(missing.Diagnostic.find("no explicit linux_time input"),
            std::string::npos);
  Options.LinuxTime.emplace().Clocks = {{1, {10, 0}}};
  auto wrong_clock = run("jni_stat_logger");
  EXPECT_EQ(wrong_clock.Stop, ProcessStopReason::UnsupportedService);
  EXPECT_NE(wrong_clock.Diagnostic.find("no explicit linux_time input"),
            std::string::npos);
}
TEST_P(AndroidJNI, StatLoggerDoesNotInventLoggingOrStatisticsUpdates) {
  Options.Android->Arguments = {2};
  auto result = run("jni_stat_logger");
  EXPECT_EQ(result.Stop, ProcessStopReason::UnsupportedService);
  EXPECT_NE(result.Diagnostic.find("unmodeled Java method"), std::string::npos);
}
TEST_P(AndroidJNI, HashMapRetainsStringNullAndReferenceValuesAcrossDetach) {
  auto result = run("jni_hash_map");
  EXPECT_EQ(result.Stop, ProcessStopReason::Returned) << result.Diagnostic;
  EXPECT_EQ(result.ReturnValue, 0u);
}
TEST_P(AndroidJNI, HashMapRefusesCustomKeysRawObjectsAndUnmodeledEquality) {
  for (uint64_t mode : {1u, 2u, 3u}) {
    Options.Android->Arguments = {mode};
    auto result = run("jni_hash_map");
    EXPECT_EQ(result.Stop, ProcessStopReason::UnsupportedService)
        << result.Diagnostic;
    EXPECT_NE(result.Diagnostic.find(mode == 1   ? "String and null keys"
                                     : mode == 2 ? "contents are not supplied"
                                                 : "equality, hashing"),
              std::string::npos);
  }
}
TEST_P(AndroidJNI, SystemPropertyIntegersShareNativeRecordsAndArtKeyEncoding) {
  Options.Android->Properties = {{"test.valid", "28"},
                                 {"test.hex", "0x1c"},
                                 {"test.min", "-2147483648"},
                                 {"test.max", "2147483647"},
                                 {"test.octal", "028"},
                                 {"test.sign", "\t -28"},
                                 {"test.empty", ""},
                                 {"test.overflow", "2147483648"},
                                 {"test.underflow", "-2147483649"},
                                 {"test.trailing", "28 "},
                                 {"test.signedhex", "-0x1c"},
                                 {"test.spacehex", " 0x1c"},
                                 {"test.hexoverflow", "0x80000000"},
                                 {"\xc0\x80\xed\xa0\x80"
                                  "a\xf0\x9f\x98\x80",
                                  "-7"}};
  auto result = run("jni_property_int");
  EXPECT_EQ(result.Stop, ProcessStopReason::Returned) << result.Diagnostic;
  EXPECT_EQ(result.ReturnValue, 0u);
}
TEST_P(AndroidJNI, SystemPropertyIntegersRejectChangedOrUnmappedNativeRecords) {
  Options.Android->Properties = {{"test.valid", "28"}};
  for (uint64_t mode : {1u, 2u}) {
    Options.Android->Arguments = {mode};
    auto result = run("jni_property_int");
    EXPECT_EQ(result.Stop, ProcessStopReason::RuntimeFailure)
        << result.Diagnostic;
    if (mode == 1)
      EXPECT_NE(
          result.Diagnostic.find("changed the immutable Android property"),
          std::string::npos);
  }
}
TEST_P(AndroidJNI, SystemPropertyReadProjectionDoesNotInventWrites) {
  Options.Android->Arguments = {3};
  auto result = run("jni_property_int");
  EXPECT_EQ(result.Stop, ProcessStopReason::UnsupportedService)
      << result.Diagnostic;
  EXPECT_NE(result.Diagnostic.find("unmodeled Java method"), std::string::npos);
}
TEST_P(AndroidJNI, SystemClockSharesLinuxEpochsIdleAndInstructionTime) {
  Options.LinuxTime.emplace();
  Options.LinuxTime->Clocks = {{1, {10, 123456789}}, {7, {50, 987654321}}};
  Options.LinuxTime->AdvanceOnIdle = true;
  for (unsigned mode : {0u, 5u}) {
    Options.Android->Arguments = {mode};
    Options.LinuxTime->NanosecondsPerInstruction = mode ? 31 : 0;
    auto result = run("jni_system_clock");
    ASSERT_EQ(result.Stop, ProcessStopReason::Returned) << result.Diagnostic;
    EXPECT_EQ(result.ReturnValue, 0u);
  }
}
TEST_P(AndroidJNI, SystemClockRequiresEachClockAndSignedNanosecondRange) {
  for (unsigned mode : {1u, 2u, 3u}) {
    Options.Android->Arguments = {mode};
    Options.LinuxTime.reset();
    auto absent = run("jni_system_clock");
    EXPECT_EQ(absent.Stop, ProcessStopReason::UnsupportedService);
    EXPECT_NE(absent.Diagnostic.find("no explicit linux_time input"),
              std::string::npos);
    Options.LinuxTime.emplace().Clocks = {{mode == 1 ? 7 : 1, {10, 123456789}}};
    auto other_clock = run("jni_system_clock");
    EXPECT_EQ(other_clock.Stop, ProcessStopReason::UnsupportedService);
    EXPECT_NE(other_clock.Diagnostic.find("no explicit linux_time input"),
              std::string::npos);
    const int clock = mode == 1 ? 1 : 7;
    for (auto time :
         {LinuxTimespec{-1, 999999999}, LinuxTimespec{9223372036, 854775808}}) {
      Options.LinuxTime->Clocks = {{clock, time}};
      auto invalid = run("jni_system_clock");
      EXPECT_EQ(invalid.Stop, ProcessStopReason::UnsupportedService);
      EXPECT_EQ(invalid.Diagnostic,
                mode == 1 ? "Android monotonic time exceeds its range"
                          : "Android boot time exceeds its range");
    }
    Options.LinuxTime->Clocks = {{clock, {9223372036, 854775807}}};
    auto maximum = run("jni_system_clock");
    ASSERT_EQ(maximum.Stop, ProcessStopReason::Returned) << maximum.Diagnostic;
    EXPECT_EQ(maximum.ReturnValue,
              mode == 3 ? uint64_t(INT64_MAX) : uint64_t(INT64_MAX) / 1000000);
    Options.LinuxTime->Clocks = {{clock, {0, 0}}};
    auto zero = run("jni_system_clock");
    ASSERT_EQ(zero.Stop, ProcessStopReason::Returned) << zero.Diagnostic;
    EXPECT_EQ(zero.ReturnValue, 0u);
  }
}
TEST_P(AndroidJNI, SystemClockDoesNotSupplyOtherNativeMethods) {
  Options.Android->Arguments = {4};
  auto result = run("jni_system_clock");
  EXPECT_EQ(result.Stop, ProcessStopReason::UnsupportedService);
  EXPECT_NE(result.Diagnostic.find("unmodeled Java method"), std::string::npos);
}
TEST_P(AndroidJNI,
       LoaderFailuresRetainExceptionsAcrossDetachWithoutClosingNamespaces) {
  using Snapshot = mobile::dalvik::AndroidApplicationSnapshot;
  using Loader = Snapshot::ClassLookupFailure::Loader;
  Options.Android->Application = Snapshot{Snapshot::Phase::Attached};
  Options.Android->Application->class_lookup_failures = {
      {Loader::Boot, "fixture.Optional", "declared missing"}};
  auto R = run("jni_loader_failure");
  EXPECT_EQ(R.Stop, ProcessStopReason::Returned) << R.Diagnostic;
  EXPECT_EQ(R.ReturnValue, 0u);
  for (unsigned Mode : {1u, 2u}) {
    Options.Android->Arguments = {Mode};
    auto Unknown = run("jni_loader_failure");
    EXPECT_EQ(Unknown.Stop, Mode == 1 ? ProcessStopReason::UnsupportedService
                                      : ProcessStopReason::RuntimeFailure)
        << Unknown.Diagnostic;
    if (Mode == 2)
      EXPECT_NE(Unknown.Diagnostic.find("unmodeled class: fixture/Optional"),
                std::string::npos);
  }
  Options.Android->Arguments.clear();
  Options.Android->Application->class_lookup_failures.clear();
  auto Unknown = run("jni_loader_failure");
  EXPECT_EQ(Unknown.Stop, ProcessStopReason::UnsupportedService);
}
TEST_P(AndroidJNI, ExactQueryLookupFailuresPreserveSpacesAcrossDetach) {
  using Snapshot = mobile::dalvik::AndroidApplicationSnapshot;
  using Loader = Snapshot::ClassLookupFailure::Loader;
  Options.Android->Application = Snapshot{Snapshot::Phase::Attached};
  Options.Android->Application->class_lookup_failures = {
      {Loader::Boot, {}, "declared missing", "fixture.Optional "}};
  auto R = run("jni_loader_exact_query");
  EXPECT_EQ(R.Stop, ProcessStopReason::Returned) << R.Diagnostic;
  EXPECT_EQ(R.ReturnValue, 0u);
  for (unsigned Mode : {1u, 2u, 3u}) {
    Options.Android->Arguments = {Mode};
    auto Unknown = run("jni_loader_exact_query");
    EXPECT_EQ(Unknown.Stop, Mode == 2 ? ProcessStopReason::RuntimeFailure
                                      : ProcessStopReason::UnsupportedService)
        << Unknown.Diagnostic;
    if (Mode == 2)
      EXPECT_NE(Unknown.Diagnostic.find("unmodeled class: fixture/Optional "),
                std::string::npos);
  }
  Options.Android->Arguments.clear();
  Options.Android->Application->class_lookup_failures.clear();
  auto Unknown = run("jni_loader_exact_query");
  EXPECT_EQ(Unknown.Stop, ProcessStopReason::UnsupportedService);
}
TEST_P(AndroidJNI, InstrumentationFieldAndGetterShareLifetimeAndWrites) {
  using Snapshot = mobile::dalvik::AndroidApplicationSnapshot;
  for (unsigned Mode : {1u, 2u, 3u}) {
    Options.Android->Application = Snapshot{Snapshot::Phase::Attached};
    if (Mode != 3)
      Options.Android->Application->instrumentation =
          Mode == 1 ? Snapshot::Instrumentation::Uninitialized
                    : Snapshot::Instrumentation::Platform;
    Options.Android->Arguments = {Mode};
    auto R = run("jni_instrumentation");
    EXPECT_EQ(R.Stop, ProcessStopReason::Returned) << R.Diagnostic;
    EXPECT_EQ(R.ReturnValue, 0u);
  }
  Options.Android->Application = Snapshot{Snapshot::Phase::Attached};
  Options.Android->Arguments = {0};
  auto Unknown = run("jni_instrumentation");
  EXPECT_EQ(Unknown.Stop, ProcessStopReason::UnsupportedService);
  EXPECT_NE(Unknown.Diagnostic.find("instrumentation state"),
            std::string::npos);
}
TEST_P(AndroidJNI, ApplicationSnapshotHasThreeDistinctLifecyclePhases) {
  using Phase = mobile::dalvik::AndroidApplicationSnapshot::Phase;
  for (auto P : {Phase::Unattached, Phase::Attached, Phase::Application}) {
    Options.Android->Application =
        mobile::dalvik::AndroidApplicationSnapshot{P, {}};
    Options.Android->Arguments = {uint64_t(P)};
    auto R = run("jni_application_snapshot");
    EXPECT_EQ(R.Stop, ProcessStopReason::Returned) << R.Diagnostic;
    EXPECT_EQ(R.ReturnValue, 0u);
  }
}
TEST_P(AndroidJNI, JavaStateObservationPreservesCompleteExecution) {
  using Phase = mobile::dalvik::AndroidApplicationSnapshot::Phase;
  Options.Android->TraceLimit = 50000;
  for (auto P :
       {std::optional<Phase>{}, std::optional{Phase::Unattached},
        std::optional{Phase::Attached}, std::optional{Phase::Application}}) {
    Options.Android->Application =
        P ? std::optional{mobile::dalvik::AndroidApplicationSnapshot{*P, {}}}
          : std::nullopt;
    Options.Android->Arguments = {uint64_t(P.value_or(Phase::Unattached))};
    Options.Android->ObserveJavaState = false;
    auto Baseline = run("jni_application_snapshot");
    ASSERT_FALSE(Baseline.JavaState);
    Options.Android->ObserveJavaState = true;
    auto Observed = run("jni_application_snapshot");
    ASSERT_TRUE(Observed.JavaState);
    const auto &State = *Observed.JavaState;
    EXPECT_EQ(State.framework.declared_phase, P);
    EXPECT_FALSE(State.classes.empty());
    if (P == Phase::Application) {
      EXPECT_NE(State.framework.application, 0u);
      EXPECT_NE(State.framework.application_loader, 0u);
      EXPECT_NE(State.framework.boot_loader,
                State.framework.application_loader);
    } else {
      EXPECT_EQ(State.framework.application, 0u);
      EXPECT_EQ(State.framework.application_loader, 0u);
    }
    EXPECT_EQ(Observed.Stop, P ? ProcessStopReason::Returned
                               : ProcessStopReason::UnsupportedService);
    // Covers counters, references/return bits, threads, calls, trace, memory,
    // diagnostics and existing observations, not just the final result code.
    Observed.JavaState.reset();
    EXPECT_EQ(processResultJSON(Observed), processResultJSON(Baseline));
  }
  Options.Android->JNI = false;
  Options.Android->Application.reset();
  auto Bad =
      emulateProcess(Path, ProcessProfile::AndroidNativeAArch64, Options);
  ASSERT_FALSE(bool(Bad));
  EXPECT_NE(llvm::toString(Bad.takeError()).find("observation requires JNI"),
            std::string::npos);
}
TEST_P(AndroidJNI,
       JavaStateSeparatesNativeRegistrationFromClassInitialization) {
#ifndef NEVERD_ANDROID_DEX_FIXTURE_DIR
  GTEST_SKIP() << "Python DEX fixture generator unavailable";
#else
  auto File = llvm::MemoryBuffer::getFile(
      (std::filesystem::path(NEVERD_ANDROID_DEX_FIXTURE_DIR) /
       "registration.dex")
          .string());
  ASSERT_TRUE(bool(File));
  auto Bytes = (*File)->getBuffer();
  Options.Android->DexImages.emplace_back(Bytes.begin(), Bytes.end());
  Options.Android->TraceLimit = 50000;
  for (unsigned Mode : {0u, 5u}) {
    SCOPED_TRACE(Mode);
    Options.Android->Arguments = {Mode};
    Options.Android->ObserveJavaState = false;
    auto Before = run("jni_native_registration");
    Options.Android->ObserveJavaState = true;
    auto After = run("jni_native_registration");
    ASSERT_EQ(After.Stop, ProcessStopReason::Returned) << After.Diagnostic;
    ASSERT_TRUE(After.JavaState);
    EXPECT_FALSE(After.JavaState->terminal);
    EXPECT_FALSE(After.JNIRegistrations.empty());
    unsigned Supplied = 0, Materialized = 0;
    for (const auto &C : After.JavaState->classes) {
      if (!C.supplied)
        continue;
      ++Supplied;
      Materialized += C.class_object != 0;
      EXPECT_EQ(C.source_id, "supplied-dex-0");
      using Init = mobile::dalvik::JavaClassState::Initialization;
      EXPECT_EQ(C.initialization,
                Mode == 5 && C.descriptor == "Lregistration/Deferred;"
                    ? Init::Erroneous
                    : Init::NotStarted);
      EXPECT_FALSE(C.projected_loader); // No application snapshot was supplied.
    }
    EXPECT_EQ(Supplied, 3u);
    EXPECT_EQ(Materialized, 3u);
    After.JavaState.reset();
    EXPECT_EQ(processResultJSON(After), processResultJSON(Before));
  }
#endif
}
TEST_P(AndroidJNI, ApplicationUnknownStateAndPackageDoNotBecomeNullOrDefaults) {
  auto Unknown = run("jni_application_snapshot");
  EXPECT_EQ(Unknown.Stop, ProcessStopReason::UnsupportedService);
  EXPECT_NE(Unknown.Diagnostic.find("lifecycle snapshot"), std::string::npos);
  Options.Android->Application = mobile::dalvik::AndroidApplicationSnapshot{
      mobile::dalvik::AndroidApplicationSnapshot::Phase::Application, {}};
  auto Package = run("jni_application_package");
  EXPECT_EQ(Package.Stop, ProcessStopReason::UnsupportedService);
  EXPECT_NE(Package.Diagnostic.find("package name"), std::string::npos);
  Options.Android->Application->package_name = "test.app";
  auto Known = run("jni_application_package");
  EXPECT_EQ(Known.Stop, ProcessStopReason::Returned) << Known.Diagnostic;
  EXPECT_EQ(Known.ReturnValue, 0u);
}
TEST_P(AndroidJNI, ApplicationLoaderIsIndependentOfApplicationCreation) {
#ifndef NEVERD_ANDROID_DEX_FIXTURE_DIR
  GTEST_SKIP() << "Python DEX fixture generator unavailable";
#else
  auto File = llvm::MemoryBuffer::getFile(
      (std::filesystem::path(NEVERD_ANDROID_DEX_FIXTURE_DIR) /
       "registration.dex")
          .string());
  ASSERT_TRUE(bool(File));
  auto Bytes = (*File)->getBuffer();
  Options.Android->DexImages.emplace_back(Bytes.begin(), Bytes.end());
  using Snapshot = mobile::dalvik::AndroidApplicationSnapshot;
  Options.Android->Application = Snapshot{Snapshot::Phase::Attached};
  Options.Android->ObserveJavaState = true;
  for (auto Established :
       {std::optional<bool>{}, std::optional{false}, std::optional{true}}) {
    Options.Android->Application->application_loader_established = Established;
    auto R = run("jni_loader_before_application");
    ASSERT_TRUE(R.JavaState);
    EXPECT_EQ(R.JavaState->framework.application_loader_established,
              Established);
    EXPECT_EQ(R.JavaState->framework.application, 0u);
    EXPECT_EQ(R.Stop, Established == true
                          ? ProcessStopReason::Returned
                          : ProcessStopReason::UnsupportedService)
        << R.Diagnostic;
    if (Established == true) {
      EXPECT_EQ(R.ReturnValue, 0u);
      EXPECT_NE(R.JavaState->framework.application_loader, 0u);
      EXPECT_NE(R.JavaState->framework.boot_loader, 0u);
    }
  }
#endif
}
TEST_P(AndroidJNI, ApplicationInputRequiresJNIAndValidPhaseMetadata) {
  Options.Android->EntrySymbol = "jni_application_snapshot";
  Options.Android->Application.emplace();
  Options.Android->JNI = false;
  auto Rejected =
      emulateProcess(Path, ProcessProfile::AndroidNativeAArch64, Options);
  EXPECT_FALSE(bool(Rejected));
  EXPECT_NE(llvm::toString(Rejected.takeError()).find("requires JNI"),
            std::string::npos);
  Options.Android->JNI = true;
  Options.Android->Application->package_name = "test.app";
  auto Invalid =
      emulateProcess(Path, ProcessProfile::AndroidNativeAArch64, Options);
  EXPECT_FALSE(bool(Invalid));
  EXPECT_NE(llvm::toString(Invalid.takeError()).find("package snapshot"),
            std::string::npos);
}
TEST_P(AndroidJNI,
       ApplicationContentResolverRetainsIdentityAcrossThreadsAndDetach) {
  using Snapshot = mobile::dalvik::AndroidApplicationSnapshot;
  Options.Android->Application = Snapshot{Snapshot::Phase::Application};
  auto R = run("jni_content_resolver");
  EXPECT_EQ(R.Stop, ProcessStopReason::Returned) << R.Diagnostic;
  EXPECT_EQ(R.ReturnValue, 0u);
  Options.Android->Arguments = {1};
  auto Unknown = run("jni_content_resolver");
  EXPECT_EQ(Unknown.Stop, ProcessStopReason::UnsupportedService);
  EXPECT_NE(Unknown.Diagnostic.find("unmodeled Java method"),
            std::string::npos);
  EXPECT_NE(Unknown.Diagnostic.find("ContentResolver.getType"),
            std::string::npos);
}
TEST_P(AndroidJNI, ApplicationPackageManagerSharesExplicitStateAcrossThreads) {
  using Snapshot = mobile::dalvik::AndroidApplicationSnapshot;
  Options.Android->Application = Snapshot{Snapshot::Phase::Application};
  auto Unknown = run("jni_package_manager");
  EXPECT_EQ(Unknown.Stop, ProcessStopReason::UnsupportedService);
  EXPECT_NE(Unknown.Diagnostic.find("availability is not supplied"),
            std::string::npos);
  Options.Android->Arguments = {3};
  auto NullReceiver = run("jni_package_manager");
  EXPECT_EQ(NullReceiver.Stop, ProcessStopReason::RuntimeFailure)
      << NullReceiver.Diagnostic;
  EXPECT_FALSE(NullReceiver.ReturnValue);
  EXPECT_NE(NullReceiver.Diagnostic.find("requires a nonnull receiver/class"),
            std::string::npos);
  Options.Android->Application->package_manager_available = true;
  Options.Android->Arguments = {0};
  auto Present = run("jni_package_manager");
  EXPECT_EQ(Present.Stop, ProcessStopReason::Returned) << Present.Diagnostic;
  EXPECT_EQ(Present.ReturnValue, 0u);
  Options.Android->Arguments = {1};
  auto Query = run("jni_package_manager");
  EXPECT_EQ(Query.Stop, ProcessStopReason::UnsupportedService);
  EXPECT_NE(Query.Diagnostic.find("package info query is not supplied"),
            std::string::npos);
  Options.Android->Arguments = {4};
  auto Foreign = run("jni_package_manager");
  EXPECT_EQ(Foreign.Stop, ProcessStopReason::UnsupportedService);
  EXPECT_NE(Foreign.Diagnostic.find("unmodeled Android Context instance"),
            std::string::npos);
  Options.Android->Application->package_manager_available = false;
  Options.Android->Arguments = {2};
  auto Absent = run("jni_package_manager");
  EXPECT_EQ(Absent.Stop, ProcessStopReason::Returned) << Absent.Diagnostic;
  EXPECT_EQ(Absent.ReturnValue, 0u);
}
TEST_P(AndroidJNI, NamedUTF8ArraysPreserveGuardsAndReferenceLifetimes) {
  auto Name = Path.filename().string();
  ASSERT_EQ(Name.find("jni-"), 0u);
  Path = Path.parent_path() / ("string_getbytes-" + Name.substr(4));
  constexpr uint64_t Buffer = 0x20000000;
  Options.Android->Memory = {{Buffer, 4096, std::vector<uint8_t>(4096, 0xa5)}};
  Options.Android->ReadMemory = {{Buffer, 4096}};
  Options.Android->ObserveMemoryOnFailure = true;
  for (unsigned Mode = 0; Mode != 3; ++Mode) {
    SCOPED_TRACE(Mode);
    Options.Android->Arguments = {Buffer, Mode};
    auto Result = run("string_getbytes");
    ASSERT_EQ(Result.Stop, Mode == 2 ? ProcessStopReason::UnsupportedService
                                     : ProcessStopReason::Returned)
        << Result.Diagnostic;
    if (Mode == 2) {
      EXPECT_FALSE(Result.ReturnValue);
      EXPECT_NE(Result.Diagnostic.find("charset is unmodeled"),
                std::string::npos);
    } else {
      EXPECT_EQ(Result.ReturnValue, 0u);
    }
    std::vector<uint8_t> Expected(4096, 0xa5);
    if (Mode == 0)
      for (unsigned I = 0; I != 6; ++I)
        llvm::support::endian::write64le(Expected.data() + I * 8, 1);
    if (Mode == 1)
      llvm::support::endian::write64le(Expected.data(), 17);
    ASSERT_EQ(Result.MemorySnapshots.size(), 1u);
    EXPECT_EQ(Result.MemorySnapshots[0].Bytes, Expected);
  }
}

TEST_P(AndroidJNI, URLValuesPreserveTextAndOfflineBoundaries) {
  for (unsigned mode : {0u, 4u, 5u}) {
    SCOPED_TRACE(mode);
    Options.Android->Arguments = {mode};
    auto r = run("jni_url_values");
    EXPECT_EQ(r.Stop, ProcessStopReason::Returned) << r.Diagnostic;
    EXPECT_EQ(r.ReturnValue, 0u);
  }
  for (unsigned mode : {1u, 2u, 3u}) {
    SCOPED_TRACE(mode);
    Options.Android->Arguments = {mode};
    auto r = run("jni_url_values");
    EXPECT_EQ(r.Stop, ProcessStopReason::UnsupportedService) << r.Diagnostic;
    EXPECT_FALSE(r.ReturnValue);
    const char *reason = mode == 1   ? "bounded HTTP value contract"
                         : mode == 2 ? "URL.openConnection"
                                     : "handler-dependent equality";
    EXPECT_NE(r.Diagnostic.find(reason), std::string::npos);
  }
  Options.Android->Arguments = {6};
  auto r = run("jni_url_values");
  EXPECT_EQ(r.Stop, ProcessStopReason::RuntimeFailure);
  EXPECT_NE(
      r.Diagnostic.find("nonvirtual method ID does not match class lookup"),
      std::string::npos);
}

TEST_P(AndroidJNI, URLHandlerRetainsCheckedExceptionsAcrossOriginalABIRoutes) {
  using Snapshot = mobile::dalvik::AndroidApplicationSnapshot;
  for (unsigned mode = 0; mode != 3; ++mode) {
    std::optional<std::string> message;
    if (mode)
      message = mode == 1 ? "" : std::string("local\0", 6) + "离线";
    Options.Android->Application = Snapshot{};
    Options.Android->Application->url_open_failure_handlers = {
        {"http", message}, {"https", message}};
    for (unsigned form = 0; form != 3; ++form) {
      SCOPED_TRACE(std::to_string(mode) + "/" + std::to_string(form));
      Options.Android->Arguments = {mode, form};
      auto result = run("jni_url_handler");
      EXPECT_EQ(result.Stop, ProcessStopReason::Returned) << result.Diagnostic;
      EXPECT_EQ(result.ReturnValue, 0u);
    }
  }
  Options.Android->Application->url_open_failure_handlers = {{"http", {}}};
  Options.Android->Arguments = {0, 1};
  auto missing = run("jni_url_handler");
  EXPECT_EQ(missing.Stop, ProcessStopReason::UnsupportedService);
  EXPECT_NE(missing.Diagnostic.find("handler state is not supplied"),
            std::string::npos);
}

TEST_P(AndroidJNI, MutableStringBuilderSharesUTF16ValuesWithJNI) {
  auto R = run("jni_string_builder");
  EXPECT_EQ(R.Stop, ProcessStopReason::Returned) << R.Diagnostic;
  EXPECT_EQ(R.ReturnValue, 0u);
  Options.Android->Arguments = {1};
  auto Unknown = run("jni_string_builder");
  EXPECT_EQ(Unknown.Stop, ProcessStopReason::UnsupportedService);
  EXPECT_NE(Unknown.Diagnostic.find("StringBuilder.capacity"),
            std::string::npos);
}
TEST_P(AndroidJNI, PackageResourcePathRetainsContextValueAcrossThreads) {
  using Snapshot = mobile::dalvik::AndroidApplicationSnapshot;
  Options.Android->Application = Snapshot{Snapshot::Phase::Application};
  auto &Input = *Options.Android->Application;
  Input.application_info = Snapshot::ApplicationInfo{"/mutable-source.apk"};
  auto Unknown = run("jni_package_resource_path");
  EXPECT_EQ(Unknown.Stop, ProcessStopReason::UnsupportedService);
  EXPECT_NE(Unknown.Diagnostic.find("package resource path is not supplied"),
            std::string::npos);
  Input.package_resource_path =
      std::optional<std::string>{"/public//fixture.apk"};
  auto Value = run("jni_package_resource_path");
  EXPECT_EQ(Value.Stop, ProcessStopReason::Returned) << Value.Diagnostic;
  EXPECT_EQ(Value.ReturnValue, 0u);
  Input.package_resource_path = std::optional<std::string>{};
  Options.Android->Arguments = {1};
  auto Null = run("jni_package_resource_path");
  EXPECT_EQ(Null.Stop, ProcessStopReason::Returned) << Null.Diagnostic;
  EXPECT_EQ(Null.ReturnValue, 0u);
  Options.Android->Arguments = {2};
  auto Foreign = run("jni_package_resource_path");
  EXPECT_EQ(Foreign.Stop, ProcessStopReason::UnsupportedService);
  EXPECT_NE(Foreign.Diagnostic.find("unmodeled Android Context instance"),
            std::string::npos);
  Options.Android->Arguments = {3};
  auto Receiver = run("jni_package_resource_path");
  EXPECT_EQ(Receiver.Stop, ProcessStopReason::RuntimeFailure);
}
TEST_P(AndroidJNI, ApplicationLabelSharesMutableCharSequenceAcrossThreads) {
  using Snapshot = mobile::dalvik::AndroidApplicationSnapshot;
  Options.Android->Application = Snapshot{Snapshot::Phase::Application};
  auto &Input = *Options.Android->Application;
  Input.package_manager_available = true;
  Input.application_info.emplace();
  auto UnknownMode = run("jni_application_label");
  EXPECT_EQ(UnknownMode.Stop, ProcessStopReason::UnsupportedService);
  EXPECT_NE(
      UnknownMode.Diagnostic.find("force-safe-labels state is not supplied"),
      std::string::npos);
  Input.package_item_force_safe_labels = true;
  auto Safe = run("jni_application_label");
  EXPECT_EQ(Safe.Stop, ProcessStopReason::UnsupportedService);
  EXPECT_NE(Safe.Diagnostic.find("safe application labels are not modeled"),
            std::string::npos);
  Input.package_item_force_safe_labels = false;
  auto UnknownField = run("jni_application_label");
  EXPECT_EQ(UnknownField.Stop, ProcessStopReason::UnsupportedService);
  EXPECT_NE(UnknownField.Diagnostic.find("nonLocalizedLabel is not supplied"),
            std::string::npos);
  Input.application_info->non_localized_label =
      std::optional<std::string>{"  fixture  "};
  auto Known = run("jni_application_label");
  EXPECT_EQ(Known.Stop, ProcessStopReason::Returned) << Known.Diagnostic;
  EXPECT_EQ(Known.ReturnValue, 0u);
  Input.package_item_force_safe_labels.reset();
  Options.Android->Arguments = {1};
  auto Null = run("jni_application_label");
  EXPECT_EQ(Null.Stop, ProcessStopReason::Returned) << Null.Diagnostic;
  EXPECT_EQ(Null.ReturnValue, 0u);
  Input.package_item_force_safe_labels = false;
  Options.Android->Arguments = {2};
  auto Foreign = run("jni_application_label");
  EXPECT_EQ(Foreign.Stop, ProcessStopReason::UnsupportedService);
  EXPECT_NE(
      Foreign.Diagnostic.find("unmodeled Android ApplicationInfo instance"),
      std::string::npos);
  Input.application_info->non_localized_label = std::optional<std::string>{};
  Options.Android->Arguments = {0};
  auto Fallback = run("jni_application_label");
  EXPECT_EQ(Fallback.Stop, ProcessStopReason::UnsupportedService);
  EXPECT_NE(Fallback.Diagnostic.find("label fallback is not supplied"),
            std::string::npos);
}
TEST_P(AndroidJNI,
       PackageApplicationInfoResponsesHaveIndependentMutableFields) {
  using Snapshot = mobile::dalvik::AndroidApplicationSnapshot;
  Options.Android->Application = Snapshot{Snapshot::Phase::Application};
  auto &Input = *Options.Android->Application;
  Input.package_manager_available = true;
  Input.application_info = Snapshot::ApplicationInfo{"/context.apk"};
  auto Unknown = run("jni_package_application_info");
  EXPECT_EQ(Unknown.Stop, ProcessStopReason::UnsupportedService);
  EXPECT_NE(Unknown.Diagnostic.find("query is not supplied"),
            std::string::npos);
  Input.application_info_queries = {
      {"test.app", 0, {"/query.apk", std::optional<std::string>{}}}};
  auto Known = run("jni_package_application_info");
  EXPECT_EQ(Known.Stop, ProcessStopReason::Returned) << Known.Diagnostic;
  EXPECT_EQ(Known.ReturnValue, 0u);
  for (unsigned Mode : {1u, 3u, 4u}) {
    Options.Android->Arguments = {Mode};
    auto Refused = run("jni_package_application_info");
    EXPECT_EQ(Refused.Stop, ProcessStopReason::UnsupportedService)
        << Refused.Diagnostic;
  }
  Input.application_info_queries[0].info.native_library_dir.reset();
  Options.Android->Arguments = {2};
  auto Field = run("jni_package_application_info");
  EXPECT_EQ(Field.Stop, ProcessStopReason::UnsupportedService);
  EXPECT_NE(Field.Diagnostic.find("nativeLibraryDir is not supplied"),
            std::string::npos);
}
TEST_P(AndroidJNI, HandlerPostSharesQueueClockAndThreadLifetime) {
  using Snapshot = mobile::dalvik::AndroidApplicationSnapshot;
  Options.Android->Application = Snapshot{};
  auto &input = *Options.Android->Application;
  input.main_looper_prepared = true;
  input.initial_main_queue.emplace(Snapshot::EmptyMessageQueue{});
  Options.Android->ObserveJavaState = true;
  auto unknown = run("jni_handler_post");
  EXPECT_EQ(unknown.Stop, ProcessStopReason::UnsupportedService);
  EXPECT_EQ(unknown.Diagnostic, "Android Message pool state is not supplied");
  input.initial_message_pool_empty = true;
  auto missing_clock = run("jni_handler_post");
  EXPECT_EQ(missing_clock.Stop, ProcessStopReason::UnsupportedService);
  EXPECT_NE(missing_clock.Diagnostic.find("no explicit linux_time input"),
            std::string::npos);
  Options.LinuxTime.emplace();
  Options.LinuxTime->Clocks = {{1, {10, 123456789}}};
  Options.LinuxTime->AdvanceOnIdle = true;
  auto returned = run("jni_handler_post");
  ASSERT_EQ(returned.Stop, ProcessStopReason::Returned) << returned.Diagnostic;
  EXPECT_EQ(returned.ReturnValue, 0u);
  ASSERT_TRUE(returned.JavaState);
  ASSERT_TRUE(returned.JavaState->framework.main_message_queue);
  const auto &queue = *returned.JavaState->framework.main_message_queue;
  EXPECT_TRUE(queue.complete);
  EXPECT_FALSE(queue.blocked);
  ASSERT_EQ(queue.messages.size(), 3u);
  for (unsigned i = 0; i != 3; ++i) {
    EXPECT_EQ(queue.messages[i].when, i ? 10125 : 10123);
    EXPECT_EQ(queue.messages[i].callback, 0u);
    EXPECT_EQ(queue.messages[i].flags, 1u);
    EXPECT_EQ(queue.messages[i].target, queue.messages[0].target);
    EXPECT_EQ(queue.messages[i].next,
              i == 2 ? 0u : queue.messages[i + 1].identity);
  }
  input.initial_main_queue->blocked = true;
  auto blocked = run("jni_handler_post");
  EXPECT_EQ(blocked.Stop, ProcessStopReason::UnsupportedService);
  EXPECT_EQ(blocked.Diagnostic, "Android MessageQueue.nativeWake is unmodeled");
  ASSERT_TRUE(blocked.JavaState->framework.main_message_queue);
  EXPECT_EQ(blocked.JavaState->framework.main_message_queue->messages.size(),
            1u);
  input.initial_main_queue->blocked = false;
  Options.Android->Arguments = {1};
  auto null_queue = run("jni_handler_post");
  EXPECT_EQ(null_queue.Stop, ProcessStopReason::UnsupportedService);
  EXPECT_EQ(null_queue.Diagnostic,
            "Android Handler null-queue warning requires logging");
  Options.Android->Arguments = {0};
  for (auto seconds : {int64_t(-1), int64_t(9223372037)}) {
    Options.LinuxTime->Clocks.at(1).Seconds = seconds;
    auto out_of_range = run("jni_handler_post");
    EXPECT_EQ(out_of_range.Stop, ProcessStopReason::UnsupportedService);
    EXPECT_EQ(out_of_range.Diagnostic,
              "Android monotonic time exceeds its range");
  }
}
TEST_P(AndroidJNI, MainLooperKeepsNullIdentityAndHandlerFieldsDistinct) {
  using Snapshot = mobile::dalvik::AndroidApplicationSnapshot;
  Options.Android->Application = Snapshot{};
  auto &input = *Options.Android->Application;
  auto unknown = run("jni_main_looper");
  EXPECT_EQ(unknown.Stop, ProcessStopReason::UnsupportedService);
  EXPECT_EQ(unknown.Diagnostic, "Android main Looper state is not supplied");
  input.main_looper_prepared = false;
  Options.Android->Arguments = {1};
  auto absent = run("jni_main_looper");
  EXPECT_EQ(absent.Stop, ProcessStopReason::Returned) << absent.Diagnostic;
  EXPECT_EQ(absent.ReturnValue, 0u);
  input.main_looper_prepared = true;
  for (unsigned mode : {0u, 2u}) {
    Options.Android->Arguments = {mode};
    auto prepared = run("jni_main_looper");
    EXPECT_EQ(prepared.Stop, ProcessStopReason::Returned)
        << prepared.Diagnostic;
    EXPECT_EQ(prepared.ReturnValue, 0u);
  }
}
TEST_P(AndroidJNI, ClassLoaderPathsKeepApplicationAndSystemStateSeparate) {
  using Snapshot = mobile::dalvik::AndroidApplicationSnapshot;
  Options.Android->Application = Snapshot{Snapshot::Phase::Application};
  auto &input = *Options.Android->Application;
  auto unknown = run("jni_classloader_paths");
  EXPECT_EQ(unknown.Stop, ProcessStopReason::UnsupportedService);
  EXPECT_NE(
      unknown.Diagnostic.find("application loader paths are not supplied"),
      std::string::npos);
  input.system_loader_paths =
      Snapshot::ClassLoaderPaths{{}, {}, {"/system/lib"}};
  auto system_only = run("jni_classloader_paths");
  EXPECT_EQ(system_only.Stop, ProcessStopReason::UnsupportedService);
  EXPECT_NE(
      system_only.Diagnostic.find("application loader paths are not supplied"),
      std::string::npos);
  input.application_loader_paths =
      Snapshot::ClassLoaderPaths{{"/one.apk"}, {"/app/lib"}, {"/system/lib"}};
  auto known = run("jni_classloader_paths");
  EXPECT_EQ(known.Stop, ProcessStopReason::Returned) << known.Diagnostic;
  EXPECT_EQ(known.ReturnValue, 0u);
  Options.Android->Arguments = {1};
  auto foreign = run("jni_classloader_paths");
  EXPECT_EQ(foreign.Stop, ProcessStopReason::UnsupportedService);
  EXPECT_NE(
      foreign.Diagnostic.find("unmodeled Android PathClassLoader instance"),
      std::string::npos);
  Options.Android->Arguments = {2};
  input.system_loader_paths.reset();
  auto app_only = run("jni_classloader_paths");
  EXPECT_EQ(app_only.Stop, ProcessStopReason::UnsupportedService);
  EXPECT_NE(app_only.Diagnostic.find("system loader paths are not supplied"),
            std::string::npos);
}
TEST_P(AndroidJNI, DefaultLocaleRequiresExplicitStateAndRetainsThreadIdentity) {
  using Snapshot = mobile::dalvik::AndroidApplicationSnapshot;
  Options.Android->Application = Snapshot{};
  auto &input = *Options.Android->Application;
  auto unknown = run("jni_default_locale");
  EXPECT_EQ(unknown.Stop, ProcessStopReason::UnsupportedService);
  EXPECT_NE(unknown.Diagnostic.find("default Locale is not supplied"),
            std::string::npos);
  input.default_locale.emplace();
  auto component = run("jni_default_locale");
  EXPECT_EQ(component.Stop, ProcessStopReason::UnsupportedService);
  EXPECT_NE(component.Diagnostic.find("Locale language is not supplied"),
            std::string::npos);
  input.default_locale = Snapshot::Locale{"en", "US"};
  auto known = run("jni_default_locale");
  EXPECT_EQ(known.Stop, ProcessStopReason::Returned) << known.Diagnostic;
  EXPECT_EQ(known.ReturnValue, 0u);
  for (unsigned mode : {2u, 3u}) {
    Options.Android->Arguments = {mode};
    auto refused = run("jni_default_locale");
    EXPECT_EQ(refused.Stop, ProcessStopReason::UnsupportedService)
        << refused.Diagnostic;
  }
  Options.Android->Arguments = {1};
  input.default_locale = Snapshot::Locale{"", ""};
  auto empty = run("jni_default_locale");
  EXPECT_EQ(empty.Stop, ProcessStopReason::Returned) << empty.Diagnostic;
  EXPECT_EQ(empty.ReturnValue, 0u);
}
TEST_P(AndroidJNI, TargetSdkVersionRetainsContextAndQueryFieldState) {
  using Snapshot = mobile::dalvik::AndroidApplicationSnapshot;
  Options.Android->Application = Snapshot{Snapshot::Phase::Application};
  auto &input = *Options.Android->Application;
  auto unknown = run("jni_target_sdk");
  EXPECT_EQ(unknown.Stop, ProcessStopReason::UnsupportedService);
  EXPECT_NE(unknown.Diagnostic.find("targetSdkVersion is not supplied"),
            std::string::npos);
  input.package_manager_available = true;
  input.application_info.emplace().target_sdk_version = 28;
  Snapshot::ApplicationInfo query;
  query.target_sdk_version = 29;
  input.application_info_queries = {{"test.app", 0, query}};
  auto known = run("jni_target_sdk");
  EXPECT_EQ(known.Stop, ProcessStopReason::Returned) << known.Diagnostic;
  EXPECT_EQ(known.ReturnValue, 0u);
  Options.Android->Arguments = {1};
  input.application_info->target_sdk_version = 0;
  auto zero = run("jni_target_sdk");
  EXPECT_EQ(zero.Stop, ProcessStopReason::Returned) << zero.Diagnostic;
  EXPECT_EQ(zero.ReturnValue, 0u);
  Options.Android->Arguments = {2};
  auto foreign = run("jni_target_sdk");
  EXPECT_EQ(foreign.Stop, ProcessStopReason::UnsupportedService);
  EXPECT_NE(
      foreign.Diagnostic.find("unmodeled Android ApplicationInfo instance"),
      std::string::npos);
  Options.Android->Arguments = {3};
  input.application_info->target_sdk_version = 28;
  input.application_info_queries[0].info.target_sdk_version.reset();
  auto query_unknown = run("jni_target_sdk");
  EXPECT_EQ(query_unknown.Stop, ProcessStopReason::UnsupportedService);
  EXPECT_NE(query_unknown.Diagnostic.find("targetSdkVersion is not supplied"),
            std::string::npos);
}
TEST_P(AndroidJNI, PackageInfoQueriesRetainFieldsAcrossNativeThreads) {
  using Snapshot = mobile::dalvik::AndroidApplicationSnapshot;
  Options.Android->Application = Snapshot{Snapshot::Phase::Application};
  auto &input = *Options.Android->Application;
  input.package_manager_available = true;
  auto unknown = run("jni_package_info");
  EXPECT_EQ(unknown.Stop, ProcessStopReason::UnsupportedService);
  EXPECT_NE(unknown.Diagnostic.find("package info query is not supplied"),
            std::string::npos);
  Snapshot::PackageInfo info;
  info.package_name = std::optional<std::string>{"result.app"};
  info.version_name = std::optional<std::string>{"fixture-version"};
  info.version_code = 7;
  input.package_info_queries = {{"test.app", 0, info}};
  auto known = run("jni_package_info");
  EXPECT_EQ(known.Stop, ProcessStopReason::Returned) << known.Diagnostic;
  EXPECT_EQ(known.ReturnValue, 0u);
  for (unsigned mode : {1u, 2u, 3u, 6u}) {
    Options.Android->Arguments = {mode};
    auto refused = run("jni_package_info");
    EXPECT_EQ(refused.Stop, ProcessStopReason::UnsupportedService)
        << mode << ": " << refused.Diagnostic;
  }
  Options.Android->Arguments = {5};
  input.package_info_queries[0].info.version_name =
      std::optional<std::string>{};
  auto null = run("jni_package_info");
  EXPECT_EQ(null.Stop, ProcessStopReason::Returned) << null.Diagnostic;
  EXPECT_EQ(null.ReturnValue, 0u);
  input.package_info_queries[0].info.version_name.reset();
  auto field = run("jni_package_info");
  EXPECT_EQ(field.Stop, ProcessStopReason::UnsupportedService);
  EXPECT_NE(field.Diagnostic.find("versionName is not supplied"),
            std::string::npos);
}
TEST_P(AndroidJNI, PackageInfoApplicationSharesReferencesAcrossThreads) {
  using Snapshot = mobile::dalvik::AndroidApplicationSnapshot;
  Options.Android->Arguments = {1};
  auto declaration = run("jni_package_application");
  EXPECT_EQ(declaration.Stop, ProcessStopReason::Returned)
      << declaration.Diagnostic;
  EXPECT_EQ(declaration.ReturnValue, 0u);
  Options.Android->Arguments.clear();
  Options.Android->Application = Snapshot{Snapshot::Phase::Application};
  auto &input = *Options.Android->Application;
  input.package_manager_available = true;
  input.application_info =
      Snapshot::ApplicationInfo{std::string("context.apk")};
  input.package_info_queries = {{"test.app", 0, {}}};
  auto unknown = run("jni_package_application");
  EXPECT_EQ(unknown.Stop, ProcessStopReason::UnsupportedService);
  EXPECT_NE(unknown.Diagnostic.find("applicationInfo is not supplied"),
            std::string::npos);
  input.package_info_queries[0].info.application_info =
      Snapshot::ApplicationInfo{std::string("parcel.apk")};
  auto known = run("jni_package_application");
  EXPECT_EQ(known.Stop, ProcessStopReason::Returned) << known.Diagnostic;
  EXPECT_EQ(known.ReturnValue, 0u);
  Options.Android->Arguments = {3};
  auto foreign = run("jni_package_application");
  EXPECT_EQ(foreign.Stop, ProcessStopReason::UnsupportedService);
  EXPECT_NE(foreign.Diagnostic.find("unmodeled Android PackageInfo instance"),
            std::string::npos);
  Options.Android->Arguments = {2};
  input.package_info_queries[0].info.application_info.emplace();
  auto null = run("jni_package_application");
  EXPECT_EQ(null.Stop, ProcessStopReason::Returned) << null.Diagnostic;
  EXPECT_EQ(null.ReturnValue, 0u);
  Options.Android->Arguments = {4};
  input.package_info_queries[0].info.application_info.reset();
  auto written = run("jni_package_application");
  EXPECT_EQ(written.Stop, ProcessStopReason::Returned) << written.Diagnostic;
  EXPECT_EQ(written.ReturnValue, 0u);
}
TEST_P(AndroidJNI, SecureSettingsCachePreservesUnknownNullAndStringValues) {
  using Snapshot = mobile::dalvik::AndroidApplicationSnapshot;
  Options.Android->Application = Snapshot{Snapshot::Phase::Application};
  auto Unknown = run("jni_secure_settings");
  EXPECT_EQ(Unknown.Stop, ProcessStopReason::UnsupportedService);
  EXPECT_NE(Unknown.Diagnostic.find("cache is not supplied"),
            std::string::npos);
  Options.Android->Application->secure_settings_cache =
      Snapshot::SecureSettingsCache{"fixture"};
  auto Value = run("jni_secure_settings");
  EXPECT_EQ(Value.Stop, ProcessStopReason::Returned) << Value.Diagnostic;
  EXPECT_EQ(Value.ReturnValue, 0u);
  Options.Android->Arguments = {2};
  auto Key = run("jni_secure_settings");
  EXPECT_EQ(Key.Stop, ProcessStopReason::UnsupportedService);
  EXPECT_NE(Key.Diagnostic.find("unmodeled Settings.Secure key"),
            std::string::npos);
  Options.Android->Arguments = {3};
  auto NullResolver = run("jni_secure_settings");
  EXPECT_EQ(NullResolver.Stop, ProcessStopReason::Returned)
      << NullResolver.Diagnostic;
  EXPECT_EQ(NullResolver.ReturnValue, 0u);
  Options.Android->Application->secure_settings_cache =
      Snapshot::SecureSettingsCache{};
  Options.Android->Arguments = {1};
  auto NullValue = run("jni_secure_settings");
  EXPECT_EQ(NullValue.Stop, ProcessStopReason::Returned)
      << NullValue.Diagnostic;
  EXPECT_EQ(NullValue.ReturnValue, 0u);
}
TEST_P(AndroidJNI, SecureKeyConstantsUseInternedStaticFieldStorage) {
  auto Known = run("jni_secure_key_constants");
  EXPECT_EQ(Known.Stop, ProcessStopReason::Returned) << Known.Diagnostic;
  EXPECT_EQ(Known.ReturnValue, 0u);
  Options.Android->Arguments = {1};
  auto Unknown = run("jni_secure_key_constants");
  EXPECT_EQ(Unknown.Stop, ProcessStopReason::UnsupportedService);
  EXPECT_NE(Unknown.Diagnostic.find("field declaration not supplied"),
            std::string::npos);
}
TEST_P(AndroidJNI, SecureSettingsIntegersUseGlobalCacheAndCallerDefault) {
  using Snapshot = mobile::dalvik::AndroidApplicationSnapshot;
  Options.Android->Application = Snapshot{Snapshot::Phase::Application};
  Options.Android->Application->secure_settings_cache =
      Snapshot::SecureSettingsCache{"99"};
  auto Unknown = run("jni_settings_int");
  EXPECT_EQ(Unknown.Stop, ProcessStopReason::UnsupportedService);
  EXPECT_NE(Unknown.Diagnostic.find("global settings cache is not supplied"),
            std::string::npos);
  struct Case {
    std::optional<std::string> text;
    uint32_t value;
  };
  const Case cases[]{{"0", 0},
                     {"1", 1},
                     {"-2147483648", 2147483648u},
                     {"2147483648", uint32_t(-23)},
                     {" 1", uint32_t(-23)},
                     {std::nullopt, uint32_t(-23)}};
  for (const auto &test : cases) {
    Options.Android->Application->global_settings_cache =
        Snapshot::GlobalSettingsCache{test.text};
    Options.Android->Arguments = {test.value, 0};
    auto Value = run("jni_settings_int");
    EXPECT_EQ(Value.Stop, ProcessStopReason::Returned) << Value.Diagnostic;
    EXPECT_EQ(Value.ReturnValue, 0u);
  }
  Options.Android->Arguments = {0, 1};
  auto Key = run("jni_settings_int");
  EXPECT_EQ(Key.Stop, ProcessStopReason::UnsupportedService);
  EXPECT_NE(Key.Diagnostic.find("unmodeled Settings.Secure key"),
            std::string::npos);
  Options.Android->Arguments = {0, 2};
  auto NullResolver = run("jni_settings_int");
  EXPECT_EQ(NullResolver.Stop, ProcessStopReason::Returned)
      << NullResolver.Diagnostic;
  EXPECT_EQ(NullResolver.ReturnValue, 0u);
}
TEST_P(AndroidJNI, ApplicationInfoSharesFieldsAcrossThreadsAndDetach) {
  using Snapshot = mobile::dalvik::AndroidApplicationSnapshot;
  Options.Android->Application = Snapshot{Snapshot::Phase::Application};
  auto Unknown = run("jni_application_info");
  EXPECT_EQ(Unknown.Stop, ProcessStopReason::UnsupportedService);
  EXPECT_NE(Unknown.Diagnostic.find("sourceDir is not supplied"),
            std::string::npos);
  for (unsigned Mode : {2u, 0u, 1u}) {
    Options.Android->Arguments = {Mode};
    if (Mode != 2)
      Options.Android->Application->application_info =
          Snapshot::ApplicationInfo{
              Mode == 0 ? std::optional<std::string>{"/test/base.apk"}
                        : std::nullopt};
    auto Value = run("jni_application_info");
    EXPECT_EQ(Value.Stop, ProcessStopReason::Returned) << Value.Diagnostic;
    EXPECT_EQ(Value.ReturnValue, 0u);
  }
  Options.Android->Application->application_info =
      Snapshot::ApplicationInfo{"/test/base.apk"};
  Options.Android->Arguments = {3};
  auto Field = run("jni_application_info");
  EXPECT_EQ(Field.Stop, ProcessStopReason::UnsupportedService);
  EXPECT_NE(Field.Diagnostic.find("field declaration not supplied"),
            std::string::npos);
}
TEST_P(AndroidJNI, ApplicationInfoNativeLibraryDirectoryHasIndependentState) {
  using Snapshot = mobile::dalvik::AndroidApplicationSnapshot;
  Options.Android->Application = Snapshot{Snapshot::Phase::Application};
  Options.Android->Application->application_info =
      Snapshot::ApplicationInfo{"/test/base.apk"};
  Options.Android->Arguments = {0, 1};
  auto Unknown = run("jni_application_info");
  EXPECT_EQ(Unknown.Stop, ProcessStopReason::UnsupportedService);
  EXPECT_NE(Unknown.Diagnostic.find("nativeLibraryDir is not supplied"),
            std::string::npos);
  for (unsigned Mode : {2u, 0u, 1u}) {
    Options.Android->Arguments = {Mode, 1};
    if (Mode != 2)
      Options.Android->Application->application_info->native_library_dir
          .emplace(Mode == 0 ? std::optional<std::string>{"/test/lib/arm64"}
                             : std::nullopt);
    auto Value = run("jni_application_info");
    EXPECT_EQ(Value.Stop, ProcessStopReason::Returned) << Value.Diagnostic;
    EXPECT_EQ(Value.ReturnValue, 0u);
  }
}
TEST_P(AndroidJNI,
       ApplicationFilesUseDeclaredDirectoryAndRetainedFileIdentity) {
  Options.Android->Application = mobile::dalvik::AndroidApplicationSnapshot{
      mobile::dalvik::AndroidApplicationSnapshot::Phase::Application,
      {},
      "/fixture/files"};
  Options.LinuxFiles.emplace();
  Options.LinuxFiles->Directories = {"/fixture/files"};
  auto R = run("jni_application_files");
  EXPECT_EQ(R.Stop, ProcessStopReason::Returned) << R.Diagnostic;
  EXPECT_EQ(R.ReturnValue, 0u);
  Options.LinuxFiles->Directories.clear();
  auto Invalid =
      emulateProcess(Path, ProcessProfile::AndroidNativeAArch64, Options);
  EXPECT_FALSE(bool(Invalid));
  EXPECT_NE(
      llvm::toString(Invalid.takeError()).find("declared Linux directory"),
      std::string::npos);
  Options.Android->Application->files_directory.reset();
  auto Unknown = run("jni_application_files");
  EXPECT_EQ(Unknown.Stop, ProcessStopReason::UnsupportedService);
  EXPECT_NE(Unknown.Diagnostic.find("files directory is not supplied"),
            std::string::npos);
}
TEST_P(AndroidJNI, BuildVersionFieldsUseTheNativePropertySnapshot) {
  Options.Android->Properties = {{"ro.build.version.release", "9.2"},
                                 {"ro.build.version.incremental", "9.2"},
                                 {"ro.build.version.sdk", "0x1c"}};
  Options.Android->Arguments = {28, 1};
  auto R = run("jni_build_version");
  EXPECT_EQ(R.Stop, ProcessStopReason::Returned) << R.Diagnostic;
  EXPECT_EQ(R.ReturnValue, 0u);
}
TEST_P(AndroidJNI, BuildVersionMissingAndEmptyPropertiesKeepPlatformDefaults) {
  Options.Android->Properties = {{"ro.build.version.release", ""}};
  Options.Android->Arguments = {0, 0};
  auto R = run("jni_build_version");
  EXPECT_EQ(R.Stop, ProcessStopReason::Returned) << R.Diagnostic;
  EXPECT_EQ(R.ReturnValue, 0u);
}
TEST_P(AndroidJNI, BuildVersionRejectsWrongTypesWritesAndUnknownFields) {
  for (const char *Entry :
       {"jni_build_version_wrong_accessor", "jni_build_version_write",
        "jni_build_version_unknown_field"}) {
    auto R = run(Entry);
    EXPECT_EQ(R.Stop, std::string_view(Entry).ends_with("unknown_field")
                          ? ProcessStopReason::UnsupportedService
                          : ProcessStopReason::RuntimeFailure)
        << Entry << R.Diagnostic;
    ASSERT_FALSE(R.NativeCalls.empty());
    EXPECT_FALSE(R.NativeCalls.back().Result.has_value());
  }
}
TEST_P(AndroidJNI, DexClassCapacityIsSeparateAndAggregate) {
#ifndef NEVERD_ANDROID_DEX_FIXTURE_DIR
  GTEST_SKIP() << "Python DEX fixture generator unavailable";
#else
  Options.MemoryLimit = 128 * 1024 * 1024;
  auto image = [](const char *Name) {
    auto File = llvm::MemoryBuffer::getFile(
        (std::filesystem::path(NEVERD_ANDROID_DEX_FIXTURE_DIR) / Name)
            .string());
    if (!File) {
      ADD_FAILURE() << File.getError().message();
      return std::vector<uint8_t>{};
    }
    auto Bytes = (*File)->getBuffer();
    return std::vector<uint8_t>(Bytes.begin(), Bytes.end());
  };
  auto Small = image("classes-17-0.dex");
  auto Half = image("classes-2048-0.dex");
  auto OtherHalf = image("classes-2048-1.dex");
  auto Full = image("classes-4096-0.dex");
  auto invoke = [&](std::vector<std::vector<uint8_t>> Images, const char *Last,
                    const char *Error = nullptr) {
    Options.Android->DexImages = std::move(Images);
    Options.Android->EntrySymbol = "jni_dex_class_pair";
    NativeMemoryRegion Region;
    Region.Address = 0x20000000;
    Region.Size = 4096;
    Region.Bytes.resize(128);
    const std::string First = "bench/d000/C000000";
    std::copy(First.begin(), First.end(), Region.Bytes.begin());
    const std::string LastName = Last;
    std::copy(LastName.begin(), LastName.end(), Region.Bytes.begin() + 64);
    Options.Android->Memory = {std::move(Region)};
    Options.Android->Arguments = {0x20000000, 0x20000040};
    auto R =
        emulateProcess(Path, ProcessProfile::AndroidNativeAArch64, Options);
    if (Error) {
      ASSERT_FALSE(bool(R));
      EXPECT_NE(llvm::toString(R.takeError()).find(Error), std::string::npos);
      return;
    }
    ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
    EXPECT_EQ(R->Stop, ProcessStopReason::Returned) << R->Diagnostic;
    EXPECT_EQ(R->ReturnValue, 0u);
  };
  invoke({Small}, "bench/d000/C000016");
  invoke({Full}, "other/d000/C004095");
  invoke({Half, OtherHalf}, "other/d001/C002047");
  invoke({image("classes-4097-0.dex")}, "", "class inventory");
  invoke({Half, image("classes-2049-1.dex")}, "", "class inventory");
  invoke({Full, OtherHalf}, "", "class inventory");
  invoke({Small, Small}, "", "duplicate supplied DEX class");
  invoke(std::vector<std::vector<uint8_t>>(17, Small), "", "at most 16");
  Small.back() ^= 1;
  invoke({Small}, "", "Invalid DEX");
#endif
}
TEST_P(AndroidJNI, ServiceManagerCacheAliasesSurviveCallsAndDetach) {
  Options.LinuxTime.emplace().Clocks = {{7, {50, 987654321}}};
  auto &state =
      Options.Android->Application.emplace().service_manager.emplace();
  state.binder_proxies = {{7, true}};
  state.cache = std::vector<
      mobile::dalvik::AndroidApplicationSnapshot::ServiceManager::CacheEntry>{
      {"alpha", 7}, {"beta", 7}};
  Options.Android->ObserveJavaState = true;
  auto result = run("jni_service_manager_cache");
  ASSERT_EQ(result.Stop, ProcessStopReason::Returned) << result.Diagnostic;
  EXPECT_EQ(result.ReturnValue, 0u);
  ASSERT_TRUE(result.JavaState);
  ASSERT_TRUE(result.JavaState->framework.service_manager);
  const auto &snapshot = *result.JavaState->framework.service_manager;
  EXPECT_TRUE(snapshot.initialized);
  EXPECT_TRUE(snapshot.cache_known);
  ASSERT_EQ(snapshot.entries.size(), 2u);
  EXPECT_EQ(snapshot.entries[0].binder, snapshot.entries[1].binder);
  ASSERT_EQ(snapshot.binder_proxies.size(), 1u);
  EXPECT_EQ(snapshot.binder_proxies[0].warn_on_blocking, false);
  EXPECT_FALSE(snapshot.manager);
}
TEST_P(AndroidJNI,
       ServiceManagerCacheHitsKeepUnknownBinderStateAndFollowMapMutations) {
  Options.LinuxTime.emplace().Clocks = {{7, {50, 0}}};
  Options.Android->ObserveJavaState = true;
  auto &input =
      Options.Android->Application.emplace().service_manager.emplace();
  input.binder_proxies = {{7, std::nullopt}};
  input.cache = std::vector<
      mobile::dalvik::AndroidApplicationSnapshot::ServiceManager::CacheEntry>{
      {"alpha", 7}};
  for (unsigned remove : {0u, 1u}) {
    Options.Android->Arguments = {remove};
    auto result = run("jni_service_manager_lookup");
    if (remove) {
      EXPECT_EQ(result.Stop, ProcessStopReason::UnsupportedService);
      EXPECT_EQ(result.Diagnostic,
                "Android ServiceManager proxy state is not supplied");
    } else {
      EXPECT_EQ(result.Stop, ProcessStopReason::Returned) << result.Diagnostic;
      EXPECT_EQ(result.ReturnValue, 0u);
    }
    ASSERT_TRUE(result.JavaState);
    EXPECT_EQ(result.JavaState->terminal, bool(remove));
    ASSERT_TRUE(result.JavaState->framework.service_manager);
    const auto &state = *result.JavaState->framework.service_manager;
    EXPECT_TRUE(state.initialized);
    EXPECT_TRUE(state.cache_known);
    EXPECT_EQ(state.entries.size(), remove ? 0u : 1u);
    EXPECT_FALSE(state.manager);
    ASSERT_EQ(state.binder_proxies.size(), 1u);
    EXPECT_FALSE(state.binder_proxies[0].warn_on_blocking.has_value());
  }
}
TEST_P(AndroidJNI, ServiceManagerMissingStateAndTransactionsStayExplicit) {
  Options.LinuxTime.emplace().Clocks = {{7, {50, 0}}};
  Options.Android->Arguments = {1};
  Options.Android->ObserveJavaState = true;
  for (unsigned mode = 0; mode < 4; ++mode) {
    auto &state =
        Options.Android->Application.emplace().service_manager.emplace();
    if (mode > 0)
      state.cache.emplace();
    if (mode > 1)
      state.manager_proxy.emplace(std::nullopt);
    if (mode > 2) {
      state.binder_proxies = {{7, true}};
      state.context_object.emplace(7);
    }
    auto result = run("jni_service_manager_cache");
    EXPECT_EQ(result.Stop, ProcessStopReason::UnsupportedService);
    const char *messages[] = {"cache contents", "proxy state", "context object",
                              "Binder transaction"};
    EXPECT_NE(result.Diagnostic.find(messages[mode]), std::string::npos)
        << result.Diagnostic;
    ASSERT_TRUE(result.JavaState);
    ASSERT_TRUE(result.JavaState->framework.service_manager);
    EXPECT_TRUE(result.JavaState->framework.service_manager->initialized);
    EXPECT_TRUE(result.JavaState->terminal);
  }
}
TEST_P(AndroidJNI, ServiceManagerActiveUsesInitializeOnceWithSharedTime) {
  Options.LinuxTime.emplace().Clocks = {{7, {50, 0}}};
  Options.Android->ObserveJavaState = true;
  for (unsigned mode = 1; mode != 5; ++mode) {
    Options.Android->Arguments = {mode};
    auto result = run("jni_service_manager_metadata");
    if (mode == 3) {
      // Initialization completes before lookup reaches the separate,
      // deliberately unsupported Object.toString value implementation.
      EXPECT_EQ(result.Stop, ProcessStopReason::UnsupportedService);
      EXPECT_NE(result.Diagnostic.find("unmodeled Java method"),
                std::string::npos);
    } else {
      EXPECT_EQ(result.Stop, ProcessStopReason::Returned) << result.Diagnostic;
      EXPECT_EQ(result.ReturnValue, 0u);
    }
    ASSERT_TRUE(result.JavaState);
    ASSERT_TRUE(result.JavaState->framework.service_manager);
    EXPECT_TRUE(result.JavaState->framework.service_manager->initialized);
    EXPECT_FALSE(result.JavaState->framework.service_manager->cache_known);
  }
}
TEST_P(AndroidJNI, ServiceManagerLoggerRetainsIndependentBootEpochAcrossIdle) {
  Options.LinuxTime.emplace().Clocks = {
      {1, {10, 123456789}}, {4, {30, 500000000}}, {7, {50, 987654321}}};
  Options.LinuxTime->AdvanceOnIdle = true;
  Options.Android->ObserveJavaState = true;
  for (unsigned advance : {0u, 31u}) {
    Options.LinuxTime->NanosecondsPerInstruction = advance;
    Options.Android->Arguments = {advance};
    auto result = run("jni_service_manager_clock");
    ASSERT_EQ(result.Stop, ProcessStopReason::Returned) << result.Diagnostic;
    EXPECT_EQ(result.ReturnValue, 0u);
    ASSERT_TRUE(result.JavaState);
    EXPECT_FALSE(result.JavaState->terminal);
    ASSERT_TRUE(result.JavaState->framework.service_manager);
    const auto &state = *result.JavaState->framework.service_manager;
    EXPECT_TRUE(state.initialized);
    EXPECT_NE(state.stat_logger, 0u);
    EXPECT_FALSE(state.cache_known);
    EXPECT_FALSE(state.manager);
  }
}
TEST_P(AndroidJNI, ServiceManagerMetadataDoesNotInitializeTheClass) {
  Options.Android->ObserveJavaState = true;
  auto R = run("jni_service_manager_metadata");
  ASSERT_EQ(R.Stop, ProcessStopReason::Returned) << R.Diagnostic;
  EXPECT_EQ(R.ReturnValue, 0u);
  ASSERT_TRUE(R.JavaState);
  EXPECT_FALSE(R.JavaState->terminal);
  bool Found = false;
  for (const auto &C : R.JavaState->classes)
    if (C.descriptor == "Landroid/os/ServiceManager;") {
      Found = true;
      EXPECT_NE(C.class_object, 0u);
      EXPECT_FALSE(C.supplied);
      EXPECT_EQ(C.initialization,
                mobile::dalvik::JavaClassState::Initialization::NotStarted);
    }
  EXPECT_TRUE(Found);
}
TEST_P(AndroidJNI, ServiceManagerActiveUsesRequireElapsedRealtime) {
  Options.Android->ObserveJavaState = true;
  const char *Calls[] = {"JNIEnv.113", "JNIEnv.144", "JNIEnv.33", "JNIEnv.27"};
  for (bool supplied : {false, true}) {
    SCOPED_TRACE(supplied);
    if (supplied)
      Options.LinuxTime.emplace().Clocks = {
          {0, {100, 0}}, {1, {10, 123456789}}, {4, {30, 500000000}}};
    for (unsigned Mode = 1; Mode != 5; ++Mode) {
      SCOPED_TRACE(Mode);
      Options.Android->Arguments = {Mode};
      auto R = run("jni_service_manager_metadata");
      EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService) << R.Diagnostic;
      EXPECT_EQ(R.Diagnostic, "Linux clock 7 has no explicit linux_time input");
      ASSERT_FALSE(R.NativeCalls.empty());
      EXPECT_EQ(R.NativeCalls.back().Name, Calls[Mode - 1]);
      EXPECT_FALSE(R.NativeCalls.back().Result);
      ASSERT_TRUE(R.JavaState);
      EXPECT_TRUE(R.JavaState->terminal);
      for (const auto &C : R.JavaState->classes)
        if (C.descriptor == "Landroid/os/ServiceManager;")
          EXPECT_EQ(
              C.initialization,
              mobile::dalvik::JavaClassState::Initialization::Initializing);
    }
  }
}
TEST_P(AndroidJNI, ExplicitClassLookupFailuresPreserveExceptionsAndUnknowns) {
  for (const std::string Type :
       {"java/lang/ClassNotFoundException", "java/lang/NoClassDefFoundError"}) {
    Options.Android->JNIClassLookupFailures = {{"fixture/Absent", Type}};
    NativeMemoryRegion Region;
    Region.Address = 0x20000000;
    Region.Size = 4096;
    Region.Bytes.assign(Type.begin(), Type.end());
    Region.Bytes.push_back(0);
    Options.Android->Memory = {Region};
    Options.Android->Arguments = {Region.Address};
    auto R = run("jni_explicit_lookup_failure");
    EXPECT_EQ(R.Stop, ProcessStopReason::Returned) << R.Diagnostic;
    EXPECT_EQ(R.ReturnValue, 0u);
  }
  Options.Android->JNIClassLookupFailures.clear();
  auto Unknown = run("jni_explicit_lookup_failure");
  EXPECT_EQ(Unknown.Stop, ProcessStopReason::RuntimeFailure);
  EXPECT_NE(Unknown.Diagnostic.find("unmodeled class: fixture/Absent"),
            std::string::npos);
}
TEST_P(AndroidJNI, RejectsContradictoryClassLoaderDeclarations) {
  for (const std::string Name : std::vector<std::string>{
           "", "java/lang/String", "android/os/Build$VERSION",
           "android/os/ServiceManager", "bad.name", "[Lfixture/Absent;", "/bad",
           "bad/", "bad//Name", std::string("bad\0Name", 8)}) {
    Options.Android->JNIClassLookupFailures = {
        {Name, "java/lang/ClassNotFoundException"}};
    Options.Android->EntrySymbol = "jni_vm_lifecycle";
    auto R =
        emulateProcess(Path, ProcessProfile::AndroidNativeAArch64, Options);
    ASSERT_FALSE(bool(R)) << Name;
    llvm::consumeError(R.takeError());
  }
  Options.Android->JNIClassLookupFailures = {
      {"fixture/Absent", "java/lang/Error"}};
  auto R = emulateProcess(Path, ProcessProfile::AndroidNativeAArch64, Options);
  ASSERT_FALSE(bool(R));
  llvm::consumeError(R.takeError());
  Options.Android->JNIClassLookupFailures = {
      {"fixture/Absent", "java/lang/ClassNotFoundException"}};
  Options.Android->JNI = false;
  R = emulateProcess(Path, ProcessProfile::AndroidNativeAArch64, Options);
  ASSERT_FALSE(bool(R));
  EXPECT_NE(llvm::toString(R.takeError()).find("require JNI"),
            std::string::npos);
}
TEST_P(AndroidJNI,
       NativeRegistrationsRetainPartialRebindingsAndDeclaringOwners) {
#ifndef NEVERD_ANDROID_DEX_FIXTURE_DIR
  GTEST_SKIP() << "Python DEX fixture generator unavailable";
#else
  auto File = llvm::MemoryBuffer::getFile(
      (std::filesystem::path(NEVERD_ANDROID_DEX_FIXTURE_DIR) /
       "registration.dex")
          .string());
  ASSERT_TRUE(bool(File));
  auto Bytes = (*File)->getBuffer();
  Options.Android->DexImages.emplace_back(Bytes.begin(), Bytes.end());
  for (unsigned Mode = 0; Mode != 12; ++Mode) {
    SCOPED_TRACE(Mode);
    Options.Android->Arguments = {Mode};
    auto R = run("jni_native_registration");
    EXPECT_EQ(R.Stop, Mode < 6 ? ProcessStopReason::Returned
                               : ProcessStopReason::RuntimeFailure)
        << R.Diagnostic;
    if (Mode < 6)
      EXPECT_EQ(R.ReturnValue, 0u);
    std::map<std::string, uint64_t> Bindings;
    for (const auto &B : R.JNIRegistrations)
      ASSERT_TRUE(
          Bindings.emplace(B.Class + "." + B.Name + B.Signature, B.Address)
              .second);
    std::map<std::string, uint64_t> Expected = {
        {"registration/Child.child()V", 0x12340008},
        {"registration/Deferred.after()V", 0x1234000c},
        {"registration/Parent.alpha(I)I", 0x12340000},
        {"registration/Parent.beta(J)J", 0x12340004}};
    if (Mode == 1)
      Expected.erase("registration/Child.child()V");
    if (Mode == 2) {
      Expected.erase("registration/Parent.alpha(I)I");
      Expected.erase("registration/Parent.beta(J)J");
    }
    if (Mode == 3 || Mode == 4)
      Expected["registration/Parent.alpha(I)I"] =
          Mode == 3 ? 0x12340010 : 0x12340014;
    if (Mode == 9 || Mode == 10)
      Expected["registration/Parent.alpha(I)I"] = 0x1234001c;
    EXPECT_EQ(Bindings, Expected);
  }
  Options.Android->JNIClassLookupFailures = {
      {"registration/Child", "java/lang/ClassNotFoundException"}};
  auto R = emulateProcess(Path, ProcessProfile::AndroidNativeAArch64, Options);
  ASSERT_FALSE(bool(R));
  EXPECT_NE(llvm::toString(R.takeError()).find("shadows a known class"),
            std::string::npos);
#endif
}
TEST_P(AndroidJNI, ModifiedUTF8UTF16AndRangeException) {
  auto R = run("jni_string_values");
  EXPECT_EQ(R.Stop, ProcessStopReason::Returned) << R.Diagnostic;
  EXPECT_EQ(R.ReturnValue, 0u);
}
TEST_P(AndroidJNI,
       RegisteredNativeCallsExecuteGuestBodiesAndRetireOnlyTheirLocals) {
#ifndef NEVERD_ANDROID_DEX_FIXTURE_DIR
  GTEST_SKIP() << "Python DEX fixture generator unavailable";
#else
  auto File = llvm::MemoryBuffer::getFile(
      (std::filesystem::path(NEVERD_ANDROID_DEX_FIXTURE_DIR) /
       "native-calls.dex")
          .string());
  ASSERT_TRUE(bool(File));
  auto Bytes = (*File)->getBuffer();
  Options.Android->DexImages.emplace_back(Bytes.begin(), Bytes.end());
  Options.Android->ObserveJavaState = true;
  Options.LinuxTime.emplace().AdvanceOnIdle = true;
  Options.LinuxTime->Clocks = {{1, {0, 0}}};
  for (unsigned Mode : {0u, 18u, 21u, 22u, 23u, 24u}) {
    SCOPED_TRACE(Mode);
    Options.Android->Arguments = {Mode};
    auto R = run("jni_native_calls");
    ASSERT_EQ(R.Stop, ProcessStopReason::Returned) << R.Diagnostic;
    EXPECT_EQ(R.ReturnValue, 0u);
    ASSERT_TRUE(R.JavaState);
    for (const auto &C : R.JavaState->classes)
      if (C.descriptor == "Lnativecall/Base;")
        EXPECT_EQ(C.initialization,
                  mobile::dalvik::JavaClassState::Initialization::Initialized);
      else if (C.descriptor == "Lnativecall/Bad;")
        EXPECT_EQ(
            C.initialization,
            Mode == 18
                ? mobile::dalvik::JavaClassState::Initialization::Erroneous
                : mobile::dalvik::JavaClassState::Initialization::NotStarted);
    EXPECT_FALSE(R.JavaState->terminal);
  }
#endif
}
TEST_P(AndroidJNI, JavaLibraryLoadResumesInitializersAndNestedNativeCalls) {
#ifndef NEVERD_ANDROID_DEX_FIXTURE_DIR
  GTEST_SKIP() << "Python DEX fixture generator unavailable";
#else
  auto File = llvm::MemoryBuffer::getFile(
      (std::filesystem::path(NEVERD_ANDROID_DEX_FIXTURE_DIR) /
       "java-resume.dex")
          .string());
  ASSERT_TRUE(bool(File));
  auto Bytes = (*File)->getBuffer();
  Options.Android->DexImages.emplace_back(Bytes.begin(), Bytes.end());
  Options.Android->ObserveJavaState = true;
  Options.Android->JavaLibrary = "resumption";
  Options.Android->Initialize = true;
  Options.LinuxTime.emplace().AdvanceOnIdle = true;
  Options.LinuxTime->Clocks = {{1, {0, 0}}};
  for (unsigned Mode = 0; Mode <= 7; ++Mode) {
    SCOPED_TRACE(Mode);
    Options.Android->Arguments = {Mode};
    auto R = run("jni_java_resume");
    ASSERT_EQ(R.Stop, ProcessStopReason::Returned) << R.Diagnostic;
    EXPECT_EQ(R.ReturnValue, 0u);
    ASSERT_TRUE(R.JavaState);
    EXPECT_FALSE(R.JavaState->terminal);
    for (const auto &C : R.JavaState->classes)
      if (C.descriptor == "Lresumption/Library;")
        EXPECT_EQ(
            C.initialization,
            Mode == 2 || Mode == 5
                ? mobile::dalvik::JavaClassState::Initialization::Erroneous
                : mobile::dalvik::JavaClassState::Initialization::Initialized);
    EXPECT_EQ(R.JNIRegistrations.size(), 4u);
  }
  Options.Android->Arguments = {8};
  auto InvalidLocal = run("jni_java_resume");
  EXPECT_EQ(InvalidLocal.Stop, ProcessStopReason::RuntimeFailure);
  EXPECT_NE(InvalidLocal.Diagnostic.find("reference"), std::string::npos);
  {
    // Remove only the synthetic export name; the original executable bytes
    // and relocations remain intact. An absent optional hook is not a call.
    auto Original = llvm::MemoryBuffer::getFile(Path.string());
    ASSERT_TRUE(bool(Original));
    auto Bytes = (*Original)->getBuffer().str();
    size_t At = 0, Renamed = 0;
    while ((At = Bytes.find("JNI_OnLoad", At)) != std::string::npos) {
      Bytes.replace(At, 10, "JNI_NoLoad");
      At += 10;
      ++Renamed;
    }
    ASSERT_GT(Renamed, 0u);
    llvm::SmallString<128> Name;
    int FD;
    ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("neverd-java-no-hook", "so",
                                                    FD, Name));
    llvm::FileRemover Cleanup(Name);
    llvm::raw_fd_ostream Stream(FD, true);
    Stream.write(Bytes.data(), Bytes.size());
    Stream.close();
    auto Saved = Path;
    Path = Name.str().str();
    Options.Android->Arguments = {9};
    auto NoHook = run("jni_java_resume");
    Path = Saved;
    EXPECT_EQ(NoHook.Stop, ProcessStopReason::Returned) << NoHook.Diagnostic;
    EXPECT_EQ(NoHook.ReturnValue, 0u);
    EXPECT_TRUE(NoHook.JNIRegistrations.empty());
  }
  Options.Android->Arguments = {0};
  for (auto Name : {"", "different"}) {
    if (*Name)
      Options.Android->JavaLibrary = Name;
    else
      Options.Android->JavaLibrary.reset();
    auto Unknown = run("jni_java_resume");
    EXPECT_EQ(Unknown.Stop, ProcessStopReason::UnsupportedService);
    EXPECT_NE(Unknown.Diagnostic.find("guest ELF binding"), std::string::npos);
    EXPECT_TRUE(Unknown.JNIRegistrations.empty());
    ASSERT_TRUE(Unknown.JavaState);
    EXPECT_TRUE(Unknown.JavaState->terminal);
  }
#endif
}
TEST_P(AndroidJNI, RegisteredNativeCallsRejectUnknownBindingsAndInvalidABIs) {
#ifndef NEVERD_ANDROID_DEX_FIXTURE_DIR
  GTEST_SKIP() << "Python DEX fixture generator unavailable";
#else
  auto File = llvm::MemoryBuffer::getFile(
      (std::filesystem::path(NEVERD_ANDROID_DEX_FIXTURE_DIR) /
       "native-calls.dex")
          .string());
  ASSERT_TRUE(bool(File));
  auto Bytes = (*File)->getBuffer();
  Options.Android->DexImages.emplace_back(Bytes.begin(), Bytes.end());
  for (unsigned Mode = 1; Mode <= 17; ++Mode) {
    SCOPED_TRACE(Mode);
    Options.Android->Arguments = {Mode};
    Options.Limits.Instructions = Mode == 16 ? 4096 : 200000;
    auto R = run("jni_native_calls");
    auto Expected = Mode == 1 || Mode == 2 || (Mode >= 10 && Mode <= 12) ||
                            Mode == 15 || Mode == 17
                        ? ProcessStopReason::UnsupportedService
                    : Mode == 16 ? ProcessStopReason::InstructionLimit
                                 : ProcessStopReason::RuntimeFailure;
    EXPECT_EQ(R.Stop, Expected) << R.Diagnostic;
    EXPECT_FALSE(R.ReturnValue);
    EXPECT_FALSE(R.Diagnostic.empty());
    if (Mode == 16)
      EXPECT_EQ(R.Instructions, 4096u);
    if (Mode == 17)
      EXPECT_NE(R.Diagnostic.find("Java invocation budget exhausted"),
                std::string::npos);
    if (Mode == 1 || Mode == 2 || Mode == 15) {
      ASSERT_FALSE(R.NativeCalls.empty());
      EXPECT_EQ(R.NativeCalls.back().Name, "JNIEnv.129");
    }
  }
#endif
}
TEST_P(AndroidJNI, ARTStringEdgesPreserveSupplementaryCharactersAndNulls) {
  auto R = run("jni_art_string_edges");
  EXPECT_EQ(R.Stop, ProcessStopReason::Returned) << R.Diagnostic;
  EXPECT_EQ(R.ReturnValue, 0u);
}
TEST_P(AndroidJNI, ArrayConstructionAbortsWithoutRecoverableException) {
  for (const char *Entry : {"jni_negative_array", "jni_negative_object_array",
                            "jni_bad_array_initializer"}) {
    auto R = run(Entry);
    EXPECT_EQ(R.Stop, ProcessStopReason::RuntimeFailure)
        << Entry << R.Diagnostic;
    EXPECT_NE(R.Diagnostic.find("JNI abort:"), std::string::npos)
        << Entry << R.Diagnostic;
    ASSERT_FALSE(R.NativeCalls.empty());
    EXPECT_FALSE(R.NativeCalls.back().Result.has_value());
  }
}
TEST_P(AndroidJNI, ArrayValuesCommitAbortAndBounds) {
  auto R = run("jni_arrays");
  EXPECT_EQ(R.Stop, ProcessStopReason::Returned) << R.Diagnostic;
  EXPECT_EQ(R.ReturnValue, 0u);
}
TEST_P(AndroidJNI, LocalFramesAndGlobalReferencesSurviveDetach) {
  auto R = run("jni_references");
  EXPECT_EQ(R.Stop, ProcessStopReason::Returned) << R.Diagnostic;
  EXPECT_EQ(R.ReturnValue, 0u);
}
TEST_P(AndroidJNI, EveryPrimitiveWidthPreservesCompleteValueBits) {
  auto R = run("jni_primitive_bits");
  EXPECT_EQ(R.Stop, ProcessStopReason::Returned) << R.Diagnostic;
  EXPECT_EQ(R.ReturnValue, 0u);
}
TEST_P(AndroidJNI, CooperativeThreadsUseDistinctEnvironments) {
  auto R = run("jni_threads");
  EXPECT_EQ(R.Stop, ProcessStopReason::Returned) << R.Diagnostic;
  EXPECT_EQ(R.ReturnValue, 0u);
  ASSERT_EQ(R.NativeThreads.size(), 2u);
  EXPECT_TRUE(R.NativeThreads[1].Finished);
}
TEST_P(AndroidJNI, InvalidIdentitiesAndBorrowsFailClearly) {
  for (const auto *Entry :
       {"jni_stale_env", "jni_stale_ref", "jni_wrong_thread",
        "jni_invalid_utf8", "jni_double_release", "jni_frame_underflow",
        "jni_detach_borrow", "jni_wrong_borrow_object",
        "jni_wrong_reference_kind", "jni_pending_illegal", "jni_table_replaced",
        "jni_named_env_spoof", "jni_named_vm_spoof"}) {
    auto R = run(Entry);
    EXPECT_EQ(R.Stop, ProcessStopReason::RuntimeFailure)
        << Entry << R.Diagnostic;
    EXPECT_NE(R.Diagnostic.find("JNI"), std::string::npos) << R.Diagnostic;
  }
}
TEST_P(AndroidJNI, PendingExceptionAllowsCleanupWithoutLosingValues) {
  auto R = run("jni_pending_cleanup");
  EXPECT_EQ(R.Stop, ProcessStopReason::Returned) << R.Diagnostic;
  EXPECT_EQ(R.ReturnValue, 0u);
}
TEST_P(AndroidJNI, InvalidBorrowCopyoutCannotSucceed) {
  auto R = run("jni_invalid_copyout");
  EXPECT_EQ(R.Stop, ProcessStopReason::RuntimeFailure) << R.Diagnostic;
}
TEST_P(AndroidJNI, JavaMethodExecutionIsNotInvented) {
  auto R = run("jni_unknown_method");
  EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService) << R.Diagnostic;
  EXPECT_NE(R.Diagnostic.find("JNIEnv.33"), std::string::npos);
}
TEST_P(AndroidJNI, IntrinsicJavaMethodsExecuteArgumentsAndPreserveIdentity) {
  auto R = run("jni_method_values");
  EXPECT_EQ(R.Stop, ProcessStopReason::Returned) << R.Diagnostic;
  EXPECT_EQ(R.ReturnValue, 0u);
}
TEST_P(AndroidJNI, StringInternIdentitySurvivesReferenceAndThreadTeardown) {
  auto R = run("jni_string_intern_identity");
  EXPECT_EQ(R.Stop, ProcessStopReason::Returned) << R.Diagnostic;
  EXPECT_EQ(R.ReturnValue, 0u);
}
TEST_P(AndroidJNI, MethodExceptionsPreserveArrayAndExceptionPrecedence) {
  auto R = run("jni_method_exceptions");
  EXPECT_EQ(R.Stop, ProcessStopReason::Returned) << R.Diagnostic;
  EXPECT_EQ(R.ReturnValue, 0u);
}
TEST_P(AndroidJNI, InvalidMethodInvocationCannotSupplyAResult) {
  for (const char *Entry :
       {"jni_method_wrong_return", "jni_method_wrong_receiver",
        "jni_method_forged_id", "jni_method_unreadable_arguments"}) {
    auto R = run(Entry);
    EXPECT_EQ(R.Stop, ProcessStopReason::RuntimeFailure)
        << Entry << R.Diagnostic;
    ASSERT_FALSE(R.NativeCalls.empty());
    EXPECT_FALSE(R.NativeCalls.back().Result.has_value());
  }
}
TEST_P(AndroidJNI, EnvironmentIsExplicitlyEnabled) {
  Options.Android->JNI = false;
  auto R = run("jni_vm_lifecycle");
  EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService) << R.Diagnostic;
}
TEST_P(AndroidJNI, SplitSourceDirsSharesMutableArraysAcrossThreads) {
  using Snapshot = mobile::dalvik::AndroidApplicationSnapshot;
  using Sources = Snapshot::ApplicationInfo::SplitSourceDirectories;
  for (bool public_sources : {false, true}) {
    SCOPED_TRACE(public_sources);
    const char *entry = public_sources ? "jni_split_public_source_dirs"
                                       : "jni_split_source_dirs";
    Options.Android->Arguments = {1};
    auto declaration = run(entry);
    EXPECT_EQ(declaration.Stop, ProcessStopReason::Returned)
        << declaration.Diagnostic;
    EXPECT_EQ(declaration.ReturnValue, 0u);
    Options.Android->Arguments.clear();
    Options.Android->Application = Snapshot{Snapshot::Phase::Application};
    auto &input = *Options.Android->Application;
    input.package_manager_available = true;
    input.application_info.emplace().source_dir = "base.apk";
    auto unknown = run(entry);
    EXPECT_EQ(unknown.Stop, ProcessStopReason::UnsupportedService);
    EXPECT_NE(unknown.Diagnostic.find(
                  public_sources ? "splitPublicSourceDirs is not supplied"
                                 : "splitSourceDirs is not supplied"),
              std::string::npos);
    auto &field = public_sources
                      ? input.application_info->split_public_source_dirs
                      : input.application_info->split_source_dirs;
    auto &other = public_sources
                      ? input.application_info->split_source_dirs
                      : input.application_info->split_public_source_dirs;
    other = Sources{"other.apk"};
    field = Sources{"first.apk", std::nullopt, "", "first.apk"};
    input.application_info_queries = {{"test.app", 0, *input.application_info}};
    auto positive = run(entry);
    EXPECT_EQ(positive.Stop, ProcessStopReason::Returned)
        << positive.Diagnostic;
    EXPECT_EQ(positive.ReturnValue, 0u);
    Options.Android->Arguments = {6};
    auto foreign = run(entry);
    EXPECT_EQ(foreign.Stop, ProcessStopReason::UnsupportedService);
    EXPECT_NE(
        foreign.Diagnostic.find("unmodeled Android ApplicationInfo instance"),
        std::string::npos);
    Options.Android->Arguments = {2};
    field.emplace();
    auto null = run(entry);
    EXPECT_EQ(null.Stop, ProcessStopReason::Returned) << null.Diagnostic;
    EXPECT_EQ(null.ReturnValue, 0u);
    Options.Android->Arguments = {3};
    field = Sources{};
    auto empty = run(entry);
    EXPECT_EQ(empty.Stop, ProcessStopReason::Returned) << empty.Diagnostic;
    EXPECT_EQ(empty.ReturnValue, 0u);
    Options.Android->Arguments = {5};
    field.reset();
    auto written = run(entry);
    EXPECT_EQ(written.Stop, ProcessStopReason::Returned) << written.Diagnostic;
    EXPECT_EQ(written.ReturnValue, 0u);
  }
}
INSTANTIATE_TEST_SUITE_P(
    Backends, AndroidJNI,
    testing::Combine(testing::Values("jni-O0-none", "jni-O2-none",
                                     "jni-O0-android", "jni-O2-android",
                                     "jni-O0-relr", "jni-O2-relr"),
                     testing::Values(ExecutionBackendKind::Unicorn,
                                     ExecutionBackendKind::HVF,
                                     ExecutionBackendKind::KVM,
                                     ExecutionBackendKind::WHP)));
} // namespace
} // namespace neverd::emulation
