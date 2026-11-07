//===- RegistrationLeafBranchTests.cpp - PE32 feasible leaf effects -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Keep leaf effects conditional on current all-path branch evidence.
//===----------------------------------------------------------------------===//

#include "../../../lib/ir/low/X86/RegistrationABIPrivate.h"
#include "../../../lib/ir/low/X86/RegistrationStackOwner.h"
#include "gtest/gtest.h"

#include "neverd/Limits.h"
#include "neverd/ir/low/RegistrationABI.h"
#include "neverd/ir/low/RegistrationLeafPrefixMemo.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/support/BinaryEncoding.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

using namespace neverd;

namespace {
constexpr va_t TextVA = 0x401000;
constexpr va_t DataVA = 0x403000;

BinaryImage branchImage(const std::vector<uint8_t> &Code) {
  BinaryImage Image;
  Image.Arch = Arch::X86;
  Image.Bits = Bitness::Bits32;
  Image.Format = BinaryFormat::COFF;
  Image.Base = 0x400000;
  Image.Entry = TextVA;
  Segment Text;
  Text.Name = ".text";
  Text.VA = TextVA;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data = Code;
  Text.Data.resize(0x81, 0xcc);
  Text.Data[0x80] = 0xc3;
  Text.Size = Text.FileSz = Text.Data.size();
  Image.Segments.push_back(Text);
  Segment Data;
  Data.Name = ".data";
  Data.VA = DataVA;
  Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
  Data.Data.resize(16);
  Data.Size = Data.FileSz = Data.Data.size();
  Image.Segments.push_back(Data);
  for (const auto &Segment : Image.Segments) {
    Section Section;
    Section.Name = Segment.Name;
    Section.VA = Segment.VA;
    Section.Size = Section.FileSz = Segment.Size;
    Section.Flags = Segment.Flags;
    Image.Sections.push_back(Section);
  }
  Image.Symbols.push_back(Symbol::makeFunc(TextVA, Code.size()));
  Image.Symbols.push_back(Symbol::makeFunc(TextVA + 0x80, 1));
  return Image;
}
} // namespace

TEST(RegistrationLeafBranch, EffectsFollowTakenAndFallthroughEvidence) {
  for (bool TakenIsExit : {false, true})
    for (unsigned Condition : {0, 1, 2})
      for (unsigned Effect : {0, 1, 2}) {
        SCOPED_TRACE(::testing::Message()
                     << TakenIsExit << ':' << Condition << ':' << Effect);
        // A mutable zero-filled word is still unknown at this invocation.
        std::vector<uint8_t> Code =
            Condition == 2 ? std::vector<uint8_t>{0xa1, 0, 0, 0, 0}
                           : std::vector<uint8_t>{0xb8, 0, 0, 0, 0};
        writeLE<uint32_t>(Code.data() + 1, Condition == 2 ? DataVA : Condition);
        Code.insert(Code.end(), {0x85, 0xc0, 0x74, 0}); // TEST EAX; JZ.
        const size_t Displacement = Code.size() - 1;
        const size_t Fallthrough = Code.size();
        if (!TakenIsExit)
          Code.insert(Code.end(), {0x31, 0xc0, 0xc3});
        const size_t EffectOffset = Code.size();
        if (Effect == 0)
          Code.insert(Code.end(), {0xff, 0xd2}); // Opaque CALL EDX.
        else if (Effect == 1) {
          Code.insert(Code.end(), {0xe8, 0, 0, 0, 0});
          writeLE<uint32_t>(Code.data() + EffectOffset + 1,
                            0x80 - (EffectOffset + 5));
        } else {
          Code.insert(Code.end(), {0xa3, 0, 0, 0, 0}); // Image write.
          writeLE<uint32_t>(Code.data() + EffectOffset + 1, DataVA + 4);
        }
        const size_t ExitOffset = Code.size();
        Code.insert(Code.end(), {0x31, 0xc0, 0xc3});
        Code[Displacement] =
            (TakenIsExit ? ExitOffset : EffectOffset) - Fallthrough;
        const auto Image = branchImage(Code);
        const bool EffectReached =
            Condition == 2 || (TakenIsExit ? Condition != 0 : Condition == 0);
        for (auto Result : {RegistrationLeafResult::Observed,
                            RegistrationLeafResult::Discarded}) {
          size_t Work = 0;
          const auto Leaf = getCheckedX86RegistrationLeafCalleeABI(
              Image, Image.Entry, &Work, Result);
          ASSERT_EQ(bool(Leaf), Effect == 2 || !EffectReached);
          if (!Leaf)
            continue;
          EXPECT_TRUE(Leaf->HasIndependentScalarReturn);
          EXPECT_EQ(Leaf->ImageReads.size(), Condition == 2 ? 1u : 0u);
          EXPECT_EQ(Leaf->ImageWrites.size(), EffectReached ? 1u : 0u);
          if (EffectReached) {
            EXPECT_EQ(Leaf->ImageWrites.front().Begin, DataVA + 4);
            EXPECT_EQ(Leaf->ImageWrites.front().End, DataVA + 8);
          }
          if (Condition == 2) {
            EXPECT_EQ(Leaf->ImageReads.front().Begin, DataVA);
            EXPECT_EQ(Leaf->ImageReads.front().End, DataVA + 4);
          }
          Work = limits::kMaxRegistrationEHStateWork;
          EXPECT_FALSE(getCheckedX86RegistrationLeafCalleeABI(
              Image, Image.Entry, &Work, Result));
        }
      }
}

TEST(RegistrationLeafBranch, BackedgesReopenAFormerlyConstantChoice) {
  for (bool Reload : {false, true}) {
    SCOPED_TRACE(Reload);
    // The first visit cannot reach the call. The backedge supplies either a
    // different constant or a mutable loaded value; neither keeps that proof.
    std::vector<uint8_t> Code = {
        0x31, 0xc0, 0x85, 0xc0, 0x75, 7,    uint8_t(Reload ? 0xa1 : 0xb8),
        0,    0,    0,    0,    0xeb, 0xf5, 0xff,
        0xd2, 0x31, 0xc0, 0xc3};
    writeLE<uint32_t>(Code.data() + 7, Reload ? DataVA : 1);
    const auto Image = branchImage(Code);
    for (auto Result :
         {RegistrationLeafResult::Observed, RegistrationLeafResult::Discarded})
      EXPECT_FALSE(getCheckedX86RegistrationLeafCalleeABI(Image, Image.Entry,
                                                          nullptr, Result));
  }
}

TEST(RegistrationLeafBranch, PrefixRefusalsKeepColdBudgetsAndChildContexts) {
  struct Case {
    std::vector<uint8_t> Code;
    unsigned Mode;
    uint32_t Pop;
    bool Retained;
  };
  const std::vector<uint8_t> Indirect{0xff, 0xd2, 0xc3};
  const std::vector<uint8_t> Empty{0xc3};
  const std::vector<uint8_t> Pop{0xb8, 42, 0, 0, 0, 0xc2, 4, 0};
  const std::vector<uint8_t> Dead{0x31, 0xc0, 0x74, 2, 0xff, 0xd2,
                                  0xb8, 42,   0,    0, 0,    0xc3};
  const std::vector<uint8_t> Nested{0xe8, 0x7b, 0, 0, 0, 0xff, 0xd2, 0xc3};
  for (const Case &C : std::vector<Case>{{Indirect, 0, 0, false},
                                         {Indirect, 1, 0, true},
                                         {Indirect, 2, 0, true},
                                         {Empty, 0, 0, false},
                                         {Empty, 1, 0, false},
                                         {Empty, 2, 0, false},
                                         {Pop, 0, 0, true},
                                         {Pop, 1, 0, true},
                                         {Pop, 1, 4, false},
                                         {Pop, 2, 4, false},
                                         {Dead, 0, 0, false},
                                         {Dead, 1, 0, true},
                                         {Dead, 2, 0, true},
                                         {Nested, 1, 0, false},
                                         {Nested, 2, 0, false}}) {
    SCOPED_TRACE(::testing::Message()
                 << C.Code.size() << ':' << C.Mode << ':' << C.Pop);
    const auto Image = branchImage(C.Code);
    struct Answer {
      bool Success;
      size_t Children;
      bool Initialized;
      bool operator==(const Answer &) const = default;
    };
    auto Query = [&](size_t &Work, bool WarmChild = false) {
      registration_abi::InitializerContext Initializers;
      registration_abi::ObjectCallbackContext Callbacks;
      registration_abi::InitializerFrame Frame{C.Pop, std::nullopt};
      if (WarmChild) {
        size_t ChildWork = 0;
        if (C.Mode == 2)
          registration_abi::checkedInitializerCalleeABI(
              Image, Image.Entry + 0x80, ChildWork, Initializers);
        else
          registration_abi::checkedObjectCallbackCalleeABI(
              Image, Image.Entry + 0x80, ChildWork, Callbacks);
      }
      const auto Result = registration_abi::checkedLeafCalleeABI(
          Image, Image.Entry, Work,
          C.Mode == 1 ? RegistrationLeafResult::Discarded
                      : RegistrationLeafResult::Observed,
          0, nullptr, C.Mode == 2 ? &Frame : nullptr,
          C.Mode == 2 ? &Initializers : nullptr,
          C.Mode == 1 ? &Callbacks : nullptr, C.Mode == 1 ? C.Pop : 0);
      return Answer{bool(Result),
                    C.Mode == 2 ? Initializers.Callees.size()
                                : Callbacks.Callees.size(),
                    Frame.Initialized.has_value()};
    };
    const uint32_t KeyPop = C.Mode ? C.Pop : 0;
    const bool ClosedCalls = C.Mode != 0;
    constexpr size_t Limit = limits::kMaxRegistrationEHStateWork;
    size_t FreshWork = 0;
    const auto Fresh = Query(FreshWork);
    ASSERT_GT(FreshWork, 1u);
    ASSERT_LT(FreshWork, Limit);
    RegistrationLeafPrefixMemo Memo(Image);
    RegistrationLeafPrefixMemoScope Scope(Memo);
    size_t Work = 0;
    EXPECT_EQ(Query(Work), Fresh);
    EXPECT_EQ(Work, FreshWork);
    const auto Known = Memo.find(Image, Image.Entry, KeyPop, ClosedCalls, 0);
    ASSERT_EQ(Known.has_value(), C.Retained);
    if (Known)
      EXPECT_EQ(*Known, FreshWork);
    EXPECT_FALSE(Memo.find(Image, Image.Entry, KeyPop, ClosedCalls, 1));
    for (bool WarmChild : {false, true})
      for (size_t Before :
           {size_t{0}, size_t{17}, Limit - FreshWork - 1, Limit - FreshWork,
            Limit - FreshWork + 1, Limit - 1, Limit, Limit + 1}) {
        SCOPED_TRACE(::testing::Message() << WarmChild << ':' << Before);
        size_t Cached = Before, Cold = Before;
        const auto Actual = Query(Cached, WarmChild);
        // A scope for another image disables reuse for this whole query,
        // including its children, without changing any proof policy.
        BinaryImage Unrelated;
        RegistrationLeafPrefixMemo Disabled(Unrelated);
        RegistrationLeafPrefixMemoScope Inner(Disabled);
        EXPECT_EQ(Actual, Query(Cold, WarmChild));
        EXPECT_EQ(Cached, Cold);
      }
    {
      RegistrationLeafPrefixMemo EmptyMemo(Image);
      RegistrationLeafPrefixMemoScope Inner(EmptyMemo);
      Work = Limit - 1;
      EXPECT_FALSE(Query(Work).Success);
      EXPECT_FALSE(EmptyMemo.find(Image, Image.Entry, KeyPop, ClosedCalls, 0));
    }
    for (bool SharedWork : {false, true}) {
      Work = 0;
      size_t OwnerWork = 29;
      RegistrationCallCalleeIndex Owner(Image, SharedWork ? &Work : &OwnerWork);
      registration_abi::StackProofOwnerScope Active(
          Owner, SharedWork ? Work : OwnerWork);
      EXPECT_EQ(Query(Work), Fresh);
      EXPECT_EQ(Work, FreshWork);
      EXPECT_EQ(OwnerWork, 29u);
    }
  }
}

TEST(RegistrationLeafBranch, PrefixRefusalsKeepImageAndInvocationDomains) {
  auto Image =
      branchImage({0x31, 0xc0, 0x74, 2, 0xff, 0xd2, 0xb8, 42, 0, 0, 0, 0xc3});
  const auto Other = branchImage({0x31, 0xc0, 0xc3});
  {
    RegistrationLeafPrefixMemo Memo(Image);
    RegistrationLeafPrefixMemoScope Scope(Memo);
    registration_abi::ObjectCallbackContext Callbacks;
    size_t Work = 0;
    EXPECT_FALSE(registration_abi::checkedLeafCalleeABI(
        Image, Image.Entry, Work, RegistrationLeafResult::Discarded, 0, nullptr,
        nullptr, nullptr, &Callbacks, 0));
    ASSERT_TRUE(Memo.find(Image, Image.Entry, 0, true, 0));
    // The ordinary leaf interpreter proves the indirect call unreachable.
    // An object graph's stronger structural requirement cannot reject it.
    Work = 0;
    EXPECT_TRUE(
        getCheckedX86RegistrationLeafCalleeABI(Image, Image.Entry, &Work));
    Work = 0;
    EXPECT_TRUE(
        getCheckedX86RegistrationLeafCalleeABI(Other, Other.Entry, &Work));
    EXPECT_FALSE(Memo.find(Other, Other.Entry, 0, true, 0));
  }
  // A new image operation receives a new memo; source changes between them
  // must not retain a refusal from the prior instruction stream.
  Image = Other;
  RegistrationLeafPrefixMemo NewMemo(Image);
  RegistrationLeafPrefixMemoScope NewScope(NewMemo);
  registration_abi::ObjectCallbackContext Callbacks;
  size_t Work = 0;
  EXPECT_TRUE(registration_abi::checkedLeafCalleeABI(
      Image, Image.Entry, Work, RegistrationLeafResult::Discarded, 0, nullptr,
      nullptr, nullptr, &Callbacks, 0));
  EXPECT_FALSE(NewMemo.find(Image, Image.Entry, 0, true, 0));
}
