//===- RegistrationCodeRangeTests.cpp - Disjoint PE32 callback chunks ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Authenticate callback ownership in canonical metadata and native tokens.
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/backend/llvm/WindowsEHMetadata.h"
#include "neverd/backend/llvm/WindowsEHMetadataEncoder.h"
#include "neverd/backend/llvm/WindowsEHNativeSource.h"
#include "neverd/backend/llvm/WindowsEHSemanticDigest.h"
#include "neverd/loader/ExceptionInfo.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/LLVMContext.h"

namespace {
using namespace neverd;

ExceptionFunction makeDisjointCatch() {
  ExceptionFunction EH;
  EH.CodeRange = {0x401000, 0x401080};
  EH.FragmentRanges = {{0x401100, 0x401120}, {0x401200, 0x401240}};
  EH.Encoding = ExceptionEncoding::X86CxxFuncInfo;
  EH.Personality = ExceptionPersonality::CxxFrameHandler3;
  EH.PersonalityVA = 0x401800;
  EH.HandlerDataVA = 0x403000;
  auto &Chain = EH.Registration.emplace();
  Chain.HandlerVA = EH.PersonalityVA;
  Chain.ScopeTableVA = EH.HandlerDataVA;
  Chain.RegistrationOffset = -12;
  Chain.TryLevelOffset = -4;
  Chain.SeededTryLevel = -1;
  Chain.ChainInstallVA = 0x401010;
  auto &Cxx = EH.Cxx.emplace();
  Cxx.NativeEncoding = CxxExceptionInfo::Encoding::FH3;
  Cxx.NativeFuncInfoVA = EH.HandlerDataVA;
  Cxx.Magic = 0x19930522;
  Cxx.Version = CxxFuncInfoVersion::WithEHFlags;
  Cxx.Flags = 1;
  Cxx.IsSynchronous = true;
  Cxx.MaxState = 2;
  Cxx.UnwindMap = {{-1, 0, CxxUnwindAction::ActionKind::None},
                   {-1, 0, CxxUnwindAction::ActionKind::None}};
  auto &Try = Cxx.TryBlocks.emplace_back();
  Try.TryLow = Try.TryHigh = 0;
  Try.CatchHigh = 1;
  Try.Handlers.emplace_back().HandlerVA = 0x401100;
  Try.Handlers[0].Adjectives = 0x40;
  return EH;
}

TEST(RegistrationCodeRange, KeepsChunksInMetadataAndSemanticIdentity) {
  auto EH = makeDisjointCatch();
  llvm::LLVMContext Context;
  const auto *Metadata = windows_eh_md::getCanonicalFunctionMetadata(
      Context, EH, Arch::X86, BinaryFormat::COFF);
  const auto Token =
      windows_eh_semantics::getCxxCatchSemanticToken(EH, Arch::X86, 0, 0);
  ASSERT_TRUE(Token);
  const auto *Ranges = llvm::cast<llvm::MDNode>(
      Metadata->getOperand(windows_eh_md::FragmentRanges));
  ASSERT_EQ(Ranges->getNumOperands(), 2u);
  for (unsigned I = 0; I != 2; ++I) {
    const auto *Range = llvm::cast<llvm::MDNode>(Ranges->getOperand(I));
    ASSERT_EQ(Range->getNumOperands(), 2u);
    EXPECT_EQ(llvm::mdconst::extract<llvm::ConstantInt>(Range->getOperand(0))
                  ->getZExtValue(),
              EH.FragmentRanges[I].Begin);
    EXPECT_EQ(llvm::mdconst::extract<llvm::ConstantInt>(Range->getOperand(1))
                  ->getZExtValue(),
              EH.FragmentRanges[I].End);
  }
  for (unsigned Mutation = 0; Mutation != 4; ++Mutation) {
    auto Changed = EH;
    if (Mutation == 0)
      Changed.FragmentRanges.clear();
    if (Mutation == 1)
      Changed.FragmentRanges[0].Begin += 1;
    if (Mutation == 2)
      Changed.FragmentRanges[0].End -= 1;
    if (Mutation == 3)
      std::swap(Changed.FragmentRanges[0], Changed.FragmentRanges[1]);
    EXPECT_NE(Metadata, windows_eh_md::getCanonicalFunctionMetadata(
                            Context, Changed, Arch::X86, BinaryFormat::COFF));
    EXPECT_NE(Token, windows_eh_semantics::getCxxCatchSemanticToken(
                         Changed, Arch::X86, 0, 0));
  }
}

TEST(RegistrationCodeRange, RejectsAmbiguousAndUnownedNativeChunks) {
  auto Source = makeDisjointCatch();
  const auto Classify = [](const auto &EH) {
    return classifyWindowsEHNativeSource(EH, Arch::X86, BinaryFormat::COFF,
                                         WindowsEHNativeCapability::IRLowering);
  };
#ifdef LLVM_NEVERD_X86_CXX_CATCH_SUBFIELDS
  ASSERT_TRUE(Classify(Source).canLowerNativeIR());
#endif
  for (unsigned Mutation = 0; Mutation != 7; ++Mutation) {
    auto EH = Source;
    if (Mutation == 0)
      EH.FragmentRanges.clear();
    if (Mutation == 1)
      EH.FragmentRanges[0].End = EH.FragmentRanges[0].Begin;
    if (Mutation == 2)
      EH.FragmentRanges[0].Begin = EH.CodeRange.End - 1;
    if (Mutation == 3)
      EH.FragmentRanges[1] = EH.FragmentRanges[0];
    if (Mutation == 4)
      std::swap(EH.FragmentRanges[0], EH.FragmentRanges[1]);
    if (Mutation == 5)
      EH.FragmentRanges[1].End = uint64_t(UINT32_MAX) + 2;
    if (Mutation == 6)
      EH.Cxx->TryBlocks[0].Handlers[0].HandlerVA = EH.FragmentRanges[0].End;
    EXPECT_FALSE(Classify(EH).canLowerNativeIR()) << Mutation;
  }
}
} // namespace
