//===- X86ModeSmmTests.inc - Software SMI and firmware return -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Firmware-edited SMM save images, RSM faults and original KVM state.
//===----------------------------------------------------------------------===//

TEST_F(X86ModeInterruptTest, SmiUsesPrivateRAMAndRsmHonorsFirmwareEdits) {
  for (bool Wide : {true, false}) {
    SCOPED_TRACE(Wide);
    const auto OutsideMode = Wide ? Mode::Long64 : Mode::Protected32;
    start(OutsideMode);
    Features.LongMode = Wide;
    if (!Wide)
      Controls.EFER = 0x800;
    Machine.Debug.emplace();
    Options.Legacy.Debug = Options.Long.DebugProfile =
        X86InstructionDebugProfile::Watchpoints;
    Machine.SMM.emplace();
    Machine.STIBlocked = true;
    Machine.Core.Core.NMIBlocked = 1;
    auto &R = Machine.Core.Core.Core.Core.Core.Core;
    R.GPR[0] = 0x9876543212345678ULL;
    R.ZMM[1][8] = 0xaa;
    const auto Before = Machine;
    auto SmmRAM = RAM;
    std::vector<uint8_t> Firmware = {0x67, 0x66, 0xc7, 0x05, 0xf8, 0xff,
                                     3,    0,    0xef, 0xbe, 0xad, 0xde};
    if (!Wide)
      Firmware[4] = 0xd0;
    std::copy(Firmware.begin(), Firmware.end(),
              SmmRAM.Memory.front().Bytes.begin() + 0x38000);
    SmmRAM.Memory.front().Bytes[0x3800c] = 0x0f;
    SmmRAM.Memory.front().Bytes[0x3800d] = 0xaa;
    Program = {{0x38000, Mode::Real16, Firmware},
               {0x3800c, Mode::Real16, {0x0f, 0xaa}}};
    bindMemory();
    auto System =
        llvm::cantFail(X86PagedMemory::create(SmmRAM, Features, Controls));
    X86ModeExecution Execution(*Memory, Options, *System);
    auto Enter = llvm::cantFail(Execution.systemManagementInterrupt(Machine));
    ASSERT_TRUE(Enter.EventAccepted);
    EXPECT_TRUE(Machine.SMM->Active);
    EXPECT_FALSE(Machine.STIBlocked);
    EXPECT_EQ(R.PC, 0x8000u);
    EXPECT_EQ(System->controls().CR0, Controls.CR0 & ~uint64_t(0x8000000d));
    EXPECT_EQ(llvm::cantFail(Execution.mode(Machine)), Mode::Real16);
    const auto Saved = bytes(*System);
    EXPECT_EQ(word(Saved, 0x3fefc, 4), Wide ? 0x20064u : 0x20000u);
    EXPECT_EQ(word(Saved, Wide ? 0x3fff8 : 0x3ffd0, Wide ? 8 : 4),
              Wide ? 0x9876543212345678ULL : 0x12345678u);
    EXPECT_EQ(word(Saved, Wide ? 0x3ff78 : 0x3fff0, Wide ? 8 : 4), 0x1000u);
    EXPECT_EQ(word(Saved, Wide ? 0x3fecb : 0x3ff1a, 1), 2u);
    EXPECT_EQ(
        llvm::cantFail(Execution.systemManagementInterrupt(Machine)).Status,
        X86LegacyExecutionStatus::EventDeferred);
    if (!Wide) {
      // The legacy format has no EFER slot. A firmware EFER write survives RSM.
      auto FirmwareControls = System->controls();
      ASSERT_EQ(FirmwareControls.EFER, 0x800u);
      FirmwareControls.EFER = 0;
      llvm::cantFail(System->bindControls(FirmwareControls));
    }
    uint64_t Budget = 2;
    auto Done = llvm::cantFail(Execution.run(Machine, Program, Budget));
    ASSERT_TRUE(Done.OwnerError.empty()) << Done.OwnerError;
    ASSERT_EQ(Done.Completions, 2u);
    EXPECT_FALSE(Machine.SMM->Active);
    EXPECT_EQ(llvm::cantFail(Execution.mode(Machine)), OutsideMode);
    EXPECT_EQ(Machine.STIBlocked, Before.STIBlocked);
    EXPECT_EQ(Machine.Core.Core.NMIBlocked, 1);
    EXPECT_EQ(R.GPR[0], Wide ? 0x98765432deadbeefULL : 0xdeadbeefu);
    EXPECT_EQ(R.PC, 0x1000u);
    EXPECT_EQ(R.ZMM[1][8], 0xaa);
    EXPECT_EQ(Machine.Debug, Before.Debug);
    EXPECT_EQ(Memory->controls().EFER, Wide ? Controls.EFER : 0u);
    EXPECT_EQ(bytes(*Memory), RAM.Memory.front().Bytes);
  }
}

TEST_F(
    X86ModeInterruptTest,
    RsmRestoresNormalizedDebugStateAndRejectsInvalidControlsBeforePaeReload) {
  for (auto Case : {0, 1, 2, 3}) {
    SCOPED_TRACE(Case);
    start(Mode::Long64);
    Machine.SMM.emplace();
    Machine.Debug.emplace();
    Options.Legacy.Debug = Options.Long.DebugProfile =
        X86InstructionDebugProfile::Watchpoints;
    auto SystemImage = RAM;
    SystemImage.Memory.front().Bytes[0x38000] = 0x0f;
    SystemImage.Memory.front().Bytes[0x38001] = 0xaa;
    bindMemory();
    auto System =
        llvm::cantFail(X86PagedMemory::create(SystemImage, Features, Controls));
    X86ModeExecution Execution(*Memory, Options, *System);
    Machine.SSBlocked = true;
    ASSERT_TRUE(llvm::cantFail(Execution.systemManagementInterrupt(Machine))
                    .EventAccepted);
    ASSERT_EQ(word(bytes(*System), 0x3fecb, 1), 1u);
    const auto Edit = [&](uint64_t At, uint64_t Value) {
      ASSERT_EQ(System->physicalMemory()
                    .storeScalar(At, GuestScalarWidth::I64, Value)
                    .Status,
                GuestMemoryAccessStatus::Completed);
    };
    ASSERT_NO_FATAL_FAILURE(Edit(0x3ff68, Case == 3 ? uint64_t(1) << 32 : 0));
    ASSERT_NO_FATAL_FAILURE(Edit(0x3ff60, 0));
    if (Case == 1 || Case == 2) {
      ASSERT_NO_FATAL_FAILURE(Edit(0x3fed0, 0));
      ASSERT_NO_FATAL_FAILURE(
          Edit(0x3ff48, 0x20 | (Case == 1 ? uint64_t(1) << 40 : 0)));
      ASSERT_NO_FATAL_FAILURE(Edit(0x3ff50, 0x40000));
      ASSERT_NO_FATAL_FAILURE(
          Edit(0x3ff58, Case == 2 ? 0x80000010 : 0x80000011));
    }
    Program = {{0x38000, Mode::Real16, {0x0f, 0xaa}}};
    uint64_t Budget = 1;
    auto R = llvm::cantFail(Execution.run(Machine, Program, Budget));
    EXPECT_EQ(R.Attempts, 1u);
    EXPECT_EQ(R.Completions, unsigned(!Case));
    EXPECT_EQ(Machine.Shutdown, bool(Case));
    EXPECT_FALSE(R.BackingFault);
    if (!Case) {
      EXPECT_TRUE(Machine.SSBlocked);
      EXPECT_EQ(Machine.Debug->DR6, 0xffff0ff0u);
      EXPECT_EQ(Machine.Debug->DR7, 0x400u);
    } else
      EXPECT_EQ(R.Status, X86LegacyExecutionStatus::Shutdown);
  }
}

TEST_F(X86ModeInterruptTest, InactiveRsmDeliversOriginalUndefinedOpcode) {
  for (auto M : {Mode::Protected32, Mode::Long64}) {
    start(M);
    Machine.SMM.emplace();
    if (M == Mode::Protected32)
      put(0x9030, 0x4060 | (uint64_t(8) << 16) | (uint64_t(0x8e) << 40));
    add(0x1000, M, {0x0f, 0xaa});
    bindMemory();
    X86ModeExecution Execution(*Memory, Options);
    uint64_t Budget = 1;
    auto R = llvm::cantFail(Execution.run(Machine, Program, Budget));
    EXPECT_EQ(R.Attempts, 1u);
    EXPECT_EQ(R.Completions, 0u);
    ASSERT_EQ(R.Status, X86LegacyExecutionStatus::EventDelivered)
        << R.OwnerError;
    EXPECT_EQ(R.DeliveredVector, 6u);
    EXPECT_EQ(Machine.Core.Core.Core.Core.Core.Core.PC, 0x4060u);
  }
}

#if defined(__linux__) && defined(__x86_64__)
TEST_F(X86ModeInterruptTest, SoftwareSmmSaveMapAndEditedRsmMatchKvm) {
  unsigned Maps = 0;
  for (bool Wide : {true, false}) {
    SCOPED_TRACE(Wide);
    X64PagedNativeCPU Native;
    ASSERT_NO_FATAL_FAILURE(Native.initialize());
    if (Native.CPU < 0 ||
        ioctl(Native.KVM, KVM_CHECK_EXTENSION, KVM_CAP_X86_SMM) <= 0)
      GTEST_SKIP() << "KVM SMM unavailable";
    start(Wide ? Mode::Long64 : Mode::Protected32);
    Features.LongMode = Wide;
    if (!Wide) {
      auto *CPUID = reinterpret_cast<kvm_cpuid2 *>(Native.CPUIDStorage.data());
      for (unsigned I = 0; I != CPUID->nent; ++I)
        if (CPUID->entries[I].function == 0x80000001)
          CPUID->entries[I].edx &= ~(1u << 29);
      ASSERT_EQ(ioctl(Native.CPU, KVM_SET_CPUID2, CPUID), 0);
      Controls.EFER = 0x800;
    }
    Machine.Debug.emplace();
    Options.Legacy.Debug = Options.Long.DebugProfile =
        X86InstructionDebugProfile::Watchpoints;
    Machine.SMM.emplace();
    auto &R = Machine.Core.Core.Core.Core.Core.Core;
    const auto SP = R.GPR[4], BX = R.GPR[3];
    std::fill(std::begin(R.GPR), std::end(R.GPR), 0);
    R.GPR[4] = SP;
    R.GPR[3] = BX;
    ASSERT_NO_FATAL_FAILURE(initializeOriginal(Native));
    auto &V = Machine.Core.Core;
    V.LDTR = {0x7900, 0x7f, 0x38, 0x82};
    V.Unusable &= ~64u;
    kvm_sregs Original{};
    ASSERT_EQ(ioctl(Native.CPU, KVM_GET_SREGS, &Original), 0);
    Original.ldt = X64PagedNativeCPU::segment(V.LDTR);
    ASSERT_EQ(ioctl(Native.CPU, KVM_SET_SREGS, &Original), 0);
    kvm_regs Initial{};
    ASSERT_EQ(ioctl(Native.CPU, KVM_GET_REGS, &Initial), 0);
    R.GPR[9] = Initial.r9 = UINT64_C(0x85961234abef5432);
    ASSERT_EQ(ioctl(Native.CPU, KVM_SET_REGS, &Initial), 0);
    auto SystemImage = RAM;
    SystemImage.Memory.front().Bytes[0x38000] = 0x0f;
    SystemImage.Memory.front().Bytes[0x38001] = 0xaa;
    auto *Private = static_cast<uint8_t *>(
        mmap(nullptr, Native.Size, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    ASSERT_NE(Private, MAP_FAILED);
    Native.ExtraMemory.push_back({0, Private, Native.Size});
    std::copy(SystemImage.Memory.front().Bytes.begin(),
              SystemImage.Memory.front().Bytes.end(), Private);
    kvm_userspace_memory_region Region{};
    Region.slot = 1u << 16;
    Region.memory_size = Native.Size;
    Region.userspace_addr = reinterpret_cast<uintptr_t>(Private);
    ASSERT_EQ(ioctl(Native.VM, KVM_SET_USER_MEMORY_REGION, &Region), 0)
        << errno;
    const auto Break = [&](uint64_t Address) {
      kvm_guest_debug D{};
      D.control = KVM_GUESTDBG_ENABLE | KVM_GUESTDBG_USE_HW_BP;
      D.arch.debugreg[0] = Address;
      D.arch.debugreg[7] = 0x401;
      ASSERT_EQ(ioctl(Native.CPU, KVM_SET_GUEST_DEBUG, &D), 0);
    };
    ASSERT_NO_FATAL_FAILURE(Break(0x38000));
    ASSERT_EQ(ioctl(Native.CPU, KVM_SMI, 0), 0);
    ASSERT_EQ(ioctl(Native.CPU, KVM_RUN, 0), 0) << errno;
    ASSERT_EQ(Native.Run->exit_reason, unsigned(KVM_EXIT_DEBUG));
    bindMemory();
    auto System =
        llvm::cantFail(X86PagedMemory::create(SystemImage, Features, Controls));
    X86ModeExecution Execution(*Memory, Options, *System);
    ASSERT_TRUE(llvm::cantFail(Execution.systemManagementInterrupt(Machine))
                    .EventAccepted);
    auto Saved = bytes(*System);
    for (unsigned At = 0x3fe00; At != 0x40000; ++At)
      EXPECT_EQ(Saved[At], Private[At]) << "SMRAM byte " << At;
    constexpr uint64_t Edited = UINT64_C(0x123456789abcdef0);
    const unsigned AX = Wide ? 0x3fff8 : 0x3ffd0;
    for (unsigned N = 0; N != (Wide ? 8u : 4u); ++N)
      Private[AX + N] = Edited >> (8 * N);
    ASSERT_EQ(System->physicalMemory()
                  .storeScalar(
                      AX, Wide ? GuestScalarWidth::I64 : GuestScalarWidth::I32,
                      Edited)
                  .Status,
              GuestMemoryAccessStatus::Completed);
    kvm_regs FirmwareRegs{};
    ASSERT_EQ(ioctl(Native.CPU, KVM_GET_REGS, &FirmwareRegs), 0);
    FirmwareRegs.r9 = R.GPR[9] = 0x5678;
    ASSERT_EQ(ioctl(Native.CPU, KVM_SET_REGS, &FirmwareRegs), 0);
    kvm_sregs FirmwareControls{};
    ASSERT_EQ(ioctl(Native.CPU, KVM_GET_SREGS, &FirmwareControls), 0);
    EXPECT_EQ(System->controls().EFER, FirmwareControls.efer);
    if (!Wide) {
      FirmwareControls.efer = 0;
      ASSERT_EQ(ioctl(Native.CPU, KVM_SET_SREGS, &FirmwareControls), 0);
      auto ControlsInSMM = System->controls();
      ControlsInSMM.EFER = 0;
      llvm::cantFail(System->bindControls(ControlsInSMM));
    }
    ASSERT_NO_FATAL_FAILURE(Break(0x1000));
    ASSERT_EQ(ioctl(Native.CPU, KVM_RUN, 0), 0) << errno;
    ASSERT_EQ(Native.Run->exit_reason, unsigned(KVM_EXIT_DEBUG));
    kvm_regs After{};
    kvm_vcpu_events Events{};
    ASSERT_EQ(ioctl(Native.CPU, KVM_GET_REGS, &After), 0);
    ASSERT_EQ(ioctl(Native.CPU, KVM_GET_VCPU_EVENTS, &Events), 0);
    Program = {{0x38000, Mode::Real16, {0x0f, 0xaa}}};
    uint64_t Budget = 1;
    ASSERT_EQ(
        llvm::cantFail(Execution.run(Machine, Program, Budget)).Completions,
        1u);
    EXPECT_EQ(Machine.SMM->Active, bool(Events.smi.smm));
    EXPECT_EQ(R.PC, After.rip);
    EXPECT_EQ(R.GPR[0], After.rax);
    EXPECT_EQ(R.GPR[3], After.rbx);
    EXPECT_EQ(R.GPR[4], After.rsp);
    EXPECT_EQ(R.GPR[9], After.r9);
    ASSERT_EQ(ioctl(Native.CPU, KVM_GET_SREGS, &Original), 0);
    EXPECT_EQ(Memory->controls().EFER, Original.efer);
    EXPECT_EQ(R.RFlags & ~uint64_t(0x10000), After.rflags & ~uint64_t(0x10000));
    ++Maps;
  }
  RecordProperty("kvm_save_maps", Maps);
}
#endif

TEST_F(X86ModeInterruptTest,
       RsmDebugUsesFirmwareBeforeCommitAndRestoredTablesAfterCommit) {
  for (auto OutsideMode : {Mode::Protected32, Mode::Long64})
    for (bool FirmwareTF : {false, true})
      for (bool OutsideTF : {false, true})
        for (bool FirmwareBreakpoint : {false, true}) {
          SCOPED_TRACE(::testing::Message()
                       << unsigned(OutsideMode) << " fwTF=" << FirmwareTF
                       << " restoredTF=" << OutsideTF
                       << " breakpoint=" << FirmwareBreakpoint);
          start(OutsideMode);
          auto &V = Machine.Core.Core;
          auto &R = V.Core.Core.Core.Core;
          Machine.SMM.emplace();
          Machine.Debug.emplace();
          Options.Legacy.Debug = Options.Long.DebugProfile =
              X86InstructionDebugProfile::Watchpoints;
          R.GPR[4] = 0xf000;
          if (OutsideTF)
            R.RFlags |= 0x100;
          if (OutsideMode == Mode::Protected32)
            put(0x9008, 0x4200 | (uint64_t(8) << 16) | (uint64_t(0x8e) << 40));
          auto Firmware = RAM;
          Firmware.Memory.front().Bytes[4] = 0;
          Firmware.Memory.front().Bytes[5] = 0xa0;
          Firmware.Memory.front().Bytes[6] = Firmware.Memory.front().Bytes[7] =
              0;
          Firmware.Memory.front().Bytes[0x38000] = 0x0f;
          Firmware.Memory.front().Bytes[0x38001] = 0xaa;
          Program = {{0x38000, Mode::Real16, {0x0f, 0xaa}}};
          bindMemory();
          auto Private = llvm::cantFail(
              X86PagedMemory::create(Firmware, Features, Controls));
          X86ModeExecution Execution(*Memory, Options, *Private);
          ASSERT_TRUE(
              llvm::cantFail(Execution.systemManagementInterrupt(Machine))
                  .EventAccepted);
          V.IDTR = {0, 0x3ff};
          R.GPR[4] = 0xf000;
          R.RFlags = FirmwareTF ? 0x102 : 2;
          if (FirmwareBreakpoint) {
            Machine.Debug->DR[0] = 0x38000;
            Machine.Debug->DR7 = 0x401;
          }
          uint64_t Budget = 1;
          auto Step = llvm::cantFail(Execution.run(Machine, Program, Budget));
          ASSERT_TRUE(Step.OwnerError.empty()) << Step.OwnerError;
          EXPECT_EQ(Step.Attempts, 1u);
          EXPECT_EQ(Step.Completions, FirmwareBreakpoint ? 0u : 1u);
          EXPECT_EQ(Machine.SMM->Active, FirmwareBreakpoint);
          if (FirmwareBreakpoint) {
            EXPECT_EQ(Step.DeliveredVector, 1u);
            EXPECT_EQ(R.PC, 0xa000u);
            EXPECT_EQ(word(bytes(*Private), uint16_t(R.GPR[4]), 2), 0x8000u);
            EXPECT_EQ(Machine.Debug->DR6 & 0x4001, 1u);
          } else if (FirmwareTF) {
            EXPECT_EQ(Step.DeliveredVector, 1u);
            EXPECT_EQ(Machine.Debug->DR6 & 0x4001, 0x4000u);
            const auto Width = OutsideMode == Mode::Long64 ? 8u : 4u;
            const auto Image = bytes(*Memory);
            const auto SP = uint32_t(R.GPR[4]);
            EXPECT_EQ(word(Image, SP, Width), 0x1000u);
            EXPECT_EQ(bool(word(Image, SP + 2 * Width, Width) & 0x100),
                      OutsideTF);
          } else {
            EXPECT_EQ(Step.DeliveredVector, 0u);
            EXPECT_EQ(R.PC, 0x1000u);
            EXPECT_EQ(bool(R.RFlags & 0x100), OutsideTF);
          }
        }
}

TEST_F(X86ModeInterruptTest, RsmChecksTheSaveImageBeforeBranchSingleStepping) {
  const std::optional<bool> Profiles[] = {std::nullopt, false, true};
  enum class Save { Valid, InvalidDebug, MissingPDPTE };
  unsigned Cases = 0;
  for (auto Profile : Profiles)
    for (auto Image : {Save::Valid, Save::InvalidDebug, Save::MissingPDPTE}) {
      SCOPED_TRACE(testing::Message()
                   << "branch=" << (Profile ? int(*Profile) : -1)
                   << " save=" << int(Image));
      start(Mode::Long64);
      Machine.SMM.emplace();
      Machine.Debug.emplace();
      Options.Legacy.Debug = Options.Long.DebugProfile =
          X86InstructionDebugProfile::BranchWatchpoints;
      Options.RsmCountsAsBranch = Profile;
      auto &R = Machine.Core.Core.Core.Core.Core.Core;
      R.GPR[4] = 0xf000;
      auto Firmware = RAM;
      Firmware.Memory.front().Bytes[0x38000] = 0x0f;
      Firmware.Memory.front().Bytes[0x38001] = 0xaa;
      Program = {{0x38000, Mode::Real16, {0x0f, 0xaa}}};
      bindMemory();
      auto Private =
          llvm::cantFail(X86PagedMemory::create(Firmware, Features, Controls));
      X86ModeExecution Execution(*Memory, Options, *Private);
      ASSERT_TRUE(llvm::cantFail(Execution.systemManagementInterrupt(Machine))
                      .EventAccepted);
      R.RFlags = 0x102;
      Machine.Debug->DebugCTL = kX86BranchTraceFlag;
      const auto Edit = [&](uint64_t Address, uint64_t Value) {
        ASSERT_EQ(Private->physicalMemory()
                      .storeScalar(Address, GuestScalarWidth::I64, Value)
                      .Status,
                  GuestMemoryAccessStatus::Completed);
      };
      if (Image == Save::InvalidDebug)
        ASSERT_NO_FATAL_FAILURE(Edit(0x3ff68, uint64_t{1} << 32));
      if (Image == Save::MissingPDPTE) {
        ASSERT_NO_FATAL_FAILURE(Edit(0x3fed0, 0));          // EFER without LMA.
        ASSERT_NO_FATAL_FAILURE(Edit(0x3ff48, 0x20));       // CR4.PAE.
        ASSERT_NO_FATAL_FAILURE(Edit(0x3ff50, 0x40000));    // Unbacked PDPT.
        ASSERT_NO_FATAL_FAILURE(Edit(0x3ff58, 0x80000011)); // CR0.PG and PE.
      }
      uint64_t Budget = 1;
      auto Result = Execution.run(Machine, Program, Budget);
      ++Cases;
      if (Image == Save::MissingPDPTE) {
        ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
        EXPECT_EQ(Result->Status, X86LegacyExecutionStatus::BackingFault);
        EXPECT_TRUE(Result->BackingFault.has_value());
        EXPECT_EQ(Result->Completions, 0u);
        EXPECT_FALSE(Machine.Shutdown);
      } else if (Image == Save::InvalidDebug) {
        ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
        EXPECT_EQ(Result->Status, X86LegacyExecutionStatus::Shutdown);
        EXPECT_EQ(Result->Completions, 0u);
        EXPECT_TRUE(Machine.Shutdown);
      } else if (!Profile) {
        ASSERT_FALSE(bool(Result));
        EXPECT_NE(
            llvm::toString(Result.takeError()).find("branch single-stepping"),
            std::string::npos);
      } else {
        ASSERT_TRUE(bool(Result)) << llvm::toString(Result.takeError());
        ASSERT_EQ(Result->Completions, 1u);
        EXPECT_FALSE(Machine.SMM->Active);
        EXPECT_EQ(Result->DeliveredVector, *Profile ? 1u : 0u);
        EXPECT_EQ(Machine.Debug->DR6 & 0x4000, *Profile ? 0x4000u : 0u);
        EXPECT_EQ(Machine.Debug->DebugCTL, *Profile ? 0u : kX86BranchTraceFlag);
        if (*Profile) {
          EXPECT_EQ(word(bytes(*Memory), uint32_t(R.GPR[4]), 8), 0x1000u);
        } else {
          EXPECT_EQ(R.PC, 0x1000u);
        }
        continue;
      }
      EXPECT_TRUE(Machine.SMM->Active);
      EXPECT_EQ(R.PC, 0x8000u);
      EXPECT_EQ(Machine.Debug->DR6 & 0x4000, 0u);
      EXPECT_EQ(Machine.Debug->DebugCTL, kX86BranchTraceFlag);
    }
  RecordProperty("software_profile_cases", Cases);
}
