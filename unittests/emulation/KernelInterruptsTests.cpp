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

#include <array>

namespace neverd::emulation {
namespace {
class KernelInterruptTest : public ::testing::Test {
protected:
  static constexpr uint64_t PDO = 0x2000, OtherPDO = 0x3000;
  static constexpr uint64_t Object = 0x100000, OtherObject = 0x100100;
  DriverResult Result;
  std::unique_ptr<KernelInterrupts> Model;
  KernelResources Resources{
      [this](uint64_t Owner) { return Model->canRelease(Owner); }};

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
    auto Candidate =
        take(Model->match(Owner, 0x91 + Index, uint8_t(5 + Index), 1));
    Candidate.Object = Index ? OtherObject : Object;
    Candidate.Routine = 0x180001000 + Index * 0x100;
    Candidate.Context = 0x500000 + Index * 0x100;
    Candidate.Version = Version;
    ok(Model->connect(Candidate));
    return Candidate;
  }
  KernelInterrupts::Connection connectPassive(unsigned Index = 0) {
    auto Candidate = take(Model->match(Index ? OtherPDO : PDO, 0x91 + Index,
                                       uint8_t(5 + Index), 1));
    Candidate.Object = Index ? OtherObject : Object;
    Candidate.Routine = 0x180005000 + Index * 0x100;
    Candidate.Context = 0x600000 + Index * 0x100;
    Candidate.Version = interrupts::FullySpecified;
    Candidate.Passive = true;
    Candidate.SynchronizeIRQL = 0;
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
  std::array<KernelInterrupts::Connection, 2>
  sharedConnections(DriverInterruptMode Mode = DriverInterruptMode::Latched) {
    std::array<KernelInterrupts::Connection, 2> Values;
    for (unsigned I = 0; I < Values.size(); ++I) {
      auto Device = configuration(I + 2);
      auto &Resource = Device.Interrupts.front();
      Resource.TranslatedVector = 0xa0;
      Resource.TranslatedLevel = 7;
      Resource.Share = DriverInterruptShare::Shared;
      Resource.Mode = Mode;
      if (Mode == DriverInterruptMode::LevelSensitive)
        Resource.RetriggerAfter100ns = 3;
      const uint64_t Owner = 0x4000 + I * 0x1000;
      ok(Resources.configure(Owner, Device));
      start(Owner);
      auto Candidate = take(Model->match(Owner, 0xa0, 7, 1));
      Candidate.Object = 0x200000 + I * 0x100;
      Candidate.Routine = 0x180004000 + I * 0x100;
      Candidate.Context = 0x600000 + I * 0x100;
      ok(Model->connect(Candidate));
      Values[I] = Candidate;
    }
    return Values;
  }
  void SetUp() override {
    Model = std::make_unique<KernelInterrupts>(Resources, Result);
    ok(Resources.configure(PDO, configuration(0)));
    ok(Resources.configure(OtherPDO, configuration(1)));
    start();
    start(OtherPDO);
  }
};

TEST_F(KernelInterruptTest, SharedLatchedLineCallsEveryCapturedHandler) {
  const auto Connections = sharedConnections();
  reject(Model->match(0, 0xa0, 7, 1), "one assigned");
  arm(0, 10, 0, "irq3");
  EXPECT_EQ(take(Model->dueCount(10)), 1u);
  const auto Delivery = queue(10);
  const auto Token = Delivery.Call.Token.ID;
  EXPECT_EQ(Delivery.Call.Arguments.front(), Connections[0].Object);
  EXPECT_EQ(take(Model->beginCall(Token, 0, 10)), 7u);
  reject(Model->disconnect(Connections[1].Object, 0),
         "owned interrupt callback");
  auto First = take(Model->finishCall(Token, 0x101, 7, 10));
  ASSERT_TRUE(First.Next);
  EXPECT_EQ(First.Next->Token.ID, Token);
  EXPECT_EQ(First.Next->Arguments.front(), Connections[1].Object);
  EXPECT_EQ(First.RestoredIRQL, 0u);
  EXPECT_TRUE(Model->hasPendingEvents());
  EXPECT_EQ(take(Model->beginCall(Token, 0, 11)), 7u);
  auto Second = take(Model->finishCall(Token, 0x100, 7, 12));
  EXPECT_FALSE(Second.Next);
  EXPECT_EQ(Second.Value, 1u);
  EXPECT_FALSE(Model->hasPendingEvents());
  const auto &Observation = Result.Interrupts.back();
  ASSERT_EQ(Observation.Handlers.size(), 2u);
  EXPECT_EQ(Observation.Handlers[0].ReturnValue, 1u);
  EXPECT_EQ(Observation.Handlers[1].ReturnValue, 0u);
  EXPECT_EQ(Observation.Handlers[1].DeliveredAt100ns, 11u);
  EXPECT_EQ(Observation.ReturnedAt100ns, 12u);
  EXPECT_EQ(Observation.ReturnValue, 1u);
}

TEST_F(KernelInterruptTest, SharedPulseRejectsLostPeerWithoutRetargeting) {
  auto Connections = sharedConnections();
  arm(0, 10, 0, "irq2");
  ok(Model->disconnect(Connections[1].Object, 0));
  Connections[1].Object += 32;
  ok(Model->connect(Connections[1]));
  reject(Model->queueNextDue(10), "captured live connection");
  EXPECT_TRUE(Result.Interrupts.back().Handlers.empty());
  EXPECT_FALSE(Result.Interrupts.back().DeliveredAt100ns);
  EXPECT_FALSE(Result.Interrupts.back().ReturnValue);
}

TEST_F(KernelInterruptTest, LevelAssertionRetriggersUntilExplicitDeassertion) {
  sharedConnections(DriverInterruptMode::LevelSensitive);
  using Action = DriverInterruptAction;
  const DriverInterruptEvent Inputs[]{{0, "irq2", "line0", Action::Assert},
                                      {8, "irq2", "line0", Action::Deassert}};
  ok(Model->arm(Inputs, 0, 0));
  for (uint64_t Time : {0u, 3u, 6u}) {
    EXPECT_EQ(Model->nextEventTime(), Time);
    EXPECT_EQ(take(Model->dueCount(Time)), 1u);
    const auto Call = queue(Time).Call;
    EXPECT_EQ(take(Model->beginCall(Call.Token.ID, 0, Time)), 7u);
    auto Return = take(Model->finishCall(Call.Token.ID, 1, 7, Time));
    EXPECT_FALSE(Return.Next);
    EXPECT_EQ(Return.Value, 1u);
    EXPECT_TRUE(Model->hasPendingEvents());
  }
  EXPECT_EQ(Model->nextEventTime(), 8u);
  EXPECT_EQ(take(Model->dueCount(8)), 0u);
  EXPECT_FALSE(take(Model->queueNextDue(8)));
  EXPECT_FALSE(Model->hasPendingEvents());
  const auto &Assert = Result.Interrupts[0];
  ASSERT_EQ(Assert.Handlers.size(), 3u);
  for (size_t I = 0; I < Assert.Handlers.size(); ++I) {
    EXPECT_EQ(Assert.Handlers[I].DeliveryIndex, I);
    EXPECT_EQ(Assert.Handlers[I].DeliveredAt100ns, I * 3);
    EXPECT_EQ(Assert.Handlers[I].ReturnValue, 1u);
  }
  EXPECT_EQ(Result.Interrupts[1].OccurredAt100ns, 8u);
  EXPECT_FALSE(Result.Interrupts[1].DeliveredAt100ns);
  EXPECT_FALSE(Result.Interrupts[1].ReturnValue);
}

TEST_F(KernelInterruptTest,
       SameBoundaryDeassertionCancelsSamplingBeforeMutation) {
  sharedConnections(DriverInterruptMode::LevelSensitive);
  using Action = DriverInterruptAction;
  const DriverInterruptEvent Inputs[]{{0, "irq2", "line0", Action::Assert},
                                      {0, "irq2", "line0", Action::Deassert}};
  ok(Model->arm(Inputs, 0, 10));
  EXPECT_EQ(take(Model->dueCount(10)), 0u);
  EXPECT_FALSE(Result.Interrupts[0].OccurredAt100ns);
  EXPECT_FALSE(Result.Interrupts[1].OccurredAt100ns);
  EXPECT_FALSE(take(Model->queueNextDue(10)));
  EXPECT_FALSE(Model->hasPendingEvents());
  for (const auto &Event : Result.Interrupts) {
    EXPECT_EQ(Event.OccurredAt100ns, 10u);
    EXPECT_TRUE(Event.Handlers.empty());
    EXPECT_FALSE(Event.ReturnValue);
  }
}

TEST_F(KernelInterruptTest,
       SharedLevelUsesSourceOrWithoutTurningReassertIntoEdge) {
  const auto Connections =
      sharedConnections(DriverInterruptMode::LevelSensitive);
  using Action = DriverInterruptAction;
  const DriverInterruptEvent Inputs[]{{0, "irq2", "line0", Action::Assert},
                                      {1, "irq3", "line0", Action::Assert},
                                      {2, "irq3", "line0", Action::Assert},
                                      {2, "irq2", "line0", Action::Deassert},
                                      {5, "irq3", "line0", Action::Deassert}};
  ok(Model->arm(Inputs, 0, 0));
  auto First = queue(0).Call;
  take(Model->beginCall(First.Token.ID, 0, 0));
  take(Model->finishCall(First.Token.ID, 1, 7, 0));
  reject(Model->disconnect(Connections[0].Object, 0), "asserted");
  for (uint64_t Time : {1u, 2u}) {
    EXPECT_EQ(take(Model->dueCount(Time)), 0u);
    EXPECT_FALSE(take(Model->queueNextDue(Time)));
  }
  EXPECT_EQ(Model->nextEventTime(), 3u);
  auto Second = queue(3).Call;
  take(Model->beginCall(Second.Token.ID, 0, 3));
  auto Unclaimed = take(Model->finishCall(Second.Token.ID, 0, 7, 3));
  ASSERT_TRUE(Unclaimed.Next);
  take(Model->beginCall(Second.Token.ID, 0, 3));
  EXPECT_FALSE(take(Model->finishCall(Second.Token.ID, 1, 7, 3)).Next);
  EXPECT_FALSE(take(Model->queueNextDue(5)));
  EXPECT_FALSE(Model->hasPendingEvents());
  EXPECT_EQ(Result.Interrupts[0].Handlers.size(), 1u);
  ASSERT_EQ(Result.Interrupts[1].Handlers.size(), 2u);
  EXPECT_EQ(Result.Interrupts[1].Handlers[0].ReturnValue, 0u);
  EXPECT_EQ(Result.Interrupts[1].Handlers[1].ReturnValue, 1u);
  EXPECT_TRUE(Result.Interrupts[2].Handlers.empty());
}

TEST_F(KernelInterruptTest,
       LevelDeliveryBudgetPrecedesMutationAndAllowsDeassertion) {
  sharedConnections(DriverInterruptMode::LevelSensitive);
  using Action = DriverInterruptAction;
  ok(Model->arm({{0, "irq2", "line0", Action::Assert}}, 0, 0));
  for (uint64_t I = 0; I < DriverInterruptDeliveryLimit; ++I) {
    const uint64_t Time = I * 3;
    const auto Call = queue(Time).Call;
    take(Model->beginCall(Call.Token.ID, 0, Time));
    take(Model->finishCall(Call.Token.ID, 1, 7, Time));
  }
  const uint64_t Next = DriverInterruptDeliveryLimit * 3;
  const auto Before = Result.Interrupts.front().ReturnedAt100ns;
  reject(Model->dueCount(Next), "delivery limit");
  reject(Model->queueNextDue(Next), "delivery limit");
  EXPECT_EQ(Model->nextEventTime(), Next);
  EXPECT_EQ(Result.Interrupts.front().ReturnedAt100ns, Before);
  EXPECT_EQ(Result.Interrupts.front().Handlers.size(),
            DriverInterruptDeliveryLimit);
  ok(Model->arm({{0, "irq2", "line0", Action::Deassert}}, 1, Next));
  EXPECT_EQ(take(Model->dueCount(Next)), 0u);
  EXPECT_FALSE(take(Model->queueNextDue(Next)));
  EXPECT_FALSE(Model->hasPendingEvents());
}

TEST_F(KernelInterruptTest, LevelDeadlineOverflowDoesNotPublishDelivery) {
  sharedConnections(DriverInterruptMode::LevelSensitive);
  using Action = DriverInterruptAction;
  ok(Model->arm({{0, "irq2", "line0", Action::Assert}}, 0, UINT64_MAX - 1));
  reject(Model->dueCount(UINT64_MAX - 1), "deadline overflows");
  reject(Model->queueNextDue(UINT64_MAX - 1), "deadline overflows");
  EXPECT_FALSE(Result.Interrupts.front().OccurredAt100ns);
  EXPECT_TRUE(Result.Interrupts.front().Handlers.empty());
  EXPECT_EQ(Model->nextEventTime(), UINT64_MAX - 1);
}

TEST_F(KernelInterruptTest, LevelReturnOverflowRetainsTheCallbackAndLock) {
  const auto Connections =
      sharedConnections(DriverInterruptMode::LevelSensitive);
  ok(Model->arm({{0, "irq2", "line0", DriverInterruptAction::Assert}}, 0, 0));
  const auto Call = queue(0).Call;
  take(Model->beginCall(Call.Token.ID, 0, 0));
  reject(Model->finishCall(Call.Token.ID, 1, 7, UINT64_MAX),
         "deadline overflows");
  ASSERT_EQ(Result.Interrupts.front().Handlers.size(), 1u);
  EXPECT_FALSE(Result.Interrupts.front().Handlers.front().ReturnValue);
  EXPECT_FALSE(Result.Interrupts.front().ReturnedAt100ns);
  reject(Model->acquire(Connections.front().Object, 77, 7), "nonrecursive");
  take(Model->finishCall(Call.Token.ID, 1, 7, 0));
  EXPECT_EQ(Model->nextEventTime(), 3u);
}

TEST_F(KernelInterruptTest, CallerLockSharesOneCriticalSectionAcrossVectors) {
  constexpr uint64_t Lock = 0x900000;
  auto First = take(Model->match(PDO, 0x91, 5, 1));
  First.Object = Object;
  First.Routine = 0x180001000;
  First.SpinLock = Lock;
  First.SynchronizeIRQL = 6;
  ok(Model->connect(First));
  auto Second = take(Model->match(OtherPDO, 0x92, 6, 1));
  Second.Object = OtherObject;
  Second.Routine = 0x180002000;
  Second.SpinLock = Lock;
  ok(Model->connect(Second));
  EXPECT_TRUE(Model->usesSpinLock(Lock));
  reject(Model->canReleaseRange(Lock + 7, 1), "interrupt spin lock");
  EXPECT_EQ(take(Model->acquire(Object, 77, 0)), 0u);
  EXPECT_EQ(Model->manualHoldIRQL(77), 6u);
  reject(Model->acquire(OtherObject, 77, 6), "nonrecursive");
  auto Call = take(Model->synchronize(OtherObject, 0x180003000, 0));
  reject(Model->beginCall(Call.Token.ID, 6, 0), "nonrecursive");
  reject(Model->release(OtherObject, 77, 0, 6), "owning execution");
  EXPECT_EQ(take(Model->release(Object, 77, 0, 6)), 0u);
  EXPECT_EQ(take(Model->beginCall(Call.Token.ID, 0, 0)), 6u);
  reject(Model->acquire(Object, 77, 6), "nonrecursive");
  take(Model->finishCall(Call.Token.ID, 1, 6, 0));
  ok(Model->arm({{0, "irq0", "line0"}}, 0, 0));
  const auto Delivery = queue(0);
  EXPECT_EQ(Delivery.IRQL, 6u);
  EXPECT_EQ(Delivery.Priority, 5u);
  take(Model->beginCall(Delivery.Call.Token.ID, 0, 0));
  take(Model->finishCall(Delivery.Call.Token.ID, 1, 6, 0));
  ok(Model->disconnect(Object, 0));
  reject(Model->canReleaseRange(Lock, 8), "interrupt spin lock");
  ok(Model->disconnect(OtherObject, 0));
  ok(Model->canReleaseRange(Lock, 8));
}

TEST_F(KernelInterruptTest,
       LegacyMatchingIgnoresUnpublishedResourcesButRejectsLiveAmbiguity) {
  KernelResources Inventory{[](uint64_t) { return llvm::Error::success(); }};
  DriverResult Observations;
  KernelInterrupts Interrupts(Inventory, Observations);
  for (uint64_t Owner : {PDO, OtherPDO}) {
    auto Device = configuration(0);
    Device.ID = Owner == PDO ? "first" : "second";
    Device.Interrupts.front().Share = DriverInterruptShare::Shared;
    ok(Inventory.configure(Owner, Device));
  }
  auto Start = [&](uint64_t Owner) {
    ok(Inventory.beginStart(Owner));
    ok(Inventory.completeLowerStart(Owner, 0));
    ok(Inventory.finishPnp(Owner, DevicePnpRequest::Start, 0));
  };
  Start(PDO);
  auto Match = take(Interrupts.match(0, 0x91, 5, 1));
  EXPECT_EQ(Match.PDO, PDO);
  EXPECT_EQ(Match.Epoch, Inventory.find(PDO)->Epoch);
  reject(Interrupts.match(OtherPDO, 0x91, 5, 1), "present assigned resource");
  Start(OtherPDO);
  reject(Interrupts.match(0, 0x91, 5, 1), "one assigned interrupt");
  EXPECT_EQ(take(Interrupts.match(PDO, 0x91, 5, 1)).PDO, PDO);
  Inventory.surpriseRemoval(OtherPDO);
  EXPECT_EQ(take(Interrupts.match(0, 0x91, 5, 1)).PDO, PDO);
  reject(Interrupts.match(OtherPDO, 0x91, 5, 1), "present assigned resource");
}

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

TEST_F(KernelInterruptTest,
       InvalidCandidateDoesNotReserveAnOtherwiseValidToken) {
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

TEST_F(KernelInterruptTest,
       ConnectionCapacityRejectionDoesNotConsumeCandidate) {
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

TEST_F(KernelInterruptTest,
       OpaqueExtentPersistsAfterDisconnectAndNeverReusesToken) {
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

TEST_F(KernelInterruptTest,
       StorageGuardUsesOnlyExactBorrowedFactsAndLiveOwners) {
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

TEST_F(KernelInterruptTest,
       PulseSurvivesSourceIrpAndCapturesIndependentIdentity) {
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

TEST_F(KernelInterruptTest,
       ReleaseGuardKeepsEventsAndConnectionsInsideTheirEpoch) {
  connect();
  const uint64_t Epoch = Resources.find(PDO)->Epoch;
  arm();
  reject(Resources.finishPnp(PDO, DevicePnpRequest::Stop, 0), "connected");
  ok(Model->disconnect(Object, 0));
  reject(Resources.finishPnp(PDO, DevicePnpRequest::Stop, 0), "event");
  EXPECT_TRUE(Resources.find(PDO)->Assigned);
  EXPECT_EQ(Resources.find(PDO)->Epoch, Epoch);
}

TEST_F(KernelInterruptTest,
       SameDeadlineOrderingUsesAdmissionIndexWithoutLosingPulses) {
  connect();
  connect(1);
  const DriverInterruptEvent Inputs[]{
      {9, "irq0", "line0"}, {5, "irq1", "line0"}, {5, "irq0", "line0"}};
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
  reject(
      Model->arm({DriverInterruptEvent{9, "irq0", "line0"}}, 0, UINT64_MAX - 8),
      "overflows");
  EXPECT_TRUE(Result.Interrupts.empty());
  reject(Model->arm({DriverInterruptEvent{0, "irq0", "line0"}},
                    uint64_t(UINT32_MAX) + 1, 0),
         "capacity");
  EXPECT_FALSE(Model->nextEventTime());
}

TEST_F(KernelInterruptTest,
       EventCapacityIsCheckedBeforeAnyObservationMutation) {
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

TEST_F(KernelInterruptTest,
       ManualLocksAreNonrecursiveAndRequireLifoOwnerAndIrql) {
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

TEST_F(KernelInterruptTest,
       NestedDifferentInterruptSyncRestoresEachCallerIrql) {
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

TEST_F(KernelInterruptTest,
       FailedCallbackEntryAndReturnDoNotConsumeContinuation) {
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

TEST_F(KernelInterruptTest,
       ManualHoldDefersIsrEntryWithoutLosingPreparedPulse) {
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

TEST_F(KernelInterruptTest,
       ContinuationCapacityPreflightPreservesDueObservation) {
  connect();
  constexpr size_t Limit =
      DriverScenarioInterruptEventLimit + profile::MaxConcurrentCallbacks;
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

TEST_F(KernelInterruptTest, MessageIdsUsePdoOrderAndDisconnectIsAtomic) {
  auto Device = configuration(2);
  auto &Resource = Device.Interrupts.front();
  Resource.RawLevel = 0;
  Resource.Messages = {{0xfee01000, 0x100, 0x93, 5, 1},
                       {0xfee01000, 0x101, 0x94, 7, 1}};
  auto Second = Resource;
  Second.ID = "second";
  Second.TranslatedVector = 0x95;
  Second.TranslatedLevel = 8;
  Second.Messages = {{0xfee02000, 0x200, 0x95, 8, 1}};
  Device.Interrupts.push_back(Second);
  constexpr uint64_t Owner = 0x4000, Table = 0x300000;
  ok(Resources.configure(Owner, Device));
  start(Owner);
  auto Candidates = take(Model->matchMessages(Owner));
  ASSERT_EQ(Candidates.size(), 3u);
  for (size_t I = 0; I < Candidates.size(); ++I) {
    Candidates[I].Object = Table + 0x100 + I * interrupts::TokenSize;
    Candidates[I].Version = interrupts::MessageBased;
    Candidates[I].Routine = 0x180005000;
    Candidates[I].Context = 0x600000;
  }
  auto Incomplete = Candidates;
  Incomplete.pop_back();
  reject(Model->connectMessages(Table, Incomplete), "every assigned message");
  EXPECT_EQ(Model->connection(Candidates.front().Object), nullptr);
  ok(Model->connectMessages(Table, Candidates));
  ok(Model->validateGuestAccess(Table, interrupts::MessageTableHeaderSize));
  reject(Model->validateGuestAccess(Table, 1, true), "read-only");
  const DriverInterruptEvent Event{0, Device.ID, "second",
                                   DriverInterruptAction::Pulse, 0};
  ok(Model->arm({Event}, 1, 0));
  auto Delivery = queue(0);
  EXPECT_EQ(Delivery.Call.Arguments,
            (std::vector<uint64_t>{Candidates[2].Object, 0x600000, 2}));
  EXPECT_EQ(Delivery.IRQL, 8u);
  EXPECT_EQ(take(Model->beginCall(Delivery.Call.Token.ID, 0, 0)), 8u);
  reject(Model->disconnect(Table, interrupts::MessageBased), "owned");
  for (const auto &Candidate : Candidates)
    EXPECT_NE(Model->connection(Candidate.Object), nullptr);
  auto Returned = take(Model->finishCall(Delivery.Call.Token.ID, 0x100, 8, 0));
  EXPECT_EQ(Returned.Value, 0u);
  ASSERT_EQ(Result.Interrupts.size(), 1u);
  EXPECT_EQ(Result.Interrupts[0].MessageID, 0u);
  ASSERT_EQ(Result.Interrupts[0].Handlers.size(), 1u);
  EXPECT_EQ(Result.Interrupts[0].Handlers[0].MessageID, 2u);
  ok(Model->disconnect(Table, interrupts::MessageBased));
  reject(Model->validateGuestAccess(Table, 1), "read-only");
  for (const auto &Candidate : Candidates)
    EXPECT_EQ(Model->connection(Candidate.Object), nullptr);
}

TEST_F(KernelInterruptTest,
       MessagePrivateLocksAndCallerLockHaveDistinctOwnership) {
  auto Device = configuration(2);
  auto &Resource = Device.Interrupts.front();
  Resource.RawLevel = 0;
  Resource.Messages = {{0xfee01000, 0x100, 0x93, 5, 1},
                       {0xfee01000, 0x101, 0x94, 7, 1}};
  constexpr uint64_t Owner = 0x4000;
  ok(Resources.configure(Owner, Device));
  start(Owner);
  for (bool Shared : {false, true}) {
    const uint64_t Table = Shared ? 0x400000 : 0x300000;
    auto Candidates = take(Model->matchMessages(Owner));
    for (size_t I = 0; I < Candidates.size(); ++I) {
      auto &Candidate = Candidates[I];
      Candidate.Object = Table + 0x100 + I * interrupts::TokenSize;
      Candidate.Version = interrupts::MessageBased;
      Candidate.Routine = 0x180005000;
      if (Shared) {
        Candidate.SpinLock = 0xffff800040000000;
        Candidate.SynchronizeIRQL = 7;
      }
    }
    ok(Model->connectMessages(Table, Candidates));
    EXPECT_EQ(take(Model->acquire(Candidates[0].Object, 1, 0)), 0u);
    if (Shared)
      reject(Model->acquire(Candidates[1].Object, 2, 0), "nonrecursive");
    else {
      EXPECT_EQ(take(Model->acquire(Candidates[1].Object, 2, 5)), 5u);
      EXPECT_EQ(take(Model->release(Candidates[1].Object, 2, 5, 7)), 5u);
    }
    reject(Model->disconnect(Table, interrupts::MessageBased), "owned");
    EXPECT_EQ(take(Model->release(Candidates[0].Object, 1, 0, Shared ? 7 : 5)),
              0u);
    ok(Model->disconnect(Table, interrupts::MessageBased));
  }
}

TEST_F(KernelInterruptTest, MessageEventRetainsOriginalGroupAcrossReconnect) {
  auto Device = configuration(2);
  auto &Resource = Device.Interrupts.front();
  Resource.RawLevel = 0;
  Resource.Messages = {{0xfee01000, 0x100, 0x93, 5, 1}};
  constexpr uint64_t Owner = 0x4000;
  ok(Resources.configure(Owner, Device));
  start(Owner);
  auto Candidates = take(Model->matchMessages(Owner));
  auto &Candidate = Candidates.front();
  Candidate.Object = 0x300100;
  Candidate.Version = interrupts::MessageBased;
  Candidate.Routine = 0x180005000;
  ok(Model->connectMessages(0x300000, Candidates));
  DriverInterruptEvent Event{5, Device.ID, "line0"};
  reject(Model->arm({Event}, 0, 0), "connected interrupt");
  Event.MessageID = 0;
  ok(Model->arm({Event}, 0, 0));
  ok(Model->disconnect(0x300000, interrupts::MessageBased));
  Candidate.Object = 0x400100;
  ok(Model->connectMessages(0x400000, Candidates));
  reject(Model->queueNextDue(5), "original interrupt");
  ASSERT_EQ(Result.Interrupts.size(), 1u);
  EXPECT_TRUE(Result.Interrupts[0].UndeliveredReason);
  EXPECT_TRUE(Result.Interrupts[0].Handlers.empty());
}

TEST_F(KernelInterruptTest,
       PassiveArrivalIsObservedWhileDeliveryWaitsForItsEvent) {
  connectPassive();
  const std::vector<DriverInterruptEvent> Inputs{
      {0, "irq0", "line0"}, {1, "irq0", "line0"}, {2, "irq0", "line0"}};
  ok(Model->arm(Inputs, 0, 0));
  EXPECT_EQ(take(Model->dueCount(2)), 1u);
  auto First = queue(0);
  EXPECT_EQ(First.IRQL, 0u);
  EXPECT_EQ(take(Model->beginCall(First.Call.Token.ID, 0, 0)), 0u);
  EXPECT_EQ(Model->nextEventTime(), 1u);
  EXPECT_FALSE(take(Model->queueNextDue(1)));
  EXPECT_EQ(Result.Interrupts[1].OccurredAt100ns, 1u);
  EXPECT_FALSE(Result.Interrupts[1].DeliveredAt100ns);
  EXPECT_EQ(Model->nextEventTime(), 2u);
  EXPECT_FALSE(take(Model->queueNextDue(2)));
  EXPECT_FALSE(Model->nextEventTime());
  EXPECT_EQ(take(Model->finishCall(First.Call.Token.ID, 1, 0, 5)).Value, 1u);
  auto Second = queue(5);
  EXPECT_EQ(take(Model->beginCall(Second.Call.Token.ID, 0, 5)), 0u);
  take(Model->finishCall(Second.Call.Token.ID, 0, 0, 5));
  auto Third = queue(5);
  EXPECT_EQ(take(Model->beginCall(Third.Call.Token.ID, 0, 5)), 0u);
  take(Model->finishCall(Third.Call.Token.ID, 0, 0, 5));
  EXPECT_EQ(Result.Interrupts[1].OccurredAt100ns, 1u);
  EXPECT_EQ(Result.Interrupts[1].DeliveredAt100ns, 5u);
  EXPECT_EQ(Result.Interrupts[2].OccurredAt100ns, 2u);
  EXPECT_FALSE(Model->hasPendingEvents());
}

TEST_F(KernelInterruptTest,
       PassiveSynchronizationWaitersReserveTheEventInOrder) {
  connectPassive();
  reject(Model->acquire(Object, 1, 0), "do not have");
  arm(0, 0);
  auto ISR = queue(0);
  take(Model->beginCall(ISR.Call.Token.ID, 0, 0));
  auto First = take(Model->synchronize(Object, 0x180005100, 11));
  auto Second = take(Model->synchronize(Object, 0x180005200, 22));
  EXPECT_FALSE(take(Model->reserveSynchronization(First.Token.ID)));
  EXPECT_FALSE(take(Model->reserveSynchronization(Second.Token.ID)));
  take(Model->finishCall(ISR.Call.Token.ID, 0, 0, 5));
  EXPECT_FALSE(take(Model->reserveSynchronization(Second.Token.ID)));
  EXPECT_TRUE(take(Model->reserveSynchronization(First.Token.ID)));
  reject(Model->disconnect(Object, interrupts::FullySpecified), "owned");
  EXPECT_EQ(take(Model->beginCall(First.Token.ID, 0, 5)), 0u);
  EXPECT_FALSE(take(Model->reserveSynchronization(Second.Token.ID)));
  EXPECT_EQ(take(Model->finishCall(First.Token.ID, 0x101, 0, 6)).Value, 1u);
  EXPECT_TRUE(take(Model->reserveSynchronization(Second.Token.ID)));
  EXPECT_EQ(take(Model->beginCall(Second.Token.ID, 0, 6)), 0u);
  EXPECT_EQ(take(Model->finishCall(Second.Token.ID, 0x100, 0, 7)).Value, 0u);
  ok(Model->disconnect(Object, interrupts::FullySpecified));
}

TEST_F(KernelInterruptTest, IndependentPassiveCallsCanResumeOutOfStackOrder) {
  connectPassive();
  connectPassive(1);
  arm(0, 0);
  arm(0, 0, 1, "irq1");
  auto First = queue(0);
  take(Model->beginCall(First.Call.Token.ID, 0, 0));
  auto Second = queue(0);
  take(Model->beginCall(Second.Call.Token.ID, 0, 0));
  EXPECT_EQ(take(Model->finishCall(First.Call.Token.ID, 1, 0, 3)).Value, 1u);
  reject(Model->disconnect(OtherObject, interrupts::FullySpecified), "owned");
  EXPECT_EQ(take(Model->finishCall(Second.Call.Token.ID, 0, 0, 5)).Value, 0u);
  ok(Model->disconnect(Object, interrupts::FullySpecified));
  ok(Model->disconnect(OtherObject, interrupts::FullySpecified));
}

TEST_F(KernelInterruptTest,
       BlockedPassiveArrivalStillChecksPowerAtItsDeadline) {
  connectPassive();
  auto Sync = take(Model->synchronize(Object, 0x180005100, 11));
  EXPECT_TRUE(take(Model->reserveSynchronization(Sync.Token.ID)));
  take(Model->beginCall(Sync.Token.ID, 0, 0));
  arm(3, 0);
  Resources.setPhysicalPower(PDO, DevicePowerState::D3);
  EXPECT_EQ(take(Model->dueCount(3)), 0u);
  reject(Model->queueNextDue(3), "D0");
  ASSERT_EQ(Result.Interrupts.size(), 1u);
  EXPECT_EQ(Result.Interrupts[0].OccurredAt100ns, 3u);
  EXPECT_TRUE(Result.Interrupts[0].UndeliveredReason);
  EXPECT_TRUE(Result.Interrupts[0].Handlers.empty());
}
} // namespace
} // namespace neverd::emulation
