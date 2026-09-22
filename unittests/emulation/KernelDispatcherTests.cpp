//===- KernelDispatcherTests.cpp - Dispatcher object contracts ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Test guest ABI boundaries, object lifetime and scheduler integration.
///
//===----------------------------------------------------------------------===//

#include "GuestMemory.h"
#include "gtest/gtest.h"
#include "windows/KernelDispatcher.h"

#include <algorithm>
#include <initializer_list>
#include <limits>

namespace neverd::emulation {
namespace {

llvm::Error failure(const char *Message) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(), Message);
}

void success(llvm::Error E) {
  if (E)
    ADD_FAILURE() << llvm::toString(std::move(E));
}

template <class T> T take(llvm::Expected<T> Result) {
  if (!Result) {
    ADD_FAILURE() << llvm::toString(Result.takeError());
    return {};
  }
  return std::move(*Result);
}

template <class T>
void expectError(llvm::Expected<T> Result, llvm::StringRef Text) {
  ASSERT_FALSE(bool(Result));
  const std::string Message = llvm::toString(Result.takeError());
  EXPECT_NE(Message.find(Text.str()), std::string::npos) << Message;
}

void expectError(llvm::Error E, llvm::StringRef Text) {
  ASSERT_TRUE(bool(E));
  const std::string Message = llvm::toString(std::move(E));
  EXPECT_NE(Message.find(Text.str()), std::string::npos) << Message;
}

class DispatcherMemory final : public GuestMemory {
public:
  static constexpr uint64_t Base = 0x100000;
  std::vector<uint8_t> Bytes = std::vector<uint8_t>(0x20000, 0xff);
  bool DenyWrites = false;

  llvm::Error map(uint64_t, uint64_t, unsigned) override {
    return failure("test memory already mapped");
  }
  llvm::Error protect(uint64_t, uint64_t, unsigned) override {
    return failure("test memory permissions are fixed");
  }
  llvm::Error read(uint64_t Address,
                   llvm::MutableArrayRef<uint8_t> Output) override {
    if (Address < Base || Address - Base > Bytes.size() ||
        Output.size() > Bytes.size() - (Address - Base))
      return failure("test memory read outside mapping");
    std::copy_n(Bytes.begin() + (Address - Base), Output.size(),
                Output.begin());
    return llvm::Error::success();
  }
  llvm::Error write(uint64_t Address, llvm::ArrayRef<uint8_t> Input) override {
    if (DenyWrites)
      return failure("test memory write denied");
    if (Address < Base || Address - Base > Bytes.size() ||
        Input.size() > Bytes.size() - (Address - Base))
      return failure("test memory write outside mapping");
    std::copy(Input.begin(), Input.end(), Bytes.begin() + (Address - Base));
    return llvm::Error::success();
  }
};

class DriverKernelDispatcher : public ::testing::Test {
protected:
  static constexpr uint64_t Event = DispatcherMemory::Base;
  static constexpr uint64_t Timer = Event + 64;
  static constexpr uint64_t DPC = Timer + 64;
  static constexpr uint64_t SecondDPC = DPC + 64;
  static constexpr uint64_t Routine = 0x180001000;
  static constexpr uint64_t Owner = 0x50000000;
  static constexpr uint64_t Thread = 0x51000000;
  DispatcherMemory Memory;
  KernelScheduler Scheduler;
  bool DenyAccess = false;
  KernelDispatcher Dispatcher{
      Memory, Scheduler, [this](uint64_t, uint32_t, bool) {
        return DenyAccess ? failure("storage lifetime denied")
                          : llvm::Error::success();
      }};

  void SetUp() override { success(Dispatcher.configure(Owner, Thread)); }

  uint64_t call(const char *Name, std::initializer_list<uint64_t> Args,
                uint8_t IRQL = 0) {
    return take(Dispatcher.call(Name, Args, IRQL));
  }

  void initializeDPC(uint64_t Address = DPC) {
    call("KeInitializeDpc", {Address, Routine, 0xabcdef});
  }

  KernelScheduler::Invocation next() {
    auto Result = take(Scheduler.next());
    if (!Result) {
      ADD_FAILURE() << "expected a callback";
      return {};
    }
    return std::move(*Result);
  }
};

TEST_F(DriverKernelDispatcher, DPCABIAndDuplicateQueueKeepOriginalArguments) {
  initializeDPC();
  EXPECT_EQ(call("KeInsertQueueDpc", {DPC, 0x1122334455667788, 0xffee}), 1u);
  EXPECT_EQ(call("KeInsertQueueDpc", {DPC, 1, 2}), 0u);
  EXPECT_FALSE(Dispatcher.isWaitable(DPC));
  auto Invocation = next();
  EXPECT_EQ(Invocation.Object, DPC);
  EXPECT_EQ(Invocation.Owner, Owner);
  EXPECT_EQ(Invocation.Thread, Thread);
  EXPECT_EQ(Invocation.PC, Routine);
  EXPECT_EQ(Invocation.IRQL, 2u);
  EXPECT_EQ(Invocation.Arguments,
            (std::vector<uint64_t>{DPC, 0xabcdef, 0x1122334455667788, 0xffee}));
  EXPECT_EQ(call("KeRemoveQueueDpc", {DPC}), 0u);
  // Dequeue permits requeue while preserving the running invocation.
  EXPECT_EQ(call("KeInsertQueueDpc", {DPC, 3, 4}), 1u);
  success(Scheduler.finish(Invocation.ID));
  Invocation = next();
  EXPECT_EQ(Invocation.Arguments[2], 3u);
  EXPECT_EQ(Invocation.Arguments[3], 4u);
  success(Scheduler.finish(Invocation.ID));
}

TEST_F(DriverKernelDispatcher, ImportanceOnlyChangesFutureQueueInsertion) {
  initializeDPC();
  initializeDPC(SecondDPC);
  call("KeInsertQueueDpc", {DPC, 0, 0});
  call("KeSetImportanceDpc", {SecondDPC, 0x123400000002});
  call("KeInsertQueueDpc", {SecondDPC, 0, 0});
  call("KeSetImportanceDpc", {DPC, 2});
  auto First = next();
  EXPECT_EQ(First.Object, SecondDPC);
  success(Scheduler.finish(First.ID));
  EXPECT_EQ(call("KeRemoveQueueDpc", {DPC}), 1u);
  EXPECT_EQ(call("KeRemoveQueueDpc", {DPC}), 0u);
}

TEST_F(DriverKernelDispatcher, NotificationAndSynchronizationEventsDiffer) {
  call("KeInitializeEvent", {Event, 0, 0});
  EXPECT_TRUE(Dispatcher.isWaitable(Event));
  EXPECT_FALSE(take(Dispatcher.tryAcquire(Event)));
  EXPECT_EQ(call("KeSetEvent", {Event, 0, 0}), 0u);
  EXPECT_TRUE(take(Dispatcher.tryAcquire(Event)));
  EXPECT_TRUE(take(Dispatcher.tryAcquire(Event)));
  EXPECT_EQ(call("KeReadStateEvent", {Event}), 1u);
  EXPECT_EQ(call("KeResetEvent", {Event}), 1u);
  EXPECT_EQ(call("KeResetEvent", {Event}), 0u);
  call("KeInitializeEvent", {Event, 1, 0x1201});
  EXPECT_EQ(call("KeReadStateEvent", {Event}), 1u);
  EXPECT_TRUE(take(Dispatcher.tryAcquire(Event)));
  EXPECT_FALSE(take(Dispatcher.tryAcquire(Event)));
  call("KeSetEvent", {Event, 0, 0});
  call("KeClearEvent", {Event});
  EXPECT_FALSE(take(Dispatcher.tryAcquire(Event)));
}

TEST_F(DriverKernelDispatcher, UnsupportedEventHandoffDoesNotSetSignal) {
  call("KeInitializeEvent", {Event, 0, 0});
  expectError(Dispatcher.call("KeSetEvent", {Event, 0, 1}, 0), "handoff");
  expectError(Dispatcher.call("KeSetEvent", {Event, 1, 0}, 0), "priority");
  EXPECT_EQ(call("KeReadStateEvent", {Event}), 0u);
  // Only the declared LONG/BOOLEAN argument bits are defined by Win64 ABI.
  call("KeSetEvent", {Event, 0x100000000, 0x100});
  EXPECT_EQ(call("KeReadStateEvent", {Event}), 1u);
}

TEST_F(DriverKernelDispatcher,
       TimerWaitUsesVirtualTimeAndConsumesOnlySyncSignal) {
  call("KeInitializeTimerEx", {Timer, 1});
  EXPECT_TRUE(Dispatcher.isWaitable(Timer));
  EXPECT_EQ(call("KeReadStateTimer", {Timer}), 0u);
  EXPECT_FALSE(take(Dispatcher.tryAcquire(Timer)));
  EXPECT_EQ(call("KeSetTimer", {Timer, uint64_t(-250), 0}), 0u);
  EXPECT_FALSE(take(Scheduler.next()));
  EXPECT_EQ(Scheduler.now100ns(), 250u);
  EXPECT_EQ(call("KeReadStateTimer", {Timer}), 1u);
  EXPECT_TRUE(take(Dispatcher.tryAcquire(Timer)));
  EXPECT_EQ(call("KeReadStateTimer", {Timer}), 0u);
  call("KeInitializeTimer", {Timer});
  call("KeSetTimer", {Timer, 300, 0});
  EXPECT_FALSE(take(Scheduler.next()));
  EXPECT_EQ(Scheduler.now100ns(), 300u);
  EXPECT_TRUE(take(Dispatcher.tryAcquire(Timer)));
  EXPECT_TRUE(take(Dispatcher.tryAcquire(Timer)));
  EXPECT_EQ(call("KeCancelTimer", {Timer}), 0u);
  EXPECT_EQ(call("KeReadStateTimer", {Timer}), 1u);
}

TEST_F(DriverKernelDispatcher,
       TimerReplacementAndCancellationAreIndependentOfSignal) {
  call("KeInitializeTimer", {Timer});
  EXPECT_EQ(call("KeSetTimerEx", {Timer, uint64_t(-100), 0, 0}), 0u);
  EXPECT_EQ(call("KeSetTimer", {Timer, uint64_t(-200), 0}), 1u);
  EXPECT_EQ(call("KeCancelTimer", {Timer}), 1u);
  EXPECT_EQ(call("KeCancelTimer", {Timer}), 0u);
  EXPECT_FALSE(take(Scheduler.next()));
  EXPECT_EQ(Scheduler.now100ns(), 0u);
  call("KeSetTimer", {Timer, 0, 0});
  EXPECT_FALSE(take(Scheduler.next()));
  EXPECT_EQ(call("KeReadStateTimer", {Timer}), 1u);
  call("KeSetTimer", {Timer, uint64_t(-10), 0});
  EXPECT_EQ(call("KeReadStateTimer", {Timer}), 0u);
  call("KeCancelTimer", {Timer});
}

TEST_F(DriverKernelDispatcher,
       ImmediateTimerSignalsInsideActiveWorkerWithoutDispatchingDPC) {
  initializeDPC();
  call("KeInitializeTimer", {Timer});
  take(Scheduler.enqueueWorkItem({Event, Owner, Thread, Routine, {}}));
  auto Worker = next();
  EXPECT_EQ(call("KeSetTimer", {Timer, 0, DPC}), 0u);
  EXPECT_EQ(call("KeReadStateTimer", {Timer}), 1u);
  EXPECT_TRUE(take(Dispatcher.tryAcquire(Timer)));
  EXPECT_EQ(Scheduler.active()->ID, Worker.ID);
  EXPECT_EQ(Scheduler.dispatchCount(), 1u);
  EXPECT_TRUE(Scheduler.isDPCQueued(DPC));
  EXPECT_EQ(Scheduler.now100ns(), 0u);
  success(Scheduler.finish(Worker.ID));
  auto Expiry = next();
  EXPECT_EQ(Expiry.Object, DPC);
  success(Scheduler.finish(Expiry.ID));
}

TEST_F(DriverKernelDispatcher,
       TimerExpiryDPCSurvivesCancelAndTimerReplacement) {
  initializeDPC();
  initializeDPC(SecondDPC);
  call("KeInitializeTimer", {Timer});
  call("KeSetTimer", {Timer, 0, DPC});
  call("KeSetImportanceDpc", {SecondDPC, 2});
  call("KeInsertQueueDpc", {SecondDPC, 0, 0});
  auto First = next();
  ASSERT_EQ(First.Object, SecondDPC);
  EXPECT_EQ(call("KeCancelTimer", {Timer}), 0u);
  call("KeSetTimer", {Timer, uint64_t(-100), SecondDPC});
  call("KeCancelTimer", {Timer});
  expectError(Dispatcher.prepareReleaseRange(Timer, 64), "outstanding DPC");
  success(Scheduler.finish(First.ID));
  auto Expiry = next();
  EXPECT_EQ(Expiry.Object, DPC);
  EXPECT_EQ(Expiry.SourceTimer, Timer);
  EXPECT_EQ(Expiry.Arguments, (std::vector<uint64_t>{DPC, 0xabcdef, 0, 0}));
  success(Scheduler.finish(Expiry.ID));
  success(Dispatcher.prepareReleaseRange(Timer, 64));
}

TEST_F(DriverKernelDispatcher, RemovingTimerDPCDoesNotCancelPeriodicProducer) {
  initializeDPC();
  initializeDPC(SecondDPC);
  call("KeInitializeTimer", {Timer});
  call("KeSetTimerEx", {Timer, 0, 1, DPC});
  call("KeSetImportanceDpc", {SecondDPC, 2});
  call("KeInsertQueueDpc", {SecondDPC, 0, 0});
  auto First = next();
  ASSERT_EQ(First.Object, SecondDPC);
  EXPECT_EQ(call("KeRemoveQueueDpc", {DPC}), 1u);
  EXPECT_TRUE(Scheduler.isTimerArmed(Timer));
  success(Scheduler.finish(First.ID));
  auto Expiry = next();
  EXPECT_EQ(Expiry.Object, DPC);
  EXPECT_EQ(Scheduler.now100ns(), 10000u);
  EXPECT_EQ(call("KeCancelTimer", {Timer}), 1u);
  expectError(Dispatcher.prepareReleaseRange(Timer, 64), "outstanding DPC");
  success(Scheduler.finish(Expiry.ID));
  success(Dispatcher.prepareReleaseRange(Timer, 64));
}

TEST_F(DriverKernelDispatcher, ActiveNonperiodicCallbackMayFreeItsOwnStorage) {
  initializeDPC();
  call("KeInitializeTimer", {Timer});
  call("KeSetTimer", {Timer, uint64_t(-1), DPC});
  auto Expiry = next();
  success(Dispatcher.prepareReleaseRange(Timer, 128));
  EXPECT_FALSE(Dispatcher.isWaitable(Timer));
  success(Dispatcher.validateGuestAccess(DPC, 64, true));
  EXPECT_EQ(Expiry.Arguments.front(), DPC);
  EXPECT_TRUE(Scheduler.hasOutstanding(Owner));
  success(Scheduler.finish(Expiry.ID));
  EXPECT_FALSE(Scheduler.hasOutstanding(Owner));
  expectError(Dispatcher.call("KeInsertQueueDpc", {DPC, 0, 0}, 0),
              "uninitialized");
}

TEST_F(DriverKernelDispatcher,
       RearmingPeriodicCallbackAsOneShotDoesNotEraseReleaseRestriction) {
  initializeDPC();
  call("KeInitializeTimer", {Timer});
  call("KeSetTimerEx", {Timer, uint64_t(-1), 1, DPC});
  auto Expiry = next();
  call("KeSetTimer", {Timer, uint64_t(-100), DPC});
  call("KeCancelTimer", {Timer});
  expectError(Dispatcher.prepareReleaseRange(Timer, 64), "outstanding DPC");
  success(Scheduler.finish(Expiry.ID));
  success(Dispatcher.prepareReleaseRange(Timer, 64));
}

TEST_F(DriverKernelDispatcher,
       ArmedTimerKeepsDPCAliveAndRejectsMetadataReplacement) {
  initializeDPC();
  call("KeInitializeTimer", {Timer});
  call("KeSetTimer", {Timer, uint64_t(-100), DPC});
  expectError(Dispatcher.prepareReleaseRange(DPC, 64), "armed timer");
  expectError(Dispatcher.call("KeInitializeDpc", {DPC, Routine + 1, 0}, 0),
              "armed timer");
  expectError(Dispatcher.call("KeSetImportanceDpc", {DPC, 2}, 0),
              "armed timer");
  expectError(Dispatcher.call("KeInitializeTimer", {Timer}, 0), "armed");
  call("KeCancelTimer", {Timer});
  success(Dispatcher.prepareReleaseRange(Timer, 128));
}

TEST_F(DriverKernelDispatcher,
       ReleasePreflightDoesNotPartiallyUnregisterObjects) {
  call("KeInitializeEvent", {Event, 0, 1});
  initializeDPC();
  call("KeInsertQueueDpc", {DPC, 0, 0});
  expectError(Dispatcher.prepareReleaseRange(Event, 192), "queued");
  EXPECT_TRUE(Dispatcher.isWaitable(Event));
  EXPECT_TRUE(take(Dispatcher.tryAcquire(Event)));
  expectError(Dispatcher.validateGuestAccess(Event, 1, false), "opaque");
  call("KeRemoveQueueDpc", {DPC});
  success(Dispatcher.prepareReleaseRange(Event, 192));
  EXPECT_FALSE(Dispatcher.isWaitable(Event));
  success(Dispatcher.validateGuestAccess(Event, 192, true));
}

TEST_F(DriverKernelDispatcher,
       OpaqueRangesProtectEdgesAndRejectPartialRelease) {
  call("KeInitializeEvent", {Event, 0, 1});
  expectError(Dispatcher.validateGuestAccess(Event - 1, 2, false), "opaque");
  expectError(Dispatcher.validateGuestAccess(Event + 23, 2, true), "opaque");
  success(Dispatcher.validateGuestAccess(Event - 1, 1, true));
  success(Dispatcher.validateGuestAccess(Event + 24, 1, false));
  expectError(Dispatcher.prepareReleaseRange(Event + 8, 16), "partially");
  expectError(Dispatcher.call("KeInitializeDpc", {Event + 8, Routine, 0}, 0),
              "overlap");
  EXPECT_EQ(call("KeReadStateEvent", {Event}), 1u);
  success(Dispatcher.prepareReleaseRange(Event, 24));
  expectError(Dispatcher.tryAcquire(Event), "uninitialized");
}

TEST_F(DriverKernelDispatcher,
       InitializationFailurePreservesPriorSignalAndType) {
  call("KeInitializeEvent", {Event, 0, 1});
  Memory.DenyWrites = true;
  expectError(Dispatcher.call("KeInitializeTimer", {Event}, 0), "write denied");
  EXPECT_EQ(call("KeReadStateEvent", {Event}), 1u);
  expectError(Dispatcher.call("KeReadStateTimer", {Event}, 0), "wrong type");
  Memory.DenyWrites = false;
  DenyAccess = true;
  expectError(Dispatcher.call("KeClearEvent", {Event}, 0), "lifetime");
  expectError(Dispatcher.tryAcquire(Event), "lifetime");
  DenyAccess = false;
  EXPECT_EQ(call("KeReadStateEvent", {Event}), 1u);
}

TEST_F(DriverKernelDispatcher, TimerFailuresPreserveExistingDeadlineAndDPC) {
  initializeDPC();
  call("KeInitializeTimer", {Timer});
  call("KeSetTimer", {Timer, uint64_t(-100), DPC});
  expectError(Dispatcher.call("KeSetTimerEx", {Timer, 0, 0xffffffff, DPC}, 0),
              "nonnegative");
  expectError(Dispatcher.call("KeSetTimer", {Timer, 0, SecondDPC}, 0),
              "uninitialized");
  auto Expiry = next();
  EXPECT_EQ(Scheduler.now100ns(), 100u);
  EXPECT_EQ(Expiry.Object, DPC);
  success(Scheduler.finish(Expiry.ID));
}

TEST_F(DriverKernelDispatcher, APIValidationEnforcesABIAndIRQLBeforeMutation) {
  EXPECT_EQ(KernelDispatcher::argumentCount("KeSetTimerEx"), 4u);
  EXPECT_FALSE(KernelDispatcher::argumentCount("KeUnknown"));
  expectError(Dispatcher.call("KeUnknown", {}, 0), "unknown");
  expectError(Dispatcher.call("KeInitializeEvent", {Event}, 0), "argument");
  expectError(Dispatcher.call("KeInitializeTimer", {Timer}, 3), "IRQL");
  expectError(Dispatcher.call("KeInitializeEvent", {Event, 2, 0}, 0), "type");
  expectError(Dispatcher.call("KeInitializeDpc", {DPC, 0, 0}, 0), "null");
  expectError(Dispatcher.call("KeInitializeDpc", {DPC + 1, Routine, 0}, 0),
              "misaligned");
  call("KeInitializeDpc", {DPC, Routine, 0}, 15);
  call("KeInsertQueueDpc", {DPC, 0, 0}, 15);
  call("KeRemoveQueueDpc", {DPC}, 15);
  call("KeSetTargetProcessorDpc", {DPC, 0x100}, 15);
  expectError(Dispatcher.call("KeSetTargetProcessorDpc", {DPC, 1}, 0),
              "processor zero");
  expectError(Dispatcher.call("KeSetImportanceDpc", {DPC, 4}, 0), "importance");
  call("KeInitializeEvent", {Event, 0, 0}, 15);
  expectError(Dispatcher.call("KeSetEvent", {Event, 0, 0}, 3), "IRQL");
  EXPECT_EQ(call("KeReadStateEvent", {Event}), 0u);
  expectError(Dispatcher.prepareReleaseRange(
                  std::numeric_limits<uint64_t>::max() - 8, 16),
              "range");
}

TEST_F(DriverKernelDispatcher, ObjectCountIsBoundedAndReleaseRestoresCapacity) {
  for (uint64_t I = 0; I < 1024; ++I)
    call("KeInitializeEvent", {Event + 64 * I, 0, 0});
  expectError(
      Dispatcher.call("KeInitializeEvent", {Event + 64 * 1024, 0, 0}, 0),
      "count");
  success(Dispatcher.prepareReleaseRange(Event, 24));
  call("KeInitializeEvent", {Event + 64 * 1024, 0, 0});
  EXPECT_TRUE(Dispatcher.isWaitable(Event + 64 * 1024));
}

TEST(DriverKernelDispatcherConfiguration, OwnerThreadAndValidatorAreRequired) {
  DispatcherMemory Memory;
  KernelScheduler Scheduler;
  KernelDispatcher MissingValidator(Memory, Scheduler, {});
  expectError(MissingValidator.configure(1, 2), "configuration");
  KernelDispatcher Dispatcher(Memory, Scheduler, [](uint64_t, uint32_t, bool) {
    return llvm::Error::success();
  });
  expectError(Dispatcher.call("KeInitializeTimer", {DispatcherMemory::Base}, 0),
              "configured");
  expectError(Dispatcher.configure(0, 2), "configuration");
  expectError(Dispatcher.configure(1, 0), "configuration");
  success(Dispatcher.configure(1, 2));
  expectError(Dispatcher.configure(1, 2), "configuration");
}

} // namespace
} // namespace neverd::emulation
