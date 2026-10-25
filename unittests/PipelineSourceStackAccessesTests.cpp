//===- PipelineSourceStackAccessesTests.cpp - Stack projection -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Source and machine conversions retain distinct stack-access contracts,
/// including when return recovery reconverts a caller.
///
//===----------------------------------------------------------------------===//

#include "PipelineSourceStackAccesses.h"
#include "gtest/gtest.h"

#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/low/LowIR.h"
#include "neverd/lift/ARM/ARMRegs.h"
#include "neverd/lift/X86/X86Regs.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/pipeline/Pipeline.h"

#include <initializer_list>
#include <utility>

namespace {

using namespace neverd;

MedFunc savedCallerRegister(Arch Architecture, bool ReadsRestore) {
  const auto &TRI = getTargetRegInfo(Architecture);
  auto Reg = [&](int Id, uint64_t Offset, int Version) {
    MedVar V;
    V.Kind = MedVar::Reg;
    V.TheArch = Architecture;
    V.Id = Id;
    V.RegOff = Offset;
    V.SSAVer = Version;
    V.Size = TRI.PointerSize;
    return V;
  };
  auto Op = [](NdOp Code, MedVar Output, std::initializer_list<MedVar> Inputs) {
    MedOp O;
    O.Opcode = Code;
    O.Output = Output;
    for (const auto &V : Inputs)
      O.addInput(V);
    return O;
  };
  const auto SP = Reg(1, TRI.StackPointer, 0);
  const auto Frame = Reg(1, TRI.StackPointer, 1);
  const auto Offset = Architecture == Arch::ARM ? armreg::R8 : x86reg::RBX;
  const auto EntryValue = Reg(2, Offset, 0);
  const auto Restored = Reg(2, Offset, 1);
  auto ReturnAddress = Reg(3, 0, 1);
  ReturnAddress.Kind = MedVar::Temp;
  MedVar None;
  None.Id = -1;
  MedBlock Block;
  Block.Id = 0;
  Block.StartAddr = 0x1000;
  Block.Ops = {
      Op(NdOp::COPY, SP, {SP}),
      Op(NdOp::COPY, EntryValue, {EntryValue}),
      Op(NdOp::INT_SUB, Frame,
         {SP, MedVar::makeConst(TRI.PointerSize, TRI.PointerSize)}),
      Op(NdOp::STORE, None, {Frame, EntryValue}),
      Op(NdOp::LOAD, Restored, {Frame}),
      Op(NdOp::LOAD, ReturnAddress, {SP}),
      Op(NdOp::RETURN, None,
         {ReadsRestore ? Restored : MedVar::makeConst(0, TRI.PointerSize)})};
  MedFunc F;
  F.Entry = Block.StartAddr;
  F.CC = Architecture == Arch::ARM ? CallingConv::ARM_AAPCS
                                   : CallingConv::SysV_AMD64;
  F.FrameSize = TRI.PointerSize;
  F.Blocks.push_back(std::move(Block));
  return F;
}

TEST(PipelineSourceStackAccesses, MachineRequestsKeepCallerStateOperations) {
  struct Case {
    bool Lift;
    bool Patch;
    bool Analysis;
    bool Projection;
    bool Source;
  };
  for (const auto Architecture : {Arch::ARM, Arch::X64})
    for (const Case C : {Case{false, false, false, false, true},
                         Case{true, false, false, false, false},
                         Case{true, false, false, true, true},
                         Case{true, true, false, true, false},
                         Case{false, false, true, true, false}})
      for (const bool ReadsRestore : {false, true}) {
        SCOPED_TRACE(::testing::Message()
                     << int(Architecture) << ':' << C.Lift << C.Patch
                     << C.Analysis << C.Projection << ':' << ReadsRestore);
        BinaryImage Image;
        Image.Arch = Architecture;
        Image.Format = BinaryFormat::ELF;
        PipelineOptions Options;
        Options.LiftMode = C.Lift;
        Options.PatchMode = C.Patch;
        Options.MedAnalysisOnly = C.Analysis;
        Options.SourceProjection = C.Projection;
        auto F = savedCallerRegister(Architecture, ReadsRestore);
        // These ARM/x64 fixtures have no x86 registration-state provenance.
        LowFunc Source;
        Source.Entry = F.Entry;
        prepareMedStackAccesses(F, Source, Image, medStackProjection(Options));
        EXPECT_EQ(F.Blocks[0].Ops[3].PrologueSave, C.Source && !ReadsRestore);
        EXPECT_EQ(F.Blocks[0].Ops[5].ReturnAddressRead,
                  C.Source && Architecture == Arch::X64);
        EXPECT_EQ(F.Blocks[0].Ops.size(), 7u);
        EXPECT_EQ(F.Blocks[0].Ops[3].Opcode, NdOp::STORE);
        EXPECT_EQ(F.Blocks[0].Ops[4].Opcode, NdOp::LOAD);
      }
}

} // namespace
