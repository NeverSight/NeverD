//===- MachOI386RelocationTestsDetail.h - Mach-O i386 relocation test harness -===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Fixtures and raw Mach-O readers and mutators shared by the
// MachOI386Relocation* translation units.  The fixture classes are in a
// named namespace so every TU in the binary sees one type per suite,
// and each free function is `inline`.
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_UNITTESTS_LIFT_FORMAT_MACHOI386RELOCATIONTESTSDETAIL_H
#define NEVERD_UNITTESTS_LIFT_FORMAT_MACHOI386RELOCATIONTESTSDETAIL_H

#include "NeverDLiftFixture.h"
#include "gtest/gtest.h"

#include "neverd/object/SectionNames.h"
#include "neverd/support/BinaryEncoding.h"
#include "neverd/support/BinaryLoading.h"
#include "neverd/ir/low/FuncDetector.h"
#include "neverd/loader/MachO/MachOLoaderUtils.h"
#include "neverd/loader/MachO/MachORelocations.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/BinaryFormat/MachO.h"
#include "llvm/Object/MachO.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/MemoryBufferRef.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <ostream>
#include <string>
#include <vector>


namespace neverd::macho_i386_test {

using namespace neverd::macho_loader::detail;

inline fs::path fixture(llvm::StringRef Name) {
  return fs::path(TEST_OBJ_DIR) / Name.str();
}

class MachOI386Relocation : public NeverDLiftTest {
protected:
  fs::path writeMutation(llvm::StringRef Name,
                         const std::vector<uint8_t> &Bytes) const {
    fs::path Path = tmpFile(Name.str());
    std::fstream Out(Path, std::ios::binary | std::ios::out | std::ios::trunc);
    Out.write(reinterpret_cast<const char *>(Bytes.data()), Bytes.size());
    EXPECT_TRUE(Out.good());
    return Path;
  }
};

struct MachOI386PipelineCase {
  const char *FixtureName;
  const char *TestName;
};

inline void PrintTo(const MachOI386PipelineCase &TestCase, std::ostream *Out) {
  *Out << TestCase.FixtureName;
}

class MachOI386Pipeline
    : public MachOI386Relocation,
      public ::testing::WithParamInterface<MachOI386PipelineCase> {};

inline std::vector<uint8_t> readBinaryFile(const fs::path &Path) {
  std::ifstream In(Path, std::ios::binary);
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(In), {});
}

inline std::unique_ptr<llvm::object::MachOObjectFile>
createMachOObject(const std::vector<uint8_t> &Bytes) {
  llvm::StringRef Data(reinterpret_cast<const char *>(Bytes.data()),
                       Bytes.size());
  auto ObjOrErr = llvm::object::ObjectFile::createMachOObjectFile(
      llvm::MemoryBufferRef(Data, "MachOI386Relocation fixture"));
  if (!ObjOrErr) {
    ADD_FAILURE() << llvm::toString(ObjOrErr.takeError());
    return nullptr;
  }
  return std::move(*ObjOrErr);
}

inline std::optional<llvm::object::SectionRef>
findSection(const llvm::object::MachOObjectFile &Obj, llvm::StringRef Name) {
  for (const llvm::object::SectionRef &Sec : Obj.sections()) {
    auto NameOrErr = Sec.getName();
    if (!NameOrErr) {
      llvm::consumeError(NameOrErr.takeError());
      continue;
    }
    if (*NameOrErr == Name)
      return Sec;
  }
  return std::nullopt;
}

inline std::vector<llvm::object::RelocationRef>
relocations(const llvm::object::SectionRef &Sec) {
  std::vector<llvm::object::RelocationRef> Result;
  for (const llvm::object::RelocationRef &Reloc : Sec.relocations())
    Result.push_back(Reloc);
  return Result;
}

struct OriginalSection {
  size_t Index = 0;
  uint64_t Base = 0;
};

inline std::optional<OriginalSection>
findOriginalSection(const llvm::object::MachOObjectFile &Obj, uint64_t Addr) {
  size_t Index = 0;
  for (const llvm::object::SectionRef &Sec : Obj.sections()) {
    uint64_t Base = Sec.getAddress();
    uint64_t Size = Sec.getSize();
    if (Addr >= Base && Addr - Base < Size)
      return OriginalSection{Index, Base};
    ++Index;
  }
  return std::nullopt;
}

inline std::optional<int64_t> readRelocationField(llvm::ArrayRef<uint8_t> Data,
                                           uint64_t Offset, uint8_t Width,
                                           bool SignedValue) {
  if (!rangeInBounds(Offset, Width, Data.size()))
    return std::nullopt;
  const uint8_t *Field = Data.data() + Offset;
  if (SignedValue) {
    switch (Width) {
    case 1:
      return readLE<int8_t>(Field);
    case 2:
      return readLE<int16_t>(Field);
    case 4:
      return readLE<int32_t>(Field);
    default:
      return std::nullopt;
    }
  }
  switch (Width) {
  case 1:
    return readLE<uint8_t>(Field);
  case 2:
    return readLE<uint16_t>(Field);
  case 4:
    return readLE<uint32_t>(Field);
  default:
    return std::nullopt;
  }
}

inline std::optional<int64_t> readLoadedField(const BinaryImage &Img, va_t Address,
                                       uint8_t Width, bool SignedValue) {
  const uint8_t *Field = Img.readVA(Address, Width);
  if (!Field)
    return std::nullopt;
  return readRelocationField(llvm::ArrayRef(Field, Width), 0, Width,
                             SignedValue);
}

inline const Symbol *findSymbol(const BinaryImage &Img, llvm::StringRef Name) {
  auto It = std::find_if(Img.Symbols.begin(), Img.Symbols.end(),
                         [&](const Symbol &Sym) { return Sym.Name == Name; });
  return It == Img.Symbols.end() ? nullptr : &*It;
}

struct RawSectionLayout {
  uint32_t Address = 0;
  uint32_t Size = 0;
  uint32_t FileOffset = 0;
  uint32_t RelocationOffset = 0;
  uint32_t RelocationCount = 0;
};

inline std::optional<RawSectionLayout>
rawSectionLayout(const std::vector<uint8_t> &Bytes, llvm::StringRef Name) {
  auto Obj = createMachOObject(Bytes);
  if (!Obj)
    return std::nullopt;
  auto Sec = findSection(*Obj, Name);
  if (!Sec)
    return std::nullopt;
  llvm::MachO::section Raw = Obj->getSection(Sec->getRawDataRefImpl());
  if (!rangeInBounds(Raw.offset, Raw.size, Bytes.size()) ||
      !rangeInBounds(Raw.reloff, uint64_t(Raw.nreloc) * 8, Bytes.size()))
    return std::nullopt;
  return RawSectionLayout{Raw.addr, Raw.size, Raw.offset, Raw.reloff,
                          Raw.nreloc};
}

inline std::optional<size_t> rawSectionHeaderOffset(const std::vector<uint8_t> &Bytes,
                                             llvm::StringRef Name) {
  auto Obj = createMachOObject(Bytes);
  if (!Obj || Obj->is64Bit())
    return std::nullopt;
  const char *Base = Obj->getData().data();
  for (const auto &LC : Obj->load_commands()) {
    if (LC.C.cmd != llvm::MachO::LC_SEGMENT)
      continue;
    auto Seg = Obj->getSegmentLoadCommand(LC);
    for (uint32_t I = 0; I < Seg.nsects; ++I) {
      auto Sec = Obj->getSection(LC, I);
      if (readMachOName(Sec.sectname) != Name)
        continue;
      const char *Header = LC.Ptr + sizeof(llvm::MachO::segment_command) +
                           size_t(I) * sizeof(llvm::MachO::section);
      if (Header < Base)
        return std::nullopt;
      size_t Offset = static_cast<size_t>(Header - Base);
      if (!rangeInBounds(Offset, sizeof(llvm::MachO::section), Bytes.size()))
        return std::nullopt;
      return Offset;
    }
  }
  return std::nullopt;
}

inline std::optional<size_t> findRawRelocation(const std::vector<uint8_t> &Bytes,
                                        const RawSectionLayout &Section,
                                        uint32_t Address) {
  for (uint32_t I = 0; I < Section.RelocationCount; ++I) {
    size_t Offset = Section.RelocationOffset + size_t(I) * 8;
    uint32_t Word0 = readLE<uint32_t>(Bytes.data() + Offset);
    uint32_t RelocAddress =
        Word0 & llvm::MachO::R_SCATTERED ? Word0 & 0x00ffffffu : Word0;
    if (RelocAddress == Address)
      return Offset;
  }
  return std::nullopt;
}

inline std::optional<uint32_t> findSymbolIndex(const std::vector<uint8_t> &Bytes,
                                        llvm::StringRef Name) {
  auto Obj = createMachOObject(Bytes);
  if (!Obj)
    return std::nullopt;
  llvm::MachO::symtab_command Symtab = Obj->getSymtabLoadCommand();
  for (uint32_t I = 0; I < Symtab.nsyms; ++I) {
    auto Sym = Obj->getSymbolByIndex(I);
    auto NameOrErr = Sym->getName();
    if (!NameOrErr) {
      llvm::consumeError(NameOrErr.takeError());
      continue;
    }
    if (*NameOrErr == Name)
      return I;
  }
  return std::nullopt;
}

inline uint32_t plainRelocationWord(uint32_t SymbolNumber, bool IsPCRel,
                             uint32_t Length, bool IsExternal, uint32_t Type) {
  return (SymbolNumber & 0x00ffffffu) | (uint32_t(IsPCRel) << 24) |
         ((Length & 3u) << 25) | (uint32_t(IsExternal) << 27) |
         ((Type & 0xfu) << 28);
}

inline uint32_t scatteredRelocationWord(uint32_t Address, bool IsPCRel,
                                 uint32_t Length, uint32_t Type) {
  return llvm::MachO::R_SCATTERED | (uint32_t(IsPCRel) << 30) |
         ((Length & 3u) << 28) | ((Type & 0xfu) << 24) |
         (Address & 0x00ffffffu);
}

inline void writeRawRelocation(std::vector<uint8_t> &Bytes, size_t Offset,
                        uint32_t Word0, uint32_t Word1) {
  writeLE<uint32_t>(Bytes.data() + Offset, Word0);
  writeLE<uint32_t>(Bytes.data() + Offset + 4, Word1);
}

inline void writeSectionField(std::vector<uint8_t> &Bytes,
                       const RawSectionLayout &Section, uint32_t Address,
                       uint32_t Value) {
  writeLE<uint32_t>(Bytes.data() + Section.FileOffset + Address, Value);
}

} // namespace neverd::macho_i386_test

#endif // NEVERD_UNITTESTS_LIFT_FORMAT_MACHOI386RELOCATIONTESTSDETAIL_H
