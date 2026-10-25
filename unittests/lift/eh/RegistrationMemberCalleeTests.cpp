//===- RegistrationMemberCalleeTests.cpp - PE32 scalar member ABI ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Check scalar member arguments, result policies and bounded graph reuse.
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/Limits.h"
#include "neverd/ir/low/RegistrationABI.h"
#include "neverd/loader/BinaryImage.h"

namespace {
using namespace neverd;

BinaryImage memberImage(std::vector<uint8_t> Code) {
  BinaryImage Image;
  Image.Arch = Arch::X86;
  Image.Bits = Bitness::Bits32;
  Image.Format = BinaryFormat::COFF;
  Image.Entry = 0x1000;
  Segment Text;
  Text.VA = Image.Entry;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data = std::move(Code);
  Text.Size = Text.FileSz = Text.Data.size();
  Section CodeSection;
  CodeSection.Name = ".text";
  CodeSection.VA = Text.VA;
  CodeSection.Flags = Text.Flags;
  CodeSection.Size = CodeSection.FileSz = Text.Size;
  Image.Sections.push_back(std::move(CodeSection));
  Image.Symbols.push_back(Symbol::makeFunc(Image.Entry));
  Image.Segments.push_back(std::move(Text));
  return Image;
}

TEST(RegistrationMemberCallee, BindsScalarWordsWithoutChangingCRTPolicy) {
  // mov eax,[ecx]; add eax,[esp+4]; ret 4
  const auto Image = memberImage({0x8b, 0x01, 0x03, 0x44, 0x24, 4, 0xc2, 4, 0});
  EXPECT_FALSE(getCheckedX86RegistrationLeafCalleeABI(Image, Image.Entry));
  EXPECT_FALSE(getCheckedX86RegistrationObjectCalleeABI(Image, Image.Entry));
  const auto ABI = getCheckedX86RegistrationMemberCalleeABI(Image, Image.Entry);
  ASSERT_TRUE(ABI);
  EXPECT_EQ(ABI->StackPopBytes, 4u);
  EXPECT_TRUE(ABI->HasIndependentScalarReturn);
  EXPECT_EQ(ABI->ECXReads, (std::vector<RegistrationObjectExtent>{{0, 4}}));
  EXPECT_TRUE(ABI->ECXWrites.empty());
  EXPECT_TRUE(ABI->CallerPCWrites.empty());
}

TEST(RegistrationMemberCallee, ScalarMembersNeedOnlyOneObjectGraphProof) {
  const auto Image = memberImage({0x8b, 0x01, 0x03, 0x44, 0x24, 4, 0xc2, 4, 0});
  // The object API proves the graph before rejecting its scalar stack argument.
  // The member API must use that same complete graph within the same allowance.
  size_t Cost = 0;
  EXPECT_FALSE(
      getCheckedX86RegistrationObjectCalleeABI(Image, Image.Entry, &Cost));
  ASSERT_GT(Cost, 0u);
  ASSERT_LT(Cost, limits::kMaxRegistrationEHStateWork);
  size_t Work = limits::kMaxRegistrationEHStateWork - Cost;
  const auto ABI =
      getCheckedX86RegistrationMemberCalleeABI(Image, Image.Entry, &Work);
  ASSERT_TRUE(ABI);
  EXPECT_EQ(ABI->StackPopBytes, 4u);
  EXPECT_TRUE(ABI->HasIndependentScalarReturn);
  EXPECT_EQ(ABI->ECXReads, (std::vector<RegistrationObjectExtent>{{0, 4}}));
  EXPECT_EQ(Work, limits::kMaxRegistrationEHStateWork);
  Work = limits::kMaxRegistrationEHStateWork - Cost + 1;
  EXPECT_FALSE(
      getCheckedX86RegistrationMemberCalleeABI(Image, Image.Entry, &Work));
  EXPECT_EQ(Work, limits::kMaxRegistrationEHStateWork);
}

TEST(RegistrationMemberCallee, KeepsZeroArgumentGraphAndReturnPolicies) {
  auto Image =
      memberImage({0xe8, 0x0b, 0, 0, 0, 0xc3, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc,
                   0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0x8b, 0x01, 0xc3});
  Image.Symbols.push_back(Symbol::makeFunc(Image.Entry + 16));
  const auto Object =
      getCheckedX86RegistrationObjectCalleeABI(Image, Image.Entry);
  const auto Member =
      getCheckedX86RegistrationMemberCalleeABI(Image, Image.Entry);
  ASSERT_TRUE(Object);
  ASSERT_TRUE(Member);
  EXPECT_EQ(Member->StackPopBytes, 0u);
  EXPECT_EQ(Member->ECXReads, Object->ECXReads);
  EXPECT_TRUE(Member->HasIndependentScalarReturn);
  // Returning the borrowed pointer cannot acquire an observed scalar contract.
  Image.Segments.front().Data[16] = 0x89;
  Image.Segments.front().Data[17] = 0xc8;
  EXPECT_FALSE(getCheckedX86RegistrationMemberCalleeABI(Image, Image.Entry));
  const auto Discarded = getCheckedX86RegistrationMemberCalleeABI(
      Image, Image.Entry, nullptr, RegistrationLeafResult::Discarded);
  ASSERT_TRUE(Discarded);
  EXPECT_EQ(Discarded->StackPopBytes, 0u);
  EXPECT_FALSE(Discarded->HasIndependentScalarReturn);
}

TEST(RegistrationMemberCallee, RefusesUncheckedWordsAndCallingConventions) {
  const std::pair<const char *, std::vector<uint8_t>> Cases[] = {
      {"word beyond pop", {0x8b, 1, 0x03, 0x44, 0x24, 8, 0xc2, 4, 0}},
      {"unaligned pop", {0x8b, 1, 0x03, 0x44, 0x24, 4, 0xc2, 6, 0}},
      {"argument dereference",
       {0x8b, 0x44, 0x24, 4, 0x8b, 0, 0x03, 1, 0xc2, 4, 0}},
      {"argument write", {0x89, 0x44, 0x24, 4, 0x8b, 1, 0xc2, 4, 0}},
      {"nonvolatile change", {0x31, 0xdb, 0x8b, 1, 0xc2, 4, 0}},
      {"no ECX object", {0x8b, 0x44, 0x24, 4, 0xc2, 4, 0}}};
  for (const auto &[Name, Code] : Cases) {
    SCOPED_TRACE(Name);
    const auto Image = memberImage(Code);
    EXPECT_FALSE(getCheckedX86RegistrationMemberCalleeABI(Image, Image.Entry));
    EXPECT_FALSE(getCheckedX86RegistrationMemberCalleeABI(
        Image, Image.Entry, nullptr, RegistrationLeafResult::Discarded));
  }
}

TEST(RegistrationMemberCallee, KeepsReturnAndWorkProofsIndependent) {
  auto Image = memberImage({0x8b, 1, 0x89, 0xc8, 0xc2, 4, 0});
  EXPECT_FALSE(getCheckedX86RegistrationMemberCalleeABI(Image, Image.Entry));
  const auto Void = getCheckedX86RegistrationMemberCalleeABI(
      Image, Image.Entry, nullptr, RegistrationLeafResult::Discarded);
  ASSERT_TRUE(Void);
  EXPECT_FALSE(Void->HasIndependentScalarReturn);
  size_t Work = limits::kMaxRegistrationEHStateWork;
  EXPECT_FALSE(
      getCheckedX86RegistrationMemberCalleeABI(Image, Image.Entry, &Work));
  Image.Segments[0].Flags = Image.Segments[0].Flags | SegmentFlags::Writable;
  EXPECT_FALSE(getCheckedX86RegistrationMemberCalleeABI(
      Image, Image.Entry, nullptr, RegistrationLeafResult::Discarded));
}
} // namespace
