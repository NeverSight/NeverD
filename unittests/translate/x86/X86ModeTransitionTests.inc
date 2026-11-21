//===- X86ModeTransitionTests.inc - Original mode and task transfers -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// {description} over the mixed-mode CPU bank.
//===----------------------------------------------------------------------===//

TEST_F(X86ModeInterruptTest, TaskGateAndNestedIretPublishBothTaskBanks) {
  for (auto M : {Mode::Protected16, Mode::Protected32}) {
    start(M);
    Machine.Halted = true;
    Machine.Debug.emplace();
    Options.Legacy.Debug = Options.Long.DebugProfile =
        X86InstructionDebugProfile::Watchpoints;
    Options.Legacy.Tasks.emplace();
    Options.Legacy.Tasks->WordGPRHigh.emplace();
    const auto TSS = [](uint32_t Base, unsigned Access) {
      return uint64_t{0x67} | (uint64_t(Base) << 16) | (uint64_t(Access) << 40);
    };
    put(0x8028, TSS(0xa000, 0x8b));
    put(0x8030, TSS(0xb000, 0x89));
    put(0x8038, TSS(0xc000, 0x82));
    put(0x9010, (uint64_t(48) << 16) | (uint64_t(0x85) << 40));
    put(0xb020, 0x3000);
    put(0xb024, 0x202);
    put(0xb028, 0x1234);
    put(0xb038, 0x1f000);
    const unsigned Selectors[] = {16, 8, 16, 16, 0, 0, 56};
    for (unsigned I = 0; I != std::size(Selectors); ++I)
      put(0xb048 + 4 * I, Selectors[I]);
    add(0x3000, Mode::Protected32, {0xcf});
    bindMemory();
    X86ModeExecution Execution(*Memory, Options);
    auto Enter =
        llvm::cantFail(Execution.interrupt(Machine, X86EventKind::NMI, 2));
    ASSERT_TRUE(Enter.OwnerError.empty()) << Enter.OwnerError;
    ASSERT_EQ(Enter.Status, X86LegacyExecutionStatus::EventDelivered);
    EXPECT_TRUE(Enter.EventAccepted);
    EXPECT_TRUE(Enter.TaskStateWritten);
    auto &V = Machine.Core.Core;
    auto &R = V.Core.Core.Core.Core;
    EXPECT_EQ(V.TR.Selector, 48);
    EXPECT_EQ(V.LDTR.Base, 0xc000u);
    EXPECT_EQ(V.Unusable, 0x30);
    EXPECT_EQ(R.PC, 0x3000u);
    EXPECT_EQ(uint32_t(R.GPR[0]), 0x1234u);
    EXPECT_TRUE(R.RFlags & 0x4000);
    uint64_t Budget = 1;
    const auto Back = llvm::cantFail(Execution.run(Machine, Program, Budget));
    ASSERT_TRUE(Back.OwnerError.empty()) << Back.OwnerError;
    EXPECT_EQ(Back.Completions, 1u);
    EXPECT_TRUE(Back.TaskStateWritten);
    EXPECT_EQ(R.PC, 0x1000u);
    EXPECT_EQ(V.TR.Selector, 40);
    EXPECT_EQ(V.NMIBlocked, 0);
    EXPECT_TRUE(V.Unusable & 64);
    EXPECT_FALSE(Machine.Pending);
  }
}

TEST_F(X86ModeInterruptTest,
       PartiallyLoadedIncomingTaskResumesTheOriginalTransfer) {
  for (const bool External : {false, true}) {
    start(Mode::Protected32);
    Machine.Debug.emplace();
    Options.Legacy.Debug = Options.Long.DebugProfile =
        X86InstructionDebugProfile::Watchpoints;
    Options.Legacy.Tasks.emplace();
    Options.Legacy.Tasks->WordGPRHigh.emplace();
    const auto TSS = [](unsigned Base, unsigned Access) {
      return uint64_t(0x67) | (uint64_t(Base) << 16) | (uint64_t(Access) << 40);
    };
    put(0x8028, TSS(0xa000, 0x8b));
    put(0x8030, TSS(0xb000, 0x89));
    put(0x8038, TSS(0xc000, 0x82));
    put(0x9010, (uint64_t(48) << 16) | (uint64_t(0x85) << 40));
    put(0xb020, 0x3000);
    put(0xb024, 0x202);
    put(0xb028, 0x1234);
    put(0xb038, 0x1f000);
    const unsigned Selectors[] = {16, 12, 16, 16, 0, 0, 56};
    for (unsigned I = 0; I != std::size(Selectors); ++I)
      put(0xb048 + 4 * I, Selectors[I]);
    add(0x1000, Mode::Protected32, {0x9a, 0, 0, 0, 0, 0x30, 0});
    const unsigned Hole = 0xc008;
    const auto Original = RAM.Memory.front().Bytes;
    RAM.Memory.front().Bytes.resize(Hole);
    RAM.Memory.push_back({Hole + 8,
                          MemoryPermission::Read | MemoryPermission::Write,
                          0,
                          {Original.begin() + Hole + 8, Original.end()}});
    bindMemory();
    X86ModeExecution Execution(*Memory, Options);
    uint64_t Budget = 1;
    auto Stop = llvm::cantFail(
        External ? Execution.interrupt(Machine, X86EventKind::NMI, 2)
                 : Execution.run(Machine, Program, Budget));
    ASSERT_EQ(Stop.Status, X86LegacyExecutionStatus::BackingFault)
        << Stop.OwnerError;
    ASSERT_TRUE(Machine.Pending);
    EXPECT_EQ(Stop.EventAccepted, External);
    EXPECT_EQ(Stop.Completions, 0u);
    EXPECT_EQ(Machine.Core.Core.TR.Selector, 48u);
    EXPECT_EQ(Machine.Core.Core.LDTR.Selector, 56u);
    EXPECT_EQ(Machine.Core.Core.Core.CS.Selector, 12u);
    EXPECT_TRUE(Machine.Core.Core.Unusable & 1);
    EXPECT_EQ(Machine.Core.Core.Core.Core.Core.Core.PC, 0x3000u);
    RAM.Memory = Memory->physicalMemory().snapshotMemoryRegions();
    RAM.Memory.push_back({Hole,
                          MemoryPermission::Read | MemoryPermission::Write,
                          0,
                          {0xff, 0xff, 0, 0, 0, 0x9b, 0xcf, 0}});
    Controls = Memory->controls();
    bindMemory();
    ASSERT_EQ(Memory->physicalMemory()
                  .storeScalar(0x1000, GuestScalarWidth::I8, 0xcc)
                  .Status,
              GuestMemoryAccessStatus::Completed);
    X86ModeExecution Resumed(*Memory, Options);
    Budget = 0;
    auto Done = llvm::cantFail(Resumed.run(Machine, {}, Budget));
    EXPECT_EQ(Done.Attempts, 0u);
    EXPECT_EQ(Done.Completions, unsigned(!External));
    EXPECT_FALSE(Done.EventAccepted);
    EXPECT_FALSE(Machine.Pending);
    EXPECT_FALSE(Machine.Core.Core.Unusable & 1);
    EXPECT_EQ(Machine.Core.Core.Core.CS.Selector, 12u);
    EXPECT_EQ(Machine.Core.Core.Core.Core.Core.Core.GPR[0] & UINT32_MAX,
              0x1234u);
  }
}

TEST_F(X86ModeInterruptTest, VM86InterruptAndReturnUseEveryFrameSelector) {
  start(Mode::Protected32);
  Options.Legacy.Virtual8086 = X86LegacyVM86Profile::IntelVME;
  Options.Legacy.Returns = X86LegacyReturnProfile::OrderedFrame;
  const uint32_t Slots[] = {0x100, 0x100, 0x23202, 0xabcd4000, 0x800,
                            0x300, 0x400, 0x500,   0x600};
  for (unsigned I = 0; I != std::size(Slots); ++I)
    put(0xf000 + 4 * I, Slots[I]);
  put(0xa004, 0x1f000);
  put(0xa008, 16);
  add(0x1000, Mode::Protected32, {0xcf});
  add(0x1100, Mode::Virtual8086, {0x40});
  add(0x4400, Mode::Protected32, {0xcf});
  bindMemory();
  X86ModeExecution Execution(*Memory, Options);
  uint64_t Budget = 2;
  auto Enter = llvm::cantFail(Execution.run(Machine, Program, Budget));
  ASSERT_TRUE(Enter.OwnerError.empty()) << Enter.OwnerError;
  ASSERT_EQ(Enter.Completions, 2u);
  auto &V = Machine.Core.Core;
  auto &R = V.Core.Core.Core.Core;
  ASSERT_EQ(llvm::cantFail(Execution.mode(Machine)), Mode::Virtual8086);
  EXPECT_EQ(R.PC, 0x101u);
  EXPECT_EQ(R.FSBase, 0x5000u);
  EXPECT_EQ(R.GSBase, 0x6000u);
  EXPECT_EQ(V.DS.Base, 0x4000u);
  EXPECT_EQ(V.ES.Base, 0x3000u);
  const auto Before = V;
  auto IRQ =
      llvm::cantFail(Execution.interrupt(Machine, X86EventKind::Interrupt, 64));
  ASSERT_TRUE(IRQ.OwnerError.empty()) << IRQ.OwnerError;
  ASSERT_EQ(IRQ.Status, X86LegacyExecutionStatus::EventDelivered);
  EXPECT_EQ(V.Unusable & 0x3c, 0x3c);
  EXPECT_EQ(V.FS.Selector, 0u);
  EXPECT_EQ(V.GS.Selector, 0u);
  EXPECT_EQ(V.DS.Selector, 0u);
  EXPECT_EQ(V.ES.Selector, 0u);
  const auto RAMBytes = bytes(*Memory);
  for (unsigned I = 0; I != std::size(Slots); ++I)
    EXPECT_EQ(word(RAMBytes, 0x1f000 - 36 + 4 * I, 4), I ? Slots[I] : 0x101u);
  Budget = 1;
  auto Back = llvm::cantFail(Execution.run(Machine, Program, Budget));
  ASSERT_EQ(Back.Completions, 1u);
  EXPECT_EQ(V, Before);
}

TEST_F(X86ModeInterruptTest,
       CompatibilityCallGatesInstallFullWidthTargetAndStack) {
  constexpr uint64_t High = 0xffff800000000000ULL;
  for (const bool Inner : {false, true})
    for (const bool Indirect : {false, true}) {
      start(Mode::Compatibility32);
      auto &V = Machine.Core.Core;
      auto &R = V.Core.Core.Core.Core;
      const unsigned CPL = Inner ? 3 : 0;
      V.Core.CS.Selector |= CPL;
      V.Core.CS.Attributes |= CPL << 5;
      V.Core.SS = V.ES = V.DS = {0x1000, UINT32_MAX, uint16_t(16 | CPL),
                                 uint16_t(0xc093 | (CPL << 5))};
      R.GPR[4] = 0x12340000f005ULL;
      put(0x8008, 0x00cf9b000000ffffULL | (uint64_t(CPL) << 45));
      put(0x8010, 0x00cf93001000ffffULL | (uint64_t(CPL) << 45));
      put(0x8030, 0x2000 | (uint64_t(24) << 16) | (uint64_t(0xec) << 40));
      put(0x8038, High >> 32);
      put(0xa004, High + 0x1f013);
      put(0x10000, 0x11007);
      put(0x10800, 0x11007);
      put(0x11000, 0x12007);
      put(0x12000, 0x87);
      if (Indirect) {
        put(0x3800, uint64_t(48) << 32);
        add(0x1000, Mode::Compatibility32,
            {0xff, 0x1d, 0x00, 0x28, 0x00, 0x00});
      } else
        add(0x1000, Mode::Compatibility32, {0x9a, 0, 0, 0, 0, 0x30, 0});
#if defined(__linux__) && defined(__x86_64__)
      const auto Initial = Machine;
#endif
      bindMemory();
      X86ModeExecution Execution(*Memory, Options);
      uint64_t Budget = 1;
      auto Got = llvm::cantFail(Execution.run(Machine, Program, Budget));
      ASSERT_TRUE(Got.OwnerError.empty()) << Got.OwnerError;
      ASSERT_EQ(Got.Completions, 1u);
      EXPECT_EQ(llvm::cantFail(Execution.mode(Machine)), Mode::Long64);
      EXPECT_EQ(R.PC, High + 0x2000);
      EXPECT_EQ(R.GPR[4], Inner ? High + 0x1eff3 : 0xeff5u);
      const unsigned SP = uint32_t(R.GPR[4]);
      const auto Image = bytes(*Memory);
      EXPECT_EQ(word(Image, SP, 8), Indirect ? 0x1006u : 0x1007u);
      EXPECT_EQ(word(Image, SP + 8, 8), 8u | CPL);
      if (Inner) {
        EXPECT_EQ(word(Image, SP + 16, 8), 0xf005u);
        EXPECT_EQ(word(Image, SP + 24, 8), 19u);
        EXPECT_TRUE(V.Unusable & 2);
      }
#if defined(__linux__) && defined(__x86_64__)
      X64PagedNativeCPU Native;
      ASSERT_NO_FATAL_FAILURE(Native.initialize());
      if (Native.CPU >= 0) {
        auto Final = Machine;
        Machine = Initial;
        ASSERT_NO_FATAL_FAILURE(initializeOriginal(Native));
        kvm_guest_debug D{};
        D.control = KVM_GUESTDBG_ENABLE | KVM_GUESTDBG_SINGLESTEP;
        ASSERT_EQ(ioctl(Native.CPU, KVM_SET_GUEST_DEBUG, &D), 0);
        ASSERT_EQ(ioctl(Native.CPU, KVM_RUN, 0), 0);
        ASSERT_EQ(Native.Run->exit_reason, unsigned(KVM_EXIT_DEBUG));
        kvm_regs Registers{};
        kvm_sregs Sys{};
        ASSERT_EQ(ioctl(Native.CPU, KVM_GET_REGS, &Registers), 0);
        ASSERT_EQ(ioctl(Native.CPU, KVM_GET_SREGS, &Sys), 0);
        Machine = std::move(Final);
        EXPECT_EQ(Registers.rip, R.PC);
        EXPECT_EQ(Registers.rsp, R.GPR[4]);
        EXPECT_EQ(Sys.cs.selector, V.Core.CS.Selector);
        EXPECT_EQ(Sys.ss.selector, V.Core.SS.Selector);
        EXPECT_TRUE(std::equal(Image.begin() + SP,
                               Image.begin() + SP + (Inner ? 32 : 16),
                               Native.RAM + SP));
      }
#endif
    }
}

TEST_F(X86ModeInterruptTest,
       CompatibilityReturnsSelectTheDestinationCodeWidth) {
  for (const bool Iret : {false, true})
    for (const bool LongTarget : {false, true})
      for (const unsigned Width : {2u, 4u}) {
        start(Mode::Compatibility32);
        Options.Legacy.Returns = X86LegacyReturnProfile::OrderedFrame;
        auto &R = Machine.Core.Core.Core.Core.Core.Core;
        R.GPR[4] = 0xf000;
        put(0xf000, 0x3210);
        put(0xf000 + Width, LongTarget ? 24 : 8);
        put(0xf000 + 2 * Width, 0x202);
        std::vector<uint8_t> Op;
        if (Width == 2)
          Op.push_back(0x66);
        Op.push_back(Iret ? 0xcf : 0xcb);
        add(0x1000, Mode::Compatibility32, Op);
        bindMemory();
        X86ModeExecution Execution(*Memory, Options);
        uint64_t Budget = 1;
        auto Got = llvm::cantFail(Execution.run(Machine, Program, Budget));
        ASSERT_TRUE(Got.OwnerError.empty()) << Got.OwnerError;
        ASSERT_EQ(Got.Completions, 1u);
        EXPECT_EQ(llvm::cantFail(Execution.mode(Machine)),
                  LongTarget ? Mode::Long64 : Mode::Compatibility32);
        EXPECT_EQ(R.PC, 0x3210u);
        EXPECT_EQ(R.GPR[4], 0xf000 + (Iret ? 3 : 2) * Width);
      }
}

TEST_F(X86ModeInterruptTest,
       DirectCompatibilityFarCallUsesFlatStackAndOperandWidthForLongCode) {
  constexpr uint32_t Top = 0x1f005, Base = 0x1000;
  for (const unsigned Width : {2u, 4u})
    for (const bool Stack32 : {false, true}) {
      SCOPED_TRACE(testing::Message()
                   << "width=" << Width << " SS.B=" << Stack32);
      start(Mode::Compatibility32);
      auto &V = Machine.Core.Core;
      auto &R = V.Core.Core.Core.Core;
      V.Core.SS.Base = Base;
      if (!Stack32)
        V.Core.SS.Attributes &= ~0x4000;
      R.GPR[4] = 0x1234000000000000ULL | Top;
      auto &Raw = RAM.Memory.front().Bytes;
      std::fill_n(Raw.begin() + Top - 8, 8, 0xa5);
      std::fill_n(Raw.begin() + Base + Top - 8, 8, 0xa5);
      const auto Next =
          add(0x1000, Mode::Compatibility32,
              Width == 2 ? std::vector<uint8_t>{0x66, 0x9a, 0, 0x20, 24, 0}
                         : std::vector<uint8_t>{0x9a, 0, 0x20, 0, 0, 24, 0});
#if defined(__linux__) && defined(__x86_64__)
      X64PagedNativeCPU Native;
      ASSERT_NO_FATAL_FAILURE(Native.initialize());
      if (Native.CPU >= 0) {
        ASSERT_NO_FATAL_FAILURE(initializeOriginal(Native));
        kvm_guest_debug Step{};
        Step.control = KVM_GUESTDBG_ENABLE | KVM_GUESTDBG_SINGLESTEP;
        ASSERT_EQ(ioctl(Native.CPU, KVM_SET_GUEST_DEBUG, &Step), 0);
        ASSERT_EQ(ioctl(Native.CPU, KVM_RUN, 0), 0);
        ASSERT_EQ(Native.Run->exit_reason, unsigned(KVM_EXIT_DEBUG));
      }
#endif
      bindMemory();
      X86ModeExecution Execution(*Memory, Options);
      uint64_t Budget = 1;
      auto Done = llvm::cantFail(Execution.run(Machine, Program, Budget));
      ASSERT_EQ(Done.Completions, 1u) << Done.OwnerError;
      ASSERT_EQ(llvm::cantFail(Execution.mode(Machine)), Mode::Long64);
      EXPECT_EQ(R.PC, 0x2000u);
      EXPECT_EQ(R.GPR[4], Top - 2 * Width);
      const auto Image = bytes(*Memory);
      EXPECT_EQ(word(Image, Top - 2 * Width, Width), Next);
      EXPECT_EQ(word(Image, Top - Width, Width), 8u);
      EXPECT_EQ(word(Image, Base + Top - 8, 8), 0xa5a5a5a5a5a5a5a5ULL);
#if defined(__linux__) && defined(__x86_64__)
      if (Native.CPU >= 0) {
        kvm_regs Regs{};
        ASSERT_EQ(ioctl(Native.CPU, KVM_GET_REGS, &Regs), 0);
        EXPECT_EQ(Regs.rsp, R.GPR[4]);
        EXPECT_EQ(Regs.rip, R.PC);
        EXPECT_TRUE(std::equal(Image.begin() + Top - 8, Image.begin() + Top,
                               Native.RAM + Top - 8));
      }
#endif
    }
}

TEST_F(X86ModeInterruptTest,
       ProtectedRealAndProtectedRoundTripWithRetainedCache) {
  start(Mode::Protected32);
  auto &R = Machine.Core.Core.Core.Core.Core.Core;
  R.GPR[0] = Controls.CR0 & ~uint64_t(1);
  add(0x1000, Mode::Protected32, {0x0f, 0x22, 0xc0});
  add(0x1003, Mode::Real32, {0xea, 0x20, 0, 0, 0, 0x01, 0x01});
  add(0x1030, Mode::Real32, {0x0f, 0x01, 0xf3}); // LMSW BX
  R.GPR[3] = Controls.CR0 & 15;
  add(0x1033, Mode::Protected32, {0xea, 0x40, 0x10, 0, 0, 8, 0});
  bindMemory();
  X86ModeExecution Execution(*Memory, Options);
#if defined(__linux__) && defined(__x86_64__)
  X64PagedNativeCPU Native;
  ASSERT_NO_FATAL_FAILURE(Native.initialize());
  if (Native.CPU >= 0) {
    ASSERT_NO_FATAL_FAILURE(initializeOriginal(Native));
    kvm_regs Registers{};
    ASSERT_EQ(ioctl(Native.CPU, KVM_GET_REGS, &Registers), 0);
    Registers.rax = R.GPR[0];
    ASSERT_EQ(ioctl(Native.CPU, KVM_SET_REGS, &Registers), 0);
    kvm_guest_debug D{};
    D.control = KVM_GUESTDBG_ENABLE | KVM_GUESTDBG_SINGLESTEP;
    ASSERT_EQ(ioctl(Native.CPU, KVM_SET_GUEST_DEBUG, &D), 0);
  }
#endif
  for (unsigned I = 0; I != 4; ++I) {
    uint64_t Budget = 1;
    auto Done = llvm::cantFail(Execution.run(Machine, Program, Budget));
    ASSERT_TRUE(Done.OwnerError.empty()) << Done.OwnerError;
    ASSERT_EQ(Done.Completions, 1u);
    EXPECT_EQ(Machine.RetainedRealCS, I == 2);
#if defined(__linux__) && defined(__x86_64__)
    if (Native.CPU >= 0) {
      ASSERT_EQ(ioctl(Native.CPU, KVM_RUN, 0), 0);
      ASSERT_EQ(Native.Run->exit_reason, unsigned(KVM_EXIT_DEBUG));
      kvm_regs Registers{};
      kvm_sregs Sys{};
      ASSERT_EQ(ioctl(Native.CPU, KVM_GET_REGS, &Registers), 0);
      ASSERT_EQ(ioctl(Native.CPU, KVM_GET_SREGS, &Sys), 0);
      EXPECT_EQ(Registers.rip, R.PC);
      EXPECT_EQ(Sys.cr0, Memory->controls().CR0);
      EXPECT_EQ(Sys.cs.selector, Machine.Core.Core.Core.CS.Selector);
      EXPECT_EQ(Sys.cs.base, Machine.Core.Core.Core.CS.Base);
      EXPECT_EQ(Sys.cs.db, bool(Machine.Core.Core.Core.CS.Attributes & 0x4000));
    }
#endif
  }
  EXPECT_EQ(R.PC, 0x1040u);
  EXPECT_EQ(Machine.Core.Core.Core.CS.Selector, 8u);
  EXPECT_TRUE(Memory->controls().CR0 & 1);
}
