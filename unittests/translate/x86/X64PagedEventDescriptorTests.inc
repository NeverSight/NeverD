//===- X64PagedEventDescriptorTests.inc - Event descriptor ranges -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Independent software interrupt, IRQ and NMI descriptor observations.
//===----------------------------------------------------------------------===//

#if defined(__linux__) && defined(__x86_64__)
class X64PagedEventDescriptorTest : public X64PagedDescriptorBoundaryFixture {
protected:
  static std::vector<uint8_t> instruction(unsigned Kind) {
    return Kind == 0   ? std::vector<uint8_t>{0xcd, 0x80}
           : Kind == 1 ? std::vector<uint8_t>{0xcc}
                       : std::vector<uint8_t>{0x90};
  }

  static X64ContinuationEventV1 event(unsigned Kind) {
    X64ContinuationEventV1 Event;
    Event.Kind = Kind < 2    ? X64ContinuationEventKind::Software
                 : Kind == 2 ? X64ContinuationEventKind::Interrupt
                             : X64ContinuationEventKind::NMI;
    Event.Vector = Kind == 0 ? 0x80 : Kind == 1 ? 3 : Kind == 2 ? 0x30 : 2;
    Event.SavedPC = 0x1000 + (Kind < 2 ? instruction(Kind).size() : 0);
    return Event;
  }

  static X64PagedExecutionOptions options(bool TailGPZero) {
    X64PagedExecutionOptions Options;
    Options.EventDescriptorCanonicality =
        TailGPZero ? X86DescriptorCanonicalityProfile::SelectorForStart
                   : X86DescriptorCanonicalityProfile::SelectorForRange;
    return Options;
  }

  static uint32_t eventError(unsigned Kind, unsigned Case, bool TailGPZero) {
    return error(Case, TailGPZero) |
           (Kind >= 2 && (Case == 1 || Case == 2 || Case == 3) ? 1u : 0u);
  }

  void splitGate(unsigned Kind) {
    const uint64_t Base = 0x6ff8 - event(Kind).Vector * 16;
    auto &Bytes = RAM.Memory.front().Bytes;
    std::memmove(Bytes.data() + Base, Bytes.data() + 0x6000, 4096);
    State.IDTR.Base = Base;
  }

  uint64_t enter(unsigned Kind, Run Driver, Schedule Scheduled,
                 X64PagedExecution &Context, uint64_t &Budget) {
    auto *Opaque = reinterpret_cast<uint8_t *>(&Context);
    if (Kind < 2)
      return Driver(&State, Opaque, &Budget);
    Shadows Shadow;
    Producer P;
    P.Candidate = {event(Kind).Vector, uint8_t(event(Kind).Kind)};
    uint64_t Polls = 1;
    const auto Result = Scheduled(&State, Opaque, &Budget, &Shadow, &Polls,
                                  Producer::peek, Producer::accepted, &P);
    EXPECT_EQ(P.Polls, 1u);
    EXPECT_EQ(P.Acks, 1u);
    EXPECT_EQ(Polls, 0u);
    return Result;
  }

  void prepare(unsigned Kind, unsigned Case) {
    // Accessed descriptors isolate canonicality from locked accessed updates.
    descriptorBoundary(instruction(Kind), Case, 0x00af9b000000ffffULL);
    auto &Core = State.Core.Core.Core.Core;
    Core.GPR[4] = 0x1d000;
    Core.RFlags = 0x246;
    // Explicit event delivery does not fetch an instruction. Pre-access the
    // interrupted/target code page in both inputs for the hardware exit probe.
    put(0x13008, 0x1027);
    State.NMIBlocked = Kind != 3;
    put(0x6000 + event(Kind).Vector * 16,
        0x1800 | (uint64_t(0x24) << 16) | (uint64_t(0x8e) << 40));
    RAM.Memory.front().Bytes[0x1800] = 0x90;
    RAM.Memory.front().Bytes[0x1801] = 0x90;
  }
};

TEST_F(X64PagedEventDescriptorTest, ObserveOriginalRanges) {
  if (!neverd::test::hostCpuIsIntel())
    GTEST_SKIP() << neverd::test::IntelOracleOnlyReason;
  unsigned Observations = 0;
  for (unsigned Kind = 0; Kind != 4; ++Kind)
    for (unsigned Case = 0; Case != 10; ++Case) {
      SCOPED_TRACE(llvm::formatv("kind={0} case={1}", Kind, Case).str());
      X64PagedNativeCPU CPU;
      ASSERT_NO_FATAL_FAILURE(CPU.initialize());
      if (IsSkipped())
        return;
      if (!neverd::test::supportsX86KVMPrefault(CPU.KVM))
        GTEST_SKIP() << "Event descriptor oracle requires KVM pre-fault memory";
      ASSERT_NO_FATAL_FAILURE(CPU.pinForReturnComparison());
      if (CPU.ReturnCoreType != 0x20 && CPU.ReturnCoreType != 0x40)
        GTEST_SKIP() << "No event descriptor profile qualified for this core";
      CPU.PrefaultMemory = true;
      Features.PhysicalAddressBits = CPU.PhysicalBits;
      prepare(Kind, Case);
      const auto Event = event(Kind);
      X64ContinuationStateV6 Native;
      bool Fault = false;
      ASSERT_NO_FATAL_FAILURE(
          CPU.execute(RAM.Memory.front().Bytes, State, Controls.CR0,
                      instruction(Kind).size(), Native, Fault, false,
                      Kind < 2 ? nullptr : &Event, Controls.CR4, true, false));
      const auto &R = Native.Core.Core.Core.Core;
      uint64_t Error = 0;
      if (Fault) {
        ASSERT_LT(R.GPR[4] + 8, X64PagedNativeCPU::Size);
        for (unsigned I = 0; I != 8; ++I)
          Error |= uint64_t(CPU.RAM[R.GPR[4] + I]) << (I * 8);
      }
      RecordProperty(
          "event_descriptor_" + std::to_string(Kind) + "_" +
              std::to_string(Case),
          llvm::formatv(
              "core={0:x};fault={1};pc={2:x};error={3:x};cr2={4:x};sp={5:x};"
              "cs={6:x};ss={7:x};nmi={8};pml4={9:x};low_pte={10:x}",
              CPU.ReturnCoreType, Fault, R.PC, Error, Native.Core.Core.CR2,
              R.GPR[4], Native.Core.CS.Selector, Native.Core.SS.Selector,
              unsigned(Native.NMIBlocked),
              unsigned(CPU.RAM[0x10000 +
                               ((descriptorAddress(Case) >> 39) & 0x1ff) * 8]),
              unsigned(CPU.RAM[0x13000]))
              .str());
      ASSERT_LE(R.GPR[4], X64PagedNativeCPU::Size - 48);
      std::array<uint64_t, 6> Frame{};
      for (unsigned I = 0; I != Frame.size(); ++I)
        for (unsigned B = 0; B != 8; ++B)
          Frame[I] |= uint64_t(CPU.RAM[R.GPR[4] + I * 8 + B]) << (B * 8);
      RecordProperty(
          "event_frame_" + std::to_string(Kind) + "_" + std::to_string(Case),
          llvm::formatv("q0={0:x};q1={1:x};q2={2:x};q3={3:x};q4={4:x};q5={5:x}",
                        Frame[0], Frame[1], Frame[2], Frame[3], Frame[4],
                        Frame[5])
              .str());
      EXPECT_EQ(Fault, faults(Case));
      EXPECT_EQ(R.PC,
                Fault ? 0x4000 + (pageFault(Case) ? 14 : 13) * 16 : 0x1800);
      EXPECT_EQ(Error, eventError(Kind, Case, CPU.ReturnCoreType == 0x40));
      EXPECT_EQ(Native.Core.Core.CR2, cr2(Case));
      EXPECT_EQ(Native.NMIBlocked, 1u);
      ++Observations;
    }
  RecordProperty("original_code64_event_descriptor_observations", Observations);
}

TEST_F(X64PagedEventDescriptorTest, OwnerMatchesOriginalRanges) {
  if (!neverd::test::hostCpuIsIntel())
    GTEST_SKIP() << neverd::test::IntelOracleOnlyReason;
  unsigned Comparisons = 0;
  for (unsigned Kind = 0; Kind != 4; ++Kind)
    for (unsigned Case = 0; Case != 10; ++Case) {
      SCOPED_TRACE(llvm::formatv("kind={0} case={1}", Kind, Case).str());
      X64PagedNativeCPU CPU;
      ASSERT_NO_FATAL_FAILURE(CPU.initialize());
      if (IsSkipped())
        return;
      if (!neverd::test::supportsX86KVMPrefault(CPU.KVM))
        GTEST_SKIP() << "Event descriptor oracle requires KVM pre-fault memory";
      ASSERT_NO_FATAL_FAILURE(CPU.pinForReturnComparison());
      if (CPU.ReturnCoreType != 0x20 && CPU.ReturnCoreType != 0x40)
        GTEST_SKIP() << "No event descriptor profile qualified for this core";
      CPU.PrefaultMemory = true;
      Features.PhysicalAddressBits = CPU.PhysicalBits;
      prepare(Kind, Case);
      const auto Event = event(Kind);
      X64ContinuationStateV6 Native;
      bool Fault = false;
      ASSERT_NO_FATAL_FAILURE(
          CPU.execute(RAM.Memory.front().Bytes, State, Controls.CR0,
                      instruction(Kind).size(), Native, Fault, false,
                      Kind < 2 ? nullptr : &Event, Controls.CR4, true, false));
      Memory = llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
      auto Result = deliverX64PagedEvent(
          State, *Memory, Event, 0xffff, nullptr,
          X86InstructionDebugProfile::InstructionStep, {},
          options(CPU.ReturnCoreType == 0x40).EventDescriptorCanonicality);
      ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
      EXPECT_EQ(Result->Status, X64PagedEventStatus::Delivered);
      EXPECT_TRUE(Result->InputAccepted);
      EXPECT_EQ(State, Native);
      compareRAM(*Memory, {CPU.RAM, X64PagedNativeCPU::Size});
      ++Comparisons;
    }
  RecordProperty("original_code64_event_descriptor_owner_comparisons",
                 Comparisons);
}

TEST_F(X64PagedEventDescriptorTest, GeneratedRangesMatchOriginalEvents) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "No test Clang";
  if (!neverd::test::hostCpuIsIntel())
    GTEST_SKIP() << neverd::test::IntelOracleOnlyReason;
  unsigned Comparisons = 0;
  for (unsigned Kind = 0; Kind != 4; ++Kind)
    for (const auto &Compiler : compilers())
      for (bool LLVM : {false, true})
        for (const std::string O : {"-O0", "-O2"}) {
          Run Driver = nullptr;
          Schedule Scheduled = nullptr;
          ASSERT_NO_FATAL_FAILURE(compile(instruction(Kind), LLVM, O, Driver,
                                          {0x1000}, Compiler, 0x1000,
                                          Kind >= 2 ? &Scheduled : nullptr));
          for (unsigned Case = 0; Case != 10; ++Case) {
            SCOPED_TRACE(llvm::formatv("kind={0} case={1} llvm={2} {3} {4}",
                                       Kind, Case, LLVM, Compiler, O)
                             .str());
            X64PagedNativeCPU CPU;
            ASSERT_NO_FATAL_FAILURE(CPU.initialize());
            if (IsSkipped())
              return;
            if (!neverd::test::supportsX86KVMPrefault(CPU.KVM))
              GTEST_SKIP()
                  << "Event descriptor oracle requires KVM pre-fault memory";
            ASSERT_NO_FATAL_FAILURE(CPU.pinForReturnComparison());
            if (CPU.ReturnCoreType != 0x20 && CPU.ReturnCoreType != 0x40)
              GTEST_SKIP()
                  << "No event descriptor profile qualified for this core";
            CPU.PrefaultMemory = true;
            Features.PhysicalAddressBits = CPU.PhysicalBits;
            prepare(Kind, Case);
            const auto Event = event(Kind);
            X64ContinuationStateV6 Native;
            bool Fault = false;
            ASSERT_NO_FATAL_FAILURE(CPU.execute(
                RAM.Memory.front().Bytes, State, Controls.CR0,
                instruction(Kind).size(), Native, Fault, false,
                Kind < 2 ? nullptr : &Event, Controls.CR4, true, false));
            ASSERT_EQ(Fault, faults(Case));
            Memory =
                llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
            X64PagedExecution Context(*Memory,
                                      options(CPU.ReturnCoreType == 0x40));
            uint64_t Budget = 1;
            EXPECT_EQ(enter(Kind, Driver, Scheduled, Context, Budget), 7u)
                << Context.lastError();
            EXPECT_TRUE(Context.lastError().empty()) << Context.lastError();
            EXPECT_EQ(Budget, Kind < 2 ? 0u : 1u);
            EXPECT_EQ(Context.lastResult().InstructionAttempted, Kind < 2);
            EXPECT_EQ(Context.lastResult().InstructionCompleted,
                      Kind < 2 && !Fault);
            ASSERT_TRUE(Context.lastResult().Delivery);
            EXPECT_TRUE(Context.lastResult().Delivery->InputAccepted);
            if (Fault) {
              ASSERT_TRUE(Context.lastResult().Delivery->LastException);
              EXPECT_EQ(Context.lastResult().Delivery->LastException->ErrorCode,
                        eventError(Kind, Case, CPU.ReturnCoreType == 0x40));
            }
            EXPECT_EQ(State, Native);
            compareRAM(*Memory, {CPU.RAM, X64PagedNativeCPU::Size});
            ++Comparisons;
          }
        }
  RecordProperty("original_code64_event_descriptor_comparisons", Comparisons);
}

TEST_F(X64PagedEventDescriptorTest, ResumeRetainsAcceptedDescriptorProfile) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "No test Clang";
  unsigned Comparisons = 0;
  for (unsigned Kind = 0; Kind != 4; ++Kind)
    for (const auto &Compiler : compilers())
      for (bool LLVM : {false, true}) {
        Run Driver = nullptr;
        Schedule Scheduled = nullptr;
        ASSERT_NO_FATAL_FAILURE(compile(instruction(Kind), LLVM, "-O2", Driver,
                                        {0x1000}, Compiler, 0x1000,
                                        Kind >= 2 ? &Scheduled : nullptr));
        for (bool TailGPZero : {false, true})
          for (unsigned Pause = 0; Pause != 5; ++Pause) {
            SCOPED_TRACE(
                llvm::formatv("kind={0} pause={1} zero={2} llvm={3} {4}", Kind,
                              Pause, TailGPZero, LLVM, Compiler)
                    .str());
            prepare(Kind, Pause == 2 ? 8 : 2);
            if (Pause == 1)
              splitGate(Kind);
            if (Pause == 4)
              State.Core.Core.Core.Core.GPR[4] = 0x1d018;
            const auto Before = State;
            auto Options = options(TailGPZero);
            Memory =
                llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
            X64PagedExecution Direct(*Memory, Options);
            uint64_t Budget = 1;
            ASSERT_EQ(enter(Kind, Driver, Scheduled, Direct, Budget), 7u)
                << Direct.lastError();
            const auto Expected = State;
            const auto ExpectedRAM = bytes(*Memory);
            State = Before;
            const unsigned Page = Pause == 0   ? 0x6000
                                  : Pause == 1 ? 0x7000
                                  : Pause == 2 ? 0x17000
                                               : 0x1c000;
            Memory = Pause == 3 ? llvm::cantFail(X86PagedMemory::create(
                                      RAM, Features, Controls))
                                : without(Page);
            if (Pause == 3)
              Options.EventDelivery.MaxAttempts = 1;
            X64PagedExecution First(*Memory, Options);
            Budget = 1;
            ASSERT_EQ(enter(Kind, Driver, Scheduled, First, Budget),
                      Pause == 3 ? uint64_t(X64PagedExit::EventBudgetExhausted)
                                 : uint64_t(X64PagedExit::BackingFault))
                << First.lastError();
            EXPECT_EQ(Budget, Kind < 2 ? 0u : 1u);
            ASSERT_TRUE(First.lastResult().Delivery);
            ASSERT_TRUE(First.lastResult().Delivery->Continuation);
            auto Pending = *First.lastResult().Delivery->Continuation;
            EXPECT_EQ(Pending.DescriptorCanonicality,
                      Options.EventDescriptorCanonicality);
            EXPECT_EQ(Pending.EventAccepted, Pause != 3);
            EXPECT_EQ(State.NMIBlocked, 1u);
            if (Pause == 1)
              EXPECT_EQ(Pending.CompletedAccesses.size(), 1u);
            if (Pause == 3) {
              EXPECT_TRUE(Pending.CompletedAccesses.empty());
              EXPECT_EQ(Pending.Event.Vector, 13);
              EXPECT_EQ(Pending.Event.ErrorCode,
                        eventError(Kind, 2, TailGPZero));
            } else {
              const auto StoppedState = State;
              const auto StoppedRAM = bytes(*Memory);
              auto Again = First.resume(State, Pending, 0xffff);
              ASSERT_TRUE(bool(Again)) << llvm::toString(Again.takeError());
              EXPECT_EQ(Again->Exit, uint64_t(X64PagedExit::BackingFault));
              EXPECT_FALSE(Again->InstructionAttempted);
              EXPECT_EQ(State, StoppedState);
              compareRAM(*Memory, StoppedRAM);
            }
            auto Repaired =
                Pause == 3 ? std::move(Memory) : repair(*Memory, Page);
            const auto StoppedState = State;
            const auto StoppedRAM = bytes(*Repaired);
            const auto StoppedControls = Repaired->controls();
            auto Changed = Options;
            Changed.EventDescriptorCanonicality =
                options(!TailGPZero).EventDescriptorCanonicality;
            X64PagedExecution Refused(*Repaired, Changed);
            auto Rejected = Refused.resume(State, Pending, 0xffff);
            ASSERT_FALSE(bool(Rejected));
            llvm::consumeError(Rejected.takeError());
            EXPECT_EQ(State, StoppedState);
            EXPECT_EQ(Repaired->controls(), StoppedControls);
            compareRAM(*Repaired, StoppedRAM);
            // The lower API consumes the retained policy without a replacement
            // argument. Exercise it independently of ordinary context refusal.
            auto Snapshot = RAM;
            Snapshot.Memory =
                Repaired->physicalMemory().snapshotMemoryRegions();
            auto LowerMemory = llvm::cantFail(X86PagedMemory::create(
                Snapshot, Repaired->features(), Repaired->controls()));
            auto LowerState = State;
            auto Lower =
                resumeX64PagedEvent(LowerState, *LowerMemory, Pending, 0xffff);
            ASSERT_TRUE(bool(Lower)) << llvm::toString(Lower.takeError());
            EXPECT_EQ(Lower->Status, X64PagedEventStatus::Delivered);
            EXPECT_EQ(LowerState, Expected);
            compareRAM(*LowerMemory, ExpectedRAM);
            Options.EventDelivery = {};
            X64PagedExecution Resumed(*Repaired, Options);
            auto Done = Resumed.resume(State, Pending, 0xffff);
            ASSERT_TRUE(bool(Done)) << llvm::toString(Done.takeError());
            EXPECT_EQ(Done->Exit, 7u);
            EXPECT_FALSE(Done->InstructionAttempted);
            EXPECT_EQ(Budget, Kind < 2 ? 0u : 1u);
            EXPECT_EQ(State, Expected);
            compareRAM(*Repaired, ExpectedRAM);
            ++Comparisons;
          }
      }
  RecordProperty("event_descriptor_resume_comparisons", Comparisons);
}

TEST_F(X64PagedEventDescriptorTest, PendingInstructionsRetainEventBinding) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "No test Clang";
  unsigned Comparisons = 0;
  for (unsigned Form = 0; Form != 3; ++Form)
    for (const auto &Compiler : compilers())
      for (bool LLVM : {false, true}) {
        const std::vector<uint8_t> Code =
            Form == 0   ? std::vector<uint8_t>{0x48, 0xcf}
            : Form == 1 ? std::vector<uint8_t>{0x8e, 0x1f}
                        : std::vector<uint8_t>{0x48, 0x0f, 0xb5, 0x07};
        Run Driver = nullptr;
        ASSERT_NO_FATAL_FAILURE(
            compile(Code, LLVM, "-O2", Driver, {0x1000}, Compiler));
        for (bool TailGPZero : {false, true}) {
          SCOPED_TRACE(llvm::formatv("form={0} zero={1} llvm={2} {3}", Form,
                                     TailGPZero, LLVM, Compiler)
                           .str());
          descriptorBoundary(Code, 2, 0x00af9b000000ffffULL);
          State.NMIBlocked = 1;
          State.Core.Core.Core.Core.GPR[4] = 0x1d000;
          put(0x1d000, 0x1800);
          put(0x1d008, 0x24);
          put(0x1d010, 0x246);
          put(0x1d018, 0x1d000);
          put(0x1d020, 0x10);
          put(0x30000, Form == 1 ? 0x24 : 0x1800);
          put(0x30008, 0x24);
          const auto Before = State;
          auto Options = options(TailGPZero);
          Options.IretAccessOrder = X64PagedIretAccessOrder::FlagsFirst;
          Options.SegmentMoveProfile =
              X64PagedSegmentMoveProfile::SelectorFirst;
          Options.FarPointerProfile = X64PagedFarPointerProfile::OffsetFirst;
          Memory =
              llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
          X64PagedExecution Direct(*Memory, Options);
          uint64_t Budget = 1;
          ASSERT_EQ(
              Driver(&State, reinterpret_cast<uint8_t *>(&Direct), &Budget), 7u)
              << Direct.lastError();
          const auto Expected = State;
          const auto ExpectedRAM = bytes(*Memory);
          State = Before;
          const unsigned Page = Form == 0 ? 0x1d000 : 0x30000;
          Memory = without(Page);
          X64PagedExecution First(*Memory, Options);
          Budget = 1;
          ASSERT_EQ(
              Driver(&State, reinterpret_cast<uint8_t *>(&First), &Budget), 18u)
              << First.lastError();
          const auto Pending = First.continuation();
          ASSERT_TRUE(Pending);
          auto Repaired = repair(*Memory, Page);
          const auto Stopped = State;
          const auto StoppedRAM = bytes(*Repaired);
          const auto StoppedControls = Repaired->controls();
          auto Changed = Options;
          Changed.EventDescriptorCanonicality =
              options(!TailGPZero).EventDescriptorCanonicality;
          X64PagedExecution Refused(*Repaired, Changed, Pending);
          EXPECT_EQ(
              Driver(&State, reinterpret_cast<uint8_t *>(&Refused), &Budget),
              5u);
          EXPECT_FALSE(Refused.lastError().empty());
          EXPECT_EQ(Refused.continuation(), Pending);
          EXPECT_FALSE(Refused.lastResult().InstructionAttempted);
          EXPECT_EQ(Budget, Form == 0 ? 0u : 1u);
          EXPECT_EQ(State, Stopped);
          EXPECT_EQ(Repaired->controls(), StoppedControls);
          compareRAM(*Repaired, StoppedRAM);
          X64PagedExecution Resumed(*Repaired, Options, Pending);
          ASSERT_EQ(
              Driver(&State, reinterpret_cast<uint8_t *>(&Resumed), &Budget),
              7u)
              << Resumed.lastError();
          EXPECT_EQ(Budget, 0u);
          EXPECT_EQ(State, Expected);
          compareRAM(*Repaired, ExpectedRAM);
          ++Comparisons;
        }
      }
  RecordProperty("event_descriptor_instruction_resume_comparisons",
                 Comparisons);
}

TEST_F(X64PagedEventDescriptorTest, InvalidProfileRefusesBeforeEffects) {
  for (unsigned Kind = 0; Kind != 4; ++Kind) {
    prepare(Kind, 8);
    const auto Before = State;
    Memory = llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
    const auto BeforeControls = Memory->controls();
    const auto BeforeRAM = bytes(*Memory);
    const auto Invalid = static_cast<X86DescriptorCanonicalityProfile>(0xff);
    auto Got = deliverX64PagedEvent(
        State, *Memory, event(Kind), 0xffff, nullptr,
        X86InstructionDebugProfile::InstructionStep, {}, Invalid);
    ASSERT_FALSE(bool(Got));
    llvm::consumeError(Got.takeError());
    auto Options = options(true);
    Options.EventDescriptorCanonicality = Invalid;
    X64PagedExecution Context(*Memory, Options);
    auto Refused = Context.deliver(State, event(Kind), 0xffff);
    ASSERT_FALSE(bool(Refused));
    llvm::consumeError(Refused.takeError());
    X64PagedEventContinuation Pending;
    Pending.Event = event(Kind);
    Pending.DescriptorCanonicality = Invalid;
    auto Resume = resumeX64PagedEvent(State, *Memory, Pending, 0xffff);
    ASSERT_FALSE(bool(Resume));
    llvm::consumeError(Resume.takeError());
    EXPECT_EQ(State, Before);
    EXPECT_EQ(Memory->controls(), BeforeControls);
    compareRAM(*Memory, BeforeRAM);
  }
}

TEST_F(X64PagedEventDescriptorTest, SoftwareDebugSequenceRetainsBinding) {
  unsigned Comparisons = 0;
  for (unsigned Kind : {0u, 1u})
    for (bool TailGPZero : {false, true}) {
      prepare(Kind, 2);
      put(0x6000 + event(Kind).Vector * 16,
          0x1800 | (uint64_t(8) << 16) | (uint64_t(0x8e) << 40));
      put(0x6010, 0x4010 | (uint64_t(0x24) << 16) | (uint64_t(0x8e) << 40));
      const auto Before = State;
      X64PagedDebugState Debug;
      Debug.DR[0] = 0x1cff8;
      Debug.DR7 = 0x10401;
      const auto BeforeDebug = Debug;
      auto Options = options(TailGPZero);
      Options.Debug = &Debug;
      Options.DebugProfile = X86InstructionDebugProfile::Watchpoints;
      Memory = llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
      X64PagedExecution Direct(*Memory, Options);
      auto Complete = Direct.deliver(State, event(Kind), 0xffff);
      ASSERT_TRUE(bool(Complete)) << llvm::toString(Complete.takeError());
      ASSERT_TRUE(Complete->Delivery);
      EXPECT_TRUE(Complete->Delivery->SoftwareCompleted);
      EXPECT_EQ(Complete->Delivery->DeliveredVector, 13u);
      const auto Expected = State;
      const auto ExpectedDebug = Debug;
      const auto ExpectedRAM = bytes(*Memory);
      State = Before;
      Debug = BeforeDebug;
      Memory = llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
      Options.EventDelivery.MaxAttempts = 1;
      X64PagedExecution First(*Memory, Options);
      auto Paused = First.deliver(State, event(Kind), 0xffff);
      ASSERT_TRUE(bool(Paused)) << llvm::toString(Paused.takeError());
      ASSERT_TRUE(Paused->Delivery && Paused->Delivery->Continuation);
      const auto Pending = *Paused->Delivery->Continuation;
      EXPECT_FALSE(Pending.EventAccepted);
      EXPECT_TRUE(Pending.SoftwareCompleted);
      EXPECT_EQ(Pending.Event.Kind, X64ContinuationEventKind::Trap);
      EXPECT_EQ(Pending.Event.Vector, 1u);
      EXPECT_EQ(Pending.DescriptorCanonicality,
                Options.EventDescriptorCanonicality);
      const auto StoppedState = State;
      const auto StoppedDebug = Debug;
      const auto StoppedRAM = bytes(*Memory);
      auto Changed = Options;
      Changed.EventDescriptorCanonicality =
          options(!TailGPZero).EventDescriptorCanonicality;
      X64PagedExecution Refused(*Memory, Changed);
      auto Bad = Refused.resume(State, Pending, 0xffff);
      ASSERT_FALSE(bool(Bad));
      llvm::consumeError(Bad.takeError());
      EXPECT_EQ(State, StoppedState);
      EXPECT_EQ(Debug, StoppedDebug);
      compareRAM(*Memory, StoppedRAM);
      auto Done = resumeX64PagedEvent(State, *Memory, Pending, 0xffff, &Debug,
                                      Options.DebugProfile);
      ASSERT_TRUE(bool(Done)) << llvm::toString(Done.takeError());
      EXPECT_EQ(Done->Status, X64PagedEventStatus::Delivered);
      EXPECT_TRUE(Done->SoftwareCompleted);
      EXPECT_EQ(Done->DeliveredVector, 13u);
      ASSERT_TRUE(Done->LastException);
      EXPECT_EQ(Done->LastException->ErrorCode, TailGPZero ? 1u : 0x25u);
      EXPECT_EQ(State, Expected);
      EXPECT_EQ(Debug, ExpectedDebug);
      compareRAM(*Memory, ExpectedRAM);
      ++Comparisons;
    }
  RecordProperty("event_descriptor_software_debug_comparisons", Comparisons);
}

TEST_F(X64PagedEventDescriptorTest, FullModeRetainsAcceptedEventProfile) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "No test Clang";
  unsigned Comparisons = 0;
  for (unsigned Kind = 0; Kind != 4; ++Kind)
    for (const auto &Compiler : compilers())
      for (bool LLVM : {false, true}) {
        const auto Code = instruction(Kind);
        llvm::json::Object Settings{
            {"profile", "kernel-events-v6"},
            {"mxcsr_mask", "0xffff"},
            {"paging_address_bits", 48},
            {"paged_memory", true},
            {"instructions", llvm::json::Array{0x1000}}};
        const auto Request =
            llvm::formatv("{0}", llvm::json::Value(std::move(Settings))).str();
        auto Export = exportX64Continuation(image(Code), Request, LLVM);
        ASSERT_TRUE(bool(Export)) << llvm::toString(Export.takeError());
        const auto Manifest =
            llvm::cantFail(llvm::json::parse(Export->Manifest));
        const auto *Units = Manifest.getAsObject()->getArray("units");
        ASSERT_NE(Units, nullptr);
        ASSERT_EQ(Units->size(), 1u);
        const auto Symbol = Units->front().getAsObject()->getString("name");
        ASSERT_TRUE(Symbol);
        const auto Adapters =
            "\nuint64_t mode_body(void *s, void *e) { return " + Symbol->str() +
            "_paged_adapter(s,e); }\n" +
            "uint64_t mode_validate(void *s, void *e) { return " +
            Symbol->str() + "_validate_paged_adapter(s,e); }\n";
        llvm::sys::DynamicLibrary DL;
        const auto Name = std::string("event-descriptor-mode-") +
                          (LLVM ? "llvm" : "high") + "-" +
                          std::to_string(Compilation++);
        ASSERT_NO_FATAL_FAILURE(
            compileProgram(*Export, Name, Compiler, "-O2", DL, Adapters));
        X86ModeInstruction Instruction{0x1000, X86ExecutionMode::Long64, Code};
        Instruction.Body =
            reinterpret_cast<X64PagedBody>(DL.getAddressOfSymbol("mode_body"));
        Instruction.Validate = reinterpret_cast<X64PagedBody>(
            DL.getAddressOfSymbol("mode_validate"));
        ASSERT_NE(Instruction.Body, nullptr);
        ASSERT_NE(Instruction.Validate, nullptr);
        for (bool TailGPZero : {false, true})
          for (unsigned Pause = 0; Pause != 3; ++Pause) {
            SCOPED_TRACE(
                llvm::formatv("kind={0} pause={1} zero={2} llvm={3} {4}", Kind,
                              Pause, TailGPZero, LLVM, Compiler)
                    .str());
            prepare(Kind, 2);
            if (Pause == 1)
              splitGate(Kind);
            const auto Before = State;
            Memory =
                llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
            auto Direct = deliverX64PagedEvent(
                State, *Memory, event(Kind), 0xffff, nullptr,
                X86InstructionDebugProfile::InstructionStep, {},
                options(TailGPZero).EventDescriptorCanonicality);
            ASSERT_TRUE(bool(Direct)) << llvm::toString(Direct.takeError());
            ASSERT_EQ(Direct->Status, X64PagedEventStatus::Delivered);
            const auto Expected = State;
            const auto ExpectedRAM = bytes(*Memory);
            X86ModeState Machine;
            Machine.Core.Core = Before;
            X86ModeExecutionOptions Options;
            Options.Long = options(TailGPZero);
            if (Pause == 2)
              Options.Long.EventDelivery.MaxAttempts = 1;
            Options.MXCSRMask = 0xffff;
            Options.PagingAddressBits = 48;
            Options.Undefined.emplace();
            Options.Legacy.Segments.FullLimitChoice =
                X86FullSegmentLimitChoice::Wrap;
            const unsigned Page = Pause == 0 ? 0x6000 : 0x7000;
            Memory = Pause == 2 ? llvm::cantFail(X86PagedMemory::create(
                                      RAM, Features, Controls))
                                : without(Page);
            X86ModeExecution First(*Memory, Options);
            uint64_t Budget = 1;
            auto Stopped = Kind < 2 ? First.run(Machine, {Instruction}, Budget)
                                    : First.interrupt(Machine, event(Kind).Kind,
                                                      event(Kind).Vector);
            ASSERT_TRUE(bool(Stopped)) << llvm::toString(Stopped.takeError());
            EXPECT_TRUE(Stopped->OwnerError.empty()) << Stopped->OwnerError;
            EXPECT_EQ(Stopped->Status,
                      Pause == 2
                          ? X86LegacyExecutionStatus::EventBudgetExhausted
                          : X86LegacyExecutionStatus::BackingFault);
            EXPECT_EQ(Budget, Kind < 2 ? 0u : 1u);
            EXPECT_TRUE(Stopped->EventAccepted);
            ASSERT_TRUE(Machine.Pending);
            auto Repaired =
                Pause == 2 ? std::move(Memory) : repair(*Memory, Page);
            Options.Long = options(!TailGPZero);
            X86ModeExecution Resumed(*Repaired, Options);
            auto Done = Resumed.run(Machine, {}, Budget);
            ASSERT_TRUE(bool(Done)) << llvm::toString(Done.takeError());
            EXPECT_TRUE(Done->OwnerError.empty()) << Done->OwnerError;
            EXPECT_EQ(Done->Status, X86LegacyExecutionStatus::EventDelivered);
            EXPECT_EQ(Done->Attempts, 0u);
            EXPECT_EQ(Done->Completions, 0u);
            EXPECT_FALSE(Done->EventAccepted);
            EXPECT_EQ(Budget, Kind < 2 ? 0u : 1u);
            EXPECT_FALSE(Machine.Pending);
            EXPECT_EQ(Machine.Core.Core, Expected);
            compareRAM(*Repaired, ExpectedRAM);
            ++Comparisons;
          }
      }
  RecordProperty("event_descriptor_full_mode_comparisons", Comparisons);
}

#endif
