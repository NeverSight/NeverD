//===- X86LegacyFlagsNativeTests.inc - Original protected stack flags ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Independent PUSHF/POPF results, privilege restrictions and restart frames.
//===----------------------------------------------------------------------===//

#include "X86LegacyScalarNativeFixture.inc"

class X86LegacyFlagsNativeTest : public X86LegacyScalarNativeFixture {};

TEST_P(X86LegacyFlagsNativeTest, PopEveryFlagAndPrivilegeCombination) {
  for (bool Code32 : {false, true})
    for (bool Stack32 : {false, true})
      for (unsigned Width : {2u, 4u})
        for (unsigned CPL = 0; CPL != 4; ++CPL)
          for (unsigned IOPL = 0; IOPL != 4; ++IOPL)
            for (uint32_t Initial : {2u, 0x3d4ed7u})
              for (unsigned Bit = 0; Bit != 34; ++Bit) {
                std::vector<uint8_t> Bytes{0x67, 0x65};
                if (Code32 != (Width == 4))
                  Bytes.push_back(0x66);
                Bytes.push_back(0x9d);
                const uint32_t Image = Bit == 32   ? 0
                                       : Bit == 33 ? UINT32_MAX
                                                   : 1u << Bit;
                ASSERT_NO_FATAL_FAILURE(
                    compare(Bytes, Code32, Stack32, CPL, 0, Image, 0x87654321,
                            (Initial & ~0x3000u) | (IOPL << 12)));
              }
  RecordProperty("cpu_comparisons", Comparisons);
}

TEST_P(X86LegacyFlagsNativeTest, PushEveryDefinedFlagAndIgnoredPrefix) {
  for (bool Code32 : {false, true})
    for (bool Stack32 : {false, true})
      for (unsigned Width : {2u, 4u})
        for (unsigned CPL = 0; CPL != 4; ++CPL)
          for (uint8_t Segment : {0u, 0x26u, 0x2eu, 0x36u, 0x3eu, 0x64u, 0x65u})
            for (uint32_t Image : {2u, 0xed7u, 0x3002u, 0x10002u, 0x40002u,
                                   0x80002u, 0x100002u, 0x200002u, 0x3d7ed7u}) {
              std::vector<uint8_t> Bytes;
              if (Segment)
                Bytes = {0x67, Segment};
              if (Code32 != (Width == 4))
                Bytes.push_back(0x66);
              Bytes.push_back(0x9c);
              ASSERT_NO_FATAL_FAILURE(compare(Bytes, Code32, Stack32, CPL, 0,
                                              0x12345678, 0x87654321, Image));
            }
  RecordProperty("cpu_comparisons", Comparisons);
}

TEST_P(X86LegacyFlagsNativeTest, NonflatStacksWrapAndRetainExactAccessWidth) {
  for (bool Code32 : {false, true})
    for (bool Stack32 : {false, true})
      for (unsigned Width : {2u, 4u})
        for (uint8_t Opcode : {0x9c, 0x9d})
          for (unsigned CPL = 0; CPL != 4; ++CPL)
            for (uint32_t SP : {0u, 2u, 0xfffcu, 0xfffeu, 0xd100u}) {
              std::vector<uint8_t> Bytes{0x67, 0x64};
              if (Code32 != (Width == 4))
                Bytes.push_back(0x66);
              Bytes.push_back(Opcode);
              const auto Configure = [&](kvm_sregs &Special, kvm_regs &Regs) {
                Special.ss.base = 0x20000;
                Regs.rsp = SP | (Stack32 ? 0 : 0xabcd0000u);
                put(0x20000 + SP, 0xfffffeff, 4);
              };
              ASSERT_NO_FATAL_FAILURE(compare(Bytes, Code32, Stack32, CPL, 0, 0,
                                              0, 0x3d7ed7, 0x4000, 0x22334455,
                                              Code, {}, Configure));
            }
  RecordProperty("cpu_comparisons", Comparisons);
}

TEST_P(X86LegacyFlagsNativeTest, StackFaultsUseOldFlagsAndCanRetry) {
  for (bool Code32 : {false, true})
    for (bool Stack32 : {false, true})
      for (unsigned Width : {2u, 4u})
        for (uint8_t Opcode : {0x9c, 0x9d})
          for (unsigned CPL = 0; CPL != 4; ++CPL)
            for (unsigned Failure = 0; Failure != 8; ++Failure) {
              std::vector<uint8_t> Bytes;
              if (Code32 != (Width == 4))
                Bytes.push_back(0x66);
              Bytes.push_back(Opcode);
              const auto Configure = [&](kvm_sregs &Special, kvm_regs &Regs) {
                const unsigned PW = GetParam() == 2 ? 4 : 8;
                const uint32_t PT = GetParam() == 2 ? 0x11000 : 0x12000;
                if (Failure == 0 || Failure == 1) {
                  // POPF cannot newly enable #AC for its own operand, and
                  // cannot clear old #AC early to avoid an unaligned access.
                  Regs.rsp += 1;
                  put(0xd101, Failure == 0 ? 0x40002 : 2, 4);
                } else if (Failure == 2) {
                  put(PT + 13 * PW, 0xd005, PW);
                } else if (Failure == 3 || Failure == 4) {
                  Regs.rsp = 0xcfff + (Opcode == 0x9c ? Width : 0);
                  put(0xcfff, 0x240002, 4);
                  put(PT + 13 * PW, Failure == 3 ? 0xd006 : 0xd005, PW);
                } else if (Failure == 5) {
                  Special.ss.g = 0;
                  Special.ss.limit = Opcode == 0x9c ? 0xd0fe : 0xd101;
                } else if (Failure == 6) {
                  Special.ss.g = 0;
                  Special.ss.type = 7; // Expand-down stack.
                  Special.ss.limit = 0xd0fe;
                }
              };
              ASSERT_NO_FATAL_FAILURE(
                  compare(Bytes, Code32, Stack32, CPL, Failure == 7 ? 21 : 0,
                          0x240002, 0x87654321, Failure == 1 ? 0x40202 : 0x202,
                          0x4000, 0x22334455, Code, {}, Configure));
            }
  RecordProperty("cpu_comparisons", Comparisons);
}

TEST_P(X86LegacyFlagsNativeTest, PVIDoesNotRedirectPOPFToVirtualIF) {
  for (bool Code32 : {false, true})
    for (unsigned Width : {2u, 4u})
      for (uint8_t Opcode : {0x9c, 0x9d})
        for (unsigned IOPL = 0; IOPL != 4; ++IOPL)
          for (uint32_t Initial : {2u, 0x202u, 0x80002u, 0x100002u})
            for (uint32_t Image : {0u, 0x202u, 0x180202u, 0xfffffeffu}) {
              std::vector<uint8_t> Bytes;
              if (Code32 != (Width == 4))
                Bytes.push_back(0x66);
              Bytes.push_back(Opcode);
              ASSERT_NO_FATAL_FAILURE(compare(
                  Bytes, Code32, true, 3, 0, Image, 0, Initial | (IOPL << 12),
                  0x4000, 0, Code, {},
                  [](kvm_sregs &Special, kvm_regs &) { Special.cr4 |= 2; }));
            }
  RecordProperty("cpu_comparisons", Comparisons);
}

INSTANTIATE_TEST_SUITE_P(LegacyFlags, X86LegacyFlagsNativeTest,
                         testing::Values(2u, 3u));
