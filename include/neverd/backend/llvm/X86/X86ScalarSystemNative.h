//===- X86ScalarSystemNative.h - Scalar MSR and port operations -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Shared widths and native names for scalar MSR and port I/O. Each access
/// is an ordered, potentially trapping effect, even when its result is dead.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_LLVM_X86_X86SCALARSYSTEMNATIVE_H
#define NEVERD_BACKEND_LLVM_X86_X86SCALARSYSTEMNATIVE_H

#include "neverd/ir/intrinsics/Intrinsics.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

namespace neverd {

struct X86ScalarSystemNative {
  Intrinsic Id;
  uint16_t Bytes;
  bool Write;
  bool Port;
  const char *LLVMName;
  const char *Builtin;
  const char *Microsoft;
  const char *Mnemonic;

  unsigned arguments() const { return Write ? 2 : 1; }
  uint16_t argumentBytes(unsigned I) const { return I ? Bytes : Port ? 2 : 4; }
};

inline constexpr std::array<X86ScalarSystemNative, 8>
    X86ScalarSystemNativeInventory = {{
        {Intrinsic::Rdmsr, 8, false, false, "llvm.x86.rdmsr",
         "__builtin_ia32_rdmsr", "__readmsr", "rdmsr"},
        {Intrinsic::Wrmsr, 8, true, false, "llvm.x86.wrmsr",
         "__builtin_ia32_wrmsr", "__writemsr", "wrmsr"},
        {Intrinsic::In, 1, false, true, "llvm.x86.inbyte",
         "__builtin_ia32_inbyte", "__inbyte", "inb"},
        {Intrinsic::In, 2, false, true, "llvm.x86.inword",
         "__builtin_ia32_inword", "__inword", "inw"},
        {Intrinsic::In, 4, false, true, "llvm.x86.indword",
         "__builtin_ia32_indword", "__indword", "inl"},
        {Intrinsic::Out, 1, true, true, "llvm.x86.outbyte",
         "__builtin_ia32_outbyte", "__outbyte", "outb"},
        {Intrinsic::Out, 2, true, true, "llvm.x86.outword",
         "__builtin_ia32_outword", "__outword", "outw"},
        {Intrinsic::Out, 4, true, true, "llvm.x86.outdword",
         "__builtin_ia32_outdword", "__outdword", "outl"},
    }};

inline bool isX86ScalarSystemIntrinsic(Intrinsic Id) {
  return Id == Intrinsic::Rdmsr || Id == Intrinsic::Wrmsr ||
         Id == Intrinsic::In || Id == Intrinsic::Out;
}

inline std::optional<X86ScalarSystemNative>
x86ScalarSystemNative(Intrinsic Id, Arch TargetArch, uint16_t Bytes) {
  if (TargetArch != Arch::X86 && TargetArch != Arch::X64)
    return std::nullopt;
  for (const auto &Plan : X86ScalarSystemNativeInventory)
    if (Plan.Id == Id && Plan.Bytes == Bytes)
      return Plan;
  return std::nullopt;
}

inline std::optional<X86ScalarSystemNative>
x86ScalarSystemNative(std::string_view Name) {
  for (const auto &Plan : X86ScalarSystemNativeInventory)
    if (Plan.LLVMName == Name)
      return Plan;
  return std::nullopt;
}

} // namespace neverd

#endif // NEVERD_BACKEND_LLVM_X86_X86SCALARSYSTEMNATIVE_H
