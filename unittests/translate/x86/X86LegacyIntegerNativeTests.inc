//===- X86LegacyIntegerNativeTests.inc - Original integer extensions -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Counts and independent carry chains in original protected CPU contexts.
//===----------------------------------------------------------------------===//

#include "X86LegacyScalarNativeObserverFixture.inc"

class X86LegacyIntegerNativeTest : public X86LegacyScalarNativeObserverFixture {
protected:
  void SetUp() override {
    X86LegacyScalarNativeObserverFixture::SetUp();
    if (HasFatalFailure() || IsSkipped())
      return;
    X86LegacyIntegerExtensions Features;
    const auto *CPUID =
        reinterpret_cast<const kvm_cpuid2 *>(CPUIDStorage.data());
    for (unsigned I = 0; I != CPUID->nent; ++I) {
      const auto &E = CPUID->entries[I];
      if (E.function == 1)
        Features.POPCNT = E.ecx & (1u << 23);
      if (E.function == 7 && E.index == 0) {
        Features.BMI1 = E.ebx & (1u << 3);
        Features.ADX = E.ebx & (1u << 19);
      }
      if (E.function == 0x80000001)
        Features.LZCNT = E.ecx & (1u << 5);
    }
    if (!Features.POPCNT || !Features.LZCNT || !Features.BMI1 || !Features.ADX)
      GTEST_SKIP() << "Original integer oracle requires POPCNT/LZCNT/BMI1/ADX";
    IntegerExtensions = Features;
  }

  void check(unsigned Operation, bool Code32, unsigned Width, bool Address32,
             unsigned Destination, int Source, uint32_t Value,
             uint32_t Original, uint32_t Flags, unsigned Case = 0,
             unsigned Prefix = 0, unsigned CPL = 3,
             bool SwapMandatory = false) {
    SCOPED_TRACE(testing::Message() << "operation=" << Operation << " width="
                                    << Width << " destination=" << Destination);
    std::vector<uint8_t> Bytes;
    if (Operation < 3 && (Width == 4) != Code32)
      Bytes.push_back(0x66);
    if (Code32 != Address32)
      Bytes.push_back(0x67);
    if (Prefix)
      Bytes.push_back(Prefix);
    if (Operation < 3)
      Bytes.insert(Bytes.end(), {0xf3, 0x0f,
                                 uint8_t(Operation == 0   ? 0xb8
                                         : Operation == 1 ? 0xbc
                                                          : 0xbd)});
    else
      Bytes.insert(Bytes.end(),
                   {uint8_t(Operation == 3 ? 0x66 : 0xf3), 0x0f, 0x38, 0xf6});
    Bytes.push_back((Destination << 3) | (Source >= 0 ? 0xc0 | Source
                                          : Address32 ? 3
                                                      : 7));
    if (SwapMandatory)
      std::swap(Bytes[0], Bytes[1]);
    const uint32_t Offset = Case == 7 || Case == 8 ? 0xc101
                            : Case == 16           ? 0xcfff
                                                   : 0xc100;
    std::array<uint32_t, 8> Registers{
        0x12345678, 0x33445566, 0x22334455, Offset, 0xd100, Offset, 0, Offset};
    Registers[Destination] = Original;
    if (Source >= 0)
      Registers[Source] = Value;
    if (Operation == 1 || Operation == 2) {
      // Only undefined OF/SF/AF/PF come from this independent host probe.
      // The original protected run below never supplies model result/CF/ZF.
      std::vector<uint8_t> Probe{0x9c, 0x89, 0xf8, 0x51, 0x9d};
      if (Width == 2)
        Probe.push_back(0x66);
      Probe.insert(Probe.end(),
                   {0xf3, 0x0f, uint8_t(Operation == 1 ? 0xbc : 0xbd), 0xc6,
                    0x9c, 0x5a, 0x48, 0xc1, 0xe2, 0x20, 0x48, 0x09, 0xd0, 0x9d,
                    0xc3});
      const uint32_t Input = Source >= 0 ? Registers[Source]
                             : Case == 3 && (Prefix == 0x36 || Prefix == 0x2e)
                                 ? 0
                                 : Value;
      uint64_t Observed;
      ASSERT_NO_FATAL_FAILURE(observeCode(Probe, 0, Input, 0, Flags, Observed));
      BitCountUndefinedFlags =
          uint32_t(Observed >> 32) & kX86LegacyBitCountUndefinedFlagsMask;
    }
    ASSERT_NO_FATAL_FAILURE(compare(Bytes, Code32, true, CPL, Case, Value, 0,
                                    Flags, 0x4000, 0, Code, Registers));
  }
};

TEST_P(X86LegacyIntegerNativeTest, CountsWidthsSourcesAndFlagImages) {
  for (bool Code32 : {false, true})
    for (unsigned Width : {2u, 4u})
      for (unsigned Operation = 0; Operation != 3; ++Operation)
        for (uint32_t Value : {0u, 1u, 2u, 0x8000u, 0x10000u, 0x80000000u,
                               0xffffffffu, 0x12345678u})
          for (uint32_t Flags : {0x202u, 0xed7u})
            for (int Source : {0, 1, -1}) {
              ASSERT_NO_FATAL_FAILURE(check(Operation, Code32, Width, true, 0,
                                            Source, Value, 0x87654321, Flags));
              if (HasFailure())
                return;
            }
  RecordProperty("cpu_comparisons", Comparisons);
}

TEST_P(X86LegacyIntegerNativeTest, CarryChainsUnsignedOverflowAndPreservation) {
  for (bool Code32 : {false, true})
    for (unsigned Operation : {3u, 4u})
      for (uint32_t Left : {0u, 1u, 0x7fffffffu, 0x80000000u, 0xffffffffu})
        for (uint32_t Right : {0u, 1u, 0x7fffffffu, 0x80000000u, 0xffffffffu})
          for (uint32_t Flags : {0x202u, 0x203u, 0xa02u, 0xed7u})
            for (int Source : {0, 1, -1}) {
              ASSERT_NO_FATAL_FAILURE(check(Operation, Code32, 4, true, 0,
                                            Source, Right, Left, Flags));
              if (HasFailure())
                return;
            }
  RecordProperty("cpu_comparisons", Comparisons);
}

TEST_P(X86LegacyIntegerNativeTest, AllDestinationsAndSourceRegisters) {
  for (bool Code32 : {false, true})
    for (unsigned Operation = 0; Operation != 5; ++Operation)
      for (unsigned Destination = 0; Destination != 8; ++Destination)
        for (unsigned Source = 0; Source != 8; ++Source) {
          ASSERT_NO_FATAL_FAILURE(check(Operation, Code32, 4, true, Destination,
                                        Source, 0x80000001, 0x7fffffff, 0xed7));
          if (HasFailure())
            return;
        }
  RecordProperty("cpu_comparisons", Comparisons);
}

TEST_P(X86LegacyIntegerNativeTest, ADOXIgnoresWordPrefixInBothOrders) {
  for (bool Code32 : {false, true})
    for (bool Swap : {false, true})
      for (int Source : {0, 1, -1})
        for (uint32_t Flags : {0x203u, 0xa02u, 0xed7u}) {
          ASSERT_NO_FATAL_FAILURE(check(4, Code32, 4, Code32, 0, Source,
                                        0x80000001, 0x80000000, Flags, 0, 0x66,
                                        3, Swap));
          if (HasFailure())
            return;
        }
  RecordProperty("cpu_comparisons", Comparisons);
}

TEST_P(X86LegacyIntegerNativeTest, SegmentsPagesAlignmentAndOriginalRepair) {
  for (bool Code32 : {false, true})
    for (bool Address32 : {false, true})
      for (unsigned CPL : {0u, 3u})
        for (unsigned Operation = 0; Operation != 5; ++Operation)
          for (unsigned Case : {0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 12u, 16u})
            for (unsigned Prefix : {0u, 0x2eu, 0x36u, 0x64u}) {
              ASSERT_NO_FATAL_FAILURE(
                  check(Operation, Code32, Operation < 3 && !Code32 ? 2 : 4,
                        Address32, 0, -1, 0x12345678, 0x87654321,
                        0xed7 | (Case == 7 || Case == 8 ? 0x40000 : 0), Case,
                        Prefix, CPL));
              if (HasFailure())
                return;
            }
  RecordProperty("cpu_comparisons", Comparisons);
}

INSTANTIATE_TEST_SUITE_P(LegacyInteger, X86LegacyIntegerNativeTest,
                         testing::Values(2u, 3u));
