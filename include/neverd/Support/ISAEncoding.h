//===- ISAEncoding.h - ISA instruction encoding constants -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Instruction encoding constants for x86, AArch64, and ARM.  Used by both
/// loader (IAT thunk scanning, prologue detection) and codegen (trampoline
/// writing, relocation patching) layers.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SUPPORT_ISAENCODING_H
#define NEVERD_SUPPORT_ISAENCODING_H

#include "neverd/Common.h"

#include "llvm/Support/Win64EH.h"

#include <cstddef>
#include <cstdint>

namespace neverd {

// ===--------------------------------------------------------------------===//
// x86/x64 instruction encoding
// ===--------------------------------------------------------------------===//

namespace x86 {

// --- Opcode bytes ---
constexpr uint8_t kCallRel32 = 0xE8;
constexpr uint8_t kJmpRel32 = 0xE9;
constexpr uint8_t kJccShortBase = 0x70;
constexpr uint8_t kJccShortEnd = 0x7F;
constexpr uint8_t kTwoByteEscape = 0x0F;
constexpr uint8_t kJccNearBase = 0x80;
constexpr uint8_t kJccNearEnd = 0x8F;
constexpr uint8_t kInt3 = 0xCC;
constexpr uint8_t kNop = 0x90;
constexpr uint8_t kJmpIndirectOp = 0xFF;
constexpr uint8_t kJmpIndirectModRM = 0x25;

// --- Instruction lengths ---
constexpr size_t kRel32DispOffset = 1;
constexpr size_t kJmpRel32Len = 5;
constexpr size_t kCallRel32Len = 5;
constexpr size_t kJccShortLen = 2;
constexpr size_t kJccShortDispOffset = 1;
constexpr size_t kJccNearLen = 6;
constexpr size_t kJccNearDispOffset = 2;
constexpr size_t kJmpIndirectLen = 6;
constexpr size_t kJmpIndirectDispOffset = 2;

} // namespace x86

// ===--------------------------------------------------------------------===//
// AArch64 instruction encoding
// ===--------------------------------------------------------------------===//

namespace aarch64 {

constexpr size_t kInsnSize = 4;

// --- Branch encoding (B / BL) ---
constexpr uint32_t kB_Mask = 0xFC000000;
constexpr uint32_t kB_Op = 0x14000000;
constexpr uint32_t kBL_Op = 0x94000000;
constexpr uint32_t kImm26Mask = 0x03FFFFFF;
constexpr uint32_t kImm26Sign = 0x02000000;
constexpr uint32_t kImm26SignExt = 0xFC000000;

// --- Conditional branch (B.cond) ---
constexpr uint32_t kBCond_Mask = 0xFF000010;
constexpr uint32_t kBCond_Op = 0x54000000;
constexpr uint32_t kBCond_Imm19Shift = 5;
constexpr uint32_t kBCond_Imm19Mask = 0x7FFFF;
constexpr uint32_t kBCond_Imm19Sign = 0x40000;
constexpr uint32_t kBCond_Imm19SignExt = 0xFFF80000;

// --- Compare and branch (CBZ / CBNZ) ---
constexpr uint32_t kCBZ_Mask = 0x7E000000;
constexpr uint32_t kCBZ_Op = 0x34000000;

// --- Test and branch (TBZ / TBNZ) ---
constexpr uint32_t kTBZ_Mask = 0x7E000000;
constexpr uint32_t kTBZ_Op = 0x36000000;
constexpr uint32_t kTBZ_Imm14Shift = 5;
constexpr uint32_t kTBZ_Imm14Mask = 0x3FFF;
constexpr uint32_t kTBZ_Imm14Sign = 0x2000;
constexpr uint32_t kTBZ_Imm14SignExt = 0xFFFFC000;

// --- PC-relative addressing (ADRP / ADR) ---
constexpr uint32_t kADRP_Mask = 0x9F000000;
constexpr uint32_t kADRP_Op = 0x90000000;
constexpr uint32_t kADR_Op = 0x10000000;
constexpr uint32_t kImmHiShift = 5;
constexpr uint32_t kImmHiMask = 0x7FFFF;
constexpr uint32_t kImmLoShift = 29;
constexpr uint32_t kImmLoMask = 0x3;
constexpr uint32_t kImm21Sign = 0x100000;
constexpr uint32_t kImm21SignExt = 0xFFE00000;
constexpr uint32_t kRdMask = 0x1F;
constexpr uint32_t kADRP_FixedBits = kADRP_Mask | kRdMask;

// --- ADRP page arithmetic ---
constexpr uint32_t kPageShift = 12;
constexpr uint32_t kImmLoWidth = 2;
constexpr uint32_t kADRP_ImmBits = 32;

// --- Load/store immediate ---
constexpr uint32_t kLDR_Imm12Shift = 10;
constexpr uint32_t kLDR_Imm12Mask = 0xFFF;
constexpr uint64_t kPageMask = ~uint64_t(0xFFF);

// --- IAT thunk pattern: ADRP x16 / LDR x16,[x16,...] / BR x16 ---
constexpr uint32_t kADRP_X16_Mask = 0x9F00001F;
constexpr uint32_t kADRP_X16_Match = 0x90000010;
constexpr uint32_t kLDR_X16_X16_Mask = 0xFFC003FF;
constexpr uint32_t kLDR_X16_X16_Match = 0xF9400210;
constexpr size_t kThunkLen = 12;

// --- Miscellaneous ---
constexpr uint32_t kNop = 0xD503201F;
constexpr uint32_t kBR_X16 = 0xD61F0200;

/// +/- 128 MB reach of B/BL (26-bit signed imm << 2).
constexpr int64_t kBranchRange = 1LL << 27;

} // namespace aarch64

// ===--------------------------------------------------------------------===//
// ELF PLT layout
// ===--------------------------------------------------------------------===//

namespace elf {
constexpr uint32_t kDefaultPLTEntrySize = 16;
constexpr uint32_t kARMPLTEntrySize = 12;
constexpr uint32_t kAArch64PLTEntrySize = 16;
constexpr uint32_t kX86PLTEntrySize = 16;

inline uint32_t getPLTEntrySize(Arch A) {
  switch (A) {
  case Arch::ARM:
    return kARMPLTEntrySize;
  case Arch::AArch64:
    return kAArch64PLTEntrySize;
  default:
    return kDefaultPLTEntrySize;
  }
}
} // namespace elf

// ===--------------------------------------------------------------------===//
// Mach-O stub layout
// ===--------------------------------------------------------------------===//

namespace macho {
constexpr uint32_t kX86StubSize = 6;
constexpr uint32_t kAArch64StubSize = 12;
constexpr uint32_t kARMStubSize = 12;
constexpr uint32_t kDefaultStubSize = 6;

inline uint32_t getStubSize(Arch A) {
  switch (A) {
  case Arch::AArch64:
    return kAArch64StubSize;
  case Arch::ARM:
    return kARMStubSize;
  default:
    return kDefaultStubSize;
  }
}
} // namespace macho

// ===--------------------------------------------------------------------===//
// ARM 32-bit instruction encoding
// ===--------------------------------------------------------------------===//

namespace arm {
constexpr uint32_t kLdrPC = 0xE51FF004u;
constexpr uint32_t kBL_Op = 0xEB000000u;
constexpr uint32_t kImm24Mask = 0x00FFFFFFu;
constexpr uint32_t kImm24Sign = 0x00800000u;
constexpr uint32_t kImm24SignExt = 0xFF000000u;
constexpr uint32_t kArmNop = 0xE1A00000u; // MOV r0, r0
constexpr uint16_t kThumbNop = 0xBF00;
constexpr uint32_t kPCBias = 8;
constexpr size_t kInsnSize = 4;
constexpr size_t kThumbInsnSize = 2;
constexpr size_t kLdrPCTrampLen = 8;
constexpr size_t kThumbMovImmInsnSize = 4;
constexpr size_t kThumbImportThunkLen = 12;
constexpr uint16_t kThumbMovImmOp1Mask = 0xFBF0;
constexpr uint16_t kThumbMovWIPOp1 = 0xF240;
constexpr uint16_t kThumbMovTIPOp1 = 0xF2C0;
constexpr uint16_t kThumbMovIPOp2Mask = 0x8F00;
constexpr uint16_t kThumbMovIPOp2 = 0x0C00;
constexpr uint32_t kThumbLdrPCFromIP = 0xF000F8DC;
constexpr uint64_t kThumbBWLen = 4;
constexpr int64_t kThumbBWMinDisp = -(int64_t(1) << 24);
constexpr int64_t kThumbBWMaxDisp = (int64_t(1) << 24) - 2;

constexpr uint32_t kBranchClassMask = 0x0E000000u;
constexpr uint32_t kBranchClassVal = 0x0A000000u;
constexpr uint32_t kBranchLinkBit = 0x01000000u;
constexpr uint32_t kCondShift = 28;
constexpr uint32_t kCondMask = 0xF;
constexpr uint32_t kCondNV = 0xF;

inline bool isBranch(uint32_t Insn) {
  uint32_t Cond = (Insn >> kCondShift) & kCondMask;
  return (Insn & kBranchClassMask) == kBranchClassVal && Cond != kCondNV;
}

inline bool isBranchLink(uint32_t Insn) {
  return isBranch(Insn) && (Insn & kBranchLinkBit);
}

inline int32_t decodeBranchImm24(uint32_t Insn) {
  int32_t Imm = Insn & kImm24Mask;
  if (Imm & kImm24Sign)
    Imm |= static_cast<int32_t>(kImm24SignExt);
  return Imm;
}
} // namespace arm

// ===--------------------------------------------------------------------===//
// PE x64 unwind info layout (cf. llvm/Support/Win64EH.h)
// ===--------------------------------------------------------------------===//

namespace unwind {
constexpr uint8_t kFlagsShift = 3;
constexpr uint8_t kFlagsMask = 0x1F;
using llvm::Win64EH::UNW_ChainInfo;
} // namespace unwind

} // namespace neverd

#endif // NEVERD_SUPPORT_ISAENCODING_H
