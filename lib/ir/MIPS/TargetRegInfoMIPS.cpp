//===- TargetRegInfoMIPS.cpp - MIPS integer register contracts -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Integer register and stack carriers for the admitted o32/n64 profiles.
///
//===----------------------------------------------------------------------===//

#include "../TargetRegInfoDetail.h"

#include "neverd/lift/MIPS/MIPSRegs.h"

#include <array>

namespace neverd {
namespace {

constexpr uint64_t O32Parameters[] = {mipsreg::r(4), mipsreg::r(5),
                                      mipsreg::r(6), mipsreg::r(7)};
constexpr uint64_t N64Parameters[] = {
    mipsreg::r(4), mipsreg::r(5), mipsreg::r(6),  mipsreg::r(7),
    mipsreg::r(8), mipsreg::r(9), mipsreg::r(10), mipsreg::r(11)};
constexpr uint64_t O32Saved[] = {mipsreg::r(16), mipsreg::r(17), mipsreg::r(18),
                                 mipsreg::r(19), mipsreg::r(20), mipsreg::r(21),
                                 mipsreg::r(22), mipsreg::r(23), mipsreg::FP};
constexpr uint64_t N64Saved[] = {mipsreg::r(16), mipsreg::r(17), mipsreg::r(18),
                                 mipsreg::r(19), mipsreg::r(20), mipsreg::r(21),
                                 mipsreg::r(22), mipsreg::r(23), mipsreg::GP,
                                 mipsreg::FP};
constexpr uint64_t ReturnRegisters[] = {mipsreg::V0, mipsreg::V1};
constexpr auto VectorViews = [] {
  std::array<SubRegEntry, mipsreg::VectorCount * 2> Result{};
  for (unsigned I = 0; I != mipsreg::VectorCount; ++I) {
    Result[I * 2] = {mipsreg::w(I), 16, mipsreg::w(I), 8, 0, false};
    Result[I * 2 + 1] = {mipsreg::w(I), 16, mipsreg::w(I), 4, 0, false};
  }
  return Result;
}();

} // namespace

const TargetRegInfo &getMIPSRegInfo(Arch Target) {
  static const auto Tables = [] {
    std::array<TargetRegInfo, 4> Result;
    constexpr Arch Targets[] = {Arch::MIPS32, Arch::MIPS32EL, Arch::MIPS64,
                                Arch::MIPS64EL};
    for (unsigned I = 0; I < Result.size(); ++I) {
      TargetRegInfo &Info = Result[I];
      const bool Wide = isMIPS64(Targets[I]);
      Info.TheArch = Targets[I];
      Info.StackPointer = mipsreg::SP;
      Info.FramePointer = mipsreg::FP;
      Info.LinkRegister = mipsreg::RA;
      Info.ProgramCounter = mipsreg::PC;
      Info.PointerSize = Info.FullRegWidth = Wide ? 8 : 4;
      Info.MinInsnAlign = 4;
      Info.VecRegBase = mipsreg::VectorBase;
      Info.VecRegStride = Info.VecRegWidth = mipsreg::VectorBytes;
      Info.VecRegCount = mipsreg::VectorCount;
      Info.SubRegs = VectorViews;
      Info.IntParamRegs =
          Wide ? llvm::ArrayRef(N64Parameters) : llvm::ArrayRef(O32Parameters);
      Info.IntReturnReg = mipsreg::V0;
      Info.IntReturnReg2 = mipsreg::V1;
      Info.IntegerReturnPairHighFirst =
          Targets[I] == Arch::MIPS32 || Targets[I] == Arch::MIPS64;
      Info.IntReturnRegs = ReturnRegisters;
      Info.CalleeSaveRegs =
          Wide ? llvm::ArrayRef(N64Saved) : llvm::ArrayRef(O32Saved);
      Info.GetRegName = Wide ? getMIPS64RegName : getMIPS32RegName;
      Info.CallStackArgumentBase = Wide ? 0 : 16;
    }
    return Result;
  }();
  switch (Target) {
  case Arch::MIPS32:
    return Tables[0];
  case Arch::MIPS32EL:
    return Tables[1];
  case Arch::MIPS64:
    return Tables[2];
  case Arch::MIPS64EL:
    return Tables[3];
  default:
    llvm_unreachable("MIPS table requested for another architecture");
  }
}

} // namespace neverd
