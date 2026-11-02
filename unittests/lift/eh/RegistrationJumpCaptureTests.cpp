//===- RegistrationJumpCaptureTests.cpp - Physical capture arguments ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Check the current frame, physical PC and complete EH3 buffer separation.

#include "../../../lib/ir/low/X86/RegistrationJumpCapture.h"
#include "../../../lib/ir/low/X86/RegistrationJumpRuntime.h"
#include "gtest/gtest.h"

#include "neverd/Limits.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/ir/low/LowIR.h"
#include "neverd/lift/X86/X86Regs.h"
#include "neverd/loader/COFF/COFFLoader.h"

using namespace neverd;
using namespace neverd::registration_state;

#ifdef NEVERD_BINARY_CORPUS_ROOT
TEST(RegistrationJumpCapture, PhysicalCapturePCPartitionsOrdinaryFlow) {
  auto Image = COFFLoader().load(
      std::string(NEVERD_BINARY_CORPUS_ROOT) +
      "/corpus/windows-eh/msvc/vs2019/x86/native/no-gs/o0/windows-seh-tests/"
      "xcpt4-msvc-x86-native-no-gs-o0.exe");
  ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
  Decoder Dec;
  ASSERT_TRUE(Dec.init(*Image));
  const auto Function =
      CFGBuilder().build(*Image, Dec, 0x401580, "capture-pcs");
  unsigned Captures = 0, Resumes = 0;
  for (const auto &Block : Function.Blocks)
    for (const auto &Op : Block.Ops) {
      if (Op.Opcode != NdOp::CALL || Op.NumInputs != 1 ||
          !Op.Inputs[0].isConst() ||
          (Op.Inputs[0].Offset != 0x407c7e && Op.Inputs[0].Offset != 0x407c66))
        continue;
      const bool Capture = Op.Inputs[0].Offset == 0x407c7e;
      if (Capture)
        ++Captures;
      else
        ++Resumes;
      EXPECT_EQ(Block.EndAddr, Op.Addr + 5);
      if (!Capture)
        continue;
      ASSERT_EQ(Block.Succs.size(), 1u);
      const auto Next = llvm::find_if(Function.Blocks, [&](const LowBlock &B) {
        return B.Id == Block.Succs[0];
      });
      ASSERT_NE(Next, Function.Blocks.end());
      EXPECT_EQ(Next->StartAddr, Op.Addr + 5);
      EXPECT_FALSE(Function.ModuleAnalysisRoots.count(Next->StartAddr));
      EXPECT_FALSE(Function.OrdinaryModuleAnalysisRoots.count(Next->StartAddr));
    }
  EXPECT_EQ(Captures, 7u);
  EXPECT_EQ(Resumes, 6u);
}

TEST(RegistrationJumpCapture, EH4CapturePCPartitionsOrdinaryFlow) {
  auto Image = COFFLoader().load(
      std::string(NEVERD_BINARY_CORPUS_ROOT) +
      "/corpus/windows-eh/msvc/vs2019/x86/native/gs/o0/windows-seh-tests/"
      "xcpt4-msvc-x86-native-gs-o0.exe");
  ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
  const ExceptionFunction *Source = nullptr;
  for (const auto &EH : Image->ExceptionMetadata.Functions)
    if (EH.Registration &&
        EH.Personality == ExceptionPersonality::ExceptHandler4 &&
        (!Source ||
         EH.Registration->Scopes.size() > Source->Registration->Scopes.size()))
      Source = &EH;
  ASSERT_TRUE(Source);
  Decoder Dec;
  ASSERT_TRUE(Dec.init(*Image));
  const auto Function = CFGBuilder().build(*Image, Dec, Source->CodeRange.Begin,
                                           "eh4-capture-pcs");
  unsigned Captures = 0;
  for (const auto &Block : Function.Blocks)
    for (const auto &Op : Block.Ops) {
      if (Op.Opcode != NdOp::CALL || Op.NumInputs != 1 ||
          !Op.Inputs[0].isConst())
        continue;
      const auto Runtime =
          checkedJumpRuntime(*Image, Op.Inputs[0].Offset, false);
      if (!Runtime || Runtime->Routine != JumpRuntimeBinding::Kind::Capture)
        continue;
      ++Captures;
      EXPECT_EQ(Block.EndAddr, Op.Addr + 5);
      ASSERT_EQ(Block.Succs.size(), 1u);
      const auto Next = llvm::find_if(Function.Blocks, [&](const LowBlock &B) {
        return B.Id == Block.Succs.front();
      });
      ASSERT_NE(Next, Function.Blocks.end());
      EXPECT_EQ(Next->StartAddr, Op.Addr + 5);
      EXPECT_FALSE(Function.ModuleAnalysisRoots.count(Next->StartAddr));
      EXPECT_FALSE(Function.OrdinaryModuleAnalysisRoots.count(Next->StartAddr));
    }
  EXPECT_GT(Captures, 0u);
}

TEST(RegistrationJumpCapture, BindsCurrentSourceAndBoundedCallerStorage) {
  for (bool EH4 : {false, true})
    for (unsigned Mutation = 0; Mutation != (EH4 ? 28u : 23u); ++Mutation) {
      SCOPED_TRACE(EH4);
      SCOPED_TRACE(Mutation);
      auto Image = COFFLoader().load(std::string(NEVERD_BINARY_CORPUS_ROOT) +
                                     "/corpus/windows-eh/msvc/vs2019/x86/"
                                     "native/no-gs/o0/windows-seh-tests/"
                                     "xcpt4-msvc-x86-native-no-gs-o0.exe");
      ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
      const auto *EH = Image->ExceptionMetadata.findFunction(0x401580);
      ASSERT_TRUE(EH && EH->Registration);
      LowFunc Function;
      Function.Entry = 0x401580;
      Function.ExceptionMetadata = *EH;
      LowBlock Block;
      Block.Id = 0;
      Block.StartAddr = 0x40223c;
      Block.EndAddr = 0x402241;
      LowOp Call;
      Call.Opcode = NdOp::CALL;
      Call.Addr = Block.StartAddr;
      Call.Seq = 0;
      Call.NumInputs = 1;
      Call.Inputs[0] = NdVar::cst(0x407c7e, 4);
      Call.Output = NdVar::reg(x86reg::RAX, 4);
      Block.Ops.push_back(Call);
      LowInstructionBoundary Source;
      Source.Address = Call.Addr;
      Source.Size = 5;
      Source.OpCount = 1;
      Source.Control = LowInstructionControl::Call;
      Source.ControlFlags = LowInstructionControlFlag::Call;
      Source.Immediate = Call.Inputs[0].Offset;
      Block.InstructionBoundaries.push_back(Source);
      Function.Blocks.push_back(Block);
      FrameState Frame;
      Frame.Registers[x86reg::RSP / x86reg::GeneralRegStride] =
          FrameValue::frame(-316);
      Frame.Registers[x86reg::RBP / x86reg::GeneralRegStride] =
          FrameValue::frame(0);
      Frame.Registers[x86reg::RBX / x86reg::GeneralRegStride].MayBeFrame = true;
      Frame.Cells.set(-316, FrameValue::frame(-288));
      Frame.Cells.set(-312, FrameValue::constant(2));
      Frame.Cells.set(-308, FrameValue::constant(0x407c78));
      Frame.Cells.set(-304, FrameValue::constant(UINT32_MAX));
      Frame.Cells.set(-16, FrameValue::previousChain());
      Frame.Cells.set(-12, FrameValue::constant(uint32_t(EH->PersonalityVA)));
      Frame.Cells.set(
          -8, FrameValue::constant(uint32_t(EH->Registration->ScopeTableVA)));
      Frame.Cells.set(-4, FrameValue::constant(UINT32_MAX));
      if (EH4) {
        auto &Metadata = *Function.ExceptionMetadata;
        Metadata.Encoding = ExceptionEncoding::X86ScopeTableEH4;
        Metadata.Personality = ExceptionPersonality::ExceptHandler4;
        Metadata.Registration->SeededTryLevel = -2;
        Image->DynInfo.SecurityCookieRVA = 0x9000;
        Frame.Cells.set(-312, FrameValue::constant(3));
        Frame.Cells.set(-304, FrameValue::constant(uint32_t(-2)));
        Frame.Cells.set(-300, FrameValue::constant(Image->Base + 0x9000));
        Frame.Cells.set(-4, FrameValue::constant(uint32_t(-2)));
        Frame.Cells.set(-8, {});
        Frame.Cells.edit([&](auto &Cells) { Cells[-8].SecurityCookie = true; });
        Frame.Cells.edit([&](auto &Cells) {
          Cells[-8].CookieXOR = uint32_t(EH->Registration->ScopeTableVA);
        });
        for (auto &Import : Image->Imports)
          if (Import.IATAddr == 0x40805c)
            Import.Name = "_seh_longjmp_unwind4";
        Image->ImportStorageSlots.at(0x40805c).Name = "_seh_longjmp_unwind4";
      }
      auto &Op = Function.Blocks[0].Ops[0];
      auto &Insn = Function.Blocks[0].InstructionBoundaries[0];
      bool Installed = true;
      size_t Work = 0;
      if (Mutation == 1)
        Installed = false;
      if (Mutation == 2)
        Frame.Cells.set(-316, FrameValue::frame(-316));
      if (Mutation == 3)
        Frame.Cells.set(-316, FrameValue::frame(-84)); // end crosses SavedESP.
      if (Mutation == 4)
        Frame.Cells.set(-312, FrameValue::constant(1));
      if (Mutation == 5)
        Frame.Cells.set(-308, FrameValue::constant(0x407c66));
      if (Mutation == 6)
        Frame.Cells.set(-304, FrameValue::constant(0));
      if (Mutation == 7)
        Frame.Registers[x86reg::RBP / x86reg::GeneralRegStride] = {};
      if (Mutation == 8)
        Frame.enterCallback(0x401683);
      if (Mutation == 9)
        Frame.Cells.set(-16, FrameValue::constant(0));
      if (Mutation == 10)
        Frame.Cells.set(-8, FrameValue::constant(0));
      if (Mutation == 11)
        Insn.Immediate = 0x407c66;
      if (Mutation == 12)
        Insn.OpCount = 0;
      if (Mutation == 13)
        Insn.ControlFlags |= LowInstructionControlFlag::NoReturn;
      if (Mutation == 14)
        Insn.ControlFlags |= LowInstructionControlFlag::Conditional;
      if (Mutation == 15)
        Op.Inputs[0].Size = 8;
      if (Mutation == 16)
        Work = limits::kMaxRegistrationEHStateWork;
      if (Mutation == 17)
        Function.ExceptionMetadata->Registration->RegistrationOffset = -20;
      if (Mutation == 18) {
        auto *Byte = const_cast<uint8_t *>(Image->readVA(Op.Addr, 1));
        ASSERT_TRUE(Byte);
        *Byte = 0x90;
      }
      if (Mutation == 19)
        Frame.Cells.edit([&](auto &Cells) { Cells[-316].ReturnPC = true; });
      if (Mutation == 20)
        Op.MemoryAddressSpace = NdMemoryAddressSpace::X86FS;
      if (Mutation == 21)
        Function.Entry = 0x401090;
      if (Mutation == 22)
        Function.ExceptionMetadata->CodeRange.End = Op.Addr + 4;
      if (Mutation == 23)
        Frame.Cells.set(-300, {});
      if (Mutation == 24)
        Frame.Cells.set(-300, FrameValue::constant(Image->Base + 0x9004));
      if (Mutation == 25)
        for (auto &Import : Image->Imports)
          if (Import.IATAddr == 0x40805c)
            Import.Name = "_seh_longjmp_unwind";
      if (Mutation == 26)
        Frame.Cells.edit([&](auto &Cells) { Cells[-8].CookieFrameOffset = 0; });
      if (Mutation == 27)
        Image->DynInfo.SecurityCookieRVA = 0;
      if (EH4 && Mutation == 0) {
        const auto Unwind = checkedJumpRuntime(*Image, 0x407c78, false);
        ASSERT_TRUE(Unwind);
        EXPECT_EQ(Unwind->Routine, JumpRuntimeBinding::Kind::EH4Unwind);
        EXPECT_EQ(
            checkedEH4JumpCookie(
                *Image, *Function.ExceptionMetadata->Registration, Frame, -316),
            Image->Base + 0x9000);
      }
      const auto Capture =
          prepareSEHJumpCapture(*Image, Function, Op, Frame, Installed, Work);
      EXPECT_EQ(Capture.has_value(), Mutation == 0);
      if (Capture) {
        size_t SourceWork = 0;
        const auto Binding =
            checkedJumpRuntimeCall(*Image, Function, Op, SourceWork);
        ASSERT_TRUE(Binding);
        EXPECT_EQ(Binding->Routine, JumpRuntimeBinding::Kind::Capture);
        EXPECT_TRUE(isSEHJumpRuntimeInstruction(*Image, Op.Addr, 5,
                                                Function.Blocks[0].Ops));
        EXPECT_EQ(Capture->Call, 0x40223cu);
        EXPECT_EQ(Capture->ReturnPC, 0x402241u);
        EXPECT_EQ(Capture->StackOffset, -316);
        EXPECT_EQ(Capture->BufferOffset, -288);
        EXPECT_EQ(Capture->UnwindTarget, 0x407c78u);
        EXPECT_EQ(Capture->Level, EH4 ? -2 : -1);
        EXPECT_EQ(Capture->Protocol,
                  EH4 ? RegistrationSEHJumpCapture::Kind::EH4
                      : RegistrationSEHJumpCapture::Kind::EH3);
        EXPECT_EQ(Capture->SecurityCookieVA, EH4 ? Image->Base + 0x9000 : 0u);
        EXPECT_EQ(Capture->Nonvolatile[0].Offset, 0);
        EXPECT_TRUE(Capture->Nonvolatile[1].MayBeFrame);
        EXPECT_GT(Work, 0u);
        const auto Detached = Op;
        EXPECT_FALSE(prepareSEHJumpCapture(*Image, Function, Detached, Frame,
                                           Installed, Work));
        EXPECT_FALSE(
            checkedJumpRuntimeCall(*Image, Function, Detached, SourceWork));
      }
    }
}
#endif
