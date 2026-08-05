//===- FunctionDiscoveryThumbTests.cpp - Thumb function discovery tests --===//

#include "gtest/gtest.h"

#include "neverd/Support/BinaryEncoding.h"
#include "neverd/Support/ISAEncoding.h"
#include "neverd/loader/COFF/COFFLoaderUtils.h"
#include "neverd/loader/FunctionDiscovery.h"

#include "llvm/Object/COFF.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/MemoryBufferRef.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <vector>

namespace {

using namespace neverd;
namespace fs = std::filesystem;

std::vector<uint8_t> readFixture(llvm::StringRef Name) {
  std::ifstream In(fs::path(TEST_OBJ_DIR) / Name.str(), std::ios::binary);
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(In), {});
}

std::unique_ptr<llvm::object::COFFObjectFile>
createCOFFObject(const std::vector<uint8_t> &Bytes) {
  llvm::StringRef Data(reinterpret_cast<const char *>(Bytes.data()),
                       Bytes.size());
  auto ObjOrErr = llvm::object::COFFObjectFile::create(
      llvm::MemoryBufferRef(Data, "FunctionDiscoveryThumb fixture"));
  if (!ObjOrErr) {
    ADD_FAILURE() << llvm::toString(ObjOrErr.takeError());
    return nullptr;
  }
  return std::move(*ObjOrErr);
}

BinaryImage makeThumbImage() {
  BinaryImage Img;
  Img.Arch = Arch::ARM;
  Img.Mode = InstructionMode::Thumb;
  Img.Bits = Bitness::Bits32;
  Img.Base = 0x1000;
  return Img;
}

Segment makeExecutableSegment(va_t VA, llvm::ArrayRef<uint8_t> Bytes) {
  Segment Seg;
  Seg.VA = VA;
  Seg.Size = Bytes.size();
  Seg.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Seg.Data.assign(Bytes.begin(), Bytes.end());
  return Seg;
}

void encodeThumbMovImm16(uint8_t *P, uint16_t Imm) {
  uint16_t Op1 = readLE<uint16_t>(P);
  uint16_t Op2 = readLE<uint16_t>(P + 2);
  Op1 = uint16_t((Op1 & arm::kThumbMovImmOp1Mask) |
                 ((Imm & 0x0800u) >> 1) |
                 ((Imm >> 12) & 0x000fu));
  Op2 = uint16_t((Op2 & arm::kThumbMovIPOp2Mask) |
                 ((Imm & 0x0700u) << 4) | (Imm & 0x00ffu));
  writeLE<uint16_t>(P, Op1);
  writeLE<uint16_t>(P + 2, Op2);
}

std::array<uint8_t, arm::kThumbImportThunkLen>
makeImportThunk(uint32_t IATAddr) {
  std::array<uint8_t, arm::kThumbImportThunkLen> Bytes = {
      0x40, 0xf2, 0x00, 0x0c, // movw ip, low16
      0xc0, 0xf2, 0x00, 0x0c, // movt ip, high16
      0xdc, 0xf8, 0x00, 0xf0, // ldr.w pc, [ip]
  };
  encodeThumbMovImm16(Bytes.data(), uint16_t(IATAddr));
  encodeThumbMovImm16(Bytes.data() + arm::kThumbMovImmInsnSize,
                      uint16_t(IATAddr >> 16));
  return Bytes;
}

TEST(FunctionDiscoveryThumb, NormalizesCodePointersBeforeDiscovery) {
  BinaryImage Img = makeThumbImage();
  constexpr va_t CodeVA = 0x1000;
  const std::array<uint8_t, 4> Prologue = {
      0x10,
      0xb5, // push {r4, lr}
      0x70,
      0x47, // bx lr
  };
  Img.Segments.push_back(makeExecutableSegment(CodeVA, Prologue));

  Segment Rodata;
  Rodata.VA = 0x2000;
  Rodata.Size = sizeof(uint32_t);
  Rodata.Flags = SegmentFlags::Readable;
  Rodata.Data.resize(sizeof(uint32_t));
  writeLE<uint32_t>(Rodata.Data.data(), uint32_t(CodeVA | 1));
  Img.Segments.push_back(std::move(Rodata));

  scanDataFuncPointers(Img);

  auto It = std::find_if(Img.Symbols.begin(), Img.Symbols.end(),
                         [](const Symbol &S) { return S.IsFunc; });
  ASSERT_NE(It, Img.Symbols.end());
  EXPECT_EQ(It->Addr, CodeVA);
  EXPECT_EQ(It->Addr & 1u, 0u);
}

TEST(FunctionDiscoveryThumb, NormalizesOnlyFunctionSymbols) {
  std::vector<uint8_t> Bytes = readFixture("test_patch_coff_arm.obj");
  if (Bytes.empty())
    GTEST_SKIP() << "ARM32 COFF fixture not built";
  auto Obj = createCOFFObject(Bytes);
  ASSERT_NE(Obj, nullptr);

  bool MutatedFunction = false;
  bool MutatedData = false;
  const char *FileBegin = Obj->getData().data();
  for (const llvm::object::SymbolRef &SymRef : Obj->symbols()) {
    llvm::object::COFFSymbolRef Sym = Obj->getCOFFSymbol(SymRef);
    auto NameOrErr = Obj->getSymbolName(Sym);
    ASSERT_TRUE(static_cast<bool>(NameOrErr))
        << llvm::toString(NameOrErr.takeError());
    if (*NameOrErr != "pe_stacky" && *NameOrErr != "g_result")
      continue;

    const char *Raw = static_cast<const char *>(Sym.getRawPtr());
    ASSERT_GE(Raw, FileBegin);
    size_t SymOff = static_cast<size_t>(Raw - FileBegin);
    size_t ValueOff =
        SymOff + offsetof(llvm::object::coff_symbol_generic, Value);
    ASSERT_TRUE(rangeInBounds(ValueOff, sizeof(uint32_t), Bytes.size()));
    writeLE<uint32_t>(Bytes.data() + ValueOff, Sym.getValue() | 1u);
    MutatedFunction |= *NameOrErr == "pe_stacky";
    MutatedData |= *NameOrErr == "g_result";
  }
  ASSERT_TRUE(MutatedFunction);
  ASSERT_TRUE(MutatedData);

  Obj = createCOFFObject(Bytes);
  ASSERT_NE(Obj, nullptr);
  BinaryImage Img = makeThumbImage();
  coff_loader::parseSymbolTable(*Obj, Img, 0);

  auto Find = [&](llvm::StringRef Name) {
    return std::find_if(Img.Symbols.begin(), Img.Symbols.end(),
                        [&](const Symbol &S) { return S.Name == Name; });
  };
  auto Function = Find("pe_stacky");
  ASSERT_NE(Function, Img.Symbols.end());
  EXPECT_TRUE(Function->IsFunc);
  EXPECT_EQ(Function->Addr, 0x10u);

  auto Data = Find("g_result");
  ASSERT_NE(Data, Img.Symbols.end());
  EXPECT_FALSE(Data->IsFunc);
  EXPECT_EQ(Data->Addr, 1u);
}

TEST(FunctionDiscoveryThumb, NormalizesTLSCallbackCodePointers) {
  std::vector<uint8_t> Bytes = readFixture("test_patch_coff_arm.exe");
  if (Bytes.empty())
    GTEST_SKIP() << "ARM32 PE fixture not built";
  auto Obj = createCOFFObject(Bytes);
  ASSERT_NE(Obj, nullptr);

  const llvm::object::data_directory *TLSDir =
      Obj->getDataDirectory(llvm::COFF::TLS_TABLE);
  ASSERT_NE(TLSDir, nullptr);
  const char *FileBegin = Obj->getData().data();
  const char *TLSDirPtr = reinterpret_cast<const char *>(TLSDir);
  ASSERT_GE(TLSDirPtr, FileBegin);
  size_t TLSDirOff = static_cast<size_t>(TLSDirPtr - FileBegin);
  ASSERT_TRUE(rangeInBounds(TLSDirOff, sizeof(*TLSDir), Bytes.size()));

  std::optional<size_t> RawTLSOff;
  uint32_t TLSRVA = 0;
  for (const llvm::object::SectionRef &SecRef : Obj->sections()) {
    auto NameOrErr = SecRef.getName();
    ASSERT_TRUE(static_cast<bool>(NameOrErr))
        << llvm::toString(NameOrErr.takeError());
    if (*NameOrErr != ".rdata")
      continue;
    const llvm::object::coff_section *Sec = Obj->getCOFFSection(SecRef);
    ASSERT_NE(Sec, nullptr);
    ASSERT_GE(uint32_t(Sec->VirtualSize),
              sizeof(llvm::object::coff_tls_directory32));
    uint32_t Offset =
        uint32_t(Sec->VirtualSize) - sizeof(llvm::object::coff_tls_directory32);
    ASSERT_LE(uint64_t(Offset) + sizeof(llvm::object::coff_tls_directory32),
              uint64_t(Sec->SizeOfRawData));
    TLSRVA = uint32_t(Sec->VirtualAddress) + Offset;
    RawTLSOff = uint64_t(Sec->PointerToRawData) + Offset;
    break;
  }
  ASSERT_TRUE(RawTLSOff.has_value());
  ASSERT_TRUE(rangeInBounds(
      *RawTLSOff, sizeof(llvm::object::coff_tls_directory32), Bytes.size()));

  constexpr va_t CallbackTableVA = 0x2800;
  std::fill_n(Bytes.data() + *RawTLSOff,
              sizeof(llvm::object::coff_tls_directory32), uint8_t(0));
  writeLE<uint32_t>(
      Bytes.data() + *RawTLSOff +
          offsetof(llvm::object::coff_tls_directory32, AddressOfCallBacks),
      uint32_t(CallbackTableVA));
  writeLE<uint32_t>(
      Bytes.data() + TLSDirOff +
          offsetof(llvm::object::data_directory, RelativeVirtualAddress),
      TLSRVA);
  writeLE<uint32_t>(Bytes.data() + TLSDirOff +
                        offsetof(llvm::object::data_directory, Size),
                    sizeof(llvm::object::coff_tls_directory32));

  Obj = createCOFFObject(Bytes);
  ASSERT_NE(Obj, nullptr);
  BinaryImage Img = makeThumbImage();
  Segment CallbackTable;
  CallbackTable.VA = CallbackTableVA;
  CallbackTable.Size = 2 * sizeof(uint32_t);
  CallbackTable.Flags = SegmentFlags::Readable;
  CallbackTable.Data.resize(2 * sizeof(uint32_t));
  writeLE<uint32_t>(CallbackTable.Data.data(), 0x1001u);
  writeLE<uint32_t>(CallbackTable.Data.data() + sizeof(uint32_t), 0u);
  Img.Segments.push_back(std::move(CallbackTable));

  coff_loader::parseTLSDirectory(*Obj, Img, 0);

  ASSERT_EQ(Img.Symbols.size(), 1u);
  EXPECT_TRUE(Img.Symbols.front().IsFunc);
  EXPECT_EQ(Img.Symbols.front().Addr, 0x1000u);
}

TEST(FunctionDiscoveryThumb, RecognizesLldImportThunk) {
  BinaryImage Img = makeThumbImage();
  constexpr va_t ThunkVA = 0x1800;
  constexpr va_t IATAddr = 0x12345678;
  const auto Bytes = makeImportThunk(uint32_t(IATAddr));
  Img.Segments.push_back(makeExecutableSegment(ThunkVA, Bytes));

  Import Imp;
  Imp.Name = "imported";
  Imp.IATAddr = IATAddr;
  Img.Imports.push_back(std::move(Imp));

  scanImportThunks(Img);
  scanImportThunks(Img);

  ASSERT_EQ(Img.Symbols.size(), 1u);
  EXPECT_TRUE(Img.Symbols.front().IsFunc);
  EXPECT_EQ(Img.Symbols.front().Addr, ThunkVA);
  EXPECT_EQ(Img.Symbols.front().Addr & 1u, 0u);
  EXPECT_EQ(Img.Symbols.front().Size, arm::kThumbImportThunkLen);
}

TEST(FunctionDiscoveryThumb, RejectsThunkWithDifferentTerminalOpcode) {
  BinaryImage Img = makeThumbImage();
  constexpr va_t IATAddr = 0x12345678;
  auto Bytes = makeImportThunk(uint32_t(IATAddr));
  Bytes[8] ^= 1;
  Img.Segments.push_back(makeExecutableSegment(0x1800, Bytes));

  Import Imp;
  Imp.Name = "imported";
  Imp.IATAddr = IATAddr;
  Img.Imports.push_back(std::move(Imp));

  scanImportThunks(Img);

  EXPECT_TRUE(Img.Symbols.empty());
}

} // namespace
