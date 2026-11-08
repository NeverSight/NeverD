//===- JumpTableCOFFJoinedTwoLevelTests.cpp - Separately guarded index
//-----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// An MSVC x64 two-level switch whose index byte reaches the dispatch along
/// two paths that bound it separately, as MSVC's CRT does:
///
///   path A: and al, 0Fh; test al, al; je default; cmp al, 20h; ja default
///   path B: and al, 3Fh; cmp al, 20h; jne default
///   movzx eax, al
///   movzx eax, byte ptr [r10+rax+index_rva]
///   mov ecx, [r10+rax*4+table_rva]
///   add rcx, r10
///   jmp rcx
///
/// No guard dominates the dispatch, and the joined domain {1..15, 32} has
/// holes.  A two-level index table's unreached index bytes are unused, so the
/// domain is bounded by its last reaching value.  A path that leaves the byte
/// unbounded, or a single-level table over the same domain, stays unresolved.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/decode/Decoder.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/ir/low/LowIR.h"
#include "neverd/loader/BinaryImage.h"

#include <cstdint>
#include <initializer_list>
#include <set>
#include <string>
#include <vector>

namespace {
using namespace neverd;

constexpr va_t ImageBase = 0x140000000;
constexpr va_t Entry = ImageBase + 0x1000;
constexpr unsigned CaseCount = 3;
constexpr unsigned IndexEntries = 33;

enum class Shape {
  JoinedTwoLevel,    // both paths bound the byte
  UnboundedPath,     // path B jumps to the dispatch with any byte
  JoinedSingleLevel, // the same domain indexes the address table directly
};

struct Writer {
  std::vector<uint8_t> Code;
  void put(std::initializer_list<uint8_t> Bytes) {
    Code.insert(Code.end(), Bytes);
  }
  size_t u32(uint32_t Value) {
    const size_t At = Code.size();
    for (unsigned I = 0; I < 4; ++I)
      Code.push_back(static_cast<uint8_t>(Value >> (8 * I)));
    return At;
  }
  void patch(size_t At, uint32_t Value) {
    for (unsigned I = 0; I < 4; ++I)
      Code[At + I] = static_cast<uint8_t>(Value >> (8 * I));
  }
  void rel32(size_t At, size_t To) {
    patch(At, static_cast<uint32_t>(To - (At + 4)));
  }
  void rel8(size_t At, size_t To) {
    Code[At] = static_cast<uint8_t>(To - (At + 1));
  }
};

/// The case an index selects: 0 for 0, i mod 3 below 16, 2 for the bytes no
/// path reaches and 1 for 32.
unsigned caseOf(unsigned Index) {
  if (Index == 32)
    return 1;
  if (Index >= 16)
    return 2;
  return Index % CaseCount;
}

struct Fixture {
  BinaryImage Image;
  va_t Branch = InvalidVA;
  std::vector<va_t> Cases;
};

Fixture makeFixture(Shape S) {
  Writer W;
  W.put({0x48, 0x83, 0xec, 0x28}); // sub rsp, 28h
  W.put({0x4c, 0x8d, 0x15});       // lea r10, [rip + __ImageBase]
  W.u32(static_cast<uint32_t>(ImageBase - (Entry + W.Code.size() + 4)));
  W.put({0x0f, 0xb6, 0x01});       // movzx eax, byte ptr [rcx]
  W.put({0x84, 0xd2, 0x74, 0x00}); // test dl, dl; je path B
  const size_t ToPathB = W.Code.size() - 1;
  // Path A: a value in 1..15.
  W.put({0x24, 0x0f, 0x84, 0xc0}); // and al, 0Fh; test al, al
  W.put({0x0f, 0x84});             // je default
  const size_t AZero = W.u32(0);
  W.put({0x3c, 0x20, 0x0f, 0x87}); // cmp al, 20h; ja default
  const size_t AAbove = W.u32(0);
  W.put({0xeb, 0x00}); // jmp dispatch
  const size_t AToDispatch = W.Code.size() - 1;
  // Path B: exactly 20h, or any byte.
  W.rel8(ToPathB, W.Code.size());
  size_t BNotEqual = 0;
  if (S != Shape::UnboundedPath) {
    W.put({0x24, 0x3f, 0x3c, 0x20, 0x0f, 0x85}); // and al, 3Fh; cmp al, 20h
    BNotEqual = W.u32(0);                        // jne default
  }
  const size_t Dispatch = W.Code.size();
  W.rel8(AToDispatch, Dispatch);
  W.put({0x0f, 0xb6, 0xc0}); // movzx eax, al
  size_t IndexRVA = 0;
  if (S != Shape::JoinedSingleLevel) {
    W.put({0x41, 0x0f, 0xb6, 0x84, 0x02}); // movzx eax, [r10+rax+index]
    IndexRVA = W.u32(0);
  }
  W.put({0x41, 0x8b, 0x8c, 0x82}); // mov ecx, [r10+rax*4+table]
  const size_t TableRVA = W.u32(0);
  W.put({0x4c, 0x01, 0xd1}); // add rcx, r10
  const va_t Branch = Entry + W.Code.size();
  W.put({0xff, 0xe1}); // jmp rcx
  const unsigned Slots =
      S == Shape::JoinedSingleLevel ? IndexEntries : CaseCount;
  std::vector<va_t> Cases;
  for (unsigned Case = 0; Case < Slots; ++Case) {
    Cases.push_back(Entry + W.Code.size());
    W.put({0xb8}); // mov eax, case + 1
    W.u32(Case + 1);
    W.put({0x48, 0x83, 0xc4, 0x28}); // add rsp, 28h
    W.put({0xc3});                   // retn
  }
  const size_t Default = W.Code.size();
  W.rel32(AZero, Default);
  W.rel32(AAbove, Default);
  if (BNotEqual)
    W.rel32(BNotEqual, Default);
  W.put({0x31, 0xc0, 0x48, 0x83, 0xc4, 0x28, 0xc3}); // xor eax, eax; ...
  const size_t CodeEnd = W.Code.size();
  while (W.Code.size() % 4)
    W.put({0xcc});
  const size_t Table = W.Code.size();
  W.patch(TableRVA, static_cast<uint32_t>(Entry + Table - ImageBase));
  for (va_t Case : Cases)
    W.u32(static_cast<uint32_t>(Case - ImageBase));
  if (IndexRVA) {
    W.patch(IndexRVA, static_cast<uint32_t>(Entry + W.Code.size() - ImageBase));
    for (unsigned Index = 0; Index < IndexEntries; ++Index)
      W.put({static_cast<uint8_t>(caseOf(Index))});
  }
  while (W.Code.size() % 16)
    W.put({0xcc});

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
  Image.Symbols.push_back(Symbol::makeFunc(Entry, CodeEnd));
  Image.Symbols.front().Name = "joined_two_level_fixture";
  Image.KnownCodeRanges.emplace_back(Entry, Entry + CodeEnd);
  ExceptionFunction Unwind;
  Unwind.CodeRange = {Entry, Entry + CodeEnd};
  Unwind.Kind = RuntimeFunctionKind::Primary;
  Unwind.Encoding = ExceptionEncoding::X64UnwindV1;
  Unwind.UnwindVersion = 1;
  Unwind.ParseStatus = ExceptionParseStatus::Complete;
  Image.ExceptionMetadata.Functions.push_back(std::move(Unwind));
  Image.ExceptionMetadata.rebuildIndex();
  return {std::move(Image), Branch, std::move(Cases)};
}

LowFunc recover(const Fixture &F) {
  Decoder D;
  EXPECT_TRUE(D.init(F.Image.Arch, F.Image.Mode));
  CFGBuilder Builder;
  std::set<va_t> Entries{Entry};
  Builder.setKnownFuncEntries(&Entries);
  return Builder.build(F.Image, D, Entry, "joined_two_level_fixture");
}

TEST(COFFJoinedTwoLevel, SeparatelyGuardedIndexBoundsByItsLastValue) {
  const Fixture F = makeFixture(Shape::JoinedTwoLevel);
  const LowFunc Low = recover(F);
  ASSERT_EQ(Low.JumpTables.size(), 1u);
  const JumpTable &Table = Low.JumpTables.front();
  EXPECT_FALSE(Table.AnalysisOnly);
  EXPECT_EQ(Table.InsnAddr, F.Branch);
  // One target per index coordinate up to the last one a path reaches.
  ASSERT_EQ(Table.Targets.size(), IndexEntries);
  for (unsigned Index = 0; Index < IndexEntries; ++Index)
    EXPECT_EQ(Table.Targets[Index], F.Cases[caseOf(Index)]) << Index;
  EXPECT_TRUE(Low.UnsafeIndirectBranchAddresses.empty());
}

TEST(COFFJoinedTwoLevel, AnUnboundedPathOrSingleLevelTableStaysUnresolved) {
  for (const Shape S : {Shape::UnboundedPath, Shape::JoinedSingleLevel}) {
    SCOPED_TRACE(static_cast<int>(S));
    const LowFunc Low = recover(makeFixture(S));
    for (const JumpTable &Table : Low.JumpTables)
      EXPECT_TRUE(Table.AnalysisOnly) << std::hex << Table.InsnAddr;
  }
}

} // namespace
