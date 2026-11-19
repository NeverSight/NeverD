//===- GoCallContract.cpp - Registers a call from Go code keeps -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Go code has no callee-saved registers, and its runtime write barriers keep
/// every general-purpose register they do not return in.
///
//===----------------------------------------------------------------------===//

#include "neverd/ir/low/GoCallContract.h"

#include "neverd/lift/AArch64/AArch64Regs.h"
#include "neverd/lift/X86/X86Regs.h"

#include <algorithm>

namespace neverd {
namespace {

/// Whether \p Register is a general-purpose register of Go's on \p A, which
/// a write barrier keeps: RAX through R15, or X0 through X30.
bool isGoGeneralRegister(Arch A, uint64_t Register) {
  if (A == Arch::X64)
    return Register <= x86reg::R15;
  if (A == Arch::AArch64)
    return Register <= a64reg::X30;
  return false;
}

} // namespace

bool isGoCode(const BinaryImage &Img, va_t Address) {
  const auto &Module = Img.ExceptionMetadata.GoModule;
  if (!Module || (Img.Arch != Arch::X64 && Img.Arch != Arch::AArch64))
    return false;
  const auto &Ranges = Module->GoCode;
  const auto After = std::upper_bound(
      Ranges.begin(), Ranges.end(), Address,
      [](va_t A, const ExceptionAddressRange &R) { return A < R.Begin; });
  return After != Ranges.begin() && std::prev(After)->contains(Address);
}

const GoWriteBarrier *goWriteBarrierAt(const BinaryImage &Img, va_t Entry) {
  const auto &Module = Img.ExceptionMetadata.GoModule;
  if (!Module)
    return nullptr;
  const auto &Barriers = Module->WriteBarriers;
  const auto It = std::lower_bound(
      Barriers.begin(), Barriers.end(), Entry,
      [](const GoWriteBarrier &B, va_t A) { return B.Entry < A; });
  return It != Barriers.end() && It->Entry == Entry ? &*It : nullptr;
}

std::optional<uint64_t> goWriteBarrierResult(Arch A, GoWriteBarrierKind Kind) {
  if (Kind != GoWriteBarrierKind::Buffer)
    return std::nullopt;
  if (A == Arch::X64)
    return x86reg::R11;
  if (A == Arch::AArch64)
    return a64reg::X25;
  return std::nullopt;
}

CallRegisterContract callRegisterContractOf(const BinaryImage &Img, va_t Caller,
                                            va_t Callee) {
  if (!isGoCode(Img, Caller))
    return CallRegisterContract::Platform;
  if (Callee != InvalidVA && goWriteBarrierAt(Img, Callee))
    return CallRegisterContract::GoWriteBarrier;
  return CallRegisterContract::GoCode;
}

std::optional<uint64_t> goCallResultRegister(const BinaryImage &Img,
                                             CallRegisterContract Contract,
                                             va_t Callee) {
  if (Contract != CallRegisterContract::GoWriteBarrier)
    return std::nullopt;
  const GoWriteBarrier *Barrier = goWriteBarrierAt(Img, Callee);
  return Barrier ? goWriteBarrierResult(Img.Arch, Barrier->Kind) : std::nullopt;
}

uint16_t goContractKeptBytes(const TargetRegInfo &TRI,
                             CallRegisterContract Contract, uint64_t RegOff,
                             uint16_t Size, std::optional<uint64_t> Result) {
  const uint16_t Width = TRI.FullRegWidth;
  if (Size == 0 || Width == 0 || RegOff % Width + Size > Width)
    return 0;
  const uint64_t Register = RegOff - RegOff % Width;
  // The stack pointer and Go's frame pointer keep their values across any
  // call ("RSP | Stack pointer | Same", "RBP | Frame pointer | Same" in
  // abi-internal.md, likewise RSP and R29 on arm64).
  if (TRI.isFrameReg(Register))
    return Size;
  if (Contract != CallRegisterContract::GoWriteBarrier ||
      !isGoGeneralRegister(TRI.TheArch, Register) ||
      (Result && Register == *Result) ||
      (TRI.TheArch == Arch::AArch64 && Register == a64reg::X27))
    return 0;
  return Size;
}

} // namespace neverd
