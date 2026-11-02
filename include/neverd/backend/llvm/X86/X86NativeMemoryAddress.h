//===- X86NativeMemoryAddress.h - Native address controls -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Preserve stack-reference provenance independently of segment overrides.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_LLVM_X86_X86NATIVEMEMORYADDRESS_H
#define NEVERD_BACKEND_LLVM_X86_X86NATIVEMEMORYADDRESS_H

#if __has_include("llvm/IR/X86NativeMemoryAddress.h")
#include "llvm/IR/X86NativeMemoryAddress.h"
#endif

namespace neverd {

inline constexpr unsigned X86NativeStackReference = 0x100;

constexpr bool x86NativeMemoryIsStack(unsigned Control) {
  return Control & X86NativeStackReference;
}

constexpr unsigned x86NativeMemoryPrefix(unsigned Control) {
  return Control & ~X86NativeStackReference;
}

/// Stack-reference controls belong to long-mode, ordinary-SIB addresses.
/// Callers separately validate code mode and address width.
constexpr bool x86NativeMemoryControlIsValid(unsigned Control) {
  const auto Prefix = x86NativeMemoryPrefix(Control);
  if (Prefix && Prefix != 0x26 && Prefix != 0x2e && Prefix != 0x36 &&
      Prefix != 0x3e && Prefix != 0x64 && Prefix != 0x65)
    return false;
  return !x86NativeMemoryIsStack(Control) || (Prefix != 0x64 && Prefix != 0x65);
}

constexpr const char *x86NativeMemorySegment(unsigned Control) {
  switch (x86NativeMemoryPrefix(Control)) {
  case 0x26:
    return "es";
  case 0x2e:
    return "cs";
  case 0x36:
    return "ss";
  case 0x3e:
    return "ds";
  case 0x64:
    return "fs";
  case 0x65:
    return "gs";
  default:
    return "";
  }
}

constexpr bool x86NativeStackMemoryIsAvailable() {
#if defined(LLVM_X86_NATIVE_MEMORY_STACK_REFERENCE) &&                         \
    LLVM_X86_NATIVE_MEMORY_STACK_REFERENCE >= 1
  return true;
#else
  return false;
#endif
}

} // namespace neverd

#endif // NEVERD_BACKEND_LLVM_X86_X86NATIVEMEMORYADDRESS_H
