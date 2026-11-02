//===- X86VirtualizationNative.h - Native VMX/SVM APIs ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Native LLVM and NeverC spellings for the owned virtualization contracts.
/// VMX status is 0 on success, 1 for ZF and 2 for CF. The lifted representation
/// keeps CF and ZF in separate bytes; the emitters convert at this boundary.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_LLVM_X86_X86VIRTUALIZATIONNATIVE_H
#define NEVERD_BACKEND_LLVM_X86_X86VIRTUALIZATIONNATIVE_H

#include "neverd/backend/llvm/X86/X86VirtualizationAsm.h"

#include <optional>
#include <string>

namespace neverd {

struct X86VirtualizationNative {
  X86VirtualizationAsm Assembly;
  std::string LLVMName;
  std::string Builtin;
  X86VirtualizationResult Result = X86VirtualizationResult::None;
  bool AddressControls = false;
  unsigned SegmentPrefix = 0;
};

inline std::optional<X86VirtualizationNative>
x86VirtualizationNative(Intrinsic Id, Arch TargetArch,
                        NdMemoryAddressSpace Space,
                        uint16_t PhysicalAddressBytes = 0) {
  const auto Assembly =
      x86VirtualizationAsm(Id, TargetArch, Space, PhysicalAddressBytes);
  // VMFUNC's native API owns only EPTP switching with EAX proved zero. The
  // existing two-register contract cannot establish that leaf by itself.
  if (!Assembly || Id == Intrinsic::VmxFunction)
    return std::nullopt;
  const auto C = x86VirtualizationContract(Id);
  X86VirtualizationNative Plan;
  Plan.Assembly = *Assembly;
  Plan.Result = Id == Intrinsic::VmxPtrStore || Id == Intrinsic::VmxOff
                    ? X86VirtualizationResult::None
                    : C.Result;
  std::string Name = Id == Intrinsic::VmxRead          ? "vmread.reg"
                     : Id == Intrinsic::VmxWriteMemory ? "vmwrite.mem"
                                                       : C.Mnemonic;
  Plan.AddressControls =
      C.MemoryArgument >= 0 && Space != NdMemoryAddressSpace::Default;
  if (Plan.AddressControls) {
    Name += ".address";
    Plan.SegmentPrefix = Space == NdMemoryAddressSpace::X86FS ? 0x64 : 0x65;
  }
  if (C.PhysicalAddress && TargetArch == Arch::X64 && PhysicalAddressBytes == 4)
    Name += ".addr32";
  Plan.Builtin = "__builtin_ia32_" + Name;
  for (char &Character : Plan.Builtin)
    if (Character == '.')
      Character = '_';
  const bool ModeSized =
      Plan.AddressControls || C.PhysicalAddress || Id == Intrinsic::VmxRead ||
      Id == Intrinsic::VmxReadMemory || Id == Intrinsic::VmxWrite ||
      Id == Intrinsic::VmxWriteMemory || Id == Intrinsic::VmxInvalidateEpt ||
      Id == Intrinsic::VmxInvalidateVpid;
  if (ModeSized && TargetArch == Arch::X86)
    Name += "32";
  Plan.LLVMName = "llvm.x86." + Name;
  return Plan;
}

} // namespace neverd

#endif // NEVERD_BACKEND_LLVM_X86_X86VIRTUALIZATIONNATIVE_H
