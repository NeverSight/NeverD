//===- X64PagedEventAccessedTests.inc - Event descriptor updates --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Original event accessed updates, including a descriptor/page-table alias.
//===----------------------------------------------------------------------===//

#if defined(__linux__) && defined(__x86_64__)
class X64PagedEventAccessedTest : public X64PagedEventDescriptorTest {
protected:
  static constexpr uint64_t AliasTable = 0x209000;

  void prepareUpdate(unsigned Kind, unsigned Case) {
    if (Case < 4) {
      const bool Last = Case >= 2;
      prepare(Kind, Last ? 5 : 8);
      RAM.Memory.front().Bytes[Last ? 0x17ffd : 0x17025] &= ~uint8_t(1);
      if (Case & 1)
        put(Last ? 0x16ff8 : 0x16000, 0x17001);
      return;
    }
    prepare(Kind, 8);
    // Put the LDT code descriptor four bytes before its own PTE. This makes
    // write translation's Dirty bit a non-accessed descriptor bit. The whole
    // descriptor fits within one cache line; no split-lock policy is assumed.
    State.LDTR.Base = 0x5004;
    State.GDTR.Base = 0xa000;
    auto &Bytes = RAM.Memory.front().Bytes;
    std::copy_n(Bytes.begin() + 0x5000, 4096, Bytes.begin() + 0xa000);
    put(0x12000, AliasTable | 3);
    std::vector<uint8_t> Table(Bytes.begin() + 0x13000,
                               Bytes.begin() + 0x14000);
    const unsigned Variant = Case - 4;
    uint64_t Entry = 0x209a03;
    if (Variant == 1 || Variant == 2 || Variant == 5)
      Entry |= 0x20;
    if (Variant == 2)
      Entry |= 0x40;
    if (Variant == 3)
      Entry |= 0x100; // Descriptor.A already set; PTE global bit.
    if (Variant >= 4)
      Entry &= ~uint64_t(2);
    for (unsigned I = 0; I != 8; ++I)
      Table[0x28 + I] = Entry >> (I * 8);
    RAM.Memory.push_back({AliasTable,
                          MemoryPermission::Read | MemoryPermission::Write, 0,
                          std::move(Table)});
  }

  static uint64_t word(const uint8_t *Bytes) {
    uint64_t Value = 0;
    for (unsigned I = 0; I != 8; ++I)
      Value |= uint64_t(Bytes[I]) << (I * 8);
    return Value;
  }

  static void compareRegions(const std::vector<GuestMemoryRegion> &Actual,
                             const std::vector<GuestMemoryRegion> &Expected) {
    ASSERT_EQ(Actual.size(), Expected.size());
    for (unsigned I = 0; I != Actual.size(); ++I) {
      EXPECT_EQ(Actual[I].Address, Expected[I].Address);
      EXPECT_EQ(Actual[I].Permissions, Expected[I].Permissions);
      EXPECT_EQ(Actual[I].Bytes, Expected[I].Bytes) << "region " << I;
    }
  }

  void preparePause(unsigned Kind, unsigned Pause) {
    prepareUpdate(Kind, Pause == 2 || Pause == 3 ? 4 : Pause == 4 ? 6 : 0);
    if (Pause == 3)
      State.Core.Core.Core.Core.GPR[4] = 0x1d018;
    if (Pause == 4) {
      // Physical read-only backing must reach the descriptor write itself,
      // without an earlier page-walk A/D write stopping the initial reads.
      auto &Table = RAM.Memory.back().Bytes;
      for (unsigned I = 0; I != Table.size(); I += 8)
        if (Table[I] & 1)
          Table[I] |= 0x60;
    }
    if (Pause == 5) {
      // The old-RSP frame field overwrites the completed descriptor update.
      // Its following FLAGS slot has no physical backing until resume.
      State.Core.Core.Core.Core.GPR[4] = 0x1d030;
      put(0x130e8, 0x17003);
    }
  }

  std::unique_ptr<X86PagedMemory> pausedMemory(unsigned Pause) {
    auto Snapshot = RAM;
    if (Pause == 4) {
      Snapshot.Memory.back().Permissions = MemoryPermission::Read;
    } else {
      const uint64_t Address = Pause == 0   ? 0x17000
                               : Pause == 1 ? 0x17020
                               : Pause == 5 ? 0x17018
                                            : 0x1c000;
      const uint64_t Size = Pause == 1 || Pause == 5 ? 8 : 4096;
      const auto Original = Snapshot.Memory.front();
      Snapshot.Memory.erase(Snapshot.Memory.begin());
      Snapshot.Memory.insert(
          Snapshot.Memory.begin(),
          {Address + Size,
           Original.Permissions,
           0,
           {Original.Bytes.begin() + Address + Size, Original.Bytes.end()}});
      if (Pause == 1)
        Snapshot.Memory.insert(Snapshot.Memory.begin(),
                               {Address,
                                MemoryPermission::Read,
                                0,
                                {Original.Bytes.begin() + Address,
                                 Original.Bytes.begin() + Address + Size}});
      Snapshot.Memory.insert(
          Snapshot.Memory.begin(),
          {0,
           Original.Permissions,
           0,
           {Original.Bytes.begin(), Original.Bytes.begin() + Address}});
    }
    return llvm::cantFail(X86PagedMemory::create(Snapshot, Features, Controls));
  }

  std::unique_ptr<X86PagedMemory> repairedMemory(X86PagedMemory &Current) {
    // Rebuild the original writable regions, retaining every completed byte
    // and restoring only missing/permission-limited physical backing.
    auto Snapshot = RAM;
    for (const auto &Region :
         Current.physicalMemory().snapshotMemoryRegions()) {
      auto &Destination = Region.Address >= AliasTable
                              ? Snapshot.Memory.back()
                              : Snapshot.Memory.front();
      std::copy(Region.Bytes.begin(), Region.Bytes.end(),
                Destination.Bytes.begin() + Region.Address -
                    Destination.Address);
    }
    return llvm::cantFail(X86PagedMemory::create(Snapshot, Current.features(),
                                                 Current.controls()));
  }

  void compareAll(X86PagedMemory &Actual, const X64PagedNativeCPU &CPU) {
    const auto Regions = Actual.physicalMemory().snapshotMemoryRegions();
    ASSERT_EQ(Regions.size(), 1 + CPU.ExtraMemory.size());
    for (const auto &Region : Regions) {
      const uint8_t *Expected = CPU.RAM;
      uint64_t Size = X64PagedNativeCPU::Size;
      if (Region.Address) {
        ASSERT_EQ(CPU.ExtraMemory.size(), 1u);
        EXPECT_EQ(Region.Address, CPU.ExtraMemory.front().Address);
        Expected = CPU.ExtraMemory.front().Bytes;
        Size = CPU.ExtraMemory.front().Size;
      }
      ASSERT_EQ(Region.Bytes.size(), Size);
      const auto Difference =
          std::mismatch(Region.Bytes.begin(), Region.Bytes.end(), Expected);
      EXPECT_EQ(Difference.first, Region.Bytes.end())
          << "physical RAM differs at "
          << Region.Address + (Difference.first - Region.Bytes.begin());
    }
  }
};

TEST_F(X64PagedEventAccessedTest, ObserveOriginalUpdates) {
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
        GTEST_SKIP() << "Event update oracle requires KVM pre-fault memory";
      ASSERT_NO_FATAL_FAILURE(CPU.pinForReturnComparison());
      if (CPU.ReturnCoreType != 0x20 && CPU.ReturnCoreType != 0x40)
        GTEST_SKIP() << "No event update profile qualified for this core";
      CPU.PrefaultMemory = true;
      Features.PhysicalAddressBits = CPU.PhysicalBits;
      prepareUpdate(Kind, Case);
      if (Case >= 4)
        ASSERT_NO_FATAL_FAILURE(
            CPU.addMemory(AliasTable, RAM.Memory.back().Bytes));
      const auto Event = event(Kind);
      if (Case >= 4)
        RecordProperty(
            "initial_alias_" + std::to_string(Kind) + "_" +
                std::to_string(Case),
            llvm::formatv("pte={0:x};descriptor={1:x}",
                          word(RAM.Memory.back().Bytes.data() + 0x28),
                          word(RAM.Memory.back().Bytes.data() + 0x24))
                .str());
      X64ContinuationStateV6 Native;
      bool Fault = false;
      ASSERT_NO_FATAL_FAILURE(
          CPU.execute(RAM.Memory.front().Bytes, State, Controls.CR0,
                      instruction(Kind).size(), Native, Fault, false,
                      Kind < 2 ? nullptr : &Event, Controls.CR4, true, false));
      const auto &Core = Native.Core.Core.Core.Core;
      RecordProperty(
          "event_accessed_" + std::to_string(Kind) + "_" + std::to_string(Case),
          llvm::formatv(
              "core={0:x};fault={1};pc={2:x};cr2={3:x};cs_base={4:x};cs_limit={"
              "5:x};cs_attr={6:x};pte={7:x};descriptor={8:x}",
              CPU.ReturnCoreType, Fault, Core.PC, Native.Core.Core.CR2,
              Native.Core.CS.Base, Native.Core.CS.Limit,
              Native.Core.CS.Attributes,
              Case >= 4 ? word(CPU.ExtraMemory.front().Bytes + 0x28) : 0,
              Case >= 4 ? word(CPU.ExtraMemory.front().Bytes + 0x24)
                        : word(CPU.RAM + (Case >= 2 ? 0x17ff8 : 0x17020)))
              .str());
      EXPECT_EQ(Fault, Case == 1 || Case == 3 || Case >= 8);
      EXPECT_EQ(Core.PC, Fault ? 0x40e0u : 0x1800u);
      ++Observations;
    }
  RecordProperty("original_code64_event_accessed_observations", Observations);
}

TEST_F(X64PagedEventAccessedTest, OwnerMatchesOriginalUpdates) {
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
        GTEST_SKIP() << "Event update oracle requires KVM pre-fault memory";
      ASSERT_NO_FATAL_FAILURE(CPU.pinForReturnComparison());
      if (CPU.ReturnCoreType != 0x20 && CPU.ReturnCoreType != 0x40)
        GTEST_SKIP() << "No event update profile qualified for this core";
      CPU.PrefaultMemory = true;
      Features.PhysicalAddressBits = CPU.PhysicalBits;
      prepareUpdate(Kind, Case);
      if (Case >= 4)
        ASSERT_NO_FATAL_FAILURE(
            CPU.addMemory(AliasTable, RAM.Memory.back().Bytes));
      const auto Event = event(Kind);
      if (Case >= 4)
        RecordProperty(
            "initial_alias_" + std::to_string(Kind) + "_" +
                std::to_string(Case),
            llvm::formatv("pte={0:x};descriptor={1:x}",
                          word(RAM.Memory.back().Bytes.data() + 0x28),
                          word(RAM.Memory.back().Bytes.data() + 0x24))
                .str());
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
      const auto Regions = Memory->physicalMemory().snapshotMemoryRegions();
      RecordProperty(
          "model_accessed_" + std::to_string(Kind) + "_" + std::to_string(Case),
          llvm::formatv(
              "pc={0:x};cr2={1:x};cs_base={2:x};cs_limit={3:x};cs_attr={4:x};"
              "pte={5:x}",
              State.Core.Core.Core.Core.PC, State.Core.Core.CR2,
              State.Core.CS.Base, State.Core.CS.Limit, State.Core.CS.Attributes,
              Case >= 4 ? word(Regions.back().Bytes.data() + 0x28) : 0)
              .str());
      EXPECT_EQ(State, Native);
      compareAll(*Memory, CPU);
      ++Comparisons;
    }
  RecordProperty("original_code64_event_accessed_owner_comparisons",
                 Comparisons);
}

TEST_F(X64PagedEventAccessedTest, GeneratedUpdatesMatchOriginalEvents) {
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
                  << "Event update oracle requires KVM pre-fault memory";
            ASSERT_NO_FATAL_FAILURE(CPU.pinForReturnComparison());
            if (CPU.ReturnCoreType != 0x20 && CPU.ReturnCoreType != 0x40)
              GTEST_SKIP() << "No event update profile qualified for this core";
            CPU.PrefaultMemory = true;
            Features.PhysicalAddressBits = CPU.PhysicalBits;
            prepareUpdate(Kind, Case);
            if (Case >= 4)
              ASSERT_NO_FATAL_FAILURE(
                  CPU.addMemory(AliasTable, RAM.Memory.back().Bytes));
            const auto Event = event(Kind);
            X64ContinuationStateV6 Native;
            bool Fault = false;
            ASSERT_NO_FATAL_FAILURE(CPU.execute(
                RAM.Memory.front().Bytes, State, Controls.CR0,
                instruction(Kind).size(), Native, Fault, false,
                Kind < 2 ? nullptr : &Event, Controls.CR4, true, false));
            ASSERT_EQ(Fault, Case == 1 || Case == 3 || Case >= 8);
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
              EXPECT_EQ(Context.lastResult().Delivery->LastException->Vector,
                        14);
              EXPECT_EQ(Context.lastResult().Delivery->LastException->ErrorCode,
                        3u);
            }
            EXPECT_EQ(State, Native);
            compareAll(*Memory, CPU);
            ++Comparisons;
          }
        }
  RecordProperty("original_code64_event_accessed_comparisons", Comparisons);
}

TEST_F(X64PagedEventAccessedTest, OriginalWriteWatchpointsMatchUpdateRange) {
  if (!neverd::test::hostCpuIsIntel())
    GTEST_SKIP() << neverd::test::IntelOracleOnlyReason;
  unsigned Comparisons = 0;
  for (unsigned Kind = 0; Kind != 4; ++Kind)
    for (bool Accessed : {false, true})
      for (unsigned Offset : {0, 4, 5, 7}) {
        SCOPED_TRACE(llvm::formatv("kind={0} accessed={1} offset={2}", Kind,
                                   Accessed, Offset)
                         .str());
        X64PagedNativeCPU CPU;
        ASSERT_NO_FATAL_FAILURE(CPU.initialize());
        if (IsSkipped())
          return;
        if (!neverd::test::supportsX86KVMPrefault(CPU.KVM))
          GTEST_SKIP() << "Event update oracle requires KVM pre-fault memory";
        ASSERT_NO_FATAL_FAILURE(CPU.pinForReturnComparison());
        if (CPU.ReturnCoreType != 0x20 && CPU.ReturnCoreType != 0x40)
          GTEST_SKIP() << "No event update profile qualified for this core";
        CPU.PrefaultMemory = true;
        Features.PhysicalAddressBits = CPU.PhysicalBits;
        prepareUpdate(Kind, Accessed ? 7 : 4);
        RAM.Memory.front().Bytes[0x1800] = 0xf4;
        X64PagedDebugState Debug;
        Debug.DR[0] = 0x5024 + Offset;
        Debug.DR7 = 0x10401;
        CPU.DebugRegisters.emplace();
        CPU.DebugRegisters->db[0] = Debug.DR[0];
        CPU.DebugRegisters->dr6 = Debug.DR6;
        CPU.DebugRegisters->dr7 = Debug.DR7;
        ASSERT_NO_FATAL_FAILURE(
            CPU.addMemory(AliasTable, RAM.Memory.back().Bytes));
        const auto Event = event(Kind);
        X64ContinuationStateV6 Native;
        bool Halted = false;
        ASSERT_NO_FATAL_FAILURE(CPU.execute(
            RAM.Memory.front().Bytes, State, Controls.CR0,
            instruction(Kind).size(), Native, Halted, true,
            Kind < 2 ? nullptr : &Event, Controls.CR4, true, false));
        ASSERT_TRUE(Halted);
        RecordProperty(
            "event_update_watch_" + std::to_string(Kind) + "_" +
                std::to_string(Accessed) + "_" + std::to_string(Offset),
            llvm::formatv("pc={0:x};dr6={1:x};cs_base={2:x};pte={3:x}",
                          Native.Core.Core.Core.Core.PC,
                          CPU.DebugRegisters->dr6, Native.Core.CS.Base,
                          word(CPU.ExtraMemory.front().Bytes + 0x28))
                .str());
        Memory =
            llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
        auto Result = deliverX64PagedEvent(
            State, *Memory, Event, 0xffff, &Debug,
            X86InstructionDebugProfile::Watchpoints, {},
            options(CPU.ReturnCoreType == 0x40).EventDescriptorCanonicality);
        ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
        EXPECT_EQ(Result->Status, X64PagedEventStatus::Delivered);
        EXPECT_EQ(State, Native);
        EXPECT_EQ(Debug.DR6, CPU.DebugRegisters->dr6);
        EXPECT_EQ(Debug.DR7, CPU.DebugRegisters->dr7);
        compareAll(*Memory, CPU);
        ++Comparisons;
      }
  RecordProperty("original_code64_event_accessed_watchpoint_comparisons",
                 Comparisons);
}

TEST_F(X64PagedEventAccessedTest, ResumeRetainsCommittedImageAndWrites) {
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
        for (unsigned Pause = 0; Pause != 6; ++Pause) {
          SCOPED_TRACE(llvm::formatv("kind={0} pause={1} llvm={2} {3}", Kind,
                                     Pause, LLVM, Compiler)
                           .str());
          preparePause(Kind, Pause);
          const auto Before = State;
          Memory =
              llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
          X64PagedExecution Direct(*Memory);
          uint64_t Budget = 1;
          ASSERT_EQ(enter(Kind, Driver, Scheduled, Direct, Budget), 7u)
              << Direct.lastError();
          const auto Expected = State;
          const auto ExpectedRAM =
              Memory->physicalMemory().snapshotMemoryRegions();
          State = Before;
          Memory = pausedMemory(Pause);
          X64PagedExecution First(*Memory);
          Budget = 1;
          ASSERT_EQ(enter(Kind, Driver, Scheduled, First, Budget), 18u)
              << First.lastError();
          EXPECT_EQ(Budget, Kind < 2 ? 0u : 1u);
          ASSERT_TRUE(First.lastResult().Delivery);
          ASSERT_TRUE(First.lastResult().Delivery->Continuation);
          const auto Pending = *First.lastResult().Delivery->Continuation;
          const auto Updates =
              std::count_if(Pending.CompletedAccesses.begin(),
                            Pending.CompletedAccesses.end(),
                            [](const auto &A) { return A.DescriptorAccessed; });
          EXPECT_EQ(Updates, Pause == 2 || Pause == 3 || Pause == 5 ? 1 : 0);
          EXPECT_EQ(State.Core.CS, Before.Core.CS);
          EXPECT_EQ(State.NMIBlocked, 1u);
          if (Pause == 5)
            EXPECT_EQ(Memory->physicalMemory()
                          .loadScalar(0x17020, GuestScalarWidth::I64)
                          .Value,
                      0x1d030u);
          const auto Stopped = State;
          const auto StoppedRAM =
              Memory->physicalMemory().snapshotMemoryRegions();
          auto Again = First.resume(State, Pending, 0xffff);
          ASSERT_TRUE(bool(Again)) << llvm::toString(Again.takeError());
          EXPECT_EQ(Again->Exit, 18u);
          EXPECT_FALSE(Again->InstructionAttempted);
          EXPECT_EQ(State, Stopped);
          compareRegions(Memory->physicalMemory().snapshotMemoryRegions(),
                         StoppedRAM);
          if (Pause == 1) {
            // The first stop retained the checked descriptor. Now the write
            // plan succeeds, but its fresh physical read has no permission.
            auto Snapshot = RAM;
            Snapshot.Memory = StoppedRAM;
            ASSERT_EQ(Snapshot.Memory.size(), 3u);
            Snapshot.Memory[1].Permissions = MemoryPermission::Write;
            Memory = llvm::cantFail(
                X86PagedMemory::create(Snapshot, Features, Memory->controls()));
            X64PagedExecution ReadStopped(*Memory);
            auto Read = ReadStopped.resume(State, Pending, 0xffff);
            ASSERT_TRUE(bool(Read)) << llvm::toString(Read.takeError());
            EXPECT_EQ(Read->Exit, 18u);
            ASSERT_TRUE(Read->Delivery);
            ASSERT_TRUE(Read->Delivery->Continuation);
            EXPECT_EQ(Read->Delivery->Continuation->CompletedAccesses.size(),
                      Pending.CompletedAccesses.size());
            EXPECT_EQ(State, Stopped);
            compareRegions(Memory->physicalMemory().snapshotMemoryRegions(),
                           Snapshot.Memory);
          }
          auto Repaired = repairedMemory(*Memory);
          auto LowerMemory = repairedMemory(*Repaired);
          auto LowerState = State;
          auto Lower =
              resumeX64PagedEvent(LowerState, *LowerMemory, Pending, 0xffff);
          ASSERT_TRUE(bool(Lower)) << llvm::toString(Lower.takeError());
          EXPECT_EQ(Lower->Status, X64PagedEventStatus::Delivered);
          EXPECT_EQ(LowerState, Expected);
          compareRegions(LowerMemory->physicalMemory().snapshotMemoryRegions(),
                         ExpectedRAM);
          X64PagedExecution Resumed(*Repaired);
          auto Done = Resumed.resume(State, Pending, 0xffff);
          ASSERT_TRUE(bool(Done)) << llvm::toString(Done.takeError());
          EXPECT_EQ(Done->Exit, 7u);
          EXPECT_FALSE(Done->InstructionAttempted);
          ASSERT_TRUE(Done->Delivery);
          EXPECT_EQ(Done->Delivery->SoftwareCompleted, Kind < 2);
          EXPECT_EQ(Budget, Kind < 2 ? 0u : 1u);
          EXPECT_EQ(State, Expected);
          compareRegions(Repaired->physicalMemory().snapshotMemoryRegions(),
                         ExpectedRAM);
          ++Comparisons;
        }
      }
  RecordProperty("event_accessed_resume_comparisons", Comparisons);
}

TEST_F(X64PagedEventAccessedTest, InvalidUpdateJournalRefusesBeforeEffects) {
  for (unsigned Kind = 0; Kind != 4; ++Kind) {
    preparePause(Kind, 5);
    Memory = pausedMemory(5);
    auto First = deliverX64PagedEvent(State, *Memory, event(Kind), 0xffff);
    ASSERT_TRUE(bool(First)) << llvm::toString(First.takeError());
    ASSERT_EQ(First->Status, X64PagedEventStatus::BackingFault);
    ASSERT_TRUE(First->Continuation);
    const auto Pending = *First->Continuation;
    auto Repaired = repairedMemory(*Memory);
    const auto Stopped = State;
    const auto StoppedRAM = Repaired->physicalMemory().snapshotMemoryRegions();
    const auto Controls = Repaired->controls();
    const auto Update = std::find_if(
        Pending.CompletedAccesses.begin(), Pending.CompletedAccesses.end(),
        [](const auto &A) { return A.DescriptorAccessed; });
    ASSERT_NE(Update, Pending.CompletedAccesses.end());
    const auto Index = Update - Pending.CompletedAccesses.begin();
    ASSERT_GT(Pending.CompletedAccesses.size(), size_t(Index + 1));
    for (unsigned Bad = 0; Bad != 6; ++Bad) {
      auto Invalid = Pending;
      auto &Access = Invalid.CompletedAccesses[Index];
      if (Bad == 0)
        Access.Write = false;
      if (Bad == 1)
        Access.Bytes = 1;
      if (Bad == 2)
        Access.Value &= ~(uint64_t{1} << 40);
      if (Bad == 3)
        Access.DescriptorAccessed = false;
      if (Bad == 4) {
        auto &Store = Invalid.CompletedAccesses.back();
        ASSERT_TRUE(Store.Write);
        ASSERT_FALSE(Store.DescriptorAccessed);
        Store.DescriptorAccessed = true;
        Store.Value |= uint64_t{1} << 40;
      }
      if (Bad == 5)
        ++Access.Address;
      auto Rejected = resumeX64PagedEvent(State, *Repaired, Invalid, 0xffff);
      ASSERT_FALSE(bool(Rejected));
      llvm::consumeError(Rejected.takeError());
      EXPECT_EQ(State, Stopped);
      EXPECT_EQ(Repaired->controls(), Controls);
      compareRegions(Repaired->physicalMemory().snapshotMemoryRegions(),
                     StoppedRAM);
    }
  }
}

TEST_F(X64PagedEventAccessedTest, FullModeRetainsCommittedUpdate) {
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
        auto Export = exportX64Continuation(
            image(Code),
            llvm::formatv("{0}", llvm::json::Value(std::move(Settings))).str(),
            LLVM);
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
            "_paged_adapter(s,e); }\nuint64_t mode_validate(void *s, void *e) "
            "{ return " +
            Symbol->str() + "_validate_paged_adapter(s,e); }\n";
        llvm::sys::DynamicLibrary DL;
        const auto Name = std::string("event-accessed-mode-") +
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
        for (unsigned Pause = 0; Pause != 6; ++Pause) {
          SCOPED_TRACE(llvm::formatv("kind={0} pause={1} llvm={2} {3}", Kind,
                                     Pause, LLVM, Compiler)
                           .str());
          preparePause(Kind, Pause);
          const auto Before = State;
          Memory =
              llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
          auto Direct =
              deliverX64PagedEvent(State, *Memory, event(Kind), 0xffff);
          ASSERT_TRUE(bool(Direct)) << llvm::toString(Direct.takeError());
          ASSERT_EQ(Direct->Status, X64PagedEventStatus::Delivered);
          const auto Expected = State;
          const auto ExpectedRAM =
              Memory->physicalMemory().snapshotMemoryRegions();
          X86ModeState Machine;
          Machine.Core.Core = Before;
          X86ModeExecutionOptions Options;
          Options.MXCSRMask = 0xffff;
          Options.PagingAddressBits = 48;
          Options.Undefined.emplace();
          Options.Legacy.Segments.FullLimitChoice =
              X86FullSegmentLimitChoice::Wrap;
          Memory = pausedMemory(Pause);
          X86ModeExecution First(*Memory, Options);
          uint64_t Budget = 1;
          auto Stopped = Kind < 2 ? First.run(Machine, {Instruction}, Budget)
                                  : First.interrupt(Machine, event(Kind).Kind,
                                                    event(Kind).Vector);
          ASSERT_TRUE(bool(Stopped)) << llvm::toString(Stopped.takeError());
          EXPECT_TRUE(Stopped->OwnerError.empty()) << Stopped->OwnerError;
          EXPECT_EQ(Stopped->Status, X86LegacyExecutionStatus::BackingFault);
          EXPECT_EQ(Budget, Kind < 2 ? 0u : 1u);
          EXPECT_TRUE(Stopped->EventAccepted);
          EXPECT_EQ(Stopped->Attempts, Kind < 2 ? 1u : 0u);
          EXPECT_EQ(Stopped->Completions, 0u);
          ASSERT_TRUE(Machine.Pending);
          auto Repaired = repairedMemory(*Memory);
          X86ModeExecution Resumed(*Repaired, Options);
          auto Done = Resumed.run(Machine, {}, Budget);
          ASSERT_TRUE(bool(Done)) << llvm::toString(Done.takeError());
          EXPECT_TRUE(Done->OwnerError.empty()) << Done->OwnerError;
          EXPECT_EQ(Done->Status, X86LegacyExecutionStatus::EventDelivered);
          EXPECT_EQ(Done->Attempts, 0u);
          EXPECT_EQ(Done->Completions, Kind < 2 ? 1u : 0u);
          EXPECT_FALSE(Done->EventAccepted);
          EXPECT_EQ(Budget, Kind < 2 ? 0u : 1u);
          EXPECT_FALSE(Machine.Pending);
          EXPECT_EQ(Machine.Core.Core, Expected);
          compareRegions(Repaired->physicalMemory().snapshotMemoryRegions(),
                         ExpectedRAM);
          ++Comparisons;
        }
      }
  RecordProperty("event_accessed_full_mode_comparisons", Comparisons);
}

TEST_F(X64PagedEventAccessedTest, WriteWatchpointSurvivesAcceptedUpdatePause) {
  unsigned Comparisons = 0;
  for (unsigned Kind = 0; Kind != 4; ++Kind)
    for (unsigned Pause : {2, 5}) {
      SCOPED_TRACE(llvm::formatv("kind={0} pause={1}", Kind, Pause).str());
      preparePause(Kind, Pause);
      const auto Before = State;
      X64PagedDebugState Debug;
      Debug.DR[0] = Pause == 2 ? 0x5024 : 0xffff800000000020ULL;
      Debug.DR7 = 0x10401;
      auto ExpectedDebug = Debug;
      auto Expected = State;
      auto Reference =
          llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
      auto Direct = deliverX64PagedEvent(
          Expected, *Reference, event(Kind), 0xffff, &ExpectedDebug,
          X86InstructionDebugProfile::Watchpoints);
      ASSERT_TRUE(bool(Direct)) << llvm::toString(Direct.takeError());
      ASSERT_EQ(Direct->Status, X64PagedEventStatus::Delivered);
      EXPECT_EQ(Direct->DeliveredVector, 1u);
      EXPECT_EQ(Direct->DeliveryAttempts, 2u);
      EXPECT_EQ(ExpectedDebug.DR6, 0xffff0ff1u);
      Memory = pausedMemory(Pause);
      auto First =
          deliverX64PagedEvent(State, *Memory, event(Kind), 0xffff, &Debug,
                               X86InstructionDebugProfile::Watchpoints);
      ASSERT_TRUE(bool(First)) << llvm::toString(First.takeError());
      ASSERT_EQ(First->Status, X64PagedEventStatus::BackingFault);
      ASSERT_TRUE(First->Continuation);
      auto Pending = *First->Continuation;
      EXPECT_EQ(Pending.DataBreakpoints, 1);
      EXPECT_FALSE(Pending.SoftwareCompleted);
      EXPECT_EQ(State.Core.CS, Before.Core.CS);
      const auto Stopped = State;
      const auto StoppedDebug = Debug;
      const auto StoppedRAM = Memory->physicalMemory().snapshotMemoryRegions();
      auto Again = resumeX64PagedEvent(State, *Memory, Pending, 0xffff, &Debug,
                                       X86InstructionDebugProfile::Watchpoints);
      ASSERT_TRUE(bool(Again)) << llvm::toString(Again.takeError());
      ASSERT_EQ(Again->Status, X64PagedEventStatus::BackingFault);
      ASSERT_TRUE(Again->Continuation);
      EXPECT_EQ(Again->Continuation->DataBreakpoints, 1);
      EXPECT_EQ(State, Stopped);
      EXPECT_EQ(Debug, StoppedDebug);
      compareRegions(Memory->physicalMemory().snapshotMemoryRegions(),
                     StoppedRAM);
      auto Repaired = repairedMemory(*Memory);
      auto Done =
          resumeX64PagedEvent(State, *Repaired, *Again->Continuation, 0xffff,
                              &Debug, X86InstructionDebugProfile::Watchpoints);
      ASSERT_TRUE(bool(Done)) << llvm::toString(Done.takeError());
      EXPECT_EQ(Done->Status, X64PagedEventStatus::Delivered);
      EXPECT_EQ(Done->DeliveredVector, Direct->DeliveredVector);
      EXPECT_EQ(Done->DeliveryAttempts, Direct->DeliveryAttempts);
      EXPECT_FALSE(Done->Continuation);
      EXPECT_EQ(State, Expected);
      EXPECT_EQ(Debug, ExpectedDebug);
      compareRegions(Repaired->physicalMemory().snapshotMemoryRegions(),
                     Reference->physicalMemory().snapshotMemoryRegions());
      ++Comparisons;
    }
  RecordProperty("event_accessed_debug_resume_comparisons", Comparisons);
}
#endif
