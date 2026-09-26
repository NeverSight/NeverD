//===- KernelInterruptsTests.cpp - Interrupt identity and lock tests -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Exercise resource matching, independent pulses, opaque identities and the
/// common lock used by real ISR, synchronized callbacks and manual sections.
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"
#include "windows/KernelInterrupts.h"

namespace neverd::emulation {
namespace {
class KernelInterruptTest : public ::testing::Test {
protected:
  static constexpr uint64_t PDO = 0x2000, OtherPDO = 0x3000;
  static constexpr uint64_t Object = 0x100000, OtherObject = 0x100100;
  DriverResult Result;
  std::unique_ptr<KernelInterrupts> Model;
  KernelResources Resources{[this](uint64_t Owner) {
    return Model->canRelease(Owner);
  }};

  void ok(llvm::Error Error) {
    if (Error)
      ADD_FAILURE() << llvm::toString(std::move(Error));
  }
  template <typename T> T take(llvm::Expected<T> Value) {
    if (!Value) {
      ADD_FAILURE() << llvm::toString(Value.takeError());
      return {};
    }
    return std::move(*Value);
  }
  void reject(llvm::Error Error, llvm::StringRef Text) {
    ASSERT_TRUE(bool(Error));
    const auto Message = llvm::toString(std::move(Error));
    EXPECT_NE(Message.find(Text.str()), std::string::npos) << Message;
  }
  template <typename T>
  void reject(llvm::Expected<T> Value, llvm::StringRef Text) {
    ASSERT_FALSE(bool(Value));
    reject(Value.takeError(), Text);
  }
  DriverPnpDevice configuration(unsigned Index) {
    DriverPnpDevice Device;
    Device.ID = "irq" + std::to_string(Index);
    Device.Bus = DriverBusKind::RegisterBank;
    Device.InitialDevicePower = DevicePowerState::D0;
    Device.InitialSystemPower = SystemPowerState::Working;
    DriverInterruptResource Interrupt;
    Interrupt.ID = "line0";
    Interrupt.RawVector = 17 + Index;
    Interrupt.RawLevel = 7;
    Interrupt.RawAffinity = 1;
    Interrupt.TranslatedVector = 0x91 + Index;
    Interrupt.TranslatedLevel = 5 + Index % 2;
    Interrupt.TranslatedAffinity = 1;
    Device.Interrupts.push_back(Interrupt);
    return Device;
  }
  void start(uint64_t Owner = PDO) {
    ok(Resources.beginStart(Owner));
    ok(Resources.completeLowerStart(Owner, 0));
    ok(Resources.finishPnp(Owner, DevicePnpRequest::Start, 0));
  }
  KernelInterrupts::Connection connect(unsigned Index = 0,
                                       uint32_t Version = 0) {
    const uint64_t Owner = Index ? OtherPDO : PDO;
    auto Candidate = take(Model->match(Owner, 0x91 + Index,
                                       uint8_t(5 + Index), 1));
    Candidate.Object = Index ? OtherObject : Object;
    Candidate.Routine = 0x180001000 + Index * 0x100;
    Candidate.Context = 0x500000 + Index * 0x100;
    Candidate.Version = Version;
    ok(Model->connect(Candidate));
    return Candidate;
  }
  void arm(uint64_t Delay = 7, uint64_t Now = 10, size_t Source = 0,
           llvm::StringRef DeviceID = "irq0") {
    const DriverInterruptEvent Event{Delay, DeviceID.str(), "line0"};
    ok(Model->arm({Event}, Source, Now));
  }
  KernelInterrupts::Delivery queue(uint64_t Now = 17) {
    auto Delivery = take(Model->queueNextDue(Now));
    EXPECT_TRUE(Delivery);
    return Delivery ? std::move(*Delivery) : KernelInterrupts::Delivery{};
  }
  void SetUp() override {
    Model = std::make_unique<KernelInterrupts>(Resources, Result);
    ok(Resources.configure(PDO, configuration(0)));
    ok(Resources.configure(OtherPDO, configuration(1)));
    start();
    start(OtherPDO);
  }
};

TEST_F(KernelInterruptTest, MatchingRequiresExactTranslatedTupleAndAssignment) {
  const auto Candidate = take(Model->match(0, 0x91, 5, 1));
  EXPECT_EQ(Candidate.PDO, PDO);
  EXPECT_EQ(Candidate.Epoch, Resources.find(PDO)->Epoch);
  EXPECT_EQ(Candidate.ResourceIndex, 0u);
  EXPECT_EQ(Candidate.IRQL, 5u);
  reject(Model->match(PDO, 17, 7, 1), "translated");
  reject(Model->match(PDO, 0x91, 6, 1), "translated");
  reject(Model->match(PDO, 0x91, 5, 2), "translated");
  reject(Model->match(OtherPDO, 0x91, 5, 1), "translated");
  ok(Resources.finishPnp(PDO, DevicePnpRequest::Stop, 0));
  reject(Model->match(PDO, 0x91, 5, 1), "assigned");
  ok(Resources.beginStart(PDO));
  reject(Model->match(PDO, 0x91, 5, 1), "assigned");
  ok(Resources.completeLowerStart(PDO, 0));
  EXPECT_EQ(take(Model->match(PDO, 0x91, 5, 1)).PDO, PDO);
}

TEST_F(KernelInterruptTest, LineBasedRequiresOneLineOnItsExplicitProvider) {
  EXPECT_EQ(take(Model->match(PDO, 0, 0, 0, true)).IRQL, 5u);
  auto Device = configuration(2);
  auto Second = Device.Interrupts.front();
  Second.ID = "line1";
  ++Second.TranslatedVector;
  Device.Interrupts.push_back(Second);
  ok(Resources.configure(0x4000, Device));
  start(0x4000);
  reject(Model->match(0x4000, 0, 0, 0, true), "one assigned");
  EXPECT_EQ(take(Model->match(0x4000, 0x93, 5, 1)).ResourceIndex, 0u);
}

TEST_F(KernelInterruptTest, DuplicateAndStaleCandidatesDoNotPublishTokens) {
  auto Candidate = connect();
  reject(Model->match(PDO, 0x91, 5, 1), "already connected");
  Candidate.Object += 32;
  reject(Model->connect(Candidate), "already connected");
  EXPECT_EQ(Model->connection(Candidate.Object), nullptr);
  ok(Model->disconnect(Object, 0));
  ok(Resources.finishPnp(PDO, DevicePnpRequest::Stop, 0));
  start();
  reject(Model->connect(Candidate), "identity");
  Candidate.Epoch = Resources.find(PDO)->Epoch;
  ok(Model->connect(Candidate));
}

TEST_F(KernelInterruptTest, InvalidCandidateDoesNotReserveAnOtherwiseValidToken) {
  auto Candidate = take(Model->match(PDO, 0x91, 5, 1));
  Candidate.Object = Object;
  Candidate.Context = 0x500000;
  reject(Model->connect(Candidate), "identity");
  EXPECT_EQ(Model->connection(Object), nullptr);
  Candidate.Routine = 0x180001000;
  Candidate.IRQL = 6;
  reject(Model->connect(Candidate), "identity");
  EXPECT_EQ(Model->connection(Object), nullptr);
  Candidate.IRQL = 5;
  ok(Model->connect(Candidate));
  EXPECT_NE(Model->connection(Object), nullptr);
}

TEST_F(KernelInterruptTest, ConnectionCapacityRejectionDoesNotConsumeCandidate) {
  std::vector<KernelInterrupts::Connection> Candidates;
  for (size_t I = 0; I <= DriverScenarioInterruptLimit; ++I) {
    const uint64_t Owner = 0x4000 + I * 0x100;
    auto Device = configuration(unsigned(I + 2));
    ok(Resources.configure(Owner, Device));
    start(Owner);
    const auto &Interrupt = Device.Interrupts.front();
    auto Candidate = take(Model->match(Owner, Interrupt.TranslatedVector,
                                       uint8_t(Interrupt.TranslatedLevel), 1));
    Candidate.Object = 0x200000 + I * 32;
    Candidate.Routine = 0x180001000;
    if (I < DriverScenarioInterruptLimit)
      ok(Model->connect(Candidate));
    Candidates.push_back(Candidate);
  }
  reject(Model->connect(Candidates.back()), "capacity");
  EXPECT_EQ(Model->connection(Candidates.back().Object), nullptr);
  ok(Model->disconnect(Candidates.front().Object, 0));
  ok(Model->connect(Candidates.back()));
  EXPECT_NE(Model->connection(Candidates.back().Object), nullptr);
}

TEST_F(KernelInterruptTest, OpaqueExtentPersistsAfterDisconnectAndNeverReusesToken) {
  auto Candidate = connect(0, 4);
  ok(Model->validateGuestAccess(Object - 4, 4));
  ok(Model->validateGuestAccess(Object + interrupts::TokenSize, 1));
  reject(Model->validateGuestAccess(Object - 1, 2), "opaque");
  reject(Model->validateGuestAccess(Object, 1), "opaque");
  reject(Model->validateGuestAccess(Object + interrupts::TokenSize - 1, 1),
         "opaque");
  reject(Model->disconnect(Object, 0), "version");
  EXPECT_NE(Model->connection(Object), nullptr);
  ok(Model->disconnect(Object, 4));
  reject(Model->validateGuestAccess(Object, 16), "opaque");
  reject(Model->connect(Candidate), "cannot be reused");
  reject(Model->disconnect(Object, 4), "live connection");
}

TEST_F(KernelInterruptTest, ContextStorageCannotRetireUntilItsConnectionEnds) {
  const auto Candidate = connect();
  ok(Model->canReleaseRange(Candidate.Context - 16, 16));
  ok(Model->canReleaseRange(Candidate.Context + 1, 16));
  ok(Model->canReleaseRange(Candidate.Context, 0));
  reject(Model->canReleaseRange(Candidate.Context, 1), "interrupt context");
  reject(Model->canReleaseRange(Candidate.Context - 16, 32),
         "interrupt context");
  reject(Model->canReleaseRange(UINT64_MAX - 3, 4), "overflowing");
  EXPECT_NE(Model->connection(Object), nullptr);
  ok(Model->disconnect(Object, 0));
  ok(Model->canReleaseRange(Candidate.Context - 16, 32));
}

TEST_F(KernelInterruptTest, ProvenOutputDeviceExtentProtectsEvenNullContext) {
  auto Candidate = take(Model->match(PDO, 0x91, 5, 1));
  Candidate.Object = Object;
  Candidate.Routine = 0x180001000;
  Candidate.Context = 0;
  Candidate.OutputDeviceBase = 0x700000;
  Candidate.OutputDeviceSize = 0x180;
  ok(Model->connect(Candidate));
  ok(Model->canReleaseRange(0, 8));
  ok(Model->canReleaseRange(0x6fffff, 1));
  ok(Model->canReleaseRange(0x700180, 1));
  reject(Model->canReleaseRange(0x700000, 0x180), "device storage");
  reject(Model->canReleaseRange(0x700010, 8), "device storage");
  reject(Model->canReleaseRange(0x6fffff, 2), "device storage");
  ok(Model->disconnect(Object, 0));
  ok(Model->canReleaseRange(0x700000, 0x180));
}

TEST_F(KernelInterruptTest, StorageGuardUsesOnlyExactBorrowedFactsAndLiveOwners) {
  const auto First = connect();
  auto Second = take(Model->match(OtherPDO, 0x92, 6, 1));
  Second.Object = OtherObject;
  Second.Routine = 0x180002000;
  Second.Context = First.Context;
  Second.OutputDeviceBase = 0x800000;
  reject(Model->connect(Second), "output device extent");
  Second.OutputDeviceSize = 16;
  Second.OutputDeviceBase = UINT64_MAX - 7;
  reject(Model->connect(Second), "output device extent");
  Second.OutputDeviceBase = 0;
  Second.OutputDeviceSize = 0;
  ok(Model->connect(Second));
  // The PDO address and unrelated FDO-like storage are not inferred owners.
  ok(Model->canReleaseRange(PDO, 16));
  ok(Model->canReleaseRange(0x800000, 16));
  ok(Model->disconnect(Object, 0));
  reject(Model->canReleaseRange(First.Context, 1), "interrupt context");
  ok(Model->disconnect(OtherObject, 0));
  ok(Model->canReleaseRange(First.Context, 1));
}

TEST_F(KernelInterruptTest, PulseSurvivesSourceIrpAndCapturesIndependentIdentity) {
  const auto Connection = connect();
  Result.Requests.emplace_back();
  Result.Requests.back().IRP = 0xdeadbeef;
  arm(7, 10, 0);
  Result.Requests.back().Completed = true;
  Result.Requests.clear();
  EXPECT_TRUE(Model->hasPendingEvents());
  EXPECT_EQ(Model->nextEventTime(), 17u);
  EXPECT_FALSE(take(Model->queueNextDue(16)));
  const auto Delivery = queue();
  EXPECT_EQ(Delivery.PDO, PDO);
  EXPECT_EQ(Delivery.IRQL, 5u);
  EXPECT_EQ(Delivery.Call.Token.Owner, GuestCallOwner::Interrupt);
  EXPECT_EQ(Delivery.Call.PC, Connection.Routine);
  EXPECT_EQ(Delivery.Call.Arguments,
            (std::vector<uint64_t>{Object, Connection.Context}));
  EXPECT_FALSE(Model->nextEventTime());
  EXPECT_TRUE(Model->hasPendingEvents());
  ASSERT_EQ(Result.Interrupts.size(), 1u);
  EXPECT_EQ(Result.Interrupts[0].SourceRequestIndex, 0u);
  EXPECT_EQ(Result.Interrupts[0].OccurredAt100ns, 17u);
  EXPECT_FALSE(Result.Interrupts[0].DeliveredAt100ns);
  EXPECT_EQ(take(Model->beginCall(Delivery.Call.Token.ID, 0, 18)), 5u);
  auto Return = take(Model->finishCall(Delivery.Call.Token.ID, 1, 5, 19));
  EXPECT_EQ(Return.Value, 1u);
  EXPECT_EQ(Return.RestoredIRQL, 0u);
  EXPECT_EQ(Result.Interrupts[0].DeliveredAt100ns, 18u);
  EXPECT_EQ(Result.Interrupts[0].ReturnedAt100ns, 19u);
  EXPECT_FALSE(Model->hasPendingEvents());
}

TEST_F(KernelInterruptTest, BooleanUsesOnlyLowAlIncludingNoncanonicalTrue) {
  connect();
  for (uint64_t Value : {0x1122334455667700ULL, 0xffffffffffffffa5ULL}) {
    arm(0, 0);
    const auto Delivery = queue(0);
    take(Model->beginCall(Delivery.Call.Token.ID, 2, 0));
    auto Return = take(Model->finishCall(Delivery.Call.Token.ID, Value, 5, 0));
    EXPECT_EQ(Return.Value, uint8_t(Value));
    EXPECT_EQ(Return.RestoredIRQL, 2u);
    EXPECT_EQ(Result.Interrupts.back().ReturnValue, uint8_t(Value));
  }
}

TEST_F(KernelInterruptTest, DisconnectNeverRetargetsAnAlreadyArmedPulse) {
  auto Candidate = connect();
  arm();
  ok(Model->disconnect(Object, 0));
  reject(Model->canRelease(PDO), "explicit interrupt event");
  Candidate.Object += 32;
  ok(Model->connect(Candidate));
  reject(Model->queueNextDue(17), "original interrupt connection");
  ASSERT_EQ(Result.Interrupts.size(), 1u);
  EXPECT_EQ(Result.Interrupts[0].OccurredAt100ns, 17u);
  EXPECT_TRUE(Result.Interrupts[0].UndeliveredReason);
  EXPECT_FALSE(Result.Interrupts[0].DeliveredAt100ns);
  EXPECT_FALSE(Result.Interrupts[0].ReturnValue);
  EXPECT_NE(Model->connection(Candidate.Object), nullptr);
}

TEST_F(KernelInterruptTest, DueD3PulseRecordsPreciseUndeliveredFailure) {
  connect();
  arm(0, 10);
  Resources.setPhysicalPower(PDO, DevicePowerState::D3);
  EXPECT_EQ(take(Model->dueCount(10)), 1u);
  reject(Model->queueNextDue(10), "power D0");
  EXPECT_TRUE(Result.Interrupts[0].UndeliveredReason);
  EXPECT_FALSE(Result.Interrupts[0].ReturnValue);
  EXPECT_FALSE(Result.Interrupts[0].DeliveredAt100ns);
}

TEST_F(KernelInterruptTest, SurpriseInvalidatesArmedPulseBeforeGuestEntry) {
  connect();
  arm(1, 10);
  Resources.surpriseRemoval(PDO);
  reject(Model->queueNextDue(11), "resource epoch");
  EXPECT_TRUE(Result.Interrupts[0].UndeliveredReason);
  EXPECT_FALSE(Result.Interrupts[0].DeliveredAt100ns);
}

TEST_F(KernelInterruptTest, ReleaseGuardKeepsEventsAndConnectionsInsideTheirEpoch) {
  connect();
  const uint64_t Epoch = Resources.find(PDO)->Epoch;
  arm();
  reject(Resources.finishPnp(PDO, DevicePnpRequest::Stop, 0), "connected");
  ok(Model->disconnect(Object, 0));
  reject(Resources.finishPnp(PDO, DevicePnpRequest::Stop, 0), "event");
  EXPECT_TRUE(Resources.find(PDO)->Assigned);
  EXPECT_EQ(Resources.find(PDO)->Epoch, Epoch);
}

TEST_F(KernelInterruptTest, SameDeadlineOrderingUsesAdmissionIndexWithoutLosingPulses) {
  connect();
  connect(1);
  const DriverInterruptEvent Inputs[]{{9, "irq0", "line0"},
                                      {5, "irq1", "line0"},
                                      {5, "irq0", "line0"}};
  ok(Model->arm(Inputs, 7, 0));
  EXPECT_EQ(take(Model->dueCount(5)), 2u);
  auto First = queue(5);
  auto Second = queue(5);
  EXPECT_EQ(First.PDO, OtherPDO);
  EXPECT_EQ(Second.PDO, PDO);
  EXPECT_EQ(Model->nextEventTime(), 9u);
  EXPECT_EQ(take(Model->dueCount(5)), 0u);
  take(Model->beginCall(First.Call.Token.ID, 0, 5));
  take(Model->finishCall(First.Call.Token.ID, 1, 6, 5));
  take(Model->beginCall(Second.Call.Token.ID, 0, 5));
  take(Model->finishCall(Second.Call.Token.ID, 1, 5, 5));
  auto Last = queue(9);
  EXPECT_EQ(Last.PDO, PDO);
  EXPECT_EQ(Result.Interrupts[0].EventIndex, 0u);
  EXPECT_EQ(Result.Interrupts[1].EventIndex, 1u);
  EXPECT_EQ(Result.Interrupts[2].EventIndex, 2u);
}

TEST_F(KernelInterruptTest, InvalidArmBatchAndDeadlineOverflowAreAtomic) {
  connect();
  const DriverInterruptEvent Batch[]{{7, "irq0", "line0"},
                                     {8, "unknown", "line0"}};
  reject(Model->arm(Batch, 0, 10), "unknown");
  EXPECT_TRUE(Result.Interrupts.empty());
  EXPECT_FALSE(Model->hasPendingEvents());
  reject(Model->arm({DriverInterruptEvent{9, "irq0", "line0"}}, 0,
                    UINT64_MAX - 8),
         "overflows");
  EXPECT_TRUE(Result.Interrupts.empty());
  reject(Model->arm({DriverInterruptEvent{0, "irq0", "line0"}},
                    uint64_t(UINT32_MAX) + 1, 0),
         "capacity");
  EXPECT_FALSE(Model->nextEventTime());
}

TEST_F(KernelInterruptTest, EventCapacityIsCheckedBeforeAnyObservationMutation) {
  connect();
  std::vector<DriverInterruptEvent> Batch(
      DriverScenarioInterruptEventsPerRequestLimit + 1,
      DriverInterruptEvent{0, "irq0", "line0"});
  reject(Model->arm(Batch, 0, 0), "capacity");
  EXPECT_TRUE(Result.Interrupts.empty());
  Batch.pop_back();
  for (size_t I = 0; I < DriverScenarioInterruptEventLimit / Batch.size(); ++I)
    ok(Model->arm(Batch, I, 0));
  ASSERT_EQ(Result.Interrupts.size(), DriverScenarioInterruptEventLimit);
  reject(Model->arm({Batch.front()}, 0, 0), "capacity");
  EXPECT_EQ(Result.Interrupts.size(), DriverScenarioInterruptEventLimit);
}

TEST_F(KernelInterruptTest, ManualLocksAreNonrecursiveAndRequireLifoOwnerAndIrql) {
  connect();
  connect(1);
  EXPECT_EQ(take(Model->acquire(Object, 77, 0)), 0u);
  reject(Model->acquire(Object, 77, 5), "nonrecursive");
  reject(Model->acquire(Object, 88, 5), "nonrecursive");
  EXPECT_EQ(take(Model->acquire(OtherObject, 77, 5)), 5u);
  reject(Model->release(Object, 77, 0, 5), "owning execution");
  reject(Model->release(OtherObject, 88, 5, 6), "owning execution");
  reject(Model->release(OtherObject, 77, 0, 6), "owning execution");
  reject(Model->release(OtherObject, 77, 5, 5), "owning execution");
  reject(Model->validateExecutionReturn(77), "retains");
  ok(Model->validateExecutionReturn(88));
  EXPECT_EQ(take(Model->release(OtherObject, 77, 5, 6)), 5u);
  EXPECT_EQ(take(Model->release(Object, 77, 0, 5)), 0u);
  ok(Model->validateExecutionReturn(77));
  reject(Model->acquire(Object, 77, 6), "exceeds");
}

TEST_F(KernelInterruptTest, NestedDifferentInterruptSyncRestoresEachCallerIrql) {
  connect();
  connect(1);
  auto Outer = take(Model->synchronize(Object, 0x180003000, 0x7000));
  EXPECT_EQ(Outer.Arguments, std::vector<uint64_t>{0x7000});
  EXPECT_EQ(Outer.Token.Owner, GuestCallOwner::Interrupt);
  EXPECT_EQ(take(Model->beginCall(Outer.Token.ID, 0, 10)), 5u);
  auto Inner = take(Model->synchronize(OtherObject, 0x180003100, 0x8000));
  EXPECT_EQ(take(Model->beginCall(Inner.Token.ID, 5, 10)), 6u);
  reject(Model->finishCall(Outer.Token.ID, 1, 5, 10), "lock or IRQL");
  auto InnerReturn = take(Model->finishCall(Inner.Token.ID, 0, 6, 10));
  EXPECT_EQ(InnerReturn.Value, 0u);
  EXPECT_EQ(InnerReturn.RestoredIRQL, 5u);
  auto OuterReturn = take(Model->finishCall(Outer.Token.ID, 1, 5, 10));
  EXPECT_EQ(OuterReturn.RestoredIRQL, 0u);
  EXPECT_TRUE(Result.Interrupts.empty());
  ok(Model->disconnect(Object, 0));
  ok(Model->disconnect(OtherObject, 0));
}

TEST_F(KernelInterruptTest, SynchronizedCallbackUsesTheSameNonrecursiveLock) {
  connect();
  auto Outer = take(Model->synchronize(Object, 0x180003000, 0));
  take(Model->beginCall(Outer.Token.ID, 0, 0));
  auto Inner = take(Model->synchronize(Object, 0x180003100, 0));
  reject(Model->beginCall(Inner.Token.ID, 5, 0), "nonrecursive");
  reject(Model->acquire(Object, 99, 5), "nonrecursive");
  EXPECT_EQ(take(Model->finishCall(Outer.Token.ID, 1, 5, 0)).RestoredIRQL, 0u);
  EXPECT_EQ(take(Model->beginCall(Inner.Token.ID, 0, 0)), 5u);
  take(Model->finishCall(Inner.Token.ID, 0, 5, 0));
  ok(Model->disconnect(Object, 0));
}

TEST_F(KernelInterruptTest, FailedCallbackEntryAndReturnDoNotConsumeContinuation) {
  connect();
  arm();
  const auto Delivery = queue();
  const auto Token = Delivery.Call.Token.ID;
  reject(Model->finishCall(Token, 1, 5, 17), "lock or IRQL");
  reject(Model->beginCall(Token, 6, 17), "exceeds");
  EXPECT_FALSE(Result.Interrupts[0].DeliveredAt100ns);
  EXPECT_EQ(take(Model->beginCall(Token, 2, 18)), 5u);
  reject(Model->beginCall(Token, 2, 18), "prepared continuation");
  reject(Model->finishCall(Token, 1, 6, 19), "lock or IRQL");
  EXPECT_FALSE(Result.Interrupts[0].ReturnedAt100ns);
  EXPECT_EQ(take(Model->finishCall(Token, 1, 5, 20)).RestoredIRQL, 2u);
  reject(Model->finishCall(Token, 1, 5, 20), "lock or IRQL");
}

TEST_F(KernelInterruptTest, ManualHoldDefersIsrEntryWithoutLosingPreparedPulse) {
  connect();
  take(Model->acquire(Object, 7, 0));
  arm(0, 0);
  const auto Delivery = queue(0);
  reject(Model->beginCall(Delivery.Call.Token.ID, 0, 0), "nonrecursive");
  EXPECT_FALSE(Result.Interrupts[0].DeliveredAt100ns);
  EXPECT_EQ(take(Model->release(Object, 7, 0, 5)), 0u);
  EXPECT_EQ(take(Model->beginCall(Delivery.Call.Token.ID, 0, 1)), 5u);
  take(Model->finishCall(Delivery.Call.Token.ID, 1, 5, 1));
  EXPECT_EQ(Result.Interrupts[0].DeliveredAt100ns, 1u);
}

TEST_F(KernelInterruptTest, CallbackCannotReturnAcrossALeakedManualHold) {
  connect();
  connect(1);
  const auto Call = take(Model->synchronize(Object, 0x180002000, 0));
  take(Model->beginCall(Call.Token.ID, 0, 0));
  take(Model->acquire(OtherObject, 99, 5));
  reject(Model->finishCall(Call.Token.ID, 1, 5, 0), "lock or IRQL");
  reject(Model->validateExecutionReturn(99), "retains");
  take(Model->release(OtherObject, 99, 5, 6));
  EXPECT_EQ(take(Model->finishCall(Call.Token.ID, 1, 5, 0)).RestoredIRQL, 0u);
}

TEST_F(KernelInterruptTest, PreparedAndActiveCallbacksPreventDisconnect) {
  connect();
  auto Call = take(Model->synchronize(Object, 0x180002000, 0));
  reject(Model->disconnect(Object, 0), "owned interrupt callback");
  take(Model->beginCall(Call.Token.ID, 0, 0));
  reject(Model->disconnect(Object, 0), "owned interrupt callback");
  take(Model->finishCall(Call.Token.ID, 1, 5, 0));
  ok(Model->disconnect(Object, 0));
}

TEST_F(KernelInterruptTest, ContinuationCapacityPreflightPreservesDueObservation) {
  connect();
  constexpr size_t Limit = DriverScenarioInterruptEventLimit +
                           profile::MaxConcurrentCallbacks;
  std::vector<KernelGuestCall> Calls;
  for (size_t I = 0; I < Limit; ++I)
    Calls.push_back(take(Model->synchronize(Object, 0x180002000, 0)));
  reject(Model->synchronize(Object, 0x180002000, 0), "capacity");
  arm(0, 0);
  reject(Model->dueCount(0), "capacity");
  reject(Model->queueNextDue(0), "capacity");
  ASSERT_EQ(Result.Interrupts.size(), 1u);
  EXPECT_FALSE(Result.Interrupts[0].OccurredAt100ns);
  take(Model->beginCall(Calls.back().Token.ID, 0, 0));
  take(Model->finishCall(Calls.back().Token.ID, 1, 5, 0));
  EXPECT_EQ(take(Model->dueCount(0)), 1u);
  EXPECT_NE(queue(0).Call.Token.ID, Calls.back().Token.ID);
}
} // namespace
} // namespace neverd::emulation
