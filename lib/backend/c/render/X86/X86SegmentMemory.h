//===- X86SegmentMemory.h - C segmented memory reads --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Shared x86 C spelling for exact FS/GS scalar reads.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_BACKEND_C_RENDER_X86_X86SEGMENTMEMORY_H
#define NEVERD_LIB_BACKEND_C_RENDER_X86_X86SEGMENTMEMORY_H

#include "neverd/Common.h"
#include "neverd/ir/NdOps.h"

#include "llvm/ADT/StringRef.h"

#include <cstdint>
#include <optional>
#include <string>

namespace neverd {

/// Read raw FS/GS offsets without truncating the target address register.
/// MSVC intrinsics are used only when their 32-bit offset and target match;
/// other reads use native assembly. ResultName must not occur in Addr.
std::string renderX86SegmentedRead(Arch TheArch, unsigned SizeBytes,
                                   llvm::StringRef Addr,
                                   NdMemoryAddressSpace AddressSpace,
                                   bool MsvcIntrinsics,
                                   std::optional<uint64_t> ConstantOffset,
                                   llvm::StringRef ResultName);

} // namespace neverd

#endif // NEVERD_LIB_BACKEND_C_RENDER_X86_X86SEGMENTMEMORY_H
