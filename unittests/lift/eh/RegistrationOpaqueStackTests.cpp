//===- RegistrationOpaqueStackTests.cpp - Call-independent stack exits ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Explicit register restoration must not inherit unused import assumptions.
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/Limits.h"
#include "neverd/ir/low/RegistrationABI.h"
#include "neverd/loader/BinaryImage.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

using namespace neverd;

namespace {
struct OpaqueStackImage {
  static constexpr va_t Entry = 0x401000;
  static constexpr va_t Slot = 0x402000;
  BinaryImage Image;

  explicit OpaqueStackImage(const std::vector<uint8_t> &Code) {
    Image.Arch = Arch::X86;
    Image.Bits = Bitness::Bits32;
    Image.Format = BinaryFormat::COFF;
    Image.Base = 0x400000;
    Image.Entry = Entry;
    Segment Text;
    Text.VA = Entry;
    Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
    Text.Data = Code;
    Text.Size = Text.FileSz = Code.size();
    Image.Segments.push_back(Text);
    Segment Data;
    Data.VA = Slot;
    Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
    Data.Data.resize(4);
    Data.Size = Data.FileSz = Data.Data.size();
    Image.Segments.push_back(Data);
    for (const auto &Segment : Image.Segments) {
      Section Section;
      Section.VA = Segment.VA;
      Section.Size = Section.FileSz = Segment.Size;
      Section.Flags = Segment.Flags;
      Section.Data = Segment.Data;
      Image.Sections.push_back(Section);
    }
    Import Imported;
    Imported.Module = "KERNEL32.dll";
    Imported.Name = "Sleep";
    Imported.IATAddr = Slot;
    Image.Imports.push_back(Imported);
    EXPECT_TRUE(Image.recordImportStorageSlot(
        Slot, Imported.Name, 0, ImportStorageEvidence::ImportDirectory));
    Image.Symbols.push_back(Symbol::makeFunc(Entry));
  }

  std::optional<uint32_t> independent(size_t &Work) {
    return getCheckedX86IndependentCalleeStackPop(
        Image, Entry, Work, limits::kMaxRegistrationEHStateWork);
  }
};

std::vector<uint8_t> anchoredCall(unsigned Register, unsigned Pop) {
  // Save a nonvolatile anchor, allocate storage, then call through a live IAT.
  // Its arbitrary returning cleanup is discarded by MOV ESP,anchor.
  std::vector<uint8_t> Code{uint8_t(0x50 + Register),
                            0x8b,
                            uint8_t(0xc4 + Register * 8),
                            0x83,
                            0xec,
                            0x10,
                            0x6a,
                            0,
                            0xff,
                            0x15,
                            0,
                            0x20,
                            0x40,
                            0,
                            0x8b,
                            uint8_t(0xe0 + Register),
                            uint8_t(0x58 + Register),
                            0xc2,
                            uint8_t(Pop),
                            uint8_t(Pop >> 8)};
  return Code;
}

TEST(RegistrationOpaqueStack, ExplicitAnchorsNeedNoImportBinding) {
  for (unsigned Register : {3u, 5u, 6u, 7u})
    for (unsigned Pop : {0u, 4u, 12u, 65532u}) {
      SCOPED_TRACE(::testing::Message() << Register << ':' << Pop);
      OpaqueStackImage F(anchoredCall(Register, Pop));
      const auto Contract = RegistrationCallCalleeIndex(F.Image).stackContract(
          OpaqueStackImage::Entry);
      ASSERT_TRUE(Contract);
      EXPECT_EQ(Contract->StackPopBytes, Pop);
      EXPECT_EQ(Contract->RequiredImports,
                std::vector<va_t>{OpaqueStackImage::Slot});
      EXPECT_FALSE(Contract->PreservesImportBindings);
      size_t Work = 0;
      EXPECT_EQ(F.independent(Work), Pop);
      EXPECT_GT(Work, 0u);
      const auto After = RegistrationCallCalleeIndex(F.Image).stackContract(
          OpaqueStackImage::Entry);
      ASSERT_TRUE(After);
      EXPECT_EQ(After->RequiredImports, Contract->RequiredImports);
      EXPECT_EQ(After->PreservesImportBindings,
                Contract->PreservesImportBindings);
    }
}

TEST(RegistrationOpaqueStack, CallEffectsCannotRestoreAnUnprovedAnchor) {
  for (unsigned Mutation = 0; Mutation != 6; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Code = anchoredCall(5, 0);
    if (Mutation == 0) // The live IAT convention is needed without restoration.
      Code[14] = Code[15] = 0x90;
    if (Mutation == 1) // EAX is volatile across the call.
      Code = anchoredCall(0, 0);
    if (Mutation == 2) // A memory reload cannot prove saved-stack identity.
      Code.insert(Code.begin() + 14, {0x8b, 0x2d, 0, 0x20, 0x40, 0});
    if (Mutation == 3) // Partial anchor writes revoke the full coordinate.
      Code.insert(Code.begin() + 14, {0x66, 0x31, 0xed});
    if (Mutation == 4) // An extra pop after restoration changes the frame.
      Code.insert(Code.begin() + 17, {0x59});
    if (Mutation == 5) // Dynamic stack arithmetic has no constant return fact.
      Code.insert(Code.begin() + 17, {0x03, 0xe0});
    OpaqueStackImage F(Code);
    size_t Work = 0;
    EXPECT_FALSE(F.independent(Work));
  }
}

TEST(RegistrationOpaqueStack, EveryPhysicalReturnMustAgree) {
  for (bool Agree : {false, true}) {
    auto Code = anchoredCall(5, 4);
    Code.resize(17);
    Code.insert(Code.end(), {0x85, 0xc0, 0x74, 3, 0xc2, 4, 0, 0xc2,
                             uint8_t(Agree ? 4 : 8), 0});
    OpaqueStackImage F(Code);
    size_t Work = 0;
    EXPECT_EQ(F.independent(Work),
              Agree ? std::optional<uint32_t>{4} : std::nullopt);
  }
}

TEST(RegistrationOpaqueStack, LoopsAndBypassedRestorationKeepEveryEdge) {
  for (unsigned Variant = 0; Variant != 3; ++Variant) {
    auto Code = anchoredCall(5, 4);
    if (Variant == 0) // A stable loop preserves the nonvolatile anchor.
      Code.insert(Code.begin() + 14, {0x85, 0xc0, 0x75, 0xfc});
    if (Variant == 1) // One loop edge can overwrite the anchor.
      Code.insert(Code.begin() + 14, {0x8b, 0xe8, 0x85, 0xc0, 0x75, 0xfa});
    if (Variant == 2) // A branch bypasses MOV ESP,EBP and POP EBP.
      Code.insert(Code.begin() + 14, {0x85, 0xc0, 0x74, 3});
    OpaqueStackImage F(Code);
    size_t Work = 0;
    EXPECT_EQ(F.independent(Work),
              Variant == 0 ? std::optional<uint32_t>{4} : std::nullopt);
    EXPECT_LE(Work, limits::kMaxRegistrationEHStateWork);
  }
}

TEST(RegistrationOpaqueStack, CurrentBytesAndBudgetRemainAuthoritative) {
  OpaqueStackImage F(anchoredCall(5, 8));
  size_t Work = 0;
  ASSERT_EQ(F.independent(Work), 8u);
  const size_t Needed = Work;
  for (size_t Budget : {size_t{0}, size_t{1}, Needed - 1, Needed}) {
    Work = 0;
    const auto Pop = getCheckedX86IndependentCalleeStackPop(
        F.Image, F.Image.Entry, Work, Budget);
    EXPECT_EQ(Pop.has_value(), Budget == Needed);
    EXPECT_LE(Work, Budget);
  }
  for (unsigned Pop : {12u, 4u, 8u}) {
    F.Image.Segments.front().Data[18] = uint8_t(Pop);
    Work = 0;
    EXPECT_EQ(F.independent(Work), Pop);
  }
  F.Image.Segments.front().Flags =
      F.Image.Segments.front().Flags | SegmentFlags::Writable;
  Work = 0;
  EXPECT_FALSE(F.independent(Work));
}

TEST(RegistrationOpaqueStack, IndependentProjectionKeepsTheOrdinaryMemo) {
  using Kind = RegistrationStackPopMemo::QueryKind;
  OpaqueStackImage F(anchoredCall(5, 8));
  size_t OrdinaryWork = 0, IndependentWork = 0;
  ASSERT_EQ(getCheckedX86CalleeStackPop(F.Image, F.Image.Entry, &OrdinaryWork),
            8u);
  ASSERT_EQ(F.independent(IndependentWork), 8u);
  ASSERT_GT(IndependentWork, OrdinaryWork);
  for (bool IndependentFirst : {false, true}) {
    RegistrationStackPopMemo Memo(F.Image);
    RegistrationStackPopMemoScope Scope(Memo);
    size_t Work = 0;
    if (IndependentFirst)
      ASSERT_EQ(F.independent(Work), 8u);
    else
      ASSERT_EQ(getCheckedX86CalleeStackPop(F.Image, F.Image.Entry, &Work), 8u);
    const auto Known = Memo.find(F.Image, F.Image.Entry, 0);
    ASSERT_TRUE(Known);
    EXPECT_EQ(Known->Pop, 8u);
    EXPECT_EQ(Known->Work, OrdinaryWork);
    EXPECT_TRUE(Known->HasImportDependencies);
    for (size_t Budget : {size_t{1}, IndependentWork - 1, IndependentWork}) {
      Work = 13;
      const auto Pop = getCheckedX86IndependentCalleeStackPop(
          F.Image, F.Image.Entry, Work, 13 + Budget);
      EXPECT_EQ(Pop.has_value(), Budget == IndependentWork);
      EXPECT_EQ(Work, 13 + Budget);
    }
    Work = 0;
    EXPECT_EQ(F.independent(Work), 8u);
    EXPECT_EQ(Work, IndependentWork);
    const auto Projection =
        Memo.find(F.Image, F.Image.Entry, 0, Kind::OpaqueProjection);
    ASSERT_TRUE(Projection);
    EXPECT_EQ(Projection->Pop, 8u);
    EXPECT_EQ(Projection->Work, IndependentWork - OrdinaryWork);
    EXPECT_FALSE(Projection->HasImportDependencies);
    EXPECT_FALSE(Memo.find(F.Image, F.Image.Entry, 1, Kind::OpaqueProjection));
    Work = 0;
    EXPECT_EQ(getCheckedX86CalleeStackPop(F.Image, F.Image.Entry, &Work), 8u);
    EXPECT_EQ(Work, OrdinaryWork);
  }
}

TEST(RegistrationOpaqueStack, MemoRefusalsKeepBudgetsAndImageIdentity) {
  using Kind = RegistrationStackPopMemo::QueryKind;
  auto Code = anchoredCall(5, 8);
  // This valid ordinary frame needs the import's cleanup. Replacing the
  // restoration with constant arithmetic cannot remove that dependency.
  Code.erase(Code.begin() + 14, Code.begin() + 16);
  Code.insert(Code.begin() + 14, {0x83, 0xc4, 0x10});
  OpaqueStackImage F(Code);
  size_t OrdinaryWork = 0, IndependentWork = 0;
  ASSERT_EQ(getCheckedX86CalleeStackPop(F.Image, F.Image.Entry, &OrdinaryWork),
            8u);
  ASSERT_FALSE(F.independent(IndependentWork));
  ASSERT_GT(IndependentWork, OrdinaryWork + 1);
  RegistrationStackPopMemo Memo(F.Image);
  RegistrationStackPopMemoScope Scope(Memo);
  size_t Work = 0;
  EXPECT_FALSE(getCheckedX86IndependentCalleeStackPop(F.Image, F.Image.Entry,
                                                      Work, OrdinaryWork + 1));
  EXPECT_EQ(Work, OrdinaryWork + 1);
  EXPECT_FALSE(Memo.find(F.Image, F.Image.Entry, 0, Kind::OpaqueProjection));
  for (unsigned Attempt = 0; Attempt != 2; ++Attempt) {
    Work = 0;
    EXPECT_FALSE(F.independent(Work));
    EXPECT_EQ(Work, IndependentWork);
    const auto Known =
        Memo.find(F.Image, F.Image.Entry, 0, Kind::OpaqueProjection);
    ASSERT_TRUE(Known);
    EXPECT_FALSE(Known->Pop);
    EXPECT_EQ(Known->Work, IndependentWork - OrdinaryWork);
    Work = 11;
    EXPECT_FALSE(getCheckedX86IndependentCalleeStackPop(
        F.Image, F.Image.Entry, Work, 11 + IndependentWork - 1));
    EXPECT_EQ(Work, 11 + IndependentWork - 1);
  }
  OpaqueStackImage Other(anchoredCall(5, 12));
  Work = 0;
  EXPECT_EQ(Other.independent(Work), 12u);
  EXPECT_FALSE(
      Memo.find(Other.Image, Other.Image.Entry, 0, Kind::OpaqueProjection));
}
} // namespace
