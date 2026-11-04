//===- X86FlagsNative.h - Exact-width flag snapshots -----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Native flag-image widths shared by LLVM and both C projections.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_LLVM_X86_X86FLAGSNATIVE_H
#define NEVERD_BACKEND_LLVM_X86_X86FLAGSNATIVE_H

#include "neverd/ir/intrinsics/Intrinsics.h"

#include "llvm/ADT/StringRef.h"

#include <array>
#include <cstdint>
#include <optional>

namespace neverd {

struct X86FlagsNative {
  Intrinsic Id;
  uint16_t Bytes;
  const char *LLVMName;
  const char *Builtin;
  const char *Assembly;

  bool write() const { return Id == Intrinsic::Popf; }
  bool supports(Arch Target) const {
    return (Target == Arch::X86 || Target == Arch::X64) &&
           (Bytes == 2 || Bytes == (Target == Arch::X64 ? 8 : 4));
  }
};

inline constexpr std::array<X86FlagsNative, 6> X86FlagsNativeInventory = {{
    {Intrinsic::Pushf, 2, "llvm.x86.flags.read.u16",
     "__builtin_ia32_readeflags_u16", "pushfw\n\tpopw $0"},
    {Intrinsic::Pushf, 4, "llvm.x86.flags.read.u32", "__readeflags",
     "pushfl\n\tpopl $0"},
    {Intrinsic::Pushf, 8, "llvm.x86.flags.read.u64", "__readeflags",
     "pushfq\n\tpopq $0"},
    {Intrinsic::Popf, 2, "llvm.x86.flags.write.u16",
     "__builtin_ia32_writeeflags_u16", "pushw $0\n\tpopfw"},
    {Intrinsic::Popf, 4, "llvm.x86.flags.write.u32", "__writeeflags",
     "pushl $0\n\tpopfl"},
    {Intrinsic::Popf, 8, "llvm.x86.flags.write.u64", "__writeeflags",
     "pushq $0\n\tpopfq"},
}};

inline std::optional<X86FlagsNative> x86FlagsNative(Intrinsic Id, Arch Target,
                                                    unsigned Bytes) {
  for (const auto &Plan : X86FlagsNativeInventory)
    if (Plan.Id == Id && Plan.Bytes == Bytes && Plan.supports(Target))
      return Plan;
  return std::nullopt;
}

inline std::optional<X86FlagsNative> x86FlagsNative(llvm::StringRef Name) {
  if (!Name.starts_with("llvm.x86.flags."))
    return std::nullopt;
  for (const auto &Plan : X86FlagsNativeInventory)
    if (Plan.LLVMName == Name)
      return Plan;
  return std::nullopt;
}

} // namespace neverd

#endif // NEVERD_BACKEND_LLVM_X86_X86FLAGSNATIVE_H
