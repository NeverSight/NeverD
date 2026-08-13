//===- COFFARMFormatTestsDetail.h - Windows ARM PE test harness -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Fixture and byte-level PE readers shared by the COFFARMFormat*
// translation units.  The fixture class lives in a named namespace and
// every free function is `inline`, because gtest requires one fixture
// type per suite name across the whole binary and the linker requires
// one definition per helper.
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_UNITTESTS_LIFT_FORMAT_COFFARMFORMATTESTSDETAIL_H
#define NEVERD_UNITTESTS_LIFT_FORMAT_COFFARMFORMATTESTSDETAIL_H

#include "gtest/gtest.h"

#include "NeverDLiftFixture.h"
#include "neverd/support/BinaryEncoding.h"
#include "neverd/support/BinaryLoading.h"
#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/backend/codegen/BinaryRewriter.h"
#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/object/SectionNames.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/loader/FunctionDiscovery.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Object/COFF.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/MemoryBufferRef.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace neverd::coff_arm_test {

inline fs::path fixture(llvm::StringRef Name) {
  return fs::path(TEST_OBJ_DIR) / Name.str();
}

class COFFARMFormat : public ::testing::Test {
protected:
  void SetUp() override {
    const auto *Info = ::testing::UnitTest::GetInstance()->current_test_info();
    TempDir = fs::temp_directory_path() /
              (std::string("neverd_coff_arm_") + Info->name());
    fs::remove_all(TempDir);
    fs::create_directories(TempDir);
  }

  void TearDown() override {
    if (!::testing::Test::HasFailure())
      fs::remove_all(TempDir);
  }

  fs::path writeMutation(llvm::StringRef Name,
                         const std::vector<uint8_t> &Bytes) const {
    fs::path Path = TempDir / Name.str();
    std::fstream Out(Path, std::ios::binary | std::ios::out | std::ios::trunc);
    Out.write(reinterpret_cast<const char *>(Bytes.data()), Bytes.size());
    EXPECT_TRUE(Out.good());
    return Path;
  }

private:
  fs::path TempDir;
};

inline std::vector<uint8_t> readFile(const fs::path &Path) {
  std::ifstream In(Path, std::ios::binary);
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(In), {});
}

inline std::unique_ptr<llvm::object::COFFObjectFile>
createCOFFObject(const std::vector<uint8_t> &Bytes) {
  llvm::StringRef Data(reinterpret_cast<const char *>(Bytes.data()),
                       Bytes.size());
  auto ObjOrErr = llvm::object::COFFObjectFile::create(
      llvm::MemoryBufferRef(Data, "COFFARMFormat fixture"));
  if (!ObjOrErr) {
    ADD_FAILURE() << llvm::toString(ObjOrErr.takeError());
    return nullptr;
  }
  return std::move(*ObjOrErr);
}

inline size_t exceptionDirectoryFileOffset(const llvm::object::COFFObjectFile &Obj) {
  const llvm::object::data_directory *Dir =
      Obj.getDataDirectory(llvm::COFF::EXCEPTION_TABLE);
  EXPECT_NE(Dir, nullptr);
  if (!Dir)
    return 0;
  const char *FileBegin = Obj.getData().data();
  const char *DirPtr = reinterpret_cast<const char *>(Dir);
  EXPECT_GE(DirPtr, FileBegin);
  EXPECT_LE(DirPtr + sizeof(*Dir), FileBegin + Obj.getData().size());
  return static_cast<size_t>(DirPtr - FileBegin);
}

inline std::optional<size_t> rvaFileOffset(const llvm::object::COFFObjectFile &Obj,
                                    uint32_t RVA) {
  uintptr_t Ptr = 0;
  if (llvm::Error Err = Obj.getRvaPtr(RVA, Ptr)) {
    llvm::consumeError(std::move(Err));
    return std::nullopt;
  }
  uintptr_t FileBegin = reinterpret_cast<uintptr_t>(Obj.getData().data());
  uintptr_t FileEnd = FileBegin + Obj.getData().size();
  if (Ptr < FileBegin || Ptr > FileEnd)
    return std::nullopt;
  return static_cast<size_t>(Ptr - FileBegin);
}

inline std::optional<uint32_t> rawExportRVA(const llvm::object::COFFObjectFile &Obj,
                                     llvm::StringRef Name) {
  for (auto I = Obj.export_directory_begin(), E = Obj.export_directory_end();
       I != E; ++I) {
    llvm::StringRef ExportName;
    if (llvm::Error Err = I->getSymbolName(ExportName)) {
      llvm::consumeError(std::move(Err));
      continue;
    }
    if (ExportName != Name)
      continue;
    uint32_t RVA = 0;
    if (llvm::Error Err = I->getExportRVA(RVA)) {
      llvm::consumeError(std::move(Err));
      return std::nullopt;
    }
    return RVA;
  }
  return std::nullopt;
}

inline const Export *findExport(const BinaryImage &Img, llvm::StringRef Name) {
  auto It = std::find_if(Img.Exports.begin(), Img.Exports.end(),
                         [&](const Export &E) { return E.Name == Name; });
  return It == Img.Exports.end() ? nullptr : &*It;
}

inline std::optional<size_t>
exportAddressEntryFileOffset(const llvm::object::COFFObjectFile &Obj,
                             llvm::StringRef Name) {
  const llvm::object::export_directory_table_entry *Table =
      Obj.getExportTable();
  if (!Table)
    return std::nullopt;

  uint32_t Index = 0;
  bool Found = false;
  for (auto I = Obj.export_directory_begin(), E = Obj.export_directory_end();
       I != E; ++I) {
    llvm::StringRef ExportName;
    if (llvm::Error Err = I->getSymbolName(ExportName)) {
      llvm::consumeError(std::move(Err));
      continue;
    }
    if (ExportName != Name)
      continue;
    uint32_t Ordinal = 0;
    if (llvm::Error Err = I->getOrdinal(Ordinal)) {
      llvm::consumeError(std::move(Err));
      return std::nullopt;
    }
    if (Ordinal < uint32_t(Table->OrdinalBase))
      return std::nullopt;
    Index = Ordinal - uint32_t(Table->OrdinalBase);
    Found = true;
    break;
  }
  if (!Found || Index >= uint32_t(Table->AddressTableEntries))
    return std::nullopt;

  auto AddressTableOff =
      rvaFileOffset(Obj, uint32_t(Table->ExportAddressTableRVA));
  if (!AddressTableOff)
    return std::nullopt;
  uint64_t EntryOff =
      *AddressTableOff +
      uint64_t(Index) * sizeof(llvm::object::export_address_table_entry);
  if (!rangeInBounds(EntryOff, sizeof(llvm::object::export_address_table_entry),
                     Obj.getData().size()))
    return std::nullopt;
  return static_cast<size_t>(EntryOff);
}

inline std::optional<uint32_t> oddDataRVA(const llvm::object::COFFObjectFile &Obj) {
  for (const llvm::object::SectionRef &SecRef : Obj.sections()) {
    auto NameOrErr = SecRef.getName();
    if (!NameOrErr) {
      llvm::consumeError(NameOrErr.takeError());
      continue;
    }
    if (*NameOrErr != section_names::coff::Data)
      continue;
    const llvm::object::coff_section *Sec = Obj.getCOFFSection(SecRef);
    if (!Sec)
      return std::nullopt;
    uint32_t Size = Sec->VirtualSize ? uint32_t(Sec->VirtualSize)
                                     : uint32_t(Sec->SizeOfRawData);
    if (Size < 2)
      return std::nullopt;
    return uint32_t(Sec->VirtualAddress) + 1u;
  }
  return std::nullopt;
}

inline constexpr size_t PDataEntrySize = 2 * sizeof(uint32_t);

inline std::optional<std::array<size_t, 3>>
firstThreePDataEntryOffsets(const llvm::object::COFFObjectFile &Obj) {
  const llvm::object::data_directory *Dir =
      Obj.getDataDirectory(llvm::COFF::EXCEPTION_TABLE);
  if (!Dir || static_cast<size_t>(Dir->Size) / PDataEntrySize < 3u)
    return std::nullopt;
  auto TableOff = rvaFileOffset(Obj, Dir->RelativeVirtualAddress);
  if (!TableOff ||
      !rangeInBounds(*TableOff, 3 * PDataEntrySize, Obj.getData().size()))
    return std::nullopt;
  return std::array<size_t, 3>{*TableOff, *TableOff + PDataEntrySize,
                               *TableOff + 2 * PDataEntrySize};
}

inline void swapPDataEntries(std::vector<uint8_t> &Bytes, size_t A, size_t B) {
  std::array<uint8_t, PDataEntrySize> Temp;
  std::memcpy(Temp.data(), Bytes.data() + A, PDataEntrySize);
  std::memcpy(Bytes.data() + A, Bytes.data() + B, PDataEntrySize);
  std::memcpy(Bytes.data() + B, Temp.data(), PDataEntrySize);
}

struct FullFunctionExpectation {
  va_t Addr;
  uint32_t Length;
};

inline std::optional<FullFunctionExpectation>
readAArch64PackedFull(const std::vector<uint8_t> &Bytes, uint64_t ImageBase,
                      size_t EntryOff) {
  if (!rangeInBounds(EntryOff, PDataEntrySize, Bytes.size()))
    return std::nullopt;
  uint32_t Begin = readLE<uint32_t>(Bytes.data() + EntryOff);
  uint32_t Unwind =
      readLE<uint32_t>(Bytes.data() + EntryOff + sizeof(uint32_t));
  if ((Unwind & 3u) != 1u)
    return std::nullopt;
  uint32_t Length = ((Unwind & 0x1ffcu) >> 2) * 4u;
  if (Length == 0 || Begin > InvalidVA - ImageBase)
    return std::nullopt;
  return FullFunctionExpectation{ImageBase + Begin, Length};
}

inline void expectFullFunctionPresent(const BinaryImage &Img,
                               const FullFunctionExpectation &Expected) {
  EXPECT_NE(
      std::find(Img.KnownCodeRanges.begin(), Img.KnownCodeRanges.end(),
                std::make_pair(Expected.Addr, Expected.Addr + Expected.Length)),
      Img.KnownCodeRanges.end());
  EXPECT_NE(std::find_if(Img.Symbols.begin(), Img.Symbols.end(),
                         [&](const Symbol &S) {
                           return S.IsFunc && S.Addr == Expected.Addr &&
                                  S.Size == Expected.Length;
                         }),
            Img.Symbols.end());
}

inline std::optional<size_t>
findPDataEntryOffsetByFlag(const llvm::object::COFFObjectFile &Obj,
                           uint32_t WantedFlag) {
  const llvm::object::data_directory *Dir =
      Obj.getDataDirectory(llvm::COFF::EXCEPTION_TABLE);
  if (!Dir)
    return std::nullopt;
  auto TableOff = rvaFileOffset(Obj, Dir->RelativeVirtualAddress);
  if (!TableOff)
    return std::nullopt;
  constexpr size_t EntrySize = 2 * sizeof(uint32_t);
  size_t Count = Dir->Size / EntrySize;
  for (size_t I = 0; I < Count; ++I) {
    size_t EntryOff = *TableOff + I * EntrySize;
    if (!rangeInBounds(EntryOff, EntrySize, Obj.getData().size()))
      break;
    const auto *Entry =
        reinterpret_cast<const uint8_t *>(Obj.getData().data()) + EntryOff;
    uint32_t UnwindWord = readLE<uint32_t>(Entry + sizeof(uint32_t));
    if (UnwindWord != 0 && (UnwindWord & 3u) == WantedFlag)
      return EntryOff;
  }
  return std::nullopt;
}

inline std::optional<size_t>
findUnpackedPDataEntryOffset(const llvm::object::COFFObjectFile &Obj) {
  return findPDataEntryOffsetByFlag(Obj, 0u);
}

inline std::optional<size_t> xdataFileOffset(const std::vector<uint8_t> &Bytes,
                                      const llvm::object::COFFObjectFile &Obj,
                                      const BinaryImage &Img,
                                      size_t PDataEntryOff) {
  if (!rangeInBounds(PDataEntryOff, 2 * sizeof(uint32_t), Bytes.size()))
    return std::nullopt;
  uint32_t UnwindWord =
      readLE<uint32_t>(Bytes.data() + PDataEntryOff + sizeof(uint32_t));
  uint32_t XDataRVA = UnwindWord & ~3u;
  if (XDataRVA > InvalidVA - Obj.getImageBase())
    return std::nullopt;
  va_t XDataVA = Obj.getImageBase() + XDataRVA;
  for (const Section &Sec : Img.Sections) {
    if (!Sec.contains(XDataVA))
      continue;
    uint64_t Delta = XDataVA - Sec.VA;
    if (Delta > InvalidVA - Sec.FileOff)
      return std::nullopt;
    uint64_t Off = Sec.FileOff + Delta;
    if (!rangeInBounds(Off, sizeof(uint32_t), Bytes.size()))
      return std::nullopt;
    return static_cast<size_t>(Off);
  }
  return std::nullopt;
}

inline uint64_t maxFunctionSize(const BinaryImage &Img) {
  uint64_t Max = 0;
  for (const auto &Sym : Img.Symbols)
    if (Sym.IsFunc)
      Max = std::max(Max, Sym.Size);
  return Max;
}

inline void expectAllFunctionRangesInsideExecutableSegments(const BinaryImage &Img) {
  for (const auto &[Begin, End] : Img.KnownCodeRanges) {
    ASSERT_LT(Begin, End);
    const Segment *Seg = Img.getSegmentFor(Begin);
    ASSERT_NE(Seg, nullptr);
    ASSERT_TRUE(Seg->isExecutable());
    ASSERT_LE(End - Seg->VA, std::min<uint64_t>(Seg->Size, Seg->Data.size()));
  }
}

inline void expectFullPDataStartsHaveBoundedSymbols(const BinaryImage &Img,
                                             const fs::path &Path) {
  std::vector<uint8_t> Bytes = readFile(Path);
  auto Obj = createCOFFObject(Bytes);
  ASSERT_NE(Obj, nullptr);
  const llvm::object::data_directory *Dir =
      Obj->getDataDirectory(llvm::COFF::EXCEPTION_TABLE);
  ASSERT_NE(Dir, nullptr);

  uintptr_t TablePtr = 0;
  llvm::Error Err = Obj->getRvaPtr(Dir->RelativeVirtualAddress, TablePtr);
  ASSERT_FALSE(static_cast<bool>(Err)) << llvm::toString(std::move(Err));
  uintptr_t FileBegin = reinterpret_cast<uintptr_t>(Obj->getData().data());
  uintptr_t FileEnd = FileBegin + Obj->getData().size();
  ASSERT_GE(TablePtr, FileBegin);
  ASSERT_LE(TablePtr, FileEnd);
  size_t Available = FileEnd - TablePtr;
  constexpr size_t EntrySize = 2 * sizeof(uint32_t);
  size_t Count = std::min<size_t>(Dir->Size, Available) / EntrySize;
  const auto *Table = reinterpret_cast<const uint8_t *>(TablePtr);
  size_t FullCount = 0;
  size_t SymbolEligibleCount = 0;

  for (size_t I = 0; I < Count; ++I) {
    SCOPED_TRACE(I);
    uint32_t BeginWord = readLE<uint32_t>(Table + I * EntrySize);
    uint32_t UnwindWord =
        readLE<uint32_t>(Table + I * EntrySize + sizeof(uint32_t));
    uint32_t Flag = UnwindWord & 3u;
    if ((BeginWord == 0 && UnwindWord == 0) || Flag == 3u)
      continue;

    uint32_t Length = 0;
    bool IsFragment = Flag == 2u;
    if (Flag == 1u || Flag == 2u) {
      uint32_t Unit = Img.Arch == Arch::ARM ? 2u : 4u;
      Length = ((UnwindWord & 0x1ffcu) >> 2) * Unit;
    } else {
      uintptr_t XDataPtr = 0;
      llvm::Error XErr = Obj->getRvaPtr(UnwindWord & ~3u, XDataPtr);
      if (XErr) {
        llvm::consumeError(std::move(XErr));
        continue;
      }
      if (XDataPtr < FileBegin || XDataPtr > FileEnd ||
          static_cast<size_t>(FileEnd - XDataPtr) < sizeof(uint32_t))
        continue;
      uint32_t Header =
          readLE<uint32_t>(reinterpret_cast<const uint8_t *>(XDataPtr));
      if (((Header >> 18) & 3u) != 0)
        continue;
      uint32_t Unit = Img.Arch == Arch::ARM ? 2u : 4u;
      Length = (Header & 0x3ffffu) * Unit;
      if (Img.Arch == Arch::ARM)
        IsFragment = (Header & (1u << 22)) != 0;
    }
    if (Length == 0 || IsFragment)
      continue;

    uint64_t Begin = normalizeCodeAddress(BeginWord, Img.Arch, Img.Mode);
    if (Begin > InvalidVA - Obj->getImageBase())
      continue;
    va_t Addr = Obj->getImageBase() + Begin;
    if (Length > InvalidVA - Addr)
      continue;
    const Segment *Seg = Img.getSegmentFor(Addr);
    if (!Seg || !Seg->isExecutable())
      continue;
    uint64_t Usable = std::min<uint64_t>(Seg->Size, Seg->Data.size());
    if (Usable > InvalidVA - Seg->VA || Addr + Length > Seg->VA + Usable)
      continue;

    ++FullCount;
    size_t Off = static_cast<size_t>(Addr - Seg->VA);
    if (!checkPrologueAtOffset(*Seg, Off, Img.Arch))
      continue;
    ++SymbolEligibleCount;
    auto Sym = std::find_if(
        Img.Symbols.begin(), Img.Symbols.end(), [&](const Symbol &S) {
          return S.IsFunc && S.Addr == Addr && S.Size == Length;
        });
    EXPECT_NE(Sym, Img.Symbols.end())
        << "missing exact function symbol at 0x" << std::hex << Addr
        << " with size 0x" << Length;
  }
  EXPECT_GE(FullCount, 2u);
  EXPECT_GE(SymbolEligibleCount, 1u);
}

} // namespace neverd::coff_arm_test

#endif // NEVERD_UNITTESTS_LIFT_FORMAT_COFFARMFORMATTESTSDETAIL_H
