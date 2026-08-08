//===- ProloguePatterns.h - ISA function prologue detection ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Common function prologue byte/word patterns used for heuristic function
/// discovery across all binary formats.  Annotated with instruction mnemonics.
///
/// Provides per-ISA prologue detection with two confidence levels:
///   - Strict:  high-confidence bytes — almost always a real function
///   - Relaxed: broader set — useful after CC padding or alignment NOPs
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SUPPORT_PROLOGUEPATTERNS_H
#define NEVERD_SUPPORT_PROLOGUEPATTERNS_H

#include "neverd/Common.h"
#include "neverd/Support/ISAEncoding.h"

#include <cstdint>
#include <cstring>

namespace neverd {

// ===--------------------------------------------------------------------===//
// Inter-function code padding
// ===--------------------------------------------------------------------===//

/// The byte a toolchain uses to pad gaps between functions in executable
/// sections: 0xCC (INT3) on x86/x86-64, zero elsewhere (ARM/AArch64).
inline uint8_t codePaddingByte(Arch A) {
  if (A == Arch::X64 || A == Arch::X86)
    return x86::kInt3;
  return 0;
}

// ===--------------------------------------------------------------------===//
// x86 / x86-64 padding and prologue constants
// ===--------------------------------------------------------------------===//

// ===--------------------------------------------------------------------===//
// x86 / x86-64 prologue byte patterns
// ===--------------------------------------------------------------------===//

inline bool isStrictPrologueByteX86(uint8_t B) {
  switch (B) {
  case 0x40: // REX        (x64 only)
  case 0x41: // REX.B
  case 0x48: // REX.W      (sub rsp, ...; mov rbp, rsp)
  case 0x49: // REX.WB
  case 0x4C: // REX.WR
  case 0x4D: // REX.WRB
  case 0x53: // push rbx / push ebx
  case 0x55: // push rbp / push ebp
  case 0x56: // push rsi / push esi
  case 0x57: // push rdi / push edi
  case 0x31: // xor r32, r32   (zero a register)
  case 0x33: // xor r32, r32   (alternate encoding)
  case 0x8B: // mov r32, r/m32 (mov ebp, esp)
  case 0xB8: // mov eax, imm32
  case 0xC3: // ret             (thunk / leaf)
  case 0xE9: // jmp rel32       (tail-call thunk)
  case 0xFF: // ff 25 ... (jmp [IAT])
    return true;
  default:
    return false;
  }
}

inline bool isRelaxedPrologueByteX86(uint8_t B) {
  switch (B) {
  // Full REX prefix range (0x40-0x4F), x64 only
  case 0x40:
  case 0x41:
  case 0x42:
  case 0x43:
  case 0x44:
  case 0x45:
  case 0x46:
  case 0x47:
  case 0x48:
  case 0x49:
  case 0x4A:
  case 0x4B:
  case 0x4C:
  case 0x4D:
  case 0x4E:
  case 0x4F:
  // PUSH r (0x50-0x57)
  case 0x50:
  case 0x51:
  case 0x52:
  case 0x53: // push rax/rcx/rdx/rbx
  case 0x54:
  case 0x55:
  case 0x56:
  case 0x57: // push rsp/rbp/rsi/rdi
  // XOR / TEST
  case 0x31:
  case 0x32:
  case 0x33: // xor
  case 0x84:
  case 0x85: // test
  // MOV variants
  case 0x88:
  case 0x89:
  case 0x8A:
  case 0x8B: // mov r/m <-> r
  case 0x8D: // lea
  // MOV r, imm (0xB0-0xBF)
  case 0xB0:
  case 0xB8:
  case 0xB9:
  case 0xBA:
  case 0xBB:
  case 0xBC:
  case 0xBD:
  case 0xBE:
  case 0xBF:
  // Arithmetic immediate
  case 0x80:
  case 0x83: // op r/m, imm8
  // RET / JMP
  case 0xC2:
  case 0xC3: // ret imm16 / ret
  case 0xE9:
  case 0xEB: // jmp rel32 / jmp rel8
  // Prefix
  case 0x66: // operand-size override
    return true;
  default:
    return false;
  }
}

// ===--------------------------------------------------------------------===//
// AArch64 prologue word patterns (first 32-bit instruction)
// ===--------------------------------------------------------------------===//

inline bool isStrictPrologueWordAArch64(uint32_t W) {
  // STP x29, x30, [sp, #imm]! — canonical frame setup
  // Encoding: 1010100110 iiiiiii 11110 11101 11111  (STP pre-index)
  if ((W & 0xFFE00000) == 0xA9800000) // STP pre-index, any offset
    return true;
  // STP with x29,x30: check Rt2=x30(11110), Rn=sp(11111), Rt=x29(11101)
  if ((W & 0xFFC07FFF) == 0xA9007BFD) // STP x29, x30, [sp, ...]
    return true;
  // SUB sp, sp, #imm — stack allocation
  if ((W & 0xFF0003FF) == 0xD10003FF) // sub sp, sp, #imm
    return true;
  // MOV x29, sp (ADD x29, sp, #0)
  if (W == 0x910003FD)
    return true;
  // PACIBSP (pointer auth on LR before push)
  if (W == 0xD503237F)
    return true;
  // BTI c / BTI j / BTI jc — branch target identification
  if ((W & 0xFFFFFF3F) == 0xD503241F)
    return true;
  // B / BL — tail-call or thunk
  if ((W & 0xFC000000) == 0x14000000 || // B
      (W & 0xFC000000) == 0x94000000)   // BL
    return true;
  // NOP (sometimes first instruction due to alignment)
  if (W == 0xD503201F)
    return true;
  return false;
}

// ===--------------------------------------------------------------------===//
// ARM 32-bit (Thumb-2 / ARM) prologue patterns
// ===--------------------------------------------------------------------===//

inline bool isStrictPrologueWordARM(uint32_t W, bool IsThumb) {
  if (IsThumb) {
    uint16_t HW = static_cast<uint16_t>(W & 0xFFFF);
    // PUSH {r4-r7, lr} variants — 0xB5xx
    if ((HW & 0xFF00) == 0xB500)
      return true;
    // PUSH {r4, ..., lr} wide: 0xE92D — STMDB sp!, {reglist}
    if (HW == 0xE92D)
      return true;
    // SUB sp, sp, #imm — 0xB0xx
    if ((HW & 0xFF80) == 0xB080)
      return true;
    // MOV r11, sp — for frame pointer setup
    if (HW == 0x466B)
      return true;
  } else {
    // STMFD sp!, {reglist} — ARM push (0xE92Dxxxx)
    if ((W & 0xFFFF0000) == 0xE92D0000)
      return true;
    // SUB sp, sp, #imm — 0xE24DDxxx
    if ((W & 0xFFFFF000) == 0xE24DD000)
      return true;
    // MOV r11, sp — 0xE1A0B00D
    if (W == 0xE1A0B00D)
      return true;
    // PUSH {r4, lr} — 0xE52DE004 (STR lr, [sp, #-4]!)
    if (W == 0xE52DE004)
      return true;
  }
  return false;
}

// ===--------------------------------------------------------------------===//
// Arch-dispatch: check first byte(s) at a candidate function start
// ===--------------------------------------------------------------------===//

/// Check if the first instruction byte(s) at \p Data look like a function
/// prologue for the given architecture.  \p Len is the number of available
/// bytes starting at \p Data.
inline bool isPrologueAt(const uint8_t *Data, size_t Len, Arch A) {
  if (A == Arch::X64 || A == Arch::X86) {
    if (Len < 1)
      return false;
    return isStrictPrologueByteX86(Data[0]);
  }
  if (A == Arch::AArch64) {
    if (Len < 4)
      return false;
    uint32_t W;
    std::memcpy(&W, Data, sizeof(W));
    return isStrictPrologueWordAArch64(W);
  }
  if (A == Arch::ARM) {
    if (Len < 2)
      return false;
    uint32_t W = 0;
    std::memcpy(&W, Data, (Len >= 4) ? 4 : 2);
    if (isStrictPrologueWordARM(W, /*IsThumb=*/true))
      return true;
    if (Len >= 4 && isStrictPrologueWordARM(W, /*IsThumb=*/false))
      return true;
    return false;
  }
  return false;
}

} // namespace neverd

#endif // NEVERD_SUPPORT_PROLOGUEPATTERNS_H
