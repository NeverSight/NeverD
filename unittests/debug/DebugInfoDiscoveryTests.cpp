//===- DebugInfoDiscoveryTests.cpp - Debug symbol precedence -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Pins the order in which NeverD believes competing sources of a function
/// name, and the search that decides which debug file an image is analyzed
/// with.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/Common.h"
#include "neverd/debug/DebugInfoDiscovery.h"
#include "neverd/loader/BinaryImage.h"

#include "llvm/Support/FileSystem.h"
#include "llvm/Support/raw_ostream.h"

#include <filesystem>
#include <fstream>

using namespace neverd;

namespace {

/// A DebugContext holding exactly the functions a test hands it, so the
/// precedence rules can be exercised without a real DWARF/PDB/MAP file.
class FakeDebugContext : public DebugContext {
public:
  explicit FakeDebugContext(std::vector<FunctionSym> Funcs)
      : Functions(std::move(Funcs)) {}

  std::optional<FunctionSym> resolveFunction(va_t Addr) const override {
    for (const FunctionSym &FS : Functions)
      if (FS.Addr == Addr)
        return FS;
    return std::nullopt;
  }
  std::optional<VariableSym> resolveVariable(va_t, int64_t) const override {
    return std::nullopt;
  }
  std::optional<TypeSym> resolveType(uint64_t) const override {
    return std::nullopt;
  }
  std::optional<SourceLoc> sourceLocation(va_t) const override {
    return std::nullopt;
  }
  std::vector<FunctionSym> allFunctions() const override { return Functions; }
  bool hasInfo() const override { return !Functions.empty(); }

private:
  std::vector<FunctionSym> Functions;
};

FunctionSym makeDebugFunc(va_t Addr, llvm::StringRef Name, uint64_t Size = 0) {
  FunctionSym FS;
  FS.Addr = Addr;
  FS.Name = Name.str();
  FS.Size = Size;
  return FS;
}

Symbol makeNamedFunc(va_t Addr, llvm::StringRef Name, uint64_t Size = 0) {
  Symbol S;
  S.Addr = Addr;
  S.Name = Name.str();
  S.Size = Size;
  S.IsFunc = true;
  return S;
}

const Symbol *findFunc(const BinaryImage &Img, va_t Addr) {
  for (const Symbol &S : Img.Symbols)
    if (S.IsFunc && S.Addr == Addr)
      return &S;
  return nullptr;
}

/// A scratch directory that removes itself, so a discovery test can lay out
/// the companion files an image is expected to be found next to.
class ScratchDir {
public:
  ScratchDir() {
    llvm::SmallString<128> Path;
    llvm::sys::fs::createUniqueDirectory("neverd-debug-discovery", Path);
    Root = std::filesystem::path(Path.str().str());
  }
  ~ScratchDir() {
    std::error_code EC;
    std::filesystem::remove_all(Root, EC);
  }

  std::filesystem::path write(llvm::StringRef Name, llvm::StringRef Content) {
    std::filesystem::path P = Root / Name.str();
    std::ofstream OS(P);
    OS << Content.str();
    return P;
  }

  std::filesystem::path path(llvm::StringRef Name) const {
    return Root / Name.str();
  }

private:
  std::filesystem::path Root;
};

/// A link.exe /MAP naming two functions in a PE whose image base is
/// 0x140000000.
constexpr llvm::StringLiteral kMSVCMap =
    " probe\n"
    "\n"
    " Preferred load address is 0000000140000000\n"
    "\n"
    " Start         Length     Name                   Class\n"
    " 0001:00000000 00001000H .text                   CODE\n"
    "\n"
    "  Address         Publics by Value        Rva+Base       Lib:Object\n"
    "\n"
    " 0001:00001000       parse_header            0000140001000 f   probe.obj\n"
    " 0001:00001040       emit_record             0000140001040 f   probe.obj\n"
    "\n"
    " entry point at        0001:00001000\n";

/// An lld-link /lldmap whose object file sits in a dot-prefixed build
/// directory, with one symbol in code and one in read-only data.
constexpr llvm::StringLiteral kCOFFLLDMap =
    "Address  Size     Align Out     In      Symbol\n"
    "00001000 00000080  4096 .text\n"
    "00001000 00000080    16         .build/probe.obj:(.text)\n"
    "00001000 00000000     0                 parse_header\n"
    "00001040 00000000     0                 emit_record\n"
    "00002000 00000200  4096 .rdata\n"
    "00002000 00000008     1         .build/probe.obj:(.rdata)\n"
    "00002000 00000000     0                 lookup_table\n";

constexpr llvm::StringLiteral kELFLLDMap =
    "VMA LMA Size Align Out In Symbol\n"
    "00001000 00001000 00000080 16 .text\n"
    "00001000 00001000 00000080 16 .build/probe.o:(.text)\n"
    "00001000 00001000 00000020 1 parse_header\n"
    "00001040 00001040 00000020 1 emit_record\n"
    "00002000 00002000 00000200 16 .rodata\n"
    "00002000 00002000 00000008 1 .build/probe.o:(.rodata)\n"
    "00002000 00002000 00000008 1 lookup_table\n";

BinaryImage makePEImage() {
  BinaryImage Img;
  Img.Format = BinaryFormat::COFF;
  Img.Arch = Arch::X64;
  Img.Bits = Bitness::Bits64;
  Img.Base = 0x140000000;
  return Img;
}

//===----------------------------------------------------------------------===//
// isSynthesizedFuncName — the predicate the whole precedence chain rests on
//===----------------------------------------------------------------------===//

TEST(NameOriginTest, PlaceholdersAreSynthesized) {
  EXPECT_TRUE(isSynthesizedFuncName(""));
  EXPECT_TRUE(isSynthesizedFuncName("sub_140001000"));
  EXPECT_TRUE(isSynthesizedFuncName("sub_1A2B"));
  EXPECT_TRUE(isSynthesizedFuncName("func_a9059cbb"));
}

TEST(NameOriginTest, RealNamesAreNotSynthesized) {
  EXPECT_FALSE(isSynthesizedFuncName("main"));
  EXPECT_FALSE(isSynthesizedFuncName("_ZN3foo3barEv"));
  EXPECT_FALSE(isSynthesizedFuncName("?bar@foo@@QEAAXXZ"));
}

// A binary is free to export something spelled like a placeholder.  Requiring
// the exact `<prefix><hex>` shape is what keeps such a name from being treated
// as up for grabs by a signature match.
TEST(NameOriginTest, PlaceholderLookalikesKeepTheirName) {
  EXPECT_FALSE(isSynthesizedFuncName("sub_total"));
  EXPECT_FALSE(isSynthesizedFuncName("sub_"));
  EXPECT_FALSE(isSynthesizedFuncName("func_ptr"));
  EXPECT_FALSE(isSynthesizedFuncName("sub_1000_thunk"));
  EXPECT_FALSE(isSynthesizedFuncName("subtract"));
}

//===----------------------------------------------------------------------===//
// applyDebugSymbols — debug info is level with the image, above a placeholder
//===----------------------------------------------------------------------===//

TEST(ApplyDebugSymbolsTest, ReplacesPlaceholderAndFillsSize) {
  BinaryImage Img;
  Img.Symbols.push_back(makeNamedFunc(0x1000, "sub_1000"));

  FakeDebugContext Dbg({makeDebugFunc(0x1000, "parse_header", 64)});
  EXPECT_EQ(applyDebugSymbols(Img, Dbg), 1u);

  const Symbol *S = findFunc(Img, 0x1000);
  ASSERT_NE(S, nullptr);
  EXPECT_EQ(S->Name, "parse_header");
  EXPECT_EQ(S->Size, 64u);
}

TEST(ApplyDebugSymbolsTest, ImageNameOutranksDebugName) {
  BinaryImage Img;
  Img.Symbols.push_back(makeNamedFunc(0x1000, "shipped_name", 32));

  FakeDebugContext Dbg({makeDebugFunc(0x1000, "stale_name", 64)});
  EXPECT_EQ(applyDebugSymbols(Img, Dbg), 0u);

  const Symbol *S = findFunc(Img, 0x1000);
  ASSERT_NE(S, nullptr);
  EXPECT_EQ(S->Name, "shipped_name");
  EXPECT_EQ(S->Size, 32u);
}

TEST(ApplyDebugSymbolsTest, AddsFunctionsTheImageDoesNotDescribe) {
  BinaryImage Img;
  Img.Symbols.push_back(makeNamedFunc(0x1000, "main", 16));

  FakeDebugContext Dbg({makeDebugFunc(0x2000, "static_helper", 48)});
  EXPECT_EQ(applyDebugSymbols(Img, Dbg), 1u);

  const Symbol *S = findFunc(Img, 0x2000);
  ASSERT_NE(S, nullptr);
  EXPECT_EQ(S->Name, "static_helper");
  EXPECT_EQ(S->Size, 48u);
  EXPECT_TRUE(S->IsFunc);
}

// Discovery passes mint a placeholder for code the symbol table also names, so
// an address can carry two entries.  The stated one is what decides.
TEST(ApplyDebugSymbolsTest, StatedEntryDecidesWhenAnAddressCarriesBoth) {
  BinaryImage Img;
  Img.Symbols.push_back(makeNamedFunc(0x1000, "sub_1000"));
  Img.Symbols.push_back(makeNamedFunc(0x1000, "shipped_name", 32));

  FakeDebugContext Dbg({makeDebugFunc(0x1000, "stale_name", 64)});
  EXPECT_EQ(applyDebugSymbols(Img, Dbg), 0u);

  for (const Symbol &S : Img.Symbols)
    EXPECT_NE(S.Name, "stale_name");
}

TEST(ApplyDebugSymbolsTest, IgnoresUnusableDebugEntries) {
  BinaryImage Img;
  FakeDebugContext Dbg(
      {makeDebugFunc(0x1000, ""), makeDebugFunc(0, "at_zero")});

  EXPECT_EQ(applyDebugSymbols(Img, Dbg), 0u);
  EXPECT_TRUE(Img.Symbols.empty());
}

//===----------------------------------------------------------------------===//
// loadDebugInfo — which file an image is analyzed with
//===----------------------------------------------------------------------===//

TEST(LoadDebugInfoTest, FindsMapBesideTheBinary) {
  ScratchDir Dir;
  Dir.write("probe.map", kMSVCMap);

  BinaryImage Img = makePEImage();
  DebugInfoResult R = loadDebugInfo(Dir.path("probe.exe"), Img);

  ASSERT_TRUE(static_cast<bool>(R));
  EXPECT_EQ(R.Kind, DebugInfoKind::Map);
  EXPECT_EQ(R.Path.filename().string(), "probe.map");

  auto Funcs = R.Context->allFunctions();
  ASSERT_EQ(Funcs.size(), 2u);
  EXPECT_EQ(Funcs[0].Name, "parse_header");
  EXPECT_EQ(Funcs[0].Addr, Img.Base + 0x1000);
  EXPECT_EQ(Funcs[1].Name, "emit_record");
  EXPECT_EQ(Funcs[1].Addr, Img.Base + 0x1040);
}

TEST(LoadDebugInfoTest, FindsMapNamedAfterTheWholeFileName) {
  ScratchDir Dir;
  Dir.write("probe.exe.map", kMSVCMap);

  BinaryImage Img = makePEImage();
  DebugInfoResult R = loadDebugInfo(Dir.path("probe.exe"), Img);

  ASSERT_TRUE(static_cast<bool>(R));
  EXPECT_EQ(R.Path.filename().string(), "probe.exe.map");
}

TEST(LoadDebugInfoTest, ReportsNothingWhenNoCompanionFileExists) {
  ScratchDir Dir;

  BinaryImage Img = makePEImage();
  DebugInfoResult R = loadDebugInfo(Dir.path("probe.exe"), Img);

  EXPECT_FALSE(static_cast<bool>(R));
  EXPECT_EQ(R.Kind, DebugInfoKind::None);
  // An absent companion file is an ordinary outcome, not a failure.
  EXPECT_TRUE(R.Error.empty());
}

TEST(LoadDebugInfoTest, DisabledSearchIgnoresAnAvailableMap) {
  ScratchDir Dir;
  Dir.write("probe.map", kMSVCMap);

  DebugInfoRequest Req;
  Req.Enabled = false;

  BinaryImage Img = makePEImage();
  DebugInfoResult R = loadDebugInfo(Dir.path("probe.exe"), Img, Req);

  EXPECT_FALSE(static_cast<bool>(R));
  EXPECT_EQ(R.Kind, DebugInfoKind::None);
}

TEST(LoadDebugInfoTest, ExplicitMapIsUsedOverTheOneBesideTheBinary) {
  ScratchDir Dir;
  Dir.write("probe.map", " Address         Publics by Value\n");
  std::filesystem::path Chosen = Dir.write("elsewhere.map", kMSVCMap);

  DebugInfoRequest Req;
  Req.MapPath = Chosen;

  BinaryImage Img = makePEImage();
  DebugInfoResult R = loadDebugInfo(Dir.path("probe.exe"), Img, Req);

  ASSERT_TRUE(static_cast<bool>(R));
  EXPECT_EQ(R.Path.filename().string(), "elsewhere.map");
}

// Someone who names a file wants to hear that it was the wrong one, rather
// than get a silent fallback to whatever happened to be lying around.
TEST(LoadDebugInfoTest, MissingExplicitMapIsAnErrorNotAFallback) {
  ScratchDir Dir;
  Dir.write("probe.map", kMSVCMap);

  DebugInfoRequest Req;
  Req.MapPath = Dir.path("absent.map");

  BinaryImage Img = makePEImage();
  DebugInfoResult R = loadDebugInfo(Dir.path("probe.exe"), Img, Req);

  EXPECT_FALSE(static_cast<bool>(R));
  EXPECT_FALSE(R.Error.empty());
}

TEST(LoadDebugInfoTest, ExplicitMapWithoutFunctionsIsAnError) {
  ScratchDir Dir;
  std::filesystem::path Empty = Dir.write("empty.map", "nothing here\n");

  DebugInfoRequest Req;
  Req.MapPath = Empty;

  BinaryImage Img = makePEImage();
  DebugInfoResult R = loadDebugInfo(Dir.path("probe.exe"), Img, Req);

  EXPECT_FALSE(static_cast<bool>(R));
  EXPECT_FALSE(R.Error.empty());
}

// lld-link states each input chunk as `<object>:(<section>)`, and that object
// path is free to start with a dot.  Mistaking it for the enclosing output
// section files every symbol under it as non-code, which loses the whole map
// while still looking like a successful parse.
TEST(LoadDebugInfoTest, LLDMapKeepsSymbolsUnderADotPrefixedObjectPath) {
  ScratchDir Dir;

  DebugInfoRequest Req;
  Req.MapPath = Dir.write("probe.exe.map", kCOFFLLDMap);

  BinaryImage Img = makePEImage();
  DebugInfoResult R = loadDebugInfo(Dir.path("probe.exe"), Img, Req);

  ASSERT_TRUE(static_cast<bool>(R));
  EXPECT_EQ(R.Kind, DebugInfoKind::Map);

  // The data symbol stays out, so tracking the output section still works.
  std::vector<FunctionSym> Funcs = R.Context->allFunctions();
  ASSERT_EQ(Funcs.size(), 2u);
  EXPECT_EQ(Funcs[0].Name, "parse_header");
  EXPECT_EQ(Funcs[1].Name, "emit_record");
}

TEST(LoadDebugInfoTest, ELFMapKeepsSymbolsUnderADotPrefixedObjectPath) {
  ScratchDir Dir;

  DebugInfoRequest Req;
  Req.MapPath = Dir.write("probe.elf.map", kELFLLDMap);

  BinaryImage Img = makePEImage();
  DebugInfoResult R = loadDebugInfo(Dir.path("probe.elf"), Img, Req);

  ASSERT_TRUE(static_cast<bool>(R));
  EXPECT_EQ(R.Kind, DebugInfoKind::Map);
  std::vector<FunctionSym> Funcs = R.Context->allFunctions();
  ASSERT_EQ(Funcs.size(), 2u);
  EXPECT_EQ(Funcs[0].Name, "parse_header");
  EXPECT_EQ(Funcs[1].Name, "emit_record");
}

TEST(LoadDebugInfoTest, MissingExplicitPDBIsAnError) {
  ScratchDir Dir;

  DebugInfoRequest Req;
  Req.PDBPath = Dir.path("absent.pdb");

  BinaryImage Img = makePEImage();
  DebugInfoResult R = loadDebugInfo(Dir.path("probe.exe"), Img, Req);

  EXPECT_FALSE(static_cast<bool>(R));
  EXPECT_FALSE(R.Error.empty());
}

// EVM and SBF carry their own metadata formats; none of DWARF, PDB, or a
// linker MAP describes either, so discovery does not go looking.
TEST(LoadDebugInfoTest, SkipsFormatsWithNoNativeDebugFile) {
  ScratchDir Dir;
  Dir.write("probe.map", kMSVCMap);

  BinaryImage Img = makePEImage();
  Img.Arch = Arch::EVM;

  DebugInfoResult R = loadDebugInfo(Dir.path("probe.exe"), Img);
  EXPECT_FALSE(static_cast<bool>(R));
}

TEST(DebugInfoKindNameTest, NamesEveryKind) {
  EXPECT_STREQ(debugInfoKindName(DebugInfoKind::None), "none");
  EXPECT_STREQ(debugInfoKindName(DebugInfoKind::DWARF), "dwarf");
  EXPECT_STREQ(debugInfoKindName(DebugInfoKind::PDB), "pdb");
  EXPECT_STREQ(debugInfoKindName(DebugInfoKind::Map), "map");
}

} // anonymous namespace
