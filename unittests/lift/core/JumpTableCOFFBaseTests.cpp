//===- JumpTableCOFFBaseTests.cpp - Exact PE table-base uses -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "NeverDLiftFixture.h"

#include "neverd/decode/Decoder.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/ir/low/SEHFrameProof.h"
#include "neverd/support/BinaryLoading.h"

#include "llvm/Support/Error.h"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <set>
#include <string>
#include <vector>

namespace {
using namespace neverd;
constexpr va_t ImageBase = 0x140000000;
constexpr va_t Entry = ImageBase + 0x1000;

struct Writer {
  std::vector<uint8_t> Code;
  void put(std::initializer_list<uint8_t> Bytes) {
    Code.insert(Code.end(), Bytes);
  }
  size_t u32(uint32_t Value) {
    size_t At = Code.size();
    for (unsigned I = 0; I < 4; ++I)
      Code.push_back(static_cast<uint8_t>(Value >> (8 * I)));
    return At;
  }
  void patch(size_t At, uint32_t Value) {
    for (unsigned I = 0; I < 4; ++I)
      Code[At + I] = static_cast<uint8_t>(Value >> (8 * I));
  }
  void branch(size_t At, size_t To) {
    patch(At, static_cast<uint32_t>(To - At - 4));
  }
  void baseLEA(va_t Value) {
    put({0x4c, 0x8d, 0x1d}); // lea r11, [rip + disp32]
    u32(static_cast<uint32_t>(Value - (Entry + Code.size() + 4)));
  }
};

struct Fixture {
  BinaryImage Image;
  std::vector<va_t> Targets;
  va_t Branch = InvalidVA;
};

Fixture makeFixture(unsigned FillerBlocks, const std::string &Variant) {
  const std::string Kind =
      Variant.starts_with("composite-") ? Variant.substr(10) : Variant;
  const va_t RuntimeBase = Kind == "otherbase" ? Entry : ImageBase;
  Writer W;
  W.put({0x85, 0xd2, 0x0f, 0x85}); // test edx; jne side path
  size_t SideBranch = W.u32(0);
  if (Kind == "scalar") {
    W.put({0x49, 0xbb}); // movabs r11, imagebase (no address provenance)
    W.u32(static_cast<uint32_t>(ImageBase));
    W.u32(static_cast<uint32_t>(ImageBase >> 32));
  } else {
    W.baseLEA(RuntimeBase);
  }
  W.put({0xe9});
  size_t ToDispatch = W.u32(0);
  size_t Side = W.Code.size();
  W.branch(SideBranch, Side);
  for (unsigned I = 0; I < FillerBlocks; ++I)
    W.put({0x83, 0xc0, 0x01, 0xeb, 0x00}); // add eax,1; jmp next
  if (Kind == "decoy" || Kind == "divergent")
    W.baseLEA(Entry);
  size_t SideToDispatch = 0;
  if (Kind == "divergent") {
    W.put({0xe9});
    SideToDispatch = W.u32(0);
  } else {
    W.put({0xc3});
  }
  size_t Dispatch = W.Code.size();
  W.branch(ToDispatch, Dispatch);
  if (SideToDispatch)
    W.branch(SideToDispatch, Dispatch);
  if (Kind == "partial")
    W.put({0x41, 0xb3, 0x01}); // mov r11b, 1
  if (Kind == "full")
    W.put({0x45, 0x31, 0xdb}); // xor r11d, r11d
  size_t CallSlot = 0;
  if (Kind.starts_with("call")) {
    W.put({0xe8});
    CallSlot = W.u32(0);
  }
  W.put({0x89, 0xc9, 0x83, 0xf9, 0x05, 0x0f, 0x87});
  size_t DefaultBranch = W.u32(0);
  size_t IndexTableSlot = 0;
  const bool Composite = Variant.starts_with("composite");
  if (Composite) {
    W.put({0x41, 0x0f, 0xb6, 0x84, 0x0b});
    IndexTableSlot = W.u32(0); // movzx eax,byte[r11+rcx+outer RVA]
  }
  W.put({0x41, 0x8b, 0x8c, Composite ? uint8_t{0x83} : uint8_t{0x8b}});
  size_t TableSlot = W.u32(0);
  if (Kind == "postload")
    W.put({0x41, 0xb3, 0x01}); // clobber anchor after the LOAD
  W.put({0x4c, 0x01, 0xd9});   // add rcx,r11
  const va_t Branch = Entry + W.Code.size();
  W.put({0xff, 0xe1});
  std::vector<va_t> Targets;
  for (int I = 0; I < 6; ++I) {
    Targets.push_back(Entry + W.Code.size());
    W.put({0xb8}); // mov eax, independently expected case result
    W.u32(10 + I);
    W.put({0xc3, 0xcc});
  }
  size_t Default = W.Code.size();
  W.put({0xc3, 0xcc, 0xcc, 0xcc});
  W.branch(DefaultBranch, Default);
  const size_t FunctionEnd = W.Code.size();
  const size_t Helper = W.Code.size();
  size_t NestedCall = 0;
  if (Kind == "call")
    W.put({0x41, 0xb3, 0x01}); // an actual partial write to the table base
  if (Kind == "call-unknown")
    W.put({0xff, 0xd0}); // an opaque callee may change the base
  if (Kind == "call-transitive" || Kind == "call-transitive-clobber") {
    W.put({0xe8});
    NestedCall = W.u32(0);
  }
  W.put({0xc3, 0xcc, 0xcc, 0xcc});
  const size_t HelperEnd = W.Code.size();
  const size_t Leaf = W.Code.size();
  if (NestedCall) {
    W.branch(NestedCall, Leaf);
    if (Kind == "call-transitive-clobber")
      W.put({0x45, 0x31, 0xdb}); // xor r11d,r11d, including the high half
    W.put({0x31, 0xc0, 0xc3});   // modifies only the result and flags
  }
  const size_t LeafEnd = W.Code.size();
  if (CallSlot)
    W.branch(CallSlot, Helper);
  const size_t Table = W.Code.size();
  W.patch(TableSlot, static_cast<uint32_t>(Entry + Table - RuntimeBase));
  for (va_t Target : Targets)
    W.u32(static_cast<uint32_t>(Target - RuntimeBase));
  W.u32(static_cast<uint32_t>(Targets.back() - RuntimeBase));
  if (Composite) {
    W.patch(IndexTableSlot,
            static_cast<uint32_t>(Entry + W.Code.size() - ImageBase));
    W.put({0, 1, 2, 3, 4, 5});
  }
  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  Image.Format = BinaryFormat::COFF;
  Image.Base = ImageBase;
  Image.Entry = Entry;
  Segment Text;
  Text.Name = ".text";
  Text.VA = Entry;
  Text.Size = Text.FileSz = W.Code.size();
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data = W.Code;
  Image.Segments.push_back(std::move(Text));
  Section Sec;
  Sec.Name = ".text";
  Sec.VA = Entry;
  Sec.Size = W.Code.size();
  Sec.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Image.Sections.push_back(std::move(Sec));
  Image.Symbols.push_back(Symbol::makeFunc(Entry, FunctionEnd));
  Image.Symbols.push_back(Symbol::makeFunc(Entry + Helper, HelperEnd - Helper));
  Image.KnownCodeRanges.emplace_back(Entry, Entry + FunctionEnd);
  Image.KnownCodeRanges.emplace_back(Entry + Helper, Entry + HelperEnd);
  if (NestedCall) {
    Image.Symbols.push_back(Symbol::makeFunc(Entry + Leaf, LeafEnd - Leaf));
    Image.KnownCodeRanges.emplace_back(Entry + Leaf, Entry + LeafEnd);
  }
  Image.Symbols.front().Name = "table_base_fixture";
  return {std::move(Image), std::move(Targets), Branch};
}

LowFunc recover(const Fixture &F, std::optional<size_t> Budget = std::nullopt) {
  Decoder D;
  EXPECT_TRUE(D.init(F.Image.Arch, F.Image.Mode));
  CFGBuilder Builder;
  std::set<va_t> Entries{Entry};
  Builder.setKnownFuncEntries(&Entries);
  if (Budget)
    Builder.setMaskFixedPointEvidenceBudgetForTesting(*Budget);
  return Builder.build(F.Image, D, Entry, "table_base_fixture");
}

void expectExact(const Fixture &F, const LowFunc &Low) {
  ASSERT_EQ(Low.JumpTables.size(), 1u);
  EXPECT_EQ(Low.JumpTables.front().InsnAddr, F.Branch);
  EXPECT_EQ(Low.JumpTables.front().Targets, F.Targets);
  EXPECT_TRUE(Low.UnsafeIndirectBranchAddresses.empty());
  EXPECT_TRUE(Low.DecodeFailureAddresses.empty());
  EXPECT_TRUE(Low.UnsupportedInstructionAddresses.empty());
}

TEST(COFFTableBase, UsesControlFlowInsteadOfLexicalDistance) {
  for (unsigned Count : {0u, 32u, 160u})
    for (const std::string Variant : {"valid", "composite", "decoy"}) {
      SCOPED_TRACE(Variant + " blocks=" + std::to_string(Count));
      const auto F = makeFixture(Count, Variant);
      expectExact(F, recover(F));
    }
}

TEST(COFFTableBase, RejectsConflictingOrNonAddressDefinitions) {
  for (const std::string Variant :
       {"divergent", "partial", "full", "call", "call-unknown",
        "call-transitive-clobber", "postload", "scalar"})
    for (bool Composite : {false, true}) {
      const std::string Kind = Composite ? "composite-" + Variant : Variant;
      SCOPED_TRACE(Kind);
      const auto F = makeFixture(32, Kind);
      EXPECT_TRUE(recover(F).JumpTables.empty());
    }
}

TEST(COFFTableBase, DecodedCallTreeKeepsUntouchedVolatileBase) {
  for (const std::string Variant : {"call-preserved", "call-transitive"})
    for (bool Composite : {false, true}) {
      const std::string Kind = Composite ? "composite-" + Variant : Variant;
      SCOPED_TRACE(Kind);
      const auto F = makeFixture(32, Kind);
      expectExact(F, recover(F));
      // An exhausted proof cannot borrow the facts of a previous build.
      EXPECT_TRUE(recover(F, size_t{0}).JumpTables.empty());
      expectExact(F, recover(F));
    }
}

TEST(COFFTableBase, KeepsOtherRelativeAnchors) {
  for (unsigned Count : {0u, 32u, 160u}) {
    SCOPED_TRACE(Count);
    const auto F = makeFixture(Count, "otherbase");
    expectExact(F, recover(F));
  }
}

TEST(COFFTableBase, ExhaustionCannotPublishAPartialTable) {
  for (const std::string Variant : {"valid", "composite"}) {
    const auto F = makeFixture(160, Variant);
    for (size_t Budget : {size_t{0}, size_t{1}, size_t{2048}}) {
      SCOPED_TRACE(Variant + " budget=" + std::to_string(Budget));
      EXPECT_TRUE(recover(F, Budget).JumpTables.empty());
    }
    expectExact(F, recover(F));
  }
}

TEST(COFFTableBase, ReplaysBothTablesAfterDiscoveringAnotherDispatch) {
  auto F = makeFixture(0, "composite");
  auto &Code = F.Image.Segments.front().Data;
  Writer W;
  W.Code = Code;
  // The first table's cases discover a common successor containing 32 new
  // block starts and a second switch. None is decoded before the first table.
  const size_t Growth = W.Code.size();
  for (va_t Case : F.Targets) {
    const size_t Offset = Case - Entry;
    ASSERT_LT(Growth - Offset - 2, 128u);
    W.Code[Offset] = 0xeb;
    W.Code[Offset + 1] = static_cast<uint8_t>(Growth - Offset - 2);
  }
  for (unsigned I = 0; I < 32; ++I)
    W.put({0x83, 0xc0, 0x01, 0xeb, 0x00});
  W.put({0x44, 0x89, 0xc1, // mov ecx,r8d: independent Win64 selector
         0x83, 0xf9, 0x05, 0x0f, 0x87});
  const size_t ToDefault = W.u32(0);
  W.put({0x41, 0x8b, 0x8c, 0x8b});
  const size_t TableSlot = W.u32(0);
  W.put({0x4c, 0x01, 0xd9});
  const va_t SecondBranch = Entry + W.Code.size();
  W.put({0xff, 0xe1});
  std::vector<va_t> SecondTargets;
  for (unsigned I = 0; I < 6; ++I) {
    SecondTargets.push_back(Entry + W.Code.size());
    W.put({0xc3, 0xcc});
  }
  W.branch(ToDefault, W.Code.size());
  W.put({0xc3, 0xcc, 0xcc, 0xcc});
  const size_t FunctionEnd = W.Code.size();
  W.patch(TableSlot, static_cast<uint32_t>(Entry + W.Code.size() - ImageBase));
  for (va_t Target : SecondTargets)
    W.u32(static_cast<uint32_t>(Target - ImageBase));
  Code = std::move(W.Code);
  F.Image.Segments.front().Size = F.Image.Segments.front().FileSz = Code.size();
  F.Image.Sections.front().Size = Code.size();
  F.Image.Symbols.front().Size = FunctionEnd;
  const va_t Handler = SecondTargets.back();
  F.Image.Symbols.resize(1);
  F.Image.KnownCodeRanges = {{Entry, Entry + FunctionEnd}};
  // Exercise the same downstream frame-CFG gate used during source
  // preparation, with a zero-size fixed frame and a handler that also has an
  // ordinary case entry. The caller explicitly supplies that ordinary role.
  ExceptionFunction EH;
  EH.CodeRange = {Entry, Entry + FunctionEnd};
  EH.Kind = RuntimeFunctionKind::Primary;
  EH.Encoding = ExceptionEncoding::X64UnwindV1;
  EH.UnwindVersion = 1;
  EH.ParseStatus = ExceptionParseStatus::Complete;
  EH.SEH.emplace();
  SEHScopeRecord Scope;
  Scope.Kind = SEHScopeKind::CatchAll;
  Scope.GuardedRange = {Entry, F.Branch};
  Scope.HandlerVA = Handler;
  EH.SEH->Scopes.push_back(Scope);
  LowFunc Low = recover(F);
  Low.ExceptionMetadata = std::move(EH);
  ASSERT_EQ(Low.JumpTables.size(), 2u);
  for (const auto &Table : Low.JumpTables) {
    if (Table.InsnAddr == F.Branch)
      EXPECT_EQ(Table.Targets, F.Targets);
    else {
      EXPECT_EQ(Table.InsnAddr, SecondBranch);
      EXPECT_EQ(Table.Targets, SecondTargets);
    }
  }
  EXPECT_TRUE(Low.UnsafeIndirectBranchAddresses.empty());
  size_t Work = 1u << 20;
  auto Proof = proveLowSEHEstablisherFrame(
      Low, Handler, getTargetRegInfo(Arch::X64), true, Work, {}, false,
      LowSEHFrameUse::SourceProjection);
  ASSERT_TRUE(static_cast<bool>(Proof)) << llvm::toString(Proof.takeError());
}

// A minimal linked PE keeps this CLI contract independent of a host linker.
// Its only section is the fixture's exact byte inventory, at the same VA.
void writePE(const Fixture &F, const fs::path &Path) {
  const auto &Code = F.Image.Segments.front().Data;
  const size_t RawSize = (Code.size() + 511) & ~size_t{511};
  std::vector<uint8_t> PE(512 + RawSize);
  auto Put = [&](size_t At, uint64_t Value, unsigned Width) {
    for (unsigned I = 0; I < Width; ++I)
      PE[At + I] = static_cast<uint8_t>(Value >> (I * 8));
  };
  PE[0] = 'M';
  PE[1] = 'Z';
  Put(0x3c, 0x80, 4);
  PE[0x80] = 'P';
  PE[0x81] = 'E';
  Put(0x84, 0x8664, 2);
  Put(0x86, 1, 2);
  Put(0x94, 240, 2);
  Put(0x96, 0x22, 2);
  constexpr size_t Opt = 0x98;
  Put(Opt, 0x20b, 2);
  Put(Opt + 4, RawSize, 4);
  Put(Opt + 16, 0x1000, 4);
  Put(Opt + 20, 0x1000, 4);
  Put(Opt + 24, ImageBase, 8);
  Put(Opt + 32, 0x1000, 4);
  Put(Opt + 36, 0x200, 4);
  Put(Opt + 40, 6, 2);
  Put(Opt + 48, 6, 2);
  Put(Opt + 56, (0x1000 + Code.size() + 4095) & ~size_t{4095}, 4);
  Put(Opt + 60, 512, 4);
  Put(Opt + 68, 3, 2);
  Put(Opt + 72, 0x100000, 8);
  Put(Opt + 80, 0x1000, 8);
  Put(Opt + 88, 0x100000, 8);
  Put(Opt + 96, 0x1000, 8);
  Put(Opt + 108, 16, 4);
  constexpr size_t Section = Opt + 240;
  const std::string Name = ".text";
  std::copy(Name.begin(), Name.end(), PE.begin() + Section);
  Put(Section + 8, Code.size(), 4);
  Put(Section + 12, 0x1000, 4);
  Put(Section + 16, RawSize, 4);
  Put(Section + 20, 512, 4);
  Put(Section + 36, 0x60000020, 4);
  std::copy(Code.begin(), Code.end(), PE.begin() + 512);
  std::ofstream Out(Path, std::ios::binary);
  Out.write(reinterpret_cast<const char *>(PE.data()), PE.size());
  ASSERT_TRUE(Out.good());
}

class COFFTableBaseCLI : public NeverDLiftTest {};

TEST_F(COFFTableBaseCLI, BothSourceRoutesUseTheSharedBaseProof) {
  for (const std::string Variant : {"decoy", "composite"}) {
    SCOPED_TRACE(Variant);
    const auto F = makeFixture(160, Variant);
    const fs::path Input = tmpFile(Variant + ".exe");
    writePE(F, Input);
    auto Image = loadBinary(Input.string());
    ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
    Fixture Loaded{std::move(*Image), F.Targets, F.Branch};
    expectExact(Loaded, recover(Loaded));
    for (bool LLVM : {false, true}) {
      const fs::path Output = tmpFile(Variant + (LLVM ? "-llvm.c" : "-high.c"));
      std::vector<std::string> Args{"decompile",   Input.string(), "--func",
                                    "0x140001000", "--language",   "c",
                                    "-o",          Output.string()};
      if (LLVM)
        Args.push_back("--llvm");
      const auto Result = exec(ndBin(), Args);
      ASSERT_TRUE(Result.ok()) << Result.err;
      ASSERT_TRUE(fs::exists(Output));
      std::ifstream Source(Output);
      const std::string Text((std::istreambuf_iterator<char>(Source)), {});
      EXPECT_NE(Text.find("sub_140001000("), std::string::npos);
      EXPECT_NE(Text.find("return"), std::string::npos);
      if (hasCrossTargetClang()) {
        // Zero is a valid argument for either the recovered integer or pointer
        // spelling. It selects case zero and must return its machine value.
        const fs::path Program =
            tmpFile(Variant + (LLVM ? "-llvm.exe" : "-high.exe"));
        const fs::path Harness =
            tmpFile(Variant + (LLVM ? "-llvm-run.c" : "-high-run.c"));
        std::ofstream RunSource(Harness);
        RunSource << Text
                  << "\nint main(void) { return sub_140001000(0, 0) == 10 ? 0 "
                     ": 1; }\n";
        RunSource.close();
        const auto Built = exec(NEVERD_TEST_CLANG, {"-O0", Harness.string(),
                                                    "-o", Program.string()});
        ASSERT_TRUE(Built.ok()) << Built.err;
        const auto Ran = exec(Program.string(), {});
        EXPECT_TRUE(Ran.ok()) << Ran.err;
      }
    }
  }
}

} // namespace
