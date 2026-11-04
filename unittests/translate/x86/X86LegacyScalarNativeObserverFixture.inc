//===- X86LegacyScalarNativeObserverFixture.inc - Explicit CPU choices ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Independent W^X host probes for target-specific scalar state choices.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_UNITTESTS_TRANSLATE_X86_X86LEGACYSCALARNATIVEOBSERVERFIXTURE_INC
#define NEVERD_UNITTESTS_TRANSLATE_X86_X86LEGACYSCALARNATIVEOBSERVERFIXTURE_INC

#include "X86LegacyScalarNativeFixture.inc"

class X86LegacyScalarNativeObserverFixture
    : public X86LegacyScalarNativeFixture {
protected:
  static constexpr unsigned kObserverBytes = 4096;
  uint8_t *Observer = nullptr;
  std::vector<uint8_t> ObserverCode;

  void SetUp() override {
    X86LegacyScalarNativeFixture::SetUp();
    if (HasFatalFailure() || IsSkipped())
      return;
    CheckOperandPageEffects = true;
    Observer = static_cast<uint8_t *>(mmap(nullptr, kObserverBytes,
                                           PROT_READ | PROT_WRITE,
                                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    ASSERT_NE(Observer, MAP_FAILED);
  }

  void TearDown() override {
    if (Observer && Observer != MAP_FAILED)
      EXPECT_EQ(munmap(Observer, kObserverBytes), 0);
    X86LegacyScalarNativeFixture::TearDown();
  }

  // SysV EDI/ESI/EDX/RCX inputs. Each probe preserves callee-saved registers,
  // restores host flags and packs a low-dword result with high-dword flags.
  // Consumers select only explicit target choices; no original KVM output
  // may initialize the executor's inputs or supply its defined result bits.
  void observeCode(const std::vector<uint8_t> &Bytes, uint32_t Destination,
                   uint32_t Source, unsigned Count, uint32_t Flags,
                   uint64_t &Observed) {
    if (Bytes != ObserverCode) {
      ASSERT_EQ(mprotect(Observer, kObserverBytes, PROT_READ | PROT_WRITE), 0);
      std::copy(Bytes.begin(), Bytes.end(), Observer);
      ASSERT_EQ(mprotect(Observer, kObserverBytes, PROT_READ | PROT_EXEC), 0);
      ObserverCode = Bytes;
    }
    using Probe = uint64_t (*)(uint32_t, uint32_t, unsigned, uint64_t);
    Observed = reinterpret_cast<Probe>(Observer)(
        Destination, Source, Count,
        (Flags & kX86LegacyArithmeticFlagsMask) | 2);
  }
};

#endif // NEVERD_UNITTESTS_TRANSLATE_X86_X86LEGACYSCALARNATIVEOBSERVERFIXTURE_INC
