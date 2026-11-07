//===- X86BoundNative.h - Exact native BOUND projection -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Exact native BOUND projection.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_LLVM_X86_X86BOUNDNATIVE_H
#define NEVERD_BACKEND_LLVM_X86_X86BOUNDNATIVE_H

#include "neverd/backend/llvm/X86/X86MemoryAddressAsm.h"
#include "neverd/backend/llvm/X86/X86NativeMemoryAddress.h"
#include "neverd/ir/intrinsics/X86/X86Bound.h"

#include <string>

namespace neverd {

struct X86BoundNative {
  unsigned Bytes, AddressBits, Prefix;

  std::string llvmName() const {
    return "llvm.x86.bound" + std::to_string(Bytes * 8) + ".address32";
  }
  std::string builtin() const {
    return "__builtin_ia32_bound" + std::to_string(Bytes * 8) + "_address32";
  }
  std::string assembly(bool GNU = false) const {
    const std::string Segment = x86NativeMemorySegment(Prefix);
    return "bound " + std::string(GNU ? "%" : "${") +
           (GNU ? (Bytes == 2 ? "w0" : "k0") : (Bytes == 2 ? "0:w}" : "0:k}")) +
           ", " + (Segment.empty() ? "" : (GNU ? "%%" : "%") + Segment + ":") +
           "(" +
           (GNU ? (AddressBits == 16 ? "%w1" : "%k1")
                : (AddressBits == 16 ? "${1:w}" : "${1:k}")) +
           ")";
  }
  std::string constraints() const {
    return std::string("r,") + x86MemoryAddressAsm(AddressBits).LLVMConstraint +
           ",~{memory}";
  }
};

} // namespace neverd

#endif // NEVERD_BACKEND_LLVM_X86_X86BOUNDNATIVE_H
