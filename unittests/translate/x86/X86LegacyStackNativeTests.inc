//===- X86LegacyStackNativeTests.inc - Original protected stack frames ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// PUSHA/POPA/ENTER widths, ordered faults, partial state and handler retry.
//===----------------------------------------------------------------------===//

#include "X86LegacyScalarNativeFixture.inc"

class X86LegacyStackNativeTest : public X86LegacyScalarNativeFixture {
protected:
  static std::vector<uint8_t> encoding(uint8_t Opcode, bool Code32,
                                       unsigned Width, uint16_t Allocation = 0,
                                       uint8_t Nesting = 0) {
    // Neither the explicit address nor segment override changes implicit SS.
    std::vector<uint8_t> Bytes{0x67, 0x64};
    if ((Width == 4) != Code32)
      Bytes.push_back(0x66);
    Bytes.push_back(Opcode);
    if (Opcode == 0xc8)
      Bytes.insert(Bytes.end(),
                   {uint8_t(Allocation), uint8_t(Allocation >> 8), Nesting});
    return Bytes;
  }

  static std::array<uint32_t, 8> inputs(uint32_t SP = 0xd100,
                                        uint32_t BP = 0xc100) {
    return {0x11223344, 0x22334455, 0x33445566, 0x44556677,
            SP,         BP,         0x66778899, 0x778899aa};
  }
};

TEST_P(X86LegacyStackNativeTest, EveryCodeDataStackWidthAndPrivilege) {
  ExtraMemoryChecks = {{0xc080, 128}, {0xd000, 256}};
  for (bool Code32 : {false, true})
    for (bool Stack32 : {false, true})
      for (unsigned Width : {2u, 4u})
        for (unsigned CPL = 0; CPL != 4; ++CPL)
          for (uint32_t Flags : {0x202u, 0xed7u})
            for (uint8_t Opcode : {0x60, 0x61, 0xc8})
              for (unsigned Nesting : {0u, 1u, 2u, 3u, 31u, 32u, 33u, 255u}) {
                if (Opcode != 0xc8 && Nesting)
                  continue;
                ASSERT_NO_FATAL_FAILURE(compare(
                    encoding(Opcode, Code32, Width, 0x80, Nesting), Code32,
                    Stack32, CPL, 0, 0, 0, Flags, 0x4000, 0, Code, inputs()));
              }
  RecordProperty("cpu_comparisons", Comparisons);
}

TEST_P(X86LegacyStackNativeTest, FaultAtEveryPushOrPopSlotRetainsPartialState) {
  ExtraMemoryChecks = {{0xcf80, 256}};
  for (bool Code32 : {false, true})
    for (bool Stack32 : {false, true})
      for (unsigned Width : {2u, 4u})
        for (uint8_t Opcode : {0x60, 0x61})
          for (unsigned Slots = 0; Slots != 8; ++Slots) {
            const uint32_t SP = Opcode == 0x60 ? 0xd000 + Slots * Width
                                               : 0xd000 - Slots * Width;
            ASSERT_NO_FATAL_FAILURE(compare(encoding(Opcode, Code32, Width),
                                            Code32, Stack32, 3,
                                            Opcode == 0x60 ? 1 : 9, 0, 0, 0xed7,
                                            0x4000, 0, Code, inputs(SP)));
          }
  RecordProperty("cpu_comparisons", Comparisons);
}

TEST_P(X86LegacyStackNativeTest, SegmentLimitsAreCheckedAtEachActualAccess) {
  ExtraMemoryChecks = {{0xcf80, 256}};
  for (bool Code32 : {false, true})
    for (bool Stack32 : {false, true})
      for (unsigned Width : {2u, 4u})
        for (uint8_t Opcode : {0x60, 0x61, 0xc8})
          for (unsigned Slots = 0; Slots != 8; ++Slots) {
            const bool Pop = Opcode == 0x61;
            ASSERT_NO_FATAL_FAILURE(
                compare(encoding(Opcode, Code32, Width, Width, 7), Code32,
                        Stack32, 3, 0, 0, 0, 0x202, 0x4000, 0, Code,
                        inputs(Pop ? 0xd000 : 0xd020, 0xd020),
                        [&](kvm_sregs &Special, kvm_regs &) {
                          Special.ss.g = 0;
                          Special.ss.type = Pop ? 3 : 7; // Expand-down writes.
                          Special.ss.limit = Pop ? 0xd000 + Slots * Width - 1
                                                 : 0xd020 - Slots * Width - 1;
                        }));
          }
  RecordProperty("cpu_comparisons", Comparisons);
}

TEST_P(X86LegacyStackNativeTest, WordStackWrapAndNonzeroBaseKeepHighRegisters) {
  ExtraMemoryChecks = {{0x20000, 256}, {0x2ff80, 256}, {0x2c080, 128}};
  for (bool Code32 : {false, true})
    for (unsigned Width : {2u, 4u})
      for (uint8_t Opcode : {0x60, 0x61, 0xc8})
        for (uint32_t SP : {0xabcd0000u, 0xabcd0010u, 0xabcdfffeu})
          for (unsigned Nesting : {0u, 1u, 3u, 31u}) {
            if (Opcode != 0xc8 && Nesting)
              continue;
            ASSERT_NO_FATAL_FAILURE(compare(
                encoding(Opcode, Code32, Width, 0xffff, Nesting), Code32, false,
                3, 0, 0, 0, 0xed7, 0x4000, 0, Code, inputs(SP, 0x9876c100),
                [&](kvm_sregs &Special, kvm_regs &) {
                  Special.ss.base = 0x20000;
                  Special.ss.g = 0;
                  Special.ss.limit = 0xffff;
                }));
          }
  RecordProperty("cpu_comparisons", Comparisons);
}

TEST_P(X86LegacyStackNativeTest, EnterDisplayAliasingAndWordAddressBorrow) {
  ExtraMemoryChecks = {{0xcf80, 256}, {0x1cf80, 256}};
  for (bool Code32 : {false, true})
    for (unsigned Width : {2u, 4u})
      for (uint32_t SP : {0xd100u, 0x1d100u})
        for (uint32_t BP : {SP, SP - Width, SP + Width, 0x10002u})
          for (unsigned Nesting : {2u, 3u, 31u, 255u})
            ASSERT_NO_FATAL_FAILURE(
                compare(encoding(0xc8, Code32, Width, 0, Nesting), Code32, true,
                        3, 0, 0, 0, 0xed7, 0x4000, 0, Code, inputs(SP, BP)));
  RecordProperty("cpu_comparisons", Comparisons);
}

TEST_P(X86LegacyStackNativeTest, EnterFinalProbeAndDisplayFaultsRetainPushes) {
  ExtraMemoryChecks = {{0xd000, 256}, {0xe000, 256}};
  for (bool Code32 : {false, true})
    for (bool Stack32 : {false, true})
      for (unsigned Width : {2u, 4u}) {
        for (unsigned Read = 0; Read != 8; ++Read)
          ASSERT_NO_FATAL_FAILURE(compare(
              encoding(0xc8, Code32, Width, 0, 9), Code32, Stack32, 3, 0, 0, 0,
              0xed7, 0x4000, 0, Code, inputs(0xd100, 0xc000 + Read * Width),
              [&](kvm_sregs &, kvm_regs &) {
                const unsigned PW = GetParam() == 2 ? 4 : 8;
                const uint32_t PT = GetParam() == 2 ? 0x11000 : 0x12000;
                put(PT + 11 * PW, 0xb006, PW);
              }));
        // A cross-page probe covers the complete operand width. The first
        // byte is present, the second page is not; the earlier push is valid.
        ASSERT_NO_FATAL_FAILURE(compare(
            encoding(0xc8, Code32, Width, 0x1101 - Width), Code32, Stack32, 3,
            9, 0, 0, 0x202, 0x4000, 0, Code, inputs(0xe100)));
        // No allocation byte is stored, but its read-only page must fault.
        ASSERT_NO_FATAL_FAILURE(compare(encoding(0xc8, Code32, Width, 0x1000),
                                        Code32, Stack32, 3, 2, 0, 0, 0x202,
                                        0x4000, 0, Code, inputs()));
        for (unsigned Misalign : {0u, 1u, 2u})
          ASSERT_NO_FATAL_FAILURE(compare(
              encoding(0xc8, Code32, Width, Misalign == 2 ? 1 : 0, 3), Code32,
              Stack32, 3, 0, 0, 0, 0x40202, 0x4000, 0, Code,
              inputs(0xd100 + (Misalign == 0), 0xc100 + (Misalign == 1))));
      }
  RecordProperty("cpu_comparisons", Comparisons);
}

TEST_P(X86LegacyStackNativeTest, PageHandlerRepairsAndRetriesPartialFrames) {
  ExtraMemoryChecks = {{0xcf80, 256}};
  for (bool Code32 : {false, true})
    for (bool Stack32 : {false, true})
      for (unsigned Width : {2u, 4u})
        for (uint8_t Opcode : {0x60, 0x61, 0xc8}) {
          const uint32_t SP = Opcode == 0x60   ? 0xd000 + 3 * Width
                              : Opcode == 0x61 ? 0xd000 - 3 * Width
                                               : 0xd100;
          ASSERT_NO_FATAL_FAILURE(compare(encoding(Opcode, Code32, Width, 0, 3),
                                          Code32, Stack32, 3,
                                          Opcode == 0x61 ? 23 : 12, 0, 0, 0xed7,
                                          0x4000, 0, Code, inputs(SP)));
        }
  RecordProperty("cpu_comparisons", Comparisons);
}

INSTANTIATE_TEST_SUITE_P(LegacyStack, X86LegacyStackNativeTest,
                         testing::Values(2u, 3u));
