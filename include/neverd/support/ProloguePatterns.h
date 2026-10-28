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
#include "neverd/support/ISAEncoding.h"

#include "llvm/Support/Endian.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <optional>

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

/// The register-or-memory operand a ModRM byte begins, in 32- and 64-bit
/// addressing.
struct X86ModRMOperand {
  uint8_t Mod = 0;
  uint8_t Reg = 0;
  uint8_t Rm = 0;
  /// Base register of a memory operand; none for an absolute or RIP-relative
  /// displacement.
  std::optional<uint8_t> Base;
  /// The memory operand adds a scaled index register.
  bool Indexed = false;
  int32_t Displacement = 0;
  /// Bytes from the ModRM byte through the displacement.
  size_t Length = 0;
};

/// Decode the ModRM operand at \p Data, or nullopt when its SIB byte or
/// displacement does not fit in \p Size bytes.
inline std::optional<X86ModRMOperand> decodeX86ModRMOperand(const uint8_t *Data,
                                                            size_t Size) {
  if (Size == 0)
    return std::nullopt;
  X86ModRMOperand Op;
  Op.Mod = Data[0] >> x86::kModRMModShift;
  Op.Reg = (Data[0] >> x86::kModRMRegShift) & x86::kModRMFieldMask;
  Op.Rm = Data[0] & x86::kModRMFieldMask;
  Op.Length = 1;
  if (Op.Mod == x86::kModRegister)
    return Op;
  size_t DispLen = Op.Mod == x86::kModMemoryDisp8    ? x86::kDisp8Len
                   : Op.Mod == x86::kModMemoryDisp32 ? x86::kDisp32Len
                                                     : 0;
  Op.Base = Op.Rm;
  if (Op.Rm == x86::kRmSIB) {
    if (Size < 2)
      return std::nullopt;
    const uint8_t Index =
        (Data[1] >> x86::kModRMRegShift) & x86::kModRMFieldMask;
    Op.Indexed = Index != x86::kRmSIB;
    Op.Base = Data[1] & x86::kModRMFieldMask;
    Op.Length = 2;
  }
  if (Op.Mod == x86::kModMemory && Op.Base == x86::kRmDisp32) {
    Op.Base.reset();
    DispLen = x86::kDisp32Len;
  }
  if (Size < Op.Length + DispLen)
    return std::nullopt;
  if (DispLen == x86::kDisp8Len)
    Op.Displacement = static_cast<int8_t>(Data[Op.Length]);
  else if (DispLen == x86::kDisp32Len)
    Op.Displacement =
        static_cast<int32_t>(static_cast<uint32_t>(Data[Op.Length]) |
                             static_cast<uint32_t>(Data[Op.Length + 1]) << 8 |
                             static_cast<uint32_t>(Data[Op.Length + 2]) << 16 |
                             static_cast<uint32_t>(Data[Op.Length + 3]) << 24);
  Op.Length += DispLen;
  return Op;
}

/// Length of the no-op at \p Data that an assembler or compiler emits to
/// align x86 code, or zero for anything else.  `nop` (90) and the multi-byte
/// `nop r/m` (0F 1F /0) may follow operand-size and CS-segment prefixes, as
/// in GNU as's and LLVM's `data16 cs nop WORD PTR [rax+rax*1+0x0]`.  32-bit
/// code also pads by copying a register onto itself (`mov esi, esi`,
/// `lea esi, [esi+0]`), which in 64-bit code would zero the upper half.
inline size_t x86AlignmentNopLength(const uint8_t *Data, size_t Size,
                                    bool Is64Bit) {
  const size_t Limit = std::min(Size, x86::kMaxInstructionLen);
  size_t Prefixes = 0;
  while (Prefixes < Limit && (Data[Prefixes] == x86::kOperandSizePrefix ||
                              Data[Prefixes] == x86::kCSSegmentPrefix))
    ++Prefixes;
  if (Prefixes == Limit)
    return 0;
  const uint8_t *Opcode = Data + Prefixes;
  const size_t Left = Limit - Prefixes;
  if (Opcode[0] == x86::kNop)
    return Prefixes + 1;
  if (Opcode[0] == x86::kTwoByteEscape) {
    if (Left < 2 || Opcode[1] != x86::kNopRmOp)
      return 0;
    const auto Operand = decodeX86ModRMOperand(Opcode + 2, Left - 2);
    return Operand && Operand->Reg == 0 ? Prefixes + 2 + Operand->Length : 0;
  }
  if (Is64Bit || Prefixes != 0)
    return 0;
  const auto Operand = decodeX86ModRMOperand(Opcode + 1, Left - 1);
  if (!Operand)
    return 0;
  const bool SelfCopy =
      ((Opcode[0] == x86::kMovRmFromRegOp ||
        Opcode[0] == x86::kMovRegFromRmOp) &&
       Operand->Mod == x86::kModRegister && Operand->Reg == Operand->Rm) ||
      (Opcode[0] == x86::kLeaOp && Operand->Mod != x86::kModRegister &&
       !Operand->Indexed && Operand->Base == Operand->Reg &&
       Operand->Displacement == 0);
  return SelfCopy ? 1 + Operand->Length : 0;
}

/// Length of a toolchain alignment no-op, or zero. ARM needs the actual
/// instruction mode; Thumb halfwords must not be read as ARM words.
inline size_t
alignmentNopLength(Arch A, const uint8_t *Data, size_t Size,
                   InstructionMode Mode = InstructionMode::Default) {
  switch (A) {
  case Arch::X86:
    return x86AlignmentNopLength(Data, Size, /*Is64Bit=*/false);
  case Arch::X64:
    return x86AlignmentNopLength(Data, Size, /*Is64Bit=*/true);
  case Arch::AArch64:
    return Size >= 4 && llvm::support::endian::read32le(Data) == aarch64::kNop
               ? 4
               : 0;
  case Arch::ARM:
    if (Mode == InstructionMode::Thumb) {
      if (Size >= 2) {
        const uint16_t Word = llvm::support::endian::read16le(Data);
        if (Word == arm::kThumbNop || Word == arm::kThumbLegacyNop)
          return 2;
      }
      return Size >= 4 &&
                     llvm::support::endian::read32le(Data) == arm::kThumbWideNop
                 ? 4
                 : 0;
    }
    if (Mode == InstructionMode::ARM && Size >= 4) {
      const uint32_t Word = llvm::support::endian::read32le(Data);
      // A predicate does not give a NOP any effect. Condition 0xf belongs
      // to a different encoding space and must not reuse this recognition.
      constexpr uint32_t BodyMask = 0x0fffffffu;
      const uint32_t Body = Word & BodyMask;
      if ((Word >> 28) != 0xf && (Body == (arm::kArmNop & BodyMask) ||
                                  Body == (arm::kArmHintNop & BodyMask)))
        return 4;
    }
    return 0;
  default:
    return 0;
  }
}

/// Length of the indirect-branch-tracking marker (`endbr64`, `endbr32`) a
/// function whose address the code takes starts with, or zero.
inline size_t x86BranchTargetMarkerLength(const uint8_t *Data, size_t Size) {
  // endbr64 is F3 0F 1E FA and endbr32 F3 0F 1E FB.
  constexpr uint8_t Marker[] = {0xF3, 0x0F, 0x1E};
  if (Size < 4 || !std::equal(std::begin(Marker), std::end(Marker), Data))
    return 0;
  return Data[3] == 0xFA || Data[3] == 0xFB ? 4 : 0;
}

/// Whether \p Byte is a trap a toolchain pads between \p A functions with, so
/// that it proves the code before it does not fall through: x86 `int3`.
inline bool isTrapPaddingByte(Arch A, uint8_t Byte) {
  return (A == Arch::X86 || A == Arch::X64) && Byte == x86::kInt3;
}

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

/// Whether the bytes before \p Off end an x86 function, so that \p Off can
/// begin the next one: `ret`, `ret imm16`, or INT3/NOP padding.  A byte
/// sequence cannot prove an instruction boundary, so this is evidence, not
/// proof, and callers pair it with a specific prologue.
inline bool isX86FunctionEndBefore(const uint8_t *Data, size_t Off) {
  if (Off >= 1) {
    const uint8_t Prev = Data[Off - 1];
    if (Prev == 0xC3 || Prev == x86::kInt3 || Prev == x86::kNop)
      return true;
  }
  // `ret imm16` is C2 iw; the last byte of the encoding is the immediate's
  // high byte, so the opcode is three bytes back.
  return Off >= 3 && Data[Off - 3] == 0xC2;
}

/// MSVC's hotpatchable x86 functions begin with `mov edi, edi` (8B FF), a
/// two-byte no-op the compiler emits only as a function's first instruction,
/// so that a patch can replace it with a short jump.  The no-op belongs to
/// the function: the push after it is the second instruction, never an entry
/// of its own.
///
/// The no-op followed by an EBP frame (`push ebp; mov ebp, esp`) begins a
/// function wherever it appears.  Followed only by a callee-saved push, it
/// does so after the end of another function: MSVC also pads with `8B FF`
/// in front of a jump table, whose first entry can begin with a push's byte.
inline bool isX86HotpatchEntryAt(const uint8_t *Data, size_t Size, size_t Off) {
  if (Off >= Size || Size - Off < 3 || Data[Off] != 0x8B ||
      Data[Off + 1] != 0xFF)
    return false;
  const uint8_t *Body = Data + Off + 2;
  const size_t BodyLen = Size - Off - 2;
  // push ebp; mov ebp, esp, in either encoding of the mov.
  if (Body[0] == 0x55)
    return BodyLen >= 3 && ((Body[1] == 0x8B && Body[2] == 0xEC) ||
                            (Body[1] == 0x89 && Body[2] == 0xE5));
  // push ebx, push esi or push edi.
  const bool CalleeSavedPush =
      Body[0] == 0x53 || Body[0] == 0x56 || Body[0] == 0x57;
  return CalleeSavedPush && isX86FunctionEndBefore(Data, Off);
}

// ===--------------------------------------------------------------------===//
// AArch64 prologue word patterns (first 32-bit instruction)
// ===--------------------------------------------------------------------===//

inline bool isStrictPrologueWordAArch64(uint32_t W) {
  // STP Xt1, Xt2, [sp, #imm]! — canonical frame setup
  // Encoding: 1010100110 iiiiiii ttttt 11111 ttttt  (STP pre-index)
  // The offset is signed and a push makes it negative, so imm7 is unmasked.
  if ((W & 0xFFC003E0) == 0xA98003E0) // STP pre-index to sp, any offset
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
  if (branch::A64BranchOrLink.matches(W))
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
    // 0x40-0x4F are REX prefixes only in 64-bit mode. IA-32 decodes them as
    // INC/DEC, which begin no function: such a byte after an int3 is more
    // often an operand's 0xCC, as in `mov [0x4040cc], eax`.
    if (A == Arch::X86 && (Data[0] & 0xF0) == 0x40)
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
