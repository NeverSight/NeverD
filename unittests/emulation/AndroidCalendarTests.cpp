//===- AndroidCalendarTests.cpp - Guest calendar ownership and ABI --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "gtest/gtest.h"

#include "neverd/emulation/ExecutionConfiguration.h"
#include "neverd/emulation/ProcessReport.h"
#include "neverd/emulation/ProcessSession.h"

#include "llvm/Support/Endian.h"

#include <array>
#include <tuple>

namespace neverd::emulation {
namespace {
constexpr uint64_t Buffer = 0x20000000;
constexpr uint64_t SharedTM = 0x7030000000, GMTName = SharedTM + 4096;
using Fields = std::array<int32_t, 9>;
void putTM(std::vector<uint8_t> &B, unsigned At, const Fields &F,
           int64_t Offset, uint64_t Zone, uint8_t Padding) {
  for (unsigned I = 0; I < F.size(); ++I)
    llvm::support::endian::write32le(B.data() + At + 4 * I, uint32_t(F[I]));
  std::fill_n(B.data() + At + 36, 4, Padding);
  llvm::support::endian::write64le(B.data() + At + 40, uint64_t(Offset));
  llvm::support::endian::write64le(B.data() + At + 48, Zone);
}
class AndroidCalendar : public testing::TestWithParam<
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
    auto P = probeExecutionBackend(C);
    ASSERT_TRUE(bool(P)) << llvm::toString(P.takeError());
    if (P->Availability != BackendAvailability::Available)
      GTEST_SKIP() << P->Reason;
    Options.Android.emplace();
    Options.Android->Initialize = false;
    Options.Android->TraceLimit = 20000;
    Options.Android->Memory.push_back(
        {Buffer, 4096, std::vector<uint8_t>(4096, 0xa5), false});
    Options.Android->ReadMemory.push_back({Buffer, 4096});
    Options.Android->FixedTimeZone = AndroidFixedTimeZone{19800, "FIX+0530"};
    Options.Android->Libraries["libcalendar.so"] = {"localtime", "localtime_r",
                                                    "gmtime", "gmtime_r"};
    Options.Limits.Instructions = 100000;
    Options.InstructionQuantum = 7;
#endif
  }
  ProcessResult run(const char *Entry, std::vector<uint64_t> Args) {
    Options.Android->EntrySymbol = Entry;
    Options.Android->Arguments = std::move(Args);
    auto R =
        emulateProcess(Path, ProcessProfile::AndroidNativeAArch64, Options);
    if (!R)
      llvm::report_fatal_error(R.takeError());
    return std::move(*R);
  }
  ProcessResult call(unsigned Op, int64_t Seconds, unsigned Mode = 0,
                     unsigned Named = 0) {
    return run("calendar_call", {Buffer, Op, uint64_t(Seconds), Mode, Named});
  }
  void complete(const ProcessResult &R, const std::vector<uint8_t> &Expected) {
    ASSERT_EQ(R.Stop, ProcessStopReason::Returned) << R.Diagnostic;
    EXPECT_EQ(R.ReturnValue, 0u);
    EXPECT_FALSE(R.TraceTruncated);
    EXPECT_EQ(R.Trace.size(), R.Instructions);
    ASSERT_EQ(R.MemorySnapshots.size(), 1u);
    EXPECT_EQ(R.MemorySnapshots[0].Bytes, Expected);
  }
  void check(unsigned Op, int64_t Seconds, const Fields &F, unsigned Mode = 0,
             unsigned Named = 0) {
    bool Local = Op == 0 || Op == 2, Caller = Op >= 2;
    int64_t Offset = Local ? Options.Android->FixedTimeZone->OffsetSeconds : 0;
    std::string Name = Local ? Options.Android->FixedTimeZone->Name : "GMT";
    uint64_t Zone = GMTName + (Local ? 64 : 0);
    std::vector<uint8_t> Expected(4096, 0xa5);
    llvm::support::endian::write64le(Expected.data(),
                                     Caller ? Buffer + 0x100 : SharedTM);
    llvm::support::endian::write64le(Expected.data() + 8, 77);
    if (Caller)
      putTM(Expected, 0x100, F, Offset, Zone, 0xa5);
    putTM(Expected, 0x200, F, Offset, Zone, Caller ? 0xa5 : 0);
    std::copy(Name.begin(), Name.end(), Expected.begin() + 0x300);
    Expected[0x300 + Name.size()] = 0;
    complete(call(Op, Seconds, Mode, Named), Expected);
  }
};
TEST_P(AndroidCalendar, UTCUsesSigned64BitTimeAndGregorianCenturyRules) {
  struct Case {
    int64_t Seconds;
    Fields F;
  };
  // Independent known civil dates, including both ends of the admitted range.
  const Case Cases[] = {
      {-62135596800LL, {0, 0, 0, 1, 0, -1899, 1, 0, 0}},
      {-2203977600LL, {0, 0, 0, 28, 1, 0, 3, 58, 0}},
      {-1, {59, 59, 23, 31, 11, 69, 3, 364, 0}},
      {0, {0, 0, 0, 1, 0, 70, 4, 0, 0}},
      {951782400, {0, 0, 0, 29, 1, 100, 2, 59, 0}},
      {2147483648LL, {8, 14, 3, 19, 0, 138, 2, 18, 0}},
      {4107542400LL, {0, 0, 0, 1, 2, 200, 1, 59, 0}},
      {253402300799LL, {59, 59, 23, 31, 11, 8099, 5, 364, 0}}};
  Options.Android->FixedTimeZone.reset();
  for (const auto &C : Cases)
    for (unsigned Op : {1, 3})
      for (unsigned Named : {0, 1}) {
        SCOPED_TRACE(C.Seconds);
        check(Op, C.Seconds, C.F, 0, Named);
      }
}
TEST_P(AndroidCalendar, ExplicitFixedZonesRetainPaddingAliasingAndErrno) {
  for (unsigned Op : {0, 2})
    for (unsigned Named : {0, 1})
      check(Op, -1, {59, 29, 5, 1, 0, 70, 4, 0, 0}, 0, Named);
  check(2, -1, {59, 29, 5, 1, 0, 70, 4, 0, 0}, 1);
  Options.Android->FixedTimeZone = AndroidFixedTimeZone{-3600, "FIX-01"};
  check(2, 0, {0, 0, 23, 31, 11, 69, 3, 364, 0}, 1);
  Options.Android->FixedTimeZone = AndroidFixedTimeZone{86400, "NEXT"};
  check(0, 0, {0, 0, 0, 2, 0, 70, 5, 1, 0});
  Options.Android->FixedTimeZone = AndroidFixedTimeZone{-86400, "PREV"};
  check(2, 0, {0, 0, 0, 31, 11, 69, 3, 364, 0});
}
TEST_P(AndroidCalendar, ProcessStaticObjectIsSharedAcrossAPIsAndThreads) {
  Options.Android->ThreadLimit = 3;
  for (unsigned Threaded : {0, 1}) {
    std::vector<uint8_t> Expected(4096, 0xa5);
    uint64_t Words[] = {SharedTM, SharedTM, Buffer + 0x100,      77,
                        1,        1,        Threaded ? 88u : 77u};
    for (unsigned I = 0; I < std::size(Words); ++I)
      llvm::support::endian::write64le(Expected.data() + I * 8, Words[I]);
    putTM(Expected, 0x100, {0, 0, 0, 29, 1, 100, 2, 59, 0}, 0, GMTName, 0xa5);
    putTM(Expected, 0x200, {59, 59, 23, 31, 11, 69, 3, 364, 0}, 0, GMTName, 0);
    for (unsigned At : {0x300, 0x400})
      putTM(Expected, At, {0, 30, 5, 1, 0, 70, 4, 0, 0}, 19800, GMTName + 64,
            0);
    complete(run("calendar_sequence", {Buffer, Threaded}), Expected);
  }
}
TEST_P(AndroidCalendar, UnknownZonesRangesAndProvidersNeverInventDates) {
  Options.Android->FixedTimeZone.reset();
  for (unsigned Op : {0, 2}) {
    auto R = call(Op, 0);
    EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService);
    EXPECT_EQ(R.Diagnostic, "localtime requires an explicit fixed time zone");
    ASSERT_EQ(R.MemorySnapshots.size(), 1u);
    EXPECT_EQ(R.MemorySnapshots[0].Bytes, std::vector<uint8_t>(4096, 0xa5));
  }
  for (int64_t S : std::array<int64_t, 4>{INT64_MIN, INT64_MAX, -62135596801LL,
                                          253402300800LL}) {
    auto R = call(3, S);
    EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService);
    EXPECT_EQ(R.Diagnostic,
              "calendar conversion supports years 1 through 9999");
    ASSERT_EQ(R.MemorySnapshots.size(), 1u);
    EXPECT_EQ(R.MemorySnapshots[0].Bytes, std::vector<uint8_t>(4096, 0xa5));
    EXPECT_FALSE(R.NativeCalls.back().Result);
  }
  auto R = call(1, 0, 0, 2);
  EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService);
  EXPECT_NE(R.Diagnostic.find("inactive dynamic library"), std::string::npos);
}
TEST_P(AndroidCalendar, InvalidMemoryAndMalformedZonesFailClearly) {
  for (unsigned Mode : {2, 3, 4}) {
    auto R = call(3, 0, Mode);
    EXPECT_EQ(R.Stop, ProcessStopReason::RuntimeFailure);
    EXPECT_FALSE(R.ReturnValue);
    EXPECT_FALSE(R.NativeCalls.back().Result);
  }
  for (auto Zone : {AndroidFixedTimeZone{86401, "X"},
                    {-86401, "X"},
                    {0, ""},
                    {0, std::string(33, 'x')},
                    {0, "bad name"},
                    {0, std::string("A\0B", 3)}}) {
    Options.Android->FixedTimeZone = Zone;
    Options.Android->EntrySymbol = "calendar_call";
    auto R =
        emulateProcess(Path, ProcessProfile::AndroidNativeAArch64, Options);
    ASSERT_FALSE(bool(R));
    EXPECT_NE(
        llvm::toString(R.takeError()).find("invalid fixed calendar time zone"),
        std::string::npos);
  }
}
TEST(AndroidCalendarOptions, StrictExplicitZoneShapeAndSignedWidth) {
  for (
      const char *S :
      {R"({"android":{"fixed_time_zone":null}})",
       R"({"android":{"fixed_time_zone":{}}})",
       R"({"android":{"fixed_time_zone":{"name":"X"}}})",
       R"({"android":{"fixed_time_zone":{"name":"X","offset_seconds":1.5}}})",
       R"({"android":{"fixed_time_zone":{"name":"X","offset_seconds":2147483648}}})",
       R"({"android":{"fixed_time_zone":{"name":"X","offset_seconds":0,"dst":true}}})"}) {
    auto R = processOptionsFromJSON(S);
    ASSERT_FALSE(bool(R));
    llvm::consumeError(R.takeError());
  }
  auto R = processOptionsFromJSON(
      R"({"android":{"fixed_time_zone":{"name":"FIX","offset_seconds":-3600}}})");
  ASSERT_TRUE(bool(R));
  ASSERT_TRUE(R->Android->FixedTimeZone);
  EXPECT_EQ(R->Android->FixedTimeZone->OffsetSeconds, -3600);
  EXPECT_EQ(R->Android->FixedTimeZone->Name, "FIX");
}
INSTANTIATE_TEST_SUITE_P(
    Native, AndroidCalendar,
    testing::Combine(testing::Values("calendar-O0-none", "calendar-O0-android",
                                     "calendar-O0-relr", "calendar-O2-none",
                                     "calendar-O2-android", "calendar-O2-relr"),
                     testing::Values(ExecutionBackendKind::Unicorn,
                                     ExecutionBackendKind::HVF,
                                     ExecutionBackendKind::KVM,
                                     ExecutionBackendKind::WHP)));
} // namespace
} // namespace neverd::emulation
