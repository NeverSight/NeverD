//===- SessionEmbeddedObjectTests.cpp - Contained ELF references ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Independent file layouts exercise contained object coordinates, prefix
/// text, rejected metadata and snapshot invalidation through the public SDK.
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/sdk/NeverDCAPI.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/JSON.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {
using Bytes = std::vector<uint8_t>;

void put(Bytes &Data, size_t At, uint64_t Value, unsigned Width,
         bool Big = false) {
  for (unsigned I = 0; I < Width; ++I)
    Data.at(At + I) = Value >> ((Big ? Width - 1 - I : I) * 8);
}

// Hand-authored ELF records: null, text, data, symbols, symbol names,
// relocations, section names. Offset 0xA0 holds a control-prefixed C string.
Bytes object(bool Wide = true, bool Big = false, unsigned Machine = 183,
             bool Rela = true) {
  const unsigned SH = Wide ? 64 : 40, SYM = Wide ? 24 : 16;
  const unsigned REL = (Wide ? 16 : 8) + (Rela ? (Wide ? 8 : 4) : 0);
  Bytes Data(0x200 + 7 * SH);
  Data[0] = 0x7f;
  Data[1] = 'E';
  Data[2] = 'L';
  Data[3] = 'F';
  Data[4] = Wide ? 2 : 1;
  Data[5] = Big ? 2 : 1;
  Data[6] = 1;
  put(Data, 16, 1, 2, Big);
  put(Data, 18, Machine, 2, Big);
  put(Data, 20, 1, 4, Big);
  put(Data, Wide ? 40 : 32, 0x200, Wide ? 8 : 4, Big);
  put(Data, Wide ? 52 : 40, Wide ? 64 : 52, 2, Big);
  put(Data, Wide ? 58 : 46, SH, 2, Big);
  put(Data, Wide ? 60 : 48, 7, 2, Big);
  put(Data, Wide ? 62 : 50, 6, 2, Big);
  const std::string Text = "\x01"
                           "fixture text";
  std::copy(Text.begin(), Text.end(), Data.begin() + 0xa0);
  const std::string Symbol = "fixture_data";
  std::copy(Symbol.begin(), Symbol.end(), Data.begin() + 0x121);
  const char Names[] =
      "\0.text\0.rodata\0.symtab\0.strtab\0.rela.text\0.shstrtab\0";
  std::copy(std::begin(Names), std::end(Names), Data.begin() + 0x1a0);
  const auto section = [&](unsigned Index, unsigned Name, unsigned Type,
                           unsigned Flags, unsigned Offset, unsigned Size,
                           unsigned Link, unsigned Info, unsigned EntSize) {
    const size_t At = 0x200 + Index * SH;
    put(Data, At, Name, 4, Big);
    put(Data, At + 4, Type, 4, Big);
    put(Data, At + 8, Flags, Wide ? 8 : 4, Big);
    put(Data, At + (Wide ? 24 : 16), Offset, Wide ? 8 : 4, Big);
    put(Data, At + (Wide ? 32 : 20), Size, Wide ? 8 : 4, Big);
    put(Data, At + (Wide ? 40 : 24), Link, 4, Big);
    put(Data, At + (Wide ? 44 : 28), Info, 4, Big);
    put(Data, At + (Wide ? 48 : 32), 1, Wide ? 8 : 4, Big);
    put(Data, At + (Wide ? 56 : 36), EntSize, Wide ? 8 : 4, Big);
  };
  section(1, 1, 1, 6, 0x80, 16, 0, 0, 0);
  section(2, 7, 1, 2, 0xa0, 32, 0, 0, 0);
  section(3, 15, 2, 0, 0xd0, 3 * SYM, 4, 2, SYM);
  section(4, 23, 3, 0, 0x120, Symbol.size() + 2, 0, 0, 0);
  section(5, 31, Rela ? 4 : 9, 0, 0x140, 3 * REL, 3, 1, REL);
  section(6, 42, 3, 0, 0x1a0, sizeof(Names), 0, 0, 0);
  for (unsigned I = 1; I <= 2; ++I) {
    const auto At = 0xd0 + I * SYM;
    put(Data, At, I == 2 ? 1 : 0, 4, Big);
    put(Data, At + (Wide ? 4 : 12), I == 1 ? 3 : 0x11, 1, Big);
    put(Data, At + (Wide ? 6 : 14), 2, 2, Big);
    put(Data, At + (Wide ? 16 : 8), I == 2 ? Text.size() + 1 : 0, Wide ? 8 : 4,
        Big);
  }
  for (unsigned I = 0; I < 3; ++I) {
    const auto At = 0x140 + I * REL;
    const unsigned Type = Machine == 183  ? (I == 0   ? 275
                                             : I == 1 ? 277
                                                      : 257)
                          : Machine == 62 ? 10
                          : Machine == 40 ? 2
                                          : 1;
    const uint64_t Info = (uint64_t(I == 2 ? 2 : 1) << (Wide ? 32 : 8)) | Type;
    put(Data, At, I * 4, Wide ? 8 : 4, Big);
    put(Data, At + (Wide ? 8 : 4), Info, Wide ? 8 : 4, Big);
    if (Rela)
      put(Data, At + (Wide ? 16 : 8), I == 2 ? 1 : 0, Wide ? 8 : 4, Big);
    else
      put(Data, 0x80 + I * 4, I == 2 ? 1 : 0, 4, Big);
  }
  return Data;
}

Bytes container(const Bytes &Payload) {
  constexpr uint64_t Base = 0x400000;
  constexpr size_t Start = 0x103; // Intentionally unaligned.
  const size_t NamesAt = Start + Payload.size();
  const char Names[] = "\0.text\0.payload\0.shstrtab\0";
  const size_t SectionsAt = (NamesAt + sizeof(Names) + 7) & ~size_t(7);
  Bytes Data(SectionsAt + 4 * 64);
  std::copy(Payload.begin(), Payload.end(), Data.begin() + Start);
  std::copy(std::begin(Names), std::end(Names), Data.begin() + NamesAt);
  Data[0] = 0x7f;
  Data[1] = 'E';
  Data[2] = 'L';
  Data[3] = 'F';
  Data[4] = 2;
  Data[5] = 1;
  Data[6] = 1;
  put(Data, 16, 2, 2);
  put(Data, 18, 183, 2);
  put(Data, 20, 1, 4);
  put(Data, 24, Base + 0x80, 8);
  put(Data, 32, 64, 8);
  put(Data, 40, SectionsAt, 8);
  put(Data, 52, 64, 2);
  put(Data, 54, 56, 2);
  put(Data, 56, 1, 2);
  put(Data, 58, 64, 2);
  put(Data, 60, 4, 2);
  put(Data, 62, 3, 2);
  put(Data, 64, 1, 4);
  put(Data, 68, 5, 4);
  put(Data, 80, Base, 8);
  put(Data, 88, Base, 8);
  put(Data, 96, Data.size(), 8);
  put(Data, 104, Data.size(), 8);
  put(Data, 112, 0x1000, 8);
  put(Data, 0x80, 0xd65f03c0, 4); // AArch64 RET.
  const auto section = [&](unsigned I, unsigned Name, unsigned Type,
                           unsigned Flags, size_t Offset, size_t Size) {
    const size_t At = SectionsAt + I * 64;
    put(Data, At, Name, 4);
    put(Data, At + 4, Type, 4);
    put(Data, At + 8, Flags, 8);
    put(Data, At + 16, Flags ? Base + Offset : 0, 8);
    put(Data, At + 24, Offset, 8);
    put(Data, At + 32, Size, 8);
    put(Data, At + 48, 1, 8);
  };
  section(1, 1, 1, 6, 0x80, 4);
  section(2, 7, 1, 2, Start, Payload.size());
  section(3, 16, 3, 0, NamesAt, sizeof(Names));
  return Data;
}

struct Input {
  std::filesystem::path Path;
  neverd_session_t Session = neverd_session_create();
  explicit Input(const Bytes &Data) {
    llvm::SmallString<128> Name;
    if (llvm::sys::fs::createTemporaryFile("neverd-contained-elf", "elf", Name))
      return;
    Path = Name.str().str();
    std::ofstream Stream(Path, std::ios::binary);
    Stream.write(reinterpret_cast<const char *>(Data.data()), Data.size());
    Stream.close();
    EXPECT_EQ(neverd_session_load(Session, Path.string().c_str()), 1);
  }
  ~Input() {
    neverd_session_destroy(Session);
    std::error_code Error;
    std::filesystem::remove(Path, Error);
  }
};

llvm::json::Value read(const char *Text) {
  EXPECT_NE(Text, nullptr);
  if (!Text)
    return nullptr;
  auto Value = llvm::json::parse(Text);
  neverd_free_string(Text);
  EXPECT_TRUE(bool(Value));
  if (!Value) {
    llvm::consumeError(Value.takeError());
    return nullptr;
  }
  return std::move(*Value);
}

TEST(SessionEmbeddedObjects, RelocationsRetainFileCoordinatesAndControlPrefix) {
  Input Image(container(object()));
  auto Catalogue = read(neverd_embedded_objects_json(Image.Session));
  auto *Objects = Catalogue.getAsArray();
  ASSERT_TRUE(Objects);
  ASSERT_EQ(Objects->size(), 1u);
  const auto *Object = Objects->front().getAsObject();
  ASSERT_TRUE(Object);
  EXPECT_EQ(Object->getString("file_offset"), "0x103");
  EXPECT_EQ(Object->getString("diagnostic"), "");
  const auto *Refs = Object->getArray("refs");
  ASSERT_TRUE(Refs);
  ASSERT_EQ(Refs->size(), 3u);
  const auto *First = Refs->front().getAsObject();
  EXPECT_EQ(First->getString("from"), "0x400183");
  EXPECT_EQ(First->getString("to"), "0x4001A3");
  EXPECT_EQ(First->getString("string_address"), "0x4001A4");
  EXPECT_EQ(First->getString("record_file_offset"), "0x243");
  auto References = read(neverd_string_refs_json(Image.Session, nullptr, 0, 1));
  const auto *Rows = References.getAsObject()->getArray("refs");
  ASSERT_TRUE(Rows);
  ASSERT_EQ(Rows->size(), 3u);
  EXPECT_EQ((*(*Rows)[0].getAsArray())[4].getAsString(), "embedded_offset");
  EXPECT_EQ((*(*Rows)[0].getAsArray())[1].getAsString(), "0x4001A3");
  EXPECT_EQ((*(*Rows)[0].getAsArray())[2].getAsString(), "0x4001A4");
  auto Later =
      read(neverd_string_refs_json(Image.Session, nullptr, 0x400081, 1));
  EXPECT_TRUE(Later.getAsObject()->getArray("refs")->empty());
}

TEST(SessionEmbeddedObjects, EndiannessClassesAndAbsoluteRelForms) {
  for (const auto Machine : {183u, 62u, 40u, 3u})
    for (bool Big : {false, true}) {
      const bool Wide = Machine == 183 || Machine == 62;
      const bool Rela = Wide;
      Input Image(container(object(Wide, Big, Machine, Rela)));
      auto Result = read(neverd_embedded_objects_json(Image.Session));
      auto *Rows = Result.getAsArray();
      ASSERT_TRUE(Rows);
      ASSERT_EQ(Rows->size(), 1u);
      const auto *Object = Rows->front().getAsObject();
      EXPECT_EQ(Object->getString("diagnostic"), "");
      EXPECT_EQ(Object->getBoolean("big_endian"), Big);
      ASSERT_EQ(Object->getArray("refs")->size(), 3u);
      EXPECT_EQ(Object->getArray("refs")->back().getAsObject()->getString("to"),
                "0x4001A4");
    }
}

TEST(SessionEmbeddedObjects, MalformedObjectsNeverPublishPartialReferences) {
  for (unsigned Fault = 0; Fault < 5; ++Fault) {
    auto Payload = object();
    switch (Fault) {
    case 0:
      put(Payload, 40, UINT64_MAX, 8);
      break;
    case 1:
      put(Payload, 0x200 + 2 * 64 + 32, UINT64_MAX, 8);
      break;
    case 2:
      put(Payload, 0x140 + 2 * 24 + 8, (uint64_t(99) << 32) | 257, 8);
      break;
    case 3:
      put(Payload, 0x200 + 5 * 64 + 56, 0, 8);
      break;
    case 4:
      put(Payload, 0x140 + 2 * 24, 15, 8);
      break;
    }
    Input Image(container(Payload));
    auto Result = read(neverd_embedded_objects_json(Image.Session));
    auto *Rows = Result.getAsArray();
    ASSERT_TRUE(Rows);
    ASSERT_EQ(Rows->size(), 1u);
    const auto *Object = Rows->front().getAsObject();
    EXPECT_FALSE(Object->getString("diagnostic")->empty()) << Fault;
    EXPECT_TRUE(Object->getArray("refs")->empty()) << Fault;
  }
}

TEST(SessionEmbeddedObjects, UnsupportedAndUnresolvedRecordsStayExplicit) {
  auto Payload = object();
  put(Payload, 0x140 + 8, (uint64_t(1) << 32) | 9999, 8);
  put(Payload, 0x140 + 24 + 16, UINT64_MAX, 8); // S-1 is outside the section.
  Input Image(container(Payload));
  auto Result = read(neverd_embedded_objects_json(Image.Session));
  const auto *Object = Result.getAsArray()->front().getAsObject();
  EXPECT_EQ(Object->getString("diagnostic"), "");
  EXPECT_EQ(Object->getInteger("unsupported_relocations"), 1);
  EXPECT_EQ(Object->getInteger("unresolved_relocations"), 1);
  EXPECT_EQ(Object->getArray("refs")->size(), 1u);
}

TEST(SessionEmbeddedObjects,
     NestedAndEncodedPayloadsDoNotBecomeOuterFunctions) {
  const auto Nested = container(object());
  Input Image(container(Nested));
  const auto Before = neverd_func_count(Image.Session);
  auto Result = read(neverd_embedded_objects_json(Image.Session));
  ASSERT_EQ(Result.getAsArray()->size(), 2u);
  EXPECT_EQ(neverd_func_count(Image.Session), Before);
  auto Encoded = object();
  for (auto &Byte : Encoded)
    Byte ^= 0xa5;
  Input Hidden(container(Encoded));
  auto Empty = read(neverd_embedded_objects_json(Hidden.Session));
  EXPECT_TRUE(Empty.getAsArray()->empty());
  auto Reload = container(Encoded);
  std::ofstream Stream(Image.Path, std::ios::binary | std::ios::trunc);
  Stream.write(reinterpret_cast<const char *>(Reload.data()), Reload.size());
  Stream.close();
  ASSERT_EQ(neverd_session_load(Image.Session, Image.Path.string().c_str()), 1);
  Empty = read(neverd_embedded_objects_json(Image.Session));
  EXPECT_TRUE(Empty.getAsArray()->empty());
}
TEST(SessionEmbeddedObjects, BytePatchAndRevertRetireTheFileSnapshot) {
  Input Image(container(object()));
  auto Before = read(neverd_embedded_objects_json(Image.Session));
  ASSERT_EQ(Before.getAsArray()->size(), 1u);
  neverd_byte_patch_options_v1 Patch{};
  Patch.struct_size = sizeof(Patch);
  Patch.address = 0x400103;
  Patch.kind = "bytes";
  Patch.bytes = "00";
  auto Applied = read(neverd_byte_patch_apply(Image.Session, &Patch));
  ASSERT_FALSE(Applied.getAsNull().has_value());
  auto Changed = read(neverd_embedded_objects_json(Image.Session));
  ASSERT_TRUE(Changed.getAsArray());
  EXPECT_TRUE(Changed.getAsArray()->empty());
  ASSERT_EQ(neverd_byte_patch_revert(Image.Session, Patch.address, 1), 0);
  auto Reverted = read(neverd_embedded_objects_json(Image.Session));
  ASSERT_EQ(Reverted.getAsArray()->size(), 1u);
  EXPECT_EQ(
      Reverted.getAsArray()->front().getAsObject()->getArray("refs")->size(),
      3u);
}

TEST(SessionEmbeddedObjects,
     UnmappedPayloadRetainsFileOffsetsWithoutVirtualAddresses) {
  auto Data = container(Bytes(32));
  const auto Payload = object();
  Data.insert(Data.end(), Payload.begin(), Payload.end());
  Input Image(Data);
  auto Result = read(neverd_embedded_objects_json(Image.Session));
  ASSERT_EQ(Result.getAsArray()->size(), 1u);
  const auto *Refs =
      Result.getAsArray()->front().getAsObject()->getArray("refs");
  ASSERT_EQ(Refs->size(), 3u);
  const auto *First = Refs->front().getAsObject();
  EXPECT_TRUE(First->get("from")->getAsNull().has_value());
  EXPECT_TRUE(First->get("to")->getAsNull().has_value());
  EXPECT_TRUE(First->getString("from_file_offset").has_value());
  EXPECT_TRUE(First->getString("to_file_offset").has_value());
}
TEST(SessionEmbeddedObjects, CompressedSectionsAndNulPrefixesAreNotGuessed) {
  auto Compressed = object();
  put(Compressed, 0x200 + 2 * 64 + 8, 0x802, 8);
  Input Packed(container(Compressed));
  auto Result = read(neverd_embedded_objects_json(Packed.Session));
  const auto *Object = Result.getAsArray()->front().getAsObject();
  EXPECT_EQ(Object->getInteger("unsupported_relocations"), 3);
  EXPECT_TRUE(Object->getArray("refs")->empty());

  auto Terminated = object();
  Terminated[0xa0] = 0;
  Input Separated(container(Terminated));
  Result = read(neverd_embedded_objects_json(Separated.Session));
  const auto *Refs =
      Result.getAsArray()->front().getAsObject()->getArray("refs");
  ASSERT_EQ(Refs->size(), 3u);
  EXPECT_FALSE(Refs->front().getAsObject()->getString("string_address"));
  EXPECT_EQ(Refs->back().getAsObject()->getString("string_address"),
            "0x4001A4");
}
} // namespace
