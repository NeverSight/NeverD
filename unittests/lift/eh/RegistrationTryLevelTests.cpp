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
#include "neverd/ir/high/MedToHigh.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/ir/low/FuncDetector.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/ir/med/MedStackAlignment.h"
#include "neverd/loader/COFF/COFFException.h"
#include "neverd/loader/COFF/COFFRegistrationEH.h"
#include "neverd/loader/ExceptionInfo.h"
#include "neverd/pipeline/Pipeline.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/IR/LLVMContext.h"
#include "llvm/Support/Endian.h"

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
  Out.insert(Out.end(), {0x55});                             // push ebp
  Out.insert(Out.end(), {0x8B, 0xEC});                       // mov ebp, esp
  Out.insert(Out.end(), {0x6A, static_cast<uint8_t>(Seed)}); // push seed
  Out.push_back(0x68);                                       // push imm32
  emit32(Out, TableVA);
  Out.push_back(0x68); // push imm32
  emit32(Out, static_cast<uint32_t>(kPersonality));
  Out.insert(Out.end(), {0x64, 0xA1, 0, 0, 0, 0});       // mov eax, fs:[0]
  Out.push_back(0x50);                                   // push eax
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
  /// Entries a heuristic scan guessed before the parse, as the data-pointer
  /// scan guesses each relocated scope-table pointer.
  std::vector<va_t> Guesses;

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
    for (const va_t Addr : Guesses)
      Img.Symbols.push_back(Symbol::makeFunc(Addr));
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

TEST(RegistrationTryLevel, SEH3RequiresAnArgumentPreservingCRTImportVeneer) {
  ImageBuilder B;
  B.Text = {0xff, 0x25};
  emit32(B.Text, kRData + 8);
  B.Text.resize(64, 0xcc);
  B.Text.push_back(0xe9);
  emit32(B.Text, uint32_t(-69));
  B.RData.resize(16);
  auto Image = B.build({});
  Image.Imports.push_back({"msvcrt.dll", "_except_handler3", 0, kRData + 8});
  EXPECT_TRUE(coff_loader::isCheckedX86SEH3Personality(Image, kText));
  EXPECT_TRUE(coff_loader::isCheckedX86SEH3Personality(Image, kText + 64));
  for (unsigned Mutation = 0; Mutation != 5; ++Mutation) {
    auto Changed = Image;
    if (Mutation == 0)
      Changed.Segments.front().Data[0] = 0xc3;
    if (Mutation == 1)
      Changed.Imports.front().Module = "custom.dll";
    if (Mutation == 2)
      Changed.Imports.front().Name = "other_handler";
    if (Mutation == 3)
      Changed.Segments[1].Data.resize(8);
    if (Mutation == 4)
      llvm::support::endian::write32le(
          Changed.Segments.front().Data.data() + 65, uint32_t(-5));
    EXPECT_FALSE(coff_loader::isCheckedX86SEH3Personality(Changed, kText + 64));
  }
}

TEST(RegistrationTryLevel, EH4WrapperRequiresTheExactRuntimeForwardingABI) {
  ImageBuilder B;
  B.Text = {0x55, 0x89, 0xe5, 0xff, 0x75, 20,   0xff, 0x75,
            16,   0xff, 0x75, 12,   0xff, 0x75, 8,    0x68};
  emit32(B.Text, kText + 64);
  B.Text.push_back(0x68);
  emit32(B.Text, kRData);
  B.Text.insert(B.Text.end(), {0xff, 0x15});
  emit32(B.Text, kRData + 8);
  B.Text.insert(B.Text.end(), {0x83, 0xc4, 24, 0x5d, 0xc3});
  B.Text.resize(64, 0xcc);
  B.Text.push_back(0xc3);
  B.RData.resize(16);
  auto Image = B.build({});
  Image.DynInfo.SecurityCookieRVA = kRData - kBase;
  Image.Imports.push_back(
      {"msvcrt.dll", "_except_handler4_common", 0, kRData + 8});
  EXPECT_EQ(coff_loader::getCheckedX86EH4CookieCheck(Image, kText), kText + 64);
  for (const auto *Module :
       {"ucrtbase.dll", "VCRUNTIME140.dll", "VCRUNTIME140D.dll"}) {
    auto KnownRuntime = Image;
    KnownRuntime.Imports.front().Module = Module;
    EXPECT_EQ(coff_loader::getCheckedX86EH4CookieCheck(KnownRuntime, kText),
              kText + 64);
  }
  for (const auto Offset :
       {0, 2, 5, 8, 11, 14, 15, 20, 21, 25, 26, 27, 31, 32, 33, 34, 35}) {
    auto Changed = Image;
    Changed.Segments.front().Data[Offset] ^= 0x80;
    EXPECT_FALSE(coff_loader::getCheckedX86EH4CookieCheck(Changed, kText))
        << Offset;
  }
  for (unsigned Mutation = 0; Mutation != 5; ++Mutation) {
    auto Changed = Image;
    if (Mutation == 0)
      Changed.DynInfo.SecurityCookieRVA = 0;
    if (Mutation == 1)
      Changed.Imports.front().Module = "custom.dll";
    if (Mutation == 2)
      Changed.Imports.front().Name = "other_handler";
    if (Mutation == 3)
      Changed.Segments.front().Flags = SegmentFlags::Readable;
    if (Mutation == 4)
      llvm::support::endian::write32le(
          Changed.Segments.front().Data.data() + 16, kRData);
    EXPECT_FALSE(coff_loader::getCheckedX86EH4CookieCheck(Changed, kText));
  }
}

TEST(RegistrationTryLevel, GSCookieCheckerHasOnlyTheCheckedLeafSuccessPath) {
  for (bool Near : {false, true})
    for (bool RepReturn : {false, true}) {
      ImageBuilder B;
      B.Text = {0x3b, 0x0d};
      emit32(B.Text, kRData);
      if (Near) {
        B.Text.insert(B.Text.end(), {0x0f, 0x85});
        emit32(B.Text, 20);
      } else
        B.Text.insert(B.Text.end(), {0x75, 24});
      const auto Return = B.Text.size();
      if (RepReturn)
        B.Text.push_back(0xf3);
      B.Text.push_back(0xc3);
      B.Text.resize(64, 0xcc);
      B.RData.resize(16);
      auto Image = B.build({});
      Image.DynInfo.SecurityCookieRVA = kRData - kBase;
      ASSERT_TRUE(
          coff_loader::hasCheckedX86CookieCheckSuccessPath(Image, kText));
      for (unsigned Mutation = 0; Mutation != 9; ++Mutation) {
        auto Changed = Image;
        auto &Code = Changed.Segments.front().Data;
        if (Mutation == 0)
          Code[1] = 0x15;
        if (Mutation == 1)
          Code[2] ^= 4;
        if (Mutation == 2)
          Code[Near ? 7 : 6] ^= 1;
        if (Mutation == 3)
          Code[Return] = 0xc2;
        if (Mutation == 4) {
          if (Near)
            llvm::support::endian::write32le(Code.data() + 8, 0);
          else
            Code[7] = 0;
        }
        if (Mutation == 5) {
          if (Near)
            llvm::support::endian::write32le(Code.data() + 8, 0x1000);
          else
            Code[7] = 0x80;
        }
        if (Mutation == 6)
          Changed.DynInfo.SecurityCookieRVA = 0;
        if (Mutation == 7)
          Changed.Segments[1].Data.clear();
        if (Mutation == 8)
          Changed.Segments.front().Flags = SegmentFlags::Readable;
        EXPECT_FALSE(
            coff_loader::hasCheckedX86CookieCheckSuccessPath(Changed, kText))
            << Near << RepReturn << Mutation;
      }
    }
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

TEST(RegistrationTryLevel, RetainsNarrowStateBitsAndInstructionWidths) {
  for (const bool LongDisplacement : {false, true}) {
    ImageBuilder B;
    B.addScope(-1, kText + 0x80, kText + 0x90);
    B.addScope(0, kText + 0xa0, kText + 0xb0);
    B.endScopes();
    emitInstall(B.Text, -1, static_cast<uint32_t>(kRData));
    emitFrameStore(B.Text, -4, 0);
    const va_t ByteVA = B.textVA();
    B.Text.insert(B.Text.end(),
                  {0xc6, uint8_t(LongDisplacement ? 0x85 : 0x45)});
    if (LongDisplacement)
      emit32(B.Text, uint32_t(-4));
    else
      B.Text.push_back(0xfc);
    B.Text.push_back(0xff);
    const va_t ByteEnd = B.textVA();
    const va_t WordVA = B.textVA();
    B.Text.insert(B.Text.end(),
                  {0x66, 0xc7, uint8_t(LongDisplacement ? 0x85 : 0x45)});
    if (LongDisplacement)
      emit32(B.Text, uint32_t(-4));
    else
      B.Text.push_back(0xfc);
    B.Text.insert(B.Text.end(), {0xff, 0xff});
    const va_t WordEnd = B.textVA();
    emitFrameStore(B.Text, -4, -1);
    B.Text.push_back(0xc3);
    auto Image = B.build({{"narrow-state", kText}});
    const auto *Chain = chainAt(Image, kText);
    ASSERT_NE(Chain, nullptr);
    ASSERT_EQ(Chain->TryLevelStores.size(), 4u);
    EXPECT_EQ(Chain->TryLevelStores[0].Width, 4u);
    EXPECT_EQ(Chain->TryLevelStores[1].StoreVA, ByteVA);
    EXPECT_EQ(Chain->TryLevelStores[1].EndVA, ByteEnd);
    EXPECT_EQ(Chain->TryLevelStores[1].Width, 1u);
    EXPECT_EQ(Chain->TryLevelStores[1].Level, 255);
    EXPECT_EQ(Chain->TryLevelStores[2].StoreVA, WordVA);
    EXPECT_EQ(Chain->TryLevelStores[2].EndVA, WordEnd);
    EXPECT_EQ(Chain->TryLevelStores[2].Width, 2u);
    EXPECT_EQ(Chain->TryLevelStores[2].Level, 65535);
    EXPECT_EQ(Chain->TryLevelStores[3].Width, 4u);
  }
}

TEST(RegistrationTryLevel, ExportAndImageEntriesOwnSymbolFreePrologues) {
  ImageBuilder B;
  B.addScope(-1, kText + 0x40, kText + 0x50);
  B.endScopes();
  emitInstall(B.Text, -1, static_cast<uint32_t>(kRData));
  emitFrameStore(B.Text, -4, 0);
  emitFrameStore(B.Text, -4, -1);
  B.Text.push_back(0xC3);
  for (bool Exported : {false, true}) {
    auto Image = B.build({});
    ASSERT_EQ(chainAt(Image, kText), nullptr);
    if (Exported) {
      Export Entry;
      Entry.Name = "guarded";
      Entry.Addr = kText;
      Image.Exports.push_back(Entry);
    } else {
      Image.Entry = kText;
    }
    coff_loader::parseX86RegistrationExceptions(Image);
    const auto *Chain = chainAt(Image, kText);
    ASSERT_NE(Chain, nullptr);
    EXPECT_EQ(Chain->RegistrationOffset, -16);
    EXPECT_EQ(Chain->TryLevelOffset, -4);
    EXPECT_TRUE(Image.hasAuthenticatedFunctionEntryAt(kText));
  }
}

TEST(RegistrationTryLevel, LeavesItsThunksToTheFunctionThatInstalledIt) {
  // A filter runs in the frame of the function that installed the record, and
  // its handler continues that function after the unwind.  The guesses the
  // data-pointer scan made at them go; a stated symbol stays as a label.
  ImageBuilder B;
  B.addScope(-1, kText + 0x40, kText + 0x50);
  B.addScope(-1, kText + 0x60, kText + 0x70);
  B.endScopes();

  emitInstall(B.Text, -1, static_cast<uint32_t>(kRData));
  emitFrameStore(B.Text, -0x04, 0);
  emitFrameStore(B.Text, -0x04, -1);
  B.Text.push_back(0xC3);
  B.Guesses = {kText + 0x40, kText + 0x50, kText + 0x70};

  const BinaryImage Img = B.build({{"guarded", kText}, {"$LN5", kText + 0x60}});
  std::vector<va_t> Functions;
  for (const Symbol *Sym : Img.getFunctionSymbols())
    Functions.push_back(Sym->Addr);
  EXPECT_EQ(Functions, (std::vector<va_t>{kText, kPersonality}));
  const Symbol *Label = Img.findSymbolAt(kText + 0x60);
  ASSERT_NE(Label, nullptr);
  EXPECT_EQ(Label->Name, "$LN5");
  EXPECT_EQ(Img.findSymbolAt(kText + 0x50), nullptr);
  const ExceptionFunction *Owner = Img.ExceptionMetadata.findFunction(kText);
  ASSERT_NE(Owner, nullptr);
  EXPECT_TRUE(Owner->CodeRange.contains(kText + 0x70));
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

TEST(RegistrationTryLevel, RegistrationLayoutSeparatesIdenticalStoreSequences) {
  ImageBuilder B;
  B.addScope(-1, kText + 0x40, kText + 0x50);
  B.endScopes();

  emitInstall(B.Text, -1, static_cast<uint32_t>(kRData));
  // Locals can carry the same sequence. The installed registration layout,
  // rather than the stored values, identifies the actual state field.
  emitFrameStore(B.Text, -0x04, 0);
  emitFrameStore(B.Text, -0x24, 0);
  emitFrameStore(B.Text, -0x04, -1);
  emitFrameStore(B.Text, -0x24, -1);
  B.Text.push_back(0xC3);

  const BinaryImage Img = B.build({{"guarded", kText}});
  const RegistrationChainInfo *Chain = chainAt(Img, kText);
  ASSERT_NE(Chain, nullptr);
  EXPECT_EQ(Chain->RegistrationOffset, -16);
  EXPECT_EQ(Chain->TryLevelOffset, -4);
  EXPECT_EQ(levels(*Chain), (std::vector<int32_t>{0, -1}));
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

TEST(RegistrationTryLevel, ScopePointerRelocationsKeepTheirDispatchRole) {
  ImageBuilder B;
  B.addScope(-1, kText + 0x40, kText + 0x50);
  B.endScopes();
  B.RData.resize(0x44, 0);
  writeLE<uint32_t>(B.RData.data() + 0x40, kText + 0x40);
  emitInstall(B.Text, -1, static_cast<uint32_t>(kRData));
  emitFrameStore(B.Text, -4, 0);
  B.Text.push_back(0x90);
  emitFrameStore(B.Text, -4, -1);
  B.Text.push_back(0xC3);
  B.Text.resize(0x40, 0xCC);
  B.addStub();
  B.Text.resize(0x50, 0xCC);
  B.addStub();
  auto Image = B.build({{"guarded", kText}});
  Image.CodePtrRelocSlots = {kRData + 4, kRData + 8};
  auto Function = liftEntry(Image, kText);
  ASSERT_TRUE(Function.RegistrationStates);
  EXPECT_TRUE(Function.RegistrationStates->Complete);
  EXPECT_FALSE(Function.OrdinaryModuleAnalysisRoots.count(kText + 0x40));
  EXPECT_FALSE(Function.OrdinaryModuleAnalysisRoots.count(kText + 0x50));
  const auto *Filter = Function.blockFor(kText + 0x40);
  ASSERT_NE(Filter, nullptr);
  EXPECT_TRUE(Function.RegistrationStates->Blocks[Filter->Id].CallbackOnly);

  // A second table's reference is independent evidence of ordinary entry.
  Image.CodePtrRelocSlots.insert(kRData + 0x40);
  Function = liftEntry(Image, kText);
  EXPECT_TRUE(Function.OrdinaryModuleAnalysisRoots.count(kText + 0x40));
  ASSERT_TRUE(Function.RegistrationStates);
  EXPECT_FALSE(Function.RegistrationStates->Complete);
}

TEST(RegistrationTryLevel, EH3DoesNotAcceptTheEH4EnclosingSentinel) {
  ImageBuilder B;
  B.addScope(-2, kText + 0x40, kText + 0x50);
  B.endScopes();
  emitInstall(B.Text, -1, static_cast<uint32_t>(kRData));
  emitFrameStore(B.Text, -4, 0);
  const va_t Guarded = B.textVA();
  B.Text.push_back(0x90);
  emitFrameStore(B.Text, -4, -1);
  B.Text.push_back(0xC3);
  B.Text.resize(0x40, 0xCC);
  B.addStub();
  B.Text.resize(0x50, 0xCC);
  B.addStub();
  BinaryImage Img = B.build({{"guarded", kText}});
  const ExceptionFunction *EH = Img.ExceptionMetadata.findFunction(kText);
  ASSERT_NE(EH, nullptr);
  EXPECT_EQ(EH->ParseStatus, ExceptionParseStatus::Partial);
  EXPECT_TRUE(edgesAt(liftEntry(Img, kText), Guarded).empty());
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
  // Reading the old chain head without writing the new one is not an install.
  std::fill(B.Text.end() - 7, B.Text.end(), 0x90);
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

void addFlowHandlers(ImageBuilder &B) {
  B.Text.resize(0xA0, 0xCC);
  B.addStub();
  B.Text.resize(0xB0, 0xCC);
  B.addStub();
}

BinaryImage makeCxxContinuationImage(va_t Target = kText + 0xc0,
                                     bool PopReturn = false,
                                     bool ReadResumedStack = false,
                                     bool SplitTry = false,
                                     bool InterleavedReturn = false) {
  ImageBuilder B;
  B.Text = {0x55, 0x8b, 0xec, 0x6a, 0xff, 0x68};
  emit32(B.Text, kPersonality);
  B.Text.insert(B.Text.end(), {0x64, 0xa1, 0, 0, 0, 0, 0x50});
  const va_t Install = B.textVA();
  B.Text.insert(B.Text.end(), {0x64, 0x89, 0x25, 0, 0, 0, 0});
  B.Text.insert(B.Text.end(), {0x83, 0xec, 0x10, 0x89, 0x65, 0xf0});
  if (ReadResumedStack) {
    B.Text.insert(B.Text.end(), {0xc7, 0x04, 0x24, 7, 0, 0, 0});
    emitFrameStore(B.Text, -24, 0);
  }
  const va_t Enter = B.textVA();
  emitFrameStore(B.Text, -4, 0);
  B.Text.push_back(0x90);
  const va_t Leave = B.textVA();
  emitFrameStore(B.Text, -4, SplitTry ? 0 : -1);
  auto JumpTo = [&](va_t Address) {
    B.Text.push_back(0xe9);
    emit32(B.Text, Address - (B.textVA() + 4));
  };
  const va_t Epilogue = kText + (SplitTry && !InterleavedReturn ? 0xe8 : 0x80);
  const va_t Continuation = kText + (SplitTry ? 0xd0 : 0xc0);
  JumpTo(kText + (SplitTry ? 0xb0 : 0x80));
  B.Text.resize(0x80, 0xcc);
  auto EmitEpilogue = [&] {
    B.Text.insert(B.Text.end(), {0x8b, 0x4d, 0xf4, 0x64, 0x89, 0x0d, 0, 0, 0, 0,
                                 0x8b, 0xe5, 0x5d, 0xc3});
  };
  if (!SplitTry || InterleavedReturn)
    EmitEpilogue();
  B.Text.resize(0xa0, 0xcc);
  if (ReadResumedStack)
    emitFrameStore(B.Text, -24, 9);
  B.Text.push_back(0xb8);
  emit32(B.Text, Target);
  if (PopReturn)
    B.Text.insert(B.Text.end(), {0xc2, 4, 0});
  else
    B.Text.push_back(0xc3);
  va_t SplitLeave = InvalidVA;
  if (SplitTry) {
    B.Text.resize(0xb0, 0xcc);
    emitFrameStore(B.Text, -24, 11);
    SplitLeave = B.textVA();
    emitFrameStore(B.Text, -4, -1);
    JumpTo(Epilogue);
  }
  B.Text.resize(Continuation - kText, 0xcc);
  emitFrameStore(B.Text, -4, -1);
  if (ReadResumedStack)
    B.Text.insert(B.Text.end(),
                  {0x8b, 0x4d, 0xe8, 0x8b, 0x04, 0x24, 0x01, 0xc8});
  JumpTo(Epilogue);
  if (SplitTry && !InterleavedReturn) {
    B.Text.resize(Epilogue - kText, 0xcc);
    EmitEpilogue();
  }
  B.RData.assign(0x100, 0);
  auto Field = [&](size_t Offset, uint32_t Value) {
    llvm::support::endian::write32le(B.RData.data() + Offset, Value);
  };
  Field(0, 0x19930522);
  Field(4, 2);
  Field(8, kRData + 0x30);
  Field(12, 1);
  Field(16, kRData + 0x40);
  Field(0x30, uint32_t(-1));
  Field(0x38, uint32_t(-1));
  Field(0x48, 1);
  Field(0x4c, 1);
  Field(0x50, kRData + 0x60);
  Field(0x6c, kText + 0xa0);
  auto Img = B.build({{"guarded_cxx", kText}});
  Img.ExceptionMetadata.Functions.clear();
  ExceptionFunction EH;
  EH.CodeRange = {kText, kText + (SplitTry ? 0xf8 : 0xf0)};
  EH.Encoding = ExceptionEncoding::X86CxxFuncInfo;
  EH.Personality = ExceptionPersonality::CxxFrameHandler3;
  EH.HandlerDataVA = kRData;
  auto &Chain = EH.Registration.emplace();
  Chain.ScopeTableVA = kRData;
  Chain.RegistrationOffset = -12;
  Chain.TryLevelOffset = -4;
  Chain.SeededTryLevel = -1;
  Chain.ChainInstallVA = Install;
  Chain.TryLevelStores = {{Enter, Enter + 7, 0},
                          {Leave, Leave + 7, SplitTry ? 0 : -1}};
  if (SplitTry)
    Chain.TryLevelStores.push_back({SplitLeave, SplitLeave + 7, -1});
  Chain.TryLevelStores.push_back({Continuation, Continuation + 7, -1});
  auto &Cxx = EH.Cxx.emplace();
  Cxx.NativeFuncInfoVA = kRData;
  Cxx.Magic = 0x19930522;
  Cxx.MaxState = 2;
  Cxx.UnwindMap = {{-1, 0, CxxUnwindAction::ActionKind::None},
                   {-1, 0, CxxUnwindAction::ActionKind::None}};
  CxxTryBlock Try;
  Try.TryLow = Try.TryHigh = 0;
  Try.CatchHigh = 1;
  CxxCatchHandler Catch;
  Catch.HandlerVA = kText + 0xa0;
  Try.Handlers.push_back(Catch);
  Cxx.TryBlocks.push_back(Try);
  Img.ExceptionMetadata.Functions.push_back(std::move(EH));
  Img.CodePtrRelocSlots.insert(kRData + 0x6c);
  return Img;
}

TEST(RegistrationTryLevel, CxxFuncInfoPointersKeepTheirRuntimeEntryRole) {
  auto Img = makeCxxContinuationImage();
  const auto &EH = Img.ExceptionMetadata.Functions.front();
  const auto Sources =
      coff_loader::getCheckedX86CxxCallbackPointerSources(Img, EH);
  ASSERT_TRUE(Sources);
  EXPECT_EQ(*Sources, (std::map<va_t, va_t>{{kRData + 0x6c, kText + 0xa0}}));
  const auto Low = liftEntry(Img, kText);
  ASSERT_TRUE(Low.RegistrationStates);
  EXPECT_TRUE(Low.RegistrationStates->Complete);
  EXPECT_FALSE(Low.OrdinaryModuleAnalysisRoots.count(kText + 0xa0));

  llvm::support::endian::write32le(Img.Segments[1].Data.data() + 0x80,
                                   kText + 0xa0);
  Img.CodePtrRelocSlots.insert(kRData + 0x80);
  const auto Independent = liftEntry(Img, kText);
  ASSERT_TRUE(Independent.RegistrationStates);
  EXPECT_TRUE(Independent.OrdinaryModuleAnalysisRoots.count(kText + 0xa0));
  EXPECT_FALSE(Independent.RegistrationStates->Complete);
  EXPECT_FALSE(Independent.RegistrationStates->CxxContinuationsComplete);
  const auto Med =
      LowToMedConverter().convert(Independent, Arch::X86, BinaryFormat::COFF);
  for (const auto &Block : Med.Blocks)
    if (Block.StartAddr == kText + 0xa0)
      for (const auto &Op : Block.Ops)
        EXPECT_EQ(Op.RegistrationRoot, MedOp::RegistrationRootKind::None);
}

TEST(RegistrationTryLevel, CxxCallbackRolesSurviveModuleFunctionDiscovery) {
  for (unsigned OrdinarySource = 0; OrdinarySource != 5; ++OrdinarySource) {
    SCOPED_TRACE(OrdinarySource);
    auto Img = makeCxxContinuationImage();
    Img.Entry = kText;
    auto CallbackSymbol = Symbol::makeFunc(kText + 0xa0);
    if (OrdinarySource == 2)
      CallbackSymbol.Origin = NameOrigin::Stated;
    Img.Symbols.push_back(CallbackSymbol);
    if (OrdinarySource == 1) {
      llvm::support::endian::write32le(Img.Segments[1].Data.data() + 0x80,
                                       kText + 0xa0);
      Img.CodePtrRelocSlots.insert(kRData + 0x80);
    }
    if (OrdinarySource == 3)
      Img.Exports.push_back({"ordinary_callback", 0, kText + 0xa0});
    if (OrdinarySource == 4) {
      auto &Text = Img.Segments[0].Data;
      Text[0xe0] = 0xe8;
      llvm::support::endian::write32le(Text.data() + 0xe1, uint32_t(-0x45));
      Text[0xe5] = 0xc3;
    }
    const auto Roles = coff_loader::getCheckedX86RegistrationPointerRoles(Img);
    ASSERT_TRUE(Roles);
    EXPECT_EQ(Roles->Sources,
              (std::map<va_t, va_t>{{kRData + 0x6c, kText + 0xa0}}));
    EXPECT_EQ(Roles->RuntimeOnlyPointerTargets.count(kText + 0xa0),
              OrdinarySource != 1);
    Decoder Dec;
    ASSERT_TRUE(Dec.init(Img));
    const auto Entries = FuncDetector().detect(Img, Dec);
    const bool HasCallback =
        std::any_of(Entries.begin(), Entries.end(), [&](const auto &Entry) {
          return Entry.first == kText + 0xa0;
        });
    EXPECT_EQ(HasCallback, OrdinarySource != 0);
  }
}

TEST(RegistrationTryLevel, AdjacentCxxCatchLabelsDoNotClipTheParentContract) {
  auto Img = makeCxxContinuationImage();
  auto &Text = Img.Segments[0];
  Text.Data.resize(0x110, 0xcc);
  Text.Size = Text.Data.size();
  Text.Data[0x100] = 0xb8;
  writeLE<uint32_t>(Text.Data.data() + 0x101, kRData);
  Text.Data[0x105] = 0xff;
  Text.Data[0x106] = 0x25;
  writeLE<uint32_t>(Text.Data.data() + 0x107, kRData + 0x90);
  for (auto &Symbol : Img.Symbols)
    if (Symbol.Addr == kPersonality)
      Symbol.Name = "cxx_personality_thunk";
  Img.Imports.push_back(
      {"vcruntime140.dll", "__CxxFrameHandler3", 0, kRData + 0x90});
  Img.Symbols.push_back(Symbol::makeFunc(kText + 0xa0));
  auto Next = Symbol::makeFunc(kText + 0xf0);
  Next.Origin = NameOrigin::Stated;
  Img.Symbols.push_back(Next);
  Img.ExceptionMetadata.Functions.clear();
  Img.ExceptionMetadata.rebuildIndex();
  coff_loader::parseX86RegistrationExceptions(Img);
  const auto *EH = Img.ExceptionMetadata.findFunction(kText);
  ASSERT_NE(EH, nullptr);
  ASSERT_TRUE(EH->Cxx);
  EXPECT_EQ(EH->ParseStatus, ExceptionParseStatus::Complete);
  EXPECT_EQ(EH->CodeRange.End, kText + 0xf0);
  EXPECT_TRUE(EH->ownsCode(kText + 0xa0));
  Decoder D;
  ASSERT_TRUE(D.init(Img));
  auto Low = CFGBuilder().build(Img, D, kText);
  ASSERT_TRUE(Low.RegistrationStates);
  EXPECT_TRUE(Low.RegistrationStates->Complete);
  EXPECT_TRUE(Low.RegistrationStates->CxxContinuationsComplete);
  EXPECT_EQ(Low.RegistrationStates->CxxContinuations.size(), 1u);
}

TEST(RegistrationTryLevel, RegistrationParserKeepsItsFocusedLanguageOwnership) {
  for (unsigned OrdinarySource = 0; OrdinarySource != 3; ++OrdinarySource) {
    SCOPED_TRACE(OrdinarySource);
    auto Img = makeCxxContinuationImage();
    auto &Text = Img.Segments[0];
    Text.Data.resize(0x110, 0xcc);
    Text.Size = Text.Data.size();
    Text.Data[0x100] = 0xb8;
    llvm::support::endian::write32le(Text.Data.data() + 0x101, kRData);
    Text.Data[0x105] = 0xff;
    Text.Data[0x106] = 0x25;
    llvm::support::endian::write32le(Text.Data.data() + 0x107, kRData + 0x90);
    for (auto &Symbol : Img.Symbols)
      if (Symbol.Addr == kPersonality)
        Symbol.Name = "cxx_personality_thunk";
    Img.Imports.push_back(
        {"vcruntime140.dll", "__CxxFrameHandler3", 0, kRData + 0x90});
    auto CallbackSymbol = Symbol::makeFunc(kText + 0xa0);
    if (OrdinarySource == 2)
      CallbackSymbol.Origin = NameOrigin::Stated;
    Img.Symbols.push_back(CallbackSymbol);
    if (OrdinarySource == 1) {
      llvm::support::endian::write32le(Img.Segments[1].Data.data() + 0x80,
                                       kText + 0xa0);
      Img.CodePtrRelocSlots.insert(kRData + 0x80);
    }
    Img.ExceptionMetadata.Functions.clear();
    Img.ExceptionMetadata.rebuildIndex();
    coff_loader::parseX86RegistrationExceptions(Img);
    const auto *EH = Img.ExceptionMetadata.findFunction(kText);
    ASSERT_NE(EH, nullptr);
    ASSERT_TRUE(EH->Cxx);
    ASSERT_EQ(EH->ParseStatus, ExceptionParseStatus::Complete);
    EXPECT_EQ(EH->Personality, ExceptionPersonality::CxxFrameHandler3);
    EXPECT_TRUE(EH->LanguageTablesResolved);
    const auto Graph = *EH->Cxx;
    Img.LoadOnlyFunctionEntries.insert(kText);
    coff_loader::ensureExceptionHandlers(Img, {kText});
    EH = Img.ExceptionMetadata.findFunction(kText);
    ASSERT_NE(EH, nullptr);
    EXPECT_EQ(EH->ParseStatus, ExceptionParseStatus::Complete);
    EXPECT_EQ(EH->Personality, ExceptionPersonality::CxxFrameHandler3);
    ASSERT_TRUE(EH->Cxx);
    EXPECT_EQ(*EH->Cxx, Graph);
    EXPECT_EQ(std::any_of(Img.Symbols.begin(), Img.Symbols.end(),
                          [&](const auto &Symbol) {
                            return Symbol.Addr == kText + 0xa0 && Symbol.IsFunc;
                          }),
              OrdinarySource != 0);
  }
}

TEST(RegistrationTryLevel,
     CxxPipelineKeepsRuntimeContinuationsAcrossSelection) {
  for (const bool Selected : {false, true}) {
    SCOPED_TRACE(Selected);
    auto Img = makeCxxContinuationImage(kText + 0xc0, false, true);
    Img.Entry = kText;
    Img.Symbols.push_back(Symbol::makeFunc(kText + 0xa0));
    llvm::LLVMContext Context;
    PipelineOptions Options;
    Options.NoOpt = true;
    Options.EmitDumpOutput = false;
    Options.MaxFunctions = 1;
    if (Selected)
      Options.OnlyFunctionEntries.insert(kText);
    const auto Result = Pipeline().run(Img, Context, Options);
    ASSERT_TRUE(Result.Success) << Result.Error;
    ASSERT_EQ(Result.LowFuncs.size(), 1u);
    ASSERT_EQ(Result.LowFuncs.front().Entry, kText);
    ASSERT_TRUE(Result.LowFuncs.front().RegistrationStates);
    const auto &State = *Result.LowFuncs.front().RegistrationStates;
    ASSERT_TRUE(State.Complete);
    ASSERT_TRUE(State.CxxContinuationsComplete);
    ASSERT_EQ(State.CxxContinuations.size(), 1u);
    ASSERT_EQ(Result.HighFuncs.size(), 1u);
    const auto &High = Result.HighFuncs.front();
    EXPECT_EQ(High.Entry, kText);
    bool HasContinuation = false;
    walkStmts(High.Body, [&](const HighStmt &Statement) {
      if (Statement.Kind == StmtKind::Return && Statement.RetVal &&
          Statement.RetVal->Kind == ExprKind::Const)
        EXPECT_NE(Statement.RetVal->ConstVal, kText + 0xc0);
      for (const auto &Clause : Statement.EHClauses)
        HasContinuation |=
            Clause.Kind == HighEHClauseKind::CxxCatch &&
            Clause.ContinuationVAs == std::vector<va_t>{kText + 0xc0};
    });
    EXPECT_TRUE(HasContinuation);
  }
}

TEST(RegistrationTryLevel,
     RegistrationPointerRolesRequireTheExactReparsedGraph) {
  for (unsigned Mutation = 0; Mutation != 11; ++Mutation) {
    auto Img = makeCxxContinuationImage();
    auto &EH = Img.ExceptionMetadata.Functions.front();
    auto &Cxx = *EH.Cxx;
    auto &Data = Img.Segments[1].Data;
    if (Mutation == 0)
      EH.ParseStatus = ExceptionParseStatus::Partial;
    if (Mutation == 1)
      Cxx.UnwindMap.front().ToState = 0;
    if (Mutation == 2)
      Cxx.TryBlocks.front().Handlers.front().Adjectives = 1;
    if (Mutation == 3)
      Cxx.TryBlocks.front().Handlers.front().CatchObjectOffset = -4;
    if (Mutation == 4)
      llvm::support::endian::write32le(Data.data() + 0x6c, kText + 0xa1);
    if (Mutation == 5)
      llvm::support::endian::write32le(Data.data() + 12, 2);
    if (Mutation == 6)
      EH.HandlerDataVA += 4;
    if (Mutation == 7)
      Cxx.NativeEncoding = CxxExceptionInfo::Encoding::FH4;
    if (Mutation == 8)
      Data.resize(0x68);
    if (Mutation == 9)
      llvm::support::endian::write32le(Data.data() + 0x34, kText + 0x80);
    if (Mutation == 10)
      Cxx.IPMap.push_back({kText, -1});
    EXPECT_FALSE(coff_loader::getCheckedX86CxxCallbackPointerSources(Img, EH))
        << Mutation;
  }
}

TEST(RegistrationTryLevel, DecodesCxxRuntimeContinuationWithoutAnUnknownRoot) {
  auto Img = makeCxxContinuationImage();
  auto Func = liftEntry(Img, kText);
  ASSERT_TRUE(Func.RegistrationStates);
  const auto &State = *Func.RegistrationStates;
  ASSERT_TRUE(State.Complete);
  EXPECT_TRUE(State.CxxContinuationsComplete);
  EXPECT_TRUE(State.RegistrationLifetimeComplete);
  ASSERT_EQ(State.CxxContinuations.size(), 1u);
  EXPECT_EQ(State.CxxContinuations[0].TargetVA, kText + 0xc0);
  const auto *Resume = Func.blockFor(kText + 0xc0);
  ASSERT_NE(Resume, nullptr);
  EXPECT_EQ(Resume->StartAddr, kText + 0xc0);
  EXPECT_FALSE(Func.OrdinaryModuleAnalysisRoots.count(Resume->StartAddr));
  EXPECT_EQ(State.Blocks[Resume->Id].Levels, (std::vector<int32_t>{1}));
  EXPECT_FALSE(State.Blocks[Resume->Id].CallbackOnly);
}

TEST(RegistrationTryLevel, RejectsUnownedOrMisdecodedCxxContinuation) {
  for (unsigned Mutation = 0; Mutation != 4; ++Mutation) {
    auto Img = makeCxxContinuationImage(Mutation == 0   ? kText + 2
                                        : Mutation == 1 ? kRData
                                                        : kText + 0xc0,
                                        Mutation == 2);
    if (Mutation == 3)
      Img.RuntimeFunctionAddrs.insert(kText + 0xc0);
    const auto Func = liftEntry(Img, kText);
    ASSERT_TRUE(Func.RegistrationStates);
    EXPECT_FALSE(Func.RegistrationStates->Complete) << Mutation;
    EXPECT_FALSE(Func.RegistrationStates->CxxContinuationsComplete) << Mutation;
    EXPECT_FALSE(Func.RegistrationStates->RegistrationLifetimeComplete)
        << Mutation;
    EXPECT_FALSE(Func.RegistrationStates->ChainOperationsComplete) << Mutation;
  }
}

TEST(RegistrationTryLevel, CxxRuntimeRootsKeepTheirFrameAndRestoredStack) {
  auto Img = makeCxxContinuationImage(kText + 0xc0, false, true);
  const auto Low = liftEntry(Img, kText);
  ASSERT_TRUE(Low.RegistrationStates);
  ASSERT_TRUE(Low.RegistrationStates->CxxContinuationsComplete);
  const auto Med =
      LowToMedConverter().convert(Low, Arch::X86, BinaryFormat::COFF);
  ASSERT_TRUE(verifyMedFunc(Med, "cxx-runtime-roots"));
  bool CatchFrame = false, ResumeFrame = false, ResumeStack = false;
  for (const auto &Block : Med.Blocks)
    for (const auto &Op : Block.Ops) {
      if (Op.RegistrationRoot ==
          MedOp::RegistrationRootKind::EstablishedFramePointer) {
        CatchFrame |= Block.StartAddr == kText + 0xa0;
        ResumeFrame |= Block.StartAddr == kText + 0xc0;
        EXPECT_EQ(
            entryStackOffset(Med, Op.Output, Arch::X86, BinaryFormat::COFF),
            -4);
      }
      if (Op.RegistrationRoot ==
          MedOp::RegistrationRootKind::RestoredStackPointer) {
        EXPECT_EQ(Block.StartAddr, kText + 0xc0);
        EXPECT_EQ(Op.RegistrationStackOffset, -28);
        EXPECT_EQ(
            entryStackOffset(Med, Op.Output, Arch::X86, BinaryFormat::COFF),
            -32);
        ResumeStack = true;
      }
      if (Op.RegistrationRoot ==
          MedOp::RegistrationRootKind::CallbackStackPointer)
        EXPECT_FALSE(
            entryStackOffset(Med, Op.Output, Arch::X86, BinaryFormat::COFF));
    }
  EXPECT_TRUE(CatchFrame);
  EXPECT_TRUE(ResumeFrame);
  EXPECT_TRUE(ResumeStack);
}

TEST(RegistrationTryLevel, IncompleteCxxContinuationCannotSeedRestoredESP) {
  for (unsigned Mutation = 0; Mutation != 5; ++Mutation) {
    auto Img = makeCxxContinuationImage(kText + 0xc0, false, true);
    auto Low = liftEntry(Img, kText);
    ASSERT_TRUE(Low.RegistrationStates);
    auto &State = *Low.RegistrationStates;
    ASSERT_EQ(State.CxxContinuations.size(), 1u);
    if (Mutation == 0)
      State.CxxContinuationsComplete = false;
    if (Mutation == 1) {
      auto Conflict = State.CxxContinuations.front();
      Conflict.SavedStackOffset -= 4;
      State.CxxContinuations.push_back(Conflict);
    }
    if (Mutation == 2)
      Low.OrdinaryModuleAnalysisRoots.insert(kText + 0xc0);
    if (Mutation == 3)
      State.CxxContinuations.front().SavedStackOffset = -4;
    const auto Med = LowToMedConverter().convert(
        Low, Arch::X86, Mutation == 4 ? BinaryFormat::ELF : BinaryFormat::COFF);
    for (const auto &Block : Med.Blocks)
      for (const auto &Op : Block.Ops)
        EXPECT_NE(Op.RegistrationRoot,
                  MedOp::RegistrationRootKind::RestoredStackPointer)
            << Mutation;
  }
}

TEST(RegistrationTryLevel,
     CxxHighIRKeepsStructuredAndAnnotatedRuntimeContinuations) {
  for (const bool ForceFallback : {false, true}) {
    SCOPED_TRACE(ForceFallback);
    auto Img = makeCxxContinuationImage(kText + 0xc0, false, true);
    const auto Low = liftEntry(Img, kText);
    auto Med = LowToMedConverter().convert(Low, Arch::X86, BinaryFormat::COFF);
    ASSERT_TRUE(Med.RegistrationStates);
    ASSERT_TRUE(Med.RegistrationStates->CxxContinuationsComplete);
    const auto &Resume = Med.RegistrationStates->CxxContinuations.front();
    if (ForceFallback)
      Med.RegistrationStates->Complete = false;
    auto &Catch =
        Med.ExceptionMetadata->Cxx->TryBlocks.front().Handlers.front();
    Catch.CatchObjectOffset = -24;
    Catch.ParentFrameOffset = 12;
    const auto High = MedToHighConverter().convert(Med, Arch::X86);
    bool HasClause = false, HasTransfer = false, HasRestore = false;
    walkStmts(High.Body, [&](const HighStmt &Stmt) {
      if (Stmt.Kind == StmtKind::Store && Stmt.Addr == Resume.Address) {
        ASSERT_TRUE(Stmt.StoreAddr && Stmt.StoreVal);
        EXPECT_EQ(Stmt.StoreAddr->Kind, ExprKind::BinOp);
        EXPECT_EQ(Stmt.StoreVal->Kind, ExprKind::BinOp);
        ASSERT_EQ(Stmt.StoreAddr->Operands.size(), 2u);
        ASSERT_EQ(Stmt.StoreVal->Operands.size(), 2u);
        EXPECT_EQ(Stmt.StoreAddr->Operands[1]->ConstVal, 20u);
        EXPECT_EQ(Stmt.StoreVal->Operands[1]->ConstVal, 32u);
        HasRestore = true;
      }
      if (Stmt.Kind == StmtKind::Goto && Stmt.GotoTarget == Resume.TargetVA)
        HasTransfer = true;
      if (Stmt.Kind == StmtKind::Return && Stmt.RetVal &&
          Stmt.RetVal->Kind == ExprKind::Const)
        EXPECT_NE(Stmt.RetVal->ConstVal, Resume.TargetVA);
      for (const auto &Clause : Stmt.EHClauses)
        if (Clause.Kind == HighEHClauseKind::CxxCatch) {
          HasClause = true;
          EXPECT_EQ(Clause.CatchObjectOffset, -24);
          EXPECT_EQ(Clause.ParentFrameOffset, 12);
          EXPECT_EQ(Clause.ContinuationVAs,
                    (std::vector<va_t>{Resume.TargetVA}));
        }
    });
    EXPECT_TRUE(HasClause);
    if (ForceFallback) {
      // An unstructured annotation retains the out-of-line handler and its
      // continuation descriptor without claiming an embedded catch body.
      EXPECT_GT(High.UnstructuredExceptionRegions, 0u);
      ASSERT_EQ(High.Body.size(), 1u);
      EXPECT_EQ(High.Body.front().Kind, StmtKind::CxxTry);
      EXPECT_FALSE(High.Body.front().EHIsReducible);
      ASSERT_EQ(High.Body.front().EHClauseBodies.size(), 1u);
      EXPECT_TRUE(High.Body.front().EHClauseBodies.front().empty());
    } else {
      EXPECT_TRUE(HasTransfer);
      EXPECT_TRUE(HasRestore);
    }
  }
}

TEST(RegistrationTryLevel, CxxSplitTryKeepsItsCatchAndProtectedTail) {
  for (unsigned Mutation = 0; Mutation != 4; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Img = makeCxxContinuationImage(kText + 0xd0, false, true, true,
                                        Mutation == 3);
    const auto Low = liftEntry(Img, kText);
    ASSERT_TRUE(Low.RegistrationStates);
    ASSERT_TRUE(Low.RegistrationStates->Complete);
    const auto Ranges = registrationRangesWhere(
        *Low.RegistrationStates, [](int32_t State) { return State == 0; });
    ASSERT_TRUE(Ranges);
    ASSERT_EQ(Ranges->size(), 2u);
    auto Med = LowToMedConverter().convert(Low, Arch::X86, BinaryFormat::COFF);
    if (Mutation == 1) {
      // A callback shared with ordinary flow cannot be taken out of that
      // flow and embedded into a catch clause.
      for (auto &Block : Med.Blocks)
        if (Block.StartAddr == kText + 0xa0)
          Block.Preds.push_back(0);
    }
    if (Mutation == 2)
      Med.RegistrationStates->Complete = false;
    // Mutation 3 places an ordinary return between the protected ranges.
    // Removing the callback must not also swallow that unprotected code.
    const auto High = MedToHighConverter().convert(Med, Arch::X86);
    if (Mutation) {
      EXPECT_EQ(High.StructuredExceptionRegions, 0u);
      EXPECT_GT(High.UnstructuredExceptionRegions, 0u);
      continue;
    }
    EXPECT_EQ(High.StructuredExceptionRegions, 1u);
    EXPECT_EQ(High.UnstructuredExceptionRegions, 0u);
    bool HasTail = false, HasCatch = false, HasResume = false;
    walkStmts(High.Body, [&](const HighStmt &Stmt) {
      if (Stmt.Kind != StmtKind::CxxTry)
        return;
      EXPECT_TRUE(Stmt.EHIsReducible);
      walkStmts(Stmt.Body, [&](const HighStmt &Body) {
        HasTail |= Body.Addr == kText + 0xb0;
        EXPECT_FALSE(Body.Addr >= kText + 0xa0 && Body.Addr < kText + 0xb0);
      });
      ASSERT_EQ(Stmt.EHClauseBodies.size(), 1u);
      walkStmts(Stmt.EHClauseBodies.front(), [&](const HighStmt &Body) {
        HasCatch |= Body.Addr == kText + 0xa0;
        HasResume |=
            Body.Kind == StmtKind::Goto && Body.GotoTarget == kText + 0xd0;
      });
    });
    EXPECT_TRUE(HasTail);
    EXPECT_TRUE(HasCatch);
    EXPECT_TRUE(HasResume);
  }
}

TEST(RegistrationTryLevel, CxxOutOfLineBodiesNeedAParentFrameProjection) {
  for (bool Registration : {false, true})
    for (bool Embedded : {false, true})
      for (const auto Kind :
           {HighEHClauseKind::CxxCatch, HighEHClauseKind::CxxCleanup}) {
        SCOPED_TRACE(Registration);
        SCOPED_TRACE(Embedded);
        SCOPED_TRACE(static_cast<int>(Kind));
        HighFunc Parent;
        Parent.Entry = kText;
        auto &EH = Parent.ExceptionMetadata.emplace();
        if (Registration)
          EH.Registration.emplace();
        HighStmt Try;
        Try.Kind = StmtKind::CxxTry;
        HighEHClause Clause;
        Clause.Kind = Kind;
        Clause.HandlerVA = Clause.FilterOrActionVA = kText + 0x80;
        Try.EHClauses.push_back(Clause);
        Try.EHClauseBodies.emplace_back();
        HighStmt Existing;
        Existing.Kind = StmtKind::Call;
        Existing.CallExpr =
            HighExpr::makeCall("checked_parent_frame", 0x5000, {});
        if (Embedded)
          Try.EHClauseBodies.front().push_back(Existing);
        Parent.Body.push_back(Try);
        HighFunc Callback;
        Callback.Entry = kText + 0x80;
        HighStmt Private;
        Private.Kind = StmtKind::Call;
        Private.CallExpr =
            HighExpr::makeCall("ordinary_private_frame", 0x6000, {});
        Callback.Body.push_back(Private);
        std::vector<HighFunc> Functions{Parent, Callback};
        attachCxxFuncletBodies(Functions);
        const auto &Body =
            Functions.front().Body.front().EHClauseBodies.front();
        if (Registration && !Embedded) {
          EXPECT_TRUE(Body.empty());
          EXPECT_EQ(Functions.front().Body.front().EHClauses.front().HandlerVA,
                    Callback.Entry);
        } else {
          ASSERT_EQ(Body.size(), 1u);
          ASSERT_TRUE(Body.front().CallExpr);
          EXPECT_EQ(Body.front().CallExpr->CallAddr,
                    Embedded ? 0x5000u : 0x6000u);
        }
      }
}

TEST(RegistrationTryLevel, IgnoresAStoreInSkippedCode) {
  ImageBuilder B;
  B.addScope(-1, kText + 0xA0, kText + 0xB0);
  B.endScopes();
  emitInstall(B.Text, -1, static_cast<uint32_t>(kRData));
  emitFrameStore(B.Text, -4, 0);
  B.Text.insert(B.Text.end(), {0xEB, 7}); // skip an unreachable state reset
  emitFrameStore(B.Text, -4, -1);
  const va_t Body = B.textVA();
  B.Text.insert(B.Text.end(), {0x90, 0xC3});
  addFlowHandlers(B);

  BinaryImage Img = B.build({{"guarded", kText}});
  const LowFunc Func = liftEntry(Img, kText);
  EXPECT_EQ(edgesAt(Func, Body),
            (std::vector<std::string>{"seh-filter@160", "seh-handler@176"}));
}

TEST(RegistrationTryLevel, UnionsBothReachingLevelsAtAJoin) {
  ImageBuilder B;
  B.addScope(-1, kText + 0xA0, kText + 0xB0);
  B.endScopes();
  emitInstall(B.Text, -1, static_cast<uint32_t>(kRData));
  B.Text.insert(B.Text.end(), {0x85, 0xC9, 0x74, 9}); // test ecx; jz reset
  emitFrameStore(B.Text, -4, 0);
  B.Text.insert(B.Text.end(), {0xEB, 7}); // jmp join
  emitFrameStore(B.Text, -4, -1);
  const va_t Join = B.textVA();
  B.Text.insert(B.Text.end(), {0x90, 0xC3});
  addFlowHandlers(B);

  BinaryImage Img = B.build({{"guarded", kText}});
  const LowFunc Func = liftEntry(Img, kText);
  EXPECT_EQ(edgesAt(Func, Join),
            (std::vector<std::string>{"seh-filter@160", "seh-handler@176"}));
  ASSERT_TRUE(Func.RegistrationStates);
  bool Found = false;
  for (const RegistrationBlockState &Block : Func.RegistrationStates->Blocks) {
    if (!Block.Range.contains(Join))
      continue;
    Found = true;
    EXPECT_EQ(Block.Levels, (std::vector<int32_t>{-1, 0}));
  }
  EXPECT_TRUE(Found);
  EXPECT_FALSE(registrationRangesWhere(
      *Func.RegistrationStates, [](int32_t Level) { return Level == 0; }));
}

TEST(RegistrationTryLevel, ReplaysStateThroughALoopBackedge) {
  ImageBuilder B;
  B.addScope(-1, kText + 0xA0, kText + 0xB0);
  B.endScopes();
  emitInstall(B.Text, -1, static_cast<uint32_t>(kRData));
  const va_t Header = B.textVA();
  B.Text.insert(B.Text.end(), {0x90, 0x85, 0xC9, 0x74, 9});
  emitFrameStore(B.Text, -4, 0);
  B.Text.insert(B.Text.end(), {0xEB, 0xF2}); // back to header, now at state 0
  emitFrameStore(B.Text, -4, -1);
  B.Text.push_back(0xC3);
  addFlowHandlers(B);

  BinaryImage Img = B.build({{"guarded", kText}});
  const LowFunc Func = liftEntry(Img, kText);
  EXPECT_EQ(edgesAt(Func, Header),
            (std::vector<std::string>{"seh-filter@160", "seh-handler@176"}));
  ASSERT_TRUE(Func.RegistrationStates);
  for (const RegistrationBlockState &Block : Func.RegistrationStates->Blocks)
    if (Block.Range.contains(Header))
      EXPECT_EQ(Block.Levels, (std::vector<int32_t>{-1, 0}));
}

TEST(RegistrationTryLevel, EntersAnInnerHandlerAtItsEnclosingLevel) {
  ImageBuilder B;
  B.addScope(-1, kText + 0xA0, kText + 0xB0);
  B.addScope(0, kText + 0xA8, kText + 0xB8);
  B.endScopes();
  emitInstall(B.Text, -1, static_cast<uint32_t>(kRData));
  emitFrameStore(B.Text, -4, 1);
  B.Text.push_back(0x90);
  emitFrameStore(B.Text, -4, -1);
  B.Text.push_back(0xC3);
  B.Text.resize(0xA0, 0xCC);
  B.addStub();
  B.Text.resize(0xA8, 0xCC);
  B.addStub();
  B.Text.resize(0xB0, 0xCC);
  B.addStub();
  B.Text.resize(0xB8, 0xCC);
  B.addStub();

  BinaryImage Img = B.build({{"guarded", kText}});
  const LowFunc Func = liftEntry(Img, kText);
  EXPECT_EQ(edgesAt(Func, kText + 0xB8),
            (std::vector<std::string>{"seh-filter@160", "seh-handler@176"}));
  EXPECT_TRUE(edgesAt(Func, kText + 0xB0).empty());
}

BinaryImage makeSafeSEHImage(uint32_t Count, std::vector<uint32_t> HandlerRVAs,
                             uint32_t TableVA = kRData + 0x148) {
  ImageBuilder B;
  B.addScope(-1, kText + 0xA0, kText + 0xB0);
  B.endScopes();
  emitInstall(B.Text, -1, static_cast<uint32_t>(kRData));
  emitFrameStore(B.Text, -4, 0);
  emitFrameStore(B.Text, -4, -1);
  B.Text.push_back(0xC3);
  addFlowHandlers(B);
  B.RData.resize(0x148, 0);
  writeLE<uint32_t>(B.RData.data() + 0x100, 0x48);
  writeLE<uint32_t>(B.RData.data() + 0x140, TableVA);
  writeLE<uint32_t>(B.RData.data() + 0x144, Count);
  for (uint32_t RVA : HandlerRVAs)
    emit32(B.RData, RVA);
  BinaryImage Img = B.build({{"guarded", kText}});
  Img.ExceptionMetadata = ExceptionInfo{};
  Img.DynInfo.LoadConfigRVA = kRData + 0x100 - kBase;
  Img.DynInfo.LoadConfigSize = 0x48;
  coff_loader::parseX86RegistrationExceptions(Img);
  return Img;
}

TEST(RegistrationTryLevel, AcceptsASortedMappedSafeSEHTable) {
  const BinaryImage Img = makeSafeSEHImage(1, {kPersonality - kBase});
  EXPECT_EQ(Img.ExceptionMetadata.ParseStatus, ExceptionParseStatus::Complete);
  EXPECT_NE(chainAt(Img, kText), nullptr);
}

TEST(RegistrationTryLevel, MalformedSafeSEHDoesNotBecomeAbsence) {
  for (auto Img :
       {makeSafeSEHImage(1, {}, 0), makeSafeSEHImage(1, {}, 0xDEADBEEF),
        makeSafeSEHImage(4097, {}),
        makeSafeSEHImage(2, {kPersonality - kBase, kPersonality - kBase}),
        makeSafeSEHImage(2, {kPersonality - kBase, kText + 0xA0 - kBase})}) {
    EXPECT_EQ(Img.ExceptionMetadata.ParseStatus,
              ExceptionParseStatus::Malformed);
    EXPECT_TRUE(Img.ExceptionMetadata.Functions.empty());
    EXPECT_FALSE(Img.ExceptionMetadata.Diagnostics.empty());
  }
}

TEST(RegistrationTryLevel, SafeSEHMembershipDoesNotProveLanguageSemantics) {
  BinaryImage Img = makeSafeSEHImage(1, {kPersonality - kBase});
  for (Symbol &Sym : Img.Symbols)
    if (Sym.Addr == kPersonality)
      Sym.Name = "custom_exception_handler";
  Img.ExceptionMetadata = {};
  coff_loader::parseX86RegistrationExceptions(Img);
  ASSERT_EQ(Img.ExceptionMetadata.Functions.size(), 1u);
  const ExceptionFunction &EH = Img.ExceptionMetadata.Functions.front();
  EXPECT_EQ(EH.Personality, ExceptionPersonality::Unknown);
  EXPECT_EQ(EH.ParseStatus, ExceptionParseStatus::Partial);
  ASSERT_TRUE(EH.Registration);
  EXPECT_FALSE(EH.Registration->Scopes.empty());
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::X86));
  LowFunc F = CFGBuilder().build(Img, Dec, kText);
  ASSERT_TRUE(F.RegistrationStates);
  EXPECT_FALSE(F.RegistrationStates->Complete);
}

TEST(RegistrationTryLevel, ScopeBudgetCannotPublishACompletePrefix) {
  ImageBuilder B;
  for (unsigned I = 0; I < 4097; ++I)
    B.addScope(-1, kText + 0xA0, kText + 0xB0);
  B.endScopes();
  emitInstall(B.Text, -1, static_cast<uint32_t>(kRData));
  emitFrameStore(B.Text, -4, 0);
  emitFrameStore(B.Text, -4, -1);
  B.Text.push_back(0xC3);
  const BinaryImage Img = B.build({{"guarded", kText}});
  ASSERT_EQ(Img.ExceptionMetadata.Functions.size(), 1u);
  const ExceptionFunction &EH = Img.ExceptionMetadata.Functions.front();
  ASSERT_TRUE(EH.Registration);
  EXPECT_EQ(EH.Registration->Scopes.size(), 4096u);
  EXPECT_EQ(EH.ParseStatus, ExceptionParseStatus::Partial);
  EXPECT_FALSE(EH.Diagnostics.empty());
}

} // namespace
