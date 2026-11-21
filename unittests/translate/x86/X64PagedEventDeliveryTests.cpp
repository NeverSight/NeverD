//===- X64PagedEventDeliveryTests.cpp - Nested architectural faults -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Restart metadata, descriptor faults, retained RAM effects and #DF shutdown.
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/analysis/arch/x86_64/X64PagedEventDelivery.h"
#include "neverd/analysis/arch/x86_64/X86ModeExecution.h"

#include "llvm/Support/Error.h"

#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

using namespace neverd::analysis;
using namespace neverd::translate;

namespace {

class X64PagedEventTest : public testing::Test {
protected:
  X64ContinuationStateV6 State;
  GuestState RAM;
  X86PagingFeatures Features{.PhysicalAddressBits = 48,
                             .PAE = true,
                             .LongMode = true,
                             .PAT = true,
                             .NX = true};
  X86PagingState Controls{.CR0 = 0x80010001,
                          .CR3 = 0x1000,
                          .CR4 = 0x20,
                          .EFER = 0xd00,
                          .RFlags = 0x202};
  std::unique_ptr<X86PagedMemory> Memory;

  void SetUp() override {
    RAM = llvm::cantFail(createZeroedGuestState(GuestArchitecture::X86_64));
    RAM.Memory.push_back({0, MemoryPermission::Read | MemoryPermission::Write,
                          0, std::vector<uint8_t>(0x20000)});
    put(0x1000, 0x2007);
    put(0x2000, 0x3007);
    put(0x3000, 0x4007);
    for (unsigned I = 0; I != 32; ++I)
      put(0x4000 + I * 8, I * 4096 | 3);
    put(0x5008, 0x00af9b000000ffffULL);
    put(0x5010, 0x00cf93000000ffffULL);
    State.Core.CS = {0, UINT32_MAX, 8, 0xa09b};
    State.Core.SS = {0, UINT32_MAX, 16, 0xc093};
    State.GDTR = {0x5000, 0x3f};
    State.IDTR = {0x6000, 4095};
    State.TR = {0x8000, 0x67, 40, 0x8b};
    State.Core.Core.Core.Core.RFlags = 0x202;
    State.Core.Core.Core.Core.PC = 0x9000;
    State.Core.Core.Core.Core.GPR[4] = 0x1e000;
    for (unsigned V = 0; V != 256; ++V)
      gate(V);
  }

  void put(uint64_t Address, uint64_t Value) {
    ASSERT_LE(Address + 8, RAM.Memory.front().Bytes.size());
    for (unsigned I = 0; I != 8; ++I)
      RAM.Memory.front().Bytes[Address + I] = Value >> (8 * I);
  }

  void gate(unsigned Vector, uint8_t Access = 0x8e, unsigned IST = 0,
            unsigned Selector = 8) {
    const uint64_t PC = 0x10000 + Vector * 16;
    const uint64_t Address = State.IDTR.Base + Vector * 16;
    put(Address, (PC & 0xffff) | (uint64_t(Selector) << 16) |
                     (uint64_t(IST) << 32) | (uint64_t(Access) << 40) |
                     ((PC & 0xffff0000) << 32));
    put(Address + 8, 0);
  }

  X64PagedEventResult deliver(unsigned Vector, X64ContinuationEventKind Kind,
                              unsigned Error = 0, uint64_t CR2 = 0) {
    Memory = llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
    X64ContinuationEventV1 Event;
    Event.Kind = Kind;
    Event.Vector = Vector;
    Event.SavedPC = State.Core.Core.Core.Core.PC;
    Event.ErrorCode = Error;
    Event.FaultAddress = CR2;
    return llvm::cantFail(deliverX64PagedEvent(State, *Memory, Event, 0xffff));
  }

  uint64_t get(uint64_t Address) {
    return Memory->physicalMemory()
        .loadScalar(Address, GuestScalarWidth::I64)
        .Value;
  }
};

TEST_F(X64PagedEventTest, PageFaultPublishesCR2AndRestartFrame) {
  const auto R = deliver(14, X64ContinuationEventKind::Fault, 7, 0xabcdef);
  EXPECT_EQ(R.Status, X64PagedEventStatus::Delivered);
  EXPECT_EQ(R.DeliveredVector, 14);
  EXPECT_EQ(R.DeliveryAttempts, 1u);
  EXPECT_EQ(State.Core.Core.CR2, 0xabcdefu);
  EXPECT_EQ(State.Core.Core.Core.Core.PC, 0x100e0u);
  const auto SP = State.Core.Core.Core.Core.GPR[4];
  EXPECT_EQ(SP, 0x1dfd0u);
  EXPECT_EQ(get(SP), 7u);
  EXPECT_EQ(get(SP + 8), 0x9000u);
  EXPECT_EQ(get(SP + 24), 0x10202u);
}

TEST_F(X64PagedEventTest,
       RsmRetainsEditedSaveImageAndPaidPaeReloadAcrossBackingRepair) {
  constexpr uint64_t Root = 0x20000, Save = 0x1fe00;
  auto SystemImage = RAM;
  SystemImage.Memory.front().Bytes[0x18000] = 0x0f;
  SystemImage.Memory.front().Bytes[0x18001] = 0xaa;
  RAM.Memory.push_back({Root, MemoryPermission::Read | MemoryPermission::Write,
                        0, std::vector<uint8_t>(8)});
  Memory = llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
  auto System =
      llvm::cantFail(X86PagedMemory::create(SystemImage, Features, Controls));
  X86ModeState CPU;
  CPU.Core.Core = State;
  auto &V = CPU.Core.Core;
  V.DS = V.ES = V.Core.SS;
  V.FS = V.GS = {UINT32_MAX, 16, 0xc093};
  V.Unusable = 64;
  CPU.Debug.emplace();
  CPU.SMM.emplace();
  CPU.SMM->SMBase = 0x10000;
  X86ModeExecutionOptions O;
  O.Legacy.Segments.FullLimitChoice = neverd::X86FullSegmentLimitChoice::Fault;
  O.Legacy.Debug = O.Long.DebugProfile =
      neverd::X86InstructionDebugProfile::Watchpoints;
  O.MXCSRMask = 0xffff;
  O.PagingAddressBits = 48;
  O.Undefined.emplace();
  X86ModeExecution Execution(*Memory, O, *System);
  ASSERT_TRUE(
      llvm::cantFail(Execution.systemManagementInterrupt(CPU)).EventAccepted);
  const auto Edit = [&](unsigned Offset, GuestScalarWidth Width,
                        uint64_t Value) {
    ASSERT_EQ(System->physicalMemory()
                  .storeScalar(Save + Offset, Width, Value)
                  .Status,
              GuestMemoryAccessStatus::Completed);
  };
  ASSERT_NO_FATAL_FAILURE(Edit(0x12, GuestScalarWidth::I16, 0xc09b));
  ASSERT_NO_FATAL_FAILURE(Edit(0xd0, GuestScalarWidth::I64, 0));
  ASSERT_NO_FATAL_FAILURE(Edit(0x148, GuestScalarWidth::I64, 0x20));
  ASSERT_NO_FATAL_FAILURE(Edit(0x150, GuestScalarWidth::I64, Root));
  ASSERT_NO_FATAL_FAILURE(Edit(0x158, GuestScalarWidth::I64, 0x80010011));
  ASSERT_NO_FATAL_FAILURE(Edit(0x1f8, GuestScalarWidth::I64, 0x1234));
  X86ModeInstruction Rsm{0x18000, X86ExecutionMode::Real16, {0x0f, 0xaa}};
  uint64_t Budget = 1;
  auto Stop = llvm::cantFail(Execution.run(CPU, Rsm, Budget));
  ASSERT_EQ(Stop.Status, X86LegacyExecutionStatus::BackingFault);
  EXPECT_TRUE(CPU.SMM->Active);
  ASSERT_TRUE(CPU.Pending);
  EXPECT_EQ(Stop.Attempts, 1u);
  EXPECT_EQ(Budget, 0u);

  ASSERT_NO_FATAL_FAILURE(Edit(0x1f8, GuestScalarWidth::I64, 0xbad));
  RAM.Memory = Memory->physicalMemory().snapshotMemoryRegions();
  RAM.Memory.back().Bytes.assign(32, 0);
  std::fill_n(RAM.Memory.back().Bytes.begin(), 8, 0xff);
  Memory = llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
  X86ModeExecution Repaired(*Memory, O, *System);
  auto Done = llvm::cantFail(Repaired.run(CPU, {}, Budget));
  EXPECT_EQ(Done.Attempts, 0u);
  EXPECT_EQ(Done.Completions, 1u);
  EXPECT_FALSE(CPU.Pending);
  EXPECT_FALSE(CPU.SMM->Active);
  EXPECT_EQ(V.Core.Core.Core.Core.GPR[0], 0x1234u);
  EXPECT_EQ(Memory->controls().PDPTE, (std::array<uint64_t, 4>{}));
  EXPECT_EQ(llvm::cantFail(Repaired.mode(CPU)), X86ExecutionMode::Protected32);
}

TEST_F(X64PagedEventTest,
       RealModeInterruptsPushParagraphSelectorsAndIretRestoresTheOriginalPC) {
  for (const auto Kind :
       {neverd::X86EventKind::NMI, neverd::X86EventKind::Interrupt}) {
    SetUp();
    X86ModeState CPU;
    CPU.Core.Core = State;
    auto &V = CPU.Core.Core;
    auto &R = V.Core.Core.Core.Core;
    V.Core.CS = {0x1000, UINT16_MAX, 0x100, 0x93};
    V.Core.SS = {0x7000, UINT16_MAX, 0x700, 0x93};
    V.DS = V.ES = {0, UINT16_MAX, 0, 0x93};
    V.FS = V.GS = {UINT16_MAX, 0, 0x93};
    V.Unusable = 64;
    V.IDTR = {0x2000, 1023};
    R.PC = 0x200;
    R.GPR[4] = 0x8800;
    R.RFlags = 0x202;
    X86ModeExecutionOptions O;
    O.Legacy.Segments.FullLimitChoice =
        neverd::X86FullSegmentLimitChoice::Fault;
    O.Legacy.Returns = X86LegacyReturnProfile::OrderedFrame;
    O.Legacy.Events = X86LegacyEventProfile::OrderedFrame;
    O.MXCSRMask = 0xffff;
    O.PagingAddressBits = 48;
    O.Undefined.emplace();
    const uint8_t Vector = Kind == neverd::X86EventKind::NMI ? 2 : 64;
    put(0x2000 + 4 * Vector, uint32_t(0x300) << 16 | 0x101);
    RAM.Memory.front().Bytes[0x3101] = 0xcf;
    Controls = {0x10, 0, 0, 0, 0x202};
    Memory = llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
    X86ModeExecution Execution(*Memory, O);
    auto Event = llvm::cantFail(Execution.interrupt(CPU, Kind, Vector));
    ASSERT_EQ(Event.Status, X86LegacyExecutionStatus::EventDelivered)
        << Event.OwnerError;
    EXPECT_TRUE(Event.EventAccepted);
    EXPECT_EQ(R.PC, 0x101u);
    EXPECT_EQ(V.Core.CS.Selector, 0x300u);
    EXPECT_EQ(V.Core.CS.Base, 0x3000u);
    EXPECT_EQ(R.GPR[4], 0x87fau);
    EXPECT_EQ(R.RFlags, 2u);
    EXPECT_EQ(get(0xf7fa) & 0xffffffffffffULL, 0x020201000200ULL);
    X86ModeInstruction Return{0x3101, X86ExecutionMode::Real16, {0xcf}};
    uint64_t Budget = 1;
    auto Done = llvm::cantFail(Execution.run(CPU, Return, Budget));
    EXPECT_EQ(Done.Completions, 1u) << Done.OwnerError;
    EXPECT_EQ(V.Core.CS.Base, 0x1000u);
    EXPECT_EQ(R.PC, 0x200u);
    EXPECT_EQ(R.GPR[4], 0x8800u);
    EXPECT_EQ(R.RFlags, 0x202u);
    EXPECT_FALSE(V.NMIBlocked);
  }
}

TEST_F(X64PagedEventTest, LongModeHaltRetiresBeforeIrqWakeAndZeroBudgetNmi) {
  enum class Wake { NMI, IRQ, SingleStep };
  for (const auto Source : {Wake::NMI, Wake::IRQ, Wake::SingleStep}) {
    SCOPED_TRACE(static_cast<unsigned>(Source));
    const bool UnderSTI = Source == Wake::IRQ;
    SetUp();
    X86ModeState CPU;
    CPU.Core.Core = State;
    auto &R = CPU.Core.Core.Core.Core.Core.Core;
    R.RFlags = UnderSTI ? 0x202 : Source == Wake::SingleStep ? 0x102 : 2;
    CPU.STIBlocked = UnderSTI;
    X86ModeExecutionOptions O;
    O.Legacy.Segments.FullLimitChoice =
        neverd::X86FullSegmentLimitChoice::Fault;
    O.MXCSRMask = 0xffff;
    O.PagingAddressBits = 48;
    O.Undefined.emplace();
    if (Source == Wake::SingleStep) {
      CPU.Debug.emplace();
      O.Legacy.Debug = O.Long.DebugProfile =
          neverd::X86InstructionDebugProfile::Watchpoints;
    }
    X86ModeInterrupts Inputs{*X86LocalApicFixed::fromState({}, 0)};
    ASSERT_EQ(Inputs.APIC.receiveFixedInterrupt(64, true, true),
              X86LocalApicArrival::Queued);
    RAM.Memory.front().Bytes[0x9000] = 0xf4;
    Memory = llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
    X86ModeExecution Execution(*Memory, O);
    X86ModeInstruction Halt{0x9000, X86ExecutionMode::Long64, {0xf4}};
    uint64_t Budget = 1;
    auto Result = llvm::cantFail(Execution.run(CPU, Halt, Budget, Inputs));
    ASSERT_EQ(Result.Completions, 1u) << Result.OwnerError;
    ASSERT_EQ(Result.Attempts, 1u);
    EXPECT_EQ(Budget, 0u);
    if (Source == Wake::NMI) {
      ASSERT_EQ(Result.Status, X86LegacyExecutionStatus::Halted);
      EXPECT_EQ(R.PC, 0x9001u);
      Inputs.NMIPending = true;
      Result = llvm::cantFail(Execution.run(CPU, {}, Budget, Inputs));
      EXPECT_EQ(Result.Attempts, 0u);
      EXPECT_EQ(Result.Completions, 0u);
    }
    ASSERT_EQ(Result.Status, X86LegacyExecutionStatus::EventDelivered)
        << Result.OwnerError;
    EXPECT_EQ(Result.DeliveredVector, UnderSTI                     ? 64u
                                      : Source == Wake::SingleStep ? 1u
                                                                   : 2u);
    if (Source == Wake::SingleStep) {
      EXPECT_TRUE(CPU.Debug->DR6 & (1u << 14));
      EXPECT_EQ(get(R.GPR[4] + 16) & 0x10100, 0x100u);
    }
    EXPECT_FALSE(CPU.Halted);
    EXPECT_FALSE(CPU.STIBlocked);
    EXPECT_EQ(get(R.GPR[4]), 0x9001u);
    EXPECT_EQ(Inputs.APIC.state().ISR[2], unsigned(UnderSTI));
  }
}

TEST_F(X64PagedEventTest, StackFrameWrapsAcrossTheLinearAddressBoundary) {
  for (uint64_t OldSP : {0u, 16u}) {
    SetUp();
    for (unsigned Level = 0; Level != 4; ++Level)
      put((Level + 1) * 0x1000 + 511 * 8,
          Level == 3 ? 0x1f003 : (Level + 2) * 0x1000 + 3);
    State.Core.Core.Core.Core.GPR[4] = OldSP;
    const auto R = deliver(14, X64ContinuationEventKind::Fault, 7, 0xabcdef);
    ASSERT_EQ(R.Status, X64PagedEventStatus::Delivered);
    EXPECT_EQ(R.DeliveryAttempts, 1u);
    const uint64_t SP = OldSP - 48;
    EXPECT_EQ(State.Core.Core.Core.Core.GPR[4], SP);
    EXPECT_EQ(get(0x1f000 + (SP & 4095)), 7u);
    EXPECT_EQ(get(0x1f000 + ((SP + 8) & 4095)), 0x9000u);
    const uint64_t SavedSP = OldSP - 16;
    EXPECT_EQ(get(SavedSP < 4096 ? SavedSP : 0x1f000 + (SavedSP & 4095)),
              OldSP);
  }
}

TEST_F(X64PagedEventTest, SamePrivilegeEntryDoesNotRereadItsCodeDescriptor) {
  // The descriptor aliases its own PTE. Setting descriptor.A changes that
  // mapping's physical address; the already consumed descriptor must not be
  // loaded again as the unused arm of a stack-switch SELECT.
  const uint64_t Descriptor = 0x00209a0000000003ULL;
  const uint64_t Table = Descriptor & 0x0000fffffffff000ULL;
  put(0x3000, Table | 7);
  put(0x4028, Descriptor);
  State.GDTR.Base = 0x5020;
  RAM.Memory.push_back(
      {Table, MemoryPermission::Read | MemoryPermission::Write, 0,
       std::vector<uint8_t>(RAM.Memory.front().Bytes.begin() + 0x4000,
                            RAM.Memory.front().Bytes.begin() + 0x5000)});
  const auto R = deliver(3, X64ContinuationEventKind::Trap);
  ASSERT_EQ(R.Status, X64PagedEventStatus::Delivered);
  EXPECT_EQ(R.DeliveredVector, 3);
  EXPECT_EQ(R.DeliveryAttempts, 1u);
  EXPECT_EQ(get(Table + 0x28), Descriptor | 0x60 | (uint64_t(1) << 40));
}

TEST_F(X64PagedEventTest, SoftwareVectorIdentityDoesNotMakeItContributory) {
  gate(13, 0x0e); // Not present; software INT13 is a benign first event.
  const auto R = deliver(13, X64ContinuationEventKind::Software);
  EXPECT_EQ(R.Status, X64PagedEventStatus::Delivered);
  EXPECT_EQ(R.DeliveredVector, 11);
  EXPECT_EQ(R.DeliveryAttempts, 2u);
  EXPECT_EQ(get(State.Core.Core.Core.Core.GPR[4]), 13u * 8 + 2);
}

TEST_F(X64PagedEventTest, ContributoryThenContributoryBecomesDoubleFault) {
  gate(13, 0x0e);
  const auto R = deliver(13, X64ContinuationEventKind::Fault);
  EXPECT_EQ(R.Status, X64PagedEventStatus::Delivered);
  EXPECT_EQ(R.DeliveredVector, 8);
  EXPECT_EQ(R.DeliveryAttempts, 2u);
  EXPECT_EQ(get(State.Core.Core.Core.Core.GPR[4]), 0u);
  ASSERT_TRUE(R.LastException);
  EXPECT_EQ(R.LastException->Vector, 11);
  EXPECT_EQ(R.LastException->ErrorCode, 13u * 8 + 3);
}

TEST_F(X64PagedEventTest, GatePrivilegeCheckPrecedesPresenceCheck) {
  State.Core.CS.Selector = 27;
  State.Core.CS.Attributes = 0xa0fb;
  gate(32, 0x0e); // DPL0 and absent; CPL3 INT must report #GP first.
  put(0x8004, 0x1e000);
  const auto R = deliver(32, X64ContinuationEventKind::Software);
  EXPECT_EQ(R.Status, X64PagedEventStatus::Delivered);
  EXPECT_EQ(R.DeliveredVector, 13);
  ASSERT_TRUE(R.LastException);
  EXPECT_EQ(R.LastException->ErrorCode, 32u * 8 + 2);
}

TEST_F(X64PagedEventTest, NestedPageFaultUpdatesCR2BeforeDoubleFault) {
  State.IDTR.Base = 0x6f30;
  for (unsigned V : {8u, 13u, 14u})
    gate(V);
  put(0x4000 + 7 * 8, 0);
  const auto R = deliver(14, X64ContinuationEventKind::Fault, 0, 0xfeed);
  EXPECT_EQ(R.Status, X64PagedEventStatus::Delivered);
  EXPECT_EQ(R.DeliveredVector, 8);
  EXPECT_EQ(R.DeliveryAttempts, 2u);
  EXPECT_EQ(State.Core.Core.CR2, 0x7010u);
  EXPECT_EQ(get(State.Core.Core.Core.Core.GPR[4]), 0u);
}

TEST_F(X64PagedEventTest, FaultWhileDeliveringDoubleFaultReportsShutdown) {
  State.IDTR.Base = 0x6f30;
  for (unsigned V : {8u, 13u, 14u})
    gate(V);
  put(0x4000 + 6 * 8, 0);
  put(0x4000 + 7 * 8, 0);
  const auto R = deliver(14, X64ContinuationEventKind::Fault, 0, 0xfeed);
  EXPECT_EQ(R.Status, X64PagedEventStatus::Shutdown);
  EXPECT_EQ(R.DeliveryAttempts, 2u);
  EXPECT_EQ(State.Core.Core.CR2, 0x6fb0u);
  EXPECT_EQ(State.Core.Core.Core.Core.PC, 0x9000u);
}

TEST_F(X64PagedEventTest,
       PartialFrameStoresSurviveDeliveryFaultAndISTRecovery) {
  State.Core.Core.Core.Core.GPR[4] = 0x1e010;
  put(0x4000 + 0x1d * 8, 0);
  gate(8, 0x8e, 1);
  put(0x8000 + 36, 0x1c000);
  const auto R = deliver(3, X64ContinuationEventKind::Trap);
  EXPECT_EQ(R.Status, X64PagedEventStatus::Delivered);
  EXPECT_EQ(R.DeliveredVector, 8);
  EXPECT_EQ(R.DeliveryAttempts, 3u);
  EXPECT_EQ(State.Core.Core.CR2, 0x1dff8u);
  EXPECT_EQ(get(0x1e008), 16u);
  EXPECT_EQ(get(0x1e000), 0x1e010u);
  EXPECT_EQ(State.Core.Core.Core.Core.GPR[4], 0x1bfd0u);
}

TEST_F(X64PagedEventTest, NMIBlockingPersistsWhenItsGateFaults) {
  gate(2, 0x0e);
  const auto R = deliver(2, X64ContinuationEventKind::NMI);
  EXPECT_EQ(R.Status, X64PagedEventStatus::Delivered);
  EXPECT_EQ(R.DeliveredVector, 11);
  EXPECT_EQ(State.NMIBlocked, 1);
  EXPECT_EQ(get(State.Core.Core.Core.Core.GPR[4]), 2u * 8 + 3);
}

TEST_F(X64PagedEventTest, InvalidInputDoesNotPublishCR2OrMemory) {
  const auto Original = State;
  const auto R = deliver(14, X64ContinuationEventKind::Software, 1, 0xfeed);
  EXPECT_EQ(R.Status, X64PagedEventStatus::InvalidState);
  EXPECT_EQ(std::memcmp(&State, &Original, sizeof(State)), 0);
  EXPECT_EQ(get(0x1000), 0x2007u);
}

TEST_F(X64PagedEventTest, BackingFailureIsNotConvertedToDoubleFault) {
  put(0x4000 + 6 * 8, 0x800003);
  const auto R = deliver(13, X64ContinuationEventKind::Fault);
  EXPECT_EQ(R.Status, X64PagedEventStatus::BackingFault);
  EXPECT_EQ(R.DeliveryAttempts, 1u);
  EXPECT_FALSE(R.LastException);
  ASSERT_TRUE(R.BackingFault);
  EXPECT_EQ(R.BackingFault->Kind, RuntimeMemoryFaultKindV1::Unmapped);
}

TEST_F(X64PagedEventTest, FREDControlCannotUseLegacyIDTDelivery) {
  Controls.CR4 |= uint64_t(1) << 32;
  Memory = llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
  const auto Before = State;
  X64ContinuationEventV1 Event;
  auto R = deliverX64PagedEvent(State, *Memory, Event, 0xffff);
  EXPECT_FALSE(bool(R));
  llvm::consumeError(R.takeError());
  EXPECT_EQ(std::memcmp(&State, &Before, sizeof(State)), 0);
  EXPECT_EQ(get(0x1000), 0x2007u);
}

TEST_F(X64PagedEventTest, AcceptedNMIResumesWithoutRecheckingItsOwnBlocker) {
  put(0x4030, 0x800003);
  const auto R = deliver(2, X64ContinuationEventKind::NMI);
  ASSERT_EQ(R.Status, X64PagedEventStatus::BackingFault);
  ASSERT_TRUE(R.Continuation);
  EXPECT_TRUE(R.InputAccepted);
  EXPECT_EQ(State.NMIBlocked, 1);
  ASSERT_EQ(Memory->physicalMemory()
                .storeScalar(0x4030, GuestScalarWidth::I64, 0x6003)
                .Status,
            GuestMemoryAccessStatus::Completed);
  const auto Resumed = llvm::cantFail(
      resumeX64PagedEvent(State, *Memory, *R.Continuation, 0xffff));
  EXPECT_EQ(Resumed.Status, X64PagedEventStatus::Delivered);
  EXPECT_EQ(Resumed.DeliveredVector, 2);
  EXPECT_TRUE(Resumed.InputAccepted);
  EXPECT_FALSE(Resumed.Continuation);
  EXPECT_EQ(State.NMIBlocked, 1);
  EXPECT_EQ(get(State.Core.Core.Core.Core.GPR[4]), 0x9000u);
}

TEST_F(X64PagedEventTest, ResumeRetainsSecondaryFaultAndSoftwareRestartPC) {
  State.IDTR.Base = 0x6f80;
  for (unsigned V = 0; V != 256; ++V)
    gate(V);
  gate(3, 0x0e);
  put(0x4038, 0x800003); // The secondary #NP gate has no backing.
  Memory = llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
  X64ContinuationEventV1 Event;
  Event.Vector = 3;
  Event.Kind = X64ContinuationEventKind::Software;
  Event.SavedPC = 0x9001;
  const auto R =
      llvm::cantFail(deliverX64PagedEvent(State, *Memory, Event, 0xffff));
  ASSERT_EQ(R.Status, X64PagedEventStatus::BackingFault);
  ASSERT_TRUE(R.Continuation);
  EXPECT_EQ(R.Continuation->Event.Vector, 11);
  EXPECT_EQ(R.Continuation->FaultPC, 0x9000u);
  ASSERT_EQ(Memory->physicalMemory()
                .storeScalar(0x4038, GuestScalarWidth::I64, 0x7003)
                .Status,
            GuestMemoryAccessStatus::Completed);
  // Repairing the original gate must not replace the already selected #NP.
  ASSERT_EQ(Memory->physicalMemory()
                .storeScalar(0x6fb5, GuestScalarWidth::I8, 0x8e)
                .Status,
            GuestMemoryAccessStatus::Completed);
  const auto Resumed = llvm::cantFail(
      resumeX64PagedEvent(State, *Memory, *R.Continuation, 0xffff));
  EXPECT_EQ(Resumed.Status, X64PagedEventStatus::Delivered);
  EXPECT_EQ(Resumed.DeliveredVector, 11);
  const auto SP = State.Core.Core.Core.Core.GPR[4];
  EXPECT_EQ(get(SP), 26u);
  EXPECT_EQ(get(SP + 8), 0x9000u);
  EXPECT_EQ(get(SP + 24), 0x10202u);
}

TEST_F(X64PagedEventTest,
       ResumeDoesNotRereadDescriptorOverwrittenByPartialFrame) {
  State.Core.Core.Core.Core.GPR[4] = 0x5020;
  put(0x4020, 0x800003); // Final PC slot crosses into unbacked page four.
  const auto R = deliver(1, X64ContinuationEventKind::Trap);
  ASSERT_EQ(R.Status, X64PagedEventStatus::BackingFault);
  ASSERT_TRUE(R.Continuation);
  EXPECT_EQ(get(0x5008),
            0x202u); // Saved flags have overwritten the CS descriptor.
  EXPECT_EQ(get(0x5010), 0x5020u);
  EXPECT_EQ(get(0x5018), 16u);
  ASSERT_EQ(Memory->physicalMemory()
                .storeScalar(0x4020, GuestScalarWidth::I64, 0x14003)
                .Status,
            GuestMemoryAccessStatus::Completed);
  const auto Resumed = llvm::cantFail(
      resumeX64PagedEvent(State, *Memory, *R.Continuation, 0xffff));
  EXPECT_EQ(Resumed.Status, X64PagedEventStatus::Delivered);
  EXPECT_EQ(Resumed.DeliveredVector, 1);
  EXPECT_EQ(State.Core.Core.Core.Core.PC, 0x10010u);
  EXPECT_EQ(State.Core.CS.Attributes, 0xa09bu);
  EXPECT_EQ(State.Core.Core.Core.Core.GPR[4], 0x4ff8u);
  EXPECT_EQ(get(0x14ff8), 0x9000u);
}

} // namespace
