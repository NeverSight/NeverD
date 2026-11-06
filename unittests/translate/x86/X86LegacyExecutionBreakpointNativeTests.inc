//===- X86LegacyExecutionBreakpointNativeTests.inc - Original comparators -===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Compare complete protected-mode debug frames, state and RAM with the CPU.
//===----------------------------------------------------------------------===//

class X86LegacyExecutionBreakpointNativeTest : public X86LegacyDebugNativeTest {
};

TEST_P(X86LegacyExecutionBreakpointNativeTest, OriginalComparatorsAndPriority) {
  struct Case {
    X86LegacyDebugNativeTest::Case Instruction;
    unsigned Offset = 0;
    bool FetchFault = false;
    uint32_t CSBase = 0;
    bool VirtualInterrupt = false;
  };
  const Case Cases[] = {
      {Nop},
      {Nop, 1},
      {Increment},
      {Increment, 1},
      {MovSS, 2},
      {ConsecutiveSS, 4},
      {PopSS, 1},
      {InvalidOpcode},
      {OperandFault},
      {Icebp},
      {Icebp, 1},
      {MissingDebugGate},
      {DebugTargetLimit},
      {Nop, 0, true},
      {Nop, 0, false, 0x2000},
      {Nop, 0, false, 0, true},
  };
  for (bool Code32 : {false, true})
    for (bool Stack32 : {false, true})
      for (bool Gate32 : {false, true})
        for (unsigned CPL : {0u, 3u})
          for (unsigned I = 0; I != std::size(Cases); ++I)
            for (unsigned Flags = 0; Flags != 4; ++Flags)
              for (unsigned Comparator = 0; Comparator != 13; ++Comparator) {
                SCOPED_TRACE(testing::Message()
                             << "breakpoint case=" << I << " flags=" << Flags
                             << " comparator=" << Comparator);
                ExecutionBreakpoint B;
                B.DR7 = 0x400 |
                        (Comparator == 12
                             ? 0xffu
                             : (1u + Comparator % 3) << (Comparator / 3 * 2));
                if (Comparator & 1)
                  B.DR7 |= 0x300;
                B.Flags = 0x202 | ((Flags & 1) ? 0x100 : 0) |
                          ((Flags & 2) ? 0x10000 : 0);
                B.Offset = Cases[I].Offset;
                B.FetchFault = Cases[I].FetchFault;
                B.CSBase = Cases[I].CSBase;
                B.VirtualInterrupt = Cases[I].VirtualInterrupt;
                ASSERT_NO_FATAL_FAILURE(compare(
                    Code32, Stack32, Gate32, CPL, Cases[I].Instruction,
                    Comparator & 1 ? 0xffffefff : 0xffff0ff0, false, B));
                if (HasFailure())
                  return;
              }
  RecordProperty("cpu_comparisons", Comparisons);
}

TEST_P(X86LegacyExecutionBreakpointNativeTest, HandlerResumesOriginalOnce) {
  for (bool Code32 : {false, true})
    for (bool Stack32 : {false, true})
      for (unsigned CPL : {0u, 3u})
        for (unsigned Comparator = 0; Comparator != 13; ++Comparator) {
          SCOPED_TRACE(Comparator);
          ExecutionBreakpoint B;
          B.DR7 = 0x400 | (Comparator == 12
                               ? 0xffu
                               : (1u + Comparator % 3) << (Comparator / 3 * 2));
          B.Resume = true;
          ASSERT_NO_FATAL_FAILURE(compare(Code32, Stack32, true, CPL, Increment,
                                          0xffff0ff0, false, B));
          if (HasFailure())
            return;
        }
  RecordProperty("cpu_comparisons", Comparisons);
  RecordProperty("round_trip_comparisons", RoundTrips);
}

INSTANTIATE_TEST_SUITE_P(LegacyExecutionBreakpoint,
                         X86LegacyExecutionBreakpointNativeTest,
                         testing::Values(2u, 3u));
