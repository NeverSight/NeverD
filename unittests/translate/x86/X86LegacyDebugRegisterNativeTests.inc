//===- X86LegacyDebugRegisterNativeTests.inc - Protected MOV-DR -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Independent original code16/code32 debug-register and handler sequences.
//===----------------------------------------------------------------------===//

class X86LegacyDebugRegisterNativeTest : public X86LegacyDebugNativeTest {};

TEST_P(X86LegacyDebugRegisterNativeTest, OriginalRegisterWidthsAndAliases) {
  for (bool Code32 : {false, true})
    for (bool Stack32 : {false, true})
      for (bool Gate32 : {false, true})
        for (bool Write : {false, true})
          for (unsigned Debug = 0; Debug != 8; ++Debug)
            for (unsigned General = 0; General != 8; ++General) {
              if (!Write && General == 4 && Debug >= 4)
                continue; // Covered without handler stack use in software.
              for (uint8_t Mod : {0x00, 0x40, 0x80, 0xc0}) {
                RegisterAccess A;
                A.Bytes = {0x0f, uint8_t(Write ? 0x23 : 0x21),
                           uint8_t(Mod | (Debug << 3) | General)};
                if ((General + Debug) & 1)
                  A.Bytes.insert(A.Bytes.begin(), 0x66);
                A.General = General;
                A.Value = General == 4 ? 0xd000 : 0x2305;
                A.Flags = (General & 1) ? 0xed7 : 0x202;
                ASSERT_NO_FATAL_FAILURE(compare(Code32, Stack32, Gate32, 0, Nop,
                                                0xffffafff, false, std::nullopt,
                                                A));
                if (HasFailure())
                  return;
              }
            }
  RecordProperty("cpu_comparisons", Comparisons);
}

TEST_P(X86LegacyDebugRegisterNativeTest, OriginalGeneralDetectAndPriority) {
  for (bool Code32 : {false, true})
    for (bool Stack32 : {false, true})
      for (bool Gate32 : {false, true})
        for (unsigned CPL : {0u, 1u, 2u, 3u})
          for (uint8_t Debug : {0, 4, 5, 6, 7})
            for (uint8_t Prefix : {0, 0x66, 0x67, 0xf0, 0xf2, 0xf3})
              for (unsigned Setup = 0; Setup != 8; ++Setup) {
                SCOPED_TRACE(testing::Message()
                             << "debug=" << unsigned(Debug) << " prefix="
                             << unsigned(Prefix) << " setup=" << Setup);
                RegisterAccess A;
                A.Bytes = {0x0f, uint8_t(Setup & 1 ? 0x23 : 0x21),
                           uint8_t(0xc0 | (Debug << 3))};
                if (Prefix)
                  A.Bytes.insert(A.Bytes.begin(), Prefix);
                A.DR7 = 0x2400;
                A.Flags |= (Setup & 1 ? 0x100 : 0) | (Setup & 2 ? 0x10000 : 0);
                A.DE = Setup == 4;
                A.SS = Setup == 5;
                A.FetchFault = Setup == 6;
                A.Resume = Setup == 7 && !CPL && !Prefix && Debug < 4;
                if (A.Resume)
                  A.Flags &= ~0x100u;
                ASSERT_NO_FATAL_FAILURE(
                    compare(Code32, Stack32, Gate32, CPL, Nop,
                            Setup & 1 ? 0xffffafff : 0xffff0fff, false,
                            std::nullopt, A));
                if (HasFailure())
                  return;
              }
  RecordProperty("cpu_comparisons", Comparisons);
  RecordProperty("round_trip_comparisons", RoundTrips);
}

TEST_P(X86LegacyDebugRegisterNativeTest, OriginalOtherDebugCausesRetireGD) {
  for (bool Code32 : {false, true})
    for (bool Stack32 : {false, true})
      for (bool Gate32 : {false, true})
        for (unsigned CPL : {0u, 1u, 2u, 3u})
          for (unsigned Flags = 0; Flags != 4; ++Flags)
            for (const auto &Bytes :
                 std::vector<std::vector<uint8_t>>{{0x90}, {0xf1}, {0xcd, 1}}) {
              RegisterAccess A;
              A.Bytes = Bytes;
              A.DR7 = 0x2400;
              A.Flags |= (Flags & 1 ? 0x100 : 0) | (Flags & 2 ? 0x10000 : 0);
              ASSERT_NO_FATAL_FAILURE(compare(Code32, Stack32, Gate32, CPL, Nop,
                                              0xffffafff, false, std::nullopt,
                                              A));
              if (HasFailure())
                return;
            }
  RecordProperty("cpu_comparisons", Comparisons);
}

INSTANTIATE_TEST_SUITE_P(LegacyDebugRegister, X86LegacyDebugRegisterNativeTest,
                         testing::Values(2, 3));
