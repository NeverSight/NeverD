//===- LanguageEHRustTests.cpp - Rust landing pad and mangling tests --===//
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

/// Build the `.gcc_except_table` shape rustc emits for a function that owns a
/// value needing `Drop`, calls `catch_unwind`, and crosses an `extern "C"`
/// boundary.  All three pad kinds appear, which is the whole point: they are
/// spelled with the same structures C++ uses and are told apart only by what
/// the action chain selects.
std::vector<uint8_t> buildRustLSDA() {
  ByteBuilder B;
  B.u8(0xff); // landing pad base defaults to the function start
  B.u8(0x00); // DW_EH_PE_absptr type table entries

  ByteBuilder Body;
  {
    ByteBuilder CallSites;
    // Cleanup only: a landing pad with no action runs `Drop` glue and resumes.
    CallSites.u32(0x10);
    CallSites.u32(0x10);
    CallSites.u32(0x40);
    CallSites.uleb(0);
    // `catch_unwind`: names the action at table offset 0, a catch on the null
    // type-table slot.
    CallSites.u32(0x20);
    CallSites.u32(0x10);
    CallSites.u32(0x50);
    CallSites.uleb(1);
    // Nounwind boundary: names the action at table offset 2, an empty filter.
    CallSites.u32(0x30);
    CallSites.u32(0x10);
    CallSites.u32(0x60);
    CallSites.uleb(3);

    Body.u8(0x03); // DW_EH_PE_udata4 call sites
    Body.uleb(CallSites.size());
    for (uint8_t Byte : CallSites.data())
      Body.u8(Byte);

    // Offset 0: catch type-table slot 1, end of chain.
    Body.sleb(1);
    Body.sleb(0);
    // Offset 2: exception-specification list 1, end of chain.
    Body.sleb(-1);
    Body.sleb(0);
  }

  // One 8-byte type-table entry below the base, and the specification list
  // above it.
  const size_t TypeTableBaseOffset = Body.size() + 8;
  B.uleb(TypeTableBaseOffset);
  const size_t AfterOffsetField = B.size();
  for (uint8_t Byte : Body.data())
    B.u8(Byte);
  B.u64(0); // slot 1: a null `std::type_info *`, which is the catch-all
  EXPECT_EQ(AfterOffsetField + TypeTableBaseOffset, B.size());
  B.uleb(0); // specification list 1: empty, so nothing may propagate
  return B.data();
}

/// Assemble an image whose single function carries the Rust LSDA above, with
/// \p PanicName defined at \p PanicVA and a direct call to it.
BinaryImage makeRustImage(va_t FuncVA, va_t PanicVA, const char *PanicName) {
  BinaryImage Img = makeImage();
  const va_t SectionVA = kDataVA;
  const va_t PersonalityVA = kTextVA + 0x800;
  const va_t LSDAVA = kDataVA + 0x400;

  Symbol Personality;
  Personality.Name = "rust_eh_personality";
  Personality.Addr = PersonalityVA;
  Personality.IsFunc = true;
  Img.Symbols.push_back(std::move(Personality));

  Symbol Core;
  Core.Name = "_ZN4core9panicking5panic17h0123456789abcdefE";
  Core.Addr = kTextVA + 0x900;
  Core.IsFunc = true;
  Img.Symbols.push_back(std::move(Core));

  Symbol Panic;
  Panic.Name = PanicName;
  Panic.Addr = PanicVA;
  Panic.IsFunc = true;
  Img.Symbols.push_back(std::move(Panic));

  FrameBytes Frame = buildSimpleFrame(SectionVA, FuncVA, 0x80, "zPLR",
                                      PersonalityVA, LSDAVA);
  writeData(Img, SectionVA, Frame.Bytes);

  Section EhFrame;
  EhFrame.Name = ".eh_frame";
  EhFrame.VA = SectionVA;
  EhFrame.Size = Frame.Bytes.size();
  EhFrame.Data = Frame.Bytes;
  Img.Sections.push_back(std::move(EhFrame));

  writeData(Img, LSDAVA, buildRustLSDA());

  // A direct `call rel32` at the start of the function body.
  ByteBuilder Call;
  Call.u8(0xe8);
  Call.i32(static_cast<int32_t>(static_cast<int64_t>(PanicVA) -
                               static_cast<int64_t>(FuncVA + 5)));
  writeData(Img, FuncVA, Call.data());
  return Img;
}

TEST(RustEH, ClassifiesEveryLandingPadKindItSharesWithCxx) {
  const va_t FuncVA = kTextVA + 0x100;
  BinaryImage Img =
      makeRustImage(FuncVA, kTextVA + 0x900,
                    "_ZN4core9panicking5panic17h0123456789abcdefE");
  parseItaniumExceptions(Img);
  rust_eh::parseRustExceptions(Img);

  ASSERT_EQ(Img.ExceptionMetadata.Functions.size(), 1u);
  const ExceptionFunction &F = Img.ExceptionMetadata.Functions[0];
  ASSERT_TRUE(F.Rust.has_value());
  ASSERT_EQ(F.Rust->LandingPads.size(), 3u);
  EXPECT_EQ(F.Rust->LandingPads[0].Kind, RustLandingPadKind::DropGlue);
  EXPECT_EQ(F.Rust->LandingPads[0].PadVA, FuncVA + 0x40);
  EXPECT_EQ(F.Rust->LandingPads[1].Kind, RustLandingPadKind::CatchUnwind);
  EXPECT_EQ(F.Rust->LandingPads[1].PadVA, FuncVA + 0x50);
  EXPECT_EQ(F.Rust->LandingPads[2].Kind, RustLandingPadKind::NoUnwindGuard);
  EXPECT_EQ(F.Rust->LandingPads[2].PadVA, FuncVA + 0x60);

  EXPECT_TRUE(F.Rust->runsDropGlue());
  EXPECT_TRUE(F.Rust->catchesUnwind());
  EXPECT_TRUE(F.Rust->guardsAgainstUnwind());
  EXPECT_FALSE(F.Rust->UsesMSVCTables);

  ASSERT_TRUE(Img.ExceptionMetadata.RustRuntime.has_value());
  const RustRuntimeInfo &RT = *Img.ExceptionMetadata.RustRuntime;
  EXPECT_EQ(RT.Strategy, RustPanicStrategy::Unwind);
  EXPECT_EQ(RT.CleanupFrames, 1u);
  EXPECT_EQ(RT.CatchUnwindFrames, 1u);
  EXPECT_EQ(RT.NoUnwindGuardFrames, 1u);
  EXPECT_FALSE(RT.UsesMSVCUnwinding);
}

TEST(RustEH, ClassifiesPanicSitesByWhatTheCheckIs) {
  struct Case {
    const char *Symbol;
    RustPanicKind Kind;
  } const Cases[] = {
      {"_ZN4core9panicking5panic17h0123456789abcdefE",
       RustPanicKind::Explicit},
      {"_ZN4core9panicking18panic_bounds_check17h0123456789abcdefE",
       RustPanicKind::BoundsCheck},
      {"_ZN4core9panicking11panic_const23panic_const_div_by_zero17h012345678"
       "9abcdefE",
       RustPanicKind::Arithmetic},
      {"_ZN4core9panicking19panic_cannot_unwind17h0123456789abcdefE",
       RustPanicKind::NoUnwind},
      {"_Unwind_Resume", RustPanicKind::Resume},
  };

  for (const Case &C : Cases) {
    const va_t FuncVA = kTextVA + 0x100;
    const va_t PanicVA = kTextVA + 0x940;
    BinaryImage Img = makeRustImage(FuncVA, PanicVA, C.Symbol);
    parseItaniumExceptions(Img);
    rust_eh::parseRustExceptions(Img);

    ASSERT_EQ(Img.ExceptionMetadata.Functions.size(), 1u) << C.Symbol;
    const ExceptionFunction &F = Img.ExceptionMetadata.Functions[0];
    ASSERT_TRUE(F.Rust.has_value()) << C.Symbol;
    ASSERT_EQ(F.Rust->Panics.size(), 1u) << C.Symbol;
    EXPECT_EQ(F.Rust->Panics[0].Kind, C.Kind) << C.Symbol;
    EXPECT_EQ(F.Rust->Panics[0].CallVA, FuncVA) << C.Symbol;
    EXPECT_EQ(F.Rust->Panics[0].TargetVA, PanicVA) << C.Symbol;
  }
}

TEST(RustEH, DoesNotTreatPanicBookkeepingAsAPanicOrigin) {
  // `std::panicking` is full of helpers that inspect a panic already in
  // flight.  Matching the module rather than the function would put a raise
  // edge on every one of them.
  const va_t FuncVA = kTextVA + 0x100;
  BinaryImage Img = makeRustImage(
      FuncVA, kTextVA + 0x940,
      "_ZN3std9panicking12catch_unwind7cleanup17h0123456789abcdefE");
  parseItaniumExceptions(Img);
  rust_eh::parseRustExceptions(Img);

  ASSERT_EQ(Img.ExceptionMetadata.Functions.size(), 1u);
  const ExceptionFunction &F = Img.ExceptionMetadata.Functions[0];
  ASSERT_TRUE(F.Rust.has_value());
  EXPECT_TRUE(F.Rust->Panics.empty());
}

TEST(RustEH, LeavesANonRustFrameAlone) {
  // The same tables under the C++ personality mean `catch (...)` and a
  // `throw()` specification, which are not Rust's semantics at all.
  const va_t FuncVA = kTextVA + 0x100;
  BinaryImage Img = makeRustImage(
      FuncVA, kTextVA + 0x940,
      "_ZN4core9panicking5panic17h0123456789abcdefE");
  for (Symbol &S : Img.Symbols)
    if (S.Name == "rust_eh_personality")
      S.Name = "__gxx_personality_v0";
  parseItaniumExceptions(Img);
  rust_eh::parseRustExceptions(Img);

  ASSERT_EQ(Img.ExceptionMetadata.Functions.size(), 1u);
  EXPECT_FALSE(Img.ExceptionMetadata.Functions[0].Rust.has_value());
  EXPECT_TRUE(Img.ExceptionMetadata.Functions[0].Itanium.has_value());
}

TEST(RustEH, ReadsAPanicAbortImageAsAborting) {
  // Nothing that can raise or continue an unwind, and no landing pad: the
  // image cannot have been built to unwind whatever else it contains.
  BinaryImage Img = makeImage();
  Symbol Core;
  Core.Name = "_ZN4core9panicking5panic17h0123456789abcdefE";
  Core.Addr = kTextVA + 0x900;
  Core.IsFunc = true;
  Img.Symbols.push_back(std::move(Core));

  rust_eh::parseRustExceptions(Img);
  ASSERT_TRUE(Img.ExceptionMetadata.RustRuntime.has_value());
  EXPECT_EQ(Img.ExceptionMetadata.RustRuntime->Strategy,
            RustPanicStrategy::Abort);
}

TEST(RustEH, IgnoresAnImageWithoutTheRustRuntime) {
  BinaryImage Img = makeImage();
  rust_eh::parseRustExceptions(Img);
  EXPECT_FALSE(Img.ExceptionMetadata.RustRuntime.has_value());
}

// A PE executable keeps its names in a PDB, so a Rust image built for MSVC
// reaches this pass with no Rust symbol to find -- and MSVC is the target
// where Rust needs recognizing most, because its frames use the same
// `__CxxFrameHandler3` tables as C++.  Whatever the image-wide detection
// already concluded has to count, or the whole target goes unclassified.
TEST(RustEH, TrustsTheImageWideDetectionWhenNoSymbolSurvives) {
  BinaryImage Img = makeImage();
  Img.ExceptionMetadata.Runtime.Runtime = SourceLanguageRuntime::Rust;
  Img.ExceptionMetadata.Runtime.Evidence.push_back("rust standard library path");

  EXPECT_TRUE(rust_eh::hasRustRuntime(Img));
  rust_eh::parseRustExceptions(Img);
  EXPECT_TRUE(Img.ExceptionMetadata.RustRuntime.has_value());
}

TEST(RustEH, TrustsRustAsASecondaryRuntimeToo) {
  // A `cdylib` linked into a C++ program leaves both runtimes' evidence, and
  // which one detection calls primary depends on how much of each it found.
  BinaryImage Img = makeImage();
  Img.ExceptionMetadata.Runtime.Runtime = SourceLanguageRuntime::CxxMSVC;
  Img.ExceptionMetadata.Runtime.IsMixed = true;
  Img.ExceptionMetadata.Runtime.SecondaryRuntimes.push_back(
      SourceLanguageRuntime::Rust);

  EXPECT_TRUE(rust_eh::hasRustRuntime(Img));
}
TEST(RustMangling, RecognizesBothManglingSchemes) {
  EXPECT_TRUE(
      isRustMangledName("_ZN4core3fmt9Formatter3pad17h0123456789abcdefE"));
  EXPECT_TRUE(isRustMangledName("_RNvCs1234_4core3foo"));
  EXPECT_FALSE(isRustMangledName("_ZNSt6vectorIiE9push_backERKi"));
  EXPECT_FALSE(isRustMangledName("plain_c_symbol"));
}

TEST(RustMangling, StripsTheLegacyDisambiguatorHash) {
  const std::string Demangled =
      demangleRustName("_ZN4core3fmt9Formatter3pad17h0123456789abcdefE");
  EXPECT_EQ(Demangled, "core::fmt::Formatter::pad");
}

TEST(RustMangling, DemanglesV0Symbols) {
  // Taken from a `-C symbol-mangling-version=v0` build of a crate named `v0`.
  EXPECT_EQ(demangleRustName("_RNvCsjH1N12swBG2_2v04main"), "v0::main");
  // The same symbol as Darwin spells it, with the platform underscore added.
  EXPECT_EQ(demangleRustName("__RNvCsjH1N12swBG2_2v04main"), "v0::main");
  EXPECT_EQ(demangleRustName("_RNvCskdKJRKLKjqM_7___rustc17rust_begin_unwind"),
            "__rustc::rust_begin_unwind");
}

TEST(RustMangling, LeavesNonRustNamesAlone) {
  EXPECT_TRUE(demangleRustName("_ZNSt6vectorIiE9push_backERKi").empty());
  EXPECT_TRUE(demangleRustName("main").empty());
}

} // namespace
