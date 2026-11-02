//===- X86SegmentMoveNative.h - Native selector moves ---------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Native compiler identities for segment selector reads and writes.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_LLVM_X86_X86SEGMENTMOVENATIVE_H
#define NEVERD_BACKEND_LLVM_X86_X86SEGMENTMOVENATIVE_H

#include "neverd/ir/intrinsics/X86/X86SegmentRegisters.h"

#include <array>

namespace neverd {

struct X86SegmentMoveNative {
  const char *LLVMName;
  const char *Builtin;
  bool Read;
  bool Memory;
};

inline constexpr std::array<X86SegmentMoveNative, 4> X86SegmentMoveInventory{{
    {"llvm.x86.readseg", "__builtin_ia32_readseg", true, false},
    {"llvm.x86.readseg.mem", "__builtin_ia32_readseg_mem", true, true},
    {"llvm.x86.writeseg", "__builtin_ia32_writeseg", false, false},
    {"llvm.x86.writeseg.mem", "__builtin_ia32_writeseg_mem", false, true},
}};

constexpr bool x86SegmentMoveRegisterIsValid(uint64_t Number, bool Read) {
  return x86SegmentRegisterName(Number) &&
         (Read || Number != static_cast<unsigned>(X86SegmentRegister::CS));
}

} // namespace neverd

#endif // NEVERD_BACKEND_LLVM_X86_X86SEGMENTMOVENATIVE_H
