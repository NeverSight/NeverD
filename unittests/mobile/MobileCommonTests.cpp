#include "MobileCommon.h"
#include "gtest/gtest.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/Support/FileSystem.h"

#include <limits>
#include <zlib.h>

using namespace neverd::mobile;
namespace {
class MobileCommonTest : public testing::Test {
protected:
  fs::path root;
  void SetUp() override {
    llvm::SmallString<256> path;
    ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(
        pathText(fs::temp_directory_path() / "neverd-mobile-unit"), path));
    root = pathFromUTF8(path.str().str());
  }
  void TearDown() override {
    std::error_code ignored;
    fs::remove_all(root, ignored);
  }
};

void put(std::string &bytes, uint64_t value, unsigned width) {
  for (unsigned i = 0; i < width; ++i)
    bytes += static_cast<char>(value >> (i * 8));
}
struct ZipItem {
  std::string name, bytes;
  bool deflate = false;
  uint32_t attributes = 0100600u << 16;
};
std::string zip(const std::vector<ZipItem> &items, bool wide = false) {
  std::string result, directory;
  for (const auto &item : items) {
    std::string compressed = item.bytes;
    if (item.deflate) {
      z_stream stream{};
      if (deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -15, 8,
                       Z_DEFAULT_STRATEGY) != Z_OK)
        throw Error("fixture compression failed");
      compressed.resize(compressBound(item.bytes.size()) + 64);
      stream.next_in =
          reinterpret_cast<Bytef *>(const_cast<char *>(item.bytes.data()));
      stream.avail_in = item.bytes.size();
      stream.next_out = reinterpret_cast<Bytef *>(compressed.data());
      stream.avail_out = compressed.size();
      auto status = deflate(&stream, Z_FINISH);
      auto size = stream.total_out;
      deflateEnd(&stream);
      if (status != Z_STREAM_END)
        throw Error("fixture compression failed");
      compressed.resize(size);
    }
    auto crc = crc32(0, reinterpret_cast<const Bytef *>(item.bytes.data()),
                     item.bytes.size());
    uint64_t local = result.size();
    put(result, 0x04034b50, 4);
    put(result, 20, 2);
    put(result, 0x800, 2);
    put(result, item.deflate ? 8 : 0, 2);
    put(result, 0, 4);
    put(result, crc, 4);
    put(result, compressed.size(), 4);
    put(result, item.bytes.size(), 4);
    put(result, item.name.size(), 2);
    put(result, 0, 2);
    result += item.name;
    result += compressed;
    put(directory, 0x02014b50, 4);
    put(directory, 0x314, 2);
    put(directory, 20, 2);
    put(directory, 0x800, 2);
    put(directory, item.deflate ? 8 : 0, 2);
    put(directory, 0, 4);
    put(directory, crc, 4);
    put(directory, compressed.size(), 4);
    put(directory, item.bytes.size(), 4);
    put(directory, item.name.size(), 2);
    put(directory, 0, 2);
    put(directory, 0, 2);
    put(directory, 0, 2);
    put(directory, 0, 2);
    put(directory, item.attributes, 4);
    put(directory, local, 4);
    directory += item.name;
  }
  auto offset = result.size();
  result += directory;
  if (wide) {
    auto zip64 = result.size();
    put(result, 0x06064b50, 4);
    put(result, 44, 8);
    put(result, 45, 2);
    put(result, 45, 2);
    put(result, 0, 4);
    put(result, 0, 4);
    put(result, items.size(), 8);
    put(result, items.size(), 8);
    put(result, directory.size(), 8);
    put(result, offset, 8);
    put(result, 0x07064b50, 4);
    put(result, 0, 4);
    put(result, zip64, 8);
    put(result, 1, 4);
  }
  put(result, 0x06054b50, 4);
  put(result, 0, 2);
  put(result, 0, 2);
  put(result, wide ? 0xffff : items.size(), 2);
  put(result, wide ? 0xffff : items.size(), 2);
  put(result, wide ? 0xffffffff : directory.size(), 4);
  put(result, wide ? 0xffffffff : offset, 4);
  put(result, 0, 2);
  return result;
}

TEST_F(MobileCommonTest, PathRulesCoverPortableNamesAndUnicode) {
  for (auto name : {"../escape", "/absolute", "a//b", "a/./b", "a\\b", "C:foo",
                    "NUL.txt", "COM1", "a. ", "a.", "a/../b"})
    EXPECT_THROW(relativeMember(name), Error) << name;
  EXPECT_THROW(relativeMember(std::string("a\0b", 3)), Error);
  EXPECT_EQ(pathText(relativeMember("代码/方法.java")), "代码/方法.java");
  EXPECT_EQ(portableCaseKey("Straße"), portableCaseKey("STRASSE"));
  EXPECT_EQ(portableCaseKey("ÉCOLE"), portableCaseKey("école"));
  EXPECT_EQ(portableCaseKey("Σ"), portableCaseKey("ς"));
  EXPECT_THROW(pathFromUTF8(std::string("\xff", 1)), Error);
}
TEST_F(MobileCommonTest, OutputCreationIsExclusiveAndReadIsBounded) {
  auto path = root / "file";
  writeFile(path, "keep");
  EXPECT_THROW(writeFile(path, "replace"), Error);
  EXPECT_EQ(readFile(path, 4), "keep");
  EXPECT_THROW(readFile(path, 3), Error);
  EXPECT_THROW(validateTree(root, {1, 1, 3}), Error);
  EXPECT_THROW(validateTree(root, {1, 1, 4}, 0, 1), Error);
}
TEST_F(MobileCommonTest, WorkOutputAndTimeBudgetsReject) {
  EXPECT_THROW((Budget({0, 1, 1})), Error);
  EXPECT_THROW(
      (Budget(
          {uint64_t(std::numeric_limits<int64_t>::max() / 1000000000), 1, 1})),
      Error);
  Budget budget({1, 10, 5});
  budget.output(5);
  EXPECT_THROW(budget.output(1), Error);
  EXPECT_THROW(budget.tick(100001), Error);
  budget.deadline = std::chrono::steady_clock::now() - std::chrono::seconds(1);
  EXPECT_THROW(budget.check(), Error);
}
TEST_F(MobileCommonTest, ExtractsStoredDeflatedEmptyAndUnicodeZip64) {
  auto archive = root / "input.zip", output = root / "output";
  writeFile(archive, zip({{"empty", ""},
                          {"目录/plain", "payload"},
                          {"deflated", std::string(100000, 'x'), true},
                          {"empty-deflated", "", true}},
                         true));
  auto files = extractZip(archive, output, {});
  ASSERT_EQ(files.size(), 4u);
  EXPECT_EQ(readFile(output / pathFromUTF8("目录/plain"), 100), "payload");
  EXPECT_EQ(readFile(output / "deflated", 100000), std::string(100000, 'x'));
  EXPECT_TRUE(readFile(output / "empty-deflated", 1).empty());
}
TEST_F(MobileCommonTest, ArchiveDirectoryEntriesValidateTheirContents) {
  unsigned index = 0;
  for (bool deflated : {false, true}) {
    auto bytes = zip({{"directory/", "", deflated, 0040755u << 16}});
    auto directory = bytes.find(std::string("PK\1\2", 4));
    ASSERT_NE(directory, std::string::npos);
    // Both headers agree, but CRC32 of the directory's empty contents is zero.
    bytes[14] = 1;
    bytes[directory + 16] = 1;
    auto archive = root / ("bad-directory-" + std::to_string(index++) + ".zip");
    auto output = root / ("bad-output-" + std::to_string(index));
    writeFile(archive, bytes);
    EXPECT_THROW(extractZip(archive, output, {}), Error);
    EXPECT_FALSE(fs::exists(output / "directory"));
  }
  auto broken = zip({{"directory/", "", true, 0040755u << 16}});
  broken[30 + std::string("directory/").size()] = char(0xff);
  writeFile(root / "bad-directory-deflate.zip", broken);
  EXPECT_THROW(
      extractZip(root / "bad-directory-deflate.zip", root / "bad-deflate", {}),
      Error);
  EXPECT_FALSE(fs::exists(root / "bad-deflate/directory"));

  writeFile(root / "directories.zip",
            zip({{"stored/", "", false, 0040755u << 16},
                 {"deflated/", "", true, 0040755u << 16}}));
  EXPECT_TRUE(
      extractZip(root / "directories.zip", root / "directories", {}).empty());
  EXPECT_TRUE(fs::is_directory(root / "directories/stored"));
  EXPECT_TRUE(fs::is_directory(root / "directories/deflated"));
}

TEST_F(MobileCommonTest, ArchivePreflightRejectsAllPathAndInventoryConflicts) {
  unsigned index = 0;
  for (const auto &items : std::vector<std::vector<ZipItem>>{
           {{"valid", "ok"}, {"../escape", "bad"}},
           {{"same", "a"}, {"same", "b"}},
           {{"Straße", "a"}, {"STRASSE", "b"}},
           {{"a", "file"}, {"a/b", "child"}},
           {{"link", "target", false, 0120777u << 16}},
           {{"directory/", "data", false, 0040755u << 16}}}) {
    auto archive = root / ("bad" + std::to_string(index++) + ".zip");
    auto output = root / "output";
    writeFile(archive, zip(items));
    EXPECT_THROW(extractZip(archive, output, {}), Error);
    EXPECT_FALSE(fs::exists(output));
  }
}
TEST_F(MobileCommonTest, ArchiveCorruptionAndLimitsReject) {
  auto bytes = zip({{"file", "payload"}});
  bytes[34] ^= 1; // First data byte; directory and local CRC stay unchanged.
  auto archive = root / "bad.zip";
  writeFile(archive, bytes);
  EXPECT_THROW(extractZip(archive, root / "crc", {}), Error);
  auto valid = root / "valid.zip";
  writeFile(valid, zip({{"a/b/file", std::string(1000, 'x'), true}}));
  EXPECT_THROW(extractZip(valid, root / "bytes", {1, 20, 999}), Error);
  EXPECT_THROW(extractZip(valid, root / "files", {1, 2, 2000}), Error);
  EXPECT_FALSE(fs::exists(root / "bytes"));
  EXPECT_FALSE(fs::exists(root / "files"));
  writeFile(root / "truncated.zip", bytes.substr(0, bytes.size() - 1));
  EXPECT_THROW(extractZip(root / "truncated.zip", root / "truncated", {}),
               Error);
}
TEST_F(MobileCommonTest, RecoveryPreservesExistingOutputAndCleansFailure) {
  auto input = root / "bad.smali";
  writeFile(
      input,
      ".class public LBad;\n.super Ljava/lang/Object;\n"
      ".method public static bad()I\n.registers 1\ninvalid\n.end method\n");
  Options options;
  options.input = input;
  options.output = root / "result";
  EXPECT_THROW(recover(options), Error);
  EXPECT_FALSE(fs::exists(options.output));
  for (auto &entry : fs::directory_iterator(root))
    EXPECT_FALSE(
        pathText(entry.path().filename()).starts_with(".neverd-mobile-"));
  fs::create_directory(options.output);
  writeFile(options.output / "keep", "original");
  EXPECT_THROW(recover(options), Error);
  EXPECT_EQ(readFile(options.output / "keep", 100), "original");
}
TEST_F(MobileCommonTest, RecoveryRejectsInapplicableOptionsAndNestedOutput) {
  auto input = root / "smali";
  fs::create_directory(input);
  Options options;
  options.input = input;
  options.output = input / "output";
  EXPECT_THROW(recover(options), Error);
  options.output = root / "output";
  options.metadata_only = true;
  EXPECT_THROW(recover(options), Error);
  options.metadata_only = false;
  options.platform = "unknown";
  EXPECT_THROW(recover(options), Error);
  EXPECT_FALSE(fs::exists(options.output));
}
TEST_F(MobileCommonTest, InputCopyRejectsLinksWithoutPublishing) {
#ifdef _WIN32
  GTEST_SKIP() << "Creating symlinks requires a Windows developer privilege";
#else
  auto input = root / "input";
  fs::create_directory(input);
  writeFile(input / "valid", "content");
  fs::create_symlink(input / "valid", input / "link");
  EXPECT_THROW(copyTree(input, root / "copy", {}), Error);
  EXPECT_FALSE(fs::exists(root / "copy"));
#endif
}
} // namespace
