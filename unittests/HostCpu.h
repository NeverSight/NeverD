//===- HostCpu.h - What the host processor is -------------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The KVM differentials compare the x86 model with the processor running the
/// original bytes, and the model follows Intel where vendors differ. Hosted CI
/// runners are a mix of AMD and Intel, so a test whose comparison reaches those
/// differences runs only on Intel and skips with the reason elsewhere.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_UNITTESTS_HOSTCPU_H
#define NEVERD_UNITTESTS_HOSTCPU_H

#if defined(__x86_64__) || defined(__i386__)
#include <cpuid.h>
#elif defined(_M_X64) || defined(_M_IX86)
#include <intrin.h>
#endif

namespace neverd::test {

/// Why an Intel-referenced KVM differential does not run on this host.
inline constexpr const char *IntelOracleOnlyReason =
    "the x86 model follows Intel; other vendors differ in hidden segment "
    "state, fault addresses and accessed-bit writes";

/// Whether the host processor reports Intel's vendor string.
inline bool hostCpuIsIntel() {
#if defined(__x86_64__) || defined(__i386__)
  unsigned Leaf = 0, Ebx = 0, Ecx = 0, Edx = 0;
  __cpuid(0, Leaf, Ebx, Ecx, Edx);
  return Ebx == 0x756e6547 && Edx == 0x49656e69 && Ecx == 0x6c65746e;
#elif defined(_M_X64) || defined(_M_IX86)
  int Registers[4] = {};
  __cpuid(Registers, 0);
  return Registers[1] == 0x756e6547 && Registers[3] == 0x49656e69 &&
         Registers[2] == 0x6c65746e;
#else
  return false;
#endif
}

} // namespace neverd::test

#endif // NEVERD_UNITTESTS_HOSTCPU_H
