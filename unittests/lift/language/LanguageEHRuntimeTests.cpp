//===- LanguageEHRuntimeTests.cpp - Language runtime identity tests ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "LanguageEHTestsDetail.h"
#include "gtest/gtest.h"

#include "neverd/sigs/SignatureDB.h"
#include "neverd/sigs/SignatureMatcher.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/raw_ostream.h"

#include <type_traits>

namespace {

using namespace neverd;
using namespace neverd::dwarf_eh;
using namespace neverd::language_eh_test;

static_assert(std::is_aggregate_v<ExceptionFunction>);
static_assert(std::is_aggregate_v<ExceptionInfo>);

//===----------------------------------------------------------------------===//
// Language runtime identity
//===----------------------------------------------------------------------===//

TEST(LanguageRuntimeNames, ClassifiesEveryKnownPersonality) {
  EXPECT_EQ(classifyPersonalityName("__gxx_personality_v0"),
            ExceptionPersonality::GxxPersonalityV0);
  EXPECT_EQ(classifyPersonalityName("___gxx_personality_v0"),
            ExceptionPersonality::GxxPersonalityV0);
  EXPECT_EQ(classifyPersonalityName("__gcc_personality_v0"),
            ExceptionPersonality::GccPersonalityV0);
  EXPECT_EQ(classifyPersonalityName("rust_eh_personality"),
            ExceptionPersonality::RustEhPersonality);
  EXPECT_EQ(classifyPersonalityName("_except_handler3"),
            ExceptionPersonality::ExceptHandler3);
  EXPECT_EQ(classifyPersonalityName("_except_handler4"),
            ExceptionPersonality::ExceptHandler4);
  EXPECT_EQ(classifyPersonalityName("__CxxFrameHandler"),
            ExceptionPersonality::CxxFrameHandlerX86);
  EXPECT_EQ(classifyPersonalityName("__CxxFrameHandler3"),
            ExceptionPersonality::CxxFrameHandler3);
  EXPECT_EQ(classifyPersonalityName("__imp___CxxFrameHandler3"),
            ExceptionPersonality::CxxFrameHandler3);
  EXPECT_EQ(classifyPersonalityName("__C_specific_handler"),
            ExceptionPersonality::CSpecificHandler);
  EXPECT_EQ(classifyPersonalityName("__DelphiExceptionHandler"),
            ExceptionPersonality::DelphiExceptionHandler);
  EXPECT_EQ(classifyPersonalityName("@HandleAnyException"),
            ExceptionPersonality::DelphiX86Handler);
  EXPECT_EQ(classifyPersonalityName("__gnat_personality_v0"),
            ExceptionPersonality::GnatPersonalityV0);
  EXPECT_EQ(classifyPersonalityName("__dmd_personality_v0"),
            ExceptionPersonality::DmdPersonalityV0);
  EXPECT_EQ(classifyPersonalityName("not_a_personality"),
            ExceptionPersonality::Unknown);
  EXPECT_EQ(classifyPersonalityName(""), ExceptionPersonality::None);
}

TEST(LanguageRuntimeNames, NamesTheAdaAndDRoutinesAndTheirVariants) {
  // GCC spells every front end's personality the same three ways -- `_v0` for
  // DWARF, `_sj0` for setjmp/longjmp, `_seh0` for Windows -- and GNAT and GDC
  // are no exception to it.  On Windows the routine the image registers and
  // the GCC-shaped one it forwards to are two symbols for one frame's
  // dispatch, so both have to reach the same enumerator.
  EXPECT_EQ(classifyPersonalityName("__gnat_personality_sj0"),
            ExceptionPersonality::GnatPersonalitySJ0);
  EXPECT_EQ(classifyPersonalityName("__gnat_personality_seh0"),
            ExceptionPersonality::GnatPersonalitySEH0);
  EXPECT_EQ(classifyPersonalityName("__gnat_personality_imp"),
            ExceptionPersonality::GnatPersonalitySEH0);
  EXPECT_EQ(classifyPersonalityName("__gdc_personality_v0"),
            ExceptionPersonality::GdcPersonalityV0);
  EXPECT_EQ(classifyPersonalityName("__gdc_personality_sj0"),
            ExceptionPersonality::GdcPersonalitySJ0);
  EXPECT_EQ(classifyPersonalityName("_d_eh_personality"),
            ExceptionPersonality::DRuntimeEhPersonality);

  // Three compilers, three names for the routine, one language behind them.
  for (ExceptionPersonality P : {ExceptionPersonality::DmdPersonalityV0,
                                 ExceptionPersonality::DRuntimeEhPersonality,
                                 ExceptionPersonality::GdcPersonalityV0,
                                 ExceptionPersonality::GdcPersonalitySJ0,
                                 ExceptionPersonality::GdcPersonalitySEH0})
    EXPECT_EQ(getPersonalityRuntime(P), SourceLanguageRuntime::D);
  for (ExceptionPersonality P : {ExceptionPersonality::GnatPersonalityV0,
                                 ExceptionPersonality::GnatPersonalitySJ0,
                                 ExceptionPersonality::GnatPersonalitySEH0})
    EXPECT_EQ(getPersonalityRuntime(P), SourceLanguageRuntime::Ada);
}

// An AArch64 image puts a mapping symbol at the start of practically every
// function, at the same address as the function's own symbol.  Resolving a
// routine by address has to step over them, or an aarch64 Rust object reports
// an unknown personality for every frame it has and decodes no landing pads.
TEST(LanguageRuntimeNames, SkipsArmMappingSymbolsWhenNamingARoutine) {
  BinaryImage Img;
  Img.Arch = Arch::AArch64;
  Img.Bits = Bitness::Bits64;
  constexpr va_t kRoutine = 0x2ecc8;
  // The order a real symbol table has them in: the marker comes first, which
  // is why taking the first match by address is not enough.
  for (llvm::StringRef Name : {"$x", "rust_eh_personality"}) {
    Symbol Sym;
    Sym.Name = Name.str();
    Sym.Addr = kRoutine;
    Sym.IsFunc = Name != "$x";
    Img.Symbols.push_back(std::move(Sym));
  }

  EXPECT_EQ(resolveRoutineName(Img, kRoutine), "rust_eh_personality");
}

TEST(LanguageRuntimeNames, TellsAMappingSymbolFromAnOrdinaryDollarName) {
  BinaryImage Img;
  constexpr va_t kRoutine = 0x1000;
  // `$a`/`$d`/`$t`/`$x` and their dotted forms are the whole of the ABI's
  // list, so a name that merely starts with `$` is still a name.
  Symbol Sym;
  Sym.Name = "$literal_pool_helper";
  Sym.Addr = kRoutine;
  Img.Symbols.push_back(std::move(Sym));

  EXPECT_EQ(resolveRoutineName(Img, kRoutine), "$literal_pool_helper");
}

// Every non-x86 ELF target encodes the CIE's personality indirectly, through a
// `DW.ref.` slot rather than the routine itself.  Left unhandled, that is not
// a cosmetic miss: an aarch64 Rust object reports an unknown personality for
// every frame it has, and so decodes none of its landing pads.
TEST(LanguageRuntimeNames, LooksThroughADwarfIndirectionSlot) {
  EXPECT_EQ(classifyPersonalityName("DW.ref.rust_eh_personality"),
            ExceptionPersonality::RustEhPersonality);
  EXPECT_EQ(classifyPersonalityName("DW.ref.__gxx_personality_v0"),
            ExceptionPersonality::GxxPersonalityV0);
  // The prefix names what the slot holds, so a slot holding nothing known is
  // still unknown rather than becoming a personality by association.
  EXPECT_EQ(classifyPersonalityName("DW.ref.not_a_personality"),
            ExceptionPersonality::Unknown);
}

TEST(LanguageRuntimeNames, MapsPersonalityToItsRuntime) {
  EXPECT_EQ(getPersonalityRuntime(ExceptionPersonality::RustEhPersonality),
            SourceLanguageRuntime::Rust);
  EXPECT_EQ(getPersonalityRuntime(ExceptionPersonality::GxxPersonalityV0),
            SourceLanguageRuntime::CxxItanium);
  EXPECT_EQ(getPersonalityRuntime(ExceptionPersonality::GccPersonalityV0),
            SourceLanguageRuntime::C);
  EXPECT_EQ(getPersonalityRuntime(ExceptionPersonality::CxxFrameHandler3),
            SourceLanguageRuntime::CxxMSVC);
  EXPECT_EQ(getPersonalityRuntime(ExceptionPersonality::DelphiX86Handler),
            SourceLanguageRuntime::Delphi);
  // A personality shared by several languages must not claim one of them.
  EXPECT_EQ(getPersonalityRuntime(ExceptionPersonality::CSpecificHandler),
            SourceLanguageRuntime::Unknown);
}
TEST(LanguageRuntimeDetection, IdentifiesRustFromStandardLibrarySymbols) {
  BinaryImage Img = makeImage();
  Symbol Sym;
  Sym.Name = "_ZN4core9panicking9panic_fmt17h0123456789abcdefE";
  Sym.Addr = kTextVA + 0x40;
  Sym.IsFunc = true;
  Img.Symbols.push_back(std::move(Sym));

  LanguageRuntimeInfo Info = detectLanguageRuntime(Img);
  EXPECT_TRUE(Info.is(SourceLanguageRuntime::Rust));
  EXPECT_FALSE(Info.Evidence.empty());
}

TEST(LanguageRuntimeDetection, IdentifiesGoFromItsFunctionTableSection) {
  BinaryImage Img = makeImage();
  Section Pcln;
  Pcln.Name = ".gopclntab";
  Pcln.VA = kDataVA + 0x800;
  Pcln.Size = 0x40;
  Img.Sections.push_back(std::move(Pcln));

  LanguageRuntimeInfo Info = detectLanguageRuntime(Img);
  EXPECT_EQ(Info.Runtime, SourceLanguageRuntime::Go);
}

TEST(LanguageRuntimeDetection, ReportsAMixedImage) {
  BinaryImage Img = makeImage();
  Section Pcln;
  Pcln.Name = ".gopclntab";
  Pcln.VA = kDataVA + 0x800;
  Pcln.Size = 0x40;
  Img.Sections.push_back(std::move(Pcln));
  Symbol Rust;
  Rust.Name = "_ZN3std2rt10lang_start17h0123456789abcdefE";
  Rust.Addr = kTextVA + 0x40;
  Rust.IsFunc = true;
  Img.Symbols.push_back(std::move(Rust));

  LanguageRuntimeInfo Info = detectLanguageRuntime(Img);
  EXPECT_TRUE(Info.IsMixed);
  EXPECT_TRUE(Info.is(SourceLanguageRuntime::Go));
  EXPECT_TRUE(Info.is(SourceLanguageRuntime::Rust));
}

TEST(LanguageRuntimeDetection, LeavesAPlainImageUnclassified) {
  BinaryImage Img = makeImage();
  LanguageRuntimeInfo Info = detectLanguageRuntime(Img);
  EXPECT_EQ(Info.Runtime, SourceLanguageRuntime::Unknown);
}

//===----------------------------------------------------------------------===//
// Personality routines a stripped image cannot name
//===----------------------------------------------------------------------===//
//
// Everything above resolves a personality from a name the image spells.  A
// stripped, statically linked image spells none, so the routine is an address
// and every frame installing it goes uninterpreted.  What follows covers the
// way back in: which addresses are offered for identification, what a name has
// to satisfy before exception classification will take it, and the signature
// path that supplies one.

constexpr va_t kPersonalityVA = kTextVA + 0x200;
constexpr va_t kSecondPersonalityVA = kTextVA + 0x400;

/// Bytes standing in for a personality routine's body.
///
/// The sequence only has to be something no other 64 bytes of the image are,
/// which the filler `0x90` of \ref makeImage is not.
std::vector<uint8_t> routineBytes(uint8_t Seed) {
  std::vector<uint8_t> Bytes(64);
  for (size_t I = 0; I < Bytes.size(); ++I)
    Bytes[I] = static_cast<uint8_t>(Seed + I * 7);
  return Bytes;
}

void addFrame(BinaryImage &Img, va_t CodeVA, va_t PersonalityVA,
              ExceptionPersonality Classified = ExceptionPersonality::Unknown) {
  ExceptionFunction F;
  F.CodeRange = {CodeVA, CodeVA + 0x20};
  F.PersonalityVA = PersonalityVA;
  F.Personality = Classified;
  Img.ExceptionMetadata.Functions.push_back(std::move(F));
}

void addSymbol(BinaryImage &Img, va_t Address, llvm::StringRef Name) {
  Symbol Sym;
  Sym.Name = Name.str();
  Sym.Addr = Address;
  Sym.IsFunc = true;
  Img.Symbols.push_back(std::move(Sym));
}

/// A `.pat` line describing \p Bytes.
///
/// \p Covered is how far the leading pattern and the CRC span reach; the rest
/// of \p Bytes becomes the tail when \p WithTail, and is left undescribed
/// otherwise.  The second form is what a signature that agrees with only the
/// start of a routine looks like, which is the case the personality gate has
/// to refuse.
std::string makePatLine(llvm::ArrayRef<uint8_t> Bytes, llvm::StringRef Name,
                        size_t Covered, bool WithTail) {
  const size_t Leading = std::min<size_t>(16, Covered);
  const size_t CRCLen = Covered - Leading;
  const uint16_t CRC =
      CRCLen
          ? sigs::SignatureMatcher::computeCRC16(Bytes.data() + Leading, CRCLen)
          : 0;

  std::string Line;
  llvm::raw_string_ostream OS(Line);
  auto emitHex = [&](size_t Begin, size_t End) {
    for (size_t I = Begin; I < End; ++I)
      OS << llvm::format("%02X", Bytes[I]);
  };

  emitHex(0, Leading);
  OS << llvm::format(" %02X %04X %04X", static_cast<unsigned>(CRCLen), CRC,
                     static_cast<unsigned>(Bytes.size()));
  OS << " :0000 " << Name;
  if (WithTail && Covered < Bytes.size()) {
    OS << " ";
    emitHex(Covered, Bytes.size());
  }
  OS << "\n";
  return Line;
}

/// An image whose only personality routine is 64 unnamed bytes of code.
BinaryImage makeStrippedPersonalityImage(const std::vector<uint8_t> &Bytes) {
  BinaryImage Img = makeImage();
  EXPECT_TRUE(Img.writeVA(kPersonalityVA, Bytes.data(), Bytes.size()));
  addFrame(Img, kTextVA + 0x800, kPersonalityVA);
  return Img;
}

TEST(UnnamedPersonalityRoutines, OffersOnlyTheAddressesTheImageCannotName) {
  BinaryImage Img = makeImage();

  // Already classified: nothing to identify, whether the classification came
  // from a symbol or from a structural proof.
  addSymbol(Img, kTextVA + 0x100, "__gxx_personality_v0");
  addFrame(Img, kTextVA + 0x900, kTextVA + 0x100,
           ExceptionPersonality::GxxPersonalityV0);

  // Named, but by a name the table does not know.  That is still the image's
  // answer, and a better one than a guess: it says the routine is something
  // other than the personalities NeverD models.
  addSymbol(Img, kSecondPersonalityVA, "vendor_specific_handler");
  addFrame(Img, kTextVA + 0x920, kSecondPersonalityVA);

  // Unnamed, and installed by two frames -- which is the normal case, since
  // one routine serves every frame in an image that uses it.
  addFrame(Img, kTextVA + 0x940, kPersonalityVA);
  addFrame(Img, kTextVA + 0x960, kPersonalityVA);

  EXPECT_EQ(collectUnnamedPersonalityRoutines(Img),
            std::vector<va_t>{kPersonalityVA});
}

TEST(UnnamedPersonalityRoutines, RefusesANameThePersonalityTableDoesNotKnow) {
  BinaryImage Img = makeStrippedPersonalityImage(routineBytes(0x11));

  EXPECT_FALSE(adoptPersonalityRoutineName(Img, kPersonalityVA, "memcpy"));
  EXPECT_EQ(Img.ExceptionMetadata.Functions.front().Personality,
            ExceptionPersonality::Unknown);
  EXPECT_TRUE(Img.Symbols.empty());
}

TEST(UnnamedPersonalityRoutines, RefusesToOverruleANameTheImageSpells) {
  BinaryImage Img = makeStrippedPersonalityImage(routineBytes(0x11));
  addSymbol(Img, kPersonalityVA, "vendor_specific_handler");

  EXPECT_FALSE(
      adoptPersonalityRoutineName(Img, kPersonalityVA, "__gxx_personality_v0"));
  EXPECT_EQ(resolveRoutineName(Img, kPersonalityVA), "vendor_specific_handler");
  EXPECT_EQ(Img.ExceptionMetadata.Functions.front().Personality,
            ExceptionPersonality::Unknown);
}

TEST(UnnamedPersonalityRoutines, RefusesAnAddressNoFrameInstalls) {
  BinaryImage Img = makeStrippedPersonalityImage(routineBytes(0x11));

  EXPECT_FALSE(adoptPersonalityRoutineName(Img, kSecondPersonalityVA,
                                           "__gxx_personality_v0"));
  EXPECT_TRUE(Img.Symbols.empty());
}

TEST(UnnamedPersonalityRoutines,
     ReclassifiesEveryFrameThatInstalledTheRoutine) {
  BinaryImage Img = makeStrippedPersonalityImage(routineBytes(0x11));
  addFrame(Img, kTextVA + 0x820, kPersonalityVA);

  ASSERT_TRUE(
      adoptPersonalityRoutineName(Img, kPersonalityVA, "__gxx_personality_v0"));

  for (const ExceptionFunction &F : Img.ExceptionMetadata.Functions) {
    EXPECT_EQ(F.Personality, ExceptionPersonality::GxxPersonalityV0);
    EXPECT_EQ(F.PersonalityName, "__gxx_personality_v0");
    // The name did not come from the file, so a reader comparing this record
    // against the bytes has to be told where it did come from.
    EXPECT_FALSE(F.Diagnostics.empty());
  }
  // Recorded as a symbol too, so the image-wide passes that read the symbol
  // table see the routine rather than only the frames that install it.
  EXPECT_EQ(resolveRoutineName(Img, kPersonalityVA), "__gxx_personality_v0");

  // The address is named now, so a second identification has nothing to add.
  EXPECT_FALSE(
      adoptPersonalityRoutineName(Img, kPersonalityVA, "rust_eh_personality"));
}

TEST(PersonalitySignatures, NamesAStrippedRoutineFromAWholeFunctionMatch) {
  const std::vector<uint8_t> Bytes = routineBytes(0x11);
  BinaryImage Img = makeStrippedPersonalityImage(Bytes);
  ASSERT_EQ(Img.ExceptionMetadata.Functions.front().Personality,
            ExceptionPersonality::Unknown);

  sigs::SignatureDB DB;
  ASSERT_FALSE(DB.loadPatternText(makePatLine(Bytes, "__gxx_personality_v0",
                                              /*Covered=*/32,
                                              /*WithTail=*/true),
                                  "test-eh"));
  ASSERT_EQ(DB.moduleCount(), 1u);

  EXPECT_EQ(DB.identifyPersonalityRoutines(Img), 1u);
  EXPECT_EQ(Img.ExceptionMetadata.Functions.front().Personality,
            ExceptionPersonality::GxxPersonalityV0);
  EXPECT_EQ(resolveRoutineName(Img, kPersonalityVA), "__gxx_personality_v0");

  ASSERT_EQ(DB.matches().size(), 1u);
  EXPECT_EQ(DB.matches().front().Address, kPersonalityVA);
  EXPECT_EQ(DB.matches().front().LibraryName, "test-eh");

  // The routine is in the symbol table now, and that is what the image-wide
  // detection reads -- so an image that could say nothing about itself can.
  EXPECT_EQ(Img.ExceptionMetadata.Runtime.Runtime,
            SourceLanguageRuntime::CxxItanium);
}

TEST(PersonalitySignatures, ReparsesSJLJCallSiteFormAfterIdentification) {
  const std::vector<uint8_t> Personality = routineBytes(0x11);
  BinaryImage Img = makeImage();
  ASSERT_TRUE(
      Img.writeVA(kPersonalityVA, Personality.data(), Personality.size()));

  const va_t FuncVA = kTextVA + 0x800;
  const va_t FrameVA = kDataVA;
  const va_t LSDAVA = kDataVA + 0x400;
  FrameBytes Frame =
      buildSimpleFrame(FrameVA, FuncVA, 0x80, "zPLR", kPersonalityVA, LSDAVA);
  writeData(Img, FrameVA, Frame.Bytes);

  Section EhFrame;
  EhFrame.Name = ".eh_frame";
  EhFrame.VA = FrameVA;
  EhFrame.Size = Frame.Bytes.size();
  EhFrame.Data = Frame.Bytes;
  Img.Sections.push_back(std::move(EhFrame));

  // Two SJLJ entries.  Without a named personality the initial loader has to
  // use the ordinary address form, where these same four ULEBs look like one
  // zero-length range with an invented landing-pad address.
  ByteBuilder LSDA;
  LSDA.u8(0xff); // landing-pad base omitted
  LSDA.u8(0xff); // type table omitted
  LSDA.u8(0x01); // ignored by the SJLJ personality
  LSDA.uleb(4);  // two (selector, action) pairs
  LSDA.uleb(0);
  LSDA.uleb(0);
  LSDA.uleb(1);
  LSDA.uleb(0);
  writeData(Img, LSDAVA, LSDA.data());

  parseItaniumExceptions(Img);

  ASSERT_EQ(Img.ExceptionMetadata.Functions.size(), 1u);
  const ExceptionFunction &Before = Img.ExceptionMetadata.Functions.front();
  EXPECT_EQ(Before.Personality, ExceptionPersonality::Unknown);
  EXPECT_EQ(Before.ParseStatus, ExceptionParseStatus::Partial);
  EXPECT_EQ(Img.ExceptionMetadata.ParseStatus, ExceptionParseStatus::Partial);
  ASSERT_TRUE(Before.Itanium.has_value());
  EXPECT_TRUE(Before.Itanium->IsCallSiteAddressForm);
  ASSERT_EQ(Before.Itanium->CallSites.size(), 1u);
  EXPECT_NE(Before.Itanium->CallSites.front().LandingPadVA, 0u);

  sigs::SignatureDB DB;
  ASSERT_FALSE(DB.loadPatternText(
      makePatLine(Personality, "__gxx_personality_sj0", 32, true), "test-eh"));
  ASSERT_EQ(DB.identifyPersonalityRoutines(Img), 1u);

  const ExceptionFunction &After = Img.ExceptionMetadata.Functions.front();
  EXPECT_EQ(After.Personality, ExceptionPersonality::GxxPersonalitySJ0);
  EXPECT_EQ(After.ParseStatus, ExceptionParseStatus::Complete);
  EXPECT_EQ(Img.ExceptionMetadata.ParseStatus, ExceptionParseStatus::Complete);
  ASSERT_TRUE(After.Itanium.has_value());
  EXPECT_FALSE(After.Itanium->IsCallSiteAddressForm);
  ASSERT_EQ(After.Itanium->CallSites.size(), 2u);
  for (size_t I = 0; I < After.Itanium->CallSites.size(); ++I) {
    const ItaniumCallSite &Site = After.Itanium->CallSites[I];
    EXPECT_EQ(Site.CallSiteIndex, I + 1);
    EXPECT_FALSE(Site.GuardedRange.isValid());
    EXPECT_EQ(Site.LandingPadVA, 0u);
  }
  for (const std::string &Diagnostic : After.Diagnostics)
    EXPECT_EQ(Diagnostic.find("unknown Itanium personality routine"),
              std::string::npos);
}

TEST(PersonalitySignatures, PreservesUnrelatedParseStateDuringRefresh) {
  const std::vector<uint8_t> Personality = routineBytes(0x11);
  BinaryImage Img = makeImage();
  ASSERT_TRUE(
      Img.writeVA(kPersonalityVA, Personality.data(), Personality.size()));

  const va_t FuncVA = kTextVA + 0x800;
  const va_t FrameVA = kDataVA;
  const va_t LSDAVA = kDataVA + 0x400;
  FrameBytes Frame =
      buildSimpleFrame(FrameVA, FuncVA, 0x80, "zPLR", kPersonalityVA, LSDAVA);
  writeData(Img, FrameVA, Frame.Bytes);

  Section EhFrame;
  EhFrame.Name = ".eh_frame";
  EhFrame.VA = FrameVA;
  EhFrame.Size = Frame.Bytes.size();
  EhFrame.Data = Frame.Bytes;
  Img.Sections.push_back(std::move(EhFrame));

  ByteBuilder LSDA;
  LSDA.u8(0xff);
  LSDA.u8(0xff);
  LSDA.u8(0x01);
  LSDA.uleb(4);
  LSDA.uleb(0);
  LSDA.uleb(0);
  LSDA.uleb(1);
  LSDA.uleb(0);
  writeData(Img, LSDAVA, LSDA.data());
  parseItaniumExceptions(Img);

  ExceptionFunction Unrelated;
  Unrelated.CodeRange = {kTextVA + 0x900, kTextVA + 0x920};
  Unrelated.Diagnostics.emplace_back("unrelated structural note");
  Img.ExceptionMetadata.Functions.push_back(Unrelated);
  // This legacy decoder contributes an informational image diagnostic after
  // the Itanium summary was built, but does not make the parse less complete.
  // Capturing the diagnostic must not freeze the old Unknown-personality
  // Partial status into the image-owned structural baseline.
  Img.ExceptionMetadata.Diagnostics.emplace_back("image structural baseline");

  sigs::SignatureDB DB;
  ASSERT_FALSE(DB.loadPatternText(
      makePatLine(Personality, "__gxx_personality_sj0", 32, true), "test-eh"));
  ASSERT_EQ(DB.identifyPersonalityRoutines(Img), 1u);

  ASSERT_EQ(Img.ExceptionMetadata.Functions.size(), 2u);
  const ExceptionFunction &Refreshed = Img.ExceptionMetadata.Functions[0];
  const ExceptionFunction &Preserved = Img.ExceptionMetadata.Functions[1];
  ASSERT_TRUE(Refreshed.Itanium.has_value());
  EXPECT_FALSE(Refreshed.Itanium->IsCallSiteAddressForm);
  EXPECT_EQ(Preserved.CodeRange.Begin, Unrelated.CodeRange.Begin);
  EXPECT_EQ(Preserved.CodeRange.End, Unrelated.CodeRange.End);
  EXPECT_EQ(Preserved.ParseStatus, Unrelated.ParseStatus);
  EXPECT_EQ(Preserved.Diagnostics, Unrelated.Diagnostics);
  EXPECT_EQ(Img.ExceptionMetadata.ParseStatus, ExceptionParseStatus::Complete);
  EXPECT_NE(std::find(Img.ExceptionMetadata.Diagnostics.begin(),
                      Img.ExceptionMetadata.Diagnostics.end(),
                      "image structural baseline"),
            Img.ExceptionMetadata.Diagnostics.end());
}

TEST(PersonalitySignatures, LeavesTheImageUnchangedWhenRefreshCannotDecode) {
  const std::vector<uint8_t> Personality = routineBytes(0x11);
  BinaryImage Img = makeImage();
  ASSERT_TRUE(
      Img.writeVA(kPersonalityVA, Personality.data(), Personality.size()));

  const va_t FuncVA = kTextVA + 0x800;
  const va_t FrameVA = kDataVA;
  const va_t LSDAVA = kDataVA + 0x400;
  FrameBytes Frame =
      buildSimpleFrame(FrameVA, FuncVA, 0x80, "zPLR", kPersonalityVA, LSDAVA);
  writeData(Img, FrameVA, Frame.Bytes);

  Section EhFrame;
  EhFrame.Name = ".eh_frame";
  EhFrame.VA = FrameVA;
  EhFrame.Size = Frame.Bytes.size();
  EhFrame.Data = Frame.Bytes;
  Img.Sections.push_back(std::move(EhFrame));

  ByteBuilder LSDA;
  LSDA.u8(0xff);
  LSDA.u8(0xff);
  LSDA.u8(0x01);
  LSDA.uleb(4);
  LSDA.uleb(0);
  LSDA.uleb(0);
  LSDA.uleb(1);
  LSDA.uleb(0);
  writeData(Img, LSDAVA, LSDA.data());
  parseItaniumExceptions(Img);

  ASSERT_EQ(Img.ExceptionMetadata.Functions.size(), 1u);
  const ExceptionFunction Before = Img.ExceptionMetadata.Functions.front();
  const ExceptionParseStatus ImageStatus = Img.ExceptionMetadata.ParseStatus;
  const std::vector<std::string> ImageDiagnostics =
      Img.ExceptionMetadata.Diagnostics;
  const size_t SymbolCount = Img.Symbols.size();

  // A non-terminating ULEB makes the call-site table length undecodable.
  std::vector<uint8_t> Broken = {0xff, 0xff, 0x01};
  Broken.insert(Broken.end(), 11, 0x80);
  writeData(Img, LSDAVA, Broken);

  sigs::SignatureDB DB;
  ASSERT_FALSE(DB.loadPatternText(
      makePatLine(Personality, "__gxx_personality_sj0", 32, true), "test-eh"));
  EXPECT_EQ(DB.identifyPersonalityRoutines(Img), 0u);

  const ExceptionFunction &After = Img.ExceptionMetadata.Functions.front();
  EXPECT_EQ(After.Personality, Before.Personality);
  EXPECT_EQ(After.PersonalityName, Before.PersonalityName);
  EXPECT_EQ(After.ParseStatus, Before.ParseStatus);
  EXPECT_EQ(After.Diagnostics, Before.Diagnostics);
  EXPECT_EQ(After.HandlerDataVA, Before.HandlerDataVA);
  ASSERT_TRUE(After.Itanium.has_value());
  ASSERT_TRUE(Before.Itanium.has_value());
  EXPECT_EQ(After.Itanium->IsCallSiteAddressForm,
            Before.Itanium->IsCallSiteAddressForm);
  EXPECT_EQ(After.Itanium->CallSites.size(), Before.Itanium->CallSites.size());
  EXPECT_EQ(Img.ExceptionMetadata.ParseStatus, ImageStatus);
  EXPECT_EQ(Img.ExceptionMetadata.Diagnostics, ImageDiagnostics);
  EXPECT_EQ(Img.Symbols.size(), SymbolCount);
  EXPECT_TRUE(resolveRoutineName(Img, kPersonalityVA).empty());
}

TEST(PersonalitySignatures, LeavesAllFramesUnchangedWhenOneRefreshFails) {
  const std::vector<uint8_t> Personality = routineBytes(0x11);
  BinaryImage Img = makeImage();
  ASSERT_TRUE(
      Img.writeVA(kPersonalityVA, Personality.data(), Personality.size()));

  const va_t FuncVA = kTextVA + 0x800;
  const va_t FrameVA = kDataVA;
  const va_t LSDAVA = kDataVA + 0x400;
  FrameBytes Frame =
      buildSimpleFrame(FrameVA, FuncVA, 0x80, "zPLR", kPersonalityVA, LSDAVA);
  writeData(Img, FrameVA, Frame.Bytes);

  Section EhFrame;
  EhFrame.Name = ".eh_frame";
  EhFrame.VA = FrameVA;
  EhFrame.Size = Frame.Bytes.size();
  EhFrame.Data = Frame.Bytes;
  Img.Sections.push_back(std::move(EhFrame));

  ByteBuilder LSDA;
  LSDA.u8(0xff);
  LSDA.u8(0xff);
  LSDA.u8(0x01);
  LSDA.uleb(4);
  LSDA.uleb(0);
  LSDA.uleb(0);
  LSDA.uleb(1);
  LSDA.uleb(0);
  writeData(Img, LSDAVA, LSDA.data());
  parseItaniumExceptions(Img);

  ASSERT_EQ(Img.ExceptionMetadata.Functions.size(), 1u);
  ExceptionFunction Unmapped = Img.ExceptionMetadata.Functions.front();
  Unmapped.CodeRange = {kTextVA + 0x900, kTextVA + 0x980};
  Unmapped.HandlerDataVA = kDataVA + 0x2000;
  Img.ExceptionMetadata.Functions.push_back(std::move(Unmapped));
  Img.ExceptionMetadata.rebuildIndex();
  Img.ExceptionMetadata.rebuildParseSummary();

  const std::vector<ExceptionFunction> Before = Img.ExceptionMetadata.Functions;
  const ExceptionParseStatus ImageStatus = Img.ExceptionMetadata.ParseStatus;
  const std::vector<std::string> ImageDiagnostics =
      Img.ExceptionMetadata.Diagnostics;
  const size_t SymbolCount = Img.Symbols.size();

  sigs::SignatureDB DB;
  ASSERT_FALSE(DB.loadPatternText(
      makePatLine(Personality, "__gxx_personality_sj0", 32, true), "test-eh"));
  EXPECT_EQ(DB.identifyPersonalityRoutines(Img), 0u);

  ASSERT_EQ(Img.ExceptionMetadata.Functions.size(), Before.size());
  for (size_t I = 0; I < Before.size(); ++I) {
    const ExceptionFunction &After = Img.ExceptionMetadata.Functions[I];
    EXPECT_EQ(After.CodeRange.Begin, Before[I].CodeRange.Begin);
    EXPECT_EQ(After.CodeRange.End, Before[I].CodeRange.End);
    EXPECT_EQ(After.HandlerDataVA, Before[I].HandlerDataVA);
    EXPECT_EQ(After.Personality, Before[I].Personality);
    EXPECT_EQ(After.PersonalityName, Before[I].PersonalityName);
    EXPECT_EQ(After.ParseStatus, Before[I].ParseStatus);
    EXPECT_EQ(After.Diagnostics, Before[I].Diagnostics);
    ASSERT_TRUE(After.Itanium.has_value());
    ASSERT_TRUE(Before[I].Itanium.has_value());
    EXPECT_EQ(After.Itanium->IsCallSiteAddressForm,
              Before[I].Itanium->IsCallSiteAddressForm);
    EXPECT_EQ(After.Itanium->CallSites.size(),
              Before[I].Itanium->CallSites.size());
  }
  EXPECT_EQ(Img.ExceptionMetadata.ParseStatus, ImageStatus);
  EXPECT_EQ(Img.ExceptionMetadata.Diagnostics, ImageDiagnostics);
  EXPECT_EQ(Img.Symbols.size(), SymbolCount);
  EXPECT_TRUE(resolveRoutineName(Img, kPersonalityVA).empty());
}

TEST(PersonalitySignatures, RefusesASignatureThatCoversOnlyTheStart) {
  const std::vector<uint8_t> Bytes = routineBytes(0x11);
  BinaryImage Img = makeStrippedPersonalityImage(Bytes);

  // Half the routine checked, the other half asserted by omission.  Two
  // routines sharing a prologue are the same thing to a signature like this,
  // and a personality decides which schema the language data is read with.
  sigs::SignatureDB DB;
  ASSERT_FALSE(DB.loadPatternText(makePatLine(Bytes, "__gxx_personality_v0",
                                              /*Covered=*/32,
                                              /*WithTail=*/false),
                                  "test-eh"));
  ASSERT_EQ(DB.moduleCount(), 1u);

  EXPECT_EQ(DB.identifyPersonalityRoutines(Img), 0u);
  EXPECT_EQ(Img.ExceptionMetadata.Functions.front().Personality,
            ExceptionPersonality::Unknown);
}

TEST(PersonalitySignatures, RefusesANameThatIsNotAPersonality) {
  const std::vector<uint8_t> Bytes = routineBytes(0x11);
  BinaryImage Img = makeStrippedPersonalityImage(Bytes);

  sigs::SignatureDB DB;
  ASSERT_FALSE(DB.loadPatternText(
      makePatLine(Bytes, "deflate_stored", /*Covered=*/32, /*WithTail=*/true),
      "test-zlib"));

  EXPECT_EQ(DB.identifyPersonalityRoutines(Img), 0u);
  EXPECT_EQ(Img.ExceptionMetadata.Functions.front().Personality,
            ExceptionPersonality::Unknown);
  EXPECT_TRUE(Img.Symbols.empty());
}

TEST(PersonalitySignatures, RefusesAnAddressTwoSignaturesNameDifferently) {
  const std::vector<uint8_t> Bytes = routineBytes(0x11);
  BinaryImage Img = makeStrippedPersonalityImage(Bytes);

  // Both describe the routine equally well and disagree about what it is.
  // Either could be picked and one of them would be wrong, so neither is.
  sigs::SignatureDB DB;
  ASSERT_FALSE(DB.loadPatternText(
      makePatLine(Bytes, "__gxx_personality_v0", 32, true), "test-libstdcxx"));
  ASSERT_FALSE(DB.loadPatternText(
      makePatLine(Bytes, "rust_eh_personality", 32, true), "test-rust"));
  ASSERT_EQ(DB.moduleCount(), 2u);

  EXPECT_EQ(DB.identifyPersonalityRoutines(Img), 0u);
  EXPECT_EQ(Img.ExceptionMetadata.Functions.front().Personality,
            ExceptionPersonality::Unknown);
}

TEST(PersonalitySignatures, LeavesAnImageThatNamesItsOwnRoutineAlone) {
  const std::vector<uint8_t> Bytes = routineBytes(0x11);
  BinaryImage Img = makeStrippedPersonalityImage(Bytes);
  addSymbol(Img, kPersonalityVA, "vendor_specific_handler");

  sigs::SignatureDB DB;
  ASSERT_FALSE(DB.loadPatternText(
      makePatLine(Bytes, "__gxx_personality_v0", 32, true), "test-eh"));

  EXPECT_EQ(DB.identifyPersonalityRoutines(Img), 0u);
  EXPECT_EQ(resolveRoutineName(Img, kPersonalityVA), "vendor_specific_handler");
}

TEST(PersonalitySignatures, AddsToTheOrdinaryMatchesRatherThanReplacingThem) {
  const std::vector<uint8_t> Personality = routineBytes(0x11);
  const std::vector<uint8_t> Ordinary = routineBytes(0x40);
  BinaryImage Img = makeStrippedPersonalityImage(Personality);
  ASSERT_TRUE(
      Img.writeVA(kSecondPersonalityVA, Ordinary.data(), Ordinary.size()));

  sigs::SignatureDB DB;
  ASSERT_FALSE(DB.loadPatternText(
      makePatLine(Personality, "__gxx_personality_v0", 32, true), "test-eh"));
  ASSERT_FALSE(DB.loadPatternText(
      makePatLine(Ordinary, "deflate_stored", 32, true), "test-zlib"));

  // The order the two passes run in is not interchangeable: `apply` begins by
  // clearing the match list, so identifying personalities first would report
  // nothing.  Running second also puts the adopted name in front of the name
  // map, which is how the routine is renamed in a function listing and not
  // only in the frames that installed it.
  DB.apply(Img, {kSecondPersonalityVA});
  ASSERT_EQ(DB.matches().size(), 1u);
  EXPECT_EQ(DB.identifyPersonalityRoutines(Img), 1u);

  ASSERT_EQ(DB.matches().size(), 2u);
  const sigs::SigMatch *Plain = DB.findMatch(kSecondPersonalityVA);
  const sigs::SigMatch *Routine = DB.findMatch(kPersonalityVA);
  ASSERT_NE(Plain, nullptr);
  ASSERT_NE(Routine, nullptr);
  EXPECT_EQ(Plain->Name, "deflate_stored");
  EXPECT_EQ(Routine->Name, "__gxx_personality_v0");
}

} // namespace
