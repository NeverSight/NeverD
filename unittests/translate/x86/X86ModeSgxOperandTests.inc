//===- X86ModeSgxOperandTests.inc - SGX1 leaf operands --------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Check alignment, paging, reserved fields, retirement and output
/// preservation.
//===----------------------------------------------------------------------===//

TEST_F(X86ModeSgxThreadTest, KeyAndReportCheckOperandsInArchitecturalOrder) {
  constexpr uint64_t Input = Base + 0x1400, Data = Base + 0x2000,
                     Output = Base + 0x2200, Missing = Base + 0x4000;
  struct Case {
    unsigned Leaf;
    uint64_t BX, CX, DX;
    unsigned InvalidByte;
    uint8_t Vector;
    uint32_t Error;
    uint64_t FaultAddress;
    bool FeatureAvailable = true;
  };
  // Input points into the read-only code page. Missing is in ELRANGE and
  // identity mapped by ordinary paging but has no matching EPCM entry.
  for (const Case &C : {Case{0, Missing, 1, 1, 0, 14, 0x8005, Missing},
                        Case{1, Missing, 1, 0, 0, 14, 0x8005, Missing},
                        Case{0, Input, Missing, 1, 128, 13, 0, 0},
                        Case{0, Input, Missing, 1, 0, 14, 0x8005, Missing},
                        Case{0, Input, Data + 64, Output, 0, 13, 0, 0},
                        Case{0, Input, Data, Output + 128, 0, 13, 0, 0},
                        Case{0, Input, Data, Input, 0, 14, 0x8007, Input},
                        Case{1, Input, Missing, 0, 78, 14, 0x8007, Missing},
                        Case{1, Input, Output + 8, 0, 0, 13, 0, 0},
                        Case{1, Input, Output, 0, 6, 13, 0, 0},
                        Case{1, Input, Output, 0, 78, 13, 0, 0},
                        Case{1, Input, Output, 0, 76, 13, 0, 0},
                        Case{1, Input, Output, 0, 3, 13, 0, 0},
                        Case{8, Input, 0, 0, 0, 13, 0, 0},
                        Case{8, Base - 4096, 0, 0, 0, 13, 0, 0},
                        Case{8, Missing, 0, 0, 0, 14, 0x8005, Missing},
                        Case{8, Missing, 0, 0, 0, 13, 0, 0, false},
                        Case{11, Missing, 1, 0, 0, 14, 0x8005, Missing},
                        Case{11, Input, Output + 16, 0, 0, 13, 0, 0},
                        Case{11, Input, Missing, 0, 78, 14, 0x8007, Missing},
                        Case{11, Input, Output, 0, 78, 13, 0, 0},
                        Case{10, Missing, 1, 1, 0, 14, 0x8005, Missing},
                        Case{10, Input + 32, Missing, 1, 0, 13, 0, 0},
                        Case{10, Input + 64, Output, 1, 0, 13, 0, 0},
                        Case{10, Input, Missing, 1, 0, 14, 0x8007, Missing},
                        Case{10, Missing, 1, 1, 0, 13, 0, 0, false},
                        Case{11, Missing, 1, 0, 0, 13, 0, 0, false}}) {
    SCOPED_TRACE(testing::Message()
                 << C.Leaf << " BX=" << C.BX << " CX=" << C.CX << " DX=" << C.DX
                 << " byte=" << C.InvalidByte);
    startThread();
    privateInstruction(0, {0x67, 0x0f, 0x01, 0xd7});
    llvm::support::endian::write32le(Secs.Bytes.data() + 20,
                                     1); // MISCSELECT.EXINFO.
    if (C.InvalidByte)
      Code.Bytes[0x400 + C.InvalidByte] = 1;
    const auto Outside = Machine;
    X86SgxFeatures Features{.SGX256 = C.FeatureAvailable && C.Leaf >= 10};
    if (C.FeatureAvailable && C.Leaf == 8)
      Features.VerifyReport2 = X86SgxVerifyReport2Profile::SDM093;
    bindThread(Features);
    X86ModeExecution Execution(*Memory, Options);
    uint64_t Budget = 1;
    ASSERT_EQ(
        llvm::cantFail(Execution.run(Machine, Program, Budget)).Completions,
        1u);
    auto &R = Machine.Core.Core.Core.Core.Core.Core;
    R.GPR[0] = C.Leaf;
    R.GPR[3] = C.BX;
    R.GPR[1] = C.CX;
    R.GPR[2] = C.DX;
    Budget = 1;
    auto Fault = llvm::cantFail(Execution.run(Machine, Program, Budget));
    ASSERT_EQ(Fault.DeliveredVector, C.Vector) << Fault.OwnerError;
    EXPECT_EQ(Fault.Attempts, 1u);
    EXPECT_EQ(Fault.Completions, 0u);
    ASSERT_FALSE(Memory->sgxMemory()->active());
    const auto Frame = bytes(*Memory);
    EXPECT_EQ(word(Frame, R.GPR[4], 8), C.Error);
    EXPECT_EQ(word(Frame, R.GPR[4] + 8, 8), 0x1800u);
    if (C.Vector == 14)
      EXPECT_EQ(Machine.Core.Core.Core.Core.CR2,
                C.FaultAddress & ~uint64_t{4095});
    Machine = Outside;
    llvm::cantFail(Memory->bindControls(Controls));
    R.GPR[0] = 3;
    Budget = 1;
    ASSERT_EQ(
        llvm::cantFail(Execution.run(Machine, Program, Budget)).Completions,
        1u);
    EXPECT_EQ(R.PC, Code.Linear);
    EXPECT_EQ(R.GPR[0], C.Leaf);
    EXPECT_EQ(R.GPR[3], C.BX);
    EXPECT_EQ(R.GPR[1], C.CX);
    EXPECT_EQ(R.GPR[2], C.DX);
    std::array<uint8_t, 16> ExInfo{};
    const auto Read = llvm::cantFail(Memory->load(
        {Previous.Linear + GPR - 16, MemoryAccessKind::Read, 3}, ExInfo));
    ASSERT_EQ(Read.Status, X86PageWalkStatus::Completed);
    EXPECT_EQ(llvm::support::endian::read64le(ExInfo.data()), C.FaultAddress);
    EXPECT_EQ(llvm::support::endian::read32le(ExInfo.data() + 8), C.Error);
  }
}

TEST_F(X86ModeSgxThreadTest,
       KeyErrorsRetireWithoutWritingOutputOrLeavingEnclave) {
  for (unsigned Name : {0u, 1u, 2u, 5u, 0xffffu}) {
    SCOPED_TRACE(Name);
    startThread();
    privateInstruction(0, {0x67, 0x0f, 0x01, 0xd7});
    llvm::support::endian::write16le(Code.Bytes.data() + 512, Name);
    std::fill_n(Previous.Bytes.begin() + 256, 16, 0xa5);
    bindThread();
    X86ModeExecution Execution(*Memory, Options);
    uint64_t Budget = 1;
    ASSERT_EQ(
        llvm::cantFail(Execution.run(Machine, Program, Budget)).Completions,
        1u);
    auto &R = Machine.Core.Core.Core.Core.Core.Core;
    R.GPR[0] = 0xff00112200000001ull;
    R.GPR[3] = Code.Linear + 512;
    R.GPR[1] = Previous.Linear + 256;
    R.RFlags |= 0x10895;
    const auto Flags = R.RFlags;
    Budget = 1;
    auto Key = llvm::cantFail(Execution.run(Machine, Program, Budget));
    ASSERT_EQ(Key.Completions, 1u) << Key.OwnerError;
    ASSERT_TRUE(Memory->sgxMemory()->active());
    EXPECT_EQ(Key.Attempts, 1u);
    EXPECT_EQ(R.GPR[0], Name > 4 ? 256u : 2u);
    EXPECT_EQ(R.PC, Code.Linear + 4);
    EXPECT_EQ(R.RFlags, (Flags & ~uint64_t{0x108d5}) | 0x40);
    std::array<uint8_t, 16> Out{};
    auto Read = llvm::cantFail(
        Memory->load({R.GPR[1], MemoryAccessKind::Read, 3}, Out));
    EXPECT_EQ(Read.Status, X86PageWalkStatus::Completed);
    EXPECT_TRUE(std::all_of(Out.begin(), Out.end(),
                            [](uint8_t B) { return B == 0xa5; }));
  }
}

void X86ModeSgxThreadTest::checkSavedEnclaveOperand(unsigned Leaf) {
  const bool Accept = Leaf == 5;
  startThread();
  privateInstruction(0, Enclu);
  put(0x12000, 0x13007);
  for (unsigned Page = 0; Page < 80; ++Page)
    put(0x13000 + 8 * Page, (Page * 4096) | 7);
  const uint64_t Hole = 0x13000 + 8 * (Next.Linear / 4096);
  const auto Original = RAM.Memory.front().Bytes;
  const auto Permissions = RAM.Memory.front().Permissions;
  RAM.Memory.front().Bytes.resize(Hole);
  RAM.Memory.push_back({Hole + 8,
                        Permissions,
                        0,
                        {Original.begin() + Hole + 8, Original.end()}});
  Next.Pending = Accept;
  Next.TrackingComplete = true;
  llvm::support::endian::write16le(Previous.Bytes.data() + 512,
                                   Accept ? 0x20b : 0xffff);
  bindThread({.SGX2 = Accept, .SGX256 = Leaf == 11});
  X86ModeExecution First(*Memory, Options);
  uint64_t Budget = 1;
  ASSERT_EQ(llvm::cantFail(First.run(Machine, Program, Budget)).Completions,
            1u);
  auto &R = Machine.Core.Core.Core.Core.Core.Core;
  R.GPR[0] = Leaf;
  R.GPR[3] = Previous.Linear + 512;
  R.GPR[1] = Next.Linear;
  Budget = 1;
  auto Stop = llvm::cantFail(First.run(Machine, Program, Budget));
  ASSERT_EQ(Stop.Status, X86LegacyExecutionStatus::BackingFault);
  ASSERT_TRUE(Machine.Pending);
  EXPECT_EQ(Stop.Attempts, 1u);
  EXPECT_EQ(Budget, 0u);
  // Rereading this value would require the sealing-key platform or
  // cause EACCEPT to fault on the replacement SECINFO.
  ASSERT_EQ(llvm::cantFail(Memory->store({R.GPR[3], MemoryAccessKind::Write, 3},
                                         std::array<uint8_t, 2>{4, 0}))
                .Status,
            X86PageWalkStatus::Completed);
  Stop = llvm::cantFail(First.run(Machine, {}, Budget));
  EXPECT_EQ(Stop.Status, X86LegacyExecutionStatus::BackingFault);
  EXPECT_EQ(Stop.Attempts, 0u);
  auto Fixed = RAM;
  Fixed.Memory = Memory->physicalMemory().snapshotMemoryRegions();
  Fixed.Memory.push_back(
      {Hole,
       Permissions,
       0,
       {Original.begin() + Hole, Original.begin() + Hole + 8}});
  auto Restored = X86PagedMemory::createWithSgx(
      Fixed, Features, Memory->controls(), Memory->sgxBank());
  ASSERT_TRUE(bool(Restored)) << llvm::toString(Restored.takeError());
  X86ModeExecution Resumed(**Restored, Options);
  auto Done = Resumed.run(Machine, {}, Budget);
  ASSERT_TRUE(bool(Done)) << llvm::toString(Done.takeError());
  ASSERT_EQ(Done->Completions, 1u) << Done->OwnerError;
  EXPECT_EQ(Done->Attempts, 0u);
  EXPECT_EQ(R.PC, Code.Linear + Enclu.size());
  EXPECT_EQ(R.GPR[0], Accept ? 0u : 256u);
  EXPECT_FALSE(Machine.Pending);
}

TEST_F(X86ModeSgxThreadTest, KeyRequestSurvivesOutputPageTableBackingRepair) {
  for (unsigned Leaf : {1u, 11u}) {
    SCOPED_TRACE(Leaf);
    checkSavedEnclaveOperand(Leaf);
  }
}

TEST_F(X86ModeSgxThreadTest,
       AcceptRetainsSecinfoAcrossRepeatedPageTableRepair) {
  checkSavedEnclaveOperand(5);
}

TEST_F(X86ModeSgxThreadTest,
       ValidKeyAndReportRequirePlatformStateAfterOperandChecks) {
  for (unsigned Leaf : {0u, 1u, 11u}) {
    startThread();
    privateInstruction(0, Enclu);
    llvm::support::endian::write16le(Code.Bytes.data() + 512,
                                     Leaf == 11 ? 4 : 3);
    std::fill_n(Previous.Bytes.begin() + 512, 512, 0x5a);
    bindThread({.SGX256 = Leaf == 11});
    X86ModeExecution Execution(*Memory, Options);
    uint64_t Budget = 1;
    ASSERT_EQ(
        llvm::cantFail(Execution.run(Machine, Program, Budget)).Completions,
        1u);
    auto &R = Machine.Core.Core.Core.Core.Core.Core;
    R.GPR[0] = Leaf;
    R.GPR[3] = Code.Linear + 512;
    R.GPR[1] = Leaf ? Previous.Linear + 512 : Previous.Linear;
    R.GPR[2] = Previous.Linear + 512;
    Budget = 1;
    auto Stop = llvm::cantFail(Execution.run(Machine, Program, Budget));
    EXPECT_EQ(Stop.Status, X86LegacyExecutionStatus::ModeUnavailable);
    EXPECT_NE(Stop.OwnerError.find("platform key state"), std::string::npos);
    EXPECT_EQ(Stop.Attempts, 1u);
    EXPECT_EQ(Stop.Completions, 0u);
    EXPECT_EQ(R.PC, Code.Linear);
    EXPECT_EQ(R.GPR[0], Leaf);
    ASSERT_TRUE(Memory->sgxMemory()->active());
    std::array<uint8_t, 512> Output{};
    ASSERT_EQ(llvm::cantFail(
                  Memory->load({R.GPR[2], MemoryAccessKind::Read, 3}, Output))
                  .Status,
              X86PageWalkStatus::Completed);
    EXPECT_TRUE(std::all_of(Output.begin(), Output.end(),
                            [](uint8_t B) { return B == 0x5a; }));
  }
}
