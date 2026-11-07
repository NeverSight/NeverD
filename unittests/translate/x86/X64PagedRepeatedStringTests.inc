//===- X64PagedRepeatedStringTests.inc - Restartable REP elements ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Compare completed and faulted string progress with the original CPU.
//===----------------------------------------------------------------------===//

#if defined(__linux__) && defined(__x86_64__)
TEST_F(X64PagedExecutionTest, PagedRepeatedTransfersMatchOriginalCPU) {
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
  Features.PhysicalAddressBits = CPU.PhysicalBits;
  struct Form {
    unsigned Kind, Width, AddressBytes, Segment, Length;
    uint64_t PC;
  };
  std::vector<Form> Forms;
  std::vector<uint8_t> Code;
  std::vector<uint64_t> Addresses;
  for (unsigned Kind = 0; Kind != 7; ++Kind)
    for (unsigned Width : {1, 2, 4, 8})
      for (unsigned Address : {4, 8})
        for (unsigned Segment : {0, 0x64, 0x65}) {
          // The shared native oracle reserves 0x1800/0x1801 for return stops.
          if (Code.size() == 0x800)
            Code.resize(0x810, 0x90);
          const unsigned Start = Code.size();
          Addresses.push_back(0x1000 + Start);
          Code.push_back(Kind == 4 || Kind == 6 ? 0xf2 : 0xf3);
          if (Segment)
            Code.push_back(Segment);
          if (Address == 4)
            Code.push_back(0x67);
          if (Width == 2)
            Code.push_back(0x66);
          if (Width == 8)
            Code.push_back(0x48);
          Code.push_back((Kind == 0   ? 0xa4
                          : Kind == 1 ? 0xaa
                          : Kind == 2 ? 0xac
                          : Kind < 5  ? 0xa6
                                      : 0xae) |
                         unsigned(Width != 1));
          Forms.push_back({Kind, Width, Address, Segment,
                           unsigned(Code.size() - Start), 0x1000 + Start});
          Code.resize(Start + 16, 0x90);
        }
  std::vector<std::string> Compilers{NEVERD_TEST_CLANG};
  if (const char *Compiler = std::getenv("NEVERD_TEST_NEVERC"))
    Compilers.emplace_back(Compiler);
  unsigned Comparisons = 0, Faults = 0, PartialFaults = 0;
  for (const auto &Compiler : Compilers)
    for (bool LLVM : {false, true})
      for (const std::string O : {"-O0", "-O2"}) {
        Run Driver = nullptr;
        ASSERT_NO_FATAL_FAILURE(
            compile(Code, LLVM, O, Driver, Addresses, Compiler));
        for (const auto &F : Forms)
          for (unsigned Case = 0; Case != 33; ++Case) {
            SCOPED_TRACE(llvm::formatv("{0} llvm={1} {2} pc={3:x} case={4}",
                                       Compiler, LLVM, O, F.PC, Case)
                             .str());
            reset(Code);
            auto &Core = State.Core.Core.Core.Core;
            Core.PC = F.PC;
            const unsigned CPL =
                Case < 8 ? Case & 3
                : Case == 15 || Case == 16 || Case == 21 || Case == 22 ? 3
                                                                       : 0;
            State.Core.CS = {0, UINT32_MAX, uint16_t(24 | CPL),
                             uint16_t(0xa09b | (CPL << 5))};
            State.Core.SS = {0, UINT32_MAX, uint16_t(32 | CPL),
                             uint16_t(0xc093 | (CPL << 5))};
            State.DS = State.ES = State.Core.SS;
            State.FS = State.GS = {UINT32_MAX, 16, 0xc093};
            Core.FSBase = 0x1000;
            Core.GSBase = 0x2000;
            const uint64_t Base = F.Segment == 0x64   ? Core.FSBase
                                  : F.Segment == 0x65 ? Core.GSBase
                                                      : 0;
            put(0x13110, 0x32007);
            put(0x13118, 0x33007);
            for (unsigned I = 0; I != 0x4000; ++I)
              RAM.Memory.front().Bytes[0x30000 + I] =
                  F.Kind >= 3
                      ? ((F.Kind == 4 || F.Kind == 6) && I >= 0x2000 ? 0x22
                                                                     : 0x11)
                      : (I * 37 + 17) & 255;
            if (F.Kind >= 5)
              Core.GPR[0] = 0x1111111111111111ULL;
            if (F.Kind >= 3 && (Case == 30 || Case == 31))
              for (unsigned Byte = 0; Byte != F.Width; ++Byte)
                RAM.Memory.front()
                    .Bytes[0x32000 + (Case == 31 ? 2 * F.Width : 0) + Byte] =
                    F.Kind == 4 || F.Kind == 6 ? 0x11 : 0x22;
            if (Case == 32) {
              Core.RFlags |= 0x8d5;
              put(0x13108, 0);
            }
            if (Case == 28)
              put(0x13008, 0); // Fetch precedes REP entry normalization.
            if (Case == 29) {
              put(0x13100, 0);
              put(0x13110, 0); // Competing source/destination faults.
            }
            put(0x8024, 0x1c000);
            for (unsigned V : {8, 12, 13, 14, 17})
              RAM.Memory.front().Bytes[0x6004 + V * 16] = 1;
            const unsigned Counts[] = {0, 1, 4, 17};
            const uint64_t Count = Case < 8                   ? Counts[Case & 3]
                                   : Case == 8                ? 0
                                   : Case == 23 || Case == 24 ? 8
                                                              : 4;
            uint64_t Source = 0x20000, Destination = 0x22000;
            if (Case >= 4 && Case < 8) {
              Core.RFlags |= 0x400;
              Source += 0x100;
              Destination += 0x100;
            }
            if (Case == 8 || Case == 17)
              Source = 0x800000000000ULL;
            if (Case == 8 || Case == 18)
              Destination = 0x800000000000ULL;
            if (Case == 9 || Case == 22)
              put(0x13100, 0);
            if (Case == 10)
              put(0x13110, 0);
            if (Case == 11 || Case == 32) {
              Source = 0x21000 - 2 * F.Width;
              put(0x13108, 0);
            }
            if (Case == 12 || Case == 13) {
              Destination = 0x23000 - 2 * F.Width;
              put(0x13118, Case == 12 ? 0 : 0x33005);
            }
            if (Case == 14)
              put(0x13100, 0x30005);
            if (Case == 15)
              put(0x13100, 0x30003);
            if (Case == 16)
              put(0x13110, 0x32003);
            if (Case == 19) {
              Source = 0x20fff;
              put(0x13108, 0);
            }
            if (Case == 20) {
              Destination = 0x22fff;
              put(0x13118, 0);
            }
            if (Case == 21 || Case == 22) {
              ++Source;
              ++Destination;
              Controls.CR0 |= 0x40000;
              Core.RFlags |= 0x40000;
            }
            if (Case == 23)
              Destination = Source + 1;
            if (Case == 24) {
              Source += 0x100;
              Destination = Source - 1;
              Core.RFlags |= 0x400;
            }
            if (Case == 25)
              Core.RFlags |= 0x10000;
            if (Case == 26)
              Core.RFlags |= 0x4000;
            if (Case == 27)
              State.NMIBlocked = 1;
            Core.GPR[1] = Count;
            Core.GPR[6] = Source - Base;
            Core.GPR[7] = Destination;
            if (F.AddressBytes == 4) {
              Core.GPR[1] |= 0xaabbccdd00000000ULL;
              Core.GPR[6] = uint32_t(Core.GPR[6]) | 0x1234567800000000ULL;
              Core.GPR[7] = uint32_t(Core.GPR[7]) | 0x8765432100000000ULL;
            }
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
                 .RepeatProfile =
                     CPU.ReturnCoreType == 0x20
                         ? X64PagedRepeatProfile::PreserveLodsCountHigh
                         : X64PagedRepeatProfile::ClearLodsCountHigh});
            const uint64_t Remaining =
                F.AddressBytes == 4
                    ? uint32_t(Native.Core.Core.Core.Core.GPR[1])
                    : Native.Core.Core.Core.Core.GPR[1];
            uint64_t Budget = std::max(Count, uint64_t(1));
            EXPECT_EQ(
                Driver(&State, reinterpret_cast<uint8_t *>(&Context), &Budget),
                Handler     ? 7u
                : Remaining ? 8u
                            : 9u);
            EXPECT_TRUE(Context.lastError().empty()) << Context.lastError();
            expectPagedSegmentCPU(State, Native, *Memory, CPU);
            EXPECT_EQ(Budget, Handler ? Remaining - 1 : Remaining);
            EXPECT_EQ(Context.lastResult().InstructionCompleted, !Handler);
            ++Comparisons;
            Faults += Handler;
            PartialFaults += Handler && Remaining < Count;
            if (HasFailure())
              return;
          }
      }
  RecordProperty("cpu_comparisons", Comparisons);
  RecordProperty("rep_fault_cpu_comparisons", Faults);
  RecordProperty("rep_partial_fault_cpu_comparisons", PartialFaults);
}
#endif
