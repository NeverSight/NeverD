//===- CallAbiSafetyTests.cpp - Safety-facing ABI argument recovery ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/ir/med/MedABIPass.h"
#include "neverd/ir/med/MedIR.h"
#include "neverd/lift/X86Regs.h"
#include "neverd/loader/BinaryImageModel.h"

using namespace neverd;

namespace {

MedVar temp(int Id) {
  MedVar V;
  V.Kind = MedVar::Temp;
  V.Id = Id;
  V.Size = 8;
  V.TheArch = Arch::X64;
  return V;
}

MedVar mkReg(uint64_t Offset, int Version) {
  MedVar V;
  V.Kind = MedVar::Reg;
  V.Id = static_cast<int>(Offset);
  V.SSAVer = Version;
  V.Size = 8;
  V.RegOff = Offset;
  V.TheArch = Arch::X64;
  return V;
}

void copy(MedBlock &Block, MedVar Output, MedVar Input) {
  MedOp Op;
  Op.Opcode = NdOp::COPY;
  Op.Output = Output;
  Op.addInput(Input);
  Block.Ops.push_back(Op);
}

} // namespace

TEST(CallAbiSafety, Win64UsesCallSiteRegisterDefinitions) {
  constexpr va_t Target = 0x2000;

  MedFunc Func;
  Func.Entry = 0x1000;
  Func.Name = "caller";
  Func.CC = CallingConv::Win64;
  Func.Blocks.resize(1);
  Func.Blocks[0].Id = 0;

  copy(Func.Blocks[0], mkReg(x86reg::RCX, 1), temp(1));
  copy(Func.Blocks[0], mkReg(x86reg::RDX, 1), temp(2));
  copy(Func.Blocks[0], mkReg(x86reg::R8, 1), temp(3));
  MedOp Call;
  Call.Opcode = NdOp::CALL;
  Call.Output = mkReg(x86reg::RAX, 1);
  Call.addInput(MedVar::makeConst(Target, 8));
  Func.Blocks[0].Ops.push_back(Call);

  BinaryImage Img;
  Img.Arch = Arch::X64;
  Img.Format = BinaryFormat::COFF;
  Img.Imports.push_back({"runtime", "memcpy", 0, Target});
  const std::map<va_t, std::string> Names{{Target, "memcpy"}};
  recoverCallAbi(Func, Arch::X64, Names, &Img);

  ASSERT_EQ(Func.CallInfos.size(), 1u);
  ASSERT_EQ(Func.CallInfos[0].Args.size(), 3u);
  EXPECT_EQ(Func.CallInfos[0].Args[0], temp(1));
  EXPECT_EQ(Func.CallInfos[0].Args[1], temp(2));
  EXPECT_EQ(Func.CallInfos[0].Args[2], temp(3));
}
