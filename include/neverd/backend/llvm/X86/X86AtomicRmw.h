//===- X86AtomicRmw.h - Native scalar RMW contract ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Native exchanges and locked additions accept unaligned memory addresses.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_LLVM_X86_X86ATOMICRMW_H
#define NEVERD_BACKEND_LLVM_X86_X86ATOMICRMW_H

#include "neverd/Common.h"
#include "neverd/ir/NdOps.h"

#include <optional>
#include <string>
#include <string_view>

namespace neverd {

inline constexpr std::string_view kX86AtomicExchangeAsmMarker =
    "# neverd.x86.exchange ";
inline constexpr std::string_view kX86AtomicAddAsmMarker =
    "# neverd.x86.fetch_add ";

struct X86AtomicRmwPlan {
  Arch Architecture;
  unsigned Bytes;
  NdMemoryAddressSpace AddressSpace;
  NdOp Opcode = NdOp::ATOMIC_XCHG;

  unsigned addressBytes() const { return Architecture == Arch::X86 ? 4 : 8; }
  std::string mnemonic() const {
    const auto Operation = Opcode == NdOp::ATOMIC_ADD ? "lock xadd" : "xchg";
    const auto Suffix = Bytes == 1   ? "b"
                        : Bytes == 2 ? "w"
                        : Bytes == 4 ? "l"
                                     : "q";
    return std::string(Operation) + Suffix;
  }
  std::string segment() const {
    return AddressSpace == NdMemoryAddressSpace::X86FS   ? "fs"
           : AddressSpace == NdMemoryAddressSpace::X86GS ? "gs"
                                                         : "";
  }
  std::string constraints() const {
    // The byte output must be an encodable low-byte register. The address
    // and value are both inputs until the operation completes.
    std::string Result = Bytes == 1 ? "=q,r,0,~{memory}" : "=r,r,0,~{memory}";
    if (Opcode == NdOp::ATOMIC_ADD)
      Result += ",~{flags}";
    return Result;
  }
  std::string assembly() const {
    const auto Prefix = segment().empty() ? "" : "%" + segment() + ":";
    const auto Marker = Opcode == NdOp::ATOMIC_ADD
                            ? kX86AtomicAddAsmMarker
                            : kX86AtomicExchangeAsmMarker;
    return std::string(Marker) + std::to_string(Bytes) + " " +
           std::to_string(static_cast<unsigned>(AddressSpace)) + "\n" +
           mnemonic() + " $0, " + Prefix + "($1)";
  }
};

inline std::optional<X86AtomicRmwPlan>
x86AtomicRmwPlan(Arch Architecture, NdOp Opcode, unsigned Bytes,
                 NdMemoryAddressSpace AddressSpace) {
  if ((Opcode != NdOp::ATOMIC_XCHG && Opcode != NdOp::ATOMIC_ADD) ||
      (Architecture != Arch::X86 && Architecture != Arch::X64) ||
      (Bytes != 1 && Bytes != 2 && Bytes != 4 && Bytes != 8) ||
      (Architecture == Arch::X86 && Bytes == 8))
    return std::nullopt;
  switch (AddressSpace) {
  case NdMemoryAddressSpace::Default:
  case NdMemoryAddressSpace::X86FS:
  case NdMemoryAddressSpace::X86GS:
    return X86AtomicRmwPlan{Architecture, Bytes, AddressSpace, Opcode};
  }
  return std::nullopt;
}

} // namespace neverd

#endif // NEVERD_BACKEND_LLVM_X86_X86ATOMICRMW_H
