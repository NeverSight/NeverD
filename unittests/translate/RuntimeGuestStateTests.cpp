//===- RuntimeGuestStateTests.cpp - Fixed translated state ABI -----------===//

#include "gtest/gtest.h"

#include "neverd/translate/RuntimeGuestState.h"

#include "llvm/ADT/APInt.h"
#include "llvm/Support/Error.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

using namespace neverd::translate;

namespace {

TEST(RuntimeGuestState, X86_64V1LayoutAndMemoryPolicyAreExact) {
  static_assert(std::is_standard_layout_v<RuntimeGuestStateX86_64V1>);
  static_assert(std::is_trivially_copyable_v<RuntimeGuestStateX86_64V1>);

  EXPECT_EQ(sizeof(RuntimeGuestStateX86_64V1), kRuntimeGuestStateX86_64SizeV1);
  EXPECT_EQ(alignof(RuntimeGuestStateX86_64V1), 16u);
  EXPECT_EQ(offsetof(RuntimeGuestStateX86_64V1, GPR), 8u);
  EXPECT_EQ(offsetof(RuntimeGuestStateX86_64V1, RIP), 136u);
  EXPECT_EQ(offsetof(RuntimeGuestStateX86_64V1, RFlagsBase), 144u);
  EXPECT_EQ(offsetof(RuntimeGuestStateX86_64V1, CF), 152u);
  EXPECT_EQ(offsetof(RuntimeGuestStateX86_64V1, OF), 158u);

  const llvm::ArrayRef<TranslationIRMemorySlot> Slots =
      runtimeGuestStateX86_64MemorySlotsV1();
  ASSERT_EQ(Slots.size(), 25u);
  for (size_t Index = 0; Index != 16; ++Index) {
    EXPECT_EQ(Slots[Index].Region, TranslationIRMemoryRegion::State);
    EXPECT_EQ(Slots[Index].Offset, offsetof(RuntimeGuestStateX86_64V1, GPR) +
                                       Index * sizeof(uint64_t));
    EXPECT_EQ(Slots[Index].Size, sizeof(uint64_t));
    EXPECT_EQ(Slots[Index].Access, TranslationIRMemoryAccess::Read |
                                       TranslationIRMemoryAccess::Write);
    EXPECT_EQ(Slots[Index].Alignment, alignof(uint64_t));
  }

  EXPECT_EQ(Slots[16].Offset, offsetof(RuntimeGuestStateX86_64V1, RIP));
  EXPECT_EQ(Slots[16].Access,
            TranslationIRMemoryAccess::Read | TranslationIRMemoryAccess::Write);
  EXPECT_EQ(Slots[17].Offset, offsetof(RuntimeGuestStateX86_64V1, RFlagsBase));
  EXPECT_EQ(Slots[17].Access, TranslationIRMemoryAccess::Read);
  constexpr uint32_t FlagAlignments[] = {8, 1, 2, 1, 4, 1, 2};
  for (size_t Index = 18; Index != Slots.size(); ++Index) {
    EXPECT_EQ(Slots[Index].Region, TranslationIRMemoryRegion::State);
    EXPECT_EQ(Slots[Index].Size, sizeof(uint8_t));
    EXPECT_EQ(Slots[Index].Access, TranslationIRMemoryAccess::Read |
                                       TranslationIRMemoryAccess::Write);
    EXPECT_EQ(Slots[Index].Alignment, FlagAlignments[Index - 18]);
  }

  EXPECT_EQ(Slots[18].Offset, offsetof(RuntimeGuestStateX86_64V1, CF));
  EXPECT_EQ(Slots[19].Offset, offsetof(RuntimeGuestStateX86_64V1, PF));
  EXPECT_EQ(Slots[20].Offset, offsetof(RuntimeGuestStateX86_64V1, AF));
  EXPECT_EQ(Slots[21].Offset, offsetof(RuntimeGuestStateX86_64V1, ZF));
  EXPECT_EQ(Slots[22].Offset, offsetof(RuntimeGuestStateX86_64V1, SF));
  EXPECT_EQ(Slots[23].Offset, offsetof(RuntimeGuestStateX86_64V1, DF));
  EXPECT_EQ(Slots[24].Offset, offsetof(RuntimeGuestStateX86_64V1, OF));
}

TEST(RuntimeGuestState, X86_64LogicalStateRoundTripsWithoutLosingRFlags) {
  GuestState Logical =
      llvm::cantFail(createZeroedGuestState(GuestArchitecture::X86_64, 73));
  Logical.Features = {"avx2"};
  Logical.Memory.push_back({0x4000,
                            MemoryPermission::Read | MemoryPermission::Write,
                            9,
                            {0x10, 0x20, 0x30}});

  for (RegisterID ID = 0; ID != 16; ++ID)
    ASSERT_FALSE(static_cast<bool>(
        setRegisterValue(Logical, ID, llvm::APInt(64, 0x100000000ull + ID))));
  ASSERT_FALSE(static_cast<bool>(
      setRegisterValue(Logical, 16, llvm::APInt(64, 0x401020))));
  constexpr uint64_t PackedFlags = (uint64_t{1} << 63) | (uint64_t{3} << 12) |
                                   (uint64_t{1} << 11) | (uint64_t{1} << 10) |
                                   (uint64_t{1} << 7) | (uint64_t{1} << 6) |
                                   (uint64_t{1} << 4) | (uint64_t{1} << 2) |
                                   (uint64_t{1} << 1) | uint64_t{1};
  ASSERT_FALSE(static_cast<bool>(
      setRegisterValue(Logical, 17, llvm::APInt(64, PackedFlags))));
  const llvm::APInt XMM0(128, 0x8877665544332211ull);
  ASSERT_FALSE(static_cast<bool>(setRegisterValue(Logical, 32, XMM0)));

  llvm::Expected<RuntimeGuestStateX86_64V1> RuntimeOrErr =
      createRuntimeGuestStateX86_64V1(Logical);
  ASSERT_TRUE(static_cast<bool>(RuntimeOrErr))
      << llvm::toString(RuntimeOrErr.takeError());
  RuntimeGuestStateX86_64V1 Runtime = *RuntimeOrErr;
  EXPECT_EQ(Runtime.GPR[static_cast<size_t>(RuntimeX86_64GPRV1::RAX)],
            0x100000000ull);
  EXPECT_EQ(Runtime.GPR[static_cast<size_t>(RuntimeX86_64GPRV1::R15)],
            0x10000000full);
  EXPECT_EQ(Runtime.RIP, 0x401020u);
  EXPECT_EQ(Runtime.RFlagsBase, PackedFlags & ~kRuntimeX86_64SplitRFlagsMaskV1);
  EXPECT_EQ(Runtime.CF, 1u);
  EXPECT_EQ(Runtime.PF, 1u);
  EXPECT_EQ(Runtime.AF, 1u);
  EXPECT_EQ(Runtime.ZF, 1u);
  EXPECT_EQ(Runtime.SF, 1u);
  EXPECT_EQ(Runtime.DF, 1u);
  EXPECT_EQ(Runtime.OF, 1u);

  Runtime.GPR[static_cast<size_t>(RuntimeX86_64GPRV1::RAX)] =
      0xfeedfacecafebeefull;
  Runtime.RIP = 0x7fff00001000ull;
  Runtime.RFlagsBase =
      (uint64_t{1} << 63) | (uint64_t{1} << 1) | (uint64_t{1} << 18);
  Runtime.CF = 0;
  Runtime.PF = 1;
  Runtime.AF = 0;
  Runtime.ZF = 1;
  Runtime.SF = 0;
  Runtime.DF = 1;
  Runtime.OF = 0;

  ASSERT_FALSE(
      static_cast<bool>(applyRuntimeGuestStateX86_64V1(Runtime, Logical)));
  EXPECT_EQ(findRegisterValue(Logical, 0)->Value,
            llvm::APInt(64, 0xfeedfacecafebeefull));
  EXPECT_EQ(findRegisterValue(Logical, 16)->Value,
            llvm::APInt(64, 0x7fff00001000ull));
  constexpr uint64_t ExpectedFlags = (uint64_t{1} << 63) | (uint64_t{1} << 18) |
                                     (uint64_t{1} << 10) | (uint64_t{1} << 6) |
                                     (uint64_t{1} << 2) | (uint64_t{1} << 1);
  EXPECT_EQ(findRegisterValue(Logical, 17)->Value,
            llvm::APInt(64, ExpectedFlags));

  EXPECT_EQ(Logical.ThreadID, 73u);
  EXPECT_EQ(Logical.Features, std::vector<std::string>({"avx2"}));
  ASSERT_EQ(Logical.Memory.size(), 1u);
  EXPECT_EQ(Logical.Memory[0].Generation, 9u);
  EXPECT_EQ(findRegisterValue(Logical, 32)->Value, XMM0);
}

TEST(RuntimeGuestState, X86_64RuntimeStateFailsClosedAndAppliesAtomically) {
  auto ExpectInvalid = [](const RuntimeGuestStateX86_64V1 &State,
                          llvm::StringRef Detail) {
    llvm::Error Error = validateRuntimeGuestStateX86_64V1(State);
    ASSERT_TRUE(static_cast<bool>(Error));
    EXPECT_NE(llvm::toString(std::move(Error)).find(Detail.str()),
              std::string::npos);
  };

  GuestState Logical =
      llvm::cantFail(createZeroedGuestState(GuestArchitecture::X86_64));
  ASSERT_FALSE(static_cast<bool>(
      setRegisterValue(Logical, 0, llvm::APInt(64, 0x1122334455667788ull))));
  RuntimeGuestStateX86_64V1 Runtime =
      llvm::cantFail(createRuntimeGuestStateX86_64V1(Logical));

  RuntimeGuestStateX86_64V1 Invalid = Runtime;
  Invalid.Magic = 0;
  ExpectInvalid(Invalid, "magic");
  Invalid = Runtime;
  Invalid.Version = 2;
  ExpectInvalid(Invalid, "version");
  Invalid = Runtime;
  Invalid.Size = 0;
  ExpectInvalid(Invalid, "size");
  Invalid = Runtime;
  Invalid.Reserved0 = 1;
  ExpectInvalid(Invalid, "reserved");
  Invalid = Runtime;
  Invalid.RFlagsBase |= uint64_t{1} << 6;
  ExpectInvalid(Invalid, "canonical");
  Invalid = Runtime;
  Invalid.CF = 2;
  ExpectInvalid(Invalid, "boolean");

  Invalid.GPR[0] = 0xffffffffffffffffull;
  llvm::Error ApplyError = applyRuntimeGuestStateX86_64V1(Invalid, Logical);
  ASSERT_TRUE(static_cast<bool>(ApplyError));
  EXPECT_EQ(llvm::toString(std::move(ApplyError)),
            "x86-64 runtime-state flag is not boolean");
  EXPECT_EQ(findRegisterValue(Logical, 0)->Value,
            llvm::APInt(64, 0x1122334455667788ull));

  GuestState AArch64 =
      llvm::cantFail(createZeroedGuestState(GuestArchitecture::AArch64));
  llvm::Expected<RuntimeGuestStateX86_64V1> WrongArchitecture =
      createRuntimeGuestStateX86_64V1(AArch64);
  ASSERT_FALSE(static_cast<bool>(WrongArchitecture));
  EXPECT_NE(llvm::toString(WrongArchitecture.takeError()).find("x86-64 guest"),
            std::string::npos);
}

TEST(RuntimeGuestState, BlockExitV1ValuesLayoutAndShapesAreStable) {
  static_assert(static_cast<uint32_t>(BlockExitKindV1::Continue) == 0x100);
  static_assert(static_cast<uint32_t>(BlockExitKindV1::DirectBranch) == 0x101);
  static_assert(static_cast<uint32_t>(BlockExitKindV1::IndirectBranch) ==
                0x102);
  static_assert(static_cast<uint32_t>(BlockExitKindV1::Call) == 0x103);
  static_assert(static_cast<uint32_t>(BlockExitKindV1::Return) == 0x104);
  static_assert(static_cast<uint32_t>(BlockExitKindV1::Unsupported) == 0x105);
  static_assert(static_cast<uint32_t>(BlockExitKindV1::Syscall) == 0x106);
  static_assert(static_cast<uint32_t>(BlockExitKindV1::Trap) == 0x107);
  static_assert(std::is_standard_layout_v<BlockExitV1>);
  static_assert(std::is_trivially_copyable_v<BlockExitV1>);

  EXPECT_EQ(sizeof(BlockExitV1), kBlockExitSizeV1);
  EXPECT_EQ(alignof(BlockExitV1), 8u);
  EXPECT_EQ(offsetof(BlockExitV1, Kind), 8u);
  EXPECT_EQ(offsetof(BlockExitV1, PC), 16u);
  EXPECT_EQ(offsetof(BlockExitV1, NextPC), 24u);
  EXPECT_EQ(offsetof(BlockExitV1, TargetPC), 32u);
  EXPECT_EQ(offsetof(BlockExitV1, Detail), 40u);

  BlockExitV1 Exit;
  Exit.PC = 0x1000;
  Exit.NextPC = 0x1004;
  EXPECT_FALSE(static_cast<bool>(validateBlockExitV1(Exit)));

  Exit.Kind = BlockExitKindV1::DirectBranch;
  Exit.NextPC = 0;
  Exit.TargetPC = 0x2000;
  EXPECT_FALSE(static_cast<bool>(validateBlockExitV1(Exit)));

  Exit.Kind = BlockExitKindV1::IndirectBranch;
  EXPECT_FALSE(static_cast<bool>(validateBlockExitV1(Exit)));

  Exit.Kind = BlockExitKindV1::Call;
  Exit.NextPC = 0x1005;
  EXPECT_FALSE(static_cast<bool>(validateBlockExitV1(Exit)));

  Exit.Kind = BlockExitKindV1::Return;
  Exit.NextPC = 0;
  EXPECT_FALSE(static_cast<bool>(validateBlockExitV1(Exit)));

  Exit.Kind = BlockExitKindV1::Unsupported;
  Exit.TargetPC = 0;
  Exit.Detail = 0x91;
  EXPECT_FALSE(static_cast<bool>(validateBlockExitV1(Exit)));

  Exit.Kind = BlockExitKindV1::Syscall;
  Exit.NextPC = 0x1002;
  Exit.Detail = 60;
  EXPECT_FALSE(static_cast<bool>(validateBlockExitV1(Exit)));

  Exit.Kind = BlockExitKindV1::Trap;
  Exit.NextPC = 0;
  Exit.Detail = 3;
  EXPECT_FALSE(static_cast<bool>(validateBlockExitV1(Exit)));
}

TEST(RuntimeGuestState, BlockExitV1RejectsUnknownAndContradictoryRecords) {
  auto ExpectInvalid = [](BlockExitV1 Exit, llvm::StringRef Detail) {
    llvm::Error Error = validateBlockExitV1(Exit);
    ASSERT_TRUE(static_cast<bool>(Error));
    EXPECT_NE(llvm::toString(std::move(Error)).find(Detail.str()),
              std::string::npos);
  };

  BlockExitV1 Exit;
  Exit.PC = 0x1000;
  Exit.NextPC = 0x1004;

  BlockExitV1 Invalid = Exit;
  Invalid.Magic = 0;
  ExpectInvalid(Invalid, "magic");
  Invalid = Exit;
  Invalid.Version = 2;
  ExpectInvalid(Invalid, "version");
  Invalid = Exit;
  Invalid.Size = 0;
  ExpectInvalid(Invalid, "size");
  Invalid = Exit;
  Invalid.Reserved0 = 1;
  ExpectInvalid(Invalid, "reserved");
  Invalid = Exit;
  Invalid.Kind = static_cast<BlockExitKindV1>(0xffffffffu);
  ExpectInvalid(Invalid, "kind");

  Invalid = Exit;
  Invalid.TargetPC = 0x2000;
  ExpectInvalid(Invalid, "continue");
  Invalid = Exit;
  Invalid.Kind = BlockExitKindV1::DirectBranch;
  Invalid.TargetPC = 0x2000;
  ExpectInvalid(Invalid, "next PC");
  Invalid = Exit;
  Invalid.Kind = BlockExitKindV1::Return;
  Invalid.NextPC = 0;
  Invalid.TargetPC = 0x2000;
  Invalid.Detail = 1;
  ExpectInvalid(Invalid, "detail");
  Invalid = Exit;
  Invalid.Kind = BlockExitKindV1::Unsupported;
  Invalid.NextPC = 0;
  Invalid.TargetPC = 0x2000;
  ExpectInvalid(Invalid, "target");
  Invalid = Exit;
  Invalid.Kind = BlockExitKindV1::Syscall;
  Invalid.TargetPC = 0x2000;
  ExpectInvalid(Invalid, "target");
  Invalid = Exit;
  Invalid.Kind = BlockExitKindV1::Trap;
  Invalid.TargetPC = 0x2000;
  ExpectInvalid(Invalid, "target");
}

} // namespace
