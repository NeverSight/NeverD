//===- X86LegacyWatchpointNativeTests.inc - Protected data debug ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Original protected-mode data comparisons and MOV/POP-SS delayed causes.
//===----------------------------------------------------------------------===//

TEST_P(X86LegacyDebugNativeTest, OriginalDataWatchpointRangesAndShadows) {
  if (!neverd::test::supportsX86KVMPrefault(KVM))
    GTEST_SKIP() << "Data-watchpoint oracle requires KVM pre-fault memory";
  for (bool Code32 : {false, true})
    for (bool Stack32 : {false, true})
      for (bool Gate32 : {false, true})
        for (unsigned CPL : {0u, 3u})
          for (unsigned Form = 0; Form != 11; ++Form)
            for (unsigned Setup = 0; Setup != 8; ++Setup) {
              SCOPED_TRACE(testing::Message()
                           << "form=" << Form << " setup=" << Setup);
              Watchpoint W;
              const std::vector<std::vector<std::vector<uint8_t>>> Forms{
                  {{0x8b, 0x07}},
                  {{0x89, 0x07}},
                  {{0xff, 0x07}},
                  {{0x8a, 0x07}},
                  {{0x88, 0x07}},
                  {{0x8e, 0x17}, {0x90}},
                  {{0x8e, 0x17}, {0xcd, 64}},
                  {{0x8e, 0x17}, {0xf1}},
                  {{0x8e, 0x17}, {0x0f, 0x0b}},
                  {{0x8e, 0x17}, {0x8e, 0x17}},
                  {{0x8d, 0x07}}};
              W.Instructions = Forms[Form];
              W.Flags =
                  0x202 | (Setup == 5 ? 0x100 : 0) | (Setup == 6 ? 0x10000 : 0);
              const unsigned Slot = Setup % 4;
              const unsigned Len = Setup % 4;
              W.DR7 = 0x400 |
                      ((Setup == 3   ? 0u
                        : Setup == 2 ? 2u
                                     : 1u)
                       << (Slot * 2)) |
                      (((Len << 2) | (Setup == 4 ? 1 : 3)) << (16 + Slot * 4));
              W.DR[Slot] = Setup == 0 ? 0x80ff : Setup == 1 ? 0x8108 : 0x8101;
              if (Setup == 7)
                W.DR[Slot] = 0x7000 + 32; // Implicit SS descriptor read.
              ASSERT_NO_FATAL_FAILURE(compare(Code32, Stack32, Gate32, CPL, Nop,
                                              0xffffafff, false, {}, {}, W));
              if (HasFailure())
                return;
            }
  RecordProperty("original_comparisons", Comparisons);
}

TEST_P(X86LegacyDebugNativeTest, OriginalStackOperandsAndEventTables) {
  if (!neverd::test::supportsX86KVMPrefault(KVM))
    GTEST_SKIP() << "Data-watchpoint oracle requires KVM pre-fault memory";
  for (bool Code32 : {false, true})
    for (bool Stack32 : {false, true})
      for (bool Gate32 : {false, true})
        for (unsigned CPL : {0u, 3u})
          for (Case Which : {PopSS, PopFlagsClear, IretClear, FarReturn,
                             NearCall, NearReturn, FarCall, CallGate,
                             SoftwareInterrupt, InvalidOpcode})
            for (unsigned Target = 0; Target != 6; ++Target) {
              SCOPED_TRACE(Target);
              Watchpoint W;
              W.Flags = 0x302;
              W.DR7 = 0xf0401;
              W.DR[0] = std::array<uint32_t, 6>{
                  0xd000, 0xd004, 0xcffc, 0x6200, 0x13000, 0x5804}[Target];
              ASSERT_NO_FATAL_FAILURE(compare(Code32, Stack32, Gate32, CPL,
                                              Which, 0xffff0ff0, false, {}, {},
                                              W));
              if (HasFailure())
                return;
            }
  RecordProperty("original_comparisons", Comparisons);
}
