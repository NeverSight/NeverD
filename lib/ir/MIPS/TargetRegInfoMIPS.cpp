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

#include "neverd/ArchSupport.h"
#include "neverd/lift/MIPS/MIPSRegs.h"

#include <array>

namespace neverd {
namespace {

constexpr uint64_t O32Parameters[] = {mipsreg::r(4), mipsreg::r(5),
                                      mipsreg::r(6), mipsreg::r(7)};
constexpr uint64_t N64Parameters[] = {
    mipsreg::r(4), mipsreg::r(5), mipsreg::r(6),  mipsreg::r(7),
    mipsreg::r(8), mipsreg::r(9), mipsreg::r(10), mipsreg::r(11)};
constexpr uint64_t O32FPParameters[] = {mipsreg::w(12), mipsreg::w(14)};
constexpr uint64_t N64FPParameters[] = {
    mipsreg::w(12), mipsreg::w(13), mipsreg::w(14), mipsreg::w(15),
    mipsreg::w(16), mipsreg::w(17), mipsreg::w(18), mipsreg::w(19)};
constexpr uint64_t FPReturnRegisters[] = {mipsreg::w(0), mipsreg::w(2)};
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
    constexpr Arch Targets[] = {
#define NEVERD_MIPS_TARGET(Name, Spelling, Triple) Arch::Name,
#include "neverd/MIPSTargets.def"
    };
    std::array<TargetRegInfo, std::size(Targets)> Result;
    for (unsigned I = 0; I < Result.size(); ++I) {
      TargetRegInfo &Info = Result[I];
      const bool O32 = isMIPSO32(Targets[I]);
      Info.TheArch = Targets[I];
      Info.StackPointer = mipsreg::SP;
      Info.FramePointer = mipsreg::FP;
      Info.LinkRegister = mipsreg::RA;
      Info.ProgramCounter = mipsreg::PC;
      Info.FullRegWidth = isMIPS64(Targets[I]) ? 8 : 4;
      Info.PointerSize = isMIPSN64(Targets[I]) ? 8 : 4;
      Info.MinInsnAlign = 4;
      Info.VecRegBase = mipsreg::VectorBase;
      Info.VecRegStride = Info.VecRegWidth = mipsreg::VectorBytes;
      Info.VecRegCount = mipsreg::VectorCount;
      Info.SubRegs = VectorViews;
      Info.IntParamRegs =
          O32 ? llvm::ArrayRef(O32Parameters) : llvm::ArrayRef(N64Parameters);
      Info.FPParamRegs = O32 ? llvm::ArrayRef(O32FPParameters)
                             : llvm::ArrayRef(N64FPParameters);
      Info.FPReturnReg = mipsreg::w(0);
      Info.FPReturnRegs = FPReturnRegisters;
      Info.FPABIRegWidth = 8;
      Info.IntReturnReg = mipsreg::V0;
      Info.IntReturnReg2 = mipsreg::V1;
      Info.IntegerReturnPairHighFirst = !archLittleEndian(Targets[I]);
      Info.IntReturnRegs = ReturnRegisters;
      Info.CalleeSaveRegs =
          O32 ? llvm::ArrayRef(O32Saved) : llvm::ArrayRef(N64Saved);
      Info.GetRegName = isMIPS64O32(Targets[I]) ? getMIPS64O32RegName
                        : O32                   ? getMIPS32RegName
                                                : getMIPS64RegName;
      Info.CallStackArgumentBase = O32 ? 16 : 0;
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
  case Arch::MIPS64N32:
    return Tables[4];
  case Arch::MIPS64N32EL:
    return Tables[5];
  case Arch::MIPS64O32:
    return Tables[6];
  case Arch::MIPS64O32EL:
    return Tables[7];
  default:
    llvm_unreachable("MIPS table requested for another architecture");
  }
}

} // namespace neverd
