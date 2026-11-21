//===- X86ModeSysenterTests.inc - Mixed-mode SYSENTER/SYSEXIT ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Original fast system transfers with changing code width and privilege.
//===----------------------------------------------------------------------===//

class X86ModeFastSystemTest : public X86ModeInterruptTest {
protected:
  enum Transfer { Enter, Exit, ExitQ, IgnoredREX };
  static constexpr uint64_t Upper = UINT64_C(0xffff800000000000);

  void setupTransfer(Mode M, Transfer T) {
    start(M);
    Options.Legacy.FastSystem = X86LegacyFastSystemProfile::IntelSEP;
    auto &V = Machine.Core.Core;
    auto &R = V.Core.Core.Core.Core;
    R.RFlags = 0x3202;
    Machine.Core.SysenterCS = 0x18;
    Machine.Core.SysenterEIP = Upper + 0x2000;
    Machine.Core.SysenterESP = Upper + 0x65000;
    R.GPR[1] = Upper + 0x75000;
    R.GPR[2] = Upper + 0x2100;
    if (T == Enter) {
      for (auto *Segment : {&V.Core.CS, &V.Core.SS}) {
        Segment->Selector |= 3;
        Segment->Attributes |= 0x60;
      }
    }
    // Both aliases execute user or supervisor code. The two destination
    // stack pages deliberately remain outside the backing RAM.
    put(0x10000, 0x11007);
    put(0x10800, 0x11007);
    put(0x11000, 0x12007);
    put(0x12000, 0x87);
    const auto Encoding = T == Enter   ? std::vector<uint8_t>{0x66, 0x0f, 0x34}
                          : T == ExitQ ? std::vector<uint8_t>{0x48, 0x0f, 0x35}
                          : T == IgnoredREX
                              ? std::vector<uint8_t>{0x48, 0x66, 0x0f, 0x35}
                              : std::vector<uint8_t>{0x66, 0x0f, 0x35};
    add(0x1000, M, Encoding);
    for (unsigned Address : {0x2000u, 0x2100u}) {
      RAM.Memory.front().Bytes[Address] = 0xe6;
      RAM.Memory.front().Bytes[Address + 1] = 0x81;
    }
  }

  void executeTransfer(Mode M, Transfer T) {
    const bool Wide = T == ExitQ || (T == Enter && (Controls.EFER & 0x400));
    bindMemory();
    X86ModeExecution Execution(*Memory, Options);
    uint64_t Budget = 1;
    auto Done = llvm::cantFail(Execution.run(Machine, Program, Budget));
    ASSERT_EQ(Done.Completions, 1u) << Done.OwnerError;
    EXPECT_TRUE(Done.CSWritten);
    EXPECT_TRUE(Done.SSWritten);
    auto &V = Machine.Core.Core;
    auto &R = V.Core.Core.Core.Core;
    EXPECT_EQ(R.PC, (Wide ? Upper : 0) + (T == Enter ? 0x2000 : 0x2100));
    EXPECT_EQ(R.GPR[4], (Wide ? Upper : 0) + (T == Enter ? 0x65000 : 0x75000));
    EXPECT_EQ(R.RFlags, T == Enter ? 0x3002u : 0x3202u);
    EXPECT_EQ(V.Core.CS.Selector, T == Enter ? 0x18 : T == ExitQ ? 0x3b : 0x2b);
    EXPECT_EQ(V.Core.SS.Selector, V.Core.CS.Selector + 8u);
    EXPECT_EQ(V.Core.CS.Attributes,
              (Wide ? 0xa09b : 0xc09b) | (T == Enter ? 0 : 0x60));
    EXPECT_EQ(V.Core.SS.Attributes, T == Enter ? 0xc093u : 0xc0f3u);
    EXPECT_EQ(R.GPR[15], 0x1111222233334444ull);
    EXPECT_EQ(llvm::cantFail(Execution.mode(Machine)),
              Wide                    ? Mode::Long64
              : Controls.EFER & 0x400 ? Mode::Compatibility32
                                      : Mode::Protected32);
  }
};

TEST_F(X86ModeFastSystemTest, TransfersUseFullMsrsAndSelectDestinationWidth) {
  for (auto M : Modes)
    for (auto T : {Enter, Exit, ExitQ, IgnoredREX}) {
      if ((T == ExitQ || T == IgnoredREX) && M != Mode::Long64)
        continue;
      SCOPED_TRACE(unsigned(M));
      SCOPED_TRACE(int(T));
      setupTransfer(M, T);
      executeTransfer(M, T);
    }
}

TEST_F(X86ModeFastSystemTest,
       SysenterMsrsUseProcessorAddressWidthBeforeTransfer) {
  for (auto M : {Mode::Protected32, Mode::Compatibility16, Mode::Long64}) {
    for (bool LA57 : {false, true}) {
      SCOPED_TRACE(unsigned(M));
      SCOPED_TRACE(LA57);
      start(M);
      Options.Legacy.FastSystem = X86LegacyFastSystemProfile::IntelSEP;
      Features.LA57 = LA57;
      auto &R = Machine.Core.Core.Core.Core.Core.Core;
      if (M == Mode::Protected32)
        put(0x9068, 0x40d0 | (uint64_t(8) << 16) | (uint64_t(0x8e) << 40));
      for (unsigned PC : {0x1000u, 0x1004u, 0x1008u, 0x100cu})
        add(PC, M, {0x0f, 0x30});
      for (unsigned PC : {0x1002u, 0x1006u, 0x100au})
        add(PC, M, {0x0f, 0x32});
      bindMemory();
      X86ModeExecution Execution(*Memory, Options);
      auto Step = [&] {
        uint64_t Budget = 1;
        return llvm::cantFail(Execution.run(Machine, Program, Budget));
      };
      for (uint32_t Index : {0x174u, 0x175u, 0x176u}) {
        const uint64_t Value =
            Index == 0x174
                ? UINT64_C(0xcafef00dbeef0018)
                : (LA57 ? UINT64_C(0x0080000000000000) : Upper) + Index;
        R.GPR[1] = Index;
        R.GPR[0] = uint32_t(Value);
        R.GPR[2] = Value >> 32;
        ASSERT_EQ(Step().Completions, 1u);
        R.GPR[0] = R.GPR[2] = 0;
        ASSERT_EQ(Step().Completions, 1u);
        EXPECT_EQ(uint32_t(R.GPR[0]), uint32_t(Value));
        EXPECT_EQ(uint32_t(R.GPR[2]), Index == 0x174 ? 0u : Value >> 32);
      }
      const uint64_t Old = Machine.Core.SysenterEIP;
      R.GPR[0] = 9;
      R.GPR[2] = LA57 ? 0x1000000 : 0x8000;
      auto Fault = Step();
      ASSERT_EQ(Fault.Status, X86LegacyExecutionStatus::EventDelivered)
          << Fault.OwnerError;
      EXPECT_EQ(Fault.DeliveredVector, 13u);
      EXPECT_EQ(Fault.Completions, 0u);
      EXPECT_EQ(Machine.Core.SysenterEIP, Old);
    }
  }
}

TEST_F(X86ModeFastSystemTest, FaultsPrecedeFastTransferStateWrites) {
  enum Case {
    NullCS,
    BadTarget,
    BadStack,
    UserExit,
    LockedUserExit,
    ChangedBytes
  };
  for (auto C :
       {NullCS, BadTarget, BadStack, UserExit, LockedUserExit, ChangedBytes}) {
    SCOPED_TRACE(int(C));
    setupTransfer(Mode::Long64, ExitQ);
    auto &V = Machine.Core.Core;
    auto &R = V.Core.Core.Core.Core;
    if (C == NullCS || C == ChangedBytes)
      Machine.Core.SysenterCS = 3;
    if (C == BadTarget || C == BadStack)
      R.GPR[C == BadTarget ? 2 : 1] = UINT64_C(1) << 47;
    if (C == UserExit || C == LockedUserExit) {
      V.Core.CS = {0, UINT32_MAX, 27, 0xa0fb};
      V.Core.SS = {0, UINT32_MAX, 19, 0xc0f3};
    }
    if (C == LockedUserExit) {
      Program.clear();
      add(0x1000, Mode::Long64, {0xf0, 0x48, 0x0f, 0x35});
    }
    if (C == ChangedBytes)
      RAM.Memory.front().Bytes[0x1002] = 0x34;
    const auto BeforeCS = V.Core.CS.Selector;
    const auto BeforeSP = R.GPR[4];
    bindMemory();
    X86ModeExecution Execution(*Memory, Options);
    uint64_t Budget = 1;
    auto Step = Execution.run(Machine, Program, Budget);
    ASSERT_TRUE(bool(Step)) << llvm::toString(Step.takeError());
    ASSERT_EQ(Step->Completions, 0u);
    if (C == ChangedBytes) {
      EXPECT_EQ(Step->Status, X86LegacyExecutionStatus::CodeChanged);
      EXPECT_EQ(R.PC, 0x1000u);
      EXPECT_EQ(V.Core.CS.Selector, BeforeCS);
      EXPECT_EQ(R.GPR[4], BeforeSP);
      continue;
    }
    ASSERT_EQ(Step->Status, X86LegacyExecutionStatus::EventDelivered)
        << Step->OwnerError;
    EXPECT_EQ(Step->DeliveredVector, C == LockedUserExit ? 6u : 13u);
    const unsigned ExceptionCode = C == LockedUserExit ? 0 : 8;
    const auto Frame = bytes(*Memory);
    const auto SP = R.GPR[4] + ExceptionCode;
    EXPECT_EQ(word(Frame, SP, 8), 0x1000u);
    EXPECT_EQ(word(Frame, SP + 8, 8), BeforeCS);
    EXPECT_EQ(word(Frame, SP + 24, 8), BeforeSP);
  }
}

TEST_F(X86ModeFastSystemTest, SingleStepUsesTheRetiredFullAddressAndStack) {
  for (auto T : {Enter, Exit, ExitQ}) {
    SCOPED_TRACE(int(T));
    const auto M = T == Enter ? Mode::Compatibility16 : Mode::Long64;
    setupTransfer(M, T);
    Machine.Debug.emplace();
    Options.Legacy.Debug = Options.Long.DebugProfile =
        X86InstructionDebugProfile::Watchpoints;
    auto &R = Machine.Core.Core.Core.Core.Core.Core;
    R.RFlags |= 0x100;
    Machine.Core.SysenterESP = Upper + 0xf000;
    bindMemory();
    X86ModeExecution Execution(*Memory, Options);
    uint64_t Budget = 1;
    auto Done = llvm::cantFail(Execution.run(Machine, Program, Budget));
    ASSERT_EQ(Done.Status, X86LegacyExecutionStatus::EventDelivered)
        << Done.OwnerError;
    EXPECT_EQ(Done.Attempts, 1u);
    EXPECT_EQ(Done.Completions, 1u);
    EXPECT_EQ(Done.DeliveredVector, 1u);
    EXPECT_EQ(R.PC, 0x4010u);
    const uint64_t Frame = T == Enter ? 0xefd8 : 0x1efd8;
    EXPECT_EQ(R.GPR[4], T == Enter ? Upper + Frame : Frame);
    const auto Image = bytes(*Memory);
    EXPECT_EQ(word(Image, Frame, 8), T == Enter   ? Upper + 0x2000
                                     : T == ExitQ ? Upper + 0x2100
                                                  : 0x2100);
    EXPECT_EQ(word(Image, Frame + 8, 8), T == Enter   ? 0x18u
                                         : T == ExitQ ? 0x3bu
                                                      : 0x2bu);
    EXPECT_EQ(word(Image, Frame + 16, 8), T == Enter ? 0x3102u : 0x3302u);
    EXPECT_EQ(word(Image, Frame + 24, 8), T == Enter   ? Upper + 0xf000
                                          : T == ExitQ ? Upper + 0x75000
                                                       : 0x75000);
  }
}

TEST_F(X86ModeFastSystemTest,
       VM86EnterAndRealAddressFaultUseTheirOriginalModes) {
  for (bool VM86 : {false, true}) {
    SCOPED_TRACE(VM86);
    start(Mode::Protected32);
    Options.Legacy.FastSystem = X86LegacyFastSystemProfile::IntelSEP;
    Options.Legacy.Virtual8086 = X86LegacyVM86Profile::IntelVME;
    auto &V = Machine.Core.Core;
    auto &R = V.Core.Core.Core.Core;
    R.RFlags = VM86 ? 0x23202 : 0x202;
    R.PC = 0x100;
    R.GPR[4] = 0xb000;
    if (!VM86)
      Controls.CR0 &= ~1u;
    const uint16_t Access = VM86 ? 0xf3 : 0x93;
    V.Core.CS = {0x1000, 0xffff, 0x100, Access};
    V.Core.SS = V.DS = V.ES = {0, 0xffff, 0, Access};
    V.FS = V.GS = {0xffff, 0, Access};
    V.GDTR = {};
    V.IDTR = {0, 0x3ff};
    Machine.Core.SysenterCS = 0x18;
    Machine.Core.SysenterEIP = Upper + 0x2000;
    Machine.Core.SysenterESP = Upper + 0x65000;
    put(13 * 4, 0x23400);
    add(0x1100, VM86 ? Mode::Virtual8086 : Mode::Real16, {0x0f, 0x34});
    bindMemory();
    X86ModeExecution Execution(*Memory, Options);
    uint64_t Budget = 1;
    auto Observed = Execution.run(Machine, Program, Budget);
    ASSERT_TRUE(bool(Observed)) << llvm::toString(Observed.takeError());
    const auto &Done = *Observed;
    ASSERT_TRUE(Done.OwnerError.empty()) << Done.OwnerError;
    if (VM86) {
      EXPECT_EQ(Done.Completions, 1u);
      EXPECT_EQ(R.PC, 0x2000u);
      EXPECT_EQ(R.GPR[4], 0x65000u);
      EXPECT_EQ(R.RFlags, 0x3002u);
      EXPECT_EQ(llvm::cantFail(Execution.mode(Machine)), Mode::Protected32);
    } else {
      ASSERT_EQ(Done.Status, X86LegacyExecutionStatus::EventDelivered);
      EXPECT_EQ(Done.DeliveredVector, 13u);
      EXPECT_EQ(Done.Completions, 0u);
      EXPECT_EQ(R.PC, 0x3400u);
      EXPECT_EQ(V.Core.CS.Selector, 2u);
    }
  }
}

#if defined(__linux__) && defined(__x86_64__)
TEST_F(X86ModeFastSystemTest, OriginalTransfersMatchKvmAcrossModes) {
  for (auto M : Modes)
    for (auto T : {Enter, Exit, ExitQ, IgnoredREX}) {
      if ((T == ExitQ || T == IgnoredREX) && M != Mode::Long64)
        continue;
      SCOPED_TRACE(unsigned(M));
      SCOPED_TRACE(int(T));
      setupTransfer(M, T);
      X64PagedNativeCPU Native;
      ASSERT_NO_FATAL_FAILURE(Native.initialize());
      if (Native.CPU < 0)
        GTEST_SKIP() << "KVM unavailable";
      ASSERT_NO_FATAL_FAILURE(initializeOriginal(Native));
      struct {
        uint32_t Count = 3, Pad = 0;
        kvm_msr_entry Entries[3]{};
      } MSRs;
      MSRs.Entries[0].index = 0x174;
      MSRs.Entries[0].data = Machine.Core.SysenterCS;
      MSRs.Entries[1].index = 0x175;
      MSRs.Entries[1].data = Machine.Core.SysenterESP;
      MSRs.Entries[2].index = 0x176;
      MSRs.Entries[2].data = Machine.Core.SysenterEIP;
      ASSERT_EQ(ioctl(Native.CPU, KVM_SET_MSRS, &MSRs), 3);
      auto &R = Machine.Core.Core.Core.Core.Core.Core;
      kvm_regs Original{};
      ASSERT_EQ(ioctl(Native.CPU, KVM_GET_REGS, &Original), 0);
      Original.rcx = R.GPR[1];
      Original.rdx = R.GPR[2];
      Original.r15 = R.GPR[15];
      ASSERT_EQ(ioctl(Native.CPU, KVM_SET_REGS, &Original), 0);
      ASSERT_EQ(ioctl(Native.CPU, KVM_RUN, 0), 0);
      ASSERT_EQ(Native.Run->exit_reason, unsigned(KVM_EXIT_IO));
      ASSERT_EQ(Native.Run->io.port, 0x81);
      ASSERT_EQ(ioctl(Native.CPU, KVM_GET_REGS, &Original), 0);
      kvm_sregs Segs{};
      ASSERT_EQ(ioctl(Native.CPU, KVM_GET_SREGS, &Segs), 0);
      executeTransfer(M, T);
      const auto &V = Machine.Core.Core;
      EXPECT_EQ(R.PC, Original.rip);
      EXPECT_EQ(R.GPR[4], Original.rsp);
      EXPECT_EQ(R.RFlags, Original.rflags);
      EXPECT_EQ(R.GPR[15], Original.r15);
      EXPECT_EQ(V.Core.CS.Selector, Segs.cs.selector);
      EXPECT_EQ(V.Core.CS.Base, Segs.cs.base);
      EXPECT_EQ(V.Core.CS.Limit, Segs.cs.limit);
      EXPECT_EQ(bool(V.Core.CS.Attributes & 0x2000), bool(Segs.cs.l));
      EXPECT_EQ(bool(V.Core.CS.Attributes & 0x4000), bool(Segs.cs.db));
      EXPECT_EQ(V.Core.SS.Selector, Segs.ss.selector);
      EXPECT_EQ(V.Core.SS.Base, Segs.ss.base);
      EXPECT_EQ(V.Core.SS.Limit, Segs.ss.limit);
    }
}
#endif
