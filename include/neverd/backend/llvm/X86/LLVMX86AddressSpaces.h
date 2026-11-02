//===- LLVMX86AddressSpaces.h - LLVM x86 FS/GS address spaces -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// LLVM's X86 backend assigns these target address spaces to segment-relative
/// pointers.  MedLLVM emission and the LLVMC printer must use the same
/// mapping; do not restate 256/257 at a call site.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_LLVM_X86_LLVMX86ADDRESSSPACES_H
#define NEVERD_BACKEND_LLVM_X86_LLVMX86ADDRESSSPACES_H

#include "neverd/ir/NdOps.h"

#include "llvm/Support/ErrorHandling.h"

namespace neverd {

inline constexpr unsigned kLLVMX86GSAddressSpace = 256;
inline constexpr unsigned kLLVMX86FSAddressSpace = 257;

inline bool isLLVMX86GSAddressSpace(unsigned AddressSpace) {
  return AddressSpace == kLLVMX86GSAddressSpace;
}

inline bool isLLVMX86FSAddressSpace(unsigned AddressSpace) {
  return AddressSpace == kLLVMX86FSAddressSpace;
}

inline bool isLLVMX86SegmentedAddressSpace(unsigned AddressSpace) {
  return isLLVMX86GSAddressSpace(AddressSpace) ||
         isLLVMX86FSAddressSpace(AddressSpace);
}

inline unsigned llvmX86MemoryAddressSpace(NdMemoryAddressSpace AddressSpace) {
  switch (AddressSpace) {
  case NdMemoryAddressSpace::Default:
    return 0;
  case NdMemoryAddressSpace::X86FS:
    return kLLVMX86FSAddressSpace;
  case NdMemoryAddressSpace::X86GS:
    return kLLVMX86GSAddressSpace;
  }
  llvm_unreachable("unknown NeverD memory address space");
}

} // namespace neverd

#endif // NEVERD_BACKEND_LLVM_X86_LLVMX86ADDRESSSPACES_H
