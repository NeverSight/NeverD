//===- COFFUnwindARMPacked.cpp - ARM and ARM64 packed unwind data --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/loader/COFF/COFFUnwindARM.h"

#include "COFFUnwindARMDetail.h"

#include "llvm/ADT/bit.h"
#include "llvm/Support/ARMWinEH.h"

#include <cstdint>
#include <string>

namespace neverd::coff_loader {
namespace {

using arm_unwind_detail::DecodeBuilder;
using arm_unwind_detail::kARM64InstructionSize;
using arm_unwind_detail::kARMFramePointer;
using arm_unwind_detail::kFirstSavedFPReg;
using arm_unwind_detail::kFirstSavedIntReg;
using arm_unwind_detail::kFramePointer;
using arm_unwind_detail::kLinkRegister;
using arm_unwind_detail::setRegister;
using arm_unwind_detail::setRegisterPair;
using arm_unwind_detail::setRegisterRange;

} // namespace

ARMUnwindDecode expandARM64PackedUnwind(uint32_t PackedWord) {
  ARMUnwindDecode Result;
  DecodeBuilder Builder(Result);
  const llvm::ARM::WinEH::RuntimeFunctionARM64 RF(
      llvm::support::ulittle32_t(0), llvm::support::ulittle32_t(PackedWord));
  const llvm::ARM::WinEH::RuntimeFunctionFlag Flag = RF.Flag();
  if (Flag != llvm::ARM::WinEH::RuntimeFunctionFlag::RFF_Packed &&
      Flag != llvm::ARM::WinEH::RuntimeFunctionFlag::RFF_PackedFragment) {
    Builder.degrade(ExceptionParseStatus::Malformed,
                    "packed unwind expansion asked for a word that is not "
                    "packed unwind data");
    return Result;
  }

  const uint32_t RegI = RF.RegI();
  const uint32_t RegF = RF.RegF();
  const uint32_t CR = RF.CR();
  const bool HomesParameters = RF.H();
  const uint64_t FrameBytes = uint64_t(RF.FrameSize()) * 16;

  // The canonical frame is laid out as a fixed sequence of areas, and the
  // sizes below are what decide where each save lands.  `IntBytes` counts the
  // callee-saved integer area including the link register when the function
  // saves it without chaining; `FPBytes` the double area, which holds RegF+1
  // registers because a lone one cannot be encoded this way.
  uint64_t IntBytes = uint64_t(RegI) * 8;
  if (CR == 0x01)
    IntBytes += 8;
  uint64_t FPBytes = uint64_t(RegF) * 8;
  if (RegF != 0)
    FPBytes += 8;
  const uint64_t SavedBytes =
      (IntBytes + FPBytes + (HomesParameters ? 64 : 0) + 0xF) & ~uint64_t(0xF);
  if (SavedBytes > FrameBytes) {
    Builder.degrade(ExceptionParseStatus::Malformed,
                    "packed unwind data saves more than its frame holds");
    return Result;
  }
  const uint64_t LocalBytes = FrameBytes - SavedBytes;

  if (RegF == 1)
    Builder.degrade(ExceptionParseStatus::Partial,
                    "packed unwind data claims a single saved double "
                    "register, which the encoding cannot express");

  auto addOp = [&](UnwindOperationKind Kind) -> UnwindOperation & {
    // Packed data has no unwind-code array to point into, so the operations
    // are numbered by position rather than by an offset that does not exist.
    return Builder.add(Kind, static_cast<uint32_t>(Result.Operations.size()), 0,
                       kARM64InstructionSize);
  };

  // Step 1: sign the return address before anything can overwrite lr.
  if (CR == 0x02) {
    UnwindOperation &Op = addOp(UnwindOperationKind::SignReturnAddress);
    setRegister(Op, UnwindRegisterClass::GeneralPurpose, kLinkRegister);
  }

  // Steps 2 and 3: the callee-saved integer registers, and the link register
  // when it is saved alongside them rather than with the frame pointer.  An
  // odd count with CR==01 puts lr in the last pair instead of on its own.
  const bool LinkRegisterPairsWithLast = CR == 0x01 && (RegI % 2) == 1;
  uint64_t IntOffset = 0;
  for (uint32_t Saved = 0; Saved < RegI;) {
    const bool IsFirst = Saved == 0;
    const bool IsLast = Saved + 1 == RegI;
    const uint16_t First = static_cast<uint16_t>(kFirstSavedIntReg + Saved);
    const bool Paired = !IsLast || LinkRegisterPairsWithLast;
    UnwindOperationKind Kind;
    if (IsFirst)
      Kind = Paired ? UnwindOperationKind::SaveRegisterPairPreIndexed
                    : UnwindOperationKind::SaveRegisterPreIndexed;
    else
      Kind = Paired ? UnwindOperationKind::SaveRegisterPair
                    : UnwindOperationKind::SaveRegister;
    UnwindOperation &Op = addOp(Kind);
    if (Paired) {
      const uint16_t Second = IsLast ? kLinkRegister
                                     : static_cast<uint16_t>(First + 1);
      setRegisterPair(Op, UnwindRegisterClass::GeneralPurpose, First, Second);
      Saved += IsLast ? 1 : 2;
    } else {
      setRegister(Op, UnwindRegisterClass::GeneralPurpose, First);
      Saved += 1;
    }
    // The first save carries the whole allocation, so the registers land at
    // the bottom of the saved area and every later save is measured from it.
    Op.StackOffset = IsFirst ? SavedBytes : IntOffset;
    IntOffset += Paired ? 16 : 8;
  }
  if (CR == 0x01 && !LinkRegisterPairsWithLast) {
    UnwindOperationKind Kind = RegI == 0
                                   ? UnwindOperationKind::SaveRegisterPreIndexed
                                   : UnwindOperationKind::SaveRegister;
    UnwindOperation &Op = addOp(Kind);
    setRegister(Op, UnwindRegisterClass::GeneralPurpose, kLinkRegister);
    Op.StackOffset = RegI == 0 ? SavedBytes : IntBytes - 8;
  }

  // Step 4: the callee-saved double registers.  When nothing before them has
  // moved the stack pointer, the first of them carries the allocation.
  //
  // A zero RegF means no double register was saved at all, not one: the field
  // counts them from a base of two because the encoding has no room for the
  // single-register case, which is why it is excluded rather than rounded.
  const bool FPCarriesAllocation = RegI == 0 && CR != 0x01 && RegF != 0;
  const uint32_t SavedFPRegs = RegF == 0 ? 0 : RegF + 1;
  uint64_t FPOffset = IntBytes;
  for (uint32_t Saved = 0; Saved < SavedFPRegs;) {
    const bool IsFirst = Saved == 0;
    const bool IsLast = Saved + 1 == SavedFPRegs;
    const uint16_t First = static_cast<uint16_t>(kFirstSavedFPReg + Saved);
    UnwindOperationKind Kind;
    if (IsFirst && FPCarriesAllocation)
      Kind = IsLast ? UnwindOperationKind::SaveRegisterPreIndexed
                    : UnwindOperationKind::SaveRegisterPairPreIndexed;
    else
      Kind = IsLast ? UnwindOperationKind::SaveRegister
                    : UnwindOperationKind::SaveRegisterPair;
    UnwindOperation &Op = addOp(Kind);
    if (IsLast) {
      setRegister(Op, UnwindRegisterClass::FloatingPoint, First);
      Saved += 1;
    } else {
      setRegisterPair(Op, UnwindRegisterClass::FloatingPoint, First,
                      static_cast<uint16_t>(First + 1));
      Saved += 2;
    }
    Op.StackOffset =
        IsFirst && FPCarriesAllocation ? SavedBytes : FPOffset;
    FPOffset += IsLast ? 8 : 16;
  }

  // Step 5: homing the incoming integer parameters.  The native encoding
  // spells these as plain nops because the unwinder does not have to undo
  // them, but the stores are real and a frame reader needs to know where the
  // incoming arguments went.
  if (HomesParameters) {
    const uint64_t HomeBase = IntBytes + FPBytes;
    for (uint16_t Pair = 0; Pair < 4; ++Pair) {
      UnwindOperation &Op = addOp(UnwindOperationKind::SaveRegisterPair);
      setRegisterPair(Op, UnwindRegisterClass::GeneralPurpose,
                      static_cast<uint16_t>(Pair * 2),
                      static_cast<uint16_t>(Pair * 2 + 1));
      Op.StackOffset = HomeBase + uint64_t(Pair) * 16;
    }
  }

  // Step 6: the local area, and the frame chain when the function keeps one.
  // A chained frame puts `<x29,lr>` at the bottom of the locals so the chain
  // can be walked from x29 alone.
  const bool Chained = CR == 0x02 || CR == 0x03;
  auto addAllocation = [&](uint64_t Bytes) {
    UnwindOperation &Op = addOp(UnwindOperationKind::AllocateStack);
    Op.StackOffset = Bytes;
  };
  // A single `sub` reaches 4080 bytes; past that the prologue splits the
  // allocation in two rather than falling back to an `.xdata` record.
  constexpr uint64_t kMaxSingleAllocation = 4080;
  if (Chained) {
    if (LocalBytes <= 512) {
      UnwindOperation &Op =
          addOp(UnwindOperationKind::SaveRegisterPairPreIndexed);
      setRegisterPair(Op, UnwindRegisterClass::GeneralPurpose, kFramePointer,
                      kLinkRegister);
      Op.StackOffset = LocalBytes;
    } else {
      if (LocalBytes > kMaxSingleAllocation) {
        addAllocation(kMaxSingleAllocation);
        addAllocation(LocalBytes - kMaxSingleAllocation);
      } else {
        addAllocation(LocalBytes);
      }
      UnwindOperation &Op = addOp(UnwindOperationKind::SaveRegisterPair);
      setRegisterPair(Op, UnwindRegisterClass::GeneralPurpose, kFramePointer,
                      kLinkRegister);
      Op.StackOffset = 0;
    }
    UnwindOperation &Op = addOp(UnwindOperationKind::SetFramePointer);
    setRegister(Op, UnwindRegisterClass::GeneralPurpose, kFramePointer);
  } else if (LocalBytes > kMaxSingleAllocation) {
    addAllocation(kMaxSingleAllocation);
    addAllocation(LocalBytes - kMaxSingleAllocation);
  } else if (LocalBytes != 0) {
    addAllocation(LocalBytes);
  }

  return Result;
}

ARMUnwindDecode expandARM32PackedUnwind(uint32_t PackedWord) {
  ARMUnwindDecode Result;
  DecodeBuilder Builder(Result);
  const llvm::ARM::WinEH::RuntimeFunction RF(
      llvm::support::ulittle32_t(0), llvm::support::ulittle32_t(PackedWord));
  const llvm::ARM::WinEH::RuntimeFunctionFlag Flag = RF.Flag();
  if (Flag != llvm::ARM::WinEH::RuntimeFunctionFlag::RFF_Packed &&
      Flag != llvm::ARM::WinEH::RuntimeFunctionFlag::RFF_PackedFragment) {
    Builder.degrade(ExceptionParseStatus::Malformed,
                    "packed unwind expansion asked for a word that is not "
                    "packed unwind data");
    return Result;
  }

  // RuntimeFunction's convenience accessors assert the encoding's cross-field
  // restrictions. Input binaries are untrusted, so validate those raw bits
  // before calling RF.C(), RF.Ret(), or SavedRegisterMask().
  const bool SavesVFP = (PackedWord & 0x00080000u) != 0;
  const bool SavesLinkRegister = (PackedWord & 0x00100000u) != 0;
  const bool Chained = (PackedWord & 0x00200000u) != 0;
  const uint8_t Reg = static_cast<uint8_t>((PackedWord >> 16) & 0x7u);
  const uint8_t Return = static_cast<uint8_t>((PackedWord >> 13) & 0x3u);

  // The encoding forbids two combinations outright, and a record that uses one
  // was not produced by a conforming toolchain -- reading it as if it were
  // would report a frame the function does not build.
  if (Chained && !SavesLinkRegister) {
    Builder.degrade(ExceptionParseStatus::Malformed,
                    "chained ARM frame does not save the link register");
    return Result;
  }
  if (Chained && Reg >= 7 && !SavesVFP) {
    Builder.degrade(ExceptionParseStatus::Malformed,
                    "chained ARM frame explicitly saves r11");
    return Result;
  }
  if (Return == static_cast<uint8_t>(llvm::ARM::WinEH::ReturnType::RT_POP) &&
      !SavesLinkRegister) {
    Builder.degrade(ExceptionParseStatus::Malformed,
                    "ARM frame returns by popping pc without saving the link "
                    "register");
    return Result;
  }

  const bool PrologueFolded = llvm::ARM::WinEH::PrologueFolding(RF);

  auto addOp = [&](UnwindOperationKind Kind,
                   uint8_t InstructionSize) -> UnwindOperation & {
    return Builder.add(Kind, static_cast<uint32_t>(Result.Operations.size()), 0,
                       InstructionSize);
  };

  // Step 1: home the incoming integer parameters.
  if (RF.H()) {
    UnwindOperation &Op =
        addOp(UnwindOperationKind::SaveRegisterPairPreIndexed, 2);
    setRegisterRange(Op, UnwindRegisterClass::GeneralPurpose, 0, 3);
    Op.StackOffset = 16;
  }

  // Step 2: the register push.  Which registers it covers is decided by four
  // interacting flags, and LLVM already derives exactly the mask the runtime
  // uses, including the registers a folded stack adjustment absorbs into the
  // push.
  const auto [GPRMask, VFPMask] =
      llvm::ARM::WinEH::SavedRegisterMask(RF, /*Prologue=*/true);
  if (GPRMask != 0) {
    UnwindOperation &Op =
        addOp(UnwindOperationKind::SaveRegisterPairPreIndexed, 4);
    Op.RegisterClass = UnwindRegisterClass::GeneralPurpose;
    Op.RegisterMask = GPRMask;
    Op.Register = static_cast<uint16_t>(llvm::countr_zero(uint32_t(GPRMask)));
    Op.StackOffset = uint64_t(llvm::popcount(uint32_t(GPRMask))) * 4;
  }

  // Step 3: establish the frame chain.  A 16-bit `mov` suffices when nothing
  // but r11 and lr was pushed; otherwise r11 sits above other saves and the
  // prologue has to add its displacement.
  if (Chained) {
    const bool NeedsDisplacement = !SavesVFP || PrologueFolded;
    UnwindOperation &Op =
        addOp(NeedsDisplacement ? UnwindOperationKind::AddFramePointer
                                : UnwindOperationKind::SetFramePointer,
              NeedsDisplacement ? 4 : 2);
    setRegister(Op, UnwindRegisterClass::GeneralPurpose, kARMFramePointer);
    // r11 is pushed below the link register, so the displacement is whatever
    // was pushed above it.
    if (NeedsDisplacement && GPRMask != 0) {
      const uint32_t Above =
          uint32_t(GPRMask) & ~((uint32_t(1) << (kARMFramePointer + 1)) - 1);
      Op.StackOffset = uint64_t(llvm::popcount(Above)) * 4;
    }
  }

  // Step 4: the double registers, which are always pushed after the integer
  // ones and never absorb a stack adjustment.
  if (VFPMask != 0) {
    UnwindOperation &Op =
        addOp(UnwindOperationKind::SaveRegisterPairPreIndexed, 4);
    Op.RegisterClass = UnwindRegisterClass::FloatingPoint;
    Op.RegisterMask = VFPMask;
    Op.Register = static_cast<uint16_t>(llvm::countr_zero(VFPMask));
    Op.StackOffset = uint64_t(llvm::popcount(VFPMask)) * 8;
  } else if (SavesVFP && Reg != 7) {
    Builder.degrade(ExceptionParseStatus::Partial,
                    "packed unwind data selects the double register file but "
                    "names no registers");
  }

  // Step 5: the explicit stack adjustment, present only when the push did not
  // already absorb it.
  const uint64_t AdjustWords = llvm::ARM::WinEH::StackAdjustment(RF);
  if (AdjustWords != 0 && !PrologueFolded) {
    // The 16-bit `sub sp` form reaches 508 bytes; past that the prologue uses
    // the 32-bit `subw`.
    const uint64_t Bytes = AdjustWords * 4;
    UnwindOperation &Op =
        addOp(UnwindOperationKind::AllocateStack, Bytes <= 508 ? 2 : 4);
    Op.StackOffset = Bytes;
  }

  return Result;
}

} // namespace neverd::coff_loader
