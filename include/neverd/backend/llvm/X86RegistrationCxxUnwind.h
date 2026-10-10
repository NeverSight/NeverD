//===- X86RegistrationCxxUnwind.h - PE32 unwind projection ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_BACKEND_LLVM_X86REGISTRATIONCXXUNWIND_H
#define NEVERD_BACKEND_LLVM_X86REGISTRATIONCXXUNWIND_H

#include <cstdint>
#include <optional>
#include <vector>

namespace neverd {
struct CxxExceptionInfo;

struct X86RegistrationCxxUnwindTarget {
  enum class Kind { Caller, Try, Cleanup };
  Kind TargetKind = Kind::Caller;
  uint32_t Index = 0;
};

/// Project synchronous try groups and direct cleanup states. Nested catch
/// regions additionally require the checked runtime invocation parent proof.
/// Indices name source try records or source cleanup states, never IR blocks.
std::optional<std::vector<X86RegistrationCxxUnwindTarget>>
projectX86RegistrationCxxUnwind(const CxxExceptionInfo &Cxx);
} // namespace neverd
#endif
