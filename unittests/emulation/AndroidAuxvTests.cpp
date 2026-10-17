//===- AndroidAuxvTests.cpp - Explicit native auxiliary vector semantics -===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "gtest/gtest.h"

#include "neverd/emulation/ExecutionConfiguration.h"
#include "neverd/emulation/ProcessReport.h"
#include "neverd/emulation/ProcessSession.h"

#include "llvm/Support/Endian.h"

#include <tuple>

namespace neverd::emulation {
namespace {
constexpr uint64_t Buffer = 0x20000000;
constexpr uint64_t WideType = 0x10000000010, WideValue = 0xfedcba9876543210;
class AndroidAuxv : public testing::TestWithParam<
                        std::tuple<const char *, ExecutionBackendKind>> {
protected:
  ProcessOptions Options;
  std::filesystem::path Path;
  void SetUp() override {
#ifndef NEVERD_ANDROID_FIXTURE_DIR
    GTEST_SKIP() << "Android cross compiler unavailable";
#else
    Path = std::filesystem::path(NEVERD_ANDROID_FIXTURE_DIR) /
           (std::string(std::get<0>(GetParam())) + ".so");
    Options.Backend = std::get<1>(GetParam());
    ExecutionConfiguration C;
    C.Backend = Options.Backend;
    C.Architecture = GuestArchitecture::AArch64;
    C.Contract = ExecutionContract::CheckedUserAArch64;
    auto Probe = probeExecutionBackend(C);
    ASSERT_TRUE(bool(Probe)) << llvm::toString(Probe.takeError());
    if (Probe->Availability != BackendAvailability::Available)
      GTEST_SKIP() << Probe->Reason;
    Options.Android.emplace();
    Options.Android->Initialize = false;
    Options.Android->TraceLimit = 10000;
    Options.Android->Memory.push_back(
        {Buffer, 4096, std::vector<uint8_t>(4096, 0xa5), false});
    Options.Android->ReadMemory.push_back({Buffer, 4096});
    Options.Android->AuxiliaryVector = std::vector<AndroidAuxiliaryValue>{
        {16, 0x1234}, {26, 0}, {WideType, WideValue}, {UINT64_MAX, 0xdeadbeef}};
    Options.Android->Libraries["libauxv.so"] = {"getauxval"};
    Options.Limits.Instructions = 100000;
    Options.InstructionQuantum = 7;
#endif
  }
  ProcessResult run(const char *Entry, uint64_t Mode = 0, uint64_t Type = 16) {
    Options.Android->EntrySymbol = Entry;
    Options.Android->Arguments = {Buffer, Mode, Type};
    auto R =
        emulateProcess(Path, ProcessProfile::AndroidNativeAArch64, Options);
    if (!R)
      llvm::report_fatal_error(R.takeError());
    return std::move(*R);
  }
  void check(const ProcessResult &R, std::initializer_list<uint64_t> Words) {
    ASSERT_EQ(R.Stop, ProcessStopReason::Returned) << R.Diagnostic;
    EXPECT_EQ(R.ReturnValue, 0u);
    EXPECT_FALSE(R.TraceTruncated);
    EXPECT_EQ(R.Trace.size(), R.Instructions);
    ASSERT_EQ(R.MemorySnapshots.size(), 1u);
    std::vector<uint8_t> Expected(4096, 0xa5);
    unsigned I = 0;
    for (uint64_t V : Words) {
      llvm::support::endian::write64le(Expected.data() + I * 8, V);
      ++I;
    }
    EXPECT_EQ(R.MemorySnapshots[0].Bytes, Expected);
  }
};
TEST_P(AndroidAuxv, StaticAndNamedCallsPreserveFullWidthsZeroAndErrno) {
  for (unsigned Mode : {0, 1}) {
    check(run("auxiliary_query", Mode, 16), {0x1234, 77, 0, 0});
    check(run("auxiliary_query", Mode, 26), {0, 77, 0, 0});
    check(run("auxiliary_query", Mode, WideType), {WideValue, 77, 0, 0});
    check(run("auxiliary_query", Mode, UINT64_MAX), {0xdeadbeef, 77, 0, 0});
    for (uint64_t Missing : {0u, 42u})
      check(run("auxiliary_query", Mode, Missing), {0, 2, 0, 0});
  }
}
TEST_P(AndroidAuxv, UnknownIsNotAnEmptyVectorAndClosedProviderStaysInvalid) {
  Options.Android->AuxiliaryVector.reset();
  auto R = run("auxiliary_query");
  EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService);
  EXPECT_EQ(R.Diagnostic, "getauxval requires an explicit auxiliary vector");
  ASSERT_FALSE(R.NativeCalls.empty());
  EXPECT_EQ(R.NativeCalls.back().Name, "getauxval");
  EXPECT_FALSE(R.NativeCalls.back().Result);
  ASSERT_EQ(R.MemorySnapshots.size(), 1u);
  EXPECT_EQ(R.MemorySnapshots[0].Bytes, std::vector<uint8_t>(4096, 0xa5));
  Options.Android->AuxiliaryVector.emplace();
  check(run("auxiliary_query"), {0, 2, 0, 0});
  R = run("auxiliary_query", 2);
  EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService);
  EXPECT_NE(R.Diagnostic.find("inactive dynamic library"), std::string::npos);
  ASSERT_FALSE(R.NativeCalls.empty());
  EXPECT_EQ(R.NativeCalls.back().Name, "getauxval");
  EXPECT_FALSE(R.NativeCalls.back().Result);
}
TEST_P(AndroidAuxv, InitializersAndFinalizersUseTheSameVector) {
  Options.Android->Initialize = true;
  check(run("auxiliary_query", 0, 26), {0, 77, 0x1234, 91});
  auto R = run("auxiliary_finalize");
  check(R, {1, 0, 83, 83});
  EXPECT_EQ(llvm::count_if(R.NativeCalls,
                           [](const auto &C) { return C.Name == "getauxval"; }),
            2);
}
TEST_P(AndroidAuxv, MissingKeysChangeOnlyTheCallingThreadsErrno) {
  Options.Android->ThreadLimit = 3;
  check(run("auxiliary_thread"), {0, 2, 0, 2, 77, 0x1234, 77});
}
TEST_P(AndroidAuxv, MalformedVectorsFailBeforeGuestInitialization) {
  for (auto Values : {std::vector<AndroidAuxiliaryValue>{{0, 1}},
                      std::vector<AndroidAuxiliaryValue>{{16, 0}, {16, 1}},
                      std::vector<AndroidAuxiliaryValue>(129, {1, 0})}) {
    Options.Android->AuxiliaryVector = Values;
    Options.Android->EntrySymbol = "auxiliary_query";
    auto R =
        emulateProcess(Path, ProcessProfile::AndroidNativeAArch64, Options);
    ASSERT_FALSE(bool(R));
    EXPECT_NE(llvm::toString(R.takeError()).find("auxiliary vector entries"),
              std::string::npos);
  }
  Options.Android->AuxiliaryVector->clear();
  for (unsigned I = 1; I <= 128; ++I)
    Options.Android->AuxiliaryVector->push_back({I, I * 3});
  check(run("auxiliary_query", 0, 128), {384, 77, 0, 0});
}
TEST(AndroidAuxvOptions, WireInputRetainsPresenceWidthsAndStrictShape) {
  for (
      const char *Text :
      {R"({"android":{"auxiliary_vector":null}})",
       R"({"android":{"auxiliary_vector":{}}})",
       R"({"android":{"auxiliary_vector":[{}]}})",
       R"({"android":{"auxiliary_vector":[{"type":16}]}})",
       R"({"android":{"auxiliary_vector":[{"type":16,"value":0,"extra":1}]}})",
       R"({"android":{"auxiliary_vector":[{"type":-1,"value":0}]}})",
       R"({"android":{"auxiliary_vector":[{"type":16,"value":1.5}]}})",
       R"({"android":{"auxiliary_vector":[{"type":16,"value":"0x10000000000000000"}]}})"}) {
    auto O = processOptionsFromJSON(Text);
    ASSERT_FALSE(bool(O)) << Text;
    llvm::consumeError(O.takeError());
  }
  auto Absent = processOptionsFromJSON(R"({"android":{}})");
  ASSERT_TRUE(bool(Absent));
  EXPECT_FALSE(Absent->Android->AuxiliaryVector);
  auto Empty = processOptionsFromJSON(R"({"android":{"auxiliary_vector":[]}})");
  ASSERT_TRUE(bool(Empty));
  ASSERT_TRUE(Empty->Android->AuxiliaryVector);
  EXPECT_TRUE(Empty->Android->AuxiliaryVector->empty());
  auto Wide = processOptionsFromJSON(
      R"({"android":{"auxiliary_vector":[{"type":"0xffffffffffffffff","value":"0xfedcba9876543210"}]}})");
  ASSERT_TRUE(bool(Wide));
  ASSERT_EQ(Wide->Android->AuxiliaryVector->size(), 1u);
  EXPECT_EQ(Wide->Android->AuxiliaryVector->front().Type, UINT64_MAX);
  EXPECT_EQ(Wide->Android->AuxiliaryVector->front().Value, WideValue);
}
INSTANTIATE_TEST_SUITE_P(
    Native, AndroidAuxv,
    testing::Combine(testing::Values("auxv-O0-none", "auxv-O0-android",
                                     "auxv-O0-relr", "auxv-O2-none",
                                     "auxv-O2-android", "auxv-O2-relr"),
                     testing::Values(ExecutionBackendKind::Unicorn,
                                     ExecutionBackendKind::HVF,
                                     ExecutionBackendKind::KVM,
                                     ExecutionBackendKind::WHP)));
} // namespace
} // namespace neverd::emulation
