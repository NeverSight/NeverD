//===- VTableEvidenceTests.cpp - COFF table source declarations
//------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Source ownership, relocation fields and weak fallback counterclaims.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/sigs/PatternGenerator.h"

#include "llvm/BinaryFormat/COFF.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBufferRef.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

using namespace llvm;
using namespace neverd::sigs;

namespace {

struct TableObject {
  std::vector<uint8_t> Bytes;
  std::string Strings;
  uint32_t WeakSearch = COFF::IMAGE_WEAK_EXTERN_SEARCH_NOLIBRARY;
  void put(uint64_t Value, unsigned Width) {
    for (unsigned I = 0; I < Width; ++I)
      Bytes.push_back(Value >> (I * 8));
  }
  void name(StringRef Name) {
    put(0, 4);
    put(4 + Strings.size(), 4);
    Strings += Name.str();
    Strings.push_back('\0');
  }
  void symbol(StringRef Name, unsigned Value, unsigned Section, unsigned Type,
              unsigned Storage, unsigned Aux = 0) {
    name(Name);
    put(Value, 4);
    put(Section, 2);
    put(Type, 2);
    put(Storage, 1);
    put(Aux, 1);
  }
  void overwrite(size_t Offset, uint64_t Value, unsigned Width) {
    for (unsigned I = 0; I < Width; ++I)
      Bytes[Offset + I] = Value >> (I * 8);
  }
  std::vector<uint8_t> build() {
    // File header, one section, three pointers and three ADDR64 relocations.
    put(COFF::IMAGE_FILE_MACHINE_AMD64, 2);
    put(1, 2);
    put(0, 4);
    put(114, 4);
    put(6, 4);
    put(0, 4);
    for (char C : std::string(".rdata\0\0", 8))
      put(C, 1);
    put(0, 8);
    put(24, 4);
    put(60, 4);
    put(84, 4);
    put(0, 4);
    put(3, 2);
    put(0, 2);
    put(COFF::IMAGE_SCN_CNT_INITIALIZED_DATA | COFF::IMAGE_SCN_MEM_READ |
            COFF::IMAGE_SCN_LNK_COMDAT,
        4);
    for (unsigned I = 0; I < 3; ++I)
      put(0, 8);
    for (auto [Offset, Index] : {std::pair(0, 1), {8, 2}, {16, 5}}) {
      put(Offset, 4);
      put(Index, 4);
      put(COFF::IMAGE_REL_AMD64_ADDR64, 2);
    }
    symbol("??_7Widget@@6B@", 8, 1, 0, COFF::IMAGE_SYM_CLASS_EXTERNAL);
    symbol("??_R4Widget@@6B@", 0, 0, 0, COFF::IMAGE_SYM_CLASS_EXTERNAL);
    const auto Function = COFF::IMAGE_SYM_DTYPE_FUNCTION
                          << COFF::SCT_COMPLEX_TYPE_SHIFT;
    symbol("destroy", 0, 0, Function, COFF::IMAGE_SYM_CLASS_WEAK_EXTERNAL, 1);
    put(4, 4); // Explicit fallback, skipping this auxiliary record.
    put(WeakSearch, 4);
    for (unsigned I = 0; I < 10; ++I)
      put(0, 1);
    symbol("fallback", 0, 0, Function, COFF::IMAGE_SYM_CLASS_EXTERNAL);
    symbol("action", 0, 0, Function, COFF::IMAGE_SYM_CLASS_EXTERNAL);
    put(4 + Strings.size(), 4);
    Bytes.insert(Bytes.end(), Strings.begin(), Strings.end());
    return Bytes;
  }
};

json::Value generate(const std::vector<uint8_t> &Bytes, bool Complete) {
  auto Obj = object::ObjectFile::createObjectFile(MemoryBufferRef(
      StringRef(reinterpret_cast<const char *>(Bytes.data()), Bytes.size()),
      "table.obj"));
  EXPECT_TRUE(bool(Obj));
  if (!Obj) {
    consumeError(Obj.takeError());
    return nullptr;
  }
  std::string Text;
  raw_string_ostream OS(Text);
  auto Stats = generateVTableEvidence(**Obj, OS);
  EXPECT_TRUE(bool(Stats));
  if (!Stats) {
    ADD_FAILURE() << toString(Stats.takeError());
    return nullptr;
  }
  EXPECT_EQ(Stats->Complete, unsigned(Complete));
  EXPECT_EQ(Stats->Incomplete, unsigned(!Complete));
  auto JSON = json::parse(Text);
  EXPECT_TRUE(bool(JSON));
  if (!JSON) {
    consumeError(JSON.takeError());
    return nullptr;
  }
  return std::move(*JSON);
}

} // namespace

TEST(VTableEvidence, WeakSearchRulesRetainBothSourceAlternatives) {
  for (unsigned Policy : {1u, 2u, 3u}) {
    TableObject Object;
    Object.WeakSearch = Policy;
    const auto Record = generate(Object.build(), true);
    const auto *Row = Record.getAsObject();
    ASSERT_NE(Row, nullptr);
    EXPECT_EQ(Row->getString("name"), "??_7Widget@@6B@");
    EXPECT_EQ(Row->getInteger("bytes"), 16);
    const auto *Slots = Row->getArray("slots");
    ASSERT_NE(Slots, nullptr);
    ASSERT_EQ(Slots->size(), 2u);
    const auto *First = Slots->front().getAsObject();
    EXPECT_EQ(First->getInteger("weak_search"), Policy);
    EXPECT_EQ(*First->getArray("names"), (json::Array{"destroy", "fallback"}));
  }
}

TEST(VTableEvidence, UnsupportedDeclarationsRemainVisibleAsCounterclaims) {
  for (unsigned Case = 0; Case < 14; ++Case) {
    TableObject Object;
    Object.build();
    switch (Case) {
    case 0:
      Object.overwrite(56, COFF::IMAGE_SCN_MEM_READ, 4);
      break;
    case 1:
      Object.Bytes[59] |= 0x80;
      break; // Writable.
    case 2:
      Object.overwrite(114 + 8, 0, 4);
      break; // Table not after COL.
    case 3:
      Object.Bytes[60 + 8] = 1;
      break; // Nonzero pointer addend.
    case 4:
      Object.overwrite(94, 0, 4);
      break; // Overlapping relocation.
    case 5:
      Object.overwrite(94 + 8, COFF::IMAGE_REL_AMD64_REL32, 2);
      break;
    case 6:
      Object.overwrite(94 + 4, 3, 4);
      break; // Auxiliary target.
    case 7:
      Object.overwrite(114 + 3 * 18 + 4, 4, 4);
      break; // Anti-dependency.
    case 8:
      Object.overwrite(114 + 3 * 18, 3, 4);
      break; // Auxiliary fallback.
    case 9:
      Object.overwrite(114 + 4 * 18 + 14, 0, 2);
      break; // Data fallback.
    case 10:
      Object.Bytes[114 + 4 * 18 + 16] = COFF::IMAGE_SYM_CLASS_STATIC;
      break;
    case 11:
      Object.overwrite(114 + 18 + 8, 1, 4);
      break; // Common COL.
    case 12:
      Object.overwrite(104 + 4, 1, 4);
      break; // Data target.
    case 13:
      Object.overwrite(114 + 2 * 18 + 8, 8, 4);
      break; // Common weak.
    }
    const auto Record = generate(Object.Bytes, false);
    const auto *Row = Record.getAsObject();
    ASSERT_NE(Row, nullptr) << Case;
    EXPECT_EQ(Row->getBoolean("complete"), false) << Case;
    EXPECT_TRUE(Row->getString("reason").has_value()) << Case;
  }
}
