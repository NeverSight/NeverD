//===- DyldCacheCAPITests.cpp - Selected cache session contracts ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Independent serialized images check catalogue selection, exact input
/// identity and separation of persistent edits through the public C API.
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/sdk/NeverDCAPI.h"
#include "neverd/support/BinaryEncoding.h"
#include "neverd/support/ProjectWriteLock.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/SHA256.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

std::string take(const char *Text) {
  std::string Result = Text ? Text : "";
  neverd_free_string(Text);
  return Result;
}

class DyldCacheCAPI : public testing::Test {
protected:
  static constexpr uint64_t Base = 0x180000000;
  llvm::SmallString<128> Directory;
  std::filesystem::path Path;
  std::vector<uint8_t> Bytes = std::vector<uint8_t>(0x2000);
  neverd_session_t Session = nullptr;

  template <typename T> void put(size_t Offset, T Value) {
    neverd::writeLE<T>(Bytes.data() + Offset, Value);
  }

  void SetUp() override {
    ASSERT_FALSE(
        llvm::sys::fs::createUniqueDirectory("neverd-cache-api", Directory));
    Path = std::filesystem::path(Directory.str().str()) / "cache";
    std::memcpy(Bytes.data(), "dyld_v1  arm64e", 16);
    put<uint32_t>(16, 0x200);
    put<uint32_t>(20, 1);
    put<uint64_t>(224, Base);
    put<uint64_t>(232, 0x2000);
    put<uint32_t>(448, 0x300);
    put<uint32_t>(452, 2);
    put<uint64_t>(0x200, Base);
    put<uint64_t>(0x208, 0x2000);
    put<uint32_t>(0x218, 5);
    put<uint32_t>(0x21c, 5);
    for (size_t I = 0; I < 2; ++I) {
      const size_t Header = I ? 0x1000 : 0x600;
      const char *Name = I ? "/usr/lib/B.dylib" : "/usr/lib/A.dylib";
      put<uint64_t>(0x300 + I * 32, Base + Header);
      put<uint32_t>(0x318 + I * 32, 0x350 + I * 32);
      std::memcpy(Bytes.data() + 0x350 + I * 32, Name, std::strlen(Name) + 1);
      put<uint32_t>(Header, 0xfeedfacf);
      put<uint32_t>(Header + 4, 0x100000c);
      put<uint32_t>(Header + 8, 2);
      put<uint32_t>(Header + 12, 6);
      put<uint32_t>(Header + 16, 2);
      put<uint32_t>(Header + 20, 200);
      put<uint32_t>(Header + 32, 0x19);
      put<uint32_t>(Header + 36, 152);
      std::memcpy(Bytes.data() + Header + 40, "__TEXT", 7);
      put<uint64_t>(Header + 56, Base + Header);
      put<uint64_t>(Header + 64, 0x400);
      put<uint64_t>(Header + 72, Header);
      put<uint64_t>(Header + 80, 0x400);
      put<uint32_t>(Header + 88, 5);
      put<uint32_t>(Header + 92, 5);
      put<uint32_t>(Header + 96, 1);
      std::memcpy(Bytes.data() + Header + 104, "__text", 7);
      std::memcpy(Bytes.data() + Header + 120, "__TEXT", 7);
      put<uint64_t>(Header + 136, Base + Header + 0x300);
      put<uint64_t>(Header + 144, 4);
      put<uint32_t>(Header + 152, Header + 0x300);
      put<uint32_t>(Header + 156, 2);
      put<uint32_t>(Header + 168, 0x80000400);
      put<uint32_t>(Header + 184, 0xd);
      put<uint32_t>(Header + 188, 48);
      put<uint32_t>(Header + 192, 24);
      std::memcpy(Bytes.data() + Header + 208, Name, std::strlen(Name) + 1);
      put<uint32_t>(Header + 0x300, 0xd65f03c0);
    }
    std::ofstream Out(Path, std::ios::binary);
    Out.write(reinterpret_cast<const char *>(Bytes.data()), Bytes.size());
    Out.close();
    ASSERT_TRUE(Out.good());
    Session = neverd_session_create();
    ASSERT_TRUE(Session);
    neverd_session_set_debug_info_enabled(Session, 0);
  }

  void TearDown() override {
    if (Session)
      neverd_session_destroy(Session);
    if (!Directory.empty())
      (void)llvm::sys::fs::remove_directories(Directory);
  }

  void load(const char *Name) {
    const std::string Options =
        "{\"loader\":\"dyld-cache\",\"cache_image\":\"" + std::string(Name) +
        "\"}";
    ASSERT_EQ(neverd_session_set_load_options(Session, Options.c_str()), 0);
    ASSERT_EQ(neverd_session_load(Session, Path.string().c_str()), 1)
        << take(neverd_last_error(Session));
  }
};

TEST_F(DyldCacheCAPI, CatalogueAndSelectionPreserveInputIdentity) {
  neverd_session_set_debug_info_enabled(Session, 1);
  auto Inventory = llvm::json::parse(
      take(neverd_dyld_cache_images_json(Path.string().c_str())));
  ASSERT_TRUE(bool(Inventory));
  const auto *Images = Inventory->getAsObject()->getArray("images");
  ASSERT_TRUE(Images);
  ASSERT_EQ(Images->size(), 2u);
  EXPECT_EQ((*Images)[1].getAsObject()->getString("path"), "/usr/lib/B.dylib");
  auto Identified =
      llvm::json::parse(take(neverd_identify_json(Path.string().c_str())));
  ASSERT_TRUE(bool(Identified));
  const auto *Rows = Identified->getAsObject()->getArray("rows");
  ASSERT_TRUE(Rows && !Rows->empty());
  EXPECT_EQ((*Rows)[0].getAsObject()->getString("loader"), "dyld-cache");
  EXPECT_EQ(neverd_session_load(Session, Path.string().c_str()), 0);
  EXPECT_NE(take(neverd_last_error(Session)).find("--cache-image"),
            std::string::npos);
  load("/usr/lib/A.dylib");
  ASSERT_FALSE(HasFatalFailure());
  EXPECT_EQ(take(neverd_session_file_path(Session)), Path.string());
  EXPECT_EQ(neverd_session_file_size(Session), Bytes.size());
  EXPECT_EQ(take(neverd_session_input_sha256(Session)),
            llvm::toHex(llvm::SHA256::hash(Bytes), true));
  auto Project = llvm::json::parse(take(neverd_project_json(Session)));
  ASSERT_TRUE(bool(Project));
  const auto *Cache =
      Project->getAsObject()->getObject("input")->getObject("dyld_cache");
  ASSERT_TRUE(Cache);
  EXPECT_EQ(Cache->getString("cache_image"), "/usr/lib/A.dylib");
  ASSERT_TRUE(Cache->getArray("file_sha256"));
  EXPECT_EQ(Cache->getArray("file_sha256")->size(), 1u);
  EXPECT_EQ(Cache->getBoolean("analysis_projection"), true);
  for (const char *Options :
       {R"({"loader":"dyld-cache"})", R"({"cache_image":""})",
        R"({"cache_image":1})",
        R"({"loader":"binary","cache_image":"/usr/lib/A.dylib"})",
        R"({"cache_image":"/usr/lib/\u0000A.dylib"})"})
    EXPECT_EQ(neverd_session_set_load_options(Session, Options), -1);
}

TEST_F(DyldCacheCAPI, EditsAreIsolatedAndRememberedSelectionReopens) {
  std::string Folders[2];
  for (unsigned I = 0; I < 2; ++I) {
    load(I ? "/usr/lib/B.dylib" : "/usr/lib/A.dylib");
    ASSERT_FALSE(HasFatalFailure());
    ASSERT_EQ(neverd_annotations_load(Session), 0);
    EXPECT_TRUE(take(neverd_annotation_get(Session, Base + 0x900)).empty());
    neverd_annotation_set(Session, Base + 0x900,
                          I ? "B annotation" : "A annotation");
    ASSERT_EQ(neverd_annotations_save(Session), 0)
        << take(neverd_last_error(Session));
    auto Project = llvm::json::parse(take(neverd_project_json(Session)));
    ASSERT_TRUE(bool(Project));
    EXPECT_EQ(Project->getAsObject()->getBoolean("matches"), true);
    Folders[I] = Project->getAsObject()->getString("folder")->str();
  }
  EXPECT_NE(Folders[0], Folders[1]);
  load("/usr/lib/A.dylib");
  ASSERT_FALSE(HasFatalFailure());
  ASSERT_EQ(neverd_annotations_load(Session), 0);
  EXPECT_EQ(take(neverd_annotation_get(Session, Base + 0x900)), "A annotation");
  ASSERT_EQ(neverd_load_options_save(Session), 0)
      << take(neverd_last_error(Session));
  neverd_session_destroy(Session);
  Session = neverd_session_create();
  ASSERT_TRUE(Session);
  neverd_session_set_debug_info_enabled(Session, 0);
  ASSERT_EQ(neverd_session_load(Session, Path.string().c_str()), 1)
      << take(neverd_last_error(Session));
  auto Options =
      llvm::json::parse(take(neverd_session_load_options_json(Session)));
  ASSERT_TRUE(bool(Options));
  EXPECT_EQ(Options->getAsObject()->getString("cache_image"),
            "/usr/lib/A.dylib");
  ASSERT_EQ(neverd_annotations_load(Session), 0);
  EXPECT_EQ(take(neverd_annotation_get(Session, Base + 0x900)), "A annotation");
}

TEST_F(DyldCacheCAPI, NamespaceLocksUseTheRealCanonicalInput) {
  const auto Alias = Path.parent_path() / "cache-alias";
  std::error_code Error;
  std::filesystem::create_symlink(Path, Alias, Error);
  if (Error)
    GTEST_SKIP() << Error.message();
  neverd::ProjectWriteLock First(Path, ".selection-A");
  ASSERT_TRUE(First) << First.error();
  neverd::ProjectWriteLock Same(Alias, ".selection-A");
  EXPECT_FALSE(Same);
  neverd::ProjectWriteLock Other(Path, ".selection-B");
  EXPECT_TRUE(Other) << Other.error();
  neverd::ProjectWriteLock Missing(Path.parent_path() / "missing",
                                   ".selection-A");
  EXPECT_FALSE(Missing);
}

TEST_F(DyldCacheCAPI, SavingSelectionAlsoOwnsItsImageManifest) {
  load("/usr/lib/A.dylib");
  ASSERT_FALSE(HasFatalFailure());
  auto Project = llvm::json::parse(take(neverd_project_json(Session)));
  ASSERT_TRUE(bool(Project));
  auto Folder = Project->getAsObject()->getString("folder");
  ASSERT_TRUE(Folder);
  auto Input = Folder->drop_back(std::strlen(".neverd"));
  const std::string Canonical = std::filesystem::canonical(Path).string();
  ASSERT_TRUE(Input.starts_with(Canonical));
  const auto Suffix = Input.drop_front(Canonical.size());
  neverd::ProjectWriteLock Locked(Path, Suffix.str());
  ASSERT_TRUE(Locked) << Locked.error();
  EXPECT_EQ(neverd_load_options_save(Session), -1);
  EXPECT_NE(take(neverd_last_error(Session)).find("project writer unavailable"),
            std::string::npos);
}

TEST_F(DyldCacheCAPI, AnotherSessionCannotChangeTheLoadedImageOnReload) {
  load("/usr/lib/A.dylib");
  ASSERT_FALSE(HasFatalFailure());
  auto Other = neverd_session_create();
  ASSERT_TRUE(Other);
  neverd_session_set_debug_info_enabled(Other, 0);
  ASSERT_EQ(
      neverd_session_set_load_options(
          Other, R"({"loader":"dyld-cache","cache_image":"/usr/lib/B.dylib"})"),
      0);
  ASSERT_EQ(neverd_session_load(Other, Path.string().c_str()), 1);
  ASSERT_EQ(neverd_load_options_save(Other), 0);
  neverd_session_destroy(Other);
  ASSERT_EQ(neverd_session_set_load_options(Session, nullptr), 0);
  ASSERT_EQ(neverd_session_load(Session, Path.string().c_str()), 1);
  auto Options =
      llvm::json::parse(take(neverd_session_load_options_json(Session)));
  ASSERT_TRUE(bool(Options));
  EXPECT_EQ(Options->getAsObject()->getString("cache_image"),
            "/usr/lib/A.dylib");
}

} // namespace
