//===- RegistrationTryLevelTests.cpp - x86-32 try-level recovery ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// An `_except_handler3`/`_except_handler4` scope table is indexed by a try
/// level the frame holds, not by address, so on its own it says which handlers
/// a function has but never which code they guard.  That mapping exists only in
/// the stores the body makes into the frame's try-level slot, and which slot
/// that is appears nowhere in the image.  These tests pin the evidence the slot
/// is proven from, the ordinary locals that evidence has to reject, and the
/// exceptional edges the recovered regions produce.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/decode/Decoder.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/loader/COFF/COFFRegistrationEH.h"
#include "neverd/loader/ExceptionInfo.h"

#include <string>
#include <vector>

namespace {

using namespace neverd;

constexpr va_t kBase = 0x400000;
constexpr va_t kText = 0x401000;
constexpr va_t kRData = 0x402000;
constexpr va_t kPersonality = 0x401100;

/// Append the little-endian bytes of \p Value to \p Out.
void emit32(std::vector<uint8_t> &Out, uint32_t Value) {
  for (unsigned Byte = 0; Byte < 4; ++Byte)
    Out.push_back(static_cast<uint8_t>(Value >> (Byte * 8)));
}

/// The prologue every MSVC x86 frame with a scope table emits: establish the
/// frame, push the seed try level, the table, and the handler, then link the
/// record in front of the one `FS:[0]` names.
void emitInstall(std::vector<uint8_t> &Out, int8_t Seed, uint32_t TableVA) {
  Out.insert(Out.end(), {0x55});             // push ebp
  Out.insert(Out.end(), {0x8B, 0xEC});       // mov ebp, esp
  Out.insert(Out.end(), {0x6A, static_cast<uint8_t>(Seed)}); // push seed
  Out.push_back(0x68);                       // push imm32
  emit32(Out, TableVA);
  Out.push_back(0x68);                       // push imm32
  emit32(Out, static_cast<uint32_t>(kPersonality));
  Out.insert(Out.end(), {0x64, 0xA1, 0, 0, 0, 0});    // mov eax, fs:[0]
  Out.push_back(0x50);                                // push eax
  Out.insert(Out.end(), {0x64, 0x89, 0x25, 0, 0, 0, 0}); // mov fs:[0], esp
}

/// `mov dword ptr [ebp+Displacement], Value`, the shape a try-level change and
/// an ordinary initialized local share.
void emitFrameStore(std::vector<uint8_t> &Out, int8_t Displacement,
                    int32_t Value) {
  Out.insert(Out.end(), {0xC7, 0x45, static_cast<uint8_t>(Displacement)});
  emit32(Out, static_cast<uint32_t>(Value));
}

/// An x86-32 COFF image with one `.text` and one `.rdata`, plus the personality
/// symbol the recovered record has to be able to name.
struct ImageBuilder {
  std::vector<uint8_t> Text;
  std::vector<uint8_t> RData;
  /// Which runtime the prologue installs.  It decides the seed sentinel and
  /// whether the table carries a cookie header.
  std::string HandlerName = "_except_handler3";

  va_t textVA() const { return kText + Text.size(); }
  va_t rdataVA() const { return kRData + RData.size(); }

  /// One 12-byte scope-table entry.
  void addScope(int32_t EnclosingLevel, va_t FilterVA, va_t HandlerVA) {
    emit32(RData, static_cast<uint32_t>(EnclosingLevel));
    emit32(RData, static_cast<uint32_t>(FilterVA));
    emit32(RData, static_cast<uint32_t>(HandlerVA));
  }

  /// Close the array.  The walk stops at an entry naming no handler, which is
  /// what keeps it out of the next function's table.
  void endScopes() {
    for (unsigned Word = 0; Word < 3; ++Word)
      emit32(RData, 0);
  }

  /// `mov eax, 1; ret` — a body just long enough to decode as a block.
  va_t addStub() {
    const va_t At = textVA();
    Text.insert(Text.end(), {0xB8, 0x01, 0x00, 0x00, 0x00, 0xC3});
    return At;
  }

  BinaryImage build(std::vector<std::pair<std::string, va_t>> Funcs) {
    BinaryImage Img;
    Img.Arch = Arch::X86;
    Img.Bits = Bitness::Bits32;
    Img.Format = BinaryFormat::COFF;
    Img.Base = kBase;

    // The personality has to live past every function the test defines, so
    // that its symbol bounds the last of them rather than splitting one.
    Text.resize(kPersonality - kText, 0xCC);
    Text.push_back(0xC3);

    Segment TextSeg;
    TextSeg.Name = ".text";
    TextSeg.VA = kText;
    TextSeg.Size = Text.size();
    TextSeg.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
    TextSeg.Data = Text;
    Img.Segments.push_back(std::move(TextSeg));

    Segment RDataSeg;
    RDataSeg.Name = ".rdata";
    RDataSeg.VA = kRData;
    RDataSeg.Size = std::max<size_t>(RData.size(), 16);
    RDataSeg.Flags = SegmentFlags::Readable;
    RDataSeg.Data = RData;
    RDataSeg.Data.resize(RDataSeg.Size, 0);
    Img.Segments.push_back(std::move(RDataSeg));

    for (const auto &[Name, Addr] : Funcs) {
      Symbol Sym;
      Sym.Name = Name;
      Sym.Addr = Addr;
      Sym.IsFunc = true;
      Img.Symbols.push_back(std::move(Sym));
    }
    Symbol Handler;
    Handler.Name = HandlerName;
    Handler.Addr = kPersonality;
    Handler.IsFunc = true;
    Img.Symbols.push_back(std::move(Handler));

    coff_loader::parseX86RegistrationExceptions(Img);
    return Img;
  }
};

const RegistrationChainInfo *chainAt(const BinaryImage &Img, va_t Entry) {
  for (const ExceptionFunction &F : Img.ExceptionMetadata.Functions)
    if (F.CodeRange.Begin == Entry && F.Registration)
      return &*F.Registration;
  return nullptr;
}

std::vector<int32_t> levels(const RegistrationChainInfo &Chain) {
  std::vector<int32_t> Levels;
  for (const RegistrationTryLevelStore &Store : Chain.TryLevelStores)
    Levels.push_back(Store.Level);
  return Levels;
}

//===----------------------------------------------------------------------===//
// Proving which frame slot holds the try level
//===----------------------------------------------------------------------===//

TEST(RegistrationTryLevel, ReadsTheSlotTheStoresAgreeOn) {
  ImageBuilder B;
  B.addScope(-1, kText + 0x40, kText + 0x50);
  B.endScopes();

  emitInstall(B.Text, -1, static_cast<uint32_t>(kRData));
  emitFrameStore(B.Text, -0x04, 0);  // enter try level 0
  emitFrameStore(B.Text, -0x04, -1); // leave it again
  B.Text.push_back(0xC3);

  const BinaryImage Img = B.build({{"guarded", kText}});
  const RegistrationChainInfo *Chain = chainAt(Img, kText);
  ASSERT_NE(Chain, nullptr);
  ASSERT_TRUE(Chain->TryLevelOffset.has_value());
  EXPECT_EQ(*Chain->TryLevelOffset, -4);
  EXPECT_EQ(levels(*Chain), (std::vector<int32_t>{0, -1}));
}

TEST(RegistrationTryLevel, RejectsALocalWhoseValueNoScopeCouldName) {
  ImageBuilder B;
  B.addScope(-1, kText + 0x40, kText + 0x50);
  B.endScopes();

  emitInstall(B.Text, -1, static_cast<uint32_t>(kRData));
  emitFrameStore(B.Text, -0x04, 0);
  // An ordinary initialized local.  -100 is not the seed and names no scope,
  // which is what rules this slot out.
  emitFrameStore(B.Text, -0x1C, -100);
  emitFrameStore(B.Text, -0x04, -1);
  B.Text.push_back(0xC3);

  const BinaryImage Img = B.build({{"guarded", kText}});
  const RegistrationChainInfo *Chain = chainAt(Img, kText);
  ASSERT_NE(Chain, nullptr);
  ASSERT_TRUE(Chain->TryLevelOffset.has_value());
  EXPECT_EQ(*Chain->TryLevelOffset, -4);
  EXPECT_EQ(levels(*Chain), (std::vector<int32_t>{0, -1}));
}

TEST(RegistrationTryLevel, RejectsALocalThatIsNeverSetToTheSeed) {
  ImageBuilder B;
  B.addScope(-1, kText + 0x40, kText + 0x50);
  B.addScope(0, 0, kText + 0x58);
  B.endScopes();

  emitInstall(B.Text, -1, static_cast<uint32_t>(kRData));
  emitFrameStore(B.Text, -0x04, 0);
  emitFrameStore(B.Text, -0x04, 1);
  // Every value here is one a scope index could take, so only the absence of
  // the seed separates this local from the real slot.
  emitFrameStore(B.Text, -0x24, 1);
  emitFrameStore(B.Text, -0x24, 0);
  emitFrameStore(B.Text, -0x04, 0);
  emitFrameStore(B.Text, -0x04, -1);
  B.Text.push_back(0xC3);

  const BinaryImage Img = B.build({{"guarded", kText}});
  const RegistrationChainInfo *Chain = chainAt(Img, kText);
  ASSERT_NE(Chain, nullptr);
  ASSERT_TRUE(Chain->TryLevelOffset.has_value());
  EXPECT_EQ(*Chain->TryLevelOffset, -4);
  EXPECT_EQ(levels(*Chain), (std::vector<int32_t>{0, 1, 0, -1}));
}

TEST(RegistrationTryLevel, PublishesNoSlotWhenTwoAreEquallyGood) {
  ImageBuilder B;
  B.addScope(-1, kText + 0x40, kText + 0x50);
  B.endScopes();

  emitInstall(B.Text, -1, static_cast<uint32_t>(kRData));
  // Two slots carrying the same sequence.  Nothing in the image separates
  // them, so publishing either would be a guess.
  emitFrameStore(B.Text, -0x04, 0);
  emitFrameStore(B.Text, -0x08, 0);
  emitFrameStore(B.Text, -0x04, -1);
  emitFrameStore(B.Text, -0x08, -1);
  B.Text.push_back(0xC3);

  const BinaryImage Img = B.build({{"guarded", kText}});
  const RegistrationChainInfo *Chain = chainAt(Img, kText);
  ASSERT_NE(Chain, nullptr);
  EXPECT_FALSE(Chain->TryLevelOffset.has_value());
  EXPECT_TRUE(Chain->TryLevelStores.empty());
}

TEST(RegistrationTryLevel, ReadsTheHandler4SeedOfMinusTwo) {
  ImageBuilder B;
  B.HandlerName = "_except_handler4";
  // `_except_handler4` prefixes the array with four cookie displacements.
  for (unsigned Word = 0; Word < 4; ++Word)
    emit32(B.RData, static_cast<uint32_t>(-2));
  B.addScope(-2, kText + 0x40, kText + 0x50);
  B.endScopes();

  emitInstall(B.Text, -2, static_cast<uint32_t>(kRData));
  emitFrameStore(B.Text, -0x04, -2);
  emitFrameStore(B.Text, -0x04, 0);
  emitFrameStore(B.Text, -0x04, -2);
  B.Text.push_back(0xC3);

  const BinaryImage Img = B.build({{"guarded", kText}});
  const RegistrationChainInfo *Chain = chainAt(Img, kText);
  ASSERT_NE(Chain, nullptr);
  ASSERT_TRUE(Chain->SeededTryLevel.has_value());
  EXPECT_EQ(*Chain->SeededTryLevel, -2);
  ASSERT_TRUE(Chain->TryLevelOffset.has_value());
  EXPECT_EQ(*Chain->TryLevelOffset, -4);
  EXPECT_EQ(levels(*Chain), (std::vector<int32_t>{-2, 0, -2}));
}

//===----------------------------------------------------------------------===//
// The exceptional edges the recovered regions produce
//===----------------------------------------------------------------------===//

/// Every exceptional successor of the block covering \p Addr, as
/// `kind@target` strings in the order the builder produced them.
std::vector<std::string> edgesAt(const LowFunc &Func, va_t Addr) {
  std::vector<std::string> Edges;
  for (const LowBlock &Block : Func.Blocks) {
    if (Addr < Block.StartAddr || Addr >= Block.EndAddr)
      continue;
    for (const ExceptionalEdge &Edge : Block.ExceptionalSuccs)
      Edges.push_back(std::string(getExceptionalEdgeKindName(Edge.Kind)) + "@" +
                      std::to_string(Edge.TargetVA - kText));
  }
  return Edges;
}

LowFunc liftEntry(BinaryImage &Img, va_t Entry) {
  Decoder Dec;
  EXPECT_TRUE(Dec.init(Arch::X86));
  CFGBuilder Builder;
  return Builder.build(Img, Dec, Entry, "guarded");
}

TEST(RegistrationTryLevel, GuardsOnlyTheBlocksTheTryLevelIsCurrentIn) {
  ImageBuilder B;
  B.addScope(-1, kText + 0x40, kText + 0x50);
  B.endScopes();

  emitInstall(B.Text, -1, static_cast<uint32_t>(kRData));
  const va_t Enter = kText + B.Text.size();
  emitFrameStore(B.Text, -0x04, 0);
  const va_t Guarded = kText + B.Text.size();
  B.Text.push_back(0x90); // the one instruction inside the __try
  const va_t Leave = kText + B.Text.size();
  emitFrameStore(B.Text, -0x04, -1);
  const va_t After = kText + B.Text.size();
  B.Text.push_back(0xC3);
  B.Text.resize(0x40, 0xCC);
  B.addStub(); // filter at +0x40
  B.Text.resize(0x50, 0xCC);
  B.addStub(); // handler at +0x50

  BinaryImage Img = B.build({{"guarded", kText}});
  ASSERT_NE(chainAt(Img, kText), nullptr);
  const LowFunc Func = liftEntry(Img, kText);

  // Before the store the frame is at the seed, which names no scope.
  EXPECT_TRUE(edgesAt(Func, Enter).empty());
  // From the store to the next one, scope 0 is current and offers both its
  // filter and its handler.
  EXPECT_EQ(edgesAt(Func, Guarded),
            (std::vector<std::string>{"seh-filter@64", "seh-handler@80"}));
  // The store that leaves has not taken effect until it retires, so its own
  // block is still guarded; the block after it is not.
  EXPECT_EQ(edgesAt(Func, Leave),
            (std::vector<std::string>{"seh-filter@64", "seh-handler@80"}));
  EXPECT_TRUE(edgesAt(Func, After).empty());
}

TEST(RegistrationTryLevel, OffersANestedScopeToItsEnclosingLevelToo) {
  ImageBuilder B;
  B.addScope(-1, kText + 0x40, kText + 0x50);
  B.addScope(0, 0, kText + 0x58); // a __finally nested in the __except
  B.endScopes();

  emitInstall(B.Text, -1, static_cast<uint32_t>(kRData));
  emitFrameStore(B.Text, -0x04, 0);
  const va_t Outer = kText + B.Text.size();
  emitFrameStore(B.Text, -0x04, 1);
  const va_t Inner = kText + B.Text.size();
  B.Text.push_back(0x90);
  emitFrameStore(B.Text, -0x04, 0);
  emitFrameStore(B.Text, -0x04, -1);
  B.Text.push_back(0xC3);
  B.Text.resize(0x40, 0xCC);
  B.addStub(); // filter at +0x40
  B.Text.resize(0x50, 0xCC);
  B.addStub(); // except body at +0x50
  B.Text.resize(0x58, 0xCC);
  B.addStub(); // finally body at +0x58

  BinaryImage Img = B.build({{"guarded", kText}});
  ASSERT_NE(chainAt(Img, kText), nullptr);
  const LowFunc Func = liftEntry(Img, kText);

  EXPECT_EQ(edgesAt(Func, Outer),
            (std::vector<std::string>{"seh-filter@64", "seh-handler@80"}));
  // At level 1 the runtime runs the finally and then still offers the
  // exception to the enclosing __except, so both scopes are reachable.
  EXPECT_EQ(edgesAt(Func, Inner),
            (std::vector<std::string>{"seh-finally@88", "seh-filter@64",
                                      "seh-handler@80"}));
}

TEST(RegistrationTryLevel, AddsNoEdgeWhenNoSlotWasProven) {
  ImageBuilder B;
  B.addScope(-1, kText + 0x40, kText + 0x50);
  B.endScopes();

  emitInstall(B.Text, -1, static_cast<uint32_t>(kRData));
  // Two indistinguishable slots leave the regions unproven, and an unproven
  // region must not be turned into control flow.
  emitFrameStore(B.Text, -0x04, 0);
  emitFrameStore(B.Text, -0x08, 0);
  const va_t Body = kText + B.Text.size();
  B.Text.push_back(0x90);
  emitFrameStore(B.Text, -0x04, -1);
  emitFrameStore(B.Text, -0x08, -1);
  B.Text.push_back(0xC3);
  B.Text.resize(0x40, 0xCC);
  B.addStub();
  B.Text.resize(0x50, 0xCC);
  B.addStub();

  BinaryImage Img = B.build({{"guarded", kText}});
  const RegistrationChainInfo *Chain = chainAt(Img, kText);
  ASSERT_NE(Chain, nullptr);
  EXPECT_FALSE(Chain->TryLevelOffset.has_value());

  const LowFunc Func = liftEntry(Img, kText);
  EXPECT_TRUE(edgesAt(Func, Body).empty());
}

} // namespace
