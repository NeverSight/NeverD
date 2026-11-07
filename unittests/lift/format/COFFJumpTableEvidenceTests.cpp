//===- COFFJumpTableEvidenceTests.cpp - Authenticated dispatch storage ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Checks exact table ownership and callable-slot roles in linked i386 PE.
//===----------------------------------------------------------------------===//

#include "../../loader/PEHeaderFixture.h"
#include "gtest/gtest.h"

#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/ir/low/FuncDetector.h"
#include "neverd/loader/COFF/COFFLoader.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/Support/Error.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <set>
#include <string>
#include <vector>

using namespace neverd;

namespace {
enum class JumpShape { Reached, Dead, Segmented, Narrow, WrongScale, Called };

std::string jumpPE(JumpShape Shape) {
  using namespace llvm::COFF;
  using namespace llvm::object;
  auto F = test::peImageBaseWalker(IMAGE_FILE_MACHINE_I386, false);
  F.Bytes.resize(0x800, '\0');
  auto *Bytes = reinterpret_cast<uint8_t *>(F.Bytes.data());
  auto PE = locatePEHeaders(Bytes, F.Bytes.size());
  PE.FileHeader->NumberOfSections = 3;
  auto *Sections = reinterpret_cast<coff_section *>(PE.SectionTable);
  auto &Text = Sections[0];
  Text.VirtualSize = 0x200;
  uint8_t *Code = Bytes + Text.PointerToRawData;
  std::fill_n(Code, 0x200, 0x90);
  auto &Data = Sections[2];
  std::memset(&Data, 0, sizeof(Data));
  std::memcpy(Data.Name, ".rdata", 6);
  Data.VirtualAddress = 0x3000;
  Data.VirtualSize = 8;
  Data.PointerToRawData = 0x600;
  Data.SizeOfRawData = 0x200;
  Data.Characteristics = IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ;
  setPESizeOfImage(PE, 0x4000);

  std::vector<uint16_t> CodeFixups;
  unsigned Offset = 0;
  if (Shape == JumpShape::Dead) {
    Code[0] = 0xc3;
    Offset = 0x10;
  }
  auto Read = [&](bool Call) {
    if (Shape == JumpShape::Segmented)
      Code[Offset++] = 0x64;
    if (Shape == JumpShape::Narrow)
      Code[Offset++] = 0x66;
    Code[Offset++] = 0xff;
    Code[Offset++] = Call ? 0x14 : 0x24;
    Code[Offset++] = Shape == JumpShape::WrongScale ? 0x45 : 0x85;
    CodeFixups.push_back(Offset);
    writeLE<uint32_t>(Code + Offset, F.Base + 0x3000);
    Offset += 4;
  };
  if (Shape == JumpShape::Called)
    Read(true);
  Read(false);
  const uint8_t Callback[] = {0x55, 0x89, 0xe5, 0x31, 0xc0, 0x5d, 0xc3};
  for (unsigned At : {0x80, 0x90})
    std::copy(std::begin(Callback), std::end(Callback), Code + At);
  writeLE<uint32_t>(Bytes + 0x600, F.Entry + 0x80);
  writeLE<uint32_t>(Bytes + 0x604, F.Entry + 0x90);
  uint8_t *Reloc = Bytes + Sections[1].PointerToRawData;
  std::fill_n(Reloc, 0x200, 0);
  writeLE<uint32_t>(Reloc, 0x1000);
  writeLE<uint32_t>(Reloc + 4, 12);
  for (unsigned I = 0; I < CodeFixups.size(); ++I)
    writeLE<uint16_t>(Reloc + 8 + 2 * I, 0x3000 | CodeFixups[I]);
  writeLE<uint32_t>(Reloc + 12, 0x3000);
  writeLE<uint32_t>(Reloc + 16, 12);
  writeLE<uint16_t>(Reloc + 20, 0x3000);
  writeLE<uint16_t>(Reloc + 22, 0x3004);
  Sections[1].VirtualSize = 24;
  setPEDataDirectory(PE, BASE_RELOCATION_TABLE, 0x2000, 24);
  return F.Bytes;
}
} // namespace

TEST(COFFJumpTableEvidence, NonDispatchBytesKeepRelocatedCallbacks) {
  for (auto Shape : {JumpShape::Dead, JumpShape::Segmented, JumpShape::Narrow,
                     JumpShape::WrongScale}) {
    SCOPED_TRACE(static_cast<int>(Shape));
    auto Bytes = jumpPE(Shape);
    auto Img =
        COFFLoader().loadBuffer(llvm::MemoryBufferRef(Bytes, "jump.exe"));
    ASSERT_TRUE(bool(Img)) << llvm::toString(Img.takeError());
    ASSERT_EQ(Img->CodePtrRelocSlots, (std::set<va_t>{0x403000, 0x403004}));
    std::set<va_t> Functions;
    for (const Symbol &S : Img->Symbols)
      if (S.IsFunc && !S.IsBoundaryGuess)
        Functions.insert(S.Addr);
    EXPECT_TRUE(Functions.count(0x401080));
    EXPECT_TRUE(Functions.count(0x401090));
  }
}

TEST(COFFJumpTableEvidence, ReachedJumpKeepsCasesInsideDispatcher) {
  auto Bytes = jumpPE(JumpShape::Reached);
  auto Img = COFFLoader().loadBuffer(llvm::MemoryBufferRef(Bytes, "jump.exe"));
  ASSERT_TRUE(bool(Img)) << llvm::toString(Img.takeError());
  for (const Symbol &S : Img->Symbols)
    if (S.IsFunc) {
      EXPECT_NE(S.Addr, 0x401080u);
      EXPECT_NE(S.Addr, 0x401090u);
    }
  Decoder Dec;
  ASSERT_TRUE(Dec.init(*Img));
  const auto Functions = FuncDetector().detect(*Img, Dec);
  ASSERT_EQ(Functions.size(), 1u);
  EXPECT_EQ(Functions[0].first, 0x401000u);
}

TEST(COFFJumpTableEvidence, SharedCallAndJumpTableKeepsCallableEntries) {
  auto Bytes = jumpPE(JumpShape::Called);
  auto Img = COFFLoader().loadBuffer(llvm::MemoryBufferRef(Bytes, "call.exe"));
  ASSERT_TRUE(bool(Img)) << llvm::toString(Img.takeError());
  Decoder Dec;
  ASSERT_TRUE(Dec.init(*Img));
  std::set<va_t> Functions;
  for (const auto &[Addr, Name] : FuncDetector().detect(*Img, Dec))
    Functions.insert(Addr);
  EXPECT_EQ(Functions, (std::set<va_t>{0x401000, 0x401080, 0x401090}));
}

TEST(COFFJumpTableEvidence, AdjacentNamedObjectKeepsItsCallback) {
  using namespace llvm::COFF;
  auto Bytes = jumpPE(JumpShape::Reached);
  Bytes.resize(0x800 + 18 + 4, '\0');
  auto *Raw = reinterpret_cast<uint8_t *>(Bytes.data());
  auto PE = locatePEHeaders(Raw, Bytes.size());
  auto *Sections =
      reinterpret_cast<llvm::object::coff_section *>(PE.SectionTable);
  Sections[2].VirtualSize = 12;
  writeLE<uint32_t>(Raw + 0x608, 0x4010a0);
  const uint8_t Callback[] = {0x55, 0x89, 0xe5, 0x31, 0xc0, 0x5d, 0xc3};
  std::copy(std::begin(Callback), std::end(Callback), Raw + 0x2a0);
  writeLE<uint32_t>(Raw + 0x410, 16);
  writeLE<uint16_t>(Raw + 0x418, 0x3008);
  Sections[1].VirtualSize = 28;
  setPEDataDirectory(PE, BASE_RELOCATION_TABLE, 0x2000, 28);
  PE.FileHeader->PointerToSymbolTable = 0x800;
  PE.FileHeader->NumberOfSymbols = 1;
  std::memcpy(Raw + 0x800, "callback", 8);
  writeLE<uint32_t>(Raw + 0x808, 8);
  writeLE<uint16_t>(Raw + 0x80c, 3);
  Raw[0x810] = IMAGE_SYM_CLASS_EXTERNAL;
  writeLE<uint32_t>(Raw + 0x812, 4);
  auto Img =
      COFFLoader().loadBuffer(llvm::MemoryBufferRef(Bytes, "neighbor.exe"));
  ASSERT_TRUE(bool(Img)) << llvm::toString(Img.takeError());
  EXPECT_TRUE(std::any_of(
      Img->Symbols.begin(), Img->Symbols.end(),
      [](const Symbol &S) { return S.IsFunc && S.Addr == 0x4010a0; }));
  Decoder Dec;
  ASSERT_TRUE(Dec.init(*Img));
  std::set<va_t> Functions;
  for (const auto &[Addr, Name] : FuncDetector().detect(*Img, Dec))
    Functions.insert(Addr);
  EXPECT_EQ(Functions, (std::set<va_t>{0x401000, 0x4010a0}));
}

TEST(COFFJumpTableEvidence, MaskDoesNotAuthorizeScalarCallTargets) {
  for (bool Relocatable : {false, true})
    for (bool AllCallable : {false, true}) {
      SCOPED_TRACE(Relocatable);
      SCOPED_TRACE(AllCallable);
      auto Bytes = jumpPE(JumpShape::Dead);
      auto Image =
          COFFLoader().loadBuffer(llvm::MemoryBufferRef(Bytes, "mask.exe"));
      ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
      Image->IsRelocatable = Relocatable;
      if (!AllCallable) {
        Image->CodePtrRelocSlots.erase(0x403004);
        for (auto &Segment : Image->Segments)
          if (Segment.contains(0x403004))
            writeLE<uint32_t>(Segment.Data.data() + 0x403004 - Segment.VA, 42);
      }
      auto Temp = [](unsigned Id) {
        MedVar V;
        V.Kind = MedVar::Temp;
        V.TheArch = Arch::X86;
        V.Id = Id;
        V.Size = 4;
        V.SSAVer = 1;
        return V;
      };
      MedFunc Caller;
      Caller.Name = "masked_call";
      Caller.Entry = 0x401000;
      Caller.ReturnType = NdType::makeVoid();
      Caller.Blocks.emplace_back();
      auto &B = Caller.Blocks.back();
      B.Id = 0;
      B.StartAddr = Caller.Entry;
      B.EndAddr = Caller.Entry + 20;
      MedVar Index = Temp(0);
      Index.Kind = MedVar::Param;
      Caller.Params.push_back(Index);
      auto Op = [&](NdOp Opcode, MedVar Out,
                    std::initializer_list<MedVar> Inputs) {
        MedOp O;
        O.Opcode = Opcode;
        O.Output = Out;
        O.Addr = Caller.Entry + B.Ops.size() * 4;
        for (auto V : Inputs)
          O.addInput(V);
        B.Ops.push_back(O);
      };
      Op(NdOp::INT_AND, Temp(1), {Index, MedVar::makeConst(4, 4)});
      Op(NdOp::INT_ADD, Temp(2),
         {MedVar::makeConst(0x403000, 4,
                            ConstantAddressProvenance::DataAddress),
          Temp(1)});
      Op(NdOp::LOAD, Temp(3), {Temp(2)});
      Op(NdOp::INDIR_CALL, {}, {Temp(3)});
      Op(NdOp::RETURN, {}, {});
      std::vector<MedFunc> Funcs{Caller};
      for (va_t Addr : {0x401080u, 0x401090u}) {
        MedFunc Callee;
        Callee.Name = "callback_" + std::to_string(Addr);
        Callee.Entry = Addr;
        Callee.ReturnType = NdType::makeVoid();
        Callee.Blocks.emplace_back();
        Callee.Blocks.back().Id = 0;
        Callee.Blocks.back().StartAddr = Addr;
        Callee.Blocks.back().EndAddr = Addr + 1;
        MedOp Return;
        Return.Opcode = NdOp::RETURN;
        Return.Addr = Addr;
        Callee.Blocks.back().Ops.push_back(Return);
        Funcs.push_back(Callee);
      }
      llvm::LLVMContext Context;
      auto Module = MedLLVMEmitter().emit(Funcs, Context, "mask", Arch::X86, {},
                                          &*Image, BinaryFormat::COFF);
      EXPECT_EQ(bool(Module), AllCallable);
    }
}

TEST(COFFJumpTableEvidence, TableTargetsCanReachAnotherDispatch) {
  for (bool Call : {false, true}) {
    SCOPED_TRACE(Call);
    auto Bytes = jumpPE(JumpShape::Reached);
    auto *Raw = reinterpret_cast<uint8_t *>(Bytes.data());
    auto PE = locatePEHeaders(Raw, Bytes.size());
    auto *Sections =
        reinterpret_cast<llvm::object::coff_section *>(PE.SectionTable);
    uint8_t *Code = Raw + Sections[0].PointerToRawData;
    Code[1] = Call ? 0x14 : 0x24;
    Code[7] = 0xc3;
    std::fill_n(Code + 0x80, 0x80, 0x90);
    // This second dispatcher is reached only through the first table.
    const uint8_t Nested[] = {0x55, 0x89, 0xe5, 0xff, 0x24, 0x85};
    std::copy(std::begin(Nested), std::end(Nested), Code + 0x80);
    writeLE<uint32_t>(Code + 0x86, 0x403004);
    const uint8_t Case[] = {0xb8, 11, 0, 0, 0, 0x5d, 0xc3};
    for (unsigned At : {0xa0, 0xb0})
      std::copy(std::begin(Case), std::end(Case), Code + At);
    writeLE<uint32_t>(Raw + 0x600, 0x401080);
    writeLE<uint32_t>(Raw + 0x604, 0x4010a0);
    writeLE<uint32_t>(Raw + 0x608, 0x4010b0);
    Sections[2].VirtualSize = 12;
    writeLE<uint16_t>(Raw + 0x40a, 0x3086);
    writeLE<uint32_t>(Raw + 0x410, 16);
    writeLE<uint16_t>(Raw + 0x418, 0x3008);
    Sections[1].VirtualSize = 28;
    setPEDataDirectory(PE, llvm::COFF::BASE_RELOCATION_TABLE, 0x2000, 28);
    auto Img =
        COFFLoader().loadBuffer(llvm::MemoryBufferRef(Bytes, "nested.exe"));
    ASSERT_TRUE(bool(Img)) << llvm::toString(Img.takeError());
    Decoder Dec;
    ASSERT_TRUE(Dec.init(*Img));
    std::set<va_t> Functions;
    for (const auto &[Addr, Name] : FuncDetector().detect(*Img, Dec))
      Functions.insert(Addr);
    std::set<va_t> Expected{0x401000};
    if (Call)
      Expected.insert(0x401080);
    EXPECT_EQ(Functions, Expected);
  }
}
