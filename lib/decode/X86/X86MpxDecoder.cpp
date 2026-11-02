//===- X86MpxDecoder.cpp - MPX decode adapters ---------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Publish the same byte-owned MPX operands in full and lightweight decoding.
//===----------------------------------------------------------------------===//

#include "neverd/decode/Decoder.h"
#include "neverd/lift/X86/X86MpxEncoding.h"

#include "llvm/Support/Format.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cstdio>
#include <string>

namespace neverd {
namespace {

void publishEncoding(cs_insn &Insn, const X86MpxEncoding &E,
                     llvm::ArrayRef<uint8_t> Bytes, va_t Address, csh Handle,
                     bool WithDetail, bool WithText) {
  // Capstone owns the detail allocation, including when disassembly failed.
  auto *Detail = Insn.detail;
  Insn = {};
  Insn.detail = Detail;
  Insn.id = E.Id;
  Insn.address = Address;
  Insn.size = E.Size;
  std::copy_n(Bytes.begin(), E.Size, Insn.bytes);
  if (Detail) {
    *Detail = {};
    if (WithDetail)
      Detail->x86 = E.Detail;
  }
  if (!WithText)
    return;
  std::snprintf(Insn.mnemonic, sizeof(Insn.mnemonic), "%s", E.Mnemonic);
  std::string Operands;
  llvm::raw_string_ostream OS(Operands);
  for (unsigned I = 0; I != E.Detail.op_count; ++I) {
    if (I)
      OS << ", ";
    const auto &Operand = E.Detail.operands[I];
    if (Operand.type == X86_OP_REG) {
      OS << cs_reg_name(Handle, Operand.reg);
      continue;
    }
    if (E.Id == X86_INS_BNDMOV)
      OS << (Operand.size == 16 ? "xmmword ptr " : "qword ptr ");
    const auto &Memory = Operand.mem;
    if (Memory.segment != X86_REG_INVALID)
      OS << cs_reg_name(Handle, Memory.segment) << ':';
    OS << '[';
    bool HasTerm = false;
    if (Memory.base != X86_REG_INVALID) {
      OS << cs_reg_name(Handle, Memory.base);
      HasTerm = true;
    }
    if (Memory.index != X86_REG_INVALID) {
      if (HasTerm)
        OS << " + ";
      OS << cs_reg_name(Handle, Memory.index);
      if (Memory.scale != 1)
        OS << '*' << Memory.scale;
      HasTerm = true;
    }
    if (Memory.disp || !HasTerm) {
      // Displacements are at most signed 32 bits. A base-free i386 address
      // uses its unsigned effective value; based operands retain their sign.
      const int64_t Displacement = !HasTerm && E.Detail.addr_size == 4
                                       ? uint32_t(Memory.disp)
                                       : Memory.disp;
      if (HasTerm)
        OS << (Displacement < 0 ? " - " : " + ");
      else if (Displacement < 0)
        OS << '-';
      OS << llvm::format_hex(Displacement < 0 ? -Displacement : Displacement,
                             1);
    }
    OS << ']';
  }
  std::snprintf(Insn.op_str, sizeof(Insn.op_str), "%s", Operands.c_str());
}

} // namespace

bool Decoder::decodeX86MpxFallback(const uint8_t *Bytes, size_t Len,
                                   va_t Address) {
  if (!X86 || !Bytes || !InsnBuf)
    return false;
  const auto E = decodeX86MpxEncoding({Bytes, Len}, TargetArch);
  if (!E)
    return false;
  publishEncoding(*InsnBuf, *E, {Bytes, Len}, Address, Handle, Detail && Text,
                  Text);
  return true;
}

void Decoder::fixupX86MpxDetail(cs_insn *Insn) const {
  if (!X86 || !Insn)
    return;
  switch (Insn->id) {
  case X86_INS_BNDCL:
  case X86_INS_BNDCU:
  case X86_INS_BNDCN:
  case X86_INS_BNDMK:
  case X86_INS_BNDMOV:
  case X86_INS_BNDLDX:
  case X86_INS_BNDSTX:
    break;
  default:
    return;
  }
  if (Insn->size > sizeof(Insn->bytes))
    return;
  const auto E = decodeX86MpxEncoding({Insn->bytes, Insn->size}, TargetArch);
  if (!E || E->Size != Insn->size)
    return;
  // publishEncoding replaces Insn, so preserve the original bytes separately.
  const auto Original = *Insn;
  publishEncoding(*Insn, *E, {Original.bytes, Original.size}, Original.address,
                  Handle, Detail && Text, Text);
}

} // namespace neverd
