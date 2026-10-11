//===- Record.cpp - Independent ZIP fixture recording ---------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Development-only C++ recorder using libarchive, never a product dependency.
///
//===----------------------------------------------------------------------===//

#include <archive.h>
#include <archive_entry.h>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace {
void check(int Status) {
  if (Status != ARCHIVE_OK)
    throw std::runtime_error("Fixture writer refused configuration or record");
}
void record(const std::filesystem::path &Path, bool Deflate) {
  if (std::filesystem::exists(Path))
    throw std::runtime_error("Refusing to replace an existing fixture");
  struct Writer {
    archive *Value = archive_write_new();
    ~Writer() { archive_write_free(Value); }
  } A;
  if (!A.Value)
    throw std::runtime_error("Cannot allocate fixture writer");
  check(archive_write_set_format_zip(A.Value));
  check(archive_write_set_format_option(A.Value, "zip", "zip64", nullptr));
  check(Deflate ? archive_write_zip_set_compression_deflate(A.Value)
                : archive_write_zip_set_compression_store(A.Value));
  check(archive_write_open_filename(A.Value, Path.c_str()));
  for (const auto &[Name, Text] :
       {std::pair<std::string_view, std::string_view>{
            "package.json",
            R"({"name":"neverd-zip-fixture","main":"main.js"})"},
        {"main.js", "export const answer = 42;\n"},
        {"web/", ""},
        {"web/index.html", "<script src='../main.js'></script>\n"},
        {"empty", ""}}) {
    struct Entry {
      archive_entry *Value = archive_entry_new();
      ~Entry() { archive_entry_free(Value); }
    } E;
    if (!E.Value)
      throw std::runtime_error("Cannot allocate fixture entry");
    archive_entry_set_pathname(E.Value, Name.data());
    archive_entry_set_filetype(E.Value,
                               Name.ends_with('/') ? AE_IFDIR : AE_IFREG);
    archive_entry_set_perm(E.Value, Name.ends_with('/') ? 0755 : 0644);
    archive_entry_set_size(E.Value, Text.size());
    archive_entry_set_mtime(E.Value, 0, 0);
    check(archive_write_header(A.Value, E.Value));
    if (archive_write_data(A.Value, Text.data(), Text.size()) !=
        static_cast<la_ssize_t>(Text.size()))
      throw std::runtime_error("Fixture writer did not consume exact bytes");
    check(archive_write_finish_entry(A.Value));
  }
  check(archive_write_close(A.Value));
}
} // namespace

int main(int Count, char **Arguments) {
  try {
    if (Count != 2)
      throw std::runtime_error("Pass an existing output directory");
    const std::filesystem::path Root(Arguments[1]);
    record(Root / "libarchive-stored.zip", false);
    record(Root / "libarchive-deflate.zip", true);
    std::cout << archive_version_string() << '\n';
    return 0;
  } catch (const std::exception &E) {
    std::cerr << E.what() << '\n';
    return 1;
  }
}
