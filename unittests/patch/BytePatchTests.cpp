//===- BytePatchTests.cpp - Interactive patch contracts -------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// File replay, rejection atomicity, instruction encoding and PC tests.
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/decode/Decoder.h"
#include "neverd/decode/NativeOperands.h"
#include "neverd/libc/LibCNames.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/object/PEChecksum.h"
#include "neverd/patch/Limits.h"
#include "neverd/patch/PatchActions.h"
#include "neverd/sigs/SignatureDB.h"
#include "neverd/support/FilePath.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/FileSystem.h"

#include <fstream>

using namespace neverd;
using namespace neverd::patch;

namespace {
class BytePatch : public testing::Test {
protected:
  std::filesystem::path Directory, Path;
  BinaryImage Img;
  PatchOverlay Overlay;
  std::optional<InputFile> Input;
  std::vector<uint8_t> Original;
  void SetUp() override {
    llvm::SmallString<128> Name;
    ASSERT_FALSE(
        llvm::sys::fs::createUniqueDirectory("neverd-byte-patch", Name));
    Directory = std::filesystem::u8path(Name.str().str());
    Path = Directory / "input.bin";
    reset({0x75, 0x04, 0xb8, 0x30, 0, 0, 0, 0xc3});
  }
  void TearDown() override {
    std::error_code EC;
    std::filesystem::remove_all(Directory, EC);
  }
  void reset(std::vector<uint8_t> Bytes, Arch A = Arch::X64,
             InstructionMode Mode = InstructionMode::Default) {
    Original = Bytes;
    {
      std::ofstream Out(Path, std::ios::binary);
      Out.write(reinterpret_cast<const char *>(Bytes.data()), Bytes.size());
    }
    Img = {};
    Overlay = {};
    Img.Arch = A;
    Img.Mode = Mode;
    Img.Format = BinaryFormat::Raw;
    Segment S;
    S.VA = 0x1000;
    S.Size = Bytes.size() + 16;
    S.FileSz = Bytes.size();
    S.Data = Bytes;
    S.Data.resize(S.Size);
    S.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
    Img.Segments.push_back(S);
    Section Section;
    Section.VA = S.VA;
    Section.Size = S.Size;
    Section.FileSz = S.FileSz;
    Section.Data = S.Data;
    Section.Flags = S.Flags;
    Img.Sections.push_back(Section);
    auto Opened = InputFile::open(Path, Img);
    ASSERT_TRUE(bool(Opened)) << llvm::toString(Opened.takeError());
    Input = std::move(*Opened);
  }
  PatchRequest action(PatchKind Kind) {
    PatchRequest R;
    R.Address = 0x1000;
    R.Kind = Kind;
    return R;
  }
};

TEST_F(BytePatch, SignatureNamesStayStableButDerivedNoReturnIsWithdrawn) {
  auto Sym = Symbol::makeFunc(0x1000, Original.size());
  Sym.Name = "abort";
  Sym.Origin = NameOrigin::Stated;
  Img.Symbols.push_back(Sym);
  sigs::SignatureDB DB;
  ASSERT_FALSE(DB.loadPatternText("7504B830000000C3 00 0000 0008 :0000 abort\n",
                                  "test"));
  DB.apply(Img, {0x1000});
  ASSERT_EQ(DB.buildNameMap().at(0x1000), "abort");
  EXPECT_TRUE(libc::isNoReturnTarget(Img, 0x1000));
  ASSERT_FALSE(
      Overlay.apply(Img, *Input, 0x1000, {0x90, 0x90}, PatchKind::Nop));
  DB.apply(Img, {0x1000});
  EXPECT_EQ(DB.buildNameMap().at(0x1000), "abort");
  EXPECT_FALSE(libc::isNoReturnTarget(Img, 0x1000));
  auto Restored = Overlay.revert(Img, 0x1000, 2);
  ASSERT_TRUE(bool(Restored)) << llvm::toString(Restored.takeError());
  EXPECT_TRUE(libc::isNoReturnTarget(Img, 0x1000));
}
TEST_F(BytePatch, OverlappingEditsRestoreFileOriginalAndAllImageCopies) {
  ASSERT_FALSE(
      Overlay.apply(Img, *Input, 0x1000, {0x90, 0x90}, PatchKind::Nop));
  ASSERT_FALSE(
      Overlay.apply(Img, *Input, 0x1001, {0xeb, 0x90}, PatchKind::Bytes));
  EXPECT_EQ(Img.Sections[0].Data, Img.Segments[0].Data);
  EXPECT_EQ(Overlay.originalByte(0x1001), 0x04);
  auto N = Overlay.revert(Img, 0x1000, 3);
  ASSERT_TRUE(bool(N)) << llvm::toString(N.takeError());
  EXPECT_EQ(*N, 3u);
  EXPECT_TRUE(Overlay.empty());
  EXPECT_TRUE(std::equal(Original.begin(), Original.end(),
                         Img.Segments[0].Data.begin()));
}

TEST_F(BytePatch, AnalysisSnapshotOwnsSignatureEvidenceWithoutPatternStorage) {
  auto Sym = Symbol::makeFunc(0x1000, Original.size());
  Sym.Name = "abort";
  Sym.Origin = NameOrigin::Stated;
  Img.Symbols.push_back(Sym);
  sigs::SignatureDB DB;
  ASSERT_FALSE(DB.loadPatternText("7504B830000000C3 00 0000 0008 :0000 abort\n",
                                  "snapshot"));
  DB.apply(Img, {0x1000});
  const auto Names = DB.buildNameMap();
  ASSERT_EQ(Names.at(0x1000), "abort");
  auto Snapshot = DB.snapshotForAnalysis();
  DB.clear();
  EXPECT_TRUE(DB.buildNameMap().empty());
  EXPECT_EQ(Snapshot.buildNameMap(), Names);
  ASSERT_EQ(Snapshot.matches().size(), 1u);
  EXPECT_EQ(Snapshot.matches().front().Name, "abort");
  EXPECT_EQ(Snapshot.moduleCount(), 0u);
  EXPECT_EQ(Snapshot.fileCount(), 0u);
}
TEST_F(BytePatch, RejectsRelocationsZeroFillAndAliasesAtomically) {
  Img.CodePtrRelocSlots.insert(0x1000);
  auto E = Overlay.apply(Img, *Input, 0x1004, {0x90}, PatchKind::Bytes);
  EXPECT_TRUE(bool(E));
  llvm::consumeError(std::move(E));
  Img.CodePtrRelocSlots.clear();
  E = Overlay.apply(Img, *Input, 0x1008, {0x90}, PatchKind::Bytes);
  EXPECT_TRUE(bool(E));
  llvm::consumeError(std::move(E));
  Img.Segments.push_back(Img.Segments[0]);
  Img.Segments.back().VA = 0x2000;
  E = Overlay.apply(Img, *Input, 0x1000, {0x90}, PatchKind::Bytes);
  EXPECT_TRUE(bool(E));
  llvm::consumeError(std::move(E));
  EXPECT_TRUE(Overlay.empty());
}
TEST_F(BytePatch, AssemblyCoversWholeInstructionsOnlyWithExplicitConsent) {
  auto R = action(PatchKind::Assemble);
  R.Text = "mov eax, 1";
  auto Refused = planPatch(Img, *Input, Overlay, R);
  ASSERT_FALSE(bool(Refused));
  llvm::consumeError(Refused.takeError());
  R.AllowOverlap = true;
  R.FillNops = false;
  Refused = planPatch(Img, *Input, Overlay, R);
  ASSERT_FALSE(bool(Refused));
  llvm::consumeError(Refused.takeError());
  R.FillNops = true;
  auto Plan = planPatch(Img, *Input, Overlay, R);
  ASSERT_TRUE(bool(Plan)) << llvm::toString(Plan.takeError());
  EXPECT_EQ(Plan->After, (std::vector<uint8_t>{0xb8, 1, 0, 0, 0, 0x90, 0x90}));
  EXPECT_EQ(Plan->Covers.size(), 2u);
  EXPECT_EQ(Plan->Before.size(), 7u);
  R.Text = "ret";
  Plan = planPatch(Img, *Input, Overlay, R);
  ASSERT_TRUE(bool(Plan)) << llvm::toString(Plan.takeError());
  EXPECT_EQ(Plan->After, (std::vector<uint8_t>{0xc3, 0x90}));
}
TEST(PatchAssembler, ExplicitFrameSymbolsDoNotGuessUnresolvedVariables) {
  AssemblyRequest R;
  R.Address = 0x1000;
  R.Text = "mov eax, [rsp+var_20]";
  auto Bad = assemble(Arch::X64, R);
  ASSERT_FALSE(bool(Bad));
  llvm::consumeError(Bad.takeError());
  R.Symbols.emplace("var_20", -32);
  auto Bytes = assemble(Arch::X64, R);
  ASSERT_TRUE(bool(Bytes)) << llvm::toString(Bytes.takeError());
  EXPECT_EQ(*Bytes, (std::vector<uint8_t>{0x8b, 0x44, 0x24, 0xe0}));
}
TEST_F(BytePatch, ReplayValidationDoesNotLosePreviousEdits) {
  ASSERT_FALSE(
      Overlay.apply(Img, *Input, 0x1000, {0x90, 0x90}, PatchKind::Nop));
  std::string JSON;
  llvm::raw_string_ostream OS(JSON);
  OS << llvm::json::Value(patchRowsJSON(Overlay));
  auto Rows = parsePatchRows(JSON);
  ASSERT_TRUE(bool(Rows)) << llvm::toString(Rows.takeError());
  (*Rows)[0].Original[0] ^= 1;
  auto E = Overlay.replace(Img, *Input, std::move(*Rows));
  EXPECT_TRUE(bool(E));
  llvm::consumeError(std::move(E));
  EXPECT_EQ(Img.Segments[0].Data[0], 0x90);
  EXPECT_EQ(Overlay.bytes(), 2u);
}
TEST_F(BytePatch, ExportIsExclusiveAndReplaysImmutableInput) {
  ASSERT_FALSE(
      Overlay.apply(Img, *Input, 0x1000, {0xeb}, PatchKind::BranchAlways));
  const auto Output = Directory / "output.bin";
  ASSERT_FALSE(exportPatches(Img, *Input, Overlay, Output));
  std::ifstream Stream(Output, std::ios::binary);
  std::vector<uint8_t> Bytes((std::istreambuf_iterator<char>(Stream)), {});
  auto Expected = Original;
  Expected[0] = 0xeb;
  EXPECT_EQ(Bytes, Expected);
  auto E = exportPatches(Img, *Input, Overlay, Output);
  EXPECT_TRUE(bool(E));
  llvm::consumeError(std::move(E));
  std::ifstream Source(Path, std::ios::binary);
  EXPECT_EQ(std::vector<uint8_t>((std::istreambuf_iterator<char>(Source)), {}),
            Original);
}
TEST_F(BytePatch, BranchActionsPreserveTargets) {
  auto P = planPatch(Img, *Input, Overlay, action(PatchKind::BranchAlways));
  ASSERT_TRUE(bool(P)) << llvm::toString(P.takeError());
  EXPECT_EQ(P->After, (std::vector<uint8_t>{0xeb, 0x04}));
  P = planPatch(Img, *Input, Overlay, action(PatchKind::BranchInvert));
  ASSERT_TRUE(bool(P)) << llvm::toString(P.takeError());
  EXPECT_EQ(P->After, (std::vector<uint8_t>{0x74, 0x04}));
  reset({0x0f, 0x85, 0xfa, 0xff, 0xff, 0xff});
  P = planPatch(Img, *Input, Overlay, action(PatchKind::BranchAlways));
  ASSERT_TRUE(bool(P)) << llvm::toString(P.takeError());
  EXPECT_EQ(P->After,
            (std::vector<uint8_t>{0xe9, 0xfb, 0xff, 0xff, 0xff, 0x90}));
}
TEST_F(BytePatch, ARMAndAArch64NOPAndBranches) {
  reset({0x40, 0, 0, 0x54}, Arch::AArch64);
  auto P = planPatch(Img, *Input, Overlay, action(PatchKind::BranchAlways));
  ASSERT_TRUE(bool(P)) << llvm::toString(P.takeError());
  EXPECT_EQ(P->After, (std::vector<uint8_t>{2, 0, 0, 0x14}));
  reset({0, 0, 0, 0x1a}, Arch::ARM, InstructionMode::ARM);
  P = planPatch(Img, *Input, Overlay, action(PatchKind::BranchAlways));
  ASSERT_TRUE(bool(P)) << llvm::toString(P.takeError());
  EXPECT_EQ(P->After, (std::vector<uint8_t>{0, 0, 0, 0xea}));
  reset({2, 0xd1}, Arch::ARM, InstructionMode::Thumb);
  P = planPatch(Img, *Input, Overlay, action(PatchKind::Nop));
  ASSERT_TRUE(bool(P)) << llvm::toString(P.takeError());
  EXPECT_EQ(P->After, (std::vector<uint8_t>{0, 0xbf}));
}
TEST_F(BytePatch, ARMInterworkingCallValueUsesAnUnconditionalMove) {
  reset({0x00, 0x00, 0x00, 0xfa}, Arch::ARM, InstructionMode::ARM);
  auto R = action(PatchKind::SkipCallValue);
  R.Value = 42;
  auto Plan = planPatch(Img, *Input, Overlay, R);
  ASSERT_TRUE(bool(Plan)) << llvm::toString(Plan.takeError());
  EXPECT_EQ(Plan->After, (std::vector<uint8_t>{0x2a, 0x00, 0xa0, 0xe3}));
}

TEST_F(BytePatch, ThumbSkipValueAndITPredicationBoundary) {
  reset({0x00, 0xf0, 0x00, 0xf8}, Arch::ARM, InstructionMode::Thumb);
  auto R = action(PatchKind::SkipCallValue);
  R.Value = 42;
  auto Plan = planPatch(Img, *Input, Overlay, R);
  ASSERT_TRUE(bool(Plan)) << llvm::toString(Plan.takeError());
  EXPECT_EQ(Plan->After, (std::vector<uint8_t>{0x40, 0xf2, 0x2a, 0x00}));
  reset({0x08, 0xbf, 0x00, 0xf0, 0x00, 0xf8, 0x00, 0xbf}, Arch::ARM,
        InstructionMode::Thumb);
  R.Address = 0x1002;
  Plan = planPatch(Img, *Input, Overlay, R);
  ASSERT_FALSE(bool(Plan));
  llvm::consumeError(Plan.takeError());
}

TEST_F(BytePatch, ThumbEditsCannotChangePredicationOutsideTheirRange) {
  // NOP; IT EQ; MOVS r0, #0. Removing only IT would make the untouched MOV
  // unconditional, even though the reviewed bytes do not include it.
  reset({0x00, 0xbf, 0x08, 0xbf, 0x00, 0x20}, Arch::ARM,
        InstructionMode::Thumb);
  auto R = action(PatchKind::FillNops);
  R.Size = 4;
  auto Plan = planPatch(Img, *Input, Overlay, R);
  ASSERT_FALSE(bool(Plan));
  llvm::consumeError(Plan.takeError());
  R.Size = 6;
  Plan = planPatch(Img, *Input, Overlay, R);
  ASSERT_TRUE(bool(Plan)) << llvm::toString(Plan.takeError());
  EXPECT_EQ(Plan->After,
            (std::vector<uint8_t>{0x00, 0xbf, 0x00, 0xbf, 0x00, 0xbf}));
  R = action(PatchKind::Nop);
  R.Address = 0x1002;
  Plan = planPatch(Img, *Input, Overlay, R);
  ASSERT_FALSE(bool(Plan));
  llvm::consumeError(Plan.takeError());
  R = action(PatchKind::Assemble);
  R.Text = "it eq";
  Plan = planPatch(Img, *Input, Overlay, R);
  ASSERT_FALSE(bool(Plan));
  llvm::consumeError(Plan.takeError());
  EXPECT_TRUE(Overlay.empty());
  EXPECT_TRUE(std::equal(Original.begin(), Original.end(),
                         Img.Segments[0].Data.begin()));
}

TEST_F(BytePatch, OverlapAndPartialRevertReplayMatchesAByteOracle) {
  std::vector<uint8_t> Bytes(128);
  for (size_t I = 0; I < Bytes.size(); ++I)
    Bytes[I] = uint8_t(I);
  reset(Bytes);
  auto Expected = Original;
  std::vector<PatchKind> ExpectedKinds(Bytes.size(), PatchKind::Bytes);
  uint32_t State = 0x831b0ad5;
  const auto Next = [&] {
    State ^= State << 13;
    State ^= State >> 17;
    State ^= State << 5;
    return State;
  };
  for (unsigned Step = 0; Step < 200; ++Step) {
    SCOPED_TRACE(Step);
    const auto First = Next() % Bytes.size();
    const auto Size = 1 + Next() % (Bytes.size() - First);
    if (Step % 3) {
      std::vector<uint8_t> Change(Size);
      for (auto &Byte : Change)
        Byte = uint8_t(Next());
      ASSERT_FALSE(Overlay.apply(Img, *Input, 0x1000 + First, Change,
                                 Step % 2 ? PatchKind::Bytes : PatchKind::Nop));
      std::copy(Change.begin(), Change.end(), Expected.begin() + First);
      std::fill_n(ExpectedKinds.begin() + First, Size,
                  Step % 2 ? PatchKind::Bytes : PatchKind::Nop);
    } else {
      const bool Changed =
          !std::equal(Expected.begin() + First, Expected.begin() + First + Size,
                      Original.begin() + First);
      auto Restored = Overlay.revert(Img, 0x1000 + First, Size);
      ASSERT_EQ(bool(Restored), Changed);
      if (!Restored)
        llvm::consumeError(Restored.takeError());
      std::copy_n(Original.begin() + First, Size, Expected.begin() + First);
    }
    EXPECT_TRUE(std::equal(Expected.begin(), Expected.end(),
                           Img.Segments[0].Data.begin()));
    EXPECT_EQ(Img.Sections[0].Data, Img.Segments[0].Data);
    // An independent byte/tag oracle checks canonical boundaries before
    // replay can normalize a mistakenly split or merged row.
    auto Row = Overlay.rows().begin();
    for (size_t I = 0; I < Expected.size();) {
      if (Expected[I] == Original[I]) {
        ++I;
        continue;
      }
      size_t End = I + 1;
      while (End < Expected.size() && Expected[End] != Original[End] &&
             ExpectedKinds[End] == ExpectedKinds[I])
        ++End;
      ASSERT_NE(Row, Overlay.rows().end());
      EXPECT_EQ(Row->second.FileOffset, I);
      EXPECT_EQ(Row->second.Address, 0x1000 + I);
      EXPECT_EQ(Row->second.size(), End - I);
      EXPECT_EQ(Row->second.Kind, ExpectedKinds[I]);
      ++Row;
      I = End;
    }
    EXPECT_EQ(Row, Overlay.rows().end());
    std::string Text;
    llvm::raw_string_ostream Stream(Text);
    Stream << llvm::json::Value(patchRowsJSON(Overlay));
    auto Rows = parsePatchRows(Text);
    ASSERT_TRUE(bool(Rows)) << llvm::toString(Rows.takeError());
    Overlay.clear(Img);
    ASSERT_FALSE(Overlay.replace(Img, *Input, std::move(*Rows)));
    EXPECT_TRUE(std::equal(Expected.begin(), Expected.end(),
                           Img.Segments[0].Data.begin()));
  }
}

TEST_F(BytePatch, MixedKindRowLimitKeepsLocalEditsCanonical) {
  constexpr auto Count = ::neverd::limits::patch::kMaxPatchRows;
  reset(std::vector<uint8_t>(Count, 0xcc));
  std::vector<PatchRow> Rows;
  Rows.reserve(Count);
  for (size_t I = 0; I < Count; ++I)
    Rows.push_back({I,
                    0x1000 + I,
                    {0xcc},
                    {uint8_t(I % 2 ? 0x90 : 0x91)},
                    I % 2 ? PatchKind::Nop : PatchKind::Bytes});
  ASSERT_FALSE(Overlay.replace(Img, *Input, std::move(Rows)));
  ASSERT_EQ(Overlay.rows().size(), Count);
  // Both outside edges and an interior edit join only matching neighbours.
  ASSERT_FALSE(Overlay.apply(Img, *Input, 0x1000, {0x90}, PatchKind::Nop));
  ASSERT_FALSE(
      Overlay.apply(Img, *Input, 0x1000 + Count - 1, {0x91}, PatchKind::Bytes));
  ASSERT_FALSE(
      Overlay.apply(Img, *Input, 0x1000 + Count / 2, {0x90}, PatchKind::Nop));
  EXPECT_EQ(Overlay.rows().size(), Count - 4);
  EXPECT_EQ(Overlay.rows().at(0).size(), 2u);
  EXPECT_EQ(Overlay.rows().at(Count / 2 - 1).size(), 3u);
  EXPECT_EQ(Overlay.rows().at(Count - 2).size(), 2u);
  for (size_t I = 0; I < Count; ++I) {
    const uint8_t Expected = I == 0 || I == Count / 2 ? 0x90
                             : I == Count - 1         ? 0x91
                             : I % 2                  ? 0x90
                                                      : 0x91;
    ASSERT_EQ(Img.Segments[0].Data[I], Expected) << I;
    ASSERT_EQ(Overlay.originalByte(0x1000 + I), 0xcc) << I;
  }
  EXPECT_EQ(Img.Sections[0].Data, Img.Segments[0].Data);
  Overlay.clear(Img);
  EXPECT_TRUE(std::equal(Original.begin(), Original.end(),
                         Img.Segments[0].Data.begin()));
}

TEST_F(BytePatch, SignedPEExportRequiresConsentAndRecomputesChecksum) {
  std::vector<uint8_t> File(0x408);
  const auto W16 = [&](size_t At, uint16_t V) {
    llvm::support::endian::write16le(File.data() + At, V);
  };
  const auto W32 = [&](size_t At, uint32_t V) {
    llvm::support::endian::write32le(File.data() + At, V);
  };
  File[0] = 'M';
  File[1] = 'Z';
  W32(0x3c, 0x80);
  File[0x80] = 'P';
  File[0x81] = 'E';
  W16(0x84, 0x8664);
  W16(0x86, 1);
  W16(0x94, 240);
  W16(0x96, 2);
  constexpr size_t Opt = 0x98;
  W16(Opt, 0x20b);
  W32(Opt + 16, 0x1000);
  llvm::support::endian::write64le(File.data() + Opt + 24, 0x140000000);
  W32(Opt + 32, 0x1000);
  W32(Opt + 36, 0x200);
  W32(Opt + 56, 0x2000);
  W32(Opt + 60, 0x200);
  W32(Opt + 64, 1);
  W16(Opt + 68, 3);
  W32(Opt + 108, 16);
  constexpr size_t Certificate = Opt + 112 + 4 * 8;
  W32(Certificate, 0x400);
  W32(Certificate + 4, 8);
  constexpr size_t Sec = Opt + 240;
  std::copy_n(".text", 5, File.begin() + Sec);
  W32(Sec + 8, 0x200);
  W32(Sec + 12, 0x1000);
  W32(Sec + 16, 0x200);
  W32(Sec + 20, 0x200);
  W32(Sec + 36, 0x60000020);
  File[0x200] = 0xc3;
  W32(0x400, 8);
  W16(0x404, 0x200);
  W16(0x406, 2);
  reset(File);
  Img.Format = BinaryFormat::COFF;
  auto PEInput = InputFile::open(Path, Img);
  ASSERT_TRUE(bool(PEInput)) << llvm::toString(PEInput.takeError());
  Input = std::move(*PEInput);
  auto HeaderPatch = Overlay.apply(Img, *Input, 0x1000, {0}, PatchKind::Bytes);
  ASSERT_TRUE(bool(HeaderPatch));
  llvm::consumeError(std::move(HeaderPatch));
  auto &S = Img.Segments[0];
  S.VA = 0x140001000;
  S.FileOff = 0x200;
  S.FileSz = S.Size = 0x200;
  S.Data.assign(File.begin() + 0x200, File.begin() + 0x400);
  Img.Sections.clear();
  ASSERT_FALSE(Overlay.apply(Img, *Input, S.VA, {0x90}, PatchKind::Nop));
  auto E = exportPatches(Img, *Input, Overlay, Directory / "signed.exe", false);
  ASSERT_TRUE(bool(E));
  llvm::consumeError(std::move(E));
  EXPECT_FALSE(std::filesystem::exists(Directory / "signed.exe"));
  ASSERT_FALSE(
      exportPatches(Img, *Input, Overlay, Directory / "unsigned.exe", true));
  std::ifstream Stream(Directory / "unsigned.exe", std::ios::binary);
  std::vector<uint8_t> Output(std::istreambuf_iterator<char>(Stream), {});
  ASSERT_EQ(Output.size(), File.size());
  EXPECT_EQ(Output[0x200], 0x90);
  EXPECT_EQ(llvm::support::endian::read64le(Output.data() + Certificate), 0u);
  const auto Sum = computePEChecksum(Output, Opt + 64);
  ASSERT_TRUE(Sum.has_value());
  EXPECT_EQ(llvm::support::endian::read32le(Output.data() + Opt + 64), *Sum);
}

TEST_F(BytePatch, MachOSignatureRequiresExplicitUnsignedExport) {
  std::vector<uint8_t> File(0x130);
  const auto W32 = [&](size_t At, uint32_t V) {
    llvm::support::endian::write32le(File.data() + At, V);
  };
  const auto W64 = [&](size_t At, uint64_t V) {
    llvm::support::endian::write64le(File.data() + At, V);
  };
  W32(0, 0xfeedfacf);
  W32(4, 0x01000007);
  W32(8, 3);
  W32(12, 2);
  W32(16, 2);
  W32(20, 88);
  W32(32, 0x19);
  W32(36, 72);
  std::copy_n("__TEXT", 6, File.begin() + 40);
  W64(56, 0x1000);
  W64(64, 0x1000);
  W64(72, 0);
  W64(80, 0x130);
  W32(88, 5);
  W32(92, 5);
  W32(104, 0x1d);
  W32(108, 16);
  W32(112, 0x120);
  W32(116, 16);
  File[0x100] = 0xc3;
  reset(File);
  Img.Format = BinaryFormat::MachO;
  auto Opened = InputFile::open(Path, Img);
  ASSERT_TRUE(bool(Opened)) << llvm::toString(Opened.takeError());
  Input = std::move(*Opened);
  auto E = Overlay.apply(Img, *Input, 0x1000, {0}, PatchKind::Bytes);
  ASSERT_TRUE(bool(E));
  llvm::consumeError(std::move(E));
  ASSERT_FALSE(Overlay.apply(Img, *Input, 0x1100, {0x90}, PatchKind::Nop));
  E = exportPatches(Img, *Input, Overlay, Directory / "signed.macho");
  ASSERT_TRUE(bool(E));
  llvm::consumeError(std::move(E));
  EXPECT_FALSE(std::filesystem::exists(Directory / "signed.macho"));
  ASSERT_FALSE(
      exportPatches(Img, *Input, Overlay, Directory / "unsigned.macho", true));
  std::ifstream Stream(Directory / "unsigned.macho", std::ios::binary);
  std::vector<uint8_t> Output(std::istreambuf_iterator<char>(Stream), {});
  ASSERT_EQ(Output.size(), File.size());
  EXPECT_EQ(Output[0x100], 0x90);
  EXPECT_EQ(llvm::support::endian::read32le(Output.data() + 16), 1u);
  EXPECT_EQ(llvm::support::endian::read32le(Output.data() + 20), 72u);
}

TEST_F(BytePatch, ImmediateSignExtensionAndControlTargetsAreNotGuessed) {
  reset({0x48, 0x83, 0xc0, 0xff});
  auto R = action(PatchKind::Constant);
  R.Value = 0xff;
  auto P = planPatch(Img, *Input, Overlay, R);
  ASSERT_FALSE(bool(P));
  llvm::consumeError(P.takeError());
  R.Value = 7;
  P = planPatch(Img, *Input, Overlay, R);
  ASSERT_TRUE(bool(P)) << llvm::toString(P.takeError());
  EXPECT_EQ(P->After.back(), 7);
  reset({0xeb, 0});
  P = planPatch(Img, *Input, Overlay, R);
  ASSERT_FALSE(bool(P));
  llvm::consumeError(P.takeError());
}

TEST_F(BytePatch, PreviewLimitsNeverLimitValidationOrHideTruncation) {
  const auto Limit = limits::patch::kMaxPreviewInstructions;
  reset(std::vector<uint8_t>(Limit + 2, 0x90));
  auto R = action(PatchKind::FillNops);
  R.Size = Limit;
  auto P = planPatch(Img, *Input, Overlay, R);
  ASSERT_TRUE(bool(P)) << llvm::toString(P.takeError());
  EXPECT_FALSE(P->CoversTruncated);
  EXPECT_FALSE(P->DecodedTruncated);
  R.Size = Original.size();
  P = planPatch(Img, *Input, Overlay, R);
  ASSERT_TRUE(bool(P)) << llvm::toString(P.takeError());
  EXPECT_EQ(P->Covers.size(), Limit);
  EXPECT_EQ(P->Decoded.size(), Limit);
  EXPECT_TRUE(P->CoversTruncated);
  EXPECT_TRUE(P->DecodedTruncated);
  EXPECT_EQ(P->Before, Original);
  EXPECT_EQ(P->After, Original);
  EXPECT_EQ(P->json().getBoolean("decoded_truncated"), true);
  EXPECT_FALSE(P->Warnings.empty());
  // A raw edit may contain data, but an invalid tail beyond the display limit
  // still has to be reported instead of being mistaken for validated code.
  R.Kind = PatchKind::Bytes;
  R.Bytes = Original;
  R.Bytes.back() = 0x0f;
  P = planPatch(Img, *Input, Overlay, R);
  ASSERT_TRUE(bool(P)) << llvm::toString(P.takeError());
  ASSERT_FALSE(P->Warnings.empty());
  EXPECT_TRUE(P->Warnings.front().getAsString()->contains("do not form"));
  EXPECT_TRUE(Overlay.empty());
}

TEST(PatchAssembler, ARMTargetsDecodeAtRequestedPC) {
  for (const auto Mode : {InstructionMode::ARM, InstructionMode::Thumb})
    for (const auto *Mnemonic : {"b", "bl", "beq"})
      for (uint64_t Target : {0x1010u, 0xff0u}) {
        AssemblyRequest R;
        R.Address = 0x1000;
        R.Mode = Mode;
        R.Text = std::string(Mnemonic) + " " + std::to_string(Target);
        auto Bytes = assemble(Arch::ARM, R);
        ASSERT_TRUE(bool(Bytes))
            << R.Text << ": " << llvm::toString(Bytes.takeError());
        Decoder Dec;
        ASSERT_TRUE(Dec.init(Arch::ARM, Mode));
        DecodedInsn I{};
        ASSERT_TRUE(Dec.decodeOne(Bytes->data(), Bytes->size(), R.Address, I));
        const auto Operands = describeNativeOperands(Dec, I, Arch::ARM, false);
        ASSERT_EQ(Operands.Immediates.size(), 1u);
        EXPECT_EQ(Operands.Immediates.front().Value, Target) << R.Text;
      }
}
TEST(PatchAssembler, DialectsMasmConstantsAndModes) {
  struct Case {
    Arch A;
    InstructionMode Mode;
    bool ATT;
    const char *Text;
    std::vector<uint8_t> Expected;
  };
  for (const Case &C :
       {Case{Arch::X64,
             InstructionMode::Default,
             false,
             "xor eax, eax",
             {0x31, 0xc0}},
        Case{Arch::X64,
             InstructionMode::Default,
             true,
             "xorl %eax, %eax",
             {0x31, 0xc0}},
        Case{Arch::X64, InstructionMode::Default, false, "retn", {0xc3}},
        Case{Arch::X64,
             InstructionMode::Default,
             false,
             "mov al, 30h",
             {0xb0, 0x30}},
        Case{Arch::X64,
             InstructionMode::Default,
             false,
             "mov al, 0x30",
             {0xb0, 0x30}},
        Case{Arch::AArch64,
             InstructionMode::Default,
             false,
             "nop",
             {0x1f, 0x20, 3, 0xd5}},
        Case{Arch::ARM, InstructionMode::Thumb, false, "nop", {0, 0xbf}}}) {
    AssemblyRequest R;
    R.Address = 0x1000;
    R.Mode = C.Mode;
    R.ATT = C.ATT;
    R.Text = C.Text;
    auto Bytes = assemble(C.A, R);
    ASSERT_TRUE(bool(Bytes))
        << C.Text << ": " << llvm::toString(Bytes.takeError());
    EXPECT_EQ(*Bytes, C.Expected) << C.Text;
  }
}
TEST(PatchAssembler, PCRelativeTargetsAndUnresolvedSymbols) {
  AssemblyRequest R;
  R.Address = 0x1000;
  R.Text = "call 0x1010";
  auto Bytes = assemble(Arch::X64, R);
  ASSERT_TRUE(bool(Bytes)) << llvm::toString(Bytes.takeError());
  EXPECT_EQ(*Bytes, (std::vector<uint8_t>{0xe8, 0x0b, 0, 0, 0}));
  R.Text = "b 0x1010";
  Bytes = assemble(Arch::AArch64, R);
  ASSERT_TRUE(bool(Bytes)) << llvm::toString(Bytes.takeError());
  EXPECT_EQ(*Bytes, (std::vector<uint8_t>{4, 0, 0, 0x14}));
  R.Text = "call missing_symbol";
  Bytes = assemble(Arch::X64, R);
  EXPECT_FALSE(bool(Bytes));
  llvm::consumeError(Bytes.takeError());
  R.Text = "jmp short 0x1010";
  Bytes = assemble(Arch::X64, R);
  EXPECT_FALSE(bool(Bytes));
  llvm::consumeError(Bytes.takeError());
  R.Text = "nop; ret";
  Bytes = assemble(Arch::X64, R);
  EXPECT_FALSE(bool(Bytes));
  llvm::consumeError(Bytes.takeError());
}
} // namespace
