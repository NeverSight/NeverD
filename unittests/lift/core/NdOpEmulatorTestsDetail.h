//===- NdOpEmulatorTestsDetail.h - NdOp emulator test harness ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// The dummy image, the LowOp builders and the fixture shared by the
// NdOpEmulator* translation units.
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_UNITTESTS_LIFT_CORE_NDOPEMULATORTESTSDETAIL_H
#define NEVERD_UNITTESTS_LIFT_CORE_NDOPEMULATORTESTSDETAIL_H

#include "neverd/ir/low/NdOpEmulator.h"
#include "neverd/ir/NdOps.h"
#include "neverd/Common.h"
#include "neverd/loader/BinaryImage.h"

#include <gtest/gtest.h>
#include <cstring>
#include <vector>

namespace neverd::ndop_emulator_test {

inline BinaryImage makeDummyImage() {
  BinaryImage Img;
  Img.Arch = Arch::X64;
  Img.Format = BinaryFormat::ELF;
  Segment Seg;
  Seg.Name = section_names::elf::Text;
  Seg.VA = 0x1000;
  Seg.Size = 256;
  Seg.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Seg.Data.resize(256, 0);
  Img.Segments.push_back(std::move(Seg));
  return Img;
}

inline LowOp makeArith(NdOp Opcode, uint64_t OutReg, uint64_t InReg,
                        uint64_t Const) {
  LowOp Op;
  Op.Opcode = Opcode;
  Op.Output = NdVar::reg(OutReg, 8);
  Op.addInput(NdVar::reg(InReg, 8));
  Op.addInput(NdVar::cst(Const, 8));
  return Op;
}

inline LowOp makeCopy(uint64_t OutReg, uint64_t InReg) {
  LowOp Op;
  Op.Opcode = NdOp::COPY;
  Op.Output = NdVar::reg(OutReg, 8);
  Op.addInput(NdVar::reg(InReg, 8));
  return Op;
}

inline LowOp makeBranchInd(uint64_t InReg) {
  LowOp Op;
  Op.Opcode = NdOp::INDIR_BR;
  Op.Output = {};
  Op.addInput(NdVar::reg(InReg, 8));
  return Op;
}

class NdOpEmulatorTest : public ::testing::Test {
protected:
  BinaryImage Img = makeDummyImage();
};

} // namespace neverd::ndop_emulator_test

#endif // NEVERD_UNITTESTS_LIFT_CORE_NDOPEMULATORTESTSDETAIL_H
