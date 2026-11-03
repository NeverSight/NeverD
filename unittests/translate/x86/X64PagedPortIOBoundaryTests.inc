//===- X64PagedPortIOBoundaryTests.inc - Port restart boundaries ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Explicit device profiles, refusal, backing repair, IDT return and events.
//===----------------------------------------------------------------------===//

#if defined(__linux__) && defined(__x86_64__)
TEST_F(X64PagedExecutionTest,
       PagedPortTransfersPreserveRestartAndDeviceEffects) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "No test Clang";
  struct Form {
    bool Input;
    unsigned Width, Length;
    uint64_t PC;
  };
  std::vector<Form> Forms;
  std::vector<uint8_t> Code;
  std::vector<uint64_t> Addresses;
  for (bool Input : {true, false})
    for (unsigned Width : {1, 2, 4}) {
      const unsigned Start = Code.size();
      Addresses.push_back(0x1000 + Start);
      Code.insert(Code.end(), {0xf3, 0x67});
      if (Width == 2)
        Code.push_back(0x66);
      Code.push_back((Input ? 0x6c : 0x6e) | unsigned(Width != 1));
      Forms.push_back(
          {Input, Width, unsigned(Code.size() - Start), 0x1000 + Start});
      Code.resize(Start + 16, 0x90);
    }
  Code.resize(0x1012, 0x90);
  // MOV [R8], R9; ADD RSP, 8; IRETQ. DX remains the port number.
  const uint8_t Handler[] = {0x4d, 0x89, 0x08, 0x48, 0x83,
                             0xc4, 0x08, 0x48, 0xcf};
  std::copy(std::begin(Handler), std::end(Handler), Code.begin() + 0x1000);
  Code[0x1010] = 0x48;
  Code[0x1011] = 0xcf;
  Addresses.insert(Addresses.end(), {0x2000, 0x2003, 0x2007, 0x2010});
  std::vector<std::string> Compilers{NEVERD_TEST_CLANG};
  if (const char *Compiler = std::getenv("NEVERD_TEST_NEVERC"))
    Compilers.emplace_back(Compiler);
  unsigned Checks = 0;
  for (const auto &Compiler : Compilers)
    for (bool LLVM : {false, true}) {
      Run Driver = nullptr;
      Schedule Scheduled = nullptr;
      ASSERT_NO_FATAL_FAILURE(compile(Code, LLVM, "-O2", Driver, Addresses,
                                      Compiler, 0x1000, &Scheduled));
      for (const auto &F : Forms)
        for (unsigned Case = 0; Case != 15; ++Case) {
          SCOPED_TRACE(llvm::formatv("{0} llvm={1} pc={2:x} case={3}", Compiler,
                                     LLVM, F.PC, Case)
                           .str());
          reset(Code);
          auto &Core = State.Core.Core.Core.Core;
          Core.PC = F.PC;
          Core.RFlags = 0x8d7;
          Core.GPR[1] = 0x1234567800000004ULL;
          Core.GPR[2] = 0x2f9;
          Core.GPR[6] = Core.GPR[7] = 0x2345678900021000ULL - 2 * F.Width;
          Core.GPR[8] = 0x13108;
          Core.GPR[9] = 0x31007;
          State.DS = State.ES = State.Core.SS;
          State.FS = State.GS = {UINT32_MAX, 16, 0xc093};
          put(0x8024, 0x1c000);
          put(0x60e0, 0x2000 | (uint64_t(8) << 16) | (uint64_t(1) << 32) |
                          (uint64_t(0x8e) << 40));
          if (Case == 3)
            RAM.Memory.front().Bytes[F.PC] = 0x90;
          if (Case == 4 || Case == 5)
            put(Case == 4 ? 0x13100 : 0x13108, 0x50007);
          if (Case == 9 || Case == 10 || Case == 12 || Case == 13)
            put(Case == 9 ? 0x13100 : 0x13108, 0);
          put(0x6020, 0x2010 | (uint64_t(8) << 16) | (uint64_t(1) << 32) |
                          (uint64_t(0x8e) << 40));
          if (Case == 13)
            put(0x13030, 0x50007); // IDT backing fails after input and #PF.
          const auto Before = State;
          Memory =
              llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
          PagedPortTape Tape;
          Tape.Ready = Case != 6;
          Tape.Fail = Case == 14;
          if (Case == 7)
            Tape.StopAfter = 2;
          X64PagedExecutionOptions Options;
          Options.IretAccessOrder = X64PagedIretAccessOrder::FlagsFirst;
          Options.PortIO = X64PagedPortIOBinding{
              Case == 12 ? X64PagedPortIOProfile::InputAfterWriteChecks
                         : X64PagedPortIOProfile::InputBeforeWrite,
              PagedPortTape::transfer, &Tape};
          if (Case == 0)
            Options.PortIO.reset();
          if (Case == 1)
            Options.PortIO->Profile = static_cast<X64PagedPortIOProfile>(255);
          if (Case == 2)
            Options.PortIO->Transfer = nullptr;
          X64PagedExecution Context(*Memory, Options);
          uint64_t Budget = Case == 8 || Case == 11 ? 2 : 4;
          EXPECT_EQ(
              Driver(&State, reinterpret_cast<uint8_t *>(&Context), &Budget),
              Case < 3 || Case == 14    ? 5u
              : Case == 3               ? 20u
              : Case < 6 || Case == 13  ? 18u
              : Case < 8                ? 22u
              : Case == 8 || Case == 11 ? 9u
                                        : 7u);
          const unsigned Completed = Case == 5 || Case == 7 || Case == 8 ||
                                             Case == 10 || Case == 11 ||
                                             Case == 12 || Case == 13
                                         ? 2
                                         : 0;
          const bool Fault =
              Case == 9 || Case == 10 || Case == 12 || Case == 13;
          const unsigned ExtraRead = Fault && F.Input && Case != 12;
          EXPECT_EQ(Tape.Transfers.size(), Completed + ExtraRead);
          EXPECT_EQ(Budget,
                    Case == 8 || Case == 11 ? 0u : 4 - Completed - Fault);
          EXPECT_EQ(Context.lastResult().DeviceTransferCompleted,
                    Fault ? bool(ExtraRead) : Case == 8 || Case == 11);
          if (Case <= 4 || Case == 6 || Case == 14)
            EXPECT_EQ(std::memcmp(&State, &Before, sizeof(State)), 0);
          else if (!Fault) {
            EXPECT_EQ(Core.GPR[1], 2u);
            EXPECT_EQ(Core.PC, F.PC);
          }
          if (Case < 3 || Case == 14)
            EXPECT_FALSE(Context.lastError().empty());
          else
            EXPECT_TRUE(Context.lastError().empty()) << Context.lastError();
          ++Checks;
          ASSERT_TRUE(Memory->physicalMemory()
                          .storeScalar(F.PC, GuestScalarWidth::I8, 0xf3)
                          .Status == GuestMemoryAccessStatus::Completed);
          if (Case == 4 || Case == 5)
            ASSERT_TRUE(Memory->physicalMemory()
                            .storeScalar(Case == 4 ? 0x13100 : 0x13108,
                                         GuestScalarWidth::I64,
                                         Case == 4 ? 0x30007 : 0x31007)
                            .Status == GuestMemoryAccessStatus::Completed);
          // For the first-page fault, the same handler repairs its selected
          // PTE.
          if (Case == 9) {
            Core.GPR[8] = 0x13100;
            Core.GPR[9] = 0x30007;
          }
          Tape.Ready = true;
          Tape.Fail = false;
          Tape.StopAfter.reset();
          Options.PortIO =
              X64PagedPortIOBinding{X64PagedPortIOProfile::InputBeforeWrite,
                                    PagedPortTape::transfer, &Tape};
          X64PagedExecution Resumed(*Memory, Options);
          Shadows Shadow;
          Producer Events;
          Events.Ready = 0;
          if (Case == 13) {
            EXPECT_EQ(Core.PC, F.PC);
            ASSERT_TRUE(Memory->physicalMemory()
                            .storeScalar(0x13030, GuestScalarWidth::I64, 0x6007)
                            .Status == GuestMemoryAccessStatus::Completed);
            // Resume the retained exception delivery; replaying the faulting
            // instruction would incorrectly consume a second device input.
            const auto &Fault = *Context.lastResult().Exception;
            X64ContinuationEventV1 E{};
            E.SavedPC = F.PC;
            E.Kind = X64ContinuationEventKind::Fault;
            E.Vector = Fault.Vector;
            E.ErrorCode = Fault.ErrorCode;
            E.FaultAddress = Fault.CR2.value_or(0);
            auto Delivered = Resumed.deliver(State, E, 0xffff);
            ASSERT_TRUE(bool(Delivered))
                << llvm::toString(Delivered.takeError());
            EXPECT_EQ(Delivered->Exit, 7u);
            EXPECT_EQ(Tape.Transfers.size(), Completed + ExtraRead);
          }
          if (Fault) {
            EXPECT_EQ(Core.PC, 0x2000u);
            EXPECT_EQ(Context.lastResult().Exception->Vector, 14u);
            Budget = 3;
            uint64_t Polls = 3;
            EXPECT_EQ(Scheduled(&State, reinterpret_cast<uint8_t *>(&Resumed),
                                &Budget, &Shadow, &Polls, Producer::peek,
                                Producer::accepted, &Events),
                      6u);
            EXPECT_EQ(Budget, 0u);
            EXPECT_EQ(Core.PC, F.PC);
          }
          if (Case == 11) {
            // Poll the committed REP boundary, then execute the original IRETQ
            // handler before continuing. No state rewind or hidden cursor.
            Events.Ready = 1;
            Events.Candidate.Kind = 3;
            Events.Candidate.Vector = 2;
            Budget = 2;
            uint64_t Polls = 1;
            EXPECT_EQ(Scheduled(&State, reinterpret_cast<uint8_t *>(&Resumed),
                                &Budget, &Shadow, &Polls, Producer::peek,
                                Producer::accepted, &Events),
                      7u);
            EXPECT_EQ(Events.Acks, 1u);
            EXPECT_EQ(Budget, 2u);
            EXPECT_EQ(Core.PC, 0x2010u);
            EXPECT_EQ(Tape.Transfers.size(), 2u);
            EXPECT_EQ(Memory->physicalMemory()
                          .loadScalar(Core.GPR[4], GuestScalarWidth::I64)
                          .Value,
                      F.PC);
            Events.Ready = 0;
            Budget = Polls = 1;
            EXPECT_EQ(Scheduled(&State, reinterpret_cast<uint8_t *>(&Resumed),
                                &Budget, &Shadow, &Polls, Producer::peek,
                                Producer::accepted, &Events),
                      6u);
            EXPECT_EQ(Core.PC, F.PC);
            EXPECT_EQ(State.NMIBlocked, 0u);
          }
          Budget = 4 - Completed;
          EXPECT_EQ(
              Driver(&State, reinterpret_cast<uint8_t *>(&Resumed), &Budget),
              9u);
          EXPECT_EQ(Budget, 0u);
          EXPECT_TRUE(Resumed.lastError().empty()) << Resumed.lastError();
          EXPECT_EQ(Core.PC, F.PC + F.Length);
          EXPECT_EQ(Core.GPR[1], 0u);
          EXPECT_EQ(Core.GPR[F.Input ? 7 : 6], 0x21000u + 2 * F.Width);
          EXPECT_EQ(Core.GPR[F.Input ? 6 : 7],
                    Before.Core.Core.Core.Core.GPR[F.Input ? 6 : 7]);
          EXPECT_EQ(Core.RFlags, 0x8d7u);
          EXPECT_EQ(Tape.Transfers.size(), 4u + ExtraRead);
          EXPECT_TRUE(Resumed.lastResult().InstructionCompleted);
          if (F.Input) {
            for (unsigned I = 0; I != 4; ++I) {
              const uint64_t Linear = 0x21000 - 2 * F.Width + I * F.Width;
              std::array<uint8_t, 4> Data{};
              auto Read =
                  Memory->load({Linear, MemoryAccessKind::Read, 0},
                               llvm::MutableArrayRef(Data).take_front(F.Width));
              ASSERT_TRUE(bool(Read));
              ASSERT_EQ(Read->Status, X86PageWalkStatus::Completed);
              const auto &Entry =
                  Tape.Transfers[I + (I >= Completed ? ExtraRead : 0)];
              for (unsigned J = 0; J != F.Width; ++J)
                EXPECT_EQ(Data[J], uint8_t(Entry.Value >> (8 * J)));
            }
          }
          ++Checks;
          if (HasFailure())
            return;
        }
    }
  RecordProperty("port_restart_checks", Checks);
}
TEST_F(X64PagedExecutionTest, PagedPortPermissionWalksKeepFaultsDistinct) {
  unsigned Checks = 0;
  for (bool Input : {true, false})
    for (unsigned Case = 0; Case != 11; ++Case) {
      reset({uint8_t(Input ? 0xed : 0xef)});
      auto &Core = State.Core.Core.Core.Core;
      State.Core.CS = {0, UINT32_MAX, 27, 0xa0fb};
      State.Core.SS = {0, UINT32_MAX, 35, 0xc0f3};
      State.TR.Limit = 0x3000;
      Core.GPR[2] = 0;
      put(0x8066, 0x100);
      uint64_t FaultAddress = 0x8066;
      if (Case == 0 || Case == 1)
        put(0x13040, Case == 0 ? 0 : 0x50007);
      if (Case == 2 || Case == 3) {
        put(0x8066, 0xfff);
        put(0x13048, Case == 2 ? 0 : 0x50007);
        FaultAddress = 0x9000;
      }
      if (Case == 4 || Case == 5) {
        State.TR.Limit = 0x100 + (Case == 5);
      }
      if (Case == 6 || Case == 7) {
        Features.SMAP = true;
        Controls.CR4 |= 0x200000;
        Core.RFlags |= 0x40000;
        // Implicit supervisor reads cannot use AC to bypass SMAP.
        if (Case == 7)
          put(0x13040, 0x8003);
      }
      if (Case == 8)
        State.TR.Attributes = 0;
      if (Case == 9)
        State.Unusable |= 128;
      if (Case == 10) {
        Core.RFlags |= 0x3000;
        State.TR.Attributes = 0;
        put(0x13040, 0);
      }
      Controls.RFlags = Core.RFlags;
      const auto Before = State;
      Memory = llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
      PagedPortTape Tape;
      auto Result = executeX64PagedPortIO(
          State, *Memory, {RAM.Memory.front().Bytes.data() + 0x1000, 1},
          X64PagedAlignmentPriority::BeforePaging,
          {X64PagedPortIOProfile::InputBeforeWrite, PagedPortTape::transfer,
           &Tape});
      if (Case == 8 || Case == 9) {
        ASSERT_FALSE(bool(Result));
        llvm::consumeError(Result.takeError());
      } else {
        ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
        if (Case == 1 || Case == 3) {
          EXPECT_EQ(Result->Access.Status, X86PageWalkStatus::BackingFault);
          EXPECT_TRUE(Result->Access.BackingFault.has_value());
          EXPECT_FALSE(Result->Access.Exception.has_value());
        } else if (Case == 0 || Case == 2 || Case == 6) {
          EXPECT_EQ(Result->Access.Status, X86PageWalkStatus::Exception);
          ASSERT_TRUE(Result->Access.Exception.has_value());
          EXPECT_EQ(Result->Access.Exception->Vector, 14u);
          EXPECT_EQ(Result->Access.Exception->CR2, FaultAddress);
          EXPECT_EQ(Result->Access.Exception->ErrorCode, Case == 6 ? 1u : 0u);
        } else if (Case == 4) {
          ASSERT_TRUE(Result->Access.Exception.has_value());
          EXPECT_EQ(Result->Access.Exception->Vector, 13u);
        } else {
          EXPECT_TRUE(Result->Completed);
          EXPECT_TRUE(Result->DeviceTransferCompleted);
          EXPECT_EQ(Tape.Transfers.size(), 1u);
        }
      }
      if (Case < 5 || Case == 6 || Case == 8 || Case == 9) {
        EXPECT_TRUE(Tape.Transfers.empty());
        EXPECT_EQ(std::memcmp(&State, &Before, sizeof(State)), 0);
      }
      ++Checks;
      if (HasFailure())
        return;
    }
  RecordProperty("port_permission_checks", Checks);
}

TEST_F(X64PagedExecutionTest, PagedPortMalformedEncodingsRemainExplicit) {
  X64ContinuationSourceOptions Options;
  Options.Profile = X64ContinuationProfile::KernelEventsNoFaultV6;
  Options.MXCSRMask = 0xffff;
  Options.PagingAddressBits = 48;
  Options.PagedMemory = true;
  for (const auto &Bytes : std::vector<std::vector<uint8_t>>{{0xf0, 0x6c},
                                                             {0xf2, 0x6c},
                                                             {0xf3, 0xf3, 0x6e},
                                                             {0x48, 0xf3, 0x6c},
                                                             {0x66, 0x66, 0xed},
                                                             {0x67, 0x67, 0x6e},
                                                             {0x64, 0x65, 0x6f},
                                                             {0xf3, 0xed},
                                                             {0xf3, 0xe4, 0x80},
                                                             {0xf0, 0xee}}) {
    auto Units = buildX64ContinuationSource(image(Bytes), {0x1000}, Options);
    if (!Units) {
      llvm::consumeError(Units.takeError());
      continue;
    }
    ASSERT_EQ(Units->size(), 1u);
    EXPECT_TRUE(Units->front().RequiresNativeExecution);
  }
  for (const auto &Bytes : std::vector<std::vector<uint8_t>>{
           {0x6c}, {0x6f}, {0xf3, 0x67, 0x6d}, {0xed}, {0xe6, 0xf3}}) {
    Options.PagedMemory = true;
    auto Owned = buildX64ContinuationSource(image(Bytes), {0x1000}, Options);
    ASSERT_TRUE(bool(Owned)) << llvm::toString(Owned.takeError());
    EXPECT_FALSE(Owned->front().RequiresNativeExecution);
    Options.PagedMemory = false;
    auto Native = buildX64ContinuationSource(image(Bytes), {0x1000}, Options);
    ASSERT_TRUE(bool(Native)) << llvm::toString(Native.takeError());
    EXPECT_TRUE(Native->front().RequiresNativeExecution);
  }
}

TEST_F(X64PagedExecutionTest, PagedPortInputFragmentsRetainOnlyBoundEffects) {
  unsigned Checks = 0;
  for (auto Profile : {X64PagedPortIOProfile::InputBeforeWrite,
                       X64PagedPortIOProfile::InputAfterWriteChecks,
                       X64PagedPortIOProfile::InputBeforeWriteFragments})
    for (unsigned Width : {2, 4})
      for (unsigned Case = 0; Case != 5; ++Case) {
        const std::vector<uint8_t> Code =
            Width == 2 ? std::vector<uint8_t>{0xf3, 0x66, 0x6d}
                       : std::vector<uint8_t>{0xf3, 0x6d};
        reset(Code);
        auto &Core = State.Core.Core.Core.Core;
        Core.GPR[1] = 1;
        Core.GPR[2] = 0x2f9;
        Core.GPR[7] = 0x20fff;
        if (Case == 0 || Case == 2 || Case == 4)
          put(0x13108, 0);
        if (Case == 1)
          put(0x13108, 0x50007);
        const auto Before = State;
        PagedPortTape Tape;
        Tape.Ready = Case != 2;
        Tape.Fail = Case == 4;
        Memory =
            llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
        auto Result = executeX64PagedPortIO(
            State, *Memory, Code, X64PagedAlignmentPriority::BeforePaging,
            {Profile, PagedPortTape::transfer, &Tape});
        const bool After =
            Profile == X64PagedPortIOProfile::InputAfterWriteChecks;
        if (Case == 4 && !After) {
          ASSERT_FALSE(bool(Result));
          llvm::consumeError(Result.takeError());
        } else {
          ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
          EXPECT_EQ(Result->DeviceTransferCompleted,
                    Case == 3 || (Case == 0 && !After));
          EXPECT_EQ(Result->Completed, Case == 3);
          EXPECT_EQ(Result->DeviceUnavailable, Case == 2 && !After);
          const unsigned Written =
              Case == 3 ? Width
              : Case == 0 &&
                      Profile ==
                          X64PagedPortIOProfile::InputBeforeWriteFragments
                  ? 1
                  : 0;
          EXPECT_EQ(Result->InputBytesWritten, Written);
          EXPECT_EQ(Memory->physicalMemory()
                        .loadScalar(0x30fff, GuestScalarWidth::I8)
                        .Value,
                    Written ? 0x21u : 0u);
          if (Case == 1)
            EXPECT_EQ(Result->Access.Status, X86PageWalkStatus::BackingFault);
          if ((Case == 0 || Case == 2 || Case == 4) &&
              !Result->DeviceUnavailable)
            EXPECT_EQ(Result->Access.Exception->Vector, 14u);
        }
        if (Case != 3)
          EXPECT_EQ(std::memcmp(&State, &Before, sizeof(State)), 0);
        ++Checks;
        if (HasFailure())
          return;
      }
  RecordProperty("port_fragment_checks", Checks);
}
#endif
