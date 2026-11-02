//===- LinuxClockTests.cpp - Independent virtual time invariants ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "gtest/gtest.h"
#include "os/linux/kernel/LinuxTime.h"

namespace neverd::emulation {
namespace {
TEST(LinuxClock, IdleAdvancementSharesElapsedTimeWithoutOverflowingEpochs) {
  std::optional<LinuxTimeOptions> Options{std::in_place};
  Options->AdvanceOnIdle = true;
  Options->Clocks = {
      {0, {4294967297, 999999998}}, {1, {12, 3}}, {7, {INT64_MIN, 999999999}}};
  linux_model::LinuxClock Clock(Options);
  ProcessResult R{ProcessProfile::LinuxELF64,
                  GuestArchitecture::X64,
                  ExecutionBackendKind::Unicorn,
                  {}};
  ASSERT_FALSE(bool(linux_model::validateTimeOptions(*Options)));
  ASSERT_EQ(Clock.deadline({1, 4}, R), 1000000004u);
  ASSERT_EQ(Clock.elapsed(), 0u);
  ASSERT_TRUE(Clock.advanceTo(1000000004, R));
  EXPECT_EQ(Clock.read(0, R)->Seconds, 4294967299);
  EXPECT_EQ(Clock.read(0, R)->Nanoseconds, 2);
  EXPECT_EQ(Clock.read(1, R)->Seconds, 13);
  EXPECT_EQ(Clock.read(1, R)->Nanoseconds, 7);
  EXPECT_EQ(Clock.read(7, R)->Seconds, INT64_MIN + 2);
  EXPECT_EQ(Clock.read(7, R)->Nanoseconds, 3);
  EXPECT_EQ(Clock.elapsed(), 1000000004u);
  EXPECT_FALSE(Clock.read(2, R));
  EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService);
}

TEST(LinuxClock, InstructionTimeSharesIdleEpochAndRefusesOverflowAtomically) {
  std::optional<LinuxTimeOptions> Options{std::in_place};
  Options->AdvanceOnIdle = true;
  Options->NanosecondsPerInstruction = 3;
  Options->Clocks = {{1, {4, 999999990}}, {2, {1, 2}}};
  linux_model::LinuxClock Clock(Options);
  ProcessResult R{ProcessProfile::LinuxELF64,
                  GuestArchitecture::X64,
                  ExecutionBackendKind::Unicorn,
                  {}};
  ASSERT_TRUE(Clock.accountInstructions(10, R));
  EXPECT_EQ(Clock.elapsed(), 30u);
  EXPECT_EQ(Clock.read(1, R)->Seconds, 5);
  EXPECT_EQ(Clock.read(1, R)->Nanoseconds, 20);
  EXPECT_EQ(Clock.read(2, R)->Nanoseconds, 2);
  ASSERT_TRUE(Clock.advanceTo(100, R));
  ASSERT_TRUE(Clock.accountInstructions(10, R));
  EXPECT_EQ(Clock.elapsed(), 100u);
  ASSERT_TRUE(Clock.accountInstructions(13, R));
  EXPECT_EQ(Clock.elapsed(), 109u);
  EXPECT_FALSE(Clock.accountInstructions(12, R));
  EXPECT_EQ(Clock.elapsed(), 109u);
  EXPECT_FALSE(Clock.accountInstructions(UINT64_MAX, R));
  EXPECT_EQ(Clock.elapsed(), 109u);
  ASSERT_TRUE(Clock.accountInstructions(14, R));
  EXPECT_EQ(Clock.elapsed(), 112u);
  Options->Clocks[0] = {INT64_MAX, 999999887};
  EXPECT_FALSE(Clock.accountInstructions(15, R));
  EXPECT_EQ(Clock.elapsed(), 112u);
  EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService);
}

TEST(LinuxClock, OverflowRefusesAllClocksAndSaturatedSleepIsNotEINVAL) {
  std::optional<LinuxTimeOptions> Options{std::in_place};
  Options->AdvanceOnIdle = true;
  Options->Clocks = {{0, {INT64_MAX, 999999998}}, {1, {3, 4}}};
  linux_model::LinuxClock Clock(Options);
  ProcessResult R{ProcessProfile::LinuxELF64,
                  GuestArchitecture::AArch64,
                  ExecutionBackendKind::Unicorn,
                  {}};
  EXPECT_FALSE(Clock.deadline({INT64_MAX, 0}, R));
  EXPECT_EQ(R.Diagnostic, linux_model::SleepRange);
  ASSERT_TRUE(Clock.advanceTo(1, R));
  EXPECT_FALSE(Clock.advanceTo(2, R));
  EXPECT_EQ(R.Diagnostic, linux_model::TimeAdvanceOverflow);
  EXPECT_EQ(Clock.elapsed(), 1u);
  EXPECT_EQ(Clock.read(0, R)->Seconds, INT64_MAX);
  EXPECT_EQ(Clock.read(0, R)->Nanoseconds, 999999999);
  EXPECT_EQ(Clock.read(1, R)->Nanoseconds, 5);

  Options->Clocks.clear();
  ASSERT_TRUE(Clock.advanceTo(INT64_MAX - 1, R));
  EXPECT_FALSE(Clock.deadline({0, 2}, R));
  EXPECT_EQ(R.Diagnostic, linux_model::SleepRange);
  EXPECT_EQ(Clock.elapsed(), uint64_t(INT64_MAX) - 1);
}
TEST(LinuxClock, ProcessCPUObservationsShareAliasesAndRemainFixedWhileIdle) {
  std::optional<LinuxTimeOptions> Options{std::in_place};
  Options->AdvanceOnIdle = true;
  Options->Clocks = {
      {1, {4, 999999999}}, {-6, {3, 4}}, {-8, {5, 6}}, {-14, {7, 8}}};
  ASSERT_FALSE(bool(linux_model::validateTimeOptions(*Options)));
  linux_model::LinuxClock Clock(Options);
  ProcessResult R{ProcessProfile::LinuxELF64,
                  GuestArchitecture::AArch64,
                  ExecutionBackendKind::Unicorn,
                  {}};
  ASSERT_TRUE(Clock.advanceTo(1000000002, R));
  EXPECT_EQ(Clock.read(1, R)->Seconds, 6);
  EXPECT_EQ(Clock.read(1, R)->Nanoseconds, 1);
  for (int32_t ID : {2, -6, -8006}) {
    EXPECT_EQ(Clock.read(ID, R)->Seconds, 3);
    EXPECT_EQ(Clock.read(ID, R)->Nanoseconds, 4);
  }
  EXPECT_EQ(Clock.read(-8008, R)->Seconds, 5);
  EXPECT_EQ(Clock.read(-14, R)->Seconds, 7);
  EXPECT_EQ(Clock.read(-14, R)->Nanoseconds, 8);
  Options->Clocks.emplace(2, LinuxTimespec{3, 4});
  auto E = linux_model::validateTimeOptions(*Options);
  ASSERT_TRUE(bool(E));
  EXPECT_EQ(llvm::toString(std::move(E)), linux_model::TimeClockOption);
}
TEST(LinuxClock, RawMonotonicSharesElapsedTimeButKeepsItsIndependentEpoch) {
  std::optional<LinuxTimeOptions> Options{std::in_place};
  Options->AdvanceOnIdle = true;
  Options->NanosecondsPerInstruction = 3;
  Options->Clocks = {{0, {2, 10}},
                     {1, {12, 3}},
                     {2, {4, 5}},
                     {4, {4294967298, 999999998}},
                     {7, {20, 100}}};
  ASSERT_FALSE(bool(linux_model::validateTimeOptions(*Options)));
  linux_model::LinuxClock Clock(Options);
  ProcessResult R{ProcessProfile::LinuxELF64,
                  GuestArchitecture::AArch64,
                  ExecutionBackendKind::Unicorn,
                  {}};
  ASSERT_TRUE(Clock.accountInstructions(10, R));
  EXPECT_EQ(Clock.read(4, R)->Seconds, 4294967299);
  EXPECT_EQ(Clock.read(4, R)->Nanoseconds, 28);
  ASSERT_TRUE(Clock.advanceTo(1000000004, R));
  ASSERT_TRUE(Clock.accountInstructions(10, R));
  ASSERT_TRUE(Clock.accountInstructions(13, R));
  EXPECT_EQ(Clock.elapsed(), 1000000013u);
  EXPECT_EQ(Clock.read(4, R)->Seconds, 4294967300);
  EXPECT_EQ(Clock.read(4, R)->Nanoseconds, 11);
  EXPECT_EQ(Clock.read(1, R)->Seconds, 13);
  EXPECT_EQ(Clock.read(1, R)->Nanoseconds, 16);
  EXPECT_EQ(Clock.read(0, R)->Seconds, 3);
  EXPECT_EQ(Clock.read(0, R)->Nanoseconds, 23);
  EXPECT_EQ(Clock.read(7, R)->Seconds, 21);
  EXPECT_EQ(Clock.read(7, R)->Nanoseconds, 113);
  EXPECT_EQ(Clock.read(2, R)->Seconds, 4);
  EXPECT_EQ(Clock.read(2, R)->Nanoseconds, 5);
}

TEST(LinuxClock, RawMonotonicOverflowRefusesAdvancementForEveryClock) {
  std::optional<LinuxTimeOptions> Options{std::in_place};
  Options->AdvanceOnIdle = true;
  Options->NanosecondsPerInstruction = 1;
  Options->Clocks = {{0, {7, 2}}, {1, {3, 4}}, {4, {INT64_MAX, 999999998}}};
  ASSERT_FALSE(bool(linux_model::validateTimeOptions(*Options)));
  linux_model::LinuxClock Clock(Options);
  ProcessResult R{ProcessProfile::LinuxELF64,
                  GuestArchitecture::AArch64,
                  ExecutionBackendKind::Unicorn,
                  {}};
  ASSERT_TRUE(Clock.advanceTo(1, R));
  EXPECT_FALSE(Clock.advanceTo(2, R));
  EXPECT_FALSE(Clock.accountInstructions(1, R));
  EXPECT_EQ(R.Diagnostic, linux_model::TimeAdvanceOverflow);
  EXPECT_EQ(Clock.elapsed(), 1u);
  EXPECT_EQ(Clock.read(4, R)->Seconds, INT64_MAX);
  EXPECT_EQ(Clock.read(4, R)->Nanoseconds, 999999999);
  EXPECT_EQ(Clock.read(0, R)->Seconds, 7);
  EXPECT_EQ(Clock.read(0, R)->Nanoseconds, 3);
  EXPECT_EQ(Clock.read(1, R)->Seconds, 3);
  EXPECT_EQ(Clock.read(1, R)->Nanoseconds, 5);
}

TEST(LinuxClock, RawMonotonicIsFixedByDefaultAndNeverBorrowsMonotonic) {
  std::optional<LinuxTimeOptions> Options{std::in_place};
  Options->Clocks = {{1, {9, 8}}, {4, {7, 6}}};
  ASSERT_FALSE(bool(linux_model::validateTimeOptions(*Options)));
  linux_model::LinuxClock Fixed(Options);
  ProcessResult R{ProcessProfile::LinuxELF64,
                  GuestArchitecture::AArch64,
                  ExecutionBackendKind::Unicorn,
                  {}};
  ASSERT_TRUE(Fixed.accountInstructions(1000, R));
  EXPECT_EQ(Fixed.elapsed(), 0u);
  EXPECT_EQ(Fixed.read(4, R)->Seconds, 7);
  EXPECT_EQ(Fixed.read(4, R)->Nanoseconds, 6);
  Options->Clocks.erase(4);
  Options->AdvanceOnIdle = true;
  linux_model::LinuxClock Missing(Options);
  ASSERT_TRUE(Missing.advanceTo(1000000000, R));
  EXPECT_FALSE(Missing.read(4, R));
  EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService);
  EXPECT_EQ(R.Diagnostic, "Linux clock 4 has no explicit linux_time input");
  EXPECT_EQ(Missing.read(1, R)->Seconds, 10);
}

TEST(LinuxClock, BootTimeRequiresAnIndependentEpochAndKeepsItsSuspendOffset) {
  std::optional<LinuxTimeOptions> Options{std::in_place};
  Options->Clocks = {
      {0, {100, 999999999}}, {1, {10, 999999999}}, {4, {30, 999999999}}};
  linux_model::LinuxClock Clock(Options);
  ProcessResult R{ProcessProfile::LinuxELF64,
                  GuestArchitecture::AArch64,
                  ExecutionBackendKind::Unicorn,
                  {}};
  ASSERT_FALSE(bool(linux_model::validateTimeOptions(*Options)));
  EXPECT_FALSE(Clock.read(7, R));
  EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService);
  EXPECT_EQ(R.Diagnostic, "Linux clock 7 has no explicit linux_time input");

  Options->Clocks.emplace(7, LinuxTimespec{50, 999999999});
  ASSERT_FALSE(bool(linux_model::validateTimeOptions(*Options)));
  ASSERT_TRUE(Clock.accountInstructions(1000, R));
  EXPECT_EQ(Clock.elapsed(), 0u);
  EXPECT_EQ(Clock.read(7, R)->Seconds, 50);
  EXPECT_EQ(Clock.read(7, R)->Nanoseconds, 999999999);
  Options->AdvanceOnIdle = true;
  Options->NanosecondsPerInstruction = 3;
  ASSERT_TRUE(Clock.accountInstructions(1, R));
  ASSERT_TRUE(Clock.advanceTo(1000000005, R));
  ASSERT_TRUE(Clock.accountInstructions(1, R));
  EXPECT_EQ(Clock.elapsed(), 1000000005u);
  EXPECT_EQ(Clock.read(7, R)->Seconds, 52);
  EXPECT_EQ(Clock.read(7, R)->Nanoseconds, 4);
  EXPECT_EQ(Clock.read(1, R)->Seconds, 12);
  EXPECT_EQ(Clock.read(4, R)->Seconds, 32);
  EXPECT_EQ(Clock.read(0, R)->Seconds, 102);
  ASSERT_TRUE(Clock.accountInstructions(2, R));
  EXPECT_EQ(Clock.read(7, R)->Nanoseconds, 7);
  EXPECT_EQ(Clock.read(7, R)->Seconds - Clock.read(1, R)->Seconds, 40);
}

TEST(LinuxClock, RawMonotonicAdmissionDoesNotEnableOtherClockPolicies) {
  for (int ID : {3, 5, 6, 8, 9, 11}) {
    SCOPED_TRACE(ID);
    LinuxTimeOptions Options;
    Options.Clocks[ID] = {3, 4};
    ASSERT_FALSE(bool(linux_model::validateTimeOptions(Options)));
    Options.AdvanceOnIdle = true;
    auto Idle = linux_model::validateTimeOptions(Options);
    ASSERT_TRUE(bool(Idle));
    EXPECT_EQ(llvm::toString(std::move(Idle)), linux_model::TimeAdvanceClock);
    Options.AdvanceOnIdle = false;
    Options.NanosecondsPerInstruction = 1;
    auto Instructions = linux_model::validateTimeOptions(Options);
    ASSERT_TRUE(bool(Instructions));
    EXPECT_EQ(llvm::toString(std::move(Instructions)),
              linux_model::TimeAdvanceClock);
  }
}
} // namespace
} // namespace neverd::emulation
