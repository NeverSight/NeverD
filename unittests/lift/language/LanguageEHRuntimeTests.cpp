//===- LanguageEHRuntimeTests.cpp - Language runtime identity tests ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "LanguageEHTestsDetail.h"

namespace {

using namespace neverd;
using namespace neverd::dwarf_eh;
using namespace neverd::language_eh_test;

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

} // namespace
