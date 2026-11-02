//===- X86GetPcThunk.h - Exact i386 get-PC helper identity ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_LOADER_X86_X86GETPCTHUNK_H
#define NEVERD_LOADER_X86_X86GETPCTHUNK_H

#include "neverd/lift/X86/X86Regs.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/support/ISAEncoding.h"

#include <optional>
#include <string>

namespace neverd {
/// The encoded general-purpose register loaded by `mov r32,[esp]; ret`,
/// optionally preceded by GCC's bounded NOP padding. Names are not evidence.
inline std::optional<unsigned> x86GetPcThunkRegister(const BinaryImage &Image,
                                                     va_t Address) {
  if (Image.Arch != Arch::X86)
    return std::nullopt;
  size_t Offset = 0;
  while (Offset < x86::kShortFunctionPadding && Address <= InvalidVA - Offset) {
    const auto *Byte = Image.readVA(Address + Offset, 1);
    if (!Byte || *Byte != x86::kNop)
      break;
    ++Offset;
  }
  if (Address > InvalidVA - Offset)
    return std::nullopt;
  const auto *Body = Image.readVA(Address + Offset, x86::kGetPcThunkBodyLen);
  if (!Body || Body[0] != x86::kMovRegFromRmOp ||
      Body[x86::kGetPcThunkBodyLen - 1] != x86::kRetNear)
    return std::nullopt;
  const unsigned Reg = (Body[1] >> x86::kModRMRegShift) & x86::kModRMFieldMask;
  if ((Body[1] >> x86::kModRMModShift) != x86::kModMemory ||
      (Body[1] & x86::kModRMFieldMask) != x86::kRmSIB ||
      Body[2] != x86::kSIBStackTop || x86reg::generalReg(Reg) == x86reg::RSP ||
      !Image.isCodeRange(Address, Offset + x86::kGetPcThunkBodyLen))
    return std::nullopt;
  return Reg;
}

inline std::string x86GetPcThunkAssembly(unsigned Register) {
  return "movl (%esp), %" +
         std::string(getX86RegName(x86reg::generalReg(Register), 4)) +
         "\n\tretl";
}
} // namespace neverd
#endif
