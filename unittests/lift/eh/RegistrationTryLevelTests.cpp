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

#include "../core/MappedSectionFlags.h"
#include "gtest/gtest.h"

#include "neverd/Limits.h"
#include "neverd/decode/Decoder.h"
#include "neverd/ir/RegistrationState.h"
#include "neverd/ir/high/MedToHigh.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/ir/low/FuncDetector.h"
#include "neverd/ir/low/RegistrationABI.h"
#include "neverd/ir/low/RegistrationCxxState.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/ir/med/MedStackAlignment.h"
#include "neverd/ir/med/X86/X86RegistrationFrame.h"
#include "neverd/ir/med/X86/X86RegistrationPhysicalFrame.h"
#include "neverd/lift/X86/X86Regs.h"
#include "neverd/loader/COFF/COFFCxxEpilogue.h"
#include "neverd/loader/COFF/COFFException.h"
#include "neverd/loader/COFF/COFFLoader.h"
#include "neverd/loader/COFF/COFFRegistrationEH.h"
#include "neverd/loader/COFF/COFFStackProbe.h"
#include "neverd/loader/ExceptionInfo.h"
#include "neverd/loader/FunctionDiscovery.h"
#include "neverd/pipeline/Pipeline.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/IR/LLVMContext.h"
#include "llvm/Support/Endian.h"

#include <algorithm>
#include <cstdlib>
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
  uint32_t SecurityCookieRVA = 0;
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
    Img.DynInfo.SecurityCookieRVA = SecurityCookieRVA;

    // The personality has to live past every function the test defines, so
    // that its symbol bounds the last of them rather than splitting one.
    Text.resize(kPersonality - kText, 0xCC);
    Text.push_back(0xC3);

    Segment TextSeg;
    TextSeg.Name = ".text";
    TextSeg.VA = kText;
    TextSeg.Size = Text.size();
    TextSeg.FileSz = TextSeg.Size;
    TextSeg.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
    TextSeg.Data = Text;
    Section TextSection;
    TextSection.Name = TextSeg.Name;
    TextSection.VA = TextSeg.VA;
    TextSection.Size = TextSection.FileSz = TextSeg.Size;
    TextSection.Flags = TextSeg.Flags;
    Img.Sections.push_back(std::move(TextSection));
    Img.Segments.push_back(std::move(TextSeg));

    Segment RDataSeg;
    RDataSeg.Name = ".rdata";
    RDataSeg.VA = kRData;
    RDataSeg.Size = std::max<size_t>(RData.size(), 16);
    RDataSeg.FileSz = RDataSeg.Size;
    RDataSeg.Flags = SegmentFlags::Readable;
    RDataSeg.Data = RData;
    RDataSeg.Data.resize(RDataSeg.Size, 0);
    Section RDataSection;
    RDataSection.Name = RDataSeg.Name;
    RDataSection.VA = RDataSeg.VA;
    RDataSection.Size = RDataSection.FileSz = RDataSeg.Size;
    RDataSection.Flags = RDataSeg.Flags;
    Img.Sections.push_back(std::move(RDataSection));
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

BinaryImage makeHelperCallerImage(bool Wide, bool SeparateBodies = true,
                                  unsigned Mutation = 0) {
  ImageBuilder B;
  B.addScope(-1, kText + 0xa0, kText + 0xb0);
  B.endScopes();
  const va_t SecondTable = B.rdataVA();
  B.addScope(-1, kText + 0xa0, kText + 0xb0);
  B.endScopes();
  constexpr va_t Helper = kText + 0xc0;
  B.Text.push_back(Mutation == 1 ? 0xb8 : 0xc3);
  // A matching prefix inside MOV's immediate cannot create a new entry.
  const size_t First = Mutation == 1 ? 1 : 0x20;
  B.Text.resize(First, 0xcc);
  auto Caller = [&](va_t Table, bool Returns) {
    B.Text.push_back(Wide ? 0x68 : 0x6a);
    if (Wide)
      emit32(B.Text, Mutation == 2 ? uint32_t(-1) : 0x20);
    else
      B.Text.push_back(Mutation == 2 ? 0xff : 0x20);
    B.Text.push_back(0x68);
    emit32(B.Text, Table);
    B.Text.push_back(0xe8);
    emit32(B.Text, uint32_t(Helper - (B.textVA() + 4)));
    emitFrameStore(B.Text, -4, 0);
    if (Returns)
      B.Text.push_back(0xc3);
  };
  Caller(kRData, SeparateBodies);
  B.Text.resize(0x40, SeparateBodies ? 0xcc : 0x90);
  Caller(SecondTable, true);
  B.Text.resize(0xa0, 0xcc);
  B.addStub();
  B.Text.resize(0xb0, 0xcc);
  B.addStub();
  B.Text.resize(0xc0, 0xcc);
  B.Text.insert(B.Text.end(), {0x6a, 0xff, 0x68});
  emit32(B.Text, kPersonality);
  B.Text.insert(B.Text.end(), {0x64, 0xa1, 0, 0, 0, 0, 0x50, 0x64, 0x89, 0x25,
                               0, 0, 0, 0, 0xc3});
  return B.build({{"preceding", kText},
                  {"filter", kText + 0xa0},
                  {"handler", kText + 0xb0},
                  {"helper", Helper}});
}

TEST(RegistrationTryLevel, SharedHelperCallersOwnTheirSeparateEntries) {
  for (bool Wide : {false, true}) {
    SCOPED_TRACE(Wide);
    const auto Image = makeHelperCallerImage(Wide);
    EXPECT_EQ(chainAt(Image, kText), nullptr);
    EXPECT_EQ(chainAt(Image, kText + 0xc0), nullptr);
    const auto *First = chainAt(Image, kText + 0x20);
    const auto *Second = chainAt(Image, kText + 0x40);
    ASSERT_NE(First, nullptr);
    ASSERT_NE(Second, nullptr);
    EXPECT_EQ(First->ScopeTableVA, kRData);
    EXPECT_EQ(Second->ScopeTableVA, kRData + 24);
    const auto *FirstFunction =
        Image.ExceptionMetadata.findFunction(kText + 0x20);
    ASSERT_NE(FirstFunction, nullptr);
    EXPECT_EQ(FirstFunction->CodeRange.End, kText + 0x40);
    // Entry attribution supplies neither a helper ABI nor a frame-state proof.
    EXPECT_FALSE(First->RegistrationOffset);
    EXPECT_FALSE(Second->RegistrationOffset);
  }
}

TEST(RegistrationTryLevel, SharedHelperPrefixDoesNotSplitOrdinaryFlow) {
  for (bool Wide : {false, true}) {
    SCOPED_TRACE(Wide);
    const auto Image = makeHelperCallerImage(Wide, false);
    ASSERT_NE(chainAt(Image, kText + 0x20), nullptr);
    EXPECT_EQ(chainAt(Image, kText + 0x40), nullptr);
    EXPECT_EQ(chainAt(Image, kText), nullptr);
  }
}

TEST(RegistrationTryLevel,
     SharedHelperEntryRejectsInteriorAndNegativePrefixes) {
  for (bool Wide : {false, true})
    for (unsigned Mutation : {1, 2}) {
      SCOPED_TRACE(::testing::Message() << Wide << " " << Mutation);
      const auto Image = makeHelperCallerImage(Wide, true, Mutation);
      EXPECT_EQ(chainAt(Image, kText + (Mutation == 1 ? 1 : 0x20)), nullptr);
    }
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
      // A relocated absolute cookie operand preserves the checked operation.
      // Its normalized field must agree with both the bytes and the fixup.
      Image.BaseRelocations.push_back({kText + 2, 3});
      ASSERT_TRUE(
          coff_loader::hasCheckedX86CookieCheckSuccessPath(Image, kText));
      RelocatedAddressField Field;
      Field.EncodedValue = kRData;
      Field.TargetVA = kRData;
      Field.Width = 4;
      Image.DataAddressRelocOperands.emplace(kText + 2, Field);
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
      for (unsigned Mutation = 0; Mutation != 20; ++Mutation) {
        auto Changed = Image;
        auto &Text = Changed.Segments.front();
        auto &Section = Changed.Sections.front();
        if (Mutation == 0)
          Text.Flags = Text.Flags | SegmentFlags::Writable;
        if (Mutation == 1)
          setMappedSectionFlags(Changed, Section,
                                Section.Flags | SegmentFlags::Writable);
        if (Mutation == 2)
          Text.Flags = SegmentFlags::Executable;
        if (Mutation == 3)
          Section.FileSz = Return;
        if (Mutation == 4)
          Text.FileSz = Return;
        if (Mutation == 5)
          ++Section.FileOff;
        if (Mutation == 6)
          Changed.Sections.push_back(Section);
        if (Mutation == 7)
          Changed.Segments.push_back(Text);
        if (Mutation == 8)
          Changed.BaseRelocations.push_back({kText, 3});
        if (Mutation == 9)
          Changed.BaseRelocations.push_back({kText + 6, 3});
        if (Mutation == 10)
          Changed.BaseRelocations.push_back({kText + Return, 3});
        if (Mutation == 11)
          Changed.BaseRelocations.push_back({kText + 2, 3});
        if (Mutation == 12)
          Changed.BaseRelocations.front().Type = 10;
        if (Mutation == 13)
          Changed.BaseRelocations.clear();
        if (Mutation == 14)
          Changed.DataAddressRelocOperands.begin()->second.TargetVA += 4;
        if (Mutation == 15)
          Changed.DataPtrRelocSlots.insert(kText + 2);
        if (Mutation == 16)
          Changed.IsRelocatable = true;
        if (Mutation == 17)
          Changed.Bits = Bitness::Bits64;
        if (Mutation == 18)
          Changed.DynInfo.SecurityCookieRVA = UINT32_MAX;
        if (Mutation == 19) {
          Changed.Base = kRData + 4;
          Changed.DynInfo.SecurityCookieRVA = InvalidVA - 3;
        }
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

struct DirectEH4Fixture {
  ImageBuilder Builder;
  size_t CookieLoad = 0;
  size_t CookieStore = 0;
  size_t Publication = 0;
};

/// Independent instruction encodings for both stack-cookie forms. Wide locals
/// force disp32 cookie stores, while PUSH places the cookie below saved GPRs.
DirectEH4Fixture makeDirectEH4Fixture(bool Wide, bool Add, bool Push, bool GS,
                                      bool Hotpatch = false) {
  DirectEH4Fixture F;
  auto &B = F.Builder;
  B.HandlerName = "_except_handler4";
  B.SecurityCookieRVA = kRData + 0x80 - kBase;
  if (Hotpatch)
    B.Text = {0x8b, 0xff};
  emitInstall(B.Text, -2, kRData);
  B.Text.resize(22 + (Hotpatch ? 2 : 0));
  const int32_t Allocation = Wide ? 200 : 20;
  B.Text.push_back(Wide ? 0x81 : 0x83);
  B.Text.push_back(Add ? 0xc4 : 0xec);
  if (Wide)
    emit32(B.Text, Add ? -Allocation : Allocation);
  else
    B.Text.push_back(uint8_t(Add ? -Allocation : Allocation));
  B.Text.insert(B.Text.end(), {0x53, 0x56, 0x57});
  int32_t SP = -16 - Allocation - 12;
  int32_t MemorySlot = -16 - Allocation + 8;
  F.CookieLoad = B.Text.size();
  B.Text.push_back(0xa1);
  emit32(B.Text, kRData + 0x80);
  B.Text.insert(B.Text.end(), {0x31, 0x45, 0xf8, 0x33, 0xc5});
  F.CookieStore = B.Text.size();
  auto CookieStore = [&] {
    const int32_t Slot = Push ? (SP -= 4) : MemorySlot;
    MemorySlot -= 4;
    if (Push)
      B.Text.push_back(0x50);
    else if (Wide) {
      B.Text.insert(B.Text.end(), {0x89, 0x85});
      emit32(B.Text, Slot);
    } else
      B.Text.insert(B.Text.end(), {0x89, 0x45, uint8_t(Slot)});
    return Slot;
  };
  const int32_t EHSlot = CookieStore();
  const int32_t GSSlot = GS ? CookieStore() : -2;
  B.Text.insert(B.Text.end(), {0x8d, 0x45, 0xf0});
  F.Publication = B.Text.size();
  B.Text.insert(B.Text.end(), {0x64, 0xa3, 0, 0, 0, 0});
  B.Text.insert(B.Text.end(), {0x89, 0x65, 0xe8});
  emitFrameStore(B.Text, -4, 0);
  B.Text.push_back(0x90);
  emitFrameStore(B.Text, -4, -2);
  B.Text.insert(B.Text.end(),
                {0x8b, 0x4d, 0xf0, 0x64, 0x89, 0x0d, 0, 0, 0, 0, 0x8d, 0xa5});
  emit32(B.Text, -16 - Allocation - 12);
  B.Text.insert(B.Text.end(), {0x5f, 0x5e, 0x5b, 0x8b, 0xe5, 0x5d, 0xc3});
  const va_t Finally = B.addStub();
  emit32(B.RData, GSSlot);
  emit32(B.RData, 0);
  emit32(B.RData, EHSlot);
  emit32(B.RData, 0);
  B.addScope(-2, 0, Finally);
  B.endScopes();
  B.RData.resize(0x84);
  return F;
}

TEST(RegistrationTryLevel, EH4PublicationFollowsAllocatedCookieStorage) {
  for (bool Wide : {false, true})
    for (bool Add : {false, true})
      for (bool Push : {false, true})
        for (bool GS : {false, true}) {
          SCOPED_TRACE(::testing::Message() << Wide << Add << Push << GS);
          auto F = makeDirectEH4Fixture(Wide, Add, Push, GS);
          auto Image = F.Builder.build({{"guarded", kText}});
          const auto *Chain = chainAt(Image, kText);
          ASSERT_NE(Chain, nullptr);
          ASSERT_EQ(Chain->RegistrationOffset, -16);
          EXPECT_EQ(Chain->TryLevelOffset, -4);
          EXPECT_EQ(Chain->ChainInstallVA, kText + F.Publication);
          const auto Low = liftEntry(Image, kText);
          ASSERT_TRUE(Low.hasCompleteLiftCoverage());
          const auto State = analyzeRegistrationStates(Low, kRData + 0x80);
          EXPECT_TRUE(State.Complete);
          EXPECT_TRUE(State.CallbackStatesComplete);
          EXPECT_TRUE(State.RegistrationLifetimeComplete);
          EXPECT_TRUE(State.SecurityCookiesComplete);
        }
}

TEST(RegistrationTryLevel, HotpatchEntryKeepsEH4FrameCoordinates) {
  for (bool Wide : {false, true})
    for (bool Add : {false, true})
      for (bool Push : {false, true})
        for (bool GS : {false, true}) {
          SCOPED_TRACE(::testing::Message() << Wide << Add << Push << GS);
          auto F = makeDirectEH4Fixture(Wide, Add, Push, GS, true);
          auto Image = F.Builder.build({{"hotpatch", kText}});
          const auto *Chain = chainAt(Image, kText);
          ASSERT_NE(Chain, nullptr);
          EXPECT_EQ(Chain->RegistrationOffset, -16);
          EXPECT_EQ(Chain->TryLevelOffset, -4);
          EXPECT_EQ(Chain->ChainInstallVA, kText + F.Publication);
          const auto Low = liftEntry(Image, kText);
          ASSERT_EQ(Low.Entry, kText);
          ASSERT_TRUE(Low.hasCompleteLiftCoverage());
          ASSERT_FALSE(Low.Blocks.empty());
          ASSERT_FALSE(Low.Blocks.front().InstructionBoundaries.empty());
          EXPECT_EQ(Low.Blocks.front().InstructionBoundaries.front().Address,
                    kText);
          EXPECT_EQ(Low.Blocks.front().InstructionBoundaries.front().Size, 2);
          const auto State = analyzeRegistrationStates(Low, kRData + 0x80);
          EXPECT_TRUE(State.Complete);
          EXPECT_TRUE(State.CallbackStatesComplete);
          EXPECT_TRUE(State.RegistrationLifetimeComplete);
          EXPECT_TRUE(State.SecurityCookiesComplete);
        }
}

TEST(RegistrationTryLevel, HotpatchEntryRequiresExactImmutableBytes) {
  for (unsigned Mutation = 0; Mutation != 5; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto F = makeDirectEH4Fixture(false, false, true, false, true);
    if (Mutation == 0)
      F.Builder.Text[1] = 0xfe; // changes EDI
    if (Mutation == 1)
      F.Builder.Text[0] = 0x66; // not a complete two-byte instruction
    if (Mutation == 2)
      F.Builder.Text[0] = 0xeb; // a control transfer
    auto Image = F.Builder.build({{"hotpatch", kText}});
    if (Mutation == 3)
      Image.BaseRelocations.push_back({kText, 3});
    if (Mutation == 4) {
      auto &Text = Image.Segments.front();
      Text.Flags = Text.Flags | SegmentFlags::Writable;
      Image.Sections.front().Flags = Text.Flags;
    }
    Image.ExceptionMetadata = {};
    coff_loader::parseX86RegistrationExceptions(Image);
    const auto *Chain = chainAt(Image, kText);
    EXPECT_TRUE(!Chain || !Chain->RegistrationOffset);
  }
}

TEST(RegistrationTryLevel, EH4PublicationRejectsChangedInitialization) {
  for (unsigned Mutation = 0; Mutation != 10; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto F = makeDirectEH4Fixture(false, true, true, false);
    auto &B = F.Builder;
    if (Mutation == 0)
      B.Text[24] = 20; // releases storage instead of allocating it
    if (Mutation == 1)
      B.Text[26] = 0x53; // duplicate saved register
    if (Mutation == 2)
      B.Text[F.CookieLoad + 1] ^= 4; // a different cookie source
    if (Mutation == 3)
      B.Text[F.CookieLoad + 7] = 0xfc; // overwrites the state, not the table
    if (Mutation == 4)
      B.Text[F.CookieLoad + 9] = 0xc4; // XOR with ESP, not EBP
    if (Mutation == 5)
      B.Text[F.CookieStore] = 0x51; // a different register is pushed
    if (Mutation == 6)
      B.Text[F.Publication - 1] = 0xec; // publishes the wrong node address
    if (Mutation == 7)
      B.Text[F.Publication + 2] = 4; // writes FS:[4], not the chain head
    if (Mutation == 8)
      llvm::support::endian::write32le(B.RData.data() + 8, uint32_t(-48));
    if (Mutation == 9)
      B.SecurityCookieRVA = 0;
    const auto Image = B.build({{"guarded", kText}});
    const auto *Chain = chainAt(Image, kText);
    EXPECT_TRUE(!Chain || !Chain->RegistrationOffset);
  }
}

TEST(RegistrationTryLevel, EH4CookieStoresMustOwnDistinctPrivateBytes) {
  for (int32_t Slot : {-52, -40, -41, -20}) {
    SCOPED_TRACE(Slot);
    auto F = makeDirectEH4Fixture(false, true, false, false);
    F.Builder.Text[F.CookieStore + 2] = uint8_t(Slot);
    llvm::support::endian::write32le(F.Builder.RData.data() + 8,
                                     uint32_t(Slot));
    const auto Image = F.Builder.build({{"guarded", kText}});
    const auto *Chain = chainAt(Image, kText);
    EXPECT_TRUE(!Chain || !Chain->RegistrationOffset);
  }
  auto F = makeDirectEH4Fixture(false, true, false, true);
  llvm::support::endian::write32le(F.Builder.RData.data(), uint32_t(-26));
  const auto Image = F.Builder.build({{"guarded", kText}});
  const auto *Chain = chainAt(Image, kText);
  EXPECT_TRUE(!Chain || !Chain->RegistrationOffset);
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

TEST(RegistrationTryLevel, RegisterZeroStoresUseTheDecodedReachingValue) {
  for (uint8_t Register : {0, 1, 2, 3, 6, 7})
    for (uint8_t Xor : {0x31, 0x33})
      for (bool WideDisplacement : {false, true}) {
        SCOPED_TRACE(::testing::Message()
                     << unsigned(Register) << " " << unsigned(Xor) << " "
                     << WideDisplacement);
        ImageBuilder B;
        B.addScope(-1, kText + 0x80, kText + 0x90);
        B.endScopes();
        emitInstall(B.Text, -1, kRData);
        B.Text.insert(B.Text.end(),
                      {Xor, uint8_t(0xc0 | (Register << 3) | Register)});
        const va_t Store = B.textVA();
        B.Text.insert(B.Text.end(),
                      {0x89, uint8_t((WideDisplacement ? 0x85 : 0x45) |
                                     (Register << 3))});
        if (WideDisplacement)
          emit32(B.Text, uint32_t(-4));
        else
          B.Text.push_back(0xfc);
        const va_t Guarded = B.textVA();
        B.Text.push_back(0x90);
        emitFrameStore(B.Text, -4, -1);
        const va_t After = B.textVA();
        B.Text.push_back(0xc3);
        B.Text.resize(0x80, 0xcc);
        B.addStub();
        B.Text.resize(0x90, 0xcc);
        B.addStub();
        auto Image = B.build({{"guarded", kText}});
        const auto *Chain = chainAt(Image, kText);
        ASSERT_NE(Chain, nullptr);
        ASSERT_EQ(Chain->TryLevelStores.size(), 2u);
        const auto &Observed = Chain->TryLevelStores.front();
        EXPECT_EQ(Observed.StoreVA, Store);
        EXPECT_EQ(Observed.EndVA, Guarded);
        EXPECT_EQ(Observed.Level, 0);
        EXPECT_EQ(Observed.Width, 4u);
        const auto Function = liftEntry(Image, kText);
        ASSERT_TRUE(Function.hasCompleteLiftCoverage());
        EXPECT_TRUE(edgesAt(Function, Store).empty());
        EXPECT_EQ(
            edgesAt(Function, Guarded),
            (std::vector<std::string>{"seh-filter@128", "seh-handler@144"}));
        EXPECT_TRUE(edgesAt(Function, After).empty());
      }
}

TEST(RegistrationTryLevel, RegisterStoreObservationsRequireAnAdjacentClear) {
  const std::vector<std::vector<uint8_t>> Candidates = {
      {0x33, 0xc0, 0x89, 0x7d, 0xfc},       // different source register
      {0x66, 0x33, 0xff, 0x89, 0x7d, 0xfc}, // only DI is cleared
      {0x33, 0xff, 0x47, 0x89, 0x7d, 0xfc}, // intervening increment
      {0x33, 0xf8, 0x89, 0x7d, 0xfc},       // not a self-XOR
      {0x33, 0xff, 0x89, 0x7e, 0xfc},       // different memory base
      {0x33, 0xff, 0x88, 0x7d, 0xfc},       // byte store
      {0x33, 0xff, 0x66, 0x89, 0x7d, 0xfc}, // word store
      {0x33, 0xe4, 0x89, 0x65, 0xfc},       // destroys the stack pointer
      {0x33, 0xed, 0x89, 0x6d, 0xfc},       // destroys the frame base
      {0x89, 0x7d, 0xfc},                   // no observed clear
  };
  for (size_t I = 0; I < Candidates.size(); ++I) {
    SCOPED_TRACE(I);
    ImageBuilder B;
    B.addScope(-1, kText + 0x80, kText + 0x90);
    B.endScopes();
    emitInstall(B.Text, -1, kRData);
    B.Text.insert(B.Text.end(), Candidates[I].begin(), Candidates[I].end());
    B.Text.push_back(0xc3);
    B.Text.resize(0x80, 0xcc);
    B.addStub();
    B.Text.resize(0x90, 0xcc);
    B.addStub();
    const auto Image = B.build({{"guarded", kText}});
    const auto *Chain = chainAt(Image, kText);
    ASSERT_NE(Chain, nullptr);
    EXPECT_TRUE(Chain->TryLevelStores.empty());
  }
}

TEST(RegistrationTryLevel, RegisterZeroHintsCannotAuthorizeUnprovenStates) {
  for (bool InteriorInstruction : {false, true}) {
    SCOPED_TRACE(InteriorInstruction);
    ImageBuilder B;
    B.addScope(-1, kText + 0x80, kText + 0x90);
    B.endScopes();
    emitInstall(B.Text, -1, kRData);
    if (InteriorInstruction)
      // The apparent XOR and MOV start in the immediate of MOV EAX, imm32.
      B.Text.push_back(0xb8);
    else {
      B.Text.push_back(0xbf); // mov edi, 1
      emit32(B.Text, 1);
      B.Text.insert(B.Text.end(), {0x85, 0xc9, 0x74, 0x02});
      // The conditional branch skips the clear and reaches the same store.
    }
    B.Text.insert(B.Text.end(), {0x33, 0xff});
    const va_t Candidate = B.textVA();
    B.Text.insert(B.Text.end(), {0x89, 0x7d, 0xfc});
    const va_t After = B.textVA();
    B.Text.push_back(0x90);
    emitFrameStore(B.Text, -4, -1);
    B.Text.push_back(0xc3);
    B.Text.resize(0x80, 0xcc);
    B.addStub();
    B.Text.resize(0x90, 0xcc);
    B.addStub();
    auto Image = B.build({{"guarded", kText}});
    const auto *Chain = chainAt(Image, kText);
    ASSERT_NE(Chain, nullptr);
    ASSERT_EQ(Chain->TryLevelStores.size(), 2u);
    EXPECT_EQ(Chain->TryLevelStores.front().StoreVA, Candidate);
    auto Function = liftEntry(Image, kText);
    ASSERT_TRUE(Function.hasCompleteLiftCoverage());
    ASSERT_TRUE(Function.RegistrationStates);
    EXPECT_FALSE(Function.RegistrationStates->Complete);
    EXPECT_FALSE(Function.RegistrationStates->SEHStateStoresComplete);
    // No trusted state-write receipt may arise from the byte observation.
    for (const auto &Store : Function.RegistrationStates->SEHStateStores)
      EXPECT_NE(Store.Address, Candidate);
    const auto *Block = Function.blockFor(After);
    ASSERT_NE(Block, nullptr);
    const auto State =
        std::find_if(Function.RegistrationStates->Blocks.begin(),
                     Function.RegistrationStates->Blocks.end(),
                     [&](const auto &S) { return S.BlockId == Block->Id; });
    ASSERT_NE(State, Function.RegistrationStates->Blocks.end());
    // Conservative dispatch edges remain inspectable, but cannot claim a
    // proven current level or authorize native exception rewriting.
    EXPECT_TRUE(State->Unknown);
  }
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

BinaryImage makeCxxEAXHelperImage(bool Wide, unsigned Variant,
                                  unsigned Mutation = 0) {
  auto Image = makeCxxContinuationImage();
  constexpr va_t Helper = kText + 0x100;
  constexpr va_t Handler = kText + 0x200;
  constexpr va_t Cookie = kRData + 0xf0;
  auto &Text = Image.Segments.front();
  Text.Data.assign(0x280, 0xcc);
  Text.Size = Text.FileSz = Text.Data.size();
  Image.Sections.front().Size = Image.Sections.front().FileSz = Text.Size;
  Image.DynInfo.SecurityCookieRVA = Cookie - kBase;
  Text.Data[0] = Mutation == 1 ? 0xb8 : 0xc3;
  if (Mutation == 2)
    std::fill(Text.Data.begin(), Text.Data.begin() + 0x20, 0x90);
  auto Caller = [&](size_t At, bool Returns) {
    std::vector<uint8_t> Bytes{uint8_t(Wide ? 0x68 : 0x6a)};
    const uint32_t Allocation = Mutation == 3 ? uint32_t(-1)
                                : Mutation == 4
                                    ? limits::kMaxSourceCallBorrowedBytes + 1
                                    : 0x20;
    if (Wide)
      emit32(Bytes, Allocation);
    else
      Bytes.push_back(Mutation == 4 ? 0x80 : uint8_t(Allocation));
    Bytes.push_back(Mutation == 5 ? 0xb9 : 0xb8);
    emit32(Bytes, Mutation == 14 ? kRData : Handler);
    Bytes.push_back(0xe8);
    emit32(Bytes,
           Helper + (Mutation == 13 ? 1 : 0) - (kText + At + Bytes.size() + 4));
    Bytes.push_back(Returns ? 0xc3 : 0x90);
    std::copy(Bytes.begin(), Bytes.end(), Text.Data.begin() + At);
  };
  Caller(Mutation == 1 ? 1 : 0x20, Mutation != 16);
  if (Mutation == 16)
    std::fill(Text.Data.begin() + 0x20 + (Wide ? 15 : 12),
              Text.Data.begin() + 0x50, 0x90);
  Caller(0x50, true);
  Text.Data[0xa0] = 0xc3;

  // Independent machine bytes for the helper's stack protocol. The caller
  // provides the allocation word and EAX handler; the helper saves the old
  // EBP, moves its return PC, initializes state -1, and publishes EBP-12.
  std::vector<uint8_t> Body = {0x50, 0x64, 0xff, 0x35, 0,    0,    0,    0,
                               0x8d, 0x44, 0x24, 0x0c, 0x2b, 0x64, 0x24, 0x0c,
                               0x53, 0x56, 0x57, 0x89, 0x28, 0x8b, 0xe8, 0xa1};
  emit32(Body, Mutation == 6 ? Cookie + 4 : Cookie);
  Body.insert(Body.end(), {0x33, 0xc5, 0x50});
  if (Variant != 0)
    Body.insert(Body.end(), {0x89, uint8_t(Variant == 1 ? 0x65 : 0x45), 0xf0});
  const size_t Tail = Body.size();
  Body.insert(Body.end(),
              {0xff, 0x75, 0xfc, 0xc7, 0x45, 0xfc, 0xff, 0xff, 0xff, 0xff,
               0x8d, 0x45, 0xf4, 0x64, 0xa3, 0,    0,    0,    0,    0xc3});
  if (Mutation == 8)
    Body[Tail + 15] = 4; // a different FS slot
  if (Mutation == 9)
    Body[Tail + 6] = 0; // a different initial C++ state
  if (Mutation == 10)
    Body[20] = 0x30; // save ESI instead of EBP
  if (Mutation == 11)
    Body.back() = 0xc2; // a different return-stack effect
  if (Mutation == 15)
    Body[Tail + 2] = 0xf8; // move the handler instead of the return PC
  std::copy(Body.begin(), Body.end(), Text.Data.begin() + 0x100);
  Text.Data[0x200] = 0xb8;
  writeLE<uint32_t>(Text.Data.data() + 0x201, kRData);
  Text.Data[0x205] = 0xff;
  Text.Data[0x206] = 0x25;
  writeLE<uint32_t>(Text.Data.data() + 0x207, kRData + 0x90);
  Image.Imports.push_back(
      {"vcruntime140.dll", "__CxxFrameHandler3", 0, kRData + 0x90});
  Image.Symbols.clear();
  for (const auto &[Name, Address] : std::vector<std::pair<std::string, va_t>>{
           {"preceding", kText}, {"helper", Helper}, {"handler", Handler}}) {
    Symbol S;
    S.Name = Name;
    S.Addr = Address;
    S.IsFunc = true;
    Image.Symbols.push_back(std::move(S));
  }
  if (Mutation == 7)
    Image.DynInfo.SecurityCookieRVA = 0;
  if (Mutation == 12) {
    Text.Flags = Text.Flags | SegmentFlags::Writable;
    Image.Sections.front().Flags = Text.Flags;
  }
  Image.ExceptionMetadata = {};
  coff_loader::parseX86RegistrationExceptions(Image);
  return Image;
}

TEST(RegistrationTryLevel, CxxEAXHelperCallersHaveCheckedSourceSlots) {
  for (bool Wide : {false, true})
    for (unsigned Variant : {0, 1, 2}) {
      SCOPED_TRACE(::testing::Message() << Wide << " " << Variant);
      const auto Image = makeCxxEAXHelperImage(Wide, Variant);
      EXPECT_EQ(chainAt(Image, kText), nullptr);
      EXPECT_EQ(chainAt(Image, kText + 0x100), nullptr);
      for (va_t Entry : {kText + 0x20, kText + 0x50}) {
        const auto *EH = Image.ExceptionMetadata.findFunction(Entry);
        ASSERT_NE(EH, nullptr);
        EXPECT_EQ(EH->CodeRange.Begin, Entry);
        EXPECT_EQ(EH->Personality, ExceptionPersonality::CxxFrameHandler3);
        EXPECT_EQ(EH->ParseStatus, ExceptionParseStatus::Complete);
        ASSERT_TRUE(EH->Cxx);
        EXPECT_EQ(EH->Cxx->MaxState, 2);
        EXPECT_EQ(EH->Cxx->TryBlocks.size(), 1u);
        ASSERT_TRUE(EH->Registration);
        const auto &Chain = *EH->Registration;
        EXPECT_EQ(Chain.HandlerVA, kText + 0x200);
        EXPECT_EQ(Chain.ScopeTableVA, kRData);
        EXPECT_EQ(Chain.SeededTryLevel, -1);
        EXPECT_EQ(Chain.RegistrationOffset, -12);
        EXPECT_EQ(Chain.TryLevelOffset, -4);
        EXPECT_EQ(Chain.cxxRuntimeFrameOffset(), 0);
        EXPECT_TRUE(Chain.TryLevelStores.empty());
        EXPECT_FALSE(Image.VerifiedFunctionEntries.count(Entry));
        EXPECT_FALSE(coff_loader::getX86RegistrationChainStoreSize(
            Image, Chain.ChainInstallVA));
      }
      EXPECT_EQ(
          Image.ExceptionMetadata.findFunction(kText + 0x20)->CodeRange.End,
          kText + 0x50);
    }
}

TEST(RegistrationTryLevel, CxxEAXHelperRequiresExactSourceProtocol) {
  for (bool Wide : {false, true})
    for (unsigned Variant : {0, 1, 2})
      for (unsigned Mutation = 3; Mutation != 16; ++Mutation) {
        SCOPED_TRACE(::testing::Message()
                     << Wide << " " << Variant << " " << Mutation);
        const auto Image = makeCxxEAXHelperImage(Wide, Variant, Mutation);
        EXPECT_EQ(chainAt(Image, kText + 0x20), nullptr);
        EXPECT_EQ(chainAt(Image, kText + 0x50), nullptr);
      }
}

BinaryImage makeCxxEAXStateImage(bool Wide, unsigned Variant,
                                 uint32_t Allocation = 32) {
  auto Image = makeCxxEAXHelperImage(Wide, Variant);
  auto &Text = Image.Segments.front().Data;
  const size_t Begin = 0x20;
  if (Wide)
    writeLE<uint32_t>(Text.data() + Begin + 1, Allocation);
  else
    Text[Begin + 1] = uint8_t(Allocation);
  std::vector<uint8_t> Body;
  emitFrameStore(Body, -4, 0);
  emitFrameStore(Body, -4, -1);
  Body.insert(Body.end(),
              {0x8b, 0x4d, 0xf4, 0x64, 0x89, 0x0d, 0, 0, 0, 0, 0x8d, 0x64, 0x24,
               uint8_t(Allocation + 28), 0x5d, 0x31, 0xc0, 0xc3});
  std::copy(Body.begin(), Body.end(), Text.begin() + Begin + (Wide ? 15 : 12));
  // A cleanup-only graph exercises the parent's state slot without claiming
  // the separate physical catch-entry protocol.
  for (auto &Segment : Image.Segments)
    if (Segment.VA == kRData) {
      writeLE<uint32_t>(Segment.Data.data() + 12, 0);
      writeLE<uint32_t>(Segment.Data.data() + 16, 0);
    }
  Image.ExceptionMetadata = {};
  coff_loader::parseX86RegistrationExceptions(Image);
  return Image;
}

TEST(RegistrationTryLevel, CxxEAXHelperOwnsItsDecodedStateTransitions) {
  for (bool Wide : {false, true})
    for (unsigned Variant : {0, 1, 2})
      for (uint32_t Allocation : {0, 1, 4, 32}) {
        SCOPED_TRACE(::testing::Message()
                     << Wide << " " << Variant << " " << Allocation);
        auto Image = makeCxxEAXStateImage(Wide, Variant, Allocation);
        const auto *EH = Image.ExceptionMetadata.findFunction(kText + 0x20);
        ASSERT_NE(EH, nullptr);
        ASSERT_TRUE(EH->Registration);
        EXPECT_EQ(EH->Registration->RegistrationOffset, -12);
        EXPECT_EQ(EH->Registration->TryLevelOffset, -4);
        ASSERT_EQ(EH->Registration->TryLevelStores.size(), 2u);
        const auto Function = liftEntry(Image, kText + 0x20);
        ASSERT_TRUE(Function.RegistrationStates);
        const auto &States = *Function.RegistrationStates;
        EXPECT_TRUE(States.Complete);
        EXPECT_TRUE(States.RegistrationLifetimeComplete);
        EXPECT_TRUE(States.ChainOperationsComplete);
        EXPECT_TRUE(States.CallFrameEffectsComplete);
        ASSERT_TRUE(States.CxxPrologue);
        EXPECT_EQ(States.CxxPrologue->Address,
                  EH->Registration->ChainInstallVA);
        EXPECT_EQ(States.CxxPrologue->HelperVA, kText + 0x100);
        EXPECT_EQ(States.CxxPrologue->StackOffset, -28 - int32_t(Allocation));
        EXPECT_EQ(States.callFrameEffect(States.CxxPrologue->Address,
                                         States.CxxPrologue->OpSeq),
                  nullptr);
        EXPECT_FALSE(cxxRegistrationFrameCoordinate(*EH, &States));
        EXPECT_TRUE(
            std::any_of(States.FrameValues.begin(), States.FrameValues.end(),
                        [&](const auto &Value) {
                          return Value.Address == States.CxxPrologue->Address &&
                                 Value.OpSeq == States.CxxPrologue->OpSeq &&
                                 Value.EstablishedFrameOffset == -12;
                        }));
        const va_t Body = kText + 0x20 + (Wide ? 15 : 12);
        // Restore ESP from its post-helper value, independently of EBP.
        // The exact source LEA must reach the saved incoming EBP at offset 0.
        EXPECT_TRUE(std::any_of(States.FrameValues.begin(),
                                States.FrameValues.end(),
                                [&](const auto &Value) {
                                  return Value.Address == Body + 24 &&
                                         Value.EstablishedFrameOffset == 0;
                                }));
        unsigned Matched = 0;
        for (const auto &State : States.Blocks) {
          if (State.Range.Begin == Body) {
            ++Matched;
            EXPECT_EQ(State.levels(), (std::vector<int32_t>{-1}));
          }
          if (State.Range.Begin == Body + 7) {
            ++Matched;
            EXPECT_EQ(State.levels(), (std::vector<int32_t>{0}));
          }
          if (State.Range.Begin == Body + 14) {
            ++Matched;
            EXPECT_EQ(State.levels(), (std::vector<int32_t>{-1}));
          }
        }
        EXPECT_EQ(Matched, 3u);
      }
}

TEST(RegistrationTryLevel, CxxEAXHelperRequiresItsExactLowIRInvocation) {
  auto Image = makeCxxEAXStateImage(false, 1);
  const auto Original = liftEntry(Image, kText + 0x20);
  ASSERT_TRUE(Original.RegistrationStates);
  ASSERT_TRUE(Original.RegistrationStates->Complete);
  const auto &EH = *Original.ExceptionMetadata;
  const auto Call = coff_loader::getCheckedX86CxxPrologueCall(Image, EH);
  const auto Handler = coff_loader::getCheckedX86CxxHandlerABI(Image, EH);
  ASSERT_TRUE(Call);
  ASSERT_TRUE(Handler);
  const std::vector<RegistrationCalleeFrameContract> Callees;
  const std::vector<RegistrationCleanupFrameContract> Cleanups;
  for (unsigned Mutation = 0; Mutation != 33; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Function = Original;
    auto &Block = Function.Blocks.front();
    ASSERT_EQ(Block.InstructionBoundaries.size(), 3u);
    auto &Boundary = Block.InstructionBoundaries.back();
    auto &Op = Block.Ops[Boundary.FirstOp];
    ASSERT_EQ(Op.Opcode, NdOp::CALL);
    switch (Mutation) {
    case 0:
      Boundary.Size = 4;
      break;
    case 1:
      ++Boundary.FirstOp;
      break;
    case 2:
      Boundary.OpCount = 0;
      break;
    case 3:
      Boundary.OpCount = 2;
      break;
    case 4:
      Boundary.Control = LowInstructionControl::None;
      break;
    case 5:
      Boundary.ControlFlags |= LowInstructionControlFlag::Conditional;
      break;
    case 6:
      Boundary.ControlFlags |= LowInstructionControlFlag::NoReturn;
      break;
    case 7:
      Boundary.ControlFlags |= LowInstructionControlFlag::Indirect;
      break;
    case 8:
      Boundary.Mode = InstructionMode::Thumb;
      break;
    case 9:
      Boundary.TargetMode = LowInstructionTargetMode::Thumb;
      break;
    case 10:
      ++*Boundary.Immediate;
      break;
    case 11:
      Boundary.Immediate.reset();
      break;
    case 12:
      ++Boundary.Address;
      break;
    case 13:
      Op.Seq = -1;
      break;
    case 14:
      Op.Output = NdVar::reg(x86reg::RSI, 4);
      break;
    case 15:
      Op.NumInputs = 2;
      break;
    case 16:
      Op.Inputs[0].Size = 8;
      break;
    case 17:
      ++Op.Inputs[0].Offset;
      break;
    case 18:
      Op.MemoryAddressSpace = NdMemoryAddressSpace::X86FS;
      break;
    case 19:
      Op.Opcode = NdOp::INDIR_CALL;
      break;
    case 20:
      Op.Output.Size = 2;
      break;
    case 21:
      Block.Preds.push_back(Block.Id);
      break;
    case 22:
      ++Function.Entry;
      break;
    case 23:
      Function.ExceptionMetadata->Registration->TryLevelOffset = -8;
      break;
    case 24:
      Function.ExceptionMetadata->Registration->SeededTryLevel = 0;
      break;
    case 25:
      // The allocation word reaches the helper through an actual source
      // store; a descriptor cannot replace a different lifted input.
      for (auto &Input : Block.Ops)
        if (Input.Addr == Function.Entry && Input.Opcode == NdOp::STORE)
          ++Input.Inputs[1].Offset;
      break;
    case 26:
      for (auto &Input : Block.Ops)
        if (Input.Opcode == NdOp::COPY && Input.Output.isReg() &&
            Input.Output.Offset == x86reg::RAX)
          ++Input.Inputs[0].Offset;
      break;
    case 27:
      for (auto &Input : Block.Ops)
        if (Input.Addr == Function.Entry && Input.Opcode == NdOp::INT_SUB)
          Input.Opcode = NdOp::INT_ADD;
      break;
    case 28:
      Function.OrdinaryModuleAnalysisRoots.insert(Call->Caller.End);
      break;
    case 29:
      Function.ExceptionMetadata->Cxx->TryBlocks.emplace_back();
      break;
    case 30:
      break; // An absent current-image description cannot use metadata.
    case 31:
      Op.MemoryOrdering = NdMemoryOrdering::Relaxed;
      break;
    case 32:
      Function.OrdinaryModuleAnalysisRoots.insert(Call->CallVA);
      break;
    }
    const auto States = analyzeRegistrationStates(
        Function, Call->SecurityCookieVA, 0, &Callees, &Cleanups, nullptr,
        nullptr, nullptr, &*Handler, Mutation == 30 ? nullptr : &*Call);
    EXPECT_FALSE(States.Complete);
    EXPECT_FALSE(States.CxxPrologue);
  }
}

TEST(RegistrationTryLevel, CxxEAXHelperRechecksBytesBeforeStateAnalysis) {
  const auto Original = makeCxxEAXStateImage(false, 1);
  for (unsigned Mutation = 0; Mutation != 6; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Image = Original;
    switch (Mutation) {
    case 0:
      Image.Segments.front().Data[0x100 + 20] ^= 1;
      break;
    case 1:
      Image.Segments.front().Data[0x20 + 2] = 0xb9;
      break;
    case 2:
      Image.Segments.front().Data[0x200] = 0xb9;
      break;
    case 3:
      Image.DynInfo.SecurityCookieRVA += 4;
      break;
    case 4:
      Image.Segments.front().Flags =
          Image.Segments.front().Flags | SegmentFlags::Writable;
      break;
    case 5:
      Image.BaseRelocations.push_back({kText + 0x101, 3});
      break;
    }
    const auto Function = liftEntry(Image, kText + 0x20);
    ASSERT_TRUE(Function.RegistrationStates);
    EXPECT_FALSE(Function.RegistrationStates->Complete);
    EXPECT_FALSE(Function.RegistrationStates->CxxPrologue);
  }
}

TEST(RegistrationTryLevel, CxxEAXHelperReportsOrderedPhysicalEffects) {
  using Value = coff_loader::X86CxxPrologueWrite::Value;
  for (bool Wide : {false, true})
    for (unsigned Variant : {0, 1, 2})
      for (uint32_t Allocation :
           {0u, 1u, 4u, 32u, 127u, limits::kMaxSourceCallBorrowedBytes}) {
        if (!Wide && Allocation > 127)
          continue;
        SCOPED_TRACE(::testing::Message()
                     << Wide << " " << Variant << " " << Allocation);
        auto Image = makeCxxEAXHelperImage(Wide, Variant);
        const auto *EH = Image.ExceptionMetadata.findFunction(kText + 0x20);
        ASSERT_TRUE(EH);
        if (Wide)
          writeLE<uint32_t>(Image.Segments.front().Data.data() + 0x21,
                            Allocation);
        else
          Image.Segments.front().Data[0x21] = Allocation;
        const auto Call = coff_loader::getCheckedX86CxxPrologueCall(Image, *EH);
        ASSERT_TRUE(Call);
        EXPECT_EQ(Call->Caller.Begin, kText + 0x20);
        EXPECT_EQ(Call->Caller.End, kText + (Wide ? 0x2f : 0x2c));
        EXPECT_EQ(Call->CallVA, Call->Caller.End - 5);
        EXPECT_EQ(Call->Helper.Begin, kText + 0x100);
        EXPECT_EQ(Call->Helper.End, kText + 0x100 + (Variant ? 54 : 51));
        EXPECT_EQ(Call->HandlerVA, kText + 0x200);
        EXPECT_EQ(Call->SecurityCookieVA, kRData + 0xf0);
        EXPECT_EQ(Call->AllocationBytes, Allocation);
        // At helper entry the allocation word is above the return PC. The
        // two node pushes, allocation, three GPR saves and cookie put ESP
        // this far below the new EBP when that return PC is popped.
        const int32_t Bytes = Allocation;
        EXPECT_EQ(Call->PostCallStackOffset, -28 - Bytes);
        std::vector<coff_loader::X86CxxPrologueWrite> Expected{
            {-8, Value::Handler},
            {-12, Value::PreviousChain},
            {-16 - Bytes, Value::EBX},
            {-20 - Bytes, Value::ESI},
            {-24 - Bytes, Value::EDI},
            {0, Value::PreviousEBP},
            {-28 - Bytes, Value::EncodedCookie}};
        if (Variant)
          Expected.push_back(
              {-16, Variant == 1 ? Value::StackPointer : Value::EncodedCookie});
        Expected.push_back({-4, Value::InitialState});
        EXPECT_EQ(Call->Writes, Expected);
        // In particular allocation zero retains the extra store's overwrite
        // of the EBX save; it cannot grant restored-register authority.
        EXPECT_EQ(EH->Registration->RegistrationOffset, -12);
        EXPECT_EQ(EH->Registration->TryLevelOffset, -4);
      }
}

TEST(RegistrationTryLevel, CxxEAXHelperRechecksEveryProtocolInstruction) {
  for (bool Wide : {false, true})
    for (unsigned Variant : {0, 1, 2}) {
      const auto Original = makeCxxEAXHelperImage(Wide, Variant);
      const auto &EH = *Original.ExceptionMetadata.findFunction(kText + 0x20);
      ASSERT_TRUE(coff_loader::getCheckedX86CxxPrologueCall(Original, EH));
      const uint32_t PushSize = Wide ? 5 : 2;
      for (const auto &[Begin, End] :
           std::vector<std::pair<uint32_t, uint32_t>>{
               {0x20, 0x20 + PushSize + 10},
               {0x100, 0x100 + (Variant ? 54 : 51)}})
        for (uint32_t Byte = Begin; Byte != End; ++Byte) {
          if (Byte > 0x20 && Byte < 0x20 + PushSize)
            continue; // The current allocation is independently decoded.
          SCOPED_TRACE(::testing::Message()
                       << Wide << " " << Variant << " " << Byte);
          auto Changed = Original;
          Changed.Segments.front().Data[Byte] ^= 1;
          EXPECT_FALSE(coff_loader::getCheckedX86CxxPrologueCall(Changed, EH));
        }
    }
}

TEST(RegistrationTryLevel, CxxEAXHelperRechecksItsCurrentParentAndStorage) {
  const auto Original = makeCxxEAXHelperImage(false, 1);
  const auto &Parent = *Original.ExceptionMetadata.findFunction(kText + 0x20);
  ASSERT_TRUE(coff_loader::getCheckedX86CxxPrologueCall(Original, Parent));
  for (unsigned Mutation = 0; Mutation != 19; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Image = Original;
    auto EH = Parent;
    switch (Mutation) {
    case 0:
      EH.ParseStatus = ExceptionParseStatus::Partial;
      break;
    case 1:
      EH.Encoding = ExceptionEncoding::X86ScopeTableEH3;
      break;
    case 2:
      EH.Registration.reset();
      break;
    case 3:
      EH.Cxx.reset();
      break;
    case 4:
      ++EH.PersonalityVA;
      break;
    case 5:
      ++EH.Registration->ChainInstallVA;
      break;
    case 6:
      ++EH.CodeRange.Begin;
      break;
    case 7:
      EH.CodeRange.End = kText + 0x2b;
      break;
    case 8:
      EH.CodeRange.End = kText + 0x101;
      break;
    case 9:
      ++EH.Cxx->MaxState;
      break;
    case 10:
      Image.Arch = Arch::X64;
      break;
    case 11:
      Image.Bits = Bitness::Bits64;
      break;
    case 12:
      Image.Format = BinaryFormat::ELF;
      break;
    case 13:
      Image.Segments.front().Flags =
          Image.Segments.front().Flags | SegmentFlags::Writable;
      break;
    case 14:
      Image.Segments.push_back(Image.Segments.front());
      break;
    case 15:
      Image.BaseRelocations.push_back({kText + 0x28, 3});
      break;
    case 16:
      Image.BaseRelocations.push_back({kText + 0x119, 3});
      break;
    case 17:
      Image.DynInfo.SecurityCookieRVA = 0;
      break;
    case 18:
      Image.IsRelocatable = true;
      break;
    }
    EXPECT_FALSE(coff_loader::getCheckedX86CxxPrologueCall(Image, EH));
  }
}

TEST(RegistrationTryLevel, CxxEAXHelperInputPE32HasTheCheckedPhysicalCall) {
  const char *Path = std::getenv("NEVERD_REGISTRATION_PROLOGUE_INPUT_PE32");
  if (!Path)
    GTEST_SKIP() << "set NEVERD_REGISTRATION_PROLOGUE_INPUT_PE32";
  auto Image = COFFLoader().load(Path);
  ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
  auto Export = [&](llvm::StringRef Name) {
    const auto Found = llvm::find_if(
        Image->Exports, [&](const auto &Entry) { return Entry.Name == Name; });
    return Found == Image->Exports.end() ? InvalidVA : Found->Addr;
  };
  const auto *EH =
      Image->ExceptionMetadata.findFunction(Export("prologue_parent"));
  ASSERT_TRUE(EH);
  const auto Call = coff_loader::getCheckedX86CxxPrologueCall(*Image, *EH);
  ASSERT_TRUE(Call);
  auto Word = [&](llvm::StringRef Name) {
    const auto *Bytes = Image->readVA(Export(Name), 4);
    return Bytes ? readLE<uint32_t>(Bytes) : UINT32_MAX;
  };
  EXPECT_EQ(Call->AllocationBytes, Word("prologue_allocation"));
  EXPECT_EQ(Call->Helper.Begin, Export("prologue_helper"));
  EXPECT_EQ(Call->HandlerVA, Export("prologue_handler"));
  EXPECT_EQ(Call->Caller.size(), Word("prologue_wide") ? 15u : 12u);
  EXPECT_EQ(Call->Helper.size(), Word("prologue_variant") ? 54u : 51u);
  EXPECT_EQ(Call->Writes.size(), Word("prologue_variant") ? 9u : 8u);
  EXPECT_EQ(EH->Registration->RegistrationOffset, -12);
  EXPECT_EQ(EH->Registration->TryLevelOffset, -4);
  const va_t StateParent = Export("prologue_state_parent");
  ASSERT_NE(StateParent, 0u);
  const auto Function = liftEntry(*Image, StateParent);
  ASSERT_TRUE(Function.RegistrationStates);
  const auto &States = *Function.RegistrationStates;
  EXPECT_TRUE(States.Complete);
  EXPECT_TRUE(States.RegistrationLifetimeComplete);
  EXPECT_TRUE(States.ChainOperationsComplete);
  EXPECT_TRUE(States.CallFrameEffectsComplete);
  ASSERT_TRUE(States.CxxPrologue);
  EXPECT_EQ(States.CxxPrologue->HelperVA, Export("prologue_helper"));
  EXPECT_EQ(States.CxxPrologue->StackOffset,
            -28 - int32_t(Word("prologue_allocation")));
  EXPECT_FALSE(
      cxxRegistrationFrameCoordinate(*Function.ExceptionMetadata, &States));
  LowToMedConverter Converter;
  Converter.setBinaryImage(&*Image);
  const auto Med = Converter.convert(Function, Arch::X86, BinaryFormat::COFF);
  ASSERT_TRUE(verifyMedFunc(Med, "shared-cxx-pe32-frame"));
  EXPECT_GE(Med.FrameSize, 36 + Word("prologue_allocation"));
  unsigned Installations = 0;
  for (const auto &Block : Med.Blocks)
    for (const auto &Op : Block.Ops) {
      EXPECT_FALSE(Op.Addr == States.CxxPrologue->Address &&
                   Op.Opcode == NdOp::CALL);
      if (Op.Addr == States.CxxPrologue->Address && Op.Opcode == NdOp::STORE &&
          Op.MemoryAddressSpace == NdMemoryAddressSpace::X86FS) {
        ++Installations;
        ASSERT_EQ(Op.NumInputs, 2u);
        EXPECT_EQ(
            entryStackOffset(Med, Op.Inputs[1], Arch::X86, BinaryFormat::COFF),
            -16);
      }
    }
  EXPECT_EQ(Installations, 1u);
  ASSERT_TRUE(Med.RegistrationStates);
  ASSERT_TRUE(Med.RegistrationStates->CxxPrologue);
  ASSERT_TRUE(Med.RegistrationPhysicalFrame);
  EXPECT_TRUE(hasCheckedX86RegistrationCxxPhysicalFrame(*Image, Med));
  EXPECT_TRUE(cxxRegistrationFrameCoordinate(Med));
  EXPECT_FALSE(cxxRegistrationFrameCoordinate(*Med.ExceptionMetadata,
                                              &*Med.RegistrationStates));
  const va_t ExitParent = Export("epilogue_state_parent");
  ASSERT_NE(ExitParent, InvalidVA);
  const auto ExitFunction = liftEntry(*Image, ExitParent);
  ASSERT_TRUE(ExitFunction.ExceptionMetadata);
  ASSERT_TRUE(ExitFunction.RegistrationStates);
  bool FoundExit = false;
  va_t ExitCall = InvalidVA;
  for (const auto &Block : ExitFunction.Blocks)
    for (const auto &Op : Block.Ops)
      if (Op.Opcode == NdOp::CALL && Op.NumInputs == 1 &&
          Op.Inputs[0].isConst() &&
          Op.Inputs[0].Offset == Export("epilogue_helper")) {
        const auto Exit = coff_loader::getCheckedX86CxxEpilogueCall(
            *Image, *ExitFunction.ExceptionMetadata, Op.Addr);
        ASSERT_TRUE(Exit);
        EXPECT_EQ(Exit->Caller.size(), 5u);
        EXPECT_EQ(Exit->Helper.size(), 20u);
        FoundExit = true;
        ExitCall = Op.Addr;
      }
  EXPECT_TRUE(FoundExit);
  EXPECT_TRUE(getCheckedX86RegistrationCxxParentABI(Function, *Image));
  const bool IntactSaves =
      Word("prologue_variant") == 0 || Word("prologue_allocation") >= 4;
  const auto &ExitStates = *ExitFunction.RegistrationStates;
  EXPECT_EQ(ExitStates.RegistrationLifetimeComplete, IntactSaves);
  EXPECT_EQ(ExitStates.ChainOperationsComplete, IntactSaves);
  EXPECT_EQ(ExitStates.CallFrameEffectsComplete, IntactSaves);
  EXPECT_EQ(bool(ExitStates.CxxPrologue), IntactSaves);
  EXPECT_FALSE(cxxRegistrationFrameCoordinate(*ExitFunction.ExceptionMetadata,
                                              &ExitStates));
  const auto ExitMed =
      Converter.convert(ExitFunction, Arch::X86, BinaryFormat::COFF);
  const bool RetainedExitCall = std::any_of(
      ExitMed.Blocks.begin(), ExitMed.Blocks.end(), [&](const auto &Block) {
        return std::any_of(
            Block.Ops.begin(), Block.Ops.end(), [&](const auto &Op) {
              return Op.Addr == ExitCall && Op.Opcode == NdOp::CALL;
            });
      });
  EXPECT_EQ(RetainedExitCall, !IntactSaves);
  EXPECT_EQ(bool(getCheckedX86RegistrationCxxParentABI(ExitFunction, *Image)),
            IntactSaves);
  EXPECT_EQ(bool(ExitMed.RegistrationPhysicalFrame), IntactSaves);
  EXPECT_EQ(bool(cxxRegistrationFrameCoordinate(ExitMed)), IntactSaves);
  if (IntactSaves)
    EXPECT_TRUE(hasCheckedX86RegistrationCxxPhysicalFrame(*Image, ExitMed));
  const va_t GuardedParent = Export("guarded_state_parent");
  ASSERT_NE(GuardedParent, InvalidVA);
  const auto GuardedFunction = liftEntry(*Image, GuardedParent);
  ASSERT_TRUE(GuardedFunction.ExceptionMetadata);
  ASSERT_TRUE(GuardedFunction.RegistrationStates);
  bool FoundGuardedExit = false;
  for (const auto &Block : GuardedFunction.Blocks)
    for (const auto &Op : Block.Ops)
      if (Op.Opcode == NdOp::CALL && Op.NumInputs == 1 &&
          Op.Inputs[0].isConst() &&
          Op.Inputs[0].Offset == Export("guarded_helper")) {
        const auto Exit = coff_loader::getCheckedX86CxxEpilogueCall(
            *Image, *GuardedFunction.ExceptionMetadata, Op.Addr);
        ASSERT_TRUE(Exit);
        EXPECT_EQ(Exit->Helper.size(), 15u);
        EXPECT_EQ(Exit->Restore.Begin, Export("epilogue_helper"));
        EXPECT_TRUE(Exit->CookieCheck);
        FoundGuardedExit = true;
      }
  EXPECT_TRUE(FoundGuardedExit);
  const bool IntactCookie =
      Word("prologue_variant") == 2 && Word("prologue_allocation") >= 4;
  const auto &GuardedStates = *GuardedFunction.RegistrationStates;
  EXPECT_EQ(GuardedStates.RegistrationLifetimeComplete, IntactCookie);
  EXPECT_EQ(GuardedStates.ChainOperationsComplete, IntactCookie);
  EXPECT_EQ(GuardedStates.SecurityCookiesComplete, IntactCookie);
  EXPECT_EQ(GuardedStates.CallFrameEffectsComplete, IntactCookie);
  EXPECT_FALSE(cxxRegistrationFrameCoordinate(
      *GuardedFunction.ExceptionMetadata, &GuardedStates));
  EXPECT_FALSE(getCheckedX86RegistrationCxxParentABI(GuardedFunction, *Image));
  const auto GuardedMed =
      Converter.convert(GuardedFunction, Arch::X86, BinaryFormat::COFF);
  EXPECT_FALSE(GuardedMed.RegistrationPhysicalFrame);
  EXPECT_FALSE(cxxRegistrationFrameCoordinate(GuardedMed));
  if (GuardedStates.CxxPrologue)
    EXPECT_FALSE(hasCheckedX86RegistrationCxxPhysicalFrame(*Image, GuardedMed));
}

TEST(RegistrationTryLevel, CxxEAXHelperProjectsItsPhysicalFrameBeforeSSA) {
  for (bool Wide : {false, true})
    for (unsigned Variant : {0, 1, 2})
      for (uint32_t Allocation : {0, 1, 4, 32}) {
        SCOPED_TRACE(::testing::Message()
                     << Wide << " " << Variant << " " << Allocation);
        auto Image = makeCxxEAXStateImage(Wide, Variant, Allocation);
        const va_t Entry = kText + 0x20;
        const va_t Body = Entry + (Wide ? 15 : 12);
        const va_t ArgumentRead = Body + 28;
        // Read the caller's argument before restoring its EBP. The shared
        // prologue establishes EBP at entry ESP - 4, so this is entry ESP + 4.
        const uint8_t Tail[] = {0x8b, 0x45, 0x08, 0x5d, 0xc3};
        auto &Bytes = Image.Segments.front().Data;
        std::copy(std::begin(Tail), std::end(Tail),
                  Bytes.begin() + ArgumentRead - kText);
        Image.ExceptionMetadata = {};
        coff_loader::parseX86RegistrationExceptions(Image);
        const auto Low = liftEntry(Image, Entry);
        ASSERT_TRUE(Low.RegistrationStates);
        ASSERT_TRUE(Low.RegistrationStates->ChainOperationsComplete);
        ASSERT_TRUE(Low.RegistrationStates->CxxPrologue);
        LowToMedConverter Converter;
        Converter.setBinaryImage(&Image);
        const auto Med = Converter.convert(Low, Arch::X86, BinaryFormat::COFF);
        ASSERT_TRUE(verifyMedFunc(Med, "shared-cxx-physical-frame"));
        ASSERT_EQ(Med.Params.size(), 1u);
        EXPECT_EQ(Med.Params.front().RegOff, kNoParamReg);
        EXPECT_GE(Med.FrameSize, 36 + Allocation);
        unsigned Returns = 0;
        const va_t Call = Low.RegistrationStates->CxxPrologue->Address;
        std::vector<int64_t> Writes;
        for (const auto &Block : Med.Blocks)
          for (const auto &Op : Block.Ops) {
            EXPECT_FALSE(Op.Addr == Call && Op.Opcode == NdOp::CALL);
            if (Op.Opcode == NdOp::RETURN) {
              ++Returns;
              ASSERT_EQ(Op.NumInputs, 1u);
              // The now-rooted [EBP+8] is the first physical stack argument.
              // Ordinary parameter recovery may replace its LOAD with arg0.
              EXPECT_EQ(Op.Inputs[0], Med.Params.front());
            }
            if (Op.Addr == Call && Op.Opcode == NdOp::STORE &&
                Op.MemoryAddressSpace == NdMemoryAddressSpace::Default) {
              ASSERT_EQ(Op.NumInputs, 2u);
              EXPECT_EQ(Op.Inputs[1].Size, 4u);
              const auto Offset = entryStackOffset(Med, Op.Inputs[0], Arch::X86,
                                                   BinaryFormat::COFF);
              ASSERT_TRUE(Offset);
              Writes.push_back(*Offset);
            }
          }
        EXPECT_EQ(Returns, 1u);
        const int64_t A = Allocation;
        std::vector<int64_t> Expected{-8,      -12,     -16, -20 - A,
                                      -24 - A, -28 - A, -4,  -32 - A};
        if (Variant)
          Expected.push_back(-20);
        Expected.insert(Expected.end(), {-36 - A, -8});
        EXPECT_EQ(Writes, Expected);
        // Projecting physical dataflow is not a native EH reconstruction
        // receipt; the existing runtime-frame gate stays independent.
        EXPECT_FALSE(cxxRegistrationFrameCoordinate(*Low.ExceptionMetadata,
                                                    &*Low.RegistrationStates));
      }
}

TEST(RegistrationTryLevel, CxxEAXHelperProjectionRechecksTheCurrentEntry) {
  auto Original = makeCxxEAXStateImage(false, 2);
  const auto Proven = liftEntry(Original, kText + 0x20);
  ASSERT_TRUE(Proven.RegistrationStates);
  ASSERT_TRUE(Proven.RegistrationStates->CxxPrologue);
  ASSERT_TRUE(getCheckedX86RegistrationCxxParentABI(Proven, Original));
  const va_t Call = Proven.RegistrationStates->CxxPrologue->Address;
  for (unsigned Mutation = 0; Mutation != 33; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Image = Original;
    auto Low = Proven;
    auto &Entry =
        *std::find_if(Low.Blocks.begin(), Low.Blocks.end(),
                      [&](const auto &B) { return B.StartAddr == Low.Entry; });
    auto &Boundary = Entry.InstructionBoundaries.back();
    switch (Mutation) {
    case 0:
      Low.RegistrationStates.reset();
      break;
    case 1:
      Low.RegistrationStates->ChainOperationsComplete = false;
      break;
    case 2:
      Low.RegistrationStates->CxxPrologue.reset();
      break;
    case 3:
      ++Low.RegistrationStates->CxxPrologue->Address;
      break;
    case 4:
      ++Low.RegistrationStates->CxxPrologue->EndAddress;
      break;
    case 5:
      ++Low.RegistrationStates->CxxPrologue->HelperVA;
      break;
    case 6:
      ++Low.RegistrationStates->CxxPrologue->StackOffset;
      break;
    case 7:
      ++Low.RegistrationStates->CxxPrologue->OpSeq;
      break;
    case 8:
      Image.Segments.front().Data[0x100 + 20] ^= 1;
      break;
    case 9:
      Image.Segments.front().Data[0x200] = 0xb9;
      break;
    case 10:
      ++Image.Segments.front().Data[0x21];
      break;
    case 11:
      ++Image.DynInfo.SecurityCookieRVA;
      break;
    case 12:
      Image.Segments.front().Flags =
          Image.Segments.front().Flags | SegmentFlags::Writable;
      break;
    case 13:
      Image.BaseRelocations.push_back({kText + 0x101, 3});
      break;
    case 14:
      ++Entry.Ops.front().Inputs[1].Offset;
      break;
    case 15:
      ++Entry.Ops[Entry.InstructionBoundaries[1].FirstOp].Inputs[0].Offset;
      break;
    case 16:
      ++Entry.Ops.back().Seq;
      break;
    case 17:
      Boundary.ControlFlags |= LowInstructionControlFlag::Conditional;
      break;
    case 18:
      --Boundary.FirstOp;
      break;
    case 19:
      ++Boundary.OpCount;
      break;
    case 20:
      Entry.Preds.push_back(Entry.Id);
      break;
    case 21:
      Low.OrdinaryModuleAnalysisRoots.insert(Call);
      break;
    case 22:
      Entry.InstructionBoundaries.pop_back();
      break;
    case 23:
      break; // No image cannot authorize a physical shared-call projection.
    case 30:
      Low.RegistrationStates->Complete = false;
      break;
    case 31:
      Low.RegistrationStates->CallbackStatesComplete = false;
      break;
    case 32:
      Low.RegistrationStates->RegistrationLifetimeComplete = false;
      break;
    default: {
      auto &Accesses = Low.RegistrationStates->ChainAccesses;
      auto It = std::find_if(Accesses.begin(), Accesses.end(),
                             [&](const auto &A) { return A.Address == Call; });
      ASSERT_NE(It, Accesses.end());
      if (Mutation == 24)
        Accesses.erase(It);
      else if (Mutation == 25 || Mutation == 29) {
        auto Extra = *It;
        if (Mutation == 29)
          Extra.AccessKind = RegistrationChainAccess::Kind::ReadPreviousHead;
        Accesses.push_back(Extra);
      } else if (Mutation == 26)
        It->AccessKind = RegistrationChainAccess::Kind::Remove;
      else if (Mutation == 27)
        ++It->EndAddress;
      else
        ++It->OpSeq;
      break;
    }
    }
    if (Mutation != 23)
      EXPECT_FALSE(getCheckedX86RegistrationCxxParentABI(Low, Image));
    LowToMedConverter Converter;
    if (Mutation != 23)
      Converter.setBinaryImage(&Image);
    const auto Med = Converter.convert(Low, Arch::X86, BinaryFormat::COFF);
    EXPECT_TRUE(
        std::any_of(Med.Blocks.begin(), Med.Blocks.end(), [&](const auto &B) {
          return std::any_of(B.Ops.begin(), B.Ops.end(), [&](const auto &Op) {
            return Op.Addr == Call && Op.Opcode == NdOp::CALL;
          });
        }));
  }
}

TEST(RegistrationTryLevel, CxxEAXHelperRequiresClosedPrecedingBody) {
  for (bool Wide : {false, true}) {
    for (unsigned Mutation : {1, 2}) {
      SCOPED_TRACE(::testing::Message() << Wide << " " << Mutation);
      const auto Image = makeCxxEAXHelperImage(Wide, 0, Mutation);
      EXPECT_EQ(chainAt(Image, kText), nullptr);
      EXPECT_EQ(chainAt(Image, kText + (Mutation == 1 ? 1 : 0x20)), nullptr);
    }
    const auto Image = makeCxxEAXHelperImage(Wide, 0, 16);
    EXPECT_NE(chainAt(Image, kText + 0x20), nullptr);
    EXPECT_EQ(chainAt(Image, kText + 0x50), nullptr);
  }
}

TEST(RegistrationTryLevel, CxxEAXHelperRefutesOnlyReachedInteriorGuesses) {
  for (bool Wide : {false, true})
    for (unsigned Root : {0u, 1u, 2u})
      for (unsigned Mutation = 0; Mutation != 10; ++Mutation) {
        SCOPED_TRACE(::testing::Message()
                     << Wide << "/" << Root << "/" << Mutation);
        auto Image = makeCxxEAXHelperImage(Wide, 0);
        auto &Text = Image.Segments.front().Data;
        // The padding guess is in a real CALL's displacement. It cannot
        // decode a closed preceding body, although that CALL independently
        // reaches the complete C++ prologue at +0x20.
        Text[0] = 0xe8;
        writeLE<uint32_t>(Text.data() + 1, 0x20 - 5);
        Text[5] = 0xc3;
        Image.Entry = kText;
        Symbol Guess = Symbol::makeFunc(kText + 2, 0);
        Guess.IsBoundaryGuess = true;
        Image.Symbols.push_back(Guess);
        if (Root == 1) {
          // A separate immutable entry reaches the CALL through a call edge.
          Segment Source;
          Source.VA = kText + 0x2000;
          Source.FileOff = 0x2000;
          Source.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
          Source.Data = {0xe8};
          emit32(Source.Data, kText - (Source.VA + 5));
          Source.Data.push_back(0xc3);
          Source.Size = Source.FileSz = Source.Data.size();
          Section Code;
          Code.VA = Source.VA;
          Code.FileOff = Source.FileOff;
          Code.Size = Code.FileSz = Source.Size;
          Code.Flags = Source.Flags;
          Image.Entry = Source.VA;
          Image.Segments.push_back(std::move(Source));
          Image.Sections.push_back(std::move(Code));
        } else if (Root == 2) {
          // No image entry reaches it. The independently accepted second
          // physical C++ entry calls the same ordinary source function.
          Image.Entry = 0;
          const size_t Call = 0x50 + (Wide ? 15 : 12);
          Text[Call] = 0xe8;
          writeLE<uint32_t>(Text.data() + Call + 1, uint32_t(-Call - 5));
          Text[Call + 5] = 0xc3;
        }
        // Reacquire after adding the separate segment.
        auto &Bytes = Image.Segments.front().Data;
        switch (Mutation) {
        case 1:
          Image.Symbols.back().IsBoundaryGuess = false;
          break;
        case 2:
          Image.Symbols.back().Addr = kText + 0x10;
          break;
        case 3:
          writeLE<uint32_t>(Bytes.data() + 1, 0x50 - 5);
          break;
        case 4:
          // A CALL-looking immediate is not an instruction edge.
          Bytes[0] = 0xb8;
          Bytes[1] = 0xe8;
          writeLE<uint32_t>(Bytes.data() + 2, 0x20 - 6);
          Bytes[6] = 0xc3;
          Image.Symbols.back().Addr = kText + 3;
          break;
        case 5:
          // Keep this away from the separately authenticated entry-jump
          // forwarding protocol: only an ordinary interior branch exists.
          Bytes[0] = 0x90;
          Bytes[1] = 0xe9;
          writeLE<uint32_t>(Bytes.data() + 2, 0x20 - 6);
          Bytes[6] = 0xc3;
          Image.Symbols.back().Addr = kText + 3;
          break;
        case 6:
          Image.BaseRelocations.push_back({kText + 1, 3});
          break;
        case 7:
          Image.BaseRelocations.push_back({kText, 3});
          break;
        case 8:
          Bytes[0] =
              0xc3; // The later candidate and CALL bytes are unreachable.
          break;
        case 9:
          Bytes[0x20 + (Wide ? 5 : 2)] = 0xb9;
          break;
        default:
          break;
        }
        Image.ExceptionMetadata = {};
        coff_loader::parseX86RegistrationExceptions(Image);
        const auto *EH = Image.ExceptionMetadata.findFunction(kText + 0x20);
        if (Mutation) {
          EXPECT_TRUE(!EH || EH->CodeRange.Begin != kText + 0x20);
          continue;
        }
        ASSERT_TRUE(EH);
        EXPECT_EQ(EH->CodeRange.Begin, kText + 0x20);
        EXPECT_TRUE(coff_loader::getCheckedX86CxxPrologueCall(Image, *EH));
        ASSERT_TRUE(EH->Registration);
        EXPECT_EQ(EH->Registration->RegistrationOffset, -12);
        EXPECT_EQ(EH->Registration->TryLevelOffset, -4);
        EXPECT_FALSE(Image.VerifiedFunctionEntries.count(kText + 0x20));
      }
}

TEST(RegistrationTryLevel, CxxEAXCallBoundaryRequiresImmutableSourceStorage) {
  for (unsigned Mutation = 0; Mutation != 5; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Image = makeCxxEAXHelperImage(false, 0);
    // The source entry lives after the candidate, so a code-storage mutation
    // can affect only the witness and leave the exact helper protocol intact.
    auto &Text = Image.Segments.front();
    Text.Data[0] = 0xe8;
    writeLE<uint32_t>(Text.Data.data() + 1, 0x20 - 5);
    Text.Data[5] = 0xc3;
    Symbol Guess = Symbol::makeFunc(kText + 2, 0);
    Guess.IsBoundaryGuess = true;
    Image.Symbols.push_back(Guess);
    Segment Source;
    Source.VA = kText + 0x2000;
    Source.FileOff = 0x2000;
    Source.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
    Source.Data = {0xe8};
    emit32(Source.Data, kText - (Source.VA + 5));
    Source.Data.push_back(0xc3);
    Source.Size = Source.FileSz = Source.Data.size();
    Section Code;
    Code.VA = Source.VA;
    Code.FileOff = Source.FileOff;
    Code.Size = Code.FileSz = Source.Size;
    Code.Flags = Source.Flags;
    Image.Entry = Source.VA;
    if (Mutation == 0)
      Source.Flags = Source.Flags | SegmentFlags::Writable;
    if (Mutation == 1)
      Source.FileSz = 4;
    if (Mutation == 2)
      Image.BaseRelocations.push_back({Source.VA + 1, 3});
    if (Mutation == 3)
      Image.Segments.push_back(Source);
    if (Mutation == 4)
      ++Code.FileOff;
    Image.Segments.push_back(std::move(Source));
    Image.Sections.push_back(std::move(Code));
    Image.ExceptionMetadata = {};
    coff_loader::parseX86RegistrationExceptions(Image);
    const auto *EH = Image.ExceptionMetadata.findFunction(kText + 0x20);
    EXPECT_TRUE(!EH || EH->CodeRange.Begin != kText + 0x20);
  }
}

TEST(RegistrationTryLevel, CxxEAXCallBoundaryChecksIndirectCallAddressFixups) {
  for (unsigned Mutation = 0; Mutation != 6; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Image = makeCxxEAXHelperImage(false, 0);
    auto &Text = Image.Segments.front().Data;
    Text[0] = 0xff;
    Text[1] = Mutation == 5 ? 0x25 : 0x15;
    writeLE<uint32_t>(Text.data() + 2, kRData + 0x90);
    Text[6] = 0xe8;
    writeLE<uint32_t>(Text.data() + 7, 0x20 - 11);
    Text[11] = 0xc3;
    Image.Entry = kText;
    Symbol Guess = Symbol::makeFunc(kText + 8, 0);
    Guess.IsBoundaryGuess = true;
    Image.Symbols.push_back(Guess);
    Image.BaseRelocations.push_back({kText + 2, 3});
    if (Mutation == 1)
      Image.BaseRelocations.push_back({kText + 7, 3});
    if (Mutation == 2)
      Image.BaseRelocations.back().Type = 10;
    if (Mutation == 3)
      Image.BaseRelocations.push_back(Image.BaseRelocations.back());
    if (Mutation == 4)
      Image.BaseRelocations.back().Address = kText;
    Image.ExceptionMetadata = {};
    coff_loader::parseX86RegistrationExceptions(Image);
    const auto *EH = Image.ExceptionMetadata.findFunction(kText + 0x20);
    EXPECT_EQ(EH && EH->CodeRange.Begin == kText + 0x20, Mutation == 0);
  }
}

TEST(RegistrationTryLevel, CxxEAXHelperChecksFixupsAndSafeSEH) {
  for (unsigned Mutation = 0; Mutation != 6; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Image = makeCxxEAXHelperImage(false, 1);
    Image.BaseRelocations = {
        {kText + 0x23, 3}, {kText + 0x53, 3}, {kText + 0x118, 3}};
    if (Mutation == 1)
      Image.BaseRelocations.back().Type = 10;
    if (Mutation == 2)
      --Image.BaseRelocations.back().Address;
    if (Mutation == 3)
      Image.BaseRelocations.push_back(Image.BaseRelocations.back());
    if (Mutation == 4)
      Image.BaseRelocations.push_back({kText + 0x28, 3});
    auto &Data = Image.Segments[1];
    Data.Data.resize(0x14c, 0);
    Data.Size = Data.FileSz = Data.Data.size();
    Image.Sections[1].Size = Image.Sections[1].FileSz = Data.Size;
    writeLE<uint32_t>(Data.Data.data() + 0x100, 0x48);
    writeLE<uint32_t>(Data.Data.data() + 0x140, kRData + 0x148);
    writeLE<uint32_t>(Data.Data.data() + 0x144, 1);
    writeLE<uint32_t>(Data.Data.data() + 0x148,
                      kText + (Mutation == 5 ? 0x100 : 0x200) - kBase);
    Image.DynInfo.LoadConfigRVA = kRData + 0x100 - kBase;
    Image.DynInfo.LoadConfigSize = 0x48;
    Image.ExceptionMetadata = {};
    coff_loader::parseX86RegistrationExceptions(Image);
    EXPECT_EQ(chainAt(Image, kText + 0x20) != nullptr, Mutation == 0);
    EXPECT_EQ(chainAt(Image, kText + 0x50) != nullptr,
              Mutation == 0 || Mutation == 4);
  }
}

std::pair<BinaryImage, va_t> makeCxxRegisterStateImage(uint8_t Register,
                                                       bool WideDisplacement,
                                                       int32_t Level,
                                                       unsigned Mutation = 0) {
  auto Image = makeCxxContinuationImage();
  auto &Chain = *Image.ExceptionMetadata.Functions.front().Registration;
  const va_t Begin = Chain.TryLevelStores.front().StoreVA;
  std::vector<uint8_t> Body;
  Body.push_back(uint8_t(0xb8 | Register));
  emit32(Body, Mutation == 2 ? 7 : uint32_t(Level));
  if (Mutation == 1)
    std::fill(Body.begin(), Body.end(), 0x90);
  Body.push_back(0x90); // the value need not be adjacent to the state write
  if (Mutation == 3) {
    const uint8_t Condition = Register == 1 ? 2 : 1;
    Body.insert(Body.end(), {0x85, uint8_t(0xc0 | (Condition << 3) | Condition),
                             0x74, 0x01, uint8_t(0x40 | Register)});
  }
  const va_t Store = Begin + Body.size();
  Body.insert(Body.end(), {0x89, uint8_t((WideDisplacement ? 0x85 : 0x45) |
                                         (Register << 3))});
  if (WideDisplacement)
    emit32(Body, uint32_t(-4));
  else
    Body.push_back(0xfc);
  Body.push_back(0x90);
  const va_t Leave = Begin + Body.size();
  emitFrameStore(Body, -4, -1);
  Body.push_back(0xe9);
  emit32(Body, uint32_t(kText + 0x80 - (Begin + Body.size() + 4)));
  auto &Text = Image.Segments.front().Data;
  std::copy(Body.begin(), Body.end(), Text.begin() + (Begin - kText));
  Chain.TryLevelStores.erase(Chain.TryLevelStores.begin(),
                             Chain.TryLevelStores.begin() + 2);
  Chain.TryLevelStores.insert(Chain.TryLevelStores.begin(),
                              {Leave, Leave + 7, -1});
  return {std::move(Image), Store};
}

TEST(RegistrationTryLevel, CxxRegisterStatesUseTheCompleteReachingValue) {
  for (uint8_t Register : {0, 1, 2, 3, 6, 7})
    for (bool Wide : {false, true})
      for (int32_t Level : {-1, 0, 1}) {
        SCOPED_TRACE(::testing::Message()
                     << unsigned(Register) << " " << Wide << " " << Level);
        auto [Image, Store] = makeCxxRegisterStateImage(Register, Wide, Level);
        const auto Function = liftEntry(Image, kText);
        ASSERT_TRUE(Function.hasCompleteLiftCoverage());
        ASSERT_TRUE(Function.RegistrationStates);
        const auto &States = *Function.RegistrationStates;
        EXPECT_TRUE(States.Complete);
        EXPECT_TRUE(States.RegistrationLifetimeComplete);
        const va_t End = Store + (Wide ? 6 : 3);
        const auto State =
            std::find_if(States.Blocks.begin(), States.Blocks.end(),
                         [&](const auto &S) { return S.Range.Begin == End; });
        ASSERT_NE(State, States.Blocks.end());
        EXPECT_FALSE(State->Unknown);
        EXPECT_EQ(State->levels(), (std::vector<int32_t>{Level}));
      }
}

TEST(RegistrationTryLevel, CxxRegisterStatesRejectUnknownAndConflictingValues) {
  for (unsigned Mutation : {1, 2, 3}) {
    SCOPED_TRACE(Mutation);
    auto [Image, Store] = makeCxxRegisterStateImage(3, false, 0, Mutation);
    const auto Function = liftEntry(Image, kText);
    ASSERT_TRUE(Function.hasCompleteLiftCoverage());
    ASSERT_TRUE(Function.RegistrationStates);
    EXPECT_FALSE(Function.RegistrationStates->Complete);
    EXPECT_FALSE(Function.RegistrationStates->RegistrationLifetimeComplete);
  }
}

TEST(RegistrationTryLevel, CxxRegisterStatesDoNotRequireByteObservations) {
  auto [Image, Store] = makeCxxRegisterStateImage(3, false, 0);
  auto &Chain = *Image.ExceptionMetadata.Functions.front().Registration;
  auto &Text = Image.Segments.front().Data;
  // Every exit stores -1 through EAX: xor eax,eax; dec eax; mov [ebp-4],eax.
  // There is neither an immediate store nor an adjacent zero-clear hint.
  const std::vector<uint8_t> Leave = {0x31, 0xc0, 0x48, 0x89, 0x45, 0xfc, 0x90};
  for (const auto &Observation : Chain.TryLevelStores) {
    ASSERT_EQ(Observation.Level, -1);
    ASSERT_EQ(Observation.EndVA - Observation.StoreVA, Leave.size());
    std::copy(Leave.begin(), Leave.end(),
              Text.begin() + (Observation.StoreVA - kText));
  }
  Chain.TryLevelStores.clear();
  const auto Function = liftEntry(Image, kText);
  ASSERT_TRUE(Function.hasCompleteLiftCoverage());
  ASSERT_TRUE(Function.RegistrationStates);
  EXPECT_TRUE(Function.RegistrationStates->Complete);
  EXPECT_TRUE(Function.RegistrationStates->RegistrationLifetimeComplete);
  const auto &States = Function.RegistrationStates->Blocks;
  const auto State =
      std::find_if(States.begin(), States.end(),
                   [&](const auto &S) { return S.Range.Begin == Store + 3; });
  ASSERT_NE(State, States.end());
  EXPECT_FALSE(State->Unknown);
  EXPECT_EQ(State->levels(), (std::vector<int32_t>{0}));
}

TEST(RegistrationTryLevel, CxxRegisterStatesRequireTheirDecodedInstruction) {
  auto [Image, Store] = makeCxxRegisterStateImage(3, false, 0);
  const auto Original = liftEntry(Image, kText);
  ASSERT_TRUE(Original.RegistrationStates);
  ASSERT_TRUE(Original.RegistrationStates->Complete);
  for (unsigned Mutation = 0; Mutation != 9; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Function = Original;
    for (auto &Block : Function.Blocks)
      for (auto &Boundary : Block.InstructionBoundaries)
        if (Boundary.Address == Store) {
          if (Mutation == 0)
            --Boundary.Size;
          if (Mutation == 1)
            ++Boundary.FirstOp;
          if (Mutation == 2)
            Boundary.OpCount = 0;
          if (Mutation == 3)
            Boundary.Control = LowInstructionControl::Call;
          if (Mutation == 4)
            Boundary.ControlFlags |=
                LowInstructionControlFlag::InstructionGuard;
          if (Mutation == 5)
            ++Block.EndAddr;
          if (Mutation == 6)
            Boundary.Mode = InstructionMode::Thumb;
          if (Mutation == 7)
            Boundary.TargetMode = LowInstructionTargetMode::Thumb;
          if (Mutation == 8)
            Boundary.Immediate = kText;
        }
    EXPECT_FALSE(analyzeRegistrationStates(Function).Complete);
  }
}

std::pair<BinaryImage, std::vector<va_t>>
makeCxxBitwiseStateImage(bool WideDisplacement, bool WideImmediate,
                         int32_t Level, bool JoinStates = false) {
  auto Image = makeCxxContinuationImage();
  auto &Chain = *Image.ExceptionMetadata.Functions.front().Registration;
  const va_t Begin = Chain.TryLevelStores.front().StoreVA;
  std::vector<uint8_t> Body;
  std::vector<va_t> Ends;
  if (JoinStates)
    Body = {0x85, 0xc9, 0x74, 5, 0x31, 0xc0, 0x89, 0x45, 0xfc};
  for (bool Enter : {true, false}) {
    Body.push_back(WideImmediate ? 0x81 : 0x83);
    Body.push_back(
        uint8_t((WideDisplacement ? 0x85 : 0x45) | ((Enter ? 4 : 1) << 3)));
    if (WideDisplacement)
      emit32(Body, uint32_t(-4));
    else
      Body.push_back(0xfc);
    if (WideImmediate)
      emit32(Body, Enter ? uint32_t(Level) : UINT32_MAX);
    else
      Body.push_back(Enter ? uint8_t(Level) : 0xff);
    Ends.push_back(Begin + Body.size());
    Body.push_back(0x90);
  }
  Body.push_back(0xe9);
  emit32(Body, uint32_t(kText + 0x80 - (Begin + Body.size() + 4)));
  std::copy(Body.begin(), Body.end(),
            Image.Segments.front().Data.begin() + (Begin - kText));
  Chain.TryLevelStores.erase(Chain.TryLevelStores.begin(),
                             Chain.TryLevelStores.begin() + 2);
  return {std::move(Image), std::move(Ends)};
}

TEST(RegistrationTryLevel, CxxBitwiseStatesRetireAtTheDecodedWrite) {
  for (bool WideDisplacement : {false, true})
    for (bool WideImmediate : {false, true})
      for (int32_t Level : {0, 1}) {
        SCOPED_TRACE(::testing::Message() << WideDisplacement << ' '
                                          << WideImmediate << ' ' << Level);
        auto [Image, Ends] =
            makeCxxBitwiseStateImage(WideDisplacement, WideImmediate, Level);
        const auto Function = liftEntry(Image, kText);
        ASSERT_TRUE(Function.hasCompleteLiftCoverage());
        ASSERT_TRUE(Function.RegistrationStates);
        const auto &States = *Function.RegistrationStates;
        EXPECT_TRUE(States.Complete);
        EXPECT_TRUE(States.RegistrationLifetimeComplete);
        for (size_t I = 0; I != Ends.size(); ++I) {
          const auto State = std::find_if(
              States.Blocks.begin(), States.Blocks.end(),
              [&](const auto &S) { return S.Range.Begin == Ends[I]; });
          ASSERT_NE(State, States.Blocks.end());
          EXPECT_FALSE(State->Unknown);
          EXPECT_EQ(State->levels(), (std::vector<int32_t>{I ? -1 : Level}));
        }
      }
}

TEST(RegistrationTryLevel, CxxBitwiseStatesRepairAJoinedValue) {
  auto [Image, Ends] = makeCxxBitwiseStateImage(false, false, 0, true);
  const auto Function = liftEntry(Image, kText);
  ASSERT_TRUE(Function.hasCompleteLiftCoverage());
  ASSERT_TRUE(Function.RegistrationStates);
  const auto &States = *Function.RegistrationStates;
  EXPECT_TRUE(States.Complete);
  EXPECT_TRUE(States.RegistrationLifetimeComplete);
  const auto Before =
      std::find_if(States.Blocks.begin(), States.Blocks.end(),
                   [&](const auto &S) { return S.Range.Begin == Ends[0] - 4; });
  ASSERT_NE(Before, States.Blocks.end());
  EXPECT_FALSE(Before->Unknown);
  EXPECT_EQ(Before->levels(), (std::vector<int32_t>{-1, 0}));
  const auto After =
      std::find_if(States.Blocks.begin(), States.Blocks.end(),
                   [&](const auto &S) { return S.Range.Begin == Ends[0]; });
  ASSERT_NE(After, States.Blocks.end());
  EXPECT_FALSE(After->Unknown);
  EXPECT_EQ(After->levels(), (std::vector<int32_t>{0}));
}

TEST(RegistrationTryLevel, CxxBitwiseStatesFollowTheCheckedSharedPrologue) {
  for (bool Wide : {false, true})
    for (unsigned Variant : {0, 1, 2}) {
      SCOPED_TRACE(::testing::Message() << Wide << ' ' << Variant);
      auto Image = makeCxxEAXStateImage(Wide, Variant);
      auto &Text = Image.Segments.front().Data;
      const size_t Body = 0x20 + (Wide ? 15 : 12);
      const uint8_t Updates[] = {0x83, 0x65, 0xfc, 0,    0x90, 0x90, 0x90,
                                 0x83, 0x4d, 0xfc, 0xff, 0x90, 0x90, 0x90};
      std::copy(std::begin(Updates), std::end(Updates), Text.begin() + Body);
      Image.ExceptionMetadata = {};
      coff_loader::parseX86RegistrationExceptions(Image);
      const auto *EH = Image.ExceptionMetadata.findFunction(kText + 0x20);
      ASSERT_NE(EH, nullptr);
      ASSERT_TRUE(EH->Registration);
      ASSERT_TRUE(EH->Registration->TryLevelStores.empty());
      const auto Function = liftEntry(Image, kText + 0x20);
      ASSERT_TRUE(Function.hasCompleteLiftCoverage());
      ASSERT_TRUE(Function.RegistrationStates);
      const auto &States = *Function.RegistrationStates;
      EXPECT_TRUE(States.Complete);
      EXPECT_TRUE(States.RegistrationLifetimeComplete);
      EXPECT_TRUE(States.ChainOperationsComplete);
      EXPECT_TRUE(States.CxxPrologue);
      for (size_t Offset : {size_t(4), size_t(11)}) {
        const auto State = std::find_if(
            States.Blocks.begin(), States.Blocks.end(), [&](const auto &S) {
              return S.Range.Begin == kText + Body + Offset;
            });
        ASSERT_NE(State, States.Blocks.end());
        EXPECT_FALSE(State->Unknown);
        EXPECT_EQ(State->levels(),
                  (std::vector<int32_t>{Offset == 4 ? 0 : -1}));
      }
    }
}

TEST(RegistrationTryLevel, CxxBitwiseStatesRequireTheirExactMemoryOperation) {
  auto [Image, Ends] = makeCxxBitwiseStateImage(false, false, 0);
  const auto Original = liftEntry(Image, kText);
  ASSERT_TRUE(Original.RegistrationStates);
  ASSERT_TRUE(Original.RegistrationStates->Complete);
  const va_t Address = Ends.front() - 4;
  for (unsigned Mutation = 0; Mutation != 22; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Function = Original;
    bool Changed = false;
    NdVar Loaded;
    for (auto &Block : Function.Blocks) {
      for (auto &Boundary : Block.InstructionBoundaries)
        if (Boundary.Address == Address && Mutation < 9) {
          Changed = true;
          switch (Mutation) {
          case 0:
            --Boundary.Size;
            break;
          case 1:
            ++Boundary.FirstOp;
            break;
          case 2:
            --Boundary.OpCount;
            break;
          case 3:
            Boundary.Control = LowInstructionControl::Call;
            break;
          case 4:
            Boundary.ControlFlags = LowInstructionControlFlag::InstructionGuard;
            break;
          case 5:
            ++Block.EndAddr;
            break;
          case 6:
            Boundary.Mode = InstructionMode::Thumb;
            break;
          case 7:
            Boundary.TargetMode = LowInstructionTargetMode::Thumb;
            break;
          case 8:
            Boundary.Immediate = kText;
            break;
          }
        }
      for (auto &Op : Block.Ops) {
        if (Op.Addr != Address || Mutation < 9)
          continue;
        if (Op.Opcode == NdOp::LOAD) {
          Loaded = Op.Output;
          if (Mutation == 9) {
            Op.MemoryAddressSpace = NdMemoryAddressSpace::X86FS;
            Changed = true;
          }
          if (Mutation == 14) {
            Op.Inputs[0] = NdVar::cst(kRData, 8);
            Changed = true;
          }
          if (Mutation == 16) {
            Op.Output.Size = 2;
            Changed = true;
          }
        }
        if (Op.Opcode == NdOp::INT_AND && Op.Output.Size == 4) {
          if (Mutation == 11 || Mutation == 12) {
            Op.Inputs[1] =
                Mutation == 11 ? NdVar::reg(x86reg::RDX, 4) : NdVar::cst(7, 4);
            Changed = true;
          }
          if (Mutation == 17) {
            Op.MemoryOrdering = NdMemoryOrdering::Relaxed;
            Changed = true;
          }
          if (Mutation == 20) {
            Op.Opcode = NdOp::INT_XOR;
            Changed = true;
          }
        }
        if (Op.Output == NdVar::reg(x86reg::ZF, 1) && Mutation == 15) {
          Op.Output = NdVar::reg(x86reg::RBP, 4);
          Changed = true;
        }
        if (Op.Opcode == NdOp::STORE) {
          if (Mutation == 10) {
            Op.MemoryOrdering = NdMemoryOrdering::SequentiallyConsistent;
            Changed = true;
          }
          if (Mutation == 13) {
            Op.Inputs[1] = Loaded;
            Changed = true;
          }
          if (Mutation == 18) {
            Op.Seq = -1;
            Changed = true;
          }
          if (Mutation == 19) {
            ++Op.Addr;
            Changed = true;
          }
          if (Mutation == 21) {
            Op.Inputs[0].Size = 1;
            Changed = true;
          }
        }
      }
    }
    ASSERT_TRUE(Changed);
    EXPECT_FALSE(analyzeRegistrationStates(Function).Complete);
  }
}

std::pair<BinaryImage, va_t>
makeCxxSharedEpilogueImage(bool Wide, unsigned Variant, uint32_t Allocation,
                           bool PopArguments = false) {
  auto Image = makeCxxEAXStateImage(Wide, Variant, Allocation);
  constexpr va_t Epilogue = kText + 0x180;
  auto &Text = Image.Segments.front().Data;
  const size_t Begin = 0x20 + (Wide ? 15 : 12);
  // Exercise actual restores: the body destroys all three saved GPRs and
  // leaves a distinct scalar in EAX before calling the shared epilogue.
  std::vector<uint8_t> Body = {0x83, 0x65, 0xfc, 0,    0x83, 0x4d, 0xfc, 0xff,
                               0x31, 0xdb, 0x31, 0xf6, 0x31, 0xff, 0xb8};
  emit32(Body, 0x11223344);
  const va_t Call = kText + Begin + Body.size();
  Body.push_back(0xe8);
  emit32(Body, uint32_t(Epilogue - Call - 5));
  if (PopArguments)
    Body.insert(Body.end(), {0xc2, 8, 0});
  else
    Body.push_back(0xc3);
  std::fill(Text.begin() + Begin, Text.begin() + 0x50, 0xcc);
  std::copy(Body.begin(), Body.end(), Text.begin() + Begin);
  const uint8_t Helper[] = {0x8b, 0x4d, 0xf4, 0x64, 0x89, 0x0d, 0,
                            0,    0,    0,    0x59, 0x5f, 0x5f, 0x5e,
                            0x5b, 0x8b, 0xe5, 0x5d, 0x51, 0xc3};
  std::copy(std::begin(Helper), std::end(Helper), Text.begin() + 0x180);
  Image.ExceptionMetadata = {};
  coff_loader::parseX86RegistrationExceptions(Image);
  return {std::move(Image), Call};
}

TEST(RegistrationTryLevel, CxxSharedEpilogueRestoresTheProvedParentFrame) {
  for (bool Wide : {false, true})
    for (unsigned Variant : {0, 1, 2})
      for (uint32_t Allocation : {0, 1, 4, 32})
        for (bool PopArguments : {false, true}) {
          if (Variant && Allocation < 4)
            continue;
          SCOPED_TRACE(::testing::Message()
                       << Wide << ' ' << Variant << ' ' << Allocation << ' '
                       << PopArguments);
          auto [Image, Call] = makeCxxSharedEpilogueImage(
              Wide, Variant, Allocation, PopArguments);
          const auto Function = liftEntry(Image, kText + 0x20);
          ASSERT_TRUE(Function.hasCompleteLiftCoverage());
          ASSERT_TRUE(Function.RegistrationStates);
          const auto &States = *Function.RegistrationStates;
          EXPECT_TRUE(States.Complete);
          EXPECT_TRUE(States.RegistrationLifetimeComplete);
          EXPECT_TRUE(States.ChainOperationsComplete);
          EXPECT_TRUE(States.CallFrameEffectsComplete);
          EXPECT_TRUE(States.CxxPrologue);
          EXPECT_TRUE(States.CallFrameEffects.empty());
          EXPECT_FALSE(cxxRegistrationFrameCoordinate(
              *Function.ExceptionMetadata, &States));
          const auto Exit = std::find_if(
              States.Blocks.begin(), States.Blocks.end(),
              [&](const auto &S) { return S.Range.Begin == Call + 5; });
          ASSERT_NE(Exit, States.Blocks.end());
          EXPECT_TRUE(Exit->Reached);
          EXPECT_FALSE(Exit->Unknown);
          EXPECT_TRUE(Exit->levels().empty());
          EXPECT_TRUE(
              std::any_of(States.ChainAccesses.begin(),
                          States.ChainAccesses.end(), [&](const auto &Access) {
                            return Access.Address == Call &&
                                   Access.EndAddress == Call + 5 &&
                                   Access.AccessKind ==
                                       RegistrationChainAccess::Kind::Remove;
                          }));
        }
}

TEST(RegistrationTryLevel,
     CxxSharedEpilogueProjectsThePhysicalReturnBeforeSSA) {
  for (bool Wide : {false, true})
    for (unsigned Variant : {0, 1, 2})
      for (uint32_t Allocation : {0, 1, 4, 32})
        for (bool PopArguments : {false, true}) {
          if (Variant && Allocation < 4)
            continue;
          SCOPED_TRACE(::testing::Message()
                       << Wide << ' ' << Variant << ' ' << Allocation << ' '
                       << PopArguments);
          auto [Image, Call] = makeCxxSharedEpilogueImage(
              Wide, Variant, Allocation, PopArguments);
          const auto Low = liftEntry(Image, kText + 0x20);
          ASSERT_TRUE(Low.RegistrationStates);
          ASSERT_TRUE(Low.RegistrationStates->ChainOperationsComplete);
          LowToMedConverter Converter;
          Converter.setBinaryImage(&Image);
          const auto Med =
              Converter.convert(Low, Arch::X86, BinaryFormat::COFF);
          ASSERT_TRUE(verifyMedFunc(Med, "shared-cxx-physical-return"));
          unsigned Returns = 0, StackRestores = 0, ChainRemovals = 0;
          for (const auto &Block : Med.Blocks)
            for (const auto &Op : Block.Ops) {
              EXPECT_FALSE(Op.Addr == Call && Op.Opcode == NdOp::CALL);
              if (Op.Opcode == NdOp::RETURN) {
                ++Returns;
                ASSERT_EQ(Op.NumInputs, 1u);
                EXPECT_TRUE(Op.Inputs[0].isConst());
                EXPECT_EQ(Op.Inputs[0].ConstVal, 0x11223344u);
              }
              if (Op.Addr == Call && Op.Output.Kind == MedVar::Reg &&
                  Op.Output.RegOff == x86reg::RSP && Op.Output.Size == 4 &&
                  entryStackOffset(Med, Op.Output, Arch::X86,
                                   BinaryFormat::COFF) == 0)
                ++StackRestores;
              if (Op.Addr == Call && Op.Opcode == NdOp::STORE &&
                  Op.MemoryAddressSpace == NdMemoryAddressSpace::X86FS)
                ++ChainRemovals;
            }
          EXPECT_EQ(Returns, 1u);
          EXPECT_NE(StackRestores, 0u);
          EXPECT_EQ(ChainRemovals, 1u);
          EXPECT_FALSE(cxxRegistrationFrameCoordinate(
              *Med.ExceptionMetadata, &*Med.RegistrationStates));
        }
}

TEST(RegistrationTryLevel, CxxSharedHelpersHaveTheCheckedParentEntryABI) {
  for (bool Wide : {false, true})
    for (unsigned Variant : {0, 1, 2})
      for (uint32_t Allocation : {0, 1, 4, 32})
        for (bool PopArguments : {false, true}) {
          if (Variant && Allocation < 4)
            continue;
          SCOPED_TRACE(::testing::Message()
                       << Wide << ' ' << Variant << ' ' << Allocation << ' '
                       << PopArguments);
          auto [Image, Call] = makeCxxSharedEpilogueImage(
              Wide, Variant, Allocation, PopArguments);
          const auto Low = liftEntry(Image, kText + 0x20);
          ASSERT_TRUE(Low.RegistrationStates);
          ASSERT_TRUE(Low.RegistrationStates->CallFrameEffectsComplete);
          const auto ABI = getCheckedX86RegistrationCxxParentABI(Low, Image);
          ASSERT_TRUE(ABI);
          EXPECT_EQ(*ABI, PopArguments ? 8u : 0u);
          EXPECT_EQ(hasCallerCleanupRegistrationABI(Low, Image), !PopArguments);
          LowToMedConverter Converter;
          Converter.setBinaryImage(&Image);
          const auto Med =
              Converter.convert(Low, Arch::X86, BinaryFormat::COFF);
          EXPECT_EQ(Med.RegistrationCxxEntryPopBytes, ABI);
          EXPECT_FALSE(cxxRegistrationFrameCoordinate(
              *Med.ExceptionMetadata, &*Med.RegistrationStates));
        }
}

TEST(RegistrationTryLevel, CxxSharedHelpersBindTheirPhysicalFrame) {
  for (bool Wide : {false, true})
    for (unsigned Variant : {0, 1, 2})
      for (uint32_t Allocation : {0, 1, 4, 32})
        for (bool PopArguments : {false, true}) {
          if (Variant && Allocation < 4)
            continue;
          SCOPED_TRACE(::testing::Message()
                       << Wide << ' ' << Variant << ' ' << Allocation << ' '
                       << PopArguments);
          auto [Image, Call] = makeCxxSharedEpilogueImage(
              Wide, Variant, Allocation, PopArguments);
          const auto Low = liftEntry(Image, kText + 0x20);
          ASSERT_TRUE(Low.RegistrationStates);
          ASSERT_TRUE(Low.RegistrationStates->CxxPrologue);
          LowToMedConverter Converter;
          Converter.setBinaryImage(&Image);
          const auto Med =
              Converter.convert(Low, Arch::X86, BinaryFormat::COFF);
          ASSERT_TRUE(Med.RegistrationPhysicalFrame);
          EXPECT_EQ(Med.RegistrationStates->CxxPrologue,
                    Low.RegistrationStates->CxxPrologue);
          EXPECT_FALSE(cxxRegistrationFrameCoordinate(
              *Low.ExceptionMetadata, &*Low.RegistrationStates));
          const auto Coordinate = cxxRegistrationFrameCoordinate(Med);
          ASSERT_TRUE(Coordinate);
          EXPECT_EQ(Coordinate->EntryOffset, -4);
          EXPECT_EQ(Coordinate->Alignment, 1u);
          EXPECT_EQ(Coordinate->AlignedOffset, 0);
          ASSERT_TRUE(hasCheckedX86RegistrationCxxPhysicalFrame(Image, Med));
          const auto *Chain = getX86RegistrationCxxChainAccesses(Med);
          ASSERT_TRUE(Chain);
          EXPECT_EQ(Chain->size(),
                    Low.RegistrationStates->ChainAccesses.size() + 1);
        }
}

TEST(RegistrationTryLevel, CxxPhysicalFrameRequiresEveryCurrentChainEffect) {
  auto [Original, Call] = makeCxxSharedEpilogueImage(false, 1, 32);
  const auto Low = liftEntry(Original, kText + 0x20);
  LowToMedConverter Converter;
  Converter.setBinaryImage(&Original);
  const auto Proven = Converter.convert(Low, Arch::X86, BinaryFormat::COFF);
  ASSERT_TRUE(Proven.RegistrationPhysicalFrame);
  ASSERT_TRUE(hasCheckedX86RegistrationCxxPhysicalFrame(Original, Proven));
  for (unsigned Mutation = 0; Mutation != 22; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Image = Original;
    auto Med = Proven;
    auto Frame = std::make_shared<X86RegistrationCxxPhysicalFrame>(
        *Med.RegistrationPhysicalFrame);
    Med.RegistrationPhysicalFrame = Frame;
    MedOp *Chain = nullptr;
    for (auto &Block : Med.Blocks)
      for (auto &Op : Block.Ops)
        if (Op.Opcode == NdOp::STORE &&
            Op.MemoryAddressSpace == NdMemoryAddressSpace::X86FS &&
            Op.Addr == Call)
          Chain = &Op;
    ASSERT_TRUE(Chain);
    switch (Mutation) {
    case 0:
      Med.RegistrationPhysicalFrame.reset();
      break;
    case 1:
      ++Frame->Entry.OpSeq;
      break;
    case 2:
      Frame->ChainAccesses.pop_back();
      break;
    case 3: {
      const auto Duplicate = Frame->ChainAccesses.front();
      Frame->ChainAccesses.push_back(Duplicate);
      break;
    }
    case 4:
      Frame->ChainAccesses.front().AccessKind =
          RegistrationChainAccess::Kind::Remove;
      break;
    case 5:
      ++Frame->ChainAccesses.back().OpSeq;
      break;
    case 6:
      ++Frame->ChainAccesses.back().EndAddress;
      break;
    case 7:
      Frame->Exits.clear();
      break;
    case 8:
      ++Frame->Exits.front().Source.OpSeq;
      break;
    case 9:
      ++Frame->Exits.front().HelperVA;
      break;
    case 10:
      ++Frame->EntryCall.SecurityCookieVA;
      break;
    case 11:
      ++Frame->EntryCall.HandlerVA;
      break;
    case 12:
      ++Chain->OriginSeq;
      break;
    case 13:
      Chain->Inputs[0].ConstVal = 4;
      break;
    case 14:
      Chain->Inputs[1].Size = 2;
      break;
    case 15:
      Chain->Dead = true;
      break;
    case 16:
      Med.RegistrationStates->Complete = false;
      break;
    case 17:
      Med.RegistrationStates->ChainAccesses.clear();
      break;
    case 18:
      Image.Segments.front().Data[0x180 + 12] ^= 1;
      break;
    case 19:
      Image.DynInfo.SecurityCookieRVA += 4;
      writeLE<uint32_t>(Image.Segments.front().Data.data() + 0x118,
                        kRData + 0xf4);
      ASSERT_TRUE(coff_loader::getCheckedX86CxxPrologueCall(
          Image, *Med.ExceptionMetadata));
      break;
    case 20:
      // Both helper variants are valid and have the same extent and final SP,
      // but one saves ESP while the other saves the encoded cookie at EBP-16.
      Image.Segments.front().Data[0x120] = 0x45;
      ASSERT_TRUE(coff_loader::getCheckedX86CxxPrologueCall(
          Image, *Med.ExceptionMetadata));
      break;
    case 21:
      Frame->EntryCall.Writes.pop_back();
      break;
    }
    EXPECT_FALSE(hasCheckedX86RegistrationCxxPhysicalFrame(Image, Med));
  }
}

TEST(RegistrationTryLevel, CxxPhysicalChainOffsetRequiresUniqueZeroTransport) {
  auto [Image, Call] = makeCxxSharedEpilogueImage(false, 1, 32);
  LowToMedConverter Converter;
  Converter.setBinaryImage(&Image);
  const auto Proven = Converter.convert(liftEntry(Image, kText + 0x20),
                                        Arch::X86, BinaryFormat::COFF);
  ASSERT_TRUE(hasCheckedX86RegistrationCxxPhysicalFrame(Image, Proven));
  int Next = 0;
  for (const auto &Block : Proven.Blocks)
    for (const auto &Op : Block.Ops)
      Next = std::max(Next, Op.Output.Id + 1);
  for (unsigned Mutation = 0; Mutation != 13; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Med = Proven;
    auto Block =
        std::find_if(Med.Blocks.begin(), Med.Blocks.end(), [&](const auto &B) {
          return B.StartAddr <= Call && Call < B.EndAddr;
        });
    ASSERT_NE(Block, Med.Blocks.end());
    auto Access =
        std::find_if(Block->Ops.begin(), Block->Ops.end(), [](const auto &Op) {
          return Op.MemoryAddressSpace == NdMemoryAddressSpace::X86FS;
        });
    ASSERT_NE(Access, Block->Ops.end());
    const size_t Index = Access - Block->Ops.begin();
    ASSERT_GE(Access->OriginSeq, 2);
    MedOp Copy;
    Copy.Opcode = NdOp::COPY;
    Copy.Addr = Call;
    Copy.OriginSeq = Access->OriginSeq - 2;
    Copy.Output.Kind = MedVar::Temp;
    Copy.Output.Id = Next;
    Copy.Output.SSAVer = 1;
    Copy.Output.Size = 4;
    Copy.addInput(MedVar::makeConst(0, 4));
    auto Extend = Copy;
    Extend.Opcode = NdOp::INT_ZEXT;
    Extend.OriginSeq += 1;
    Extend.Output.Id += 1;
    Extend.Output.Size = 8;
    Extend.Inputs[0] = Copy.Output;
    Access->Inputs[0] = Extend.Output;
    switch (Mutation) {
    case 0:
      break;
    case 1:
      Copy.Inputs[0].ConstVal = 4;
      break;
    case 2:
      Extend.Inputs[0] = Extend.Output;
      break;
    case 3:
      Extend.Dead = true;
      break;
    case 4:
      Copy.MemoryAddressSpace = NdMemoryAddressSpace::X86FS;
      break;
    case 5:
      ++Copy.Addr;
      break;
    case 6:
      Copy.OriginSeq = Access->OriginSeq;
      break;
    case 7:
      Copy.Output.Size = 2;
      break;
    case 8:
      Copy.addInput(MedVar::makeConst(0, 4));
      break;
    case 9:
      Extend.Opcode = NdOp::INT_SEXT;
      break;
    case 10:
      // A second definition cannot authenticate this value, even when equal.
      Med.Blocks.front().Ops.push_back(Copy);
      break;
    case 11:
      Copy.Output.Id += 2;
      break;
    case 12:
      std::swap(Copy, Extend);
      break;
    }
    Block->Ops.insert(Block->Ops.begin() + Index, {Copy, Extend});
    EXPECT_EQ(hasCheckedX86RegistrationCxxPhysicalFrame(Image, Med),
              Mutation == 0);
  }
}

TEST(RegistrationTryLevel, CxxSharedEpilogueProjectionRequiresItsCurrentCall) {
  auto [Original, Call] = makeCxxSharedEpilogueImage(false, 2, 32);
  const auto Proven = liftEntry(Original, kText + 0x20);
  ASSERT_TRUE(Proven.RegistrationStates);
  ASSERT_TRUE(Proven.RegistrationStates->ChainOperationsComplete);
  for (unsigned Mutation = 0; Mutation != 24; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Image = Original;
    auto Low = Proven;
    auto &Block =
        *std::find_if(Low.Blocks.begin(), Low.Blocks.end(),
                      [&](const auto &B) { return B.EndAddr == Call + 5; });
    auto &Boundary = Block.InstructionBoundaries.back();
    auto &Accesses = Low.RegistrationStates->ChainAccesses;
    auto Access =
        std::find_if(Accesses.begin(), Accesses.end(), [&](const auto &A) {
          return A.Address == Call &&
                 A.AccessKind == RegistrationChainAccess::Kind::Remove;
        });
    ASSERT_NE(Access, Accesses.end());
    switch (Mutation) {
    case 0:
      Accesses.erase(Access);
      break;
    case 1:
      ++Access->OpSeq;
      break;
    case 2:
      ++Access->EndAddress;
      break;
    case 3:
      ++Access->Address;
      break;
    case 4: {
      const auto Duplicate = *Access;
      Accesses.push_back(Duplicate);
      break;
    }
    case 5:
      Access->AccessKind = RegistrationChainAccess::Kind::ReadInstalledHead;
      break;
    case 6:
      Image.Segments.front().Data[0x180 + 12] ^= 1;
      break;
    case 7:
      Image.Segments.front().Data[Call - kText] = 0xe9;
      break;
    case 8:
      Image.BaseRelocations.push_back({kText + 0x181, 3});
      break;
    case 9:
      Image.Segments.front().Flags =
          Image.Segments.front().Flags | SegmentFlags::Writable;
      break;
    case 10:
      --Boundary.FirstOp;
      break;
    case 11:
      ++Boundary.OpCount;
      break;
    case 12:
      --Boundary.Size;
      break;
    case 13:
      Boundary.Mode = InstructionMode::Thumb;
      break;
    case 14:
      Boundary.TargetMode = LowInstructionTargetMode::ARM;
      break;
    case 15:
      Boundary.ControlFlags |= LowInstructionControlFlag::NoReturn;
      break;
    case 16:
      Block.Ops.back().Output = NdVar::reg(x86reg::RDX, 4);
      break;
    case 17:
      ++Block.Ops.back().Inputs[0].Offset;
      break;
    case 18:
      Low.OrdinaryModuleAnalysisRoots.insert(Call + 1);
      break;
    case 19:
      ++Block.EndAddr;
      break;
    case 20:
      Low.RegistrationStates->CxxPrologue.reset();
      break;
    case 21:
      Low.RegistrationStates->ChainOperationsComplete = false;
      break;
    case 22: {
      auto Extra = *Access;
      Extra.AccessKind = RegistrationChainAccess::Kind::ReadInstalledHead;
      Accesses.push_back(Extra);
      break;
    }
    case 23:
      Boundary.Control = LowInstructionControl::Branch;
      break;
    }
    EXPECT_FALSE(getCheckedX86RegistrationCxxParentABI(Low, Image));
    LowToMedConverter Converter;
    Converter.setBinaryImage(&Image);
    const auto Med = Converter.convert(Low, Arch::X86, BinaryFormat::COFF);
    EXPECT_TRUE(
        std::any_of(Med.Blocks.begin(), Med.Blocks.end(), [&](const auto &B) {
          return std::any_of(B.Ops.begin(), B.Ops.end(), [&](const auto &Op) {
            return Op.Addr == Call && Op.Opcode == NdOp::CALL;
          });
        }));
  }
}

TEST(RegistrationTryLevel, CxxSharedEpilogueCannotRestoreOverwrittenSaves) {
  for (bool Wide : {false, true})
    for (unsigned Variant : {1, 2})
      for (uint32_t Allocation : {0, 1}) {
        SCOPED_TRACE(::testing::Message()
                     << Wide << ' ' << Variant << ' ' << Allocation);
        auto [Image, Call] =
            makeCxxSharedEpilogueImage(Wide, Variant, Allocation);
        const auto Function = liftEntry(Image, kText + 0x20);
        ASSERT_TRUE(Function.hasCompleteLiftCoverage());
        ASSERT_TRUE(Function.RegistrationStates);
        EXPECT_FALSE(Function.RegistrationStates->RegistrationLifetimeComplete);
        EXPECT_FALSE(Function.RegistrationStates->ChainOperationsComplete);
      }
}

TEST(RegistrationTryLevel, CxxSharedEntryOwnsOnlyDisprovedPaddingBoundaries) {
  for (bool Wide : {false, true})
    for (unsigned Mutation = 0; Mutation != 7; ++Mutation) {
      SCOPED_TRACE(::testing::Message() << Wide << ' ' << Mutation);
      auto [Image, Call] = makeCxxSharedEpilogueImage(Wide, 0, 32);
      const va_t Body = Call - 19, Boundary = Body + 1;
      auto Guess = Symbol::makeFunc(Boundary);
      Guess.IsBoundaryGuess = true;
      if (Mutation == 1)
        Guess.Name = "stated_entry";
      Image.Symbols.push_back(Guess);
      if (Mutation == 2) {
        Export Other;
        Other.Addr = Boundary;
        Image.Exports.push_back(Other);
      }
      if (Mutation == 3)
        Image.Symbols.push_back(Symbol::makeFunc(Boundary));
      auto &Text = Image.Segments.front().Data;
      if (Mutation == 4) {
        Text[Body - kText] = 0xeb;
        Text[Body - kText + 1] = 0xff; // A branch explicitly enters the byte.
      }
      if (Mutation == 5) {
        Text[Body - kText] = 0xe8;
        writeLE<uint32_t>(Text.data() + Body - kText + 1, uint32_t(-4));
      }
      if (Mutation == 6)
        Text[Body - kText] = 0xc3; // The guessed byte is unreachable.
      Image.ExceptionMetadata = {};
      coff_loader::parseX86RegistrationExceptions(Image);
      const auto *EH = Image.ExceptionMetadata.findFunction(kText + 0x20);
      ASSERT_TRUE(EH);
      EXPECT_EQ(EH->CodeRange.Begin, kText + 0x20);
      EXPECT_EQ(EH->CodeRange.End, Mutation == 0 ? Call + 6 : Boundary);
      if (Mutation == 0) {
        EXPECT_TRUE(
            coff_loader::getCheckedX86CxxEpilogueCall(Image, *EH, Call));
        const auto Function = liftEntry(Image, kText + 0x20);
        ASSERT_TRUE(Function.RegistrationStates);
        EXPECT_TRUE(Function.RegistrationStates->RegistrationLifetimeComplete);
      }
    }
}

TEST(RegistrationTryLevel, CxxSharedEntryDisprovesUntypedDataPointerBoundary) {
  for (bool Wide : {false, true})
    for (bool Relocated : {false, true}) {
      SCOPED_TRACE(::testing::Message() << Wide << ' ' << Relocated);
      auto [Image, Call] = makeCxxSharedEpilogueImage(Wide, 0, 32);
      const va_t Boundary = Call - 4;
      // The low byte of MOV EAX's scalar immediate also looks like PUSH EBP.
      Image.Segments.front().Data[Boundary - kText] = 0x55;
      for (auto &Segment : Image.Segments)
        if (Segment.VA == kRData)
          writeLE<uint32_t>(Segment.Data.data() + 0xf8, Boundary);
      if (Relocated)
        Image.CodePtrRelocSlots.insert(kRData + 0xf8);
      Image.ExceptionMetadata = {};
      scanDataFuncPointers(Image);
      const auto Candidate = std::find_if(
          Image.Symbols.begin(), Image.Symbols.end(),
          [&](const auto &Symbol) { return Symbol.Addr == Boundary; });
      ASSERT_NE(Candidate, Image.Symbols.end());
      EXPECT_EQ(Candidate->IsBoundaryGuess, !Relocated);
      coff_loader::parseX86RegistrationExceptions(Image);
      const auto *EH = Image.ExceptionMetadata.findFunction(kText + 0x20);
      ASSERT_TRUE(EH);
      EXPECT_EQ(EH->CodeRange.End, Relocated ? Boundary : Call + 6);
      EXPECT_EQ(
          bool(coff_loader::getCheckedX86CxxEpilogueCall(Image, *EH, Call)),
          !Relocated);
    }
}

TEST(RegistrationTryLevel, CxxSharedEntryOwnsItsProvenReturningFallthrough) {
  for (bool Wide : {false, true})
    for (unsigned Mutation = 0; Mutation != 6; ++Mutation) {
      SCOPED_TRACE(::testing::Message() << Wide << ' ' << Mutation);
      auto [Image, Call] = makeCxxSharedEpilogueImage(Wide, 0, 32);
      const va_t Body = Call - 19, Boundary = Body + 8;
      auto Guess = Symbol::makeFunc(Boundary);
      Guess.IsBoundaryGuess = true;
      if (Mutation == 1) {
        Guess.Origin = NameOrigin::Stated;
        Guess.Name = "stated_entry";
      }
      Image.Symbols.push_back(Guess);
      if (Mutation == 2) {
        Export Other;
        Other.Addr = Boundary;
        Image.Exports.push_back(Other);
      }
      auto &Text = Image.Segments.front().Data;
      if (Mutation == 3 || Mutation == 4) {
        // Even another call to the same helper does not inherit the exact
        // entry prefix's returning-call evidence.
        Text[Body - kText] = 0xe8;
        const va_t Target = kText + (Mutation == 3 ? 0x280 : 0x100);
        writeLE<uint32_t>(Text.data() + Body - kText + 1,
                          uint32_t(Target - Body - 5));
        std::fill(Text.begin() + Body - kText + 5,
                  Text.begin() + Boundary - kText, 0x90);
      }
      if (Mutation == 5)
        Text[Body - kText] = 0xc3; // An earlier return owns no fallthrough.
      Image.ExceptionMetadata = {};
      coff_loader::parseX86RegistrationExceptions(Image);
      const auto *EH = Image.ExceptionMetadata.findFunction(kText + 0x20);
      ASSERT_TRUE(EH);
      EXPECT_EQ(EH->CodeRange.End, Mutation == 0   ? Call + 6
                                   : Mutation == 5 ? Body + 1
                                                   : Boundary);
      if (Mutation == 0) {
        const auto Function = liftEntry(Image, kText + 0x20);
        ASSERT_TRUE(Function.RegistrationStates);
        EXPECT_TRUE(Function.RegistrationStates->RegistrationLifetimeComplete);
      }
    }
}

BinaryImage makeCxxEntryAlignedImage(unsigned Alignment, uint32_t Allocation,
                                     bool Wide, unsigned Saves = 7,
                                     int32_t NodeAdjustment = 0) {
  auto Image = makeCxxEAXStateImage(false, 0);
  auto &Text = Image.Segments.front().Data;
  std::fill(Text.begin() + 0x20, Text.begin() + 0x100, 0xcc);
  std::vector<uint8_t> Code{0x55, 0x8b, 0xec};
  Code.insert(Code.end(), {uint8_t(Wide ? 0x81 : 0x83), 0xe4});
  if (Wide)
    emit32(Code, uint32_t(-Alignment));
  else
    Code.push_back(uint8_t(-Alignment));
  Code.insert(Code.end(), {0x6a, 0xff, 0x68});
  emit32(Code, kText + 0x200);
  Code.insert(Code.end(), {0x64, 0xa1, 0, 0, 0, 0, 0x50,
                           uint8_t(Wide ? 0x81 : 0x83), 0xec});
  if (Wide)
    emit32(Code, Allocation);
  else
    Code.push_back(uint8_t(Allocation));
  unsigned SavedWords = 0;
  const uint8_t Registers[] = {0x53, 0x56, 0x57};
  for (unsigned I = 0; I != 3; ++I)
    if (Saves & (1u << I)) {
      Code.push_back(Registers[I]);
      ++SavedWords;
    }
  Code.push_back(0xa1);
  emit32(Code, kRData + 0xf0);
  Code.insert(Code.end(),
              {0x33, 0xc4, 0x50, 0x8d, uint8_t(Wide ? 0x84 : 0x44), 0x24});
  if (Wide)
    emit32(Code, Allocation + 4 * (SavedWords + 1) + NodeAdjustment);
  else
    Code.push_back(uint8_t(Allocation + 4 * (SavedWords + 1) + NodeAdjustment));
  Code.insert(Code.end(), {0x64, 0xa3, 0, 0, 0, 0, 0x8b, 0x08, 0x64, 0x89, 0x0d,
                           0, 0, 0, 0, 0x8b, 0xe5, 0x5d, 0xc3});
  std::copy(Code.begin(), Code.end(), Text.begin() + 0x20);
  auto Guess = Symbol::makeFunc(kText + 0x10);
  Guess.IsBoundaryGuess = true;
  Image.Symbols.push_back(Guess);
  Text[0xa0] = 0xc3;
  Image.Symbols.push_back(Symbol::makeFunc(kText + 0xa0));
  Image.ExceptionMetadata = {};
  coff_loader::parseX86RegistrationExceptions(Image);
  return Image;
}

TEST(RegistrationTryLevel, CxxEntryAlignmentDoesNotBorrowAPrecedingGuess) {
  for (bool Wide : {false, true})
    for (unsigned Alignment : {4, 8, 16, 128})
      for (uint32_t Allocation : {0, 48, 256}) {
        if (!Wide && Allocation > 127 - 16)
          continue;
        SCOPED_TRACE(::testing::Message()
                     << Wide << ' ' << Alignment << ' ' << Allocation);
        const auto Image =
            makeCxxEntryAlignedImage(Alignment, Allocation, Wide);
        const auto *EH = Image.ExceptionMetadata.findFunction(kText + 0x20);
        ASSERT_TRUE(EH);
        EXPECT_EQ(EH->CodeRange.Begin, kText + 0x20);
        EXPECT_EQ(EH->PersonalityVA, kText + 0x200);
        ASSERT_TRUE(EH->Registration);
        EXPECT_EQ(EH->Registration->ChainInstallVA,
                  kText + 0x20 + (Wide ? 16 : 13));
        // Identifying an actual entry grants neither a fixed EBP coordinate
        // nor the distinct realigned runtime-frame protocol.
        EXPECT_FALSE(EH->Registration->RegistrationOffset);
        EXPECT_FALSE(EH->Registration->TryLevelOffset);
        EXPECT_FALSE(EH->Registration->RealignedFrame);
        EXPECT_FALSE(Image.VerifiedFunctionEntries.count(kText + 0x20));
      }
}

TEST(RegistrationTryLevel, CxxEntryAlignmentCountsTheActualSavedWords) {
  for (bool Wide : {false, true})
    for (unsigned Saves = 0; Saves != 8; ++Saves)
      for (uint32_t Allocation : {0, 20, 48}) {
        SCOPED_TRACE(::testing::Message()
                     << Wide << ' ' << Saves << ' ' << Allocation);
        for (int32_t Adjustment : {0, -4, 4}) {
          SCOPED_TRACE(Adjustment);
          const auto Image =
              makeCxxEntryAlignedImage(8, Allocation, Wide, Saves, Adjustment);
          const auto *EH = Image.ExceptionMetadata.findFunction(kText + 0x20);
          if (Adjustment) {
            EXPECT_TRUE(!EH || EH->CodeRange.Begin != kText + 0x20);
            continue;
          }
          ASSERT_TRUE(EH);
          EXPECT_EQ(EH->CodeRange.Begin, kText + 0x20);
          ASSERT_TRUE(EH->Registration);
          EXPECT_FALSE(EH->Registration->RegistrationOffset);
          EXPECT_FALSE(EH->Registration->TryLevelOffset);
          EXPECT_FALSE(EH->Registration->RealignedFrame);
          EXPECT_FALSE(Image.VerifiedFunctionEntries.count(kText + 0x20));
        }
      }
}

TEST(RegistrationTryLevel, CxxEntryAlignmentSeparatesAdjacentRegistrations) {
  auto Image = makeCxxEntryAlignedImage(8, 20, false, 6);
  auto &Text = Image.Segments.front().Data;
  // There is no independently discovered symbol at the second entry. Its
  // complete registration prefix must delimit the first parent's metadata.
  std::copy_n(Text.begin() + 0x20, 0x40, Text.begin() + 0x80);
  writeLE<uint32_t>(Text.data() + 0x89, kText + 0x220);
  std::copy_n(Text.begin() + 0x200, 11, Text.begin() + 0x220);
  for (auto &Symbol : Image.Symbols)
    if (Symbol.Addr == kText + 0xa0)
      Symbol.Addr = kText + 0x100;
  Image.ExceptionMetadata = {};
  coff_loader::parseX86RegistrationExceptions(Image);
  const auto *First = Image.ExceptionMetadata.findFunction(kText + 0x20);
  const auto *Second = Image.ExceptionMetadata.findFunction(kText + 0x80);
  ASSERT_TRUE(First);
  ASSERT_TRUE(Second);
  EXPECT_EQ(First->CodeRange.Begin, kText + 0x20);
  EXPECT_EQ(First->CodeRange.End, kText + 0x80);
  EXPECT_EQ(First->PersonalityVA, kText + 0x200);
  EXPECT_EQ(Second->CodeRange.Begin, kText + 0x80);
  EXPECT_EQ(Second->PersonalityVA, kText + 0x220);
  EXPECT_NE(First, Second);
}

TEST(RegistrationTryLevel, CxxEntryAlignmentRequiresTheCompleteCurrentPrefix) {
  const auto Original = makeCxxEntryAlignedImage(8, 48, false);
  for (unsigned Mutation = 0; Mutation != 15; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Image = Original;
    auto *Code = Image.Segments.front().Data.data() + 0x20;
    switch (Mutation) {
    case 0:
      Code[0] = 0x56;
      break;
    case 1:
      Code[2] = 0xed;
      break;
    case 2:
      Code[5] = 0xfe;
      break;
    case 3:
      Code[7] = 0;
      break;
    case 4:
      Code[9] ^= 1;
      break;
    case 5:
      Code[15] = 4;
      break;
    case 6:
      Code[22] = 0xff;
      break;
    case 7:
      Code[24] = 0x57;
      break;
    case 8:
      Code[27] ^= 4;
      break;
    case 9:
      Code[32] = 0xc5;
      break;
    case 10:
      Code[37] += 4;
      break;
    case 11:
      Code[40] = 4;
      break;
    case 12:
      Image.BaseRelocations.push_back({kText + 0x25, 3});
      break;
    case 13:
      Image.Segments.front().Flags =
          Image.Segments.front().Flags | SegmentFlags::Writable;
      break;
    case 14:
      Image.DynInfo.SecurityCookieRVA = 0;
      break;
    }
    Image.ExceptionMetadata = {};
    coff_loader::parseX86RegistrationExceptions(Image);
    const auto *EH = Image.ExceptionMetadata.findFunction(kText + 0x20);
    EXPECT_TRUE(!EH || EH->CodeRange.Begin != kText + 0x20);
  }
}

std::pair<BinaryImage, va_t>
makeCxxGuardedEpilogueImage(bool Wide, uint32_t Allocation,
                            bool PopArguments = false,
                            bool DoubleGuard = false) {
  auto [Image, Call] =
      makeCxxSharedEpilogueImage(Wide, 2, Allocation, PopArguments);
  auto &Text = Image.Segments.front().Data;
  constexpr va_t Wrapper = kText + 0x1a0, Checker = kText + 0x240;
  writeLE<uint32_t>(Text.data() + Call - kText + 1, Wrapper - Call - 5);
  // Load the separate GS word, decode it with source EBP, check it, then
  // tail-jump to the same physical restore. The check changes flags.
  std::vector<uint8_t> Guard{0x8b, 0x4d, 0xf0, 0x33, 0xcd, 0xe8};
  emit32(Guard, Checker - Wrapper - 10);
  Guard.push_back(0xe9);
  emit32(Guard, uint32_t(kText + 0x180 - Wrapper - 15));
  std::copy(Guard.begin(), Guard.end(), Text.begin() + 0x1a0);
  std::vector<uint8_t> Check{0x3b, 0x0d};
  emit32(Check, kRData + 0xf0);
  Check.insert(Check.end(), {0x75, 0x18, 0xf3, 0xc3});
  std::copy(Check.begin(), Check.end(), Text.begin() + 0x240);
  // The exceptional dispatcher owns the same cookie, in node coordinates.
  std::vector<uint8_t> Handler{0x8b, 0x54, 0x24, 8, 0x8d, 0x42, 12};
  auto CheckCall = [&] {
    Handler.insert(Handler.end(), {0x33, 0xc8, 0xe8});
    emit32(Handler, uint32_t(Checker - (kText + 0x200 + Handler.size() + 4)));
  };
  if (DoubleGuard) {
    Handler.insert(Handler.end(), {0x8b, 0x8a});
    emit32(Handler, uint32_t(-16 - int32_t(Allocation)));
    CheckCall();
  }
  Handler.insert(Handler.end(), {0x8b, 0x4a, 0xfc});
  CheckCall();
  Handler.push_back(0xb8);
  emit32(Handler, kRData);
  Handler.insert(Handler.end(), {0xff, 0x25});
  emit32(Handler, kRData + 0x90);
  std::copy(Handler.begin(), Handler.end(), Text.begin() + 0x200);
  Image.ExceptionMetadata = {};
  coff_loader::parseX86RegistrationExceptions(Image);
  return {std::move(Image), Call};
}

TEST(RegistrationTryLevel, CxxGuardedSharedEpilogueProvesTheCookieBeforeExit) {
  for (bool Wide : {false, true})
    for (uint32_t Allocation : {4, 32, 248})
      for (bool PopArguments : {false, true})
        for (bool DoubleGuard : {false, true}) {
          if (!Wide && Allocation > 127)
            continue;
          SCOPED_TRACE(::testing::Message()
                       << Wide << ' ' << Allocation << ' ' << PopArguments
                       << ' ' << DoubleGuard);
          auto [Image, Call] = makeCxxGuardedEpilogueImage(
              Wide, Allocation, PopArguments, DoubleGuard);
          const auto Function = liftEntry(Image, kText + 0x20);
          ASSERT_TRUE(Function.ExceptionMetadata);
          const auto Exit = coff_loader::getCheckedX86CxxEpilogueCall(
              Image, *Function.ExceptionMetadata, Call);
          ASSERT_TRUE(Exit);
          EXPECT_EQ(Exit->Restore.Begin, kText + 0x180);
          ASSERT_TRUE(Exit->CookieCheck);
          EXPECT_EQ(Exit->CookieCheck->Checker.Begin, kText + 0x240);
          EXPECT_EQ(Exit->CookieCheck->SecurityCookieVA, kRData + 0xf0);
          EXPECT_EQ(Exit->CookieCheck->FailureVA, kText + 0x260);
          ASSERT_TRUE(Function.RegistrationStates);
          const auto &States = *Function.RegistrationStates;
          EXPECT_TRUE(States.Complete);
          EXPECT_TRUE(States.RegistrationLifetimeComplete);
          EXPECT_TRUE(States.ChainOperationsComplete);
          EXPECT_TRUE(States.CallFrameEffectsComplete);
          EXPECT_TRUE(States.SecurityCookiesComplete);
          EXPECT_TRUE(States.CxxPrologue);
          ASSERT_EQ(States.CxxCookieConditions.size(), DoubleGuard ? 2u : 1u);
          EXPECT_EQ(States.CxxCookieConditions.back().FrameOffset, -16);
          EXPECT_EQ(States.CxxCookieConditions.back().XORFrameOffset, 0);
          if (DoubleGuard)
            EXPECT_EQ(States.CxxCookieConditions.front().FrameOffset,
                      -28 - int32_t(Allocation));
          // The complete physical wrapper is not an ordinary cookie CALL.
          EXPECT_TRUE(States.CookieChecks.empty());
          EXPECT_TRUE(States.CallFrameEffects.empty());
          EXPECT_FALSE(cxxRegistrationFrameCoordinate(
              *Function.ExceptionMetadata, &States));
          EXPECT_FALSE(getCheckedX86RegistrationCxxParentABI(Function, Image));
          LowToMedConverter Converter;
          Converter.setBinaryImage(&Image);
          const auto Med =
              Converter.convert(Function, Arch::X86, BinaryFormat::COFF);
          EXPECT_FALSE(Med.RegistrationPhysicalFrame);
          EXPECT_TRUE(std::any_of(
              Med.Blocks.begin(), Med.Blocks.end(), [&](const auto &B) {
                return std::any_of(
                    B.Ops.begin(), B.Ops.end(), [&](const auto &Op) {
                      return Op.Addr == Call && Op.Opcode == NdOp::CALL;
                    });
              }));
        }
}

TEST(RegistrationTryLevel, CxxGuardedSharedEpilogueRechecksItsWholeCallChain) {
  auto [OriginalImage, Call] = makeCxxGuardedEpilogueImage(false, 32);
  const auto Original = liftEntry(OriginalImage, kText + 0x20);
  ASSERT_TRUE(Original.RegistrationStates);
  ASSERT_TRUE(Original.RegistrationStates->SecurityCookiesComplete);
  const std::vector<RegistrationCalleeFrameContract> Callees;
  const std::vector<RegistrationCleanupFrameContract> Cleanups;
  for (unsigned Mutation = 0; Mutation != 35; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Image = OriginalImage;
    auto &Text = Image.Segments.front().Data;
    if (Mutation < 15)
      Text[0x1a0 + Mutation] ^= 1;
    if (Mutation >= 15 && Mutation < 24) {
      // A different mapped mismatch destination is still a valid leaf
      // success path. All opcode/cookie/return changes must be rejected.
      const unsigned Byte = Mutation - 15;
      Text[0x240 + Byte + (Byte >= 7)] ^= 1;
    }
    switch (Mutation) {
    case 24:
      Image.BaseRelocations.push_back({kText + 0x241, 3});
      break;
    case 25:
      Image.BaseRelocations.push_back({kText + 0x1a6, 3});
      break;
    case 26:
      Text[0x180 + 13] = 0x5f;
      break;
    case 27:
      Image.BaseRelocations.push_back({Call + 1, 3});
      break;
    case 28:
      Image.Segments.push_back(Image.Segments.front());
      break;
    case 29:
      Image.Segments.front().Flags =
          Image.Segments.front().Flags | SegmentFlags::Writable;
      break;
    case 30:
      Text[0x209] = 0xf8; // Handler checks a different word.
      break;
    case 31:
      Text[0x206] = 8; // Handler decodes against a different frame point.
      break;
    case 32:
      Text[0x120] = 0x65; // Prologue stores ESP instead of the GS cookie.
      break;
    case 33:
      Image.DynInfo.SecurityCookieRVA += 4;
      break;
    case 34:
      // Retarget the caller away from the authenticated wrapper entry.
      Text[Call - kText + 1] ^= 1;
      break;
    }
    if (Mutation < 30 || Mutation >= 33)
      EXPECT_FALSE(coff_loader::getCheckedX86CxxEpilogueCall(
          Image, *Original.ExceptionMetadata, Call));
    const auto States =
        analyzeCheckedX86CxxStates(Image, Original, &Callees, &Cleanups);
    EXPECT_FALSE(States.RegistrationLifetimeComplete);
    EXPECT_FALSE(States.SecurityCookiesComplete);
    EXPECT_FALSE(States.CallFrameEffectsComplete);
    EXPECT_FALSE(States.CxxPrologue);
  }
}

TEST(RegistrationTryLevel, CxxGuardedSharedEpilogueUsesTheSuccessfulCMPFlags) {
  // A wrong flag choice reaches an ordinary unproved call. The success
  // branches must instead use CMP(cookie,cookie), not the body's OR flags.
  for (uint8_t Opcode : {0x74, 0x7a, 0x73, 0x79, 0x71})
    for (bool Invert : {false, true}) {
      SCOPED_TRACE(::testing::Message() << unsigned(Opcode) << ' ' << Invert);
      auto [Image, Call] = makeCxxGuardedEpilogueImage(false, 32);
      const uint8_t Before[] = {0x31, 0xc0, 0x83, 0xe8, 2}; // 0 - 2
      std::copy(std::begin(Before), std::end(Before),
                Image.Segments.front().Data.begin() + Call - kText - 5);
      std::vector<uint8_t> Tail{uint8_t(Opcode ^ Invert), 5, 0xe8};
      emit32(Tail, uint32_t(kText + 0x260 - Call - 12));
      Tail.push_back(0xc3);
      std::copy(Tail.begin(), Tail.end(),
                Image.Segments.front().Data.begin() + Call - kText + 5);
      Image.ExceptionMetadata = {};
      coff_loader::parseX86RegistrationExceptions(Image);
      const auto Function = liftEntry(Image, kText + 0x20);
      ASSERT_TRUE(Function.RegistrationStates);
      EXPECT_EQ(Function.RegistrationStates->CallFrameEffectsComplete, !Invert);
      EXPECT_EQ(Function.RegistrationStates->SecurityCookiesComplete, !Invert);
    }
}

TEST(RegistrationTryLevel, CxxGuardedSharedEpilogueRejectsAChangedLiveCookie) {
  auto [Image, Call] = makeCxxGuardedEpilogueImage(false, 32);
  const auto Original = liftEntry(Image, kText + 0x20);
  ASSERT_TRUE(Original.RegistrationStates);
  ASSERT_TRUE(Original.RegistrationStates->SecurityCookiesComplete);
  const std::vector<RegistrationCalleeFrameContract> Callees;
  const std::vector<RegistrationCleanupFrameContract> Cleanups;
  for (int32_t Offset : {-19, -18, -17, -16, -15, -14, -13}) {
    SCOPED_TRACE(Offset);
    auto Function = Original;
    bool Changed = false;
    for (auto &Block : Function.Blocks)
      for (auto &Op : Block.Ops)
        if (Op.Addr == Call - 19 && Op.Opcode == NdOp::INT_ADD &&
            Op.NumInputs == 2 && Op.Inputs[1].isConst() &&
            uint32_t(Op.Inputs[1].Offset) == uint32_t(-4)) {
          Op.Inputs[1] = NdVar::cst(uint32_t(Offset), 4);
          Changed = true;
        }
    ASSERT_TRUE(Changed);
    const auto States =
        analyzeCheckedX86CxxStates(Image, Function, &Callees, &Cleanups);
    EXPECT_FALSE(States.SecurityCookiesComplete);
    EXPECT_FALSE(States.RegistrationLifetimeComplete);
    EXPECT_FALSE(States.CallFrameEffectsComplete);
  }
}

TEST(RegistrationTryLevel, CxxSharedEpilogueRequiresCurrentImmutableBytes) {
  auto [OriginalImage, Call] = makeCxxSharedEpilogueImage(false, 1, 32);
  const auto Original = liftEntry(OriginalImage, kText + 0x20);
  ASSERT_TRUE(Original.ExceptionMetadata);
  ASSERT_TRUE(Original.RegistrationStates);
  ASSERT_TRUE(Original.RegistrationStates->RegistrationLifetimeComplete);
  for (unsigned Mutation = 0; Mutation != 26; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Image = OriginalImage;
    if (Mutation < 20)
      Image.Segments.front().Data[0x180 + Mutation] ^= 1;
    if (Mutation == 20)
      Image.BaseRelocations.push_back({kText + 0x181, 3});
    if (Mutation == 21)
      Image.BaseRelocations.push_back({Call + 1, 3});
    if (Mutation == 22)
      Image.Segments.front().Flags =
          Image.Segments.front().Flags | SegmentFlags::Writable;
    if (Mutation == 23)
      Image.Segments.push_back(Image.Segments.front());
    if (Mutation == 24)
      Image.Segments.front().Data[0x201] ^= 1;
    if (Mutation == 25)
      Image.Segments.front().Data[Call - kText + 1] ^= 1;
    EXPECT_FALSE(coff_loader::getCheckedX86CxxEpilogueCall(
        Image, *Original.ExceptionMetadata, Call));
    const auto States = analyzeCheckedX86CxxStates(Image, Original);
    EXPECT_FALSE(States.RegistrationLifetimeComplete);
    EXPECT_FALSE(States.ChainOperationsComplete);
  }
}

TEST(RegistrationTryLevel, CxxSharedEpilogueRequiresTheLiveCallAndSaves) {
  auto [Image, Call] = makeCxxSharedEpilogueImage(false, 1, 32);
  const auto Original = liftEntry(Image, kText + 0x20);
  ASSERT_TRUE(Original.RegistrationStates);
  ASSERT_TRUE(Original.RegistrationStates->RegistrationLifetimeComplete);
  for (unsigned Mutation = 0; Mutation != 18; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Function = Original;
    bool Changed = false;
    if (Mutation == 17) {
      Function.OrdinaryModuleAnalysisRoots.insert(Call + 1);
      Changed = true;
    }
    for (auto &Block : Function.Blocks) {
      for (auto &Boundary : Block.InstructionBoundaries)
        if (Boundary.Address == Call && Mutation < 6) {
          Changed = true;
          switch (Mutation) {
          case 0:
            --Boundary.Size;
            break;
          case 1:
            Boundary.Control = LowInstructionControl::Branch;
            break;
          case 2:
            Boundary.ControlFlags |=
                LowInstructionControlFlag::InstructionGuard;
            break;
          case 3:
            ++*Boundary.Immediate;
            break;
          case 4:
            Boundary.OpCount = 0;
            break;
          case 5:
            ++Block.EndAddr;
            break;
          }
        }
      for (auto &Op : Block.Ops) {
        if (Op.Addr == Call) {
          switch (Mutation) {
          case 6:
            Op.Output.Size = 8;
            Changed = true;
            break;
          case 7:
            Op.NumInputs = 2;
            Changed = true;
            break;
          case 8:
            ++Op.Inputs[0].Offset;
            Changed = true;
            break;
          case 9:
            Op.Seq = -1;
            Changed = true;
            break;
          case 10:
            Op.MemoryOrdering = NdMemoryOrdering::Relaxed;
            Changed = true;
            break;
          }
        }
        if ((Mutation == 11 || Mutation == 12) && Op.Addr == Call - 5 &&
            Op.Opcode == NdOp::COPY &&
            Op.Output == NdVar::reg(x86reg::RAX, 4)) {
          Op.Output = NdVar::reg(Mutation == 11 ? x86reg::RSP : x86reg::RBP, 4);
          Changed = true;
        }
        if (Mutation == 13 && Op.Opcode == NdOp::INT_OR &&
            Op.Output.Size == 4 && Op.Addr == Call - 15) {
          Op.Inputs[1] = NdVar::cst(0, 4);
          Changed = true;
        }
        if (Mutation >= 14 && Mutation <= 16 && Op.Addr == Call - 19 &&
            Op.Opcode == NdOp::INT_ADD && Op.NumInputs == 2 &&
            Op.Inputs[1].isConst() &&
            uint32_t(Op.Inputs[1].Offset) == uint32_t(-4)) {
          // Destroy the caller PC, a saved EBX word, or the previous chain.
          const int32_t Offset = Mutation == 14   ? 4
                                 : Mutation == 15 ? -48
                                                  : -12;
          Op.Inputs[1] = NdVar::cst(uint32_t(Offset), 4);
          Changed = true;
        }
      }
    }
    ASSERT_TRUE(Changed);
    const auto States = analyzeCheckedX86CxxStates(Image, Function);
    EXPECT_FALSE(States.RegistrationLifetimeComplete);
    EXPECT_FALSE(States.ChainOperationsComplete);
  }
}

std::pair<BinaryImage, va_t>
makeDirectCxxStackGuardImage(unsigned Allocation, unsigned Saves,
                             bool AlternateXor, bool AlternateStore,
                             unsigned Mutation = 0, bool Hotpatch = false,
                             bool Probe = false, bool PriorOwner = false) {
  auto Img = makeCxxContinuationImage();
  constexpr uint32_t Cookie = kRData + 0xf0;
  Img.DynInfo.SecurityCookieRVA = Cookie - kBase;
  std::vector<uint8_t> Prefix = {0x55, 0x8b, 0xec, 0x6a, 0xff, 0x68};
  if (Hotpatch)
    Prefix.insert(Prefix.begin(), {0x8b, 0xff});
  emit32(Prefix, kPersonality);
  Prefix.insert(Prefix.end(), {0x64, 0xa1, 0, 0, 0, 0, 0x50});
  if (Probe) {
    Prefix.push_back(0xb8);
    emit32(Prefix, Allocation);
    Prefix.push_back(0xe8);
    emit32(Prefix, 0x200 - (Prefix.size() + 4));
  } else if (Allocation || Mutation == 10 || Mutation == 11) {
    if (Allocation < 128 && Mutation != 10 && Mutation != 11)
      Prefix.insert(Prefix.end(), {0x83, 0xec, uint8_t(Allocation)});
    else {
      Prefix.insert(Prefix.end(), {0x81, 0xec});
      emit32(Prefix, Mutation == 10   ? uint32_t(-4)
                     : Mutation == 11 ? limits::kMaxSourceCallBorrowedBytes + 1
                                      : Allocation);
    }
  }
  const uint8_t Saved[] = {0x53, 0x56, 0x57};
  for (unsigned I = 0; I != 3; ++I)
    if (Saves & (1u << I))
      Prefix.push_back(Saved[I]);
  if (Mutation == 8)
    Prefix.insert(Prefix.end(), {0x53, 0x53});
  if (Mutation == 9)
    Prefix.push_back(0x90);
  Prefix.push_back(0xa1);
  emit32(Prefix, Mutation == 2 ? Cookie + 4 : Cookie);
  Prefix.push_back(AlternateXor ? 0x33 : 0x31);
  Prefix.push_back(Mutation == 3 ? (AlternateXor ? 0xc4 : 0xe0)
                                 : (AlternateXor ? 0xc5 : 0xe8));
  Prefix.push_back(Mutation == 4 ? 0x51 : 0x50);
  Prefix.insert(Prefix.end(),
                {0x8d, 0x45, uint8_t(Mutation == 5 ? 0xf8 : 0xf4)});
  const va_t Publication = kText + Prefix.size();
  if (AlternateStore)
    Prefix.insert(Prefix.end(),
                  {0x64, 0x89, uint8_t(Mutation == 7 ? 0x0d : 0x05)});
  else
    Prefix.insert(Prefix.end(), {0x64, uint8_t(Mutation == 7 ? 0xa2 : 0xa3)});
  emit32(Prefix, Mutation == 6 ? 4 : 0);
  // Ordinary body allocation starts after publication and leaves the
  // original node and pushed guard word in place.
  Prefix.insert(Prefix.end(), {0x83, 0xec, 0x10});
  emitFrameStore(Prefix, -4, 0);
  emitFrameStore(Prefix, -4, -1);
  Prefix.push_back(0xe9);
  emit32(Prefix, 0x80 - (Prefix.size() + 4));
  EXPECT_LT(Prefix.size(), 0x80u);
  auto &Text = Img.Segments[0];
  std::fill(Text.Data.begin(), Text.Data.begin() + 0x80, 0xcc);
  std::copy(Prefix.begin(), Prefix.end(), Text.Data.begin());
  if (Mutation == 13)
    Text.Data[4] = 0;
  if (Mutation == 14)
    Text.Data[2] = 0xc4;
  if (Mutation == 1)
    Img.DynInfo.SecurityCookieRVA = 0;
  if (Mutation == 15)
    Img.DynInfo.SecurityCookieRVA = UINT32_MAX;
  // Reconstruct the C++ identity from the actual personality thunk and
  // FuncInfo bytes, rather than carrying over the helper's metadata.
  Text.Data.resize(0x110, 0xcc);
  Text.Size = Text.FileSz = Text.Data.size();
  Img.Sections[0].Size = Img.Sections[0].FileSz = Text.Size;
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
  if (Probe) {
    const std::vector<uint8_t> Helper = {
        0x51, 0x8d, 0x4c, 0x24, 0x04, 0x2b, 0xc8, 0x1b, 0xc0, 0xf7, 0xd0,
        0x23, 0xc8, 0x8b, 0xc4, 0x25, 0x00, 0xf0, 0xff, 0xff, 0x3b, 0xc8,
        0x72, 0x0a, 0x8b, 0xc1, 0x59, 0x94, 0x8b, 0x00, 0x89, 0x04, 0x24,
        0xc3, 0x2d, 0x00, 0x10, 0x00, 0x00, 0x85, 0x00, 0xeb, 0xe9};
    Text.Data.resize(0x200, 0xcc);
    Text.Data.insert(Text.Data.end(), Helper.begin(), Helper.end());
    Text.Size = Text.FileSz = Text.Data.size();
    Img.Sections[0].Size = Img.Sections[0].FileSz = Text.Size;
    Img.Symbols.push_back(Symbol::makeFunc(kText + 0x200));
  }
  if (Mutation == 12) {
    Text.Data.resize(Publication - kText + 5);
    Text.Size = Text.FileSz = Text.Data.size();
    Img.Sections[0].Size = Img.Sections[0].FileSz = Text.Size;
  }
  if (PriorOwner) {
    // A preceding guessed range must not own this complete installation.
    Text.Data.insert(Text.Data.begin(), 0x20, 0xcc);
    Text.Data.front() = 0xc3;
    Text.VA -= 0x20;
    Text.Size = Text.FileSz = Text.Data.size();
    Img.Sections[0].VA = Text.VA;
    Img.Sections[0].Size = Img.Sections[0].FileSz = Text.Size;
    for (auto &Symbol : Img.Symbols)
      if (Symbol.Addr == kText)
        Symbol = Symbol::makeFunc(kText - 0x20);
    Img.VerifiedFunctionEntries.clear();
  }
  Img.ExceptionMetadata = {};
  coff_loader::parseX86RegistrationExceptions(Img);
  return std::make_pair(std::move(Img), Publication);
}

TEST(RegistrationTryLevel, DirectCxxStackGuardPreservesTheRegistrationNode) {
  for (unsigned Allocation : {0u, 16u, 128u})
    for (unsigned Saves = 0; Saves != 8; ++Saves)
      for (bool Xor : {false, true})
        for (bool Store : {false, true}) {
          SCOPED_TRACE(std::to_string(Allocation) + "/" +
                       std::to_string(Saves) + "/" + std::to_string(Xor) + "/" +
                       std::to_string(Store));
          const auto [Img, Publication] =
              makeDirectCxxStackGuardImage(Allocation, Saves, Xor, Store);
          const auto *EH = Img.ExceptionMetadata.findFunction(kText);
          ASSERT_NE(EH, nullptr);
          ASSERT_TRUE(EH->Registration);
          ASSERT_TRUE(EH->Cxx);
          EXPECT_EQ(EH->ParseStatus, ExceptionParseStatus::Complete);
          const auto &Chain = *EH->Registration;
          EXPECT_EQ(Chain.RegistrationOffset, -12);
          EXPECT_EQ(Chain.TryLevelOffset, -4);
          EXPECT_EQ(Chain.SeededTryLevel, -1);
          EXPECT_EQ(Chain.cxxRuntimeFrameOffset(), 0);
          EXPECT_EQ(Chain.ChainInstallVA, Publication);
          EXPECT_FALSE(Chain.HasSecurityCookies);
        }
  for (unsigned Mutation = 1; Mutation != 16; ++Mutation)
    for (bool Store : {false, true}) {
      SCOPED_TRACE(std::to_string(Mutation) + "/" + std::to_string(Store));
      const auto [Img, Publication] =
          makeDirectCxxStackGuardImage(16, 7, true, Store, Mutation);
      const auto *EH = Img.ExceptionMetadata.findFunction(kText);
      EXPECT_FALSE(EH && EH->Registration &&
                   EH->Registration->cxxRuntimeFrameOffset());
    }
  for (bool Store : {false, true}) {
    const auto [Img, Publication] =
        makeDirectCxxStackGuardImage(16, 7, true, Store, 0, true);
    const auto *EH = Img.ExceptionMetadata.findFunction(kText);
    ASSERT_NE(EH, nullptr);
    EXPECT_EQ(EH->CodeRange.Begin, kText);
    ASSERT_TRUE(EH->Registration);
    EXPECT_EQ(EH->Registration->RegistrationOffset, -12);
    EXPECT_EQ(EH->Registration->TryLevelOffset, -4);
    EXPECT_EQ(EH->Registration->ChainInstallVA, Publication);
  }
}

TEST(RegistrationTryLevel, DirectCxxAllocatingProbeKeepsItsFrameAndCall) {
  for (unsigned Allocation :
       {0u, 1u, 16u, 128u, 0x12e0u, limits::kMaxSourceCallBorrowedBytes})
    for (unsigned Saves : {0u, 7u})
      for (bool Store : {false, true})
        for (unsigned Owner = 0; Owner != 3; ++Owner) {
          SCOPED_TRACE(::testing::Message() << Allocation << "/" << Saves << "/"
                                            << Store << "/" << Owner);
          const bool Hotpatch = Owner == 1;
          const auto [Img, Publication] = makeDirectCxxStackGuardImage(
              Allocation, Saves, true, Store, 0, Hotpatch, true, Owner == 2);
          const auto *EH = Img.ExceptionMetadata.findFunction(kText);
          ASSERT_NE(EH, nullptr);
          ASSERT_EQ(EH->CodeRange.Begin, kText);
          ASSERT_TRUE(EH->Registration);
          EXPECT_EQ(EH->Registration->RegistrationOffset, -12);
          EXPECT_EQ(EH->Registration->TryLevelOffset, -4);
          EXPECT_EQ(EH->Registration->ChainInstallVA, Publication);
          EXPECT_EQ(EH->Registration->cxxRuntimeFrameOffset(), 0);
          const va_t CallVA = kText + 22 + (Hotpatch ? 2 : 0);
          const auto Probe =
              coff_loader::getCheckedX86AllocatingStackProbeCall(Img, CallVA);
          ASSERT_TRUE(Probe);
          EXPECT_EQ(Probe->Caller.Begin, CallVA - 5);
          EXPECT_EQ(Probe->Caller.End, CallVA + 5);
          EXPECT_EQ(Probe->CallVA, CallVA);
          EXPECT_EQ(Probe->Helper.Begin, kText + 0x200);
          EXPECT_EQ(Probe->Helper.End, kText + 0x22b);
          EXPECT_EQ(Probe->AllocationBytes, Allocation);
        }
  auto [Img, Publication] =
      makeDirectCxxStackGuardImage(0x12e0, 7, true, false, 0, false, true);
  const auto Low = liftEntry(Img, kText);
  unsigned Calls = 0;
  for (const auto &Block : Low.Blocks)
    for (const auto &Op : Block.Ops)
      if (Op.Addr == kText + 22 && Op.Opcode == NdOp::CALL) {
        ++Calls;
        ASSERT_EQ(Op.NumInputs, 1);
        EXPECT_EQ(Op.Inputs[0], NdVar::cst(kText + 0x200, 4));
      }
  EXPECT_EQ(Calls, 1u);
  ASSERT_TRUE(Low.RegistrationStates);
  EXPECT_FALSE(Low.RegistrationStates->Complete);
  LowToMedConverter Converter;
  Converter.setBinaryImage(&Img);
  const auto Med = Converter.convert(Low, Arch::X86, BinaryFormat::COFF);
  Calls = 0;
  for (const auto &Block : Med.Blocks)
    for (const auto &Op : Block.Ops)
      if (Op.Addr == kText + 22 && Op.Opcode == NdOp::CALL)
        ++Calls;
  EXPECT_EQ(Calls, 1u);
  EXPECT_FALSE(Med.RegistrationPhysicalFrame);
}

TEST(RegistrationTryLevel,
     AllocatingProbeRequiresCurrentInstructionsAndStorage) {
  const auto [Original, Publication] =
      makeDirectCxxStackGuardImage(0x12e0, 7, true, false, 0, false, true);
  constexpr va_t CallVA = kText + 22;
  auto CheckRejected = [&](BinaryImage Image) {
    EXPECT_FALSE(
        coff_loader::getCheckedX86AllocatingStackProbeCall(Image, CallVA));
    Image.ExceptionMetadata = {};
    coff_loader::parseX86RegistrationExceptions(Image);
    const auto *EH = Image.ExceptionMetadata.findFunction(kText);
    EXPECT_FALSE(EH && EH->Registration &&
                 EH->Registration->cxxRuntimeFrameOffset());
  };
  for (unsigned Byte = 0; Byte != 43; ++Byte) {
    SCOPED_TRACE(Byte);
    auto Image = Original;
    Image.Segments[0].Data[0x200 + Byte] ^= 1;
    CheckRejected(std::move(Image));
  }
  for (unsigned Mutation = 0; Mutation != 19; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Image = Original;
    auto &Text = Image.Segments[0];
    switch (Mutation) {
    case 0:
      Text.Data[17] = 0xb9; // Allocation no longer enters EAX.
      break;
    case 1:
      Text.Data[22] = 0xe9; // Tail jump has no continuation PC.
      break;
    case 2:
      ++Text.Data[23]; // Enter the helper at an operand/interior instruction.
      break;
    case 3:
      writeLE<uint32_t>(Text.Data.data() + 18, UINT32_MAX);
      break;
    case 4:
      writeLE<uint32_t>(Text.Data.data() + 18,
                        limits::kMaxSourceCallBorrowedBytes + 1);
      break;
    case 5:
      Text.Flags = Text.Flags | SegmentFlags::Writable;
      break;
    case 6:
      setMappedSectionFlags(Image, Image.Sections[0],
                            Image.Sections[0].Flags | SegmentFlags::Writable);
      break;
    case 7:
      Text.Data.pop_back();
      break;
    case 8:
      --Text.FileSz;
      break;
    case 9:
      --Image.Sections[0].FileSz;
      break;
    case 10:
      Image.Segments.push_back(Text);
      break;
    case 11:
      Image.Sections.push_back(Image.Sections[0]);
      break;
    case 12:
      Image.BaseRelocations.push_back({CallVA - 4, 3});
      break;
    case 13:
      Image.BaseRelocations.push_back({CallVA + 1, 3});
      break;
    case 14:
      Image.BaseRelocations.push_back({kText + 0x210, 3});
      break;
    case 15:
      Image.Format = BinaryFormat::ELF;
      break;
    case 16:
      Image.Arch = Arch::X64;
      break;
    case 17:
      Image.Bits = Bitness::Bits64;
      break;
    case 18:
      Image.IsRelocatable = true;
      break;
    }
    CheckRejected(std::move(Image));
  }
  for (va_t Call : {va_t(0), va_t(4), va_t(UINT32_MAX), CallVA - 1, CallVA + 1})
    EXPECT_FALSE(
        coff_loader::getCheckedX86AllocatingStackProbeCall(Original, Call));
  // A valid probe still cannot authenticate a changed publication or node.
  for (unsigned Mutation : {1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 9u, 13u, 14u}) {
    SCOPED_TRACE(Mutation);
    const auto [Image, Store] = makeDirectCxxStackGuardImage(
        0x12e0, 7, true, false, Mutation, false, true, true);
    ASSERT_TRUE(
        coff_loader::getCheckedX86AllocatingStackProbeCall(Image, CallVA));
    const auto *EH = Image.ExceptionMetadata.findFunction(kText);
    EXPECT_FALSE(EH && EH->Registration &&
                 EH->Registration->cxxRuntimeFrameOffset());
    EXPECT_FALSE(Image.VerifiedFunctionEntries.count(kText));
  }
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
  EXPECT_EQ(State.Blocks[Resume->Id].levels(), (std::vector<int32_t>{1}));
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
    EXPECT_EQ(Block.levels(), (std::vector<int32_t>{-1, 0}));
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
      EXPECT_EQ(Block.levels(), (std::vector<int32_t>{-1, 0}));
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

TEST(RegistrationTryLevel, DistinctDirectProloguesDoNotShareAGuessedOwner) {
  for (bool Damaged : {false, true}) {
    SCOPED_TRACE(Damaged);
    ImageBuilder B;
    B.addScope(-1, kText + 0xa0, kText + 0xb0);
    B.endScopes();
    const auto SecondTable = B.rdataVA();
    B.addScope(-1, kText + 0xa8, kText + 0xb8);
    B.endScopes();
    emitInstall(B.Text, -1, kRData);
    B.Text.push_back(0xc3);
    const auto SecondEntry = B.textVA();
    emitInstall(B.Text, -1, SecondTable);
    B.Text.push_back(0xc3);
    if (Damaged)
      B.Text[SecondEntry - kText + 1] = 0x90;
    B.Text.resize(0xa0, 0xcc);
    B.addStub();
    B.Text.resize(0xa8, 0xcc);
    B.addStub();
    B.Text.resize(0xb0, 0xcc);
    B.addStub();
    B.Text.resize(0xb8, 0xcc);
    B.addStub();
    const auto Image = B.build({{"first", kText}});
    const auto *First = chainAt(Image, kText);
    ASSERT_TRUE(First);
    EXPECT_EQ(First->ChainInstallVA, kText + 22);
    const auto *Second = chainAt(Image, SecondEntry);
    if (Damaged)
      EXPECT_FALSE(Second);
    else {
      ASSERT_TRUE(Second);
      EXPECT_EQ(Second->ScopeTableVA, SecondTable);
      EXPECT_EQ(Second->ChainInstallVA, SecondEntry + 22);
      EXPECT_EQ(Second->RegistrationOffset, -16);
    }
  }
}
