//===- TargetRegInfo.cpp - Architecture register information ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/ir/TargetRegInfo.h"

#include "TargetRegInfoDetail.h"

#include "neverd/lift/AArch64Regs.h"
#include "neverd/lift/ARMRegs.h"
#include "neverd/lift/X86Regs.h"

#include <algorithm>
#include <limits>

namespace neverd {

//===----------------------------------------------------------------------===//
// CondCode helpers
//===----------------------------------------------------------------------===//

CondCode invertCond(CondCode CC) {
  switch (CC) {
  case CondCode::EQ:
    return CondCode::NE;
  case CondCode::NE:
    return CondCode::EQ;
  case CondCode::SLT:
    return CondCode::SGE;
  case CondCode::SLE:
    return CondCode::SGT;
  case CondCode::SGT:
    return CondCode::SLE;
  case CondCode::SGE:
    return CondCode::SLT;
  case CondCode::ULT:
    return CondCode::UGE;
  case CondCode::ULE:
    return CondCode::UGT;
  case CondCode::UGT:
    return CondCode::ULE;
  case CondCode::UGE:
    return CondCode::ULT;
  default:
    return CondCode::Invalid;
  }
}

NdOp condToOpcode(CondCode CC) {
  switch (CC) {
  case CondCode::EQ:
    return NdOp::INT_EQUAL;
  case CondCode::NE:
    return NdOp::INT_NOTEQUAL;
  case CondCode::SLT:
  case CondCode::SGT:
    return NdOp::INT_SLESS;
  case CondCode::SLE:
  case CondCode::SGE:
    return NdOp::INT_SLESSEQUAL;
  case CondCode::ULT:
  case CondCode::UGT:
    return NdOp::INT_LESS;
  case CondCode::ULE:
  case CondCode::UGE:
    return NdOp::INT_LESSEQUAL;
  case CondCode::VS:
    return NdOp::INT_SOVF;
  default:
    return NdOp::NOP;
  }
}

bool condSwapsOperands(CondCode CC) {
  return CC == CondCode::SGT || CC == CondCode::SGE || CC == CondCode::UGT ||
         CC == CondCode::UGE;
}

CondCode TargetRegInfo::singleFlagCond(uint64_t FlagOff, bool Inverted) const {
  CondCode Base = CondCode::Invalid;
  if (FlagOff == FlagCF)
    Base = CfCondCode;
  else if (FlagOff == FlagZF)
    Base = CondCode::EQ;
  else if (FlagOff == FlagNF)
    Base = CondCode::SLT;
  else if (FlagOff == FlagVF)
    Base = CondCode::VS;
  else
    return CondCode::Invalid;
  return Inverted ? invertCond(Base) : Base;
}

//===----------------------------------------------------------------------===//
// Sub-register query implementations
//===----------------------------------------------------------------------===//

bool TargetRegInfo::isSubRegOf(uint64_t NarrowOff, uint16_t NarrowSz,
                               uint64_t WideOff, uint16_t WideSz) const {
  if (NarrowOff == WideOff && NarrowSz < WideSz)
    return true;
  for (const auto &E : SubRegs) {
    if (E.WideRegOff == WideOff && E.WideSize == WideSz &&
        E.NarrowRegOff == NarrowOff && E.NarrowSize == NarrowSz)
      return true;
  }
  return false;
}

bool TargetRegInfo::writeZeroExtends(uint64_t RegOff, uint16_t Size) const {
  const uint16_t MaxWidth = maxRegisterWidth(RegOff);
  for (const auto &E : SubRegs) {
    if (E.NarrowRegOff == RegOff && E.NarrowSize == Size &&
        E.WideSize <= MaxWidth && E.WriteZeroExtends)
      return true;
  }
  return false;
}

int TargetRegInfo::subRegByteOffset(uint64_t NarrowOff, uint16_t NarrowSz,
                                    uint64_t WideOff, uint16_t WideSz) const {
  if (NarrowOff == WideOff && NarrowSz < WideSz)
    return 0;
  for (const auto &E : SubRegs) {
    if (E.WideRegOff == WideOff && E.WideSize == WideSz &&
        E.NarrowRegOff == NarrowOff && E.NarrowSize == NarrowSz)
      return E.ByteOffset;
  }
  return -1;
}

std::pair<uint64_t, uint16_t> TargetRegInfo::findWideReg(uint64_t RegOff,
                                                         uint16_t Size) const {
  uint64_t BestOff = RegOff;
  uint16_t BestSz = Size;
  const uint16_t MaxWidth = maxRegisterWidth(RegOff);
  for (const auto &E : SubRegs) {
    if (E.NarrowRegOff == RegOff && E.NarrowSize == Size &&
        E.WideSize <= MaxWidth && E.WideSize > BestSz) {
      BestOff = E.WideRegOff;
      BestSz = E.WideSize;
    }
  }
  const bool MayUseLegacyFallback =
      GeneralRegs.empty() && RegOff % FullRegWidth == 0;
  if (BestSz == Size && Size < FullRegWidth &&
      (isGeneralReg(RegOff) || MayUseLegacyFallback)) {
    BestOff = RegOff;
    BestSz = FullRegWidth;
  }
  return {BestOff, BestSz};
}

//===----------------------------------------------------------------------===//
// Fallback
//===----------------------------------------------------------------------===//

static const TargetRegInfo UnknownRegInfo = {};

static void initSubRegs() {
  // The pipeline's first getTargetRegInfo() call comes from CFGBuilder inside
  // the parallel buildLowIR phase (nothing on the path before it — decoder init
  // and function detection — touches this), so several worker threads race to
  // initialize.  A plain `bool` flag let a losing thread see the flag already
  // set while the stores below were still in flight and read a zero
  // MinInsnAlign or an empty SubRegs table; function-local static
  // initialization blocks the other threads until the lambda completes.
  [[maybe_unused]] static const bool Initialized = [] {
    // NOTE: these RegInfo objects must be non-const.  Assigning SubRegs through
    // a const_cast on a genuinely `const` object is UB and the store may be
    // dropped under optimization, leaving SubRegs empty (which silently broke
    // high-byte registers AH/BH/CH/DH and other table-driven sub-reg lookups).
    initX86RegInfoTables();
    initAArch64RegInfoTables();
    initARMRegInfoTables();
    return true;
  }();
}

const TargetRegInfo &getTargetRegInfo(Arch TheArch) {
  initSubRegs();
  switch (TheArch) {
  case Arch::X64:
    return X64RegInfo;
  case Arch::X86:
    return X86RegInfo;
  case Arch::AArch64:
    return A64RegInfo;
  case Arch::ARM:
    return ARMRegInfo;
  default:
    return UnknownRegInfo;
  }
}

//===----------------------------------------------------------------------===//
// regToArgIdx
//===----------------------------------------------------------------------===//

bool TargetRegInfo::isX87StackReg(uint64_t RegOff) const {
  if (TheArch != Arch::X86 && TheArch != Arch::X64)
    return false;
  return RegOff >= x86reg::ST0 && RegOff <= x86reg::ST7 &&
         (RegOff - x86reg::ST0) % x86reg::FPURegStride == 0;
}

bool TargetRegInfo::isCallPreserved(uint64_t RegOff, uint16_t Size) const {
  if (isCalleeSaveReg(RegOff))
    return true;

  // AAPCS-VFP preserves d8-d15.  S-register views and aligned Q4-Q7 views are
  // preserved exactly when their complete byte range is inside that bank.
  if (TheArch == Arch::ARM) {
    constexpr uint64_t PreservedBegin = armreg::D(8);
    constexpr uint64_t PreservedEnd = armreg::D(16);
    return RegOff >= PreservedBegin && RegOff < PreservedEnd &&
           Size <= PreservedEnd - RegOff;
  }

  // AAPCS64 requires callees to preserve only the low 64 bits of v8-v15.
  // Treating the whole Q register as callee-saved would retain an upper half
  // the callee may overwrite; treating it as volatile loses valid D-register
  // values, which clang commonly uses to save HFA results across calls.
  if (TheArch != Arch::AArch64 || Size > 8 || !isVectorReg(RegOff))
    return false;
  unsigned VecIdx = static_cast<unsigned>((RegOff - VecRegBase) / VecRegStride);
  return VecIdx >= 8 && VecIdx <= 15;
}

uint16_t TargetRegInfo::callPreservedPrefixSize(uint64_t RegOff,
                                                uint16_t Size) const {
  if (isCallPreserved(RegOff, Size))
    return Size;
  if (TheArch != Arch::AArch64 || Size <= 8 || !isVectorReg(RegOff))
    return 0;
  unsigned VecIdx = static_cast<unsigned>((RegOff - VecRegBase) / VecRegStride);
  return VecIdx >= 8 && VecIdx <= 15 ? 8 : 0;
}

std::vector<TargetRegisterRange>
TargetRegInfo::callPreservedRanges(BinaryFormat Format) const {
  std::vector<TargetRegisterRange> Ranges;
  auto Add = [&](uint64_t Offset, uint16_t Bytes) {
    if (Bytes == 0)
      return;
    auto It = std::find_if(Ranges.begin(), Ranges.end(),
                           [Offset](const TargetRegisterRange &Range) {
                             return Range.Offset == Offset;
                           });
    if (It == Ranges.end())
      Ranges.push_back({Offset, Bytes});
    else
      It->Bytes = std::max(It->Bytes, Bytes);
  };

  Add(StackPointer, PointerSize);
  Add(FramePointer, PointerSize);
  for (uint64_t Reg : CalleeSaveRegs)
    Add(Reg, FullRegWidth);

  if (VecRegStride <= std::numeric_limits<uint16_t>::max()) {
    const auto VecBytes = static_cast<uint16_t>(VecRegStride);
    for (unsigned I = 0; I < VecRegCount; ++I) {
      const uint64_t Reg = VecRegBase + uint64_t(I) * VecRegStride;
      Add(Reg, callPreservedPrefixSize(Reg, VecBytes));
    }
  }

  if (TheArch == Arch::X64 && Format == BinaryFormat::COFF) {
    Add(x86reg::RSI, FullRegWidth);
    Add(x86reg::RDI, FullRegWidth);
    for (unsigned I = 6; I <= 15 && I < VecRegCount; ++I)
      Add(VecRegBase + uint64_t(I) * VecRegStride, 16);
  }

  return Ranges;
}

uint64_t TargetRegInfo::indirectResultReg() const {
  // AArch64 AAPCS returns a >16-byte aggregate through the buffer pointed to by
  // the indirect-result register x8.  x86-64 (RDI) and ARM (r0) use an ordinary
  // argument register, recovered through the normal argument path.
  return TheArch == Arch::AArch64 ? a64reg::X8 : 0;
}

llvm::ArrayRef<uint64_t>
TargetRegInfo::integerParamRegs(BinaryFormat Format) const {
  if (TheArch == Arch::X64 && Format == BinaryFormat::COFF &&
      !Win64ParamRegs.empty())
    return Win64ParamRegs;
  return IntParamRegs;
}

int TargetRegInfo::regToArgIdx(uint64_t RegOff) const {
  for (size_t I = 0; I < IntParamRegs.size(); ++I)
    if (IntParamRegs[I] == RegOff)
      return static_cast<int>(I);
  if (TheArch == Arch::AArch64 || TheArch == Arch::ARM) {
    // Stride-based: param regs are contiguous at FullRegWidth intervals
    if (RegOff <= IntParamRegs.back() && RegOff % FullRegWidth == 0)
      return static_cast<int>(RegOff / FullRegWidth);
  }
  return -1;
}

int TargetRegInfo::regToArgIdx(uint64_t RegOff, bool IsWin64) const {
  if (IsWin64 && !Win64ParamRegs.empty()) {
    for (size_t I = 0; I < Win64ParamRegs.size(); ++I)
      if (Win64ParamRegs[I] == RegOff)
        return static_cast<int>(I);
    for (size_t I = 0; I < FPParamRegs.size() && I < Win64ParamRegs.size(); ++I)
      if (FPParamRegs[I] == RegOff)
        return static_cast<int>(I);
    return -1;
  }
  int Idx = regToArgIdx(RegOff);
  if (Idx >= 0)
    return Idx;
  for (size_t I = 0; I < FPParamRegs.size(); ++I)
    if (FPParamRegs[I] == RegOff)
      return static_cast<int>(IntParamRegs.size() + I);
  return -1;
}

} // namespace neverd
