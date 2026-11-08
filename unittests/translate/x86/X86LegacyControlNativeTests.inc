//===- X86LegacyControlNativeTests.inc - Original protected MOV-CR --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Independent privileged instructions with full control/register outcomes.
//===----------------------------------------------------------------------===//

#include "X86LegacyScalarNativeFixture.inc"

class X86LegacyControlNativeTest : public X86LegacyScalarNativeFixture {
protected:
  void SetUp() override {
    X86LegacyScalarNativeFixture::SetUp();
#ifdef KVM_GET_SREGS2
    if (GetParam() == 3 && ioctl(KVM, KVM_CHECK_EXTENSION, KVM_CAP_SREGS2) <= 0)
      GTEST_SKIP() << "PAE control qualification requires KVM_GET_SREGS2";
#else
    if (GetParam() == 3)
      GTEST_SKIP()
          << "PAE control qualification requires KVM_GET_SREGS2 headers";
#endif
    ControlRegisters = X86LegacyControlRegisterBinding{0x88};
  }

  void check(unsigned CR, bool Write, uint32_t Value, unsigned GPR = 6,
             unsigned Mod = 3, bool Code32 = true, bool Overrides = false,
             unsigned CPL = 0, uint8_t Prefix = 0,
             std::function<void(kvm_sregs &, kvm_regs &)> Configure = {}) {
    std::array<uint32_t, 8> Registers{
        0x89abcdef, 0x10203040, 0x55667788, 0xc100, 0xd100, 0xc100, 0, 0xc100};
    Registers[GPR] = Value;
    ASSERT_NO_FATAL_FAILURE(
        compare(neverd::test::legacyControlBytes(CR, Write, GPR, Mod, Overrides,
                                                 Prefix),
                Code32, true, CPL, 0, 0x12345678, 0x80, 0xed7, 0x4000, 0, Code,
                Registers, Configure));
  }
};

TEST_P(X86LegacyControlNativeTest, WidthsRegistersIgnoredModAndPrefixes) {
  for (bool Code32 : {false, true})
    for (bool Overrides : {false, true})
      for (bool Write : {false, true})
        for (unsigned CR : {0u, 2u, 3u, 4u})
          for (unsigned GPR = 0; GPR != 8; ++GPR)
            for (unsigned Mod = 0; Mod != 4; ++Mod) {
              const uint32_t Value = CR == 0           ? 0x80050019
                                     : CR == 2         ? 0xdeadbeef
                                     : CR == 3         ? 0x10000
                                     : GetParam() == 2 ? 0x88
                                                       : 0xa8;
              ASSERT_NO_FATAL_FAILURE(
                  check(CR, Write, Value, GPR, Mod, Code32, Overrides));
              if (HasFailure())
                return;
            }
  RecordProperty("cpu_comparisons", Comparisons);
}

TEST_P(X86LegacyControlNativeTest, PrivilegeLockAndUndefinedRegisterPriority) {
  for (bool Write : {false, true})
    for (unsigned CR = 0; CR != 8; ++CR)
      for (unsigned CPL = 0; CPL != 4; ++CPL)
        for (bool Locked : {false, true}) {
          if (CPL == 0 && !Locked && (CR == 0 || CR == 2 || CR == 3 || CR == 4))
            continue;
          ASSERT_NO_FATAL_FAILURE(check(CR, Write, 0xffffffff, 6, 0, true, true,
                                        CPL, Locked ? 0xf0 : 0));
          if (HasFailure())
            return;
        }
  RecordProperty("cpu_comparisons", Comparisons);
}

TEST_P(X86LegacyControlNativeTest, RepeatAndSegmentPrefixesRemainRegisterOnly) {
  for (uint8_t Prefix : {0xf2, 0xf3, 0x26, 0x2e, 0x36, 0x3e, 0x64, 0x65})
    for (bool Code32 : {false, true})
      for (bool Write : {false, true})
        for (unsigned CR : {0u, 2u, 3u, 4u}) {
          const uint32_t Value = CR == 0           ? 0x80050011
                                 : CR == 2         ? 0xdeadbeef
                                 : CR == 3         ? 0x10000
                                 : GetParam() == 2 ? 0
                                                   : 0x20;
          ASSERT_NO_FATAL_FAILURE(
              check(CR, Write, Value, 6, 0, Code32, false, 0, Prefix));
          if (HasFailure())
            return;
        }
  RecordProperty("cpu_comparisons", Comparisons);
}

TEST_P(X86LegacyControlNativeTest, CR0FixedIgnoredAndInvalidBits) {
  for (unsigned Bit = 0; Bit != 32; ++Bit)
    for (bool Set : {false, true}) {
      const uint32_t Value =
          Set ? 0x80050011 | (1u << Bit) : 0x80050011 & ~(1u << Bit);
      ASSERT_NO_FATAL_FAILURE(check(0, true, Value));
      if (HasFailure())
        return;
    }
  for (uint32_t Value : {0xc0050011u, 0xe0050011u, 0xdffaffffu}) {
    ASSERT_NO_FATAL_FAILURE(check(0, true, Value));
    if (HasFailure())
      return;
  }
  RecordProperty("cpu_comparisons", Comparisons);
}

TEST_P(X86LegacyControlNativeTest, CR3RebindAndPAEPointerValidation) {
  for (unsigned Low = 0; Low != 32; ++Low) {
    ASSERT_NO_FATAL_FAILURE(check(3, true, 0x13000 | Low, 6, 3, true, false, 0,
                                  0, [&](kvm_sregs &, kvm_regs &) {
                                    put(0x13000,
                                        GetParam() == 2 ? 0x11007 : 0x11001,
                                        GetParam() == 2 ? 4 : 8);
                                  }));
    if (HasFailure())
      return;
  }
  if (GetParam() == 3)
    for (unsigned Entry = 0; Entry != 4; ++Entry)
      for (uint64_t Reserved : {2ULL, 4ULL, 0x20ULL, 0x100ULL, 1ULL << 63}) {
        ASSERT_NO_FATAL_FAILURE(check(3, true, 0x13000, 6, 3, true, false, 0, 0,
                                      [&](kvm_sregs &, kvm_regs &) {
                                        put(0x13000, 0x11001);
                                        put(0x13000 + Entry * 8, Reserved | 1);
                                      }));
        if (HasFailure())
          return;
      }
  RecordProperty("cpu_comparisons", Comparisons);
}

TEST_P(X86LegacyControlNativeTest, CR4ChangesAndReservedBitFaults) {
  const uint32_t PAE = GetParam() == 2 ? 0 : 0x20;
  for (uint32_t Value : {PAE, PAE | 8, PAE | 0x10, PAE | 0x80, PAE | 0x80000000,
                         PAE | 0x20000}) {
    ASSERT_NO_FATAL_FAILURE(check(4, true, Value));
    if (HasFailure())
      return;
  }
  RecordProperty("cpu_comparisons", Comparisons);
}

INSTANTIATE_TEST_SUITE_P(LegacyControl, X86LegacyControlNativeTest,
                         testing::Values(2, 3));
