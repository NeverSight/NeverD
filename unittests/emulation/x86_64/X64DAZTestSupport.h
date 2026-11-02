//===- X64DAZTestSupport.h - Independent denormal input expectations -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_UNITTESTS_EMULATION_X86_64_X64DAZTESTSUPPORT_H
#define NEVERD_UNITTESTS_EMULATION_X86_64_X64DAZTESTSUPPORT_H

#include "X64VectorTestSupport.h"

#include "llvm/ADT/APFloat.h"

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#endif

namespace neverd::emulation::daz_test {
#define NEVERD_DAZ_VALUE(Name, Value) inline constexpr uint64_t Name = Value;
#define NEVERD_DAZ_TEXT(Name, Value) inline constexpr char Name[] = Value;
#include "X64DAZCases.def"
#undef NEVERD_DAZ_TEXT
#undef NEVERD_DAZ_VALUE

inline constexpr vector_test::Parameter Parameters[] = {
#define NEVERD_DAZ_BACKEND(Name, Backend, User)                                \
  {#Name, ExecutionBackendKind::Backend, User},
#include "X64DAZCases.def"
#undef NEVERD_DAZ_BACKEND
};

// Normalize the mathematical input before APFloat evaluates the operation.
// The guest's source bits remain untouched and are checked separately.
inline llvm::APFloat operand(const llvm::fltSemantics &Format,
                             const llvm::APInt &Bits, uint64_t Control) {
  llvm::APFloat Value(Format, Bits);
  if ((Control & DenormalsAreZero) && Value.isDenormal())
    return llvm::APFloat::getZero(Format, Value.isNegative());
  return Value;
}

inline uint32_t hostMXCSRMask() {
#if defined(__x86_64__) || defined(_M_X64)
  alignas(HostFXAlignment) std::array<uint8_t, HostFXBytes> State{};
  _fxsave64(State.data());
  const auto Mask =
      llvm::support::endian::read32le(State.data() + HostMXCSRMaskOffset);
  return Mask ? Mask : BaselineMXCSRMask;
#else
  return 0;
#endif
}

template <typename Base> class Fixture : public Base {
protected:
  void SetUp() override {
    Base::SetUp();
    if (this->IsSkipped() || this->HasFatalFailure())
      return;
    auto Mask = this->CPU->supportedControlBits(CPURegister::X64MXCSR);
    ASSERT_TRUE(bool(Mask)) << llvm::toString(Mask.takeError());
    if (!((*Mask)[0] & DenormalsAreZero))
      GTEST_SKIP() << Unavailable;
  }
};

} // namespace neverd::emulation::daz_test
#endif
