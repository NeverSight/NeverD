//===- TargetCodegenInfo.h - Architecture machine-code emission -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Centralizes per-architecture machine-code emission (NOP encoding, gap
/// padding, branch trampolines) behind one query point, mirroring
/// TargetRegInfo.  Generic codegen / pass code obtains a TargetCodegenInfo
/// via getTargetCodegenInfo(Arch, InstructionMode) and calls the
/// architecture-generic methods instead of switching on Arch and reaching
/// into the ISAEncoding constants.
///
/// Header-only so it is usable from every layer (loader, codegen, MIR passes)
/// without introducing link dependencies.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SUPPORT_TARGETCODEGENINFO_H
#define NEVERD_SUPPORT_TARGETCODEGENINFO_H

#include "neverd/Common.h"
#include "neverd/Support/BinaryEncoding.h"
#include "neverd/Support/ISAEncoding.h"

#include <cstdint>
#include <cstring>
#include <vector>

namespace neverd {

/// Architecture-specific machine-code emission helpers.  Query once via
/// getTargetCodegenInfo(Arch, InstructionMode) and call the methods below.
struct TargetCodegenInfo {
  Arch TheArch = Arch::Unknown;
  InstructionMode Mode = InstructionMode::Default;

  /// Append a single architectural NOP instruction to \p Code.
  /// Returns false for an unsupported architecture (nothing appended).
  bool appendNop(std::vector<uint8_t> &Code) const {
    switch (TheArch) {
    case Arch::X64:
    case Arch::X86:
      Code.push_back(x86::kNop);
      return true;
    case Arch::AArch64: {
      uint32_t Nop = aarch64::kNop;
      const auto *B = reinterpret_cast<const uint8_t *>(&Nop);
      Code.insert(Code.end(), B, B + aarch64::kInsnSize);
      return true;
    }
    case Arch::ARM: {
      if (Mode == InstructionMode::Thumb) {
        uint16_t Nop = arm::kThumbNop;
        const auto *B = reinterpret_cast<const uint8_t *>(&Nop);
        Code.insert(Code.end(), B, B + arm::kThumbInsnSize);
        return true;
      }
      uint32_t Nop = arm::kArmNop;
      const auto *B = reinterpret_cast<const uint8_t *>(&Nop);
      Code.insert(Code.end(), B, B + arm::kInsnSize);
      return true;
    }
    default:
      return false;
    }
  }

  /// Fill [Dst, Dst+Len) with inter-function padding: INT3 on x86 (traps on
  /// stray execution), NOP on AArch64, and the selected ARM-state NOP.
  void fillPadding(uint8_t *Dst, uint64_t Len) const {
    if (TheArch == Arch::AArch64) {
      uint32_t NopInsn = aarch64::kNop;
      for (uint64_t P = 0; P + aarch64::kInsnSize <= Len;
           P += aarch64::kInsnSize)
        std::memcpy(Dst + P, &NopInsn, aarch64::kInsnSize);
    } else if (TheArch == Arch::ARM) {
      if (Mode == InstructionMode::Thumb) {
        uint16_t ThumbNop = arm::kThumbNop;
        for (uint64_t P = 0; P + arm::kThumbInsnSize <= Len;
             P += arm::kThumbInsnSize)
          std::memcpy(Dst + P, &ThumbNop, arm::kThumbInsnSize);
      } else {
        uint32_t ArmNop = arm::kArmNop;
        for (uint64_t P = 0; P + arm::kInsnSize <= Len; P += arm::kInsnSize)
          std::memcpy(Dst + P, &ArmNop, arm::kInsnSize);
      }
    } else {
      std::memset(Dst, x86::kInt3, Len);
    }
  }

  uint64_t trampolineSize() const {
    if (TheArch == Arch::ARM && Mode == InstructionMode::Thumb)
      return arm::kThumbBWLen;
    if (TheArch == Arch::ARM)
      return arm::kLdrPCTrampLen;
    if (TheArch == Arch::AArch64)
      return aarch64::kInsnSize;
    if (TheArch == Arch::X64 || TheArch == Arch::X86)
      return x86::kJmpRel32Len;
    return 0;
  }

  /// Write a direct-branch trampoline into \p Data at byte offset \p FromOff
  /// (virtual address \p FromVA) targeting \p TargetVA.  Returns false if it
  /// does not fit or the displacement is out of branch range.
  bool writeTrampoline(std::vector<uint8_t> &Data, uint64_t FromOff,
                       uint64_t TargetVA, uint64_t FromVA,
                       uint64_t MaxOverwriteBytes = ~uint64_t(0)) const {
    uint64_t TrampolineSize = trampolineSize();
    if (TrampolineSize == 0 || MaxOverwriteBytes < TrampolineSize ||
        !rangeInBounds(FromOff, TrampolineSize, Data.size()))
      return false;
    if (TheArch == Arch::X64 || TheArch == Arch::X86) {
      int32_t Disp =
          static_cast<int32_t>(TargetVA - (FromVA + x86::kJmpRel32Len));
      Data[FromOff] = x86::kJmpRel32;
      writeLE<int32_t>(Data.data() + FromOff + 1, Disp);
      return true;
    }
    if (TheArch == Arch::AArch64) {
      int64_t Diff =
          static_cast<int64_t>(TargetVA) - static_cast<int64_t>(FromVA);
      if (Diff < -aarch64::kBranchRange || Diff >= aarch64::kBranchRange)
        return false;
      uint32_t Imm26 = static_cast<uint32_t>((Diff >> 2) & aarch64::kImm26Mask);
      writeLE<uint32_t>(Data.data() + FromOff, aarch64::kB_Op | Imm26);
      return true;
    }
    if (TheArch == Arch::ARM) {
      if (FromVA > uint64_t(UINT32_MAX) || TargetVA > uint64_t(UINT32_MAX))
        return false;
      if (Mode == InstructionMode::Thumb) {
        int64_t Diff = static_cast<int64_t>(TargetVA) -
                       (static_cast<int64_t>(FromVA) + 4);
        if ((FromVA & 1) || (TargetVA & 1) || (Diff & 1) ||
            Diff < arm::kThumbBWMinDisp || Diff > arm::kThumbBWMaxDisp)
          return false;
        uint32_t Imm25 = static_cast<uint32_t>(Diff) & 0x01ffffffu;
        uint32_t S = (Imm25 >> 24) & 1;
        uint32_t I1 = (Imm25 >> 23) & 1;
        uint32_t I2 = (Imm25 >> 22) & 1;
        uint32_t J1 = (~(I1 ^ S)) & 1;
        uint32_t J2 = (~(I2 ^ S)) & 1;
        uint16_t H1 = uint16_t(0xf000u | (S << 10) |
                               ((Imm25 >> 12) & 0x03ffu));
        uint16_t H2 = uint16_t(0x9000u | (J1 << 13) | (J2 << 11) |
                               ((Imm25 >> 1) & 0x07ffu));
        writeLE<uint16_t>(Data.data() + FromOff, H1);
        writeLE<uint16_t>(Data.data() + FromOff + 2, H2);
        return true;
      }
      writeLE<uint32_t>(Data.data() + FromOff, arm::kLdrPC);
      writeLE<uint32_t>(Data.data() + FromOff + arm::kInsnSize,
                        static_cast<uint32_t>(TargetVA));
      return true;
    }
    return false;
  }
};

/// Return the codegen info for a given architecture.
inline TargetCodegenInfo
getTargetCodegenInfo(Arch TheArch,
                     InstructionMode Mode = InstructionMode::Default) {
  return TargetCodegenInfo{TheArch, Mode};
}

} // namespace neverd

#endif // NEVERD_SUPPORT_TARGETCODEGENINFO_H
