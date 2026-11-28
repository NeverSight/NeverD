//===- X86ModeSgxNotificationTests.inc - SGX AEX notification ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Notification delivery and EDECCSSA over separately owned enclave SSA slots.
//===----------------------------------------------------------------------===//

class X86ModeSgxThreadTest : public X86ModeInterruptTest {
protected:
  static constexpr uint64_t Base = 0x40000, Root = 0x50000;
  static constexpr unsigned GPR = 4096 - 184;
  const std::vector<uint8_t> Enclu{0x0f, 1, 0xd7};
  X86SgxPage Secs, TCS, Code, Previous, Next;

  void startThread() {
    namespace le = llvm::support::endian;
    start(Mode::Long64);
    auto &V = Machine.Core.Core;
    auto &R = V.Core.Core.Core.Core;
    V.Core.CS = {0, UINT32_MAX, 0x33, 0xa0fb};
    V.Core.SS = V.DS = V.ES = {0, UINT32_MAX, 0x3b, 0xc0f3};
    V.FS = V.GS = {UINT32_MAX, 0x3b, 0xc0f3};
    Controls.CR4 |= 0x200;
    Options.Legacy.ControlRegisters->OtherCR4Features |= 0x200;
    put(0x8030, 0x00affb000000ffffULL);
    put(0x8038, 0x00cff3000000ffffULL);
    put(0x10000, 0x11007);
    put(0x11000, 0x12007);
    put(0x12000, 0x87);
    Secs = {.Physical = Root, .Type = X86SgxPageType::SECS};
    le::write64le(Secs.Bytes.data(), 0x8000);
    le::write64le(Secs.Bytes.data() + 8, Base);
    le::write32le(Secs.Bytes.data() + 16, 1);
    le::write64le(Secs.Bytes.data() + 48, 5);
    le::write64le(Secs.Bytes.data() + 56, 3);
    TCS = {.Physical = Base,
           .Linear = Base,
           .OwnerSECS = Root,
           .Type = X86SgxPageType::TCS};
    le::write64le(TCS.Bytes.data() + 16, 0x2000);
    le::write32le(TCS.Bytes.data() + 28, 2);
    le::write64le(TCS.Bytes.data() + 32, 0x1000);
    Code = {.Physical = Base + 0x1000,
            .Linear = Base + 0x1000,
            .OwnerSECS = Root,
            .Permissions = MemoryPermission::Read | MemoryPermission::Execute};
    Previous = {.Physical = Base + 0x2000,
                .Linear = Base + 0x2000,
                .OwnerSECS = Root,
                .Permissions =
                    MemoryPermission::Read | MemoryPermission::Write};
    Next = Previous;
    Next.Physical = Next.Linear = Base + 0x3000;
    R.GPR[0] = 2;
    R.GPR[1] = 0x1800;
    R.GPR[3] = Base;
    R.ZMM[2][0] = 0xad;
    add(0x1000, Mode::Long64, Enclu);
  }

  void privateInstruction(unsigned Offset, llvm::ArrayRef<uint8_t> Bytes) {
    std::copy(Bytes.begin(), Bytes.end(), Code.Bytes.begin() + Offset);
    Program.push_back({Code.Linear + Offset, Mode::Long64, Bytes.vec()});
  }

  void bindThread(X86SgxFeatures FeaturesSGX = {}) {
    auto EPC = llvm::cantFail(X86SgxMemory::create(
        {Secs, TCS, Code, Previous, Next}, {}, FeaturesSGX));
    std::array<uint8_t, 160> X87{};
    X87[0] = 0x7f;
    X87[1] = 3;
    llvm::cantFail(EPC->bindX87(X87, 3));
    Memory = llvm::cantFail(
        X86PagedMemory::createWithSgx(RAM, Features, Controls, std::move(EPC)));
  }
};

TEST_F(X86ModeSgxThreadTest, PendingVirtualInterruptRestartsEnclaveExit) {
  startThread();
  privateInstruction(0, Enclu);
  Controls.CR4 |= 2; // PVI.
  const auto Outside = Machine;
  bindThread();
  X86ModeExecution Execution(*Memory, Options);
  uint64_t Budget = 1;
  auto Enter = llvm::cantFail(Execution.run(Machine, Program, Budget));
  ASSERT_EQ(Enter.Completions, 1u) << Enter.OwnerError;
  ASSERT_TRUE(Memory->sgxMemory()->active());
  auto &R = Machine.Core.Core.Core.Core.Core.Core;
  ASSERT_EQ(R.PC, Code.Linear);
  R.GPR[0] = 4;
  R.GPR[3] = 0x2000;
  R.RFlags |= 0x180000; // Both virtual interrupt bits pending at EEXIT.
  Budget = 1;
  auto Interrupted = llvm::cantFail(Execution.run(Machine, Program, Budget));
  ASSERT_EQ(Interrupted.DeliveredVector, 13u) << Interrupted.OwnerError;
  EXPECT_EQ(Interrupted.Attempts, 1u);
  EXPECT_EQ(Interrupted.Completions, 0u);
  EXPECT_FALSE(Memory->sgxMemory()->active());
  const auto OutsideFrame = bytes(*Memory);
  EXPECT_EQ(word(OutsideFrame, R.GPR[4], 8), 0u);
  EXPECT_EQ(word(OutsideFrame, R.GPR[4] + 8, 8), 0x1800u);

  // The outside handler resolves the virtual interrupt condition before
  // ERESUME. The retained SSA must select the original EEXIT and operands.
  Machine = Outside;
  Controls.CR4 &= ~uint64_t{2};
  llvm::cantFail(Memory->bindControls(Controls));
  R.GPR[0] = 3;
  Budget = 1;
  auto Resume = llvm::cantFail(Execution.run(Machine, Program, Budget));
  ASSERT_EQ(Resume.Completions, 1u) << Resume.OwnerError;
  ASSERT_TRUE(Memory->sgxMemory()->active());
  EXPECT_EQ(R.PC, Code.Linear);
  EXPECT_EQ(R.GPR[0], 4u);
  EXPECT_EQ(R.GPR[3], 0x2000u);
  Budget = 1;
  auto Exit = llvm::cantFail(Execution.run(Machine, Program, Budget));
  EXPECT_EQ(Exit.Completions, 1u) << Exit.OwnerError;
  EXPECT_FALSE(Memory->sgxMemory()->active());
  EXPECT_EQ(R.PC, 0x2000u);
}

TEST_F(X86ModeSgxThreadTest,
       EnclaveNotificationResumeAndDecrementSelectCorrectFrames) {
  namespace le = llvm::support::endian;
  enum Case { Notify, LastSlot, BlockedNext, AttributesDisagree, LeafDisabled };
  for (auto Scenario :
       {Notify, LastSlot, BlockedNext, AttributesDisagree, LeafDisabled}) {
    SCOPED_TRACE(int(Scenario));
    startThread();
    auto &V = Machine.Core.Core;
    auto &R = V.Core.Core.Core.Core;
    le::write64le(Secs.Bytes.data() + 48,
                  Scenario == AttributesDisagree ? 5 : 0x405);
    le::write64le(TCS.Bytes.data() + 8, 2);
    le::write32le(TCS.Bytes.data() + 24, 1);
    le::write32le(TCS.Bytes.data() + 28, Scenario == LastSlot ? 1 : 2);
    le::write64le(Previous.Bytes.data() + GPR + 136, Base + 0x1060);
    Previous.Bytes[GPR + 167] = 1;
    le::write32le(Previous.Bytes.data() + 24, 0x10000);
    Next.Blocked = Scenario == BlockedNext;
    privateInstruction(0, Enclu);
    privateInstruction(3, Enclu);
    R.GPR[0] = 3;
    const auto Outside = Machine;
    bindThread({.AEXNotify = true, .DecrementSSA = Scenario != LeafDisabled});
    X86ModeExecution Execution(*Memory, Options);
    uint64_t Budget = 1;
    auto Enter = llvm::cantFail(Execution.run(Machine, Program, Budget));
    if (Scenario == LastSlot || Scenario == AttributesDisagree ||
        Scenario == BlockedNext) {
      ASSERT_EQ(Enter.Completions, 0u);
      EXPECT_EQ(Enter.DeliveredVector, Scenario == BlockedNext ? 14u : 13u);
      EXPECT_FALSE(Memory->sgxMemory()->active());
      if (Scenario == BlockedNext)
        EXPECT_EQ(V.Core.Core.CR2, Next.Linear);
      continue;
    }
    ASSERT_EQ(Enter.Completions, 1u) << Enter.OwnerError;
    ASSERT_TRUE(Memory->sgxMemory()->active());
    EXPECT_EQ(R.PC, Code.Linear);
    EXPECT_EQ(R.GPR[0], 1u);
    EXPECT_EQ(R.GPR[1], 0x1003u);
    EXPECT_EQ(R.ZMM[2][0], 0xadu);
    std::array<uint8_t, 1> Disable{};
    ASSERT_EQ(llvm::cantFail(Memory->store({Previous.Linear + GPR + 167,
                                            MemoryAccessKind::Write, 3},
                                           Disable))
                  .Status,
              X86PageWalkStatus::Completed);
    R.GPR[0] = 9;
    Budget = 1;
    auto Select = llvm::cantFail(Execution.run(Machine, Program, Budget));
    if (Scenario == LeafDisabled) {
      EXPECT_EQ(Select.DeliveredVector, 13u);
      EXPECT_FALSE(Memory->sgxMemory()->active());
      continue;
    }
    ASSERT_EQ(Select.Completions, 1u) << Select.OwnerError;
    EXPECT_EQ(R.PC, Code.Linear + 3);
    R.GPR[9] = 0xabcdef1234567890ull;
    ASSERT_TRUE(llvm::cantFail(
                    Execution.interrupt(Machine, X86EventKind::Interrupt, 64))
                    .EventAccepted);
    ASSERT_FALSE(Memory->sgxMemory()->active());
    Machine = Outside;
    llvm::cantFail(Memory->bindControls(Controls));
    Budget = 1;
    auto Resume = llvm::cantFail(Execution.run(Machine, Program, Budget));
    ASSERT_EQ(Resume.Completions, 1u) << Resume.OwnerError;
    ASSERT_TRUE(Memory->sgxMemory()->active());
    EXPECT_EQ(R.PC, Code.Linear + 3);
    EXPECT_EQ(R.GPR[9], 0xabcdef1234567890ull);
    R.GPR[0] = 9;
    Budget = 1;
    auto Empty = llvm::cantFail(Execution.run(Machine, Program, Budget));
    EXPECT_EQ(Empty.DeliveredVector, 13u);
  }
}

TEST_F(X86ModeSgxThreadTest, DebugOptInSelectsTrapAndBreakpointState) {
  enum Case { SingleStep, Int1, Int3, Execute, Suppressed, Data };
  for (auto Scenario : {SingleStep, Int1, Int3, Execute, Suppressed, Data}) {
    SCOPED_TRACE(int(Scenario));
    startThread();
    auto &V = Machine.Core.Core;
    auto &R = V.Core.Core.Core.Core;
    Machine.Debug.emplace();
    Options.Legacy.Debug = Options.Long.DebugProfile =
        X86InstructionDebugProfile::Watchpoints;
    const bool OptIn = Scenario != Suppressed;
    llvm::support::endian::write64le(Secs.Bytes.data() + 48, 7);
    llvm::support::endian::write64le(TCS.Bytes.data() + 8, OptIn ? 1 : 0);
    if (Scenario == SingleStep)
      R.RFlags |= 0x100;
    if (Scenario == Int1 || Scenario == Int3)
      privateInstruction(0, {uint8_t(Scenario == Int1 ? 0xf1 : 0xcc)});
    else
      privateInstruction(0, Enclu);
    if (Scenario == Execute || Scenario == Suppressed) {
      Machine.Debug->DR[0] = Code.Linear;
      Machine.Debug->DR7 |= 1;
    }
    bindThread();
    auto Outside = Machine;
    X86ModeExecution Execution(*Memory, Options);
    uint64_t Budget = 1;
    auto Enter = llvm::cantFail(Execution.run(Machine, Program, Budget));
    ASSERT_EQ(Enter.Completions, 1u) << Enter.OwnerError;
    if (Scenario == SingleStep) {
      EXPECT_EQ(Enter.DeliveredVector, 1u);
      EXPECT_TRUE(Machine.Debug->DR6 & 0x4000);
    } else {
      ASSERT_TRUE(Memory->sgxMemory()->active());
      if (Scenario == Suppressed || Scenario == Data) {
        auto &D = *Machine.Debug;
        D.DR[1] = Previous.Linear;
        D.DR[2] = 0x1610;
        D.DR7 |= 0x03300014;
        X86WatchpointObservation Watch(*Memory, &D, Options.Long.DebugProfile);
        std::array<uint8_t, 1> B{};
        ASSERT_EQ(
            llvm::cantFail(
                Memory->load({Previous.Linear, MemoryAccessKind::Read, 3}, B))
                .Status,
            X86PageWalkStatus::Completed);
        EXPECT_EQ(Watch.Matches, OptIn ? 2u : 0u);
        ASSERT_EQ(
            llvm::cantFail(Memory->load({0x1610, MemoryAccessKind::Read, 3}, B))
                .Status,
            X86PageWalkStatus::Completed);
        EXPECT_EQ(Watch.Matches, OptIn ? 6u : 4u);
      }
      R.GPR[0] = 4;
      R.GPR[3] = 0x2000;
      Budget = 1;
      auto Stop = llvm::cantFail(Execution.run(Machine, Program, Budget));
      if (Scenario == Suppressed || Scenario == Data) {
        EXPECT_EQ(Stop.Completions, 1u) << Stop.OwnerError;
        EXPECT_EQ(R.PC, 0x2000u);
        EXPECT_FALSE(Memory->sgxMemory()->active());
        continue;
      }
      ASSERT_TRUE(Stop.OwnerError.empty()) << Stop.OwnerError;
      EXPECT_EQ(Stop.Completions, 0u);
      EXPECT_EQ(Stop.DeliveredVector, Scenario == Int3 ? 3u : 1u);
      if (Scenario == Execute)
        EXPECT_TRUE(Machine.Debug->DR6 & 1);
    }
    ASSERT_FALSE(Memory->sgxMemory()->active());
    EXPECT_EQ(word(bytes(*Memory), R.GPR[4], 8), 0x1800u);
    Machine = Outside;
    R.GPR[0] = 3;
    R.RFlags |= 0x100;
    Machine.Debug->DR7 = 0x400;
    llvm::cantFail(Memory->bindControls(Controls));
    Budget = 1;
    auto Resume = llvm::cantFail(Execution.run(Machine, Program, Budget));
    // ERESUME preserves the outside TF on opt-in, without stepping ERESUME.
    ASSERT_EQ(Resume.Completions, 1u);
    ASSERT_TRUE(Memory->sgxMemory()->active());
    EXPECT_EQ(R.PC, Code.Linear);
    EXPECT_TRUE(R.RFlags & 0x100);
    std::array<uint8_t, 8> SavedFlags{};
    ASSERT_EQ(llvm::cantFail(Memory->load({Previous.Linear + GPR + 128,
                                           MemoryAccessKind::Read, 3},
                                          SavedFlags))
                  .Status,
              X86PageWalkStatus::Completed);
    const auto Flags = llvm::support::endian::read64le(SavedFlags.data());
    EXPECT_EQ(Flags & 0x100u, 0u);
    if (Scenario == Int1 || Scenario == Int3)
      EXPECT_TRUE(Flags & 0x10000u);
  }
}
