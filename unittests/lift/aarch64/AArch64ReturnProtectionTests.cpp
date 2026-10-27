//===- AArch64ReturnProtectionTests.cpp - Source return protection ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/decode/Decoder.h"
#include "neverd/ir/high/MedToHigh.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/ir/med/MedPrologueSaves.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/Support/raw_ostream.h"

#include <initializer_list>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace neverd;

MedFunc decodedFunction(std::initializer_list<uint32_t> Words,
                        BinaryFormat Format) {
  BinaryImage Image;
  Image.Arch = Arch::AArch64;
  Image.Format = Format;
  Image.Bits = Bitness::Bits64;
  Segment Text;
  Text.VA = 0x1000;
  Text.Size = Text.FileSz = Words.size() * 4;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.resize(Text.Size);
  size_t At = 0;
  for (uint32_t Word : Words) {
    writeLE<uint32_t>(Text.Data.data() + At, Word);
    At += 4;
  }
  Image.Segments.push_back(std::move(Text));
  Decoder Dec;
  EXPECT_TRUE(Dec.init(Image.Arch));
  Dec.setStrict(true);
  auto Low = CFGBuilder().build(Image, Dec, 0x1000, "protected_return");
  LowToMedConverter Converter;
  Converter.setBinaryImage(&Image);
  auto Med = Converter.convert(Low, Image.Arch, Format);
  markSourceStackAccesses(Med, Image.Arch, Format);
  return Med;
}

std::string source(std::initializer_list<uint32_t> Words, BinaryFormat Format,
                   Arch OutputArchitecture = Arch::AArch64) {
  const auto Med = decodedFunction(Words, Format);
  auto High = MedToHighConverter().convert(Med, Arch::AArch64);
  CEmitterOptions Options;
  Options.TheArch = OutputArchitecture;
  Options.Format = Format;
  std::string TextC;
  llvm::raw_string_ostream Out(TextC);
  EXPECT_TRUE(HighCEmitter().emit({High}, Out, Options));
  return TextC;
}

TEST(AArch64ReturnProtection, LeafKeepsKeyAndReturnValue) {
  for (BinaryFormat Format :
       {BinaryFormat::ELF, BinaryFormat::MachO, BinaryFormat::COFF}) {
    // pacibsp; mov x0, #7; retab.
    const auto Text = source({0xd503237f, 0xd28000e0, 0xd65f0fff}, Format);
    EXPECT_NE(Text.find("branch-protection=pac-ret+leaf+b-key"),
              std::string::npos)
        << Text;
    EXPECT_NE(Text.find("return 7;"), std::string::npos) << Text;
    EXPECT_EQ(Text.find("__pointer"), std::string::npos) << Text;
    EXPECT_EQ(Text.find("__builtin_ptrauth_auth"), std::string::npos) << Text;
  }
}

TEST(AArch64ReturnProtection, SavedReturnAddressBecomesFunctionAttribute) {
  for (BinaryFormat Format :
       {BinaryFormat::ELF, BinaryFormat::MachO, BinaryFormat::COFF}) {
    // paciasp; stp fp,lr,[sp,#-32]!; mov fp,sp; bl 0x1100;
    // mov x0,#7; ldp fp,lr,[sp],#32; autiasp; ret.
    const auto Text = source({0xd503233f, 0xa9be7bfd, 0x910003fd, 0x9400003d,
                              0xd28000e0, 0xa8c27bfd, 0xd50323bf, 0xd65f03c0},
                             Format);
    EXPECT_NE(Text.find("target(\"branch-protection=pac-ret\")"),
              std::string::npos)
        << Text;
    EXPECT_NE(Text.find("return 7;"), std::string::npos) << Text;
    EXPECT_EQ(Text.find("__pointer"), std::string::npos) << Text;
    EXPECT_EQ(Text.find("stack_storage"), std::string::npos) << Text;
  }
}

TEST(AArch64ReturnProtection, IncompleteOrObservableRecipesStayExplicit) {
  for (auto Words :
       {// PAC and AUT keys differ.
        std::initializer_list<uint32_t>{0xd503233f, 0xd50323ff, 0xd65f03c0},
        // Signed LR is the observable result: mov x0,lr.
        {0xd503233f, 0xaa1e03e0, 0xd50323bf, 0xd65f03c0},
        // No authentication.
        {0xd503233f, 0xd28000e0, 0xd65f03c0},
        // Authentication uses a different stack position.
        {0xd503233f, 0xd10043ff, 0xd50323bf, 0xd65f03c0}}) {
    const auto Text = source(Words, BinaryFormat::ELF);
    EXPECT_EQ(Text.find("branch-protection="), std::string::npos) << Text;
    EXPECT_NE(Text.find("pacia "), std::string::npos) << Text;
  }
}

TEST(AArch64ReturnProtection, OtherTargetsCannotDiscardThePolicy) {
  EXPECT_THROW(source({0xd503237f, 0xd28000e0, 0xd65f0fff}, BinaryFormat::ELF,
                      Arch::X64),
               std::invalid_argument);
}

TEST(AArch64ReturnProtection, MalformedRecipesCannotDisappearBeforeValidation) {
  // Use the owning lowering path for register identities and entry seeds.
  // paciasp; autiasp; ret.
  const auto Function =
      decodedFunction({0xd503233f, 0xd50323bf, 0xd65f03c0}, BinaryFormat::ELF);
  ASSERT_TRUE(findAArch64ReturnProtection(Function, BinaryFormat::ELF));
  unsigned Checked = 0;
  for (size_t Index = 0; Index < Function.Blocks.front().Ops.size(); ++Index) {
    if (Function.Blocks.front().Ops[Index].Opcode != NdOp::INTRINSIC)
      continue;
    auto Broken = Function;
    Broken.Blocks.front().Ops[Index].Inputs[2].Size = 4;
    EXPECT_FALSE(findAArch64ReturnProtection(Broken, BinaryFormat::ELF));
    ++Checked;
  }
  EXPECT_EQ(Checked, 2u);
}
} // namespace
