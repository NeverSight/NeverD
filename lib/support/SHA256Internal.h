//===- SHA256Internal.h - Digest implementation selection -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Private feature admission and dispatch observation. Tests may compile
/// SHA256.cpp with NEVERD_SHA256_PORTABLE to exercise the LLVM fallback;
/// product builds always retain runtime feature selection.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_SUPPORT_SHA256INTERNAL_H
#define NEVERD_LIB_SUPPORT_SHA256INTERNAL_H

#include <array>
#include <cstdint>

namespace neverd::detail {

enum class X86CPUIDRegister : unsigned { EAX, EBX, ECX, EDX };
using X86CPUIDResult = std::array<uint32_t, 4>;

struct SHA256X86Feature {
  const char *Name;
  uint32_t Leaf;
  uint32_t Subleaf;
  X86CPUIDRegister Register;
  unsigned Bit;
};

inline constexpr SHA256X86Feature SHA256X86Features[] = {
#define NEVERD_SHA256_X86_FEATURE(Name, Leaf, Subleaf, Register, Bit)          \
  {Name, Leaf, Subleaf, X86CPUIDRegister::Register, Bit},
#include "SHA256.def"
};

/// Admit only the required bits from supported basic CPUID leaves. The reader
/// keeps the query boundary testable without issuing unsupported instructions.
/// Adjacent requirements from the same leaf share one query.
template <class Query> bool x86HasSHA256Features(Query Read) {
  uint32_t Leaf = 0, Subleaf = 0;
  X86CPUIDResult Registers = Read(Leaf, Subleaf);
  const uint32_t Maximum =
      Registers[static_cast<unsigned>(X86CPUIDRegister::EAX)];
  for (const auto &Feature : SHA256X86Features) {
    if (Feature.Leaf > Maximum)
      return false;
    if (Feature.Leaf != Leaf || Feature.Subleaf != Subleaf) {
      Leaf = Feature.Leaf;
      Subleaf = Feature.Subleaf;
      Registers = Read(Leaf, Subleaf);
    }
    const uint32_t Value = Registers[static_cast<unsigned>(Feature.Register)];
    if (!(Value & (uint32_t{1} << Feature.Bit)))
      return false;
  }
  return true;
}

/// Cached OS/CPU capability decision actually used by the one-shot owner.
bool sha256UsesHardware();

} // namespace neverd::detail

#endif // NEVERD_LIB_SUPPORT_SHA256INTERNAL_H
