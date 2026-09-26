#include "gtest/gtest.h"

#include "neverd/decode/Decoder.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/libc/LibCNames.h"
#include "neverd/loader/BinaryImage.h"

#include <algorithm>

using namespace neverd;
namespace {
TEST(AdjacentCalls, ProvenNoReturnTargetKeepsDecodedCallInsteadOfGetPCPush) {
  for (auto Architecture : {Arch::X86, Arch::X64})
    for (auto Format :
         {BinaryFormat::ELF, BinaryFormat::MachO, BinaryFormat::COFF})
      for (bool Indexed : {false, true})
        for (bool NoReturn : {false, true}) {
          SCOPED_TRACE(testing::Message()
                       << static_cast<int>(Architecture) << " "
                       << static_cast<int>(Format) << " indexed=" << Indexed);
          BinaryImage Image;
          Image.Arch = Architecture;
          Image.Bits =
              Architecture == Arch::X64 ? Bitness::Bits64 : Bitness::Bits32;
          Image.Format = Format;
          Image.Base = 0x1000;
          // call next; ud2. The next address names an independently known
          // no-return function. The caller must end at its real CALL.
          Segment Text;
          Text.VA = Image.Base;
          Text.Size = 7;
          Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
          Text.Data = NoReturn
                          ? std::vector<uint8_t>{0xe8, 0, 0, 0, 0, 0x0f, 0x0b}
                          : std::vector<uint8_t>{0xe8, 0, 0, 0, 0, 0x58, 0xc3};
          Image.Segments.push_back(std::move(Text));
          auto Callee = Symbol::makeFunc(0x1005);
          Callee.Name = NoReturn ? "abort" : "warn";
          Image.Symbols.push_back(Callee);
          Decoder Dec;
          ASSERT_TRUE(Dec.init(Architecture));
          CFGBuilder Builder;
          std::optional<libc::NoReturnTargetIndex> Index;
          if (Indexed) {
            Index.emplace(Image);
            Builder.setNoReturnTargetIndex(&*Index);
          }
          auto Function =
              Builder.build(Image, Dec, Image.Base, "adjacent_call");
          ASSERT_EQ(Function.Blocks.size(), 1U);
          const auto &Block = Function.Blocks.front();
          if (!NoReturn) {
            ASSERT_EQ(Block.InstructionBoundaries.size(), 3U);
            EXPECT_EQ(Block.InstructionBoundaries.front().Control,
                      LowInstructionControl::None);
            EXPECT_TRUE(std::any_of(
                Block.Ops.begin(), Block.Ops.end(),
                [](const auto &Op) { return Op.Opcode == NdOp::STORE; }));
            continue;
          }
          ASSERT_EQ(Block.InstructionBoundaries.size(), 1U);
          const auto &Boundary = Block.InstructionBoundaries.front();
          EXPECT_EQ(Boundary.Control, LowInstructionControl::Call);
          EXPECT_TRUE(hasLowInstructionControlFlag(
              Boundary.ControlFlags, LowInstructionControlFlag::NoReturn));
          ASSERT_EQ(Block.Ops.size(), 1U);
          EXPECT_EQ(Block.Ops[0].Opcode, NdOp::CALL);
          ASSERT_EQ(Block.Ops[0].NumInputs, 1U);
          EXPECT_EQ(Block.Ops[0].Inputs[0].Offset, 0x1005U);
          EXPECT_FALSE(static_cast<bool>(validateLowInstructionBoundaries(
              Function, LowInstructionBoundaryRequirement::Required)));
        }
}

TEST(AdjacentCalls, DirectTailTransferRespectsSymbolOutsideFunctionWorkSet) {
  for (auto Architecture : {Arch::X86, Arch::X64})
    for (auto Format :
         {BinaryFormat::ELF, BinaryFormat::MachO, BinaryFormat::COFF}) {
      SCOPED_TRACE(testing::Message() << static_cast<int>(Architecture) << " "
                                     << static_cast<int>(Format));
      BinaryImage Image;
      Image.Arch = Architecture;
      Image.Bits =
          Architecture == Arch::X64 ? Bitness::Bits64 : Bitness::Bits32;
      Image.Format = Format;
      Image.Base = 0x1000;
      // jmp next; ret. The target symbol is not in a supplied function work
      // set, but the explicit transfer must still preserve its identity.
      Segment Text;
      Text.VA = Image.Base;
      Text.Data = {0xe9, 0, 0, 0, 0, 0xc3};
      Text.Size = Text.Data.size();
      Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
      Image.Segments.push_back(std::move(Text));
      auto Callee = Symbol::makeFunc(Image.Base + 5);
      Callee.Name = "returning_tail_callee";
      Image.Symbols.push_back(Callee);
      Decoder Dec;
      ASSERT_TRUE(Dec.init(Architecture));
      CFGBuilder Builder;
      const auto Function =
          Builder.build(Image, Dec, Image.Base, "adjacent_tail");
      ASSERT_EQ(Function.Blocks.size(), 1U);
      const auto &Block = Function.Blocks.front();
      ASSERT_EQ(Block.InstructionBoundaries.size(), 1U);
      EXPECT_EQ(Block.InstructionBoundaries.front().Control,
                LowInstructionControl::TailCall);
      ASSERT_EQ(Block.Ops.size(), 2U);
      EXPECT_EQ(Block.Ops[0].Opcode, NdOp::CALL);
      ASSERT_EQ(Block.Ops[0].NumInputs, 1U);
      EXPECT_EQ(Block.Ops[0].Inputs[0].Offset, Callee.Addr);
      EXPECT_EQ(Block.Ops[1].Opcode, NdOp::RETURN);
      EXPECT_FALSE(static_cast<bool>(validateLowInstructionBoundaries(
          Function, LowInstructionBoundaryRequirement::Required)));
    }
}
} // namespace
