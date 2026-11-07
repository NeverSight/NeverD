//===- X86VirtualizationAsm.h - Owned VMX/SVM asm contracts ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// One assembly contract shared by LLVM lowering and both C routes. Capturing
/// CF/ZF with SETcc does not change flags or require the optional LAHF feature.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_LLVM_X86_X86VIRTUALIZATIONASM_H
#define NEVERD_BACKEND_LLVM_X86_X86VIRTUALIZATIONASM_H

#include "neverd/ir/intrinsics/X86/X86Virtualization.h"

#include <optional>
#include <string>
#include <vector>

namespace neverd {

struct X86VirtualizationAsm {
  Intrinsic Id = Intrinsic::None;
  Arch TargetArch = Arch::Unknown;
  std::string Text;
  std::string Constraints;
  std::vector<std::string> OutputConstraints;
  std::vector<std::string> InputConstraints;
  std::vector<uint16_t> InputBytes;
};

inline std::optional<X86VirtualizationAsm>
x86VirtualizationAsm(Intrinsic Id, Arch TargetArch, NdMemoryAddressSpace Space,
                     uint16_t PhysicalAddressBytes = 0) {
  // Context and hypervisor APIs have separate register/memory transports.
  // Only the architectural instruction inventory can acquire an asm plan.
  switch (Id) {
#define NEVERD_X86_VIRTUALIZATION(Id, ...) case Intrinsic::Id:
#include "neverd/ir/intrinsics/X86/X86Virtualization.def"
#undef NEVERD_X86_VIRTUALIZATION
    break;
  default:
    return std::nullopt;
  }
  const auto C = x86VirtualizationContract(Id);
  if (!C.Mnemonic || (TargetArch != Arch::X86 && TargetArch != Arch::X64) ||
      !isKnownMemoryAddressSpace(Space) ||
      (C.MemoryArgument < 0 && Space != NdMemoryAddressSpace::Default))
    return std::nullopt;
  const uint16_t Word = TargetArch == Arch::X64 ? 8 : 4;
  if (C.PhysicalAddress &&
      ((PhysicalAddressBytes != 4 && PhysicalAddressBytes != 8) ||
       (TargetArch == Arch::X86 && PhysicalAddressBytes != 4)))
    return std::nullopt;
  X86VirtualizationAsm P;
  P.Id = Id;
  P.TargetArch = TargetArch;
  if (C.Result == X86VirtualizationResult::Read)
    P.OutputConstraints = {"=&r", "={ax}"};
  else if (C.Result == X86VirtualizationResult::Flags)
    P.OutputConstraints = {"={ax}"};
  for (uint8_t I = 0; I < C.Arguments; ++I) {
    P.InputConstraints.push_back("r");
    P.InputBytes.push_back(I == C.MemoryArgument         ? Word
                           : C.PhysicalAddress && I == 0 ? PhysicalAddressBytes
                           : Id == Intrinsic::VmxFunction ||
                                   Id == Intrinsic::SvmInvalidate
                               ? 4
                               : Word);
  }
  const std::string Segment = Space == NdMemoryAddressSpace::X86FS   ? "%fs:"
                              : Space == NdMemoryAddressSpace::X86GS ? "%gs:"
                                                                     : "";
  auto Operand = [&](unsigned I) {
    return "$" + std::to_string(P.OutputConstraints.size() + I);
  };
  auto Memory = [&](unsigned I) { return Segment + "(" + Operand(I) + ")"; };
  P.Text = C.Mnemonic;
  switch (Id) {
  case Intrinsic::VmxRead:
    P.Text += " " + Operand(0) + ", $0";
    break;
  case Intrinsic::VmxReadMemory:
    P.Text += " " + Operand(0) + ", " + Memory(1);
    break;
  case Intrinsic::VmxWrite:
    P.Text += " " + Operand(1) + ", " + Operand(0);
    break;
  case Intrinsic::VmxWriteMemory:
    P.Text += " " + Memory(1) + ", " + Operand(0);
    break;
  case Intrinsic::VmxInvalidateEpt:
  case Intrinsic::VmxInvalidateVpid:
    P.Text += " " + Memory(1) + ", " + Operand(0);
    break;
  case Intrinsic::VmxFunction:
    P.InputConstraints = {"{ax}", "{cx}"};
    break;
  case Intrinsic::SvmLoad:
  case Intrinsic::SvmSave:
    P.InputConstraints = {"{ax}"};
    P.Text += " " + Operand(0);
    break;
  case Intrinsic::SvmInvalidate:
    P.InputConstraints = {"{ax}", "{cx}"};
    P.Text += " " + Operand(0) + ", " + Operand(1);
    break;
  default:
    if (C.MemoryArgument == 0)
      P.Text += " " + Memory(0);
    break;
  }
  if (C.Result != X86VirtualizationResult::None)
    P.Text += "\n\tsetc %al\n\tsetz %ah";
  auto Constraint = [&](const std::string &Text) {
    if (!P.Constraints.empty())
      P.Constraints += ',';
    P.Constraints += Text;
  };
  for (const auto &O : P.OutputConstraints)
    Constraint(O);
  for (const auto &I : P.InputConstraints)
    Constraint(I);
  if (C.Result != X86VirtualizationResult::None)
    Constraint("~{flags}");
  Constraint("~{memory}");
  return P;
}

} // namespace neverd

#endif // NEVERD_BACKEND_LLVM_X86_X86VIRTUALIZATIONASM_H
