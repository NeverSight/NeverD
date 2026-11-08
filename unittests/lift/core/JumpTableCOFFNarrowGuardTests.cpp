//===- JumpTableCOFFNarrowGuardTests.cpp - Guards narrower than the index -===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// MSVC x64 image-relative switches whose selector is extended to 64 bits
/// before a narrower range check, as an SEH termination handler reads its
/// selector through the establisher frame:
///
///   movsxd rcx, dword ptr [rax]
///   cmp    ecx, 5
///   ja     default
///   lea    rdx, [__ImageBase]
///   mov    ecx, dword ptr [rdx + rcx*4 + table_rva]
///   add    rcx, rdx
///   jmp    rcx
///
/// The unsigned check of the low 32 bits leaves bit 31 clear, so the
/// sign-extended index is the same small value. Controls keep every shape
/// whose check does not bound the 64-bit index unresolved.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/decode/Decoder.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/ir/low/LowIR.h"
#include "neverd/loader/BinaryImage.h"

#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <set>
#include <string>
#include <vector>

namespace {
using namespace neverd;

constexpr va_t ImageBase = 0x140000000;
constexpr va_t Entry = ImageBase + 0x1000;
constexpr unsigned CaseCount = 6;

/// How the 64-bit index register gets its value.
enum class Producer {
  SignExtendedLoad,     // movsxd rcx, dword ptr [rax]
  SignExtendedRegister, // mov eax, dword ptr [rax]; movsxd rcx, eax
  // mov eax, dword ptr [rax]; test edx, edx; je next | movsxd rcx, eax: the
  // guard's block extends a register that a predecessor block wrote.
  SignExtendedIncomingRegister,
  SignExtendedWordLoad, // movsx rcx, word ptr [rax]
  SignExtendedByteLoad, // movsx rcx, byte ptr [rax]
  ZeroExtendedLoad,     // mov ecx, dword ptr [rax]
  WideLoad,             // mov rcx, qword ptr [rax]
};

/// The width of the range check: cmp cl/cx/ecx/rcx, 5.
enum class Guard { Byte, Word, Dword, Qword };

struct Shape {
  Producer Index = Producer::SignExtendedLoad;
  Guard Check = Guard::Dword;
  bool Signed = false;             // jg instead of ja
  bool RedefineAfterGuard = false; // mov rcx, [rax+8] after the check
  bool TableInsideFunction = true; // the unwind range covers the table

  std::string name() const {
    static const char *const Producers[] = {
        "movsxd-load", "movsxd-reg", "movsxd-incoming-reg", "movsx16-load",
        "movsx8-load", "mov32-load", "mov64-load"};
    static const char *const Guards[] = {"cl", "cx", "ecx", "rcx"};
    return std::string(Producers[static_cast<int>(Index)]) + " cmp " +
           Guards[static_cast<int>(Check)] + (Signed ? " jg" : " ja") +
           (RedefineAfterGuard ? " redefined" : "") +
           (TableInsideFunction ? " table-inside" : " table-after");
  }
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
};

struct Fixture {
  BinaryImage Image;
  va_t Branch = InvalidVA;
  std::vector<va_t> Targets;
};

Fixture makeFixture(const Shape &S) {
  Writer W;
  W.put({0x40, 0x55});             // push rbp
  W.put({0x48, 0x83, 0xec, 0x20}); // sub rsp, 0x20
  W.put({0x48, 0x8b, 0xea});       // mov rbp, rdx (establisher frame)
  W.put({0x48, 0x8b, 0x45, 0x40}); // mov rax, [rbp+0x40]
  switch (S.Index) {
  case Producer::SignExtendedLoad:
    W.put({0x48, 0x63, 0x08});
    break;
  case Producer::SignExtendedRegister:
    W.put({0x8b, 0x00, 0x48, 0x63, 0xc8});
    break;
  case Producer::SignExtendedIncomingRegister:
    W.put({0x8b, 0x00, 0x85, 0xd2, 0x74, 0x00, 0x48, 0x63, 0xc8});
    break;
  case Producer::SignExtendedWordLoad:
    W.put({0x48, 0x0f, 0xbf, 0x08});
    break;
  case Producer::SignExtendedByteLoad:
    W.put({0x48, 0x0f, 0xbe, 0x08});
    break;
  case Producer::ZeroExtendedLoad:
    W.put({0x8b, 0x08});
    break;
  case Producer::WideLoad:
    W.put({0x48, 0x8b, 0x08});
    break;
  }
  switch (S.Check) {
  case Guard::Byte:
    W.put({0x80, 0xf9, 0x05});
    break;
  case Guard::Word:
    W.put({0x66, 0x83, 0xf9, 0x05});
    break;
  case Guard::Dword:
    W.put({0x83, 0xf9, 0x05});
    break;
  case Guard::Qword:
    W.put({0x48, 0x83, 0xf9, 0x05});
    break;
  }
  W.put({0x0f, static_cast<uint8_t>(S.Signed ? 0x8f : 0x87)});
  const size_t ToEpilogue = W.u32(0);
  if (S.RedefineAfterGuard)
    W.put({0x48, 0x8b, 0x48, 0x08}); // mov rcx, [rax+8]
  W.put({0x48, 0x8d, 0x15});         // lea rdx, [rip + __ImageBase]
  W.u32(static_cast<uint32_t>(ImageBase - (Entry + W.Code.size() + 4)));
  W.put({0x8b, 0x8c, 0x8a}); // mov ecx, [rdx + rcx*4 + table_rva]
  const size_t TableRVA = W.u32(0);
  W.put({0x48, 0x03, 0xca}); // add rcx, rdx
  const va_t Branch = Entry + W.Code.size();
  W.put({0xff, 0xe1}); // jmp rcx
  std::vector<va_t> Targets;
  std::vector<size_t> CaseExits;
  for (unsigned Case = 0; Case < CaseCount; ++Case) {
    Targets.push_back(Entry + W.Code.size());
    // mov dword ptr [rbp+0x20], case: the selector's observable effect.
    W.put({0xc7, 0x45, 0x20, static_cast<uint8_t>(Case), 0, 0, 0});
    if (Case + 1 < CaseCount) {
      W.put({0xeb, 0x00}); // jmp epilogue
      CaseExits.push_back(W.Code.size() - 1);
    }
  }
  const size_t Epilogue = W.Code.size();
  for (size_t Exit : CaseExits)
    W.Code[Exit] = static_cast<uint8_t>(Epilogue - (Exit + 1));
  W.patch(ToEpilogue, static_cast<uint32_t>(Epilogue - (ToEpilogue + 4)));
  W.put({0x48, 0x83, 0xc4, 0x20}); // add rsp, 0x20
  W.put({0x5d, 0xc3});             // pop rbp; ret
  const size_t CodeEnd = W.Code.size();
  while (W.Code.size() % 4)
    W.put({0xcc});
  const size_t Table = W.Code.size();
  W.patch(TableRVA, static_cast<uint32_t>(Entry + Table - ImageBase));
  for (va_t Target : Targets)
    W.u32(static_cast<uint32_t>(Target - ImageBase));
  const size_t FunctionEnd = S.TableInsideFunction ? W.Code.size() : CodeEnd;

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
  Image.Symbols.front().Name = "narrow_guard_fixture";
  Image.KnownCodeRanges.emplace_back(Entry, Entry + FunctionEnd);
  // MSVC's unwind record covers the function's table as well as its code.
  ExceptionFunction Unwind;
  Unwind.CodeRange = {Entry, Entry + FunctionEnd};
  Unwind.Kind = RuntimeFunctionKind::Primary;
  Unwind.Encoding = ExceptionEncoding::X64UnwindV1;
  Unwind.UnwindVersion = 1;
  Unwind.ParseStatus = ExceptionParseStatus::Complete;
  Image.ExceptionMetadata.Functions.push_back(std::move(Unwind));
  Image.ExceptionMetadata.rebuildIndex();
  return {std::move(Image), Branch, std::move(Targets)};
}

LowFunc recover(const Fixture &F) {
  Decoder D;
  EXPECT_TRUE(D.init(F.Image.Arch, F.Image.Mode));
  CFGBuilder Builder;
  std::set<va_t> Entries{Entry};
  Builder.setKnownFuncEntries(&Entries);
  return Builder.build(F.Image, D, Entry, "narrow_guard_fixture");
}

void expectExact(const Fixture &F, const LowFunc &Low) {
  ASSERT_EQ(Low.JumpTables.size(), 1u);
  const JumpTable &Table = Low.JumpTables.front();
  EXPECT_FALSE(Table.AnalysisOnly);
  EXPECT_EQ(Table.InsnAddr, F.Branch);
  EXPECT_EQ(Table.Targets, F.Targets);
  EXPECT_TRUE(Table.IsPEImageRelativeRVA);
  EXPECT_TRUE(Low.UnsafeIndirectBranchAddresses.empty());
  EXPECT_TRUE(Low.DecodeFailureAddresses.empty());
  EXPECT_TRUE(Low.UnsupportedInstructionAddresses.empty());
  // Every case is decoded as a block of the function.
  for (va_t Target : F.Targets)
    EXPECT_TRUE(std::any_of(
        Low.Blocks.begin(), Low.Blocks.end(),
        [&](const LowBlock &Block) { return Block.StartAddr == Target; }))
        << std::hex << Target;
}

void expectUnresolved(const LowFunc &Low) {
  for (const JumpTable &Table : Low.JumpTables)
    EXPECT_TRUE(Table.AnalysisOnly) << std::hex << Table.InsnAddr;
}

TEST(COFFNarrowGuard, SignExtendedSelectorUnderUnsignedDwordCheck) {
  for (bool Inside : {true, false}) {
    Shape S;
    S.TableInsideFunction = Inside;
    SCOPED_TRACE(S.name());
    const auto F = makeFixture(S);
    expectExact(F, recover(F));
  }
}

TEST(COFFNarrowGuard, ExtendedSelectorsTheCheckCovers) {
  for (const Shape S :
       {Shape{Producer::SignExtendedRegister, Guard::Dword},
        Shape{Producer::SignExtendedIncomingRegister, Guard::Dword},
        Shape{Producer::SignExtendedWordLoad, Guard::Word},
        Shape{Producer::SignExtendedByteLoad, Guard::Byte},
        Shape{Producer::ZeroExtendedLoad, Guard::Dword},
        Shape{Producer::SignExtendedLoad, Guard::Qword},
        Shape{Producer::WideLoad, Guard::Qword}}) {
    SCOPED_TRACE(S.name());
    const auto F = makeFixture(S);
    expectExact(F, recover(F));
  }
}

TEST(COFFNarrowGuard, ChecksThatLeaveTheIndexUnboundedStayUnresolved) {
  Shape Signed;
  Signed.Signed = true;
  Shape SignedZeroExtended{Producer::ZeroExtendedLoad, Guard::Dword};
  SignedZeroExtended.Signed = true;
  Shape Redefined;
  Redefined.RedefineAfterGuard = true;
  Shape SignedIncoming{Producer::SignExtendedIncomingRegister, Guard::Dword};
  SignedIncoming.Signed = true;
  for (const Shape S :
       {Shape{Producer::SignExtendedLoad, Guard::Word},
        Shape{Producer::SignExtendedIncomingRegister, Guard::Word},
        Shape{Producer::SignExtendedWordLoad, Guard::Byte},
        Shape{Producer::ZeroExtendedLoad, Guard::Word},
        Shape{Producer::WideLoad, Guard::Dword}, Signed, SignedZeroExtended,
        SignedIncoming, Redefined}) {
    SCOPED_TRACE(S.name());
    expectUnresolved(recover(makeFixture(S)));
  }
}

} // namespace
