//===- X86SystemStateNative.h - Native system APIs ----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Shared native API selection for bounded system effects. The assembly
/// contract remains the authority for lifted operands and legacy SDK output.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_LLVM_X86_X86SYSTEMSTATENATIVE_H
#define NEVERD_BACKEND_LLVM_X86_X86SYSTEMSTATENATIVE_H

#include "neverd/backend/llvm/X86/X86SystemStateAsm.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace llvm {
class Function;
}

namespace neverd {

/// RTM builtins and LLVM instruction selection require the same CPU feature.
inline bool isX86RtmNativeName(std::string_view Name) {
  return Name == "llvm.x86.xbegin" || Name == "llvm.x86.xend" ||
         Name == "llvm.x86.xabort" || Name == "llvm.x86.xtest";
}

inline const char *x86SystemNativeTargetFeature(std::string_view Name) {
  if (isX86RtmNativeName(Name))
    return "rtm";
  if (Name == "llvm.x86.clzero")
    return "clzero";
  return nullptr;
}

/// Retain native instruction-selection requirements in exported LLVM IR.
/// Existing family features stay attached to the same function.
void applyX86SystemNativeTargetFeatures(llvm::Function &Function);

inline constexpr Intrinsic X86SystemStateNativeInventory[] = {
    Intrinsic::Xend,
    Intrinsic::Xabort,
    Intrinsic::Clac,
    Intrinsic::Stac,
    Intrinsic::Clzero,
    Intrinsic::X86ClearTaskSwitched,
    Intrinsic::X86WriteMsrNonSerializing,
    Intrinsic::X86ReadMsrList,
    Intrinsic::X86WriteMsrList,
    Intrinsic::X86LoadKernelGs,
    Intrinsic::X86LoadKernelGsMemory,
    Intrinsic::X86InvalidateBroadcast,
    Intrinsic::X86TlbSync,
    Intrinsic::X86LoadAccessRights,
    Intrinsic::X86LoadAccessRightsMemory,
    Intrinsic::X86LoadSegmentLimit,
    Intrinsic::X86LoadSegmentLimitMemory};

struct X86SystemStateNative {
  X86SystemStateAsm Assembly;
  std::string LLVMName;
  std::string Builtin;
  std::vector<uint16_t> InputBytes;
  bool PackMsr = false;
  bool DescriptorQuery = false;
  unsigned AddressBits = 0;
  unsigned SegmentPrefix = 0;
};

struct X86SystemStateNativeAddress {
  unsigned Bits;
  unsigned SegmentPrefix;
};

inline std::optional<X86SystemStateNative> x86SystemStateNative(
    Intrinsic Id, Arch TargetArch, uint16_t Width, NdMemoryAddressSpace Space,
    std::optional<X86SystemStateNativeAddress> Address = std::nullopt) {
  bool Known = false;
  for (const auto Candidate : X86SystemStateNativeInventory)
    Known |= Id == Candidate;
  if (!Known)
    return std::nullopt;
  const auto Assembly = x86SystemStateAsm(Id, TargetArch, Width, Space);
  if (!Assembly)
    return std::nullopt;
  X86SystemStateNative Plan;
  Plan.Assembly = *Assembly;
  Plan.PackMsr = Id == Intrinsic::X86WriteMsrNonSerializing;
  const auto C = x86SystemStateContract(Id);
  Plan.DescriptorQuery = C.Kind == X86SystemStateKind::AccessRights;
  const unsigned WordBits = TargetArch == Arch::X64 ? 64 : 32;
  if (!Address && Space != NdMemoryAddressSpace::Default)
    Address = X86SystemStateNativeAddress{
        WordBits, Space == NdMemoryAddressSpace::X86FS ? 0x64u : 0x65u};
  if (Address) {
    const auto Explicit = x86SystemStateAddressAsm(
        Id, TargetArch, Width, Address->Bits, Address->SegmentPrefix);
    if (!Explicit)
      return std::nullopt;
    Plan.Assembly = *Explicit;
    Plan.AddressBits = Address->Bits;
    Plan.SegmentPrefix = Address->SegmentPrefix;
  }
  Plan.InputBytes =
      Plan.PackMsr ? std::vector<uint16_t>{4, 8} : Assembly->InputBytes;
  if (Plan.AddressBits)
    Plan.InputBytes.insert(Plan.InputBytes.end(), {4, 4});
  std::string Name = C.Mnemonic;
  if (Plan.DescriptorQuery) {
    Name += C.MemoryArgument == 0 ? ".mem" : ".reg";
    Name += std::to_string(8 * Width);
    if (Plan.AddressBits)
      Name += ".address";
    Plan.InputBytes.push_back(Width);
  } else if (Id == Intrinsic::X86LoadKernelGs)
    Name += ".reg";
  else if (Id == Intrinsic::X86LoadKernelGsMemory)
    Name += Plan.AddressBits ? ".address" : ".mem";
  else if (Id == Intrinsic::X86InvalidateBroadcast && TargetArch == Arch::X64 &&
           Width == 4)
    Name += ".addr32";
  Plan.Builtin = "__builtin_ia32_" + Name;
  for (char &Character : Plan.Builtin)
    if (Character == '.')
      Character = '_';
  if ((Id == Intrinsic::X86InvalidateBroadcast || Plan.AddressBits) &&
      TargetArch == Arch::X86)
    Name += "32";
  Plan.LLVMName = "llvm.x86." + Name;
  return Plan;
}

} // namespace neverd

#endif // NEVERD_BACKEND_LLVM_X86_X86SYSTEMSTATENATIVE_H
