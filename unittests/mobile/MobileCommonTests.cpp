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
  std::string local_extra = {}, central_extra = {};
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
    put(result, item.local_extra.size(), 2);
    result += item.name;
    result += item.local_extra;
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
    put(directory, item.central_extra.size(), 2);
    put(directory, 0, 2);
    put(directory, 0, 2);
    put(directory, 0, 2);
    put(directory, item.attributes, 4);
    put(directory, local, 4);
    directory += item.name;
    directory += item.central_extra;
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
TEST_F(MobileCommonTest, LocalAlignmentPaddingPreservesPayloadValidation) {
  unsigned index = 0;
  for (bool deflated : {false, true})
    for (unsigned count : {1u, 2u, 3u, 4u, 5u, 4095u}) {
      auto stem = "aligned-" + std::to_string(index++);
      auto archive = root / (stem + ".apk");
      ZipItem code{"classes.dex", "bytecode", deflated},
          resource{"resources.arsc", "resource", deflated};
      code.local_extra.assign(count, '\0');
      // A normal extra record can precede the zero alignment tail.
      put(resource.local_extra, 0x1234, 2);
      put(resource.local_extra, 3, 2);
      resource.local_extra += "abc";
      resource.local_extra.append(count, '\0');
      auto bytes = zip({code, resource});
      writeFile(archive, bytes);
      auto output = root / stem;
      auto files = extractZip(archive, output, {}, [](const fs::path &path) {
        return path == "classes.dex";
      });
      ASSERT_EQ(files.size(), 1u);
      EXPECT_EQ(readFile(files[0], 1024), "bytecode");
      EXPECT_FALSE(fs::exists(output / "resources.arsc"));
      // Padding acceptance cannot bypass checksums of unwritten resources.
      auto central = bytes.rfind(std::string("PK\1\2", 4));
      ASSERT_NE(central, std::string::npos);
      bytes[central + 16] ^= 1;
      auto local = bytes.find(std::string("PK\3\4", 4), 1);
      ASSERT_NE(local, std::string::npos);
      bytes[local + 14] ^= 1;
      auto corrupt = root / (stem + "-bad-crc.apk");
      writeFile(corrupt, bytes);
      EXPECT_THROW(extractZip(corrupt, root / (stem + "-bad-crc"), {},
                              [](const fs::path &) { return false; }),
                   Error);
    }
  ZipItem large{"resources.arsc", std::string(1024, 'r'), true};
  large.local_extra.assign(2, '\0');
  auto compressed = zip({large});
  ASSERT_LT(compressed.size(), large.bytes.size());
  writeFile(root / "aligned-budget.zip", compressed);
  Limits limits;
  limits.max_bytes = compressed.size();
  EXPECT_THROW(extractZip(root / "aligned-budget.zip", root / "budget", limits,
                          [](const fs::path &) { return false; }),
               Error);
}

TEST_F(MobileCommonTest, AlignmentPaddingDoesNotHideMissingExtraMetadata) {
  unsigned index = 0;
  for (const auto &tail :
       {std::string("\1", 1), std::string("\0\1", 2), std::string("\0\0\1", 3),
        std::string("\x34\x12\x10\0", 4)}) {
    ZipItem item{"classes.dex", "bytecode"};
    item.local_extra = tail;
    auto archive = root / ("bad-extra-" + std::to_string(index++) + ".zip");
    writeFile(archive, zip({item}));
    EXPECT_THROW(extractZip(archive, root / archive.stem(), {}), Error);
  }
  ZipItem item{"classes.dex", "bytecode"};
  item.central_extra.assign(2, '\0');
  writeFile(root / "central-padding.zip", zip({item}));
  EXPECT_THROW(extractZip(root / "central-padding.zip", root / "central", {}),
               Error);
  item.central_extra.clear();
  item.local_extra.assign(2, '\0');
  auto missing = zip({item});
  // A ZIP64 sentinel still requires an actual ZIP64 extra-field value.
  for (unsigned i = 18; i < 22; ++i)
    missing[i] = char(0xff);
  writeFile(root / "missing-zip64.zip", missing);
  EXPECT_THROW(extractZip(root / "missing-zip64.zip", root / "missing", {}),
               Error);
  item.local_extra.clear();
  put(item.local_extra, 1, 2);
  put(item.local_extra, 16, 2);
  put(item.local_extra, item.bytes.size(), 8);
  put(item.local_extra, item.bytes.size(), 8);
  item.local_extra.append(2, '\0');
  auto resolved = zip({item});
  resolved[4] = 45;
  auto central = resolved.find(std::string("PK\1\2", 4));
  ASSERT_NE(central, std::string::npos);
  resolved[central + 6] = 45;
  for (unsigned i = 18; i < 26; ++i)
    resolved[i] = char(0xff);
  writeFile(root / "padded-zip64.zip", resolved);
  auto files = extractZip(root / "padded-zip64.zip", root / "padded-zip64", {});
  ASSERT_EQ(files.size(), 1u);
  EXPECT_EQ(readFile(files[0], 1024), "bytecode");
}

TEST_F(MobileCommonTest, SelectedCodeKeepsCaseDistinctResourcesInTheArchive) {
  auto archive = root / "release.apk", output = root / "code";
  writeFile(archive, zip({{"classes.dex", "bytecode"},
                          {"res/-A.xml", "first resource", true},
                          {"res/-a.xml", "second resource", true},
                          {"res/2F.xml", "third resource"},
                          {"res/2f.xml", "fourth resource"}}));
  auto files = extractZip(archive, output, {}, [](const fs::path &Path) {
    return Path == "classes.dex";
  });
  ASSERT_EQ(files.size(), 1u);
  EXPECT_EQ(files.front(), output / "classes.dex");
  EXPECT_EQ(readFile(files.front(), 100), "bytecode");
  EXPECT_FALSE(fs::exists(output / "res"));
  // Extracting the whole package still needs portable destination identities.
  EXPECT_THROW(extractZip(archive, root / "whole-archive", {}), Error);
  EXPECT_FALSE(fs::exists(root / "whole-archive"));
}
TEST_F(MobileCommonTest,
       SelectionStillValidatesUnwrittenPayloadsAndNamespaces) {
  auto SelectCode = [](const fs::path &Path) { return Path == "classes.dex"; };
  auto bytes = zip({{"resource", "payload"}, {"classes.dex", "bytecode"}});
  bytes[30 + std::string("resource").size()] ^= 1;
  writeFile(root / "bad-resource.apk", bytes);
  EXPECT_THROW(
      extractZip(root / "bad-resource.apk", root / "bad-crc", {}, SelectCode),
      Error);
  EXPECT_FALSE(fs::exists(root / "bad-crc/resource"));
  EXPECT_FALSE(fs::exists(root / "bad-crc/classes.dex"));
  unsigned Index = 0;
  for (const auto &Items : std::vector<std::vector<ZipItem>>{
           {{"res/item", "one"}, {"res/item", "two"}},
           {{"res/item", "file"}, {"res/item/child", "child"}}}) {
    auto ArchivePath = root / ("duplicate-" + std::to_string(Index++) + ".apk");
    writeFile(ArchivePath, zip(Items));
    EXPECT_THROW(
        extractZip(ArchivePath, root / "bad-namespace", {}, SelectCode), Error);
    EXPECT_FALSE(fs::exists(root / "bad-namespace"));
  }
  writeFile(root / "selected-case.zip",
            zip({{"Code.dex", "one"}, {"code.dex", "two"}}));
  EXPECT_THROW(extractZip(root / "selected-case.zip", root / "case-conflict",
                          {}, [](const fs::path &) { return true; }),
               Error);
  EXPECT_FALSE(fs::exists(root / "case-conflict"));
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
