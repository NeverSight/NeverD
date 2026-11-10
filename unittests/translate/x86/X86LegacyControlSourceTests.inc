//===- X86LegacyControlSourceTests.inc - Compiled MOV-CR continuations ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Generated C sees live paging changes, fault retries and committed traps.
//===----------------------------------------------------------------------===//

TEST_P(X86LegacySourceTest, ControlRegisterWidthsAndReadAfterWrite) {
  for (bool Code32 : {false, true})
    for (unsigned CR : {0u, 2u, 3u, 4u}) {
      controlState(false, Code32);
      S.Core.GPR[6] = CR == 0   ? 0x80050009
                      : CR == 2 ? 0xdeadbeef
                      : CR == 3 ? 0x10018
                                : 0x88;
      S.Core.GPR[0] = 0x12345678;
      auto Write = test::legacyControlBytes(CR, true, 6, 0, true);
      auto Read = test::legacyControlBytes(CR, false, 0, 1, true);
      const unsigned Bits = Code32 ? 32 : 16;
      Driver Run = nullptr;
      ASSERT_NO_FATAL_FAILURE(
          compile({{0x1000, Bits, Write},
                   {uint32_t(0x1000 + Write.size()), Bits, Read}},
                  Run));
      X86LegacySourceExecution Context(S, *M, O);
      uint64_t Budget = 2;
      ASSERT_EQ(Run(&Context, &Budget), 258u) << Context.lastError().str();
      EXPECT_EQ(Budget, 0u);
      EXPECT_EQ(S.Core.GPR[0], CR == 0 ? 0x80050019u : S.Core.GPR[6]);
      EXPECT_EQ(S.EIP, 0x1000 + Write.size() + Read.size());
      EXPECT_EQ(S.EFlags, 0xed7u);
    }
}

TEST_P(X86LegacySourceTest, ControlRegisterCR3SelectsNewInstructionBytes) {
  for (bool PAE : {false, true}) {
    controlState(PAE);
    Driver Run = nullptr;
    ASSERT_NO_FATAL_FAILURE(
        compile({{0x1000, 32, {0x0f, 0x22, 0xde}},
                 {0x1003, 32, {0xb8, 0x78, 0x56, 0x34, 0x12}}},
                Run));
    if (PAE) {
      put(0x13000, 0x14001);
      put(0x14000, 0x15007);
      put(0x15008, 0x9007);
    } else {
      put(0x13000, 0x14007, GuestScalarWidth::I32);
      put(0x14004, 0x9007, GuestScalarWidth::I32);
    }
    put(0x1003, 0xccccccccccULL);
    put(0x9003, 0x12345678b8ULL);
    S.Core.GPR[6] = 0x13000;
    X86LegacySourceExecution Context(S, *M, O);
    uint64_t Budget = 2;
    ASSERT_EQ(Run(&Context, &Budget), 258u) << Context.lastError().str();
    EXPECT_EQ(S.EIP, 0x1008u);
    EXPECT_EQ(S.Core.GPR[0], 0x12345678u);
    EXPECT_EQ(M->controls().CR3, 0x13000u);
  }
}

TEST_P(X86LegacySourceTest, ControlRegisterPDPTEFaultRepairAndRetry) {
  controlState(true);
  Driver Run = nullptr;
  ASSERT_NO_FATAL_FAILURE(
      compile({{0x1000, 32, {0x0f, 0x22, 0xde}},
               {0x1003, 32, {0x0f, 0x20, 0xd8}},
               {0x2000, 32, {0xc7, 5, 0x18, 0x30, 1, 0, 0, 0, 0, 0}},
               {0x200a, 32, {0x83, 0xc4, 4}},
               {0x200d, 32, {0xcf}}},
              Run));
  S.Core.GPR[6] = 0x13000;
  put(0x13000, 0x11001);
  put(0x13018, 3);
  X86LegacySourceExecution Context(S, *M, O);
  uint64_t Budget = 1;
  ASSERT_EQ(Run(&Context, &Budget), 1u) << Context.lastError().str();
  EXPECT_EQ(Context.lastResult().DeliveredVector, 13);
  EXPECT_FALSE(Context.lastResult().InstructionCompleted);
  EXPECT_EQ(M->controls().CR3, 0x10000u);
  EXPECT_EQ(S.CR2, 0x12345678u);
  Budget = 5;
  ASSERT_EQ(Run(&Context, &Budget), 258u) << Context.lastError().str();
  EXPECT_EQ(S.EIP, 0x1006u);
  EXPECT_EQ(S.Core.GPR[0], 0x13000u);
  EXPECT_EQ(M->controls().CR3, 0x13000u);
}

TEST_P(X86LegacySourceTest, ControlRegisterCommitPrecedesDebugEntry) {
  controlState();
  Driver Run = nullptr;
  ASSERT_NO_FATAL_FAILURE(compile({{0x1000, 32, {0x0f, 0x22, 0xde}}}, Run));
  // The new CR3 remaps the handler's IDT page. A stale bank would enter 2000.
  put(0x13000, 0x14007, GuestScalarWidth::I32);
  for (unsigned I = 0; I != 32; ++I)
    put(0x14000 + I * 4, I * 4096 | 7, GuestScalarWidth::I32);
  put(0x14018, 0x40007, GuestScalarWidth::I32);
  put(0x9008, 0x3000 | (uint64_t{8} << 16) | (uint64_t{0x8e} << 40));
  S.Core.GPR[6] = 0x13000;
  S.EFlags = 0x302;
  S.Debug.emplace();
  O.Debug = X86LegacyDebugProfile::InstructionStep;
  X86LegacySourceExecution Context(S, *M, O);
  uint64_t Budget = 1;
  ASSERT_EQ(Run(&Context, &Budget), 4u) << Context.lastError().str();
  EXPECT_TRUE(Context.lastResult().InstructionCompleted);
  ASSERT_TRUE(S.PendingEvent);
  EXPECT_EQ(S.EIP, 0x1003u);
  EXPECT_EQ(Budget, 0u);
  EXPECT_EQ(M->controls().CR3, 0x13000u);
  put(0x14018, 0x9007, GuestScalarWidth::I32);
  ASSERT_EQ(Context.resume(), 1u) << Context.lastError().str();
  EXPECT_EQ(Context.lastResult().DeliveredVector, 1);
  EXPECT_EQ(S.EIP, 0x3000u);
  EXPECT_EQ(get(S.Core.GPR[4]), 0x1003u);
  EXPECT_EQ(M->controls().CR3, 0x13000u);
}

TEST_P(X86LegacySourceTest, ControlRegisterBackingStopKeepsBudgetAndState) {
  controlState(true);
  Driver Run = nullptr;
  ASSERT_NO_FATAL_FAILURE(compile({{0x1000, 32, {0x0f, 0x22, 0xde}}}, Run));
  S.Core.GPR[6] = 0x20000;
  X86LegacySourceExecution Context(S, *M, O);
  uint64_t Budget = 1;
  ASSERT_EQ(Run(&Context, &Budget), 4u) << Context.lastError().str();
  EXPECT_EQ(Budget, 0u);
  EXPECT_EQ(S.EIP, 0x1000u);
  EXPECT_EQ(M->controls().CR3, 0x10000u);
  EXPECT_EQ(M->controls().PDPTE[0], 0x11001u);
  auto RAM = llvm::cantFail(createZeroedGuestState(GuestArchitecture::X86_64));
  RAM.Memory = M->physicalMemory().snapshotMemoryRegions();
  RAM.Memory.push_back({0x20000,
                        MemoryPermission::Read | MemoryPermission::Write, 0,
                        std::vector<uint8_t>(32)});
  M = llvm::cantFail(X86PagedMemory::create(RAM, M->features(), M->controls()));
  put(0x20000, 0x11001);
  X86LegacySourceExecution Restored(S, *M, O);
  ASSERT_EQ(Run(&Restored, &Budget), 258u) << Restored.lastError().str();
  EXPECT_EQ(Budget, 0u);
  EXPECT_EQ(S.EIP, 0x1003u);
  EXPECT_EQ(S.Core.GPR[6], 0x20000u);
  EXPECT_EQ(M->controls().CR3, 0x20000u);
  EXPECT_EQ(M->controls().PDPTE[0], 0x11001u);
  EXPECT_FALSE(Restored.lastResult().InstructionAttempted);
  EXPECT_TRUE(Restored.lastResult().InstructionCompleted);
}

TEST_P(X86LegacySourceTest, ControlRegisterBindingRefusesBeforeFetch) {
  Driver Run = nullptr;
  ASSERT_NO_FATAL_FAILURE(compile({{0x1000, 32, {0x0f, 0x20, 0xc0}}}, Run));
  X86LegacySourceExecution Context(S, *M, O);
  uint64_t Budget = 1;
  EXPECT_EQ(Run(&Context, &Budget), 256u);
  EXPECT_EQ(Budget, 1u);
  EXPECT_EQ(get(0x11004), 0x1007u);
  EXPECT_EQ(S.EIP, 0x1000u);
}

TEST_P(X86LegacySourceTest, ControlReloadResumesOriginalAttemptAndReadPrefix) {
  for (bool Code32 : {false, true})
    for (unsigned Control : {0u, 3u, 4u})
      for (unsigned Read = 0; Read != 4; ++Read) {
        SCOPED_TRACE(llvm::formatv("code={0}, CR{1}, read={2}",
                                   Code32 ? 32 : 16, Control, Read)
                         .str());
        controlState(Control != 4, Code32);
        auto C = M->controls();
        if (Control == 0)
          C.CR0 &= ~(1u << 31);
        if (Control != 3) {
          C.CR3 = 0x13020;
          C.PDPTE = {3, 3, 3,
                     3}; // Inactive cached bank must not reject reload.
        }
        put(0x13000, 0x11007, GuestScalarWidth::I32);
        put(0x14000, 0x15007);
        for (unsigned I = 0; I != 32; ++I)
          put(0x15000 + I * 8, I * 4096 | 7);
        for (unsigned I = 0; I != 4; ++I)
          put(0x13020 + I * 8, I ? 0 : 0x14001);
        llvm::cantFail(M->bindControls(C));
        S.Core.GPR[6] = Control == 0   ? 0x80010011
                        : Control == 3 ? 0x13020
                                       : 0x20;
        const auto Bytes = test::legacyControlBytes(Control, true);
        Driver Run = nullptr;
        ASSERT_NO_FATAL_FAILURE(compile({{0x1000, Code32 ? 32u : 16u, Bytes},
                                         {0x1003, Code32 ? 32u : 16u, {0x90}}},
                                        Run));
        auto RAM =
            llvm::cantFail(createZeroedGuestState(GuestArchitecture::X86_64));
        const auto Full = M->physicalMemory().snapshotMemoryRegions().front();
        auto Prefix = Full;
        Prefix.Bytes.resize(0x13020 + Read * 8);
        auto Suffix = Full;
        Suffix.Address = 0x13040;
        Suffix.Bytes.assign(Full.Bytes.begin() + Suffix.Address,
                            Full.Bytes.end());
        RAM.Memory = {Prefix, Suffix};
        M = llvm::cantFail(
            X86PagedMemory::create(RAM, M->features(), M->controls()));
        X86LegacySourceExecution Context(S, *M, O);
        uint64_t Budget = 1;
        ASSERT_EQ(Run(&Context, &Budget), 4u) << Context.lastError().str();
        EXPECT_EQ(Budget, 0u);
        EXPECT_EQ(neverd_x86_legacy_pending_v1(&Context), 1u);
        EXPECT_EQ(S.EIP, 0x1000u);
        EXPECT_EQ(M->controls().CR0, C.CR0);
        EXPECT_EQ(M->controls().CR3, C.CR3);
        EXPECT_EQ(M->controls().CR4, C.CR4);
        EXPECT_EQ(M->controls().PDPTE, C.PDPTE);
        ASSERT_TRUE(Context.lastResult().BackingFault);
        EXPECT_EQ(Context.lastResult().BackingFault->Exit.Address,
                  0x13020 + Read * 8);
        EXPECT_EQ(Run(&Context, &Budget), 4u);
        EXPECT_EQ(Budget, 0u);
        EXPECT_FALSE(Context.lastResult().InstructionAttempted);
        const auto Frozen = M->physicalMemory().snapshotMemoryRegions();
        const auto Reject = [&] {
          X86LegacySourceExecution Changed(S, *M, O);
          EXPECT_EQ(Run(&Changed, &Budget),
                    uint32_t(X86LegacySourceExit::InvalidState));
          EXPECT_EQ(Budget, 0u);
          EXPECT_EQ(neverd_x86_legacy_pending_v1(&Changed), 1u);
          EXPECT_EQ(M->physicalMemory().snapshotMemoryRegions().front().Bytes,
                    Frozen.front().Bytes);
        };
        ++S.Core.GPR[6];
        Reject();
        --S.Core.GPR[6];
        ++S.EIP;
        Reject();
        --S.EIP;
        S.Core.Segments[1].Attributes ^= 0x4000;
        Reject();
        S.Core.Segments[1].Attributes ^= 0x4000;
        S.EFlags ^= 0x10000;
        Reject();
        S.EFlags ^= 0x10000;
        const auto Features = M->features();
        auto ChangedFeatures = Features;
        ChangedFeatures.PhysicalAddressBits = 36;
        RAM.Memory = Frozen;
        M = llvm::cantFail(
            X86PagedMemory::create(RAM, ChangedFeatures, M->controls()));
        Reject();
        M = llvm::cantFail(
            X86PagedMemory::create(RAM, Features, M->controls()));
        const auto Binding = O.ControlRegisters;
        O.ControlRegisters.reset();
        Reject();
        O.ControlRegisters = Binding;
        auto Wrong = executeX86LegacyInstruction(S, *M, {0x90}, O);
        ASSERT_FALSE(bool(Wrong));
        llvm::consumeError(Wrong.takeError());
        S.PendingEvent.emplace();
        auto Conflict = executeX86LegacyInstruction(S, *M, Bytes, O);
        ASSERT_FALSE(bool(Conflict));
        llvm::consumeError(Conflict.takeError());
        EXPECT_TRUE(S.PendingInstructionAttempted);
        S.PendingEvent.reset();
        X86LegacySourceExecution RejectedEvent(S, *M, O);
        auto Event =
            RejectedEvent.deliver({S.EIP, 0, 0, 2, X86LegacyEventKind::NMI});
        EXPECT_EQ(Event, uint32_t(X86LegacySourceExit::InvalidState));
        EXPECT_EQ(neverd_x86_legacy_pending_v1(&RejectedEvent), 1u);
        RAM.Memory = Frozen;
        RAM.Memory.push_back(
            {0x13020 + Read * 8,
             MemoryPermission::Read | MemoryPermission::Write, 0,
             std::vector<uint8_t>(Full.Bytes.begin() + 0x13020 + Read * 8,
                                  Full.Bytes.begin() + 0x13040)});
        M = llvm::cantFail(
            X86PagedMemory::create(RAM, M->features(), M->controls()));
        put(0x1000, 0xcc, GuestScalarWidth::I8);
        // Fault injection after suspension proves no refetch and that physical
        // reads are retained. It is not a concurrent-CPU or device-memory
        // contract.
        for (unsigned I = 0; I != Read; ++I)
          put(0x13020 + I * 8, 3);
        X86LegacySourceExecution Restored(S, *M, O);
        ASSERT_EQ(Run(&Restored, &Budget), 258u) << Restored.lastError().str();
        EXPECT_EQ(Budget, 0u);
        EXPECT_EQ(S.EIP, 0x1003u); // The selected following NOP has not run.
        EXPECT_FALSE(Restored.lastResult().InstructionAttempted);
        EXPECT_TRUE(Restored.lastResult().InstructionCompleted);
        EXPECT_EQ(neverd_x86_legacy_pending_v1(&Restored), 0u);
        EXPECT_EQ(M->controls().CR0, 0x80010011u);
        EXPECT_EQ(M->controls().CR3, 0x13020u);
        EXPECT_EQ(M->controls().CR4, 0x20u);
        EXPECT_EQ(M->controls().PDPTE,
                  (std::array<uint64_t, 4>{0x14001, 0, 0, 0}));
      }
}

TEST_P(X86LegacySourceTest, ControlReloadAdmitsTargetDomainBeforePhysicalRead) {
  for (bool Debug : {false, true}) {
    controlState(true);
    auto C = M->controls();
    C.CR3 = 0x20000; // Cached PDPTEs still supply the valid old address space.
    C.CR4 = Debug ? 0x28 : 0x20;
    llvm::cantFail(M->bindControls(C));
    if (Debug) {
      S.Debug.emplace();
      S.Debug->DR7 = 0x20401;
      O.Debug = X86LegacyDebugProfile::Watchpoints;
      S.Core.GPR[6] = 0xa0; // PGE reloads; clearing DE would unbind I/O #DB.
    } else {
      O.ControlRegisters->OtherCR4Features |= 1u << 23;
      S.Core.GPR[6] = 0x8000a0; // Valid CET/WP request needs another RAM owner.
    }
    Driver Run = nullptr;
    ASSERT_NO_FATAL_FAILURE(compile({{0x1000, 32, {0x0f, 0x22, 0xe6}}}, Run));
    X86LegacySourceExecution Context(S, *M, O);
    uint64_t Budget = 1;
    EXPECT_EQ(Run(&Context, &Budget),
              uint32_t(X86LegacySourceExit::InvalidState));
    EXPECT_EQ(Budget, 1u);
    EXPECT_EQ(neverd_x86_legacy_pending_v1(&Context), 0u);
    EXPECT_EQ(M->controls().CR4, C.CR4);
    EXPECT_EQ(M->controls().PDPTE, C.PDPTE);
    S.Core.GPR[6] = Debug ? 0xa8 : 0xa0;
    ASSERT_EQ(Run(&Context, &Budget), 4u) << Context.lastError().str();
    EXPECT_EQ(Budget, 0u);
    EXPECT_EQ(neverd_x86_legacy_pending_v1(&Context), 1u);
    auto RAM =
        llvm::cantFail(createZeroedGuestState(GuestArchitecture::X86_64));
    RAM.Memory = M->physicalMemory().snapshotMemoryRegions();
    RAM.Memory.push_back({0x20000,
                          MemoryPermission::Read | MemoryPermission::Write, 0,
                          std::vector<uint8_t>(32)});
    M = llvm::cantFail(
        X86PagedMemory::create(RAM, M->features(), M->controls()));
    put(0x20000, 0x11001);
    X86LegacySourceExecution Restored(S, *M, O);
    ASSERT_EQ(Run(&Restored, &Budget), 258u) << Restored.lastError().str();
    EXPECT_EQ(Budget, 0u);
    EXPECT_EQ(S.EIP, 0x1003u);
    EXPECT_EQ(M->controls().CR4, S.Core.GPR[6]);
    EXPECT_EQ(M->controls().PDPTE, C.PDPTE);
    EXPECT_FALSE(Restored.lastResult().InstructionAttempted);
    EXPECT_TRUE(Restored.lastResult().InstructionCompleted);
  }
}

TEST_P(X86LegacySourceTest, ControlReloadRetiresIntoFaultOrDebugEvent) {
  for (bool Fault : {true, false}) {
    controlState(true);
    S.EFlags = 0x10302;
    S.STIBlocked = S.SSBlocked = true;
    S.Debug.emplace();
    S.Debug->DR = {0x1000, 0x20000, 0, 0};
    S.Debug->DR7 = 0x400 | 1 | (1u << 2) | (3u << 20);
    O.Debug = X86LegacyDebugProfile::Watchpoints;
    S.Core.GPR[6] = 0x20000;
    Driver Run = nullptr;
    ASSERT_NO_FATAL_FAILURE(compile({{0x1000, 32, {0x0f, 0x22, 0xde}},
                                     {0x1003, 32, {0x90}},
                                     {0x2000, 32, {0x90}},
                                     {0x3000, 32, {0x90}}},
                                    Run));
    put(0x14000, 0x15007);
    for (unsigned I = 0; I != 32; ++I)
      put(0x15000 + I * 8, I * 4096 | 7);
    // Distinct inaccessible old/new IDT pages distinguish event ownership.
    put(0x12030, 0x40007);
    put(0x15030, 0x50007);
    for (unsigned V = 0; V != 256; ++V)
      put(0x9000 + V * 8,
          0x3000 | (uint64_t{8} << 16) | (uint64_t{0xee} << 40));
    auto RAM =
        llvm::cantFail(createZeroedGuestState(GuestArchitecture::X86_64));
    RAM.Memory = M->physicalMemory().snapshotMemoryRegions();
    RAM.Memory.push_back({0x20000,
                          MemoryPermission::Read | MemoryPermission::Write, 0,
                          std::vector<uint8_t>(24)});
    M = llvm::cantFail(
        X86PagedMemory::create(RAM, M->features(), M->controls()));
    put(0x20000, 0x14001);
    X86LegacySourceExecution Context(S, *M, O);
    uint64_t Budget = 1;
    ASSERT_EQ(Run(&Context, &Budget), 4u) << Context.lastError().str();
    EXPECT_EQ(Budget, 0u);
    EXPECT_EQ(S.EFlags, 0x10302u);
    EXPECT_TRUE(S.STIBlocked);
    EXPECT_TRUE(S.SSBlocked);
    EXPECT_EQ(S.Debug->DR6, 0xffff0ff0u);
    EXPECT_EQ(Run(&Context, &Budget), 4u);
    EXPECT_FALSE(Context.lastResult().InstructionAttempted);
    EXPECT_EQ(S.EFlags, 0x10302u);
    EXPECT_EQ(S.Debug->DR6, 0xffff0ff0u);
    RAM.Memory = M->physicalMemory().snapshotMemoryRegions();
    RAM.Memory.push_back({0x20018,
                          MemoryPermission::Read | MemoryPermission::Write, 0,
                          std::vector<uint8_t>(8)});
    M = llvm::cantFail(
        X86PagedMemory::create(RAM, M->features(), M->controls()));
    put(0x20018, Fault ? 3 : 0);
    X86LegacySourceExecution Continued(S, *M, O);
    ASSERT_EQ(Run(&Continued, &Budget), 4u) << Continued.lastError().str();
    EXPECT_EQ(Budget, 0u);
    EXPECT_FALSE(Continued.lastResult().InstructionAttempted);
    EXPECT_EQ(Continued.lastResult().InstructionCompleted, !Fault);
    ASSERT_TRUE(S.PendingEvent);
    EXPECT_EQ(S.PendingEvent->Event.Vector, Fault ? 13 : 1);
    ASSERT_TRUE(Continued.lastResult().BackingFault);
    EXPECT_EQ(Continued.lastResult().BackingFault->Exit.Address,
              (Fault ? 0x40000u : 0x50000u) + (Fault ? 13u : 1u) * 8);
    EXPECT_EQ(M->controls().CR3, Fault ? 0x10000u : 0x20000u);
    EXPECT_EQ(M->controls().PDPTE[0], Fault ? 0x11001u : 0x14001u);
    EXPECT_EQ(S.EIP, Fault ? 0x1000u : 0x1003u);
    EXPECT_FALSE(S.STIBlocked);
    EXPECT_FALSE(S.SSBlocked);
    EXPECT_EQ(S.Debug->DR6 & 0x4003, Fault ? 0u : 0x4000u);
    EXPECT_EQ(neverd_x86_legacy_pending_v1(&Continued), 0u);
    EXPECT_EQ(Run(&Continued, &Budget), 258u);
    EXPECT_TRUE(S.PendingEvent);
    RAM.Memory = M->physicalMemory().snapshotMemoryRegions();
    const auto &Data = RAM.Memory.front().Bytes;
    const unsigned Source = Fault ? 0x6000 : 0x9000;
    RAM.Memory.push_back({Fault ? 0x40000u : 0x50000u,
                          MemoryPermission::Read | MemoryPermission::Write, 0,
                          std::vector<uint8_t>(Data.begin() + Source,
                                               Data.begin() + Source + 4096)});
    M = llvm::cantFail(
        X86PagedMemory::create(RAM, M->features(), M->controls()));
    X86LegacySourceExecution Event(S, *M, O);
    ASSERT_EQ(Event.resume(), 1u) << Event.lastError().str();
    EXPECT_EQ(Event.lastResult().DeliveredVector, Fault ? 13 : 1);
    EXPECT_EQ(S.EIP, Fault ? 0x2000u : 0x3000u);
    EXPECT_EQ(get(S.Core.GPR[4] + (Fault ? 4 : 0)), Fault ? 0x1000u : 0x1003u);
    EXPECT_EQ(Run(&Event, &Budget), 258u);
    EXPECT_EQ(S.EIP, Fault ? 0x2000u : 0x3000u);
    EXPECT_FALSE(S.PendingEvent);
  }
}
