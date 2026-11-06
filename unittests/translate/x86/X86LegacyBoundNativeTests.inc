//===- X86LegacyBoundNativeTests.inc - Original BOUND witnesses -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Compare independent raw BOUND bytes, RAM and complete exception frames.
//===----------------------------------------------------------------------===//

#include "X86LegacyScalarNativeFixture.inc"

class X86LegacyBoundNativeTest : public X86LegacyScalarNativeFixture {
protected:
  static std::vector<uint8_t> encoding(unsigned Width, bool Code32,
                                       bool Address32, unsigned Register,
                                       unsigned Prefix = 0) {
    std::vector<uint8_t> B;
    if ((Width == 4) != Code32)
      B.push_back(0x66);
    if (Address32 != Code32)
      B.push_back(0x67);
    if (Prefix)
      B.push_back(Prefix);
    B.insert(B.end(), {0x62, uint8_t(Register << 3 | (Address32 ? 3 : 7))});
    return B;
  }
};

TEST_P(X86LegacyBoundNativeTest, SignedExtremesWidthsSegmentsAndPrivileges) {
  for (bool Code32 : {false, true})
    for (bool Address32 : {false, true})
      for (unsigned Width : {2u, 4u})
        for (unsigned CPL = 0; CPL != 4; ++CPL)
          for (unsigned Prefix : {0u, 0x26u, 0x2eu, 0x36u, 0x3eu, 0x64u, 0x65u})
            for (uint32_t Index : {0xffff8000u, 0xfffffffbu, 0xfffffffcu, 0u,
                                   4u, 5u, 0x7fffffffu, 0x80000000u}) {
              const auto Configure = [&](kvm_sregs &, kvm_regs &) {
                put(0xc100, -4, Width);
                put(0xc100 + Width, 4, Width);
              };
              ASSERT_NO_FATAL_FAILURE(
                  compare(encoding(Width, Code32, Address32, 1, Prefix), Code32,
                          true, CPL, 0, 0, Index, Index & 1 ? 0xed7 : 0x202,
                          0x4000, 0, Code, {}, Configure));
            }
  RecordProperty("cpu_comparisons", Comparisons);
}

TEST_P(X86LegacyBoundNativeTest,
       LowerComparisonPrecedesUpperPageAndSegmentFaults) {
  if (!neverd::test::hostCpuIsIntel())
    GTEST_SKIP() << neverd::test::IntelOracleOnlyReason;
  for (bool Code32 : {false, true})
    for (bool Address32 : {false, true})
      for (unsigned Width : {2u, 4u})
        for (unsigned CPL = 0; CPL != 4; ++CPL)
          for (unsigned Case = 0; Case != 7; ++Case)
            for (int32_t Index : {-5, 0, 5}) {
              const uint32_t Offset = Case < 3 ? 0xd000 - Width : 0xc101;
              const std::array<uint32_t, 8> Registers{
                  0x12345678, uint32_t(Index), 0x22334455, Offset,
                  0xe100,     0xc100,          0,          0xc100};
              const auto Configure = [&](kvm_sregs &Special, kvm_regs &) {
                put(Offset, -4, Width);
                put(Offset + Width, 4, Width);
                const unsigned PW = GetParam() == 2 ? 4 : 8;
                const uint32_t PT = GetParam() == 2 ? 0x11000 : 0x12000;
                if (Case == 0 || Case == 2)
                  put(PT + 13 * PW, 0xd006, PW);
                if (Case == 1 || Case == 2) {
                  Special.ds.g = 0;
                  Special.ds.limit = 0xcfff;
                }
                if (Case == 4)
                  put(PT + 12 * PW, 0xc006, PW);
                if (Case == 5)
                  Special.ds.type &= ~2u; // BOUND never writes the pair.
                if (Case == 6)
                  Special.ds.unusable = 1;
              };
              ASSERT_NO_FATAL_FAILURE(
                  compare(encoding(Width, Code32, Address32, 1), Code32, true,
                          CPL, 0, 0, 0, Case >= 3 ? 0x40ed7 : 0xed7, 0x4000, 0,
                          Code, Registers, Configure));
            }
  RecordProperty("cpu_comparisons", Comparisons);
}

TEST_P(X86LegacyBoundNativeTest, UpperFieldAddressWrapAndSeparateFieldLimits) {
  if (!neverd::test::hostCpuIsIntel())
    GTEST_SKIP() << neverd::test::IntelOracleOnlyReason;
  for (bool Code32 : {false, true})
    for (bool Address32 : {false, true})
      for (unsigned Width : {2u, 4u})
        for (unsigned Tail = 1; Tail <= Width * 2; ++Tail)
          for (bool FullLimit : {false, true})
            for (int32_t Index : {-5, 0, 5}) {
              const uint32_t Offset = (Address32 ? 0u : 0x10000u) - Tail;
              const std::array<uint32_t, 8> Registers{
                  0x12345678, uint32_t(Index),
                  0x22334455, (Address32 ? Offset : 0xabcd0000u | Offset),
                  0xe100,     0xc100,
                  0,          0xc100};
              const auto Configure = [&](kvm_sregs &Special, kvm_regs &) {
                // Move the segment away from the fixture's page directory.
                Special.fs.base = 0x20000;
                Special.fs.limit = FullLimit ? UINT32_MAX : 0xffff;
                Special.fs.g = FullLimit;
                put(uint32_t(0x20000 + Offset), -4, Width);
                put(uint32_t(0x20000 + Offset + Width), 4, Width);
                put(0x20000, -100,
                    Width); // Makes an erroneous no-wrap observable.
              };
              ASSERT_NO_FATAL_FAILURE(compare(
                  encoding(Width, Code32, Address32, 1, 0x64), Code32, true, 3,
                  0, 0, 0, 0xed7, 0x4000, 0, Code, Registers, Configure));
            }
  RecordProperty("cpu_comparisons", Comparisons);
}

TEST_P(X86LegacyBoundNativeTest, OriginalBRAndUpperPageHandlersReturnAndRetry) {
  for (bool Code32 : {false, true})
    for (bool Address32 : {false, true})
      for (unsigned Width : {2u, 4u})
        for (unsigned CPL = 0; CPL != 4; ++CPL)
          for (bool Page : {false, true}) {
            const uint32_t Offset = 0xd000 - Width;
            const std::array<uint32_t, 8> Registers{
                0x12345678, Page ? 0u : uint32_t(-5),
                0x22334455, Offset,
                0xe100,     0xc100,
                0,          0xc100};
            const auto Configure = [&](kvm_sregs &, kvm_regs &) {
              put(Offset, -4, Width);
              put(Offset + Width, 4, Width);
            };
            ASSERT_NO_FATAL_FAILURE(
                compare(encoding(Width, Code32, Address32, 1), Code32, true,
                        CPL, Page ? 23 : 22, 0, 0, 0xed7, 0x4000, 0, Code,
                        Registers, Configure));
          }
  RecordProperty("cpu_comparisons", Comparisons);
}

INSTANTIATE_TEST_SUITE_P(LegacyBound, X86LegacyBoundNativeTest,
                         testing::Values(2u, 3u));
