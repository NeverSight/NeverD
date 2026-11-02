//===- X86SystemRegisterAsm.h - CR/DR native contract -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Native-width control/debug register moves shared by LLVM and C emission.
/// These accesses are ordered, potentially trapping, and leave arithmetic
/// flags undefined. DR4/DR5 keep their native CR4.DE-dependent behavior.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_LLVM_X86_X86SYSTEMREGISTERASM_H
#define NEVERD_BACKEND_LLVM_X86_X86SYSTEMREGISTERASM_H

#include "neverd/ir/intrinsics/Intrinsics.h"

#include <cstdint>
#include <optional>
#include <string>

namespace neverd {

struct X86SystemRegisterAsm {
  Intrinsic Id;
  std::string Register;
  std::string Text;
  std::string Constraints;
  uint16_t Bytes;
  bool Write;
  bool Debug;
  uint64_t Index;
  std::string LLVMName;
  std::string Builtin;
};

inline std::optional<X86SystemRegisterAsm>
x86SystemRegisterAsm(Intrinsic Id, Arch TargetArch, uint64_t DebugIndex) {
  const auto Reg = x86SystemRegisterName(Id, DebugIndex);
  if (!Reg || (TargetArch != Arch::X86 && TargetArch != Arch::X64) ||
      (TargetArch != Arch::X64 &&
       (Id == Intrinsic::ReadCr8 || Id == Intrinsic::WriteCr8)))
    return std::nullopt;
  const bool Write = Id == Intrinsic::WriteCr0 || Id == Intrinsic::WriteCr2 ||
                     Id == Intrinsic::WriteCr3 || Id == Intrinsic::WriteCr4 ||
                     Id == Intrinsic::WriteCr8 || Id == Intrinsic::WriteDr;
  const bool Debug = Id == Intrinsic::ReadDr || Id == Intrinsic::WriteDr;
  const std::string Access =
      std::string(Write ? "write" : "read") + (Debug ? "dr" : "cr");
  return X86SystemRegisterAsm{
      Id, *Reg, Write ? "mov $0, " + *Reg : "mov " + *Reg + ", $0",
      Write ? "r,~{flags},~{memory}" : "=r,~{flags},~{memory}",
      static_cast<uint16_t>(TargetArch == Arch::X64 ? 8 : 4), Write, Debug,
      // The authoritative name above has one decimal digit (CR0..8/DR0..7).
      static_cast<uint64_t>(Reg->back() - '0'),
      "llvm.x86." + Access + (TargetArch == Arch::X86 ? "32" : ""),
      "__builtin_ia32_" + Access};
}

} // namespace neverd

#endif // NEVERD_BACKEND_LLVM_X86_X86SYSTEMREGISTERASM_H
