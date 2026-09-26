//===- KernelInterruptBridgeTests.cpp - Interrupt execution boundaries ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Exercise interrupt deadlines and IRQL ownership through KernelModel and
/// real backend contexts. Genuine fixtures separately execute DriverSession.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"
#include "unicorn/UnicornBackend.h"
#include "windows/DriverImage.h"
#include "windows/KernelModel.h"
#include "windows/WindowsKernelLayout.h"

#include <array>

namespace neverd::emulation {
namespace {
using namespace windows;

class KernelInterruptBridge : public ::testing::Test {
protected:
  static constexpr uint64_t Scratch = 0x60000000;
  static constexpr uint64_t Entry = 0x180001000;
  static constexpr uint64_t ISR = 0x180001100;
  static constexpr uint64_t Synchronize = 0x180001200;
  static constexpr uint64_t DPC = 0x180001300;
  static constexpr uint64_t ParentFrame = Scratch + 0x8000;
  static constexpr uint64_t ChildFrame = Scratch + 0x9000;
  std::unique_ptr<UnicornBackend> Memory;
  DriverResult Result;
  std::unique_ptr<KernelModel> Model;
  uint64_t PDO = 0, FDO = 0, Interrupt = 0;

  void ok(llvm::Error E) {
    if (E)
      ADD_FAILURE() << llvm::toString(std::move(E));
  }
  template <class T> T take(llvm::Expected<T> Value) {
    if (!Value) {
      ADD_FAILURE() << llvm::toString(Value.takeError());
      return {};
    }
    return std::move(*Value);
  }
  void reject(llvm::Error E, llvm::StringRef Text) {
    ASSERT_TRUE(bool(E));
    EXPECT_NE(llvm::toString(std::move(E)).find(Text.str()),
              std::string::npos);
  }
  template <class T>
  void reject(llvm::Expected<T> Value, llvm::StringRef Text) {
    ASSERT_FALSE(bool(Value));
    reject(Value.takeError(), Text);
  }
  uint64_t get(uint64_t Address, unsigned Width = 8) {
    return take(Memory->readInteger(Address, Width));
  }
  void put(uint64_t Address, uint64_t Value, unsigned Width = 8) {
    ok(Model->validateGuestAccess(Address, Width, true));
    ok(Memory->writeInteger(Address, Value, Width));
  }
  uint64_t call(const char *Name, std::initializer_list<uint64_t> Args) {
    return take(Model->call(Name, Args));
  }
  void complete(uint64_t IRP) {
    put(IRP + IRPStatusOffset, StatusSuccess, 4);
    put(IRP + IRPInformationOffset, 0);
    call("IofCompleteRequest", {IRP, 0});
  }
  void SetUp() override {
    Memory = take(UnicornBackend::create(8 * 1024 * 1024));
    ASSERT_TRUE(Memory);
    ok(Memory->map(Scratch, 0x10000, Read | Write));
    DriverOptions Options;
    DriverPnpDevice Device;
    Device.ID = "sensor";
    Device.Bus = DriverBusKind::RegisterBank;
    Device.InitialDevicePower = DevicePowerState::D0;
    Device.InitialSystemPower = SystemPowerState::Working;
    Device.InitialReportedDevicePower = DevicePowerState::D0;
    DriverMemoryResource Resource;
    Resource.ID = "registers";
    Resource.RawStart = 0x300000000;
    Resource.TranslatedStart = 0x400000000;
    Resource.Length = 0x1000;
    Resource.Registers = {{0, 4, DriverRegisterAccess::ReadWrite, 0}};
    Device.Resources.push_back(Resource);
    DriverInterruptResource IRQ;
    IRQ.ID = "irq";
    IRQ.RawVector = 7;
    IRQ.RawLevel = 7;
    IRQ.RawAffinity = 1;
    IRQ.TranslatedVector = 0x55;
    IRQ.TranslatedLevel = 5;
    IRQ.TranslatedAffinity = 1;
    Device.Interrupts.push_back(IRQ);
    Options.PnpDevices.push_back(Device);
    Result.Configuration = Options;
    Model = std::make_unique<KernelModel>(*Memory, Result);
    DriverImage Image;
    Image.Base = 0x180000000;
    Image.Entry = Entry;
    Image.Size = 0x3000;
    ok(Model->initialize(Image, Options));
    const auto Extension = get(Model->driverObject() + DriverExtensionOffset);
    put(Extension + DriverAddDeviceOffset, Entry);
    for (unsigned Major : {0u, 2u, 3u, 4u, 14u, 18u, 22u, 27u})
      put(Model->driverObject() + DriverDispatchOffset + Major * 8, Entry);
    ok(Model->finishEntry());
    ok(Model->preparePnpDevices());
    PDO = take(Model->beginAddDevice("sensor")).Argument1;
    ASSERT_NE(PDO, 0u);
    EXPECT_EQ(call("IoCreateDevice", {Model->driverObject(), 64, 0,
                                      UnknownDeviceType, 0, 0, Scratch}),
              StatusSuccess);
    FDO = get(Scratch);
    put(FDO + DeviceFlagsOffset, DeviceBufferedIO | DevicePowerPageable, 4);
    EXPECT_EQ(call("IoAttachDeviceToDeviceStack", {FDO, PDO}), PDO);
    ok(Model->finishAddDevice("sensor", StatusSuccess));
    DriverRequest Start;
    Start.Kind = DriverRequestKind::Pnp;
    Start.DeviceID = "sensor";
    Start.Pnp =
        DriverPnpOperation{DevicePnpRequest::Start, {StatusSuccess, 0}};
    const auto Packet = take(Model->beginRequest(Start));
    const uint64_t Stack = get(Packet.IRP + IRPStackPointerOffset);
    std::array<uint8_t, StackCompletionOffset> Prefix;
    ok(Memory->read(Stack, Prefix));
    ok(Memory->write(Stack - StackSize, Prefix));
    EXPECT_EQ(call("IofCallDriver", {PDO, Packet.IRP}), StatusSuccess);
    ok(Model->recordDispatchReturn(Packet.IRP, StatusSuccess));
    ok(Model->finalizeRequest(Packet.IRP));
    EXPECT_EQ(call("IoConnectInterrupt",
                   {Scratch, ISR, Scratch + 0x100, 0, 0x55, 5, 5, 1, 0, 1, 0}),
              StatusSuccess);
    Interrupt = get(Scratch);
    ASSERT_NE(Interrupt, 0u);
    DriverRequest Open;
    Open.Kind = DriverRequestKind::Create;
    Open.DeviceID = "sensor";
    Open.File = 1;
    const auto File = take(Model->beginRequest(Open));
    complete(File.IRP);
    ok(Model->recordDispatchReturn(File.IRP, StatusSuccess));
    ok(Model->finalizeRequest(File.IRP));
    Model->enterExecution(ParentFrame);
  }
  uint64_t request(uint64_t Delay, size_t SourceIndex = 17) {
    DriverRequest Input;
    Input.Kind = DriverRequestKind::DeviceControl;
    Input.DeviceID = "sensor";
    Input.File = 1;
    Input.ControlCode = 0x222000;
    Input.InterruptEvents = {{Delay, "sensor", "irq"}};
    const auto Packet = take(Model->beginRequest(Input, SourceIndex));
    call("IoMarkIrpPending", {Packet.IRP});
    ok(Model->recordDispatchReturn(Packet.IRP, StatusPending));
    return Packet.IRP;
  }
  void timer(uint64_t Delay) {
    call("KeInitializeDpc", {Scratch + 0x200, DPC, Scratch + 0x300});
    call("KeInitializeTimer", {Scratch + 0x400});
    EXPECT_EQ(call("KeSetTimer", {Scratch + 0x400, uint64_t(-int64_t(Delay)),
                                  Scratch + 0x200}),
              0u);
  }
  KernelScheduler::Invocation next(bool Advance = false) {
    auto Call = take(Model->nextScheduled(Advance));
    EXPECT_TRUE(Call);
    return Call.value_or(KernelScheduler::Invocation{});
  }
  void finishISR(const KernelScheduler::Invocation &Call, uint64_t Value) {
    EXPECT_FALSE(take(Model->continueScheduled(Call.ID, Value)));
    ok(Model->finishScheduled(Call.ID));
  }
  void exParameters(uint64_t Address, uint32_t Version) {
    call("IoDisconnectInterrupt", {Interrupt});
    put(Address + interrupts::VersionOffset, Version, 4);
    put(Address + interrupts::PDOOffset, PDO);
    put(Address + interrupts::OutputOffset, Scratch);
    put(Address + interrupts::RoutineOffset, ISR);
    put(Address + interrupts::ContextOffset, Scratch + 0x100);
    put(Address + interrupts::SpinLockOffset, 0);
    put(Address + interrupts::SynchronizeIRQL, 5, 1);
    put(Address + interrupts::FloatingSave, 0, 1);
  }
};

TEST_F(KernelInterruptBridge, LineBasedExDoesNotReadUnselectedUnionTail) {
  // FloatingSave is the final selected byte. Every later union field lies
  // outside this mapping; a whole-record read would reject a valid call.
  const uint64_t Parameters =
      Scratch + 0x10000 - (interrupts::FloatingSave + 1);
  exParameters(Parameters, interrupts::LineBased);
  EXPECT_EQ(call("IoConnectInterruptEx", {Parameters}), StatusSuccess);
  EXPECT_NE(get(Scratch), Interrupt);
}

TEST_F(KernelInterruptBridge, FullySpecifiedExDoesNotReadGroupField) {
  // Version 1 ends with KAFFINITY. Group belongs only to version 4 and is
  // deliberately on the first unmapped byte, with the remaining union tail.
  const uint64_t Parameters = Scratch + 0x10000 - interrupts::Group;
  exParameters(Parameters, interrupts::FullySpecified);
  put(Parameters + interrupts::ShareVector, 0, 1);
  put(Parameters + interrupts::Vector, 0x55, 4);
  put(Parameters + interrupts::IRQL, 5, 1);
  put(Parameters + interrupts::Mode, 1, 4);
  put(Parameters + interrupts::Affinity, 1);
  EXPECT_EQ(call("IoConnectInterruptEx", {Parameters}), StatusSuccess);
  EXPECT_NE(get(Scratch), Interrupt);
}

TEST_F(KernelInterruptBridge, SameDeadlineInterruptPrecedesTimerDPC) {
  const auto IRP = request(20);
  timer(20);
  const auto Service = next(true);
  EXPECT_EQ(Service.Kind, KernelScheduler::CallbackKind::Interrupt);
  EXPECT_EQ(Service.PC, ISR);
  EXPECT_EQ(Service.Arguments,
            (std::vector<uint64_t>{Interrupt, Scratch + 0x100}));
  EXPECT_EQ(Service.IRQL, 5);
  EXPECT_EQ(Model->currentIRQL(), 5);
  ASSERT_EQ(Result.Interrupts.size(), 1u);
  EXPECT_EQ(Result.Interrupts[0].SourceRequestIndex, 17u);
  EXPECT_EQ(Result.Interrupts[0].OccurredAt100ns, 20u);
  EXPECT_EQ(Result.Interrupts[0].DeliveredAt100ns, 20u);
  EXPECT_TRUE(Model->hasQueuedDPC());
  reject(Model->call("IofCompleteRequest", {IRP, 0}), "IRQL");
  EXPECT_FALSE(Result.Requests.back().Completed);
  finishISR(Service, 0x100); // Undefined high RAX bits do not claim the IRQ.
  EXPECT_EQ(Result.Interrupts[0].ReturnValue, 0u);
  EXPECT_EQ(Model->currentIRQL(), 0);
  const auto Deferred = next();
  EXPECT_EQ(Deferred.Kind, KernelScheduler::CallbackKind::DPC);
  EXPECT_EQ(Deferred.PC, DPC);
  EXPECT_EQ(Model->currentIRQL(), 2);
  complete(IRP);
  ok(Model->finishScheduled(Deferred.ID));
  ok(Model->finalizeRequest(IRP));
  EXPECT_FALSE(Model->hasPendingInterruptEvents());
}

TEST_F(KernelInterruptBridge,
       SharedCapacityFailurePreservesTimeTimerAndInterruptBeforeRetry) {
  request(20);
  timer(20);
  const auto Work = call("IoAllocateWorkItem", {FDO});
  uint64_t Last = 0;
  for (uint64_t I = 0; I < scheduler::DefaultMaxPendingCallbacks - 1; ++I) {
    call("IoQueueWorkItem", {Work, Entry, 1, 0});
    const auto Worker = next();
    ASSERT_EQ(Worker.Kind, KernelScheduler::CallbackKind::WorkItem);
    ok(Model->suspendScheduled(Worker.ID));
    Last = Worker.ID;
  }
  reject(Model->nextScheduled(true), "pending callback limit");
  EXPECT_EQ(Model->nextEventTime(), 20u);
  EXPECT_EQ(call("KeReadStateTimer", {Scratch + 0x400}), 0u);
  EXPECT_FALSE(Model->hasQueuedDPC());
  ASSERT_EQ(Result.Interrupts.size(), 1u);
  EXPECT_FALSE(Result.Interrupts[0].OccurredAt100ns);
  EXPECT_FALSE(Result.Interrupts[0].DeliveredAt100ns);
  EXPECT_FALSE(Result.Interrupts[0].ReturnValue);
  EXPECT_FALSE(Result.Requests.back().Completed);
  ok(Model->resumeScheduled(Last));
  ok(Model->finishScheduled(Last));
  const auto Service = next(true);
  EXPECT_EQ(Service.Kind, KernelScheduler::CallbackKind::Interrupt);
  EXPECT_EQ(Result.Interrupts[0].DeliveredAt100ns, 20u);
  EXPECT_TRUE(Model->hasQueuedDPC());
  finishISR(Service, 1);
}

TEST_F(KernelInterruptBridge,
       CompletedSourceRetainsIndependentDueEventAndExplicitSourceIndex) {
  const auto IRP = request(7, 42);
  complete(IRP);
  ok(Model->finalizeRequest(IRP));
  EXPECT_FALSE(Model->requestPending());
  EXPECT_TRUE(Model->hasPendingInterruptEvents());
  reject(Model->validateGuestAccess(IRP + IRPStatusOffset, 4, false), "freed");
  const auto Service = next(true);
  EXPECT_EQ(Service.Kind, KernelScheduler::CallbackKind::Interrupt);
  EXPECT_EQ(Result.Interrupts[0].SourceRequestIndex, 42u);
  EXPECT_EQ(Result.Interrupts[0].DeliveredAt100ns, 7u);
  finishISR(Service, 0x180);
  EXPECT_EQ(Result.Interrupts[0].ReturnValue, 0x80u);
  EXPECT_FALSE(Model->hasPendingInterruptEvents());
}

TEST_F(KernelInterruptBridge,
       SynchronizationRestoresPassiveAndDPCParentsIncludingSavedCR8) {
  for (uint8_t Level : {uint8_t(0), uint8_t(2)}) {
    std::optional<KernelScheduler::Invocation> ParentDPC;
    if (Level == 2) {
      call("KeInitializeDpc", {Scratch + 0x200, DPC, 0});
      EXPECT_EQ(call("KeInsertQueueDpc", {Scratch + 0x200, 0, 0}), 1u);
      ParentDPC = next();
    }
    Model->enterExecution(ParentFrame);
    ASSERT_EQ(Model->currentIRQL(), Level);
    ok(Memory->setReg(X64Register::CR8, Level));
    ok(Memory->setReg(X64Register::BX, 0x123456789abcdef0));
    call("KeSynchronizeExecution", {Interrupt, Synchronize, Scratch + 0x500});
    // Preparation must not raise the parent's IRQL before its context save.
    EXPECT_EQ(Model->currentIRQL(), Level);
    auto Call = Model->takeGuestCall();
    ASSERT_TRUE(Call);
    EXPECT_EQ(Call->Token.Owner, GuestCallOwner::Interrupt);
    EXPECT_EQ(Call->Arguments, (std::vector<uint64_t>{Scratch + 0x500}));
    auto Parent = take(Memory->saveContext());
    ASSERT_TRUE(Parent);
    ok(Model->beginGuestCall(Call->Token));
    EXPECT_EQ(Model->currentIRQL(), 5);
    Model->enterExecution(ChildFrame);
    ok(Memory->setReg(X64Register::CR8, Model->currentIRQL()));
    EXPECT_EQ(take(Memory->reg(X64Register::CR8)), 5u);
    ok(Memory->setReg(X64Register::BX, 0));
    ok(Model->validateExecutionReturn(ChildFrame, 5));
    EXPECT_EQ(take(Model->finishGuestCall(Call->Token, 0xabcdef80)),
              std::optional<uint64_t>(0x80));
    EXPECT_EQ(Model->currentIRQL(), Level);
    Model->enterExecution(ParentFrame);
    ok(Memory->restoreContext(*Parent));
    EXPECT_EQ(take(Memory->reg(X64Register::CR8)), Level);
    EXPECT_EQ(take(Memory->reg(X64Register::BX)), 0x123456789abcdef0u);
    ok(Model->validateExecutionReturn(ParentFrame, Level));
    if (ParentDPC)
      ok(Model->finishScheduled(ParentDPC->ID));
  }
}

TEST_F(KernelInterruptBridge, FailedGuestEntryDoesNotAlterTheHeldLockOrIRQL) {
  reject(Model->beginGuestCall({GuestCallOwner::Interrupt, 12345}),
         "prepared continuation");
  EXPECT_EQ(Model->currentIRQL(), 0);
  call("KeSynchronizeExecution", {Interrupt, Synchronize, 0});
  auto Call = Model->takeGuestCall();
  ASSERT_TRUE(Call);
  ok(Model->beginGuestCall(Call->Token));
  EXPECT_EQ(Model->currentIRQL(), 5);
  reject(Model->beginGuestCall(Call->Token), "prepared continuation");
  EXPECT_EQ(Model->currentIRQL(), 5);
  reject(Model->call("KeAcquireInterruptSpinLock", {Interrupt}),
         "nonrecursive");
  EXPECT_EQ(take(Model->finishGuestCall(Call->Token, 0x100)),
            std::optional<uint64_t>(0));
  EXPECT_EQ(Model->currentIRQL(), 0);
  EXPECT_EQ(call("KeAcquireInterruptSpinLock", {Interrupt}), 0u);
  EXPECT_EQ(Model->currentIRQL(), 5);
  call("KeReleaseInterruptSpinLock", {Interrupt, 0});
  EXPECT_EQ(Model->currentIRQL(), 0);
}

TEST_F(KernelInterruptBridge, ManualLockReleaseRequiresExactFrameAndOldIRQL) {
  EXPECT_EQ(call("KeAcquireInterruptSpinLock", {Interrupt}), 0u);
  EXPECT_EQ(Model->currentIRQL(), 5);
  reject(Model->validateExecutionReturn(ParentFrame, 0), "manual interrupt");
  Model->enterExecution(ChildFrame);
  reject(Model->call("KeReleaseInterruptSpinLock", {Interrupt, 0}),
         "owning execution");
  EXPECT_EQ(Model->currentIRQL(), 5);
  Model->enterExecution(ParentFrame);
  reject(Model->call("KeReleaseInterruptSpinLock", {Interrupt, 2}),
         "saved IRQL");
  EXPECT_EQ(Model->currentIRQL(), 5);
  call("KeReleaseInterruptSpinLock", {Interrupt, 0x100});
  EXPECT_EQ(Model->currentIRQL(), 0);
  ok(Model->validateExecutionReturn(ParentFrame, 0));
}

TEST_F(KernelInterruptBridge,
       DisconnectedEventFailsAtItsDeadlineWithoutRetargetingNewConnection) {
  request(20);
  call("IoDisconnectInterrupt", {Interrupt});
  EXPECT_EQ(
      call("IoConnectInterrupt", {Scratch, ISR, 0, 0, 0x55, 5, 5, 1, 0, 1, 0}),
      StatusSuccess);
  EXPECT_NE(get(Scratch), Interrupt);
  reject(Model->nextScheduled(true), "original interrupt connection");
  ASSERT_EQ(Result.Interrupts.size(), 1u);
  EXPECT_EQ(Result.Interrupts[0].OccurredAt100ns, 20u);
  EXPECT_TRUE(Result.Interrupts[0].UndeliveredReason);
  EXPECT_FALSE(Result.Interrupts[0].DeliveredAt100ns);
  EXPECT_FALSE(Result.Interrupts[0].ReturnValue);
}

} // namespace
} // namespace neverd::emulation
