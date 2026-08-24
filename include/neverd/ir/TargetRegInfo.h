//===- TargetRegInfo.h - Architecture register information -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Table-driven architecture register information for IR passes.
/// Centralizes all per-architecture register queries (frame registers,
/// parameter registers, callee-save registers, flag offsets, etc.)
/// to eliminate scattered if-else chains.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_IR_TARGETREGINFO_H
#define NEVERD_IR_TARGETREGINFO_H

#include "neverd/Common.h"
#include "neverd/ir/NdOps.h"

#include "llvm/ADT/ArrayRef.h"

#include <cstdint>
#include <utility>
#include <vector>

namespace neverd {

/// Architecture-independent condition codes for flag elimination.
/// Mirrors LLVM's ISD::CondCode but simplified for decompiler use.
enum class CondCode : uint8_t {
  EQ,  ///< Equal (ZF=1)
  NE,  ///< Not equal (ZF=0)
  SLT, ///< Signed less than
  SLE, ///< Signed less or equal
  SGT, ///< Signed greater than
  SGE, ///< Signed greater or equal
  ULT, ///< Unsigned less than
  ULE, ///< Unsigned less or equal
  UGT, ///< Unsigned greater than
  UGE, ///< Unsigned greater or equal
  VS,  ///< Signed overflow (V=1)
  Invalid
};

/// Return the inverse condition (e.g., EQ -> NE, SLT -> SGE).
CondCode invertCond(CondCode CC);

/// Return the NdOp comparison opcode for a given CondCode.
NdOp condToOpcode(CondCode CC);

/// Whether the condition requires swapped operands (b op a instead of a op b).
bool condSwapsOperands(CondCode CC);

/// Describes one sub-register relationship (e.g. RAX→AL, X0→W0).
struct SubRegEntry {
  uint64_t WideRegOff;
  uint16_t WideSize;
  uint64_t NarrowRegOff;
  uint16_t NarrowSize;
  uint16_t ByteOffset; ///< Byte offset of narrow within wide (0 for low, 1 for
                       ///< AH, etc.)
  bool WriteZeroExtends; ///< Writing narrow zero-extends to wide (true for
                         ///< EAX→RAX, W→X)
};

/// Function pointer type for architecture-specific register name lookup.
using RegNameFn = const char *(*)(uint64_t RegOff, uint16_t Size);

struct TargetRegisterRange {
  uint64_t Offset = 0;
  uint16_t Bytes = 0;
};

struct TargetRegInfo {
  Arch TheArch = Arch::Unknown;

  uint64_t StackPointer = 0;
  uint64_t FramePointer = 0;
  uint64_t LinkRegister = 0;

  uint16_t PointerSize = 8;
  uint16_t FullRegWidth = 8;

  llvm::ArrayRef<uint64_t> IntParamRegs;
  llvm::ArrayRef<uint64_t> Win64ParamRegs;
  llvm::ArrayRef<uint64_t> FPParamRegs;

  uint64_t IntReturnReg = 0;
  uint64_t IntReturnReg2 = 0;
  uint64_t FPReturnReg = 0;

  llvm::ArrayRef<uint64_t> CalleeSaveRegs;

  uint64_t FlagCF = 0;
  uint64_t FlagZF = 0;
  uint64_t FlagNF = 0;
  uint64_t FlagVF = 0;
  uint64_t FlagPF = 0;
  uint64_t FlagDF = 0;
  uint64_t FlagRangeStart = 0;
  uint64_t FlagRangeEnd = 0;

  uint64_t VecRegBase = 0;
  uint64_t VecRegStride = 0;
  unsigned VecRegCount = 0;

  /// What CF=1 means after CMP: ULT on x86 (borrow), UGE on ARM (carry).
  CondCode CfCondCode = CondCode::ULT;

  /// Architecture-specific register name function.
  RegNameFn GetRegName = nullptr;

  /// Sub-register relationship table.
  llvm::ArrayRef<SubRegEntry> SubRegs = {};

  /// Minimum instruction alignment in bytes (1 on x86, 2 on ARM/Thumb,
  /// 4 on AArch64).  Used to validate indirect-branch / jump-table targets.
  /// Set by getTargetRegInfo() alongside SubRegs (not in the const table).
  uint16_t MinInsnAlign = 1;

  /// The lifter models a floating-point return value in the integer return
  /// register (ARM/AArch64: V0/D0 are not the tracked variable), rather than
  /// in FPReturnReg (x86 returns FP in XMM0).  Set by getTargetRegInfo().
  bool ModelsFPReturnInIntReg = false;

  /// An auto-declared unknown external callee must use a variadic prototype so
  /// the backend never mislays arguments (true on ARM/AArch64 where variadic
  /// and non-variadic calling conventions differ).  Set by getTargetRegInfo().
  bool UnknownExternIsVarArg = false;

  /// Integer return registers in ABI order (x86-64 RAX,RDX; AArch64 X0,X1;
  /// i386 EAX,EDX; ARM R0,R1) and the floating-point/vector return registers in
  /// ABI order (x86-64 XMM0,XMM1; AArch64 V0..V3; none on i386/ARM32, which
  /// return FP through the x87 stack / soft-float pair).  Used by the multi-
  /// register struct-by-value return modeling.  Set by getTargetRegInfo().
  llvm::ArrayRef<uint64_t> IntReturnRegs = {};
  llvm::ArrayRef<uint64_t> FPReturnRegs = {};

  /// Check if (NarrowOff, NarrowSz) is a sub-register of (WideOff, WideSz).
  bool isSubRegOf(uint64_t NarrowOff, uint16_t NarrowSz, uint64_t WideOff,
                  uint16_t WideSz) const;

  /// Whether writing a register at (RegOff, Size) implicitly zero-extends
  /// to the full-width register (e.g. writing EAX zero-extends to RAX).
  bool writeZeroExtends(uint64_t RegOff, uint16_t Size) const;

  /// Return the byte offset of a narrow register within its containing wide
  /// register, or -1 if not a sub-register.
  int subRegByteOffset(uint64_t NarrowOff, uint16_t NarrowSz, uint64_t WideOff,
                       uint16_t WideSz) const;

  /// Find the containing full-width register for a given register.
  /// Returns (WideOff, WideSize) or (RegOff, Size) if no wider container.
  std::pair<uint64_t, uint16_t> findWideReg(uint64_t RegOff,
                                            uint16_t Size) const;

  bool isFrameReg(uint64_t RegOff) const {
    return RegOff == StackPointer || RegOff == FramePointer;
  }

  bool isFrameOrLinkReg(uint64_t RegOff) const {
    return isFrameReg(RegOff) || (LinkRegister != 0 && RegOff == LinkRegister);
  }

  bool isStackPointer(uint64_t RegOff) const { return RegOff == StackPointer; }
  bool isFramePointer(uint64_t RegOff) const { return RegOff == FramePointer; }
  bool isLinkRegister(uint64_t RegOff) const {
    return LinkRegister != 0 && RegOff == LinkRegister;
  }

  bool isFlag(uint64_t RegOff, uint16_t Size) const {
    return Size == 1 && RegOff >= FlagRangeStart && RegOff <= FlagRangeEnd;
  }

  bool isReturnReg(uint64_t RegOff) const {
    if (RegOff == IntReturnReg)
      return true;
    if (IntReturnReg2 != 0 && RegOff == IntReturnReg2)
      return true;
    if (FPReturnReg != 0 && RegOff == FPReturnReg)
      return true;
    return false;
  }

  bool isCalleeSaveReg(uint64_t RegOff) const {
    for (uint64_t R : CalleeSaveRegs)
      if (R == RegOff)
        return true;
    return false;
  }

  /// Whether a call preserves this exact register view.  This is usually an
  /// offset-only callee-save query, except for ABIs with partially preserved
  /// FP/vector register banks (AAPCS/AAPCS64).
  bool isCallPreserved(uint64_t RegOff, uint16_t Size) const;

  /// Number of low-order bytes preserved across a call for this register
  /// view.  Equal to Size for a fully callee-saved view and zero for a fully
  /// caller-saved view.  AAPCS64 v8-v15 are the only partial case: their low
  /// 8 bytes are preserved even when a wider Q-register view is used.
  uint16_t callPreservedPrefixSize(uint64_t RegOff, uint16_t Size) const;

  /// Every register byte range preserved by the platform calling convention.
  /// Includes the stack/frame pointers, partially preserved vector banks, and
  /// Win64's additional nonvolatile GPR/SIMD registers.
  std::vector<TargetRegisterRange>
  callPreservedRanges(BinaryFormat Format) const;

  bool isVectorReg(uint64_t RegOff) const {
    if (VecRegCount == 0)
      return false;
    return RegOff >= VecRegBase &&
           RegOff < VecRegBase + VecRegStride * VecRegCount &&
           (RegOff - VecRegBase) % VecRegStride == 0;
  }

  /// Whether \p RegOff names a register that can carry a floating-point
  /// argument.  Identical to isVectorReg on most targets, but ARM AAPCS-VFP
  /// also passes `float` arguments in the single-width S registers (s0,s1,...),
  /// where each odd S register is the high half of a D register: the D-register
  /// stride is 8 yet each S lane is 4 bytes, so those high-half lanes are not
  /// isVectorReg even though they are valid FP-argument registers.
  bool isFPArgReg(uint64_t RegOff) const {
    if (isVectorReg(RegOff))
      return true;
    if (TheArch == Arch::ARM && VecRegStride == 8 && !FPParamRegs.empty())
      return RegOff >= VecRegBase &&
             RegOff < VecRegBase + 4 * (2 * FPParamRegs.size()) &&
             ((RegOff - VecRegBase) % 4 == 0);
    return false;
  }

  /// Whether \p RegOff names an x87 floating-point stack register (st0..st7).
  /// x86/x86-64 only; always false on ARM/AArch64.  Used to recognize the i386
  /// cdecl convention that returns floating point through st0.
  bool isX87StackReg(uint64_t RegOff) const;

  /// Whether the architecture has a floating-point return register.
  bool hasFPReturnReg() const { return FPReturnReg != 0; }

  /// Register offset the LLVM emitter treats as carrying a floating-point
  /// return value: FPReturnReg on x86 (XMM0), but the integer return register
  /// on ARM/AArch64 where the lifter models the FP return there.
  uint64_t fpReturnModelReg() const {
    return ModelsFPReturnInIntReg ? IntReturnReg : FPReturnReg;
  }

  /// Map a single flag register to a CondCode.
  /// \p Inverted is true when the flag is wrapped in BOOL_NOT.
  CondCode singleFlagCond(uint64_t FlagOff, bool Inverted) const;

  /// Integer argument-register order for an image format. x86-64 COFF follows
  /// Win64 (RCX, RDX, R8, R9); other formats and architectures use the
  /// architecture's ordinary integer parameter order.
  llvm::ArrayRef<uint64_t> integerParamRegs(BinaryFormat Format) const;

  /// Map a register offset to a parameter index, or -1 if not a param reg.
  int regToArgIdx(uint64_t RegOff) const;

  /// Map a register offset to a parameter index for a specific calling
  /// convention. \p IsWin64 selects the Win64 register order on x86-64.
  int regToArgIdx(uint64_t RegOff, bool IsWin64) const;

  /// Check whether \p RegOff is a parameter register in any convention.
  bool isParamReg(uint64_t RegOff) const { return regToArgIdx(RegOff) >= 0; }

  /// Register offset that carries the hidden indirect-result (sret) pointer
  /// when a function returns a by-value aggregate too large for the return
  /// registers: AArch64 x8; 0 (none) on targets that return such aggregates
  /// through an ordinary argument register (x86-64 RDI, ARM r0).
  uint64_t indirectResultReg() const;
};

/// Return the register info table for the given architecture.
const TargetRegInfo &getTargetRegInfo(Arch TheArch);

} // namespace neverd

#endif // NEVERD_IR_TARGETREGINFO_H
