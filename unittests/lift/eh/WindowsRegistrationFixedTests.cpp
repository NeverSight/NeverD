//===- WindowsRegistrationFixedTests.cpp - Displaced C++ frames ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/ir/med/MedStackAlignment.h"
#include "neverd/ir/med/X86RegistrationFrame.h"
#include "neverd/loader/COFF/COFFLoader.h"
#include "neverd/loader/COFF/COFFRegistrationEH.h"

#include <cstdlib>

using namespace neverd;

namespace {

TEST(WindowsRegistrationFixed, RuntimeOffsetsRequireTheCompleteLayout) {
  RegistrationChainInfo Chain;
  EXPECT_FALSE(Chain.cxxRuntimeFrameOffset());
  Chain.RegistrationOffset = -24;
  Chain.TryLevelOffset = -16;
  Chain.SeededTryLevel = -1;
  EXPECT_EQ(Chain.cxxRuntimeFrameOffset(), -12);
  EXPECT_EQ(Chain.cxxSourceFrameOffset(-64), -76);
  EXPECT_EQ(Chain.cxxSourceFrameOffset(INT32_MIN + 12), INT32_MIN);
  EXPECT_FALSE(Chain.cxxSourceFrameOffset(INT32_MIN + 11));
  EXPECT_EQ(Chain.chainInstallInstructionSize(), 6);
  for (unsigned Mutation = 0; Mutation != 5; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Changed = Chain;
    if (Mutation == 0)
      Changed.RegistrationOffset.reset();
    if (Mutation == 1)
      Changed.TryLevelOffset = -4;
    if (Mutation == 2)
      Changed.SeededTryLevel = -2;
    if (Mutation == 3)
      Changed.RegistrationOffset = -28;
    if (Mutation == 4)
      Changed.RealignedFrame.emplace();
    EXPECT_FALSE(Changed.cxxRuntimeFrameOffset());
    EXPECT_FALSE(Changed.cxxSourceFrameOffset(-64));
  }
  Chain.RegistrationOffset = -12;
  Chain.TryLevelOffset = -4;
  EXPECT_EQ(Chain.cxxRuntimeFrameOffset(), 0);
  EXPECT_EQ(Chain.cxxSourceFrameOffset(-64), -64);
  EXPECT_EQ(Chain.chainInstallInstructionSize(), 7);
}

TEST(WindowsRegistrationFixed, InputPE32ProvesDisplacedFrame) {
  const auto *Path = std::getenv("NEVERD_REGISTRATION_REALIGNED_PE32");
  if (!Path)
    GTEST_SKIP() << "requires the LLVM fixed-frame PE32 fixture";
  auto Image = COFFLoader().load(Path);
  ASSERT_TRUE(bool(Image));
  const auto It =
      llvm::find_if(Image->ExceptionMetadata.Functions,
                    [](const auto &EH) { return EH.Registration && EH.Cxx; });
  ASSERT_NE(It, Image->ExceptionMetadata.Functions.end());
  const auto EH = *It;
  ASSERT_FALSE(EH.Registration->RealignedFrame);
  ASSERT_EQ(EH.Registration->RegistrationOffset, -24);
  ASSERT_EQ(EH.Registration->TryLevelOffset, -16);
  ASSERT_EQ(EH.Cxx->TryBlocks.size(), 1u);
  ASSERT_EQ(EH.Cxx->TryBlocks[0].Handlers.size(), 1u);
  const auto Catch = EH.Cxx->TryBlocks[0].Handlers[0].HandlerVA;
  Decoder Decode;
  ASSERT_TRUE(Decode.init(*Image));
  const auto Low =
      CFGBuilder().build(*Image, Decode, EH.CodeRange.Begin, "fixed_parent");
  ASSERT_TRUE(Low.RegistrationStates);
  const auto &State = *Low.RegistrationStates;
  ASSERT_TRUE(State.Complete);
  ASSERT_TRUE(State.CxxContinuationsComplete);
  ASSERT_EQ(State.CxxContinuations.size(), 1u);
  auto Med = LowToMedConverter().convert(Low, Arch::X86, BinaryFormat::COFF);
  ASSERT_TRUE(verifyMedFunc(Med, "fixed-registration-frame"));
  unsigned Roots = 0;
  for (const auto &Block : Med.Blocks)
    for (const auto &Op : Block.Ops) {
      if (Op.RegistrationRoot !=
          MedOp::RegistrationRootKind::DisplacedFramePointer)
        continue;
      ++Roots;
      const auto Coordinate = registrationRootFrameCoordinate(Med, Op);
      ASSERT_TRUE(Coordinate);
      EXPECT_EQ(Coordinate->EntryOffset, -4);
      EXPECT_EQ(Coordinate->Alignment, 1u);
      EXPECT_EQ(Coordinate->AlignedOffset, -12);
      EXPECT_EQ(entryStackOffset(Med, Op.Output, Arch::X86, BinaryFormat::COFF),
                -16);
      for (unsigned Mutation = 0; Mutation != 7; ++Mutation) {
        SCOPED_TRACE(Mutation);
        auto Changed = Med;
        auto Root = Op;
        if (Mutation == 0)
          Changed.ExceptionMetadata->Registration->TryLevelOffset = -4;
        if (Mutation == 1)
          Changed.RegistrationStates->Complete = false;
        if (Mutation == 2)
          Changed.ExceptionMetadata->Registration->RegistrationOffset = -12;
        if (Mutation == 3)
          ++Root.Addr;
        if (Mutation == 4)
          Root.RegistrationStackOffset = -12;
        if (Mutation == 5)
          ++Changed.Entry;
        if (Mutation == 6)
          Changed.RegistrationStates->CxxContinuationsComplete = false;
        EXPECT_FALSE(registrationRootFrameCoordinate(Changed, Root));
      }
    }
  EXPECT_GE(Roots, 2u);

  auto Change = [](BinaryImage &Img, va_t Address, uint8_t Byte) {
    for (auto &Segment : Img.Segments)
      if (Address >= Segment.VA && Address - Segment.VA < Segment.Data.size()) {
        Segment.Data[Address - Segment.VA] = Byte;
        return true;
      }
    return false;
  };
  const auto Begin = EH.CodeRange.Begin;
  const auto *Prologue = Image->readVA(Begin, 12);
  ASSERT_TRUE(Prologue);
  const unsigned Prefix = Prologue[6] == 0x83 ? 9 : 12;
  const std::pair<unsigned, uint8_t> Edits[] = {
      {0, 0x50},           {3, 0x50},           {6, 0x82},
      {Prefix, 0x8b},      {Prefix + 2, 0xe8},  {Prefix + 5, 0xfc},
      {Prefix + 6, 0},     {Prefix + 12, 0xf4}, {Prefix + 15, 0xf8},
      {Prefix + 22, 0x1d}, {Prefix + 29, 0xf4}, {Prefix + 32, 4}};
  for (const auto &[Offset, Byte] : Edits) {
    SCOPED_TRACE(Offset);
    BinaryImage Bad = *Image;
    ASSERT_TRUE(Change(Bad, Begin + Offset, Byte));
    Bad.ExceptionMetadata = {};
    coff_loader::parseX86RegistrationExceptions(Bad);
    const auto *Graph = Bad.ExceptionMetadata.findFunction(Begin);
    EXPECT_TRUE(!Graph || !Graph->Registration ||
                !Graph->Registration->RegistrationOffset);
  }
  const auto Read = EH.Registration->ChainInstallVA - 10;
  const auto ModRM = Image->readVA(Read, 3)[2];
  ASSERT_TRUE(ModRM == 0x0d || ModRM == 0x15);
  for (bool Matched : {false, true}) {
    BinaryImage Variant = *Image;
    ASSERT_TRUE(Change(Variant, Read + 2, ModRM == 0x0d ? 0x15 : 0x0d));
    if (Matched)
      ASSERT_TRUE(Change(Variant, Read + 8, ModRM == 0x0d ? 0x55 : 0x4d));
    Variant.ExceptionMetadata = {};
    coff_loader::parseX86RegistrationExceptions(Variant);
    const auto *Graph = Variant.ExceptionMetadata.findFunction(Begin);
    ASSERT_TRUE(Graph && Graph->Registration);
    EXPECT_EQ(Graph->Registration->RegistrationOffset.has_value(), Matched);
  }
  const auto &Resume = State.CxxContinuations.front();
  for (const auto &[Address, Byte] : {std::pair<va_t, uint8_t>{Catch, 0x50},
                                      {Resume.Address - 1, 0x58},
                                      {Catch + 3, 16},
                                      {Resume.TargetVA + 2, 16}}) {
    SCOPED_TRACE(Address);
    BinaryImage Bad = *Image;
    ASSERT_TRUE(Change(Bad, Address, Byte));
    Decoder BadDecode;
    ASSERT_TRUE(BadDecode.init(Bad));
    const auto Broken = CFGBuilder().build(Bad, BadDecode, Begin, "bad_fixed");
    ASSERT_TRUE(Broken.RegistrationStates);
    EXPECT_FALSE(Broken.RegistrationStates->Complete &&
                 Broken.RegistrationStates->CxxContinuationsComplete &&
                 Broken.RegistrationStates->ImageReadsComplete &&
                 Broken.RegistrationStates->ChainOperationsComplete);
  }
  if (EH.Cxx->TryBlocks[0].Handlers[0].CatchObjectOffset) {
    auto Bad = Low;
    Bad.ExceptionMetadata->Cxx->TryBlocks[0].Handlers[0].CatchObjectOffset = 8;
    const auto Broken = analyzeRegistrationStates(
        Bad, 0, 0, &State.CalleeContracts, &State.CleanupContracts);
    EXPECT_FALSE(Broken.CxxCatchObjectsComplete);
  }
}

} // namespace
