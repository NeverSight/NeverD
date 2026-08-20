//===- SinkScannerTests.cpp - Scanning and name-source precedence --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/debug/DebugContext.h"
#include "neverd/ir/med/MedIR.h"
#include "neverd/loader/BinaryImageModel.h"
#include "neverd/loader/FunctionDiscovery.h"
#include "neverd/safety/SinkScanner.h"
#include "neverd/support/BinaryEncoding.h"

using namespace neverd;
using namespace neverd::safety;

namespace {

// A MedFunc with a single call op and one recovered call record.
MedFunc makeCallFunc(va_t Entry, const std::string &Name, va_t CalleeAddr,
                     const std::string &CalleeName, bool Indirect = false,
                     va_t CallVA = 0x1000) {
  MedFunc F;
  F.Entry = Entry;
  F.Name = Name;

  MedBlock B;
  B.Id = 0;
  MedOp Call;
  Call.Opcode = Indirect ? NdOp::INDIR_CALL : NdOp::CALL;
  Call.Addr = CallVA;
  Call.addInput(MedVar::makeConst(CalleeAddr, 8));
  B.Ops.push_back(Call);
  F.Blocks.push_back(std::move(B));

  MedCallInfo CI;
  CI.BlockId = 0;
  CI.OpIdx = 0;
  CI.TargetAddr = CalleeAddr;
  CI.TargetName = CalleeName;
  CI.IsIndirect = Indirect;
  F.CallInfos.push_back(std::move(CI));
  return F;
}

// A debug context that names exactly one address.
class OneFunctionDebug : public NullDebugContext {
public:
  OneFunctionDebug(va_t Addr, std::string Name) : Addr(Addr), Name(Name) {}
  std::optional<FunctionSym> resolveFunction(va_t A) const override {
    if (A == Addr) {
      FunctionSym S;
      S.Name = Name;
      S.Addr = Addr;
      S.Size = 4;
      return S;
    }
    return std::nullopt;
  }
  bool hasInfo() const override { return true; }

private:
  va_t Addr;
  std::string Name;
};

} // namespace

TEST(SinkScanner, MatchesCopyAllocFreeAcrossFunctions) {
  std::vector<MedFunc> Funcs;
  Funcs.push_back(makeCallFunc(0x100, "copy_it", 0x2000, "_memcpy"));
  Funcs.push_back(makeCallFunc(0x200, "grab", 0x2010, "_malloc"));
  Funcs.push_back(makeCallFunc(0x300, "drop", 0x2020, "_free"));

  BinaryImage Img;
  AnalysisInput In;
  In.Img = &Img;
  In.MedFuncs = &Funcs;

  SinkCatalog Cat = SinkCatalog::defaults();
  std::vector<SinkSite> Sites = scanSinks(In, Cat);
  ASSERT_EQ(Sites.size(), 3u);
  EXPECT_EQ(Sites[0].Sink, "memcpy");
  EXPECT_EQ(Sites[0].Kind, SinkKind::Copy);
  EXPECT_EQ(Sites[0].ArgIndex, 2); // memcpy count argument.
  EXPECT_EQ(Sites[0].CallVA, 0x1000u);
  EXPECT_EQ(Sites[1].Sink, "malloc");
  EXPECT_EQ(Sites[1].Kind, SinkKind::Alloc);
  EXPECT_EQ(Sites[2].Sink, "free");
  EXPECT_EQ(Sites[2].Kind, SinkKind::Free);
}

TEST(SinkScanner, IndirectCallMatchesImportByName) {
  std::vector<MedFunc> Funcs;
  Funcs.push_back(makeCallFunc(0x100, "f", 0, "memcpy", /*Indirect=*/true));

  BinaryImage Img;
  Import Imp;
  Imp.Module = "msvcrt.dll";
  Imp.Name = "memcpy";
  Imp.IATAddr = 0x2000;
  Img.Imports.push_back(Imp);

  AnalysisInput In;
  In.Img = &Img;
  In.MedFuncs = &Funcs;

  std::vector<SinkSite> Sites = scanSinks(In, SinkCatalog::defaults());
  ASSERT_EQ(Sites.size(), 1u);
  EXPECT_TRUE(Sites[0].IsIndirect);
  EXPECT_EQ(Sites[0].Source, NameSource::Import);
}

TEST(SinkScanner, ImportAddressRecoversSyntheticCalleeName) {
  std::vector<MedFunc> Funcs;
  Funcs.push_back(makeCallFunc(0x100, "f", 0x2000, "sub_2000"));

  BinaryImage Img;
  Import Imp;
  Imp.Module = "runtime";
  Imp.Name = "memcpy";
  Imp.IATAddr = 0x2000;
  Img.Imports.push_back(Imp);

  AnalysisInput In;
  In.Img = &Img;
  In.MedFuncs = &Funcs;

  std::vector<SinkSite> Sites = scanSinks(In, SinkCatalog::defaults());
  ASSERT_EQ(Sites.size(), 1u);
  EXPECT_EQ(Sites[0].Sink, "memcpy");
  EXPECT_EQ(Sites[0].StatedName, "memcpy");
  EXPECT_EQ(Sites[0].Source, NameSource::Import);
}

TEST(SinkScanner, DirectInternalCallDoesNotBorrowUnrelatedImportIdentity) {
  BinaryImage Img;
  Img.Imports.push_back({"runtime", "memcpy", 0, 0x2000});

  MedFunc Func = makeCallFunc(0x100, "f", 0x3000, "_memcpy");
  AnalysisInput In;
  In.Img = &Img;

  EXPECT_EQ(resolveCallName(In, Func.CallInfos[0]), "_memcpy");
}

TEST(SinkScanner, AArch64ELFPLTVeneerRecoversImportIdentity) {
  constexpr va_t StubVA = 0x10CC0;
  constexpr va_t IATAddr = 0x30EB0;

  BinaryImage Img;
  Img.Arch = Arch::AArch64;
  Img.Bits = Bitness::Bits64;
  Img.Format = BinaryFormat::ELF;
  Segment Text;
  Text.Name = ".plt";
  Text.VA = StubVA;
  Text.Size = 16;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.resize(Text.Size);
  writeLE<uint32_t>(Text.Data.data(), 0x90000110u);
  writeLE<uint32_t>(Text.Data.data() + 4, 0xF9475A11u);
  writeLE<uint32_t>(Text.Data.data() + 8, 0x913AC210u);
  writeLE<uint32_t>(Text.Data.data() + 12, 0xD61F0220u);
  Img.Segments.push_back(std::move(Text));
  Img.Imports.push_back({"extern", "memcpy", 0, IATAddr});

  scanImportThunks(Img);
  std::vector<MedFunc> Funcs;
  Funcs.push_back(makeCallFunc(0x100, "f", StubVA, "sub_10CC0"));
  AnalysisInput In;
  In.Img = &Img;
  In.MedFuncs = &Funcs;

  std::vector<SinkSite> Sites = scanSinks(In, SinkCatalog::defaults());
  ASSERT_EQ(Sites.size(), 1u);
  EXPECT_EQ(Sites[0].Sink, "memcpy");
  EXPECT_EQ(Sites[0].Source, NameSource::Import);
}

TEST(SinkScanner, ImportBeatsDebugForCalleeName) {
  // An imported memcpy is `import`, even with debug info loaded.
  std::vector<MedFunc> Funcs;
  Funcs.push_back(makeCallFunc(0x100, "f", 0x2000, "_memcpy"));

  BinaryImage Img;
  Import Imp;
  Imp.Module = "libSystem";
  Imp.Name = "memcpy";
  Imp.IATAddr = 0x2000;
  Img.Imports.push_back(Imp);

  OneFunctionDebug Dbg(0x2000, "memcpy");
  AnalysisInput In;
  In.Img = &Img;
  In.MedFuncs = &Funcs;
  In.Dbg = &Dbg;
  In.DebugKind = DebugInfoKind::DWARF;

  std::vector<SinkSite> Sites = scanSinks(In, SinkCatalog::defaults());
  ASSERT_EQ(Sites.size(), 1u);
  EXPECT_EQ(Sites[0].Source, NameSource::Import);
}

TEST(SinkScanner, DebugKindSelectsNameSource) {
  BinaryImage Img;
  std::vector<MedFunc> Funcs;
  Funcs.push_back(makeCallFunc(0x100, "f", 0x5000, "memcpy"));

  OneFunctionDebug Dbg(0x5000, "memcpy");
  AnalysisInput In;
  In.Img = &Img;
  In.MedFuncs = &Funcs;
  In.Dbg = &Dbg;

  In.DebugKind = DebugInfoKind::DWARF;
  EXPECT_EQ(scanSinks(In, SinkCatalog::defaults())[0].Source,
            NameSource::Dwarf);
  In.DebugKind = DebugInfoKind::PDB;
  EXPECT_EQ(scanSinks(In, SinkCatalog::defaults())[0].Source, NameSource::Pdb);
  In.DebugKind = DebugInfoKind::Map;
  EXPECT_EQ(scanSinks(In, SinkCatalog::defaults())[0].Source, NameSource::Map);
}

TEST(SinkScanner, RenameIsStrongest) {
  BinaryImage Img;
  Import Imp;
  Imp.Name = "memcpy";
  Imp.IATAddr = 0x2000;
  Img.Imports.push_back(Imp);

  std::vector<MedFunc> Funcs;
  Funcs.push_back(makeCallFunc(0x100, "f", 0x2000, "_memcpy"));

  std::map<va_t, std::string> Renames{{0x2000, "memcpy"}};
  AnalysisInput In;
  In.Img = &Img;
  In.MedFuncs = &Funcs;
  In.Renames = &Renames;

  EXPECT_EQ(scanSinks(In, SinkCatalog::defaults())[0].Source,
            NameSource::Rename);
}

TEST(SinkScanner, ExportAndSymbolAndSignatureFallbacks) {
  std::vector<MedFunc> Funcs;
  Funcs.push_back(makeCallFunc(0x100, "f", 0x3000, "memcpy"));

  BinaryImage Img;
  // Export at the callee address -> export.
  Export Exp;
  Exp.Name = "memcpy";
  Exp.Addr = 0x3000;
  Img.Exports.push_back(Exp);
  AnalysisInput In;
  In.Img = &Img;
  In.MedFuncs = &Funcs;
  EXPECT_EQ(scanSinks(In, SinkCatalog::defaults())[0].Source,
            NameSource::Export);

  // With only a symbol -> symbol.
  BinaryImage Img2;
  Symbol Sym = Symbol::makeFunc(0x3000, 4);
  Sym.Name = "memcpy";
  Img2.Symbols.push_back(Sym);
  In.Img = &Img2;
  EXPECT_EQ(scanSinks(In, SinkCatalog::defaults())[0].Source,
            NameSource::Symbol);

  // With neither, a signature-named address -> sig.
  BinaryImage Img3;
  std::set<va_t> SigNamed{0x3000};
  In.Img = &Img3;
  In.SignatureNamed = &SigNamed;
  EXPECT_EQ(scanSinks(In, SinkCatalog::defaults())[0].Source, NameSource::Sig);
}
