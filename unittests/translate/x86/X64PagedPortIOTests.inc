//===- X64PagedPortIOTests.inc - Device and architectural progress --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Compare original intercepted instructions with an independent device tape.
/// KVM port emulation is reported separately from bare CPU comparisons.
//===----------------------------------------------------------------------===//

#if defined(__linux__) && defined(__x86_64__)
TEST_F(X64PagedExecutionTest, PagedPortTransfersMatchKVM) {
  if (!neverd::test::hostCpuIsIntel())
    GTEST_SKIP() << neverd::test::IntelOracleOnlyReason;
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "No test Clang";
  X64PagedNativeCPU CPU;
  ASSERT_NO_FATAL_FAILURE(CPU.initialize());
  if (testing::Test::IsSkipped())
    return;
  ASSERT_NO_FATAL_FAILURE(CPU.pinForReturnComparison());
  RecordProperty("processor", CPU.ReturnProcessor);
  RecordProperty("core_type", CPU.ReturnCoreType);
  RecordProperty("oracle",
                 "KVM intercepted port emulation and synthetic device");
  Features.PhysicalAddressBits = CPU.PhysicalBits;
  CPU.PortTransfer = PagedPortTape::kvm;
  struct Form {
    bool Input, String, Repeat, Immediate;
    unsigned Width, AddressBytes, Segment, Length;
    uint64_t PC;
  };
  std::vector<Form> Forms;
  std::vector<uint8_t> Code;
  std::vector<uint64_t> Addresses;
  // Scalar DX/immediate plus both single and repeated string directions.
  for (unsigned Kind = 0; Kind != 8; ++Kind)
    for (unsigned Width : {1, 2, 4})
      for (unsigned Address : {4, 8})
        for (unsigned Segment : {0, 0x36, 0x64, 0x65})
          for (bool Rex : {false, true}) {
            const bool Input = !(Kind & 1), String = Kind >= 4;
            if (!String && (Address == 4 || Segment))
              continue;
            if (Code.size() == 0x800)
              Code.resize(0x810, 0x90);
            const unsigned Start = Code.size();
            const uint64_t PC = 0x1000 + Start;
            Addresses.push_back(PC);
            if (Kind >= 6)
              Code.push_back(0xf3);
            if (Segment)
              Code.push_back(Segment);
            if (Address == 4)
              Code.push_back(0x67);
            if (Width == 2)
              Code.push_back(0x66);
            // REX.W overrides 66 but still transfers at most a dword.
            if (Rex)
              Code.push_back(0x48);
            Code.push_back((String      ? 0x6c
                            : Kind >= 2 ? 0xe4
                                        : 0xec) |
                           unsigned(!Input) * 2 | unsigned(Width != 1));
            if (Kind >= 2 && !String)
              Code.push_back(0xf3); // Immediate data must not become REP.
            Forms.push_back({Input, String, Kind >= 6, Kind >= 2 && !String,
                             Width == 2 && Rex ? 4u : Width, Address, Segment,
                             unsigned(Code.size() - Start), PC});
            Code.resize(Start + 16, 0x90);
          }
  std::vector<std::string> Compilers{NEVERD_TEST_CLANG};
  if (const char *Compiler = std::getenv("NEVERD_TEST_NEVERC"))
    Compilers.emplace_back(Compiler);
  unsigned Comparisons = 0, Faults = 0, InputFaults = 0, MissingAC = 0,
           WrongSS = 0;
  for (const auto &Compiler : Compilers)
    for (bool LLVM : {false, true})
      for (const std::string O : {"-O0", "-O2"}) {
        Run Driver = nullptr;
        ASSERT_NO_FATAL_FAILURE(
            compile(Code, LLVM, O, Driver, Addresses, Compiler));
        for (const auto &F : Forms)
          for (unsigned Case = 0; Case != 23; ++Case) {
            SCOPED_TRACE(llvm::formatv("{0} llvm={1} {2} pc={3:x} case={4}",
                                       Compiler, LLVM, O, F.PC, Case)
                             .str());
            reset(Code);
            auto &Core = State.Core.Core.Core.Core;
            Core.PC = F.PC;
            const unsigned CPL = Case == 20   ? 1
                                 : Case >= 21 ? 2
                                 : Case >= 3  ? 3
                                              : 0;
            State.Core.CS = {0, UINT32_MAX, uint16_t(24 | CPL),
                             uint16_t(0xa09b | (CPL << 5))};
            State.Core.SS = {0, UINT32_MAX, uint16_t(32 | CPL),
                             uint16_t(0xc093 | (CPL << 5))};
            State.DS = State.ES = State.Core.SS;
            State.FS = State.GS = {UINT32_MAX, 16, 0xc093};
            State.TR.Limit = 0x3000;
            put(0x8066, 0x100);
            Core.FSBase = 0x1000;
            Core.GSBase = 0x2000;
            const uint64_t Base = F.Segment == 0x64   ? Core.FSBase
                                  : F.Segment == 0x65 ? Core.GSBase
                                                      : 0;
            const unsigned Count = Case == 2 || Case == 5   ? 0
                                   : Case <= 1 || Case == 3 ? 4
                                                            : 1;
            Core.GPR[1] = Count;
            Core.GPR[2] = 0x12345678000002f9ULL;
            Core.GPR[6] = 0x20000 - Base;
            Core.GPR[7] = 0x22000;
            put(0x13110, 0x32007);
            put(0x13118, 0x33007);
            put(0x8024, 0x1c000);
            for (unsigned V : {8, 12, 13, 14, 17})
              RAM.Memory.front().Bytes[0x6004 + V * 16] = 1;
            if (Case == 1) {
              Core.RFlags |= 0x400;
              Core.GPR[6] += 0x20;
              Core.GPR[7] += 0x20;
            }
            const auto Port = F.Immediate ? 0xf3u : 0x2f9u;
            if (Case == 4 || Case == 5 || Case == 6)
              RAM.Memory.front().Bytes[0x8100 + Port / 8] = 1 << (Port & 7);
            if (Case == 6)
              Core.RFlags |= 0x3000; // IOPL bypasses a denying bitmap.
            if (Case == 7)
              State.TR.Limit = 0x66;
            if (Case == 8)
              put(0x8066, 0xffff);
            if (Case == 9 || Case == 10) {
              put(0x13100, 0);
              put(0x13110, 0);
              if (Case == 10)
                RAM.Memory.front().Bytes[0x8100 + Port / 8] = 0xff;
            }
            if (Case == 11) {
              Core.GPR[6] = 0x20fff - Base;
              Core.GPR[7] = 0x22fff;
              put(0x13108, 0);
              put(0x13118, 0);
            }
            if (Case == 12) {
              Core.GPR[6] = 0x800000000000ULL - Base;
              Core.GPR[7] = 0x800000000000ULL;
            }
            if (Case == 13) {
              put(0x13100, 0x30003);
              put(0x13110, 0x32003);
            }
            if (Case == 14) {
              Core.RFlags |= 0x40000;
              Controls.CR0 |= 0x40000;
              ++Core.GPR[6];
              ++Core.GPR[7];
            }
            if (Case == 15)
              Core.RFlags |= 0x10000;
            if (Case == 16)
              Core.RFlags |= 0x4000;
            if (Case == 17)
              put(0x13008, 0); // Fetch precedes permissions and device input.
            if (Case == 18) {
              Core.GPR[2] = 0xffff;
              RAM.Memory.front().Bytes[0x8100 + 8192] = 0xff;
            }
            if (Case == 19) {
              Core.GPR[2] = 0x2ff;
              RAM.Memory.front().Bytes[0x8100 + 0x2ff / 8 + 1] = 2;
            }
            if (Case == 21 || Case == 22) {
              Core.RFlags |= (Case - 20) << 12;
              RAM.Memory.front().Bytes[0x8100 + Port / 8] = 0xff;
            }
            if (F.AddressBytes == 4) {
              Core.GPR[1] |= 0xaabbccdd00000000ULL;
              Core.GPR[6] = uint32_t(Core.GPR[6]) | 0x1234567800000000ULL;
              Core.GPR[7] = uint32_t(Core.GPR[7]) | 0x8765432100000000ULL;
            }
            PagedPortTape NativeTape, Tape;
            CPU.PortContext = &NativeTape;
            X64ContinuationStateV6 Native;
            bool Handler = false;
            ASSERT_NO_FATAL_FAILURE(CPU.execute(
                RAM.Memory.front().Bytes, State, Controls.CR0, F.Length, Native,
                Handler, false, nullptr, Controls.CR4, true, false));
            Memory =
                llvm::cantFail(X86PagedMemory::create(RAM, Features, Controls));
            X64PagedExecution Context(
                *Memory,
                {.AlignmentPriority = X64PagedAlignmentPriority::BeforePaging,
                 .PortIO = X64PagedPortIOBinding{
                     X64PagedPortIOProfile::InputBeforeWriteFragments,
                     PagedPortTape::transfer, &Tape}});
            const auto EntryState = State;
            const bool ACGap = Case == 14 && F.String && F.Width != 1;
            const bool SSGap = Case == 12 && F.String && !F.Input &&
                               F.Segment == 0x36 && F.AddressBytes == 8;
            uint64_t Budget = F.Repeat ? std::max(Count, 1u) : 1;
            EXPECT_EQ(
                Driver(&State, reinterpret_cast<uint8_t *>(&Context), &Budget),
                (Handler || ACGap) ? 7u : 9u);
            EXPECT_TRUE(Context.lastError().empty()) << Context.lastError();
            if (ACGap) {
              // KVM's port emulator omits alignment checking. Preserve the
              // ISA's #AC and record this oracle limitation separately.
              EXPECT_FALSE(Handler);
              ASSERT_TRUE(Context.lastResult().Exception.has_value());
              EXPECT_EQ(Context.lastResult().Exception->Vector, 17u);
              EXPECT_EQ(Core.PC, 0x4110u);
              EXPECT_EQ(Core.GPR[1], EntryState.Core.Core.Core.Core.GPR[1]);
              EXPECT_EQ(Core.GPR[6], EntryState.Core.Core.Core.Core.GPR[6]);
              EXPECT_EQ(Core.GPR[7], EntryState.Core.Core.Core.Core.GPR[7]);
              EXPECT_EQ(Tape.Transfers.size(), unsigned(F.Input));
              EXPECT_EQ(Context.lastResult().DeviceTransferCompleted, F.Input);
              ++MissingAC;
            } else if (SSGap) {
              // KVM incorrectly applies SS to an implicit RSI reference.
              // Intel SDM Vol. 1, 3.3.7.1 ignores that override, including its
              // fault class. Assert the discrepancy before comparing the
              // otherwise identical exception state and frame.
              ASSERT_TRUE(Handler);
              EXPECT_EQ(Native.Core.Core.Core.Core.PC, 0x40c0u);
              ASSERT_TRUE(Context.lastResult().Exception.has_value());
              EXPECT_EQ(Context.lastResult().Exception->Vector, 13u);
              EXPECT_EQ(Core.PC, 0x40d0u);
              EXPECT_TRUE(Tape.Transfers.empty());
              EXPECT_TRUE(NativeTape.Transfers.empty());
              EXPECT_FALSE(Context.lastResult().DeviceTransferCompleted);
              Native.Core.Core.Core.Core.PC = 0x40d0;
              expectPagedSegmentCPU(State, Native, *Memory, CPU);
              ++WrongSS;
            } else {
              expectPagedSegmentCPU(State, Native, *Memory, CPU);
              EXPECT_EQ(Tape.Transfers, NativeTape.Transfers);
              EXPECT_EQ(Context.lastResult().DeviceTransferCompleted,
                        !Tape.Transfers.empty());
              ++Comparisons;
              Faults += Handler;
              InputFaults += Handler && !Tape.Transfers.empty() && F.Input;
            }
            if (HasFailure())
              return;
          }
      }
  RecordProperty("kvm_port_missing_alignment_checks", MissingAC);
  RecordProperty("kvm_port_wrong_ss_faults", WrongSS);
  RecordProperty("kvm_port_comparisons", Comparisons);
  RecordProperty("kvm_port_fault_comparisons", Faults);
  RecordProperty("kvm_port_input_fault_effects", InputFaults);
}
#endif
